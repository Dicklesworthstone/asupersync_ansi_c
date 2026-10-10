/*
 * oneshot.c — single-value, single-use channel
 *
 * Walking skeleton: fixed-size arena, single-threaded.
 *
 * Wake-driven waiting: asx_oneshot_recv finding no value inside a
 * scheduler poll parks the calling task; the send, a sender drop, or a
 * receiver drop wakes every parked task so it observes the final state.
 * try_recv never parks (bd-vc1v).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../sync/wait_queue.h"
#include <asx/core/oneshot.h>
#include <asx/runtime/runtime.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_oneshot_state state;
    uint16_t generation;
    uint64_t value;
    int sender_alive;
    int receiver_alive;
    int reserved;           /* a permit holds the sending side (asx_oneshot_reserve) */
    asx_wait_queue waiters; /* tasks parked in asx_oneshot_recv */
} asx_oneshot_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_oneshot_slot g_slots[ASX_MAX_ONESHOTS];
static uint32_t g_slot_count = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* Final state reached (value sent, a side dropped): every parked task
 * re-polls and observes it. */
static void oneshot_wake_waiters(asx_oneshot_slot *s) {
    if (s->waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->waiters);
}

void asx_oneshot_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_ONESHOTS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].state = ASX_ONESHOT_EMPTY;
        g_slots[i].value = 0;
        g_slots[i].sender_alive = 0;
        g_slots[i].receiver_alive = 0;
        g_slots[i].reserved = 0;
        asx_wait_queue_init(&g_slots[i].waiters, NULL);
    }
    g_slot_count = 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_oneshot_create(asx_oneshot_sender *out_sender, asx_oneshot_receiver *out_receiver) {
    uint32_t idx;
    asx_oneshot_slot *s;

    if (out_sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    idx = ASX_MAX_ONESHOTS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
            if (!g_slots[i].sender_alive && !g_slots[i].receiver_alive &&
                g_slots[i].state != ASX_ONESHOT_EMPTY) {
                /* Recycled slot: bump generation */
                g_slots[i].generation = next_gen(g_slots[i].generation);
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_ONESHOTS) {
        if (g_slot_count >= ASX_MAX_ONESHOTS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->state = ASX_ONESHOT_EMPTY;
    s->generation = next_gen(s->generation);
    s->value = 0;
    s->sender_alive = 1;
    s->receiver_alive = 1;
    s->reserved = 0;
    asx_wait_queue_init(&s->waiters, NULL);

    out_sender->slot = idx;
    out_sender->generation = s->generation;
    out_receiver->slot = idx;
    out_receiver->generation = s->generation;

    return ASX_OK;
}

void asx_oneshot_sender_drop(asx_oneshot_sender *sender) {
    asx_oneshot_slot *s;
    if (sender == NULL) return;
    if (sender->slot >= g_slot_count) return;
    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return;
    /* A reserved sender was consumed: its permit owns the sending side. */
    if (!s->sender_alive || s->reserved) return;

    s->sender_alive = 0;
    if (s->state == ASX_ONESHOT_EMPTY) { s->state = ASX_ONESHOT_SENDER_DROPPED; }
    oneshot_wake_waiters(s);
}

void asx_oneshot_receiver_drop(asx_oneshot_receiver *receiver) {
    asx_oneshot_slot *s;
    if (receiver == NULL) return;
    if (receiver->slot >= g_slot_count) return;
    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return;
    if (!s->receiver_alive) return;

    s->receiver_alive = 0;
    if (s->state == ASX_ONESHOT_EMPTY || s->state == ASX_ONESHOT_FILLED) {
        s->state = ASX_ONESHOT_RECEIVER_DROPPED;
    }
    oneshot_wake_waiters(s);
}

/* ------------------------------------------------------------------ */
/* Send / Receive                                                      */
/* ------------------------------------------------------------------ */

/* The send itself, for a live sender or the permit holding it. */
static asx_status oneshot_deliver(const asx_oneshot_sender *sender, asx_oneshot_slot *s,
                                  uint64_t value) {
    if (!s->receiver_alive) {
        s->sender_alive = 0;
        return ASX_E_DISCONNECTED;
    }

    if (s->state != ASX_ONESHOT_EMPTY) return ASX_E_INVALID_STATE;

    s->value = value;
    s->state = ASX_ONESHOT_FILLED;
    s->sender_alive = 0;

    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)sender->slot, value);
    oneshot_wake_waiters(s);

    return ASX_OK;
}

/* Look up a sender's slot. */
static asx_status oneshot_sender_slot(const asx_oneshot_sender *sender, asx_oneshot_slot **out) {
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;
    *out = &g_slots[sender->slot];
    if ((*out)->generation != sender->generation) return ASX_E_STALE_HANDLE;
    return ASX_OK;
}

asx_status asx_oneshot_try_send(asx_oneshot_sender *sender, uint64_t value) {
    asx_oneshot_slot *s;
    asx_status st;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    st = oneshot_sender_slot(sender, &s);
    if (st != ASX_OK) return st;
    if (!s->sender_alive || s->reserved) return ASX_E_INVALID_STATE;
    return oneshot_deliver(sender, s, value);
}

/* Receive the value. Empty: a waiting receive (asx_oneshot_recv) parks the
 * calling task until a send or a drop; try_recv never parks (Rust
 * try_recv registers no waker). */
static asx_status oneshot_recv_impl(asx_oneshot_receiver *receiver, uint64_t *out_value, int park) {
    asx_oneshot_slot *s;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return ASX_E_STALE_HANDLE;
    if (!s->receiver_alive) {
        asx_wait_queue_leave_current(&s->waiters);
        return ASX_E_INVALID_STATE;
    }

    if (s->state == ASX_ONESHOT_FILLED) {
        *out_value = s->value;
        s->state = ASX_ONESHOT_CONSUMED;
        s->receiver_alive = 0;
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)receiver->slot, s->value);
        asx_wait_queue_leave_current(&s->waiters);
        /* The receiver is spent: any other task parked on it must see that. */
        oneshot_wake_waiters(s);
        return ASX_OK;
    }

    if (s->state == ASX_ONESHOT_SENDER_DROPPED || !s->sender_alive) {
        s->receiver_alive = 0;
        asx_wait_queue_leave_current(&s->waiters);
        oneshot_wake_waiters(s);
        return ASX_E_DISCONNECTED;
    }

    if (park) (void)asx_wait_queue_park_current(&s->waiters);
    return ASX_E_WOULD_BLOCK;
}

