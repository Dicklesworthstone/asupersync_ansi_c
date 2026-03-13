/*
 * replay.c — replay oracles, snapshot/restore, counterexample minimization
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/runtime/replay.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Snapshot arena                                                      */
/* ------------------------------------------------------------------ */

static asx_snapshot g_snapshots[ASX_SNAPSHOT_MAX];
static uint32_t g_snapshot_count;

asx_status asx_snapshot_take(const asx_lab *lab, asx_snapshot_id *out) {
    uint32_t i;
    if (lab == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!lab->initialized) return ASX_E_INVALID_STATE;

    for (i = 0; i < ASX_SNAPSHOT_MAX; i++) {
        if (!g_snapshots[i].valid) {
            g_snapshots[i].config = lab->config;
            g_snapshots[i].entropy_state = lab->entropy_state;
            g_snapshots[i].current_time = lab->vtime.current_time;
            g_snapshots[i].valid = 1;
            out->slot = i;
            if (i >= g_snapshot_count) g_snapshot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_snapshot_restore(asx_lab *lab, asx_snapshot_id id) {
    asx_snapshot *snap;
    if (lab == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id.slot >= ASX_SNAPSHOT_MAX) return ASX_E_INVALID_ARGUMENT;
    snap = &g_snapshots[id.slot];
    if (!snap->valid) return ASX_E_INVALID_STATE;

    /* Restore PRNG and virtual time to snapshot state */
    lab->entropy_state = snap->entropy_state;
    asx_vtime_init(&lab->vtime, snap->current_time, snap->config.tick_ns);

    /* Reset runtime subsystems for clean replay */
    asx_runtime_reset();

    return ASX_OK;
}

asx_status asx_snapshot_discard(asx_snapshot_id id) {
    if (id.slot >= ASX_SNAPSHOT_MAX) return ASX_E_INVALID_ARGUMENT;
    if (!g_snapshots[id.slot].valid) return ASX_E_INVALID_STATE;
    g_snapshots[id.slot].valid = 0;
    return ASX_OK;
}

int asx_snapshot_is_valid(asx_snapshot_id id) {
    if (id.slot >= ASX_SNAPSHOT_MAX) return 0;
    return g_snapshots[id.slot].valid;
}

void asx_snapshot_reset(void) {
    memset(g_snapshots, 0, sizeof(g_snapshots));
    g_snapshot_count = 0;
}

/* ------------------------------------------------------------------ */
/* Built-in oracles                                                    */
/* ------------------------------------------------------------------ */

static asx_oracle_result make_result(asx_oracle_verdict verdict, const char *name,
                                     const char *msg) {
    asx_oracle_result r;
    r.verdict = verdict;
    r.oracle_name = name;
    r.message = msg;
    r.detail_count = 0;
    return r;
}

asx_oracle_result asx_oracle_quiescence(const asx_lab *lab, void *ctx) {
    (void)ctx;
    if (lab == NULL) return make_result(ASX_ORACLE_FAIL, "quiescence", "null lab");
    if (!lab->initialized)
        return make_result(ASX_ORACLE_INCONCLUSIVE, "quiescence", "lab not initialized");

    /* Check if runtime has active tasks */
    {
        uint32_t i;
        for (i = 0; i < g_task_count; i++) {
            if (g_tasks[i].alive && g_tasks[i].state < ASX_TASK_COMPLETED)
                return make_result(ASX_ORACLE_FAIL, "quiescence", "tasks still active");
        }
    }
    return make_result(ASX_ORACLE_PASS, "quiescence", "all tasks quiescent");
}

asx_oracle_result asx_oracle_leak(const asx_lab *lab, void *ctx) {
    (void)ctx;
    if (lab == NULL) return make_result(ASX_ORACLE_FAIL, "leak", "null lab");
    if (!lab->initialized)
        return make_result(ASX_ORACLE_INCONCLUSIVE, "leak", "lab not initialized");

    {
        uint32_t i;
        for (i = 0; i < g_region_count; i++) {
            if (g_regions[i].alive && g_regions[i].state != ASX_REGION_CLOSED)
                return make_result(ASX_ORACLE_FAIL, "leak", "regions still open");
        }
    }
    return make_result(ASX_ORACLE_PASS, "leak", "no resource leaks detected");
}

asx_oracle_result asx_oracle_replay_match(const asx_lab_result *r1, const asx_lab_result *r2) {
    if (r1 == NULL || r2 == NULL) return make_result(ASX_ORACLE_FAIL, "replay", "null result");

    if (r1->steps_completed != r2->steps_completed)
        return make_result(ASX_ORACLE_FAIL, "replay", "step count diverged");
    if (r1->elapsed_ns != r2->elapsed_ns)
        return make_result(ASX_ORACLE_FAIL, "replay", "elapsed time diverged");
    if (r1->last_status != r2->last_status)
        return make_result(ASX_ORACLE_FAIL, "replay", "final status diverged");

    return make_result(ASX_ORACLE_PASS, "replay", "deterministic match");
}

/* ------------------------------------------------------------------ */
/* Oracle suite                                                        */
/* ------------------------------------------------------------------ */

void asx_oracle_suite_init(asx_oracle_suite *suite) {
    if (suite == NULL) return;
    memset(suite, 0, sizeof(*suite));
}

asx_status asx_oracle_suite_add(asx_oracle_suite *suite, asx_oracle_fn oracle, void *ctx) {
    if (suite == NULL || oracle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (suite->count >= ASX_ORACLE_SUITE_MAX) return ASX_E_RESOURCE_EXHAUSTED;
    suite->oracles[suite->count] = oracle;
    suite->oracle_ctx[suite->count] = ctx;
    suite->count++;
    return ASX_OK;
}

asx_status asx_oracle_suite_run(asx_oracle_suite *suite, const asx_lab *lab) {
    uint32_t i;
    int any_fail = 0;

    if (suite == NULL || lab == NULL) return ASX_E_INVALID_ARGUMENT;

    suite->pass_count = 0;
    suite->fail_count = 0;

    for (i = 0; i < suite->count; i++) {
        suite->results[i] = suite->oracles[i](lab, suite->oracle_ctx[i]);
        if (suite->results[i].verdict == ASX_ORACLE_PASS)
            suite->pass_count++;
        else if (suite->results[i].verdict == ASX_ORACLE_FAIL) {
            suite->fail_count++;
            any_fail = 1;
        }
    }

    return any_fail ? ASX_E_INVALID_STATE : ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Counterexample minimization                                         */
/* ------------------------------------------------------------------ */

asx_status asx_minimize_init(asx_minimize_state *state, const asx_lab_config *config,
                             const asx_lab_scenario *failing_scenario, asx_oracle_fn oracle,
                             void *oracle_ctx, uint32_t max_attempts) {
    if (state == NULL || config == NULL || failing_scenario == NULL || oracle == NULL)
        return ASX_E_INVALID_ARGUMENT;

    state->config = config;
    state->scenario = *failing_scenario; /* copy the scenario */
    state->oracle = oracle;
    state->oracle_ctx = oracle_ctx;
    state->original_steps = failing_scenario->step_count;
    state->attempts = 0;
    state->max_attempts = max_attempts;
    state->found_smaller = 0;
    return ASX_OK;
}

asx_status asx_minimize_step(asx_minimize_state *state) {
    asx_lab lab;
    asx_lab_result result;
    asx_oracle_result oracle_result;
    asx_lab_scenario candidate;
    uint32_t try_remove;
    uint32_t i, j;
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (state->attempts >= state->max_attempts) return ASX_OK;
    if (state->scenario.step_count <= 1) return ASX_OK;

    /* Try removing each step from the end toward the front */
    try_remove = state->scenario.step_count - 1 - (state->attempts % state->scenario.step_count);
    if (try_remove >= state->scenario.step_count) try_remove = 0;

    /* Build candidate without step[try_remove] */
    asx_lab_scenario_init(&candidate, state->scenario.name);
    j = 0;
    for (i = 0; i < state->scenario.step_count; i++) {
        if (i == try_remove) continue;
        candidate.steps[j] = state->scenario.steps[i];
        candidate.step_data[j] = state->scenario.step_data[i];
        j++;
    }
    candidate.step_count = j;

    state->attempts++;

    /* Run candidate and check oracle */
    st = asx_lab_init(&lab, state->config);
    if (st != ASX_OK) return ASX_E_PENDING;

    st = asx_lab_run_scenario(&lab, &candidate, &result);
    (void)st; /* we care about oracle verdict, not scenario result */

    oracle_result = state->oracle(&lab, state->oracle_ctx);
    asx_lab_shutdown(&lab);

    if (oracle_result.verdict == ASX_ORACLE_FAIL) {
        /* Candidate still fails — accept the smaller scenario */
        state->scenario = candidate;
        state->found_smaller = 1;
    }

    if (state->attempts >= state->max_attempts || state->scenario.step_count <= 1) return ASX_OK;

    return ASX_E_PENDING;
}

const asx_lab_scenario *asx_minimize_result(const asx_minimize_state *state) {
    if (state == NULL) return NULL;
    return &state->scenario;
}

uint32_t asx_minimize_attempts(const asx_minimize_state *state) {
    if (state == NULL) return 0;
    return state->attempts;
}
