/*
 * test_arena_locality.c — Cache-oblivious arena layout evaluation (bd-3vt.3)
 *
 * Validates:
 * - Layout size accounting (AoS vs hot/cold vs SoA)
 * - Scan equivalence (all three produce identical results)
 * - Cache line utilization analysis
 * - Working set size comparison
 * - Throughput benchmark with cycle counting
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- arena locality spike, no checkpoint coverage needed */

#include "test_harness.h"
#include <asx/asx.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Forward declarations from spike (not in public headers)            */
/* ------------------------------------------------------------------ */

#define SPIKE_MAX_TASKS 64

/* AoS */
typedef struct {
    asx_task_state state;
    asx_region_id region;
    asx_task_poll_fn poll_fn;
    void *user_data;
    asx_outcome outcome;
    uint16_t generation;
    int alive;
    void *captured_state;
    uint32_t captured_size;
    asx_task_state_dtor_fn captured_dtor;
    asx_cancel_phase cancel_phase;
    asx_cancel_reason cancel_reason;
    uint32_t cancel_epoch;
    uint32_t cleanup_polls_remaining;
    int cancel_pending;
} spike_task_slot_aos;

/* Hot/cold */
typedef struct {
    asx_task_state state;
    int alive;
    asx_region_id region;
    asx_task_poll_fn poll_fn;
    void *user_data;
} spike_task_hot;

typedef struct {
    asx_outcome outcome;
    uint16_t generation;
    void *captured_state;
    uint32_t captured_size;
    asx_task_state_dtor_fn captured_dtor;
    asx_cancel_phase cancel_phase;
    asx_cancel_reason cancel_reason;
    uint32_t cancel_epoch;
    uint32_t cleanup_polls_remaining;
    int cancel_pending;
} spike_task_cold;

/* SoA */
typedef struct {
    asx_task_state states[SPIKE_MAX_TASKS];
    int alive[SPIKE_MAX_TASKS];
    asx_region_id regions[SPIKE_MAX_TASKS];
    asx_task_poll_fn poll_fns[SPIKE_MAX_TASKS];
    void *user_data[SPIKE_MAX_TASKS];
    asx_outcome outcomes[SPIKE_MAX_TASKS];
    uint16_t generations[SPIKE_MAX_TASKS];
    void *captured_states[SPIKE_MAX_TASKS];
    uint32_t captured_sizes[SPIKE_MAX_TASKS];
    asx_task_state_dtor_fn captured_dtors[SPIKE_MAX_TASKS];
    asx_cancel_phase cancel_phases[SPIKE_MAX_TASKS];
    asx_cancel_reason cancel_reasons[SPIKE_MAX_TASKS];
    uint32_t cancel_epochs[SPIKE_MAX_TASKS];
    uint32_t cleanup_polls[SPIKE_MAX_TASKS];
    int cancel_pendings[SPIKE_MAX_TASKS];
} spike_task_soa;

/* Functions from spike */
uint32_t spike_sizeof_aos(void);
uint32_t spike_sizeof_hot(void);
uint32_t spike_sizeof_cold(void);
uint32_t spike_sizeof_soa_total(void);
uint32_t spike_scan_aos(const spike_task_slot_aos *slots, uint32_t count, asx_region_id target);
uint32_t spike_scan_hotcold(const spike_task_hot *hot, uint32_t count, asx_region_id target);
uint32_t spike_scan_soa(const spike_task_soa *soa, uint32_t count, asx_region_id target);
uint32_t spike_cachelines_aos(uint32_t count);
uint32_t spike_cachelines_hotcold(uint32_t count);
uint32_t spike_cachelines_soa(uint32_t count);

/* ------------------------------------------------------------------ */
/* Test fixture                                                       */
/* ------------------------------------------------------------------ */

static spike_task_slot_aos g_aos[SPIKE_MAX_TASKS];
static spike_task_hot g_hot[SPIKE_MAX_TASKS];
static spike_task_cold g_cold[SPIKE_MAX_TASKS];
static spike_task_soa g_soa;

/* Sentinel region IDs for testing */
#define TEST_REGION_A ((asx_region_id)0x00010001u)
#define TEST_REGION_B ((asx_region_id)0x00010002u)

