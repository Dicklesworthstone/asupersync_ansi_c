/*
 * barrier.c — N-way rendezvous with leader election
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/barrier.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int      active;
    int      released;      /* barrier has tripped */
    int      is_leader;
} barrier_waiter_slot;

typedef struct {
    uint16_t             generation;
    int                  alive;
    uint32_t             threshold;   /* N tasks required */
    uint32_t             arrived;     /* tasks currently waiting */
    int                  tripped;     /* barrier released */
    barrier_waiter_slot  waiters[ASX_BARRIER_MAX * 4]; /* up to 4x threshold */
    uint32_t             waiter_count;
} barrier_slot;

/* Use a generous waiter limit per barrier */
#define BARRIER_MAX_WAITERS (ASX_BARRIER_MAX * 4u)

static barrier_slot g_slots[ASX_BARRIER_MAX];
static uint32_t     g_slot_count;

static uint16_t next_gen(uint16_t g)
{
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_barrier_create(
    uint32_t count,
    asx_barrier_handle *out)
{
    uint32_t i;
    if (out == NULL || count == 0) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_BARRIER_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].threshold = count;
            g_slots[i].arrived = 0;
            g_slots[i].tripped = 0;
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

asx_status asx_barrier_close(asx_barrier_handle handle)
{
    barrier_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation)
        return ASX_E_STALE_HANDLE;

    for (i = 0; i < BARRIER_MAX_WAITERS; i++) {
        s->waiters[i].active = 0;
    }

    s->alive = 0;
    s->waiter_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Wait                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_barrier_wait_begin(
    asx_barrier_handle handle,
    asx_barrier_waiter *out)
{
    barrier_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation)
        return ASX_E_STALE_HANDLE;

    for (i = 0; i < BARRIER_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) {
            s->waiters[i].active = 1;
            s->waiters[i].released = 0;
            s->waiters[i].is_leader = 0;
            s->arrived++;
            s->waiter_count++;
            out->barrier_slot = handle.slot;
            out->waiter_slot = i;
            out->generation = handle.generation;
            out->is_leader = 0;

            /* Check if barrier trips */
            if (s->arrived >= s->threshold) {
                uint32_t j;
                s->tripped = 1;
                /* Last to arrive is leader */
                s->waiters[i].is_leader = 1;
                /* Release all waiters */
                for (j = 0; j < BARRIER_MAX_WAITERS; j++) {
                    if (s->waiters[j].active) {
                        s->waiters[j].released = 1;
                    }
                }
            }

            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_barrier_poll_wait(
    asx_barrier_waiter *waiter,
    asx_cx *cx)
{
    barrier_slot *s;
    barrier_waiter_slot *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->barrier_slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->barrier_slot];
    if (!s->alive || s->generation != waiter->generation)
        return ASX_E_DISCONNECTED;

    w = &s->waiters[waiter->waiter_slot];
    if (!w->active) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            w->active = 0;
            s->arrived--;
            s->waiter_count--;
            return cst;
        }
    }

    if (w->released) {
        waiter->is_leader = w->is_leader;
        w->active = 0;
        s->waiter_count--;
        return ASX_OK;
    }

    return ASX_E_PENDING;
}

asx_status asx_barrier_wait_cancel(asx_barrier_waiter *waiter)
{
    barrier_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->barrier_slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->barrier_slot];
    if (!s->alive || s->generation != waiter->generation)
        return ASX_OK;

    if (s->waiters[waiter->waiter_slot].active) {
        s->waiters[waiter->waiter_slot].active = 0;
        s->arrived--;
        s->waiter_count--;
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_barrier_waiting_count(asx_barrier_handle handle)
{
    if (handle.slot >= ASX_BARRIER_MAX) return 0;
    if (!g_slots[handle.slot].alive ||
        g_slots[handle.slot].generation != handle.generation) return 0;
    return g_slots[handle.slot].arrived;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_barrier_reset(void)
{
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].arrived = 0;
        g_slots[i].tripped = 0;
        g_slots[i].waiter_count = 0;
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
