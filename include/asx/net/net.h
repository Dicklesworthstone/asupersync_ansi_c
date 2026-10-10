/*
 * asx/net/net.h — deterministic network types and in-memory socket primitives
 *
 * Portable network surface: address types, TCP/UDP handle types, and poll-based
 * I/O. Platform socket integration is still deferred, but the core now ships a
 * deterministic in-memory loopback transport so tests and higher-level modules
 * can exercise real connection and datagram flows without OS dependencies.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_NET_H
#define ASX_NET_NET_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/cx/cx.h>
#include <asx/runtime/waker.h>
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

#ifndef ASX_RESOLVER_CACHE_CAPACITY
#define ASX_RESOLVER_CACHE_CAPACITY 8u
#endif

#ifndef ASX_RESOLVER_HOST_CAPACITY
#define ASX_RESOLVER_HOST_CAPACITY 256u /* DNS names are at most 253 octets */
#endif

#if (ASX_MAX_TCP_LISTENERS) < 1 || (ASX_MAX_TCP_STREAMS) < 1 || (ASX_MAX_UDP_SOCKETS) < 1
#error "ASX_MAX_TCP_LISTENERS, ASX_MAX_TCP_STREAMS and ASX_MAX_UDP_SOCKETS must be at least 1"
#endif
#if (ASX_RESOLVER_CACHE_CAPACITY) < 1 || (ASX_RESOLVER_HOST_CAPACITY) < 1
#error "ASX_RESOLVER_CACHE_CAPACITY and ASX_RESOLVER_HOST_CAPACITY must be at least 1"
#endif

#ifndef ASX_RESOLVE_MAX_INFLIGHT
#define ASX_RESOLVE_MAX_INFLIGHT 8u /* concurrent native lookups */
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

typedef struct {
    char host[ASX_RESOLVER_HOST_CAPACITY];
    asx_resolve_options options;
    asx_resolve_result result;
    asx_status status;
    uint8_t occupied;
} asx_resolver_cache_entry;

typedef struct {
    asx_resolver_cache_entry entries[ASX_RESOLVER_CACHE_CAPACITY];
    uint32_t next_slot;
} asx_resolver;

/* Caller-owned state of one asynchronous lookup (asx_resolve_poll). */
typedef struct {
    char host[ASX_RESOLVER_HOST_CAPACITY];
    asx_resolve_options options;
    asx_resolve_result result;
    asx_status status;
    uint8_t state;           /* 0 = not started, 1 = in flight, 2 = done */
    uint32_t job_slot;       /* in-flight native lookup */
    uint32_t job_generation; /* guards against a recycled lookup slot */
    asx_waker waker;         /* wakes the polling task on completion */
} asx_resolve_request;

/* -------------------------------------------------------------------
 * Socket backend
 *
 * MEMORY — deterministic in-memory loopback transport (lab/replay): only
 *          connections between sockets of this process exist.
 * NATIVE — real non-blocking OS sockets (POSIX builds). Operations that
 *          would block park the polled task on reactor readiness and
 *          return ASX_E_PENDING; the scheduler wakes it on readiness.
 *
 * Default: NATIVE in non-deterministic POSIX builds, MEMORY otherwise.
 * The backend applies to sockets created afterwards; existing handles keep
 * the backend they were created with. asx_net_reset() restores the
 * default.
 *
 * Shared stream semantics: a read returning ASX_OK with 0 bytes means the
 * peer closed its write side (EOF); writes may be partial (check the
 * written count); ASX_E_DISCONNECTED reports a reset/refused/broken
 * connection.
 * ------------------------------------------------------------------- */

typedef enum { ASX_NET_BACKEND_MEMORY = 0, ASX_NET_BACKEND_NATIVE = 1 } asx_net_backend;

/* Select the backend for sockets created from now on.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for unknown values, or
 * ASX_E_PERMISSION_DENIED when NATIVE is unavailable in this build. */
ASX_API ASX_MUST_USE asx_status asx_net_set_backend(asx_net_backend backend);

/* Report the backend used for newly created sockets. */
ASX_API asx_net_backend asx_net_get_backend(void);

/* -------------------------------------------------------------------
 * TCP listener API
 * ------------------------------------------------------------------- */

/* Bind a TCP listener to the given address. With the NATIVE backend this
 * binds and listens on a real socket (port 0 picks an ephemeral port; read
 * it back with asx_tcp_listener_local_addr). With MEMORY it records the
 * address and accepts in-memory loopback connects. */
