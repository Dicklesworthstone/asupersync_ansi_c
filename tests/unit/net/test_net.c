/*
 * test_net.c — unit tests for deterministic network surface
 *
 * Covers: socket addresses, TCP listener/stream lifecycle, UDP socket
 * lifecycle, resolver/cache, happy-eyeballs ordering, host-connect helpers,
 * arena exhaustion, generation-guarded handle safety, and reset.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/net.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Socket address tests                                                */
/* ------------------------------------------------------------------ */

TEST(ipv4_addr_construction) {
    asx_socket_addr sa = asx_socket_addr_ipv4(10, 0, 0, 1, 8080);
    ASSERT_EQ(sa.family, ASX_AF_INET4);
    ASSERT_EQ(sa.port, 8080);
    ASSERT_EQ(sa.addr[0], 10);
    ASSERT_EQ(sa.addr[1], 0);
    ASSERT_EQ(sa.addr[2], 0);
    ASSERT_EQ(sa.addr[3], 1);
}

TEST(loopback_addr) {
    asx_socket_addr sa = asx_socket_addr_loopback(443);
    ASSERT_EQ(sa.family, ASX_AF_INET4);
    ASSERT_EQ(sa.port, 443);
    ASSERT_EQ(sa.addr[0], 127);
    ASSERT_EQ(sa.addr[1], 0);
    ASSERT_EQ(sa.addr[2], 0);
    ASSERT_EQ(sa.addr[3], 1);
}

TEST(ipv6_loopback_addr) {
    asx_socket_addr sa = asx_socket_addr_ipv6_loopback(9090);
    uint8_t i;
    ASSERT_EQ(sa.family, ASX_AF_INET6);
    ASSERT_EQ(sa.port, 9090);
    for (i = 0; i < 15; i++) { ASSERT_EQ(sa.addr[i], 0); }
    ASSERT_EQ(sa.addr[15], 1);
}

TEST(addr_equality) {
    asx_socket_addr a = asx_socket_addr_loopback(80);
    asx_socket_addr b = asx_socket_addr_loopback(80);
    asx_socket_addr c = asx_socket_addr_loopback(81);
    asx_socket_addr d = asx_socket_addr_ipv4(10, 0, 0, 1, 80);

    ASSERT_TRUE(asx_socket_addr_eq(&a, &b));
    ASSERT_FALSE(asx_socket_addr_eq(&a, &c)); /* different port */
    ASSERT_FALSE(asx_socket_addr_eq(&a, &d)); /* different addr */
    ASSERT_FALSE(asx_socket_addr_eq(NULL, &a));
    ASSERT_FALSE(asx_socket_addr_eq(&a, NULL));
}

TEST(addr_family_mismatch) {
    asx_socket_addr v4 = asx_socket_addr_loopback(80);
    asx_socket_addr v6 = asx_socket_addr_ipv6_loopback(80);
    ASSERT_FALSE(asx_socket_addr_eq(&v4, &v6));
}

/* ------------------------------------------------------------------ */
/* TCP listener tests                                                  */
/* ------------------------------------------------------------------ */

TEST(tcp_listener_bind_and_local_addr) {
    asx_tcp_listener lis;
    asx_socket_addr addr = asx_socket_addr_loopback(5000);
    asx_socket_addr out;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_TRUE(asx_tcp_listener_is_alive(lis));
    ASSERT_EQ(asx_tcp_listener_local_addr(lis, &out), ASX_OK);
    ASSERT_TRUE(asx_socket_addr_eq(&addr, &out));
    asx_tcp_listener_close(lis);
}

TEST(tcp_listener_null_args) {
    asx_tcp_listener lis;
    asx_socket_addr addr = asx_socket_addr_loopback(5001);

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(NULL, &addr), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tcp_listener_bind(&lis, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(tcp_listener_close_makes_dead) {
    asx_tcp_listener lis;
    asx_socket_addr addr = asx_socket_addr_loopback(5002);

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_TRUE(asx_tcp_listener_is_alive(lis));
    asx_tcp_listener_close(lis);
    ASSERT_FALSE(asx_tcp_listener_is_alive(lis));
}

TEST(tcp_listener_accept_empty_returns_pending) {
    asx_tcp_listener lis;
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(5003);

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &stream, NULL), ASX_E_PENDING);
    asx_tcp_listener_close(lis);
}

TEST(tcp_listener_exhaustion) {
    asx_tcp_listener listeners[ASX_MAX_TCP_LISTENERS + 1];
    uint32_t i;

    asx_net_reset();
    for (i = 0; i < ASX_MAX_TCP_LISTENERS; i++) {
        asx_socket_addr addr = asx_socket_addr_loopback((uint16_t)(6000 + i));
        ASSERT_EQ(asx_tcp_listener_bind(&listeners[i], &addr), ASX_OK);
    }
    {
        asx_socket_addr addr = asx_socket_addr_loopback(6999);
        ASSERT_EQ(asx_tcp_listener_bind(&listeners[ASX_MAX_TCP_LISTENERS], &addr),
                  ASX_E_RESOURCE_EXHAUSTED);
    }
    for (i = 0; i < ASX_MAX_TCP_LISTENERS; i++) { asx_tcp_listener_close(listeners[i]); }
}

/* The in-memory port namespace (bd-9kll.10.2): port 0 takes an ephemeral
 * port a client can connect to, a listening address cannot be bound twice,
 * and it is free again once that listener closes. */
TEST(tcp_memory_port_namespace) {
    asx_tcp_listener lis, dup;
    asx_tcp_stream client, server;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_socket_addr bound;
    asx_socket_addr fixed = asx_socket_addr_loopback(6100);

    asx_net_reset();
    if (asx_net_get_backend() != ASX_NET_BACKEND_MEMORY) return;
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &any), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_local_addr(lis, &bound), ASX_OK);
    ASSERT_TRUE(bound.port >= 49152u);
    ASSERT_EQ(asx_tcp_connect(&client, &bound), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);
    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);

    ASSERT_EQ(asx_tcp_listener_bind(&lis, &fixed), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_bind(&dup, &fixed), ASX_E_ALREADY_EXISTS);
    asx_tcp_listener_close(lis);
    ASSERT_EQ(asx_tcp_listener_bind(&dup, &fixed), ASX_OK);
    asx_tcp_listener_close(dup);
}

