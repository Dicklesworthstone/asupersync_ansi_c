/*
 * test_scheduler.c — unit tests for deterministic scheduler loop
 *
 * Tests: event sequencing, deterministic ordering, budget exhaustion,
 * round tracking, multi-task tie-break, and replay identity.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "../../test_runtime.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/runtime.h>

/* ---- Test poll functions ---- */

/* Completes immediately */
static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
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
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* Fails immediately */
static asx_status poll_fail(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

/* ---- Event sequence helpers ---- */

TEST(scheduler_single_task_immediate_complete) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_scheduler_event ev;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    asx_status st;
    asx_containment_policy policy;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fail, NULL, &tid), ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    policy = asx_containment_policy_active();
    if (policy == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(st, ASX_OK);
        ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)3);
    } else {
        ASSERT_EQ(st, ASX_E_INVALID_STATE);
        ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)2);
    }

    /* poll(0), complete(1), [optional quiescent(2)] */
    ASSERT_TRUE(asx_scheduler_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_POLL);

    ASSERT_TRUE(asx_scheduler_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_COMPLETE);
}

TEST(scheduler_multi_task_deterministic_order) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_scheduler_event e0, e1, e2;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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
    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    c1 = 1;
    c2 = 0;

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c1, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &c2, &t2), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    count1 = asx_scheduler_event_count();
    ASSERT_TRUE(count1 <= 16u);
    for (i = 0; i < count1 && i < 16u; i++) { asx_scheduler_event_get(i, &events_run1[i]); }

    /* --- Run 2 (identical setup) --- */
    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    c1 = 1;
    c2 = 0;

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
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

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    /* No tasks spawned */

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Should just get quiescent event */
    ASSERT_EQ(asx_scheduler_event_count(), (uint32_t)1);
    ASSERT_TRUE(asx_scheduler_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_SCHED_EVENT_QUIESCENT);
}

TEST(scheduler_trace_emits_round_markers) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_trace_event ev;
    int counter = 1; /* round 0 pending, round 1 complete + quiescent */
    uint32_t round_count = 0;
    uint32_t i;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_yield_n, &counter, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    for (i = 0; i < asx_trace_event_count(); i++) {
        ASSERT_TRUE(asx_trace_event_get(i, &ev));
        if (ev.kind != ASX_TRACE_SCHED_ROUND) continue;

        ASSERT_EQ(ev.entity_id, (uint64_t)ASX_INVALID_ID);
        ASSERT_EQ(ev.aux, (uint64_t)round_count);
        round_count++;
    }

    ASSERT_EQ(round_count, (uint32_t)2);
}

/* ---- Dispatch order within a round (bd-9kll.4.6) ----
 *
 * Rust's scheduler dispatches the cancel lane before the ready lane, a
 * lane's entries by priority and then by wake order (priority.rs:546-560),
 * and gives the ready lane a turn after cancel_streak_limit (16) cancel
 * dispatches in a row. The sweep has no wake order; arena order stands in. */

/* The slot indices of the last run's POLL events, in order. */
static uint32_t poll_order(uint32_t *out, uint32_t cap) {
    uint32_t n = 0;
    uint32_t i;
    for (i = 0; i < asx_scheduler_event_count() && n < cap; i++) {
        asx_scheduler_event ev;
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_POLL) {
            out[n++] = asx_handle_slot(ev.task_id);
        }
    }
    return n;
}

static asx_budget budget_with_priority(uint8_t priority) {
    asx_budget b = asx_budget_new();
    b.priority = priority;
    return b;
}

TEST(scheduler_polls_higher_budget_priority_first) {
    asx_region_id rid;
    asx_task_id low, high;
    asx_budget budget;
    asx_budget b_low = budget_with_priority(10u);
    asx_budget b_high = budget_with_priority(200u);
    int c_low = 2;
    int c_high = 2;
    uint32_t order[16];
    uint32_t n;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    /* The low-priority task is first in arena order. */
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_yield_n, &c_low, &b_low, &low), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_yield_n, &c_high, &b_high, &high), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* Three rounds of two polls: the high-priority task goes first in each. */
    n = poll_order(order, 16u);
    ASSERT_EQ(n, 6u);
    ASSERT_EQ(order[0], (uint32_t)asx_handle_slot(high));
    ASSERT_EQ(order[1], (uint32_t)asx_handle_slot(low));
    ASSERT_EQ(order[2], (uint32_t)asx_handle_slot(high));
    ASSERT_EQ(order[3], (uint32_t)asx_handle_slot(low));
    ASSERT_EQ(order[4], (uint32_t)asx_handle_slot(high));
    ASSERT_EQ(order[5], (uint32_t)asx_handle_slot(low));
}

