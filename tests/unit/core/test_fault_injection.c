/*
 * test_fault_injection.c — deterministic fault injection tests
 *
 * Verifies that the runtime behaves correctly under injected faults:
 * clock anomalies, entropy patterns, allocator failures, and hook
 * seal violations. Uses custom hook implementations to simulate
 * fault scenarios without new API surface.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <stdlib.h>
#include <string.h>

/* ---- Fault injection hook implementations ---- */

static asx_time g_clock_value;
static uint64_t g_entropy_value;
static uint64_t g_entropy_sequence;
static int g_alloc_fail_after;
static int g_alloc_call_count;

static asx_time fault_clock(void *ctx)
{
    (void)ctx;
    return g_clock_value;
}

static asx_time fault_logical_clock(void *ctx)
{
    (void)ctx;
    return g_clock_value;
}

static uint64_t fault_entropy(void *ctx)
{
    (void)ctx;
    g_entropy_sequence++;
    return g_entropy_value;
}

static uint64_t seeded_entropy(void *ctx)
{
    (void)ctx;
    /* Simple LCG matching default PRNG for determinism verification */
    g_entropy_value = g_entropy_value * 6364136223846793005ULL + 1442695040888963407ULL;
    g_entropy_sequence++;
    return g_entropy_value;
}

static void *failing_malloc(void *ctx, size_t size)
{
    (void)ctx;
    g_alloc_call_count++;
    if (g_alloc_fail_after >= 0 && g_alloc_call_count > g_alloc_fail_after) {
        return NULL;
    }
    return malloc(size);
}

static void *failing_realloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    g_alloc_call_count++;
    if (g_alloc_fail_after >= 0 && g_alloc_call_count > g_alloc_fail_after) {
        return NULL;
    }
    return realloc(ptr, size);
}

static void passthrough_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

static void install_fault_hooks(void)
{
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = fault_clock;
    hooks.clock.logical_now_ns_fn = fault_logical_clock;
    hooks.entropy.random_u64_fn = fault_entropy;
    hooks.deterministic_seeded_prng = 1;
    hooks.allocator.malloc_fn = failing_malloc;
    hooks.allocator.realloc_fn = failing_realloc;
    hooks.allocator.free_fn = passthrough_free;
    (void)asx_runtime_set_hooks(&hooks);
}

static void reset_fault_state(void)
{
    g_clock_value = 1000000000ULL; /* 1 second */
    g_entropy_value = 42;
    g_entropy_sequence = 0;
    g_alloc_fail_after = -1; /* no failure */
    g_alloc_call_count = 0;
}

/* ---- Clock fault injection ---- */

TEST(fault_clock_backward_budget_not_past_deadline) {
    asx_budget budget;
    asx_time now;
    reset_fault_state();
    install_fault_hooks();

    /* Set a deadline in the "future" */
    budget = asx_budget_infinite();
    budget.deadline = 2000000000ULL; /* 2 seconds */

    /* Clock at 1 second — not past deadline */
    g_clock_value = 1000000000ULL;
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, 1000000000ULL);
    ASSERT_FALSE(asx_budget_is_past_deadline(&budget, now));

    /* Clock jumps backward to 500ms — still not past deadline */
    g_clock_value = 500000000ULL;
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, 500000000ULL);
    ASSERT_FALSE(asx_budget_is_past_deadline(&budget, now));
}

TEST(fault_clock_overflow_boundary) {
    asx_budget budget;
    asx_time now;
    reset_fault_state();
    install_fault_hooks();

    /* Near-max clock value */
    g_clock_value = UINT64_MAX - 1000u;
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, UINT64_MAX - 1000u);

    /* Budget with deadline at max — not past */
    budget = asx_budget_infinite();
    budget.deadline = UINT64_MAX;
    ASSERT_FALSE(asx_budget_is_past_deadline(&budget, now));

    /* Budget with deadline below current — past */
    budget.deadline = 1u;
    ASSERT_TRUE(asx_budget_is_past_deadline(&budget, now));
}

TEST(fault_clock_zero_no_deadline) {
    asx_budget budget;
    asx_time now;
    reset_fault_state();
    install_fault_hooks();

    /* Clock at zero */
    g_clock_value = 0;
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(now, (asx_time)0);

    /* Budget with deadline=0 means unconstrained — never past */
    budget = asx_budget_infinite();
    ASSERT_EQ(budget.deadline, (asx_time)0);
    ASSERT_FALSE(asx_budget_is_past_deadline(&budget, now));
}

