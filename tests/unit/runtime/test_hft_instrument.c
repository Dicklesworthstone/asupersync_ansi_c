/*
 * test_hft_instrument.c — tests for HFT instrumentation API (bd-j4m.3)
 *
 * Verifies:
 *   - Histogram bin assignment and percentile computation
 *   - Jitter tracker (MAD) computation
 *   - Deterministic overload policy evaluation
 *   - Metric gate pass/fail thresholds
 *   - Global instrumentation state management
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/hft_instrument.h>
#include <string.h>

/* ===================================================================
 * Histogram tests
 * =================================================================== */

TEST(histogram_init_zeroed)
{
    asx_hft_histogram h;
    uint32_t i;

    asx_hft_histogram_init(&h);
    ASSERT_EQ(h.total, 0u);
    ASSERT_EQ(h.overflow, 0u);
    ASSERT_EQ(h.sum_ns, (uint64_t)0);
    ASSERT_EQ(h.max_ns, (uint64_t)0);
    for (i = 0; i < ASX_HFT_HISTOGRAM_BINS; i++) {
        ASSERT_EQ(h.bins[i], 0u);
    }
}

TEST(histogram_single_sample)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    asx_hft_histogram_record(&h, 100);

    ASSERT_EQ(h.total, 1u);
    ASSERT_EQ(h.sum_ns, (uint64_t)100);
    ASSERT_EQ(h.min_ns, (uint64_t)100);
    ASSERT_EQ(h.max_ns, (uint64_t)100);
    ASSERT_EQ(asx_hft_histogram_mean(&h), (uint64_t)100);
}

TEST(histogram_zero_sample)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    asx_hft_histogram_record(&h, 0);

    ASSERT_EQ(h.total, 1u);
    ASSERT_EQ(h.bins[0], 1u);
    ASSERT_EQ(h.min_ns, (uint64_t)0);
}

TEST(histogram_overflow_sample)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    asx_hft_histogram_record(&h, 100000);

    ASSERT_EQ(h.total, 1u);
    ASSERT_EQ(h.overflow, 1u);
    ASSERT_EQ(h.max_ns, (uint64_t)100000);
}

TEST(histogram_bin_assignment)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);

    /* bin 0: [0, 1) — sample 0 maps to bin 0 (log2(0+1) = 0) */
    asx_hft_histogram_record(&h, 0);
    ASSERT_EQ(h.bins[0], 1u);

    /* bin 1: [1, 2) — sample 1 maps to bin 1 (log2(1+1) = 1) */
    asx_hft_histogram_record(&h, 1);
    ASSERT_EQ(h.bins[1], 1u);

    /* bin 2: [2, 4) — sample 3 maps to bin 2 (log2(3+1) = 2) */
    asx_hft_histogram_record(&h, 3);
    ASSERT_EQ(h.bins[2], 1u);

    /* bin 3: [4, 8) — sample 5 maps to bin 2 (log2(5+1) = 2) or bin 3 */
    asx_hft_histogram_record(&h, 7);
    ASSERT_EQ(h.bins[3], 1u);

    ASSERT_EQ(h.total, 4u);
}

TEST(histogram_percentile_uniform)
{
    asx_hft_histogram h;
    uint32_t i;
    uint64_t p50;

    asx_hft_histogram_init(&h);

    /* Record 100 samples at value 50 */
    for (i = 0; i < 100; i++) {
        asx_hft_histogram_record(&h, 50);
    }

    p50 = asx_hft_histogram_percentile(&h, 50);
    /* All samples in same bin, so any percentile returns that bin's lower bound */
    ASSERT_TRUE(p50 <= 64);  /* bin boundary */
}

