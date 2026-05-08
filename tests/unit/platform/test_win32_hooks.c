/*
 * test_win32_hooks.c - unit/compile checks for Win32 platform hooks
 *
 * The default host lane verifies the public capability contract without
 * needing Windows headers.  The PROFILE=WIN32 cross lane builds the live
 * hook installation path and asserts unsupported surfaces stay fail-closed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"

#include <asx/asx.h>
#include <asx/platform/win32.h>

TEST(win32_capability_contract_names_supported_and_deferred_surfaces) {
    ASSERT_EQ(ASX_WIN32_HAS_QPC_CLOCK, 1u);
    ASSERT_EQ(ASX_WIN32_HAS_BCRYPT_ENTROPY, 1u);
    ASSERT_EQ(ASX_WIN32_HAS_TIMED_REACTOR_WAIT, 1u);
    ASSERT_EQ(ASX_WIN32_HAS_SOCKET_REACTOR_REGISTRATION, 0u);
    ASSERT_EQ(ASX_WIN32_HAS_IOCP_REACTOR, 0u);
    ASSERT_EQ(ASX_WIN32_HAS_BLOCKING_HOOKS, 0u);
}

#if defined(ASX_PROFILE_WIN32) && defined(_WIN32)

TEST(win32_install_rejects_null_hooks) {
    ASSERT_EQ(asx_win32_hooks_install(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(win32_install_populates_supported_hooks_only) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(asx_win32_hooks_install(&hooks), ASX_OK);
    ASSERT_TRUE(hooks.clock.now_ns_fn != NULL);
    ASSERT_TRUE(hooks.clock.logical_now_ns_fn != NULL);
    ASSERT_TRUE(hooks.entropy.random_u64_fn != NULL);
    ASSERT_TRUE(hooks.reactor.wait_fn != NULL);
    ASSERT_TRUE(hooks.reactor.ghost_wait_fn != NULL);
    ASSERT_TRUE(hooks.log.write_fn != NULL);

    ASSERT_TRUE(hooks.blocking.ctx == NULL);
    ASSERT_TRUE(hooks.blocking.submit_fn == NULL);
    ASSERT_TRUE(hooks.blocking.shutdown_fn == NULL);
    ASSERT_TRUE(hooks.blocking.capacity_fn == NULL);
}

TEST(win32_install_validates_for_active_build_mode) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(asx_win32_hooks_install(&hooks), ASX_OK);
#if ASX_DETERMINISTIC
    ASSERT_EQ(hooks.deterministic_seeded_prng, 1u);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
#else
    ASSERT_EQ(hooks.deterministic_seeded_prng, 0u);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
#endif
}

TEST(win32_qpc_clock_is_nonzero_and_non_decreasing) {
    asx_time previous;
    asx_time current;
    unsigned int i;

    previous = asx_win32_qpc_now_ns(NULL);
    ASSERT_TRUE(previous > 0u);

    for (i = 0; i < 64u; ++i) {
        current = asx_win32_qpc_now_ns(NULL);
        ASSERT_TRUE(current >= previous);
        previous = current;
    }
}

TEST(win32_reactor_zero_timeout_reports_no_ready_events) {
    asx_runtime_hooks hooks;
    uint32_t ready_count = 99u;

    ASSERT_EQ(asx_win32_hooks_install(&hooks), ASX_OK);
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 0u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 0u);
}

TEST(win32_ghost_reactor_reports_no_ready_events) {
    asx_runtime_hooks hooks;
    uint32_t ready_count = 99u;

    ASSERT_EQ(asx_win32_hooks_install(&hooks), ASX_OK);
    ASSERT_EQ(hooks.reactor.ghost_wait_fn(hooks.reactor.ctx, 7u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 0u);
}

TEST(win32_entropy_primitive_produces_sample) {
    uint64_t sample;

    sample = asx_win32_entropy_u64(NULL);
    ASSERT_TRUE(sample != 0u);
}

#endif

int main(void) {
    RUN_TEST(win32_capability_contract_names_supported_and_deferred_surfaces);
#if defined(ASX_PROFILE_WIN32) && defined(_WIN32)
    RUN_TEST(win32_install_rejects_null_hooks);
    RUN_TEST(win32_install_populates_supported_hooks_only);
    RUN_TEST(win32_install_validates_for_active_build_mode);
    RUN_TEST(win32_qpc_clock_is_nonzero_and_non_decreasing);
    RUN_TEST(win32_reactor_zero_timeout_reports_no_ready_events);
    RUN_TEST(win32_ghost_reactor_reports_no_ready_events);
    RUN_TEST(win32_entropy_primitive_produces_sample);
#endif
    TEST_REPORT();
    return test_failures;
}
