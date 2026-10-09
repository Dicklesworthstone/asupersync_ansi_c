/*
 * test_task_group.c — task groups: join_all / race / first_ok / quorum over
 * spawned tasks, with losers cancelled and drained before the group
 * resolves, owner cancellation, deadlines, and loser obligations.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/task_group.h>
#include <string.h>

#define MS ((uint64_t)1000000u)
#define HOUR ((uint64_t)3600u * 1000u * MS)

static asx_runtime g_rt;
static uint32_t g_seq;

static void setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    (void)st;
    g_seq = 0;
}

/* -------------------------------------------------------------------
 * Fixtures
 * ------------------------------------------------------------------- */

typedef struct {
    asx_time finish_at;     /* virtual time to finish; 0 = park until cancelled */
    asx_status finish;      /* status returned at finish_at */
    uint32_t cleanup_polls; /* extra polls spent cleaning up once cancelled */
    uint32_t polls;
    int cancelled;
    asx_cancel_kind kind;
    uint32_t done_seq; /* global completion order */
    asx_region_id region;
    int hold_obligation; /* reserve an obligation on the first poll */
    asx_obligation_id ob;
} member_state;

static asx_status poll_member(void *ud, asx_task_id self) {
    member_state *m = (member_state *)ud;
    asx_checkpoint_result cp;
    asx_status st;

    m->polls++;
    if (m->hold_obligation && m->ob == ASX_INVALID_ID) {
        st = asx_obligation_reserve(m->region, &m->ob);
        if (st != ASX_OK) return st;
    }
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        if (!m->cancelled) {
            m->cancelled = 1;
            m->kind = cp.kind;
        }
        if (m->cleanup_polls > 0u) {
            m->cleanup_polls--;
            return ASX_E_PENDING;
        }
        m->done_seq = ++g_seq;
        return ASX_OK;
    }
    if (m->finish_at != 0u) {
        if (asx_task_wait_until(self, m->finish_at) == ASX_OK) {
            m->done_seq = ++g_seq;
            return m->finish;
        }
        return ASX_E_PENDING;
    }
    st = asx_task_park(self);
    if (st != ASX_OK) return st;
    return ASX_E_PENDING;
}

typedef struct {
    asx_task_group group;
    uint32_t polls;
    asx_status result;
    int done;
    uint32_t done_seq;
} owner_state;

/* Drives the group; records its result and completes OK so failing
 * groups do not trip fault containment. */
static asx_status poll_owner(void *ud, asx_task_id self) {
    owner_state *o = (owner_state *)ud;
    asx_status st;
    o->polls++;
    st = asx_task_group_poll(&o->group, self);
    if (st == ASX_E_PENDING) return st;
    o->result = st;
    o->done = 1;
    o->done_seq = ++g_seq;
    return ASX_OK;
}

static member_state g_m[ASX_TASK_GROUP_MAX];
static owner_state g_owner;

static void member_init(uint32_t i, uint64_t finish_ms, asx_status finish) {
    memset(&g_m[i], 0, sizeof(g_m[i]));
    g_m[i].finish_at = finish_ms == 0u ? 0u : asx_runtime_virtual_now() + finish_ms * MS;
    g_m[i].finish = finish;
    g_m[i].ob = ASX_INVALID_ID;
}

/* Spawn the owner first, then `n` members (from g_m) into the group. */
static asx_status spawn_group(asx_region_id r, asx_task_group_mode mode, uint32_t needed,
                              uint32_t n, asx_task_id *owner) {
    asx_status st;
    uint32_t i;
    memset(&g_owner, 0, sizeof(g_owner));
    st = asx_task_group_init(&g_owner.group, mode, needed);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(r, poll_owner, &g_owner, owner);
    if (st != ASX_OK) return st;
    for (i = 0; i < n; i++) {
        g_m[i].region = r;
        st = asx_task_group_spawn(&g_owner.group, r, poll_member, &g_m[i], NULL);
        if (st != ASX_OK) return st;
    }
    return ASX_OK;
}

/* Run until the owner resolves. A failing member surfaces from
 * asx_scheduler_run under FAIL_FAST containment; keep driving. */
static int run_until_owner_done(asx_region_id r) {
    int rounds = 0;
    while (!g_owner.done && rounds++ < 8) {
        asx_budget run = asx_budget_from_polls(200);
        asx_status st = asx_scheduler_run(r, &run);
        (void)st;
    }
    return g_owner.done;
}

