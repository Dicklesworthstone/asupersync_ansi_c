/*
 * asx/runtime/lab.h — seeded lab runtime for deterministic testing
 *
 * Provides a high-level entrypoint that wires together virtual time,
 * seeded entropy, scenario configuration, and runtime init into a
 * single deterministic lab runtime. This is the bridge from low-level
 * testability to product-level reproducibility.
 *
 * Usage:
 *   asx_lab lab;
 *   asx_lab_config cfg;
 *   asx_lab_config_init(&cfg);
 *   cfg.seed = 42;
 *   cfg.tick_ns = 1000000;  // 1ms per tick
 *
 *   asx_lab_init(&lab, &cfg);
 *   // ... run scenarios ...
 *   asx_lab_shutdown(&lab);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_LAB_H
#define ASX_RUNTIME_LAB_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/virtual_time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Lab configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t seed;          /* PRNG seed for deterministic entropy */
    uint64_t tick_ns;       /* Virtual time tick (ns per query), 0 = 1ms default */
    asx_time start_time_ns; /* Virtual time start (0 = default) */
    uint32_t max_polls;     /* Max polls per scenario step (0 = 1024 default) */
} asx_lab_config;

/* Initialize config with defaults. */
ASX_API void asx_lab_config_init(asx_lab_config *cfg);

/* -------------------------------------------------------------------
 * Lab runtime
 * ------------------------------------------------------------------- */

typedef struct {
    asx_runtime rt;
    asx_vtime_state vtime;
    asx_lab_config config;
    uint64_t entropy_state; /* seeded PRNG state */
    int initialized;
} asx_lab;

/* Initialize the lab runtime with deterministic config.
 * Sets up virtual time, seeded entropy, and runtime hooks. */
ASX_API ASX_MUST_USE asx_status asx_lab_init(asx_lab *lab, const asx_lab_config *cfg);

/* Shut down the lab runtime and restore hooks. */
ASX_API void asx_lab_shutdown(asx_lab *lab);

/* Reset the lab runtime to initial state (preserves config) and
 * re-bootstrap the deterministic runtime hook table. */
ASX_API void asx_lab_reset(asx_lab *lab);

/* -------------------------------------------------------------------
 * Time control
 * ------------------------------------------------------------------- */

/* Advance virtual time by n ticks. */
ASX_API void asx_lab_advance_time(asx_lab *lab, uint32_t ticks);

/* Get current virtual time. */
ASX_API asx_time asx_lab_now(const asx_lab *lab);

/* -------------------------------------------------------------------
 * Scenario execution
 * ------------------------------------------------------------------- */

/* A scenario step: poll function + user data. */
typedef asx_status (*asx_lab_step_fn)(asx_lab *lab, void *user_data);

/* Scenario definition: a sequence of steps. */
#ifndef ASX_LAB_MAX_STEPS
#define ASX_LAB_MAX_STEPS 32u
#endif

typedef struct {
    const char *name;
    asx_lab_step_fn steps[ASX_LAB_MAX_STEPS];
    void *step_data[ASX_LAB_MAX_STEPS];
    uint32_t step_count;
} asx_lab_scenario;

/* Initialize a scenario. */
ASX_API void asx_lab_scenario_init(asx_lab_scenario *sc, const char *name);

/* Add a step to a scenario. */
ASX_API ASX_MUST_USE asx_status asx_lab_scenario_add_step(asx_lab_scenario *sc,
                                                          asx_lab_step_fn step_fn, void *user_data);

/* -------------------------------------------------------------------
 * Scenario result
 * ------------------------------------------------------------------- */

typedef struct {
    const char *scenario_name;
    uint32_t steps_completed;
    uint32_t steps_total;
    asx_status last_status; /* status of last step run */
    asx_time elapsed_ns;    /* virtual time elapsed */
    uint64_t polls_total;   /* total polls across all steps */
} asx_lab_result;

/* Run a scenario in the lab runtime.
 * Executes each step in order. Stops on first error.
 * Fails closed with ASX_E_INVALID_STATE if the scenario descriptor is
 * malformed (step_count overflow or NULL step entry inside the active range).
 * Returns ASX_OK if all steps succeed. */
ASX_API ASX_MUST_USE asx_status asx_lab_run_scenario(asx_lab *lab, const asx_lab_scenario *scenario,
                                                     asx_lab_result *out_result);

/* -------------------------------------------------------------------
 * Seeded entropy
 * ------------------------------------------------------------------- */

/* Get a deterministic random u64 from the lab's PRNG. */
ASX_API uint64_t asx_lab_random_u64(asx_lab *lab);

/* -------------------------------------------------------------------
 * Convenience: open region in lab context
 * ------------------------------------------------------------------- */

ASX_API ASX_MUST_USE asx_status asx_lab_open_region(asx_lab *lab, asx_region_id *out_id);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_LAB_H */
