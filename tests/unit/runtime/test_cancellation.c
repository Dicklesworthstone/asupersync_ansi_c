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
#include "../../test_log.h"
#include <asx/asx.h>
#include <asx/core/cancel.h>
#include <asx/core/ghost.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls.
 * GCC's (void) cast does not silence warn_unused_result under -Werror. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* A fresh runtime with the opt-in hard cleanup bound
 * (asx_runtime_config.cleanup_hard_bound): for the tests about
 * force-completing cancelled tasks that never finish, a deviation from
 * Rust that a deployment must ask for. */
static asx_status reset_with_hard_cleanup_bound(void) {
    static asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_shutdown(&rt);
    asx_runtime_config_init(&cfg);
    cfg.cleanup_hard_bound = 1u;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    return st;
}

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

/* The task-transition events traced for `tid` from event `from` on. */
static uint32_t task_transitions_since(uint32_t from, asx_task_id tid, uint64_t *out,
                                       uint32_t cap) {
    uint32_t i;
    uint32_t n = 0;
    asx_trace_event ev;
    for (i = from; i < asx_trace_event_count(); i++) {
        if (!asx_trace_event_get(i, &ev)) continue;
        if (ev.kind != ASX_TRACE_TASK_TRANSITION || ev.entity_id != (uint64_t)tid) continue;
        if (n < cap) out[n] = ev.aux;
        n++;
    }
    return n;
}

#define TRANSITION_AUX(from, to) (((uint64_t)(uint32_t)(from) << 32) | (uint64_t)(uint32_t)(to))

TEST(cancel_created_task_goes_straight_to_cancel_requested) {
    /* Rust request_cancel* (record/task.rs:803-816) takes a task not yet
     * polled from Created to CancelRequested in one step and sets the
     * cancel epoch to 1; a later, stronger request strengthens the reason
     * and leaves the epoch at 1. The never-polled task's first poll
     * observes the cancel and it completes Cancelled. */
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_task_slot *slot;
    asx_outcome out;
    asx_budget budget;
    uint64_t seen[4];
    uint32_t mark;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    mark = asx_trace_event_count();
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_EQ(task_transitions_since(mark, tid, seen, 4u), (uint32_t)1u);
    ASSERT_EQ(seen[0], TRANSITION_AUX(ASX_TASK_CREATED, ASX_TASK_CANCEL_REQUESTED));
    ASSERT_EQ(asx_task_slot_lookup(tid, &slot), ASX_OK);
    ASSERT_EQ(slot->cancel_epoch, (uint32_t)1u);

    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(tid, &slot), ASX_OK);
    ASSERT_EQ((int)slot->cancel_reason.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(slot->cancel_epoch, (uint32_t)1u);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)asx_outcome_severity_of(&out), (int)ASX_OUTCOME_CANCELLED);
}

TEST(budget_cancel_of_a_created_task_materializes_in_one_step) {
    /* A budget cancel a checkpoint raised reaches the record of a task
     * not yet polled as Created → CancelRequested, epoch 1
     * (reconcile_checkpoint_cancel, record/task.rs:1104-1122). */
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_task_slot *slot;
    asx_checkpoint_result cr;
    asx_budget b;
    uint64_t seen[4];
    uint32_t mark;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    b = asx_budget_infinite();
    b.deadline = 1u; /* already passed: the clock is past 1 ns */
    asx_runtime_virtual_advance(10u);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_pending, NULL, &b, &tid), ASX_OK);

    mark = asx_trace_event_count();
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCELLING);
    ASSERT_TRUE(task_transitions_since(mark, tid, seen, 4u) >= 1u);
    ASSERT_EQ(seen[0], TRANSITION_AUX(ASX_TASK_CREATED, ASX_TASK_CANCEL_REQUESTED));
    ASSERT_EQ(asx_task_slot_lookup(tid, &slot), ASX_OK);
    ASSERT_EQ(slot->cancel_epoch, (uint32_t)1u);
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
 * Tests: the acknowledged cancel's path to Completed, observed as real
 * task-state transitions.
 *
 * Rust ends a task that acknowledged its cancel and then returns with
 * cleanup_done() (Cancelling -> Finalizing) and finalize_done()
 * (Finalizing -> Completed(Cancelled)) in the same step it observes the
 * return (record/task.rs:1378, :1416; lab/runtime.rs:4862-4888,
 * runtime/scheduler/worker.rs:686-696, three_lane.rs:9320-9326). A task
 * that ends Panicked or Err goes through record.complete(), straight
 * from Cancelling. Canonical vocabulary v2 has no task-state event, so
 * a Rust-captured fixture cannot tell the two paths apart (a snapshot
 * sees only the end state): the runtime's task-transition trace is the
 * witness, and these tests are its only parity check.
 * ------------------------------------------------------------------- */

/* Acknowledges the cancel, then ends the task with `*(asx_status *)data`
 * (the poll's return) after the optional extra step. */
typedef struct {
    asx_status ret;
    int finalize_first;
    int panic_first;
} ack_then_return_cfg;

static asx_status poll_ack_then_return(void *data, asx_task_id self) {
    const ack_then_return_cfg *cfg = (const ack_then_return_cfg *)data;
    asx_checkpoint_result cr;
    if (asx_checkpoint(self, &cr) != ASX_OK || !cr.cancelled) return ASX_E_PENDING;
    if (cfg->finalize_first) {
        asx_status st = asx_task_finalize(self);
        if (st != ASX_OK) return st;
    }
    if (cfg->panic_first) {
        asx_status st = asx_task_panic(self, "panic during cleanup");
        if (st != ASX_OK) return st;
    }
    return cfg->ret;
}

