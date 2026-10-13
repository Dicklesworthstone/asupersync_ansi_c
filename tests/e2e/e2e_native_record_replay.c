/*
 * e2e_native_record_replay.c — RP3: a real native HTTP server run, recorded
 * to a journal and replayed without sockets (bd-9kll.11.3)
 *
 * One binary, five modes (and a sixth in a -DASX_TEST_RACE build), driven
 * by native_record_replay.sh (record, load, replay, and the planted race)
 * and native_crash_continuity.sh (record-crash, load, replay-prefix). Built
 * against a live POSIX library (DETERMINISTIC=0).
 *
 *   record JOURNAL
 *       Run the HTTP server (GET /hello, POST /echo) on a loopback port with
 *       the platform hooks wrapped by a journal that streams to JOURNAL. It
 *       serves until SIGTERM, then drains and exits. Prints "LISTENING
 *       <port>" once bound, "DRAIN" when the drain starts, and at exit
 *       "STATS served=N accepted=N ...", "DIGEST <hex>" (the trace digest of
 *       the run) and "JOURNAL <bytes>".
 *
 *   load PORT
 *       A small deterministic client: a keep-alive connection with two
 *       requests, a mid-request disconnect, a connection reset (RST) in the
 *       middle of a request, and a keep-alive connection held open until the
 *       drain closes it. Prints "READY" when every client but the last is
 *       done (the script then sends SIGTERM) and "DRAINED" at the end.
 *
 *   replay JOURNAL DIGEST SERVED [RUST_REV]
 *       Run the same server program under asx_journal_replay(): no socket is
 *       opened (the adapters' real-call counter is checked), and the trace
 *       digest and the number of requests served must equal the recorded
 *       ones. Prints "REPLAY_DIGEST", "REPLAY_SERVED", "REAL_NET_CALLS" and
 *       "REPLAY_OK", or "REPLAY_REFUSED <error> seq=N kind=K what=...".
 *
 *   record-crash JOURNAL CKPTLOG KILL_AT      (bd-9kll.11.4, RP4)
 *       As record, but the journal persists at chain commit points to
 *       JOURNAL through the POSIX file store (write, fsync per commit), a
 *       chain every 8 records, and each commit appends the checkpoint it
 *       carries ("CKPT seq=.. records=.. bytes=.. valid=.. events=..
 *       digest=..") to CKPTLOG. When the stream reaches KILL_AT bytes the
 *       process writes what precedes that byte and kills itself with
 *       SIGKILL: no flush, no finish, a torn tail wherever the offset
 *       falls. KILL_AT 0 never kills.
 *
 *   replay-prefix JOURNAL [RUST_REV]
 *       Run the same server program under asx_journal_replay_prefix(): the
 *       committed prefix of a cut journal is replayed with no socket opened
 *       and every trace checkpoint verified, ending at the cut. Prints
 *       "PREFIX complete=.. bytes=.. records=.. cut_seq=.. reason=..",
 *       "PREFIX_PROGRESS served=.. checkpoints=.. seq=.. events=..
 *       digest=..", "REAL_NET_CALLS" and "REPLAY_CUT seq=N" (or, for a
 *       complete journal, "REPLAY_DIGEST" and "REPLAY_OK"; else
 *       "REPLAY_REFUSED ...").
 *
 *   -DASX_TEST_RACE                              (bd-9kll.11.3, step 5)
 *       Plants an order dependence in the server program: POST /register
 *       sets a flag and GET /lookup answers 500 when the flag is not set.
 *       "load-race PORT" runs one client of each, started together, then the
 *       held connection of "load"; whether the lookup arrives first is the
 *       kernel's accept order. "load-race PORT serial" runs them one after
 *       the other, register first: the order the program assumes, which
 *       never triggers it. record prints "RACE hits=N events=E
 *       digest=D" (lookups served before the register; the trace's event
 *       count and digest at the first), replay prints "REPLAY_RACE" with the
 *       same three, which a replay of a recording that triggered it must
 *       reproduce exactly.
 *
 * Exit status: 0 on success, 1 on a failure, 2 on a usage error.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "../../src/net/net_native.h"
#include <arpa/inet.h>
#include <asx/asx.h>
#include <asx/net/http.h>
#include <asx/net/server.h>
#include <asx/platform/posix.h>
#include <asx/runtime/journal.h>
#include <asx/runtime/journal_replay.h>
#include <asx/runtime/trace.h>
#include <asx/signal/signal.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#ifdef ASX_TEST_RACE
#include <pthread.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* The pinned asupersync revision the journal header names, and the build
 * tag; replay passes both so the identity check is part of the proof. */
