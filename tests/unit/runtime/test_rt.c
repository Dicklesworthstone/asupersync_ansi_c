/*
 * test_rt.c — unit tests for runtime object lifecycle and state queries
 *
 * Tests cover:
 *   - Config validation
 *   - Runtime init/shutdown lifecycle
 *   - Default initialization
 *   - State queries (region/task/obligation counts)
 *   - Config/hooks retrieval
 *   - Guard behavior for uninitialized/NULL runtime
 *   - Capacity queries
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* Suppress warn_unused_result in test helpers where we don't check */
static asx_status rt_test_sink_;
#define MUST_OK(expr) do { rt_test_sink_ = (expr); (void)rt_test_sink_; } while(0)

/* ------------------------------------------------------------------ */
/* Dummy poll function for task spawn tests                            */
/* ------------------------------------------------------------------ */

static asx_status dummy_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Helper: create a valid config                                       */
/* ------------------------------------------------------------------ */

static void make_valid_config(asx_runtime_config *cfg)
{
    asx_runtime_config_init(cfg);
}

static void make_valid_hooks(asx_runtime_hooks *hooks)
{
    asx_runtime_hooks_init(hooks);
}

/* ------------------------------------------------------------------ */
/* Config validation tests                                             */
/* ------------------------------------------------------------------ */

TEST(config_validate_null_fails) {
    ASSERT_EQ(asx_runtime_config_validate(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_bad_size_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.size = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_zero_cancel_depth_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.max_cancel_chain_depth = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_zero_finalizer_budget_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.finalizer_poll_budget = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_defaults_ok) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Init/shutdown lifecycle tests                                       */
/* ------------------------------------------------------------------ */

TEST(init_null_rt_fails) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(NULL, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_config_fails) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&rt, NULL, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_hooks_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_bad_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.size = 0; /* invalid */
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_success) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    ASSERT_NE(rt.generation, 0u);
    asx_runtime_shutdown(&rt);
}

TEST(init_generation_increments) {
    asx_runtime a, b;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    uint32_t gen_a;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&a, &cfg, &hooks));
    gen_a = a.generation;
    asx_runtime_shutdown(&a);

    MUST_OK(asx_runtime_init(&b, &cfg, &hooks));
    ASSERT_TRUE(b.generation > gen_a);
    asx_runtime_shutdown(&b);
}

TEST(shutdown_clears_state) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
    ASSERT_EQ(rt.generation, 0u);
}

TEST(shutdown_null_safe) {
    asx_runtime_shutdown(NULL);
}

TEST(shutdown_double_safe) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    asx_runtime_shutdown(&rt);
    asx_runtime_shutdown(&rt); /* should not crash */
}

TEST(shutdown_uninitialized_safe) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    asx_runtime_shutdown(&rt); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Default initialization tests                                        */
/* ------------------------------------------------------------------ */

TEST(init_default_null_fails) {
    ASSERT_EQ(asx_runtime_init_default(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_default_success) {
    asx_runtime rt;
    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* Verify defaults were applied */
    ASSERT_EQ(rt.config.leak_response, ASX_LEAK_LOG);
    ASSERT_EQ(rt.config.finalizer_poll_budget, 100u);
    ASSERT_EQ(rt.config.max_cancel_chain_depth, 16u);

    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* is_initialized tests                                                */
/* ------------------------------------------------------------------ */

TEST(is_initialized_null_returns_false) {
    ASSERT_FALSE(asx_runtime_is_initialized(NULL));
}

TEST(is_initialized_zeroed_returns_false) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
}

/* ------------------------------------------------------------------ */
/* Config/hooks retrieval tests                                        */
/* ------------------------------------------------------------------ */

TEST(get_config_null_rt_fails) {
    asx_runtime_config out;
    ASSERT_EQ(asx_runtime_get_config(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_config_null_out_fails) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    asx_runtime_shutdown(&rt);
}

TEST(get_config_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_config out;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_E_INVALID_STATE);
}

TEST(get_config_returns_stored_config) {
    asx_runtime rt;
    asx_runtime_config cfg, out;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.finalizer_poll_budget = 42;

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.finalizer_poll_budget, 42u);
    asx_runtime_shutdown(&rt);
}

TEST(get_hooks_null_rt_fails) {
    asx_runtime_hooks out;
    ASSERT_EQ(asx_runtime_get_hooks_from(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_hooks_null_out_fails) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    asx_runtime_shutdown(&rt);
}

TEST(get_hooks_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_hooks out;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, &out), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* State query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(region_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_region_count(NULL), 0u);
}

TEST(task_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_task_count(NULL), 0u);
}

TEST(obligation_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_obligation_count(NULL), 0u);
}

