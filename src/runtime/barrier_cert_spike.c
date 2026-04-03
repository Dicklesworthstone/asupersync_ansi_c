/*
 * barrier_cert_spike.c — SOS barrier-certificate style bounds for adaptive
 * scheduler safety (bd-3vt.6)
 *
 * Implements a lightweight barrier-function evaluator that verifies
 * Lyapunov-style decrease conditions on task wait counters. This is a
 * discrete approximation of SOS barrier certificates adapted for the
 * finite-state, deterministic scheduler model.
 *
 * Key insight: full SOS (Sum-of-Squares) polynomial optimization requires
 * offline semidefinite programming. We instead implement the *runtime
 * verification* side: given a pre-computed barrier function B(x), check
 * that B decreases along observed trajectories.
 *
 * Deferred stub status: research spike. This file is an evaluation surface,
 * not a claim that SOS/barrier-certificate machinery is part of shipped
 * runtime policy. See docs/DEFERRED_STUBS_REGISTER.md.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- barrier cert spike, no checkpoint coverage needed */

#include <asx/asx.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Barrier function types                                             */
/* ------------------------------------------------------------------ */

#define ASX_BARRIER_MAX_TASKS 64u

/*
 * A barrier function B(x) maps scheduler state to a non-negative scalar.
 * Safety property: B(x) > 0 for all reachable states implies the system
 * never enters the unsafe set.
 *
 * For starvation prevention:
 *   x_i = rounds since task i was last polled
 *   B(x) = K - max(x_i)   where K is the starvation bound
 *
 * B(x) > 0 iff no task has waited more than K rounds.
 *
 * Decrease condition (discrete Lyapunov):
 *   B(x') - B(x) < 0  (or <= -epsilon for strict decrease)
 *   where x' is the state after one scheduler round.
 */

typedef struct {
    uint32_t wait_counts[ASX_BARRIER_MAX_TASKS]; /* rounds since last poll */
    uint32_t task_count;
    uint32_t round;
} asx_barrier_state;

typedef struct {
    uint32_t starvation_bound; /* K: max allowed wait rounds */
    uint32_t alive_count;      /* tasks that should be polled */
} asx_barrier_config;

typedef struct {
    int32_t value;           /* B(x) = K - max(wait) */
    int safe;                /* 1 if B(x) > 0 */
    int decreasing;          /* 1 if B decreased since last eval */
    uint32_t max_wait;       /* current max wait across all tasks */
    uint32_t violator_index; /* task with highest wait (or 0) */
} asx_barrier_result;

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

void asx_barrier_state_init(asx_barrier_state *state, uint32_t task_count);
void asx_barrier_record_poll(asx_barrier_state *state, uint32_t task_index);
void asx_barrier_advance_round(asx_barrier_state *state);
void asx_barrier_evaluate(const asx_barrier_state *state, const asx_barrier_config *cfg,
                          int32_t prev_value, asx_barrier_result *result);
uint32_t asx_barrier_max_wait(const asx_barrier_state *state);
int asx_barrier_admits_bound(const asx_barrier_config *cfg, uint32_t task_count);

/* ------------------------------------------------------------------ */
/* Implementation                                                     */
/* ------------------------------------------------------------------ */

void asx_barrier_state_init(asx_barrier_state *state, uint32_t task_count) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    state->task_count = (task_count > ASX_BARRIER_MAX_TASKS) ? ASX_BARRIER_MAX_TASKS : task_count;
}

void asx_barrier_record_poll(asx_barrier_state *state, uint32_t task_index) {
    if (state == NULL || task_index >= state->task_count) return;
    state->wait_counts[task_index] = 0;
}

void asx_barrier_advance_round(asx_barrier_state *state) {
    uint32_t i;
    if (state == NULL) return;

    for (i = 0; i < state->task_count; i++) {
        if (state->wait_counts[i] < UINT32_MAX) { state->wait_counts[i]++; }
    }
    state->round++;
}

uint32_t asx_barrier_max_wait(const asx_barrier_state *state) {
    uint32_t i;
    uint32_t max_w = 0;

    if (state == NULL) return 0;

    for (i = 0; i < state->task_count; i++) {
        if (state->wait_counts[i] > max_w) { max_w = state->wait_counts[i]; }
    }
    return max_w;
}

void asx_barrier_evaluate(const asx_barrier_state *state, const asx_barrier_config *cfg,
                          int32_t prev_value, asx_barrier_result *result) {
    uint32_t i;
    uint32_t max_w = 0;
    uint32_t violator = 0;

    if (result == NULL) return;
    memset(result, 0, sizeof(*result));

    if (state == NULL || cfg == NULL) return;

    for (i = 0; i < state->task_count; i++) {
        if (state->wait_counts[i] > max_w) {
            max_w = state->wait_counts[i];
            violator = i;
        }
    }

    result->max_wait = max_w;
    result->violator_index = violator;
    result->value = (int32_t)cfg->starvation_bound - (int32_t)max_w;
    result->safe = (result->value > 0) ? 1 : 0;
    result->decreasing = (result->value < prev_value) ? 1 : 0;
}

int asx_barrier_admits_bound(const asx_barrier_config *cfg, uint32_t task_count) {
    /*
     * For round-robin scheduling with N tasks and budget >= N,
     * each task is guaranteed a poll every round. So the starvation
     * bound K must satisfy K >= 1 for round-robin.
     *
     * For budget < N (partial rounds), the worst case is:
     * max_wait = ceil(N / budget). We require K > ceil(N / budget).
     *
     * For simplicity, we check K > 0 and alive_count <= task_count.
     */
    if (cfg == NULL) return 0;
    if (cfg->starvation_bound == 0) return 0;
    if (cfg->alive_count > task_count) return 0;
    return 1;
}
