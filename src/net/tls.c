/*
 * tls.c — TLS connector/acceptor and stream implementation over a provider
 *
 * The core carries no cryptography. Each stream is a session of the
 * provider named by its configuration (asx_tls_provider): the core owns the
 * handles, the state machine and the surface gate, and the provider owns
 * the handshake, the record layer and the peer's certificate. A
 * configuration without a provider is refused. The null provider at the
 * end of this file is a plaintext passthrough for tests and simulations and
 * is selected only by a caller that asks for it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/tls.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

#if ASX_HAS_TLS_SURFACE
typedef struct {
    asx_tls_config config;
    asx_tls_session session; /* session.config points at `config` */
    asx_tls_state state;
    uint32_t generation;
    int alive;
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

/* A provider without its required callbacks cannot carry a session. */
static int tls_provider_complete(const asx_tls_provider *p) {
    return p->handshake_fn != NULL && p->read_fn != NULL && p->write_fn != NULL &&
           p->shutdown_fn != NULL;
}

/* ------------------------------------------------------------------ */
/* TLS configuration                                                   */
/* ------------------------------------------------------------------ */

void asx_tls_config_init(asx_tls_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->min_version = ASX_TLS_VERSION_1_2;
    cfg->max_version = ASX_TLS_VERSION_1_3;
    cfg->verify_mode = ASX_TLS_VERIFY_PEER;
    cfg->root_source = ASX_TLS_ROOT_SOURCE_NONE;
    cfg->provider = NULL;
}

const char *asx_tls_root_source_str(asx_tls_root_source source) {
    switch (source) {
    case ASX_TLS_ROOT_SOURCE_NONE: return "none";
    case ASX_TLS_ROOT_SOURCE_NATIVE: return "native";
    case ASX_TLS_ROOT_SOURCE_WEBPKI: return "webpki";
    default: return "unknown";
    }
}

asx_status asx_tls_config_set_sni(asx_tls_config *cfg, const char *hostname) {
    size_t len;

    if (cfg == NULL || hostname == NULL) return ASX_E_INVALID_ARGUMENT;
    len = tls_bounded_strlen(hostname, ASX_TLS_MAX_HOSTNAME);
    if (len == 0u || len >= ASX_TLS_MAX_HOSTNAME) return ASX_E_INVALID_ARGUMENT;
    memcpy(cfg->sni_hostname, hostname, len + 1u);
    return ASX_OK;
}

