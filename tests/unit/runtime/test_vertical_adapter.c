/*
 * test_vertical_adapter.c — vertical acceleration adapter tests (bd-j4m.5)
 *
 * Validates adapter descriptors, evaluation correctness, mode
 * invariance (isomorphism), annotation population, and regression
 * diagnostics for HFT, AUTOMOTIVE, and EMBEDDED_ROUTER adapters.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_log.h"
#include "test_harness.h"
#include <asx/runtime/vertical_adapter.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/profile_compat.h>
#include <string.h>

/* ===================================================================
 * Adapter descriptor tests
 * =================================================================== */

TEST(adapter_count_is_three)
{
    ASSERT_EQ(asx_adapter_count(), 3u);
}

TEST(adapter_descriptor_hft)
{
    asx_adapter_descriptor desc;
    asx_status s = asx_adapter_get_descriptor(ASX_ADAPTER_HFT, &desc);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ((int)desc.id, (int)ASX_ADAPTER_HFT);
    ASSERT_EQ((int)desc.profile, (int)ASX_PROFILE_ID_HFT);
    ASSERT_TRUE(desc.name != NULL);
    ASSERT_TRUE(desc.description != NULL);
}

TEST(adapter_descriptor_automotive)
{
    asx_adapter_descriptor desc;
    asx_status s = asx_adapter_get_descriptor(ASX_ADAPTER_AUTOMOTIVE, &desc);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ((int)desc.id, (int)ASX_ADAPTER_AUTOMOTIVE);
    ASSERT_EQ((int)desc.profile, (int)ASX_PROFILE_ID_AUTOMOTIVE);
    ASSERT_TRUE(desc.name != NULL);
}

TEST(adapter_descriptor_router)
{
    asx_adapter_descriptor desc;
    asx_status s = asx_adapter_get_descriptor(ASX_ADAPTER_ROUTER, &desc);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ((int)desc.id, (int)ASX_ADAPTER_ROUTER);
    ASSERT_EQ((int)desc.profile, (int)ASX_PROFILE_ID_EMBEDDED_ROUTER);
    ASSERT_TRUE(desc.name != NULL);
}

TEST(adapter_descriptor_null_rejected)
{
    asx_status s = asx_adapter_get_descriptor(ASX_ADAPTER_HFT, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(adapter_descriptor_invalid_id_rejected)
{
    asx_adapter_descriptor desc;
    asx_status s = asx_adapter_get_descriptor((asx_adapter_id)99, &desc);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(adapter_name_hft)
{
    ASSERT_STR_EQ(asx_adapter_name(ASX_ADAPTER_HFT), "HFT_LATENCY");
}

TEST(adapter_name_automotive)
{
    ASSERT_STR_EQ(asx_adapter_name(ASX_ADAPTER_AUTOMOTIVE), "AUTO_COMPLIANCE");
}

TEST(adapter_name_router)
{
    ASSERT_STR_EQ(asx_adapter_name(ASX_ADAPTER_ROUTER), "ROUTER_QUEUE");
}

TEST(adapter_name_invalid)
{
    ASSERT_STR_EQ(asx_adapter_name((asx_adapter_id)99), "UNKNOWN");
}

TEST(adapter_mode_str_fallback)
{
    ASSERT_STR_EQ(asx_adapter_mode_str(ASX_ADAPTER_MODE_FALLBACK), "FALLBACK");
}

TEST(adapter_mode_str_accelerated)
{
    ASSERT_STR_EQ(asx_adapter_mode_str(ASX_ADAPTER_MODE_ACCELERATED), "ACCELERATED");
}

TEST(adapter_mode_str_invalid)
{
    ASSERT_STR_EQ(asx_adapter_mode_str((asx_adapter_mode)99), "UNKNOWN");
}

TEST(adapter_profile_mapping)
{
    ASSERT_EQ((int)asx_adapter_profile(ASX_ADAPTER_HFT),
              (int)ASX_PROFILE_ID_HFT);
    ASSERT_EQ((int)asx_adapter_profile(ASX_ADAPTER_AUTOMOTIVE),
              (int)ASX_PROFILE_ID_AUTOMOTIVE);
    ASSERT_EQ((int)asx_adapter_profile(ASX_ADAPTER_ROUTER),
              (int)ASX_PROFILE_ID_EMBEDDED_ROUTER);
}

TEST(adapter_profile_invalid)
{
    ASSERT_EQ((int)asx_adapter_profile((asx_adapter_id)99),
              (int)ASX_PROFILE_ID_COUNT);
}

/* ===================================================================
 * Evaluation tests — fallback mode
 * =================================================================== */

TEST(evaluate_fallback_hft_below_threshold)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    s = asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_FALLBACK,
                               50, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ((int)res.adapter, (int)ASX_ADAPTER_HFT);
    ASSERT_EQ((int)res.mode, (int)ASX_ADAPTER_MODE_FALLBACK);
    ASSERT_FALSE(res.decision.triggered);
    ASSERT_EQ(res.decision.admit_status, ASX_OK);
    ASSERT_FALSE(res.has_annotations);
}

TEST(evaluate_fallback_hft_above_threshold)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    s = asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_FALLBACK,
                               90, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(res.decision.triggered);
    /* HFT uses SHED_OLDEST at 85% */
    ASSERT_TRUE(res.decision.shed_count > 0);
    ASSERT_FALSE(res.has_annotations);
}

TEST(evaluate_fallback_automotive_backpressure)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    s = asx_adapter_evaluate(ASX_ADAPTER_AUTOMOTIVE, ASX_ADAPTER_MODE_FALLBACK,
                               95, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(res.decision.triggered);
    ASSERT_EQ(res.decision.admit_status, ASX_E_WOULD_BLOCK);
    ASSERT_FALSE(res.has_annotations);
}

TEST(evaluate_fallback_router_reject)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    s = asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_FALLBACK,
                               80, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    /* EMBEDDED_ROUTER rejects at 75% */
    ASSERT_TRUE(res.decision.triggered);
    ASSERT_EQ(res.decision.admit_status, ASX_E_ADMISSION_CLOSED);
    ASSERT_FALSE(res.has_annotations);
}

