/*
 * test_combinator.c — unit tests for async control flow combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/combinator.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static asx_runtime g_rt;
static void setup_runtime(void) { MUST_OK(asx_runtime_init_default(&g_rt)); }
static void teardown_runtime(void) { asx_runtime_reset(); }

/* ------------------------------------------------------------------ */
/* Test poll functions                                                  */
/* ------------------------------------------------------------------ */

/* Completes immediately with ASX_OK */
static asx_status poll_ok(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* Returns ASX_E_PENDING N times, then ASX_OK.
 * user_data points to a uint32_t counter. */
static asx_status poll_pending_then_ok(void *user_data, asx_task_id self) {
    uint32_t *counter = (uint32_t *)user_data;
    (void)self;
    if (*counter > 0) {
        (*counter)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* Always fails with the status stored in user_data */
static asx_status poll_fail(void *user_data, asx_task_id self) {
    (void)self;
    return *(asx_status *)user_data;
}

/* Returns PENDING N times, then fails */
static asx_status poll_pending_then_fail(void *user_data, asx_task_id self) {
    uint32_t *counter = (uint32_t *)user_data;
    (void)self;
    if (*counter > 0) {
        (*counter)--;
        return ASX_E_PENDING;
    }
    return ASX_E_INVALID_STATE;
}

/* Always returns PENDING (never completes) */
static asx_status poll_forever(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_PENDING;
}

/* Tracks loser drain polling before cancellation. */
static asx_status poll_count_pending(void *user_data, asx_task_id self) {
    uint32_t *count = (uint32_t *)user_data;
    (void)self;
    (*count)++;
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Join tests                                                          */
/* ------------------------------------------------------------------ */

TEST(join_empty) {
    asx_join_state js;
    MUST_OK(asx_join_init(&js));
    ASSERT_EQ(asx_join_poll(&js, 0), ASX_OK);
}

TEST(join_single_immediate) {
    asx_join_state js;
    MUST_OK(asx_join_init(&js));
    MUST_OK(asx_join_add(&js, poll_ok, NULL));
    ASSERT_EQ(asx_join_poll(&js, 0), ASX_OK);
}

TEST(join_two_immediate) {
    asx_join_state js;
    MUST_OK(asx_join_init(&js));
    MUST_OK(asx_join_add(&js, poll_ok, NULL));
    MUST_OK(asx_join_add(&js, poll_ok, NULL));
    ASSERT_EQ(asx_join_poll(&js, 0), ASX_OK);
    ASSERT_EQ(asx_join_outcome(&js).severity, ASX_OUTCOME_OK);
}

TEST(join_waits_for_all) {
    asx_join_state js;
    uint32_t c1 = 2, c2 = 1;
    MUST_OK(asx_join_init(&js));
    MUST_OK(asx_join_add(&js, poll_pending_then_ok, &c1));
    MUST_OK(asx_join_add(&js, poll_pending_then_ok, &c2));

    ASSERT_EQ(asx_join_poll(&js, 0), ASX_E_PENDING); /* both pending */
    ASSERT_EQ(asx_join_poll(&js, 0), ASX_E_PENDING); /* c2 done, c1 still pending */
    ASSERT_EQ(asx_join_poll(&js, 0), ASX_OK);        /* both done */
}

TEST(join_with_failure) {
    asx_join_state js;
    asx_status fail_st = ASX_E_INVALID_STATE;
    MUST_OK(asx_join_init(&js));
    MUST_OK(asx_join_add(&js, poll_ok, NULL));
    MUST_OK(asx_join_add(&js, poll_fail, &fail_st));
    ASSERT_EQ(asx_join_poll(&js, 0), ASX_OK); /* completes (all done) */
    /* Combined outcome reflects the failure */
    ASSERT_EQ(asx_join_outcome(&js).severity, ASX_OUTCOME_ERR);
}

TEST(join_null_fails) { ASSERT_EQ(asx_join_init(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(join_overflow) {
    asx_join_state js;
    uint32_t i;
    MUST_OK(asx_join_init(&js));
    for (i = 0; i < ASX_COMBINATOR_MAX_BRANCHES; i++) { MUST_OK(asx_join_add(&js, poll_ok, NULL)); }
    ASSERT_EQ(asx_join_add(&js, poll_ok, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Race tests                                                          */
/* ------------------------------------------------------------------ */

TEST(race_empty) {
    asx_race_state rs;
    MUST_OK(asx_race_init(&rs));
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_OK);
}

TEST(race_single) {
    asx_race_state rs;
    MUST_OK(asx_race_init(&rs));
    MUST_OK(asx_race_add(&rs, poll_ok, NULL));
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_race_winner(&rs), 0);
}

TEST(race_first_wins) {
    asx_race_state rs;
    uint32_t c = 5; /* second branch takes 5 polls */
    MUST_OK(asx_race_init(&rs));
    MUST_OK(asx_race_add(&rs, poll_ok, NULL));            /* immediate */
    MUST_OK(asx_race_add(&rs, poll_pending_then_ok, &c)); /* slow */
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_race_winner(&rs), 0);
    /* Loser should be marked done */
    ASSERT_TRUE(rs.branches[1].done);
}

TEST(race_second_wins) {
    asx_race_state rs;
    uint32_t c = 1;
    MUST_OK(asx_race_init(&rs));
    MUST_OK(asx_race_add(&rs, poll_pending_then_ok, &c)); /* 1 pending */
    MUST_OK(asx_race_add(&rs, poll_ok, NULL));            /* immediate */
    /* First poll: branch 0 returns PENDING, branch 1 returns OK */
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_race_winner(&rs), 1);
}

TEST(race_both_pending) {
    asx_race_state rs;
    uint32_t c1 = 2, c2 = 1;
    MUST_OK(asx_race_init(&rs));
    MUST_OK(asx_race_add(&rs, poll_pending_then_ok, &c1));
    MUST_OK(asx_race_add(&rs, poll_pending_then_ok, &c2));
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_OK); /* c2 finishes first */
    ASSERT_EQ(asx_race_winner(&rs), 1);
}

TEST(race_winner_result_failure) {
    asx_race_state rs;
    asx_status fail_st = ASX_E_TIMED_OUT;
    MUST_OK(asx_race_init(&rs));
    MUST_OK(asx_race_add(&rs, poll_fail, &fail_st));
    MUST_OK(asx_race_add(&rs, poll_forever, NULL));
    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_E_TIMED_OUT);
    ASSERT_EQ(asx_race_winner(&rs), 0);
    ASSERT_EQ(asx_race_winner_result(&rs), ASX_E_TIMED_OUT);
}

TEST(race_drain_polls_loser_once_before_cancel) {
    asx_race_state rs;
    uint32_t loser_polls = 0;

    MUST_OK(asx_race_init(&rs));
    MUST_OK(asx_race_add(&rs, poll_ok, NULL));
    MUST_OK(asx_race_add(&rs, poll_count_pending, &loser_polls));

    ASSERT_EQ(asx_race_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_race_winner(&rs), 0);
    ASSERT_EQ(loser_polls, 1u);
    ASSERT_TRUE(rs.branches[1].done);
    ASSERT_EQ(rs.branches[1].result, ASX_E_CANCELLED);
    ASSERT_EQ(rs.drained, 1u);
}

/* ------------------------------------------------------------------ */
/* Select tests                                                        */
/* ------------------------------------------------------------------ */

TEST(select_empty) {
    asx_select_state ss;
    MUST_OK(asx_select_init(&ss));
    ASSERT_EQ(asx_select_poll(&ss, 0), ASX_OK);
}

TEST(select_first_ready_wins) {
    asx_select_state ss;
    MUST_OK(asx_select_init(&ss));
    MUST_OK(asx_select_add(&ss, poll_ok, NULL));
    MUST_OK(asx_select_add(&ss, poll_forever, NULL));
    ASSERT_EQ(asx_select_poll(&ss, 0), ASX_OK);
    ASSERT_EQ(asx_select_winner(&ss), 0);
}

TEST(select_fairness_rotation) {
    /* With offset rotation, if branch 0 is always pending and branch 1
     * eventually completes, branch 1 should win despite order */
    asx_select_state ss;
    uint32_t c = 0; /* branch 1: immediate after 0 pending polls */
    MUST_OK(asx_select_init(&ss));
    MUST_OK(asx_select_add(&ss, poll_forever, NULL));
    MUST_OK(asx_select_add(&ss, poll_pending_then_ok, &c));
    /* First poll starts at offset 0: tries 0 (pending), tries 1 (ready) */
    ASSERT_EQ(asx_select_poll(&ss, 0), ASX_OK);
    ASSERT_EQ(asx_select_winner(&ss), 1);
}

TEST(select_drain_polls_loser_once_before_cancel) {
    asx_select_state ss;
    uint32_t loser_polls = 0;

    MUST_OK(asx_select_init(&ss));
    MUST_OK(asx_select_add(&ss, poll_ok, NULL));
    MUST_OK(asx_select_add(&ss, poll_count_pending, &loser_polls));

    ASSERT_EQ(asx_select_poll(&ss, 0), ASX_OK);
    ASSERT_EQ(asx_select_winner(&ss), 0);
    ASSERT_EQ(loser_polls, 1u);
    ASSERT_TRUE(ss.branches[1].done);
    ASSERT_EQ(ss.branches[1].result, ASX_E_CANCELLED);
    ASSERT_EQ(ss.drained, 1u);
}

/* ------------------------------------------------------------------ */
/* Timeout combinator tests                                            */
/* ------------------------------------------------------------------ */

TEST(timeout_inner_completes_immediately) {
    asx_timeout_combinator_state ts;
    setup_runtime();
    MUST_OK(asx_timeout_combinator_init(&ts, poll_ok, NULL, 1000000ULL));
    ASSERT_EQ(asx_timeout_combinator_poll(&ts, 0), ASX_OK);
    ASSERT_FALSE(ts.timed_out);
    teardown_runtime();
}

TEST(timeout_null_fails) {
    ASSERT_EQ(asx_timeout_combinator_init(NULL, poll_ok, NULL, 1000), ASX_E_INVALID_ARGUMENT);
}

TEST(timeout_null_inner_fails) {
    asx_timeout_combinator_state ts;
    ASSERT_EQ(asx_timeout_combinator_init(&ts, NULL, NULL, 1000), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* FirstOk tests                                                       */
/* ------------------------------------------------------------------ */

TEST(first_ok_empty) {
    asx_first_ok_state fs;
    MUST_OK(asx_first_ok_init(&fs));
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_E_INVALID_STATE);
}

TEST(first_ok_first_succeeds) {
    asx_first_ok_state fs;
    MUST_OK(asx_first_ok_init(&fs));
    MUST_OK(asx_first_ok_add(&fs, poll_ok, NULL));
    MUST_OK(asx_first_ok_add(&fs, poll_forever, NULL));
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_OK);
    ASSERT_EQ(asx_first_ok_winner(&fs), 0);
}

TEST(first_ok_fallback_to_second) {
    asx_first_ok_state fs;
    asx_status fail_st = ASX_E_INVALID_STATE;
    MUST_OK(asx_first_ok_init(&fs));
    MUST_OK(asx_first_ok_add(&fs, poll_fail, &fail_st));
    MUST_OK(asx_first_ok_add(&fs, poll_ok, NULL));
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_OK);
    ASSERT_EQ(asx_first_ok_winner(&fs), 1);
}

TEST(first_ok_all_fail) {
    asx_first_ok_state fs;
    asx_status fail1 = ASX_E_INVALID_STATE;
    asx_status fail2 = ASX_E_TIMED_OUT;
    MUST_OK(asx_first_ok_init(&fs));
    MUST_OK(asx_first_ok_add(&fs, poll_fail, &fail1));
    MUST_OK(asx_first_ok_add(&fs, poll_fail, &fail2));
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_E_TIMED_OUT); /* last error */
    ASSERT_EQ(asx_first_ok_winner(&fs), -1);
}

TEST(first_ok_waits_for_pending) {
    asx_first_ok_state fs;
    uint32_t c = 2;
    MUST_OK(asx_first_ok_init(&fs));
    MUST_OK(asx_first_ok_add(&fs, poll_pending_then_ok, &c));
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_OK);
    ASSERT_EQ(asx_first_ok_winner(&fs), 0);
}

TEST(first_ok_pending_then_fail_then_ok) {
    asx_first_ok_state fs;
    uint32_t c = 1;
    MUST_OK(asx_first_ok_init(&fs));
    MUST_OK(asx_first_ok_add(&fs, poll_pending_then_fail, &c));
    MUST_OK(asx_first_ok_add(&fs, poll_ok, NULL));
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_E_PENDING); /* first is pending */
    ASSERT_EQ(asx_first_ok_poll(&fs, 0), ASX_OK);        /* first fails, second succeeds */
    ASSERT_EQ(asx_first_ok_winner(&fs), 1);
}

