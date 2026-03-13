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

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/asx_config.h>
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
 * uninitialized and returns the error. */
ASX_API asx_status asx_once_get_or_init(
    asx_once_handle handle,
    asx_once_init_fn init_fn,
    void *user_data,
    uint64_t *out_value);

/* Get the value if already initialized. Returns ASX_E_INVALID_STATE
 * if not yet initialized. */
ASX_API asx_status asx_once_get(
    asx_once_handle handle,
    uint64_t *out_value);

/* Check if the cell has been initialized. */
ASX_API int asx_once_is_initialized(asx_once_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

ASX_API void asx_once_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_ONCE_H */
