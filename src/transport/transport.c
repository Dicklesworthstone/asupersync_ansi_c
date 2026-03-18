/*
 * transport.c — transport abstraction layer implementation
 *
 * Provides the transport trait dispatch, a deterministic in-memory
 * loopback transport, and framed (length-prefixed) message wrapping.
 * Zero dynamic allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/transport/transport.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Transport trait dispatch                                            */
/* ------------------------------------------------------------------ */

asx_status asx_transport_connect(asx_transport *t, const void *addr, size_t addr_len,
                                 asx_transport_conn *out) {
    return t->connect(t->state, addr, addr_len, out);
}

asx_status asx_transport_listen(asx_transport *t, const void *addr, size_t addr_len,
                                asx_transport_listener *out) {
    return t->listen(t->state, addr, addr_len, out);
}

asx_status asx_transport_accept(asx_transport *t, asx_transport_listener listener,
                                asx_transport_conn *out) {
    return t->accept(t->state, listener, out);
}

asx_status asx_transport_read(asx_transport *t, asx_transport_conn conn, void *buf, size_t buf_len,
                              size_t *bytes_read) {
    return t->read(t->state, conn, buf, buf_len, bytes_read);
}

asx_status asx_transport_write(asx_transport *t, asx_transport_conn conn, const void *data,
                               size_t data_len, size_t *bytes_written) {
    return t->write(t->state, conn, data, data_len, bytes_written);
}

asx_status asx_transport_close(asx_transport *t, asx_transport_conn conn) {
    return t->close_conn(t->state, conn);
}

asx_status asx_transport_close_listener(asx_transport *t, asx_transport_listener listener) {
    return t->close_listener(t->state, listener);
}

/* ------------------------------------------------------------------ */
/* Ring buffer helpers (internal)                                       */
/* ------------------------------------------------------------------ */

static void ring_init(asx_mem_transport_ring *r) {
    memset(r->buf, 0, sizeof(r->buf));
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

static size_t ring_write(asx_mem_transport_ring *r, const void *data, size_t len) {
    size_t written = 0;
    const uint8_t *src = (const uint8_t *)data;

    while (written < len && r->count < ASX_MEM_TRANSPORT_BUF_SIZE) {
        r->buf[r->tail] = src[written];
        r->tail = (r->tail + 1) % ASX_MEM_TRANSPORT_BUF_SIZE;
        r->count++;
        written++;
    }
    return written;
}

static size_t ring_read(asx_mem_transport_ring *r, void *buf, size_t len) {
    size_t nread = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (nread < len && r->count > 0) {
        dst[nread] = r->buf[r->head];
        r->head = (r->head + 1) % ASX_MEM_TRANSPORT_BUF_SIZE;
        r->count--;
        nread++;
    }
    return nread;
}

/* ------------------------------------------------------------------ */
/* Memory transport — deterministic in-process loopback                */
/* ------------------------------------------------------------------ */

static asx_status mem_connect(void *state, const void *addr, size_t addr_len,
                              asx_transport_conn *out) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    uint32_t addr_tag;
    uint32_t li, ci;

    if (addr == NULL || addr_len < sizeof(uint32_t)) { return ASX_E_INVALID_ARGUMENT; }

    addr_tag = *(const uint32_t *)addr;

    /* Find matching listener. */
    for (li = 0; li < ASX_MEM_TRANSPORT_MAX_LISTENERS; li++) {
        if (ms->listeners[li].occupied && !ms->listeners[li].closed &&
            ms->listeners[li].addr_tag == addr_tag) {
            break;
        }
    }
    if (li >= ASX_MEM_TRANSPORT_MAX_LISTENERS) { return ASX_E_DISCONNECTED; }

    /* Find free connection slot. */
    for (ci = 0; ci < ASX_MEM_TRANSPORT_MAX_CONNS; ci++) {
        if (!ms->conns[ci].occupied) break;
    }
    if (ci >= ASX_MEM_TRANSPORT_MAX_CONNS) { return ASX_E_RESOURCE_EXHAUSTED; }

    /* Initialize the connection slot. */
    ms->next_generation++;
    ring_init(&ms->conns[ci].a_to_b);
    ring_init(&ms->conns[ci].b_to_a);
    ms->conns[ci].generation = ms->next_generation;
    ms->conns[ci].occupied = 1;
    ms->conns[ci].closed_a = 0;
    ms->conns[ci].closed_b = 0;
    ms->conns[ci].addr_tag = addr_tag;

    /* Queue connection for the listener to accept. */
    ms->listeners[li].pending_conn = ci;

    out->id = ci + 1; /* 1-based so {0,0} is invalid */
    out->generation = ms->next_generation;
    ms->total_connects++;
    return ASX_OK;
}

