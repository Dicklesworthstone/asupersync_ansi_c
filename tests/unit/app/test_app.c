/*
 * test_app.c — unit tests for net primitives, app lifecycle, CLI, and doctor
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/app.h>
#include <asx/app/doctor.h>
#include <asx/core/budget.h>
#include <asx/fs/fs.h>
#include <asx/net/net.h>
#include <asx/runtime/runtime.h>
#include <asx/signal/signal.h>
#include <stdio.h>
#include <string.h>

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        asx_runtime_reset();                                                                       \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

/* ================================================================== */
/* NET: Socket address tests                                          */
/* ================================================================== */

static void test_socket_addr_ipv4(void) {
    asx_socket_addr sa = asx_socket_addr_ipv4(192, 168, 1, 100, 8080);
    ASSERT(sa.addr[0] == 192, "addr[0]");
    ASSERT(sa.addr[1] == 168, "addr[1]");
    ASSERT(sa.addr[2] == 1, "addr[2]");
    ASSERT(sa.addr[3] == 100, "addr[3]");
    ASSERT(sa.port == 8080, "port");
    ASSERT(sa.family == ASX_AF_INET4, "family");
}

static void test_socket_addr_loopback(void) {
    asx_socket_addr sa = asx_socket_addr_loopback(3000);
    ASSERT(sa.addr[0] == 127, "loopback addr[0]");
    ASSERT(sa.addr[1] == 0, "loopback addr[1]");
    ASSERT(sa.addr[2] == 0, "loopback addr[2]");
    ASSERT(sa.addr[3] == 1, "loopback addr[3]");
    ASSERT(sa.port == 3000, "loopback port");
}

static void test_socket_addr_ipv6_loopback(void) {
    asx_socket_addr sa = asx_socket_addr_ipv6_loopback(3001);
    ASSERT(sa.family == ASX_AF_INET6, "ipv6 family");
    ASSERT(sa.port == 3001, "ipv6 port");
    ASSERT(sa.addr[15] == 1, "ipv6 loopback tail");
}

static void test_socket_addr_eq(void) {
    asx_socket_addr a = asx_socket_addr_ipv4(10, 0, 0, 1, 80);
    asx_socket_addr b = asx_socket_addr_ipv4(10, 0, 0, 1, 80);
    asx_socket_addr c = asx_socket_addr_ipv4(10, 0, 0, 2, 80);
    asx_socket_addr d = asx_socket_addr_ipv4(10, 0, 0, 1, 81);

    ASSERT(asx_socket_addr_eq(&a, &b) != 0, "same addrs equal");
    ASSERT(asx_socket_addr_eq(&a, &c) == 0, "diff addr not equal");
    ASSERT(asx_socket_addr_eq(&a, &d) == 0, "diff port not equal");
    ASSERT(asx_socket_addr_eq(NULL, &b) == 0, "null lhs");
    ASSERT(asx_socket_addr_eq(&a, NULL) == 0, "null rhs");
}

/* ================================================================== */
/* NET: TCP listener tests                                            */
/* ================================================================== */

static void test_tcp_listener_bind_close(void) {
    asx_tcp_listener listener;
    asx_socket_addr addr = asx_socket_addr_loopback(8080);
    asx_status st;

    st = asx_tcp_listener_bind(&listener, &addr);
    ASSERT(st == ASX_OK, "bind ok");
    ASSERT(asx_tcp_listener_is_alive(listener), "listener alive");

    st = asx_tcp_listener_close(listener);
    ASSERT(st == ASX_OK, "close ok");
    ASSERT(!asx_tcp_listener_is_alive(listener), "listener dead");
}

static void test_tcp_listener_local_addr(void) {
    asx_tcp_listener listener;
    asx_socket_addr addr = asx_socket_addr_ipv4(10, 0, 0, 1, 9090);
    asx_socket_addr out;

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    ASSERT(asx_tcp_listener_local_addr(listener, &out) == ASX_OK, "local_addr ok");
    ASSERT(asx_socket_addr_eq(&addr, &out), "local addr matches bind addr");

    asx_tcp_listener_close(listener);
}

static void test_tcp_listener_poll_accept_pending(void) {
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(7070);

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));

    ASSERT(asx_tcp_listener_poll_accept(listener, &stream, NULL) == ASX_E_PENDING,
           "poll_accept pending");

    asx_tcp_listener_close(listener);
}

static void test_tcp_listener_poll_accept_ready_for_loopback_connect(void) {
    asx_tcp_listener listener;
    asx_tcp_stream client;
    asx_tcp_stream accepted;
    asx_socket_addr addr = asx_socket_addr_loopback(7072);
    asx_socket_addr peer;

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    MUST_OK(asx_tcp_connect(&client, &addr));

    ASSERT(asx_tcp_listener_poll_accept(listener, &accepted, &peer) == ASX_OK,
           "accept returns ready stream");
    ASSERT(asx_socket_addr_eq(&peer, &addr) == 0, "accepted peer uses client ephemeral addr");
    ASSERT(asx_tcp_stream_is_alive(accepted), "accepted stream alive");

    asx_tcp_stream_close(accepted);
    asx_tcp_stream_close(client);
    asx_tcp_listener_close(listener);
}

static void test_tcp_listener_bind_with_cx_permission_denied(void) {
    asx_tcp_listener listener;
    asx_socket_addr addr = asx_socket_addr_loopback(8181);
    asx_cx cx;

    MUST_OK(asx_cx_init(&cx, 1, ASX_INVALID_ID, ASX_CAP_CLOCK_READ));
    ASSERT(asx_tcp_listener_bind_with_cx(&listener, &addr, &cx) == ASX_E_PERMISSION_DENIED,
           "bind with cx requires channel cap");
}

