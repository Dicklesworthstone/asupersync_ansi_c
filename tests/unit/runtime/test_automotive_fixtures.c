/*
 * test_automotive_fixtures.c — automotive deadline/watchdog fixture family (bd-1md.9)
 *
 * Implements the three canonical scenarios from VERT-AUTOMOTIVE-WATCHDOG:
 *   auto-watchdog-checkpoint-001: checkpoint success under bounded budget
 *   auto-degraded-transition-002: deterministic degraded-mode transition
 *   auto-deadline-miss-003:       explicit deadline miss classification
 *
 * Plus supporting assertions for deadline/checkpoint/watchdog transition
 * legality and deterministic replay evidence.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/outcome.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* ===================================================================
 * Poll functions for automotive scenarios
 * =================================================================== */

/* Completes immediately */
static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

/* Always yields (never completes on its own) */
static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* Checkpoint-aware: returns OK when cancelled, yields otherwise.
 * This models a cooperative automotive task that honors cancellation. */
static asx_status poll_checkpoint_cooperative(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

/* Checkpoint-aware: checkpoints but never completes (models a task
 * that fails to clean up in time — tests watchdog budget exhaustion). */
static asx_status poll_checkpoint_stubborn(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    asx_status st;
    (void)data;
    st = asx_checkpoint(self, &cr);
    (void)st;
    return ASX_E_PENDING;
}

/* Poll counter: tracks how many times polled (for mutation detection). */
typedef struct {
    int poll_count;
    int checkpoint_seen;
} poll_counter_state;

static asx_status poll_counting(void *data, asx_task_id self) {
    poll_counter_state *s = (poll_counter_state *)data;
    asx_checkpoint_result cr;

    s->poll_count++;

    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) {
        s->checkpoint_seen = 1;
        return ASX_OK;
    }
    return ASX_E_PENDING;
}

/* ===================================================================
 * SCENARIO: auto-watchdog-checkpoint-001
 * Checkpoint success under bounded budget
 *
 * A task runs within a comfortable budget, checkpoints regularly,
 * and completes normally without any cancellation trigger.
 * =================================================================== */

TEST(auto_watchdog_checkpoint_001_basic) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_task_state state;
    asx_outcome out;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    /* Run with generous budget — task completes in first poll */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Verify clean completion */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_OK);
}

TEST(auto_watchdog_checkpoint_001_checkpoint_clean) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_checkpoint_result cr;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Run one round to make task Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Checkpoint on uncancelled task reports clean */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_FALSE(cr.cancelled);
    ASSERT_EQ((int)cr.phase, 0);
}

TEST(auto_watchdog_checkpoint_001_budget_not_past_deadline) {
    asx_budget budget;
    asx_time now = 100;

    /* Budget with deadline in the future — not past */
    budget = asx_budget_infinite();
    budget.deadline = 200;
    ASSERT_FALSE(asx_budget_is_past_deadline(&budget, now));

    /* Budget with zero deadline (unconstrained) — never past */
    budget.deadline = ASX_TIME_ZERO;
    ASSERT_FALSE(asx_budget_is_past_deadline(&budget, now));
}

TEST(auto_watchdog_checkpoint_001_scheduler_quiescent) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    uint32_t i;
    int found_quiescent = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Verify QUIESCENT event emitted */
    for (i = 0; i < asx_scheduler_event_count(); i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_QUIESCENT) {
            found_quiescent = 1;
            break;
        }
    }
    ASSERT_TRUE(found_quiescent);
}

/* ===================================================================
 * SCENARIO: auto-degraded-transition-002
 * Deterministic degraded-mode transition
 *
 * A task is cancelled with ASX_CANCEL_DEADLINE. It transitions
 * through the full cancellation protocol. The transition reason
 * and phase are captured. Running twice produces identical event
 * sequences (deterministic replay).
 * =================================================================== */