#define RR_RUST_REV "5e60b1c4c"
#define RR_BUILD_TAG "e2e-native-record-replay"

/* ------------------------------------------------------------------ */
/* The server program (the same function records and replays)          */
/* ------------------------------------------------------------------ */

typedef struct {
    int announce; /* record: print LISTENING and DRAIN; replay: stay quiet */
    uint64_t digest;
    uint32_t served;
    uint32_t accepted;
    uint32_t timed_out;
    uint32_t drain_closed;
    uint32_t rejected;
    uint32_t race_hits;   /* -DASX_TEST_RACE: lookups served before the register */
    uint32_t race_events; /* the trace's event count at the first one */
    uint64_t race_digest; /* the trace digest at the first one */
    asx_status run_status;
} serve_ctx;

static asx_server g_srv;
static asx_http_server g_hs;
static asx_http_router g_router;
static asx_signal_subscription g_sub;
static asx_task_id g_server_task;
static int g_announce;

#ifdef ASX_TEST_RACE
/* A planted order dependence (bd-9kll.11.3, step 5): GET /lookup assumes
 * POST /register has been served and answers 500 when it has not. Which of
 * the two a concurrent pair of clients gets in first is up to the kernel's
 * accept order, so a recording may or may not trigger it; the state the
 * handler reads is the program's own, so a replay of a recording that
 * triggered it must trigger it at the same trace position, every time. */
static uint32_t g_registered;
static uint32_t g_race_hits;
static uint32_t g_race_events;
static uint64_t g_race_digest;

static asx_status h_register(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ctx;
    (void)ud;
    g_registered = 1u;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    return asx_http_body_set_bytes(&resp->body, "ok", 2u);
}

static asx_status h_lookup(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ctx;
    (void)ud;
    if (g_registered != 0u) {
        asx_http_response_init(resp, ASX_HTTP_200_OK);
        return asx_http_body_set_bytes(&resp->body, "found", 5u);
    }
    if (g_race_hits++ == 0u) {
        g_race_events = asx_trace_event_count();
        g_race_digest = asx_trace_digest();
    }
    asx_http_response_init(resp, ASX_HTTP_500_INTERNAL_ERROR);
    return asx_http_body_set_bytes(&resp->body, "race", 4u);
}
#endif

static asx_status h_hello(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ctx;
    (void)ud;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    return asx_http_body_set_bytes(&resp->body, "hello", 5u);
}

static asx_status h_echo(asx_http_request_context *ctx, asx_http_response *resp, void *ud) {
    (void)ud;
    asx_http_response_init(resp, ASX_HTTP_200_OK);
    return asx_http_body_set_bytes(&resp->body, ctx->request->body.data, ctx->request->body.len);
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
    if (g_announce) {
        printf("DRAIN\n");
        fflush(stdout);
    }
    return asx_task_wake(g_server_task);
}

static asx_status serve_program(void *obs) {
    serve_ctx *c = (serve_ctx *)obs;
    asx_server_config scfg;
    asx_http_server_config hcfg;
    asx_socket_addr bound;
    asx_region_id region;
    asx_task_id sig_task;
    asx_budget budget;
    asx_status st;

    g_announce = c->announce;
    asx_http_router_init(&g_router);
    st = asx_http_router_add_route(&g_router, ASX_HTTP_GET, "/hello", h_hello, NULL, NULL, NULL);
    if (st != ASX_OK) return st;
    st = asx_http_router_add_route(&g_router, ASX_HTTP_POST, "/echo", h_echo, NULL, NULL, NULL);
    if (st != ASX_OK) return st;
#ifdef ASX_TEST_RACE
    g_registered = 0u;
    g_race_hits = 0u;
    g_race_events = 0u;
    g_race_digest = 0u;
    st = asx_http_router_add_route(&g_router, ASX_HTTP_POST, "/register", h_register, NULL, NULL,
                                   NULL);
    if (st != ASX_OK) return st;
    st = asx_http_router_add_route(&g_router, ASX_HTTP_GET, "/lookup", h_lookup, NULL, NULL, NULL);
    if (st != ASX_OK) return st;
#endif

    asx_server_config_init(&scfg);
    scfg.listen_addr = asx_socket_addr_loopback(0u);
    scfg.drain_timeout_ms = 3000u;
    asx_server_init(&g_srv, &scfg);
    st = asx_server_listen(&g_srv);
    if (st != ASX_OK) return st;
    st = asx_tcp_listener_local_addr(g_srv.listener, &bound);
    if (st != ASX_OK) return st;

    asx_http_server_config_init(&hcfg, &g_router);
    hcfg.idle_timeout_ms = 0u; /* no timers: the run is driven by its I/O */
    st = asx_http_server_init(&g_hs, &g_srv, &hcfg);
    if (st != ASX_OK) return st;
    st = asx_signal_subscribe(&g_sub, ASX_SIGNAL_TERM);
    if (st != ASX_OK) return st;

    st = asx_region_open(&region);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(region, server_task, NULL, &g_server_task);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(region, signal_task, NULL, &sig_task);
    if (st != ASX_OK) return st;

    if (c->announce) {
        printf("LISTENING %u\n", (unsigned)bound.port);
        fflush(stdout);
    }
    budget = asx_budget_infinite();
    c->run_status = asx_scheduler_run(region, &budget);

    c->digest = asx_trace_digest();
    c->served = asx_http_server_requests_served(&g_hs);
    c->accepted = asx_server_total_accepted(&g_srv);
    c->timed_out = g_hs.connections_timed_out;
    c->drain_closed = g_hs.connections_drain_closed;
    c->rejected = asx_server_total_rejected(&g_srv);
#ifdef ASX_TEST_RACE
    c->race_hits = g_race_hits;
    c->race_events = g_race_events;
    c->race_digest = g_race_digest;
#endif
    st = asx_signal_unsubscribe(g_sub);
    if (c->run_status != ASX_OK) return c->run_status;
    return st;
}

