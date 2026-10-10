/*
 * semaphore.c — counting semaphore with permit obligations, and the lock
 * under asx_mutex
 *
 * Waiters are records in the shared wait-node pool (wait_queue.h), linked
 * in arrival order, so a semaphore has no waiter limit of its own. A
 * semaphore and the mutex's one-permit semaphore follow their Rust
 * counterparts, which differ:
 *
 *   Semaphore (Rust sync/semaphore.rs): permits stay pooled. A waiter joins
 *   the line at its first poll that cannot take a permit (its record moves
 *   to the tail then). Only the front of the line may take permits, and a
 *   newcomer only while nobody is queued. A release adds the permit and
 *   wakes the front waiter, if it can now run, and nobody else
 *   (add_permits_deferred); a waiter that takes a permit leaves the line
 *   and wakes the new front, if it can run (poll_with_registration). One
 *   that leaves without a permit (cancel, dropped) wakes the next one only
 *   if it was the front (remove_waiter_and_take_next_waker). Cancel-pending
 *   waiters keep their place: they leave when polled. try_acquire fails
 *   while anyone is queued.
 *
 *   Mutex (Rust sync/mutex.rs): the line is the order of acquire_begin.
 *   Unlock hands the lock to the front waiter (the baton): release grants
 *   the permit directly to the oldest un-granted waiter, waking its task,
 *   or returns it to the pool; a pooled permit owed to the waiters next in
 *   line is granted to them as soon as they are parked; a waiter that
 *   gives up passes a grant on; a cancel-pending waiter is skipped when
 *   granting.
 *
 * Wake-driven waiting: poll_acquire returning ASX_E_PENDING inside a
 * scheduler poll records the calling task on the waiter and parks it. A
 * waiter whose task completed without cancelling is reclaimed lazily (and
 * whenever the wait-node pool needs nodes back). Close wakes every parked
 * waiter so it observes ASX_E_DISCONNECTED.
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
#include <stddef.h>
#include <string.h>

/* Waiter record flags. */
#define SEM_ACQUIRED 0x1u /* mutex: the lock was handed to this waiter */
#define SEM_QUEUED 0x2u   /* semaphore: in line (a poll had to wait) */

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t permits;       /* available permits */
    uint32_t max_permits;   /* initial capacity (for queries) */
    asx_wait_queue waiters; /* waiter records, arrival order */
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

static asx_wait_node *sem_node(uint32_t i) { return asx_wait_node_at(i); }

/* A waiter whose parked task has a cancel request pending: it is skipped
 * when granting (it would not consume the permit). */
static int sem_waiter_doomed(const asx_wait_node *w) {
    return asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DOOMED;
}

/* Release a waiter record, returning a permit it was granted but never
 * consumed to the pool. */
static void sem_waiter_retire(sem_slot *s, uint32_t i) {
    if ((sem_node(i)->flags & SEM_ACQUIRED) != 0u) s->permits++;
    asx_wait_record_release(&s->waiters, i);
}

/* Semaphore: the front of the line, the oldest queued waiter, or
 * ASX_WAIT_NIL. */
static uint32_t sem_front(const sem_slot *s) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if ((sem_node(i)->flags & SEM_QUEUED) != 0u) return i;
    }
    return ASX_WAIT_NIL;
}

/* Semaphore: wake the front waiter if it can take what it asked for now
 * (front_waiter_waker_if_runnable). */
static void sem_wake_front(const sem_slot *s) {
    uint32_t i = sem_front(s);
    if (i != ASX_WAIT_NIL && s->permits >= sem_node(i)->value) {
        asx_wait_wake_task(sem_node(i)->task);
    }
}

/* Semaphore: a waiter leaves the line without a permit; the next one is
 * woken only if this one was the front (remove_waiter_and_take_next_waker). */
static void sem_leave(sem_slot *s, uint32_t i) {
    int was_front = (sem_node(i)->flags & SEM_QUEUED) != 0u && sem_front(s) == i;
    sem_waiter_retire(s, i);
    if (was_front) sem_wake_front(s);
}

/* Reclaim waiters whose parked task completed (or whose slot was reused)
 * without cancelling the acquire. */
static void sem_reap(sem_slot *s) {
    uint32_t i = asx_wait_queue_first(&s->waiters);
    while (i != ASX_WAIT_NIL) {
        uint32_t next = asx_wait_queue_next(i);
        const asx_wait_node *w = sem_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) {
            if (s->is_mutex) {
                sem_waiter_retire(s, i);
            } else {
                sem_leave(s, i);
            }
        }
        i = next;
    }
}

/* Mutex: the oldest un-granted, not cancel-pending waiter other than
 * `except` (ASX_WAIT_NIL for none), or ASX_WAIT_NIL. */
static uint32_t sem_first_in_line(const sem_slot *s, uint32_t except) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        const asx_wait_node *w = sem_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (i == except || (w->flags & SEM_ACQUIRED) != 0u) continue;
        if (sem_waiter_doomed(w)) continue;
        return i;
    }
    return ASX_WAIT_NIL;
}

