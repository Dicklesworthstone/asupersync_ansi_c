/*
 * test_scheduler_wake.c — wake-driven scheduling, task timers, join
 * waiters, virtual time, subtree scheduling, and structured drain.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/time/sleep.h>
#include <string.h>

#define MS ((uint64_t)1000000u)

static asx_runtime g_rt;

/* Always run on the runtime's virtual clock (default stub hooks), even in
 * live POSIX builds where asx_runtime_init_default installs a real clock:
 * these scenarios sleep for virtual hours. */
static void setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    (void)st;
}

/* -------------------------------------------------------------------
 * Fixtures
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t polls;
    int done;
} park_state;

/* Parks every poll until `done` is set externally. */
static asx_status poll_park_until_done(void *ud, asx_task_id self) {
    park_state *s = (park_state *)ud;
    s->polls++;
    if (s->done) return ASX_OK;
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

typedef struct {
    asx_task_id target;
    uint32_t polls;
} waker_state;

/* Sets the parked task's flag and wakes it, then completes. */
static park_state *g_wake_target_state;
static asx_status poll_wake_other(void *ud, asx_task_id self) {
    waker_state *s = (waker_state *)ud;
    (void)self;
    s->polls++;
    g_wake_target_state->done = 1;
    return asx_task_wake(s->target);
}

typedef struct {
    asx_sleep_state sleep;
    uint32_t polls;
    uint32_t order_slot;
} sleeper_state;

static uint32_t g_finish_order[8];
static uint32_t g_finish_count;

static asx_status poll_sleeper(void *ud, asx_task_id self) {
    sleeper_state *s = (sleeper_state *)ud;
    asx_status st;
    s->polls++;
    st = asx_sleep_poll(&s->sleep, self);
    if (st == ASX_OK && g_finish_count < 8u) g_finish_order[g_finish_count++] = s->order_slot;
    return st;
}

typedef struct {
    asx_task_id target;
    asx_outcome outcome;
    asx_status join_status;
    uint32_t polls;
} joiner_state;

static asx_status poll_joiner(void *ud, asx_task_id self) {
    joiner_state *s = (joiner_state *)ud;
    s->polls++;
    s->join_status = asx_task_join_poll(self, s->target, &s->outcome);
    if (s->join_status == ASX_E_PENDING) return ASX_E_PENDING;
    return ASX_OK;
}

typedef struct {
    uint32_t remaining;
} countdown_state;

static asx_status poll_countdown(void *ud, asx_task_id self) {
    countdown_state *s = (countdown_state *)ud;
    (void)self;
    if (s->remaining == 0u) return ASX_OK;
    s->remaining--;
    return ASX_E_PENDING;
}

static asx_status poll_fail_io(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_DISCONNECTED;
}

static asx_status poll_ok(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Park / wake
 * ------------------------------------------------------------------- */

TEST(parked_task_is_not_repolled_until_woken) {
    asx_region_id r;
    asx_task_id parked;
    park_state ps;
    asx_budget budget;

    setup();
    memset(&ps, 0, sizeof(ps));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_park_until_done, &ps, &parked), ASX_OK);

    /* Nothing can wake it: the run reports WOULD_BLOCK after one poll
     * instead of burning the whole budget. */
    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(ps.polls, 1u);
    ASSERT_EQ(asx_budget_polls(&budget), 999u);

    /* An external wake makes it runnable again. */
    ps.done = 1;
    ASSERT_EQ(asx_task_wake(parked), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(ps.polls, 2u);
}

TEST(task_wakes_another_task) {
    asx_region_id r;
    asx_task_id parked;
    asx_task_id waker;
    park_state ps;
    waker_state ws;
    countdown_state cd;
    asx_task_id delay;
    asx_budget budget;

    setup();
    memset(&ps, 0, sizeof(ps));
    memset(&ws, 0, sizeof(ws));
    cd.remaining = 3u;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_park_until_done, &ps, &parked), ASX_OK);
    /* Countdown keeps the run busy for a few rounds before the waker runs. */
    ASSERT_EQ(asx_task_spawn(r, poll_countdown, &cd, &delay), ASX_OK);
    ws.target = parked;
    g_wake_target_state = &ps;
    ASSERT_EQ(asx_task_spawn(r, poll_wake_other, &ws, &waker), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(ps.polls, 2u);
}

