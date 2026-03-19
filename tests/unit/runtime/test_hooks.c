/*
 * test_hooks.c — unit tests for runtime hooks, config, fault injection
 *
 * Exercises the hook management, validation, dispatch, allocator seal,
 * fault injection, safety profiles, and containment policy APIs from
 * asx_config.h as implemented in hooks.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/asx_config.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Custom hook implementations for testing                             */
/* ------------------------------------------------------------------ */

static int g_alloc_count;
static int g_free_count;
static int g_realloc_count;
static int g_clock_count;
static int g_entropy_count;
static int g_reactor_count;
static int g_native_reactor_count;
static int g_log_count;
static int g_log_level;
static char g_log_buf[256];

static void *test_malloc(void *ctx, size_t size) {
    (void)ctx;
    g_alloc_count++;
    return malloc(size);
}

static void *test_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    g_realloc_count++;
    return realloc(ptr, size);
}

static void test_free(void *ctx, void *ptr) {
    (void)ctx;
    g_free_count++;
    free(ptr);
}

static asx_time test_wall_clock(void *ctx) {
    (void)ctx;
    g_clock_count++;
    return 1000000;
}

static asx_time test_logical_clock(void *ctx) {
    (void)ctx;
    g_clock_count++;
    return 42;
}

static uint64_t test_entropy(void *ctx) {
    (void)ctx;
    g_entropy_count++;
    return 0xCAFEBABEu;
}

static asx_status test_ghost_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    g_reactor_count++;
    *ready_count = 3;
    return ASX_OK;
}

static asx_status test_native_reactor(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    (void)ctx;
    (void)timeout_ms;
    g_native_reactor_count++;
    *ready_count = 7;
    return ASX_OK;
}

static void test_log_sink(void *ctx, int level, const char *message) {
    (void)ctx;
    g_log_count++;
    g_log_level = level;
    if (message) {
        strncpy(g_log_buf, message, sizeof(g_log_buf) - 1);
        g_log_buf[sizeof(g_log_buf) - 1] = '\0';
    }
}

static void reset_counters(void) {
    g_alloc_count = 0;
    g_free_count = 0;
    g_realloc_count = 0;
    g_clock_count = 0;
    g_entropy_count = 0;
    g_reactor_count = 0;
    g_native_reactor_count = 0;
    g_log_count = 0;
    g_log_level = 0;
    g_log_buf[0] = '\0';
}

static asx_status install_test_hooks(void) {
    asx_runtime_hooks hooks;
    asx_status st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return st;

    hooks.allocator.malloc_fn = test_malloc;
    hooks.allocator.realloc_fn = test_realloc;
    hooks.allocator.free_fn = test_free;
    hooks.clock.now_ns_fn = test_wall_clock;
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    hooks.entropy.random_u64_fn = test_entropy;
    hooks.reactor.ghost_wait_fn = test_ghost_reactor;
    hooks.log.write_fn = test_log_sink;
    hooks.deterministic_seeded_prng = 1;

    return asx_runtime_set_hooks(&hooks);
}

/* ------------------------------------------------------------------ */
/* Hook init                                                           */
/* ------------------------------------------------------------------ */

