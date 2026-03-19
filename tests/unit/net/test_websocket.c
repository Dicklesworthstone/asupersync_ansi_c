/*
 * test_websocket.c — unit tests for deterministic WebSocket surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/websocket.h>
#include <string.h>

TEST(ws_frame_init_defaults) {
    asx_ws_frame frame;
    asx_ws_frame_init(&frame);
    ASSERT_EQ(frame.opcode, ASX_WS_OPCODE_TEXT);
    ASSERT_EQ(frame.fin, 1u);
    ASSERT_EQ(frame.payload_len, 0u);
}

TEST(ws_frame_set_payload) {
    asx_ws_frame frame;
    uint8_t data[] = "hello-ws";
    ASSERT_EQ(asx_ws_frame_set_payload(&frame, ASX_WS_OPCODE_BINARY, data, 8), ASX_OK);
    ASSERT_EQ(frame.opcode, ASX_WS_OPCODE_BINARY);
    ASSERT_EQ(frame.payload_len, 8u);
    ASSERT_TRUE(memcmp(frame.payload, "hello-ws", 8) == 0);
}

TEST(ws_frame_set_payload_rejects_null_nonempty) {
    asx_ws_frame frame;
    asx_ws_frame_init(&frame);
    ASSERT_EQ(asx_ws_frame_set_payload(&frame, ASX_WS_OPCODE_BINARY, NULL, 1u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(frame.payload_len, 0u);
}

TEST(ws_frame_close_encodes_code) {
    asx_ws_frame frame;
    ASSERT_EQ(asx_ws_frame_close(&frame, 1000), ASX_OK);
    ASSERT_EQ(frame.opcode, ASX_WS_OPCODE_CLOSE);
    ASSERT_EQ(frame.close_code, 1000);
    ASSERT_EQ(frame.payload_len, 2u);
    ASSERT_EQ(frame.payload[0], 3); /* 1000 >> 8 */
    ASSERT_EQ(frame.payload[1], 232); /* 1000 & 0xFF */
}

TEST(ws_connect_and_state) {
    asx_ws_conn conn;
    asx_tcp_listener lis;
    asx_tcp_stream tcp;
    asx_socket_addr addr = asx_socket_addr_loopback(13000);

    asx_net_reset();
    asx_ws_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp, &addr), ASX_OK);

    ASSERT_EQ(asx_ws_connect(&conn, tcp), ASX_OK);
    ASSERT_TRUE(asx_ws_conn_is_alive(conn));
    ASSERT_EQ(asx_ws_conn_state(conn), ASX_WS_STATE_OPEN);
    ASSERT_EQ(asx_ws_conn_role(conn), ASX_WS_ROLE_CLIENT);

    asx_ws_conn_release(conn);
    ASSERT_FALSE(asx_ws_conn_is_alive(conn));
    asx_tcp_stream_close(tcp);
    asx_tcp_listener_close(lis);
}

TEST(ws_accept_server_role) {
    asx_ws_conn conn;
    asx_tcp_listener lis;
    asx_tcp_stream tcp_client, tcp_server;
    asx_socket_addr addr = asx_socket_addr_loopback(13001);

    asx_net_reset();
    asx_ws_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp_client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &tcp_server, NULL), ASX_OK);

    ASSERT_EQ(asx_ws_accept(&conn, tcp_server), ASX_OK);
    ASSERT_EQ(asx_ws_conn_role(conn), ASX_WS_ROLE_SERVER);

    asx_ws_conn_release(conn);
    asx_tcp_stream_close(tcp_client);
    asx_tcp_stream_close(tcp_server);
    asx_tcp_listener_close(lis);
}

