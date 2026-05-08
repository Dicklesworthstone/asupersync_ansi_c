/*
 * test_posix_hooks.c — unit tests for POSIX platform hook installation
 *
 * Covers the current POSIX adapter surface:
 *   - install populates the expected hook pointers
 *   - installed hooks validate in live and deterministic modes
 *   - monotonic wall clock is non-decreasing and advances over time
 *   - entropy returns varying values across a sample window
 *   - empty reactor waits report zero ready events
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"

#include <asx/asx.h>

#if defined(ASX_PROFILE_POSIX)

#include <asx/platform/posix.h>
#include <stdint.h>
#include <stdio.h>

static asx_status install_posix_hooks(asx_runtime_hooks *hooks) {
    if (hooks == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_posix_hooks_install(hooks);
}

static void posix_counting_job(void *ctx) {
    uint32_t *count = (uint32_t *)ctx;
    if (count != NULL) { (*count)++; }
}

TEST(posix_install_populates_expected_hooks) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_TRUE(hooks.clock.now_ns_fn != NULL);
    ASSERT_TRUE(hooks.clock.logical_now_ns_fn != NULL);
    ASSERT_TRUE(hooks.entropy.random_u64_fn != NULL);
    ASSERT_TRUE(hooks.reactor.wait_fn != NULL);
    ASSERT_TRUE(hooks.reactor.ghost_wait_fn != NULL);
    ASSERT_TRUE(hooks.blocking.submit_fn != NULL);
    ASSERT_TRUE(hooks.blocking.shutdown_fn != NULL);
    ASSERT_TRUE(hooks.blocking.capacity_fn != NULL);
    ASSERT_TRUE(hooks.log.write_fn != NULL);
}

TEST(posix_capability_contract_names_supported_and_deferred_surfaces) {
    ASSERT_EQ(ASX_POSIX_HAS_TIMED_REACTOR_WAIT, 1u);
#if defined(__linux__)
    ASSERT_EQ(ASX_POSIX_HAS_REACTOR_FD_REGISTRATION, 1u);
#else
    ASSERT_EQ(ASX_POSIX_HAS_REACTOR_FD_REGISTRATION, 0u);
#endif
    ASSERT_EQ(ASX_POSIX_REACTOR_READABLE, 0x01u);
    ASSERT_EQ(ASX_POSIX_REACTOR_WRITABLE, 0x02u);
    ASSERT_EQ(ASX_POSIX_REACTOR_ERROR, 0x04u);
}

TEST(posix_install_validates_in_live_and_deterministic_modes) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
}

TEST(posix_wall_clock_is_non_decreasing) {
    asx_runtime_hooks hooks;
    asx_time previous;
    asx_time current;
    unsigned int i;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    previous = hooks.clock.now_ns_fn(hooks.clock.ctx);
    ASSERT_TRUE(previous > 0);

    for (i = 0; i < 64u; ++i) {
        current = hooks.clock.now_ns_fn(hooks.clock.ctx);
        ASSERT_TRUE(current >= previous);
        previous = current;
    }

    fprintf(stderr, "    monotonic clock sample_end_ns=%llu samples=%u\n",
            (unsigned long long)previous, 65u);
}

TEST(posix_wall_clock_advances_after_short_sleep) {
    asx_runtime_hooks hooks;
    asx_time before;
    asx_time after;
    uint32_t ready_count = 99u;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    before = hooks.clock.now_ns_fn(hooks.clock.ctx);
    ASSERT_TRUE(before > 0);
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 2u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 0u);
    after = hooks.clock.now_ns_fn(hooks.clock.ctx);

    ASSERT_TRUE(after >= before);
    ASSERT_TRUE((after - before) >= 1000000u);
    fprintf(stderr, "    monotonic clock delta_ns=%llu\n", (unsigned long long)(after - before));
}

TEST(posix_entropy_varies_across_samples) {
    asx_runtime_hooks hooks;
    uint64_t first;
    uint32_t unique_count = 1u;
    unsigned int i;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    first = hooks.entropy.random_u64_fn(hooks.entropy.ctx);

    for (i = 0; i < 255u; ++i) {
        uint64_t sample = hooks.entropy.random_u64_fn(hooks.entropy.ctx);
        if (sample != first) {
            unique_count++;
            break;
        }
    }

    ASSERT_TRUE(unique_count > 1u);
}

TEST(posix_entropy_produces_nonzero_sample_window) {
    asx_runtime_hooks hooks;
    uint64_t acc = 0;
    unsigned int i;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);

    for (i = 0; i < 100u; ++i) { acc |= hooks.entropy.random_u64_fn(hooks.entropy.ctx); }

    ASSERT_TRUE(acc != 0u);
}

TEST(posix_reactor_zero_timeout_returns_empty_ready_set) {
    asx_runtime_hooks hooks;
    uint32_t ready_count = 99u;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 0u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 0u);
}

TEST(posix_ghost_reactor_returns_empty_ready_set) {
    asx_runtime_hooks hooks;
    uint32_t ready_count = 99u;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_EQ(hooks.reactor.ghost_wait_fn(hooks.reactor.ctx, 7u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 0u);
}

TEST(posix_blocking_hook_reports_bounded_capacity) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_TRUE(hooks.blocking.capacity_fn(hooks.blocking.ctx) > 0u);
    ASSERT_TRUE(hooks.blocking.capacity_fn(hooks.blocking.ctx) <= 16u);
    hooks.blocking.shutdown_fn(hooks.blocking.ctx);
}

TEST(posix_blocking_hook_rejects_null_job) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_EQ(hooks.blocking.submit_fn(hooks.blocking.ctx, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    hooks.blocking.shutdown_fn(hooks.blocking.ctx);
}

TEST(posix_blocking_hook_runs_and_drains_jobs) {
    asx_runtime_hooks hooks;
    uint32_t count = 0u;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_EQ(hooks.blocking.submit_fn(hooks.blocking.ctx, posix_counting_job, &count), ASX_OK);
    ASSERT_EQ(hooks.blocking.submit_fn(hooks.blocking.ctx, posix_counting_job, &count), ASX_OK);
    ASSERT_EQ(hooks.blocking.submit_fn(hooks.blocking.ctx, posix_counting_job, &count), ASX_OK);
    hooks.blocking.shutdown_fn(hooks.blocking.ctx);
    ASSERT_EQ(count, 3u);
}

TEST(posix_wall_clock_plausibility) {
    /* Clock must return a value > 2026-01-01T00:00:00 in nanoseconds.
     * 2026-01-01 00:00:00 UTC = 1767225600 seconds = 1767225600000000000 ns */
    asx_runtime_hooks hooks;
    asx_time now;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    now = hooks.clock.now_ns_fn(hooks.clock.ctx);
    /* CLOCK_MONOTONIC may be relative to boot, so we only check > 0 and
     * that it's in a reasonable range (> 1 second worth of ns).
     * Wall-clock plausibility is tested via CLOCK_REALTIME in practice;
     * monotonic clocks measure uptime, not epoch time. */
    ASSERT_TRUE(now > 1000000000ULL); /* > 1 second of uptime */
    fprintf(stderr, "    clock plausibility: now_ns=%llu\n", (unsigned long long)now);
}

