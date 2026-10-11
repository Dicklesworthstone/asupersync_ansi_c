/*
 * asx/sync/semaphore.h — counting semaphore with permit obligations
 *
 * Async-friendly counting semaphore. Permits are tracked obligations
 * that must be released. Cancel-safe: cancellation during acquire
 * does not consume a permit.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_SEMAPHORE_H
#define ASX_SYNC_SEMAPHORE_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_SEMAPHORE_MAX
#define ASX_SEMAPHORE_MAX 16u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_semaphore_handle;

/* `obligation` is the permit's SemaphorePermit runtime obligation, reserved
 * when an acquire polled with a task Cx is granted and committed by
 * release (Rust sync/semaphore.rs:119, :1274). ASX_INVALID_ID when the
 * permit is untracked: try_acquire (no Cx), an acquire without a task Cx,
 * a mutex guard (Rust's Mutex has no obligation), or a refused
 * reservation. A tracked permit still held when its task completes is
 * dropped with it, as Rust drops the task body's locals: its permits go
 * back to the pool, the obligation commits (SemaphorePermit's drop,
 * sync/semaphore.rs:1081), and the permit value is spent. Nothing leaks.
 * `count` is the number of permits it holds (an
 * acquire of n takes n at once); release returns them all. `serial`
 * names a mutex guard's hold of the lock (0 for a semaphore's permit):
 * only the current holder's guard unlocks. */
typedef struct {
    uint32_t sem_slot;
    uint16_t generation;
    uint32_t count;
    asx_obligation_id obligation;
    uint32_t serial;
} asx_semaphore_permit;

/* A waiter names a record in the runtime's shared wait-node pool, so a
 * semaphore has no waiter limit of its own (Rust's waiter queue is
 * unbounded); *_begin reports ASX_E_RESOURCE_EXHAUSTED only when the whole
 * pool (ASX_WAIT_NODE_CAPACITY, four nodes per task slot by default) is
 * in use. */
typedef struct {
    uint32_t sem_slot;
    uint32_t waiter_slot;       /* wait-node index */
    uint16_t generation;        /* the semaphore's */
    uint16_t waiter_generation; /* the wait node's */
} asx_semaphore_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a counting semaphore with initial permits. */
ASX_API ASX_MUST_USE asx_status asx_semaphore_create(uint32_t initial_permits,
                                                     asx_semaphore_handle *out);

/* Close a semaphore, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_semaphore_close(asx_semaphore_handle handle);

/* -------------------------------------------------------------------
 * Acquire (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to acquire a permit immediately. Returns ASX_OK + permit if
 * available, ASX_E_WOULD_BLOCK if none available. */
ASX_API asx_status asx_semaphore_try_acquire(asx_semaphore_handle handle,
                                             asx_semaphore_permit *out);

/* Try to acquire `count` permits at once, all or nothing (Rust
 * Semaphore::try_acquire(count)). ASX_E_WOULD_BLOCK while fewer than
 * `count` are free or anyone is queued; a count of 0 succeeds at once with
 * a permit holding none. ASX_E_INVALID_ARGUMENT for a mutex's semaphore
 * and a count other than 1. */
ASX_API asx_status asx_semaphore_try_acquire_many(asx_semaphore_handle handle, uint32_t count,
                                                  asx_semaphore_permit *out);

/* -------------------------------------------------------------------
 * Acquire (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async acquire of one permit. Must be followed by poll_acquire
 * calls. */
ASX_API ASX_MUST_USE asx_status asx_semaphore_acquire_begin(asx_semaphore_handle handle,
                                                            asx_semaphore_waiter *out);

/* Begin async acquire of `count` permits as one all-or-nothing operation
 * (Rust Semaphore::acquire(cx, count)): the waiter takes them only when
 * `count` are free and it is at the front of the line; a count of 0
 * succeeds at its first poll with a permit holding none and no
 * obligation. ASX_E_INVALID_ARGUMENT for a mutex's semaphore and a count
 * other than 1. */
ASX_API ASX_MUST_USE asx_status asx_semaphore_acquire_many_begin(asx_semaphore_handle handle,
                                                                 uint32_t count,
                                                                 asx_semaphore_waiter *out);

/* Poll for the permits. Returns ASX_OK + permit when acquired,
 * ASX_E_PENDING when waiting. Permits go to waiters in arrival (FIFO)
 * order: a waiter takes free permits only when no earlier waiter is still
 * queued. Inside a scheduler poll an ASX_E_PENDING result parks the
 * calling task until a release lets it take them (or close). */
ASX_API asx_status asx_semaphore_poll_acquire(asx_semaphore_waiter *waiter,
                                              asx_semaphore_permit *out, asx_cx *cx);

/* Cancel an async acquire (safe even if already acquired). A permit that
 * was already granted to the waiter passes on to the next waiter in line.
 * Waiters whose parked task completed without cancelling are reclaimed
 * lazily. A cancel-pending waiter keeps its place until its own poll or
 * cancel gives it up, as in Rust. */
ASX_API asx_status asx_semaphore_acquire_cancel(asx_semaphore_waiter *waiter);

/* -------------------------------------------------------------------
 * Release
 * ------------------------------------------------------------------- */

/* Release a permit's `count` permits back to the semaphore. A semaphore
 * pools them and wakes the front waiter if it can now take what it asked
 * for; a mutex hands the lock to the oldest waiter (waking its task if
 * parked), else returns it to the pool. The permit's obligation, if any,
 * is committed. A permit value is released once (Rust's permit is
 * consumed by its drop); a second release is refused with
 * ASX_E_INVALID_STATE and nothing changes. The runtime knows a permit
 * was released when it is a mutex guard (its hold of the lock ended), a
 * tracked permit whose obligation is resolved (its release or its drop
 * with the holder task committed it, forget aborted it), or one whose
 * count exceeds what the semaphore's handed-out permits still hold, so
 * releases never return more permits than were handed out.
 * An untracked permit's copy released while other permits are out is
 * beyond that check: release each permit value once. */
ASX_API asx_status asx_semaphore_release(asx_semaphore_permit permit);

/* Forget a permit (Rust SemaphorePermit::forget): its permits are not
 * returned to the pool, and its obligation, if any, is aborted with
 * ASX_OBLIGATION_ABORT_EXPLICIT. ASX_E_INVALID_STATE for a permit already
 * released or forgotten (as for release), ASX_E_INVALID_ARGUMENT for a
 * mutex's semaphore, ASX_E_STALE_HANDLE for a closed semaphore (the
 * obligation is still aborted). */
ASX_API asx_status asx_semaphore_forget(asx_semaphore_permit permit);

/* Add `count` permits to the pool, saturating (Rust
 * Semaphore::add_permits): the front waiter is woken if it can now take
 * what it asked for, and nobody else. A count of 0 does nothing.
 * ASX_E_STALE_HANDLE for a closed semaphore, ASX_E_INVALID_ARGUMENT for a
 * mutex's semaphore. */
ASX_API asx_status asx_semaphore_add_permits(asx_semaphore_handle handle, uint32_t count);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of available permits. */
ASX_API uint32_t asx_semaphore_available(asx_semaphore_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all semaphore arena state (test support). */
ASX_API void asx_semaphore_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_SEMAPHORE_H */
