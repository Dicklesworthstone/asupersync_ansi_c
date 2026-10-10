/*
 * asx/net/server.h — server connection, accept-loop, and graceful shutdown
 *
 * Provides reusable server infrastructure: connection tracking with bounded
 * capacity, accept-loop dispatch, graceful shutdown with drain deadline,
 * and connection lifecycle diagnostics.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_SERVER_H
#define ASX_NET_SERVER_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/net/net.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if ASX_HAS_SERVER_SURFACE
/* -------------------------------------------------------------------
 * Server configuration and types
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SERVER_STATE_IDLE = 0,
    ASX_SERVER_STATE_LISTENING = 1,
    ASX_SERVER_STATE_DRAINING = 2,
    ASX_SERVER_STATE_STOPPED = 3
} asx_server_state;

#ifndef ASX_SERVER_MAX_CONNECTIONS
#define ASX_SERVER_MAX_CONNECTIONS 16u
#endif

typedef struct {
    uint32_t max_connections; /* at most ASX_SERVER_MAX_CONNECTIONS */
    /* How long a graceful shutdown waits for connections to finish before
     * the remaining ones are closed (asx_server_drain_deadline). */
    uint32_t drain_timeout_ms;
    /* Address to listen on: IPv4 or IPv6, a specific address or the
     * unspecified one (0.0.0.0 / ::) for every interface; port 0 takes an
     * ephemeral port (read it back with asx_tcp_listener_local_addr). */
    asx_socket_addr listen_addr;
} asx_server_config;

/* Initialize server config to defaults: ASX_SERVER_MAX_CONNECTIONS
 * connections, a 30 s drain timeout (Rust's Http1ListenerConfig default),
 * and 127.0.0.1 on an ephemeral port. */
ASX_API void asx_server_config_init(asx_server_config *cfg);

/* -------------------------------------------------------------------
 * Server connection slot
 * ------------------------------------------------------------------- */

typedef struct {
    asx_tcp_stream stream;
    uint32_t id;
    uint8_t active;
} asx_server_conn;

/* -------------------------------------------------------------------
 * Server instance
 * ------------------------------------------------------------------- */

typedef struct {
    asx_server_config config;
    asx_tcp_listener listener;
    asx_server_conn connections[ASX_SERVER_MAX_CONNECTIONS];
    uint32_t active_count;
    uint32_t total_accepted;
    uint32_t total_rejected;
    uint32_t next_conn_id;
    asx_time drain_deadline; /* while DRAINING, if drain_has_deadline */
    asx_server_state state;
    uint8_t listener_bound;
    uint8_t drain_has_deadline;
} asx_server;

/* -------------------------------------------------------------------
 * Server API
 * ------------------------------------------------------------------- */

/* Initialize a server instance. */
ASX_API void asx_server_init(asx_server *srv, const asx_server_config *cfg);

/* Start listening for connections. */
ASX_API ASX_MUST_USE asx_status asx_server_listen(asx_server *srv);

/* Poll for and accept one incoming connection. Returns
 * ASX_E_RESOURCE_EXHAUSTED without accepting at max_connections. */
ASX_API ASX_MUST_USE asx_status asx_server_poll_accept(asx_server *srv, asx_server_conn *out);

/* Accept one pending connection and close it at once: what a server at
 * its connection limit does with an extra client, as Rust's listener drops
 * the stream when its connection manager is full, so the client fails fast
 * instead of waiting in the backlog. Counted in asx_server_total_rejected.
 * Returns ASX_OK if a connection was rejected, the accept's wait status
 * (ASX_E_PENDING / ASX_E_WOULD_BLOCK) if none is pending, or
 * ASX_E_INVALID_STATE unless listening. */
ASX_API ASX_MUST_USE asx_status asx_server_reject_pending(asx_server *srv);

/* Close a specific connection by ID. */
ASX_API asx_status asx_server_close_conn(asx_server *srv, uint32_t conn_id);

/* Initiate graceful shutdown: stop accepting and let the connections
 * finish, until drain_timeout_ms after this call (the drain deadline);
 * whoever drives the connections closes the rest then (asx_http_server_poll
 * does). */
ASX_API asx_status asx_server_shutdown(asx_server *srv);

/* While draining: ASX_OK and the runtime-clock time after which the
 * remaining connections are to be closed. ASX_E_INVALID_STATE when not
 * draining; ASX_E_HOOK_MISSING if the runtime clock was unavailable at
 * shutdown (the drain then has no deadline). */
ASX_API asx_status asx_server_drain_deadline(const asx_server *srv, asx_time *out);

/* Force-close all connections and stop immediately. */
ASX_API asx_status asx_server_stop(asx_server *srv);

/* Get the current server state. */
ASX_API asx_server_state asx_server_get_state(const asx_server *srv);

/* Get the number of active connections. */
ASX_API uint32_t asx_server_active_count(const asx_server *srv);

/* Get the total number of accepted connections. */
ASX_API uint32_t asx_server_total_accepted(const asx_server *srv);

/* Get the number of connections closed at the connection limit
 * (asx_server_reject_pending). */
ASX_API uint32_t asx_server_total_rejected(const asx_server *srv);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_SERVER_H */
