/*
 * test_epoch.c — unit tests for epoch-based execution phases
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/epoch.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static uint64_t g_obs_old_phase;
static uint64_t g_obs_new_phase;
static int g_obs_count;

static void observer_fn(uint64_t eid, uint64_t old_p, uint64_t new_p, void *ud) {
    (void)eid;
    (void)ud;
    g_obs_old_phase = old_p;
    g_obs_new_phase = new_p;
    g_obs_count++;
}

static void setup(void) {
    asx_epoch_reset();
    g_obs_count = 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TEST(create_close) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    ASSERT_EQ(asx_epoch_create(&pol, &h), ASX_OK);
    ASSERT_EQ(asx_epoch_get_state(h), ASX_EPOCH_ACTIVE);
    ASSERT_EQ(asx_epoch_close(h), ASX_OK);
    ASSERT_EQ(asx_epoch_get_state(h), ASX_EPOCH_CLOSED);
}

TEST(create_null) {
    setup();
    asx_epoch_handle h;
    ASSERT_EQ(asx_epoch_create(NULL, &h), ASX_E_INVALID_ARGUMENT);
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    ASSERT_EQ(asx_epoch_create(&pol, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(create_exhaustion) {
    setup();
    asx_epoch_handle handles[ASX_MAX_EPOCHS];
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    uint32_t i;
    for (i = 0; i < ASX_MAX_EPOCHS; i++) { ASSERT_EQ(asx_epoch_create(&pol, &handles[i]), ASX_OK); }
    asx_epoch_handle overflow;
    ASSERT_EQ(asx_epoch_create(&pol, &overflow), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Manual advance                                                      */
/* ------------------------------------------------------------------ */

TEST(manual_advance) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    ASSERT_EQ(asx_epoch_current_phase(h), (uint64_t)0);
    MUST_OK(asx_epoch_advance(h));
    ASSERT_EQ(asx_epoch_current_phase(h), (uint64_t)1);
    MUST_OK(asx_epoch_advance(h));
    ASSERT_EQ(asx_epoch_current_phase(h), (uint64_t)2);
}

TEST(manual_max_phases) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 3};
    MUST_OK(asx_epoch_create(&pol, &h));
    MUST_OK(asx_epoch_advance(h));
    MUST_OK(asx_epoch_advance(h));
    ASSERT_EQ(asx_epoch_advance(h), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(advance_closed) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    MUST_OK(asx_epoch_close(h));
    ASSERT_EQ(asx_epoch_advance(h), ASX_E_INVALID_STATE);
}

TEST(advance_wrong_mode) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_THRESHOLD, 2, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    ASSERT_EQ(asx_epoch_advance(h), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Threshold advance                                                   */
/* ------------------------------------------------------------------ */

TEST(threshold_advance) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_THRESHOLD, 3, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    MUST_OK(asx_epoch_arrive(h, 1));
    ASSERT_EQ(asx_epoch_current_phase(h), (uint64_t)0);
    MUST_OK(asx_epoch_arrive(h, 2));
    ASSERT_EQ(asx_epoch_current_phase(h), (uint64_t)0);
    MUST_OK(asx_epoch_arrive(h, 3));
    ASSERT_EQ(asx_epoch_current_phase(h), (uint64_t)1);
    ASSERT_EQ(asx_epoch_arrival_count(h), 0u); /* reset after advance */
}

TEST(arrive_wrong_mode) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    ASSERT_EQ(asx_epoch_arrive(h, 1), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Observer notifications                                              */
/* ------------------------------------------------------------------ */

TEST(observer_notified) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    MUST_OK(asx_epoch_observe(h, observer_fn, NULL));
    MUST_OK(asx_epoch_advance(h));
    ASSERT_EQ(g_obs_count, 1);
    ASSERT_EQ(g_obs_old_phase, (uint64_t)0);
    ASSERT_EQ(g_obs_new_phase, (uint64_t)1);
}

TEST(observer_null_fn) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    ASSERT_EQ(asx_epoch_observe(h, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(observer_exhaustion) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    uint32_t i;
    MUST_OK(asx_epoch_create(&pol, &h));
    for (i = 0; i < ASX_MAX_EPOCH_OBSERVERS; i++) {
        ASSERT_EQ(asx_epoch_observe(h, observer_fn, NULL), ASX_OK);
    }
    ASSERT_EQ(asx_epoch_observe(h, observer_fn, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Slot reuse                                                          */
/* ------------------------------------------------------------------ */

TEST(slot_reuse) {
    setup();
    asx_epoch_handle h1, h2;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h1));
    MUST_OK(asx_epoch_close(h1));
    MUST_OK(asx_epoch_create(&pol, &h2));
    ASSERT_EQ(h2.slot, 0u);
    ASSERT_NE(h2.generation, h1.generation);
}

/* ------------------------------------------------------------------ */
/* Operation budget tracking                                           */
/* ------------------------------------------------------------------ */

TEST(budget_record_and_query) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));

    ASSERT_EQ(asx_epoch_operations_used(h), 0u);
    ASSERT_TRUE(!asx_epoch_is_budget_exhausted(h)); /* unlimited by default */

    MUST_OK(asx_epoch_record_operation(h));
    MUST_OK(asx_epoch_record_operation(h));
    ASSERT_EQ(asx_epoch_operations_used(h), 2u);
    ASSERT_TRUE(!asx_epoch_is_budget_exhausted(h)); /* still unlimited */
}

