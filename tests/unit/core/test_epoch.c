/*
 * test_epoch.c — unit tests for epoch-based execution phases
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/epoch.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while (0)

static uint64_t g_obs_old_phase;
static uint64_t g_obs_new_phase;
static int g_obs_count;

static void observer_fn(uint64_t eid, uint64_t old_p, uint64_t new_p, void *ud) {
    (void)eid; (void)ud;
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
    for (i = 0; i < ASX_MAX_EPOCHS; i++) {
        ASSERT_EQ(asx_epoch_create(&pol, &handles[i]), ASX_OK);
    }
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

    TEST_REPORT();
    return test_failures;
}
