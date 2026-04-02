/*
 * test_cancel.c — unit tests for cancellation severity, budget, and strengthen
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/core/cancel.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Per-kind severity lookup                                            */
/* ------------------------------------------------------------------ */

TEST(cancel_severity_user) { ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_USER), (int)0); }

TEST(cancel_severity_monotone) {
    /* Severity is non-decreasing with kind ordinal */
    int prev = 0;
    int k;
    for (k = 0; k <= 10; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        ASSERT_TRUE(sev >= prev);
        prev = sev;
    }
}

TEST(cancel_severity_known_values) {
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_TIMEOUT), (int)1);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_DEADLINE), (int)1);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_POLL_QUOTA), (int)2);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_COST_BUDGET), (int)2);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_FAIL_FAST), (int)3);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_RACE_LOST), (int)3);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_LINKED_EXIT), (int)3);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_PARENT), (int)4);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_RESOURCE), (int)4);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_SHUTDOWN), (int)5);
}

/* ------------------------------------------------------------------ */
/* Per-kind quota and priority                                         */
/* ------------------------------------------------------------------ */

TEST(cancel_quota_decreasing) {
    /* Higher severity kinds get tighter (lower) cleanup quotas */
    asx_budget user_b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut_b = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(user_b.poll_quota > shut_b.poll_quota);
}

TEST(cancel_priority_increasing) {
    /* Higher severity kinds get higher cleanup priority */
    asx_budget user_b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut_b = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(shut_b.priority > user_b.priority);
}

/* ------------------------------------------------------------------ */
/* Cleanup budget construction                                         */
/* ------------------------------------------------------------------ */

TEST(cancel_cleanup_budget_sets_quota) {
    asx_budget b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    ASSERT_TRUE(b.poll_quota > 0);
    ASSERT_TRUE(b.poll_quota < UINT32_MAX); /* not infinite */
}

TEST(cancel_cleanup_budget_shutdown_tight) {
    asx_budget user = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(shut.poll_quota < user.poll_quota);
    ASSERT_TRUE(shut.priority > user.priority);
}

/* ------------------------------------------------------------------ */
/* Reason strengthening                                                */
/* ------------------------------------------------------------------ */

TEST(cancel_strengthen_higher_severity_wins) {
    asx_cancel_reason user = {ASX_CANCEL_USER, 0, 0, 100, "user", NULL, 0};
    asx_cancel_reason shut = {ASX_CANCEL_SHUTDOWN, 0, 0, 200, "shut", NULL, 0};
    asx_cancel_reason result = asx_cancel_strengthen(&user, &shut);
    ASSERT_EQ(result.kind, ASX_CANCEL_SHUTDOWN);
}

TEST(cancel_strengthen_commutative_for_different_severity) {
    asx_cancel_reason user = {ASX_CANCEL_USER, 0, 0, 100, "user", NULL, 0};
    asx_cancel_reason shut = {ASX_CANCEL_SHUTDOWN, 0, 0, 200, "shut", NULL, 0};
    asx_cancel_reason ab = asx_cancel_strengthen(&user, &shut);
    asx_cancel_reason ba = asx_cancel_strengthen(&shut, &user);
    ASSERT_EQ(ab.kind, ba.kind);
}

TEST(cancel_strengthen_equal_severity_earlier_wins) {
    asx_cancel_reason a = {ASX_CANCEL_TIMEOUT, 0, 0, 100, "early", NULL, 0};
    asx_cancel_reason b = {ASX_CANCEL_DEADLINE, 0, 0, 200, "late", NULL, 0};
    /* Both severity 1 — earlier timestamp wins */
    asx_cancel_reason result = asx_cancel_strengthen(&a, &b);
    ASSERT_EQ(result.timestamp, (asx_time)100);
}

TEST(cancel_severity_out_of_range_clamps) {
    /* Values beyond ASX_CANCEL_SHUTDOWN (10) should return max severity (5) */
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)11), (int)5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)100), (int)5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)255), (int)5);
}

TEST(cancel_strengthen_equal_severity_same_timestamp) {
    /* Equal severity, equal timestamp: first argument wins (left-bias) */
    asx_cancel_reason a = {ASX_CANCEL_TIMEOUT, 0, 0, 100, "a", NULL, 0};
    asx_cancel_reason b = {ASX_CANCEL_DEADLINE, 0, 0, 100, "b", NULL, 0};
    asx_cancel_reason result = asx_cancel_strengthen(&a, &b);
    ASSERT_EQ(result.kind, ASX_CANCEL_TIMEOUT);
}

/* ------------------------------------------------------------------ */
/* Cancel witness lifecycle                                            */
/* ------------------------------------------------------------------ */

TEST(witness_create_and_query_phase) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 100, "test", NULL, 0};
    asx_cancel_phase phase;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_NE(w, ASX_INVALID_ID);
    ASSERT_TRUE(asx_cancel_witness_is_valid(w));

    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_REQUESTED);

    asx_cancel_witness_release(w);
}

TEST(witness_query_reason) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_SHUTDOWN, 0, 0, 42, "shutdown-test", NULL, 0};
    asx_cancel_reason out;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);

    memset(&out, 0, sizeof(out));
    ASSERT_EQ(asx_cancel_witness_reason(w, &out), ASX_OK);
    ASSERT_EQ(out.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(out.timestamp, (asx_time)42);

    asx_cancel_witness_release(w);
}

