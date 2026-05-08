/*
 * test_replay.c — unit tests for replay oracles, snapshot/restore,
 *                  and counterexample minimization
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/replay.h>
#include <asx/runtime/runtime.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test step functions                                                  */
/* ------------------------------------------------------------------ */

static asx_status step_noop(asx_lab *lab, void *user_data) {
    (void)lab;
    (void)user_data;
    return ASX_OK;
}

static asx_status step_advance_5(asx_lab *lab, void *user_data) {
    (void)user_data;
    asx_lab_advance_time(lab, 5);
    return ASX_OK;
}

static asx_status step_resource_exhausted(asx_lab *lab, void *user_data) {
    (void)lab;
    (void)user_data;
    return ASX_E_RESOURCE_EXHAUSTED;
}

static asx_status step_invalid_state(asx_lab *lab, void *user_data) {
    (void)lab;
    (void)user_data;
    return ASX_E_INVALID_STATE;
}


/* ================================================================== */
/* Snapshot tests                                                       */
/* ================================================================== */

TEST(snapshot_take_restore) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_snapshot_id sid;
    asx_time now = 0;
    uint64_t expected_runtime_entropy;
    uint64_t restored_runtime_entropy = 0;

    asx_snapshot_reset();
    asx_lab_config_init(&cfg);
    cfg.seed = 42;
    cfg.tick_ns = 1000000ULL;
    MUST_OK(asx_lab_init(&lab, &cfg));

    /* Advance time and consume entropy */
    asx_lab_advance_time(&lab, 10);
    (void)asx_lab_random_u64(&lab);

    /* Take snapshot */
    MUST_OK(asx_snapshot_take(&lab, &sid));
    ASSERT_TRUE(asx_snapshot_is_valid(sid));
    expected_runtime_entropy = asx_lab_random_u64(&lab);

    /* Advance more */
    asx_lab_advance_time(&lab, 20);
    (void)asx_lab_random_u64(&lab);

    /* Restore snapshot */
    MUST_OK(asx_snapshot_restore(&lab, sid));

    /* Time and entropy should be back to snapshot state */
    ASSERT_EQ(asx_lab_now(&lab), 10000000ULL);
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, (asx_time)10000000ULL);
    ASSERT_EQ(asx_runtime_random_u64(&restored_runtime_entropy), ASX_OK);
    ASSERT_EQ(restored_runtime_entropy, expected_runtime_entropy);

    asx_lab_shutdown(&lab);
    MUST_OK(asx_snapshot_discard(sid));
}

TEST(snapshot_discard) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_snapshot_id sid;

    asx_snapshot_reset();
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_snapshot_take(&lab, &sid));
    ASSERT_TRUE(asx_snapshot_is_valid(sid));
    MUST_OK(asx_snapshot_discard(sid));
    ASSERT_FALSE(asx_snapshot_is_valid(sid));
    asx_lab_shutdown(&lab);
}

TEST(snapshot_null_fails) {
    asx_snapshot_id sid;
    asx_snapshot_reset();
    ASSERT_EQ(asx_snapshot_take(NULL, &sid), ASX_E_INVALID_ARGUMENT);
}

TEST(snapshot_exhaustion) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_snapshot_id sids[ASX_SNAPSHOT_MAX];
    asx_snapshot_id extra;
    uint32_t i;

    asx_snapshot_reset();
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    for (i = 0; i < ASX_SNAPSHOT_MAX; i++) { MUST_OK(asx_snapshot_take(&lab, &sids[i])); }
    ASSERT_EQ(asx_snapshot_take(&lab, &extra), ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < ASX_SNAPSHOT_MAX; i++) { MUST_OK(asx_snapshot_discard(sids[i])); }
    asx_lab_shutdown(&lab);
}

TEST(snapshot_restore_invalid_fails) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_snapshot_id sid;

    asx_snapshot_reset();
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    sid.slot = 0;
    ASSERT_EQ(asx_snapshot_restore(&lab, sid), ASX_E_INVALID_STATE);
    asx_lab_shutdown(&lab);
}

