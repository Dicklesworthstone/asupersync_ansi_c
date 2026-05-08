/*
 * parallel.c — optional parallel profile worker model and lane scheduler
 *
 * Implements lane-based task scheduling with bounded fairness controls.
 * Deferred stub status: graduating-internal implementation. The core owns
 * deterministic worker-lane routing, bounded steal ordering, and replay-stable
 * event commits. Platform adapters may later execute worker lanes concurrently
 * only if they preserve this committed order.
 * See docs/DEFERRED_STUBS_REGISTER.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/asx_config.h>
#include <asx/core/cancel.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* -------------------------------------------------------------------
 * Lane internal state
 * ------------------------------------------------------------------- */

typedef struct {
    asx_task_id tasks[ASX_LANE_TASK_CAPACITY];
    uint32_t count;
    uint32_t polls_this_round;
    uint32_t starvation_count;
} lane_internal;

/* -------------------------------------------------------------------
 * Global parallel scheduler state
 * ------------------------------------------------------------------- */

static int g_initialized;
static asx_parallel_config g_config;
static lane_internal g_lanes[ASX_MAX_LANES];
static asx_worker_state g_workers[ASX_MAX_WORKERS];

#define ASX_PARALLEL_TASK_UNOWNED 0xffffu

static uint16_t g_task_worker_owner[ASX_MAX_TASKS];
static uint8_t g_task_lane_override_valid[ASX_MAX_TASKS];
static asx_lane_class g_task_lane_override[ASX_MAX_TASKS];
static uint32_t g_worker_lane_depths[ASX_MAX_WORKERS][ASX_MAX_LANES];
static uint32_t g_lane_dispatch_cursor[ASX_MAX_LANES];

/* Cancel-streak fairness state */
static uint32_t g_cancel_streak_limit = 16u;
static asx_scheduling_metrics g_metrics;

static int parallel_task_handle_valid(asx_task_id tid) {
    asx_task_slot *slot;
    return asx_task_slot_lookup(tid, &slot) == ASX_OK;
}

static void parallel_reset_routing(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        g_task_worker_owner[i] = ASX_PARALLEL_TASK_UNOWNED;
        g_task_lane_override_valid[i] = 0u;
        g_task_lane_override[i] = ASX_LANE_READY;
    }
    memset(g_worker_lane_depths, 0, sizeof(g_worker_lane_depths));
    memset(g_lane_dispatch_cursor, 0, sizeof(g_lane_dispatch_cursor));
}

static uint32_t parallel_active_worker_count(void) {
    return g_config.worker_count == 0u ? 1u : g_config.worker_count;
}

static uint32_t parallel_seed_task_owner(uint16_t slot_idx) {
    uint32_t worker_count = parallel_active_worker_count();
    uint32_t owner;
    if (slot_idx >= ASX_MAX_TASKS) { return 0u; }
    if (worker_count > ASX_MAX_WORKERS) { worker_count = ASX_MAX_WORKERS; }
    if (g_task_worker_owner[slot_idx] == ASX_PARALLEL_TASK_UNOWNED ||
        g_task_worker_owner[slot_idx] >= worker_count) {
        g_task_worker_owner[slot_idx] = (uint16_t)(slot_idx % worker_count);
    }
    owner = (uint32_t)g_task_worker_owner[slot_idx];
    return owner >= worker_count ? 0u : owner;
}

static void parallel_clear_task_routing(uint16_t slot_idx) {
    if (slot_idx >= ASX_MAX_TASKS) { return; }
    g_task_worker_owner[slot_idx] = ASX_PARALLEL_TASK_UNOWNED;
    g_task_lane_override_valid[slot_idx] = 0u;
    g_task_lane_override[slot_idx] = ASX_LANE_READY;
}

static void parallel_rebuild_worker_depths(void) {
    uint32_t lane_idx;
    memset(g_worker_lane_depths, 0, sizeof(g_worker_lane_depths));
    for (lane_idx = 0; lane_idx < ASX_MAX_LANES; lane_idx++) {
        lane_internal *lane = &g_lanes[lane_idx];
        uint32_t i;
        for (i = 0; i < lane->count;
             i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
            uint16_t slot_idx = asx_handle_slot(lane->tasks[i]);
            uint32_t owner = parallel_seed_task_owner(slot_idx);
            if (owner < ASX_MAX_WORKERS) { g_worker_lane_depths[owner][lane_idx]++; }
        }
    }
}

