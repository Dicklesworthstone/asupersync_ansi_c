/*
 * bench_journal_http.c — what recording a native run costs: an HTTP/1.1
 * keep-alive server with the boundary journal off and on (bd-9kll.11.1,
 * RP1's overhead budget)
 *
 * The RP1 budget: with recording on, at most 10% throughput loss on the HTTP
 * keep-alive bench. This program is that bench. The parent process runs an
 * asx HTTP server on a loopback port in a live POSIX runtime (the platform
 * hooks, real sockets, the reactor); a forked client process holds one
 * keep-alive connection and issues N GET /hello requests back to back,
 * timing them, so the server's work is the only variable between runs. Each
 * round runs the same workload in every mode, in turn:
 *
 *   off    no journal: the hook table is the platform's own.
 *   null   a journal over the hooks (clock, entropy, reactor wait and poll)
 *          and the native adapters (accept, read, write, ...), its sink a
 *          counter that does no I/O: the cost of recording itself, which is
 *          what the budget is about.
 *   file   the same, persisted at chain commit points to a file with fsync
 *          per commit (the POSIX profile's policy). Informational: it adds
 *          the disk. The file is unlinked once opened, so nothing is left.
 *
 * Reported per mode: requests per second (median of the rounds), ns per
 * request, journal records and bytes per request, and the throughput loss
 * against "off". The verdict is on "null"; --gate makes it the exit status.
 *
 * Needs a live POSIX build (PROFILE=POSIX DETERMINISTIC=0); `make
 * bench-journal-overhead` builds one and runs this.
 *
 * Usage: bench_journal_http [--requests N] [--rounds R] [--modes off,null,file]
 *                           [--budget-pct P] [--gate] [--dir DIR]
 * Exit:  0; 1 with --gate when "null" loses more than the budget, or on a
 *        workload failure; 2 when the build has no native sockets.
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <asx/asx.h>
#include <asx/net/http.h>
#include <asx/net/net.h>
#include <asx/net/server.h>
#include <asx/platform/posix.h>
#include <asx/runtime/journal.h>
#include <asx/runtime/runtime.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define RR_RUST_REV "5e60b1c4c"
#define RR_BUILD_TAG "bench-journal-http"

enum { MODE_OFF = 0, MODE_NULL = 1, MODE_FILE = 2, MODE_COUNT = 3 };

static const char *const k_mode_name[MODE_COUNT] = {"off", "null", "file"};

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* The client (a forked process: plain blocking sockets)               */
/* ------------------------------------------------------------------ */

