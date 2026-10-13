/*
 * e2e_nested_regions.c — end-to-end nested region lifecycle scenarios
 *
 * Exercises parent/child/grandchild region shutdown ordering, child-count
 * unlinking, and detailed quiescence evidence for Q2.
 *
 * Output:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *   DETAIL <JSON object> for actual mask-worker and joiner observations
 *   DIGEST <hex>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_runtime.h"
#include "runtime/runtime_internal.h"
#include <asx/asx.h>
#include <asx/runtime/trace.h>
#include <stdio.h>
#include <string.h>

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

/* Trace index of a region's REGION_CLOSED event (UINT32_MAX if absent). */
static uint32_t region_closed_trace_index(asx_region_id id) {
    uint32_t i;
    uint32_t n = asx_trace_event_count(test_runtime());
    for (i = 0; i < n; i++) {
        asx_trace_event ev;
        if (!asx_trace_event_get(test_runtime(), i, &ev)) continue;
        if (ev.kind == ASX_TRACE_REGION_CLOSED && ev.entity_id == id) return i;
    }
    return UINT32_MAX;
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

    asx_runtime_reset(test_runtime());

    SCENARIO_CHECK(asx_region_open(test_runtime(), &root) == ASX_OK, "open_root");
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

    /* Draining a child shuts down exactly its own subtree. */
    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(child1, &budget) == ASX_OK, "drain_child1_subtree");
    SCENARIO_CHECK(grandchild_slot->state == ASX_REGION_CLOSED, "grandchild_closed");
    /* Unlinked from child1's children; it still names child1 (a closed
     * child keeps its slot while its parent lives). */
    SCENARIO_CHECK(child1_slot->child_count == 0u, "grandchild_unlinked");
    SCENARIO_CHECK(grandchild_slot->parent_id == child1, "grandchild_names_parent");
    SCENARIO_CHECK(child1_slot->state == ASX_REGION_CLOSED, "child1_closed");
    SCENARIO_CHECK(root_slot->child_count == 1u, "root_count_after_child1");
    SCENARIO_CHECK(child2_slot->state == ASX_REGION_OPEN, "child2_untouched");
    SCENARIO_CHECK(region_closed_trace_index(grandchild) < region_closed_trace_index(child1),
                   "grandchild_closed_before_child1");
    log_region_state("after-child1-subtree-close", root);

    /* Draining the root is a structured shutdown of everything left: the
     * open child2 closes before the root does. */
    budget = asx_budget_from_polls(8);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_OK, "drain_root");
    SCENARIO_CHECK(child2_slot->state == ASX_REGION_CLOSED, "child2_closed");
    SCENARIO_CHECK(root_slot->child_count == 0u, "root_no_children_remaining");
    SCENARIO_CHECK(region_closed_trace_index(child2) < region_closed_trace_index(root),
                   "child2_closed_before_root");
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

    asx_runtime_reset(test_runtime());

    SCENARIO_CHECK(asx_region_open(test_runtime(), &root) == ASX_OK, "open_root");
    SCENARIO_CHECK(asx_region_open_child(root, &child) == ASX_OK, "open_child");
    SCENARIO_CHECK(asx_region_open_child(child, &grandchild) == ASX_OK, "open_grandchild");
    SCENARIO_CHECK(asx_region_slot_lookup(root, &root_slot) == ASX_OK, "lookup_root");
    SCENARIO_CHECK(asx_region_slot_lookup(child, &child_slot) == ASX_OK, "lookup_child");

    /* Draining the middle region closes its grandchild first (inside-out)
     * and leaves the root open. */
    budget = asx_budget_from_polls(4);
    SCENARIO_CHECK(asx_region_drain(child, &budget) == ASX_OK, "drain_child_subtree");
    SCENARIO_CHECK(child_slot->state == ASX_REGION_CLOSED, "child_closed");
    SCENARIO_CHECK(child_slot->child_count == 0u, "child_has_no_children");
    SCENARIO_CHECK(region_closed_trace_index(grandchild) < region_closed_trace_index(child),
                   "grandchild_closed_before_child");
    SCENARIO_CHECK(root_slot->state == ASX_REGION_OPEN, "root_still_open");
    log_region_state("child-closed", child);

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

    asx_runtime_reset(test_runtime());

    SCENARIO_CHECK(asx_region_open(test_runtime(), &root) == ASX_OK, "open_root");
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

