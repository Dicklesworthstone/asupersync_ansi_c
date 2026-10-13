/*
 * asx/runtime/waker.h — wake registration for task readiness signaling
 *
 * A waker is a token that, when signaled, marks an associated task
 * as ready to be polled. Tasks register wakers with the IO driver
 * or timer system when they need to be woken on external events.
 *
 * Wakers signal readiness in a fixed-size arena, one per runtime: a waker
 * names the runtime of its task by the tag in its slot, so a wake from a
 * worker thread reaches that runtime's arena and no other. Signals carry
 * a deterministic sequence number so cross-boundary wake drains can replay
 * a stable ready order even when worker/reactor adapters are live. A wake
 * from another thread must not race with the shutdown of the runtime that
 * owns the waker (its arena is the runtime's storage block).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_WAKER_H
#define ASX_RUNTIME_WAKER_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (ASX_MAX_WAKERS) < 1
#error "ASX_MAX_WAKERS must be at least 1"
#endif

/* -------------------------------------------------------------------
 * Waker handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot; /* arena index (low 16 bits), runtime tag (high 16 bits) */
    uint16_t generation;
} asx_waker;

/* -------------------------------------------------------------------
 * API: Registration
 * ------------------------------------------------------------------- */

/* Register a waker for a task, in the arena of the runtime that issued the
 * task. The waker can later be signaled to indicate the task should be
 * re-polled.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT if task is ASX_INVALID_ID or out_waker is NULL.
 * Returns ASX_E_NOT_FOUND if no runtime holds the tag the task carries.
 * Returns ASX_E_RESOURCE_EXHAUSTED if that runtime's waker arena is full. */
ASX_API ASX_MUST_USE asx_status asx_waker_register(asx_task_id task, asx_waker *out_waker);

/* Deregister a waker. Safe to call on already-deregistered wakers. */
ASX_API void asx_waker_deregister(asx_waker *waker);

/* -------------------------------------------------------------------
 * API: Signaling
 * ------------------------------------------------------------------- */

/* Signal a waker, marking its associated task as ready to poll.
 * Returns ASX_OK on success.
 * Returns ASX_E_NOT_FOUND if waker is stale/deregistered. */
ASX_API ASX_MUST_USE asx_status asx_waker_wake(const asx_waker *waker);

/* Clone a waker (both refer to the same task).
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_waker_clone(const asx_waker *src, asx_waker *out_clone);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Check if a waker has been signaled since last clear.
 * Returns 1 if signaled, 0 otherwise. */
ASX_API int asx_waker_is_signaled(const asx_waker *waker);

/* Clear the signaled state of a waker. */
ASX_API void asx_waker_clear(asx_waker *waker);

/* Get the task ID associated with a waker.
 * Returns ASX_INVALID_ID if waker is NULL or stale. */
ASX_API asx_task_id asx_waker_task(const asx_waker *waker);

/* Get the count of active wakers of a runtime (0 for NULL or a tag no
 * runtime holds). */
ASX_API uint32_t asx_waker_active_count(const asx_runtime *rt);

/* -------------------------------------------------------------------
 * API: Drain signaled wakers
 * ------------------------------------------------------------------- */

/* Collect all signaled task IDs of a runtime into the output buffer.
 * Clears the signaled state for collected wakers.
 * Returns the number of tasks collected (0 for NULL or a tag no runtime
 * holds). */
ASX_API uint32_t asx_waker_drain_signaled(const asx_runtime *rt, asx_task_id *out_tasks,
                                          uint32_t max_tasks);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset one runtime's waker arena state (test support). */
ASX_API void asx_waker_reset(const asx_runtime *rt);

#ifdef __cplusplus
}
#endif

/* -------------------------------------------------------------------
 * Runtime-internal layout (not API)
 *
 * The runtime kernel embeds the arena, one per runtime. Guarded apart from
 * the public part, so a unit that included the header without the access
 * macro and then asks for it (runtime_internal.h) gets the layout too.
 * ------------------------------------------------------------------- */

#if defined(ASX_INTERNAL_WAKER_ACCESS) && !defined(ASX_RUNTIME_WAKER_INTERNAL_H)
#define ASX_RUNTIME_WAKER_INTERNAL_H

#include <asx/platform/atomics.h>

typedef struct {
    asx_task_id task;
    uint16_t generation;
    int alive;
    int signaled;
    uint64_t signal_sequence;
} asx_waker_slot;

/* A zeroed state is an empty arena, unlocked, no scheduler blocking. */
typedef struct {
    asx_waker_slot slots[ASX_MAX_WAKERS];
    uint32_t slot_count;
    uint32_t active_count;
    uint64_t signal_sequence;
    int sched_blocking; /* the runtime's scheduler is (about to be) blocked */
    asx_atomic_u32 lock;
} asx_waker_state;

#endif /* ASX_INTERNAL_WAKER_ACCESS */

#endif /* ASX_RUNTIME_WAKER_H */
