/*
 * notify.c — event signaling primitive
 *
 * Waiters are records in the shared wait-node pool (wait_queue.h), linked
 * in arrival order, so a notify has no waiter limit of its own.
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

#include "../runtime/runtime_internal.h"
#include "wait_queue.h"
#include <asx/sync/notify.h>
#include <stddef.h>
#include <string.h>

/* Waiter record flags: how it was notified (only notify_one
 * notifications are passed on), and whether it was last polled with a
 * Cx (which checks the task's cancel). */
#define NOTIFY_KIND_MASK 0x3u
#define NOTIFY_NONE 0u
#define NOTIFY_ONE 1u
#define NOTIFY_ALL 2u
#define NOTIFY_CANCEL_AWARE 0x4u

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/* The arenas are the kernels' (asx_kernel.notifies, sync_state.h): the
 * slot field of a handle or waiter holds the index in its low half and the
 * tag of the runtime that issued it above. NULL when no runtime holds the
 * tag or the index is out of range. */
static asx_notify_slot *notify_slot_at(uint32_t field) {
    asx_kernel *k = asx_kernel_for_tag((uint16_t)(field >> ASX_SYNC_SLOT_TAG_SHIFT));
    uint32_t idx = field & ASX_SYNC_SLOT_INDEX_MASK;
    if (k == NULL || idx >= ASX_NOTIFY_MAX) return NULL;
    return &k->notifies.slots[idx];
}

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Waiter line                                                         */
/* ------------------------------------------------------------------ */

static asx_wait_node *notify_node(uint32_t i) { return asx_wait_node_at(i); }

static uint32_t notify_kind(const asx_wait_node *w) { return w->flags & NOTIFY_KIND_MASK; }

static void notify_set_kind(asx_wait_node *w, uint32_t kind) {
    w->flags = (uint8_t)((w->flags & ~NOTIFY_KIND_MASK) | kind);
}

/* Oldest un-notified waiter that will not give up: a cancel-aware waiter
 * (polled with a cancel-checking Cx) whose task is cancel-pending abandons
 * on its next poll, so it is skipped. A waiter polled without a Cx, like
 * Rust's Notified (sync/notify.rs:347), waits on through a cancel and is
 * notified like any other. */
static uint32_t notify_first_in_line(const asx_notify_slot *s) {
    uint32_t i;
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        const asx_wait_node *w = notify_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (notify_kind(w) != NOTIFY_NONE) continue;
        if ((w->flags & NOTIFY_CANCEL_AWARE) != 0u && asx_handle_is_valid(w->task) &&
            asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DOOMED) {
            continue;
        }
        return i;
    }
    return ASX_WAIT_NIL;
}

/* Deliver one notification to the next waiter in line, or store it when
 * no waiter can take it. Returns 1 if a waiter took it. */
static int notify_deliver_one(asx_notify_slot *s) {
    uint32_t i = notify_first_in_line(s);
    if (i == ASX_WAIT_NIL) {
        if (s->stored < UINT32_MAX) s->stored++;
        return 0;
    }
    notify_set_kind(notify_node(i), NOTIFY_ONE);
    asx_wait_wake_task(notify_node(i)->task);
    return 1;
}

/* Release a waiter that leaves without consuming its notification. A
 * notify_one notification it held is passed on (or stored again). */
static void notify_waiter_abandon(asx_notify_slot *s, uint32_t i) {
    int pass_on = notify_kind(notify_node(i)) == NOTIFY_ONE;
    asx_wait_record_release(&s->waiters, i);
    if (pass_on) (void)notify_deliver_one(s);
}

/* Reclaim waiters whose parked task completed without cancelling. */
static void notify_reap(asx_notify_slot *s) {
    uint32_t i = asx_wait_queue_first(&s->waiters);
    while (i != ASX_WAIT_NIL) {
        uint32_t next = asx_wait_queue_next(i);
        const asx_wait_node *w = notify_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) {
            notify_waiter_abandon(s, i);
        }
        i = next;
    }
}

