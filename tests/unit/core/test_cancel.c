/*
 * test_cancel.c — unit tests for cancellation severity, budget, and strengthen
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/core/cancel.h>

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
    TEST_REPORT();
    return test_failures;
}