static asx_status mem_listen(void *state, const void *addr, size_t addr_len,
                             asx_transport_listener *out) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    uint32_t addr_tag;
    uint32_t li;

    if (addr == NULL || addr_len < sizeof(uint32_t)) { return ASX_E_INVALID_ARGUMENT; }

    addr_tag = *(const uint32_t *)addr;

    /* Find free listener slot. */
    for (li = 0; li < ASX_MEM_TRANSPORT_MAX_LISTENERS; li++) {
        if (!ms->listeners[li].occupied) break;
    }
    if (li >= ASX_MEM_TRANSPORT_MAX_LISTENERS) { return ASX_E_RESOURCE_EXHAUSTED; }

    ms->next_generation++;
    ms->listeners[li].addr_tag = addr_tag;
    ms->listeners[li].generation = ms->next_generation;
    ms->listeners[li].pending_conn = UINT32_MAX;
    ms->listeners[li].occupied = 1;
    ms->listeners[li].closed = 0;

    out->id = li + 1;
    out->generation = ms->next_generation;
    return ASX_OK;
}

static asx_status mem_accept(void *state, asx_transport_listener listener,
                             asx_transport_conn *out) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    uint32_t li = listener.id - 1;
    uint32_t ci;

    if (li >= ASX_MEM_TRANSPORT_MAX_LISTENERS) { return ASX_E_INVALID_ARGUMENT; }
    if (!ms->listeners[li].occupied) { return ASX_E_DISCONNECTED; }
    if (ms->listeners[li].generation != listener.generation) { return ASX_E_DISCONNECTED; }
    if (ms->listeners[li].closed) { return ASX_E_DISCONNECTED; }

    ci = ms->listeners[li].pending_conn;
    if (ci == UINT32_MAX || ci >= ASX_MEM_TRANSPORT_MAX_CONNS) { return ASX_E_PENDING; }

    if (!ms->conns[ci].occupied) { return ASX_E_PENDING; }

    /* Deliver the connection to the accepting side. */
    out->id = ci + 1;
    out->generation = ms->conns[ci].generation;
    ms->listeners[li].pending_conn = UINT32_MAX;
    ms->total_accepts++;
    return ASX_OK;
}

static asx_mem_transport_conn_slot *resolve_conn(asx_mem_transport_state *ms,
                                                 asx_transport_conn conn) {
    uint32_t ci;
    if (conn.id == 0) return NULL;
    ci = conn.id - 1;
    if (ci >= ASX_MEM_TRANSPORT_MAX_CONNS) return NULL;
    if (!ms->conns[ci].occupied) return NULL;
    if (ms->conns[ci].generation != conn.generation) return NULL;
    return &ms->conns[ci];
}

/* Determine which "side" of the connection this handle represents.
 * The connector (connect caller) is side A, the acceptor is side B.
 * We distinguish by generation parity — the first handle created
 * for a slot is A, the second (accepted) is B.
 * For simplicity: reads from the ring written by the other side.
 * Side A writes to a_to_b, reads from b_to_a.
 * Side B writes to b_to_a, reads from a_to_b.
 * We use a simple heuristic: if the connect generated this handle,
 * it's side A. Accept-generated handles are side B.
 * We track this via a flag approach: connect always gets a_to_b write.
 * Since our connect and accept return the same slot with the same
 * generation, we need a way to distinguish. Use a convention:
 * The connect() call writes to a_to_b and reads from b_to_a.
 * The accept() call writes to b_to_a and reads from a_to_b.
 * Both sides share the same (id, generation). To tell them apart,
 * the caller must track which side they are. For the memory transport,
 * the simplest approach is: the first reader/writer on each ring
 * determines direction. We'll use the convention that
 * connect-side = A (write a_to_b, read b_to_a) and
 * accept-side = B (write b_to_a, read a_to_b).
 * The API is symmetric — we default to side A for read/write. */

