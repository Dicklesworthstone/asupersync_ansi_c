/*
 * test_rt.c — unit tests for runtime object lifecycle and state queries
 *
 * Tests cover:
 *   - Config validation
 *   - Runtime init/shutdown lifecycle
 *   - Default initialization
 *   - State queries (region/task/obligation counts)
 *   - Config/hooks retrieval
 *   - Guard behavior for uninitialized/NULL runtime
 *   - Capacity queries
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <string.h>

#if defined(_WIN32)
#include <stdlib.h>
static void rt_setenv(const char *name, const char *value) { _putenv_s(name, value); }
static void rt_unsetenv(const char *name) { _putenv_s(name, ""); }
#else
#include <stdlib.h>
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
static void rt_setenv(const char *name, const char *value) { (void)setenv(name, value, 1); }
static void rt_unsetenv(const char *name) { (void)unsetenv(name); }
#endif

/* Suppress warn_unused_result in test helpers where we don't check */
static asx_status rt_test_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        rt_test_sink_ = (expr);                                                                    \
        (void)rt_test_sink_;                                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* Dummy poll function for task spawn tests                            */
/* ------------------------------------------------------------------ */

static asx_status dummy_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static void cleanup_mark(void *ctx) {
    int *flag = (int *)ctx;
    *flag += 1;
}

#if ASX_HAS_BLOCKING_SURFACE
static uint64_t add_one(void *user_data) {
    uint64_t value = *(uint64_t *)user_data;
    return value + 1u;
}
#endif

static uint32_t g_rt_ready_count;

static asx_status rt_fixed_ready_reactor(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    (void)ctx;
    (void)logical_step;
    if (ready_count == NULL) return ASX_E_INVALID_ARGUMENT;
    *ready_count = g_rt_ready_count;
    return ASX_OK;
}

static asx_status lab_noop_step(asx_lab *lab, void *user_data) {
    (void)user_data;
    return asx_lab_open_region(lab, (asx_region_id *)user_data);
}

static void clear_rt_test_env(void) {
    rt_unsetenv("ASX_RUNTIME_PRESET");
    rt_unsetenv("ASX_RUNTIME_WAIT_POLICY");
    rt_unsetenv("ASX_RUNTIME_IO_BACKEND");
    rt_unsetenv("ASX_RUNTIME_LEAK_RESPONSE");
    rt_unsetenv("ASX_RUNTIME_FINALIZER_POLL_BUDGET");
    rt_unsetenv("ASX_RUNTIME_FINALIZER_TIME_BUDGET_NS");
    rt_unsetenv("ASX_RUNTIME_FINALIZER_ESCALATION");
    rt_unsetenv("ASX_RUNTIME_MAX_CANCEL_CHAIN_DEPTH");
    rt_unsetenv("ASX_RUNTIME_MAX_CANCEL_CHAIN_MEMORY");

    rt_unsetenv("TESTRT_PRESET");
    rt_unsetenv("TESTRT_WAIT_POLICY");
    rt_unsetenv("TESTRT_IO_BACKEND");
    rt_unsetenv("TESTRT_LEAK_RESPONSE");
    rt_unsetenv("TESTRT_FINALIZER_POLL_BUDGET");
    rt_unsetenv("TESTRT_FINALIZER_TIME_BUDGET_NS");
    rt_unsetenv("TESTRT_FINALIZER_ESCALATION");
    rt_unsetenv("TESTRT_MAX_CANCEL_CHAIN_DEPTH");
    rt_unsetenv("TESTRT_MAX_CANCEL_CHAIN_MEMORY");
}

/* ------------------------------------------------------------------ */
/* Helper: create a valid config                                       */
/* ------------------------------------------------------------------ */

static void make_valid_config(asx_runtime_config *cfg) { asx_runtime_config_init(cfg); }

static void make_valid_hooks(asx_runtime_hooks *hooks) { asx_runtime_hooks_init(hooks); }

/* ------------------------------------------------------------------ */
/* Config validation tests                                             */
/* ------------------------------------------------------------------ */

