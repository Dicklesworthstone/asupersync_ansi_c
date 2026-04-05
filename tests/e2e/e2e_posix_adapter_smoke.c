/*
 * e2e_posix_adapter_smoke.c — E2E smoke test for POSIX platform adapter
 *
 * Exercises the POSIX hook installation path integrated with the runtime:
 *   1. Install POSIX hooks and verify runtime init succeeds
 *   2. Clock returns plausible monotonic values
 *   3. Entropy produces distinct values
 *   4. Reactor returns promptly with no fds registered
 *   5. Log hook writes to stderr without crashing
 *
 * Emits SCENARIO lines consumed by the e2e harness.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdint.h>
#include <stdio.h>

#if defined(ASX_PROFILE_POSIX)
#include <asx/platform/posix.h>
#endif

static void scenario(const char *id, int pass, const char *detail) {
    printf("SCENARIO %s %s%s%s\n", id, pass ? "pass" : "fail", detail ? " " : "",
           detail ? detail : "");
}

int main(void) {
#if !defined(ASX_PROFILE_POSIX)
    /* If not built with POSIX profile, skip all scenarios */
    scenario("posix_adapter_smoke.skipped", 1, "not ASX_PROFILE_POSIX");
    return 0;
#else
    asx_runtime_hooks hooks;
    asx_status st;

    /* Scenario 1: Install POSIX hooks */
    st = asx_posix_hooks_install(&hooks);
    scenario("posix_adapter_smoke.install", st == ASX_OK, NULL);
    if (st != ASX_OK) return 1;

    /* Scenario 2: Clock returns plausible monotonic value (> 1s uptime) */
    {
        asx_time now = hooks.clock.now_ns_fn(hooks.clock.ctx);
        scenario("posix_adapter_smoke.clock_plausible", now > 1000000000ULL, NULL);
    }

    /* Scenario 3: Entropy produces at least 2 distinct values in 10 samples */
    {
        uint64_t first = hooks.entropy.random_u64_fn(hooks.entropy.ctx);
        int distinct = 0;
        unsigned int i;
        for (i = 0; i < 10u; ++i) {
            if (hooks.entropy.random_u64_fn(hooks.entropy.ctx) != first) {
                distinct = 1;
                break;
            }
        }
        scenario("posix_adapter_smoke.entropy_varies", distinct, NULL);
    }

    /* Scenario 4: Reactor with 0ms timeout returns promptly */
    {
        uint32_t ready = 99u;
        st = hooks.reactor.wait_fn(hooks.reactor.ctx, 0u, &ready);
        scenario("posix_adapter_smoke.reactor_empty_poll", st == ASX_OK && ready == 0u, NULL);
    }

    /* Scenario 5: Log hook doesn't crash */
    {
        hooks.log.write_fn(hooks.log.ctx, 2, "posix_adapter_smoke: log hook OK");
        scenario("posix_adapter_smoke.log_write", 1, NULL);
    }

    /* Scenario 6: Ghost reactor returns 0 ready in deterministic mode */
    {
        uint32_t ready = 99u;
        st = hooks.reactor.ghost_wait_fn(hooks.reactor.ctx, 1u, &ready);
        scenario("posix_adapter_smoke.ghost_reactor", st == ASX_OK && ready == 0u, NULL);
    }

    return 0;
#endif
}
