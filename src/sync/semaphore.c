/*
 * semaphore.c — counting semaphore with permit obligations
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/semaphore.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int      active;
    int      acquired;      /* permit was granted to this waiter */
} sem_waiter_slot;

typedef struct {
    uint16_t        generation;
    int             alive;
    uint32_t        permits;        /* available permits */
    uint32_t        max_permits;    /* initial capacity (for queries) */
    sem_waiter_slot waiters[ASX_SEMAPHORE_MAX_WAITERS];
    uint32_t        waiter_count;
} sem_slot;

static sem_slot  g_slots[ASX_SEMAPHORE_MAX];
static uint32_t  g_slot_count;

static uint16_t next_gen(uint16_t g)
{
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_create(
    uint32_t initial_permits,
    asx_semaphore_handle *out)
{
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_SEMAPHORE_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].permits = initial_permits;
            g_slots[i].max_permits = initial_permits;
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

asx_status asx_semaphore_close(asx_semaphore_handle handle)
{
    sem_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation)
        return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        s->waiters[i].active = 0;
    }

    s->alive = 0;
    s->waiter_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Acquire (non-blocking)                                              */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_try_acquire(
    asx_semaphore_handle handle,
    asx_semaphore_permit *out)
{
    sem_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation)
        return ASX_E_STALE_HANDLE;

    if (s->permits > 0) {
        s->permits--;
        out->sem_slot = handle.slot;
        out->generation = handle.generation;
        return ASX_OK;
    }
    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Acquire (async)                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_acquire_begin(
    asx_semaphore_handle handle,
    asx_semaphore_waiter *out)
{
    sem_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation)
        return ASX_E_STALE_HANDLE;

    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) {
            s->waiters[i].active = 1;
            s->waiters[i].acquired = 0;
            s->waiter_count++;
            out->sem_slot = handle.slot;
            out->waiter_slot = i;
            out->generation = handle.generation;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_semaphore_poll_acquire(
    asx_semaphore_waiter *waiter,
    asx_semaphore_permit *out,
    asx_cx *cx)
{
    sem_slot *s;
    sem_waiter_slot *w;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->sem_slot];
    if (!s->alive || s->generation != waiter->generation)
        return ASX_E_DISCONNECTED;

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

    /* Already acquired by a prior release? */
    if (w->acquired) {
        w->active = 0;
        w->acquired = 0;
        s->waiter_count--;
        out->sem_slot = waiter->sem_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Try to grab a permit */
    if (s->permits > 0) {
        s->permits--;
        w->active = 0;
        s->waiter_count--;
        out->sem_slot = waiter->sem_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    return ASX_E_PENDING;
}

asx_status asx_semaphore_acquire_cancel(asx_semaphore_waiter *waiter)
{
    sem_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->sem_slot];
    if (!s->alive || s->generation != waiter->generation)
        return ASX_OK;

    if (s->waiters[waiter->waiter_slot].active) {
        /* If a permit was already granted, return it */
        if (s->waiters[waiter->waiter_slot].acquired) {
            s->permits++;
        }
        s->waiters[waiter->waiter_slot].active = 0;
        s->waiter_count--;
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Release                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_release(asx_semaphore_permit permit)
{
    sem_slot *s;
    uint32_t i;
    if (permit.sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[permit.sem_slot];
    if (!s->alive || s->generation != permit.generation)
        return ASX_E_STALE_HANDLE;

    /* FIFO: grant permit to first waiting waiter */
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        if (s->waiters[i].active && !s->waiters[i].acquired) {
            s->waiters[i].acquired = 1;
            return ASX_OK;
        }
    }

    /* No waiters, return permit to pool */
    s->permits++;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_semaphore_available(asx_semaphore_handle handle)
{
    if (handle.slot >= ASX_SEMAPHORE_MAX) return 0;
    if (!g_slots[handle.slot].alive ||
        g_slots[handle.slot].generation != handle.generation) return 0;
    return g_slots[handle.slot].permits;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_semaphore_reset(void)
{
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].permits = 0;
        g_slots[i].waiter_count = 0;
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