static uint32_t max_member_seq(uint32_t n) {
    uint32_t i;
    uint32_t mx = 0;
    for (i = 0; i < n; i++) {
        if (g_m[i].done_seq > mx) mx = g_m[i].done_seq;
    }
    return mx;
}

/* -------------------------------------------------------------------
 * JOIN_ALL
 * ------------------------------------------------------------------- */

TEST(join_all_waits_for_every_member) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;
    asx_outcome out;

    setup();
    member_init(0, 3, ASX_OK);
    member_init(1, 1, ASX_OK);
    member_init(2, 2, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_JOIN_ALL, 0, 3, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_OK);
    ASSERT_EQ(asx_task_group_phase_of(&g_owner.group), ASX_TASK_GROUP_DONE);
    ASSERT_EQ(asx_task_group_ok_count(&g_owner.group), 3u);
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_OK);
    ASSERT_TRUE(g_owner.done_seq > max_member_seq(3));
    /* Wake-driven: the owner is polled once per member completion, not
     * once per scheduler round. */
    ASSERT_TRUE(g_owner.polls <= 4u);
}

TEST(join_all_reports_most_severe_failure) {
    asx_region_id r;
    asx_task_id owner;
    asx_outcome out;
    asx_status mst;

    setup();
    member_init(0, 1, ASX_OK);
    member_init(1, 2, ASX_E_DISCONNECTED);
    member_init(2, 3, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_JOIN_ALL, 0, 3, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_E_DISCONNECTED);
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_ERR);
    ASSERT_EQ(asx_task_group_member_result(&g_owner.group, 1, &out, &mst), ASX_OK);
    ASSERT_EQ(mst, ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_task_group_ok_count(&g_owner.group), 2u);
}

TEST(empty_join_all_resolves_immediately) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_JOIN_ALL, 0, 0, &owner), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_OK);
    ASSERT_EQ(g_owner.polls, 1u);
}

/* -------------------------------------------------------------------
 * RACE
 * ------------------------------------------------------------------- */

TEST(race_cancels_and_drains_losers_before_resolving) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;
    asx_outcome out;
    asx_status mst;

    setup();
    member_init(0, 60u * 60u * 1000u, ASX_OK); /* one hour */
    member_init(1, 2, ASX_OK);                 /* winner */
    member_init(2, 0, ASX_OK);                 /* parks until cancelled */
    g_m[0].cleanup_polls = 3;
    g_m[2].cleanup_polls = 2;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 3, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_OK);
    ASSERT_EQ(asx_task_group_winner(&g_owner.group), 1);
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_OK);

    /* Losers were cancelled as race losers and ran their cleanup to the
     * end before the owner saw the result. */
    ASSERT_TRUE(g_m[0].cancelled);
    ASSERT_TRUE(g_m[2].cancelled);
    ASSERT_EQ(g_m[0].kind, ASX_CANCEL_RACE_LOST);
    ASSERT_EQ(g_m[2].kind, ASX_CANCEL_RACE_LOST);
    ASSERT_EQ(g_m[0].cleanup_polls, 0u);
    ASSERT_EQ(g_m[2].cleanup_polls, 0u);
    ASSERT_TRUE(g_owner.done_seq > max_member_seq(3));
    ASSERT_EQ(asx_task_group_member_result(&g_owner.group, 0, &out, &mst), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(mst, ASX_E_CANCELLED);

    /* The hour-long loser was cancelled, not waited out. */
    ASSERT_TRUE(asx_runtime_virtual_now() < 1000u * MS);
}

TEST(race_winner_failure_is_group_result) {
    asx_region_id r;
    asx_task_id owner;
    asx_outcome out;

    setup();
    member_init(0, 1, ASX_E_DISCONNECTED);
    member_init(1, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 2, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_task_group_winner(&g_owner.group), 0);
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_ERR);
    ASSERT_TRUE(g_m[1].cancelled);
}

TEST(race_same_round_completion_picks_lowest_index) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;

    setup();
    member_init(0, 5, ASX_OK);
    member_init(1, 5, ASX_OK);
    member_init(2, 5, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 3, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_task_group_winner(&g_owner.group), 0);
}

