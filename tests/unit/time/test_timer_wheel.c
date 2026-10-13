/*
 * test_timer_wheel.c — timer wheel unit tests (bd-2cw.4)
 *
 * Tests timer registration, firing, ordering, O(1) cancel, generation
 * validation, resource exhaustion, and churn scenarios.
 *
 * Ordering is Rust's (asupersync 5e60b1c, src/time/wheel.rs), not a sort by
 * deadline: a timer is filed by its deadline's 1 ms tick, timers in one
 * tick fire in the order they reached it, and a timer from a coarser level
 * follows the ones already in its slot. Vectors cite the Rust function that
 * decides them; the ones taken from a Rust test say so.
 *
 * SPDX-License-Identifier: MIT
 */

/* The runtime's own wheel is declared for internal access only. */
#define ASX_INTERNAL_TIMER_WHEEL_ACCESS 1

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

#define TW_MS ((asx_time)1000000u)
#define TW_DAY ((asx_time)86400u * (asx_time)1000000000u)

/* Suppress warn_unused_result for intentionally-ignored calls */
#define REGISTER_IGNORE(w, dl, data, h)                                                            \
    do {                                                                                           \
        asx_status s_ = asx_timer_register((w), (dl), (data), (h));                                \
        (void)s_;                                                                                  \
    } while (0)

static uint64_t timer_trace_entity_id(const asx_timer_handle *handle) {
    return ((uint64_t)handle->slot << 32) | (uint64_t)handle->generation;
}

/* -------------------------------------------------------------------
 * Test: register and fire a single timer
 * ------------------------------------------------------------------- */

TEST(timer_register_and_fire) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0xAABB, &h), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);

    /* Advance past deadline and collect */
    count = asx_timer_collect_expired(w, 100, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xAABB);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);
}

/* -------------------------------------------------------------------
 * Test: timer does not fire before deadline
 * ------------------------------------------------------------------- */

TEST(timer_does_not_fire_early) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0x1234, &h), ASX_OK);

    /* Advance to just before deadline */
    count = asx_timer_collect_expired(w, 99, wakers, 4);
    ASSERT_EQ(count, (uint32_t)0);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);
}

/* -------------------------------------------------------------------
 * Test: cancel prevents timer from firing
 * ------------------------------------------------------------------- */

TEST(timer_cancel_prevents_fire) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    uint32_t count;
    int cancelled;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0x5678, &h), ASX_OK);

    cancelled = asx_timer_cancel(w, &h);
    ASSERT_TRUE(cancelled);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);

    /* Advance past deadline — should collect nothing */
    count = asx_timer_collect_expired(w, 200, wakers, 4);
    ASSERT_EQ(count, (uint32_t)0);
}

TEST(timer_trace_emits_set_fire_and_cancel) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle fire_h, cancel_h;
    asx_trace_event ev;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);
    asx_trace_reset();

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0xAABB, &fire_h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 200, (void *)0xCCDD, &cancel_h), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &cancel_h));

    count = asx_timer_collect_expired(w, 100, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xAABB);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)4);

    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_SET);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&fire_h));
    ASSERT_EQ(ev.aux, (uint64_t)100);

    ASSERT_TRUE(asx_trace_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_SET);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&cancel_h));
    ASSERT_EQ(ev.aux, (uint64_t)200);

    ASSERT_TRUE(asx_trace_event_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_CANCEL);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&cancel_h));
    ASSERT_EQ(ev.aux, (uint64_t)200);

    ASSERT_TRUE(asx_trace_event_get(3, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TIMER_FIRE);
    ASSERT_EQ(ev.entity_id, timer_trace_entity_id(&fire_h));
    ASSERT_EQ(ev.aux, (uint64_t)100);
}

/* -------------------------------------------------------------------
 * Test: stale handle cancel returns false
 * ------------------------------------------------------------------- */

TEST(timer_stale_handle_cancel_returns_false) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h1, h2;

    asx_timer_wheel_reset(w);

    /* Register and cancel to make handle stale */
    ASSERT_EQ(asx_timer_register(w, 100, NULL, &h1), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &h1));

    /* Re-register in same slot — new generation */
    ASSERT_EQ(asx_timer_register(w, 200, NULL, &h2), ASX_OK);

    /* Old handle should be stale */
    ASSERT_FALSE(asx_timer_cancel(w, &h1));
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);
}

/* -------------------------------------------------------------------
 * Test: cancel after fire returns false
 * ------------------------------------------------------------------- */

TEST(timer_cancel_after_fire_returns_false) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, NULL, &h), ASX_OK);

    /* Fire the timer */
    asx_timer_collect_expired(w, 200, wakers, 4);

    /* Cancel after fire — should return false */
    ASSERT_FALSE(asx_timer_cancel(w, &h));
}

/* -------------------------------------------------------------------
 * Test: same-deadline timers fire in insertion order (deterministic)
 * ------------------------------------------------------------------- */