static uint32_t parallel_select_worker(asx_lane_class lane, asx_task_id tid) {
    uint32_t worker_count = parallel_active_worker_count();
    uint16_t slot_idx = asx_handle_slot(tid);
    uint32_t owner = parallel_seed_task_owner(slot_idx);
    uint32_t selected;

    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) { return owner; }
    if (worker_count <= 1u) { return 0u; }

    selected = g_lane_dispatch_cursor[(int)lane] % worker_count;
    g_lane_dispatch_cursor[(int)lane]++;

    if (selected != owner) {
        g_metrics.steal_attempts++;
        if (g_worker_lane_depths[owner][(int)lane] > 0u) {
            g_worker_lane_depths[owner][(int)lane]--;
            g_worker_lane_depths[selected][(int)lane]++;
            g_task_worker_owner[slot_idx] = (uint16_t)selected;
            g_metrics.steals_succeeded++;
            g_workers[selected].steals_total++;
            return selected;
        }
        g_metrics.worker_yields++;
    }

    return owner;
}

static void parallel_task_leaves_worker_lane(uint32_t worker_idx, asx_lane_class lane) {
    if (worker_idx >= ASX_MAX_WORKERS) { return; }
    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) { return; }
    if (g_worker_lane_depths[worker_idx][(int)lane] > 0u) {
        g_worker_lane_depths[worker_idx][(int)lane]--;
    }
}

static void parallel_commit_worker_event(uint32_t worker_idx) {
    if (worker_idx >= parallel_active_worker_count()) { worker_idx = 0u; }
    g_workers[worker_idx].commits_total++;
    g_workers[worker_idx].last_commit_sequence = g_metrics.commit_sequence;
    if (g_metrics.commit_sequence < UINT32_MAX) { g_metrics.commit_sequence++; }
}

static void parallel_mark_workers_running(void) {
    uint32_t i;
    for (i = 0; i < g_config.worker_count;
         i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_WORKERS validation") */
        g_workers[i].active = 1;
        g_workers[i].lifecycle = ASX_WORKER_RUNNING;
    }
}

static void parallel_mark_workers_drained(void) {
    uint32_t i;
    for (i = 0; i < g_config.worker_count;
         i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_WORKERS validation") */
        g_workers[i].active = 0;
        g_workers[i].lifecycle = ASX_WORKER_DRAINED;
    }
}

/* -------------------------------------------------------------------
 * Init / Reset
 * ------------------------------------------------------------------- */

asx_status asx_parallel_init(const asx_parallel_config *cfg) {
    uint32_t i;

    if (cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cfg->worker_count == 0) { return ASX_E_INVALID_ARGUMENT; }
    if (cfg->worker_count > ASX_MAX_WORKERS) { return ASX_E_RESOURCE_EXHAUSTED; }

    g_config = *cfg;

    /* Initialize lanes */
    for (i = 0; i < ASX_MAX_LANES; i++) { memset(&g_lanes[i], 0, sizeof(lane_internal)); }
    parallel_reset_routing();

    /* Initialize workers */
    for (i = 0; i < ASX_MAX_WORKERS; i++) {
        memset(&g_workers[i], 0, sizeof(g_workers[i]));
        g_workers[i].id = i;
        g_workers[i].lifecycle = ASX_WORKER_STOPPED;
    }
    for (i = 0; i < cfg->worker_count;
         i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_WORKERS checked above") */
        g_workers[i].id = i;
        g_workers[i].domain = ASX_AFFINITY_DOMAIN_ANY;
        g_workers[i].active = 1;
        g_workers[i].polls_total = 0;
        g_workers[i].tasks_completed = 0;
        g_workers[i].steals_total = 0;
        g_workers[i].commits_total = 0;
        g_workers[i].last_commit_sequence = 0;
        g_workers[i].lifecycle = ASX_WORKER_RUNNING;
        memset(g_workers[i].lane_depths, 0, sizeof(g_workers[i].lane_depths));
    }

    g_initialized = 1;
    return ASX_OK;
}

