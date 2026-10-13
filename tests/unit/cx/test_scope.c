/*
 * test_scope.c — unit tests for structured concurrency scope
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "../../test_runtime.h"
#include <asx/cx/scope.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* Suppress warn_unused_result in test helpers */
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Poll functions for testing                                          */
/* ------------------------------------------------------------------ */

static asx_status poll_immediate_ok(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static int g_poll_count = 0;

static asx_status poll_count_twice(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    g_poll_count++;
    if (g_poll_count < 2) return ASX_E_PENDING;
    return ASX_OK;
}

static asx_status poll_always_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Helper: set up runtime + region + cx                                */
/* ------------------------------------------------------------------ */

static asx_runtime g_rt;
static asx_region_id g_rid;
static asx_cx g_cx;

static void setup(void) {
    MUST_OK(asx_runtime_init_default(&g_rt));
    MUST_OK(asx_region_open(&g_rt, &g_rid));
    MUST_OK(asx_cx_init(&g_cx, g_rid, ASX_INVALID_ID, ASX_CAP_ALL));
    g_poll_count = 0;
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

/* ------------------------------------------------------------------ */
/* Join error string tests                                             */
/* ------------------------------------------------------------------ */

TEST(join_error_str_ok) { ASSERT_STR_EQ(asx_join_error_str(ASX_JOIN_OK), "ok"); }

TEST(join_error_str_cancelled) {
    ASSERT_STR_EQ(asx_join_error_str(ASX_JOIN_CANCELLED), "cancelled");
}

TEST(join_error_str_panicked) { ASSERT_STR_EQ(asx_join_error_str(ASX_JOIN_PANICKED), "panicked"); }

TEST(join_error_str_polled_after) {
    ASSERT_STR_EQ(asx_join_error_str(ASX_JOIN_POLLED_AFTER_COMPLETE), "polled after completion");
}

/* ------------------------------------------------------------------ */
/* Scope init tests                                                    */
/* ------------------------------------------------------------------ */

TEST(scope_init_null_scope_fails) {
    setup();
    ASSERT_EQ(asx_scope_init(NULL, g_rid, &g_cx, asx_budget_infinite()), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(scope_init_null_cx_fails) {
    asx_scope scope;
    setup();
    ASSERT_EQ(asx_scope_init(&scope, g_rid, NULL, asx_budget_infinite()), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(scope_init_invalid_region_fails) {
    asx_scope scope;
    setup();
    ASSERT_EQ(asx_scope_init(&scope, ASX_INVALID_ID, &g_cx, asx_budget_infinite()),
              ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(scope_init_no_spawn_cap_fails) {
    asx_scope scope;
    asx_cx nocap;
    setup();
    MUST_OK(asx_cx_init(&nocap, g_rid, ASX_INVALID_ID, ASX_CAP_NONE));
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &nocap, asx_budget_infinite()),
              ASX_E_PERMISSION_DENIED);
    teardown();
}

TEST(scope_init_region_mismatch_fails_closed) {
    asx_scope scope;
    asx_cx wrong_region;
    setup();
    MUST_OK(asx_cx_init(&wrong_region, g_rid + 1u, ASX_INVALID_ID, ASX_CAP_ALL));
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &wrong_region, asx_budget_infinite()),
              ASX_E_PERMISSION_DENIED);
    teardown();
}

TEST(scope_init_invalid_cx_fails) {
    asx_scope scope;
    asx_cx invalid;
    memset(&invalid, 0, sizeof(invalid));
    setup();
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &invalid, asx_budget_infinite()), ASX_E_INVALID_STATE);
    teardown();
}

TEST(scope_init_success) {
    asx_scope scope;
    setup();
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()), ASX_OK);
    ASSERT_EQ(asx_scope_region(&scope), g_rid);
    ASSERT_EQ(asx_scope_spawned_count(&scope), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Scope spawn tests                                                   */
/* ------------------------------------------------------------------ */

TEST(scope_spawn_null_scope_fails) {
    asx_task_handle h;
    ASSERT_EQ(asx_scope_spawn(NULL, poll_immediate_ok, NULL, &h), ASX_E_INVALID_ARGUMENT);
}

TEST(scope_spawn_null_poll_fails) {
    asx_scope scope;
    asx_task_handle h;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn(&scope, NULL, NULL, &h), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(scope_spawn_null_handle_fails) {
    asx_scope scope;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(scope_spawn_zeroed_scope_fails_closed) {
    asx_scope scope;
    asx_task_handle h;
    memset(&scope, 0, sizeof(scope));
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h), ASX_E_INVALID_STATE);
}

TEST(scope_spawn_success) {
    asx_scope scope;
    asx_task_handle h;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h), ASX_OK);
    ASSERT_NE(asx_task_handle_task_id(&h), ASX_INVALID_ID);
    ASSERT_EQ(h.region_id, g_rid);
    ASSERT_EQ(h.joined, 0);
    ASSERT_EQ(asx_scope_spawned_count(&scope), 1u);
    teardown();
}