/* Mutex: 1 if no waiter that arrived before `idx` still waits. */
static int sem_next_in_line(const sem_slot *s, uint32_t idx) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL && i != idx;
         i = asx_wait_queue_next(i)) {
        const asx_wait_node *w = sem_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if ((w->flags & SEM_ACQUIRED) == 0u && !sem_waiter_doomed(w)) return 0;
    }
    return 1;
}

static void sem_grant(uint32_t i) {
    asx_wait_node *w = sem_node(i);
    w->flags |= SEM_ACQUIRED;
    asx_wait_wake_task(w->task);
}

/* Mutex: grant a pooled lock to the waiters next in line while they are
 * parked. Stops at the first one that is not parked: it takes the lock
 * when it polls, so granting past it would break arrival order. A
 * semaphore grants nothing: its waiters take permits when they poll. */
static void sem_dispatch_parked(sem_slot *s) {
    if (!s->is_mutex) return;
    while (s->permits > 0) {
        uint32_t i = sem_first_in_line(s, ASX_WAIT_NIL);
        ASX_CHECKPOINT_WAIVER("bounded: each pass grants a permit or stops");
        if (i == ASX_WAIT_NIL || !asx_handle_is_valid(sem_node(i)->task)) break;
        s->permits--;
        sem_grant(i);
    }
}

/* Wait-node pool reclamation: retire this semaphore's dead waiters. */
static void sem_reap_queue(asx_wait_queue *q) {
    sem_slot *s = (sem_slot *)(void *)((char *)q - offsetof(sem_slot, waiters));
    sem_reap(s);
    sem_dispatch_parked(s);
}

/* The live record a waiter handle names, or ASX_WAIT_NIL. */
static uint32_t sem_waiter_index(sem_slot *s, const asx_semaphore_waiter *waiter) {
    if (asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation) == NULL) {
        return ASX_WAIT_NIL;
    }
    return waiter->waiter_slot;
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
            g_slots[i].is_mutex = is_mutex;
            asx_wait_records_init(&g_slots[i].waiters, sem_reap_queue);
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
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        asx_wait_wake_task(sem_node(i)->task);
    }
    asx_wait_queue_clear(&s->waiters);

    s->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Acquire (non-blocking)                                              */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_try_acquire(asx_semaphore_handle handle, asx_semaphore_permit *out) {
    return asx_semaphore_try_acquire_many(handle, 1u, out);
}

