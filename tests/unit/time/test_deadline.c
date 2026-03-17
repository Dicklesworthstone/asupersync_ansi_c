/*
 * test_deadline.c — unit tests for deadline abstraction
 *
 * Tests cover:
 *   - Deadline init and after
 *   - Arming and disarming
 *   - Expiry checks (at-time and runtime-time)
 *   - Remaining time queries
 *   - Null argument rejection
 *   - Integration with virtual time for deterministic testing
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/rt.h>
#include <asx/runtime/virtual_time.h>
#include <asx/time/deadline.h>
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
    /* Install virtual time: start=0, tick=1ms per query */
    asx_vtime_init(&g_vt, 0, 1000000ULL);
    /* Manually install vtime as the logical clock hook */
    orig = asx_runtime_get_hooks();
    memcpy(&hooks, orig, sizeof(hooks));
    hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
    hooks.clock.ctx = &g_vt;
    MUST_OK(asx_runtime_set_hooks(&hooks));
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

/* ------------------------------------------------------------------ */
/* Init tests                                                          */
/* ------------------------------------------------------------------ */

TEST(init_null_fails) { ASSERT_EQ(asx_deadline_init(NULL, 100), ASX_E_INVALID_ARGUMENT); }

TEST(init_sets_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 5000000ULL));
    ASSERT_EQ(asx_deadline_target(&dl), (asx_time)5000000ULL);
    ASSERT_FALSE(asx_deadline_is_armed(&dl));
    ASSERT_EQ(dl.expired, 0);
}

TEST(after_null_fails) { ASSERT_EQ(asx_deadline_after(NULL, 100), ASX_E_INVALID_ARGUMENT); }

TEST(after_computes_target_from_now) {
    asx_deadline dl;
    asx_time target;
    setup();
    /* after reads current time internally and adds duration */
    MUST_OK(asx_deadline_after(&dl, 10000000ULL));
    target = asx_deadline_target(&dl);
    /* Target must be >= duration (now >= 0 at time of call) */
    ASSERT_TRUE(target >= 10000000ULL);
    /* And not absurdly large */
    ASSERT_TRUE(target < 100000000ULL);
    teardown();
}

