/*
 * test_quic.c — unit tests for deterministic QUIC surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/quic.h>
#include <string.h>

TEST(quic_config_defaults) {
    asx_quic_config cfg;
    asx_quic_config_init(&cfg);
    ASSERT_EQ(cfg.max_bidi_streams, 4u);
    ASSERT_EQ(cfg.max_uni_streams, 4u);
    ASSERT_EQ(cfg.idle_timeout_ms, 30000u);
    ASSERT_EQ(cfg.enable_datagrams, 0u);
}

TEST(quic_connect_and_state) {
    asx_quic_conn conn;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);
    ASSERT_TRUE(asx_quic_conn_is_alive(conn));
    ASSERT_EQ(asx_quic_conn_state(conn), ASX_QUIC_STATE_CONNECTED);

    ASSERT_EQ(asx_quic_conn_close(conn, 0), ASX_OK);
    ASSERT_FALSE(asx_quic_conn_is_alive(conn));
}

TEST(quic_accept_and_state) {
    asx_quic_conn conn;
    asx_quic_config cfg;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_accept(&conn, &cfg), ASX_OK);
    ASSERT_EQ(asx_quic_conn_state(conn), ASX_QUIC_STATE_CONNECTED);
    asx_quic_conn_close(conn, 0);
}

TEST(quic_stream_open_and_write_read) {
    asx_quic_conn conn;
    asx_quic_config cfg;
    asx_quic_stream stream;
    uint8_t data[] = "quic-data";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);

    ASSERT_EQ(asx_quic_stream_open(&stream, conn, ASX_QUIC_STREAM_BIDI), ASX_OK);
    ASSERT_TRUE(asx_quic_stream_is_alive(stream));

    src = asx_buf_from(data, 9);
    ASSERT_EQ(asx_quic_stream_poll_write(stream, &src, &written), ASX_OK);
    ASSERT_EQ(written, 9u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_stream_poll_read(stream, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 9u);
    ASSERT_TRUE(memcmp(dst.data, "quic-data", 9) == 0);

    asx_quic_stream_close(stream);
    ASSERT_FALSE(asx_quic_stream_is_alive(stream));
    asx_quic_conn_close(conn, 0);
}

TEST(quic_stream_limit) {
    asx_quic_conn conn;
    asx_quic_config cfg;
    asx_quic_stream streams[5];
    uint32_t i;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.max_bidi_streams = 3u;
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);

    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(asx_quic_stream_open(&streams[i], conn, ASX_QUIC_STREAM_BIDI), ASX_OK);
    }
    ASSERT_EQ(asx_quic_stream_open(&streams[3], conn, ASX_QUIC_STREAM_BIDI),
              ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < 3u; i++) {
        asx_quic_stream_close(streams[i]);
    }
    asx_quic_conn_close(conn, 0);
}

TEST(quic_datagram_send_recv) {
    asx_quic_conn conn;
    asx_quic_config cfg;
    uint8_t payload[] = "dgram";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t read_n;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 1u;
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);

    src = asx_buf_from(payload, 5);
    ASSERT_EQ(asx_quic_send_datagram(conn, &src), ASX_OK);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_poll_recv_datagram(conn, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 5u);
    ASSERT_TRUE(memcmp(dst.data, "dgram", 5) == 0);

    asx_quic_conn_close(conn, 0);
}

TEST(quic_datagram_disabled) {
    asx_quic_conn conn;
    asx_quic_config cfg;
    uint8_t payload[] = "x";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t read_n;

    asx_quic_reset();
    asx_quic_config_init(&cfg);
    cfg.enable_datagrams = 0u;
    ASSERT_EQ(asx_quic_connect(&conn, &cfg), ASX_OK);

    src = asx_buf_from(payload, 1);
    ASSERT_EQ(asx_quic_send_datagram(conn, &src), ASX_E_INVALID_STATE);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_quic_poll_recv_datagram(conn, &dst, &read_n), ASX_E_INVALID_STATE);

    asx_quic_conn_close(conn, 0);
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

    for (i = 0; i < ASX_MAX_QUIC_CONNECTIONS; i++) {
        asx_quic_conn_close(conns[i], 0);
    }
}

TEST(quic_null_args) {
    asx_quic_config cfg;
    asx_quic_config_init(&cfg);
    ASSERT_EQ(asx_quic_connect(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_quic_connect(NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== quic tests ===\n");
    RUN_TEST(quic_config_defaults);
    RUN_TEST(quic_connect_and_state);
    RUN_TEST(quic_accept_and_state);
    RUN_TEST(quic_stream_open_and_write_read);
    RUN_TEST(quic_stream_limit);
    RUN_TEST(quic_datagram_send_recv);
    RUN_TEST(quic_datagram_disabled);
    RUN_TEST(quic_connection_exhaustion);
    RUN_TEST(quic_null_args);
    TEST_REPORT();
    return test_failures;
}
