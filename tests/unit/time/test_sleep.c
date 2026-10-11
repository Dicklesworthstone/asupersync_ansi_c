/*
 * test_sleep.c — unit tests for sleep, timeout, and interval primitives
 *
 * Uses virtual time for deterministic testing. Virtual time advances
 * 1ms per query, so a 5ms sleep completes after ~5 polls.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/rt.h>
#include <asx/runtime/virtual_time.h>
#include <asx/time/sleep.h>
#include <string.h>

/* Suppress warn_unused_result */
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static asx_runtime g_rt;
static asx_vtime_state g_vt;

static void setup(void) {
    const asx_runtime_hooks *orig;
    asx_runtime_hooks hooks;

    MUST_OK(asx_runtime_init_default(&g_rt));
    /* 1ms per query for easy reasoning */
    asx_vtime_init(&g_vt, 0, 1000000ULL);
    /* Manually install vtime as the clock: the logical clock hook, and the
     * wall clock hook a live build reads (asx_runtime_now_ns). */
    orig = asx_runtime_get_hooks();
    memcpy(&hooks, orig, sizeof(hooks));
    hooks.clock.now_ns_fn = asx_vtime_now_ns;
    hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
    hooks.clock.ctx = &g_vt;
    MUST_OK(asx_runtime_set_hooks(&hooks));
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

/* A poll function that returns PENDING n times then OK */
static int g_inner_count;
static int g_inner_limit;

static asx_status poll_n_then_ok(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    g_inner_count++;
    if (g_inner_count < g_inner_limit) return ASX_E_PENDING;
    return ASX_OK;
}

/* A poll function that never completes */
static asx_status poll_forever(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Sleep init tests                                                    */
/* ------------------------------------------------------------------ */

TEST(sleep_init_null_fails) { ASSERT_EQ(asx_sleep_init(NULL, 1000), ASX_E_INVALID_ARGUMENT); }

TEST(sleep_init_success) {
    asx_sleep_state s;
    MUST_OK(asx_sleep_init(&s, 5000000ULL));
    ASSERT_EQ(s.duration_ns, (uint64_t)5000000ULL);
    ASSERT_EQ(s.initialized, 0);
}

/* ------------------------------------------------------------------ */
/* Sleep poll tests                                                    */
/* ------------------------------------------------------------------ */

TEST(sleep_poll_null_fails) {
    ASSERT_EQ(asx_sleep_poll(NULL, ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

TEST(sleep_zero_duration_completes_immediately) {
    asx_sleep_state s;
    setup();
    MUST_OK(asx_sleep_init(&s, 0));
    ASSERT_EQ(asx_sleep_poll(&s, ASX_INVALID_ID), ASX_OK);
    teardown();
}

TEST(sleep_completes_after_duration) {
    asx_sleep_state s;
    asx_status st;
    int polls = 0;
    setup();
    /* Sleep for 5ms; vtime ticks 1ms/query */
    MUST_OK(asx_sleep_init(&s, 5000000ULL));
    do {
        st = asx_sleep_poll(&s, ASX_INVALID_ID);
        polls++;
        if (polls > 100) break; /* safety */
    } while (st == ASX_E_PENDING);
    ASSERT_EQ(st, ASX_OK);
    /* Should take a few polls (first poll sets up, subsequent check time) */
    ASSERT_TRUE(polls >= 3);
    ASSERT_TRUE(polls <= 20);
    teardown();
}

TEST(sleep_returns_pending_before_done) {
    asx_sleep_state s;
    setup();
    MUST_OK(asx_sleep_init(&s, 10000000ULL)); /* 10ms */
    /* First poll should return PENDING (sets up deadline) */
    ASSERT_EQ(asx_sleep_poll(&s, ASX_INVALID_ID), ASX_E_PENDING);
    /* Second poll: time is ~2ms, deadline is ~11ms, still pending */
    ASSERT_EQ(asx_sleep_poll(&s, ASX_INVALID_ID), ASX_E_PENDING);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Timeout init tests                                                  */
/* ------------------------------------------------------------------ */

TEST(timeout_init_null_state_fails) {
    ASSERT_EQ(asx_timeout_init(NULL, 1000, poll_forever, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(timeout_init_null_poll_fails) {
    asx_timeout_state ts;
    ASSERT_EQ(asx_timeout_init(&ts, 1000, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(timeout_init_success) {
    asx_timeout_state ts;
    MUST_OK(asx_timeout_init(&ts, 5000000ULL, poll_forever, NULL));
    ASSERT_EQ(ts.timeout_ns, (uint64_t)5000000ULL);
    ASSERT_EQ(ts.inner_poll, poll_forever);
    ASSERT_EQ(ts.initialized, 0);
    ASSERT_EQ(ts.inner_done, 0);
}

/* ------------------------------------------------------------------ */
/* Timeout poll tests                                                  */
/* ------------------------------------------------------------------ */

TEST(timeout_poll_null_fails) {
    ASSERT_EQ(asx_timeout_poll(NULL, ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

TEST(timeout_inner_completes_before_deadline) {
    asx_timeout_state ts;
    asx_status st;
    int polls = 0;
    setup();
    g_inner_count = 0;
    g_inner_limit = 2; /* inner completes after 2 polls */
    MUST_OK(asx_timeout_init(&ts, 50000000ULL, poll_n_then_ok, NULL)); /* 50ms timeout */
    do {
        st = asx_timeout_poll(&ts, ASX_INVALID_ID);
        polls++;
        if (polls > 100) break;
    } while (st == ASX_E_PENDING);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(polls <= 10);
    teardown();
}

TEST(timeout_expires_before_inner) {
    asx_timeout_state ts;
    asx_status st;
    int polls = 0;
    setup();
    /* Timeout is 3ms, inner never completes */
    MUST_OK(asx_timeout_init(&ts, 3000000ULL, poll_forever, NULL));
    do {
        st = asx_timeout_poll(&ts, ASX_INVALID_ID);
        polls++;
        if (polls > 100) break;
    } while (st == ASX_E_PENDING);
    ASSERT_EQ(st, ASX_E_TIMED_OUT);
    teardown();
}

/* The clock at `now_ns`, fixed: each query returns it until moved. */
static void clock_at(asx_time now_ns) { asx_vtime_init(&g_vt, now_ns, 0u); }

/* Rust's TimeoutFuture (time/timeout_future.rs:286-315): completed work
 * wins at the deadline exactly; the inner function is polled first. */
TEST(timeout_at_the_deadline_prefers_completed_work) {
    asx_timeout_state ts;
    setup();
    clock_at(0);
    g_inner_count = 0;
    g_inner_limit = 2;
    MUST_OK(asx_timeout_init(&ts, 5000000ULL, poll_n_then_ok, NULL)); /* deadline 5 ms */
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_E_PENDING);
    clock_at(5000000ULL);
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(g_inner_count, 2);
    ASSERT_EQ(ts.inner_done, 1);
    teardown();
}

TEST(timeout_at_the_deadline_with_pending_work_times_out) {
    asx_timeout_state ts;
    setup();
    clock_at(0);
    g_inner_count = 0;
    g_inner_limit = 100;
    MUST_OK(asx_timeout_init(&ts, 5000000ULL, poll_n_then_ok, NULL));
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_E_PENDING);
    clock_at(5000000ULL);
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_E_TIMED_OUT);
    ASSERT_EQ(g_inner_count, 2); /* polled once more, at the deadline */
    teardown();
}

TEST(timeout_past_the_deadline_does_not_poll_the_inner) {
    asx_timeout_state ts;
    setup();
    clock_at(0);
    g_inner_count = 0;
    g_inner_limit = 2;
    MUST_OK(asx_timeout_init(&ts, 5000000ULL, poll_n_then_ok, NULL));
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_E_PENDING);
    clock_at(5000001ULL);
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_E_TIMED_OUT);
    ASSERT_EQ(g_inner_count, 1); /* the inner that would complete is not polled */
    teardown();
}

TEST(timeout_repoll_after_completion_fails_closed) {
    asx_timeout_state ts;
    setup();
    clock_at(0);
    g_inner_count = 0;
    g_inner_limit = 1;
    MUST_OK(asx_timeout_init(&ts, 5000000ULL, poll_n_then_ok, NULL));
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_timeout_poll(&ts, ASX_INVALID_ID), ASX_E_TIMED_OUT);
    ASSERT_EQ(g_inner_count, 1);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Interval init tests                                                 */
/* ------------------------------------------------------------------ */

TEST(interval_init_null_fails) {
    ASSERT_EQ(asx_interval_init(NULL, 1000, 5), ASX_E_INVALID_ARGUMENT);
}

TEST(interval_init_zero_period_fails) {
    asx_interval_state is;
    ASSERT_EQ(asx_interval_init(&is, 0, 5), ASX_E_INVALID_ARGUMENT);
}

TEST(interval_init_success) {
    asx_interval_state is;
    MUST_OK(asx_interval_init(&is, 2000000ULL, 3));
    ASSERT_EQ(is.period_ns, (uint64_t)2000000ULL);
    ASSERT_EQ(is.max_ticks, 3u);
    ASSERT_EQ(is.ticks, 0u);
}

/* ------------------------------------------------------------------ */
/* Interval poll tests                                                 */
/* ------------------------------------------------------------------ */

TEST(interval_ticks_null_returns_zero) { ASSERT_EQ(asx_interval_ticks(NULL), 0u); }

TEST(interval_poll_null_fails) {
    ASSERT_EQ(asx_interval_poll(NULL, ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

/* Rust interval(now, period) (time/interval.rs:443): the first tick is at
 * the start, then one every period: ticks at 0, 2 and 4 ms. */
TEST(interval_first_tick_is_at_the_first_poll) {
    asx_interval_state is;
    setup();
    clock_at(0);
    MUST_OK(asx_interval_init(&is, 2000000ULL, 3));
    ASSERT_EQ((int)is.missed_tick_behavior, (int)ASX_MISSED_TICK_BURST);
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 1u);
    clock_at(1999999ULL);
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 1u);
    clock_at(2000000ULL);
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 2u);
    clock_at(4000000ULL);
    ASSERT_EQ(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 3u);
    /* A completed interval stays completed. */
    clock_at(9000000ULL);
    ASSERT_EQ(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 3u);
    teardown();
}

/* Rust interval_at(start, period): the first tick is at `start`. */
TEST(interval_at_starts_at_the_given_time) {
    asx_interval_state is;
    setup();
    clock_at(1000000ULL);
    MUST_OK(asx_interval_init_at(&is, 5000000ULL, 2000000ULL, 2));
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 0u);
    clock_at(5000000ULL);
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 1u);
    clock_at(7000000ULL);
    ASSERT_EQ(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 2u);
    ASSERT_EQ(asx_interval_init_at(NULL, 0u, 1u, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_interval_init_at(&is, 0u, 0u, 1u), ASX_E_INVALID_ARGUMENT);
    teardown();
}

/* MissedTickBehavior (time/interval.rs:385-420), ticks at 0 then polled at
 * 7 ms with a 2 ms period. Burst (the default) fires the ticks due at 2, 4
 * and 6 ms at once and keeps its schedule (next at 8 ms); Delay fires one
 * and restarts the period at 7 ms (next at 9 ms); Skip fires one and moves
 * to the next boundary of the original schedule (next at 8 ms). */
TEST(interval_missed_ticks_follow_the_behavior) {
    asx_interval_state burst;
    asx_interval_state delay;
    asx_interval_state skip;
    setup();
    clock_at(0);
    MUST_OK(asx_interval_init(&burst, 2000000ULL, 0));
    MUST_OK(asx_interval_init(&delay, 2000000ULL, 0));
    MUST_OK(asx_interval_init(&skip, 2000000ULL, 0));
    ASSERT_EQ(asx_interval_set_missed_tick_behavior(&delay, ASX_MISSED_TICK_DELAY), ASX_OK);
    ASSERT_EQ(asx_interval_set_missed_tick_behavior(&skip, ASX_MISSED_TICK_SKIP), ASX_OK);
    (void)asx_interval_poll(&burst, ASX_INVALID_ID);
    (void)asx_interval_poll(&delay, ASX_INVALID_ID);
    (void)asx_interval_poll(&skip, ASX_INVALID_ID);

    clock_at(7000000ULL);
    (void)asx_interval_poll(&burst, ASX_INVALID_ID);
    (void)asx_interval_poll(&delay, ASX_INVALID_ID);
    (void)asx_interval_poll(&skip, ASX_INVALID_ID);
    ASSERT_EQ(asx_interval_ticks(&burst), 4u);
    ASSERT_EQ(asx_interval_ticks(&delay), 2u);
    ASSERT_EQ(asx_interval_ticks(&skip), 2u);
    ASSERT_EQ(asx_deadline_target(&burst.deadline), (asx_time)8000000ULL);
    ASSERT_EQ(asx_deadline_target(&delay.deadline), (asx_time)9000000ULL);
    ASSERT_EQ(asx_deadline_target(&skip.deadline), (asx_time)8000000ULL);

    clock_at(8000000ULL);
    (void)asx_interval_poll(&burst, ASX_INVALID_ID);
    (void)asx_interval_poll(&delay, ASX_INVALID_ID);
    (void)asx_interval_poll(&skip, ASX_INVALID_ID);
    ASSERT_EQ(asx_interval_ticks(&burst), 5u);
    ASSERT_EQ(asx_interval_ticks(&delay), 2u);
    ASSERT_EQ(asx_interval_ticks(&skip), 3u);

    ASSERT_EQ(asx_interval_set_missed_tick_behavior(NULL, ASX_MISSED_TICK_SKIP),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_interval_set_missed_tick_behavior(&skip, (asx_missed_tick_behavior)3),
              ASX_E_INVALID_ARGUMENT);
    teardown();
}

/* A burst never counts past max_ticks. */
TEST(interval_burst_stops_at_max_ticks) {
    asx_interval_state is;
    setup();
    clock_at(0);
    MUST_OK(asx_interval_init(&is, 2000000ULL, 3));
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    clock_at(100000000ULL);
    ASSERT_EQ(asx_interval_poll(&is, ASX_INVALID_ID), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 3u);
    teardown();
}

/* Rust saturates the deadline at Time::MAX, fires the tick there and then
 * stays silent (Interval::advance_deadline, `exhausted`); C then reports
 * ASX_E_TIMER_DURATION_EXCEEDED rather than waiting forever. */
TEST(interval_saturates_and_ends_after_the_last_tick) {
    asx_interval_state is;
    setup();
    clock_at(UINT64_MAX - 3u);
    MUST_OK(asx_interval_init(&is, 2u, 0u));
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_E_TIMER_DURATION_EXCEEDED);
    ASSERT_EQ(asx_interval_ticks(&is), 1u);
    clock_at(UINT64_MAX - 1u);
    ASSERT_NE(asx_interval_poll(&is, ASX_INVALID_ID), ASX_E_TIMER_DURATION_EXCEEDED);
    ASSERT_EQ(asx_interval_ticks(&is), 2u);
    ASSERT_EQ(asx_deadline_target(&is.deadline), (asx_time)UINT64_MAX);
    clock_at(UINT64_MAX);
    ASSERT_EQ(asx_interval_poll(&is, ASX_INVALID_ID), ASX_E_TIMER_DURATION_EXCEEDED);
    ASSERT_EQ(asx_interval_ticks(&is), 3u);
    ASSERT_EQ(asx_interval_poll(&is, ASX_INVALID_ID), ASX_E_TIMER_DURATION_EXCEEDED);
    ASSERT_EQ(asx_interval_ticks(&is), 3u);
    teardown();
}

TEST(interval_completes_after_max_ticks) {
    asx_interval_state is;
    asx_status st;
    int polls = 0;
    setup();
    /* 2ms period, 3 ticks max */
    MUST_OK(asx_interval_init(&is, 2000000ULL, 3));
    do {
        st = asx_interval_poll(&is, ASX_INVALID_ID);
        polls++;
        if (polls > 200) break;
    } while (st == ASX_E_PENDING);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&is), 3u);
    teardown();
}

TEST(interval_ticks_increment) {
    asx_interval_state is;
    asx_status st;
    int polls;
    setup();
    /* 2ms period, 5 max ticks */
    MUST_OK(asx_interval_init(&is, 2000000ULL, 5));
    /* Poll enough to get at least one tick */
    for (polls = 0; polls < 10; polls++) {
        st = asx_interval_poll(&is, ASX_INVALID_ID);
        (void)st;
    }
    ASSERT_TRUE(asx_interval_ticks(&is) >= 1);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_sleep ===\n");

    /* Sleep init */
    RUN_TEST(sleep_init_null_fails);
    RUN_TEST(sleep_init_success);

    /* Sleep poll */
    RUN_TEST(sleep_poll_null_fails);
    RUN_TEST(sleep_zero_duration_completes_immediately);
    RUN_TEST(sleep_completes_after_duration);
    RUN_TEST(sleep_returns_pending_before_done);

    /* Timeout init */
    RUN_TEST(timeout_init_null_state_fails);
    RUN_TEST(timeout_init_null_poll_fails);
    RUN_TEST(timeout_init_success);

    /* Timeout poll */
    RUN_TEST(timeout_poll_null_fails);
    RUN_TEST(timeout_inner_completes_before_deadline);
    RUN_TEST(timeout_expires_before_inner);
    RUN_TEST(timeout_at_the_deadline_prefers_completed_work);
    RUN_TEST(timeout_at_the_deadline_with_pending_work_times_out);
    RUN_TEST(timeout_past_the_deadline_does_not_poll_the_inner);
    RUN_TEST(timeout_repoll_after_completion_fails_closed);

    /* Interval init */
    RUN_TEST(interval_init_null_fails);
    RUN_TEST(interval_init_zero_period_fails);
    RUN_TEST(interval_init_success);

    /* Interval poll */
    RUN_TEST(interval_ticks_null_returns_zero);
    RUN_TEST(interval_poll_null_fails);
    RUN_TEST(interval_first_tick_is_at_the_first_poll);
    RUN_TEST(interval_at_starts_at_the_given_time);
    RUN_TEST(interval_missed_ticks_follow_the_behavior);
    RUN_TEST(interval_burst_stops_at_max_ticks);
    RUN_TEST(interval_saturates_and_ends_after_the_last_tick);
    RUN_TEST(interval_completes_after_max_ticks);
    RUN_TEST(interval_ticks_increment);

    TEST_REPORT();
    return test_failures;
}