typedef struct {
    int polls;
    int cancelled;
    int value;
    int park_first;
} body_child_ctx;

static asx_status poll_body_child(void *data, asx_task_id self) {
    body_child_ctx *ctx = (body_child_ctx *)data;
    asx_checkpoint_result checkpoint;
    asx_status st = asx_checkpoint(self, &checkpoint);
    if (st != ASX_OK) return st;
    ctx->polls++;
    if (checkpoint.cancelled) {
        ctx->cancelled++;
        return ASX_OK;
    }
    if (ctx->polls == 1) {
        if (ctx->park_first) {
            st = asx_task_park(self);
            if (st != ASX_OK) return st;
        }
        return ASX_E_PENDING;
    }
    ctx->value = 42;
    return ASX_OK;
}

static uint32_t body_cancel_trace_count(void) {
    uint32_t i, count = 0u;
    for (i = 0u; i < asx_trace_event_count(test_runtime()); i++) {
        asx_trace_event event;
        if (asx_trace_event_get(test_runtime(), i, &event) &&
            (event.kind == ASX_TRACE_CANCEL_REQUEST || event.kind == ASX_TRACE_REGION_CANCELLED)) {
            count++;
        }
    }
    return count;
}

static void scenario_nested_body_ok_waits(void) {
    SCENARIO_BEGIN("nested.body_ok_waits");
    asx_region_id regions[3];
    asx_task_id tasks[3];
    asx_task_id refused = ASX_INVALID_ID;
    body_child_ctx contexts[3];
    asx_region_state state;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK), outcome;
    asx_budget budget = asx_budget_from_polls(1u);
    uint32_t i;
    asx_runtime_reset(test_runtime());
    memset(contexts, 0, sizeof(contexts));
    SCENARIO_CHECK(asx_region_open(test_runtime(), &regions[0]) == ASX_OK, "open_root");
    SCENARIO_CHECK(asx_region_open_child(regions[0], &regions[1]) == ASX_OK, "open_child");
    SCENARIO_CHECK(asx_region_open_child(regions[1], &regions[2]) == ASX_OK, "open_grandchild");
    for (i = 0u; i < 3u; i++) {
        SCENARIO_CHECK(asx_task_spawn(regions[i], poll_body_child, &contexts[i], &tasks[i]) ==
                           ASX_OK,
                       "spawn_real_child");
    }
    SCENARIO_CHECK(asx_region_close_with_outcome(regions[0], &body, NULL) == ASX_OK,
                   "finish_ok_root");
    SCENARIO_CHECK(asx_task_spawn(regions[0], poll_complete, NULL, &refused) == ASX_E_REGION_CLOSED,
                   "ordinary_spawn_refusal");
    SCENARIO_CHECK(refused == ASX_INVALID_ID, "refused_output_unchanged");
    SCENARIO_CHECK(asx_region_wait_closed(regions[0], &budget) == ASX_E_POLL_BUDGET_EXHAUSTED,
                   "budget_boundary");
    budget = asx_budget_from_polls(16u);
    SCENARIO_CHECK(asx_region_wait_closed(regions[0], &budget) == ASX_E_PENDING,
                   "open_child_pending");
    SCENARIO_CHECK(asx_region_get_state(regions[1], &state) == ASX_OK && state == ASX_REGION_OPEN,
                   "child_not_closed_by_wait");
    SCENARIO_CHECK(asx_region_get_state(regions[2], &state) == ASX_OK && state == ASX_REGION_OPEN,
                   "grandchild_not_closed_by_wait");
    SCENARIO_CHECK(asx_region_close_with_outcome(regions[1], &body, NULL) == ASX_OK,
                   "finish_ok_child");
    SCENARIO_CHECK(asx_region_wait_closed(regions[0], &budget) == ASX_E_PENDING,
                   "grandchild_pending");
    SCENARIO_CHECK(asx_region_close_with_outcome(regions[2], &body, NULL) == ASX_OK,
                   "finish_ok_grandchild");
    SCENARIO_CHECK(asx_region_wait_closed(regions[0], &budget) == ASX_OK, "wait_inside_out_close");
    for (i = 0u; i < 3u; i++) {
        SCENARIO_CHECK(contexts[i].polls == 2 && contexts[i].cancelled == 0 &&
                           contexts[i].value == 42,
                       "normal_child_result_survives");
        SCENARIO_CHECK(asx_task_get_outcome(tasks[i], &outcome) == ASX_OK &&
                           outcome.severity == ASX_OUTCOME_OK,
                       "normal_child_outcome");
        SCENARIO_CHECK(asx_quiescence_check(regions[i]) == ASX_OK, "fully_quiescent");
        SCENARIO_CHECK(region_closed_trace_index(regions[i]) != UINT32_MAX, "close_event_present");
        log_region_state("body-ok-final", regions[i]);
    }
    SCENARIO_CHECK(region_closed_trace_index(regions[2]) < region_closed_trace_index(regions[1]) &&
                       region_closed_trace_index(regions[1]) <
                           region_closed_trace_index(regions[0]),
                   "body_close_inside_out_trace");
    SCENARIO_CHECK(body_cancel_trace_count() == 0u, "ok_must_never_cancel");
    fprintf(stderr, "[nested-e2e] body-ok values=42,42,42 cancelled=0,0,0 closes=%u,%u,%u\n",
            region_closed_trace_index(regions[2]), region_closed_trace_index(regions[1]),
            region_closed_trace_index(regions[0]));
    SCENARIO_END();
}

