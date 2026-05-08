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
#include <asx/runtime/blocking.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/waker.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <stdio.h>
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
static uint8_t g_task_ready_to_poll[ASX_MAX_TASKS];
static uint8_t g_task_timed_since_valid[ASX_MAX_TASKS];
static uint32_t g_task_timed_since_round[ASX_MAX_TASKS];
static uint32_t g_worker_lane_depths[ASX_MAX_WORKERS][ASX_MAX_LANES];
static uint32_t g_lane_dispatch_cursor[ASX_MAX_LANES];
static uint32_t g_current_round;
static int g_pressure_triggered;

/* Cancel-streak fairness state */
static uint32_t g_cancel_streak_limit = 16u;
static asx_scheduling_metrics g_metrics;
static asx_parallel_admission_decision g_last_admission;

static void sat_inc_u32(uint32_t *value) {
    if (value != NULL && *value < UINT32_MAX) { (*value)++; }
}

static void sat_add_u32(uint32_t *value, uint32_t delta) {
    if (value == NULL) { return; }
    if (UINT32_MAX - *value < delta) {
        *value = UINT32_MAX;
    } else {
        *value += delta;
    }
}

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
        g_task_ready_to_poll[i] = 0u;
        g_task_timed_since_valid[i] = 0u;
        g_task_timed_since_round[i] = 0u;
    }
    memset(g_worker_lane_depths, 0, sizeof(g_worker_lane_depths));
    memset(g_lane_dispatch_cursor, 0, sizeof(g_lane_dispatch_cursor));
    g_current_round = 0u;
    g_pressure_triggered = 0;
}

static uint32_t parallel_active_worker_count(void) {
    return g_config.worker_count == 0u ? 1u : g_config.worker_count;
}

static uint32_t percent_u32(uint32_t used, uint32_t capacity) {
    if (capacity == 0u) { return 100u; }
    if (used >= capacity) { return 100u; }
    return (uint32_t)(((uint64_t)used * 100u) / (uint64_t)capacity);
}

static int admission_mode_valid(asx_parallel_admission_mode mode) {
    return mode == ASX_PARALLEL_ADMISSION_REJECT || mode == ASX_PARALLEL_ADMISSION_BACKPRESSURE ||
           mode == ASX_PARALLEL_ADMISSION_SHED_OLDEST;
}

static int locality_mode_valid(asx_parallel_locality_mode mode) {
    return mode == ASX_PARALLEL_LOCALITY_COMPACT || mode == ASX_PARALLEL_LOCALITY_WORKER_SHARDED ||
           mode == ASX_PARALLEL_LOCALITY_NUMA_DOMAIN_SHARDED;
}

static void normalize_admission_policy(asx_parallel_admission_policy *policy) {
    if (policy == NULL) { return; }
    if (policy->pressure_threshold_pct == 0u) {
        policy->pressure_threshold_pct = ASX_PARALLEL_DEFAULT_PRESSURE_THRESHOLD_PCT;
    }
    if (policy->shed_max == 0u && policy->mode == ASX_PARALLEL_ADMISSION_SHED_OLDEST) {
        policy->shed_max = 1u;
    }
}

static uint32_t ceil_div_u32(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0u) { return 0u; }
    return (numerator + denominator - 1u) / denominator;
}

static int parallel_locality_sharding_available(void) {
#if defined(ASX_PROFILE_FREESTANDING) || defined(ASX_PROFILE_BROWSER) ||                           \
    defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return 0;
#else
    return 1;
#endif
}

static void normalize_locality_config(asx_parallel_locality_config *locality,
                                      uint32_t worker_count) {
    uint32_t shard_count;

    if (locality == NULL) { return; }
    if (!parallel_locality_sharding_available()) { locality->mode = ASX_PARALLEL_LOCALITY_COMPACT; }

    if (locality->mode == ASX_PARALLEL_LOCALITY_COMPACT || worker_count <= 1u) {
        locality->mode = ASX_PARALLEL_LOCALITY_COMPACT;
        locality->shard_count = 1u;
        locality->tasks_per_shard = ASX_MAX_TASKS;
        return;
    }

    shard_count = locality->shard_count;
    if (shard_count == 0u) { shard_count = worker_count; }
    if (shard_count > ASX_PARALLEL_MAX_LOCALITY_SHARDS) {
        shard_count = ASX_PARALLEL_MAX_LOCALITY_SHARDS;
    }
    if (shard_count > ASX_MAX_TASKS) { shard_count = ASX_MAX_TASKS; }
    if (shard_count == 0u) { shard_count = 1u; }

    locality->shard_count = shard_count;
    if (locality->tasks_per_shard == 0u) {
        locality->tasks_per_shard = ceil_div_u32(ASX_MAX_TASKS, shard_count);
    }
    if (locality->tasks_per_shard == 0u) { locality->tasks_per_shard = 1u; }
}