TEST(timer_tiebreak_insertion_order) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h1, h2, h3;
    void *wakers[8];
    uint32_t count;

    asx_timer_wheel_reset(w);

    /* Register 3 timers with same deadline */
    ASSERT_EQ(asx_timer_register(w, 100, (void *)1, &h1), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)2, &h2), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)3, &h3), ASX_OK);

    /* Collect — should be in insertion order (1, 2, 3) */
    count = asx_timer_collect_expired(w, 100, wakers, 8);
    ASSERT_EQ(count, (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(wakers[1], (void *)2);
    ASSERT_EQ(wakers[2], (void *)3);
}

/* -------------------------------------------------------------------
 * Ordering is the wheel's, not the deadline's
 *
 * Rust files a timer by its deadline's 1 ms tick (insert_entry_at,
 * wheel.rs:710-755): into that tick's slot, or on the ready list when the
 * tick has come. A due slot's timers move to the ready list in slot order
 * (tick_level0 :849-864, collect_bucket :889-902), and drain_ready
 * (:921-) wakes the ready list front to back. Two timers in one tick
 * therefore fire in the order they were registered, whatever their exact
 * deadlines.
 * ------------------------------------------------------------------- */

/* The bd-9kll.7.1 example: A at 5.7 ms registered before B at 5.2 ms. Both
 * sit in tick 5's slot as [A, B] and are readied in that order. */
TEST(timer_same_tick_fires_in_registration_order) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS + 700000u, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS + 200000u, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 6u * TW_MS, wakers, 4), (uint32_t)2);
    ASSERT_EQ(wakers[0], (void *)1); /* 5.7 ms, registered first */
    ASSERT_EQ(wakers[1], (void *)2); /* 5.2 ms, registered second */
}

/* Timers in different ticks fire in tick order, whatever the registration
 * order: the slots are visited as time advances. */
TEST(timer_ticks_fire_in_tick_order) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 12u * TW_MS + 500000u, (void *)3, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 3u * TW_MS + 900000u, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 7u * TW_MS + 100000u, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 20u * TW_MS, wakers, 4), (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)1); /* tick 3 */
    ASSERT_EQ(wakers[1], (void *)2); /* tick 7 */
    ASSERT_EQ(wakers[2], (void *)3); /* tick 12 */
}

/* Two ticks interleaved in registration order: each tick's timers keep
 * their registration order, the earlier tick comes first. */
TEST(timer_interleaved_ticks_keep_registration_order_within_each) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[8];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 9u * TW_MS + 900000u, (void *)11, &h), ASX_OK); /* tick 9 */
    ASSERT_EQ(asx_timer_register(w, 4u * TW_MS + 200000u, (void *)21, &h), ASX_OK); /* tick 4 */
    ASSERT_EQ(asx_timer_register(w, 9u * TW_MS + 100000u, (void *)12, &h), ASX_OK); /* tick 9 */
    ASSERT_EQ(asx_timer_register(w, 4u * TW_MS + 800000u, (void *)22, &h), ASX_OK); /* tick 4 */
    ASSERT_EQ(asx_timer_register(w, 9u * TW_MS + 500000u, (void *)13, &h), ASX_OK); /* tick 9 */

    ASSERT_EQ(asx_timer_collect_expired(w, 10u * TW_MS, wakers, 8), (uint32_t)5);
    ASSERT_EQ(wakers[0], (void *)21);
    ASSERT_EQ(wakers[1], (void *)22);
    ASSERT_EQ(wakers[2], (void *)11);
    ASSERT_EQ(wakers[3], (void *)12);
    ASSERT_EQ(wakers[4], (void *)13);
}

/* Deadlines inside the wheel's current millisecond are on the ready list
 * from registration (insert_entry_at :711-713, :737-738), in registration
 * order. These are the ns-scale values the C wheel used to sort by exact
 * deadline: Rust fires them as registered. */
TEST(timer_sub_millisecond_deadlines_fire_in_registration_order) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[8];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 300, (void *)3, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 200, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 300, wakers, 8), (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)3);
    ASSERT_EQ(wakers[1], (void *)1);
    ASSERT_EQ(wakers[2], (void *)2);
}

TEST(timer_ready_list_keeps_registration_order_across_equal_and_earlier_deadlines) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[8];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)10, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)20, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)30, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 50, (void *)5, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)40, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 100, wakers, 8), (uint32_t)5);
    ASSERT_EQ(wakers[0], (void *)10);
    ASSERT_EQ(wakers[1], (void *)20);
    ASSERT_EQ(wakers[2], (void *)30);
    ASSERT_EQ(wakers[3], (void *)5); /* deadline 50, but registered fourth */
    ASSERT_EQ(wakers[4], (void *)40);
}

/* A timer in the current tick that is not yet due stays on the ready list
 * (drain_ready re-inserts it, :948-949) and fires on a later collect;
 * a due one beside it fires at once. */
TEST(timer_in_current_tick_but_not_due_waits) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS + 700000u, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS + 200000u, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 5u * TW_MS + 400000u, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)2);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);

    ASSERT_EQ(asx_timer_collect_expired(w, 5u * TW_MS + 800000u, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);
}

/* A timer 256 ms or more away waits in a coarser level and joins its
 * level-0 slot when the level cascades (cascade, wheel.rs:865-888), behind
 * whatever the slot already holds.
 *
 * A registers 300 ms ahead at time 0: level 1, slot 1. B registers the same
 * deadline at 100 ms, 200 ms ahead: level 0, slot 300 % 256 = 44, directly.
 * At tick 256 level 1 cascades and A lands in slot 44 after B, so B fires
 * first although A was registered first (and has the same deadline). */
TEST(timer_cascaded_from_coarser_level_follows_direct_timer_in_slot) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 300u * TW_MS, (void *)1, &h), ASX_OK);
    asx_timer_advance(w, 100u * TW_MS);
    ASSERT_EQ(asx_timer_register(w, 300u * TW_MS, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 300u * TW_MS, wakers, 4), (uint32_t)2);
    ASSERT_EQ(wakers[0], (void *)2); /* B, direct */
    ASSERT_EQ(wakers[1], (void *)1); /* A, cascaded */
}

/* The cascade keeps registration order among the timers that came down
 * together: three timers 300 ms out (level 1, slot 1) fire as registered. */
