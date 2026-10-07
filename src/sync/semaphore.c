/*
 * semaphore.c — counting semaphore with permit obligations
 *
 * Waiters are slot records stamped with an arrival sequence number, and
 * permits are handed over strictly in arrival (FIFO) order:
 *   - release() grants the permit directly to the oldest un-granted waiter
 *     (waking its task if it is parked) or returns it to the pool;
 *   - an un-granted waiter takes a pooled permit at poll time only when it
 *     is next in line, so later arrivals never overtake earlier ones;
 *   - a pooled permit owed to the waiters next in line is granted to them
 *     (and their tasks woken) as soon as they are parked: after a poll
 *     acquires, after a cancel, and after a dead waiter is reclaimed.
 *
 * Wake-driven waiting: poll_acquire returning ASX_E_PENDING inside a
 * scheduler poll records the calling task on the waiter and parks it.
 *
 * Cancel safety: a waiter that gives up (acquire_cancel, or a failing Cx
 * checkpoint) passes a permit it was granted on to the next waiter in
 * line. A parked waiter whose task completed without cancelling is
 * reclaimed lazily (its grant passed on), and a cancel-pending waiter is
 * skipped when granting so it never absorbs a permit another task needs.
 * Close wakes every parked waiter so it observes ASX_E_DISCONNECTED.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/sync/semaphore.h>
#include <string.h>

#define SEM_NO_WAITER ASX_SEMAPHORE_MAX_WAITERS

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;
    int acquired;     /* permit was granted to this waiter */
    uint32_t seq;     /* arrival order */
    asx_task_id task; /* task parked on this waiter, ASX_INVALID_ID if none */
} sem_waiter_slot;

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t permits;     /* available permits */
    uint32_t max_permits; /* initial capacity (for queries) */
    sem_waiter_slot waiters[ASX_SEMAPHORE_MAX_WAITERS];
    uint32_t waiter_count;
    uint32_t next_seq; /* arrival stamp for the next waiter */
} sem_slot;

static sem_slot g_slots[ASX_SEMAPHORE_MAX];
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

/* A waiter whose parked task has a cancel request pending: it is skipped
 * when granting (it would not consume the permit). */
static int sem_waiter_doomed(const sem_waiter_slot *w) {
    return asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DOOMED;
}

/* Deactivate a waiter record, returning a permit it was granted but never
 * consumed to the pool. */
static void sem_waiter_retire(sem_slot *s, sem_waiter_slot *w) {
    if (w->acquired) s->permits++;
    w->active = 0;
    w->acquired = 0;
    w->task = ASX_INVALID_ID;
    s->waiter_count--;
}

/* Reclaim waiters whose parked task completed (or whose slot was reused)
 * without cancelling the acquire. */
static void sem_reap(sem_slot *s) {
    uint32_t i;
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        sem_waiter_slot *w = &s->waiters[i];
        if (!w->active || !asx_handle_is_valid(w->task)) continue;
        if (asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) sem_waiter_retire(s, w);
    }
}

/* The oldest active, un-granted, not cancel-pending waiter other than
 * `except` (SEM_NO_WAITER for none), or SEM_NO_WAITER. */
static uint32_t sem_first_in_line(const sem_slot *s, uint32_t except) {
    uint32_t i;
    uint32_t best = SEM_NO_WAITER;
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        const sem_waiter_slot *w = &s->waiters[i];
        if (i == except || !w->active || w->acquired) continue;
        if (best != SEM_NO_WAITER && !seq_before(w->seq, s->waiters[best].seq)) continue;
        if (sem_waiter_doomed(w)) continue;
        best = i;
    }
    return best;
}

/* 1 if no waiter that arrived before `idx` still waits for a permit. */
static int sem_next_in_line(const sem_slot *s, uint32_t idx) {
    uint32_t first = sem_first_in_line(s, idx);
    return first == SEM_NO_WAITER || !seq_before(s->waiters[first].seq, s->waiters[idx].seq);
}

static void sem_grant(sem_waiter_slot *w) {
    w->acquired = 1;
    asx_wait_wake_task(w->task);
}

/* Grant pooled permits to the waiters next in line while they are parked.
 * Stops at the first one that is not parked: it takes its permit when it
 * polls, so granting past it would break arrival order. */
