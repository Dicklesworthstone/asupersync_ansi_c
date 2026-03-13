/*
 * asx/sync/mutex.h — async mutex with guard obligations
 *
 * Mutual exclusion primitive for async workflows. The guard is
 * an obligation that must be released. Cancel-safe: cancellation
 * during lock acquisition does not acquire the lock.
 *
 * Implemented as a semaphore(1) with a typed API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_MUTEX_H
#define ASX_SYNC_MUTEX_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/asx_config.h>
#include <asx/sync/semaphore.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    asx_semaphore_handle sem;
} asx_mutex_handle;

typedef struct {
    asx_semaphore_permit permit;
} asx_mutex_guard;

typedef struct {
    asx_semaphore_waiter waiter;
} asx_mutex_lock_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a mutex (initially unlocked). */
ASX_API ASX_MUST_USE asx_status asx_mutex_create(asx_mutex_handle *out);

/* Close a mutex, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_mutex_close(asx_mutex_handle handle);

/* -------------------------------------------------------------------
 * Lock (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to lock immediately. Returns ASX_OK + guard if unlocked,
 * ASX_E_WOULD_BLOCK if locked. */
ASX_API asx_status asx_mutex_try_lock(
    asx_mutex_handle handle,
    asx_mutex_guard *out);

/* -------------------------------------------------------------------
 * Lock (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async lock. Must be followed by poll_lock calls. */
ASX_API ASX_MUST_USE asx_status asx_mutex_lock_begin(
    asx_mutex_handle handle,
    asx_mutex_lock_waiter *out);

/* Poll for lock. Returns ASX_OK + guard when acquired,
 * ASX_E_PENDING when waiting. */
ASX_API asx_status asx_mutex_poll_lock(
    asx_mutex_lock_waiter *waiter,
    asx_mutex_guard *out,
    asx_cx *cx);

/* Cancel an async lock (safe even if already acquired). */
ASX_API asx_status asx_mutex_lock_cancel(asx_mutex_lock_waiter *waiter);

/* -------------------------------------------------------------------
 * Unlock
 * ------------------------------------------------------------------- */

/* Unlock the mutex (release the guard). Wakes one waiter if any. */
ASX_API asx_status asx_mutex_unlock(asx_mutex_guard guard);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Check if the mutex is currently locked. */
ASX_API int asx_mutex_is_locked(asx_mutex_handle handle);

/* -------------------------------------------------------------------
 * Arena management (delegates to semaphore)
 * ------------------------------------------------------------------- */

/* No separate reset — uses semaphore arena. */

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_MUTEX_H */
