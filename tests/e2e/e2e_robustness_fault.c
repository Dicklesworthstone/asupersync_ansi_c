/*
 * e2e_robustness_fault.c — robustness e2e for fault-injection boundaries
 *
 * Exercises: clock anomalies (backward, overflow, zero), entropy
 * determinism with seeded PRNG, allocator failure paths, scheduler
 * behavior under injected faults, and trace digest stability.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* ---- Fault injection hook state ---- */

static asx_time g_clock_value;
static uint64_t g_entropy_value;
static uint64_t g_entropy_sequence;
static int g_alloc_fail_after;
static int g_alloc_call_count;

static asx_time fault_clock(void *ctx) {
    (void)ctx;
    return g_clock_value;
}

static asx_time fault_logical_clock(void *ctx) {
    (void)ctx;
    return g_clock_value;
}

static uint64_t fault_entropy(void *ctx) {
    (void)ctx;
    g_entropy_sequence++;
    return g_entropy_value;
}

static uint64_t seeded_entropy(void *ctx) {
    (void)ctx;
    g_entropy_value = g_entropy_value * 6364136223846793005ULL + 1442695040888963407ULL;
    g_entropy_sequence++;
    return g_entropy_value;
}

static void *failing_malloc(void *ctx, size_t size) {
    (void)ctx;
    g_alloc_call_count++;
    if (g_alloc_fail_after >= 0 && g_alloc_call_count > g_alloc_fail_after) { return NULL; }
    return malloc(size);
}

static void *failing_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    g_alloc_call_count++;
    if (g_alloc_fail_after >= 0 && g_alloc_call_count > g_alloc_fail_after) { return NULL; }
    return realloc(ptr, size);
}

static void passthrough_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static void install_fault_hooks(void) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = fault_clock;
    hooks.clock.logical_now_ns_fn = fault_logical_clock;
    hooks.entropy.random_u64_fn = fault_entropy;
    hooks.deterministic_seeded_prng = 1;
    hooks.allocator.malloc_fn = failing_malloc;
    hooks.allocator.realloc_fn = failing_realloc;
    hooks.allocator.free_fn = passthrough_free;
    IGNORE_RC(asx_runtime_set_hooks(&hooks));
}

static void reset_fault_state(void) {
    g_clock_value = 1000000000ULL; /* 1 second */
    g_entropy_value = 42;
    g_entropy_sequence = 0;
    g_alloc_fail_after = -1; /* no failure */
    g_alloc_call_count = 0;
}

static asx_status poll_complete_fn(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}


/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* robust-fault-time-001: clock backward jump does not corrupt state */
static void scenario_clock_backward(void) {
    SCENARIO_BEGIN("robust-fault-time-001.clock_backward");
    asx_runtime_reset();
    reset_fault_state();
    install_fault_hooks();

    asx_region_id rid;
    asx_task_id tid;

    g_clock_value = 2000000000ULL; /* 2 seconds */

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");

    /* Jump clock backward */
    g_clock_value = 500000000ULL; /* 0.5 seconds */

    asx_budget budget = asx_budget_from_polls(10);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "scheduler should complete despite clock jump");

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task should complete");

    SCENARIO_END();
}

/* robust-fault-time-002: clock at zero does not crash */
static void scenario_clock_zero(void) {
    SCENARIO_BEGIN("robust-fault-time-002.clock_zero");
    asx_runtime_reset();
    reset_fault_state();
    install_fault_hooks();

    g_clock_value = 0;

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "scheduler with zero clock");

    SCENARIO_END();
}

/* robust-fault-time-003: clock overflow (max uint64) */
static void scenario_clock_overflow(void) {
    SCENARIO_BEGIN("robust-fault-time-003.clock_overflow");
    asx_runtime_reset();
    reset_fault_state();
    install_fault_hooks();

    g_clock_value = 0xFFFFFFFFFFFFFFFFULL;

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "scheduler with max clock");

    SCENARIO_END();
}

/* robust-fault-entropy-001: seeded entropy is deterministic across runs */
static void scenario_entropy_deterministic(void) {
    SCENARIO_BEGIN("robust-fault-entropy-001.seeded_deterministic");
    asx_runtime_reset();
    reset_fault_state();

    /* Install seeded entropy */
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    hooks.clock.now_ns_fn = fault_clock;
    hooks.clock.logical_now_ns_fn = fault_logical_clock;
    hooks.entropy.random_u64_fn = seeded_entropy;
    hooks.deterministic_seeded_prng = 1;
    IGNORE_RC(asx_runtime_set_hooks(&hooks));

    g_entropy_value = 42;
    g_entropy_sequence = 0;

    /* Generate sequence */
    uint64_t vals1[4];
    uint32_t i;
    for (i = 0; i < 4; i++) { IGNORE_RC(asx_runtime_random_u64(&vals1[i])); }
    uint64_t seq1 = g_entropy_sequence;

    /* Reset and replay */
    g_entropy_value = 42;
    g_entropy_sequence = 0;

    uint64_t vals2[4];
    for (i = 0; i < 4; i++) { IGNORE_RC(asx_runtime_random_u64(&vals2[i])); }
    uint64_t seq2 = g_entropy_sequence;

    SCENARIO_CHECK(seq1 == seq2, "sequence counts must match");
    for (i = 0; i < 4; i++) { SCENARIO_CHECK(vals1[i] == vals2[i], "entropy values must match"); }

    SCENARIO_END();
}