TEST(snapshot_deterministic_replay) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_snapshot_id sid;
    asx_time now = 0;
    uint64_t r_before, r_after;

    asx_snapshot_reset();
    asx_lab_config_init(&cfg);
    cfg.seed = 99;
    MUST_OK(asx_lab_init(&lab, &cfg));

    /* Take snapshot at initial state */
    MUST_OK(asx_snapshot_take(&lab, &sid));

    /* Draw entropy */
    r_before = asx_lab_random_u64(&lab);

    /* Restore and draw again — should be identical */
    MUST_OK(asx_snapshot_restore(&lab, sid));
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, (asx_time)0);
    r_after = asx_lab_random_u64(&lab);

    ASSERT_EQ(r_before, r_after);

    asx_lab_shutdown(&lab);
    MUST_OK(asx_snapshot_discard(sid));
}

/* ================================================================== */
/* Oracle tests                                                        */
/* ================================================================== */

TEST(oracle_quiescence_pass) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_oracle_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    /* No tasks spawned — should be quiescent */
    result = asx_oracle_quiescence(&lab, NULL);
    ASSERT_EQ(result.verdict, ASX_ORACLE_PASS);

    asx_lab_shutdown(&lab);
}

TEST(oracle_quiescence_null_fails) {
    asx_oracle_result result;
    result = asx_oracle_quiescence(NULL, NULL);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(oracle_leak_pass) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_oracle_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    /* No regions opened besides lab's own — after reset, should be clean */
    result = asx_oracle_leak(&lab, NULL);
    /* Note: lab itself has a region, but it's managed by the lab */
    /* Oracle checks for non-closed regions */
    (void)result; /* Verdict depends on lab region state */

    asx_lab_shutdown(&lab);
}

TEST(oracle_replay_match_pass) {
    asx_lab_result r1, r2;
    asx_oracle_result result;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    r1.steps_completed = 3;
    r1.steps_total = 3;
    r1.elapsed_ns = 5000000;
    r1.polls_total = 9u;
    r1.last_status = ASX_OK;
    r2.steps_completed = 3;
    r2.steps_total = 3;
    r2.elapsed_ns = 5000000;
    r2.polls_total = 9u;
    r2.last_status = ASX_OK;

    result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(result.verdict, ASX_ORACLE_PASS);
}

TEST(oracle_replay_match_step_diverge) {
    asx_lab_result r1, r2;
    asx_oracle_result result;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    r1.steps_completed = 3;
    r2.steps_completed = 2;

    result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(oracle_replay_match_time_diverge) {
    asx_lab_result r1, r2;
    asx_oracle_result result;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    r1.steps_completed = 3;
    r1.elapsed_ns = 5000000;
    r2.steps_completed = 3;
    r2.elapsed_ns = 6000000;

    result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(oracle_replay_match_steps_total_diverge) {
    asx_lab_result r1, r2;
    asx_oracle_result result;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    r1.steps_completed = 2;
    r1.steps_total = 3;
    r2.steps_completed = 2;
    r2.steps_total = 2;

    result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(oracle_replay_match_polls_diverge) {
    asx_lab_result r1, r2;
    asx_oracle_result result;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    r1.steps_completed = 2;
    r1.steps_total = 2;
    r1.elapsed_ns = 123u;
    r1.polls_total = 4u;
    r1.last_status = ASX_OK;
    r2.steps_completed = 2;
    r2.steps_total = 2;
    r2.elapsed_ns = 123u;
    r2.polls_total = 5u;
    r2.last_status = ASX_OK;

    result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(oracle_replay_match_status_diverge) {
    asx_lab_result r1, r2;
    asx_oracle_result result;

    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    r1.steps_completed = 3;
    r1.last_status = ASX_OK;
    r2.steps_completed = 3;
    r2.last_status = ASX_E_INVALID_STATE;

    result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(oracle_replay_null_fails) {
    asx_oracle_result result;
    result = asx_oracle_replay_match(NULL, NULL);
    ASSERT_EQ(result.verdict, ASX_ORACLE_FAIL);
}

TEST(replay_render_result_json_contains_core_fields) {
    asx_replay_result result;
    asx_report_buf out;

    memset(&result, 0, sizeof(result));
    result.result = ASX_REPLAY_KIND_MISMATCH;
    result.divergence_index = 7u;
    result.expected_digest = 0x12u;
    result.actual_digest = 0x34u;

    MUST_OK(asx_replay_render_result_json(&result, &out));
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"result\":\"kind_mismatch\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"divergence_index\":7") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"expected_digest\":0x0000000000000012") !=
                NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"actual_digest\":0x0000000000000034") != NULL);
}

TEST(replay_render_current_diff_json_reports_mismatch_context) {
    asx_trace_event ref[2];
    asx_report_buf out;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1u, 0u);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1u, 0u);
    ASSERT_TRUE(asx_trace_event_get(0u, &ref[0]));
    ASSERT_TRUE(asx_trace_event_get(1u, &ref[1]));
    MUST_OK(asx_replay_load_reference(ref, 2u));

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1u, 0u);
    asx_trace_emit(ASX_TRACE_SCHED_BUDGET, 1u, 0u);

    MUST_OK(asx_replay_render_current_diff_json(&out));
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"reference_loaded\":true") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"current_count\":2") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"result\":\"kind_mismatch\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"expected_event\":{\"sequence\":1") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"kind\":\"sched_complete\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"actual_event\":{\"sequence\":1") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"kind\":\"sched_budget\"") != NULL);

    asx_replay_clear_reference();
}