/* Runs one task to completion through an acknowledged cancel, and leaves
 * its post-cancel task transitions in `seen` (count in *out_n). */
static asx_status run_acknowledged_cancel(const ack_then_return_cfg *cfg, asx_task_id *out_tid,
                                          uint64_t *seen, uint32_t cap, uint32_t *out_n) {
    asx_region_id rid;
    asx_budget budget;
    asx_status st;
    uint32_t mark;

    asx_runtime_reset();
    st = asx_region_open(&rid);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(rid, poll_ack_then_return, (void *)cfg, out_tid);
    if (st != ASX_OK) return st;
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget); /* Created -> Running; the poll is pending */
    st = asx_task_cancel(*out_tid, ASX_CANCEL_USER);
    if (st != ASX_OK) return st;
    mark = asx_trace_event_count();
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);
    *out_n = task_transitions_since(mark, *out_tid, seen, cap);
    return ASX_OK;
}

TEST(natural_return_after_acknowledge_completes_through_finalizing) {
    ack_then_return_cfg cfg;
    asx_task_id tid;
    asx_task_state state;
    asx_cancel_phase phase;
    asx_outcome out;
    uint64_t seen[8];
    uint32_t n = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ret = ASX_OK;
    ASSERT_EQ(run_acknowledged_cancel(&cfg, &tid, seen, 8u, &n), ASX_OK);

    ASSERT_EQ(n, (uint32_t)3u);
    ASSERT_EQ(seen[0], TRANSITION_AUX(ASX_TASK_CANCEL_REQUESTED, ASX_TASK_CANCELLING));
    ASSERT_EQ(seen[1], TRANSITION_AUX(ASX_TASK_CANCELLING, ASX_TASK_FINALIZING));
    ASSERT_EQ(seen[2], TRANSITION_AUX(ASX_TASK_FINALIZING, ASX_TASK_COMPLETED));
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_COMPLETED);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

TEST(error_return_after_acknowledge_completes_cancelled_through_finalizing) {
    /* Rust's task wrapper reports Ok(()) whatever the body returned, so an
     * acknowledged cancel's task finalizes whichever way the body ends
     * (lab/runtime.rs:4918 "create_task wraps user futures to always
     * return Outcome::Ok(())"); the C poll's error is how the task
     * acknowledged, not a fault. */
    ack_then_return_cfg cfg;
    asx_task_id tid;
    asx_outcome out;
    uint64_t seen[8];
    uint32_t n = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ret = ASX_E_CANCELLED;
    ASSERT_EQ(run_acknowledged_cancel(&cfg, &tid, seen, 8u, &n), ASX_OK);

    ASSERT_EQ(n, (uint32_t)3u);
    ASSERT_EQ(seen[0], TRANSITION_AUX(ASX_TASK_CANCEL_REQUESTED, ASX_TASK_CANCELLING));
    ASSERT_EQ(seen[1], TRANSITION_AUX(ASX_TASK_CANCELLING, ASX_TASK_FINALIZING));
    ASSERT_EQ(seen[2], TRANSITION_AUX(ASX_TASK_FINALIZING, ASX_TASK_COMPLETED));
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

TEST(finalizing_is_entered_once_when_the_task_finalizes_itself) {
    /* The task calls asx_task_finalize() before it returns: the completion
     * adds no second Cancelling -> Finalizing step (Rust's cleanup_done
     * is a no-op outside Cancelling, task.rs:1378-1400). */
    ack_then_return_cfg cfg;
    asx_task_id tid;
    asx_outcome out;
    uint64_t seen[8];
    uint32_t n = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ret = ASX_OK;
    cfg.finalize_first = 1;
    ASSERT_EQ(run_acknowledged_cancel(&cfg, &tid, seen, 8u, &n), ASX_OK);

    ASSERT_EQ(n, (uint32_t)3u);
    ASSERT_EQ(seen[0], TRANSITION_AUX(ASX_TASK_CANCEL_REQUESTED, ASX_TASK_CANCELLING));
    ASSERT_EQ(seen[1], TRANSITION_AUX(ASX_TASK_CANCELLING, ASX_TASK_FINALIZING));
    ASSERT_EQ(seen[2], TRANSITION_AUX(ASX_TASK_FINALIZING, ASX_TASK_COMPLETED));
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

TEST(panic_during_cleanup_completes_straight_from_cancelling) {
    /* A panic is Outcome::Panicked at the poll boundary and ends the task
     * with record.complete(), a direct Cancelling -> Completed that the
     * transition table allows for an error or panic during cleanup
     * (record/task.rs:160-161, :175; lab/runtime.rs:4899). It never
     * finalizes, and it dominates the cancel in the outcome. */
    ack_then_return_cfg cfg;
    asx_task_id tid;
    asx_outcome out;
    uint64_t seen[8];
    uint32_t n = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ret = ASX_OK;
    cfg.panic_first = 1;
    ASSERT_EQ(run_acknowledged_cancel(&cfg, &tid, seen, 8u, &n), ASX_OK);

    ASSERT_EQ(n, (uint32_t)2u);
    ASSERT_EQ(seen[0], TRANSITION_AUX(ASX_TASK_CANCEL_REQUESTED, ASX_TASK_CANCELLING));
    ASSERT_EQ(seen[1], TRANSITION_AUX(ASX_TASK_CANCELLING, ASX_TASK_COMPLETED));
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_PANICKED);
}

/* -------------------------------------------------------------------
 * Test: cleanup budget exhaustion forces task completion (opt-in hard
 * bound only; Rust never force-completes)
 * ------------------------------------------------------------------- */

TEST(cleanup_budget_exhaustion_forces_completion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_outcome out;
    asx_cancel_phase phase;
    asx_budget budget;
    asx_checkpoint_result cr;

    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);

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

    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);

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

    /* The tasks never finish on their own: only the hard bound ends them. */
    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);

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

