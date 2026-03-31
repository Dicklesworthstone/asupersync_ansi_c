/*
 * asx/net/websocket.h — deterministic WebSocket framing and connection types
 *
 * Provides WebSocket client/server handshake, frame encoding/decoding,
 * message send/receive, and close protocol. In the deterministic core,
 * WebSocket operates over in-memory TCP streams without real HTTP upgrade.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_WEBSOCKET_H
#define ASX_NET_WEBSOCKET_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/net/net.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * WebSocket constants and types
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_WS_OPCODE_TEXT = 1,
    ASX_WS_OPCODE_BINARY = 2,
    ASX_WS_OPCODE_CLOSE = 8,
    ASX_WS_OPCODE_PING = 9,
    ASX_WS_OPCODE_PONG = 10
} asx_ws_opcode;

typedef enum {
    ASX_WS_STATE_CONNECTING = 0,
    ASX_WS_STATE_OPEN = 1,
    ASX_WS_STATE_CLOSING = 2,
    ASX_WS_STATE_CLOSED = 3
} asx_ws_state;

typedef enum { ASX_WS_ROLE_CLIENT = 0, ASX_WS_ROLE_SERVER = 1 } asx_ws_role;

#ifndef ASX_WS_MAX_PAYLOAD
#define ASX_WS_MAX_PAYLOAD 4096u
#endif

#ifndef ASX_MAX_WS_CONNECTIONS
#define ASX_MAX_WS_CONNECTIONS 8u
#endif

/* -------------------------------------------------------------------
 * WebSocket frame (value type for send/receive)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_ws_opcode opcode;
    uint8_t payload[ASX_WS_MAX_PAYLOAD];
    uint32_t payload_len;
    uint8_t fin;
    uint16_t close_code;
} asx_ws_frame;

/* Initialize a frame to empty text. */
ASX_API void asx_ws_frame_init(asx_ws_frame *frame);

/* Set frame payload from a buffer. */
ASX_API asx_status asx_ws_frame_set_payload(asx_ws_frame *frame, asx_ws_opcode opcode,
                                            const void *data, uint32_t len);

/* Create a close frame with status code. */
ASX_API asx_status asx_ws_frame_close(asx_ws_frame *frame, uint16_t code);

/* -------------------------------------------------------------------
 * WebSocket connection handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_ws_conn;

/* -------------------------------------------------------------------
 * WebSocket API
 * ------------------------------------------------------------------- */

/* Perform a WebSocket client handshake over a TCP stream.
 * In deterministic mode, immediately transitions to OPEN state. */
ASX_API ASX_MUST_USE asx_status asx_ws_connect(asx_ws_conn *out, asx_tcp_stream tcp);

/* Accept a WebSocket server handshake over a TCP stream. */
ASX_API ASX_MUST_USE asx_status asx_ws_accept(asx_ws_conn *out, asx_tcp_stream tcp);

/* Send a WebSocket frame. */
ASX_API ASX_MUST_USE asx_status asx_ws_send(asx_ws_conn conn, const asx_ws_frame *frame);

/* Poll-receive a WebSocket frame. Returns ASX_E_PENDING if no frame available. */
ASX_API ASX_MUST_USE asx_status asx_ws_poll_recv(asx_ws_conn conn, asx_ws_frame *out);

/* Get the current connection state. */
ASX_API asx_ws_state asx_ws_conn_state(asx_ws_conn conn);

/* Get the connection role (client or server). */
ASX_API asx_ws_role asx_ws_conn_role(asx_ws_conn conn);

/* Initiate a close handshake with code. */
ASX_API asx_status asx_ws_close(asx_ws_conn conn, uint16_t code);

/* Release a WebSocket connection. */
ASX_API asx_status asx_ws_conn_release(asx_ws_conn conn);

/* Check if a WebSocket connection is alive. */
ASX_API int asx_ws_conn_is_alive(asx_ws_conn conn);

/* Reset all WebSocket state (test support). */
ASX_API void asx_ws_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_WEBSOCKET_H */
