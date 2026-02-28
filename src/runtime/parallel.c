/*
 * parallel.c — optional parallel profile worker model and lane scheduler
 *
 * Implements lane-based task scheduling with bounded fairness controls.
 * Walking skeleton: single-threaded simulation. Tasks are classified
 * into READY, CANCEL, and TIMED lanes. The scheduler polls lanes
 * according to the configured fairness policy.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/asx_config.h>
#include <asx/core/transition.h>
#include <asx/core/cancel.h>
#include "runtime_internal.h"
#include <string.h>

/* -------------------------------------------------------------------
 * Lane internal state
 * ------------------------------------------------------------------- */

typedef struct {
    asx_task_id  tasks[ASX_LANE_TASK_CAPACITY];
    uint32_t     count;
    uint32_t     polls_this_round;
    uint32_t     starvation_count;
} lane_internal;

/* -------------------------------------------------------------------
 * Global parallel scheduler state
 * ------------------------------------------------------------------- */

static int               g_initialized;
static asx_parallel_config g_config;
static lane_internal     g_lanes[ASX_MAX_LANES];
static asx_worker_state  g_workers[ASX_MAX_WORKERS];

/* -------------------------------------------------------------------
 * Init / Reset
 * ------------------------------------------------------------------- */

asx_status asx_parallel_init(const asx_parallel_config *cfg)
{
    uint32_t i;

    if (cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cfg->worker_count == 0 || cfg->worker_count > ASX_MAX_WORKERS) {
        return ASX_E_INVALID_ARGUMENT;
    }

    g_config = *cfg;

    /* Initialize lanes */
    for (i = 0; i < ASX_MAX_LANES; i++) {
        memset(&g_lanes[i], 0, sizeof(lane_internal));
    }

    /* Initialize workers */
    for (i = 0; i < cfg->worker_count; i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_WORKERS checked above") */
        g_workers[i].id = i;
        g_workers[i].domain = ASX_AFFINITY_DOMAIN_ANY;
        g_workers[i].active = 1;
        g_workers[i].polls_total = 0;
        g_workers[i].tasks_completed = 0;
    }

    g_initialized = 1;
    return ASX_OK;
}

void asx_parallel_reset(void)
{
    memset(g_lanes, 0, sizeof(g_lanes));
    memset(g_workers, 0, sizeof(g_workers));
    memset(&g_config, 0, sizeof(g_config));
    g_initialized = 0;
}

/* -------------------------------------------------------------------
 * Lane management
 * ------------------------------------------------------------------- */

asx_status asx_lane_assign(asx_task_id tid, asx_lane_class lane)
{
    lane_internal *l;

    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) {
        return ASX_E_INVALID_ARGUMENT;
    }

    l = &g_lanes[(int)lane];
    if (l->count >= ASX_LANE_TASK_CAPACITY) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    l->tasks[l->count] = tid;
    l->count++;
    return ASX_OK;
}

asx_status asx_lane_remove(asx_task_id tid)
{
    uint32_t i, j;

    for (i = 0; i < ASX_MAX_LANES; i++) {
        lane_internal *l = &g_lanes[i];
        for (j = 0; j < l->count; j++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
            if (l->tasks[j] == tid) {
                /* Shift remaining tasks down */
                uint32_t k;
                for (k = j; k + 1 < l->count; k++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
                    l->tasks[k] = l->tasks[k + 1];
                }
                l->count--;
                return ASX_OK;
            }
        }
    }

    return ASX_E_NOT_FOUND;
}

asx_status asx_lane_get_state(asx_lane_class lane, asx_lane_state *out)
{
    lane_internal *l;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) {
        return ASX_E_INVALID_ARGUMENT;
    }

    l = &g_lanes[(int)lane];
    out->lane_class = lane;
    out->weight = g_config.lane_weights[(int)lane];
    out->task_count = l->count;
    out->polls_this_round = l->polls_this_round;
    out->starvation_count = l->starvation_count;
    out->max_starvation = g_config.starvation_limit;

    return ASX_OK;
}

uint32_t asx_lane_total_tasks(void)
{
    uint32_t total = 0;
    uint32_t i;
    for (i = 0; i < ASX_MAX_LANES; i++) {
        total += g_lanes[i].count;
    }
    return total;
}

/* -------------------------------------------------------------------
 * Worker queries
 * ------------------------------------------------------------------- */

asx_status asx_worker_get_state(uint32_t worker_index,
                                 asx_worker_state *out)
{
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (worker_index >= g_config.worker_count) {
        return ASX_E_INVALID_ARGUMENT;
    }

    *out = g_workers[worker_index];
    return ASX_OK;
}

uint32_t asx_parallel_worker_count(void)
{
    return g_config.worker_count;
}

/* -------------------------------------------------------------------
 * Budget distribution helpers
 * ------------------------------------------------------------------- */

