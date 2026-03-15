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

static void copy_addr16(uint8_t dst[16], const uint8_t src[16]) {
    uint32_t i;
    for (i = 0; i < 16u; i++) dst[i] = src[i];
}

static int parse_ipv4_host(const char *host, uint8_t out[4]) {
    uint32_t part = 0u;
    uint32_t parts_seen = 0u;
    int have_digit = 0;

    if (host == NULL || *host == '\0') return 0;
    for (; *host != '\0'; host++) {
        char ch = *host;
        if (ch >= '0' && ch <= '9') {
            have_digit = 1;
            part = (part * 10u) + (uint32_t)(ch - '0');
            if (part > 255u) return 0;
            continue;
        }
        if (ch != '.' || !have_digit || parts_seen >= 3u) return 0;
        out[parts_seen++] = (uint8_t)part;
        part = 0u;
        have_digit = 0;
    }
    if (!have_digit || parts_seen != 3u) return 0;
    out[3] = (uint8_t)part;
    return 1;
}

static asx_status net_require_channel_cx(const asx_cx *cx) {
    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx)) return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_CHANNEL)) return ASX_E_PERMISSION_DENIED;
    return ASX_OK;
}

static asx_status net_poll_checkpoint(asx_cx *cx) {
    asx_status st = net_require_channel_cx(cx);
    if (st != ASX_OK) return st;
    return asx_cx_checkpoint(cx);
}

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

asx_socket_addr asx_socket_addr_ipv6(const uint8_t addr[16], uint16_t port) {
    asx_socket_addr sa;
    memset(&sa, 0, sizeof(sa));
    if (addr != NULL) copy_addr16(sa.addr, addr);
    sa.port = port;
    sa.family = ASX_AF_INET6;
    return sa;
}

asx_socket_addr asx_socket_addr_ipv6_loopback(uint16_t port) {
    uint8_t addr[16];
    memset(addr, 0, sizeof(addr));
    addr[15] = 1u;
    return asx_socket_addr_ipv6(addr, port);
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
    return asx_tcp_listener_bind_with_cx(out, addr, NULL);
}

asx_status asx_tcp_listener_bind_with_cx(asx_tcp_listener *out, const asx_socket_addr *addr,
                                         const asx_cx *cx) {
    uint32_t idx;
    tcp_listener_slot *s;

    if (out == NULL || addr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_require_channel_cx(cx);
        if (st != ASX_OK) return st;
    }

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
    return asx_tcp_listener_poll_accept_with_cx(listener, out, peer_addr, NULL);
}

