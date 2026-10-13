/*
 * bench_digest.c — what the runtime's rolling digests cost, before and
 * after (bd-o5zh)
 *
 * Three rolling digests are folded from the scheduler's hot path: the trace
 * digest (every trace event), the hindsight digest (every clock read,
 * entropy draw and reactor result) and the event-log hash chain (every
 * lifecycle event). Each is FNV-1a over the little-endian bytes of fixed
 * width fields. This program measures them two ways, in one process on one
 * core:
 *
 *   1. the fold itself, ns per event, over event mixes shaped like the
 *      runtime's: the previous implementations (copied here verbatim: the
 *      byte loop over a formatted byte array of hindsight and telemetry,
 *      the byte-at-a-time chain of the event log, the zero-run loop of the
 *      trace digest) against src/runtime/fnv_fold.h, which every digest of
 *      the runtime folds with now. Every pass also checks that the two
 *      produce the same digest, so a run of this program is a bit-identity
 *      check on the mixes it times.
 *   2. ns per spawn+run (one task, a fresh runtime, scheduler to
 *      quiescence), as the benchmark suite times it:
 *        before  the previous eager folds of the event-log chain and the
 *                hindsight digest, replayed with the previous code over
 *                this run's own events, added to the run: the previous
 *                runtime folded them at each emit. The trace digest was
 *                already lazy and is not added.
 *        after   the runtime as built: every digest lazy, unread.
 *        after, digests read   the same, then each digest read once, as a
 *                diagnostics consumer does: what the lazy folds cost when
 *                someone asks.
 *      and, over a sustained run that wraps every ring (spawn+join batches
 *      with no reset), the events per operation and the estimated ns per
 *      operation the new fold saves.
 *
 * "before" is the previous code's fold work added to the as-built run, not
 * a second library: it is exact for the folds, and approximate only in
 * cache effects. To compare against a whole previous tree, build it and run
 * its `make bench`.
 *
 * Build:  make perf-digest
 * Run:    build/bench/bench_digest [--rounds N] [--runs N]
 * Exit:   0, or 1 if a new digest differs from the previous code's.
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 199309L

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../src/runtime/fnv_fold.h"
#include <asx/asx.h>
#include <asx/runtime/event.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t median_u64(uint64_t *v, uint32_t n) {
    qsort(v, (size_t)n, sizeof(uint64_t), cmp_u64);
    return n == 0u ? 0u : v[n / 2u];
}

static double median_double(double *v, uint32_t n) {
    uint32_t i;
    uint32_t j;
    for (i = 1u; i < n; i++) {
        double x = v[i];
        for (j = i; j > 0u && v[j - 1u] > x; j--) v[j] = v[j - 1u];
        v[j] = x;
    }
    return n == 0u ? 0.0 : v[n / 2u];
}

/* ------------------------------------------------------------------ */
/* The previous folds, verbatim                                        */
/* ------------------------------------------------------------------ */

#define OLD_PRIME 0x00000100000001B3ULL

/* hindsight.c and telemetry.c: each field formatted into a byte array,
 * then a byte loop. */
static uint64_t old_mix(uint64_t hash, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= OLD_PRIME;
    }
    return hash;
}

static uint64_t old_mix_u32(uint64_t hash, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    return old_mix(hash, bytes, 4);
}

static uint64_t old_mix_u64(uint64_t hash, uint64_t v) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    bytes[4] = (uint8_t)((v >> 32) & 0xFFu);
    bytes[5] = (uint8_t)((v >> 40) & 0xFFu);
    bytes[6] = (uint8_t)((v >> 48) & 0xFFu);
    bytes[7] = (uint8_t)((v >> 56) & 0xFFu);
    return old_mix(hash, bytes, 8);
}

/* event_log.c: byte at a time. */
static uint64_t old_byte(uint64_t h, unsigned char b) {
    h ^= (uint64_t)b;
    h *= OLD_PRIME;
    return h;
}