/* A listener on the unspecified address (0.0.0.0 / ::) takes connections
 * to its port, and holds that port against specific binds and vice versa,
 * as the OS's does (bd-9kll.10.3). */
TEST(tcp_memory_wildcard_listener) {
    static const uint8_t v6_any[16] = {0};
    asx_tcp_listener lis, dup;
    asx_tcp_stream client, server;
    asx_socket_addr wild4 = asx_socket_addr_ipv4(0, 0, 0, 0, 6200);
    asx_socket_addr wild6 = asx_socket_addr_ipv6(v6_any, 6201);
    asx_socket_addr lo4 = asx_socket_addr_loopback(6200);
    asx_socket_addr lo6 = asx_socket_addr_ipv6_loopback(6201);

    asx_net_reset();
    if (asx_net_get_backend() != ASX_NET_BACKEND_MEMORY) return;
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &wild4), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_bind(&dup, &lo4), ASX_E_ALREADY_EXISTS);
    ASSERT_EQ(asx_tcp_connect(&client, &lo4), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);
    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);

    ASSERT_EQ(asx_tcp_listener_bind(&lis, &lo6), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_bind(&dup, &wild6), ASX_E_ALREADY_EXISTS);
    asx_tcp_listener_close(lis);
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &wild6), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &lo6), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);
    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);
}

/* ------------------------------------------------------------------ */
/* TCP stream tests                                                    */
/* ------------------------------------------------------------------ */

TEST(tcp_connect_loopback_creates_linked_pair) {
    asx_tcp_listener lis;
    asx_tcp_stream client, accepted;
    asx_socket_addr addr = asx_socket_addr_loopback(7000);
    asx_socket_addr peer_addr;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_TRUE(asx_tcp_stream_is_alive(client));

    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &accepted, &peer_addr), ASX_OK);
    ASSERT_TRUE(asx_tcp_stream_is_alive(accepted));

    asx_tcp_stream_close(client);
    asx_tcp_stream_close(accepted);
    asx_tcp_listener_close(lis);
}

TEST(tcp_bidirectional_io) {
    asx_tcp_listener lis;
    asx_tcp_stream client, server;
    asx_socket_addr addr = asx_socket_addr_loopback(7001);
    uint8_t write_data[] = "hello";
    uint8_t read_buf[32];
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);

    /* client writes, server reads */
    src = asx_buf_from(write_data, 5);
    ASSERT_EQ(asx_tcp_stream_poll_write(client, &src, &written), ASX_OK);
    ASSERT_EQ(written, 5u);

    (void)read_buf;
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_tcp_stream_poll_read(server, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 5u);
    ASSERT_TRUE(memcmp(dst.data, "hello", 5) == 0);

    /* server writes, client reads */
    {
        uint8_t reply[] = "world";
        asx_buf reply_src;
        asx_buf_mut reply_dst;

        reply_src = asx_buf_from(reply, 5);
        ASSERT_EQ(asx_tcp_stream_poll_write(server, &reply_src, &written), ASX_OK);
        ASSERT_EQ(written, 5u);

        asx_buf_mut_init(&reply_dst);
        ASSERT_EQ(asx_tcp_stream_poll_read(client, &reply_dst, &read_n), ASX_OK);
        ASSERT_EQ(read_n, 5u);
        ASSERT_TRUE(memcmp(reply_dst.data, "world", 5) == 0);
    }

    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);
}

TEST(tcp_read_empty_returns_pending) {
    asx_tcp_listener lis;
    asx_tcp_stream client, server;
    asx_socket_addr addr = asx_socket_addr_loopback(7002);
    asx_buf_mut dst;
    uint32_t read_n;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_tcp_stream_poll_read(server, &dst, &read_n), ASX_E_PENDING);
    ASSERT_EQ(read_n, 0u);

    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);
}

typedef struct {
    asx_tcp_stream stream;
    uint32_t polls;
    uint32_t got;
    int eof;
} read_loop;

static asx_status read_loop_poll(void *ud, asx_task_id self) {
    read_loop *rl = (read_loop *)ud;
    uint8_t buf[16];
    uint32_t n = 0u;
    asx_status st;
    (void)self;
    for (;;) {
        rl->polls++;
        st = asx_tcp_stream_read(rl->stream, buf, sizeof(buf), &n);
        if (st != ASX_OK) return st;
        if (n == 0u) {
            rl->eof = 1;
            return ASX_OK;
        }
        rl->got += n;
    }
}

/* An empty in-memory stream parks its reader (bd-9kll.10.2): the peer's
 * write wakes it with the data, the peer's close with EOF, and in between
 * it is not polled. */
TEST(tcp_memory_read_parks_until_data_or_eof) {
    asx_tcp_listener lis;
    asx_tcp_stream client, server;
    asx_socket_addr addr = asx_socket_addr_loopback(7012);
    read_loop rl;
    asx_region_id r;
    asx_task_id t;
    asx_budget b;
    uint32_t written;
    uint32_t polls;

    asx_runtime_reset();
    asx_net_reset();
    if (asx_net_get_backend() != ASX_NET_BACKEND_MEMORY) return;
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);
    memset(&rl, 0, sizeof(rl));
    rl.stream = server;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, read_loop_poll, &rl, &t), ASX_OK);

    b = asx_budget_from_polls(1000000);
    ASSERT_EQ(asx_scheduler_run(r, &b), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(rl.polls, 1u);

    ASSERT_EQ(asx_tcp_stream_write(client, (const uint8_t *)"hello", 5u, &written), ASX_OK);
    b = asx_budget_from_polls(1000000);
    ASSERT_EQ(asx_scheduler_run(r, &b), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(rl.got, 5u);
    polls = rl.polls;
    ASSERT_TRUE(polls < 10u);

    ASSERT_EQ(asx_tcp_stream_close(client), ASX_OK);
    b = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(r, &b), ASX_OK);
    ASSERT_TRUE(rl.eof);
    ASSERT_TRUE(rl.polls < polls + 3u);

    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);
}