TEST(park_outside_poll_is_rejected) {
    asx_region_id r;
    asx_task_id t;
    park_state ps;

    setup();
    memset(&ps, 0, sizeof(ps));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_park_until_done, &ps, &t), ASX_OK);
    ASSERT_EQ(asx_task_park(t), ASX_E_INVALID_STATE);
}

/* -------------------------------------------------------------------
 * Timers and virtual time
 * ------------------------------------------------------------------- */

TEST(sleep_parks_and_virtual_time_jumps_to_deadline) {
    asx_region_id r;
    asx_task_id t;
    sleeper_state *s = NULL;
    asx_budget budget;

    setup();
    g_finish_count = 0;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, poll_sleeper, (uint32_t)sizeof(sleeper_state), NULL, &t,
                                      (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_sleep_init(&s->sleep, 3600u * 1000u * MS), ASX_OK); /* one hour */

    /* An hour-long sleep finishes with exactly two polls and three poll
     * units of budget: idle time is free and virtual time jumps. */
    budget = asx_budget_from_polls(3);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(3600u * 1000u * MS));
}

TEST(sleepers_finish_in_deadline_order) {
    asx_region_id r;
    asx_task_id t;
    sleeper_state *s[4];
    static const uint64_t durations[4] = {40u * MS, 10u * MS, 30u * MS, 10u * MS};
    uint32_t i;
    asx_budget budget;

    setup();
    g_finish_count = 0;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    for (i = 0; i < 4u; i++) {
        ASSERT_EQ(asx_task_spawn_captured(r, poll_sleeper, (uint32_t)sizeof(sleeper_state), NULL,
                                          &t, (void **)&s[i]),
                  ASX_OK);
        ASSERT_EQ(asx_sleep_init(&s[i]->sleep, durations[i]), ASX_OK);
        s[i]->order_slot = i;
    }

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(g_finish_count, 4u);
    /* EDF with insertion-stable ties: 10ms (#1), 10ms (#3), 30ms, 40ms */
    ASSERT_EQ(g_finish_order[0], 1u);
    ASSERT_EQ(g_finish_order[1], 3u);
    ASSERT_EQ(g_finish_order[2], 2u);
    ASSERT_EQ(g_finish_order[3], 0u);
    for (i = 0; i < 4u; i++) ASSERT_EQ(s[i]->polls, 2u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(40u * MS));
}

typedef struct {
    asx_timeout_state timeout;
    park_state inner;
} timeout_fixture;

TEST(timeout_fires_for_parked_inner) {
    asx_region_id r;
    asx_task_id t;
    timeout_fixture *f = NULL;
    asx_outcome out;
    asx_status err = ASX_OK;
    asx_budget budget;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, asx_timeout_poll, (uint32_t)sizeof(timeout_fixture), NULL,
                                      &t, (void **)&f),
              ASX_OK);
    memset(&f->inner, 0, sizeof(f->inner));
    ASSERT_EQ(asx_timeout_init(&f->timeout, 25u * MS, poll_park_until_done, &f->inner), ASX_OK);

    /* Debug builds use fail-fast containment: the run reports the fault. */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_TIMED_OUT);
    ASSERT_EQ(asx_task_get_outcome(t, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_ERR);
    ASSERT_EQ(asx_task_get_error(t, &err), ASX_OK);
    ASSERT_EQ(err, ASX_E_TIMED_OUT);
    ASSERT_EQ(f->inner.polls, 1u); /* parked inner polled once, then timed out */
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(25u * MS));
}

typedef struct {
    asx_interval_state interval;
} interval_fixture;

TEST(interval_ticks_on_virtual_time) {
    asx_region_id r;
    asx_task_id t;
    interval_fixture *f = NULL;
    asx_budget budget;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, asx_interval_poll, (uint32_t)sizeof(interval_fixture),
                                      NULL, &t, (void **)&f),
              ASX_OK);
    ASSERT_EQ(asx_interval_init(&f->interval, 5u * MS, 4u), ASX_OK);

    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_interval_ticks(&f->interval), 4u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(20u * MS));
}

