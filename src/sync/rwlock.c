/*
 * rwlock.c — async read-write lock with writer-preference fairness
 *
 * Multiple concurrent readers or a single exclusive writer.
 * When a writer is waiting, new readers are blocked (writer-preference).
 *
 * Waiters are records in the shared wait-node pool (wait_queue.h), linked
 * in arrival order, so an rwlock has no waiter limit of its own.
 * Unlocking grants the lock directly: to the oldest waiting writer if any
 * writer waits, otherwise to every waiting reader. A writer acquires at
 * poll time only when it is the oldest waiting writer, so writers are
 * served in arrival (FIFO) order.
 *
 * Wake-driven waiting: poll_read / poll_write returning ASX_E_PENDING
 * inside a scheduler poll record the calling task on the waiter and park
 * it; a grant wakes the recorded task.
 *
 * Cancel safety: a waiter that gives up (waiter_cancel or a failing Cx
 * checkpoint) returns a lock it was granted and the lock passes on to the
 * parked waiters now eligible. A parked waiter whose task completed
 * without cancelling is reclaimed lazily the same way, and cancel-pending
 * waiters are skipped when granting. Close wakes every parked waiter so it
 * observes ASX_E_DISCONNECTED.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/sync/rwlock.h>
#include <stddef.h>
#include <string.h>

/* Waiter record flags. */
#define RW_ACQUIRED 0x1u /* the lock was granted to this waiter */
#define RW_WRITE 0x2u    /* write waiter (else read waiter) */

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t readers;         /* active reader count */
    int write_locked;         /* 1 if a writer holds the lock */
    uint32_t writers_waiting; /* count of waiting writers (for preference) */
    asx_wait_queue waiters;   /* waiter records, arrival order */
} rw_slot;

static rw_slot g_slots[ASX_RWLOCK_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static rw_slot *slot_lookup(uint32_t idx, uint16_t gen) {
    rw_slot *s;
    if (idx >= ASX_RWLOCK_MAX) return NULL;
    s = &g_slots[idx];
    if (!s->alive || s->generation != gen) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* Waiter line                                                         */
/* ------------------------------------------------------------------ */

static asx_wait_node *rw_node(uint32_t i) { return asx_wait_node_at(i); }

/* A waiter whose parked task has a cancel request pending: it is skipped
 * when granting (it would not consume the lock). */
static int rw_waiter_doomed(const asx_wait_node *w) {
    return asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DOOMED;
}

static void rw_grant(rw_slot *s, uint32_t i) {
    asx_wait_node *w = rw_node(i);
    w->flags |= RW_ACQUIRED;
    if ((w->flags & RW_WRITE) != 0u) {
        s->write_locked = 1;
    } else {
        s->readers++;
    }
    asx_wait_wake_task(w->task);
}

/* Oldest un-granted, not cancel-pending writer other than `except`
 * (ASX_WAIT_NIL for none), or ASX_WAIT_NIL. */
static uint32_t rw_first_writer(const rw_slot *s, uint32_t except) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        const asx_wait_node *w = rw_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (i == except || (w->flags & (RW_ACQUIRED | RW_WRITE)) != RW_WRITE) continue;
        if (rw_waiter_doomed(w)) continue;
        return i;
    }
    return ASX_WAIT_NIL;
}

/* 1 if no writer that arrived before waiter `idx` still waits. */
static int rw_writer_next_in_line(const rw_slot *s, uint32_t idx) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL && i != idx;
         i = asx_wait_queue_next(i)) {
        const asx_wait_node *w = rw_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if ((w->flags & (RW_ACQUIRED | RW_WRITE)) == RW_WRITE && !rw_waiter_doomed(w)) return 0;
    }
    return 1;
}

/* Grant waiting readers (all of them, or only the parked ones), oldest
 * first. */
static void rw_grant_readers(rw_slot *s, int parked_only) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        const asx_wait_node *w = rw_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if ((w->flags & (RW_ACQUIRED | RW_WRITE)) != 0u) continue;
        if (parked_only && !asx_handle_is_valid(w->task)) continue;
        if (rw_waiter_doomed(w)) continue;
        rw_grant(s, i);
    }
}

/* Wake eligible waiters after an unlock. Writer-preference: if any writer
 * is waiting, grant the oldest one; otherwise grant all waiting readers. */
