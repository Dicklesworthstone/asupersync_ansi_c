/*
 * asx/runtime/deadline_monitor.h — runtime deadline monitor
 *
 * Provides per-task deadline registration, miss detection, and
 * statistics for the runtime scheduler. The deadline monitor runs
 * as a lightweight subsystem that integrates with the time/deadline
 * primitives and the diagnostic/telemetry layer.
 *
 * Walking skeleton: single-threaded, check-on-poll model.
 * Deadlines are evaluated when the caller invokes
 * asx_deadline_monitor_check().
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_DEADLINE_MONITOR_H
#define ASX_RUNTIME_DEADLINE_MONITOR_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_DEADLINE_MONITORS 32u

/* -------------------------------------------------------------------
 * Deadline entry state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DEADLINE_PENDING = 0,  /* registered, not yet evaluated as expired */
    ASX_DEADLINE_MET = 1,      /* completed before deadline */
    ASX_DEADLINE_MISSED = 2,   /* deadline elapsed without completion */
    ASX_DEADLINE_CANCELLED = 3 /* explicitly cancelled before resolution */
} asx_deadline_state;

/* -------------------------------------------------------------------
 * Deadline monitor handle — returned on registration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_deadline_monitor_handle;

/* -------------------------------------------------------------------
 * Deadline monitor callback — invoked on miss detection
 * ------------------------------------------------------------------- */

/* Called when a deadline miss is detected during check().
 * entity_id identifies the task/scope, margin_ns is how far past
 * the deadline (positive = amount overdue). user_data is from
 * registration. */
typedef void (*asx_deadline_miss_fn)(uint64_t entity_id, uint64_t margin_ns,
                                     void *user_data);

/* -------------------------------------------------------------------
 * Deadline monitor statistics
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t total_registered;  /* total deadlines ever registered */
    uint32_t active_count;      /* currently pending deadlines */
    uint32_t met_count;         /* deadlines met */
    uint32_t missed_count;      /* deadlines missed */
    uint32_t cancelled_count;   /* deadlines cancelled */
    int64_t worst_margin_ns;    /* most negative margin (worst miss) */
    int64_t best_margin_ns;     /* most positive margin (best hit) */
} asx_deadline_monitor_stats;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Initialize the deadline monitor subsystem.
 * Re-initialization resets all prior registrations.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_deadline_monitor_init(void);

/* Shut down the deadline monitor, cancelling all pending deadlines. */
ASX_API void asx_deadline_monitor_shutdown(void);

/* Query whether the deadline monitor is initialized. */
ASX_API int asx_deadline_monitor_is_initialized(void);

/* -------------------------------------------------------------------
 * API: Registration
 * ------------------------------------------------------------------- */

/* Register a deadline for a task/entity.
 * target_ns: absolute time at which the deadline expires.
 * entity_id: the task or scope being monitored.
 * on_miss: optional callback invoked when miss is detected (may be NULL).
 * user_data: passed to on_miss callback.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT if out_handle is NULL.
 * Returns ASX_E_INVALID_STATE if monitor is not initialized.
 * Returns ASX_E_RESOURCE_EXHAUSTED if no slots available. */
ASX_API ASX_MUST_USE asx_status asx_deadline_monitor_register(
    asx_time target_ns, uint64_t entity_id,
    asx_deadline_miss_fn on_miss, void *user_data,
    asx_deadline_monitor_handle *out_handle);

/* Cancel a pending deadline.
 * Safe to call on already-resolved deadlines (no-op).
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT if handle is NULL.
 * Returns ASX_E_NOT_FOUND for stale/unknown handles. */
ASX_API ASX_MUST_USE asx_status asx_deadline_monitor_cancel(
    const asx_deadline_monitor_handle *handle);

/* Mark a deadline as met (task completed before deadline).
 * completion_ns: the time at which the task completed.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT if handle is NULL.
 * Returns ASX_E_NOT_FOUND for stale/unknown handles.
 * Returns ASX_E_INVALID_STATE if deadline is not PENDING. */
ASX_API ASX_MUST_USE asx_status asx_deadline_monitor_complete(
    const asx_deadline_monitor_handle *handle, asx_time completion_ns);

/* -------------------------------------------------------------------
 * API: Check / evaluation
 * ------------------------------------------------------------------- */

/* Check all pending deadlines against the given time.
 * Any deadline whose target_ns <= now_ns transitions to MISSED,
 * and its on_miss callback (if set) is invoked.
 * Returns the number of newly detected misses. */
ASX_API uint32_t asx_deadline_monitor_check(asx_time now_ns);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the state of a registered deadline.
 * Returns ASX_DEADLINE_CANCELLED for stale/unknown handles. */
ASX_API asx_deadline_state asx_deadline_monitor_get_state(
    const asx_deadline_monitor_handle *handle);

/* Get the count of currently active (pending) deadlines. */
ASX_API uint32_t asx_deadline_monitor_active_count(void);

/* Get statistics for the deadline monitor. */
ASX_API void asx_deadline_monitor_get_stats(asx_deadline_monitor_stats *out);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all deadline monitor state (test support). */
ASX_API void asx_deadline_monitor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_DEADLINE_MONITOR_H */
