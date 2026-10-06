/*
 * test_budget_obligation.c — task/region budgets (deadline, poll quota,
 * cost quota) and obligation kinds, holders, and leak policies.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <string.h>

#define MS ((uint64_t)1000000u)

static asx_runtime g_rt;

/* Virtual-clock runtime (stub hooks) with the given leak policy. */
static asx_status setup_with_policy(asx_leak_response response,
                                    asx_leak_escalation_config *escalation) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    cfg.leak_response = response;
    cfg.leak_escalation = escalation;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    return st;
}

static void setup(void) {
    asx_status st = setup_with_policy(ASX_LEAK_LOG, NULL);
    (void)st;
}

/* -------------------------------------------------------------------
 * Fixtures
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t polls;
    int saw_cancel;
    asx_cancel_kind kind;
    int park;           /* park between polls (else yield) */
    uint64_t cost;      /* cost charged per poll (0 = none) */
    int cost_exhausted; /* asx_task_consume_cost reported exhaustion */
} probe_state;

/* Runs until cancelled, then records the cancel kind and completes. */
static asx_status poll_probe(void *ud, asx_task_id self) {
    probe_state *p = (probe_state *)ud;
    asx_checkpoint_result cp;
    p->polls++;
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) {
        p->saw_cancel = 1;
        p->kind = cp.kind;
        return ASX_OK;
    }
    if (p->cost != 0u && asx_task_consume_cost(self, p->cost) == ASX_E_COST_QUOTA_EXHAUSTED) {
        p->cost_exhausted = 1;
    }
    if (p->park && asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

typedef struct {
    asx_region_id region;
    asx_obligation_id ob;
    asx_status reserve_status;
    int park; /* park after reserving instead of completing */
} reserver_state;

/* Reserves one obligation on the first poll; completes unless `park`. */
static asx_status poll_reserver(void *ud, asx_task_id self) {
    reserver_state *s = (reserver_state *)ud;
    asx_checkpoint_result cp;
    if (s->ob == ASX_INVALID_ID) { s->reserve_status = asx_obligation_reserve(s->region, &s->ob); }
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) return ASX_OK;
    if (!s->park) return ASX_OK;
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Budgets
 * ------------------------------------------------------------------- */

TEST(deadline_cancels_parked_task_on_virtual_time) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;
    asx_outcome out;

    setup();
    memset(&p, 0, sizeof(p));
    p.park = 1;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = asx_runtime_virtual_now() + 5u * MS;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);

    /* Without the deadline the parked task would block forever; the
     * deadline timer wakes it and the scheduler cancels it (DEADLINE). */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_DEADLINE);
    ASSERT_EQ(p.polls, 2u);
    ASSERT_TRUE(asx_runtime_virtual_now() >= tb.deadline);
    ASSERT_EQ(asx_task_get_outcome(t, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
}

TEST(expired_deadline_cancels_before_first_poll) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;

    setup();
    memset(&p, 0, sizeof(p));
    asx_runtime_virtual_advance(10u * MS);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.deadline = 1u * MS;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(p.polls, 1u);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_DEADLINE);
}

TEST(poll_quota_cancels_after_quota_polls) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;
    asx_budget left;

    setup();
    memset(&p, 0, sizeof(p));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_from_polls(3);
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    /* Three budgeted polls, then one cancel-observing poll. */
    ASSERT_EQ(p.polls, 4u);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(asx_task_get_budget(t, &left), ASX_OK);
    ASSERT_EQ(left.poll_quota, 0u);
}

TEST(unbounded_poll_quota_is_not_consumed) {
    asx_region_id r;
    asx_task_id t;
    asx_budget run;
    asx_budget left;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_complete, NULL, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(t, &left), ASX_OK);
    ASSERT_EQ(left.poll_quota, UINT32_MAX);
    ASSERT_EQ(left.deadline, (asx_time)0);
}

TEST(cost_quota_exhaustion_cancels_with_cost_budget) {
    asx_region_id r;
    asx_task_id t;
    probe_state p;
    asx_budget tb;
    asx_budget run;
    asx_budget left;

    setup();
    memset(&p, 0, sizeof(p));
    p.cost = 4u;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    tb = asx_budget_infinite();
    tb.cost_quota = 10u;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_probe, &p, &tb, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    /* 10 -> 6 -> 2, third charge (4 > 2) fails and cancels. */
    ASSERT_TRUE(p.cost_exhausted);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_COST_BUDGET);
    ASSERT_EQ(p.polls, 4u);
    ASSERT_EQ(asx_task_get_budget(t, &left), ASX_OK);
    ASSERT_EQ(left.cost_quota, (uint64_t)2u);
}

TEST(consume_cost_rejects_stale_handle) {
    setup();
    ASSERT_EQ(asx_task_consume_cost(ASX_INVALID_ID, 1u), ASX_E_NOT_FOUND);
}

