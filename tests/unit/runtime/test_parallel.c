/*
 * test_parallel.c — unit tests for parallel profile lane scheduler
 *
 * Tests: init/reset, lane assignment/removal, fairness policies,
 * starvation detection, worker state, parallel_run integration,
 * budget exhaustion, cancel lane segregation, deterministic ordering.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>

/* ---- Test poll functions ---- */

static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_yield_n(void *data, asx_task_id self) {
    int *counter = (int *)data;
    (void)self;
    if (*counter > 0) {
        (*counter)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static asx_status poll_forever(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_fail(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

static asx_status poll_checkpoint_forever(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    asx_status cst;
    (void)data;
    cst = asx_checkpoint(self, &cr);
    (void)cst;
    return ASX_E_PENDING;
}

static int g_parallel_dtor_calls;
static uint32_t g_parallel_dtor_last_size;

static void reset_parallel_dtor_tracker(void) {
    g_parallel_dtor_calls = 0;
    g_parallel_dtor_last_size = 0;
}

static void parallel_test_dtor(void *state, uint32_t state_size) {
    (void)state;
    g_parallel_dtor_calls++;
    g_parallel_dtor_last_size = state_size;
}

/* ---- Helpers ---- */

static void reset_all(void) {
    asx_runtime_reset();
    asx_ghost_reset();
    asx_parallel_reset();
}

static asx_parallel_config default_config(void) {
    asx_parallel_config cfg;
    cfg.worker_count = 1;
    cfg.fairness = ASX_FAIRNESS_ROUND_ROBIN;
    cfg.lane_weights[0] = 1;
    cfg.lane_weights[1] = 1;
    cfg.lane_weights[2] = 1;
    cfg.starvation_limit = 5;
    return cfg;
}

/* ================================================================
 * Init / Reset
 * ================================================================ */

TEST(parallel_init_null_config) { ASSERT_EQ(asx_parallel_init(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(parallel_init_zero_workers) {
    asx_parallel_config cfg = default_config();
    cfg.worker_count = 0;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(parallel_init_too_many_workers) {
    asx_parallel_config cfg = default_config();
    cfg.worker_count = ASX_MAX_WORKERS + 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(parallel_init_valid) {
    asx_parallel_config cfg = default_config();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_TRUE(asx_parallel_is_initialized());
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)1);
    asx_parallel_reset();
}

TEST(parallel_init_worker_count_boundaries) {
    asx_parallel_config cfg = default_config();

    cfg.worker_count = 0u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_E_INVALID_ARGUMENT);

    cfg.worker_count = 1u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)1);
    asx_parallel_reset();

    cfg.worker_count = 4u;
#if ASX_MAX_WORKERS >= 4u
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)4);
    asx_parallel_reset();
#else
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_E_RESOURCE_EXHAUSTED);
#endif

    cfg.worker_count = ASX_PARALLEL_GENERIC_TARGET_WORKERS;
#if ASX_MAX_WORKERS >= ASX_PARALLEL_GENERIC_TARGET_WORKERS
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)ASX_PARALLEL_GENERIC_TARGET_WORKERS);
    asx_parallel_reset();
#else
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_E_RESOURCE_EXHAUSTED);
#endif

    cfg.worker_count = ASX_MAX_WORKERS;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)ASX_MAX_WORKERS);
    asx_parallel_reset();

    cfg.worker_count = ASX_MAX_WORKERS + 1u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(parallel_reset_clears_state) {
    asx_parallel_config cfg = default_config();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_TRUE(asx_parallel_is_initialized());

    asx_parallel_reset();
    ASSERT_FALSE(asx_parallel_is_initialized());
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)0);
}

TEST(parallel_public_api_requires_init) {
    asx_lane_state lane_state;
    asx_worker_state worker_state;
    asx_scheduling_metrics metrics;

    reset_all();

    ASSERT_EQ(asx_lane_assign(1, ASX_LANE_READY), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_lane_remove(1), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_lane_get_state(ASX_LANE_READY, &lane_state), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_worker_get_state(0, &worker_state), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_inject_ready(1), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_inject_cancel(1), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_inject_timed(1), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_E_INVALID_STATE);
}