TEST(config_validate_null_fails) {
    ASSERT_EQ(asx_runtime_config_validate(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_bad_size_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.size = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_zero_cancel_depth_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.max_cancel_chain_depth = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_zero_finalizer_budget_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.finalizer_poll_budget = 0;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_wait_policy_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.wait_policy = (asx_wait_policy)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_io_backend_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.io_backend = (asx_io_backend)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_leak_response_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.leak_response = (asx_leak_response)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_finalizer_escalation_fails) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    cfg.finalizer_escalation = (asx_finalizer_escalation)99;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_invalid_leak_escalation_fails) {
    asx_runtime_config cfg;
    asx_leak_escalation_config escalation;
    make_valid_config(&cfg);
    escalation.threshold = 1u;
    escalation.escalate_to = (asx_leak_response)99;
    cfg.leak_escalation = &escalation;
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(config_validate_defaults_ok) {
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_config_validate(&cfg), ASX_OK);
    ASSERT_EQ(cfg.io_backend, ASX_IO_BACKEND_GHOST);
}

/* ------------------------------------------------------------------ */
/* Init/shutdown lifecycle tests                                       */
/* ------------------------------------------------------------------ */

TEST(init_null_rt_fails) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(NULL, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_config_fails) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&rt, NULL, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_hooks_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_bad_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.size = 0; /* invalid */
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_invalid_enum_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.wait_policy = (asx_wait_policy)99;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_unsupported_io_backend_fails_closed) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.io_backend = ASX_IO_BACKEND_IO_URING;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_PERMISSION_DENIED);
}

TEST(init_invalid_leak_escalation_config_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_leak_escalation_config escalation;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    escalation.threshold = 1u;
    escalation.escalate_to = (asx_leak_response)99;
    cfg.leak_escalation = &escalation;
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_E_INVALID_ARGUMENT);
}

TEST(init_success) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    ASSERT_NE(rt.generation, 0u);
    asx_runtime_shutdown(&rt);
}

TEST(init_generation_increments) {
    asx_runtime a, b;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    uint32_t gen_a;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&a, &cfg, &hooks));
    gen_a = a.generation;
    asx_runtime_shutdown(&a);

    MUST_OK(asx_runtime_init(&b, &cfg, &hooks));
    ASSERT_TRUE(b.generation > gen_a);
    asx_runtime_shutdown(&b);
}

TEST(stale_runtime_reports_uninitialized_after_new_init) {
    asx_runtime a, b;
    asx_runtime_config out;

    MUST_OK(asx_runtime_init_default(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&a));

    MUST_OK(asx_runtime_init_default(&b));
    ASSERT_FALSE(asx_runtime_is_initialized(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    ASSERT_EQ(asx_runtime_get_config(&a, &out), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_runtime_get_config(&b, &out), ASX_OK);

    asx_runtime_shutdown(&a);
    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    asx_runtime_shutdown(&b);
}

TEST(shutdown_clears_state) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));
    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
    ASSERT_EQ(rt.generation, 0u);
}

TEST(shutdown_null_safe) { asx_runtime_shutdown(NULL); }

TEST(shutdown_double_safe) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    asx_runtime_shutdown(&rt);
    asx_runtime_shutdown(&rt); /* should not crash */
}

TEST(stale_shutdown_does_not_tear_down_active_runtime) {
    asx_runtime a, b;
    asx_runtime_config out;

    MUST_OK(asx_runtime_init_default(&a));
    MUST_OK(asx_runtime_init_default(&b));

    ASSERT_FALSE(asx_runtime_is_initialized(&a));
    ASSERT_TRUE(asx_runtime_is_initialized(&b));

    asx_runtime_shutdown(&a);

    ASSERT_TRUE(asx_runtime_is_initialized(&b));
    ASSERT_EQ(asx_runtime_get_config(&b, &out), ASX_OK);

    asx_runtime_shutdown(&b);
}

TEST(shutdown_uninitialized_safe) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    asx_runtime_shutdown(&rt); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Default initialization tests                                        */
/* ------------------------------------------------------------------ */

TEST(init_default_null_fails) { ASSERT_EQ(asx_runtime_init_default(NULL), ASX_E_INVALID_ARGUMENT); }

TEST(init_from_env_null_rt_fails) {
    clear_rt_test_env();
    ASSERT_EQ(asx_runtime_init_from_env(NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(init_default_success) {
    asx_runtime rt;
    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* Verify defaults were applied */
    ASSERT_EQ(rt.config.io_backend, ASX_IO_BACKEND_GHOST);
    ASSERT_EQ(rt.config.leak_response, ASX_LEAK_LOG);
    ASSERT_EQ(rt.config.finalizer_poll_budget, 100u);
    ASSERT_EQ(rt.config.max_cancel_chain_depth, 16u);

    asx_runtime_shutdown(&rt);
}

TEST(init_from_env_applies_default_prefix_overrides) {
    asx_runtime rt;
    asx_runtime_config cfg;

    clear_rt_test_env();
    rt_setenv("ASX_RUNTIME_PRESET", "high-throughput");
    rt_setenv("ASX_RUNTIME_WAIT_POLICY", "yield");
    rt_setenv("ASX_RUNTIME_IO_BACKEND", "ghost");
    rt_setenv("ASX_RUNTIME_FINALIZER_POLL_BUDGET", "77");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, NULL), ASX_OK);
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_YIELD);
    ASSERT_EQ(cfg.io_backend, ASX_IO_BACKEND_GHOST);
    ASSERT_EQ(cfg.finalizer_poll_budget, 77u);
    ASSERT_EQ(cfg.max_cancel_chain_depth, 32u);

    asx_runtime_shutdown(&rt);
    clear_rt_test_env();
}

