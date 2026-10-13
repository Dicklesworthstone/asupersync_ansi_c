/*
 * test_waker.c — unit tests for wake registration system
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "../../test_runtime.h"
#include <asx/runtime/waker.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static void setup(void) { asx_waker_reset(test_runtime()); }

/* ------------------------------------------------------------------ */
/* Registration tests                                                  */
/* ------------------------------------------------------------------ */

TEST(register_null_out_fails) {
    ASSERT_EQ(asx_waker_register(ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(register_invalid_task_fails) {
    asx_waker w;
    setup();
    ASSERT_EQ(asx_waker_register(ASX_INVALID_ID, &w), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_waker_active_count(test_runtime()), 0u);
}

TEST(register_success) {
    asx_waker w;
    setup();
    ASSERT_EQ(asx_waker_register(42, &w), ASX_OK);
    ASSERT_EQ(asx_waker_active_count(test_runtime()), 1u);
    ASSERT_EQ(asx_waker_task(&w), (asx_task_id)42);
}

TEST(deregister_decrements_count) {
    asx_waker w;
    setup();
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_waker_active_count(test_runtime()), 1u);
    asx_waker_deregister(&w);
    ASSERT_EQ(asx_waker_active_count(test_runtime()), 0u);
}

TEST(deregister_null_safe) { asx_waker_deregister(NULL); /* should not crash */ }

/* ------------------------------------------------------------------ */
/* Signaling tests                                                     */
/* ------------------------------------------------------------------ */

TEST(not_signaled_initially) {
    asx_waker w;
    setup();
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_FALSE(asx_waker_is_signaled(&w));
}

TEST(wake_signals) {
    asx_waker w;
    setup();
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_waker_wake(&w));
    ASSERT_TRUE(asx_waker_is_signaled(&w));
}

TEST(clear_resets_signal) {
    asx_waker w;
    setup();
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_waker_wake(&w));
    asx_waker_clear(&w);
    ASSERT_FALSE(asx_waker_is_signaled(&w));
}

TEST(wake_deregistered_fails) {
    asx_waker w;
    setup();
    MUST_OK(asx_waker_register(1, &w));
    asx_waker_deregister(&w);
    ASSERT_EQ(asx_waker_wake(&w), ASX_E_NOT_FOUND);
}

TEST(wake_null_fails) { ASSERT_EQ(asx_waker_wake(NULL), ASX_E_INVALID_ARGUMENT); }

/* ------------------------------------------------------------------ */
/* Clone tests                                                         */
/* ------------------------------------------------------------------ */

TEST(clone_success) {
    asx_waker w, c;
    setup();
    MUST_OK(asx_waker_register(10, &w));
    MUST_OK(asx_waker_clone(&w, &c));
    ASSERT_EQ(asx_waker_task(&c), (asx_task_id)10);
    /* Signaling clone signals same underlying */
    MUST_OK(asx_waker_wake(&c));
    ASSERT_TRUE(asx_waker_is_signaled(&w));
}

