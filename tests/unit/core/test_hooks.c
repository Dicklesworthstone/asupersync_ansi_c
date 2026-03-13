/*
 * test_hooks.c — unit tests for runtime hook contract
 *
 * Tests: initialization, validation (deterministic/live), allocator seal,
 * hook dispatch, and forbidden entropy in deterministic mode.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test hook implementations                                          */
/* ------------------------------------------------------------------ */

static int alloc_count = 0;
static int free_count = 0;
static int log_count = 0;
static asx_time fake_time = 0;
static uint64_t fake_entropy = 42;

static void *test_malloc(void *ctx, size_t size) {
    (void)ctx;
    alloc_count++;
    return malloc(size);
}

static void *test_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    return realloc(ptr, size);
}

static void test_free(void *ctx, void *ptr) {
    (void)ctx;
    free_count++;
    free(ptr);
}

static asx_time test_clock(void *ctx) {
    (void)ctx;
    return 1000000000ULL; /* 1 second */
}

static asx_time test_logical_clock(void *ctx) {
    (void)ctx;
    return fake_time;
}

static uint64_t test_entropy(void *ctx) {
    (void)ctx;
    return fake_entropy;
}

static void test_log(void *ctx, int level, const char *message) {
    (void)ctx;
    (void)level;
    (void)message;
    log_count++;
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

TEST(hooks_init_defaults) {
    asx_runtime_hooks hooks;
    asx_status s = asx_runtime_hooks_init(&hooks);
    ASSERT_EQ(s, ASX_OK);
    /* Default allocator must be set */
    ASSERT_TRUE(hooks.allocator.malloc_fn != NULL);
    ASSERT_TRUE(hooks.allocator.free_fn != NULL);
    /* Log sink is opt-in */
    ASSERT_TRUE(hooks.log.write_fn == NULL);
    /* Deterministic-safe defaults are installed */
    ASSERT_TRUE(hooks.clock.now_ns_fn != NULL);
    ASSERT_TRUE(hooks.clock.logical_now_ns_fn != NULL);
    ASSERT_TRUE(hooks.entropy.random_u64_fn != NULL);
    ASSERT_TRUE(hooks.reactor.wait_fn == NULL);
    ASSERT_TRUE(hooks.reactor.ghost_wait_fn != NULL);
    /* Flags clear */
    ASSERT_EQ(hooks.deterministic_seeded_prng, (uint8_t)1);
    ASSERT_EQ(hooks.allocator_sealed, (uint8_t)0);
}

TEST(hooks_init_null_rejected) { ASSERT_EQ(asx_runtime_hooks_init(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(hooks_validate_live_needs_clock) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    /* No clock set -> validation fails for live mode */
    hooks.clock.now_ns_fn = NULL;
    hooks.clock.logical_now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
    /* Set clock → passes */
    hooks.clock.now_ns_fn = test_clock;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
}

TEST(hooks_validate_deterministic_needs_logical_clock) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    /* Missing logical clock -> fails in deterministic mode */
    hooks.clock.logical_now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
    /* Set logical clock → passes */
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
}

TEST(hooks_validate_deterministic_forbids_ambient_entropy) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    /* Entropy without seeded PRNG flag → rejected */
    hooks.entropy.random_u64_fn = test_entropy;
    hooks.deterministic_seeded_prng = 0;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
    /* Set seeded PRNG flag → passes */
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
}

TEST(hooks_validate_missing_allocator) {
    asx_runtime_hooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.clock.now_ns_fn = test_clock;
    /* No allocator → rejected */
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_install_and_retrieve) {
    asx_runtime_hooks hooks;
    const asx_runtime_hooks *active;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = test_clock;

    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    active = asx_runtime_get_hooks();
    ASSERT_TRUE(active != NULL);
    ASSERT_TRUE(active->allocator.malloc_fn != NULL);
    ASSERT_TRUE(active->clock.now_ns_fn == test_clock);
}

TEST(hooks_alloc_dispatch) {
    asx_runtime_hooks hooks;
    void *ptr = NULL;
    asx_runtime_hooks_init(&hooks);
    hooks.allocator.malloc_fn = test_malloc;
    hooks.allocator.realloc_fn = test_realloc;
    hooks.allocator.free_fn = test_free;
    hooks.clock.now_ns_fn = test_clock;
    asx_runtime_set_hooks(&hooks);

    alloc_count = 0;
    free_count = 0;

    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    ASSERT_EQ(alloc_count, 1);

    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
    ASSERT_EQ(free_count, 1);
}

TEST(hooks_allocator_seal) {
    asx_runtime_hooks hooks;
    void *ptr = NULL;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = test_clock;
    asx_runtime_set_hooks(&hooks);

    /* Alloc works before seal */
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    asx_runtime_free(ptr);
    ptr = NULL;

    /* Seal allocator */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);

    /* Alloc rejected after seal */
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_E_ALLOCATOR_SEALED);

    /* Second seal is rejected */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_E_INVALID_STATE);
}

TEST(hooks_clock_dispatch) {
    asx_runtime_hooks hooks;
    asx_time now = 0;
    fake_time = 7777;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = test_clock;
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    asx_runtime_set_hooks(&hooks);

    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, (asx_time)fake_time);
}

TEST(hooks_log_dispatch) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = test_clock;
    hooks.log.write_fn = test_log;
    asx_runtime_set_hooks(&hooks);

    log_count = 0;
    ASSERT_EQ(asx_runtime_log_write(0, "test message"), ASX_OK);
    ASSERT_EQ(log_count, 1);
}

TEST(hooks_config_init) {
    asx_runtime_config cfg;
    asx_runtime_config_init(&cfg);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(asx_runtime_config));
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_YIELD);
    ASSERT_EQ(cfg.leak_response, ASX_LEAK_LOG);
    ASSERT_EQ(cfg.finalizer_poll_budget, (uint32_t)100);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)5000000000ULL);
    ASSERT_EQ(cfg.finalizer_escalation, ASX_FINALIZER_BOUNDED_LOG);
    ASSERT_EQ(cfg.max_cancel_chain_depth, (uint16_t)16);
    ASSERT_EQ(cfg.max_cancel_chain_memory, (uint32_t)4096);
}

TEST(hooks_entropy_forbidden_without_prng) {
    asx_runtime_hooks hooks;
    uint64_t val = 0;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = test_clock;
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    hooks.entropy.random_u64_fn = NULL;
    hooks.deterministic_seeded_prng = 1;
    asx_runtime_set_hooks(&hooks);

    /* Should fail: entropy function not installed */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_E_INVALID_STATE);
}

int main(void) {
    fprintf(stderr, "=== test_hooks ===\n");
    RUN_TEST(hooks_init_defaults);
    RUN_TEST(hooks_init_null_rejected);
    RUN_TEST(hooks_validate_live_needs_clock);
    RUN_TEST(hooks_validate_deterministic_needs_logical_clock);
    RUN_TEST(hooks_validate_deterministic_forbids_ambient_entropy);
    RUN_TEST(hooks_validate_missing_allocator);
    RUN_TEST(hooks_install_and_retrieve);
    RUN_TEST(hooks_alloc_dispatch);
    RUN_TEST(hooks_allocator_seal);
    RUN_TEST(hooks_clock_dispatch);
    RUN_TEST(hooks_log_dispatch);
    RUN_TEST(hooks_config_init);
    RUN_TEST(hooks_entropy_forbidden_without_prng);
    TEST_REPORT();
    return test_failures;
}