static void scenario_nested_body_exception(asx_outcome_severity severity) {
    SCENARIO_BEGIN(severity == ASX_OUTCOME_ERR ? "nested.body_err_fail_fast"
                                               : "nested.body_panic_fail_fast");
    asx_region_id regions[3];
    asx_task_id tasks[3];
    body_child_ctx contexts[3];
    asx_outcome body = asx_outcome_make(severity), outcome;
    asx_cancel_reason reason;
    asx_budget budget = asx_budget_from_polls(16u);
    uint32_t i;
    asx_runtime_reset(test_runtime());
    memset(contexts, 0, sizeof(contexts));
    SCENARIO_CHECK(asx_region_open(test_runtime(), &regions[0]) == ASX_OK, "open_root");
    SCENARIO_CHECK(asx_region_open_child(regions[0], &regions[1]) == ASX_OK, "open_child");
    SCENARIO_CHECK(asx_region_open_child(regions[1], &regions[2]) == ASX_OK, "open_grandchild");
    for (i = 0u; i < 3u; i++) {
        SCENARIO_CHECK(asx_task_spawn(regions[i], poll_body_child, &contexts[i], &tasks[i]) ==
                           ASX_OK,
                       "spawn_real_child");
    }
    SCENARIO_CHECK(asx_region_close_with_outcome(regions[0], &body, NULL) == ASX_OK,
                   "finish_exceptional_body");
    SCENARIO_CHECK(asx_region_wait_closed(regions[0], &budget) == ASX_OK, "wait_exception_cleanup");
    for (i = 0u; i < 3u; i++) {
        SCENARIO_CHECK(contexts[i].cancelled == 1 && contexts[i].value == 0,
                       "exception_must_cancel_child");
        SCENARIO_CHECK(asx_task_get_cancel_reason(tasks[i], &reason) == ASX_OK, "reason_available");
        SCENARIO_CHECK(reason.kind == (i == 0u ? ASX_CANCEL_FAIL_FAST : ASX_CANCEL_PARENT),
                       "fail_fast_root_parent_descendants");
        SCENARIO_CHECK(reason.origin_region == regions[i == 0u ? 0u : i - 1u],
                       "reason_attribution");
        SCENARIO_CHECK(asx_task_get_outcome(tasks[i], &outcome) == ASX_OK &&
                           outcome.severity == ASX_OUTCOME_CANCELLED,
                       "cancelled_child_outcome");
    }
    SCENARIO_CHECK(asx_region_get_close_outcome(regions[0], &outcome) == ASX_OK &&
                       outcome.severity == ASX_OUTCOME_CANCELLED && body.severity == severity,
                   "body_separate_from_child_ledger");
    SCENARIO_CHECK(body_cancel_trace_count() == 6u, "real_cancel_trace_evidence");
    fprintf(stderr, "[nested-e2e] body-exception severity=%d child_ledger=%d cancel_events=%u\n",
            (int)body.severity, (int)outcome.severity, body_cancel_trace_count());
    SCENARIO_END();
}