TEST(hooks_init_null_returns_error) {
    ASSERT_EQ(asx_runtime_hooks_init(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_init_sets_defaults) {
    asx_runtime_hooks hooks;
    memset(&hooks, 0xFF, sizeof(hooks));

    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_TRUE(hooks.allocator.malloc_fn != NULL);
    ASSERT_TRUE(hooks.allocator.realloc_fn != NULL);
    ASSERT_TRUE(hooks.allocator.free_fn != NULL);
    ASSERT_TRUE(hooks.clock.logical_now_ns_fn != NULL);
    ASSERT_TRUE(hooks.entropy.random_u64_fn != NULL);
    ASSERT_EQ(hooks.deterministic_seeded_prng, 1);
    ASSERT_EQ(hooks.allocator_sealed, 0);
}

/* ------------------------------------------------------------------ */
/* Hook validation                                                     */
/* ------------------------------------------------------------------ */

TEST(hooks_validate_null_returns_error) {
    ASSERT_EQ(asx_runtime_hooks_validate(NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_hooks_validate(NULL, 1), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_default_passes) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
}

TEST(hooks_validate_no_malloc_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.allocator.malloc_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_no_free_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.allocator.free_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hooks_validate_deterministic_no_logical_clock_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.logical_now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(hooks_validate_deterministic_unseeded_entropy_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.deterministic_seeded_prng = 0;
    /* entropy fn set + not seeded → determinism violation */
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_E_DETERMINISM_VIOLATION);
}

TEST(hooks_validate_live_no_wall_clock_fails) {
    asx_runtime_hooks hooks;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.now_ns_fn = NULL;
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Hook install and retrieval                                          */
/* ------------------------------------------------------------------ */

TEST(set_hooks_null_returns_error) {
    ASSERT_EQ(asx_runtime_set_hooks(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(set_hooks_installs_and_get_retrieves) {
    const asx_runtime_hooks *retrieved;

    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    retrieved = asx_runtime_get_hooks();
    ASSERT_TRUE(retrieved != NULL);
    ASSERT_TRUE(retrieved->allocator.malloc_fn == test_malloc);
    ASSERT_TRUE(retrieved->allocator.free_fn == test_free);
    ASSERT_TRUE(retrieved->log.write_fn == test_log_sink);
}

TEST(runtime_reset_clears_installed_hooks) {
    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_TRUE(asx_runtime_get_hooks() != NULL);

    asx_runtime_reset();
    ASSERT_TRUE(asx_runtime_get_hooks() == NULL);
}

/* ------------------------------------------------------------------ */
/* Allocator seal                                                      */
/* ------------------------------------------------------------------ */

TEST(seal_without_hooks_returns_invalid_state) {
    asx_runtime_reset();
    /* No hooks installed after reset */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_E_INVALID_STATE);
}

TEST(seal_then_alloc_returns_sealed) {
    void *ptr = NULL;

    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_ALLOCATOR_SEALED);
    ASSERT_TRUE(ptr == NULL);
}

TEST(seal_then_realloc_returns_sealed) {
    void *ptr = NULL;

    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_realloc(NULL, 64, &ptr), ASX_E_ALLOCATOR_SEALED);
}

TEST(double_seal_returns_invalid_state) {
    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Hook-backed allocator dispatch                                      */
/* ------------------------------------------------------------------ */

TEST(alloc_null_out_returns_error) {
    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(alloc_no_hooks_returns_hook_missing) {
    void *ptr = NULL;
    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_HOOK_MISSING);
}

TEST(alloc_dispatches_to_hook) {
    void *ptr = NULL;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    ASSERT_TRUE(g_alloc_count > 0);

    /* Clean up */
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
}

TEST(realloc_dispatches_to_hook) {
    void *ptr = NULL;
    void *ptr2 = NULL;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_OK);
    ASSERT_EQ(asx_runtime_realloc(ptr, 128, &ptr2), ASX_OK);
    ASSERT_TRUE(ptr2 != NULL);
    ASSERT_TRUE(g_realloc_count > 0);

    ASSERT_EQ(asx_runtime_free(ptr2), ASX_OK);
}

TEST(free_no_hooks_returns_hook_missing) {
    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_free(NULL), ASX_E_HOOK_MISSING);
}

TEST(free_dispatches_to_hook) {
    void *ptr = NULL;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_OK);
    ASSERT_EQ(asx_runtime_free(ptr), ASX_OK);
    ASSERT_TRUE(g_free_count > 0);
}

/* ------------------------------------------------------------------ */
/* Clock dispatch                                                      */
/* ------------------------------------------------------------------ */

TEST(now_ns_null_returns_error) {
    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_now_ns(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(now_ns_no_hooks_returns_hook_missing) {
    asx_time t;
    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_now_ns(&t), ASX_E_HOOK_MISSING);
}

TEST(now_ns_dispatches_to_logical_clock) {
    asx_time t = 0;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&t), ASX_OK);
    /* In deterministic mode, dispatches to logical clock (returns 42) */
    ASSERT_EQ(t, (asx_time)42);
    ASSERT_TRUE(g_clock_count > 0);
}

/* ------------------------------------------------------------------ */
/* Entropy dispatch                                                    */
/* ------------------------------------------------------------------ */

TEST(random_u64_null_returns_error) {
    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(random_u64_no_hooks_returns_hook_missing) {
    uint64_t v;
    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_random_u64(&v), ASX_E_HOOK_MISSING);
}

TEST(random_u64_missing_entropy_stream_returns_invalid_state) {
    asx_runtime_hooks hooks;
    uint64_t v = 0;

    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.entropy.random_u64_fn = NULL;
    hooks.deterministic_seeded_prng = 1;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&v), ASX_E_INVALID_STATE);
}