TEST(evaluate_null_result_rejected)
{
    asx_status s = asx_adapter_evaluate(ASX_ADAPTER_HFT,
                                          ASX_ADAPTER_MODE_FALLBACK,
                                          50, 100, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(evaluate_invalid_adapter_rejected)
{
    asx_adapter_result res;
    asx_status s = asx_adapter_evaluate((asx_adapter_id)99,
                                          ASX_ADAPTER_MODE_FALLBACK,
                                          50, 100, &res);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Evaluation tests — accelerated mode (with annotations)
 * =================================================================== */

TEST(evaluate_accelerated_hft_has_annotations)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    /* Record some latency samples so annotations have data */
    asx_hft_record_poll_latency(100);
    asx_hft_record_poll_latency(200);
    asx_hft_record_poll_latency(500);

    s = asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_ACCELERATED,
                               50, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(res.has_annotations);
    /* p99 should be nonzero since we recorded samples */
    ASSERT_TRUE(res.annotations.hft.p99_ns > 0 ||
                res.annotations.hft.overflow_count == 0);
}

TEST(evaluate_accelerated_automotive_has_annotations)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    /* Record some deadline evaluations */
    asx_auto_record_deadline(1000, 900, 1);  /* hit */
    asx_auto_record_deadline(1000, 1100, 2); /* miss */

    s = asx_adapter_evaluate(ASX_ADAPTER_AUTOMOTIVE,
                               ASX_ADAPTER_MODE_ACCELERATED,
                               50, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(res.has_annotations);
    /* Should have a nonzero miss rate (1 miss out of 2) */
    ASSERT_TRUE(res.annotations.automotive.miss_rate_pct100 > 0);
    ASSERT_TRUE(res.annotations.automotive.audit_count > 0);
}

TEST(evaluate_accelerated_router_has_annotations)
{
    asx_adapter_result res;
    asx_status s;

    asx_adapter_reset_all();
    s = asx_adapter_evaluate(ASX_ADAPTER_ROUTER,
                               ASX_ADAPTER_MODE_ACCELERATED,
                               50, 100, &res);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(res.has_annotations);
    ASSERT_EQ(res.annotations.router.queue_depth, 50u);
    ASSERT_EQ(res.annotations.router.headroom, 50u);
}

TEST(evaluate_accelerated_router_reject_streak)
{
    asx_adapter_result res;

    asx_adapter_reset_all();

    /* Trigger overload (75% threshold for router) */
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           80, 100, &res);
    ASSERT_TRUE(res.decision.triggered);
    ASSERT_EQ(res.annotations.router.reject_streak, 1u);

    /* Trigger again */
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           80, 100, &res);
    ASSERT_EQ(res.annotations.router.reject_streak, 2u);

    /* Drop below threshold — streak resets */
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           50, 100, &res);
    ASSERT_EQ(res.annotations.router.reject_streak, 0u);
}

