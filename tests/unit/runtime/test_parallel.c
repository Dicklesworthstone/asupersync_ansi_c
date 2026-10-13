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
#include "../../test_runtime.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/runtime/waker.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

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

typedef struct {
    int id;
} poll_order_state;

static int g_poll_order[8];
static uint32_t g_poll_order_count;
static uint32_t g_parallel_ghost_ready;

static asx_status poll_record_order(void *data, asx_task_id self) {
    poll_order_state *state = (poll_order_state *)data;
    (void)self;
    if (state != NULL && g_poll_order_count < 8u) {
        g_poll_order[g_poll_order_count] = state->id;
        g_poll_order_count++;
    }
    return ASX_OK;
}

static void reset_poll_order(void) {
    uint32_t i;
    for (i = 0; i < 8u; i++) { g_poll_order[i] = 0; }
    g_poll_order_count = 0u;
}

static asx_status parallel_test_ghost_reactor(void *ctx, uint64_t logical_step,
                                              uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = g_parallel_ghost_ready;
    return ASX_OK;
}

/* The same readiness through the native hook, which live builds wait on
 * instead of the ghost reactor (asx_runtime_reactor_wait). */
static asx_status parallel_test_native_reactor(void *ctx, uint32_t timeout_ms,
                                               uint32_t *ready_count) {
    (void)timeout_ms;
    return parallel_test_ghost_reactor(ctx, 0u, ready_count);
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
    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    asx_parallel_reset();
    asx_waker_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_driver_reset();
#endif
    reset_poll_order();
    g_parallel_ghost_ready = 0u;
}

static asx_parallel_config default_config(void) {
    asx_parallel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.worker_count = 1;
    cfg.fairness = ASX_FAIRNESS_ROUND_ROBIN;
    cfg.lane_weights[0] = 1;
    cfg.lane_weights[1] = 1;
    cfg.lane_weights[2] = 1;
    cfg.starvation_limit = 5;
    asx_parallel_admission_policy_init(&cfg.admission_policy);
    asx_parallel_locality_config_init(&cfg.locality);
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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    for (i = 0; i < ASX_LANE_TASK_CAPACITY; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tids[i]), ASX_OK);
        ASSERT_EQ(asx_lane_assign(tids[i], ASX_LANE_READY), ASX_OK);
    }

    /* Reassigning an already-lane-owned task is idempotent, not a duplicate. */
    ASSERT_EQ(asx_lane_assign(tids[0], ASX_LANE_READY), ASX_OK);
    ASSERT_EQ(asx_lane_total_tasks(), ASX_LANE_TASK_CAPACITY);

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

TEST(worker_lifecycle_drains_on_quiescence) {
    asx_region_id rid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_worker_state ws;

    cfg.worker_count = 4;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
    ASSERT_TRUE(ws.active);
    ASSERT_EQ((int)ws.lifecycle, (int)ASX_WORKER_RUNNING);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
    ASSERT_FALSE(ws.active);
    ASSERT_EQ((int)ws.lifecycle, (int)ASX_WORKER_DRAINED);

    asx_parallel_reset();
}

TEST(worker_lifecycle_marks_draining_on_budget_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_worker_state ws;

    cfg.worker_count = 2;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_forever, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
    ASSERT_TRUE(ws.active);
    ASSERT_EQ((int)ws.lifecycle, (int)ASX_WORKER_DRAINING);

    asx_parallel_reset();
}

TEST(worker_lane_depths_track_manual_injection) {
    asx_parallel_config cfg = default_config();
    asx_region_id rid;
    asx_task_id ready_task, cancel_task, timed_task;
    uint32_t lane_totals[ASX_MAX_LANES] = {0u, 0u, 0u};
    uint32_t i;

    cfg.worker_count = 4;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &ready_task), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &cancel_task), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &timed_task), ASX_OK);

    ASSERT_EQ(asx_inject_ready(ready_task), ASX_OK);
    ASSERT_EQ(asx_inject_cancel(cancel_task), ASX_OK);
    ASSERT_EQ(asx_inject_timed(timed_task), ASX_OK);

    for (i = 0; i < cfg.worker_count; i++) {
        asx_worker_state ws;
        ASSERT_EQ(asx_worker_get_state(i, &ws), ASX_OK);
        lane_totals[ASX_LANE_READY] += ws.lane_depths[ASX_LANE_READY];
        lane_totals[ASX_LANE_CANCEL] += ws.lane_depths[ASX_LANE_CANCEL];
        lane_totals[ASX_LANE_TIMED] += ws.lane_depths[ASX_LANE_TIMED];
    }

    ASSERT_EQ(lane_totals[ASX_LANE_READY], 1u);
    ASSERT_EQ(lane_totals[ASX_LANE_CANCEL], 1u);
    ASSERT_EQ(lane_totals[ASX_LANE_TIMED], 1u);

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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

/* Acknowledges its cancel by returning ASX_E_CANCELLED. */
static asx_status poll_checkpoint_then_cancelled_error(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_E_CANCELLED;
    return ASX_E_PENDING;
}

TEST(parallel_cancelled_task_returning_its_error_is_not_a_fault) {
    /* The error a cancelled task returns is how it acknowledged the
     * cancel: it completes CANCELLED and nothing is contained, as in the
     * single-threaded scheduler. Before, the parallel scheduler handed the
     * error to fault containment (fail-fast returned it from the run, the
     * poison policy poisoned the region). */
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_outcome out;
    int poisoned = 1;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_cancelled_error, NULL, &tid), ASX_OK);
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    ASSERT_FALSE(poisoned);

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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    ASSERT_EQ(asx_lane_assign(tid, ASX_LANE_TIMED), ASX_OK);
    ASSERT_EQ(asx_lane_get_state(ASX_LANE_TIMED, &ls), ASX_OK);
    ASSERT_EQ(ls.max_starvation, (uint32_t)7);
    ASSERT_EQ(ls.weight, (uint32_t)1);

    asx_parallel_reset();
}

/* ================================================================
 * Timed lane, wakers, and reactor readiness
 * ================================================================ */

TEST(timed_lane_waits_for_waker_before_polling) {
    asx_region_id rid;
    asx_task_id tid;
    asx_waker waker;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_scheduling_metrics metrics;
    poll_order_state task = {1};

    cfg.worker_count = 2;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &task, &tid), ASX_OK);
    ASSERT_EQ(asx_waker_register(tid, &waker), ASX_OK);
    ASSERT_EQ(asx_inject_timed(tid), ASX_OK);

    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(g_poll_order_count, 0u);

    ASSERT_EQ(asx_waker_wake(&waker), ASX_OK);
    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 1u);
    ASSERT_EQ(g_poll_order[0], 1);

    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_OK);
    ASSERT_EQ(metrics.waker_ready, 1u);
    ASSERT_EQ(metrics.timed_promotions, 1u);

    asx_parallel_reset();
}