TEST(after_overflow_fails) {
    asx_deadline dl;
    setup();
    asx_vtime_init(&g_vt, UINT64_MAX - 1u, 1u);
    ASSERT_EQ(asx_deadline_after(&dl, 2u), ASX_E_TIMER_DURATION_EXCEEDED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Arm / Disarm tests                                                  */
/* ------------------------------------------------------------------ */

TEST(arm_null_fails) { ASSERT_EQ(asx_deadline_arm(NULL, NULL), ASX_E_INVALID_ARGUMENT); }

TEST(arm_registers_timer) {
    asx_deadline dl;
    setup();
    MUST_OK(asx_deadline_init(&dl, 5000000ULL));
    ASSERT_FALSE(asx_deadline_is_armed(&dl));
    MUST_OK(asx_deadline_arm(&dl, NULL));
    ASSERT_TRUE(asx_deadline_is_armed(&dl));
    ASSERT_TRUE(asx_timer_active_count(asx_timer_wheel_global()) > 0);
    teardown();
}

TEST(arm_double_arm_fails) {
    asx_deadline dl;
    setup();
    MUST_OK(asx_deadline_init(&dl, 5000000ULL));
    MUST_OK(asx_deadline_arm(&dl, NULL));
    ASSERT_EQ(asx_deadline_arm(&dl, NULL), ASX_E_INVALID_STATE);
    teardown();
}

TEST(disarm_null_fails) { ASSERT_EQ(asx_deadline_disarm(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(disarm_cancels_timer) {
    asx_deadline dl;
    uint32_t before;
    setup();
    MUST_OK(asx_deadline_init(&dl, 5000000ULL));
    MUST_OK(asx_deadline_arm(&dl, NULL));
    before = asx_timer_active_count(asx_timer_wheel_global());
    MUST_OK(asx_deadline_disarm(&dl));
    ASSERT_FALSE(asx_deadline_is_armed(&dl));
    ASSERT_EQ(asx_timer_active_count(asx_timer_wheel_global()), before - 1);
    teardown();
}

TEST(disarm_unarmed_is_noop) {
    asx_deadline dl;
    setup();
    MUST_OK(asx_deadline_init(&dl, 5000000ULL));
    MUST_OK(asx_deadline_disarm(&dl)); /* should not fail */
    teardown();
}

/* ------------------------------------------------------------------ */
/* Expiry tests                                                        */
/* ------------------------------------------------------------------ */

TEST(not_expired_before_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_FALSE(asx_deadline_is_expired_at(&dl, 5000000ULL));
}

TEST(expired_at_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_TRUE(asx_deadline_is_expired_at(&dl, 10000000ULL));
}

TEST(expired_after_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_TRUE(asx_deadline_is_expired_at(&dl, 15000000ULL));
}

TEST(expired_caches_result) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_FALSE(asx_deadline_is_expired_at(&dl, 5000000ULL));
    ASSERT_TRUE(asx_deadline_is_expired_at(&dl, 10000000ULL));
    /* Once expired, stays expired even at earlier time */
    ASSERT_TRUE(asx_deadline_is_expired_at(&dl, 1000000ULL));
}

TEST(expired_null_returns_false) {
    ASSERT_FALSE(asx_deadline_is_expired_at(NULL, 100));
    ASSERT_FALSE(asx_deadline_is_expired(NULL));
}

/* ------------------------------------------------------------------ */
/* Remaining time tests                                                */
/* ------------------------------------------------------------------ */

TEST(remaining_before_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_EQ(asx_deadline_remaining_ns(&dl, 3000000ULL), (uint64_t)7000000ULL);
}

TEST(remaining_at_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_EQ(asx_deadline_remaining_ns(&dl, 10000000ULL), (uint64_t)0);
}

TEST(remaining_after_target) {
    asx_deadline dl;
    MUST_OK(asx_deadline_init(&dl, 10000000ULL));
    ASSERT_EQ(asx_deadline_remaining_ns(&dl, 15000000ULL), (uint64_t)0);
}

TEST(remaining_null_returns_zero) { ASSERT_EQ(asx_deadline_remaining_ns(NULL, 100), (uint64_t)0); }

/* ------------------------------------------------------------------ */
/* Target query tests                                                  */
/* ------------------------------------------------------------------ */

TEST(target_null_returns_zero) { ASSERT_EQ(asx_deadline_target(NULL), (asx_time)0); }

TEST(armed_null_returns_false) { ASSERT_FALSE(asx_deadline_is_armed(NULL)); }

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_deadline ===\n");

    /* Init */
    RUN_TEST(init_null_fails);
    RUN_TEST(init_sets_target);
    RUN_TEST(after_null_fails);
    RUN_TEST(after_computes_target_from_now);
    RUN_TEST(after_overflow_fails);

    /* Arm / Disarm */
    RUN_TEST(arm_null_fails);
    RUN_TEST(arm_registers_timer);
    RUN_TEST(arm_double_arm_fails);
    RUN_TEST(disarm_null_fails);
    RUN_TEST(disarm_cancels_timer);
    RUN_TEST(disarm_unarmed_is_noop);

    /* Expiry */
    RUN_TEST(not_expired_before_target);
    RUN_TEST(expired_at_target);
    RUN_TEST(expired_after_target);
    RUN_TEST(expired_caches_result);
    RUN_TEST(expired_null_returns_false);

    /* Remaining */
    RUN_TEST(remaining_before_target);
    RUN_TEST(remaining_at_target);
    RUN_TEST(remaining_after_target);
    RUN_TEST(remaining_null_returns_zero);

    /* Queries */
    RUN_TEST(target_null_returns_zero);
    RUN_TEST(armed_null_returns_false);

    TEST_REPORT();
    return test_failures;
}