static void wake_waiters(rw_slot *s) {
    uint32_t i;

    if (s->write_locked || s->readers > 0) return;

    if (s->writers_waiting > 0) {
        i = rw_first_writer(s, ASX_WAIT_NIL);
        if (i != ASX_WAIT_NIL) {
            rw_grant(s, i);
            return;
        }
    }

    /* No writer to serve — wake all waiting readers */
    rw_grant_readers(s, 0);
}

/* Hand the lock to parked waiters that became eligible after a cancel or
 * a reap. Waiters that are not parked acquire on their own next poll; the
 * oldest writer, parked or not, keeps its turn. */
static void rw_dispatch_parked(rw_slot *s) {
    uint32_t i;

    if (s->write_locked) return;

    if (s->writers_waiting > 0) {
        if (s->readers > 0) return;
        i = rw_first_writer(s, ASX_WAIT_NIL);
        if (i != ASX_WAIT_NIL && asx_handle_is_valid(rw_node(i)->task)) rw_grant(s, i);
        return;
    }

    rw_grant_readers(s, 1);
}

/* Release a waiter that leaves without consuming its grant: a granted
 * lock is returned, a write waiter stops counting toward preference. */
static void rw_waiter_retire(rw_slot *s, uint32_t i) {
    const asx_wait_node *w = rw_node(i);
    if ((w->flags & RW_ACQUIRED) != 0u) {
        /* Permit was granted — return it */
        if ((w->flags & RW_WRITE) != 0u) {
            s->write_locked = 0;
        } else {
            if (s->readers > 0) s->readers--;
        }
    }
    if ((w->flags & RW_WRITE) != 0u && s->writers_waiting > 0) s->writers_waiting--;
    asx_wait_record_release(&s->waiters, i);
}

/* Reclaim parked waiters whose task completed without cancelling, then
 * pass on whatever they held. */
static void rw_reap(rw_slot *s) {
    uint32_t i = asx_wait_queue_first(&s->waiters);
    int reaped = 0;
    while (i != ASX_WAIT_NIL) {
        uint32_t next = asx_wait_queue_next(i);
        const asx_wait_node *w = rw_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) {
            rw_waiter_retire(s, i);
            reaped = 1;
        }
        i = next;
    }
    if (reaped) rw_dispatch_parked(s);
}

/* Wait-node pool reclamation: retire this rwlock's dead waiters. */
static void rw_reap_queue(asx_wait_queue *q) {
    rw_reap((rw_slot *)(void *)((char *)q - offsetof(rw_slot, waiters)));
}

/* Consume a granted lock: the waiter record is done. */
static void rw_waiter_consume(rw_slot *s, uint32_t i) {
    if ((rw_node(i)->flags & RW_WRITE) != 0u && s->writers_waiting > 0) s->writers_waiting--;
    asx_wait_record_release(&s->waiters, i);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_create(asx_rwlock_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_RWLOCK_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].readers = 0;
            g_slots[i].write_locked = 0;
            g_slots[i].writers_waiting = 0;
            asx_wait_records_init(&g_slots[i].waiters, rw_reap_queue);
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_rwlock_close(asx_rwlock_handle handle) {
    rw_slot *s = slot_lookup(handle.slot, handle.generation);
    uint32_t i;
    if (s == NULL) return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        asx_wait_wake_task(rw_node(i)->task);
    }
    asx_wait_queue_clear(&s->waiters);
    s->alive = 0;
    s->writers_waiting = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Read lock (non-blocking)                                            */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_try_read(asx_rwlock_handle handle, asx_rwlock_read_guard *out) {
    rw_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    /* Writer-preference: block if write-locked or writer waiting */
    if (s->write_locked || s->writers_waiting > 0) return ASX_E_WOULD_BLOCK;

    s->readers++;
    out->rw_slot = handle.slot;
    out->generation = handle.generation;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Write lock (non-blocking)                                           */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_try_write(asx_rwlock_handle handle, asx_rwlock_write_guard *out) {
    rw_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    if (s->write_locked || s->readers > 0) return ASX_E_WOULD_BLOCK;

    s->write_locked = 1;
    out->rw_slot = handle.slot;
    out->generation = handle.generation;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Read lock (async)                                                   */
/* ------------------------------------------------------------------ */

static asx_status rw_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out, int is_write) {
    rw_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    i = asx_wait_record_add(&s->waiters);
    if (i == ASX_WAIT_NIL) return ASX_E_RESOURCE_EXHAUSTED;

    if (is_write) {
        rw_node(i)->flags = RW_WRITE;
        s->writers_waiting++;
    }
    out->rw_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    out->waiter_generation = rw_node(i)->generation;
    out->is_write = is_write;
    return ASX_OK;
}

asx_status asx_rwlock_read_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out) {
    return rw_begin(handle, out, 0);
}

/* Shared prologue of poll_read / poll_write: validate, run the Cx
 * checkpoint (giving up passes the lock on), and settle the line. */
static asx_status rw_poll_prologue(asx_rwlock_waiter *waiter, asx_cx *cx, rw_slot **out_s,
                                   asx_wait_node **out_w) {
    rw_slot *s;
    asx_wait_node *w;

    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    w = asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation);
    if (w == NULL) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            rw_waiter_retire(s, waiter->waiter_slot);
            rw_dispatch_parked(s);
            return cst;
        }
    }

    if ((w->flags & RW_ACQUIRED) == 0u) rw_reap(s);
    *out_s = s;
    *out_w = w;
    return ASX_OK;
}

