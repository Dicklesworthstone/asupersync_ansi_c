/*
 * test_safety_profiles.c — unit tests for safety profiles and fault injection
 *
 * Tests: safety profile query, fault injection (clock skew, clock reverse,
 * entropy constant, alloc fail), trigger_after/trigger_count semantics,
 * fault clear, and deterministic replay consistency.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <string.h>

/* ---- Helper hooks ---- */

static asx_time g_logical_time = 1000000000ULL; /* 1s */

static asx_time test_logical_clock(void *ctx) {
    (void)ctx;
    return g_logical_time;
}

static asx_time test_wall_clock(void *ctx) {
    (void)ctx;
    return 999999999ULL;
}

static uint64_t g_entropy_seq = 0;
static uint64_t test_entropy(void *ctx) {
    (void)ctx;
    return ++g_entropy_seq;
}

static void install_test_hooks(void) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = test_wall_clock;
    hooks.clock.logical_now_ns_fn = test_logical_clock;
    hooks.entropy.random_u64_fn = test_entropy;
    hooks.deterministic_seeded_prng = 1;
    asx_runtime_set_hooks(&hooks);
}

/* ---- Safety profile queries ---- */

TEST(safety_profile_active_matches_compile_flag) {
    asx_safety_profile p = asx_safety_profile_active();
    /* In debug builds (default), should be ASX_SAFETY_DEBUG */
    ASSERT_EQ(p, (asx_safety_profile)ASX_SAFETY_PROFILE_SELECTED);
}

TEST(safety_profile_str_debug) {
    ASSERT_STR_EQ(asx_safety_profile_str(ASX_SAFETY_DEBUG), "debug");
}

TEST(safety_profile_str_hardened) {
    ASSERT_STR_EQ(asx_safety_profile_str(ASX_SAFETY_HARDENED), "hardened");
}

TEST(safety_profile_str_release) {
    ASSERT_STR_EQ(asx_safety_profile_str(ASX_SAFETY_RELEASE), "release");
}

TEST(safety_profile_str_unknown) {
    ASSERT_STR_EQ(asx_safety_profile_str((asx_safety_profile)99), "unknown");
}

/* ---- Fault injection: basic API ---- */

TEST(fault_inject_null_rejected) {
    asx_fault_clear();
    ASSERT_EQ(asx_fault_inject(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(fault_inject_none_kind_rejected) {
    asx_fault_injection f;
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_NONE;
    ASSERT_EQ(asx_fault_inject(&f), ASX_E_INVALID_ARGUMENT);
}

TEST(fault_inject_and_count) {
    asx_fault_injection f;
    asx_fault_clear();
    ASSERT_EQ(asx_fault_injection_count(), (uint32_t)0);

    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_SKEW;
    f.param = 100;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);
    ASSERT_EQ(asx_fault_injection_count(), (uint32_t)1);
}

TEST(fault_clear_resets_count) {
    asx_fault_injection f;
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_SKEW;
    f.param = 100;
    asx_fault_inject(&f);
    ASSERT_TRUE(asx_fault_injection_count() > (uint32_t)0);

    asx_fault_clear();
    ASSERT_EQ(asx_fault_injection_count(), (uint32_t)0);
}

/* ---- Fault injection: clock skew ---- */

TEST(fault_clock_skew_adds_offset) {
    asx_fault_injection f;
    asx_time now = 0;

    install_test_hooks();
    asx_fault_clear();

    /* Without fault: should get logical time */
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, g_logical_time);

    /* Install clock skew: +5000 ns */
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_SKEW;
    f.param = 5000;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, g_logical_time + 5000u);

    asx_fault_clear();
}

/* ---- Fault injection: clock reverse ---- */

TEST(fault_clock_reverse_subtracts_offset) {
    asx_fault_injection f;
    asx_time now = 0;

    install_test_hooks();
    asx_fault_clear();

    g_logical_time = 10000;

    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_REVERSE;
    f.param = 3000;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, (asx_time)7000);

    asx_fault_clear();
    g_logical_time = 1000000000ULL;
}

TEST(fault_clock_reverse_clamps_to_zero) {
    asx_fault_injection f;
    asx_time now = 0;

    install_test_hooks();
    asx_fault_clear();

    g_logical_time = 100;

    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_REVERSE;
    f.param = 500; /* larger than time value */
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, (asx_time)0);

    asx_fault_clear();
    g_logical_time = 1000000000ULL;
}

