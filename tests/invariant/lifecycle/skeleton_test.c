/*
 * skeleton_test.c — walking skeleton end-to-end lifecycle integration test
 *
 * Validates the complete lifecycle path through real public headers:
 *   1. Region open
 *   2. Task spawn (with no-op poll function)
 *   3. Scheduler loop drives task to completion
 *   4. Region drain (Close → Finalize → Closed)
 *   5. Quiescence assertion
 *
 * This test is deterministic and must produce identical results
 * across all required CI targets and QEMU configurations.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include "runtime/runtime_internal.h"

/* -------------------------------------------------------------------
 * Test poll functions
 * ------------------------------------------------------------------- */

/* Immediately completes (no-op task) */
static asx_status noop_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* Completes after N polls */
typedef struct {
    int remaining;
} countdown_ctx;

static asx_status countdown_poll(void *user_data, asx_task_id self)
{
    countdown_ctx *ctx = (countdown_ctx *)user_data;
    (void)self;
    if (ctx->remaining > 0) {
        ctx->remaining--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* Always fails */
static asx_status failing_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

/* -------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------- */

TEST(region_open_close)
{
    asx_region_id rid;
    asx_region_state state;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(asx_handle_is_valid(rid));
    ASSERT_EQ(asx_handle_type_tag(rid), ASX_TYPE_REGION);

    st = asx_region_get_state(rid, &state);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);

    st = asx_region_close(rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_region_get_state(rid, &state);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSING);
}

TEST(task_spawn_in_open_region)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state tstate;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(asx_handle_is_valid(tid));
    ASSERT_EQ(asx_handle_type_tag(tid), ASX_TYPE_TASK);

    st = asx_task_get_state(tid, &tstate);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(tstate, ASX_TASK_CREATED);
}

TEST(noop_task_scheduler_run)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state tstate;
    asx_outcome outcome;
    asx_budget budget;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    /* Task should be completed */
    st = asx_task_get_state(tid, &tstate);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(tstate, ASX_TASK_COMPLETED);

    /* Outcome should be OK */
    st = asx_task_get_outcome(tid, &outcome);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
}

TEST(countdown_task_multiple_polls)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state tstate;
    asx_budget budget;
    asx_status st;
    countdown_ctx ctx;

    ctx.remaining = 3;  /* needs 4 polls total (3 pending + 1 ok) */

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, countdown_poll, &ctx, &tid);
    ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_get_state(tid, &tstate);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(tstate, ASX_TASK_COMPLETED);

    ASSERT_EQ(ctx.remaining, 0);
}

TEST(failing_task_outcome_err)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome outcome;
    asx_budget budget;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, failing_poll, NULL, &tid);
    ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_get_outcome(tid, &outcome);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_ERR);
}

TEST(budget_exhaustion_stops_scheduler)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state tstate;
    asx_budget budget;
    asx_status st;
    countdown_ctx ctx;

    ctx.remaining = 100;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, countdown_poll, &ctx, &tid);
    ASSERT_EQ(st, ASX_OK);

    /* Budget of 3 polls — not enough to finish */
    budget = asx_budget_infinite();
    budget.poll_quota = 3;

    st = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(st, ASX_E_POLL_BUDGET_EXHAUSTED);

    /* Task should NOT be completed */
    st = asx_task_get_state(tid, &tstate);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_NE(tstate, ASX_TASK_COMPLETED);
}

TEST(region_drain_full_lifecycle)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_region_state rstate;
    asx_task_state tstate;
    asx_budget budget;
    asx_status st;
    countdown_ctx ctx;

    ctx.remaining = 2;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, countdown_poll, &ctx, &tid);
    ASSERT_EQ(st, ASX_OK);

    /* Drain: close + run scheduler + finalize */
    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    /* Region should be CLOSED */
    st = asx_region_get_state(rid, &rstate);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(rstate, ASX_REGION_CLOSED);

    /* Task should be completed */
    st = asx_task_get_state(tid, &tstate);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(tstate, ASX_TASK_COMPLETED);
}

TEST(quiescence_after_drain)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    /* Quiescence check should pass */
    st = asx_quiescence_check(rid);
    ASSERT_EQ(st, ASX_OK);
}

TEST(quiescence_fails_before_close)
{
    asx_region_id rid;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    /* Region is OPEN — quiescence should fail */
    st = asx_quiescence_check(rid);
    ASSERT_EQ(st, ASX_E_QUIESCENCE_NOT_REACHED);
}

TEST(spawn_rejected_after_close)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    /* Spawn in closed region should fail */
    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    ASSERT_EQ(st, ASX_E_REGION_NOT_OPEN);
}