TEST(histogram_percentile_bimodal)
{
    asx_hft_histogram h;
    uint32_t i;
    uint64_t p50;
    uint64_t p99;

    asx_hft_histogram_init(&h);

    /* 90 samples at value 10 (low latency) */
    for (i = 0; i < 90; i++) {
        asx_hft_histogram_record(&h, 10);
    }
    /* 10 samples at value 10000 (high latency) */
    for (i = 0; i < 10; i++) {
        asx_hft_histogram_record(&h, 10000);
    }

    p50 = asx_hft_histogram_percentile(&h, 50);
    p99 = asx_hft_histogram_percentile(&h, 99);

    /* p50 should be in the low-latency bin */
    ASSERT_TRUE(p50 < 100);
    /* p99 should be in the high-latency bin */
    ASSERT_TRUE(p99 > 100);
}

TEST(histogram_mean_multiple)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    asx_hft_histogram_record(&h, 100);
    asx_hft_histogram_record(&h, 200);
    asx_hft_histogram_record(&h, 300);

    ASSERT_EQ(asx_hft_histogram_mean(&h), (uint64_t)200);
}

TEST(histogram_mean_empty)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    ASSERT_EQ(asx_hft_histogram_mean(&h), (uint64_t)0);
}

TEST(histogram_reset)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    asx_hft_histogram_record(&h, 42);
    ASSERT_EQ(h.total, 1u);

    asx_hft_histogram_reset(&h);
    ASSERT_EQ(h.total, 0u);
    ASSERT_EQ(h.sum_ns, (uint64_t)0);
}

TEST(histogram_min_max_tracked)
{
    asx_hft_histogram h;

    asx_hft_histogram_init(&h);
    asx_hft_histogram_record(&h, 50);
    asx_hft_histogram_record(&h, 10);
    asx_hft_histogram_record(&h, 200);

    ASSERT_EQ(h.min_ns, (uint64_t)10);
    ASSERT_EQ(h.max_ns, (uint64_t)200);
}

/* ===================================================================
 * Jitter tracker tests
 * =================================================================== */

TEST(jitter_init_zero)
{
    asx_hft_jitter_tracker jt;

    asx_hft_jitter_init(&jt, 0);
    ASSERT_EQ(asx_hft_jitter_get(&jt), (uint64_t)0);
    ASSERT_EQ(jt.hist.total, 0u);
}

TEST(jitter_uniform_low)
{
    asx_hft_jitter_tracker jt;
    uint32_t i;

    asx_hft_jitter_init(&jt, 0);  /* recompute every sample */

    /* All same value (0) — bin midpoint equals mean, so MAD = 0 */
    for (i = 0; i < 100; i++) {
        asx_hft_jitter_record(&jt, 0);
    }

    /* All samples in bin 0 (midpoint=0), mean=0, MAD=0 */
    ASSERT_EQ(asx_hft_jitter_get(&jt), (uint64_t)0);
}

TEST(jitter_bimodal_nonzero)
{
    asx_hft_jitter_tracker jt;
    uint32_t i;

    asx_hft_jitter_init(&jt, 0);

    /* Half at 10ns, half at 10000ns — should have nonzero jitter */
    for (i = 0; i < 50; i++) {
        asx_hft_jitter_record(&jt, 10);
    }
    for (i = 0; i < 50; i++) {
        asx_hft_jitter_record(&jt, 10000);
    }

    ASSERT_TRUE(asx_hft_jitter_get(&jt) > 0);
}

TEST(jitter_recompute_interval)
{
    asx_hft_jitter_tracker jt;
    uint32_t i;
    uint64_t jitter_before;

    asx_hft_jitter_init(&jt, 10);  /* recompute every 10 samples */

    /* First 9 samples at value 0: jitter not yet recomputed */
    for (i = 0; i < 9; i++) {
        asx_hft_jitter_record(&jt, 0);
    }
    jitter_before = asx_hft_jitter_get(&jt);
    ASSERT_EQ(jitter_before, (uint64_t)0);  /* not yet triggered */

    /* 10th sample (also 0) triggers recompute */
    asx_hft_jitter_record(&jt, 0);
    /* All samples at 0 (bin 0, midpoint 0), mean 0, MAD 0 */
    ASSERT_EQ(asx_hft_jitter_get(&jt), (uint64_t)0);
}