TEST(random_u64_dispatches_to_hook) {
    uint64_t v = 0;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_random_u64(&v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)0xCAFEBABEu);
    ASSERT_TRUE(g_entropy_count > 0);
}

/* ------------------------------------------------------------------ */
/* Reactor dispatch                                                    */
/* ------------------------------------------------------------------ */

TEST(reactor_wait_null_returns_error) {
    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_runtime_reactor_wait(100, NULL, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(reactor_wait_no_hooks_returns_hook_missing) {
    uint32_t ready = 0;
    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 0), ASX_E_HOOK_MISSING);
}

TEST(reactor_wait_dispatches_to_ghost_hook) {
    uint32_t ready = 0;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 5), ASX_OK);
    ASSERT_EQ(ready, 3u);
    ASSERT_TRUE(g_reactor_count > 0);
}

TEST(reactor_wait_prefers_native_hook_when_both_are_installed) {
    uint32_t ready = 0;
    asx_runtime_hooks hooks;

    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.reactor.wait_fn = test_native_reactor;
    hooks.reactor.ghost_wait_fn = test_ghost_reactor;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    ASSERT_EQ(asx_runtime_reactor_wait(100, &ready, 5), ASX_OK);
#if ASX_DETERMINISTIC
    /* In deterministic mode, ghost reactor takes priority for reproducibility. */
    ASSERT_EQ(ready, 3u);
    ASSERT_EQ(g_reactor_count, 1);
    ASSERT_EQ(g_native_reactor_count, 0);
#else
    /* In non-deterministic mode, native reactor takes priority for live I/O. */
    ASSERT_EQ(ready, 7u);
    ASSERT_EQ(g_native_reactor_count, 1);
    ASSERT_EQ(g_reactor_count, 0);
#endif
}

/* ------------------------------------------------------------------ */
/* Log dispatch                                                        */
/* ------------------------------------------------------------------ */

TEST(log_write_no_hooks_returns_hook_missing) {
    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_log_write(1, "test"), ASX_E_HOOK_MISSING);
}

TEST(log_write_dispatches_to_hook) {
    asx_runtime_reset();
    reset_counters();
    ASSERT_EQ(install_test_hooks(), ASX_OK);

    ASSERT_EQ(asx_runtime_log_write(3, "hello world"), ASX_OK);
    ASSERT_EQ(g_log_count, 1);
    ASSERT_EQ(g_log_level, 3);
    ASSERT_EQ(strcmp(g_log_buf, "hello world"), 0);
}

TEST(log_write_no_sink_is_silent) {
    asx_runtime_hooks hooks;

    asx_runtime_reset();
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.log.write_fn = NULL;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    /* Should return OK even without a log sink */
    ASSERT_EQ(asx_runtime_log_write(1, "ignored"), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Fault injection                                                     */
/* ------------------------------------------------------------------ */

TEST(fault_inject_null_returns_error) { ASSERT_EQ(asx_fault_inject(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(fault_inject_none_kind_returns_error) {
    asx_fault_injection fi;
    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_NONE;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_E_INVALID_ARGUMENT);
}

TEST(fault_inject_and_count) {
    asx_fault_injection fi;

    asx_runtime_reset();
    ASSERT_EQ(asx_fault_clear(), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), 0u);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_CLOCK_SKEW;
    fi.param = 100;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), 1u);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), 0u);
}

