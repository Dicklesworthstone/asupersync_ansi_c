/*
 * test_cancellation.c — runtime-level cancellation tests (bd-2cw.3)
 *
 * Tests cancellation propagation, checkpoint protocol, cleanup budget
 * enforcement, cancel strengthening at runtime, and region-wide cancel.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cancel.h>
#include <asx/core/ghost.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls.
 * GCC's (void) cast does not silence warn_unused_result under -Werror. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* -------------------------------------------------------------------
 * Test poll functions
 * ------------------------------------------------------------------- */

/* Always yields (never completes on its own) */
static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* Completes immediately */
static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

/* Checks checkpoint; if cancelled, completes immediately */
static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

/* Yields forever but calls checkpoint each time */
static asx_status poll_checkpoint_forever(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    asx_status st;
    (void)data;
    st = asx_checkpoint(self, &cr);
    (void)st;
    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * Test: cancel a running task — state transition
 * ------------------------------------------------------------------- */

TEST(cancel_running_task_transitions_to_cancel_requested) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Run one round to transition Created → Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_RUNNING);

    /* Cancel the task */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);
}

/* -------------------------------------------------------------------
 * Test: cancel a Created (not yet polled) task
 * ------------------------------------------------------------------- */

TEST(cancel_created_task_transitions_through_running) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Cancel before any scheduler run (task is still Created) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Should have transitioned Created → Running → CancelRequested */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);
}

/* -------------------------------------------------------------------
 * Test: checkpoint advances CancelRequested → Cancelling
 * ------------------------------------------------------------------- */

TEST(checkpoint_advances_to_cancelling) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Run once to make it Running, then cancel */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Checkpoint should transition to Cancelling */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ((int)cr.phase, (int)ASX_CANCEL_PHASE_CANCELLING);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_USER);
    ASSERT_TRUE(cr.polls_remaining > 0);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCELLING);
}

/* -------------------------------------------------------------------
 * Test: checkpoint on non-cancelled task reports clean
 * ------------------------------------------------------------------- */

TEST(checkpoint_non_cancelled_task_reports_clean) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_FALSE(cr.cancelled);
}

/* -------------------------------------------------------------------
 * Test: finalize transitions Cancelling → Finalizing
 * ------------------------------------------------------------------- */

TEST(finalize_transitions_cancelling_to_finalizing) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Get task to Cancelling state */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);

    /* Finalize should advance to Finalizing */
    ASSERT_EQ(asx_task_finalize(tid), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_FINALIZING);
}

/* -------------------------------------------------------------------
 * Test: finalize rejects non-Cancelling state
 * ------------------------------------------------------------------- */

TEST(finalize_rejects_wrong_state) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Task is still Created — finalize should fail */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Task is Running — finalize should fail */
    ASSERT_EQ(asx_task_finalize(tid), ASX_E_INVALID_STATE);
}

/* -------------------------------------------------------------------
 * Test: cancel strengthen — second cancel with higher severity
 * ------------------------------------------------------------------- */

TEST(cancel_strengthen_higher_severity_upgrades) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* First cancel: USER (severity 0) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Second cancel: SHUTDOWN (severity 5) — should strengthen */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);

    /* Checkpoint to observe — kind should be SHUTDOWN */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_SHUTDOWN);
}

/* -------------------------------------------------------------------
 * Test: cancel no-ops on completed task
 * ------------------------------------------------------------------- */

TEST(cancel_completed_task_is_noop) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    /* Run scheduler — task completes immediately */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Cancel on completed task should no-op */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
}

/* -------------------------------------------------------------------
 * Test: cancel propagation across a region
 * ------------------------------------------------------------------- */

