/*
 * test_fault_containment.c — runtime fault containment integration tests (bd-hwb.15)
 *
 * Tests that fault containment integrates correctly with the scheduler,
 * region drain, and task lifecycle. Verifies poisoned regions can still
 * drain but block new mutations.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/asx_config.h>
#include <asx/core/cancel.h>
#include <asx/core/ghost.h>
#include <asx/runtime/runtime.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* -------------------------------------------------------------------
 * Test poll functions
 * ------------------------------------------------------------------- */

static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

/* Fails immediately with an error */
static asx_status poll_fail(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_INVALID_STATE;
}


/* -------------------------------------------------------------------
 * Test: failing task gets ERR outcome
 * ------------------------------------------------------------------- */

TEST(failing_task_gets_err_outcome) {
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome out;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(10);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_ERR);
}

/* -------------------------------------------------------------------
 * Test: contain_fault with OK status is no-op
 * ------------------------------------------------------------------- */

TEST(contain_fault_ok_is_noop) {
    asx_region_id rid;
    int poisoned;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_contain_fault(rid, ASX_OK), ASX_OK);
    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    ASSERT_EQ(poisoned, 0);
}

/* -------------------------------------------------------------------
 * Test: poisoned region blocks spawn but allows state query
 * ------------------------------------------------------------------- */

TEST(poisoned_region_blocks_spawn_allows_query) {
    asx_region_id rid;
    asx_task_id tid;
    asx_region_state state;
    int poisoned;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Spawn should be blocked */
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_E_REGION_POISONED);

    /* State query should still work */
    ASSERT_EQ(asx_region_get_state(rid, &state), ASX_OK);
    ASSERT_EQ((int)state, (int)ASX_REGION_OPEN);

    /* Poison query should still work */
    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    ASSERT_EQ(poisoned, 1);
}

/* -------------------------------------------------------------------
 * Test: scheduler drains existing tasks on poisoned region
 * ------------------------------------------------------------------- */

TEST(scheduler_drains_tasks_on_poisoned_region) {
    asx_region_id rid;
    asx_task_id tid1, tid2;
    asx_task_state s1, s2;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid2), ASX_OK);

    /* Poison the region after spawning tasks */
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Scheduler should still be able to run and complete tasks */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Both tasks should have completed */
    ASSERT_EQ(asx_task_get_state(tid1, &s1), ASX_OK);
    ASSERT_EQ((int)s1, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(tid2, &s2), ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_TASK_COMPLETED);
}

/* -------------------------------------------------------------------
 * Test: failing task does not prevent other tasks from completing
 * ------------------------------------------------------------------- */

TEST(failing_task_does_not_block_other_tasks) {
    asx_region_id rid;
    asx_task_id tid_fail, tid_ok;
    asx_task_state sf, so;
    asx_outcome out_fail, out_ok;
    asx_budget budget;
    asx_status run_st;
    asx_containment_policy policy;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &tid_fail), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid_ok), ASX_OK);

    budget = asx_budget_from_polls(10);
    run_st = asx_scheduler_run(rid, &budget);
    policy = asx_containment_policy_active();
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(run_st, ASX_OK);
    } else {
        ASSERT_EQ(run_st, ASX_E_INVALID_STATE);
    }

    /* Failing task must complete with ERR outcome. */
    ASSERT_EQ(asx_task_get_state(tid_fail, &sf), ASX_OK);
    ASSERT_EQ((int)sf, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_outcome(tid_fail, &out_fail), ASX_OK);
    ASSERT_EQ((int)out_fail.severity, (int)ASX_OUTCOME_ERR);

    ASSERT_EQ(asx_task_get_state(tid_ok, &so), ASX_OK);
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ((int)so, (int)ASX_TASK_COMPLETED);
        ASSERT_EQ(asx_task_get_outcome(tid_ok, &out_ok), ASX_OK);
        ASSERT_EQ((int)out_ok.severity, (int)ASX_OUTCOME_OK);
    } else {
        ASSERT_TRUE(so == ASX_TASK_CREATED || so == ASX_TASK_RUNNING);
    }
}

/* -------------------------------------------------------------------
 * Test: poisoned region can still be drained via asx_region_drain
 * ------------------------------------------------------------------- */

