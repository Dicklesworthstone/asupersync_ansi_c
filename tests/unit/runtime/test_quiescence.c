/*
 * test_quiescence.c — focused unit tests for quiescence.c
 *
 * Exercises branch-level quiescence/finalization behavior that broader
 * lifecycle suites only cover indirectly.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/runtime/trace.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

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

static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

static void cleanup_mark(void *ctx) {
    int *flag = (int *)ctx;
    *flag += 1;
}

typedef struct {
    asx_region_id region;
    asx_status spawn_status;
    asx_task_id task_id;
} cleanup_spawn_task_ctx;

static void cleanup_spawn_immediate_task(void *ctx) {
    cleanup_spawn_task_ctx *spawn = (cleanup_spawn_task_ctx *)ctx;

    if (spawn == NULL) return;
    spawn->task_id = ASX_INVALID_ID;
    spawn->spawn_status = asx_task_spawn(spawn->region, poll_complete, NULL, &spawn->task_id);
}

static void log_child_summary(const char *label, asx_region_id parent,
                              asx_region_slot *parent_slot) {
    fprintf(stderr, "[child-test] %s parent=0x%016llx child_count=%u state=%d\n", label,
            (unsigned long long)parent, parent_slot->child_count, (int)parent_slot->state);
}

static void log_child_link(const char *label, asx_region_id region, asx_region_slot *region_slot,
                           asx_region_id expected_parent) {
    fprintf(stderr, "[child-test] %s region=0x%016llx parent=0x%016llx state=%d\n", label,
            (unsigned long long)region, (unsigned long long)expected_parent,
            (int)region_slot->state);
}

TEST(quiescence_closed_region_with_live_tasks_reports_tasks_live) {
    asx_region_id rid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 1;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_TASKS_LIVE);
}

TEST(quiescence_closed_region_with_unresolved_obligation_reports_error) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_OBLIGATIONS_UNRESOLVED);
}

TEST(region_drain_advances_draining_region_to_closed_and_runs_cleanup) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    asx_budget budget;
    asx_trace_event ev;
    uint32_t event_count;
    int cleaned = 0;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_DRAINING;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(cleaned, 1);
    event_count = asx_trace_event_count();
    ASSERT_TRUE(event_count > 0u);
    ASSERT_TRUE(asx_trace_event_get(event_count - 1u, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_CLOSED);
    ASSERT_EQ(ev.entity_id, rid);
}

TEST(region_drain_recancels_uncancelled_tasks_before_scheduler_run) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_RUNNING);

    budget = asx_budget_from_polls(16);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(region_drain_budget_exhaustion_leaves_region_closing) {
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    budget = asx_budget_from_polls(0);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(region->state, ASX_REGION_CLOSING);
    ASSERT_TRUE(region->task_count > 0);
}

TEST(region_drain_finalizer_spawned_task_requires_followup_drain) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    cleanup_spawn_task_ctx spawn_ctx;
    asx_task_state task_state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    spawn_ctx.region = rid;
    spawn_ctx.spawn_status = ASX_E_INVALID_STATE;
    spawn_ctx.task_id = ASX_INVALID_ID;
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_spawn_immediate_task, &spawn_ctx, &handle),
              ASX_OK);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(spawn_ctx.spawn_status, ASX_OK);
    ASSERT_NE(spawn_ctx.task_id, ASX_INVALID_ID);
    ASSERT_EQ(region->state, ASX_REGION_FINALIZING);
    ASSERT_EQ(region->task_count, 1u);
    ASSERT_EQ(asx_task_get_state(spawn_ctx.task_id, &task_state), ASX_OK);
    ASSERT_EQ(task_state, ASX_TASK_CREATED);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(spawn_ctx.task_id, &task_state), ASX_OK);
    ASSERT_EQ(task_state, ASX_TASK_COMPLETED);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(finalizing_region_allows_cleanup_task_spawn) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_task_id tid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(region->task_count, 1u);
}

TEST(finalizing_region_allows_captured_cleanup_task_spawn) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_task_id tid;
    uint32_t *captured = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_task_spawn_captured(rid, poll_complete, sizeof(*captured), NULL, &tid,
                                      (void **)&captured),
              ASX_OK);
    ASSERT_TRUE(captured != NULL);
    ASSERT_EQ(*captured, 0u);
    ASSERT_EQ(region->task_count, 1u);
}

TEST(finalizing_region_still_rejects_obligation_reserve) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_obligation_id oid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_NOT_OPEN);
}

TEST(region_open_child_null_out_fails) {
    asx_region_id parent;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(region_open_child_invalid_parent_returns_not_found) {
    asx_region_id child;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open_child(ASX_INVALID_ID, &child), ASX_E_NOT_FOUND);
}

TEST(region_open_child_rejects_non_open_parent) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->state = ASX_REGION_CLOSING;

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_NOT_OPEN);
}

TEST(region_open_child_rejects_closed_parent) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->state = ASX_REGION_CLOSED;
    log_child_summary("closed-parent", parent, parent_slot);

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_NOT_OPEN);
}

TEST(region_open_child_rejects_poisoned_parent) {
    asx_region_id parent;
    asx_region_id child;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_poison(parent), ASX_OK);

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_POISONED);
}

TEST(region_open_child_rejects_full_child_list) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->child_count = ASX_MAX_REGION_CHILDREN;

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(region_open_child_sets_parent_and_tracks_child) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(child_slot->state, ASX_REGION_OPEN);
}

TEST(region_open_multiple_children_tracks_all_children) {
    asx_region_id parent;
    asx_region_id child1;
    asx_region_id child2;
    asx_region_id child3;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child1), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child2), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child3), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    log_child_summary("multi-open", parent, parent_slot);

    ASSERT_EQ(parent_slot->child_count, 3u);
    ASSERT_EQ(parent_slot->children[0], child1);
    ASSERT_EQ(parent_slot->children[1], child2);
    ASSERT_EQ(parent_slot->children[2], child3);

    ASSERT_EQ(asx_region_slot_lookup(child1, &child_slot), ASX_OK);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(asx_region_slot_lookup(child2, &child_slot), ASX_OK);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(asx_region_slot_lookup(child3, &child_slot), ASX_OK);
    ASSERT_EQ(child_slot->parent_id, parent);
}

TEST(region_open_child_real_capacity_exhaustion) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    uint32_t i;
    uint32_t opened = 0u;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    for (i = 0; i < ASX_MAX_REGION_CHILDREN; i++) {
        if (asx_region_open_child(parent, &child) != ASX_OK) break;
        opened++;
    }
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    log_child_summary("real-max", parent, parent_slot);
    ASSERT_EQ(opened, (uint32_t)(ASX_MAX_REGIONS - 1));
    ASSERT_EQ(parent_slot->child_count, opened);

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(region_drain_closed_child_unlinks_from_parent) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    child_slot->state = ASX_REGION_FINALIZING;
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);

    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, ASX_INVALID_ID);
    ASSERT_EQ(parent_slot->child_count, 0u);
    ASSERT_EQ(parent_slot->children[0], ASX_INVALID_ID);
}

TEST(region_drain_closed_child_tolerates_missing_parent_slot) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    parent_slot->alive = 0;
    child_slot->state = ASX_REGION_FINALIZING;
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);

    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, ASX_INVALID_ID);
    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
}

TEST(region_drain_parent_waits_for_open_child_to_close) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_E_PENDING);
    ASSERT_EQ(parent_slot->state, ASX_REGION_CLOSING);
    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
    ASSERT_EQ(child_slot->state, ASX_REGION_OPEN);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(parent_slot->child_count, 0u);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(parent_slot->state, ASX_REGION_CLOSED);
}

TEST(region_drain_nested_grandchild_closes_inside_out) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_id grandchild;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_region_slot *grandchild_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_open_child(child, &grandchild), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(grandchild, &grandchild_slot), ASX_OK);
    log_child_summary("nested-parent-before", parent, parent_slot);
    log_child_summary("nested-child-before", child, child_slot);
    log_child_link("nested-child-link", child, child_slot, parent);
    log_child_link("nested-grandchild-link", grandchild, grandchild_slot, child);

    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(grandchild_slot->parent_id, child);
    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(child_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
    ASSERT_EQ(child_slot->children[0], grandchild);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(grandchild, &budget), ASX_OK);
    log_child_summary("nested-child-after-grandchild", child, child_slot);
    ASSERT_EQ(grandchild_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(grandchild_slot->parent_id, ASX_INVALID_ID);
    ASSERT_EQ(child_slot->child_count, 0u);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    log_child_summary("nested-parent-after-child", parent, parent_slot);
    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, ASX_INVALID_ID);
    ASSERT_EQ(parent_slot->child_count, 0u);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(parent_slot->state, ASX_REGION_CLOSED);
}

/* --- New tests covering additional branches --- */