TEST(jitter_reset)
{
    asx_hft_jitter_tracker jt;

    asx_hft_jitter_init(&jt, 0);
    asx_hft_jitter_record(&jt, 100);
    asx_hft_jitter_record(&jt, 10000);

    asx_hft_jitter_reset(&jt);
    ASSERT_EQ(asx_hft_jitter_get(&jt), (uint64_t)0);
    ASSERT_EQ(jt.hist.total, 0u);
}

/* ===================================================================
 * Overload policy tests
 * =================================================================== */

TEST(overload_default_reject_at_90)
{
    asx_overload_policy pol;

    asx_overload_policy_init(&pol);
    ASSERT_EQ((int)pol.mode, (int)ASX_OVERLOAD_REJECT);
    ASSERT_EQ(pol.threshold_pct, 90u);
}

TEST(overload_below_threshold_admits)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    asx_overload_evaluate(&pol, 50, 100, &dec);

    ASSERT_EQ(dec.triggered, 0);
    ASSERT_EQ((int)dec.admit_status, (int)ASX_OK);
    ASSERT_EQ(dec.load_pct, 50u);
}

TEST(overload_at_threshold_rejects)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    asx_overload_evaluate(&pol, 90, 100, &dec);

    ASSERT_EQ(dec.triggered, 1);
    ASSERT_EQ((int)dec.admit_status, (int)ASX_E_ADMISSION_CLOSED);
    ASSERT_EQ(dec.load_pct, 90u);
}

TEST(overload_above_threshold_rejects)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    asx_overload_evaluate(&pol, 95, 100, &dec);

    ASSERT_EQ(dec.triggered, 1);
    ASSERT_EQ((int)dec.admit_status, (int)ASX_E_ADMISSION_CLOSED);
}

TEST(overload_shed_oldest_reports_count)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    pol.mode = ASX_OVERLOAD_SHED_OLDEST;
    pol.shed_max = 3;
    pol.threshold_pct = 80;

    asx_overload_evaluate(&pol, 85, 100, &dec);

    ASSERT_EQ(dec.triggered, 1);
    ASSERT_EQ((int)dec.mode, (int)ASX_OVERLOAD_SHED_OLDEST);
    ASSERT_EQ(dec.shed_count, 3u);
    ASSERT_EQ((int)dec.admit_status, (int)ASX_OK);
}

TEST(overload_shed_clamped_to_used)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    pol.mode = ASX_OVERLOAD_SHED_OLDEST;
    pol.shed_max = 100;  /* more than used */
    pol.threshold_pct = 50;

    asx_overload_evaluate(&pol, 2, 3, &dec);

    ASSERT_EQ(dec.triggered, 1);
    ASSERT_EQ(dec.shed_count, 2u);  /* clamped to used */
}

TEST(overload_backpressure_would_block)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    pol.mode = ASX_OVERLOAD_BACKPRESSURE;
    pol.threshold_pct = 75;

    asx_overload_evaluate(&pol, 80, 100, &dec);

    ASSERT_EQ(dec.triggered, 1);
    ASSERT_EQ((int)dec.admit_status, (int)ASX_E_WOULD_BLOCK);
}

TEST(overload_zero_capacity)
{
    asx_overload_policy pol;
    asx_overload_decision dec;

    asx_overload_policy_init(&pol);
    asx_overload_evaluate(&pol, 0, 0, &dec);

    ASSERT_EQ(dec.triggered, 1);
    ASSERT_EQ(dec.load_pct, 100u);
    ASSERT_EQ((int)dec.admit_status, (int)ASX_E_RESOURCE_EXHAUSTED);
}