TEST(budgets_tighten_down_the_region_tree) {
    asx_region_id root;
    asx_region_id child;
    asx_region_id grandchild;
    asx_region_id sibling;
    asx_task_id t;
    asx_budget b;
    asx_budget got;

    setup();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_get_budget(root, &got), ASX_OK);
    ASSERT_EQ(got.deadline, (asx_time)0);
    ASSERT_EQ(got.poll_quota, UINT32_MAX);

    b = asx_budget_infinite();
    b.deadline = 100u * MS;
    ASSERT_EQ(asx_region_open_child_with_budget(root, &b, &child), ASX_OK);

    /* A looser deadline on the grandchild cannot widen the parent's. */
    b = asx_budget_infinite();
    b.deadline = 200u * MS;
    b.poll_quota = 7u;
    ASSERT_EQ(asx_region_open_child_with_budget(child, &b, &grandchild), ASX_OK);
    ASSERT_EQ(asx_region_get_budget(grandchild, &got), ASX_OK);
    ASSERT_EQ(got.deadline, (asx_time)(100u * MS));
    ASSERT_EQ(got.poll_quota, 7u);

    /* Plain children inherit the parent's budget unchanged. */
    ASSERT_EQ(asx_region_open_child(child, &sibling), ASX_OK);
    ASSERT_EQ(asx_region_get_budget(sibling, &got), ASX_OK);
    ASSERT_EQ(got.deadline, (asx_time)(100u * MS));
    ASSERT_EQ(got.poll_quota, UINT32_MAX);

    /* Tasks inherit the region budget, met with their own. */
    b = asx_budget_from_polls(50);
    ASSERT_EQ(asx_task_spawn_with_budget(grandchild, poll_complete, NULL, &b, &t), ASX_OK);
    ASSERT_EQ(asx_task_get_budget(t, &got), ASX_OK);
    ASSERT_EQ(got.poll_quota, 7u);
    ASSERT_EQ(got.deadline, (asx_time)(100u * MS));

    ASSERT_EQ(asx_region_get_budget(grandchild, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_get_budget(t, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(region_deadline_cancels_its_tasks) {
    asx_region_id root;
    asx_region_id child;
    asx_task_id t;
    probe_state p;
    asx_budget b;
    asx_budget run;

    setup();
    memset(&p, 0, sizeof(p));
    p.park = 1;
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    b = asx_budget_infinite();
    b.deadline = asx_runtime_virtual_now() + 3u * MS;
    ASSERT_EQ(asx_region_open_child_with_budget(root, &b, &child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, poll_probe, &p, &t), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(root, &run), ASX_OK);
    ASSERT_TRUE(p.saw_cancel);
    ASSERT_EQ(p.kind, ASX_CANCEL_DEADLINE);
}

/* -------------------------------------------------------------------
 * Obligations: kinds and holders
 * ------------------------------------------------------------------- */

TEST(reserve_ex_records_kind_and_holder) {
    asx_region_id r;
    asx_task_id t;
    asx_obligation_id o;
    asx_obligation_info info;
    asx_budget run;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_complete, NULL, &t), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_SEND_PERMIT, t, &o), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(info.kind, ASX_OBLIGATION_KIND_SEND_PERMIT);
    ASSERT_EQ(info.holder, t);
    ASSERT_EQ(info.region, r);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_NONE);

    /* Committed before the holder completes: no leak. */
    ASSERT_EQ(asx_obligation_commit(o), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

TEST(reserve_ex_validates_arguments) {
    asx_region_id r;
    asx_task_id t;
    asx_obligation_id o;
    asx_obligation_info info;
    asx_budget run;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, (asx_obligation_kind)99, ASX_INVALID_ID, &o),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_LEASE, ASX_INVALID_ID, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_obligation_get_info(ASX_INVALID_ID, &info), ASX_E_NOT_FOUND);

    /* A completed task can no longer hold obligations. */
    ASSERT_EQ(asx_task_spawn(r, poll_complete, NULL, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_ACK, t, &o), ASX_E_INVALID_STATE);

    /* Unowned obligations are allowed. */
    ASSERT_EQ(asx_obligation_reserve_ex(r, ASX_OBLIGATION_KIND_IO_OP, ASX_INVALID_ID, &o), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.holder, ASX_INVALID_ID);
    ASSERT_EQ(info.kind, ASX_OBLIGATION_KIND_IO_OP);
    ASSERT_EQ(asx_obligation_get_info(o, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(reserve_outside_poll_has_no_holder) {
    asx_region_id r;
    asx_obligation_id o;
    asx_obligation_info info;

    setup();
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(r, &o), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.holder, ASX_INVALID_ID);
    ASSERT_EQ(info.kind, ASX_OBLIGATION_KIND_GENERIC);
    ASSERT_EQ(asx_obligation_abort(o), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(o, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_EXPLICIT);
}

/* -------------------------------------------------------------------
 * Obligations: leak resolution at holder completion
 * ------------------------------------------------------------------- */

TEST(reserve_in_poll_binds_current_task_and_leaks_under_log) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_LOG, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(s.reserve_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.holder, t);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);

    /* A leaked obligation is terminal: the region can still close. */
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
}

TEST(cancelled_holder_aborts_obligation_with_cancel_reason) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;

    setup();
    memset(&s, 0, sizeof(s));
    s.park = 1;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_RESERVED);

    /* Draining cancels the holder; its obligation is aborted, not leaked. */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_region_drain(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_CANCEL);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

