/*
 * tls.c — deterministic TLS connector/acceptor and stream implementation
 *
 * In the deterministic core profile, TLS is a passthrough wrapper that
 * tracks handshake state and configuration but delegates I/O to the
 * underlying TCP stream. Real crypto is deferred to platform adapters.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/tls.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

#if ASX_HAS_TLS_SURFACE
typedef struct {
    asx_tcp_stream tcp;
    asx_tls_config config;
    asx_tls_state state;
    uint32_t generation;
    int alive;
    char negotiated_alpn[ASX_TLS_ALPN_MAX_LEN];
} tls_stream_slot;

static tls_stream_slot g_tls_streams[ASX_MAX_TLS_STREAMS];

static uint32_t tls_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static tls_stream_slot *tls_lookup(asx_tls_stream h) {
    tls_stream_slot *s;

    if (h.slot >= ASX_MAX_TLS_STREAMS) return NULL;
    s = &g_tls_streams[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static size_t tls_bounded_strlen(const char *str, size_t max) {
    size_t len;

    if (str == NULL) return 0u;
    for (len = 0u; len < max; len++) {
        if (str[len] == '\0') return len;
    }
    return max;
}

static asx_status tls_surface_gate(void) { return asx_surface_gate(ASX_SURFACE_TLS); }

/* ------------------------------------------------------------------ */
/* TLS configuration                                                   */
/* ------------------------------------------------------------------ */

void asx_tls_config_init(asx_tls_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->min_version = ASX_TLS_VERSION_1_2;
    cfg->max_version = ASX_TLS_VERSION_1_3;
    cfg->verify_mode = ASX_TLS_VERIFY_PEER;
}

asx_status asx_tls_config_set_sni(asx_tls_config *cfg, const char *hostname) {
    size_t len;

    if (cfg == NULL || hostname == NULL) return ASX_E_INVALID_ARGUMENT;
    len = tls_bounded_strlen(hostname, ASX_TLS_MAX_HOSTNAME);
    if (len == 0u || len >= ASX_TLS_MAX_HOSTNAME) return ASX_E_INVALID_ARGUMENT;
    memcpy(cfg->sni_hostname, hostname, len + 1u);
    return ASX_OK;
}

asx_status asx_tls_config_add_alpn(asx_tls_config *cfg, const char *protocol) {
    size_t len;

    if (cfg == NULL || protocol == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cfg->alpn_count >= ASX_TLS_MAX_ALPN) return ASX_E_RESOURCE_EXHAUSTED;
    len = tls_bounded_strlen(protocol, ASX_TLS_ALPN_MAX_LEN);
    if (len == 0u || len >= ASX_TLS_ALPN_MAX_LEN) return ASX_E_INVALID_ARGUMENT;
    memcpy(cfg->alpn_protocols[cfg->alpn_count], protocol, len + 1u);
    cfg->alpn_count++;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* TLS connector and acceptor                                          */
/* ------------------------------------------------------------------ */

void asx_tls_connector_init(asx_tls_connector *conn, const asx_tls_config *cfg) {
    if (conn == NULL) return;
    memset(conn, 0, sizeof(*conn));
    if (cfg != NULL) {
        conn->config = *cfg;
    } else {
        asx_tls_config_init(&conn->config);
    }
}

void asx_tls_acceptor_init(asx_tls_acceptor *acc, const asx_tls_config *cfg) {
    if (acc == NULL) return;
    memset(acc, 0, sizeof(*acc));
    if (cfg != NULL) {
        acc->config = *cfg;
    } else {
        asx_tls_config_init(&acc->config);
    }
}

/* ------------------------------------------------------------------ */
/* TLS stream API                                                      */
/* ------------------------------------------------------------------ */

static asx_status tls_alloc(asx_tls_stream *out, asx_tcp_stream tcp, const asx_tls_config *cfg) {
    uint32_t idx;
    tls_stream_slot *s;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!asx_tcp_stream_is_alive(tcp)) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_TLS_STREAMS; idx++) {
        if (!g_tls_streams[idx].alive) break;
    }
    if (idx >= ASX_MAX_TLS_STREAMS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_tls_streams[idx];
    memset(s, 0, sizeof(*s));
    s->generation = tls_next_gen(s->generation);
    s->alive = 1;
    s->tcp = tcp;
    s->config = *cfg;
    /* Deterministic mode: handshake succeeds immediately */
    s->state = ASX_TLS_STATE_READY;
    /* Negotiate first ALPN if any */
    if (cfg->alpn_count > 0u) {
        memcpy(s->negotiated_alpn, cfg->alpn_protocols[0],
               tls_bounded_strlen(cfg->alpn_protocols[0], ASX_TLS_ALPN_MAX_LEN) + 1u);
    }

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_tls_connect(asx_tls_stream *out, const asx_tls_connector *conn, asx_tcp_stream tcp) {
    if (conn == NULL) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    return tls_alloc(out, tcp, &conn->config);
}

asx_status asx_tls_accept(asx_tls_stream *out, const asx_tls_acceptor *acc, asx_tcp_stream tcp) {
    if (acc == NULL) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    return tls_alloc(out, tcp, &acc->config);
}

asx_status asx_tls_stream_poll_read(asx_tls_stream stream, asx_buf_mut *dst, uint32_t *bytes_read) {
    tls_stream_slot *s;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    if (!asx_tcp_stream_is_alive(s->tcp)) {
        s->state = ASX_TLS_STATE_ERROR;
        return ASX_E_DISCONNECTED;
    }
    return asx_tcp_stream_poll_read(s->tcp, dst, bytes_read);
}

asx_status asx_tls_stream_poll_write(asx_tls_stream stream, const asx_buf *src,
                                     uint32_t *bytes_written) {
    tls_stream_slot *s;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    if (!asx_tcp_stream_is_alive(s->tcp)) {
        s->state = ASX_TLS_STATE_ERROR;
        return ASX_E_DISCONNECTED;
    }
    return asx_tcp_stream_poll_write(s->tcp, src, bytes_written);
}

asx_tls_state asx_tls_stream_state(asx_tls_stream stream) {
    tls_stream_slot *s = tls_lookup(stream);
    if (s == NULL) return ASX_TLS_STATE_CLOSED;
    return s->state;
}

asx_status asx_tls_stream_alpn(asx_tls_stream stream, char *out, uint32_t out_capacity) {
    tls_stream_slot *s;
    size_t len;

    if (out == NULL || out_capacity == 0u) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    len = tls_bounded_strlen(s->negotiated_alpn, ASX_TLS_ALPN_MAX_LEN);
    if (len + 1u > out_capacity) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(out, s->negotiated_alpn, len + 1u);
    return ASX_OK;
}

asx_status asx_tls_stream_shutdown(asx_tls_stream stream) {
    tls_stream_slot *s;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    s->state = ASX_TLS_STATE_SHUTDOWN;
    return ASX_OK;
}

asx_status asx_tls_stream_close(asx_tls_stream stream) {
    tls_stream_slot *s;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    s->state = ASX_TLS_STATE_CLOSED;
    s->alive = 0;
    return ASX_OK;
}

int asx_tls_stream_is_alive(asx_tls_stream stream) { return tls_lookup(stream) != NULL; }

void asx_tls_reset(void) { memset(g_tls_streams, 0, sizeof(g_tls_streams)); }
#endif