static void test_tcp_listener_poll_accept_with_cx_budget_checkpoint(void) {
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(7071);
    asx_cx cx;
    asx_budget budget = asx_budget_from_polls(1);

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    MUST_OK(asx_cx_init(&cx, 1, 1,
                        ASX_CAP_CHANNEL | ASX_CAP_CANCEL_CHECK | ASX_CAP_BUDGET_READ |
                            ASX_CAP_BUDGET_CONSUME));
    MUST_OK(asx_cx_bind_budget(&cx, &budget));

    ASSERT(asx_tcp_listener_poll_accept_with_cx(listener, &stream, NULL, &cx) == ASX_E_PENDING,
           "first accept poll pending");
    ASSERT(asx_tcp_listener_poll_accept_with_cx(listener, &stream, NULL, &cx) ==
               ASX_E_POLL_BUDGET_EXHAUSTED,
           "second accept poll exhausts budget");

    asx_tcp_listener_close(listener);
}

static void test_tcp_listener_null_args(void) {
    asx_socket_addr addr = asx_socket_addr_loopback(80);
    asx_tcp_listener listener;

    ASSERT(asx_tcp_listener_bind(NULL, &addr) == ASX_E_INVALID_ARGUMENT, "bind null out");
    ASSERT(asx_tcp_listener_bind(&listener, NULL) == ASX_E_INVALID_ARGUMENT, "bind null addr");
}

static void test_tcp_listener_arena_exhaustion(void) {
    asx_tcp_listener listeners[ASX_MAX_TCP_LISTENERS];
    asx_tcp_listener extra;
    asx_socket_addr addr = asx_socket_addr_loopback(5000);
    uint32_t i;
    asx_status st;

    for (i = 0; i < ASX_MAX_TCP_LISTENERS; i++) {
        addr.port = (uint16_t)(5000 + i);
        st = asx_tcp_listener_bind(&listeners[i], &addr);
        ASSERT(st == ASX_OK, "bind within capacity");
    }

    addr.port = 9999;
    st = asx_tcp_listener_bind(&extra, &addr);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "arena exhausted");

    for (i = 0; i < ASX_MAX_TCP_LISTENERS; i++) { asx_tcp_listener_close(listeners[i]); }
}

/* ================================================================== */
/* NET: TCP stream tests                                              */
/* ================================================================== */

static void test_tcp_connect_close(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(4000);

    ASSERT(asx_tcp_connect(&stream, &addr) == ASX_OK, "connect ok");
    ASSERT(asx_tcp_stream_is_alive(stream), "stream alive");

    ASSERT(asx_tcp_stream_close(stream) == ASX_OK, "close ok");
    ASSERT(!asx_tcp_stream_is_alive(stream), "stream dead");
}

static void test_tcp_connect_with_cx_permission_denied(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(4001);
    asx_cx cx;

    MUST_OK(asx_cx_init(&cx, 1, ASX_INVALID_ID, ASX_CAP_SPAWN));
    ASSERT(asx_tcp_connect_with_cx(&stream, &addr, &cx) == ASX_E_PERMISSION_DENIED,
           "connect with cx requires channel cap");
}

static void test_tcp_stream_peer_addr(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_ipv4(10, 0, 0, 5, 443);
    asx_socket_addr out;

    MUST_OK(asx_tcp_connect(&stream, &addr));
    ASSERT(asx_tcp_stream_peer_addr(stream, &out) == ASX_OK, "peer_addr ok");
    ASSERT(asx_socket_addr_eq(&addr, &out), "peer addr matches connect addr");

    asx_tcp_stream_close(stream);
}

static void test_tcp_stream_poll_pending(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(2000);
    asx_buf_mut dst;
    asx_buf src;
    uint32_t n;

    MUST_OK(asx_tcp_connect(&stream, &addr));

    asx_buf_mut_init(&dst);
    ASSERT(asx_tcp_stream_poll_read(stream, &dst, &n) == ASX_E_PENDING, "poll_read pending");
    ASSERT(n == 0, "no bytes read");

    src = asx_buf_from_cstr("hello");
    ASSERT(asx_tcp_stream_poll_write(stream, &src, &n) == ASX_E_PENDING, "poll_write pending");
    ASSERT(n == 0, "no bytes written");

    asx_tcp_stream_close(stream);
}

static void test_tcp_stream_loopback_transfer(void) {
    asx_tcp_listener listener;
    asx_tcp_stream client;
    asx_tcp_stream accepted;
    asx_socket_addr addr = asx_socket_addr_loopback(2003);
    asx_buf src;
    asx_buf_mut dst;
    asx_buf readable;
    uint32_t n = 0u;

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    MUST_OK(asx_tcp_connect(&client, &addr));
    MUST_OK(asx_tcp_listener_poll_accept(listener, &accepted, NULL));

    src = asx_buf_from_cstr("hello-loopback");
    ASSERT(asx_tcp_stream_poll_write(client, &src, &n) == ASX_OK, "loopback write ok");
    ASSERT(n == src.len, "loopback write length");

    asx_buf_mut_init(&dst);
    ASSERT(asx_tcp_stream_poll_read(accepted, &dst, &n) == ASX_OK, "loopback read ok");
    ASSERT(n == src.len, "loopback read length");
    readable = asx_buf_mut_readable(&dst);
    ASSERT(readable.len == src.len, "readable len matches");
    ASSERT(memcmp(readable.ptr, src.ptr, src.len) == 0, "loopback payload matches");

    asx_tcp_stream_close(accepted);
    asx_tcp_stream_close(client);
    asx_tcp_listener_close(listener);
}

static void test_tcp_stream_poll_with_cx_budget_checkpoint(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(2001);
    asx_buf_mut dst;
    asx_buf src;
    uint32_t n;
    asx_cx cx;
    asx_budget budget = asx_budget_from_polls(1);

    MUST_OK(asx_tcp_connect(&stream, &addr));
    MUST_OK(asx_cx_init(&cx, 1, 1,
                        ASX_CAP_CHANNEL | ASX_CAP_CANCEL_CHECK | ASX_CAP_BUDGET_READ |
                            ASX_CAP_BUDGET_CONSUME));
    MUST_OK(asx_cx_bind_budget(&cx, &budget));

    asx_buf_mut_init(&dst);
    ASSERT(asx_tcp_stream_poll_read_with_cx(stream, &dst, &n, &cx) == ASX_E_PENDING,
           "first poll_read pending");
    ASSERT(asx_tcp_stream_poll_read_with_cx(stream, &dst, &n, &cx) == ASX_E_POLL_BUDGET_EXHAUSTED,
           "second poll_read exhausts budget");

    src = asx_buf_from_cstr("hello");
    MUST_OK(asx_cx_init(&cx, 1, 1,
                        ASX_CAP_CHANNEL | ASX_CAP_CANCEL_CHECK | ASX_CAP_BUDGET_READ |
                            ASX_CAP_BUDGET_CONSUME));
    budget = asx_budget_from_polls(1);
    MUST_OK(asx_cx_bind_budget(&cx, &budget));
    ASSERT(asx_tcp_stream_poll_write_with_cx(stream, &src, &n, &cx) == ASX_E_PENDING,
           "first poll_write pending");
    ASSERT(asx_tcp_stream_poll_write_with_cx(stream, &src, &n, &cx) == ASX_E_POLL_BUDGET_EXHAUSTED,
           "second poll_write exhausts budget");

    asx_tcp_stream_close(stream);
}

