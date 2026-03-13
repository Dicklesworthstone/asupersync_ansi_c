/*
 * test_scheduler_enhanced.c — tests for three-lane scheduler enhancements
 *
 * Tests cover:
 *   - Global injector API (inject_cancel, inject_timed, inject_ready)
 *   - Cancel-streak fairness enforcement
 *   - Scheduling metrics tracking
 *   - Cancel-streak limit configuration
 *   - Deterministic parity between single-worker parallel and basic scheduler
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/rt.h>
#include <string.h>

/* Suppress warn_unused_result */
static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

/* ------------------------------------------------------------------ */
/* Poll functions                                                      */
/* ------------------------------------------------------------------ */

static asx_status poll_ok(void *data, asx_task_id self)
{
    (void)data; (void)self;
    return ASX_OK;
}

static int g_pending_count = 0;
static int g_pending_limit = 2;

static asx_status poll_n_pending(void *data, asx_task_id self)
{
    (void)data; (void)self;
    g_pending_count++;
    if (g_pending_count < g_pending_limit)
        return ASX_E_PENDING;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static asx_runtime g_rt;

static void setup(void)
{
    asx_parallel_config pcfg;
    MUST_OK(asx_runtime_init_default(&g_rt));
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.worker_count = 1;
    pcfg.fairness = ASX_FAIRNESS_PRIORITY;
    pcfg.lane_weights[0] = 50;
    pcfg.lane_weights[1] = 30;
    pcfg.lane_weights[2] = 20;
    pcfg.starvation_limit = 10;
    MUST_OK(asx_parallel_init(&pcfg));
    g_pending_count = 0;
    g_pending_limit = 2;
}

static void teardown(void)
{
    asx_runtime_shutdown(&g_rt);
}

/* ------------------------------------------------------------------ */
/* Injector API tests                                                  */
/* ------------------------------------------------------------------ */

TEST(inject_ready_assigns_to_ready_lane) {
    asx_region_id rid;
    asx_task_id tid;
    asx_lane_state ls;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &tid));
    ASSERT_EQ(asx_inject_ready(tid), ASX_OK);
    MUST_OK(asx_lane_get_state(ASX_LANE_READY, &ls));
    ASSERT_EQ(ls.task_count, 1u);
    teardown();
}

TEST(inject_cancel_assigns_to_cancel_lane) {
    asx_region_id rid;
    asx_task_id tid;
    asx_lane_state ls;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &tid));
    ASSERT_EQ(asx_inject_cancel(tid), ASX_OK);
    MUST_OK(asx_lane_get_state(ASX_LANE_CANCEL, &ls));
    ASSERT_EQ(ls.task_count, 1u);
    teardown();
}

TEST(inject_timed_assigns_to_timed_lane) {
    asx_region_id rid;
    asx_task_id tid;
    asx_lane_state ls;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &tid));
    ASSERT_EQ(asx_inject_timed(tid), ASX_OK);
    MUST_OK(asx_lane_get_state(ASX_LANE_TIMED, &ls));
    ASSERT_EQ(ls.task_count, 1u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Metrics tests                                                       */
/* ------------------------------------------------------------------ */

TEST(metrics_null_out_fails) {
    ASSERT_EQ(asx_parallel_get_metrics(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(metrics_zero_after_reset) {
    asx_scheduling_metrics m;
    setup();
    asx_parallel_reset_metrics();
    MUST_OK(asx_parallel_get_metrics(&m));
    ASSERT_EQ(m.cancel_dispatches, 0u);
    ASSERT_EQ(m.timed_dispatches, 0u);
    ASSERT_EQ(m.ready_dispatches, 0u);
    ASSERT_EQ(m.cancel_streak, 0u);
    ASSERT_EQ(m.cancel_streak_max, 0u);
    ASSERT_EQ(m.fairness_yields, 0u);
    teardown();
}

TEST(metrics_track_ready_dispatches) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget b;
    asx_scheduling_metrics m;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &tid));
    b = asx_budget_from_polls(100);
    asx_parallel_reset_metrics();
    MUST_OK(asx_parallel_run(rid, &b));
    MUST_OK(asx_parallel_get_metrics(&m));
    ASSERT_TRUE(m.ready_dispatches > 0);
    ASSERT_EQ(m.cancel_dispatches, 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Cancel-streak limit tests                                           */
/* ------------------------------------------------------------------ */

TEST(cancel_streak_limit_default) {
    setup();
    ASSERT_EQ(asx_parallel_cancel_streak_limit(), 16u);
    teardown();
}

TEST(cancel_streak_limit_set_get) {
    setup();
    asx_parallel_set_cancel_streak_limit(8);
    ASSERT_EQ(asx_parallel_cancel_streak_limit(), 8u);
    asx_parallel_set_cancel_streak_limit(0);
    ASSERT_EQ(asx_parallel_cancel_streak_limit(), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Deterministic parity test                                           */
/* ------------------------------------------------------------------ */

TEST(single_worker_parallel_completes_all_tasks) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget b;
    asx_task_state st1, st2, st3;
    setup();
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &t1));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &t2));
    MUST_OK(asx_task_spawn(rid, poll_ok, NULL, &t3));
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &b), ASX_OK);
    MUST_OK(asx_task_get_state(t1, &st1));
    MUST_OK(asx_task_get_state(t2, &st2));
    MUST_OK(asx_task_get_state(t3, &st3));
    ASSERT_EQ(st1, ASX_TASK_COMPLETED);
    ASSERT_EQ(st2, ASX_TASK_COMPLETED);
    ASSERT_EQ(st3, ASX_TASK_COMPLETED);
    teardown();
}

TEST(parallel_run_with_pending_tasks) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget b;
    asx_task_state st;
    setup();
    g_pending_count = 0;
    g_pending_limit = 3;
    MUST_OK(asx_region_open(&rid));
    MUST_OK(asx_task_spawn(rid, poll_n_pending, NULL, &tid));
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &b), ASX_OK);
    MUST_OK(asx_task_get_state(tid, &st));
    ASSERT_EQ(st, ASX_TASK_COMPLETED);
    ASSERT_EQ(g_pending_count, 3);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Capacity queries via runtime                                        */
/* ------------------------------------------------------------------ */

TEST(capacity_queries) {
    ASSERT_EQ(asx_runtime_region_capacity(), (uint32_t)ASX_MAX_REGIONS);
    ASSERT_EQ(asx_runtime_task_capacity(), (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(asx_runtime_obligation_capacity(), (uint32_t)ASX_MAX_OBLIGATIONS);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_scheduler_enhanced ===\n");

    /* Injector */
    RUN_TEST(inject_ready_assigns_to_ready_lane);
    RUN_TEST(inject_cancel_assigns_to_cancel_lane);
    RUN_TEST(inject_timed_assigns_to_timed_lane);

    /* Metrics */
    RUN_TEST(metrics_null_out_fails);
    RUN_TEST(metrics_zero_after_reset);
    RUN_TEST(metrics_track_ready_dispatches);

    /* Cancel-streak limit */
    RUN_TEST(cancel_streak_limit_default);
    RUN_TEST(cancel_streak_limit_set_get);

    /* Deterministic parity */
    RUN_TEST(single_worker_parallel_completes_all_tasks);
    RUN_TEST(parallel_run_with_pending_tasks);

    /* Capacity */
    RUN_TEST(capacity_queries);

    TEST_REPORT();
    return test_failures;
}
