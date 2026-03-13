/*
 * test_app.c — unit tests for net primitives, app lifecycle, CLI, and doctor
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/net.h>
#include <asx/app/app.h>
#include <asx/app/doctor.h>
#include <asx/runtime/runtime.h>
#include <asx/core/budget.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

#define ASSERT(cond, msg) do {                                           \
    if (!(cond)) {                                                       \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__);                 \
        g_fail++; return;                                                \
    }                                                                    \
} while (0)

#define RUN(fn) do {                                                     \
    printf("  " #fn "...\n");                                            \
    asx_runtime_reset();                                                 \
    fn(); g_pass++;                                                      \
} while (0)

/* ================================================================== */
/* NET: Socket address tests                                          */
/* ================================================================== */

static void test_socket_addr_ipv4(void)
{
    asx_socket_addr sa = asx_socket_addr_ipv4(192, 168, 1, 100, 8080);
    ASSERT(sa.addr[0] == 192, "addr[0]");
    ASSERT(sa.addr[1] == 168, "addr[1]");
    ASSERT(sa.addr[2] == 1,   "addr[2]");
    ASSERT(sa.addr[3] == 100, "addr[3]");
    ASSERT(sa.port == 8080,   "port");
    ASSERT(sa.family == ASX_AF_INET4, "family");
}

static void test_socket_addr_loopback(void)
{
    asx_socket_addr sa = asx_socket_addr_loopback(3000);
    ASSERT(sa.addr[0] == 127, "loopback addr[0]");
    ASSERT(sa.addr[1] == 0,   "loopback addr[1]");
    ASSERT(sa.addr[2] == 0,   "loopback addr[2]");
    ASSERT(sa.addr[3] == 1,   "loopback addr[3]");
    ASSERT(sa.port == 3000,   "loopback port");
}

static void test_socket_addr_eq(void)
{
    asx_socket_addr a = asx_socket_addr_ipv4(10, 0, 0, 1, 80);
    asx_socket_addr b = asx_socket_addr_ipv4(10, 0, 0, 1, 80);
    asx_socket_addr c = asx_socket_addr_ipv4(10, 0, 0, 2, 80);
    asx_socket_addr d = asx_socket_addr_ipv4(10, 0, 0, 1, 81);

    ASSERT(asx_socket_addr_eq(&a, &b) != 0, "same addrs equal");
    ASSERT(asx_socket_addr_eq(&a, &c) == 0, "diff addr not equal");
    ASSERT(asx_socket_addr_eq(&a, &d) == 0, "diff port not equal");
    ASSERT(asx_socket_addr_eq(NULL, &b) == 0, "null lhs");
    ASSERT(asx_socket_addr_eq(&a, NULL) == 0, "null rhs");
}

/* ================================================================== */
/* NET: TCP listener tests                                            */
/* ================================================================== */

static void test_tcp_listener_bind_close(void)
{
    asx_tcp_listener listener;
    asx_socket_addr addr = asx_socket_addr_loopback(8080);
    asx_status st;

    st = asx_tcp_listener_bind(&listener, &addr);
    ASSERT(st == ASX_OK, "bind ok");
    ASSERT(asx_tcp_listener_is_alive(listener), "listener alive");

    st = asx_tcp_listener_close(listener);
    ASSERT(st == ASX_OK, "close ok");
    ASSERT(!asx_tcp_listener_is_alive(listener), "listener dead");
}

static void test_tcp_listener_local_addr(void)
{
    asx_tcp_listener listener;
    asx_socket_addr addr = asx_socket_addr_ipv4(10, 0, 0, 1, 9090);
    asx_socket_addr out;

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    ASSERT(asx_tcp_listener_local_addr(listener, &out) == ASX_OK, "local_addr ok");
    ASSERT(asx_socket_addr_eq(&addr, &out), "local addr matches bind addr");

    asx_tcp_listener_close(listener);
}

static void test_tcp_listener_poll_accept_pending(void)
{
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(7070);

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));

    /* Ghost stub: always pending */
    ASSERT(asx_tcp_listener_poll_accept(listener, &stream, NULL) == ASX_E_PENDING,
           "poll_accept pending");

    asx_tcp_listener_close(listener);
}