TEST(overload_mode_str)
{
    ASSERT_STR_EQ(asx_overload_mode_str(ASX_OVERLOAD_REJECT), "REJECT");
    ASSERT_STR_EQ(asx_overload_mode_str(ASX_OVERLOAD_SHED_OLDEST), "SHED_OLDEST");
    ASSERT_STR_EQ(asx_overload_mode_str(ASX_OVERLOAD_BACKPRESSURE), "BACKPRESSURE");
    ASSERT_STR_EQ(asx_overload_mode_str((asx_overload_mode)99), "UNKNOWN");
}

TEST(overload_deterministic_replay)
{
    asx_overload_policy pol;
    asx_overload_decision dec1;
    asx_overload_decision dec2;

    asx_overload_policy_init(&pol);

    /* Same inputs must produce identical decisions */
    asx_overload_evaluate(&pol, 92, 100, &dec1);
    asx_overload_evaluate(&pol, 92, 100, &dec2);

    ASSERT_EQ(dec1.triggered, dec2.triggered);
    ASSERT_EQ((int)dec1.admit_status, (int)dec2.admit_status);
    ASSERT_EQ(dec1.load_pct, dec2.load_pct);
    ASSERT_EQ(dec1.shed_count, dec2.shed_count);
}

/* ===================================================================
 * Metric gate tests
 * =================================================================== */

TEST(gate_default_init)
{
    asx_hft_gate gate;

    asx_hft_gate_init(&gate);
    ASSERT_EQ(gate.p99_ns, (uint64_t)10000);
    ASSERT_EQ(gate.p99_9_ns, (uint64_t)0);
    ASSERT_EQ(gate.p99_99_ns, (uint64_t)0);
    ASSERT_EQ(gate.jitter_ns, (uint64_t)0);
}

TEST(gate_passes_low_latency)
{
    asx_hft_gate gate;
    asx_hft_histogram h;
    asx_hft_gate_result result;
    uint32_t i;

    asx_hft_gate_init(&gate);
    asx_hft_histogram_init(&h);

    /* All samples at 100ns — well under 10us p99 threshold */
    for (i = 0; i < 1000; i++) {
        asx_hft_histogram_record(&h, 100);
    }

    asx_hft_gate_evaluate(&gate, &h, NULL, &result);
    ASSERT_EQ(result.pass, 1);
    ASSERT_EQ(result.violations, 0u);
}

TEST(gate_fails_high_p99)
{
    asx_hft_gate gate;
    asx_hft_histogram h;
    asx_hft_gate_result result;
    uint32_t i;

    asx_hft_gate_init(&gate);
    gate.p99_ns = 100;  /* tight threshold */
    asx_hft_histogram_init(&h);

    /* 50% of samples at high latency — p99 will be high */
    for (i = 0; i < 50; i++) {
        asx_hft_histogram_record(&h, 10);
    }
    for (i = 0; i < 50; i++) {
        asx_hft_histogram_record(&h, 20000);
    }

    asx_hft_gate_evaluate(&gate, &h, NULL, &result);
    ASSERT_EQ(result.pass, 0);
    ASSERT_TRUE(result.violations & ASX_GATE_VIOLATION_P99);
}

TEST(gate_jitter_threshold)
{
    asx_hft_gate gate;
    asx_hft_histogram h;
    asx_hft_jitter_tracker jt;
    asx_hft_gate_result result;
    uint32_t i;

    asx_hft_gate_init(&gate);
    gate.jitter_ns = 10;  /* very tight jitter threshold */
    asx_hft_histogram_init(&h);
    asx_hft_jitter_init(&jt, 0);

    /* Bimodal distribution = high jitter */
    for (i = 0; i < 50; i++) {
        asx_hft_jitter_record(&jt, 10);
    }
    for (i = 0; i < 50; i++) {
        asx_hft_jitter_record(&jt, 10000);
    }

    asx_hft_gate_evaluate(&gate, &h, &jt, &result);
    ASSERT_EQ(result.pass, 0);
    ASSERT_TRUE(result.violations & ASX_GATE_VIOLATION_JITTER);
}

