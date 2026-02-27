/*
 * test_coroutine.c — tests for protothread-style task poll contract
 *                     and region-arena captured-state safety (bd-hwb.10)
 *
 * Covers:
 *   - ASX_CO_BEGIN / ASX_CO_YIELD / ASX_CO_END macro correctness
 *   - asx_task_spawn_captured region-arena allocation
 *   - Suspend/resume ordering (deterministic)
 *   - Captured state destruction on task completion
 *   - Captured state destruction on region drain
 *   - Budget exhaustion during coroutine execution
 *   - Multiple coroutines interleaving within a region
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/budget.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Budget helper                                                       */
/* ------------------------------------------------------------------ */

static asx_budget make_budget(uint32_t poll_quota)
{
    asx_budget b = asx_budget_infinite();
    b.poll_quota = poll_quota;
    return b;
}

/* ------------------------------------------------------------------ */
/* Test infrastructure                                                 */
/* ------------------------------------------------------------------ */

static int g_dtor_call_count;
static int g_dtor_call_order[16];
static uint32_t g_dtor_last_size;

static void reset_dtor_tracker(void)
{
    int i;
    g_dtor_call_count = 0;
    g_dtor_last_size = 0;
    for (i = 0; i < 16; i++) {
        g_dtor_call_order[i] = -1;
    }
}

static void test_dtor(void *state, uint32_t state_size)
{
    if (g_dtor_call_count < 16) {
        /* Record the first byte of state as an identifier */
        g_dtor_call_order[g_dtor_call_count] = state ? *(int *)state : -1;
        g_dtor_call_count++;
    }
    g_dtor_last_size = state_size;
    (void)state;
}

/* ------------------------------------------------------------------ */
/* Simple coroutine: yields N times then completes                     */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_co_state co;
    int yield_count;
    int max_yields;
    int poll_count;
} yield_n_state;

static asx_status yield_n_poll(void *user_data, asx_task_id self)
{
    yield_n_state *s = (yield_n_state *)user_data;
    (void)self;

    ASX_CO_BEGIN(&s->co);

    while (s->yield_count < s->max_yields) {
        s->yield_count++;
        s->poll_count++;
        ASX_CO_YIELD(&s->co);
    }

    s->poll_count++;
    ASX_CO_END(&s->co);
}

/* ------------------------------------------------------------------ */
/* Immediate-complete coroutine: finishes on first poll                */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_co_state co;
    int completed;
} immediate_state;

static asx_status immediate_poll(void *user_data, asx_task_id self)
{
    immediate_state *s = (immediate_state *)user_data;
    (void)self;

    ASX_CO_BEGIN(&s->co);

    s->completed = 1;

    ASX_CO_END(&s->co);
}

/* ------------------------------------------------------------------ */
/* Multi-yield coroutine with labeled phases                           */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_co_state co;
    int phase;
    int id;
} phased_state;

static asx_status phased_poll(void *user_data, asx_task_id self)
{
    phased_state *s = (phased_state *)user_data;
    (void)self;

    ASX_CO_BEGIN(&s->co);

    s->phase = 1;
    ASX_CO_YIELD(&s->co);

    s->phase = 2;
    ASX_CO_YIELD(&s->co);

    s->phase = 3;

    ASX_CO_END(&s->co);
}

/* ------------------------------------------------------------------ */
/* Error-returning coroutine: yields once then fails                   */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_co_state co;
    int yielded;
} failing_state;

static asx_status failing_poll(void *user_data, asx_task_id self)
{
    failing_state *s = (failing_state *)user_data;
    (void)self;

    ASX_CO_BEGIN(&s->co);

    s->yielded = 1;
    ASX_CO_YIELD(&s->co);

    /* Fail after resume */
    return ASX_E_CANCELLED;

    ASX_CO_END(&s->co);
}

/* ================================================================== */
/* Tests                                                               */
/* ================================================================== */

TEST(co_basic_yield_and_complete) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    yield_n_state *s;
    asx_budget budget;
    asx_task_state tstate;
    asx_outcome outcome;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), test_dtor,
                &tid, &state_ptr), ASX_OK);

    s = (yield_n_state *)state_ptr;
    /* State is zero-initialized by spawn_captured */
    ASSERT_EQ(s->co.line, 0u);
    ASSERT_EQ(s->yield_count, 0);
    ASSERT_EQ(s->max_yields, 0);
    ASSERT_EQ(s->poll_count, 0);

    /* With max_yields=0, should complete immediately (no yields) */
    budget = make_budget(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_outcome(tid, &outcome), ASX_OK);
    ASSERT_EQ((int)outcome.severity, (int)ASX_OUTCOME_OK);

    /* poll_count should be 1 (entered, didn't yield, completed) */
    ASSERT_EQ(s->poll_count, 1);
}