TEST(quiescence_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;

    asx_runtime_reset();

    ASSERT_EQ(asx_quiescence_check(bad), ASX_E_NOT_FOUND);
}

TEST(quiescence_open_region_returns_not_reached) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_NOT_REACHED);
}

TEST(quiescence_success_after_drain_no_tasks) {
    asx_region_id rid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_success_with_committed_obligation) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_success_with_aborted_obligation) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_closed_region_with_pending_cleanup_returns_not_reached) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, NULL, &handle), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_NOT_REACHED);
}

TEST(drain_null_budget_returns_invalid_argument) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_drain(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(drain_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;
    asx_budget budget;

    asx_runtime_reset();

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(bad, &budget), ASX_E_NOT_FOUND);
}

TEST(drain_multiple_tasks_all_complete) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t3), ASX_OK);

    /* Prime all tasks to RUNNING state */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(t1, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t2, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t3, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(drain_cleanup_lifo_multiple) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h1 = ASX_CLEANUP_INVALID;
    asx_cleanup_handle h2 = ASX_CLEANUP_INVALID;
    asx_cleanup_handle h3 = ASX_CLEANUP_INVALID;
    asx_budget budget;
    int c1 = 0, c2 = 0, c3 = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_DRAINING;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c1, &h1), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c2, &h2), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c3, &h3), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(c1, 1);
    ASSERT_EQ(c2, 1);
    ASSERT_EQ(c3, 1);
}

