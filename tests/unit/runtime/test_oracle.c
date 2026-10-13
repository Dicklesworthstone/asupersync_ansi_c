/*
 * test_oracle.c — the kernel-state oracles (src/runtime/oracle.c,
 * bd-9kll.9.2, docs/LAB_ORACLE_CATALOG.md).
 *
 * Each oracle checks something the kernel enforces by construction, so a
 * violation cannot be reached through the API. The tests reach it the way
 * test_quiescence.c does, by writing the slot fields directly, and show that
 * the oracle names it and that the healthy state passes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/runtime/replay.h>
#include <asx/runtime/rt.h>
#include <string.h>

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

static void cleanup_mark(void *ctx) {
    int *flag = (int *)ctx;
    *flag += 1;
}

static void fresh(void) {
    asx_runtime_reset();
    asx_ghost_reset();
}

static asx_oracle_result check(const char *name) { return asx_oracle_check_named(name); }

/* Every kernel-state oracle, the passing run: every verdict PASS. */
static int all_pass(void) {
    uint32_t i;
    for (i = 0; i < asx_oracle_kernel_count(); i++) {
        if (check(asx_oracle_kernel_name(i)).verdict != ASX_ORACLE_PASS) return 0;
    }
    return 1;
}

TEST(kernel_oracles_carry_rusts_registry_names_and_unknown_names_fail) {
    static const char *const want[] = {"task_leak",
                                       "obligation_leak",
                                       "quiescence",
                                       "finalizer",
                                       "region_tree",
                                       "deadline_monotone",
                                       "cancellation_protocol",
                                       "cancel_correctness",
                                       "cancel_debt",
                                       "task_counters",
                                       "arena_counters"};
    uint32_t i;
    asx_oracle_result r;
    ASSERT_EQ(asx_oracle_kernel_count(), 11u);
    for (i = 0; i < 11u; i++) { ASSERT_STR_EQ(asx_oracle_kernel_name(i), want[i]); }
    ASSERT_TRUE(asx_oracle_kernel_name(11u) == NULL);
    r = check("no_such_oracle");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.oracle_name, "unknown");
    r = asx_oracle_check_named(NULL);
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
}

TEST(kernel_oracles_refuse_a_null_or_uninitialized_lab) {
    asx_lab lab;
    asx_oracle_result r;
    memset(&lab, 0, sizeof(lab));
    r = asx_oracle_region_tree(NULL, NULL);
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    r = asx_oracle_task_leak(&lab, NULL);
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_INCONCLUSIVE);
    lab.initialized = 1;
    fresh();
    r = asx_oracle_deadline_monotone(&lab, NULL);
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_PASS);
    ASSERT_STR_EQ(r.oracle_name, "deadline_monotone");
}

TEST(kernel_oracles_pass_on_a_healthy_tree_with_a_closed_child) {
    asx_region_id root;
    asx_region_id child;
    asx_task_id t;
    asx_budget budget;
    asx_lab lab;

    fresh();
    memset(&lab, 0, sizeof(lab));
    lab.initialized = 1;
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, poll_complete, NULL, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(root, &budget), ASX_OK);
    ASSERT_TRUE(all_pass());

    /* The child closes; the root stays open and is the one root. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    ASSERT_TRUE(all_pass());
    ASSERT_EQ((int)asx_oracle_region_quiescence(&lab, NULL).verdict, (int)ASX_ORACLE_PASS);
}

TEST(task_leak_names_a_closed_region_with_a_live_task) {
    asx_region_id rid;
    asx_task_id t;
    asx_region_slot *region = NULL;
    asx_oracle_result r;

    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_TRUE(all_pass());

    region->state = ASX_REGION_CLOSED; /* what asx_region_finalize_one refuses */
    r = check("task_leak");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_STR_EQ(r.oracle_name, "task_leak");
    ASSERT_EQ((int)check("quiescence").verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ((int)check("obligation_leak").verdict, (int)ASX_ORACLE_PASS);
    ASSERT_EQ((int)check("finalizer").verdict, (int)ASX_ORACLE_PASS);
}

TEST(obligation_leak_names_a_closed_region_with_a_reserved_obligation) {
    asx_region_id rid;
    asx_task_id holder;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;
    asx_oracle_result r;

    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &holder), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, holder, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ((int)check("obligation_leak").verdict, (int)ASX_ORACLE_PASS); /* region is open */

    region->state = ASX_REGION_CLOSED;
    r = check("obligation_leak");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_EQ((int)check("quiescence").verdict, (int)ASX_ORACLE_FAIL);
}

