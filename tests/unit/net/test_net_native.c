/*
 * test_net_native.c — real sockets + readiness reactor + wake-driven
 * scheduler, end to end (POSIX, non-deterministic builds).
 *
 * Every scenario runs server and client as tasks in one region on one
 * thread: progress is only possible if I/O waits park tasks and the
 * reactor wakes exactly the right task. Poll counts are bounded to prove
 * the runtime blocks in epoll/poll instead of spinning.
 *
 * In deterministic or non-POSIX builds the file reports a skip: those
 * builds use the in-memory network by design.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/net/net.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/time/sleep.h>
#include <string.h>

#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC

static asx_runtime g_rt;

static int setup(void) {
    if (asx_runtime_init_default(&g_rt) != ASX_OK) return 0;
    return asx_net_get_backend() == ASX_NET_BACKEND_NATIVE;
}

/* -------------------------------------------------------------------
 * TCP echo (uppercase) server and client
 * ------------------------------------------------------------------- */

typedef struct {
    asx_co_state co;
    asx_tcp_listener listener;
    asx_tcp_stream conn;
    uint8_t buf[64];
    uint32_t len;
    uint32_t sent;
    uint32_t polls;
} server_state;

static asx_status server_poll(void *ud, asx_task_id self) {
    server_state *s = (server_state *)ud;
    asx_status st;
    uint32_t n;
    (void)self;
    s->polls++;
    ASX_CO_BEGIN(&s->co);
    for (;;) {
        st = asx_tcp_listener_poll_accept(s->listener, &s->conn, NULL);
        if (st == ASX_OK) break;
        if (st != ASX_E_PENDING) return st;
        ASX_CO_YIELD(&s->co);
    }
    /* Read a line. */
    while (s->len == 0u || s->buf[s->len - 1u] != '\n') {
        st = asx_tcp_stream_read(s->conn, &s->buf[s->len], (uint32_t)sizeof(s->buf) - s->len, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&s->co);
            continue;
        }
        if (st != ASX_OK) return st;
        if (n == 0u) return ASX_E_DISCONNECTED; /* EOF before newline */
        s->len += n;
    }
    for (n = 0; n < s->len; n++) {
        if (s->buf[n] >= 'a' && s->buf[n] <= 'z') s->buf[n] = (uint8_t)(s->buf[n] - 32u);
    }
    while (s->sent < s->len) {
        st = asx_tcp_stream_write(s->conn, &s->buf[s->sent], s->len - s->sent, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&s->co);
            continue;
        }
        if (st != ASX_OK) return st;
        s->sent += n;
    }
    (void)asx_tcp_stream_close(s->conn);
    ASX_CO_END(&s->co);
}

typedef struct {
    asx_co_state co;
    asx_socket_addr addr;
    asx_tcp_stream conn;
    const char *msg;
    uint32_t sent;
    uint8_t reply[64];
    uint32_t reply_len;
    uint32_t polls;
} client_state;

static asx_status client_poll(void *ud, asx_task_id self) {
    client_state *c = (client_state *)ud;
    asx_status st;
    uint32_t n;
    (void)self;
    c->polls++;
    ASX_CO_BEGIN(&c->co);
    st = asx_tcp_connect(&c->conn, &c->addr);
    if (st != ASX_OK) return st;
    while (c->sent < (uint32_t)strlen(c->msg)) {
        st = asx_tcp_stream_write(c->conn, (const uint8_t *)c->msg + c->sent,
                                  (uint32_t)strlen(c->msg) - c->sent, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&c->co);
            continue;
        }
        if (st != ASX_OK) return st;
        c->sent += n;
    }
    /* Read until EOF. */
    for (;;) {
        st = asx_tcp_stream_read(c->conn, &c->reply[c->reply_len],
                                 (uint32_t)sizeof(c->reply) - c->reply_len, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&c->co);
            continue;
        }
        if (st != ASX_OK) return st;
        if (n == 0u) break;
        c->reply_len += n;
    }
    (void)asx_tcp_stream_close(c->conn);
    ASX_CO_END(&c->co);
}

TEST(tcp_echo_over_real_sockets_parks_instead_of_spinning) {
    asx_region_id r;
    asx_task_id ts;
    asx_task_id tc;
    server_state *s = NULL;
    client_state *c = NULL;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, server_poll, (uint32_t)sizeof(server_state), NULL, &ts,
                                      (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_tcp_listener_bind(&s->listener, &any), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, client_poll, (uint32_t)sizeof(client_state), NULL, &tc,
                                      (void **)&c),
              ASX_OK);
    ASSERT_EQ(asx_tcp_listener_local_addr(s->listener, &c->addr), ASX_OK);
    ASSERT_TRUE(c->addr.port != 0u); /* ephemeral port was assigned */
    c->msg = "hello, asx\n";

    budget = asx_budget_from_polls(200);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(c->reply_len, (uint32_t)strlen("HELLO, ASX\n"));
    ASSERT_EQ(memcmp(c->reply, "HELLO, ASX\n", c->reply_len), 0);
    /* Wake-driven: a handful of polls each, not thousands of spins. */
    ASSERT_TRUE(s->polls < 20u);
    ASSERT_TRUE(c->polls < 20u);
    ASSERT_EQ(asx_tcp_listener_close(s->listener), ASX_OK);
}