TEST(init_from_env_applies_custom_prefix_overrides) {
    asx_runtime rt;
    asx_runtime_config cfg;

    clear_rt_test_env();
    rt_setenv("TESTRT_PRESET", "current-thread");
    rt_setenv("TESTRT_IO_BACKEND", "ghost");
    rt_setenv("TESTRT_FINALIZER_TIME_BUDGET_NS", "4444");
    rt_setenv("TESTRT_MAX_CANCEL_CHAIN_MEMORY", "12345");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, "TESTRT_"), ASX_OK);
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    ASSERT_EQ(cfg.wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(cfg.io_backend, ASX_IO_BACKEND_GHOST);
    ASSERT_EQ(cfg.finalizer_time_budget_ns, (uint64_t)4444u);
    ASSERT_EQ(cfg.max_cancel_chain_memory, 12345u);

    asx_runtime_shutdown(&rt);
    clear_rt_test_env();
}

TEST(init_from_env_rejects_invalid_env_without_initializing) {
    asx_runtime rt;

    memset(&rt, 0, sizeof(rt));
    clear_rt_test_env();
    rt_setenv("ASX_RUNTIME_MAX_CANCEL_CHAIN_DEPTH", "0");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));

    clear_rt_test_env();
}

TEST(init_from_env_rejects_unsupported_io_backend_without_initializing) {
    asx_runtime rt;

    memset(&rt, 0, sizeof(rt));
    clear_rt_test_env();
    rt_setenv("ASX_RUNTIME_IO_BACKEND", "io_uring");

    ASSERT_EQ(asx_runtime_init_from_env(&rt, NULL), ASX_E_PERMISSION_DENIED);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));

    clear_rt_test_env();
}

TEST(init_default_wires_blocking_surface) {
    asx_runtime rt;
#if ASX_HAS_BLOCKING_SURFACE
    asx_blocking_handle h;
    uint64_t input = 9;
    uint64_t result = 0;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);

#if ASX_HAS_BLOCKING_SURFACE
    if (asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        ASSERT_EQ(asx_spawn_blocking(add_one, &input, NULL, &h), ASX_OK);
        ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_OK);
        ASSERT_EQ(result, (uint64_t)10);
    } else {
        ASSERT_EQ(asx_spawn_blocking(add_one, &input, NULL, &h), ASX_E_PERMISSION_DENIED);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_BLOCKING));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
#endif

    asx_runtime_shutdown(&rt);
}

TEST(init_default_wires_io_surface) {
    asx_runtime rt;
    asx_waker w;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_token tok;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_EQ(asx_waker_register(1, &w), ASX_OK);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_OK);
        asx_io_deregister(&tok);
    } else {
        ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_E_PERMISSION_DENIED);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_IO_DRIVER));
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
#endif

    asx_runtime_shutdown(&rt);
}

TEST(init_default_io_poll_wakes_registered_task) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    asx_waker w;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_token tok;
    asx_io_event event;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, &hooks), ASX_OK);
    hooks.reactor.ghost_wait_fn = rt_fixed_ready_reactor;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);
    g_rt_ready_count = 0u;

    ASSERT_EQ(asx_waker_register(7, &w), ASX_OK);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_OK);
        ASSERT_FALSE(asx_waker_is_signaled(&w));
        g_rt_ready_count = 1u;
        ASSERT_EQ(asx_io_driver_poll(&event, 1u, 5u), 1u);
        ASSERT_EQ(event.token.slot, tok.slot);
        ASSERT_EQ(event.token.generation, tok.generation);
        ASSERT_EQ(event.ready, ASX_IO_READABLE);
        ASSERT_TRUE(asx_waker_is_signaled(&w));
        asx_io_deregister(&tok);
    } else {
        ASSERT_EQ(asx_io_register(42, ASX_IO_READABLE, &w, &tok), ASX_E_PERMISSION_DENIED);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_IO_DRIVER));
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_FALSE(asx_waker_is_signaled(&w));
#endif

    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* is_initialized tests                                                */
/* ------------------------------------------------------------------ */

TEST(is_initialized_null_returns_false) { ASSERT_FALSE(asx_runtime_is_initialized(NULL)); }

TEST(is_initialized_zeroed_returns_false) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
}

