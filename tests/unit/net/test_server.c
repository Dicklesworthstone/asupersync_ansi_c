/*
 * test_server.c — unit tests for server connection and shutdown
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/server.h>
#include <string.h>

TEST(server_config_defaults) {
    asx_server_config cfg;
    asx_server_config_init(&cfg);
    ASSERT_EQ(cfg.max_connections, ASX_SERVER_MAX_CONNECTIONS);
    ASSERT_EQ(cfg.drain_timeout_ms, 5000u);
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
    cfg.listen_port = 14000;
    asx_server_init(&srv, &cfg);

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
    cfg.listen_port = 14001;
    asx_server_init(&srv, &cfg);
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
    cfg.listen_port = 14002;
    asx_server_init(&srv, &cfg);
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);

    ASSERT_EQ(asx_server_poll_accept(&srv, &conn), ASX_E_PENDING);
    asx_server_stop(&srv);
}

TEST(server_graceful_shutdown) {
    asx_server srv;
    asx_server_config cfg;

    asx_net_reset();
    asx_server_config_init(&cfg);
    cfg.listen_port = 14003;
    asx_server_init(&srv, &cfg);
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
    cfg.listen_port = 14004;
    asx_server_init(&srv, &cfg);
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
    cfg.listen_port = 14005;
    asx_server_init(&srv, &cfg);
    ASSERT_EQ(asx_server_listen(&srv), ASX_OK);
    ASSERT_EQ(asx_server_listen(&srv), ASX_E_INVALID_STATE);
    asx_server_stop(&srv);
}

TEST(server_null_args) {
    ASSERT_EQ(asx_server_listen(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_server_shutdown(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_server_stop(NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== server tests ===\n");
    RUN_TEST(server_config_defaults);
    RUN_TEST(server_init_and_state);
    RUN_TEST(server_listen_and_accept);
    RUN_TEST(server_close_conn);
    RUN_TEST(server_accept_empty_pending);
    RUN_TEST(server_graceful_shutdown);
    RUN_TEST(server_shutdown_with_active_conns_drains);
    RUN_TEST(server_listen_twice_fails);
    RUN_TEST(server_null_args);
    TEST_REPORT();
    return test_failures;
}