TEST(timer_cascade_preserves_slot_order) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 300u * TW_MS + 600000u, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 300u * TW_MS + 100000u, (void *)2, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 300u * TW_MS + 300000u, (void *)3, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 301u * TW_MS, wakers, 4), (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(wakers[1], (void *)2);
    ASSERT_EQ(wakers[2], (void *)3);
}

/* A timer in a long-range level fires at its own deadline, not at the
 * cascade that reaches it. */
TEST(timer_in_coarse_levels_fires_at_its_deadline) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    /* 90 s: level 2. 5 h: level 3. */
    ASSERT_EQ(asx_timer_register(w, 90u * 1000u * TW_MS, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 5u * 3600u * 1000u * TW_MS, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 90u * 1000u * TW_MS - 1u, wakers, 4), (uint32_t)0);
    ASSERT_EQ(asx_timer_collect_expired(w, 90u * 1000u * TW_MS, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(asx_timer_collect_expired(w, 5u * 3600u * 1000u * TW_MS - 1u, wakers, 4),
              (uint32_t)0);
    ASSERT_EQ(asx_timer_collect_expired(w, 5u * 3600u * 1000u * TW_MS, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)2);
}

/* -------------------------------------------------------------------
 * next_deadline (TimerWheel::next_deadline, coalescing off)
 * ------------------------------------------------------------------- */

TEST(timer_next_deadline_none_when_empty) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    asx_time next = 77u;

    asx_timer_wheel_reset(w);
    ASSERT_EQ(asx_timer_next_deadline(w, &next), 0);
    ASSERT_EQ(asx_timer_next_deadline(NULL, &next), 0);
    ASSERT_EQ(asx_timer_next_deadline(w, NULL), 0);

    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS, NULL, &h), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &h));
    ASSERT_EQ(asx_timer_next_deadline(w, &next), 0);
}

/* Rust next_deadline_ready_same_tick_returns_actual_deadline
 * (wheel.rs:1459): a 500 us deadline is on the ready list, still in the
 * current tick, and is reported as itself, not rounded to the tick. */
TEST(timer_next_deadline_same_tick_future_deadline_is_kept) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    asx_time next = 0u;

    asx_timer_wheel_reset(w);
    ASSERT_EQ(asx_timer_register(w, 500000u, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_timer_next_deadline(w, &next), 1);
    ASSERT_EQ(next, (asx_time)500000u);
}

/* A timer that is already due is reported at the wheel's time, whole
 * milliseconds (raw_next_deadline, wheel.rs:642-643). */
TEST(timer_next_deadline_clamps_a_due_timer_to_wheel_time) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    asx_time next = 0u;

    asx_timer_wheel_reset(w);
    ASSERT_EQ(asx_timer_register(w, 500000u, NULL, &h), ASX_OK);
    asx_timer_advance(w, 1u * TW_MS + 300000u);
    ASSERT_EQ(asx_timer_next_deadline(w, &next), 1);
    ASSERT_EQ(next, 1u * TW_MS);
}

/* Rust next_deadline_sees_earlier_uncascaded_higher_level_entry
 * (wheel.rs:1478): A at 300 ms sits in level 1 until the tick-256 cascade;
 * B at 400 ms, registered at 200 ms, sits in a wrapped level-0 slot. The
 * next deadline is A's, and A fires at 300 ms. */
TEST(timer_next_deadline_sees_uncascaded_higher_level_entry) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    asx_time next = 0u;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 300u * TW_MS, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_collect_expired(w, 200u * TW_MS, wakers, 4), (uint32_t)0);
    ASSERT_EQ(asx_timer_register(w, 400u * TW_MS, (void *)2, &h), ASX_OK);

    ASSERT_EQ(asx_timer_next_deadline(w, &next), 1);
    ASSERT_EQ(next, 300u * TW_MS);

    ASSERT_EQ(asx_timer_collect_expired(w, 300u * TW_MS, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
}

/* -------------------------------------------------------------------
 * Test: resource exhaustion
 * ------------------------------------------------------------------- */

TEST(timer_resource_exhaustion) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    uint32_t i;
    asx_status st;

    asx_timer_wheel_reset(w);

    /* Fill all slots */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        st = asx_timer_register(w, (asx_time)(i + 1000), NULL, &h);
        ASSERT_EQ(st, ASX_OK);
    }

    /* Next registration should fail */
    st = asx_timer_register(w, 9999, NULL, &h);
    ASSERT_EQ(st, ASX_E_RESOURCE_EXHAUSTED);
}

/* -------------------------------------------------------------------
 * Test: slot recycling after cancel
 * ------------------------------------------------------------------- */

TEST(timer_slot_recycling_after_cancel) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h[ASX_MAX_TIMERS + 1];
    uint32_t i;
    asx_status st;

    asx_timer_wheel_reset(w);

    /* Fill all slots */
    for (i = 0; i < ASX_MAX_TIMERS; i++) {
        st = asx_timer_register(w, (asx_time)(i + 1000), NULL, &h[i]);
        ASSERT_EQ(st, ASX_OK);
    }

    /* Cancel one */
    ASSERT_TRUE(asx_timer_cancel(w, &h[0]));

    /* Should now be able to register one more (recycles slot 0) */
    st = asx_timer_register(w, 5000, (void *)0xDEAD, &h[ASX_MAX_TIMERS]);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_timer_active_count(w), ASX_MAX_TIMERS);
}

/* -------------------------------------------------------------------
 * Durations: a registration beyond the maximum is clamped, as Rust's
 * TimerWheel::register does (wheel.rs:515-528), not refused
 * ------------------------------------------------------------------- */