TEST(mask_depth_is_a_nesting_depth_not_a_consumed_deferral_budget) {
    /* bd-9kll.3.18 witness for inv.cancel.mask_monotone (#12). The formal
     * model's mask is a deferral budget that only a masked checkpoint
     * lowers (Step.cancelMasked, formal/lean/Asupersync.lean:453-463).
     * Rust implements a nesting depth instead: Cx::masked raises it with no
     * state check, only the 64 bound (cx/cx.rs:3315-3333), and
     * MaskGuard::drop lowers it (cx/cx.rs:710-715). C is the same. This
     * test fixes what holds: the depth may grow after a cancel is requested
     * and again after it is acknowledged, a masked checkpoint leaves it
     * unchanged, and the cancel is acknowledged once every mask is
     * released. Eventual delivery therefore assumes every masked section
     * ends. */
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result cr;
    uint32_t depth;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);

    /* Nesting after the request is accepted, as Rust's Cx::masked. */
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 2u);

    /* A masked checkpoint defers and consumes nothing. */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 0);
    ASSERT_EQ(cr.masked, 1);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.masked, 1);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 2u);

    /* Released one level: still deferred; at 0 the cancel is
     * acknowledged. */
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 0);
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_EQ(cr.cancelled, 1);
    ASSERT_EQ((int)cr.phase, (int)ASX_CANCEL_PHASE_CANCELLING);

    /* After acknowledgement the depth can grow again (cleanup code may
     * mask, as in Rust): the depth is not monotone during cancel
     * processing. */
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 1u);
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_task_unmask(tid), ASX_E_INVALID_STATE); /* no underflow */
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

    /* Even the hard bound never interrupts a masked section. */
    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);
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

    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    budget = asx_budget_from_polls(500);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
}

typedef struct {
    uint32_t calls;
    uint32_t depth;
    asx_status result;
    asx_checkpoint_result checkpoint;
} masked_callback_state;

static asx_status masked_callback(void *data, asx_task_id self) {
    masked_callback_state *state = (masked_callback_state *)data;
    asx_status st;
    state->calls++;
    st = asx_task_mask_depth(self, &state->depth);
    if (st != ASX_OK) return st;
    st = asx_checkpoint(self, &state->checkpoint);
    if (st != ASX_OK) return st;
    return state->result;
}

TEST(masked_callback_restores_on_success_error_and_pending_returns) {
    const asx_status returns[] = {ASX_OK, ASX_E_INVALID_ARGUMENT, ASX_E_CANCELLED, ASX_E_PENDING};
    asx_region_id rid;
    asx_task_id tid;
    uint32_t i;
    uint32_t depth;
    masked_callback_state state;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    for (i = 0; i < sizeof(returns) / sizeof(returns[0]); i++) {
        memset(&state, 0, sizeof(state));
        state.result = returns[i];
        ASSERT_EQ(asx_task_masked(tid, masked_callback, &state), returns[i]);
        ASSERT_EQ(state.calls, 1u);
        ASSERT_EQ(state.depth, 1u);
        ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
        ASSERT_EQ(depth, 0u);
    }
}

typedef struct {
    masked_callback_state inner;
    uint32_t before;
    uint32_t after;
    asx_status inner_result;
} nested_masked_callback_state;

static asx_status nested_masked_callback(void *data, asx_task_id self) {
    nested_masked_callback_state *state = (nested_masked_callback_state *)data;
    asx_status st = asx_task_mask_depth(self, &state->before);
    if (st != ASX_OK) return st;
    state->inner_result = asx_task_masked(self, masked_callback, &state->inner);
    st = asx_task_mask_depth(self, &state->after);
    if (st != ASX_OK) return st;
    return state->inner_result;
}

TEST(masked_callback_nested_error_restores_each_level_and_delivers_cancel) {
    asx_region_id rid;
    asx_task_id tid;
    asx_checkpoint_result checkpoint;
    nested_masked_callback_state state;
    uint32_t depth;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    memset(&state, 0, sizeof(state));
    state.inner.result = ASX_E_INVALID_ARGUMENT;
    ASSERT_EQ(asx_task_masked(tid, nested_masked_callback, &state), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(state.before, 1u);
    ASSERT_EQ(state.inner.depth, 2u);
    ASSERT_EQ(state.after, 1u);
    ASSERT_EQ(state.inner.calls, 1u);
    ASSERT_TRUE(state.inner.checkpoint.masked);
    ASSERT_TRUE(!state.inner.checkpoint.cancelled);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 0u);
    ASSERT_EQ(asx_checkpoint(tid, &checkpoint), ASX_OK);
    ASSERT_TRUE(checkpoint.cancelled);
    ASSERT_TRUE(!checkpoint.masked);
}

TEST(masked_callback_preserves_an_existing_raw_mask) {
    asx_region_id rid;
    asx_task_id tid;
    masked_callback_state state;
    uint32_t depth;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    memset(&state, 0, sizeof(state));
    state.result = ASX_E_CANCELLED;
    ASSERT_EQ(asx_task_masked(tid, masked_callback, &state), ASX_E_CANCELLED);
    ASSERT_EQ(state.depth, 2u);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 1u);
    ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
}

