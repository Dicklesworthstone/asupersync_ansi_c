/*
 * test_tls.c — unit tests for deterministic TLS surface
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/tls.h>
#include <asx/runtime/browser_boundary.h>
#include <string.h>

#if ASX_HAS_TLS_SURFACE
static int tls_surface_available(void) { return asx_surface_available_active(ASX_SURFACE_TLS); }

/* A configuration that selects the plaintext null provider on purpose:
 * the passthrough is for tests like these and is never the default. */
static asx_status null_provider_config(asx_tls_config *cfg) {
    asx_tls_config_init(cfg);
    return asx_tls_config_set_provider(cfg, asx_tls_null_provider());
}

TEST(tls_config_defaults) {
    asx_tls_config cfg;
    asx_tls_config_init(&cfg);
    ASSERT_TRUE(cfg.provider == NULL);
    ASSERT_EQ(cfg.min_version, ASX_TLS_VERSION_1_2);
    ASSERT_EQ(cfg.max_version, ASX_TLS_VERSION_1_3);
    ASSERT_EQ(cfg.verify_mode, ASX_TLS_VERIFY_PEER);
    ASSERT_EQ(cfg.root_source, ASX_TLS_ROOT_SOURCE_NONE);
    ASSERT_EQ(cfg.alpn_count, 0u);
}

TEST(tls_root_source_strings) {
    ASSERT_STR_EQ(asx_tls_root_source_str(ASX_TLS_ROOT_SOURCE_NONE), "none");
    ASSERT_STR_EQ(asx_tls_root_source_str(ASX_TLS_ROOT_SOURCE_NATIVE), "native");
    ASSERT_STR_EQ(asx_tls_root_source_str(ASX_TLS_ROOT_SOURCE_WEBPKI), "webpki");
    ASSERT_STR_EQ(asx_tls_root_source_str((asx_tls_root_source)99), "unknown");
}

TEST(tls_config_root_source_variants) {
    asx_tls_config cfg;

    asx_tls_config_init(&cfg);
    ASSERT_EQ(asx_tls_config_set_root_source(&cfg, ASX_TLS_ROOT_SOURCE_NONE), ASX_OK);
    ASSERT_EQ(cfg.root_source, ASX_TLS_ROOT_SOURCE_NONE);
    ASSERT_EQ(asx_tls_config_set_root_source(&cfg, ASX_TLS_ROOT_SOURCE_NATIVE),
              ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(cfg.root_source, ASX_TLS_ROOT_SOURCE_NONE);
    ASSERT_EQ(asx_tls_config_set_root_source(&cfg, ASX_TLS_ROOT_SOURCE_WEBPKI),
              ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(cfg.root_source, ASX_TLS_ROOT_SOURCE_NONE);
    ASSERT_EQ(asx_tls_config_set_root_source(&cfg, (asx_tls_root_source)77),
              ASX_E_INVALID_ARGUMENT);
}

TEST(tls_config_set_sni) {
    asx_tls_config cfg;
    asx_tls_config_init(&cfg);
    ASSERT_EQ(asx_tls_config_set_sni(&cfg, "example.com"), ASX_OK);
    ASSERT_STR_EQ(cfg.sni_hostname, "example.com");
}

TEST(tls_config_add_alpn) {
    asx_tls_config cfg;
    asx_tls_config_init(&cfg);
    ASSERT_EQ(asx_tls_config_add_alpn(&cfg, "h2"), ASX_OK);
    ASSERT_EQ(asx_tls_config_add_alpn(&cfg, "http/1.1"), ASX_OK);
    ASSERT_EQ(cfg.alpn_count, 2u);
    ASSERT_STR_EQ(cfg.alpn_protocols[0], "h2");
    ASSERT_STR_EQ(cfg.alpn_protocols[1], "http/1.1");
}

TEST(tls_config_alpn_exhaustion) {
    asx_tls_config cfg;
    uint32_t i;
    asx_tls_config_init(&cfg);
    for (i = 0; i < ASX_TLS_MAX_ALPN; i++) {
        ASSERT_EQ(asx_tls_config_add_alpn(&cfg, "proto"), ASX_OK);
    }
    ASSERT_EQ(asx_tls_config_add_alpn(&cfg, "extra"), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(tls_connect_and_state) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    asx_tcp_listener lis;
    asx_tcp_stream tcp_client, tcp_server;
    asx_socket_addr addr = asx_socket_addr_loopback(12000);

    asx_net_reset();
    asx_tls_reset();
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);

    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp_client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &tcp_server, NULL), ASX_OK);

    if (!tls_surface_available()) {
        ASSERT_EQ(asx_tls_connect(&tls, &conn, tcp_client), ASX_E_PERMISSION_DENIED);
        return;
    }

    ASSERT_EQ(asx_tls_connect(&tls, &conn, tcp_client), ASX_OK);
    ASSERT_TRUE(asx_tls_stream_is_alive(tls));
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_READY);

    asx_tls_stream_close(tls);
    ASSERT_FALSE(asx_tls_stream_is_alive(tls));

    asx_tcp_stream_close(tcp_client);
    asx_tcp_stream_close(tcp_server);
    asx_tcp_listener_close(lis);
}

