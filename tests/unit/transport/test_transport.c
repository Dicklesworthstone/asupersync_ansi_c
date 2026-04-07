/*
 * test_transport.c — unit tests for transport abstraction layer
 *
 * Tests transport trait dispatch, memory transport (connect, listen,
 * accept, read, write, close), and framed transport.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/transport/transport.h>
#include <string.h>

typedef struct {
    uint32_t connect_count;
    uint32_t close_count;
    uint32_t fail_writes_before_success;
    uint32_t fail_reads_before_success;
    uint32_t generation;
    char payload[32];
    size_t payload_len;
    int connected;
} reconnect_mock_state;

static asx_status reconnect_mock_connect(void *state, const void *addr, size_t addr_len,
                                         asx_transport_conn *out) {
    reconnect_mock_state *mock = (reconnect_mock_state *)state;
    (void)addr;
    (void)addr_len;
    mock->connect_count++;
    mock->generation++;
    mock->connected = 1;
    out->id = 77u;
    out->generation = mock->generation;
    return ASX_OK;
}

static asx_status reconnect_mock_listen(void *state, const void *addr, size_t addr_len,
                                        asx_transport_listener *out) {
    (void)state;
    (void)addr;
    (void)addr_len;
    (void)out;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status reconnect_mock_accept(void *state, asx_transport_listener listener,
                                        asx_transport_conn *out) {
    (void)state;
    (void)listener;
    (void)out;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status reconnect_mock_read(void *state, asx_transport_conn conn, void *buf,
                                      size_t buf_len, size_t *bytes_read) {
    reconnect_mock_state *mock = (reconnect_mock_state *)state;
    (void)conn;

    if (!mock->connected) {
        *bytes_read = 0u;
        return ASX_E_DISCONNECTED;
    }
    if (mock->fail_reads_before_success > 0u) {
        mock->fail_reads_before_success--;
        mock->connected = 0;
        *bytes_read = 0u;
        return ASX_E_DISCONNECTED;
    }
    if (mock->payload_len > buf_len) {
        *bytes_read = 0u;
        return ASX_E_INVALID_ARGUMENT;
    }

    memcpy(buf, mock->payload, mock->payload_len);
    *bytes_read = mock->payload_len;
    return ASX_OK;
}

static asx_status reconnect_mock_write(void *state, asx_transport_conn conn, const void *data,
                                       size_t data_len, size_t *bytes_written) {
    reconnect_mock_state *mock = (reconnect_mock_state *)state;
    (void)conn;

    if (!mock->connected) {
        *bytes_written = 0u;
        return ASX_E_DISCONNECTED;
    }
    if (mock->fail_writes_before_success > 0u) {
        mock->fail_writes_before_success--;
        mock->connected = 0;
        *bytes_written = 0u;
        return ASX_E_DISCONNECTED;
    }
    if (data_len > sizeof(mock->payload)) {
        *bytes_written = 0u;
        return ASX_E_INVALID_ARGUMENT;
    }

    memcpy(mock->payload, data, data_len);
    mock->payload_len = data_len;
    *bytes_written = data_len;
    return ASX_OK;
}

static asx_status reconnect_mock_close_conn(void *state, asx_transport_conn conn) {
    reconnect_mock_state *mock = (reconnect_mock_state *)state;
    (void)conn;
    mock->close_count++;
    mock->connected = 0;
    return ASX_OK;
}

static asx_status reconnect_mock_close_listener(void *state, asx_transport_listener listener) {
    (void)state;
    (void)listener;
    return ASX_OK;
}

static asx_transport make_reconnect_mock_transport(reconnect_mock_state *state) {
    asx_transport t;
    memset(state, 0, sizeof(*state));
    t.connect = reconnect_mock_connect;
    t.listen = reconnect_mock_listen;
    t.accept = reconnect_mock_accept;
    t.read = reconnect_mock_read;
    t.write = reconnect_mock_write;
    t.close_conn = reconnect_mock_close_conn;
    t.close_listener = reconnect_mock_close_listener;
    t.state = state;
    return t;
}

typedef struct {
    uint8_t written[64];
    size_t written_len;
    uint8_t read_data[64];
    size_t read_len;
    size_t read_pos;
    size_t max_write_chunk;
    size_t max_read_chunk;
} framed_partial_mock_state;

static asx_status framed_partial_mock_connect(void *state, const void *addr, size_t addr_len,
                                              asx_transport_conn *out) {
    (void)state;
    (void)addr;
    (void)addr_len;
    (void)out;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status framed_partial_mock_listen(void *state, const void *addr, size_t addr_len,
                                             asx_transport_listener *out) {
    (void)state;
    (void)addr;
    (void)addr_len;
    (void)out;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status framed_partial_mock_accept(void *state, asx_transport_listener listener,
                                             asx_transport_conn *out) {
    (void)state;
    (void)listener;
    (void)out;
    return ASX_E_INVALID_ARGUMENT;
}

static asx_status framed_partial_mock_read(void *state, asx_transport_conn conn, void *buf,
                                           size_t buf_len, size_t *bytes_read) {
    framed_partial_mock_state *mock = (framed_partial_mock_state *)state;
    size_t remaining;
    size_t chunk;

    (void)conn;

    remaining = mock->read_len - mock->read_pos;
    if (remaining == 0u) {
        *bytes_read = 0u;
        return ASX_E_PENDING;
    }

    chunk = remaining;
    if (chunk > mock->max_read_chunk) { chunk = mock->max_read_chunk; }
    if (chunk > buf_len) { chunk = buf_len; }
    if (chunk == 0u) {
        *bytes_read = 0u;
        return ASX_E_PENDING;
    }

    memcpy(buf, mock->read_data + mock->read_pos, chunk);
    mock->read_pos += chunk;
    *bytes_read = chunk;
    return ASX_OK;
}

static asx_status framed_partial_mock_write(void *state, asx_transport_conn conn, const void *data,
                                            size_t data_len, size_t *bytes_written) {
    framed_partial_mock_state *mock = (framed_partial_mock_state *)state;
    size_t remaining_capacity;
    size_t chunk;

    (void)conn;

    remaining_capacity = sizeof(mock->written) - mock->written_len;
    chunk = data_len;
    if (chunk > mock->max_write_chunk) { chunk = mock->max_write_chunk; }
    if (chunk > remaining_capacity) { chunk = remaining_capacity; }
    if (chunk == 0u) {
        *bytes_written = 0u;
        return ASX_E_PENDING;
    }

    memcpy(mock->written + mock->written_len, data, chunk);
    mock->written_len += chunk;
    *bytes_written = chunk;
    return ASX_OK;
}

static asx_status framed_partial_mock_close_conn(void *state, asx_transport_conn conn) {
    (void)state;
    (void)conn;
    return ASX_OK;
}

static asx_status framed_partial_mock_close_listener(void *state, asx_transport_listener listener) {
    (void)state;
    (void)listener;
    return ASX_OK;
}

static asx_transport make_framed_partial_mock_transport(framed_partial_mock_state *state) {
    asx_transport t;
    memset(&t, 0, sizeof(t));
    t.connect = framed_partial_mock_connect;
    t.listen = framed_partial_mock_listen;
    t.accept = framed_partial_mock_accept;
    t.read = framed_partial_mock_read;
    t.write = framed_partial_mock_write;
    t.close_conn = framed_partial_mock_close_conn;
    t.close_listener = framed_partial_mock_close_listener;
    t.state = state;
    return t;
}

/* ================================================================== */
/* Transport connection handle tests                                   */
/* ================================================================== */

