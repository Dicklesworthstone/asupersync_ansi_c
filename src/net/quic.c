/*
 * quic.c — in-memory QUIC model (MODEL-ONLY, DEC-010)
 *
 * Not a QUIC implementation: there is no UDP socket, no TLS 1.3 handshake,
 * no packet or frame, no loss recovery and no congestion control. This file
 * models two QUIC endpoints inside one process so lab scenarios can exercise
 * connection and stream lifecycles deterministically.
 *
 * The model is honest about what it has: a connection starts in HANDSHAKE
 * and becomes CONNECTED only when an in-process accept pairs it with its
 * peer end (asx_quic_accept); with no peer it never completes. Datagrams are
 * delivered to the paired peer's queue. Streams are loopback buffers: bytes
 * written to a stream are read back from the same stream handle.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/quic.h>
#include <asx/runtime/runtime.h>
#include <string.h>
#define QUIC_DGRAM_QUEUE_DEPTH 4u

typedef struct {
    uint8_t data[ASX_QUIC_MAX_DGRAM];
    uint32_t len;
} quic_datagram;

typedef struct {
    asx_quic_config config;
    asx_quic_state state;
    quic_datagram dgram_queue[QUIC_DGRAM_QUEUE_DEPTH];
    uint32_t dgram_head;
    uint32_t dgram_count;
    uint32_t stream_count;
    uint32_t generation;
    /* The paired end, set when an accept completed the handshake. */
    uint32_t peer_slot;
    uint32_t peer_generation;
    int has_peer;
    /* Order of a connect still waiting for its accept (0: not waiting). */
    uint32_t connect_seq;
    int alive;
} quic_conn_slot;

typedef struct {
    uint32_t conn_slot;
    uint32_t conn_generation;
    asx_quic_stream_type stype;
    asx_buf_mut inbox;
    uint32_t generation;
    int alive;
} quic_stream_slot;

static quic_conn_slot g_quic_conns[ASX_MAX_QUIC_CONNECTIONS];
static quic_stream_slot g_quic_streams[ASX_MAX_QUIC_STREAMS];
static uint32_t g_quic_connect_seq;