TEST(tcp_close_unlinks_peer) {
    asx_tcp_listener lis;
    asx_tcp_stream client, server;
    asx_socket_addr addr = asx_socket_addr_loopback(7003);
    uint8_t data[] = "test";
    asx_buf src;
    uint32_t written;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);

    asx_tcp_stream_close(server);
    /* Peer is gone: writes fail like EPIPE instead of pending forever,
     * and reads report EOF (OK with zero bytes). */
    src = asx_buf_from(data, 4);
    ASSERT_EQ(asx_tcp_stream_poll_write(client, &src, &written), ASX_E_DISCONNECTED);
    {
        uint8_t buf[8];
        uint32_t n = 99u;
        ASSERT_EQ(asx_tcp_stream_read(client, buf, (uint32_t)sizeof(buf), &n), ASX_OK);
        ASSERT_EQ(n, 0u);
    }

    asx_tcp_stream_close(client);
    asx_tcp_listener_close(lis);
}

TEST(tcp_shutdown_write_delivers_eof_after_data) {
    asx_tcp_listener lis;
    asx_tcp_stream client, server;
    asx_socket_addr addr = asx_socket_addr_loopback(7013);
    uint8_t buf[16];
    uint32_t n;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);

    ASSERT_EQ(asx_tcp_stream_read(server, buf, (uint32_t)sizeof(buf), &n), ASX_E_PENDING);
    ASSERT_EQ(asx_tcp_stream_write(client, (const uint8_t *)"abc", 3u, &n), ASX_OK);
    ASSERT_EQ(n, 3u);
    ASSERT_EQ(asx_tcp_stream_shutdown_write(client), ASX_OK);
    ASSERT_EQ(asx_tcp_stream_write(client, (const uint8_t *)"x", 1u, &n), ASX_E_INVALID_STATE);

    /* Buffered data first, then EOF; the server can still reply. */
    ASSERT_EQ(asx_tcp_stream_read(server, buf, (uint32_t)sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 3u);
    ASSERT_EQ(asx_tcp_stream_read(server, buf, (uint32_t)sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 0u);
    ASSERT_EQ(asx_tcp_stream_write(server, (const uint8_t *)"ok", 2u, &n), ASX_OK);
    ASSERT_EQ(asx_tcp_stream_read(client, buf, (uint32_t)sizeof(buf), &n), ASX_OK);
    ASSERT_EQ(n, 2u);

    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);
}

TEST(tcp_stale_handle_after_close_does_not_alias) {
    asx_tcp_listener lis;
    asx_tcp_stream first, second, accepted;
    asx_socket_addr addr = asx_socket_addr_loopback(7014);

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&first, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &accepted, NULL), ASX_OK);
    ASSERT_EQ(asx_tcp_stream_close(first), ASX_OK);
    ASSERT_EQ(asx_tcp_stream_close(accepted), ASX_OK);

    /* The next stream reuses the slot but not the generation. */
    ASSERT_EQ(asx_tcp_connect(&second, &addr), ASX_OK);
    ASSERT_EQ(second.slot, first.slot);
    ASSERT_TRUE(second.generation != first.generation);
    ASSERT_FALSE(asx_tcp_stream_is_alive(first));
    ASSERT_TRUE(asx_tcp_stream_is_alive(second));

    asx_tcp_listener_close(lis);
}

TEST(tcp_peer_addr) {
    asx_tcp_listener lis;
    asx_tcp_stream client;
    asx_socket_addr addr = asx_socket_addr_loopback(7004);
    asx_socket_addr peer;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_stream_peer_addr(client, &peer), ASX_OK);
    ASSERT_TRUE(asx_socket_addr_eq(&peer, &addr));

    asx_tcp_stream_close(client);
    asx_tcp_listener_close(lis);
}

TEST(tcp_connect_null_args) {
    asx_tcp_stream out;
    asx_socket_addr addr = asx_socket_addr_loopback(7005);

    asx_net_reset();
    ASSERT_EQ(asx_tcp_connect(NULL, &addr), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tcp_connect(&out, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* UDP socket tests                                                    */
/* ------------------------------------------------------------------ */

TEST(udp_bind_and_local_addr) {
    asx_udp_socket sock;
    asx_socket_addr addr = asx_socket_addr_loopback(8000);
    asx_socket_addr out;

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(&sock, &addr), ASX_OK);
    ASSERT_TRUE(asx_udp_is_alive(sock));
    ASSERT_EQ(asx_udp_local_addr(sock, &out), ASX_OK);
    ASSERT_TRUE(asx_socket_addr_eq(&addr, &out));
    asx_udp_close(sock);
}

TEST(udp_connect_and_peer_addr) {
    asx_udp_socket sock;
    asx_socket_addr local = asx_socket_addr_loopback(8001);
    asx_socket_addr peer = asx_socket_addr_loopback(8002);
    asx_socket_addr out;

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(&sock, &local), ASX_OK);
    ASSERT_EQ(asx_udp_connect(sock, &peer), ASX_OK);
    ASSERT_EQ(asx_udp_peer_addr(sock, &out), ASX_OK);
    ASSERT_TRUE(asx_socket_addr_eq(&peer, &out));
    asx_udp_close(sock);
}

TEST(udp_send_recv_loopback) {
    asx_udp_socket sender, receiver;
    asx_socket_addr sender_addr = asx_socket_addr_loopback(8010);
    asx_socket_addr recv_addr = asx_socket_addr_loopback(8011);
    uint8_t payload[] = "ping";
    uint8_t buf[32];
    asx_buf src;
    asx_buf_mut dst;
    asx_socket_addr from;
    uint32_t written, read_n;

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(&sender, &sender_addr), ASX_OK);
    ASSERT_EQ(asx_udp_bind(&receiver, &recv_addr), ASX_OK);

    src = asx_buf_from(payload, 4);
    ASSERT_EQ(asx_udp_poll_send(sender, &src, &written, &recv_addr), ASX_OK);
    ASSERT_EQ(written, 4u);

    memset(buf, 0, sizeof(buf));
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_udp_poll_recv(receiver, &dst, &read_n, &from), ASX_OK);
    ASSERT_EQ(read_n, 4u);
    ASSERT_TRUE(memcmp(dst.data, "ping", 4) == 0);
    ASSERT_TRUE(asx_socket_addr_eq(&from, &sender_addr));

    asx_udp_close(sender);
    asx_udp_close(receiver);
}

