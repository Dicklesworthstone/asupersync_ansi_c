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
#include <asx/runtime/rt.h>
#include <asx/runtime/trace.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* A fresh runtime with the opt-in hard cleanup bound
 * (asx_runtime_config.cleanup_hard_bound): draining a tree of tasks that
 * never finish relies on it (Rust never force-completes). */
static asx_status reset_with_hard_cleanup_bound(void) {
    static asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    cfg.cleanup_hard_bound = 1u;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    return st;
}

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
    spawn->spawn_status =
        asx_task_spawn_cleanup(spawn->region, poll_complete, NULL, &spawn->task_id);
}

typedef struct {
    int *polls;
    int *drops;
} captured_cleanup_state;

typedef struct {
    cleanup_spawn_task_ctx spawn;
    int polls;
    int drops;
} captured_cleanup_ctx;

static asx_status poll_captured_cleanup(void *data, asx_task_id self) {
    captured_cleanup_state *state = (captured_cleanup_state *)data;
    (void)self;
    (*state->polls)++;
    return ASX_OK;
}

static void drop_captured_cleanup(void *data, uint32_t size) {
    captured_cleanup_state *state = (captured_cleanup_state *)data;
    if (size == sizeof(*state)) (*state->drops)++;
}

static void finalizer_spawn_captured_cleanup(void *data) {
    captured_cleanup_ctx *ctx = (captured_cleanup_ctx *)data;
    captured_cleanup_state *state = NULL;
    ctx->spawn.spawn_status =
        asx_task_spawn_cleanup_captured(ctx->spawn.region, poll_captured_cleanup, sizeof(*state),
                                        drop_captured_cleanup, &ctx->spawn.task_id,
                                        (void **)&state);
    if (ctx->spawn.spawn_status == ASX_OK) {
        state->polls = &ctx->polls;
        state->drops = &ctx->drops;
    }
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
    asx_outcome outcome;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    spawn_ctx.region = rid;
    spawn_ctx.spawn_status = ASX_E_INVALID_STATE;
    spawn_ctx.task_id = ASX_INVALID_ID;
    ASSERT_EQ(asx_region_register_finalizer(rid, cleanup_spawn_immediate_task, &spawn_ctx, &handle),
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
    ASSERT_EQ(asx_task_get_outcome(spawn_ctx.task_id, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
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

    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_E_REGION_CLOSED);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_OK);
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

    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, sizeof(*captured), NULL, &tid,
                                              (void **)&captured),
              ASX_OK);
    ASSERT_TRUE(captured != NULL);
    ASSERT_EQ(*captured, 0u);
    ASSERT_EQ(region->task_count, 1u);
}

