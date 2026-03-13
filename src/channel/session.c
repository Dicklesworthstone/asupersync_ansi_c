/*
 * session.c — bidirectional session channel with obligation tracking
 *
 * Walking skeleton: uses inline ring buffers (not MPSC channels)
 * to avoid coupling session arena to channel arena capacity.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/session.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal: per-direction buffer                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t buf[ASX_SESSION_MAX_CAPACITY];
    uint32_t head;
    uint32_t len;
    uint32_t capacity;
} asx_session_queue;

static void queue_init(asx_session_queue *q, uint32_t capacity)
{
    memset(q->buf, 0, sizeof(q->buf));
    q->head = 0;
    q->len = 0;
    q->capacity = capacity;
}

static int queue_push(asx_session_queue *q, uint64_t value)
{
    uint32_t tail;
    if (q->len >= q->capacity) return 0;
    tail = (q->head + q->len) % q->capacity;
    q->buf[tail] = value;
    q->len++;
    return 1;
}

static int queue_pop(asx_session_queue *q, uint64_t *out)
{
    if (q->len == 0) return 0;
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->len--;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t           generation;
    asx_session_state  state;
    asx_region_id      region;
    int                initiator_alive;
    int                responder_alive;
    asx_session_queue  i2r;           /* initiator → responder */
    asx_session_queue  r2i;           /* responder → initiator */
    uint32_t           obligations;   /* outstanding request count */
} asx_session_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_session_slot g_slots[ASX_MAX_SESSIONS];
static uint32_t g_slot_count = 0;

static uint16_t next_gen(uint16_t g)
{
    g++;
    if (g == 0) g = 1;
    return g;
}

void asx_session_reset(void)
{
    uint32_t i;
    for (i = 0; i < ASX_MAX_SESSIONS; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].state = ASX_SESSION_CLOSED;
        g_slots[i].initiator_alive = 0;
        g_slots[i].responder_alive = 0;
        g_slots[i].obligations = 0;
    }
    g_slot_count = 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_session_create(asx_region_id region,
                               uint32_t capacity,
                               asx_session_endpoint *out_initiator,
                               asx_session_endpoint *out_responder)
{
    uint32_t idx;
    asx_session_slot *s;

    if (out_initiator == NULL || out_responder == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (capacity == 0 || capacity > ASX_SESSION_MAX_CAPACITY)
        return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    idx = ASX_MAX_SESSIONS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            if (g_slots[i].state == ASX_SESSION_CLOSED &&
                !g_slots[i].initiator_alive && !g_slots[i].responder_alive) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_SESSIONS) {
        if (g_slot_count >= ASX_MAX_SESSIONS)
            return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->state = ASX_SESSION_OPEN;
    s->region = region;
    s->initiator_alive = 1;
    s->responder_alive = 1;
    s->obligations = 0;
    queue_init(&s->i2r, capacity);
    queue_init(&s->r2i, capacity);

    out_initiator->slot = idx;
    out_initiator->generation = s->generation;
    out_initiator->is_initiator = 1;

    out_responder->slot = idx;
    out_responder->generation = s->generation;
    out_responder->is_initiator = 0;

    return ASX_OK;
}

void asx_session_endpoint_drop(asx_session_endpoint *ep)
{
    asx_session_slot *s;
    if (ep == NULL) return;
    if (ep->slot >= g_slot_count) return;
    s = &g_slots[ep->slot];
    if (s->generation != ep->generation) return;

    if (ep->is_initiator) {
        s->initiator_alive = 0;
    } else {
        s->responder_alive = 0;
    }

    if (!s->initiator_alive && !s->responder_alive) {
        s->state = ASX_SESSION_CLOSED;
    } else if (s->state == ASX_SESSION_OPEN) {
        s->state = ASX_SESSION_HALF_CLOSED;
    }
}

/* ------------------------------------------------------------------ */
/* Send / Receive                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_session_send(asx_session_endpoint *ep, uint64_t value)
{
    asx_session_slot *s;
    asx_session_queue *q;

    if (ep == NULL) return ASX_E_INVALID_ARGUMENT;
    if (ep->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[ep->slot];
    if (s->generation != ep->generation) return ASX_E_STALE_HANDLE;

    /* Check sender is alive */
    if (ep->is_initiator && !s->initiator_alive) return ASX_E_INVALID_STATE;
    if (!ep->is_initiator && !s->responder_alive) return ASX_E_INVALID_STATE;

    /* Check peer is alive */
    if (ep->is_initiator && !s->responder_alive) return ASX_E_DISCONNECTED;
    if (!ep->is_initiator && !s->initiator_alive) return ASX_E_DISCONNECTED;

    /* Select direction */
    q = ep->is_initiator ? &s->i2r : &s->r2i;

    if (!queue_push(q, value))
        return ASX_E_CHANNEL_FULL;

    /* Initiator sends are requests: increment obligations */
    if (ep->is_initiator) {
        s->obligations++;
    }

    asx_trace_emit(ASX_TRACE_CHANNEL_SEND,
                   (uint64_t)ep->slot, value);

    return ASX_OK;
}

asx_status asx_session_try_recv(asx_session_endpoint *ep, uint64_t *out_value)
{
    asx_session_slot *s;
    asx_session_queue *q;
    int peer_alive;

    if (ep == NULL || out_value == NULL)
        return ASX_E_INVALID_ARGUMENT;
    if (ep->slot >= g_slot_count) return ASX_E_NOT_FOUND;

    s = &g_slots[ep->slot];
    if (s->generation != ep->generation) return ASX_E_STALE_HANDLE;

    /* Receive from the peer's send direction */
    q = ep->is_initiator ? &s->r2i : &s->i2r;
    peer_alive = ep->is_initiator ? s->responder_alive : s->initiator_alive;

    if (queue_pop(q, out_value)) {
        /* Responder receiving a request: decrement obligations */
        if (!ep->is_initiator) {
            if (s->obligations > 0) s->obligations--;
        }
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV,
                       (uint64_t)ep->slot, *out_value);
        return ASX_OK;
    }

    if (!peer_alive) return ASX_E_DISCONNECTED;
    return ASX_E_WOULD_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

asx_session_state asx_session_get_state(uint32_t slot, uint16_t gen)
{
    if (slot >= g_slot_count) return ASX_SESSION_CLOSED;
    if (g_slots[slot].generation != gen) return ASX_SESSION_CLOSED;
    return g_slots[slot].state;
}

uint32_t asx_session_obligations(uint32_t slot, uint16_t gen)
{
    if (slot >= g_slot_count) return 0;
    if (g_slots[slot].generation != gen) return 0;
    return g_slots[slot].obligations;
}

int asx_session_peer_alive(const asx_session_endpoint *ep)
{
    const asx_session_slot *s;
    if (ep == NULL) return 0;
    if (ep->slot >= g_slot_count) return 0;
    s = &g_slots[ep->slot];
    if (s->generation != ep->generation) return 0;
    return ep->is_initiator ? s->responder_alive : s->initiator_alive;
}