static void test_tcp_stream_poll_with_cx_permission_denied(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(2002);
    asx_buf_mut dst;
    asx_buf src;
    uint32_t n;
    asx_cx cx;

    MUST_OK(asx_tcp_connect(&stream, &addr));
    MUST_OK(asx_cx_init(&cx, 1, 1, ASX_CAP_CANCEL_CHECK));

    asx_buf_mut_init(&dst);
    ASSERT(asx_tcp_stream_poll_read_with_cx(stream, &dst, &n, &cx) == ASX_E_PERMISSION_DENIED,
           "poll_read with cx requires channel cap");

    src = asx_buf_from_cstr("hello");
    ASSERT(asx_tcp_stream_poll_write_with_cx(stream, &src, &n, &cx) == ASX_E_PERMISSION_DENIED,
           "poll_write with cx requires channel cap");

    asx_tcp_stream_close(stream);
}

static void test_tcp_stream_stale_handle(void) {
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(1111);

    MUST_OK(asx_tcp_connect(&stream, &addr));
    asx_tcp_stream_close(stream);

    /* Stale handle after close */
    ASSERT(!asx_tcp_stream_is_alive(stream), "stale not alive");
    ASSERT(asx_tcp_stream_close(stream) == ASX_E_INVALID_ARGUMENT, "double close rejected");
}

static void test_net_reset(void) {
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(6000);

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    MUST_OK(asx_tcp_connect(&stream, &addr));

    asx_net_reset();

    ASSERT(!asx_tcp_listener_is_alive(listener), "listener dead after reset");
    ASSERT(!asx_tcp_stream_is_alive(stream), "stream dead after reset");
}

/* ================================================================== */
/* NET: UDP tests                                                     */
/* ================================================================== */

static void test_udp_bind_connect_and_addrs(void) {
    asx_udp_socket socket;
    asx_socket_addr local = asx_socket_addr_loopback(6100);
    asx_socket_addr peer = asx_socket_addr_ipv4(10, 0, 0, 42, 5353);
    asx_socket_addr out;

    MUST_OK(asx_udp_bind(&socket, &local));
    ASSERT(asx_udp_is_alive(socket), "udp alive");
    ASSERT(asx_udp_local_addr(socket, &out) == ASX_OK, "udp local addr ok");
    ASSERT(asx_socket_addr_eq(&local, &out), "udp local addr matches");

    MUST_OK(asx_udp_connect(socket, &peer));
    ASSERT(asx_udp_peer_addr(socket, &out) == ASX_OK, "udp peer addr ok");
    ASSERT(asx_socket_addr_eq(&peer, &out), "udp peer addr matches");

    ASSERT(asx_udp_close(socket) == ASX_OK, "udp close ok");
    ASSERT(!asx_udp_is_alive(socket), "udp dead after close");
}

static void test_udp_poll_pending(void) {
    asx_udp_socket socket;
    asx_socket_addr local = asx_socket_addr_loopback(6101);
    asx_socket_addr peer = asx_socket_addr_ipv4(10, 0, 0, 99, 5353);
    asx_socket_addr from;
    asx_buf_mut dst;
    asx_buf src;
    uint32_t n = 999u;

    MUST_OK(asx_udp_bind(&socket, &local));
    MUST_OK(asx_udp_connect(socket, &peer));

    asx_buf_mut_init(&dst);
    ASSERT(asx_udp_poll_recv(socket, &dst, &n, &from) == ASX_E_PENDING, "udp recv pending");
    ASSERT(n == 0u, "udp recv zero bytes");
    ASSERT(asx_socket_addr_eq(&from, &peer), "udp recv reports peer");

    src = asx_buf_from_cstr("ping");
    n = 999u;
    ASSERT(asx_udp_poll_send(socket, &src, &n, NULL) == ASX_E_PENDING, "udp send pending");
    ASSERT(n == 0u, "udp send zero bytes");

    asx_udp_close(socket);
}

static void test_udp_loopback_delivery(void) {
    asx_udp_socket sender;
    asx_udp_socket receiver;
    asx_socket_addr sender_addr = asx_socket_addr_loopback(6105);
    asx_socket_addr receiver_addr = asx_socket_addr_loopback(6106);
    asx_socket_addr from;
    asx_buf src;
    asx_buf_mut dst;
    asx_buf readable;
    uint32_t n = 0u;

    MUST_OK(asx_udp_bind(&sender, &sender_addr));
    MUST_OK(asx_udp_bind(&receiver, &receiver_addr));
    MUST_OK(asx_udp_connect(sender, &receiver_addr));

    src = asx_buf_from_cstr("udp-loopback");
    ASSERT(asx_udp_poll_send(sender, &src, &n, NULL) == ASX_OK, "udp loopback send ok");
    ASSERT(n == src.len, "udp loopback send length");

    asx_buf_mut_init(&dst);
    ASSERT(asx_udp_poll_recv(receiver, &dst, &n, &from) == ASX_OK, "udp loopback recv ok");
    ASSERT(n == src.len, "udp loopback recv length");
    ASSERT(asx_socket_addr_eq(&from, &sender_addr) != 0, "udp loopback sender addr");
    readable = asx_buf_mut_readable(&dst);
    ASSERT(readable.len == src.len, "udp readable len matches");
    ASSERT(memcmp(readable.ptr, src.ptr, src.len) == 0, "udp payload matches");

    asx_udp_close(receiver);
    asx_udp_close(sender);
}