/* ------------------------------------------------------------------ */
/* Config/hooks retrieval tests                                        */
/* ------------------------------------------------------------------ */

TEST(get_config_null_rt_fails) {
    asx_runtime_config out;
    ASSERT_EQ(asx_runtime_get_config(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_config_null_out_fails) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    asx_runtime_shutdown(&rt);
}

TEST(get_config_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_config out;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_E_INVALID_STATE);
}

TEST(get_config_returns_stored_config) {
    asx_runtime rt;
    asx_runtime_config cfg, out;
    asx_runtime_hooks hooks;
    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    cfg.finalizer_poll_budget = 42;
    cfg.io_backend = ASX_IO_BACKEND_GHOST;

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.finalizer_poll_budget, 42u);
    ASSERT_EQ(out.io_backend, ASX_IO_BACKEND_GHOST);
    asx_runtime_shutdown(&rt);
}

TEST(runtime_io_backend_query_tracks_runtime_config) {
    asx_runtime rt;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_io_backend(&rt), ASX_IO_BACKEND_GHOST);
    asx_runtime_shutdown(&rt);
    ASSERT_EQ(asx_runtime_io_backend(&rt), ASX_IO_BACKEND_GHOST);
}

TEST(init_copies_leak_escalation_config) {
    asx_runtime rt;
    asx_runtime_config cfg, out;
    asx_runtime_hooks hooks;
    asx_leak_escalation_config escalation;

    make_valid_config(&cfg);
    make_valid_hooks(&hooks);
    escalation.threshold = 7u;
    escalation.escalate_to = ASX_LEAK_RECOVER;
    cfg.leak_escalation = &escalation;

    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));

    escalation.threshold = 99u;
    escalation.escalate_to = ASX_LEAK_PANIC;

    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_TRUE(out.leak_escalation != NULL);
    ASSERT_TRUE(out.leak_escalation != &escalation);
    ASSERT_EQ(out.leak_escalation->threshold, (uint64_t)7u);
    ASSERT_EQ(out.leak_escalation->escalate_to, ASX_LEAK_RECOVER);
    asx_runtime_shutdown(&rt);
}

TEST(get_hooks_null_rt_fails) {
    asx_runtime_hooks out;
    ASSERT_EQ(asx_runtime_get_hooks_from(NULL, &out), ASX_E_INVALID_ARGUMENT);
}

TEST(get_hooks_null_out_fails) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, NULL), ASX_E_INVALID_ARGUMENT);
    asx_runtime_shutdown(&rt);
}

TEST(get_hooks_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_hooks out;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_get_hooks_from(&rt, &out), ASX_E_INVALID_STATE);
}

TEST(validate_reload_uninitialized_fails) {
    asx_runtime rt;
    asx_runtime_config cfg;

    memset(&rt, 0, sizeof(rt));
    make_valid_config(&cfg);
    ASSERT_EQ(asx_runtime_validate_reload_config(&rt, &cfg, NULL), ASX_E_INVALID_STATE);
}

TEST(validate_reload_reports_restart_required_field) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed;
    const char *rejection = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    proposed.max_cancel_chain_depth = (uint16_t)(cfg.max_cancel_chain_depth + 1u);

    ASSERT_EQ(asx_runtime_validate_reload_config(&rt, &proposed, &rejection),
              ASX_E_CONFIG_RESTART_REQ);
    ASSERT_TRUE(rejection != NULL);
    ASSERT_STR_EQ(rejection, "max_cancel_chain_depth");
    asx_runtime_shutdown(&rt);
}

TEST(reload_config_updates_reloadable_fields) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed, out;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    proposed.wait_policy = ASX_WAIT_BUSY_SPIN;
    proposed.finalizer_poll_budget = 222u;
    proposed.finalizer_time_budget_ns = (uint64_t)2222222u;

    ASSERT_EQ(asx_runtime_reload_config(&rt, &proposed, NULL), ASX_OK);
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.wait_policy, ASX_WAIT_BUSY_SPIN);
    ASSERT_EQ(out.finalizer_poll_budget, 222u);
    ASSERT_EQ(out.finalizer_time_budget_ns, (uint64_t)2222222u);
    asx_runtime_shutdown(&rt);
}

TEST(reload_config_copies_leak_escalation_config) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed, out;
    asx_leak_escalation_config escalation;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    escalation.threshold = 11u;
    escalation.escalate_to = ASX_LEAK_SILENT;
    proposed.leak_escalation = &escalation;

    ASSERT_EQ(asx_runtime_reload_config(&rt, &proposed, NULL), ASX_OK);

    escalation.threshold = 22u;
    escalation.escalate_to = ASX_LEAK_PANIC;

    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_TRUE(out.leak_escalation != NULL);
    ASSERT_TRUE(out.leak_escalation != &escalation);
    ASSERT_EQ(out.leak_escalation->threshold, (uint64_t)11u);
    ASSERT_EQ(out.leak_escalation->escalate_to, ASX_LEAK_SILENT);
    asx_runtime_shutdown(&rt);
}

