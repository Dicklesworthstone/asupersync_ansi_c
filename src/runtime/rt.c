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
#include <asx/fs/fs.h>
#include <asx/net/net.h>
#include <asx/process/process.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/builder.h>
#include <asx/runtime/config_reload.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/signal/signal.h>
#include <stddef.h>
#include <string.h>
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
#include <asx/platform/posix.h>
#endif
#if defined(ASX_PROFILE_WIN32) && !ASX_DETERMINISTIC
#include <asx/platform/win32.h>
#endif

asx_io_backend asx_runtime_active_io_backend_selected(void);

/* Generation counter for runtime instances (a kernel records the one that
 * holds it: asx_kernel_hold) */
static uint32_t g_rt_generation = 1u;
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

static int runtime_has_pending_obligations(const asx_kernel *k) {
    uint32_t i;

    for (i = 0; i < k->obligation_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static obligation slots") */
        if (!k->obligations[i].alive) continue;
        if (k->obligations[i].state == ASX_OBLIGATION_RESERVED) return 1;
    }

    return 0;
}

static int runtime_has_live_tasks(const asx_kernel *k) {
    uint32_t i;

    for (i = 0; i < k->task_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static task slots") */
        if (!k->tasks[i].alive) continue;
        if (asx_task_is_terminal(k->tasks[i].state)) continue;
        return 1;
    }

    return 0;
}

static int runtime_has_pending_region_cleanup(const asx_kernel *k) {
    uint32_t i;

    for (i = 0; i < k->region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static region slots") */
        if (!k->regions[i].alive) continue;
        if (asx_cleanup_pending(&k->regions[i].cleanup) != 0u) return 1;
    }

    return 0;
}

/* Rust's RegionState::is_closing (record/region.rs:162-164): Closing,
 * Draining or Finalizing. An Open region is not closing. */