static void test_udp_poll_requires_peer_or_destination(void) {
    asx_udp_socket socket;
    asx_socket_addr local = asx_socket_addr_loopback(6102);
    asx_buf src = asx_buf_from_cstr("ping");
    uint32_t n = 0u;

    MUST_OK(asx_udp_bind(&socket, &local));
    ASSERT(asx_udp_poll_send(socket, &src, &n, NULL) == ASX_E_INVALID_ARGUMENT,
           "udp send requires peer or explicit destination");
    asx_udp_close(socket);
}

static void test_udp_with_cx_budget_and_permission(void) {
    asx_udp_socket socket;
    asx_socket_addr local = asx_socket_addr_loopback(6103);
    asx_socket_addr peer = asx_socket_addr_loopback(6104);
    asx_buf_mut dst;
    asx_buf src = asx_buf_from_cstr("hello");
    asx_cx cx;
    asx_budget budget = asx_budget_from_polls(1);
    uint32_t n = 0u;

    MUST_OK(asx_udp_bind(&socket, &local));
    MUST_OK(asx_udp_connect(socket, &peer));

    MUST_OK(asx_cx_init(&cx, 1, 1,
                        ASX_CAP_CHANNEL | ASX_CAP_CANCEL_CHECK | ASX_CAP_BUDGET_READ |
                            ASX_CAP_BUDGET_CONSUME));
    MUST_OK(asx_cx_bind_budget(&cx, &budget));
    asx_buf_mut_init(&dst);
    ASSERT(asx_udp_poll_recv_with_cx(socket, &dst, &n, NULL, &cx) == ASX_E_PENDING,
           "udp recv with cx pending");
    ASSERT(asx_udp_poll_recv_with_cx(socket, &dst, &n, NULL, &cx) == ASX_E_POLL_BUDGET_EXHAUSTED,
           "udp recv checkpoint budget");

    MUST_OK(asx_cx_init(&cx, 1, 1, ASX_CAP_CANCEL_CHECK));
    ASSERT(asx_udp_poll_send_with_cx(socket, &src, &n, NULL, &cx) == ASX_E_PERMISSION_DENIED,
           "udp send with cx requires channel cap");

    asx_udp_close(socket);
}

static void test_udp_arena_exhaustion(void) {
    asx_udp_socket sockets[ASX_MAX_UDP_SOCKETS];
    asx_socket_addr addr = asx_socket_addr_loopback(6200);
    uint32_t i;

    for (i = 0; i < ASX_MAX_UDP_SOCKETS; i++) {
        addr.port = (uint16_t)(6200 + i);
        ASSERT(asx_udp_bind(&sockets[i], &addr) == ASX_OK, "udp bind within capacity");
    }
    ASSERT(asx_udp_bind(&sockets[0], &addr) == ASX_E_RESOURCE_EXHAUSTED, "udp arena exhausted");
    for (i = 0; i < ASX_MAX_UDP_SOCKETS; i++) { asx_udp_close(sockets[i]); }
}

/* ================================================================== */
/* NET: resolve / happy-eyeballs tests                                */
/* ================================================================== */

static void test_resolve_localhost_prefers_ipv6(void) {
    asx_resolve_options opts;
    asx_resolve_result out;

    asx_resolve_options_init(&opts, 8080);
    opts.preferred_family = ASX_AF_INET6;

    ASSERT(asx_resolve_host(&out, "localhost", &opts) == ASX_OK, "resolve localhost");
    ASSERT(out.count == 2u, "dual stack localhost");
    ASSERT(out.addrs[0].family == ASX_AF_INET6, "ipv6 first");
    ASSERT(out.addrs[1].family == ASX_AF_INET4, "ipv4 second");
}

static void test_resolve_ipv4_literal(void) {
    asx_resolve_options opts;
    asx_resolve_result out;

    asx_resolve_options_init(&opts, 53);
    opts.allow_ipv6 = 0u;
    ASSERT(asx_resolve_host(&out, "203.0.113.7", &opts) == ASX_OK, "resolve ipv4 literal");
    ASSERT(out.count == 1u, "single ipv4 result");
    ASSERT(out.addrs[0].family == ASX_AF_INET4, "ipv4 family");
    ASSERT(out.addrs[0].addr[0] == 203, "ipv4 octet 0");
    ASSERT(out.addrs[0].addr[1] == 0, "ipv4 octet 1");
    ASSERT(out.addrs[0].addr[2] == 113, "ipv4 octet 2");
    ASSERT(out.addrs[0].addr[3] == 7, "ipv4 octet 3");
    ASSERT(out.addrs[0].port == 53, "resolved port");
}

static void test_resolve_rejects_unknown_host(void) {
    asx_resolve_result out;
    ASSERT(asx_resolve_host(&out, "not-a-known-host", NULL) == ASX_E_NOT_FOUND,
           "unknown host rejected");
}

static void test_resolver_cache_round_trip(void) {
    asx_resolver resolver;
    asx_resolve_options opts;
    asx_resolve_result out;
    uint8_t cache_hit = 9u;

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&opts, 8123u);

    ASSERT(asx_resolver_lookup(&resolver, "localhost", &opts, &out, &cache_hit) == ASX_OK,
           "resolver first lookup ok");
    ASSERT(cache_hit == 0u, "first lookup is miss");
    ASSERT(asx_resolver_cached_count(&resolver) == 1u, "one resolver cache entry");

    ASSERT(asx_resolver_lookup(&resolver, "localhost", &opts, &out, &cache_hit) == ASX_OK,
           "resolver second lookup ok");
    ASSERT(cache_hit == 1u, "second lookup is hit");
    ASSERT(out.count == 2u, "resolver cached dual-stack localhost");
}

