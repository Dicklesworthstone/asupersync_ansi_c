/*
 * asx/runtime/waker.h — wake registration for task readiness signaling
 *
 * A waker is a token that, when signaled, marks an associated task
 * as ready to be polled. Tasks register wakers with the IO driver
 * or timer system when they need to be woken on external events.
 *
 * Walking skeleton: single-threaded, no atomics. Wakers signal
 * readiness by setting a flag in a fixed-size wake arena.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_WAKER_H
#define ASX_RUNTIME_WAKER_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_WAKERS 64u

/* -------------------------------------------------------------------
 * Waker handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_waker;

/* -------------------------------------------------------------------
 * API: Registration
 * ------------------------------------------------------------------- */

/* Register a waker for a task. The waker can later be signaled
 * to indicate the task should be re-polled.
 * Returns ASX_OK on success.
 * Returns ASX_E_RESOURCE_EXHAUSTED if waker arena is full. */
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

/* Get the count of active wakers. */
ASX_API uint32_t asx_waker_active_count(void);

/* -------------------------------------------------------------------
 * API: Drain signaled wakers
 * ------------------------------------------------------------------- */

/* Collect all signaled task IDs into the output buffer.
 * Clears the signaled state for collected wakers.
 * Returns the number of tasks collected. */
ASX_API uint32_t asx_waker_drain_signaled(asx_task_id *out_tasks, uint32_t max_tasks);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_waker_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_WAKER_H */