TEST(reload_config_rejects_restart_required_change_without_mutation) {
    asx_runtime rt;
    asx_runtime_config cfg, proposed, out;
    const char *rejection = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg), ASX_OK);
    proposed = cfg;
    proposed.wait_policy = ASX_WAIT_BUSY_SPIN;
    proposed.max_cancel_chain_memory = cfg.max_cancel_chain_memory + 512u;

    ASSERT_EQ(asx_runtime_reload_config(&rt, &proposed, &rejection), ASX_E_CONFIG_RESTART_REQ);
    ASSERT_TRUE(rejection != NULL);
    ASSERT_STR_EQ(rejection, "max_cancel_chain_memory");
    ASSERT_EQ(asx_runtime_get_config(&rt, &out), ASX_OK);
    ASSERT_EQ(out.wait_policy, cfg.wait_policy);
    ASSERT_EQ(out.max_cancel_chain_memory, cfg.max_cancel_chain_memory);
    asx_runtime_shutdown(&rt);
}

TEST(umbrella_exposes_runtime_support_surfaces) {
    asx_runtime_snapshot snap;
    asx_auto_deadline_tracker dt;
    asx_lab_config lab_cfg;
    asx_vtime_state vt;
    asx_adapter_decision adapter_decision;
    asx_hindsight_policy hindsight_policy;
    asx_perf_snapshot perf_snapshot;
    asx_evidence_sink evidence_sink;
    asx_oracle_suite oracle_suite;

    asx_runtime_snapshot_init(&snap);
    asx_auto_deadline_init(&dt);
    asx_lab_config_init(&lab_cfg);
    asx_vtime_init(&vt, 0u, 1000u);
    asx_event_log_reset();
    asx_hindsight_init();
    asx_telemetry_reset();
    asx_parallel_reset();
    memset(&adapter_decision, 0, sizeof(adapter_decision));
    memset(&hindsight_policy, 0, sizeof(hindsight_policy));
    memset(&perf_snapshot, 0, sizeof(perf_snapshot));
    asx_evidence_sink_init(&evidence_sink);
    asx_oracle_suite_init(&oracle_suite);

    ASSERT_EQ(snap.region_count, 0u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(&dt), 0u);
    ASSERT_EQ(lab_cfg.max_polls, 1024u);
    ASSERT_EQ(asx_vtime_current(&vt), (asx_time)0u);
    ASSERT_EQ(dt.total_deadlines, 0u);
    ASSERT_EQ(asx_event_log_count(), 0u);
    ASSERT_EQ(asx_parallel_cancel_streak_limit(), 16u);
    ASSERT_EQ(asx_parallel_is_initialized(), 0);
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
    ASSERT_EQ(asx_hindsight_readable_count(), 0u);
    ASSERT_STR_EQ(asx_adapter_domain_str(ASX_ADAPTER_DOMAIN_AUTOMOTIVE), "Automotive");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_TASK_SPAWN), "task_spawn");
    ASSERT_STR_EQ(asx_nd_event_kind_str(ASX_ND_IO_READY), "io_ready");
    ASSERT_STR_EQ(asx_telemetry_tier_str(ASX_TELEMETRY_ULTRA_MIN), "ultra_min");
    ASSERT_STR_EQ(asx_adapter_name(ASX_ADAPTER_HFT), "HFT_LATENCY");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_TIMER), "timer");
    ASSERT_EQ(adapter_decision.triggered, 0);
    ASSERT_EQ(evidence_sink.count, 0u);
    ASSERT_EQ(asx_evidence_verdict(&evidence_sink), ASX_EVIDENCE_PASS);
    ASSERT_EQ(oracle_suite.count, 0u);
    ASSERT_EQ(perf_snapshot.total_events, 0u);
    ASSERT_EQ(hindsight_policy.flush_on_invariant, 0);
    ASSERT_EQ(hindsight_policy.flush_on_divergence, 0);
}

