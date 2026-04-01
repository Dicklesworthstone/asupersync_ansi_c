/*
 * oneshot.c — single-value, single-use channel
 *
 * Walking skeleton: fixed-size arena, single-threaded.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/oneshot.h>
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

void asx_oneshot_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_ONESHOTS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].state = ASX_ONESHOT_EMPTY;
        g_slots[i].value = 0;
        g_slots[i].sender_alive = 0;
        g_slots[i].receiver_alive = 0;
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

    return ASX_OK;
}

asx_status asx_oneshot_try_recv(asx_oneshot_receiver *receiver, uint64_t *out_value) {
    asx_oneshot_slot *s;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[receiver->slot];
    if (s->generation != receiver->generation) return ASX_E_STALE_HANDLE;
    if (!s->receiver_alive) return ASX_E_INVALID_STATE;

    if (s->state == ASX_ONESHOT_FILLED) {
        *out_value = s->value;
        s->state = ASX_ONESHOT_CONSUMED;
        s->receiver_alive = 0;
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)receiver->slot, s->value);
        return ASX_OK;
    }

    if (s->state == ASX_ONESHOT_SENDER_DROPPED || !s->sender_alive) {
        s->receiver_alive = 0;
        return ASX_E_DISCONNECTED;
    }

    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

asx_oneshot_state asx_oneshot_get_state(uint32_t slot, uint16_t generation) {
    if (slot >= g_slot_count) return ASX_ONESHOT_EMPTY;
    if (g_slots[slot].generation != generation) return ASX_ONESHOT_EMPTY;
    return g_slots[slot].state;
}