TEST(clone_null_fails) {
    asx_waker c;
    ASSERT_EQ(asx_waker_clone(NULL, &c), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Drain signaled tests                                                */
/* ------------------------------------------------------------------ */

TEST(drain_empty_returns_zero) {
    asx_task_id tasks[4];
    setup();
    ASSERT_EQ(asx_waker_drain_signaled(test_runtime(), tasks, 4), 0u);
}

TEST(drain_collects_signaled) {
    asx_waker w1, w2, w3;
    asx_task_id tasks[4];
    uint32_t count;
    setup();
    MUST_OK(asx_waker_register(10, &w1));
    MUST_OK(asx_waker_register(20, &w2));
    MUST_OK(asx_waker_register(30, &w3));
    MUST_OK(asx_waker_wake(&w1));
    MUST_OK(asx_waker_wake(&w3));
    count = asx_waker_drain_signaled(test_runtime(), tasks, 4);
    ASSERT_EQ(count, 2u);
    /* After drain, signals are cleared */
    ASSERT_FALSE(asx_waker_is_signaled(&w1));
    ASSERT_FALSE(asx_waker_is_signaled(&w3));
}

TEST(drain_uses_signal_order) {
    asx_waker w1, w2, w3;
    asx_task_id tasks[4];
    uint32_t count;
    setup();
    MUST_OK(asx_waker_register(10, &w1));
    MUST_OK(asx_waker_register(20, &w2));
    MUST_OK(asx_waker_register(30, &w3));

    MUST_OK(asx_waker_wake(&w2));
    MUST_OK(asx_waker_wake(&w1));
    MUST_OK(asx_waker_wake(&w3));

    count = asx_waker_drain_signaled(test_runtime(), tasks, 4);
    ASSERT_EQ(count, 3u);
    ASSERT_EQ(tasks[0], (asx_task_id)20);
    ASSERT_EQ(tasks[1], (asx_task_id)10);
    ASSERT_EQ(tasks[2], (asx_task_id)30);
}

TEST(duplicate_wake_keeps_first_signal_order) {
    asx_waker w1, w2;
    asx_task_id tasks[4];
    uint32_t count;
    setup();
    MUST_OK(asx_waker_register(10, &w1));
    MUST_OK(asx_waker_register(20, &w2));

    MUST_OK(asx_waker_wake(&w1));
    MUST_OK(asx_waker_wake(&w2));
    MUST_OK(asx_waker_wake(&w1));

    count = asx_waker_drain_signaled(test_runtime(), tasks, 4);
    ASSERT_EQ(count, 2u);
    ASSERT_EQ(tasks[0], (asx_task_id)10);
    ASSERT_EQ(tasks[1], (asx_task_id)20);
}

TEST(drain_null_returns_zero) {
    ASSERT_EQ(asx_waker_drain_signaled(test_runtime(), NULL, 4), 0u);
    ASSERT_EQ(asx_waker_drain_signaled(NULL, NULL, 4), 0u);
}

/* ------------------------------------------------------------------ */
/* Query null safety                                                   */
/* ------------------------------------------------------------------ */

TEST(signaled_null_false) { ASSERT_FALSE(asx_waker_is_signaled(NULL)); }

TEST(task_null_invalid) { ASSERT_EQ(asx_waker_task(NULL), ASX_INVALID_ID); }

/* ------------------------------------------------------------------ */
/* Runtime tags                                                        */
/* ------------------------------------------------------------------ */

/* A tag no runtime holds (no build has 15 runtimes live here): a task of
 * such a runtime has no arena to register in, and a waker naming one
 * reaches nothing. */
TEST(a_tag_no_runtime_holds_has_no_arena) {
    asx_task_id ghost = asx_handle_with_runtime_tag(10u, 9u);
    asx_waker w;
    asx_waker named;
    asx_task_id tasks[2];

    setup();
    ASSERT_EQ(asx_waker_register(ghost, &w), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_waker_active_count(test_runtime()), 0u);

    MUST_OK(asx_waker_register(1, &w));
    named.slot = w.slot | (9u << 16);
    named.generation = w.generation;
    ASSERT_EQ(asx_waker_wake(&named), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_waker_clone(&named, &w), ASX_E_NOT_FOUND);
    ASSERT_FALSE(asx_waker_is_signaled(&named));
    ASSERT_EQ(asx_waker_task(&named), ASX_INVALID_ID);
    asx_waker_clear(&named);
    asx_waker_deregister(&named);
    ASSERT_EQ(asx_waker_active_count(test_runtime()), 1u);
    ASSERT_EQ(asx_waker_drain_signaled(test_runtime(), tasks, 2), 0u);
}

/* ------------------------------------------------------------------ */
/* Exhaustion                                                          */
/* ------------------------------------------------------------------ */

TEST(arena_exhaustion) {
    asx_waker w;
    uint32_t i;
    asx_status st;
    setup();
    for (i = 0; i < ASX_MAX_WAKERS; i++) {
        st = asx_waker_register((asx_task_id)(i + 1u), &w);
        ASSERT_EQ(st, ASX_OK);
    }
    st = asx_waker_register(999, &w);
    ASSERT_EQ(st, ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_waker ===\n");

    RUN_TEST(register_null_out_fails);
    RUN_TEST(register_invalid_task_fails);
    RUN_TEST(register_success);
    RUN_TEST(deregister_decrements_count);
    RUN_TEST(deregister_null_safe);

    RUN_TEST(not_signaled_initially);
    RUN_TEST(wake_signals);
    RUN_TEST(clear_resets_signal);
    RUN_TEST(wake_deregistered_fails);
    RUN_TEST(wake_null_fails);

    RUN_TEST(clone_success);
    RUN_TEST(clone_null_fails);

    RUN_TEST(drain_empty_returns_zero);
    RUN_TEST(drain_collects_signaled);
    RUN_TEST(drain_uses_signal_order);
    RUN_TEST(duplicate_wake_keeps_first_signal_order);
    RUN_TEST(drain_null_returns_zero);

    RUN_TEST(signaled_null_false);
    RUN_TEST(task_null_invalid);

    RUN_TEST(a_tag_no_runtime_holds_has_no_arena);

    RUN_TEST(arena_exhaustion);

    TEST_REPORT();
    return test_failures;
}