TEST(tls_accept_and_io) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_acceptor acc;
    asx_tls_stream tls_client, tls_server;
    asx_tcp_listener lis;
    asx_tcp_stream tcp_client, tcp_server;
    asx_socket_addr addr = asx_socket_addr_loopback(12001);
    uint8_t data[] = "tls-data";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_net_reset();
    asx_tls_reset();
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    asx_tls_acceptor_init(&acc, &cfg);

    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp_client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &tcp_server, NULL), ASX_OK);

    if (!tls_surface_available()) {
        ASSERT_EQ(asx_tls_connect(&tls_client, &conn, tcp_client), ASX_E_PERMISSION_DENIED);
        ASSERT_EQ(asx_tls_accept(&tls_server, &acc, tcp_server), ASX_E_PERMISSION_DENIED);
        return;
    }

    ASSERT_EQ(asx_tls_connect(&tls_client, &conn, tcp_client), ASX_OK);
    ASSERT_EQ(asx_tls_accept(&tls_server, &acc, tcp_server), ASX_OK);

    src = asx_buf_from(data, 8);
    ASSERT_EQ(asx_tls_stream_poll_write(tls_client, &src, &written), ASX_OK);
    ASSERT_EQ(written, 8u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_tls_stream_poll_read(tls_server, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 8u);
    ASSERT_TRUE(memcmp(dst.data, "tls-data", 8) == 0);

    asx_tls_stream_close(tls_client);
    asx_tls_stream_close(tls_server);
    asx_tcp_stream_close(tcp_client);
    asx_tcp_stream_close(tcp_server);
    asx_tcp_listener_close(lis);
}

TEST(tls_alpn_negotiation) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    asx_tcp_listener lis;
    asx_tcp_stream tcp_client, tcp_server;
    asx_socket_addr addr = asx_socket_addr_loopback(12002);
    char alpn_buf[32];

    asx_net_reset();
    asx_tls_reset();
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_config_add_alpn(&cfg, "h2");
    asx_tls_connector_init(&conn, &cfg);

    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp_client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &tcp_server, NULL), ASX_OK);

    if (!tls_surface_available()) {
        ASSERT_EQ(asx_tls_connect(&tls, &conn, tcp_client), ASX_E_PERMISSION_DENIED);
        return;
    }

    ASSERT_EQ(asx_tls_connect(&tls, &conn, tcp_client), ASX_OK);
    ASSERT_EQ(asx_tls_stream_alpn(tls, alpn_buf, sizeof(alpn_buf)), ASX_OK);
    ASSERT_STR_EQ(alpn_buf, "h2");

    asx_tls_stream_close(tls);
    asx_tcp_stream_close(tcp_client);
    asx_tcp_stream_close(tcp_server);
    asx_tcp_listener_close(lis);
}