TEST(conn_invalid_sentinel) {
    asx_transport_conn c = ASX_TRANSPORT_CONN_INVALID;
    ASSERT_FALSE(asx_transport_conn_is_valid(c));
}

TEST(conn_valid_after_set) {
    asx_transport_conn c;
    c.id = 1;
    c.generation = 1;
    ASSERT_TRUE(asx_transport_conn_is_valid(c));
}

/* ================================================================== */
/* Memory transport initialization tests                               */
/* ================================================================== */

TEST(mem_transport_init_zero_state) {
    asx_transport t;
    asx_mem_transport_state state;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_mem_transport_active_conns(&state), 0u);
    ASSERT_EQ(asx_mem_transport_active_listeners(&state), 0u);
    ASSERT_EQ(asx_mem_transport_bytes_written(&state), 0u);
    ASSERT_EQ(asx_mem_transport_bytes_read(&state), 0u);
}

/* ================================================================== */
/* Memory transport listen tests                                       */
/* ================================================================== */

TEST(mem_listen_succeeds) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    uint32_t addr = 42;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_mem_transport_active_listeners(&state), 1u);
}

TEST(mem_listen_null_addr_fails) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, NULL, 0, &listener), ASX_E_INVALID_ARGUMENT);
}

TEST(mem_listen_exhaustion) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listeners[ASX_MEM_TRANSPORT_MAX_LISTENERS + 1];
    uint32_t i;

    asx_mem_transport_init(&t, &state);

    for (i = 0; i < ASX_MEM_TRANSPORT_MAX_LISTENERS; i++) {
        uint32_t addr = i + 1;
        ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listeners[i]), ASX_OK);
    }

    {
        uint32_t addr = 999;
        ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr),
                                       &listeners[ASX_MEM_TRANSPORT_MAX_LISTENERS]),
                  ASX_E_RESOURCE_EXHAUSTED);
    }
}