static void test_resolver_negative_cache_and_invalidate(void) {
    asx_resolver resolver;
    asx_resolve_result out;
    uint8_t cache_hit = 9u;

    asx_resolver_init(&resolver);

    ASSERT(asx_resolver_lookup(&resolver, "not-a-known-host", NULL, &out, &cache_hit) ==
               ASX_E_NOT_FOUND,
           "resolver caches negative lookup");
    ASSERT(cache_hit == 0u, "negative first lookup is miss");
    ASSERT(asx_resolver_cached_count(&resolver) == 1u, "negative result cached");

    ASSERT(asx_resolver_lookup(&resolver, "not-a-known-host", NULL, &out, &cache_hit) ==
               ASX_E_NOT_FOUND,
           "negative cache hit preserves status");
    ASSERT(cache_hit == 1u, "negative second lookup is hit");

    asx_resolver_invalidate(&resolver, "not-a-known-host");
    ASSERT(asx_resolver_cached_count(&resolver) == 0u, "invalidate drops cached host");
}

static void test_happy_eyeballs_reorders_preference(void) {
    asx_resolve_result in;
    asx_resolve_result out;

    memset(&in, 0, sizeof(in));
    in.addrs[0] = asx_socket_addr_loopback(80);
    in.addrs[1] = asx_socket_addr_ipv6_loopback(80);
    in.count = 2u;

    ASSERT(asx_happy_eyeballs_order(&out, &in, ASX_AF_INET6) == ASX_OK,
           "happy eyeballs reorder ok");
    ASSERT(out.count == 2u, "reordered count");
    ASSERT(out.addrs[0].family == ASX_AF_INET6, "ipv6 promoted first");
    ASSERT(out.addrs[1].family == ASX_AF_INET4, "ipv4 follows");
}

static void test_tcp_connect_host_uses_resolver_cache(void) {
    asx_resolver resolver;
    asx_resolve_options opts;
    asx_tcp_listener listener;
    asx_tcp_stream client;
    asx_tcp_stream accepted;
    asx_socket_addr listen_addr;
    asx_socket_addr selected;
    uint8_t cache_hit = 9u;

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&opts, 7011u);
    opts.preferred_family = ASX_AF_INET6;

    listen_addr = asx_socket_addr_ipv6_loopback(7011u);
    MUST_OK(asx_tcp_listener_bind(&listener, &listen_addr));
    ASSERT(asx_tcp_connect_host(&client, &resolver, "localhost", &opts, &selected, &cache_hit) ==
               ASX_OK,
           "tcp connect host ok");
    ASSERT(cache_hit == 0u, "tcp connect host first lookup miss");
    ASSERT(selected.family == ASX_AF_INET6, "tcp connect host selects preferred family");
    ASSERT(asx_tcp_listener_poll_accept(listener, &accepted, NULL) == ASX_OK,
           "listener accepts resolved stream");

    asx_tcp_stream_close(accepted);
    asx_tcp_stream_close(client);
    asx_tcp_listener_close(listener);

    ASSERT(asx_tcp_connect_host(&client, &resolver, "localhost", &opts, &selected, &cache_hit) ==
               ASX_OK,
           "tcp connect host second lookup ok");
    ASSERT(cache_hit == 1u, "tcp connect host second lookup hit");
    asx_tcp_stream_close(client);
}

static void test_udp_connect_host_uses_resolver_cache(void) {
    asx_resolver resolver;
    asx_resolve_options opts;
    asx_udp_socket socket;
    asx_socket_addr bind_addr;
    asx_socket_addr selected;
    uint8_t cache_hit = 9u;

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&opts, 6107u);
    opts.allow_ipv6 = 0u;

    bind_addr = asx_socket_addr_loopback(6108u);
    MUST_OK(asx_udp_bind(&socket, &bind_addr));
    ASSERT(asx_udp_connect_host(socket, &resolver, "localhost", &opts, &selected, &cache_hit) ==
               ASX_OK,
           "udp connect host ok");
    ASSERT(cache_hit == 0u, "udp connect host first lookup miss");
    ASSERT(selected.family == ASX_AF_INET4, "udp connect host selects ipv4 endpoint");

    ASSERT(asx_udp_connect_host(socket, &resolver, "localhost", &opts, &selected, &cache_hit) ==
               ASX_OK,
           "udp connect host second lookup ok");
    ASSERT(cache_hit == 1u, "udp connect host second lookup hit");
    asx_udp_close(socket);
}

/* ================================================================== */
/* APP: CLI argument parsing                                          */
/* ================================================================== */

static void test_parse_args_defaults(void) {
    asx_app_args args;
    const char *argv[] = {"myapp"};

    MUST_OK(asx_app_parse_args(&args, 1, argv));
    ASSERT(args.command == ASX_APP_CMD_RUN, "default command is run");
    ASSERT(args.verbose == 0, "default verbose 0");
    ASSERT(args.help == 0, "default help 0");
    ASSERT(args.seed == 0, "default seed 0");
    ASSERT(args.scenario == NULL, "default scenario null");
}

static void test_parse_args_doctor(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "doctor"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.command == ASX_APP_CMD_DOCTOR, "doctor command");
}

static void test_parse_args_replay(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "replay", "scenario1"};

    MUST_OK(asx_app_parse_args(&args, 3, argv));
    ASSERT(args.command == ASX_APP_CMD_REPLAY, "replay command");
    ASSERT(args.scenario != NULL, "scenario set");
    ASSERT(strcmp(args.scenario, "scenario1") == 0, "scenario name");
}

static void test_parse_args_server(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "server"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.command == ASX_APP_CMD_SERVER, "server command");
}

static void test_parse_args_verbose(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "-v", "--verbose"};

    MUST_OK(asx_app_parse_args(&args, 3, argv));
    ASSERT(args.verbose == 2, "double verbose");
}

static void test_parse_args_seed(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "--seed=12345"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.seed == 12345, "seed parsed");
}

static void test_parse_args_help(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "--help"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.help == 1, "help flag");
}

static void test_parse_args_unknown_rejected(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "--mystery"};

    ASSERT(asx_app_parse_args(&args, 2, argv) == ASX_E_INVALID_ARGUMENT, "unknown rejected");
}

static void test_parse_args_replay_requires_scenario(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "replay"};

    ASSERT(asx_app_parse_args(&args, 2, argv) == ASX_E_INVALID_ARGUMENT,
           "replay requires scenario");
}

static void test_parse_args_seed_requires_decimal(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "--seed=12x"};

    ASSERT(asx_app_parse_args(&args, 2, argv) == ASX_E_INVALID_ARGUMENT, "seed requires decimal");
}