TEST(tls_shutdown_transitions) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    asx_tcp_listener lis;
    asx_tcp_stream tcp_client, tcp_server;
    asx_socket_addr addr = asx_socket_addr_loopback(12003);

    asx_net_reset();
    asx_tls_reset();
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);

    ASSERT_EQ(asx_tcp_listener_bind(&lis, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_connect(&tcp_client, &addr), ASX_OK);
    ASSERT_EQ(asx_tcp_listener_poll_accept(lis, &tcp_server, NULL), ASX_OK);
    if (!tls_surface_available()) {
        ASSERT_EQ(asx_tls_connect(&tls, &conn, tcp_client), ASX_E_PERMISSION_DENIED);
        return;
    }
    ASSERT_EQ(asx_tls_connect(&tls, &conn, tcp_client), ASX_OK);

    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_READY);
    ASSERT_EQ(asx_tls_stream_shutdown(tls), ASX_OK);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_SHUTDOWN);
    /* Can't shutdown again from SHUTDOWN state */
    ASSERT_EQ(asx_tls_stream_shutdown(tls), ASX_E_INVALID_STATE);

    asx_tls_stream_close(tls);
    asx_tcp_stream_close(tcp_client);
    asx_tcp_stream_close(tcp_server);
    asx_tcp_listener_close(lis);
}

/* ------------------------------------------------------------------ */
/* Providers (bd-9kll.17.2)                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_tcp_listener lis;
    asx_tcp_stream client;
    asx_tcp_stream server;
} tls_pair;

static int tls_pair_open(tls_pair *p, uint16_t port) {
    asx_socket_addr addr = asx_socket_addr_loopback(port);

    asx_net_reset();
    asx_tls_reset();
    if (asx_tcp_listener_bind(&p->lis, &addr) != ASX_OK) return 0;
    if (asx_tcp_connect(&p->client, &addr) != ASX_OK) return 0;
    if (asx_tcp_listener_poll_accept(p->lis, &p->server, NULL) != ASX_OK) return 0;
    return 1;
}

static void tls_pair_close(tls_pair *p) {
    (void)asx_tcp_stream_close(p->client);
    (void)asx_tcp_stream_close(p->server);
    (void)asx_tcp_listener_close(p->lis);
}

/* With the TLS surface gated off, every connect says so; the tests below
 * then have nothing else to assert. */