TEST(witness_advance_monotone_phases) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "adv", NULL, 0};
    asx_cancel_phase phase;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);

    /* requested → cancelling */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_CANCELLING);

    /* cancelling → finalizing */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_FINALIZING), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_FINALIZING);

    /* finalizing → completed */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_COMPLETED), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_OK);
    ASSERT_EQ(phase, ASX_CANCEL_PHASE_COMPLETED);

    asx_cancel_witness_release(w);
}

TEST(witness_advance_backward_rejected) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "back", NULL, 0};

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_OK);

    /* Backward transition must fail */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_REQUESTED), ASX_E_INVALID_TRANSITION);

    /* Same-phase transition must fail (not strictly increasing) */
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_E_INVALID_TRANSITION);

    asx_cancel_witness_release(w);
}

TEST(witness_release_invalidates) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "rel", NULL, 0};
    asx_cancel_phase phase;

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_TRUE(asx_cancel_witness_is_valid(w));

    ASSERT_EQ(asx_cancel_witness_release(w), ASX_OK);
    ASSERT_TRUE(!asx_cancel_witness_is_valid(w));

    /* Queries on released witness should fail */
    ASSERT_EQ(asx_cancel_witness_phase(w, &phase), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_release(w), ASX_E_NOT_FOUND);
}

TEST(witness_create_null_args_rejected) {
    asx_cancel_witness_id w;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "null", NULL, 0};

    ASSERT_EQ(asx_cancel_witness_create(NULL, 1, &reason), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cancel_witness_create(&w, 1, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(witness_phase_null_out_rejected) {
    asx_cancel_witness_id w = ASX_INVALID_ID;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "np", NULL, 0};

    ASSERT_EQ(asx_cancel_witness_create(&w, 1, &reason), ASX_OK);
    ASSERT_EQ(asx_cancel_witness_phase(w, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cancel_witness_reason(w, NULL), ASX_E_INVALID_ARGUMENT);
    asx_cancel_witness_release(w);
}

TEST(witness_invalid_id_rejected) {
    asx_cancel_phase phase;
    asx_cancel_reason out;

    ASSERT_TRUE(!asx_cancel_witness_is_valid(ASX_INVALID_ID));
    ASSERT_EQ(asx_cancel_witness_phase(ASX_INVALID_ID, &phase), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_reason(ASX_INVALID_ID, &out), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_advance(ASX_INVALID_ID, ASX_CANCEL_PHASE_CANCELLING),
              ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cancel_witness_release(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* Cancel kind/phase string helpers                                    */
/* ------------------------------------------------------------------ */

TEST(cancel_kind_str_known) {
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_USER), "user");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_SHUTDOWN), "shutdown");
    ASSERT_STR_EQ(asx_cancel_kind_str(ASX_CANCEL_TIMEOUT), "timeout");
}

TEST(cancel_kind_str_unknown) {
    ASSERT_STR_EQ(asx_cancel_kind_str((asx_cancel_kind)99), "unknown");
}

TEST(cancel_phase_str_known) {
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_REQUESTED), "requested");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_CANCELLING), "cancelling");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_FINALIZING), "finalizing");
    ASSERT_STR_EQ(asx_cancel_phase_str(ASX_CANCEL_PHASE_COMPLETED), "completed");
}

TEST(cancel_phase_str_unknown) {
    ASSERT_STR_EQ(asx_cancel_phase_str((asx_cancel_phase)99), "unknown");
}

int main(void) {
    fprintf(stderr, "=== test_cancel ===\n");
    RUN_TEST(cancel_severity_user);
    RUN_TEST(cancel_severity_monotone);
    RUN_TEST(cancel_severity_known_values);
    RUN_TEST(cancel_quota_decreasing);
    RUN_TEST(cancel_priority_increasing);
    RUN_TEST(cancel_cleanup_budget_sets_quota);
    RUN_TEST(cancel_cleanup_budget_shutdown_tight);
    RUN_TEST(cancel_strengthen_higher_severity_wins);
    RUN_TEST(cancel_strengthen_commutative_for_different_severity);
    RUN_TEST(cancel_strengthen_equal_severity_earlier_wins);
    RUN_TEST(cancel_severity_out_of_range_clamps);
    RUN_TEST(cancel_strengthen_equal_severity_same_timestamp);

    /* Witness lifecycle */
    RUN_TEST(witness_create_and_query_phase);
    RUN_TEST(witness_query_reason);
    RUN_TEST(witness_advance_monotone_phases);
    RUN_TEST(witness_advance_backward_rejected);
    RUN_TEST(witness_release_invalidates);
    RUN_TEST(witness_create_null_args_rejected);
    RUN_TEST(witness_phase_null_out_rejected);
    RUN_TEST(witness_invalid_id_rejected);

    /* String helpers */
    RUN_TEST(cancel_kind_str_known);
    RUN_TEST(cancel_kind_str_unknown);
    RUN_TEST(cancel_phase_str_known);
    RUN_TEST(cancel_phase_str_unknown);

    TEST_REPORT();
    return test_failures;
}