TEST(public_finalizer_captured_cleanup_executes_and_drops_once) {
    captured_cleanup_ctx ctx;
    asx_cleanup_handle handle;
    asx_budget budget;
    asx_outcome outcome;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&ctx.spawn.region), ASX_OK);
    ASSERT_EQ(asx_region_register_finalizer(ctx.spawn.region, finalizer_spawn_captured_cleanup,
                                            &ctx, &handle),
              ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(ctx.spawn.region, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(ctx.spawn.spawn_status, ASX_OK);
    ASSERT_EQ(ctx.polls, 0);
    ASSERT_EQ(ctx.drops, 0);
    ASSERT_EQ(asx_region_drain(ctx.spawn.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.drops, 1);
    ASSERT_EQ(asx_task_get_outcome(ctx.spawn.task_id, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
    ASSERT_EQ(asx_quiescence_check(ctx.spawn.region), ASX_OK);
    ASSERT_EQ(asx_region_drain(ctx.spawn.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.drops, 1);
    asx_runtime_reset();
    ASSERT_EQ(ctx.drops, 1);
}

TEST(finalizing_region_still_rejects_obligation_reserve) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_obligation_id oid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_CLOSED);
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

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_CLOSED);
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

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_CLOSED);
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
    /* Whichever runs out first: the region arena (the parent holds one
     * slot) or the parent's child slots (a classed build can have more
     * regions than child slots). */
    ASSERT_EQ(opened, (uint32_t)(ASX_MAX_REGIONS - 1) < (uint32_t)ASX_MAX_REGION_CHILDREN
                          ? (uint32_t)(ASX_MAX_REGIONS - 1)
                          : (uint32_t)ASX_MAX_REGION_CHILDREN);
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
    /* Unlinked from the parent's children; it still names its parent,
     * which keeps its slot from being recycled while the parent lives. */
    ASSERT_EQ(child_slot->parent_id, parent);
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
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
}

TEST(region_drain_parent_closes_open_child_first) {
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

    /* Draining the parent is a structured shutdown of its subtree: the
     * open child is closed and finalized first, then the parent. */
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(parent_slot->child_count, 0u);
    ASSERT_EQ(parent_slot->state, ASX_REGION_CLOSED);
}

static asx_status q_poll_pending(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

TEST(region_drain_cancels_descendant_tasks_with_parent_attribution) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_id grandchild;
    asx_task_id t_child;
    asx_task_id t_grand;
    asx_outcome out;
    asx_budget budget;

    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_open_child(child, &grandchild), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, q_poll_pending, NULL, &t_child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(grandchild, q_poll_pending, NULL, &t_grand), ASX_OK);

    /* Never-finishing tasks deep in the tree are cancelled and, under the
     * hard bound, force-completed within their cleanup budget; every
     * region closes. */
    budget = asx_budget_from_polls(10000);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(t_child, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_outcome(t_grand, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_region_is_quiescent(parent), 1);
}

/* A closed child of a live parent keeps its slot while others are free, so
 * the handle its opener holds still names a closed region (Rust refuses a
 * spawn into a recently closed region as RegionClosed; fuzz gen-26-180);
 * only a full arena recycles it. */
TEST(closed_child_of_live_parent_keeps_its_slot) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_id other;
    asx_region_state state;
    asx_task_id tid;
    asx_budget budget;
    uint32_t i;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&other), ASX_OK);

    ASSERT_EQ(asx_region_get_state(child, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_spawn(child, q_poll_pending, NULL, &tid), ASX_E_REGION_CLOSED);

    /* Fill the arena: the closed child is recycled only when no other slot
     * is left, and its handle then goes stale. */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        asx_region_id r;
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        if (asx_region_get_state(child, &state) == ASX_E_STALE_HANDLE) break;
    }
    ASSERT_TRUE(i < ASX_MAX_REGIONS);
    ASSERT_EQ(asx_task_spawn(child, q_poll_pending, NULL, &tid), ASX_E_STALE_HANDLE);
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
    ASSERT_EQ(grandchild_slot->parent_id, child);
    ASSERT_EQ(child_slot->child_count, 0u);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    log_child_summary("nested-parent-after-child", parent, parent_slot);
    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, parent);
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

/* Admission expectations are the pinned Rust RegionState predicates
 * (record/region.rs:140-151). State injection exercises transient phases;
 * the finalizer tests below reach FINALIZING through the public close path. */
TEST(task_spawn_admission_matrix) {
    unsigned state;
    unsigned cleanup;
    unsigned captured;
    for (state = ASX_REGION_OPEN; state <= ASX_REGION_CLOSED; state++) {
        for (cleanup = 0u; cleanup < 2u; cleanup++) {
            for (captured = 0u; captured < 2u; captured++) {
                asx_region_id rid;
                asx_region_slot *r;
                asx_task_id tid = ASX_INVALID_ID;
                void *data = NULL;
                asx_status st;
                int accepts =
                    state == ASX_REGION_OPEN || (cleanup && state == ASX_REGION_FINALIZING);
                asx_runtime_reset();
                ASSERT_EQ(asx_region_open(&rid), ASX_OK);
                ASSERT_EQ(asx_region_slot_lookup(rid, &r), ASX_OK);
                r->state = (asx_region_state)state;
                if (captured) {
                    st = cleanup
                             ? asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid,
                                                               &data)
                             : asx_task_spawn_captured(rid, poll_complete, 4u, NULL, &tid, &data);
                } else {
                    st = cleanup ? asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid)
                                 : asx_task_spawn(rid, poll_complete, NULL, &tid);
                }
                ASSERT_EQ(st, accepts ? ASX_OK : ASX_E_REGION_CLOSED);
                ASSERT_EQ(r->task_count, accepts ? 1u : 0u);
                ASSERT_EQ(r->capture_used, accepts && captured ? 8u : 0u);
                if (!accepts) {
                    ASSERT_EQ(tid, ASX_INVALID_ID);
                    ASSERT_TRUE(data == NULL);
                }
            }
        }
    }
}

