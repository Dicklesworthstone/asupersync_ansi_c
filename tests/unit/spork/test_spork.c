/*
 * test_spork.c — unit tests for spawn/fork orchestration primitives
 *
 * Tests work items, work pools, parallel map, scatter-gather, and pipelines.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/spork/spork.h>
#include <string.h>

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static asx_status work_double(void *user_data, void *result) {
    int val = *(int *)user_data;
    *(int *)result = val * 2;
    return ASX_OK;
}

static asx_status work_fail(void *user_data, void *result) {
    (void)user_data;
    (void)result;
    return ASX_E_DISCONNECTED;
}

static asx_status map_triple(const void *input, void *output, void *user_data) {
    (void)user_data;
    *(int *)output = *(const int *)input * 3;
    return ASX_OK;
}

static asx_status stage_add_ten(void *input, void *output, void *user_data) {
    (void)user_data;
    *(int *)output = *(int *)input + 10;
    return ASX_OK;
}

static asx_status stage_multiply_two(void *input, void *output, void *user_data) {
    (void)user_data;
    *(int *)output = *(int *)input * 2;
    return ASX_OK;
}

static asx_status stage_fail(void *input, void *output, void *user_data) {
    (void)input;
    (void)output;
    (void)user_data;
    return ASX_E_INVALID_STATE;
}

/* ================================================================== */
/* Work item tests                                                     */
/* ================================================================== */

TEST(work_item_execute) {
    asx_work_item item;
    int val = 5;
    int result = 0;

    asx_work_item_init(&item, work_double, &val, &result);
    ASSERT_EQ(asx_work_item_execute(&item), ASX_OK);
    ASSERT_EQ(result, 10);
    ASSERT_TRUE(item.completed);
}

TEST(work_item_failure) {
    asx_work_item item;
    int result = 0;

    asx_work_item_init(&item, work_fail, NULL, &result);
    ASSERT_EQ(asx_work_item_execute(&item), ASX_E_DISCONNECTED);
    ASSERT_TRUE(item.completed);
}

/* ================================================================== */
/* Work pool tests                                                     */
/* ================================================================== */

TEST(pool_execute_all_succeed) {
    asx_work_pool pool;
    int vals[] = {1, 2, 3};
    int results[3] = {0};

    asx_work_pool_init(&pool);
    ASSERT_EQ(asx_work_pool_add(&pool, work_double, &vals[0], &results[0]), ASX_OK);
    ASSERT_EQ(asx_work_pool_add(&pool, work_double, &vals[1], &results[1]), ASX_OK);
    ASSERT_EQ(asx_work_pool_add(&pool, work_double, &vals[2], &results[2]), ASX_OK);

    ASSERT_EQ(asx_work_pool_execute_all(&pool), ASX_OK);
    ASSERT_EQ(results[0], 2);
    ASSERT_EQ(results[1], 4);
    ASSERT_EQ(results[2], 6);
    ASSERT_EQ(asx_work_pool_completed(&pool), 3u);
    ASSERT_EQ(asx_work_pool_failed(&pool), 0u);
}

TEST(pool_execute_with_failure) {
    asx_work_pool pool;
    int val = 1;
    int result = 0;

    asx_work_pool_init(&pool);
    ASSERT_EQ(asx_work_pool_add(&pool, work_double, &val, &result), ASX_OK);
    ASSERT_EQ(asx_work_pool_add(&pool, work_fail, NULL, &result), ASX_OK);

    ASSERT_EQ(asx_work_pool_execute_all(&pool), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_work_pool_completed(&pool), 1u);
    ASSERT_EQ(asx_work_pool_failed(&pool), 1u);
}

TEST(pool_capacity_limit) {
    asx_work_pool pool;
    int val = 1;
    int result = 0;
    uint32_t i;

    asx_work_pool_init(&pool);
    for (i = 0; i < ASX_WORK_POOL_MAX_ITEMS; i++) {
        ASSERT_EQ(asx_work_pool_add(&pool, work_double, &val, &result), ASX_OK);
    }
    ASSERT_EQ(asx_work_pool_add(&pool, work_double, &val, &result), ASX_E_RESOURCE_EXHAUSTED);
}

/* ================================================================== */
/* Parallel map tests                                                  */
/* ================================================================== */

TEST(parallel_map_all_elements) {
    int inputs[] = {1, 2, 3, 4};
    int outputs[4] = {0};
    asx_parallel_map_state state;

    asx_parallel_map_init(&state, map_triple, NULL, inputs, outputs, sizeof(int), sizeof(int), 4);
    ASSERT_EQ(asx_parallel_map_execute(&state), ASX_OK);
    ASSERT_EQ(outputs[0], 3);
    ASSERT_EQ(outputs[1], 6);
    ASSERT_EQ(outputs[2], 9);
    ASSERT_EQ(outputs[3], 12);
    ASSERT_EQ(asx_parallel_map_completed(&state), 4u);
}

