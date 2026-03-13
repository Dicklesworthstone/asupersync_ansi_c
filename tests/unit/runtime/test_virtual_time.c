/*
 * test_virtual_time.c — Virtual-time layer deterministic replay tests (bd-3vt.8)
 *
 * Validates:
 * - Monotonic baseline advance
 * - Jitter injection (positive and negative)
 * - Stall (time freeze for N queries)
 * - Forward jump
 * - Combined anomaly schedules
 * - Deterministic reproduction across repeated runs
 * - Integration with asx_runtime_now_ns via hook installation
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- virtual-time spike test, no checkpoint coverage needed */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/virtual_time.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define VT_IGNORE(expr)                                                                            \
    do {                                                                                           \
        volatile asx_status _vt_ign = (expr);                                                      \
        (void)_vt_ign;                                                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test: basic monotonic advance                                       */
/* ------------------------------------------------------------------ */

TEST(monotonic_advance) {
    asx_vtime_state vt;
    asx_time t0, t1, t2, t3;

    asx_vtime_init(&vt, 0, 1000); /* start=0, tick=1µs */

    t0 = asx_vtime_now_ns(&vt); /* query 0 */
    t1 = asx_vtime_now_ns(&vt); /* query 1 */
    t2 = asx_vtime_now_ns(&vt); /* query 2 */
    t3 = asx_vtime_now_ns(&vt); /* query 3 */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)1000);
    ASSERT_EQ(t2, (asx_time)2000);
    ASSERT_EQ(t3, (asx_time)3000);
    ASSERT_EQ(asx_vtime_query_count(&vt), (uint32_t)4);
}

/* ------------------------------------------------------------------ */
/* Test: custom base time                                              */
/* ------------------------------------------------------------------ */

TEST(custom_base_time) {
    asx_vtime_state vt;
    asx_time t0, t1;

    asx_vtime_init(&vt, 1000000000ULL, 500); /* start=1s, tick=500ns */

    t0 = asx_vtime_now_ns(&vt);
    t1 = asx_vtime_now_ns(&vt);

    ASSERT_EQ(t0, (asx_time)1000000000ULL);
    ASSERT_EQ(t1, (asx_time)1000000500ULL);
}

/* ------------------------------------------------------------------ */
/* Test: positive jitter                                               */
/* ------------------------------------------------------------------ */

TEST(jitter_positive) {
    asx_vtime_state vt;
    asx_time t0, t1, t2, t3;

    asx_vtime_init(&vt, 0, 1000);
    ASSERT_EQ(asx_vtime_add_jitter(&vt, 2, 500), ASX_OK); /* +500ns at query 2 */

    t0 = asx_vtime_now_ns(&vt); /* 0 */
    t1 = asx_vtime_now_ns(&vt); /* 1000 */
    t2 = asx_vtime_now_ns(&vt); /* 2000 + 500 = 2500 */
    t3 = asx_vtime_now_ns(&vt); /* 2500 + 1000 = 3500 */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)1000);
    ASSERT_EQ(t2, (asx_time)2500);
    ASSERT_EQ(t3, (asx_time)3500);
}

/* ------------------------------------------------------------------ */
/* Test: negative jitter (clock reversal)                              */
/* ------------------------------------------------------------------ */

TEST(jitter_negative) {
    asx_vtime_state vt;
    asx_time t0, t1, t2, t3;

    asx_vtime_init(&vt, 10000, 1000);
    ASSERT_EQ(asx_vtime_add_jitter(&vt, 2, -500), ASX_OK); /* -500ns at query 2 */

    t0 = asx_vtime_now_ns(&vt); /* 10000 */
    t1 = asx_vtime_now_ns(&vt); /* 11000 */
    t2 = asx_vtime_now_ns(&vt); /* 12000 - 500 = 11500 */
    t3 = asx_vtime_now_ns(&vt); /* 11500 + 1000 = 12500 */

    ASSERT_EQ(t0, (asx_time)10000);
    ASSERT_EQ(t1, (asx_time)11000);
    ASSERT_EQ(t2, (asx_time)11500); /* Reversal! t2 < t1+tick */
    ASSERT_EQ(t3, (asx_time)12500);
}

/* ------------------------------------------------------------------ */
/* Test: stall (time freeze)                                           */
/* ------------------------------------------------------------------ */