/* robust-fault-entropy-002: constant entropy does not cause infinite loops */
static void scenario_entropy_constant(void) {
    SCENARIO_BEGIN("robust-fault-entropy-002.constant_entropy");
    asx_runtime_reset();
    reset_fault_state();
    install_fault_hooks();

    /* Constant entropy: always returns 0 */
    g_entropy_value = 0;

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "scheduler with constant entropy");

    SCENARIO_END();
}

/* robust-fault-alloc-001: allocator seal blocks new allocations */
static void scenario_allocator_seal(void) {
    SCENARIO_BEGIN("robust-fault-alloc-001.seal_blocks_alloc");
    asx_runtime_reset();
    reset_fault_state();
    install_fault_hooks();

    /* Seal the allocator */
    IGNORE_RC(asx_runtime_seal_allocator());

    /* Attempt allocation via the runtime — should fail */
    void *ptr = NULL;
    asx_status alloc_rc = asx_runtime_alloc(64, &ptr);
    SCENARIO_CHECK(alloc_rc == ASX_E_ALLOCATOR_SEALED,
                   "alloc should return ALLOCATOR_SEALED after seal");
    SCENARIO_CHECK(ptr == NULL, "ptr should remain NULL after sealed alloc");

    SCENARIO_END();
}

/* robust-fault-alloc-002: task completion survives allocator failure */
static void scenario_allocator_failure_during_run(void) {
    SCENARIO_BEGIN("robust-fault-alloc-002.alloc_fail_during_run");
    asx_runtime_reset();
    reset_fault_state();
    install_fault_hooks();

    /* Allow initial setup but fail later allocations */
    g_alloc_fail_after = 100;

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    asx_status rc = asx_scheduler_run(rid, &budget);
    /* Scheduler should still complete the task (no heap allocs during poll) */
    SCENARIO_CHECK(rc == ASX_OK, "scheduler should complete under alloc pressure");

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task should complete");

    SCENARIO_END();
}

/* robust-fault-trace-001: trace digest stable under fault hooks */
static void scenario_trace_digest_stable_under_faults(void) {
    SCENARIO_BEGIN("robust-fault-trace-001.digest_stable");
    asx_runtime_reset();
    asx_trace_reset();
    reset_fault_state();
    install_fault_hooks();

    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");
    budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    uint64_t digest1 = asx_trace_digest();

    /* Replay with same fault hooks */
    asx_runtime_reset();
    asx_trace_reset();
    reset_fault_state();
    install_fault_hooks();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn_2");
    budget = asx_budget_from_polls(10);
    IGNORE_RC(asx_scheduler_run(rid, &budget));

    uint64_t digest2 = asx_trace_digest();

    SCENARIO_CHECK(digest1 == digest2, "trace digest must be stable across runs");
    SCENARIO_CHECK(digest1 != 0, "trace digest should not be zero");

    printf("DIGEST %016llx\n", (unsigned long long)digest1);

    SCENARIO_END();
}

/* robust-fault-containment-001: poisoned region blocks spawn but allows query */
static void scenario_fault_containment_poison(void) {
    SCENARIO_BEGIN("robust-fault-containment-001.poison_isolation");
    asx_runtime_reset();

    asx_region_id r1, r2;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&r1) == ASX_OK, "region_open_r1");
    SCENARIO_CHECK(asx_region_open(&r2) == ASX_OK, "region_open_r2");

    /* Poison r1 */
    SCENARIO_CHECK(asx_region_poison(r1) == ASX_OK, "poison_r1");

    /* r1: spawn blocked, state query works */
    asx_status rc = asx_task_spawn(r1, poll_complete_fn, NULL, &tid);
    SCENARIO_CHECK(rc == ASX_E_REGION_POISONED, "spawn blocked on poisoned r1");

    asx_region_state rs;
    SCENARIO_CHECK(asx_region_get_state(r1, &rs) == ASX_OK, "state query on poisoned r1");

    /* r2: unaffected by r1's poison */
    SCENARIO_CHECK(asx_task_spawn(r2, poll_complete_fn, NULL, &tid) == ASX_OK,
                   "spawn on healthy r2");

    SCENARIO_END();
}

/* robust-fault-containment-002: scheduler drains existing tasks on poisoned region */
static void scenario_fault_containment_drain_poisoned(void) {
    SCENARIO_BEGIN("robust-fault-containment-002.drain_poisoned");
    asx_runtime_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete_fn, NULL, &tid) == ASX_OK, "task_spawn");

    /* Poison region — existing task should still be schedulable */
    SCENARIO_CHECK(asx_region_poison(rid) == ASX_OK, "poison_region");

    asx_budget budget = asx_budget_from_polls(10);
    asx_status rc = asx_scheduler_run(rid, &budget);
    SCENARIO_CHECK(rc == ASX_OK, "scheduler drains tasks on poisoned region");

    asx_task_state ts;
    SCENARIO_CHECK(asx_task_get_state(tid, &ts) == ASX_OK && ts == ASX_TASK_COMPLETED,
                   "task should complete");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_clock_backward();
    scenario_clock_zero();
    scenario_clock_overflow();
    scenario_entropy_deterministic();
    scenario_entropy_constant();
    scenario_allocator_seal();
    scenario_allocator_failure_during_run();
    scenario_trace_digest_stable_under_faults();
    scenario_fault_containment_poison();
    scenario_fault_containment_drain_poisoned();

    fprintf(stderr, "[e2e] robustness_fault: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