asx_status asx_tcp_listener_poll_accept_with_cx(asx_tcp_listener listener, asx_tcp_stream *out,
                                                asx_socket_addr *peer_addr, asx_cx *cx) {
    tcp_listener_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    if (cx != NULL) {
        asx_status st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

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
    return asx_tcp_connect_with_cx(out, addr, NULL);
}

asx_status asx_tcp_connect_with_cx(asx_tcp_stream *out, const asx_socket_addr *addr,
                                   const asx_cx *cx) {
    uint32_t idx;
    tcp_stream_slot *s;

    if (out == NULL || addr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_require_channel_cx(cx);
        if (st != ASX_OK) return st;
    }

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
    return asx_tcp_stream_poll_read_with_cx(stream, dst, bytes_read, NULL);
}

asx_status asx_tcp_stream_poll_read_with_cx(asx_tcp_stream stream, asx_buf_mut *dst,
                                            uint32_t *bytes_read, asx_cx *cx) {
    tcp_stream_slot *s;
    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;

    if (cx != NULL) {
        asx_status st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    *bytes_read = 0;
    return ASX_E_PENDING; /* ghost: no data */
}

asx_status asx_tcp_stream_poll_write(asx_tcp_stream stream, const asx_buf *src,
                                     uint32_t *bytes_written) {
    return asx_tcp_stream_poll_write_with_cx(stream, src, bytes_written, NULL);
}

asx_status asx_tcp_stream_poll_write_with_cx(asx_tcp_stream stream, const asx_buf *src,
                                             uint32_t *bytes_written, asx_cx *cx) {
    tcp_stream_slot *s;
    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;

    if (cx != NULL) {
        asx_status st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

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
/* UDP socket arena                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_socket_addr local;
    asx_socket_addr peer;
    uint32_t generation;
    int alive;
    int has_peer;
} udp_socket_slot;

static udp_socket_slot g_udp_sockets[ASX_MAX_UDP_SOCKETS];

static udp_socket_slot *udp_lookup(asx_udp_socket h) {
    udp_socket_slot *s;
    if (h.slot >= ASX_MAX_UDP_SOCKETS) return NULL;
    s = &g_udp_sockets[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

asx_status asx_udp_bind(asx_udp_socket *out, const asx_socket_addr *addr) {
    return asx_udp_bind_with_cx(out, addr, NULL);
}

asx_status asx_udp_bind_with_cx(asx_udp_socket *out, const asx_socket_addr *addr,
                                const asx_cx *cx) {
    uint32_t idx;
    udp_socket_slot *s;

    if (out == NULL || addr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_require_channel_cx(cx);
        if (st != ASX_OK) return st;
    }

    for (idx = 0; idx < ASX_MAX_UDP_SOCKETS; idx++) {
        if (!g_udp_sockets[idx].alive) break;
    }
    if (idx >= ASX_MAX_UDP_SOCKETS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_udp_sockets[idx];
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->local = *addr;
    memset(&s->peer, 0, sizeof(s->peer));
    s->has_peer = 0;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_udp_connect(asx_udp_socket socket, const asx_socket_addr *peer_addr) {
    return asx_udp_connect_with_cx(socket, peer_addr, NULL);
}

asx_status asx_udp_connect_with_cx(asx_udp_socket socket, const asx_socket_addr *peer_addr,
                                   const asx_cx *cx) {
    udp_socket_slot *s = udp_lookup(socket);
    if (peer_addr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_require_channel_cx(cx);
        if (st != ASX_OK) return st;
    }
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->peer = *peer_addr;
    s->has_peer = 1;
    return ASX_OK;
}

asx_status asx_udp_poll_send(asx_udp_socket socket, const asx_buf *src, uint32_t *bytes_written,
                             const asx_socket_addr *to) {
    return asx_udp_poll_send_with_cx(socket, src, bytes_written, to, NULL);
}

asx_status asx_udp_poll_send_with_cx(asx_udp_socket socket, const asx_buf *src,
                                     uint32_t *bytes_written, const asx_socket_addr *to,
                                     asx_cx *cx) {
    udp_socket_slot *s;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (to == NULL && !s->has_peer) return ASX_E_INVALID_ARGUMENT;

    *bytes_written = 0u;
    return ASX_E_PENDING;
}

asx_status asx_udp_poll_recv(asx_udp_socket socket, asx_buf_mut *dst, uint32_t *bytes_read,
                             asx_socket_addr *from) {
    return asx_udp_poll_recv_with_cx(socket, dst, bytes_read, from, NULL);
}

asx_status asx_udp_poll_recv_with_cx(asx_udp_socket socket, asx_buf_mut *dst, uint32_t *bytes_read,
                                     asx_socket_addr *from, asx_cx *cx) {
    udp_socket_slot *s;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (from != NULL) {
        if (s->has_peer) {
            *from = s->peer;
        } else {
            memset(from, 0, sizeof(*from));
        }
    }
    *bytes_read = 0u;
    return ASX_E_PENDING;
}

asx_status asx_udp_close(asx_udp_socket socket) {
    udp_socket_slot *s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->alive = 0;
    s->has_peer = 0;
    return ASX_OK;
}

asx_status asx_udp_local_addr(asx_udp_socket socket, asx_socket_addr *out) {
    udp_socket_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = s->local;
    return ASX_OK;
}

asx_status asx_udp_peer_addr(asx_udp_socket socket, asx_socket_addr *out) {
    udp_socket_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = udp_lookup(socket);
    if (s == NULL || !s->has_peer) return ASX_E_INVALID_ARGUMENT;
    *out = s->peer;
    return ASX_OK;
}

int asx_udp_is_alive(asx_udp_socket socket) { return udp_lookup(socket) != NULL; }

/* ------------------------------------------------------------------ */
/* Resolve / happy-eyeballs helpers                                    */
/* ------------------------------------------------------------------ */

void asx_resolve_options_init(asx_resolve_options *out, uint16_t port) {
    if (out == NULL) return;
    out->port = port;
    out->preferred_family = ASX_AF_INET6;
    out->allow_ipv4 = 1u;
    out->allow_ipv6 = 1u;
}

asx_status asx_happy_eyeballs_order(asx_resolve_result *out, const asx_resolve_result *in,
                                    asx_addr_family preferred_family) {
    uint32_t pass;
    uint32_t i;

    if (out == NULL || in == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    for (pass = 0u; pass < 2u; pass++) {
        asx_addr_family want =
            pass == 0u ? preferred_family
                       : (preferred_family == ASX_AF_INET6 ? ASX_AF_INET4 : ASX_AF_INET6);
        for (i = 0u; i < in->count; i++) {
            if (in->addrs[i].family == want && out->count < ASX_RESOLVE_MAX_RESULTS) {
                out->addrs[out->count++] = in->addrs[i];
            }
        }
    }
    return out->count == 0u ? ASX_E_NOT_FOUND : ASX_OK;
}

asx_status asx_resolve_host(asx_resolve_result *out, const char *host,
                            const asx_resolve_options *options) {
    asx_resolve_options opts;
    uint8_t ipv4[4];
    asx_resolve_result unordered;

    if (out == NULL || host == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    memset(&unordered, 0, sizeof(unordered));

    if (options != NULL) {
        opts = *options;
    } else {
        asx_resolve_options_init(&opts, 0u);
    }
    if (!opts.allow_ipv4 && !opts.allow_ipv6) return ASX_E_INVALID_ARGUMENT;

    if (strcmp(host, "localhost") == 0) {
        if (opts.allow_ipv6 && unordered.count < ASX_RESOLVE_MAX_RESULTS) {
            unordered.addrs[unordered.count++] = asx_socket_addr_ipv6_loopback(opts.port);
        }
        if (opts.allow_ipv4 && unordered.count < ASX_RESOLVE_MAX_RESULTS) {
            unordered.addrs[unordered.count++] = asx_socket_addr_loopback(opts.port);
        }
    } else if (strcmp(host, "::1") == 0) {
        if (!opts.allow_ipv6) return ASX_E_NOT_FOUND;
        unordered.addrs[unordered.count++] = asx_socket_addr_ipv6_loopback(opts.port);
    } else if (parse_ipv4_host(host, ipv4)) {
        if (!opts.allow_ipv4) return ASX_E_NOT_FOUND;
        unordered.addrs[unordered.count++] =
            asx_socket_addr_ipv4(ipv4[0], ipv4[1], ipv4[2], ipv4[3], opts.port);
    } else {
        return ASX_E_NOT_FOUND;
    }

    return asx_happy_eyeballs_order(out, &unordered, opts.preferred_family);
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_net_reset(void) {
    memset(g_listeners, 0, sizeof(g_listeners));
    g_listener_count = 0;
    memset(g_streams, 0, sizeof(g_streams));
    g_stream_count = 0;
    memset(g_udp_sockets, 0, sizeof(g_udp_sockets));
}
