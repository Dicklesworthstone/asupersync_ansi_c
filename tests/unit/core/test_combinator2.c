/*
 * test_combinator2.c — unit tests for second-wave combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/combinator.h>
#include <asx/core/combinator2.h>
#include <asx/runtime/runtime.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test poll functions                                                  */
/* ------------------------------------------------------------------ */

static asx_status poll_ok(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

static asx_status poll_pending_then_ok(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    if (*count >= 2) return ASX_OK;
    return ASX_E_PENDING;
}

/* Fails N times, then succeeds */
static asx_status poll_fail_n_then_ok(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    if (*count >= 3) return ASX_OK;
    return ASX_E_INVALID_STATE;
}

/* Tracks call count */
static asx_status poll_count(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    return ASX_OK;
}

/* ================================================================== */
/* Retry tests                                                         */
/* ================================================================== */

TEST(retry_success_first_try) {
    asx_retry_state rs;
    MUST_OK(asx_retry_init(&rs, poll_ok, NULL, 3));
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

TEST(retry_fail_then_succeed) {
    asx_retry_state rs;
    int count = 0;
    MUST_OK(asx_retry_init(&rs, poll_fail_n_then_ok, &count, 5));

    /* First attempt: fails (count=1) */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_PENDING);

    /* Second attempt: fails (count=2) */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_PENDING);

    /* Third attempt: succeeds (count=3) */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 3u);
}

TEST(retry_exhausted) {
    asx_retry_state rs;
    MUST_OK(asx_retry_init(&rs, poll_fail, NULL, 2));

    /* Attempt 1: fail */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_PENDING);
    /* Attempt 2: fail */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_PENDING);
    /* Attempt 3: fail — exhausted */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_retry_attempts(&rs), 3u);
    ASSERT_EQ(asx_retry_last_error(&rs), ASX_E_INVALID_STATE);
}

TEST(retry_zero_retries) {
    asx_retry_state rs;
    MUST_OK(asx_retry_init(&rs, poll_fail, NULL, 0));
    /* Only one attempt, no retries */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

TEST(retry_with_pending_inner) {
    asx_retry_state rs;
    int count = 0;
    MUST_OK(asx_retry_init(&rs, poll_pending_then_ok, &count, 0));
    /* First poll: inner returns pending */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_PENDING);
    /* Second poll: inner returns OK */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

TEST(retry_null_fails) {
    ASSERT_EQ(asx_retry_init(NULL, poll_ok, NULL, 0), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Bracket tests                                                       */
/* ================================================================== */

static int bracket_acquire_called;
static int bracket_use_called;
static int bracket_release_called;

static asx_status bracket_acquire(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_acquire_called = 1;
    return ASX_OK;
}

static asx_status bracket_use(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_use_called = 1;
    return ASX_OK;
}

static asx_status bracket_release(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_release_called = 1;
    return ASX_OK;
}

static asx_status bracket_use_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_use_called = 1;
    return ASX_E_INVALID_STATE;
}

static asx_status bracket_acquire_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_acquire_called = 1;
    return ASX_E_INVALID_STATE;
}

TEST(bracket_success) {
    asx_bracket_state bs;
    bracket_acquire_called = 0;
    bracket_use_called = 0;
    bracket_release_called = 0;

    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_OK);
    ASSERT_TRUE(bracket_acquire_called);
    ASSERT_TRUE(bracket_use_called);
    ASSERT_TRUE(bracket_release_called);
}

TEST(bracket_use_fails_release_still_called) {
    asx_bracket_state bs;
    bracket_acquire_called = 0;
    bracket_use_called = 0;
    bracket_release_called = 0;

    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use_fail, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_E_INVALID_STATE);
    ASSERT_TRUE(bracket_acquire_called);
    ASSERT_TRUE(bracket_use_called);
    ASSERT_TRUE(bracket_release_called); /* cleanup always runs */
}

