/*
 * ex_network_surface.c — deterministic network surface walkthrough
 *
 * Demonstrates:
 *   1. Building a current-thread runtime
 *   2. Resolving dual-stack localhost endpoints
 *   3. Applying happy-eyeballs ordering
 *   4. Exercising TCP listener/stream and UDP socket ghost contracts
 *
 * Output: SCENARIO lines for smoke-test validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

static void scenario_resolve_and_order(void) {
    SCENARIO_BEGIN("network_surface.resolve_and_order");

    asx_resolve_options opts;
    asx_resolve_result resolved;
    asx_resolve_result ordered;

    asx_resolve_options_init(&opts, 9100u);
    SCENARIO_CHECK(asx_resolve_host(&resolved, "localhost", &opts) == ASX_OK, "resolve_localhost");
    SCENARIO_CHECK(resolved.count >= 2u, "dual_stack_results");
    SCENARIO_CHECK(asx_happy_eyeballs_order(&ordered, &resolved, ASX_AF_INET6) == ASX_OK,
                   "happy_eyeballs");
    SCENARIO_CHECK(ordered.addrs[0].family == ASX_AF_INET6, "ipv6_first");
    SCENARIO_CHECK(ordered.addrs[1].family == ASX_AF_INET4, "ipv4_second");
    SCENARIO_CHECK(asx_resolve_host(&resolved, "not-a-known-host", &opts) == ASX_E_NOT_FOUND,
                   "missing_host");

    SCENARIO_END();
}

static void scenario_runtime_tcp_udp_flow(void) {
    SCENARIO_BEGIN("network_surface.runtime_tcp_udp_flow");

    asx_runtime_builder builder;
    asx_runtime runtime;
    asx_socket_addr local_addr;
    asx_socket_addr peer_addr;
    asx_socket_addr udp_peer;
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_udp_socket socket;
    asx_buf payload;
    asx_buf_mut dst;
    uint32_t n = 0u;

    SCENARIO_CHECK(asx_runtime_builder_init_current_thread(&builder) == ASX_OK, "builder_init");
    SCENARIO_CHECK(asx_runtime_builder_set_finalizer_poll_budget(&builder, 40u) == ASX_OK,
                   "builder_budget");
    SCENARIO_CHECK(asx_runtime_builder_build(&builder, &runtime) == ASX_OK, "runtime_build");

    local_addr = asx_socket_addr_loopback(9100u);
    peer_addr = asx_socket_addr_ipv6_loopback(9100u);

    SCENARIO_CHECK(asx_tcp_listener_bind(&listener, &local_addr) == ASX_OK, "listener_bind");
    SCENARIO_CHECK(asx_tcp_listener_is_alive(listener), "listener_alive");
    SCENARIO_CHECK(asx_tcp_listener_poll_accept(listener, &stream, NULL) == ASX_E_PENDING,
                   "listener_pending");

    SCENARIO_CHECK(asx_tcp_connect(&stream, &peer_addr) == ASX_OK, "tcp_connect");
    SCENARIO_CHECK(asx_tcp_stream_peer_addr(stream, &peer_addr) == ASX_OK, "peer_addr");
    SCENARIO_CHECK(peer_addr.port == 9100u, "peer_port");

    payload = asx_buf_from_cstr("example-network");
    asx_buf_mut_init(&dst);
    SCENARIO_CHECK(asx_tcp_stream_poll_write(stream, &payload, &n) == ASX_E_PENDING,
                   "tcp_write_pending");
    SCENARIO_CHECK(asx_tcp_stream_poll_read(stream, &dst, &n) == ASX_E_PENDING,
                   "tcp_read_pending");

    SCENARIO_CHECK(asx_udp_bind(&socket, &local_addr) == ASX_OK, "udp_bind");
    SCENARIO_CHECK(asx_udp_connect(socket, &peer_addr) == ASX_OK, "udp_connect");
    SCENARIO_CHECK(asx_udp_peer_addr(socket, &udp_peer) == ASX_OK, "udp_peer");
    SCENARIO_CHECK(asx_udp_poll_send(socket, &payload, &n, NULL) == ASX_E_PENDING,
                   "udp_send_pending");
    SCENARIO_CHECK(asx_udp_poll_recv(socket, &dst, &n, NULL) == ASX_E_PENDING,
                   "udp_recv_pending");

    SCENARIO_CHECK(asx_udp_close(socket) == ASX_OK, "udp_close");
    SCENARIO_CHECK(asx_tcp_stream_close(stream) == ASX_OK, "tcp_close");
    SCENARIO_CHECK(asx_tcp_listener_close(listener) == ASX_OK, "listener_close");
    asx_runtime_shutdown(&runtime);

    SCENARIO_END();
}

int main(void) {
    scenario_resolve_and_order();
    scenario_runtime_tcp_udp_flow();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