static uint64_t old_log_u32(uint64_t h, uint32_t v) {
    h = old_byte(h, (unsigned char)(v & 0xFFu));
    h = old_byte(h, (unsigned char)((v >> 8) & 0xFFu));
    h = old_byte(h, (unsigned char)((v >> 16) & 0xFFu));
    h = old_byte(h, (unsigned char)((v >> 24) & 0xFFu));
    return h;
}

static uint64_t old_log_u64(uint64_t h, uint64_t v) {
    h = old_log_u32(h, (uint32_t)(v & 0xFFFFFFFFu));
    h = old_log_u32(h, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
    return h;
}

/* trace.c as of a846aa9: a run of zero high bytes is one multiply by a
 * power of the prime. */
static const uint64_t old_trace_pow[9] = {
    0x1ULL,
    0x100000001b3ULL,
    0x366000002e329ULL,
    0x8a97b0004e7feabULL,
    0x9ffaac085635bc91ULL,
    0xcaee32a7d4f6a63ULL,
    0xdc966432edf1c639ULL,
    0xc5527b8a51d3d2dbULL,
    0x1efac7090aef4a21ULL,
};

static uint64_t old_trace_le(uint64_t hash, uint64_t v, uint32_t width) {
    while (v != 0u) {
        hash = (hash ^ (v & 0xFFu)) * OLD_PRIME;
        v >>= 8;
        width--;
    }
    return hash * old_trace_pow[width];
}

/* ------------------------------------------------------------------ */
/* Event mixes shaped like the runtime's                               */
/* ------------------------------------------------------------------ */

#define MIX_N 256u

typedef struct {
    uint32_t sequence;
    uint32_t kind;
    uint64_t entity;
    uint64_t aux;
    uint32_t trace_seq;
} mix_event;

/* A tagged handle: a few bytes of slot and generation low, a tag high. */
static uint64_t mix_handle(uint32_t i) {
    return (UINT64_C(0x00A5) << 40) | ((uint64_t)(i % 64u) << 16) | (uint64_t)(1u + i % 5u);
}

static void mix_trace(mix_event *m) {
    static const uint32_t kinds[] = {0x00u, 0x00u, 0x13u, 0x14u, 0x01u};
    uint32_t i;
    for (i = 0; i < MIX_N; i++) {
        m[i].sequence = 40000u + i;
        m[i].kind = kinds[i % 5u];
        m[i].entity = mix_handle(i);
        m[i].aux = (i % 5u == 0u) ? (uint64_t)(i % 7u) : 0u;
        m[i].trace_seq = 0u;
    }
}

/* Clock reads: no entity, the raw nanosecond reading wide. */
static void mix_hindsight(mix_event *m) {
    uint32_t i;
    for (i = 0; i < MIX_N; i++) {
        m[i].sequence = i;
        m[i].kind = 0u;
        m[i].entity = 0u;
        m[i].aux = UINT64_C(1760000000000000000) + (uint64_t)i * 997u;
        m[i].trace_seq = 100u + i;
    }
}

static void mix_log(mix_event *m) {
    static const uint32_t kinds[] = {2u, 3u, 4u};
    uint32_t i;
    for (i = 0; i < MIX_N; i++) {
        m[i].sequence = i;
        m[i].kind = kinds[i % 3u];
        m[i].entity = mix_handle(i);
        m[i].aux = 0u;
        m[i].trace_seq = 0u;
    }
}

/* One pass of each fold over a mix, from `h`; returns the digest. */
static uint64_t pass_hs_old(const mix_event *m, uint64_t h) {
    uint32_t i;
    for (i = 0; i < MIX_N; i++) {
        h = old_mix_u32(h, m[i].sequence);
        h = old_mix_u32(h, m[i].kind);
        h = old_mix_u64(h, m[i].entity);
        h = old_mix_u64(h, m[i].aux);
        h = old_mix_u32(h, m[i].trace_seq);
    }
    return h;
}

static uint64_t pass_hs_new(const mix_event *m, uint64_t h) {
    asx_fnv_fold f;
    uint32_t i;
    asx_fnv_fold_init(&f, h);
    for (i = 0; i < MIX_N; i++) {
        asx_fnv_fold_le(&f, m[i].sequence, 4u);
        asx_fnv_fold_le(&f, m[i].kind, 4u);
        asx_fnv_fold_le(&f, m[i].entity, 8u);
        asx_fnv_fold_le(&f, m[i].aux, 8u);
        asx_fnv_fold_le(&f, m[i].trace_seq, 4u);
    }
    return asx_fnv_fold_value(&f);
}

static uint64_t pass_log_old(const mix_event *m, uint64_t h) {
    uint32_t i;
    for (i = 0; i < MIX_N; i++) {
        h = old_log_u32(h, m[i].kind);
        h = old_log_u64(h, m[i].entity);
        h = old_log_u32(h, m[i].sequence);
    }
    return h;
}

static uint64_t pass_log_new(const mix_event *m, uint64_t h) {
    asx_fnv_fold f;
    uint32_t i;
    asx_fnv_fold_init(&f, h);
    for (i = 0; i < MIX_N; i++) {
        asx_fnv_fold_le(&f, m[i].kind, 4u);
        asx_fnv_fold_le(&f, m[i].entity, 8u);
        asx_fnv_fold_le(&f, m[i].sequence, 4u);
    }
    return asx_fnv_fold_value(&f);
}

static uint64_t pass_trace_old(const mix_event *m, uint64_t h) {
    uint32_t i;
    for (i = 0; i < MIX_N; i++) {
        h = old_trace_le(h, m[i].sequence, 4u);
        h = old_trace_le(h, m[i].kind, 4u);
        h = old_trace_le(h, m[i].entity, 8u);
        h = old_trace_le(h, m[i].aux, 8u);
    }
    return h;
}

static uint64_t pass_trace_new(const mix_event *m, uint64_t h) {
    asx_fnv_fold f;
    uint32_t i;
    asx_fnv_fold_init(&f, h);
    for (i = 0; i < MIX_N; i++) {
        asx_fnv_fold_le(&f, m[i].sequence, 4u);
        asx_fnv_fold_le(&f, m[i].kind, 4u);
        asx_fnv_fold_le(&f, m[i].entity, 8u);
        asx_fnv_fold_le(&f, m[i].aux, 8u);
    }
    return asx_fnv_fold_value(&f);
}

typedef uint64_t (*pass_fn)(const mix_event *m, uint64_t h);

static volatile uint64_t g_sink;
static int g_failed; /* a runtime call returned an error: the timings mean nothing */

/* ns per event of `fn` over `m`, `reps` passes chained. */
static double time_pass(pass_fn fn, const mix_event *m, uint32_t reps, uint64_t *digest) {
    uint64_t h = 0x517cc1b727220a95ULL;
    uint32_t r;
    uint64_t t0 = now_ns();
    uint64_t t1;
    for (r = 0; r < reps; r++) h = fn(m, h);
    t1 = now_ns();
    g_sink = h;
    *digest = h;
    return (double)(t1 - t0) / ((double)reps * (double)MIX_N);
}

typedef struct {
    const char *name;
    void (*make)(mix_event *);
    pass_fn old_fn;
    pass_fn new_fn;
    const char *old_label;
} fold_case;

static int bench_folds(uint32_t rounds, double *log_old, double *log_new, double *hs_old,
                       double *hs_new, double *tr_old, double *tr_new) {
    static const fold_case cases[] = {
        {"hindsight digest (clock reads)", mix_hindsight, pass_hs_old, pass_hs_new,
         "formatted bytes + loop"},
        {"event-log chain (lifecycle)", mix_log, pass_log_old, pass_log_new, "byte at a time"},
        {"trace digest (scheduler events)", mix_trace, pass_trace_old, pass_trace_new,
         "zero-run loop (a846aa9)"},
    };
    static mix_event m[MIX_N];
    double *outs_old[3];
    double *outs_new[3];
    uint32_t c;
    const uint32_t reps = 4000u;
    int ok = 1;

    outs_old[0] = hs_old;
    outs_old[1] = log_old;
    outs_old[2] = tr_old;
    outs_new[0] = hs_new;
    outs_new[1] = log_new;
    outs_new[2] = tr_new;

    printf("fold cost, ns per event (median of %u interleaved rounds, %u events x %u passes):\n",
           (unsigned)rounds, (unsigned)MIX_N, (unsigned)reps);
    for (c = 0; c < 3u; c++) {
        double olds[64];
        double news[64];
        uint32_t r;
        uint64_t d_old = 0u;
        uint64_t d_new = 0u;
        uint32_t n = rounds < 64u ? rounds : 64u;

        cases[c].make(m);
        for (r = 0; r < n; r++) {
            olds[r] = time_pass(cases[c].old_fn, m, reps, &d_old);
            news[r] = time_pass(cases[c].new_fn, m, reps, &d_new);
            if (d_old != d_new) ok = 0;
        }
        *outs_old[c] = median_double(olds, n);
        *outs_new[c] = median_double(news, n);
        printf("  %-34s before %6.2f  after %6.2f  (%.2fx)  [%s]  digest %s\n", cases[c].name,
               *outs_old[c], *outs_new[c], *outs_new[c] > 0.0 ? *outs_old[c] / *outs_new[c] : 0.0,
               cases[c].old_label, d_old == d_new ? "identical" : "DIFFERENT");
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* spawn + run                                                         */
/* ------------------------------------------------------------------ */

static asx_status noop_poll(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

/* The previous eager folds, replayed over the events this run produced:
 * the event-log chain and the hindsight digest. Returns the digests. */
static uint64_t replay_old_folds(void) {
    uint64_t chain = 14695981039346656037ULL;
    uint64_t hs = 0x517cc1b727220a95ULL;
    uint32_t n = asx_event_log_count();
    uint32_t i;
    asx_event_record er;
    asx_hindsight_event he;

    if (n > (uint32_t)ASX_EVENT_LOG_CAPACITY) n = (uint32_t)ASX_EVENT_LOG_CAPACITY;
    for (i = 0; i < n; i++) {
        if (!asx_event_log_get(i, &er)) break;
        chain = old_log_u32(chain, (uint32_t)er.kind);
        chain = old_log_u64(chain, er.entity_id);
        chain = old_log_u32(chain, er.sequence);
    }
    n = asx_hindsight_readable_count();
    for (i = 0; i < n; i++) {
        if (!asx_hindsight_get(i, &he)) break;
        hs = old_mix_u32(hs, he.sequence);
        hs = old_mix_u32(hs, (uint32_t)he.kind);
        hs = old_mix_u64(hs, he.entity_id);
        hs = old_mix_u64(hs, he.observed_value);
        hs = old_mix_u32(hs, he.trace_seq);
    }
    return chain ^ hs;
}

typedef struct {
    uint64_t run_ns;  /* spawn + run, no digest read */
    uint64_t read_ns; /* the three digest reads that follow */
    uint64_t old_ns;  /* the previous eager folds over this run's events */
    uint32_t trace_events;
    uint32_t hindsight_events;
    uint32_t log_events;
} one_run;

static void spawn_run_once(one_run *out) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t sink = 0u;

    asx_runtime_reset();
    if (asx_region_open(&rid) != ASX_OK) g_failed = 1;

    t0 = now_ns();
    if (asx_task_spawn(rid, noop_poll, NULL, &tid) != ASX_OK) g_failed = 1;
    budget = asx_budget_from_polls(64);
    if (asx_scheduler_run(rid, &budget) != ASX_OK) g_failed = 1;
    t1 = now_ns();

    /* The digests a diagnostics consumer reads, once. */
    sink ^= asx_trace_digest();
    sink ^= asx_hindsight_digest();
    sink ^= asx_event_hash_chain();
    t2 = now_ns();

    out->trace_events = (uint32_t)asx_trace_emitted_total();
    out->hindsight_events = asx_hindsight_total_count();
    out->log_events = asx_event_log_count();

    /* What the previous runtime did at each emit, over the same events. */
    t3 = now_ns();
    sink ^= replay_old_folds();
    out->old_ns = now_ns() - t3;
    g_sink = sink;

    out->run_ns = t1 - t0;
    out->read_ns = t2 - t1;
}

static int bench_spawn_run(uint32_t runs, uint64_t *before_ns, uint64_t *after_ns,
                           uint64_t *after_read_ns) {
    uint64_t *run = (uint64_t *)malloc((size_t)runs * sizeof(uint64_t));
    uint64_t *rd = (uint64_t *)malloc((size_t)runs * sizeof(uint64_t));
    uint64_t *old = (uint64_t *)malloc((size_t)runs * sizeof(uint64_t));
    uint64_t *timer = (uint64_t *)malloc((size_t)runs * sizeof(uint64_t));
    one_run r;
    uint32_t i;
    uint32_t warm;
    uint64_t timer_ns;
    uint32_t tev = 0u;
    uint32_t hev = 0u;
    uint32_t lev = 0u;

    if (run == NULL || rd == NULL || old == NULL || timer == NULL) {
        free(run);
        free(rd);
        free(old);
        free(timer);
        return 0;
    }
    /* The cost of reading the clock twice, to take out of each figure. */
    for (i = 0; i < runs; i++) {
        uint64_t a = now_ns();
        uint64_t b = now_ns();
        timer[i] = b - a;
    }
    timer_ns = median_u64(timer, runs);

    for (warm = 0; warm < 200u; warm++) spawn_run_once(&r);
    for (i = 0; i < runs; i++) {
        spawn_run_once(&r);
        run[i] = r.run_ns > timer_ns ? r.run_ns - timer_ns : 0u;
        rd[i] = r.read_ns > timer_ns ? r.read_ns - timer_ns : 0u;
        old[i] = r.old_ns > timer_ns ? r.old_ns - timer_ns : 0u;
        tev = r.trace_events;
        hev = r.hindsight_events;
        lev = r.log_events;
    }
    *after_ns = median_u64(run, runs);
    *after_read_ns = *after_ns + median_u64(rd, runs);
    *before_ns = *after_ns + median_u64(old, runs);

    printf("\nspawn + run, one task, fresh runtime (median of %u runs; clock read cost %" PRIu64
           " ns taken out):\n",
           (unsigned)runs, timer_ns);
    printf("  events per run: trace %u, hindsight %u, event log %u\n", (unsigned)tev, (unsigned)hev,
           (unsigned)lev);
    printf("  before   %6" PRIu64
           " ns   (run + the previous eager event-log and hindsight folds)\n",
           *before_ns);
    printf("  after    %6" PRIu64 " ns   (every digest lazy, none read)\n", *after_ns);
    printf("  after + digests read once   %6" PRIu64 " ns\n", *after_read_ns);
    free(run);
    free(rd);
    free(old);
    free(timer);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Sustained spawn + join: every ring wraps                            */
/* ------------------------------------------------------------------ */

/* Tasks live at once: the arena, at most 32 (as tools/perf_compare does). */
#if ASX_MAX_TASKS < 32
#define BATCH ASX_MAX_TASKS
#else
#define BATCH 32u
#endif

static void bench_sustained(uint32_t ops, double log_old, double log_new, double hs_old,
                            double hs_new, double tr_old, double tr_new) {
    asx_region_id rid;
    asx_task_id tids[BATCH];
    uint64_t done = 0u;
    uint64_t t0;
    uint64_t t1;
    uint64_t trace0;
    uint32_t hs0;
    uint32_t log0;
    double ns_per_op;
    double tev;
    double hev;
    double lev;
    double saved;

    asx_runtime_reset();
    if (asx_region_open(&rid) != ASX_OK) return;
    trace0 = asx_trace_emitted_total();
    hs0 = asx_hindsight_total_count();
    log0 = asx_event_log_count();
    t0 = now_ns();
    while (done < ops) {
        asx_budget budget;
        uint32_t i;
        for (i = 0; i < BATCH; i++) {
            if (asx_task_spawn(rid, noop_poll, NULL, &tids[i]) != ASX_OK) return;
        }
        budget = asx_budget_from_polls((uint32_t)BATCH * 2u + 16u);
        if (asx_scheduler_run(rid, &budget) != ASX_OK) return;
        for (i = 0; i < BATCH; i++) {
            if (asx_task_join(tids[i], NULL) != ASX_OK) return;
        }
        done += BATCH;
    }
    t1 = now_ns();
    ns_per_op = (double)(t1 - t0) / (double)done;
    tev = (double)(asx_trace_emitted_total() - trace0) / (double)done;
    hev = (double)(asx_hindsight_total_count() - hs0) / (double)done;
    lev = (double)(asx_event_log_count() - log0) / (double)done;
    saved = tev * (tr_old - tr_new) + hev * (hs_old - hs_new) + lev * (log_old - log_new);

    printf("\nsustained spawn + join, %u ops in batches of %u, no reset (the rings wrap):\n",
           (unsigned)done, (unsigned)BATCH);
    printf("  after   %7.1f ns/op   events per op: trace %.1f, hindsight %.1f, event log %.1f\n",
           ns_per_op, tev, hev, lev);
    printf(
        "  the new fold saves an estimated %.1f ns/op (events/op x the per-event savings above)\n",
        saved);
    printf("  before (estimated)  %7.1f ns/op\n", ns_per_op + saved);
}

int main(int argc, char **argv) {
    uint32_t rounds = 15u;
    uint32_t runs = 5001u;
    double log_old = 0.0;
    double log_new = 0.0;
    double hs_old = 0.0;
    double hs_new = 0.0;
    double tr_old = 0.0;
    double tr_new = 0.0;
    uint64_t before = 0u;
    uint64_t after = 0u;
    uint64_t after_read = 0u;
    int ok;
    int i;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--rounds") == 0) rounds = (uint32_t)strtoul(argv[i + 1], NULL, 10);
        if (strcmp(argv[i], "--runs") == 0) runs = (uint32_t)strtoul(argv[i + 1], NULL, 10);
    }
    if (rounds == 0u) rounds = 1u;
    if (runs < 11u) runs = 11u;

    printf("bench_digest: trace capacity %u, hindsight capacity %u, event log capacity %u\n",
           (unsigned)ASX_TRACE_CAPACITY, (unsigned)ASX_HINDSIGHT_CAPACITY,
           (unsigned)ASX_EVENT_LOG_CAPACITY);
    ok = bench_folds(rounds, &log_old, &log_new, &hs_old, &hs_new, &tr_old, &tr_new);
    if (!bench_spawn_run(runs, &before, &after, &after_read)) {
        fprintf(stderr, "bench_digest: out of memory\n");
        return 1;
    }
    bench_sustained(200000u, log_old, log_new, hs_old, hs_new, tr_old, tr_new);

    printf("\nSUMMARY spawn+run: before %" PRIu64 " ns, after %" PRIu64 " ns (%.2fx), "
           "after with digests read %" PRIu64 " ns\n",
           before, after, after > 0u ? (double)before / (double)after : 0.0, after_read);
    if (!ok) {
        fprintf(stderr, "bench_digest: a new fold produced a different digest than the previous "
                        "one\n");
        return 1;
    }
    if (g_failed) {
        fprintf(stderr, "bench_digest: a runtime call failed; the timings above mean nothing\n");
        return 1;
    }
    return 0;
}