static void scenario_scope_finish_wake_retry(void) {
    SCENARIO_BEGIN("nested.scope_finish_wake_retry");
    asx_region_id region;
    asx_cx cx;
    asx_scope scope;
    asx_task_handle child;
    body_child_ctx context;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome changed = asx_outcome_make(ASX_OUTCOME_ERR), outcome;
    asx_runtime_reset(test_runtime());
    memset(&context, 0, sizeof(context));
    context.park_first = 1;
    SCENARIO_CHECK(asx_region_open(test_runtime(), &region) == ASX_OK, "open_scope_region");
    SCENARIO_CHECK(asx_cx_init(&cx, region, ASX_INVALID_ID, ASX_CAP_ALL) == ASX_OK, "init_host_cx");
    SCENARIO_CHECK(asx_scope_init(&scope, region, &cx, asx_budget_from_polls(8u)) == ASX_OK,
                   "init_scope");
    SCENARIO_CHECK(asx_scope_spawn(&scope, poll_body_child, &context, &child) == ASX_OK,
                   "spawn_child");
    SCENARIO_CHECK(asx_scope_finish(&scope, &body, NULL) == ASX_E_WOULD_BLOCK,
                   "parked_wait_boundary");
    SCENARIO_CHECK(scope.finish_started == 1 && scope.body_outcome.severity == ASX_OUTCOME_OK,
                   "retain_body_before_retry");
    SCENARIO_CHECK(asx_scope_finish(&scope, &changed, NULL) == ASX_E_INVALID_STATE,
                   "changed_body_refused");
    SCENARIO_CHECK(context.polls == 1 && context.cancelled == 0, "park_not_cancelled");
    SCENARIO_CHECK(asx_task_wake(child.task_id) == ASX_OK, "real_wake");
    SCENARIO_CHECK(asx_scope_finish(&scope, &body, NULL) == ASX_OK, "same_body_retry_completes");
    SCENARIO_CHECK(context.polls == 2 && context.cancelled == 0 && context.value == 42,
                   "normal_value_after_wake");
    SCENARIO_CHECK(asx_task_handle_try_join(&child, &outcome) == ASX_OK &&
                       outcome.severity == ASX_OUTCOME_OK,
                   "joined_normal_child");
    SCENARIO_CHECK(body_cancel_trace_count() == 0u, "retry_must_not_cancel");
    log_quiescence_state("scope-body-finished", region);
    SCENARIO_END();
}

/* These native scheduler witnesses assume bounded polling and explicit release
 * of raw masks. They do not retain a Rust-style guard across an await. */
typedef struct {
    const char *scenario;
    asx_task_id worker;
    uint32_t worker_polls;
    uint32_t joiner_polls;
    uint32_t leaf_calls;
    uint32_t outer_calls;
    uint32_t leaf_depth;
    uint32_t masked_checkpoints;
    uint32_t observed_at;
    uint32_t worker_polls_at_join;
    int mode;
    int released;
    int joined;
    const char *error;
    asx_outcome joined_outcome;
} mask_e2e_context;

static void emit_mask_detail(const mask_e2e_context *ctx, const char *actor, const char *stage,
                             uint32_t polls, uint32_t depth, int phase, int masked, int cancelled,
                             asx_status status, int outcome) {
    printf("DETAIL {\"test\":\"%s\",\"actor\":\"%s\",\"stage\":\"%s\","
           "\"poll\":%u,\"worker_polls\":%u,\"joiner_polls\":%u,\"mask_depth\":%u,"
           "\"cancel_phase\":%d,\"checkpoint_masked\":%d,\"checkpoint_cancelled\":%d,"
           "\"observed_status\":%d,\"callback_calls\":%u,\"terminal_outcome\":",
           ctx->scenario, actor, stage, polls, ctx->worker_polls, ctx->joiner_polls, depth, phase,
           masked, cancelled, (int)status, ctx->leaf_calls);
    if (outcome < 0)
        printf("null");
    else
        printf("%d", outcome);
    printf("}\n");
}

