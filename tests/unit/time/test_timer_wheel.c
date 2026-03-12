/*
 * test_timer_wheel.c — timer wheel unit tests (bd-2cw.4)
 *
 * Tests timer registration, firing, deterministic ordering, O(1) cancel,
 * generation validation, resource exhaustion, and churn scenarios.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define REGISTER_IGNORE(w, dl, data, h) \
    do { asx_status s_ = asx_timer_register((w), (dl), (data), (h)); (void)s_; } while (0)

static uint64_t timer_trace_entity_id(const asx_timer_handle *handle)
{
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
 * Test: mixed deadlines fire in correct order
 * ------------------------------------------------------------------- */

TEST(timer_mixed_deadline_ordering) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h1, h2, h3;
    void *wakers[8];
    uint32_t count;

    asx_timer_wheel_reset(w);

    /* Register timers out of deadline order */
    ASSERT_EQ(asx_timer_register(w, 300, (void *)3, &h1), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)1, &h2), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 200, (void *)2, &h3), ASX_OK);

    /* Collect all at once */
    count = asx_timer_collect_expired(w, 300, wakers, 8);
    ASSERT_EQ(count, (uint32_t)3);
    ASSERT_EQ(wakers[0], (void *)1);  /* deadline 100 first */
    ASSERT_EQ(wakers[1], (void *)2);  /* deadline 200 second */
    ASSERT_EQ(wakers[2], (void *)3);  /* deadline 300 third */
}

/* -------------------------------------------------------------------
 * Test: deadline + insertion_seq tie-break
 * ------------------------------------------------------------------- */

TEST(timer_deadline_plus_seq_tiebreak) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h[5];
    void *wakers[8];
    uint32_t count;

    asx_timer_wheel_reset(w);

    /* Same deadline (100): a, b, c */
    ASSERT_EQ(asx_timer_register(w, 100, (void *)10, &h[0]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)20, &h[1]), ASX_OK);
    ASSERT_EQ(asx_timer_register(w, 100, (void *)30, &h[2]), ASX_OK);
    /* Earlier deadline (50): d */
    ASSERT_EQ(asx_timer_register(w, 50, (void *)5, &h[3]), ASX_OK);
    /* Same as first group (100): e */
    ASSERT_EQ(asx_timer_register(w, 100, (void *)40, &h[4]), ASX_OK);

    count = asx_timer_collect_expired(w, 100, wakers, 8);
    ASSERT_EQ(count, (uint32_t)5);
    /* Expected order: deadline 50 first, then deadline 100 in insertion order */
    ASSERT_EQ(wakers[0], (void *)5);   /* deadline 50 */
    ASSERT_EQ(wakers[1], (void *)10);  /* deadline 100, seq 0 */
    ASSERT_EQ(wakers[2], (void *)20);  /* deadline 100, seq 1 */
    ASSERT_EQ(wakers[3], (void *)30);  /* deadline 100, seq 2 */
    ASSERT_EQ(wakers[4], (void *)40);  /* deadline 100, seq 4 */
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
 * Test: duration exceeded
 * ------------------------------------------------------------------- */

TEST(timer_duration_exceeded) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;

    asx_timer_wheel_reset(w);

    /* Set a short max duration for testing */
    asx_timer_set_max_duration(w, 1000);

    /* Try to register timer with duration > max */
    ASSERT_EQ(asx_timer_register(w, 2000, NULL, &h),
              ASX_E_TIMER_DURATION_EXCEEDED);

    /* Timer within max should succeed */
    ASSERT_EQ(asx_timer_register(w, 500, NULL, &h), ASX_OK);
}

/* -------------------------------------------------------------------
 * Test: max_wakers limit respected
 * ------------------------------------------------------------------- */

TEST(timer_collect_respects_max_wakers) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[2];
    uint32_t i, count;

    asx_timer_wheel_reset(w);

    /* Register 5 timers all at same deadline */
    for (i = 0; i < 5; i++) {
        REGISTER_IGNORE(w, 100, (void *)(uintptr_t)(i + 1), &h);
    }

    /* Collect only 2 */
    count = asx_timer_collect_expired(w, 100, wakers, 2);
    ASSERT_EQ(count, (uint32_t)2);
    ASSERT_EQ(asx_timer_active_count(w), (uint32_t)3); /* 3 remaining */
}

/* -------------------------------------------------------------------
 * Test: zero-capacity collection still advances wheel time
 * ------------------------------------------------------------------- */

TEST(timer_collect_zero_capacity_advances_time) {
    asx_timer_wheel *w = asx_timer_wheel_global();
    asx_timer_handle h;
    void *wakers[1];
    uint32_t count;

    asx_timer_wheel_reset(w);
    asx_timer_set_max_duration(w, 1000);

    ASSERT_EQ(asx_timer_register(w, 100, (void *)1, &h), ASX_OK);
    count = asx_timer_collect_expired(w, 100, wakers, 0);
    ASSERT_EQ(count, (uint32_t)0);

    /* If collect() advanced current_time to 100, delta is 950 and should pass. */
    ASSERT_EQ(asx_timer_register(w, 1050, (void *)2, &h), ASX_OK);
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
    RUN_TEST(timer_mixed_deadline_ordering);
    RUN_TEST(timer_deadline_plus_seq_tiebreak);
    RUN_TEST(timer_resource_exhaustion);
    RUN_TEST(timer_slot_recycling_after_cancel);
    RUN_TEST(timer_duration_exceeded);
    RUN_TEST(timer_collect_respects_max_wakers);
    RUN_TEST(timer_collect_zero_capacity_advances_time);
    RUN_TEST(timer_update_cancels_old_and_registers_new);
    RUN_TEST(timer_update_allows_null_old_handle);
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

    TEST_REPORT();
    return test_failures;
}