TEST(counts_zero_after_init) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_region_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 0u);
    asx_runtime_shutdown(&rt);
}

TEST(counts_reflect_opened_region) {
    asx_runtime rt;
    asx_region_id rid;
    MUST_OK(asx_runtime_init_default(&rt));

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_runtime_region_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(counts_reflect_spawned_task) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    MUST_OK(asx_runtime_init_default(&rt));

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_runtime_task_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(counts_uninitialized_returns_zero) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_region_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 0u);
}

/* ------------------------------------------------------------------ */
/* Capacity query tests                                                */
/* ------------------------------------------------------------------ */

TEST(region_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_region_capacity(), (uint32_t)ASX_MAX_REGIONS);
}

TEST(task_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_task_capacity(), (uint32_t)ASX_MAX_TASKS);
}

TEST(obligation_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_obligation_capacity(), (uint32_t)ASX_MAX_OBLIGATIONS);
}

/* ------------------------------------------------------------------ */
/* Safety/containment query tests                                      */
/* ------------------------------------------------------------------ */

TEST(safety_profile_returns_active) {
    asx_runtime rt;
    asx_safety_profile sp;
    MUST_OK(asx_runtime_init_default(&rt));
    sp = asx_runtime_safety_profile(&rt);
    ASSERT_EQ(sp, asx_safety_profile_active());
    asx_runtime_shutdown(&rt);
}

TEST(containment_policy_returns_active) {
    asx_runtime rt;
    asx_containment_policy cp;
    MUST_OK(asx_runtime_init_default(&rt));
    cp = asx_runtime_containment_policy(&rt);
    ASSERT_EQ(cp, asx_containment_policy_active());
    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* Integration: init → use → shutdown round-trip                       */
/* ------------------------------------------------------------------ */

TEST(full_roundtrip) {
    asx_runtime rt;
    asx_region_id rid;
    asx_obligation_id oid;
    asx_runtime_config cfg_out;

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* Open region */
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_runtime_region_count(&rt), 1u);

    /* Reserve and commit obligation */
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 1u);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    /* Query config */
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg_out), ASX_OK);
    ASSERT_EQ(cfg_out.finalizer_poll_budget, 100u);

    /* Shutdown resets everything */
    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_rt ===\n");

    /* Config validation */
    RUN_TEST(config_validate_null_fails);
    RUN_TEST(config_validate_bad_size_fails);
    RUN_TEST(config_validate_zero_cancel_depth_fails);
    RUN_TEST(config_validate_zero_finalizer_budget_fails);
    RUN_TEST(config_validate_defaults_ok);

    /* Init/shutdown lifecycle */
    RUN_TEST(init_null_rt_fails);
    RUN_TEST(init_null_config_fails);
    RUN_TEST(init_null_hooks_fails);
    RUN_TEST(init_bad_config_fails);
    RUN_TEST(init_success);
    RUN_TEST(init_generation_increments);
    RUN_TEST(shutdown_clears_state);
    RUN_TEST(shutdown_null_safe);
    RUN_TEST(shutdown_double_safe);
    RUN_TEST(shutdown_uninitialized_safe);

    /* Default init */
    RUN_TEST(init_default_null_fails);
    RUN_TEST(init_default_success);

    /* is_initialized */
    RUN_TEST(is_initialized_null_returns_false);
    RUN_TEST(is_initialized_zeroed_returns_false);

    /* Config/hooks retrieval */
    RUN_TEST(get_config_null_rt_fails);
    RUN_TEST(get_config_null_out_fails);
    RUN_TEST(get_config_uninitialized_fails);
    RUN_TEST(get_config_returns_stored_config);
    RUN_TEST(get_hooks_null_rt_fails);
    RUN_TEST(get_hooks_null_out_fails);
    RUN_TEST(get_hooks_uninitialized_fails);

    /* State queries */
    RUN_TEST(region_count_null_returns_zero);
    RUN_TEST(task_count_null_returns_zero);
    RUN_TEST(obligation_count_null_returns_zero);
    RUN_TEST(counts_zero_after_init);
    RUN_TEST(counts_reflect_opened_region);
    RUN_TEST(counts_reflect_spawned_task);
    RUN_TEST(counts_uninitialized_returns_zero);

    /* Capacity queries */
    RUN_TEST(region_capacity_returns_max);
    RUN_TEST(task_capacity_returns_max);
    RUN_TEST(obligation_capacity_returns_max);

    /* Safety/containment */
    RUN_TEST(safety_profile_returns_active);
    RUN_TEST(containment_policy_returns_active);

    /* Integration */
    RUN_TEST(full_roundtrip);

    TEST_REPORT();
    return test_failures;
}
