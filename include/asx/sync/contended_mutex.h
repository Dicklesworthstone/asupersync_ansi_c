/*
 * asx/sync/contended_mutex.h — instrumented mutex with contention metrics
 *
 * Wraps asx_mutex with per-lock instrumentation: tracks acquisitions,
 * contentions (failed try_lock), and max contention streak. Useful for
 * diagnosing lock hotspots in production. Zero overhead when compiled
 * without ASX_LOCK_METRICS.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_CONTENDED_MUTEX_H
#define ASX_SYNC_CONTENDED_MUTEX_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/sync/mutex.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Metrics snapshot
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t acquisitions;       /* total successful lock acquisitions */
    uint64_t contentions;        /* times try_lock returned WOULD_BLOCK */
    uint64_t max_contention_run; /* longest consecutive contention streak */
} asx_lock_metrics_snapshot;

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_CONTENDED_MUTEX_MAX
#define ASX_CONTENDED_MUTEX_MAX 16u
#endif

/* -------------------------------------------------------------------
 * Handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_contended_mutex_handle;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create an instrumented mutex. */
ASX_API ASX_MUST_USE asx_status asx_contended_mutex_create(asx_contended_mutex_handle *out);

/* Close an instrumented mutex. */
ASX_API asx_status asx_contended_mutex_close(asx_contended_mutex_handle handle);

/* -------------------------------------------------------------------
 * Lock / Unlock (delegates to inner mutex)
 * ------------------------------------------------------------------- */

/* Try to lock immediately. Tracks contention on failure. */
ASX_API asx_status asx_contended_mutex_try_lock(asx_contended_mutex_handle handle,
                                                asx_mutex_guard *out);

/* Unlock (delegates directly to mutex). */
ASX_API asx_status asx_contended_mutex_unlock(asx_mutex_guard guard);

/* -------------------------------------------------------------------
 * Async lock (delegates to inner mutex with instrumentation)
 * ------------------------------------------------------------------- */

/* Begin async lock acquisition. */
ASX_API ASX_MUST_USE asx_status asx_contended_mutex_lock_begin(asx_contended_mutex_handle handle,
                                                               asx_mutex_lock_waiter *out);

/* Poll for async lock. Tracks contention on PENDING. */
ASX_API asx_status asx_contended_mutex_poll_lock(asx_contended_mutex_handle handle,
                                                 asx_mutex_lock_waiter *waiter,
                                                 asx_mutex_guard *out, asx_cx *cx);

/* Cancel an async lock acquisition. */
ASX_API asx_status asx_contended_mutex_lock_cancel(asx_mutex_lock_waiter *waiter);

/* -------------------------------------------------------------------
 * Metrics
 * ------------------------------------------------------------------- */

/* Get a snapshot of contention metrics. */
ASX_API asx_status asx_contended_mutex_metrics(asx_contended_mutex_handle handle,
                                               asx_lock_metrics_snapshot *out);

/* Reset contention metrics to zero. */
ASX_API asx_status asx_contended_mutex_reset_metrics(asx_contended_mutex_handle handle);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Check if the underlying mutex is currently locked. */
ASX_API int asx_contended_mutex_is_locked(asx_contended_mutex_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all contended mutex arena state. For tests only. */
ASX_API void asx_contended_mutex_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_CONTENDED_MUTEX_H */
