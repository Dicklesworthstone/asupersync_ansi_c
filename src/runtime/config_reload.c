/*
 * config_reload.c — Hot-reload config boundaries (bd-3vt.9)
 *
 * Implements config field classification and atomic reload with
 * validation and rollback.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- config reload spike, no checkpoint coverage needed */

#include <asx/asx.h>
#include <asx/runtime/config_reload.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Field descriptor table                                              */
/* ------------------------------------------------------------------ */

/*
 * Classification rationale:
 *
 * FROZEN_INIT: Fields that affect ABI or semantic contracts.
 *   - size: must match for forward compatibility
 *
 * RELOADABLE: Fields that affect operational behavior only.
 *   - wait_policy: idle strategy, no semantic impact
 *   - leak_response: diagnostic escalation only
 *   - leak_escalation: diagnostic threshold + escalation
 *   - finalizer_poll_budget: advisory finalization cap
 *   - finalizer_time_budget_ns: advisory finalization timeout
 *   - finalizer_escalation: diagnostic escalation on budget exhaust
 *
 * RESTART_REQUIRED: Fields that affect in-flight task state.
 *   - max_cancel_chain_depth: existing chains may exceed new limit
 *   - max_cancel_chain_memory: existing allocations may exceed new cap
 */

#define FIELD_DESC(name_str, cls, field)                                                           \
    {name_str, cls, (uint32_t)offsetof(asx_runtime_config, field),                                 \
     (uint32_t)sizeof(((asx_runtime_config *)0)->field)}

static const asx_config_field_desc g_field_table[] = {
    FIELD_DESC("size", ASX_CONFIG_FROZEN_INIT, size),
    FIELD_DESC("wait_policy", ASX_CONFIG_RELOADABLE, wait_policy),
    FIELD_DESC("leak_response", ASX_CONFIG_RELOADABLE, leak_response),
    FIELD_DESC("leak_escalation", ASX_CONFIG_RELOADABLE, leak_escalation),
    FIELD_DESC("finalizer_poll_budget", ASX_CONFIG_RELOADABLE, finalizer_poll_budget),
    FIELD_DESC("finalizer_time_budget_ns", ASX_CONFIG_RELOADABLE, finalizer_time_budget_ns),
    FIELD_DESC("finalizer_escalation", ASX_CONFIG_RELOADABLE, finalizer_escalation),
    FIELD_DESC("max_cancel_chain_depth", ASX_CONFIG_RESTART_REQUIRED, max_cancel_chain_depth),
    FIELD_DESC("max_cancel_chain_memory", ASX_CONFIG_RESTART_REQUIRED, max_cancel_chain_memory),
};

#define FIELD_COUNT (sizeof(g_field_table) / sizeof(g_field_table[0]))

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int field_bytes_differ(const asx_runtime_config *a, const asx_runtime_config *b,
                              uint32_t offset, uint32_t size) {
    const unsigned char *pa = (const unsigned char *)a + offset;
    const unsigned char *pb = (const unsigned char *)b + offset;
    return memcmp(pa, pb, size) != 0;
}

/* ------------------------------------------------------------------ */
/* API implementation                                                  */
/* ------------------------------------------------------------------ */

void asx_config_state_init(asx_config_state *state) {
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
}

asx_status asx_config_load(asx_config_state *state, const asx_runtime_config *cfg) {
    if (state == NULL || cfg == NULL) { return ASX_E_INVALID_ARGUMENT; }

    memcpy(&state->active, cfg, sizeof(asx_runtime_config));
    state->loaded = 1;
    return ASX_OK;
}

asx_status asx_config_validate_reload(const asx_config_state *state,
                                      const asx_runtime_config *proposed,
                                      const char **rejection_field) {
    uint32_t i;

    if (state == NULL || proposed == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!state->loaded) { return ASX_E_INVALID_STATE; }

    if (rejection_field != NULL) { *rejection_field = NULL; }

    for (i = 0; i < FIELD_COUNT; i++) {
        const asx_config_field_desc *f = &g_field_table[i];

        if (!field_bytes_differ(&state->active, proposed, f->offset, f->field_size)) {
            continue; /* Field unchanged — always OK */
        }

        switch (f->reload_class) {
        case ASX_CONFIG_FROZEN_COMPILE:
        case ASX_CONFIG_FROZEN_INIT:
            if (rejection_field != NULL) { *rejection_field = f->name; }
            return ASX_E_CONFIG_FROZEN;

        case ASX_CONFIG_RESTART_REQUIRED:
            if (rejection_field != NULL) { *rejection_field = f->name; }
            return ASX_E_CONFIG_RESTART_REQ;

        case ASX_CONFIG_RELOADABLE: break; /* OK to change */
        }
    }

    return ASX_OK;
}

asx_status asx_config_reload(asx_config_state *state, const asx_runtime_config *new_cfg) {
    asx_status st;
    const char *rejection = NULL;

    if (state == NULL || new_cfg == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!state->loaded) { return ASX_E_INVALID_STATE; }

    /* Validate before applying (rollback guarantee) */
    st = asx_config_validate_reload(state, new_cfg, &rejection);
    if (st != ASX_OK) { return st; }

    /* Apply atomically (single-threaded — just copy) */
    memcpy(&state->active, new_cfg, sizeof(asx_runtime_config));
    return ASX_OK;
}

const asx_runtime_config *asx_config_active(const asx_config_state *state) {
    if (state == NULL || !state->loaded) { return NULL; }
    return &state->active;
}

asx_config_reload_class asx_config_field_class(const char *name) {
    uint32_t i;

    if (name == NULL) { return ASX_CONFIG_FROZEN_COMPILE; }

    for (i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(g_field_table[i].name, name) == 0) { return g_field_table[i].reload_class; }
    }

    return ASX_CONFIG_FROZEN_COMPILE; /* Unknown fields are frozen */
}

const asx_config_field_desc *asx_config_field_table(uint32_t *count) {
    if (count != NULL) { *count = (uint32_t)FIELD_COUNT; }
    return g_field_table;
}