/* Compute per-lane poll quota for this round based on fairness policy */
static void compute_lane_quotas(uint32_t total_budget,
                                 uint32_t quotas[ASX_MAX_LANES])
{
    uint32_t i;

    switch (g_config.fairness) {
    case ASX_FAIRNESS_ROUND_ROBIN: {
        uint32_t active_lanes = 0;
        uint32_t per_lane;
        for (i = 0; i < ASX_MAX_LANES; i++) {
            if (g_lanes[i].count > 0) active_lanes++;
        }
        per_lane = (active_lanes > 0) ? total_budget / active_lanes : 0;
        for (i = 0; i < ASX_MAX_LANES; i++) {
            quotas[i] = (g_lanes[i].count > 0) ? per_lane : 0;
        }
        break;
    }

    case ASX_FAIRNESS_WEIGHTED: {
        uint32_t total_weight = 0;
        for (i = 0; i < ASX_MAX_LANES; i++) {
            if (g_lanes[i].count > 0) {
                total_weight += g_config.lane_weights[i];
            }
        }
        for (i = 0; i < ASX_MAX_LANES; i++) {
            if (g_lanes[i].count > 0 && total_weight > 0) {
                quotas[i] = (total_budget * g_config.lane_weights[i])
                            / total_weight;
            } else {
                quotas[i] = 0;
            }
        }
        break;
    }

    case ASX_FAIRNESS_PRIORITY:
        /* Cancel lane gets full budget first, then ready, then timed */
        quotas[ASX_LANE_CANCEL] = total_budget;
        quotas[ASX_LANE_READY]  = total_budget;
        quotas[ASX_LANE_TIMED]  = total_budget;
        break;

    default:
        for (i = 0; i < ASX_MAX_LANES; i++) {
            quotas[i] = total_budget / ASX_MAX_LANES;
        }
        break;
    }
}

/* Priority-ordered lane indices for scheduling */
static const int g_priority_order[ASX_MAX_LANES] = {
    ASX_LANE_CANCEL,  /* cancel tasks drain first */
    ASX_LANE_READY,   /* then ready tasks */
    ASX_LANE_TIMED    /* timed tasks last */
};

/* Internal lane wrappers (scheduler context, return values consumed) */
static void lane_remove_internal(asx_task_id tid)
{
    asx_status st_ = asx_lane_remove(tid);
    (void)st_;
}

static void lane_assign_internal(asx_task_id tid, asx_lane_class lc)
{
    asx_status st_ = asx_lane_assign(tid, lc);
    (void)st_;
}

/* -------------------------------------------------------------------
 * Parallel scheduler run
 *
 * Polls tasks lane-by-lane according to fairness policy.
 * In single-worker mode, produces deterministic event streams.
 * ------------------------------------------------------------------- */

