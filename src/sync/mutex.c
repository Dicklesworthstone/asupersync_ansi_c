/*
 * mutex.c — async mutex (semaphore(1) wrapper)
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/mutex.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_mutex_create(asx_mutex_handle *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_semaphore_create(1, &out->sem);
}

asx_status asx_mutex_close(asx_mutex_handle handle) { return asx_semaphore_close(handle.sem); }

/* ------------------------------------------------------------------ */
/* Lock (non-blocking)                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_mutex_try_lock(asx_mutex_handle handle, asx_mutex_guard *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_semaphore_try_acquire(handle.sem, &out->permit);
}

/* ------------------------------------------------------------------ */
/* Lock (async)                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_mutex_lock_begin(asx_mutex_handle handle, asx_mutex_lock_waiter *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_semaphore_acquire_begin(handle.sem, &out->waiter);
}

asx_status asx_mutex_poll_lock(asx_mutex_lock_waiter *waiter, asx_mutex_guard *out, asx_cx *cx) {
    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_semaphore_poll_acquire(&waiter->waiter, &out->permit, cx);
}

asx_status asx_mutex_lock_cancel(asx_mutex_lock_waiter *waiter) {
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_semaphore_acquire_cancel(&waiter->waiter);
}

/* ------------------------------------------------------------------ */
/* Unlock                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_mutex_unlock(asx_mutex_guard guard) { return asx_semaphore_release(guard.permit); }

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

int asx_mutex_is_locked(asx_mutex_handle handle) {
    return asx_semaphore_available(handle.sem) == 0;
}