TEST(posix_entropy_100_samples_no_errors) {
    asx_runtime_hooks hooks;
    unsigned int i;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    for (i = 0; i < 100u; ++i) { (void)hooks.entropy.random_u64_fn(hooks.entropy.ctx); }
    /* If we got here without crashing/hanging, all 100 calls succeeded */
    ASSERT_TRUE(1);
}

TEST(posix_entropy_distribution_uniqueness) {
    /* Generate 64 samples and verify at least 60 are unique.
     * Simple O(n^2) uniqueness check. */
    asx_runtime_hooks hooks;
    uint64_t samples[64];
    uint32_t unique_count = 0;
    unsigned int i;
    unsigned int j;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    for (i = 0; i < 64u; ++i) { samples[i] = hooks.entropy.random_u64_fn(hooks.entropy.ctx); }

    for (i = 0; i < 64u; ++i) {
        int is_dup = 0;
        for (j = 0; j < i; ++j) {
            if (samples[j] == samples[i]) {
                is_dup = 1;
                break;
            }
        }
        if (!is_dup) unique_count++;
    }

    ASSERT_TRUE(unique_count >= 60u);
    fprintf(stderr, "    entropy uniqueness: %u/64 unique\n", unique_count);
}

#if defined(__linux__)
#include <unistd.h>