/* ================================================================
 * Lane management
 * ================================================================ */

TEST(lane_assign_and_query) {
    asx_parallel_config cfg = default_config();
    asx_lane_state ls;
    asx_region_id rid;
    asx_task_id tid;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_lane_assign(tid, ASX_LANE_READY), ASX_OK);

    ASSERT_EQ(asx_lane_get_state(ASX_LANE_READY, &ls), ASX_OK);
    ASSERT_EQ(ls.task_count, (uint32_t)1);
    ASSERT_EQ(ls.lane_class, ASX_LANE_READY);

    ASSERT_EQ(asx_lane_total_tasks(), (uint32_t)1);
    asx_parallel_reset();
}

TEST(lane_assign_rejects_invalid_task_handle) {
    asx_parallel_config cfg = default_config();
    asx_region_id rid;
    asx_task_id tid;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_lane_assign(ASX_INVALID_ID, ASX_LANE_READY), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_inject_ready(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_inject_cancel(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_inject_timed(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_lane_assign(rid, ASX_LANE_READY), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_lane_assign(tid + 1u, ASX_LANE_READY), ASX_E_INVALID_ARGUMENT);

    asx_parallel_reset();
}

TEST(lane_assign_to_all_classes) {
    asx_parallel_config cfg = default_config();
    asx_lane_state ls;
    asx_region_id rid;
    asx_task_id t1, t2, t3;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t3), ASX_OK);

    ASSERT_EQ(asx_lane_assign(t1, ASX_LANE_READY), ASX_OK);
    ASSERT_EQ(asx_lane_assign(t2, ASX_LANE_CANCEL), ASX_OK);
    ASSERT_EQ(asx_lane_assign(t3, ASX_LANE_TIMED), ASX_OK);

    ASSERT_EQ(asx_lane_total_tasks(), (uint32_t)3);

    ASSERT_EQ(asx_lane_get_state(ASX_LANE_CANCEL, &ls), ASX_OK);
    ASSERT_EQ(ls.task_count, (uint32_t)1);

    asx_parallel_reset();
}

TEST(lane_remove_existing) {
    asx_parallel_config cfg = default_config();
    asx_region_id rid;
    asx_task_id tid;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_lane_assign(tid, ASX_LANE_READY), ASX_OK);
    ASSERT_EQ(asx_lane_total_tasks(), (uint32_t)1);

    ASSERT_EQ(asx_lane_remove(tid), ASX_OK);
    ASSERT_EQ(asx_lane_total_tasks(), (uint32_t)0);

    asx_parallel_reset();
}

TEST(lane_remove_not_found) {
    asx_parallel_config cfg = default_config();

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_lane_remove(999), ASX_E_NOT_FOUND);

    asx_parallel_reset();
}

TEST(lane_assign_fills_capacity) {
    asx_parallel_config cfg = default_config();
    asx_region_id rid;
    asx_task_id tids[ASX_LANE_TASK_CAPACITY];
    uint32_t i;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0; i < ASX_LANE_TASK_CAPACITY; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tids[i]), ASX_OK);
        ASSERT_EQ(asx_lane_assign(tids[i], ASX_LANE_READY), ASX_OK);
    }

    /* Next should fail */
    ASSERT_EQ(asx_lane_assign(tids[0], ASX_LANE_READY), ASX_E_RESOURCE_EXHAUSTED);

    asx_parallel_reset();
}

TEST(lane_get_state_null_out) {
    ASSERT_EQ(asx_lane_get_state(ASX_LANE_READY, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(lane_get_state_invalid_class) {
    asx_lane_state ls;
    ASSERT_EQ(asx_lane_get_state((asx_lane_class)99, &ls), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================
 * Worker state
 * ================================================================ */

TEST(worker_get_state_valid) {
    asx_parallel_config cfg = default_config();
    cfg.worker_count = 2;
    asx_worker_state ws;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
    ASSERT_EQ(ws.id, (uint32_t)0);
    ASSERT_TRUE(ws.active);

    ASSERT_EQ(asx_worker_get_state(1, &ws), ASX_OK);
    ASSERT_EQ(ws.id, (uint32_t)1);
    ASSERT_TRUE(ws.active);

    asx_parallel_reset();
}

TEST(worker_get_state_out_of_range) {
    asx_parallel_config cfg = default_config();
    asx_worker_state ws;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_worker_get_state(99, &ws), ASX_E_INVALID_ARGUMENT);

    asx_parallel_reset();
}

TEST(worker_get_state_null_out) {
    asx_parallel_config cfg = default_config();

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_worker_get_state(0, NULL), ASX_E_INVALID_ARGUMENT);

    asx_parallel_reset();
}

/* ================================================================
 * Parallel run — single task immediate complete
 * ================================================================ */

TEST(parallel_run_single_task_completes) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    asx_parallel_reset();
}

TEST(parallel_run_task_yields_then_completes) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    int counter = 3;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &counter, &tid), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    asx_parallel_reset();
}