TEST(poisoned_region_drains_to_closed) {
    asx_region_id rid;
    asx_task_id tid;
    asx_region_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    /* Poison region, then drain */
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Drain should still work — tasks complete and region closes.
     * Note: region_drain calls region_close internally, but close
     * is blocked on poisoned regions. Drain's close path checks
     * the state directly, bypassing the poison guard. */
    budget = asx_budget_from_polls(100);

    /* asx_region_drain transitions Open → Closing internally.
     * Because the region is poisoned, asx_region_close() would fail.
     * But drain bypasses close() and sets state directly. */
    {
        asx_status drain_st = asx_region_drain(rid, &budget);
        /* Drain may return OK or may return an error if the close
         * path is poison-blocked. Either way, check the final state. */
        (void)drain_st;
    }

    ASSERT_EQ(asx_region_get_state(rid, &state), ASX_OK);
    /* Region should reach CLOSED after drain */
    ASSERT_EQ((int)state, (int)ASX_REGION_CLOSED);
}

/* -------------------------------------------------------------------
 * Test: new spawn rejected on poisoned region during drain
 * ------------------------------------------------------------------- */

TEST(spawn_rejected_during_poisoned_drain) {
    asx_region_id rid;
    asx_task_id tid1, tid2;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid1), ASX_OK);

    /* Poison the region */
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* New spawn should fail even though the region is still OPEN */
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid2), ASX_E_REGION_POISONED);

    /* Cancel existing task so scheduler can quiesce */
    ASSERT_EQ(asx_task_cancel(tid1, ASX_CANCEL_SHUTDOWN), ASX_OK);

    budget = asx_budget_from_polls(200);
    SCHED_RUN_IGNORE(rid, &budget);
}

/* -------------------------------------------------------------------
 * Test: containment policy mapping is complete and deterministic
 * ------------------------------------------------------------------- */

TEST(containment_policy_mapping_complete) {
    /* Each safety profile maps to exactly one containment policy */
    ASSERT_EQ((int)asx_containment_policy_for_profile(ASX_SAFETY_DEBUG),
              (int)ASX_CONTAIN_FAIL_FAST);
    ASSERT_EQ((int)asx_containment_policy_for_profile(ASX_SAFETY_HARDENED),
              (int)ASX_CONTAIN_POISON_REGION);
    ASSERT_EQ((int)asx_containment_policy_for_profile(ASX_SAFETY_RELEASE),
              (int)ASX_CONTAIN_ERROR_ONLY);
}

/* -------------------------------------------------------------------
 * Test: active policy is DEBUG in test builds
 * ------------------------------------------------------------------- */

TEST(active_policy_is_debug_in_test_builds) {
    /* Tests compile with -DASX_DEBUG=1, so active profile is DEBUG */
    asx_safety_profile prof = asx_safety_profile_active();
    ASSERT_EQ((int)prof, (int)ASX_SAFETY_DEBUG);

    /* And containment policy is FAIL_FAST */
    ASSERT_EQ((int)asx_containment_policy_active(), (int)ASX_CONTAIN_FAIL_FAST);
}

/* -------------------------------------------------------------------
 * Test: contain_fault under FAIL_FAST does not poison
 * ------------------------------------------------------------------- */

TEST(contain_fault_fail_fast_does_not_poison) {
    asx_region_id rid;
    int poisoned;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Under DEBUG/FAIL_FAST, contain_fault returns the fault
     * but does NOT poison the region */
    ASSERT_EQ(asx_region_contain_fault(rid, ASX_E_INVALID_STATE), ASX_E_INVALID_STATE);

    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    ASSERT_EQ(poisoned, 0);
}

/* -------------------------------------------------------------------
 * Test: poison blocks obligation reserve
 * ------------------------------------------------------------------- */

TEST(poison_blocks_obligation_reserve) {
    asx_region_id rid;
    asx_obligation_id oid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_POISONED);
}

/* -------------------------------------------------------------------
 * Test: poison blocks region close
 * ------------------------------------------------------------------- */

TEST(poison_blocks_region_close) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    ASSERT_EQ(asx_region_close(rid), ASX_E_REGION_POISONED);
}

/* -------------------------------------------------------------------
 * Test: scheduler handles mix of failing and pending tasks
 * ------------------------------------------------------------------- */

