/*
 * test_abi.c — unit tests for ABI version contract and frozen constants
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/asx_abi.h>

TEST(abi_version_functions_match_header_macros) {
    ASSERT_EQ(asx_abi_version_major(), (unsigned int)ASX_ABI_VERSION_MAJOR);
    ASSERT_EQ(asx_abi_version_minor(), (unsigned int)ASX_ABI_VERSION_MINOR);
    ASSERT_EQ(asx_abi_version_patch(), (unsigned int)ASX_ABI_VERSION_PATCH);
}

TEST(abi_composite_version_matches_formula) {
    unsigned int expected = (unsigned int)(ASX_ABI_VERSION_MAJOR * 1000000u) +
                            (unsigned int)(ASX_ABI_VERSION_MINOR * 1000u) +
                            (unsigned int)ASX_ABI_VERSION_PATCH;

    ASSERT_EQ((unsigned int)ASX_ABI_VERSION, expected);
    ASSERT_TRUE(ASX_ABI_VERSION >= 1000000u);
}

TEST(abi_check_macro_compiles_for_current_major) {
    ASX_ABI_CHECK(1);
    ASSERT_TRUE(1);
}

TEST(abi_size_constants_match_public_types) {
    ASSERT_EQ(sizeof(asx_region_id), (size_t)ASX_ABI_SIZEOF_REGION_ID);
    ASSERT_EQ(sizeof(asx_task_id), (size_t)ASX_ABI_SIZEOF_TASK_ID);
    ASSERT_EQ(sizeof(asx_obligation_id), (size_t)ASX_ABI_SIZEOF_OBLIGATION_ID);
    ASSERT_EQ(sizeof(asx_timer_id), (size_t)ASX_ABI_SIZEOF_TIMER_ID);
    ASSERT_EQ(sizeof(asx_channel_id), (size_t)ASX_ABI_SIZEOF_CHANNEL_ID);
    ASSERT_EQ(sizeof(asx_status), (size_t)ASX_ABI_SIZEOF_STATUS);
    ASSERT_EQ(sizeof(asx_time), (size_t)ASX_ABI_SIZEOF_TIME);
}

TEST(abi_enum_sentinels_match_frozen_values) {
    ASSERT_EQ((unsigned int)ASX_REGION_CLOSED, ASX_ABI_REGION_STATE_LAST);
    ASSERT_EQ((unsigned int)ASX_TASK_COMPLETED, ASX_ABI_TASK_STATE_LAST);
    ASSERT_EQ((unsigned int)ASX_OBLIGATION_LEAKED, ASX_ABI_OBLIGATION_STATE_LAST);
    ASSERT_EQ((unsigned int)ASX_OUTCOME_PANICKED, ASX_ABI_OUTCOME_SEVERITY_LAST);
    ASSERT_EQ((unsigned int)ASX_CANCEL_SHUTDOWN, ASX_ABI_CANCEL_KIND_LAST);
    ASSERT_EQ((unsigned int)ASX_CANCEL_PHASE_COMPLETED, ASX_ABI_CANCEL_PHASE_LAST);
}

TEST(abi_frozen_numeric_assignments_remain_stable) {
    ASSERT_EQ((unsigned int)ASX_REGION_OPEN, 0u);
    ASSERT_EQ((unsigned int)ASX_REGION_CLOSED, 4u);
    ASSERT_EQ((unsigned int)ASX_TASK_CREATED, 0u);
    ASSERT_EQ((unsigned int)ASX_TASK_COMPLETED, 5u);
    ASSERT_EQ((unsigned int)ASX_OUTCOME_OK, 0u);
    ASSERT_EQ((unsigned int)ASX_OUTCOME_PANICKED, 3u);
    ASSERT_EQ((unsigned int)ASX_CANCEL_USER, 0u);
    ASSERT_EQ((unsigned int)ASX_CANCEL_PHASE_REQUESTED, 0u);
    ASSERT_EQ((unsigned int)ASX_CANCEL_PHASE_COMPLETED, 3u);
}

TEST(abi_public_entry_points_resolve) {
    ASSERT_TRUE(asx_region_open != NULL);
    ASSERT_TRUE(asx_task_spawn != NULL);
    ASSERT_TRUE(asx_scheduler_run != NULL);
    ASSERT_TRUE(asx_runtime_reset != NULL);
    ASSERT_TRUE(asx_status_str != NULL);
    ASSERT_TRUE(asx_abi_version_major != NULL);
}

int main(void) {
    fprintf(stderr, "=== test_abi ===\n");
    RUN_TEST(abi_version_functions_match_header_macros);
    RUN_TEST(abi_composite_version_matches_formula);
    RUN_TEST(abi_check_macro_compiles_for_current_major);
    RUN_TEST(abi_size_constants_match_public_types);
    RUN_TEST(abi_enum_sentinels_match_frozen_values);
    RUN_TEST(abi_frozen_numeric_assignments_remain_stable);
    RUN_TEST(abi_public_entry_points_resolve);
    TEST_REPORT();
    return test_failures;
}