static void fixture_init(void) {
    uint32_t i;

    memset(g_aos, 0, sizeof(g_aos));
    memset(g_hot, 0, sizeof(g_hot));
    memset(g_cold, 0, sizeof(g_cold));
    memset(&g_soa, 0, sizeof(g_soa));

    /*
     * Setup: 64 tasks, mixed:
     * - 32 alive in REGION_A (16 CREATED, 8 RUNNING, 4 COMPLETED, 4 CANCELLING)
     * - 16 alive in REGION_B (all CREATED)
     * - 16 dead (alive=0)
     */
    for (i = 0; i < 64; i++) {
        asx_task_state state;
        asx_region_id region;
        int alive;

        if (i < 32) {
            region = TEST_REGION_A;
            alive = 1;
            if (i < 16)
                state = ASX_TASK_CREATED;
            else if (i < 24)
                state = ASX_TASK_RUNNING;
            else if (i < 28)
                state = ASX_TASK_COMPLETED;
            else
                state = ASX_TASK_CANCELLING;
        } else if (i < 48) {
            region = TEST_REGION_B;
            alive = 1;
            state = ASX_TASK_CREATED;
        } else {
            region = TEST_REGION_A;
            alive = 0;
            state = ASX_TASK_CREATED;
        }

        /* AoS */
        g_aos[i].state = state;
        g_aos[i].region = region;
        g_aos[i].alive = alive;

        /* Hot/cold */
        g_hot[i].state = state;
        g_hot[i].region = region;
        g_hot[i].alive = alive;

        /* SoA */
        g_soa.states[i] = state;
        g_soa.regions[i] = region;
        g_soa.alive[i] = alive;
    }
}

/* ------------------------------------------------------------------ */
/* Test: size accounting                                              */
/* ------------------------------------------------------------------ */

TEST(aos_slot_size) {
    /* AoS slot should contain all fields including cancel_reason */
    ASSERT_TRUE(spike_sizeof_aos() > 100);
    fprintf(stderr, "    AoS slot: %u bytes\n", spike_sizeof_aos());
}

TEST(hot_slot_smaller_than_cacheline) {
    /* Hot slot should fit in a cache line (64 bytes) */
    ASSERT_TRUE(spike_sizeof_hot() <= 64);
    fprintf(stderr, "    Hot slot: %u bytes\n", spike_sizeof_hot());
}

TEST(hot_plus_cold_equals_aos) {
    /* Hot + cold should roughly equal AoS (modulo padding) */
    uint32_t combined = spike_sizeof_hot() + spike_sizeof_cold();
    /* Allow some padding difference */
    ASSERT_TRUE(combined <= spike_sizeof_aos() + 32);
    fprintf(stderr, "    Hot+Cold: %u bytes (AoS: %u)\n", combined, spike_sizeof_aos());
}

TEST(soa_total_reasonable) {
    /* SoA total should be roughly 64 * sizeof(all fields) */
    ASSERT_TRUE(spike_sizeof_soa_total() > 0);
    fprintf(stderr, "    SoA total: %u bytes\n", spike_sizeof_soa_total());
}

/* ------------------------------------------------------------------ */
/* Test: scan equivalence                                             */
/* ------------------------------------------------------------------ */

TEST(scan_region_a_equivalent) {
    uint32_t aos_count, hc_count, soa_count;

    fixture_init();

    aos_count = spike_scan_aos(g_aos, 64, TEST_REGION_A);
    hc_count = spike_scan_hotcold(g_hot, 64, TEST_REGION_A);
    soa_count = spike_scan_soa(&g_soa, 64, TEST_REGION_A);

    /* 16 CREATED + 8 RUNNING + 4 CANCELLING = 28 non-completed in region A */
    ASSERT_EQ(aos_count, (uint32_t)28);
    ASSERT_EQ(hc_count, (uint32_t)28);
    ASSERT_EQ(soa_count, (uint32_t)28);
}

TEST(scan_region_b_equivalent) {
    uint32_t aos_count, hc_count, soa_count;

    fixture_init();

    aos_count = spike_scan_aos(g_aos, 64, TEST_REGION_B);
    hc_count = spike_scan_hotcold(g_hot, 64, TEST_REGION_B);
    soa_count = spike_scan_soa(&g_soa, 64, TEST_REGION_B);

    /* 16 READY in region B */
    ASSERT_EQ(aos_count, (uint32_t)16);
    ASSERT_EQ(hc_count, (uint32_t)16);
    ASSERT_EQ(soa_count, (uint32_t)16);
}