/* -------------------------------------------------------------------
 * Many concurrent connections
 * ------------------------------------------------------------------- */

#define FANOUT 8u

typedef struct {
    asx_co_state co;
    asx_tcp_listener listener;
    asx_tcp_stream conns[FANOUT];
    uint32_t accepted;
    uint32_t done;
    uint8_t got[FANOUT];
} multi_server_state;

static asx_status multi_server_poll(void *ud, asx_task_id self) {
    multi_server_state *s = (multi_server_state *)ud;
    asx_status st;
    uint32_t i;
    (void)self;
    while (s->accepted < FANOUT) {
        st = asx_tcp_listener_poll_accept(s->listener, &s->conns[s->accepted], NULL);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) return st;
        s->accepted++;
    }
    for (i = 0; i < FANOUT; i++) {
        uint8_t b;
        uint32_t n;
        if (s->got[i]) continue;
        st = asx_tcp_stream_read(s->conns[i], &b, 1u, &n);
        if (st == ASX_E_PENDING) continue;
        if (st != ASX_OK || n != 1u) return ASX_E_DISCONNECTED;
        s->got[i] = 1u;
        st = asx_tcp_stream_write(s->conns[i], &b, 1u, &n);
        if (st != ASX_OK || n != 1u) return ASX_E_DISCONNECTED;
        s->done++;
    }
    return s->done == FANOUT ? ASX_OK : ASX_E_PENDING;
}

typedef struct {
    asx_co_state co;
    asx_socket_addr addr;
    asx_tcp_stream conn;
    uint8_t id;
    uint8_t echo;
} multi_client_state;

static asx_status multi_client_poll(void *ud, asx_task_id self) {
    multi_client_state *c = (multi_client_state *)ud;
    asx_status st;
    uint32_t n;
    (void)self;
    ASX_CO_BEGIN(&c->co);
    st = asx_tcp_connect(&c->conn, &c->addr);
    if (st != ASX_OK) return st;
    for (;;) {
        st = asx_tcp_stream_write(c->conn, &c->id, 1u, &n);
        if (st == ASX_OK && n == 1u) break;
        if (st != ASX_E_PENDING) return st == ASX_OK ? ASX_E_DISCONNECTED : st;
        ASX_CO_YIELD(&c->co);
    }
    for (;;) {
        st = asx_tcp_stream_read(c->conn, &c->echo, 1u, &n);
        if (st == ASX_OK && n == 1u) break;
        if (st != ASX_E_PENDING) return ASX_E_DISCONNECTED;
        ASX_CO_YIELD(&c->co);
    }
    (void)asx_tcp_stream_close(c->conn);
    ASX_CO_END(&c->co);
}

TEST(tcp_fanout_many_clients_one_server) {
    asx_region_id r;
    asx_task_id t;
    multi_server_state *s = NULL;
    multi_client_state *c[FANOUT];
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_socket_addr bound;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, multi_server_poll, (uint32_t)sizeof(multi_server_state),
                                      NULL, &t, (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_tcp_listener_bind(&s->listener, &any), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_local_addr(s->listener, &bound), ASX_OK);
    for (i = 0; i < FANOUT; i++) {
        ASSERT_EQ(asx_task_spawn_captured(r, multi_client_poll,
                                          (uint32_t)sizeof(multi_client_state), NULL, &t,
                                          (void **)&c[i]),
                  ASX_OK);
        c[i]->addr = bound;
        c[i]->id = (uint8_t)(0x41u + i);
    }

    budget = asx_budget_from_polls(2000);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    for (i = 0; i < FANOUT; i++) ASSERT_EQ(c[i]->echo, (uint8_t)(0x41u + i));
    for (i = 0; i < FANOUT; i++) (void)asx_tcp_stream_close(s->conns[i]);
    ASSERT_EQ(asx_tcp_listener_close(s->listener), ASX_OK);
}

/* -------------------------------------------------------------------
 * Connection refused, EOF, UDP, and timers + I/O together
 * ------------------------------------------------------------------- */

typedef struct {
    asx_socket_addr addr;
    asx_tcp_stream conn;
    asx_status result;
    int connected;
} refuse_state;

static asx_status refuse_poll(void *ud, asx_task_id self) {
    refuse_state *s = (refuse_state *)ud;
    uint8_t b = 0;
    uint32_t n;
    asx_status st;
    (void)self;
    if (!s->connected) {
        st = asx_tcp_connect(&s->conn, &s->addr);
        if (st != ASX_OK) {
            s->result = st;
            return ASX_OK;
        }
        s->connected = 1;
    }
    st = asx_tcp_stream_write(s->conn, &b, 1u, &n);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    s->result = st;
    (void)asx_tcp_stream_close(s->conn);
    return ASX_OK;
}

