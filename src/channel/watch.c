/*
 * watch.c — single-value observable state channel
 *
 * Walking skeleton: fixed-size arena, single-threaded.
 *
 * Wake-driven waiting: asx_watch_poll_changed() reporting ASX_E_PENDING
 * inside a scheduler poll parks the calling task; every publish and the
 * sender drop wake all parked tasks (FIFO), since every receiver observes
 * every new version.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../sync/wait_queue.h"
#include <asx/core/watch.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int sender_alive;
    uint64_t value;
    uint64_t version; /* incremented on each send; 0 = initial value */
    uint32_t receiver_count;
    asx_task_id wait_slots[ASX_WATCH_MAX_RECEIVERS];
    asx_wait_queue waiters; /* tasks parked in poll_changed */
} asx_watch_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_watch_slot g_slots[ASX_MAX_WATCHES];
static uint32_t g_slot_count = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void watch_waiters_init(asx_watch_slot *s) {
    asx_wait_queue_init(&s->waiters, s->wait_slots, ASX_WATCH_MAX_RECEIVERS);
}

/* New version or sender gone: every parked receiver task re-polls. */
static void watch_wake_waiters(asx_watch_slot *s) {
    if (s->waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->waiters);
}

void asx_watch_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_WATCHES; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].sender_alive = 0;
        g_slots[i].value = 0;
        g_slots[i].version = 0;
        g_slots[i].receiver_count = 0;
        watch_waiters_init(&g_slots[i]);
    }
    g_slot_count = 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_watch_create(uint64_t initial_value, asx_watch_sender *out_sender,
                            asx_watch_receiver *out_receiver) {
    uint32_t idx;
    asx_watch_slot *s;

    if (out_sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    idx = ASX_MAX_WATCHES;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
            if (!g_slots[i].sender_alive && g_slots[i].receiver_count == 0) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_WATCHES) {
        if (g_slot_count >= ASX_MAX_WATCHES) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->sender_alive = 1;
    s->value = initial_value;
    /* Version 0 holds the initial value and the first receiver has seen
     * it: only later sends count as changes (Rust channel/watch.rs). */
    s->version = 0;
    s->receiver_count = 1;
    watch_waiters_init(s);

    out_sender->slot = idx;
    out_sender->generation = s->generation;
    out_receiver->slot = idx;
    out_receiver->generation = s->generation;
    out_receiver->last_seen_version = 0;

    return ASX_OK;
}

asx_status asx_watch_subscribe(const asx_watch_sender *sender, asx_watch_receiver *out_receiver) {
    asx_watch_slot *s;

    if (sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return ASX_E_STALE_HANDLE;
    if (!s->sender_alive) return ASX_E_DISCONNECTED;
    if (s->receiver_count >= ASX_WATCH_MAX_RECEIVERS) return ASX_E_RESOURCE_EXHAUSTED;

    s->receiver_count++;
    out_receiver->slot = sender->slot;
    out_receiver->generation = s->generation;
    /* A new subscriber has seen the current version and observes only
     * later changes (Rust Sender::subscribe). */
    out_receiver->last_seen_version = s->version;

    return ASX_OK;
}

void asx_watch_sender_drop(asx_watch_sender *sender) {
    asx_watch_slot *s;
    if (sender == NULL) return;
    if (sender->slot >= g_slot_count) return;
    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return;
    s->sender_alive = 0;
    watch_wake_waiters(s);
}

void asx_watch_receiver_drop(asx_watch_receiver *receiver) {
    asx_watch_slot *s;
    if (receiver == NULL) return;
    if (receiver->slot >= g_slot_count) return;
    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return;
    if (s->receiver_count > 0) s->receiver_count--;
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_watch_send(asx_watch_sender *sender, uint64_t value) {
    asx_watch_slot *s;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return ASX_E_STALE_HANDLE;
    if (!s->sender_alive) return ASX_E_INVALID_STATE;

    s->value = value;
    s->version++;

    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)sender->slot, value);
    watch_wake_waiters(s);

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_watch_recv(asx_watch_receiver *receiver, uint64_t *out_value) {
    asx_watch_slot *s;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return ASX_E_STALE_HANDLE;

    *out_value = s->value;
    receiver->last_seen_version = s->version;

    return ASX_OK;
}

int asx_watch_has_changed(const asx_watch_receiver *receiver) {
    const asx_watch_slot *s;

    if (receiver == NULL) return 0;
    if (receiver->slot >= g_slot_count) return 0;

    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return 0;

    return s->version != receiver->last_seen_version;
}

static void watch_trace(const asx_cx *cx, const char *message) {
    if (cx != NULL) asx_trace_user(cx->task_id, message);
}

void asx_watch_changed_begin(const asx_watch_receiver *receiver, asx_cx *cx) {
    if (receiver == NULL) return;
    watch_trace(cx, "watch::changed starting wait");
}

asx_status asx_watch_poll_changed(asx_watch_receiver *receiver, asx_cx *cx) {
    asx_watch_slot *s;

    if (receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return ASX_E_STALE_HANDLE;

    /* Rust's poll_changed order: cancellation, a newer version, the sender
     * gone (watch.rs:735-761). */
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        asx_wait_queue_leave_current(&s->waiters);
        watch_trace(cx, "watch::changed cancelled");
        return ASX_E_CANCELLED;
    }

    if (s->version != receiver->last_seen_version) {
        receiver->last_seen_version = s->version;
        asx_wait_queue_leave_current(&s->waiters);
        watch_trace(cx, "watch::changed received update");
        return ASX_OK;
    }

    if (!s->sender_alive) {
        asx_wait_queue_leave_current(&s->waiters);
        watch_trace(cx, "watch::changed sender dropped");
        return ASX_E_DISCONNECTED;
    }

    (void)asx_wait_queue_park_current(&s->waiters);
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_watch_receiver_count(const asx_watch_sender *sender) {
    if (sender == NULL) return 0;
    if (sender->slot >= g_slot_count) return 0;
    if (g_slots[sender->slot].generation != sender->generation) return 0;
    return g_slots[sender->slot].receiver_count;
}

int asx_watch_sender_is_alive(uint32_t slot, uint16_t generation) {
    if (slot >= g_slot_count) return 0;
    if (g_slots[slot].generation != generation) return 0;
    return g_slots[slot].sender_alive;
}