static int runtime_has_closing_region(const asx_kernel *k) {
    uint32_t i;

    for (i = 0; i < k->region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan over static region slots") */
        if (!k->regions[i].alive) continue;
        if (k->regions[i].state != ASX_REGION_OPEN && k->regions[i].state != ASX_REGION_CLOSED) {
            return 1;
        }
    }

    return 0;
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
    if (config->cleanup_hard_bound > 1u) return ASX_E_INVALID_ARGUMENT;
    /* A caller's block must fit, be aligned and not be the library's own. */
    return asx_kernel_storage_validate_internal(config->storage, config->storage_size);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_init_checked(asx_runtime *rt, const asx_runtime_config *config,
                                    const asx_runtime_hooks *hooks,
                                    uint64_t application_fingerprint) {
    asx_status st;

    if (application_fingerprint != asx_runtime_config_fingerprint()) return ASX_E_CONFIG_MISMATCH;
    if (rt == NULL || config == NULL || hooks == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Step 1: validate config */
    st = asx_runtime_config_validate(config);
    if (st != ASX_OK) return st;
    if (!runtime_io_backend_supported(config->io_backend)) return ASX_E_PERMISSION_DENIED;

    /* Step 2: validate hooks */
    st = asx_runtime_hooks_validate(hooks, ASX_DETERMINISTIC);
    if (st != ASX_OK) return st;
    /* Initialization accepts an uninitialized destination: do not inspect
     * its bytes to authorize replacing the active process-wide runtime. */
    if (asx_kernel_any_held()) return ASX_E_INVALID_STATE;
    /* Step 3: bind the runtime's state storage (the caller's block, zeroed,
     * or the library's) and reset all internal state (deterministic clean
     * slate). Nothing above wrote any state, so a refusal left it intact. */
    asx_kernel_bind_internal(config->storage);
    asx_runtime_clear_internal(g_kernels[0]);

    /* Step 4: install hooks */
    st = asx_runtime_set_hooks(hooks);
    if (st != ASX_OK) {
        asx_runtime_reset(); /* unbind: a failed init leaves no binding */
        return st;
    }

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

#if ASX_HAS_NATIVE_RUNTIME_SURFACES && !ASX_DETERMINISTIC
    /* A live readiness reactor means real I/O: sockets, files, child
     * processes and signal subscriptions created from now on use the
     * native backends (where the platform provides them; elsewhere the
     * setters refuse and the deterministic models stay selected). */
    if (asx_io_driver_is_live()) {
        st = asx_net_set_backend(ASX_NET_BACKEND_NATIVE);
        (void)st;
        st = asx_fs_set_backend(ASX_FS_BACKEND_NATIVE);
        (void)st;
        st = asx_process_set_backend(ASX_PROCESS_BACKEND_NATIVE);
        (void)st;
        st = asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE);
        (void)st;
    }
#endif

    /* Step 5: store config and mark initialized */
    memset(rt, 0, sizeof(*rt));
    runtime_config_copy(rt, config);
    asx_runtime_set_leak_policy_internal(g_kernels[0], config->leak_response,
                                         config->leak_escalation);
    asx_runtime_set_cleanup_hard_bound_internal(g_kernels[0], config->cleanup_hard_bound != 0u);
    asx_cancel_chain_limits_set_internal(config->max_cancel_chain_depth,
                                         config->max_cancel_chain_memory);
    asx_region_finalizer_config_set_internal(g_kernels[0], config->finalizer_poll_budget,
                                             config->finalizer_time_budget_ns,
                                             config->finalizer_escalation);
    rt->hooks = *hooks;
    rt->generation = g_rt_generation++;
    rt->initialized = 1;
    asx_kernel_hold(rt);

    return ASX_OK;
}

asx_status asx_runtime_init_default_checked(asx_runtime *rt, uint64_t application_fingerprint) {
    asx_runtime_config config;
    asx_runtime_hooks hooks;
    asx_status st;

    if (application_fingerprint != asx_runtime_config_fingerprint()) return ASX_E_CONFIG_MISMATCH;
    if (rt == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_runtime_config_init_checked(&config, application_fingerprint);

#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    /* Live POSIX builds default to the real platform: monotonic clock,
     * OS entropy, epoll/poll readiness reactor, blocking pool. */
    st = asx_posix_hooks_install_checked(&hooks, application_fingerprint);
#elif defined(ASX_PROFILE_WIN32) && !ASX_DETERMINISTIC
    /* Live WIN32 defaults take the platform's entropy (BCryptGenRandom),
     * so the WebSocket keys, session IDs and CSRF tokens drawn from the
     * runtime are not the default table's fixed-seed PRNG. Its clocks and
     * reactor stay the table's. Profiles with no platform source keep the
     * default table, and their security draws are refused
     * (asx_runtime_random_secure_u64). */
    st = asx_runtime_hooks_init_checked(&hooks, application_fingerprint);
    if (st == ASX_OK) {
        asx_runtime_hooks win32_hooks;
        st = asx_win32_hooks_install_checked(&win32_hooks, application_fingerprint);
        if (st == ASX_OK) {
            hooks.entropy = win32_hooks.entropy;
            hooks.deterministic_seeded_prng = win32_hooks.deterministic_seeded_prng;
        }
    }
#else
    st = asx_runtime_hooks_init_checked(&hooks, application_fingerprint);
#endif
    if (st != ASX_OK) return st;

    return asx_runtime_init_checked(rt, &config, &hooks, application_fingerprint);
}

asx_status asx_runtime_init_from_env_checked(asx_runtime *rt, const char *prefix,
                                             uint64_t application_fingerprint) {
    asx_runtime_builder builder;
    asx_status st;

    if (application_fingerprint != asx_runtime_config_fingerprint()) return ASX_E_CONFIG_MISMATCH;
    if (rt == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_runtime_builder_init_checked(&builder, application_fingerprint);
    if (st != ASX_OK) return st;

    st = asx_runtime_builder_apply_env(&builder, prefix);
    if (st != ASX_OK) return st;

    return asx_runtime_builder_build_checked(&builder, rt, application_fingerprint);
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
    }
    memset(rt, 0, sizeof(*rt));
}

int asx_runtime_is_initialized(const asx_runtime *rt) {
    if (rt == NULL) return 0;
    return rt->initialized && rt->generation != 0u && asx_kernel_holds(rt->tag, rt->generation);
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
    asx_runtime_set_leak_policy_internal(asx_kernel_for_tag(rt->tag), proposed->leak_response,
                                         proposed->leak_escalation);
    asx_region_finalizer_config_set_internal(asx_kernel_for_tag(rt->tag),
                                             proposed->finalizer_poll_budget,
                                             proposed->finalizer_time_budget_ns,
                                             proposed->finalizer_escalation);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* State queries                                                       */
/* ------------------------------------------------------------------ */

uint32_t asx_runtime_region_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
    return asx_kernel_for_tag(rt->tag)->region_live;
}

uint32_t asx_runtime_task_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
    return asx_kernel_for_tag(rt->tag)->task_live;
}

uint32_t asx_runtime_obligation_count(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0u;
    return asx_kernel_for_tag(rt->tag)->obligation_live;
}

/* Rust's RuntimeState::is_quiescent (runtime/state.rs:7400-7413): no live
 * task, no pending obligation, no pending cancel dispatch, an empty I/O
 * driver, and no region with a pending finalizer, still closing, or with
 * a pending spawn. An idle Open region is quiescent. Under lab dispatch
 * the deferred spawns and cancel commands are the lab's mailboxes. */
int asx_runtime_is_quiescent(const asx_runtime *rt) {
    if (rt == NULL || !asx_runtime_is_initialized(rt)) return 0;
    if (runtime_has_live_tasks(asx_kernel_for_tag(rt->tag))) return 0;
    if (runtime_has_pending_obligations(asx_kernel_for_tag(rt->tag))) return 0;
    /* ASX_ANALYZER_WAIVER("config-dependent: 0 without ASX_HAS_NATIVE_IO_DRIVER") */
    if (asx_runtime_io_registration_count(rt) != 0u) return 0;
    if (runtime_has_pending_region_cleanup(asx_kernel_for_tag(rt->tag))) return 0;
    if (runtime_has_closing_region(asx_kernel_for_tag(rt->tag))) return 0;
    if (asx_lab_admissions_pending() || asx_lab_region_commands_pending() ||
        asx_lab_handle_cancels_pending()) {
        return 0;
    }
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

/* Raw reset is an explicit teardown of the runtime that holds `k`: its hold
 * on the kernel goes, another runtime's does not. The io backend choice is
 * process-wide and goes with the last held kernel. */
void asx_runtime_object_reset_internal(asx_kernel *k) {
    k->rt_generation = 0u;
    if (!asx_kernel_other_held(k)) g_active_io_backend = ASX_IO_BACKEND_GHOST;
}