static uint32_t parallel_slot_locality_shard(uint16_t slot_idx) {
    const asx_parallel_locality_config *locality = &g_config.locality;
    uint32_t shard;

    if (slot_idx >= ASX_MAX_TASKS) { return 0u; }
    if (locality->mode == ASX_PARALLEL_LOCALITY_COMPACT || locality->shard_count <= 1u) {
        return 0u;
    }

    shard = (uint32_t)slot_idx / locality->tasks_per_shard;
    if (shard >= locality->shard_count) { shard = locality->shard_count - 1u; }
    return shard;
}

static uint32_t parallel_locality_owner_for_shard(uint32_t shard) {
    uint32_t worker_count = parallel_active_worker_count();
    if (worker_count == 0u) { return 0u; }
    if (worker_count > ASX_MAX_WORKERS) { worker_count = ASX_MAX_WORKERS; }
    return shard % worker_count;
}

static uint32_t parallel_locality_seed_owner(uint16_t slot_idx) {
    uint32_t shard = parallel_slot_locality_shard(slot_idx);
    return parallel_locality_owner_for_shard(shard);
}

static uint32_t parallel_locality_effective_owner(uint16_t slot_idx) {
    uint32_t worker_count = parallel_active_worker_count();
    uint32_t owner;

    if (slot_idx >= ASX_MAX_TASKS) { return 0u; }
    if (worker_count == 0u) { return 0u; }
    if (worker_count > ASX_MAX_WORKERS) { worker_count = ASX_MAX_WORKERS; }

    owner = (uint32_t)g_task_worker_owner[slot_idx];
    if (owner == ASX_PARALLEL_TASK_UNOWNED || owner >= worker_count) {
        return parallel_locality_seed_owner(slot_idx);
    }
    return owner;
}

static void parallel_fill_locality_snapshot(asx_parallel_locality_snapshot *out) {
    uint32_t lane_idx;

    if (out == NULL) { return; }
    memset(out, 0, sizeof(*out));
    out->mode = g_config.locality.mode;
    out->shard_count = g_config.locality.shard_count == 0u ? 1u : g_config.locality.shard_count;
    out->tasks_per_shard =
        g_config.locality.tasks_per_shard == 0u ? ASX_MAX_TASKS : g_config.locality.tasks_per_shard;
    out->slot_count = ASX_MAX_TASKS;

    for (lane_idx = 0u; lane_idx < ASX_MAX_LANES; lane_idx++) {
        lane_internal *lane = &g_lanes[lane_idx];
        uint32_t i;

        for (i = 0u; i < lane->count;
             i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
            uint32_t shard = parallel_slot_locality_shard(asx_handle_slot(lane->tasks[i]));
            if (shard < ASX_PARALLEL_MAX_LOCALITY_SHARDS) {
                sat_inc_u32(&out->shard_task_counts[shard]);
                if (out->shard_task_counts[shard] > out->max_shard_tasks) {
                    out->max_shard_tasks = out->shard_task_counts[shard];
                    out->hot_shard = shard;
                }
            }
        }
    }
}

static void parallel_record_admission_decision(const asx_parallel_admission_decision *decision) {
    int triggered;

    if (decision == NULL) { return; }
    g_last_admission = *decision;
    triggered = decision->triggered ? 1 : 0;
    if (triggered != g_pressure_triggered) {
        sat_inc_u32(&g_metrics.pressure_transitions);
        g_pressure_triggered = triggered;
    }
    if (!decision->triggered) { return; }

    switch (decision->mode) {
    case ASX_PARALLEL_ADMISSION_REJECT: sat_inc_u32(&g_metrics.admission_rejects); break;
    case ASX_PARALLEL_ADMISSION_BACKPRESSURE: sat_inc_u32(&g_metrics.admission_backpressure); break;
    case ASX_PARALLEL_ADMISSION_SHED_OLDEST:
        sat_add_u32(&g_metrics.admission_sheds, decision->shed_count);
        break;
    default: break;
    }
}