ASX_API ASX_MUST_USE asx_status asx_tcp_listener_bind(asx_tcp_listener *out,
                                                      const asx_socket_addr *addr);

/* Bind a TCP listener under explicit communication authority.
 * Fails closed if cx is invalid or lacks ASX_CAP_CHANNEL. */
ASX_API ASX_MUST_USE asx_status asx_tcp_listener_bind_with_cx(asx_tcp_listener *out,
                                                              const asx_socket_addr *addr,
                                                              const asx_cx *cx);

/* Poll for incoming connections.
 * Returns ASX_OK when a loopback connection is queued, otherwise ASX_E_PENDING. */
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
 * Loopback connects against a bound listener become readable/writable in-memory streams. */
ASX_API ASX_MUST_USE asx_status asx_tcp_connect(asx_tcp_stream *out, const asx_socket_addr *addr);

/* Initiate a TCP connection under explicit communication authority.
 * Fails closed if cx is invalid or lacks ASX_CAP_CHANNEL. */
ASX_API ASX_MUST_USE asx_status asx_tcp_connect_with_cx(asx_tcp_stream *out,
                                                        const asx_socket_addr *addr,
                                                        const asx_cx *cx);

/* Poll-read from a TCP stream.
 * Returns ASX_OK when buffered bytes are available, otherwise ASX_E_PENDING. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_read(asx_tcp_stream stream, asx_buf_mut *dst,
                                                         uint32_t *bytes_read);

/* Poll-read under explicit communication authority.
 * Applies a Cx checkpoint before returning the ghost pending result. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_read_with_cx(asx_tcp_stream stream,
                                                                 asx_buf_mut *dst,
                                                                 uint32_t *bytes_read, asx_cx *cx);

/* Poll-write to a TCP stream.
 * Returns ASX_OK when bytes are delivered to the linked peer, otherwise ASX_E_PENDING. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_write(asx_tcp_stream stream, const asx_buf *src,
                                                          uint32_t *bytes_written);

/* Poll-write under explicit communication authority.
 * Applies a Cx checkpoint before returning the ghost pending result. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_poll_write_with_cx(asx_tcp_stream stream,
                                                                  const asx_buf *src,
                                                                  uint32_t *bytes_written,
                                                                  asx_cx *cx);

/* Read up to cap bytes into dst. Returns ASX_OK with *out_read > 0 for
 * data, ASX_OK with *out_read == 0 at EOF, ASX_E_PENDING when no data is
 * available yet (native: the polled task is parked until readable),
 * ASX_E_DISCONNECTED on reset, ASX_E_INVALID_ARGUMENT for bad handles. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_read(asx_tcp_stream stream, uint8_t *dst,
                                                    uint32_t cap, uint32_t *out_read);

/* Write up to len bytes from src. Returns ASX_OK with the number written
 * (possibly fewer than len), ASX_E_PENDING when the socket cannot accept
 * data yet (native: the polled task is parked until writable),
 * ASX_E_DISCONNECTED if the peer is gone. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_write(asx_tcp_stream stream, const uint8_t *src,
                                                     uint32_t len, uint32_t *out_written);

/* Half-close: signal EOF to the peer while keeping the read side open.
 * Returns ASX_OK or ASX_E_INVALID_ARGUMENT for bad handles. */
ASX_API asx_status asx_tcp_stream_shutdown_write(asx_tcp_stream stream);

/* Enable/disable Nagle's algorithm (TCP_NODELAY) on a native stream.
 * No-op returning ASX_OK on the MEMORY backend. */
ASX_API asx_status asx_tcp_stream_set_nodelay(asx_tcp_stream stream, int enabled);

/* Close a TCP stream. */
ASX_API asx_status asx_tcp_stream_close(asx_tcp_stream stream);

/* Get the local address of a stream. */
ASX_API asx_status asx_tcp_stream_local_addr(asx_tcp_stream stream, asx_socket_addr *out);

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
 * With no explicit destination, uses the connected peer. Deterministic loopback delivery is
 * performed when the destination matches a bound local socket. */
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

