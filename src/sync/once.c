/*
 * once.c — compute-once cell
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/once.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    int initialized;
    uint64_t value;
} once_slot;

static once_slot g_slots[ASX_ONCE_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_once_create(asx_once_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_ONCE_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].initialized = 0;
            g_slots[i].value = 0;
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_once_close(asx_once_handle handle) {
    once_slot *s;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    s->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Access                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_once_get_or_init(asx_once_handle handle, asx_once_init_fn init_fn, void *user_data,
                                uint64_t *out_value) {
    once_slot *s;
    asx_status st;

    if (init_fn == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    if (s->initialized) {
        *out_value = s->value;
        return ASX_OK;
    }

    /* Initialize */
    st = init_fn(user_data, &s->value);
    if (st != ASX_OK) return st;

    s->initialized = 1;
    *out_value = s->value;
    return ASX_OK;
}

asx_status asx_once_get(asx_once_handle handle, uint64_t *out_value) {
    once_slot *s;
    if (out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    if (!s->initialized) return ASX_E_INVALID_STATE;

    *out_value = s->value;
    return ASX_OK;
}

int asx_once_is_initialized(asx_once_handle handle) {
    if (handle.slot >= ASX_ONCE_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return g_slots[handle.slot].initialized;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_once_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].initialized = 0;
        g_slots[i].value = 0;
    }
    g_slot_count = 0;
}