static asx_status parallel_observe_lane_admission(uint32_t lane_count_after) {
    asx_parallel_admission_decision decision;
    asx_status st;

    st = asx_parallel_evaluate_admission(&g_config.admission_policy, lane_count_after,
                                         ASX_LANE_TASK_CAPACITY, &decision);
    if (st != ASX_OK) { return st; }
    parallel_record_admission_decision(&decision);

    if (g_config.admission_policy.enforce && decision.admit_status != ASX_OK) {
        return decision.admit_status;
    }
    return ASX_OK;
}

static uint32_t parallel_seed_task_owner(uint16_t slot_idx) {
    uint32_t worker_count = parallel_active_worker_count();
    uint32_t locality_owner;
    uint32_t owner;
    if (slot_idx >= ASX_MAX_TASKS) { return 0u; }
    if (worker_count > ASX_MAX_WORKERS) { worker_count = ASX_MAX_WORKERS; }
    locality_owner = parallel_locality_seed_owner(slot_idx);
    if (g_task_worker_owner[slot_idx] == ASX_PARALLEL_TASK_UNOWNED ||
        g_task_worker_owner[slot_idx] >= worker_count) {
        g_task_worker_owner[slot_idx] = (uint16_t)locality_owner;
    }
    owner = (uint32_t)g_task_worker_owner[slot_idx];
    return owner >= worker_count ? 0u : owner;
}

static void parallel_clear_task_routing(uint16_t slot_idx) {
    if (slot_idx >= ASX_MAX_TASKS) { return; }
    g_task_worker_owner[slot_idx] = ASX_PARALLEL_TASK_UNOWNED;
    g_task_lane_override_valid[slot_idx] = 0u;
    g_task_lane_override[slot_idx] = ASX_LANE_READY;
    g_task_ready_to_poll[slot_idx] = 0u;
    g_task_timed_since_valid[slot_idx] = 0u;
    g_task_timed_since_round[slot_idx] = 0u;
}

static int parallel_find_task_lane(uint16_t slot_idx, uint16_t generation, uint32_t *out_lane,
                                   uint32_t *out_pos) {
    uint32_t lane_idx;

    for (lane_idx = 0; lane_idx < ASX_MAX_LANES; lane_idx++) {
        lane_internal *lane = &g_lanes[lane_idx];
        uint32_t pos;
        for (pos = 0; pos < lane->count;
             pos++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
            asx_task_id existing = lane->tasks[pos];
            if (asx_handle_slot(existing) == slot_idx &&
                asx_handle_generation(existing) == generation) {
                if (out_lane != NULL) *out_lane = lane_idx;
                if (out_pos != NULL) *out_pos = pos;
                return 1;
            }
        }
    }

    return 0;
}

static void parallel_remove_lane_pos(uint32_t lane_idx, uint32_t pos) {
    lane_internal *lane;
    uint32_t k;

    if (lane_idx >= ASX_MAX_LANES) return;
    lane = &g_lanes[lane_idx];
    if (pos >= lane->count) return;

    for (k = pos; k + 1 < lane->count;
         k++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
        lane->tasks[k] = lane->tasks[k + 1];
    }
    lane->count--;
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
        sat_inc_u32(&g_metrics.steal_attempts);
        if (g_worker_lane_depths[owner][(int)lane] > 0u) {
            g_worker_lane_depths[owner][(int)lane]--;
            g_worker_lane_depths[selected][(int)lane]++;
            g_task_worker_owner[slot_idx] = (uint16_t)selected;
            sat_inc_u32(&g_metrics.steals_succeeded);
            sat_inc_u32(&g_workers[selected].steals_total);
            return selected;
        }
        sat_inc_u32(&g_metrics.steals_failed);
        sat_inc_u32(&g_metrics.worker_yields);
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
    sat_inc_u32(&g_workers[worker_idx].commits_total);
    g_workers[worker_idx].last_commit_sequence = g_metrics.commit_sequence;
    sat_inc_u32(&g_metrics.commit_sequence);
}

static asx_status parallel_native_live_status(void) {
#if defined(ASX_PROFILE_POSIX) || defined(ASX_PROFILE_WIN32)
    return ASX_E_HOOK_MISSING;
#else
    return ASX_E_PERMISSION_DENIED;
#endif
}