#define TLS_REQUIRE_SURFACE(conn, tls, pair)                                                       \
    do {                                                                                           \
        if (!tls_surface_available()) {                                                            \
            ASSERT_EQ(asx_tls_connect(&(tls), &(conn), (pair).client), ASX_E_PERMISSION_DENIED);   \
            tls_pair_close(&(pair));                                                               \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* A toy provider. Its "cipher" flips every byte, so the wire never carries
 * the plaintext; its handshake needs several steps; its shutdown flushes in
 * two. It records what the core asked of it. File-scope so that no stream
 * left open by a failed assertion outlives its provider. */
typedef struct {
    uint32_t opens;
    uint32_t handshakes;
    uint32_t handshake_steps_needed;
    uint32_t shutdowns;
    uint32_t closes;
    asx_status open_failure;
    asx_status handshake_failure;
} toy_tls;

static toy_tls g_toy;

static asx_status toy_open(void *ctx, asx_tls_session *session) {
    toy_tls *t = (toy_tls *)ctx;
    t->opens++;
    if (t->open_failure != ASX_OK) return t->open_failure;
    session->provider_state = t;
    memcpy(session->negotiated_alpn, "toy", 4u);
    return ASX_OK;
}

static asx_status toy_handshake(void *ctx, asx_tls_session *session) {
    toy_tls *t = (toy_tls *)ctx;
    (void)session;
    t->handshakes++;
    if (t->handshake_failure != ASX_OK) return t->handshake_failure;
    return t->handshakes >= t->handshake_steps_needed ? ASX_OK : ASX_E_PENDING;
}

static asx_status toy_write(void *ctx, asx_tls_session *session, const asx_buf *src,
                            uint32_t *bytes_written) {
    uint8_t wire[64];
    asx_buf out;
    uint32_t i;
    (void)ctx;
    if (src->len > sizeof(wire)) return ASX_E_BUFFER_TOO_SMALL;
    for (i = 0u; i < src->len; i++) wire[i] = (uint8_t)(src->ptr[i] ^ 0xFFu);
    out = asx_buf_from(wire, src->len);
    return asx_tcp_stream_poll_write(session->tcp, &out, bytes_written);
}

static asx_status toy_read(void *ctx, asx_tls_session *session, asx_buf_mut *dst,
                           uint32_t *bytes_read) {
    uint32_t start = dst->wr_pos;
    uint32_t i;
    asx_status st = asx_tcp_stream_poll_read(session->tcp, dst, bytes_read);
    (void)ctx;
    if (st != ASX_OK) return st;
    for (i = start; i < dst->wr_pos; i++) dst->data[i] = (uint8_t)(dst->data[i] ^ 0xFFu);
    return ASX_OK;
}

static asx_status toy_shutdown(void *ctx, asx_tls_session *session) {
    toy_tls *t = (toy_tls *)ctx;
    (void)session;
    t->shutdowns++;
    return t->shutdowns >= 2u ? ASX_OK : ASX_E_PENDING;
}

static void toy_close(void *ctx, asx_tls_session *session) {
    toy_tls *t = (toy_tls *)ctx;
    (void)session;
    t->closes++;
}

static asx_status toy_peer_cert(void *ctx, asx_tls_session *session, uint8_t *out,
                                uint32_t capacity, uint32_t *out_len) {
    static const uint8_t cert[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    (void)ctx;
    (void)session;
    if (capacity < sizeof(cert)) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(out, cert, sizeof(cert));
    *out_len = (uint32_t)sizeof(cert);
    return ASX_OK;
}

static const asx_tls_provider g_toy_provider = {"toy",         &g_toy,    toy_open,
                                                toy_handshake, toy_read,  toy_write,
                                                toy_shutdown,  toy_close, toy_peer_cert};

static asx_status toy_config(asx_tls_config *cfg, uint32_t handshake_steps_needed) {
    memset(&g_toy, 0, sizeof(g_toy));
    g_toy.handshake_steps_needed = handshake_steps_needed;
    asx_tls_config_init(cfg);
    return asx_tls_config_set_provider(cfg, &g_toy_provider);
}

TEST(tls_connect_and_accept_refuse_without_provider) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_acceptor acc;
    asx_tls_stream tls;
    asx_tls_stream probe;
    tls_pair p;
    uint32_t i;

    ASSERT_TRUE(tls_pair_open(&p, 12010u));
    asx_tls_config_init(&cfg); /* names no provider */
    asx_tls_connector_init(&conn, &cfg);
    asx_tls_acceptor_init(&acc, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);

    /* TLS selected, no provider: refused, not served as plaintext. */
    tls.slot = 0xFFFFFFFFu;
    tls.generation = 0u;
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_E_HOOK_MISSING);
    ASSERT_EQ(asx_tls_accept(&tls, &acc, p.server), ASX_E_HOOK_MISSING);
    ASSERT_EQ(tls.slot, 0xFFFFFFFFu); /* the handle was not written */

    /* Nothing was opened, and refusals consume no stream slot: more of
     * them than there are slots, then a stream still opens. */
    probe.slot = 0u;
    probe.generation = 1u;
    ASSERT_FALSE(asx_tls_stream_is_alive(probe));
    for (i = 0u; i <= ASX_MAX_TLS_STREAMS; i++) {
        ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_E_HOOK_MISSING);
    }
    ASSERT_EQ(asx_tls_config_set_provider(&cfg, asx_tls_null_provider()), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_OK);
    ASSERT_EQ(asx_tls_stream_close(tls), ASX_OK);
    tls_pair_close(&p);
}

TEST(tls_set_provider_validates_and_clears) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    asx_tls_provider incomplete;
    tls_pair p;

    ASSERT_TRUE(tls_pair_open(&p, 12011u));
    asx_tls_config_init(&cfg);
    memset(&incomplete, 0, sizeof(incomplete));
    incomplete.name = "incomplete";
    incomplete.handshake_fn = toy_handshake; /* read, write, shutdown missing */

    ASSERT_EQ(asx_tls_config_set_provider(NULL, asx_tls_null_provider()), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tls_config_set_provider(&cfg, &incomplete), ASX_E_HOOK_INVALID);
    ASSERT_TRUE(cfg.provider == NULL); /* unchanged */
    ASSERT_EQ(asx_tls_config_set_provider(&cfg, asx_tls_null_provider()), ASX_OK);
    ASSERT_TRUE(cfg.provider == asx_tls_null_provider());
    ASSERT_EQ(asx_tls_config_set_provider(&cfg, NULL), ASX_OK);
    ASSERT_TRUE(cfg.provider == NULL);

    /* A table put straight into the field is still checked at connect. */
    cfg.provider = &incomplete;
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_E_HOOK_INVALID);
    tls_pair_close(&p);
}

