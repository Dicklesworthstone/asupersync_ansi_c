/*
 * waker.c — wake registration for task readiness signaling
 *
 * Uses bounded sequence-tagged signaling to preserve deterministic wake
 * ordering across timer/reactor/worker boundaries. Each runtime's kernel
 * owns an arena (asx_kernel.wakers); a waker names its arena by the
 * runtime tag in the high half of its slot, so a wake reaches that
 * runtime's arena and sequence counter and no other, and one runtime's
 * registrations never take another's slots or ready order.
 *
 * Thread safety: asx_waker_wake() may be called from blocking-pool worker
 * threads while the scheduler thread registers, drains, or blocks, so
 * every arena access holds a short spinlock (the arena's own). The
 * scheduler publishes "about to block" under the same lock
 * (asx_waker_prepare_block_internal): a wake that lands after that check
 * sees the flag and interrupts the reactor through
 * asx_runtime_reactor_notify(), so no wake is lost between the scheduler's
 * last drain and its blocking wait.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/platform/atomics.h>
#include <asx/runtime/waker.h>
#include <string.h>

#if (ASX_MAX_WAKERS) > 65535
#error "ASX_MAX_WAKERS must fit the low half of a waker slot"
#endif

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

#define WAKER_INDEX_MASK 0xFFFFu
#define WAKER_TAG_SHIFT 16u

/* The kernel a waker was issued by, NULL when no runtime holds its tag. */
static asx_kernel *waker_kernel(const asx_waker *waker) {
    return asx_kernel_for_tag((uint16_t)(waker->slot >> WAKER_TAG_SHIFT));
}

static void waker_lock(asx_waker_state *s) {
    uint32_t expected;
    for (;;) {
        ASX_CHECKPOINT_WAIVER("spinlock: critical sections are a few stores");
        expected = 0u;
        if (asx_atomic_u32_compare_exchange(&s->lock, &expected, 1u)) return;
    }
}

static void waker_unlock(asx_waker_state *s) { asx_atomic_u32_store(&s->lock, 0u); }

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* Slot for a live, current-generation handle, or NULL. Lock held. */
static asx_waker_slot *waker_slot_locked(asx_waker_state *s, const asx_waker *waker) {
    uint32_t idx = waker->slot & WAKER_INDEX_MASK;
    if (idx >= s->slot_count) return NULL;
    if (s->slots[idx].generation != waker->generation) return NULL;
    if (!s->slots[idx].alive) return NULL;
    return &s->slots[idx];
}

void asx_waker_reset_internal(asx_kernel *k) {
    asx_waker_state *s = &k->wakers;
    uint32_t i;
    waker_lock(s);
    for (i = 0; i < ASX_MAX_WAKERS; i++) {
        s->slots[i].generation = next_gen(s->slots[i].generation);
        s->slots[i].alive = 0;
        s->slots[i].signaled = 0;
        s->slots[i].signal_sequence = 0;
        s->slots[i].task = ASX_INVALID_ID;
    }
    s->slot_count = 0;
    s->active_count = 0;
    s->signal_sequence = 0;
    s->sched_blocking = 0;
    waker_unlock(s);
}

void asx_waker_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_waker_reset_internal(k);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_waker_register(asx_task_id task, asx_waker *out_waker) {
    asx_kernel *k;
    asx_waker_state *s;
    uint32_t idx = ASX_MAX_WAKERS;
    uint32_t i;

    if (task == ASX_INVALID_ID || out_waker == NULL) return ASX_E_INVALID_ARGUMENT;
    k = asx_kernel_of(task);
    if (k == NULL) return ASX_E_NOT_FOUND;
    s = &k->wakers;

    waker_lock(s);
    for (i = 0; i < s->slot_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
        if (!s->slots[i].alive) {
            idx = i;
            break;
        }
    }
    if (idx == ASX_MAX_WAKERS) {
        if (s->slot_count >= ASX_MAX_WAKERS) {
            waker_unlock(s);
            return ASX_E_RESOURCE_EXHAUSTED;
        }
        idx = s->slot_count++;
    }

    s->slots[idx].task = task;
    s->slots[idx].generation = next_gen(s->slots[idx].generation);
    s->slots[idx].alive = 1;
    s->slots[idx].signaled = 0;
    s->slots[idx].signal_sequence = 0;
    s->active_count++;

    out_waker->slot = ((uint32_t)k->tag << WAKER_TAG_SHIFT) | idx;
    out_waker->generation = s->slots[idx].generation;
    waker_unlock(s);
    return ASX_OK;
}

void asx_waker_deregister(asx_waker *waker) {
    asx_kernel *k;
    asx_waker_state *s;
    asx_waker_slot *slot;

    if (waker == NULL) return;
    k = waker_kernel(waker);
    if (k == NULL) return;
    s = &k->wakers;
    waker_lock(s);
    slot = waker_slot_locked(s, waker);
    if (slot != NULL) {
        slot->alive = 0;
        slot->signaled = 0;
        if (s->active_count > 0) s->active_count--;
    }
    waker_unlock(s);
}

