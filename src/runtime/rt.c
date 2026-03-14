/*
 * rt.c — runtime object lifecycle and state queries
 *
 * Wraps the existing global state management into an explicit
 * runtime object with deterministic bootstrap sequencing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <stddef.h>
#include <string.h>

/* Forward declarations for global state access (defined in lifecycle.c) */
extern uint32_t g_region_count;
extern uint32_t g_task_count;
extern uint32_t g_obligation_count;

/* Generation counter for runtime instances */
static uint32_t g_rt_generation = 1u;

/* ------------------------------------------------------------------ */
/* Config validation                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_config_validate(const asx_runtime_config *config) {
    if (config == NULL) return ASX_E_INVALID_ARGUMENT;
    if (config->size != (uint32_t)sizeof(asx_runtime_config)) return ASX_E_INVALID_ARGUMENT;
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

    /* Step 2: validate hooks */
    st = asx_runtime_hooks_validate(hooks, ASX_DETERMINISTIC);
    if (st != ASX_OK) return st;

    /* Step 3: reset all internal state (deterministic clean slate) */
    asx_runtime_reset();

    /* Step 4: install hooks */
    st = asx_runtime_set_hooks(hooks);
    if (st != ASX_OK) return st;

    /* Step 4a: initialize shipped runtime subsystems.
     * Browser profile builds fail closed for native-only surfaces, which is
     * expected and should not prevent the runtime object itself from starting. */
    st = asx_io_driver_init();
    if (st != ASX_OK && st != ASX_E_PERMISSION_DENIED) {
        asx_runtime_reset();
        return st;
    }

    st = asx_blocking_pool_init();
    if (st != ASX_OK && st != ASX_E_PERMISSION_DENIED) {
        asx_io_driver_shutdown();
        asx_runtime_reset();
        return st;
    }

    /* Step 5: store config and mark initialized */
    memset(rt, 0, sizeof(*rt));
    rt->config = *config;
    rt->hooks = *hooks;
    rt->generation = g_rt_generation++;
    rt->initialized = 1;

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

void asx_runtime_shutdown(asx_runtime *rt) {
    if (rt == NULL) return;
    if (rt->initialized) {
        asx_io_driver_shutdown();
        asx_blocking_pool_shutdown();
        asx_runtime_reset();
    }
    memset(rt, 0, sizeof(*rt));
}

int asx_runtime_is_initialized(const asx_runtime *rt) {
    if (rt == NULL) return 0;
    return rt->initialized && rt->generation != 0u;
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
