/*
 * test_scheduler.c — unit tests for deterministic scheduler loop
 *
 * Tests: event sequencing, deterministic ordering, budget exhaustion,
 * round tracking, multi-task tie-break, and replay identity.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/ghost.h>

/* ---- Test poll functions ---- */

/* Completes immediately */
static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data; (void)self;
    return ASX_OK;
}

/* Yields N times then completes. Counter in user_data. */
static asx_status poll_yield_n(void *data, asx_task_id self) {
    int *counter = (int *)data;
    (void)self;
    if (*counter > 0) {
        (*counter)--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* Always pending (never completes) */
static asx_status poll_forever(void *data, asx_task_id self) {
    (void)data; (void)self;
    return ASX_E_PENDING;
}

/* Fails immediately */
static asx_status poll_fail(void *data, asx_task_id self) {
    (void)data; (void)self;
    return ASX_E_INVALID_STATE;
}

/* ---- Event sequence helpers ---- */

TEST(scheduler_single_task_immediate_complete) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_scheduler_event ev;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Should have: poll(0), complete(1), quiescent(2) */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)3);

    ASSERT_TRUE(asx_scheduler_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_POLL);
    ASSERT_EQ(ev.sequence, (uint32_t)0);
    ASSERT_EQ(ev.round, (uint32_t)0);

    ASSERT_TRUE(asx_scheduler_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_COMPLETE);
    ASSERT_EQ(ev.sequence, (uint32_t)1);

    ASSERT_TRUE(asx_scheduler_event_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_QUIESCENT);
    ASSERT_EQ(ev.sequence, (uint32_t)2);
}

TEST(scheduler_task_yields_then_completes) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    int counter = 2; /* yield twice then complete */

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &counter, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Round 0: poll(pending), round 1: poll(pending), round 2: poll+complete, quiescent */
    /* Expect 3 polls + 1 complete + 1 quiescent = 5 events */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)5);

    {
        asx_scheduler_event e0, e4;
        ASSERT_TRUE(asx_scheduler_event_get(0, &e0));
        ASSERT_EQ(e0.kind, ASX_SCHED_EVENT_POLL);
        ASSERT_EQ(e0.round, (uint32_t)0);

        ASSERT_TRUE(asx_scheduler_event_get(4, &e4));
        ASSERT_EQ(e4.kind, ASX_SCHED_EVENT_QUIESCENT);
    }
}

TEST(scheduler_budget_exhaustion_emits_event) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_scheduler_event ev;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_forever, NULL, &tid), ASX_OK);

    /* Only 2 poll units — task will exhaust budget */
    budget = asx_budget_infinite();
    budget.poll_quota = 2;
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    /* Events: poll(0), poll(1), budget(2) */
    ASSERT_TRUE(asx_scheduler_event_count() >= (uint32_t)2);

    {
        uint32_t last_idx = asx_scheduler_event_count() - 1u;
        ASSERT_TRUE(asx_scheduler_event_get(last_idx, &ev));
        ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_BUDGET);
    }
}

TEST(scheduler_task_failure_emits_complete) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_scheduler_event ev;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* poll(0), complete(1), quiescent(2) */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)3);

    ASSERT_TRUE(asx_scheduler_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_COMPLETE);
}

TEST(scheduler_multi_task_deterministic_order) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_scheduler_event e0, e1, e2;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* Spawn 3 tasks that all complete immediately */
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &t3), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* 3 tasks × (poll+complete) + quiescent = 7 events */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)7);

    /* First round: poll t1, complete t1, poll t2, complete t2, poll t3, complete t3 */
    ASSERT_TRUE(asx_scheduler_event_get(0, &e0));
    ASSERT_TRUE(asx_scheduler_event_get(2, &e1));
    ASSERT_TRUE(asx_scheduler_event_get(4, &e2));

    /* All polls should be from round 0 */
    ASSERT_EQ(e0.round, (uint32_t)0);
    ASSERT_EQ(e1.round, (uint32_t)0);
    ASSERT_EQ(e2.round, (uint32_t)0);

    /* Task slots should be in index order (deterministic tie-break).
     * State mask bits differ between spawn-time and poll-time handles,
     * so compare by slot index which is the stable identity. */
    ASSERT_EQ(asx_handle_slot(e0.task_id), asx_handle_slot(t1));
    ASSERT_EQ(asx_handle_slot(e1.task_id), asx_handle_slot(t2));
    ASSERT_EQ(asx_handle_slot(e2.task_id), asx_handle_slot(t3));

    /* Sequences are monotonic */
    ASSERT_EQ(e0.sequence, (uint32_t)0);
    ASSERT_EQ(e1.sequence, (uint32_t)2);
    ASSERT_EQ(e2.sequence, (uint32_t)4);
}