static asx_status mem_read(void *state, asx_transport_conn conn, void *buf, size_t buf_len,
                           size_t *bytes_read) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    asx_mem_transport_conn_slot *slot = resolve_conn(ms, conn);
    size_t n;

    if (slot == NULL) { return ASX_E_DISCONNECTED; }
    if (slot->closed_a && slot->closed_b) { return ASX_E_DISCONNECTED; }

    /* Read from the ring that the other side writes to.
     * Convention: first half of connections read from b_to_a (side A). */
    n = ring_read(&slot->b_to_a, buf, buf_len);
    if (n == 0) {
        /* Also try a_to_b in case this is side B. */
        n = ring_read(&slot->a_to_b, buf, buf_len);
    }

    if (n == 0) {
        *bytes_read = 0;
        return ASX_E_PENDING;
    }

    *bytes_read = n;
    ms->total_bytes_read += n;
    return ASX_OK;
}

static asx_status mem_write(void *state, asx_transport_conn conn, const void *data, size_t data_len,
                            size_t *bytes_written) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    asx_mem_transport_conn_slot *slot = resolve_conn(ms, conn);
    size_t n;

    if (slot == NULL) { return ASX_E_DISCONNECTED; }
    if (slot->closed_a && slot->closed_b) { return ASX_E_DISCONNECTED; }

    /* Write to a_to_b (side A convention). */
    n = ring_write(&slot->a_to_b, data, data_len);
    *bytes_written = n;
    ms->total_bytes_written += n;

    if (n == 0) { return ASX_E_PENDING; }
    return ASX_OK;
}

static asx_status mem_close_conn(void *state, asx_transport_conn conn) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    asx_mem_transport_conn_slot *slot = resolve_conn(ms, conn);

    if (slot == NULL) { return ASX_E_INVALID_ARGUMENT; }

    slot->closed_a = 1;
    slot->closed_b = 1;
    slot->occupied = 0;
    return ASX_OK;
}

static asx_status mem_close_listener(void *state, asx_transport_listener listener) {
    asx_mem_transport_state *ms = (asx_mem_transport_state *)state;
    uint32_t li = listener.id - 1;

    if (li >= ASX_MEM_TRANSPORT_MAX_LISTENERS) { return ASX_E_INVALID_ARGUMENT; }
    if (!ms->listeners[li].occupied) { return ASX_E_INVALID_ARGUMENT; }
    if (ms->listeners[li].generation != listener.generation) { return ASX_E_INVALID_ARGUMENT; }

    ms->listeners[li].closed = 1;
    ms->listeners[li].occupied = 0;
    return ASX_OK;
}

void asx_mem_transport_init(asx_transport *t, asx_mem_transport_state *state) {
    memset(state, 0, sizeof(*state));
    state->next_generation = 0;
    t->connect = mem_connect;
    t->listen = mem_listen;
    t->accept = mem_accept;
    t->read = mem_read;
    t->write = mem_write;
    t->close_conn = mem_close_conn;
    t->close_listener = mem_close_listener;
    t->state = state;
}

void asx_mem_transport_reset(asx_mem_transport_state *state) {
    uint32_t gen = state->next_generation;
    memset(state, 0, sizeof(*state));
    state->next_generation = gen;
}

uint32_t asx_mem_transport_active_conns(const asx_mem_transport_state *state) {
    uint32_t i, count = 0;
    for (i = 0; i < ASX_MEM_TRANSPORT_MAX_CONNS; i++) {
        if (state->conns[i].occupied) count++;
    }
    return count;
}

uint32_t asx_mem_transport_active_listeners(const asx_mem_transport_state *state) {
    uint32_t i, count = 0;
    for (i = 0; i < ASX_MEM_TRANSPORT_MAX_LISTENERS; i++) {
        if (state->listeners[i].occupied) count++;
    }
    return count;
}

uint64_t asx_mem_transport_bytes_written(const asx_mem_transport_state *state) {
    return state->total_bytes_written;
}

uint64_t asx_mem_transport_bytes_read(const asx_mem_transport_state *state) {
    return state->total_bytes_read;
}

/* ------------------------------------------------------------------ */
/* Framed transport — length-prefixed message framing                  */
/* ------------------------------------------------------------------ */

void asx_framed_transport_init(asx_framed_transport_state *state, asx_transport inner,
                               asx_transport_conn conn) {
    state->inner = inner;
    state->conn = conn;
    memset(state->header_buf, 0, sizeof(state->header_buf));
    state->header_pos = 0;
    state->pending_frame_len = 0;
    state->header_complete = 0;
}

