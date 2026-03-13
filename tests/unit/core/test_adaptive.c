/*
 * test_adaptive.c — unit tests for adaptive-decision contract
 *
 * Tests expected-loss decision layer, evidence ledger, deterministic
 * fallback, and replay digest stability.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/core/adaptive.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test loss function: simple table-based L(action, state)            */
/* ------------------------------------------------------------------ */

/* 2 actions x 2 states loss table:
 *   action 0: [10, 30]  (cautious)
 *   action 1: [5,  50]  (aggressive)
 *
 * In fp 16.16: multiply by 65536.
 */
static uint32_t loss_table[2][2] = {
    {10u << 16, 30u << 16}, /* action 0 */
    {5u << 16, 50u << 16}   /* action 1 */
};

static uint32_t test_loss_fn(void *ctx, asx_adaptive_action action, uint8_t state_index) {
    (void)ctx;
    return loss_table[action][state_index];
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

TEST(init_reset) {
    asx_adaptive_init();
    ASSERT_EQ(asx_adaptive_ledger_count(), 0u);
    ASSERT_EQ(asx_adaptive_fallback_count(), 0u);
    ASSERT_EQ(asx_adaptive_in_fallback(), 0);
    ASSERT_EQ(asx_adaptive_ledger_overflowed(), 0);
}

TEST(policy_set_get) {
    asx_adaptive_policy p;
    asx_adaptive_policy got;

    asx_adaptive_init();

    p.confidence_threshold_fp32 = 1000u;
    p.budget_remaining = 50;
    ASSERT_EQ(asx_adaptive_set_policy(&p), ASX_OK);

    got = asx_adaptive_policy_active();
    ASSERT_EQ(got.confidence_threshold_fp32, 1000u);
    ASSERT_EQ(got.budget_remaining, 50u);
}

TEST(policy_null_returns_error) {
    asx_adaptive_init();
    ASSERT_EQ(asx_adaptive_set_policy(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(decide_selects_min_expected_loss) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();

    memset(&surface, 0, sizeof(surface));
    surface.name = "test-surface";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    /* P(state0) = 0.7, P(state1) = 0.3 in fp 0.32 */
    posterior.posterior[0] = (uint32_t)(0.7 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.3 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX; /* high confidence */

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);

    /* E[L(0)] = 10*0.7 + 30*0.3 = 16
     * E[L(1)] = 5*0.7  + 50*0.3 = 18.5
     * → action 0 should be selected */
    ASSERT_EQ(result.selected, 0u);
    ASSERT_EQ(result.counterfactual, 1u);
    ASSERT_EQ(result.used_fallback, 0);
}

TEST(decide_selects_aggressive_when_favorable) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();

    memset(&surface, 0, sizeof(surface));
    surface.name = "test-surface";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    /* P(state0) = 0.95, P(state1) = 0.05 */
    posterior.posterior[0] = (uint32_t)(0.95 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.05 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);

    /* E[L(0)] = 10*0.95 + 30*0.05 = 11.0
     * E[L(1)] = 5*0.95  + 50*0.05 = 7.25
     * → action 1 (aggressive) should be selected */
    ASSERT_EQ(result.selected, 1u);
    ASSERT_EQ(result.counterfactual, 0u);
}

TEST(fallback_on_low_confidence) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;
    asx_adaptive_policy policy;

    asx_adaptive_init();

    /* Threshold at 50% confidence */
    policy.confidence_threshold_fp32 = (uint32_t)(0.5 * 4294967296.0);
    policy.budget_remaining = 0;
    ASSERT_EQ(asx_adaptive_set_policy(&policy), ASX_OK);

    memset(&surface, 0, sizeof(surface));
    surface.name = "test-surface";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0; /* cautious is fallback */

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.5 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.5 * 4294967296.0);
    posterior.state_count = 2;
    /* Low confidence: 0.3 < threshold 0.5 */
    posterior.confidence_fp32 = (uint32_t)(0.3 * 4294967296.0);

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);

    ASSERT_EQ(result.selected, 0u); /* fallback action */
    ASSERT_EQ(result.used_fallback, 1);
    ASSERT_EQ(asx_adaptive_in_fallback(), 1);
    ASSERT_EQ(asx_adaptive_fallback_count(), 1u);
}

