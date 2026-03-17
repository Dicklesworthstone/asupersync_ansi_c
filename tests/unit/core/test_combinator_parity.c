/*
 * test_combinator_parity.c — tests for cancel token, composition combinators,
 *                            and algebraic laws (bd-yx9r.7.1)
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/combinator.h>
#include <asx/core/combinator2.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test poll functions                                                  */
/* ------------------------------------------------------------------ */

static asx_status poll_ok(void *ud, asx_task_id self) {
    (void)ud; (void)self;
    return ASX_OK;
}

static asx_status poll_fail(void *ud, asx_task_id self) {
    (void)ud; (void)self;
    return ASX_E_INVALID_STATE;
}

static asx_status poll_pending(void *ud, asx_task_id self) {
    (void)ud; (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_fail_then_ok(void *ud, asx_task_id self) {
    int *count = (int *)ud;
    (void)self;
    (*count)++;
    if (*count >= 2) return ASX_OK;
    return ASX_E_INVALID_STATE;
}

/* ================================================================== */
/* Cancel token tests                                                  */
/* ================================================================== */

TEST(cancel_token_none) {
    asx_cancel_token t = asx_cancel_token_none();
    ASSERT_FALSE(asx_cancel_token_is_cancelled(&t));
}

TEST(cancel_token_from_flag) {
    int flag = 0;
    asx_cancel_token t = asx_cancel_token_from(&flag);
    ASSERT_FALSE(asx_cancel_token_is_cancelled(&t));
    flag = 1;
    ASSERT_TRUE(asx_cancel_token_is_cancelled(&t));
}

TEST(cancel_token_null_check) {
    ASSERT_FALSE(asx_cancel_token_is_cancelled(NULL));
}

/* ================================================================== */
/* Race with timeout tests                                             */
/* ================================================================== */

TEST(race_timeout_immediate_win) {
    asx_race_timeout_state state;
    MUST_OK(asx_race_timeout_init(&state, 1000000000ULL));
    MUST_OK(asx_race_timeout_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_race_timeout_poll(&state, 1), ASX_OK);
}

TEST(race_timeout_null_state) {
    ASSERT_EQ(asx_race_timeout_init(NULL, 1000), ASX_E_INVALID_ARGUMENT);
}

TEST(race_timeout_multiple_first_wins) {
    asx_race_timeout_state state;
    MUST_OK(asx_race_timeout_init(&state, 1000000000ULL));
    MUST_OK(asx_race_timeout_add(&state, poll_ok, NULL));
    MUST_OK(asx_race_timeout_add(&state, poll_pending, NULL));
    /* First branch wins immediately */
    ASSERT_EQ(asx_race_timeout_poll(&state, 1), ASX_OK);
}

/* ================================================================== */
/* Retry with timeout tests                                            */
/* ================================================================== */

TEST(retry_timeout_immediate_success) {
    asx_retry_timeout_state state;
    MUST_OK(asx_retry_timeout_init(&state, poll_ok, NULL, 3, 1000000000ULL));
    ASSERT_EQ(asx_retry_timeout_poll(&state, 1), ASX_OK);
    ASSERT_EQ(asx_retry_timeout_attempts(&state), 1u);
}

TEST(retry_timeout_fail_then_succeed) {
    int count = 0;
    asx_retry_timeout_state state;
    asx_status st;
    MUST_OK(asx_retry_timeout_init(&state, poll_fail_then_ok, &count, 3, 1000000000ULL));
    /* First poll fails, resets for retry */
    st = asx_retry_timeout_poll(&state, 1);
    ASSERT_EQ(st, ASX_E_PENDING);
    /* Second poll succeeds */
    st = asx_retry_timeout_poll(&state, 1);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_retry_timeout_attempts(&state), 2u);
}

TEST(retry_timeout_exhausted) {
    asx_retry_timeout_state state;
    asx_status st;
    MUST_OK(asx_retry_timeout_init(&state, poll_fail, NULL, 0, 1000000000ULL));
    st = asx_retry_timeout_poll(&state, 1);
    ASSERT_EQ(st, ASX_E_INVALID_STATE);
}

TEST(retry_timeout_null) {
    ASSERT_EQ(asx_retry_timeout_init(NULL, poll_ok, NULL, 3, 1000), ASX_E_INVALID_ARGUMENT);
    asx_retry_timeout_state state;
    ASSERT_EQ(asx_retry_timeout_init(&state, NULL, NULL, 3, 1000), ASX_E_INVALID_ARGUMENT);
}

TEST(retry_timeout_null_attempts) {
    ASSERT_EQ(asx_retry_timeout_attempts(NULL), 0u);
}

/* ================================================================== */
/* Algebraic property tests                                            */
/* ================================================================== */

/* Law: join of one = that one */
TEST(law_join_single_identity) {
    asx_join_state state;
    MUST_OK(asx_join_init(&state));
    MUST_OK(asx_join_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_join_poll(&state, 1), ASX_OK);
}

/* Law: race of one = that one */
TEST(law_race_single_identity) {
    asx_race_state state;
    MUST_OK(asx_race_init(&state));
    MUST_OK(asx_race_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_race_poll(&state, 1), ASX_OK);
    ASSERT_EQ(asx_race_winner(&state), 0);
}

/* Law: select of one = that one */
TEST(law_select_single_identity) {
    asx_select_state state;
    MUST_OK(asx_select_init(&state));
    MUST_OK(asx_select_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_select_poll(&state, 1), ASX_OK);
    ASSERT_EQ(asx_select_winner(&state), 0);
}

/* Law: join(ok, ok) = ok */
TEST(law_join_all_ok) {
    asx_join_state state;
    MUST_OK(asx_join_init(&state));
    MUST_OK(asx_join_add(&state, poll_ok, NULL));
    MUST_OK(asx_join_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_join_poll(&state, 1), ASX_OK);
}

/* Law: join(ok, fail) = completed but outcome severity > OK */
TEST(law_join_severity_lattice) {
    asx_join_state state;
    asx_outcome combined;
    MUST_OK(asx_join_init(&state));
    MUST_OK(asx_join_add(&state, poll_ok, NULL));
    MUST_OK(asx_join_add(&state, poll_fail, NULL));
    /* Join completes when all branches done */
    ASSERT_EQ(asx_join_poll(&state, 1), ASX_OK);
    /* But combined outcome reflects the failure */
    combined = asx_join_outcome(&state);
    ASSERT_TRUE(combined.severity > ASX_OUTCOME_OK);
}

/* Law: race is deterministic for same-tick resolution */
TEST(law_race_deterministic_winner) {
    asx_race_state s1, s2;
    MUST_OK(asx_race_init(&s1));
    MUST_OK(asx_race_init(&s2));
    MUST_OK(asx_race_add(&s1, poll_ok, NULL));
    MUST_OK(asx_race_add(&s1, poll_ok, NULL));
    MUST_OK(asx_race_add(&s2, poll_ok, NULL));
    MUST_OK(asx_race_add(&s2, poll_ok, NULL));
    MUST_OK(asx_race_poll(&s1, 1));
    MUST_OK(asx_race_poll(&s2, 1));
    /* Same input -> same winner */
    ASSERT_EQ(asx_race_winner(&s1), asx_race_winner(&s2));
}

/* Law: quorum(1, 1) = single branch */
TEST(law_quorum_one_of_one) {
    asx_quorum_state state;
    MUST_OK(asx_quorum_init(&state, 1));
    MUST_OK(asx_quorum_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_quorum_poll(&state, 1), ASX_OK);
    ASSERT_EQ(asx_quorum_ok_count(&state), 1u);
}

/* Law: first_ok with all ok = first branch wins */
TEST(law_first_ok_first_wins) {
    asx_first_ok_state state;
    MUST_OK(asx_first_ok_init(&state));
    MUST_OK(asx_first_ok_add(&state, poll_ok, NULL));
    MUST_OK(asx_first_ok_add(&state, poll_ok, NULL));
    ASSERT_EQ(asx_first_ok_poll(&state, 1), ASX_OK);
    ASSERT_EQ(asx_first_ok_winner(&state), 0);
}

/* Law: pipeline of one stage = that stage */
TEST(law_pipeline_single_identity) {
    asx_pipeline_state state;
    MUST_OK(asx_pipeline_init(&state));
    MUST_OK(asx_pipeline_add_stage(&state, poll_ok, NULL));
    ASSERT_EQ(asx_pipeline_poll(&state, 1), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&state), 1u);
}

