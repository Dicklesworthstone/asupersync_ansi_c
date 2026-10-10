/*
 * rwlock.c — async read-write lock with bounded writer-preference fairness
 *
 * Multiple concurrent readers or a single exclusive writer, following
 * Rust's sync/rwlock.rs:
 *
 *   - An acquire joins the line at its first poll that cannot take the lock
 *     at once (its record moves to the tail then); before that it is
 *     invisible. A read takes the lock at once when no writer holds it or
 *     waits; a write when nobody holds it and no writer is queued.
 *   - A queued waiter only ever gets the lock by grant: re-polling it
 *     without one leaves it pending (Rust's replace_waker).
 *   - The last reader's release hands the lock to the oldest queued writer.
 *     A writer's release hands it to the oldest queued writer if that one
 *     arrived before every queued reader (should_wake_writer); otherwise to
 *     the queued readers that arrived before the first queued writer (all
 *     of them if none is queued).
 *   - Bounded reader admission: after RW_MAX_WRITER_STREAK consecutive
 *     writer hand-offs while readers were queued, a writer's release admits
 *     exactly the oldest queued reader, provided a writer is still queued
 *     (take_forced_reader_turn).
 *   - Grants go to the front of a line whatever its task's cancel state, as
 *     Rust pops the queue front unconditionally; a cancel-pending grantee
 *     gives the lock back at its next poll (bd-z783, bd-rm2d).
 *   - try_read fails while a writer holds or waits; try_write while anyone
 *     holds the lock or a writer waits.
 *
 * Waiters are records in the shared wait-node pool (wait_queue.h), so an
 * rwlock has no waiter limit of its own. One list in arrival order stands
 * in for Rust's reader queue and writer queue with their arrival ids.
 *
 * Wake-driven waiting: poll_read / poll_write returning ASX_E_PENDING
 * inside a scheduler poll record the calling task on the waiter and park
 * it; a grant wakes the recorded task.
 *
 * Giving up (waiter_cancel, a failing Cx checkpoint, or a parked waiter
 * whose task completed, reclaimed lazily) follows Rust's
 * abandon_read_waiter / abandon_write_waiter: a queued reader leaves
 * quietly; a queued writer that was the last waiting writer, with no
 * writer holding the lock, admits every queued reader; a granted lock is
 * released as its guard would be. Close wakes every parked waiter so it
 * observes ASX_E_DISCONNECTED.
 *
 * Poisoning: a write guard released as dropped during a panic
 * (asx_rwlock_write_unlock_poisoned; Rust's RwLockWriteGuard drop,
 * rwlock.rs:1083-1090) poisons the lock and wakes every queued waiter,
 * readers then writers, granting nothing. try_read / try_write, and every
 * poll after its Cx checkpoint, then fail with ASX_E_INVALID_STATE (Rust
 * RwLockError::Poisoned). A read guard never poisons.
 * Not ported: write-guard downgrade.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/sync/rwlock.h>
#include <stddef.h>
#include <string.h>

/* Waiter record flags. A record that is neither queued nor granted has not
 * been polled into the line yet. */
#define RW_ACQUIRED 0x1u /* the lock was granted to this waiter */
#define RW_WRITE 0x2u    /* write waiter (else read waiter) */
#define RW_QUEUED 0x4u   /* in line, not granted (a poll had to wait) */

/* Rust's MAX_CONSECUTIVE_WRITERS_BEFORE_READER_BATCH. */
#define RW_MAX_WRITER_STREAK 16u

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t readers;         /* readers holding the lock, granted ones included */
    int write_locked;         /* a writer holds the lock (or was granted it) */
    uint32_t writers_waiting; /* queued writers plus granted ones not yet polled */
    uint32_t writer_streak;   /* writer hand-offs in a row while readers were queued */
    int poisoned;             /* a write guard was dropped while its task panicked */
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
/* The line                                                            */
/* ------------------------------------------------------------------ */

static asx_wait_node *rw_node(uint32_t i) { return asx_wait_node_at(i); }

static int rw_is_queued(uint32_t i) { return (rw_node(i)->flags & RW_QUEUED) != 0u; }

static int rw_is_writer(uint32_t i) { return (rw_node(i)->flags & RW_WRITE) != 0u; }

/* The oldest queued writer (want_write 1) or reader (0), or ASX_WAIT_NIL. */
static uint32_t rw_front(const rw_slot *s, int want_write) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (rw_is_queued(i) && rw_is_writer(i) == want_write) return i;
    }
    return ASX_WAIT_NIL;
}

/* Grant queued waiter i the lock and wake its task. */
static void rw_grant(rw_slot *s, uint32_t i) {
    asx_wait_node *w = rw_node(i);
    w->flags = (uint8_t)((w->flags | RW_ACQUIRED) & ~RW_QUEUED);
    if ((w->flags & RW_WRITE) != 0u) {
        s->write_locked = 1;
    } else {
        s->readers++;
    }
    asx_wait_wake_task(w->task);
}

