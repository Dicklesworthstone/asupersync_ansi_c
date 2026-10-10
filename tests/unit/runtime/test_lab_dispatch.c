/*
 * test_lab_dispatch.c — the Rust LabRuntime dispatch model
 * (asx_scheduler_use_lab_dispatch, bd-9kll.4.2).
 *
 * Expected values come from asupersync 5e60b1c4c: xorshift64 outputs for a
 * seed (src/util/det_rng.rs) and the pick of entry r % n among the
 * highest-priority entries in generation order (scheduler/priority.rs).
 * The Rust-vs-C fixtures (make conformance, make fuzz-differential) check
 * the same model end to end against captured Rust runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

static asx_runtime g_rt;

/* A runtime on the virtual clock (default stub hooks). */
static asx_status setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    return st;
}

/* Poll order record. */
static char g_order[32];
static uint32_t g_order_n = 0;

typedef struct {
    char name;
    uint32_t yields; /* polls that yield before completing */
    int checkpoint;  /* complete when a checkpoint reports a cancel */
} probe;

static asx_status poll_probe(void *ud, asx_task_id self) {
    probe *p = (probe *)ud;
    asx_checkpoint_result cr;
    if (g_order_n + 1u < sizeof(g_order)) g_order[g_order_n++] = p->name;
    if (p->checkpoint && asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    if (p->yields > 0u) {
        p->yields--;
        return ASX_E_PENDING; /* not parked: a yield */
    }
    return ASX_OK;
}

static void order_reset(void) {
    memset(g_order, 0, sizeof(g_order));
    g_order_n = 0;
}

TEST(rng_replicates_rust_xorshift64) {
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    /* DetRng::new(42).next_u64() x5 */
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(45454805674));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(11532217803599905471));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(10021416941527320954));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(2899061411254629736));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(5661411637479084162));
}

TEST(rng_remaps_degenerate_seeds) {
    ASSERT_EQ(setup(), ASX_OK);
    /* Seed 0 starts from state 1. */
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(0u), ASX_OK);
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(1082269761));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(1152992998833853505));
    /* All ones is offset by the golden-ratio constant. */
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(UINT64_C(0xFFFFFFFFFFFFFFFF)), ASX_OK);
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(15860402103189073388));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(8426428137925997623));
}

/* Seed 42, three host-spawned tasks at priority 0 (generations 0, 1, 2).
 * Step 1: r1 % 3 = 1 picks B, which yields (generation 3). Step 2:
 * [A, C, B], r2 % 3 = 1 picks C. Step 3: [A, B], r3 % 2 = 0 picks A.
 * Step 4: B. */
TEST(ties_are_broken_by_the_step_value_in_generation_order) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    probe a = {'A', 0u, 0};
    probe b = {'B', 1u, 0};
    probe c = {'C', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &c, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
}

/* First `n` Cx::random_usize(bound) draws of task `t`'s entropy stream. */
static int entropy_draws(asx_task_id t, uint32_t bound, uint32_t n, uint32_t *out) {
    asx_task_slot *slot;
    uint32_t i;
    if (asx_task_slot_lookup(t, &slot) != ASX_OK) return 0;
    for (i = 0; i < n; i++) {
        if (!asx_lab_entropy_index_internal(slot, bound, &out[i])) return 0;
    }
    return 1;
}

/* Host-spawned tasks fork the runtime's DetEntropy (seeded with the lab
 * seed) in creation order: task k (arena index k, fork counter k) draws
 * from xorshift64 seeded with mix_seed(seed + 0x9e3779b97f4a7c15 + k + k)
 * (util/entropy.rs:66-150). Expected values computed from Rust's
 * formulas; seed 10, task 0, bound 2 starts with 1, the pick the
 * Rust-captured fixture task-groups-race-same-round-tie-001 shows. */
TEST(host_tasks_draw_from_rust_detentropy_streams) {
    static const uint32_t want10[3][3] = {{1u, 1u, 1u}, {1u, 0u, 1u}, {0u, 0u, 1u}};
    static const uint32_t want42[3][2] = {{1u, 2u}, {2u, 0u}, {2u, 1u}};
    asx_region_id r;
    asx_task_id t[3];
    probe p = {'P', 0u, 0};
    uint32_t got[3];
    uint32_t i;
    uint32_t k;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(10u), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(asx_task_spawn(r, poll_probe, &p, &t[i]), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_TRUE(entropy_draws(t[i], 2u, 3u, got));
        for (k = 0; k < 3u; k++) ASSERT_EQ(got[k], want10[i][k]);
    }

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(asx_task_spawn(r, poll_probe, &p, &t[i]), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_TRUE(entropy_draws(t[i], 3u, 2u, got));
        for (k = 0; k < 2u; k++) ASSERT_EQ(got[k], want42[i][k]);
    }
}

