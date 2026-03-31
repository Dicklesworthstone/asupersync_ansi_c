/*
 * asx/time/timer_wheel.h — timer wheel with deterministic ordering and O(1) cancel
 *
 * Provides timer registration, firing, and cancellation with:
 *   - Deterministic tie-break: same-deadline timers fire in insertion order
 *   - O(1) cancel via generation-validated handles
 *   - Fixed-size arena (no dynamic allocation)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TIME_TIMER_WHEEL_H
#define ASX_TIME_TIMER_WHEEL_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of concurrent timers in the wheel */
#define ASX_MAX_TIMERS 128u

/* Default maximum timer duration (24 hours in nanoseconds) */
#define ASX_TIMER_MAX_DURATION_NS ((uint64_t)86400ULL * 1000000000ULL)

/* -------------------------------------------------------------------
 * Timer handle
 *
 * Opaque, copyable token for cancel/update operations.
 * Contains (id, generation) for stale-handle detection.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;       /* arena slot index */
    uint32_t generation; /* generation at registration time */
} asx_timer_handle;

/* -------------------------------------------------------------------
 * Timer wheel (opaque, initialized by asx_timer_wheel_init)
 * ------------------------------------------------------------------- */

typedef struct asx_timer_wheel asx_timer_wheel;

/* Initialize a timer wheel. Must be called before any other operation.
 * Sets current_time to 0 and all slots to empty. */
ASX_API void asx_timer_wheel_init(asx_timer_wheel *wheel);

/* Reset a timer wheel to its initial state (test support). */
ASX_API void asx_timer_wheel_reset(asx_timer_wheel *wheel);

/* -------------------------------------------------------------------
 * Timer registration
 * ------------------------------------------------------------------- */

/* Register a timer that fires at the given deadline.
 * Returns ASX_OK on success and fills *out_handle.
 * Returns ASX_E_TIMER_DURATION_EXCEEDED if deadline - now > max duration.
 * Returns ASX_E_RESOURCE_EXHAUSTED if no slots available. */
ASX_API ASX_MUST_USE asx_status asx_timer_register(asx_timer_wheel *wheel, asx_time deadline,
                                                   void *waker_data, asx_timer_handle *out_handle);

/* -------------------------------------------------------------------
 * Timer cancellation (O(1) logical cancel)
 *
 * Looks up the handle's slot and checks generation. If the handle
 * is live, marks the timer as cancelled and returns 1. If stale or
 * already cancelled/fired, returns 0. No effect on other timers.
 * ------------------------------------------------------------------- */

/* Cancel a timer by handle. Returns 1 if cancelled, 0 if stale/fired.
 *
 * Preconditions: wheel and handle must not be NULL.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Timer. */
ASX_API int asx_timer_cancel(asx_timer_wheel *wheel, const asx_timer_handle *handle);

/* -------------------------------------------------------------------
 * Timer collection (fire expired timers)
 *
 * Advances the wheel to 'now' and collects all timers with
 * deadline <= now into out_wakers (up to max_wakers).
 * Returns the number of timers collected.
 *
 * Deterministic ordering: timers are returned sorted by
 * (deadline ASC, insertion_seq ASC) for deterministic replay.
 * ------------------------------------------------------------------- */

/* Collect expired timers into out_wakers. Returns the count collected.
 *
 * Preconditions: wheel and out_wakers must not be NULL.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_timer_collect_expired(asx_timer_wheel *wheel, asx_time now, void **out_wakers,
                                           uint32_t max_wakers);

/* -------------------------------------------------------------------
 * Timer update (cancel + re-register)
 *
 * Registers the replacement timer before retiring the old one so the
 * operation is failure-atomic. If the new registration fails, the old
 * timer remains live. If the old handle is stale, the new timer is
 * still registered (old cancel is a no-op).
 * ------------------------------------------------------------------- */

/* Register a new timer and then retire the old one with updated deadline.
 *
 * Preconditions: wheel and out_handle must not be NULL.
 * old_handle may be NULL (treated as a pure register operation).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if NULL params,
 *   ASX_E_RESOURCE_EXHAUSTED if timer arena is full, and leaves the old
 *   timer untouched on registration failure.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_timer_update(asx_timer_wheel *wheel,
                                                 const asx_timer_handle *old_handle,
                                                 asx_time new_deadline, void *waker_data,
                                                 asx_timer_handle *out_handle);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Return the number of active (live, non-cancelled) timers. */
ASX_API uint32_t asx_timer_active_count(const asx_timer_wheel *wheel);

/* Set the maximum allowed timer duration in nanoseconds. */
ASX_API void asx_timer_set_max_duration(asx_timer_wheel *wheel, uint64_t max_duration_ns);

/* Advance the wheel's current time without collecting. */
ASX_API void asx_timer_advance(asx_timer_wheel *wheel, asx_time now);

/* -------------------------------------------------------------------
 * Singleton wheel (global instance for walking skeleton)
 * ------------------------------------------------------------------- */

/* Get the global timer wheel instance. */
ASX_API asx_timer_wheel *asx_timer_wheel_global(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_TIMER_WHEEL_H */
