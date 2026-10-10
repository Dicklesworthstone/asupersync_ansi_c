/*
 * asx/sync/rwlock.h — async read-write lock with guard obligations
 *
 * Allows multiple concurrent readers or a single exclusive writer, with
 * Rust's bounded writer-preference fairness (sync/rwlock.rs): while a
 * writer waits, new reads queue; a release serves the oldest queued
 * writer unless readers queued before it, who go first; after 16
 * consecutive writer hand-offs while readers were queued, the oldest
 * queued reader gets a turn. Cancel-safe: cancellation during acquisition
 * does not acquire the lock.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_RWLOCK_H
#define ASX_SYNC_RWLOCK_H

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

#ifndef ASX_RWLOCK_MAX
#define ASX_RWLOCK_MAX 16u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_rwlock_handle;

typedef struct {
    uint32_t rw_slot;
    uint16_t generation;
} asx_rwlock_read_guard;

typedef struct {
    uint32_t rw_slot;
    uint16_t generation;
} asx_rwlock_write_guard;

/* A waiter names a record in the runtime's shared wait-node pool, so an
 * rwlock has no waiter limit of its own; *_begin reports
 * ASX_E_RESOURCE_EXHAUSTED only when the whole pool
 * (ASX_WAIT_NODE_CAPACITY) is in use. */
typedef struct {
    uint32_t rw_slot;
    uint32_t waiter_slot;       /* wait-node index */
    uint16_t generation;        /* the rwlock's */
    uint16_t waiter_generation; /* the wait node's */
    int is_write;               /* 1 = write waiter, 0 = read waiter */
} asx_rwlock_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a read-write lock (initially unlocked). */
ASX_API ASX_MUST_USE asx_status asx_rwlock_create(asx_rwlock_handle *out);

/* Close a rwlock, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_rwlock_close(asx_rwlock_handle handle);

/* -------------------------------------------------------------------
 * Read lock (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to acquire a read lock immediately. Returns ASX_OK + guard if
 * no writer holds or is waiting, ASX_E_WOULD_BLOCK otherwise. */
ASX_API asx_status asx_rwlock_try_read(asx_rwlock_handle handle, asx_rwlock_read_guard *out);

/* -------------------------------------------------------------------
 * Write lock (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to acquire a write lock immediately. Returns ASX_OK + guard if
 * nobody holds the lock and no writer is waiting, ASX_E_WOULD_BLOCK
 * otherwise. */
ASX_API asx_status asx_rwlock_try_write(asx_rwlock_handle handle, asx_rwlock_write_guard *out);

/* -------------------------------------------------------------------
 * Read lock (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async read-lock. Must be followed by poll_read calls. The waiter
 * joins the line at its first poll that has to wait, not here. */
ASX_API ASX_MUST_USE asx_status asx_rwlock_read_begin(asx_rwlock_handle handle,
                                                      asx_rwlock_waiter *out);

/* Poll for read lock (a non-NULL cx is checkpointed first; giving up
 * returns its status). The first poll takes the lock if no writer holds
 * or waits for it; otherwise the waiter joins the line, and later polls
 * return ASX_OK only once an unlock has granted it the lock. Returns
 * ASX_E_PENDING while waiting; inside a scheduler poll the calling task is
 * then parked until a grant (or close). */
ASX_API asx_status asx_rwlock_poll_read(asx_rwlock_waiter *waiter, asx_rwlock_read_guard *out,
                                        asx_cx *cx);

/* -------------------------------------------------------------------
 * Write lock (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async write-lock. Must be followed by poll_write calls. The
 * waiter joins the line at its first poll that has to wait, not here. */
ASX_API ASX_MUST_USE asx_status asx_rwlock_write_begin(asx_rwlock_handle handle,
                                                       asx_rwlock_waiter *out);

/* Poll for write lock (a non-NULL cx is checkpointed first). The first
 * poll takes the lock if nobody holds it and no writer is queued;
 * otherwise the waiter joins the line, counts as a waiting writer, and
 * later polls return ASX_OK only once an unlock has granted it the lock.
 * Writers are served in arrival (FIFO) order. Returns ASX_E_PENDING while
 * waiting; inside a scheduler poll the calling task is then parked until a
 * grant (or close). Polling a waiter of the other kind returns
 * ASX_E_INVALID_STATE. */
ASX_API asx_status asx_rwlock_poll_write(asx_rwlock_waiter *waiter, asx_rwlock_write_guard *out,
                                         asx_cx *cx);

/* -------------------------------------------------------------------
 * Cancel async acquisition
 * ------------------------------------------------------------------- */

/* Cancel an async read or write acquisition (safe even if acquired). A
 * lock already granted to the waiter is released as an unlock would;
 * a queued writer that was the last waiting writer, with no writer
 * holding the lock, admits every queued reader. */
ASX_API asx_status asx_rwlock_waiter_cancel(asx_rwlock_waiter *waiter);

/* -------------------------------------------------------------------
 * Unlock
 * ------------------------------------------------------------------- */

/* Release a read lock. May wake a waiting writer. */
ASX_API asx_status asx_rwlock_read_unlock(asx_rwlock_read_guard guard);

/* Release a write lock. May wake waiting readers or a writer. */
ASX_API asx_status asx_rwlock_write_unlock(asx_rwlock_write_guard guard);

/* Release a write lock as Rust's RwLockWriteGuard does when dropped while
 * its task panics (rwlock.rs:1083-1090): the lock is poisoned, then
 * released, waking every queued waiter. From then on try_read, try_write
 * and every poll after its Cx checkpoint fail with ASX_E_INVALID_STATE
 * (Rust RwLockError::Poisoned, vocabulary §5). Read guards never poison. */
ASX_API asx_status asx_rwlock_write_unlock_poisoned(asx_rwlock_write_guard guard);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of active readers (0 if write-locked). */
ASX_API uint32_t asx_rwlock_reader_count(asx_rwlock_handle handle);

/* Check if the rwlock is currently write-locked. */
ASX_API int asx_rwlock_is_write_locked(asx_rwlock_handle handle);

/* 1 if the rwlock is poisoned (asx_rwlock_write_unlock_poisoned). */
ASX_API int asx_rwlock_is_poisoned(asx_rwlock_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all rwlock arena state. For tests only. */
ASX_API void asx_rwlock_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_RWLOCK_H */