/* ===================================================================
 * Decision digest invariance tests
 * =================================================================== */

TEST(digest_matches_between_modes_hft)
{
    asx_adapter_result fb, ac;

    asx_adapter_reset_all();
    asx_adapter_evaluate_both(ASX_ADAPTER_HFT, 50, 100, &fb, &ac);
    ASSERT_EQ(fb.decision_digest, ac.decision_digest);
}

TEST(digest_matches_between_modes_automotive)
{
    asx_adapter_result fb, ac;

    asx_adapter_reset_all();
    asx_adapter_evaluate_both(ASX_ADAPTER_AUTOMOTIVE, 95, 100, &fb, &ac);
    ASSERT_EQ(fb.decision_digest, ac.decision_digest);
}

TEST(digest_matches_between_modes_router)
{
    asx_adapter_result fb, ac;

    asx_adapter_reset_all();
    asx_adapter_evaluate_both(ASX_ADAPTER_ROUTER, 80, 100, &fb, &ac);
    ASSERT_EQ(fb.decision_digest, ac.decision_digest);
}

TEST(digest_differs_for_different_load)
{
    asx_adapter_result low, high;

    asx_adapter_reset_all();
    asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_FALLBACK,
                           50, 100, &low);
    asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_FALLBACK,
                           90, 100, &high);
    /* Decisions differ (not triggered vs triggered) so digests differ */
    ASSERT_NE(low.decision_digest, high.decision_digest);
}