TEST(scheduler_polls_three_priorities_highest_first_arena_order_within_one) {
    asx_region_id rid;
    asx_task_id t[4];
    asx_budget budget;
    asx_budget b50 = budget_with_priority(50u);
    asx_budget b200 = budget_with_priority(200u);
    uint32_t order[8];

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_complete, NULL, &b50, &t[0]), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_complete, NULL, &b200, &t[1]), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_complete, NULL, &b50, &t[2]), ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_complete, NULL, &b200, &t[3]), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(poll_order(order, 8u), 4u);
    ASSERT_EQ(order[0], (uint32_t)asx_handle_slot(t[1]));
    ASSERT_EQ(order[1], (uint32_t)asx_handle_slot(t[3]));
    ASSERT_EQ(order[2], (uint32_t)asx_handle_slot(t[0]));
    ASSERT_EQ(order[3], (uint32_t)asx_handle_slot(t[2]));
}

TEST(scheduler_polls_the_cancel_lane_before_the_ready_lane) {
    asx_region_id rid;
    asx_task_id a, b, c;
    asx_budget budget;
    uint32_t order[8];

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &a), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &b), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &c), ASX_OK);
    ASSERT_EQ(asx_task_cancel(c, ASX_CANCEL_USER), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    /* c is last in arena order but is cancel work. */
    ASSERT_EQ(poll_order(order, 8u), 3u);
    ASSERT_EQ(order[0], (uint32_t)asx_handle_slot(c));
    ASSERT_EQ(order[1], (uint32_t)asx_handle_slot(a));
    ASSERT_EQ(order[2], (uint32_t)asx_handle_slot(b));
}

TEST(scheduler_orders_the_cancel_lane_by_cleanup_priority) {
    asx_region_id rid;
    asx_task_id a, b, c;
    asx_budget budget;
    uint32_t order[8];

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &a), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &b), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &c), ASX_OK);
    /* SHUTDOWN's cleanup priority (255) is above USER's (200). */
    ASSERT_EQ(asx_task_cancel(a, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_task_cancel(c, ASX_CANCEL_SHUTDOWN), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(poll_order(order, 8u), 3u);
    ASSERT_EQ(order[0], (uint32_t)asx_handle_slot(c));
    ASSERT_EQ(order[1], (uint32_t)asx_handle_slot(a));
    ASSERT_EQ(order[2], (uint32_t)asx_handle_slot(b));
}

TEST(scheduler_cancel_streak_gives_ready_work_a_turn) {
    /* cancel_streak_limit = 16: after 16 cancel-lane dispatches in a row
     * the ready lane is tried before the 17th. */
    enum { CANCELLED = 18 };
    asx_region_id rid;
    asx_task_id ready;
    asx_task_id c[CANCELLED];
    asx_budget budget;
    uint32_t order[32];
    uint32_t i;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &ready), ASX_OK);
    for (i = 0; i < (uint32_t)CANCELLED; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &c[i]), ASX_OK);
        ASSERT_EQ(asx_task_cancel(c[i], ASX_CANCEL_USER), ASX_OK);
    }

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(poll_order(order, 32u), (uint32_t)(CANCELLED + 1));
    for (i = 0; i < 16u; i++) { ASSERT_EQ(order[i], (uint32_t)asx_handle_slot(c[i])); }
    ASSERT_EQ(order[16], (uint32_t)asx_handle_slot(ready));
    ASSERT_EQ(order[17], (uint32_t)asx_handle_slot(c[16]));
    ASSERT_EQ(order[18], (uint32_t)asx_handle_slot(c[17]));
}

typedef struct {
    asx_region_id region;
    asx_task_id child;
} spawn_once_state;

static asx_status poll_spawn_once_then_complete(void *data, asx_task_id self) {
    spawn_once_state *s = (spawn_once_state *)data;
    (void)self;
    return asx_task_spawn(s->region, poll_complete, NULL, &s->child);
}

