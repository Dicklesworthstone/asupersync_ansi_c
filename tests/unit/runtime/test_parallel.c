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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_io_driver_init(), ASX_OK);

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    asx_trace_reset();
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 1;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    single_count = asx_trace_event_count();
    ASSERT_TRUE(single_count > 0u);
    ASSERT_TRUE(single_count <= 96u);
    for (i = 0; i < single_count; i++) { ASSERT_TRUE(asx_trace_event_get(i, &single_events[i])); }

    reset_all();
    asx_trace_reset();
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 4;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_trace_event_count(), single_count);
    for (i = 0; i < single_count; i++) {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(i, &ev));
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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0u; i < 8u; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_forever, NULL, &tids[i]), ASX_OK);
        ASSERT_EQ(asx_lane_assign(tids[i], ASX_LANE_READY), ASX_OK);
    }

    ASSERT_EQ(asx_parallel_get_locality_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ((int)snapshot.mode, (int)ASX_PARALLEL_LOCALITY_WORKER_SHARDED);
    ASSERT_EQ(snapshot.shard_count, 4u);
    ASSERT_EQ(snapshot.tasks_per_shard, 2u);
    ASSERT_EQ(snapshot.shard_task_counts[0], 2u);
    ASSERT_EQ(snapshot.shard_task_counts[1], 2u);
    ASSERT_EQ(snapshot.shard_task_counts[2], 2u);
    ASSERT_EQ(snapshot.shard_task_counts[3], 2u);
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

TEST(parallel_worker_sharded_locality_defaults_to_worker_shards) {
    asx_parallel_config cfg = default_config();
    asx_parallel_locality_snapshot snapshot;

    cfg.worker_count = 8u;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;

    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_parallel_get_locality_snapshot(&snapshot), ASX_OK);
    ASSERT_EQ((int)snapshot.mode, (int)ASX_PARALLEL_LOCALITY_WORKER_SHARDED);
    ASSERT_EQ(snapshot.shard_count, 8u);
    ASSERT_EQ(snapshot.tasks_per_shard, 8u);

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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    asx_trace_reset();
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 4;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    compact_count = asx_trace_event_count();
    ASSERT_TRUE(compact_count > 0u);
    ASSERT_TRUE(compact_count <= 96u);
    for (i = 0; i < compact_count; i++) { ASSERT_TRUE(asx_trace_event_get(i, &compact_events[i])); }

    reset_all();
    asx_trace_reset();
    c1 = 2;
    c2 = 1;
    c3 = 0;
    cfg.worker_count = 4;
    cfg.locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;
    cfg.locality.shard_count = 4u;
    cfg.locality.tasks_per_shard = 1u;
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c3, &t3), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_parallel_run(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_trace_event_count(), compact_count);
    for (i = 0; i < compact_count; i++) {
        asx_trace_event ev;
        ASSERT_TRUE(asx_trace_event_get(i, &ev));
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
    asx_parallel_config cfg = default_config();
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_scheduling_metrics metrics;

    cfg.admission_policy.mode = ASX_PARALLEL_ADMISSION_BACKPRESSURE;
    cfg.admission_policy.pressure_threshold_pct = 2u;
    cfg.admission_policy.enforce = 1;

    reset_all();
    ASSERT_EQ(asx_parallel_init(&cfg), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t2), ASX_OK);

    ASSERT_EQ(asx_lane_assign(t1, ASX_LANE_READY), ASX_OK);
    ASSERT_EQ(asx_lane_assign(t2, ASX_LANE_READY), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_lane_total_tasks(), 1u);

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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
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
    RUN_TEST(worker_lifecycle_drains_on_quiescence);
    RUN_TEST(worker_lifecycle_marks_draining_on_budget_exhaustion);
    RUN_TEST(worker_lane_depths_track_manual_injection);

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

    /* Timed lane / wakers / reactor readiness */
    RUN_TEST(timed_lane_waits_for_waker_before_polling);
    RUN_TEST(timed_equal_deadline_wakers_preserve_fire_order);
    RUN_TEST(reactor_readiness_promotes_timed_waker);

    /* Worker routing */
    RUN_TEST(parallel_multi_worker_steals_preserve_completion);
    RUN_TEST(parallel_multi_worker_trace_matches_single_worker);

    /* Replay identity */
    RUN_TEST(parallel_replay_identity);
    RUN_TEST(parallel_single_worker_trace_matches_core_scheduler);

    /* Multi-worker */
    RUN_TEST(parallel_multi_worker_init);
    RUN_TEST(parallel_locality_default_compact_snapshot);
    RUN_TEST(parallel_worker_sharded_locality_routes_by_contiguous_slot_ranges);
    RUN_TEST(parallel_worker_sharded_locality_defaults_to_worker_shards);
    RUN_TEST(parallel_task_locality_rejects_stale_handle);
    RUN_TEST(parallel_worker_sharded_trace_matches_compact);

    /* Lane weight query */
    RUN_TEST(parallel_lane_weight_query);

    /* Admission and telemetry evidence */
    RUN_TEST(parallel_admission_evaluate_modes);
    RUN_TEST(parallel_admission_enforced_backpressure_is_atomic);
    RUN_TEST(parallel_telemetry_snapshot_and_jsonl_are_failure_atomic);
    RUN_TEST(parallel_commit_authority_snapshot_tracks_single_commit_stream);

    TEST_REPORT();
    return test_failures;
}