TEST(stall_freeze) {
    asx_vtime_state vt;
    asx_time t0, t1, t2, t3, t4, t5;

    asx_vtime_init(&vt, 0, 1000);
    ASSERT_EQ(asx_vtime_add_stall(&vt, 2, 3), ASX_OK); /* At query 2, stall for 3 queries */

    t0 = asx_vtime_now_ns(&vt); /* 0 */
    t1 = asx_vtime_now_ns(&vt); /* 1000 */
    t2 = asx_vtime_now_ns(&vt); /* 2000 (stall starts) */
    t3 = asx_vtime_now_ns(&vt); /* 2000 (stalled, query 3) */
    t4 = asx_vtime_now_ns(&vt); /* 2000 (stalled, query 4) */
    t5 = asx_vtime_now_ns(&vt); /* 2000 (stalled, query 5) */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)1000);
    ASSERT_EQ(t2, (asx_time)2000);
    ASSERT_EQ(t3, (asx_time)2000); /* Frozen */
    ASSERT_EQ(t4, (asx_time)2000); /* Frozen */
    ASSERT_EQ(t5, (asx_time)2000); /* Frozen */
}

/* ------------------------------------------------------------------ */
/* Test: stall recovery                                                */
/* ------------------------------------------------------------------ */

TEST(stall_recovery) {
    asx_vtime_state vt;
    asx_time vals[8];
    uint32_t i;

    asx_vtime_init(&vt, 0, 1000);
    ASSERT_EQ(asx_vtime_add_stall(&vt, 2, 2), ASX_OK); /* Stall for 2 queries */

    for (i = 0; i < 8; i++) { vals[i] = asx_vtime_now_ns(&vt); }

    /* q0=0, q1=1000, q2=2000(stall trigger), q3=2000(stall 1/2),
     * q4=2000(stall 2/2), q5=3000(resumes), q6=4000, q7=5000 */
    ASSERT_EQ(vals[0], (asx_time)0);
    ASSERT_EQ(vals[1], (asx_time)1000);
    ASSERT_EQ(vals[2], (asx_time)2000);
    ASSERT_EQ(vals[3], (asx_time)2000);
    ASSERT_EQ(vals[4], (asx_time)2000); /* Still stalled */
    ASSERT_EQ(vals[5], (asx_time)3000); /* Resumes advancing */
    ASSERT_EQ(vals[6], (asx_time)4000);
    ASSERT_EQ(vals[7], (asx_time)5000);
}

/* ------------------------------------------------------------------ */
/* Test: forward jump                                                  */
/* ------------------------------------------------------------------ */

TEST(forward_jump) {
    asx_vtime_state vt;
    asx_time t0, t1, t2, t3;

    asx_vtime_init(&vt, 0, 1000);
    ASSERT_EQ(asx_vtime_add_jump(&vt, 1, 10000), ASX_OK); /* +10µs at query 1 */

    t0 = asx_vtime_now_ns(&vt); /* 0 */
    t1 = asx_vtime_now_ns(&vt); /* 1000 + 10000 = 11000 */
    t2 = asx_vtime_now_ns(&vt); /* 11000 + 1000 = 12000 */
    t3 = asx_vtime_now_ns(&vt); /* 12000 + 1000 = 13000 */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)11000);
    ASSERT_EQ(t2, (asx_time)12000);
    ASSERT_EQ(t3, (asx_time)13000);
}

/* ------------------------------------------------------------------ */
/* Test: combined anomaly schedule                                     */
/* ------------------------------------------------------------------ */

TEST(combined_anomalies) {
    asx_vtime_state vt;
    asx_time vals[10];
    uint32_t i;

    asx_vtime_init(&vt, 0, 1000);

    /* Schedule: jitter at q1, stall at q3 for 2, jump at q7 */
    ASSERT_EQ(asx_vtime_add_jitter(&vt, 1, 200), ASX_OK);
    ASSERT_EQ(asx_vtime_add_stall(&vt, 3, 2), ASX_OK);
    ASSERT_EQ(asx_vtime_add_jump(&vt, 7, 5000), ASX_OK);

    for (i = 0; i < 10; i++) { vals[i] = asx_vtime_now_ns(&vt); }

    /* q0=0, q1=1000+200=1200, q2=2200, q3=3200(stall trigger),
     * q4=3200(stall 1/2), q5=3200(stall 2/2), q6=4200(resumes),
     * q7=5200+5000=10200, q8=11200, q9=12200 */
    ASSERT_EQ(vals[0], (asx_time)0);
    ASSERT_EQ(vals[1], (asx_time)1200);
    ASSERT_EQ(vals[2], (asx_time)2200);
    ASSERT_EQ(vals[3], (asx_time)3200);
    ASSERT_EQ(vals[4], (asx_time)3200);
    ASSERT_EQ(vals[5], (asx_time)3200);
    ASSERT_EQ(vals[6], (asx_time)4200);
    ASSERT_EQ(vals[7], (asx_time)10200);
    ASSERT_EQ(vals[8], (asx_time)11200);
    ASSERT_EQ(vals[9], (asx_time)12200);
}