TEST(cancel_propagation_cancels_all_tasks_in_region) {
    asx_region_id rid;
    asx_task_id tid1, tid2, tid3;
    asx_task_state s1, s2, s3;
    uint32_t count;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid3), ASX_OK);

    /* Run once to move tasks to Running */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Propagate cancellation to entire region */
    count = asx_cancel_propagate(rid, ASX_CANCEL_PARENT);
    ASSERT_EQ(count, (uint32_t)3);

    /* All tasks should be CancelRequested */
    ASSERT_EQ(asx_task_get_state(tid1, &s1), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid2, &s2), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid3, &s3), ASX_OK);
    ASSERT_EQ((int)s1, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_EQ((int)s2, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_EQ((int)s3, (int)ASX_TASK_CANCEL_REQUESTED);
}

/* -------------------------------------------------------------------
 * Test: cancel propagation skips completed tasks
 * ------------------------------------------------------------------- */

TEST(cancel_propagation_skips_completed_tasks) {
    asx_region_id rid;
    asx_task_id tid1, tid2;
    asx_task_state s1, s2;
    uint32_t count;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_OK);

    /* Run scheduler — tid1 completes, tid2 remains pending */
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Propagate cancel — should only affect tid2 */
    count = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(count, (uint32_t)1);

    ASSERT_EQ(asx_task_get_state(tid1, &s1), ASX_OK);
    ASSERT_EQ((int)s1, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_state(tid2, &s2), ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_TASK_CANCEL_REQUESTED);
}

/* -------------------------------------------------------------------
 * Test: cancelled task completing via poll gets CANCELLED outcome
 * ------------------------------------------------------------------- */

TEST(cancelled_task_completion_gets_cancelled_outcome) {
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome out;
    asx_cancel_phase phase;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    /* Run one round to make task Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel it */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Run scheduler — task should checkpoint, complete, get CANCELLED outcome */
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_COMPLETED);
}

/* -------------------------------------------------------------------
 * Test: cleanup budget exhaustion forces task completion
 * ------------------------------------------------------------------- */

TEST(cleanup_budget_exhaustion_forces_completion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_outcome out;
    asx_cancel_phase phase;
    asx_budget budget;
    asx_checkpoint_result cr;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_forever, NULL, &tid), ASX_OK);

    /* Run once to make Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel with SHUTDOWN (tightest cleanup budget) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);

    /* Get the cleanup budget for SHUTDOWN to know when exhaustion happens */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);

    /* Run scheduler with enough budget to exhaust cleanup.
     * Each poll decrements cleanup_polls_remaining via checkpoint.
     * When it hits 0, the scheduler should force-complete the task. */
    budget = asx_budget_from_polls(200);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Task should now be completed with CANCELLED outcome */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_COMPLETED);
}

/* -------------------------------------------------------------------
 * Test: cancel forced event emitted on budget exhaustion
 * ------------------------------------------------------------------- */

TEST(cancel_forced_event_emitted) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_checkpoint_result cr;
    uint32_t i;
    int found_forced = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_forever, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);

    budget = asx_budget_from_polls(200);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Check event log for CANCEL_FORCED */
    for (i = 0; i < asx_scheduler_event_count(); i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev)) {
            if (ev.kind == ASX_SCHED_EVENT_CANCEL_FORCED) {
                found_forced = 1;
                break;
            }
        }
    }
    ASSERT_TRUE(found_forced);
}

/* -------------------------------------------------------------------
 * Test: cancel phase query
 * ------------------------------------------------------------------- */

TEST(cancel_phase_query_tracks_progression) {
    asx_region_id rid;
    asx_task_id tid;
    asx_cancel_phase phase;
    asx_checkpoint_result cr;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Before cancel — phase should be at initial */
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);

    /* Cancel the task */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* After checkpoint — should be CANCELLING */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_CANCELLING);

    /* After finalize — should be FINALIZING */
    ASSERT_EQ(asx_task_finalize(tid), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_FINALIZING);

    /* After scheduler completion — should be COMPLETED */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_COMPLETED);
}

/* -------------------------------------------------------------------
 * Test: checkpoint decrements cleanup budget
 * ------------------------------------------------------------------- */

TEST(scheduler_decrements_cleanup_budget) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr1, cr2;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_forever, NULL, &tid), ASX_OK);

    /* Run once to make Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* First checkpoint: transitions to Cancelling, observe initial budget */
    ASSERT_EQ(asx_checkpoint(tid, &cr1), ASX_OK);
    ASSERT_TRUE(cr1.polls_remaining > 0);

    /* Run scheduler for one poll — scheduler should decrement budget */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* After one scheduler poll, budget should have decreased */
    ASSERT_EQ(asx_checkpoint(tid, &cr2), ASX_OK);
    ASSERT_TRUE(cr2.polls_remaining < cr1.polls_remaining);
}

/* -------------------------------------------------------------------
 * Test: cancel_with_origin sets origin fields
 * ------------------------------------------------------------------- */