void asx_parallel_reset(void) {
    memset(g_lanes, 0, sizeof(g_lanes));
    memset(g_workers, 0, sizeof(g_workers));
    memset(&g_config, 0, sizeof(g_config));
    memset(&g_metrics, 0, sizeof(g_metrics));
    parallel_reset_routing();
    g_cancel_streak_limit = 16u;
    g_initialized = 0;
}

/* -------------------------------------------------------------------
 * Lane management
 * ------------------------------------------------------------------- */

asx_status asx_lane_assign(asx_task_id tid, asx_lane_class lane) {
    lane_internal *l;
    uint16_t slot_idx;

    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (!parallel_task_handle_valid(tid)) return ASX_E_INVALID_ARGUMENT;

    l = &g_lanes[(int)lane];
    if (l->count >= ASX_LANE_TASK_CAPACITY) { return ASX_E_RESOURCE_EXHAUSTED; }

    l->tasks[l->count] = tid;
    l->count++;
    slot_idx = asx_handle_slot(tid);
    if (slot_idx < ASX_MAX_TASKS) {
        uint32_t owner;
        g_task_lane_override_valid[slot_idx] = 1u;
        g_task_lane_override[slot_idx] = lane;
        owner = parallel_seed_task_owner(slot_idx);
        if (owner < ASX_MAX_WORKERS) { g_worker_lane_depths[owner][(int)lane]++; }
    }
    return ASX_OK;
}

asx_status asx_lane_remove(asx_task_id tid) {
    uint32_t i, j;

    if (!g_initialized) return ASX_E_INVALID_STATE;

    for (i = 0; i < ASX_MAX_LANES; i++) {
        lane_internal *l = &g_lanes[i];
        for (j = 0; j < l->count;
             j++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
            if (l->tasks[j] == tid) {
                /* Shift remaining tasks down */
                uint32_t k;
                for (k = j; k + 1 < l->count;
                     k++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
                    l->tasks[k] = l->tasks[k + 1];
                }
                l->count--;
                parallel_clear_task_routing(asx_handle_slot(tid));
                return ASX_OK;
            }
        }
    }

    return ASX_E_NOT_FOUND;
}

asx_status asx_lane_get_state(asx_lane_class lane, asx_lane_state *out) {
    lane_internal *l;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) return ASX_E_INVALID_STATE;

    l = &g_lanes[(int)lane];
    out->lane_class = lane;
    out->weight = g_config.lane_weights[(int)lane];
    out->task_count = l->count;
    out->polls_this_round = l->polls_this_round;
    out->starvation_count = l->starvation_count;
    out->max_starvation = g_config.starvation_limit;

    return ASX_OK;
}

uint32_t asx_lane_total_tasks(void) {
    uint32_t total = 0;
    uint32_t i;
    for (i = 0; i < ASX_MAX_LANES; i++) { total += g_lanes[i].count; }
    return total;
}

/* -------------------------------------------------------------------
 * Worker queries
 * ------------------------------------------------------------------- */

asx_status asx_worker_get_state(uint32_t worker_index, asx_worker_state *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (worker_index >= g_config.worker_count) { return ASX_E_INVALID_ARGUMENT; }

    for (i = 0; i < ASX_MAX_LANES; i++) {
        g_workers[worker_index].lane_depths[i] = g_worker_lane_depths[worker_index][i];
    }
    *out = g_workers[worker_index];
    return ASX_OK;
}

uint32_t asx_parallel_worker_count(void) { return g_config.worker_count; }

/* -------------------------------------------------------------------
 * Budget distribution helpers
 * ------------------------------------------------------------------- */