/* ------------------------------------------------------------------ */
/* Signaling                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_waker_wake(const asx_waker *waker) {
    asx_kernel *k;
    asx_waker_state *s;
    asx_waker_slot *slot;
    int notify;

    if (waker == NULL) return ASX_E_INVALID_ARGUMENT;
    k = waker_kernel(waker);
    if (k == NULL) return ASX_E_NOT_FOUND;
    s = &k->wakers;
    waker_lock(s);
    slot = waker_slot_locked(s, waker);
    if (slot == NULL) {
        waker_unlock(s);
        return ASX_E_NOT_FOUND;
    }
    if (!slot->signaled) {
        slot->signaled = 1;
        slot->signal_sequence = ++s->signal_sequence;
    }
    notify = s->sched_blocking;
    waker_unlock(s);

    /* The scheduler committed to blocking before this signal: interrupt
     * its reactor wait (a notify written before the wait starts still
     * makes that wait return). */
    if (notify) asx_runtime_reactor_notify();
    return ASX_OK;
}

asx_status asx_waker_clone(const asx_waker *src, asx_waker *out_clone) {
    asx_kernel *k;
    asx_waker_state *s;
    asx_status st = ASX_OK;

    if (src == NULL || out_clone == NULL) return ASX_E_INVALID_ARGUMENT;
    k = waker_kernel(src);
    if (k == NULL) return ASX_E_NOT_FOUND;
    s = &k->wakers;
    waker_lock(s);
    if (waker_slot_locked(s, src) == NULL) {
        st = ASX_E_NOT_FOUND;
    } else {
        /* Clone is the same handle (references same slot) */
        out_clone->slot = src->slot;
        out_clone->generation = src->generation;
    }
    waker_unlock(s);
    return st;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

int asx_waker_is_signaled(const asx_waker *waker) {
    asx_kernel *k;
    asx_waker_state *s;
    asx_waker_slot *slot;
    int signaled;

    if (waker == NULL) return 0;
    k = waker_kernel(waker);
    if (k == NULL) return 0;
    s = &k->wakers;
    waker_lock(s);
    slot = waker_slot_locked(s, waker);
    signaled = slot != NULL ? slot->signaled : 0;
    waker_unlock(s);
    return signaled;
}

void asx_waker_clear(asx_waker *waker) {
    asx_kernel *k;
    asx_waker_state *s;
    uint32_t idx;

    if (waker == NULL) return;
    k = waker_kernel(waker);
    if (k == NULL) return;
    s = &k->wakers;
    idx = waker->slot & WAKER_INDEX_MASK;
    waker_lock(s);
    if (idx < s->slot_count && s->slots[idx].generation == waker->generation) {
        s->slots[idx].signaled = 0;
    }
    waker_unlock(s);
}

asx_task_id asx_waker_task(const asx_waker *waker) {
    asx_kernel *k;
    asx_waker_state *s;
    asx_waker_slot *slot;
    asx_task_id task;

    if (waker == NULL) return ASX_INVALID_ID;
    k = waker_kernel(waker);
    if (k == NULL) return ASX_INVALID_ID;
    s = &k->wakers;
    waker_lock(s);
    slot = waker_slot_locked(s, waker);
    task = slot != NULL ? slot->task : ASX_INVALID_ID;
    waker_unlock(s);
    return task;
}

uint32_t asx_waker_active_count(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    asx_waker_state *s;
    uint32_t n;

    if (k == NULL) return 0u;
    s = &k->wakers;
    waker_lock(s);
    n = s->active_count;
    waker_unlock(s);
    return n;
}

/* ------------------------------------------------------------------ */
/* Drain signaled                                                      */
/* ------------------------------------------------------------------ */

uint32_t asx_waker_drain_signaled_internal(asx_kernel *k, asx_task_id *out_tasks,
                                           uint32_t max_tasks) {
    asx_waker_state *s = &k->wakers;
    uint32_t count = 0;

    if (out_tasks == NULL || max_tasks == 0) return 0;

    waker_lock(s);
    while (count < max_tasks) {
        uint32_t i;
        uint32_t best = ASX_MAX_WAKERS;
        uint64_t best_sequence = UINT64_MAX;

        for (i = 0; i < s->slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
            if (s->slots[i].alive && s->slots[i].signaled &&
                s->slots[i].signal_sequence < best_sequence) {
                best = i;
                best_sequence = s->slots[i].signal_sequence;
            }
        }

        if (best == ASX_MAX_WAKERS) break;

        out_tasks[count++] = s->slots[best].task;
        s->slots[best].signaled = 0;
        s->slots[best].signal_sequence = 0;
    }
    waker_unlock(s);
    return count;
}

uint32_t asx_waker_drain_signaled(const asx_runtime *rt, asx_task_id *out_tasks,
                                  uint32_t max_tasks) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k == NULL) return 0u;
    return asx_waker_drain_signaled_internal(k, out_tasks, max_tasks);
}

/* ------------------------------------------------------------------ */
/* Scheduler block handshake (internal)                                */
/* ------------------------------------------------------------------ */

int asx_waker_prepare_block_internal(asx_kernel *k) {
    asx_waker_state *s = &k->wakers;
    uint32_t i;
    int pending = 0;
    waker_lock(s);
    for (i = 0; i < s->slot_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (s->slots[i].alive && s->slots[i].signaled) {
            pending = 1;
            break;
        }
    }
    if (!pending) s->sched_blocking = 1;
    waker_unlock(s);
    return !pending;
}

void asx_waker_finish_block_internal(asx_kernel *k) {
    asx_waker_state *s = &k->wakers;
    waker_lock(s);
    s->sched_blocking = 0;
    waker_unlock(s);
}