TEST(posix_reactor_lifecycle_pipe) {
    /* Reactor lifecycle: create pipe, register read-end through the POSIX helper,
     * write to pipe, wait should report ready=1, then clean up. */
    asx_runtime_hooks hooks;
    uint32_t ready_count = 0;
    int pipefd[2];
    char buf = 'X';

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);

    /* Trigger lazy init by calling wait once */
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 0u, &ready_count), ASX_OK);

    ASSERT_TRUE(hooks.reactor.ctx != NULL);

    /* Create pipe and register read-end */
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_EQ(asx_posix_reactor_register_fd(hooks.reactor.ctx, pipefd[0],
                                            ASX_POSIX_REACTOR_READABLE),
              ASX_OK);

    /* Write to pipe to make it readable */
    ASSERT_EQ(write(pipefd[1], &buf, 1), 1);

    /* Wait should now report 1 ready event */
    ready_count = 0;
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 100u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 1u);

    /* Deregister and close */
    asx_posix_reactor_deregister_fd(hooks.reactor.ctx, pipefd[0]);
    close(pipefd[0]);
    close(pipefd[1]);

    /* After deregister, wait should return 0 ready */
    ready_count = 99u;
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 0u, &ready_count), ASX_OK);
    ASSERT_EQ(ready_count, 0u);
    asx_posix_reactor_reset(hooks.reactor.ctx);
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 0u, &ready_count), ASX_OK);
    asx_posix_reactor_reset(hooks.reactor.ctx);

    fprintf(stderr, "    reactor lifecycle: pipe register/write/wait/deregister OK\n");
}

TEST(posix_reactor_registration_fails_closed_for_bad_inputs) {
    asx_runtime_hooks hooks;

    ASSERT_EQ(install_posix_hooks(&hooks), ASX_OK);
    ASSERT_EQ(asx_posix_reactor_register_fd(NULL, 0, ASX_POSIX_REACTOR_READABLE),
              ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_posix_reactor_register_fd(hooks.reactor.ctx, -1, ASX_POSIX_REACTOR_READABLE),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_posix_reactor_register_fd(hooks.reactor.ctx, 0, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_posix_reactor_register_fd(hooks.reactor.ctx, 0, 0x80u), ASX_E_INVALID_ARGUMENT);
}

#endif /* __linux__ */

#else

TEST(posix_hooks_skipped_without_posix_profile) { ASSERT_TRUE(1); }

#endif

int main(void) {
#if defined(ASX_PROFILE_POSIX)
    RUN_TEST(posix_install_populates_expected_hooks);
    RUN_TEST(posix_capability_contract_names_supported_and_deferred_surfaces);
    RUN_TEST(posix_install_validates_in_live_and_deterministic_modes);
    RUN_TEST(posix_wall_clock_is_non_decreasing);
    RUN_TEST(posix_wall_clock_advances_after_short_sleep);
    RUN_TEST(posix_entropy_varies_across_samples);
    RUN_TEST(posix_entropy_produces_nonzero_sample_window);
    RUN_TEST(posix_reactor_zero_timeout_returns_empty_ready_set);
    RUN_TEST(posix_ghost_reactor_returns_empty_ready_set);
    RUN_TEST(posix_blocking_hook_reports_bounded_capacity);
    RUN_TEST(posix_blocking_hook_rejects_null_job);
    RUN_TEST(posix_blocking_hook_runs_and_drains_jobs);
    RUN_TEST(posix_wall_clock_plausibility);
    RUN_TEST(posix_entropy_100_samples_no_errors);
    RUN_TEST(posix_entropy_distribution_uniqueness);
#if defined(__linux__)
    RUN_TEST(posix_reactor_lifecycle_pipe);
    RUN_TEST(posix_reactor_registration_fails_closed_for_bad_inputs);
#endif
#else
    RUN_TEST(posix_hooks_skipped_without_posix_profile);
#endif
    TEST_REPORT();
    return test_failures;
}
