/*
 * test_quic.c — unit tests for the in-memory QUIC model (MODEL-ONLY, DEC-010)
 *
 * The model has no network: a connect begins a handshake that only an
 * in-process accept completes. These tests pin that, and the lifecycle,
 * stream and datagram behavior of a paired connection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/quic.h>
#include <string.h>

/* A connected pair: the client end and the server end, through the model's
 * only way to complete a handshake. */
static asx_status connect_pair(asx_quic_conn *client, asx_quic_conn *server,
                               const asx_quic_config *client_cfg,
                               const asx_quic_config *server_cfg) {
    asx_status st = asx_quic_connect(client, client_cfg);
    if (st != ASX_OK) return st;
    return asx_quic_accept(server, server_cfg);
}

TEST(quic_config_defaults) {
    asx_quic_config cfg;
    asx_quic_config_init(&cfg);
    ASSERT_EQ(cfg.max_bidi_streams, 4u);
    ASSERT_EQ(cfg.max_uni_streams, 4u);
    ASSERT_EQ(cfg.idle_timeout_ms, 30000u);
    ASSERT_EQ(cfg.enable_datagrams, 0u);
}

TEST(quic_connect_alone_does_not_connect) {
    asx_quic_conn conn;
    asx_quic_config cfg;
    asx_quic_stream stream;
    asx_buf src;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 1u;
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);

    /* The handshake was begun, not completed: no network, no peer. */
    ASSERT_TRUE(asx_quic_conn_is_alive(conn));
    ASSERT_EQ(asx_quic_conn_state(conn), ASX_QUIC_STATE_HANDSHAKE);
    ASSERT_EQ(asx_quic_stream_open(&stream, conn, ASX_QUIC_STREAM_BIDI), ASX_E_INVALID_STATE);
    src = asx_buf_from("x", 1u);
    ASSERT_EQ(asx_quic_send_datagram(conn, &src), ASX_E_INVALID_STATE);

    /* Closing it is allowed and leaves nothing behind. */
    ASSERT_EQ(asx_quic_conn_close(conn, 0), ASX_OK);
    ASSERT_FALSE(asx_quic_conn_is_alive(conn));
    ASSERT_EQ(asx_quic_conn_state(conn), ASX_QUIC_STATE_CLOSED);
}

TEST(quic_accept_without_a_connect_is_pending) {
    asx_quic_conn conn;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    conn.slot = 77u;
    conn.generation = 99u;
    ASSERT_EQ(asx_quic_accept(&conn, &cfg), ASX_E_PENDING);
    ASSERT_EQ(conn.slot, 77u); /* untouched */
    ASSERT_EQ(conn.generation, 99u);
}

