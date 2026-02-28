/*
 * test_adapter.c — tests for vertical acceleration adapters (bd-j4m.5)
 *
 * Tests cover:
 *   1. HFT adapter: accelerated and fallback paths
 *   2. Automotive adapter: accelerated (with deadline), fallback
 *   3. Router adapter: accelerated (with resource class), fallback
 *   4. Unified dispatch for all domains and modes
 *   5. Isomorphism proof: single point and sweep
 *   6. Edge cases: zero capacity, boundary loads
 *   7. Diagnostics: string functions, version
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() — bounded iteration over small enums and fixed sweeps */

#include "test_harness.h"
#include <asx/runtime/adapter.h>
#include <string.h>

/* ===================================================================
 * HFT adapter tests
 * =================================================================== */

TEST(hft_accel_below_threshold_admits)
{
    asx_adapter_decision d;
    asx_adapter_hft_decide(80, 100, &d);
    ASSERT_EQ(d.triggered, 0);
    ASSERT_EQ(d.admit_status, ASX_OK);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_ACCELERATED);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_SHED_OLDEST);
}

TEST(hft_accel_at_threshold_triggers)
{
    asx_adapter_decision d;
    asx_adapter_hft_decide(85, 100, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_EQ(d.admit_status, ASX_OK); /* SHED admits after eviction */
    ASSERT_TRUE(d.shed_count > 0);
    ASSERT_TRUE(d.shed_count <= 2);
}

TEST(hft_accel_above_threshold_sheds)
{
    asx_adapter_decision d;
    asx_adapter_hft_decide(95, 100, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_EQ(d.shed_count, 2);
}

TEST(hft_accel_zero_capacity)
{
    asx_adapter_decision d;
    asx_adapter_hft_decide(5, 0, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_EQ(d.load_pct, 100);
}

TEST(hft_fallback_below_threshold_admits)
{
    asx_adapter_decision d;
    asx_adapter_hft_fallback(85, 100, &d);
    ASSERT_EQ(d.triggered, 0);
    ASSERT_EQ(d.admit_status, ASX_OK);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_FALLBACK);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_REJECT);
}

TEST(hft_fallback_at_threshold_rejects)
{
    asx_adapter_decision d;
    asx_adapter_hft_fallback(90, 100, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_NE(d.admit_status, ASX_OK);
}

TEST(hft_fallback_zero_capacity)
{
    asx_adapter_decision d;
    asx_adapter_hft_fallback(0, 0, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_EQ(d.load_pct, 100);
}

/* ===================================================================
 * Automotive adapter tests
 * =================================================================== */

TEST(auto_accel_below_threshold_admits)
{
    asx_adapter_decision d;
    asx_adapter_auto_decide(80, 100, NULL, &d);
    ASSERT_EQ(d.triggered, 0);
    ASSERT_EQ(d.admit_status, ASX_OK);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_ACCELERATED);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_BACKPRESSURE);
}

TEST(auto_accel_at_threshold_blocks)
{
    asx_adapter_decision d;
    asx_adapter_auto_decide(90, 100, NULL, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_EQ(d.admit_status, ASX_E_WOULD_BLOCK);
}

TEST(auto_accel_deadline_escalation)
{
    /* Create a deadline tracker with high miss rate */
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);
    /* Record 10 misses */
    {
        int i;
        for (i = 0; i < 10; i++) {
            asx_auto_deadline_record(&dt, 100, 200);
        }
    }
    /* Miss rate should be 100%, load at 82% should trigger */
    {
        asx_adapter_decision d;
        asx_adapter_auto_decide(82, 100, &dt, &d);
        ASSERT_EQ(d.triggered, 1);
    }
}

TEST(auto_accel_no_escalation_low_miss_rate)
{
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);
    /* Record 10 hits */
    {
        int i;
        for (i = 0; i < 10; i++) {
            asx_auto_deadline_record(&dt, 200, 100);
        }
    }
    /* Miss rate is 0%, load at 82% should NOT trigger */
    {
        asx_adapter_decision d;
        asx_adapter_auto_decide(82, 100, &dt, &d);
        ASSERT_EQ(d.triggered, 0);
    }
}

