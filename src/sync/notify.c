/*
 * notify.c — event signaling primitive
 *
 * Waiters are slot records stamped with an arrival sequence number.
 * notify_one marks the oldest waiter that is not yet notified (FIFO by
 * arrival); notify_all marks every registered waiter.
 *
 * Wake-driven waiting: poll_wait returning ASX_E_PENDING inside a
 * scheduler poll records the calling task on the waiter and parks it; a
 * notification wakes the recorded task.
 *
 * Cancel safety: a waiter that received a notify_one notification and then
 * gives up (wait_cancel, a failing Cx checkpoint, or its task completing
 * without cancelling, reclaimed lazily) passes the notification on to the
 * next waiter in line. notify_one skips cancel-pending waiters so they
 * never absorb a notification another task needs. Close wakes every
 * parked waiter so it observes ASX_E_DISCONNECTED.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/sync/notify.h>
#include <string.h>

#define NOTIFY_NO_WAITER ASX_NOTIFY_MAX_WAITERS

/* How a waiter was notified (only notify_one notifications are passed on). */
#define NOTIFY_NONE 0
#define NOTIFY_ONE 1
#define NOTIFY_ALL 2

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int notified;     /* NOTIFY_NONE / NOTIFY_ONE / NOTIFY_ALL */
    int active;       /* waiter is registered */
    uint32_t seq;     /* arrival order */
    asx_task_id task; /* task parked on this waiter, ASX_INVALID_ID if none */
} notify_waiter_slot;

typedef struct {
    uint16_t generation;
    int alive;
    notify_waiter_slot waiters[ASX_NOTIFY_MAX_WAITERS];
    uint32_t waiter_count;
    uint32_t next_seq; /* arrival stamp for the next waiter */
} notify_slot;

static notify_slot g_slots[ASX_NOTIFY_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Waiter line                                                         */
/* ------------------------------------------------------------------ */

/* Wrap-safe "a arrived before b". */
static int seq_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

/* Oldest active, un-notified waiter whose task is not cancel-pending. */
static uint32_t notify_first_in_line(const notify_slot *s) {
    uint32_t i;
    uint32_t best = NOTIFY_NO_WAITER;
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        const notify_waiter_slot *w = &s->waiters[i];
        if (!w->active || w->notified != NOTIFY_NONE) continue;
        if (best != NOTIFY_NO_WAITER && !seq_before(w->seq, s->waiters[best].seq)) continue;
        if (asx_handle_is_valid(w->task) &&
            asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DOOMED) {
            continue;
        }
        best = i;
    }
    return best;
}

/* Deliver one notification to the next waiter in line. Returns 1 if a
 * waiter took it. */
static int notify_deliver_one(notify_slot *s) {
    uint32_t i = notify_first_in_line(s);
    if (i == NOTIFY_NO_WAITER) return 0;
    s->waiters[i].notified = NOTIFY_ONE;
    asx_wait_wake_task(s->waiters[i].task);
    return 1;
}

/* Deactivate a waiter that leaves without consuming its notification. A
 * notify_one notification it held is passed on. */
static void notify_waiter_abandon(notify_slot *s, notify_waiter_slot *w) {
    int pass_on = (w->notified == NOTIFY_ONE);
    w->active = 0;
    w->notified = NOTIFY_NONE;
    w->task = ASX_INVALID_ID;
    s->waiter_count--;
    if (pass_on) (void)notify_deliver_one(s);
}

/* Reclaim waiters whose parked task completed without cancelling. */
static void notify_reap(notify_slot *s) {
    uint32_t i;
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        notify_waiter_slot *w = &s->waiters[i];
        if (!w->active || !asx_handle_is_valid(w->task)) continue;
        if (asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) notify_waiter_abandon(s, w);
    }
}

static uint32_t notify_free_waiter(const notify_slot *s) {
    uint32_t i;
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) return i;
    }
    return NOTIFY_NO_WAITER;
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
            g_slots[i].next_seq = 0;
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
    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        if (s->waiters[i].active) asx_wait_wake_task(s->waiters[i].task);
        s->waiters[i].active = 0;
        s->waiters[i].task = ASX_INVALID_ID;
    }

    s->alive = 0;
    s->waiter_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Signal                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_notify_one(asx_notify_handle handle) {
    notify_slot *s;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* FIFO: notify the oldest waiter (reclaiming dead ones first) */
    notify_reap(s);
    (void)notify_deliver_one(s);
    return ASX_OK; /* no waiters, that's fine */
}

asx_status asx_notify_all(asx_notify_handle handle) {
    notify_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    for (i = 0; i < ASX_NOTIFY_MAX_WAITERS; i++) {
        if (s->waiters[i].active) {
            if (s->waiters[i].notified == NOTIFY_NONE) s->waiters[i].notified = NOTIFY_ALL;
            asx_wait_wake_task(s->waiters[i].task);
        }
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

    /* Find free waiter slot (reclaiming records of dead tasks if full) */
    i = notify_free_waiter(s);
    if (i == NOTIFY_NO_WAITER) {
        notify_reap(s);
        i = notify_free_waiter(s);
        if (i == NOTIFY_NO_WAITER) return ASX_E_RESOURCE_EXHAUSTED;
    }

    s->waiters[i].active = 1;
    s->waiters[i].notified = NOTIFY_NONE;
    s->waiters[i].seq = s->next_seq++;
    s->waiters[i].task = ASX_INVALID_ID;
    s->waiter_count++;
    out->notify_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    return ASX_OK;
}

asx_status asx_notify_poll_wait(asx_notify_waiter *waiter, asx_cx *cx) {
    notify_slot *s;
    notify_waiter_slot *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->notify_slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (waiter->waiter_slot >= ASX_NOTIFY_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->notify_slot];

    /* Check if notify was closed */
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;

    w = &s->waiters[waiter->waiter_slot];
    if (!w->active) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            /* Give up: a notify_one notification passes to the next waiter. */
            notify_waiter_abandon(s, w);
            return cst;
        }
    }

    if (w->notified != NOTIFY_NONE) {
        w->active = 0;
        w->notified = NOTIFY_NONE;
        w->task = ASX_INVALID_ID;
        s->waiter_count--;
        return ASX_OK;
    }

    /* Inside a scheduler poll, park until notified (or closed). */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

asx_status asx_notify_wait_cancel(asx_notify_waiter *waiter) {
    notify_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->notify_slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (waiter->waiter_slot >= ASX_NOTIFY_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->notify_slot];
    if (!s->alive || s->generation != waiter->generation)
        return ASX_OK; /* already closed, nothing to cancel */

    if (s->waiters[waiter->waiter_slot].active) {
        notify_waiter_abandon(s, &s->waiters[waiter->waiter_slot]);
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
        g_slots[i].next_seq = 0;
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