TEST(timer_beyond_max_duration_is_clamped_to_the_maximum) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);
    asx_timer_set_max_duration(w, 1000);

    /* 2000 ns out with a 1000 ns maximum: registered at 1000. */
    ASSERT_EQ(asx_timer_register(w, 2000, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);

    ASSERT_EQ(asx_timer_collect_expired(w, 999, wakers, 4), (uint32_t)0);
    ASSERT_EQ(asx_timer_collect_expired(w, 1000, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
}

/* Rust's default maximum is 7 days (TimerWheelConfig, wheel.rs:98-104). A
 * 6-day and a 7-day deadline are kept; an 8-day one is clamped to 7 days
 * and fires with the 7-day one, behind it. */
TEST(timer_default_max_duration_is_seven_days_and_clamps_beyond_it) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);
    ASSERT_EQ(ASX_TIMER_MAX_DURATION_NS, 7u * TW_DAY);

    ASSERT_EQ(asx_timer_register(w, 6u * TW_DAY, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 7u * TW_DAY, (void *)2, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 8u * TW_DAY, (void *)3, &h), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)3);

    /* Nothing before 6 days. */
    ASSERT_EQ(asx_timer_collect_expired(w, 6u * TW_DAY - 1u, wakers, 4), (uint32_t)0);
    /* The 6-day timer at 6 days. */
    ASSERT_EQ(asx_timer_collect_expired(w, 6u * TW_DAY, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
    /* Nothing between 6 and 7 days. */
    ASSERT_EQ(asx_timer_collect_expired(w, 7u * TW_DAY - 1u, wakers, 4), (uint32_t)0);
    /* The 7-day timer and the clamped 8-day timer, in registration order. */
    ASSERT_EQ(asx_timer_collect_expired(w, 7u * TW_DAY, wakers, 4), (uint32_t)2);
    ASSERT_EQ(wakers[0], (void *)2);
    ASSERT_EQ(wakers[1], (void *)3);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);
}

/* The maximum is measured from the wheel's time, whole milliseconds
 * (wheel.rs:515-522: current_time() is tick-aligned): at 1.5 ms the wheel
 * is at 1 ms, so a deadline 7 days and 0.4 ms past 1 ms is clamped to
 * 1 ms + 7 days. */
TEST(timer_clamp_measures_from_wheel_time_in_whole_milliseconds) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    const asx_time clamped = 1u * TW_MS + 7u * TW_DAY;

    asx_timer_wheel_reset(w);
    asx_timer_advance(w, 1u * TW_MS + 500000u);

    ASSERT_EQ(asx_timer_register(w, clamped + 400000u, (void *)1, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, clamped - 1u, wakers, 4), (uint32_t)0);
    ASSERT_EQ(asx_timer_collect_expired(w, clamped, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
}

/* -------------------------------------------------------------------
 * Overflow: timers beyond the wheel's range (24 h by default) wait in a
 * heap and join the wheel as time advances (refill_overflow, wheel.rs:903)
 * ------------------------------------------------------------------- */

/* Rust wheel_overflow_promotes_when_in_range (wheel.rs:1440): a timer one
 * tick past the wheel's range fires when time reaches it. */
TEST(timer_overflow_timer_promotes_and_fires) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    const asx_time far = ASX_TIMER_MAX_WHEEL_DURATION_NS + TW_MS;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, far, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_collect_expired(w, far - 1u, wakers, 4), (uint32_t)0);
    ASSERT_EQ(asx_timer_collect_expired(w, far, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
}

/* Overflow timers come out by deadline, equal deadlines by registration
 * (OverflowEntry::cmp, wheel.rs:276-294), and fire in the order they
 * reach the ready list. */
TEST(timer_overflow_timers_fire_by_deadline_then_registration) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 30u * 3600u * 1000u * TW_MS, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 25u * 3600u * 1000u * TW_MS, (void *)2, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 25u * 3600u * 1000u * TW_MS, (void *)3, &h), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 31u * 3600u * 1000u * TW_MS, wakers, 4), (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)2);
    ASSERT_EQ(wakers[1], (void *)3);
    ASSERT_EQ(wakers[2], (void *)1);
}

/* Cancelling an overflow timer removes it from the heap. */
TEST(timer_overflow_timer_can_be_cancelled) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle a, b, c;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 40u * 3600u * 1000u * TW_MS, (void *)1, &a), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 26u * 3600u * 1000u * TW_MS, (void *)2, &b), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 33u * 3600u * 1000u * TW_MS, (void *)3, &c), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &b));
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)2);

    ASSERT_EQ(asx_timer_collect_expired(w, 50u * 3600u * 1000u * TW_MS, wakers, 4), (uint32_t)2);
    ASSERT_EQ(wakers[0], (void *)3);
    ASSERT_EQ(wakers[1], (void *)1);
}

/* -------------------------------------------------------------------
 * Test: max_wakers limit respected
 * ------------------------------------------------------------------- */

TEST(timer_collect_respects_max_wakers) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[8];
    uint32_t i, count;

    asx_timer_wheel_reset(w);

    /* Register 5 timers all at same deadline */
    for (i = 0; i < 5; i++) { REGISTER_IGNORE(w, 100, (void *)(uintptr_t)(i + 1), &h); }

    /* Collect only 2 */
    count = asx_timer_collect_expired(w, 100, wakers, 2);
    ASSERT_EQ(count, (uint32_t)2);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(wakers[1], (void *)2);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)3); /* 3 remaining */

    /* The rest stay due, in order, for the next call. */
    count = asx_timer_collect_expired(w, 100, wakers, 8);
    ASSERT_EQ(count, (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)3);
    ASSERT_EQ(wakers[1], (void *)4);
    ASSERT_EQ(wakers[2], (void *)5);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);
}