TEST(masked_callback_refuses_excess_depth_without_running_body) {
    asx_region_id rid;
    asx_task_id tid;
    masked_callback_state state;
    uint32_t depth;
    uint32_t i;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    for (i = 0; i < ASX_MAX_MASK_DEPTH - 1u; i++) ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    memset(&state, 0, sizeof(state));
    ASSERT_EQ(asx_task_masked(tid, masked_callback, &state), ASX_OK);
    ASSERT_EQ(state.calls, 1u);
    ASSERT_EQ(state.depth, ASX_MAX_MASK_DEPTH);
    ASSERT_EQ(asx_task_mask(tid), ASX_OK);
    state.calls = 0u;
    ASSERT_EQ(asx_task_masked(tid, masked_callback, &state), ASX_E_INVALID_STATE);
    ASSERT_EQ(state.calls, 0u);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, ASX_MAX_MASK_DEPTH);
    for (i = 0; i < ASX_MAX_MASK_DEPTH; i++) ASSERT_EQ(asx_task_unmask(tid), ASX_OK);
}

TEST(masked_callback_rejects_null_terminal_and_stale_admission) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    masked_callback_state state;
    uint32_t depth;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    memset(&state, 0, sizeof(state));
    ASSERT_EQ(asx_task_masked(tid, NULL, &state), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 0u);
    budget = asx_budget_from_polls(4u);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_masked(tid, masked_callback, &state), ASX_E_INVALID_STATE);
    ASSERT_EQ(state.calls, 0u);
    asx_runtime_reset();
    ASSERT_NE(asx_task_masked(tid, masked_callback, &state), ASX_OK);
    ASSERT_EQ(state.calls, 0u);
    ASSERT_NE(asx_task_masked(ASX_INVALID_ID, masked_callback, &state), ASX_OK);
    ASSERT_EQ(state.calls, 0u);
}