TEST(cleanup_spawn_limits_and_failure_atomicity) {
    asx_region_id rid;
    asx_region_slot *r;
    asx_region_limits limits;
    asx_task_id tid = ASX_INVALID_ID;
    void *data = NULL;
    uint32_t i;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &r), ASX_OK);
    ASSERT_EQ(asx_region_get_limits(rid, &limits), ASX_OK);
    limits.max_tasks = 0u;
    ASSERT_EQ(asx_region_set_limits(rid, &limits), ASX_OK);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, &data),
              ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(r->capture_used, 0u);
    ASSERT_EQ(r->task_count, 0u);
    ASSERT_EQ(tid, ASX_INVALID_ID);
    ASSERT_TRUE(data == NULL);

    r->state = ASX_REGION_FINALIZING;
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete,
                                              ASX_REGION_CAPTURE_ARENA_BYTES + 1u, NULL, &tid,
                                              &data),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(r->capture_used, 0u);
    for (i = 0u; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_OK);
    }
    tid = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, &data),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(r->task_count, ASX_MAX_TASKS);
    ASSERT_EQ(r->capture_used, 0u);
    ASSERT_EQ(tid, ASX_INVALID_ID);
    ASSERT_TRUE(data == NULL);
}

TEST(cleanup_spawn_invalid_arguments_and_poison) {
    asx_region_id rid;
    asx_task_id tid = ASX_INVALID_ID;
    void *data = NULL;
    asx_runtime_reset();
    ASSERT_EQ(asx_task_spawn_cleanup(ASX_INVALID_ID, poll_complete, NULL, &tid), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, NULL, NULL, &tid), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 0u, NULL, &tid, &data),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_E_REGION_POISONED);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, &data),
              ASX_E_REGION_POISONED);
    ASSERT_EQ(tid, ASX_INVALID_ID);
    ASSERT_TRUE(data == NULL);
}

typedef struct {
    asx_region_id region;
    asx_region_id other;
    asx_obligation_id obligation;
    asx_status commit;
    asx_status recursive_drain;
    asx_status other_drain;
    asx_region_state after_commit;
    int calls;
} public_finalizer_ctx;

static void finalizer_resolve_and_drain(void *data) {
    public_finalizer_ctx *ctx = (public_finalizer_ctx *)data;
    asx_budget budget = asx_budget_from_polls(8u);
    ctx->calls++;
    ctx->commit = asx_obligation_commit(ctx->obligation);
    if (asx_region_get_state(ctx->region, &ctx->after_commit) != ASX_OK) {
        ctx->after_commit = ASX_REGION_OPEN;
    }
    ctx->recursive_drain = asx_region_drain(ctx->region, &budget);
    ctx->other_drain = asx_region_drain(ctx->other, &budget);
}