/* Rust's task arena frees a completed task's index (state.rs:8497-8503):
 * the next task reuses it with the generation bumped, and its entropy is
 * keyed by (1 << 32) | 1 at the runtime's third fork. Without lab
 * dispatch there is no stream and the caller decides. */
TEST(completed_tasks_free_their_arena_index_for_the_next_task) {
    asx_region_id r;
    asx_task_id a;
    asx_task_id b;
    asx_task_id c;
    asx_task_slot *slot;
    asx_budget budget;
    probe pa = {'A', 30u, 0};
    probe pb = {'B', 0u, 0};
    probe pc = {'C', 0u, 0};
    uint32_t got[3];
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pa, &a), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pb, &b), ASX_OK);
    budget = asx_budget_from_polls(10);
    /* B completes within these polls; A keeps yielding. */
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_task_slot_lookup(b, &slot), ASX_OK);
    ASSERT_EQ(slot->state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pc, &c), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(c, &slot), ASX_OK);
    ASSERT_EQ(slot->lab_rust_index, 1u);
    ASSERT_EQ(slot->lab_rust_gen, 1u);
    ASSERT_TRUE(entropy_draws(c, 5u, 3u, got));
    ASSERT_EQ(got[0], 2u);
    ASSERT_EQ(got[1], 4u);
    ASSERT_EQ(got[2], 4u);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);

    ASSERT_EQ(setup(), ASX_OK); /* no lab dispatch */
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pb, &b), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(b, &slot), ASX_OK);
    i = 7u;
    ASSERT_FALSE(asx_lab_entropy_index_internal(slot, 2u, &i));
    ASSERT_EQ(i, 7u);
}

/* A cancelled task goes to the cancel lane, which is served before the
 * ready lane whatever the step value. */
TEST(cancel_lane_is_served_first) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget budget;
    probe a = {'A', 0u, 1};
    probe b = {'B', 0u, 1};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tb, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BA"), 0);
}

/* run_with_auto_advance: with nothing scheduled the clock moves to the next
 * timer, which wakes the sleeper. */
typedef struct {
    asx_time woke_at;
    int armed;
} sleeper;

static asx_status poll_sleeper(void *ud, asx_task_id self) {
    sleeper *s = (sleeper *)ud;
    asx_status st = asx_task_wait_until(self, 100u);
    s->armed = 1;
    if (st == ASX_OK) {
        s->woke_at = asx_runtime_virtual_now();
        return ASX_OK;
    }
    return st;
}

TEST(auto_advance_moves_the_clock_to_the_next_timer) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    sleeper s;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_sleeper, &s, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    /* run_until_idle stops with the sleeper parked and the clock at 0. */
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &budget), ASX_E_PENDING);
    ASSERT_TRUE(s.armed);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)0);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(s.woke_at, (asx_time)100);
}

/* Spends its one-poll quota yielding, then (pre-poll quota cancel on poll
 * 2) either checkpoints or just completes. */
typedef struct {
    uint32_t polls;
    int checkpoint;
} quota_probe;

static asx_status poll_quota_probe(void *ud, asx_task_id self) {
    quota_probe *q = (quota_probe *)ud;
    asx_checkpoint_result cr;
    q->polls++;
    if (q->polls == 1u) return ASX_E_PENDING;
    if (q->checkpoint && asx_checkpoint(self, &cr) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

static asx_status quota_reason(int checkpoint, asx_region_id *out_root, asx_cancel_reason *out) {
    asx_task_id t = ASX_INVALID_ID;
    asx_budget tb;
    asx_budget run;
    quota_probe q;
    asx_outcome o;
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    if (st == ASX_OK) st = asx_region_open(out_root);
    memset(&q, 0, sizeof(q));
    q.checkpoint = checkpoint;
    tb = asx_budget_from_polls(1);
    if (st == ASX_OK) st = asx_task_spawn_with_budget(*out_root, poll_quota_probe, &q, &tb, &t);
    run = asx_budget_from_polls(20);
    if (st == ASX_OK) st = asx_scheduler_run(*out_root, &run);
    if (st == ASX_OK) st = asx_task_get_outcome(t, &o);
    if (st == ASX_OK) st = asx_task_get_cancel_reason(t, out);
    return st;
}

/* Rust's lab stamps the pre-poll quota cancel with CancelReason::poll_quota()'s
 * testing defaults (lab/runtime.rs:4667): region at arena index 0, no task,
 * 1 s. A checkpoint afterwards re-attributes it (earlier timestamp wins). */
TEST(pre_poll_quota_cancel_has_rust_lab_attribution) {
    asx_region_id root;
    asx_cancel_reason r;
    ASSERT_EQ(quota_reason(0, &root, &r), ASX_OK);
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(r.origin_region, root);
    ASSERT_EQ(r.origin_task, ASX_INVALID_ID);
    ASSERT_EQ(r.timestamp, (asx_time)1000000000u);
    ASSERT_EQ(quota_reason(1, &root, &r), ASX_OK);
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_TRUE(r.origin_task != ASX_INVALID_ID);
    ASSERT_EQ(r.timestamp, (asx_time)0);
}

/* Dispatch records (asx_scheduler_record_dispatches): Rust's
 * ForcedDispatch fields, steps counted from 1 across runs. Two tasks that
 * yield once each: four ready-lane dispatches at time 0. A buffer too
 * small keeps the first records and still counts every dispatch. */
TEST(dispatch_records_carry_task_lane_step_and_time) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget run;
    probe a = {'a', 1u, 0};
    probe b = {'b', 1u, 0};
    asx_dispatch_record rec[3];
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    memset(rec, 0, sizeof(rec));
    asx_scheduler_record_dispatches(rec, 3u);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    run = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 4u);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(rec[i].step, (uint64_t)(i + 1u));
        ASSERT_EQ(rec[i].at, (asx_time)0);
        ASSERT_EQ(rec[i].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
        ASSERT_TRUE(asx_handle_index(rec[i].task) == asx_handle_index(ta) ||
                    asx_handle_index(rec[i].task) == asx_handle_index(tb));
    }
    /* A new lab stops recording. */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 0u);
}

