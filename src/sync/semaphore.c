/*
 * semaphore.c — counting semaphore with permit obligations, and the lock
 * under asx_mutex
 *
 * Waiters are slot records stamped with an arrival sequence number and
 * served in arrival (FIFO) order. A semaphore and the mutex's one-permit
 * semaphore follow their Rust counterparts, which differ:
 *
 *   Semaphore (Rust sync/semaphore.rs): permits stay pooled. A waiter joins
 *   the line at its first poll that cannot take a permit. Only the front of
 *   the line may take permits, and a newcomer only while nobody is queued.
 *   A release adds the permit and wakes the front waiter, if it can now
 *   run, and nobody else (add_permits_deferred); a waiter that takes a
 *   permit leaves the line and wakes the new front, if it can run
 *   (poll_with_registration). One that leaves without a permit (cancel,
 *   dropped) wakes the next one only if it was the front
 *   (remove_waiter_and_take_next_waker). Cancel-pending waiters keep their
 *   place: they leave when polled. try_acquire fails while anyone is
 *   queued.
 *
 *   Mutex (Rust sync/mutex.rs): unlock hands the lock to the front waiter
 *   (the baton): release grants the permit directly to the oldest
 *   un-granted waiter, waking its task, or returns it to the pool; a
 *   pooled permit owed to the waiters next in line is granted to them as
 *   soon as they are parked; a waiter that gives up passes a grant on; a
 *   cancel-pending waiter is skipped when granting.
 *
 * Wake-driven waiting: poll_acquire returning ASX_E_PENDING inside a
 * scheduler poll records the calling task on the waiter and parks it. A
 * waiter whose task completed without cancelling is reclaimed lazily.
 * Close wakes every parked waiter so it observes ASX_E_DISCONNECTED.
 *
 * Obligations: a permit granted to an acquire polled with a task Cx
 * reserves a SemaphorePermit obligation held by that task, and release
 * commits it (Rust sync/semaphore.rs:119, :1274). A refused reservation
 * leaves the permit untracked, as Rust's Option-returning registration
 * does; mutex semaphores never track (Rust's Mutex has no obligation).
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/runtime/runtime.h>
#include <asx/sync/semaphore.h>
#include <string.h>

#define SEM_NO_WAITER ASX_SEMAPHORE_MAX_WAITERS

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;
    int acquired;     /* mutex: the lock was handed to this waiter */
    int queued;       /* semaphore: in line (a poll had to wait) */
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
    /* The lock under asx_mutex: Rust's Mutex hand-off, and no obligation
     * (Rust's Mutex registers none). Otherwise a Semaphore, whose permits
     * reserve SemaphorePermit obligations. */
    int is_mutex;
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
    w->queued = 0;
    w->task = ASX_INVALID_ID;
    s->waiter_count--;
}

/* Semaphore: the front of the line, the oldest queued waiter, or
 * SEM_NO_WAITER. */
static uint32_t sem_front(const sem_slot *s) {
    uint32_t i;
    uint32_t best = SEM_NO_WAITER;
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        const sem_waiter_slot *w = &s->waiters[i];
        if (!w->active || !w->queued) continue;
        if (best == SEM_NO_WAITER || seq_before(w->seq, s->waiters[best].seq)) best = i;
    }
    return best;
}

/* Semaphore: wake the front waiter if it can take a permit now
 * (front_waiter_waker_if_runnable). */
static void sem_wake_front(const sem_slot *s) {
    uint32_t i = sem_front(s);
    if (i != SEM_NO_WAITER && s->permits >= 1u) asx_wait_wake_task(s->waiters[i].task);
}

/* Semaphore: a waiter leaves the line without a permit; the next one is
 * woken only if this one was the front (remove_waiter_and_take_next_waker). */
static void sem_leave(sem_slot *s, sem_waiter_slot *w) {
    int was_front = w->queued && sem_front(s) == (uint32_t)(w - s->waiters);
    sem_waiter_retire(s, w);
    if (was_front) sem_wake_front(s);
}

