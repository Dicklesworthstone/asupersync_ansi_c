/*
 * asx/net/quic.h — deterministic QUIC connection and stream types
 *
 * Provides QUIC connection lifecycle, bidirectional/unidirectional streams,
 * and datagram support. In deterministic core, QUIC operates over in-memory
 * transport with simulated connection state.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_QUIC_H
#define ASX_NET_QUIC_H

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
    ASX_QUIC_STATE_IDLE        = 0,
    ASX_QUIC_STATE_HANDSHAKE   = 1,
    ASX_QUIC_STATE_CONNECTED   = 2,
    ASX_QUIC_STATE_DRAINING    = 3,
    ASX_QUIC_STATE_CLOSED      = 4
} asx_quic_state;

typedef enum {
    ASX_QUIC_STREAM_BIDI = 0,
    ASX_QUIC_STREAM_UNI  = 1
} asx_quic_stream_type;

#ifndef ASX_MAX_QUIC_CONNECTIONS
#define ASX_MAX_QUIC_CONNECTIONS 4u
#endif

#ifndef ASX_MAX_QUIC_STREAMS
#define ASX_MAX_QUIC_STREAMS 16u
#endif

#ifndef ASX_QUIC_MAX_DGRAM
#define ASX_QUIC_MAX_DGRAM 1200u
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

/* Create a QUIC client connection (deterministic handshake). */
ASX_API ASX_MUST_USE asx_status asx_quic_connect(asx_quic_conn *out,
                                                   const asx_quic_config *cfg);

/* Accept a QUIC server connection (deterministic handshake). */
ASX_API ASX_MUST_USE asx_status asx_quic_accept(asx_quic_conn *out,
                                                  const asx_quic_config *cfg);

/* Get the current connection state. */
ASX_API asx_quic_state asx_quic_conn_state(asx_quic_conn conn);

/* Initiate graceful connection close with error code. */
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

/* Send a datagram on a QUIC connection (unreliable). */
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
