/*
 * asx/transport/transport.h — transport abstraction layer
 *
 * Provides the Transport trait (listen/connect/read/write) and concrete
 * adapters: memory (in-process loopback), and abstract transport
 * composition (framed, reconnecting).
 *
 * Design:
 *   - Transport trait is a set of function pointers for I/O operations.
 *   - Connection is a handle representing one established link.
 *   - Listener is a handle that accepts incoming connections.
 *   - Zero dynamic allocation — all types are stack-allocable value types.
 *   - Data is passed as (const void*, size_t) pairs.
 *   - Integrates with asx_status for error propagation.
 *
 * Poll convention:
 *   ASX_OK           — operation completed
 *   ASX_E_PENDING    — would block, try again later
 *   ASX_E_DISCONNECTED — connection/listener closed
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TRANSPORT_TRANSPORT_H
#define ASX_TRANSPORT_TRANSPORT_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Transport connection handle                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t id;
    uint32_t generation;
} asx_transport_conn;

/* Sentinel value for an invalid connection. */
#define ASX_TRANSPORT_CONN_INVALID ((asx_transport_conn){0, 0})

/* Check if a connection handle is valid. */
static inline int asx_transport_conn_is_valid(asx_transport_conn c) {
    return c.id != 0 || c.generation != 0;
}

/* ------------------------------------------------------------------ */
/* Transport listener handle                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t id;
    uint32_t generation;
} asx_transport_listener;

/* Sentinel value for an invalid listener. */
#define ASX_TRANSPORT_LISTENER_INVALID ((asx_transport_listener){0, 0})

/* ------------------------------------------------------------------ */
/* Transport trait — abstract I/O channel                              */
/* ------------------------------------------------------------------ */

/* Connect to a remote endpoint identified by opaque address data. */
typedef asx_status (*asx_transport_connect_fn)(void *state, const void *addr, size_t addr_len,
                                               asx_transport_conn *out);

/* Listen on a local endpoint for incoming connections. */
typedef asx_status (*asx_transport_listen_fn)(void *state, const void *addr, size_t addr_len,
                                              asx_transport_listener *out);

/* Accept an incoming connection on a listener. */
typedef asx_status (*asx_transport_accept_fn)(void *state, asx_transport_listener listener,
                                              asx_transport_conn *out);

/* Read data from a connection. */
typedef asx_status (*asx_transport_read_fn)(void *state, asx_transport_conn conn, void *buf,
                                            size_t buf_len, size_t *bytes_read);

/* Write data to a connection. */
typedef asx_status (*asx_transport_write_fn)(void *state, asx_transport_conn conn, const void *data,
                                             size_t data_len, size_t *bytes_written);

/* Close a connection. */
typedef asx_status (*asx_transport_close_fn)(void *state, asx_transport_conn conn);

/* Close a listener. */
typedef asx_status (*asx_transport_close_listener_fn)(void *state, asx_transport_listener listener);

typedef struct {
    asx_transport_connect_fn connect;
    asx_transport_listen_fn listen;
    asx_transport_accept_fn accept;
    asx_transport_read_fn read;
    asx_transport_write_fn write;
    asx_transport_close_fn close_conn;
    asx_transport_close_listener_fn close_listener;
    void *state;
} asx_transport;

/* Connect via transport. */
ASX_API ASX_MUST_USE asx_status asx_transport_connect(asx_transport *t, const void *addr,
                                                      size_t addr_len, asx_transport_conn *out);

/* Listen via transport. */
ASX_API ASX_MUST_USE asx_status asx_transport_listen(asx_transport *t, const void *addr,
                                                     size_t addr_len, asx_transport_listener *out);

/* Accept via transport. */
ASX_API ASX_MUST_USE asx_status asx_transport_accept(asx_transport *t,
                                                     asx_transport_listener listener,
                                                     asx_transport_conn *out);

/* Read from connection. */
ASX_API ASX_MUST_USE asx_status asx_transport_read(asx_transport *t, asx_transport_conn conn,
                                                   void *buf, size_t buf_len, size_t *bytes_read);

/* Write to connection. */
ASX_API ASX_MUST_USE asx_status asx_transport_write(asx_transport *t, asx_transport_conn conn,
                                                    const void *data, size_t data_len,
                                                    size_t *bytes_written);

/* Close a connection. */
ASX_API asx_status asx_transport_close(asx_transport *t, asx_transport_conn conn);

/* Close a listener. */
ASX_API asx_status asx_transport_close_listener(asx_transport *t, asx_transport_listener listener);

/* ------------------------------------------------------------------ */
/* Memory transport — deterministic in-process loopback                */
/* ------------------------------------------------------------------ */

/* Arena limits for the memory transport. */
#ifndef ASX_MEM_TRANSPORT_MAX_CONNS
#define ASX_MEM_TRANSPORT_MAX_CONNS 16u
#endif

#ifndef ASX_MEM_TRANSPORT_MAX_LISTENERS
#define ASX_MEM_TRANSPORT_MAX_LISTENERS 4u
#endif

#ifndef ASX_MEM_TRANSPORT_BUF_SIZE
#define ASX_MEM_TRANSPORT_BUF_SIZE 1024u
#endif

typedef struct {
    uint8_t buf[ASX_MEM_TRANSPORT_BUF_SIZE];
    size_t head;  /* read offset */
    size_t tail;  /* write offset */
    size_t count; /* bytes available */
} asx_mem_transport_ring;

typedef struct {
    asx_mem_transport_ring a_to_b; /* data from side A to side B */
    asx_mem_transport_ring b_to_a; /* data from side B to side A */
    uint32_t generation;
    uint8_t occupied;
    uint8_t closed_a;
    uint8_t closed_b;
    uint32_t addr_tag; /* matches the listener addr_tag for loopback routing */
} asx_mem_transport_conn_slot;

