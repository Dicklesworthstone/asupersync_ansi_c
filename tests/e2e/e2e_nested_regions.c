/*
 * e2e_nested_regions.c — end-to-end nested region lifecycle scenarios
 *
 * Exercises parent/child/grandchild region shutdown ordering, child-count
 * unlinking, and detailed quiescence evidence for Q2.
 *
 * Output:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *   DIGEST <hex>
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime/runtime_internal.h"
#include <asx/asx.h>
#include <asx/runtime/trace.h>
#include <stdio.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static void log_region_state(const char *label, asx_region_id id) {
    asx_region_slot *slot = NULL;

    if (asx_region_slot_lookup(id, &slot) != ASX_OK) {
        fprintf(stderr, "[nested-e2e] %s region=0x%016llx lookup=not-found\n", label,
                (unsigned long long)id);
        return;
    }

    fprintf(stderr,
            "[nested-e2e] %s region=0x%016llx state=%d parent=0x%016llx child_count=%u "
            "task_count=%u task_total=%u\n",
            label, (unsigned long long)id, (int)slot->state, (unsigned long long)slot->parent_id,
            slot->child_count, slot->task_count, slot->task_total);
}

static void log_quiescence_state(const char *label, asx_region_id id) {
    asx_quiescence_report report;

    if (asx_quiescence_check_detailed(id, &report) != ASX_OK) {
        fprintf(stderr, "[nested-e2e] %s quiescence=not-found\n", label);
        return;
    }

    fprintf(stderr,
            "[nested-e2e] %s quiescent=%d q1=%d q2=%d q3=%d q4=%d state=%d live=%u reserved=%u "
            "cleanup_drained=%d\n",
            label, report.quiescent, report.q1_tasks_complete, report.q2_children_closed,
            report.q3_obligations_resolved, report.q4_cleanup_drained, (int)report.region_state,
            report.progress.tasks_live, report.progress.obligations_reserved,
            report.progress.cleanup_drained);
}

static void scenario_nested_lifecycle_full(void) {
    SCENARIO_BEGIN("nested.lifecycle_full");
    asx_region_id root;
    asx_region_id child1;
    asx_region_id child2;
    asx_region_id grandchild;
    asx_task_id root_task;
    asx_task_id child1_task;
    asx_task_id child2_task;
    asx_task_id grandchild_task;
    asx_region_slot *root_slot = NULL;
    asx_region_slot *child1_slot = NULL;
    asx_region_slot *child2_slot = NULL;
    asx_region_slot *grandchild_slot = NULL;
    asx_quiescence_report report;
    asx_budget budget;

    asx_runtime_reset();

    SCENARIO_CHECK(asx_region_open(&root) == ASX_OK, "open_root");
    SCENARIO_CHECK(asx_region_open_child(root, &child1) == ASX_OK, "open_child1");
    SCENARIO_CHECK(asx_region_open_child(root, &child2) == ASX_OK, "open_child2");
    SCENARIO_CHECK(asx_region_open_child(child1, &grandchild) == ASX_OK, "open_grandchild");

    SCENARIO_CHECK(asx_region_slot_lookup(root, &root_slot) == ASX_OK, "lookup_root");
    SCENARIO_CHECK(asx_region_slot_lookup(child1, &child1_slot) == ASX_OK, "lookup_child1");
    SCENARIO_CHECK(asx_region_slot_lookup(child2, &child2_slot) == ASX_OK, "lookup_child2");
    SCENARIO_CHECK(asx_region_slot_lookup(grandchild, &grandchild_slot) == ASX_OK,
                   "lookup_grandchild");

    log_region_state("after-open-root", root);
    log_region_state("after-open-child1", child1);
    log_region_state("after-open-child2", child2);
    log_region_state("after-open-grandchild", grandchild);

    SCENARIO_CHECK(root_slot->child_count == 2u, "root_child_count_after_open");
    SCENARIO_CHECK(child1_slot->child_count == 1u, "child1_child_count_after_open");
    SCENARIO_CHECK(child1_slot->parent_id == root, "child1_parent");
    SCENARIO_CHECK(child2_slot->parent_id == root, "child2_parent");
    SCENARIO_CHECK(grandchild_slot->parent_id == child1, "grandchild_parent");

    SCENARIO_CHECK(asx_task_spawn(root, poll_complete, NULL, &root_task) == ASX_OK,
                   "spawn_root_task");
    SCENARIO_CHECK(asx_task_spawn(child1, poll_complete, NULL, &child1_task) == ASX_OK,
                   "spawn_child1_task");
    SCENARIO_CHECK(asx_task_spawn(child2, poll_complete, NULL, &child2_task) == ASX_OK,
                   "spawn_child2_task");
    SCENARIO_CHECK(asx_task_spawn(grandchild, poll_complete, NULL, &grandchild_task) == ASX_OK,
                   "spawn_grandchild_task");

    budget = asx_budget_infinite();
    SCENARIO_CHECK(asx_scheduler_run(grandchild, &budget) == ASX_OK, "run_grandchild_scheduler");

    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_E_PENDING, "root_pending_with_children");
    SCENARIO_CHECK(root_slot->state == ASX_REGION_CLOSING, "root_stays_closing");
    log_region_state("root-pending", root);

    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(grandchild, &budget) == ASX_OK, "drain_grandchild");
    SCENARIO_CHECK(grandchild_slot->state == ASX_REGION_CLOSED, "grandchild_closed");
    SCENARIO_CHECK(grandchild_slot->parent_id == ASX_INVALID_ID, "grandchild_unlinked");
    SCENARIO_CHECK(child1_slot->child_count == 0u, "child1_count_after_grandchild");
    log_region_state("after-grandchild-close", child1);

    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(child1, &budget) == ASX_OK, "drain_child1");
    SCENARIO_CHECK(child1_slot->state == ASX_REGION_CLOSED, "child1_closed");
    SCENARIO_CHECK(root_slot->child_count == 1u, "root_count_after_child1");
    log_region_state("after-child1-close", root);

    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_E_PENDING,
                   "root_pending_with_child2_open");
    SCENARIO_CHECK(root_slot->child_count == 1u, "root_still_has_child2");

    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(child2, &budget) == ASX_OK, "drain_child2");
    SCENARIO_CHECK(child2_slot->state == ASX_REGION_CLOSED, "child2_closed");
    SCENARIO_CHECK(root_slot->child_count == 0u, "root_no_children_remaining");
    log_region_state("after-child2-close", root);

    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_OK, "drain_root");
    SCENARIO_CHECK(root_slot->state == ASX_REGION_CLOSED, "root_closed");
    SCENARIO_CHECK(asx_quiescence_check_detailed(root, &report) == ASX_OK, "root_quiescence");
    log_quiescence_state("root-final", root);
    SCENARIO_CHECK(report.quiescent == 1, "root_not_quiescent");
    SCENARIO_CHECK(report.q1_tasks_complete == 1, "q1_failed");
    SCENARIO_CHECK(report.q2_children_closed == 1, "q2_failed");
    SCENARIO_CHECK(report.q3_obligations_resolved == 1, "q3_failed");
    SCENARIO_CHECK(report.q4_cleanup_drained == 1, "q4_failed");

    SCENARIO_END();
}

static void scenario_nested_inside_out_pending(void) {
    SCENARIO_BEGIN("nested.inside_out_pending");
    asx_region_id root;
    asx_region_id child;
    asx_region_id grandchild;
    asx_region_slot *root_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    SCENARIO_CHECK(asx_region_open(&root) == ASX_OK, "open_root");
    SCENARIO_CHECK(asx_region_open_child(root, &child) == ASX_OK, "open_child");
    SCENARIO_CHECK(asx_region_open_child(child, &grandchild) == ASX_OK, "open_grandchild");
    SCENARIO_CHECK(asx_region_slot_lookup(root, &root_slot) == ASX_OK, "lookup_root");
    SCENARIO_CHECK(asx_region_slot_lookup(child, &child_slot) == ASX_OK, "lookup_child");

    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(child, &budget) == ASX_E_PENDING, "child_pending");
    SCENARIO_CHECK(child_slot->state == ASX_REGION_CLOSING, "child_closing");
    SCENARIO_CHECK(child_slot->child_count == 1u, "child_still_has_grandchild");
    log_region_state("child-pending", child);

    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(grandchild, &budget) == ASX_OK, "drain_grandchild");
    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(child, &budget) == ASX_OK, "drain_child");
    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_OK, "drain_root");
    SCENARIO_CHECK(root_slot->child_count == 0u, "root_has_no_children");

    SCENARIO_END();
}

static void scenario_nested_q2_evidence(void) {
    SCENARIO_BEGIN("nested.q2_evidence");
    asx_region_id root;
    asx_region_id child;
    asx_quiescence_report report;
    asx_budget budget;

    asx_runtime_reset();

    SCENARIO_CHECK(asx_region_open(&root) == ASX_OK, "open_root");
    SCENARIO_CHECK(asx_region_open_child(root, &child) == ASX_OK, "open_child");
    SCENARIO_CHECK(asx_quiescence_check_detailed(root, &report) == ASX_OK, "report_open_child");
    log_quiescence_state("root-with-open-child", root);
    SCENARIO_CHECK(report.q1_tasks_complete == 1, "q1_with_open_child");
    SCENARIO_CHECK(report.q2_children_closed == 0, "q2_should_fail");
    SCENARIO_CHECK(report.q3_obligations_resolved == 1, "q3_with_open_child");
    SCENARIO_CHECK(report.q4_cleanup_drained == 1, "q4_with_open_child");
    SCENARIO_CHECK(report.quiescent == 0, "root_should_not_be_quiescent");

    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(child, &budget) == ASX_OK, "drain_child");
    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_OK, "drain_root");
    SCENARIO_CHECK(asx_quiescence_check_detailed(root, &report) == ASX_OK, "report_closed_root");
    log_quiescence_state("root-after-close", root);
    SCENARIO_CHECK(report.q2_children_closed == 1, "q2_should_pass");
    SCENARIO_CHECK(report.quiescent == 1, "root_should_be_quiescent");

    SCENARIO_END();
}

int main(void) {
    scenario_nested_lifecycle_full();
    scenario_nested_inside_out_pending();
    scenario_nested_q2_evidence();

    printf("DIGEST %016llx\n", (unsigned long long)asx_trace_digest());
    fprintf(stderr, "[e2e] nested_regions: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