/* Hand the lock to the oldest queued writer, if any (pop_writer_waiter);
 * the hand-off extends the writer streak while readers are queued. */
static void rw_grant_front_writer(rw_slot *s) {
    uint32_t i = rw_front(s, 1);
    if (i == ASX_WAIT_NIL) return;
    rw_grant(s, i);
    if (rw_front(s, 0) == ASX_WAIT_NIL) {
        s->writer_streak = 0;
    } else if (s->writer_streak < UINT32_MAX) {
        s->writer_streak++;
    }
}

/* Grant queued readers in arrival order up to the first queued writer
 * (take_eligible_reader_waiters), or all of them when `all` is set
 * (drain_reader_waiters). Returns the number granted. */
static uint32_t rw_grant_readers(rw_slot *s, int all) {
    uint32_t n = 0;
    uint32_t i = asx_wait_queue_first(&s->waiters);
    while (i != ASX_WAIT_NIL) {
        uint32_t next = asx_wait_queue_next(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (rw_is_queued(i)) {
            if (!rw_is_writer(i)) {
                rw_grant(s, i);
                n++;
            } else if (!all) {
                break;
            }
        }
        i = next;
    }
    return n;
}

/* Poisoned: wake every queued waiter, readers then writers, each in
 * arrival order, granting nothing (queued_waiter_wakers); each fails at
 * its next poll. */
static void rw_wake_queued(const rw_slot *s) {
    int want_write;
    uint32_t i;
    for (want_write = 0; want_write <= 1; want_write++) {
        for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
            ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
            if (rw_is_queued(i) && rw_is_writer(i) == want_write) {
                asx_wait_wake_task(rw_node(i)->task);
            }
        }
    }
}

/* The lock is no longer write-held: Rust release_writer's policy. */
static void rw_after_writer(rw_slot *s) {
    uint32_t reader = rw_front(s, 0);
    uint32_t writer = rw_front(s, 1);
    int force = s->writer_streak >= RW_MAX_WRITER_STREAK && reader != ASX_WAIT_NIL &&
                writer != ASX_WAIT_NIL;
    uint32_t i;

    if (s->poisoned) {
        rw_wake_queued(s);
        return;
    }
    if (force) {
        /* Forced turn: exactly the oldest queued reader; the head writer
         * keeps its place. */
        rw_grant(s, reader);
        s->writer_streak = 0;
        return;
    }
    /* The head writer goes next if it arrived before every queued reader
     * (should_wake_writer). */
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (rw_is_queued(i)) break;
    }
    if (i != ASX_WAIT_NIL && rw_is_writer(i)) {
        rw_grant_front_writer(s);
        return;
    }
    if (rw_grant_readers(s, 0) > 0u) s->writer_streak = 0;
}

/* A reader stopped holding the lock: Rust release_reader. */
static void rw_after_reader(rw_slot *s) {
    if (s->readers > 0u) s->readers--;
    if (s->readers == 0u && s->writers_waiting > 0u) rw_grant_front_writer(s);
}

/* Waiter i gives up (abandon_read_waiter / abandon_write_waiter): a lock
 * it was granted is released, a queued writer stops counting, and the
 * record is released. */
static void rw_abandon(rw_slot *s, uint32_t i) {
    uint8_t flags = rw_node(i)->flags;
    asx_wait_record_release(&s->waiters, i);
    if ((flags & RW_WRITE) == 0u) {
        if ((flags & RW_ACQUIRED) != 0u) rw_after_reader(s);
        return;
    }
    if ((flags & (RW_QUEUED | RW_ACQUIRED)) == 0u) return; /* never counted */
    if (s->writers_waiting > 0u) s->writers_waiting--;
    if ((flags & RW_ACQUIRED) != 0u) {
        s->write_locked = 0;
        rw_after_writer(s);
    } else if (s->writers_waiting == 0u && !s->write_locked && !s->poisoned) {
        (void)rw_grant_readers(s, 1);
    }
}

/* Reclaim parked waiters whose task completed without giving up, as
 * dropping their Rust futures would. */
static void rw_reap(rw_slot *s) {
    uint32_t i = asx_wait_queue_first(&s->waiters);
    while (i != ASX_WAIT_NIL) {
        uint32_t next = asx_wait_queue_next(i);
        const asx_wait_node *w = rw_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) {
            rw_abandon(s, i);
            /* Abandoning may grant others but releases only record i. */
        }
        i = next;
    }
}

