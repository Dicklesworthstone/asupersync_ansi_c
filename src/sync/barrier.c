/*
 * barrier.c — N-way rendezvous with leader election
 *
 * Waiters are records in the shared wait-node pool (wait_queue.h), linked
 * in arrival order, so a barrier has no waiter limit of its own and any
 * party count can trip it.
 *
 * A waiter arrives at its first poll, after the Cx checkpoint that every
 * poll starts with, as Rust's BarrierWaitFuture does (sync/barrier.rs:
 * 300-312): a task already cancelled or out of budget never arrives.
 *
 * Wake-driven waiting: poll_wait returning ASX_E_PENDING inside a
 * scheduler poll records the calling task on the waiter and parks it. The
 * arrival that trips the barrier wakes every parked waiter (in arrival
 * order); close wakes them all so they observe ASX_E_DISCONNECTED.
 * A parked waiter whose task completed without cancelling is reclaimed
 * lazily and its arrival withdrawn, exactly like wait_cancel.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/sync/barrier.h>
#include <stddef.h>
#include <string.h>

/* Waiter record flags. */
#define BARRIER_RELEASED 0x1u /* its round has tripped */
#define BARRIER_LEADER 0x2u   /* the arrival that tripped it */
#define BARRIER_ARRIVED 0x4u  /* counted in a round (from its first poll) */

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    uint32_t threshold;     /* N tasks required */
    uint32_t arrived;       /* tasks currently waiting */
    int tripped;            /* barrier released */
    asx_wait_queue waiters; /* waiter records, arrival order */
} barrier_slot;

