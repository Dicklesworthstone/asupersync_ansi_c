/*
 * asx/runtime/rt.h — runtime object lifecycle and state queries
 *
 * Provides explicit runtime creation, configuration, and teardown.
 * The runtime object manages the bootstrap sequence, ensuring
 * deterministic initialization order and making configuration
 * explicit rather than relying on hidden global state.
 *
 * Usage:
 *   asx_runtime rt;
 *   asx_runtime_config cfg;
 *   asx_runtime_hooks hooks;
 *
 *   asx_runtime_config_init(&cfg);
 *   asx_runtime_hooks_init(&hooks);
 *   asx_runtime_init(&rt, &cfg, &hooks);
 *   // ... use runtime ...
 *   asx_runtime_shutdown(&rt);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_RT_H
#define ASX_RUNTIME_RT_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Runtime object                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_runtime_config config;
    asx_runtime_hooks hooks;
    uint32_t generation; /* nonzero when initialized */
    int initialized;     /* guard flag */
} asx_runtime;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Initialize a runtime with explicit configuration and hooks.
 *
 * Performs the full bootstrap sequence:
 *   1. Validate config (size field, bounds)
 *   2. Validate hooks (deterministic constraints)
 *   3. Reset all internal state (arenas, scheduler, trace, etc.)
 *   4. Install hooks
 *   5. Mark runtime as initialized
 *
 * Preconditions: rt, config, hooks must not be NULL.
 *   config->size must equal sizeof(asx_runtime_config).
 * Postconditions: runtime is initialized and ready for use.
 * Returns ASX_OK on success,
 *   ASX_E_INVALID_ARGUMENT if any pointer is NULL or config->size is wrong,
 *   ASX_E_HOOK_INVALID if hook validation fails.
 *
 * Thread-safety: not thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_runtime_init(asx_runtime *rt, const asx_runtime_config *config,
                                                 const asx_runtime_hooks *hooks);

/* Initialize a runtime with all defaults (profile-appropriate config,
 * safe default hooks). Convenience wrapper over asx_runtime_init.
 *
 * Preconditions: rt must not be NULL.
 * Postconditions: runtime is initialized with defaults.
 * Returns ASX_OK on success.
 *
 * Thread-safety: not thread-safe. */
ASX_API ASX_MUST_USE asx_status asx_runtime_init_default(asx_runtime *rt);

/* Shut down a runtime, resetting all internal state.
 * After shutdown, the runtime must be re-initialized before use.
 *
 * Preconditions: rt must not be NULL.
 * Postconditions: all arenas/scheduler/trace/hooks are reset;
 *   rt->initialized is cleared.
 * Safe to call on an already-shutdown or never-initialized runtime.
 *
 * Thread-safety: not thread-safe. */
ASX_API void asx_runtime_shutdown(asx_runtime *rt);

/* Check if a runtime is initialized.
 * Returns nonzero if initialized, 0 otherwise. */
ASX_API int asx_runtime_is_initialized(const asx_runtime *rt);

/* ------------------------------------------------------------------ */
/* Configuration queries                                               */
/* ------------------------------------------------------------------ */

/* Get the active configuration from a runtime.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if NULL,
 *   ASX_E_INVALID_STATE if runtime is not initialized. */
ASX_API ASX_MUST_USE asx_status asx_runtime_get_config(const asx_runtime *rt,
                                                       asx_runtime_config *out);

/* Get the active hooks from a runtime.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if NULL,
 *   ASX_E_INVALID_STATE if runtime is not initialized. */
ASX_API ASX_MUST_USE asx_status asx_runtime_get_hooks_from(const asx_runtime *rt,
                                                           asx_runtime_hooks *out);

/* Validate a runtime config without initializing.
 * Checks size field, bounds on fields (cancel chain depth, etc.).
 * Returns ASX_OK if valid, ASX_E_INVALID_ARGUMENT if NULL or invalid. */
ASX_API ASX_MUST_USE asx_status asx_runtime_config_validate(const asx_runtime_config *config);

/* ------------------------------------------------------------------ */
/* State queries                                                       */
/* ------------------------------------------------------------------ */

/* Query counts of active entities.
 * Returns 0 if rt is NULL or not initialized. */
ASX_API uint32_t asx_runtime_region_count(const asx_runtime *rt);
ASX_API uint32_t asx_runtime_task_count(const asx_runtime *rt);
ASX_API uint32_t asx_runtime_obligation_count(const asx_runtime *rt);

/* Query arena capacities (compile-time constants, but useful for
 * downstream tracks to check without hardcoding). */
ASX_API uint32_t asx_runtime_region_capacity(void);
ASX_API uint32_t asx_runtime_task_capacity(void);
ASX_API uint32_t asx_runtime_obligation_capacity(void);

/* Query the runtime's profile and resource class. */
ASX_API asx_safety_profile asx_runtime_safety_profile(const asx_runtime *rt);
ASX_API asx_containment_policy asx_runtime_containment_policy(const asx_runtime *rt);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_RT_H */