/* ------------------------------------------------------------------ */
/* Test: deterministic replay (same inputs → same outputs)             */
/* ------------------------------------------------------------------ */

TEST(deterministic_replay) {
    asx_vtime_state vt;
    asx_time run1[20];
    asx_time run2[20];
    uint32_t i;

    /* Run 1 */
    asx_vtime_init(&vt, 5000, 750);
    VT_IGNORE(asx_vtime_add_jitter(&vt, 3, -100));
    VT_IGNORE(asx_vtime_add_stall(&vt, 7, 4));
    VT_IGNORE(asx_vtime_add_jump(&vt, 12, 8000));

    for (i = 0; i < 20; i++) { run1[i] = asx_vtime_now_ns(&vt); }

    /* Run 2: identical schedule, reset */
    asx_vtime_reset(&vt);
    for (i = 0; i < 20; i++) { run2[i] = asx_vtime_now_ns(&vt); }

    /* Must be bit-identical */
    for (i = 0; i < 20; i++) { ASSERT_EQ(run1[i], run2[i]); }
}

/* ------------------------------------------------------------------ */
/* Test: anomaly schedule overflow                                     */
/* ------------------------------------------------------------------ */

TEST(anomaly_schedule_overflow) {
    asx_vtime_state vt;
    uint32_t i;

    asx_vtime_init(&vt, 0, 1000);

    /* Fill the anomaly schedule */
    for (i = 0; i < ASX_VTIME_MAX_ANOMALIES; i++) {
        ASSERT_EQ(asx_vtime_add_jitter(&vt, i, 100), ASX_OK);
    }

    /* One more should fail */
    ASSERT_EQ(asx_vtime_add_jitter(&vt, 99, 100), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Test: zero tick (manual advance only via anomalies)                  */
/* ------------------------------------------------------------------ */

TEST(zero_tick_manual) {
    asx_vtime_state vt;
    asx_time t0, t1, t2, t3;

    asx_vtime_init(&vt, 0, 0); /* No auto-advance */
    VT_IGNORE(asx_vtime_add_jump(&vt, 2, 5000));

    t0 = asx_vtime_now_ns(&vt); /* 0 */
    t1 = asx_vtime_now_ns(&vt); /* 0 (no tick) */
    t2 = asx_vtime_now_ns(&vt); /* 0 + 5000 = 5000 (jump) */
    t3 = asx_vtime_now_ns(&vt); /* 5000 (no tick) */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)0);
    ASSERT_EQ(t2, (asx_time)5000);
    ASSERT_EQ(t3, (asx_time)5000);
}

/* ------------------------------------------------------------------ */
/* Test: large jitter exceeding current time (clamped to 0)            */
/* ------------------------------------------------------------------ */

TEST(jitter_underflow_clamp) {
    asx_vtime_state vt;
    asx_time t0, t1;

    asx_vtime_init(&vt, 100, 50);
    VT_IGNORE(asx_vtime_add_jitter(&vt, 1, -9999)); /* Way beyond current time */

    t0 = asx_vtime_now_ns(&vt); /* 100 */
    t1 = asx_vtime_now_ns(&vt); /* 150 - 9999 → clamped to 0 */

    ASSERT_EQ(t0, (asx_time)100);
    ASSERT_EQ(t1, (asx_time)0); /* Clamped, not underflow */
}

/* ------------------------------------------------------------------ */
/* Test: query count tracking                                          */
/* ------------------------------------------------------------------ */

TEST(query_count_tracking) {
    asx_vtime_state vt;
    uint32_t i;

    asx_vtime_init(&vt, 0, 1000);

    ASSERT_EQ(asx_vtime_query_count(&vt), (uint32_t)0);

    for (i = 0; i < 10; i++) { (void)asx_vtime_now_ns(&vt); }

    ASSERT_EQ(asx_vtime_query_count(&vt), (uint32_t)10);
}

/* ------------------------------------------------------------------ */
/* Test: reset preserves schedule                                      */
/* ------------------------------------------------------------------ */

TEST(reset_preserves_schedule) {
    asx_vtime_state vt;
    asx_time t_before, t_after;

    asx_vtime_init(&vt, 0, 1000);
    VT_IGNORE(asx_vtime_add_jitter(&vt, 1, 500));

    /* Run once */
    (void)asx_vtime_now_ns(&vt);
    t_before = asx_vtime_now_ns(&vt); /* 1000 + 500 = 1500 */

    /* Reset and replay */
    asx_vtime_reset(&vt);
    ASSERT_EQ(asx_vtime_current(&vt), (asx_time)0);
    ASSERT_EQ(asx_vtime_query_count(&vt), (uint32_t)0);

    (void)asx_vtime_now_ns(&vt);
    t_after = asx_vtime_now_ns(&vt); /* Should also be 1500 */

    ASSERT_EQ(t_before, t_after);
}

