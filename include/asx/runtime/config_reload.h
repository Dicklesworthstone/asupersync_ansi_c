/*
 * asx/runtime/config_reload.h — Hot-reload config boundaries (bd-3vt.9)
 *
 * Defines which runtime configuration fields are safe to modify
 * during execution and provides atomic reload with validation.
 *
 * Field classification:
 *   FROZEN_COMPILE  — compile-time only (profile, deterministic mode)
 *   FROZEN_INIT     — set once at hook install (allocator, clock hooks)
 *   RELOADABLE      — safe to change mid-run (wait policy, budgets)
 *   RESTART_REQUIRED — requires runtime reset to take effect
 *
 * Reload protocol:
 *   1. Caller prepares new config via asx_runtime_config_init()
 *   2. Caller modifies only RELOADABLE fields
 *   3. asx_config_reload() validates + applies atomically
 *   4. If validation fails, no changes are applied (rollback)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_CONFIG_RELOAD_H
#define ASX_RUNTIME_CONFIG_RELOAD_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Field reload classification                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CONFIG_FROZEN_COMPILE = 0,  /* Compile-time only */
    ASX_CONFIG_FROZEN_INIT = 1,     /* Set once at hook install */
    ASX_CONFIG_RELOADABLE = 2,      /* Safe to change mid-run */
    ASX_CONFIG_RESTART_REQUIRED = 3 /* Needs runtime reset */
} asx_config_reload_class;

/* ------------------------------------------------------------------ */
/* Field descriptor (for introspection and validation)                 */
/* ------------------------------------------------------------------ */

#define ASX_CONFIG_MAX_FIELDS 16u

typedef struct {
    const char *name;
    asx_config_reload_class reload_class;
    uint32_t offset;     /* byte offset in asx_runtime_config */
    uint32_t field_size; /* sizeof(field) */
} asx_config_field_desc;

/* ------------------------------------------------------------------ */
/* Config snapshot (for atomic reload with rollback)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_runtime_config active; /* Currently active config */
    int loaded;                /* 1 if config has been loaded */
} asx_config_state;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Initialize the config state from defaults.
 * Must be called before any reload. */
ASX_API void asx_config_state_init(asx_config_state *state);

/* Load initial config (sets active config + marks as loaded).
 * Returns ASX_OK or ASX_E_INVALID_ARGUMENT. */
ASX_API asx_status asx_config_load(asx_config_state *state, const asx_runtime_config *cfg);

/* Reload config atomically with validation.
 * Only RELOADABLE fields may differ from the active config.
 * If any FROZEN or RESTART_REQUIRED field changed, returns error
 * and no changes are applied (rollback guarantee).
 *
 * Returns:
 *   ASX_OK                    — reload succeeded
 *   ASX_E_INVALID_ARGUMENT    — NULL argument
 *   ASX_E_INVALID_STATE       — config not yet loaded
 *   ASX_E_CONFIG_FROZEN       — attempted to change frozen field
 *   ASX_E_CONFIG_RESTART_REQ  — field requires restart to change
 */
ASX_API asx_status asx_config_reload(asx_config_state *state, const asx_runtime_config *new_cfg);

/* Query the currently active config (read-only view). */
ASX_API const asx_runtime_config *asx_config_active(const asx_config_state *state);

/* Query the reload classification of a named field.
 * Returns the classification or ASX_CONFIG_FROZEN_COMPILE if unknown. */
ASX_API asx_config_reload_class asx_config_field_class(const char *name);

/* Get the full field descriptor table.
 * *count is set to the number of fields. */
ASX_API const asx_config_field_desc *asx_config_field_table(uint32_t *count);

/* Validate a proposed config against reload rules.
 * Fills rejection_field with the name of the first offending field
 * (or NULL if all pass). Returns ASX_OK if reloadable.
 * Does NOT apply any changes. */
ASX_API asx_status asx_config_validate_reload(const asx_config_state *state,
                                              const asx_runtime_config *proposed,
                                              const char **rejection_field);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_CONFIG_RELOAD_H */