/* ------------------------------------------------------------------ */
/* State query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(region_count_null_returns_zero) { ASSERT_EQ(asx_runtime_region_count(NULL), 0u); }

TEST(task_count_null_returns_zero) { ASSERT_EQ(asx_runtime_task_count(NULL), 0u); }

TEST(obligation_count_null_returns_zero) { ASSERT_EQ(asx_runtime_obligation_count(NULL), 0u); }

TEST(io_registration_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_io_registration_count(NULL), 0u);
}

TEST(blocking_active_count_null_returns_zero) {
    ASSERT_EQ(asx_runtime_blocking_active_count(NULL), 0u);
}

TEST(io_driver_initialized_null_returns_false) {
    ASSERT_FALSE(asx_runtime_io_driver_initialized(NULL));
}

TEST(blocking_pool_initialized_null_returns_false) {
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(NULL));
}

TEST(runtime_is_quiescent_null_returns_false) { ASSERT_FALSE(asx_runtime_is_quiescent(NULL)); }

TEST(counts_zero_after_init) {
    asx_runtime rt;
    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_runtime_region_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 0u);
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(counts_reflect_opened_region) {
    asx_runtime rt;
    asx_region_id rid;
    MUST_OK(asx_runtime_init_default(&rt));

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_runtime_region_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(counts_reflect_spawned_task) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    MUST_OK(asx_runtime_init_default(&rt));

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_runtime_task_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(task_count_retains_completed_task_slot_until_reset) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_task_count(&rt), 1u);

    asx_runtime_shutdown(&rt);
}

TEST(counts_uninitialized_returns_zero) {
    asx_runtime rt;
    memset(&rt, 0, sizeof(rt));
    ASSERT_EQ(asx_runtime_region_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
}

TEST(runtime_is_quiescent_false_with_open_region) {
    asx_runtime rt;
    asx_region_id rid;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_live_task) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_true_with_completed_task_in_closed_region) {
    asx_runtime rt;
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, dummy_poll, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;

    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_pending_obligation) {
    asx_runtime rt;
    asx_region_id rid;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_pending_region_cleanup) {
    asx_runtime rt;
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    int cleaned = 0;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);
    ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
    ASSERT_EQ(cleaned, 0);
    asx_runtime_shutdown(&rt);
}

TEST(runtime_is_quiescent_false_with_active_io_registration) {
    asx_runtime rt;

    MUST_OK(asx_runtime_init_default(&rt));

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        asx_waker w;
        asx_io_token tok;

        ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
        ASSERT_EQ(asx_waker_register(88, &w), ASX_OK);
        ASSERT_EQ(asx_io_register(88, ASX_IO_READABLE, &w, &tok), ASX_OK);
        ASSERT_FALSE(asx_runtime_is_quiescent(&rt));
        asx_io_deregister(&tok);
        ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    } else {
        ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
    }
#else
    ASSERT_TRUE(asx_runtime_is_quiescent(&rt));
#endif

    asx_runtime_shutdown(&rt);
}

TEST(runtime_subsystem_queries_track_live_state) {
    asx_runtime rt;
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_waker w;
    asx_io_token tok;
#endif
#if ASX_HAS_BLOCKING_SURFACE
    asx_blocking_handle blocking;
    uint64_t blocking_input = 5u;
#endif

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);

#if ASX_HAS_NATIVE_IO_DRIVER
    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
        ASSERT_TRUE(asx_runtime_io_driver_initialized(&rt));
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
        ASSERT_EQ(asx_waker_register(77, &w), ASX_OK);
        ASSERT_EQ(asx_io_register(77, ASX_IO_READABLE, &w, &tok), ASX_OK);
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 1u);
        asx_io_deregister(&tok);
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    } else {
        ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
        ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_IO_DRIVER));
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
#endif

#if ASX_HAS_BLOCKING_SURFACE
    if (asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        ASSERT_TRUE(asx_runtime_blocking_pool_initialized(&rt));
        ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
        ASSERT_EQ(asx_spawn_blocking(add_one, &blocking_input, NULL, &blocking), ASX_OK);
        ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
    } else {
        ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
        ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
    }
#else
    ASSERT_FALSE(asx_surface_available_active(ASX_SURFACE_BLOCKING));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
#endif

    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_io_driver_initialized(&rt));
    ASSERT_FALSE(asx_runtime_blocking_pool_initialized(&rt));
    ASSERT_EQ(asx_runtime_io_registration_count(&rt), 0u);
    ASSERT_EQ(asx_runtime_blocking_active_count(&rt), 0u);
}

/* ------------------------------------------------------------------ */
/* Capacity query tests                                                */
/* ------------------------------------------------------------------ */

TEST(region_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_region_capacity(), (uint32_t)ASX_MAX_REGIONS);
}

TEST(task_capacity_returns_max) { ASSERT_EQ(asx_runtime_task_capacity(), (uint32_t)ASX_MAX_TASKS); }

TEST(obligation_capacity_returns_max) {
    ASSERT_EQ(asx_runtime_obligation_capacity(), (uint32_t)ASX_MAX_OBLIGATIONS);
}

/* ------------------------------------------------------------------ */
/* Safety/containment query tests                                      */
/* ------------------------------------------------------------------ */

