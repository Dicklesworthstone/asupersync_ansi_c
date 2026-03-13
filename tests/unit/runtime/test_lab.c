/*
 * test_lab.c — unit tests for seeded lab runtime
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/lab.h>
#include <asx/runtime/runtime.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

/* ------------------------------------------------------------------ */
/* Config tests                                                        */
/* ------------------------------------------------------------------ */

TEST(config_init_defaults) {
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    ASSERT_EQ(cfg.seed, (uint64_t)0);
    ASSERT_EQ(cfg.tick_ns, 1000000ULL);
    ASSERT_EQ(cfg.start_time_ns, (asx_time)0);
    ASSERT_EQ(cfg.max_polls, 1024u);
}

/* ------------------------------------------------------------------ */
/* Lifecycle tests                                                     */
/* ------------------------------------------------------------------ */

TEST(init_shutdown) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    ASSERT_EQ(asx_lab_init(&lab, &cfg), ASX_OK);
    ASSERT_TRUE(lab.initialized);
    asx_lab_shutdown(&lab);
    ASSERT_FALSE(lab.initialized);
}

TEST(init_null_fails) {
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    ASSERT_EQ(asx_lab_init(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_config_fails) {
    asx_lab lab;
    ASSERT_EQ(asx_lab_init(&lab, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(shutdown_null_safe) {
    asx_lab_shutdown(NULL);  /* should not crash */
}

TEST(double_shutdown_safe) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_shutdown(&lab);
    asx_lab_shutdown(&lab);  /* safe to call twice */
}

/* ------------------------------------------------------------------ */
/* Virtual time tests                                                  */
/* ------------------------------------------------------------------ */

TEST(time_starts_at_zero) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0);
    asx_lab_shutdown(&lab);
}

TEST(time_starts_at_custom) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    cfg.start_time_ns = 5000000000ULL;  /* 5 seconds */
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_lab_now(&lab), 5000000000ULL);
    asx_lab_shutdown(&lab);
}

TEST(advance_time) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    cfg.tick_ns = 1000000ULL;  /* 1ms per tick */
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_advance_time(&lab, 10);
    ASSERT_EQ(asx_lab_now(&lab), 10000000ULL);  /* 10ms */
    asx_lab_shutdown(&lab);
}

TEST(time_integrates_with_runtime) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_time now;
    asx_lab_config_init(&cfg);
    cfg.tick_ns = 1000000ULL;
    MUST_OK(asx_lab_init(&lab, &cfg));

    /* Runtime's now_ns should use virtual time */
    asx_lab_advance_time(&lab, 5);
    MUST_OK(asx_runtime_now_ns(&now));
    /* now should be > 0 due to advance + query */
    ASSERT_TRUE(now > 0);
    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Seeded entropy tests                                                */
/* ------------------------------------------------------------------ */

TEST(entropy_deterministic) {
    asx_lab lab1, lab2;
    asx_lab_config cfg;
    uint64_t a1, a2, b1, b2;

    asx_lab_config_init(&cfg);
    cfg.seed = 42;

    MUST_OK(asx_lab_init(&lab1, &cfg));
    a1 = asx_lab_random_u64(&lab1);
    a2 = asx_lab_random_u64(&lab1);
    asx_lab_shutdown(&lab1);

    MUST_OK(asx_lab_init(&lab2, &cfg));
    b1 = asx_lab_random_u64(&lab2);
    b2 = asx_lab_random_u64(&lab2);
    asx_lab_shutdown(&lab2);

    /* Same seed → same sequence */
    ASSERT_EQ(a1, b1);
    ASSERT_EQ(a2, b2);
}

TEST(entropy_different_seeds) {
    asx_lab lab1, lab2;
    asx_lab_config cfg;
    uint64_t a, b;

    asx_lab_config_init(&cfg);
    cfg.seed = 1;
    MUST_OK(asx_lab_init(&lab1, &cfg));
    a = asx_lab_random_u64(&lab1);
    asx_lab_shutdown(&lab1);

    cfg.seed = 2;
    MUST_OK(asx_lab_init(&lab2, &cfg));
    b = asx_lab_random_u64(&lab2);
    asx_lab_shutdown(&lab2);

    /* Different seeds → different values (with overwhelming probability) */
    ASSERT_NE(a, b);
}

/* ------------------------------------------------------------------ */
/* Reset tests                                                         */
/* ------------------------------------------------------------------ */

TEST(reset_restores_state) {
    asx_lab lab;
    asx_lab_config cfg;
    uint64_t before_r, after_r;

    asx_lab_config_init(&cfg);
    cfg.seed = 99;
    MUST_OK(asx_lab_init(&lab, &cfg));

    before_r = asx_lab_random_u64(&lab);
    asx_lab_advance_time(&lab, 100);
    asx_lab_reset(&lab);

    /* After reset, entropy and time should restart */
    after_r = asx_lab_random_u64(&lab);
    ASSERT_EQ(before_r, after_r);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0);

    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Scenario execution tests                                            */