TEST(recover_policy_aborts_leaked_obligation) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_RECOVER, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_LEAK_RECOVERED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
}

TEST(panic_policy_routes_leak_through_containment) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_obligation_info info;
    asx_budget run;
    asx_status st;
    int poisoned = 0;

    ASSERT_EQ(setup_with_policy(ASX_LEAK_PANIC, NULL), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    st = asx_scheduler_run(r, &run);
    ASSERT_EQ(asx_obligation_get_info(s.ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);

    /* Same routing as a failing poll: POISON_REGION poisons and keeps
     * draining; FAIL_FAST / ERROR_ONLY surface the fault to the caller. */
    if (asx_containment_policy_active() == ASX_CONTAIN_POISON_REGION) {
        ASSERT_EQ(st, ASX_OK);
        ASSERT_EQ(asx_region_is_poisoned(r, &poisoned), ASX_OK);
        ASSERT_TRUE(poisoned);
    } else {
        ASSERT_EQ(st, ASX_E_UNRESOLVED_OBLIGATIONS);
    }
}

TEST(leak_escalation_switches_policy_at_threshold) {
    asx_leak_escalation_config esc;
    asx_region_id r;
    asx_task_id t;
    reserver_state s[3];
    asx_obligation_info info;
    asx_budget run;
    uint32_t i;

    esc.threshold = 2u;
    esc.escalate_to = ASX_LEAK_RECOVER;
    ASSERT_EQ(setup_with_policy(ASX_LEAK_SILENT, &esc), ASX_OK);
    memset(s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    for (i = 0; i < 3u; i++) {
        s[i].region = r;
        s[i].ob = ASX_INVALID_ID;
        ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s[i], &t), ASX_OK);
    }
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    /* Tasks complete in slot order: two silent leaks, then the escalated
     * policy (RECOVER) aborts the third. */
    ASSERT_EQ(asx_obligation_get_info(s[0].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_get_info(s[1].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_get_info(s[2].ob, &info), ASX_OK);
    ASSERT_EQ(info.state, ASX_OBLIGATION_ABORTED);
    ASSERT_EQ(info.abort_reason, ASX_OBLIGATION_ABORT_LEAK_RECOVERED);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)3u);
}

TEST(runtime_reset_clears_leak_count) {
    asx_region_id r;
    asx_task_id t;
    reserver_state s;
    asx_budget run;

    setup();
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    s.region = r;
    s.ob = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn(r, poll_reserver, &s, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)1u);
    asx_runtime_reset();
    ASSERT_EQ(asx_obligation_leak_count(), (uint64_t)0u);
}

int main(void) {
    fprintf(stderr, "=== test_budget_obligation ===\n");

    RUN_TEST(deadline_cancels_parked_task_on_virtual_time);
    RUN_TEST(expired_deadline_cancels_before_first_poll);
    RUN_TEST(poll_quota_cancels_after_quota_polls);
    RUN_TEST(unbounded_poll_quota_is_not_consumed);
    RUN_TEST(cost_quota_exhaustion_cancels_with_cost_budget);
    RUN_TEST(consume_cost_rejects_stale_handle);
    RUN_TEST(budgets_tighten_down_the_region_tree);
    RUN_TEST(region_deadline_cancels_its_tasks);
    RUN_TEST(reserve_ex_records_kind_and_holder);
    RUN_TEST(reserve_ex_validates_arguments);
    RUN_TEST(reserve_outside_poll_has_no_holder);
    RUN_TEST(reserve_in_poll_binds_current_task_and_leaks_under_log);
    RUN_TEST(cancelled_holder_aborts_obligation_with_cancel_reason);
    RUN_TEST(recover_policy_aborts_leaked_obligation);
    RUN_TEST(panic_policy_routes_leak_through_containment);
    RUN_TEST(leak_escalation_switches_policy_at_threshold);
    RUN_TEST(runtime_reset_clears_leak_count);

    TEST_REPORT();
    return test_failures;
}