/* Compute per-lane poll quota for this round based on fairness policy */
static void compute_lane_quotas(uint32_t total_budget, uint32_t quotas[ASX_MAX_LANES]) {
    uint32_t i;

    switch (g_config.fairness) {
    case ASX_FAIRNESS_ROUND_ROBIN: {
        uint32_t active_lanes = 0;
        uint32_t per_lane;
        for (i = 0; i < ASX_MAX_LANES; i++) {
            if (g_lanes[i].count > 0) active_lanes++;
        }
        per_lane = (active_lanes > 0) ? total_budget / active_lanes : 0;
        for (i = 0; i < ASX_MAX_LANES; i++) { quotas[i] = (g_lanes[i].count > 0) ? per_lane : 0; }
        break;
    }

    case ASX_FAIRNESS_WEIGHTED: {
        uint32_t total_weight = 0;
        for (i = 0; i < ASX_MAX_LANES; i++) {
            if (g_lanes[i].count > 0) { total_weight += g_config.lane_weights[i]; }
        }
        for (i = 0; i < ASX_MAX_LANES; i++) {
            if (g_lanes[i].count > 0 && total_weight > 0) {
                quotas[i] =
                    (uint32_t)((uint64_t)total_budget * g_config.lane_weights[i] / total_weight);
            } else {
                quotas[i] = 0;
            }
        }
        break;
    }

    case ASX_FAIRNESS_PRIORITY:
        /* Cancel lane gets full budget first, then ready, then timed */
        quotas[ASX_LANE_CANCEL] = total_budget;
        quotas[ASX_LANE_READY] = total_budget;
        quotas[ASX_LANE_TIMED] = total_budget;
        break;

    default:
        for (i = 0; i < ASX_MAX_LANES; i++) { quotas[i] = total_budget / ASX_MAX_LANES; }
        break;
    }
}

/* Priority-ordered lane indices for scheduling */
static const int g_priority_order[ASX_MAX_LANES] = {
    ASX_LANE_CANCEL, /* cancel tasks drain first */
    ASX_LANE_READY,  /* then ready tasks */
    ASX_LANE_TIMED   /* timed tasks last */
};

/* Internal lane wrappers (scheduler context, return values consumed) */
static void lane_remove_internal(asx_task_id tid) {
    asx_status st_ = asx_lane_remove(tid);
    (void)st_;
}

static void lane_assign_internal(asx_task_id tid, asx_lane_class lc) {
    asx_status st_ = asx_lane_assign(tid, lc);
    (void)st_;
}

static asx_status parallel_return_budget(uint32_t round) {
    uint32_t i;
    for (i = 0; i < g_config.worker_count;
         i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_WORKERS validation") */
        g_workers[i].lifecycle = ASX_WORKER_DRAINING;
    }
    parallel_commit_worker_event(0u);
    asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
    return ASX_E_POLL_BUDGET_EXHAUSTED;
}

static asx_status parallel_return_quiescent(uint32_t round) {
    parallel_mark_workers_drained();
    parallel_commit_worker_event(0u);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, ASX_INVALID_ID, round);
    return ASX_OK;
}

