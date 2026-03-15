/*
 * vignette_network.c — operator-path walkthrough for deterministic network APIs
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

static int require_status(asx_status st, const char *label) {
    if (st != ASX_OK) {
        fprintf(stderr, "%s failed: %s\n", label, asx_status_str(st));
        return 0;
    }
    return 1;
}

int main(void) {
    asx_runtime_builder builder;
    asx_runtime runtime;
    asx_resolve_options resolve_opts;
    asx_resolve_result resolved;
    asx_resolve_result ordered;
    asx_socket_addr listen_addr;
    asx_socket_addr accept_peer;
    asx_socket_addr stream_peer;
    asx_socket_addr udp_local;
    asx_socket_addr udp_peer;
    asx_tcp_stream accepted;
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_udp_socket receiver;
    asx_udp_socket socket;
    asx_buf payload;
    asx_buf_mut dst;
    uint32_t io_n = 0u;

    if (!require_status(asx_runtime_builder_init_current_thread(&builder), "builder init") ||
        !require_status(asx_runtime_builder_set_finalizer_poll_budget(&builder, 48u),
                        "builder budget") ||
        !require_status(asx_runtime_builder_build(&builder, &runtime), "builder build")) {
        return 1;
    }

    asx_resolve_options_init(&resolve_opts, 7000u);
    if (!require_status(asx_resolve_host(&resolved, "localhost", &resolve_opts), "resolve_host") ||
        !require_status(asx_happy_eyeballs_order(&ordered, &resolved, ASX_AF_INET6),
                        "happy_eyeballs")) {
        asx_runtime_shutdown(&runtime);
        return 1;
    }

    if (ordered.count < 2u || ordered.addrs[0].family != ASX_AF_INET6 ||
        ordered.addrs[1].family != ASX_AF_INET4) {
        fprintf(stderr, "unexpected resolver ordering\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }

    listen_addr = asx_socket_addr_loopback(7000u);
    if (!require_status(asx_tcp_listener_bind(&listener, &listen_addr), "listener bind") ||
        !require_status(asx_tcp_connect(&stream, &ordered.addrs[0]), "tcp connect") ||
        !require_status(asx_tcp_listener_poll_accept(listener, &accepted, &accept_peer),
                        "listener accept") ||
        !require_status(asx_tcp_stream_peer_addr(stream, &stream_peer), "stream peer addr") ||
        !require_status(asx_udp_bind(&socket, &listen_addr), "udp bind") ||
        !require_status(asx_udp_bind(&receiver, &ordered.addrs[1]), "udp bind peer") ||
        !require_status(asx_udp_connect(socket, &ordered.addrs[1]), "udp connect") ||
        !require_status(asx_udp_local_addr(socket, &udp_local), "udp local addr") ||
        !require_status(asx_udp_peer_addr(socket, &udp_peer), "udp peer addr")) {
        asx_runtime_shutdown(&runtime);
        return 1;
    }

    payload = asx_buf_from_cstr("network-demo");
    asx_buf_mut_init(&dst);
    if (asx_tcp_stream_poll_write(stream, &payload, &io_n) != ASX_OK || io_n != payload.len) {
        fprintf(stderr, "expected loopback stream write\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    if (asx_tcp_stream_poll_read(accepted, &dst, &io_n) != ASX_OK || io_n != payload.len) {
        fprintf(stderr, "expected loopback stream read\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }

    asx_buf_mut_clear(&dst);
    if (asx_udp_poll_send(socket, &payload, &io_n, NULL) != ASX_OK || io_n != payload.len) {
        fprintf(stderr, "expected udp loopback send\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    if (asx_udp_poll_recv(receiver, &dst, &io_n, NULL) != ASX_OK || io_n != payload.len) {
        fprintf(stderr, "expected udp loopback recv\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }

    printf("network.resolve.count=%u\n", (unsigned)ordered.count);
    printf("network.resolve.order=%d,%d\n", (int)ordered.addrs[0].family,
           (int)ordered.addrs[1].family);
    printf("network.listener.alive=%d\n", asx_tcp_listener_is_alive(listener));
    printf("network.accept.peer_port=%u\n", (unsigned)accept_peer.port);
    printf("network.stream.peer_port=%u\n", (unsigned)stream_peer.port);
    printf("network.udp.local_port=%u\n", (unsigned)udp_local.port);
    printf("network.udp.peer_family=%d\n", (int)udp_peer.family);
    printf("rerun_hint: rch exec -- make %s\n", "build/tests/vignettes/vignette_network");

    if (asx_udp_close(receiver) != ASX_OK || asx_udp_close(socket) != ASX_OK ||
        asx_tcp_stream_close(accepted) != ASX_OK || asx_tcp_stream_close(stream) != ASX_OK ||
        asx_tcp_listener_close(listener) != ASX_OK) {
        fprintf(stderr, "close contract failed\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }

    asx_runtime_shutdown(&runtime);
    return 0;
}