static asx_status masked_callback_unbalances_its_raw_mask(void *data, asx_task_id self) {
    (void)data;
    if (asx_task_unmask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

TEST(masked_callback_reports_a_failed_restoration) {
    asx_region_id rid;
    asx_task_id tid;
    uint32_t depth;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_masked(tid, masked_callback_unbalances_its_raw_mask, NULL),
              ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 0u);
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

TEST(mask_refuses_a_completed_task_and_a_handle_from_a_reset_runtime) {
    /* bd-9kll.3.19: a completed task cannot enter a critical section (Rust
     * has no Cx to mask once the body returned), and a handle the runtime
     * no longer holds is refused, its depth unread. */
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    uint32_t depth = 7u;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    budget = asx_budget_from_polls(4);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_mask(tid), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 0u);

    asx_runtime_reset();
    ASSERT_NE(asx_task_mask(tid), ASX_OK);
    ASSERT_NE(asx_task_unmask(tid), ASX_OK);
    depth = 7u;
    ASSERT_NE(asx_task_mask_depth(tid, &depth), ASX_OK);
    ASSERT_EQ(depth, 7u);
}

/* A worker doing nested masked work: masks twice on its first poll,
 * releases the inner mask at poll `inner_off` and the outer at
 * `outer_off` (0: never), checkpoints every poll and records its depth. */
typedef struct {
    uint32_t polls;
    uint32_t inner_off;
    uint32_t outer_off;
    uint32_t observed_at; /* poll whose checkpoint acknowledged the cancel */
    uint32_t masked_checkpoints;
    uint32_t depth_log[16];
} nested_mask_worker;

static asx_status poll_nested_mask_worker(void *data, asx_task_id self) {
    nested_mask_worker *w = (nested_mask_worker *)data;
    asx_checkpoint_result cr;
    uint32_t depth = 0;
    w->polls++;
    if (w->polls == 1u && (asx_task_mask(self) != ASX_OK || asx_task_mask(self) != ASX_OK)) {
        return ASX_E_INVALID_STATE;
    }
    if (w->polls == w->inner_off && asx_task_unmask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    if (w->polls == w->outer_off && asx_task_unmask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    if (asx_task_mask_depth(self, &depth) != ASX_OK) return ASX_E_INVALID_STATE;
    if (w->polls <= 16u) w->depth_log[w->polls - 1u] = depth;
    if (asx_checkpoint(self, &cr) != ASX_OK) return ASX_E_INVALID_STATE;
    if (cr.masked) w->masked_checkpoints++;
    if (cr.cancelled) {
        w->observed_at = w->polls;
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* Joins `target` and records what the join saw. */
typedef struct {
    asx_task_id target;
    const nested_mask_worker *worker;
    int joined;
    uint32_t worker_polls_at_join;
    asx_outcome outcome;
} mask_joiner;

static asx_status poll_mask_joiner(void *data, asx_task_id self) {
    mask_joiner *j = (mask_joiner *)data;
    asx_status st = asx_task_join_poll(self, j->target, &j->outcome);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    j->joined = st == ASX_OK;
    j->worker_polls_at_join = j->worker->polls;
    return ASX_OK;
}

/* Spawn the worker and its joiner, give the worker its first (masking)
 * poll, then cancel it and run. */
static void run_nested_mask_scenario(nested_mask_worker *w, mask_joiner *j, asx_task_id *wid) {
    asx_region_id rid;
    asx_task_id jid;
    asx_budget budget;
    asx_status st;
    asx_runtime_reset();
    st = asx_region_open(&rid);
    if (st == ASX_OK) st = asx_task_spawn(rid, poll_nested_mask_worker, w, wid);
    j->target = *wid;
    j->worker = w;
    if (st == ASX_OK) st = asx_task_spawn(rid, poll_mask_joiner, j, &jid);
    budget = asx_budget_from_polls(2);
    if (st == ASX_OK) ASX_IGNORE_RESULT(asx_scheduler_run(rid, &budget)); /* stops at the budget */
    if (st == ASX_OK) st = asx_task_cancel(*wid, ASX_CANCEL_USER);
    budget = asx_budget_from_polls(100);
    if (st == ASX_OK) ASX_IGNORE_RESULT(asx_scheduler_run(rid, &budget));
}

TEST(nested_masked_work_drains_after_the_outer_mask_while_a_joiner_waits) {
    /* bd-9kll.3.19: a worker cancelled inside two nested masks keeps
     * working (each masked checkpoint defers and consumes nothing,
     * Cx::masked nesting, cx/cx.rs:3315-3333), releases the inner mask
     * (still deferred), then the outer, and its next checkpoint
     * acknowledges: it completes Cancelled. A task joining it waits all
     * along and sees that outcome only once the worker has finished. */
    static nested_mask_worker w;
    static mask_joiner j;
    asx_task_id wid = ASX_INVALID_ID;
    asx_outcome out;
    static const uint32_t expected_depths[7] = {2u, 2u, 2u, 1u, 1u, 1u, 0u};
    uint32_t i;

    memset(&w, 0, sizeof(w));
    memset(&j, 0, sizeof(j));
    w.inner_off = 4u;
    w.outer_off = 7u;
    run_nested_mask_scenario(&w, &j, &wid);

    ASSERT_EQ(w.polls, 7u);
    ASSERT_EQ(w.observed_at, 7u);
    ASSERT_EQ(w.masked_checkpoints, 5u); /* polls 2..6: cancel pending, masked */
    for (i = 0; i < 7u; i++) ASSERT_EQ(w.depth_log[i], expected_depths[i]);
    /* The join took the worker's outcome (a joined task's handle is spent). */
    ASSERT_NE(asx_task_get_outcome(wid, &out), ASX_OK);
    ASSERT_TRUE(j.joined);
    ASSERT_EQ(j.worker_polls_at_join, 7u);
    ASSERT_EQ(asx_outcome_severity_of(&j.outcome), ASX_OUTCOME_CANCELLED);
}

TEST(a_mask_never_released_keeps_the_cancel_out_and_the_joiner_waiting) {
    /* Negative control for the test above: delivery assumes every masked
     * section ends. With the outer mask never released (a leaked
     * restoration) the cancel is never acknowledged, the cleanup bound
     * does not interrupt the masked work, and the join never resolves:
     * finite completion is a property of the program, not a liveness
     * guarantee of the runtime. */
    static nested_mask_worker w;
    static mask_joiner j;
    asx_task_id wid = ASX_INVALID_ID;
    uint32_t depth = 0;

    memset(&w, 0, sizeof(w));
    memset(&j, 0, sizeof(j));
    w.inner_off = 4u;
    w.outer_off = 0u;
    run_nested_mask_scenario(&w, &j, &wid);

    ASSERT_EQ(w.observed_at, 0u);
    ASSERT_TRUE(w.polls > 7u);
    ASSERT_EQ(w.masked_checkpoints, w.polls - 1u);
    ASSERT_EQ(asx_task_mask_depth(wid, &depth), ASX_OK);
    ASSERT_EQ(depth, 1u);
    ASSERT_TRUE(!j.joined);
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
    asx_cx_init(&cx, rid, tid, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCELLING);

    b = asx_budget_infinite();
    b.deadline = 1u; /* already passed: the clock is past 1 ns */
    asx_runtime_virtual_advance(10u);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_pending, NULL, &b, &late), ASX_OK);
    asx_cx_init(&cx, rid, late, ASX_CAP_NONE);
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_E_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(late, &got), ASX_OK);
    ASSERT_EQ((int)got.kind, (int)ASX_CANCEL_DEADLINE);
}

TEST(cx_is_cancelled_reports_whether_a_cancel_was_requested) {
    /* Rust Cx::is_cancel_requested (cx.rs:2641) reads the Cx's
     * cancel_requested flag, set by a request and never cleared: false
     * for a task that completed without one, true through a mask
     * (cx.rs:6660-6664), true after the cancelled task completed, and
     * true once a masked checkpoint raised a budget cancel the task's
     * record does not show yet (cx.rs:2824-2836). */
    asx_region_id rid;
    asx_task_id done;
    asx_task_id masked;
    asx_task_id late;
    asx_task_state state;
    asx_checkpoint_result cr;
    asx_outcome out;
    asx_budget b;
    asx_budget budget;
    asx_cx cx;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &done), ASX_OK);
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(done, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);
    asx_cx_init(&cx, rid, done, ASX_CAP_NONE);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));

    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &masked), ASX_OK);
    ASSERT_EQ(asx_task_mask(masked), ASX_OK);
    asx_cx_init(&cx, rid, masked, ASX_CAP_NONE);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
    ASSERT_EQ(asx_task_cancel(masked, ASX_CANCEL_USER), ASX_OK);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
    ASSERT_EQ(asx_cx_checkpoint(&cx), ASX_OK);
    ASSERT_EQ(asx_task_unmask(masked), ASX_OK);
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(masked, &out), ASX_OK);
    ASSERT_EQ((int)asx_outcome_severity_of(&out), (int)ASX_OUTCOME_CANCELLED);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));

    b = asx_budget_infinite();
    b.deadline = 1u; /* already passed: the clock is past 1 ns */
    asx_runtime_virtual_advance(10u);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_pending, NULL, &b, &late), ASX_OK);
    ASSERT_EQ(asx_task_mask(late), ASX_OK);
    asx_cx_init(&cx, rid, late, ASX_CAP_NONE);
    ASSERT_FALSE(asx_cx_is_cancelled(&cx));
    ASSERT_EQ(asx_checkpoint(late, &cr), ASX_OK);
    ASSERT_FALSE(cr.cancelled);
    ASSERT_EQ(asx_task_get_state(late, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CREATED);
    ASSERT_TRUE(asx_cx_is_cancelled(&cx));
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
    ASSERT_EQ(asx_obligation_abort_with_reason(a, ASX_OBLIGATION_ABORT_NONE),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_abort_with_reason(a, (asx_obligation_abort_reason)4),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_abort_with_reason(a, ASX_OBLIGATION_ABORT_ERROR), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(a, &info), ASX_OK);
    ASSERT_EQ((int)info.abort_reason, (int)ASX_OBLIGATION_ABORT_ERROR);
    ASSERT_EQ(asx_obligation_abort(b), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(b, &info), ASX_OK);
    ASSERT_EQ((int)info.abort_reason, (int)ASX_OBLIGATION_ABORT_EXPLICIT);
}

