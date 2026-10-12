/*
 * asx/net/tls.h — TLS connector/acceptor and stream types over a provider
 *
 * The core carries no cryptography. A TLS stream is a session of a
 * provider: a table of callbacks, in the style of asx_runtime_hooks, that a
 * TLS implementation supplies (an adapter over mbedTLS, BearSSL, the
 * platform's own stack). The core owns what is not cryptography: the
 * stream handles, the configuration, the handshake/shutdown state machine
 * and the surface gate.
 *
 * asx_tls_connect and asx_tls_accept refuse a configuration that names no
 * provider (ASX_E_HOOK_MISSING). The one provider the core ships,
 * asx_tls_null_provider(), moves bytes in the clear; it exists for tests
 * and simulations and is never selected by default.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_TLS_H
#define ASX_NET_TLS_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/net/net.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_TLS_SURFACE
/* -------------------------------------------------------------------
 * TLS version and cipher configuration
 * ------------------------------------------------------------------- */

typedef enum { ASX_TLS_VERSION_1_2 = 0, ASX_TLS_VERSION_1_3 = 1 } asx_tls_version;

typedef enum {
    ASX_TLS_VERIFY_NONE = 0,
    ASX_TLS_VERIFY_PEER = 1,
    ASX_TLS_VERIFY_REQUIRED = 2
} asx_tls_verify_mode;

typedef enum {
    ASX_TLS_ROOT_SOURCE_NONE = 0,
    ASX_TLS_ROOT_SOURCE_NATIVE = 1,
    ASX_TLS_ROOT_SOURCE_WEBPKI = 2
} asx_tls_root_source;

typedef enum { ASX_TLS_ROLE_CLIENT = 0, ASX_TLS_ROLE_SERVER = 1 } asx_tls_role;

#if (ASX_MAX_TLS_STREAMS) < 1
#error "ASX_MAX_TLS_STREAMS must be at least 1"
#endif

struct asx_tls_provider;

/* -------------------------------------------------------------------
 * TLS configuration (value type)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_tls_version min_version;
    asx_tls_version max_version;
    asx_tls_verify_mode verify_mode;
    asx_tls_root_source root_source;
    char sni_hostname[ASX_TLS_MAX_HOSTNAME];
    char alpn_protocols[ASX_TLS_MAX_ALPN][ASX_TLS_ALPN_MAX_LEN];
    uint32_t alpn_count;
    /* The provider that carries this configuration's sessions. NULL until
     * asx_tls_config_set_provider: connect and accept then refuse with
     * ASX_E_HOOK_MISSING. Borrowed: it must outlive every stream opened
     * with this configuration. */
    const struct asx_tls_provider *provider;
} asx_tls_config;

/* Initialize TLS config to safe defaults (TLS 1.2+, verify peer, no
 * provider). */
ASX_API void asx_tls_config_init(asx_tls_config *cfg);

/* Get a human-readable trust-root source name. */
ASX_API ASX_MUST_USE const char *asx_tls_root_source_str(asx_tls_root_source source);

/* Set SNI hostname for client connections. */
ASX_API asx_status asx_tls_config_set_sni(asx_tls_config *cfg, const char *hostname);

/* Select the trust-root source for peer verification. */
ASX_API asx_status asx_tls_config_set_root_source(asx_tls_config *cfg, asx_tls_root_source source);

/* Add an ALPN protocol name (e.g. "h2", "http/1.1"). */
ASX_API asx_status asx_tls_config_add_alpn(asx_tls_config *cfg, const char *protocol);

/* -------------------------------------------------------------------
 * TLS providers
 * ------------------------------------------------------------------- */

/* One TLS session as the core tracks it and a provider's callbacks see it.
 * The core fills in everything but negotiated_alpn and provider_state;
 * the session lives in its stream's slot until the stream is closed. */
typedef struct {
    asx_tls_role role;
    asx_tcp_stream tcp;                         /* the transport the session runs over */
    const asx_tls_config *config;               /* the stream's own copy of its configuration */
    char negotiated_alpn[ASX_TLS_ALPN_MAX_LEN]; /* the provider's to fill; "" if none */
    void *provider_state;                       /* the provider's; the core never touches it */
} asx_tls_session;

/* The callbacks of a TLS implementation. Every callback receives `ctx`.
 * handshake_fn, read_fn, write_fn and shutdown_fn are required; open_fn,
 * close_fn and peer_cert_fn may be NULL.
 *
 *   open_fn       Called once, before the first handshake step, to set up
 *                 provider_state. A failure stops the connect or accept.
 *   handshake_fn  Advance the handshake over session->tcp. ASX_OK when it
 *                 is complete, ASX_E_PENDING while it needs more I/O, any
 *                 other status when it failed (the stream is then in
 *                 ASX_TLS_STATE_ERROR).
 *   read_fn       Decrypt into dst; the contract of asx_tcp_stream_poll_read.
 *   write_fn      Encrypt src to the wire; the contract of
 *                 asx_tcp_stream_poll_write.
 *   shutdown_fn   Send close_notify. ASX_OK when sent, ASX_E_PENDING while
 *                 it is still being flushed.
 *   close_fn      Release provider_state. Called once, when the stream is
 *                 closed or its connect failed after open_fn.
 *   peer_cert_fn  Copy the peer's leaf certificate (DER) into out, set
 *                 *out_len; ASX_E_BUFFER_TOO_SMALL if capacity is short,
 *                 ASX_E_NOT_FOUND if the peer sent none. */