TEST(race_loser_obligations_are_leaked_before_resolve) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;
    asx_obligation_info info;

    setup();
    member_init(0, 1, ASX_OK);
    member_init(1, 0, ASX_OK);
    g_m[1].hold_obligation = 1;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 2, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_TRUE(g_m[1].ob != ASX_INVALID_ID);
    /* The cancelled loser completed holding it: a leak, resolved before the
     * group resolves (Rust drops the loser's token, which posts a Leak). */
    ASSERT_EQ(asx_obligation_get_info(g_m[1].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
}

/* -------------------------------------------------------------------
 * FIRST_OK / QUORUM
 * ------------------------------------------------------------------- */

/* FIRST_OK: the owner first, then `n` attempts (from g_m), spawned by the
 * group one at a time. */
static asx_status first_ok_group(asx_region_id r, uint32_t n, asx_task_id *owner) {
    asx_status st;
    uint32_t i;
    memset(&g_owner, 0, sizeof(g_owner));
    st = asx_task_group_init(&g_owner.group, ASX_TASK_GROUP_FIRST_OK, 0);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(r, poll_owner, &g_owner, owner);
    if (st != ASX_OK) return st;
    for (i = 0; i < n; i++) {
        g_m[i].region = r;
        st = asx_task_group_add_attempt(&g_owner.group, r, poll_member, &g_m[i]);
        if (st != ASX_OK) return st;
    }
    return ASX_OK;
}

/* An error moves on to the next attempt; the first OK wins and later
 * attempts never start. */
TEST(first_ok_skips_failures) {
    asx_region_id r;
    asx_task_id owner;
    asx_status mst;
    asx_outcome out;

    setup();
    member_init(0, 1, ASX_E_DISCONNECTED);
    member_init(1, 2, ASX_OK);
    member_init(2, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(first_ok_group(r, 3, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_OK);
    ASSERT_EQ(asx_task_group_winner(&g_owner.group), 1);
    ASSERT_EQ(g_m[2].polls, 0u);
    ASSERT_EQ(asx_task_group_member_result(&g_owner.group, 0, &out, &mst), ASX_OK);
    ASSERT_EQ(mst, ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_task_group_member_result(&g_owner.group, 2, &out, &mst),
              ASX_E_TASK_NOT_COMPLETED);
}

/* Attempts do not overlap: the second starts only after the first ended,
 * so although its deadline (1 ms) is earlier, it completes second. */
TEST(first_ok_runs_attempts_one_at_a_time) {
    asx_region_id r;
    asx_task_id owner;

    setup();
    member_init(0, 5, ASX_E_DISCONNECTED);
    member_init(1, 1, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(first_ok_group(r, 2, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_OK);
    ASSERT_EQ(asx_task_group_winner(&g_owner.group), 1);
    ASSERT_TRUE(g_m[0].done_seq < g_m[1].done_seq);
}

/* The owner's cancel is passed to the running attempt (with the owner's
 * reason), which is still awaited; no further attempt starts. */
TEST(first_ok_owner_cancel_reaches_the_running_attempt) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;
    asx_status st;

    setup();
    member_init(0, 0, ASX_OK); /* parks until cancelled */
    member_init(1, 1, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(first_ok_group(r, 2, &owner), ASX_OK);
    run = asx_budget_from_polls(50);
    st = asx_scheduler_run(r, &run);
    ASSERT_TRUE(st != ASX_OK);
    ASSERT_TRUE(!g_owner.done);
    ASSERT_EQ(g_m[0].polls > 0u, 1);
    ASSERT_EQ(asx_task_cancel(owner, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_E_CANCELLED);
    ASSERT_TRUE(g_m[0].cancelled);
    ASSERT_EQ((int)g_m[0].kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(g_m[1].polls, 0u);
}

/* Attempts are the only FIRST_OK members; an empty FIRST_OK fails. */
TEST(first_ok_api) {
    asx_region_id r;
    asx_task_id owner;
    asx_task_id t;
    asx_task_group g;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_group_init(&g, ASX_TASK_GROUP_FIRST_OK, 0), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_member, &g_m[0], &t), ASX_OK);
    ASSERT_EQ(asx_task_group_add(&g, t), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_group_spawn(&g, r, poll_member, &g_m[1], NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_group_add_attempt(&g, r, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_group_init(&g, ASX_TASK_GROUP_RACE, 0), ASX_OK);
    ASSERT_EQ(asx_task_group_add_attempt(&g, r, poll_member, &g_m[1]), ASX_E_INVALID_STATE);

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(first_ok_group(r, 0, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_E_INVALID_ARGUMENT);
}

TEST(first_ok_fails_when_every_member_fails) {
    asx_region_id r;
    asx_task_id owner;
    asx_outcome out;

    setup();
    member_init(0, 1, ASX_E_DISCONNECTED);
    member_init(1, 2, ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(first_ok_group(r, 2, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_task_group_winner(&g_owner.group), -1);
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_ERR);
}

TEST(quorum_reached_drains_remaining_members) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;
    asx_outcome out;

    setup();
    member_init(0, 3, ASX_OK);
    member_init(1, 0, ASX_OK);
    member_init(2, 1, ASX_OK);
    member_init(3, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_QUORUM, 2, 4, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_OK);
    ASSERT_EQ(asx_task_group_ok_count(&g_owner.group), 2u);
    ASSERT_TRUE(g_m[1].cancelled);
    ASSERT_TRUE(g_m[3].cancelled);
    ASSERT_TRUE(!g_m[0].cancelled);
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_OK);
}

TEST(quorum_impossible_resolves_early) {
    asx_region_id r;
    asx_task_id owner;

    setup();
    member_init(0, 1, ASX_E_DISCONNECTED);
    member_init(1, 2, ASX_E_DISCONNECTED);
    member_init(2, 0, ASX_OK);
    member_init(3, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    /* 3-of-4 is impossible after two failures. */
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_QUORUM, 3, 4, &owner), ASX_OK);
    ASSERT_TRUE(run_until_owner_done(r));
    ASSERT_EQ(g_owner.result, ASX_E_DISCONNECTED);
    ASSERT_TRUE(g_m[2].cancelled);
    ASSERT_TRUE(g_m[3].cancelled);
}

TEST(quorum_rejects_invalid_threshold_after_draining) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;

    setup();
    member_init(0, 0, ASX_OK);
    member_init(1, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_QUORUM, 3, 2, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_E_INVALID_ARGUMENT);
    ASSERT_TRUE(g_m[0].cancelled);
    ASSERT_TRUE(g_m[1].cancelled);
}

/* -------------------------------------------------------------------
 * Early resolution: owner cancel, deadline, explicit cancel
 * ------------------------------------------------------------------- */

TEST(owner_cancel_drains_members_with_parent_kind) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;
    asx_outcome out;

    setup();
    member_init(0, 0, ASX_OK);
    member_init(1, 0, ASX_OK);
    g_m[1].cleanup_polls = 2;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 2, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_task_cancel(owner, ASX_CANCEL_USER), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_E_CANCELLED);
    ASSERT_EQ(g_m[0].kind, ASX_CANCEL_PARENT);
    ASSERT_EQ(g_m[1].kind, ASX_CANCEL_PARENT);
    ASSERT_TRUE(g_owner.done_seq > max_member_seq(2));
    out = asx_task_group_outcome(&g_owner.group);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_outcome(owner, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
}

TEST(deadline_times_out_and_drains_members) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;

    setup();
    member_init(0, 0, ASX_OK);
    member_init(1, 50, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 2, &owner), ASX_OK);
    ASSERT_EQ(asx_task_group_set_deadline(&g_owner.group, asx_runtime_virtual_now() + 5u * MS),
              ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_E_TIMED_OUT);
    ASSERT_EQ(g_m[0].kind, ASX_CANCEL_TIMEOUT);
    ASSERT_EQ(g_m[1].kind, ASX_CANCEL_TIMEOUT);
    ASSERT_TRUE(asx_runtime_virtual_now() < 50u * MS);
}

TEST(quorum_deadline_reports_threshold_timeout) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;

    setup();
    member_init(0, 1, ASX_OK);
    member_init(1, 0, ASX_OK);
    member_init(2, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_QUORUM, 2, 3, &owner), ASX_OK);
    ASSERT_EQ(asx_task_group_set_deadline(&g_owner.group, asx_runtime_virtual_now() + 5u * MS),
              ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(g_owner.result, ASX_E_THRESHOLD_TIMEOUT);
    ASSERT_EQ(asx_task_group_ok_count(&g_owner.group), 1u);
}

TEST(explicit_group_cancel_drains_and_resolves_cancelled) {
    asx_region_id r;
    asx_task_id owner;
    asx_budget run;

    setup();
    member_init(0, 0, ASX_OK);
    member_init(1, 0, ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_JOIN_ALL, 0, 2, &owner), ASX_OK);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_task_group_cancel(&g_owner.group, ASX_CANCEL_SHUTDOWN), ASX_OK);
    ASSERT_EQ(asx_task_group_phase_of(&g_owner.group), ASX_TASK_GROUP_DRAINING);
    run = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(g_owner.done);
    ASSERT_EQ(g_owner.result, ASX_E_CANCELLED);
    ASSERT_EQ(g_m[0].kind, ASX_CANCEL_SHUTDOWN);
}