asx_status asx_oneshot_try_recv(asx_oneshot_receiver *receiver, uint64_t *out_value) {
    return oneshot_recv_impl(receiver, out_value, 0);
}

/* ------------------------------------------------------------------ */
/* Cx-aware send / receive                                             */
/* ------------------------------------------------------------------ */

static void oneshot_trace(const asx_cx *cx, const char *message) {
    if (cx != NULL) asx_trace_user(cx->task_id, message);
}

asx_status asx_oneshot_reserve(asx_oneshot_sender *sender, asx_cx *cx, asx_oneshot_permit *out) {
    asx_oneshot_slot *s;
    asx_status st;

    if (sender == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = oneshot_sender_slot(sender, &s);
    if (st != ASX_OK) return st;
    /* A consumed sender leaves *out alone: it may be the live permit. */
    if (!s->sender_alive || s->reserved) return ASX_E_INVALID_STATE;
    out->live = 0;
    out->obligation = ASX_INVALID_ID;

    /* Cancellation consumes the sender, closing the channel
     * (oneshot.rs:497-510). */
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        oneshot_trace(cx, "oneshot::reserve cancelled");
        asx_oneshot_sender_drop(sender);
        return ASX_E_CANCELLED;
    }
    oneshot_trace(cx, "oneshot::reserve creating permit");
    s->reserved = 1;
    out->sender = *sender;
    out->live = 1;
    /* Registered after the permit exists (:519-528); a refusal leaves the
     * permit untracked. */
    if (cx != NULL && cx->task_id != ASX_INVALID_ID) {
        asx_obligation_id ob;
        if (asx_obligation_reserve_ex(cx->region_id, ASX_OBLIGATION_KIND_SEND_PERMIT, cx->task_id,
                                      &ob) == ASX_OK) {
            out->obligation = ob;
        }
    }
    return ASX_OK;
}