asx_status asx_parallel_run(asx_region_id region, asx_budget *budget)
{
    asx_region_slot *rslot;
    asx_status st;
    uint32_t round;
    uint32_t lane_idx;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;

    st = asx_region_slot_lookup(region, &rslot);
    if (st != ASX_OK) return st;

    /* Auto-classify existing tasks into lanes */
    {
        uint32_t i;
        /* Clear lanes first */
        for (i = 0; i < ASX_MAX_LANES; i++) {
            g_lanes[i].count = 0;
        }
        /* Scan task arena and assign to lanes */
        for (i = 0; i < g_task_count; i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_MAX_TASKS") */
            asx_task_slot *t = &g_tasks[i];
            asx_task_id tid;
            asx_lane_class lc;

            if (!t->alive) continue;
            if (t->region != region) continue;
            if (asx_task_is_terminal(t->state)) continue;

            tid = asx_handle_pack(ASX_TYPE_TASK,
                                  (uint16_t)(1u << (unsigned)t->state),
                                  asx_handle_pack_index(t->generation,
                                                         (uint16_t)i));

            /* Classify by cancel state */
            if (t->cancel_pending) {
                lc = ASX_LANE_CANCEL;
            } else {
                lc = ASX_LANE_READY;
            }
            /* Note: TIMED lane assignment would require timer integration.
             * Walking skeleton assigns to READY by default. */

            lane_assign_internal(tid, lc);
        }
    }

    /* Scheduler loop */
    for (round = 0; ; round++) {
        uint32_t total_active;
        uint32_t quotas[ASX_MAX_LANES];
        uint32_t lane_order_idx;
        int any_polled;

        ASX_CHECKPOINT_WAIVER("kernel-parallel-scheduler: budget exhaustion "
                              "provides bounded termination");

        if (asx_budget_is_exhausted(budget)) {
            return ASX_E_POLL_BUDGET_EXHAUSTED;
        }

        total_active = asx_lane_total_tasks();
        if (total_active == 0) {
            return ASX_OK;
        }

        /* Compute per-lane budgets for this round */
        compute_lane_quotas(asx_budget_polls(budget), quotas);

        /* Reset per-round counters */
        for (lane_idx = 0; lane_idx < ASX_MAX_LANES; lane_idx++) {
            g_lanes[lane_idx].polls_this_round = 0;
        }

        any_polled = 0;

        /* Poll each lane in priority order */
        for (lane_order_idx = 0; lane_order_idx < ASX_MAX_LANES;
             lane_order_idx++) {
            int li = g_priority_order[lane_order_idx];
            lane_internal *lane = &g_lanes[li];
            uint32_t quota = quotas[li];
            uint32_t j;
            uint32_t polls_this_lane = 0;

            ASX_CHECKPOINT_WAIVER("kernel-parallel-scheduler: lane iteration "
                                  "bounded by ASX_LANE_TASK_CAPACITY");

            if (lane->count == 0) continue;

            /* Poll tasks in this lane up to quota */
            j = 0;
            while (j < lane->count && polls_this_lane < quota) {
                asx_task_id tid;
                asx_task_slot *t;
                uint16_t slot_idx;
                asx_status poll_result;

                ASX_CHECKPOINT_WAIVER("kernel-parallel-scheduler: inner poll "
                                      "bounded by lane count and quota");

                if (asx_budget_is_exhausted(budget)) {
                    return ASX_E_POLL_BUDGET_EXHAUSTED;
                }

                tid = lane->tasks[j];
                slot_idx = asx_handle_slot(tid);
                if (slot_idx >= ASX_MAX_TASKS) {
                    j++;
                    continue;
                }
                t = &g_tasks[slot_idx];

                if (!t->alive || asx_task_is_terminal(t->state)) {
                    /* Remove completed task from lane */
                    lane_remove_internal(tid);
                    continue; /* don't increment j, array shifted */
                }

                /* Handle cancel force-completion */
                if (t->cancel_pending &&
                    (t->state == ASX_TASK_CANCELLING ||
                     t->state == ASX_TASK_CANCEL_REQUESTED) &&
                    t->cleanup_polls_remaining == 0) {
                    t->state = ASX_TASK_COMPLETED;
                    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                    rslot->task_count--;
                    lane_remove_internal(tid);
                    g_workers[0].tasks_completed++;
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE,
                                   (uint64_t)tid, round);
                    continue;
                }

                if (t->state == ASX_TASK_FINALIZING) {
                    t->state = ASX_TASK_COMPLETED;
                    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                    rslot->task_count--;
                    lane_remove_internal(tid);
                    g_workers[0].tasks_completed++;
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE,
                                   (uint64_t)tid, round);
                    continue;
                }

                /* Consume budget */
                if (asx_budget_consume_poll(budget) == 0) {
                    return ASX_E_POLL_BUDGET_EXHAUSTED;
                }

                /* Transition Created → Running */
                if (t->state == ASX_TASK_CREATED) {
                    t->state = ASX_TASK_RUNNING;
                }

                asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)tid, round);

                /* Poll the task */
                poll_result = t->poll_fn(t->user_data, tid);

                if (poll_result == ASX_OK) {
                    t->state = ASX_TASK_COMPLETED;
                    t->outcome = asx_outcome_make(
                        t->cancel_pending ? ASX_OUTCOME_CANCELLED
                                          : ASX_OUTCOME_OK);
                    rslot->task_count--;
                    lane_remove_internal(tid);
                    g_workers[0].tasks_completed++;
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE,
                                   (uint64_t)tid, round);
                    continue;
                } else if (poll_result != ASX_E_PENDING) {
                    t->state = ASX_TASK_COMPLETED;
                    t->outcome = asx_outcome_make(
                        t->cancel_pending ? ASX_OUTCOME_CANCELLED
                                          : ASX_OUTCOME_ERR);
                    rslot->task_count--;
                    lane_remove_internal(tid);
                    g_workers[0].tasks_completed++;
                    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE,
                                   (uint64_t)tid, round);
                    continue;
                }

                /* PENDING — still active */
                if (t->cancel_pending && t->cleanup_polls_remaining > 0) {
                    t->cleanup_polls_remaining--;
                }

                polls_this_lane++;
                g_workers[0].polls_total++;
                any_polled = 1;
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

        if (!any_polled) {
            /* All tasks in lanes are either completed or no budget */
            if (asx_lane_total_tasks() == 0) {
                return ASX_OK;
            }
        }
    }
}

/* -------------------------------------------------------------------
 * Fairness queries
 * ------------------------------------------------------------------- */

int asx_parallel_starvation_detected(void)
{
    uint32_t i;
    for (i = 0; i < ASX_MAX_LANES; i++) {
        if (g_lanes[i].count > 0 &&
            g_lanes[i].starvation_count > g_config.starvation_limit) {
            return 1;
        }
    }
    return 0;
}

uint32_t asx_parallel_max_starvation(void)
{
    uint32_t max_val = 0;
    uint32_t i;
    for (i = 0; i < ASX_MAX_LANES; i++) {
        if (g_lanes[i].starvation_count > max_val) {
            max_val = g_lanes[i].starvation_count;
        }
    }
    return max_val;
}

asx_fairness_policy asx_parallel_fairness_policy(void)
{
    return g_config.fairness;
}

int asx_parallel_is_initialized(void)
{
    return g_initialized;
}