static asx_status mask_worker_checkpoint(mask_e2e_context *ctx, asx_task_id self, const char *stage,
                                         uint32_t expected_depth, int expected_masked,
                                         int expected_cancelled) {
    uint32_t depth = UINT32_MAX;
    asx_checkpoint_result checkpoint;
    asx_status st = asx_task_mask_depth(self, &depth);
    memset(&checkpoint, 0, sizeof(checkpoint));
    if (st == ASX_OK) st = asx_checkpoint(self, &checkpoint);
    emit_mask_detail(ctx, "worker", stage, ctx->worker_polls, depth,
                     st == ASX_OK ? (int)checkpoint.phase : -1, checkpoint.masked,
                     checkpoint.cancelled, st, -1);
    if (st != ASX_OK || depth != expected_depth || checkpoint.masked != expected_masked ||
        checkpoint.cancelled != expected_cancelled ||
        (expected_masked && checkpoint.phase != ASX_CANCEL_PHASE_REQUESTED) ||
        (expected_cancelled && checkpoint.phase != ASX_CANCEL_PHASE_CANCELLING)) {
        if (ctx->error == NULL) ctx->error = stage;
        return ASX_E_INVALID_STATE;
    }
    if (checkpoint.masked) ctx->masked_checkpoints++;
    if (checkpoint.cancelled && ctx->observed_at == 0u) ctx->observed_at = ctx->worker_polls;
    return ASX_OK;
}