TEST(arm_timer_keeps_earliest_deadline) {
    asx_region_id r;
    asx_task_id t;
    park_state ps;
    asx_budget budget;

    setup();
    memset(&ps, 0, sizeof(ps));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_park_until_done, &ps, &t), ASX_OK);
    ASSERT_EQ(asx_task_arm_timer(t, 50u * MS), ASX_OK);
    ASSERT_EQ(asx_task_arm_timer(t, 20u * MS), ASX_OK);
    ASSERT_EQ(asx_task_arm_timer(t, 90u * MS), ASX_OK);

    /* First poll parks; the timer (20ms) wakes it once; nothing else can. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(ps.polls, 2u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(20u * MS));
}

/* -------------------------------------------------------------------
 * Join waiters
 * ------------------------------------------------------------------- */

TEST(join_poll_parks_until_target_completes) {
    asx_region_id r;
    asx_task_id target;
    asx_task_id joiner;
    sleeper_state *s = NULL;
    joiner_state js;
    asx_task_state st;
    asx_budget budget;

    setup();
    g_finish_count = 0;
    memset(&js, 0, sizeof(js));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, poll_sleeper, (uint32_t)sizeof(sleeper_state), NULL,
                                      &target, (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_sleep_init(&s->sleep, 7u * MS), ASX_OK);
    js.target = target;
    ASSERT_EQ(asx_task_spawn(r, poll_joiner, &js, &joiner), ASX_OK);

    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(js.join_status, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&js.outcome), ASX_OUTCOME_OK);
    ASSERT_EQ(js.polls, 2u); /* parked once, woken by completion */
    /* The join consumed the target: its handle is gone. */
    ASSERT_EQ(asx_task_get_state(target, &st), ASX_E_NOT_FOUND);
}

TEST(join_poll_reports_error_outcome) {
    asx_region_id r;
    asx_task_id target;
    asx_task_id joiner;
    joiner_state js;
    asx_status err = ASX_OK;
    asx_budget budget;

    setup();
    memset(&js, 0, sizeof(js));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_fail_io, NULL, &target), ASX_OK);
    ASSERT_EQ(asx_task_get_error(target, &err), ASX_E_TASK_NOT_COMPLETED);
    js.target = target;
    ASSERT_EQ(asx_task_spawn(r, poll_joiner, &js, &joiner), ASX_OK);

    /* Fail-fast containment surfaces the target's failure; resuming the
     * run lets the woken joiner observe it. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(js.join_status, ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&js.outcome), ASX_OUTCOME_ERR);
}

TEST(join_poll_rejects_self_join) {
    asx_region_id r;
    asx_task_id t;
    asx_outcome out;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_ok, NULL, &t), ASX_OK);
    ASSERT_EQ(asx_task_join_poll(t, t, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_error_reports_failure_status) {
    asx_region_id r;
    asx_task_id t;
    asx_status err = ASX_OK;
    asx_budget budget;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_fail_io, NULL, &t), ASX_OK);
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_task_get_error(t, &err), ASX_OK);
    ASSERT_EQ(err, ASX_E_DISCONNECTED);
}

/* -------------------------------------------------------------------
 * Cancellation and structured scheduling
 * ------------------------------------------------------------------- */

TEST(cancel_wakes_parked_task) {
    asx_region_id r;
    asx_task_id t;
    park_state ps;
    asx_outcome out;
    asx_budget budget;

    setup();
    memset(&ps, 0, sizeof(ps));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_park_until_done, &ps, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);

    /* The cancelled task keeps parking without ever reaching completion:
     * bounded cleanup (SHUTDOWN: 50 polls) force-completes it instead of
     * blocking forever. */
    ASSERT_EQ(asx_task_cancel(t, ASX_CANCEL_SHUTDOWN), ASX_OK);
    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(t, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_TRUE(ps.polls >= 2u);
}