/* -------------------------------------------------------------------
 * Cleanup budgets, Rust semantics (bd-9kll.3.2): the cleanup budget
 * becomes the task's budget when it acknowledges, a spent cleanup quota
 * only strengthens the reason, and no task is force-completed.
 * ------------------------------------------------------------------- */

static int saw_cancel_forced(void) {
    uint32_t i;
    for (i = 0; i < asx_scheduler_event_count(); i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_CANCEL_FORCED) return 1;
    }
    return 0;
}

typedef struct {
    uint32_t cleanup_polls; /* polls since the cancel was acknowledged */
    uint32_t finish_after;  /* completes on this many cleanup polls */
    asx_cancel_kind kind;   /* reason kind seen on the latest one */
} cleanup_state;

/* Checkpoints every poll; once cancelled, cleans up for `finish_after`
 * polls and then completes. */
static asx_status poll_cleanup(void *data, asx_task_id self) {
    cleanup_state *s = (cleanup_state *)data;
    asx_checkpoint_result cr;
    if (asx_checkpoint(self, &cr) != ASX_OK) return ASX_E_INVALID_STATE;
    if (!cr.cancelled) return ASX_E_PENDING;
    s->cleanup_polls++;
    s->kind = cr.kind;
    return s->cleanup_polls >= s->finish_after ? ASX_OK : ASX_E_PENDING;
}

/* Spawn poll_cleanup, run it once, cancel it with `kind`, run to the end. */
static asx_status run_cleanup(cleanup_state *s, asx_cancel_kind kind, asx_task_id *out_tid) {
    asx_region_id rid;
    asx_budget budget;
    asx_status st;
    asx_runtime_reset();
    st = asx_region_open(&rid);
    if (st == ASX_OK) st = asx_task_spawn(rid, poll_cleanup, s, out_tid);
    if (st != ASX_OK) return st;
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    st = asx_task_cancel(*out_tid, kind);
    if (st != ASX_OK) return st;
    budget = asx_budget_from_polls(5000);
    return asx_scheduler_run(rid, &budget);
}

TEST(cleanup_within_its_quota_completes_with_the_cancel_reason) {
    cleanup_state s;
    asx_task_id tid;
    asx_outcome out;
    memset(&s, 0, sizeof(s));
    s.finish_after = 300u; /* TIMEOUT's cleanup quota is 500 */
    ASSERT_EQ(run_cleanup(&s, ASX_CANCEL_TIMEOUT, &tid), ASX_OK);
    ASSERT_EQ(s.cleanup_polls, 300u);
    ASSERT_EQ((int)s.kind, (int)ASX_CANCEL_TIMEOUT);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_FALSE(saw_cancel_forced());
}

TEST(cleanup_past_its_quota_is_strengthened_not_force_completed) {
    cleanup_state s;
    asx_task_id tid;
    asx_outcome out;
    asx_cancel_reason reason;
    memset(&s, 0, sizeof(s));
    s.finish_after = 600u; /* past TIMEOUT's 500 cleanup polls */
    ASSERT_EQ(run_cleanup(&s, ASX_CANCEL_TIMEOUT, &tid), ASX_OK);
    /* The task ran its whole cleanup: it was not cut short at 500. */
    ASSERT_EQ(s.cleanup_polls, 600u);
    /* The spent quota strengthened TIMEOUT (severity 1) to POLL_QUOTA. */
    ASSERT_EQ((int)s.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_FALSE(saw_cancel_forced());
}

TEST(unacknowledged_cancel_is_never_force_completed) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    /* Far past SHUTDOWN's 50 cleanup polls: the task, which never
     * checkpoints, keeps running until the caller's budget runs out. */
    budget = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_FALSE(saw_cancel_forced());
}

TEST(acknowledgement_replaces_the_budget_with_the_cleanup_budget) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget tb;
    asx_budget budget;
    asx_budget got;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    tb = asx_budget_from_polls(10);
    tb.cost_quota = 50u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_checkpoint_forever, NULL, &tb, &tid), ASX_OK);
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);
    /* Not acknowledged yet: the task keeps its own budget. */
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 9u);
    /* The poll acknowledges; then DEADLINE's cleanup budget (500 polls,
     * priority 210, no deadline or cost) replaces it. */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 500u);
    ASSERT_EQ((int)got.priority, 210);
    ASSERT_EQ(got.deadline, ASX_TIME_INFINITE);
    ASSERT_EQ(got.cost_quota, UINT64_MAX);
    /* The next poll is charged to it. */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 499u);
}