TEST(auto_accel_zero_capacity)
{
    asx_adapter_decision d;
    asx_adapter_auto_decide(1, 0, NULL, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_EQ(d.admit_status, ASX_E_WOULD_BLOCK);
}

TEST(auto_fallback_below_threshold_admits)
{
    asx_adapter_decision d;
    asx_adapter_auto_fallback(85, 100, &d);
    ASSERT_EQ(d.triggered, 0);
    ASSERT_EQ(d.admit_status, ASX_OK);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_FALLBACK);
}

TEST(auto_fallback_at_threshold_rejects)
{
    asx_adapter_decision d;
    asx_adapter_auto_fallback(90, 100, &d);
    ASSERT_EQ(d.triggered, 1);
}

/* ===================================================================
 * Router adapter tests
 * =================================================================== */

TEST(router_accel_below_threshold_admits)
{
    asx_adapter_decision d;
    asx_adapter_router_decide(70, 100, ASX_CLASS_R2, &d);
    ASSERT_EQ(d.triggered, 0);
    ASSERT_EQ(d.admit_status, ASX_OK);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_ACCELERATED);
}

TEST(router_accel_at_threshold_rejects)
{
    asx_adapter_decision d;
    asx_adapter_router_decide(75, 100, ASX_CLASS_R2, &d);
    ASSERT_EQ(d.triggered, 1);
    ASSERT_NE(d.admit_status, ASX_OK);
}

TEST(router_accel_r1_scales_down)
{
    /* R1 halves capacity: 100 -> 50. Load 40 -> 80% >= 75% threshold */
    asx_adapter_decision d;
    asx_adapter_router_decide(40, 100, ASX_CLASS_R1, &d);
    ASSERT_EQ(d.triggered, 1);
}

TEST(router_accel_r3_scales_up)
{
    /* R3 doubles capacity: 100 -> 200, but safety guard stays fail-closed
     * relative to CORE fallback (140/100 => 140% >= 90% => must trigger). */
    asx_adapter_decision d;
    asx_adapter_router_decide(140, 100, ASX_CLASS_R3, &d);
    ASSERT_EQ(d.triggered, 1);
}

TEST(router_accel_zero_capacity)
{
    asx_adapter_decision d;
    asx_adapter_router_decide(1, 0, ASX_CLASS_R2, &d);
    ASSERT_EQ(d.triggered, 1);
}

TEST(router_fallback_below_threshold)
{
    asx_adapter_decision d;
    asx_adapter_router_fallback(85, 100, &d);
    ASSERT_EQ(d.triggered, 0);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_FALLBACK);
}

TEST(router_fallback_at_threshold)
{
    asx_adapter_decision d;
    asx_adapter_router_fallback(90, 100, &d);
    ASSERT_EQ(d.triggered, 1);
}

/* ===================================================================
 * Unified dispatch tests
 * =================================================================== */

TEST(dispatch_hft_accelerated)
{
    asx_adapter_decision d;
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_HFT, ASX_ADAPTER_ACCELERATED,
                         50, 100, NULL, &d);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_ACCELERATED);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_SHED_OLDEST);
}

TEST(dispatch_hft_fallback)
{
    asx_adapter_decision d;
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_HFT, ASX_ADAPTER_FALLBACK,
                         50, 100, NULL, &d);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_FALLBACK);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_REJECT);
}

TEST(dispatch_auto_accelerated)
{
    asx_adapter_decision d;
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_AUTOMOTIVE, ASX_ADAPTER_ACCELERATED,
                         50, 100, NULL, &d);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_ACCELERATED);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_BACKPRESSURE);
}

TEST(dispatch_router_accelerated)
{
    asx_resource_class rc = ASX_CLASS_R2;
    asx_adapter_decision d;
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_ROUTER, ASX_ADAPTER_ACCELERATED,
                         50, 100, &rc, &d);
    ASSERT_EQ((int)d.path_used, (int)ASX_ADAPTER_ACCELERATED);
    ASSERT_EQ((int)d.mode, (int)ASX_OVERLOAD_REJECT);
}

