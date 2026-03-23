/*
 * server.c — server connection, accept-loop, and graceful shutdown
 *
 * Reusable server infrastructure with bounded connection tracking,
 * graceful shutdown with drain semantics, and connection lifecycle
 * diagnostics.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/server.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

#if ASX_HAS_SERVER_SURFACE
/* ------------------------------------------------------------------ */
/* Server config                                                       */
/* ------------------------------------------------------------------ */

void asx_server_config_init(asx_server_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_connections = ASX_SERVER_MAX_CONNECTIONS;
    cfg->drain_timeout_ms = 5000u;
    cfg->listen_port = 0u;
}

/* ------------------------------------------------------------------ */
/* Server API                                                          */
/* ------------------------------------------------------------------ */

void asx_server_init(asx_server *srv, const asx_server_config *cfg) {
    if (srv == NULL) return;
    memset(srv, 0, sizeof(*srv));
    if (cfg != NULL) {
        srv->config = *cfg;
    } else {
        asx_server_config_init(&srv->config);
    }
    srv->state = ASX_SERVER_STATE_IDLE;
    srv->next_conn_id = 1u;
}

asx_status asx_server_listen(asx_server *srv) {
    asx_socket_addr addr;
    asx_status st;

    if (srv == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_SERVER);
    if (st != ASX_OK) return st;
    if (srv->state != ASX_SERVER_STATE_IDLE) return ASX_E_INVALID_STATE;

    addr = asx_socket_addr_loopback(srv->config.listen_port);
    st = asx_tcp_listener_bind(&srv->listener, &addr);
    if (st != ASX_OK) return st;

    srv->listener_bound = 1u;
    srv->state = ASX_SERVER_STATE_LISTENING;
    return ASX_OK;
}

asx_status asx_server_poll_accept(asx_server *srv, asx_server_conn *out) {
    asx_tcp_stream stream;
    asx_status st;
    uint32_t idx;

    if (srv == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_surface_gate(ASX_SURFACE_SERVER);
    if (st != ASX_OK) return st;
    if (srv->state != ASX_SERVER_STATE_LISTENING) return ASX_E_INVALID_STATE;
    if (srv->active_count >= srv->config.max_connections) return ASX_E_RESOURCE_EXHAUSTED;

    st = asx_tcp_listener_poll_accept(srv->listener, &stream, NULL);
    if (st != ASX_OK) return st;

    for (idx = 0u; idx < ASX_SERVER_MAX_CONNECTIONS; idx++) {
        if (!srv->connections[idx].active) break;
    }
    if (idx >= ASX_SERVER_MAX_CONNECTIONS) {
        asx_tcp_stream_close(stream);
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    srv->connections[idx].stream = stream;
    srv->connections[idx].id = srv->next_conn_id++;
    srv->connections[idx].active = 1u;
    srv->active_count++;
    srv->total_accepted++;

    *out = srv->connections[idx];
    return ASX_OK;
}

asx_status asx_server_close_conn(asx_server *srv, uint32_t conn_id) {
    uint32_t idx;

    if (srv == NULL) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = asx_surface_gate(ASX_SURFACE_SERVER);
        if (st != ASX_OK) return st;
    }

    for (idx = 0u; idx < ASX_SERVER_MAX_CONNECTIONS; idx++) {
        if (srv->connections[idx].active && srv->connections[idx].id == conn_id) {
            asx_tcp_stream_close(srv->connections[idx].stream);
            srv->connections[idx].active = 0u;
            if (srv->active_count > 0u) srv->active_count--;
            return ASX_OK;
        }
    }
    return ASX_E_NOT_FOUND;
}

asx_status asx_server_shutdown(asx_server *srv) {
    if (srv == NULL) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = asx_surface_gate(ASX_SURFACE_SERVER);
        if (st != ASX_OK) return st;
    }
    if (srv->state != ASX_SERVER_STATE_LISTENING) return ASX_E_INVALID_STATE;

    srv->state = ASX_SERVER_STATE_DRAINING;
    if (srv->listener_bound) {
        asx_tcp_listener_close(srv->listener);
        srv->listener_bound = 0u;
    }

    /* In deterministic mode, drain completes immediately if no connections */
    if (srv->active_count == 0u) { srv->state = ASX_SERVER_STATE_STOPPED; }
    return ASX_OK;
}

asx_status asx_server_stop(asx_server *srv) {
    uint32_t idx;

    if (srv == NULL) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = asx_surface_gate(ASX_SURFACE_SERVER);
        if (st != ASX_OK) return st;
    }
    if (srv->listener_bound) {
        asx_tcp_listener_close(srv->listener);
        srv->listener_bound = 0u;
    }
    for (idx = 0u; idx < ASX_SERVER_MAX_CONNECTIONS; idx++) {
        if (srv->connections[idx].active) {
            asx_tcp_stream_close(srv->connections[idx].stream);
            srv->connections[idx].active = 0u;
        }
    }
    srv->active_count = 0u;
    srv->state = ASX_SERVER_STATE_STOPPED;
    return ASX_OK;
}

asx_server_state asx_server_get_state(const asx_server *srv) {
    if (srv == NULL) return ASX_SERVER_STATE_STOPPED;
    return srv->state;
}

uint32_t asx_server_active_count(const asx_server *srv) {
    if (srv == NULL) return 0u;
    return srv->active_count;
}

uint32_t asx_server_total_accepted(const asx_server *srv) {
    if (srv == NULL) return 0u;
    return srv->total_accepted;
}
#endif
