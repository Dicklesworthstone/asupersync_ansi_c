/*
 * test_scope.c — unit tests for structured concurrency scope
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/cx/scope.h>
#include <asx/runtime/rt.h>
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
    MUST_OK(asx_region_open(&g_rid));
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
    ASSERT_EQ(asx_scope_spawn_with_cx(&scope, ASX_CAP_CLOCK_READ | ASX_CAP_CANCEL_CHECK,
                                      poll_immediate_ok, NULL, &h, &child_cx),
              ASX_OK);
    ASSERT_EQ(asx_cx_region(&child_cx), g_rid);
    ASSERT_EQ(asx_cx_task(&child_cx), h.task_id);
    ASSERT_TRUE(asx_cx_has_cap(&child_cx, ASX_CAP_CLOCK_READ));
    ASSERT_TRUE(asx_cx_has_cap(&child_cx, ASX_CAP_CANCEL_CHECK));
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
    RUN_TEST(scope_spawn_success);
    RUN_TEST(scope_spawn_multiple);
    RUN_TEST(scope_spawn_with_cx_binds_spawned_task);
    RUN_TEST(scope_spawn_with_cx_escalation_fails);
    RUN_TEST(scope_spawn_with_cx_escalation_does_not_spawn_task);

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

    TEST_REPORT();
    return test_failures;
}
