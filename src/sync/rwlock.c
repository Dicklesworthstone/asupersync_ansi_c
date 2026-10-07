/*
 * rwlock.c — async read-write lock with writer-preference fairness
 *
 * Multiple concurrent readers or a single exclusive writer.
 * When a writer is waiting, new readers are blocked (writer-preference).
 *
 * Waiters are slot records stamped with an arrival sequence number.
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
#include <string.h>

#define RW_NO_WAITER ASX_RWLOCK_MAX_WAITERS

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;
    int acquired;     /* lock was granted to this waiter */
    int is_write;     /* 1 = write waiter, 0 = read waiter */
    uint32_t seq;     /* arrival order */
    asx_task_id task; /* task parked on this waiter, ASX_INVALID_ID if none */
} rw_waiter_slot;

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t readers;         /* active reader count */
    int write_locked;         /* 1 if a writer holds the lock */
    uint32_t writers_waiting; /* count of waiting writers (for preference) */
    rw_waiter_slot waiters[ASX_RWLOCK_MAX_WAITERS];
    uint32_t waiter_count;
    uint32_t next_seq; /* arrival stamp for the next waiter */
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

/* Wrap-safe "a arrived before b". */
static int seq_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

/* A waiter whose parked task has a cancel request pending: it is skipped
 * when granting (it would not consume the lock). */
static int rw_waiter_doomed(const rw_waiter_slot *w) {
    return asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DOOMED;
}

static void rw_grant(rw_slot *s, rw_waiter_slot *w) {
    w->acquired = 1;
    if (w->is_write) {
        s->write_locked = 1;
    } else {
        s->readers++;
    }
    asx_wait_wake_task(w->task);
}

/* Oldest active, un-granted, not cancel-pending writer other than
 * `except` (RW_NO_WAITER for none), or RW_NO_WAITER. */
static uint32_t rw_first_writer(const rw_slot *s, uint32_t except) {
    uint32_t i;
    uint32_t best = RW_NO_WAITER;
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        const rw_waiter_slot *w = &s->waiters[i];
        if (i == except || !w->active || w->acquired || !w->is_write) continue;
        if (best != RW_NO_WAITER && !seq_before(w->seq, s->waiters[best].seq)) continue;
        if (rw_waiter_doomed(w)) continue;
        best = i;
    }
    return best;
}

/* 1 if no writer that arrived before waiter `idx` still waits. */
static int rw_writer_next_in_line(const rw_slot *s, uint32_t idx) {
    uint32_t first = rw_first_writer(s, idx);
    return first == RW_NO_WAITER || !seq_before(s->waiters[first].seq, s->waiters[idx].seq);
}

/* Grant waiting readers (all of them, or only the parked ones). */
static void rw_grant_readers(rw_slot *s, int parked_only) {
    uint32_t i;
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        rw_waiter_slot *w = &s->waiters[i];
        if (!w->active || w->acquired || w->is_write) continue;
        if (parked_only && !asx_handle_is_valid(w->task)) continue;
        if (rw_waiter_doomed(w)) continue;
        rw_grant(s, w);
    }
}

/* Wake eligible waiters after an unlock. Writer-preference: if any writer
 * is waiting, grant the oldest one; otherwise grant all waiting readers. */
static void wake_waiters(rw_slot *s) {
    uint32_t i;

    if (s->write_locked || s->readers > 0) return;

    if (s->writers_waiting > 0) {
        i = rw_first_writer(s, RW_NO_WAITER);
        if (i != RW_NO_WAITER) {
            rw_grant(s, &s->waiters[i]);
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
        i = rw_first_writer(s, RW_NO_WAITER);
        if (i != RW_NO_WAITER && asx_handle_is_valid(s->waiters[i].task)) {
            rw_grant(s, &s->waiters[i]);
        }
        return;
    }

    rw_grant_readers(s, 1);
}

/* Deactivate a waiter that leaves without consuming its grant: a granted
 * lock is returned, a write waiter stops counting toward preference. */
static void rw_waiter_retire(rw_slot *s, rw_waiter_slot *w) {
    if (w->acquired) {
        /* Permit was granted — return it */
        if (w->is_write) {
            s->write_locked = 0;
        } else {
            if (s->readers > 0) s->readers--;
        }
    }
    if (w->is_write && s->writers_waiting > 0) { s->writers_waiting--; }
    w->active = 0;
    w->acquired = 0;
    w->task = ASX_INVALID_ID;
    s->waiter_count--;
}

/* Reclaim parked waiters whose task completed without cancelling, then
 * pass on whatever they held. */
static void rw_reap(rw_slot *s) {
    uint32_t i;
    int reaped = 0;
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        rw_waiter_slot *w = &s->waiters[i];
        if (!w->active || !asx_handle_is_valid(w->task)) continue;
        if (asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) {
            rw_waiter_retire(s, w);
            reaped = 1;
        }
    }
    if (reaped) rw_dispatch_parked(s);
}