/* -------------------------------------------------------------------
 * Test: zero-capacity collection still advances wheel time
 * ------------------------------------------------------------------- */

TEST(timer_collect_zero_capacity_advances_time) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);
    asx_timer_set_max_duration(w, 10u * TW_MS);

    ASSERT_EQ(asx_timer_register(w, 1u * TW_MS, (void *)1, &h), ASX_OK);
    count = asx_timer_collect_expired(w, 5u * TW_MS, wakers, 0);
    ASSERT_EQ(count, (uint32_t)0);

    /* collect() advanced the wheel to 5 ms: 14.5 ms is 9.5 ms ahead, within
     * the 10 ms maximum, so it is kept. From time 0 it would be clamped to
     * 10 ms and fire with the 1 ms timer. */
    ASSERT_EQ(asx_timer_register(w, 14u * TW_MS + 500000u, (void *)2, &h), ASX_OK);

    count = asx_timer_collect_expired(w, 10u * TW_MS, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);
}

/* -------------------------------------------------------------------
 * Test: timer update (cancel + re-register)
 * ------------------------------------------------------------------- */

TEST(timer_update_cancels_old_and_registers_new) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle old_h, new_h;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0xAA, &old_h), ASX_OK);

    /* Update to a later deadline */
    ASSERT_EQ(asx_timer_update(w, &old_h, 200, (void *)0xBB, &new_h), ASX_OK);

    /* Old timer should not fire at old deadline */
    count = asx_timer_collect_expired(w, 150, wakers, 4);
    ASSERT_EQ(count, (uint32_t)0);

    /* New timer fires at new deadline */
    count = asx_timer_collect_expired(w, 200, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xBB);

    /* Old handle should be stale */
    ASSERT_FALSE(asx_timer_cancel(w, &old_h));
}

TEST(timer_update_allows_null_old_handle) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[2];
    uint32_t count;

    asx_timer_wheel_reset(w);
    ASSERT_EQ(asx_timer_update(w, NULL, 100, (void *)0xCAFE, &h), ASX_OK);

    count = asx_timer_collect_expired(w, 100, wakers, 2);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xCAFE);
}

TEST(timer_update_on_a_full_wheel_reuses_the_slot) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle replacement;
    void *wakers[4];
    uint32_t i, count;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0xAA, &handles[0]), ASX_OK);
    for (i = 1; i < ASX_MAX_TIMERS; i++) {
        ASSERT_EQ(asx_timer_register(w, (asx_time)(1000u + i), (void *)(uintptr_t)i, &handles[i]),
                  ASX_OK);
    }

    /* As Rust (cancel, then register into the freed slab id). */
    ASSERT_EQ(asx_timer_update(w, &handles[0], 200, (void *)0xBB, &replacement), ASX_OK);
    ASSERT_EQ(replacement.slot, handles[0].slot);
    ASSERT_TRUE(replacement.generation != handles[0].generation);
    ASSERT_EQ(asx_timer_active_count(w), ASX_MAX_TIMERS);
    ASSERT_FALSE(asx_timer_cancel(w, &handles[0]));

    count = asx_timer_collect_expired(w, 100, wakers, 4);
    ASSERT_EQ(count, (uint32_t)0);
    count = asx_timer_collect_expired(w, 200, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xBB);
}

TEST(timer_update_refuses_stale_handles_and_registers_nothing) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle fired, cancelled, reused, out;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);

    /* Fired: updating it must not bring the timer back. */
    ASSERT_EQ(asx_timer_register(w, 100, (void *)0xAA, &fired), ASX_OK);
    ASSERT_EQ(asx_timer_collect_expired(w, 100, wakers, 4), (uint32_t)1);
    out.slot = 77u;
    out.generation = 77u;
    ASSERT_EQ(asx_timer_update(w, &fired, 300, (void *)0xBB, &out), ASX_E_STALE_HANDLE);
    ASSERT_EQ(out.slot, 77u);
    ASSERT_EQ(out.generation, 77u);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);

    /* Cancelled, then its slot re-armed: the new timer is unaffected. */
    ASSERT_EQ(asx_timer_register(w, 400, (void *)0xCC, &cancelled), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &cancelled));
    ASSERT_EQ(asx_timer_register(w, 500, (void *)0xDD, &reused), ASX_OK);
    ASSERT_EQ(reused.slot, cancelled.slot);
    ASSERT_EQ(asx_timer_update(w, &cancelled, 300, (void *)0xBB, &out), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);

    count = asx_timer_collect_expired(w, 450, wakers, 4);
    ASSERT_EQ(count, (uint32_t)0);
    count = asx_timer_collect_expired(w, 500, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xDD);

    /* A slot outside the arena. */
    out = reused;
    out.slot = ASX_MAX_TIMERS;
    ASSERT_EQ(asx_timer_update(w, &out, 600, (void *)0xBB, &out), ASX_E_STALE_HANDLE);
}

/* update is cancel + register (time/driver.rs:522-540), so a deadline
 * beyond the maximum is clamped as in register, and the old timer is gone. */
TEST(timer_update_clamps_a_deadline_beyond_the_maximum) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle old_h, replacement;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);
    asx_timer_set_max_duration(w, 1000u);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)0xAA, &old_h), ASX_OK);
    ASSERT_EQ(asx_timer_update(w, &old_h, 2000, (void *)0xBB, &replacement), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);

    /* The old registration is gone; the new one is clamped to 1000. */
    count = asx_timer_collect_expired(w, 999, wakers, 4);
    ASSERT_EQ(count, (uint32_t)0);
    count = asx_timer_collect_expired(w, 1000, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xBB);
}

