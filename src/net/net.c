/*
 * net.c — minimal network types and socket primitives
 *
 * Walking skeleton: ghost stubs for TCP listener/stream.
 * Platform socket integration deferred to Phase 3.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/net.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Socket address                                                      */
/* ------------------------------------------------------------------ */

asx_socket_addr asx_socket_addr_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    asx_socket_addr sa;
    memset(&sa, 0, sizeof(sa));
    sa.addr[0] = a;
    sa.addr[1] = b;
    sa.addr[2] = c;
    sa.addr[3] = d;
    sa.port = port;
    sa.family = ASX_AF_INET4;
    return sa;
}

asx_socket_addr asx_socket_addr_loopback(uint16_t port) {
    return asx_socket_addr_ipv4(127, 0, 0, 1, port);
}

int asx_socket_addr_eq(const asx_socket_addr *a, const asx_socket_addr *b) {
    if (a == NULL || b == NULL) return 0;
    if (a->family != b->family) return 0;
    if (a->port != b->port) return 0;
    if (a->family == ASX_AF_INET4) { return memcmp(a->addr, b->addr, 4) == 0; }
    return memcmp(a->addr, b->addr, 16) == 0;
}

/* ------------------------------------------------------------------ */
/* TCP listener arena                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_socket_addr addr;
    uint32_t generation;
    int alive;
} tcp_listener_slot;

static tcp_listener_slot g_listeners[ASX_MAX_TCP_LISTENERS];
static uint32_t g_listener_count;

static uint32_t next_gen(uint32_t g) {
    g++;
    return g == 0 ? 1 : g;
}

static tcp_listener_slot *listener_lookup(asx_tcp_listener h) {
    tcp_listener_slot *s;
    if (h.slot >= ASX_MAX_TCP_LISTENERS) return NULL;
    s = &g_listeners[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

asx_status asx_tcp_listener_bind(asx_tcp_listener *out, const asx_socket_addr *addr) {
    uint32_t idx;
    tcp_listener_slot *s;

    if (out == NULL || addr == NULL) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0; idx < ASX_MAX_TCP_LISTENERS; idx++) {
        if (!g_listeners[idx].alive) break;
    }
    if (idx >= ASX_MAX_TCP_LISTENERS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_listeners[idx];
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->addr = *addr;

    if (idx >= g_listener_count) g_listener_count = idx + 1;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_tcp_listener_poll_accept(asx_tcp_listener listener, asx_tcp_stream *out,
                                        asx_socket_addr *peer_addr) {
    tcp_listener_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    s = listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Walking skeleton: no real accept */
    (void)peer_addr;
    return ASX_E_PENDING;
}

asx_status asx_tcp_listener_close(asx_tcp_listener listener) {
    tcp_listener_slot *s = listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->alive = 0;
    return ASX_OK;
}

asx_status asx_tcp_listener_local_addr(asx_tcp_listener listener, asx_socket_addr *out) {
    tcp_listener_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = s->addr;
    return ASX_OK;
}

int asx_tcp_listener_is_alive(asx_tcp_listener listener) {
    return listener_lookup(listener) != NULL;
}

/* ------------------------------------------------------------------ */
/* TCP stream arena                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_socket_addr peer;
    uint32_t generation;
    int alive;
} tcp_stream_slot;

static tcp_stream_slot g_streams[ASX_MAX_TCP_STREAMS];
static uint32_t g_stream_count;

static tcp_stream_slot *stream_lookup(asx_tcp_stream h) {
    tcp_stream_slot *s;
    if (h.slot >= ASX_MAX_TCP_STREAMS) return NULL;
    s = &g_streams[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

asx_status asx_tcp_connect(asx_tcp_stream *out, const asx_socket_addr *addr) {
    uint32_t idx;
    tcp_stream_slot *s;

    if (out == NULL || addr == NULL) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0; idx < ASX_MAX_TCP_STREAMS; idx++) {
        if (!g_streams[idx].alive) break;
    }
    if (idx >= ASX_MAX_TCP_STREAMS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_streams[idx];
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->peer = *addr;

    if (idx >= g_stream_count) g_stream_count = idx + 1;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_tcp_stream_poll_read(asx_tcp_stream stream, asx_buf_mut *dst, uint32_t *bytes_read) {
    tcp_stream_slot *s;
    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;

    s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    *bytes_read = 0;
    return ASX_E_PENDING; /* ghost: no data */
}

asx_status asx_tcp_stream_poll_write(asx_tcp_stream stream, const asx_buf *src,
                                     uint32_t *bytes_written) {
    tcp_stream_slot *s;
    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;

    s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    *bytes_written = 0;
    return ASX_E_PENDING; /* ghost: cannot write */
}

asx_status asx_tcp_stream_close(asx_tcp_stream stream) {
    tcp_stream_slot *s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->alive = 0;
    return ASX_OK;
}

asx_status asx_tcp_stream_peer_addr(asx_tcp_stream stream, asx_socket_addr *out) {
    tcp_stream_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = s->peer;
    return ASX_OK;
}

int asx_tcp_stream_is_alive(asx_tcp_stream stream) { return stream_lookup(stream) != NULL; }

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_net_reset(void) {
    memset(g_listeners, 0, sizeof(g_listeners));
    g_listener_count = 0;
    memset(g_streams, 0, sizeof(g_streams));
    g_stream_count = 0;
}
