/*
 * net.c — deterministic in-memory network types and socket primitives
 *
 * The portable core intentionally avoids OS sockets, but higher layers still
 * need a materially useful network substrate. This implementation provides a
 * deterministic loopback transport for bound listeners and UDP sockets while
 * preserving ghost-pending behavior for addresses outside the in-memory model.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/net.h>
#include <string.h>

#define ASX_NET_TCP_ACCEPT_QUEUE_DEPTH ASX_MAX_TCP_STREAMS
#define ASX_NET_UDP_QUEUE_DEPTH 4u

typedef struct {
    uint8_t bytes[ASX_BUF_CAPACITY];
    uint32_t len;
    asx_socket_addr from;
} udp_datagram;

static void copy_addr16(uint8_t dst[16], const uint8_t src[16]) {
    uint32_t i;
    for (i = 0; i < 16u; i++) dst[i] = src[i];
}

static int parse_ipv4_host(const char *host, uint8_t out[4]) {
    uint32_t part;
    uint32_t parts_seen;
    int have_digit;

    part = 0u;
    parts_seen = 0u;
    have_digit = 0;

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

static int resolve_options_eq(const asx_resolve_options *a, const asx_resolve_options *b) {
    if (a == NULL || b == NULL) return 0;
    return a->port == b->port && a->preferred_family == b->preferred_family &&
           a->allow_ipv4 == b->allow_ipv4 && a->allow_ipv6 == b->allow_ipv6;
}

static size_t bounded_host_len(const char *host) {
    size_t len;

    if (host == NULL) return 0u;
    for (len = 0u; len < ASX_RESOLVER_HOST_CAPACITY; len++) {
        if (host[len] == '\0') return len;
    }
    return ASX_RESOLVER_HOST_CAPACITY;
}

static int resolver_host_eq(const char *a, const char *b) {
    size_t len_a;
    size_t len_b;

    if (a == NULL || b == NULL) return 0;
    len_a = bounded_host_len(a);
    len_b = bounded_host_len(b);
    if (len_a != len_b || len_a == ASX_RESOLVER_HOST_CAPACITY) return 0;
    return memcmp(a, b, len_a + 1u) == 0;
}

static asx_status net_require_channel_cx(const asx_cx *cx) {
    if (cx == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_cx_is_valid(cx)) return ASX_E_INVALID_STATE;
    if (!asx_cx_has_cap(cx, ASX_CAP_CHANNEL)) return ASX_E_PERMISSION_DENIED;
    return ASX_OK;
}

static asx_status net_poll_checkpoint(asx_cx *cx) {
    asx_status st;

    st = net_require_channel_cx(cx);
    if (st != ASX_OK) return st;
    return asx_cx_checkpoint(cx);
}

static int resolver_entry_matches(const asx_resolver_cache_entry *entry, const char *host,
                                  const asx_resolve_options *options) {
    if (entry == NULL || !entry->occupied || host == NULL || options == NULL) return 0;
    return resolver_host_eq(entry->host, host) && resolve_options_eq(&entry->options, options);
}

static asx_resolver_cache_entry *resolver_find_mut(asx_resolver *resolver, const char *host,
                                                   const asx_resolve_options *options) {
    uint32_t i;

    if (resolver == NULL) return NULL;
    for (i = 0u; i < ASX_RESOLVER_CACHE_CAPACITY; i++) {
        if (resolver_entry_matches(&resolver->entries[i], host, options))
            return &resolver->entries[i];
    }
    return NULL;
}

static const asx_resolver_cache_entry *resolver_find(const asx_resolver *resolver, const char *host,
                                                     const asx_resolve_options *options) {
    uint32_t i;

    if (resolver == NULL) return NULL;
    for (i = 0u; i < ASX_RESOLVER_CACHE_CAPACITY; i++) {
        if (resolver_entry_matches(&resolver->entries[i], host, options))
            return &resolver->entries[i];
    }
    return NULL;
}

static asx_status resolver_store(asx_resolver *resolver, const char *host,
                                 const asx_resolve_options *options,
                                 const asx_resolve_result *result, asx_status status) {
    asx_resolver_cache_entry *entry;
    size_t len;

    if (resolver == NULL || host == NULL || options == NULL) return ASX_E_INVALID_ARGUMENT;
    len = bounded_host_len(host);
    if (len == ASX_RESOLVER_HOST_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    entry = resolver_find_mut(resolver, host, options);
    if (entry == NULL) {
        entry = &resolver->entries[resolver->next_slot % ASX_RESOLVER_CACHE_CAPACITY];
        resolver->next_slot = (resolver->next_slot + 1u) % ASX_RESOLVER_CACHE_CAPACITY;
    }

    memset(entry, 0, sizeof(*entry));
    memcpy(entry->host, host, len + 1u);
    entry->options = *options;
    if (result != NULL) entry->result = *result;
    entry->status = status;
    entry->occupied = 1u;
    return ASX_OK;
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
    if (a->family == ASX_AF_INET4) return memcmp(a->addr, b->addr, 4u) == 0;
    return memcmp(a->addr, b->addr, 16u) == 0;
}

/* ------------------------------------------------------------------ */
/* TCP listener arena                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_socket_addr addr;
    asx_tcp_stream pending[ASX_NET_TCP_ACCEPT_QUEUE_DEPTH];
    uint32_t generation;
    uint32_t accept_head;
    uint32_t accept_count;
    int alive;
} tcp_listener_slot;

static tcp_listener_slot g_listeners[ASX_MAX_TCP_LISTENERS];
static uint32_t g_listener_count;

/* ------------------------------------------------------------------ */
/* TCP stream arena                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_socket_addr local;
    asx_socket_addr peer;
    asx_buf_mut inbox;
    uint32_t generation;
    uint32_t peer_slot;
    uint32_t peer_generation;
    int alive;
    int linked;
} tcp_stream_slot;

static tcp_stream_slot g_streams[ASX_MAX_TCP_STREAMS];
static uint32_t g_stream_count;

/* ------------------------------------------------------------------ */
/* UDP socket arena                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_socket_addr local;
    asx_socket_addr peer;
    udp_datagram queue[ASX_NET_UDP_QUEUE_DEPTH];
    uint32_t generation;
    uint32_t recv_head;
    uint32_t recv_count;
    int alive;
    int has_peer;
} udp_socket_slot;

static udp_socket_slot g_udp_sockets[ASX_MAX_UDP_SOCKETS];
static uint16_t g_next_ephemeral_port = 49152u;

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static asx_socket_addr net_make_loopback(asx_addr_family family, uint16_t port) {
    return family == ASX_AF_INET6 ? asx_socket_addr_ipv6_loopback(port)
                                  : asx_socket_addr_loopback(port);
}

static asx_socket_addr net_ephemeral_addr(asx_addr_family family) {
    uint16_t port;

    port = g_next_ephemeral_port++;
    if (g_next_ephemeral_port < 49152u) g_next_ephemeral_port = 49152u;
    return net_make_loopback(family, port);
}

static int net_is_loopback_addr(const asx_socket_addr *addr) {
    uint8_t i;

    if (addr == NULL) return 0;
    if (addr->family == ASX_AF_INET4) {
        return addr->addr[0] == 127u && addr->addr[1] == 0u && addr->addr[2] == 0u &&
               addr->addr[3] == 1u;
    }
    for (i = 0u; i < 15u; i++) {
        if (addr->addr[i] != 0u) return 0;
    }
    return addr->addr[15] == 1u;
}

static tcp_listener_slot *listener_lookup(asx_tcp_listener h) {
    tcp_listener_slot *s;

    if (h.slot >= ASX_MAX_TCP_LISTENERS) return NULL;
    s = &g_listeners[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static tcp_listener_slot *listener_find_by_addr(const asx_socket_addr *addr) {
    uint32_t idx;

    if (addr == NULL) return NULL;
    for (idx = 0u; idx < ASX_MAX_TCP_LISTENERS; idx++) {
        if (g_listeners[idx].alive && asx_socket_addr_eq(&g_listeners[idx].addr, addr)) {
            return &g_listeners[idx];
        }
    }
    return NULL;
}

static tcp_stream_slot *stream_lookup(asx_tcp_stream h) {
    tcp_stream_slot *s;

    if (h.slot >= ASX_MAX_TCP_STREAMS) return NULL;
    s = &g_streams[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static tcp_stream_slot *stream_linked_peer(const tcp_stream_slot *s) {
    tcp_stream_slot *peer;
    uint32_t self_slot;

    if (s == NULL || !s->linked || s->peer_slot >= ASX_MAX_TCP_STREAMS) return NULL;
    peer = &g_streams[s->peer_slot];
    if (!peer->alive) return NULL;
    if (peer->generation != s->peer_generation) return NULL;
    if (!peer->linked) return NULL;
    if (peer->peer_generation != s->generation) return NULL;
    self_slot = (uint32_t)(s - g_streams);
    if (peer->peer_slot != self_slot) return NULL;
    return peer;
}

static void stream_unlink_peer(tcp_stream_slot *s) {
    tcp_stream_slot *peer;

    if (s == NULL) return;
    peer = stream_linked_peer(s);
    if (peer != NULL) {
        peer->linked = 0;
        peer->peer_slot = 0u;
        peer->peer_generation = 0u;
    }
    s->linked = 0;
    s->peer_slot = 0u;
    s->peer_generation = 0u;
}

static asx_status stream_alloc(const asx_socket_addr *local, const asx_socket_addr *peer,
                               asx_tcp_stream *out, tcp_stream_slot **out_slot) {
    uint32_t idx;
    tcp_stream_slot *s;

    if (local == NULL || peer == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_TCP_STREAMS; idx++) {
        if (!g_streams[idx].alive) break;
    }
    if (idx >= ASX_MAX_TCP_STREAMS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_streams[idx];
    memset(s, 0, sizeof(*s));
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->local = *local;
    s->peer = *peer;
    asx_buf_mut_init(&s->inbox);

    if (idx >= g_stream_count) g_stream_count = idx + 1u;

    out->slot = idx;
    out->generation = s->generation;
    if (out_slot != NULL) *out_slot = s;
    return ASX_OK;
}

static asx_status listener_queue_accept(tcp_listener_slot *listener, asx_tcp_stream accepted) {
    uint32_t tail;

    if (listener == NULL) return ASX_E_INVALID_ARGUMENT;
    if (listener->accept_count >= ASX_NET_TCP_ACCEPT_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;

    tail = (listener->accept_head + listener->accept_count) % ASX_NET_TCP_ACCEPT_QUEUE_DEPTH;
    listener->pending[tail] = accepted;
    listener->accept_count++;
    return ASX_OK;
}

static udp_socket_slot *udp_lookup(asx_udp_socket h) {
    udp_socket_slot *s;

    if (h.slot >= ASX_MAX_UDP_SOCKETS) return NULL;
    s = &g_udp_sockets[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static asx_status udp_enqueue(udp_socket_slot *dst, const asx_socket_addr *from,
                              const asx_buf *src) {
    uint32_t tail;
    udp_datagram *packet;

    if (dst == NULL || from == NULL || src == NULL) return ASX_E_INVALID_ARGUMENT;
    if (src->len > ASX_BUF_CAPACITY) return ASX_E_BUFFER_TOO_SMALL;
    if (dst->recv_count >= ASX_NET_UDP_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;

    tail = (dst->recv_head + dst->recv_count) % ASX_NET_UDP_QUEUE_DEPTH;
    packet = &dst->queue[tail];
    memset(packet, 0, sizeof(*packet));
    packet->from = *from;
    packet->len = src->len;
    if (src->len > 0u) memcpy(packet->bytes, src->ptr, src->len);
    dst->recv_count++;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* TCP listener API                                                    */