TEST(dispatch_all_fallback_same_behavior)
{
    /* All domains in fallback mode should produce identical results */
    asx_adapter_decision d_hft, d_auto, d_router;
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_HFT, ASX_ADAPTER_FALLBACK,
                         50, 100, NULL, &d_hft);
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_AUTOMOTIVE, ASX_ADAPTER_FALLBACK,
                         50, 100, NULL, &d_auto);
    asx_adapter_dispatch(ASX_ADAPTER_DOMAIN_ROUTER, ASX_ADAPTER_FALLBACK,
                         50, 100, NULL, &d_router);

    ASSERT_EQ(d_hft.triggered, d_auto.triggered);
    ASSERT_EQ(d_auto.triggered, d_router.triggered);
    ASSERT_EQ(d_hft.load_pct, d_auto.load_pct);
    ASSERT_EQ(d_auto.load_pct, d_router.load_pct);
}

/* ===================================================================
 * Isomorphism proof tests
 * =================================================================== */

TEST(iso_hft_single_low_load)
{
    asx_adapter_isomorphism proof;
    asx_adapter_prove_isomorphism(ASX_ADAPTER_DOMAIN_HFT,
                                  50, 100, NULL, &proof);
    ASSERT_EQ(proof.pass, 1);
    ASSERT_EQ(proof.accel_decision.triggered, 0);
    ASSERT_EQ(proof.fallback_decision.triggered, 0);
}

TEST(iso_hft_single_high_load)
{
    asx_adapter_isomorphism proof;
    asx_adapter_prove_isomorphism(ASX_ADAPTER_DOMAIN_HFT,
                                  95, 100, NULL, &proof);
    /* Both should trigger (HFT at 85%, CORE at 90%) */
    ASSERT_EQ(proof.pass, 1);
    ASSERT_EQ(proof.accel_decision.triggered, 1);
    ASSERT_EQ(proof.fallback_decision.triggered, 1);
}

TEST(iso_hft_gray_zone)
{
    /* Load at 87%: HFT triggers (>=85%) but CORE doesn't (>=90%) */
    asx_adapter_isomorphism proof;
    asx_adapter_prove_isomorphism(ASX_ADAPTER_DOMAIN_HFT,
                                  87, 100, NULL, &proof);
    /* Accel is stricter, which is safe — proof should pass */
    ASSERT_EQ(proof.pass, 1);
    ASSERT_EQ(proof.accel_decision.triggered, 1);
    ASSERT_EQ(proof.fallback_decision.triggered, 0);
}

TEST(iso_hft_sweep_passes)
{
    int result = asx_adapter_prove_isomorphism_sweep(
        ASX_ADAPTER_DOMAIN_HFT, 100, NULL, NULL);
    ASSERT_EQ(result, 1);
}

TEST(iso_auto_sweep_passes)
{
    int result = asx_adapter_prove_isomorphism_sweep(
        ASX_ADAPTER_DOMAIN_AUTOMOTIVE, 100, NULL, NULL);
    ASSERT_EQ(result, 1);
}

TEST(iso_router_sweep_passes)
{
    asx_resource_class rc = ASX_CLASS_R2;
    int result = asx_adapter_prove_isomorphism_sweep(
        ASX_ADAPTER_DOMAIN_ROUTER, 100, &rc, NULL);
    ASSERT_EQ(result, 1);
}

TEST(iso_router_r1_sweep_passes)
{
    asx_resource_class rc = ASX_CLASS_R1;
    int result = asx_adapter_prove_isomorphism_sweep(
        ASX_ADAPTER_DOMAIN_ROUTER, 100, &rc, NULL);
    ASSERT_EQ(result, 1);
}

TEST(iso_router_r3_sweep_passes)
{
    asx_resource_class rc = ASX_CLASS_R3;
    int result = asx_adapter_prove_isomorphism_sweep(
        ASX_ADAPTER_DOMAIN_ROUTER, 100, &rc, NULL);
    ASSERT_EQ(result, 1);
}

TEST(iso_zero_capacity_all_domains)
{
    int i;
    for (i = 0; i < (int)ASX_ADAPTER_DOMAIN_COUNT; i++) {
        asx_adapter_isomorphism proof;
        asx_adapter_prove_isomorphism((asx_adapter_domain)i,
                                      0, 0, NULL, &proof);
        ASSERT_EQ(proof.pass, 1);
        ASSERT_EQ(proof.accel_decision.triggered, 1);
        ASSERT_EQ(proof.fallback_decision.triggered, 1);
    }
}

