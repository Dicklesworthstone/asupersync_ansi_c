/*
 * oneshot.c — single-value, single-use channel
 *
 * Walking skeleton: fixed-size arena, single-threaded.
 *
 * Wake-driven waiting: try_recv reporting ASX_E_WOULD_BLOCK inside a
 * scheduler poll parks the calling task; the send, a sender drop, or a
 * receiver drop wakes every parked task so it observes the final state.
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

/* Parked receiver tasks per oneshot (one receiver handle; slack for tasks
 * sharing it). */
#define ONESHOT_MAX_WAITERS 4u

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_oneshot_state state;
    uint16_t generation;
    uint64_t value;
    int sender_alive;
    int receiver_alive;
    asx_task_id wait_slots[ONESHOT_MAX_WAITERS];
    asx_wait_queue waiters; /* tasks parked in try_recv */
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
        asx_wait_queue_init(&g_slots[i].waiters, g_slots[i].wait_slots, ONESHOT_MAX_WAITERS);
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
    asx_wait_queue_init(&s->waiters, s->wait_slots, ONESHOT_MAX_WAITERS);

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
    if (!s->sender_alive) return;

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

asx_status asx_oneshot_try_send(asx_oneshot_sender *sender, uint64_t value) {
    asx_oneshot_slot *s;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return ASX_E_STALE_HANDLE;
    if (!s->sender_alive) return ASX_E_INVALID_STATE;

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

asx_status asx_oneshot_try_recv(asx_oneshot_receiver *receiver, uint64_t *out_value) {
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

    (void)asx_wait_queue_park_current(&s->waiters);
    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Cx-aware send / receive                                             */
/* ------------------------------------------------------------------ */

static void oneshot_trace(const asx_cx *cx, const char *message) {
    if (cx != NULL) asx_trace_user(cx->task_id, message);
}

asx_status asx_oneshot_send(asx_oneshot_sender *sender, asx_cx *cx, uint64_t value) {
    asx_oneshot_slot *s;
    asx_obligation_id ob = ASX_INVALID_ID;
    asx_status st;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;
    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return ASX_E_STALE_HANDLE;
    if (!s->sender_alive) return ASX_E_INVALID_STATE;

    /* Rust's reserve: cancellation consumes the sender, closing the
     * channel (oneshot.rs:497-510). */
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        oneshot_trace(cx, "oneshot::reserve cancelled");
        asx_oneshot_sender_drop(sender);
        return ASX_E_CANCELLED;
    }
    oneshot_trace(cx, "oneshot::reserve creating permit");
    if (cx != NULL && cx->task_id != ASX_INVALID_ID &&
        asx_obligation_reserve_ex(cx->region_id, ASX_OBLIGATION_KIND_SEND_PERMIT, cx->task_id,
                                  &ob) != ASX_OK) {
        ob = ASX_INVALID_ID;
    }

    /* The permit's send: delivered commits, a dropped receiver aborts
     * with reason Error (:731-772). */
    st = asx_oneshot_try_send(sender, value);
    if (ob != ASX_INVALID_ID) {
        asx_status rs = st == ASX_OK
                            ? asx_obligation_commit(ob)
                            : asx_obligation_abort_with_reason(ob, ASX_OBLIGATION_ABORT_ERROR);
        (void)rs; /* the obligation was reserved just above */
    }
    return st;
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
    st = asx_oneshot_try_recv(receiver, out_value);
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
