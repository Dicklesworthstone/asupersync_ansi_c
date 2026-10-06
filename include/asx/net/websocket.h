/*
 * asx/net/websocket.h — RFC 6455 WebSocket protocol engine
 *
 * Three layers:
 *
 *   1. Frame codec (pure functions over byte buffers): header encode/decode
 *      with 7/16/64-bit payload lengths, masking, frame validation, and an
 *      incremental strict UTF-8 validator.
 *
 *   2. Sans-IO engine (asx_ws_engine, caller-owned value type): opening
 *      handshake (client Upgrade request / server 101 response with
 *      Sec-WebSocket-Accept = base64(SHA-1(key || GUID)), optional
 *      subprotocol negotiation), fragmentation/continuation reassembly with
 *      a configurable message limit, automatic pong replies, the close
 *      handshake, and protocol-error detection mapped to close codes
 *      1002 / 1007 / 1009. The engine never performs I/O: feed it received
 *      bytes and drain its pending output into any byte transport.
 *
 *   3. Thin adapter (asx_ws_conn handles) that drives an engine over an
 *      asx_tcp_stream with the poll-based read/write API from net.h.
 *
 * Masking keys and the client handshake nonce come from a configurable
 * random source; by default the runtime entropy hook whitened through
 * SHA-256 (deterministic when the hook is seeded).
 *
 * Thread safety: none; engines and connection slots are single-threaded.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_WEBSOCKET_H
#define ASX_NET_WEBSOCKET_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/net/net.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Limits and protocol constants
 * ------------------------------------------------------------------- */

/* Capacity of the per-engine message reassembly buffer (bytes). */
#ifndef ASX_WS_MAX_MESSAGE
#define ASX_WS_MAX_MESSAGE 16384u
#endif

/* Capacity of the per-engine outgoing byte buffer. Must hold one
 * max-size frame plus pending control frames. */
#ifndef ASX_WS_TX_CAPACITY
#define ASX_WS_TX_CAPACITY (ASX_WS_MAX_MESSAGE + 512u)
#endif

/* Maximum size of an HTTP handshake message (request or response). */
#ifndef ASX_WS_HANDSHAKE_MAX
#define ASX_WS_HANDSHAKE_MAX 4096u
#endif

/* Capacity for a comma-separated subprotocol list (including NUL). */
#ifndef ASX_WS_PROTOCOLS_MAX
#define ASX_WS_PROTOCOLS_MAX 128u
#endif

#ifndef ASX_MAX_WS_CONNECTIONS
#define ASX_MAX_WS_CONNECTIONS 8u
#endif

#define ASX_WS_MAX_FRAME_HEADER 14u  /* 2 + 8 (64-bit length) + 4 (mask) */
#define ASX_WS_CONTROL_MAX 125u      /* control frame payload limit */
#define ASX_WS_CLOSE_REASON_MAX 123u /* 125 - 2-byte status code */
#define ASX_WS_KEY_LEN 24u           /* base64 of a 16-byte nonce */
#define ASX_WS_ACCEPT_LEN 28u        /* base64 of a 20-byte SHA-1 digest */
#define ASX_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Close status codes (RFC 6455 §7.4.1) */
#define ASX_WS_CLOSE_NORMAL 1000u
#define ASX_WS_CLOSE_GOING_AWAY 1001u
#define ASX_WS_CLOSE_PROTOCOL_ERROR 1002u
#define ASX_WS_CLOSE_UNSUPPORTED_DATA 1003u
#define ASX_WS_CLOSE_NO_STATUS 1005u /* never sent on the wire */
#define ASX_WS_CLOSE_ABNORMAL 1006u  /* never sent on the wire */
#define ASX_WS_CLOSE_INVALID_PAYLOAD 1007u
#define ASX_WS_CLOSE_POLICY_VIOLATION 1008u
#define ASX_WS_CLOSE_MESSAGE_TOO_BIG 1009u
#define ASX_WS_CLOSE_MANDATORY_EXTENSION 1010u
#define ASX_WS_CLOSE_INTERNAL_ERROR 1011u

typedef enum {
    ASX_WS_OPCODE_CONTINUATION = 0x0,
    ASX_WS_OPCODE_TEXT = 0x1,
    ASX_WS_OPCODE_BINARY = 0x2,
    ASX_WS_OPCODE_CLOSE = 0x8,
    ASX_WS_OPCODE_PING = 0x9,
    ASX_WS_OPCODE_PONG = 0xA
} asx_ws_opcode;