TEST(timed_equal_deadline_wakers_preserve_fire_order) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_waker w1, w2;
    asx_timer_handle h1, h2;
    void *ready_wakers[2];
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    poll_order_state s1 = {1};
    poll_order_state s2 = {2};
    uint32_t ready_count;
    uint32_t i;

    cfg.worker_count = 2;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &s1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &s2, &t2), ASX_OK);
    ASSERT_EQ(asx_waker_register(t1, &w1), ASX_OK);
    ASSERT_EQ(asx_waker_register(t2, &w2), ASX_OK);
    ASSERT_EQ(asx_inject_timed(t1), ASX_OK);
    ASSERT_EQ(asx_inject_timed(t2), ASX_OK);

    ASSERT_EQ(asx_timer_register(asx_timer_wheel_global(), 100, &w1, &h1), ASX_OK);
    ASSERT_EQ(asx_timer_register(asx_timer_wheel_global(), 100, &w2, &h2), ASX_OK);
    ready_count = asx_timer_collect_expired(asx_timer_wheel_global(), 100, ready_wakers, 2);
    ASSERT_EQ(ready_count, 2u);
    for (i = 0; i < ready_count; i++) {
        ASSERT_EQ(asx_waker_wake((const asx_waker *)ready_wakers[i]), ASX_OK);
    }

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 2u);
    ASSERT_EQ(g_poll_order[0], 1);
    ASSERT_EQ(g_poll_order[1], 2);

    asx_parallel_reset();
}

TEST(reactor_readiness_promotes_timed_waker) {
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_region_id rid;
    asx_task_id tid;
    asx_waker waker;
    asx_io_token token;
    asx_runtime_hooks hooks;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_scheduling_metrics metrics;
    poll_order_state task = {7};

    cfg.worker_count = 2;
    reset_all();
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.reactor.ghost_wait_fn = parallel_test_ghost_reactor;
    hooks.reactor.wait_fn = parallel_test_native_reactor;
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_io_driver_init(), ASX_OK);

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &task, &tid), ASX_OK);
    ASSERT_EQ(asx_waker_register(tid, &waker), ASX_OK);
    ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &waker, &token), ASX_OK);
    ASSERT_EQ(asx_inject_timed(tid), ASX_OK);

    g_parallel_ghost_ready = 1u;
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 1u);
    ASSERT_EQ(g_poll_order[0], 7);

    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_OK);
    ASSERT_TRUE(metrics.reactor_polls > 0u);
    ASSERT_EQ(metrics.reactor_ready, 1u);
    ASSERT_EQ(metrics.waker_ready, 1u);
    ASSERT_EQ(metrics.timed_promotions, 1u);

    asx_io_driver_shutdown();
#endif
}

/* ================================================================
 * Multi-worker routing and replay-stable commit accounting
 * ================================================================ */

TEST(parallel_multi_worker_steals_preserve_completion) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    asx_scheduling_metrics metrics;
    asx_worker_state ws;
    int c1 = 3;
    int c2 = 3;

    cfg.worker_count = 4;
    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_OK);
    ASSERT_TRUE(metrics.steal_attempts > 0u);
    ASSERT_TRUE(metrics.steals_succeeded > 0u);
    ASSERT_TRUE(metrics.commit_sequence >= metrics.ready_dispatches);

    ASSERT_EQ(asx_worker_get_state(0, &ws), ASX_OK);
    ASSERT_EQ(ws.lane_depths[ASX_LANE_READY], 0u);

    asx_parallel_reset();
}

TEST(parallel_multi_worker_trace_matches_single_worker) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    int c1, c2, c3;
    uint32_t i;
    uint32_t single_count;
    asx_trace_event single_events[96];

    reset_all();
    asx_trace_reset(test_runtime());
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    single_count = asx_trace_event_count(test_runtime());
    ASSERT_TRUE(single_count > 0u);
    ASSERT_TRUE(single_count <= 96u);
    for (i = 0; i < single_count; i++) {
        ASSERT_TRUE(asx_trace_event_get(test_runtime(), i, &single_events[i]));
    }

    reset_all();
    asx_trace_reset(test_runtime());
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 4;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_trace_event_count(test_runtime()), single_count);
    for (i = 0; i < single_count; i++) {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(test_runtime(), i, &ev));
        ASSERT_EQ((int)ev.kind, (int)single_events[i].kind);
        ASSERT_EQ(ev.entity_id, single_events[i].entity_id);
        ASSERT_EQ(ev.aux, single_events[i].aux);
    }

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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    asx_trace_reset(test_runtime());
    c1 = 1;
    c2 = 1;
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    core_count = asx_trace_event_count(test_runtime());
    ASSERT_TRUE(core_count > 0u);
    ASSERT_TRUE(core_count <= 64u);
    for (i = 0; i < core_count; i++) {
        ASSERT_TRUE(asx_trace_event_get(test_runtime(), i, &core_events[i]));
    }

    /* Parallel single-worker run with identical setup. */
    reset_all();
    asx_trace_reset(test_runtime());
    c1 = 1;
    c2 = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_trace_event_count(test_runtime()), core_count);
    for (i = 0; i < core_count; i++) {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(test_runtime(), i, &ev));
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

TEST(parallel_locality_default_compact_snapshot) {
    asx_parallel_config cfg = default_config();
    asx_parallel_locality_snapshot snapshot;

    cfg.worker_count = 4;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_get_locality_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ((int)snapshot.mode, (int)ASX_PARALLEL_LOCALITY_COMPACT);
    ASSERT_EQ(snapshot.shard_count, 1u);
    ASSERT_EQ(snapshot.tasks_per_shard, (uint32_t)ASX_MAX_TASKS);
    ASSERT_EQ(snapshot.slot_count, (uint32_t)ASX_MAX_TASKS);

    asx_parallel_reset();
}

TEST(parallel_worker_sharded_locality_routes_by_contiguous_slot_ranges) {
    asx_region_id rid;
    asx_task_id tids[8];
    asx_parallel_config cfg = default_config();
    asx_parallel_locality_snapshot snapshot;
    asx_worker_state ws;
    uint32_t shard;
    uint32_t worker;
    uint32_t i;

    cfg.worker_count = 4;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;
    cfg.locality.shard_count = 4u;
    cfg.locality.tasks_per_shard = 2u;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);

    for (i = 0u; i < 8u; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_forever, NULL, &tids[i]), ASX_OK);
        ASSERT_EQ(asx_lane_assign(tids[i], ASX_LANE_READY), ASX_OK);
    }

    ASSERT_EQ(asx_parallel_get_locality_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ((int)snapshot.mode, (int)ASX_PARALLEL_LOCALITY_WORKER_SHARDED);
    ASSERT_EQ(snapshot.shard_count, 4u);
    ASSERT_EQ(snapshot.tasks_per_shard, 2u);
    /* Indexed through i: the test is compiled (and skipped) on profiles whose
     * shard_task_counts has fewer than 4 entries, where constant indices
     * would trip -Warray-bounds. */
    for (i = 0u; i < 4u; i++) { ASSERT_EQ(snapshot.shard_task_counts[i], 2u); }
    ASSERT_EQ(snapshot.max_shard_tasks, 2u);

    ASSERT_EQ(asx_parallel_task_locality(tids[0], &shard, &worker), ASX_OK);
    ASSERT_EQ(shard, 0u);
    ASSERT_EQ(worker, 0u);
    ASSERT_EQ(asx_parallel_task_locality(tids[2], &shard, &worker), ASX_OK);
    ASSERT_EQ(shard, 1u);
    ASSERT_EQ(worker, 1u);
    ASSERT_EQ(asx_parallel_task_locality(tids[6], &shard, &worker), ASX_OK);
    ASSERT_EQ(shard, 3u);
    ASSERT_EQ(worker, 3u);

    ASSERT_EQ(asx_worker_get_state(3u, &ws), ASX_OK);
    ASSERT_EQ(ws.lane_depths[ASX_LANE_READY], 2u);

    asx_parallel_reset();
}