TEST(fault_inject_exhaustion) {
    asx_fault_injection fi;
    uint32_t i;

    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_ALLOC_FAIL;
    /* Fill all 8 slots */
    for (i = 0; i < 8; i++) { ASSERT_EQ(asx_fault_inject(&fi), ASX_OK); }
    /* 9th should fail */
    ASSERT_EQ(asx_fault_inject(&fi), ASX_E_RESOURCE_EXHAUSTED);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

TEST(fault_alloc_fail_triggers) {
    asx_fault_injection fi;
    void *ptr = NULL;

    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_ALLOC_FAIL;
    fi.trigger_after = 0;
    fi.trigger_count = 0; /* permanent */
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);

    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_TRUE(ptr == NULL);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

TEST(fault_clock_skew_adds_offset) {
    asx_fault_injection fi;
    asx_time t = 0;

    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_CLOCK_SKEW;
    fi.param = 1000;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&t), ASX_OK);
    /* logical clock returns 42, skew adds 1000 → 1042 */
    ASSERT_EQ(t, (asx_time)1042);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

TEST(fault_entropy_const_overrides) {
    asx_fault_injection fi;
    uint64_t v = 0;

    asx_runtime_reset();
    ASSERT_EQ(install_test_hooks(), ASX_OK);
    ASSERT_EQ(asx_fault_clear(), ASX_OK);

    memset(&fi, 0, sizeof(fi));
    fi.kind = ASX_FAULT_ENTROPY_CONST;
    fi.param = 0xDEADBEEFu;
    ASSERT_EQ(asx_fault_inject(&fi), ASX_OK);

    ASSERT_EQ(asx_runtime_random_u64(&v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)0xDEADBEEFu);

    ASSERT_EQ(asx_fault_clear(), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Config init                                                         */
/* ------------------------------------------------------------------ */

TEST(config_init_null_safe) {
    /* Should not crash */
    asx_runtime_config_init(NULL);
}

TEST(config_init_sets_defaults) {
    asx_runtime_config cfg;
    memset(&cfg, 0xFF, sizeof(cfg));

    asx_runtime_config_init(&cfg);
    ASSERT_EQ(cfg.size, (uint32_t)sizeof(cfg));
    ASSERT_EQ(cfg.leak_response, (asx_leak_response)ASX_LEAK_LOG);
    ASSERT_EQ(cfg.finalizer_poll_budget, 100u);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)5000000000ULL);
    ASSERT_EQ(cfg.finalizer_escalation, (asx_finalizer_escalation)ASX_FINALIZER_BOUNDED_LOG);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 16u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 4096u);
}

/* ------------------------------------------------------------------ */
/* Safety profile queries                                              */
/* ------------------------------------------------------------------ */

TEST(safety_profile_active_returns_valid) {
    asx_safety_profile p = asx_safety_profile_active();
    ASSERT_TRUE(p == ASX_SAFETY_DEBUG || p == ASX_SAFETY_HARDENED || p == ASX_SAFETY_RELEASE);
}

TEST(safety_profile_str_returns_known_strings) {
    ASSERT_EQ(strcmp(asx_safety_profile_str(ASX_SAFETY_DEBUG), "debug"), 0);
    ASSERT_EQ(strcmp(asx_safety_profile_str(ASX_SAFETY_HARDENED), "hardened"), 0);
    ASSERT_EQ(strcmp(asx_safety_profile_str(ASX_SAFETY_RELEASE), "release"), 0);
}

/* ------------------------------------------------------------------ */
/* Containment policy                                                  */
/* ------------------------------------------------------------------ */

TEST(containment_policy_for_profile_maps_correctly) {
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_DEBUG), ASX_CONTAIN_FAIL_FAST);
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_HARDENED), ASX_CONTAIN_POISON_REGION);
    ASSERT_EQ(asx_containment_policy_for_profile(ASX_SAFETY_RELEASE), ASX_CONTAIN_ERROR_ONLY);
}

