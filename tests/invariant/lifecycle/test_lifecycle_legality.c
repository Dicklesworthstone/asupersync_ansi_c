/*
 * test_lifecycle_legality.c — lifecycle legality invariant tests
 *
 * Verifies that forbidden state transitions are rejected and that
 * the lifecycle state machine enforces its contracts:
 *
 *   - Region: OPEN → CLOSING → DRAINING → FINALIZING → CLOSED
 *   - Task: CREATED → RUNNING → COMPLETED (with cancel variants)
 *   - Obligation: RESERVED → COMMITTED | ABORTED
 *
 * Each test resets state to ensure isolation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_log.h"
#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/transition.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static void reset_all(void)
{
    asx_runtime_reset();
    asx_ghost_reset();
}

static asx_status poll_ok(void *ud, asx_task_id self)
{
    (void)ud; (void)self;
    return ASX_OK;
}

static asx_status poll_pending(void *ud, asx_task_id self)
{
    (void)ud; (void)self;
    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * Region lifecycle legality
 * ------------------------------------------------------------------- */

TEST(region_double_close_rejected)
{
    asx_region_id rid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);

    /* Second close on a CLOSING region should fail */
    ASSERT_NE(asx_region_close(rid), ASX_OK);
}

TEST(region_close_on_invalid_handle_rejected)
{
    reset_all();

    /* Close with invalid handle */
    ASSERT_NE(asx_region_close(ASX_INVALID_ID), ASX_OK);
}

TEST(region_spawn_after_close_rejected)
{
    asx_region_id rid;
    asx_task_id tid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);

    /* Spawn in a CLOSING region should fail */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid),
              ASX_E_REGION_NOT_OPEN);
}

TEST(region_obligation_after_close_rejected)
{
    asx_region_id rid;
    asx_obligation_id oid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);

    /* Obligation reserve in CLOSING region should fail */
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_NOT_OPEN);
}

TEST(region_spawn_after_poison_rejected)
{
    asx_region_id rid;
    asx_task_id tid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Spawn in poisoned region should fail */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid),
              ASX_E_REGION_POISONED);
}

TEST(region_close_after_poison_rejected)
{
    asx_region_id rid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Close on poisoned region should fail */
    ASSERT_EQ(asx_region_close(rid), ASX_E_REGION_POISONED);
}

/* -------------------------------------------------------------------
 * Task lifecycle legality
 * ------------------------------------------------------------------- */

TEST(task_spawn_null_poll_rejected)
{
    asx_region_id rid;
    asx_task_id tid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* NULL poll function should fail */
    ASSERT_EQ(asx_task_spawn(rid, NULL, NULL, &tid),
              ASX_E_INVALID_ARGUMENT);
}

TEST(task_spawn_null_out_id_rejected)
{
    asx_region_id rid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* NULL out_id should fail */
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(task_get_outcome_before_complete_rejected)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome out;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Outcome query on non-terminal task should fail */
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_E_TASK_NOT_COMPLETED);
}

TEST(task_state_after_completion_is_completed)
{
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_ok, NULL, &tid), ASX_OK);

    {
        asx_budget budget = asx_budget_from_polls(10);
        ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    }

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
}

/* -------------------------------------------------------------------
 * Obligation lifecycle legality
 * ------------------------------------------------------------------- */

TEST(obligation_double_commit_rejected)
{
    asx_region_id rid;
    asx_obligation_id oid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    /* Second commit should fail (already in COMMITTED state) */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
}

TEST(obligation_commit_then_abort_rejected)
{
    asx_region_id rid;
    asx_obligation_id oid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    /* Abort after commit should fail */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);
}

TEST(obligation_abort_then_commit_rejected)
{
    asx_region_id rid;
    asx_obligation_id oid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    /* Commit after abort should fail */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
}

TEST(obligation_double_abort_rejected)
{
    asx_region_id rid;
    asx_obligation_id oid;

    reset_all();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    /* Second abort should fail */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);
}

TEST(obligation_invalid_handle_rejected)
{
    reset_all();

    ASSERT_NE(asx_obligation_commit(ASX_INVALID_ID), ASX_OK);
    ASSERT_NE(asx_obligation_abort(ASX_INVALID_ID), ASX_OK);
}

/* -------------------------------------------------------------------
 * Transition table smoke checks
 * ------------------------------------------------------------------- */

TEST(region_transition_open_to_closing_legal)
{
    ASSERT_EQ(asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSING),
              ASX_OK);
}

TEST(region_transition_open_to_closed_illegal)
{
    /* Direct OPEN → CLOSED skips intermediate states */
    ASSERT_NE(asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSED),
              ASX_OK);
}

TEST(region_transition_closed_to_open_illegal)
{
    /* CLOSED is terminal — no forward transitions */
    ASSERT_NE(asx_region_transition_check(ASX_REGION_CLOSED, ASX_REGION_OPEN),
              ASX_OK);
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    test_log_open("invariant", "lifecycle/legality", "test_lifecycle_legality");

    /* Region legality */
    RUN_TEST(region_double_close_rejected);
    RUN_TEST(region_close_on_invalid_handle_rejected);
    RUN_TEST(region_spawn_after_close_rejected);
    RUN_TEST(region_obligation_after_close_rejected);
    RUN_TEST(region_spawn_after_poison_rejected);
    RUN_TEST(region_close_after_poison_rejected);

    /* Task legality */
    RUN_TEST(task_spawn_null_poll_rejected);
    RUN_TEST(task_spawn_null_out_id_rejected);
    RUN_TEST(task_get_outcome_before_complete_rejected);
    RUN_TEST(task_state_after_completion_is_completed);

    /* Obligation legality */
    RUN_TEST(obligation_double_commit_rejected);
    RUN_TEST(obligation_commit_then_abort_rejected);
    RUN_TEST(obligation_abort_then_commit_rejected);
    RUN_TEST(obligation_double_abort_rejected);
    RUN_TEST(obligation_invalid_handle_rejected);

    /* Transition table checks */
    RUN_TEST(region_transition_open_to_closing_legal);
    RUN_TEST(region_transition_open_to_closed_illegal);
    RUN_TEST(region_transition_closed_to_open_illegal);

    TEST_REPORT();
    return test_failures;
}
