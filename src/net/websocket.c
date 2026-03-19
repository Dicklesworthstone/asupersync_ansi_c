/*
 * websocket.c — deterministic WebSocket framing and connection
 *
 * In the deterministic core, WebSocket operates over in-memory TCP streams.
 * Frames are queued as value types (no real HTTP upgrade or masking).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/websocket.h>
#include <string.h>

#define ASX_WS_FRAME_QUEUE_DEPTH 8u

typedef struct {
    asx_tcp_stream tcp;
    asx_ws_state state;
    asx_ws_role role;
    asx_ws_frame recv_queue[ASX_WS_FRAME_QUEUE_DEPTH];
    uint32_t recv_head;
    uint32_t recv_count;
    uint32_t generation;
    uint32_t peer_slot;
    uint32_t peer_generation;
    int alive;
    int linked;
} ws_conn_slot;

static ws_conn_slot g_ws_conns[ASX_MAX_WS_CONNECTIONS];

static uint32_t ws_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static ws_conn_slot *ws_lookup(asx_ws_conn h) {
    ws_conn_slot *s;

    if (h.slot >= ASX_MAX_WS_CONNECTIONS) return NULL;
    s = &g_ws_conns[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static ws_conn_slot *ws_peer(const ws_conn_slot *s) {
    ws_conn_slot *peer;

    if (s == NULL || !s->linked) return NULL;
    if (s->peer_slot >= ASX_MAX_WS_CONNECTIONS) return NULL;
    peer = &g_ws_conns[s->peer_slot];
    if (!peer->alive || peer->generation != s->peer_generation) return NULL;
    return peer;
}

/* ------------------------------------------------------------------ */
/* Frame helpers                                                       */
/* ------------------------------------------------------------------ */

void asx_ws_frame_init(asx_ws_frame *frame) {
    if (frame == NULL) return;
    memset(frame, 0, sizeof(*frame));
    frame->opcode = ASX_WS_OPCODE_TEXT;
    frame->fin = 1u;
}

asx_status asx_ws_frame_set_payload(asx_ws_frame *frame, asx_ws_opcode opcode,
                                     const void *data, uint32_t len) {
    if (frame == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len > ASX_WS_MAX_PAYLOAD) return ASX_E_BUFFER_TOO_SMALL;
    if (len > 0u && data == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(frame->payload, 0, sizeof(frame->payload));
    frame->opcode = opcode;
    frame->payload_len = len;
    frame->fin = 1u;
    if (len > 0u) memcpy(frame->payload, data, len);
    return ASX_OK;
}

asx_status asx_ws_frame_close(asx_ws_frame *frame, uint16_t code) {
    if (frame == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(frame, 0, sizeof(*frame));
    frame->opcode = ASX_WS_OPCODE_CLOSE;
    frame->close_code = code;
    frame->fin = 1u;
    /* Encode close code as 2-byte big-endian payload */
    frame->payload[0] = (uint8_t)(code >> 8);
    frame->payload[1] = (uint8_t)(code & 0xFFu);
    frame->payload_len = 2u;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* WebSocket connection API                                            */
/* ------------------------------------------------------------------ */

static asx_status ws_alloc(asx_ws_conn *out, asx_tcp_stream tcp, asx_ws_role role) {
    uint32_t idx;
    ws_conn_slot *s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_WS_CONNECTIONS; idx++) {
        if (!g_ws_conns[idx].alive) break;
    }
    if (idx >= ASX_MAX_WS_CONNECTIONS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_ws_conns[idx];
    memset(s, 0, sizeof(*s));
    s->generation = ws_next_gen(s->generation);
    s->alive = 1;
    s->tcp = tcp;
    s->role = role;
    s->state = ASX_WS_STATE_OPEN;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_ws_connect(asx_ws_conn *out, asx_tcp_stream tcp) {
    return ws_alloc(out, tcp, ASX_WS_ROLE_CLIENT);
}

asx_status asx_ws_accept(asx_ws_conn *out, asx_tcp_stream tcp) {
    return ws_alloc(out, tcp, ASX_WS_ROLE_SERVER);
}

asx_status asx_ws_send(asx_ws_conn conn, const asx_ws_frame *frame) {
    ws_conn_slot *s;
    ws_conn_slot *peer;
    uint32_t tail;

    if (frame == NULL) return ASX_E_INVALID_ARGUMENT;
    s = ws_lookup(conn);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state != ASX_WS_STATE_OPEN && s->state != ASX_WS_STATE_CLOSING)
        return ASX_E_INVALID_STATE;

    peer = ws_peer(s);
    if (peer == NULL) {
        /* No linked peer — queue on self for loopback testing */
        if (s->recv_count >= ASX_WS_FRAME_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;
        tail = (s->recv_head + s->recv_count) % ASX_WS_FRAME_QUEUE_DEPTH;
        s->recv_queue[tail] = *frame;
        s->recv_count++;
    } else {
        if (peer->recv_count >= ASX_WS_FRAME_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;
        tail = (peer->recv_head + peer->recv_count) % ASX_WS_FRAME_QUEUE_DEPTH;
        peer->recv_queue[tail] = *frame;
        peer->recv_count++;
    }

    if (frame->opcode == ASX_WS_OPCODE_CLOSE) {
        s->state = ASX_WS_STATE_CLOSING;
    }
    return ASX_OK;
}

asx_status asx_ws_poll_recv(asx_ws_conn conn, asx_ws_frame *out) {
    ws_conn_slot *s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = ws_lookup(conn);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->recv_count == 0u) return ASX_E_PENDING;

    *out = s->recv_queue[s->recv_head];
    s->recv_head = (s->recv_head + 1u) % ASX_WS_FRAME_QUEUE_DEPTH;
    s->recv_count--;

    if (out->opcode == ASX_WS_OPCODE_CLOSE) {
        s->state = ASX_WS_STATE_CLOSED;
    }
    return ASX_OK;
}

asx_ws_state asx_ws_conn_state(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    if (s == NULL) return ASX_WS_STATE_CLOSED;
    return s->state;
}

asx_ws_role asx_ws_conn_role(asx_ws_conn conn) {
    ws_conn_slot *s = ws_lookup(conn);
    if (s == NULL) return ASX_WS_ROLE_CLIENT;
    return s->role;
}

asx_status asx_ws_close(asx_ws_conn conn, uint16_t code) {
    asx_ws_frame close_frame;
    asx_status st;

    st = asx_ws_frame_close(&close_frame, code);
    if (st != ASX_OK) return st;
    return asx_ws_send(conn, &close_frame);
}

asx_status asx_ws_conn_release(asx_ws_conn conn) {
    ws_conn_slot *s;

    s = ws_lookup(conn);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->state = ASX_WS_STATE_CLOSED;
    s->alive = 0;
    return ASX_OK;
}

int asx_ws_conn_is_alive(asx_ws_conn conn) { return ws_lookup(conn) != NULL; }

void asx_ws_reset(void) { memset(g_ws_conns, 0, sizeof(g_ws_conns)); }
