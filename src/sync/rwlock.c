/*
 * rwlock.c — async read-write lock with writer-preference fairness
 *
 * Multiple concurrent readers or a single exclusive writer.
 * When a writer is waiting, new readers are blocked (writer-preference).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/rwlock.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;
    int acquired; /* lock was granted to this waiter */
    int is_write; /* 1 = write waiter, 0 = read waiter */
} rw_waiter_slot;

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t readers;         /* active reader count */
    int write_locked;         /* 1 if a writer holds the lock */
    uint32_t writers_waiting; /* count of waiting writers (for preference) */
    rw_waiter_slot waiters[ASX_RWLOCK_MAX_WAITERS];
    uint32_t waiter_count;
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

    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) { s->waiters[i].active = 0; }
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

asx_status asx_rwlock_read_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out) {
    rw_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) {
            s->waiters[i].active = 1;
            s->waiters[i].acquired = 0;
            s->waiters[i].is_write = 0;
            s->waiter_count++;
            out->rw_slot = handle.slot;
            out->waiter_slot = i;
            out->generation = handle.generation;
            out->is_write = 0;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_rwlock_poll_read(asx_rwlock_waiter *waiter, asx_rwlock_read_guard *out, asx_cx *cx) {
    rw_slot *s;
    rw_waiter_slot *w;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_RWLOCK_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;

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

    /* Already granted by a prior unlock? */
    if (w->acquired) {
        w->active = 0;
        w->acquired = 0;
        s->waiter_count--;
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Writer-preference: block if write-locked or writer waiting */
    if (!s->write_locked && s->writers_waiting == 0) {
        s->readers++;
        w->active = 0;
        s->waiter_count--;
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Write lock (async)                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_rwlock_write_begin(asx_rwlock_handle handle, asx_rwlock_waiter *out) {
    rw_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        if (!s->waiters[i].active) {
            s->waiters[i].active = 1;
            s->waiters[i].acquired = 0;
            s->waiters[i].is_write = 1;
            s->waiter_count++;
            s->writers_waiting++;
            out->rw_slot = handle.slot;
            out->waiter_slot = i;
            out->generation = handle.generation;
            out->is_write = 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_rwlock_poll_write(asx_rwlock_waiter *waiter, asx_rwlock_write_guard *out,
                                 asx_cx *cx) {
    rw_slot *s;
    rw_waiter_slot *w;

    if (waiter == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = slot_lookup(waiter->rw_slot, waiter->generation);
    if (s == NULL) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_RWLOCK_MAX_WAITERS) return ASX_E_INVALID_ARGUMENT;

    w = &s->waiters[waiter->waiter_slot];
    if (!w->active) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            w->active = 0;
            s->waiter_count--;
            s->writers_waiting--;
            return cst;
        }
    }

    /* Already granted by a prior unlock? */
    if (w->acquired) {
        w->active = 0;
        w->acquired = 0;
        s->waiter_count--;
        s->writers_waiting--;
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

    /* Can acquire only if no readers and no writer */
    if (!s->write_locked && s->readers == 0) {
        s->write_locked = 1;
        w->active = 0;
        s->waiter_count--;
        s->writers_waiting--;
        out->rw_slot = waiter->rw_slot;
        out->generation = waiter->generation;
        return ASX_OK;
    }

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
        s->waiter_count--;
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Unlock                                                              */
/* ------------------------------------------------------------------ */

/* Wake eligible waiters after state change. Writer-preference:
 * if any writer is waiting, wake exactly one writer.
 * Otherwise wake all waiting readers. */
static void wake_waiters(rw_slot *s) {
    uint32_t i;

    /* First pass: check for waiting writers (preference) */
    if (s->writers_waiting > 0) {
        for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
            if (s->waiters[i].active && !s->waiters[i].acquired && s->waiters[i].is_write) {
                /* Grant write lock to this waiter */
                s->waiters[i].acquired = 1;
                s->write_locked = 1;
                return;
            }
        }
    }

    /* No writers waiting — wake all waiting readers */
    for (i = 0; i < ASX_RWLOCK_MAX_WAITERS; i++) {
        if (s->waiters[i].active && !s->waiters[i].acquired && !s->waiters[i].is_write) {
            s->waiters[i].acquired = 1;
            s->readers++;
        }
    }
}

asx_status asx_rwlock_read_unlock(asx_rwlock_read_guard guard) {
    rw_slot *s = slot_lookup(guard.rw_slot, guard.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (s->readers == 0) return ASX_E_INVALID_STATE;

    s->readers--;

    /* Only wake waiters when all readers have released */
    if (s->readers == 0) { wake_waiters(s); }

    return ASX_OK;
}

asx_status asx_rwlock_write_unlock(asx_rwlock_write_guard guard) {
    rw_slot *s = slot_lookup(guard.rw_slot, guard.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (!s->write_locked) return ASX_E_INVALID_STATE;

    s->write_locked = 0;
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
        memset(g_slots[i].waiters, 0, sizeof(g_slots[i].waiters));
    }
    g_slot_count = 0;
}