TEST(parallel_map_empty) {
    asx_parallel_map_state state;
    asx_parallel_map_init(&state, map_triple, NULL, NULL, NULL, sizeof(int), sizeof(int), 0);
    ASSERT_EQ(asx_parallel_map_execute(&state), ASX_OK);
    ASSERT_EQ(asx_parallel_map_completed(&state), 0u);
}

/* ================================================================== */
/* Scatter-gather tests                                                */
/* ================================================================== */

TEST(scatter_gather_all_succeed) {
    asx_scatter_gather sg;
    int vals[] = {10, 20};
    int results[2] = {0};

    asx_scatter_gather_init(&sg);
    ASSERT_EQ(asx_scatter_gather_add(&sg, work_double, &vals[0], &results[0]), ASX_OK);
    ASSERT_EQ(asx_scatter_gather_add(&sg, work_double, &vals[1], &results[1]), ASX_OK);

    ASSERT_EQ(asx_scatter_gather_execute(&sg), ASX_OK);
    ASSERT_EQ(results[0], 20);
    ASSERT_EQ(results[1], 40);
    ASSERT_EQ(asx_scatter_gather_gathered(&sg), 2u);
}

TEST(scatter_gather_with_failure) {
    asx_scatter_gather sg;
    int val = 5;
    int result = 0;

    asx_scatter_gather_init(&sg);
    ASSERT_EQ(asx_scatter_gather_add(&sg, work_double, &val, &result), ASX_OK);
    ASSERT_EQ(asx_scatter_gather_add(&sg, work_fail, NULL, &result), ASX_OK);

    ASSERT_EQ(asx_scatter_gather_execute(&sg), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_scatter_gather_gathered(&sg), 2u);
}

/* ================================================================== */
/* Pipeline tests                                                      */
/* ================================================================== */

TEST(pipeline_two_stages) {
    asx_spork_pipeline pipe;
    int input = 5;
    int output = 0;
    int scratch = 0;

    asx_spork_pipeline_init(&pipe);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_add_ten, NULL), ASX_OK);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_multiply_two, NULL), ASX_OK);

    /* (5 + 10) * 2 = 30 */
    ASSERT_EQ(asx_spork_pipeline_execute(&pipe, &input, &output, &scratch), ASX_OK);
    ASSERT_EQ(output, 30);
}

TEST(pipeline_single_stage) {
    asx_spork_pipeline pipe;
    int input = 7;
    int output = 0;
    int scratch = 0;

    asx_spork_pipeline_init(&pipe);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_add_ten, NULL), ASX_OK);

    ASSERT_EQ(asx_spork_pipeline_execute(&pipe, &input, &output, &scratch), ASX_OK);
    ASSERT_EQ(output, 17);
}

TEST(pipeline_failure_stops) {
    asx_spork_pipeline pipe;
    int input = 5;
    int output = 0;
    int scratch = 0;

    asx_spork_pipeline_init(&pipe);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_add_ten, NULL), ASX_OK);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_fail, NULL), ASX_OK);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_multiply_two, NULL), ASX_OK);

    ASSERT_EQ(asx_spork_pipeline_execute(&pipe, &input, &output, &scratch), ASX_E_INVALID_STATE);
}

TEST(pipeline_stage_count) {
    asx_spork_pipeline pipe;
    asx_spork_pipeline_init(&pipe);

    ASSERT_EQ(asx_spork_pipeline_stage_count(&pipe), 0u);
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_add_ten, NULL), ASX_OK);
    ASSERT_EQ(asx_spork_pipeline_stage_count(&pipe), 1u);
}

TEST(pipeline_capacity_limit) {
    asx_spork_pipeline pipe;
    uint32_t i;

    asx_spork_pipeline_init(&pipe);
    for (i = 0; i < ASX_SPORK_PIPELINE_MAX_STAGES; i++) {
        ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_add_ten, NULL), ASX_OK);
    }
    ASSERT_EQ(asx_spork_pipeline_add_stage(&pipe, stage_add_ten, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== spork tests ===\n");

    /* Work item */
    RUN_TEST(work_item_execute);
    RUN_TEST(work_item_failure);

    /* Work pool */
    RUN_TEST(pool_execute_all_succeed);
    RUN_TEST(pool_execute_with_failure);
    RUN_TEST(pool_capacity_limit);

    /* Parallel map */
    RUN_TEST(parallel_map_all_elements);
    RUN_TEST(parallel_map_empty);

    /* Scatter-gather */
    RUN_TEST(scatter_gather_all_succeed);
    RUN_TEST(scatter_gather_with_failure);

    /* Pipeline */
    RUN_TEST(pipeline_two_stages);
    RUN_TEST(pipeline_single_stage);
    RUN_TEST(pipeline_failure_stops);
    RUN_TEST(pipeline_stage_count);
    RUN_TEST(pipeline_capacity_limit);

    TEST_REPORT();
    return test_failures;
}