asx_status asx_rwlock_poll_read(asx_rwlock_waiter *waiter, asx_rwlock_read_guard *out, asx_cx *cx) {
    rw_slot *s;
    asx_wait_node *w;
    asx_status st;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = rw_poll_prologue(waiter, cx, &s, &w);
    if (st != ASX_OK) return st;

    /* Already granted by a prior unlock? */
    if ((w->flags & RW_ACQUIRED) != 0u) {
        rw_waiter_consume(s, waiter->waiter_slot);
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Writer-preference: block if write-locked or writer waiting */
    if (!s->write_locked && s->writers_waiting == 0) {
        s->readers++;
        rw_waiter_consume(s, waiter->waiter_slot);
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Inside a scheduler poll, park until granted (or closed). */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Write lock (async)                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_write_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out) {
    return rw_begin(handle, out, 1);
}

asx_status asx_rwlock_poll_write(asx_rwlock_waiter *waiter, asx_rwlock_write_guard *out,
                                 asx_cx *cx) {
    rw_slot *s;
    asx_wait_node *w;
    asx_status st;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = rw_poll_prologue(waiter, cx, &s, &w);
    if (st != ASX_OK) return st;

    /* Already granted by a prior unlock? */
    if ((w->flags & RW_ACQUIRED) != 0u) {
        rw_waiter_consume(s, waiter->waiter_slot);
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Can acquire only if no readers, no writer, and no earlier writer */
    if (!s->write_locked && s->readers == 0 && rw_writer_next_in_line(s, waiter->waiter_slot)) {
        s->write_locked = 1;
        rw_waiter_consume(s, waiter->waiter_slot);
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Inside a scheduler poll, park until granted (or closed). */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Cancel async acquisition                                            */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_waiter_cancel(asx_rwlock_waiter *waiter) {
    rw_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_OK; /* already closed */

    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;
    if (asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation) != NULL) {
        /* A granted lock is returned and passes on to parked waiters. */
        rw_waiter_retire(s, waiter->waiter_slot);
        rw_dispatch_parked(s);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Unlock                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_read_unlock(asx_rwlock_read_guard guard) {
    rw_slot *s = slot_lookup(guard.rw_slot, guard.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (s->readers == 0) return ASX_E_INVALID_STATE;

    s->readers--;

    /* Only wake waiters when all readers have released */
    if (s->readers == 0) {
        rw_reap(s);
        wake_waiters(s);
    }

    return ASX_OK;
}

asx_status asx_rwlock_write_unlock(asx_rwlock_write_guard guard) {
    rw_slot *s = slot_lookup(guard.rw_slot, guard.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (!s->write_locked) return ASX_E_INVALID_STATE;

    s->write_locked = 0;
    rw_reap(s);
    wake_waiters(s);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_rwlock_reader_count(asx_rwlock_handle handle) {
    rw_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return 0;
    return s->readers;
}

int asx_rwlock_is_write_locked(asx_rwlock_handle handle) {
    rw_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return 0;
    return s->write_locked;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_rwlock_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].readers = 0;
        g_slots[i].write_locked = 0;
        g_slots[i].writers_waiting = 0;
        asx_wait_records_init(&g_slots[i].waiters, rw_reap_queue);
    }
    g_slot_count = 0;
}