TEST(scheduler_polls_a_task_spawned_in_a_round_in_that_round) {
    asx_region_id rid;
    asx_task_id parent;
    asx_budget budget;
    spawn_once_state s;
    asx_scheduler_event ev;
    uint32_t polls_in_round_0 = 0;
    uint32_t i;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    s.region = rid;
    s.child = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(rid, poll_spawn_once_then_complete, &s, &parent), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    for (i = 0; i < asx_scheduler_event_count(); i++) {
        if (asx_scheduler_event_get(i, &ev) && ev.kind == ASX_SCHED_EVENT_POLL && ev.round == 0u) {
            polls_in_round_0++;
        }
    }
    /* The parent and the child it spawned. */
    ASSERT_EQ(polls_in_round_0, 2u);
}

/* ---- A task spawned into a region opened in that round (bd-9kll.4.12) ---- */

static char g_scope_log[8];
static uint32_t g_scope_n;

static void scope_note(char c) {
    if (g_scope_n < sizeof(g_scope_log)) g_scope_log[g_scope_n++] = c;
}

static asx_status poll_scope_child(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    scope_note('c');
    return ASX_OK;
}

typedef struct {
    asx_region_id parent;
    int stage;
} scope_opener;

/* Opens a child region, spawns a task into it and yields once ("o"); on its
 * next poll it completes ("O"). */
static asx_status poll_scope_opener(void *data, asx_task_id self) {
    scope_opener *o = (scope_opener *)data;
    asx_region_id child;
    asx_task_id tid;
    asx_status st;
    (void)self;
    if (o->stage == 0) {
        scope_note('o');
        st = asx_region_open_child(o->parent, &child);
        if (st != ASX_OK) return st;
        st = asx_task_spawn(child, poll_scope_child, NULL, &tid);
        if (st != ASX_OK) return st;
        o->stage = 1;
        return ASX_E_PENDING; /* a yield: the task stays runnable */
    }
    scope_note('O');
    return ASX_OK;
}

/* Rust's ready queue runs a spawned task before its spawner's yield returns.
 * The sweep took the round's scope before the walk, so a task spawned into a
 * region the spawner opened in that round waited for the next round and ran
 * after the spawner resumed. */
TEST(scheduler_polls_a_task_in_a_region_opened_in_the_round_before_its_opener_resumes) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    scope_opener o;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    g_scope_n = 0u;
    memset(g_scope_log, 0, sizeof(g_scope_log));
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    o.parent = rid;
    o.stage = 0;
    ASSERT_EQ(asx_task_spawn(rid, poll_scope_opener, &o, &tid), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);

    ASSERT_EQ(g_scope_n, 3u);
    ASSERT_TRUE(g_scope_log[0] == 'o');
    ASSERT_TRUE(g_scope_log[1] == 'c');
    ASSERT_TRUE(g_scope_log[2] == 'O');
}

/* ---- A cleanup task's time budget is advisory (bd-9kll.4.14) ---- */

/* A runtime with the default hooks, whose clock is the virtual one: a Sleep
 * reads the time through asx_runtime_now_ns, which is ASX_E_HOOK_MISSING (and
 * ends the task failed) when no hooks are installed. */
static asx_runtime g_rt;

static asx_status setup_with_clock(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_shutdown(&g_rt);
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    return st;
}

typedef struct {
    asx_sleep_state sleep;
    int started;
    int done;
    int cancel_seen;
} cleanup_probe;

/* Sleeps 150 ns, past its budget's deadline at 100; records whether a cancel
 * was pending when the sleep ended. */
static asx_status poll_cleanup_sleeps_past_its_deadline(void *data, asx_task_id self) {
    cleanup_probe *p = (cleanup_probe *)data;
    asx_cancel_reason reason;
    asx_status st;
    if (!p->started) {
        p->started = 1;
        st = asx_sleep_init(&p->sleep, 150u);
        if (st != ASX_OK) return st;
    }
    st = asx_sleep_poll(&p->sleep, self);
    if (st != ASX_OK) return st;
    p->cancel_seen = asx_task_get_cancel_reason(self, &reason) == ASX_OK;
    p->done = 1;
    return ASX_OK;
}

typedef struct {
    asx_region_id region;
    cleanup_probe probe;
} cleanup_spawner;

/* Spawns the probe as a cleanup task of its own region: spawned from a poll
 * there, it takes this task's budget, whose deadline is 100. */
