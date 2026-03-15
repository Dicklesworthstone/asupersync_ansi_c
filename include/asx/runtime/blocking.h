/*
 * asx/runtime/blocking.h — blocking pool for offloading blocking work
 *
 * Provides spawn_blocking semantics: submit a blocking function that
 * runs outside the async scheduler. A waker is signaled when the
 * blocking work completes.
 *
 * Walking skeleton: single-threaded, executes blocking work inline
 * (synchronously). The API surface is ready for future multi-threaded
 * dispatch via platform threading hooks.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_BLOCKING_H
#define ASX_RUNTIME_BLOCKING_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/waker.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_BLOCKING_TASKS 16u

/* -------------------------------------------------------------------
 * Blocking task function signature
 * ------------------------------------------------------------------- */

/* A blocking function receives user_data and returns a uint64_t result.
 * It may perform blocking operations (file IO, DNS lookup, etc). */
typedef uint64_t (*asx_blocking_fn)(void *user_data);

/* -------------------------------------------------------------------
 * Blocking task state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_BLOCKING_PENDING = 0,
    ASX_BLOCKING_RUNNING = 1,
    ASX_BLOCKING_COMPLETED = 2
} asx_blocking_state;

/* -------------------------------------------------------------------
 * Blocking task handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_blocking_handle;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Initialize the blocking pool.
 * Re-initialization resets all prior slot state and invalidates old handles. */
ASX_API ASX_MUST_USE asx_status asx_blocking_pool_init(void);

/* Shut down the blocking pool, completing all pending tasks. */
ASX_API void asx_blocking_pool_shutdown(void);

/* -------------------------------------------------------------------
 * API: Spawn blocking work
 * ------------------------------------------------------------------- */

/* Submit a blocking function for execution.
 * The waker (if non-NULL) is signaled when the work completes.
 * Walking skeleton: executes fn synchronously and signals waker
 * immediately.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_spawn_blocking(asx_blocking_fn fn, void *user_data,
                                                   const asx_waker *completion_waker,
                                                   asx_blocking_handle *out_handle);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the state of a blocking task. */
ASX_API asx_blocking_state asx_blocking_get_state(const asx_blocking_handle *handle);

/* Get the result of a completed blocking task.
 * Returns ASX_OK and fills *out_result on success.
 * Returns ASX_E_PENDING if task hasn't completed.
 * Returns ASX_E_INVALID_ARGUMENT if handle or out_result is NULL. */
ASX_API ASX_MUST_USE asx_status asx_blocking_get_result(const asx_blocking_handle *handle,
                                                        uint64_t *out_result);

/* Get the number of active (pending + running) blocking tasks. */
ASX_API uint32_t asx_blocking_active_count(void);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_blocking_pool_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_BLOCKING_H */