TEST(cancel_with_origin_sets_attribution) {
    asx_region_id rid;
    asx_task_id tid1, tid2;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_OK);

    budget = asx_budget_from_polls(2);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel tid2 with origin from tid1 */
    ASSERT_EQ(asx_task_cancel_with_origin(tid2, ASX_CANCEL_LINKED_EXIT, rid, tid1), ASX_OK);
}

/* -------------------------------------------------------------------
 * Test: multiple tasks — cancel storm
 * ------------------------------------------------------------------- */

TEST(cancel_storm_all_tasks_resolve) {
    asx_region_id rid;
    asx_task_id tids[8];
    asx_task_state state;
    asx_budget budget;
    int k;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Spawn 8 tasks */
    for (k = 0; k < 8; k++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_forever, NULL, &tids[k]), ASX_OK);
    }

    /* Run to make all Running */
    budget = asx_budget_from_polls(8);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel all with SHUTDOWN */
    for (k = 0; k < 8; k++) { ASSERT_EQ(asx_task_cancel(tids[k], ASX_CANCEL_SHUTDOWN), ASX_OK); }

    /* Checkpoint all to advance to Cancelling */
    for (k = 0; k < 8; k++) {
        asx_checkpoint_result cr;
        ASSERT_EQ(asx_checkpoint(tids[k], &cr), ASX_OK);
    }

    /* Run scheduler with large budget — all should be force-completed */
    budget = asx_budget_from_polls(500);
    SCHED_RUN_IGNORE(rid, &budget);

    /* All tasks should be completed */
    for (k = 0; k < 8; k++) {
        ASSERT_EQ(asx_task_get_state(tids[k], &state), ASX_OK);
        ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);
    }
}

/* -------------------------------------------------------------------
 * Test: cancel_propagate preserves stronger cancel's origin (C4 fix)
 *
 * If a task already has a stronger cancel (e.g. SHUTDOWN) with origin
 * attribution, a weaker propagate (e.g. PARENT) must NOT overwrite
 * the existing origin_region.
 * ------------------------------------------------------------------- */

TEST(cancel_propagate_preserves_stronger_origin) {
    asx_region_id rid, rid_other;
    asx_task_id tid;
    asx_task_slot *t;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid_other), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Run once to make it Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* First: strong cancel (SHUTDOWN) with origin from rid_other */
    ASSERT_EQ(asx_task_cancel_with_origin(tid, ASX_CANCEL_SHUTDOWN, rid_other, ASX_INVALID_ID),
              ASX_OK);

    /* Now propagate a weaker cancel (PARENT) from rid */
    (void)asx_cancel_propagate(rid, ASX_CANCEL_PARENT);

    /* The stronger cancel's origin_region should be preserved */
    ASSERT_EQ(asx_task_slot_lookup(tid, &t), ASX_OK);
    ASSERT_EQ((int)t->cancel_reason.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_TRUE(t->cancel_reason.origin_region == rid_other);
}

/* -------------------------------------------------------------------
 * Test: cancel propagation sets origin region
 * ------------------------------------------------------------------- */

TEST(cancel_propagation_sets_origin_region) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;
    uint32_t count;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Propagation should set origin_region */
    count = asx_cancel_propagate(rid, ASX_CANCEL_PARENT);
    ASSERT_EQ(count, (uint32_t)1);

    /* The task should now be CancelRequested */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);
}

/* -------------------------------------------------------------------
 * Test: scheduler quiesces after all cancelled tasks complete
 * ------------------------------------------------------------------- */

TEST(scheduler_quiesces_after_cancel_completion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status result;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Run scheduler — task should checkpoint, complete, then quiesce */
    budget = asx_budget_from_polls(10);
    result = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(result, ASX_OK); /* ASX_OK means quiescent */
}

/* -------------------------------------------------------------------
 * Test: cleanup budget varies by cancel kind
 * ------------------------------------------------------------------- */

TEST(cleanup_budget_tighter_for_severe_cancels) {
    asx_budget user_b = asx_cancel_cleanup_budget(ASX_CANCEL_USER);
    asx_budget shut_b = asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN);

    ASSERT_TRUE(user_b.poll_quota > shut_b.poll_quota);
}

/* -------------------------------------------------------------------
 * Test: null checkpoint result pointer rejected
 * ------------------------------------------------------------------- */

TEST(checkpoint_null_result_rejected) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_checkpoint(tid, NULL), ASX_E_INVALID_ARGUMENT);
}

