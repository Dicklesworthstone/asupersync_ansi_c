/*
 * test_status.c — unit tests for asx_status
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_status.h>

TEST(status_ok_str) { ASSERT_STR_EQ(asx_status_str(ASX_OK), "OK"); }

TEST(status_error_strs) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_INVALID_TRANSITION), "invalid state transition");
    ASSERT_STR_EQ(asx_status_str(ASX_E_REGION_CLOSED), "region closed");
    ASSERT_STR_EQ(asx_status_str(ASX_E_OBLIGATION_ALREADY_RESOLVED), "obligation already resolved");
    ASSERT_STR_EQ(asx_status_str(ASX_E_CANCELLED), "cancelled");
    ASSERT_STR_EQ(asx_status_str(ASX_E_STALE_HANDLE), "stale handle");
}

TEST(status_is_error) {
    ASSERT_FALSE(asx_is_error(ASX_OK));
    ASSERT_TRUE(asx_is_error(ASX_E_CANCELLED));
    ASSERT_TRUE(asx_is_error(ASX_E_INVALID_TRANSITION));
    ASSERT_TRUE(asx_is_error(ASX_E_RESOURCE_EXHAUSTED));
}

TEST(status_category_maps_public_taxonomy) {
    ASSERT_EQ((int)asx_status_category(ASX_OK), (int)ASX_ERROR_CATEGORY_NONE);
    ASSERT_EQ((int)asx_status_category(ASX_E_INVALID_ARGUMENT), (int)ASX_ERROR_CATEGORY_GENERAL);
    ASSERT_EQ((int)asx_status_category(ASX_E_CANCELLED), (int)ASX_ERROR_CATEGORY_CANCELLATION);
    ASSERT_EQ((int)asx_status_category(ASX_E_CHANNEL_FULL), (int)ASX_ERROR_CATEGORY_CHANNEL);
    ASSERT_EQ((int)asx_status_category(ASX_E_CONFIG_RESTART_REQ), (int)ASX_ERROR_CATEGORY_CONFIG);
    ASSERT_EQ((int)asx_status_category(ASX_E_PERMISSION_DENIED),
              (int)ASX_ERROR_CATEGORY_PERMISSION);
    ASSERT_STR_EQ(asx_error_category_str(ASX_ERROR_CATEGORY_CANCELLATION), "cancellation");
}

TEST(status_recoverability_classifies_retryability) {
    ASSERT_EQ((int)asx_status_recoverability(ASX_OK), (int)ASX_RECOVERABILITY_NONE);
    ASSERT_EQ((int)asx_status_recoverability(ASX_E_CHANNEL_FULL),
              (int)ASX_RECOVERABILITY_TRANSIENT);
    ASSERT_EQ((int)asx_status_recoverability(ASX_E_CANCELLED),
              (int)ASX_RECOVERABILITY_PERMANENT);
    ASSERT_EQ((int)asx_status_recoverability(ASX_E_RESOURCE_EXHAUSTED),
              (int)ASX_RECOVERABILITY_CONTEXT_DEPENDENT);
    ASSERT_TRUE(asx_status_is_retryable(ASX_E_CHANNEL_FULL));
    ASSERT_FALSE(asx_status_is_retryable(ASX_E_CANCELLED));
    ASSERT_STR_EQ(asx_recoverability_str(ASX_RECOVERABILITY_CONTEXT_DEPENDENT),
                  "context-dependent");
}

TEST(status_recovery_actions_expose_guidance) {
    asx_backoff_hint hint;

    ASSERT_EQ((int)asx_status_recovery_action(ASX_E_WOULD_BLOCK),
              (int)ASX_RECOVERY_RETRY_IMMEDIATELY);
    ASSERT_EQ((int)asx_status_recovery_action(ASX_E_RESOURCE_EXHAUSTED),
              (int)ASX_RECOVERY_RETRY_WITH_BACKOFF);
    ASSERT_EQ((int)asx_status_recovery_action(ASX_E_STALE_HANDLE),
              (int)ASX_RECOVERY_REINITIALIZE_CONTEXT);
    ASSERT_EQ((int)asx_status_recovery_action(ASX_E_CONFIG_RESTART_REQ),
              (int)ASX_RECOVERY_INSPECT_CONFIGURATION);
    ASSERT_EQ((int)asx_status_recovery_action(ASX_E_INVALID_TRANSITION),
              (int)ASX_RECOVERY_ESCALATE);
    ASSERT_EQ((int)asx_status_recovery_action(ASX_E_CANCELLED), (int)ASX_RECOVERY_PROPAGATE);
    ASSERT_STR_EQ(asx_recovery_action_str(ASX_RECOVERY_RETRY_WITH_BACKOFF),
                  "retry-with-backoff");

    hint = asx_status_backoff_hint(ASX_E_CHANNEL_FULL);
    ASSERT_EQ(hint.initial_delay_ms, 10u);
    ASSERT_EQ(hint.max_delay_ms, 1000u);
    ASSERT_EQ(hint.max_attempts, 3u);

    hint = asx_status_backoff_hint(ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(hint.initial_delay_ms, 0u);
    ASSERT_EQ(hint.max_delay_ms, 0u);
    ASSERT_EQ(hint.max_attempts, 0u);
}

TEST(status_cancel_helpers_identify_cancel_family) {
    ASSERT_TRUE(asx_status_is_cancel(ASX_E_CANCELLED));
    ASSERT_TRUE(asx_status_is_cancel(ASX_E_WITNESS_REASON_WEAKENED));
    ASSERT_FALSE(asx_status_is_cancel(ASX_E_TIMED_OUT));
}

TEST(status_unknown) { ASSERT_STR_EQ(asx_status_str((asx_status)99999), "unknown status"); }

int main(void) {
    fprintf(stderr, "=== test_status ===\n");
    RUN_TEST(status_ok_str);
    RUN_TEST(status_error_strs);
    RUN_TEST(status_is_error);
    RUN_TEST(status_category_maps_public_taxonomy);
    RUN_TEST(status_recoverability_classifies_retryability);
    RUN_TEST(status_recovery_actions_expose_guidance);
    RUN_TEST(status_cancel_helpers_identify_cancel_family);
    RUN_TEST(status_unknown);
    TEST_REPORT();
    return test_failures;
}