/* ---- Fault injection: entropy constant ---- */

TEST(fault_entropy_const_overrides_prng) {
    asx_fault_injection f;
    uint64_t val = 0;

    install_test_hooks();
    asx_fault_clear();
    g_entropy_seq = 0;

    /* Without fault: should get incrementing sequence */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)1); /* first call returns 1 */

    /* Install constant entropy fault */
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_ENTROPY_CONST;
    f.param = 0xDEADBEEFULL;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)0xDEADBEEFULL);

    /* Repeated calls give same constant */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)0xDEADBEEFULL);

    asx_fault_clear();
}

/* ---- Fault injection: alloc fail ---- */

TEST(fault_alloc_fail_returns_exhausted) {
    asx_fault_injection f;
    void *ptr = NULL;

    install_test_hooks();
    asx_fault_clear();

    /* Without fault: alloc succeeds */
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);
    asx_runtime_free(ptr);
    ptr = NULL;

    /* Install alloc fail fault */
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_ALLOC_FAIL;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    /* Alloc should now fail */
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_E_RESOURCE_EXHAUSTED);

    asx_fault_clear();
}

/* ---- Fault injection: trigger_after ---- */

TEST(fault_trigger_after_delays_activation) {
    asx_fault_injection f;
    asx_time now = 0;

    install_test_hooks();
    asx_fault_clear();

    /* Skew activates after 2 clock calls */
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_SKEW;
    f.param = 9999;
    f.trigger_after = 2;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    /* First two calls: no skew */
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, g_logical_time);

    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, g_logical_time);

    /* Third call: skew active */
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, g_logical_time + 9999u);

    asx_fault_clear();
}

/* ---- Fault injection: trigger_count ---- */

TEST(fault_trigger_count_limits_injections) {
    asx_fault_injection f;
    uint64_t val = 0;

    install_test_hooks();
    asx_fault_clear();
    g_entropy_seq = 0;

    /* Constant entropy for exactly 2 calls */
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_ENTROPY_CONST;
    f.param = 42;
    f.trigger_after = 0;
    f.trigger_count = 2;
    ASSERT_EQ(asx_fault_inject(&f), ASX_OK);

    /* First call: constant */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);

    /* Second call: constant */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);

    /* Third call: back to normal */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_NE(val, (uint64_t)42);

    asx_fault_clear();
}

/* ---- Determinism: fault injection is deterministic across reruns ---- */

TEST(fault_injection_deterministic_replay) {
    asx_fault_injection f;
    asx_time t1 = 0, t2 = 0;

    install_test_hooks();

    /* Run 1: skew */
    asx_fault_clear();
    memset(&f, 0, sizeof(f));
    f.kind = ASX_FAULT_CLOCK_SKEW;
    f.param = 777;
    asx_fault_inject(&f);
    asx_runtime_now_ns(&t1);

    /* Run 2: same setup, same result */
    asx_fault_clear();
    asx_fault_inject(&f);
    asx_runtime_now_ns(&t2);

    ASSERT_EQ(t1, t2);

    asx_fault_clear();
}

int main(void) {
    fprintf(stderr, "=== test_safety_profiles ===\n");

    /* Safety profile queries */
    RUN_TEST(safety_profile_active_matches_compile_flag);
    RUN_TEST(safety_profile_str_debug);
    RUN_TEST(safety_profile_str_hardened);
    RUN_TEST(safety_profile_str_release);
    RUN_TEST(safety_profile_str_unknown);

    /* Fault injection: basic API */
    RUN_TEST(fault_inject_null_rejected);
    RUN_TEST(fault_inject_none_kind_rejected);
    RUN_TEST(fault_inject_and_count);
    RUN_TEST(fault_clear_resets_count);

    /* Fault injection: clock */
    RUN_TEST(fault_clock_skew_adds_offset);
    RUN_TEST(fault_clock_reverse_subtracts_offset);
    RUN_TEST(fault_clock_reverse_clamps_to_zero);

    /* Fault injection: entropy */
    RUN_TEST(fault_entropy_const_overrides_prng);

    /* Fault injection: allocator */
    RUN_TEST(fault_alloc_fail_returns_exhausted);

    /* Fault injection: trigger semantics */
    RUN_TEST(fault_trigger_after_delays_activation);
    RUN_TEST(fault_trigger_count_limits_injections);

    /* Determinism */
    RUN_TEST(fault_injection_deterministic_replay);

    TEST_REPORT();
    return test_failures;
}
