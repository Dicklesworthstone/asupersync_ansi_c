/*
 * broadcast.c — bounded broadcast channel
 *
 * Walking skeleton: fixed-size arena, single-threaded.
 * Ring buffer with per-receiver cursors.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/broadcast.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int sender_alive;
    uint32_t capacity;
    uint64_t buffer[ASX_BROADCAST_MAX_CAPACITY];
    uint32_t write_seq; /* next write position (monotonic) */
    uint32_t receiver_count;
} asx_broadcast_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_broadcast_slot g_slots[ASX_MAX_BROADCASTS];
static uint32_t g_slot_count = 0;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

void asx_broadcast_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_BROADCASTS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].sender_alive = 0;
        g_slots[i].capacity = 0;
        g_slots[i].write_seq = 0;
        g_slots[i].receiver_count = 0;
        memset(g_slots[i].buffer, 0, sizeof(g_slots[i].buffer));
    }
    g_slot_count = 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_broadcast_create(uint32_t capacity, asx_broadcast_sender *out_sender,
                                asx_broadcast_receiver *out_receiver) {
    uint32_t idx;
    asx_broadcast_slot *s;

    if (out_sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    if (capacity == 0 || capacity > ASX_BROADCAST_MAX_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    idx = ASX_MAX_BROADCASTS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            if (!g_slots[i].sender_alive && g_slots[i].receiver_count == 0) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_BROADCASTS) {
        if (g_slot_count >= ASX_MAX_BROADCASTS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->sender_alive = 1;
    s->capacity = capacity;
    s->write_seq = 0;
    s->receiver_count = 1;
    memset(s->buffer, 0, sizeof(s->buffer));

    out_sender->slot = idx;
    out_sender->generation = s->generation;
    out_receiver->slot = idx;
    out_receiver->generation = s->generation;
    out_receiver->cursor = 0;

    return ASX_OK;
}

asx_status asx_broadcast_subscribe(const asx_broadcast_sender *sender,
                                   asx_broadcast_receiver *out_receiver) {
    asx_broadcast_slot *s;

    if (sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return ASX_E_STALE_HANDLE;
    if (!s->sender_alive) return ASX_E_DISCONNECTED;
    if (s->receiver_count >= ASX_BROADCAST_MAX_RECEIVERS) return ASX_E_RESOURCE_EXHAUSTED;

    s->receiver_count++;
    out_receiver->slot = sender->slot;
    out_receiver->generation = s->generation;
    out_receiver->cursor = s->write_seq; /* start from next message */

    return ASX_OK;
}

void asx_broadcast_sender_drop(asx_broadcast_sender *sender) {
    asx_broadcast_slot *s;
    if (sender == NULL) return;
    if (sender->slot >= g_slot_count) return;
    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return;
    s->sender_alive = 0;
}

void asx_broadcast_receiver_drop(asx_broadcast_receiver *receiver) {
    asx_broadcast_slot *s;
    if (receiver == NULL) return;
    if (receiver->slot >= g_slot_count) return;
    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return;
    if (s->receiver_count > 0) s->receiver_count--;
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_broadcast_send(asx_broadcast_sender *sender, uint64_t value) {
    asx_broadcast_slot *s;
    uint32_t ring_idx;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    if (sender->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return ASX_E_STALE_HANDLE;
    if (!s->sender_alive) return ASX_E_INVALID_STATE;

    /* Write into ring buffer (overwrites old messages) */
    ring_idx = s->write_seq % s->capacity;
    s->buffer[ring_idx] = value;
    s->write_seq++;

    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)sender->slot, value);

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_broadcast_try_recv(asx_broadcast_receiver *receiver, uint64_t *out_value) {
    asx_broadcast_slot *s;
    uint32_t oldest_available;
    uint32_t ring_idx;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return ASX_E_STALE_HANDLE;

    /* Nothing to read */
    if (receiver->cursor >= s->write_seq) {
        if (!s->sender_alive) return ASX_E_DISCONNECTED;
        return ASX_E_WOULD_BLOCK;
    }

    /* Check for lag: cursor too far behind write_seq */
    oldest_available = 0;
    if (s->write_seq > s->capacity) { oldest_available = s->write_seq - s->capacity; }

    if (receiver->cursor < oldest_available) {
        /* Receiver has lagged — advance cursor to oldest available */
        receiver->cursor = oldest_available;
        return ASX_E_LAGGED;
    }

    /* Read the value */
    ring_idx = receiver->cursor % s->capacity;
    *out_value = s->buffer[ring_idx];
    receiver->cursor++;

    asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)receiver->slot, *out_value);

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_broadcast_receiver_count(const asx_broadcast_sender *sender) {
    if (sender == NULL) return 0;
    if (sender->slot >= g_slot_count) return 0;
    if (g_slots[sender->slot].generation != sender->generation) return 0;
    return g_slots[sender->slot].receiver_count;
}

uint32_t asx_broadcast_total_sent(const asx_broadcast_sender *sender) {
    if (sender == NULL) return 0;
    if (sender->slot >= g_slot_count) return 0;
    if (g_slots[sender->slot].generation != sender->generation) return 0;
    return g_slots[sender->slot].write_seq;
}