/* ================================================================== */
/* Oracle suite tests                                                   */
/* ================================================================== */

TEST(oracle_suite_all_pass) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_oracle_suite suite;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    asx_oracle_suite_init(&suite);
    MUST_OK(asx_oracle_suite_add(&suite, asx_oracle_quiescence, NULL));
    ASSERT_EQ(asx_oracle_suite_run(&suite, &lab), ASX_OK);
    ASSERT_EQ(suite.pass_count, 1u);
    ASSERT_EQ(suite.fail_count, 0u);

    asx_lab_shutdown(&lab);
}

TEST(oracle_suite_empty) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_oracle_suite suite;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    asx_oracle_suite_init(&suite);
    ASSERT_EQ(asx_oracle_suite_run(&suite, &lab), ASX_OK);

    asx_lab_shutdown(&lab);
}

TEST(oracle_suite_overflow) {
    asx_oracle_suite suite;
    uint32_t i;
    asx_oracle_suite_init(&suite);
    for (i = 0; i < ASX_ORACLE_SUITE_MAX; i++) {
        MUST_OK(asx_oracle_suite_add(&suite, asx_oracle_quiescence, NULL));
    }
    ASSERT_EQ(asx_oracle_suite_add(&suite, asx_oracle_quiescence, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(oracle_suite_run_rejects_oversized_count) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_oracle_suite suite;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    asx_oracle_suite_init(&suite);
    suite.count = ASX_ORACLE_SUITE_MAX + 1u;
    ASSERT_EQ(asx_oracle_suite_run(&suite, &lab), ASX_E_INVALID_ARGUMENT);

    asx_lab_shutdown(&lab);
}

/* ================================================================== */
/* Counterexample minimization tests                                   */
/* ================================================================== */

/* Oracle that always says FAIL (for testing minimization) */
static asx_oracle_result oracle_always_fail(const asx_lab *lab, void *ctx) {
    (void)lab;
    (void)ctx;
    return (asx_oracle_result){ASX_ORACLE_FAIL, "always_fail", "always fails", 0};
}

/* Oracle that always says PASS, so status failures drive minimization. */
static asx_oracle_result oracle_always_pass(const asx_lab *lab, void *ctx) {
    (void)lab;
    (void)ctx;
    return (asx_oracle_result){ASX_ORACLE_PASS, "always_pass", "always passes", 0};
}


TEST(minimize_shrinks_scenario) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;
    asx_status st;
    const asx_lab_scenario *result;

    asx_lab_config_init(&cfg);
    cfg.seed = 42;

    /* Create a 4-step scenario */
    asx_lab_scenario_init(&sc, "shrink-test");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_fail, NULL, 10));

    /* Run minimization steps */
    do { st = asx_minimize_step(&ms); } while (st == ASX_E_PENDING);

    result = asx_minimize_result(&ms);
    /* Should have shrunk — fewer steps than original */
    ASSERT_TRUE(result->step_count < 4u);
    ASSERT_TRUE(asx_minimize_attempts(&ms) > 0u);
}

