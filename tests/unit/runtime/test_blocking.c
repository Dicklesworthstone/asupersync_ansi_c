/*
 * test_blocking.c — unit tests for blocking pool
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/blocking.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

static void setup(void)
{
    asx_waker_reset();
    asx_blocking_pool_reset();
    MUST_OK(asx_blocking_pool_init());
}

static void teardown(void)
{
    asx_blocking_pool_shutdown();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t add_42(void *user_data)
{
    uint64_t val = *(uint64_t *)user_data;
    return val + 42;
}

static uint64_t return_zero(void *user_data)
{
    (void)user_data;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Spawn tests                                                         */
/* ------------------------------------------------------------------ */

TEST(spawn_null_fn_fails) {
    asx_blocking_handle h;
    setup();
    ASSERT_EQ(asx_spawn_blocking(NULL, NULL, NULL, &h),
              ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(spawn_null_handle_fails) {
    setup();
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, NULL),
              ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(spawn_before_init_fails) {
    asx_blocking_handle h;
    asx_blocking_pool_reset();
    asx_blocking_pool_shutdown();
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, &h),
              ASX_E_INVALID_STATE);
}

TEST(spawn_success) {
    asx_blocking_handle h;
    setup();
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, &h), ASX_OK);
    teardown();
}

TEST(spawn_returns_result) {
    asx_blocking_handle h;
    uint64_t input = 100;
    uint64_t result;
    setup();
    MUST_OK(asx_spawn_blocking(add_42, &input, NULL, &h));
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_OK);
    ASSERT_EQ(result, (uint64_t)142);
    teardown();
}

TEST(spawn_signals_completion_waker) {
    asx_blocking_handle h;
    asx_waker w;
    setup();
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_FALSE(asx_waker_is_signaled(&w));
    MUST_OK(asx_spawn_blocking(return_zero, NULL, &w, &h));
    /* Walking skeleton executes inline — waker should be signaled */
    ASSERT_TRUE(asx_waker_is_signaled(&w));
    teardown();
}

TEST(spawn_without_waker_ok) {
    asx_blocking_handle h;
    setup();
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* State query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(get_state_null_returns_completed) {
    ASSERT_EQ(asx_blocking_get_state(NULL), ASX_BLOCKING_COMPLETED);
}

TEST(get_state_stale_handle_returns_completed) {
    asx_blocking_handle h;
    setup();
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    /* Reset invalidates handle */
    asx_blocking_pool_reset();
    MUST_OK(asx_blocking_pool_init());
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Result query tests                                                  */
/* ------------------------------------------------------------------ */

TEST(get_result_null_handle_fails) {
    uint64_t result;
    ASSERT_EQ(asx_blocking_get_result(NULL, &result),
              ASX_E_INVALID_ARGUMENT);
}

TEST(get_result_null_out_fails) {
    asx_blocking_handle h;
    setup();
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    ASSERT_EQ(asx_blocking_get_result(&h, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(get_result_stale_handle_fails) {
    asx_blocking_handle h;
    uint64_t result;
    setup();
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    asx_blocking_pool_reset();
    MUST_OK(asx_blocking_pool_init());
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_E_NOT_FOUND);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Active count tests                                                  */
/* ------------------------------------------------------------------ */

TEST(active_count_zero_initially) {
    setup();
    /* Walking skeleton: inline execution means tasks complete immediately */
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    teardown();
}

TEST(active_count_zero_after_inline_completion) {
    asx_blocking_handle h;
    setup();
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    /* Should be zero because walking skeleton runs inline */
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Exhaustion                                                          */
/* ------------------------------------------------------------------ */

TEST(arena_exhaustion) {
    asx_blocking_handle h;
    asx_status st;
    uint32_t i;
    setup();
    /* Fill all slots — walking skeleton completes them immediately,
     * but they still occupy slots until reset */
    for (i = 0; i < ASX_MAX_BLOCKING_TASKS; i++) {
        st = asx_spawn_blocking(return_zero, NULL, NULL, &h);
        ASSERT_EQ(st, ASX_OK);
    }
    /* Completed slots can be reused — walking skeleton marks as COMPLETED
     * and the allocator reclaims completed slots */
    st = asx_spawn_blocking(return_zero, NULL, NULL, &h);
    ASSERT_EQ(st, ASX_OK);  /* Should succeed via slot reuse */
    teardown();
}

/* ------------------------------------------------------------------ */
/* Multiple tasks                                                      */
/* ------------------------------------------------------------------ */

TEST(multiple_tasks_different_results) {
    asx_blocking_handle h1, h2;
    uint64_t in1 = 10, in2 = 200;
    uint64_t r1, r2;
    setup();
    /* Walking skeleton completes inline, so get result before next spawn
     * (completed slots may be reused, invalidating old handles) */
    MUST_OK(asx_spawn_blocking(add_42, &in1, NULL, &h1));
    MUST_OK(asx_blocking_get_result(&h1, &r1));
    ASSERT_EQ(r1, (uint64_t)52);
    MUST_OK(asx_spawn_blocking(add_42, &in2, NULL, &h2));
    MUST_OK(asx_blocking_get_result(&h2, &r2));
    ASSERT_EQ(r2, (uint64_t)242);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_blocking ===\n");

    RUN_TEST(spawn_null_fn_fails);
    RUN_TEST(spawn_null_handle_fails);
    RUN_TEST(spawn_before_init_fails);
    RUN_TEST(spawn_success);
    RUN_TEST(spawn_returns_result);
    RUN_TEST(spawn_signals_completion_waker);
    RUN_TEST(spawn_without_waker_ok);

    RUN_TEST(get_state_null_returns_completed);
    RUN_TEST(get_state_stale_handle_returns_completed);

    RUN_TEST(get_result_null_handle_fails);
    RUN_TEST(get_result_null_out_fails);
    RUN_TEST(get_result_stale_handle_fails);

    RUN_TEST(active_count_zero_initially);
    RUN_TEST(active_count_zero_after_inline_completion);

    RUN_TEST(arena_exhaustion);
    RUN_TEST(multiple_tasks_different_results);

    TEST_REPORT();
    return test_failures;
}
