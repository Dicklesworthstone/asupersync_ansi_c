/*
 * asx/net/net.h — minimal network types and socket primitives
 *
 * Walking-skeleton network surface: address types, TCP/UDP handle
 * types, and poll-based I/O. Platform socket integration is deferred
 * to Phase 3; this layer provides the API shape and ghost stubs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_NET_H
#define ASX_NET_NET_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/cx/cx.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Address family
 * ------------------------------------------------------------------- */

typedef enum { ASX_AF_INET4 = 0, ASX_AF_INET6 = 1 } asx_addr_family;

/* -------------------------------------------------------------------
 * Socket address — value type
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t addr[16]; /* IPv4 in first 4 bytes, IPv6 all 16 */
    uint16_t port;
    asx_addr_family family;
} asx_socket_addr;

/* Construct IPv4 address */
ASX_API asx_socket_addr asx_socket_addr_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                                             uint16_t port);

/* Construct loopback address (127.0.0.1) */
ASX_API asx_socket_addr asx_socket_addr_loopback(uint16_t port);

/* Construct IPv6 address from raw 16-byte storage. */
ASX_API asx_socket_addr asx_socket_addr_ipv6(const uint8_t addr[16], uint16_t port);

/* Construct IPv6 loopback address (::1). */
ASX_API asx_socket_addr asx_socket_addr_ipv6_loopback(uint16_t port);

/* Check if two addresses are equal */
ASX_API int asx_socket_addr_eq(const asx_socket_addr *a, const asx_socket_addr *b);

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_TCP_LISTENERS
#define ASX_MAX_TCP_LISTENERS 4u
#endif

#ifndef ASX_MAX_TCP_STREAMS
#define ASX_MAX_TCP_STREAMS 16u
#endif

#ifndef ASX_MAX_UDP_SOCKETS
#define ASX_MAX_UDP_SOCKETS 16u
#endif

#ifndef ASX_RESOLVE_MAX_RESULTS
#define ASX_RESOLVE_MAX_RESULTS 4u
#endif

/* -------------------------------------------------------------------
 * TCP handle types (forward declarations for cross-references)
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_tcp_listener;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_tcp_stream;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_udp_socket;

typedef struct {
    uint16_t port;
    asx_addr_family preferred_family;
    uint8_t allow_ipv4;
    uint8_t allow_ipv6;
} asx_resolve_options;

typedef struct {
    asx_socket_addr addrs[ASX_RESOLVE_MAX_RESULTS];
    uint32_t count;
} asx_resolve_result;

/* -------------------------------------------------------------------
 * TCP listener API
 * ------------------------------------------------------------------- */

/* Bind a TCP listener to the given address.
 * Walking skeleton: records the address but does not create a real socket. */
ASX_API ASX_MUST_USE asx_status asx_tcp_listener_bind(asx_tcp_listener *out,
                                                      const asx_socket_addr *addr);

/* Bind a TCP listener under explicit communication authority.
 * Fails closed if cx is invalid or lacks ASX_CAP_CHANNEL. */
ASX_API ASX_MUST_USE asx_status asx_tcp_listener_bind_with_cx(asx_tcp_listener *out,
                                                              const asx_socket_addr *addr,
                                                              const asx_cx *cx);

/* Poll for incoming connections.
 * Walking skeleton: always returns ASX_E_PENDING (no real accept). */
ASX_API ASX_MUST_USE asx_status asx_tcp_listener_poll_accept(asx_tcp_listener listener,
                                                             asx_tcp_stream *out,
                                                             asx_socket_addr *peer_addr);

/* Poll for incoming connections under explicit communication authority.
 * Applies a Cx checkpoint before returning the ghost pending result. */
ASX_API ASX_MUST_USE asx_status asx_tcp_listener_poll_accept_with_cx(asx_tcp_listener listener,
                                                                     asx_tcp_stream *out,
                                                                     asx_socket_addr *peer_addr,
                                                                     asx_cx *cx);

/* Close a TCP listener. */
ASX_API asx_status asx_tcp_listener_close(asx_tcp_listener listener);

/* Get the local address of a listener. */
ASX_API asx_status asx_tcp_listener_local_addr(asx_tcp_listener listener, asx_socket_addr *out);

/* Check if a listener is alive. */
ASX_API int asx_tcp_listener_is_alive(asx_tcp_listener listener);

/* -------------------------------------------------------------------
 * TCP stream API
 * ------------------------------------------------------------------- */

/* Initiate a TCP connection.
 * Walking skeleton: creates the handle but does not open a real socket. */
ASX_API ASX_MUST_USE asx_status asx_tcp_connect(asx_tcp_stream *out, const asx_socket_addr *addr);

/* Initiate a TCP connection under explicit communication authority.
 * Fails closed if cx is invalid or lacks ASX_CAP_CHANNEL. */
ASX_API ASX_MUST_USE asx_status asx_tcp_connect_with_cx(asx_tcp_stream *out,
                                                        const asx_socket_addr *addr,
                                                        const asx_cx *cx);

