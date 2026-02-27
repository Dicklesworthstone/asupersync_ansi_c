/*
 * test_cleanup.c — unit tests for deterministic cleanup stack
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/core/cleanup.h>

/* ---- Test helpers ---- */

static int g_call_order[ASX_CLEANUP_STACK_CAPACITY];
static int g_call_count;

static void reset_tracker(void)
{
    int i;
    g_call_count = 0;
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        g_call_order[i] = -1;
    }
}

static void track_cleanup(void *user_data)
{
    int id = *(int *)user_data;
    if (g_call_count < ASX_CLEANUP_STACK_CAPACITY) {
        g_call_order[g_call_count++] = id;
    }
}

static void increment_counter(void *user_data)
{
    int *counter = (int *)user_data;
    (*counter)++;
}

/* ---- Tests ---- */

TEST(cleanup_init_empty) {
    asx_cleanup_stack s;
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)0);
}

TEST(cleanup_push_and_pending) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)1);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)2);
}

TEST(cleanup_push_null_args) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(NULL, increment_counter, NULL, &h),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cleanup_push(&s, NULL, NULL, &h),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(cleanup_pop_resolves) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)1);
    ASSERT_EQ(asx_cleanup_pop(&s, h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)0);
}

TEST(cleanup_pop_invalid_handle) {
    asx_cleanup_stack s;
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_pop(&s, 0), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_cleanup_pop(&s, ASX_CLEANUP_INVALID), ASX_E_NOT_FOUND);
}

TEST(cleanup_pop_double_pop) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pop(&s, h), ASX_OK);
    ASSERT_EQ(asx_cleanup_pop(&s, h), ASX_E_NOT_FOUND);
}

TEST(cleanup_drain_lifo_order) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    int ids[3] = {10, 20, 30};

    reset_tracker();
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, track_cleanup, &ids[0], &h), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&s, track_cleanup, &ids[1], &h), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&s, track_cleanup, &ids[2], &h), ASX_OK);

    asx_cleanup_drain(&s);

    ASSERT_EQ(g_call_count, 3);
    /* LIFO: last pushed (30) called first, then 20, then 10 */
    ASSERT_EQ(g_call_order[0], 30);
    ASSERT_EQ(g_call_order[1], 20);
    ASSERT_EQ(g_call_order[2], 10);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)0);
}

TEST(cleanup_drain_skips_popped) {
    asx_cleanup_stack s;
    asx_cleanup_handle h0, h1, h2;
    int ids[3] = {10, 20, 30};

    reset_tracker();
    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, track_cleanup, &ids[0], &h0), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&s, track_cleanup, &ids[1], &h1), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&s, track_cleanup, &ids[2], &h2), ASX_OK);

    /* Pop the middle entry */
    ASSERT_EQ(asx_cleanup_pop(&s, h1), ASX_OK);

    asx_cleanup_drain(&s);

    ASSERT_EQ(g_call_count, 2);
    ASSERT_EQ(g_call_order[0], 30);
    ASSERT_EQ(g_call_order[1], 10);
}

TEST(cleanup_drain_idempotent) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    int counter = 0;

    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, &counter, &h), ASX_OK);

    asx_cleanup_drain(&s);
    ASSERT_EQ(counter, 1);

    /* Second drain is a no-op (drained flag set) */
    asx_cleanup_drain(&s);
    ASSERT_EQ(counter, 1);
}

TEST(cleanup_drain_empty_is_noop) {
    asx_cleanup_stack s;
    asx_cleanup_init(&s);
    /* Should not crash */
    asx_cleanup_drain(&s);
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)0);
}

TEST(cleanup_drain_null_is_safe) {
    /* Should not crash */
    asx_cleanup_drain(NULL);
}

TEST(cleanup_capacity_exhaustion) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    uint32_t i;

    asx_cleanup_init(&s);
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, &h), ASX_OK);
    }
    ASSERT_EQ(asx_cleanup_pending(&s), (uint32_t)ASX_CLEANUP_STACK_CAPACITY);

    /* One more should fail */
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, NULL, &h),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(cleanup_pending_null) {
    ASSERT_EQ(asx_cleanup_pending(NULL), (uint32_t)0);
}

TEST(cleanup_init_null_is_safe) {
    /* Should not crash */
    asx_cleanup_init(NULL);
}

TEST(cleanup_user_data_passed_through) {
    asx_cleanup_stack s;
    asx_cleanup_handle h;
    int counter = 0;

    asx_cleanup_init(&s);
    ASSERT_EQ(asx_cleanup_push(&s, increment_counter, &counter, &h), ASX_OK);
    asx_cleanup_drain(&s);
    ASSERT_EQ(counter, 1);
}

TEST(cleanup_pop_null_stack) {
    ASSERT_EQ(asx_cleanup_pop(NULL, 0), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== test_cleanup ===\n");
    RUN_TEST(cleanup_init_empty);
    RUN_TEST(cleanup_push_and_pending);
    RUN_TEST(cleanup_push_null_args);
    RUN_TEST(cleanup_pop_resolves);
    RUN_TEST(cleanup_pop_invalid_handle);
    RUN_TEST(cleanup_pop_double_pop);
    RUN_TEST(cleanup_drain_lifo_order);
    RUN_TEST(cleanup_drain_skips_popped);
    RUN_TEST(cleanup_drain_idempotent);
    RUN_TEST(cleanup_drain_empty_is_noop);
    RUN_TEST(cleanup_drain_null_is_safe);
    RUN_TEST(cleanup_capacity_exhaustion);
    RUN_TEST(cleanup_pending_null);
    RUN_TEST(cleanup_init_null_is_safe);
    RUN_TEST(cleanup_user_data_passed_through);
    RUN_TEST(cleanup_pop_null_stack);
    TEST_REPORT();
    return test_failures;
}
