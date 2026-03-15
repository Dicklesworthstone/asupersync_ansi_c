/*
 * test_builder.c — unit tests for public runtime builder presets and setters
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/builder.h>
#include <asx/runtime/rt.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
#endif

static uint64_t fake_time(void *ctx) {
    uint64_t *value = (uint64_t *)ctx;
    return *value;
}

static uint64_t fake_entropy(void *ctx) {
    uint64_t *value = (uint64_t *)ctx;
    return *value;
}

static void test_set_env(const char *name, const char *value) {
#if defined(_WIN32)
    ASSERT_EQ(_putenv_s(name, value), 0);
#else
    ASSERT_EQ(setenv(name, value, 1), 0);
#endif
}

static void test_unset_env(const char *name) {
#if defined(_WIN32)
    ASSERT_EQ(_putenv_s(name, ""), 0);
#else
    ASSERT_EQ(unsetenv(name), 0);
#endif
}

static void clear_builder_test_env(void) {
    test_unset_env("ASX_RUNTIME_PRESET");
    test_unset_env("ASX_RUNTIME_WAIT_POLICY");
    test_unset_env("ASX_RUNTIME_LEAK_RESPONSE");
    test_unset_env("ASX_RUNTIME_FINALIZER_POLL_BUDGET");
    test_unset_env("ASX_RUNTIME_FINALIZER_TIME_BUDGET_NS");
    test_unset_env("ASX_RUNTIME_FINALIZER_ESCALATION");
    test_unset_env("ASX_RUNTIME_MAX_CANCEL_CHAIN_DEPTH");
    test_unset_env("ASX_RUNTIME_MAX_CANCEL_CHAIN_MEMORY");
    test_unset_env("TESTRT_PRESET");
    test_unset_env("TESTRT_WAIT_POLICY");
    test_unset_env("TESTRT_LEAK_RESPONSE");
    test_unset_env("TESTRT_FINALIZER_POLL_BUDGET");
    test_unset_env("TESTRT_FINALIZER_TIME_BUDGET_NS");
    test_unset_env("TESTRT_FINALIZER_ESCALATION");
    test_unset_env("TESTRT_MAX_CANCEL_CHAIN_DEPTH");
    test_unset_env("TESTRT_MAX_CANCEL_CHAIN_MEMORY");
}

TEST(init_null_fails) { ASSERT_EQ(asx_runtime_builder_init(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(init_defaults_ok) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_preset(&builder), ASX_RUNTIME_PRESET_DEFAULT);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(cfg));
    ASSERT_EQ(cfg.finalizer_poll_budget, 100u);
}

TEST(preset_names_are_stable) {
    ASSERT_STR_EQ(asx_runtime_preset_str(ASX_RUNTIME_PRESET_DEFAULT), "default");
    ASSERT_STR_EQ(asx_runtime_preset_str(ASX_RUNTIME_PRESET_CURRENT_THREAD), "current-thread");
    ASSERT_STR_EQ(asx_runtime_preset_str(ASX_RUNTIME_PRESET_LOW_LATENCY), "low-latency");
    ASSERT_STR_EQ(asx_runtime_preset_str(ASX_RUNTIME_PRESET_HIGH_THROUGHPUT), "high-throughput");
}

TEST(current_thread_preset_tightens_for_single_thread) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    ASSERT_EQ(asx_runtime_builder_init_current_thread(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(cfg.finalizer_poll_budget, 64u);
}

TEST(low_latency_preset_reduces_cleanup_budget) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    ASSERT_EQ(asx_runtime_builder_init_low_latency(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(cfg.finalizer_poll_budget, 32u);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 8u);
}

TEST(high_throughput_preset_expands_cleanup_budget) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    ASSERT_EQ(asx_runtime_builder_init_high_throughput(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_SLEEP);
    ASSERT_EQ(cfg.finalizer_poll_budget, 256u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 8192u);
}

TEST(setters_override_preset_values) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    ASSERT_EQ(asx_runtime_builder_init_low_latency(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_wait_policy(&builder, ASX_WAIT_YIELD), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_finalizer_poll_budget(&builder, 77u), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_finalizer_time_budget_ns(&builder, 1234u), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_max_cancel_chain_depth(&builder, 19u), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_max_cancel_chain_memory(&builder, 9000u), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_leak_response(&builder, ASX_LEAK_RECOVER), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);

    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_YIELD);
    ASSERT_EQ(cfg.finalizer_poll_budget, 77u);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)1234);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 19u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 9000u);
    ASSERT_EQ(cfg.leak_response, ASX_LEAK_RECOVER);
}

TEST(invalid_setter_inputs_fail) {
    asx_runtime_builder builder;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_finalizer_poll_budget(NULL, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_builder_set_finalizer_poll_budget(&builder, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_builder_set_finalizer_time_budget_ns(&builder, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_builder_set_max_cancel_chain_depth(&builder, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_builder_set_max_cancel_chain_memory(&builder, 0u),
              ASX_E_INVALID_ARGUMENT);
}

TEST(hook_setters_replace_hook_families) {
    asx_runtime_builder builder;
    asx_runtime_hooks hooks;
    asx_clock_hooks clock;
    uint64_t now = 99u;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_hooks(&builder, &hooks), ASX_OK);
    ASSERT_TRUE(hooks.clock.logical_now_ns_fn != NULL);

    memset(&clock, 0, sizeof(clock));
    clock.ctx = &now;
    clock.now_ns_fn = fake_time;
    clock.logical_now_ns_fn = fake_time;
    ASSERT_EQ(asx_runtime_builder_set_clock_hooks(&builder, &clock), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_hooks(&builder, &hooks), ASX_OK);
    ASSERT_EQ(hooks.clock.now_ns_fn(hooks.clock.ctx), (uint64_t)99);
}

TEST(set_hooks_rejects_invalid_full_hook_table) {
    asx_runtime_builder builder;
    asx_runtime_hooks hooks;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);

    hooks.clock.logical_now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_builder_set_hooks(&builder, &hooks), ASX_E_DETERMINISM_VIOLATION);
}

TEST(set_hooks_accepts_valid_full_hook_table) {
    asx_runtime_builder builder;
    asx_runtime_hooks hooks;
    uint64_t now = 123u;
    uint64_t entropy = 456u;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);

    hooks.clock.now_ns_fn = fake_time;
    hooks.clock.logical_now_ns_fn = fake_time;
    hooks.clock.ctx = &now;
    hooks.entropy.random_u64_fn = fake_entropy;
    hooks.entropy.ctx = &entropy;
    hooks.deterministic_seeded_prng = 1;

    ASSERT_EQ(asx_runtime_builder_set_hooks(&builder, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_hooks(&builder, &hooks), ASX_OK);
    ASSERT_EQ(hooks.clock.logical_now_ns_fn(hooks.clock.ctx), (uint64_t)123);
    ASSERT_EQ(hooks.entropy.random_u64_fn(hooks.entropy.ctx), (uint64_t)456);
}

TEST(validate_rejects_bad_builder_config) {
    asx_runtime_builder builder;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    builder.config.finalizer_poll_budget = 0u;
    ASSERT_EQ(asx_runtime_builder_validate(&builder), ASX_E_INVALID_ARGUMENT);
}

TEST(build_roundtrip_works) {
    asx_runtime_builder builder;
    asx_runtime rt;
    asx_runtime_config cfg;

    ASSERT_EQ(asx_runtime_builder_init_high_throughput(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_set_finalizer_poll_budget(&builder, 144u), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_build(&builder, &rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    ASSERT_EQ(cfg.finalizer_poll_budget, 144u);
    asx_runtime_shutdown(&rt);
}

TEST(build_null_fails) {
    asx_runtime_builder builder;
    asx_runtime rt;

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_build(NULL, &rt), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_builder_build(&builder, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(apply_env_null_builder_fails) {
    clear_builder_test_env();
    ASSERT_EQ(asx_runtime_builder_apply_env(NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(apply_env_no_variables_is_noop) {
    asx_runtime_builder builder;
    asx_runtime_config before;
    asx_runtime_config after;

    clear_builder_test_env();
    ASSERT_EQ(asx_runtime_builder_init_low_latency(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &before), ASX_OK);

    ASSERT_EQ(asx_runtime_builder_apply_env(&builder, NULL), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &after), ASX_OK);
    ASSERT_EQ(memcmp(&before, &after, sizeof(before)), 0);
    ASSERT_EQ(asx_runtime_builder_preset(&builder), ASX_RUNTIME_PRESET_LOW_LATENCY);
}

TEST(apply_env_uses_default_prefix_and_overrides_config) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    clear_builder_test_env();
    test_set_env("ASX_RUNTIME_PRESET", "high-throughput");
    test_set_env("ASX_RUNTIME_WAIT_POLICY", "yield");
    test_set_env("ASX_RUNTIME_LEAK_RESPONSE", "recover");
    test_set_env("ASX_RUNTIME_FINALIZER_POLL_BUDGET", "321");
    test_set_env("ASX_RUNTIME_FINALIZER_TIME_BUDGET_NS", "654321");
    test_set_env("ASX_RUNTIME_FINALIZER_ESCALATION", "bounded-panic");
    test_set_env("ASX_RUNTIME_MAX_CANCEL_CHAIN_DEPTH", "21");
    test_set_env("ASX_RUNTIME_MAX_CANCEL_CHAIN_MEMORY", "7000");

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_apply_env(&builder, NULL), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);

    ASSERT_EQ(asx_runtime_builder_preset(&builder), ASX_RUNTIME_PRESET_HIGH_THROUGHPUT);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_YIELD);
    ASSERT_EQ(cfg.leak_response, ASX_LEAK_RECOVER);
    ASSERT_EQ(cfg.finalizer_poll_budget, 321u);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)654321);
    ASSERT_EQ(cfg.finalizer_escalation, ASX_FINALIZER_BOUNDED_PANIC);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 21u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 7000u);

    clear_builder_test_env();
}

TEST(apply_env_accepts_custom_prefix) {
    asx_runtime_builder builder;
    asx_runtime_config cfg;

    clear_builder_test_env();
    test_set_env("TESTRT_PRESET", "current_thread");
    test_set_env("TESTRT_FINALIZER_POLL_BUDGET", "88");

    ASSERT_EQ(asx_runtime_builder_init(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_apply_env(&builder, "TESTRT_"), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &cfg), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_preset(&builder), ASX_RUNTIME_PRESET_CURRENT_THREAD);
    ASSERT_EQ(cfg.finalizer_poll_budget, 88u);

    clear_builder_test_env();
}

TEST(apply_env_invalid_value_is_atomic) {
    asx_runtime_builder builder;
    asx_runtime_config before;
    asx_runtime_config after;

    clear_builder_test_env();
    ASSERT_EQ(asx_runtime_builder_init_low_latency(&builder), ASX_OK);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &before), ASX_OK);

    test_set_env("ASX_RUNTIME_PRESET", "high-throughput");
    test_set_env("ASX_RUNTIME_FINALIZER_POLL_BUDGET", "not-a-number");

    ASSERT_EQ(asx_runtime_builder_apply_env(&builder, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_builder_get_config(&builder, &after), ASX_OK);
    ASSERT_EQ(memcmp(&before, &after, sizeof(before)), 0);
    ASSERT_EQ(asx_runtime_builder_preset(&builder), ASX_RUNTIME_PRESET_LOW_LATENCY);

    clear_builder_test_env();
}

int main(void) {
    fprintf(stderr, "=== test_builder ===\n");
    RUN_TEST(init_null_fails);
    RUN_TEST(init_defaults_ok);
    RUN_TEST(preset_names_are_stable);
    RUN_TEST(current_thread_preset_tightens_for_single_thread);
    RUN_TEST(low_latency_preset_reduces_cleanup_budget);
    RUN_TEST(high_throughput_preset_expands_cleanup_budget);
    RUN_TEST(setters_override_preset_values);
    RUN_TEST(invalid_setter_inputs_fail);
    RUN_TEST(hook_setters_replace_hook_families);
    RUN_TEST(set_hooks_rejects_invalid_full_hook_table);
    RUN_TEST(set_hooks_accepts_valid_full_hook_table);
    RUN_TEST(validate_rejects_bad_builder_config);
    RUN_TEST(build_roundtrip_works);
    RUN_TEST(build_null_fails);
    RUN_TEST(apply_env_null_builder_fails);
    RUN_TEST(apply_env_no_variables_is_noop);
    RUN_TEST(apply_env_uses_default_prefix_and_overrides_config);
    RUN_TEST(apply_env_accepts_custom_prefix);
    RUN_TEST(apply_env_invalid_value_is_atomic);
    TEST_REPORT();
    return test_failures;
}
