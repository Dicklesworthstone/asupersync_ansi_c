/*
 * watch.c — single-value observable state channel
 *
 * Walking skeleton: fixed-size arena, single-threaded.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/watch.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int sender_alive;
    uint64_t value;
    uint32_t version; /* incremented on each send */
    uint32_t receiver_count;
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

void asx_watch_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_WATCHES; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].sender_alive = 0;
        g_slots[i].value = 0;
        g_slots[i].version = 0;
        g_slots[i].receiver_count = 0;
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
    s->version = 1; /* start at version 1 so receivers can detect first value */
    s->receiver_count = 1;

    out_sender->slot = idx;
    out_sender->generation = s->generation;
    out_receiver->slot = idx;
    out_receiver->generation = s->generation;
    out_receiver->last_seen_version = 0; /* hasn't seen any version yet */

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
    out_receiver->last_seen_version = 0;

    return ASX_OK;
}

void asx_watch_sender_drop(asx_watch_sender *sender) {
    asx_watch_slot *s;
    if (sender == NULL) return;
    if (sender->slot >= g_slot_count) return;
    s = &g_slots[sender->slot];
    if (s->generation != sender->generation) return;
    s->sender_alive = 0;
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
