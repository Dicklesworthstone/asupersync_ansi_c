/*
 * e2e_http_robustness.c — a live native HTTP server for http_robustness.sh
 *
 * Serves GET /hello on real sockets until SIGTERM, then drains
 * (bd-9kll.10.3). Built against a live POSIX library (DETERMINISTIC=0).
 *
 * Usage: e2e_http_robustness ADDR IDLE_MS DRAIN_MS
 *   ADDR is 127.0.0.1, 0.0.0.0, ::1 or ::; the port is ephemeral.
 *
 * Prints "LISTENING <port>" once bound, "DRAIN" when SIGTERM starts the
 * graceful drain, and at exit one line
 * "STATS served=N timed_out=N drain_closed=N rejected=N accepted=N".
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/net/http.h>
#include <asx/net/server.h>
#include <asx/signal/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static asx_server g_srv;
static asx_http_server g_hs;
static asx_http_router g_router;
static asx_signal_subscription g_sub;
static asx_task_id g_server_task;

static asx_status h_hello(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ctx;
    (void)ud;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    return asx_http_body_set_bytes(&resp->body, "hello", 5u);
}

static asx_status server_task(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return asx_http_server_poll(&g_hs);
}

/* SIGTERM starts the graceful drain; the server task finishes it. */
static asx_status signal_task(void *ud, asx_task_id self) {
    uint32_t count = 0u;
    asx_status st;
    (void)ud;
    (void)self;
    st = asx_signal_poll(g_sub, &count);
    if (st != ASX_OK) return st;
    if (asx_server_shutdown(&g_srv) != ASX_OK) return ASX_E_INVALID_STATE;
    printf("DRAIN\n");
    fflush(stdout);
    return asx_task_wake(g_server_task);
}

static int parse_addr(const char *text, asx_socket_addr *out) {
    static const uint8_t any6[16] = {0};

    if (strcmp(text, "127.0.0.1") == 0) {
        *out = asx_socket_addr_loopback(0u);
    } else if (strcmp(text, "0.0.0.0") == 0) {
        *out = asx_socket_addr_ipv4(0u, 0u, 0u, 0u, 0u);
    } else if (strcmp(text, "::1") == 0) {
        *out = asx_socket_addr_ipv6_loopback(0u);
    } else if (strcmp(text, "::") == 0) {
        *out = asx_socket_addr_ipv6(any6, 0u);
    } else {
        return 0;
    }
    return 1;
}

static int fail(const char *what, asx_status st) {
    fprintf(stderr, "e2e_http_robustness: %s: %s\n", what, asx_status_str(st));
    return 1;
}

int main(int argc, char **argv) {
    asx_runtime rt;
    asx_server_config scfg;
    asx_http_server_config hcfg;
    asx_socket_addr bound;
    asx_region_id region;
    asx_task_id sig_task;
    asx_budget budget;
    asx_status st;

    if (argc != 4) {
        fprintf(stderr, "usage: %s ADDR IDLE_MS DRAIN_MS\n", argv[0]);
        return 2;
    }
    st = asx_runtime_init_default(&rt);
    if (st != ASX_OK) return fail("runtime init", st);
    if (asx_net_get_backend() != ASX_NET_BACKEND_NATIVE) {
        fprintf(stderr, "e2e_http_robustness: needs a live POSIX build (native sockets)\n");
        return 1;
    }

    asx_http_router_init(&g_router);
    st = asx_http_router_add_route(&g_router, ASX_HTTP_GET, "/hello", h_hello, NULL, NULL, NULL);
    if (st != ASX_OK) return fail("route", st);

    asx_server_config_init(&scfg);
    if (!parse_addr(argv[1], &scfg.listen_addr)) {
        fprintf(stderr, "e2e_http_robustness: unknown address %s\n", argv[1]);
        return 2;
    }
    scfg.drain_timeout_ms = (uint32_t)strtoul(argv[3], NULL, 10);
    asx_server_init(&g_srv, &scfg);
    st = asx_server_listen(&g_srv);
    if (st != ASX_OK) return fail("listen", st);
    st = asx_tcp_listener_local_addr(g_srv.listener, &bound);
    if (st != ASX_OK) return fail("local addr", st);

    asx_http_server_config_init(&hcfg, &g_router);
    hcfg.idle_timeout_ms = (uint32_t)strtoul(argv[2], NULL, 10);
    st = asx_http_server_init(&g_hs, &g_srv, &hcfg);
    if (st != ASX_OK) return fail("http server init", st);
    st = asx_signal_subscribe(&g_sub, ASX_SIGNAL_TERM);
    if (st != ASX_OK) return fail("signal subscribe", st);

    st = asx_region_open(&region);
    if (st != ASX_OK) return fail("region", st);
    st = asx_task_spawn(region, server_task, NULL, &g_server_task);
    if (st != ASX_OK) return fail("spawn server", st);
    st = asx_task_spawn(region, signal_task, NULL, &sig_task);
    if (st != ASX_OK) return fail("spawn signal", st);

    printf("LISTENING %u\n", (unsigned)bound.port);
    fflush(stdout);
    budget = asx_budget_infinite();
    st = asx_scheduler_run(region, &budget);

    printf("STATS served=%u timed_out=%u drain_closed=%u rejected=%u accepted=%u\n",
           (unsigned)asx_http_server_requests_served(&g_hs), (unsigned)g_hs.connections_timed_out,
           (unsigned)g_hs.connections_drain_closed, (unsigned)asx_server_total_rejected(&g_srv),
           (unsigned)asx_server_total_accepted(&g_srv));
    fflush(stdout);
    if (asx_signal_unsubscribe(g_sub) != ASX_OK) fprintf(stderr, "unsubscribe failed\n");
    asx_runtime_shutdown(&rt);
    if (st != ASX_OK) return fail("run", st);
    return 0;
}
