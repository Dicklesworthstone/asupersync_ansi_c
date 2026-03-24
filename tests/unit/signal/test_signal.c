/*
 * test_signal.c — unit tests for deterministic signal host surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/signal/signal.h>
#include <asx/runtime/browser_boundary.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES

TEST(subscribe_raise_poll) {
    asx_signal_subscription subscription;
    uint32_t count = 0u;
    asx_signal_reset();
    ASSERT_EQ(asx_signal_subscribe(&subscription, ASX_SIGNAL_HUP), ASX_OK);
    ASSERT_EQ(asx_signal_raise(ASX_SIGNAL_HUP), ASX_OK);
    ASSERT_EQ(asx_signal_poll(subscription, &count), ASX_OK);
    ASSERT_EQ(count, 1u);
}

TEST(poll_without_signal_is_pending) {
    asx_signal_subscription subscription;
    uint32_t count = 0u;
    asx_signal_reset();
    ASSERT_EQ(asx_signal_subscribe(&subscription, ASX_SIGNAL_USR1), ASX_OK);
    ASSERT_EQ(asx_signal_poll(subscription, &count), ASX_E_PENDING);
}

TEST(term_sets_shutdown_requested) {
    asx_signal_subscription subscription;
    uint32_t count = 0u;
    asx_signal_reset();
    ASSERT_EQ(asx_signal_subscribe(&subscription, ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_FALSE(asx_signal_shutdown_requested());
    ASSERT_EQ(asx_signal_raise(ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_TRUE(asx_signal_shutdown_requested());
    ASSERT_EQ(asx_signal_poll(subscription, &count), ASX_OK);
    ASSERT_EQ(count, 1u);
    asx_signal_clear_shutdown();
    ASSERT_FALSE(asx_signal_shutdown_requested());
}

TEST(unsubscribe_invalidates_handle) {
    asx_signal_subscription subscription;
    uint32_t count = 0u;
    asx_signal_reset();
    ASSERT_EQ(asx_signal_subscribe(&subscription, ASX_SIGNAL_INT), ASX_OK);
    ASSERT_EQ(asx_signal_unsubscribe(subscription), ASX_OK);
    ASSERT_EQ(asx_signal_poll(subscription, &count), ASX_E_NOT_FOUND);
}

TEST(capacity_is_enforced) {
    asx_signal_subscription subscriptions[ASX_MAX_SIGNAL_SUBSCRIPTIONS];
    asx_signal_subscription extra;
    uint32_t i;
    asx_signal_reset();
    for (i = 0; i < ASX_MAX_SIGNAL_SUBSCRIPTIONS; i++) {
        ASSERT_EQ(asx_signal_subscribe(&subscriptions[i], ASX_SIGNAL_USR1), ASX_OK);
    }
    ASSERT_EQ(asx_signal_subscribe(&extra, ASX_SIGNAL_USR1), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(reset_clears_pending_and_shutdown) {
    asx_signal_subscription subscription;
    uint32_t count = 0u;
    asx_signal_reset();
    ASSERT_EQ(asx_signal_subscribe(&subscription, ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_EQ(asx_signal_raise(ASX_SIGNAL_TERM), ASX_OK);
    ASSERT_TRUE(asx_signal_shutdown_requested());
    asx_signal_reset();
    ASSERT_FALSE(asx_signal_shutdown_requested());
    ASSERT_EQ(asx_signal_poll(subscription, &count), ASX_E_NOT_FOUND);
}

#else

TEST(signal_surface_compile_time_hidden_in_browser) {
    ASSERT_EQ(ASX_HAS_NATIVE_RUNTIME_SURFACES, 0);
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_SIGNAL));
    ASSERT_EQ(asx_surface_gate(ASX_SURFACE_SIGNAL), ASX_E_PERMISSION_DENIED);
}

#endif

int main(void) {
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    RUN_TEST(subscribe_raise_poll);
    RUN_TEST(poll_without_signal_is_pending);
    RUN_TEST(term_sets_shutdown_requested);
    RUN_TEST(unsubscribe_invalidates_handle);
    RUN_TEST(capacity_is_enforced);
    RUN_TEST(reset_clears_pending_and_shutdown);
#else
    RUN_TEST(signal_surface_compile_time_hidden_in_browser);
#endif
    TEST_REPORT();
    return test_failures;
}
