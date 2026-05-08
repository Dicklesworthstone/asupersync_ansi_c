/*
 * bench_runtime.c — performance benchmark suite for asx runtime (bd-1md.6)
 *
 * Microbenchmarks for scheduler, timer wheel, channel, and quiescence
 * paths. Emits p50/p95/p99/p99.9/p99.99 plus jitter and deadline-miss
 * metrics in machine-readable JSON for CI gates and trend tracking.
 *
 * Build:  make bench
 * Run:    build/bench/bench_runtime [--json]
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

#include <asx/asx.h>
#include <asx/core/adaptive.h>
#include <asx/core/budget.h>
#include <asx/core/channel.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>

/* -------------------------------------------------------------------
 * Timing helpers
 * ------------------------------------------------------------------- */

static uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------
 * Sample collection and statistics
 * ------------------------------------------------------------------- */

#define BENCH_MAX_SAMPLES 10000u

typedef struct {
    uint64_t samples[BENCH_MAX_SAMPLES];
    uint32_t count;
} bench_samples;

static void bench_samples_init(bench_samples *s) { s->count = 0; }

static void bench_samples_add(bench_samples *s, uint64_t val) {
    if (s->count < BENCH_MAX_SAMPLES) { s->samples[s->count] = val; }
    s->count++;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void bench_samples_sort(bench_samples *s) {
    uint32_t n = s->count < BENCH_MAX_SAMPLES ? s->count : BENCH_MAX_SAMPLES;
    qsort(s->samples, (size_t)n, sizeof(uint64_t), cmp_u64);
}

static uint64_t bench_percentile(const bench_samples *s, double p) {
    uint32_t n = s->count < BENCH_MAX_SAMPLES ? s->count : BENCH_MAX_SAMPLES;
    uint32_t idx;
    if (n == 0) return 0;
    idx = (uint32_t)(p / 100.0 * (double)(n - 1u));
    if (idx >= n) idx = n - 1u;
    return s->samples[idx];
}

typedef struct {
    uint64_t mean;
    uint64_t min_val;
    uint64_t max_val;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    uint64_t p99_9;
    uint64_t p99_99;
    uint64_t jitter; /* mean absolute deviation */
    uint32_t count;
} bench_stats;

static void bench_print_stats_members_json(const bench_stats *st, const char *indent) {
    printf("%s\"count\": %" PRIu32 ",\n", indent, st->count);
    printf("%s\"mean_ns\": %" PRIu64 ",\n", indent, st->mean);
    printf("%s\"min_ns\": %" PRIu64 ",\n", indent, st->min_val);
    printf("%s\"max_ns\": %" PRIu64 ",\n", indent, st->max_val);
    printf("%s\"p50_ns\": %" PRIu64 ",\n", indent, st->p50);
    printf("%s\"p95_ns\": %" PRIu64 ",\n", indent, st->p95);
    printf("%s\"p99_ns\": %" PRIu64 ",\n", indent, st->p99);
    printf("%s\"p99_9_ns\": %" PRIu64 ",\n", indent, st->p99_9);
    printf("%s\"p99_99_ns\": %" PRIu64 ",\n", indent, st->p99_99);
    printf("%s\"jitter_ns\": %" PRIu64 "\n", indent, st->jitter);
}

static bench_stats bench_compute_stats(bench_samples *s) {
    bench_stats st;
    uint64_t sum = 0;
    uint64_t jitter_sum = 0;
    uint32_t i;
    uint32_t n = s->count < BENCH_MAX_SAMPLES ? s->count : BENCH_MAX_SAMPLES;

    memset(&st, 0, sizeof(st));
    if (n == 0) return st;

    bench_samples_sort(s);

    st.min_val = s->samples[0];
    st.max_val = s->samples[n - 1u];

    for (i = 0; i < n; i++) { sum += s->samples[i]; }
    st.mean = sum / (uint64_t)n;

    for (i = 0; i < n; i++) {
        uint64_t diff = s->samples[i] > st.mean ? s->samples[i] - st.mean : st.mean - s->samples[i];
        jitter_sum += diff;
    }
    st.jitter = jitter_sum / (uint64_t)n;

    st.p50 = bench_percentile(s, 50.0);
    st.p95 = bench_percentile(s, 95.0);
    st.p99 = bench_percentile(s, 99.0);
    st.p99_9 = bench_percentile(s, 99.9);
    st.p99_99 = bench_percentile(s, 99.99);
    st.count = n;

    return st;
}

static void bench_print_stats_json(const char *name, const bench_stats *st, int last) {
    printf("    \"%s\": {\n", name);
    bench_print_stats_members_json(st, "      ");
    printf("    }%s\n", last ? "" : ",");
}

static void bench_print_json_string(const char *value) {
    const unsigned char *p;

    if (value == NULL) { value = ""; }

    putchar('"');
    p = (const unsigned char *)value;
    while (*p != '\0') {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar((int)*p);
        } else if (*p == '\b') {
            printf("\\b");
        } else if (*p == '\f') {
            printf("\\f");
        } else if (*p == '\n') {
            printf("\\n");
        } else if (*p == '\r') {
            printf("\\r");
        } else if (*p == '\t') {
            printf("\\t");
        } else if (*p < 0x20u) {
            printf("\\u%04x", (unsigned int)*p);
        } else {
            putchar((int)*p);
        }
        p++;
    }
    putchar('"');
}

static const char *bench_profile_name(void) {
#if defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_HFT)
    return "HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "AUTOMOTIVE";
#elif defined(ASX_PROFILE_POSIX)
    return "POSIX";
#elif defined(ASX_PROFILE_WIN32)
    return "WIN32";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "FREESTANDING";
#elif defined(ASX_PROFILE_PARALLEL)
    return "PARALLEL";
#elif defined(ASX_PROFILE_BROWSER)
    return "BROWSER";
#else
    return "CORE";
#endif
}

static const char *bench_codec_name(void) {
#if defined(ASX_CODEC_BIN)
    return "BIN";
#else
    return "JSON";
#endif
}