/* Profiles without worker-sharded locality accept the request and run COMPACT. */
TEST(parallel_sharded_request_falls_back_to_compact_without_sharding) {
    asx_parallel_config cfg = default_config();
    asx_parallel_locality_snapshot snapshot;

    cfg.worker_count = ASX_MAX_WORKERS;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;
    cfg.locality.shard_count = ASX_MAX_WORKERS;
    cfg.locality.tasks_per_shard = 2u;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_get_locality_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ((int)snapshot.mode, (int)ASX_PARALLEL_LOCALITY_COMPACT);

    asx_parallel_reset();
}

TEST(parallel_worker_sharded_locality_defaults_to_worker_shards) {
    asx_parallel_config cfg = default_config();
    asx_parallel_locality_snapshot snapshot;

    cfg.worker_count = 8u;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_get_locality_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ((int)snapshot.mode, (int)ASX_PARALLEL_LOCALITY_WORKER_SHARDED);
    ASSERT_EQ(snapshot.shard_count, 8u);
    /* Default shard size covers the task arena: ceil(ASX_MAX_TASKS / shards). */
    ASSERT_EQ(snapshot.tasks_per_shard, (uint32_t)((ASX_MAX_TASKS + 7u) / 8u));

    asx_parallel_reset();
}

TEST(parallel_task_locality_rejects_stale_handle) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_id stale;
    asx_parallel_config cfg = default_config();
    uint32_t shard;
    uint32_t worker;

    cfg.worker_count = 2;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_forever, NULL, &tid), ASX_OK);

    stale = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << ASX_TASK_CREATED),
                            asx_handle_pack_index((uint16_t)(asx_handle_generation(tid) + 1u),
                                                  asx_handle_slot(tid)));
    ASSERT_EQ(asx_parallel_task_locality(stale, &shard, &worker), ASX_E_INVALID_ARGUMENT);

    asx_parallel_reset();
}

TEST(parallel_worker_sharded_trace_matches_compact) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    int c1, c2, c3;
    uint32_t i;
    uint32_t compact_count;
    asx_trace_event compact_events[96];

    reset_all();
    asx_trace_reset(test_runtime());
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 4;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    compact_count = asx_trace_event_count(test_runtime());
    ASSERT_TRUE(compact_count > 0u);
    ASSERT_TRUE(compact_count <= 96u);
    for (i = 0; i < compact_count; i++) {
        ASSERT_TRUE(asx_trace_event_get(test_runtime(), i, &compact_events[i]));
    }

    reset_all();
    asx_trace_reset(test_runtime());
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 4;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;
    cfg.locality.shard_count = 4u;
    cfg.locality.tasks_per_shard = 1u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_trace_event_count(test_runtime()), compact_count);
    for (i = 0; i < compact_count; i++) {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(test_runtime(), i, &ev));
        ASSERT_EQ((int)ev.kind, (int)compact_events[i].kind);
        ASSERT_EQ(ev.entity_id, compact_events[i].entity_id);
        ASSERT_EQ(ev.aux, compact_events[i].aux);
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
 * Admission and telemetry evidence
 * ================================================================ */

TEST(parallel_admission_evaluate_modes) {
    asx_parallel_admission_policy policy;
    asx_parallel_admission_decision decision;

    asx_parallel_admission_policy_init(&policy);
    policy.pressure_threshold_pct = 50u;

    ASSERT_EQ(asx_parallel_evaluate_admission(&policy, 49u, 100u, &decision), ASX_OK);
    ASSERT_FALSE(decision.triggered);
    ASSERT_EQ((int)decision.admit_status, (int)ASX_OK);

    ASSERT_EQ(asx_parallel_evaluate_admission(&policy, 50u, 100u, &decision), ASX_OK);
    ASSERT_TRUE(decision.triggered);
    ASSERT_EQ((int)decision.mode, (int)ASX_PARALLEL_ADMISSION_REJECT);
    ASSERT_EQ((int)decision.admit_status, (int)ASX_E_ADMISSION_CLOSED);

    policy.mode = ASX_PARALLEL_ADMISSION_BACKPRESSURE;
    ASSERT_EQ(asx_parallel_evaluate_admission(&policy, 75u, 100u, &decision), ASX_OK);
    ASSERT_TRUE(decision.triggered);
    ASSERT_EQ((int)decision.admit_status, (int)ASX_E_WOULD_BLOCK);

    policy.mode = ASX_PARALLEL_ADMISSION_SHED_OLDEST;
    policy.shed_max = 3u;
    ASSERT_EQ(asx_parallel_evaluate_admission(&policy, 75u, 100u, &decision), ASX_OK);
    ASSERT_TRUE(decision.triggered);
    ASSERT_EQ((int)decision.admit_status, (int)ASX_OK);
    ASSERT_EQ(decision.shed_count, 3u);

    policy.pressure_threshold_pct = 101u;
    ASSERT_EQ(asx_parallel_evaluate_admission(&policy, 75u, 100u, &decision),
              ASX_E_INVALID_ARGUMENT);
}

TEST(parallel_admission_enforced_backpressure_is_atomic) {
    /* Lane depth at which pressure first reaches the 2% threshold:
     * depth * 100 >= 2 * ASX_LANE_TASK_CAPACITY (2 for the default 64). */
    const uint32_t trip = (2u * ASX_LANE_TASK_CAPACITY + 99u) / 100u;
    asx_parallel_config cfg = default_config();
    asx_region_id rid;
    asx_task_id tids[ASX_LANE_TASK_CAPACITY];
    asx_scheduling_metrics metrics;
    uint32_t i;

    cfg.admission_policy.mode = ASX_PARALLEL_ADMISSION_BACKPRESSURE;
    cfg.admission_policy.pressure_threshold_pct = 2u;
    cfg.admission_policy.enforce = 1;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    for (i = 0u; i < trip; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tids[i]), ASX_OK);
    }

    for (i = 0u; i + 1u < trip; i++) {
        ASSERT_EQ(asx_lane_assign(tids[i], ASX_LANE_READY), ASX_OK);
    }
    ASSERT_EQ(asx_lane_assign(tids[trip - 1u], ASX_LANE_READY), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_lane_total_tasks(), trip - 1u);

    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_OK);
    ASSERT_EQ(metrics.admission_backpressure, 1u);
    ASSERT_EQ(metrics.pressure_transitions, 1u);

    asx_parallel_reset();
}

