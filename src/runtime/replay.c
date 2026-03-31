/*
 * replay.c — replay oracles, snapshot/restore, counterexample minimization
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/replay.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
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
    asx_status st;
    if (lab == NULL) return ASX_E_INVALID_ARGUMENT;
    if (id.slot >= ASX_SNAPSHOT_MAX) return ASX_E_INVALID_ARGUMENT;
    snap = &g_snapshots[id.slot];
    if (!snap->valid) return ASX_E_INVALID_STATE;

    /* Rebuild the embedded lab runtime so deterministic hooks remain installed
     * after restore, then apply the saved PRNG and virtual-time state. */
    asx_lab_shutdown(lab);
    st = asx_lab_init(lab, &snap->config);
    if (st != ASX_OK) return st;
    lab->entropy_state = snap->entropy_state;
    lab->vtime.current_time = snap->current_time;

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

static void replay_append_json_escaped(asx_report_buf *out, const char *text) {
    const unsigned char *p;

    if (out == NULL || text == NULL) return;

    p = (const unsigned char *)text;
    while (*p != '\0') {
        switch (*p) {
        case '\"': asx_report_buf_append(out, "\\\""); break;
        case '\\': asx_report_buf_append(out, "\\\\"); break;
        case '\b': asx_report_buf_append(out, "\\b"); break;
        case '\f': asx_report_buf_append(out, "\\f"); break;
        case '\n': asx_report_buf_append(out, "\\n"); break;
        case '\r': asx_report_buf_append(out, "\\r"); break;
        case '\t': asx_report_buf_append(out, "\\t"); break;
        default:
            if (*p < 0x20u) {
                static const char hex[] = "0123456789abcdef";
                char esc[7];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(*p >> 4) & 0x0fu];
                esc[5] = hex[*p & 0x0fu];
                esc[6] = '\0';
                asx_report_buf_append(out, esc);
            } else {
                char ch[2];
                ch[0] = (char)*p;
                ch[1] = '\0';
                asx_report_buf_append(out, ch);
            }
            break;
        }
        p++;
    }
}

static void replay_append_event_json(asx_report_buf *out, const asx_trace_event *event) {
    if (out == NULL) return;
    if (event == NULL) {
        asx_report_buf_append(out, "null");
        return;
    }

    asx_report_buf_append(out, "{\"sequence\":");
    asx_report_buf_append_u32(out, event->sequence);
    asx_report_buf_append(out, ",\"kind\":\"");
    replay_append_json_escaped(out, asx_trace_event_kind_str(event->kind));
    asx_report_buf_append(out, "\",\"entity_id\":");
    asx_report_buf_append_hex64(out, event->entity_id);
    asx_report_buf_append(out, ",\"aux\":");
    asx_report_buf_append_hex64(out, event->aux);
    asx_report_buf_append(out, "}");
}

static void replay_append_result_json(asx_report_buf *out, const asx_replay_result *result) {
    if (out == NULL || result == NULL) return;

    asx_report_buf_append(out, "{\"result\":\"");
    replay_append_json_escaped(out, asx_replay_result_kind_str(result->result));
    asx_report_buf_append(out, "\",\"divergence_index\":");
    asx_report_buf_append_u32(out, result->divergence_index);
    asx_report_buf_append(out, ",\"expected_digest\":");
    asx_report_buf_append_hex64(out, result->expected_digest);
    asx_report_buf_append(out, ",\"actual_digest\":");
    asx_report_buf_append_hex64(out, result->actual_digest);
    asx_report_buf_append(out, "}");
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
    if (r1->steps_total != r2->steps_total)
        return make_result(ASX_ORACLE_FAIL, "replay", "total step contract diverged");
    if (r1->elapsed_ns != r2->elapsed_ns)
        return make_result(ASX_ORACLE_FAIL, "replay", "elapsed time diverged");
    if (r1->polls_total != r2->polls_total)
        return make_result(ASX_ORACLE_FAIL, "replay", "poll count diverged");
    if (r1->last_status != r2->last_status)
        return make_result(ASX_ORACLE_FAIL, "replay", "final status diverged");

    return make_result(ASX_ORACLE_PASS, "replay", "deterministic match");
}

asx_status asx_replay_render_result_json(const asx_replay_result *result, asx_report_buf *out) {
    if (result == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_init(out);
    replay_append_result_json(out, result);
    return ASX_OK;
}

asx_status asx_replay_render_current_diff_json(asx_report_buf *out) {
    asx_replay_result result;
    uint32_t current_count;
    uint32_t reference_count;
    asx_trace_event expected_event;
    asx_trace_event actual_event;
    int have_expected;
    int have_actual;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    result = asx_replay_verify();
    current_count = asx_trace_event_count();
    reference_count = asx_replay_reference_event_count();
    have_expected = asx_replay_reference_event_get(result.divergence_index, &expected_event);
    have_actual = asx_trace_event_get(result.divergence_index, &actual_event);

    asx_report_buf_init(out);
    asx_report_buf_append(out, "{\"reference_loaded\":");
    asx_report_buf_append(out, reference_count > 0u ? "true" : "false");
    asx_report_buf_append(out, ",\"reference_count\":");
    asx_report_buf_append_u32(out, reference_count);
    asx_report_buf_append(out, ",\"current_count\":");
    asx_report_buf_append_u32(out, current_count);
    asx_report_buf_append(out, ",\"verification\":");
    replay_append_result_json(out, &result);
    asx_report_buf_append(out, ",\"expected_event\":");
    replay_append_event_json(out, have_expected ? &expected_event : NULL);
    asx_report_buf_append(out, ",\"actual_event\":");
    replay_append_event_json(out, have_actual ? &actual_event : NULL);
    asx_report_buf_append(out, "}");

    return ASX_OK;
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

asx_status asx_minimize_run(asx_minimize_state *state) {
    asx_status st;

    if (state == NULL) return ASX_E_INVALID_ARGUMENT;

    do { st = asx_minimize_step(state); } while (st == ASX_E_PENDING);

    return st;
}

const asx_lab_scenario *asx_minimize_result(const asx_minimize_state *state) {
    if (state == NULL) return NULL;
    return &state->scenario;
}

uint32_t asx_minimize_attempts(const asx_minimize_state *state) {
    if (state == NULL) return 0;
    return state->attempts;
}

asx_status asx_minimize_render_json(const asx_minimize_state *state, asx_report_buf *out) {
    if (state == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_init(out);
    asx_report_buf_append(out, "{\"scenario_name\":\"");
    replay_append_json_escaped(out, state->scenario.name != NULL ? state->scenario.name : "");
    asx_report_buf_append(out, "\",\"original_steps\":");
    asx_report_buf_append_u32(out, state->original_steps);
    asx_report_buf_append(out, ",\"current_steps\":");
    asx_report_buf_append_u32(out, state->scenario.step_count);
    asx_report_buf_append(out, ",\"attempts\":");
    asx_report_buf_append_u32(out, state->attempts);
    asx_report_buf_append(out, ",\"max_attempts\":");
    asx_report_buf_append_u32(out, state->max_attempts);
    asx_report_buf_append(out, ",\"found_smaller\":");
    asx_report_buf_append(out, state->found_smaller ? "true" : "false");
    asx_report_buf_append(out, "}");
    return ASX_OK;
}