static void test_parse_args_seed_rejects_overflow(void) {
    asx_app_args args;
    const char *argv[] = {"myapp", "--seed=18446744073709551616"};

    ASSERT(asx_app_parse_args(&args, 2, argv) == ASX_E_INVALID_ARGUMENT, "seed overflow rejected");
}

static void test_parse_args_null(void) {
    ASSERT(asx_app_parse_args(NULL, 0, NULL) == ASX_E_INVALID_ARGUMENT, "null args rejected");
}

/* ================================================================== */
/* APP: Lifecycle                                                     */
/* ================================================================== */

static asx_status noop_poll(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static void test_app_init_shutdown(void) {
    asx_app app;
    asx_app_config config;
    asx_status st;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";

    st = asx_app_init(&app, &config);
    ASSERT(st == ASX_OK, "app init ok");
    ASSERT(app.initialized == 1, "app initialized");
    ASSERT(app.config.poll_budget == 10000, "default poll budget");

    asx_app_shutdown(&app);
    ASSERT(app.initialized == 0, "app shut down");
}

static void test_app_run_noop(void) {
    asx_app app;
    asx_app_config config;
    asx_exit_code ec;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";
    config.poll_budget = 100;

    MUST_OK(asx_app_init(&app, &config));

    ec = asx_app_run(&app, noop_poll, NULL);
    ASSERT(ec == ASX_EXIT_OK, "noop run ok");

    asx_app_shutdown(&app);
}

static void test_app_run_with_cx_noop(void) {
    asx_app app;
    asx_app_config config;
    asx_exit_code ec;
    asx_cx cx;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";
    config.poll_budget = 100;

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_cx_init(&cx, asx_app_region(&app), ASX_INVALID_ID, ASX_CAP_SPAWN));

    ec = asx_app_run_with_cx(&app, &cx, noop_poll, NULL);
    ASSERT(ec == ASX_EXIT_OK, "noop run with cx ok");

    asx_app_shutdown(&app);
}

static void test_app_run_with_cx_permission_denied(void) {
    asx_app app;
    asx_app_config config;
    asx_cx cx;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_cx_init(&cx, asx_app_region(&app), ASX_INVALID_ID, ASX_CAP_CLOCK_READ));

    ASSERT(asx_app_run_with_cx(&app, &cx, noop_poll, NULL) == ASX_EXIT_TASK_FAILED,
           "run with cx no spawn denied");

    asx_app_shutdown(&app);
}

static void test_app_run_with_cx_region_mismatch(void) {
    asx_app app;
    asx_app_config config;
    asx_cx cx;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_cx_init(&cx, asx_app_region(&app) + 1u, ASX_INVALID_ID, ASX_CAP_SPAWN));

    ASSERT(asx_app_run_with_cx(&app, &cx, noop_poll, NULL) == ASX_EXIT_TASK_FAILED,
           "run with cx wrong region denied");

    asx_app_shutdown(&app);
}

static void test_app_region(void) {
    asx_app app;
    asx_app_config config;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";

    MUST_OK(asx_app_init(&app, &config));
    ASSERT(asx_app_region(&app) != 0, "region is valid handle");

    asx_app_shutdown(&app);
}

static void test_app_null_args(void) {
    asx_app app;
    asx_app_config config;

    memset(&config, 0, sizeof(config));

    ASSERT(asx_app_init(NULL, &config) == ASX_E_INVALID_ARGUMENT, "null app");
    ASSERT(asx_app_init(&app, NULL) == ASX_E_INVALID_ARGUMENT, "null config");
    ASSERT(asx_app_run(NULL, noop_poll, NULL) == ASX_EXIT_ERROR, "null app run");
    ASSERT(asx_app_run_with_cx(NULL, NULL, noop_poll, NULL) == ASX_EXIT_ERROR, "null app run cx");
    ASSERT(asx_app_run_server_with_cx(NULL, NULL, NULL, noop_poll, NULL, NULL, NULL) ==
               ASX_EXIT_ERROR,
           "null app run server cx");

    memset(&app, 0, sizeof(app));
    ASSERT(asx_app_run(&app, noop_poll, NULL) == ASX_EXIT_INIT_FAILED, "uninit app run");
    ASSERT(asx_app_run_with_cx(&app, NULL, noop_poll, NULL) == ASX_EXIT_ERROR, "null cx run");
    ASSERT(asx_app_run_server_with_cx(&app, NULL, NULL, noop_poll, NULL, NULL, NULL) ==
               ASX_EXIT_ERROR,
           "null server cx run");
    ASSERT(asx_app_region(NULL) == 0, "null app region");
}

static void test_app_run_server_happy_path(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;
    asx_report_buf summary;
    asx_fs_path path;
    asx_file_handle file;
    asx_buf payload;
    uint32_t n;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    config.poll_budget = 50;
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.bootstrap_process_name = "sidecar";
    server.run_poll_budget = 50;
    server.require_config = 1;

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_fs_path_from_cstr(&path, "/service/app.cfg"));
    server.config_path = &path;
    MUST_OK(asx_fs_file_open(&file, &path, ASX_FS_OPEN_CREATE | ASX_FS_OPEN_WRITE));
    payload = asx_buf_from_cstr("port=7000");
    MUST_OK(asx_fs_file_poll_write(file, &payload, &n));
    MUST_OK(asx_fs_file_close(file));

    ASSERT(asx_app_run_server(&app, &server, noop_poll, NULL, &report, &summary) == ASX_EXIT_OK,
           "server run ok");
    ASSERT(report.config_loaded == 1, "config loaded");
    ASSERT(report.bootstrap_process_spawned == 1, "bootstrap spawned");
    ASSERT(report.main_task_spawned == 1, "main task spawned");
    ASSERT(strstr(asx_report_buf_cstr(&summary), "Server Summary:") != NULL, "summary present");
    ASSERT(strstr(asx_report_buf_cstr(&summary), "Doctor:") != NULL, "doctor present");
    ASSERT(strstr(asx_report_buf_cstr(&summary), "Runtime Inspection:") != NULL,
           "inspection present");
    ASSERT(strstr(asx_report_buf_cstr(&summary), "io_driver:") != NULL, "io driver in inspection");
    ASSERT(strstr(asx_report_buf_cstr(&summary), "blocking:") != NULL, "blocking in inspection");

    asx_app_shutdown(&app);
}