TEST(parallel_telemetry_snapshot_and_jsonl_are_failure_atomic) {
    asx_parallel_config cfg = default_config();
    asx_parallel_telemetry_snapshot snapshot;
    asx_region_id rid;
    asx_task_id t1, t2;
    char tiny[8] = "keep";
    char jsonl[2048];
    uint32_t needed = 0u;

    cfg.worker_count = 4u;
    cfg.admission_policy.mode = ASX_PARALLEL_ADMISSION_SHED_OLDEST;
    cfg.admission_policy.pressure_threshold_pct = 1u;
    cfg.admission_policy.shed_max = 2u;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_lane_assign(t1, ASX_LANE_READY), ASX_OK);
    ASSERT_EQ(asx_lane_assign(t2, ASX_LANE_READY), ASX_OK);

    ASSERT_EQ(asx_parallel_get_telemetry_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ(snapshot.worker_count, 4u);
    ASSERT_EQ(snapshot.total_queue_depth, 2u);
    ASSERT_TRUE(snapshot.pressure_pct >= 3u);
    ASSERT_EQ(snapshot.admission.triggered, 1);
    ASSERT_EQ((int)snapshot.admission.mode, (int)ASX_PARALLEL_ADMISSION_SHED_OLDEST);
    ASSERT_EQ(snapshot.metrics.admission_sheds, 3u);
    ASSERT_EQ(snapshot.commit_authority.commit_sequence, 0u);
    ASSERT_EQ(snapshot.commit_authority.total_worker_commits, 0u);
    ASSERT_EQ(snapshot.commit_authority.drift_detected, 0u);

    ASSERT_EQ(asx_parallel_render_telemetry_jsonl(&snapshot, tiny, (uint32_t)sizeof(tiny), &needed),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_STR_EQ(tiny, "keep");
    ASSERT_TRUE(needed > (uint32_t)sizeof(tiny));

    memset(jsonl, 0, sizeof(jsonl));
    ASSERT_EQ(asx_parallel_render_telemetry_jsonl(&snapshot, jsonl, (uint32_t)sizeof(jsonl),
                                                  &needed),
              ASX_OK);
    ASSERT_TRUE(strstr(jsonl, "\"kind\":\"parallel_telemetry\"") != NULL);
    ASSERT_TRUE(strstr(jsonl, "\"admission\"") != NULL);
    ASSERT_TRUE(strstr(jsonl, "\"shed_oldest\"") != NULL);
    ASSERT_TRUE(strstr(jsonl, "\"commit_authority\"") != NULL);
    ASSERT_TRUE(strstr(jsonl, "\"native_live_enabled\":0") != NULL);

    asx_parallel_reset();
}

TEST(parallel_commit_authority_snapshot_tracks_single_commit_stream) {
    asx_parallel_config cfg = default_config();
    asx_parallel_telemetry_snapshot snapshot;
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    int c1 = 2;
    int c2 = 1;
    int c3 = 0;

    cfg.worker_count = 4u;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;
    cfg.locality.shard_count = 4u;
    cfg.locality.tasks_per_shard = 1u;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);

    budget = asx_budget_from_polls(100u);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_parallel_get_telemetry_snapshot(&snapshot), ASX_OK);

    ASSERT_TRUE(snapshot.commit_authority.commit_sequence > 0u);
    ASSERT_EQ(snapshot.commit_authority.total_worker_commits,
              snapshot.commit_authority.commit_sequence);
    ASSERT_TRUE(snapshot.commit_authority.max_worker_commit_sequence <
                snapshot.commit_authority.commit_sequence);
    ASSERT_EQ(snapshot.commit_authority.drift_detected, 0u);
    ASSERT_EQ(snapshot.commit_authority.native_live_enabled, 0u);
    ASSERT_TRUE(snapshot.commit_authority.native_live_status != ASX_OK);

    asx_parallel_reset();
}

/* ================================================================
 * Lane order, EDF timed lane, parked tasks (bd-9kll.4.7)
 *
 * Rust's three-lane scheduler dispatches cancel, then timed (earliest
 * deadline first, and only once due), then ready (three_lane.rs:3,
 * priority.rs:54-79, 546-560), and polls a task again only when woken.
 * ================================================================ */

typedef struct {
    int id;
    int polls;
    int yield_polls;    /* ASX_E_PENDING for this many polls, then ASX_OK */
    asx_task_id victim; /* cancelled by this task's first poll */
} order_task;

static asx_status poll_order_yield_cancel(void *data, asx_task_id self) {
    order_task *s = (order_task *)data;
    (void)self;
    if (g_poll_order_count < 8u) {
        g_poll_order[g_poll_order_count] = s->id;
        g_poll_order_count++;
    }
    s->polls++;
    if (s->victim != ASX_INVALID_ID) {
        asx_status st = asx_task_cancel(s->victim, ASX_CANCEL_USER);
        (void)st;
        s->victim = ASX_INVALID_ID;
    }
    return s->polls <= s->yield_polls ? ASX_E_PENDING : ASX_OK;
}

static asx_budget budget_with_deadline(asx_time deadline) {
    asx_budget b = asx_budget_new();
    b.deadline = deadline;
    return b;
}

TEST(parallel_lanes_dispatch_cancel_then_timed_then_ready) {
    asx_region_id rid;
    asx_task_id ready_t, timed_t, cancel_t;
    asx_budget budget;
    asx_budget due = budget_with_deadline(0u);
    asx_parallel_config cfg = default_config();
    poll_order_state ready_s = {1};
    poll_order_state timed_s = {2};
    poll_order_state cancel_s = {3};

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    /* The ready task is spawned first and the cancel task last, so any
     * arena-order or ready-first dispatch shows. */
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &ready_s, &ready_t), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_record_order, &timed_s, &due, &timed_t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &cancel_s, &cancel_t), ASX_OK);
    ASSERT_EQ(asx_inject_timed(timed_t), ASX_OK);
    ASSERT_EQ(asx_task_cancel(cancel_t, ASX_CANCEL_USER), ASX_OK);

    budget = asx_budget_from_polls(30);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(g_poll_order_count, 3u);
    ASSERT_EQ(g_poll_order[0], 3); /* cancel */
    ASSERT_EQ(g_poll_order[1], 2); /* timed, due */
    ASSERT_EQ(g_poll_order[2], 1); /* ready */

    asx_parallel_reset();
}

TEST(parallel_timed_lane_dispatches_earliest_deadline_first) {
    asx_region_id rid;
    asx_task_id t[4];
    asx_budget budget;
    asx_budget b30 = budget_with_deadline(30u);
    asx_budget b10 = budget_with_deadline(10u);
    asx_budget b20 = budget_with_deadline(20u);
    asx_parallel_config cfg = default_config();
    poll_order_state s[4];
    uint32_t i;

    reset_all();
    for (i = 0; i < 4u; i++) { s[i].id = (int)i + 1; }
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    /* Deadlines 30, 10, 20, 10: injected in that order. */
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_record_order, &s[0], &b30, &t[0]), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_record_order, &s[1], &b10, &t[1]), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_record_order, &s[2], &b20, &t[2]), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_record_order, &s[3], &b10, &t[3]), ASX_OK);
    for (i = 0; i < 4u; i++) { ASSERT_EQ(asx_inject_timed(t[i]), ASX_OK); }

    /* Every deadline is due. */
    ASSERT_TRUE(asx_runtime_clock_is_virtual());
    asx_runtime_virtual_advance(test_runtime(), 30u);

    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(g_poll_order_count, 4u);
    ASSERT_EQ(g_poll_order[0], 2); /* deadline 10, injected first of the two */
    ASSERT_EQ(g_poll_order[1], 4); /* deadline 10, injected second */
    ASSERT_EQ(g_poll_order[2], 3); /* deadline 20 */
    ASSERT_EQ(g_poll_order[3], 1); /* deadline 30 */

    asx_parallel_reset();
}