TEST(auto_degraded_transition_002_phase_progression) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_task_state state;
    asx_checkpoint_result cr;
    asx_cancel_phase phase;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid), ASX_OK);

    /* Run once to make Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_RUNNING);

    /* Cancel with DEADLINE — severity 1 */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);

    /* Checkpoint advances to Cancelling */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_DEADLINE);
    ASSERT_EQ((int)cr.phase, (int)ASX_CANCEL_PHASE_CANCELLING);
    ASSERT_TRUE(cr.polls_remaining > 0);

    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCELLING);

    /* Phase query confirms Cancelling */
    ASSERT_EQ(asx_task_get_cancel_phase(tid, &phase), ASX_OK);
    ASSERT_EQ((int)phase, (int)ASX_CANCEL_PHASE_CANCELLING);
}

TEST(auto_degraded_transition_002_cooperative_completion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_task_state state;
    asx_outcome out;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid), ASX_OK);

    /* Run once to make Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel with DEADLINE */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    /* Run scheduler — cooperative task checkpoints and completes */
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Verify degraded-mode: CANCELLED outcome */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

TEST(auto_degraded_transition_002_deterministic_events) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    uint32_t event_count_run1;
    uint32_t event_count_run2;

    /* First run */
    asx_runtime_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    event_count_run1 = asx_scheduler_event_count();

    /* Second run — identical scenario */
    asx_runtime_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    event_count_run2 = asx_scheduler_event_count();

    /* Deterministic: same event count */
    ASSERT_EQ(event_count_run1, event_count_run2);
}

TEST(auto_degraded_transition_002_deadline_severity) {
    /* DEADLINE cancel has severity 1 — same as TIMEOUT */
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_DEADLINE), 1);
    ASSERT_EQ(asx_cancel_severity(ASX_CANCEL_TIMEOUT), 1);

    /* Severity ordering: USER(0) < DEADLINE(1) < POLL_QUOTA(2) < SHUTDOWN(5) */
    ASSERT_TRUE(asx_cancel_severity(ASX_CANCEL_USER) < asx_cancel_severity(ASX_CANCEL_DEADLINE));
    ASSERT_TRUE(asx_cancel_severity(ASX_CANCEL_DEADLINE) <
                asx_cancel_severity(ASX_CANCEL_POLL_QUOTA));
}

TEST(auto_degraded_transition_002_cleanup_budget_adequate) {
    asx_budget deadline_b = asx_cancel_cleanup_budget(ASX_CANCEL_DEADLINE);

    /* DEADLINE cleanup budget should provide nonzero poll quota */
    ASSERT_TRUE(deadline_b.poll_quota > 0);

    /* DEADLINE budget should be more generous than SHUTDOWN */
    ASSERT_TRUE(deadline_b.poll_quota >= asx_cancel_cleanup_budget(ASX_CANCEL_SHUTDOWN).poll_quota);
}

/* ===================================================================
 * SCENARIO: auto-deadline-miss-003
 * Explicit deadline miss classification
 *
 * A budget's deadline expires. The task is cancelled via DEADLINE.
 * The watchdog (scheduler budget exhaustion) forces completion if
 * the task fails to cooperate. No partial mutation occurs.
 * =================================================================== */

TEST(auto_deadline_miss_003_budget_past_deadline) {
    asx_budget budget;
    asx_time now = 500;

    /* Budget with deadline already past */
    budget = asx_budget_infinite();
    budget.deadline = 100;
    ASSERT_TRUE(asx_budget_is_past_deadline(&budget, now));

    /* Budget with deadline exactly at now — past */
    budget.deadline = now;
    ASSERT_TRUE(asx_budget_is_past_deadline(&budget, now));
}