/* ------------------------------------------------------------------ */
/* record                                                              */
/* ------------------------------------------------------------------ */

#define STAGE_BYTES 65536u

static uint8_t g_stage[STAGE_BYTES];

static asx_status file_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    return fwrite(bytes, 1u, (size_t)len, (FILE *)ctx) == (size_t)len ? ASX_OK
                                                                      : ASX_E_RESOURCE_EXHAUSTED;
}

static int fail(const char *what, asx_status st) {
    fprintf(stderr, "e2e_native_record_replay: %s: %s\n", what, asx_status_str(st));
    return 1;
}

static int run_record(const char *journal_path) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_journal journal;
    asx_journal_config jcfg;
    serve_ctx ctx;
    asx_status st;
    asx_status js;
    FILE *jf;
    long bytes;

    jf = fopen(journal_path, "wb");
    if (jf == NULL) {
        fprintf(stderr, "e2e_native_record_replay: cannot open %s\n", journal_path);
        return 1;
    }
    memset(&jcfg, 0, sizeof(jcfg));
    jcfg.sink_fn = file_sink;
    jcfg.sink_ctx = jf;
    jcfg.buffer = g_stage;
    jcfg.buffer_cap = STAGE_BYTES;
    jcfg.config_fingerprint = ASX_CONFIG_FINGERPRINT;
    jcfg.rust_rev_hash = asx_journal_text_hash(RR_RUST_REV);
    jcfg.build_hash = asx_journal_text_hash(RR_BUILD_TAG);

    st = asx_posix_hooks_install(&hooks);
    if (st == ASX_OK) st = asx_runtime_config_init(&cfg);
    if (st == ASX_OK) st = asx_journal_init(&journal, &jcfg);
    if (st == ASX_OK) st = asx_journal_wrap_hooks(&journal, &hooks);
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) {
        asx_journal_uninstall();
        (void)fclose(jf);
        return fail("journal and runtime setup", st);
    }
    if (asx_net_get_backend() != ASX_NET_BACKEND_NATIVE) {
        fprintf(stderr, "e2e_native_record_replay: needs a live POSIX build (native sockets)\n");
        asx_runtime_shutdown(&rt);
        asx_journal_uninstall();
        (void)fclose(jf);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.announce = 1;
    st = serve_program(&ctx);
    asx_runtime_shutdown(&rt);
    asx_journal_uninstall();
    js = asx_journal_finish(&journal);
    bytes = ftell(jf);
    if (fclose(jf) != 0 && js == ASX_OK) js = ASX_E_RESOURCE_EXHAUSTED;

    printf("STATS served=%u accepted=%u timed_out=%u drain_closed=%u rejected=%u\n",
           (unsigned)ctx.served, (unsigned)ctx.accepted, (unsigned)ctx.timed_out,
           (unsigned)ctx.drain_closed, (unsigned)ctx.rejected);
    printf("DIGEST %016llx\n", (unsigned long long)ctx.digest);
#ifdef ASX_TEST_RACE
    printf("RACE hits=%u events=%u digest=%016llx\n", (unsigned)ctx.race_hits,
           (unsigned)ctx.race_events, (unsigned long long)ctx.race_digest);
#endif
    printf("JOURNAL %ld\n", bytes);
    fflush(stdout);
    if (st != ASX_OK) return fail("server run", st);
    if (js != ASX_OK) return fail("journal", js);
    return 0;
}

