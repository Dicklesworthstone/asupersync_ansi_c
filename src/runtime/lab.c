/*
 * lab.c — seeded lab runtime for deterministic testing
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/lab.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Seeded PRNG (splitmix64)                                            */
/* ------------------------------------------------------------------ */

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z;
    *state += 0x9e3779b97f4a7c15ULL;
    z = *state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Entropy hook callback */
static uint64_t lab_entropy_u64(void *ctx) {
    asx_lab *lab = (asx_lab *)ctx;
    return splitmix64(&lab->entropy_state);
}

/* ------------------------------------------------------------------ */
/* Lab config                                                          */
/* ------------------------------------------------------------------ */

void asx_lab_config_init(asx_lab_config *cfg) {
    if (cfg == NULL) return;
    cfg->seed = 0;
    cfg->tick_ns = 1000000ULL; /* 1ms default tick */
    cfg->start_time_ns = 0;
    cfg->max_polls = 1024;
}

/* ------------------------------------------------------------------ */
/* Lab lifecycle                                                       */
/* ------------------------------------------------------------------ */

static asx_status lab_bootstrap_runtime(asx_lab *lab) {
    asx_runtime_config rt_cfg;
    asx_runtime_hooks hooks;

    /* Set up runtime config */
    asx_runtime_config_init(&rt_cfg);

    /* Set up hooks with virtual time and seeded entropy */
    asx_runtime_hooks_init(&hooks);
    hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
    hooks.clock.ctx = &lab->vtime;
    hooks.entropy.random_u64_fn = lab_entropy_u64;
    hooks.entropy.ctx = lab;
    hooks.deterministic_seeded_prng = 1;

    return asx_runtime_init(&lab->rt, &rt_cfg, &hooks);
}

asx_status asx_lab_init(asx_lab *lab, const asx_lab_config *cfg) {
    asx_status st;

    if (lab == NULL || cfg == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(lab, 0, sizeof(*lab));
    lab->config = *cfg;

    /* Apply defaults */
    if (lab->config.tick_ns == 0) lab->config.tick_ns = 1000000ULL;
    if (lab->config.max_polls == 0) lab->config.max_polls = 1024;

    /* Initialize seeded PRNG */
    lab->entropy_state = lab->config.seed;

    /* Initialize virtual time */
    asx_vtime_init(&lab->vtime, lab->config.start_time_ns, lab->config.tick_ns);

    st = lab_bootstrap_runtime(lab);
    if (st != ASX_OK) return st;

    lab->initialized = 1;
    return ASX_OK;
}

void asx_lab_shutdown(asx_lab *lab) {
    if (lab == NULL) return;
    if (lab->initialized) {
        asx_runtime_shutdown(&lab->rt);
        lab->initialized = 0;
    }
}

void asx_lab_reset(asx_lab *lab) {
    asx_status st;

    if (lab == NULL || !lab->initialized) return;
    asx_runtime_shutdown(&lab->rt);
    asx_vtime_init(&lab->vtime, lab->config.start_time_ns, lab->config.tick_ns);
    lab->entropy_state = lab->config.seed;
    st = lab_bootstrap_runtime(lab);
    if (st != ASX_OK) lab->initialized = 0;
}

/* ------------------------------------------------------------------ */
/* Time control                                                        */
/* ------------------------------------------------------------------ */

void asx_lab_advance_time(asx_lab *lab, uint32_t ticks) {
    uint64_t delta;
    if (lab == NULL) return;
    delta = (uint64_t)ticks * lab->vtime.tick_ns;
    if (lab->vtime.current_time <= UINT64_MAX - delta) {
        lab->vtime.current_time += delta;
    } else {
        lab->vtime.current_time = UINT64_MAX;
    }
}

asx_time asx_lab_now(const asx_lab *lab) {
    if (lab == NULL) return 0;
    return asx_vtime_current(&lab->vtime);
}

/* ------------------------------------------------------------------ */
/* Scenario                                                            */
/* ------------------------------------------------------------------ */

void asx_lab_scenario_init(asx_lab_scenario *sc, const char *name) {
    if (sc == NULL) return;
    memset(sc, 0, sizeof(*sc));
    sc->name = name;
}

asx_status asx_lab_scenario_add_step(asx_lab_scenario *sc, asx_lab_step_fn step_fn,
                                     void *user_data) {
    if (sc == NULL || step_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sc->step_count >= ASX_LAB_MAX_STEPS) return ASX_E_RESOURCE_EXHAUSTED;
    sc->steps[sc->step_count] = step_fn;
    sc->step_data[sc->step_count] = user_data;
    sc->step_count++;
    return ASX_OK;
}

asx_status asx_lab_run_scenario(asx_lab *lab, const asx_lab_scenario *scenario,
                                asx_lab_result *out_result) {
    uint32_t i;
    asx_status st;
    asx_time start_time;

    if (lab == NULL || scenario == NULL || out_result == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!lab->initialized) return ASX_E_INVALID_STATE;
    if (scenario->step_count > ASX_LAB_MAX_STEPS) return ASX_E_INVALID_STATE;

    memset(out_result, 0, sizeof(*out_result));
    out_result->scenario_name = scenario->name;
    out_result->steps_total = scenario->step_count;
    start_time = asx_lab_now(lab);

    for (i = 0; i < scenario->step_count; i++) {
        if (scenario->steps[i] == NULL) {
            out_result->last_status = ASX_E_INVALID_STATE;
            out_result->elapsed_ns = asx_lab_now(lab) - start_time;
            return ASX_E_INVALID_STATE;
        }
        st = scenario->steps[i](lab, scenario->step_data[i]);
        out_result->steps_completed++;
        out_result->last_status = st;
        if (st != ASX_OK) {
            out_result->elapsed_ns = asx_lab_now(lab) - start_time;
            return st;
        }
    }

    out_result->elapsed_ns = asx_lab_now(lab) - start_time;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Seeded entropy                                                      */
/* ------------------------------------------------------------------ */

uint64_t asx_lab_random_u64(asx_lab *lab) {
    if (lab == NULL) return 0;
    return splitmix64(&lab->entropy_state);
}

/* ------------------------------------------------------------------ */
/* Convenience                                                         */
/* ------------------------------------------------------------------ */

asx_status asx_lab_open_region(asx_lab *lab, asx_region_id *out_id) {
    if (lab == NULL || out_id == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!lab->initialized) return ASX_E_INVALID_STATE;
    return asx_region_open(out_id);
}