TEST(auto_deadline_miss_003_forced_completion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_task_state state;
    asx_outcome out;
    asx_checkpoint_result cr;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* Stubborn task: checkpoints but never completes voluntarily */
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_stubborn, NULL, &tid), ASX_OK);

    /* Run once to make Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel with DEADLINE */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    /* Checkpoint to transition to Cancelling */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_DEADLINE);

    /* Run scheduler with large budget — cleanup budget (500 polls for
     * DEADLINE severity 1) will exhaust, forcing task completion.
     * Need >500 scheduler polls to cover cleanup + force-complete check. */
    budget = asx_budget_from_polls(1000);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Task should be force-completed */
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_COMPLETED);

    /* Outcome should be CANCELLED (not OK or PANICKED) */
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

TEST(auto_deadline_miss_003_cancel_forced_event) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_checkpoint_result cr;
    uint32_t i;
    int found_forced = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_stubborn, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Use SHUTDOWN (50-poll cleanup budget) so the CANCEL_FORCED event
     * falls within the 256-entry scheduler event log. DEADLINE's 500-poll
     * cleanup budget would overflow the log before the event is recorded. */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);

    budget = asx_budget_from_polls(200);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Verify CANCEL_FORCED event in scheduler log */
    for (i = 0; i < asx_scheduler_event_count(); i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_CANCEL_FORCED) {
            found_forced = 1;
            break;
        }
    }
    ASSERT_TRUE(found_forced);
}

TEST(auto_deadline_miss_003_no_partial_mutation) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    poll_counter_state counter;
    asx_outcome out;
    int polls_before_cancel;

    counter.poll_count = 0;
    counter.checkpoint_seen = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_counting, &counter, &tid), ASX_OK);

    /* Run once to make Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    polls_before_cancel = counter.poll_count;

    /* Cancel with DEADLINE */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    /* Run scheduler — task sees checkpoint, completes cleanly */
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Task must have been polled at least once more (to observe cancellation) */
    ASSERT_TRUE(counter.poll_count > polls_before_cancel);

    /* Checkpoint was observed by the task */
    ASSERT_TRUE(counter.checkpoint_seen);

    /* Outcome is CANCELLED — clean classification, not partial/errored */
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

/* ===================================================================
 * Supporting assertions: deadline/checkpoint/watchdog transition legality
 * =================================================================== */

TEST(deadline_cancel_transitions_are_legal) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_task_state state;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Created → Running (via scheduler poll) */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_RUNNING);

    /* Running → CancelRequested (via cancel) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCEL_REQUESTED);

    /* CancelRequested → Cancelling (via checkpoint) */
    {
        asx_checkpoint_result cr;
        ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    }
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_CANCELLING);

    /* Cancelling → Finalizing (via finalize) */
    ASSERT_EQ(asx_task_finalize(tid), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_TASK_FINALIZING);
}

TEST(deadline_strengthen_to_shutdown) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_checkpoint_result cr;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* First cancel: DEADLINE (severity 1) */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);

    /* Second cancel: SHUTDOWN (severity 5) — should strengthen */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);

    /* Checkpoint to observe — kind should be SHUTDOWN (stronger) */
    ASSERT_EQ(asx_checkpoint(tid, &cr), ASX_OK);
    ASSERT_TRUE(cr.cancelled);
    ASSERT_EQ((int)cr.kind, (int)ASX_CANCEL_SHUTDOWN);
}

TEST(budget_poll_quota_exhaustion) {
    asx_budget budget;

    budget = asx_budget_from_polls(2);
    ASSERT_FALSE(asx_budget_is_exhausted(&budget));

    /* Consume first poll */
    ASSERT_TRUE(asx_budget_consume_poll(&budget) > 0);
    ASSERT_FALSE(asx_budget_is_exhausted(&budget));

    /* Consume second poll */
    ASSERT_TRUE(asx_budget_consume_poll(&budget) > 0);
    ASSERT_TRUE(asx_budget_is_exhausted(&budget));

    /* Third consume: already exhausted */
    ASSERT_EQ(asx_budget_consume_poll(&budget), (uint32_t)0);
}

