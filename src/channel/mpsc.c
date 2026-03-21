/*
 * mpsc.c — bounded MPSC two-phase channel implementation (bd-2cw.6)
 *
 * Walking skeleton: single-threaded, non-blocking (try_reserve/try_recv).
 * Fixed-size arena of channel slots with ring-buffer message queues.
 *
 * Two-phase protocol:
 *   1. try_reserve — claims capacity, returns permit
 *   2. send (via permit) — enqueues value FIFO
 *      OR abort (via permit) — returns capacity without enqueuing
 *
 * Capacity invariant: queue_len + reserved_count <= capacity
 *
 * Semantics specified in docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md.
 *
 * ASX_PROOF_BLOCK_WAIVER("reason: bug fix for recv after close, no semantic break")
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal channel slot                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_channel_state state;
    asx_region_id region;
    uint16_t generation;
    int alive;

    /* Bounded ring buffer */
    uint32_t capacity;
    uint64_t queue[ASX_CHANNEL_MAX_CAPACITY];
    uint32_t queue_head; /* next read position */
    uint32_t queue_len;  /* committed messages in queue */

    /* Two-phase accounting */
    uint32_t reserved;                                /* outstanding permits */
    uint32_t next_token;                              /* monotonic permit token */
    uint32_t permit_tokens[ASX_CHANNEL_MAX_CAPACITY]; /* 0 = free */
} asx_channel_slot;

static asx_channel_slot g_channels[ASX_MAX_CHANNELS];
static uint32_t g_channel_count;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static asx_status channel_slot_lookup(asx_channel_id id, asx_channel_slot **out) {
    uint16_t slot_idx;
    uint16_t gen;
    asx_channel_slot *s;

    if (!asx_handle_is_valid(id)) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_handle_type_tag(id) != ASX_TYPE_CHANNEL) { return ASX_E_INVALID_ARGUMENT; }

    slot_idx = asx_handle_slot(id);
    gen = asx_handle_generation(id);

    if (slot_idx >= ASX_MAX_CHANNELS) { return ASX_E_NOT_FOUND; }

    s = &g_channels[slot_idx];
    if (!s->alive) { return ASX_E_NOT_FOUND; }
    if (s->generation != gen) { return ASX_E_STALE_HANDLE; }

    *out = s;
    return ASX_OK;
}

static asx_channel_id channel_make_handle(uint16_t slot_idx, uint16_t gen) {
    uint32_t index = asx_handle_pack_index(gen, slot_idx);
    return asx_handle_pack(ASX_TYPE_CHANNEL, 0, index);
}

static int channel_token_find(const asx_channel_slot *s, uint32_t token, uint32_t *out_idx) {
    uint32_t i;

    if (token == 0u) return 0;

    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        if (s->permit_tokens[i] == token) {
            if (out_idx != NULL) { *out_idx = i; }
            return 1;
        }
    }

    return 0;
}

static uint32_t channel_token_allocate(asx_channel_slot *s) {
    uint32_t token;

    /* token 0 is reserved as "invalid / free-slot marker" */
    token = s->next_token;
    if (token == 0u) token = 1u;

    while (channel_token_find(s, token, NULL)) {
        ASX_CHECKPOINT_WAIVER("bounded: at most ASX_CHANNEL_MAX_CAPACITY tokens active");
        token++;
        if (token == 0u) token = 1u;
    }

    s->next_token = token + 1u;
    if (s->next_token == 0u) s->next_token = 1u;

    return token;
}

