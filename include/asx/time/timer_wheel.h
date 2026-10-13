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

/* The application timer wheel of a runtime (one per runtime, set up on
 * first use; the zeroed runtime object names the library's static kernel).
 * The runtime never collects it. NULL for NULL or a tag no runtime holds. */
ASX_API asx_timer_wheel *asx_timer_wheel_global(const asx_runtime *rt);

#ifdef __cplusplus
}
#endif

#endif /* ASX_TIME_TIMER_WHEEL_H */

/* -------------------------------------------------------------------
 * Internal access: the wheel's layout and the scheduler's instance.
 *
 * Guarded apart from the public part, so a unit that included the header
 * without the access macro and then asks for it (runtime_internal.h) gets
 * the layout too. The runtime kernel embeds the scheduler's wheel and its
 * arena, one per runtime.
 * ------------------------------------------------------------------- */

#if defined(ASX_INTERNAL_TIMER_WHEEL_ACCESS) && !defined(ASX_TIME_TIMER_WHEEL_INTERNAL_H)
#define ASX_TIME_TIMER_WHEEL_INTERNAL_H

/* Geometry (wheel.rs:46-57) */
#define TW_LEVELS 4u
#define TW_SLOTS 256u
#define TW_BITMAP_WORDS (TW_SLOTS / 64u)
#define TW_TICK_NS ((uint64_t)1000000u) /* level 0 resolution: 1 ms */
#define TW_NIL 0xFFFFu

/* Arena indices are 16-bit. */
#if (ASX_MAX_TIMERS) > 65534
#error "ASX_MAX_TIMERS must be at most 65534"
#endif
#if (2 * (ASX_MAX_TASKS)) > 65534
#error "twice ASX_MAX_TASKS must be at most 65534 (the runtime wheel holds two timers per task)"
#endif

#define TW_APP_CAPACITY ((uint32_t)(ASX_MAX_TIMERS))
#define TW_RUNTIME_CAPACITY (2u * (uint32_t)(ASX_MAX_TASKS))

/* Where a timer is stored. */
#define TW_FREE 0u
#define TW_READY 1u
#define TW_BUCKET 2u
#define TW_OVERFLOW 3u

typedef struct {
    /* When this timer fires (clamped to the maximum duration). */
    asx_time deadline;
    /* Opaque callback data. */
    void *waker_data;
    /* Registration order: the overflow heap's tie-break. */
    uint64_t seq;
    /* For stale-handle detection. */
    uint32_t generation;
    /* List links; `next` is also the free-list link. */
    uint16_t prev;
    uint16_t next;
    /* Position in the overflow heap. */
    uint16_t heap_pos;
    /* TW_FREE, TW_READY, TW_BUCKET or TW_OVERFLOW; for TW_BUCKET, the
     * wheel level and the slot within it. */
    uint8_t where;
    uint8_t level;
    uint8_t slot;
} tw_entry;

typedef struct {
    uint16_t head;
    uint16_t tail;
} tw_list;

struct asx_timer_wheel {
    tw_entry *entries;
    uint16_t *overflow; /* binary min-heap of entry indices by (deadline, seq) */
    uint32_t capacity;
    uint32_t slot_count; /* arena high-water mark */
    uint32_t free_head;  /* LIFO list of released slots, as Rust's slab */
    uint32_t active_count;
    uint32_t overflow_len;
    uint64_t next_seq;
    uint64_t current_tick; /* current time in 1 ms ticks */
    uint32_t cursor[TW_LEVELS];
    uint64_t occupied[TW_LEVELS][TW_BITMAP_WORDS];
    tw_list bucket[TW_LEVELS][TW_SLOTS];
    tw_list ready;
    uint64_t max_timer_duration_ns; /* longer registrations are clamped (wheel.rs:98-104) */
    uint64_t max_wheel_duration_ns; /* longer ones wait in the overflow heap */
    int traced;                     /* emit TIMER_SET / TIMER_FIRE / TIMER_CANCEL */
    uint16_t tag;                   /* the runtime whose trace those events go to */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Set up `wheel` as a scheduler wheel over the caller's arena of
 * TW_RUNTIME_CAPACITY entries and overflow slots: the same implementation as
 * the global wheel, with room for two timers per task, emitting no trace
 * events of its own (the scheduler records sleeps itself). Clears the
 * arena; a wheel set up once is reset with asx_timer_wheel_reset, which
 * keeps stale-handle safety across the reset. */
ASX_API void asx_timer_wheel_setup_scheduler(asx_timer_wheel *wheel, tw_entry *entries,
                                             uint16_t *overflow);

#ifdef __cplusplus
}
#endif

#endif /* ASX_INTERNAL_TIMER_WHEEL_ACCESS */