/* Resolve a permit's obligation and release the slot's reservation. */
static asx_oneshot_slot *oneshot_permit_take(asx_oneshot_permit *permit) {
    asx_oneshot_slot *s;
    permit->live = 0;
    if (oneshot_sender_slot(&permit->sender, &s) != ASX_OK) return NULL;
    s->reserved = 0;
    return s;
}

static void oneshot_permit_resolve(asx_oneshot_permit *permit, int delivered,
                                   asx_obligation_abort_reason reason) {
    asx_status st;
    if (permit->obligation == ASX_INVALID_ID) return;
    st = delivered ? asx_obligation_commit(permit->obligation)
                   : asx_obligation_abort_with_reason(permit->obligation, reason);
    (void)st; /* refused only for an obligation the runtime already resolved */
    permit->obligation = ASX_INVALID_ID;
}

asx_status asx_oneshot_permit_send(asx_oneshot_permit *permit, uint64_t value) {
    asx_oneshot_slot *s;
    asx_status st;

    if (permit == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!permit->live) return ASX_E_INVALID_STATE;
    s = oneshot_permit_take(permit);
    st = s != NULL ? oneshot_deliver(&permit->sender, s, value) : ASX_E_STALE_HANDLE;
    /* Delivered commits; a dropped receiver aborts with reason Error
     * (:731-772). */
    oneshot_permit_resolve(permit, st == ASX_OK, ASX_OBLIGATION_ABORT_ERROR);
    return st;
}

void asx_oneshot_permit_abort(asx_oneshot_permit *permit) {
    if (permit == NULL || !permit->live) return;
    /* The channel closes: the receiver sees it closed (:774-799). */
    if (oneshot_permit_take(permit) != NULL) asx_oneshot_sender_drop(&permit->sender);
    oneshot_permit_resolve(permit, 0, ASX_OBLIGATION_ABORT_EXPLICIT);
}

asx_status asx_oneshot_send(asx_oneshot_sender *sender, asx_cx *cx, uint64_t value) {
    asx_oneshot_permit permit;
    asx_status st = asx_oneshot_reserve(sender, cx, &permit);
    if (st != ASX_OK) return st;
    return asx_oneshot_permit_send(&permit, value);
}

asx_status asx_oneshot_recv(asx_oneshot_receiver *receiver, asx_cx *cx, uint64_t *out_value) {
    asx_oneshot_slot *s;
    asx_status st;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;
    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return ASX_E_STALE_HANDLE;

    /* A value, then a closed channel, take precedence over cancellation
     * (oneshot.rs:1088-1116). */
    if (s->receiver_alive && (s->state == ASX_ONESHOT_FILLED ||
                              s->state == ASX_ONESHOT_SENDER_DROPPED || !s->sender_alive)) {
        st = asx_oneshot_try_recv(receiver, out_value);
        oneshot_trace(cx, st == ASX_OK ? "oneshot::recv received value"
                                       : "oneshot::recv channel closed");
        return st;
    }
    if (s->receiver_alive && cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        asx_wait_queue_leave_current(&s->waiters);
        oneshot_trace(cx, "oneshot::recv cancelled while waiting");
        return ASX_E_CANCELLED;
    }
    st = oneshot_recv_impl(receiver, out_value, 1);
    return st == ASX_E_WOULD_BLOCK ? ASX_E_PENDING : st;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

asx_oneshot_state asx_oneshot_get_state(uint32_t slot, uint16_t generation) {
    if (slot >= g_slot_count) return ASX_ONESHOT_EMPTY;
    if (g_slots[slot].generation != generation) return ASX_ONESHOT_EMPTY;
    return g_slots[slot].state;
}