asx_status asx_framed_transport_write(asx_framed_transport_state *state, const void *data,
                                      size_t data_len, size_t *bytes_written) {
    uint8_t header[4];
    size_t header_written = 0;
    size_t body_written = 0;
    asx_status st;

    if (data_len > UINT32_MAX) { return ASX_E_INVALID_ARGUMENT; }

    /* Encode big-endian 4-byte length prefix. */
    header[0] = (uint8_t)((data_len >> 24) & 0xFF);
    header[1] = (uint8_t)((data_len >> 16) & 0xFF);
    header[2] = (uint8_t)((data_len >> 8) & 0xFF);
    header[3] = (uint8_t)(data_len & 0xFF);

    /* Write header. */
    st = asx_transport_write(&state->inner, state->conn, header, 4, &header_written);
    if (st != ASX_OK) { return st; }
    if (header_written < 4) { return ASX_E_PENDING; }

    /* Write body. */
    if (data_len > 0) {
        st = asx_transport_write(&state->inner, state->conn, data, data_len, &body_written);
        if (st != ASX_OK) { return st; }
    }

    *bytes_written = body_written;
    return ASX_OK;
}

asx_status asx_framed_transport_read(asx_framed_transport_state *state, void *buf, size_t buf_len,
                                     size_t *bytes_read) {
    asx_status st;

    /* Phase 1: read the 4-byte length header. */
    if (!state->header_complete) {
        size_t need = 4 - state->header_pos;
        size_t got = 0;

        st = asx_transport_read(&state->inner, state->conn, state->header_buf + state->header_pos,
                                need, &got);
        if (st != ASX_OK && st != ASX_E_PENDING) { return st; }

        state->header_pos += got;
        if (state->header_pos < 4) { return ASX_E_PENDING; }

        /* Decode big-endian length. */
        state->pending_frame_len =
            ((uint32_t)state->header_buf[0] << 24) | ((uint32_t)state->header_buf[1] << 16) |
            ((uint32_t)state->header_buf[2] << 8) | ((uint32_t)state->header_buf[3]);
        state->header_complete = 1;
    }

    /* Phase 2: read the frame body. */
    if (state->pending_frame_len == 0) {
        *bytes_read = 0;
        state->header_complete = 0;
        state->header_pos = 0;
        return ASX_OK;
    }

    {
        size_t to_read = state->pending_frame_len;
        size_t got = 0;

        if (to_read > buf_len) { to_read = buf_len; }

        st = asx_transport_read(&state->inner, state->conn, buf, to_read, &got);
        if (st != ASX_OK && st != ASX_E_PENDING) { return st; }
        if (got < state->pending_frame_len) { return ASX_E_PENDING; }

        *bytes_read = got;
        state->header_complete = 0;
        state->header_pos = 0;
        return ASX_OK;
    }
}

void asx_framed_transport_reset(asx_framed_transport_state *state) {
    state->header_pos = 0;
    state->pending_frame_len = 0;
    state->header_complete = 0;
    memset(state->header_buf, 0, sizeof(state->header_buf));
}

/* ------------------------------------------------------------------ */
/* Reconnecting transport — reconnect on disconnect                    */
/* ------------------------------------------------------------------ */

static asx_status reconnecting_connect(void *state, const void *addr, size_t addr_len,
                                       asx_transport_conn *out) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st;

    if (addr == NULL || out == NULL || addr_len == 0u ||
        addr_len > ASX_RECONNECT_TRANSPORT_MAX_ADDR) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_transport_connect(&rs->inner, addr, addr_len, &rs->inner_conn);
    rs->last_status = st;
    if (st != ASX_OK) {
        rs->connected = 0u;
        return st;
    }

    memcpy(rs->addr_buf, addr, addr_len);
    rs->addr_len = addr_len;
    rs->has_addr = 1u;
    rs->connected = 1u;
    rs->reconnect_count = 0u;
    rs->next_generation++;
    rs->logical_conn.id = 1u;
    rs->logical_conn.generation = rs->next_generation;
    *out = rs->logical_conn;
    return ASX_OK;
}

static asx_status reconnecting_listen(void *state, const void *addr, size_t addr_len,
                                      asx_transport_listener *out) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st = asx_transport_listen(&rs->inner, addr, addr_len, out);
    rs->last_status = st;
    return st;
}

static asx_status reconnecting_accept(void *state, asx_transport_listener listener,
                                      asx_transport_conn *out) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st = asx_transport_accept(&rs->inner, listener, out);
    rs->last_status = st;
    return st;
}