TEST(public_finalizer_resolves_obligation_before_close_and_handles_reentrancy) {
    public_finalizer_ctx ctx;
    asx_region_id parent;
    asx_region_id child;
    asx_cleanup_handle handle;
    asx_budget budget;
    asx_region_state state;
    int remaining_calls = 0;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&ctx.region), ASX_OK);
    parent = ctx.region;
    ASSERT_EQ(asx_region_open_child(ctx.region, &child), ASX_OK);
    ASSERT_EQ(asx_region_open(&ctx.other), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(child, &ctx.obligation), ASX_OK);
    ctx.region = child;
    ASSERT_EQ(asx_region_register_finalizer(child, cleanup_mark, &remaining_calls, &handle),
              ASX_OK);
    ASSERT_EQ(asx_region_register_finalizer(child, finalizer_resolve_and_drain, &ctx, &handle),
              ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(ctx.commit, ASX_OK);
    ASSERT_EQ(ctx.after_commit, ASX_REGION_FINALIZING);
    ASSERT_EQ(ctx.recursive_drain, ASX_E_PENDING);
    ASSERT_EQ(ctx.other_drain, ASX_OK);
    ASSERT_EQ(ctx.calls, 1);
    ASSERT_EQ(remaining_calls, 1);
    ASSERT_EQ(asx_region_get_state(parent, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_quiescence_check(ctx.region), ASX_OK);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(ctx.calls, 1);
}

TEST(public_finalizer_capacity_unregistration_and_errors) {
    asx_region_id rid;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    asx_cleanup_handle old_handle;
    asx_budget budget;
    uint32_t i;
    int calls = 0;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_register_finalizer(ASX_INVALID_ID, cleanup_mark, &calls, &handle),
              ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_register_finalizer(rid, NULL, &calls, &handle), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_register_finalizer(rid, cleanup_mark, &calls, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_register_finalizer(rid, cleanup_mark, &calls, &old_handle), ASX_OK);
    ASSERT_EQ(asx_region_unregister_finalizer(rid, old_handle), ASX_OK);
    for (i = 0u; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_region_register_finalizer(rid, cleanup_mark, &calls, &handle), ASX_OK);
    }
    ASSERT_EQ(asx_region_unregister_finalizer(rid, old_handle), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_register_finalizer(rid, cleanup_mark, &calls, &handle),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_region_unregister_finalizer(rid, handle), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(calls, ASX_CLEANUP_STACK_CAPACITY - 1u);
    ASSERT_EQ(asx_region_register_finalizer(rid, cleanup_mark, &calls, &handle),
              ASX_E_REGION_CLOSED);
    ASSERT_EQ(asx_region_unregister_finalizer(rid, handle), ASX_E_NOT_FOUND);
}

typedef struct {
    asx_region_id other;
    asx_cancel_reason reason;
    asx_status status;
    int calls;
} finalizer_cancel_ctx;

static void finalizer_cancel_other_tree(void *data) {
    finalizer_cancel_ctx *ctx = (finalizer_cancel_ctx *)data;
    ctx->calls++;
    ctx->status = asx_region_cancel(ctx->other, &ctx->reason, NULL);
}

TEST(public_finalizer_nested_cancel_preserves_outer_subtree) {
    finalizer_cancel_ctx ctx;
    asx_region_id parent;
    asx_region_id first;
    asx_region_id second;
    asx_region_id other_children[2];
    asx_cleanup_handle handle;
    uint32_t children = 0u;
    uint32_t i;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &first), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &second), ASX_OK);
    ASSERT_EQ(asx_region_open(&ctx.other), ASX_OK);
    /* R1 has four region slots. Exercise nested cancellation there too;
     * with six available slots, the second tree overwrites every entry of
     * the original three-region propagation scratch in the old code. */
    for (i = 4u; i < ASX_MAX_REGIONS && children < 2u; i++) {
        ASSERT_EQ(asx_region_open_child(ctx.other, &other_children[children]), ASX_OK);
        children++;
    }
    ctx.reason.kind = ASX_CANCEL_USER;
    ctx.reason.origin_region = parent;
    ctx.reason.origin_task = ASX_INVALID_ID;
    /* Cancel visits the last child first; its callback must not replace
     * the outer walk before the older sibling has been finalized. */
    ASSERT_EQ(asx_region_register_finalizer(second, finalizer_cancel_other_tree, &ctx, &handle),
              ASX_OK);
    ASSERT_EQ(asx_region_cancel(parent, &ctx.reason, NULL), ASX_OK);
    ASSERT_EQ(ctx.calls, 1);
    ASSERT_EQ(ctx.status, ASX_OK);
    ASSERT_EQ(asx_quiescence_check(first), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(second), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(parent), ASX_OK);
    for (i = 0u; i < children; i++) { ASSERT_EQ(asx_quiescence_check(other_children[i]), ASX_OK); }
    ASSERT_EQ(asx_quiescence_check(ctx.other), ASX_OK);
}