TEST(scope_spawn_multiple) {
    asx_scope scope;
    asx_task_handle h1, h2, h3;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h2), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h3), ASX_OK);
    ASSERT_EQ(asx_scope_spawned_count(&scope), 3u);
    ASSERT_NE(h1.task_id, h2.task_id);
    ASSERT_NE(h2.task_id, h3.task_id);
    teardown();
}

TEST(scope_spawn_with_cx_binds_spawned_task) {
    asx_scope scope;
    asx_task_handle h;
    asx_cx child_cx;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn_with_cx(&scope, ASX_CAP_CLOCK_READ | ASX_CAP_ENTROPY,
                                      poll_immediate_ok, NULL, &h, &child_cx),
              ASX_OK);
    ASSERT_EQ(asx_cx_region(&child_cx), g_rid);
    ASSERT_EQ(asx_cx_task(&child_cx), h.task_id);
    ASSERT_TRUE(asx_cx_has_cap(&child_cx, ASX_CAP_CLOCK_READ));
    ASSERT_TRUE(asx_cx_has_cap(&child_cx, ASX_CAP_ENTROPY));
    ASSERT_FALSE(asx_cx_has_cap(&child_cx, ASX_CAP_SPAWN));
    teardown();
}

TEST(scope_spawn_with_cx_escalation_fails) {
    asx_scope scope;
    asx_task_handle h;
    asx_cx child_cx;
    asx_cx limited;
    setup();
    MUST_OK(asx_cx_init(&limited, g_rid, ASX_INVALID_ID, ASX_CAP_SPAWN | ASX_CAP_CLOCK_READ));
    MUST_OK(asx_scope_init(&scope, g_rid, &limited, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn_with_cx(&scope, ASX_CAP_SPAWN | ASX_CAP_ENTROPY, poll_immediate_ok,
                                      NULL, &h, &child_cx),
              ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(scope_spawn_with_cx_escalation_does_not_spawn_task) {
    asx_scope scope;
    asx_task_handle h;
    asx_cx child_cx;
    asx_cx limited;
    setup();
    MUST_OK(asx_cx_init(&limited, g_rid, ASX_INVALID_ID, ASX_CAP_SPAWN | ASX_CAP_CLOCK_READ));
    MUST_OK(asx_scope_init(&scope, g_rid, &limited, asx_budget_infinite()));
    h.task_id = ASX_INVALID_ID;
    ASSERT_EQ(asx_scope_spawn_with_cx(&scope, ASX_CAP_SPAWN | ASX_CAP_ENTROPY, poll_immediate_ok,
                                      NULL, &h, &child_cx),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_scope_spawned_count(&scope), 0u);
    ASSERT_EQ(h.task_id, ASX_INVALID_ID);
    teardown();
}

/* A scope carries the envelope its Cx held when it was made (Rust
 * Scope::new_with_capability_budget), and the Cx of a task spawned through
 * it applies that (scope.rs:1542): the task's Cx can only be as wide as the
 * scope's (bd-9kll.3.27). */
TEST(scope_spawned_cx_carries_the_scopes_envelope) {
    asx_scope scope;
    asx_task_handle h;
    asx_cx child_cx;
    asx_cx narrowed;
    asx_capability_budget env =
        asx_capability_budget_with_memory_bytes(asx_capability_budget_unspecified(), 64u);
    setup();
    MUST_OK(asx_cx_init(&narrowed, g_rid, ASX_INVALID_ID, ASX_CAP_SPAWN));
    MUST_OK(asx_cx_apply_child_capability_budget(&narrowed, &env, NULL, NULL, NULL));
    MUST_OK(asx_scope_init(&scope, g_rid, &narrowed, asx_budget_infinite()));
    ASSERT_EQ(asx_scope_spawn_with_cx(&scope, ASX_CAP_SPAWN, poll_immediate_ok, NULL, &h,
                                      &child_cx),
              ASX_OK);
    ASSERT_TRUE(asx_cx_capability_budget(&child_cx).has_memory_bytes);
    ASSERT_EQ(asx_cx_capability_budget(&child_cx).memory_bytes, (uint64_t)64);
    ASSERT_FALSE(asx_cx_capability_budget(&child_cx).has_cpu_units);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Task handle tests                                                   */
/* ------------------------------------------------------------------ */

TEST(handle_null_returns_invalid) {
    ASSERT_EQ(asx_task_handle_task_id(NULL), ASX_INVALID_ID);
    ASSERT_FALSE(asx_task_handle_is_finished(NULL));
}

TEST(handle_try_join_null_fails) {
    asx_outcome out;
    ASSERT_EQ(asx_task_handle_try_join(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(handle_try_join_null_out_fails) {
    asx_task_handle h;
    h.task_id = ASX_INVALID_ID;
    h.joined = 0;
    ASSERT_EQ(asx_task_handle_try_join(&h, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(handle_not_finished_before_run) {
    asx_scope scope;
    asx_task_handle h;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    MUST_OK(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h));
    /* Task is spawned but not yet polled — should not be finished */
    /* (Actually task might be in CREATED state, which is not COMPLETED) */
    ASSERT_FALSE(asx_task_handle_is_finished(&h));
    teardown();
}

/* ------------------------------------------------------------------ */
/* Scope run + join integration tests                                  */
/* ------------------------------------------------------------------ */

TEST(scope_run_single_task_completes) {
    asx_scope scope;
    asx_task_handle h;
    asx_outcome out;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(100)));
    MUST_OK(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h));
    ASSERT_EQ(asx_scope_run(&scope), ASX_OK);
    ASSERT_TRUE(asx_task_handle_is_finished(&h));
    ASSERT_EQ(asx_task_handle_try_join(&h, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(asx_task_handle_join_error(&out), ASX_JOIN_OK);
    teardown();
}

TEST(scope_run_multi_poll_task) {
    asx_scope scope;
    asx_task_handle h;
    asx_outcome out;
    setup();
    g_poll_count = 0;
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(100)));
    MUST_OK(asx_scope_spawn(&scope, poll_count_twice, NULL, &h));
    ASSERT_EQ(asx_scope_run(&scope), ASX_OK);
    ASSERT_TRUE(asx_task_handle_is_finished(&h));
    ASSERT_EQ(asx_task_handle_try_join(&h, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(g_poll_count, 2);
    teardown();
}

TEST(scope_run_budget_exhaustion) {
    asx_scope scope;
    asx_task_handle h;
    asx_status st;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(1)));
    MUST_OK(asx_scope_spawn(&scope, poll_always_pending, NULL, &h));
    st = asx_scope_run(&scope);
    /* Should exhaust budget before completing */
    ASSERT_TRUE(st == ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_FALSE(asx_task_handle_is_finished(&h));
    teardown();
}

TEST(scope_run_multiple_tasks) {
    asx_scope scope;
    asx_task_handle h1, h2;
    asx_outcome out1, out2;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(100)));
    MUST_OK(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h1));
    MUST_OK(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h2));
    ASSERT_EQ(asx_scope_run(&scope), ASX_OK);
    ASSERT_TRUE(asx_task_handle_is_finished(&h1));
    ASSERT_TRUE(asx_task_handle_is_finished(&h2));
    ASSERT_EQ(asx_task_handle_try_join(&h1, &out1), ASX_OK);
    ASSERT_EQ(asx_task_handle_try_join(&h2, &out2), ASX_OK);
    ASSERT_EQ(out1.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(out2.severity, ASX_OUTCOME_OK);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Try join double-consume guard                                       */
/* ------------------------------------------------------------------ */

TEST(try_join_double_consume_fails) {
    asx_scope scope;
    asx_task_handle h;
    asx_outcome out;
    setup();
    MUST_OK(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(100)));
    MUST_OK(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &h));
    MUST_OK(asx_scope_run(&scope));
    ASSERT_EQ(asx_task_handle_try_join(&h, &out), ASX_OK);
    /* Second join should fail */
    ASSERT_EQ(asx_task_handle_try_join(&h, &out), ASX_E_INVALID_STATE);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Abort tests                                                         */
/* ------------------------------------------------------------------ */

TEST(handle_abort_null_fails) { ASSERT_EQ(asx_task_handle_abort(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(handle_abort_with_kind_null_fails) {
    ASSERT_EQ(asx_task_handle_abort_with_kind(NULL, ASX_CANCEL_USER), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Join error classification tests                                     */
/* ------------------------------------------------------------------ */

TEST(join_error_ok_outcome) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_OK);
    ASSERT_EQ(asx_task_handle_join_error(&o), ASX_JOIN_OK);
}

TEST(join_error_err_outcome) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_ERR);
    ASSERT_EQ(asx_task_handle_join_error(&o), ASX_JOIN_OK);
}

TEST(join_error_cancelled_outcome) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_handle_join_error(&o), ASX_JOIN_CANCELLED);
}

TEST(join_error_panicked_outcome) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_task_handle_join_error(&o), ASX_JOIN_PANICKED);
}

TEST(join_error_null_returns_ok) { ASSERT_EQ(asx_task_handle_join_error(NULL), ASX_JOIN_OK); }

/* ------------------------------------------------------------------ */
/* Scope query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(scope_region_null_returns_invalid) { ASSERT_EQ(asx_scope_region(NULL), ASX_INVALID_ID); }

TEST(scope_spawned_count_null_returns_zero) { ASSERT_EQ(asx_scope_spawned_count(NULL), 0u); }

TEST(scope_run_null_fails) { ASSERT_EQ(asx_scope_run(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(scope_drain_null_fails) { ASSERT_EQ(asx_scope_drain(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(scope_run_zeroed_scope_fails_closed) {
    asx_scope scope;
    memset(&scope, 0, sizeof(scope));
    ASSERT_EQ(asx_scope_run(&scope), ASX_E_INVALID_STATE);
}

TEST(scope_drain_zeroed_scope_fails_closed) {
    asx_scope scope;
    memset(&scope, 0, sizeof(scope));
    ASSERT_EQ(asx_scope_drain(&scope), ASX_E_INVALID_STATE);
}

/* Body-driven finish uses real cooperative children. The integer result is
 * user state, not an outcome payload (the public C outcome is severity only). */
typedef struct {
    int polls;
    int cancelled;
    int value;
    int park_first;
    int goal;
} finish_child_ctx;

static asx_status poll_finish_child(void *data, asx_task_id self) {
    finish_child_ctx *ctx = (finish_child_ctx *)data;
    asx_checkpoint_result checkpoint;
    asx_status st = asx_checkpoint(self, &checkpoint);
    if (st != ASX_OK) return st;
    ctx->polls++;
    if (checkpoint.cancelled) {
        ctx->cancelled++;
        return ASX_OK;
    }
    if (ctx->park_first && ctx->polls == 1) {
        st = asx_task_park(self);
        return st == ASX_OK ? ASX_E_PENDING : st;
    }
    if (ctx->polls < ctx->goal) return ASX_E_PENDING;
    ctx->value = 42;
    return ASX_OK;
}

static uint32_t scope_cancel_trace_count(void) {
    uint32_t i, count = 0u;
    for (i = 0u; i < asx_trace_event_count(&g_rt); i++) {
        asx_trace_event event;
        if (asx_trace_event_get(&g_rt, i, &event) &&
            (event.kind == ASX_TRACE_CANCEL_REQUEST || event.kind == ASX_TRACE_REGION_CANCELLED)) {
            count++;
        }
    }
    return count;
}

TEST(scope_finish_ok_preserves_normal_child_result_and_join) {
    asx_scope scope;
    asx_task_handle child;
    finish_child_ctx ctx;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome child_outcome, region_outcome;
    asx_region_state state;
    setup();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 2;
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(16u)), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_finish_child, &ctx, &child), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    ASSERT_EQ(scope.finish_started, 1);
    ASSERT_EQ(scope.body_outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(ctx.polls, 2);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(ctx.value, 42);
    ASSERT_EQ(asx_region_get_state(g_rid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_handle_try_join(&child, &child_outcome), ASX_OK);
    ASSERT_EQ(child_outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(asx_region_get_close_outcome(g_rid, &region_outcome), ASX_OK);
    ASSERT_EQ(region_outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(scope_cancel_trace_count(), 0u);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    ASSERT_EQ(ctx.polls, 2);
    teardown();
}

TEST(scope_finish_budget_retry_never_cancels_or_reissues_close) {
    asx_scope scope;
    asx_task_handle child;
    finish_child_ctx ctx;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome changed = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_task_handle refused;
    asx_scope before;
    uint32_t events;
    setup();
    memset(&ctx, 0, sizeof(ctx));
    memset(&refused, 0, sizeof(refused));
    ctx.goal = 3;
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(1u)), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_finish_child, &ctx, &child), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(scope.budget.poll_quota, 0u);
    memcpy(&before, &scope, sizeof(before));
    events = asx_trace_event_count(&g_rt);
    ASSERT_EQ(asx_scope_finish(&scope, &changed, NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(memcmp(&scope, &before, sizeof(scope)), 0);
    ASSERT_EQ(asx_trace_event_count(&g_rt), events);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_immediate_ok, NULL, &refused), ASX_E_REGION_CLOSED);
    ASSERT_EQ(refused.task_id, ASX_INVALID_ID);
    scope.budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    ASSERT_EQ(ctx.polls, 3);
    ASSERT_EQ(ctx.value, 42);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(scope_cancel_trace_count(), 0u);
    teardown();
}

TEST(scope_finish_parked_child_resumes_after_explicit_wake) {
    asx_scope scope;
    asx_task_handle child;
    finish_child_ctx ctx;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    setup();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 2;
    ctx.park_first = 1;
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_finish_child, &ctx, &child), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(asx_task_wake(child.task_id), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    ASSERT_EQ(ctx.polls, 2);
    ASSERT_EQ(ctx.value, 42);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(scope_cancel_trace_count(), 0u);
    teardown();
}

TEST(scope_finish_exception_keeps_body_separate_from_child_outcome) {
    static const asx_outcome_severity severities[] = {ASX_OUTCOME_ERR, ASX_OUTCOME_PANICKED};
    uint32_t i;
    for (i = 0u; i < 2u; i++) {
        asx_scope scope;
        asx_task_handle child;
        finish_child_ctx ctx;
        asx_outcome body = asx_outcome_make(severities[i]);
        asx_outcome child_outcome, region_outcome;
        asx_cancel_reason reason;
        setup();
        memset(&ctx, 0, sizeof(ctx));
        ctx.goal = 2;
        ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
        ASSERT_EQ(asx_scope_spawn(&scope, poll_finish_child, &ctx, &child), ASX_OK);
        ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
        ASSERT_EQ(scope.body_outcome.severity, severities[i]);
        ASSERT_EQ(body.severity, severities[i]);
        ASSERT_EQ(ctx.cancelled, 1);
        ASSERT_EQ(ctx.value, 0);
        ASSERT_EQ(asx_task_get_cancel_reason(child.task_id, &reason), ASX_OK);
        ASSERT_EQ(reason.kind, ASX_CANCEL_FAIL_FAST);
        ASSERT_EQ(asx_task_handle_try_join(&child, &child_outcome), ASX_OK);
        ASSERT_EQ(child_outcome.severity, ASX_OUTCOME_CANCELLED);
        ASSERT_EQ(asx_region_get_close_outcome(g_rid, &region_outcome), ASX_OK);
        ASSERT_EQ(region_outcome.severity, ASX_OUTCOME_CANCELLED);
        ASSERT_TRUE(scope_cancel_trace_count() > 0u);
        teardown();
    }
}

TEST(scope_finish_cancelled_body_preserves_requested_reason) {
    asx_scope scope;
    asx_task_handle child;
    finish_child_ctx ctx;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_cancel_reason reason =
        asx_cancel_reason_default(test_runtime(), ASX_CANCEL_SHUTDOWN, "body cancelled");
    asx_cancel_reason seen;
    setup();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 2;
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_finish_child, &ctx, &child), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, &reason), ASX_OK);
    ASSERT_EQ(ctx.cancelled, 1);
    ASSERT_EQ(asx_task_get_cancel_reason(child.task_id, &seen), ASX_OK);
    ASSERT_EQ(seen.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_STR_EQ(seen.message, "body cancelled");
    teardown();
}