TEST(budget_meet_tightens_deadline) {
    asx_budget a, b, m;

    a = asx_budget_infinite();
    a.deadline = 100;
    a.poll_quota = 50;

    b = asx_budget_infinite();
    b.deadline = 200;
    b.poll_quota = 30;

    m = asx_budget_meet(&a, &b);

    /* Meet takes the tighter (smaller non-zero) deadline */
    ASSERT_EQ((uint64_t)m.deadline, (uint64_t)100);
    /* Meet takes the smaller poll quota */
    ASSERT_EQ(m.poll_quota, (uint32_t)30);
}

TEST(watchdog_region_containment_after_deadline) {
    asx_region_id rid;
    asx_task_id tid;
    int poisoned = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Poison the region (simulating deadline watchdog triggering containment) */
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Region should be poisoned */
    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    ASSERT_TRUE(poisoned);

    /* Further spawn should fail with REGION_POISONED */
    {
        asx_task_id tid2;
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_E_REGION_POISONED);
    }

    /* But querying state still works (read-only operations survive poison) */
    {
        asx_task_state state;
        ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    }
}

TEST(scheduler_budget_event_on_exhaustion) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    uint32_t i;
    int found_budget = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Run with exactly 2 polls — task never completes, budget exhausts */
    budget = asx_budget_from_polls(2);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Verify BUDGET event emitted */
    for (i = 0; i < asx_scheduler_event_count(); i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_BUDGET) {
            found_budget = 1;
            break;
        }
    }
    ASSERT_TRUE(found_budget);
}

TEST(deterministic_trace_digest_across_runs) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    uint64_t digest1;
    uint64_t digest2;

    /* First run */
    asx_runtime_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    digest1 = asx_trace_digest();

    /* Second identical run */
    asx_runtime_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_cooperative, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_DEADLINE), ASX_OK);
    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    digest2 = asx_trace_digest();

    /* Deterministic: identical scenarios produce identical digests */
    ASSERT_EQ(digest1, digest2);
}

/* ===================================================================
 * Main
 * =================================================================== */

int main(void) {
    fprintf(stderr, "=== test_automotive_fixtures (bd-1md.9) ===\n");

    /* auto-watchdog-checkpoint-001 */
    RUN_TEST(auto_watchdog_checkpoint_001_basic);
    RUN_TEST(auto_watchdog_checkpoint_001_checkpoint_clean);
    RUN_TEST(auto_watchdog_checkpoint_001_budget_not_past_deadline);
    RUN_TEST(auto_watchdog_checkpoint_001_scheduler_quiescent);

    /* auto-degraded-transition-002 */
    RUN_TEST(auto_degraded_transition_002_phase_progression);
    RUN_TEST(auto_degraded_transition_002_cooperative_completion);
    RUN_TEST(auto_degraded_transition_002_deterministic_events);
    RUN_TEST(auto_degraded_transition_002_deadline_severity);
    RUN_TEST(auto_degraded_transition_002_cleanup_budget_adequate);

    /* auto-deadline-miss-003 */
    RUN_TEST(auto_deadline_miss_003_budget_past_deadline);
    RUN_TEST(auto_deadline_miss_003_forced_completion);
    RUN_TEST(auto_deadline_miss_003_cancel_forced_event);
    RUN_TEST(auto_deadline_miss_003_no_partial_mutation);

    /* Supporting deadline/checkpoint/watchdog assertions */
    RUN_TEST(deadline_cancel_transitions_are_legal);
    RUN_TEST(deadline_strengthen_to_shutdown);
    RUN_TEST(budget_poll_quota_exhaustion);
    RUN_TEST(budget_meet_tightens_deadline);
    RUN_TEST(watchdog_region_containment_after_deadline);
    RUN_TEST(scheduler_budget_event_on_exhaustion);
    RUN_TEST(deterministic_trace_digest_across_runs);

    TEST_REPORT();
    return test_failures;
}