static uint64_t asx_trace_task_transition_aux(asx_task_state from, asx_task_state to) {
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

/* -------------------------------------------------------------------
 * Parallel scheduler run
 *
 * Polls tasks lane-by-lane according to fairness policy.
 * In single-worker mode, produces deterministic event streams.
 * ------------------------------------------------------------------- */

asx_status asx_parallel_run(asx_region_id region, asx_budget *budget) {
    asx_region_slot *rslot;
    asx_status st;
    uint32_t round;
    uint32_t lane_idx;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;

    st = asx_region_slot_lookup(region, &rslot);
    if (st != ASX_OK) return st;

    parallel_mark_workers_running();
    memset(g_lane_dispatch_cursor, 0, sizeof(g_lane_dispatch_cursor));

    /* Auto-classify existing tasks into lanes */
    {
        uint32_t i;
        /* Clear lanes first */
        for (i = 0; i < ASX_MAX_LANES; i++) { g_lanes[i].count = 0; }
        /* Scan task arena and assign to lanes */
        for (i = 0; i < g_task_count; i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_TASKS") */
            asx_task_slot *t = &g_tasks[i];
            asx_task_id tid;
            asx_lane_class lc;

            if (!t->alive) continue;
            if (t->region != region) continue;
            if (asx_task_is_terminal(t->state)) continue;

            tid = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)t->state),
                                  asx_handle_pack_index(t->generation, (uint16_t)i));

            /* Classify by cancel state */
            if (t->cancel_pending) {
                lc = ASX_LANE_CANCEL;
            } else if (g_task_lane_override_valid[i]) {
                lc = g_task_lane_override[i];
            } else {
                lc = ASX_LANE_READY;
            }

            lane_assign_internal(tid, lc);
        }
        parallel_rebuild_worker_depths();
    }

    /* Scheduler loop */
    for (round = 0;; round++) {
        uint32_t total_active;
        uint32_t quotas[ASX_MAX_LANES];
        uint32_t lane_order_idx;
        int any_polled;

        ASX_CHECKPOINT_WAIVER("kernel-parallel-scheduler: budget exhaustion "
                              "provides bounded termination");

        asx_trace_emit(ASX_TRACE_SCHED_ROUND, ASX_INVALID_ID, round);

        if (asx_budget_is_exhausted(budget)) { return parallel_return_budget(round); }

        total_active = asx_lane_total_tasks();
        if (total_active == 0) { return parallel_return_quiescent(round); }

        /* Compute per-lane budgets for this round */
        compute_lane_quotas(asx_budget_polls(budget), quotas);

        /* Reset per-round counters */
        for (lane_idx = 0; lane_idx < ASX_MAX_LANES; lane_idx++) {
            g_lanes[lane_idx].polls_this_round = 0;
        }

        any_polled = 0;

        /* Poll each lane in priority order */
        for (lane_order_idx = 0; lane_order_idx < ASX_MAX_LANES; lane_order_idx++) {
            int li = g_priority_order[lane_order_idx];
            lane_internal *lane = &g_lanes[li];
            uint32_t quota = quotas[li];
            uint32_t j;
            uint32_t polls_this_lane = 0;

            ASX_CHECKPOINT_WAIVER("kernel-parallel-scheduler: lane iteration "
                                  "bounded by ASX_LANE_TASK_CAPACITY");

            if (lane->count == 0) continue;

            /* Cancel-streak fairness: skip cancel lane if streak
             * limit reached and other lanes have work */
            if (li == (int)ASX_LANE_CANCEL && g_cancel_streak_limit > 0 &&
                g_metrics.cancel_streak >= g_cancel_streak_limit) {
                /* Check if any other lane has tasks */
                if (g_lanes[ASX_LANE_READY].count > 0 || g_lanes[ASX_LANE_TIMED].count > 0) {
                    g_metrics.fairness_yields++;
                    continue;
                }
                /* Fallback: only cancel work remains, allow it */
            }

            /* Poll tasks in this lane up to quota */
            j = 0;
            while (j < lane->count && polls_this_lane < quota) {
                asx_task_id tid;
                asx_task_slot *t;
                uint16_t slot_idx;
                asx_status poll_result;
                uint32_t worker_idx;

                ASX_CHECKPOINT_WAIVER("kernel-parallel-scheduler: inner poll "
                                      "bounded by lane count and quota");

                if (asx_budget_is_exhausted(budget)) { return parallel_return_budget(round); }

                tid = lane->tasks[j];
                slot_idx = asx_handle_slot(tid);
                if (slot_idx >= ASX_MAX_TASKS) {
                    lane_remove_internal(tid);
                    continue; /* don't increment j, array shifted */
                }
                t = &g_tasks[slot_idx];

                if (!t->alive || asx_task_is_terminal(t->state)) {
                    /* Remove completed task from lane */
                    parallel_task_leaves_worker_lane(parallel_seed_task_owner(slot_idx),
                                                     (asx_lane_class)li);
                    parallel_clear_task_routing(slot_idx);
                    lane_remove_internal(tid);
                    continue; /* don't increment j, array shifted */
                }

                /* Refresh handle from live slot to avoid stale state masks. */
                tid = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)t->state),
                                      asx_handle_pack_index(t->generation, slot_idx));
                lane->tasks[j] = tid;
                worker_idx = parallel_select_worker((asx_lane_class)li, tid);

                /* Handle cancel force-completion */
                if (t->cancel_pending &&
                    (t->state == ASX_TASK_CANCELLING || t->state == ASX_TASK_CANCEL_REQUESTED) &&
                    t->cleanup_polls_remaining == 0) {
                    if (t->state == ASX_TASK_CANCEL_REQUESTED) {
                        asx_task_state from = t->state;
                        (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_CANCELLING);
                        t->state = ASX_TASK_CANCELLING;
                        t->cancel_phase = ASX_CANCEL_PHASE_CANCELLING;
                        {
                            asx_status w_st_ =
                                asx_cancel_witness_advance(t->cancel_witness,
                                                           ASX_CANCEL_PHASE_CANCELLING);
                            (void)w_st_;
                        }
                        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                       asx_trace_task_transition_aux(from, ASX_TASK_CANCELLING));
                    }
                    {
                        asx_task_state from = t->state;
                        (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_FINALIZING);
                        t->state = ASX_TASK_FINALIZING;
                        t->cancel_phase = ASX_CANCEL_PHASE_FINALIZING;
                        {
                            asx_status w_st_ =
                                asx_cancel_witness_advance(t->cancel_witness,
                                                           ASX_CANCEL_PHASE_FINALIZING);
                            (void)w_st_;
                        }
                        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                       asx_trace_task_transition_aux(from, ASX_TASK_FINALIZING));
                    }
                    {
                        asx_task_state from = t->state;
                        (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                        t->state = ASX_TASK_COMPLETED;
                        t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                        {
                            asx_status w_st_ =
                                asx_cancel_witness_advance(t->cancel_witness,
                                                           ASX_CANCEL_PHASE_COMPLETED);
                            (void)w_st_;
                        }
                        {
                            asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness);
                            (void)w_st_;
                        }
                        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                       asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                    }
                    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                    asx_task_release_capture_internal(t);
                    if (rslot->task_count > 0) rslot->task_count--;
                    parallel_task_leaves_worker_lane(worker_idx, (asx_lane_class)li);
                    parallel_clear_task_routing(slot_idx);
                    lane_remove_internal(tid);
                    g_workers[worker_idx].tasks_completed++;
                    parallel_commit_worker_event(worker_idx);
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
                    continue;
                }

                if (t->state == ASX_TASK_FINALIZING) {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                    t->state = ASX_TASK_COMPLETED;
                    t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                    {
                        asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness,
                                                                      ASX_CANCEL_PHASE_COMPLETED);
                        (void)w_st_;
                    }
                    {
                        asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness);
                        (void)w_st_;
                    }
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                    asx_task_release_capture_internal(t);
                    if (rslot->task_count > 0) rslot->task_count--;
                    parallel_task_leaves_worker_lane(worker_idx, (asx_lane_class)li);
                    parallel_clear_task_routing(slot_idx);
                    lane_remove_internal(tid);
                    g_workers[worker_idx].tasks_completed++;
                    parallel_commit_worker_event(worker_idx);
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
                    continue;
                }

                /* Consume budget */
                if (asx_budget_consume_poll(budget) == 0) { return parallel_return_budget(round); }

                /* Transition Created → Running */
                if (t->state == ASX_TASK_CREATED) {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_RUNNING);
                    t->state = ASX_TASK_RUNNING;
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_RUNNING));
                }

                asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)tid, round);
                parallel_commit_worker_event(worker_idx);

                /* Poll the task */
                asx_error_ledger_bind_task(tid);
                poll_result = t->poll_fn(t->user_data, tid);
                asx_error_ledger_bind_task(ASX_INVALID_ID);
                polls_this_lane++;
                g_workers[worker_idx].polls_total++;
                any_polled = 1;

                /* Update per-lane dispatch metrics and cancel streak */
                if (li == (int)ASX_LANE_CANCEL) {
                    g_metrics.cancel_dispatches++;
                    g_metrics.cancel_streak++;
                    if (g_metrics.cancel_streak > g_metrics.cancel_streak_max)
                        g_metrics.cancel_streak_max = g_metrics.cancel_streak;
                } else {
                    g_metrics.cancel_streak = 0;
                    if (li == (int)ASX_LANE_TIMED)
                        g_metrics.timed_dispatches++;
                    else
                        g_metrics.ready_dispatches++;
                }

                if (poll_result == ASX_OK) {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                    t->state = ASX_TASK_COMPLETED;
                    if (t->cancel_pending) {
                        t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                        {
                            asx_status w_st_ =
                                asx_cancel_witness_advance(t->cancel_witness,
                                                           ASX_CANCEL_PHASE_COMPLETED);
                            (void)w_st_;
                        }
                        {
                            asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness);
                            (void)w_st_;
                        }
                    }
                    t->outcome = asx_outcome_make(t->cancel_pending ? ASX_OUTCOME_CANCELLED
                                                                    : ASX_OUTCOME_OK);
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                    asx_task_release_capture_internal(t);
                    if (rslot->task_count > 0) rslot->task_count--;
                    parallel_task_leaves_worker_lane(worker_idx, (asx_lane_class)li);
                    parallel_clear_task_routing(slot_idx);
                    lane_remove_internal(tid);
                    g_workers[worker_idx].tasks_completed++;
                    parallel_commit_worker_event(worker_idx);
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
                    continue;
                } else if (poll_result != ASX_E_PENDING) {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                    t->state = ASX_TASK_COMPLETED;
                    if (t->cancel_pending) {
                        t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                        {
                            asx_status w_st_ =
                                asx_cancel_witness_advance(t->cancel_witness,
                                                           ASX_CANCEL_PHASE_COMPLETED);
                            (void)w_st_;
                        }
                        {
                            asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness);
                            (void)w_st_;
                        }
                    }
                    t->outcome = asx_outcome_make(t->cancel_pending ? ASX_OUTCOME_CANCELLED
                                                                    : ASX_OUTCOME_ERR);
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                    asx_task_release_capture_internal(t);
                    if (rslot->task_count > 0) rslot->task_count--;
                    parallel_task_leaves_worker_lane(worker_idx, (asx_lane_class)li);
                    parallel_clear_task_routing(slot_idx);
                    lane_remove_internal(tid);
                    g_workers[worker_idx].tasks_completed++;
                    parallel_commit_worker_event(worker_idx);
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);

                    {
                        asx_status fc_ = asx_region_contain_fault(region, poll_result);
                        if (fc_ != ASX_OK &&
                            asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
                            return fc_;
                        }
                    }
                    continue;
                }

                /* PENDING — still active */
                if (t->cancel_pending && t->cleanup_polls_remaining > 0) {
                    t->cleanup_polls_remaining--;
                }

                j++;
            }

            lane->polls_this_round = polls_this_lane;

            /* Track starvation */
            if (polls_this_lane == 0 && lane->count > 0) {
                lane->starvation_count++;
            } else {
                lane->starvation_count = 0;
            }
        }

        if (asx_lane_total_tasks() == 0) { return parallel_return_quiescent(round); }

        if (!any_polled) {
            /* All tasks in lanes are either completed or no budget */
            /* Budget too small for any lane to get a quota — return
             * exhausted instead of spinning forever. */
            return parallel_return_budget(round);
        }
    }
}