/* Reclaim waiters whose parked task completed (or whose slot was reused)
 * without cancelling the acquire. */
static void sem_reap(sem_slot *s) {
    uint32_t i;
    for (i = 0; i < ASX_SEMAPHORE_MAX_WAITERS; i++) {
        sem_waiter_slot *w = &s->waiters[i];
        if (!w->active || !asx_handle_is_valid(w->task)) continue;
        if (asx_wait_task_liveness(w->task) != ASX_WAIT_TASK_DEAD) continue;
        if (s->is_mutex) {
            sem_waiter_retire(s, w);
        } else {
            sem_leave(s, w);
        }
    }
}

/* Mutex: the oldest active, un-granted, not cancel-pending waiter other
 * than `except` (SEM_NO_WAITER for none), or SEM_NO_WAITER. */
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

/* Mutex: 1 if no waiter that arrived before `idx` still waits. */
static int sem_next_in_line(const sem_slot *s, uint32_t idx) {
    uint32_t first = sem_first_in_line(s, idx);
    return first == SEM_NO_WAITER || !seq_before(s->waiters[first].seq, s->waiters[idx].seq);
}

static void sem_grant(sem_waiter_slot *w) {
    w->acquired = 1;
    asx_wait_wake_task(w->task);
}

/* Mutex: grant a pooled lock to the waiters next in line while they are
 * parked. Stops at the first one that is not parked: it takes the lock
 * when it polls, so granting past it would break arrival order. A
 * semaphore grants nothing: its waiters take permits when they poll. */
