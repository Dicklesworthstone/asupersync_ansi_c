/*
 * e2e_network_surface.c — deterministic smoke lane for public network surfaces
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

static unsigned long long mix_u64(unsigned long long state, unsigned long long value) {
    state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    return state;
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
    asx_socket_addr udp_peer;
    asx_tcp_stream accepted;
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_udp_socket receiver;
    asx_udp_socket socket;
    asx_buf payload;
    asx_buf_mut dst;
    asx_buf readable;
    uint32_t io_n = 0u;
    unsigned long long digest = 0xcbf29ce484222325ULL;

    if (asx_runtime_builder_init_current_thread(&builder) != ASX_OK ||
        asx_runtime_builder_set_finalizer_poll_budget(&builder, 40u) != ASX_OK ||
        asx_runtime_builder_build(&builder, &runtime) != ASX_OK) {
        printf("SCENARIO network.builder fail builder_contract\n");
        return 1;
    }
    printf("SCENARIO network.builder pass\n");
    digest = mix_u64(digest, 40u);

    asx_resolve_options_init(&resolve_opts, 8123u);
    if (asx_resolve_host(&resolved, "localhost", &resolve_opts) != ASX_OK ||
        resolved.count < 2u ||
        asx_happy_eyeballs_order(&ordered, &resolved, ASX_AF_INET6) != ASX_OK ||
        ordered.count != resolved.count || ordered.addrs[0].family != ASX_AF_INET6 ||
        ordered.addrs[1].family != ASX_AF_INET4) {
        printf("SCENARIO network.resolve fail resolver_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.resolve pass\n");
    digest = mix_u64(digest, (unsigned long long)ordered.count);

    if (asx_resolve_host(&resolved, "not-a-known-host", &resolve_opts) != ASX_E_NOT_FOUND) {
        printf("SCENARIO network.resolve_failure fail missing_not_found\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.resolve_failure pass\n");
    digest = mix_u64(digest, 2u);

    listen_addr = asx_socket_addr_loopback(8123u);
    if (asx_tcp_listener_bind(&listener, &listen_addr) != ASX_OK ||
        !asx_tcp_listener_is_alive(listener) ||
        asx_tcp_listener_poll_accept(listener, &stream, NULL) != ASX_E_PENDING) {
        printf("SCENARIO network.listener fail listener_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.listener pass\n");
    digest = mix_u64(digest, (unsigned long long)listen_addr.port);

    if (asx_tcp_connect(&stream, &ordered.addrs[0]) != ASX_OK ||
        asx_tcp_listener_poll_accept(listener, &accepted, &accept_peer) != ASX_OK ||
        asx_tcp_stream_peer_addr(stream, &stream_peer) != ASX_OK ||
        stream_peer.port != 8123u || !asx_tcp_stream_is_alive(stream) ||
        !asx_tcp_stream_is_alive(accepted) || accept_peer.port == 0u) {
        printf("SCENARIO network.stream_setup fail connect_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.stream_setup pass\n");
    digest = mix_u64(digest, (unsigned long long)stream_peer.family);

    payload = asx_buf_from_cstr("network-surface");
    asx_buf_mut_init(&dst);
    if (asx_tcp_stream_poll_write(stream, &payload, &io_n) != ASX_OK || io_n != payload.len ||
        asx_tcp_stream_poll_read(accepted, &dst, &io_n) != ASX_OK || io_n != payload.len) {
        printf("SCENARIO network.stream_io fail pending_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    readable = asx_buf_mut_readable(&dst);
    if (readable.len != payload.len || memcmp(readable.ptr, payload.ptr, payload.len) != 0) {
        printf("SCENARIO network.stream_io fail payload_mismatch\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.stream_io pass\n");
    digest = mix_u64(digest, (unsigned long long)payload.len);

    if (asx_udp_bind(&socket, &listen_addr) != ASX_OK ||
        asx_udp_bind(&receiver, &ordered.addrs[1]) != ASX_OK ||
        asx_udp_connect(socket, &ordered.addrs[1]) != ASX_OK ||
        asx_udp_peer_addr(socket, &udp_peer) != ASX_OK ||
        asx_udp_poll_send(socket, &payload, &io_n, NULL) != ASX_OK || io_n != payload.len) {
        printf("SCENARIO network.udp fail udp_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    asx_buf_mut_clear(&dst);
    if (asx_udp_poll_recv(receiver, &dst, &io_n, NULL) != ASX_OK || io_n != payload.len) {
        printf("SCENARIO network.udp fail udp_recv_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    readable = asx_buf_mut_readable(&dst);
    if (readable.len != payload.len || memcmp(readable.ptr, payload.ptr, payload.len) != 0) {
        printf("SCENARIO network.udp fail payload_mismatch\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.udp pass\n");
    digest = mix_u64(digest, (unsigned long long)udp_peer.family);

    if (asx_udp_close(receiver) != ASX_OK || asx_udp_close(socket) != ASX_OK ||
        asx_tcp_stream_close(accepted) != ASX_OK || asx_tcp_stream_close(stream) != ASX_OK ||
        asx_tcp_listener_close(listener) != ASX_OK) {
        printf("SCENARIO network.close fail close_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.close pass\n");
    digest = mix_u64(digest, 6u);

    asx_runtime_shutdown(&runtime);
    printf("DIGEST %llx\n", digest);
    return 0;
}