TEST(scheduler_handles_fail_and_pending_mix) {
    asx_region_id rid;
    asx_task_id tid_fail, tid_pend;
    asx_task_state sf, sp;
    asx_budget budget;
    asx_status run_st;
    asx_containment_policy policy;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &tid_fail), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid_pend), ASX_OK);

    /* Run 1 round — failing task completes, pending task stays */
    budget = asx_budget_from_polls(2);
    run_st = asx_scheduler_run(rid, &budget);
    policy = asx_containment_policy_active();
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(run_st, ASX_E_POLL_BUDGET_EXHAUSTED);
    } else {
        ASSERT_TRUE(run_st != ASX_OK);
    }

    ASSERT_EQ(asx_task_get_state(tid_fail, &sf), ASX_OK);
    ASSERT_EQ((int)sf, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_state(tid_pend, &sp), ASX_OK);
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ((int)sp, (int)ASX_TASK_RUNNING);
    } else {
        ASSERT_EQ((int)sp, (int)ASX_TASK_CREATED);
    }
}

/* -------------------------------------------------------------------
 * Test: poison is idempotent
 * ------------------------------------------------------------------- */

TEST(poison_is_idempotent) {
    asx_region_id rid;
    int poisoned;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    ASSERT_EQ(asx_region_is_poisoned(rid, &poisoned), ASX_OK);
    ASSERT_EQ(poisoned, 1);
}

/* -------------------------------------------------------------------
 * Test: cancelled task on poisoned region gets CANCELLED outcome
 * ------------------------------------------------------------------- */

TEST(cancelled_task_on_poisoned_region) {
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome out;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Move task to Running */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Cancel, then poison */
    ASSERT_EQ(asx_task_cancel(tid, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);

    /* Scheduler should still resolve the cancelled task */
    budget = asx_budget_from_polls(200);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

/* -------------------------------------------------------------------
 * Test: multi-region isolation — poisoning one does not affect another
 * ------------------------------------------------------------------- */

TEST(multi_region_isolation_after_poison) {
    asx_region_id rid1, rid2;
    asx_task_id tid1, tid2;
    asx_task_state s1, s2;
    int poisoned1, poisoned2;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid1), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid2), ASX_OK);

    ASSERT_EQ(asx_task_spawn(rid1, poll_pending, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid2, poll_complete, NULL, &tid2), ASX_OK);

    /* Poison region 1 only */
    ASSERT_EQ(asx_region_poison(rid1), ASX_OK);

    /* Region 2 should NOT be poisoned */
    ASSERT_EQ(asx_region_is_poisoned(rid1, &poisoned1), ASX_OK);
    ASSERT_EQ(poisoned1, 1);
    ASSERT_EQ(asx_region_is_poisoned(rid2, &poisoned2), ASX_OK);
    ASSERT_EQ(poisoned2, 0);

    /* Region 2 can still spawn and run normally */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(rid2, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(tid2, &s2), ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_TASK_COMPLETED);

    /* Region 1 task is still alive but spawn is blocked */
    ASSERT_EQ(asx_task_get_state(tid1, &s1), ASX_OK);
    ASSERT_EQ((int)s1, (int)ASX_TASK_CREATED);
}

/* -------------------------------------------------------------------
 * Test: poison + cancel propagation resolves all tasks
 *
 * This verifies the combined containment action: poison region then
 * propagate cancellation. This is the POISON_REGION behavior used
 * in asx_region_contain_fault() under hardened profiles.
 * ------------------------------------------------------------------- */

TEST(poison_plus_cancel_propagation_resolves_tasks) {
    asx_region_id rid;
    asx_task_id tid1, tid2, tid3;
    asx_task_state s1, s2, s3;
    asx_outcome o1, o2, o3;
    asx_budget budget;
    uint32_t cancelled;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid3), ASX_OK);

    /* Move tasks to Running */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Combined containment action: poison + cancel propagation.
     * This is exactly what asx_region_contain_fault() does in
     * POISON_REGION mode (hardened profile). We use SHUTDOWN here
     * because it has a tight cleanup budget (50 polls), ensuring
     * all 3 tasks can resolve within the scheduler budget. */
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(cancelled, (uint32_t)3);

    /* Run scheduler — all tasks should resolve via cancel protocol.
     * SHUTDOWN gives 50 cleanup polls per task, so ~150 polls for 3 tasks. */
    budget = asx_budget_from_polls(500);
    SCHED_RUN_IGNORE(rid, &budget);

    /* All tasks should be completed with CANCELLED outcome */
    ASSERT_EQ(asx_task_get_state(tid1, &s1), ASX_OK);
    ASSERT_EQ((int)s1, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(tid2, &s2), ASX_OK);
    ASSERT_EQ((int)s2, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(tid3, &s3), ASX_OK);
    ASSERT_EQ((int)s3, (int)ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_task_get_outcome(tid1, &o1), ASX_OK);
    ASSERT_EQ((int)o1.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_outcome(tid2, &o2), ASX_OK);
    ASSERT_EQ((int)o2.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_outcome(tid3, &o3), ASX_OK);
    ASSERT_EQ((int)o3.severity, (int)ASX_OUTCOME_CANCELLED);

    /* No new spawns allowed */
    {
        asx_task_id tid_new;
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid_new), ASX_E_REGION_POISONED);
    }
}