TEST(safety_profile_returns_active) {
    asx_runtime rt;
    asx_safety_profile sp;
    MUST_OK(asx_runtime_init_default(&rt));
    sp = asx_runtime_safety_profile(&rt);
    ASSERT_EQ(sp, asx_safety_profile_active());
    asx_runtime_shutdown(&rt);
}

TEST(containment_policy_returns_active) {
    asx_runtime rt;
    asx_containment_policy cp;
    MUST_OK(asx_runtime_init_default(&rt));
    cp = asx_runtime_containment_policy(&rt);
    ASSERT_EQ(cp, asx_containment_policy_active());
    asx_runtime_shutdown(&rt);
}

TEST(umbrella_runtime_lab_deadline_roundtrip) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario scenario;
    asx_lab_result result;
    asx_auto_deadline_tracker *deadline;
    asx_auto_audit_ring *audit;
    asx_region_id rid = ASX_INVALID_ID;

    asx_auto_instrument_reset();
    deadline = asx_auto_deadline_global();
    audit = asx_auto_audit_global();
    ASSERT_TRUE(deadline != NULL);
    ASSERT_TRUE(audit != NULL);
    ASSERT_EQ(asx_auto_audit_count(audit), 0u);

    asx_auto_record_deadline(1000u, 800u, 1u);
    asx_auto_record_deadline(1000u, 1200u, 2u);
    ASSERT_EQ(deadline->total_deadlines, 2u);
    ASSERT_EQ(deadline->deadline_hits, 1u);
    ASSERT_EQ(deadline->deadline_misses, 1u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(deadline), 5000u);
    ASSERT_EQ(asx_auto_audit_count(audit), 1u);
    ASSERT_STR_EQ(asx_audit_kind_str(ASX_AUDIT_DEADLINE_MISS), "DEADLINE_MISS");

    asx_lab_config_init(&cfg);
    cfg.seed = 42u;
    cfg.tick_ns = 100u;
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_runtime_region_count(&lab.rt), 0u);
    ASSERT_EQ(asx_runtime_task_count(&lab.rt), 0u);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0u);

    asx_lab_scenario_init(&scenario, "runtime-lab-deadline-roundtrip");
    MUST_OK(asx_lab_scenario_add_step(&scenario, lab_noop_step, &rid));
    asx_lab_advance_time(&lab, 3u);
    MUST_OK(asx_lab_run_scenario(&lab, &scenario, &result));

    ASSERT_TRUE(rid != ASX_INVALID_ID);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.steps_total, 1u);
    ASSERT_EQ(result.last_status, ASX_OK);
    ASSERT_EQ(result.elapsed_ns, (asx_time)0u);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)300u);
    ASSERT_EQ(asx_runtime_region_count(&lab.rt), 1u);
    ASSERT_EQ(asx_runtime_safety_profile(&lab.rt), asx_safety_profile_active());
    ASSERT_EQ(asx_runtime_containment_policy(&lab.rt), asx_containment_policy_active());

    asx_lab_shutdown(&lab);
    asx_auto_instrument_reset();
}

/* ------------------------------------------------------------------ */
/* Integration: init → use → shutdown round-trip                       */
/* ------------------------------------------------------------------ */

