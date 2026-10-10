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

#include "wait_queue.h"
#include <asx/sync/notify.h>
#include <stddef.h>
#include <string.h>

/* Waiter record flags: how it was notified (only notify_one
 * notifications are passed on), and whether it was last polled with a
 * cancel-checking Cx. */
#define NOTIFY_KIND_MASK 0x3u
#define NOTIFY_NONE 0u
#define NOTIFY_ONE 1u
#define NOTIFY_ALL 2u
#define NOTIFY_CANCEL_AWARE 0x4u

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    asx_wait_queue waiters; /* waiter records, arrival order */
    /* notify_one notifications that found no waiter. They accumulate and
     * each later waiter consumes one (Rust sync/notify.rs
     * stored_notifications; unlike tokio, which stores at most one). */
    uint32_t stored;
} notify_slot;

static notify_slot g_slots[ASX_NOTIFY_MAX];
static uint32_t g_slot_count;

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
static uint32_t notify_first_in_line(const notify_slot *s) {
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
static int notify_deliver_one(notify_slot *s) {
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
static void notify_waiter_abandon(notify_slot *s, uint32_t i) {
    int pass_on = notify_kind(notify_node(i)) == NOTIFY_ONE;
    asx_wait_record_release(&s->waiters, i);
    if (pass_on) (void)notify_deliver_one(s);
}

/* Reclaim waiters whose parked task completed without cancelling. */
static void notify_reap(notify_slot *s) {
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
    notify_reap((notify_slot *)(void *)((char *)q - offsetof(notify_slot, waiters)));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_notify_create(asx_notify_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    for (i = 0; i < ASX_NOTIFY_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].stored = 0;
            asx_wait_records_init(&g_slots[i].waiters, notify_reap_queue);
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_notify_close(asx_notify_handle handle) {
    notify_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
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
    notify_slot *s;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* FIFO: notify the oldest waiter (reclaiming dead ones first). With no
     * waiter the notification is stored for the next one, so a notify that
     * races ahead of a wait is never lost. */
    notify_reap(s);
    (void)notify_deliver_one(s);
    return ASX_OK;
}

asx_status asx_notify_all(asx_notify_handle handle) {
    notify_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
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
    notify_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
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
    notify_slot *s;
    asx_wait_node *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->notify_slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->notify_slot];

    /* Check if notify was closed */
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;

    w = asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation);
    if (w == NULL) return ASX_E_INVALID_STATE;

    /* Cx cancellation/budget checkpoint */
    if (cx != NULL && asx_cx_has_cap(cx, ASX_CAP_CANCEL_CHECK)) {
        w->flags |= NOTIFY_CANCEL_AWARE;
    } else {
        w->flags &= (uint8_t)~NOTIFY_CANCEL_AWARE;
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
    notify_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->notify_slot >= ASX_NOTIFY_MAX) return ASX_E_INVALID_ARGUMENT;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->notify_slot];
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
    if (handle.slot >= ASX_NOTIFY_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return asx_wait_queue_len(&g_slots[handle.slot].waiters);
}

uint32_t asx_notify_stored_count(asx_notify_handle handle) {
    if (handle.slot >= ASX_NOTIFY_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return g_slots[handle.slot].stored;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_notify_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].stored = 0;
        asx_wait_records_init(&g_slots[i].waiters, notify_reap_queue);
    }
    g_slot_count = 0;
}
