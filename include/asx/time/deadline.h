/*
 * asx/time/deadline.h — deadline abstraction with timer-wheel integration
 *
 * A deadline binds a target time to a timer wheel handle. It provides:
 *   - Registration with the global timer wheel
 *   - Expiry checking against current runtime time
 *   - Budget integration (deadline-triggered poll budget limits)
 *   - Cancel integration (deadline-triggered cancellation)
 *
 * Deadlines are value types. No dynamic allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TIME_DEADLINE_H
#define ASX_TIME_DEADLINE_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/time/timer_wheel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Deadline state
 * ------------------------------------------------------------------- */

typedef struct {
    asx_time target_ns;     /* absolute deadline (nanoseconds) */
    asx_timer_handle timer; /* handle into the timer wheel */
    int registered;         /* 0, or 1 + tag of the runtime whose wheel holds the timer */
    int expired;            /* 1 if deadline has been observed as expired */
} asx_deadline;

/* -------------------------------------------------------------------
 * API: Deadline lifecycle
 * ------------------------------------------------------------------- */

/* Initialize a deadline with an absolute target time.
 * Does NOT register with the timer wheel — call asx_deadline_arm() for that.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if dl is NULL. */
ASX_API ASX_MUST_USE asx_status asx_deadline_init(asx_deadline *dl, asx_time target_ns);

/* Create a deadline relative to the current runtime time.
 * Reads the clock of the task being polled (else the live runtime's) and
 * adds duration_ns, saturating at UINT64_MAX as Rust's deadline arithmetic
 * does (a target that overflows the clock never comes; it is not an error).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if dl is NULL,
 *   ASX_E_HOOK_MISSING if clock hooks are not installed. */
ASX_API ASX_MUST_USE asx_status asx_deadline_after(asx_deadline *dl, uint64_t duration_ns);

/* Arm a deadline by registering it with the runtime's application timer
 * wheel (asx_timer_wheel_global(rt)).
 * waker_data is passed to the timer and returned when the timer fires.
 * As in Rust, a target farther than the wheel's maximum duration (7 days)
 * from now is registered at that maximum: the timer fires early and the
 * owner re-checks the target (asx_deadline_is_expired).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if rt or dl is NULL,
 *   ASX_E_INVALID_STATE if already armed or if no runtime holds rt's tag,
 *   ASX_E_RESOURCE_EXHAUSTED if timer wheel is full. */
ASX_API ASX_MUST_USE asx_status asx_deadline_arm(const asx_runtime *rt, asx_deadline *dl,
                                                 void *waker_data);

/* Disarm a deadline by cancelling its timer.
 * Safe to call if not armed (no-op).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if dl is NULL. */
ASX_API ASX_MUST_USE asx_status asx_deadline_disarm(asx_deadline *dl);

/* -------------------------------------------------------------------
 * API: Deadline queries
 * ------------------------------------------------------------------- */

/* Check if the deadline has expired by comparing against current time.
 * Reads the clock as asx_deadline_after does. Caches the result.
 * Returns 1 if expired (now >= target), 0 otherwise.
 * Returns 0 if dl is NULL or clock is unavailable. */
ASX_API int asx_deadline_is_expired(asx_deadline *dl);

/* Check if the deadline has expired against a provided time.
 * Does NOT read the clock. Caches the result.
 * Returns 1 if expired (now >= target), 0 otherwise. */
ASX_API int asx_deadline_is_expired_at(asx_deadline *dl, asx_time now);

/* Get the remaining nanoseconds until the deadline.
 * Returns 0 if already expired or dl is NULL. */
ASX_API uint64_t asx_deadline_remaining_ns(const asx_deadline *dl, asx_time now);

/* Get the target time of a deadline.
 * Returns 0 if dl is NULL. */
ASX_API asx_time asx_deadline_target(const asx_deadline *dl);

/* Check if a deadline is armed (has a live timer registration).
 * Returns 1 if armed, 0 otherwise. */
ASX_API int asx_deadline_is_armed(const asx_deadline *dl);

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_DEADLINE_H */
