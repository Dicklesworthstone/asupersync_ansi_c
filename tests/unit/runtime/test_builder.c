/*
 * test_builder.c — unit tests for public runtime builder presets and setters
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/builder.h>
#include <asx/runtime/rt.h>
#include <string.h>

static uint64_t fake_time(void *ctx) {
    uint64_t *value = (uint64_t *)ctx;
    return *value;
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
    RUN_TEST(validate_rejects_bad_builder_config);
    RUN_TEST(build_roundtrip_works);
    RUN_TEST(build_null_fails);
    TEST_REPORT();
    return test_failures;
}