static void test_tcp_listener_null_args(void)
{
    asx_socket_addr addr = asx_socket_addr_loopback(80);
    asx_tcp_listener listener;

    ASSERT(asx_tcp_listener_bind(NULL, &addr) == ASX_E_INVALID_ARGUMENT,
           "bind null out");
    ASSERT(asx_tcp_listener_bind(&listener, NULL) == ASX_E_INVALID_ARGUMENT,
           "bind null addr");
}

static void test_tcp_listener_arena_exhaustion(void)
{
    asx_tcp_listener listeners[ASX_MAX_TCP_LISTENERS];
    asx_tcp_listener extra;
    asx_socket_addr addr = asx_socket_addr_loopback(5000);
    uint32_t i;
    asx_status st;

    for (i = 0; i < ASX_MAX_TCP_LISTENERS; i++) {
        addr.port = (uint16_t)(5000 + i);
        st = asx_tcp_listener_bind(&listeners[i], &addr);
        ASSERT(st == ASX_OK, "bind within capacity");
    }

    addr.port = 9999;
    st = asx_tcp_listener_bind(&extra, &addr);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "arena exhausted");

    for (i = 0; i < ASX_MAX_TCP_LISTENERS; i++) {
        asx_tcp_listener_close(listeners[i]);
    }
}

/* ================================================================== */
/* NET: TCP stream tests                                              */
/* ================================================================== */

static void test_tcp_connect_close(void)
{
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(4000);

    ASSERT(asx_tcp_connect(&stream, &addr) == ASX_OK, "connect ok");
    ASSERT(asx_tcp_stream_is_alive(stream), "stream alive");

    ASSERT(asx_tcp_stream_close(stream) == ASX_OK, "close ok");
    ASSERT(!asx_tcp_stream_is_alive(stream), "stream dead");
}

static void test_tcp_stream_peer_addr(void)
{
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_ipv4(10, 0, 0, 5, 443);
    asx_socket_addr out;

    MUST_OK(asx_tcp_connect(&stream, &addr));
    ASSERT(asx_tcp_stream_peer_addr(stream, &out) == ASX_OK, "peer_addr ok");
    ASSERT(asx_socket_addr_eq(&addr, &out), "peer addr matches connect addr");

    asx_tcp_stream_close(stream);
}

static void test_tcp_stream_poll_pending(void)
{
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(2000);
    asx_buf_mut dst;
    asx_buf src;
    uint32_t n;

    MUST_OK(asx_tcp_connect(&stream, &addr));

    asx_buf_mut_init(&dst);
    ASSERT(asx_tcp_stream_poll_read(stream, &dst, &n) == ASX_E_PENDING,
           "poll_read pending");
    ASSERT(n == 0, "no bytes read");

    src = asx_buf_from_cstr("hello");
    ASSERT(asx_tcp_stream_poll_write(stream, &src, &n) == ASX_E_PENDING,
           "poll_write pending");
    ASSERT(n == 0, "no bytes written");

    asx_tcp_stream_close(stream);
}

static void test_tcp_stream_stale_handle(void)
{
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(1111);

    MUST_OK(asx_tcp_connect(&stream, &addr));
    asx_tcp_stream_close(stream);

    /* Stale handle after close */
    ASSERT(!asx_tcp_stream_is_alive(stream), "stale not alive");
    ASSERT(asx_tcp_stream_close(stream) == ASX_E_INVALID_ARGUMENT,
           "double close rejected");
}

static void test_net_reset(void)
{
    asx_tcp_listener listener;
    asx_tcp_stream stream;
    asx_socket_addr addr = asx_socket_addr_loopback(6000);

    MUST_OK(asx_tcp_listener_bind(&listener, &addr));
    MUST_OK(asx_tcp_connect(&stream, &addr));

    asx_net_reset();

    ASSERT(!asx_tcp_listener_is_alive(listener), "listener dead after reset");
    ASSERT(!asx_tcp_stream_is_alive(stream), "stream dead after reset");
}

/* ================================================================== */
/* APP: CLI argument parsing                                          */
/* ================================================================== */

