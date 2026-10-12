/*
 * asx/time/timer_wheel.h — hierarchical timer wheel, a port of Rust's TimerWheel
 *
 * Provides timer registration, firing, and cancellation with:
 *   - Rust's order: four levels of 256 slots (1 ms, 256 ms, 65.5 s, 4.66 h
 *     per slot). A timer lands in a slot by its deadline's 1 ms tick, so
 *     timers in the same tick fire in the order they reached the slot, not
 *     by exact deadline (time/wheel.rs:710-967)
 *   - O(1) cancel via generation-validated handles
 *   - Fixed-size arena (no dynamic allocation)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TIME_TIMER_WHEEL_H
#define ASX_TIME_TIMER_WHEEL_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of concurrent timers in the wheel (a build-time resource
 * class sizes it when not set explicitly, asx_config.h) */
#if (ASX_MAX_TIMERS) < 1
#error "ASX_MAX_TIMERS must be at least 1"
#endif

/* Default maximum timer duration: 7 days in nanoseconds, Rust's
 * TimerWheelConfig::max_timer_duration (time/wheel.rs:98-104). A deadline
 * farther ahead than this is clamped to it. */
#define ASX_TIMER_MAX_DURATION_NS ((uint64_t)604800ULL * 1000000000ULL)

/* Default range the wheel holds directly: 24 hours, Rust's
 * TimerWheelConfig::max_wheel_duration (time/wheel.rs:98-104). A timer
 * farther ahead waits in an overflow heap and joins the wheel as time
 * advances; this changes where it waits, never what fires or in which
 * order. */
#define ASX_TIMER_MAX_WHEEL_DURATION_NS ((uint64_t)86400ULL * 1000000000ULL)

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
 *
 * As Rust's TimerWheel::register (time/wheel.rs:515-528), a deadline
 * farther than the maximum timer duration (7 days by default) past the
 * wheel's time is clamped to that duration, not refused: the timer fires
 * early and its owner re-registers if the true deadline has not come. The
 * wheel's time is the last time advanced to, in whole milliseconds.
 *
 * Returns ASX_OK on success and fills *out_handle.
 * Returns ASX_E_RESOURCE_EXHAUSTED if no slots available; the wheel is
 * unchanged. */
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
 * Order is the Rust wheel's, not deadline order: timers whose deadline
 * falls in the same 1 ms tick are returned in the order they reached that
 * tick's slot (registration order for timers placed directly; timers that
 * came down from a coarser level follow those already in the slot). Timers
 * in earlier ticks come first. A timer 5.7 ms out registered before one
 * 5.2 ms out is returned first when both are due in one call.
 * ------------------------------------------------------------------- */

/* Collect expired timers into out_wakers. Returns the count collected.
 * Due timers beyond max_wakers stay registered, in order, for the next
 * call.
 *
 * Preconditions: wheel must not be NULL; with out_wakers NULL or
 * max_wakers 0 the wheel only advances.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_timer_collect_expired(asx_timer_wheel *wheel, asx_time now, void **out_wakers,
                                           uint32_t max_wakers);

/* -------------------------------------------------------------------
 * Timer update (cancel + re-register)
 *
 * As the Rust timer driver's update: only a live timer is moved. A stale
 * handle (fired, cancelled, or its slot reused) is refused and nothing
 * is registered. A live timer is re-armed in its own slot under a new
 * generation, so the old handle goes stale and a full wheel cannot
 * refuse the update. The moved timer goes behind the timers already
 * registered, as a fresh registration does. As in register, a deadline
 * beyond the maximum duration is clamped.
 * ------------------------------------------------------------------- */

/* Move a live timer to a new deadline and waker.
 *
 * Preconditions: wheel and out_handle must not be NULL.
 * old_handle may be NULL (treated as a pure register operation).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if NULL params, and
 *   ASX_E_STALE_HANDLE if old_handle is not a live timer. On failure the
 *   wheel and out_handle are unchanged.
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

/* Set the maximum timer duration in nanoseconds: registrations farther
 * ahead are clamped to it. */
ASX_API void asx_timer_set_max_duration(asx_timer_wheel *wheel, uint64_t max_duration_ns);

/* Advance the wheel's current time without collecting. Timers that come
 * due are readied, not fired: collect_expired returns them. */
ASX_API void asx_timer_advance(asx_timer_wheel *wheel, asx_time now);

/* The earliest live deadline, clamped to the wheel's time (whole
 * milliseconds) when a timer is already due: Rust's
 * TimerWheel::next_deadline with coalescing off. Returns 1 and fills
 * *out_deadline, or 0 when no timer is pending. */
ASX_API int asx_timer_next_deadline(const asx_timer_wheel *wheel, asx_time *out_deadline);

/* -------------------------------------------------------------------
 * Singleton wheel (global instance for walking skeleton)
 * ------------------------------------------------------------------- */

/* Get the global timer wheel instance. The runtime never collects it. */
ASX_API asx_timer_wheel *asx_timer_wheel_global(void);

#ifdef ASX_INTERNAL_TIMER_WHEEL_ACCESS
/* The scheduler's own wheel for task wake and budget-deadline timers: the
 * same implementation as the global one, a separate instance with room
 * for two timers per task, emitting no trace events of its own (the
 * scheduler records sleeps itself). */
ASX_API asx_timer_wheel *asx_timer_wheel_runtime(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_TIMER_WHEEL_H */
