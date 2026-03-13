/*
 * test_transition.c — unit tests for transition authority tables
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/transition.h>

/* Region transitions */
TEST(region_legal_forward) {
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSING), ASX_OK);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSING, ASX_REGION_DRAINING), ASX_OK);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSING, ASX_REGION_FINALIZING), ASX_OK);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_DRAINING, ASX_REGION_FINALIZING), ASX_OK);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_FINALIZING, ASX_REGION_CLOSED), ASX_OK);
}

TEST(region_forbidden_backward) {
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSING, ASX_REGION_OPEN),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_DRAINING, ASX_REGION_OPEN),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_FINALIZING, ASX_REGION_OPEN),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_OPEN),
              ASX_E_INVALID_TRANSITION);
}

TEST(region_forbidden_skip) {
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_DRAINING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_FINALIZING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSED),
              ASX_E_INVALID_TRANSITION);
}

TEST(region_closed_absorbing) {
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_OPEN),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_CLOSING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_DRAINING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_FINALIZING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_CLOSED),
              ASX_E_INVALID_TRANSITION);
}

TEST(region_predicates) {
    ASSERT_TRUE(asx_region_can_spawn(ASX_REGION_OPEN));
    ASSERT_FALSE(asx_region_can_spawn(ASX_REGION_CLOSING));
    ASSERT_FALSE(asx_region_can_spawn(ASX_REGION_FINALIZING));
    ASSERT_TRUE(asx_region_can_accept_work(ASX_REGION_OPEN));
    ASSERT_TRUE(asx_region_can_accept_work(ASX_REGION_FINALIZING));
    ASSERT_FALSE(asx_region_can_accept_work(ASX_REGION_DRAINING));
    ASSERT_TRUE(asx_region_is_closing(ASX_REGION_CLOSING));
    ASSERT_TRUE(asx_region_is_closing(ASX_REGION_DRAINING));
    ASSERT_TRUE(asx_region_is_closing(ASX_REGION_FINALIZING));
    ASSERT_FALSE(asx_region_is_closing(ASX_REGION_OPEN));
    ASSERT_TRUE(asx_region_is_terminal(ASX_REGION_CLOSED));
    ASSERT_FALSE(asx_region_is_terminal(ASX_REGION_OPEN));
}

/* Task transitions */
TEST(task_legal_happy_path) {
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_CREATED, ASX_TASK_RUNNING), ASX_OK);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_RUNNING, ASX_TASK_COMPLETED), ASX_OK);
}

TEST(task_legal_cancel_path) {
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_RUNNING, ASX_TASK_CANCEL_REQUESTED), ASX_OK);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_CANCEL_REQUESTED, ASX_TASK_CANCELLING), ASX_OK);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_CANCELLING, ASX_TASK_FINALIZING), ASX_OK);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_FINALIZING, ASX_TASK_COMPLETED), ASX_OK);
}

TEST(task_self_transitions) {
    /* Strengthening (self-transitions) */
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_CANCEL_REQUESTED, ASX_TASK_CANCEL_REQUESTED),
              ASX_OK);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_CANCELLING, ASX_TASK_CANCELLING), ASX_OK);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_FINALIZING, ASX_TASK_FINALIZING), ASX_OK);
}

TEST(task_completed_absorbing) {
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_COMPLETED, ASX_TASK_CREATED),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_COMPLETED, ASX_TASK_RUNNING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_COMPLETED, ASX_TASK_CANCEL_REQUESTED),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_COMPLETED, ASX_TASK_CANCELLING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_COMPLETED, ASX_TASK_FINALIZING),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_COMPLETED, ASX_TASK_COMPLETED),
              ASX_E_INVALID_TRANSITION);
}

/* Obligation transitions */
TEST(obligation_legal) {
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_RESERVED, ASX_OBLIGATION_COMMITTED),
              ASX_OK);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_RESERVED, ASX_OBLIGATION_ABORTED),
              ASX_OK);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_RESERVED, ASX_OBLIGATION_LEAKED),
              ASX_OK);
}

TEST(obligation_terminal_absorbing) {
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_RESERVED),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_COMMITTED),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_ABORTED),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_COMMITTED),
              ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_LEAKED, ASX_OBLIGATION_COMMITTED),
              ASX_E_INVALID_TRANSITION);
}

/* State string functions */
TEST(region_state_str_all) {
    ASSERT_STR_EQ(asx_region_state_str(ASX_REGION_OPEN), "Open");
    ASSERT_STR_EQ(asx_region_state_str(ASX_REGION_CLOSING), "Closing");
    ASSERT_STR_EQ(asx_region_state_str(ASX_REGION_DRAINING), "Draining");
    ASSERT_STR_EQ(asx_region_state_str(ASX_REGION_FINALIZING), "Finalizing");
    ASSERT_STR_EQ(asx_region_state_str(ASX_REGION_CLOSED), "Closed");
}

TEST(region_state_str_out_of_range) {
    ASSERT_STR_EQ(asx_region_state_str((asx_region_state)99), "Unknown");
}