/* A moved timer is a fresh registration: it goes behind the timers already
 * in its tick, not back to its old place. A and B at 5.2 and 5.5 ms; A is
 * moved to 5.3 ms and so fires after B. */
TEST(timer_update_moves_the_timer_behind_those_already_in_its_tick) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle a, b, a2;
    void *wakers[4];

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS + 200000u, (void *)1, &a), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 5u * TW_MS + 500000u, (void *)2, &b), ASX_OK);
    ASSERT_EQ(asx_timer_update(w, &a, 5u * TW_MS + 300000u, (void *)3, &a2), ASX_OK);

    ASSERT_EQ(asx_timer_collect_expired(w, 6u * TW_MS, wakers, 4), (uint32_t)2);
    ASSERT_EQ(wakers[0], (void *)2);
    ASSERT_EQ(wakers[1], (void *)3);
}

/* -------------------------------------------------------------------
 * Test: churn — rapid register/cancel cycles
 * ------------------------------------------------------------------- */

TEST(timer_churn_register_cancel) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    uint32_t i;

    asx_timer_wheel_reset(w);

    /* Rapid register/cancel cycle */
    for (i = 0; i < 1000; i++) {
        ASSERT_EQ(asx_timer_register(w, (asx_time)(i + 100), NULL, &h), ASX_OK);
        ASSERT_TRUE(asx_timer_cancel(w, &h));
    }

    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)0);
}

/* -------------------------------------------------------------------
 * Test: generation increments on slot reuse
 * ------------------------------------------------------------------- */

TEST(timer_generation_increments_on_reuse) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h1, h2;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, NULL, &h1), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &h1));

    ASSERT_EQ(asx_timer_register(w, 200, NULL, &h2), ASX_OK);

    /* Same slot, different generation */
    ASSERT_EQ(h1.slot, h2.slot);
    ASSERT_NE(h1.generation, h2.generation);
}

/* -------------------------------------------------------------------
 * Test: stale handle from pre-reset epoch is rejected
 * ------------------------------------------------------------------- */

TEST(timer_stale_handle_rejected_after_reset) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle old_h, new_h;

    asx_timer_wheel_reset(w);
    ASSERT_EQ(asx_timer_register(w, 100, NULL, &old_h), ASX_OK);

    /* Reset bumps per-slot generations so old handles cannot be reused. */
    asx_timer_wheel_reset(w);
    ASSERT_EQ(asx_timer_register(w, 200, NULL, &new_h), ASX_OK);

    ASSERT_EQ(old_h.slot, new_h.slot);
    ASSERT_NE(old_h.generation, new_h.generation);
    ASSERT_FALSE(asx_timer_cancel(w, &old_h));
    ASSERT_TRUE(asx_timer_cancel(w, &new_h));
}

/* -------------------------------------------------------------------
 * Test: timer at deadline 0 fires immediately
 * ------------------------------------------------------------------- */

TEST(timer_zero_deadline_fires_immediately) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 0, (void *)0xFF, &h), ASX_OK);

    count = asx_timer_collect_expired(w, 0, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)0xFF);
}

/* -------------------------------------------------------------------
 * Test: null argument rejection
 * ------------------------------------------------------------------- */

TEST(timer_null_argument_rejection) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(NULL, 100, NULL, &h), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_timer_register(w, 100, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_FALSE(asx_timer_cancel(NULL, &h));
    ASSERT_FALSE(asx_timer_cancel(w, NULL));
    ASSERT_EQ(asx_timer_active_count(NULL), (uint32_t)0);
}

/* -------------------------------------------------------------------
 * Test: advance does not fire timers
 * ------------------------------------------------------------------- */

TEST(timer_advance_does_not_fire) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, NULL, &h), ASX_OK);
    asx_timer_advance(w, 200);

    /* Timer should still be alive — advance doesn't fire */
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)1);
}

/* -------------------------------------------------------------------
 * Test: collect after advance fires correctly
 * ------------------------------------------------------------------- */

TEST(timer_collect_after_advance) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[4];
    uint32_t count;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)42, &h), ASX_OK);
    asx_timer_advance(w, 50);

    /* Advance to 50, then collect at 100 — should fire */
    count = asx_timer_collect_expired(w, 100, wakers, 4);
    ASSERT_EQ(count, (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)42);
}

/* -------------------------------------------------------------------
 * Test: double cancel returns false on second attempt
 * ------------------------------------------------------------------- */

TEST(timer_double_cancel_returns_false) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;

    asx_timer_wheel_reset(w);

    ASSERT_EQ(asx_timer_register(w, 100, NULL, &h), ASX_OK);
    ASSERT_TRUE(asx_timer_cancel(w, &h));
    ASSERT_FALSE(asx_timer_cancel(w, &h));
}

/* -------------------------------------------------------------------
 * Test: cancellation race — cancel some, fire rest
 * ------------------------------------------------------------------- */