typedef struct {
    uint32_t addr_tag;
    uint32_t generation;
    uint32_t pending_conn; /* slot index of a pending connection, or UINT32_MAX */
    uint8_t occupied;
    uint8_t closed;
} asx_mem_transport_listener_slot;

typedef struct {
    asx_mem_transport_conn_slot conns[ASX_MEM_TRANSPORT_MAX_CONNS];
    asx_mem_transport_listener_slot listeners[ASX_MEM_TRANSPORT_MAX_LISTENERS];
    uint32_t next_generation;
    uint32_t total_connects;
    uint32_t total_accepts;
    uint64_t total_bytes_written;
    uint64_t total_bytes_read;
} asx_mem_transport_state;

/* Initialize a memory transport instance. */
ASX_API void asx_mem_transport_init(asx_transport *t, asx_mem_transport_state *state);

/* Reset all connections and listeners. */
ASX_API void asx_mem_transport_reset(asx_mem_transport_state *state);

/* Return the number of active connections. */
ASX_API uint32_t asx_mem_transport_active_conns(const asx_mem_transport_state *state);

/* Return the number of active listeners. */
ASX_API uint32_t asx_mem_transport_active_listeners(const asx_mem_transport_state *state);

/* Return total bytes written across all connections. */
ASX_API uint64_t asx_mem_transport_bytes_written(const asx_mem_transport_state *state);

/* Return total bytes read across all connections. */
ASX_API uint64_t asx_mem_transport_bytes_read(const asx_mem_transport_state *state);

/* ------------------------------------------------------------------ */
/* Framed transport — length-prefixed message framing                  */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_transport inner;
    asx_transport_conn conn;    /* connection being framed */
    uint8_t header_buf[4];      /* 4-byte big-endian length prefix */
    size_t header_pos;          /* bytes of header read so far */
    uint32_t pending_frame_len; /* announced frame body length */
    int header_complete;        /* nonzero when header has been read */
    uint8_t *body_buf;          /* caller buffer reused until frame completes */
    uint32_t body_bytes_read;   /* body bytes assembled so far */
    uint8_t write_header_buf[4]; /* 4-byte big-endian length prefix for writes */
    size_t write_header_pos;     /* header bytes accepted so far */
    size_t write_body_pos;       /* body bytes accepted so far */
    const uint8_t *write_data;   /* caller payload reused until frame completes */
    size_t write_data_len;       /* payload length for the in-flight write */
    int write_in_progress;       /* nonzero while a framed write is being resumed */
} asx_framed_transport_state;

/* Initialize framed transport wrapping an existing connection. */
ASX_API void asx_framed_transport_init(asx_framed_transport_state *state, asx_transport inner,
                                       asx_transport_conn conn);

/* Write a complete frame (length prefix + data).
 * Returns ASX_E_PENDING if the inner transport only accepts part of the
 * frame. In that case, retry with the same data pointer and length until
 * the write completes or fails. On ASX_OK, *bytes_written is the body
 * length (not including the 4-byte header). */
ASX_API ASX_MUST_USE asx_status asx_framed_transport_write(asx_framed_transport_state *state,
                                                           const void *data, size_t data_len,
                                                           size_t *bytes_written);

/* Read the next complete frame. Returns ASX_E_PENDING if the full
 * frame has not yet arrived. Retries after ASX_E_PENDING must reuse the
 * same output buffer until the frame completes. Returns
 * ASX_E_BUFFER_TOO_SMALL if buf_len cannot hold the announced frame body.
 * On ASX_OK, *bytes_read holds the full frame body length (not including
 * the 4-byte header). */
ASX_API ASX_MUST_USE asx_status asx_framed_transport_read(asx_framed_transport_state *state,
                                                          void *buf, size_t buf_len,
                                                          size_t *bytes_read);

/* Reset framing state (e.g., after a protocol error). */
ASX_API void asx_framed_transport_reset(asx_framed_transport_state *state);

/* ------------------------------------------------------------------ */
/* Reconnecting transport — reconnect on disconnect                    */
/* ------------------------------------------------------------------ */

#ifndef ASX_RECONNECT_TRANSPORT_MAX_ADDR
#define ASX_RECONNECT_TRANSPORT_MAX_ADDR 64u
#endif

typedef struct {
    asx_transport inner;
    uint8_t addr_buf[ASX_RECONNECT_TRANSPORT_MAX_ADDR];
    size_t addr_len;
    asx_transport_conn inner_conn;
    asx_transport_conn logical_conn;
    uint32_t next_generation;
    uint32_t reconnect_count;
    uint32_t max_reconnects;
    uint8_t has_addr;
    uint8_t connected;
    asx_status last_status;
} asx_reconnecting_transport_state;

/* Initialize a reconnecting transport wrapper around an inner transport.
 * The wrapper maintains a stable logical handle while swapping the underlying
 * connection on disconnect. */
ASX_API void asx_reconnecting_transport_init(asx_transport *t,
                                             asx_reconnecting_transport_state *state,
                                             asx_transport inner, uint32_t max_reconnects);

/* Return the number of reconnects performed on the current logical session. */
ASX_API uint32_t
asx_reconnecting_transport_reconnect_count(const asx_reconnecting_transport_state *state);

/* Return the last terminal or transient status observed by the wrapper. */
ASX_API asx_status
asx_reconnecting_transport_last_status(const asx_reconnecting_transport_state *state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_TRANSPORT_TRANSPORT_H */