static void parallel_fill_commit_authority_snapshot(asx_parallel_commit_authority_snapshot *out) {
    uint32_t i;
    uint32_t active_workers;

    if (out == NULL) { return; }

    memset(out, 0, sizeof(*out));
    out->commit_sequence = g_metrics.commit_sequence;
    out->native_live_enabled = 0u;
    out->native_live_status = parallel_native_live_status();

    active_workers = parallel_active_worker_count();
    if (active_workers > ASX_MAX_WORKERS) { active_workers = ASX_MAX_WORKERS; }

    for (i = 0u; i < active_workers; i++) {
        sat_add_u32(&out->total_worker_commits, g_workers[i].commits_total);
        if (g_workers[i].last_commit_sequence > out->max_worker_commit_sequence) {
            out->max_worker_commit_sequence = g_workers[i].last_commit_sequence;
        }
        if (g_workers[i].last_commit_sequence > g_metrics.commit_sequence) {
            out->drift_detected = 1u;
        }
    }

    if (out->total_worker_commits != g_metrics.commit_sequence) { out->drift_detected = 1u; }
    if (g_metrics.commit_sequence == 0u) {
        if (out->max_worker_commit_sequence != 0u) { out->drift_detected = 1u; }
    } else if (out->max_worker_commit_sequence >= g_metrics.commit_sequence) {
        out->drift_detected = 1u;
    }
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
    if (!admission_mode_valid(cfg->admission_policy.mode)) { return ASX_E_INVALID_ARGUMENT; }
    if (cfg->admission_policy.pressure_threshold_pct > 100u) { return ASX_E_INVALID_ARGUMENT; }
    if (!locality_mode_valid(cfg->locality.mode)) { return ASX_E_INVALID_ARGUMENT; }
    if (cfg->locality.shard_count > ASX_PARALLEL_MAX_LOCALITY_SHARDS) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    g_config = *cfg;
    normalize_admission_policy(&g_config.admission_policy);
    normalize_locality_config(&g_config.locality, g_config.worker_count);

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
    memset(&g_last_admission, 0, sizeof(g_last_admission));
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
    uint16_t generation;
    uint32_t old_lane;
    uint32_t old_pos;
    int already_assigned;

    if ((int)lane < 0 || (int)lane >= (int)ASX_MAX_LANES) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (!parallel_task_handle_valid(tid)) return ASX_E_INVALID_ARGUMENT;

    l = &g_lanes[(int)lane];
    slot_idx = asx_handle_slot(tid);
    generation = asx_handle_generation(tid);
    already_assigned = parallel_find_task_lane(slot_idx, generation, &old_lane, &old_pos);

    if (already_assigned && old_lane == (uint32_t)lane) {
        g_task_lane_override_valid[slot_idx] = 1u;
        g_task_lane_override[slot_idx] = lane;
        g_task_ready_to_poll[slot_idx] = (uint8_t)(lane == ASX_LANE_TIMED ? 0u : 1u);
        if (lane == ASX_LANE_TIMED) {
            g_task_timed_since_valid[slot_idx] = 1u;
            g_task_timed_since_round[slot_idx] = g_current_round;
        } else {
            g_task_timed_since_valid[slot_idx] = 0u;
            g_task_timed_since_round[slot_idx] = 0u;
        }
        return ASX_OK;
    }

    if (l->count >= ASX_LANE_TASK_CAPACITY) {
        (void)parallel_observe_lane_admission(ASX_LANE_TASK_CAPACITY);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    if (!already_assigned) {
        asx_status admission_st = parallel_observe_lane_admission(l->count + 1u);
        if (admission_st != ASX_OK) { return admission_st; }
    }

    if (already_assigned) {
        uint32_t owner = parallel_seed_task_owner(slot_idx);
        parallel_task_leaves_worker_lane(owner, (asx_lane_class)old_lane);
        parallel_remove_lane_pos(old_lane, old_pos);
    }

    l->tasks[l->count] = tid;
    l->count++;
    if (slot_idx < ASX_MAX_TASKS) {
        uint32_t owner;
        g_task_lane_override_valid[slot_idx] = 1u;
        g_task_lane_override[slot_idx] = lane;
        g_task_ready_to_poll[slot_idx] = (uint8_t)(lane == ASX_LANE_TIMED ? 0u : 1u);
        if (lane == ASX_LANE_TIMED) {
            g_task_timed_since_valid[slot_idx] = 1u;
            g_task_timed_since_round[slot_idx] = g_current_round;
        } else {
            g_task_timed_since_valid[slot_idx] = 0u;
            g_task_timed_since_round[slot_idx] = 0u;
        }
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
                uint32_t owner = parallel_seed_task_owner(asx_handle_slot(tid));
                parallel_task_leaves_worker_lane(owner, (asx_lane_class)i);
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

static void parallel_promote_ready_task(asx_region_id region, asx_task_id tid) {
    asx_task_slot *slot;
    uint16_t slot_idx;
    asx_lane_class target_lane;
    uint32_t old_lane;
    uint32_t old_pos;
    int was_timed = 0;
    asx_status st;

    st = asx_task_slot_lookup(tid, &slot);
    if (st != ASX_OK) return;
    if (!slot->alive || slot->region != region || asx_task_is_terminal(slot->state)) return;

    slot_idx = asx_handle_slot(tid);
    if (slot_idx >= ASX_MAX_TASKS) return;

    if (parallel_find_task_lane(slot_idx, asx_handle_generation(tid), &old_lane, &old_pos) &&
        old_lane == (uint32_t)ASX_LANE_TIMED) {
        (void)old_pos;
        was_timed = 1;
    }

    target_lane = slot->cancel_pending ? ASX_LANE_CANCEL : ASX_LANE_READY;
    st = asx_lane_assign(tid, target_lane);
    if (st == ASX_OK) {
        g_task_ready_to_poll[slot_idx] = 1u;
        sat_inc_u32(&g_metrics.waker_ready);
        if (was_timed) {
            uint32_t latency = 0u;
            if (g_task_timed_since_valid[slot_idx] &&
                g_current_round >= g_task_timed_since_round[slot_idx]) {
                latency = g_current_round - g_task_timed_since_round[slot_idx];
            }
            sat_inc_u32(&g_metrics.timed_promotions);
            sat_add_u32(&g_metrics.timed_wake_latency_rounds_total, latency);
            if (latency > g_metrics.timed_wake_latency_rounds_max) {
                g_metrics.timed_wake_latency_rounds_max = latency;
            }
            g_task_timed_since_valid[slot_idx] = 0u;
            g_task_timed_since_round[slot_idx] = 0u;
        }
    }
}

static void parallel_drain_ready_sources(asx_region_id region, uint32_t round) {
    asx_task_id ready_tasks[ASX_LANE_TASK_CAPACITY];
    uint32_t ready_count;
    uint32_t i;

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_io_driver_is_initialized()) {
        asx_io_event events[ASX_LANE_TASK_CAPACITY];
        uint32_t event_count;
        event_count = asx_io_driver_poll(events, ASX_LANE_TASK_CAPACITY, 0u);
        (void)events;
        sat_inc_u32(&g_metrics.reactor_polls);
        sat_add_u32(&g_metrics.reactor_ready, event_count);
    }
#else
    (void)round;
#endif

    (void)round;
    ready_count = asx_waker_drain_signaled(ready_tasks, ASX_LANE_TASK_CAPACITY);
    for (i = 0; i < ready_count;
         i++) { /* ASX_CHECKPOINT_WAIVER("bounded by ASX_LANE_TASK_CAPACITY") */
        parallel_promote_ready_task(region, ready_tasks[i]);
    }
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

        g_current_round = round;
        asx_trace_emit(ASX_TRACE_SCHED_ROUND, ASX_INVALID_ID, round);

        if (asx_budget_is_exhausted(budget)) { return parallel_return_budget(round); }

        parallel_drain_ready_sources(region, round);

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
                    sat_inc_u32(&g_metrics.fairness_yields);
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

                if (li == (int)ASX_LANE_TIMED && !g_task_ready_to_poll[slot_idx]) {
                    j++;
                    continue;
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
                    sat_inc_u32(&g_workers[worker_idx].tasks_completed);
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
                    sat_inc_u32(&g_workers[worker_idx].tasks_completed);
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
                sat_inc_u32(&g_workers[worker_idx].polls_total);
                any_polled = 1;

                /* Update per-lane dispatch metrics and cancel streak */
                if (li == (int)ASX_LANE_CANCEL) {
                    sat_inc_u32(&g_metrics.cancel_dispatches);
                    sat_inc_u32(&g_metrics.cancel_streak);
                    if (g_metrics.cancel_streak > g_metrics.cancel_streak_max)
                        g_metrics.cancel_streak_max = g_metrics.cancel_streak;
                } else {
                    g_metrics.cancel_streak = 0;
                    if (li == (int)ASX_LANE_TIMED)
                        sat_inc_u32(&g_metrics.timed_dispatches);
                    else
                        sat_inc_u32(&g_metrics.ready_dispatches);
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
                    sat_inc_u32(&g_workers[worker_idx].tasks_completed);
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
                    sat_inc_u32(&g_workers[worker_idx].tasks_completed);
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

/* -------------------------------------------------------------------
 * Admission and large-swarm telemetry
 * ------------------------------------------------------------------- */

void asx_parallel_admission_policy_init(asx_parallel_admission_policy *policy) {
    if (policy == NULL) { return; }
    policy->mode = ASX_PARALLEL_ADMISSION_REJECT;
    policy->pressure_threshold_pct = ASX_PARALLEL_DEFAULT_PRESSURE_THRESHOLD_PCT;
    policy->shed_max = 1u;
    policy->enforce = 0;
}

void asx_parallel_locality_config_init(asx_parallel_locality_config *locality) {
    if (locality == NULL) { return; }
    locality->mode = ASX_PARALLEL_LOCALITY_COMPACT;
    locality->shard_count = 0u;
    locality->tasks_per_shard = 0u;
}

const char *asx_parallel_locality_mode_str(asx_parallel_locality_mode mode) {
    switch (mode) {
    case ASX_PARALLEL_LOCALITY_COMPACT: return "compact";
    case ASX_PARALLEL_LOCALITY_WORKER_SHARDED: return "worker_sharded";
    case ASX_PARALLEL_LOCALITY_NUMA_DOMAIN_SHARDED: return "numa_domain_sharded";
    default: return "unknown";
    }
}

asx_status asx_parallel_get_locality_snapshot(asx_parallel_locality_snapshot *out) {
    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) { return ASX_E_INVALID_STATE; }
    parallel_fill_locality_snapshot(out);
    return ASX_OK;
}

asx_status asx_parallel_task_locality(asx_task_id tid, uint32_t *out_shard, uint32_t *out_worker) {
    asx_task_slot *slot;
    uint16_t slot_idx;
    uint32_t shard;

    if (out_shard == NULL || out_worker == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) { return ASX_E_INVALID_STATE; }
    if (asx_task_slot_lookup(tid, &slot) != ASX_OK) { return ASX_E_INVALID_ARGUMENT; }
    (void)slot;

    slot_idx = asx_handle_slot(tid);
    shard = parallel_slot_locality_shard(slot_idx);
    *out_shard = shard;
    *out_worker = parallel_locality_effective_owner(slot_idx);
    return ASX_OK;
}

asx_status asx_parallel_evaluate_admission(const asx_parallel_admission_policy *policy,
                                           uint32_t queued, uint32_t capacity,
                                           asx_parallel_admission_decision *decision) {
    asx_parallel_admission_policy normalized;
    uint32_t pressure_pct;

    if (policy == NULL || decision == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!admission_mode_valid(policy->mode)) { return ASX_E_INVALID_ARGUMENT; }
    if (policy->pressure_threshold_pct > 100u) { return ASX_E_INVALID_ARGUMENT; }

    normalized = *policy;
    normalize_admission_policy(&normalized);

    memset(decision, 0, sizeof(*decision));
    decision->mode = normalized.mode;
    decision->queued = queued;
    decision->capacity = capacity;

    if (capacity == 0u) {
        decision->triggered = 1;
        decision->pressure_pct = 100u;
        decision->admit_status = ASX_E_RESOURCE_EXHAUSTED;
        return ASX_OK;
    }

    pressure_pct = percent_u32(queued, capacity);
    decision->pressure_pct = pressure_pct;
    if (pressure_pct < normalized.pressure_threshold_pct) {
        decision->triggered = 0;
        decision->admit_status = ASX_OK;
        return ASX_OK;
    }

    decision->triggered = 1;
    switch (normalized.mode) {
    case ASX_PARALLEL_ADMISSION_REJECT: decision->admit_status = ASX_E_ADMISSION_CLOSED; break;
    case ASX_PARALLEL_ADMISSION_BACKPRESSURE: decision->admit_status = ASX_E_WOULD_BLOCK; break;
    case ASX_PARALLEL_ADMISSION_SHED_OLDEST:
        decision->shed_count = normalized.shed_max;
        if (decision->shed_count > queued) { decision->shed_count = queued; }
        decision->admit_status = ASX_OK;
        break;
    default: return ASX_E_INVALID_ARGUMENT;
    }

    return ASX_OK;
}

asx_status asx_parallel_set_admission_policy(const asx_parallel_admission_policy *policy) {
    asx_parallel_admission_policy normalized;

    if (policy == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) { return ASX_E_INVALID_STATE; }
    if (!admission_mode_valid(policy->mode)) { return ASX_E_INVALID_ARGUMENT; }
    if (policy->pressure_threshold_pct > 100u) { return ASX_E_INVALID_ARGUMENT; }

    normalized = *policy;
    normalize_admission_policy(&normalized);
    g_config.admission_policy = normalized;
    return ASX_OK;
}

asx_status asx_parallel_get_admission_policy(asx_parallel_admission_policy *out) {
    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) { return ASX_E_INVALID_STATE; }
    *out = g_config.admission_policy;
    return ASX_OK;
}

const char *asx_parallel_admission_mode_str(asx_parallel_admission_mode mode) {
    switch (mode) {
    case ASX_PARALLEL_ADMISSION_REJECT: return "reject";
    case ASX_PARALLEL_ADMISSION_BACKPRESSURE: return "backpressure";
    case ASX_PARALLEL_ADMISSION_SHED_OLDEST: return "shed_oldest";
    default: return "unknown";
    }
}

asx_status asx_parallel_get_telemetry_snapshot(asx_parallel_telemetry_snapshot *out) {
    uint32_t i;
    uint32_t total_polls = 0u;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!g_initialized) { return ASX_E_INVALID_STATE; }

    memset(out, 0, sizeof(*out));
    out->worker_count = g_config.worker_count;
    out->metrics = g_metrics;
    out->admission = g_last_admission;
    parallel_fill_locality_snapshot(&out->locality);
    parallel_fill_commit_authority_snapshot(&out->commit_authority);

    for (i = 0u; i < ASX_MAX_LANES; i++) {
        out->lane_depths[i] = g_lanes[i].count;
        out->total_queue_depth += g_lanes[i].count;
        if (g_lanes[i].count > out->max_lane_depth) { out->max_lane_depth = g_lanes[i].count; }
    }
    out->pressure_pct = percent_u32(out->max_lane_depth, ASX_LANE_TASK_CAPACITY);

    for (i = 0u; i < g_config.worker_count; i++) { total_polls += g_workers[i].polls_total; }

    for (i = 0u; i < g_config.worker_count; i++) {
        uint32_t lane_idx;
        uint32_t depth = 0u;

        for (lane_idx = 0u; lane_idx < ASX_MAX_LANES; lane_idx++) {
            depth += g_worker_lane_depths[i][lane_idx];
        }
        out->worker_queue_depths[i] = depth;
        if (depth > out->max_worker_queue_depth) {
            out->max_worker_queue_depth = depth;
            out->hot_worker = i;
        }

        if (total_polls > 0u) {
            out->worker_busy_permille[i] =
                (uint32_t)(((uint64_t)g_workers[i].polls_total * 1000u) / total_polls);
            out->worker_idle_permille[i] =
                out->worker_busy_permille[i] > 1000u ? 0u : 1000u - out->worker_busy_permille[i];
        } else {
            out->worker_busy_permille[i] = 0u;
            out->worker_idle_permille[i] = 1000u;
        }
    }

#if ASX_HAS_BLOCKING_SURFACE
    out->blocking_backlog = asx_blocking_active_count();
#else
    out->blocking_backlog = 0u;
#endif

    return ASX_OK;
}

asx_status asx_parallel_render_telemetry_jsonl(const asx_parallel_telemetry_snapshot *snapshot,
                                               char *buf, uint32_t buf_len, uint32_t *out_len) {
    char tmp[3072];
    int needed;

    if (snapshot == NULL || buf == NULL) { return ASX_E_INVALID_ARGUMENT; }

    needed = snprintf(
        tmp, sizeof(tmp),
        "{\"kind\":\"parallel_telemetry\","
        "\"worker_count\":%u,"
        "\"queue_depth\":%u,"
        "\"pressure_pct\":%u,"
        "\"max_lane_depth\":%u,"
        "\"max_worker_queue_depth\":%u,"
        "\"hot_worker\":%u,"
        "\"blocking_backlog\":%u,"
        "\"steals\":{\"attempts\":%u,\"succeeded\":%u,\"failed\":%u},"
        "\"cancel\":{\"streak\":%u,\"streak_max\":%u},"
        "\"timed\":{\"promotions\":%u,\"wake_latency_rounds_total\":%u,"
        "\"wake_latency_rounds_max\":%u},"
        "\"reactor\":{\"polls\":%u,\"ready\":%u,\"waker_ready\":%u},"
        "\"admission\":{\"triggered\":%d,\"mode\":\"%s\",\"pressure_pct\":%u,"
        "\"queued\":%u,\"capacity\":%u,\"shed_count\":%u,\"status_code\":%d,"
        "\"status_text\":\"%s\"},"
        "\"locality\":{\"mode\":\"%s\",\"shard_count\":%u,\"tasks_per_shard\":%u,"
        "\"hot_shard\":%u,\"max_shard_tasks\":%u},"
        "\"commit_authority\":{\"commit_sequence\":%u,\"total_worker_commits\":%u,"
        "\"max_worker_commit_sequence\":%u,\"drift_detected\":%u,"
        "\"native_live_enabled\":%u,\"native_live_status_code\":%d,"
        "\"native_live_status_text\":\"%s\"},"
        "\"worker0\":{\"queue_depth\":%u,\"busy_permille\":%u,\"idle_permille\":%u},"
        "\"rerun\":\"make parallel-parity\"}\n",
        (unsigned)snapshot->worker_count, (unsigned)snapshot->total_queue_depth,
        (unsigned)snapshot->pressure_pct, (unsigned)snapshot->max_lane_depth,
        (unsigned)snapshot->max_worker_queue_depth, (unsigned)snapshot->hot_worker,
        (unsigned)snapshot->blocking_backlog, (unsigned)snapshot->metrics.steal_attempts,
        (unsigned)snapshot->metrics.steals_succeeded, (unsigned)snapshot->metrics.steals_failed,
        (unsigned)snapshot->metrics.cancel_streak, (unsigned)snapshot->metrics.cancel_streak_max,
        (unsigned)snapshot->metrics.timed_promotions,
        (unsigned)snapshot->metrics.timed_wake_latency_rounds_total,
        (unsigned)snapshot->metrics.timed_wake_latency_rounds_max,
        (unsigned)snapshot->metrics.reactor_polls, (unsigned)snapshot->metrics.reactor_ready,
        (unsigned)snapshot->metrics.waker_ready, snapshot->admission.triggered,
        asx_parallel_admission_mode_str(snapshot->admission.mode),
        (unsigned)snapshot->admission.pressure_pct, (unsigned)snapshot->admission.queued,
        (unsigned)snapshot->admission.capacity, (unsigned)snapshot->admission.shed_count,
        (int)snapshot->admission.admit_status, asx_status_str(snapshot->admission.admit_status),
        asx_parallel_locality_mode_str(snapshot->locality.mode),
        (unsigned)snapshot->locality.shard_count, (unsigned)snapshot->locality.tasks_per_shard,
        (unsigned)snapshot->locality.hot_shard, (unsigned)snapshot->locality.max_shard_tasks,
        (unsigned)snapshot->commit_authority.commit_sequence,
        (unsigned)snapshot->commit_authority.total_worker_commits,
        (unsigned)snapshot->commit_authority.max_worker_commit_sequence,
        (unsigned)snapshot->commit_authority.drift_detected,
        (unsigned)snapshot->commit_authority.native_live_enabled,
        (int)snapshot->commit_authority.native_live_status,
        asx_status_str(snapshot->commit_authority.native_live_status),
        (unsigned)snapshot->worker_queue_depths[0], (unsigned)snapshot->worker_busy_permille[0],
        (unsigned)snapshot->worker_idle_permille[0]);

    if (needed < 0) { return ASX_E_INVALID_STATE; }
    if (out_len != NULL) { *out_len = (uint32_t)needed; }
    if ((uint32_t)needed >= sizeof(tmp)) { return ASX_E_RESOURCE_EXHAUSTED; }
    if (buf_len <= (uint32_t)needed) { return ASX_E_BUFFER_TOO_SMALL; }

    memcpy(buf, tmp, (size_t)needed + 1u);
    return ASX_OK;
}