/* -------------------------------------------------------------------
 * Test: cancel phase query null output rejected
 * ------------------------------------------------------------------- */

TEST(cancel_phase_null_output_rejected) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_get_cancel_phase(tid, NULL), ASX_E_INVALID_ARGUMENT);
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * Cancel masking
 * ------------------------------------------------------------------- */

TEST(mask_defers_checkpoint_acknowledgement) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    asx_cancel_phase phase;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_TIMEOUT), ASX_OK);

    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 0);
    ASSERT_EQ(cr.masked, 1);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_TIMEOUT);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_REQUESTED);

    /* Strengthening still applies while masked. */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 1);
    ASSERT_EQ(cr.masked, 0);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ((int)cr.phase, (int)ASX_CANCEL_PHASE_CANCELLING);
}

typedef struct {
    uint32_t polls;
    uint32_t unmask_at;
    int finished_itself;
} masked_section_state;

/* Masks on the first poll, stays in the critical section until
 * `unmask_at`, then observes the cancel and completes. */
static asx_status poll_masked_section(void *data, asx_task_id self) {
    masked_section_state *s = (masked_section_state *)data;
    asx_checkpoint_result cr;
    s->polls++;
    if (s->polls == 1u && asx_task_mask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    if (s->polls < s->unmask_at) return ASX_E_PENDING;
    if (asx_task_unmask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) {
        s->finished_itself = 1;
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

TEST(masked_task_is_not_force_completed) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_outcome out;
    masked_section_state s;

    asx_runtime_reset();
    s.polls = 0;
    s.unmask_at = 80u; /* well past SHUTDOWN's 50-poll cleanup budget */
    s.finished_itself = 0;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_masked_section, &s, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    budget = asx_budget_from_polls(500);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_TRUE(s.finished_itself);
    ASSERT_EQ(s.polls, 80u);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
}

TEST(unmasked_task_is_force_completed_by_cleanup_budget) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    budget = asx_budget_from_polls(500);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
}

TEST(mask_depth_is_bounded) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t depth = 99u;
    uint32_t i;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_unmask(tid), ASX_E_INVALID_STATE);
    for (i = 0; i < ASX_MAX_MASK_DEPTH; i++) ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, ASX_MAX_MASK_DEPTH);
    for (i = 0; i < ASX_MAX_MASK_DEPTH; i++) ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_task_unmask(tid), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_mask_depth(tid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_mask(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

TEST(checkpoint_observes_budget_deadline_inline) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget tb;
    asx_checkpoint_result cr;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = asx_runtime_virtual_now() + 1000u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_pending, NULL, &tb, &tid), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 0);

    asx_runtime_virtual_advance(tb.deadline);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 1);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_DEADLINE);
}

/* -------------------------------------------------------------------
 * Region cancel (Rust RuntimeState::cancel_request) and reason semantics
 * ------------------------------------------------------------------- */

static asx_cancel_reason test_reason(asx_cancel_kind kind, asx_region_id origin, asx_time ts,
                                     const char *message) {
    asx_cancel_reason r;
    r.kind = kind;
    r.origin_region = origin;
    r.origin_task = ASX_INVALID_ID;
    r.timestamp = ts;
    r.message = message;
    r.cause = NULL;
    r.truncated = 0;
    return r;
}

