/*
 * test_outcome.c — unit tests for outcome severity lattice
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/outcome.h>
#include <string.h>

TEST(outcome_severity_of_null) { ASSERT_EQ(asx_outcome_severity_of(NULL), ASX_OUTCOME_OK); }

TEST(outcome_severity_of_ok) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_OK);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_OK);
}

TEST(outcome_severity_of_err) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_ERR);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_ERR);
}

TEST(outcome_severity_of_cancelled) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_CANCELLED);
}

TEST(outcome_severity_of_panicked) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_PANICKED);
}

TEST(outcome_lattice_order) {
    ASSERT_TRUE(ASX_OUTCOME_OK < ASX_OUTCOME_ERR);
    ASSERT_TRUE(ASX_OUTCOME_ERR < ASX_OUTCOME_CANCELLED);
    ASSERT_TRUE(ASX_OUTCOME_CANCELLED < ASX_OUTCOME_PANICKED);
}

TEST(outcome_join_max_severity) {
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome result = asx_outcome_join(&ok, &err);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
}

TEST(outcome_join_left_bias) {
    /* Equal severity: left operand wins */
    asx_outcome a = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome b = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome result = asx_outcome_join(&a, &b);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
}

TEST(outcome_join_null_identity) {
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome result = asx_outcome_join(NULL, &err);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
    result = asx_outcome_join(&err, NULL);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
}

TEST(outcome_join_both_null) {
    asx_outcome result = asx_outcome_join(NULL, NULL);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_OK);
}

TEST(outcome_join_associative) {
    /* (a ⊔ b) ⊔ c == a ⊔ (b ⊔ c) */
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome can = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome ab = asx_outcome_join(&ok, &err);
    asx_outcome lhs = asx_outcome_join(&ab, &can);
    asx_outcome bc = asx_outcome_join(&err, &can);
    asx_outcome rhs = asx_outcome_join(&ok, &bc);
    ASSERT_EQ(asx_outcome_severity_of(&lhs), asx_outcome_severity_of(&rhs));
}

TEST(outcome_join_commutative_severity) {
    /* join(a,b) and join(b,a) yield same severity */
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome pan = asx_outcome_make(ASX_OUTCOME_PANICKED);
    asx_outcome ab = asx_outcome_join(&err, &pan);
    asx_outcome ba = asx_outcome_join(&pan, &err);
    ASSERT_EQ(asx_outcome_severity_of(&ab), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&ba), ASX_OUTCOME_PANICKED);
}

TEST(outcome_join_panicked_dominates) {
    /* Panicked wins against every other severity */
    asx_outcome pan = asx_outcome_make(ASX_OUTCOME_PANICKED);
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome can = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome r1, r2, r3, r4;
    r1 = asx_outcome_join(&pan, &ok);
    r2 = asx_outcome_join(&pan, &err);
    r3 = asx_outcome_join(&pan, &can);
    r4 = asx_outcome_join(&ok, &pan);
    ASSERT_EQ(asx_outcome_severity_of(&r1), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&r2), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&r3), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&r4), ASX_OUTCOME_PANICKED);
}