TEST(containment_policy_active_matches_profile) {
    asx_safety_profile p = asx_safety_profile_active();
    asx_containment_policy expected = asx_containment_policy_for_profile(p);
    ASSERT_EQ(asx_containment_policy_active(), expected);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_hooks ===\n");

    /* --- Run "no hooks installed" tests first to exercise pristine state --- */
    RUN_TEST(seal_without_hooks_returns_invalid_state);
    RUN_TEST(alloc_no_hooks_returns_hook_missing);
    RUN_TEST(free_no_hooks_returns_hook_missing);
    RUN_TEST(now_ns_no_hooks_returns_hook_missing);
    RUN_TEST(random_u64_no_hooks_returns_hook_missing);
    RUN_TEST(reactor_wait_no_hooks_returns_hook_missing);
    RUN_TEST(log_write_no_hooks_returns_hook_missing);

    /* Hook init */
    RUN_TEST(hooks_init_null_returns_error);
    RUN_TEST(hooks_init_sets_defaults);

    /* Hook validation */
    RUN_TEST(hooks_validate_null_returns_error);
    RUN_TEST(hooks_validate_default_passes);
    RUN_TEST(hooks_validate_no_malloc_fails);
    RUN_TEST(hooks_validate_no_free_fails);
    RUN_TEST(hooks_validate_deterministic_no_logical_clock_fails);
    RUN_TEST(hooks_validate_deterministic_unseeded_entropy_fails);
    RUN_TEST(hooks_validate_live_no_wall_clock_fails);

    /* Hook install/retrieval */
    RUN_TEST(set_hooks_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(set_hooks_installs_and_get_retrieves);
    asx_runtime_reset();
    RUN_TEST(runtime_reset_clears_installed_hooks);

    /* Allocator seal */
    asx_runtime_reset();
    RUN_TEST(seal_then_alloc_returns_sealed);
    asx_runtime_reset();
    RUN_TEST(seal_then_realloc_returns_sealed);
    asx_runtime_reset();
    RUN_TEST(double_seal_returns_invalid_state);

    /* Allocator dispatch */
    asx_runtime_reset();
    RUN_TEST(alloc_null_out_returns_error);
    asx_runtime_reset();
    RUN_TEST(alloc_dispatches_to_hook);
    asx_runtime_reset();
    RUN_TEST(realloc_dispatches_to_hook);
    asx_runtime_reset();
    RUN_TEST(free_dispatches_to_hook);

    /* Clock dispatch */
    asx_runtime_reset();
    RUN_TEST(now_ns_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(now_ns_dispatches_to_logical_clock);

    /* Entropy dispatch */
    asx_runtime_reset();
    RUN_TEST(random_u64_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(random_u64_no_hooks_returns_hook_missing);
    asx_runtime_reset();
    RUN_TEST(random_u64_missing_entropy_stream_returns_invalid_state);
    asx_runtime_reset();
    RUN_TEST(random_u64_dispatches_to_hook);

    /* Reactor dispatch */
    asx_runtime_reset();
    RUN_TEST(reactor_wait_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(reactor_wait_dispatches_to_ghost_hook);
    asx_runtime_reset();
    RUN_TEST(reactor_wait_prefers_native_hook_when_both_are_installed);

    /* Log dispatch */
    asx_runtime_reset();
    RUN_TEST(log_write_dispatches_to_hook);
    asx_runtime_reset();
    RUN_TEST(log_write_no_sink_is_silent);

    /* Fault injection */
    RUN_TEST(fault_inject_null_returns_error);
    RUN_TEST(fault_inject_none_kind_returns_error);
    asx_runtime_reset();
    RUN_TEST(fault_inject_and_count);
    RUN_TEST(fault_inject_exhaustion);
    asx_runtime_reset();
    RUN_TEST(fault_alloc_fail_triggers);
    asx_runtime_reset();
    RUN_TEST(fault_clock_skew_adds_offset);
    asx_runtime_reset();
    RUN_TEST(fault_entropy_const_overrides);

    /* Config init */
    RUN_TEST(config_init_null_safe);
    RUN_TEST(config_init_sets_defaults);

    /* Safety profile */
    RUN_TEST(safety_profile_active_returns_valid);
    RUN_TEST(safety_profile_str_returns_known_strings);

    /* Containment policy */
    RUN_TEST(containment_policy_for_profile_maps_correctly);
    RUN_TEST(containment_policy_active_matches_profile);

    TEST_REPORT();
    return test_failures;
}