TEST(finalizer_names_a_closed_region_with_cleanup_pending) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    asx_oracle_result r;
    int ran = 0;

    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &ran, &handle), ASX_OK);
    ASSERT_TRUE(all_pass()); /* open: nothing to say yet */

    region->state = ASX_REGION_CLOSED;
    r = check("finalizer");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_EQ((int)check("quiescence").verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ((int)check("task_leak").verdict, (int)ASX_ORACLE_PASS);
}

TEST(quiescence_names_a_closed_region_with_a_child_that_is_not_closed) {
    asx_region_id root;
    asx_region_id child;
    asx_region_slot *parent = NULL;
    asx_oracle_result r;

    fresh();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(root, &parent), ASX_OK);
    ASSERT_TRUE(all_pass());

    parent->state = ASX_REGION_CLOSED; /* its child is still open */
    r = check("quiescence");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_TRUE(r.detail_count >= 1u);
    ASSERT_EQ((int)check("task_leak").verdict, (int)ASX_ORACLE_PASS);
}

TEST(region_tree_names_a_second_root_and_an_unlisted_child) {
    asx_region_id a;
    asx_region_id b;
    asx_region_id child;
    asx_region_slot *parent = NULL;
    asx_oracle_result r;

    fresh();
    ASSERT_EQ(asx_region_open(&a), ASX_OK);
    ASSERT_EQ(asx_region_open_child(a, &child), ASX_OK);
    ASSERT_EQ((int)check("region_tree").verdict, (int)ASX_ORACLE_PASS);

    /* The rooted-tree rule: a second parentless region. */
    ASSERT_EQ(asx_region_open(&b), ASX_OK);
    r = check("region_tree");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.message, "the region tree has more than one root");

    /* A child its parent does not list. */
    fresh();
    ASSERT_EQ(asx_region_open(&a), ASX_OK);
    ASSERT_EQ(asx_region_open_child(a, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(a, &parent), ASX_OK);
    parent->child_count = 0u;
    r = check("region_tree");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.message, "a region is not listed by its parent");
    ASSERT_EQ(r.detail_count, 1u);
}

TEST(deadline_monotone_names_a_child_with_a_later_deadline_than_its_parent) {
    asx_region_id root;
    asx_region_id child;
    asx_region_slot *rs = NULL;
    asx_region_slot *cs = NULL;
    asx_oracle_result r;

    fresh();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(root, &rs), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &cs), ASX_OK);
    rs->budget.deadline = 100u;
    cs->budget.deadline = 50u; /* tighter: fine */
    ASSERT_EQ((int)check("deadline_monotone").verdict, (int)ASX_ORACLE_PASS);

    cs->budget.deadline = 200u; /* looser than the parent's */
    r = check("deadline_monotone");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
}

