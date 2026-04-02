/*
 * test_epoch_combinators.c — unit tests for epoch-scoped combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/epoch.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static asx_status poll_ok(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

static void setup(void) { asx_epoch_reset(); }

/* ------------------------------------------------------------------ */
/* Epoch join                                                          */
/* ------------------------------------------------------------------ */

TEST(epoch_join_advances_on_success) {
    asx_epoch_handle h;
    asx_epoch_policy policy;
    asx_combinator_poll_fn fns[2];
    uint64_t phase;

    setup();
    policy.mode = ASX_EPOCH_ADVANCE_MANUAL;
    policy.threshold = 0;
    policy.max_phases = 5;
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    phase = asx_epoch_current_phase(h);

    fns[0] = poll_ok;
    fns[1] = poll_ok;
    ASSERT_EQ(asx_epoch_join(h, fns, NULL, 2, 0), ASX_OK);
    ASSERT_EQ(asx_epoch_current_phase(h), phase + 1u);

    asx_epoch_close(h);
}

TEST(epoch_join_null_fns_fails) {
    asx_epoch_handle h;
    asx_epoch_policy policy;

    setup();
    policy.mode = ASX_EPOCH_ADVANCE_MANUAL;
    policy.threshold = 0;
    policy.max_phases = 5;
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    ASSERT_EQ(asx_epoch_join(h, NULL, NULL, 1, 0), ASX_E_INVALID_ARGUMENT);
    asx_epoch_close(h);
}

/* ------------------------------------------------------------------ */
/* Epoch race                                                          */
/* ------------------------------------------------------------------ */

TEST(epoch_race_advances_on_winner) {
    asx_epoch_handle h;
    asx_epoch_policy policy;
    asx_combinator_poll_fn fns[2];
    int32_t winner = -1;
    uint64_t phase;

    setup();
    policy.mode = ASX_EPOCH_ADVANCE_MANUAL;
    policy.threshold = 0;
    policy.max_phases = 5;
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    phase = asx_epoch_current_phase(h);

    fns[0] = poll_ok;
    fns[1] = poll_fail;
    ASSERT_EQ(asx_epoch_race(h, fns, NULL, 2, 0, &winner), ASX_OK);
    ASSERT_EQ(winner, 0);
    ASSERT_EQ(asx_epoch_current_phase(h), phase + 1u);

    asx_epoch_close(h);
}

/* ------------------------------------------------------------------ */
/* Epoch select                                                        */
/* ------------------------------------------------------------------ */

TEST(epoch_select_advances_on_winner) {
    asx_epoch_handle h;
    asx_epoch_policy policy;
    asx_combinator_poll_fn fns[2];
    int32_t winner = -1;
    uint64_t phase;

    setup();
    policy.mode = ASX_EPOCH_ADVANCE_MANUAL;
    policy.threshold = 0;
    policy.max_phases = 5;
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    phase = asx_epoch_current_phase(h);

    fns[0] = poll_ok;
    fns[1] = poll_ok;
    ASSERT_EQ(asx_epoch_select(h, fns, NULL, 2, 0, &winner), ASX_OK);
    ASSERT_TRUE(winner >= 0);
    ASSERT_EQ(asx_epoch_current_phase(h), phase + 1u);

    asx_epoch_close(h);
}

/* ------------------------------------------------------------------ */
/* Bulkhead and circuit breaker in epoch                               */
/* ------------------------------------------------------------------ */

TEST(epoch_bulkhead_call_advances) {
    asx_epoch_handle h;
    asx_epoch_policy policy;
    uint64_t phase;

    setup();
    policy.mode = ASX_EPOCH_ADVANCE_MANUAL;
    policy.threshold = 0;
    policy.max_phases = 5;
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    phase = asx_epoch_current_phase(h);

    ASSERT_EQ(asx_bulkhead_call_in_epoch(h, poll_ok, NULL, 0), ASX_OK);
    ASSERT_EQ(asx_epoch_current_phase(h), phase + 1u);

    asx_epoch_close(h);
}

TEST(epoch_circuit_breaker_call_advances) {
    asx_epoch_handle h;
    asx_epoch_policy policy;
    uint64_t phase;

    setup();
    policy.mode = ASX_EPOCH_ADVANCE_MANUAL;
    policy.threshold = 0;
    policy.max_phases = 5;
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    phase = asx_epoch_current_phase(h);

    ASSERT_EQ(asx_circuit_breaker_call_in_epoch(h, poll_ok, NULL, 0), ASX_OK);
    ASSERT_EQ(asx_epoch_current_phase(h), phase + 1u);

    asx_epoch_close(h);
}

TEST(epoch_bulkhead_null_fn_fails) {
    asx_epoch_handle h;
    asx_epoch_policy policy;

    setup();
    policy = asx_epoch_policy_infinite();
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    ASSERT_EQ(asx_bulkhead_call_in_epoch(h, NULL, NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_circuit_breaker_call_in_epoch(h, NULL, NULL, 0), ASX_E_INVALID_ARGUMENT);
    asx_epoch_close(h);
}

/* ------------------------------------------------------------------ */
/* Closed epoch rejects operations                                     */
/* ------------------------------------------------------------------ */

TEST(epoch_join_rejects_closed) {
    asx_epoch_handle h;
    asx_epoch_policy policy;
    asx_combinator_poll_fn fns[1];

    setup();
    policy = asx_epoch_policy_single();
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    asx_epoch_close(h);

    fns[0] = poll_ok;
    ASSERT_EQ(asx_epoch_join(h, fns, NULL, 1, 0), ASX_E_INVALID_STATE);
}

TEST(epoch_bulkhead_rejects_closed) {
    asx_epoch_handle h;
    asx_epoch_policy policy;

    setup();
    policy = asx_epoch_policy_single();
    ASSERT_EQ(asx_epoch_create(&policy, &h), ASX_OK);
    asx_epoch_close(h);

    ASSERT_EQ(asx_bulkhead_call_in_epoch(h, poll_ok, NULL, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_circuit_breaker_call_in_epoch(h, poll_ok, NULL, 0), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_epoch_combinators ===\n");

    RUN_TEST(epoch_join_advances_on_success);
    RUN_TEST(epoch_join_null_fns_fails);
    RUN_TEST(epoch_race_advances_on_winner);
    RUN_TEST(epoch_select_advances_on_winner);
    RUN_TEST(epoch_bulkhead_call_advances);
    RUN_TEST(epoch_circuit_breaker_call_advances);
    RUN_TEST(epoch_bulkhead_null_fn_fails);
    RUN_TEST(epoch_join_rejects_closed);
    RUN_TEST(epoch_bulkhead_rejects_closed);

    TEST_REPORT();
    return test_failures;
}