TEST(timer_cancellation_race) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h[6];
    void *wakers[8];
    uint32_t count;

    asx_timer_wheel_reset(w);

    /* Register 6 timers */
    ASSERT_EQ(asx_timer_register(w, 100, (void *)1, &h[0]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)2, &h[1]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)3, &h[2]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)4, &h[3]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)5, &h[4]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)6, &h[5]), ASX_OK);

    /* Cancel odd-indexed timers */
    ASSERT_TRUE(asx_timer_cancel(w, &h[1]));
    ASSERT_TRUE(asx_timer_cancel(w, &h[3]));
    ASSERT_TRUE(asx_timer_cancel(w, &h[5]));

    /* Collect remaining — should be 1, 3, 5 in insertion order */
    count = asx_timer_collect_expired(w, 100, wakers, 8);
    ASSERT_EQ(count, (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(wakers[1], (void *)3);
    ASSERT_EQ(wakers[2], (void *)5);
}

/* -------------------------------------------------------------------
 * Test: large time jump fires all timers
 * ------------------------------------------------------------------- */

TEST(timer_large_time_jump) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[8];
    uint32_t count;

    asx_timer_wheel_reset(w);
    asx_timer_set_max_duration(w, 1000000);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)1, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 200, (void *)2, &h), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 500, (void *)5, &h), ASX_OK);

    /* Jump far into the future */
    count = asx_timer_collect_expired(w, 999999, wakers, 8);
    ASSERT_EQ(count, (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)1);
    ASSERT_EQ(wakers[1], (void *)2);
    ASSERT_EQ(wakers[2], (void *)5);
}

/* -------------------------------------------------------------------
 * The runtime's wheel is the same implementation, a separate instance (the
 * kernel's own): application timers never share it, and it emits no trace
 * events (the scheduler records sleeps itself)
 * ------------------------------------------------------------------- */

TEST(timer_runtime_wheel_is_separate_and_silent) {
    asx_timer_wheel *app = asx_timer_wheel_global();
    asx_timer_wheel *rt;
    asx_timer_handle h;
    void *wakers[4];

    asx_runtime_reset();
    rt = &g_kernels[0]->wheel;
    ASSERT_TRUE(app != rt);
    asx_timer_wheel_reset(app);
    asx_timer_wheel_reset(rt);
    asx_trace_reset();

    ASSERT_EQ(asx_timer_register(rt, 5u * TW_MS, (void *)7, &h), ASX_OK);
    ASSERT_EQ(asx_timer_active_count(rt), (uint32_t)1);
    ASSERT_EQ(asx_timer_active_count(app), (uint32_t)0);
    ASSERT_EQ(asx_timer_collect_expired(app, 5u * TW_MS, wakers, 4), (uint32_t)0);

    ASSERT_EQ(asx_timer_collect_expired(rt, 5u * TW_MS, wakers, 4), (uint32_t)1);
    ASSERT_EQ(wakers[0], (void *)7);
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)0);

    /* The application wheel traces its own timers. */
    ASSERT_EQ(asx_timer_register(app, 5u * TW_MS, (void *)8, &h), ASX_OK);
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)1);
}

/* -------------------------------------------------------------------
 * Churn: random registrations, cancels and advances across every level
 * and the overflow heap. No timer fires before its deadline, none is lost
 * (after a collect nothing live is due), the count tracks, and
 * next_deadline equals a scan of the live timers (Rust's own check,
 * next_deadline_matches_a_full_scan_under_random_churn, wheel.rs:1560-).
 * ------------------------------------------------------------------- */

static uint64_t churn_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static asx_time churn_span(uint64_t pick, uint64_t r) {
    /* Ready or current tick, levels 0-1, levels 1-2, levels 2-3, the edge of
     * the wheel's 24 h range, overflow, and beyond the 7-day maximum. */
    switch (pick % 7u) {
    case 0: return r % (2u * TW_MS);
    case 1: return r % (300u * TW_MS);
    case 2: return r % (70u * 1000u * TW_MS);
    case 3: return r % (5u * 3600u * 1000u * TW_MS);
    case 4: return 23u * 3600u * 1000u * TW_MS + r % (2u * 3600u * 1000u * TW_MS);
    case 5: return 25u * 3600u * 1000u * TW_MS + r % (5u * 3600u * 1000u * TW_MS);
    default: return 6u * TW_DAY + r % (3u * TW_DAY);
    }
}