/* ================================================================== */
/* Memory transport connect + accept tests                             */
/* ================================================================== */

TEST(mem_connect_to_listener) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn;
    uint32_t addr = 100;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_TRUE(asx_transport_conn_is_valid(client_conn));
    ASSERT_EQ(asx_mem_transport_active_conns(&state), 1u);
}

TEST(mem_connect_no_listener_fails) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_conn conn;
    uint32_t addr = 200;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &conn), ASX_E_DISCONNECTED);
}

TEST(mem_accept_pending_connection) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 300;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);
    ASSERT_TRUE(asx_transport_conn_is_valid(server_conn));
}

TEST(mem_accept_no_pending_returns_pending) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn server_conn;
    uint32_t addr = 400;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_E_PENDING);
}

/* ================================================================== */
/* Memory transport read/write tests                                   */
/* ================================================================== */

TEST(mem_write_and_read) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 500;
    const char *msg = "hello";
    char buf[32];
    size_t written = 0, nread = 0;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);

    /* Client writes, server reads. */
    ASSERT_EQ(asx_transport_write(&t, client_conn, msg, 5, &written), ASX_OK);
    ASSERT_EQ(written, 5u);

    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(asx_transport_read(&t, server_conn, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(nread, 5u);
    ASSERT_TRUE(memcmp(buf, "hello", 5) == 0);
}

TEST(mem_client_cannot_read_its_own_write) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 550;
    const char *msg = "hello";
    char buf[32];
    size_t written = 0, nread = 0;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);

    ASSERT_EQ(asx_transport_write(&t, client_conn, msg, 5, &written), ASX_OK);
    ASSERT_EQ(written, 5u);

    ASSERT_EQ(asx_transport_read(&t, client_conn, buf, sizeof(buf), &nread), ASX_E_PENDING);
    ASSERT_EQ(nread, 0u);
    ASSERT_EQ(asx_transport_read(&t, server_conn, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(nread, 5u);
    ASSERT_TRUE(memcmp(buf, "hello", 5) == 0);
}

TEST(mem_read_empty_returns_pending) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 600;
    char buf[16];
    size_t nread = 0;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);

    ASSERT_EQ(asx_transport_read(&t, server_conn, buf, sizeof(buf), &nread), ASX_E_PENDING);
    ASSERT_EQ(nread, 0u);
}

TEST(mem_write_tracks_stats) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 700;
    const char *msg = "data";
    size_t written = 0;
    char buf[16];
    size_t nread = 0;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);

    ASSERT_EQ(asx_transport_write(&t, client_conn, msg, 4, &written), ASX_OK);
    ASSERT_EQ(asx_mem_transport_bytes_written(&state), 4u);

    ASSERT_EQ(asx_transport_read(&t, server_conn, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(asx_mem_transport_bytes_read(&state), 4u);
}

/* ================================================================== */
/* Memory transport close tests                                        */
/* ================================================================== */

TEST(mem_close_connection) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 800;
    char buf[8];
    size_t nread = 0u;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);
    ASSERT_EQ(asx_mem_transport_active_conns(&state), 1u);

    ASSERT_EQ(asx_transport_close(&t, client_conn), ASX_OK);
    ASSERT_EQ(asx_mem_transport_active_conns(&state), 1u);
    ASSERT_EQ(asx_transport_read(&t, server_conn, buf, sizeof(buf), &nread), ASX_E_DISCONNECTED);
    ASSERT_EQ(nread, 0u);

    ASSERT_EQ(asx_transport_close(&t, server_conn), ASX_OK);
    ASSERT_EQ(asx_mem_transport_active_conns(&state), 0u);
}