static const char *bench_compiler_name(void) {
#if defined(__clang__)
    return "clang";
#elif defined(_MSC_VER)
    return "msvc";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

static const char *bench_compiler_version(void) {
#if defined(__clang_version__)
    return __clang_version__;
#elif defined(_MSC_VER)
    return "msvc";
#elif defined(__VERSION__)
    return __VERSION__;
#else
    return "unknown";
#endif
}

static uint64_t bench_ops_per_sec(uint32_t ops_per_sample, const bench_stats *st) {
    if (st->mean == 0u) { return 0u; }
    return ((uint64_t)ops_per_sample * UINT64_C(1000000000)) / st->mean;
}

/* -------------------------------------------------------------------
 * Noop poll function for scheduler benchmarks
 * ------------------------------------------------------------------- */

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* Poll function that returns PENDING N times before OK */
typedef struct {
    int remaining;
} countdown_ctx;

static asx_status countdown_poll(void *user_data, asx_task_id self) {
    countdown_ctx *ctx = (countdown_ctx *)user_data;
    (void)self;
    if (ctx->remaining > 0) {
        ctx->remaining--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

static asx_status cancel_checkpoint_poll(void *user_data, asx_task_id self) {
    asx_checkpoint_result cr;

    (void)user_data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

/* -------------------------------------------------------------------
 * BENCH 1: Scheduler — single-task single-poll throughput
 *
 * Measures: time to spawn one task, run scheduler to completion,
 * and verify quiescence. This is the scheduler hot path.
 * ------------------------------------------------------------------- */

static bench_stats bench_scheduler_single_task(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < BENCH_MAX_SAMPLES; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1;

        asx_runtime_reset();
        (void)asx_region_open(&rid);

        t0 = bench_now_ns();

        (void)asx_task_spawn(rid, noop_poll, NULL, &tid);
        budget = asx_budget_from_polls(64);
        (void)asx_scheduler_run(rid, &budget);

        t1 = bench_now_ns();
        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 2: Scheduler — multi-task single-round throughput
 *
 * Measures: scheduler_run with N noop tasks that each complete in
 * one poll. Tests scaling of the arena scan.
 * ------------------------------------------------------------------- */

static bench_stats bench_scheduler_multi_task(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 1000; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1;
        uint32_t t_i;

        asx_runtime_reset();
        (void)asx_region_open(&rid);

        /* Spawn ASX_MAX_TASKS tasks */
        for (t_i = 0; t_i < ASX_MAX_TASKS; t_i++) {
            (void)asx_task_spawn(rid, noop_poll, NULL, &tid);
        }

        budget = asx_budget_from_polls(ASX_MAX_TASKS * 2u);

        t0 = bench_now_ns();
        (void)asx_scheduler_run(rid, &budget);
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 3: Scheduler — multi-round (tasks need multiple polls)
 *
 * Measures: scheduler round-robin cost when tasks yield multiple
 * times before completion.
 * ------------------------------------------------------------------- */

static bench_stats bench_scheduler_multi_round(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 1000; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1;
        uint32_t t_i;
        countdown_ctx ctxs[16];

        asx_runtime_reset();
        (void)asx_region_open(&rid);

        /* Spawn 16 tasks that each need 10 polls */
        for (t_i = 0; t_i < 16; t_i++) {
            ctxs[t_i].remaining = 10;
            (void)asx_task_spawn(rid, countdown_poll, &ctxs[t_i], &tid);
        }

        budget = asx_budget_from_polls(16u * 12u);

        t0 = bench_now_ns();
        (void)asx_scheduler_run(rid, &budget);
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 4: Timer wheel — register throughput
 *
 * Measures: time to register N timers sequentially.
 * ------------------------------------------------------------------- */

static bench_stats bench_timer_register(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 2000; iter++) {
        asx_timer_wheel *w = asx_timer_wheel_global();
        asx_timer_handle h;
        uint64_t t0, t1;
        uint32_t t_i;

        asx_timer_wheel_reset(w);

        t0 = bench_now_ns();
        for (t_i = 0; t_i < ASX_MAX_TIMERS; t_i++) {
            (void)asx_timer_register(w, (asx_time)(t_i + 1u) * 1000u, NULL, &h);
        }
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 5: Timer wheel — O(1) cancel throughput
 *
 * Measures: time to cancel N timers after registration.
 * ------------------------------------------------------------------- */

static bench_stats bench_timer_cancel(void) {
    bench_samples s;
    uint32_t iter;
    asx_timer_handle handles[ASX_MAX_TIMERS];

    bench_samples_init(&s);

    for (iter = 0; iter < 2000; iter++) {
        asx_timer_wheel *w = asx_timer_wheel_global();
        uint64_t t0, t1;
        uint32_t t_i;

        asx_timer_wheel_reset(w);

        /* Register all timers */
        for (t_i = 0; t_i < ASX_MAX_TIMERS; t_i++) {
            (void)asx_timer_register(w, (asx_time)(t_i + 1u) * 1000u, NULL, &handles[t_i]);
        }

        t0 = bench_now_ns();
        for (t_i = 0; t_i < ASX_MAX_TIMERS; t_i++) { (void)asx_timer_cancel(w, &handles[t_i]); }
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 6: Timer wheel — collect expired (deterministic ordering)
 *
 * Measures: time to collect all expired timers with deterministic
 * sorting by (deadline ASC, insertion_seq ASC).
 * ------------------------------------------------------------------- */

static bench_stats bench_timer_collect(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 2000; iter++) {
        asx_timer_wheel *w = asx_timer_wheel_global();
        asx_timer_handle h;
        void *wakers[ASX_MAX_TIMERS];
        uint64_t t0, t1;
        uint32_t t_i;

        asx_timer_wheel_reset(w);

        /* Register timers at various deadlines */
        for (t_i = 0; t_i < ASX_MAX_TIMERS; t_i++) {
            (void)asx_timer_register(w, (asx_time)((t_i % 8u) + 1u) * 100u, NULL, &h);
        }

        t0 = bench_now_ns();
        (void)asx_timer_collect_expired(w, (asx_time)10000u, wakers, ASX_MAX_TIMERS);
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 7: Channel — reserve+send throughput
 *
 * Measures: time for N reserve-send pairs on a bounded channel.
 * ------------------------------------------------------------------- */

static bench_stats bench_channel_send(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 2000; iter++) {
        asx_region_id rid;
        asx_channel_id cid;
        uint64_t t0, t1;
        uint32_t m_i;

        asx_runtime_reset();
        asx_channel_reset();
        (void)asx_region_open(&rid);
        (void)asx_channel_create(rid, ASX_CHANNEL_MAX_CAPACITY, &cid);

        t0 = bench_now_ns();
        for (m_i = 0; m_i < ASX_CHANNEL_MAX_CAPACITY; m_i++) {
            asx_send_permit permit;
            (void)asx_channel_try_reserve(cid, &permit);
            (void)asx_send_permit_send(&permit, (uint64_t)m_i);
        }
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 8: Channel — recv throughput
 *
 * Measures: time to recv N messages from a full channel.
 * ------------------------------------------------------------------- */

static bench_stats bench_channel_recv(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 2000; iter++) {
        asx_region_id rid;
        asx_channel_id cid;
        uint64_t t0, t1;
        uint32_t m_i;

        asx_runtime_reset();
        asx_channel_reset();
        (void)asx_region_open(&rid);
        (void)asx_channel_create(rid, ASX_CHANNEL_MAX_CAPACITY, &cid);

        /* Fill channel */
        for (m_i = 0; m_i < ASX_CHANNEL_MAX_CAPACITY; m_i++) {
            asx_send_permit permit;
            (void)asx_channel_try_reserve(cid, &permit);
            (void)asx_send_permit_send(&permit, (uint64_t)m_i);
        }

        t0 = bench_now_ns();
        for (m_i = 0; m_i < ASX_CHANNEL_MAX_CAPACITY; m_i++) {
            uint64_t val;
            (void)asx_channel_try_recv(cid, &val);
        }
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 9: Quiescence — region drain (open → close → drain → closed)
 *
 * Measures: full drain path including task completion and region
 * finalization.
 * ------------------------------------------------------------------- */

static bench_stats bench_quiescence_drain(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 2000; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1;
        uint32_t t_i;

        asx_runtime_reset();
        (void)asx_region_open(&rid);

        /* Spawn 8 noop tasks */
        for (t_i = 0; t_i < 8; t_i++) { (void)asx_task_spawn(rid, noop_poll, NULL, &tid); }

        budget = asx_budget_from_polls(64);

        t0 = bench_now_ns();
        (void)asx_region_drain(rid, &budget);
        t1 = bench_now_ns();

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 10: Budget algebra — meet operation throughput
 *
 * Measures: time for N budget meet operations (compositional
 * constraint tightening).
 * ------------------------------------------------------------------- */

static bench_stats bench_budget_meet(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < BENCH_MAX_SAMPLES; iter++) {
        asx_budget a, b, result;
        uint64_t t0, t1;
        uint32_t m_i;

        a = asx_budget_infinite();
        b.deadline = 5000;
        b.poll_quota = 100;
        b.cost_quota = 10000;
        b.priority = 3;

        t0 = bench_now_ns();
        for (m_i = 0; m_i < 1000; m_i++) {
            result = asx_budget_meet(&a, &b);
            a = result;
            /* Prevent optimizing away: perturb input */
            b.poll_quota = (uint32_t)(100u + (m_i & 0xFu));
        }
        t1 = bench_now_ns();

        /* Use result to prevent DCE */
        if (result.poll_quota == UINT32_MAX) { fprintf(stderr, "unexpected\n"); }

        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 11: Embedded pressure — scheduler under tight budget
 *
 * Measures: scheduler behavior when poll budget is barely sufficient.
 * Simulates embedded/resource-constrained execution.
 * ------------------------------------------------------------------- */

static bench_stats bench_embedded_pressure(void) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);

    for (iter = 0; iter < 1000; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        asx_status st;
        uint64_t t0, t1;
        uint32_t t_i;
        countdown_ctx ctxs[8];
        uint32_t rounds = 0;

        asx_runtime_reset();
        (void)asx_region_open(&rid);

        /* 8 tasks, each needs 5 polls */
        for (t_i = 0; t_i < 8; t_i++) {
            ctxs[t_i].remaining = 5;
            (void)asx_task_spawn(rid, countdown_poll, &ctxs[t_i], &tid);
        }

        t0 = bench_now_ns();

        /* Run with budget of 10 polls at a time (simulates tight budget) */
        do {
            budget = asx_budget_from_polls(10);
            st = asx_scheduler_run(rid, &budget);
            rounds++;
        } while (st == ASX_E_POLL_BUDGET_EXHAUSTED && rounds < 100);

        t1 = bench_now_ns();
        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

/* -------------------------------------------------------------------
 * BENCH 12: Parallel large swarm — telemetry/admission evidence
 *
 * Measures the optional parallel scheduler over the largest task set the
 * current core arena admits, and captures the final structured telemetry
 * snapshot for benchmark JSON artifacts.
 * ------------------------------------------------------------------- */

typedef struct {
    bench_stats stats;
    bench_stats cancel_stats;
    bench_stats mpsc_stats;
    asx_parallel_telemetry_snapshot snapshot;
    uint32_t requested_worker_count;
    uint32_t worker_count;
    uint32_t task_count;
    uint32_t samples;
    int supported;
    uint32_t peak_pressure_pct;
    uint32_t peak_max_lane_depth;
    uint32_t peak_max_worker_queue_depth;
    uint64_t scheduler_throughput_ops_per_sec;
    uint64_t cancel_latency_mean_ns;
    uint64_t mpsc_contention_ops_per_sec;
    uint64_t trace_commit_mean_ns;
} bench_parallel_report;

#define BENCH_PARALLEL_SEED UINT32_C(0x41535850)
#define BENCH_PARALLEL_LARGE_SWARM_SAMPLES 1000u
#define BENCH_PARALLEL_BASELINE_SAMPLES 256u
#define BENCH_PARALLEL_AUX_SAMPLES 128u
#define BENCH_PARALLEL_BASELINE_COUNT 5u
#define BENCH_PARALLEL_CANCEL_WARN_NS UINT64_C(1000000)
#define BENCH_PARALLEL_TRACE_COMMIT_WARN_NS UINT64_C(1000000)
#define BENCH_PARALLEL_MPSC_FLOOR_OPS_PER_SEC UINT64_C(1000)

typedef struct {
    bench_parallel_report rows[BENCH_PARALLEL_BASELINE_COUNT];
    uint32_t count;
    uint32_t supported_count;
} bench_parallel_baselines;

static void bench_parallel_config_init(asx_parallel_config *cfg, uint32_t worker_count) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->worker_count = worker_count;
    cfg->fairness = ASX_FAIRNESS_ROUND_ROBIN;
    cfg->lane_weights[0] = 1u;
    cfg->lane_weights[1] = 1u;
    cfg->lane_weights[2] = 1u;
    cfg->starvation_limit = 8u;
    asx_parallel_admission_policy_init(&cfg->admission_policy);
    asx_parallel_locality_config_init(&cfg->locality);
    if (worker_count > 1u) { cfg->locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED; }
}

static bench_stats bench_parallel_cancel_latency(uint32_t worker_count, uint32_t sample_count) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);
    for (iter = 0u; iter < sample_count; iter++) {
        asx_parallel_config cfg;
        asx_region_id rid;
        asx_task_id tids[ASX_MAX_TASKS];
        asx_budget budget;
        uint64_t t0, t1;
        uint32_t i;

        bench_parallel_config_init(&cfg, worker_count);
        asx_runtime_reset();
        asx_parallel_reset();
        (void)asx_parallel_init(&cfg);
        (void)asx_region_open(&rid);

        for (i = 0u; i < ASX_MAX_TASKS; i++) {
            (void)asx_task_spawn(rid, cancel_checkpoint_poll, NULL, &tids[i]);
        }

        budget = asx_budget_from_polls(ASX_MAX_TASKS);
        (void)asx_parallel_run(rid, &budget);

        for (i = 0u; i < ASX_MAX_TASKS; i++) { (void)asx_task_cancel(tids[i], ASX_CANCEL_USER); }

        budget = asx_budget_from_polls(ASX_MAX_TASKS * 4u);
        t0 = bench_now_ns();
        (void)asx_parallel_run(rid, &budget);
        t1 = bench_now_ns();
        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

static bench_stats bench_parallel_mpsc_contention(uint32_t worker_count, uint32_t sample_count) {
    bench_samples s;
    uint32_t iter;

    bench_samples_init(&s);
    for (iter = 0u; iter < sample_count; iter++) {
        asx_parallel_config cfg;
        asx_region_id rid;
        asx_channel_id cid;
        uint64_t t0, t1;
        uint32_t i;

        bench_parallel_config_init(&cfg, worker_count);
        asx_runtime_reset();
        asx_channel_reset();
        asx_parallel_reset();
        (void)asx_parallel_init(&cfg);
        (void)asx_region_open(&rid);
        (void)asx_channel_create(rid, ASX_CHANNEL_MAX_CAPACITY, &cid);

        t0 = bench_now_ns();
        for (i = 0u; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
            asx_send_permit permit;
            (void)asx_channel_try_reserve(cid, &permit);
            (void)asx_send_permit_send(&permit, (uint64_t)i);
        }
        for (i = 0u; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
            uint64_t value;
            (void)asx_channel_try_recv(cid, &value);
        }
        t1 = bench_now_ns();
        bench_samples_add(&s, t1 - t0);
    }

    return bench_compute_stats(&s);
}

static bench_parallel_report bench_parallel_large_swarm_for_workers(uint32_t requested_worker_count,
                                                                    uint32_t sample_count) {
    bench_parallel_report rpt;
    bench_samples s;
    uint32_t iter;

    memset(&rpt, 0, sizeof(rpt));
    bench_samples_init(&s);
    rpt.requested_worker_count = requested_worker_count;
    rpt.samples = sample_count;
    rpt.task_count = ASX_MAX_TASKS;

    if (requested_worker_count == 0u || requested_worker_count > ASX_MAX_WORKERS) { return rpt; }

    rpt.worker_count = requested_worker_count;
    rpt.supported = 1;

    for (iter = 0; iter < sample_count; iter++) {
        asx_parallel_config cfg;
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1;
        uint32_t i;

        bench_parallel_config_init(&cfg, rpt.worker_count);
        asx_runtime_reset();
        asx_parallel_reset();
        (void)asx_parallel_init(&cfg);
        (void)asx_region_open(&rid);
        for (i = 0u; i < ASX_MAX_TASKS; i++) { (void)asx_task_spawn(rid, noop_poll, NULL, &tid); }

        (void)asx_parallel_get_telemetry_snapshot(&rpt.snapshot);
        rpt.peak_pressure_pct = rpt.snapshot.pressure_pct;
        rpt.peak_max_lane_depth = rpt.snapshot.max_lane_depth;
        rpt.peak_max_worker_queue_depth = rpt.snapshot.max_worker_queue_depth;

        budget = asx_budget_from_polls(ASX_MAX_TASKS * 4u);
        t0 = bench_now_ns();
        (void)asx_parallel_run(rid, &budget);
        t1 = bench_now_ns();
        bench_samples_add(&s, t1 - t0);

        (void)asx_parallel_get_telemetry_snapshot(&rpt.snapshot);
        if (rpt.snapshot.admission.pressure_pct > rpt.peak_pressure_pct) {
            rpt.peak_pressure_pct = rpt.snapshot.admission.pressure_pct;
            rpt.peak_max_lane_depth = rpt.snapshot.admission.queued;
            rpt.peak_max_worker_queue_depth =
                rpt.worker_count > 0u
                    ? (rpt.snapshot.admission.queued + rpt.worker_count - 1u) / rpt.worker_count
                    : rpt.snapshot.admission.queued;
        }
    }

    rpt.stats = bench_compute_stats(&s);
    rpt.cancel_stats = bench_parallel_cancel_latency(rpt.worker_count, BENCH_PARALLEL_AUX_SAMPLES);
    rpt.mpsc_stats = bench_parallel_mpsc_contention(rpt.worker_count, BENCH_PARALLEL_AUX_SAMPLES);
    rpt.scheduler_throughput_ops_per_sec = bench_ops_per_sec(rpt.task_count, &rpt.stats);
    rpt.cancel_latency_mean_ns =
        rpt.task_count > 0u ? rpt.cancel_stats.mean / (uint64_t)rpt.task_count : 0u;
    rpt.mpsc_contention_ops_per_sec =
        bench_ops_per_sec(ASX_CHANNEL_MAX_CAPACITY * 2u, &rpt.mpsc_stats);
    rpt.trace_commit_mean_ns = rpt.snapshot.metrics.commit_sequence > 0u
                                   ? rpt.stats.mean / (uint64_t)rpt.snapshot.metrics.commit_sequence
                                   : 0u;
    return rpt;
}

static bench_parallel_report bench_parallel_large_swarm(void) {
    uint32_t worker_count = ASX_MAX_WORKERS < ASX_PARALLEL_GENERIC_TARGET_WORKERS
                                ? ASX_MAX_WORKERS
                                : ASX_PARALLEL_GENERIC_TARGET_WORKERS;
    return bench_parallel_large_swarm_for_workers(worker_count, BENCH_PARALLEL_LARGE_SWARM_SAMPLES);
}

static bench_parallel_baselines bench_parallel_worker_baselines(void) {
    static const uint32_t workers[BENCH_PARALLEL_BASELINE_COUNT] = {1u, 2u, 8u, 32u, 64u};
    bench_parallel_baselines baselines;
    uint32_t i;

    memset(&baselines, 0, sizeof(baselines));
    baselines.count = BENCH_PARALLEL_BASELINE_COUNT;
    for (i = 0u; i < BENCH_PARALLEL_BASELINE_COUNT; i++) {
        baselines.rows[i] =
            bench_parallel_large_swarm_for_workers(workers[i], BENCH_PARALLEL_BASELINE_SAMPLES);
        if (baselines.rows[i].supported) { baselines.supported_count++; }
    }

    return baselines;
}

static int bench_parallel_threshold_pass(const bench_parallel_report *rpt) {
    if (!rpt->supported) { return 1; }
    if (rpt->stats.count == 0u) { return 0; }
    if (rpt->scheduler_throughput_ops_per_sec == 0u) { return 0; }
    if (rpt->mpsc_contention_ops_per_sec < BENCH_PARALLEL_MPSC_FLOOR_OPS_PER_SEC) { return 0; }
    if (rpt->cancel_latency_mean_ns > BENCH_PARALLEL_CANCEL_WARN_NS) { return 0; }
    if (rpt->trace_commit_mean_ns > BENCH_PARALLEL_TRACE_COMMIT_WARN_NS) { return 0; }
    return 1;
}

static void bench_print_parallel_baseline_json(const bench_parallel_report *rpt, int last) {
    printf("    {\n");
    printf("      \"requested_worker_count\": %" PRIu32 ",\n", rpt->requested_worker_count);
    printf("      \"worker_count\": %" PRIu32 ",\n", rpt->worker_count);
    printf("      \"supported\": %s,\n", rpt->supported ? "true" : "false");

    if (!rpt->supported) {
        printf(
            "      \"unsupported_reason\": \"requested_worker_count_exceeds_ASX_MAX_WORKERS\",\n");
        printf("      \"threshold_status\": \"skipped\"\n");
        printf("    }%s\n", last ? "" : ",");
        return;
    }

    printf("      \"sample_count\": %" PRIu32 ",\n", rpt->samples);
    printf("      \"task_count\": %" PRIu32 ",\n", rpt->task_count);
    printf("      \"seed\": %" PRIu32 ",\n", BENCH_PARALLEL_SEED);
    printf("      \"runtime_config\": {\n");
    printf("        \"fairness\": \"ROUND_ROBIN\",\n");
    printf("        \"lane_weights\": [1, 1, 1],\n");
    printf("        \"starvation_limit\": 8,\n");
    printf("        \"admission_mode\": \"observe_only\",\n");
    printf("        \"admission_pressure_threshold_pct\": %" PRIu32 ",\n",
           ASX_PARALLEL_DEFAULT_PRESSURE_THRESHOLD_PCT);
    printf("        \"locality_mode\": \"%s\",\n",
           asx_parallel_locality_mode_str(rpt->snapshot.locality.mode));
    printf("        \"locality_shard_count\": %" PRIu32 ",\n", rpt->snapshot.locality.shard_count);
    printf("        \"locality_tasks_per_shard\": %" PRIu32 "\n",
           rpt->snapshot.locality.tasks_per_shard);
    printf("      },\n");
    printf("      \"scheduler_stats\": {\n");
    bench_print_stats_members_json(&rpt->stats, "        ");
    printf("      },\n");
    printf("      \"scheduler_throughput_ops_per_sec\": %" PRIu64 ",\n",
           rpt->scheduler_throughput_ops_per_sec);
    printf("      \"cancel_latency\": {\n");
    printf("        \"mean_per_task_ns\": %" PRIu64 ",\n", rpt->cancel_latency_mean_ns);
    printf("        \"drain_stats\": {\n");
    bench_print_stats_members_json(&rpt->cancel_stats, "          ");
    printf("        }\n");
    printf("      },\n");
    printf("      \"mpsc_contention\": {\n");
    printf("        \"operations_per_sample\": %" PRIu32 ",\n", ASX_CHANNEL_MAX_CAPACITY * 2u);
    printf("        \"ops_per_sec\": %" PRIu64 ",\n", rpt->mpsc_contention_ops_per_sec);
    printf("        \"roundtrip_stats\": {\n");
    bench_print_stats_members_json(&rpt->mpsc_stats, "          ");
    printf("        }\n");
    printf("      },\n");
    printf("      \"telemetry\": {\n");
    printf("        \"final_pressure_pct\": %" PRIu32 ",\n", rpt->snapshot.pressure_pct);
    printf("        \"peak_pressure_pct\": %" PRIu32 ",\n", rpt->peak_pressure_pct);
    printf("        \"peak_max_lane_depth\": %" PRIu32 ",\n", rpt->peak_max_lane_depth);
    printf("        \"peak_max_worker_queue_depth\": %" PRIu32 ",\n",
           rpt->peak_max_worker_queue_depth);
    printf("        \"steal_attempts\": %" PRIu32 ",\n", rpt->snapshot.metrics.steal_attempts);
    printf("        \"steals_succeeded\": %" PRIu32 ",\n", rpt->snapshot.metrics.steals_succeeded);
    printf("        \"steals_failed\": %" PRIu32 ",\n", rpt->snapshot.metrics.steals_failed);
    printf("        \"steal_success_rate\": %.6f,\n",
           rpt->snapshot.metrics.steal_attempts > 0u
               ? (double)rpt->snapshot.metrics.steals_succeeded /
                     (double)rpt->snapshot.metrics.steal_attempts
               : 0.0);
    printf("        \"timed_wake_latency_rounds_max\": %" PRIu32 ",\n",
           rpt->snapshot.metrics.timed_wake_latency_rounds_max);
    printf("        \"blocking_backlog\": %" PRIu32 ",\n", rpt->snapshot.blocking_backlog);
    printf("        \"reactor_polls\": %" PRIu32 ",\n", rpt->snapshot.metrics.reactor_polls);
    printf("        \"reactor_ready\": %" PRIu32 ",\n", rpt->snapshot.metrics.reactor_ready);
    printf("        \"waker_ready\": %" PRIu32 ",\n", rpt->snapshot.metrics.waker_ready);
    printf("        \"worker_yields\": %" PRIu32 ",\n", rpt->snapshot.metrics.worker_yields);
    printf("        \"commit_sequence\": %" PRIu32 ",\n", rpt->snapshot.metrics.commit_sequence);
    printf("        \"locality_mode\": \"%s\",\n",
           asx_parallel_locality_mode_str(rpt->snapshot.locality.mode));
    printf("        \"locality_hot_shard\": %" PRIu32 ",\n", rpt->snapshot.locality.hot_shard);
    printf("        \"locality_max_shard_tasks\": %" PRIu32 "\n",
           rpt->snapshot.locality.max_shard_tasks);
    printf("      },\n");
    printf("      \"trace_commit_mean_ns\": %" PRIu64 ",\n", rpt->trace_commit_mean_ns);
    printf("      \"thresholds\": {\n");
    printf("        \"mode\": \"observe_only_gross_regression\",\n");
    printf("        \"scheduler_throughput_ops_per_sec_floor\": 1,\n");
    printf("        \"mpsc_contention_ops_per_sec_floor\": %" PRIu64 ",\n",
           BENCH_PARALLEL_MPSC_FLOOR_OPS_PER_SEC);
    printf("        \"cancel_latency_mean_ns_warn\": %" PRIu64 ",\n",
           BENCH_PARALLEL_CANCEL_WARN_NS);
    printf("        \"trace_commit_mean_ns_warn\": %" PRIu64 "\n",
           BENCH_PARALLEL_TRACE_COMMIT_WARN_NS);
    printf("      },\n");
    printf("      \"threshold_status\": \"%s\"\n",
           bench_parallel_threshold_pass(rpt) ? "pass" : "warn");
    printf("    }%s\n", last ? "" : ",");
}

/* -------------------------------------------------------------------
 * BENCH 13: Deadline miss measurement
 *
 * Measures: how many operations complete after their target deadline
 * under various load conditions.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t total_ops;
    uint32_t deadline_misses;
    uint64_t max_overshoot_ns;
    uint64_t mean_overshoot_ns;
} bench_deadline_report;

static bench_deadline_report bench_deadline_miss(void) {
    bench_deadline_report rpt;
    uint64_t overshoot_sum = 0;
    uint32_t iter;

    memset(&rpt, 0, sizeof(rpt));

    for (iter = 0; iter < 2000; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1, deadline_ns;

        asx_runtime_reset();
        (void)asx_region_open(&rid);
        (void)asx_task_spawn(rid, noop_poll, NULL, &tid);

        budget = asx_budget_from_polls(4);

        t0 = bench_now_ns();
        /* Target: complete within 10 microseconds */
        deadline_ns = t0 + UINT64_C(10000);

        (void)asx_scheduler_run(rid, &budget);
        t1 = bench_now_ns();

        rpt.total_ops++;
        if (t1 > deadline_ns) {
            uint64_t overshoot = t1 - deadline_ns;
            rpt.deadline_misses++;
            overshoot_sum += overshoot;
            if (overshoot > rpt.max_overshoot_ns) { rpt.max_overshoot_ns = overshoot; }
        }
    }

    if (rpt.deadline_misses > 0) {
        rpt.mean_overshoot_ns = overshoot_sum / (uint64_t)rpt.deadline_misses;
    }

    return rpt;
}

/* -------------------------------------------------------------------
 * BENCH 14: Adaptive evidence/fallback metrics
 *
 * Exercises the adaptive decision surface with deterministic posterior
 * regimes so CI can trend confidence/fallback behavior over time.
 * ------------------------------------------------------------------- */

#define BENCH_ADAPTIVE_DECISIONS 2048u

typedef struct {
    uint32_t decisions_total;
    uint32_t fallback_exercise_count;
    double fallback_rate;
    uint32_t confidence_threshold_fp32;
    uint32_t mean_confidence_fp32;
    uint32_t mean_expected_loss_fp16;
    uint64_t ledger_digest;
    uint32_t ledger_count;
    int ledger_overflowed;
} bench_adaptive_report;

typedef struct {
    uint32_t table[3][3];
} bench_adaptive_loss_ctx;

static uint32_t bench_adaptive_loss_fn(void *ctx, asx_adaptive_action action, uint8_t state_index) {
    const bench_adaptive_loss_ctx *loss = (const bench_adaptive_loss_ctx *)ctx;
    if (loss == NULL || action >= 3u || state_index >= 3u) { return UINT32_C(0xFFFFFFFF); }
    return loss->table[action][state_index];
}

static bench_adaptive_report bench_adaptive_metrics(void) {
    bench_adaptive_report rpt;
    bench_adaptive_loss_ctx loss_ctx;
    asx_adaptive_surface surface;
    asx_adaptive_policy policy;
    uint64_t confidence_sum = 0;
    uint64_t expected_loss_sum = 0;
    uint32_t i;

    memset(&rpt, 0, sizeof(rpt));
    memset(&loss_ctx, 0, sizeof(loss_ctx));
    memset(&surface, 0, sizeof(surface));
    memset(&policy, 0, sizeof(policy));

    /* Loss table is fixed-point 16.16. */
    loss_ctx.table[0][0] = UINT32_C(163840); /* 2.5 */
    loss_ctx.table[0][1] = UINT32_C(131072); /* 2.0 */
    loss_ctx.table[0][2] = UINT32_C(98304);  /* 1.5 */

    loss_ctx.table[1][0] = UINT32_C(85196);  /* 1.3 */
    loss_ctx.table[1][1] = UINT32_C(91750);  /* 1.4 */
    loss_ctx.table[1][2] = UINT32_C(104857); /* 1.6 */

    loss_ctx.table[2][0] = UINT32_C(52428);  /* 0.8 */
    loss_ctx.table[2][1] = UINT32_C(78643);  /* 1.2 */
    loss_ctx.table[2][2] = UINT32_C(235929); /* 3.6 */

    surface.name = "bench_adaptive_surface";
    surface.action_count = 3u;
    surface.state_count = 3u;
    surface.loss_fn = bench_adaptive_loss_fn;
    surface.loss_ctx = &loss_ctx;
    surface.fallback = 0u;

    policy.confidence_threshold_fp32 = UINT32_C(2576980377); /* 0.60 */
    policy.budget_remaining = 0u;

    asx_adaptive_reset();
    (void)asx_adaptive_set_policy(&policy);

    for (i = 0; i < BENCH_ADAPTIVE_DECISIONS; i++) {
        asx_adaptive_posterior posterior;
        asx_adaptive_evidence_term evidence[2];
        asx_adaptive_decision decision;
        asx_status st;

        memset(&posterior, 0, sizeof(posterior));
        memset(&decision, 0, sizeof(decision));
        posterior.state_count = 3u;

        switch (i & 3u) {
        case 0u:
            posterior.posterior[0] = UINT32_C(3006477107);    /* 0.70 */
            posterior.posterior[1] = UINT32_C(858993459);     /* 0.20 */
            posterior.posterior[2] = UINT32_C(429496729);     /* 0.10 */
            posterior.confidence_fp32 = UINT32_C(3951369912); /* 0.92 */
            break;
        case 1u:
            posterior.posterior[0] = UINT32_C(858993459);     /* 0.20 */
            posterior.posterior[1] = UINT32_C(2362232012);    /* 0.55 */
            posterior.posterior[2] = UINT32_C(1073741824);    /* 0.25 */
            posterior.confidence_fp32 = UINT32_C(3092376453); /* 0.72 */
            break;
        case 2u:
            posterior.posterior[0] = UINT32_C(429496729);     /* 0.10 */
            posterior.posterior[1] = UINT32_C(1073741824);    /* 0.25 */
            posterior.posterior[2] = UINT32_C(2791728742);    /* 0.65 */
            posterior.confidence_fp32 = UINT32_C(2018634629); /* 0.47 */
            break;
        default:
            posterior.posterior[0] = UINT32_C(2147483648);    /* 0.50 */
            posterior.posterior[1] = UINT32_C(1503238553);    /* 0.35 */
            posterior.posterior[2] = UINT32_C(644245094);     /* 0.15 */
            posterior.confidence_fp32 = UINT32_C(1503238553); /* 0.35 */
            break;
        }

        evidence[0].label = "confidence_fp32";
        evidence[0].value_fp32 = posterior.confidence_fp32;
        evidence[1].label = "synthetic_load_fp32";
        evidence[1].value_fp32 = (uint32_t)(((uint64_t)(i % 100u) * UINT32_C(4294967295)) / 100u);

        st = asx_adaptive_decide(&surface, &posterior, evidence, 2u, &decision);
        if (st != ASX_OK) { continue; }

        rpt.decisions_total++;
        confidence_sum += decision.confidence_fp32;
        expected_loss_sum += decision.expected_loss_fp16;
    }

    rpt.fallback_exercise_count = asx_adaptive_fallback_count();
    rpt.confidence_threshold_fp32 = policy.confidence_threshold_fp32;
    rpt.ledger_digest = asx_adaptive_ledger_digest();
    rpt.ledger_count = asx_adaptive_ledger_count();
    rpt.ledger_overflowed = asx_adaptive_ledger_overflowed();

    if (rpt.decisions_total > 0u) {
        rpt.fallback_rate = (double)rpt.fallback_exercise_count / (double)rpt.decisions_total;
        rpt.mean_confidence_fp32 = (uint32_t)(confidence_sum / (uint64_t)rpt.decisions_total);
        rpt.mean_expected_loss_fp16 = (uint32_t)(expected_loss_sum / (uint64_t)rpt.decisions_total);
    }

    return rpt;
}

/* -------------------------------------------------------------------
 * Cold-start report — measures full init-to-first-completion path
 *
 * Captures the latency of the first-use initialization sequence that
 * embedded and cold-start-sensitive deployments experience on boot.
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t init_p99_ns; /* p99 of runtime_reset + region_open */
    uint64_t init_p99_9_ns;
    uint64_t first_task_p99_ns; /* p99 of full cold-start-to-completion */
    uint64_t first_task_p99_9_ns;
    uint64_t init_jitter_ns;
    uint64_t first_task_jitter_ns;
    uint32_t samples;
} bench_cold_start_report;

static bench_cold_start_report bench_cold_start(void) {
    bench_cold_start_report rpt;
    bench_samples init_s;
    bench_samples full_s;
    bench_stats init_st;
    bench_stats full_st;
    uint32_t iter;

    bench_samples_init(&init_s);
    bench_samples_init(&full_s);

    for (iter = 0; iter < 5000u; iter++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0, t1, t2;

        t0 = bench_now_ns();
        asx_runtime_reset();
        (void)asx_region_open(&rid);
        t1 = bench_now_ns();

        (void)asx_task_spawn(rid, noop_poll, NULL, &tid);
        budget = asx_budget_from_polls(64);
        (void)asx_scheduler_run(rid, &budget);
        t2 = bench_now_ns();

        bench_samples_add(&init_s, t1 - t0);
        bench_samples_add(&full_s, t2 - t0);
    }

    init_st = bench_compute_stats(&init_s);
    full_st = bench_compute_stats(&full_s);

    memset(&rpt, 0, sizeof(rpt));
    rpt.init_p99_ns = init_st.p99;
    rpt.init_p99_9_ns = init_st.p99_9;
    rpt.init_jitter_ns = init_st.jitter;
    rpt.first_task_p99_ns = full_st.p99;
    rpt.first_task_p99_9_ns = full_st.p99_9;
    rpt.first_task_jitter_ns = full_st.jitter;
    rpt.samples = init_st.count;

    return rpt;
}

/* -------------------------------------------------------------------
 * Main — run all benchmarks and emit JSON report
 * ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    bench_stats st;
    bench_deadline_report dlr;
    bench_adaptive_report adr;
    bench_parallel_report plr;
    bench_parallel_baselines pbl;
    int json_only = 0;

    (void)argc;
    (void)argv;

    /* Check for --json flag */
    if (argc > 1 && strcmp(argv[1], "--json") == 0) { json_only = 1; }

    if (!json_only) {
        fprintf(stderr, "[asx-bench] Performance benchmark suite (bd-1md.6)\n");
        fprintf(stderr, "[asx-bench] ASX v%d.%d.%d\n", ASX_API_VERSION_MAJOR, ASX_API_VERSION_MINOR,
                ASX_API_VERSION_PATCH);
        fprintf(stderr, "[asx-bench] Running benchmarks...\n\n");
    }

    printf("{\n");
    printf("  \"version\": \"%d.%d.%d\",\n", ASX_API_VERSION_MAJOR, ASX_API_VERSION_MINOR,
           ASX_API_VERSION_PATCH);
    printf("  \"profile\": ");
    bench_print_json_string(bench_profile_name());
    printf(",\n");

    printf("  \"deterministic\": %d,\n", ASX_DETERMINISTIC);
    printf("  \"bench_metadata\": {\n");
    printf("    \"schema\": \"asx.bench_runtime.v2\",\n");
    printf("    \"codec\": ");
    bench_print_json_string(bench_codec_name());
    printf(",\n");
    printf("    \"compiler\": ");
    bench_print_json_string(bench_compiler_name());
    printf(",\n");
    printf("    \"compiler_version\": ");
    bench_print_json_string(bench_compiler_version());
    printf(",\n");
#if defined(__STDC_VERSION__)
    printf("    \"c_standard\": %ld,\n", (long)__STDC_VERSION__);
#else
    printf("    \"c_standard\": 0,\n");
#endif
    printf("    \"seed\": %" PRIu32 ",\n", BENCH_PARALLEL_SEED);
    printf("    \"rch_expected_command\": \"rch exec -- make parallel-bench-json "
           "PROFILE=PARALLEL\",\n");
    printf("    \"run_tag\": ");
    bench_print_json_string(getenv("ASX_CI_RUN_TAG"));
    printf(",\n");
    printf("    \"rch_worker_id\": ");
    bench_print_json_string(getenv("RCH_WORKER_ID"));
    printf("\n");
    printf("  },\n");

    printf("  \"benchmarks\": {\n");

    /* Scheduler benchmarks */
    if (!json_only) fprintf(stderr, "  scheduler_single_task... ");
    st = bench_scheduler_single_task();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("scheduler_single_task", &st, 0);

    if (!json_only) fprintf(stderr, "  scheduler_multi_task... ");
    st = bench_scheduler_multi_task();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("scheduler_multi_task", &st, 0);

    if (!json_only) fprintf(stderr, "  scheduler_multi_round... ");
    st = bench_scheduler_multi_round();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("scheduler_multi_round", &st, 0);

    /* Timer benchmarks */
    if (!json_only) fprintf(stderr, "  timer_register... ");
    st = bench_timer_register();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("timer_register", &st, 0);

    if (!json_only) fprintf(stderr, "  timer_cancel... ");
    st = bench_timer_cancel();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("timer_cancel", &st, 0);

    if (!json_only) fprintf(stderr, "  timer_collect_expired... ");
    st = bench_timer_collect();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("timer_collect_expired", &st, 0);

    /* Channel benchmarks */
    if (!json_only) fprintf(stderr, "  channel_send... ");
    st = bench_channel_send();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("channel_send", &st, 0);

    if (!json_only) fprintf(stderr, "  channel_recv... ");
    st = bench_channel_recv();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("channel_recv", &st, 0);

    /* Quiescence benchmark */
    if (!json_only) fprintf(stderr, "  quiescence_drain... ");
    st = bench_quiescence_drain();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("quiescence_drain", &st, 0);

    /* Budget algebra benchmark */
    if (!json_only) fprintf(stderr, "  budget_meet_1000x... ");
    st = bench_budget_meet();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("budget_meet_1000x", &st, 0);

    /* Embedded pressure benchmark */
    if (!json_only) fprintf(stderr, "  embedded_pressure... ");
    st = bench_embedded_pressure();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", st.p50);
    bench_print_stats_json("embedded_pressure", &st, 0);

    /* Parallel large-swarm benchmark */
    if (!json_only) fprintf(stderr, "  parallel_large_swarm... ");
    plr = bench_parallel_large_swarm();
    if (!json_only) fprintf(stderr, "done (p50=%" PRIu64 "ns)\n", plr.stats.p50);
    bench_print_stats_json("parallel_large_swarm", &plr.stats, 1);

    if (!json_only) fprintf(stderr, "  parallel_worker_baselines... ");
    pbl = bench_parallel_worker_baselines();
    if (!json_only) {
        fprintf(stderr, "done (%" PRIu32 "/%" PRIu32 " supported worker counts)\n",
                pbl.supported_count, pbl.count);
    }

    printf("  },\n");

    /* Deadline miss report */
    if (!json_only) fprintf(stderr, "  deadline_miss_10us... ");
    dlr = bench_deadline_miss();
    if (!json_only) {
        fprintf(stderr, "done (misses=%" PRIu32 "/%" PRIu32 ")\n", dlr.deadline_misses,
                dlr.total_ops);
    }

    printf("  \"deadline_report\": {\n");
    printf("    \"target_ns\": 10000,\n");
    printf("    \"total_ops\": %" PRIu32 ",\n", dlr.total_ops);
    printf("    \"deadline_misses\": %" PRIu32 ",\n", dlr.deadline_misses);
    printf("    \"miss_rate\": %.6f,\n",
           dlr.total_ops > 0 ? (double)dlr.deadline_misses / (double)dlr.total_ops : 0.0);
    printf("    \"max_overshoot_ns\": %" PRIu64 ",\n", dlr.max_overshoot_ns);
    printf("    \"mean_overshoot_ns\": %" PRIu64 "\n", dlr.mean_overshoot_ns);
    printf("  },\n");

    if (!json_only) fprintf(stderr, "  adaptive_decision_surface... ");
    adr = bench_adaptive_metrics();
    if (!json_only) {
        fprintf(stderr, "done (fallback=%" PRIu32 "/%" PRIu32 ", confidence_fp32=%" PRIu32 ")\n",
                adr.fallback_exercise_count, adr.decisions_total, adr.mean_confidence_fp32);
    }

    printf("  \"adaptive_report\": {\n");
    printf("    \"decisions_total\": %" PRIu32 ",\n", adr.decisions_total);
    printf("    \"fallback_exercise_count\": %" PRIu32 ",\n", adr.fallback_exercise_count);
    printf("    \"fallback_rate\": %.6f,\n", adr.fallback_rate);
    printf("    \"confidence_threshold_fp32\": %" PRIu32 ",\n", adr.confidence_threshold_fp32);
    printf("    \"mean_confidence_fp32\": %" PRIu32 ",\n", adr.mean_confidence_fp32);
    printf("    \"mean_expected_loss_fp16\": %" PRIu32 ",\n", adr.mean_expected_loss_fp16);
    printf("    \"ledger_digest\": \"0x%016" PRIx64 "\",\n", adr.ledger_digest);
    printf("    \"ledger_count\": %" PRIu32 ",\n", adr.ledger_count);
    printf("    \"ledger_overflowed\": %s\n", adr.ledger_overflowed ? "true" : "false");
    printf("  },\n");

    printf("  \"parallel_report\": {\n");
    printf("    \"worker_count\": %" PRIu32 ",\n", plr.worker_count);
    printf("    \"task_count\": %" PRIu32 ",\n", plr.task_count);
    printf("    \"final_pressure_pct\": %" PRIu32 ",\n", plr.snapshot.pressure_pct);
    printf("    \"peak_pressure_pct\": %" PRIu32 ",\n", plr.peak_pressure_pct);
    printf("    \"peak_max_lane_depth\": %" PRIu32 ",\n", plr.peak_max_lane_depth);
    printf("    \"peak_max_worker_queue_depth\": %" PRIu32 ",\n", plr.peak_max_worker_queue_depth);
    printf("    \"steal_attempts\": %" PRIu32 ",\n", plr.snapshot.metrics.steal_attempts);
    printf("    \"steals_succeeded\": %" PRIu32 ",\n", plr.snapshot.metrics.steals_succeeded);
    printf("    \"steals_failed\": %" PRIu32 ",\n", plr.snapshot.metrics.steals_failed);
    printf("    \"admission_rejects_observed\": %" PRIu32 ",\n",
           plr.snapshot.metrics.admission_rejects);
    printf("    \"pressure_transitions\": %" PRIu32 ",\n",
           plr.snapshot.metrics.pressure_transitions);
    printf("    \"admission_pressure_pct\": %" PRIu32 ",\n", plr.snapshot.admission.pressure_pct);
    printf("    \"admission_queued\": %" PRIu32 ",\n", plr.snapshot.admission.queued);
    printf("    \"admission_capacity\": %" PRIu32 ",\n", plr.snapshot.admission.capacity);
    printf("    \"admission_status_code\": %d,\n", (int)plr.snapshot.admission.admit_status);
    printf("    \"admission_status_text\": \"%s\",\n",
           asx_status_str(plr.snapshot.admission.admit_status));
    printf("    \"locality_mode\": \"%s\",\n",
           asx_parallel_locality_mode_str(plr.snapshot.locality.mode));
    printf("    \"locality_shard_count\": %" PRIu32 ",\n", plr.snapshot.locality.shard_count);
    printf("    \"locality_tasks_per_shard\": %" PRIu32 ",\n",
           plr.snapshot.locality.tasks_per_shard);
    printf("    \"locality_hot_shard\": %" PRIu32 ",\n", plr.snapshot.locality.hot_shard);
    printf("    \"locality_max_shard_tasks\": %" PRIu32 "\n",
           plr.snapshot.locality.max_shard_tasks);
    printf("  },\n");

    printf("  \"parallel_threshold_governance\": {\n");
    printf("    \"mode\": \"observe_only_gross_regression\",\n");
    printf("    \"status_field\": \"parallel_worker_baselines[].threshold_status\",\n");
    printf("    \"hard_fail_default\": false,\n");
    printf("    \"rationale\": \"surface gross regressions in benchmark artifacts before turning "
           "exploratory PARALLEL thresholds into blocking CI\"\n");
    printf("  },\n");

    printf("  \"parallel_worker_baselines\": [\n");
    {
        uint32_t i;

        for (i = 0u; i < pbl.count; i++) {
            bench_print_parallel_baseline_json(&pbl.rows[i], i + 1u == pbl.count);
        }
    }
    printf("  ],\n");

    /* Cold-start report */
    {
        bench_cold_start_report csr;

        if (!json_only) fprintf(stderr, "  cold_start... ");
        csr = bench_cold_start();
        if (!json_only) {
            fprintf(stderr, "done (init_p99=%" PRIu64 "ns, first_task_p99=%" PRIu64 "ns)\n",
                    csr.init_p99_ns, csr.first_task_p99_ns);
        }

        printf("  \"cold_start_report\": {\n");
        printf("    \"samples\": %" PRIu32 ",\n", csr.samples);
        printf("    \"init_p99_ns\": %" PRIu64 ",\n", csr.init_p99_ns);
        printf("    \"init_p99_9_ns\": %" PRIu64 ",\n", csr.init_p99_9_ns);
        printf("    \"init_jitter_ns\": %" PRIu64 ",\n", csr.init_jitter_ns);
        printf("    \"first_task_p99_ns\": %" PRIu64 ",\n", csr.first_task_p99_ns);
        printf("    \"first_task_p99_9_ns\": %" PRIu64 ",\n", csr.first_task_p99_9_ns);
        printf("    \"first_task_jitter_ns\": %" PRIu64 "\n", csr.first_task_jitter_ns);
        printf("  }\n");
    }

    printf("}\n");

    if (!json_only) { fprintf(stderr, "\n[asx-bench] All benchmarks complete.\n"); }

    return 0;
}