TEST(scheduler_runs_whole_region_subtree) {
    asx_region_id root;
    asx_region_id child;
    asx_region_id grandchild;
    asx_task_id t1;
    asx_task_id t2;
    asx_task_id t3;
    asx_task_state st;
    asx_budget budget;

    setup();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    ASSERT_EQ(asx_region_open_child(child, &grandchild), ASX_OK);
    ASSERT_EQ(asx_task_spawn(root, poll_ok, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, poll_ok, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(grandchild, poll_ok, NULL, &t3), ASX_OK);

    /* Running the child drives the child and grandchild, not the root. */
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(child, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(t1, &st), ASX_OK);
    ASSERT_EQ(st, ASX_TASK_CREATED);
    ASSERT_EQ(asx_task_get_state(t2, &st), ASX_OK);
    ASSERT_EQ(st, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t3, &st), ASX_OK);
    ASSERT_EQ(st, ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_scheduler_run(root, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(t1, &st), ASX_OK);
    ASSERT_EQ(st, ASX_TASK_COMPLETED);
}

TEST(drain_cancels_sleepers_without_waiting_for_deadline) {
    asx_region_id root;
    asx_region_id child;
    asx_task_id t;
    sleeper_state *s = NULL;
    asx_outcome out;
    asx_budget budget;

    setup();
    g_finish_count = 0;
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(child, poll_sleeper, (uint32_t)sizeof(sleeper_state), NULL,
                                      &t, (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_sleep_init(&s->sleep, 3600u * 1000u * MS), ASX_OK);

    /* Park the sleeper first. */
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_scheduler_run(root, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_region_drain(root, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(t, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_region_is_quiescent(root), 1);
    /* Shutdown did not have to wait out the hour of virtual time. */
    ASSERT_TRUE(asx_runtime_virtual_now() < (asx_time)(3600u * 1000u * MS));
}

TEST(wake_driven_run_is_deterministic) {
    uint32_t run;
    uint32_t order[2][4];

    for (run = 0; run < 2u; run++) {
        asx_region_id r;
        asx_task_id t;
        sleeper_state *s[4];
        static const uint64_t durations[4] = {3u * MS, 1u * MS, 3u * MS, 2u * MS};
        uint32_t i;
        asx_budget budget;

        setup();
        g_finish_count = 0;
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        for (i = 0; i < 4u; i++) {
            ASSERT_EQ(asx_task_spawn_captured(r, poll_sleeper, (uint32_t)sizeof(sleeper_state),
                                              NULL, &t, (void **)&s[i]),
                      ASX_OK);
            ASSERT_EQ(asx_sleep_init(&s[i]->sleep, durations[i]), ASX_OK);
            s[i]->order_slot = i;
        }
        budget = asx_budget_from_polls(100);
        ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
        for (i = 0; i < 4u; i++) order[run][i] = g_finish_order[i];
    }
    ASSERT_EQ(memcmp(order[0], order[1], sizeof(order[0])), 0);
    ASSERT_EQ(order[0][0], 1u);
    ASSERT_EQ(order[0][1], 3u);
}

int main(void) {
    fprintf(stderr, "=== test_scheduler_wake ===\n");

    RUN_TEST(parked_task_is_not_repolled_until_woken);
    RUN_TEST(task_wakes_another_task);
    RUN_TEST(park_outside_poll_is_rejected);
    RUN_TEST(sleep_parks_and_virtual_time_jumps_to_deadline);
    RUN_TEST(sleepers_finish_in_deadline_order);
    RUN_TEST(timeout_fires_for_parked_inner);
    RUN_TEST(interval_ticks_on_virtual_time);
    RUN_TEST(arm_timer_keeps_earliest_deadline);
    RUN_TEST(join_poll_parks_until_target_completes);
    RUN_TEST(join_poll_reports_error_outcome);
    RUN_TEST(join_poll_rejects_self_join);
    RUN_TEST(get_error_reports_failure_status);
    RUN_TEST(cancel_wakes_parked_task);
    RUN_TEST(scheduler_runs_whole_region_subtree);
    RUN_TEST(drain_cancels_sleepers_without_waiting_for_deadline);
    RUN_TEST(wake_driven_run_is_deterministic);

    TEST_REPORT();
    return test_failures;
}