TEST(quic_accept_completes_both_ends) {
    asx_quic_conn client, server;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(&client, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_state(client), ASX_QUIC_STATE_HANDSHAKE);

    ASSERT_EQ(asx_quic_accept(&server, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_state(client), ASX_QUIC_STATE_CONNECTED);
    ASSERT_EQ(asx_quic_conn_state(server), ASX_QUIC_STATE_CONNECTED);
    ASSERT_TRUE(client.slot != server.slot);

    /* The connect was consumed: a second accept has nothing to take. */
    ASSERT_EQ(asx_quic_accept(&server, &cfg), ASX_E_PENDING);

    ASSERT_EQ(asx_quic_conn_close(client, 0), ASX_OK);
    ASSERT_EQ(asx_quic_conn_close(server, 0), ASX_OK);
}

TEST(quic_accept_takes_connects_in_order) {
    asx_quic_conn first, second, s1, s2;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(&first, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_connect(&second, &cfg), ASX_OK);

    ASSERT_EQ(asx_quic_accept(&s1, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_state(first), ASX_QUIC_STATE_CONNECTED);
    ASSERT_EQ(asx_quic_conn_state(second), ASX_QUIC_STATE_HANDSHAKE);

    ASSERT_EQ(asx_quic_accept(&s2, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_state(second), ASX_QUIC_STATE_CONNECTED);

    asx_quic_conn_close(first, 0);
    asx_quic_conn_close(second, 0);
    asx_quic_conn_close(s1, 0);
    asx_quic_conn_close(s2, 0);
}

TEST(quic_accept_without_a_free_slot_leaves_the_connect_waiting) {
    asx_quic_conn conns[ASX_MAX_QUIC_CONNECTIONS];
    asx_quic_conn server;
    asx_quic_config cfg;
    uint32_t i;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    for (i = 0; i < ASX_MAX_QUIC_CONNECTIONS; i++) {
        ASSERT_EQ(asx_quic_connect(&conns[i], &cfg), ASX_OK);
    }
    /* Every slot is a waiting client: the server end has nowhere to live. */
    ASSERT_EQ(asx_quic_accept(&server, &cfg), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_quic_conn_state(conns[0]), ASX_QUIC_STATE_HANDSHAKE);

    /* Freeing one slot lets the oldest connect be accepted. */
    ASSERT_EQ(asx_quic_conn_close(conns[ASX_MAX_QUIC_CONNECTIONS - 1u], 0), ASX_OK);
    ASSERT_EQ(asx_quic_accept(&server, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_state(conns[0]), ASX_QUIC_STATE_CONNECTED);

    for (i = 0; i < ASX_MAX_QUIC_CONNECTIONS - 1u; i++) asx_quic_conn_close(conns[i], 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_closed_connect_is_not_accepted) {
    asx_quic_conn conn, server;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_close(conn, 0), ASX_OK);
    ASSERT_EQ(asx_quic_accept(&server, &cfg), ASX_E_PENDING);
}

TEST(quic_stream_open_and_write_read) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream stream;
    uint8_t data[] = "quic-data";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    ASSERT_EQ(asx_quic_stream_open(&stream, client, ASX_QUIC_STREAM_BIDI), ASX_OK);
    ASSERT_TRUE(asx_quic_stream_is_alive(stream));

    /* The model's stream is a loopback buffer on the handle that wrote. */
    src = asx_buf_from(data, 9);
    ASSERT_EQ(asx_quic_stream_poll_write(stream, &src, &written), ASX_OK);
    ASSERT_EQ(written, 9u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_stream_poll_read(stream, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 9u);
    ASSERT_TRUE(memcmp(dst.data, "quic-data", 9) == 0);

    asx_quic_stream_close(stream);
    ASSERT_FALSE(asx_quic_stream_is_alive(stream));
    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_stream_limit) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream streams[5];
    uint32_t i;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.max_bidi_streams = 3u;
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(asx_quic_stream_open(&streams[i], client, ASX_QUIC_STREAM_BIDI), ASX_OK);
    }
    ASSERT_EQ(asx_quic_stream_open(&streams[3], client, ASX_QUIC_STREAM_BIDI),
              ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < 3u; i++) { asx_quic_stream_close(streams[i]); }
    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_stream_reopen_after_close) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream s1, s2;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.max_bidi_streams = 1u;
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    /* Open one stream, filling the limit */
    ASSERT_EQ(asx_quic_stream_open(&s1, client, ASX_QUIC_STREAM_BIDI), ASX_OK);
    /* Second should fail */
    ASSERT_EQ(asx_quic_stream_open(&s2, client, ASX_QUIC_STREAM_BIDI), ASX_E_RESOURCE_EXHAUSTED);
    /* Close the first, then opening again should succeed */
    asx_quic_stream_close(s1);
    ASSERT_EQ(asx_quic_stream_open(&s2, client, ASX_QUIC_STREAM_BIDI), ASX_OK);

    asx_quic_stream_close(s2);
    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_stream_write_rejects_null_nonempty_payload) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream stream;
    asx_buf src;
    uint32_t written = 99u;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_stream_open(&stream, client, ASX_QUIC_STREAM_BIDI), ASX_OK);

    src.ptr = NULL;
    src.len = 1u;
    ASSERT_EQ(asx_quic_stream_poll_write(stream, &src, &written), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(written, 99u);

    asx_quic_stream_close(stream);
    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_stream_read_rejects_full_destination) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream stream;
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written;
    uint32_t read_n = 77u;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_stream_open(&stream, client, ASX_QUIC_STREAM_BIDI), ASX_OK);

    src = asx_buf_from("x", 1u);
    ASSERT_EQ(asx_quic_stream_poll_write(stream, &src, &written), ASX_OK);
    ASSERT_EQ(written, 1u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_buf_mut_put(&dst, dst.data, dst.capacity), ASX_OK);
    ASSERT_EQ(asx_quic_stream_poll_read(stream, &dst, &read_n), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(read_n, 77u);

    asx_buf_mut_clear(&dst);
    ASSERT_EQ(asx_quic_stream_poll_read(stream, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 1u);
    ASSERT_EQ(dst.data[0], (uint8_t)'x');

    asx_quic_stream_close(stream);
    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_conn_close_cleans_streams) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream s1, s2;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_stream_open(&s1, client, ASX_QUIC_STREAM_BIDI), ASX_OK);
    ASSERT_EQ(asx_quic_stream_open(&s2, client, ASX_QUIC_STREAM_UNI), ASX_OK);
    ASSERT_TRUE(asx_quic_stream_is_alive(s1));
    ASSERT_TRUE(asx_quic_stream_is_alive(s2));

    /* Closing the connection should clean up all streams */
    asx_quic_conn_close(client, 0);
    ASSERT_FALSE(asx_quic_stream_is_alive(s1));
    ASSERT_FALSE(asx_quic_stream_is_alive(s2));
    asx_quic_conn_close(server, 0);
}