TEST(iso_decision_hash_deterministic)
{
    asx_adapter_isomorphism proof1, proof2;
    asx_adapter_prove_isomorphism(ASX_ADAPTER_DOMAIN_HFT,
                                  50, 100, NULL, &proof1);
    asx_adapter_prove_isomorphism(ASX_ADAPTER_DOMAIN_HFT,
                                  50, 100, NULL, &proof2);
    ASSERT_EQ(proof1.accel_hash, proof2.accel_hash);
    ASSERT_EQ(proof1.fallback_hash, proof2.fallback_hash);
}

/* ===================================================================
 * Diagnostics tests
 * =================================================================== */

TEST(domain_str_all_valid)
{
    int i;
    for (i = 0; i < (int)ASX_ADAPTER_DOMAIN_COUNT; i++) {
        const char *name = asx_adapter_domain_str((asx_adapter_domain)i);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(name[0] != '\0');
    }
}

TEST(mode_str_all_valid)
{
    const char *fb = asx_adapter_mode_str(ASX_ADAPTER_FALLBACK);
    const char *ac = asx_adapter_mode_str(ASX_ADAPTER_ACCELERATED);
    ASSERT_TRUE(fb != NULL);
    ASSERT_TRUE(ac != NULL);
    ASSERT_TRUE(fb[0] != '\0');
    ASSERT_TRUE(ac[0] != '\0');
}

TEST(adapter_version_nonzero)
{
    ASSERT_TRUE(asx_adapter_version() > 0);
    ASSERT_EQ(asx_adapter_version(), ASX_ADAPTER_VERSION);
}

/* ===================================================================
 * Test runner
 * =================================================================== */

int main(void)
{
    /* HFT adapter */
    RUN_TEST(hft_accel_below_threshold_admits);
    RUN_TEST(hft_accel_at_threshold_triggers);
    RUN_TEST(hft_accel_above_threshold_sheds);
    RUN_TEST(hft_accel_zero_capacity);
    RUN_TEST(hft_fallback_below_threshold_admits);
    RUN_TEST(hft_fallback_at_threshold_rejects);
    RUN_TEST(hft_fallback_zero_capacity);

    /* Automotive adapter */
    RUN_TEST(auto_accel_below_threshold_admits);
    RUN_TEST(auto_accel_at_threshold_blocks);
    RUN_TEST(auto_accel_deadline_escalation);
    RUN_TEST(auto_accel_no_escalation_low_miss_rate);
    RUN_TEST(auto_accel_zero_capacity);
    RUN_TEST(auto_fallback_below_threshold_admits);
    RUN_TEST(auto_fallback_at_threshold_rejects);

    /* Router adapter */
    RUN_TEST(router_accel_below_threshold_admits);
    RUN_TEST(router_accel_at_threshold_rejects);
    RUN_TEST(router_accel_r1_scales_down);
    RUN_TEST(router_accel_r3_scales_up);
    RUN_TEST(router_accel_zero_capacity);
    RUN_TEST(router_fallback_below_threshold);
    RUN_TEST(router_fallback_at_threshold);

    /* Dispatch */
    RUN_TEST(dispatch_hft_accelerated);
    RUN_TEST(dispatch_hft_fallback);
    RUN_TEST(dispatch_auto_accelerated);
    RUN_TEST(dispatch_router_accelerated);
    RUN_TEST(dispatch_all_fallback_same_behavior);

    /* Isomorphism proofs */
    RUN_TEST(iso_hft_single_low_load);
    RUN_TEST(iso_hft_single_high_load);
    RUN_TEST(iso_hft_gray_zone);
    RUN_TEST(iso_hft_sweep_passes);
    RUN_TEST(iso_auto_sweep_passes);
    RUN_TEST(iso_router_sweep_passes);
    RUN_TEST(iso_router_r1_sweep_passes);
    RUN_TEST(iso_router_r3_sweep_passes);
    RUN_TEST(iso_zero_capacity_all_domains);
    RUN_TEST(iso_decision_hash_deterministic);

    /* Diagnostics */
    RUN_TEST(domain_str_all_valid);
    RUN_TEST(mode_str_all_valid);
    RUN_TEST(adapter_version_nonzero);

    TEST_REPORT();
    return test_failures > 0 ? 1 : 0;
}