TEST(tcp_connect_refused_reports_disconnected) {
    asx_region_id r;
    asx_task_id t;
    refuse_state *s = NULL;
    asx_tcp_listener l;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_budget budget;

    ASSERT_TRUE(setup());
    /* Grab a free port, then close it so nothing listens there. */
    ASSERT_EQ(asx_tcp_listener_bind(&l, &any), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, refuse_poll, (uint32_t)sizeof(refuse_state), NULL, &t,
                                      (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_tcp_listener_local_addr(l, &s->addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_close(l), ASX_OK);

    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(s->result, ASX_E_DISCONNECTED);
}

typedef struct {
    asx_udp_socket a;
    asx_udp_socket b;
    asx_socket_addr b_addr;
    uint8_t got[16];
    uint32_t got_len;
    asx_socket_addr from;
    int sent;
} udp_state;

static asx_status udp_poll(void *ud, asx_task_id self) {
    udp_state *s = (udp_state *)ud;
    asx_buf_mut buf;
    asx_buf msg = asx_buf_from("ping", 4u);
    uint32_t n;
    asx_status st;
    (void)self;
    if (!s->sent) {
        st = asx_udp_poll_send(s->a, &msg, &n, &s->b_addr);
        if (st == ASX_E_PENDING) return st;
        if (st != ASX_OK || n != 4u) return ASX_E_DISCONNECTED;
        s->sent = 1;
    }
    asx_buf_mut_init(&buf);
    st = asx_udp_poll_recv(s->b, &buf, &n, &s->from);
    if (st != ASX_OK) return st;
    memcpy(s->got, buf.data, n);
    s->got_len = n;
    return ASX_OK;
}

TEST(udp_datagram_between_real_sockets) {
    asx_region_id r;
    asx_task_id t;
    udp_state *s = NULL;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_socket_addr a_addr;
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, udp_poll, (uint32_t)sizeof(udp_state), NULL, &t,
                                      (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_udp_bind(&s->a, &any), ASX_OK);
    ASSERT_EQ(asx_udp_bind(&s->b, &any), ASX_OK);
    ASSERT_EQ(asx_udp_local_addr(s->b, &s->b_addr), ASX_OK);
    ASSERT_EQ(asx_udp_local_addr(s->a, &a_addr), ASX_OK);

    budget = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(s->got_len, 4u);
    ASSERT_EQ(memcmp(s->got, "ping", 4u), 0);
    ASSERT_EQ(s->from.port, a_addr.port);
    ASSERT_EQ(asx_udp_close(s->a), ASX_OK);
    ASSERT_EQ(asx_udp_close(s->b), ASX_OK);
}

typedef struct {
    asx_sleep_state sleep;
    uint32_t polls;
} sleeper;

static asx_status sleeper_poll(void *ud, asx_task_id self) {
    sleeper *s = (sleeper *)ud;
    s->polls++;
    return asx_sleep_poll(&s->sleep, self);
}

TEST(real_clock_sleep_blocks_in_reactor_without_spinning) {
    asx_region_id r;
    asx_task_id t;
    sleeper *s = NULL;
    asx_time start;
    asx_time end;
    asx_budget budget;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_runtime_clock_is_virtual(), 0);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(r, sleeper_poll, (uint32_t)sizeof(sleeper), NULL, &t,
                                      (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_sleep_init(&s->sleep, 30u * 1000000u), ASX_OK);

    ASSERT_EQ(asx_runtime_now_ns(&start), ASX_OK);
    budget = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_runtime_now_ns(&end), ASX_OK);
    ASSERT_TRUE(end - start >= (asx_time)(30u * 1000000u));
    ASSERT_TRUE(s->polls <= 3u);
}

static int run_native(void) {
    RUN_TEST(tcp_echo_over_real_sockets_parks_instead_of_spinning);
    RUN_TEST(tcp_fanout_many_clients_one_server);
    RUN_TEST(tcp_connect_refused_reports_disconnected);
    RUN_TEST(udp_datagram_between_real_sockets);
    RUN_TEST(real_clock_sleep_blocks_in_reactor_without_spinning);
    return 0;
}

#else

/* Deterministic / non-POSIX builds default to the in-memory network;
 * NATIVE is only selectable where the platform provides it. */
TEST(backend_selection_follows_build_capabilities) {
    asx_net_reset();
    ASSERT_EQ(asx_net_get_backend(), ASX_NET_BACKEND_MEMORY);
#if defined(ASX_PROFILE_POSIX)
    ASSERT_EQ(asx_net_set_backend(ASX_NET_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_net_get_backend(), ASX_NET_BACKEND_NATIVE);
#else
    ASSERT_EQ(asx_net_set_backend(ASX_NET_BACKEND_NATIVE), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_net_get_backend(), ASX_NET_BACKEND_MEMORY);
#endif
    asx_net_reset();
    ASSERT_EQ(asx_net_get_backend(), ASX_NET_BACKEND_MEMORY);
}

static int run_native(void) {
    fprintf(stderr, "  SKIP: socket I/O scenarios need a non-deterministic POSIX build\n");
    RUN_TEST(backend_selection_follows_build_capabilities);
    return 0;
}

#endif

int main(void) {
    fprintf(stderr, "=== test_net_native ===\n");
    (void)run_native();
    TEST_REPORT();
    return test_failures;
}