typedef enum {
    ASX_WS_STATE_CONNECTING = 0, /* opening handshake in progress */
    ASX_WS_STATE_OPEN = 1,       /* data transfer */
    ASX_WS_STATE_CLOSING = 2,    /* our close frame sent, awaiting the peer's */
    ASX_WS_STATE_CLOSED = 3      /* close handshake done or connection failed */
} asx_ws_state;

typedef enum { ASX_WS_ROLE_CLIENT = 0, ASX_WS_ROLE_SERVER = 1 } asx_ws_role;

/* -------------------------------------------------------------------
 * Frame codec
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t fin;         /* FIN bit */
    uint8_t rsv;         /* RSV1..RSV3 as bits 2..0 */
    uint8_t opcode;      /* raw 4-bit opcode */
    uint8_t masked;      /* MASK bit */
    uint8_t mask_key[4]; /* valid when masked */
    uint64_t payload_len;
    uint32_t header_len; /* encoded header size on the wire (2..14) */
} asx_ws_frame_header;

/* Returns 1 for close/ping/pong opcodes, 0 otherwise. */
ASX_API int asx_ws_opcode_is_control(uint8_t opcode);

/* Encode a frame header using the minimal payload-length form.
 * header_len in h is ignored and the encoded size is written to *out_len.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT (NULL, opcode > 0xF, rsv > 7,
 * payload_len >= 2^63), or ASX_E_BUFFER_TOO_SMALL. */
ASX_API ASX_MUST_USE asx_status asx_ws_header_encode(const asx_ws_frame_header *h, uint8_t *out,
                                                     uint32_t cap, uint32_t *out_len);

/* Decode a frame header from the start of in.
 * Returns ASX_OK (h filled, h->header_len set), ASX_E_PENDING when more
 * bytes are needed, or ASX_E_INVALID_ARGUMENT for wire-format violations
 * (non-minimal length encoding, 64-bit length with the MSB set). Opcode,
 * RSV, control-frame, and masking rules are checked separately by
 * asx_ws_header_check(). */
ASX_API ASX_MUST_USE asx_status asx_ws_header_decode(const uint8_t *in, uint32_t len,
                                                     asx_ws_frame_header *h);

/* Validate a decoded header as received by an endpoint of receiver_role
 * with no extensions negotiated. Returns 0 when valid, otherwise the close
 * code to fail with (ASX_WS_CLOSE_PROTOCOL_ERROR for RSV bits, reserved
 * opcodes, fragmented or oversized control frames, unmasked client frames at
 * a server, or masked server frames at a client). */
ASX_API uint16_t asx_ws_header_check(const asx_ws_frame_header *h, asx_ws_role receiver_role);

/* Encode a complete frame (header + payload). mask_key == NULL produces an
 * unmasked frame; otherwise the payload is XOR-masked with the 4-byte key.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT (NULL args, control frame with
 * fin == 0 or payload > 125), or ASX_E_BUFFER_TOO_SMALL. */
ASX_API ASX_MUST_USE asx_status asx_ws_frame_encode(asx_ws_opcode opcode, int fin,
                                                    const uint8_t *mask_key, const uint8_t *payload,
                                                    uint32_t payload_len, uint8_t *out,
                                                    uint32_t cap, uint32_t *out_len);

/* XOR-(un)mask len bytes in place. offset is the payload position of
 * data[0], so a payload can be processed in arbitrary chunks. */
ASX_API void asx_ws_mask(uint8_t *data, uint32_t len, const uint8_t key[4], uint64_t offset);

/* Incremental strict UTF-8 validator (rejects overlongs, surrogates,
 * code points above U+10FFFF, and truncated sequences). */
typedef struct {
    uint8_t need;   /* continuation bytes still expected */
    uint8_t lo;     /* lower bound for the next continuation byte */
    uint8_t hi;     /* upper bound for the next continuation byte */
    uint8_t failed; /* sticky failure flag */
} asx_ws_utf8;

/* Reset a validator to the start-of-text state. */
ASX_API void asx_ws_utf8_init(asx_ws_utf8 *v);