TEST(parallel_timed_task_waits_for_its_deadline) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_budget b100 = budget_with_deadline(100u);
    asx_parallel_config cfg = default_config();
    poll_order_state s = {1};

    reset_all();
    ASSERT_TRUE(asx_runtime_clock_is_virtual());
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_record_order, &s, &b100, &tid), ASX_OK);
    ASSERT_EQ(asx_inject_timed(tid), ASX_OK);

    /* The clock is at 0: the entry is not due, and nothing else can run. */
    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(g_poll_order_count, 0u);

    asx_runtime_virtual_advance(test_runtime(), 99u);
    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(g_poll_order_count, 0u);

    asx_runtime_virtual_advance(test_runtime(), 100u);
    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 1u);
    ASSERT_EQ(g_poll_order[0], 1);

    asx_parallel_reset();
}

TEST(parallel_timed_task_with_armed_timer_runs_at_the_timer) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    poll_order_state s = {1};

    reset_all();
    ASSERT_TRUE(asx_runtime_clock_is_virtual());
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &s, &tid), ASX_OK);
    ASSERT_EQ(asx_task_arm_timer(tid, 50u), ASX_OK);
    ASSERT_EQ(asx_inject_timed(tid), ASX_OK);

    /* Nothing is runnable at time 0; the run idles, the virtual clock
     * jumps to the timer, and the task is due. Its fired timer has left
     * the heap, so the deadline it carried into the lane is what keeps it
     * due. */
    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 1u);
    ASSERT_EQ(asx_runtime_virtual_now(test_runtime()), (asx_time)50u);

    asx_parallel_reset();
}

TEST(parallel_timed_dispatch_consumes_the_lane_entry) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_budget due = budget_with_deadline(0u);
    asx_parallel_config cfg = default_config();
    asx_lane_state ls;
    asx_scheduling_metrics metrics;
    order_task s;

    reset_all();
    memset(&s, 0, sizeof(s));
    s.id = 1;
    s.yield_polls = 5;
    s.victim = ASX_INVALID_ID;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_order_yield_cancel, &s, &due, &tid), ASX_OK);
    ASSERT_EQ(asx_inject_timed(tid), ASX_OK);

    /* Two polls: the timed dispatch, then the task as ready work. */
    budget = asx_budget_from_polls(2);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(s.polls, 2);

    ASSERT_EQ(asx_lane_get_state(ASX_LANE_TIMED, &ls), ASX_OK);
    ASSERT_EQ(ls.task_count, 0u);
    ASSERT_EQ(asx_lane_get_state(ASX_LANE_READY, &ls), ASX_OK);
    ASSERT_EQ(ls.task_count, 1u);
    ASSERT_EQ(asx_parallel_get_metrics(&metrics), ASX_OK);
    ASSERT_EQ(metrics.timed_dispatches, 1u);
    ASSERT_EQ(metrics.ready_dispatches, 1u);

    asx_parallel_reset();
}

TEST(parallel_cancel_requested_mid_run_is_dispatched_from_the_cancel_lane) {
    asx_region_id rid;
    asx_task_id killer_t, y_t, z_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    order_task killer;
    order_task y;
    order_task z;

    reset_all();
    memset(&killer, 0, sizeof(killer));
    memset(&y, 0, sizeof(y));
    memset(&z, 0, sizeof(z));
    killer.id = 3;
    killer.victim = ASX_INVALID_ID;
    y.id = 1;
    y.yield_polls = 2;
    y.victim = ASX_INVALID_ID;
    z.id = 2;
    z.yield_polls = 2;
    z.victim = ASX_INVALID_ID;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_order_yield_cancel, &killer, &killer_t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_order_yield_cancel, &y, &y_t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_order_yield_cancel, &z, &z_t), ASX_OK);
    killer.victim = z_t;

    budget = asx_budget_from_polls(30);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    /* Round 0: killer (cancels z), y, z. From round 1 on z is cancel
     * work: it goes before y. Left in the ready lane it would follow y. */
    ASSERT_EQ(g_poll_order_count, 7u);
    ASSERT_EQ(g_poll_order[0], 3);
    ASSERT_EQ(g_poll_order[1], 1);
    ASSERT_EQ(g_poll_order[2], 2);
    ASSERT_EQ(g_poll_order[3], 2);
    ASSERT_EQ(g_poll_order[4], 1);
    ASSERT_EQ(g_poll_order[5], 2);
    ASSERT_EQ(g_poll_order[6], 1);

    asx_parallel_reset();
}

typedef struct {
    int polls;
    int go;
} park_state;

/* Waits for `go`: parks itself after arranging nothing but the host's
 * wake (asx_task_wake from outside the run). */
static asx_status poll_park_until_go(void *data, asx_task_id self) {
    park_state *s = (park_state *)data;
    asx_status st;
    s->polls++;
    if (s->go) { return ASX_OK; }
    st = asx_task_park(self);
    (void)st;
    return ASX_E_PENDING;
}

/* Wakes itself, then parks: the wake arrived before the poll returned, so
 * the park is void and the task stays runnable. */
static asx_status poll_wake_self_then_park(void *data, asx_task_id self) {
    park_state *s = (park_state *)data;
    asx_status st;
    s->polls++;
    if (s->polls >= 2) { return ASX_OK; }
    st = asx_task_wake(self);
    (void)st;
    st = asx_task_park(self);
    (void)st;
    return ASX_E_PENDING;
}

TEST(parallel_parked_task_is_not_polled_until_woken) {
    asx_region_id rid;
    asx_task_id parked_t, done_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    park_state s;

    reset_all();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_park_until_go, &s, &parked_t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &done_t), ASX_OK);

    /* The parked task is polled once; the other completes; then nothing
     * can wake the parked one, which the run reports instead of spinning
     * its poll budget away. */
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(s.polls, 1);
    ASSERT_EQ(asx_budget_polls(&budget), 48u);

    /* Still parked on another run: no poll. */
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(s.polls, 1);

    /* A wake makes it runnable again. */
    s.go = 1;
    ASSERT_EQ(asx_task_wake(parked_t), ASX_OK);
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s.polls, 2);

    asx_parallel_reset();
}

TEST(parallel_wake_before_the_poll_returns_voids_the_park) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    park_state s;

    reset_all();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_wake_self_then_park, &s, &tid), ASX_OK);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s.polls, 2);

    asx_parallel_reset();
}

typedef struct {
    int polls;
    asx_time deadline;
} sleep_state;

static asx_status poll_sleep_until(void *data, asx_task_id self) {
    sleep_state *s = (sleep_state *)data;
    s->polls++;
    return asx_task_wait_until(self, s->deadline);
}

TEST(parallel_sleeping_task_is_woken_by_its_timer) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    sleep_state s;

    reset_all();
    ASSERT_TRUE(asx_runtime_clock_is_virtual());
    s.polls = 0;
    s.deadline = 50u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_sleep_until, &s, &tid), ASX_OK);

    /* First poll arms the timer and parks; the run idles, the virtual
     * clock jumps to the deadline, the timer wakes the task, and its
     * second poll completes. Two polls, however large the budget. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s.polls, 2);
    ASSERT_EQ(asx_runtime_virtual_now(test_runtime()), (asx_time)50u);

    asx_parallel_reset();
}

TEST(parallel_run_stops_at_the_budget_deadline_while_idle) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    sleep_state s;

    reset_all();
    ASSERT_TRUE(asx_runtime_clock_is_virtual());
    s.polls = 0;
    s.deadline = 1000u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_sleep_until, &s, &tid), ASX_OK);

    /* The sleeper wakes at 1000; the run's own deadline is 200. */
    budget = asx_budget_from_polls(100);
    budget.deadline = 200u;
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_TIMED_OUT);
    ASSERT_EQ(s.polls, 1);

    asx_parallel_reset();
}

