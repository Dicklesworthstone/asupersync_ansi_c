/*
 * asx/sync/once.h — compute-once cell
 *
 * OnceCell ensures a value is computed exactly once. Subsequent
 * readers get the cached value. Cancel-safe: if the initializer
 * is cancelled, the cell remains unset and the next caller retries.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_ONCE_H
#define ASX_SYNC_ONCE_H

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

#ifndef ASX_ONCE_MAX
#define ASX_ONCE_MAX 16u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_once_handle;

/* Initializer callback: computes the value, stores it via out_value. */
typedef asx_status (*asx_once_init_fn)(void *user_data, uint64_t *out_value);

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create an uninitialized once cell. */
ASX_API ASX_MUST_USE asx_status asx_once_create(asx_once_handle *out);

/* Close a once cell. */
ASX_API asx_status asx_once_close(asx_once_handle handle);

/* -------------------------------------------------------------------
 * Access
 * ------------------------------------------------------------------- */

/* Get the value, initializing if needed. The init_fn is called at most
 * once. Returns ASX_OK + value. If init_fn fails, the cell remains
 * uninitialized and returns the error.
 *
 * Asynchronous initializers: if init_fn returns ASX_E_PENDING inside a
 * scheduler poll, the calling task becomes the cell's initializer (it calls
 * get_or_init again to continue). Meanwhile other tasks get ASX_E_PENDING
 * without running init_fn and are parked (FIFO) until the initializer
 * finishes: all are woken on success; on failure the oldest one is woken to
 * retry. An initializer whose task completes or is cancelled mid-init is
 * replaced by the next caller. Outside a scheduler poll an ASX_E_PENDING
 * init_fn leaves the cell uninitialized, as any failure does. */
ASX_API asx_status asx_once_get_or_init(asx_once_handle handle, asx_once_init_fn init_fn,
                                        void *user_data, uint64_t *out_value);

/* Withdraw `task` from the cell — the analog of dropping a Rust
 * get_or_init future. If `task` is the in-flight initializer, the
 * initialization is abandoned (the cell stays uninitialized) and the oldest
 * parked waiter is woken to retry; if it is a parked waiter it stops
 * waiting, passing on a retry turn it may have been given.
 * Returns ASX_OK (also when `task` is not involved), ASX_E_INVALID_ARGUMENT
 *   for an out-of-range handle, ASX_E_STALE_HANDLE for a closed cell.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API asx_status asx_once_wait_cancel(asx_once_handle handle, asx_task_id task);

/* Get the value if already initialized. Returns ASX_E_INVALID_STATE
 * if not yet initialized. */
ASX_API asx_status asx_once_get(asx_once_handle handle, uint64_t *out_value);

/* Check if the cell has been initialized. */
ASX_API int asx_once_is_initialized(asx_once_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all once-cell arena state (test support). */
ASX_API void asx_once_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_ONCE_H */