/* A checkpoint that raises a budget cancel leaves the task's cancel waker
 * due; it fires with the acknowledgement, after the poll, even when that
 * poll completes the task: Rust then schedules the retired task id and the
 * next step dispatches it, polling nothing (record/task.rs:1227-1233,
 * lab/runtime.rs:5011, 4772-4790; fixture budget-poll-quota-exhaustion-001).
 * Without the checkpoint nothing is due. */
static asx_status run_quota_probe(int checkpoint, quota_probe *q, asx_dispatch_record *rec,
                                  uint32_t cap, asx_task_id *out_t) {
    asx_region_id root = ASX_INVALID_ID;
    asx_budget tb;
    asx_budget run;
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    asx_scheduler_record_dispatches(rec, cap);
    if (st == ASX_OK) st = asx_region_open(&root);
    memset(q, 0, sizeof(*q));
    q->checkpoint = checkpoint;
    tb = asx_budget_from_polls(1);
    if (st == ASX_OK) st = asx_task_spawn_with_budget(root, poll_quota_probe, q, &tb, out_t);
    run = asx_budget_from_polls(20);
    if (st == ASX_OK) st = asx_scheduler_run(root, &run);
    return st;
}

TEST(budget_cancel_at_a_completing_checkpoint_dispatches_the_retired_task) {
    asx_cancel_reason reason;
    asx_dispatch_record rec[8];
    asx_task_id t;
    quota_probe q;

    memset(rec, 0, sizeof(rec));
    ASSERT_EQ(run_quota_probe(1, &q, rec, 8u, &t), ASX_OK);
    ASSERT_EQ(q.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 3u);
    ASSERT_EQ(rec[0].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
    ASSERT_EQ(rec[2].lane, (uint8_t)ASX_DISPATCH_LANE_CANCEL);
    ASSERT_EQ(rec[2].step, (uint64_t)3);
    ASSERT_TRUE(asx_handle_index(rec[2].task) == asx_handle_index(t));
    ASSERT_EQ(asx_task_get_cancel_reason(t, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_POLL_QUOTA);
    /* No checkpoint, no acknowledgement: two dispatches. */
    ASSERT_EQ(run_quota_probe(0, &q, rec, 8u, &t), ASX_OK);
    ASSERT_EQ(q.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 2u);
}

/* The budget-deadline timer (Rust Cx::arm_budget_deadline, armed at
 * creation): at the deadline it cancels with DEADLINE stamped with the
 * deadline and wakes the task on the cancel lane, though the task waits
 * for a later time (fixture budget-deadline-sleep-checkpoint-001). It is a
 * pending timer: auto-advance moves the clock to it for a task parked with
 * nothing else to wake it. */
typedef struct {
    uint32_t polls;
    asx_time wait_until; /* 0: park */
} deadline_probe;

static asx_status poll_deadline_probe(void *ud, asx_task_id self) {
    deadline_probe *d = (deadline_probe *)ud;
    asx_checkpoint_result cr;
    asx_status st;
    d->polls++;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    if (d->wait_until != 0u) return asx_task_wait_until(self, d->wait_until);
    st = asx_task_park(self);
    return st == ASX_OK ? ASX_E_PENDING : st;
}

static asx_status run_deadline_probe(deadline_probe *d, asx_dispatch_record *rec, uint32_t cap,
                                     asx_task_id *out_t) {
    asx_region_id r = ASX_INVALID_ID;
    asx_budget b = asx_budget_infinite();
    asx_budget run;
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(9u);
    asx_scheduler_record_dispatches(rec, cap);
    if (st == ASX_OK) st = asx_region_open(&r);
    b.deadline = 100u;
    if (st == ASX_OK) st = asx_task_spawn_with_budget(r, poll_deadline_probe, d, &b, out_t);
    run = asx_budget_from_polls(50);
    if (st == ASX_OK) st = asx_scheduler_run(r, &run);
    return st;
}

TEST(budget_deadline_timer_cancels_on_the_cancel_lane) {
    deadline_probe d;
    asx_dispatch_record rec[4];
    asx_task_id t;
    asx_cancel_reason reason;

    memset(&d, 0, sizeof(d));
    d.wait_until = 200u;
    memset(rec, 0, sizeof(rec));
    ASSERT_EQ(run_deadline_probe(&d, rec, 4u, &t), ASX_OK);
    ASSERT_EQ(d.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 2u);
    ASSERT_EQ(rec[1].at, (asx_time)100);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_CANCEL);
    ASSERT_EQ(asx_task_get_cancel_reason(t, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_DEADLINE);
    ASSERT_EQ(reason.timestamp, (asx_time)100);

    /* Parked with no timer of its own: the clock moves to the deadline. */
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(run_deadline_probe(&d, rec, 4u, &t), ASX_OK);
    ASSERT_EQ(d.polls, 2u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)100);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_CANCEL);
}