/* --- Drain progress snapshot tests (bd-1eqo.5.2) --- */

TEST(drain_progress_null_out_returns_invalid_argument) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_drain_progress(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(drain_progress_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_drain_progress(bad, &prog), ASX_E_NOT_FOUND);
}

TEST(drain_progress_open_region_with_tasks) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t2), ASX_OK);

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.region_state, ASX_REGION_OPEN);
    ASSERT_EQ(prog.tasks_total, 2u);
    ASSERT_EQ(prog.tasks_live, 2u);
    ASSERT_EQ(prog.tasks_completed, 0u);
    ASSERT_EQ(prog.tasks_cancelled, 0u);
    ASSERT_EQ(prog.obligations_total, 0u);
    ASSERT_EQ(prog.poisoned, 0);
}

TEST(drain_progress_with_obligations) {
    asx_region_id rid;
    asx_obligation_id o1, o2, o3;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o1), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o2), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o3), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(o1), ASX_OK);

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.obligations_total, 3u);
    ASSERT_EQ(prog.obligations_reserved, 2u);
    ASSERT_EQ(prog.obligations_resolved, 1u);
}

TEST(drain_progress_after_partial_drain) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t2), ASX_OK);

    /* Prime both tasks */
    budget = asx_budget_from_polls(2);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Start drain — t1 will complete (checkpoint-aware), t2 stays pending */
    budget = asx_budget_from_polls(8);
    {
        asx_status drain_st = asx_region_drain(rid, &budget);
        (void)drain_st;
    }

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    /* t1 completed, t2 still live with cancel_pending */
    ASSERT_TRUE(prog.tasks_completed > 0u);
    ASSERT_TRUE(prog.tasks_total >= prog.tasks_live + prog.tasks_completed);
}