TEST(full_roundtrip) {
    asx_runtime rt;
    asx_region_id rid;
    asx_obligation_id oid;
    asx_runtime_config cfg_out;

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    ASSERT_TRUE(asx_runtime_is_initialized(&rt));

    /* Open region */
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_runtime_region_count(&rt), 1u);

    /* Reserve and commit obligation */
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_runtime_obligation_count(&rt), 1u);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    /* Query config */
    ASSERT_EQ(asx_runtime_get_config(&rt, &cfg_out), ASX_OK);
    ASSERT_EQ(cfg_out.finalizer_poll_budget, 100u);

    /* Shutdown resets everything */
    asx_runtime_shutdown(&rt);
    ASSERT_FALSE(asx_runtime_is_initialized(&rt));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_rt ===\n");

    /* Config validation */
    RUN_TEST(config_validate_null_fails);
    RUN_TEST(config_validate_bad_size_fails);
    RUN_TEST(config_validate_zero_cancel_depth_fails);
    RUN_TEST(config_validate_zero_finalizer_budget_fails);
    RUN_TEST(config_validate_invalid_wait_policy_fails);
    RUN_TEST(config_validate_invalid_io_backend_fails);
    RUN_TEST(config_validate_invalid_leak_response_fails);
    RUN_TEST(config_validate_invalid_finalizer_escalation_fails);
    RUN_TEST(config_validate_invalid_leak_escalation_fails);
    RUN_TEST(config_validate_defaults_ok);

    /* Init/shutdown lifecycle */
    RUN_TEST(init_null_rt_fails);
    RUN_TEST(init_null_config_fails);
    RUN_TEST(init_null_hooks_fails);
    RUN_TEST(init_bad_config_fails);
    RUN_TEST(init_invalid_enum_config_fails);
    RUN_TEST(init_unsupported_io_backend_fails_closed);
    RUN_TEST(init_invalid_leak_escalation_config_fails);
    RUN_TEST(init_success);
    RUN_TEST(init_generation_increments);
    RUN_TEST(stale_runtime_reports_uninitialized_after_new_init);
    RUN_TEST(shutdown_clears_state);
    RUN_TEST(shutdown_null_safe);
    RUN_TEST(shutdown_double_safe);
    RUN_TEST(stale_shutdown_does_not_tear_down_active_runtime);
    RUN_TEST(shutdown_uninitialized_safe);

    /* Default init */
    RUN_TEST(init_default_null_fails);
    RUN_TEST(init_from_env_null_rt_fails);
    RUN_TEST(init_default_success);
    RUN_TEST(init_from_env_applies_default_prefix_overrides);
    RUN_TEST(init_from_env_applies_custom_prefix_overrides);
    RUN_TEST(init_from_env_rejects_invalid_env_without_initializing);
    RUN_TEST(init_from_env_rejects_unsupported_io_backend_without_initializing);
    RUN_TEST(init_default_wires_blocking_surface);
    RUN_TEST(init_default_wires_io_surface);
    RUN_TEST(init_default_io_poll_wakes_registered_task);

    /* is_initialized */
    RUN_TEST(is_initialized_null_returns_false);
    RUN_TEST(is_initialized_zeroed_returns_false);

    /* Config/hooks retrieval */
    RUN_TEST(get_config_null_rt_fails);
    RUN_TEST(get_config_null_out_fails);
    RUN_TEST(get_config_uninitialized_fails);
    RUN_TEST(get_config_returns_stored_config);
    RUN_TEST(runtime_io_backend_query_tracks_runtime_config);
    RUN_TEST(init_copies_leak_escalation_config);
    RUN_TEST(get_hooks_null_rt_fails);
    RUN_TEST(get_hooks_null_out_fails);
    RUN_TEST(get_hooks_uninitialized_fails);
    RUN_TEST(validate_reload_uninitialized_fails);
    RUN_TEST(validate_reload_reports_restart_required_field);
    RUN_TEST(reload_config_updates_reloadable_fields);
    RUN_TEST(reload_config_copies_leak_escalation_config);
    RUN_TEST(reload_config_rejects_restart_required_change_without_mutation);
    RUN_TEST(umbrella_exposes_runtime_support_surfaces);

    /* State queries */
    RUN_TEST(region_count_null_returns_zero);
    RUN_TEST(task_count_null_returns_zero);
    RUN_TEST(obligation_count_null_returns_zero);
    RUN_TEST(io_registration_count_null_returns_zero);
    RUN_TEST(blocking_active_count_null_returns_zero);
    RUN_TEST(io_driver_initialized_null_returns_false);
    RUN_TEST(blocking_pool_initialized_null_returns_false);
    RUN_TEST(runtime_is_quiescent_null_returns_false);
    RUN_TEST(counts_zero_after_init);
    RUN_TEST(counts_reflect_opened_region);
    RUN_TEST(counts_reflect_spawned_task);
    RUN_TEST(task_count_retains_completed_task_slot_until_reset);
    RUN_TEST(counts_uninitialized_returns_zero);
    RUN_TEST(runtime_is_quiescent_false_with_open_region);
    RUN_TEST(runtime_is_quiescent_false_with_live_task);
    RUN_TEST(runtime_is_quiescent_true_with_completed_task_in_closed_region);
    RUN_TEST(runtime_is_quiescent_false_with_pending_obligation);
    RUN_TEST(runtime_is_quiescent_false_with_pending_region_cleanup);
    RUN_TEST(runtime_is_quiescent_false_with_active_io_registration);
    RUN_TEST(runtime_subsystem_queries_track_live_state);

    /* Capacity queries */
    RUN_TEST(region_capacity_returns_max);
    RUN_TEST(task_capacity_returns_max);
    RUN_TEST(obligation_capacity_returns_max);

    /* Safety/containment */
    RUN_TEST(safety_profile_returns_active);
    RUN_TEST(containment_policy_returns_active);
    RUN_TEST(umbrella_runtime_lab_deadline_roundtrip);

    /* Integration */
    RUN_TEST(full_roundtrip);

    TEST_REPORT();
    return test_failures;
}