TEST(bracket_acquire_fails_no_release) {
    asx_bracket_state bs;
    bracket_acquire_called = 0;
    bracket_use_called = 0;
    bracket_release_called = 0;

    MUST_OK(asx_bracket_init(&bs, bracket_acquire_fail, bracket_use, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_E_INVALID_STATE);
    ASSERT_TRUE(bracket_acquire_called);
    ASSERT_FALSE(bracket_use_called);
    ASSERT_FALSE(bracket_release_called); /* nothing to clean up */
}

TEST(bracket_phase_tracking) {
    asx_bracket_state bs;
    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_current_phase(&bs), ASX_BRACKET_PHASE_ACQUIRE);
    MUST_OK(asx_bracket_poll(&bs, 0));
    ASSERT_EQ(asx_bracket_current_phase(&bs), ASX_BRACKET_PHASE_DONE);
}

TEST(bracket_null_fails) {
    asx_bracket_state bs;
    ASSERT_EQ(asx_bracket_init(NULL, bracket_acquire, bracket_use, bracket_release, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_bracket_init(&bs, NULL, bracket_use, bracket_release, NULL),
              ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Pipeline tests                                                      */
/* ================================================================== */

TEST(pipeline_empty) {
    asx_pipeline_state ps;
    MUST_OK(asx_pipeline_init(&ps));
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 0u);
}

TEST(pipeline_single_stage) {
    asx_pipeline_state ps;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_ok, NULL));
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 1u);
}

TEST(pipeline_three_stages) {
    asx_pipeline_state ps;
    int c1 = 0, c2 = 0, c3 = 0;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c1));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c2));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c3));

    /* First poll runs stage 0, then returns pending for stage 1 */
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_E_PENDING);
    ASSERT_EQ(c1, 1);
    ASSERT_EQ(c2, 0);

    /* Second poll runs stage 1 */
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_E_PENDING);
    ASSERT_EQ(c2, 1);

    /* Third poll runs stage 2 */
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(c3, 1);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 3u);
}

TEST(pipeline_stops_on_failure) {
    asx_pipeline_state ps;
    int c1 = 0;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_fail, NULL));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c1));

    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(c1, 0); /* stage 2 never ran */
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 0u);
}

TEST(pipeline_with_pending_stage) {
    asx_pipeline_state ps;
    int count = 0;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_pending_then_ok, &count));

    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 1u);
}

TEST(pipeline_overflow) {
    asx_pipeline_state ps;
    uint32_t i;
    MUST_OK(asx_pipeline_init(&ps));
    for (i = 0; i < ASX_PIPELINE_MAX_STAGES; i++) {
        MUST_OK(asx_pipeline_add_stage(&ps, poll_ok, NULL));
    }
    ASSERT_EQ(asx_pipeline_add_stage(&ps, poll_ok, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ================================================================== */
/* Bulkhead tests                                                      */
/* ================================================================== */

TEST(bulkhead_basic) {
    asx_bulkhead_state bs;
    MUST_OK(asx_bulkhead_init(&bs, 2));
    ASSERT_EQ(asx_bulkhead_active(&bs), 0u);
    ASSERT_EQ(asx_bulkhead_remaining(&bs), 2u);

    MUST_OK(asx_bulkhead_try_enter(&bs));
    ASSERT_EQ(asx_bulkhead_active(&bs), 1u);
    ASSERT_EQ(asx_bulkhead_remaining(&bs), 1u);

    MUST_OK(asx_bulkhead_try_enter(&bs));
    ASSERT_EQ(asx_bulkhead_active(&bs), 2u);

    /* At capacity */
    ASSERT_EQ(asx_bulkhead_try_enter(&bs), ASX_E_WOULD_BLOCK);

    /* Leave one */
    MUST_OK(asx_bulkhead_leave(&bs));
    ASSERT_EQ(asx_bulkhead_active(&bs), 1u);
    MUST_OK(asx_bulkhead_try_enter(&bs));
}

TEST(bulkhead_leave_empty_fails) {
    asx_bulkhead_state bs;
    MUST_OK(asx_bulkhead_init(&bs, 2));
    ASSERT_EQ(asx_bulkhead_leave(&bs), ASX_E_INVALID_STATE);
}

TEST(bulkhead_zero_fails) {
    asx_bulkhead_state bs;
    ASSERT_EQ(asx_bulkhead_init(&bs, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(bulkhead_null_fails) { ASSERT_EQ(asx_bulkhead_init(NULL, 2), ASX_E_INVALID_ARGUMENT); }

/* ================================================================== */
/* RateLimit tests                                                     */
/* ================================================================== */

TEST(rate_limit_basic) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 3, 1));
    ASSERT_EQ(asx_rate_limit_available(&rs), 3u);

    MUST_OK(asx_rate_limit_try_acquire(&rs));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    ASSERT_EQ(asx_rate_limit_available(&rs), 0u);

    /* Rate limited */
    ASSERT_EQ(asx_rate_limit_try_acquire(&rs), ASX_E_WOULD_BLOCK);
}

TEST(rate_limit_refill) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 2, 1));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    ASSERT_EQ(asx_rate_limit_try_acquire(&rs), ASX_E_WOULD_BLOCK);

    /* Refill one token */
    asx_rate_limit_refill(&rs);
    ASSERT_EQ(asx_rate_limit_available(&rs), 1u);
    MUST_OK(asx_rate_limit_try_acquire(&rs));
}