TEST(tls_null_provider_is_named_test_only_and_never_default) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    tls_pair p;

    ASSERT_TRUE(asx_tls_null_provider() != NULL);
    ASSERT_TRUE(strstr(asx_tls_null_provider()->name, "test-only") != NULL);
    ASSERT_TRUE(strstr(asx_tls_null_provider()->name, "plaintext") != NULL);

    /* No path installs it for a caller: a fresh configuration and a
     * connector built from no configuration both name no provider. */
    asx_tls_config_init(&cfg);
    ASSERT_TRUE(cfg.provider == NULL);
    asx_tls_connector_init(&conn, NULL);
    ASSERT_TRUE(conn.config.provider == NULL);

    ASSERT_TRUE(tls_pair_open(&p, 12012u));
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_OK);
    ASSERT_TRUE(asx_tls_stream_provider(tls) == asx_tls_null_provider());
    ASSERT_EQ(asx_tls_stream_close(tls), ASX_OK);
    ASSERT_TRUE(asx_tls_stream_provider(tls) == NULL);
    tls_pair_close(&p);
}

TEST(tls_null_provider_puts_plaintext_on_the_wire) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    tls_pair p;
    asx_buf src = asx_buf_from("not-a-secret", 12u);
    asx_buf_mut raw;
    uint32_t written = 0u;
    uint32_t n = 0u;
    uint8_t cert[8];
    uint32_t cert_len = 0u;

    ASSERT_TRUE(tls_pair_open(&p, 12013u));
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_OK);
    ASSERT_EQ(asx_tls_stream_poll_write(tls, &src, &written), ASX_OK);
    ASSERT_EQ(written, 12u);

    /* This is the property that makes the provider test-only: what the
     * TCP peer reads is exactly what was written "through TLS". */
    asx_buf_mut_init(&raw);
    ASSERT_EQ(asx_tcp_stream_poll_read(p.server, &raw, &n), ASX_OK);
    ASSERT_EQ(n, 12u);
    ASSERT_TRUE(memcmp(raw.data, "not-a-secret", 12u) == 0);

    /* It authenticates no peer, so it has no certificate to report. */
    ASSERT_EQ(asx_tls_stream_peer_certificate(tls, cert, sizeof(cert), &cert_len), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_tls_stream_close(tls), ASX_OK);
    tls_pair_close(&p);
}