static void test_app_run_server_with_cx_happy_path(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;
    asx_cx cx;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    config.poll_budget = 20;
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.bootstrap_process_name = "sidecar";
    server.run_poll_budget = 20;

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_cx_init(&cx, asx_app_region(&app), ASX_INVALID_ID, ASX_CAP_SPAWN));

    ASSERT(asx_app_run_server_with_cx(&app, &cx, &server, noop_poll, NULL, &report, NULL) ==
               ASX_EXIT_OK,
           "server run with cx ok");
    ASSERT(app.exit_code == ASX_EXIT_OK, "app exit code updated");
    ASSERT(report.bootstrap_process_spawned == 1, "bootstrap spawned");
    ASSERT(report.signal_subscription_active == 1, "signal subscribed");
    ASSERT(report.main_task_spawned == 1, "main task spawned");

    asx_app_shutdown(&app);
}

static void test_app_run_server_requires_config(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    server.require_config = 1;
    server.shutdown_signal = ASX_SIGNAL_TERM;

    MUST_OK(asx_app_init(&app, &config));
    ASSERT(asx_app_run_server(&app, &server, noop_poll, NULL, &report, NULL) == ASX_EXIT_OK,
           "no config path accepted when absent");

    {
        asx_fs_path path;
        MUST_OK(asx_fs_path_from_cstr(&path, "/missing.cfg"));
        server.config_path = &path;
        ASSERT(asx_app_run_server(&app, &server, noop_poll, NULL, &report, NULL) ==
                   ASX_EXIT_INIT_FAILED,
               "missing required config fails");
        ASSERT(app.exit_code == ASX_EXIT_INIT_FAILED, "app exit code updated on init failure");
        ASSERT(report.last_status == ASX_E_NOT_FOUND, "missing config status");
    }

    asx_app_shutdown(&app);
}

static void test_app_run_server_signal_shutdown(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.bootstrap_process_name = "sidecar";

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_signal_raise(ASX_SIGNAL_TERM));
    ASSERT(asx_app_run_server(&app, &server, noop_poll, NULL, &report, NULL) == ASX_EXIT_OK,
           "signal shutdown still clean");
    ASSERT(report.shutdown_requested == 1, "shutdown requested");
    ASSERT(report.bootstrap_process_exited == 1, "bootstrap exited");
    ASSERT(report.bootstrap_process_exit_code == 0, "clean shutdown code");
    asx_app_shutdown(&app);
}

static void test_app_run_server_bootstrap_failure(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.bootstrap_process_name = "failing-sidecar";
    server.bootstrap_polls_until_exit = 0;
    server.bootstrap_exit_code = 23;

    MUST_OK(asx_app_init(&app, &config));
    ASSERT(asx_app_run_server(&app, &server, noop_poll, NULL, &report, NULL) ==
               ASX_EXIT_TASK_FAILED,
           "unexpected bootstrap failure bubbles");
    ASSERT(report.bootstrap_process_exited == 1, "bootstrap exited");
    ASSERT(report.bootstrap_process_exit_code == 23, "non-zero code preserved");
    asx_app_shutdown(&app);
}

static void test_app_run_server_with_cx_permission_denied_fails_closed(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;
    asx_cx cx;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.bootstrap_process_name = "sidecar";

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_cx_init(&cx, asx_app_region(&app), ASX_INVALID_ID, ASX_CAP_CLOCK_READ));

    ASSERT(asx_app_run_server_with_cx(&app, &cx, &server, noop_poll, NULL, &report, NULL) ==
               ASX_EXIT_TASK_FAILED,
           "server run without spawn denied");
    ASSERT(app.exit_code == ASX_EXIT_TASK_FAILED, "app exit code updated on deny");
    ASSERT(report.last_status == ASX_E_PERMISSION_DENIED, "permission denied reported");
    ASSERT(report.config_loaded == 0, "config not touched");
    ASSERT(report.bootstrap_process_spawned == 0, "bootstrap not spawned");
    ASSERT(report.signal_subscription_active == 0, "signal not subscribed");
    ASSERT(report.main_task_spawned == 0, "main task not spawned");

    asx_app_shutdown(&app);
}

static void test_app_run_server_with_cx_region_mismatch_fails_closed(void) {
    asx_app app;
    asx_app_config config;
    asx_app_server_config server;
    asx_app_server_report report;
    asx_cx cx;

    memset(&config, 0, sizeof(config));
    memset(&server, 0, sizeof(server));
    config.name = "srv";
    server.shutdown_signal = ASX_SIGNAL_TERM;
    server.bootstrap_process_name = "sidecar";

    MUST_OK(asx_app_init(&app, &config));
    MUST_OK(asx_cx_init(&cx, asx_app_region(&app) + 1u, ASX_INVALID_ID, ASX_CAP_SPAWN));

    ASSERT(asx_app_run_server_with_cx(&app, &cx, &server, noop_poll, NULL, &report, NULL) ==
               ASX_EXIT_TASK_FAILED,
           "server run wrong region denied");
    ASSERT(report.last_status == ASX_E_PERMISSION_DENIED, "region mismatch reported");
    ASSERT(report.bootstrap_process_spawned == 0, "bootstrap not spawned");
    ASSERT(report.signal_subscription_active == 0, "signal not subscribed");
    ASSERT(report.main_task_spawned == 0, "main task not spawned");

    asx_app_shutdown(&app);
}

/* ================================================================== */
/* DOCTOR: Diagnostic checks                                          */
/* ================================================================== */

static void test_doctor_initialized_runtime(void) {
    asx_runtime rt;
    asx_doctor_report report;
    asx_status st;

    MUST_OK(asx_runtime_init_default(&rt));

    st = asx_doctor_run(&rt, &report);
    ASSERT(st == ASX_OK, "doctor run ok");
    ASSERT(report.check_count == 8, "8 checks");
    ASSERT(asx_doctor_is_healthy(&report), "healthy");
    ASSERT(asx_doctor_overall(&report) == ASX_DOCTOR_OK, "overall ok");

    /* First check should be runtime=OK */
    ASSERT(strcmp(report.checks[0].name, "runtime") == 0, "first check is runtime");
    ASSERT(report.checks[0].severity == ASX_DOCTOR_OK, "runtime ok");

    asx_runtime_shutdown(&rt);
}