#ifndef ASX_DEBUG_QUARANTINE
typedef struct {
    asx_region_id replacement;
    asx_task_id task;
    asx_task_id first_task;
    asx_status open_status;
    asx_status spawn_status;
    asx_status join_status;
    int polls;
} finalizer_recycle_ctx;

static asx_status poll_replacement_region(void *data, asx_task_id self) {
    finalizer_recycle_ctx *ctx = (finalizer_recycle_ctx *)data;
    (void)self;
    ctx->polls++;
    return ASX_OK;
}

static void finalizer_recycle_closed_sibling(void *data) {
    finalizer_recycle_ctx *ctx = (finalizer_recycle_ctx *)data;
    asx_outcome outcome;
    ctx->join_status = asx_task_join(ctx->first_task, &outcome);
    ctx->open_status = asx_region_open(&ctx->replacement);
    if (ctx->open_status == ASX_OK) {
        ctx->spawn_status =
            asx_task_spawn(ctx->replacement, poll_replacement_region, ctx, &ctx->task);
    }
}

TEST(public_finalizer_recycled_sibling_does_not_extend_outer_drain) {
    finalizer_recycle_ctx ctx;
    asx_region_id parent, first, second, extra;
    asx_task_id second_task;
    asx_cleanup_handle handle;
    asx_region_state state;
    asx_budget budget;
    asx_outcome outcome;
    uint32_t i;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.replacement = ASX_INVALID_ID;
    ctx.task = ASX_INVALID_ID;
    ctx.spawn_status = ASX_E_PENDING;
    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &first), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &second), ASX_OK);
    for (i = 3u; i < ASX_MAX_REGIONS; i++) { ASSERT_EQ(asx_region_open(&extra), ASX_OK); }
    ASSERT_EQ(asx_task_spawn(first, poll_complete, NULL, &ctx.first_task), ASX_OK);
    ASSERT_EQ(asx_task_spawn(second, poll_complete, NULL, &second_task), ASX_OK);
    /* The first task completes before its sibling. Retire its joinable
     * record in the sibling's finalizer, then recycle the closed region
     * for an unrelated root with live work while the outer scheduler runs. */
    ASSERT_EQ(asx_region_register_finalizer(second, finalizer_recycle_closed_sibling, &ctx,
                                            &handle),
              ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(ctx.join_status, ASX_OK);
    ASSERT_EQ(ctx.open_status, ASX_OK);
    ASSERT_EQ(ctx.spawn_status, ASX_OK);
    ASSERT_EQ(asx_handle_slot(ctx.replacement), asx_handle_slot(first));
    ASSERT_EQ(asx_region_get_state(parent, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_region_get_state(first, &state), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_region_get_state(ctx.replacement, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    ASSERT_EQ(ctx.polls, 0);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ctx.replacement, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(asx_task_get_outcome(ctx.task, &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
}
#endif

int main(void) {
    RUN_TEST(task_spawn_admission_matrix);
    RUN_TEST(cleanup_spawn_limits_and_failure_atomicity);
    RUN_TEST(cleanup_spawn_invalid_arguments_and_poison);
    RUN_TEST(public_finalizer_resolves_obligation_before_close_and_handles_reentrancy);
    RUN_TEST(public_finalizer_capacity_unregistration_and_errors);
    RUN_TEST(public_finalizer_captured_cleanup_executes_and_drops_once);
    RUN_TEST(public_finalizer_nested_cancel_preserves_outer_subtree);
#ifndef ASX_DEBUG_QUARANTINE
    RUN_TEST(public_finalizer_recycled_sibling_does_not_extend_outer_drain);
#endif
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
    RUN_TEST(region_drain_parent_closes_open_child_first);
    RUN_TEST(region_drain_cancels_descendant_tasks_with_parent_attribution);
    asx_runtime_reset();
    RUN_TEST(closed_child_of_live_parent_keeps_its_slot);
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
