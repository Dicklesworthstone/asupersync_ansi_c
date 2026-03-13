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

TEST(status_unknown) { ASSERT_STR_EQ(asx_status_str((asx_status)99999), "unknown status"); }

int main(void) {
    fprintf(stderr, "=== test_status ===\n");
    RUN_TEST(status_ok_str);
    RUN_TEST(status_error_strs);
    RUN_TEST(status_is_error);
    RUN_TEST(status_unknown);
    TEST_REPORT();
    return test_failures;
}
