/*
 * runtime_hooks_test.c — hook contract tests for deterministic/runtime policy
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>

static int g_wait_calls = 0;
static int g_ghost_wait_calls = 0;
static int g_log_calls = 0;

static asx_time test_now_ns(void *ctx) { return *(asx_time *)ctx; }

static uint64_t test_entropy_u64(void *ctx) { return *(uint64_t *)ctx; }

static asx_status test_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    (void)ctx;
    g_wait_calls++;
    *ready_count = timeout_ms;
    return ASX_OK;
}

static asx_status test_ghost_wait(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    g_ghost_wait_calls++;
    *ready_count = (uint32_t)(logical_step + 1U);
    return ASX_OK;
}

static void test_log_sink(void *ctx, int level, const char *message) {
    (void)ctx;
    (void)level;
    (void)message;
    g_log_calls++;
}

TEST(hooks_init_defaults_validate) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
}

TEST(deterministic_rejects_unseeded_entropy) {
    asx_runtime_hooks hooks;
    uint64_t entropy_value = 1234U;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.entropy.ctx = &entropy_value;
    hooks.entropy.random_u64_fn = test_entropy_u64;
    hooks.deterministic_seeded_prng = 0;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(deterministic_requires_ghost_reactor) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.reactor.wait_fn = test_wait;
    hooks.reactor.ghost_wait_fn = NULL; /* clear default to trigger violation */
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(deterministic_requires_logical_clock) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.logical_now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(runtime_random_seeded_entropy_ok) {
    asx_runtime_hooks hooks;
    uint64_t entropy_value = 42U;
    uint64_t out = 0U;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.entropy.ctx = &entropy_value;
    hooks.entropy.random_u64_fn = test_entropy_u64;
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&out), ASX_OK);
    ASSERT_EQ(out, (uint64_t)42U);
}

TEST(runtime_reactor_wait_prefers_ghost_in_deterministic) {
    asx_runtime_hooks hooks;
    uint32_t ready = 0;
    asx_time logical_now = 99;
    g_wait_calls = 0;
    g_ghost_wait_calls = 0;

    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.ctx = &logical_now;
    hooks.clock.now_ns_fn = test_now_ns;
    hooks.clock.logical_now_ns_fn = test_now_ns;
    hooks.reactor.wait_fn = test_wait;
    hooks.reactor.ghost_wait_fn = test_ghost_wait;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_reactor_wait(50, &ready, 7), ASX_OK);
    ASSERT_EQ(ready, (uint32_t)8);
    ASSERT_EQ(g_wait_calls, 0);
    ASSERT_EQ(g_ghost_wait_calls, 1);
}

TEST(runtime_allocator_seal_blocks_new_allocations) {
    asx_runtime_hooks hooks;
    void *ptr = NULL;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(16, &ptr), ASX_OK);
    ASSERT_NE(ptr, NULL);
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(16, &ptr), ASX_E_ALLOCATOR_SEALED);
}

TEST(runtime_log_sink_silent_when_missing) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    /* Log sink is opt-in; missing sink is silent, not an error */
    ASSERT_EQ(asx_runtime_log_write(1, "x"), ASX_OK);
}

TEST(runtime_log_sink_invoked) {
    asx_runtime_hooks hooks;
    g_log_calls = 0;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.log.write_fn = test_log_sink;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_log_write(2, "deterministic message"), ASX_OK);
    ASSERT_EQ(g_log_calls, 1);
}

int main(void) {
    fprintf(stderr, "=== runtime_hooks_test ===\n");
    RUN_TEST(hooks_init_defaults_validate);
    RUN_TEST(deterministic_rejects_unseeded_entropy);
    RUN_TEST(deterministic_requires_ghost_reactor);
    RUN_TEST(deterministic_requires_logical_clock);
    RUN_TEST(runtime_random_seeded_entropy_ok);
    RUN_TEST(runtime_reactor_wait_prefers_ghost_in_deterministic);
    RUN_TEST(runtime_allocator_seal_blocks_new_allocations);
    RUN_TEST(runtime_log_sink_silent_when_missing);
    RUN_TEST(runtime_log_sink_invoked);
    TEST_REPORT();
    return test_failures;
}