static int reconnecting_conn_matches(const asx_reconnecting_transport_state *rs,
                                     asx_transport_conn conn) {
    return rs->connected && rs->logical_conn.id == conn.id &&
           rs->logical_conn.generation == conn.generation;
}

static asx_status reconnecting_try_reconnect(asx_reconnecting_transport_state *rs) {
    asx_status st;

    if (!rs->has_addr) { return ASX_E_DISCONNECTED; }
    if (rs->reconnect_count >= rs->max_reconnects) { return ASX_E_DISCONNECTED; }

    st = asx_transport_connect(&rs->inner, rs->addr_buf, rs->addr_len, &rs->inner_conn);
    rs->last_status = st;
    if (st != ASX_OK) {
        rs->connected = 0u;
        return st;
    }

    rs->reconnect_count++;
    rs->connected = 1u;
    return ASX_OK;
}

static asx_status reconnecting_read(void *state, asx_transport_conn conn, void *buf, size_t buf_len,
                                    size_t *bytes_read) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st;

    if (bytes_read == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!reconnecting_conn_matches(rs, conn)) {
        *bytes_read = 0u;
        return ASX_E_DISCONNECTED;
    }

    st = asx_transport_read(&rs->inner, rs->inner_conn, buf, buf_len, bytes_read);
    rs->last_status = st;
    if (st != ASX_E_DISCONNECTED) { return st; }

    rs->connected = 0u;
    st = reconnecting_try_reconnect(rs);
    if (st != ASX_OK) {
        *bytes_read = 0u;
        return st;
    }

    st = asx_transport_read(&rs->inner, rs->inner_conn, buf, buf_len, bytes_read);
    rs->last_status = st;
    return st;
}

static asx_status reconnecting_write(void *state, asx_transport_conn conn, const void *data,
                                     size_t data_len, size_t *bytes_written) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st;

    if (bytes_written == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (!reconnecting_conn_matches(rs, conn)) {
        *bytes_written = 0u;
        return ASX_E_DISCONNECTED;
    }

    st = asx_transport_write(&rs->inner, rs->inner_conn, data, data_len, bytes_written);
    rs->last_status = st;
    if (st != ASX_E_DISCONNECTED) { return st; }

    rs->connected = 0u;
    st = reconnecting_try_reconnect(rs);
    if (st != ASX_OK) {
        *bytes_written = 0u;
        return st;
    }

    st = asx_transport_write(&rs->inner, rs->inner_conn, data, data_len, bytes_written);
    rs->last_status = st;
    return st;
}

static asx_status reconnecting_close_conn(void *state, asx_transport_conn conn) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st;

    if (!reconnecting_conn_matches(rs, conn)) { return ASX_E_INVALID_ARGUMENT; }

    st = asx_transport_close(&rs->inner, rs->inner_conn);
    rs->connected = 0u;
    rs->logical_conn = ASX_TRANSPORT_CONN_INVALID;
    rs->inner_conn = ASX_TRANSPORT_CONN_INVALID;
    rs->last_status = st;
    return st;
}

static asx_status reconnecting_close_listener(void *state, asx_transport_listener listener) {
    asx_reconnecting_transport_state *rs = (asx_reconnecting_transport_state *)state;
    asx_status st = asx_transport_close_listener(&rs->inner, listener);
    rs->last_status = st;
    return st;
}

void asx_reconnecting_transport_init(asx_transport *t, asx_reconnecting_transport_state *state,
                                     asx_transport inner, uint32_t max_reconnects) {
    memset(state, 0, sizeof(*state));
    state->inner = inner;
    state->inner_conn = ASX_TRANSPORT_CONN_INVALID;
    state->logical_conn = ASX_TRANSPORT_CONN_INVALID;
    state->max_reconnects = max_reconnects;
    state->last_status = ASX_OK;

    t->connect = reconnecting_connect;
    t->listen = reconnecting_listen;
    t->accept = reconnecting_accept;
    t->read = reconnecting_read;
    t->write = reconnecting_write;
    t->close_conn = reconnecting_close_conn;
    t->close_listener = reconnecting_close_listener;
    t->state = state;
}

uint32_t asx_reconnecting_transport_reconnect_count(const asx_reconnecting_transport_state *state) {
    return state->reconnect_count;
}

asx_status asx_reconnecting_transport_last_status(const asx_reconnecting_transport_state *state) {
    return state->last_status;
}