TEST(scope_finish_invalid_body_and_reason_are_failure_atomic) {
    asx_scope scope, before;
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome cancelled = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome invalid = asx_outcome_make((asx_outcome_severity)99);
    asx_cancel_reason reason = asx_cancel_reason_default(test_runtime(), ASX_CANCEL_USER, NULL);
    asx_region_state state;
    uint32_t events;
    setup();
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    memcpy(&before, &scope, sizeof(before));
    events = asx_trace_event_count(&g_rt);
    ASSERT_EQ(asx_scope_finish(NULL, &ok, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_scope_finish(&scope, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_scope_finish(&scope, &invalid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_scope_finish(&scope, &cancelled, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_scope_finish(&scope, &ok, &reason), ASX_E_INVALID_ARGUMENT);
    reason.kind = (asx_cancel_kind)99;
    ASSERT_EQ(asx_scope_finish(&scope, &cancelled, &reason), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(memcmp(&scope, &before, sizeof(scope)), 0);
    ASSERT_EQ(asx_trace_event_count(&g_rt), events);
    ASSERT_EQ(asx_region_get_state(g_rid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    ASSERT_EQ(asx_scope_finish(&scope, &ok, NULL), ASX_OK);
    teardown();
}

TEST(scope_finish_cancelled_retry_without_reason_does_not_cancel_again) {
    asx_scope scope;
    asx_task_handle child;
    finish_child_ctx ctx;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_cancel_reason requested =
        asx_cancel_reason_default(test_runtime(), ASX_CANCEL_USER, "original body");
    asx_cancel_reason seen;
    uint32_t cancellations;
    setup();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 2;
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(0u)), ASX_OK);
    ASSERT_EQ(asx_scope_spawn(&scope, poll_finish_child, &ctx, &child), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, &requested), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(scope.finish_started, 1);
    ASSERT_EQ(scope.body_outcome.severity, ASX_OUTCOME_CANCELLED);
    cancellations = scope_cancel_trace_count();
    ASSERT_TRUE(cancellations > 0u);
    ASSERT_EQ(ctx.polls, 0);
    scope.budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    ASSERT_EQ(scope_cancel_trace_count(), cancellations);
    ASSERT_EQ(ctx.cancelled, 1);
    ASSERT_EQ(asx_task_get_cancel_reason(child.task_id, &seen), ASX_OK);
    ASSERT_EQ(seen.kind, ASX_CANCEL_USER);
    ASSERT_STR_EQ(seen.message, "original body");
    teardown();
}

TEST(scope_finish_uninitialized_or_invalid_authority_refuses_before_close) {
    asx_scope scope, valid;
    asx_cx other_cx;
    asx_region_id other;
    asx_region_state state;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    uint32_t events;
    memset(&scope, 0, sizeof(scope));
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_E_INVALID_STATE);
    setup();
    ASSERT_EQ(asx_scope_init(&valid, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &other), ASX_OK);
    ASSERT_EQ(asx_cx_init(&other_cx, other, ASX_INVALID_ID, ASX_CAP_ALL), ASX_OK);
    memcpy(&scope, &valid, sizeof(scope));
    scope.cx = other_cx;
    events = asx_trace_event_count(&g_rt);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_trace_event_count(&g_rt), events);
    ASSERT_EQ(asx_region_get_state(g_rid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    memcpy(&scope, &valid, sizeof(scope));
    ASSERT_EQ(asx_cx_init(&scope.cx, g_rid, ASX_INVALID_ID, ASX_CAP_NONE), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_trace_event_count(&g_rt), events);
    ASSERT_EQ(asx_scope_finish(&valid, &body, NULL), ASX_OK);
    teardown();
}

TEST(scope_finish_poison_and_stale_region_preserve_saved_body_and_budget) {
    asx_scope scope, before;
#ifndef ASX_DEBUG_QUARANTINE
    asx_scope stale;
    asx_budget budget = asx_budget_from_polls(8u);
#endif
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    uint32_t events;
    setup();
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_region_poison(g_rid), ASX_OK);
    memcpy(&before, &scope, sizeof(before));
    events = asx_trace_event_count(&g_rt);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_E_REGION_POISONED);
    ASSERT_EQ(memcmp(&scope, &before, sizeof(scope)), 0);
    ASSERT_EQ(asx_trace_event_count(&g_rt), events);
    teardown();

#ifndef ASX_DEBUG_QUARANTINE
    setup();
    ASSERT_EQ(asx_scope_init(&stale, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(g_rid, &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(g_rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &g_rid), ASX_OK);
    ASSERT_EQ(asx_handle_slot(stale.region_id), asx_handle_slot(g_rid));
    ASSERT_NE(asx_handle_generation(stale.region_id), asx_handle_generation(g_rid));
    ASSERT_EQ(asx_cx_init(&g_cx, g_rid, ASX_INVALID_ID, ASX_CAP_ALL), ASX_OK);
    memcpy(&before, &stale, sizeof(before));
    events = asx_trace_event_count(&g_rt);
    ASSERT_EQ(asx_scope_finish(&stale, &body, NULL), ASX_E_STALE_HANDLE);
    ASSERT_EQ(memcmp(&stale, &before, sizeof(stale)), 0);
    ASSERT_EQ(asx_trace_event_count(&g_rt), events);
    ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    teardown();
#endif
}

typedef struct {
    asx_scope *scope;
    asx_outcome body;
    asx_cancel_reason stronger;
    asx_status same_status;
    asx_status changed_status;
    asx_status reason_status;
    asx_cancel_kind observed_reason;
    int different_first;
    int calls;
    int saved_at_entry;
    asx_outcome_severity body_at_entry;
    uint32_t cancel_events_before;
    uint32_t cancel_events_after;
} finish_reentry_ctx;

static void finalizer_reenter_scope_finish(void *data) {
    finish_reentry_ctx *ctx = (finish_reentry_ctx *)data;
    asx_outcome changed = asx_outcome_make(ASX_OUTCOME_OK);
    asx_cancel_reason observed;
    const asx_cancel_reason *retry_reason =
        ctx->body.severity == ASX_OUTCOME_CANCELLED ? &ctx->stronger : NULL;
    ctx->calls++;
    ctx->saved_at_entry = ctx->scope->finish_started;
    ctx->body_at_entry = ctx->scope->body_outcome.severity;
    ctx->cancel_events_before = scope_cancel_trace_count();
    if (ctx->different_first) {
        ctx->changed_status = asx_scope_finish(ctx->scope, &changed, NULL);
        ctx->same_status = asx_scope_finish(ctx->scope, &ctx->body, retry_reason);
    } else {
        ctx->same_status = asx_scope_finish(ctx->scope, &ctx->body, retry_reason);
        ctx->changed_status = asx_scope_finish(ctx->scope, &changed, NULL);
    }
    ctx->reason_status = asx_region_get_cancel_reason(ctx->scope->region_id, &observed);
    if (ctx->reason_status == ASX_OK) ctx->observed_reason = observed.kind;
    ctx->cancel_events_after = scope_cancel_trace_count();
}

TEST(scope_finish_finalizer_reentry_keeps_first_body_and_cancel_reason) {
    static const asx_outcome_severity severities[] = {ASX_OUTCOME_CANCELLED, ASX_OUTCOME_ERR,
                                                      ASX_OUTCOME_PANICKED};
    uint32_t severity, order;
    for (severity = 0u; severity < 3u; severity++) {
        for (order = 0u; order < 2u; order++) {
            asx_scope scope;
            finish_reentry_ctx ctx;
            asx_cancel_reason first =
                asx_cancel_reason_default(test_runtime(), ASX_CANCEL_USER, "first body");
            asx_cancel_reason observed;
            asx_region_state state;
            setup();
            memset(&ctx, 0, sizeof(ctx));
            ctx.scope = &scope;
            ctx.body = asx_outcome_make(severities[severity]);
            ctx.stronger = asx_cancel_reason_default(&g_rt, ASX_CANCEL_SHUTDOWN, "reentrant retry");
            ctx.different_first = (int)order;
            ASSERT_EQ(asx_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)), ASX_OK);
            ASSERT_EQ(asx_region_defer_sync(g_rid, finalizer_reenter_scope_finish, &ctx), ASX_OK);
            ASSERT_EQ(asx_scope_finish(&scope, &ctx.body,
                                       ctx.body.severity == ASX_OUTCOME_CANCELLED ? &first : NULL),
                      ASX_OK);
            ASSERT_EQ(ctx.calls, 1);
            ASSERT_EQ(ctx.saved_at_entry, 1);
            ASSERT_EQ(ctx.body_at_entry, severities[severity]);
            /* The finalizer is a task of the region, polled by the wait the
             * finish drives: a finish from a task poll is refused, either
             * way, and changes nothing. */
            ASSERT_EQ(ctx.same_status, ASX_E_INVALID_STATE);
            ASSERT_EQ(ctx.changed_status, ASX_E_INVALID_STATE);
            ASSERT_EQ(ctx.cancel_events_before, 1u);
            ASSERT_EQ(ctx.cancel_events_after, ctx.cancel_events_before);
            ASSERT_EQ(ctx.reason_status, ASX_OK);
            ASSERT_EQ(ctx.observed_reason, severity == 0u ? ASX_CANCEL_USER : ASX_CANCEL_FAIL_FAST);
            ASSERT_EQ(scope.finish_started, 1);
            ASSERT_EQ(scope.body_outcome.severity, severities[severity]);
            ASSERT_EQ(asx_region_get_cancel_reason(g_rid, &observed), ASX_OK);
            ASSERT_EQ(observed.kind, severity == 0u ? ASX_CANCEL_USER : ASX_CANCEL_FAIL_FAST);
            if (severity == 0u) ASSERT_STR_EQ(observed.message, "first body");
            ASSERT_EQ(asx_region_get_state(g_rid, &state), ASX_OK);
            ASSERT_EQ(state, ASX_REGION_CLOSED);
            ASSERT_EQ(scope_cancel_trace_count(), 1u);
            ASSERT_EQ(asx_scope_finish(&scope, &ctx.body, NULL), ASX_OK);
            ASSERT_EQ(ctx.calls, 1);
            ASSERT_EQ(scope_cancel_trace_count(), 1u);
            teardown();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_scope ===\n");

    /* Join error strings */
    RUN_TEST(join_error_str_ok);
    RUN_TEST(join_error_str_cancelled);
    RUN_TEST(join_error_str_panicked);
    RUN_TEST(join_error_str_polled_after);

    /* Scope init */
    RUN_TEST(scope_init_null_scope_fails);
    RUN_TEST(scope_init_null_cx_fails);
    RUN_TEST(scope_init_invalid_region_fails);
    RUN_TEST(scope_init_no_spawn_cap_fails);
    RUN_TEST(scope_init_region_mismatch_fails_closed);
    RUN_TEST(scope_init_invalid_cx_fails);
    RUN_TEST(scope_init_success);

    /* Scope spawn */
    RUN_TEST(scope_spawn_null_scope_fails);
    RUN_TEST(scope_spawn_null_poll_fails);
    RUN_TEST(scope_spawn_null_handle_fails);
    RUN_TEST(scope_spawn_zeroed_scope_fails_closed);
    RUN_TEST(scope_spawn_success);
    RUN_TEST(scope_spawn_multiple);
    RUN_TEST(scope_spawn_with_cx_binds_spawned_task);
    RUN_TEST(scope_spawn_with_cx_escalation_fails);
    RUN_TEST(scope_spawn_with_cx_escalation_does_not_spawn_task);
    RUN_TEST(scope_spawned_cx_carries_the_scopes_envelope);

    /* Task handle */
    RUN_TEST(handle_null_returns_invalid);
    RUN_TEST(handle_try_join_null_fails);
    RUN_TEST(handle_try_join_null_out_fails);
    RUN_TEST(handle_not_finished_before_run);

    /* Scope run + join */
    RUN_TEST(scope_run_single_task_completes);
    RUN_TEST(scope_run_multi_poll_task);
    RUN_TEST(scope_run_budget_exhaustion);
    RUN_TEST(scope_run_multiple_tasks);

    /* Double consume guard */
    RUN_TEST(try_join_double_consume_fails);

    /* Abort */
    RUN_TEST(handle_abort_null_fails);
    RUN_TEST(handle_abort_with_kind_null_fails);

    /* Join error classification */
    RUN_TEST(join_error_ok_outcome);
    RUN_TEST(join_error_err_outcome);
    RUN_TEST(join_error_cancelled_outcome);
    RUN_TEST(join_error_panicked_outcome);
    RUN_TEST(join_error_null_returns_ok);

    /* Scope queries */
    RUN_TEST(scope_region_null_returns_invalid);
    RUN_TEST(scope_spawned_count_null_returns_zero);
    RUN_TEST(scope_run_null_fails);
    RUN_TEST(scope_drain_null_fails);
    RUN_TEST(scope_run_zeroed_scope_fails_closed);
    RUN_TEST(scope_drain_zeroed_scope_fails_closed);

    RUN_TEST(scope_finish_ok_preserves_normal_child_result_and_join);
    RUN_TEST(scope_finish_budget_retry_never_cancels_or_reissues_close);
    RUN_TEST(scope_finish_parked_child_resumes_after_explicit_wake);
    RUN_TEST(scope_finish_exception_keeps_body_separate_from_child_outcome);
    RUN_TEST(scope_finish_cancelled_body_preserves_requested_reason);
    RUN_TEST(scope_finish_invalid_body_and_reason_are_failure_atomic);
    RUN_TEST(scope_finish_cancelled_retry_without_reason_does_not_cancel_again);
    RUN_TEST(scope_finish_uninitialized_or_invalid_authority_refuses_before_close);
    RUN_TEST(scope_finish_poison_and_stale_region_preserve_saved_body_and_budget);
    RUN_TEST(scope_finish_finalizer_reentry_keeps_first_body_and_cancel_reason);

    TEST_REPORT();
    return test_failures;
}