/* ---- Entropy fault injection ---- */

TEST(fault_entropy_deterministic_sequence) {
    uint64_t v1, v2, v3;
    uint64_t r1, r2, r3;
    reset_fault_state();
    install_fault_hooks();

    /* Install seeded entropy for reproducible sequence */
    {
        asx_runtime_hooks hooks;
        asx_runtime_hooks_init(&hooks);
        hooks.clock.now_ns_fn = fault_clock;
        hooks.clock.logical_now_ns_fn = fault_logical_clock;
        hooks.entropy.random_u64_fn = seeded_entropy;
        hooks.deterministic_seeded_prng = 1;
        (void)asx_runtime_set_hooks(&hooks);
    }

    /* First run */
    g_entropy_value = 0x5DEECE66DULL;
    g_entropy_sequence = 0;
    ASSERT_EQ(asx_runtime_random_u64(&v1), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&v2), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&v3), ASX_OK);

    /* Second run with same seed — must produce identical sequence */
    g_entropy_value = 0x5DEECE66DULL;
    g_entropy_sequence = 0;
    ASSERT_EQ(asx_runtime_random_u64(&r1), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&r2), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&r3), ASX_OK);

    ASSERT_EQ(v1, r1);
    ASSERT_EQ(v2, r2);
    ASSERT_EQ(v3, r3);
}

TEST(fault_entropy_constant_value) {
    uint64_t v1;
    reset_fault_state();
    install_fault_hooks();

    /* Entropy hook returns constant — degenerate but deterministic */
    g_entropy_value = 0xDEADBEEFCAFEBABEULL;
    ASSERT_EQ(asx_runtime_random_u64(&v1), ASX_OK);
    /* fault_entropy increments sequence but returns same base value */
    ASSERT_TRUE(v1 != 0); /* Non-zero value returned */
}

/* ---- Allocator fault injection ---- */

TEST(fault_allocator_immediate_failure) {
    void *ptr = NULL;
    reset_fault_state();
    install_fault_hooks();

    /* Allocator fails on first call */
    g_alloc_fail_after = 0;
    ASSERT_EQ(asx_runtime_alloc(64, &ptr), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_TRUE(ptr == NULL);
}

TEST(fault_allocator_delayed_failure) {
    void *p1 = NULL;
    void *p2 = NULL;
    reset_fault_state();
    install_fault_hooks();

    /* First alloc succeeds, second fails */
    g_alloc_fail_after = 1;
    ASSERT_EQ(asx_runtime_alloc(32, &p1), ASX_OK);
    ASSERT_TRUE(p1 != NULL);

    ASSERT_EQ(asx_runtime_alloc(32, &p2), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_TRUE(p2 == NULL);

    (void)asx_runtime_free(p1);
}

TEST(fault_allocator_realloc_failure) {
    void *ptr = NULL;
    void *new_ptr = NULL;
    reset_fault_state();
    install_fault_hooks();

    /* Alloc succeeds, realloc fails */
    g_alloc_fail_after = 1;
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_OK);
    ASSERT_TRUE(ptr != NULL);

    ASSERT_EQ(asx_runtime_realloc(ptr, 128, &new_ptr),
              ASX_E_RESOURCE_EXHAUSTED);

    /* Original pointer still valid — caller must free */
    (void)asx_runtime_free(ptr);
}

/* ---- Allocator seal enforcement ---- */

TEST(fault_allocator_seal_blocks_alloc) {
    void *ptr = NULL;
    reset_fault_state();
    install_fault_hooks();

    /* Seal allocator */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);

    /* All allocations blocked */
    ASSERT_EQ(asx_runtime_alloc(32, &ptr), ASX_E_ALLOCATOR_SEALED);
    ASSERT_TRUE(ptr == NULL);

    ASSERT_EQ(asx_runtime_realloc(NULL, 32, &ptr), ASX_E_ALLOCATOR_SEALED);
    ASSERT_TRUE(ptr == NULL);

    /* Double seal is rejected */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_E_INVALID_STATE);
}

/* ---- Determinism policy enforcement ---- */