TEST(drain_progress_cleanup_drained_flag) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h = ASX_CLEANUP_INVALID;
    asx_drain_progress prog;
    int cleaned = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &h), ASX_OK);

    /* Before drain: cleanup not drained */
    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.cleanup_drained, 0);

    /* Pop cleanup manually */
    asx_cleanup_drain(&region->cleanup);

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.cleanup_drained, 1);
    ASSERT_EQ(cleaned, 1);
}

/* --- Quiescence check detailed tests (bd-1eqo.5.2) --- */

TEST(quiescence_detailed_null_out_returns_invalid_argument) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_quiescence_check_detailed(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(quiescence_detailed_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_quiescence_check_detailed(bad, &report), ASX_E_NOT_FOUND);
}

TEST(region_is_quiescent_invalid_handle_returns_false) {
    asx_runtime_reset();
    ASSERT_FALSE(asx_region_is_quiescent(0xDEADBEEFu));
}

TEST(quiescence_detailed_fully_quiescent) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 1);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q2_children_closed, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_EQ(report.region_state, ASX_REGION_CLOSED);
    ASSERT_TRUE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_live_tasks_q1_fails) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 3;
    asx_cleanup_init(&region->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 0);
    ASSERT_EQ(report.q2_children_closed, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_open_child_q2_fails) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->state = ASX_REGION_CLOSED;
    parent_slot->task_count = 0;
    asx_cleanup_init(&parent_slot->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(parent, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q2_children_closed, 0);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_FALSE(asx_region_is_quiescent(parent));
}

TEST(quiescence_detailed_all_children_closed_q2_passes) {
    asx_region_id parent;
    asx_region_id child1;
    asx_region_id child2;
    asx_region_slot *parent_slot = NULL;
    asx_quiescence_report report;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child1), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child2), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    log_child_summary("q2-before-close", parent, parent_slot);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child1, &budget), ASX_OK);
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child2, &budget), ASX_OK);

    parent_slot->state = ASX_REGION_CLOSED;
    parent_slot->task_count = 0;
    asx_cleanup_init(&parent_slot->cleanup);
    log_child_summary("q2-after-close", parent, parent_slot);

    ASSERT_EQ(asx_quiescence_check_detailed(parent, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 1);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q2_children_closed, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_TRUE(asx_region_is_quiescent(parent));
}

TEST(quiescence_detailed_unresolved_obligations_q3_fails) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 0);
    ASSERT_EQ(report.progress.obligations_reserved, 1u);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_undrained_cleanup_q4_fails) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h = ASX_CLEANUP_INVALID;
    asx_quiescence_report report;
    int cleaned = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &h), ASX_OK);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 0);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_not_closed_not_quiescent) {
    asx_region_id rid;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.region_state, ASX_REGION_OPEN);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(drain_progress_monotonicity_during_drain) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_drain_progress before, after;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t3), ASX_OK);

    /* Prime tasks */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_region_drain_progress(rid, &before), ASX_OK);

    /* Full drain */
    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_region_drain_progress(rid, &after), ASX_OK);

    /* Monotonicity: tasks_live decreased, tasks_completed increased */
    ASSERT_TRUE(after.tasks_live <= before.tasks_live);
    ASSERT_TRUE(after.tasks_completed >= before.tasks_completed);
}

TEST(drain_already_closed_is_idempotent) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
}

TEST(drain_full_lifecycle_tasks_obligations_cleanup) {
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;
    asx_budget budget;
    int cleaned = 0;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);

    /* Prime task to RUNNING */
    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Full drain */
    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(cleaned, 1);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