asx_status asx_tls_config_set_root_source(asx_tls_config *cfg, asx_tls_root_source source) {
    if (cfg == NULL) return ASX_E_INVALID_ARGUMENT;

    switch (source) {
    case ASX_TLS_ROOT_SOURCE_NONE: cfg->root_source = source; return ASX_OK;
    case ASX_TLS_ROOT_SOURCE_NATIVE:
#if ASX_HAS_TLS_NATIVE_ROOTS
        cfg->root_source = source;
        return ASX_OK;
#else
        return ASX_E_PERMISSION_DENIED;
#endif
    case ASX_TLS_ROOT_SOURCE_WEBPKI:
#if ASX_HAS_TLS_WEBPKI_ROOTS
        cfg->root_source = source;
        return ASX_OK;
#else
        return ASX_E_PERMISSION_DENIED;
#endif
    default: return ASX_E_INVALID_ARGUMENT;
    }
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

asx_status asx_tls_config_set_provider(asx_tls_config *cfg, const asx_tls_provider *provider) {
    if (cfg == NULL) return ASX_E_INVALID_ARGUMENT;
    if (provider != NULL && !tls_provider_complete(provider)) return ASX_E_HOOK_INVALID;
    cfg->provider = provider;
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

/* One step of the handshake. READY on completion, HANDSHAKE while the
 * provider needs more I/O (ASX_E_PENDING), ERROR with the provider's
 * status otherwise. */
static asx_status tls_handshake_step(tls_stream_slot *s) {
    const asx_tls_provider *p = s->config.provider;
    asx_status st = p->handshake_fn(p->ctx, &s->session);

    if (st == ASX_OK) {
        s->state = ASX_TLS_STATE_READY;
        return ASX_OK;
    }
    if (st == ASX_E_PENDING) {
        s->state = ASX_TLS_STATE_HANDSHAKE;
        return ASX_E_PENDING;
    }
    s->state = ASX_TLS_STATE_ERROR;
    return st;
}

/* The handshake of a stream that is still in it: a dead transport ends it. */
static asx_status tls_handshake_poll(tls_stream_slot *s) {
    if (!asx_tcp_stream_is_alive(s->session.tcp)) {
        s->state = ASX_TLS_STATE_ERROR;
        return ASX_E_DISCONNECTED;
    }
    return tls_handshake_step(s);
}

static asx_status tls_open(asx_tls_stream *out, asx_tls_role role, asx_tcp_stream tcp,
                           const asx_tls_config *cfg) {
    const asx_tls_provider *p;
    tls_stream_slot *s;
    uint32_t generation;
    uint32_t idx;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    /* A configuration that selects TLS without a provider is refused before
     * anything is allocated: the stream would otherwise carry plaintext
     * under the name of TLS. */
    p = cfg->provider;
    if (p == NULL) return ASX_E_HOOK_MISSING;
    if (!tls_provider_complete(p)) return ASX_E_HOOK_INVALID;
    if (!asx_tcp_stream_is_alive(tcp)) return ASX_E_INVALID_ARGUMENT;

    for (idx = 0u; idx < ASX_MAX_TLS_STREAMS; idx++) {
        if (!g_tls_streams[idx].alive) break;
    }
    if (idx >= ASX_MAX_TLS_STREAMS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_tls_streams[idx];
    /* The generation outlives the slot's contents: a handle to a closed
     * stream must not match the stream that reuses its slot. */
    generation = tls_next_gen(s->generation);
    memset(s, 0, sizeof(*s));
    s->generation = generation;
    s->alive = 1;
    s->config = *cfg;
    s->session.role = role;
    s->session.tcp = tcp;
    s->session.config = &s->config;
    s->state = ASX_TLS_STATE_HANDSHAKE;

    if (p->open_fn != NULL) {
        st = p->open_fn(p->ctx, &s->session);
        if (st != ASX_OK) {
            s->alive = 0;
            return st;
        }
    }
    st = tls_handshake_step(s);
    if (st != ASX_OK && st != ASX_E_PENDING) {
        if (p->close_fn != NULL) p->close_fn(p->ctx, &s->session);
        s->alive = 0;
        return st;
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
    return tls_open(out, ASX_TLS_ROLE_CLIENT, tcp, &conn->config);
}

asx_status asx_tls_accept(asx_tls_stream *out, const asx_tls_acceptor *acc, asx_tcp_stream tcp) {
    if (acc == NULL) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    return tls_open(out, ASX_TLS_ROLE_SERVER, tcp, &acc->config);
}

asx_status asx_tls_stream_poll_handshake(asx_tls_stream stream) {
    tls_stream_slot *s;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state == ASX_TLS_STATE_READY) return ASX_OK;
    if (s->state != ASX_TLS_STATE_HANDSHAKE) return ASX_E_INVALID_STATE;
    return tls_handshake_poll(s);
}

asx_status asx_tls_stream_poll_read(asx_tls_stream stream, asx_buf_mut *dst, uint32_t *bytes_read) {
    tls_stream_slot *s;
    const asx_tls_provider *p;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state == ASX_TLS_STATE_HANDSHAKE) {
        asx_status st = tls_handshake_poll(s);
        if (st != ASX_OK) return st;
    }
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    if (!asx_tcp_stream_is_alive(s->session.tcp)) {
        s->state = ASX_TLS_STATE_ERROR;
        return ASX_E_DISCONNECTED;
    }
    p = s->config.provider;
    return p->read_fn(p->ctx, &s->session, dst, bytes_read);
}

asx_status asx_tls_stream_poll_write(asx_tls_stream stream, const asx_buf *src,
                                     uint32_t *bytes_written) {
    tls_stream_slot *s;
    const asx_tls_provider *p;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state == ASX_TLS_STATE_HANDSHAKE) {
        asx_status st = tls_handshake_poll(s);
        if (st != ASX_OK) return st;
    }
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    if (!asx_tcp_stream_is_alive(s->session.tcp)) {
        s->state = ASX_TLS_STATE_ERROR;
        return ASX_E_DISCONNECTED;
    }
    p = s->config.provider;
    return p->write_fn(p->ctx, &s->session, src, bytes_written);
}

asx_tls_state asx_tls_stream_state(asx_tls_stream stream) {
    tls_stream_slot *s = tls_lookup(stream);
    if (s == NULL) return ASX_TLS_STATE_CLOSED;
    return s->state;
}

const asx_tls_provider *asx_tls_stream_provider(asx_tls_stream stream) {
    tls_stream_slot *s = tls_lookup(stream);
    if (s == NULL) return NULL;
    return s->config.provider;
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
    len = tls_bounded_strlen(s->session.negotiated_alpn, ASX_TLS_ALPN_MAX_LEN);
    if (len + 1u > out_capacity) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(out, s->session.negotiated_alpn, len + 1u);
    return ASX_OK;
}

asx_status asx_tls_stream_peer_certificate(asx_tls_stream stream, uint8_t *out, uint32_t capacity,
                                           uint32_t *out_len) {
    tls_stream_slot *s;
    const asx_tls_provider *p;

    if (out_len == NULL || (out == NULL && capacity > 0u)) return ASX_E_INVALID_ARGUMENT;
    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    p = s->config.provider;
    if (p->peer_cert_fn == NULL) return ASX_E_NOT_FOUND;
    return p->peer_cert_fn(p->ctx, &s->session, out, capacity, out_len);
}

asx_status asx_tls_stream_shutdown(asx_tls_stream stream) {
    tls_stream_slot *s;
    const asx_tls_provider *p;
    asx_status st;

    {
        asx_status gate = tls_surface_gate();
        if (gate != ASX_OK) return gate;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->state != ASX_TLS_STATE_READY) return ASX_E_INVALID_STATE;
    if (!asx_tcp_stream_is_alive(s->session.tcp)) {
        s->state = ASX_TLS_STATE_ERROR;
        return ASX_E_DISCONNECTED;
    }
    p = s->config.provider;
    st = p->shutdown_fn(p->ctx, &s->session);
    if (st == ASX_OK) {
        s->state = ASX_TLS_STATE_SHUTDOWN;
    } else if (st != ASX_E_PENDING) {
        s->state = ASX_TLS_STATE_ERROR;
    }
    return st;
}

/* Release a live stream's slot through its provider. */
static void tls_release(tls_stream_slot *s) {
    const asx_tls_provider *p = s->config.provider;

    if (p != NULL && p->close_fn != NULL) p->close_fn(p->ctx, &s->session);
    s->state = ASX_TLS_STATE_CLOSED;
    s->alive = 0;
}

asx_status asx_tls_stream_close(asx_tls_stream stream) {
    tls_stream_slot *s;

    {
        asx_status st = tls_surface_gate();
        if (st != ASX_OK) return st;
    }
    s = tls_lookup(stream);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    tls_release(s);
    return ASX_OK;
}

int asx_tls_stream_is_alive(asx_tls_stream stream) { return tls_lookup(stream) != NULL; }

void asx_tls_reset(void) {
    uint32_t i;

    for (i = 0u; i < ASX_MAX_TLS_STREAMS; i++) {
        if (g_tls_streams[i].alive) tls_release(&g_tls_streams[i]);
    }
    memset(g_tls_streams, 0, sizeof(g_tls_streams));
}

/* ------------------------------------------------------------------ */
/* The null provider: plaintext, for tests and simulations only        */
/* ------------------------------------------------------------------ */

/* No handshake, no certificates, no encryption: bytes go to the TCP stream
 * as they are. The first configured ALPN protocol is "negotiated" so that
 * code that reads the result has something to read. */
static asx_status tls_null_open(void *ctx, asx_tls_session *session) {
    const asx_tls_config *cfg = session->config;
    (void)ctx;

    if (cfg->alpn_count > 0u) {
        size_t len = tls_bounded_strlen(cfg->alpn_protocols[0], ASX_TLS_ALPN_MAX_LEN);
        memcpy(session->negotiated_alpn, cfg->alpn_protocols[0], len);
        session->negotiated_alpn[len] = '\0';
    }
    return ASX_OK;
}

static asx_status tls_null_handshake(void *ctx, asx_tls_session *session) {
    (void)ctx;
    (void)session;
    return ASX_OK;
}

/* ASX_ANALYZER_WAIVER("callback signature: a const session would need a function-pointer cast") */
static asx_status tls_null_read(void *ctx, asx_tls_session *session, asx_buf_mut *dst,
                                uint32_t *bytes_read) {
    (void)ctx;
    return asx_tcp_stream_poll_read(session->tcp, dst, bytes_read);
}

/* ASX_ANALYZER_WAIVER("callback signature: a const session would need a function-pointer cast") */
static asx_status tls_null_write(void *ctx, asx_tls_session *session, const asx_buf *src,
                                 uint32_t *bytes_written) {
    (void)ctx;
    return asx_tcp_stream_poll_write(session->tcp, src, bytes_written);
}

static asx_status tls_null_shutdown(void *ctx, asx_tls_session *session) {
    (void)ctx;
    (void)session;
    return ASX_OK;
}

static const asx_tls_provider g_tls_null_provider = {"null (plaintext, test-only)",
                                                     NULL,
                                                     tls_null_open,
                                                     tls_null_handshake,
                                                     tls_null_read,
                                                     tls_null_write,
                                                     tls_null_shutdown,
                                                     NULL,
                                                     NULL};

const asx_tls_provider *asx_tls_null_provider(void) { return &g_tls_null_provider; }
#endif