TEST(cancel_during_cleanup_meets_and_refills_the_budget) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_budget got;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_forever, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(11);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 990u); /* USER: 1000, 10 polls since the ack */
    /* A SHUTDOWN request meets the cleanup budget (min quota, max
     * priority) and the task takes it whole (record/task.rs:752-770). */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 50u);
    ASSERT_EQ((int)got.priority, 255);
    budget = asx_budget_from_polls(5);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 45u);
    /* A weaker request changes neither the reason nor the met budget, but
     * the task takes the met budget again: its quota is refilled. */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 50u);
}

TEST(get_cleanup_budget_reports_the_met_budget_of_a_pending_cancel) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget got;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_get_cleanup_budget(tid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_get_cleanup_budget(tid, &got), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_TIMEOUT), ASX_OK);
    ASSERT_EQ(asx_task_get_cleanup_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 500u);
    ASSERT_EQ((int)got.priority, 210);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_FAIL_FAST), ASX_OK);
    ASSERT_EQ(asx_task_get_cleanup_budget(tid, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 200u);
    ASSERT_EQ((int)got.priority, 220);
    ASSERT_EQ(asx_task_get_cleanup_budget(ASX_INVALID_ID, &got), ASX_E_NOT_FOUND);
}

/* -------------------------------------------------------------------
 * Budget cancels the record has not taken (bd-mex3): Rust raises them on
 * the task's Cx; the record stays Running until a checkpoint acknowledges.
 * ------------------------------------------------------------------- */

/* Poll 1 spends its only quota unit; a masked checkpoint sees it spent
 * (a budget cancel it cannot acknowledge yet); then it parks for good. */
static asx_status poll_spend_masked_then_park(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_task_mask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    if (asx_checkpoint(self, &cr) != ASX_OK || cr.cancelled || !cr.masked) {
        return ASX_E_INVALID_STATE;
    }
    if (asx_task_unmask(self) != ASX_OK) return ASX_E_INVALID_STATE;
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

static asx_status spend_masked(asx_region_id *out_rid, asx_task_id *out_tid) {
    asx_budget tb = asx_budget_from_polls(1);
    asx_budget run = asx_budget_from_polls(10);
    asx_status st;
    asx_runtime_reset();
    st = asx_region_open(out_rid);
    if (st == ASX_OK) {
        st = asx_task_spawn_with_budget(*out_rid, poll_spend_masked_then_park, NULL, &tb, out_tid);
    }
    if (st == ASX_OK) st = asx_scheduler_run(*out_rid, &run);
    return st;
}

TEST(budget_cancel_stays_off_the_record_until_acknowledged) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_cancel_reason r;

    /* Parked with no waker: the run reports it would block. The task was
     * not woken by its own budget cancel. */
    ASSERT_EQ(spend_masked(&rid, &tid), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_RUNNING);
    /* The cancel is pending (what the task's own checks see). */
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &r), ASX_OK);
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_POLL_QUOTA);
}

TEST(cancel_request_newly_cancels_a_record_with_a_pending_budget_cancel) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_cancel_reason r;
    uint64_t requests_before;

    ASSERT_EQ(spend_masked(&rid, &tid), ASX_E_WOULD_BLOCK);
    /* A region cancel newly cancels the record (a cancel.requested event),
     * its reason the stronger of the two: Shutdown over PollQuota. */
    requests_before = asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST);
    ASSERT_EQ(asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN), (uint32_t)1);
    ASSERT_EQ(asx_trace_kind_total(ASX_TRACE_CANCEL_REQUEST), requests_before + 1u);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_EQ(asx_task_get_cancel_reason(tid, &r), ASX_OK);
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_SHUTDOWN);
}

TEST(cleanup_hard_bound_config_is_validated) {
    asx_runtime_config cfg;
    asx_runtime_config_init(&cfg);
    ASSERT_EQ(cfg.cleanup_hard_bound, 0u);
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_OK);
    cfg.cleanup_hard_bound = 1u;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_OK);
    cfg.cleanup_hard_bound = 2u;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

/* bd-9kll.3.22: every task-state write goes through
 * asx_task_set_state_internal, which applies the transition table in every
 * build. For each of the 36 (from, to) pairs a legal move writes `to`; an
 * illegal one leaves `from`, returns ASX_E_INVALID_TRANSITION and, in ghost
 * builds, records a task-protocol violation. A state word outside the enum
 * is refused too. */
TEST(task_state_write_applies_the_transition_table_in_every_build) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_slot *slot;
    int from;
    int to;
    uint32_t legal = 0;
    uint32_t refused = 0;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(tid, &slot), ASX_OK);

    for (from = 0; from <= (int)ASX_TASK_COMPLETED; from++) {
        for (to = 0; to <= (int)ASX_TASK_COMPLETED; to++) {
            asx_status want = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
            slot->state = (asx_task_state)from;
            ASSERT_EQ(asx_task_set_state_internal(slot, tid, (asx_task_state)to), want);
            ASSERT_EQ((int)slot->state, want == ASX_OK ? to : from);
            if (want == ASX_OK) {
                legal++;
            } else {
                ASSERT_EQ(want, ASX_E_INVALID_TRANSITION);
                refused++;
            }
        }
    }
    /* T1-T13 of the table; the 23 other pairs are refused. */
    ASSERT_EQ(legal, (uint32_t)13u);
    ASSERT_EQ(refused, (uint32_t)23u);