TEST(cancellation_protocol_names_a_child_a_region_cancel_did_not_reach) {
    asx_region_id root;
    asx_region_id child;
    asx_region_slot *rs = NULL;
    asx_region_slot *cs = NULL;
    asx_cancel_reason req;
    asx_oracle_result r;

    fresh();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(root, &rs), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &cs), ASX_OK);

    /* A cancel the kernel did not propagate. */
    rs->cancel_requested = 1;
    r = check("cancellation_protocol");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);

    /* The kernel's own region cancel reaches every descendant. */
    fresh();
    ASSERT_EQ(asx_region_open(&root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    memset(&req, 0, sizeof(req));
    req.kind = ASX_CANCEL_USER;
    req.origin_region = root;
    req.origin_task = ASX_INVALID_ID;
    req.timestamp = 5u;
    ASSERT_EQ(asx_region_cancel(root, &req, NULL), ASX_OK);
    ASSERT_EQ((int)check("cancellation_protocol").verdict, (int)ASX_ORACLE_PASS);
}

static void request_cancel(asx_region_id rid, asx_task_id t) {
    asx_cancel_reason req;
    memset(&req, 0, sizeof(req));
    req.kind = ASX_CANCEL_USER;
    req.origin_region = rid;
    req.origin_task = ASX_INVALID_ID;
    req.timestamp = 5u;
    ASSERT_EQ(asx_cancel_request(t, &req), ASX_OK);
}

TEST(cancel_correctness_names_a_witness_that_is_not_its_tasks) {
    asx_region_id rid;
    asx_task_id t;
    asx_task_slot *ts = NULL;
    asx_oracle_result r;
    uint32_t epoch;

    /* The kernel's own witness is its task's. */
    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t), ASX_OK);
    request_cancel(rid, t);
    ASSERT_EQ(asx_task_slot_lookup(t, &ts), ASX_OK);
    ASSERT_TRUE(ts->cancel_witness != ASX_INVALID_ID);
    ASSERT_TRUE(all_pass());

    /* The task's record moved to another epoch than its witness's. */
    epoch = ts->cancel_epoch;
    ts->cancel_epoch = epoch + 1u;
    r = check("cancel_correctness");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_STR_EQ(r.message, "a cancel witness carries another epoch than its task's");
    ts->cancel_epoch = epoch;
    ASSERT_EQ((int)check("cancel_correctness").verdict, (int)ASX_ORACLE_PASS);

    /* A witness that ran ahead of the phase its task is in. */
    ASSERT_EQ(asx_cancel_witness_advance(ts->cancel_witness, ASX_CANCEL_PHASE_FINALIZING), ASX_OK);
    r = check("cancel_correctness");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.message, "a cancel witness is ahead of its task's cancel phase");
    ts->cancel_phase = ASX_CANCEL_PHASE_FINALIZING;
    ASSERT_EQ((int)check("cancel_correctness").verdict, (int)ASX_ORACLE_PASS);

    /* A completed task that kept its witness (the kernel releases it at
     * completion). */
    ts->state = ASX_TASK_COMPLETED;
    r = check("cancel_correctness");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.message, "a completed task still holds an active cancel witness");
    ASSERT_EQ(asx_cancel_witness_release(ts->cancel_witness), ASX_OK);
    ASSERT_EQ((int)check("cancel_correctness").verdict, (int)ASX_ORACLE_PASS);
}

TEST(cancel_debt_names_a_requested_cancel_that_nothing_will_run) {
    asx_region_id rid;
    asx_task_id t;
    asx_task_slot *ts = NULL;
    asx_oracle_result r;

    /* Without lab dispatch the sweep's idle handling delivers the cancel:
     * the same state is not a violation, and the oracle says it did not look. */
    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t), ASX_OK);
    request_cancel(rid, t);
    ASSERT_EQ(asx_task_slot_lookup(t, &ts), ASX_OK);
    ts->lab_scheduled = 0u;
    r = check("cancel_debt");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_PASS);
    ASSERT_TRUE(strstr(r.message, "no lab dispatch") != NULL);

    /* Under lab dispatch the request scheduled the task. */
    fresh();
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_OK);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t), ASX_OK);
    request_cancel(rid, t);
    ASSERT_EQ(asx_task_slot_lookup(t, &ts), ASX_OK);
    ASSERT_EQ((int)ts->state, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(ts->lab_scheduled != 0u);
    ASSERT_TRUE(all_pass());

    /* A wake that never arrived: not scheduled, not polled since, nothing armed. */
    ts->lab_scheduled = 0u;
    r = check("cancel_debt");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_STR_EQ(r.message, "a task has a cancel requested and nothing will run it");

    /* Polled since, it is the task's own business; masked, it defers; armed
     * with a timer, the timer runs it. */
    ts->cancel_polled = 1u;
    ASSERT_EQ((int)check("cancel_debt").verdict, (int)ASX_ORACLE_PASS);
    ts->cancel_polled = 0u;
    ts->mask_depth = 1u;
    ASSERT_EQ((int)check("cancel_debt").verdict, (int)ASX_ORACLE_PASS);
    ts->mask_depth = 0u;
    ts->timer_armed = 1u;
    ASSERT_EQ((int)check("cancel_debt").verdict, (int)ASX_ORACLE_PASS);
    ts->timer_armed = 0u;
    ASSERT_EQ((int)check("cancel_debt").verdict, (int)ASX_ORACLE_FAIL);
}