/* ------------------------------------------------------------------ */
/* Quorum tests                                                        */
/* ------------------------------------------------------------------ */

TEST(quorum_all_succeed) {
    asx_quorum_state qs;
    MUST_OK(asx_quorum_init(&qs, 2));
    MUST_OK(asx_quorum_add(&qs, poll_ok, NULL));
    MUST_OK(asx_quorum_add(&qs, poll_ok, NULL));
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_OK);
    ASSERT_EQ(asx_quorum_ok_count(&qs), 2u);
}

TEST(quorum_2_of_3) {
    asx_quorum_state qs;
    asx_status fail_st = ASX_E_INVALID_STATE;
    MUST_OK(asx_quorum_init(&qs, 2));
    MUST_OK(asx_quorum_add(&qs, poll_ok, NULL));
    MUST_OK(asx_quorum_add(&qs, poll_fail, &fail_st));
    MUST_OK(asx_quorum_add(&qs, poll_ok, NULL));
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_OK);
    ASSERT_EQ(asx_quorum_ok_count(&qs), 2u);
    ASSERT_EQ(asx_quorum_fail_count(&qs), 1u);
}

TEST(quorum_impossible) {
    asx_quorum_state qs;
    asx_status fail_st = ASX_E_INVALID_STATE;
    MUST_OK(asx_quorum_init(&qs, 3)); /* need 3 */
    MUST_OK(asx_quorum_add(&qs, poll_ok, NULL));
    MUST_OK(asx_quorum_add(&qs, poll_fail, &fail_st));
    MUST_OK(asx_quorum_add(&qs, poll_fail, &fail_st));
    /* 1 ok + 2 fail = impossible to reach 3 */
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_quorum_ok_count(&qs), 1u);
}