#ifdef ASX_DEBUG_GHOST
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)23u);
    {
        asx_ghost_violation v;
        ASSERT_TRUE(asx_ghost_violation_get(0u, &v));
        ASSERT_EQ((int)v.kind, (int)ASX_GHOST_PROTOCOL_TASK);
        ASSERT_EQ(v.entity_id, (uint64_t)tid);
    }
#endif

    slot->state = (asx_task_state)6;
    ASSERT_EQ(asx_task_set_state_internal(slot, tid, ASX_TASK_COMPLETED), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ((int)slot->state, 6);
    slot->state = ASX_TASK_CREATED;
}

/* bd-9kll.3.22 negative control at a runtime boundary: a task whose state
 * word is corrupt (outside the enum) returns Ready from its poll. The
 * scheduler's completion write is refused and asx_scheduler_run reports
 * it; the word is not overwritten with Completed and nothing of the
 * completion happens (no transition trace, no outcome). Before the checked
 * write, only the ghost monitor saw this and the task completed. */
TEST(scheduler_refuses_to_complete_a_task_whose_state_is_corrupt) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_slot *slot;
    asx_outcome out;
    asx_budget budget;
    uint64_t seen[4];
    uint32_t mark;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(tid, &slot), ASX_OK);
    slot->state = (asx_task_state)6;

    mark = asx_trace_event_count();
    budget = asx_budget_from_polls(4);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_slot_lookup(tid, &slot), ASX_OK);
    ASSERT_EQ((int)slot->state, 6);
    ASSERT_EQ(task_transitions_since(mark, tid, seen, 4u), (uint32_t)0u);
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_E_TASK_NOT_COMPLETED);
#ifdef ASX_DEBUG_GHOST
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)1u);
#endif
    slot->state = ASX_TASK_CREATED;
}

int main(void) {
    test_log_open("unit", "runtime/cancellation", "test_cancellation");
    fprintf(stderr, "=== test_cancellation (runtime) ===\n");

    RUN_TEST(cancel_running_task_transitions_to_cancel_requested);
    RUN_TEST(cancel_created_task_goes_straight_to_cancel_requested);
    RUN_TEST(budget_cancel_of_a_created_task_materializes_in_one_step);
    RUN_TEST(checkpoint_advances_to_cancelling);
    RUN_TEST(checkpoint_non_cancelled_task_reports_clean);
    RUN_TEST(finalize_transitions_cancelling_to_finalizing);
    RUN_TEST(finalize_rejects_wrong_state);
    RUN_TEST(cancel_strengthen_higher_severity_upgrades);
    RUN_TEST(cancel_completed_task_is_noop);
    RUN_TEST(cancel_propagation_cancels_all_tasks_in_region);
    RUN_TEST(cancel_propagation_skips_completed_tasks);
    RUN_TEST(cancelled_task_completion_gets_cancelled_outcome);
    RUN_TEST(natural_return_after_acknowledge_completes_through_finalizing);
    RUN_TEST(error_return_after_acknowledge_completes_cancelled_through_finalizing);
    RUN_TEST(finalizing_is_entered_once_when_the_task_finalizes_itself);
    RUN_TEST(panic_during_cleanup_completes_straight_from_cancelling);
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
    RUN_TEST(mask_depth_is_a_nesting_depth_not_a_consumed_deferral_budget);
    RUN_TEST(masked_task_is_not_force_completed);
    RUN_TEST(unmasked_task_is_force_completed_by_cleanup_budget);
    RUN_TEST(masked_callback_restores_on_success_error_and_pending_returns);
    RUN_TEST(masked_callback_nested_error_restores_each_level_and_delivers_cancel);
    RUN_TEST(masked_callback_preserves_an_existing_raw_mask);
    RUN_TEST(masked_callback_refuses_excess_depth_without_running_body);
    RUN_TEST(masked_callback_rejects_null_terminal_and_stale_admission);
    RUN_TEST(masked_callback_reports_a_failed_restoration);
    RUN_TEST(mask_depth_is_bounded);
    RUN_TEST(mask_refuses_a_completed_task_and_a_handle_from_a_reset_runtime);
    RUN_TEST(nested_masked_work_drains_after_the_outer_mask_while_a_joiner_waits);
    RUN_TEST(a_mask_never_released_keeps_the_cancel_out_and_the_joiner_waiting);
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
    RUN_TEST(cx_is_cancelled_reports_whether_a_cancel_was_requested);
    RUN_TEST(budget_cancel_is_attributed_to_the_task_and_records_no_request);
    RUN_TEST(obligation_abort_with_reason_records_it);
    RUN_TEST(cleanup_within_its_quota_completes_with_the_cancel_reason);
    RUN_TEST(cleanup_past_its_quota_is_strengthened_not_force_completed);
    RUN_TEST(unacknowledged_cancel_is_never_force_completed);
    RUN_TEST(acknowledgement_replaces_the_budget_with_the_cleanup_budget);
    RUN_TEST(cancel_during_cleanup_meets_and_refills_the_budget);
    RUN_TEST(get_cleanup_budget_reports_the_met_budget_of_a_pending_cancel);
    RUN_TEST(budget_cancel_stays_off_the_record_until_acknowledged);
    RUN_TEST(cancel_request_newly_cancels_a_record_with_a_pending_budget_cancel);
    RUN_TEST(cleanup_hard_bound_config_is_validated);
    RUN_TEST(task_state_write_applies_the_transition_table_in_every_build);
    RUN_TEST(scheduler_refuses_to_complete_a_task_whose_state_is_corrupt);

    TEST_REPORT();
    test_log_close();
    return test_failures;
}