TEST(task_counters_names_a_region_whose_counter_disagrees_with_its_tasks) {
    asx_region_id rid;
    asx_region_id other;
    asx_task_id a;
    asx_task_id b;
    asx_region_slot *region = NULL;
    asx_task_slot *ts = NULL;
    asx_oracle_result r;
    asx_budget budget;

    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_open_child(rid, &other), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &a), ASX_OK);
    ASSERT_EQ(asx_task_spawn(other, poll_pending, NULL, &b), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_TRUE(all_pass());

    /* A counter that ran ahead of its tasks, and one that fell behind. */
    region->task_count += 1u;
    r = check("task_counters");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_STR_EQ(r.message, "a region's live-task counter disagrees with its tasks");
    region->task_count -= 2u;
    ASSERT_EQ((int)check("task_counters").verdict, (int)ASX_ORACLE_FAIL);
    region->task_count += 1u;
    ASSERT_EQ((int)check("task_counters").verdict, (int)ASX_ORACLE_PASS);

    /* A task that completed and was not counted off: a different state, the
     * same disagreement. The kernel does both in one call. */
    ASSERT_EQ(asx_task_slot_lookup(b, &ts), ASX_OK);
    ts->state = ASX_TASK_COMPLETED;
    r = check("task_counters");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);

    /* The kernel's own completion keeps them equal. */
    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &a), ASX_OK);
    ASSERT_EQ((int)check("task_counters").verdict, (int)ASX_ORACLE_PASS);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ((int)check("task_counters").verdict, (int)ASX_ORACLE_PASS);
}

TEST(arena_counters_names_a_live_counter_or_free_list_that_disagrees_with_its_arena) {
    asx_region_id rid;
    asx_task_id a;
    asx_task_id b;
    asx_outcome out;
    asx_budget budget;
    asx_kernel *k = g_kernels[0];
    asx_oracle_result r;

    /* Two completed tasks, one joined: its slot is released and on the free list. */
    fresh();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &a), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &b), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_join(a, &out), ASX_OK);
    ASSERT_TRUE(k->task_free_head != 0u);
    ASSERT_TRUE(all_pass());

    /* A live counter off by one, for each arena. */
    k->task_live += 1u;
    r = check("arena_counters");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_EQ(r.detail_count, 1u);
    ASSERT_STR_EQ(r.message, "the live task counter disagrees with the live tasks");
    k->task_live -= 1u;
    k->region_live += 1u;
    r = check("arena_counters");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.message, "the live region counter disagrees with the live regions");
    k->region_live -= 1u;
    k->obligation_live += 1u;
    r = check("arena_counters");
    ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
    ASSERT_STR_EQ(r.message, "the live obligation counter disagrees with the live obligations");
    k->obligation_live -= 1u;
    ASSERT_TRUE(all_pass());

    /* A free list that lost its released slot, and one that names a live one. */
    {
        uint32_t head = k->task_free_head;
        k->task_free_head = 0u;
        r = check("arena_counters");
        ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
        ASSERT_STR_EQ(r.message, "a free list does not hold exactly the released slots");
        k->task_free_head = (uint32_t)asx_handle_slot(b) + 1u;
        r = check("arena_counters");
        ASSERT_EQ((int)r.verdict, (int)ASX_ORACLE_FAIL);
        ASSERT_STR_EQ(r.message, "a free-list entry is not a released slot");
        k->task_free_head = head;
    }
    ASSERT_TRUE(all_pass());
}

int main(void) {
    fprintf(stderr, "=== test_oracle ===\n");
    RUN_TEST(kernel_oracles_carry_rusts_registry_names_and_unknown_names_fail);
    RUN_TEST(kernel_oracles_refuse_a_null_or_uninitialized_lab);
    RUN_TEST(kernel_oracles_pass_on_a_healthy_tree_with_a_closed_child);
    RUN_TEST(task_leak_names_a_closed_region_with_a_live_task);
    RUN_TEST(obligation_leak_names_a_closed_region_with_a_reserved_obligation);
    RUN_TEST(finalizer_names_a_closed_region_with_cleanup_pending);
    RUN_TEST(quiescence_names_a_closed_region_with_a_child_that_is_not_closed);
    RUN_TEST(region_tree_names_a_second_root_and_an_unlisted_child);
    RUN_TEST(deadline_monotone_names_a_child_with_a_later_deadline_than_its_parent);
    RUN_TEST(cancellation_protocol_names_a_child_a_region_cancel_did_not_reach);
    RUN_TEST(cancel_correctness_names_a_witness_that_is_not_its_tasks);
    RUN_TEST(cancel_debt_names_a_requested_cancel_that_nothing_will_run);
    RUN_TEST(task_counters_names_a_region_whose_counter_disagrees_with_its_tasks);
    RUN_TEST(arena_counters_names_a_live_counter_or_free_list_that_disagrees_with_its_arena);
    TEST_REPORT();
    return test_failures;
}