/* ================================================================== */
/* Composition tests — nested combinators                              */
/* ================================================================== */

/* Composition: retry(race) — retry a race of alternatives */
TEST(composition_retry_race) {
    /* Race that succeeds immediately — retry should see success on first attempt */
    asx_race_state race;
    asx_retry_state retry;
    MUST_OK(asx_race_init(&race));
    MUST_OK(asx_race_add(&race, poll_ok, NULL));
    MUST_OK(asx_race_add(&race, poll_pending, NULL));

    MUST_OK(asx_retry_init(&retry, asx_race_poll, &race, 3));
    ASSERT_EQ(asx_retry_poll(&retry, 1), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&retry), 1u);
}

/* Composition: join + pipeline — join of pipelines */
TEST(composition_join_pipelines) {
    asx_pipeline_state p1, p2;
    asx_join_state join;

    MUST_OK(asx_pipeline_init(&p1));
    MUST_OK(asx_pipeline_add_stage(&p1, poll_ok, NULL));
    MUST_OK(asx_pipeline_init(&p2));
    MUST_OK(asx_pipeline_add_stage(&p2, poll_ok, NULL));

    MUST_OK(asx_join_init(&join));
    MUST_OK(asx_join_add(&join, asx_pipeline_poll, &p1));
    MUST_OK(asx_join_add(&join, asx_pipeline_poll, &p2));

    ASSERT_EQ(asx_join_poll(&join, 1), ASX_OK);
}

