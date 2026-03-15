/*
 * vignette_hooks.c — API ergonomics vignette: freestanding hooks
 *
 * Exercises: custom hook installation for embedded/freestanding targets,
 * hook validation, deterministic PRNG entropy, allocator sealing,
 * and runtime configuration initialization.
 *
 * This vignette demonstrates what an embedded integrator would write
 * to wire up asx to a custom platform without POSIX dependencies.
 *
 * bd-56t.5 — API ergonomics validation gate
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <unistd.h>
#endif

/* -------------------------------------------------------------------
 * Custom platform hooks — minimal freestanding implementation
 * ------------------------------------------------------------------- */

/* Allocator: wraps malloc/free (a real embedded target would use a
 * static pool or arena allocator). */
static void *custom_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *custom_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    return realloc(ptr, size);
}

static void custom_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

/* Clock: deterministic monotonic counter (no real time). */
static uint64_t g_clock_ns = 0;
static asx_time custom_clock_now(void *ctx) {
    (void)ctx;
    g_clock_ns += 1000000; /* advance 1ms per call */
    return g_clock_ns;
}

/* Entropy: deterministic PRNG (xorshift64). */
static uint64_t g_prng_state = 42;
static int g_reactor_waits = 0;
#if defined(__unix__) || defined(__APPLE__)
static int g_native_reactor_waits = 0;
#endif

static uint64_t custom_random(void *ctx) {
    (void)ctx;
    g_prng_state ^= g_prng_state << 13;
    g_prng_state ^= g_prng_state >> 7;
    g_prng_state ^= g_prng_state << 17;
    return g_prng_state;
}

static asx_status custom_ghost_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    g_reactor_waits++;
    *ready_count = 2;
    return ASX_OK;
}

#if defined(__unix__) || defined(__APPLE__)
typedef struct {
    int read_fd;
} native_reactor_ctx;

static asx_status native_poll_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    native_reactor_ctx *native = (native_reactor_ctx *)ctx;
    struct pollfd pfd;
    int rc;

    (void)logical_step;

    if (native == NULL || ready_count == NULL || native->read_fd < 0) { return ASX_E_INVALID_ARGUMENT; }

    pfd.fd = native->read_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    g_native_reactor_waits++;
    rc = poll(&pfd, 1, 0);
    if (rc < 0) { return ASX_E_INVALID_STATE; }

    *ready_count = (rc > 0 && (pfd.revents & POLLIN) != 0) ? 1u : 0u;
    return ASX_OK;
}
#endif

/* Log sink: print to stderr. */
static void custom_log(void *ctx, int level, const char *message) {
    (void)ctx;
    fprintf(stderr, "[asx-log L%d] %s\n", level, message);
}