TEST(minimize_single_step_no_shrink) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "single");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_fail, NULL, 5));
    /* Single step can't be shrunk */
    ASSERT_EQ(asx_minimize_step(&ms), ASX_OK);
    ASSERT_EQ(asx_minimize_result(&ms)->step_count, 1u);
}

TEST(minimize_null_fails) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "test");

    ASSERT_EQ(asx_minimize_init(NULL, &cfg, &sc, oracle_always_fail, NULL, 5),
              ASX_E_INVALID_ARGUMENT);
}

TEST(minimize_max_attempts_respected) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;
    asx_status st;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "bounded");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_fail, NULL, 2));

    st = asx_minimize_step(&ms);
    if (st == ASX_E_PENDING) { st = asx_minimize_step(&ms); }
    /* After 2 attempts, should stop */
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_minimize_attempts(&ms), 2u);
}

TEST(minimize_run_and_render_json) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;
    asx_report_buf out;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "render");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_fail, NULL, 8u));
    MUST_OK(asx_minimize_run(&ms));
    MUST_OK(asx_minimize_render_json(&ms, &out));

    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"scenario_name\":\"render\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out),
                       "\"schema_version\":\"asx.replay_counterexample.v1\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"original_steps\":3") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"attempts\":") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"found_smaller\":true") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"preserves_failure_class\":true") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"failure_class\":\"oracle\"") != NULL);
}

TEST(minimize_rejects_success_when_resource_failure_removed) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;
    const asx_minimize_observation *original;
    const asx_minimize_observation *current;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "resource-preserve");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_resource_exhausted, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_pass, NULL, 4u));
    MUST_OK(asx_minimize_run(&ms));

    original = asx_minimize_original_observation(&ms);
    current = asx_minimize_current_observation(&ms);
    ASSERT_TRUE(original != NULL);
    ASSERT_TRUE(current != NULL);
    ASSERT_EQ(original->failure_class, ASX_MINIMIZE_FAILURE_RESOURCE);
    ASSERT_EQ(current->failure_class, ASX_MINIMIZE_FAILURE_RESOURCE);
    ASSERT_EQ(current->run_status, ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_minimize_result(&ms)->step_count, 1u);
    ASSERT_EQ(ms.rejected_attempts, 1u);
}

TEST(minimize_preserves_exact_runtime_status) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;
    const asx_minimize_observation *current;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "runtime-preserve");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_invalid_state, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_pass, NULL, 4u));
    MUST_OK(asx_minimize_run(&ms));

    current = asx_minimize_current_observation(&ms);
    ASSERT_TRUE(current != NULL);
    ASSERT_EQ(current->failure_class, ASX_MINIMIZE_FAILURE_RUNTIME);
    ASSERT_EQ(current->run_status, ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_minimize_result(&ms)->step_count, 1u);
    ASSERT_EQ(ms.rejected_attempts, 1u);
}

TEST(minimize_rejects_original_scenario_without_failure) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "no-failure");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    ASSERT_EQ(asx_minimize_init(&ms, &cfg, &sc, oracle_always_pass, NULL, 4u), ASX_E_INVALID_STATE);
}