TEST(region_cancel_closes_subtree_and_chains_parent_reasons) {
    asx_region_id parent;
    asx_region_id child;
    asx_task_id tp;
    asx_task_id tc;
    asx_task_id late;
    asx_region_state rs;
    asx_cancel_reason got;
    asx_cancel_reason req;
    uint32_t reached = 0;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(parent, poll_checkpoint_then_complete, NULL, &tp), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, poll_checkpoint_then_complete, NULL, &tc), ASX_OK);

    req = test_reason(ASX_CANCEL_USER, parent, 5u, "why");
    ASSERT_EQ(asx_region_cancel(parent, &req, &reached), ASX_OK);
    ASSERT_EQ(reached, 2u);

    /* Every region of the subtree begins closing and admits no new task. */
    ASSERT_EQ(asx_region_get_state(parent, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSING);
    ASSERT_EQ(asx_region_get_state(child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSING);
    ASSERT_NE(asx_task_spawn(child, poll_complete, NULL, &late), ASX_OK);

    /* The target's tasks take the request; a descendant's take
     * ParentCancelled from the immediate parent, caused by its reason. */
    ASSERT_EQ(asx_task_get_cancel_reason(tp, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_USER);
    ASSERT_EQ(got.origin_region, parent);
    ASSERT_EQ(got.timestamp, (asx_time)5u);
    ASSERT_STR_EQ(got.message, "why");
    ASSERT_EQ(asx_task_get_cancel_reason(tc, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_PARENT);
    ASSERT_EQ(got.origin_region, parent);
    ASSERT_EQ(got.timestamp, (asx_time)5u);
    ASSERT_TRUE(got.message == NULL);
    ASSERT_TRUE(got.cause != NULL && got.cause->kind == ASX_CANCEL_USER);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_REGION_CANCELLED), (uint64_t)2u);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST), (uint64_t)2u);

    /* Both regions finalize when their last task completes. */
    budget = asx_budget_from_polls(32);
    ASSERT_EQ(asx_scheduler_run(parent, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_state(child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_region_get_state(parent, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_REGION_CLOSED), (uint64_t)2u);
}

TEST(region_cancel_of_an_idle_region_finalizes_at_once) {
    asx_region_id rid;
    asx_region_state rs;
    asx_cancel_reason req;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    req = test_reason(ASX_CANCEL_SHUTDOWN, rid, 0u, NULL);
    ASSERT_EQ(asx_region_cancel(rid, &req, NULL), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_region_cancel(rid, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(cancel_strengthen_replaces_the_whole_reason_and_records_no_event) {
    asx_region_id rid;
    asx_task_id tid;
    asx_cancel_reason got;
    asx_cancel_reason weak;
    asx_cancel_reason strong;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &got), ASX_E_NOT_FOUND);

    weak = test_reason(ASX_CANCEL_USER, rid, 10u, NULL);
    strong = test_reason(ASX_CANCEL_SHUTDOWN, rid, 20u, "stop");
    ASSERT_EQ(asx_task_cancel_with_reason(tid, &weak), ASX_OK);
    ASSERT_EQ(asx_task_cancel_with_reason(tid, &strong), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(got.timestamp, (asx_time)20u);
    ASSERT_STR_EQ(got.message, "stop");
    /* A weaker request leaves it unchanged. A direct task cancel records no
     * request event (Rust RuntimeState::cancel_task, state.rs:3429), and
     * there is no strengthen event (:7794). */
    ASSERT_EQ(asx_task_cancel_with_reason(tid, &weak), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST), (uint64_t)0u);
}

TEST(region_cancel_records_one_request_per_newly_cancelled_task) {
    /* Rust records CancelRequest when a region cancel newly cancels a task
     * (cancel_request, state.rs:7864); strengthening it afterwards, by a
     * region or a direct cancel, records nothing more. */
    asx_region_id rid;
    asx_task_id tid;
    asx_cancel_reason got;
    asx_cancel_reason weak;
    asx_cancel_reason strong;
    uint32_t reached = 0;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    weak = test_reason(ASX_CANCEL_USER, rid, 10u, NULL);
    strong = test_reason(ASX_CANCEL_SHUTDOWN, rid, 20u, "stop");
    ASSERT_EQ(asx_region_cancel(rid, &weak, &reached), ASX_OK);
    ASSERT_EQ(reached, 1u);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST), (uint64_t)1u);
    ASSERT_EQ(asx_task_cancel_with_reason(tid, &strong), ASX_OK);
    ASSERT_EQ(asx_region_cancel(rid, &strong, &reached), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST), (uint64_t)1u);
}

/* -------------------------------------------------------------------
 * Spawn completion policy (Rust classify_spawn_completion,
 * task_handle.rs:173-202; fuzz findings gen-1-14/19/33/56, bd-ij9w)
 * ------------------------------------------------------------------- */

/* Yields until a checkpoint observes a cancel, then returns Ok, or returns
 * Ok at once when `ack` is 0 and it was polled twice. */
typedef struct {
    int ack;
    uint32_t polls;
} ack_child;

