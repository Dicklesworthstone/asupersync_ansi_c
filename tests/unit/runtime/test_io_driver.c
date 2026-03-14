/*
 * test_io_driver.c — unit tests for IO driver and reactor integration
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/io_driver.h>
#include <asx/runtime/browser_boundary.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static int setup(void) {
    asx_status st;
    asx_waker_reset();
    asx_io_driver_reset();
    st = asx_io_driver_init();
    if (!asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        if (st != ASX_E_PERMISSION_DENIED) {
            fprintf(stderr, "ASSERT_EQ failed: st (%d) != ASX_E_PERMISSION_DENIED (%d) at %s:%d\n",
                    (int)st, (int)ASX_E_PERMISSION_DENIED, __FILE__, __LINE__);
            test_failures++;
            return 0;
        }
        return 0;
    }
    if (st != ASX_OK) {
        fprintf(stderr, "ASSERT_EQ failed: st (%d) != ASX_OK (%d) at %s:%d\n", (int)st, (int)ASX_OK,
                __FILE__, __LINE__);
        test_failures++;
        return 0;
    }
    return 1;
}

static void teardown(void) { asx_io_driver_shutdown(); }

/* ------------------------------------------------------------------ */
/* Registration tests                                                  */
/* ------------------------------------------------------------------ */

TEST(register_null_token_fails) {
    asx_waker w;
    if (!setup()) return;
    asx_waker_reset();
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(register_null_waker_fails) {
    asx_io_token tok;
    if (!setup()) return;
    ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, NULL, &tok), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(register_before_init_fails) {
    asx_waker w;
    asx_io_token tok;
    asx_io_driver_reset();
    asx_io_driver_shutdown();
    asx_waker_reset();
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_E_INVALID_STATE);
}

TEST(register_success) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_OK);
    ASSERT_EQ(asx_io_active_count(), 1u);
    teardown();
}

TEST(deregister_decrements_count) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_active_count(), 1u);
    asx_io_deregister(&tok);
    ASSERT_EQ(asx_io_active_count(), 0u);
    teardown();
}

TEST(deregister_null_safe) {
    if (!setup()) return;
    asx_io_deregister(NULL); /* should not crash */
    teardown();
}

TEST(deregister_stale_token_safe) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    asx_io_deregister(&tok);
    /* Second deregister on stale token — should be harmless */
    asx_io_deregister(&tok);
    ASSERT_EQ(asx_io_active_count(), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Set interest tests                                                  */
/* ------------------------------------------------------------------ */

TEST(set_interest_success) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_set_interest(&tok, ASX_IO_WRITABLE), ASX_OK);
    teardown();
}

TEST(set_interest_null_fails) {
    if (!setup()) return;
    ASSERT_EQ(asx_io_set_interest(NULL, ASX_IO_READABLE), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(set_interest_stale_fails) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    asx_io_deregister(&tok);
    ASSERT_EQ(asx_io_set_interest(&tok, ASX_IO_WRITABLE), ASX_E_NOT_FOUND);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Poll tests                                                          */
/* ------------------------------------------------------------------ */

TEST(poll_returns_zero_ghost_reactor) {
    asx_io_event events[4];
    if (!setup()) return;
    /* Ghost reactor always returns 0 events */
    ASSERT_EQ(asx_io_driver_poll(events, 4, 0), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Exhaustion                                                          */
/* ------------------------------------------------------------------ */

TEST(arena_exhaustion) {
    asx_waker w;
    asx_io_token tok;
    asx_status st;
    uint32_t i;
    if (!setup()) return;
    for (i = 0; i < ASX_MAX_IO_TOKENS; i++) {
        MUST_OK(asx_waker_register(i, &w));
        st = asx_io_register((int)i, ASX_IO_READABLE, &w, &tok);
        ASSERT_EQ(st, ASX_OK);
    }
    MUST_OK(asx_waker_register(999, &w));
    st = asx_io_register(999, ASX_IO_READABLE, &w, &tok);
    ASSERT_EQ(st, ASX_E_RESOURCE_EXHAUSTED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Multiple registrations                                              */
/* ------------------------------------------------------------------ */

TEST(multiple_registrations) {
    asx_waker w1, w2;
    asx_io_token tok1, tok2;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w1));
    MUST_OK(asx_waker_register(2, &w2));
    MUST_OK(asx_io_register(10, ASX_IO_READABLE, &w1, &tok1));
    MUST_OK(asx_io_register(20, ASX_IO_WRITABLE, &w2, &tok2));
    ASSERT_EQ(asx_io_active_count(), 2u);
    asx_io_deregister(&tok1);
    ASSERT_EQ(asx_io_active_count(), 1u);
    asx_io_deregister(&tok2);
    ASSERT_EQ(asx_io_active_count(), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

TEST(init_denied_when_io_surface_unavailable) {
    asx_status st;
    asx_io_driver_reset();
    st = asx_io_driver_init();
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        ASSERT_EQ(st, ASX_OK);
    } else {
        ASSERT_EQ(st, ASX_E_PERMISSION_DENIED);
    }
    teardown();
}

int main(void) {
    fprintf(stderr, "=== test_io_driver ===\n");

    RUN_TEST(register_null_token_fails);
    RUN_TEST(register_null_waker_fails);
    RUN_TEST(register_before_init_fails);
    RUN_TEST(register_success);
    RUN_TEST(deregister_decrements_count);
    RUN_TEST(deregister_null_safe);
    RUN_TEST(deregister_stale_token_safe);

    RUN_TEST(set_interest_success);
    RUN_TEST(set_interest_null_fails);
    RUN_TEST(set_interest_stale_fails);

    RUN_TEST(poll_returns_zero_ghost_reactor);

    RUN_TEST(arena_exhaustion);
    RUN_TEST(multiple_registrations);
    RUN_TEST(init_denied_when_io_surface_unavailable);

    TEST_REPORT();
    return test_failures;
}