/* Wait-node pool reclamation: retire this rwlock's dead waiters. */
static void rw_reap_queue(asx_wait_queue *q) {
    rw_reap((rw_slot *)(void *)((char *)q - offsetof(rw_slot, waiters)));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void rw_slot_init(rw_slot *s) {
    s->readers = 0;
    s->write_locked = 0;
    s->writers_waiting = 0;
    s->writer_streak = 0;
    s->poisoned = 0;
    asx_wait_records_init(&s->waiters, rw_reap_queue);
}

asx_status asx_rwlock_create(asx_rwlock_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_RWLOCK_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            rw_slot_init(&g_slots[i]);
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
/* Non-blocking acquisition                                            */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_try_read(asx_rwlock_handle handle, asx_rwlock_read_guard *out) {
    rw_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (s->poisoned) return ASX_E_INVALID_STATE; /* checked first (rwlock.rs:335) */

    /* Writer-preference: block if write-locked or writer waiting */
    if (s->write_locked || s->writers_waiting > 0) return ASX_E_WOULD_BLOCK;

    s->readers++;
    out->rw_slot = handle.slot;
    out->generation = handle.generation;
    return ASX_OK;
}

asx_status asx_rwlock_try_write(asx_rwlock_handle handle, asx_rwlock_write_guard *out) {
    rw_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (s->poisoned) return ASX_E_INVALID_STATE; /* checked first (rwlock.rs:361) */

    if (s->write_locked || s->readers > 0 || s->writers_waiting > 0) return ASX_E_WOULD_BLOCK;

    s->write_locked = 1;
    out->rw_slot = handle.slot;
    out->generation = handle.generation;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Async acquisition                                                   */
/* ------------------------------------------------------------------ */

static asx_status rw_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out, int is_write) {
    rw_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    i = asx_wait_record_add(&s->waiters);
    if (i == ASX_WAIT_NIL) return ASX_E_RESOURCE_EXHAUSTED;

    if (is_write) rw_node(i)->flags = RW_WRITE;
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

asx_status asx_rwlock_write_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out) {
    return rw_begin(handle, out, 1);
}

/* One poll of a read or write acquire (Rust ReadFuture / WriteFuture). */
static asx_status rw_poll(asx_rwlock_waiter *waiter, asx_cx *cx, int is_write) {
    rw_slot *s;
    asx_wait_node *w;
    uint32_t i;

    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;
    i = waiter->waiter_slot;
    w = asx_wait_record_get(&s->waiters, i, waiter->waiter_generation);
    if (w == NULL || rw_is_writer(i) != is_write) return ASX_E_INVALID_STATE;

    /* Cancelled or out of budget: give up, before looking at the lock. */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            rw_abandon(s, i);
            return cst;
        }
    }

    /* Poisoned: give up (a grant is released as abandon does) and fail,
     * before looking at a grant (ReadFuture / WriteFuture,
     * rwlock.rs:786-791, :906-912). */
    if (s->poisoned) {
        rw_abandon(s, i);
        return ASX_E_INVALID_STATE;
    }

    /* Granted by a release: the lock is ours. */
    if ((w->flags & RW_ACQUIRED) != 0u) {
        if (is_write && s->writers_waiting > 0u) s->writers_waiting--;
        asx_wait_record_release(&s->waiters, i);
        return ASX_OK;
    }

    if ((w->flags & RW_QUEUED) == 0u) {
        rw_reap(s);
        /* First poll: take the lock at once if the policy allows. */
        if (is_write ? !s->write_locked && s->readers == 0u && rw_front(s, 1) == ASX_WAIT_NIL
                     : !s->write_locked && s->writers_waiting == 0u) {
            if (is_write) {
                s->write_locked = 1;
            } else {
                s->readers++;
            }
            asx_wait_record_release(&s->waiters, i);
            return ASX_OK;
        }
        /* Join the line now, behind everyone already queued. */
        w->flags |= RW_QUEUED;
        asx_wait_record_move_to_tail(&s->waiters, i);
        if (is_write) s->writers_waiting++;
    }

    /* Inside a scheduler poll, park until granted (or closed). */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

asx_status asx_rwlock_poll_read(asx_rwlock_waiter *waiter, asx_rwlock_read_guard *out, asx_cx *cx) {
    asx_status st;
    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = rw_poll(waiter, cx, 0);
    if (st == ASX_OK) {
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
    }
    return st;
}

asx_status asx_rwlock_poll_write(asx_rwlock_waiter *waiter, asx_rwlock_write_guard *out,
                                 asx_cx *cx) {
    asx_status st;
    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = rw_poll(waiter, cx, 1);
    if (st == ASX_OK) {
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
    }
    return st;
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
        rw_abandon(s, waiter->waiter_slot);
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

    rw_reap(s);
    rw_after_reader(s);
    return ASX_OK;
}

asx_status asx_rwlock_write_unlock(asx_rwlock_write_guard guard) {
    rw_slot *s = slot_lookup(guard.rw_slot, guard.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (!s->write_locked) return ASX_E_INVALID_STATE;

    s->write_locked = 0;
    rw_reap(s);
    rw_after_writer(s);
    return ASX_OK;
}

asx_status asx_rwlock_write_unlock_poisoned(asx_rwlock_write_guard guard) {
    rw_slot *s = slot_lookup(guard.rw_slot, guard.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (!s->write_locked) return ASX_E_INVALID_STATE;
    s->poisoned = 1;
    return asx_rwlock_write_unlock(guard);
}

int asx_rwlock_is_poisoned(asx_rwlock_handle handle) {
    rw_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return 0;
    return s->poisoned;
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
        rw_slot_init(&g_slots[i]);
    }
    g_slot_count = 0;
}