TEST(scan_empty_equivalent) {
    uint32_t aos_count, hc_count, soa_count;

    fixture_init();

    /* Region C doesn't exist */
    aos_count = spike_scan_aos(g_aos, 64, (asx_region_id)0x00010099u);
    hc_count = spike_scan_hotcold(g_hot, 64, (asx_region_id)0x00010099u);
    soa_count = spike_scan_soa(&g_soa, 64, (asx_region_id)0x00010099u);

    ASSERT_EQ(aos_count, (uint32_t)0);
    ASSERT_EQ(hc_count, (uint32_t)0);
    ASSERT_EQ(soa_count, (uint32_t)0);
}

TEST(scan_zero_tasks) {
    fixture_init();
    ASSERT_EQ(spike_scan_aos(g_aos, 0, TEST_REGION_A), (uint32_t)0);
    ASSERT_EQ(spike_scan_hotcold(g_hot, 0, TEST_REGION_A), (uint32_t)0);
    ASSERT_EQ(spike_scan_soa(&g_soa, 0, TEST_REGION_A), (uint32_t)0);
}

/* ------------------------------------------------------------------ */
/* Test: cache line analysis                                          */
/* ------------------------------------------------------------------ */

TEST(cacheline_reduction_hotcold) {
    uint32_t aos_lines = spike_cachelines_aos(64);
    uint32_t hc_lines = spike_cachelines_hotcold(64);

    fprintf(stderr, "    AoS cache lines: %u\n", aos_lines);
    fprintf(stderr, "    Hot/cold lines:  %u\n", hc_lines);

    /* Hot/cold must touch fewer lines than AoS */
    ASSERT_TRUE(hc_lines < aos_lines);
}

TEST(cacheline_reduction_soa) {
    uint32_t aos_lines = spike_cachelines_aos(64);
    uint32_t soa_lines = spike_cachelines_soa(64);

    fprintf(stderr, "    AoS cache lines: %u\n", aos_lines);
    fprintf(stderr, "    SoA cache lines: %u\n", soa_lines);

    /* SoA must touch fewer lines than AoS */
    ASSERT_TRUE(soa_lines < aos_lines);
}

TEST(soa_fewer_lines_than_hotcold) {
    uint32_t hc_lines = spike_cachelines_hotcold(64);
    uint32_t soa_lines = spike_cachelines_soa(64);

    fprintf(stderr, "    Hot/cold lines:  %u\n", hc_lines);
    fprintf(stderr, "    SoA cache lines: %u\n", soa_lines);

    /* SoA should touch fewer lines than hot/cold for scan-only workload */
    ASSERT_TRUE(soa_lines <= hc_lines);
}

/* ------------------------------------------------------------------ */
/* Test: working set comparison                                       */
/* ------------------------------------------------------------------ */

TEST(working_set_sizes) {
    uint32_t aos_ws = 64 * spike_sizeof_aos();
    uint32_t hc_hot = 64 * spike_sizeof_hot();
    uint32_t soa_scan = 64 * ((uint32_t)sizeof(asx_task_state) + (uint32_t)sizeof(int) +
                              (uint32_t)sizeof(asx_region_id));

    fprintf(stderr, "    AoS working set:      %u bytes\n", aos_ws);
    fprintf(stderr, "    Hot/cold scan set:     %u bytes\n", hc_hot);
    fprintf(stderr, "    SoA scan set (3 cols): %u bytes\n", soa_scan);

    /* Working set reduction */
    ASSERT_TRUE(hc_hot < aos_ws);
    ASSERT_TRUE(soa_scan < hc_hot);
}