/* -------------------------------------------------------------------
 * API validation
 * ------------------------------------------------------------------- */

TEST(group_api_validation) {
    asx_task_group g;
    asx_region_id r;
    asx_task_id t;
    asx_task_id d;
    asx_outcome out;
    asx_status mst;
    uint32_t i;

    setup();
    ASSERT_EQ(asx_task_group_init(NULL, ASX_TASK_GROUP_RACE, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_group_init(&g, (asx_task_group_mode)9, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_group_init(&g, ASX_TASK_GROUP_RACE, 0), ASX_OK);
    ASSERT_EQ(asx_task_group_winner(&g), -1);
    ASSERT_EQ(asx_task_group_phase_of(&g), ASX_TASK_GROUP_COLLECTING);

    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_member, &g_m[0], &t), ASX_OK);
    ASSERT_EQ(asx_task_group_add(&g, t), ASX_OK);
    ASSERT_EQ(asx_task_group_add(&g, t), ASX_E_ALREADY_EXISTS);
    ASSERT_EQ(asx_task_group_add(&g, ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_spawn(r, poll_member, &g_m[1], &d), ASX_OK);
    ASSERT_EQ(asx_task_detach(d), ASX_OK);
    ASSERT_EQ(asx_task_group_add(&g, d), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_group_member_result(&g, 0, &out, &mst), ASX_E_TASK_NOT_COMPLETED);
    ASSERT_EQ(asx_task_group_member_result(&g, 5, &out, &mst), ASX_E_INVALID_ARGUMENT);

    for (i = 1; i < ASX_TASK_GROUP_MAX; i++) {
        ASSERT_EQ(asx_task_group_spawn(&g, r, poll_member, &g_m[i], NULL), ASX_OK);
    }
    ASSERT_EQ(asx_task_group_spawn(&g, r, poll_member, &g_m[0], NULL), ASX_E_RESOURCE_EXHAUSTED);

    ASSERT_EQ(asx_task_group_cancel(&g, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_task_group_add(&g, t), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_group_set_deadline(&g, 1u), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_task_group_poll(NULL, t), ASX_E_INVALID_ARGUMENT);
}

TEST(race_is_deterministic_across_runs) {
    int32_t winners[2];
    uint32_t seqs[2][3];
    uint32_t run_idx;

    for (run_idx = 0; run_idx < 2u; run_idx++) {
        asx_region_id r;
        asx_task_id owner;
        asx_budget run;
        uint32_t i;

        setup();
        member_init(0, 7, ASX_OK);
        member_init(1, 4, ASX_OK);
        member_init(2, 4, ASX_OK);
        g_m[0].cleanup_polls = 1;
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        ASSERT_EQ(spawn_group(r, ASX_TASK_GROUP_RACE, 0, 3, &owner), ASX_OK);
        run = asx_budget_from_polls(200);
        ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
        winners[run_idx] = asx_task_group_winner(&g_owner.group);
        for (i = 0; i < 3u; i++) seqs[run_idx][i] = g_m[i].done_seq;
    }
    ASSERT_EQ(winners[0], winners[1]);
    ASSERT_EQ(winners[0], 1);
    ASSERT_EQ(memcmp(seqs[0], seqs[1], sizeof(seqs[0])), 0);
}

int main(void) {
    fprintf(stderr, "=== test_task_group ===\n");

    RUN_TEST(join_all_waits_for_every_member);
    RUN_TEST(join_all_reports_most_severe_failure);
    RUN_TEST(empty_join_all_resolves_immediately);
    RUN_TEST(race_cancels_and_drains_losers_before_resolving);
    RUN_TEST(race_winner_failure_is_group_result);
    RUN_TEST(race_same_round_completion_picks_lowest_index);
    RUN_TEST(race_loser_obligations_are_leaked_before_resolve);
    RUN_TEST(first_ok_skips_failures);
    RUN_TEST(first_ok_runs_attempts_one_at_a_time);
    RUN_TEST(first_ok_owner_cancel_reaches_the_running_attempt);
    RUN_TEST(first_ok_api);
    RUN_TEST(first_ok_fails_when_every_member_fails);
    RUN_TEST(quorum_reached_drains_remaining_members);
    RUN_TEST(quorum_impossible_resolves_early);
    RUN_TEST(quorum_rejects_invalid_threshold_after_draining);
    RUN_TEST(owner_cancel_drains_members_with_parent_kind);
    RUN_TEST(deadline_times_out_and_drains_members);
    RUN_TEST(quorum_deadline_reports_threshold_timeout);
    RUN_TEST(explicit_group_cancel_drains_and_resolves_cancelled);
    RUN_TEST(group_api_validation);
    RUN_TEST(race_is_deterministic_across_runs);

    TEST_REPORT();
    return test_failures;
}
