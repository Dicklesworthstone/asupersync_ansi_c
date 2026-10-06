/*
 * waker.c — wake registration for task readiness signaling
 *
 * Uses bounded sequence-tagged signaling to preserve deterministic wake
 * ordering across timer/reactor/worker boundaries.
 *
 * Thread safety: asx_waker_wake() may be called from blocking-pool worker
 * threads while the scheduler thread registers, drains, or blocks, so
 * every arena access holds a short spinlock. The scheduler publishes
 * "about to block" under the same lock (asx_waker_prepare_block_internal):
 * a wake that lands after that check sees the flag and interrupts the
 * reactor through asx_runtime_reactor_notify(), so no wake is lost
 * between the scheduler's last drain and its blocking wait.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/platform/atomics.h>
#include <asx/runtime/waker.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_task_id task;
    uint16_t generation;
    int alive;
    int signaled;
    uint64_t signal_sequence;
} asx_waker_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_waker_slot g_slots[ASX_MAX_WAKERS];
static uint32_t g_slot_count = 0;
static uint32_t g_active_count = 0;
static uint64_t g_signal_sequence = 0;
static int g_sched_blocking = 0; /* scheduler is (about to be) blocked */
static asx_atomic_u32 g_waker_lock;

static void waker_lock(void) {
    uint32_t expected;
    for (;;) {
        ASX_CHECKPOINT_WAIVER("spinlock: critical sections are a few stores");
        expected = 0u;
        if (asx_atomic_u32_compare_exchange(&g_waker_lock, &expected, 1u)) return;
    }
}

static void waker_unlock(void) { asx_atomic_u32_store(&g_waker_lock, 0u); }

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* Slot for a live, current-generation handle, or NULL. Lock held. */
static asx_waker_slot *waker_slot_locked(const asx_waker *waker) {
    if (waker == NULL || waker->slot >= g_slot_count) return NULL;
    if (g_slots[waker->slot].generation != waker->generation) return NULL;
    if (!g_slots[waker->slot].alive) return NULL;
    return &g_slots[waker->slot];
}

void asx_waker_reset(void) {
    uint32_t i;
    waker_lock();
    for (i = 0; i < ASX_MAX_WAKERS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].signaled = 0;
        g_slots[i].signal_sequence = 0;
        g_slots[i].task = ASX_INVALID_ID;
    }
    g_slot_count = 0;
    g_active_count = 0;
    g_signal_sequence = 0;
    g_sched_blocking = 0;
    waker_unlock();
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_waker_register(asx_task_id task, asx_waker *out_waker) {
    uint32_t idx = ASX_MAX_WAKERS;
    uint32_t i;

    if (task == ASX_INVALID_ID || out_waker == NULL) return ASX_E_INVALID_ARGUMENT;

    waker_lock();
    for (i = 0; i < g_slot_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
        if (!g_slots[i].alive) {
            idx = i;
            break;
        }
    }
    if (idx == ASX_MAX_WAKERS) {
        if (g_slot_count >= ASX_MAX_WAKERS) {
            waker_unlock();
            return ASX_E_RESOURCE_EXHAUSTED;
        }
        idx = g_slot_count++;
    }

    g_slots[idx].task = task;
    g_slots[idx].generation = next_gen(g_slots[idx].generation);
    g_slots[idx].alive = 1;
    g_slots[idx].signaled = 0;
    g_slots[idx].signal_sequence = 0;
    g_active_count++;

    out_waker->slot = idx;
    out_waker->generation = g_slots[idx].generation;
    waker_unlock();
    return ASX_OK;
}

void asx_waker_deregister(asx_waker *waker) {
    asx_waker_slot *s;
    waker_lock();
    s = waker_slot_locked(waker);
    if (s != NULL) {
        s->alive = 0;
        s->signaled = 0;
        if (g_active_count > 0) g_active_count--;
    }
    waker_unlock();
}