TEST(parallel_run_cancelled_task_sets_completed_cancel_phase) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_outcome out;
    asx_cancel_phase phase;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    /* First poll transitions CREATED->RUNNING and returns PENDING. */
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_COMPLETED);

    asx_parallel_reset();
}

TEST(parallel_run_finalizing_task_sets_completed_cancel_phase) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_cancel_phase phase;
    asx_checkpoint_result cr;
    asx_task_state state;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_forever, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ(asx_task_finalize(tid), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_FINALIZING);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_COMPLETED);

    asx_parallel_reset();
}

TEST(parallel_run_task_fails) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_status st;
    asx_outcome out;
    asx_containment_policy policy;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(100);
    st = asx_parallel_run(rid, &budget);
    policy = asx_containment_policy_active();
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(st, ASX_OK);
    } else {
        ASSERT_EQ(st, ASX_E_INVALID_STATE);
    }

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_ERR);

    asx_parallel_reset();
}

TEST(parallel_run_captured_state_dtor_on_complete) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    void *state_ptr = NULL;

    reset_all();
    reset_parallel_dtor_tracker();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(rid, poll_complete, (uint32_t)sizeof(uint32_t),
                                      parallel_test_dtor, &tid, &state_ptr),
              ASX_OK);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_parallel_dtor_calls, 1);
    ASSERT_EQ(g_parallel_dtor_last_size, (uint32_t)sizeof(uint32_t));

    asx_parallel_reset();
}

/* ================================================================
 * Budget exhaustion
 * ================================================================ */

TEST(parallel_run_budget_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_forever, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(3);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    asx_parallel_reset();
}

TEST(parallel_run_null_budget) { ASSERT_EQ(asx_parallel_run(0, NULL), ASX_E_INVALID_ARGUMENT); }

TEST(parallel_run_not_initialized) {
    asx_budget budget;
    asx_parallel_reset();
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(0, &budget), ASX_E_INVALID_STATE);
}

/* ================================================================
 * Multi-task deterministic ordering
 * ================================================================ */

TEST(parallel_run_multi_task_all_complete) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_parallel_config cfg = default_config();

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t3), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    /* Verify worker completed tasks */
    {
        asx_worker_state ws;
        ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
        ASSERT_EQ(ws.tasks_completed, (uint32_t)3);
    }

    asx_parallel_reset();
}

TEST(parallel_run_no_tasks_is_ok) {
    asx_region_id rid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* No tasks spawned */

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    asx_parallel_reset();
}

/* ================================================================
 * Fairness policy queries
 * ================================================================ */

TEST(parallel_fairness_round_robin) {
    asx_parallel_config cfg = default_config();
    cfg.fairness = ASX_FAIRNESS_ROUND_ROBIN;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_fairness_policy(), ASX_FAIRNESS_ROUND_ROBIN);

    asx_parallel_reset();
}

TEST(parallel_fairness_weighted) {
    asx_parallel_config cfg = default_config();
    cfg.fairness = ASX_FAIRNESS_WEIGHTED;
    cfg.lane_weights[0] = 3;
    cfg.lane_weights[1] = 2;
    cfg.lane_weights[2] = 1;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_fairness_policy(), ASX_FAIRNESS_WEIGHTED);

    asx_parallel_reset();
}