static asx_status channel_token_consume(asx_channel_slot *s, uint32_t token) {
    uint32_t idx;

    if (!channel_token_find(s, token, &idx)) { return ASX_E_INVALID_STATE; }

    s->permit_tokens[idx] = 0u;
    if (s->reserved > 0u) { s->reserved--; }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Channel lifecycle                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_channel_create(asx_region_id region, uint32_t capacity, asx_channel_id *out_id) {
    uint16_t i;
    asx_channel_slot *s;
    asx_region_state region_state;
    asx_status st;

    if (out_id == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (capacity == 0 || capacity > ASX_CHANNEL_MAX_CAPACITY) { return ASX_E_INVALID_ARGUMENT; }
    if (!asx_handle_is_valid(region)) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_handle_type_tag(region) != ASX_TYPE_REGION) { return ASX_E_INVALID_ARGUMENT; }

    st = asx_region_get_state(region, &region_state);
    if (st != ASX_OK) { return st; }
    if (region_state != ASX_REGION_OPEN) { return ASX_E_INVALID_STATE; }

    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        if (!g_channels[i].alive || g_channels[i].state == ASX_CHANNEL_FULLY_CLOSED) {
            s = &g_channels[i];

            s->generation++;
            if (s->generation == 0) s->generation = 1;
            s->state = ASX_CHANNEL_OPEN;
            s->region = region;
            s->alive = 1;
            s->capacity = capacity;
            s->queue_head = 0;
            s->queue_len = 0;
            s->reserved = 0;
            s->next_token = 1;
            memset(s->queue, 0, sizeof(s->queue));
            memset(s->permit_tokens, 0, sizeof(s->permit_tokens));

            g_channel_count++;
            *out_id = channel_make_handle(i, s->generation);
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_channel_close_sender(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    switch (s->state) {
    case ASX_CHANNEL_OPEN: s->state = ASX_CHANNEL_SENDER_CLOSED; return ASX_OK;
    case ASX_CHANNEL_RECEIVER_CLOSED: s->state = ASX_CHANNEL_FULLY_CLOSED; return ASX_OK;
    case ASX_CHANNEL_SENDER_CLOSED:
    case ASX_CHANNEL_FULLY_CLOSED: return ASX_E_INVALID_STATE;
    }

    return ASX_E_INVALID_STATE;
}

asx_status asx_channel_close_receiver(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    switch (s->state) {
    case ASX_CHANNEL_OPEN:
        s->state = ASX_CHANNEL_RECEIVER_CLOSED;
        s->queue_len = 0;
        s->queue_head = 0;
        return ASX_OK;
    case ASX_CHANNEL_SENDER_CLOSED:
        s->state = ASX_CHANNEL_FULLY_CLOSED;
        s->queue_len = 0;
        s->queue_head = 0;
        return ASX_OK;
    case ASX_CHANNEL_RECEIVER_CLOSED:
    case ASX_CHANNEL_FULLY_CLOSED: return ASX_E_INVALID_STATE;
    }

    return ASX_E_INVALID_STATE;
}

/* ------------------------------------------------------------------ */
/* Channel queries                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_channel_get_state(asx_channel_id id, asx_channel_state *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    *out = s->state;
    return ASX_OK;
}

asx_status asx_channel_queue_len(asx_channel_id id, uint32_t *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    *out = s->queue_len;
    return ASX_OK;
}

asx_status asx_channel_reserved_count(asx_channel_id id, uint32_t *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    *out = s->reserved;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: reserve                                            */
/* ------------------------------------------------------------------ */

asx_status asx_channel_try_reserve(asx_channel_id id, asx_send_permit *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_INVALID_STATE;
    }

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED) { return ASX_E_DISCONNECTED; }

    if (s->queue_len + s->reserved >= s->capacity) { return ASX_E_CHANNEL_FULL; }

    {
        uint32_t permit_idx;
        uint32_t token = channel_token_allocate(s);
        int found = 0;
        for (permit_idx = 0; permit_idx < ASX_CHANNEL_MAX_CAPACITY; permit_idx++) {
            ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
            if (s->permit_tokens[permit_idx] == 0u) {
                s->permit_tokens[permit_idx] = token;
                found = 1;
                out->channel_id = id;
                out->token = token;
                out->consumed = 0;
                s->reserved++;
                break;
            }
        }
        if (!found) { return ASX_E_RESOURCE_EXHAUSTED; }
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: commit (send value)                                */
/* ------------------------------------------------------------------ */

asx_status asx_send_permit_send(asx_send_permit *permit, uint64_t value) {
    asx_channel_slot *s;
    asx_status st;
    uint32_t write_pos;

    if (permit == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (permit->consumed) { return ASX_E_INVALID_STATE; }

    st = channel_slot_lookup(permit->channel_id, &s);
    if (st != ASX_OK) {
        permit->consumed = 1;
        return st;
    }

    st = channel_token_consume(s, permit->token);
    if (st != ASX_OK) {
        permit->consumed = 1;
        return st;
    }

    permit->consumed = 1;

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_DISCONNECTED;
    }

    if (s->queue_len >= s->capacity) { return ASX_E_CHANNEL_FULL; }

    write_pos = (s->queue_head + s->queue_len) % s->capacity;
    s->queue[write_pos] = value;
    s->queue_len++;
    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)permit->channel_id, value);

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: abort (return capacity)                            */
/* ------------------------------------------------------------------ */

void asx_send_permit_abort(asx_send_permit *permit) {
    asx_channel_slot *s;
    asx_status st;

    if (permit == NULL || permit->consumed) { return; }

    permit->consumed = 1;

    st = channel_slot_lookup(permit->channel_id, &s);
    if (st != ASX_OK) { return; }

    (void)channel_token_consume(s, permit->token);
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

asx_status asx_channel_try_recv(asx_channel_id id, uint64_t *out_value) {
    asx_channel_slot *s;
    asx_status st;

    if (out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    if (s->queue_len > 0) {
        *out_value = s->queue[s->queue_head];
        s->queue_head = (s->queue_head + 1u) % s->capacity;
        s->queue_len--;
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)id, *out_value);
        return ASX_OK;
    }

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_DISCONNECTED;
    }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED) {
        if (s->reserved == 0) { return ASX_E_DISCONNECTED; }
    }

    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Reset (test support)                                               */
/* ------------------------------------------------------------------ */

void asx_channel_reset(void) {
    uint16_t i;

    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        if (g_channels[i].alive) { g_channels[i].generation++; }
        g_channels[i].alive = 0;
        g_channels[i].state = ASX_CHANNEL_OPEN;
        g_channels[i].queue_head = 0;
        g_channels[i].queue_len = 0;
        g_channels[i].reserved = 0;
        g_channels[i].next_token = 1;
        memset(g_channels[i].queue, 0, sizeof(g_channels[i].queue));
        memset(g_channels[i].permit_tokens, 0, sizeof(g_channels[i].permit_tokens));
    }
    g_channel_count = 0;
}