asx_status asx_semaphore_try_acquire_many(asx_semaphore_handle handle, uint32_t count,
                                          asx_semaphore_permit *out) {
    sem_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;
    if (s->is_mutex && count != 1u) return ASX_E_INVALID_ARGUMENT;

    out->sem_slot = handle.slot;
    out->generation = handle.generation;
    out->obligation = ASX_INVALID_ID; /* no Cx to hold it */
    out->count = 0u;
    if (count == 0u) return ASX_OK; /* nothing to take (Rust) */

    /* A semaphore with anyone queued is FIFO-blocked (Rust try_acquire). */
    if (!s->is_mutex) sem_reap(s);
    if (s->permits >= count && (s->is_mutex || sem_front(s) == ASX_WAIT_NIL)) {
        s->permits -= count;
        out->count = count;
        return ASX_OK;
    }
    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Acquire (async)                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_semaphore_acquire_begin(asx_semaphore_handle handle, asx_semaphore_waiter *out) {
    return asx_semaphore_acquire_many_begin(handle, 1u, out);
}

asx_status asx_semaphore_acquire_many_begin(asx_semaphore_handle handle, uint32_t count,
                                            asx_semaphore_waiter *out) {
    sem_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;
    if (s->is_mutex && count != 1u) return ASX_E_INVALID_ARGUMENT;

    i = asx_wait_record_add(&s->waiters);
    if (i == ASX_WAIT_NIL) return ASX_E_RESOURCE_EXHAUSTED;
    sem_node(i)->value = count;

    out->sem_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    out->waiter_generation = sem_node(i)->generation;
    return ASX_OK;
}

/* Fill the permit of `count` handed to an acquire polled with `cx`,
 * reserving its SemaphorePermit obligation for the Cx's task (a
 * semaphore's, not the mutex's). A refused reservation leaves the permit
 * untracked. */
static asx_status sem_hand_out(const sem_slot *s, const asx_semaphore_waiter *waiter,
                               uint32_t count, asx_semaphore_permit *out, const asx_cx *cx) {
    out->sem_slot = waiter->sem_slot;
    out->generation = waiter->generation;
    out->count = count;
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
static asx_status sem_consume_grant(sem_slot *s, uint32_t i, const asx_semaphore_waiter *waiter,
                                    asx_semaphore_permit *out, const asx_cx *cx) {
    /* consumed, not returned to the pool */
    sem_node(i)->flags = (uint8_t)(sem_node(i)->flags & ~SEM_ACQUIRED);
    sem_waiter_retire(s, i);
    return sem_hand_out(s, waiter, 1u, out, cx);
}

asx_status asx_semaphore_poll_acquire(asx_semaphore_waiter *waiter, asx_semaphore_permit *out,
                                      asx_cx *cx) {
    sem_slot *s;
    uint32_t i;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->sem_slot];
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    i = sem_waiter_index(s, waiter);
    if (i == ASX_WAIT_NIL) return ASX_E_INVALID_STATE;

    if (!s->is_mutex) {
        uint32_t front;
        uint32_t count = sem_node(i)->value;
        /* Acquiring nothing succeeds at once, before the checkpoint and
         * with no obligation (Rust poll_with_registration). */
        if (count == 0u) {
            asx_wait_record_release(&s->waiters, i);
            out->sem_slot = waiter->sem_slot;
            out->generation = waiter->generation;
            out->count = 0u;
            out->obligation = ASX_INVALID_ID;
            return ASX_OK;
        }
        /* Cancelled or out of budget: leave the line (Rust's checkpoint
         * comes before any permit check). */
        if (cx != NULL) {
            asx_status cst = asx_cx_checkpoint(cx);
            if (cst != ASX_OK) {
                sem_leave(s, i);
                return cst;
            }
        }
        sem_reap(s);
        /* The front of the line, or a newcomer while nobody is queued, may
         * take its permits, all at once; it then wakes the new front if
         * that can run. */
        front = sem_front(s);
        if (((sem_node(i)->flags & SEM_QUEUED) != 0u ? front == i : front == ASX_WAIT_NIL) &&
            s->permits >= count) {
            asx_status st;
            s->permits -= count;
            sem_waiter_retire(s, i);
            st = sem_hand_out(s, waiter, count, out, cx);
            sem_wake_front(s);
            return st;
        }
        if ((sem_node(i)->flags & SEM_QUEUED) == 0u) {
            /* Joins the line now, behind everyone already queued. */
            sem_node(i)->flags |= SEM_QUEUED;
            asx_wait_record_move_to_tail(&s->waiters, i);
        }
        (void)asx_wait_park_current(&sem_node(i)->task);
        return ASX_E_PENDING;
    }

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            /* Give up: a granted permit passes to the next waiter. */
            sem_waiter_retire(s, i);
            sem_dispatch_parked(s);
            return cst;
        }
    }

    /* Already acquired by a prior release? */
    if ((sem_node(i)->flags & SEM_ACQUIRED) != 0u) return sem_consume_grant(s, i, waiter, out, cx);

    /* Settle the line: reclaim waiters of dead tasks (their grants return
     * to the pool) and hand pooled permits to parked waiters in order. */
    sem_reap(s);
    sem_dispatch_parked(s);
    if ((sem_node(i)->flags & SEM_ACQUIRED) != 0u) return sem_consume_grant(s, i, waiter, out, cx);

    /* Take a pooled permit only when no earlier arrival is still waiting. */
    if (s->permits > 0 && sem_next_in_line(s, i)) {
        s->permits--;
        sem_waiter_retire(s, i);
        /* Permits left over belong to the parked waiters behind us. */
        sem_dispatch_parked(s);
        return sem_hand_out(s, waiter, 1u, out, cx);
    }

    /* Wait for a release; inside a scheduler poll, park until granted. */
    (void)asx_wait_park_current(&sem_node(i)->task);
    return ASX_E_PENDING;
}

asx_status asx_semaphore_acquire_cancel(asx_semaphore_waiter *waiter) {
    sem_slot *s;
    uint32_t i;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->sem_slot >= ASX_SEMAPHORE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->sem_slot];
    if (!s->alive || s->generation != waiter->generation) return ASX_OK;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    i = sem_waiter_index(s, waiter);
    if (i == ASX_WAIT_NIL) return ASX_OK; /* already consumed or given up */
    if (!s->is_mutex) {
        sem_leave(s, i);
        return ASX_OK;
    }
    /* A lock already handed over goes back and on to the next waiter. */
    sem_waiter_retire(s, i);
    sem_dispatch_parked(s);
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
        /* Back to the pool; the front waiter is woken if it can run now.
         * Releasing nothing wakes nobody (add_permits_deferred(0)). */
        if (permit.count > 0u) {
            s->permits =
                permit.count > UINT32_MAX - s->permits ? UINT32_MAX : s->permits + permit.count;
            sem_wake_front(s);
        }
        sem_commit_obligation(&permit);
        return ASX_OK;
    }
    /* Mutex: hand the lock to the oldest waiter still in line */
    i = sem_first_in_line(s, ASX_WAIT_NIL);
    if (i != ASX_WAIT_NIL) {
        sem_grant(i);
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
        asx_wait_records_init(&g_slots[i].waiters, sem_reap_queue);
    }
    g_slot_count = 0;
}