TEST(quic_datagram_goes_to_the_peer_not_the_sender) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_buf src;
    asx_buf_mut dst;
    uint32_t read_n = 0u;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 1u;
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    src = asx_buf_from("dgram", 5);
    ASSERT_EQ(asx_quic_send_datagram(client, &src), ASX_OK);

    /* Not looped back to the sender... */
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_poll_recv_datagram(client, &dst, &read_n), ASX_E_PENDING);
    ASSERT_EQ(read_n, 0u);

    /* ...it is the peer's. */
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_poll_recv_datagram(server, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 5u);
    ASSERT_TRUE(memcmp(dst.data, "dgram", 5) == 0);

    /* And back the other way. */
    src = asx_buf_from("pong", 4);
    ASSERT_EQ(asx_quic_send_datagram(server, &src), ASX_OK);
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_poll_recv_datagram(client, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 4u);
    ASSERT_TRUE(memcmp(dst.data, "pong", 4) == 0);

    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_datagram_needs_both_ends_enabled) {
    asx_quic_conn client, server;
    asx_quic_config on, off;
    asx_buf src;

    asx_quic_reset();
    asx_quic_config_init(&on);
    asx_quic_config_init(&off);
    on.enable_datagrams = 1u;
    ASSERT_EQ(connect_pair(&client, &server, &on, &off), ASX_OK);

    src = asx_buf_from("x", 1);
    /* The peer did not advertise datagrams, so none can be sent to it. */
    ASSERT_EQ(asx_quic_send_datagram(client, &src), ASX_E_INVALID_STATE);
    /* A sender that did not enable them cannot send either. */
    ASSERT_EQ(asx_quic_send_datagram(server, &src), ASX_E_INVALID_STATE);

    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_datagram_disabled) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    uint8_t payload[] = "x";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t read_n;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 0u;
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    src = asx_buf_from(payload, 1);
    ASSERT_EQ(asx_quic_send_datagram(client, &src), ASX_E_INVALID_STATE);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_poll_recv_datagram(client, &dst, &read_n), ASX_E_INVALID_STATE);

    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_datagram_rejects_null_nonempty_payload) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_buf src;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 1u;
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    src.ptr = NULL;
    src.len = 1u;
    ASSERT_EQ(asx_quic_send_datagram(client, &src), ASX_E_INVALID_ARGUMENT);

    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_peer_close_drains_the_other_end) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream stream;
    asx_buf src;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 1u;
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);

    ASSERT_EQ(asx_quic_conn_close(client, 7u), ASX_OK);

    /* The server end saw the close: it can send and open nothing more. */
    ASSERT_TRUE(asx_quic_conn_is_alive(server));
    ASSERT_EQ(asx_quic_conn_state(server), ASX_QUIC_STATE_DRAINING);
    src = asx_buf_from("x", 1);
    ASSERT_EQ(asx_quic_send_datagram(server, &src), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_quic_stream_open(&stream, server, ASX_QUIC_STREAM_BIDI), ASX_E_INVALID_STATE);

    ASSERT_EQ(asx_quic_conn_close(server, 0), ASX_OK);
    ASSERT_FALSE(asx_quic_conn_is_alive(server));
}