TEST(multiple_tasks_in_region)
{
    asx_region_id rid;
    asx_task_id tid1, tid2, tid3;
    asx_task_state ts1, ts2, ts3;
    asx_outcome o1, o2, o3;
    asx_budget budget;
    asx_status st;
    countdown_ctx ctx1, ctx2;

    ctx1.remaining = 1;
    ctx2.remaining = 2;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid1);
    ASSERT_EQ(st, ASX_OK);
    st = asx_task_spawn(rid, countdown_poll, &ctx1, &tid2);
    ASSERT_EQ(st, ASX_OK);
    st = asx_task_spawn(rid, countdown_poll, &ctx2, &tid3);
    ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    st = asx_task_get_state(tid1, &ts1);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(ts1, ASX_TASK_COMPLETED);

    st = asx_task_get_state(tid2, &ts2);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(ts2, ASX_TASK_COMPLETED);

    st = asx_task_get_state(tid3, &ts3);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(ts3, ASX_TASK_COMPLETED);

    st = asx_task_get_outcome(tid1, &o1);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&o1), ASX_OUTCOME_OK);

    st = asx_task_get_outcome(tid2, &o2);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&o2), ASX_OUTCOME_OK);

    st = asx_task_get_outcome(tid3, &o3);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&o3), ASX_OUTCOME_OK);
}

/* -------------------------------------------------------------------
 * Cleanup integration: verify region drain calls cleanup handlers
 * ------------------------------------------------------------------- */

static int g_cleanup_called;
static int g_cleanup_order[4];
static int g_cleanup_idx;

static void cleanup_set_flag(void *user_data)
{
    (void)user_data;
    g_cleanup_called = 1;
}

static void cleanup_record_order(void *user_data)
{
    int id = *(int *)user_data;
    if (g_cleanup_idx < 4) {
        g_cleanup_order[g_cleanup_idx++] = id;
    }
}

TEST(region_drain_calls_cleanup)
{
    asx_region_id rid;
    asx_region_slot *r;
    asx_cleanup_handle ch;
    asx_budget budget;
    asx_status st;

    g_cleanup_called = 0;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    /* Access internal slot to push cleanup */
    st = asx_region_slot_lookup(rid, &r);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&r->cleanup, cleanup_set_flag, NULL, &ch), ASX_OK);

    /* Drain should call cleanup during finalization */
    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(g_cleanup_called, 1);
}

TEST(region_drain_cleanup_lifo_order)
{
    asx_region_id rid;
    asx_region_slot *r;
    asx_cleanup_handle ch;
    asx_budget budget;
    asx_status st;
    int ids[3] = {10, 20, 30};

    g_cleanup_idx = 0;

    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);

    st = asx_region_slot_lookup(rid, &r);
    ASSERT_EQ(st, ASX_OK);

    ASSERT_EQ(asx_cleanup_push(&r->cleanup, cleanup_record_order, &ids[0], &ch), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&r->cleanup, cleanup_record_order, &ids[1], &ch), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&r->cleanup, cleanup_record_order, &ids[2], &ch), ASX_OK);

    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    ASSERT_EQ(st, ASX_OK);

    /* Verify LIFO: 30, 20, 10 */
    ASSERT_EQ(g_cleanup_idx, 3);
    ASSERT_EQ(g_cleanup_order[0], 30);
    ASSERT_EQ(g_cleanup_order[1], 20);
    ASSERT_EQ(g_cleanup_order[2], 10);
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "=== skeleton_test (walking skeleton e2e lifecycle) ===\n");

    asx_runtime_reset(); RUN_TEST(region_open_close);
    asx_runtime_reset(); RUN_TEST(task_spawn_in_open_region);
    asx_runtime_reset(); RUN_TEST(noop_task_scheduler_run);
    asx_runtime_reset(); RUN_TEST(countdown_task_multiple_polls);
    asx_runtime_reset(); RUN_TEST(failing_task_outcome_err);
    asx_runtime_reset(); RUN_TEST(budget_exhaustion_stops_scheduler);
    asx_runtime_reset(); RUN_TEST(region_drain_full_lifecycle);
    asx_runtime_reset(); RUN_TEST(quiescence_after_drain);
    asx_runtime_reset(); RUN_TEST(quiescence_fails_before_close);
    asx_runtime_reset(); RUN_TEST(spawn_rejected_after_close);
    asx_runtime_reset(); RUN_TEST(multiple_tasks_in_region);
    asx_runtime_reset(); RUN_TEST(region_drain_calls_cleanup);
    asx_runtime_reset(); RUN_TEST(region_drain_cleanup_lifo_order);

    TEST_REPORT();
    return test_failures;
}
