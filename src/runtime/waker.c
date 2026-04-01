/*
 * waker.c — wake registration for task readiness signaling
 *
 * Walking skeleton: single-threaded, flag-based signaling.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/waker.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_task_id task;
    uint16_t generation;
    int alive;
    int signaled;
} asx_waker_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_waker_slot g_slots[ASX_MAX_WAKERS];
static uint32_t g_slot_count = 0;
static uint32_t g_active_count = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

void asx_waker_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_WAKERS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].signaled = 0;
        g_slots[i].task = ASX_INVALID_ID;
    }
    g_slot_count = 0;
    g_active_count = 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_waker_register(asx_task_id task, asx_waker *out_waker) {
    uint32_t idx;

    if (task == ASX_INVALID_ID || out_waker == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    idx = ASX_MAX_WAKERS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
            if (!g_slots[i].alive) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_WAKERS) {
        if (g_slot_count >= ASX_MAX_WAKERS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    g_slots[idx].task = task;
    g_slots[idx].generation = next_gen(g_slots[idx].generation);
    g_slots[idx].alive = 1;
    g_slots[idx].signaled = 0;
    g_active_count++;

    out_waker->slot = idx;
    out_waker->generation = g_slots[idx].generation;

    return ASX_OK;
}

void asx_waker_deregister(asx_waker *waker) {
    if (waker == NULL) return;
    if (waker->slot >= g_slot_count) return;
    if (g_slots[waker->slot].generation != waker->generation) return;
    if (!g_slots[waker->slot].alive) return;

    g_slots[waker->slot].alive = 0;
    g_slots[waker->slot].signaled = 0;
    if (g_active_count > 0) g_active_count--;
}

/* ------------------------------------------------------------------ */
/* Signaling                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_waker_wake(const asx_waker *waker) {
    if (waker == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waker->slot >= g_slot_count) return ASX_E_NOT_FOUND;
    if (g_slots[waker->slot].generation != waker->generation) return ASX_E_NOT_FOUND;
    if (!g_slots[waker->slot].alive) return ASX_E_NOT_FOUND;

    g_slots[waker->slot].signaled = 1;
    return ASX_OK;
}

asx_status asx_waker_clone(const asx_waker *src, asx_waker *out_clone) {
    if (src == NULL || out_clone == NULL) return ASX_E_INVALID_ARGUMENT;
    if (src->slot >= g_slot_count) return ASX_E_NOT_FOUND;
    if (g_slots[src->slot].generation != src->generation) return ASX_E_NOT_FOUND;
    if (!g_slots[src->slot].alive) return ASX_E_NOT_FOUND;

    /* Clone is the same handle (references same slot) */
    out_clone->slot = src->slot;
    out_clone->generation = src->generation;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

int asx_waker_is_signaled(const asx_waker *waker) {
    if (waker == NULL) return 0;
    if (waker->slot >= g_slot_count) return 0;
    if (g_slots[waker->slot].generation != waker->generation) return 0;
    if (!g_slots[waker->slot].alive) return 0;
    return g_slots[waker->slot].signaled;
}

void asx_waker_clear(asx_waker *waker) {
    if (waker == NULL) return;
    if (waker->slot >= g_slot_count) return;
    if (g_slots[waker->slot].generation != waker->generation) return;
    g_slots[waker->slot].signaled = 0;
}

asx_task_id asx_waker_task(const asx_waker *waker) {
    if (waker == NULL) return ASX_INVALID_ID;
    if (waker->slot >= g_slot_count) return ASX_INVALID_ID;
    if (g_slots[waker->slot].generation != waker->generation) return ASX_INVALID_ID;
    if (!g_slots[waker->slot].alive) return ASX_INVALID_ID;
    return g_slots[waker->slot].task;
}

uint32_t asx_waker_active_count(void) { return g_active_count; }

/* ------------------------------------------------------------------ */
/* Drain signaled                                                      */
/* ------------------------------------------------------------------ */

uint32_t asx_waker_drain_signaled(asx_task_id *out_tasks, uint32_t max_tasks) {
    uint32_t i, count = 0;

    if (out_tasks == NULL || max_tasks == 0) return 0;

    for (i = 0; i < g_slot_count && count < max_tasks; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (g_slots[i].alive && g_slots[i].signaled) {
            out_tasks[count++] = g_slots[i].task;
            g_slots[i].signaled = 0;
        }
    }
    return count;
}