/* ------------------------------------------------------------------ */
/* record-crash: persist at chains, die by SIGKILL at a stream offset  */
/* ------------------------------------------------------------------ */

/* Small enough that a window is often handed over in pieces, so a kill
 * lands mid-window as often as at a commit point. */
#define CRASH_STAGE_BYTES 1024u
#define CRASH_CHAIN_EVERY 8u

typedef struct {
    asx_posix_journal_file file;
    int ckpt_fd;
    uint64_t kill_at; /* the stream offset the process dies at; 0 = never */
    uint64_t offered; /* stream bytes handed to the sink so far */
} crash_store;

/* The file store's sink, except that the process is killed when the stream
 * reaches kill_at: what precedes that byte is written, nothing after it
 * exists, and there is no chance to flush or finish. */
static asx_status crash_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    crash_store *s = (crash_store *)ctx;
    if (s->kill_at != 0u && s->offered + (uint64_t)len >= s->kill_at) {
        uint32_t keep = (uint32_t)(s->kill_at - s->offered);
        if (keep > 0u) (void)asx_posix_journal_file_sink(&s->file, bytes, keep);
        (void)kill(getpid(), SIGKILL);
        _exit(137); /* not reached: SIGKILL cannot be caught */
    }
    s->offered += (uint64_t)len;
    return asx_posix_journal_file_sink(&s->file, bytes, len);
}

/* fsync the prefix, then log the checkpoint the commit carries: the
 * independent record of what the stream's chains say, written by the
 * recorder itself as it goes. */
static asx_status crash_commit(void *ctx, const asx_journal_commit *c) {
    crash_store *s = (crash_store *)ctx;
    char line[192];
    int n;
    asx_status st = asx_posix_journal_file_commit(&s->file, c);
    if (st != ASX_OK) return st;
    n = snprintf(line, sizeof(line),
                 "CKPT seq=%llu records=%llu bytes=%llu valid=%d events=%llu digest=%016llx "
                 "final=%d\n",
                 (unsigned long long)c->chain_seq, (unsigned long long)c->data_records,
                 (unsigned long long)c->stream_bytes, c->trace_valid,
                 (unsigned long long)c->trace_events, (unsigned long long)c->trace_digest,
                 c->final);
    if (n <= 0 || (size_t)n >= sizeof(line)) return ASX_E_BUFFER_TOO_SMALL;
    if (write(s->ckpt_fd, line, (size_t)n) != (ssize_t)n) return ASX_E_RESOURCE_EXHAUSTED;
    return ASX_OK;
}

static uint8_t g_crash_stage[CRASH_STAGE_BYTES];

static int run_record_crash(const char *journal_path, const char *ckpt_path,
                            const char *kill_text) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_journal journal;
    asx_journal_config jcfg;
    asx_journal_stats jst;
    crash_store store;
    serve_ctx ctx;
    asx_status st;
    asx_status js;

    memset(&store, 0, sizeof(store));
    store.kill_at = strtoull(kill_text, NULL, 10);
    store.ckpt_fd = open(ckpt_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (store.ckpt_fd < 0) {
        fprintf(stderr, "e2e_native_record_replay: cannot create %s\n", ckpt_path);
        return 1;
    }
    st = asx_posix_journal_file_open(&store.file, journal_path);
    if (st != ASX_OK) return fail("journal file", st);

    memset(&jcfg, 0, sizeof(jcfg));
    jcfg.sink_fn = crash_sink;
    jcfg.sink_ctx = &store;
    jcfg.buffer = g_crash_stage;
    jcfg.buffer_cap = CRASH_STAGE_BYTES;
    jcfg.chain_every = CRASH_CHAIN_EVERY;
    jcfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
    jcfg.commit_every_chains = 1u;
    jcfg.commit_fn = crash_commit;
    jcfg.config_fingerprint = ASX_CONFIG_FINGERPRINT;
    jcfg.rust_rev_hash = asx_journal_text_hash(RR_RUST_REV);
    jcfg.build_hash = asx_journal_text_hash(RR_BUILD_TAG);

    st = asx_posix_hooks_install(&hooks);
    if (st == ASX_OK) st = asx_runtime_config_init(&cfg);
    if (st == ASX_OK) st = asx_journal_init(&journal, &jcfg);
    if (st == ASX_OK) st = asx_journal_wrap_hooks(&journal, &hooks);
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) {
        asx_journal_uninstall();
        return fail("journal and runtime setup", st);
    }
    if (asx_net_get_backend() != ASX_NET_BACKEND_NATIVE) {
        fprintf(stderr, "e2e_native_record_replay: needs a live POSIX build (native sockets)\n");
        asx_runtime_shutdown(&rt);
        asx_journal_uninstall();
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.announce = 1;
    st = serve_program(&ctx);
    asx_runtime_shutdown(&rt);
    asx_journal_uninstall();
    js = asx_journal_finish(&journal);
    asx_journal_get_stats(&journal, &jst);
    (void)asx_posix_journal_file_close(&store.file);
    (void)close(store.ckpt_fd);

    printf("STATS served=%u accepted=%u timed_out=%u drain_closed=%u rejected=%u\n",
           (unsigned)ctx.served, (unsigned)ctx.accepted, (unsigned)ctx.timed_out,
           (unsigned)ctx.drain_closed, (unsigned)ctx.rejected);
    printf("DIGEST %016llx\n", (unsigned long long)ctx.digest);
    printf("JOURNAL %llu commits=%llu fsyncs=%llu\n", (unsigned long long)store.file.bytes,
           (unsigned long long)jst.commits, (unsigned long long)store.file.syncs);
    fflush(stdout);
    if (st != ASX_OK) return fail("server run", st);
    if (js != ASX_OK) return fail("journal", js);
    return 0;
}