TEST(quic_stale_handle_stays_dead_after_slot_reuse) {
    asx_quic_conn first, second;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(&first, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_close(first, 0), ASX_OK);

    /* The slot is reused; the old handle must not see the new connection. */
    ASSERT_EQ(asx_quic_connect(&second, &cfg), ASX_OK);
    ASSERT_EQ(second.slot, first.slot);
    ASSERT_TRUE(second.generation != first.generation);
    ASSERT_FALSE(asx_quic_conn_is_alive(first));
    ASSERT_TRUE(asx_quic_conn_is_alive(second));

    asx_quic_conn_close(second, 0);
}

TEST(quic_stale_stream_handle_stays_dead_after_slot_reuse) {
    asx_quic_conn client, server;
    asx_quic_config cfg;
    asx_quic_stream first, second;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(connect_pair(&client, &server, &cfg, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_stream_open(&first, client, ASX_QUIC_STREAM_BIDI), ASX_OK);
    ASSERT_EQ(asx_quic_stream_close(first), ASX_OK);
    ASSERT_EQ(asx_quic_stream_open(&second, client, ASX_QUIC_STREAM_BIDI), ASX_OK);
    ASSERT_EQ(second.slot, first.slot);
    ASSERT_TRUE(second.generation != first.generation);
    ASSERT_FALSE(asx_quic_stream_is_alive(first));
    ASSERT_TRUE(asx_quic_stream_is_alive(second));

    asx_quic_stream_close(second);
    asx_quic_conn_close(client, 0);
    asx_quic_conn_close(server, 0);
}

TEST(quic_connection_exhaustion) {
    asx_quic_conn conns[ASX_MAX_QUIC_CONNECTIONS + 1];
    asx_quic_config cfg;
    uint32_t i;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    for (i = 0; i < ASX_MAX_QUIC_CONNECTIONS; i++) {
        ASSERT_EQ(asx_quic_connect(&conns[i], &cfg), ASX_OK);
    }
    ASSERT_EQ(asx_quic_connect(&conns[ASX_MAX_QUIC_CONNECTIONS], &cfg), ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < ASX_MAX_QUIC_CONNECTIONS; i++) { asx_quic_conn_close(conns[i], 0); }
}

TEST(quic_null_args) {
    asx_quic_config cfg;
    asx_quic_conn conn;
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_quic_connect(NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_quic_connect(&conn, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_quic_accept(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_quic_accept(&conn, NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== quic tests ===\n");
    RUN_TEST(quic_config_defaults);
    RUN_TEST(quic_connect_alone_does_not_connect);
    RUN_TEST(quic_accept_without_a_connect_is_pending);
    RUN_TEST(quic_accept_completes_both_ends);
    RUN_TEST(quic_accept_takes_connects_in_order);
    RUN_TEST(quic_accept_without_a_free_slot_leaves_the_connect_waiting);
    RUN_TEST(quic_closed_connect_is_not_accepted);
    RUN_TEST(quic_stream_open_and_write_read);
    RUN_TEST(quic_stream_limit);
    RUN_TEST(quic_stream_reopen_after_close);
    RUN_TEST(quic_stream_write_rejects_null_nonempty_payload);
    RUN_TEST(quic_stream_read_rejects_full_destination);
    RUN_TEST(quic_conn_close_cleans_streams);
    RUN_TEST(quic_datagram_goes_to_the_peer_not_the_sender);
    RUN_TEST(quic_datagram_needs_both_ends_enabled);
    RUN_TEST(quic_datagram_disabled);
    RUN_TEST(quic_datagram_rejects_null_nonempty_payload);
    RUN_TEST(quic_peer_close_drains_the_other_end);
    RUN_TEST(quic_stale_handle_stays_dead_after_slot_reuse);
    RUN_TEST(quic_stale_stream_handle_stays_dead_after_slot_reuse);
    RUN_TEST(quic_connection_exhaustion);
    RUN_TEST(quic_null_args);
    TEST_REPORT();
    return test_failures;
}