TEST(scheduler_replay_identity) {
    /* Run the same scenario twice and verify identical event streams */
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    int c1, c2;
    uint32_t count1;
    asx_scheduler_event events_run1[16];
    uint32_t i;

    /* --- Run 1 --- */
    asx_runtime_reset();
    asx_ghost_reset();
    c1 = 1; c2 = 0;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    count1 = asx_scheduler_event_count();
    ASSERT_TRUE(count1 <= 16u);
    for (i = 0; i < count1 && i < 16u; i++) {
        asx_scheduler_event_get(i, &events_run1[i]);
    }

    /* --- Run 2 (identical setup) --- */
    asx_runtime_reset();
    asx_ghost_reset();
    c1 = 1; c2 = 0;

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Event counts must match */
    ASSERT_EQ(asx_scheduler_event_count(), count1);

    /* Every event must match */
    for (i = 0; i < count1 && i < 16u; i++) {
        asx_scheduler_event e;
        ASSERT_TRUE(asx_scheduler_event_get(i, &e));
        ASSERT_EQ(e.kind, events_run1[i].kind);
        ASSERT_EQ(e.task_id, events_run1[i].task_id);
        ASSERT_EQ(e.sequence, events_run1[i].sequence);
        ASSERT_EQ(e.round, events_run1[i].round);
    }
}

TEST(scheduler_event_get_out_of_bounds) {
    asx_scheduler_event ev;
    asx_scheduler_event_reset();

    ASSERT_FALSE(asx_scheduler_event_get(0, &ev));
    ASSERT_FALSE(asx_scheduler_event_get(0, NULL));
}

TEST(scheduler_event_reset_clears) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_TRUE(asx_scheduler_event_count() > (uint32_t)0);

    asx_scheduler_event_reset();
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)0);
}

TEST(scheduler_round_tracking_multi_round) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    int counter = 3; /* yield 3 times */
    asx_scheduler_event e0, e1, e2;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &counter, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* 4 polls (3 pending + 1 complete) + 1 complete + 1 quiescent = 6 events */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)6);

    /* Verify rounds increment */
    ASSERT_TRUE(asx_scheduler_event_get(0, &e0));
    ASSERT_TRUE(asx_scheduler_event_get(1, &e1));
    ASSERT_TRUE(asx_scheduler_event_get(2, &e2));
    ASSERT_EQ(e0.round, (uint32_t)0);
    ASSERT_EQ(e1.round, (uint32_t)1);
    ASSERT_EQ(e2.round, (uint32_t)2);
}

TEST(scheduler_no_tasks_is_quiescent) {
    asx_region_id rid;
    asx_budget budget;
    asx_scheduler_event ev;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* No tasks spawned */

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Should just get quiescent event */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)1);
    ASSERT_TRUE(asx_scheduler_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_QUIESCENT);
}

int main(void) {
    fprintf(stderr, "=== test_scheduler ===\n");

    RUN_TEST(scheduler_single_task_immediate_complete);
    RUN_TEST(scheduler_task_yields_then_completes);
    RUN_TEST(scheduler_budget_exhaustion_emits_event);
    RUN_TEST(scheduler_task_failure_emits_complete);
    RUN_TEST(scheduler_multi_task_deterministic_order);
    RUN_TEST(scheduler_replay_identity);
    RUN_TEST(scheduler_event_get_out_of_bounds);
    RUN_TEST(scheduler_event_reset_clears);
    RUN_TEST(scheduler_round_tracking_multi_round);
    RUN_TEST(scheduler_no_tasks_is_quiescent);

    TEST_REPORT();
    return test_failures;
}