static void sem_dispatch_parked(sem_slot *s) {
    if (!s->is_mutex) return;
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

static asx_status sem_create(uint32_t initial_permits, int is_mutex, asx_semaphore_handle *out) {
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
            g_slots[i].is_mutex = is_mutex;
            memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_semaphore_create(uint32_t initial_permits, asx_semaphore_handle *out) {
    return sem_create(initial_permits, 0, out);
}

asx_status asx_semaphore_create_untracked(uint32_t initial_permits, asx_semaphore_handle *out) {
    return sem_create(initial_permits, 1, out);
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

    /* A semaphore with anyone queued is FIFO-blocked (Rust try_acquire). */
    if (!s->is_mutex) sem_reap(s);
    if (s->permits > 0 && (s->is_mutex || sem_front(s) == SEM_NO_WAITER)) {
        s->permits--;
        out->sem_slot = handle.slot;
        out->generation = handle.generation;
        out->obligation = ASX_INVALID_ID; /* no Cx to hold it */
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
    s->waiters[i].queued = 0;
    s->waiters[i].seq = s->next_seq++;
    s->waiters[i].task = ASX_INVALID_ID;
    s->waiter_count++;
    out->sem_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    return ASX_OK;
}

/* Fill the permit handed to an acquire polled with `cx`, reserving its
 * SemaphorePermit obligation for the Cx's task (a semaphore's, not the
 * mutex's). A refused reservation leaves the permit untracked. */
static asx_status sem_hand_out(const sem_slot *s, const asx_semaphore_waiter *waiter,
                               asx_semaphore_permit *out, const asx_cx *cx) {
    out->sem_slot = waiter->sem_slot;
    out->generation = waiter->generation;
    out->obligation = ASX_INVALID_ID;
    if (!s->is_mutex && cx != NULL && cx->task_id != ASX_INVALID_ID) {
        asx_obligation_id id;
        if (asx_obligation_reserve_ex(cx->region_id, ASX_OBLIGATION_KIND_SEMAPHORE_PERMIT,
                                      cx->task_id, &id) == ASX_OK) {
            out->obligation = id;
        }
    }
    return ASX_OK;
}

/* Hand a granted permit to the polling caller and retire its waiter. */
static asx_status sem_consume_grant(sem_slot *s, sem_waiter_slot *w,
                                    const asx_semaphore_waiter *waiter, asx_semaphore_permit *out,
                                    const asx_cx *cx) {
    w->acquired = 0; /* consumed by this caller, not returned to the pool */
    sem_waiter_retire(s, w);
    return sem_hand_out(s, waiter, out, cx);
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

    if (!s->is_mutex) {
        uint32_t front;
        /* Cancelled or out of budget: leave the line (Rust's checkpoint
         * comes before any permit check). */
        if (cx != NULL) {
            asx_status cst = asx_cx_checkpoint(cx);
            if (cst != ASX_OK) {
                sem_leave(s, w);
                return cst;
            }
        }
        sem_reap(s);
        /* The front of the line, or a newcomer while nobody is queued, may
         * take a permit; it then wakes the new front if that can run. */
        front = sem_front(s);
        if ((w->queued ? front == waiter->waiter_slot : front == SEM_NO_WAITER) &&
            s->permits >= 1u) {
            asx_status st;
            s->permits--;
            sem_waiter_retire(s, w);
            st = sem_hand_out(s, waiter, out, cx);
            sem_wake_front(s);
            return st;
        }
        if (!w->queued) {
            w->queued = 1;
            w->seq = s->next_seq++;
        }
        (void)asx_wait_park_current(&w->task);
        return ASX_E_PENDING;
    }

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
    if (w->acquired) return sem_consume_grant(s, w, waiter, out, cx);

    /* Settle the line: reclaim waiters of dead tasks (their grants return
     * to the pool) and hand pooled permits to parked waiters in order. */
    sem_reap(s);
    sem_dispatch_parked(s);
    if (w->acquired) return sem_consume_grant(s, w, waiter, out, cx);

    /* Take a pooled permit only when no earlier arrival is still waiting. */
    if (s->permits > 0 && sem_next_in_line(s, waiter->waiter_slot)) {
        s->permits--;
        sem_waiter_retire(s, w);
        /* Permits left over belong to the parked waiters behind us. */
        sem_dispatch_parked(s);
        return sem_hand_out(s, waiter, out, cx);
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
        if (!s->is_mutex) {
            sem_leave(s, &s->waiters[waiter->waiter_slot]);
            return ASX_OK;
        }
        /* A lock already handed over goes back and on to the next waiter. */
        sem_waiter_retire(s, &s->waiters[waiter->waiter_slot]);
        sem_dispatch_parked(s);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Release                                                             */
/* ------------------------------------------------------------------ */

/* Commit the permit's obligation, if it has one. Rust's permit drop
 * commits even when the semaphore is gone (sync/semaphore.rs:1274); a
 * permit whose task already completed was reported leaked and the commit
 * is refused harmlessly. */
static void sem_commit_obligation(const asx_semaphore_permit *permit) {
    asx_status st;
    if (permit->obligation == ASX_INVALID_ID) return;
    st = asx_obligation_commit(permit->obligation);
    (void)st; /* refused only for an obligation already reported leaked */
}

asx_status asx_semaphore_release(asx_semaphore_permit permit) {
    sem_slot *s;
    uint32_t i;
    if (permit.sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[permit.sem_slot];
    if (!s->alive || s->generation != permit.generation) {
        sem_commit_obligation(&permit);
        return ASX_E_STALE_HANDLE;
    }

    sem_reap(s);
    if (!s->is_mutex) {
        /* Back to the pool; the front waiter is woken if it can run. */
        s->permits++;
        sem_wake_front(s);
        sem_commit_obligation(&permit);
        return ASX_OK;
    }
    /* Mutex: hand the lock to the oldest waiter still in line */
    i = sem_first_in_line(s, SEM_NO_WAITER);
    if (i != SEM_NO_WAITER) {
        sem_grant(&s->waiters[i]);
        sem_dispatch_parked(s); /* permits a reap returned */
    } else {
        /* No waiters, return permit to pool */
        s->permits++;
        sem_dispatch_parked(s);
    }
    sem_commit_obligation(&permit);
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
