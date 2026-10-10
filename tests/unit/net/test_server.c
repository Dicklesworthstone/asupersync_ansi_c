/*
 * test_server.c — unit tests for server connection and shutdown
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/server.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/runtime.h>
#include <string.h>

#if ASX_HAS_SERVER_SURFACE
static int server_surface_available(void) {
    return asx_surface_available_active(ASX_SURFACE_SERVER);
}

TEST(server_config_defaults) {
    asx_server_config cfg;
    asx_server_config_init(&cfg);
    ASSERT_EQ(cfg.max_connections, ASX_SERVER_MAX_CONNECTIONS);
    ASSERT_EQ(cfg.drain_timeout_ms, 30000u);
}

TEST(server_init_and_state) {
    asx_server srv;
    asx_server_config cfg;
    asx_server_config_init(&cfg);
    asx_server_init(&srv, &cfg);
    ASSERT_EQ(asx_server_get_state(&srv), ASX_SERVER_STATE_IDLE);
    ASSERT_EQ(asx_server_active_count(&srv), 0u);
    ASSERT_EQ(asx_server_total_accepted(&srv), 0u);
}

TEST(server_listen_and_accept) {
    asx_server srv;
    asx_server_config cfg;
    asx_server_conn conn;
    asx_tcp_stream client;
    asx_socket_addr addr;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14000);
    asx_server_init(&srv, &cfg);

    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }

    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);
    ASSERT_EQ(asx_server_get_state(&srv), ASX_SERVER_STATE_LISTENING);

    addr = asx_socket_addr_loopback(14000);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_server_poll_accept(&srv, &conn), ASX_OK);
    ASSERT_EQ(conn.active, 1u);
    ASSERT_EQ(asx_server_active_count(&srv), 1u);
    ASSERT_EQ(asx_server_total_accepted(&srv), 1u);

    asx_tcp_stream_close(client);
    asx_server_stop(&srv);
}

TEST(server_close_conn) {
    asx_server srv;
    asx_server_config cfg;
    asx_server_conn conn;
    asx_tcp_stream client;
    asx_socket_addr addr;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14001);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_server_close_conn(&srv, 1u), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);

    addr = asx_socket_addr_loopback(14001);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_server_poll_accept(&srv, &conn), ASX_OK);
    ASSERT_EQ(asx_server_active_count(&srv), 1u);

    ASSERT_EQ(asx_server_close_conn(&srv, conn.id), ASX_OK);
    ASSERT_EQ(asx_server_active_count(&srv), 0u);

    /* Closing again should fail */
    ASSERT_EQ(asx_server_close_conn(&srv, conn.id), ASX_E_NOT_FOUND);

    asx_tcp_stream_close(client);
    asx_server_stop(&srv);
}

TEST(server_accept_empty_pending) {
    asx_server srv;
    asx_server_config cfg;
    asx_server_conn conn;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14002);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);

    ASSERT_EQ(asx_server_poll_accept(&srv, &conn), ASX_E_PENDING);
    asx_server_stop(&srv);
}

TEST(server_graceful_shutdown) {
    asx_server srv;
    asx_server_config cfg;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14003);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_server_shutdown(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);

    ASSERT_EQ(asx_server_shutdown(&srv), ASX_OK);
    /* No active connections, so goes straight to STOPPED */
    ASSERT_EQ(asx_server_get_state(&srv), ASX_SERVER_STATE_STOPPED);
}

TEST(server_shutdown_with_active_conns_drains) {
    asx_server srv;
    asx_server_config cfg;
    asx_server_conn conn;
    asx_tcp_stream client;
    asx_socket_addr addr;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14004);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_server_shutdown(&srv), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_server_stop(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);

    addr = asx_socket_addr_loopback(14004);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    ASSERT_EQ(asx_server_poll_accept(&srv, &conn), ASX_OK);

    ASSERT_EQ(asx_server_shutdown(&srv), ASX_OK);
    ASSERT_EQ(asx_server_get_state(&srv), ASX_SERVER_STATE_DRAINING);
    ASSERT_EQ(asx_server_active_count(&srv), 1u);

    /* Force stop clears all */
    ASSERT_EQ(asx_server_stop(&srv), ASX_OK);
    ASSERT_EQ(asx_server_get_state(&srv), ASX_SERVER_STATE_STOPPED);
    ASSERT_EQ(asx_server_active_count(&srv), 0u);

    asx_tcp_stream_close(client);
}

TEST(server_listen_twice_fails) {
    asx_server srv;
    asx_server_config cfg;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14005);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);
    ASSERT_EQ(asx_server_listen(&srv), ASX_E_INVALID_STATE);
    asx_server_stop(&srv);
}