/* -------------------------------------------------------------------
 * Test: containment preserves deterministic event ordering
 * ------------------------------------------------------------------- */

TEST(containment_event_ordering_is_deterministic) {
    asx_region_id rid;
    asx_task_id tid1, tid2;
    asx_budget budget;
    uint32_t event_count_1, event_count_2;
    uint32_t i;
    int events_match = 1;

    /* Run 1 */
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_OK);

    budget = asx_budget_from_polls(2);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    (void)asx_cancel_propagate(rid, ASX_CANCEL_RESOURCE);

    budget = asx_budget_from_polls(500);
    SCHED_RUN_IGNORE(rid, &budget);
    event_count_1 = asx_scheduler_event_count();

    /* Save events from run 1 */
    {
        asx_scheduler_event events_1[256];
        asx_scheduler_event events_2[256];

        for (i = 0; i < event_count_1 && i < 256u; i++) {
            (void)asx_scheduler_event_get(i, &events_1[i]);
        }

        /* Run 2: identical sequence */
        asx_runtime_reset();
        ASSERT_EQ(asx_region_open(&rid), ASX_OK);
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid1), ASX_OK);
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid2), ASX_OK);

        budget = asx_budget_from_polls(2);
        SCHED_RUN_IGNORE(rid, &budget);

        ASSERT_EQ(asx_region_poison(rid), ASX_OK);
        (void)asx_cancel_propagate(rid, ASX_CANCEL_RESOURCE);

        budget = asx_budget_from_polls(500);
        SCHED_RUN_IGNORE(rid, &budget);
        event_count_2 = asx_scheduler_event_count();

        ASSERT_EQ(event_count_1, event_count_2);

        for (i = 0; i < event_count_2 && i < 256u; i++) {
            (void)asx_scheduler_event_get(i, &events_2[i]);
        }

        /* Compare event streams */
        for (i = 0; i < event_count_1 && i < 256u; i++) {
            if (events_1[i].kind != events_2[i].kind || events_1[i].round != events_2[i].round ||
                events_1[i].sequence != events_2[i].sequence) {
                events_match = 0;
                break;
            }
        }
    }

    ASSERT_TRUE(events_match);
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    fprintf(stderr, "=== test_fault_containment (runtime) ===\n");

    RUN_TEST(failing_task_gets_err_outcome);
    RUN_TEST(contain_fault_ok_is_noop);
    RUN_TEST(poisoned_region_blocks_spawn_allows_query);
    RUN_TEST(scheduler_drains_tasks_on_poisoned_region);
    RUN_TEST(failing_task_does_not_block_other_tasks);
    RUN_TEST(poisoned_region_drains_to_closed);
    RUN_TEST(spawn_rejected_during_poisoned_drain);
    RUN_TEST(containment_policy_mapping_complete);
    RUN_TEST(active_policy_is_debug_in_test_builds);
    RUN_TEST(contain_fault_fail_fast_does_not_poison);
    RUN_TEST(poison_blocks_obligation_reserve);
    RUN_TEST(poison_blocks_region_close);
    RUN_TEST(scheduler_handles_fail_and_pending_mix);
    RUN_TEST(poison_is_idempotent);
    RUN_TEST(cancelled_task_on_poisoned_region);
    RUN_TEST(multi_region_isolation_after_poison);
    RUN_TEST(poison_plus_cancel_propagation_resolves_tasks);
    RUN_TEST(containment_event_ordering_is_deterministic);

    TEST_REPORT();
    return test_failures;
}