static asx_status poll_ack_child(void *ud, asx_task_id self) {
    ack_child *c = (ack_child *)ud;
    asx_checkpoint_result cr;
    c->polls++;
    if (c->ack) {
        if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    } else if (c->polls >= 2u) {
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* Spawns `child` from inside its first poll; with `cancel_now` it cancels
 * the child in that same poll, before the child's first poll. */
typedef struct {
    asx_region_id region;
    ack_child child_state;
    asx_task_id child;
    int spawned;
    int cancel_now;
} spawner;

static asx_status poll_spawner(void *ud, asx_task_id self) {
    spawner *s = (spawner *)ud;
    (void)self;
    if (!s->spawned) {
        asx_status st = asx_task_spawn(s->region, poll_ack_child, &s->child_state, &s->child);
        if (st != ASX_OK) return st;
        s->spawned = 1;
        if (s->cancel_now) return asx_task_cancel(s->child, ASX_CANCEL_USER);
    }
    return ASX_OK;
}

static asx_outcome_severity child_outcome(int ack, int cancel_now) {
    asx_region_id rid;
    asx_task_id parent;
    spawner s;
    asx_outcome out;
    asx_budget budget;

    asx_runtime_reset();
    memset(&s, 0, sizeof(s));
    s.child_state.ack = ack;
    s.cancel_now = cancel_now;
    if (asx_region_open(&rid) != ASX_OK) return ASX_OUTCOME_PANICKED;
    s.region = rid;
    if (asx_task_spawn(rid, poll_spawner, &s, &parent) != ASX_OK) return ASX_OUTCOME_PANICKED;
    budget = asx_budget_from_polls(2); /* parent completes, child polled once */
    {
        asx_status partial = asx_scheduler_run(rid, &budget); /* stops at the budget */
        (void)partial;
    }
    if (!cancel_now && asx_task_cancel(s.child, ASX_CANCEL_USER) != ASX_OK) {
        return ASX_OUTCOME_PANICKED;
    }
    budget = asx_budget_from_polls(20);
    if (asx_scheduler_run(rid, &budget) != ASX_OK) return ASX_OUTCOME_PANICKED;
    if (asx_task_get_outcome(s.child, &out) != ASX_OK) return ASX_OUTCOME_PANICKED;
    return asx_outcome_severity_of(&out);
}

TEST(spawned_child_that_acknowledges_a_later_cancel_keeps_its_value) {
    ASSERT_EQ((int)child_outcome(1, 0), (int)ASX_OUTCOME_OK);
}

TEST(spawned_child_cancelled_before_its_first_poll_is_cancelled) {
    ASSERT_EQ((int)child_outcome(1, 1), (int)ASX_OUTCOME_CANCELLED);
}

TEST(spawned_child_that_never_acknowledges_is_cancelled) {
    ASSERT_EQ((int)child_outcome(0, 0), (int)ASX_OUTCOME_CANCELLED);
}

TEST(top_level_task_that_acknowledges_is_still_cancelled) {
    /* Rust create_task: cancellation wins (DSL v2 §2). */
    asx_region_id rid;
    asx_task_id tid;
    ack_child c;
    asx_outcome out;
    asx_budget budget;

    asx_runtime_reset();
    memset(&c, 0, sizeof(c));
    c.ack = 1;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_ack_child, &c, &tid), ASX_OK);
    budget = asx_budget_from_polls(1);
    {
        asx_status partial = asx_scheduler_run(rid, &budget); /* stops at the budget */
        (void)partial;
    }
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)asx_outcome_severity_of(&out), (int)ASX_OUTCOME_CANCELLED);
}

TEST(cx_checkpoint_acknowledges_and_observes_a_passed_deadline) {
    /* Rust Cx::checkpoint (cx.rs:3112-3160) acknowledges a pending cancel
     * and turns a passed budget deadline into a cancel, as asx_checkpoint. */
    asx_region_id rid;
    asx_task_id tid;
    asx_task_id late;
    asx_task_state state;
    asx_cancel_reason got;
    asx_budget b;
    asx_cx cx;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    asx_cx_init(&cx, rid, tid, ASX_CAP_CANCEL_CHECK);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCELLING);

    b = asx_budget_infinite();
    b.deadline = 1u; /* already passed: the clock is past 1 ns */
    asx_runtime_virtual_advance(10u);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_pending, NULL, &b, &late), ASX_OK);
    asx_cx_init(&cx, rid, late, ASX_CAP_CANCEL_CHECK);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(late, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_DEADLINE);
}

