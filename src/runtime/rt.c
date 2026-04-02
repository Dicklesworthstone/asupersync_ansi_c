/*
 * rt.c — runtime object lifecycle and state queries
 *
 * Wraps the existing global state management into an explicit
 * runtime object with deterministic bootstrap sequencing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/asx_config.h>
#include <asx/core/transition.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/builder.h>
#include <asx/runtime/config_reload.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <stddef.h>
#include <string.h>

asx_io_backend asx_runtime_active_io_backend_selected(void);

/* Generation counter for runtime instances */
static uint32_t g_rt_generation = 1u;
static uint32_t g_active_rt_generation = 0u;
static asx_io_backend g_active_io_backend = ASX_IO_BACKEND_GHOST;

static int runtime_wait_policy_valid(asx_wait_policy policy) {
    switch (policy) {
    case ASX_WAIT_BUSY_SPIN:
    case ASX_WAIT_YIELD:
    case ASX_WAIT_SLEEP: return 1;
    }
    return 0;
}

static int runtime_leak_response_valid(asx_leak_response response) {
    switch (response) {
    case ASX_LEAK_PANIC:
    case ASX_LEAK_LOG:
    case ASX_LEAK_SILENT:
    case ASX_LEAK_RECOVER: return 1;
    }
    return 0;
}

static int runtime_io_backend_known(asx_io_backend backend) {
    switch (backend) {
    case ASX_IO_BACKEND_GHOST:
    case ASX_IO_BACKEND_IO_URING: return 1;
    }
    return 0;
}

static int runtime_io_backend_supported(asx_io_backend backend) {
    switch (backend) {
    case ASX_IO_BACKEND_GHOST: return 1;
    case ASX_IO_BACKEND_IO_URING: return ASX_HAS_IO_URING;
    }
    return 0;
}

static int runtime_has_pending_obligations(void) {
    uint32_t i;

    for (i = 0; i < g_obligation_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static obligation slots") */
        if (!g_obligations[i].alive) continue;
        if (g_obligations[i].state == ASX_OBLIGATION_RESERVED) return 1;
    }

    return 0;
}

static int runtime_has_live_tasks(void) {
    uint32_t i;

    for (i = 0; i < g_task_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static task slots") */
        if (!g_tasks[i].alive) continue;
        if (asx_task_is_terminal(g_tasks[i].state)) continue;
        return 1;
    }

    return 0;
}

static int runtime_has_pending_region_cleanup(void) {
    uint32_t i;

    for (i = 0; i < g_region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static region slots") */
        if (!g_regions[i].alive) continue;
        if (asx_cleanup_pending(&g_regions[i].cleanup) != 0u) return 1;
    }

    return 0;
}

static int runtime_all_regions_closed(void) {
    uint32_t i;

    for (i = 0; i < g_region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static region slots") */
        if (!g_regions[i].alive) continue;
        if (g_regions[i].state != ASX_REGION_CLOSED) return 0;
    }

    return 1;
}

static int runtime_finalizer_escalation_valid(asx_finalizer_escalation escalation) {
    switch (escalation) {
    case ASX_FINALIZER_SOFT:
    case ASX_FINALIZER_BOUNDED_LOG:
    case ASX_FINALIZER_BOUNDED_PANIC: return 1;
    }
    return 0;
}

static int runtime_leak_escalation_valid(const asx_leak_escalation_config *config) {
    if (config == NULL) return 1;
    return runtime_leak_response_valid(config->escalate_to);
}