TEST(gate_null_hist_and_jt)
{
    asx_hft_gate gate;
    asx_hft_gate_result result;

    asx_hft_gate_init(&gate);
    asx_hft_gate_evaluate(&gate, NULL, NULL, &result);

    /* No data to check = pass */
    ASSERT_EQ(result.pass, 1);
    ASSERT_EQ(result.violations, 0u);
}

/* ===================================================================
 * Global instrumentation tests
 * =================================================================== */

TEST(global_instrument_reset)
{
    asx_hft_histogram *h;
    asx_hft_jitter_tracker *jt;

    asx_hft_instrument_reset();

    h = asx_hft_sched_histogram();
    jt = asx_hft_sched_jitter();

    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(jt != NULL);
    ASSERT_EQ(h->total, 0u);
    ASSERT_EQ(asx_hft_jitter_get(jt), (uint64_t)0);
}

TEST(global_record_poll_latency)
{
    asx_hft_histogram *h;

    asx_hft_instrument_reset();
    asx_hft_record_poll_latency(500);
    asx_hft_record_poll_latency(1000);

    h = asx_hft_sched_histogram();
    ASSERT_EQ(h->total, 2u);
    ASSERT_EQ(h->min_ns, (uint64_t)500);
    ASSERT_EQ(h->max_ns, (uint64_t)1000);
}

TEST(global_record_updates_jitter)
{
    asx_hft_jitter_tracker *jt;
    uint32_t i;

    asx_hft_instrument_reset();

    /* Record enough samples to trigger jitter recompute (every 64) */
    for (i = 0; i < 64; i++) {
        asx_hft_record_poll_latency(0);
    }

    jt = asx_hft_sched_jitter();
    /* All same value (0) in bin 0 = zero MAD */
    ASSERT_EQ(asx_hft_jitter_get(jt), (uint64_t)0);
}

/* ===================================================================
 * Runner
 * =================================================================== */

int main(void)
{
    /* Histogram */
    RUN_TEST(histogram_init_zeroed);
    RUN_TEST(histogram_single_sample);
    RUN_TEST(histogram_zero_sample);
    RUN_TEST(histogram_overflow_sample);
    RUN_TEST(histogram_bin_assignment);
    RUN_TEST(histogram_percentile_uniform);
    RUN_TEST(histogram_percentile_bimodal);
    RUN_TEST(histogram_mean_multiple);
    RUN_TEST(histogram_mean_empty);
    RUN_TEST(histogram_reset);
    RUN_TEST(histogram_min_max_tracked);

    /* Jitter tracker */
    RUN_TEST(jitter_init_zero);
    RUN_TEST(jitter_uniform_low);
    RUN_TEST(jitter_bimodal_nonzero);
    RUN_TEST(jitter_recompute_interval);
    RUN_TEST(jitter_reset);

    /* Overload policy */
    RUN_TEST(overload_default_reject_at_90);
    RUN_TEST(overload_below_threshold_admits);
    RUN_TEST(overload_at_threshold_rejects);
    RUN_TEST(overload_above_threshold_rejects);
    RUN_TEST(overload_shed_oldest_reports_count);
    RUN_TEST(overload_shed_clamped_to_used);
    RUN_TEST(overload_backpressure_would_block);
    RUN_TEST(overload_zero_capacity);
    RUN_TEST(overload_mode_str);
    RUN_TEST(overload_deterministic_replay);

    /* Metric gate */
    RUN_TEST(gate_default_init);
    RUN_TEST(gate_passes_low_latency);
    RUN_TEST(gate_fails_high_p99);
    RUN_TEST(gate_jitter_threshold);
    RUN_TEST(gate_null_hist_and_jt);

    /* Global instrumentation */
    RUN_TEST(global_instrument_reset);
    RUN_TEST(global_record_poll_latency);
    RUN_TEST(global_record_updates_jitter);

    TEST_REPORT();
    return test_failures > 0 ? 1 : 0;
}