static void test_parse_args_defaults(void)
{
    asx_app_args args;
    const char *argv[] = {"myapp"};

    MUST_OK(asx_app_parse_args(&args, 1, argv));
    ASSERT(args.command == ASX_APP_CMD_RUN, "default command is run");
    ASSERT(args.verbose == 0, "default verbose 0");
    ASSERT(args.help == 0, "default help 0");
    ASSERT(args.seed == 0, "default seed 0");
    ASSERT(args.scenario == NULL, "default scenario null");
}

static void test_parse_args_doctor(void)
{
    asx_app_args args;
    const char *argv[] = {"myapp", "doctor"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.command == ASX_APP_CMD_DOCTOR, "doctor command");
}

static void test_parse_args_replay(void)
{
    asx_app_args args;
    const char *argv[] = {"myapp", "replay", "scenario1"};

    MUST_OK(asx_app_parse_args(&args, 3, argv));
    ASSERT(args.command == ASX_APP_CMD_REPLAY, "replay command");
    ASSERT(args.scenario != NULL, "scenario set");
    ASSERT(strcmp(args.scenario, "scenario1") == 0, "scenario name");
}

static void test_parse_args_verbose(void)
{
    asx_app_args args;
    const char *argv[] = {"myapp", "-v", "--verbose"};

    MUST_OK(asx_app_parse_args(&args, 3, argv));
    ASSERT(args.verbose == 2, "double verbose");
}

static void test_parse_args_seed(void)
{
    asx_app_args args;
    const char *argv[] = {"myapp", "--seed=12345"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.seed == 12345, "seed parsed");
}

static void test_parse_args_help(void)
{
    asx_app_args args;
    const char *argv[] = {"myapp", "--help"};

    MUST_OK(asx_app_parse_args(&args, 2, argv));
    ASSERT(args.help == 1, "help flag");
}

static void test_parse_args_null(void)
{
    ASSERT(asx_app_parse_args(NULL, 0, NULL) == ASX_E_INVALID_ARGUMENT,
           "null args rejected");
}

/* ================================================================== */
/* APP: Lifecycle                                                     */
/* ================================================================== */

static asx_status noop_poll(void *data, asx_task_id self)
{
    (void)data;
    (void)self;
    return ASX_OK;
}

static void test_app_init_shutdown(void)
{
    asx_app app;
    asx_app_config config;
    asx_status st;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";

    st = asx_app_init(&app, &config);
    ASSERT(st == ASX_OK, "app init ok");
    ASSERT(app.initialized == 1, "app initialized");
    ASSERT(app.config.poll_budget == 10000, "default poll budget");

    asx_app_shutdown(&app);
    ASSERT(app.initialized == 0, "app shut down");
}

static void test_app_run_noop(void)
{
    asx_app app;
    asx_app_config config;
    asx_exit_code ec;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";
    config.poll_budget = 100;

    MUST_OK(asx_app_init(&app, &config));

    ec = asx_app_run(&app, noop_poll, NULL);
    ASSERT(ec == ASX_EXIT_OK, "noop run ok");

    asx_app_shutdown(&app);
}

static void test_app_region(void)
{
    asx_app app;
    asx_app_config config;

    memset(&config, 0, sizeof(config));
    config.name = "test_app";

    MUST_OK(asx_app_init(&app, &config));
    ASSERT(asx_app_region(&app) != 0, "region is valid handle");

    asx_app_shutdown(&app);
}

static void test_app_null_args(void)
{
    asx_app app;
    asx_app_config config;

    memset(&config, 0, sizeof(config));

    ASSERT(asx_app_init(NULL, &config) == ASX_E_INVALID_ARGUMENT, "null app");
    ASSERT(asx_app_init(&app, NULL) == ASX_E_INVALID_ARGUMENT, "null config");
    ASSERT(asx_app_run(NULL, noop_poll, NULL) == ASX_EXIT_ERROR, "null app run");

    memset(&app, 0, sizeof(app));
    ASSERT(asx_app_run(&app, noop_poll, NULL) == ASX_EXIT_INIT_FAILED,
           "uninit app run");
    ASSERT(asx_app_region(NULL) == 0, "null app region");
}