/* Minimal task used by the end-to-end hook scenario. */
static asx_status poll_hook_smoke(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static uint64_t blocking_add_seven(void *user_data) {
    uint64_t input = *(uint64_t *)user_data;
    return input + 7u;
}

static asx_status populate_custom_hooks(asx_runtime_hooks *hooks) {
    asx_status st;

    st = asx_runtime_hooks_init(hooks);
    if (st != ASX_OK) { return st; }

    hooks->allocator.malloc_fn = custom_malloc;
    hooks->allocator.realloc_fn = custom_realloc;
    hooks->allocator.free_fn = custom_free;
    hooks->allocator.ctx = NULL;

    hooks->clock.now_ns_fn = custom_clock_now;
    hooks->clock.logical_now_ns_fn = custom_clock_now;
    hooks->clock.ctx = NULL;

    hooks->entropy.random_u64_fn = custom_random;
    hooks->entropy.ctx = NULL;

    hooks->reactor.ghost_wait_fn = custom_ghost_reactor;
    hooks->reactor.ctx = NULL;

    hooks->log.write_fn = custom_log;
    hooks->log.ctx = NULL;

    hooks->deterministic_seeded_prng = 1;

    return asx_runtime_hooks_validate(hooks, 1);
}

static asx_status install_custom_hooks(void) {
    asx_runtime_hooks hooks;
    asx_status st;

    st = populate_custom_hooks(&hooks);
    if (st != ASX_OK) { return st; }

    return asx_runtime_set_hooks(&hooks);
}

/* -------------------------------------------------------------------
 * Scenario 1: Hook installation and validation
 * ------------------------------------------------------------------- */
static int scenario_hook_setup(void) {
    asx_status st;

    printf("--- scenario: hook setup ---\n");

    /*
     * ERGO: asx_runtime_hooks_init(&hooks) zero-initializes the hooks
     * struct with safe defaults. This is the recommended starting point.
     * Without it, the user would need to memset or manually initialize
     * all 5 vtable structs — error-prone.
     */
    st = install_custom_hooks();
    if (st != ASX_OK) {
        printf("  FAIL: install_custom_hooks returned %s\n", asx_status_str(st));
        return 1;
    }
    printf("  hook validation: PASS\n");

    /* Verify we can read back the installed hooks. */
    const asx_runtime_hooks *active = asx_runtime_get_hooks();
    if (active == NULL) {
        printf("  FAIL: get_hooks returned NULL\n");
        return 1;
    }
    if (active->allocator.malloc_fn != custom_malloc) {
        printf("  FAIL: hooks not correctly installed\n");
        return 1;
    }
    if (active->reactor.ghost_wait_fn != custom_ghost_reactor) {
        printf("  FAIL: ghost reactor hook not installed\n");
        return 1;
    }

    printf("  PASS: hook setup\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 2: Use hooks through the runtime API
 * ------------------------------------------------------------------- */
static int scenario_hook_usage(void) {
    asx_status st;
    asx_time now = 0;
    uint64_t rval = 0;

    printf("--- scenario: hook usage ---\n");

    /*
     * ERGO: Runtime hook wrappers (asx_runtime_now_ns, asx_runtime_random_u64)
     * provide a clean, uniform interface. The user doesn't call hook
     * function pointers directly — the runtime handles dispatch and
     * error checking (ASX_E_HOOK_MISSING when the hook family is absent).
     */
    st = asx_runtime_now_ns(&now);
    if (st != ASX_OK) {
        printf("  FAIL: runtime_now_ns returned %s\n", asx_status_str(st));
        return 1;
    }
    printf("  clock read: %llu ns\n", (unsigned long long)now);

    st = asx_runtime_random_u64(&rval);
    if (st != ASX_OK) {
        printf("  FAIL: runtime_random_u64 returned %s\n", asx_status_str(st));
        return 1;
    }
    printf("  entropy read: 0x%016llx\n", (unsigned long long)rval);

    /* Log through the hook. */
    st = asx_runtime_log_write(0, "vignette: testing log hook");
    if (st != ASX_OK) {
        printf("  FAIL: runtime_log_write returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: hook usage\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 3: Runtime configuration
 * ------------------------------------------------------------------- */
static int scenario_config(void) {
    asx_runtime_config cfg;

    printf("--- scenario: runtime config ---\n");

    /*
     * ERGO: asx_runtime_config_init fills the config with profile-
     * appropriate defaults. The size-field pattern (cfg.size = sizeof(cfg))
     * enables forward compatibility — new fields can be added without
     * breaking old code. This is a well-known C pattern.
     *
     * OBSERVATION: The init function sets cfg.size internally, so the
     * user doesn't need to remember to do it. Good ergonomics.
     */
    asx_runtime_config_init(&cfg);

    printf("  config size: %u\n", cfg.size);
    printf("  finalizer_poll_budget: %u\n", cfg.finalizer_poll_budget);
    printf("  max_cancel_chain_depth: %u\n", cfg.max_cancel_chain_depth);

    /*
     * ERGO: The config struct exposes sensible defaults. Users only
     * need to override what they care about. The naming is descriptive
     * (finalizer_poll_budget, max_cancel_chain_depth). The enum types
     * for policies (wait_policy, leak_response, finalizer_escalation)
     * are self-documenting.
     */

    printf("  PASS: runtime config\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 4: Allocator sealing
 * ------------------------------------------------------------------- */
static int scenario_allocator_seal(void) {
    asx_status st;
    void *ptr = NULL;

    printf("--- scenario: allocator seal ---\n");

    /* Ensure hooks are installed from scenario 1. */

    /* Allocate before sealing — should work. */
    st = asx_runtime_alloc(64, &ptr);
    if (st != ASX_OK) {
        printf("  FAIL: alloc before seal returned %s\n", asx_status_str(st));
        return 1;
    }
    printf("  alloc before seal: OK (ptr=%p)\n", ptr);

    /* Free the allocation. */
    st = asx_runtime_free(ptr);
    if (st != ASX_OK) {
        printf("  FAIL: free returned %s\n", asx_status_str(st));
        return 1;
    }

    /*
     * ERGO: asx_runtime_seal_allocator() prevents further allocations.
     * This is powerful for embedded targets that want to guarantee no
     * heap usage during steady-state operation. The error message
     * (ASX_E_ALLOCATOR_SEALED) is clear.
     */
    st = asx_runtime_seal_allocator();
    if (st != ASX_OK) {
        printf("  FAIL: seal_allocator returned %s\n", asx_status_str(st));
        return 1;
    }

    /* Allocate after sealing — should fail. */
    ptr = NULL;
    st = asx_runtime_alloc(64, &ptr);
    if (st != ASX_E_ALLOCATOR_SEALED) {
        printf("  FAIL: alloc after seal should return ALLOCATOR_SEALED, "
               "got %s\n",
               asx_status_str(st));
        return 1;
    }
    printf("  alloc after seal correctly returned: %s\n", asx_status_str(st));

    printf("  PASS: allocator seal\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 5: Reactor and blocking seams
 * ------------------------------------------------------------------- */
static int scenario_reactor_and_blocking(void) {
    asx_status st;
    uint32_t ready = 0;
    asx_waker waker;
    asx_blocking_handle handle;
    uint64_t input = 35;
    uint64_t result = 0;

    printf("--- scenario: reactor and blocking ---\n");

    asx_runtime_reset();
    asx_waker_reset();
    g_reactor_waits = 0;

    st = install_custom_hooks();
    if (st != ASX_OK) {
        printf("  FAIL: install_custom_hooks returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_runtime_reactor_wait(25, &ready, 7);
    if (st != ASX_OK) {
        printf("  FAIL: reactor_wait returned %s\n", asx_status_str(st));
        return 1;
    }
    if (ready != 2u || g_reactor_waits != 1) {
        printf("  FAIL: ghost reactor did not report expected readiness\n");
        return 1;
    }
    printf("  reactor wait returned %u ready events\n", ready);

    st = asx_blocking_pool_init();
    if (st != ASX_OK) {
        printf("  FAIL: blocking_pool_init returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_waker_register(1, &waker);
    if (st != ASX_OK) {
        printf("  FAIL: waker_register returned %s\n", asx_status_str(st));
        asx_blocking_pool_shutdown();
        return 1;
    }

    st = asx_spawn_blocking(blocking_add_seven, &input, &waker, &handle);
    if (st != ASX_OK) {
        printf("  FAIL: spawn_blocking returned %s\n", asx_status_str(st));
        asx_blocking_pool_shutdown();
        return 1;
    }

    st = asx_blocking_get_result(&handle, &result);
    if (st != ASX_OK) {
        printf("  FAIL: blocking_get_result returned %s\n", asx_status_str(st));
        asx_blocking_pool_shutdown();
        return 1;
    }
    if (result != 42u || !asx_waker_is_signaled(&waker)) {
        printf("  FAIL: blocking completion did not produce expected result/wake\n");
        asx_blocking_pool_shutdown();
        return 1;
    }
    printf("  spawn_blocking completed with result=%llu\n", (unsigned long long)result);

    asx_blocking_pool_shutdown();
    printf("  PASS: reactor and blocking\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 6: Native-host reactor path
 * ------------------------------------------------------------------- */
static int scenario_native_reactor(void) {
#if defined(__unix__) || defined(__APPLE__)
    asx_status st;
    asx_runtime_hooks hooks;
    native_reactor_ctx native;
    uint32_t ready = 0;
    int pipe_fds[2] = {-1, -1};
    const char byte = 'x';

    printf("--- scenario: native reactor path ---\n");

    if (pipe(pipe_fds) != 0) {
        printf("  FAIL: pipe setup failed\n");
        return 1;
    }

    asx_runtime_reset();
    g_native_reactor_waits = 0;
    native.read_fd = pipe_fds[0];

    st = populate_custom_hooks(&hooks);
    if (st != ASX_OK) {
        printf("  FAIL: populate_custom_hooks returned %s\n", asx_status_str(st));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 1;
    }

    hooks.reactor.ghost_wait_fn = native_poll_reactor;
    hooks.reactor.ctx = &native;

    st = asx_runtime_set_hooks(&hooks);
    if (st != ASX_OK) {
        printf("  FAIL: runtime_set_hooks returned %s\n", asx_status_str(st));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 1;
    }

    if (write(pipe_fds[1], &byte, 1u) != 1) {
        printf("  FAIL: native readiness write failed\n");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 1;
    }

    st = asx_runtime_reactor_wait(0, &ready, 19);
    if (st != ASX_OK) {
        printf("  FAIL: runtime_reactor_wait returned %s\n", asx_status_str(st));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 1;
    }
    if (ready != 1u || g_native_reactor_waits != 1) {
        printf("  FAIL: native reactor did not surface readiness\n");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return 1;
    }

    printf("  native reactor reported %u ready event via poll-backed hook\n", ready);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    printf("  PASS: native reactor path\n");
    return 0;
#else
    printf("--- scenario: native reactor path ---\n");
    printf("  SKIP: native reactor smoke requires Unix poll/pipe support\n");
    return 0;
#endif
}

/* -------------------------------------------------------------------
 * Scenario 7: Builder and deadline/watchdog surfaces
 * ------------------------------------------------------------------- */
static int scenario_builder_and_deadlines(void) {
    asx_status st;
    asx_runtime_builder builder;
    asx_runtime rt;
    asx_runtime_hooks hooks;
    asx_runtime_config cfg;
    asx_auto_compliance_gate gate;
    asx_auto_compliance_result result;
    asx_auto_deadline_tracker *dt;
    asx_auto_watchdog *wd;
    asx_auto_audit_ring *ring;
    const asx_audit_entry *entry;

    printf("--- scenario: builder and deadline monitors ---\n");

    memset(&rt, 0, sizeof(rt));
    asx_runtime_reset();
    asx_auto_instrument_reset();

    st = asx_runtime_builder_init_current_thread(&builder);
    if (st != ASX_OK) {
        printf("  FAIL: builder_init_current_thread returned %s\n", asx_status_str(st));
        return 1;
    }

    st = populate_custom_hooks(&hooks);
    if (st != ASX_OK) {
        printf("  FAIL: populate_custom_hooks returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_runtime_builder_set_hooks(&builder, &hooks);
    if (st != ASX_OK) {
        printf("  FAIL: builder_set_hooks returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_runtime_builder_set_finalizer_poll_budget(&builder, 48u);
    if (st != ASX_OK) {
        printf("  FAIL: builder_set_finalizer_poll_budget returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_runtime_builder_build(&builder, &rt);
    if (st != ASX_OK) {
        printf("  FAIL: builder_build returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_runtime_get_config(&rt, &cfg);
    if (st != ASX_OK) {
        printf("  FAIL: runtime_get_config returned %s\n", asx_status_str(st));
        asx_runtime_shutdown(&rt);
        return 1;
    }
    if (cfg.wait_policy != ASX_WAIT_BUSY_SPIN || cfg.finalizer_poll_budget != 48u) {
        printf("  FAIL: builder-built runtime config was not applied\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }
    printf("  builder preset produced wait_policy=%d finalizer_poll_budget=%u\n",
           (int)cfg.wait_policy, cfg.finalizer_poll_budget);

    dt = asx_auto_deadline_global();
    wd = asx_auto_watchdog_global();
    ring = asx_auto_audit_global();
    asx_auto_watchdog_init(wd, 1000u);

    asx_auto_record_deadline(1000u, 900u, 11u);
    asx_auto_record_deadline(1000u, 1300u, 11u);
    asx_auto_record_checkpoint(100u, 11u);
    asx_auto_record_checkpoint(1400u, 11u);

    asx_auto_compliance_gate_init(&gate);
    asx_auto_compliance_evaluate(&gate, dt, wd, &result);
    entry = asx_auto_audit_get(ring, 0u);

    if (result.pass || dt->deadline_misses != 1u || wd->violations != 1u || entry == NULL) {
        printf("  FAIL: deadline/watchdog evidence did not accumulate as expected\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    printf("  deadline miss rate=%u watchdog violations=%u checkpoints=%u\n",
           result.actual_miss_rate, result.actual_violations, result.actual_checkpoints);
    printf("  first audit event=%s total_events=%u\n", asx_audit_kind_str(entry->kind),
           asx_auto_audit_total(ring));
    printf("  compliance gate pass=%d mask=0x%x\n", result.pass, result.violation_mask);

    asx_runtime_shutdown(&rt);
    printf("  PASS: builder and deadline monitors\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Scenario 8: End-to-end with custom hooks
 * ------------------------------------------------------------------- */
static int scenario_end_to_end(void) {
    asx_status st;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;

    printf("--- scenario: end-to-end with hooks ---\n");

    /* Reset runtime (clears seal). */
    asx_runtime_reset();

    /* Re-install hooks (reset clears them). */
    {
        st = install_custom_hooks();
        if (st != ASX_OK) {
            printf("  FAIL: install_custom_hooks returned %s\n", asx_status_str(st));
            return 1;
        }
    }

    st = asx_region_open(&region);
    if (st != ASX_OK) {
        printf("  FAIL: region_open returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_task_spawn(region, poll_hook_smoke, NULL, &task);
    if (st != ASX_OK) {
        printf("  FAIL: task_spawn returned %s\n", asx_status_str(st));
        return 1;
    }

    budget = asx_budget_from_polls(100);
    st = asx_region_drain(region, &budget);
    if (st != ASX_OK) {
        printf("  FAIL: drain returned %s\n", asx_status_str(st));
        return 1;
    }

    printf("  PASS: end-to-end with hooks\n");
    return 0;
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */
int main(void) {
    int failures = 0;

    printf("=== vignette: freestanding hooks ===\n\n");

    failures += scenario_hook_setup();
    failures += scenario_hook_usage();
    failures += scenario_config();
    failures += scenario_allocator_seal();
    failures += scenario_reactor_and_blocking();
    failures += scenario_native_reactor();
    failures += scenario_builder_and_deadlines();
    failures += scenario_end_to_end();

    printf("\n=== hooks: %d failures ===\n", failures);
    return failures;
}