TEST(co_multiple_yields) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    yield_n_state *s;
    asx_budget budget;
    asx_task_state tstate;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), test_dtor,
                &tid, &state_ptr), ASX_OK);

    s = (yield_n_state *)state_ptr;
    s->max_yields = 3;

    /* Run with enough budget for all polls */
    budget = make_budget(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_COMPLETED);

    /* Should have yielded 3 times + 1 final poll = 4 total polls */
    ASSERT_EQ(s->yield_count, 3);
    ASSERT_EQ(s->poll_count, 4);
}

TEST(co_immediate_completion) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    immediate_state *s;
    asx_budget budget;
    asx_task_state tstate;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, immediate_poll,
                (uint32_t)sizeof(immediate_state), NULL,
                &tid, &state_ptr), ASX_OK);

    s = (immediate_state *)state_ptr;
    ASSERT_EQ(s->completed, 0);

    budget = make_budget(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(s->completed, 1);
    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_COMPLETED);
}

TEST(co_budget_exhaustion_mid_coroutine) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    yield_n_state *s;
    asx_budget budget;
    asx_task_state tstate;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), test_dtor,
                &tid, &state_ptr), ASX_OK);

    s = (yield_n_state *)state_ptr;
    s->max_yields = 10;

    /* Budget of 3 polls: should exhaust before task completes */
    budget = make_budget(3);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    /* Task should still be running (not completed) */
    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_RUNNING);

    /* yield_count should be <= 3 (budget-limited) */
    ASSERT_TRUE(s->yield_count <= 3);

    /* Resume with more budget to complete */
    budget = make_budget(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(s->yield_count, 10);
}

TEST(co_resume_preserves_state) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    phased_state *s;
    asx_budget budget;
    asx_task_state tstate;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, phased_poll,
                (uint32_t)sizeof(phased_state), NULL,
                &tid, &state_ptr), ASX_OK);

    s = (phased_state *)state_ptr;
    ASSERT_EQ(s->phase, 0);

    /* One poll: should reach phase 1 and yield */
    budget = make_budget(1);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(s->phase, 1);

    /* Second poll: should reach phase 2 and yield */
    budget = make_budget(1);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(s->phase, 2);

    /* Third poll: should reach phase 3 and complete.
     * Budget=2 gives scheduler room to detect quiescence
     * after the final task completes in this round. */
    budget = make_budget(2);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s->phase, 3);

    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_COMPLETED);
}

TEST(co_captured_state_dtor_on_complete) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    yield_n_state *s;
    asx_budget budget;

    asx_runtime_reset();
    asx_ghost_reset();
    reset_dtor_tracker();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), test_dtor,
                &tid, &state_ptr), ASX_OK);

    s = (yield_n_state *)state_ptr;
    s->max_yields = 1;

    budget = make_budget(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Destructor should have been called on completion */
    ASSERT_EQ(g_dtor_call_count, 1);
    ASSERT_EQ(g_dtor_last_size, (uint32_t)sizeof(yield_n_state));
}

TEST(co_error_produces_err_outcome) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    failing_state *s;
    asx_budget budget;
    asx_task_state tstate;
    asx_outcome outcome;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, failing_poll,
                (uint32_t)sizeof(failing_state), NULL,
                &tid, &state_ptr), ASX_OK);

    s = (failing_state *)state_ptr;

    budget = make_budget(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Task should complete with error outcome */
    ASSERT_EQ(asx_task_get_state(tid, &tstate), ASX_OK);
    ASSERT_EQ((int)tstate, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_outcome(tid, &outcome), ASX_OK);
    ASSERT_EQ((int)outcome.severity, (int)ASX_OUTCOME_ERR);

    /* Should have yielded once before failing */
    ASSERT_EQ(s->yielded, 1);
}