static void sem_dispatch_parked(sem_slot *s) {
    while (s->permits > 0) {
        uint32_t i = sem_first_in_line(s, SEM_NO_WAITER);
        if (i == SEM_NO_WAITER || !asx_handle_is_valid(s->waiters[i].task)) break;
        s->permits--;
        sem_grant(&s->waiters[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_create(uint32_t initial_permits, asx_semaphore_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_SEMAPHORE_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].permits = initial_permits;
            g_slots[i].max_permits = initial_permits;
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

asx_status asx_semaphore_close(asx_semaphore_handle handle) {
    sem_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        if (s->waiters[i].active) asx_wait_wake_task(s->waiters[i].task);
        s->waiters[i].active = 0;
        s->waiters[i].task = ASX_INVALID_ID;
    }

    s->alive = 0;
    s->waiter_count = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Acquire (non-blocking)                                              */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_try_acquire(asx_semaphore_handle handle, asx_semaphore_permit *out) {
    sem_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

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

static uint32_t sem_free_waiter(const sem_slot *s) {
    uint32_t i;
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) return i;
    }
    return SEM_NO_WAITER;
}

asx_status asx_semaphore_acquire_begin(asx_semaphore_handle handle, asx_semaphore_waiter *out) {
    sem_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    i = sem_free_waiter(s);
    if (i == SEM_NO_WAITER) {
        /* Reclaim records abandoned by completed tasks before giving up. */
        sem_reap(s);
        sem_dispatch_parked(s);
        i = sem_free_waiter(s);
        if (i == SEM_NO_WAITER) return ASX_E_RESOURCE_EXHAUSTED;
    }

    s->waiters[i].active = 1;
    s->waiters[i].acquired = 0;
    s->waiters[i].seq = s->next_seq++;
    s->waiters[i].task = ASX_INVALID_ID;
    s->waiter_count++;
    out->sem_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    return ASX_OK;
}

/* Hand a granted permit to the polling caller and retire its waiter. */
static asx_status sem_consume_grant(sem_slot *s, sem_waiter_slot *w,
                                    const asx_semaphore_waiter *waiter, asx_semaphore_permit *out) {
    w->acquired = 0; /* consumed by this caller, not returned to the pool */
    sem_waiter_retire(s, w);
    out->sem_slot = waiter->sem_slot;
    out->generation = waiter->generation;
    return ASX_OK;
}

asx_status asx_semaphore_poll_acquire(asx_semaphore_waiter *waiter, asx_semaphore_permit *out,
                                      asx_cx *cx) {
    sem_slot *s;
    sem_waiter_slot *w;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->sem_slot];
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_SEMAPHORE_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;

    w = &s->waiters[waiter->waiter_slot];
    if (!w->active) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            /* Give up: a granted permit passes to the next waiter. */
            sem_waiter_retire(s, w);
            sem_dispatch_parked(s);
            return cst;
        }
    }

    /* Already acquired by a prior release? */
    if (w->acquired) return sem_consume_grant(s, w, waiter, out);

    /* Settle the line: reclaim waiters of dead tasks (their grants return
     * to the pool) and hand pooled permits to parked waiters in order. */
    sem_reap(s);
    sem_dispatch_parked(s);
    if (w->acquired) return sem_consume_grant(s, w, waiter, out);

    /* Take a pooled permit only when no earlier arrival is still waiting. */
    if (s->permits > 0 && sem_next_in_line(s, waiter->waiter_slot)) {
        s->permits--;
        sem_waiter_retire(s, w);
        /* Permits left over belong to the parked waiters behind us. */
        sem_dispatch_parked(s);
        out->sem_slot = waiter->sem_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Wait for a release; inside a scheduler poll, park until granted. */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

asx_status asx_semaphore_acquire_cancel(asx_semaphore_waiter *waiter) {
    sem_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->sem_slot];
    if (!s->alive || s->generation != waiter->generation) return ASX_OK;
    if (waiter->waiter_slot >= ASX_SEMAPHORE_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;

    if (s->waiters[waiter->waiter_slot].active) {
        /* A permit already granted goes back and on to the next waiter. */
        sem_waiter_retire(s, &s->waiters[waiter->waiter_slot]);
        sem_dispatch_parked(s);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Release                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_release(asx_semaphore_permit permit) {
    sem_slot *s;
    uint32_t i;
    if (permit.sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[permit.sem_slot];
    if (!s->alive || s->generation != permit.generation) return ASX_E_STALE_HANDLE;

    /* FIFO: grant the permit to the oldest waiter still in line */
    sem_reap(s);
    i = sem_first_in_line(s, SEM_NO_WAITER);
    if (i != SEM_NO_WAITER) {
        sem_grant(&s->waiters[i]);
        sem_dispatch_parked(s); /* permits a reap returned */
        return ASX_OK;
    }

    /* No waiters, return permit to pool */
    s->permits++;
    sem_dispatch_parked(s);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_semaphore_available(asx_semaphore_handle handle) {
    if (handle.slot >= ASX_SEMAPHORE_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return g_slots[handle.slot].permits;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_semaphore_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].permits = 0;
        g_slots[i].waiter_count = 0;
        g_slots[i].next_seq = 0;
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