/* ---- Primitives that park the current task ---------------------------
 *
 * Channels, mutexes and the like park their caller through
 * asx_task_current(). A parallel poll is the current task, as the sweep's
 * is, so a task blocked in one is skipped until the state change that
 * satisfies it wakes it. */

typedef struct {
    asx_channel_id ch;
    uint32_t polls;
    uint32_t got;
    uint64_t value;
} chan_rx_state;

static asx_status poll_chan_receiver(void *data, asx_task_id self) {
    chan_rx_state *s = (chan_rx_state *)data;
    asx_status st;
    (void)self;
    s->polls++;
    st = asx_channel_recv(s->ch, NULL, &s->value); /* parks when empty */
    if (st == ASX_E_PENDING) { return ASX_E_PENDING; }
    if (st == ASX_OK) { s->got++; }
    return ASX_OK;
}

typedef struct {
    asx_channel_id ch;
    int delay; /* polls spent yielding before the send */
} chan_tx_state;

static asx_status poll_chan_sender(void *data, asx_task_id self) {
    chan_tx_state *s = (chan_tx_state *)data;
    asx_status st;
    (void)self;
    if (s->delay > 0) {
        s->delay--;
        return ASX_E_PENDING; /* a yield: stays runnable */
    }
    st = asx_channel_send(s->ch, NULL, 42u);
    return st;
}

TEST(parallel_task_blocked_in_channel_recv_is_not_polled_until_a_send_wakes_it) {
    asx_region_id rid;
    asx_task_id rx_t, tx_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    chan_rx_state rx;
    chan_tx_state tx;

    reset_all();
    memset(&rx, 0, sizeof(rx));
    memset(&tx, 0, sizeof(tx));
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &rx.ch), ASX_OK);
    tx.ch = rx.ch;
    tx.delay = 5;
    /* The receiver is first in arena order, so it finds the channel empty. */
    ASSERT_EQ(asx_task_spawn(rid, poll_chan_receiver, &rx, &rx_t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_chan_sender, &tx, &tx_t), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    /* Two receiver polls however long the sender yields: the one that
     * parked it, and the one the send woke. Busy-polled it would be six. */
    ASSERT_EQ(rx.polls, 2u);
    ASSERT_EQ(rx.got, 1u);
    ASSERT_EQ(rx.value, (uint64_t)42);

    asx_parallel_reset();
}

TEST(parallel_blocked_receiver_with_no_sender_would_block_until_a_send) {
    asx_region_id rid;
    asx_task_id rx_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    chan_rx_state rx;

    reset_all();
    memset(&rx, 0, sizeof(rx));
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 4, &rx.ch), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_chan_receiver, &rx, &rx_t), ASX_OK);

    /* Nothing can wake it: one poll, then the run says so instead of
     * spending the budget. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(rx.polls, 1u);
    ASSERT_EQ(asx_budget_polls(&budget), 99u);

    /* A send from outside the run wakes it. */
    {
        asx_send_permit permit;
        ASSERT_EQ(asx_channel_try_reserve(rx.ch, &permit), ASX_OK);
        ASSERT_EQ(asx_send_permit_send(&permit, 5u), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(rx.polls, 2u);
    ASSERT_EQ(rx.value, (uint64_t)5);

    asx_parallel_reset();
}

typedef struct {
    asx_region_id region;
    asx_task_id child;
    int child_polls;
    int parent_polls;
} spawner_state;

/* Acknowledges a cancel and still returns normally. */
static asx_status poll_child_ack_then_ok(void *data, asx_task_id self) {
    int *polls = (int *)data;
    asx_checkpoint_result cr;
    (*polls)++;
    if (*polls == 1) { return ASX_E_PENDING; } /* first poll: no cancel yet */
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

/* First poll: spawn the child and yield. Second: cancel it and finish. */
static asx_status poll_spawn_then_cancel_child(void *data, asx_task_id self) {
    spawner_state *s = (spawner_state *)data;
    asx_status st;
    (void)self;
    s->parent_polls++;
    if (s->parent_polls == 1) {
        st = asx_task_spawn(s->region, poll_child_ack_then_ok, &s->child_polls, &s->child);
        if (st != ASX_OK) { return st; }
        return ASX_E_PENDING;
    }
    return asx_task_cancel(s->child, ASX_CANCEL_USER);
}

TEST(parallel_spawned_child_that_acknowledges_a_cancel_and_returns_keeps_its_value) {
    /* Rust's classify_spawn_completion: a child spawned from a poll that
     * acknowledged a cancel after its first poll and then returns Ok
     * completes Ok, not Cancelled. The policy reads the task's state, so
     * it has to be read before the completion walk moves it. */
    asx_region_id rid;
    asx_task_id parent_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    spawner_state s;
    asx_outcome out;

    reset_all();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    s.region = rid;
    ASSERT_EQ(asx_task_spawn(rid, poll_spawn_then_cancel_child, &s, &parent_t), ASX_OK);

    /* One run: the parent spawns the child (polled this round, once,
     * uncancelled), cancels it next round, and the child acknowledges at
     * its checkpoint and returns. */
    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s.child_polls, 2);
    ASSERT_EQ(asx_task_get_outcome(s.child, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_OK);

    asx_parallel_reset();
}

/* ---- Tasks spawned during a run (bd-9kll.4.11) ------------------------
 *
 * A run classifies the region's tasks once, at its start; a task a poll
 * spawns joins a lane as it is created. */

typedef struct {
    asx_region_id region;
    int child_yields; /* the child yields this many polls before it ends */
    int child_polls;
    asx_task_id child;
} spawn_and_finish_state;

static asx_status poll_yield_counted(void *data, asx_task_id self) {
    spawn_and_finish_state *s = (spawn_and_finish_state *)data;
    (void)self;
    s->child_polls++;
    return s->child_polls <= s->child_yields ? ASX_E_PENDING : ASX_OK;
}

/* Spawns the child and completes at once. */
static asx_status poll_spawn_child_and_finish(void *data, asx_task_id self) {
    spawn_and_finish_state *s = (spawn_and_finish_state *)data;
    (void)self;
    return asx_task_spawn(s->region, poll_yield_counted, s, &s->child);
}

TEST(parallel_run_does_not_return_ok_while_a_spawned_child_is_live) {
    asx_region_id rid;
    asx_task_id parent_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    spawn_and_finish_state s;
    asx_task_state state;

    reset_all();
    memset(&s, 0, sizeof(s));
    s.child_yields = 3;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    s.region = rid;
    ASSERT_EQ(asx_task_spawn(rid, poll_spawn_child_and_finish, &s, &parent_t), ASX_OK);

    /* The parent is done after its first poll. The run must still poll the
     * child to its end before it says OK (without admission it returned OK
     * with the child never polled). */
    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(s.child_polls, 4);
    ASSERT_EQ(asx_task_get_state(s.child, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_lane_total_tasks(), 0u);

    asx_parallel_reset();
}

typedef struct {
    asx_region_id region;
    order_task a;
    order_task b;
    asx_task_id a_id;
    asx_task_id b_id;
    int polls;
} family_state;

static asx_status poll_spawn_family(void *data, asx_task_id self) {
    family_state *s = (family_state *)data;
    asx_status st;
    (void)self;
    s->polls++;
    if (s->polls > 1) { return ASX_OK; }
    st = asx_task_spawn(s->region, poll_order_yield_cancel, &s->a, &s->a_id);
    if (st != ASX_OK) { return st; }
    st = asx_task_spawn(s->region, poll_order_yield_cancel, &s->b, &s->b_id);
    if (st != ASX_OK) { return st; }
    st = asx_task_cancel(s->b_id, ASX_CANCEL_USER);
    if (st != ASX_OK) { return st; }
    return ASX_E_PENDING;
}

TEST(parallel_child_cancelled_after_admission_is_dispatched_from_the_cancel_lane) {
    asx_region_id rid;
    asx_task_id parent_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    family_state s;

    reset_all();
    memset(&s, 0, sizeof(s));
    s.a.id = 1;
    s.a.yield_polls = 1;
    s.a.victim = ASX_INVALID_ID;
    s.b.id = 2;
    s.b.yield_polls = 1;
    s.b.victim = ASX_INVALID_ID;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    s.region = rid;
    ASSERT_EQ(asx_task_spawn(rid, poll_spawn_family, &s, &parent_t), ASX_OK);

    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    /* Round 0: the parent spawns a and b and cancels b; both are admitted
     * and polled in order. Round 1: b is cancel work and goes before a. */
    ASSERT_EQ(g_poll_order_count, 4u);
    ASSERT_EQ(g_poll_order[0], 1);
    ASSERT_EQ(g_poll_order[1], 2);
    ASSERT_EQ(g_poll_order[2], 2);
    ASSERT_EQ(g_poll_order[3], 1);

    asx_parallel_reset();
}

TEST(parallel_spawn_outside_a_run_is_not_admitted_until_the_next_run) {
    asx_region_id rid;
    asx_task_id first_t, second_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    poll_order_state s1 = {1};
    poll_order_state s2 = {2};

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &s1, &first_t), ASX_OK);
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 1u);

    /* No run is in progress: the spawn path must not touch the lanes. */
    ASSERT_EQ(asx_task_spawn(rid, poll_record_order, &s2, &second_t), ASX_OK);
    ASSERT_EQ(asx_lane_total_tasks(), 0u);
    ASSERT_EQ(g_poll_order_count, 1u);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);
    ASSERT_EQ(g_poll_order_count, 2u);
    ASSERT_EQ(g_poll_order[1], 2);

    asx_parallel_reset();
}