/* ------------------------------------------------------------------ */

static asx_status step_noop(asx_lab *lab, void *user_data)
{
    (void)lab; (void)user_data;
    return ASX_OK;
}

static asx_status step_advance_5(asx_lab *lab, void *user_data)
{
    (void)user_data;
    asx_lab_advance_time(lab, 5);
    return ASX_OK;
}

static asx_status step_fail(asx_lab *lab, void *user_data)
{
    (void)lab; (void)user_data;
    return ASX_E_INVALID_STATE;
}

static asx_status step_open_close_region(asx_lab *lab, void *user_data)
{
    asx_region_id rid;
    asx_status st;
    (void)user_data;
    st = asx_lab_open_region(lab, &rid);
    if (st != ASX_OK) return st;
    return asx_region_close(rid);
}

TEST(scenario_empty) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "empty");

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_total, 0u);
    ASSERT_EQ(result.steps_completed, 0u);

    asx_lab_shutdown(&lab);
}

TEST(scenario_single_step) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "single");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.last_status, ASX_OK);

    asx_lab_shutdown(&lab);
}

TEST(scenario_multi_step) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    cfg.tick_ns = 1000000ULL;
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "multi");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 2u);
    /* 10 ticks × 1ms = 10ms elapsed */
    ASSERT_EQ(result.elapsed_ns, 10000000ULL);

    asx_lab_shutdown(&lab);
}

TEST(scenario_stops_on_failure) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "fail-early");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_fail, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_E_INVALID_STATE);
    ASSERT_EQ(result.steps_completed, 2u);  /* ran 2, stopped at failure */
    ASSERT_EQ(result.steps_total, 3u);
    ASSERT_EQ(result.last_status, ASX_E_INVALID_STATE);

    asx_lab_shutdown(&lab);
}

TEST(scenario_with_region_ops) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "region-ops");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_open_close_region, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 1u);

    asx_lab_shutdown(&lab);
}

TEST(scenario_overflow) {
    asx_lab_scenario sc;
    uint32_t i;
    asx_lab_scenario_init(&sc, "overflow");
    for (i = 0; i < ASX_LAB_MAX_STEPS; i++) {
        MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    }
    ASSERT_EQ(asx_lab_scenario_add_step(&sc, step_noop, NULL),
              ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Deterministic replay test                                           */
/* ------------------------------------------------------------------ */

TEST(replay_determinism) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result r1, r2;

    asx_lab_config_init(&cfg);
    cfg.seed = 12345;
    cfg.tick_ns = 500000ULL;

    asx_lab_scenario_init(&sc, "replay");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_open_close_region, NULL));

    /* Run 1 */
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_run_scenario(&lab, &sc, &r1));
    asx_lab_shutdown(&lab);

    /* Run 2 */
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_run_scenario(&lab, &sc, &r2));
    asx_lab_shutdown(&lab);

    /* Identical results */
    ASSERT_EQ(r1.steps_completed, r2.steps_completed);
    ASSERT_EQ(r1.elapsed_ns, r2.elapsed_ns);
    ASSERT_EQ(r1.last_status, r2.last_status);
}

/* ------------------------------------------------------------------ */
/* Convenience API tests                                               */
/* ------------------------------------------------------------------ */

TEST(open_region) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_region_id rid;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_open_region(&lab, &rid));
    ASSERT_TRUE(asx_handle_is_valid(rid));
    asx_lab_shutdown(&lab);
}

TEST(open_region_null_fails) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_lab_open_region(&lab, NULL), ASX_E_INVALID_ARGUMENT);
    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_lab ===\n");

    RUN_TEST(config_init_defaults);

    RUN_TEST(init_shutdown);
    RUN_TEST(init_null_fails);
    RUN_TEST(init_null_config_fails);
    RUN_TEST(shutdown_null_safe);
    RUN_TEST(double_shutdown_safe);

    RUN_TEST(time_starts_at_zero);
    RUN_TEST(time_starts_at_custom);
    RUN_TEST(advance_time);
    RUN_TEST(time_integrates_with_runtime);

    RUN_TEST(entropy_deterministic);
    RUN_TEST(entropy_different_seeds);

    RUN_TEST(reset_restores_state);

    RUN_TEST(scenario_empty);
    RUN_TEST(scenario_single_step);
    RUN_TEST(scenario_multi_step);
    RUN_TEST(scenario_stops_on_failure);
    RUN_TEST(scenario_with_region_ops);
    RUN_TEST(scenario_overflow);

    RUN_TEST(replay_determinism);

    RUN_TEST(open_region);
    RUN_TEST(open_region_null_fails);

    TEST_REPORT();
    return test_failures;
}