TEST(timer_churn_never_fires_early_loses_none_and_next_deadline_matches_a_scan) {
    enum { LIVE = 20, OPS = 4000 };
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle handles[LIVE];
    asx_time deadlines[LIVE]; /* the deadline each timer is registered at */
    int live[LIVE];
    void *wakers[LIVE];
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    asx_time now = 0u;
    asx_time wheel_time = 0u; /* whole milliseconds of the last time the wheel saw */
    uint32_t live_count = 0u;
    uint32_t op, i;

    memset(live, 0, sizeof(live));
    memset(deadlines, 0, sizeof(deadlines));
    asx_timer_wheel_reset(w);

    for (op = 0u; op < (uint32_t)OPS; op++) {
        uint64_t r = churn_next(&state);
        uint32_t kind = (uint32_t)(r % 8u);
        asx_time next = 0u;
        asx_time scan_min = 0u;
        int have_min = 0;

        if (kind < 3u) {
            /* Register, after synchronizing as the Rust driver does. */
            uint32_t slot = LIVE;
            for (i = 0u; i < (uint32_t)LIVE; i++) {
                if (!live[i]) {
                    slot = i;
                    break;
                }
            }
            if (slot != (uint32_t)LIVE) {
                uint64_t r2 = churn_next(&state);
                asx_time requested = now + churn_span(r2, churn_next(&state));
                asx_time effective = requested;
                asx_timer_advance(w, now);
                wheel_time = (now / TW_MS) * TW_MS;
                if (requested > wheel_time && requested - wheel_time > 7u * TW_DAY) {
                    effective = wheel_time + 7u * TW_DAY;
                }
                ASSERT_EQ(asx_timer_register(w, requested, (void *)(uintptr_t)(slot + 1u),
                                             &handles[slot]),
                          ASX_OK);
                deadlines[slot] = effective;
                live[slot] = 1;
                live_count++;
            }
        } else if (kind < 5u) {
            /* Cancel a live timer. */
            uint32_t start = (uint32_t)(churn_next(&state) % (uint32_t)LIVE);
            for (i = 0u; i < (uint32_t)LIVE; i++) {
                uint32_t k = (start + i) % (uint32_t)LIVE;
                if (live[k]) {
                    ASSERT_TRUE(asx_timer_cancel(w, &handles[k]));
                    ASSERT_FALSE(asx_timer_cancel(w, &handles[k]));
                    live[k] = 0;
                    live_count--;
                    break;
                }
            }
        } else {
            /* Advance and collect. */
            uint32_t n, fired;
            now += churn_span(churn_next(&state), churn_next(&state)) / 4u + 1u;
            fired = asx_timer_collect_expired(w, now, wakers, (uint32_t)LIVE);
            wheel_time = (now / TW_MS) * TW_MS;
            for (n = 0u; n < fired; n++) {
                uint32_t k = (uint32_t)(uintptr_t)wakers[n] - 1u;
                /* It fires once, only if registered, and never early. */
                ASSERT_TRUE(k < (uint32_t)LIVE);
                ASSERT_TRUE(live[k]);
                ASSERT_TRUE(deadlines[k] <= now);
                live[k] = 0;
                live_count--;
            }
            for (i = 0u; i < (uint32_t)LIVE; i++) {
                if (live[i]) ASSERT_TRUE(deadlines[i] > now); /* nothing due is left behind */
            }
        }

        ASSERT_EQ(asx_timer_active_count(w), live_count);

        for (i = 0u; i < (uint32_t)LIVE; i++) {
            if (live[i] && (!have_min || deadlines[i] < scan_min)) {
                scan_min = deadlines[i];
                have_min = 1;
            }
        }
        if (!have_min) {
            ASSERT_EQ(asx_timer_next_deadline(w, &next), 0);
        } else {
            /* Due timers are reported at the wheel's time. */
            ASSERT_EQ(asx_timer_next_deadline(w, &next), 1);
            ASSERT_EQ(next, scan_min <= wheel_time ? wheel_time : scan_min);
        }
    }
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    fprintf(stderr, "=== test_timer_wheel ===\n");

    RUN_TEST(timer_register_and_fire);
    RUN_TEST(timer_does_not_fire_early);
    RUN_TEST(timer_cancel_prevents_fire);
    RUN_TEST(timer_trace_emits_set_fire_and_cancel);
    RUN_TEST(timer_stale_handle_cancel_returns_false);
    RUN_TEST(timer_cancel_after_fire_returns_false);
    RUN_TEST(timer_tiebreak_insertion_order);
    RUN_TEST(timer_same_tick_fires_in_registration_order);
    RUN_TEST(timer_ticks_fire_in_tick_order);
    RUN_TEST(timer_interleaved_ticks_keep_registration_order_within_each);
    RUN_TEST(timer_sub_millisecond_deadlines_fire_in_registration_order);
    RUN_TEST(timer_ready_list_keeps_registration_order_across_equal_and_earlier_deadlines);
    RUN_TEST(timer_in_current_tick_but_not_due_waits);
    RUN_TEST(timer_cascaded_from_coarser_level_follows_direct_timer_in_slot);
    RUN_TEST(timer_cascade_preserves_slot_order);
    RUN_TEST(timer_in_coarse_levels_fires_at_its_deadline);
    RUN_TEST(timer_next_deadline_none_when_empty);
    RUN_TEST(timer_next_deadline_same_tick_future_deadline_is_kept);
    RUN_TEST(timer_next_deadline_clamps_a_due_timer_to_wheel_time);
    RUN_TEST(timer_next_deadline_sees_uncascaded_higher_level_entry);
    RUN_TEST(timer_resource_exhaustion);
    RUN_TEST(timer_slot_recycling_after_cancel);
    RUN_TEST(timer_beyond_max_duration_is_clamped_to_the_maximum);
    RUN_TEST(timer_default_max_duration_is_seven_days_and_clamps_beyond_it);
    RUN_TEST(timer_clamp_measures_from_wheel_time_in_whole_milliseconds);
    RUN_TEST(timer_overflow_timer_promotes_and_fires);
    RUN_TEST(timer_overflow_timers_fire_by_deadline_then_registration);
    RUN_TEST(timer_overflow_timer_can_be_cancelled);
    RUN_TEST(timer_collect_respects_max_wakers);
    RUN_TEST(timer_collect_zero_capacity_advances_time);
    RUN_TEST(timer_update_cancels_old_and_registers_new);
    RUN_TEST(timer_update_allows_null_old_handle);
    RUN_TEST(timer_update_on_a_full_wheel_reuses_the_slot);
    RUN_TEST(timer_update_refuses_stale_handles_and_registers_nothing);
    RUN_TEST(timer_update_clamps_a_deadline_beyond_the_maximum);
    RUN_TEST(timer_update_moves_the_timer_behind_those_already_in_its_tick);
    RUN_TEST(timer_churn_register_cancel);
    RUN_TEST(timer_generation_increments_on_reuse);
    RUN_TEST(timer_stale_handle_rejected_after_reset);
    RUN_TEST(timer_zero_deadline_fires_immediately);
    RUN_TEST(timer_null_argument_rejection);
    RUN_TEST(timer_advance_does_not_fire);
    RUN_TEST(timer_collect_after_advance);
    RUN_TEST(timer_double_cancel_returns_false);
    RUN_TEST(timer_cancellation_race);
    RUN_TEST(timer_large_time_jump);
    RUN_TEST(timer_runtime_wheel_is_separate_and_silent);
    RUN_TEST(timer_churn_never_fires_early_loses_none_and_next_deadline_matches_a_scan);

    TEST_REPORT();
    return test_failures;
}
