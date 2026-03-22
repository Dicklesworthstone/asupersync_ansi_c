/*
 * test_join_set.c — unit tests for JoinSet dynamic task collection
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/join_set.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>

static asx_runtime g_rt;

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static void setup(void) {
    asx_status st = asx_runtime_init_default(&g_rt);
    (void)st;
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

TEST(join_set_init) {
    asx_join_set js;
    asx_join_set_init(&js);
    ASSERT_EQ(asx_join_set_total_count(&js), 0u);
    ASSERT_EQ(asx_join_set_active_count(&js), 0u);
    ASSERT_TRUE(asx_join_set_is_empty(&js));
}

TEST(join_set_add_and_count) {
    asx_join_set js;
    asx_region_id region;
    asx_task_id t1, t2;

    setup();
    asx_join_set_init(&js);
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, noop_poll, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, noop_poll, NULL, &t2), ASX_OK);

    ASSERT_EQ(asx_join_set_add(&js, t1), ASX_OK);
    ASSERT_EQ(asx_join_set_add(&js, t2), ASX_OK);
    ASSERT_EQ(asx_join_set_total_count(&js), 2u);
    ASSERT_EQ(asx_join_set_active_count(&js), 2u);
    ASSERT_FALSE(asx_join_set_is_empty(&js));

    teardown();
}

TEST(join_set_poll_completed) {
    asx_join_set js;
    asx_join_set_result result;
    asx_region_id region;
    asx_task_id tid;
    asx_budget budget;

    setup();
    asx_join_set_init(&js);
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, noop_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_join_set_add(&js, tid), ASX_OK);

    /* Run scheduler to complete the task */
    budget = asx_budget_from_polls(10u);
    ASSERT_EQ(asx_scheduler_run(region, &budget), ASX_OK);

    /* Now poll_next should find the completed task */
    ASSERT_EQ(asx_join_set_poll_next(&js, &result), ASX_OK);
    ASSERT_EQ(result.task_id, tid);
    ASSERT_EQ(result.outcome.severity, ASX_OUTCOME_OK);

    /* No more tasks */
    ASSERT_TRUE(asx_join_set_is_empty(&js));
    ASSERT_EQ(asx_join_set_poll_next(&js, &result), ASX_E_NOT_FOUND);

    teardown();
}

TEST(join_set_null_args) {
    asx_join_set js;
    asx_join_set_result result;

    asx_join_set_init(&js);
    ASSERT_EQ(asx_join_set_add(NULL, 1), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_join_set_add(&js, ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_join_set_poll_next(NULL, &result), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_join_set_poll_next(&js, NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== join_set tests ===\n");
    RUN_TEST(join_set_init);
    RUN_TEST(join_set_add_and_count);
    RUN_TEST(join_set_poll_completed);
    RUN_TEST(join_set_null_args);
    TEST_REPORT();
    return test_failures;
}