/* Resolve a host string into endpoints ordered by family preference.
 *
 * "localhost", IPv4 dotted quads and IPv6 literals (RFC 4291 text form,
 * optionally in brackets, including "::" compression and an embedded IPv4
 * tail) resolve deterministically in every build. Other names resolve
 * through the native resolver (getaddrinfo) when the NATIVE backend is
 * active — synchronously, blocking the caller; inside tasks prefer
 * asx_resolve_poll(). Otherwise they fail with ASX_E_NOT_FOUND.
 * Returns ASX_OK, ASX_E_NOT_FOUND, ASX_E_TIMED_OUT (temporary resolver
 * failure), ASX_E_RESOURCE_EXHAUSTED, or ASX_E_INVALID_ARGUMENT. */
ASX_API ASX_MUST_USE asx_status asx_resolve_host(asx_resolve_result *out, const char *host,
                                                 const asx_resolve_options *options);

/* Prepare an asynchronous lookup of `host` (copied into the request).
 * options may be NULL for the defaults.
 * Returns ASX_E_INVALID_ARGUMENT for a NULL/empty host, a host that does
 * not fit ASX_RESOLVER_HOST_CAPACITY, or options allowing no family. */
ASX_API ASX_MUST_USE asx_status asx_resolve_request_init(asx_resolve_request *req, const char *host,
                                                         const asx_resolve_options *options);

/* Drive an asynchronous lookup from task `self`.
 *
 * Literals and "localhost" complete on the first poll. With the NATIVE
 * backend, other names are resolved by getaddrinfo on the blocking pool:
 * the poll parks `self` and returns ASX_E_PENDING, and the pool
 * completion wakes it (falling back to a synchronous lookup when the pool
 * is unavailable). Once done, every poll returns the same final status
 * (see asx_resolve_host) and copies the ordered result to *out when out
 * is not NULL.
 * Returns ASX_E_RESOURCE_EXHAUSTED when ASX_RESOLVE_MAX_INFLIGHT lookups
 * or the waker arena are in use. */
ASX_API ASX_MUST_USE asx_status asx_resolve_poll(asx_resolve_request *req, asx_task_id self,
                                                 asx_resolve_result *out);

/* Abandon an in-flight lookup (e.g. when the polling task is cancelled):
 * the background lookup finishes on its own and its result is discarded.
 * The request ends in the done state with ASX_E_CANCELLED. No effect on
 * requests that are not in flight. */
ASX_API void asx_resolve_request_cancel(asx_resolve_request *req);

/* Initialize or clear a resolver cache. */
ASX_API void asx_resolver_init(asx_resolver *out);
ASX_API void asx_resolver_reset(asx_resolver *resolver);

/* Return the number of occupied resolver cache entries. */
ASX_API uint32_t asx_resolver_cached_count(const asx_resolver *resolver);

/* Resolve through the deterministic cache. `cache_hit` is set to 1 when reused. */
ASX_API ASX_MUST_USE asx_status asx_resolver_lookup(asx_resolver *resolver, const char *host,
                                                    const asx_resolve_options *options,
                                                    asx_resolve_result *out, uint8_t *cache_hit);

/* Remove cached entries for a host. */
ASX_API void asx_resolver_invalidate(asx_resolver *resolver, const char *host);

/* Deterministically reorder resolved endpoints using a happy-eyeballs style family preference. */
ASX_API ASX_MUST_USE asx_status asx_happy_eyeballs_order(asx_resolve_result *out,
                                                         const asx_resolve_result *in,
                                                         asx_addr_family preferred_family);

/* Resolve a host and connect to the first ordered TCP endpoint. */
ASX_API ASX_MUST_USE asx_status asx_tcp_connect_host(asx_tcp_stream *out, asx_resolver *resolver,
                                                     const char *host,
                                                     const asx_resolve_options *options,
                                                     asx_socket_addr *selected_addr,
                                                     uint8_t *cache_hit);

/* Resolve a host and connect a UDP socket to the first ordered endpoint. */
ASX_API ASX_MUST_USE asx_status asx_udp_connect_host(asx_udp_socket socket, asx_resolver *resolver,
                                                     const char *host,
                                                     const asx_resolve_options *options,
                                                     asx_socket_addr *selected_addr,
                                                     uint8_t *cache_hit);

/* -------------------------------------------------------------------
 * Unix-domain socket address
 * ------------------------------------------------------------------- */

#ifndef ASX_UNIX_PATH_MAX
#define ASX_UNIX_PATH_MAX 108u
#endif

