/*
 * asx/net/quic.h — in-memory QUIC model for lab scenarios
 *
 * MODEL-ONLY (DEC-010). This is a deterministic in-memory model. It is not a
 * QUIC client or server: there is no UDP socket, no TLS 1.3 handshake, no
 * packets, frames, loss recovery or congestion control, and no network
 * protocol of any kind. A success status means the in-process model accepted
 * the call, not that anything left the process.
 *
 * What the model gives a lab scenario is the shape of a connection and stream
 * lifecycle between two ends inside one process:
 *   - asx_quic_connect begins a client handshake. With no network the
 *     connection stays in ASX_QUIC_STATE_HANDSHAKE until asx_quic_accept
 *     pairs it with its server end; both ends are then CONNECTED.
 *   - Datagrams go to the paired peer's queue. Stream bytes are a loopback
 *     buffer: they are read back from the handle that wrote them (the model
 *     has no peer-initiated streams).
 *   - idle_timeout_ms is stored, not enforced.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_QUIC_H
#define ASX_NET_QUIC_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * QUIC configuration and types
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_QUIC_STATE_IDLE = 0,
    ASX_QUIC_STATE_HANDSHAKE = 1,
    ASX_QUIC_STATE_CONNECTED = 2,
    ASX_QUIC_STATE_DRAINING = 3,
    ASX_QUIC_STATE_CLOSED = 4
} asx_quic_state;

typedef enum { ASX_QUIC_STREAM_BIDI = 0, ASX_QUIC_STREAM_UNI = 1 } asx_quic_stream_type;

#if (ASX_MAX_QUIC_CONNECTIONS) < 1 || (ASX_MAX_QUIC_STREAMS) < 1
#error "ASX_MAX_QUIC_CONNECTIONS and ASX_MAX_QUIC_STREAMS must be at least 1"
#endif

/* -------------------------------------------------------------------
 * QUIC configuration (value type)
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_bidi_streams;
    uint32_t max_uni_streams;
    uint32_t idle_timeout_ms;
    uint8_t enable_datagrams;
} asx_quic_config;

/* Initialize QUIC config to safe defaults. */
ASX_API void asx_quic_config_init(asx_quic_config *cfg);

/* -------------------------------------------------------------------
 * QUIC connection handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_quic_conn;

/* -------------------------------------------------------------------
 * QUIC stream handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_quic_stream;

/* -------------------------------------------------------------------
 * QUIC connection API
 * ------------------------------------------------------------------- */

/* Begin a client connection. There is no network, so the handshake does not
 * complete here: the connection is in ASX_QUIC_STATE_HANDSHAKE (streams and
 * datagrams are refused with ASX_E_INVALID_STATE) until an in-process
 * asx_quic_accept pairs it. ASX_E_RESOURCE_EXHAUSTED when no slot is free. */
ASX_API ASX_MUST_USE asx_status asx_quic_connect(asx_quic_conn *out, const asx_quic_config *cfg);

/* Accept the oldest client connection that is waiting in HANDSHAKE and return
 * the server end. Both ends are CONNECTED on ASX_OK. ASX_E_PENDING (and *out
 * untouched) when no connect is waiting; ASX_E_RESOURCE_EXHAUSTED when no slot
 * is free for the server end (the client keeps waiting). */
ASX_API ASX_MUST_USE asx_status asx_quic_accept(asx_quic_conn *out, const asx_quic_config *cfg);

/* Get the current connection state: HANDSHAKE until paired, CONNECTED once
 * paired, DRAINING when the peer closed first, CLOSED for a dead handle. */
ASX_API asx_quic_state asx_quic_conn_state(asx_quic_conn conn);

/* Close a connection. The paired end, if any, moves to DRAINING and can only
 * be closed. A connect that was never accepted simply stops waiting. */
ASX_API asx_status asx_quic_conn_close(asx_quic_conn conn, uint32_t error_code);

/* Check if a QUIC connection is alive. */
ASX_API int asx_quic_conn_is_alive(asx_quic_conn conn);

/* -------------------------------------------------------------------
 * QUIC stream API
 * ------------------------------------------------------------------- */

/* Open a new stream on a connection. */
ASX_API ASX_MUST_USE asx_status asx_quic_stream_open(asx_quic_stream *out, asx_quic_conn conn,
                                                     asx_quic_stream_type stype);

/* Poll-read from a QUIC stream. */
ASX_API ASX_MUST_USE asx_status asx_quic_stream_poll_read(asx_quic_stream stream, asx_buf_mut *dst,
                                                          uint32_t *bytes_read);

/* Poll-write to a QUIC stream. */
ASX_API ASX_MUST_USE asx_status asx_quic_stream_poll_write(asx_quic_stream stream,
                                                           const asx_buf *src,
                                                           uint32_t *bytes_written);

/* Close a QUIC stream. */
ASX_API asx_status asx_quic_stream_close(asx_quic_stream stream);

/* Check if a QUIC stream is alive. */
ASX_API int asx_quic_stream_is_alive(asx_quic_stream stream);

/* -------------------------------------------------------------------
 * QUIC datagram API
 * ------------------------------------------------------------------- */

/* Queue a datagram for the paired end. ASX_E_INVALID_STATE unless the
 * connection is CONNECTED and both ends enabled datagrams. */
ASX_API ASX_MUST_USE asx_status asx_quic_send_datagram(asx_quic_conn conn, const asx_buf *src);

/* Poll-receive a datagram from a QUIC connection. */
ASX_API ASX_MUST_USE asx_status asx_quic_poll_recv_datagram(asx_quic_conn conn, asx_buf_mut *dst,
                                                            uint32_t *bytes_read);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all QUIC connection and stream state (test support). */
ASX_API void asx_quic_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_QUIC_H */