typedef struct asx_tls_provider {
    const char *name; /* for diagnostics */
    void *ctx;
    asx_status (*open_fn)(void *ctx, asx_tls_session *session);
    asx_status (*handshake_fn)(void *ctx, asx_tls_session *session);
    asx_status (*read_fn)(void *ctx, asx_tls_session *session, asx_buf_mut *dst,
                          uint32_t *bytes_read);
    asx_status (*write_fn)(void *ctx, asx_tls_session *session, const asx_buf *src,
                           uint32_t *bytes_written);
    asx_status (*shutdown_fn)(void *ctx, asx_tls_session *session);
    void (*close_fn)(void *ctx, asx_tls_session *session);
    asx_status (*peer_cert_fn)(void *ctx, asx_tls_session *session, uint8_t *out, uint32_t capacity,
                               uint32_t *out_len);
} asx_tls_provider;

/* Select the provider of a configuration. NULL clears it (connect and
 * accept then refuse). Returns ASX_E_INVALID_ARGUMENT for a NULL cfg and
 * ASX_E_HOOK_INVALID for a provider missing a required callback; the
 * configuration is unchanged in both cases. */
ASX_API ASX_MUST_USE asx_status asx_tls_config_set_provider(asx_tls_config *cfg,
                                                            const asx_tls_provider *provider);

/* The null provider: plaintext passthrough over the TCP stream, for tests
 * and simulations only. It performs no handshake, authenticates no peer and
 * encrypts nothing; a stream over it is exactly as confidential as the
 * TCP stream beneath it, whatever its configuration says about
 * certificates. It "negotiates" the first configured ALPN protocol. It is
 * never installed by default: a caller must pass it to
 * asx_tls_config_set_provider on purpose. */
ASX_API ASX_MUST_USE const asx_tls_provider *asx_tls_null_provider(void);

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
    ASX_TLS_STATE_INIT = 0,
    ASX_TLS_STATE_HANDSHAKE = 1,
    ASX_TLS_STATE_READY = 2,
    ASX_TLS_STATE_SHUTDOWN = 3,
    ASX_TLS_STATE_CLOSED = 4,
    ASX_TLS_STATE_ERROR = 5
} asx_tls_state;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_tls_stream;

/* -------------------------------------------------------------------
 * TLS stream API
 * ------------------------------------------------------------------- */

/* Open a TLS client session over a TCP stream and take its first handshake
 * step. Returns ASX_OK with the stream in ASX_TLS_STATE_READY, or in
 * ASX_TLS_STATE_HANDSHAKE when the provider needs more I/O (drive it with
 * asx_tls_stream_poll_handshake; reads and writes drive it too).
 * ASX_E_HOOK_MISSING when the connector names no provider,
 * ASX_E_HOOK_INVALID for an incomplete one; any other provider failure is
 * returned with no stream left open. */
ASX_API ASX_MUST_USE asx_status asx_tls_connect(asx_tls_stream *out, const asx_tls_connector *conn,
                                                asx_tcp_stream tcp);

/* Open a TLS server session over a TCP stream; as asx_tls_connect. */
ASX_API ASX_MUST_USE asx_status asx_tls_accept(asx_tls_stream *out, const asx_tls_acceptor *acc,
                                               asx_tcp_stream tcp);

/* Advance a stream's handshake one step. ASX_OK once it is READY,
 * ASX_E_PENDING while the provider needs more I/O, the provider's status
 * (the stream is then in ASX_TLS_STATE_ERROR) when it failed,
 * ASX_E_INVALID_STATE for a stream past its handshake. */
ASX_API ASX_MUST_USE asx_status asx_tls_stream_poll_handshake(asx_tls_stream stream);

/* Poll-read decrypted data from a TLS stream. While the handshake is
 * unfinished this drives it and returns ASX_E_PENDING. */
ASX_API ASX_MUST_USE asx_status asx_tls_stream_poll_read(asx_tls_stream stream, asx_buf_mut *dst,
                                                         uint32_t *bytes_read);

/* Poll-write data through a TLS stream (encrypted on wire). While the
 * handshake is unfinished this drives it and returns ASX_E_PENDING. */
ASX_API ASX_MUST_USE asx_status asx_tls_stream_poll_write(asx_tls_stream stream, const asx_buf *src,
                                                          uint32_t *bytes_written);

/* Get the current TLS handshake/connection state. */
ASX_API asx_tls_state asx_tls_stream_state(asx_tls_stream stream);

/* The provider a stream runs over, NULL for a stream that is not alive. */
ASX_API const asx_tls_provider *asx_tls_stream_provider(asx_tls_stream stream);

/* Get the negotiated ALPN protocol (empty string if none). */
ASX_API asx_status asx_tls_stream_alpn(asx_tls_stream stream, char *out, uint32_t out_capacity);

/* Copy the peer's leaf certificate (DER) into out and set *out_len.
 * ASX_E_NOT_FOUND when the provider has no certificate to give (the null
 * provider never has one), ASX_E_BUFFER_TOO_SMALL when capacity is short,
 * ASX_E_INVALID_STATE before the stream is READY. */
ASX_API asx_status asx_tls_stream_peer_certificate(asx_tls_stream stream, uint8_t *out,
                                                   uint32_t capacity, uint32_t *out_len);

/* Initiate TLS shutdown (close_notify). ASX_E_PENDING while the provider is
 * still flushing it; the stream stays READY until it is sent. */
ASX_API asx_status asx_tls_stream_shutdown(asx_tls_stream stream);

/* Close and release a TLS stream. */
ASX_API asx_status asx_tls_stream_close(asx_tls_stream stream);

/* Check if a TLS stream is alive. */
ASX_API int asx_tls_stream_is_alive(asx_tls_stream stream);

/* Reset all TLS state (test support). Streams still alive are released
 * through their provider's close_fn. */
ASX_API void asx_tls_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_TLS_H */
