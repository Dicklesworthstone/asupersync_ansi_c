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
    ASX_SERVER_STATE_IDLE       = 0,
    ASX_SERVER_STATE_LISTENING  = 1,
    ASX_SERVER_STATE_DRAINING   = 2,
    ASX_SERVER_STATE_STOPPED    = 3
} asx_server_state;

#ifndef ASX_SERVER_MAX_CONNECTIONS
#define ASX_SERVER_MAX_CONNECTIONS 16u
#endif

typedef struct {
    uint32_t max_connections;
    uint32_t drain_timeout_ms;
    uint16_t listen_port;
} asx_server_config;

/* Initialize server config to defaults. */
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
    uint32_t next_conn_id;
    asx_server_state state;
    uint8_t listener_bound;
} asx_server;

/* -------------------------------------------------------------------
 * Server API
 * ------------------------------------------------------------------- */

/* Initialize a server instance. */
ASX_API void asx_server_init(asx_server *srv, const asx_server_config *cfg);

/* Start listening for connections. */
ASX_API ASX_MUST_USE asx_status asx_server_listen(asx_server *srv);

/* Poll for and accept one incoming connection. */
ASX_API ASX_MUST_USE asx_status asx_server_poll_accept(asx_server *srv, asx_server_conn *out);

/* Close a specific connection by ID. */
ASX_API asx_status asx_server_close_conn(asx_server *srv, uint32_t conn_id);

/* Initiate graceful shutdown (stop accepting, drain existing). */
ASX_API asx_status asx_server_shutdown(asx_server *srv);

/* Force-close all connections and stop immediately. */
ASX_API asx_status asx_server_stop(asx_server *srv);

/* Get the current server state. */
ASX_API asx_server_state asx_server_get_state(const asx_server *srv);

/* Get the number of active connections. */
ASX_API uint32_t asx_server_active_count(const asx_server *srv);

/* Get the total number of accepted connections. */
ASX_API uint32_t asx_server_total_accepted(const asx_server *srv);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_SERVER_H */