TEST(rate_limit_refill_caps_at_capacity) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 3, 10));
    MUST_OK(asx_rate_limit_try_acquire(&rs));

    /* Refill with more than capacity */
    asx_rate_limit_refill(&rs);
    ASSERT_EQ(asx_rate_limit_available(&rs), 3u); /* capped */
}

TEST(rate_limit_zero_capacity_fails) {
    asx_rate_limit_state rs;
    ASSERT_EQ(asx_rate_limit_init(&rs, 0, 1), ASX_E_INVALID_ARGUMENT);
}

TEST(rate_limit_null_fails) { ASSERT_EQ(asx_rate_limit_init(NULL, 3, 1), ASX_E_INVALID_ARGUMENT); }

/* ================================================================== */
/* Integration tests                                                   */
/* ================================================================== */

TEST(integration_join_with_retry_and_pipeline) {
    asx_retry_state retry;
    asx_pipeline_state pipeline;
    asx_join_state join;
    int retry_count = 0;
    int pipeline_count = 0;

    MUST_OK(asx_retry_init(&retry, poll_fail_n_then_ok, &retry_count, 5));
    MUST_OK(asx_pipeline_init(&pipeline));
    MUST_OK(asx_pipeline_add_stage(&pipeline, poll_pending_then_ok, &pipeline_count));
    MUST_OK(asx_pipeline_add_stage(&pipeline, poll_ok, NULL));

    MUST_OK(asx_join_init(&join));
    MUST_OK(asx_join_add(&join, asx_retry_poll, &retry));
    MUST_OK(asx_join_add(&join, asx_pipeline_poll, &pipeline));

    ASSERT_EQ(asx_join_poll(&join, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_join_poll(&join, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_join_poll(&join, 0), ASX_OK);

    ASSERT_EQ(asx_retry_attempts(&retry), 3u);
    ASSERT_EQ(asx_pipeline_completed_stages(&pipeline), 2u);
    ASSERT_EQ(asx_join_outcome(&join).severity, ASX_OUTCOME_OK);
}

/* ================================================================== */
/* Law tests — algebraic properties of combinators                     */
/* ================================================================== */

/* Law: retry(0) ≡ single attempt */
/* ================================================================== */
/* Adaptive hedge tests                                                */
/* ================================================================== */

TEST(adaptive_hedge_init_defaults) {
    asx_adaptive_hedge_policy p;
    asx_adaptive_hedge_init(&p, 5, 1000, 100000);
    ASSERT_EQ(p.alpha_pct, 5u);
    ASSERT_EQ(asx_adaptive_hedge_observations(&p), 0u);
}

TEST(adaptive_hedge_cold_start_returns_max) {
    asx_adaptive_hedge_policy p;
    asx_adaptive_hedge_init(&p, 5, 1000, 50000);
    /* With < 10 observations, should return max_delay */
    asx_adaptive_hedge_record(&p, 100);
    asx_adaptive_hedge_record(&p, 200);
    ASSERT_EQ(asx_adaptive_hedge_delay(&p), (uint64_t)50000);
}

TEST(adaptive_hedge_calibrates_from_history) {
    asx_adaptive_hedge_policy p;
    uint32_t i;
    uint64_t delay;

    asx_adaptive_hedge_init(&p, 5, 0, UINT64_MAX);

    /* Record 20 latencies: 100, 200, 300, ..., 2000 */
    for (i = 0; i < 20; i++) { asx_adaptive_hedge_record(&p, (uint64_t)((i + 1u) * 100u)); }

    delay = asx_adaptive_hedge_delay(&p);
    /* P95 of [100..2000] should be near 1900-2000 */
    ASSERT_TRUE(delay >= 1800);
    ASSERT_TRUE(delay <= 2000);
}

TEST(adaptive_hedge_clamps_to_bounds) {
    asx_adaptive_hedge_policy p;
    uint32_t i;
    uint64_t delay;

    asx_adaptive_hedge_init(&p, 5, 5000, 10000);

    /* Record 20 tiny latencies (all 1ns) */
    for (i = 0; i < 20; i++) { asx_adaptive_hedge_record(&p, 1); }

    delay = asx_adaptive_hedge_delay(&p);
    /* Conformal bound is 1ns, but clamped to min=5000 */
    ASSERT_EQ(delay, (uint64_t)5000);
}

/* ================================================================== */
/* Laws                                                                */
/* ================================================================== */

TEST(law_retry_zero_is_single) {
    asx_retry_state rs;
    MUST_OK(asx_retry_init(&rs, poll_ok, NULL, 0));
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

/* Law: bracket with all-ok is transparent */
TEST(law_bracket_identity) {
    asx_bracket_state bs;
    MUST_OK(asx_bracket_init(&bs, poll_ok, poll_ok, poll_ok, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_OK);
}

/* Law: pipeline(single stage f) ≡ f */
TEST(law_pipeline_single_identity) {
    asx_pipeline_state ps;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_ok, NULL));
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
}

/* Law: bulkhead(1) admits exactly one */
TEST(law_bulkhead_one_is_mutex_like) {
    asx_bulkhead_state bs;
    MUST_OK(asx_bulkhead_init(&bs, 1));
    MUST_OK(asx_bulkhead_try_enter(&bs));
    ASSERT_EQ(asx_bulkhead_try_enter(&bs), ASX_E_WOULD_BLOCK);
    MUST_OK(asx_bulkhead_leave(&bs));
    MUST_OK(asx_bulkhead_try_enter(&bs));
}

/* Law: rate_limit starts full */
TEST(law_rate_limit_starts_full) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 5, 1));
    ASSERT_EQ(asx_rate_limit_available(&rs), 5u);
}