/* Feed bytes; returns 1 while the stream is still valid, 0 once invalid. */
ASX_API int asx_ws_utf8_feed(asx_ws_utf8 *v, const uint8_t *data, size_t len);

/* Returns 1 when the stream is valid and not inside a multi-byte sequence. */
ASX_API int asx_ws_utf8_complete(const asx_ws_utf8 *v);

/* One-shot strict UTF-8 check. Returns 1 when data is valid UTF-8. */
ASX_API int asx_ws_utf8_valid(const uint8_t *data, size_t len);

/* Returns 1 if code may be sent in a close frame
 * (1000-1003, 1007-1014, 3000-4999). */
ASX_API int asx_ws_close_code_sendable(uint16_t code);

/* Returns 1 if a received close frame may carry code
 * (1000-1003, 1007-1014, 1016-4999; never 1004-1006 or 1015). */
ASX_API int asx_ws_close_code_receivable(uint16_t code);

/* -------------------------------------------------------------------
 * Opening handshake helpers
 * ------------------------------------------------------------------- */

/* Compute Sec-WebSocket-Accept = base64(SHA-1(key || ASX_WS_GUID)) and
 * NUL-terminate it into out. Returns ASX_OK or ASX_E_INVALID_ARGUMENT. */
ASX_API ASX_MUST_USE asx_status asx_ws_compute_accept(const char *key, size_t key_len,
                                                      char out[ASX_WS_ACCEPT_LEN + 1]);

/* Build a client Upgrade request. host is required; path defaults to "/";
 * origin and protocols (comma-separated offer) are optional.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT (missing host/key, CR/LF in a
 * field), or ASX_E_BUFFER_TOO_SMALL. out is NUL-terminated. */
ASX_API ASX_MUST_USE asx_status asx_ws_handshake_build_request(const char *host, const char *path,
                                                               const char *origin,
                                                               const char *protocols,
                                                               const char *key, char *out,
                                                               size_t cap, size_t *out_len);

/* -------------------------------------------------------------------
 * Sans-IO protocol engine
 * ------------------------------------------------------------------- */

/* Random source for masking keys and the client nonce. */
typedef asx_status (*asx_ws_random_fn)(void *ctx, uint8_t *out, uint32_t len);

typedef struct {
    asx_ws_role role;
    uint32_t max_message;       /* 0 or > ASX_WS_MAX_MESSAGE -> ASX_WS_MAX_MESSAGE */
    asx_ws_random_fn random_fn; /* NULL -> whitened runtime entropy hook */
    void *random_ctx;
    const char *host;      /* client: Host header (required unless skip_handshake) */
    const char *path;      /* client: request target, default "/" */
    const char *origin;    /* client: optional Origin header */
    const char *protocols; /* client: offered list; server: supported list */
    int skip_handshake;    /* start OPEN (upgrade already done by an outer layer) */
} asx_ws_config;

/* Fill a config with defaults for the given role. */
ASX_API void asx_ws_config_init(asx_ws_config *cfg, asx_ws_role role);

typedef struct {
    asx_ws_role role;
    asx_ws_state state;
    uint32_t max_message;
    asx_ws_random_fn random_fn;
    void *random_ctx;

    /* opening handshake */
    char key[ASX_WS_KEY_LEN + 1];
    char protocols[ASX_WS_PROTOCOLS_MAX]; /* client offer / server support list */
    char protocol[ASX_WS_PROTOCOLS_MAX];  /* negotiated subprotocol ("" = none) */
    uint8_t hs_buf[ASX_WS_HANDSHAKE_MAX];
    uint32_t hs_len;

    /* receive path */
    uint8_t rx_in_payload;
    uint8_t hdr_buf[ASX_WS_MAX_FRAME_HEADER];
    uint32_t hdr_len;
    asx_ws_frame_header cur;
    uint64_t payload_pos;
    uint8_t ctrl_buf[ASX_WS_CONTROL_MAX];
    uint8_t msg_buf[ASX_WS_MAX_MESSAGE];
    uint32_t msg_len;
    uint8_t msg_opcode;
    uint8_t msg_in_progress;
    uint8_t msg_ready;
    asx_ws_utf8 rx_utf8;

    /* transmit path */
    uint8_t tx_buf[ASX_WS_TX_CAPACITY];
    uint32_t tx_start;
    uint32_t tx_len;
    uint8_t tx_fragmenting;
    uint8_t tx_frag_opcode;
    asx_ws_utf8 tx_utf8;

    /* close / failure bookkeeping */
    uint8_t close_sent;
    uint8_t close_received;
    uint8_t failed;
    uint16_t close_code; /* peer close code (1005 none, 1006 abnormal) */
    uint16_t error_code; /* close code we failed the connection with */
    char close_reason[ASX_WS_CLOSE_REASON_MAX + 1];
    uint32_t pings_received;
    uint32_t pongs_received;
} asx_ws_engine;