/* ================================================================== */
/* DOCTOR: Diagnostic checks                                          */
/* ================================================================== */

static void test_doctor_initialized_runtime(void)
{
    asx_runtime rt;
    asx_doctor_report report;
    asx_status st;

    MUST_OK(asx_runtime_init_default(&rt));

    st = asx_doctor_run(&rt, &report);
    ASSERT(st == ASX_OK, "doctor run ok");
    ASSERT(report.check_count == 6, "6 checks");
    ASSERT(asx_doctor_is_healthy(&report), "healthy");
    ASSERT(asx_doctor_overall(&report) == ASX_DOCTOR_OK, "overall ok");

    /* First check should be runtime=OK */
    ASSERT(strcmp(report.checks[0].name, "runtime") == 0, "first check is runtime");
    ASSERT(report.checks[0].severity == ASX_DOCTOR_OK, "runtime ok");

    asx_runtime_shutdown(&rt);
}

static void test_doctor_uninitialized_runtime(void)
{
    asx_runtime rt;
    asx_doctor_report report;

    memset(&rt, 0, sizeof(rt));

    MUST_OK(asx_doctor_run(&rt, &report));
    ASSERT(report.fail_count > 0, "uninitialized has fails");
    ASSERT(!asx_doctor_is_healthy(&report), "not healthy");
    ASSERT(asx_doctor_overall(&report) == ASX_DOCTOR_FAIL, "overall fail");
}

static void test_doctor_null_args(void)
{
    asx_runtime rt;
    asx_doctor_report report;

    ASSERT(asx_doctor_run(NULL, &report) == ASX_E_INVALID_ARGUMENT, "null rt");
    ASSERT(asx_doctor_run(&rt, NULL) == ASX_E_INVALID_ARGUMENT, "null report");
    ASSERT(asx_doctor_is_healthy(NULL) == 0, "null report not healthy");
    ASSERT(asx_doctor_overall(NULL) == ASX_DOCTOR_FAIL, "null report overall fail");
}

static void test_doctor_check_fields(void)
{
    asx_runtime rt;
    asx_doctor_report report;
    uint32_t i;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_doctor_run(&rt, &report));

    /* Every check should have a non-NULL name and message */
    for (i = 0; i < report.check_count; i++) {
        ASSERT(report.checks[i].name != NULL, "check has name");
        ASSERT(report.checks[i].message != NULL, "check has message");
    }

    /* pass + warn + fail should equal check_count */
    ASSERT(report.pass_count + report.warn_count + report.fail_count
           == report.check_count, "counts sum to total");

    asx_runtime_shutdown(&rt);
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void)
{
    printf("test_app:\n");

    /* Net: socket address */
    RUN(test_socket_addr_ipv4);
    RUN(test_socket_addr_loopback);
    RUN(test_socket_addr_eq);

    /* Net: TCP listener */
    RUN(test_tcp_listener_bind_close);
    RUN(test_tcp_listener_local_addr);
    RUN(test_tcp_listener_poll_accept_pending);
    RUN(test_tcp_listener_null_args);
    RUN(test_tcp_listener_arena_exhaustion);

    /* Net: TCP stream */
    RUN(test_tcp_connect_close);
    RUN(test_tcp_stream_peer_addr);
    RUN(test_tcp_stream_poll_pending);
    RUN(test_tcp_stream_stale_handle);
    RUN(test_net_reset);

    /* App: CLI parsing */
    RUN(test_parse_args_defaults);
    RUN(test_parse_args_doctor);
    RUN(test_parse_args_replay);
    RUN(test_parse_args_verbose);
    RUN(test_parse_args_seed);
    RUN(test_parse_args_help);
    RUN(test_parse_args_null);

    /* App: lifecycle */
    RUN(test_app_init_shutdown);
    RUN(test_app_run_noop);
    RUN(test_app_region);
    RUN(test_app_null_args);

    /* Doctor */
    RUN(test_doctor_initialized_runtime);
    RUN(test_doctor_uninitialized_runtime);
    RUN(test_doctor_null_args);
    RUN(test_doctor_check_fields);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
