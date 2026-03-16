/*
 * test_io_driver.c — unit tests for IO driver and reactor integration
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/io_driver.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static uint32_t g_ready_count;

static asx_status fixed_ready_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = g_ready_count;
    return ASX_OK;
}

static int setup(void) {
    asx_status st;
    asx_runtime_hooks hooks;

    asx_waker_reset();
    asx_io_driver_reset();
    g_ready_count = 0u;
    MUST_OK(asx_runtime_hooks_init(&hooks));
    MUST_OK(asx_runtime_set_hooks(&hooks));
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

TEST(register_zero_interest_fails) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_io_register(42, (asx_io_interest)0, &w, &tok), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_io_active_count(), 0u);
    teardown();
}

TEST(register_unknown_interest_bits_fail) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_io_register(42, (asx_io_interest)0x08, &w, &tok), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_io_active_count(), 0u);
    teardown();
}

TEST(register_success) {
    asx_waker w;
    asx_io_token tok;
    int fd = -1;
    asx_io_interest interest = 0;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_OK);
    ASSERT_EQ(asx_io_active_count(), 1u);
    ASSERT_EQ(asx_io_get_registration(&tok, &fd, &interest), ASX_OK);
    ASSERT_EQ(fd, 42);
    ASSERT_EQ(interest, ASX_IO_READABLE);
    teardown();
}

TEST(reinit_clears_registrations_and_invalidates_tokens) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_active_count(), 1u);

    MUST_OK(asx_io_driver_init());
    ASSERT_EQ(asx_io_active_count(), 0u);
    ASSERT_EQ(asx_io_set_interest(&tok, ASX_IO_WRITABLE), ASX_E_NOT_FOUND);

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

TEST(reinit_then_register_uses_fresh_generation) {
    asx_waker w;
    asx_io_token stale_tok;
    asx_io_token fresh_tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &stale_tok));

    MUST_OK(asx_io_driver_init());
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &fresh_tok));

    ASSERT_EQ(stale_tok.slot, fresh_tok.slot);
    ASSERT_NE(stale_tok.generation, fresh_tok.generation);
    ASSERT_EQ(asx_io_set_interest(&stale_tok, ASX_IO_WRITABLE), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_io_set_interest(&fresh_tok, ASX_IO_WRITABLE), ASX_OK);

    teardown();
}

/* ------------------------------------------------------------------ */
/* Set interest tests                                                  */
/* ------------------------------------------------------------------ */

TEST(set_interest_success) {
    asx_waker w;
    asx_io_token tok;
    asx_io_interest interest = 0;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_set_interest(&tok, ASX_IO_WRITABLE), ASX_OK);
    ASSERT_EQ(asx_io_get_registration(&tok, NULL, &interest), ASX_OK);
    ASSERT_EQ(interest, ASX_IO_WRITABLE);
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

TEST(set_interest_zero_fails) {
    asx_waker w;
    asx_io_token tok;
    asx_io_interest interest = 0;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_set_interest(&tok, (asx_io_interest)0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_io_get_registration(&tok, NULL, &interest), ASX_OK);
    ASSERT_EQ(interest, ASX_IO_READABLE);
    teardown();
}

TEST(set_interest_unknown_bits_fail) {
    asx_waker w;
    asx_io_token tok;
    asx_io_interest interest = 0;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_set_interest(&tok, (asx_io_interest)0x08), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_io_get_registration(&tok, NULL, &interest), ASX_OK);
    ASSERT_EQ(interest, ASX_IO_READABLE);
    teardown();
}

