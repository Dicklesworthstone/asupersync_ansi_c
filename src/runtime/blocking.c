/*
 * blocking.c — blocking pool for offloading blocking work
 *
 * Walking skeleton: executes blocking work inline (synchronously).
 * The API surface is ready for multi-threaded dispatch.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/blocking.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_blocking_fn fn;
    void *user_data;
    asx_waker completion_waker;
    int has_waker;
    uint16_t generation;
    asx_blocking_state state;
    uint64_t result;
} asx_blocking_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_blocking_slot g_slots[ASX_MAX_BLOCKING_TASKS];
static uint32_t g_slot_count = 0;
static uint32_t g_active_count = 0;
static int g_initialized = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_blocking_pool_init(void) {
    asx_status st = asx_surface_gate(ASX_SURFACE_BLOCKING);
    if (st != ASX_OK) return st;

    /* Re-initialization must start from a fresh pool state so stale handles
     * and completed results cannot leak across runtime lifecycles. */
    asx_blocking_pool_reset();
    g_initialized = 1;
    return ASX_OK;
}

void asx_blocking_pool_shutdown(void) {
    asx_blocking_pool_reset();
    g_initialized = 0;
}

int asx_blocking_pool_is_initialized(void) { return g_initialized; }

void asx_blocking_pool_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_BLOCKING_TASKS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].state = ASX_BLOCKING_COMPLETED;
        g_slots[i].fn = NULL;
        g_slots[i].user_data = NULL;
        g_slots[i].has_waker = 0;
        g_slots[i].result = 0;
    }
    g_slot_count = 0;
    g_active_count = 0;
}

/* ------------------------------------------------------------------ */
/* Spawn blocking                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_spawn_blocking(asx_blocking_fn fn, void *user_data,
                              const asx_waker *completion_waker, asx_blocking_handle *out_handle) {
    uint32_t idx;
    asx_blocking_slot *s;
    asx_status st;

    if (fn == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_BLOCKING);
    if (st != ASX_OK) return st;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (completion_waker != NULL && asx_waker_task(completion_waker) == ASX_INVALID_ID) {
        return ASX_E_INVALID_ARGUMENT;
    }

    /* Find free slot */
    idx = ASX_MAX_BLOCKING_TASKS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            if (g_slots[i].state == ASX_BLOCKING_COMPLETED) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_BLOCKING_TASKS) {
        if (g_slot_count >= ASX_MAX_BLOCKING_TASKS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->fn = fn;
    s->user_data = user_data;
    s->state = ASX_BLOCKING_PENDING;

    if (completion_waker != NULL) {
        s->completion_waker = *completion_waker;
        s->has_waker = 1;
    } else {
        s->has_waker = 0;
    }

    out_handle->slot = idx;
    out_handle->generation = s->generation;

    g_active_count++;

    /* Walking skeleton: execute inline immediately */
    s->state = ASX_BLOCKING_RUNNING;
    s->result = s->fn(s->user_data);
    s->state = ASX_BLOCKING_COMPLETED;

    if (g_active_count > 0) g_active_count--;

    /* Signal completion waker */
    if (s->has_waker) {
        asx_status wake_st;
        wake_st = asx_waker_wake(&s->completion_waker);
        (void)wake_st;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

asx_blocking_state asx_blocking_get_state(const asx_blocking_handle *handle) {
    if (handle == NULL) return ASX_BLOCKING_COMPLETED;
    if (handle->slot >= g_slot_count) return ASX_BLOCKING_COMPLETED;
    if (g_slots[handle->slot].generation != handle->generation) return ASX_BLOCKING_COMPLETED;
    return g_slots[handle->slot].state;
}

asx_status asx_blocking_get_result(const asx_blocking_handle *handle, uint64_t *out_result) {
    const asx_blocking_slot *s;
    asx_status st;

    st = asx_surface_gate(ASX_SURFACE_BLOCKING);
    if (st != ASX_OK) return st;
    if (handle == NULL || out_result == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!g_initialized) return ASX_E_INVALID_STATE;
    if (handle->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[handle->slot];
    if (s->generation != handle->generation) return ASX_E_STALE_HANDLE;
    if (s->state != ASX_BLOCKING_COMPLETED) return ASX_E_PENDING;

    *out_result = s->result;
    return ASX_OK;
}

uint32_t asx_blocking_active_count(void) { return g_active_count; }