int main(void) {
    fprintf(stderr, "=== test_quiescence ===\n");

    asx_runtime_reset();
    RUN_TEST(quiescence_closed_region_with_live_tasks_reports_tasks_live);
    asx_runtime_reset();
    RUN_TEST(quiescence_closed_region_with_unresolved_obligation_reports_error);
    asx_runtime_reset();
    RUN_TEST(region_drain_advances_draining_region_to_closed_and_runs_cleanup);
    asx_runtime_reset();
    RUN_TEST(region_drain_recancels_uncancelled_tasks_before_scheduler_run);
    asx_runtime_reset();
    RUN_TEST(region_drain_budget_exhaustion_leaves_region_closing);
    asx_runtime_reset();
    RUN_TEST(region_drain_finalizer_spawned_task_requires_followup_drain);
    asx_runtime_reset();
    RUN_TEST(finalizing_region_allows_cleanup_task_spawn);
    asx_runtime_reset();
    RUN_TEST(finalizing_region_allows_captured_cleanup_task_spawn);
    asx_runtime_reset();
    RUN_TEST(finalizing_region_still_rejects_obligation_reserve);
    asx_runtime_reset();
    RUN_TEST(region_open_child_null_out_fails);
    asx_runtime_reset();
    RUN_TEST(region_open_child_invalid_parent_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_non_open_parent);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_closed_parent);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_poisoned_parent);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_full_child_list);
    asx_runtime_reset();
    RUN_TEST(region_open_child_sets_parent_and_tracks_child);
    asx_runtime_reset();
    RUN_TEST(region_open_multiple_children_tracks_all_children);
    asx_runtime_reset();
    RUN_TEST(region_open_child_real_capacity_exhaustion);
    asx_runtime_reset();
    RUN_TEST(region_drain_closed_child_unlinks_from_parent);
    asx_runtime_reset();
    RUN_TEST(region_drain_closed_child_tolerates_missing_parent_slot);
    asx_runtime_reset();
    RUN_TEST(region_drain_parent_waits_for_open_child_to_close);
    asx_runtime_reset();
    RUN_TEST(region_drain_nested_grandchild_closes_inside_out);
    asx_runtime_reset();
    RUN_TEST(quiescence_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(quiescence_open_region_returns_not_reached);
    asx_runtime_reset();
    RUN_TEST(quiescence_success_after_drain_no_tasks);
    asx_runtime_reset();
    RUN_TEST(quiescence_success_with_committed_obligation);
    asx_runtime_reset();
    RUN_TEST(quiescence_success_with_aborted_obligation);
    asx_runtime_reset();
    RUN_TEST(quiescence_closed_region_with_pending_cleanup_returns_not_reached);
    asx_runtime_reset();
    RUN_TEST(drain_null_budget_returns_invalid_argument);
    asx_runtime_reset();
    RUN_TEST(drain_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(drain_multiple_tasks_all_complete);
    asx_runtime_reset();
    RUN_TEST(drain_cleanup_lifo_multiple);
    asx_runtime_reset();
    RUN_TEST(drain_progress_null_out_returns_invalid_argument);
    asx_runtime_reset();
    RUN_TEST(drain_progress_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(drain_progress_open_region_with_tasks);
    asx_runtime_reset();
    RUN_TEST(drain_progress_with_obligations);
    asx_runtime_reset();
    RUN_TEST(drain_progress_after_partial_drain);
    asx_runtime_reset();
    RUN_TEST(drain_progress_cleanup_drained_flag);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_null_out_returns_invalid_argument);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(region_is_quiescent_invalid_handle_returns_false);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_fully_quiescent);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_live_tasks_q1_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_open_child_q2_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_all_children_closed_q2_passes);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_unresolved_obligations_q3_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_undrained_cleanup_q4_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_not_closed_not_quiescent);
    asx_runtime_reset();
    RUN_TEST(drain_progress_monotonicity_during_drain);
    asx_runtime_reset();
    RUN_TEST(drain_already_closed_is_idempotent);
    asx_runtime_reset();
    RUN_TEST(drain_full_lifecycle_tasks_obligations_cleanup);

    TEST_REPORT();
    return test_failures;
}