TEST(fallback_on_budget_exhaustion) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;
    asx_adaptive_policy policy;
    uint32_t i;

    asx_adaptive_init();

    /* Budget: 3 adaptive decisions */
    policy.confidence_threshold_fp32 = 0;
    policy.budget_remaining = 3;
    ASSERT_EQ(asx_adaptive_set_policy(&policy), ASX_OK);

    memset(&surface, 0, sizeof(surface));
    surface.name = "test-surface";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.5 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.5 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;

    /* First 3 decisions should be adaptive */
    for (i = 0; i < 3; i++) {
        ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
        ASSERT_EQ(result.used_fallback, 0);
    }

    /* 4th decision should use fallback (budget exhausted) */
    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
    ASSERT_EQ(result.used_fallback, 1);
}

TEST(evidence_ledger_records_decisions) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;
    asx_adaptive_ledger_entry entry;
    asx_adaptive_evidence_term ev[2];

    asx_adaptive_init();

    memset(&surface, 0, sizeof(surface));
    surface.name = "cancel-lane";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.7 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.3 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;

    ev[0].label = "drain_rate";
    ev[0].value_fp32 = 42000;
    ev[1].label = "queue_depth";
    ev[1].value_fp32 = 8000;

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, ev, 2, &result), ASX_OK);

    ASSERT_EQ(asx_adaptive_ledger_count(), 1u);
    ASSERT_TRUE(asx_adaptive_ledger_get(0, &entry));
    ASSERT_EQ(entry.sequence, 0u);
    ASSERT_EQ(entry.evidence_count, 2u);
    ASSERT_STR_EQ(entry.surface, "cancel-lane");
    ASSERT_EQ(entry.decision.selected, result.selected);
}

TEST(ledger_overflow_wraps) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;
    asx_adaptive_ledger_entry entry;
    uint32_t i;

    asx_adaptive_init();

    memset(&surface, 0, sizeof(surface));
    surface.name = "wrap-test";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.5 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.5 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;

    /* Write more than LEDGER_DEPTH entries */
    for (i = 0; i < ASX_ADAPTIVE_LEDGER_DEPTH + 10; i++) {
        ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
    }

    ASSERT_EQ(asx_adaptive_ledger_count(), ASX_ADAPTIVE_LEDGER_DEPTH + 10);
    ASSERT_TRUE(asx_adaptive_ledger_overflowed());

    /* Oldest readable should be entry #10 (0-indexed) */
    ASSERT_TRUE(asx_adaptive_ledger_get(0, &entry));
    ASSERT_EQ(entry.sequence, 10u);
}

TEST(digest_deterministic) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;
    uint64_t d1, d2;

    asx_adaptive_init();

    memset(&surface, 0, sizeof(surface));
    surface.name = "digest-test";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.6 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.4 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
    d1 = asx_adaptive_ledger_digest();

    /* Reset and repeat identical sequence */
    asx_adaptive_reset();
    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
    d2 = asx_adaptive_ledger_digest();

    ASSERT_EQ(d1, d2);
}

TEST(decide_null_surface_returns_error) {
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();
    memset(&posterior, 0, sizeof(posterior));
    ASSERT_EQ(asx_adaptive_decide(NULL, &posterior, NULL, 0, &result), ASX_E_INVALID_ARGUMENT);
}

TEST(decide_null_posterior_returns_error) {
    asx_adaptive_surface surface;
    asx_adaptive_decision result;

    asx_adaptive_init();
    memset(&surface, 0, sizeof(surface));
    surface.name = "null-test";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.fallback = 0;
    ASSERT_EQ(asx_adaptive_decide(&surface, NULL, NULL, 0, &result), ASX_E_INVALID_ARGUMENT);
}