TEST(ws_send_recv_loopback) {
    asx_ws_conn conn;
    asx_tcp_listener lis;
    asx_tcp_stream tcp;
    asx_socket_addr addr = asx_socket_addr_loopback(13002);
    asx_ws_frame send_frame, recv_frame;

    asx_net_reset();
    asx_ws_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp, &addr), ASX_OK);
    ASSERT_EQ(asx_ws_connect(&conn, tcp), ASX_OK);

    asx_ws_frame_set_payload(&send_frame, ASX_WS_OPCODE_TEXT, "msg", 3);
    ASSERT_EQ(asx_ws_send(conn, &send_frame), ASX_OK);

    ASSERT_EQ(asx_ws_poll_recv(conn, &recv_frame), ASX_OK);
    ASSERT_EQ(recv_frame.opcode, ASX_WS_OPCODE_TEXT);
    ASSERT_EQ(recv_frame.payload_len, 3u);
    ASSERT_TRUE(memcmp(recv_frame.payload, "msg", 3) == 0);

    asx_ws_conn_release(conn);
    asx_tcp_stream_close(tcp);
    asx_tcp_listener_close(lis);
}

TEST(ws_recv_empty_pending) {
    asx_ws_conn conn;
    asx_tcp_listener lis;
    asx_tcp_stream tcp;
    asx_socket_addr addr = asx_socket_addr_loopback(13003);
    asx_ws_frame frame;

    asx_net_reset();
    asx_ws_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp, &addr), ASX_OK);
    ASSERT_EQ(asx_ws_connect(&conn, tcp), ASX_OK);

    ASSERT_EQ(asx_ws_poll_recv(conn, &frame), ASX_E_PENDING);

    asx_ws_conn_release(conn);
    asx_tcp_stream_close(tcp);
    asx_tcp_listener_close(lis);
}

TEST(ws_close_protocol) {
    asx_ws_conn conn;
    asx_tcp_listener lis;
    asx_tcp_stream tcp;
    asx_socket_addr addr = asx_socket_addr_loopback(13004);
    asx_ws_frame recv_frame;

    asx_net_reset();
    asx_ws_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp, &addr), ASX_OK);
    ASSERT_EQ(asx_ws_connect(&conn, tcp), ASX_OK);

    ASSERT_EQ(asx_ws_close(conn, 1000), ASX_OK);
    ASSERT_EQ(asx_ws_conn_state(conn), ASX_WS_STATE_CLOSING);

    /* Receive the close frame */
    ASSERT_EQ(asx_ws_poll_recv(conn, &recv_frame), ASX_OK);
    ASSERT_EQ(recv_frame.opcode, ASX_WS_OPCODE_CLOSE);
    ASSERT_EQ(asx_ws_conn_state(conn), ASX_WS_STATE_CLOSED);

    asx_ws_conn_release(conn);
    asx_tcp_stream_close(tcp);
    asx_tcp_listener_close(lis);
}

TEST(ws_ping_pong) {
    asx_ws_conn conn;
    asx_tcp_listener lis;
    asx_tcp_stream tcp;
    asx_socket_addr addr = asx_socket_addr_loopback(13005);
    asx_ws_frame ping, recv;

    asx_net_reset();
    asx_ws_reset();
    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp, &addr), ASX_OK);
    ASSERT_EQ(asx_ws_connect(&conn, tcp), ASX_OK);

    asx_ws_frame_set_payload(&ping, ASX_WS_OPCODE_PING, "test", 4);
    ASSERT_EQ(asx_ws_send(conn, &ping), ASX_OK);
    ASSERT_EQ(asx_ws_poll_recv(conn, &recv), ASX_OK);
    ASSERT_EQ(recv.opcode, ASX_WS_OPCODE_PING);

    asx_ws_conn_release(conn);
    asx_tcp_stream_close(tcp);
    asx_tcp_listener_close(lis);
}

int main(void) {
    fprintf(stderr, "=== websocket tests ===\n");
    RUN_TEST(ws_frame_init_defaults);
    RUN_TEST(ws_frame_set_payload);
    RUN_TEST(ws_frame_set_payload_rejects_null_nonempty);
    RUN_TEST(ws_frame_close_encodes_code);
    RUN_TEST(ws_connect_and_state);
    RUN_TEST(ws_accept_server_role);
    RUN_TEST(ws_send_recv_loopback);
    RUN_TEST(ws_recv_empty_pending);
    RUN_TEST(ws_close_protocol);
    RUN_TEST(ws_ping_pong);
    TEST_REPORT();
    return test_failures;
}