/* Wait-node pool reclamation: retire this notify's dead waiters. */
static void notify_reap_queue(asx_wait_queue *q) {
    notify_reap((asx_notify_slot *)((char *)q - offsetof(asx_notify_slot, waiters)));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Whether any notify was created since the last reset. */
asx_status asx_notify_create(const asx_runtime *rt, asx_notify_handle *out) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    uint32_t i;
    if (out == NULL || rt == NULL) return ASX_E_INVALID_ARGUMENT;
    if (k == NULL) return ASX_E_INVALID_STATE;

    /* Find free slot */
    for (i = 0; i < ASX_NOTIFY_MAX; i++) {
        asx_notify_slot *s = &k->notifies.slots[i];
        if (!s->alive) {
            s->alive = 1;
            s->generation = next_gen(s->generation);
            s->stored = 0;
            asx_wait_records_init(&s->waiters, notify_reap_queue, k->tag);
            out->slot = ((uint32_t)k->tag << ASX_SYNC_SLOT_TAG_SHIFT) | i;
            out->generation = s->generation;
            if (i >= k->notifies.slot_count) k->notifies.slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_notify_close(asx_notify_handle handle) {
    asx_notify_slot *s;
    uint32_t i;
    s = notify_slot_at(handle.slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        asx_wait_wake_task(notify_node(i)->task);
    }
    asx_wait_queue_clear(&s->waiters);

    s->alive = 0;
    s->stored = 0; /* no consumer can arrive after close */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Signal                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_notify_one(asx_notify_handle handle) {
    asx_notify_slot *s;
    s = notify_slot_at(handle.slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* FIFO: notify the oldest waiter (reclaiming dead ones first). With no
     * waiter the notification is stored for the next one, so a notify that
     * races ahead of a wait is never lost. */
    notify_reap(s);
    (void)notify_deliver_one(s);
    return ASX_OK;
}

asx_status asx_notify_all(asx_notify_handle handle) {
    asx_notify_slot *s;
    uint32_t i;
    s = notify_slot_at(handle.slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        asx_wait_node *w = notify_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (notify_kind(w) == NOTIFY_NONE) notify_set_kind(w, NOTIFY_ALL);
        asx_wait_wake_task(w->task);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Wait                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_notify_wait_begin(asx_notify_handle handle, asx_notify_waiter *out) {
    asx_notify_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = notify_slot_at(handle.slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    i = asx_wait_record_add(&s->waiters);
    if (i == ASX_WAIT_NIL) return ASX_E_RESOURCE_EXHAUSTED;

    /* Claim a stored notification. It behaves like a notify_one delivery,
     * so a waiter that gives up passes it on (or stores it again). */
    if (s->stored > 0) {
        s->stored--;
        notify_set_kind(notify_node(i), NOTIFY_ONE);
    }
    out->notify_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    out->waiter_generation = notify_node(i)->generation;
    return ASX_OK;
}

asx_status asx_notify_poll_wait(asx_notify_waiter *waiter, asx_cx *cx) {
    asx_notify_slot *s;
    asx_wait_node *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    s = notify_slot_at(waiter->notify_slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    /* Check if notify was closed */
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;

    w = asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation);
    if (w == NULL) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL) {
        w->flags |= NOTIFY_CANCEL_AWARE;
    } else {
        w->flags = (uint8_t)(w->flags & ~NOTIFY_CANCEL_AWARE);
    }
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            /* Give up: a notify_one notification passes to the next waiter. */
            notify_waiter_abandon(s, waiter->waiter_slot);
            return cst;
        }
    }

    if (notify_kind(w) != NOTIFY_NONE) {
        asx_wait_record_release(&s->waiters, waiter->waiter_slot);
        return ASX_OK;
    }

    /* Inside a scheduler poll, park until notified (or closed). */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

asx_status asx_notify_wait_cancel(asx_notify_waiter *waiter) {
    asx_notify_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    s = notify_slot_at(waiter->notify_slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != waiter->generation)
        return ASX_OK; /* already closed, nothing to cancel */

    if (asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation) != NULL) {
        notify_waiter_abandon(s, waiter->waiter_slot);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_notify_waiter_count(asx_notify_handle handle) {
    const asx_notify_slot *s = notify_slot_at(handle.slot);
    if (s == NULL) return 0;
    if (!s->alive || s->generation != handle.generation) return 0;
    return asx_wait_queue_len(&s->waiters);
}

uint32_t asx_notify_stored_count(asx_notify_handle handle) {
    const asx_notify_slot *s = notify_slot_at(handle.slot);
    if (s == NULL) return 0;
    if (!s->alive || s->generation != handle.generation) return 0;
    return s->stored;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_notify_reset_internal(asx_kernel *k) {
    asx_notify_state *st = &k->notifies;
    uint32_t i;
    for (i = 0; i < st->slot_count; i++) {
        st->slots[i].generation = next_gen(st->slots[i].generation);
        st->slots[i].alive = 0;
        st->slots[i].stored = 0;
        asx_wait_records_init(&st->slots[i].waiters, notify_reap_queue, k->tag);
    }
    st->slot_count = 0;
}

void asx_notify_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_notify_reset_internal(k);
}