static asx_status poll_spawn_cleanup_probe(void *data, asx_task_id self) {
    cleanup_spawner *s = (cleanup_spawner *)data;
    asx_task_id child;
    (void)self;
    return asx_task_spawn_cleanup(s->region, poll_cleanup_sleeps_past_its_deadline, &s->probe,
                                  &child);
}

/* Rust arms no deadline timer for a cleanup task (state.rs:4401), so one
 * that outlives its budget's deadline is not cancelled when it wakes; only a
 * checkpoint evaluates the budget (cx.rs:3112-3160). The sweep woke it at the
 * deadline and cancelled it before the poll. */
TEST(scheduler_does_not_cancel_a_cleanup_task_at_its_budget_deadline_before_it_polls) {
    asx_region_id rid;
    asx_task_id parent;
    asx_budget parent_budget;
    asx_budget run;
    cleanup_spawner s;

    ASSERT_EQ(setup_with_clock(), ASX_OK);
    asx_ghost_reset();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&g_rt, &rid), ASX_OK);
    s.region = rid;
    parent_budget = asx_budget_infinite();
    parent_budget.deadline = 100u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_spawn_cleanup_probe, &s, &parent_budget,
                                         &parent),
              ASX_OK);

    run = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);
    ASSERT_TRUE(s.probe.done);
    ASSERT_FALSE(s.probe.cancel_seen);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)150u);
    asx_runtime_shutdown(&g_rt);
}

/* ---- A passed budget deadline strengthens a pending cancel (bd-9kll.4.15) ---- */

typedef struct {
    asx_sleep_state sleep;
    int started;
    asx_task_id victim;
} canceller_state;

/* Sleeps 100 ns, then cancels its victim with User. */
static asx_status poll_cancel_after_sleep(void *data, asx_task_id self) {
    canceller_state *c = (canceller_state *)data;
    asx_status st;
    if (!c->started) {
        c->started = 1;
        st = asx_sleep_init(&c->sleep, 100u);
        if (st != ASX_OK) return st;
    }
    st = asx_sleep_poll(&c->sleep, self);
    if (st != ASX_OK) return st;
    return asx_task_cancel(c->victim, ASX_CANCEL_USER);
}

typedef struct {
    asx_sleep_state sleep;
    int started;
} sleeper_state;

/* Sleeps 150 ns. */
static asx_status poll_sleep_150(void *data, asx_task_id self) {
    sleeper_state *s = (sleeper_state *)data;
    asx_status st;
    if (!s->started) {
        s->started = 1;
        st = asx_sleep_init(&s->sleep, 150u);
        if (st != ASX_OK) return st;
    }
    return asx_sleep_poll(&s->sleep, self);
}

/* The victim's budget deadline (100) passes at the instant a task cancels it
 * with User. Rust's deadline timer fires at the deadline in the same step,
 * before either task polls, so the victim holds the stronger Deadline and its
 * sleep, which a Deadline cancel does not end, runs to 150. The sweep checked
 * the deadline only when it polled the victim, after the User cancel, so the
 * cancel ended the sleep at 100 (fixture cancel-region-request-lane-priority-001). */
TEST(scheduler_a_passed_deadline_strengthens_a_user_cancel_that_arrived_first) {
    asx_region_id rid;
    asx_task_id canceller;
    asx_task_id victim;
    asx_budget budget;
    asx_budget victim_budget;
    asx_cancel_reason reason;
    canceller_state c;
    sleeper_state s;

    ASSERT_EQ(setup_with_clock(), ASX_OK);
    asx_ghost_reset();
    memset(&c, 0, sizeof(c));
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&g_rt, &rid), ASX_OK);
    /* The canceller is first in arena order, so it runs before the victim in
     * the round both wake in. */
    ASSERT_EQ(asx_task_spawn(rid, poll_cancel_after_sleep, &c, &canceller), ASX_OK);
    victim_budget = asx_budget_infinite();
    victim_budget.deadline = 100u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_sleep_150, &s, &victim_budget, &victim), ASX_OK);
    c.victim = victim;

    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)150u);
    ASSERT_EQ(asx_task_get_cancel_reason(victim, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_DEADLINE);
    asx_runtime_shutdown(&g_rt);
}

/* ---- Budget deadlines fire before any task polls (bd-9kll.4.16) ---- */

typedef struct {
    asx_sleep_state sleep;
    int started;
    asx_task_id victim;
} aborter_state;

/* Sleeps to its own deadline (100), then aborts its victim with the reason its
 * own deadline cancelled it with, as a race's owner aborts its timeout wrapper
 * with its own cancel reason. */