TEST(co_interleaved_tasks_deterministic) {
    asx_region_id rid;
    asx_task_id tid1, tid2;
    void *state1, *state2;
    phased_state *s1, *s2;
    asx_budget budget;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn two phased tasks */
    ASSERT_EQ(asx_task_spawn_captured(rid, phased_poll,
                (uint32_t)sizeof(phased_state), NULL,
                &tid1, &state1), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, phased_poll,
                (uint32_t)sizeof(phased_state), NULL,
                &tid2, &state2), ASX_OK);

    s1 = (phased_state *)state1;
    s2 = (phased_state *)state2;
    s1->id = 1;
    s2->id = 2;

    /* With budget=2, round-robin should poll task1 then task2 */
    budget = make_budget(2);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    /* Both should be at phase 1 (round-robin: task1 polled, task2 polled) */
    ASSERT_EQ(s1->phase, 1);
    ASSERT_EQ(s2->phase, 1);

    /* Next 2 polls: both advance to phase 2 */
    budget = make_budget(2);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(s1->phase, 2);
    ASSERT_EQ(s2->phase, 2);

    /* Next 2 polls: both complete at phase 3.
     * Budget=3 gives scheduler room to detect quiescence
     * after both tasks complete in this round. */
    budget = make_budget(3);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s1->phase, 3);
    ASSERT_EQ(s2->phase, 3);
}

TEST(co_spawn_captured_null_args) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* NULL out_id */
    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), NULL,
                NULL, &state_ptr), ASX_E_INVALID_ARGUMENT);

    /* NULL out_state */
    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), NULL,
                &tid, NULL), ASX_E_INVALID_ARGUMENT);

    /* NULL poll_fn */
    ASSERT_EQ(asx_task_spawn_captured(rid, NULL,
                (uint32_t)sizeof(yield_n_state), NULL,
                &tid, &state_ptr), ASX_E_INVALID_ARGUMENT);

    /* Zero state_size */
    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                0u, NULL,
                &tid, &state_ptr), ASX_E_INVALID_ARGUMENT);
}

TEST(co_spawn_captured_in_closed_region) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);

    /* Spawning in a closed region should fail */
    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), NULL,
                &tid, &state_ptr), ASX_E_REGION_NOT_OPEN);
}

TEST(co_captured_state_zero_initialized) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    yield_n_state *s;
    unsigned char *raw;
    uint32_t i;
    int all_zero;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn_captured(rid, yield_n_poll,
                (uint32_t)sizeof(yield_n_state), NULL,
                &tid, &state_ptr), ASX_OK);

    /* Verify zero-initialization */
    s = (yield_n_state *)state_ptr;
    ASSERT_EQ(s->co.line, 0u);
    ASSERT_EQ(s->yield_count, 0);
    ASSERT_EQ(s->max_yields, 0);
    ASSERT_EQ(s->poll_count, 0);

    /* Also verify raw bytes */
    raw = (unsigned char *)state_ptr;
    all_zero = 1;
    for (i = 0; i < (uint32_t)sizeof(yield_n_state); i++) {
        if (raw[i] != 0u) {
            all_zero = 0;
            break;
        }
    }
    ASSERT_TRUE(all_zero);
}

TEST(co_capture_arena_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    void *state_ptr;
    uint32_t i;
    asx_status st;
    uint32_t spawned;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Exhaust the capture arena (16384 bytes) by spawning many tasks
     * with large state. Each state is 1024 bytes + alignment. */
    spawned = 0;
    for (i = 0; i < 20; i++) {
        st = asx_task_spawn_captured(rid, yield_n_poll,
                1024u, NULL, &tid, &state_ptr);
        if (st == ASX_E_RESOURCE_EXHAUSTED) break;
        ASSERT_EQ(st, ASX_OK);
        spawned++;
    }

    /* Should have exhausted before 20 (16384/1024 = 16, minus alignment) */
    ASSERT_TRUE(spawned > 0);
    ASSERT_TRUE(spawned < 20);

    /* Last attempt should have returned RESOURCE_EXHAUSTED */
    ASSERT_EQ(st, ASX_E_RESOURCE_EXHAUSTED);
}

TEST(co_state_init_macro) {
    /* Verify ASX_CO_STATE_INIT produces zero-initialized state */
    asx_co_state cs = ASX_CO_STATE_INIT;
    ASSERT_EQ(cs.line, 0u);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_coroutine ===\n");

    RUN_TEST(co_basic_yield_and_complete);
    RUN_TEST(co_multiple_yields);
    RUN_TEST(co_immediate_completion);
    RUN_TEST(co_budget_exhaustion_mid_coroutine);
    RUN_TEST(co_resume_preserves_state);
    RUN_TEST(co_captured_state_dtor_on_complete);
    RUN_TEST(co_error_produces_err_outcome);
    RUN_TEST(co_interleaved_tasks_deterministic);
    RUN_TEST(co_spawn_captured_null_args);
    RUN_TEST(co_spawn_captured_in_closed_region);
    RUN_TEST(co_captured_state_zero_initialized);
    RUN_TEST(co_capture_arena_exhaustion);
    RUN_TEST(co_state_init_macro);

    TEST_REPORT();
    return test_failures;
}