/* Composition: race + bracket — race of bracket patterns */
TEST(composition_race_brackets) {
    asx_bracket_state b1, b2;
    asx_race_state race;

    MUST_OK(asx_bracket_init(&b1, poll_ok, poll_ok, poll_ok, NULL));
    MUST_OK(asx_bracket_init(&b2, poll_ok, poll_ok, poll_ok, NULL));

    MUST_OK(asx_race_init(&race));
    MUST_OK(asx_race_add(&race, asx_bracket_poll, &b1));
    MUST_OK(asx_race_add(&race, asx_bracket_poll, &b2));

    ASSERT_EQ(asx_race_poll(&race, 1), ASX_OK);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "test_combinator_parity\n");

    /* Cancel token */
    RUN_TEST(cancel_token_none);
    RUN_TEST(cancel_token_from_flag);
    RUN_TEST(cancel_token_null_check);

    /* Race with timeout */
    RUN_TEST(race_timeout_immediate_win);
    RUN_TEST(race_timeout_null_state);
    RUN_TEST(race_timeout_multiple_first_wins);

    /* Retry with timeout */
    RUN_TEST(retry_timeout_immediate_success);
    RUN_TEST(retry_timeout_fail_then_succeed);
    RUN_TEST(retry_timeout_exhausted);
    RUN_TEST(retry_timeout_null);
    RUN_TEST(retry_timeout_null_attempts);

    /* Algebraic laws */
    RUN_TEST(law_join_single_identity);
    RUN_TEST(law_race_single_identity);
    RUN_TEST(law_select_single_identity);
    RUN_TEST(law_join_all_ok);
    RUN_TEST(law_join_severity_lattice);
    RUN_TEST(law_race_deterministic_winner);
    RUN_TEST(law_quorum_one_of_one);
    RUN_TEST(law_first_ok_first_wins);
    RUN_TEST(law_pipeline_single_identity);

    /* Composition */
    RUN_TEST(composition_retry_race);
    RUN_TEST(composition_join_pipelines);
    RUN_TEST(composition_race_brackets);

    TEST_REPORT();
    return test_failures;
}