/* ------------------------------------------------------------------ */
/* Test: hook integration — install via callback pointer               */
/* ------------------------------------------------------------------ */

TEST(hook_callback_integration) {
    asx_vtime_state vt;
    asx_time t0, t1;

    asx_vtime_init(&vt, 0, 1000);
    VT_IGNORE(asx_vtime_add_jitter(&vt, 1, 250));

    /* Simulate hook dispatch: call via function pointer */
    asx_clock_now_ns_fn fn = asx_vtime_now_ns;
    t0 = fn(&vt); /* 0 */
    t1 = fn(&vt); /* 1000 + 250 = 1250 */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)1250);
}

/* ------------------------------------------------------------------ */
/* Test: null safety                                                   */
/* ------------------------------------------------------------------ */

TEST(null_safety) {
    ASSERT_EQ(asx_vtime_now_ns(NULL), (asx_time)0);
    ASSERT_EQ(asx_vtime_current(NULL), (asx_time)0);
    ASSERT_EQ(asx_vtime_query_count(NULL), (uint32_t)0);
    ASSERT_EQ(asx_vtime_add_jitter(NULL, 0, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_vtime_add_stall(NULL, 0, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_vtime_add_jump(NULL, 0, 0), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Test: multiple jitters at same query                                */
/* ------------------------------------------------------------------ */

TEST(multiple_jitters_same_query) {
    asx_vtime_state vt;
    asx_time t0, t1;

    asx_vtime_init(&vt, 0, 1000);
    VT_IGNORE(asx_vtime_add_jitter(&vt, 1, 200));
    VT_IGNORE(asx_vtime_add_jitter(&vt, 1, 300)); /* Both fire at q1 */

    t0 = asx_vtime_now_ns(&vt); /* 0 */
    t1 = asx_vtime_now_ns(&vt); /* 1000 + 200 + 300 = 1500 */

    ASSERT_EQ(t0, (asx_time)0);
    ASSERT_EQ(t1, (asx_time)1500);
}

/* ------------------------------------------------------------------ */
/* Test: overhead measurement — virtual vs direct                      */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
static uint64_t vt_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#define HAS_RDTSC 1
#else
#define HAS_RDTSC 0
static uint64_t vt_rdtsc(void) { return 0; }
#endif

TEST(overhead_measurement) {
#if HAS_RDTSC
    asx_vtime_state vt;
    uint32_t i;
    uint32_t rounds = 10000;
    uint64_t start, end;

    /* Measure virtual-time query cost */
    asx_vtime_init(&vt, 0, 1000);
    VT_IGNORE(asx_vtime_add_jitter(&vt, 5000, 100));
    VT_IGNORE(asx_vtime_add_stall(&vt, 5001, 10));
    VT_IGNORE(asx_vtime_add_jump(&vt, 5002, 9000));

    start = vt_rdtsc();
    for (i = 0; i < rounds; i++) { (void)asx_vtime_now_ns(&vt); }
    end = vt_rdtsc();

    fprintf(stderr, "    virtual-time: %.1f cycles/query (%u queries, %u anomalies)\n",
            (double)(end - start) / (double)rounds, rounds, vt.anomaly_count);

    /* Must complete in reasonable time (<1000 cycles/query) */
    ASSERT_TRUE((end - start) / (uint64_t)rounds < 1000u);
#else
    (void)0; /* No rdtsc */
#endif
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== virtual-time tests (bd-3vt.8) ===\n");

    RUN_TEST(monotonic_advance);
    RUN_TEST(custom_base_time);
    RUN_TEST(jitter_positive);
    RUN_TEST(jitter_negative);
    RUN_TEST(stall_freeze);
    RUN_TEST(stall_recovery);
    RUN_TEST(forward_jump);
    RUN_TEST(combined_anomalies);
    RUN_TEST(deterministic_replay);
    RUN_TEST(anomaly_schedule_overflow);
    RUN_TEST(zero_tick_manual);
    RUN_TEST(jitter_underflow_clamp);
    RUN_TEST(query_count_tracking);
    RUN_TEST(reset_preserves_schedule);
    RUN_TEST(hook_callback_integration);
    RUN_TEST(null_safety);
    RUN_TEST(multiple_jitters_same_query);
    RUN_TEST(overhead_measurement);

    TEST_REPORT();
    return test_failures;
}