/* Initialize an engine. A client engine generates a fresh 16-byte nonce and
 * queues its Upgrade request; a server engine waits for one. With
 * skip_handshake the engine starts OPEN.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT (bad config, protocols list too
 * long), ASX_E_BUFFER_TOO_SMALL (request exceeds the handshake limit), or
 * the random source's error status. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_init(asx_ws_engine *ws, const asx_ws_config *cfg);

/* Feed received bytes. *consumed reports how many were taken; the engine
 * stops early (returning ASX_OK) while a complete message awaits
 * asx_ws_engine_recv() or while pending output leaves no room for an
 * automatic control reply — drain/receive and feed the rest again.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT, or ASX_E_DISCONNECTED once the
 * engine has failed the connection (handshake rejected or protocol error;
 * see asx_ws_engine_error_code(); a close frame or HTTP error response may
 * be pending in the output). Bytes after a completed close are discarded. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_feed(asx_ws_engine *ws, const uint8_t *data,
                                                   uint32_t len, uint32_t *consumed);

/* Take the next complete message. Returns ASX_OK, ASX_E_PENDING when none is
 * ready, ASX_E_BUFFER_TOO_SMALL (message retained; *len = required size), or
 * ASX_E_INVALID_ARGUMENT. buf may be NULL when cap == 0. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_recv(asx_ws_engine *ws, asx_ws_opcode *opcode,
                                                   uint8_t *buf, uint32_t cap, uint32_t *len);

/* Queue one frame. Data frames: TEXT/BINARY start a message and
 * CONTINUATION continues one; fin ends it. Control frames (PING/PONG)
 * require fin and <= 125 bytes; use asx_ws_engine_close() for CLOSE.
 * Text payloads must keep the message valid UTF-8 (checked incrementally).
 * Client frames are masked with a fresh key from the random source.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT (bad opcode/sequence/UTF-8/size),
 * ASX_E_INVALID_STATE (not OPEN, or close already sent),
 * ASX_E_WOULD_BLOCK (drain output first), ASX_E_BUFFER_TOO_SMALL (frame can
 * never fit ASX_WS_TX_CAPACITY; fragment it), or the random source status.
 * Failure-atomic: nothing is queued on error. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_send_frame(asx_ws_engine *ws, asx_ws_opcode opcode,
                                                         int fin, const uint8_t *data,
                                                         uint32_t len);

/* Queue a complete (unfragmented) TEXT, BINARY, PING, or PONG message.
 * Same contract as asx_ws_engine_send_frame() with fin = 1. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_send(asx_ws_engine *ws, asx_ws_opcode opcode,
                                                   const uint8_t *data, uint32_t len);

/* Start (or answer) the close handshake. code 0 sends an empty close
 * payload; otherwise code must be sendable and reason (optional) valid
 * UTF-8 of at most 123 bytes. Returns ASX_OK, ASX_E_INVALID_ARGUMENT,
 * ASX_E_INVALID_STATE (handshake incomplete or close already sent),
 * ASX_E_WOULD_BLOCK, or the random source status. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_close(asx_ws_engine *ws, uint16_t code,
                                                    const char *reason);

/* View pending output bytes (valid until the next engine call).
 * Returns the number of pending bytes; *data may be NULL when 0. */
ASX_API uint32_t asx_ws_engine_output(const asx_ws_engine *ws, const uint8_t **data);

/* Mark n pending output bytes as written to the transport.
 * Returns ASX_OK or ASX_E_INVALID_ARGUMENT when n exceeds the pending count. */
ASX_API ASX_MUST_USE asx_status asx_ws_engine_consume_output(asx_ws_engine *ws, uint32_t n);

/* Report that the transport closed. An engine that has not completed the
 * close handshake moves to CLOSED with close code 1006 (abnormal). */