TEST(parallel_fairness_priority) {
    asx_parallel_config cfg = default_config();
    cfg.fairness = ASX_FAIRNESS_PRIORITY;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_fairness_policy(), ASX_FAIRNESS_PRIORITY);

    asx_parallel_reset();
}

/* ================================================================
 * Weighted fairness — lane weights affect scheduling
 * ================================================================ */

TEST(parallel_weighted_run_completes) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    int c1 = 2, c2 = 2;

    cfg.fairness = ASX_FAIRNESS_WEIGHTED;
    cfg.lane_weights[0] = 10; /* READY high weight */
    cfg.lane_weights[1] = 1;  /* CANCEL low weight */
    cfg.lane_weights[2] = 1;  /* TIMED low weight */

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    {
        asx_worker_state ws;
        ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
        ASSERT_EQ(ws.tasks_completed, (uint32_t)2);
    }

    asx_parallel_reset();
}

/* ================================================================
 * Priority fairness — cancel lane gets budget first
 * ================================================================ */

TEST(parallel_priority_run_completes) {
    asx_region_id rid;
    asx_task_id t1;
    asx_budget budget;
    asx_parallel_config cfg = default_config();

    cfg.fairness = ASX_FAIRNESS_PRIORITY;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t1), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    asx_parallel_reset();
}

/* ================================================================
 * Starvation detection
 * ================================================================ */

TEST(parallel_no_starvation_initially) {
    asx_parallel_config cfg = default_config();
    cfg.starvation_limit = 3;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_FALSE(asx_parallel_starvation_detected());
    ASSERT_EQ(asx_parallel_max_starvation(), (uint32_t)0);

    asx_parallel_reset();
}

TEST(parallel_starvation_limit_in_lane_state) {
    asx_parallel_config cfg = default_config();
    asx_lane_state ls;
    asx_region_id rid;
    asx_task_id tid;

    cfg.starvation_limit = 7;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    ASSERT_EQ(asx_lane_assign(tid, ASX_LANE_TIMED), ASX_OK);
    ASSERT_EQ(asx_lane_get_state(ASX_LANE_TIMED, &ls), ASX_OK);
    ASSERT_EQ(ls.max_starvation, (uint32_t)7);
    ASSERT_EQ(ls.weight, (uint32_t)1);

    asx_parallel_reset();
}

/* ================================================================
 * Replay identity — run twice, verify same completion count
 * ================================================================ */

TEST(parallel_replay_identity) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    int c1, c2;
    uint32_t completed_run1;

    /* Run 1 */
    reset_all();
    c1 = 2;
    c2 = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    {
        asx_worker_state ws;
        ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
        completed_run1 = ws.tasks_completed;
    }

    /* Run 2 (identical setup) */
    reset_all();
    c1 = 2;
    c2 = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    {
        asx_worker_state ws;
        ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
        ASSERT_EQ(ws.tasks_completed, completed_run1);
    }

    asx_parallel_reset();
}

TEST(parallel_single_worker_trace_matches_core_scheduler) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    int c1, c2;
    uint32_t i;
    uint32_t core_count;
    asx_trace_event core_events[64];

    /* Core scheduler run. */
    reset_all();
    asx_trace_reset();
    c1 = 1;
    c2 = 1;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    core_count = asx_trace_event_count();
    ASSERT_TRUE(core_count > 0u);
    ASSERT_TRUE(core_count <= 64u);
    for (i = 0; i < core_count; i++) { ASSERT_TRUE(asx_trace_event_get(i, &core_events[i])); }

    /* Parallel single-worker run with identical setup. */
    reset_all();
    asx_trace_reset();
    c1 = 1;
    c2 = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_trace_event_count(), core_count);
    for (i = 0; i < core_count; i++) {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(i, &ev));
        ASSERT_EQ((int)ev.kind, (int)core_events[i].kind);
        ASSERT_EQ(ev.entity_id, core_events[i].entity_id);
        ASSERT_EQ(ev.aux, core_events[i].aux);
    }

    asx_parallel_reset();
}

/* ================================================================
 * Multi-worker config
 * ================================================================ */