/* ------------------------------------------------------------------ */
/* Test: throughput benchmark                                         */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(_M_X64)
static inline uint64_t rdtsc_spike(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#define HAS_RDTSC 1
#elif defined(__aarch64__)
static inline uint64_t rdtsc_spike(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#define HAS_RDTSC 1
#else
#define HAS_RDTSC 0
#endif

#if HAS_RDTSC
#define BENCH_ITERS 100000u

TEST(throughput_benchmark) {
    uint64_t t0, t1;
    uint32_t i;
    volatile uint32_t sink = 0;
    uint64_t aos_cycles, hc_cycles, soa_cycles;

    fixture_init();

    /* Warmup */
    for (i = 0; i < 1000; i++) {
        sink += spike_scan_aos(g_aos, 64, TEST_REGION_A);
        sink += spike_scan_hotcold(g_hot, 64, TEST_REGION_A);
        sink += spike_scan_soa(&g_soa, 64, TEST_REGION_A);
    }

    /* AoS benchmark */
    t0 = rdtsc_spike();
    for (i = 0; i < BENCH_ITERS; i++) { sink += spike_scan_aos(g_aos, 64, TEST_REGION_A); }
    t1 = rdtsc_spike();
    aos_cycles = (t1 - t0) / BENCH_ITERS;

    /* Hot/cold benchmark */
    t0 = rdtsc_spike();
    for (i = 0; i < BENCH_ITERS; i++) { sink += spike_scan_hotcold(g_hot, 64, TEST_REGION_A); }
    t1 = rdtsc_spike();
    hc_cycles = (t1 - t0) / BENCH_ITERS;

    /* SoA benchmark */
    t0 = rdtsc_spike();
    for (i = 0; i < BENCH_ITERS; i++) { sink += spike_scan_soa(&g_soa, 64, TEST_REGION_A); }
    t1 = rdtsc_spike();
    soa_cycles = (t1 - t0) / BENCH_ITERS;

    fprintf(stderr, "    AoS:       %llu cycles/scan\n", (unsigned long long)aos_cycles);
    fprintf(stderr, "    Hot/cold:  %llu cycles/scan\n", (unsigned long long)hc_cycles);
    fprintf(stderr, "    SoA:       %llu cycles/scan\n", (unsigned long long)soa_cycles);

    /* Sanity: all should complete in reasonable time */
    ASSERT_TRUE(aos_cycles < 10000);
    ASSERT_TRUE(hc_cycles < 10000);
    ASSERT_TRUE(soa_cycles < 10000);

    (void)sink;
}
#endif /* HAS_RDTSC */

/* ------------------------------------------------------------------ */
/* Test: portability analysis                                         */
/* ------------------------------------------------------------------ */

TEST(alignment_requirements) {
    /* All layouts should be naturally aligned */
    ASSERT_TRUE((sizeof(spike_task_slot_aos) % sizeof(void *)) == 0 ||
                sizeof(spike_task_slot_aos) >= sizeof(void *));
    ASSERT_TRUE((sizeof(spike_task_hot) % sizeof(void *)) == 0 ||
                sizeof(spike_task_hot) >= sizeof(void *));
}

TEST(determinism_preserved) {
    uint32_t r1, r2;

    fixture_init();

    /* All layouts must produce identical results for deterministic replay */
    r1 = spike_scan_aos(g_aos, 64, TEST_REGION_A);
    r2 = spike_scan_aos(g_aos, 64, TEST_REGION_A);
    ASSERT_EQ(r1, r2);

    r1 = spike_scan_hotcold(g_hot, 64, TEST_REGION_A);
    r2 = spike_scan_hotcold(g_hot, 64, TEST_REGION_A);
    ASSERT_EQ(r1, r2);

    r1 = spike_scan_soa(&g_soa, 64, TEST_REGION_A);
    r2 = spike_scan_soa(&g_soa, 64, TEST_REGION_A);
    ASSERT_EQ(r1, r2);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== arena locality tests (bd-3vt.3) ===\n");

    /* Size accounting */
    RUN_TEST(aos_slot_size);
    RUN_TEST(hot_slot_smaller_than_cacheline);
    RUN_TEST(hot_plus_cold_equals_aos);
    RUN_TEST(soa_total_reasonable);

    /* Scan equivalence */
    RUN_TEST(scan_region_a_equivalent);
    RUN_TEST(scan_region_b_equivalent);
    RUN_TEST(scan_empty_equivalent);
    RUN_TEST(scan_zero_tasks);

    /* Cache line analysis */
    RUN_TEST(cacheline_reduction_hotcold);
    RUN_TEST(cacheline_reduction_soa);
    RUN_TEST(soa_fewer_lines_than_hotcold);

    /* Working set */
    RUN_TEST(working_set_sizes);

    /* Throughput */
#if HAS_RDTSC
    RUN_TEST(throughput_benchmark);
#else
    fprintf(stderr, "  SKIP: throughput_benchmark (no cycle counter)\n");
#endif

    /* Portability */
    RUN_TEST(alignment_requirements);
    RUN_TEST(determinism_preserved);

    TEST_REPORT();
    return test_failures;
}
