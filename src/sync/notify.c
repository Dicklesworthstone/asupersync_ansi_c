/*
 * notify.c — event signaling primitive
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/notify.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int notified; /* has been notified */
    int active;   /* waiter is registered */
} notify_waiter_slot;

typedef struct {
    uint16_t generation;
    int alive;
    notify_waiter_slot waiters[ASX_NOTIFY_MAX_WAITERS];
    uint32_t waiter_count;
} notify_slot;

static notify_slot g_slots[ASX_NOTIFY_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_notify_create(asx_notify_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    for (i = 0; i < ASX_NOTIFY_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].waiter_count = 0;
            memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_notify_close(asx_notify_handle handle) {
    notify_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) { s->waiters[i].active = 0; }

    s->alive = 0;
    s->waiter_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Signal                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_notify_one(asx_notify_handle handle) {
    notify_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* FIFO: wake first active waiter */
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        if (s->waiters[i].active && !s->waiters[i].notified) {
            s->waiters[i].notified = 1;
            return ASX_OK;
        }
    }
    return ASX_OK; /* no waiters, that's fine */
}

asx_status asx_notify_all(asx_notify_handle handle) {
    notify_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        if (s->waiters[i].active) { s->waiters[i].notified = 1; }
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Wait                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_notify_wait_begin(asx_notify_handle handle, asx_notify_waiter *out) {
    notify_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* Find free waiter slot */
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) {
            s->waiters[i].active = 1;
            s->waiters[i].notified = 0;
            s->waiter_count++;
            out->notify_slot = handle.slot;
            out->waiter_slot = i;
            out->generation = handle.generation;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_notify_poll_wait(asx_notify_waiter *waiter, asx_cx *cx) {
    notify_slot *s;
    notify_waiter_slot *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->notify_slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->notify_slot];

    /* Check if notify was closed */
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;

    w = &s->waiters[waiter->waiter_slot];
    if (!w->active) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            w->active = 0;
            s->waiter_count--;
            return cst;
        }
    }

    if (w->notified) {
        w->active = 0;
        w->notified = 0;
        s->waiter_count--;
        return ASX_OK;
    }

    return ASX_E_PENDING;
}

asx_status asx_notify_wait_cancel(asx_notify_waiter *waiter) {
    notify_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->notify_slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->notify_slot];
    if (!s->alive || s->generation != waiter->generation)
        return ASX_OK; /* already closed, nothing to cancel */

    if (s->waiters[waiter->waiter_slot].active) {
        s->waiters[waiter->waiter_slot].active = 0;
        s->waiter_count--;
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_notify_waiter_count(asx_notify_handle handle) {
    if (handle.slot >= ASX_NOTIFY_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return g_slots[handle.slot].waiter_count;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_notify_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].waiter_count = 0;
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