static asx_status poll_mask_leaf(void *data, asx_task_id self) {
    mask_e2e_context *ctx = (mask_e2e_context *)data;
    asx_status st;
    ctx->leaf_calls++;
    st = mask_worker_checkpoint(ctx, self, "leaf_first", ctx->leaf_depth, 1, 0);
    if (st != ASX_OK) return st;
    st = mask_worker_checkpoint(ctx, self, "leaf_repeated", ctx->leaf_depth, 1, 0);
    if (st != ASX_OK) return st;
    /* A genuine early error return must still release this guard. */
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status poll_mask_outer(void *data, asx_task_id self) {
    mask_e2e_context *ctx = (mask_e2e_context *)data;
    uint32_t depth = ctx->mode == 1 ? 2u : 1u;
    asx_status st;
    ctx->outer_calls++;
    st = mask_worker_checkpoint(ctx, self, "outer_enter", depth, 1, 0);
    if (st != ASX_OK) return st;
    st = asx_task_masked(self, poll_mask_leaf, ctx);
    if (st != ASX_E_INVALID_ARGUMENT) {
        if (ctx->error == NULL) ctx->error = "nested_error_status";
        return ASX_E_INVALID_STATE;
    }
    st = mask_worker_checkpoint(ctx, self, "inner_restored", depth, 1, 0);
    if (st != ASX_OK) return st;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status poll_mask_worker(void *data, asx_task_id self) {
    mask_e2e_context *ctx = (mask_e2e_context *)data;
    uint32_t depth;
    uint32_t i;
    asx_status st;
    ctx->worker_polls++;
    if (ctx->worker_polls == 1u) {
        depth = ctx->mode == 2 ? ASX_MAX_MASK_DEPTH - 1u : ctx->mode == 1 ? 1u : 0u;
        for (i = 0u; i < depth; i++) {
            st = asx_task_mask(self);
            if (st != ASX_OK) {
                ctx->error = "initial_raw_mask";
                return st;
            }
        }
        st = mask_worker_checkpoint(ctx, self, "before_cancel", depth, 0, 0);
        return st == ASX_OK ? ASX_E_PENDING : st;
    }
    if (ctx->worker_polls == 2u) {
        if (ctx->mode == 2) {
            uint32_t calls;
            uint32_t refused_depth = UINT32_MAX;
            uint64_t digest;
            asx_cancel_phase refused_phase;
            st = asx_task_masked(self, poll_mask_leaf, ctx);
            if (st != ASX_E_INVALID_ARGUMENT) {
                ctx->error = "depth64_error_status";
                return ASX_E_INVALID_STATE;
            }
            st = mask_worker_checkpoint(ctx, self, "depth63_restored", ASX_MAX_MASK_DEPTH - 1u, 1,
                                        0);
            if (st != ASX_OK) return st;
            st = asx_task_mask(self);
            if (st != ASX_OK) {
                ctx->error = "raw_depth64";
                return st;
            }
            calls = ctx->leaf_calls;
            digest = asx_trace_digest(test_runtime());
            st = asx_task_masked(self, poll_mask_leaf, ctx);
            if (asx_task_mask_depth(self, &refused_depth) != ASX_OK ||
                asx_task_get_cancel_phase(self, &refused_phase) != ASX_OK) {
                ctx->error = "depth65_observation";
                return ASX_E_INVALID_STATE;
            }
            emit_mask_detail(ctx, "worker", "depth65_refused", ctx->worker_polls, refused_depth,
                             (int)refused_phase, -1, -1, st, -1);
            if (st != ASX_E_INVALID_STATE || ctx->leaf_calls != calls ||
                asx_trace_digest(test_runtime()) != digest) {
                ctx->error = "depth65_must_not_run_callback";
                return ASX_E_INVALID_STATE;
            }
            st = mask_worker_checkpoint(ctx, self, "depth64_unchanged", ASX_MAX_MASK_DEPTH, 1, 0);
            if (st != ASX_OK) return st;
            for (i = 0u; i < ASX_MAX_MASK_DEPTH; i++) {
                st = asx_task_unmask(self);
                if (st != ASX_OK) {
                    ctx->error = "release_depth64";
                    return st;
                }
            }
        } else {
            st = asx_task_masked(self, poll_mask_outer, ctx);
            if (st != ASX_E_INVALID_ARGUMENT) {
                if (ctx->error == NULL) ctx->error = "outer_error_status";
                return ASX_E_INVALID_STATE;
            }
        }
    }
    depth = ctx->mode == 1 && !ctx->released ? 1u : 0u;
    st = mask_worker_checkpoint(ctx, self, depth == 0u ? "unmasked_checkpoint" : "raw_mask_leaked",
                                depth, depth != 0u, depth == 0u);
    if (st != ASX_OK) return st;
    /* One more real cleanup poll after acknowledgement, while the joiner
     * remains parked. Completion wakes the joiner through the runtime. */
    return ctx->observed_at != 0u && ctx->worker_polls > ctx->observed_at ? ASX_OK : ASX_E_PENDING;
}

static asx_status poll_mask_joiner(void *data, asx_task_id self) {
    mask_e2e_context *ctx = (mask_e2e_context *)data;
    uint32_t depth = UINT32_MAX;
    asx_cancel_phase phase;
    asx_status st;
    ctx->joiner_polls++;
    st = asx_task_mask_depth(ctx->worker, &depth);
    if (st == ASX_OK) st = asx_task_get_cancel_phase(ctx->worker, &phase);
    if (st != ASX_OK) {
        ctx->error = "joiner_target_observation";
        return st;
    }
    st = asx_task_join_poll(self, ctx->worker, &ctx->joined_outcome);
    emit_mask_detail(ctx, "joiner", st == ASX_E_PENDING ? "join_waiting" : "join_result",
                     ctx->joiner_polls, depth, (int)phase, -1, -1, st,
                     st == ASX_OK ? (int)ctx->joined_outcome.severity : -1);
    if (st == ASX_E_PENDING) return st;
    if (st != ASX_OK) {
        ctx->error = "joiner_failed";
        return st;
    }
    ctx->joined = 1;
    ctx->worker_polls_at_join = ctx->worker_polls;
    return ASX_OK;
}

static void scenario_nested_guarded_mask(int mode) {
    SCENARIO_BEGIN(mode == 1   ? "nested.raw_mask_release_required"
                   : mode == 2 ? "nested.mask_guard_depth_limit"
                               : "nested.mask_guard_restores");
    mask_e2e_context ctx;
    asx_region_id root, child;
    asx_task_id joiner;
    asx_outcome outcome;
    asx_budget budget;
    uint32_t depth;
    asx_cancel_phase phase;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scenario = _scenario_id;
    ctx.mode = mode;
    ctx.leaf_depth = mode == 2 ? ASX_MAX_MASK_DEPTH : mode == 1 ? 3u : 2u;
    asx_runtime_reset(test_runtime());
    SCENARIO_CHECK(asx_region_open(test_runtime(), &root) == ASX_OK, "open_mask_root");
    SCENARIO_CHECK(asx_region_open_child(root, &child) == ASX_OK, "open_mask_child");
    /* The joiner polls before its child worker, genuinely parks, then is
     * woken by completion rather than by a test-injected notification. */
    SCENARIO_CHECK(asx_task_spawn(root, poll_mask_joiner, &ctx, &joiner) == ASX_OK,
                   "spawn_mask_joiner");
    SCENARIO_CHECK(asx_task_spawn(child, poll_mask_worker, &ctx, &ctx.worker) == ASX_OK,
                   "spawn_mask_worker");
    budget = asx_budget_from_polls(2u);
    SCENARIO_CHECK(asx_scheduler_run(root, &budget) == ASX_E_POLL_BUDGET_EXHAUSTED,
                   "initial_bounded_polls");
    SCENARIO_CHECK(ctx.worker_polls == 1u && ctx.joiner_polls == 1u && !ctx.joined &&
                       ctx.error == NULL,
                   "real_joiner_waits_before_cancel");
    SCENARIO_CHECK(asx_task_cancel(ctx.worker, ASX_CANCEL_USER) == ASX_OK, "cancel_real_worker");
    if (mode == 1) {
        budget = asx_budget_from_polls(4u);
        SCENARIO_CHECK(asx_scheduler_run(root, &budget) == ASX_E_POLL_BUDGET_EXHAUSTED,
                       "leaked_mask_stays_bounded_pending");
        SCENARIO_CHECK(ctx.worker_polls == 5u && ctx.joiner_polls == 1u && !ctx.joined &&
                           ctx.observed_at == 0u && ctx.masked_checkpoints == 8u &&
                           ctx.error == NULL,
                       "leaked_mask_defers_cancel_and_join");
        SCENARIO_CHECK(asx_task_mask_depth(ctx.worker, &depth) == ASX_OK && depth == 1u &&
                           asx_task_get_cancel_phase(ctx.worker, &phase) == ASX_OK &&
                           phase == ASX_CANCEL_PHASE_REQUESTED,
                       "raw_mask_remains_requested");
        SCENARIO_CHECK(asx_task_get_outcome(ctx.worker, &outcome) == ASX_E_TASK_NOT_COMPLETED,
                       "masked_worker_not_completed");
        SCENARIO_CHECK(asx_task_unmask(ctx.worker) == ASX_OK, "explicit_release_raw_mask");
        ctx.released = 1;
    }
    budget = asx_budget_from_polls(8u);
    SCENARIO_CHECK(asx_scheduler_run(root, &budget) == ASX_OK, "masked_worker_then_joiner_drain");
    SCENARIO_CHECK(ctx.error == NULL, ctx.error == NULL ? "mask_observation" : ctx.error);
    SCENARIO_CHECK(ctx.leaf_calls == 1u && ctx.outer_calls == (mode == 2 ? 0u : 1u),
                   "guard_callbacks_run_exactly_once");
    SCENARIO_CHECK(ctx.observed_at == (mode == 1 ? 6u : 2u) &&
                       ctx.worker_polls == ctx.observed_at + 1u && ctx.joiner_polls == 2u &&
                       ctx.joined && ctx.worker_polls_at_join == ctx.worker_polls,
                   "join_only_after_unmasked_cleanup_completion");
    SCENARIO_CHECK(ctx.joined_outcome.severity == ASX_OUTCOME_CANCELLED,
                   "join_observes_cancelled_terminal_outcome");
    SCENARIO_CHECK(asx_task_get_outcome(ctx.worker, &outcome) != ASX_OK, "worker_join_consumed");
    SCENARIO_CHECK(asx_task_join(joiner, &outcome) == ASX_OK && outcome.severity == ASX_OUTCOME_OK,
                   "joiner_finishes_normally");
    budget = asx_budget_from_polls(8u);
    SCENARIO_CHECK(asx_region_drain(root, &budget) == ASX_OK, "mask_subtree_closes");
    SCENARIO_CHECK(asx_quiescence_check(root) == ASX_OK, "mask_subtree_quiescent");
    SCENARIO_END();
}

int main(void) {
    scenario_nested_lifecycle_full();
    scenario_nested_inside_out_pending();
    scenario_nested_q2_evidence();
    scenario_nested_body_ok_waits();
    scenario_nested_body_exception(ASX_OUTCOME_ERR);
    scenario_nested_body_exception(ASX_OUTCOME_PANICKED);
    scenario_scope_finish_wake_retry();
    scenario_nested_guarded_mask(0);
    scenario_nested_guarded_mask(1);
    scenario_nested_guarded_mask(2);

    printf("DIGEST %016llx\n", (unsigned long long)asx_trace_digest(test_runtime()));
    fprintf(stderr, "[e2e] nested_regions: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