static asx_status poll_abort_with_own_reason(void *data, asx_task_id self) {
    aborter_state *a = (aborter_state *)data;
    asx_checkpoint_result cr;
    asx_cancel_reason reason;
    asx_status st;
    if (!a->started) {
        a->started = 1;
        st = asx_sleep_init(&a->sleep, 100u);
        if (st != ASX_OK) return st;
    }
    st = asx_sleep_poll(&a->sleep, self);
    if (st != ASX_OK) return st;
    st = asx_checkpoint(self, &cr);
    if (st != ASX_OK) return st;
    if (!cr.cancelled) return ASX_E_INVALID_STATE;
    st = asx_task_get_cancel_reason(self, &reason);
    if (st != ASX_OK) return st;
    return asx_task_abort_request(a->victim, &reason);
}

/* Rust's budget-deadline timers all fire at the start of a step, before any
 * task polls, each cancelling its own task with its own origin. The sweep
 * enforced a deadline when it polled the task, so the task polled first
 * aborted the second with its reason, which was then the second's first
 * cancel and its origin: the tie of the two Deadlines at 100 kept it
 * (fixture race-deadline-wrapper-cx-reason-001). */
TEST(scheduler_budget_deadlines_fire_before_any_task_polls_in_the_round) {
    asx_region_id rid;
    asx_task_id aborter;
    asx_task_id victim;
    asx_budget budget;
    asx_budget tb;
    asx_cancel_reason reason;
    aborter_state a;
    sleeper_state s;

    ASSERT_EQ(setup_with_clock(), ASX_OK);
    asx_ghost_reset();
    memset(&a, 0, sizeof(a));
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&g_rt, &rid), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = 100u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_abort_with_own_reason, &a, &tb, &aborter),
              ASX_OK);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_sleep_150, &s, &tb, &victim), ASX_OK);
    a.victim = victim;

    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)150u);
    ASSERT_EQ(asx_task_get_cancel_reason(victim, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_DEADLINE);
    ASSERT_TRUE(asx_handle_index(reason.origin_task) == asx_handle_index(victim));
    asx_runtime_shutdown(&g_rt);
}

/* Parks until a cancel arrives, then returns, the cancel dominating. */
static asx_status poll_park_until_cancelled(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    asx_status st;
    (void)data;
    st = asx_checkpoint(self, &cr);
    if (st != ASX_OK) return st;
    if (cr.cancelled) return ASX_OK;
    return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
}

/* A task with a budget deadline parks with nothing else pending: no timer,
 * no other task. Rust's lab advances time to the budget-deadline timer and
 * cancels the task there. The sweep arms its own wake at the deadline when
 * the task parks (the round-start pass cannot help: nothing moves the clock
 * to the deadline, and the idle step finds no timer). Without the wake the run
 * ends ASX_E_WOULD_BLOCK with the task parked and uncancelled. No
 * Rust-captured fixture has this shape yet (scenario
 * budget-deadline-parked-barrier-001 asks for one), so the oracle's sweep
 * mutant drops the wake and the pass together; this test sees the wake
 * alone. */
TEST(scheduler_a_lone_parked_task_is_cancelled_at_its_budget_deadline) {
    asx_region_id rid;
    asx_task_id t;
    asx_budget budget;
    asx_budget tb;
    asx_cancel_reason reason;

    ASSERT_EQ(setup_with_clock(), ASX_OK);
    asx_ghost_reset();
    ASSERT_EQ(asx_region_open(&g_rt, &rid), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = 100u;
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_park_until_cancelled, NULL, &tb, &t), ASX_OK);

    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)100u);
    ASSERT_EQ(asx_task_get_cancel_reason(t, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_DEADLINE);
    ASSERT_EQ(reason.timestamp, (asx_time)100u);
    asx_runtime_shutdown(&g_rt);
}

/* ---- Poll quota enforced before a poll (bd-9kll.4.13) ---- */

typedef struct {
    uint32_t polls;
    int checkpoint;
    int saw_cancel;
    asx_cancel_reason reason;
} quota_probe;

/* Pends twice, so a quota of 2 is spent when its third poll starts, then
 * (optionally checkpointing first) reads the cancel reason raised for it. */