/* ------------------------------------------------------------------ */
/* replay                                                              */
/* ------------------------------------------------------------------ */

static const char *error_name(asx_replay_error e) {
    switch (e) {
    case ASX_REPLAY_OK: return "OK";
    case ASX_REPLAY_E_BUILD_MISMATCH: return "BUILD_MISMATCH";
    case ASX_REPLAY_E_INCOMPLETE: return "INCOMPLETE";
    case ASX_REPLAY_E_CORRUPT: return "CORRUPT";
    case ASX_REPLAY_E_UNSUPPORTED: return "UNSUPPORTED";
    case ASX_REPLAY_E_DIVERGED: return "DIVERGED";
    case ASX_REPLAY_E_EXHAUSTED: return "EXHAUSTED";
    case ASX_REPLAY_E_RUNTIME: return "RUNTIME";
    case ASX_REPLAY_E_CUT: return "CUT";
    default: return "?";
    }
}

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    if (f == NULL) return NULL;
    size = fseek(f, 0L, SEEK_END) == 0 ? ftell(f) : -1L;
    if (size < 0 || fseek(f, 0L, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1u);
    if (buf == NULL || fread(buf, 1u, (size_t)size, f) != (size_t)size) {
        free(buf);
        (void)fclose(f);
        return NULL;
    }
    (void)fclose(f);
    *out_len = (size_t)size;
    return buf;
}

static int run_replay(const char *journal_path, const char *digest_hex, const char *served_text,
                      const char *rust_rev) {
    size_t len = 0u;
    uint8_t *bytes = read_file(journal_path, &len);
    asx_replay_identity id;
    asx_replay_report rep;
    asx_replay_error err;
    asx_status ps = ASX_OK;
    serve_ctx ctx;
    unsigned long long want_digest = strtoull(digest_hex, NULL, 16);
    unsigned long want_served = strtoul(served_text, NULL, 10);
    uint32_t calls_before;
    uint32_t calls;

    if (bytes == NULL) {
        fprintf(stderr, "e2e_native_record_replay: cannot read %s\n", journal_path);
        return 1;
    }
    id.rust_rev_hash = asx_journal_text_hash(rust_rev != NULL ? rust_rev : RR_RUST_REV);
    id.build_hash = asx_journal_text_hash(RR_BUILD_TAG);
    memset(&ctx, 0, sizeof(ctx));
    memset(&rep, 0, sizeof(rep));
    ctx.announce = 0;

    calls_before = asx_native_net_real_calls();
    err = asx_journal_replay(bytes, len, &id, NULL, serve_program, &ctx, &ps, &rep);
    calls = asx_native_net_real_calls() - calls_before;
    free(bytes);

    printf("REPLAY_DIGEST %016llx\n", (unsigned long long)ctx.digest);
    printf("REPLAY_SERVED %u\n", (unsigned)ctx.served);
#ifdef ASX_TEST_RACE
    printf("REPLAY_RACE hits=%u events=%u digest=%016llx\n", (unsigned)ctx.race_hits,
           (unsigned)ctx.race_events, (unsigned long long)ctx.race_digest);
#endif
    printf("REAL_NET_CALLS %u\n", (unsigned)calls);
    if (err != ASX_REPLAY_OK) {
        printf("REPLAY_REFUSED %s seq=%llu kind=%u what=%s program_status=%s\n", error_name(err),
               (unsigned long long)rep.seq, (unsigned)rep.expected_kind,
               rep.what != NULL ? rep.what : "", asx_status_str(ps));
        fflush(stdout);
        return 1;
    }
    if ((unsigned long long)ctx.digest != want_digest || (unsigned long)ctx.served != want_served ||
        calls != 0u || ps != ASX_OK) {
        printf("REPLAY_MISMATCH digest=%s served=%s real_calls=%u program_status=%s\n",
               (unsigned long long)ctx.digest == want_digest ? "equal" : "DIFFERENT",
               (unsigned long)ctx.served == want_served ? "equal" : "DIFFERENT", (unsigned)calls,
               asx_status_str(ps));
        fflush(stdout);
        return 1;
    }
    printf("REPLAY_OK\n");
    fflush(stdout);
    return 0;
}

