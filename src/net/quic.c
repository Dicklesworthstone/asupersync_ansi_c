/*
 * quic.c — deterministic QUIC connection and stream implementation
 *
 * In the deterministic core, QUIC connections and streams operate over
 * in-memory buffers. Handshakes succeed immediately and datagrams are
 * queued per-connection for deterministic testing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/quic.h>
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

static asx_status quic_alloc(asx_quic_conn *out, const asx_quic_config *cfg) {
    uint32_t idx;
    quic_conn_slot *s;

    if (out == NULL || cfg == NULL) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_QUIC_CONNECTIONS; idx++) {
        if (!g_quic_conns[idx].alive) break;
    }
    if (idx >= ASX_MAX_QUIC_CONNECTIONS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_quic_conns[idx];
    memset(s, 0, sizeof(*s));
    s->generation = quic_next_gen(s->generation);
    s->alive = 1;
    s->config = *cfg;
    s->state = ASX_QUIC_STATE_CONNECTED;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_quic_connect(asx_quic_conn *out, const asx_quic_config *cfg) {
    return quic_alloc(out, cfg);
}

asx_status asx_quic_accept(asx_quic_conn *out, const asx_quic_config *cfg) {
    return quic_alloc(out, cfg);
}

asx_quic_state asx_quic_conn_state(asx_quic_conn conn) {
    quic_conn_slot *s = quic_conn_lookup(conn);
    if (s == NULL) return ASX_QUIC_STATE_CLOSED;
    return s->state;
}

asx_status asx_quic_conn_close(asx_quic_conn conn, uint32_t error_code) {
    quic_conn_slot *s;

    (void)error_code;
    s = quic_conn_lookup(conn);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state == ASX_QUIC_STATE_CLOSED) return ASX_E_INVALID_STATE;
    s->state = ASX_QUIC_STATE_DRAINING;
    /* In deterministic mode, draining completes immediately */
    s->state = ASX_QUIC_STATE_CLOSED;
    s->alive = 0;
    return ASX_OK;
}

int asx_quic_conn_is_alive(asx_quic_conn conn) {
    return quic_conn_lookup(conn) != NULL;
}

/* ------------------------------------------------------------------ */
/* QUIC stream API                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_quic_stream_open(asx_quic_stream *out, asx_quic_conn conn,
                                 asx_quic_stream_type stype) {
    quic_conn_slot *c;
    uint32_t idx;
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
    memset(s, 0, sizeof(*s));
    s->generation = quic_next_gen(s->generation);
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
    s = quic_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    /* In deterministic mode, writes go to own inbox for self-test */
    if (src->len > asx_buf_mut_writable(&s->inbox)) return ASX_E_WOULD_BLOCK;

    st = asx_buf_mut_put(&s->inbox, src->ptr, src->len);
    if (st != ASX_OK) return st;
    *bytes_written = src->len;
    return ASX_OK;
}

asx_status asx_quic_stream_close(asx_quic_stream stream) {
    quic_stream_slot *s;

    s = quic_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->alive = 0;
    return ASX_OK;
}

int asx_quic_stream_is_alive(asx_quic_stream stream) {
    return quic_stream_lookup(stream) != NULL;
}

/* ------------------------------------------------------------------ */
/* QUIC datagram API                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_quic_send_datagram(asx_quic_conn conn, const asx_buf *src) {
    quic_conn_slot *c;
    uint32_t tail;
    quic_datagram *d;

    if (src == NULL) return ASX_E_INVALID_ARGUMENT;
    c = quic_conn_lookup(conn);
    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!c->config.enable_datagrams) return ASX_E_INVALID_STATE;
    if (src->len > ASX_QUIC_MAX_DGRAM) return ASX_E_BUFFER_TOO_SMALL;
    if (c->dgram_count >= QUIC_DGRAM_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;

    tail = (c->dgram_head + c->dgram_count) % QUIC_DGRAM_QUEUE_DEPTH;
    d = &c->dgram_queue[tail];
    memset(d, 0, sizeof(*d));
    d->len = src->len;
    if (src->len > 0u) memcpy(d->data, src->ptr, src->len);
    c->dgram_count++;
    return ASX_OK;
}

asx_status asx_quic_poll_recv_datagram(asx_quic_conn conn, asx_buf_mut *dst,
                                        uint32_t *bytes_read) {
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
}
