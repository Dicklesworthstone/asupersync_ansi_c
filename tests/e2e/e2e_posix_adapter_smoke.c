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

#if defined(ASX_PROFILE_POSIX) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include <asx/asx.h>
#include <stdint.h>
#include <stdio.h>

#if defined(ASX_PROFILE_POSIX)
#include <asx/platform/posix.h>
#include <pthread.h>
#include <time.h>
#endif

static void scenario(const char *id, int pass, const char *detail) {
    printf("SCENARIO %s %s%s%s\n", id, pass ? "pass" : "fail", detail ? " " : "",
           detail ? detail : "");
}

#if defined(ASX_PROFILE_POSIX)
static pthread_mutex_t g_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cond = PTHREAD_COND_INITIALIZER;
static int g_gate_open = 0;

static uint64_t add_seven(void *user_data) {
    uint64_t value = *(uint64_t *)user_data;
    return value + 7u;
}

static uint64_t gated_job(void *user_data) {
    (void)user_data;
    pthread_mutex_lock(&g_gate_mutex);
    while (!g_gate_open) { pthread_cond_wait(&g_gate_cond, &g_gate_mutex); }
    pthread_mutex_unlock(&g_gate_mutex);
    return 1u;
}

static void open_gate(void) {
    pthread_mutex_lock(&g_gate_mutex);
    g_gate_open = 1;
    pthread_cond_broadcast(&g_gate_cond);
    pthread_mutex_unlock(&g_gate_mutex);
}

static int wait_result(const asx_blocking_handle *handle, uint64_t *out) {
    unsigned int i;
    for (i = 0; i < 200u; ++i) {
        asx_status st = asx_blocking_get_result(handle, out);
        if (st == ASX_OK) return 1;
        if (st != ASX_E_PENDING) return 0;
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            nanosleep(&ts, NULL);
        }
    }
    return 0;
}
#endif

int main(void) {
#if !defined(ASX_PROFILE_POSIX)
    /* If not built with POSIX profile, skip all scenarios */
    scenario("posix_adapter_smoke.skipped", 1, "not ASX_PROFILE_POSIX");
    return 0;
#else
    asx_runtime_hooks hooks;
    asx_runtime_config config;
    asx_runtime rt;
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

    /* Scenario 7: POSIX hook table exposes a bounded blocking-submit family */
    scenario("posix_adapter_smoke.blocking_hook_present",
             hooks.blocking.submit_fn != NULL && hooks.blocking.shutdown_fn != NULL &&
                 hooks.blocking.capacity_fn != NULL &&
                 hooks.blocking.capacity_fn(hooks.blocking.ctx) > 0u,
             NULL);

    /* Scenario 8: Runtime spawn_blocking uses the POSIX worker pool in live mode */
    asx_runtime_config_init(&config);
    st = asx_runtime_init(&rt, &config, &hooks);
    scenario("posix_adapter_smoke.runtime_init", st == ASX_OK, NULL);
    if (st != ASX_OK) return 1;

    {
        asx_blocking_handle handle;
        uint64_t input = 35u;
        uint64_t result = 0u;
        st = asx_spawn_blocking(add_seven, &input, NULL, &handle);
        scenario("posix_adapter_smoke.spawn_blocking_submit", st == ASX_OK, NULL);
        scenario("posix_adapter_smoke.spawn_blocking_result",
                 st == ASX_OK && wait_result(&handle, &result) && result == 42u, NULL);
    }

    /* Scenario 9: bounded queue rejects overload without leaking active work */
    {
        asx_blocking_handle handles[ASX_MAX_BLOCKING_TASKS];
        uint32_t ok_count = 0u;
        uint32_t exhausted_count = 0u;
        uint32_t i;

        g_gate_open = 0;
        for (i = 0; i < ASX_MAX_BLOCKING_TASKS; ++i) {
            st = asx_spawn_blocking(gated_job, NULL, NULL, &handles[i]);
            if (st == ASX_OK) {
                ok_count++;
            } else if (st == ASX_E_RESOURCE_EXHAUSTED) {
                exhausted_count++;
                break;
            } else {
                break;
            }
        }
        scenario("posix_adapter_smoke.blocking_queue_bounded",
                 ok_count > 0u && exhausted_count > 0u, NULL);
        open_gate();
    }

    asx_runtime_shutdown(&rt);
    scenario("posix_adapter_smoke.shutdown_drains",
             !asx_runtime_blocking_pool_initialized(&rt) && asx_runtime_blocking_active_count(&rt) == 0u,
             NULL);

    {
        asx_blocking_handle handle;
        uint64_t input = 1u;
        st = asx_spawn_blocking(add_seven, &input, NULL, &handle);
        scenario("posix_adapter_smoke.submit_after_shutdown_rejected", st == ASX_E_INVALID_STATE,
                 NULL);
    }

    return 0;
#endif
}