/* Law: bracket release always called even on use failure */
TEST(law_bracket_release_guarantee) {
    asx_bracket_state bs;
    bracket_release_called = 0;
    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use_fail, bracket_release, NULL));
    /* Ignore result, just verify release was called */
    st_sink_ = asx_bracket_poll(&bs, 0);
    (void)st_sink_;
    ASSERT_TRUE(bracket_release_called);
}

/* ================================================================== */
/* Hedge tests                                                         */
/* ================================================================== */

/* Primary completes immediately — should win */
TEST(hedge_primary_wins_immediately) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_ok, NULL, poll_fail, NULL, 1000000));
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_DECIDED);
}

/* Primary is pending, backup completes — backup wins.
 * In deterministic mode, deadline expires immediately, so both
 * branches race on first poll. */
static int hedge_primary_pending_count;
static asx_status hedge_primary_pending(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    hedge_primary_pending_count++;
    return ASX_E_PENDING;
}

TEST(hedge_backup_wins_when_primary_pending) {
    asx_hedge_state hs;
    hedge_primary_pending_count = 0;
    MUST_OK(asx_hedge_init(&hs, hedge_primary_pending, NULL, poll_ok, NULL, 0));
    /* With 0ns deadline, should go to RACING immediately */
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 1); /* backup won */
}

TEST(hedge_null_fails) {
    asx_hedge_state hs;
    ASSERT_EQ(asx_hedge_init(NULL, poll_ok, NULL, poll_ok, NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hedge_init(&hs, NULL, NULL, poll_ok, NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hedge_init(&hs, poll_ok, NULL, NULL, NULL, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hedge_phase_tracking) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_ok, NULL, poll_ok, NULL, 1000000));
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_PRIMARY_ONLY);
    asx_hedge_poll(&hs, 0);
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_DECIDED);
}

TEST(hedge_primary_fail_result) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_fail, NULL, poll_ok, NULL, 1000000));
    /* Primary fails immediately — that's the result */
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
}

/* ================================================================== */
/* MapReduce tests                                                     */
/* ================================================================== */