ASX_API void asx_ws_engine_transport_closed(asx_ws_engine *ws);

/* Current connection state. */
ASX_API asx_ws_state asx_ws_engine_state(const asx_ws_engine *ws);

/* Peer's close code: 0 while no close frame was received, 1005 when it had
 * no status code, 1006 after an abnormal transport closure. */
ASX_API uint16_t asx_ws_engine_close_code(const asx_ws_engine *ws);

/* Peer's close reason ("" when none). Never NULL. */
ASX_API const char *asx_ws_engine_close_reason(const asx_ws_engine *ws);

/* Close code the engine failed the connection with (0 if it has not
 * failed): 1002 protocol error/handshake failure, 1007 invalid payload,
 * 1009 message too big. */
ASX_API uint16_t asx_ws_engine_error_code(const asx_ws_engine *ws);

/* Negotiated subprotocol ("" when none). Never NULL. */
ASX_API const char *asx_ws_engine_protocol(const asx_ws_engine *ws);

/* -------------------------------------------------------------------
 * TCP adapter (connection handles over asx_tcp_stream)
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_ws_conn;

/* Start a client connection over a connected TCP stream (cfg->role is
 * forced to client) and flush the Upgrade request. The stream stays owned
 * by the caller. Returns ASX_OK, ASX_E_INVALID_ARGUMENT,
 * ASX_E_RESOURCE_EXHAUSTED (no free slot), or an engine init error. */
ASX_API ASX_MUST_USE asx_status asx_ws_connect(asx_ws_conn *out, asx_tcp_stream tcp,
                                               const asx_ws_config *cfg);

/* Start a server connection over an accepted TCP stream (cfg->role is
 * forced to server; cfg may be NULL for defaults). Same error contract as
 * asx_ws_connect(). */
ASX_API ASX_MUST_USE asx_status asx_ws_accept(asx_ws_conn *out, asx_tcp_stream tcp,
                                              const asx_ws_config *cfg);

/* Drive I/O: flush pending output to the stream, read available bytes into
 * the engine, and flush any replies (handshake, pongs, close echoes).
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT (stale handle), ASX_E_DISCONNECTED
 * when the engine failed the connection or the TCP stream was closed (the
 * engine then reports close code 1006 unless the close handshake had
 * completed), or a TCP error status. */
ASX_API ASX_MUST_USE asx_status asx_ws_poll(asx_ws_conn conn);

/* Queue a frame (see asx_ws_engine_send_frame) and flush. */
ASX_API ASX_MUST_USE asx_status asx_ws_send_frame(asx_ws_conn conn, asx_ws_opcode opcode, int fin,
                                                  const void *data, uint32_t len);

/* Queue a complete TEXT/BINARY/PING/PONG message and flush. */
ASX_API ASX_MUST_USE asx_status asx_ws_send(asx_ws_conn conn, asx_ws_opcode opcode,
                                            const void *data, uint32_t len);

/* Drive I/O, then take the next complete message (see asx_ws_engine_recv).
 * Returns ASX_E_PENDING when no message is ready. */
ASX_API ASX_MUST_USE asx_status asx_ws_poll_recv(asx_ws_conn conn, asx_ws_opcode *opcode, void *buf,
                                                 uint32_t cap, uint32_t *len);

/* Initiate the close handshake (see asx_ws_engine_close) and flush. */
ASX_API ASX_MUST_USE asx_status asx_ws_close(asx_ws_conn conn, uint16_t code, const char *reason);

/* Current state (CLOSED for stale handles). */
ASX_API asx_ws_state asx_ws_conn_state(asx_ws_conn conn);

/* Connection role (CLIENT for stale handles). */
ASX_API asx_ws_role asx_ws_conn_role(asx_ws_conn conn);

/* Read-only view of the connection's engine (NULL for stale handles). */
ASX_API const asx_ws_engine *asx_ws_conn_engine(asx_ws_conn conn);

/* Release a connection slot. The TCP stream is not closed. */
ASX_API asx_status asx_ws_conn_release(asx_ws_conn conn);

/* Returns 1 if the handle refers to a live connection slot. */
ASX_API int asx_ws_conn_is_alive(asx_ws_conn conn);

/* Reset all WebSocket connection slots (test support). */
ASX_API void asx_ws_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_WEBSOCKET_H */