TEST(task_state_str_all) {
    ASSERT_STR_EQ(asx_task_state_str(ASX_TASK_CREATED), "Created");
    ASSERT_STR_EQ(asx_task_state_str(ASX_TASK_RUNNING), "Running");
    ASSERT_STR_EQ(asx_task_state_str(ASX_TASK_CANCEL_REQUESTED), "CancelRequested");
    ASSERT_STR_EQ(asx_task_state_str(ASX_TASK_CANCELLING), "Cancelling");
    ASSERT_STR_EQ(asx_task_state_str(ASX_TASK_FINALIZING), "Finalizing");
    ASSERT_STR_EQ(asx_task_state_str(ASX_TASK_COMPLETED), "Completed");
}

TEST(task_state_str_out_of_range) {
    ASSERT_STR_EQ(asx_task_state_str((asx_task_state)99), "Unknown");
}

TEST(obligation_state_str_all) {
    ASSERT_STR_EQ(asx_obligation_state_str(ASX_OBLIGATION_RESERVED), "Reserved");
    ASSERT_STR_EQ(asx_obligation_state_str(ASX_OBLIGATION_COMMITTED), "Committed");
    ASSERT_STR_EQ(asx_obligation_state_str(ASX_OBLIGATION_ABORTED), "Aborted");
    ASSERT_STR_EQ(asx_obligation_state_str(ASX_OBLIGATION_LEAKED), "Leaked");
}

TEST(obligation_state_str_out_of_range) {
    ASSERT_STR_EQ(asx_obligation_state_str((asx_obligation_state)99), "Unknown");
}

/* Out-of-range transition checks */
TEST(region_transition_out_of_range) {
    ASSERT_EQ(asx_region_transition_check((asx_region_state)99, ASX_REGION_OPEN),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_OPEN, (asx_region_state)99),
              ASX_E_INVALID_ARGUMENT);
}

TEST(task_transition_out_of_range) {
    ASSERT_EQ(asx_task_transition_check((asx_task_state)99, ASX_TASK_CREATED),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_transition_check(ASX_TASK_CREATED, (asx_task_state)99),
              ASX_E_INVALID_ARGUMENT);
}

TEST(obligation_transition_out_of_range) {
    ASSERT_EQ(asx_obligation_transition_check((asx_obligation_state)99, ASX_OBLIGATION_RESERVED),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_transition_check(ASX_OBLIGATION_RESERVED, (asx_obligation_state)99),
              ASX_E_INVALID_ARGUMENT);
}

/* Terminal predicates (exhaustive) */
TEST(task_terminal_predicates) {
    ASSERT_FALSE(asx_task_is_terminal(ASX_TASK_CREATED));
    ASSERT_FALSE(asx_task_is_terminal(ASX_TASK_RUNNING));
    ASSERT_FALSE(asx_task_is_terminal(ASX_TASK_CANCEL_REQUESTED));
    ASSERT_FALSE(asx_task_is_terminal(ASX_TASK_CANCELLING));
    ASSERT_FALSE(asx_task_is_terminal(ASX_TASK_FINALIZING));
    ASSERT_TRUE(asx_task_is_terminal(ASX_TASK_COMPLETED));
}

TEST(obligation_terminal_predicates) {
    ASSERT_FALSE(asx_obligation_is_terminal(ASX_OBLIGATION_RESERVED));
    ASSERT_TRUE(asx_obligation_is_terminal(ASX_OBLIGATION_COMMITTED));
    ASSERT_TRUE(asx_obligation_is_terminal(ASX_OBLIGATION_ABORTED));
    ASSERT_TRUE(asx_obligation_is_terminal(ASX_OBLIGATION_LEAKED));
}

int main(void) {
    fprintf(stderr, "=== test_transition ===\n");
    RUN_TEST(region_legal_forward);
    RUN_TEST(region_forbidden_backward);
    RUN_TEST(region_forbidden_skip);
    RUN_TEST(region_closed_absorbing);
    RUN_TEST(region_predicates);
    RUN_TEST(task_legal_happy_path);
    RUN_TEST(task_legal_cancel_path);
    RUN_TEST(task_self_transitions);
    RUN_TEST(task_completed_absorbing);
    RUN_TEST(obligation_legal);
    RUN_TEST(obligation_terminal_absorbing);
    RUN_TEST(region_state_str_all);
    RUN_TEST(region_state_str_out_of_range);
    RUN_TEST(task_state_str_all);
    RUN_TEST(task_state_str_out_of_range);
    RUN_TEST(obligation_state_str_all);
    RUN_TEST(obligation_state_str_out_of_range);
    RUN_TEST(region_transition_out_of_range);
    RUN_TEST(task_transition_out_of_range);
    RUN_TEST(obligation_transition_out_of_range);
    RUN_TEST(task_terminal_predicates);
    RUN_TEST(obligation_terminal_predicates);
    TEST_REPORT();
    return test_failures;
}