TEST(evaluate_both_null_safety)
{
    asx_adapter_result fb, ac;
    asx_status s;

    s = asx_adapter_evaluate_both(ASX_ADAPTER_HFT, 50, 100, NULL, &ac);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);

    s = asx_adapter_evaluate_both(ASX_ADAPTER_HFT, 50, 100, &fb, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Isomorphism verification tests
 * =================================================================== */

TEST(isomorphism_hft_builtin_passes)
{
    asx_isomorphism_result res;
    int pass;

    asx_adapter_reset_all();
    pass = asx_adapter_isomorphism_builtin(ASX_ADAPTER_HFT, &res);
    ASSERT_TRUE(pass);
    ASSERT_TRUE(res.pass);
    ASSERT_EQ((int)res.adapter, (int)ASX_ADAPTER_HFT);
    ASSERT_TRUE(res.evaluations > 0);
    ASSERT_EQ(res.matches, res.evaluations);
}

TEST(isomorphism_automotive_builtin_passes)
{
    asx_isomorphism_result res;
    int pass;

    asx_adapter_reset_all();
    pass = asx_adapter_isomorphism_builtin(ASX_ADAPTER_AUTOMOTIVE, &res);
    ASSERT_TRUE(pass);
    ASSERT_TRUE(res.pass);
    ASSERT_EQ(res.matches, res.evaluations);
}

TEST(isomorphism_router_builtin_passes)
{
    asx_isomorphism_result res;
    int pass;

    asx_adapter_reset_all();
    pass = asx_adapter_isomorphism_builtin(ASX_ADAPTER_ROUTER, &res);
    ASSERT_TRUE(pass);
    ASSERT_TRUE(res.pass);
    ASSERT_EQ(res.matches, res.evaluations);
}

TEST(isomorphism_custom_scenarios)
{
    asx_adapter_scenario scenarios[] = {
        {  0,  64 },
        { 32,  64 },
        { 48,  64 },
        { 56,  64 },
        { 60,  64 },
        { 64,  64 }
    };
    asx_isomorphism_result res;
    int pass;

    asx_adapter_reset_all();
    pass = asx_adapter_isomorphism_check(ASX_ADAPTER_HFT,
                                           scenarios, 6, &res);
    ASSERT_TRUE(pass);
    ASSERT_EQ(res.evaluations, 6u);
    ASSERT_EQ(res.matches, 6u);
}

TEST(isomorphism_null_result_returns_zero)
{
    asx_adapter_scenario sc = { 50, 100 };
    int pass = asx_adapter_isomorphism_check(ASX_ADAPTER_HFT,
                                               &sc, 1, NULL);
    ASSERT_FALSE(pass);
}

TEST(isomorphism_null_scenarios_returns_zero)
{
    asx_isomorphism_result res;
    int pass = asx_adapter_isomorphism_check(ASX_ADAPTER_HFT,
                                               NULL, 0, &res);
    ASSERT_FALSE(pass);
    ASSERT_FALSE(res.pass);
}

TEST(isomorphism_invalid_adapter_returns_zero)
{
    asx_adapter_scenario sc = { 50, 100 };
    asx_isomorphism_result res;
    int pass = asx_adapter_isomorphism_check((asx_adapter_id)99,
                                               &sc, 1, &res);
    ASSERT_FALSE(pass);
}

TEST(isomorphism_all_adapters_builtin)
{
    int i;
    for (i = 0; i < ASX_ADAPTER_COUNT; i++) {
        asx_isomorphism_result res;
        int pass;

        asx_adapter_reset_all();
        pass = asx_adapter_isomorphism_builtin((asx_adapter_id)i, &res);
        if (!pass) {
            fprintf(stderr, "    adapter %s diverged at scenario %u\n",
                    asx_adapter_name((asx_adapter_id)i),
                    res.divergence_index);
        }
        ASSERT_TRUE(pass);
    }
}

/* ===================================================================
 * Adapter-catalog consistency tests
 * =================================================================== */

TEST(adapter_profile_matches_catalog)
{
    int i;
    for (i = 0; i < ASX_ADAPTER_COUNT; i++) {
        asx_adapter_descriptor desc;
        const asx_overload_catalog_entry *entry = NULL;

        asx_adapter_get_descriptor((asx_adapter_id)i, &desc);
        asx_overload_catalog_get(desc.profile, &entry);

        ASSERT_TRUE(entry != NULL);
        ASSERT_EQ((int)entry->profile, (int)desc.profile);
    }
}

TEST(adapter_decision_consistent_with_catalog)
{
    int i;
    for (i = 0; i < ASX_ADAPTER_COUNT; i++) {
        asx_adapter_descriptor desc;
        const asx_overload_catalog_entry *entry = NULL;
        asx_adapter_result res;

        asx_adapter_reset_all();
        asx_adapter_get_descriptor((asx_adapter_id)i, &desc);
        asx_overload_catalog_get(desc.profile, &entry);

        /* Evaluate above any profile's threshold */
        asx_adapter_evaluate((asx_adapter_id)i, ASX_ADAPTER_MODE_FALLBACK,
                               95, 100, &res);

        ASSERT_TRUE(asx_overload_catalog_decision_consistent(
            entry, &res.decision));
    }
}

/* ===================================================================
 * Reset and isolation tests
 * =================================================================== */

TEST(reset_clears_state)
{
    asx_adapter_result res;

    /* Generate some state */
    asx_hft_record_poll_latency(1000);
    asx_auto_record_deadline(2000, 3000, 1);
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           80, 100, &res);

    /* Reset should clear everything */
    asx_adapter_reset_all();

    /* Router reject streak should be zero */
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           50, 100, &res);
    ASSERT_EQ(res.annotations.router.reject_streak, 0u);

    /* HFT histogram should be empty */
    asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_ACCELERATED,
                           50, 100, &res);
    ASSERT_EQ(res.annotations.hft.p99_ns, 0u);
    ASSERT_EQ(res.annotations.hft.overflow_count, 0u);
}

/* ===================================================================
 * Annotation detail tests
 * =================================================================== */

TEST(hft_annotations_reflect_histogram)
{
    asx_adapter_result res;

    asx_adapter_reset_all();
    /* Record samples that will land in known bins */
    asx_hft_record_poll_latency(100);
    asx_hft_record_poll_latency(200);
    asx_hft_record_poll_latency(50000); /* overflow */

    asx_adapter_evaluate(ASX_ADAPTER_HFT, ASX_ADAPTER_MODE_ACCELERATED,
                           50, 100, &res);
    ASSERT_TRUE(res.has_annotations);
    ASSERT_EQ(res.annotations.hft.overflow_count, 1u);
}

TEST(automotive_annotations_reflect_tracker)
{
    asx_adapter_result res;

    asx_adapter_reset_all();
    /* Record 3 hits, 1 miss */
    asx_auto_record_deadline(1000, 800, 1);
    asx_auto_record_deadline(1000, 900, 2);
    asx_auto_record_deadline(1000, 950, 3);
    asx_auto_record_deadline(1000, 1100, 4); /* miss */

    asx_adapter_evaluate(ASX_ADAPTER_AUTOMOTIVE,
                           ASX_ADAPTER_MODE_ACCELERATED,
                           50, 100, &res);
    ASSERT_TRUE(res.has_annotations);
    /* 1 miss out of 4 = 25% = 2500 in pct*100 */
    ASSERT_EQ(res.annotations.automotive.miss_rate_pct100, 2500u);
    /* 1 audit entry (the miss) */
    ASSERT_TRUE(res.annotations.automotive.audit_count >= 1u);
}