static void test_doctor_uninitialized_runtime(void) {
    asx_runtime rt;
    asx_doctor_report report;

    memset(&rt, 0, sizeof(rt));

    MUST_OK(asx_doctor_run(&rt, &report));
    ASSERT(report.fail_count > 0, "uninitialized has fails");
    ASSERT(!asx_doctor_is_healthy(&report), "not healthy");
    ASSERT(asx_doctor_overall(&report) == ASX_DOCTOR_FAIL, "overall fail");
}

static void test_doctor_null_args(void) {
    asx_runtime rt;
    asx_doctor_report report;

    ASSERT(asx_doctor_run(NULL, &report) == ASX_E_INVALID_ARGUMENT, "null rt");
    ASSERT(asx_doctor_run(&rt, NULL) == ASX_E_INVALID_ARGUMENT, "null report");
    ASSERT(asx_doctor_is_healthy(NULL) == 0, "null report not healthy");
    ASSERT(asx_doctor_overall(NULL) == ASX_DOCTOR_FAIL, "null report overall fail");
}

static void test_doctor_check_fields(void) {
    asx_runtime rt;
    asx_doctor_report report;
    uint32_t i;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_doctor_run(&rt, &report));

    /* Every check should have a non-NULL name and message */
    for (i = 0; i < report.check_count; i++) {
        ASSERT(report.checks[i].name != NULL, "check has name");
        ASSERT(report.checks[i].message != NULL, "check has message");
    }

    ASSERT(strcmp(report.checks[4].name, "io_driver") == 0, "io driver check present");
    ASSERT(strcmp(report.checks[5].name, "blocking") == 0, "blocking check present");

    /* pass + warn + fail should equal check_count */
    ASSERT(report.pass_count + report.warn_count + report.fail_count == report.check_count,
           "counts sum to total");

    asx_runtime_shutdown(&rt);
}
#else
static int g_pass, g_fail;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

static void test_app_surface_compile_time_hidden_in_browser(void) {
    ASSERT(ASX_HAS_NATIVE_RUNTIME_SURFACES == 0, "native runtime surfaces hidden");
}
#endif

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void) {
    printf("test_app:\n");

#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    /* Net: socket address */
    RUN(test_socket_addr_ipv4);
    RUN(test_socket_addr_loopback);
    RUN(test_socket_addr_ipv6_loopback);
    RUN(test_socket_addr_eq);

    /* Net: TCP listener */
    RUN(test_tcp_listener_bind_close);
    RUN(test_tcp_listener_local_addr);
    RUN(test_tcp_listener_poll_accept_pending);
    RUN(test_tcp_listener_poll_accept_ready_for_loopback_connect);
    RUN(test_tcp_listener_bind_with_cx_permission_denied);
    RUN(test_tcp_listener_poll_accept_with_cx_budget_checkpoint);
    RUN(test_tcp_listener_null_args);
    RUN(test_tcp_listener_arena_exhaustion);

    /* Net: TCP stream */
    RUN(test_tcp_connect_close);
    RUN(test_tcp_connect_with_cx_permission_denied);
    RUN(test_tcp_stream_peer_addr);
    RUN(test_tcp_stream_poll_pending);
    RUN(test_tcp_stream_loopback_transfer);
    RUN(test_tcp_stream_poll_with_cx_budget_checkpoint);
    RUN(test_tcp_stream_poll_with_cx_permission_denied);
    RUN(test_tcp_stream_stale_handle);
    RUN(test_net_reset);

    /* Net: UDP + resolve */
    RUN(test_udp_bind_connect_and_addrs);
    RUN(test_udp_poll_pending);
    RUN(test_udp_loopback_delivery);
    RUN(test_udp_poll_requires_peer_or_destination);
    RUN(test_udp_with_cx_budget_and_permission);
    RUN(test_udp_arena_exhaustion);
    RUN(test_resolve_localhost_prefers_ipv6);
    RUN(test_resolve_ipv4_literal);
    RUN(test_resolve_rejects_unknown_host);
    RUN(test_resolver_cache_round_trip);
    RUN(test_resolver_negative_cache_and_invalidate);
    RUN(test_happy_eyeballs_reorders_preference);
    RUN(test_tcp_connect_host_uses_resolver_cache);
    RUN(test_udp_connect_host_uses_resolver_cache);

    /* App: CLI parsing */
    RUN(test_parse_args_defaults);
    RUN(test_parse_args_doctor);
    RUN(test_parse_args_replay);
    RUN(test_parse_args_server);
    RUN(test_parse_args_verbose);
    RUN(test_parse_args_seed);
    RUN(test_parse_args_help);
    RUN(test_parse_args_unknown_rejected);
    RUN(test_parse_args_replay_requires_scenario);
    RUN(test_parse_args_seed_requires_decimal);
    RUN(test_parse_args_seed_rejects_overflow);
    RUN(test_parse_args_null);

    /* App: lifecycle */
    RUN(test_app_init_shutdown);
    RUN(test_app_run_noop);
    RUN(test_app_run_with_cx_noop);
    RUN(test_app_run_with_cx_permission_denied);
    RUN(test_app_run_with_cx_region_mismatch);
    RUN(test_app_region);
    RUN(test_app_null_args);
    RUN(test_app_run_server_happy_path);
    RUN(test_app_run_server_with_cx_happy_path);
    RUN(test_app_run_server_requires_config);
    RUN(test_app_run_server_signal_shutdown);
    RUN(test_app_run_server_bootstrap_failure);
    RUN(test_app_run_server_with_cx_permission_denied_fails_closed);
    RUN(test_app_run_server_with_cx_region_mismatch_fails_closed);

    /* Doctor */
    RUN(test_doctor_initialized_runtime);
    RUN(test_doctor_uninitialized_runtime);
    RUN(test_doctor_null_args);
    RUN(test_doctor_check_fields);
#else
    RUN(test_app_surface_compile_time_hidden_in_browser);
#endif

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