TEST(udp_recv_empty_returns_pending) {
    asx_udp_socket sock;
    asx_socket_addr addr = asx_socket_addr_loopback(8020);
    asx_buf_mut dst;
    uint32_t read_n;

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(&sock, &addr), ASX_OK);
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_udp_poll_recv(sock, &dst, &read_n, NULL), ASX_E_PENDING);
    ASSERT_EQ(read_n, 0u);
    asx_udp_close(sock);
}

TEST(udp_send_no_peer_no_dest_fails) {
    asx_udp_socket sock;
    asx_socket_addr addr = asx_socket_addr_loopback(8030);
    uint8_t payload[] = "x";
    asx_buf src;
    uint32_t written;

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(&sock, &addr), ASX_OK);
    src = asx_buf_from(payload, 1);
    ASSERT_EQ(asx_udp_poll_send(sock, &src, &written, NULL), ASX_E_INVALID_ARGUMENT);
    asx_udp_close(sock);
}

TEST(udp_close_makes_dead) {
    asx_udp_socket sock;
    asx_socket_addr addr = asx_socket_addr_loopback(8040);

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(&sock, &addr), ASX_OK);
    ASSERT_TRUE(asx_udp_is_alive(sock));
    asx_udp_close(sock);
    ASSERT_FALSE(asx_udp_is_alive(sock));
}

