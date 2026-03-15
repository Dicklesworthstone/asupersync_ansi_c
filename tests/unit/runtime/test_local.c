/*
 * test_local.c — unit tests for current-thread local task wrappers
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/local.h>
#include <asx/runtime/rt.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static asx_runtime g_rt;
static asx_region_id g_rid;
static asx_cx g_cx;
static int g_poll_count;

static asx_status poll_immediate_ok(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_count_twice(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    g_poll_count++;
    if (g_poll_count < 2) return ASX_E_PENDING;
    return ASX_OK;
}

static void setup(void) {
    MUST_OK(asx_runtime_init_default(&g_rt));
    MUST_OK(asx_region_open(&g_rid));
    MUST_OK(asx_cx_init(&g_cx, g_rid, ASX_INVALID_ID, ASX_CAP_ALL));
    g_poll_count = 0;
}

static void teardown(void) { asx_runtime_shutdown(&g_rt); }

TEST(local_scope_init_null_scope_fails) {
    setup();
    ASSERT_EQ(asx_local_scope_init(NULL, g_rid, &g_cx, asx_budget_infinite()),
              ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(local_scope_init_success) {
    asx_local_scope scope;
    setup();
    ASSERT_EQ(asx_local_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()), ASX_OK);
    ASSERT_EQ(asx_local_scope_region(&scope), g_rid);
    ASSERT_EQ(asx_local_scope_spawned_count(&scope), 0u);
    teardown();
}

TEST(local_scope_spawn_success) {
    asx_local_scope scope;
    asx_local_task_handle handle;
    setup();
    MUST_OK(asx_local_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_local_scope_spawn(&scope, poll_immediate_ok, NULL, &handle), ASX_OK);
    ASSERT_NE(asx_local_task_handle_task_id(&handle), ASX_INVALID_ID);
    ASSERT_EQ(asx_local_scope_spawned_count(&scope), 1u);
    teardown();
}

TEST(local_scope_spawn_with_cx_binds_child) {
    asx_local_scope scope;
    asx_local_task_handle handle;
    asx_cx child_cx;
    setup();
    MUST_OK(asx_local_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_local_scope_spawn_with_cx(&scope, ASX_CAP_CLOCK_READ | ASX_CAP_CANCEL_CHECK,
                                            poll_immediate_ok, NULL, &handle, &child_cx),
              ASX_OK);
    ASSERT_EQ(asx_cx_region(&child_cx), g_rid);
    ASSERT_EQ(asx_cx_task(&child_cx), asx_local_task_handle_task_id(&handle));
    ASSERT_TRUE(asx_cx_has_cap(&child_cx, ASX_CAP_CLOCK_READ));
    ASSERT_FALSE(asx_cx_has_cap(&child_cx, ASX_CAP_SPAWN));
    teardown();
}

TEST(local_scope_run_and_join) {
    asx_local_scope scope;
    asx_local_task_handle handle;
    asx_outcome out;
    setup();
    MUST_OK(asx_local_scope_init(&scope, g_rid, &g_cx, asx_budget_from_polls(8u)));
    MUST_OK(asx_local_scope_spawn(&scope, poll_count_twice, NULL, &handle));
    ASSERT_EQ(asx_local_scope_run(&scope), ASX_OK);
    ASSERT_TRUE(asx_local_task_handle_is_finished(&handle));
    ASSERT_EQ(asx_local_task_handle_try_join(&handle, &out), ASX_OK);
    ASSERT_EQ(asx_local_task_handle_join_error(&out), ASX_JOIN_OK);
    teardown();
}

TEST(local_scope_spawn_captured_success) {
    asx_local_scope scope;
    asx_local_task_handle handle;
    void *state;
    setup();
    MUST_OK(asx_local_scope_init(&scope, g_rid, &g_cx, asx_budget_infinite()));
    ASSERT_EQ(asx_local_scope_spawn_captured(&scope, poll_immediate_ok, 16u, NULL, &handle, &state),
              ASX_OK);
    ASSERT_TRUE(state != NULL);
    ASSERT_EQ(asx_local_scope_spawned_count(&scope), 1u);
    teardown();
}

TEST(local_handle_abort_null_fails) {
    ASSERT_EQ(asx_local_task_handle_abort(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(local_handle_try_join_null_fails) {
    asx_local_task_handle handle;
    memset(&handle, 0, sizeof(handle));
    ASSERT_EQ(asx_local_task_handle_try_join(NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_local_task_handle_try_join(&handle, NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== test_local ===\n");
    RUN_TEST(local_scope_init_null_scope_fails);
    RUN_TEST(local_scope_init_success);
    RUN_TEST(local_scope_spawn_success);
    RUN_TEST(local_scope_spawn_with_cx_binds_child);
    RUN_TEST(local_scope_run_and_join);
    RUN_TEST(local_scope_spawn_captured_success);
    RUN_TEST(local_handle_abort_null_fails);
    RUN_TEST(local_handle_try_join_null_fails);
    TEST_REPORT();
    return test_failures;
}
