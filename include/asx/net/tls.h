/*
 * asx/net/tls.h — deterministic TLS connector/acceptor and stream types
 *
 * Provides the public TLS surface for the async runtime. In the deterministic
 * core profile, TLS is modeled as a passthrough wrapper over TCP streams with
 * handshake state tracking and configurable certificate/cipher parameters.
 * Real cryptographic operations are deferred to platform adapters (Wave C).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_TLS_H
#define ASX_NET_TLS_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/net/net.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * TLS version and cipher configuration
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_TLS_VERSION_1_2 = 0,
    ASX_TLS_VERSION_1_3 = 1
} asx_tls_version;

typedef enum {
    ASX_TLS_VERIFY_NONE     = 0,
    ASX_TLS_VERIFY_PEER     = 1,
    ASX_TLS_VERIFY_REQUIRED = 2
} asx_tls_verify_mode;

#ifndef ASX_TLS_MAX_HOSTNAME
#define ASX_TLS_MAX_HOSTNAME 64u
#endif

#ifndef ASX_TLS_MAX_ALPN
#define ASX_TLS_MAX_ALPN 4u
#endif

#ifndef ASX_TLS_ALPN_MAX_LEN
#define ASX_TLS_ALPN_MAX_LEN 16u
#endif

#ifndef ASX_MAX_TLS_STREAMS
#define ASX_MAX_TLS_STREAMS 8u
#endif

/* -------------------------------------------------------------------
 * TLS configuration (value type)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_tls_version min_version;
    asx_tls_version max_version;
    asx_tls_verify_mode verify_mode;
    char sni_hostname[ASX_TLS_MAX_HOSTNAME];
    char alpn_protocols[ASX_TLS_MAX_ALPN][ASX_TLS_ALPN_MAX_LEN];
    uint32_t alpn_count;
} asx_tls_config;

/* Initialize TLS config to safe defaults (TLS 1.2+, verify peer). */
ASX_API void asx_tls_config_init(asx_tls_config *cfg);

/* Set SNI hostname for client connections. */
ASX_API asx_status asx_tls_config_set_sni(asx_tls_config *cfg, const char *hostname);

/* Add an ALPN protocol name (e.g. "h2", "http/1.1"). */
ASX_API asx_status asx_tls_config_add_alpn(asx_tls_config *cfg, const char *protocol);

/* -------------------------------------------------------------------
 * TLS connector and acceptor
 * ------------------------------------------------------------------- */

typedef struct {
    asx_tls_config config;
} asx_tls_connector;

typedef struct {
    asx_tls_config config;
} asx_tls_acceptor;

/* Initialize a TLS connector (client-side). */
ASX_API void asx_tls_connector_init(asx_tls_connector *conn, const asx_tls_config *cfg);

/* Initialize a TLS acceptor (server-side). */
ASX_API void asx_tls_acceptor_init(asx_tls_acceptor *acc, const asx_tls_config *cfg);

/* -------------------------------------------------------------------
 * TLS stream handle
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_TLS_STATE_INIT       = 0,
    ASX_TLS_STATE_HANDSHAKE  = 1,
    ASX_TLS_STATE_READY      = 2,
    ASX_TLS_STATE_SHUTDOWN   = 3,
    ASX_TLS_STATE_CLOSED     = 4,
    ASX_TLS_STATE_ERROR      = 5
} asx_tls_state;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_tls_stream;

/* -------------------------------------------------------------------
 * TLS stream API
 * ------------------------------------------------------------------- */

/* Perform a TLS client handshake over a TCP stream.
 * In deterministic mode, transitions immediately to READY. */
ASX_API ASX_MUST_USE asx_status asx_tls_connect(asx_tls_stream *out,
                                                  const asx_tls_connector *conn,
                                                  asx_tcp_stream tcp);

/* Perform a TLS server handshake over a TCP stream.
 * In deterministic mode, transitions immediately to READY. */
ASX_API ASX_MUST_USE asx_status asx_tls_accept(asx_tls_stream *out,
                                                 const asx_tls_acceptor *acc,
                                                 asx_tcp_stream tcp);

/* Poll-read decrypted data from a TLS stream. */
ASX_API ASX_MUST_USE asx_status asx_tls_stream_poll_read(asx_tls_stream stream, asx_buf_mut *dst,
                                                          uint32_t *bytes_read);

/* Poll-write data through a TLS stream (encrypted on wire). */
ASX_API ASX_MUST_USE asx_status asx_tls_stream_poll_write(asx_tls_stream stream,
                                                           const asx_buf *src,
                                                           uint32_t *bytes_written);

/* Get the current TLS handshake/connection state. */
ASX_API asx_tls_state asx_tls_stream_state(asx_tls_stream stream);

/* Get the negotiated ALPN protocol (empty string if none). */
ASX_API asx_status asx_tls_stream_alpn(asx_tls_stream stream, char *out, uint32_t out_capacity);

/* Initiate TLS shutdown (close_notify). */
ASX_API asx_status asx_tls_stream_shutdown(asx_tls_stream stream);

/* Close and release a TLS stream. */
ASX_API asx_status asx_tls_stream_close(asx_tls_stream stream);

/* Check if a TLS stream is alive. */
ASX_API int asx_tls_stream_is_alive(asx_tls_stream stream);

/* Reset all TLS state (test support). */
ASX_API void asx_tls_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_TLS_H */