/* ------------------------------------------------------------------ */
/* Signaling                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_waker_wake(const asx_waker *waker) {
    asx_waker_slot *s;
    int notify;

    if (waker == NULL) return ASX_E_INVALID_ARGUMENT;
    waker_lock();
    s = waker_slot_locked(waker);
    if (s == NULL) {
        waker_unlock();
        return ASX_E_NOT_FOUND;
    }
    if (!s->signaled) {
        s->signaled = 1;
        s->signal_sequence = ++g_signal_sequence;
    }
    notify = g_sched_blocking;
    waker_unlock();

    /* The scheduler committed to blocking before this signal: interrupt
     * its reactor wait (a notify written before the wait starts still
     * makes that wait return). */
    if (notify) asx_runtime_reactor_notify();
    return ASX_OK;
}

asx_status asx_waker_clone(const asx_waker *src, asx_waker *out_clone) {
    asx_status st = ASX_OK;
    if (src == NULL || out_clone == NULL) return ASX_E_INVALID_ARGUMENT;
    waker_lock();
    if (waker_slot_locked(src) == NULL) {
        st = ASX_E_NOT_FOUND;
    } else {
        /* Clone is the same handle (references same slot) */
        out_clone->slot = src->slot;
        out_clone->generation = src->generation;
    }
    waker_unlock();
    return st;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

int asx_waker_is_signaled(const asx_waker *waker) {
    asx_waker_slot *s;
    int signaled;
    waker_lock();
    s = waker_slot_locked(waker);
    signaled = s != NULL ? s->signaled : 0;
    waker_unlock();
    return signaled;
}

void asx_waker_clear(asx_waker *waker) {
    waker_lock();
    if (waker != NULL && waker->slot < g_slot_count &&
        g_slots[waker->slot].generation == waker->generation) {
        g_slots[waker->slot].signaled = 0;
    }
    waker_unlock();
}

asx_task_id asx_waker_task(const asx_waker *waker) {
    asx_waker_slot *s;
    asx_task_id task;
    waker_lock();
    s = waker_slot_locked(waker);
    task = s != NULL ? s->task : ASX_INVALID_ID;
    waker_unlock();
    return task;
}

uint32_t asx_waker_active_count(void) {
    uint32_t n;
    waker_lock();
    n = g_active_count;
    waker_unlock();
    return n;
}

/* ------------------------------------------------------------------ */
/* Drain signaled                                                      */
/* ------------------------------------------------------------------ */

uint32_t asx_waker_drain_signaled(asx_task_id *out_tasks, uint32_t max_tasks) {
    uint32_t count = 0;

    if (out_tasks == NULL || max_tasks == 0) return 0;

    waker_lock();
    while (count < max_tasks) {
        uint32_t i;
        uint32_t best = ASX_MAX_WAKERS;
        uint64_t best_sequence = UINT64_MAX;

        for (i = 0; i < g_slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
            if (g_slots[i].alive && g_slots[i].signaled &&
                g_slots[i].signal_sequence < best_sequence) {
                best = i;
                best_sequence = g_slots[i].signal_sequence;
            }
        }

        if (best == ASX_MAX_WAKERS) break;

        out_tasks[count++] = g_slots[best].task;
        g_slots[best].signaled = 0;
        g_slots[best].signal_sequence = 0;
    }
    waker_unlock();
    return count;
}

/* ------------------------------------------------------------------ */
/* Scheduler block handshake (internal)                                */
/* ------------------------------------------------------------------ */

int asx_waker_prepare_block_internal(void) {
    uint32_t i;
    int pending = 0;
    waker_lock();
    for (i = 0; i < g_slot_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (g_slots[i].alive && g_slots[i].signaled) {
            pending = 1;
            break;
        }
    }
    if (!pending) g_sched_blocking = 1;
    waker_unlock();
    return !pending;
}

void asx_waker_finish_block_internal(void) {
    waker_lock();
    g_sched_blocking = 0;
    waker_unlock();
}