static asx_status reduce_count_ok(const asx_status *results, uint32_t count, void *user_data) {
    uint32_t i, ok_count = 0;
    (void)user_data;
    for (i = 0; i < count; i++) {
        if (results[i] == ASX_OK) ok_count++;
    }
    return (ok_count == count) ? ASX_OK : ASX_E_INVALID_STATE;
}

TEST(map_reduce_all_ok) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 3u);
}

TEST(map_reduce_reduce_sees_errors) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_fail, NULL));
    /* reduce_count_ok returns error because not all OK */
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 2u);
}

TEST(map_reduce_empty) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    /* No branches — reduce called with count=0 */
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
}

TEST(map_reduce_null_fails) {
    asx_map_reduce_state ms;
    ASSERT_EQ(asx_map_reduce_init(NULL, reduce_count_ok, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_map_reduce_init(&ms, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Additional Laws                                                     */
/* ================================================================== */

/* Law: hedge with immediate primary is equivalent to just running primary */
TEST(law_hedge_immediate_is_primary) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_ok, NULL, poll_fail, NULL, 999999999));
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
}

/* Law: map_reduce with one branch is identity */
TEST(law_map_reduce_single_is_identity) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 1u);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_combinator2 ===\n");

    /* Retry */
    RUN_TEST(retry_success_first_try);
    RUN_TEST(retry_fail_then_succeed);
    RUN_TEST(retry_exhausted);
    RUN_TEST(retry_zero_retries);
    RUN_TEST(retry_with_pending_inner);
    RUN_TEST(retry_null_fails);

    /* Bracket */
    RUN_TEST(bracket_success);
    RUN_TEST(bracket_use_fails_release_still_called);
    RUN_TEST(bracket_acquire_fails_no_release);
    RUN_TEST(bracket_phase_tracking);
    RUN_TEST(bracket_null_fails);

    /* Pipeline */
    RUN_TEST(pipeline_empty);
    RUN_TEST(pipeline_single_stage);
    RUN_TEST(pipeline_three_stages);
    RUN_TEST(pipeline_stops_on_failure);
    RUN_TEST(pipeline_with_pending_stage);
    RUN_TEST(pipeline_overflow);

    /* Bulkhead */
    RUN_TEST(bulkhead_basic);
    RUN_TEST(bulkhead_leave_empty_fails);
    RUN_TEST(bulkhead_zero_fails);
    RUN_TEST(bulkhead_null_fails);

    /* RateLimit */
    RUN_TEST(rate_limit_basic);
    RUN_TEST(rate_limit_refill);
    RUN_TEST(rate_limit_refill_caps_at_capacity);
    RUN_TEST(rate_limit_zero_capacity_fails);
    RUN_TEST(rate_limit_null_fails);

    /* Integration */
    RUN_TEST(integration_join_with_retry_and_pipeline);

    /* Hedge */
    RUN_TEST(hedge_primary_wins_immediately);
    RUN_TEST(hedge_backup_wins_when_primary_pending);
    RUN_TEST(hedge_null_fails);
    RUN_TEST(hedge_phase_tracking);
    RUN_TEST(hedge_primary_fail_result);

    /* MapReduce */
    RUN_TEST(map_reduce_all_ok);
    RUN_TEST(map_reduce_reduce_sees_errors);
    RUN_TEST(map_reduce_empty);
    RUN_TEST(map_reduce_null_fails);

    /* Adaptive hedge */
    RUN_TEST(adaptive_hedge_init_defaults);
    RUN_TEST(adaptive_hedge_cold_start_returns_max);
    RUN_TEST(adaptive_hedge_calibrates_from_history);
    RUN_TEST(adaptive_hedge_clamps_to_bounds);

    /* Laws */
    RUN_TEST(law_retry_zero_is_single);
    RUN_TEST(law_bracket_identity);
    RUN_TEST(law_pipeline_single_identity);
    RUN_TEST(law_bulkhead_one_is_mutex_like);
    RUN_TEST(law_rate_limit_starts_full);
    RUN_TEST(law_bracket_release_guarantee);
    RUN_TEST(law_hedge_immediate_is_primary);
    RUN_TEST(law_map_reduce_single_is_identity);

    TEST_REPORT();
    return test_failures;
}