static uint32_t quic_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static quic_conn_slot *quic_conn_lookup(asx_quic_conn h) {
    quic_conn_slot *s;

    if (h.slot >= ASX_MAX_QUIC_CONNECTIONS) return NULL;
    s = &g_quic_conns[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static quic_stream_slot *quic_stream_lookup(asx_quic_stream h) {
    quic_stream_slot *s;

    if (h.slot >= ASX_MAX_QUIC_STREAMS) return NULL;
    s = &g_quic_streams[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* QUIC config                                                         */
/* ------------------------------------------------------------------ */

void asx_quic_config_init(asx_quic_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_bidi_streams = 4u;
    cfg->max_uni_streams = 4u;
    cfg->idle_timeout_ms = 30000u;
    cfg->enable_datagrams = 0u;
}

/* ------------------------------------------------------------------ */
/* QUIC connection API                                                 */
/* ------------------------------------------------------------------ */

/* A connection slot in HANDSHAKE: no peer yet, nothing has happened. The
 * slot's generation survives reuse, so a handle to a closed connection
 * stays dead. */
static asx_status quic_alloc(asx_quic_conn *out, const asx_quic_config *cfg) {
    uint32_t idx;
    uint32_t generation;
    quic_conn_slot *s;

    for (idx = 0u; idx < ASX_MAX_QUIC_CONNECTIONS; idx++) {
        if (!g_quic_conns[idx].alive) break;
    }
    if (idx >= ASX_MAX_QUIC_CONNECTIONS) return ASX_E_RESOURCE_EXHAUSTED;
    asx_runtime_reset_hook_register_internal(ASX_RESET_SLOT_QUIC, asx_quic_reset);
    s = &g_quic_conns[idx];
    generation = quic_next_gen(s->generation);
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->alive = 1;
    s->config = *cfg;
    s->state = ASX_QUIC_STATE_HANDSHAKE;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

/* A client handshake is begun, not completed: with no network and no peer
 * the connection stays in HANDSHAKE until asx_quic_accept pairs it. */
asx_status asx_quic_connect(asx_quic_conn *out, const asx_quic_config *cfg) {
    asx_status st;
    asx_quic_conn conn;

    if (out == NULL || cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    st = quic_alloc(&conn, cfg);
    if (st != ASX_OK) return st;
    g_quic_connect_seq++;
    if (g_quic_connect_seq == 0u) g_quic_connect_seq = 1u;
    g_quic_conns[conn.slot].connect_seq = g_quic_connect_seq;
    *out = conn;
    return ASX_OK;
}

/* The server end of the oldest connect nobody has accepted. Both ends are
 * CONNECTED when this returns ASX_OK; with no connect waiting there is
 * nothing to accept (ASX_E_PENDING, *out untouched). */
asx_status asx_quic_accept(asx_quic_conn *out, const asx_quic_config *cfg) {
    uint32_t idx;
    uint32_t best = ASX_MAX_QUIC_CONNECTIONS;
    quic_conn_slot *client;
    quic_conn_slot *server;
    asx_quic_conn conn;
    asx_status st;

    if (out == NULL || cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    for (idx = 0u; idx < ASX_MAX_QUIC_CONNECTIONS; idx++) {
        const quic_conn_slot *c = &g_quic_conns[idx];
        if (!c->alive || c->connect_seq == 0u) continue;
        if (best == ASX_MAX_QUIC_CONNECTIONS || c->connect_seq < g_quic_conns[best].connect_seq) {
            best = idx;
        }
    }
    if (best == ASX_MAX_QUIC_CONNECTIONS) return ASX_E_PENDING;

    st = quic_alloc(&conn, cfg);
    if (st != ASX_OK) return st;
    client = &g_quic_conns[best];
    server = &g_quic_conns[conn.slot];
    client->connect_seq = 0u;
    client->has_peer = 1;
    client->peer_slot = conn.slot;
    client->peer_generation = conn.generation;
    client->state = ASX_QUIC_STATE_CONNECTED;
    server->has_peer = 1;
    server->peer_slot = best;
    server->peer_generation = client->generation;
    server->state = ASX_QUIC_STATE_CONNECTED;
    *out = conn;
    return ASX_OK;
}

asx_quic_state asx_quic_conn_state(asx_quic_conn conn) {
    quic_conn_slot *s = quic_conn_lookup(conn);
    if (s == NULL) return ASX_QUIC_STATE_CLOSED;
    return s->state;
}

asx_status asx_quic_conn_close(asx_quic_conn conn, uint32_t error_code) {
    quic_conn_slot *s;
    uint32_t idx;

    (void)error_code;
    s = quic_conn_lookup(conn);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state == ASX_QUIC_STATE_CLOSED) return ASX_E_INVALID_STATE;
    s->state = ASX_QUIC_STATE_DRAINING;

    /* The paired end sees the close: it can send nothing more and can only
     * close its own side. A connect nobody accepted just stops waiting. */
    if (s->has_peer && s->peer_slot < ASX_MAX_QUIC_CONNECTIONS) {
        quic_conn_slot *peer = &g_quic_conns[s->peer_slot];
        if (peer->alive && peer->generation == s->peer_generation) {
            peer->has_peer = 0;
            if (peer->state == ASX_QUIC_STATE_CONNECTED) peer->state = ASX_QUIC_STATE_DRAINING;
        }
    }
    s->has_peer = 0;
    s->connect_seq = 0u;

    /* Close all streams belonging to this connection */
    for (idx = 0u; idx < ASX_MAX_QUIC_STREAMS; idx++) {
        if (g_quic_streams[idx].alive && g_quic_streams[idx].conn_slot == conn.slot &&
            g_quic_streams[idx].conn_generation == conn.generation) {
            g_quic_streams[idx].alive = 0;
        }
    }
    s->stream_count = 0u;

    /* In deterministic mode, draining completes immediately */
    /* ASX_ANALYZER_WAIVER("state machine: DRAINING is the protocol's step before CLOSED") */
    s->state = ASX_QUIC_STATE_CLOSED;
    s->alive = 0;
    return ASX_OK;
}

int asx_quic_conn_is_alive(asx_quic_conn conn) { return quic_conn_lookup(conn) != NULL; }

/* ------------------------------------------------------------------ */
/* QUIC stream API                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_quic_stream_open(asx_quic_stream *out, asx_quic_conn conn,
                                asx_quic_stream_type stype) {
    quic_conn_slot *c;
    uint32_t idx;
    uint32_t generation;
    quic_stream_slot *s;
    uint32_t max;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    c = quic_conn_lookup(conn);
    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (c->state != ASX_QUIC_STATE_CONNECTED) return ASX_E_INVALID_STATE;

    max = stype == ASX_QUIC_STREAM_BIDI ? c->config.max_bidi_streams : c->config.max_uni_streams;
    if (c->stream_count >= max) return ASX_E_RESOURCE_EXHAUSTED;

    for (idx = 0u; idx < ASX_MAX_QUIC_STREAMS; idx++) {
        if (!g_quic_streams[idx].alive) break;
    }
    if (idx >= ASX_MAX_QUIC_STREAMS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_quic_streams[idx];
    generation = quic_next_gen(s->generation);
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->alive = 1;
    s->conn_slot = conn.slot;
    s->conn_generation = conn.generation;
    s->stype = stype;
    asx_buf_mut_init(&s->inbox);

    c->stream_count++;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_quic_stream_poll_read(asx_quic_stream stream, asx_buf_mut *dst,
                                     uint32_t *bytes_read) {
    quic_stream_slot *s;
    asx_buf readable;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    s = quic_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_buf_mut_remaining(&s->inbox) == 0u) {
        *bytes_read = 0u;
        return ASX_E_PENDING;
    }
    if (asx_buf_mut_writable(dst) == 0u) return ASX_E_BUFFER_TOO_SMALL;

    readable = asx_buf_mut_readable(&s->inbox);
    to_copy = readable.len;
    if (to_copy > asx_buf_mut_writable(dst)) to_copy = asx_buf_mut_writable(dst);

    st = asx_buf_mut_put(dst, readable.ptr, to_copy);
    if (st != ASX_OK) return st;
    st = asx_buf_mut_advance(&s->inbox, to_copy);
    if (st != ASX_OK) return st;
    if (asx_buf_mut_remaining(&s->inbox) == 0u) asx_buf_mut_clear(&s->inbox);

    *bytes_read = to_copy;
    return ASX_OK;
}

asx_status asx_quic_stream_poll_write(asx_quic_stream stream, const asx_buf *src,
                                      uint32_t *bytes_written) {
    quic_stream_slot *s;
    asx_status st;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    if (src->len > 0u && src->ptr == NULL) return ASX_E_INVALID_ARGUMENT;
    s = quic_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    /* A stream is a loopback buffer: the model has no peer-initiated
     * streams, so bytes written here are read back from this handle. */
    if (src->len > asx_buf_mut_writable(&s->inbox)) return ASX_E_WOULD_BLOCK;

    st = asx_buf_mut_put(&s->inbox, src->ptr, src->len);
    if (st != ASX_OK) return st;
    *bytes_written = src->len;
    return ASX_OK;
}

asx_status asx_quic_stream_close(asx_quic_stream stream) {
    quic_stream_slot *s;
    quic_conn_slot *c;

    s = quic_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Decrement the parent connection's stream count if still alive */
    if (s->conn_slot < ASX_MAX_QUIC_CONNECTIONS) {
        c = &g_quic_conns[s->conn_slot];
        if (c->alive && c->generation == s->conn_generation && c->stream_count > 0u) {
            c->stream_count--;
        }
    }

    s->alive = 0;
    return ASX_OK;
}

int asx_quic_stream_is_alive(asx_quic_stream stream) { return quic_stream_lookup(stream) != NULL; }

/* ------------------------------------------------------------------ */
/* QUIC datagram API                                                   */
/* ------------------------------------------------------------------ */

/* Delivered to the paired end's queue. Both ends must have datagrams
 * enabled (a peer that did not advertise them cannot be sent one), and the
 * connection must be CONNECTED: a connect with no accept has no peer. */
asx_status asx_quic_send_datagram(asx_quic_conn conn, const asx_buf *src) {
    quic_conn_slot *c;
    quic_conn_slot *peer;
    uint32_t tail;
    quic_datagram *d;

    if (src == NULL) return ASX_E_INVALID_ARGUMENT;
    c = quic_conn_lookup(conn);
    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!c->config.enable_datagrams) return ASX_E_INVALID_STATE;
    if (c->state != ASX_QUIC_STATE_CONNECTED || !c->has_peer) return ASX_E_INVALID_STATE;
    peer = &g_quic_conns[c->peer_slot];
    if (!peer->alive || peer->generation != c->peer_generation) return ASX_E_INVALID_STATE;
    if (!peer->config.enable_datagrams) return ASX_E_INVALID_STATE;
    if (src->len > ASX_QUIC_MAX_DGRAM) return ASX_E_BUFFER_TOO_SMALL;
    if (src->len > 0u && src->ptr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (peer->dgram_count >= QUIC_DGRAM_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;

    tail = (peer->dgram_head + peer->dgram_count) % QUIC_DGRAM_QUEUE_DEPTH;
    d = &peer->dgram_queue[tail];
    memset(d, 0, sizeof(*d));
    d->len = src->len;
    if (src->len > 0u) memcpy(d->data, src->ptr, src->len);
    peer->dgram_count++;
    return ASX_OK;
}

asx_status asx_quic_poll_recv_datagram(asx_quic_conn conn, asx_buf_mut *dst, uint32_t *bytes_read) {
    quic_conn_slot *c;
    quic_datagram *d;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    c = quic_conn_lookup(conn);
    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!c->config.enable_datagrams) return ASX_E_INVALID_STATE;
    if (c->dgram_count == 0u) {
        *bytes_read = 0u;
        return ASX_E_PENDING;
    }

    d = &c->dgram_queue[c->dgram_head];
    st = asx_buf_mut_put(dst, d->data, d->len);
    if (st != ASX_OK) return st;
    *bytes_read = d->len;
    c->dgram_head = (c->dgram_head + 1u) % QUIC_DGRAM_QUEUE_DEPTH;
    c->dgram_count--;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_quic_reset(void) {
    memset(g_quic_conns, 0, sizeof(g_quic_conns));
    memset(g_quic_streams, 0, sizeof(g_quic_streams));
    g_quic_connect_seq = 0u;
}