/* -------------------------------------------------------------------
 * Fairness queries
 * ------------------------------------------------------------------- */

int asx_parallel_starvation_detected(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_LANES; i++) {
        if (g_lanes[i].count > 0 && g_lanes[i].starvation_count > g_config.starvation_limit) {
            return 1;
        }
    }
    return 0;
}

uint32_t asx_parallel_max_starvation(void) {
    uint32_t max_val = 0;
    uint32_t i;
    for (i = 0; i < ASX_MAX_LANES; i++) {
        if (g_lanes[i].starvation_count > max_val) { max_val = g_lanes[i].starvation_count; }
    }
    return max_val;
}

asx_fairness_policy asx_parallel_fairness_policy(void) { return g_config.fairness; }

int asx_parallel_is_initialized(void) { return g_initialized; }

/* -------------------------------------------------------------------
 * Global injector
 * ------------------------------------------------------------------- */

asx_status asx_inject_cancel(asx_task_id tid) { return asx_lane_assign(tid, ASX_LANE_CANCEL); }

asx_status asx_inject_timed(asx_task_id tid) { return asx_lane_assign(tid, ASX_LANE_TIMED); }

asx_status asx_inject_ready(asx_task_id tid) { return asx_lane_assign(tid, ASX_LANE_READY); }

/* -------------------------------------------------------------------
 * Scheduling metrics
 * ------------------------------------------------------------------- */

asx_status asx_parallel_get_metrics(asx_scheduling_metrics *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    *out = g_metrics;
    return ASX_OK;
}

void asx_parallel_reset_metrics(void) { memset(&g_metrics, 0, sizeof(g_metrics)); }

/* -------------------------------------------------------------------
 * Cancel-streak fairness configuration
 * ------------------------------------------------------------------- */

void asx_parallel_set_cancel_streak_limit(uint32_t limit) { g_cancel_streak_limit = limit; }

uint32_t asx_parallel_cancel_streak_limit(void) { return g_cancel_streak_limit; }