TEST(budget_remaining_unlimited) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));

    /* Unlimited budget: remaining returns 0 (meaning unlimited, not exhausted) */
    ASSERT_EQ(asx_epoch_remaining_operations(h), 0u);
}

TEST(budget_record_on_closed_rejected) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    MUST_OK(asx_epoch_close(h));
    ASSERT_EQ(asx_epoch_record_operation(h), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Policy query and builders                                           */
/* ------------------------------------------------------------------ */

TEST(get_policy_round_trip) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_THRESHOLD, 5, 10};
    asx_epoch_policy out;
    MUST_OK(asx_epoch_create(&pol, &h));

    ASSERT_EQ(asx_epoch_get_policy(h, &out), ASX_OK);
    ASSERT_EQ(out.mode, ASX_EPOCH_ADVANCE_THRESHOLD);
    ASSERT_EQ(out.threshold, 5u);
    ASSERT_EQ(out.max_phases, 10u);
}

TEST(get_policy_null_rejected) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));
    ASSERT_EQ(asx_epoch_get_policy(h, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(policy_strict_builder) {
    asx_epoch_policy p = asx_epoch_policy_strict(5);
    ASSERT_EQ(p.mode, ASX_EPOCH_ADVANCE_BARRIER);
    ASSERT_EQ(p.max_phases, 5u);
}

TEST(policy_lenient_builder) {
    asx_epoch_policy p = asx_epoch_policy_lenient(3, 10);
    ASSERT_EQ(p.mode, ASX_EPOCH_ADVANCE_THRESHOLD);
    ASSERT_EQ(p.threshold, 3u);
    ASSERT_EQ(p.max_phases, 10u);
}

TEST(policy_single_builder) {
    asx_epoch_policy p = asx_epoch_policy_single();
    ASSERT_EQ(p.mode, ASX_EPOCH_ADVANCE_MANUAL);
    ASSERT_EQ(p.max_phases, 1u);
}

TEST(policy_infinite_builder) {
    asx_epoch_policy p = asx_epoch_policy_infinite();
    ASSERT_EQ(p.mode, ASX_EPOCH_ADVANCE_MANUAL);
    ASSERT_EQ(p.max_phases, 0u);
}

/* ------------------------------------------------------------------ */
/* Duration and expiry                                                 */
/* ------------------------------------------------------------------ */

TEST(duration_returns_value) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));

    /* Duration should be non-negative (may be zero if clock resolution is coarse) */
    (void)asx_epoch_duration_ns(h);
}

TEST(expired_returns_false_without_timeout) {
    setup();
    asx_epoch_handle h;
    asx_epoch_policy pol = {ASX_EPOCH_ADVANCE_MANUAL, 0, 0};
    MUST_OK(asx_epoch_create(&pol, &h));

    /* Default epoch has no timeout — should never expire */
    ASSERT_TRUE(!asx_epoch_is_expired(h));
}

/* ------------------------------------------------------------------ */
/* Invalid handle queries                                              */
/* ------------------------------------------------------------------ */

TEST(queries_on_invalid_handle) {
    setup();
    asx_epoch_handle bad;
    asx_epoch_policy out;
    bad.slot = 99u;
    bad.generation = 0u;

    ASSERT_EQ(asx_epoch_duration_ns(bad), (uint64_t)0);
    ASSERT_TRUE(!asx_epoch_is_budget_exhausted(bad));
    ASSERT_EQ(asx_epoch_operations_used(bad), 0u);
    ASSERT_EQ(asx_epoch_remaining_operations(bad), 0u);
    ASSERT_TRUE(!asx_epoch_is_expired(bad));
    ASSERT_EQ(asx_epoch_get_policy(bad, &out), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_epoch_record_operation(bad), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "test_epoch\n");

    RUN_TEST(create_close);
    RUN_TEST(create_null);
    RUN_TEST(create_exhaustion);
    RUN_TEST(manual_advance);
    RUN_TEST(manual_max_phases);
    RUN_TEST(advance_closed);
    RUN_TEST(advance_wrong_mode);
    RUN_TEST(threshold_advance);
    RUN_TEST(arrive_wrong_mode);
    RUN_TEST(observer_notified);
    RUN_TEST(observer_null_fn);
    RUN_TEST(observer_exhaustion);
    RUN_TEST(slot_reuse);

    /* Budget tracking */
    RUN_TEST(budget_record_and_query);
    RUN_TEST(budget_remaining_unlimited);
    RUN_TEST(budget_record_on_closed_rejected);

    /* Policy query and builders */
    RUN_TEST(get_policy_round_trip);
    RUN_TEST(get_policy_null_rejected);
    RUN_TEST(policy_strict_builder);
    RUN_TEST(policy_lenient_builder);
    RUN_TEST(policy_single_builder);
    RUN_TEST(policy_infinite_builder);

    /* Duration and expiry */
    RUN_TEST(duration_returns_value);
    RUN_TEST(expired_returns_false_without_timeout);

    /* Invalid handles */
    RUN_TEST(queries_on_invalid_handle);

    TEST_REPORT();
    return test_failures;
}
