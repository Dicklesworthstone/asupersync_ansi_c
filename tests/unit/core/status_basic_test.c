/*
 * status_basic_test.c — basic tests for asx_status_str and asx_is_error
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_status.h>

TEST(ok_is_not_error) { ASSERT_FALSE(asx_is_error(ASX_OK)); }

TEST(errors_are_errors) {
    ASSERT_TRUE(asx_is_error(ASX_E_INVALID_ARGUMENT));
    ASSERT_TRUE(asx_is_error(ASX_E_INVALID_TRANSITION));
    ASSERT_TRUE(asx_is_error(ASX_E_CANCELLED));
    ASSERT_TRUE(asx_is_error(ASX_E_STALE_HANDLE));
    ASSERT_TRUE(asx_is_error(ASX_E_RESOURCE_EXHAUSTED));
}

TEST(status_str_ok) { ASSERT_STR_EQ(asx_status_str(ASX_OK), "OK"); }

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

TEST(status_names_are_enumerator_spellings) {
    ASSERT_STR_EQ(asx_status_name(ASX_OK), "ASX_OK");
    ASSERT_STR_EQ(asx_status_name(ASX_E_REGION_CLOSED), "ASX_E_REGION_CLOSED");
    ASSERT_STR_EQ(asx_status_name(ASX_E_PERMISSION_DENIED), "ASX_E_PERMISSION_DENIED");
    ASSERT_TRUE(asx_status_name((asx_status)99999) == NULL);
    ASSERT_TRUE(asx_status_name((asx_status)-1) == NULL);
}

TEST(status_names_round_trip_for_every_code) {
    /* Every code in [0, 2000) that has a name parses back to itself, and
     * every code asx_status_str knows has a name. */
    int code;
    int named = 0;
    for (code = 0; code < 2000; code++) {
        asx_status parsed = ASX_E_INVALID_STATE;
        const char *name = asx_status_name((asx_status)code);
        if (name == NULL) {
            ASSERT_STR_EQ(asx_status_str((asx_status)code), "unknown status");
            continue;
        }
        named++;
        ASSERT_TRUE(asx_status_from_name(name, &parsed));
        ASSERT_EQ((int)parsed, code);
    }
    ASSERT_TRUE(named >= 67);
}

TEST(status_from_name_rejects_near_misses) {
    asx_status out = ASX_E_PENDING;
    ASSERT_FALSE(asx_status_from_name("asx_ok", &out));
    ASSERT_FALSE(asx_status_from_name("ASX_OK ", &out));
    ASSERT_FALSE(asx_status_from_name("", &out));
    ASSERT_FALSE(asx_status_from_name(NULL, &out));
    ASSERT_FALSE(asx_status_from_name("ASX_OK", NULL));
    ASSERT_EQ(out, ASX_E_PENDING); /* untouched on failure */
}

int main(void) {
    RUN_TEST(ok_is_not_error);
    RUN_TEST(errors_are_errors);
    RUN_TEST(status_str_ok);
    RUN_TEST(status_str_errors_not_null);
    RUN_TEST(status_str_specific_messages);
    RUN_TEST(unknown_status_returns_unknown);
    RUN_TEST(status_names_are_enumerator_spellings);
    RUN_TEST(status_names_round_trip_for_every_code);
    RUN_TEST(status_from_name_rejects_near_misses);
    TEST_REPORT();
    return test_failures;
}
