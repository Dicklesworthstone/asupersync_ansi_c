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
    /* Manually install vtime as the logical clock hook */
    orig = asx_runtime_get_hooks();
    memcpy(&hooks, orig, sizeof(hooks));
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

    /* Interval init */
    RUN_TEST(interval_init_null_fails);
    RUN_TEST(interval_init_zero_period_fails);
    RUN_TEST(interval_init_success);

    /* Interval poll */
    RUN_TEST(interval_ticks_null_returns_zero);
    RUN_TEST(interval_poll_null_fails);
    RUN_TEST(interval_completes_after_max_ticks);
    RUN_TEST(interval_ticks_increment);

    TEST_REPORT();
    return test_failures;
}