TEST(tls_provider_drives_handshake_and_record_layer) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    tls_pair p;
    asx_buf src = asx_buf_from("hello", 5u);
    asx_buf_mut raw;
    asx_buf_mut dec;
    uint8_t cert[8];
    uint32_t cert_len = 0u;
    char alpn[16];
    uint32_t written = 0u;
    uint32_t n = 0u;
    uint32_t i;

    ASSERT_TRUE(tls_pair_open(&p, 12014u));
    ASSERT_EQ(toy_config(&cfg, 4u), ASX_OK);
    ASSERT_EQ(asx_tls_config_add_alpn(&cfg, "h2"), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);

    /* The first step runs at connect and needs more I/O. */
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_OK);
    ASSERT_EQ(g_toy.opens, 1u);
    ASSERT_EQ(g_toy.handshakes, 1u);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_HANDSHAKE);
    ASSERT_TRUE(asx_tls_stream_provider(tls) == &g_toy_provider);
    /* No certificate before the handshake is done. */
    ASSERT_EQ(asx_tls_stream_peer_certificate(tls, cert, sizeof(cert), &cert_len),
              ASX_E_INVALID_STATE);

    /* A write and a read each drive one step and are told to wait. */
    ASSERT_EQ(asx_tls_stream_poll_write(tls, &src, &written), ASX_E_PENDING);
    ASSERT_EQ(g_toy.handshakes, 2u);
    asx_buf_mut_init(&dec);
    ASSERT_EQ(asx_tls_stream_poll_read(tls, &dec, &n), ASX_E_PENDING);
    ASSERT_EQ(g_toy.handshakes, 3u);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_HANDSHAKE);

    /* The explicit step completes it; once READY there is nothing left. */
    ASSERT_EQ(asx_tls_stream_poll_handshake(tls), ASX_OK);
    ASSERT_EQ(g_toy.handshakes, 4u);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_READY);
    ASSERT_EQ(asx_tls_stream_poll_handshake(tls), ASX_OK);
    ASSERT_EQ(g_toy.handshakes, 4u);

    /* The provider's record layer: the peer sees flipped bytes. */
    ASSERT_EQ(asx_tls_stream_poll_write(tls, &src, &written), ASX_OK);
    ASSERT_EQ(written, 5u);
    asx_buf_mut_init(&raw);
    ASSERT_EQ(asx_tcp_stream_poll_read(p.server, &raw, &n), ASX_OK);
    ASSERT_EQ(n, 5u);
    ASSERT_TRUE(memcmp(raw.data, "hello", 5u) != 0);
    for (i = 0u; i < 5u; i++) ASSERT_EQ(raw.data[i], (uint8_t)("hello"[i] ^ 0xFF));

    /* ALPN is the provider's to fill, whatever was offered. */
    ASSERT_EQ(asx_tls_stream_alpn(tls, alpn, sizeof(alpn)), ASX_OK);
    ASSERT_STR_EQ(alpn, "toy");

    /* The peer certificate comes from the provider, bounded by capacity. */
    ASSERT_EQ(asx_tls_stream_peer_certificate(tls, cert, 2u, &cert_len), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_tls_stream_peer_certificate(tls, cert, sizeof(cert), &cert_len), ASX_OK);
    ASSERT_EQ(cert_len, 4u);
    ASSERT_EQ(cert[0], 0xDEu);
    ASSERT_EQ(cert[3], 0xEFu);
    ASSERT_EQ(asx_tls_stream_peer_certificate(tls, cert, sizeof(cert), NULL),
              ASX_E_INVALID_ARGUMENT);

    /* close_notify still flushing keeps the stream READY. */
    ASSERT_EQ(asx_tls_stream_shutdown(tls), ASX_E_PENDING);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_READY);
    ASSERT_EQ(asx_tls_stream_shutdown(tls), ASX_OK);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_SHUTDOWN);

    /* Closing releases the provider's session exactly once. */
    ASSERT_EQ(g_toy.closes, 0u);
    ASSERT_EQ(asx_tls_stream_close(tls), ASX_OK);
    ASSERT_EQ(g_toy.closes, 1u);
    ASSERT_FALSE(asx_tls_stream_is_alive(tls));
    tls_pair_close(&p);
}

TEST(tls_provider_failures_leave_no_stream_open) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    asx_tls_stream probe;
    asx_buf src = asx_buf_from("x", 1u);
    asx_buf_mut dec;
    uint32_t written = 0u;
    uint32_t n = 0u;
    tls_pair p;

    ASSERT_TRUE(tls_pair_open(&p, 12015u));
    probe.slot = 0u;
    probe.generation = 1u;
    ASSERT_EQ(toy_config(&cfg, 1u), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);

    /* open_fn fails: no session was established, so none is closed. */
    g_toy.open_failure = ASX_E_RESOURCE_EXHAUSTED;
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(g_toy.closes, 0u);
    ASSERT_FALSE(asx_tls_stream_is_alive(probe));

    /* The first handshake step fails: the opened session is closed. */
    g_toy.open_failure = ASX_OK;
    g_toy.handshake_failure = ASX_E_PERMISSION_DENIED;
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(g_toy.closes, 1u);
    ASSERT_FALSE(asx_tls_stream_is_alive(probe));

    /* A later step fails: the stream is in ERROR, refuses data, and is
     * still ours to close. */
    g_toy.handshake_failure = ASX_OK;
    g_toy.handshakes = 0u;
    g_toy.handshake_steps_needed = 3u;
    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_OK);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_HANDSHAKE);
    g_toy.handshake_failure = ASX_E_DISCONNECTED;
    ASSERT_EQ(asx_tls_stream_poll_handshake(tls), ASX_E_DISCONNECTED);
    ASSERT_EQ(asx_tls_stream_state(tls), ASX_TLS_STATE_ERROR);
    ASSERT_EQ(asx_tls_stream_poll_write(tls, &src, &written), ASX_E_INVALID_STATE);
    asx_buf_mut_init(&dec);
    ASSERT_EQ(asx_tls_stream_poll_read(tls, &dec, &n), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_tls_stream_poll_handshake(tls), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_tls_stream_close(tls), ASX_OK);
    ASSERT_EQ(g_toy.closes, 2u);
    tls_pair_close(&p);
}