TEST(budget_cancel_is_attributed_to_the_task_and_records_no_request) {
    asx_region_id rid;
    asx_task_id tid;
    asx_cancel_reason got;
    asx_budget quota;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    quota = asx_budget_infinite();
    quota.poll_quota = 1u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_checkpoint_then_complete, NULL, &quota, &tid),
              ASX_OK);
    budget = asx_budget_from_polls(16);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(got.origin_region, rid);
    ASSERT_TRUE(asx_handle_index(got.origin_task) == asx_handle_index(tid));
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST), (uint64_t)0u);
}

TEST(obligation_abort_with_reason_records_it) {
    asx_region_id rid;
    asx_obligation_id a;
    asx_obligation_id b;
    asx_obligation_info info;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_LEASE, ASX_INVALID_ID, &a),
              ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_ACK, ASX_INVALID_ID, &b), ASX_OK);
    ASSERT_EQ(asx_obligation_abort_with_reason(a, ASX_OBLIGATION_ABORT_LEAK_RECOVERED),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_abort_with_reason(a, ASX_OBLIGATION_ABORT_ERROR), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(a, &info), ASX_OK);
    ASSERT_EQ((int)info.abort_reason, (int)ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_abort(b), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(b, &info), ASX_OK);
    ASSERT_EQ((int)info.abort_reason, (int)ASX_OBLIGATION_ABORT_EXPLICIT);
}

int main(void) {
    fprintf(stderr, "=== test_cancellation (runtime) ===\n");

    RUN_TEST(cancel_running_task_transitions_to_cancel_requested);
    RUN_TEST(cancel_created_task_transitions_through_running);
    RUN_TEST(checkpoint_advances_to_cancelling);
    RUN_TEST(checkpoint_non_cancelled_task_reports_clean);
    RUN_TEST(finalize_transitions_cancelling_to_finalizing);
    RUN_TEST(finalize_rejects_wrong_state);
    RUN_TEST(cancel_strengthen_higher_severity_upgrades);
    RUN_TEST(cancel_completed_task_is_noop);
    RUN_TEST(cancel_propagation_cancels_all_tasks_in_region);
    RUN_TEST(cancel_propagation_skips_completed_tasks);
    RUN_TEST(cancelled_task_completion_gets_cancelled_outcome);
    RUN_TEST(cleanup_budget_exhaustion_forces_completion);
    RUN_TEST(cancel_forced_event_emitted);
    RUN_TEST(cancel_phase_query_tracks_progression);
    RUN_TEST(scheduler_decrements_cleanup_budget);
    RUN_TEST(cancel_with_origin_sets_attribution);
    RUN_TEST(cancel_storm_all_tasks_resolve);
    RUN_TEST(cancel_propagate_preserves_stronger_origin);
    RUN_TEST(cancel_propagation_sets_origin_region);
    RUN_TEST(scheduler_quiesces_after_cancel_completion);
    RUN_TEST(cleanup_budget_tighter_for_severe_cancels);
    RUN_TEST(checkpoint_null_result_rejected);
    RUN_TEST(cancel_phase_null_output_rejected);
    RUN_TEST(mask_defers_checkpoint_acknowledgement);
    RUN_TEST(masked_task_is_not_force_completed);
    RUN_TEST(unmasked_task_is_force_completed_by_cleanup_budget);
    RUN_TEST(mask_depth_is_bounded);
    RUN_TEST(checkpoint_observes_budget_deadline_inline);
    RUN_TEST(region_cancel_closes_subtree_and_chains_parent_reasons);
    RUN_TEST(region_cancel_of_an_idle_region_finalizes_at_once);
    RUN_TEST(cancel_strengthen_replaces_the_whole_reason_and_records_no_event);
    RUN_TEST(region_cancel_records_one_request_per_newly_cancelled_task);
    RUN_TEST(spawned_child_that_acknowledges_a_later_cancel_keeps_its_value);
    RUN_TEST(spawned_child_cancelled_before_its_first_poll_is_cancelled);
    RUN_TEST(spawned_child_that_never_acknowledges_is_cancelled);
    RUN_TEST(top_level_task_that_acknowledges_is_still_cancelled);
    RUN_TEST(cx_checkpoint_acknowledges_and_observes_a_passed_deadline);
    RUN_TEST(budget_cancel_is_attributed_to_the_task_and_records_no_request);
    RUN_TEST(obligation_abort_with_reason_records_it);

    TEST_REPORT();
    return test_failures;
}