typedef struct {
    char path[ASX_UNIX_PATH_MAX];
    uint8_t has_path;
} asx_unix_addr;

/* Construct a Unix-domain address from a path string. */
ASX_API asx_status asx_unix_addr_from_path(asx_unix_addr *out, const char *path);

/* Compare two Unix-domain addresses. */
ASX_API int asx_unix_addr_eq(const asx_unix_addr *a, const asx_unix_addr *b);

/* -------------------------------------------------------------------
 * Unix-domain arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_UNIX_LISTENERS
#define ASX_MAX_UNIX_LISTENERS 4u
#endif

#ifndef ASX_MAX_UNIX_STREAMS
#define ASX_MAX_UNIX_STREAMS 16u
#endif

#ifndef ASX_MAX_UNIX_DGRAM_SOCKETS
#define ASX_MAX_UNIX_DGRAM_SOCKETS 8u
#endif

#if (ASX_MAX_UNIX_LISTENERS) < 1 || (ASX_MAX_UNIX_STREAMS) < 1
#error "ASX_MAX_UNIX_LISTENERS and ASX_MAX_UNIX_STREAMS must be at least 1"
#endif
#if (ASX_MAX_UNIX_DGRAM_SOCKETS) < 1
#error "ASX_MAX_UNIX_DGRAM_SOCKETS must be at least 1"
#endif

/* -------------------------------------------------------------------
 * Unix-domain handle types
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_unix_listener;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_unix_stream;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_unix_dgram;

/* -------------------------------------------------------------------
 * Unix-domain listener API
 * ------------------------------------------------------------------- */

/* Bind a Unix-domain listener to the given path.
 * Deterministic in-memory transport: accepts loopback connects by path match. */
ASX_API ASX_MUST_USE asx_status asx_unix_listener_bind(asx_unix_listener *out,
                                                       const asx_unix_addr *addr);

/* Poll for incoming Unix-domain connections. */
ASX_API ASX_MUST_USE asx_status asx_unix_listener_poll_accept(asx_unix_listener listener,
                                                              asx_unix_stream *out);

/* Close a Unix-domain listener. */
ASX_API asx_status asx_unix_listener_close(asx_unix_listener listener);

/* Get the bound path of a Unix-domain listener. */
ASX_API asx_status asx_unix_listener_local_addr(asx_unix_listener listener, asx_unix_addr *out);

/* Check if a Unix-domain listener is alive. */
ASX_API int asx_unix_listener_is_alive(asx_unix_listener listener);

/* -------------------------------------------------------------------
 * Unix-domain stream API
 * ------------------------------------------------------------------- */

/* Connect to a Unix-domain listener by path.
 * Creates a linked in-memory bidirectional stream pair. */
ASX_API ASX_MUST_USE asx_status asx_unix_connect(asx_unix_stream *out, const asx_unix_addr *addr);

/* Poll-read from a Unix-domain stream. */
ASX_API ASX_MUST_USE asx_status asx_unix_stream_poll_read(asx_unix_stream stream, asx_buf_mut *dst,
                                                          uint32_t *bytes_read);

/* Poll-write to a Unix-domain stream. */
ASX_API ASX_MUST_USE asx_status asx_unix_stream_poll_write(asx_unix_stream stream,
                                                           const asx_buf *src,
                                                           uint32_t *bytes_written);

/* Close a Unix-domain stream. */
ASX_API asx_status asx_unix_stream_close(asx_unix_stream stream);

/* Check if a Unix-domain stream is alive. */
ASX_API int asx_unix_stream_is_alive(asx_unix_stream stream);

/* -------------------------------------------------------------------
 * Unix-domain datagram API
 * ------------------------------------------------------------------- */

/* Bind a Unix-domain datagram socket to the given path. */
ASX_API ASX_MUST_USE asx_status asx_unix_dgram_bind(asx_unix_dgram *out, const asx_unix_addr *addr);

/* Poll-send a datagram to the specified Unix-domain path. */
ASX_API ASX_MUST_USE asx_status asx_unix_dgram_poll_send(asx_unix_dgram socket, const asx_buf *src,
                                                         uint32_t *bytes_written,
                                                         const asx_unix_addr *to);

/* Poll-receive a datagram. */
ASX_API ASX_MUST_USE asx_status asx_unix_dgram_poll_recv(asx_unix_dgram socket, asx_buf_mut *dst,
                                                         uint32_t *bytes_read, asx_unix_addr *from);

