/*
 * test_protothread.c — unit tests for protothread poll contract and captured state
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>

typedef struct yield_state {
    asx_co_state co;
    uint32_t count;
    int order[4];
} yield_state;

static int g_dtor_calls = 0;
static uint32_t g_dtor_size = 0;

static void dtor_reset(void) {
    g_dtor_calls = 0;
    g_dtor_size = 0;
}

static void state_dtor(void *state, uint32_t state_size) {
    (void)state;
    g_dtor_calls++;
    g_dtor_size = state_size;
}

static asx_status yielding_poll(void *user_data, asx_task_id self) {
    yield_state *st = (yield_state *)user_data;
    (void)self;

    ASX_CO_BEGIN(&st->co);
    st->order[st->count++] = 1;
    ASX_CO_YIELD(&st->co);
    st->order[st->count++] = 2;
    ASX_CO_YIELD(&st->co);
    st->order[st->count++] = 3;
    ASX_CO_END(&st->co);
}

static asx_status immediate_ok_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

TEST(protothread_yield_resume_ordering) {
    asx_region_id rid;
    asx_task_id tid;
    yield_state *st = NULL;
    asx_outcome out;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, yielding_poll, (uint32_t)sizeof(yield_state), NULL, &tid,
                                      (void **)&st),
              ASX_OK);
    ASSERT_TRUE(st != NULL);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(st->count, 3u);
    ASSERT_EQ(st->order[0], 1);
    ASSERT_EQ(st->order[1], 2);
    ASSERT_EQ(st->order[2], 3);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
}

TEST(captured_state_arena_limit_enforced) {
    asx_region_id rid;
    asx_task_id tid;
    void *state = NULL;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, immediate_ok_poll, ASX_REGION_CAPTURE_ARENA_BYTES + 8u,
                                      NULL, &tid, &state),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(captured_state_destructor_runs_on_completion) {
    asx_region_id rid;
    asx_task_id tid;
    void *state = NULL;
    asx_budget budget;

    asx_runtime_reset();
    dtor_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, immediate_ok_poll, 24u, state_dtor, &tid, &state),
              ASX_OK);
    ASSERT_TRUE(state != NULL);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_dtor_calls, 1);
    ASSERT_EQ(g_dtor_size, 24u);
}

TEST(region_drain_completes_yielding_task) {
    asx_region_id rid;
    asx_task_id tid;
    yield_state *st = NULL;
    asx_budget budget;
    asx_region_state rstate;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, yielding_poll, (uint32_t)sizeof(yield_state), NULL, &tid,
                                      (void **)&st),
              ASX_OK);
    ASSERT_TRUE(st != NULL);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(st->count, 3u);
    ASSERT_EQ(asx_region_get_state(rid, &rstate), ASX_OK);
    ASSERT_EQ(rstate, ASX_REGION_CLOSED);
}

int main(void) {
    fprintf(stderr, "=== test_protothread ===\n");
    RUN_TEST(protothread_yield_resume_ordering);
    RUN_TEST(captured_state_arena_limit_enforced);
    RUN_TEST(captured_state_destructor_runs_on_completion);
    RUN_TEST(region_drain_completes_yielding_task);
    TEST_REPORT();
    return test_failures;
}