/* Poll-read from a TCP stream.
 * Walking skeleton: always returns ASX_E_PENDING. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_read(asx_tcp_stream stream, asx_buf_mut *dst,
                                                         uint32_t *bytes_read);

/* Poll-read under explicit communication authority.
 * Applies a Cx checkpoint before returning the ghost pending result. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_read_with_cx(asx_tcp_stream stream,
                                                                 asx_buf_mut *dst,
                                                                 uint32_t *bytes_read, asx_cx *cx);

/* Poll-write to a TCP stream.
 * Walking skeleton: always returns ASX_E_PENDING. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_write(asx_tcp_stream stream, const asx_buf *src,
                                                          uint32_t *bytes_written);

/* Poll-write under explicit communication authority.
 * Applies a Cx checkpoint before returning the ghost pending result. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_write_with_cx(asx_tcp_stream stream,
                                                                  const asx_buf *src,
                                                                  uint32_t *bytes_written,
                                                                  asx_cx *cx);

/* Close a TCP stream. */
ASX_API asx_status asx_tcp_stream_close(asx_tcp_stream stream);

/* Get the remote address of a stream. */
ASX_API asx_status asx_tcp_stream_peer_addr(asx_tcp_stream stream, asx_socket_addr *out);

/* Check if a stream is alive. */
ASX_API int asx_tcp_stream_is_alive(asx_tcp_stream stream);

/* -------------------------------------------------------------------
 * UDP socket API
 * ------------------------------------------------------------------- */

/* Bind a UDP socket to the given local address. */
ASX_API ASX_MUST_USE asx_status asx_udp_bind(asx_udp_socket *out, const asx_socket_addr *addr);

/* Bind a UDP socket under explicit communication authority. */
ASX_API ASX_MUST_USE asx_status asx_udp_bind_with_cx(asx_udp_socket *out,
                                                     const asx_socket_addr *addr, const asx_cx *cx);

/* Attach a default remote peer to a bound UDP socket. */
ASX_API ASX_MUST_USE asx_status asx_udp_connect(asx_udp_socket socket,
                                                const asx_socket_addr *peer_addr);

/* Attach a default remote peer under explicit communication authority. */
ASX_API ASX_MUST_USE asx_status asx_udp_connect_with_cx(asx_udp_socket socket,
                                                        const asx_socket_addr *peer_addr,
                                                        const asx_cx *cx);

/* Poll-send a datagram.
 * With no explicit destination, uses the connected peer. */
ASX_API ASX_MUST_USE asx_status asx_udp_poll_send(asx_udp_socket socket, const asx_buf *src,
                                                  uint32_t *bytes_written,
                                                  const asx_socket_addr *to);

/* Poll-send under explicit communication authority. */
ASX_API ASX_MUST_USE asx_status asx_udp_poll_send_with_cx(asx_udp_socket socket, const asx_buf *src,
                                                          uint32_t *bytes_written,
                                                          const asx_socket_addr *to, asx_cx *cx);

/* Poll-receive a datagram. */
ASX_API ASX_MUST_USE asx_status asx_udp_poll_recv(asx_udp_socket socket, asx_buf_mut *dst,
                                                  uint32_t *bytes_read, asx_socket_addr *from);

/* Poll-receive under explicit communication authority. */
ASX_API ASX_MUST_USE asx_status asx_udp_poll_recv_with_cx(asx_udp_socket socket, asx_buf_mut *dst,
                                                          uint32_t *bytes_read,
                                                          asx_socket_addr *from, asx_cx *cx);

/* Close a UDP socket. */
ASX_API asx_status asx_udp_close(asx_udp_socket socket);

/* Get the local address of a UDP socket. */
ASX_API asx_status asx_udp_local_addr(asx_udp_socket socket, asx_socket_addr *out);

/* Get the configured remote peer of a UDP socket. */
ASX_API asx_status asx_udp_peer_addr(asx_udp_socket socket, asx_socket_addr *out);

/* Check if a UDP socket is alive. */
ASX_API int asx_udp_is_alive(asx_udp_socket socket);

/* -------------------------------------------------------------------
 * Resolve / happy-eyeballs helpers
 * ------------------------------------------------------------------- */

/* Initialize resolve options to a deterministic dual-stack localhost-friendly default. */
ASX_API void asx_resolve_options_init(asx_resolve_options *out, uint16_t port);

/* Resolve a host string into deterministic ghost endpoints. */
ASX_API ASX_MUST_USE asx_status asx_resolve_host(asx_resolve_result *out, const char *host,
                                                 const asx_resolve_options *options);

/* Deterministically reorder resolved endpoints using a happy-eyeballs style family preference. */
ASX_API ASX_MUST_USE asx_status asx_happy_eyeballs_order(asx_resolve_result *out,
                                                         const asx_resolve_result *in,
                                                         asx_addr_family preferred_family);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_net_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_NET_H */