/* Child-region commands (Rust RegionCommand, LR:4135-4217). The opener
 * opens a child region of `parent`, optionally spawns a kid into it that
 * yields until a checkpoint reports its cancel, then closes the region. */
typedef struct {
    asx_region_id parent;
    int spawn_kid;
    uint32_t polls;
    uint32_t phase;
    asx_status first; /* the first open call */
    asx_region_id child;
    asx_task_id kid;
} opener;

static asx_status poll_kid(void *ud, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)ud;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    return ASX_E_PENDING; /* a yield */
}

static asx_status poll_opener(void *ud, asx_task_id self) {
    opener *o = (opener *)ud;
    asx_status st;
    o->polls++;
    if (o->phase == 0u) {
        st = asx_region_open_child_poll(self, o->parent, NULL, &o->child);
        if (o->polls == 1u) o->first = st;
        if (st != ASX_OK) return st;
        if (o->spawn_kid && asx_task_spawn(o->child, poll_kid, NULL, &o->kid) != ASX_OK) {
            return ASX_E_INVALID_STATE;
        }
        o->phase = 1u;
    }
    return asx_region_close_poll(self, o->child);
}

static asx_status run_opener(int lab, int spawn_kid, opener *o) {
    asx_task_id t;
    asx_budget budget;
    asx_status st = setup();
    if (st == ASX_OK && lab) st = asx_scheduler_use_lab_dispatch(42u);
    memset(o, 0, sizeof(*o));
    o->spawn_kid = spawn_kid;
    o->child = ASX_INVALID_ID;
    if (st == ASX_OK) st = asx_region_open(&o->parent);
    if (st == ASX_OK) st = asx_task_spawn(o->parent, poll_opener, o, &t);
    budget = asx_budget_from_polls(100);
    if (st == ASX_OK) st = asx_scheduler_run(o->parent, &budget);
    return st;
}

/* The open is applied at the start of the next step: the first call parks
 * the opener; it is woken once with the region. The close is a command
 * too; the opener stays parked until the CLOSED transition wakes it, so it
 * is polled exactly three times. The kid is cancelled with the reason
 * Rust's Close carries. */