TEST(mem_close_listener) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    uint32_t addr = 900;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_mem_transport_active_listeners(&state), 1u);

    ASSERT_EQ(asx_transport_close_listener(&t, listener), ASX_OK);
    ASSERT_EQ(asx_mem_transport_active_listeners(&state), 0u);
}

TEST(mem_close_invalid_conn_fails) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_conn invalid = ASX_TRANSPORT_CONN_INVALID;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_close(&t, invalid), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Memory transport reset tests                                        */
/* ================================================================== */

TEST(mem_reset_clears_all) {
    asx_transport t;
    asx_mem_transport_state state;
    asx_transport_listener listener;
    asx_transport_conn conn;
    uint32_t addr = 1000;
    const char *msg = "test";
    size_t written = 0;

    asx_mem_transport_init(&t, &state);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &conn), ASX_OK);
    ASSERT_EQ(asx_transport_write(&t, conn, msg, 4, &written), ASX_OK);

    asx_mem_transport_reset(&state);

    ASSERT_EQ(asx_mem_transport_active_conns(&state), 0u);
    ASSERT_EQ(asx_mem_transport_active_listeners(&state), 0u);
}

/* ================================================================== */
/* Framed transport tests                                              */
/* ================================================================== */

TEST(framed_write_and_read_roundtrip) {
    asx_transport t;
    asx_mem_transport_state mstate;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 1100;
    asx_framed_transport_state writer_frame, reader_frame;
    const char *msg = "framed-data";
    char buf[64];
    size_t written = 0, nread = 0;

    asx_mem_transport_init(&t, &mstate);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);

    asx_framed_transport_init(&writer_frame, t, client_conn);
    asx_framed_transport_init(&reader_frame, t, server_conn);

    ASSERT_EQ(asx_framed_transport_write(&writer_frame, msg, 11, &written), ASX_OK);
    ASSERT_EQ(written, 11u);

    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(asx_framed_transport_read(&reader_frame, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(nread, 11u);
    ASSERT_TRUE(memcmp(buf, "framed-data", 11) == 0);
}

TEST(framed_empty_frame) {
    asx_transport t;
    asx_mem_transport_state mstate;
    asx_transport_listener listener;
    asx_transport_conn client_conn, server_conn;
    uint32_t addr = 1200;
    asx_framed_transport_state writer_frame, reader_frame;
    char buf[16];
    size_t written = 0, nread = 0;

    asx_mem_transport_init(&t, &mstate);

    ASSERT_EQ(asx_transport_listen(&t, &addr, sizeof(addr), &listener), ASX_OK);
    ASSERT_EQ(asx_transport_connect(&t, &addr, sizeof(addr), &client_conn), ASX_OK);
    ASSERT_EQ(asx_transport_accept(&t, listener, &server_conn), ASX_OK);

    asx_framed_transport_init(&writer_frame, t, client_conn);
    asx_framed_transport_init(&reader_frame, t, server_conn);

    ASSERT_EQ(asx_framed_transport_write(&writer_frame, NULL, 0, &written), ASX_OK);

    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(asx_framed_transport_read(&reader_frame, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(nread, 0u);
}

TEST(framed_write_retries_partial_body_without_duplicate_header) {
    framed_partial_mock_state mock;
    asx_transport t;
    asx_framed_transport_state frame;
    asx_transport_conn conn = {1u, 1u};
    const uint8_t expected[] = {0u, 0u, 0u, 5u, 'h', 'e', 'l', 'l', 'o'};
    size_t written = 0u;

    memset(&mock, 0, sizeof(mock));
    mock.max_write_chunk = 5u;
    t = make_framed_partial_mock_transport(&mock);
    asx_framed_transport_init(&frame, t, conn);

    ASSERT_EQ(asx_framed_transport_write(&frame, "hello", 5u, &written), ASX_E_PENDING);
    ASSERT_EQ(mock.written_len, 5u);
    ASSERT_EQ(written, 1u);

    ASSERT_EQ(asx_framed_transport_write(&frame, "hello", 5u, &written), ASX_OK);
    ASSERT_EQ(written, 5u);
    ASSERT_EQ(mock.written_len, sizeof(expected));
    ASSERT_TRUE(memcmp(mock.written, expected, sizeof(expected)) == 0);
}

TEST(framed_read_retries_partial_body_until_complete) {
    framed_partial_mock_state mock;
    asx_transport t;
    asx_framed_transport_state frame;
    asx_transport_conn conn = {1u, 1u};
    char buf[16];
    size_t nread = 0u;

    memset(&mock, 0, sizeof(mock));
    mock.max_read_chunk = 3u;
    mock.read_data[0] = 0u;
    mock.read_data[1] = 0u;
    mock.read_data[2] = 0u;
    mock.read_data[3] = 5u;
    memcpy(mock.read_data + 4, "hello", 5u);
    mock.read_len = 9u;
    t = make_framed_partial_mock_transport(&mock);
    asx_framed_transport_init(&frame, t, conn);
    memset(buf, 0, sizeof(buf));

    ASSERT_EQ(asx_framed_transport_read(&frame, buf, sizeof(buf), &nread), ASX_E_PENDING);
    ASSERT_EQ(nread, 0u);
    ASSERT_EQ(asx_framed_transport_read(&frame, buf, sizeof(buf), &nread), ASX_E_PENDING);
    ASSERT_EQ(nread, 0u);
    ASSERT_EQ(asx_framed_transport_read(&frame, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(nread, 5u);
    ASSERT_TRUE(memcmp(buf, "hello", 5u) == 0);
}

TEST(framed_read_rejects_too_small_buffer_without_consuming_body) {
    framed_partial_mock_state mock;
    asx_transport t;
    asx_framed_transport_state frame;
    asx_transport_conn conn = {1u, 1u};
    char small[4];
    char big[16];
    size_t nread = 0u;

    memset(&mock, 0, sizeof(mock));
    mock.max_read_chunk = 9u;
    mock.read_data[0] = 0u;
    mock.read_data[1] = 0u;
    mock.read_data[2] = 0u;
    mock.read_data[3] = 5u;
    memcpy(mock.read_data + 4, "hello", 5u);
    mock.read_len = 9u;
    t = make_framed_partial_mock_transport(&mock);
    asx_framed_transport_init(&frame, t, conn);
    memset(big, 0, sizeof(big));

    ASSERT_EQ(asx_framed_transport_read(&frame, small, sizeof(small), &nread),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(nread, 0u);
    ASSERT_EQ(mock.read_pos, 4u);

    ASSERT_EQ(asx_framed_transport_read(&frame, big, sizeof(big), &nread), ASX_OK);
    ASSERT_EQ(nread, 5u);
    ASSERT_TRUE(memcmp(big, "hello", 5u) == 0);
}

TEST(framed_reset_clears_state) {
    asx_framed_transport_state frame;
    asx_transport t;
    asx_transport_conn conn;

    memset(&t, 0, sizeof(t));
    conn.id = 1;
    conn.generation = 1;

    asx_framed_transport_init(&frame, t, conn);
    frame.header_pos = 2;
    frame.header_complete = 1;
    frame.pending_frame_len = 42;

    asx_framed_transport_reset(&frame);

    ASSERT_EQ(frame.header_pos, 0u);
    ASSERT_EQ(frame.header_complete, 0);
    ASSERT_EQ(frame.pending_frame_len, 0u);
}

/* ================================================================== */
/* Reconnecting transport tests                                        */
/* ================================================================== */

TEST(reconnecting_write_retries_after_disconnect) {
    reconnect_mock_state inner_state;
    asx_transport inner = make_reconnect_mock_transport(&inner_state);
    asx_reconnecting_transport_state reconnect_state;
    asx_transport reconnecting;
    asx_transport_conn conn;
    uint32_t addr = 1300u;
    size_t written = 0u;

    inner_state.fail_writes_before_success = 1u;
    asx_reconnecting_transport_init(&reconnecting, &reconnect_state, inner, 2u);

    ASSERT_EQ(asx_transport_connect(&reconnecting, &addr, sizeof(addr), &conn), ASX_OK);
    ASSERT_EQ(asx_transport_write(&reconnecting, conn, "abc", 3u, &written), ASX_OK);
    ASSERT_EQ(written, 3u);
    ASSERT_EQ(inner_state.connect_count, 2u);
    ASSERT_EQ(asx_reconnecting_transport_reconnect_count(&reconnect_state), 1u);
    ASSERT_EQ(asx_reconnecting_transport_last_status(&reconnect_state), ASX_OK);
}

TEST(reconnecting_read_retries_after_disconnect) {
    reconnect_mock_state inner_state;
    asx_transport inner = make_reconnect_mock_transport(&inner_state);
    asx_reconnecting_transport_state reconnect_state;
    asx_transport reconnecting;
    asx_transport_conn conn;
    uint32_t addr = 1400u;
    char buf[32];
    size_t nread = 0u;

    memcpy(inner_state.payload, "reply", 5u);
    inner_state.payload_len = 5u;
    inner_state.fail_reads_before_success = 1u;
    asx_reconnecting_transport_init(&reconnecting, &reconnect_state, inner, 2u);

    ASSERT_EQ(asx_transport_connect(&reconnecting, &addr, sizeof(addr), &conn), ASX_OK);
    ASSERT_EQ(asx_transport_read(&reconnecting, conn, buf, sizeof(buf), &nread), ASX_OK);
    ASSERT_EQ(nread, 5u);
    ASSERT_TRUE(memcmp(buf, "reply", 5u) == 0);
    ASSERT_EQ(inner_state.connect_count, 2u);
    ASSERT_EQ(asx_reconnecting_transport_reconnect_count(&reconnect_state), 1u);
}

TEST(reconnecting_respects_reconnect_budget) {
    reconnect_mock_state inner_state;
    asx_transport inner = make_reconnect_mock_transport(&inner_state);
    asx_reconnecting_transport_state reconnect_state;
    asx_transport reconnecting;
    asx_transport_conn conn;
    uint32_t addr = 1500u;
    size_t written = 0u;

    inner_state.fail_writes_before_success = 2u;
    asx_reconnecting_transport_init(&reconnecting, &reconnect_state, inner, 1u);

    ASSERT_EQ(asx_transport_connect(&reconnecting, &addr, sizeof(addr), &conn), ASX_OK);
    ASSERT_EQ(asx_transport_write(&reconnecting, conn, "abc", 3u, &written), ASX_E_DISCONNECTED);
    ASSERT_EQ(written, 0u);
    ASSERT_EQ(inner_state.connect_count, 2u);
    ASSERT_EQ(asx_reconnecting_transport_reconnect_count(&reconnect_state), 1u);
    ASSERT_EQ(asx_reconnecting_transport_last_status(&reconnect_state), ASX_E_DISCONNECTED);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== transport tests ===\n");

    /* Connection handles */
    RUN_TEST(conn_invalid_sentinel);
    RUN_TEST(conn_valid_after_set);

    /* Memory transport init */
    RUN_TEST(mem_transport_init_zero_state);

    /* Listen */
    RUN_TEST(mem_listen_succeeds);
    RUN_TEST(mem_listen_null_addr_fails);
    RUN_TEST(mem_listen_exhaustion);

    /* Connect + accept */
    RUN_TEST(mem_connect_to_listener);
    RUN_TEST(mem_connect_no_listener_fails);
    RUN_TEST(mem_accept_pending_connection);
    RUN_TEST(mem_accept_no_pending_returns_pending);

    /* Read/write */
    RUN_TEST(mem_write_and_read);
    RUN_TEST(mem_client_cannot_read_its_own_write);
    RUN_TEST(mem_read_empty_returns_pending);
    RUN_TEST(mem_write_tracks_stats);

    /* Close */
    RUN_TEST(mem_close_connection);
    RUN_TEST(mem_close_listener);
    RUN_TEST(mem_close_invalid_conn_fails);

    /* Reset */
    RUN_TEST(mem_reset_clears_all);

    /* Framed transport */
    RUN_TEST(framed_write_and_read_roundtrip);
    RUN_TEST(framed_empty_frame);
    RUN_TEST(framed_write_retries_partial_body_without_duplicate_header);
    RUN_TEST(framed_read_retries_partial_body_until_complete);
    RUN_TEST(framed_read_rejects_too_small_buffer_without_consuming_body);
    RUN_TEST(framed_reset_clears_state);

    /* Reconnecting transport */
    RUN_TEST(reconnecting_write_retries_after_disconnect);
    RUN_TEST(reconnecting_read_retries_after_disconnect);
    RUN_TEST(reconnecting_respects_reconnect_budget);

    TEST_REPORT();
    return test_failures;
}
