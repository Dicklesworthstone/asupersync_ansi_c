/*
 * status_basic_test.c — basic tests for asx_status_str and asx_is_error
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx_status.h>

TEST(ok_is_not_error) {
    ASSERT_FALSE(asx_is_error(ASX_OK));
}

TEST(errors_are_errors) {
    ASSERT_TRUE(asx_is_error(ASX_E_INVALID_ARGUMENT));
    ASSERT_TRUE(asx_is_error(ASX_E_INVALID_TRANSITION));
    ASSERT_TRUE(asx_is_error(ASX_E_CANCELLED));
    ASSERT_TRUE(asx_is_error(ASX_E_STALE_HANDLE));
    ASSERT_TRUE(asx_is_error(ASX_E_RESOURCE_EXHAUSTED));
}

TEST(status_str_ok) {
    ASSERT_STR_EQ(asx_status_str(ASX_OK), "OK");
}

TEST(status_str_errors_not_null) {
    /* Every known error code must produce a non-"unknown" string */
    ASSERT_NE(asx_status_str(ASX_E_INVALID_ARGUMENT), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_INVALID_TRANSITION), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_REGION_NOT_FOUND), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_TASK_NOT_FOUND), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_CANCELLED), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_DISCONNECTED), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_TIMER_NOT_FOUND), asx_status_str((asx_status)99999));
    ASSERT_NE(asx_status_str(ASX_E_STALE_HANDLE), asx_status_str((asx_status)99999));
}

TEST(status_str_specific_messages) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_INVALID_ARGUMENT), "invalid argument");
    ASSERT_STR_EQ(asx_status_str(ASX_E_INVALID_TRANSITION), "invalid state transition");
    ASSERT_STR_EQ(asx_status_str(ASX_E_CANCELLED), "cancelled");
    ASSERT_STR_EQ(asx_status_str(ASX_E_STALE_HANDLE), "stale handle");
}

TEST(unknown_status_returns_unknown) {
    ASSERT_STR_EQ(asx_status_str((asx_status)99999), "unknown status");
}

int main(void) {
    RUN_TEST(ok_is_not_error);
    RUN_TEST(errors_are_errors);
    RUN_TEST(status_str_ok);
    RUN_TEST(status_str_errors_not_null);
    RUN_TEST(status_str_specific_messages);
    RUN_TEST(unknown_status_returns_unknown);
    TEST_REPORT();
    return test_failures;
}