static void runtime_config_copy(asx_runtime *rt, const asx_runtime_config *config) {
    rt->config = *config;
    rt->has_leak_escalation = 0;
    rt->config.leak_escalation = NULL;
    memset(&rt->leak_escalation_storage, 0, sizeof(rt->leak_escalation_storage));
    if (config->leak_escalation != NULL) {
        rt->leak_escalation_storage = *config->leak_escalation;
        rt->config.leak_escalation = &rt->leak_escalation_storage;
        rt->has_leak_escalation = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Config validation                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_config_validate(const asx_runtime_config *config) {
    if (config == NULL) return ASX_E_INVALID_ARGUMENT;
    if (config->size != (uint32_t)sizeof(asx_runtime_config)) return ASX_E_INVALID_ARGUMENT;
    if (!runtime_wait_policy_valid(config->wait_policy)) return ASX_E_INVALID_ARGUMENT;
    if (!runtime_io_backend_known(config->io_backend)) return ASX_E_INVALID_ARGUMENT;
    if (!runtime_leak_response_valid(config->leak_response)) return ASX_E_INVALID_ARGUMENT;
    if (!runtime_leak_escalation_valid(config->leak_escalation)) return ASX_E_INVALID_ARGUMENT;
    if (!runtime_finalizer_escalation_valid(config->finalizer_escalation))
        return ASX_E_INVALID_ARGUMENT;
    if (config->max_cancel_chain_depth == 0u) return ASX_E_INVALID_ARGUMENT;
    if (config->finalizer_poll_budget == 0u) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_init(asx_runtime *rt, const asx_runtime_config *config,
                            const asx_runtime_hooks *hooks) {
    asx_status st;

    if (rt == NULL || config == NULL || hooks == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Step 1: validate config */
    st = asx_runtime_config_validate(config);
    if (st != ASX_OK) return st;
    if (!runtime_io_backend_supported(config->io_backend)) return ASX_E_PERMISSION_DENIED;

    /* Step 2: validate hooks */
    st = asx_runtime_hooks_validate(hooks, ASX_DETERMINISTIC);
    if (st != ASX_OK) return st;

    /* Step 3: reset all internal state (deterministic clean slate) */
    asx_runtime_reset();
    g_active_rt_generation = 0u;
    g_active_io_backend = ASX_IO_BACKEND_GHOST;

    /* Step 4: install hooks */
    st = asx_runtime_set_hooks(hooks);
    if (st != ASX_OK) return st;

    g_active_io_backend = config->io_backend;

    /* Step 4a: initialize shipped runtime subsystems.
     * Browser profile builds fail closed for native-only surfaces, which is
     * expected and should not prevent the runtime object itself from starting. */
#if ASX_HAS_NATIVE_IO_DRIVER
    st = asx_io_driver_init();
    if (st != ASX_OK && st != ASX_E_PERMISSION_DENIED) {
        g_active_io_backend = ASX_IO_BACKEND_GHOST;
        asx_runtime_reset();
        return st;
    }
#endif

#if ASX_HAS_BLOCKING_SURFACE
    st = asx_blocking_pool_init();
    if (st != ASX_OK && st != ASX_E_PERMISSION_DENIED) {
        asx_io_driver_shutdown();
        g_active_io_backend = ASX_IO_BACKEND_GHOST;
        asx_runtime_reset();
        return st;
    }
#endif

    /* Step 5: store config and mark initialized */
    memset(rt, 0, sizeof(*rt));
    runtime_config_copy(rt, config);
    rt->hooks = *hooks;
    rt->generation = g_rt_generation++;
    rt->initialized = 1;
    g_active_rt_generation = rt->generation;

    return ASX_OK;
}

asx_status asx_runtime_init_default(asx_runtime *rt) {
    asx_runtime_config config;
    asx_runtime_hooks hooks;
    asx_status st;

    if (rt == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_runtime_config_init(&config);

    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return st;

    return asx_runtime_init(rt, &config, &hooks);
}

asx_status asx_runtime_init_from_env(asx_runtime *rt, const char *prefix) {
    asx_runtime_builder builder;
    asx_status st;

    if (rt == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_builder_init(&builder);
    if (st != ASX_OK) return st;

    st = asx_runtime_builder_apply_env(&builder, prefix);
    if (st != ASX_OK) return st;

    return asx_runtime_builder_build(&builder, rt);
}

void asx_runtime_shutdown(asx_runtime *rt) {
    if (rt == NULL) return;
    if (asx_runtime_is_initialized(rt)) {
#if ASX_HAS_NATIVE_IO_DRIVER
        asx_io_driver_shutdown();
#endif
#if ASX_HAS_BLOCKING_SURFACE
        asx_blocking_pool_shutdown();
#endif
        asx_runtime_reset();
        g_active_rt_generation = 0u;
        g_active_io_backend = ASX_IO_BACKEND_GHOST;
    }
    memset(rt, 0, sizeof(*rt));
}

int asx_runtime_is_initialized(const asx_runtime *rt) {
    if (rt == NULL) return 0;
    return rt->initialized && rt->generation != 0u && rt->generation == g_active_rt_generation;
}

/* ------------------------------------------------------------------ */
/* Configuration queries                                               */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_get_config(const asx_runtime *rt, asx_runtime_config *out) {
    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_runtime_is_initialized(rt)) return ASX_E_INVALID_STATE;
    *out = rt->config;
    return ASX_OK;
}

asx_status asx_runtime_get_hooks_from(const asx_runtime *rt, asx_runtime_hooks *out) {
    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_runtime_is_initialized(rt)) return ASX_E_INVALID_STATE;
    *out = rt->hooks;
    return ASX_OK;
}

asx_status asx_runtime_validate_reload_config(const asx_runtime *rt,
                                              const asx_runtime_config *proposed,
                                              const char **rejection_field) {
    asx_config_state state;
    asx_status st;

    if (rt == NULL || proposed == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_runtime_is_initialized(rt)) return ASX_E_INVALID_STATE;

    st = asx_runtime_config_validate(proposed);
    if (st != ASX_OK) return st;

    asx_config_state_init(&state);
    st = asx_config_load(&state, &rt->config);
    if (st != ASX_OK) return st;

    return asx_config_validate_reload(&state, proposed, rejection_field);
}

asx_status asx_runtime_reload_config(asx_runtime *rt, const asx_runtime_config *proposed,
                                     const char **rejection_field) {
    asx_status st;

    if (rt == NULL || proposed == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_validate_reload_config(rt, proposed, rejection_field);
    if (st != ASX_OK) return st;

    runtime_config_copy(rt, proposed);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* State queries                                                       */
/* ------------------------------------------------------------------ */

uint32_t asx_runtime_region_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
    return g_region_count;
}

uint32_t asx_runtime_task_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
    return g_task_count;
}

uint32_t asx_runtime_obligation_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
    return g_obligation_count;
}

int asx_runtime_is_quiescent(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0;
    if (runtime_has_live_tasks()) return 0;
    if (runtime_has_pending_obligations()) return 0;
    if (asx_runtime_io_registration_count(rt) != 0u) return 0;
    if (runtime_has_pending_region_cleanup()) return 0;
    if (!runtime_all_regions_closed()) return 0;
    return 1;
}

int asx_runtime_io_driver_initialized(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0;
#if ASX_HAS_NATIVE_IO_DRIVER
    return asx_io_driver_is_initialized();
#else
    return 0;
#endif
}

uint32_t asx_runtime_io_registration_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
#if ASX_HAS_NATIVE_IO_DRIVER
    return asx_io_active_count();
#else
    return 0u;
#endif
}

int asx_runtime_blocking_pool_initialized(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0;
#if ASX_HAS_BLOCKING_SURFACE
    return asx_blocking_pool_is_initialized();
#else
    return 0;
#endif
}

uint32_t asx_runtime_blocking_active_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
#if ASX_HAS_BLOCKING_SURFACE
    return asx_blocking_active_count();
#else
    return 0u;
#endif
}

uint32_t asx_runtime_region_capacity(void) { return ASX_MAX_REGIONS; }

uint32_t asx_runtime_task_capacity(void) { return ASX_MAX_TASKS; }

uint32_t asx_runtime_obligation_capacity(void) { return ASX_MAX_OBLIGATIONS; }

asx_safety_profile asx_runtime_safety_profile(const asx_runtime *rt) {
    (void)rt;
    return asx_safety_profile_active();
}

asx_containment_policy asx_runtime_containment_policy(const asx_runtime *rt) {
    (void)rt;
    return asx_containment_policy_active();
}

asx_io_backend asx_runtime_io_backend(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return ASX_IO_BACKEND_GHOST;
    return rt->config.io_backend;
}

asx_io_backend asx_runtime_active_io_backend_selected(void) { return g_active_io_backend; }