static asx_status poll_probe_quota_reason(void *data, asx_task_id self) {
    quota_probe *p = (quota_probe *)data;
    asx_checkpoint_result cr;
    p->polls++;
    if (p->polls < 3u) return ASX_E_PENDING;
    if (p->checkpoint) {
        asx_status st = asx_checkpoint(self, &cr);
        if (st != ASX_OK) return st;
    }
    p->saw_cancel = asx_task_get_cancel_reason(self, &p->reason) == ASX_OK;
    return ASX_OK;
}

/* Rust's lab (lab/runtime.rs:4667) and its production scheduler
 * (three_lane.rs consume_budget_poll) both stamp an exhausted poll quota
 * with CancelReason::poll_quota()'s testing defaults. */
TEST(scheduler_stamps_an_exhausted_poll_quota_with_the_poll_quota_defaults) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget quota;
    asx_budget run;
    asx_cancel_reason expect;
    quota_probe p;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    memset(&p, 0, sizeof(p));
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    quota = asx_budget_from_polls(2);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_probe_quota_reason, &p, &quota, &tid), ASX_OK);
    run = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);

    ASSERT_EQ(p.polls, 3u);
    ASSERT_TRUE(p.saw_cancel);
    expect = asx_cancel_reason_default(ASX_CANCEL_POLL_QUOTA, NULL);
    ASSERT_EQ((int)p.reason.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(p.reason.origin_region, expect.origin_region);
    ASSERT_EQ(p.reason.origin_task, expect.origin_task);
    ASSERT_EQ(p.reason.timestamp, expect.timestamp);
}

/* The default stamp is provisional: the task's own checkpoint re-attributes
 * it to the task and its region at the current time, the earlier timestamp
 * winning (Cx::checkpoint, cx.rs:3112-3160; bd-wxep). */
TEST(scheduler_poll_quota_stamp_is_reattributed_by_a_later_checkpoint) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget quota;
    asx_budget run;
    quota_probe p;

    asx_runtime_reset(test_runtime());
    asx_ghost_reset();
    memset(&p, 0, sizeof(p));
    p.checkpoint = 1;
    ASSERT_EQ(asx_region_open(test_runtime(), &rid), ASX_OK);
    quota = asx_budget_from_polls(2);
    ASSERT_EQ(asx_task_spawn_with_budget(rid, poll_probe_quota_reason, &p, &quota, &tid), ASX_OK);
    run = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(rid, &run), ASX_OK);

    ASSERT_EQ(p.polls, 3u);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ((int)p.reason.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_TRUE(asx_handle_index(p.reason.origin_task) == asx_handle_index(tid));
    ASSERT_TRUE(asx_handle_index(p.reason.origin_region) == asx_handle_index(rid));
    ASSERT_TRUE(p.reason.timestamp < (asx_time)1000000000u);
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
    RUN_TEST(scheduler_trace_emits_round_markers);

    /* Dispatch order within a round: cancel lane, then budget priority */
    RUN_TEST(scheduler_polls_higher_budget_priority_first);
    RUN_TEST(scheduler_polls_three_priorities_highest_first_arena_order_within_one);
    RUN_TEST(scheduler_polls_the_cancel_lane_before_the_ready_lane);
    RUN_TEST(scheduler_orders_the_cancel_lane_by_cleanup_priority);
    RUN_TEST_IF(ASX_MAX_TASKS >= 19u, scheduler_cancel_streak_gives_ready_work_a_turn,
                "needs 19 tasks: 16 cancel polls in a row, the ready task, two more");
    RUN_TEST(scheduler_polls_a_task_spawned_in_a_round_in_that_round);
    RUN_TEST(scheduler_polls_a_task_in_a_region_opened_in_the_round_before_its_opener_resumes);
    RUN_TEST(scheduler_does_not_cancel_a_cleanup_task_at_its_budget_deadline_before_it_polls);
    RUN_TEST(scheduler_a_passed_deadline_strengthens_a_user_cancel_that_arrived_first);
    RUN_TEST(scheduler_budget_deadlines_fire_before_any_task_polls_in_the_round);
    RUN_TEST(scheduler_a_lone_parked_task_is_cancelled_at_its_budget_deadline);

    /* Budget enforcement before a poll */
    RUN_TEST(scheduler_stamps_an_exhausted_poll_quota_with_the_poll_quota_defaults);
    RUN_TEST(scheduler_poll_quota_stamp_is_reattributed_by_a_later_checkpoint);

    TEST_REPORT();
    return test_failures;
}
