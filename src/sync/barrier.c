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

#include "../runtime/runtime_internal.h"
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

/* The arenas are the kernels' (asx_kernel.barriers, sync_state.h): the
 * slot field of a handle or waiter holds the index in its low half and the
 * tag of the runtime that issued it above. NULL when no runtime holds the
 * tag or the index is out of range. */
static asx_barrier_slot *barrier_slot_at(uint32_t field) {
    asx_kernel *k = asx_kernel_for_tag((uint16_t)(field >> ASX_SYNC_SLOT_TAG_SHIFT));
    uint32_t idx = field & ASX_SYNC_SLOT_INDEX_MASK;
    if (k == NULL || idx >= ASX_BARRIER_MAX) return NULL;
    return &k->barriers.slots[idx];
}

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static asx_wait_node *barrier_node(uint32_t i) { return asx_wait_node_at(i); }

/* Withdraw a waiter (wait_cancel semantics). Only an arrival that has not
 * been released still counts toward the current round; a released waiter
 * belongs to a round that already tripped and reset `arrived`. */
static void barrier_waiter_withdraw(asx_barrier_slot *s, uint32_t i) {
    if ((barrier_node(i)->flags & (BARRIER_ARRIVED | BARRIER_RELEASED)) == BARRIER_ARRIVED &&
        s->arrived > 0) {
        s->arrived--;
    }
    asx_wait_record_release(&s->waiters, i);
}

/* Reclaim parked waiters whose task completed without cancelling. */
static void barrier_reap(asx_barrier_slot *s) {
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
    barrier_reap((asx_barrier_slot *)((char *)q - offsetof(asx_barrier_slot, waiters)));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_barrier_create(const asx_runtime *rt, uint32_t count, asx_barrier_handle *out) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    uint32_t i;
    if (out == NULL || rt == NULL || count == 0) return ASX_E_INVALID_ARGUMENT;
    if (k == NULL) return ASX_E_INVALID_STATE;

    for (i = 0; i < ASX_BARRIER_MAX; i++) {
        asx_barrier_slot *s = &k->barriers.slots[i];
        if (!s->alive) {
            s->alive = 1;
            s->generation = next_gen(s->generation);
            s->threshold = count;
            s->arrived = 0;
            s->tripped = 0;
            asx_wait_records_init(&s->waiters, barrier_reap_queue, k->tag);
            out->slot = ((uint32_t)k->tag << ASX_SYNC_SLOT_TAG_SHIFT) | i;
            out->generation = s->generation;
            if (i >= k->barriers.slot_count) k->barriers.slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_barrier_close(asx_barrier_handle handle) {
    asx_barrier_slot *s = barrier_slot_at(handle.slot);
    uint32_t i;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
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
    asx_barrier_slot *s = barrier_slot_at(handle.slot);
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
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
static int barrier_arrive(asx_barrier_slot *s, uint32_t i) {
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
    asx_barrier_slot *s;
    asx_wait_node *w;

    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    s = barrier_slot_at(waiter->barrier_slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
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
    asx_barrier_slot *s;
    if (waiter == NULL) return ASX_E_INVALID_ARGUMENT;
    s = barrier_slot_at(waiter->barrier_slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
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
    const asx_barrier_slot *s = barrier_slot_at(handle.slot);
    if (s == NULL) return 0;
    if (!s->alive || s->generation != handle.generation) return 0;
    return s->arrived;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_barrier_reset_internal(asx_kernel *k) {
    asx_barrier_state *st = &k->barriers;
    uint32_t i;
    for (i = 0; i < st->slot_count; i++) {
        st->slots[i].generation = next_gen(st->slots[i].generation);
        st->slots[i].alive = 0;
        st->slots[i].arrived = 0;
        st->slots[i].tripped = 0;
        asx_wait_records_init(&st->slots[i].waiters, barrier_reap_queue, k->tag);
    }
    st->slot_count = 0;
}

void asx_barrier_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_barrier_reset_internal(k);
}