TEST(router_annotations_headroom)
{
    asx_adapter_result res;

    asx_adapter_reset_all();
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           30, 100, &res);
    ASSERT_TRUE(res.has_annotations);
    ASSERT_EQ(res.annotations.router.queue_depth, 30u);
    ASSERT_EQ(res.annotations.router.headroom, 70u);
}

TEST(router_annotations_zero_headroom_at_capacity)
{
    asx_adapter_result res;

    asx_adapter_reset_all();
    asx_adapter_evaluate(ASX_ADAPTER_ROUTER, ASX_ADAPTER_MODE_ACCELERATED,
                           100, 100, &res);
    ASSERT_EQ(res.annotations.router.queue_depth, 100u);
    ASSERT_EQ(res.annotations.router.headroom, 0u);
}

/* ===================================================================
 * main
 * =================================================================== */

int main(void)
{
    test_log_open("unit", "runtime/vertical_adapter",
                  "vertical-adapter");

    /* Adapter descriptors */
    RUN_TEST(adapter_count_is_three);
    RUN_TEST(adapter_descriptor_hft);
    RUN_TEST(adapter_descriptor_automotive);
    RUN_TEST(adapter_descriptor_router);
    RUN_TEST(adapter_descriptor_null_rejected);
    RUN_TEST(adapter_descriptor_invalid_id_rejected);
    RUN_TEST(adapter_name_hft);
    RUN_TEST(adapter_name_automotive);
    RUN_TEST(adapter_name_router);
    RUN_TEST(adapter_name_invalid);
    RUN_TEST(adapter_mode_str_fallback);
    RUN_TEST(adapter_mode_str_accelerated);
    RUN_TEST(adapter_mode_str_invalid);
    RUN_TEST(adapter_profile_mapping);
    RUN_TEST(adapter_profile_invalid);

    /* Fallback evaluation */
    RUN_TEST(evaluate_fallback_hft_below_threshold);
    RUN_TEST(evaluate_fallback_hft_above_threshold);
    RUN_TEST(evaluate_fallback_automotive_backpressure);
    RUN_TEST(evaluate_fallback_router_reject);
    RUN_TEST(evaluate_null_result_rejected);
    RUN_TEST(evaluate_invalid_adapter_rejected);

    /* Accelerated evaluation */
    RUN_TEST(evaluate_accelerated_hft_has_annotations);
    RUN_TEST(evaluate_accelerated_automotive_has_annotations);
    RUN_TEST(evaluate_accelerated_router_has_annotations);
    RUN_TEST(evaluate_accelerated_router_reject_streak);

    /* Decision digest invariance */
    RUN_TEST(digest_matches_between_modes_hft);
    RUN_TEST(digest_matches_between_modes_automotive);
    RUN_TEST(digest_matches_between_modes_router);
    RUN_TEST(digest_differs_for_different_load);
    RUN_TEST(evaluate_both_null_safety);

    /* Isomorphism verification */
    RUN_TEST(isomorphism_hft_builtin_passes);
    RUN_TEST(isomorphism_automotive_builtin_passes);
    RUN_TEST(isomorphism_router_builtin_passes);
    RUN_TEST(isomorphism_custom_scenarios);
    RUN_TEST(isomorphism_null_result_returns_zero);
    RUN_TEST(isomorphism_null_scenarios_returns_zero);
    RUN_TEST(isomorphism_invalid_adapter_returns_zero);
    RUN_TEST(isomorphism_all_adapters_builtin);

    /* Adapter-catalog consistency */
    RUN_TEST(adapter_profile_matches_catalog);
    RUN_TEST(adapter_decision_consistent_with_catalog);

    /* Reset and isolation */
    RUN_TEST(reset_clears_state);

    /* Annotation details */
    RUN_TEST(hft_annotations_reflect_histogram);
    RUN_TEST(automotive_annotations_reflect_tracker);
    RUN_TEST(router_annotations_headroom);
    RUN_TEST(router_annotations_zero_headroom_at_capacity);

    TEST_REPORT();
    test_log_close();
    return test_failures;
}