/* ------------------------------------------------------------------ */

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

    for (idx = 0u; idx < ASX_MAX_TCP_LISTENERS; idx++) {
        if (!g_listeners[idx].alive) break;
    }
    if (idx >= ASX_MAX_TCP_LISTENERS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_listeners[idx];
    memset(s, 0, sizeof(*s));
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->addr = *addr;

    if (idx >= g_listener_count) g_listener_count = idx + 1u;

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
    tcp_stream_slot *accepted;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->accept_count == 0u) return ASX_E_PENDING;

    *out = s->pending[s->accept_head];
    s->accept_head = (s->accept_head + 1u) % ASX_NET_TCP_ACCEPT_QUEUE_DEPTH;
    s->accept_count--;

    if (peer_addr != NULL) {
        accepted = stream_lookup(*out);
        if (accepted != NULL) {
            *peer_addr = accepted->peer;
        } else {
            memset(peer_addr, 0, sizeof(*peer_addr));
        }
    }
    return ASX_OK;
}

asx_status asx_tcp_listener_close(asx_tcp_listener listener) {
    tcp_listener_slot *s;
    uint32_t i;

    s = listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0u; i < s->accept_count; i++) {
        uint32_t idx = (s->accept_head + i) % ASX_NET_TCP_ACCEPT_QUEUE_DEPTH;
        (void)asx_tcp_stream_close(s->pending[idx]);
    }

    memset(s, 0, sizeof(*s));
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
/* TCP stream API                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_tcp_connect(asx_tcp_stream *out, const asx_socket_addr *addr) {
    return asx_tcp_connect_with_cx(out, addr, NULL);
}

asx_status asx_tcp_connect_with_cx(asx_tcp_stream *out, const asx_socket_addr *addr,
                                   const asx_cx *cx) {
    asx_socket_addr local;
    asx_status st;
    tcp_listener_slot *listener;
    tcp_stream_slot *client;

    if (out == NULL || addr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        st = net_require_channel_cx(cx);
        if (st != ASX_OK) return st;
    }

    local = net_ephemeral_addr(addr->family);
    st = stream_alloc(&local, addr, out, &client);
    if (st != ASX_OK) return st;

    listener = listener_find_by_addr(addr);
    if (listener != NULL && net_is_loopback_addr(addr)) {
        asx_tcp_stream accepted_handle;
        tcp_stream_slot *accepted;

        st = stream_alloc(addr, &local, &accepted_handle, &accepted);
        if (st != ASX_OK) {
            memset(client, 0, sizeof(*client));
            return st;
        }

        client->linked = 1;
        client->peer_slot = accepted_handle.slot;
        client->peer_generation = accepted_handle.generation;
        accepted->linked = 1;
        accepted->peer_slot = out->slot;
        accepted->peer_generation = out->generation;

        st = listener_queue_accept(listener, accepted_handle);
        if (st != ASX_OK) {
            memset(accepted, 0, sizeof(*accepted));
            memset(client, 0, sizeof(*client));
            return st;
        }
    }

    return ASX_OK;
}

asx_status asx_tcp_stream_poll_read(asx_tcp_stream stream, asx_buf_mut *dst, uint32_t *bytes_read) {
    return asx_tcp_stream_poll_read_with_cx(stream, dst, bytes_read, NULL);
}

asx_status asx_tcp_stream_poll_read_with_cx(asx_tcp_stream stream, asx_buf_mut *dst,
                                            uint32_t *bytes_read, asx_cx *cx) {
    tcp_stream_slot *s;
    asx_buf readable;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = stream_lookup(stream);
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

asx_status asx_tcp_stream_poll_write(asx_tcp_stream stream, const asx_buf *src,
                                     uint32_t *bytes_written) {
    return asx_tcp_stream_poll_write_with_cx(stream, src, bytes_written, NULL);
}

asx_status asx_tcp_stream_poll_write_with_cx(asx_tcp_stream stream, const asx_buf *src,
                                             uint32_t *bytes_written, asx_cx *cx) {
    tcp_stream_slot *s;
    tcp_stream_slot *peer;
    asx_status st;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    peer = stream_linked_peer(s);
    if (peer == NULL) {
        *bytes_written = 0u;
        return ASX_E_PENDING;
    }
    if (src->len > asx_buf_mut_writable(&peer->inbox)) return ASX_E_WOULD_BLOCK;

    st = asx_buf_mut_put(&peer->inbox, src->ptr, src->len);
    if (st != ASX_OK) return st;

    *bytes_written = src->len;
    return ASX_OK;
}

asx_status asx_tcp_stream_close(asx_tcp_stream stream) {
    tcp_stream_slot *s;

    s = stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    stream_unlink_peer(s);
    memset(s, 0, sizeof(*s));
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
/* UDP socket API                                                      */
/* ------------------------------------------------------------------ */

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

    for (idx = 0u; idx < ASX_MAX_UDP_SOCKETS; idx++) {
        if (!g_udp_sockets[idx].alive) break;
    }
    if (idx >= ASX_MAX_UDP_SOCKETS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_udp_sockets[idx];
    memset(s, 0, sizeof(*s));
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->local = *addr;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_udp_connect(asx_udp_socket socket, const asx_socket_addr *peer_addr) {
    return asx_udp_connect_with_cx(socket, peer_addr, NULL);
}

asx_status asx_udp_connect_with_cx(asx_udp_socket socket, const asx_socket_addr *peer_addr,
                                   const asx_cx *cx) {
    udp_socket_slot *s;

    if (peer_addr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        asx_status st = net_require_channel_cx(cx);
        if (st != ASX_OK) return st;
    }

    s = udp_lookup(socket);
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
    const asx_socket_addr *dest;
    uint32_t idx;
    asx_status st;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (to == NULL && !s->has_peer) return ASX_E_INVALID_ARGUMENT;

    dest = to != NULL ? to : &s->peer;
    for (idx = 0u; idx < ASX_MAX_UDP_SOCKETS; idx++) {
        if (g_udp_sockets[idx].alive && asx_socket_addr_eq(&g_udp_sockets[idx].local, dest)) {
            st = udp_enqueue(&g_udp_sockets[idx], &s->local, src);
            if (st != ASX_OK) return st;
            *bytes_written = src->len;
            return ASX_OK;
        }
    }

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
    udp_datagram *packet;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cx != NULL) {
        st = net_poll_checkpoint(cx);
        if (st != ASX_OK) return st;
    }

    s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->recv_count == 0u) {
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
    if (asx_buf_mut_writable(dst) == 0u) return ASX_E_BUFFER_TOO_SMALL;

    packet = &s->queue[s->recv_head];
    to_copy = packet->len;
    if (to_copy > asx_buf_mut_writable(dst)) to_copy = asx_buf_mut_writable(dst);
    st = asx_buf_mut_put(dst, packet->bytes, to_copy);
    if (st != ASX_OK) return st;

    if (from != NULL) *from = packet->from;
    *bytes_read = to_copy;
    s->recv_head = (s->recv_head + 1u) % ASX_NET_UDP_QUEUE_DEPTH;
    s->recv_count--;
    return ASX_OK;
}