/* Replay the committed prefix of a journal that may have been cut: the
 * server program runs against it with no socket opened, every committed
 * record is reproduced and every trace checkpoint on the way verified, and
 * the run ends at the cut with its seq. Exit 0 for a cut or a complete
 * stream consumed exactly, with no native network call made. */
static int run_replay_prefix(const char *journal_path, const char *rust_rev) {
    size_t len = 0u;
    uint8_t *bytes = read_file(journal_path, &len);
    asx_replay_identity id;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_replay_error err;
    asx_status ps = ASX_OK;
    serve_ctx ctx;
    uint32_t calls_before;
    uint32_t calls;

    if (bytes == NULL) {
        fprintf(stderr, "e2e_native_record_replay: cannot read %s\n", journal_path);
        return 1;
    }
    id.rust_rev_hash = asx_journal_text_hash(rust_rev != NULL ? rust_rev : RR_RUST_REV);
    id.build_hash = asx_journal_text_hash(RR_BUILD_TAG);
    memset(&ctx, 0, sizeof(ctx));
    memset(&rep, 0, sizeof(rep));
    memset(&pfx, 0, sizeof(pfx));
    ctx.announce = 0;

    calls_before = asx_native_net_real_calls();
    err = asx_journal_replay_prefix(bytes, len, &id, NULL, serve_program, &ctx, &ps, &rep, &pfx);
    calls = asx_native_net_real_calls() - calls_before;
    free(bytes);

    printf("PREFIX complete=%d bytes=%llu records=%llu cut_seq=%llu reason=%s stream=%llu\n",
           pfx.complete, (unsigned long long)pfx.committed_bytes,
           (unsigned long long)pfx.committed_records, (unsigned long long)pfx.cut_seq,
           error_name(pfx.cut_reason), (unsigned long long)pfx.stream_bytes);
    printf("PREFIX_PROGRESS served=%llu checkpoints=%llu seq=%llu events=%llu digest=%016llx\n",
           (unsigned long long)pfx.served, (unsigned long long)pfx.checkpoints,
           (unsigned long long)pfx.checkpoint_seq, (unsigned long long)pfx.checkpoint_events,
           (unsigned long long)pfx.checkpoint_digest);
    printf("REAL_NET_CALLS %u\n", (unsigned)calls);
    if (err == ASX_REPLAY_E_CUT) {
        printf("REPLAY_CUT seq=%llu what=%s program_status=%s\n", (unsigned long long)rep.seq,
               rep.what != NULL ? rep.what : "", asx_status_str(ps));
    } else if (err == ASX_REPLAY_OK) {
        printf("REPLAY_DIGEST %016llx\n", (unsigned long long)ctx.digest);
        printf("REPLAY_SERVED %u\n", (unsigned)ctx.served);
        printf("REPLAY_OK\n");
    } else {
        printf("REPLAY_REFUSED %s seq=%llu kind=%u what=%s program_status=%s\n", error_name(err),
               (unsigned long long)rep.seq, (unsigned)rep.expected_kind,
               rep.what != NULL ? rep.what : "", asx_status_str(ps));
    }
    fflush(stdout);
    return (err == ASX_REPLAY_E_CUT || err == ASX_REPLAY_OK) && calls == 0u ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* load: the clients                                                   */
/* ------------------------------------------------------------------ */

static int dial(uint16_t port, int timeout_s) {
    struct sockaddr_in a;
    struct timeval tv;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *s) {
    size_t n = strlen(s);
    size_t off = 0u;
    while (off < n) {
        ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return 0;
        off += (size_t)w;
    }
    return 1;
}

/* Read one response: its status and body (at most cap - 1 bytes, NUL
 * terminated). 1 on success. */
static int read_response(int fd, int *status, char *body, size_t cap) {
    char head[2048];
    size_t n = 0u;
    size_t want = 0u;
    size_t got = 0u;
    const char *p;
    int complete = 0;

    while (n < sizeof(head) - 1u) {
        ssize_t r = recv(fd, head + n, 1u, 0);
        if (r <= 0) return 0;
        n += (size_t)r;
        head[n] = '\0';
        if (n >= 4u && memcmp(head + n - 4u, "\r\n\r\n", 4u) == 0) {
            complete = 1;
            break;
        }
    }
    if (!complete) return 0;
    if (sscanf(head, "HTTP/%*d.%*d %d", status) != 1) return 0;
    for (p = head; (p = strstr(p, "\r\n")) != NULL;) {
        p += 2;
        if (strncasecmp(p, "content-length:", 15u) == 0) want = (size_t)strtoul(p + 15, NULL, 10);
    }
    if (want >= cap) return 0;
    while (got < want) {
        ssize_t r = recv(fd, body + got, want - got, 0);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    body[got] = '\0';
    return 1;
}

static int expect_get(int fd, const char *what) {
    char body[64];
    int status = 0;
    if (!send_all(fd, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n") ||
        !read_response(fd, &status, body, sizeof(body)) || status != 200 ||
        strcmp(body, "hello") != 0) {
        printf("LOAD_FAIL %s: GET /hello\n", what);
        return 0;
    }
    return 1;
}

/* A keep-alive connection held open: the SIGTERM drain closes it. Prints
 * READY once its request is answered (the script then sends SIGTERM) and
 * DRAINED when the server closes it. 0 on success. */
static int hold_until_drained(uint16_t port) {
    int held = dial(port, 10);
    char b;
    ssize_t r;

    if (held < 0) {
        printf("LOAD_FAIL D: connect\n");
        return 1;
    }
    if (!expect_get(held, "D")) return 1;
    printf("READY\n");
    fflush(stdout);
    r = recv(held, &b, 1u, 0);
    if (r != 0) {
        printf("LOAD_FAIL D: the drain did not close the idle connection (recv=%ld errno=%d)\n",
               (long)r, errno);
        return 1;
    }
    (void)close(held);
    printf("DRAINED\n");
    fflush(stdout);
    return 0;
}

static int run_load(uint16_t port) {
    char body[64];
    int status = 0;
    int fd;
    struct linger lg;

    (void)signal(SIGPIPE, SIG_IGN);

    /* A: a keep-alive connection, two requests, then a normal close. */
    fd = dial(port, 5);
    if (fd < 0) {
        printf("LOAD_FAIL A: connect\n");
        return 1;
    }
    if (!expect_get(fd, "A")) return 1;
    if (!send_all(fd, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 9\r\n\r\nping-pong") ||
        !read_response(fd, &status, body, sizeof(body)) || status != 200 ||
        strcmp(body, "ping-pong") != 0) {
        printf("LOAD_FAIL A: POST /echo\n");
        return 1;
    }
    (void)close(fd);

    /* B: a mid-request disconnect: half a request line, then FIN. */
    fd = dial(port, 5);
    if (fd < 0) {
        printf("LOAD_FAIL B: connect\n");
        return 1;
    }
    if (!send_all(fd, "GET /hel")) {
        printf("LOAD_FAIL B: send\n");
        return 1;
    }
    (void)close(fd);

    /* C: a request answered, then a connection reset in the middle of the
     * next one: SO_LINGER {1,0} makes close() send RST. */
    fd = dial(port, 5);
    if (fd < 0) {
        printf("LOAD_FAIL C: connect\n");
        return 1;
    }
    if (!expect_get(fd, "C")) return 1;
    if (!send_all(fd, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\npart")) {
        printf("LOAD_FAIL C: send\n");
        return 1;
    }
    lg.l_onoff = 1;
    lg.l_linger = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    (void)close(fd);

    /* D: a keep-alive connection held open: the SIGTERM drain closes it. */
    return hold_until_drained(port);
}

#ifdef ASX_TEST_RACE
/* The racing pair of -DASX_TEST_RACE: one client POSTs /register, the other
 * GETs /lookup, started together. Both dial after a rendezvous and send after
 * a second one, so the order the server sees them in is the kernel's accept
 * order. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int arrived;
    int parties;
    int generation;
} rendezvous;

static void rendezvous_wait(rendezvous *r) {
    int gen;
    (void)pthread_mutex_lock(&r->mu);
    gen = r->generation;
    if (++r->arrived == r->parties) {
        r->arrived = 0;
        r->generation++;
        (void)pthread_cond_broadcast(&r->cv);
    } else {
        while (gen == r->generation) (void)pthread_cond_wait(&r->cv, &r->mu);
    }
    (void)pthread_mutex_unlock(&r->mu);
}

typedef struct {
    uint16_t port;
    rendezvous *rv;
    int lookup; /* 1: GET /lookup; 0: POST /register */
    int status; /* the response's status, 0 when there was none */
} race_client;

static void *race_client_main(void *arg) {
    race_client *c = (race_client *)arg;
    char body[64];
    int fd;

    rendezvous_wait(c->rv);
    fd = dial(c->port, 5);
    rendezvous_wait(c->rv);
    if (fd < 0) return NULL;
    if (send_all(fd, c->lookup
                         ? "GET /lookup HTTP/1.1\r\nHost: t\r\n\r\n"
                         : "POST /register HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n")) {
        (void)read_response(fd, &c->status, body, sizeof(body));
    }
    (void)close(fd);
    return NULL;
}

/* serial: the register, then the lookup, one after the other in this
 * thread: the order the program assumes, which never triggers it (the
 * control recording). Otherwise the pair races. */
static int run_load_race(uint16_t port, int serial) {
    pthread_t th[2];
    race_client cl[2];
    rendezvous rv;
    int i;

    (void)signal(SIGPIPE, SIG_IGN);
    memset(&rv, 0, sizeof(rv));
    rv.parties = serial ? 1 : 2;
    (void)pthread_mutex_init(&rv.mu, NULL);
    (void)pthread_cond_init(&rv.cv, NULL);
    for (i = 0; i < 2; i++) {
        memset(&cl[i], 0, sizeof(cl[i]));
        cl[i].port = port;
        cl[i].rv = &rv;
        cl[i].lookup = i;
    }
    if (serial) {
        (void)race_client_main(&cl[0]);
        (void)race_client_main(&cl[1]);
    } else {
        for (i = 0; i < 2; i++) {
            if (pthread_create(&th[i], NULL, race_client_main, &cl[i]) != 0) {
                printf("LOAD_FAIL race: thread\n");
                return 1;
            }
        }
        for (i = 0; i < 2; i++) (void)pthread_join(th[i], NULL);
    }
    printf("RACE_CLIENTS register=%d lookup=%d\n", cl[0].status, cl[1].status);
    fflush(stdout);
    if (cl[0].status != 200 || (cl[1].status != 200 && cl[1].status != 500)) {
        printf("LOAD_FAIL race: no answer\n");
        return 1;
    }
    return hold_until_drained(port);
}
#endif

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s record JOURNAL\n"
            "       %s load PORT\n"
            "       %s replay JOURNAL DIGEST SERVED [RUST_REV]\n"
            "       %s record-crash JOURNAL CKPTLOG KILL_AT\n"
            "       %s replay-prefix JOURNAL [RUST_REV]\n"
            "       %s load-race PORT [serial]    (built with -DASX_TEST_RACE)\n",
            argv0, argv0, argv0, argv0, argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "record") == 0) return run_record(argv[2]);
    if (argc >= 5 && strcmp(argv[1], "record-crash") == 0) {
        return run_record_crash(argv[2], argv[3], argv[4]);
    }
    if (argc >= 3 && strcmp(argv[1], "replay-prefix") == 0) {
        return run_replay_prefix(argv[2], argc >= 4 ? argv[3] : NULL);
    }
    if (argc >= 3 && strcmp(argv[1], "load") == 0) {
        unsigned long port = strtoul(argv[2], NULL, 10);
        if (port == 0ul || port > 65535ul) return usage(argv[0]);
        return run_load((uint16_t)port);
    }
#ifdef ASX_TEST_RACE
    if (argc >= 3 && strcmp(argv[1], "load-race") == 0) {
        unsigned long port = strtoul(argv[2], NULL, 10);
        if (port == 0ul || port > 65535ul) return usage(argv[0]);
        return run_load_race((uint16_t)port, argc >= 4 && strcmp(argv[3], "serial") == 0);
    }
#endif
    if (argc >= 5 && strcmp(argv[1], "replay") == 0) {
        return run_replay(argv[2], argv[3], argv[4], argc >= 6 ? argv[5] : NULL);
    }
    return usage(argv[0]);
}