static barrier_slot g_slots[ASX_BARRIER_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static asx_wait_node *barrier_node(uint32_t i) { return asx_wait_node_at(i); }

/* Withdraw a waiter (wait_cancel semantics). Only an arrival that has not
 * been released still counts toward the current round; a released waiter
 * belongs to a round that already tripped and reset `arrived`. */
static void barrier_waiter_withdraw(barrier_slot *s, uint32_t i) {
    if ((barrier_node(i)->flags & (BARRIER_ARRIVED | BARRIER_RELEASED)) == BARRIER_ARRIVED &&
        s->arrived > 0) {
        s->arrived--;
    }
    asx_wait_record_release(&s->waiters, i);
}

/* Reclaim parked waiters whose task completed without cancelling. */
static void barrier_reap(barrier_slot *s) {
    uint32_t i = asx_wait_queue_first(&s->waiters);
    while (i != ASX_WAIT_NIL) {
        uint32_t next = asx_wait_queue_next(i);
        const asx_wait_node *w = barrier_node(i);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if (asx_handle_is_valid(w->task) && asx_wait_task_liveness(w->task) == ASX_WAIT_TASK_DEAD) {
            barrier_waiter_withdraw(s, i);
        }
        i = next;
    }
}

/* Wait-node pool reclamation: withdraw this barrier's dead waiters. */
static void barrier_reap_queue(asx_wait_queue *q) {
    barrier_reap((barrier_slot *)(void *)((char *)q - offsetof(barrier_slot, waiters)));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_barrier_create(uint32_t count, asx_barrier_handle *out) {
    uint32_t i;
    if (out == NULL || count == 0) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_BARRIER_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].threshold = count;
            g_slots[i].arrived = 0;
            g_slots[i].tripped = 0;
            asx_wait_records_init(&g_slots[i].waiters, barrier_reap_queue);
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_barrier_close(asx_barrier_handle handle) {
    barrier_slot *s;
    uint32_t i;
    if (handle.slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* Wake all waiters as disconnected */
    for (i = asx_wait_queue_first(&s->waiters); i != ASX_WAIT_NIL; i = asx_wait_queue_next(i)) {
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        asx_wait_wake_task(barrier_node(i)->task);
    }
    asx_wait_queue_clear(&s->waiters);

    s->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Wait                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_barrier_wait_begin(asx_barrier_handle handle, asx_barrier_waiter *out) {
    barrier_slot *s;
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* Arrivals of tasks that died while parked no longer count. */
    barrier_reap(s);

    i = asx_wait_record_add(&s->waiters);
    if (i == ASX_WAIT_NIL) return ASX_E_RESOURCE_EXHAUSTED;
    out->barrier_slot = handle.slot;
    out->waiter_slot = i;
    out->generation = handle.generation;
    out->waiter_generation = barrier_node(i)->generation;
    out->is_leader = 0;
    return ASX_OK;
}

/* Waiter i arrives. The arrival that completes the round trips the
 * barrier: it leads, every other arrival of the round is released and
 * woken, and the next arrival starts a new round (Rust resets arrived and
 * advances the generation on trip). Returns 1 if i tripped it. */
static int barrier_arrive(barrier_slot *s, uint32_t i) {
    uint32_t j;
    barrier_node(i)->flags |= BARRIER_ARRIVED;
    s->arrived++;
    if (s->arrived < s->threshold) return 0;
    s->tripped = 1;
    barrier_node(i)->flags |= BARRIER_RELEASED | BARRIER_LEADER;
    for (j = asx_wait_queue_first(&s->waiters); j != ASX_WAIT_NIL; j = asx_wait_queue_next(j)) {
        asx_wait_node *w = barrier_node(j);
        ASX_CHECKPOINT_WAIVER("bounded: waiters <= ASX_WAIT_NODE_CAPACITY");
        if ((w->flags & (BARRIER_ARRIVED | BARRIER_RELEASED)) != BARRIER_ARRIVED) continue;
        w->flags |= BARRIER_RELEASED;
        asx_wait_wake_task(w->task);
    }
    s->arrived = 0;
    return 1;
}

asx_status asx_barrier_poll_wait(asx_barrier_waiter *waiter, asx_cx *cx) {
    barrier_slot *s;
    asx_wait_node *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->barrier_slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;

    s = &g_slots[waiter->barrier_slot];
    if (!s->alive || s->generation != waiter->generation) return ASX_E_DISCONNECTED;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    w = asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation);
    if (w == NULL) return ASX_E_INVALID_STATE;

    /* Every poll checkpoints first (sync/barrier.rs:304). A cancelled
     * waiter withdraws its arrival, if it made one; but release wins the
     * race: a waiter whose round already tripped completes successfully,
     * never as leader, though its Cx stays cancelled (finish_cancelled). */
    if (cx != NULL) {
        asx_status cst = asx_cx_checkpoint(cx);
        if (cst != ASX_OK) {
            if ((w->flags & BARRIER_RELEASED) != 0u) {
                waiter->is_leader = 0;
                asx_wait_record_release(&s->waiters, waiter->waiter_slot);
                return ASX_OK;
            }
            barrier_waiter_withdraw(s, waiter->waiter_slot);
            return cst;
        }
    }

    /* The first poll arrives; the arrival that completes N leads. */
    if ((w->flags & BARRIER_ARRIVED) == 0u && barrier_arrive(s, waiter->waiter_slot)) {
        waiter->is_leader = 1;
        asx_wait_record_release(&s->waiters, waiter->waiter_slot);
        return ASX_OK;
    }

    if ((w->flags & BARRIER_RELEASED) != 0u) {
        waiter->is_leader = 0;
        asx_wait_record_release(&s->waiters, waiter->waiter_slot);
        return ASX_OK;
    }

    /* Inside a scheduler poll, park until the barrier trips (or closes). */
    (void)asx_wait_park_current(&w->task);
    return ASX_E_PENDING;
}

asx_status asx_barrier_wait_cancel(asx_barrier_waiter *waiter) {
    barrier_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    if (waiter->barrier_slot >= ASX_BARRIER_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[waiter->barrier_slot];
    if (!s->alive || s->generation != waiter->generation) return ASX_OK;
    if (waiter->waiter_slot >= ASX_WAIT_NODE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    if (asx_wait_record_get(&s->waiters, waiter->waiter_slot, waiter->waiter_generation) != NULL) {
        barrier_waiter_withdraw(s, waiter->waiter_slot);
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_barrier_waiting_count(asx_barrier_handle handle) {
    if (handle.slot >= ASX_BARRIER_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return g_slots[handle.slot].arrived;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_barrier_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].arrived = 0;
        g_slots[i].tripped = 0;
        asx_wait_records_init(&g_slots[i].waiters, barrier_reap_queue);
    }
    g_slot_count = 0;
}
