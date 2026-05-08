/*
 * e2e_network_surface.c — deterministic smoke lane for public network surfaces
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long mix_u64(unsigned long long state, unsigned long long value) {
    state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    return state;
}

static const char *env_or_default(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value != NULL ? value : fallback;
}

static const char *compile_profile_name(void) {
#if defined(ASX_PROFILE_POSIX)
    return "POSIX";
#elif defined(ASX_PROFILE_WIN32)
    return "WIN32";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "FREESTANDING";
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_HFT)
    return "HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "AUTOMOTIVE";
#elif defined(ASX_PROFILE_PARALLEL)
    return "PARALLEL";
#elif defined(ASX_PROFILE_BROWSER)
    return "BROWSER";
#else
    return "CORE";
#endif
}

static const char *compile_codec_name(void) {
#if defined(ASX_CODEC_BIN)
    return "bin";
#else
    return "json";
#endif
}

static const char *compile_deterministic_name(void) {
#if ASX_DETERMINISTIC
    return "1";
#else
    return "0";
#endif
}

static void scenario_line(const char *id, int pass, const char *detail) {
    printf("SCENARIO %s %s%s%s\n", id, pass ? "pass" : "fail", detail != NULL ? " " : "",
           detail != NULL ? detail : "");
}

static unsigned long long record_run_config(unsigned long long digest) {
    const char *seed = env_or_default("ASX_E2E_SEED", "42");
    const char *profile = env_or_default("ASX_E2E_PROFILE", compile_profile_name());
    const char *codec = env_or_default("ASX_E2E_CODEC", compile_codec_name());
    const char *deterministic =
        env_or_default("ASX_E2E_DETERMINISTIC", compile_deterministic_name());
    const char *resource_class = env_or_default("ASX_E2E_RESOURCE_CLASS", "R3");
    char detail[384];
    int n;

    n = snprintf(detail, sizeof(detail),
                 "seed=%s profile=%s codec=%s deterministic=%s resource_class=%s "
                 "command=make_test-e2e-network-surface",
                 seed, profile, codec, deterministic, resource_class);
    if (n < 0 || (size_t)n >= sizeof(detail)) {
        scenario_line("network.config", 0, "detail_truncated");
        return digest;
    }
    scenario_line("network.config", 1, detail);
    printf("TRACE network.config %s\n", detail);
    printf("REPLAY ASX_E2E_SEED=%s ASX_E2E_PROFILE=%s ASX_E2E_CODEC=%s "
           "ASX_E2E_DETERMINISTIC=%s make test-e2e-network-surface\n",
           seed, profile, codec, deterministic);

    digest = mix_u64(digest, (unsigned long long)ASX_BUF_CAPACITY);
    digest = mix_u64(digest, (unsigned long long)ASX_MAX_PIPES);
    return digest;
}

static unsigned long long record_unsupported_profile(unsigned long long digest) {
#if defined(ASX_PROFILE_POSIX)
    scenario_line("network.unsupported_profile", 1,
                  "profile=POSIX protocol_stacks=deferred fail_closed=1");
#elif defined(ASX_PROFILE_CORE)
    scenario_line("network.unsupported_profile", 1,
                  "profile=CORE native_socket_backend=deferred fail_closed=1");
#elif defined(ASX_PROFILE_BROWSER)
    scenario_line("network.unsupported_profile", 1,
                  "profile=BROWSER live_socket_backend=unsupported fail_closed=1");
#else
    scenario_line("network.unsupported_profile", 1,
                  "profile=non_core live_socket_claim=not_counted fail_closed=1");
#endif

#if ASX_HAS_IO_URING == 0
    scenario_line("network.unsupported_io_uring", 1, "ASX_HAS_IO_URING=0");
#else
    scenario_line("network.unsupported_io_uring", 0, "unexpected_io_uring_support");
#endif
    return mix_u64(digest, 0x0u);
}

static void fill_pattern(uint8_t *buf, uint32_t len) {
    uint32_t i;
    for (i = 0u; i < len; i++) { buf[i] = (uint8_t)('A' + (i % 23u)); }
}

static int run_pipe_backed_surface(unsigned long long *digest) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    asx_pipe_read rds[ASX_MAX_PIPES];
    asx_pipe_write wrs[ASX_MAX_PIPES];
    asx_pipe_read temp_rd;
    asx_pipe_write temp_wr;
    asx_buf payload;
    asx_buf readable;
    asx_buf src;
    asx_buf one_src;
    asx_buf_mut dst;
    uint8_t full[ASX_BUF_CAPACITY];
    uint8_t one = 'z';
    uint32_t n = 0u;
    uint32_t i;

    asx_pipe_reset();
    if (asx_pipe_open(&rd, &wr) != ASX_OK || !asx_pipe_read_is_alive(rd) ||
        !asx_pipe_write_is_alive(wr)) {
        scenario_line("network.pipe_open", 0, "pipe_pair_not_live");
        return 0;
    }
    scenario_line("network.pipe_open", 1, "bounded_pair=1");
    *digest = mix_u64(*digest, 1u);

    payload = asx_buf_from_cstr("pipe-backed-network");
    asx_buf_mut_init(&dst);
    if (asx_pipe_poll_write(wr, &payload, &n) != ASX_OK || n != payload.len ||
        asx_pipe_poll_read(rd, &dst, &n) != ASX_OK || n != payload.len) {
        scenario_line("network.pipe_lifecycle", 0, "pipe_roundtrip_failed");
        return 0;
    }
    readable = asx_buf_mut_readable(&dst);
    if (readable.len != payload.len || memcmp(readable.ptr, payload.ptr, payload.len) != 0) {
        scenario_line("network.pipe_lifecycle", 0, "payload_mismatch");
        return 0;
    }
    scenario_line("network.pipe_lifecycle", 1, "roundtrip_bytes=19");
    *digest = mix_u64(*digest, (unsigned long long)payload.len);

    if (asx_pipe_close_write(wr) != ASX_OK ||
        asx_pipe_poll_read(rd, &dst, &n) != ASX_E_DISCONNECTED || n != 0u ||
        asx_pipe_close_read(rd) != ASX_OK) {
        scenario_line("network.pipe_close_cancel", 0, "close_did_not_cancel_reader");
        return 0;
    }
    scenario_line("network.pipe_close_cancel", 1, "reader_observed_disconnected");
    *digest = mix_u64(*digest, 2u);

    asx_pipe_reset();
    if (asx_pipe_open(&rd, &wr) != ASX_OK) {
        scenario_line("network.pipe_backpressure", 0, "pipe_reopen_failed");
        return 0;
    }
    fill_pattern(full, ASX_BUF_CAPACITY);
    src = asx_buf_from(full, ASX_BUF_CAPACITY);
    one_src = asx_buf_from(&one, 1u);
    n = 0u;
    if (asx_pipe_poll_write(wr, &src, &n) != ASX_OK || n != ASX_BUF_CAPACITY) {
        scenario_line("network.pipe_backpressure", 0, "initial_fill_failed");
        return 0;
    }
    n = 123u;
    if (asx_pipe_poll_write(wr, &one_src, &n) != ASX_E_WOULD_BLOCK || n != 0u) {
        scenario_line("network.pipe_backpressure", 0, "full_buffer_not_would_block");
        return 0;
    }
    asx_buf_mut_clear(&dst);
    if (asx_pipe_poll_read(rd, &dst, &n) != ASX_OK || n != ASX_BUF_CAPACITY ||
        asx_pipe_poll_write(wr, &one_src, &n) != ASX_OK || n != 1u) {
        scenario_line("network.pipe_backpressure", 0, "capacity_not_released_after_read");
        return 0;
    }
    scenario_line("network.pipe_backpressure", 1, "bounded_buffer_would_block_then_released");
    *digest = mix_u64(*digest, (unsigned long long)ASX_BUF_CAPACITY);
    asx_pipe_close_read(rd);
    asx_pipe_close_write(wr);

    asx_pipe_reset();
    for (i = 0u; i < ASX_MAX_PIPES; i++) {
        if (asx_pipe_open(&rds[i], &wrs[i]) != ASX_OK) {
            scenario_line("network.pipe_resource_exhaustion", 0, "open_within_cap_failed");
            return 0;
        }
    }
    if (asx_pipe_open(&temp_rd, &temp_wr) != ASX_E_RESOURCE_EXHAUSTED) {
        scenario_line("network.pipe_resource_exhaustion", 0, "over_cap_did_not_fail");
        return 0;
    }
    for (i = 0u; i < ASX_MAX_PIPES; i++) {
        asx_pipe_close_read(rds[i]);
        asx_pipe_close_write(wrs[i]);
    }
    scenario_line("network.pipe_resource_exhaustion", 1, "over_cap_failed_atomically");
    *digest = mix_u64(*digest, (unsigned long long)ASX_MAX_PIPES);

    return 1;
}

int main(void) {
    asx_runtime_builder builder;
    asx_runtime runtime;
    asx_resolver resolver;
    asx_resolve_options resolve_opts;
    asx_resolve_options udp_resolve_opts;
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
    uint8_t cache_hit = 0u;
    uint32_t io_n = 0u;
    unsigned long long digest = 0xcbf29ce484222325ULL;

    digest = record_run_config(digest);
    digest = record_unsupported_profile(digest);

    if (asx_runtime_builder_init_current_thread(&builder) != ASX_OK ||
        asx_runtime_builder_set_finalizer_poll_budget(&builder, 40u) != ASX_OK ||
        asx_runtime_builder_build(&builder, &runtime) != ASX_OK) {
        printf("SCENARIO network.builder fail builder_contract\n");
        return 1;
    }
    printf("SCENARIO network.builder pass\n");
    digest = mix_u64(digest, 40u);

    asx_resolver_init(&resolver);
    asx_resolve_options_init(&resolve_opts, 8123u);
    asx_resolve_options_init(&udp_resolve_opts, 8123u);
    udp_resolve_opts.preferred_family = ASX_AF_INET4;
    udp_resolve_opts.allow_ipv6 = 0u;
    if (asx_resolver_lookup(&resolver, "localhost", &resolve_opts, &resolved, &cache_hit) !=
            ASX_OK ||
        cache_hit != 0u || resolved.count < 2u ||
        asx_happy_eyeballs_order(&ordered, &resolved, ASX_AF_INET6) != ASX_OK ||
        ordered.count != resolved.count || ordered.addrs[0].family != ASX_AF_INET6 ||
        ordered.addrs[1].family != ASX_AF_INET4) {
        printf("SCENARIO network.resolve fail resolver_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.resolve pass\n");
    digest = mix_u64(digest, (unsigned long long)ordered.count);

    if (asx_resolver_lookup(&resolver, "localhost", &resolve_opts, &resolved, &cache_hit) !=
            ASX_OK ||
        cache_hit != 1u ||
        asx_resolver_lookup(&resolver, "not-a-known-host", &resolve_opts, &resolved, &cache_hit) !=
            ASX_E_NOT_FOUND ||
        cache_hit != 0u) {
        printf("SCENARIO network.resolve_failure fail missing_not_found\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.resolve_failure pass\n");
    digest = mix_u64(digest, 2u);

    listen_addr = ordered.addrs[0];
    if (asx_tcp_listener_bind(&listener, &listen_addr) != ASX_OK ||
        !asx_tcp_listener_is_alive(listener) ||
        asx_tcp_listener_poll_accept(listener, &stream, NULL) != ASX_E_PENDING) {
        printf("SCENARIO network.listener fail listener_contract\n");
        asx_runtime_shutdown(&runtime);
        return 1;
    }
    printf("SCENARIO network.listener pass\n");
    digest = mix_u64(digest, (unsigned long long)listen_addr.port);

    if (asx_tcp_connect_host(&stream, &resolver, "localhost", &resolve_opts, &stream_peer,
                             &cache_hit) != ASX_OK ||
        cache_hit != 1u ||
        asx_tcp_listener_poll_accept(listener, &accepted, &accept_peer) != ASX_OK ||
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
        asx_udp_connect_host(socket, &resolver, "localhost", &udp_resolve_opts, &udp_peer,
                             &cache_hit) != ASX_OK ||
        cache_hit != 0u || asx_udp_peer_addr(socket, &udp_peer) != ASX_OK ||
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

    if (!run_pipe_backed_surface(&digest)) {
        asx_runtime_shutdown(&runtime);
        return 1;
    }

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