/* Close a Unix-domain datagram socket. */
ASX_API asx_status asx_unix_dgram_close(asx_unix_dgram socket);

/* Check if a Unix-domain datagram socket is alive. */
ASX_API int asx_unix_dgram_is_alive(asx_unix_dgram socket);

/* -------------------------------------------------------------------
 * Ancillary data (deterministic fd-passing model)
 *
 * Provides a portable ancillary-data container for passing opaque
 * descriptors alongside stream/datagram I/O. In the deterministic
 * core, descriptors are modeled as uint32_t tokens.
 * ------------------------------------------------------------------- */

#ifndef ASX_ANCILLARY_MAX_FDS
#define ASX_ANCILLARY_MAX_FDS 8u
#endif

typedef struct {
    uint32_t fds[ASX_ANCILLARY_MAX_FDS];
    uint32_t count;
} asx_ancillary;

/* Initialize ancillary data to empty. */
ASX_API void asx_ancillary_init(asx_ancillary *anc);

/* Push a descriptor token. Returns ASX_E_RESOURCE_EXHAUSTED if full. */
ASX_API ASX_MUST_USE asx_status asx_ancillary_push_fd(asx_ancillary *anc, uint32_t fd);

/* Pop the oldest descriptor token. Returns ASX_E_NOT_FOUND if empty. */
ASX_API ASX_MUST_USE asx_status asx_ancillary_pop_fd(asx_ancillary *anc, uint32_t *out);

/* Return the number of descriptor tokens. */
ASX_API uint32_t asx_ancillary_count(const asx_ancillary *anc);

/* Send ancillary data alongside a Unix-domain stream write.
 * Tokens are delivered to the peer's ancillary receive buffer. */
ASX_API ASX_MUST_USE asx_status asx_unix_stream_send_ancillary(asx_unix_stream stream,
                                                               const asx_ancillary *anc);

/* Receive ancillary data from a Unix-domain stream.
 * Drains tokens that were delivered by the peer. */
ASX_API ASX_MUST_USE asx_status asx_unix_stream_recv_ancillary(asx_unix_stream stream,
                                                               asx_ancillary *out);

/* -------------------------------------------------------------------
 * Split-stream API
 *
 * Splits a bidirectional stream into independent read and write halves.
 * Each half holds a reference to the same underlying stream. Closing
 * one half does not close the other; both must be closed to release
 * the underlying stream.
 * ------------------------------------------------------------------- */

typedef enum { ASX_SPLIT_SOURCE_TCP = 0, ASX_SPLIT_SOURCE_UNIX = 1 } asx_split_source_kind;

typedef struct {
    asx_split_source_kind kind;
    uint32_t slot;
    uint32_t generation;
    uint8_t active;
} asx_read_half;

typedef struct {
    asx_split_source_kind kind;
    uint32_t slot;
    uint32_t generation;
    uint8_t active;
} asx_write_half;

/* Split a TCP stream into read and write halves.
 * The original stream handle remains valid but I/O should go through
 * the halves for proper half-close tracking. */
ASX_API ASX_MUST_USE asx_status asx_tcp_stream_split(asx_tcp_stream stream, asx_read_half *rd,
                                                     asx_write_half *wr);

/* Split a Unix-domain stream into read and write halves. */
ASX_API ASX_MUST_USE asx_status asx_unix_stream_split(asx_unix_stream stream, asx_read_half *rd,
                                                      asx_write_half *wr);

/* Poll-read through a read half. */
ASX_API ASX_MUST_USE asx_status asx_read_half_poll_read(asx_read_half *half, asx_buf_mut *dst,
                                                        uint32_t *bytes_read);

/* Poll-write through a write half. */
ASX_API ASX_MUST_USE asx_status asx_write_half_poll_write(asx_write_half *half, const asx_buf *src,
                                                          uint32_t *bytes_written);

/* Close a read half. */
ASX_API asx_status asx_read_half_close(asx_read_half *half);

/* Close a write half. */
ASX_API asx_status asx_write_half_close(asx_write_half *half);

/* Check if a read/write half is active. */
ASX_API int asx_read_half_is_active(const asx_read_half *half);
ASX_API int asx_write_half_is_active(const asx_write_half *half);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all network state including TCP, UDP, and Unix sockets (test support). */
ASX_API void asx_net_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_NET_H */