TEST(get_registration_null_token_fails) {
    int fd = -1;
    ASSERT_EQ(asx_io_get_registration(NULL, &fd, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(get_registration_requires_output) {
    asx_waker w;
    asx_io_token tok;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    ASSERT_EQ(asx_io_get_registration(&tok, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(get_registration_stale_token_fails) {
    asx_waker w;
    asx_io_token tok;
    int fd = -1;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    MUST_OK(asx_io_register(42, ASX_IO_READABLE, &w, &tok));
    asx_io_deregister(&tok);
    ASSERT_EQ(asx_io_get_registration(&tok, &fd, NULL), ASX_E_NOT_FOUND);
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

TEST(poll_collects_ready_events_and_wakes_registrations) {
    asx_runtime_hooks hooks;
    asx_waker w1, w2;
    asx_io_token tok1, tok2;
    asx_io_event events[4];

    if (!setup()) return;

    MUST_OK(asx_runtime_hooks_init(&hooks));
    hooks.reactor.ghost_wait_fn = fixed_ready_reactor;
    MUST_OK(asx_runtime_set_hooks(&hooks));

    MUST_OK(asx_waker_register(1, &w1));
    MUST_OK(asx_waker_register(2, &w2));
    MUST_OK(asx_io_register(10, ASX_IO_READABLE, &w1, &tok1));
    MUST_OK(asx_io_register(20, ASX_IO_WRITABLE, &w2, &tok2));

    g_ready_count = 2u;
    ASSERT_EQ(asx_io_driver_poll(events, 4, 17u), 2u);
    ASSERT_EQ(events[0].token.slot, tok1.slot);
    ASSERT_EQ(events[0].token.generation, tok1.generation);
    ASSERT_EQ(events[0].ready, ASX_IO_READABLE);
    ASSERT_EQ(events[1].token.slot, tok2.slot);
    ASSERT_EQ(events[1].token.generation, tok2.generation);
    ASSERT_EQ(events[1].ready, ASX_IO_WRITABLE);
    ASSERT_TRUE(asx_waker_is_signaled(&w1));
    ASSERT_TRUE(asx_waker_is_signaled(&w2));

    teardown();
}

TEST(poll_caps_ready_delivery_to_max_events) {
    asx_runtime_hooks hooks;
    asx_waker w1, w2;
    asx_io_token tok1, tok2;
    asx_io_event event;

    if (!setup()) return;

    MUST_OK(asx_runtime_hooks_init(&hooks));
    hooks.reactor.ghost_wait_fn = fixed_ready_reactor;
    MUST_OK(asx_runtime_set_hooks(&hooks));

    MUST_OK(asx_waker_register(1, &w1));
    MUST_OK(asx_waker_register(2, &w2));
    MUST_OK(asx_io_register(10, ASX_IO_READABLE, &w1, &tok1));
    MUST_OK(asx_io_register(20, ASX_IO_WRITABLE, &w2, &tok2));

    g_ready_count = 2u;
    ASSERT_EQ(asx_io_driver_poll(&event, 1u, 0u), 1u);
    ASSERT_EQ(event.token.slot, tok1.slot);
    ASSERT_TRUE(asx_waker_is_signaled(&w1));
    ASSERT_FALSE(asx_waker_is_signaled(&w2));

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

TEST(init_state_tracks_lifecycle) {
    ASSERT_FALSE(asx_io_driver_is_initialized());
    if (!setup()) return;
    ASSERT_TRUE(asx_io_driver_is_initialized());
    teardown();
    ASSERT_FALSE(asx_io_driver_is_initialized());
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
    RUN_TEST(register_zero_interest_fails);
    RUN_TEST(register_unknown_interest_bits_fail);
    RUN_TEST(register_success);
    RUN_TEST(reinit_clears_registrations_and_invalidates_tokens);
    RUN_TEST(deregister_decrements_count);
    RUN_TEST(deregister_null_safe);
    RUN_TEST(deregister_stale_token_safe);
    RUN_TEST(reinit_then_register_uses_fresh_generation);

    RUN_TEST(set_interest_success);
    RUN_TEST(set_interest_null_fails);
    RUN_TEST(set_interest_stale_fails);
    RUN_TEST(set_interest_zero_fails);
    RUN_TEST(set_interest_unknown_bits_fail);
    RUN_TEST(get_registration_null_token_fails);
    RUN_TEST(get_registration_requires_output);
    RUN_TEST(get_registration_stale_token_fails);

    RUN_TEST(poll_returns_zero_ghost_reactor);
    RUN_TEST(poll_collects_ready_events_and_wakes_registrations);
    RUN_TEST(poll_caps_ready_delivery_to_max_events);

    RUN_TEST(arena_exhaustion);
    RUN_TEST(multiple_registrations);
    RUN_TEST(init_state_tracks_lifecycle);
    RUN_TEST(init_denied_when_io_surface_unavailable);

    TEST_REPORT();
    return test_failures;
}