TEST(udp_null_args) {
    asx_udp_socket sock;
    asx_socket_addr addr = asx_socket_addr_loopback(8050);

    asx_net_reset();
    ASSERT_EQ(asx_udp_bind(NULL, &addr), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_udp_bind(&sock, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Resolver and happy-eyeballs tests                                   */
/* ------------------------------------------------------------------ */

TEST(resolve_localhost_dual_stack) {
    asx_resolve_result result;
    asx_resolve_options opts;

    asx_resolve_options_init(&opts, 443);
    ASSERT_EQ(asx_resolve_host(&result, "localhost", &opts), ASX_OK);
    ASSERT_TRUE(result.count >= 2u);
    /* happy-eyeballs default prefers IPv6 */
    ASSERT_EQ(result.addrs[0].family, ASX_AF_INET6);
    ASSERT_EQ(result.addrs[0].port, 443);
    ASSERT_EQ(result.addrs[1].family, ASX_AF_INET4);
}

TEST(resolve_ipv4_literal) {
    asx_resolve_result result;
    asx_resolve_options opts;

    asx_resolve_options_init(&opts, 80);
    ASSERT_EQ(asx_resolve_host(&result, "192.168.1.1", &opts), ASX_OK);
    ASSERT_EQ(result.count, 1u);
    ASSERT_EQ(result.addrs[0].family, ASX_AF_INET4);
    ASSERT_EQ(result.addrs[0].addr[0], 192);
    ASSERT_EQ(result.addrs[0].addr[1], 168);
    ASSERT_EQ(result.addrs[0].addr[2], 1);
    ASSERT_EQ(result.addrs[0].addr[3], 1);
}

TEST(resolve_ipv6_loopback_literal) {
    asx_resolve_result result;
    asx_resolve_options opts;

    asx_resolve_options_init(&opts, 8080);
    ASSERT_EQ(asx_resolve_host(&result, "::1", &opts), ASX_OK);
    ASSERT_EQ(result.count, 1u);
    ASSERT_EQ(result.addrs[0].family, ASX_AF_INET6);
    ASSERT_EQ(result.addrs[0].port, 8080);
}

TEST(resolve_ipv6_literal_forms) {
    static const struct {
        const char *text;
        uint8_t bytes[16];
    } ok[] = {
        {"::", {0}},
        {"[::1]", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
        {"2001:db8::1", {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
        {"fe80::", {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"1:2:3:4:5:6:7:8", {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}},
        {"::ffff:192.0.2.33", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 33}},
        {"64:FF9B::C000:221", {0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0x02, 0x21}},
        {"1:0:0:0:0:0:0::", {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    };
    static const char *const bad[] = {
        ":",        ":::",     "1::2::3",   "12345::", "1:2:3:4:5:6:7:8:9",
        "g::1",     "1:",      "[::1",      "::1]",    "1:2:3:4:5:6:7",
        "::1%eth0", "::1.2.3", "1.2.3.4::", "[]",      "1:2:3:4:5:6:7:1.2.3.4"};
    asx_resolve_result result;
    asx_resolve_options opts;
    uint32_t i;

    asx_net_reset();
    asx_resolve_options_init(&opts, 443);
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        ASSERT_EQ(asx_resolve_host(&result, ok[i].text, &opts), ASX_OK);
        ASSERT_EQ(result.count, 1u);
        ASSERT_EQ(result.addrs[0].family, ASX_AF_INET6);
        ASSERT_EQ(result.addrs[0].port, 443);
        ASSERT_EQ(memcmp(result.addrs[0].addr, ok[i].bytes, 16), 0);
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ(asx_resolve_host(&result, bad[i], &opts), ASX_E_NOT_FOUND);
    }
    opts.allow_ipv6 = 0;
    ASSERT_EQ(asx_resolve_host(&result, "2001:db8::1", &opts), ASX_E_NOT_FOUND);
}

TEST(async_resolve_literal_completes_on_first_poll) {
    asx_resolve_request req;
    asx_resolve_result result;
    asx_resolve_options opts;

    asx_net_reset();
    asx_resolve_options_init(&opts, 53);
    ASSERT_EQ(asx_resolve_request_init(&req, "10.1.2.3", &opts), ASX_OK);
    ASSERT_EQ(asx_resolve_poll(&req, ASX_INVALID_ID, &result), ASX_OK);
    ASSERT_EQ(result.count, 1u);
    ASSERT_EQ(result.addrs[0].family, ASX_AF_INET4);
    ASSERT_EQ(result.addrs[0].addr[3], 3u);
    ASSERT_EQ(result.addrs[0].port, 53);
    /* Final status is stable. */
    memset(&result, 0, sizeof(result));
    ASSERT_EQ(asx_resolve_poll(&req, ASX_INVALID_ID, &result), ASX_OK);
    ASSERT_EQ(result.count, 1u);

    ASSERT_EQ(asx_resolve_request_init(&req, "localhost", NULL), ASX_OK);
    ASSERT_EQ(asx_resolve_poll(&req, ASX_INVALID_ID, &result), ASX_OK);
    ASSERT_EQ(result.count, 2u);
    ASSERT_EQ(result.addrs[0].family, ASX_AF_INET6);
}

TEST(async_resolve_name_needs_native_backend) {
    asx_resolve_request req;
    asx_resolve_result result;

    asx_net_reset(); /* MEMORY backend: no resolver for real names */
    ASSERT_EQ(asx_resolve_request_init(&req, "example.com", NULL), ASX_OK);
    ASSERT_EQ(asx_resolve_poll(&req, ASX_INVALID_ID, &result), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_resolve_poll(&req, ASX_INVALID_ID, NULL), ASX_E_NOT_FOUND);
    asx_resolve_request_cancel(&req); /* not in flight: no effect */
    ASSERT_EQ(asx_resolve_poll(&req, ASX_INVALID_ID, NULL), ASX_E_NOT_FOUND);
}

TEST(async_resolve_request_validation) {
    asx_resolve_request req;
    asx_resolve_options opts;
    char long_host[ASX_RESOLVER_HOST_CAPACITY + 8u];

    memset(long_host, 'a', sizeof(long_host) - 1u);
    long_host[sizeof(long_host) - 1u] = '\0';
    ASSERT_EQ(asx_resolve_request_init(NULL, "x", NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_resolve_request_init(&req, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_resolve_request_init(&req, "", NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_resolve_request_init(&req, long_host, NULL), ASX_E_INVALID_ARGUMENT);
    asx_resolve_options_init(&opts, 0);
    opts.allow_ipv4 = 0;
    opts.allow_ipv6 = 0;
    ASSERT_EQ(asx_resolve_request_init(&req, "localhost", &opts), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_resolve_poll(NULL, ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(resolve_unknown_host_not_found) {
    asx_resolve_result result;
    asx_resolve_options opts;

    asx_resolve_options_init(&opts, 80);
    ASSERT_EQ(asx_resolve_host(&result, "example.com", &opts), ASX_E_NOT_FOUND);
}

TEST(resolve_null_args) {
    asx_resolve_result result;
    asx_resolve_options opts;

    asx_resolve_options_init(&opts, 80);
    ASSERT_EQ(asx_resolve_host(NULL, "localhost", &opts), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_resolve_host(&result, NULL, &opts), ASX_E_INVALID_ARGUMENT);
}

TEST(happy_eyeballs_ipv4_preferred) {
    asx_resolve_result in, out;

    memset(&in, 0, sizeof(in));
    in.addrs[0] = asx_socket_addr_ipv6_loopback(80);
    in.addrs[1] = asx_socket_addr_loopback(80);
    in.count = 2;

    ASSERT_EQ(asx_happy_eyeballs_order(&out, &in, ASX_AF_INET4), ASX_OK);
    ASSERT_EQ(out.count, 2u);
    ASSERT_EQ(out.addrs[0].family, ASX_AF_INET4);
    ASSERT_EQ(out.addrs[1].family, ASX_AF_INET6);
}

TEST(resolver_cache_hit) {
    asx_resolver resolver;
    asx_resolve_result result1, result2;
    asx_resolve_options opts;
    uint8_t cache_hit;

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&opts, 80);

    ASSERT_EQ(asx_resolver_lookup(&resolver, "localhost", &opts, &result1, &cache_hit), ASX_OK);
    ASSERT_EQ(cache_hit, 0u);
    ASSERT_EQ(asx_resolver_cached_count(&resolver), 1u);

    ASSERT_EQ(asx_resolver_lookup(&resolver, "localhost", &opts, &result2, &cache_hit), ASX_OK);
    ASSERT_EQ(cache_hit, 1u);
    ASSERT_EQ(result1.count, result2.count);
}

TEST(resolver_invalidate) {
    asx_resolver resolver;
    asx_resolve_result result;
    asx_resolve_options opts;
    uint8_t cache_hit;

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&opts, 80);

    ASSERT_EQ(asx_resolver_lookup(&resolver, "localhost", &opts, &result, &cache_hit), ASX_OK);
    ASSERT_EQ(asx_resolver_cached_count(&resolver), 1u);
    asx_resolver_invalidate(&resolver, "localhost");
    ASSERT_EQ(asx_resolver_cached_count(&resolver), 0u);
}

TEST(resolver_reset_clears_all) {
    asx_resolver resolver;
    asx_resolve_result result;
    asx_resolve_options opts;
    uint8_t cache_hit;

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&opts, 80);

    ASSERT_EQ(asx_resolver_lookup(&resolver, "localhost", &opts, &result, &cache_hit), ASX_OK);
    ASSERT_EQ(asx_resolver_lookup(&resolver, "127.0.0.1", &opts, &result, &cache_hit), ASX_OK);
    ASSERT_EQ(asx_resolver_cached_count(&resolver), 2u);
    asx_resolver_reset(&resolver);
    ASSERT_EQ(asx_resolver_cached_count(&resolver), 0u);
}

/* ------------------------------------------------------------------ */
/* Host-connect helpers                                                */
/* ------------------------------------------------------------------ */

TEST(tcp_connect_host_localhost) {
    asx_tcp_listener lis;
    asx_tcp_stream client;
    asx_resolver resolver;
    asx_resolve_options opts;
    asx_socket_addr selected, listen_addr;
    uint8_t cache_hit;

    asx_net_reset();
    asx_resolver_init(&resolver);

    /* bind on IPv4 loopback */
    listen_addr = asx_socket_addr_loopback(9000);
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &listen_addr), ASX_OK);

    asx_resolve_options_init(&opts, 9000);
    opts.preferred_family = ASX_AF_INET4;
    opts.allow_ipv6 = 0;

    ASSERT_EQ(asx_tcp_connect_host(&client, &resolver, "localhost", &opts, &selected, &cache_hit),
              ASX_OK);
    ASSERT_TRUE(asx_tcp_stream_is_alive(client));
    ASSERT_EQ(selected.port, 9000);

    asx_tcp_stream_close(client);
    asx_tcp_listener_close(lis);
}

TEST(udp_connect_host_localhost) {
    asx_udp_socket sock;
    asx_resolver resolver;
    asx_resolve_options opts;
    asx_socket_addr local = asx_socket_addr_loopback(9010);
    asx_socket_addr selected;
    asx_socket_addr peer_out;
    uint8_t cache_hit;

    asx_net_reset();
    asx_resolver_init(&resolver);
    ASSERT_EQ(asx_udp_bind(&sock, &local), ASX_OK);

    asx_resolve_options_init(&opts, 9011);
    opts.preferred_family = ASX_AF_INET4;
    opts.allow_ipv6 = 0;

    ASSERT_EQ(asx_udp_connect_host(sock, &resolver, "localhost", &opts, &selected, &cache_hit),
              ASX_OK);
    ASSERT_EQ(asx_udp_peer_addr(sock, &peer_out), ASX_OK);
    ASSERT_EQ(peer_out.port, 9011);

    asx_udp_close(sock);
}

/* ------------------------------------------------------------------ */
/* Reset test                                                          */
/* ------------------------------------------------------------------ */

TEST(net_reset_clears_all_state) {
    asx_tcp_listener lis;
    asx_udp_socket sock;
    asx_socket_addr addr1 = asx_socket_addr_loopback(9100);
    asx_socket_addr addr2 = asx_socket_addr_loopback(9101);

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr1), ASX_OK);
    ASSERT_EQ(asx_udp_bind(&sock, &addr2), ASX_OK);

    asx_net_reset();
    ASSERT_FALSE(asx_tcp_listener_is_alive(lis));
    ASSERT_FALSE(asx_udp_is_alive(sock));
}

/* ------------------------------------------------------------------ */
/* Unix-domain address tests                                           */
/* ------------------------------------------------------------------ */

TEST(unix_addr_from_path) {
    asx_unix_addr addr;
    ASSERT_EQ(asx_unix_addr_from_path(&addr, "/tmp/test.sock"), ASX_OK);
    ASSERT_TRUE(addr.has_path);
    ASSERT_STR_EQ(addr.path, "/tmp/test.sock");
}

TEST(unix_addr_null_args) {
    asx_unix_addr addr;
    ASSERT_EQ(asx_unix_addr_from_path(NULL, "/tmp/t.sock"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_unix_addr_from_path(&addr, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_unix_addr_from_path(&addr, ""), ASX_E_INVALID_ARGUMENT);
}

TEST(unix_addr_equality) {
    asx_unix_addr a, b, c;
    asx_unix_addr_from_path(&a, "/tmp/a.sock");
    asx_unix_addr_from_path(&b, "/tmp/a.sock");
    asx_unix_addr_from_path(&c, "/tmp/b.sock");
    ASSERT_TRUE(asx_unix_addr_eq(&a, &b));
    ASSERT_FALSE(asx_unix_addr_eq(&a, &c));
    ASSERT_FALSE(asx_unix_addr_eq(NULL, &a));
}

/* ------------------------------------------------------------------ */
/* Unix-domain listener tests                                          */
/* ------------------------------------------------------------------ */

TEST(unix_listener_bind_and_local_addr) {
    asx_unix_listener lis;
    asx_unix_addr addr, out;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/lis.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_TRUE(asx_unix_listener_is_alive(lis));
    ASSERT_EQ(asx_unix_listener_local_addr(lis, &out), ASX_OK);
    ASSERT_TRUE(asx_unix_addr_eq(&addr, &out));
    asx_unix_listener_close(lis);
}

TEST(unix_listener_accept_empty_pending) {
    asx_unix_listener lis;
    asx_unix_stream stream;
    asx_unix_addr addr;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/empty.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_listener_poll_accept(lis, &stream), ASX_E_PENDING);
    asx_unix_listener_close(lis);
}

TEST(unix_listener_close_makes_dead) {
    asx_unix_listener lis;
    asx_unix_addr addr;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/dead.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    asx_unix_listener_close(lis);
    ASSERT_FALSE(asx_unix_listener_is_alive(lis));
}

/* ------------------------------------------------------------------ */
/* Unix-domain stream tests                                            */
/* ------------------------------------------------------------------ */

TEST(unix_connect_creates_linked_pair) {
    asx_unix_listener lis;
    asx_unix_stream client, accepted;
    asx_unix_addr addr;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/pair.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_connect(&client, &addr), ASX_OK);
    ASSERT_TRUE(asx_unix_stream_is_alive(client));
    ASSERT_EQ(asx_unix_listener_poll_accept(lis, &accepted), ASX_OK);
    ASSERT_TRUE(asx_unix_stream_is_alive(accepted));

    asx_unix_stream_close(client);
    asx_unix_stream_close(accepted);
    asx_unix_listener_close(lis);
}

TEST(unix_stream_bidirectional_io) {
    asx_unix_listener lis;
    asx_unix_stream client, server;
    asx_unix_addr addr;
    uint8_t data[] = "unix-hello";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/bio.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_listener_poll_accept(lis, &server), ASX_OK);

    src = asx_buf_from(data, 10);
    ASSERT_EQ(asx_unix_stream_poll_write(client, &src, &written), ASX_OK);
    ASSERT_EQ(written, 10u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_unix_stream_poll_read(server, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 10u);
    ASSERT_TRUE(memcmp(dst.data, "unix-hello", 10) == 0);

    asx_unix_stream_close(client);
    asx_unix_stream_close(server);
    asx_unix_listener_close(lis);
}

TEST(unix_stream_close_unlinks_peer) {
    asx_unix_listener lis;
    asx_unix_stream client, server;
    asx_unix_addr addr;
    uint8_t data[] = "x";
    asx_buf src;
    uint32_t written;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/unlink.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_listener_poll_accept(lis, &server), ASX_OK);

    asx_unix_stream_close(server);
    src = asx_buf_from(data, 1);
    ASSERT_EQ(asx_unix_stream_poll_write(client, &src, &written), ASX_E_PENDING);

    asx_unix_stream_close(client);
    asx_unix_listener_close(lis);
}

/* ------------------------------------------------------------------ */
/* Unix-domain datagram tests                                          */
/* ------------------------------------------------------------------ */

TEST(unix_dgram_send_recv) {
    asx_unix_dgram sender, receiver;
    asx_unix_addr send_addr, recv_addr;
    uint8_t payload[] = "dgram";
    asx_buf src;
    asx_buf_mut dst;
    asx_unix_addr from;
    uint32_t written, read_n;

    asx_net_reset();
    asx_unix_addr_from_path(&send_addr, "/tmp/send.sock");
    asx_unix_addr_from_path(&recv_addr, "/tmp/recv.sock");
    ASSERT_EQ(asx_unix_dgram_bind(&sender, &send_addr), ASX_OK);
    ASSERT_EQ(asx_unix_dgram_bind(&receiver, &recv_addr), ASX_OK);

    src = asx_buf_from(payload, 5);
    ASSERT_EQ(asx_unix_dgram_poll_send(sender, &src, &written, &recv_addr), ASX_OK);
    ASSERT_EQ(written, 5u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_unix_dgram_poll_recv(receiver, &dst, &read_n, &from), ASX_OK);
    ASSERT_EQ(read_n, 5u);
    ASSERT_TRUE(memcmp(dst.data, "dgram", 5) == 0);
    ASSERT_TRUE(asx_unix_addr_eq(&from, &send_addr));

    asx_unix_dgram_close(sender);
    asx_unix_dgram_close(receiver);
}

TEST(unix_dgram_recv_empty_pending) {
    asx_unix_dgram sock;
    asx_unix_addr addr;
    asx_buf_mut dst;
    uint32_t read_n;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/empty_dgram.sock");
    ASSERT_EQ(asx_unix_dgram_bind(&sock, &addr), ASX_OK);
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_unix_dgram_poll_recv(sock, &dst, &read_n, NULL), ASX_E_PENDING);
    asx_unix_dgram_close(sock);
}

TEST(unix_dgram_close_makes_dead) {
    asx_unix_dgram sock;
    asx_unix_addr addr;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/dead_dgram.sock");
    ASSERT_EQ(asx_unix_dgram_bind(&sock, &addr), ASX_OK);
    ASSERT_TRUE(asx_unix_dgram_is_alive(sock));
    asx_unix_dgram_close(sock);
    ASSERT_FALSE(asx_unix_dgram_is_alive(sock));
}

/* ------------------------------------------------------------------ */
/* Ancillary data tests                                                */
/* ------------------------------------------------------------------ */

TEST(ancillary_push_pop) {
    asx_ancillary anc;
    uint32_t fd;

    asx_ancillary_init(&anc);
    ASSERT_EQ(asx_ancillary_count(&anc), 0u);

    ASSERT_EQ(asx_ancillary_push_fd(&anc, 42), ASX_OK);
    ASSERT_EQ(asx_ancillary_push_fd(&anc, 99), ASX_OK);
    ASSERT_EQ(asx_ancillary_count(&anc), 2u);

    ASSERT_EQ(asx_ancillary_pop_fd(&anc, &fd), ASX_OK);
    ASSERT_EQ(fd, 42u);
    ASSERT_EQ(asx_ancillary_pop_fd(&anc, &fd), ASX_OK);
    ASSERT_EQ(fd, 99u);
    ASSERT_EQ(asx_ancillary_pop_fd(&anc, &fd), ASX_E_NOT_FOUND);
}

TEST(ancillary_exhaustion) {
    asx_ancillary anc;
    uint32_t i;

    asx_ancillary_init(&anc);
    for (i = 0; i < ASX_ANCILLARY_MAX_FDS; i++) {
        ASSERT_EQ(asx_ancillary_push_fd(&anc, i), ASX_OK);
    }
    ASSERT_EQ(asx_ancillary_push_fd(&anc, 999), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(ancillary_send_recv_over_unix_stream) {
    asx_unix_listener lis;
    asx_unix_stream client, server;
    asx_unix_addr addr;
    asx_ancillary send_anc, recv_anc;
    uint32_t fd;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/anc.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_listener_poll_accept(lis, &server), ASX_OK);

    asx_ancillary_init(&send_anc);
    ASSERT_EQ(asx_ancillary_push_fd(&send_anc, 7), ASX_OK);
    ASSERT_EQ(asx_ancillary_push_fd(&send_anc, 13), ASX_OK);

    ASSERT_EQ(asx_unix_stream_send_ancillary(client, &send_anc), ASX_OK);

    ASSERT_EQ(asx_unix_stream_recv_ancillary(server, &recv_anc), ASX_OK);
    ASSERT_EQ(asx_ancillary_count(&recv_anc), 2u);
    ASSERT_EQ(asx_ancillary_pop_fd(&recv_anc, &fd), ASX_OK);
    ASSERT_EQ(fd, 7u);
    ASSERT_EQ(asx_ancillary_pop_fd(&recv_anc, &fd), ASX_OK);
    ASSERT_EQ(fd, 13u);

    asx_unix_stream_close(client);
    asx_unix_stream_close(server);
    asx_unix_listener_close(lis);
}

/* ------------------------------------------------------------------ */
/* Split-stream tests                                                  */
/* ------------------------------------------------------------------ */

TEST(tcp_split_stream_io) {
    asx_tcp_listener lis;
    asx_tcp_stream client, server;
    asx_socket_addr addr = asx_socket_addr_loopback(11000);
    asx_read_half rd;
    asx_write_half wr;
    uint8_t data[] = "split";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_net_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &server, NULL), ASX_OK);

    ASSERT_EQ(asx_tcp_stream_split(client, &rd, &wr), ASX_OK);
    ASSERT_TRUE(asx_read_half_is_active(&rd));
    ASSERT_TRUE(asx_write_half_is_active(&wr));

    /* Write through write half, read from server */
    src = asx_buf_from(data, 5);
    ASSERT_EQ(asx_write_half_poll_write(&wr, &src, &written), ASX_OK);
    ASSERT_EQ(written, 5u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_tcp_stream_poll_read(server, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 5u);

    /* Write from server, read through read half */
    {
        uint8_t reply[] = "back";
        asx_buf reply_src = asx_buf_from(reply, 4);
        asx_buf_mut reply_dst;

        ASSERT_EQ(asx_tcp_stream_poll_write(server, &reply_src, &written), ASX_OK);
        asx_buf_mut_init(&reply_dst);
        ASSERT_EQ(asx_read_half_poll_read(&rd, &reply_dst, &read_n), ASX_OK);
        ASSERT_EQ(read_n, 4u);
        ASSERT_TRUE(memcmp(reply_dst.data, "back", 4) == 0);
    }

    asx_read_half_close(&rd);
    ASSERT_FALSE(asx_read_half_is_active(&rd));
    asx_write_half_close(&wr);
    ASSERT_FALSE(asx_write_half_is_active(&wr));

    asx_tcp_stream_close(client);
    asx_tcp_stream_close(server);
    asx_tcp_listener_close(lis);
}

TEST(unix_split_stream_io) {
    asx_unix_listener lis;
    asx_unix_stream client, server;
    asx_unix_addr addr;
    asx_read_half rd;
    asx_write_half wr;
    uint8_t data[] = "usplit";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_net_reset();
    asx_unix_addr_from_path(&addr, "/tmp/split.sock");
    ASSERT_EQ(asx_unix_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_unix_listener_poll_accept(lis, &server), ASX_OK);

    ASSERT_EQ(asx_unix_stream_split(client, &rd, &wr), ASX_OK);

    src = asx_buf_from(data, 6);
    ASSERT_EQ(asx_write_half_poll_write(&wr, &src, &written), ASX_OK);
    ASSERT_EQ(written, 6u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_unix_stream_poll_read(server, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 6u);
    ASSERT_TRUE(memcmp(dst.data, "usplit", 6) == 0);

    asx_read_half_close(&rd);
    asx_write_half_close(&wr);
    asx_unix_stream_close(client);
    asx_unix_stream_close(server);
    asx_unix_listener_close(lis);
}

TEST(split_null_args) {
    asx_read_half rd;
    asx_write_half wr;
    asx_tcp_stream fake;
    fake.slot = 0;
    fake.generation = 999;

    ASSERT_EQ(asx_tcp_stream_split(fake, NULL, &wr), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tcp_stream_split(fake, &rd, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(split_inactive_half_fails) {
    asx_read_half rd;
    asx_write_half wr;
    asx_buf_mut dst;
    asx_buf src;
    uint32_t n;

    memset(&rd, 0, sizeof(rd));
    memset(&wr, 0, sizeof(wr));
    rd.active = 0;
    wr.active = 0;

    asx_buf_mut_init(&dst);
    src = asx_buf_empty();
    ASSERT_EQ(asx_read_half_poll_read(&rd, &dst, &n), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_write_half_poll_write(&wr, &src, &n), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== net tests ===\n");

    /* Address construction */
    RUN_TEST(ipv4_addr_construction);
    RUN_TEST(loopback_addr);
    RUN_TEST(ipv6_loopback_addr);
    RUN_TEST(addr_equality);
    RUN_TEST(addr_family_mismatch);

    /* TCP listener */
    RUN_TEST(tcp_listener_bind_and_local_addr);
    RUN_TEST(tcp_listener_null_args);
    RUN_TEST(tcp_listener_close_makes_dead);
    RUN_TEST(tcp_listener_accept_empty_returns_pending);
    RUN_TEST(tcp_listener_exhaustion);
    RUN_TEST(tcp_memory_port_namespace);
    RUN_TEST(tcp_memory_wildcard_listener);

    /* TCP stream */
    RUN_TEST(tcp_connect_loopback_creates_linked_pair);
    RUN_TEST(tcp_bidirectional_io);
    RUN_TEST(tcp_read_empty_returns_pending);
    RUN_TEST(tcp_memory_read_parks_until_data_or_eof);
    RUN_TEST(tcp_close_unlinks_peer);
    RUN_TEST(tcp_shutdown_write_delivers_eof_after_data);
    RUN_TEST(tcp_stale_handle_after_close_does_not_alias);
    RUN_TEST(tcp_peer_addr);
    RUN_TEST(tcp_connect_null_args);

    /* UDP */
    RUN_TEST(udp_bind_and_local_addr);
    RUN_TEST(udp_connect_and_peer_addr);
    RUN_TEST(udp_send_recv_loopback);
    RUN_TEST(udp_recv_empty_returns_pending);
    RUN_TEST(udp_send_no_peer_no_dest_fails);
    RUN_TEST(udp_close_makes_dead);
    RUN_TEST(udp_null_args);

    /* Resolver / happy-eyeballs */
    RUN_TEST(resolve_localhost_dual_stack);
    RUN_TEST(resolve_ipv4_literal);
    RUN_TEST(resolve_ipv6_loopback_literal);
    RUN_TEST(resolve_ipv6_literal_forms);
    RUN_TEST(async_resolve_literal_completes_on_first_poll);
    RUN_TEST(async_resolve_name_needs_native_backend);
    RUN_TEST(async_resolve_request_validation);
    RUN_TEST(resolve_unknown_host_not_found);
    RUN_TEST(resolve_null_args);
    RUN_TEST(happy_eyeballs_ipv4_preferred);
    RUN_TEST(resolver_cache_hit);
    RUN_TEST(resolver_invalidate);
    RUN_TEST(resolver_reset_clears_all);

    /* Host-connect helpers */
    RUN_TEST(tcp_connect_host_localhost);
    RUN_TEST(udp_connect_host_localhost);

    /* Unix-domain address */
    RUN_TEST(unix_addr_from_path);
    RUN_TEST(unix_addr_null_args);
    RUN_TEST(unix_addr_equality);

    /* Unix-domain listener */
    RUN_TEST(unix_listener_bind_and_local_addr);
    RUN_TEST(unix_listener_accept_empty_pending);
    RUN_TEST(unix_listener_close_makes_dead);

    /* Unix-domain stream */
    RUN_TEST(unix_connect_creates_linked_pair);
    RUN_TEST(unix_stream_bidirectional_io);
    RUN_TEST(unix_stream_close_unlinks_peer);

    /* Unix-domain datagram */
    RUN_TEST(unix_dgram_send_recv);
    RUN_TEST(unix_dgram_recv_empty_pending);
    RUN_TEST(unix_dgram_close_makes_dead);

    /* Ancillary data */
    RUN_TEST(ancillary_push_pop);
    RUN_TEST(ancillary_exhaustion);
    RUN_TEST(ancillary_send_recv_over_unix_stream);

    /* Split-stream */
    RUN_TEST(tcp_split_stream_io);
    RUN_TEST(unix_split_stream_io);
    RUN_TEST(split_null_args);
    RUN_TEST(split_inactive_half_fails);

    /* Reset */
    RUN_TEST(net_reset_clears_all_state);

    TEST_REPORT();
    return test_failures;
}