/* A region the run cancels in a poll: a spawn into it is refused, as Rust's
 * RegionState::can_accept_work refuses a closing region, so a spawn into a
 * cancelled region puts nothing in any lane. Children spawned before the
 * cancel, in the same poll, are queued ready and take the region's cancel;
 * the lanes are classified again at the start of the next round, which
 * moves them to the cancel lane. A child yields on its first poll and, on
 * its second, reads the lanes: a poll leaves a pending task in its lane
 * and removes a completed one, so the lanes hold the polled child and the
 * one after it. */

typedef struct {
    int polls;
    uint32_t cancel_lane; /* task counts at its second poll */
    uint32_t ready_lane;
} cancel_region_child;

typedef struct {
    asx_region_id region;
    int polls;
    int spawn_before_cancel; /* spawn, then cancel (else cancel, then spawn) */
    asx_status spawn_status;
    asx_status spawn_status2;
    asx_task_id spawned[2];
    uint32_t lanes_before_spawn;
    uint32_t lanes_after_spawn;
    cancel_region_child child[2];
} cancel_region_state;

static asx_status poll_cancel_region_child(void *data, asx_task_id self) {
    cancel_region_child *c = (cancel_region_child *)data;
    asx_lane_state ls;
    asx_checkpoint_result cr;
    c->polls++;
    if (c->polls == 1) { return ASX_E_PENDING; }
    if (asx_lane_get_state(ASX_LANE_CANCEL, &ls) == ASX_OK) { c->cancel_lane = ls.task_count; }
    if (asx_lane_get_state(ASX_LANE_READY, &ls) == ASX_OK) { c->ready_lane = ls.task_count; }
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

static asx_status poll_cancel_region_parent(void *data, asx_task_id self) {
    cancel_region_state *s = (cancel_region_state *)data;
    asx_cancel_reason reason = asx_cancel_reason_default(test_runtime(), ASX_CANCEL_USER, NULL);
    asx_checkpoint_result cr;
    asx_status st;

    s->polls++;
    if (s->polls > 1) {
        if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
        return ASX_E_PENDING;
    }
    s->spawned[0] = ASX_INVALID_ID;
    s->spawned[1] = ASX_INVALID_ID;
    s->lanes_before_spawn = asx_lane_total_tasks();
    if (s->spawn_before_cancel) {
        s->spawn_status =
            asx_task_spawn(s->region, poll_cancel_region_child, &s->child[0], &s->spawned[0]);
        s->spawn_status2 =
            asx_task_spawn(s->region, poll_cancel_region_child, &s->child[1], &s->spawned[1]);
        s->lanes_after_spawn = asx_lane_total_tasks();
        st = asx_region_cancel(s->region, &reason, NULL);
    } else {
        st = asx_region_cancel(s->region, &reason, NULL);
        s->spawn_status =
            asx_task_spawn(s->region, poll_cancel_region_child, &s->child[0], &s->spawned[0]);
        s->spawn_status2 =
            asx_task_spawn(s->region, poll_cancel_region_child, &s->child[1], &s->spawned[1]);
        s->lanes_after_spawn = asx_lane_total_tasks();
    }
    if (st != ASX_OK) { return st; }
    return ASX_E_PENDING;
}

TEST(parallel_spawn_into_a_cancelled_region_is_refused_and_admits_nothing) {
    asx_region_id rid;
    asx_task_id parent_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    cancel_region_state s;
    asx_region_state rstate;

    reset_all();
    memset(&s, 0, sizeof(s));
    s.spawn_before_cancel = 0;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    s.region = rid;
    ASSERT_EQ(asx_task_spawn(rid, poll_cancel_region_parent, &s, &parent_t), ASX_OK);

    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    /* Refused, with the handle left as it was, and no lane took anything. */
    ASSERT_EQ((int)s.spawn_status, (int)ASX_E_REGION_CLOSED);
    ASSERT_EQ((int)s.spawn_status2, (int)ASX_E_REGION_CLOSED);
    ASSERT_EQ(s.spawned[0], ASX_INVALID_ID);
    ASSERT_EQ(s.spawned[1], ASX_INVALID_ID);
    ASSERT_EQ(s.lanes_after_spawn, s.lanes_before_spawn);
    ASSERT_EQ(s.child[0].polls, 0);
    ASSERT_EQ(s.child[1].polls, 0);
    ASSERT_EQ(asx_lane_total_tasks(), 0u);
    ASSERT_EQ(asx_region_get_state(rid, &rstate), ASX_OK);
    ASSERT_EQ((int)rstate, (int)ASX_REGION_CLOSED);

    asx_parallel_reset();
}

TEST(parallel_child_spawned_then_region_cancelled_is_dispatched_from_the_cancel_lane) {
    asx_region_id rid;
    asx_task_id parent_t;
    asx_budget budget;
    asx_parallel_config cfg = default_config();
    cancel_region_state s;
    asx_task_state cstate;

    reset_all();
    memset(&s, 0, sizeof(s));
    s.spawn_before_cancel = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    s.region = rid;
    ASSERT_EQ(asx_task_spawn(rid, poll_cancel_region_parent, &s, &parent_t), ASX_OK);

    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    /* Both admitted at their spawns (two more tasks in the lanes), then
     * cancelled by the region's cancel in the same poll. */
    ASSERT_EQ((int)s.spawn_status, (int)ASX_OK);
    ASSERT_EQ((int)s.spawn_status2, (int)ASX_OK);
    ASSERT_EQ(s.lanes_after_spawn, s.lanes_before_spawn + 2u);
    /* Each child yielded once and was polled again: the first, with both
     * children in the cancel lane and the ready lane empty (the parent had
     * completed); the second, with itself alone. A queued task whose
     * cancel never moved it would be in the ready lane instead. */
    ASSERT_EQ(s.child[0].polls, 2);
    ASSERT_EQ(s.child[1].polls, 2);
    ASSERT_EQ(s.child[0].cancel_lane, 2u);
    ASSERT_EQ(s.child[0].ready_lane, 0u);
    ASSERT_EQ(s.child[1].cancel_lane, 1u);
    ASSERT_EQ(s.child[1].ready_lane, 0u);
    ASSERT_EQ(asx_task_get_state(s.spawned[0], &cstate), ASX_OK);
    ASSERT_EQ((int)cstate, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(s.spawned[1], &cstate), ASX_OK);
    ASSERT_EQ((int)cstate, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_lane_total_tasks(), 0u);

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
    RUN_TEST_IF(ASX_LANE_TASK_CAPACITY <= ASX_MAX_TASKS, lane_assign_fills_capacity,
                "a lane holds more tasks than the task arena (classed build)");
    RUN_TEST(lane_get_state_null_out);
    RUN_TEST(lane_get_state_invalid_class);

    /* Worker state. Multi-worker tests run only on profiles whose
     * ASX_MAX_WORKERS can host them (FREESTANDING/BROWSER have 1). */
    RUN_TEST_IF(ASX_MAX_WORKERS >= 2u, worker_get_state_valid, "needs 2 workers");
    RUN_TEST(worker_get_state_out_of_range);
    RUN_TEST(worker_get_state_null_out);
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u, worker_lifecycle_drains_on_quiescence, "needs 4 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 2u, worker_lifecycle_marks_draining_on_budget_exhaustion,
                "needs 2 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u, worker_lane_depths_track_manual_injection,
                "needs 4 workers");

    /* Parallel run */
    RUN_TEST(parallel_run_single_task_completes);
    RUN_TEST(parallel_run_task_yields_then_completes);
    RUN_TEST(parallel_run_cancelled_task_sets_completed_cancel_phase);
    RUN_TEST(parallel_run_finalizing_task_sets_completed_cancel_phase);
    RUN_TEST(parallel_run_task_fails);
    RUN_TEST(parallel_cancelled_task_returning_its_error_is_not_a_fault);
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

    /* Timed lane / wakers / reactor readiness */
    RUN_TEST_IF(ASX_MAX_WORKERS >= 2u, timed_lane_waits_for_waker_before_polling,
                "needs 2 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 2u, timed_equal_deadline_wakers_preserve_fire_order,
                "needs 2 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 2u, reactor_readiness_promotes_timed_waker, "needs 2 workers");

    /* Lane order (cancel > timed > ready), EDF timed lane, parked tasks */
    RUN_TEST(parallel_lanes_dispatch_cancel_then_timed_then_ready);
    RUN_TEST(parallel_timed_lane_dispatches_earliest_deadline_first);
    RUN_TEST(parallel_timed_task_waits_for_its_deadline);
    RUN_TEST(parallel_timed_task_with_armed_timer_runs_at_the_timer);
    RUN_TEST(parallel_timed_dispatch_consumes_the_lane_entry);
    RUN_TEST(parallel_cancel_requested_mid_run_is_dispatched_from_the_cancel_lane);
    RUN_TEST(parallel_parked_task_is_not_polled_until_woken);
    RUN_TEST(parallel_wake_before_the_poll_returns_voids_the_park);
    RUN_TEST(parallel_sleeping_task_is_woken_by_its_timer);
    RUN_TEST(parallel_run_stops_at_the_budget_deadline_while_idle);
    RUN_TEST(parallel_task_blocked_in_channel_recv_is_not_polled_until_a_send_wakes_it);
    RUN_TEST(parallel_blocked_receiver_with_no_sender_would_block_until_a_send);
    RUN_TEST(parallel_spawned_child_that_acknowledges_a_cancel_and_returns_keeps_its_value);
    RUN_TEST(parallel_run_does_not_return_ok_while_a_spawned_child_is_live);
    RUN_TEST(parallel_child_cancelled_after_admission_is_dispatched_from_the_cancel_lane);
    RUN_TEST(parallel_spawn_outside_a_run_is_not_admitted_until_the_next_run);
    RUN_TEST(parallel_spawn_into_a_cancelled_region_is_refused_and_admits_nothing);
    RUN_TEST(parallel_child_spawned_then_region_cancelled_is_dispatched_from_the_cancel_lane);

    /* Worker routing */
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u, parallel_multi_worker_steals_preserve_completion,
                "needs 4 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u, parallel_multi_worker_trace_matches_single_worker,
                "needs 4 workers");

    /* Replay identity */
    RUN_TEST(parallel_replay_identity);
    RUN_TEST(parallel_single_worker_trace_matches_core_scheduler);

    /* Multi-worker */
    RUN_TEST(parallel_multi_worker_init);
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u, parallel_locality_default_compact_snapshot,
                "needs 4 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u && ASX_PARALLEL_HAS_LOCALITY_SHARDING,
                parallel_worker_sharded_locality_routes_by_contiguous_slot_ranges,
                "needs 4 workers and worker-sharded locality");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 8u && ASX_PARALLEL_HAS_LOCALITY_SHARDING,
                parallel_worker_sharded_locality_defaults_to_worker_shards,
                "needs 8 workers and worker-sharded locality");
    RUN_TEST_IF(!ASX_PARALLEL_HAS_LOCALITY_SHARDING,
                parallel_sharded_request_falls_back_to_compact_without_sharding,
                "profile has worker-sharded locality");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 2u, parallel_task_locality_rejects_stale_handle,
                "needs 2 workers");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u, parallel_worker_sharded_trace_matches_compact,
                "needs 4 workers");

    /* Lane weight query */
    RUN_TEST(parallel_lane_weight_query);

    /* Admission and telemetry evidence */
    RUN_TEST(parallel_admission_evaluate_modes);
    RUN_TEST(parallel_admission_enforced_backpressure_is_atomic);
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u && ASX_LANE_TASK_CAPACITY <= 100u,
                parallel_telemetry_snapshot_and_jsonl_are_failure_atomic,
                "needs 4 workers; its shed counts assume one queued task reaches 1% of a lane");
    RUN_TEST_IF(ASX_MAX_WORKERS >= 4u,
                parallel_commit_authority_snapshot_tracks_single_commit_stream, "needs 4 workers");

    TEST_REPORT();
    return test_failures;
}