TEST(parallel_multi_worker_init) {
    asx_parallel_config cfg = default_config();
    cfg.worker_count = ASX_MAX_WORKERS;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_worker_count(), (uint32_t)ASX_MAX_WORKERS);

    {
        uint32_t i;
        for (i = 0; i < ASX_MAX_WORKERS; i++) {
            asx_worker_state ws;
            ASSERT_EQ(asx_worker_get_state(i, &ws), ASX_OK);
            ASSERT_EQ(ws.id, i);
            ASSERT_TRUE(ws.active);
        }
    }

    asx_parallel_reset();
}

/* ================================================================
 * Lane weight reflected in state
 * ================================================================ */

TEST(parallel_lane_weight_query) {
    asx_parallel_config cfg = default_config();
    asx_lane_state ls;

    cfg.lane_weights[0] = 10; /* READY */
    cfg.lane_weights[1] = 5;  /* CANCEL */
    cfg.lane_weights[2] = 1;  /* TIMED */

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);

    ASSERT_EQ(asx_lane_get_state(ASX_LANE_READY, &ls), ASX_OK);
    ASSERT_EQ(ls.weight, (uint32_t)10);

    ASSERT_EQ(asx_lane_get_state(ASX_LANE_CANCEL, &ls), ASX_OK);
    ASSERT_EQ(ls.weight, (uint32_t)5);

    ASSERT_EQ(asx_lane_get_state(ASX_LANE_TIMED, &ls), ASX_OK);
    ASSERT_EQ(ls.weight, (uint32_t)1);

    asx_parallel_reset();
}

/* ================================================================
 * main
 * ================================================================ */

int main(void) {
    fprintf(stderr, "=== test_parallel ===\n");

    /* Init / Reset */
    RUN_TEST(parallel_init_null_config);
    RUN_TEST(parallel_init_zero_workers);
    RUN_TEST(parallel_init_too_many_workers);
    RUN_TEST(parallel_init_valid);
    RUN_TEST(parallel_init_worker_count_boundaries);
    RUN_TEST(parallel_reset_clears_state);
    RUN_TEST(parallel_public_api_requires_init);

    /* Lane management */
    RUN_TEST(lane_assign_and_query);
    RUN_TEST(lane_assign_rejects_invalid_task_handle);
    RUN_TEST(lane_assign_to_all_classes);
    RUN_TEST(lane_remove_existing);
    RUN_TEST(lane_remove_not_found);
    RUN_TEST(lane_assign_fills_capacity);
    RUN_TEST(lane_get_state_null_out);
    RUN_TEST(lane_get_state_invalid_class);

    /* Worker state */
    RUN_TEST(worker_get_state_valid);
    RUN_TEST(worker_get_state_out_of_range);
    RUN_TEST(worker_get_state_null_out);

    /* Parallel run */
    RUN_TEST(parallel_run_single_task_completes);
    RUN_TEST(parallel_run_task_yields_then_completes);
    RUN_TEST(parallel_run_cancelled_task_sets_completed_cancel_phase);
    RUN_TEST(parallel_run_finalizing_task_sets_completed_cancel_phase);
    RUN_TEST(parallel_run_task_fails);
    RUN_TEST(parallel_run_captured_state_dtor_on_complete);
    RUN_TEST(parallel_run_budget_exhaustion);
    RUN_TEST(parallel_run_null_budget);
    RUN_TEST(parallel_run_not_initialized);
    RUN_TEST(parallel_run_multi_task_all_complete);
    RUN_TEST(parallel_run_no_tasks_is_ok);

    /* Fairness policies */
    RUN_TEST(parallel_fairness_round_robin);
    RUN_TEST(parallel_fairness_weighted);
    RUN_TEST(parallel_fairness_priority);
    RUN_TEST(parallel_weighted_run_completes);
    RUN_TEST(parallel_priority_run_completes);

    /* Starvation */
    RUN_TEST(parallel_no_starvation_initially);
    RUN_TEST(parallel_starvation_limit_in_lane_state);

    /* Replay identity */
    RUN_TEST(parallel_replay_identity);
    RUN_TEST(parallel_single_worker_trace_matches_core_scheduler);

    /* Multi-worker */
    RUN_TEST(parallel_multi_worker_init);

    /* Lane weight query */
    RUN_TEST(parallel_lane_weight_query);

    TEST_REPORT();
    return test_failures;
}