asx_status asx_udp_close(asx_udp_socket socket) {
    udp_socket_slot *s;

    s = udp_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(s, 0, sizeof(*s));
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
        asx_addr_family want;

        want = pass == 0u ? preferred_family
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

void asx_resolver_init(asx_resolver *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
}

void asx_resolver_reset(asx_resolver *resolver) { asx_resolver_init(resolver); }

uint32_t asx_resolver_cached_count(const asx_resolver *resolver) {
    uint32_t i;
    uint32_t count;

    if (resolver == NULL) return 0u;
    count = 0u;
    for (i = 0u; i < ASX_RESOLVER_CACHE_CAPACITY; i++) {
        if (resolver->entries[i].occupied) count++;
    }
    return count;
}

asx_status asx_resolver_lookup(asx_resolver *resolver, const char *host,
                               const asx_resolve_options *options, asx_resolve_result *out,
                               uint8_t *cache_hit) {
    asx_resolve_options opts;
    const asx_resolver_cache_entry *entry;
    asx_status st;

    if (resolver == NULL || host == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (options != NULL) {
        opts = *options;
    } else {
        asx_resolve_options_init(&opts, 0u);
    }

    entry = resolver_find(resolver, host, &opts);
    if (entry != NULL) {
        *out = entry->result;
        if (cache_hit != NULL) *cache_hit = 1u;
        return entry->status;
    }

    st = asx_resolve_host(out, host, &opts);
    if (cache_hit != NULL) *cache_hit = 0u;
    (void)resolver_store(resolver, host, &opts, out, st);
    return st;
}

void asx_resolver_invalidate(asx_resolver *resolver, const char *host) {
    uint32_t i;

    if (resolver == NULL || host == NULL) return;
    for (i = 0u; i < ASX_RESOLVER_CACHE_CAPACITY; i++) {
        if (resolver->entries[i].occupied && resolver_host_eq(resolver->entries[i].host, host)) {
            memset(&resolver->entries[i], 0, sizeof(resolver->entries[i]));
        }
    }
}

asx_status asx_tcp_connect_host(asx_tcp_stream *out, asx_resolver *resolver, const char *host,
                                const asx_resolve_options *options, asx_socket_addr *selected_addr,
                                uint8_t *cache_hit) {
    asx_resolve_result resolved;
    asx_status st;

    if (out == NULL || resolver == NULL || host == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(&resolved, 0, sizeof(resolved));

    st = asx_resolver_lookup(resolver, host, options, &resolved, cache_hit);
    if (st != ASX_OK) return st;
    if (resolved.count == 0u) return ASX_E_NOT_FOUND;
    if (selected_addr != NULL) *selected_addr = resolved.addrs[0];
    return asx_tcp_connect(out, &resolved.addrs[0]);
}

asx_status asx_udp_connect_host(asx_udp_socket socket, asx_resolver *resolver, const char *host,
                                const asx_resolve_options *options, asx_socket_addr *selected_addr,
                                uint8_t *cache_hit) {
    asx_resolve_result resolved;
    asx_status st;

    if (resolver == NULL || host == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(&resolved, 0, sizeof(resolved));

    st = asx_resolver_lookup(resolver, host, options, &resolved, cache_hit);
    if (st != ASX_OK) return st;
    if (resolved.count == 0u) return ASX_E_NOT_FOUND;
    if (selected_addr != NULL) *selected_addr = resolved.addrs[0];
    return asx_udp_connect(socket, &resolved.addrs[0]);
}

/* ------------------------------------------------------------------ */
/* Unix-domain address                                                 */
/* ------------------------------------------------------------------ */

static size_t bounded_path_len(const char *path) {
    size_t len;

    if (path == NULL) return 0u;
    for (len = 0u; len < ASX_UNIX_PATH_MAX; len++) {
        if (path[len] == '\0') return len;
    }
    return ASX_UNIX_PATH_MAX;
}

asx_status asx_unix_addr_from_path(asx_unix_addr *out, const char *path) {
    size_t len;

    if (out == NULL || path == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    len = bounded_path_len(path);
    if (len == 0u || len >= ASX_UNIX_PATH_MAX) return ASX_E_INVALID_ARGUMENT;
    memcpy(out->path, path, len + 1u);
    out->has_path = 1u;
    return ASX_OK;
}

int asx_unix_addr_eq(const asx_unix_addr *a, const asx_unix_addr *b) {
    if (a == NULL || b == NULL) return 0;
    if (!a->has_path || !b->has_path) return 0;
    return strcmp(a->path, b->path) == 0;
}

/* ------------------------------------------------------------------ */
/* Unix-domain listener arena                                          */
/* ------------------------------------------------------------------ */

#define ASX_UNIX_ACCEPT_QUEUE_DEPTH ASX_MAX_UNIX_STREAMS

typedef struct {
    asx_unix_addr addr;
    asx_unix_stream pending[ASX_UNIX_ACCEPT_QUEUE_DEPTH];
    uint32_t generation;
    uint32_t accept_head;
    uint32_t accept_count;
    int alive;
} unix_listener_slot;

static unix_listener_slot g_unix_listeners[ASX_MAX_UNIX_LISTENERS];

/* ------------------------------------------------------------------ */
/* Unix-domain stream arena                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_unix_addr local;
    asx_unix_addr peer;
    asx_buf_mut inbox;
    asx_ancillary anc_inbox;
    uint32_t generation;
    uint32_t peer_slot;
    uint32_t peer_generation;
    int alive;
    int linked;
} unix_stream_slot;

static unix_stream_slot g_unix_streams[ASX_MAX_UNIX_STREAMS];

/* ------------------------------------------------------------------ */
/* Unix-domain datagram arena                                          */
/* ------------------------------------------------------------------ */

#define ASX_UNIX_DGRAM_QUEUE_DEPTH 4u

typedef struct {
    uint8_t bytes[ASX_BUF_CAPACITY];
    uint32_t len;
    asx_unix_addr from;
} unix_dgram_packet;

typedef struct {
    asx_unix_addr local;
    unix_dgram_packet queue[ASX_UNIX_DGRAM_QUEUE_DEPTH];
    uint32_t generation;
    uint32_t recv_head;
    uint32_t recv_count;
    int alive;
} unix_dgram_slot;

static unix_dgram_slot g_unix_dgrams[ASX_MAX_UNIX_DGRAM_SOCKETS];

/* ------------------------------------------------------------------ */
/* Unix-domain helpers                                                 */
/* ------------------------------------------------------------------ */

static unix_listener_slot *unix_listener_lookup(asx_unix_listener h) {
    unix_listener_slot *s;

    if (h.slot >= ASX_MAX_UNIX_LISTENERS) return NULL;
    s = &g_unix_listeners[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static unix_listener_slot *unix_listener_find_by_addr(const asx_unix_addr *addr) {
    uint32_t idx;

    if (addr == NULL || !addr->has_path) return NULL;
    for (idx = 0u; idx < ASX_MAX_UNIX_LISTENERS; idx++) {
        if (g_unix_listeners[idx].alive && asx_unix_addr_eq(&g_unix_listeners[idx].addr, addr)) {
            return &g_unix_listeners[idx];
        }
    }
    return NULL;
}

static unix_stream_slot *unix_stream_lookup(asx_unix_stream h) {
    unix_stream_slot *s;

    if (h.slot >= ASX_MAX_UNIX_STREAMS) return NULL;
    s = &g_unix_streams[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static unix_stream_slot *unix_stream_linked_peer(const unix_stream_slot *s) {
    unix_stream_slot *peer;
    uint32_t self_slot;

    if (s == NULL || !s->linked || s->peer_slot >= ASX_MAX_UNIX_STREAMS) return NULL;
    peer = &g_unix_streams[s->peer_slot];
    if (!peer->alive) return NULL;
    if (peer->generation != s->peer_generation) return NULL;
    if (!peer->linked) return NULL;
    if (peer->peer_generation != s->generation) return NULL;
    self_slot = (uint32_t)(s - g_unix_streams);
    if (peer->peer_slot != self_slot) return NULL;
    return peer;
}

static void unix_stream_unlink_peer(unix_stream_slot *s) {
    unix_stream_slot *peer;

    if (s == NULL) return;
    peer = unix_stream_linked_peer(s);
    if (peer != NULL) {
        peer->linked = 0;
        peer->peer_slot = 0u;
        peer->peer_generation = 0u;
    }
    s->linked = 0;
    s->peer_slot = 0u;
    s->peer_generation = 0u;
}

static asx_status unix_stream_alloc(const asx_unix_addr *local, const asx_unix_addr *peer,
                                     asx_unix_stream *out, unix_stream_slot **out_slot) {
    uint32_t idx;
    unix_stream_slot *s;

    if (local == NULL || peer == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_UNIX_STREAMS; idx++) {
        if (!g_unix_streams[idx].alive) break;
    }
    if (idx >= ASX_MAX_UNIX_STREAMS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_unix_streams[idx];
    memset(s, 0, sizeof(*s));
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->local = *local;
    s->peer = *peer;
    asx_buf_mut_init(&s->inbox);
    asx_ancillary_init(&s->anc_inbox);

    out->slot = idx;
    out->generation = s->generation;
    if (out_slot != NULL) *out_slot = s;
    return ASX_OK;
}

static unix_dgram_slot *unix_dgram_lookup(asx_unix_dgram h) {
    unix_dgram_slot *s;

    if (h.slot >= ASX_MAX_UNIX_DGRAM_SOCKETS) return NULL;
    s = &g_unix_dgrams[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* Unix-domain listener API                                            */
/* ------------------------------------------------------------------ */

asx_status asx_unix_listener_bind(asx_unix_listener *out, const asx_unix_addr *addr) {
    uint32_t idx;
    unix_listener_slot *s;

    if (out == NULL || addr == NULL || !addr->has_path) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_UNIX_LISTENERS; idx++) {
        if (!g_unix_listeners[idx].alive) break;
    }
    if (idx >= ASX_MAX_UNIX_LISTENERS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_unix_listeners[idx];
    memset(s, 0, sizeof(*s));
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->addr = *addr;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_unix_listener_poll_accept(asx_unix_listener listener, asx_unix_stream *out) {
    unix_listener_slot *s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = unix_listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->accept_count == 0u) return ASX_E_PENDING;

    *out = s->pending[s->accept_head];
    s->accept_head = (s->accept_head + 1u) % ASX_UNIX_ACCEPT_QUEUE_DEPTH;
    s->accept_count--;
    return ASX_OK;
}

asx_status asx_unix_listener_close(asx_unix_listener listener) {
    unix_listener_slot *s;
    uint32_t i;

    s = unix_listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0u; i < s->accept_count; i++) {
        uint32_t idx = (s->accept_head + i) % ASX_UNIX_ACCEPT_QUEUE_DEPTH;
        (void)asx_unix_stream_close(s->pending[idx]);
    }

    memset(s, 0, sizeof(*s));
    return ASX_OK;
}

asx_status asx_unix_listener_local_addr(asx_unix_listener listener, asx_unix_addr *out) {
    unix_listener_slot *s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = unix_listener_lookup(listener);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = s->addr;
    return ASX_OK;
}

int asx_unix_listener_is_alive(asx_unix_listener listener) {
    return unix_listener_lookup(listener) != NULL;
}

/* ------------------------------------------------------------------ */
/* Unix-domain stream API                                              */
/* ------------------------------------------------------------------ */

asx_status asx_unix_connect(asx_unix_stream *out, const asx_unix_addr *addr) {
    asx_unix_addr client_addr;
    asx_status st;
    unix_listener_slot *listener;
    unix_stream_slot *client;

    if (out == NULL || addr == NULL || !addr->has_path) return ASX_E_INVALID_ARGUMENT;

    memset(&client_addr, 0, sizeof(client_addr));

    st = unix_stream_alloc(&client_addr, addr, out, &client);
    if (st != ASX_OK) return st;

    listener = unix_listener_find_by_addr(addr);
    if (listener != NULL) {
        asx_unix_stream accepted_handle;
        unix_stream_slot *accepted;
        uint32_t tail;

        st = unix_stream_alloc(addr, &client_addr, &accepted_handle, &accepted);
        if (st != ASX_OK) {
            memset(client, 0, sizeof(*client));
            return st;
        }

        client->linked = 1;
        client->peer_slot = accepted_handle.slot;
        client->peer_generation = accepted_handle.generation;
        accepted->linked = 1;
        accepted->peer_slot = out->slot;
        accepted->peer_generation = out->generation;

        if (listener->accept_count >= ASX_UNIX_ACCEPT_QUEUE_DEPTH) {
            memset(accepted, 0, sizeof(*accepted));
            memset(client, 0, sizeof(*client));
            return ASX_E_WOULD_BLOCK;
        }
        tail = (listener->accept_head + listener->accept_count) % ASX_UNIX_ACCEPT_QUEUE_DEPTH;
        listener->pending[tail] = accepted_handle;
        listener->accept_count++;
    }

    return ASX_OK;
}

asx_status asx_unix_stream_poll_read(asx_unix_stream stream, asx_buf_mut *dst,
                                      uint32_t *bytes_read) {
    unix_stream_slot *s;
    asx_buf readable;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;

    s = unix_stream_lookup(stream);
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

asx_status asx_unix_stream_poll_write(asx_unix_stream stream, const asx_buf *src,
                                       uint32_t *bytes_written) {
    unix_stream_slot *s;
    unix_stream_slot *peer;
    asx_status st;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;

    s = unix_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    peer = unix_stream_linked_peer(s);
    if (peer == NULL) {
        *bytes_written = 0u;
        return ASX_E_PENDING;
    }
    if (src->len > asx_buf_mut_writable(&peer->inbox)) return ASX_E_WOULD_BLOCK;

    st = asx_buf_mut_put(&peer->inbox, src->ptr, src->len);
    if (st != ASX_OK) return st;

    *bytes_written = src->len;
    return ASX_OK;
}

asx_status asx_unix_stream_close(asx_unix_stream stream) {
    unix_stream_slot *s;

    s = unix_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    unix_stream_unlink_peer(s);
    memset(s, 0, sizeof(*s));
    return ASX_OK;
}

int asx_unix_stream_is_alive(asx_unix_stream stream) {
    return unix_stream_lookup(stream) != NULL;
}

/* ------------------------------------------------------------------ */
/* Unix-domain datagram API                                            */
/* ------------------------------------------------------------------ */

asx_status asx_unix_dgram_bind(asx_unix_dgram *out, const asx_unix_addr *addr) {
    uint32_t idx;
    unix_dgram_slot *s;

    if (out == NULL || addr == NULL || !addr->has_path) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_UNIX_DGRAM_SOCKETS; idx++) {
        if (!g_unix_dgrams[idx].alive) break;
    }
    if (idx >= ASX_MAX_UNIX_DGRAM_SOCKETS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_unix_dgrams[idx];
    memset(s, 0, sizeof(*s));
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->local = *addr;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_unix_dgram_poll_send(asx_unix_dgram socket, const asx_buf *src,
                                     uint32_t *bytes_written, const asx_unix_addr *to) {
    unix_dgram_slot *sender;
    unix_dgram_slot *dest;
    uint32_t idx;
    uint32_t tail;
    unix_dgram_packet *packet;

    if (src == NULL || bytes_written == NULL || to == NULL || !to->has_path)
        return ASX_E_INVALID_ARGUMENT;
    if (src->len > ASX_BUF_CAPACITY) return ASX_E_BUFFER_TOO_SMALL;

    sender = unix_dgram_lookup(socket);
    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;

    dest = NULL;
    for (idx = 0u; idx < ASX_MAX_UNIX_DGRAM_SOCKETS; idx++) {
        if (g_unix_dgrams[idx].alive && asx_unix_addr_eq(&g_unix_dgrams[idx].local, to)) {
            dest = &g_unix_dgrams[idx];
            break;
        }
    }
    if (dest == NULL) {
        *bytes_written = 0u;
        return ASX_E_PENDING;
    }
    if (dest->recv_count >= ASX_UNIX_DGRAM_QUEUE_DEPTH) return ASX_E_WOULD_BLOCK;

    tail = (dest->recv_head + dest->recv_count) % ASX_UNIX_DGRAM_QUEUE_DEPTH;
    packet = &dest->queue[tail];
    memset(packet, 0, sizeof(*packet));
    packet->from = sender->local;
    packet->len = src->len;
    if (src->len > 0u) memcpy(packet->bytes, src->ptr, src->len);
    dest->recv_count++;

    *bytes_written = src->len;
    return ASX_OK;
}

asx_status asx_unix_dgram_poll_recv(asx_unix_dgram socket, asx_buf_mut *dst, uint32_t *bytes_read,
                                     asx_unix_addr *from) {
    unix_dgram_slot *s;
    unix_dgram_packet *packet;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;

    s = unix_dgram_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->recv_count == 0u) {
        *bytes_read = 0u;
        return ASX_E_PENDING;
    }
    if (asx_buf_mut_writable(dst) == 0u) return ASX_E_BUFFER_TOO_SMALL;

    packet = &s->queue[s->recv_head];
    to_copy = packet->len;
    if (to_copy > asx_buf_mut_writable(dst)) to_copy = asx_buf_mut_writable(dst);
    st = asx_buf_mut_put(dst, packet->bytes, to_copy);
    if (st != ASX_OK) return st;

    if (from != NULL) *from = packet->from;
    *bytes_read = to_copy;
    s->recv_head = (s->recv_head + 1u) % ASX_UNIX_DGRAM_QUEUE_DEPTH;
    s->recv_count--;
    return ASX_OK;
}

asx_status asx_unix_dgram_close(asx_unix_dgram socket) {
    unix_dgram_slot *s;

    s = unix_dgram_lookup(socket);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    memset(s, 0, sizeof(*s));
    return ASX_OK;
}

int asx_unix_dgram_is_alive(asx_unix_dgram socket) { return unix_dgram_lookup(socket) != NULL; }

/* ------------------------------------------------------------------ */
/* Ancillary data                                                      */
/* ------------------------------------------------------------------ */

void asx_ancillary_init(asx_ancillary *anc) {
    if (anc == NULL) return;
    memset(anc, 0, sizeof(*anc));
}

asx_status asx_ancillary_push_fd(asx_ancillary *anc, uint32_t fd) {
    if (anc == NULL) return ASX_E_INVALID_ARGUMENT;
    if (anc->count >= ASX_ANCILLARY_MAX_FDS) return ASX_E_RESOURCE_EXHAUSTED;
    anc->fds[anc->count++] = fd;
    return ASX_OK;
}

asx_status asx_ancillary_pop_fd(asx_ancillary *anc, uint32_t *out) {
    uint32_t i;

    if (anc == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (anc->count == 0u) return ASX_E_NOT_FOUND;
    *out = anc->fds[0];
    for (i = 1u; i < anc->count; i++) {
        anc->fds[i - 1u] = anc->fds[i];
    }
    anc->count--;
    return ASX_OK;
}

uint32_t asx_ancillary_count(const asx_ancillary *anc) {
    if (anc == NULL) return 0u;
    return anc->count;
}

asx_status asx_unix_stream_send_ancillary(asx_unix_stream stream, const asx_ancillary *anc) {
    unix_stream_slot *s;
    unix_stream_slot *peer;
    uint32_t i;

    if (anc == NULL) return ASX_E_INVALID_ARGUMENT;
    s = unix_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    peer = unix_stream_linked_peer(s);
    if (peer == NULL) return ASX_E_PENDING;

    for (i = 0u; i < anc->count; i++) {
        if (peer->anc_inbox.count >= ASX_ANCILLARY_MAX_FDS) return ASX_E_RESOURCE_EXHAUSTED;
        peer->anc_inbox.fds[peer->anc_inbox.count++] = anc->fds[i];
    }
    return ASX_OK;
}

asx_status asx_unix_stream_recv_ancillary(asx_unix_stream stream, asx_ancillary *out) {
    unix_stream_slot *s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = unix_stream_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    *out = s->anc_inbox;
    asx_ancillary_init(&s->anc_inbox);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Split-stream API                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_tcp_stream_split(asx_tcp_stream stream, asx_read_half *rd, asx_write_half *wr) {
    if (rd == NULL || wr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (stream_lookup(stream) == NULL) return ASX_E_INVALID_ARGUMENT;

    rd->kind = ASX_SPLIT_SOURCE_TCP;
    rd->slot = stream.slot;
    rd->generation = stream.generation;
    rd->active = 1u;

    wr->kind = ASX_SPLIT_SOURCE_TCP;
    wr->slot = stream.slot;
    wr->generation = stream.generation;
    wr->active = 1u;

    return ASX_OK;
}

asx_status asx_unix_stream_split(asx_unix_stream stream, asx_read_half *rd, asx_write_half *wr) {
    if (rd == NULL || wr == NULL) return ASX_E_INVALID_ARGUMENT;
    if (unix_stream_lookup(stream) == NULL) return ASX_E_INVALID_ARGUMENT;

    rd->kind = ASX_SPLIT_SOURCE_UNIX;
    rd->slot = stream.slot;
    rd->generation = stream.generation;
    rd->active = 1u;

    wr->kind = ASX_SPLIT_SOURCE_UNIX;
    wr->slot = stream.slot;
    wr->generation = stream.generation;
    wr->active = 1u;

    return ASX_OK;
}

asx_status asx_read_half_poll_read(asx_read_half *half, asx_buf_mut *dst, uint32_t *bytes_read) {
    if (half == NULL || !half->active) return ASX_E_INVALID_ARGUMENT;

    if (half->kind == ASX_SPLIT_SOURCE_TCP) {
        asx_tcp_stream s;
        s.slot = half->slot;
        s.generation = half->generation;
        return asx_tcp_stream_poll_read(s, dst, bytes_read);
    }
    if (half->kind == ASX_SPLIT_SOURCE_UNIX) {
        asx_unix_stream s;
        s.slot = half->slot;
        s.generation = half->generation;
        return asx_unix_stream_poll_read(s, dst, bytes_read);
    }
    return ASX_E_INVALID_ARGUMENT;
}

asx_status asx_write_half_poll_write(asx_write_half *half, const asx_buf *src,
                                      uint32_t *bytes_written) {
    if (half == NULL || !half->active) return ASX_E_INVALID_ARGUMENT;

    if (half->kind == ASX_SPLIT_SOURCE_TCP) {
        asx_tcp_stream s;
        s.slot = half->slot;
        s.generation = half->generation;
        return asx_tcp_stream_poll_write(s, src, bytes_written);
    }
    if (half->kind == ASX_SPLIT_SOURCE_UNIX) {
        asx_unix_stream s;
        s.slot = half->slot;
        s.generation = half->generation;
        return asx_unix_stream_poll_write(s, src, bytes_written);
    }
    return ASX_E_INVALID_ARGUMENT;
}

asx_status asx_read_half_close(asx_read_half *half) {
    if (half == NULL || !half->active) return ASX_E_INVALID_ARGUMENT;
    half->active = 0u;
    return ASX_OK;
}

asx_status asx_write_half_close(asx_write_half *half) {
    if (half == NULL || !half->active) return ASX_E_INVALID_ARGUMENT;
    half->active = 0u;
    return ASX_OK;
}

int asx_read_half_is_active(const asx_read_half *half) {
    if (half == NULL) return 0;
    return half->active != 0u;
}

int asx_write_half_is_active(const asx_write_half *half) {
    if (half == NULL) return 0;
    return half->active != 0u;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_net_reset(void) {
    memset(g_listeners, 0, sizeof(g_listeners));
    g_listener_count = 0u;
    memset(g_streams, 0, sizeof(g_streams));
    g_stream_count = 0u;
    memset(g_udp_sockets, 0, sizeof(g_udp_sockets));
    g_next_ephemeral_port = 49152u;
    memset(g_unix_listeners, 0, sizeof(g_unix_listeners));
    memset(g_unix_streams, 0, sizeof(g_unix_streams));
    memset(g_unix_dgrams, 0, sizeof(g_unix_dgrams));
}