TEST(fault_determinism_rejects_unseeded_entropy) {
    asx_runtime_hooks hooks;
    reset_fault_state();

    asx_runtime_hooks_init(&hooks);
    hooks.clock.logical_now_ns_fn = fault_logical_clock;
    hooks.entropy.random_u64_fn = fault_entropy;
    hooks.deterministic_seeded_prng = 0; /* NOT seeded */

    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1),
              ASX_E_DETERMINISM_VIOLATION);
}

TEST(fault_determinism_rejects_missing_logical_clock) {
    asx_runtime_hooks hooks;
    reset_fault_state();

    asx_runtime_hooks_init(&hooks);
    hooks.clock.logical_now_ns_fn = NULL; /* missing logical clock */

    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1),
              ASX_E_DETERMINISM_VIOLATION);
}

TEST(fault_determinism_accepts_valid_config) {
    asx_runtime_hooks hooks;
    reset_fault_state();

    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = fault_clock;
    hooks.clock.logical_now_ns_fn = fault_logical_clock;
    hooks.entropy.random_u64_fn = seeded_entropy;
    hooks.deterministic_seeded_prng = 1;

    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
}

/* ---- Error ledger integration with fault injection ---- */

TEST(fault_error_ledger_captures_alloc_failure) {
    void *ptr = NULL;
    asx_task_id bound;
    reset_fault_state();
    install_fault_hooks();

    asx_error_ledger_reset();
    asx_error_ledger_bind_task(ASX_INVALID_ID);

    /* Inject allocator failure */
    g_alloc_fail_after = 0;

    /* ASX_TRY won't work here since we're not in a function returning
     * asx_status. Instead, record manually. */
    {
        asx_status st = asx_runtime_alloc(64, &ptr);
        if (st != ASX_OK) {
            asx_error_ledger_record_current(st, "asx_runtime_alloc",
                                             __FILE__, (uint32_t)__LINE__);
        }
    }

    /* Verify ledger captured the failure */
    bound = asx_error_ledger_bound_task();
    ASSERT_EQ(asx_error_ledger_count(bound), (uint32_t)1);

    {
        asx_error_ledger_entry entry;
        ASSERT_TRUE(asx_error_ledger_get(bound, 0, &entry));
        ASSERT_EQ(entry.status, ASX_E_RESOURCE_EXHAUSTED);
    }
}

/* ---- Lifecycle operations under fault conditions ---- */

static asx_status noop_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_OK;
}

TEST(fault_lifecycle_region_open_succeeds_without_alloc) {
    asx_region_id rid;
    reset_fault_state();
    install_fault_hooks();
    asx_runtime_reset();

    /* Walking skeleton uses static arenas — region open doesn't
     * need the hook allocator. Should succeed even with sealed alloc. */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
}

TEST(fault_lifecycle_spawn_succeeds_without_alloc) {
    asx_region_id rid;
    asx_task_id tid;
    reset_fault_state();

    /* Re-install hooks (seal is per-install) */
    install_fault_hooks();
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Seal allocator — walking skeleton task spawn is static */
    ASSERT_EQ(asx_runtime_seal_allocator(), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, noop_poll, NULL, &tid), ASX_OK);
}

int main(void)
{
    fprintf(stderr, "=== test_fault_injection ===\n");

    /* Clock fault injection */
    RUN_TEST(fault_clock_backward_budget_not_past_deadline);
    RUN_TEST(fault_clock_overflow_boundary);
    RUN_TEST(fault_clock_zero_no_deadline);

    /* Entropy fault injection */
    RUN_TEST(fault_entropy_deterministic_sequence);
    RUN_TEST(fault_entropy_constant_value);

    /* Allocator fault injection */
    RUN_TEST(fault_allocator_immediate_failure);
    RUN_TEST(fault_allocator_delayed_failure);
    RUN_TEST(fault_allocator_realloc_failure);

    /* Allocator seal enforcement */
    RUN_TEST(fault_allocator_seal_blocks_alloc);

    /* Determinism policy enforcement */
    RUN_TEST(fault_determinism_rejects_unseeded_entropy);
    RUN_TEST(fault_determinism_rejects_missing_logical_clock);
    RUN_TEST(fault_determinism_accepts_valid_config);

    /* Error ledger integration */
    RUN_TEST(fault_error_ledger_captures_alloc_failure);

    /* Lifecycle under faults */
    RUN_TEST(fault_lifecycle_region_open_succeeds_without_alloc);
    RUN_TEST(fault_lifecycle_spawn_succeeds_without_alloc);

    TEST_REPORT();
    return test_failures;
}