TEST(child_region_open_and_close_are_next_step_commands) {
    opener o;
    asx_region_state rs;
    asx_outcome out;
    asx_cancel_reason reason;

    ASSERT_EQ(run_opener(1, 1, &o), ASX_OK);
    ASSERT_EQ(o.first, ASX_E_PENDING);
    ASSERT_EQ(o.polls, 3u);
    ASSERT_EQ(asx_region_get_state(o.child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_get_outcome(o.kid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(o.kid, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_USER);
    ASSERT_EQ(strcmp(reason.message, "owned child region body finished"), 0);
    ASSERT_EQ(reason.origin_task, ASX_INVALID_ID);
    ASSERT_EQ(reason.timestamp, (asx_time)1000000000u);
}

/* Closing an empty child region: the Close command finalizes it at once
 * when applied, and that wakes the parked closer. */
TEST(closing_an_empty_child_region_wakes_the_closer) {
    opener o;
    asx_region_state rs;
    ASSERT_EQ(run_opener(1, 0, &o), ASX_OK);
    ASSERT_EQ(o.polls, 3u);
    ASSERT_EQ(asx_region_get_state(o.child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
}

/* Without lab dispatch the commands apply at once: the open completes in
 * the first call, the close cancels the kid at once and the closer waits
 * parked for CLOSED only. */
TEST(child_region_commands_apply_at_once_without_lab_dispatch) {
    opener o;
    asx_region_state rs;
    asx_outcome out;
    ASSERT_EQ(run_opener(0, 1, &o), ASX_OK);
    ASSERT_EQ(o.first, ASX_OK);
    ASSERT_EQ(o.polls, 2u);
    ASSERT_EQ(asx_region_get_state(o.child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_get_outcome(o.kid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

/* A region cancel request from a task (Rust ChildRegion::cancel) leaves
 * the region open until the next step applies it. */
typedef struct {
    asx_region_id target;
    uint32_t polls;
    asx_status request;
    asx_region_state at_request;
    asx_region_state next_poll;
} canceller;

static asx_status poll_canceller(void *ud, asx_task_id self) {
    canceller *c = (canceller *)ud;
    (void)self;
    c->polls++;
    if (c->polls == 1u) {
        asx_cancel_reason reason;
        memset(&reason, 0, sizeof(reason));
        reason.kind = ASX_CANCEL_SHUTDOWN;
        reason.origin_region = c->target;
        reason.origin_task = ASX_INVALID_ID;
        c->request = asx_region_cancel_request(c->target, &reason);
        if (asx_region_get_state(c->target, &c->at_request) != ASX_OK) return ASX_E_INVALID_STATE;
        return ASX_E_PENDING; /* a yield */
    }
    if (asx_region_get_state(c->target, &c->next_poll) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

TEST(region_cancel_request_applies_at_the_next_step) {
    asx_region_id root;
    asx_task_id t;
    asx_budget budget;
    canceller c;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    memset(&c, 0, sizeof(c));
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &c.target), ASX_OK);
    ASSERT_EQ(asx_task_spawn(root, poll_canceller, &c, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(root, &budget), ASX_OK);
    ASSERT_EQ(c.request, ASX_OK);
    ASSERT_EQ((int)c.at_request, (int)ASX_REGION_OPEN);
    ASSERT_EQ((int)c.next_poll, (int)ASX_REGION_CLOSED);
}

/* A full command queue refuses the request and changes nothing; the
 * queued commands still apply, at most eight per step. */
TEST(region_command_queue_full_is_failure_atomic) {
    asx_region_id root;
    asx_region_id child;
    asx_region_state rs;
    asx_budget budget;
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(5u), ASX_OK);
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    for (i = 0; i < (uint32_t)ASX_MAX_TASKS + (uint32_t)ASX_MAX_REGIONS; i++) {
        ASSERT_EQ(asx_region_close_request(child), ASX_OK);
    }
    ASSERT_EQ(asx_region_close_request(child), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_region_get_state(child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_OPEN);
    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run_until_idle(root, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_state(child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_lab_region_commands_pending(), 0);
}

/* Join-handle aborts (Rust JoinHandle::abort_with_reason): applied at the
 * next step. The aborter aborts `target` (or a child it spawns first) on
 * its first poll, records the target's state right after, and yields. */
typedef struct {
    asx_region_id region;
    asx_task_id target;
    int spawn_child;
    int twice;
    uint32_t polls;
    asx_status request;
    asx_task_state at_request;
} aborter;

static asx_status poll_aborter(void *ud, asx_task_id self) {
    aborter *a = (aborter *)ud;
    asx_cancel_reason reason;
    a->polls++;
    if (a->polls > 1u) return ASX_OK;
    if (a->spawn_child && asx_task_spawn(a->region, poll_kid, NULL, &a->target) != ASX_OK) {
        return ASX_E_INVALID_STATE;
    }
    memset(&reason, 0, sizeof(reason));
    reason.kind = ASX_CANCEL_USER;
    reason.origin_region = a->region;
    reason.origin_task = self;
    a->request = asx_task_abort_request(a->target, &reason);
    if (a->twice && a->request == ASX_OK) {
        reason.kind = ASX_CANCEL_SHUTDOWN;
        a->request = asx_task_abort_request(a->target, &reason);
    }
    if (asx_task_get_state(a->target, &a->at_request) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING; /* a yield */
}

static asx_status run_aborter(int lab, int spawn_child, int twice, aborter *a) {
    asx_task_id t;
    asx_budget budget;
    asx_status st = setup();
    if (st == ASX_OK && lab) st = asx_scheduler_use_lab_dispatch(9u);
    memset(a, 0, sizeof(*a));
    a->spawn_child = spawn_child;
    a->twice = twice;
    if (st == ASX_OK) st = asx_region_open(&a->region);
    if (st == ASX_OK && !spawn_child) st = asx_task_spawn(a->region, poll_kid, NULL, &a->target);
    if (st == ASX_OK) st = asx_task_spawn(a->region, poll_aborter, a, &t);
    budget = asx_budget_from_polls(100);
    if (st == ASX_OK) st = asx_scheduler_run(a->region, &budget);
    return st;
}

static int cancelled_with(asx_task_id t, asx_cancel_kind kind) {
    asx_outcome out;
    asx_cancel_reason reason;
    if (asx_task_get_outcome(t, &out) != ASX_OK) return 0;
    if (out.severity != ASX_OUTCOME_CANCELLED) return 0;
    if (asx_task_get_cancel_reason(t, &reason) != ASX_OK) return 0;
    return reason.kind == kind;
}

/* The abort leaves the target's record alone until the next step applies
 * it; the target then ends cancelled with the abort's reason. */
TEST(handle_abort_applies_at_the_next_step) {
    aborter a;
    ASSERT_EQ(run_aborter(1, 0, 0, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_TRUE(a.at_request != ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_USER));
}

/* A child aborted in the poll that spawned it takes the abort as it is
 * admitted: its first poll already sees the cancel. */
TEST(handle_abort_of_an_unadmitted_child_applies_at_admission) {
    aborter a;
    ASSERT_EQ(run_aborter(1, 1, 0, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_USER));
}

/* Two aborts of one task in a step coalesce; the stronger reason wins. */
TEST(handle_aborts_in_one_step_coalesce_to_the_strongest) {
    aborter a;
    ASSERT_EQ(run_aborter(1, 0, 1, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_TRUE(a.at_request != ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_SHUTDOWN));
}

/* Without lab dispatch the abort applies at once. */
TEST(handle_abort_applies_at_once_without_lab_dispatch) {
    aborter a;
    ASSERT_EQ(run_aborter(0, 0, 0, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_EQ((int)a.at_request, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_USER));
}

/* A spawn from a poll into a closed region: refused at once, but Rust's
 * spawn mailbox refuses it at the next step's admission, and a join of it
 * waits for that (fixture spawn-into-cancelled-region-step-001). */
typedef struct {
    asx_region_id closed;
    uint32_t polls;
    uint32_t ticket;
    asx_status spawn;
} refuser;

static asx_status poll_noop(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static asx_status poll_refuser(void *ud, asx_task_id self) {
    refuser *f = (refuser *)ud;
    f->polls++;
    if (f->polls == 1u) {
        asx_task_id kid = ASX_INVALID_ID;
        f->spawn = asx_task_spawn(f->closed, poll_noop, NULL, &kid);
        f->ticket = asx_scheduler_last_spawn_refusal();
    }
    return asx_task_await_refusal(self, f->ticket);
}

TEST(spawn_refusal_reaches_a_join_at_the_next_step) {
    asx_region_id r;
    asx_task_id t;
    asx_budget run;
    asx_cancel_reason reason;
    asx_dispatch_record rec[4];
    refuser f;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(5u), ASX_OK);
    asx_scheduler_record_dispatches(rec, 4u);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    memset(&f, 0, sizeof(f));
    ASSERT_EQ(asx_region_open_child(r, &f.closed), ASX_OK);
    memset(&reason, 0, sizeof(reason));
    reason.kind = ASX_CANCEL_USER;
    reason.origin_region = f.closed;
    ASSERT_EQ(asx_region_cancel(f.closed, &reason, NULL), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_refuser, &f, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_last_spawn_refusal(), 0u);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(f.spawn, ASX_E_REGION_CLOSED);
    ASSERT_TRUE(f.ticket != 0u);
    /* Poll 1 refuses and waits; step 2's admission delivers the refusal
     * and wakes it on the ready lane; poll 2 sees it delivered. */
    ASSERT_EQ(f.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 2u);
    ASSERT_EQ(rec[1].step, (uint64_t)2);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
    ASSERT_EQ(asx_task_await_refusal(t, f.ticket), ASX_OK);
    ASSERT_EQ(asx_task_await_refusal(t, 0u), ASX_OK);
}

/* bd-orxy: under lab dispatch a spawn from a poll is admitted at the next
 * step, as Rust's spawn mailbox is, and the admission checks the region:
 * max_tasks against the live tasks then (the spawner, completed in the
 * same poll, no longer counts; spawns admitted earlier in the step do),
 * and whether the region still accepts work. A refused child never runs
 * and completes Cancelled. */
typedef struct {
    asx_region_id into;
    uint32_t spawns;
    int cancel_into; /* cancel `into` right after spawning (synchronously) */
    asx_task_id kids[3];
    asx_status spawned[3];
    asx_status pending[3]; /* asx_task_admission_status right after the spawn */
} admitter;

static uint32_t g_kid_polls;

static asx_status poll_counted_kid(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    g_kid_polls++;
    return ASX_OK;
}

static asx_status poll_admitter(void *ud, asx_task_id self) {
    admitter *a = (admitter *)ud;
    uint32_t i;
    (void)self;
    for (i = 0; i < a->spawns; i++) {
        a->spawned[i] = asx_task_spawn(a->into, poll_counted_kid, NULL, &a->kids[i]);
        a->pending[i] = asx_task_admission_status(a->kids[i]);
    }
    if (a->cancel_into) {
        asx_cancel_reason reason;
        memset(&reason, 0, sizeof(reason));
        reason.kind = ASX_CANCEL_USER;
        reason.origin_region = a->into;
        if (asx_region_cancel(a->into, &reason, NULL) != ASX_OK) return ASX_E_INVALID_STATE;
    }
    return ASX_OK; /* completes in the spawning poll */
}

/* Arms a wake timer at 100 on its first poll; a later poll before then
 * parks without polling that sleep again; at 100 it completes. */
typedef struct {
    char name;
    int phase;
} stale_sleeper;

static asx_status poll_stale_sleeper(void *ud, asx_task_id self) {
    stale_sleeper *s = (stale_sleeper *)ud;
    asx_time now = 0;
    if (g_order_n + 1u < sizeof(g_order)) g_order[g_order_n++] = s->name;
    if (asx_runtime_now_ns(&now) == ASX_OK && now >= 100u) return ASX_OK;
    if (s->phase == 0) {
        s->phase = 1;
        return asx_task_wait_until(self, 100u);
    }
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

/* A Rust Sleep wakes the waker it registered last, at that waker's
 * priority. S arms its timer at priority 0; its budget priority then
 * becomes 200 (in Rust a cancel's cleanup budget does that; set directly
 * here so no cancel lane is involved) and S is polled again, getting a new
 * waker, without polling its sleep. At 100 the timer still wakes S at 0,
 * behind M (150), although S's timer is first in registration order (fuzz
 * gen-17-81, bd-1maj). */
TEST(a_timer_wakes_the_waker_it_was_armed_with) {
    asx_region_id r;
    asx_task_id ts;
    asx_task_id tm;
    asx_task_slot *slot;
    asx_budget run;
    asx_budget high = asx_budget_infinite();
    stale_sleeper s = {'S', 0};
    stale_sleeper m = {'M', 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(5u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_stale_sleeper, &s, &ts), ASX_OK);
    high.priority = 150u;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_stale_sleeper, &m, &high, &tm), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &run), ASX_E_PENDING);
    ASSERT_EQ(g_order_n, 2u); /* both armed their timers at 100 */

    ASSERT_EQ(asx_task_slot_lookup(ts, &slot), ASX_OK);
    slot->budget.priority = 200u;
    ASSERT_EQ(asx_task_wake(ts), ASX_OK);
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &run), ASX_E_PENDING);
    ASSERT_EQ(g_order_n, 3u); /* S polled again, with a new waker at 200 */
    ASSERT_EQ(slot->lab_waker_prio, 200u);

    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(g_order_n, 5u);
    ASSERT_EQ(g_order[3], 'M');
    ASSERT_EQ(g_order[4], 'S');
}

TEST(spawn_limit_is_checked_at_the_next_step_admission) {
    asx_region_id r;
    asx_region_limits limits;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget run;
    asx_outcome outcome;
    asx_cancel_reason reason;
    admitter a;
    probe b = {'B', 10u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    memset(&a, 0, sizeof(a));
    a.into = r;
    a.spawns = 2u;
    g_kid_polls = 0;
    ASSERT_EQ(asx_task_spawn(r, poll_admitter, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    limits.max_tasks = 2u;
    limits.max_children = ASX_REGION_UNLIMITED;
    limits.max_obligations = ASX_REGION_UNLIMITED;
    ASSERT_EQ(asx_region_set_limits(r, &limits), ASX_OK);
    ASSERT_EQ(asx_task_admission_status(ta), ASX_OK); /* host tasks are not deferred */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    /* Both spawns returned their task: the call checks nothing. */
    ASSERT_EQ(a.spawned[0], ASX_OK);
    ASSERT_EQ(a.spawned[1], ASX_OK);
    ASSERT_EQ(a.pending[0], ASX_E_PENDING);
    ASSERT_EQ(a.pending[1], ASX_E_PENDING);
    /* At admission the spawner had completed: B and the first child make
     * 2 live tasks, so the second child is refused, without running. */
    ASSERT_EQ(asx_task_admission_status(a.kids[0]), ASX_OK);
    ASSERT_EQ(asx_task_admission_status(a.kids[1]), ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(g_kid_polls, 1u);
    ASSERT_EQ(asx_task_get_outcome(a.kids[1], &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(a.kids[1], &reason), ASX_OK);
    ASSERT_EQ(reason.kind, ASX_CANCEL_USER);
    ASSERT_TRUE(reason.message != NULL &&
                strcmp(reason.message, "[ASUP-E006] region admission limit reached") == 0);
    ASSERT_EQ(reason.timestamp, (asx_time)1000000000u);
    ASSERT_TRUE(asx_runtime_is_quiescent(&g_rt));
}

TEST(spawn_into_a_region_closed_before_admission_is_refused) {
    asx_region_id r;
    asx_region_id c;
    asx_task_id ta;
    asx_budget run;
    asx_cancel_reason reason;
    admitter a;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_region_open_child(r, &c), ASX_OK);
    memset(&a, 0, sizeof(a));
    a.into = c;
    a.spawns = 1u;
    a.cancel_into = 1;
    g_kid_polls = 0;
    ASSERT_EQ(asx_task_spawn(r, poll_admitter, &a, &ta), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    ASSERT_EQ(a.spawned[0], ASX_OK);
    /* The region closed before the next step: Rust's admission refuses the
     * spawn (SpawnError::RegionClosed), resolving it ParentCancelled. */
    ASSERT_EQ(asx_task_admission_status(a.kids[0]), ASX_E_REGION_CLOSED);
    ASSERT_EQ(g_kid_polls, 0u);
    ASSERT_EQ(asx_task_get_cancel_reason(a.kids[0], &reason), ASX_OK);
    ASSERT_EQ(reason.kind, ASX_CANCEL_PARENT);
    ASSERT_TRUE(asx_runtime_is_quiescent(&g_rt));
}

TEST(admission_status_of_a_bad_handle_is_a_lookup_error) {
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_TRUE(asx_task_admission_status(ASX_INVALID_ID) != ASX_OK);
}

TEST(use_lab_dispatch_requires_no_live_task) {
    asx_region_id r;
    asx_task_id t;
    probe a = {'A', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_E_INVALID_STATE);
    /* A runtime reset turns it off again. */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_lab_dispatch_active(), 0);
}

int main(void) {
    fprintf(stderr, "=== test_lab_dispatch ===\n");
    RUN_TEST(rng_replicates_rust_xorshift64);
    RUN_TEST(rng_remaps_degenerate_seeds);
    RUN_TEST(ties_are_broken_by_the_step_value_in_generation_order);
    RUN_TEST(host_tasks_draw_from_rust_detentropy_streams);
    RUN_TEST(completed_tasks_free_their_arena_index_for_the_next_task);
    RUN_TEST(cancel_lane_is_served_first);
    RUN_TEST(auto_advance_moves_the_clock_to_the_next_timer);
    RUN_TEST(pre_poll_quota_cancel_has_rust_lab_attribution);
    RUN_TEST(dispatch_records_carry_task_lane_step_and_time);
    RUN_TEST(budget_cancel_at_a_completing_checkpoint_dispatches_the_retired_task);
    RUN_TEST(budget_deadline_timer_cancels_on_the_cancel_lane);
    RUN_TEST(child_region_open_and_close_are_next_step_commands);
    RUN_TEST(closing_an_empty_child_region_wakes_the_closer);
    RUN_TEST(child_region_commands_apply_at_once_without_lab_dispatch);
    RUN_TEST(region_cancel_request_applies_at_the_next_step);
    RUN_TEST(region_command_queue_full_is_failure_atomic);
    RUN_TEST(handle_abort_applies_at_the_next_step);
    RUN_TEST(handle_abort_of_an_unadmitted_child_applies_at_admission);
    RUN_TEST(handle_aborts_in_one_step_coalesce_to_the_strongest);
    RUN_TEST(handle_abort_applies_at_once_without_lab_dispatch);
    RUN_TEST(spawn_refusal_reaches_a_join_at_the_next_step);
    RUN_TEST(a_timer_wakes_the_waker_it_was_armed_with);
    RUN_TEST(spawn_limit_is_checked_at_the_next_step_admission);
    RUN_TEST(spawn_into_a_region_closed_before_admission_is_refused);
    RUN_TEST(admission_status_of_a_bad_handle_is_a_lookup_error);
    RUN_TEST(use_lab_dispatch_requires_no_live_task);

    TEST_REPORT();
    return test_failures;
}