TEST(decide_null_output_returns_error) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;

    asx_adaptive_init();
    memset(&surface, 0, sizeof(surface));
    surface.name = "null-test";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.fallback = 0;
    memset(&posterior, 0, sizeof(posterior));
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;
    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(decide_zero_actions_returns_error) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();
    memset(&surface, 0, sizeof(surface));
    surface.name = "bad-surface";
    surface.action_count = 0;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.fallback = 0;
    memset(&posterior, 0, sizeof(posterior));
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;
    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_E_INVALID_ARGUMENT);
}

TEST(decide_state_count_mismatch_returns_error) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();
    memset(&surface, 0, sizeof(surface));
    surface.name = "mismatch";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.fallback = 0;
    memset(&posterior, 0, sizeof(posterior));
    posterior.state_count = 3; /* mismatch */
    posterior.confidence_fp32 = UINT32_MAX;
    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_E_INVALID_ARGUMENT);
}

TEST(decide_null_loss_fn_returns_error) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();
    memset(&surface, 0, sizeof(surface));
    surface.name = "no-loss";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = NULL;
    surface.fallback = 0;
    memset(&posterior, 0, sizeof(posterior));
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;
    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_E_INVALID_ARGUMENT);
}

TEST(ledger_get_out_of_bounds_returns_zero) {
    asx_adaptive_ledger_entry entry;

    asx_adaptive_init();
    ASSERT_EQ(asx_adaptive_ledger_get(0, &entry), 0);
}

TEST(single_action_surface) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;

    asx_adaptive_init();

    memset(&surface, 0, sizeof(surface));
    surface.name = "single-action";
    surface.action_count = 1;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.5 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.5 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = UINT32_MAX;

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
    ASSERT_EQ(result.selected, 0u);
    ASSERT_EQ(result.counterfactual, 0u); /* only one action */
}

TEST(high_confidence_uses_adaptive_not_fallback) {
    asx_adaptive_surface surface;
    asx_adaptive_posterior posterior;
    asx_adaptive_decision result;
    asx_adaptive_policy policy;

    asx_adaptive_init();

    policy.confidence_threshold_fp32 = (uint32_t)(0.5 * 4294967296.0);
    policy.budget_remaining = 0;
    ASSERT_EQ(asx_adaptive_set_policy(&policy), ASX_OK);

    memset(&surface, 0, sizeof(surface));
    surface.name = "high-conf";
    surface.action_count = 2;
    surface.state_count = 2;
    surface.loss_fn = test_loss_fn;
    surface.loss_ctx = NULL;
    surface.fallback = 0;

    memset(&posterior, 0, sizeof(posterior));
    posterior.posterior[0] = (uint32_t)(0.95 * 4294967296.0);
    posterior.posterior[1] = (uint32_t)(0.05 * 4294967296.0);
    posterior.state_count = 2;
    posterior.confidence_fp32 = (uint32_t)(0.9 * 4294967296.0); /* above threshold */

    ASSERT_EQ(asx_adaptive_decide(&surface, &posterior, NULL, 0, &result), ASX_OK);
    ASSERT_EQ(result.used_fallback, 0);
}

/* ------------------------------------------------------------------ */
/* Test runner                                                        */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_adaptive ===\n");

    RUN_TEST(init_reset);
    RUN_TEST(policy_set_get);
    RUN_TEST(policy_null_returns_error);
    RUN_TEST(decide_selects_min_expected_loss);
    RUN_TEST(decide_selects_aggressive_when_favorable);
    RUN_TEST(fallback_on_low_confidence);
    RUN_TEST(fallback_on_budget_exhaustion);
    RUN_TEST(evidence_ledger_records_decisions);
    RUN_TEST(ledger_overflow_wraps);
    RUN_TEST(digest_deterministic);
    RUN_TEST(decide_null_surface_returns_error);
    RUN_TEST(decide_null_posterior_returns_error);
    RUN_TEST(decide_null_output_returns_error);
    RUN_TEST(decide_zero_actions_returns_error);
    RUN_TEST(decide_state_count_mismatch_returns_error);
    RUN_TEST(decide_null_loss_fn_returns_error);
    RUN_TEST(ledger_get_out_of_bounds_returns_zero);
    RUN_TEST(single_action_surface);
    RUN_TEST(high_confidence_uses_adaptive_not_fallback);

    TEST_REPORT();
    return test_failures;
}