TEST(outcome_join_idempotent) {
    /* join(x, x) == x for all severity levels */
    asx_outcome_severity levels[] = {ASX_OUTCOME_OK, ASX_OUTCOME_ERR, ASX_OUTCOME_CANCELLED,
                                     ASX_OUTCOME_PANICKED};
    int i;
    for (i = 0; i < 4; i++) {
        asx_outcome x = asx_outcome_make(levels[i]);
        asx_outcome r = asx_outcome_join(&x, &x);
        ASSERT_EQ(asx_outcome_severity_of(&r), levels[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Payload: Cancelled(reason) and Panicked(message) (bd-9kll.3.14)     */
/* ------------------------------------------------------------------ */

static asx_cancel_reason reason_of(asx_cancel_kind kind, asx_time ts, const char *message) {
    asx_cancel_reason r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.origin_region = ASX_INVALID_ID;
    r.origin_task = ASX_INVALID_ID;
    r.timestamp = ts;
    r.message = message;
    return r;
}

TEST(outcome_make_carries_no_payload) {
    asx_outcome c = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome p = asx_outcome_make(ASX_OUTCOME_PANICKED);
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);

    /* The lattice-only constructor leaves a payload that is all zero. */
    ASSERT_EQ(c.cancel_reason.kind, ASX_CANCEL_USER);
    ASSERT_EQ(c.cancel_reason.origin_region, ASX_INVALID_ID);
    ASSERT_EQ(c.cancel_reason.origin_task, ASX_INVALID_ID);
    ASSERT_EQ(c.cancel_reason.timestamp, (asx_time)0u);
    ASSERT_TRUE(c.cancel_reason.message == NULL);
    ASSERT_TRUE(c.cancel_reason.cause == NULL);
    ASSERT_TRUE(c.panic_message == NULL);
    ASSERT_TRUE(p.panic_message == NULL);
    ASSERT_TRUE(ok.panic_message == NULL);
    ASSERT_EQ(ok.cancel_reason.timestamp, (asx_time)0u);
}

TEST(outcome_cancelled_and_panicked_constructors_carry_their_payload) {
    asx_cancel_reason r = reason_of(ASX_CANCEL_TIMEOUT, 7000u, "too slow");
    asx_outcome c = asx_outcome_cancelled(&r);
    asx_outcome p = asx_outcome_panicked("boom");
    asx_outcome u = asx_outcome_panicked(NULL);
    asx_outcome z = asx_outcome_cancelled(NULL);

    ASSERT_EQ(c.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(c.cancel_reason.kind, ASX_CANCEL_TIMEOUT);
    ASSERT_EQ(c.cancel_reason.timestamp, (asx_time)7000u);
    ASSERT_TRUE(c.cancel_reason.message == r.message);
    ASSERT_TRUE(c.panic_message == NULL);

    ASSERT_EQ(p.severity, ASX_OUTCOME_PANICKED);
    ASSERT_STR_EQ(p.panic_message, "boom");
    ASSERT_EQ(p.cancel_reason.timestamp, (asx_time)0u);

    /* A non-string payload is "unknown panic", as in Rust. */
    ASSERT_STR_EQ(u.panic_message, "unknown panic");
    /* A NULL reason is the zero reason. */
    ASSERT_EQ(z.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(z.cancel_reason.timestamp, (asx_time)0u);
}

TEST(outcome_join_of_cancelled_strengthens_to_the_more_severe_kind) {
    /* kind severities 0 and 5 */
    asx_cancel_reason weak = reason_of(ASX_CANCEL_USER, 500u, "weak");
    asx_cancel_reason strong = reason_of(ASX_CANCEL_SHUTDOWN, 900u, "strong");
    asx_outcome a = asx_outcome_cancelled(&weak);
    asx_outcome b = asx_outcome_cancelled(&strong);
    asx_outcome ab = asx_outcome_join(&a, &b);
    asx_outcome ba = asx_outcome_join(&b, &a);

    /* The stronger reason wins whichever side it is on (Rust strengthens
     * the left reason when the right is strictly more severe), whole:
     * kind, timestamp and message travel together. */
    ASSERT_EQ(ab.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(ab.cancel_reason.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(ab.cancel_reason.timestamp, (asx_time)900u);
    ASSERT_STR_EQ(ab.cancel_reason.message, "strong");
    ASSERT_EQ(ba.cancel_reason.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_STR_EQ(ba.cancel_reason.message, "strong");
}

TEST(outcome_join_of_cancelled_at_equal_severity_stays_left) {
    /* TIMEOUT and DEADLINE are both severity 1. The right one is earlier,
     * which asx_cancel_strengthen alone would prefer, but Rust's join only
     * strengthens on a strictly more severe kind: the left stays. */
    asx_cancel_reason left = reason_of(ASX_CANCEL_TIMEOUT, 100u, "left");
    asx_cancel_reason right = reason_of(ASX_CANCEL_DEADLINE, 1u, "right");
    asx_outcome a = asx_outcome_cancelled(&left);
    asx_outcome b = asx_outcome_cancelled(&right);
    asx_outcome ab = asx_outcome_join(&a, &b);
    asx_outcome ba = asx_outcome_join(&b, &a);

    ASSERT_EQ(ab.cancel_reason.kind, ASX_CANCEL_TIMEOUT);
    ASSERT_STR_EQ(ab.cancel_reason.message, "left");
    ASSERT_EQ(ba.cancel_reason.kind, ASX_CANCEL_DEADLINE);
    ASSERT_STR_EQ(ba.cancel_reason.message, "right");
}

TEST(outcome_join_returns_the_winners_payload_whole) {
    asx_cancel_reason r = reason_of(ASX_CANCEL_RACE_LOST, 40u, "lost");
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome can = asx_outcome_cancelled(&r);
    asx_outcome pan_a = asx_outcome_panicked("first");
    asx_outcome pan_b = asx_outcome_panicked("second");
    asx_outcome j;

    /* Cancelled beats Ok and Err, on either side, with its reason. */
    j = asx_outcome_join(&ok, &can);
    ASSERT_EQ(j.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_STR_EQ(j.cancel_reason.message, "lost");
    j = asx_outcome_join(&can, &err);
    ASSERT_EQ(j.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_STR_EQ(j.cancel_reason.message, "lost");

    /* Panicked beats Cancelled and takes only its own payload. */
    j = asx_outcome_join(&can, &pan_a);
    ASSERT_EQ(j.severity, ASX_OUTCOME_PANICKED);
    ASSERT_STR_EQ(j.panic_message, "first");
    ASSERT_EQ(j.cancel_reason.timestamp, (asx_time)0u);

    /* Two panics: the left one, as Rust's join keeps the left. */
    j = asx_outcome_join(&pan_a, &pan_b);
    ASSERT_STR_EQ(j.panic_message, "first");
    j = asx_outcome_join(&pan_b, &pan_a);
    ASSERT_STR_EQ(j.panic_message, "second");
}

TEST(outcome_join_with_payload_is_associative) {
    /* severities 1, 3, 2: the most severe reason wins, whatever the
     * grouping. */
    asx_cancel_reason r1 = reason_of(ASX_CANCEL_TIMEOUT, 10u, "r1");
    asx_cancel_reason r2 = reason_of(ASX_CANCEL_FAIL_FAST, 20u, "r2");
    asx_cancel_reason r3 = reason_of(ASX_CANCEL_POLL_QUOTA, 30u, "r3");
    asx_outcome a = asx_outcome_cancelled(&r1);
    asx_outcome b = asx_outcome_cancelled(&r2);
    asx_outcome c = asx_outcome_cancelled(&r3);
    asx_outcome ab = asx_outcome_join(&a, &b);
    asx_outcome bc = asx_outcome_join(&b, &c);
    asx_outcome lhs = asx_outcome_join(&ab, &c);
    asx_outcome rhs = asx_outcome_join(&a, &bc);

    ASSERT_STR_EQ(lhs.cancel_reason.message, "r2");
    ASSERT_STR_EQ(rhs.cancel_reason.message, "r2");
    ASSERT_EQ(lhs.cancel_reason.kind, rhs.cancel_reason.kind);
    ASSERT_EQ(lhs.cancel_reason.timestamp, rhs.cancel_reason.timestamp);
}

/* ---- The runtime carries the payload through completion ---- */

static asx_status poll_acknowledge_cancel(void *ud, asx_task_id self) {
    asx_checkpoint_result cp;
    (void)ud;
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) return ASX_OK;
    return ASX_E_PENDING;
}

static asx_status poll_panic(void *ud, asx_task_id self) {
    (void)ud;
    if (asx_task_panic(self, "task body panicked") != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

TEST(task_outcome_carries_the_reason_it_was_cancelled_with) {
    asx_region_id rid;
    asx_task_id tid;
    asx_cancel_reason reason = asx_cancel_reason_default(ASX_CANCEL_TIMEOUT, "too slow");
    asx_cancel_reason recorded;
    asx_outcome out;
    asx_budget run;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_acknowledge_cancel, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel_with_reason(tid, &reason), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(out.cancel_reason.kind, ASX_CANCEL_TIMEOUT);
    ASSERT_EQ(out.cancel_reason.timestamp, reason.timestamp);
    ASSERT_TRUE(out.cancel_reason.message == reason.message);
    ASSERT_TRUE(out.panic_message == NULL);

    /* It is the very reason the task's record reports. */
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &recorded), ASX_OK);
    ASSERT_EQ(out.cancel_reason.kind, recorded.kind);
    ASSERT_EQ(out.cancel_reason.origin_region, recorded.origin_region);
    ASSERT_EQ(out.cancel_reason.timestamp, recorded.timestamp);
}

TEST(task_join_returns_the_panic_message) {
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome out;
    asx_budget run;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_panic, NULL, &tid), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);

    ASSERT_EQ(asx_task_join(tid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_PANICKED);
    ASSERT_STR_EQ(out.panic_message, "task body panicked");
    ASSERT_EQ(out.cancel_reason.timestamp, (asx_time)0u);
}

TEST(region_close_outcome_joins_member_payloads) {
    asx_region_id rid;
    asx_task_id cancelled;
    asx_task_id panicking;
    asx_cancel_reason reason = asx_cancel_reason_default(ASX_CANCEL_USER, "stop");
    asx_outcome out;
    asx_budget run;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_acknowledge_cancel, NULL, &cancelled), ASX_OK);
    ASSERT_EQ(asx_task_cancel_with_reason(cancelled, &reason), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);

    /* One cancelled task: the region's outcome is its cancellation. */
    ASSERT_EQ(asx_region_get_close_outcome(rid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(out.cancel_reason.kind, ASX_CANCEL_USER);
    ASSERT_TRUE(out.cancel_reason.message == reason.message);

    /* A panicking sibling outranks it, with its message. */
    ASSERT_EQ(asx_task_spawn(rid, poll_panic, NULL, &panicking), ASX_OK);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_PANICKED);
    ASSERT_STR_EQ(out.panic_message, "task body panicked");
}

TEST(cancelled_region_without_tasks_reports_its_own_reason) {
    asx_region_id rid;
    asx_cancel_reason reason = asx_cancel_reason_default(ASX_CANCEL_SHUTDOWN, "going down");
    asx_outcome out;
    uint32_t reached = 99u;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_cancel(rid, &reason, &reached), ASX_OK);
    ASSERT_EQ(reached, 0u);

    /* No task recorded an outcome: Rust's complete_close defaults a
     * cancelled region to Cancelled with the region's reason. */
    ASSERT_EQ(asx_region_get_close_outcome(rid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(out.cancel_reason.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(out.cancel_reason.message == reason.message);
}

int main(void) {
    fprintf(stderr, "=== test_outcome ===\n");
    RUN_TEST(outcome_severity_of_null);
    RUN_TEST(outcome_severity_of_ok);
    RUN_TEST(outcome_severity_of_err);
    RUN_TEST(outcome_severity_of_cancelled);
    RUN_TEST(outcome_severity_of_panicked);
    RUN_TEST(outcome_lattice_order);
    RUN_TEST(outcome_join_max_severity);
    RUN_TEST(outcome_join_left_bias);
    RUN_TEST(outcome_join_null_identity);
    RUN_TEST(outcome_join_both_null);
    RUN_TEST(outcome_join_associative);
    RUN_TEST(outcome_join_commutative_severity);
    RUN_TEST(outcome_join_panicked_dominates);
    RUN_TEST(outcome_join_idempotent);
    RUN_TEST(outcome_make_carries_no_payload);
    RUN_TEST(outcome_cancelled_and_panicked_constructors_carry_their_payload);
    RUN_TEST(outcome_join_of_cancelled_strengthens_to_the_more_severe_kind);
    RUN_TEST(outcome_join_of_cancelled_at_equal_severity_stays_left);
    RUN_TEST(outcome_join_returns_the_winners_payload_whole);
    RUN_TEST(outcome_join_with_payload_is_associative);
    RUN_TEST(task_outcome_carries_the_reason_it_was_cancelled_with);
    RUN_TEST(task_join_returns_the_panic_message);
    RUN_TEST(region_close_outcome_joins_member_payloads);
    RUN_TEST(cancelled_region_without_tasks_reports_its_own_reason);
    TEST_REPORT();
    return test_failures;
}