static int dial(uint16_t port) {
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* One GET /hello and its whole response, 1 on a 200 with body "hello". */
static int one_request(int fd) {
    static const char req[] = "GET /hello HTTP/1.1\r\nHost: bench\r\n\r\n";
    char buf[1024];
    size_t n = 0u;
    size_t off = 0u;
    char *end = NULL;
    size_t want = 0u;
    const char *p;

    while (off < sizeof(req) - 1u) {
        ssize_t w = send(fd, req + off, sizeof(req) - 1u - off, MSG_NOSIGNAL);
        if (w <= 0) return 0;
        off += (size_t)w;
    }
    for (;;) {
        ssize_t r;
        if (n >= sizeof(buf) - 1u) return 0;
        r = recv(fd, buf + n, sizeof(buf) - 1u - n, 0);
        if (r <= 0) return 0;
        n += (size_t)r;
        buf[n] = '\0';
        end = strstr(buf, "\r\n\r\n");
        if (end == NULL) continue;
        want = 0u;
        for (p = buf; (p = strstr(p, "\r\n")) != NULL && p < end;) {
            p += 2;
            if (strncasecmp(p, "content-length:", 15u) == 0) {
                want = (size_t)strtoul(p + 15, NULL, 10);
            }
        }
        if (n >= (size_t)(end - buf) + 4u + want) break;
    }
    return strncmp(buf, "HTTP/1.1 200", 12u) == 0 && want == 5u &&
           memcmp(end + 4, "hello", 5u) == 0;
}

/* Warm the connection, then time `n` requests; write the elapsed ns to
 * `out_fd` (0 on a failure). Never returns. */
static void client_main(uint16_t port, uint32_t n, int out_fd) {
    uint64_t elapsed = 0u;
    uint64_t t0;
    uint32_t i;
    int fd;

    (void)signal(SIGPIPE, SIG_IGN);
    fd = dial(port);
    if (fd >= 0) {
        int ok = 1;
        for (i = 0; i < 200u && ok; i++) ok = one_request(fd);
        t0 = now_ns();
        for (i = 0; i < n && ok; i++) ok = one_request(fd);
        if (ok) elapsed = now_ns() - t0;
        (void)close(fd);
    }
    if (write(out_fd, &elapsed, sizeof(elapsed)) != (ssize_t)sizeof(elapsed)) _exit(1);
    _exit(elapsed != 0u ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/* The server and the journal                                          */
/* ------------------------------------------------------------------ */

static asx_server g_srv;
static asx_http_server g_hs;
static asx_http_router g_router;
static int g_shutdown_called;

static asx_status h_hello(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ctx;
    (void)ud;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    return asx_http_body_set_bytes(&resp->body, "hello", 5u);
}

/* Poll the server; once the client has gone, drain and finish. */
static asx_status server_task(void *ud, asx_task_id self) {
    asx_status st;
    (void)ud;
    (void)self;
    st = asx_http_server_poll(&g_hs);
    if (st == ASX_E_PENDING && !g_shutdown_called && g_hs.connections_completed >= 1u &&
        asx_http_server_active_conns(&g_hs) == 0u) {
        g_shutdown_called = 1;
        if (asx_server_shutdown(&g_srv) == ASX_OK) st = asx_http_server_poll(&g_hs);
    }
    return st;
}

#define STAGE_BYTES 65536u
static uint8_t g_stage[STAGE_BYTES];

typedef struct {
    uint64_t bytes;
} null_sink_ctx;

static asx_status null_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    (void)bytes;
    ((null_sink_ctx *)ctx)->bytes += (uint64_t)len;
    return ASX_OK;
}

typedef struct {
    uint64_t elapsed_ns; /* the client's N requests */
    uint64_t records;
    uint64_t bytes;
    uint64_t commits;
} run_result;

static uint32_t g_file_counter;

/* One run of `n` requests in `mode`. 0 on success. */
static int run_once(int mode, uint32_t n, const char *dir, run_result *out) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_journal journal;
    asx_journal_config jcfg;
    asx_journal_stats jst;
    null_sink_ctx nctx;
    asx_posix_journal_file file;
    asx_server_config scfg;
    asx_http_server_config hcfg;
    asx_socket_addr bound;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    asx_status st;
    int pipefd[2];
    pid_t pid;
    uint64_t elapsed = 0u;
    int child_status = 0;
    int journaling = mode != MODE_OFF;

    memset(out, 0, sizeof(*out));
    memset(&nctx, 0, sizeof(nctx));
    memset(&file, 0, sizeof(file));
    file.fd = -1;
    g_shutdown_called = 0;

    st = asx_posix_hooks_install(&hooks);
    if (st == ASX_OK) st = asx_runtime_config_init(&cfg);
    if (st != ASX_OK) return 1;

    if (journaling) {
        memset(&jcfg, 0, sizeof(jcfg));
        jcfg.buffer = g_stage;
        jcfg.buffer_cap = STAGE_BYTES;
        jcfg.config_fingerprint = ASX_CONFIG_FINGERPRINT;
        jcfg.rust_rev_hash = asx_journal_text_hash(RR_RUST_REV);
        jcfg.build_hash = asx_journal_text_hash(RR_BUILD_TAG);
        if (mode == MODE_NULL) {
            jcfg.sink_fn = null_sink;
            jcfg.sink_ctx = &nctx;
        } else {
            char path[512];
            (void)snprintf(path, sizeof(path), "%s/bench_journal_%ld_%u.journal", dir,
                           (long)getpid(), (unsigned)g_file_counter++);
            st = asx_posix_journal_file_open(&file, path);
            if (st != ASX_OK) {
                fprintf(stderr, "bench_journal_http: cannot create %s: %s\n", path,
                        asx_status_str(st));
                return 1;
            }
            /* Nothing is left behind: the file goes with its last close. */
            (void)unlink(path);
            jcfg.sink_fn = asx_posix_journal_file_sink;
            jcfg.sink_ctx = &file;
            jcfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
            jcfg.commit_every_chains = 1u;
            jcfg.commit_fn = asx_posix_journal_file_commit;
        }
        st = asx_journal_init(&journal, &jcfg);
        if (st == ASX_OK) st = asx_journal_wrap_hooks(&journal, &hooks);
        if (st != ASX_OK) {
            asx_journal_uninstall();
            (void)asx_posix_journal_file_close(&file);
            return 1;
        }
    }
    st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) {
        asx_journal_uninstall();
        (void)asx_posix_journal_file_close(&file);
        return 1;
    }
    if (asx_net_get_backend() != ASX_NET_BACKEND_NATIVE) {
        asx_runtime_shutdown(&rt);
        asx_journal_uninstall();
        (void)asx_posix_journal_file_close(&file);
        return 2;
    }

    asx_http_router_init(&g_router);
    st = asx_http_router_add_route(&g_router, ASX_HTTP_GET, "/hello", h_hello, NULL, NULL, NULL);
    asx_server_config_init(&scfg);
    scfg.listen_addr = asx_socket_addr_loopback(0u);
    scfg.drain_timeout_ms = 1000u;
    asx_server_init(&g_srv, &scfg);
    if (st == ASX_OK) st = asx_server_listen(&g_srv);
    if (st == ASX_OK) st = asx_tcp_listener_local_addr(g_srv.listener, &bound);
    asx_http_server_config_init(&hcfg, &g_router);
    hcfg.idle_timeout_ms = 0u;
    if (st == ASX_OK) st = asx_http_server_init(&g_hs, &g_srv, &hcfg);
    if (st == ASX_OK) st = asx_region_open(&region);
    if (st == ASX_OK) st = asx_task_spawn(region, server_task, NULL, &task);
    if (st != ASX_OK || pipe(pipefd) != 0) {
        asx_runtime_shutdown(&rt);
        asx_journal_uninstall();
        (void)asx_posix_journal_file_close(&file);
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        asx_runtime_shutdown(&rt);
        asx_journal_uninstall();
        (void)asx_posix_journal_file_close(&file);
        return 1;
    }
    if (pid == 0) {
        (void)close(pipefd[0]);
        client_main(bound.port, n, pipefd[1]);
    }
    (void)close(pipefd[1]);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(region, &budget);
    if (read(pipefd[0], &elapsed, sizeof(elapsed)) != (ssize_t)sizeof(elapsed)) elapsed = 0u;
    (void)close(pipefd[0]);
    (void)waitpid(pid, &child_status, 0);

    asx_runtime_shutdown(&rt);
    asx_journal_uninstall();
    if (journaling) {
        asx_status js = asx_journal_finish(&journal);
        asx_journal_get_stats(&journal, &jst);
        out->records = jst.records;
        out->bytes = jst.bytes;
        out->commits = jst.commits;
        (void)asx_posix_journal_file_close(&file);
        /* A journal that failed (a sink error) recorded less than it
         * should: its timing is not the recording's. */
        if (js != ASX_OK) return 1;
    }
    out->elapsed_ns = elapsed;
    if (st != ASX_OK || elapsed == 0u) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rounds and the report                                               */
/* ------------------------------------------------------------------ */

#define MAX_ROUNDS 64u

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t median(uint64_t *v, uint32_t n) {
    qsort(v, (size_t)n, sizeof(uint64_t), cmp_u64);
    return n == 0u ? 0u : v[n / 2u];
}

int main(int argc, char **argv) {
    uint32_t requests = 20000u;
    uint32_t rounds = 7u;
    unsigned modes_mask = (1u << MODE_OFF) | (1u << MODE_NULL);
    double budget_pct = 10.0;
    int gate = 0;
    const char *dir = "/tmp";
    static uint64_t ns[MODE_COUNT][MAX_ROUNDS];
    static uint64_t rec[MODE_COUNT][MAX_ROUNDS];
    static uint64_t byt[MODE_COUNT][MAX_ROUNDS];
    uint32_t done[MODE_COUNT];
    uint64_t med_ns[MODE_COUNT];
    uint32_t r;
    int m;
    int i;
    int failures = 0;
    double loss_null = 0.0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gate") == 0) {
            gate = 1;
        } else if (i + 1 < argc && strcmp(argv[i], "--requests") == 0) {
            requests = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (i + 1 < argc && strcmp(argv[i], "--rounds") == 0) {
            rounds = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (i + 1 < argc && strcmp(argv[i], "--budget-pct") == 0) {
            budget_pct = strtod(argv[++i], NULL);
        } else if (i + 1 < argc && strcmp(argv[i], "--dir") == 0) {
            dir = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--modes") == 0) {
            const char *s = argv[++i];
            modes_mask = 0u;
            for (m = 0; m < MODE_COUNT; m++) {
                if (strstr(s, k_mode_name[m]) != NULL) modes_mask |= 1u << m;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--requests N] [--rounds R] [--modes off,null,file] "
                    "[--budget-pct P] [--gate] [--dir DIR]\n",
                    argv[0]);
            return 2;
        }
    }
    if (rounds == 0u) rounds = 1u;
    if (rounds > MAX_ROUNDS) rounds = MAX_ROUNDS;
    if (requests == 0u) requests = 1u;
    modes_mask |= 1u << MODE_OFF; /* the baseline */

    printf("bench_journal_http: %u requests per run, %u rounds, interleaved\n", (unsigned)requests,
           (unsigned)rounds);
    memset(done, 0, sizeof(done));
    for (r = 0; r < rounds; r++) {
        for (m = 0; m < MODE_COUNT; m++) {
            run_result res;
            int rc;
            if ((modes_mask & (1u << m)) == 0u) continue;
            rc = run_once(m, requests, dir, &res);
            if (rc == 2) {
                fprintf(stderr, "bench_journal_http: needs a live POSIX build (native sockets)\n");
                return 2;
            }
            if (rc != 0) {
                fprintf(stderr, "bench_journal_http: round %u mode %s failed\n", (unsigned)r,
                        k_mode_name[m]);
                failures++;
                continue;
            }
            ns[m][done[m]] = res.elapsed_ns;
            rec[m][done[m]] = res.records;
            byt[m][done[m]] = res.bytes;
            done[m]++;
        }
    }

    printf("\n%-5s %12s %12s %14s %14s %10s\n", "mode", "requests/s", "ns/request", "records/req",
           "bytes/req", "loss vs off");
    for (m = 0; m < MODE_COUNT; m++) {
        double rps;
        double loss;
        double nsreq;
        if ((modes_mask & (1u << m)) == 0u || done[m] == 0u) continue;
        med_ns[m] = median(ns[m], done[m]);
        nsreq = (double)med_ns[m] / (double)requests;
        rps = 1e9 / nsreq;
        loss = m == MODE_OFF ? 0.0 : (1.0 - ((double)med_ns[MODE_OFF] / (double)med_ns[m])) * 100.0;
        if (m == MODE_NULL) loss_null = loss;
        printf("%-5s %12.0f %12.0f %14.2f %14.1f %9.2f%%\n", k_mode_name[m], rps, nsreq,
               m == MODE_OFF ? 0.0 : (double)median(rec[m], done[m]) / (double)requests,
               m == MODE_OFF ? 0.0 : (double)median(byt[m], done[m]) / (double)requests, loss);
    }
    if (done[MODE_OFF] == 0u || ((modes_mask & (1u << MODE_NULL)) != 0u && done[MODE_NULL] == 0u)) {
        fprintf(stderr, "bench_journal_http: no complete rounds\n");
        return 1;
    }
    if ((modes_mask & (1u << MODE_NULL)) != 0u) {
        printf("\nRP1 budget: recording costs at most %.1f%% of throughput (mode null): %.2f%% -> "
               "%s\n",
               budget_pct, loss_null, loss_null <= budget_pct ? "WITHIN BUDGET" : "OVER BUDGET");
        if (gate && loss_null > budget_pct) failures++;
    }
    return failures != 0 ? 1 : 0;
}