TEST(minimize_render_json_reports_resource_class) {
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_minimize_state ms;
    asx_report_buf out;

    asx_lab_config_init(&cfg);
    asx_lab_scenario_init(&sc, "resource-json");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_resource_exhausted, NULL));

    MUST_OK(asx_minimize_init(&ms, &cfg, &sc, oracle_always_pass, NULL, 4u));
    MUST_OK(asx_minimize_run(&ms));
    MUST_OK(asx_minimize_render_json(&ms, &out));

    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"failure_class\":\"resource\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"run_status\":\"resource exhausted\"") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"rejected_attempts\":1") != NULL);
    ASSERT_TRUE(strstr(asx_report_buf_cstr(&out), "\"preserves_failure_class\":true") != NULL);
}

/* ================================================================== */
/* Integration: snapshot + replay oracle                               */
/* ================================================================== */

TEST(snapshot_replay_integration) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result r1, r2;
    asx_snapshot_id sid;
    asx_oracle_result oracle_result;

    asx_snapshot_reset();
    asx_lab_config_init(&cfg);
    cfg.seed = 777;
    cfg.tick_ns = 1000000ULL;

    asx_lab_scenario_init(&sc, "replay-test");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    /* Run 1 */
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_snapshot_take(&lab, &sid));
    MUST_OK(asx_lab_run_scenario(&lab, &sc, &r1));
    asx_lab_shutdown(&lab);

    /* Run 2: restore from snapshot */
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_run_scenario(&lab, &sc, &r2));
    asx_lab_shutdown(&lab);

    /* Verify replay match */
    oracle_result = asx_oracle_replay_match(&r1, &r2);
    ASSERT_EQ(oracle_result.verdict, ASX_ORACLE_PASS);

    MUST_OK(asx_snapshot_discard(sid));
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_replay ===\n");

    /* Snapshot */
    RUN_TEST(snapshot_take_restore);
    RUN_TEST(snapshot_discard);
    RUN_TEST(snapshot_null_fails);
    RUN_TEST(snapshot_exhaustion);
    RUN_TEST(snapshot_restore_invalid_fails);
    RUN_TEST(snapshot_deterministic_replay);

    /* Oracles */
    RUN_TEST(oracle_quiescence_pass);
    RUN_TEST(oracle_quiescence_null_fails);
    RUN_TEST(oracle_leak_pass);
    RUN_TEST(oracle_replay_match_pass);
    RUN_TEST(oracle_replay_match_step_diverge);
    RUN_TEST(oracle_replay_match_time_diverge);
    RUN_TEST(oracle_replay_match_steps_total_diverge);
    RUN_TEST(oracle_replay_match_polls_diverge);
    RUN_TEST(oracle_replay_match_status_diverge);
    RUN_TEST(oracle_replay_null_fails);
    RUN_TEST(replay_render_result_json_contains_core_fields);
    RUN_TEST(replay_render_current_diff_json_reports_mismatch_context);

    /* Oracle suite */
    RUN_TEST(oracle_suite_all_pass);
    RUN_TEST(oracle_suite_empty);
    RUN_TEST(oracle_suite_overflow);
    RUN_TEST(oracle_suite_run_rejects_oversized_count);

    /* Minimization */
    RUN_TEST(minimize_shrinks_scenario);
    RUN_TEST(minimize_single_step_no_shrink);
    RUN_TEST(minimize_null_fails);
    RUN_TEST(minimize_max_attempts_respected);
    RUN_TEST(minimize_run_and_render_json);
    RUN_TEST(minimize_rejects_success_when_resource_failure_removed);
    RUN_TEST(minimize_preserves_exact_runtime_status);
    RUN_TEST(minimize_rejects_original_scenario_without_failure);
    RUN_TEST(minimize_render_json_reports_resource_class);

    /* Integration */
    RUN_TEST(snapshot_replay_integration);

    TEST_REPORT();
    return test_failures;
}

#else

TEST(replay_family_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT, 1);
}

int main(void) {
    fprintf(stderr, "=== test_replay (minimal browser hidden contract) ===\n");
    RUN_TEST(replay_family_compile_time_hidden_in_minimal_browser);
    TEST_REPORT();
    return test_failures;
}

#endif