TEST(tls_stale_handle_does_not_match_a_reused_slot) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream first;
    asx_tls_stream second;
    tls_pair p;

    ASSERT_TRUE(tls_pair_open(&p, 12016u));
    ASSERT_EQ(null_provider_config(&cfg), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, first, p);

    ASSERT_EQ(asx_tls_connect(&first, &conn, p.client), ASX_OK);
    ASSERT_EQ(asx_tls_stream_close(first), ASX_OK);
    ASSERT_EQ(asx_tls_connect(&second, &conn, p.client), ASX_OK);

    /* The first free slot is reused; its generation must have moved on, or
     * the closed stream's handle would address the new stream. */
    ASSERT_EQ(second.slot, first.slot);
    ASSERT_TRUE(second.generation != first.generation);
    ASSERT_FALSE(asx_tls_stream_is_alive(first));
    ASSERT_TRUE(asx_tls_stream_is_alive(second));
    ASSERT_EQ(asx_tls_stream_close(first), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tls_stream_close(second), ASX_OK);
    tls_pair_close(&p);
}

TEST(tls_reset_releases_open_sessions_through_the_provider) {
    asx_tls_config cfg;
    asx_tls_connector conn;
    asx_tls_stream tls;
    tls_pair p;

    ASSERT_TRUE(tls_pair_open(&p, 12017u));
    ASSERT_EQ(toy_config(&cfg, 1u), ASX_OK);
    asx_tls_connector_init(&conn, &cfg);
    TLS_REQUIRE_SURFACE(conn, tls, p);

    ASSERT_EQ(asx_tls_connect(&tls, &conn, p.client), ASX_OK);
    ASSERT_EQ(g_toy.closes, 0u);
    asx_tls_reset();
    ASSERT_EQ(g_toy.closes, 1u);
    ASSERT_FALSE(asx_tls_stream_is_alive(tls));
    tls_pair_close(&p);
}

TEST(tls_null_args) {
    asx_tls_config cfg;
    asx_tls_config_init(&cfg);
    ASSERT_EQ(asx_tls_config_set_sni(NULL, "host"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tls_config_set_sni(&cfg, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tls_config_set_root_source(NULL, ASX_TLS_ROOT_SOURCE_NONE),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_tls_config_add_alpn(NULL, "h2"), ASX_E_INVALID_ARGUMENT);
}
#else
TEST(tls_surface_compile_time_hidden_in_browser) {
    ASSERT_EQ(ASX_HAS_TLS_SURFACE, 0);
    ASSERT_EQ(asx_surface_available_active(ASX_SURFACE_TLS), 0);
    ASSERT_EQ((int)asx_surface_gate(ASX_SURFACE_TLS), (int)ASX_E_PERMISSION_DENIED);
}
#endif

int main(void) {
    fprintf(stderr, "=== tls tests ===\n");
#if ASX_HAS_TLS_SURFACE
    RUN_TEST(tls_config_defaults);
    RUN_TEST(tls_root_source_strings);
    RUN_TEST(tls_config_root_source_variants);
    RUN_TEST(tls_config_set_sni);
    RUN_TEST(tls_config_add_alpn);
    RUN_TEST(tls_config_alpn_exhaustion);
    RUN_TEST(tls_connect_and_state);
    RUN_TEST(tls_accept_and_io);
    RUN_TEST(tls_alpn_negotiation);
    RUN_TEST(tls_shutdown_transitions);
    RUN_TEST(tls_connect_and_accept_refuse_without_provider);
    RUN_TEST(tls_set_provider_validates_and_clears);
    RUN_TEST(tls_null_provider_is_named_test_only_and_never_default);
    RUN_TEST(tls_null_provider_puts_plaintext_on_the_wire);
    RUN_TEST(tls_provider_drives_handshake_and_record_layer);
    RUN_TEST(tls_provider_failures_leave_no_stream_open);
    RUN_TEST(tls_stale_handle_does_not_match_a_reused_slot);
    RUN_TEST(tls_reset_releases_open_sessions_through_the_provider);
    RUN_TEST(tls_null_args);
#else
    RUN_TEST(tls_surface_compile_time_hidden_in_browser);
#endif
    TEST_REPORT();
    return test_failures;
}