TEST(server_null_args) {
    ASSERT_EQ(asx_server_listen(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_server_shutdown(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_server_stop(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(server_multiple_clients) {
    asx_server srv;
    asx_server_config cfg;
    asx_server_conn c1, c2;
    asx_tcp_stream client1, client2;
    asx_socket_addr addr;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14010);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);

    addr = asx_socket_addr_loopback(14010);
    ASSERT_EQ(asx_tcp_connect(&client1, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&client2, &addr), ASX_OK);
    ASSERT_EQ(asx_server_poll_accept(&srv, &c1), ASX_OK);
    ASSERT_EQ(asx_server_poll_accept(&srv, &c2), ASX_OK);
    ASSERT_EQ(asx_server_active_count(&srv), 2u);
    ASSERT_EQ(asx_server_total_accepted(&srv), 2u);
    ASSERT_NE(c1.id, c2.id);

    ASSERT_EQ(asx_server_close_conn(&srv, c1.id), ASX_OK);
    ASSERT_EQ(asx_server_active_count(&srv), 1u);

    asx_tcp_stream_close(client1);
    asx_tcp_stream_close(client2);
    asx_server_stop(&srv);
}

typedef struct {
    asx_server *srv;
    uint32_t polls;
    uint32_t accepted;
} accept_loop;

static asx_status accept_loop_poll(void *ud, asx_task_id self) {
    accept_loop *a = (accept_loop *)ud;
    asx_server_conn conn;
    asx_status st;
    (void)self;
    a->polls++;
    st = asx_server_poll_accept(a->srv, &conn);
    if (st != ASX_OK) return st;
    a->accepted++;
    return ASX_OK;
}

/* The in-memory backend's accept parks the polling task until a client
 * connects (bd-9kll.10.2): an accept loop with no client stops after its
 * first poll instead of spinning through the run budget. */
TEST(memory_accept_parks_until_a_client_connects) {
    asx_server srv;
    asx_server_config cfg;
    accept_loop a;
    asx_region_id r;
    asx_task_id t;
    asx_budget b;
    asx_tcp_stream client;
    asx_socket_addr addr;

    asx_runtime_reset();
    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14100);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available() || asx_net_get_backend() != ASX_NET_BACKEND_MEMORY) return;
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);
    memset(&a, 0, sizeof(a));
    a.srv = &srv;
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, accept_loop_poll, &a, &t), ASX_OK);

    b = asx_budget_from_polls(1000000);
    ASSERT_EQ(asx_scheduler_run(r, &b), ASX_E_WOULD_BLOCK);
    ASSERT_TRUE(a.polls < 10u);
    ASSERT_EQ(a.accepted, 0u);

    /* A client connects: the parked task wakes and accepts. */
    addr = asx_socket_addr_loopback(14100);
    ASSERT_EQ(asx_tcp_connect(&client, &addr), ASX_OK);
    b = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run(r, &b), ASX_OK);
    ASSERT_EQ(a.accepted, 1u);
    ASSERT_TRUE(a.polls < 10u);
}

TEST(server_stop_idempotent) {
    asx_server srv;
    asx_server_config cfg;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14011);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_listen(&srv), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_server_stop(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);
    ASSERT_EQ(asx_server_stop(&srv), ASX_OK);
    ASSERT_EQ(asx_server_get_state(&srv), ASX_SERVER_STATE_STOPPED);
    /* Second stop should still work */
    ASSERT_EQ(asx_server_stop(&srv), ASX_OK);
}

TEST(server_shutdown_idle_goes_stopped) {
    asx_server srv;
    asx_server_config cfg;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_addr = asx_socket_addr_loopback(14012);
    asx_server_init(&srv, &cfg);
    if (!server_surface_available()) {
        ASSERT_EQ(asx_server_shutdown(&srv), ASX_E_PERMISSION_DENIED);
        return;
    }
    /* Can't shutdown before listen */
    ASSERT_EQ(asx_server_shutdown(&srv), ASX_E_INVALID_STATE);
}
#else
TEST(server_surface_compile_time_hidden_in_browser) {
    ASSERT_EQ(ASX_HAS_SERVER_SURFACE, 0);
    ASSERT_EQ(asx_surface_available_active(ASX_SURFACE_SERVER), 0);
    ASSERT_EQ((int)asx_surface_gate(ASX_SURFACE_SERVER), (int)ASX_E_PERMISSION_DENIED);
}
#endif

int main(void) {
    fprintf(stderr, "=== server tests ===\n");
#if ASX_HAS_SERVER_SURFACE
    RUN_TEST(server_config_defaults);
    RUN_TEST(server_init_and_state);
    RUN_TEST(server_listen_and_accept);
    RUN_TEST(server_close_conn);
    RUN_TEST(server_accept_empty_pending);
    RUN_TEST(server_graceful_shutdown);
    RUN_TEST(server_shutdown_with_active_conns_drains);
    RUN_TEST(server_listen_twice_fails);
    RUN_TEST(server_null_args);
    RUN_TEST(server_multiple_clients);
    RUN_TEST(memory_accept_parks_until_a_client_connects);
    RUN_TEST(server_stop_idempotent);
    RUN_TEST(server_shutdown_idle_goes_stopped);
#else
    RUN_TEST(server_surface_compile_time_hidden_in_browser);
#endif
    TEST_REPORT();
    return test_failures;
}