TEST(quorum_waits_for_pending) {
    asx_quorum_state qs;
    uint32_t c1 = 1, c2 = 2;
    MUST_OK(asx_quorum_init(&qs, 2));
    MUST_OK(asx_quorum_add(&qs, poll_pending_then_ok, &c1));
    MUST_OK(asx_quorum_add(&qs, poll_pending_then_ok, &c2));
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_OK);
    ASSERT_EQ(asx_quorum_ok_count(&qs), 2u);
}

TEST(quorum_zero_threshold_fails) {
    asx_quorum_state qs;
    ASSERT_EQ(asx_quorum_init(&qs, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(quorum_null_fails) { ASSERT_EQ(asx_quorum_init(NULL, 1), ASX_E_INVALID_ARGUMENT); }

TEST(quorum_1_of_3_early_decide) {
    asx_quorum_state qs;
    MUST_OK(asx_quorum_init(&qs, 1));
    MUST_OK(asx_quorum_add(&qs, poll_ok, NULL));
    MUST_OK(asx_quorum_add(&qs, poll_forever, NULL));
    MUST_OK(asx_quorum_add(&qs, poll_forever, NULL));
    /* First succeeds → quorum reached, others cancelled */
    ASSERT_EQ(asx_quorum_poll(&qs, 0), ASX_OK);
    ASSERT_EQ(asx_quorum_ok_count(&qs), 1u);
    /* Other branches should be drained */
    ASSERT_TRUE(qs.branches[1].done);
    ASSERT_TRUE(qs.branches[2].done);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_combinator ===\n");

    /* Join */
    RUN_TEST(join_empty);
    RUN_TEST(join_single_immediate);
    RUN_TEST(join_two_immediate);
    RUN_TEST(join_waits_for_all);
    RUN_TEST(join_with_failure);
    RUN_TEST(join_null_fails);
    RUN_TEST(join_overflow);

    /* Race */
    RUN_TEST(race_empty);
    RUN_TEST(race_single);
    RUN_TEST(race_first_wins);
    RUN_TEST(race_second_wins);
    RUN_TEST(race_both_pending);
    RUN_TEST(race_winner_result_failure);
    RUN_TEST(race_drain_polls_loser_once_before_cancel);

    /* Select */
    RUN_TEST(select_empty);
    RUN_TEST(select_first_ready_wins);
    RUN_TEST(select_fairness_rotation);
    RUN_TEST(select_drain_polls_loser_once_before_cancel);

    /* Timeout */
    RUN_TEST(timeout_inner_completes_immediately);
    RUN_TEST(timeout_null_fails);
    RUN_TEST(timeout_null_inner_fails);

    /* FirstOk */
    RUN_TEST(first_ok_empty);
    RUN_TEST(first_ok_first_succeeds);
    RUN_TEST(first_ok_fallback_to_second);
    RUN_TEST(first_ok_all_fail);
    RUN_TEST(first_ok_waits_for_pending);
    RUN_TEST(first_ok_pending_then_fail_then_ok);

    /* Quorum */
    RUN_TEST(quorum_all_succeed);
    RUN_TEST(quorum_2_of_3);
    RUN_TEST(quorum_impossible);
    RUN_TEST(quorum_waits_for_pending);
    RUN_TEST(quorum_zero_threshold_fails);
    RUN_TEST(quorum_null_fails);
    RUN_TEST(quorum_1_of_3_early_decide);

    TEST_REPORT();
    return test_failures;
}
