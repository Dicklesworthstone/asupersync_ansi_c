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

/* All seven, the passing run: every verdict PASS. */
static int all_pass(void) {
    uint32_t i;
    for (i = 0; i < asx_oracle_kernel_count(); i++) {
        if (check(asx_oracle_kernel_name(i)).verdict != ASX_ORACLE_PASS) return 0;
    }
    return 1;
}

TEST(kernel_oracles_carry_rusts_registry_names_and_unknown_names_fail) {
    static const char *const want[] = {
        "task_leak",   "obligation_leak",   "quiescence",           "finalizer",
        "region_tree", "deadline_monotone", "cancellation_protocol"};
    uint32_t i;
    asx_oracle_result r;
    ASSERT_EQ(asx_oracle_kernel_count(), 7u);
    for (i = 0; i < 7u; i++) { ASSERT_STR_EQ(asx_oracle_kernel_name(i), want[i]); }
    ASSERT_TRUE(asx_oracle_kernel_name(7u) == NULL);
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
    TEST_REPORT();
    return test_failures;
}
