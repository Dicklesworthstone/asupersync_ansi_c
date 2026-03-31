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

TEST(tls_config_defaults) {
    asx_tls_config cfg;
    asx_tls_config_init(&cfg);
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
    asx_tls_config_init(&cfg);
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
    asx_tls_config_init(&cfg);
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
    asx_tls_config_init(&cfg);
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
    asx_tls_config_init(&cfg);
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
    RUN_TEST(tls_null_args);
#else
    RUN_TEST(tls_surface_compile_time_hidden_in_browser);
#endif
    TEST_REPORT();
    return test_failures;
}