static uint32_t rw_free_waiter(rw_slot *s) {
    uint32_t i;
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) return i;
    }
    rw_reap(s);
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) return i;
    }
    return RW_NO_WAITER;
}

/* Consume a granted lock: the waiter record is done. */
static void rw_waiter_consume(rw_slot *s, rw_waiter_slot *w) {
    if (w->is_write && s->writers_waiting > 0) s->writers_waiting--;
    w->active = 0;
    w->acquired = 0;
    w->task = ASX_INVALID_ID;
    s->waiter_count--;
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

asx_status asx_rwlock_close(asx_rwlock_handle handle) {
    rw_slot *s = slot_lookup(handle.slot, handle.generation);
    uint32_t i;
    if (s == NULL) return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        if (s->waiters[i].active) asx_wait_wake_task(s->waiters[i].task);
        s->waiters[i].active = 0;
        s->waiters[i].task = ASX_INVALID_ID;
    }
    s->alive = 0;
    s->waiter_count = 0;
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

    i = rw_free_waiter(s);
    if (i == RW_NO_WAITER) return ASX_E_RESOURCE_EXHAUSTED;

    s->waiters[i].active = 1;
    s->waiters[i].acquired = 0;
    s->waiters[i].is_write = is_write;
    s->waiters[i].seq = s->next_seq++;
    s->waiters[i].task = ASX_INVALID_ID;
    s->waiter_count++;
    if (is_write) s->writers_waiting++;
    out->rw_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    out->is_write = is_write;
    return ASX_OK;
}

asx_status asx_rwlock_read_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out) {
    return rw_begin(handle, out, 0);
}

/* Shared prologue of poll_read / poll_write: validate, run the Cx
 * checkpoint (giving up passes the lock on), and settle the line. */
static asx_status rw_poll_prologue(asx_rwlock_waiter *waiter, asx_cx *cx, rw_slot **out_s,
                                   rw_waiter_slot **out_w) {
    rw_slot *s;
    rw_waiter_slot *w;

    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_RWLOCK_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;

    w = &s->waiters[waiter->waiter_slot];
    if (!w->active) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            rw_waiter_retire(s, w);
            rw_dispatch_parked(s);
            return cst;
        }
    }

    if (!w->acquired) rw_reap(s);
    *out_s = s;
    *out_w = w;
    return ASX_OK;
}

asx_status asx_rwlock_poll_read(asx_rwlock_waiter *waiter, asx_rwlock_read_guard *out, asx_cx *cx) {
    rw_slot *s;
    rw_waiter_slot *w;
    asx_status st;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = rw_poll_prologue(waiter, cx, &s, &w);
    if (st != ASX_OK) return st;

    /* Already granted by a prior unlock? */
    if (w->acquired) {
        rw_waiter_consume(s, w);
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Writer-preference: block if write-locked or writer waiting */
    if (!s->write_locked && s->writers_waiting == 0) {
        s->readers++;
        rw_waiter_consume(s, w);
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
    rw_waiter_slot *w;
    asx_status st;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = rw_poll_prologue(waiter, cx, &s, &w);
    if (st != ASX_OK) return st;

    /* Already granted by a prior unlock? */
    if (w->acquired) {
        rw_waiter_consume(s, w);
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Can acquire only if no readers, no writer, and no earlier writer */
    if (!s->write_locked && s->readers == 0 && rw_writer_next_in_line(s, waiter->waiter_slot)) {
        s->write_locked = 1;
        rw_waiter_consume(s, w);
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
    rw_waiter_slot *w;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_OK; /* already closed */

    if (waiter->waiter_slot >= ASX_RWLOCK_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;
    w = &s->waiters[waiter->waiter_slot];

    if (w->active) {
        /* A granted lock is returned and passes on to parked waiters. */
        rw_waiter_retire(s, w);
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
        g_slots[i].waiter_count = 0;
        g_slots[i].next_seq = 0;
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
