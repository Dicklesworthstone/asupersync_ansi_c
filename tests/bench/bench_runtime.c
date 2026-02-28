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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>
#include <asx/core/channel.h>
#include <asx/core/adaptive.h>
#include <asx/core/budget.h>

/* -------------------------------------------------------------------
 * Timing helpers
 * ------------------------------------------------------------------- */

static uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
         + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------
 * Sample collection and statistics
 * ------------------------------------------------------------------- */

#define BENCH_MAX_SAMPLES 10000u

typedef struct {
    uint64_t samples[BENCH_MAX_SAMPLES];
    uint32_t count;
} bench_samples;

static void bench_samples_init(bench_samples *s)
{
    s->count = 0;
}

static void bench_samples_add(bench_samples *s, uint64_t val)
{
    if (s->count < BENCH_MAX_SAMPLES) {
        s->samples[s->count] = val;
    }
    s->count++;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return  1;
    return 0;
}

static void bench_samples_sort(bench_samples *s)
{
    uint32_t n = s->count < BENCH_MAX_SAMPLES ? s->count : BENCH_MAX_SAMPLES;
    qsort(s->samples, (size_t)n, sizeof(uint64_t), cmp_u64);
}

static uint64_t bench_percentile(const bench_samples *s, double p)
{
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
    uint64_t jitter;  /* mean absolute deviation */
    uint32_t count;
} bench_stats;

static bench_stats bench_compute_stats(bench_samples *s)
{
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

    for (i = 0; i < n; i++) {
        sum += s->samples[i];
    }
    st.mean = sum / (uint64_t)n;

    for (i = 0; i < n; i++) {
        uint64_t diff = s->samples[i] > st.mean
                      ? s->samples[i] - st.mean
                      : st.mean - s->samples[i];
        jitter_sum += diff;
    }
    st.jitter = jitter_sum / (uint64_t)n;

    st.p50    = bench_percentile(s, 50.0);
    st.p95    = bench_percentile(s, 95.0);
    st.p99    = bench_percentile(s, 99.0);
    st.p99_9  = bench_percentile(s, 99.9);
    st.p99_99 = bench_percentile(s, 99.99);
    st.count  = n;

    return st;
}

static void bench_print_stats_json(const char *name, const bench_stats *st,
                                   int last)
{
    printf("    \"%s\": {\n", name);
    printf("      \"count\": %" PRIu32 ",\n", st->count);
    printf("      \"mean_ns\": %" PRIu64 ",\n", st->mean);
    printf("      \"min_ns\": %" PRIu64 ",\n", st->min_val);
    printf("      \"max_ns\": %" PRIu64 ",\n", st->max_val);
    printf("      \"p50_ns\": %" PRIu64 ",\n", st->p50);
    printf("      \"p95_ns\": %" PRIu64 ",\n", st->p95);
    printf("      \"p99_ns\": %" PRIu64 ",\n", st->p99);
    printf("      \"p99_9_ns\": %" PRIu64 ",\n", st->p99_9);
    printf("      \"p99_99_ns\": %" PRIu64 ",\n", st->p99_99);
    printf("      \"jitter_ns\": %" PRIu64 "\n", st->jitter);
    printf("    }%s\n", last ? "" : ",");
}

/* -------------------------------------------------------------------
 * Noop poll function for scheduler benchmarks
 * ------------------------------------------------------------------- */

static asx_status noop_poll(void *user_data, asx_task_id self)
{
    (void)user_data;
    (void)self;
    return ASX_OK;
}

/* Poll function that returns PENDING N times before OK */
typedef struct {
    int remaining;
} countdown_ctx;

static asx_status countdown_poll(void *user_data, asx_task_id self)
{
    countdown_ctx *ctx = (countdown_ctx *)user_data;
    (void)self;
    if (ctx->remaining > 0) {
        ctx->remaining--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * BENCH 1: Scheduler — single-task single-poll throughput
 *
 * Measures: time to spawn one task, run scheduler to completion,
 * and verify quiescence. This is the scheduler hot path.
 * ------------------------------------------------------------------- */

static bench_stats bench_scheduler_single_task(void)
{
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

static bench_stats bench_scheduler_multi_task(void)
{
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

static bench_stats bench_scheduler_multi_round(void)
{
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
            (void)asx_task_spawn(rid, countdown_poll,
                                 &ctxs[t_i], &tid);
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

static bench_stats bench_timer_register(void)
{
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
            (void)asx_timer_register(w,
                                     (asx_time)(t_i + 1u) * 1000u,
                                     NULL, &h);
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

static bench_stats bench_timer_cancel(void)
{
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
            (void)asx_timer_register(w,
                                     (asx_time)(t_i + 1u) * 1000u,
                                     NULL, &handles[t_i]);
        }

        t0 = bench_now_ns();
        for (t_i = 0; t_i < ASX_MAX_TIMERS; t_i++) {
            (void)asx_timer_cancel(w, &handles[t_i]);
        }
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

static bench_stats bench_timer_collect(void)
{
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
            (void)asx_timer_register(w,
                                     (asx_time)((t_i % 8u) + 1u) * 100u,
                                     NULL, &h);
        }

        t0 = bench_now_ns();
        (void)asx_timer_collect_expired(w,
                                        (asx_time)10000u,
                                        wakers,
                                        ASX_MAX_TIMERS);
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

static bench_stats bench_channel_send(void)
{
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

static bench_stats bench_channel_recv(void)
{
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

static bench_stats bench_quiescence_drain(void)
{
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
        for (t_i = 0; t_i < 8; t_i++) {
            (void)asx_task_spawn(rid, noop_poll, NULL, &tid);
        }

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

static bench_stats bench_budget_meet(void)
{
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
        if (result.poll_quota == UINT32_MAX) {
            fprintf(stderr, "unexpected\n");
        }

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

static bench_stats bench_embedded_pressure(void)
{
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
            (void)asx_task_spawn(rid, countdown_poll,
                                 &ctxs[t_i], &tid);
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
 * BENCH 12: Deadline miss measurement
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

static bench_deadline_report bench_deadline_miss(void)
{
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
            if (overshoot > rpt.max_overshoot_ns) {
                rpt.max_overshoot_ns = overshoot;
            }
        }
    }

    if (rpt.deadline_misses > 0) {
        rpt.mean_overshoot_ns = overshoot_sum / (uint64_t)rpt.deadline_misses;
    }

    return rpt;
}

/* -------------------------------------------------------------------
 * BENCH 13: Adaptive evidence/fallback metrics
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

static uint32_t bench_adaptive_loss_fn(void *ctx,
                                       asx_adaptive_action action,
                                       uint8_t state_index)
{
    const bench_adaptive_loss_ctx *loss = (const bench_adaptive_loss_ctx *)ctx;
    if (loss == NULL || action >= 3u || state_index >= 3u) {
        return UINT32_C(0xFFFFFFFF);
    }
    return loss->table[action][state_index];
}

static bench_adaptive_report bench_adaptive_metrics(void)
{
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
            posterior.posterior[0] = UINT32_C(3006477107); /* 0.70 */
            posterior.posterior[1] = UINT32_C(858993459);  /* 0.20 */
            posterior.posterior[2] = UINT32_C(429496729);  /* 0.10 */
            posterior.confidence_fp32 = UINT32_C(3951369912); /* 0.92 */
            break;
        case 1u:
            posterior.posterior[0] = UINT32_C(858993459);  /* 0.20 */
            posterior.posterior[1] = UINT32_C(2362232012); /* 0.55 */
            posterior.posterior[2] = UINT32_C(1073741824); /* 0.25 */
            posterior.confidence_fp32 = UINT32_C(3092376453); /* 0.72 */
            break;
        case 2u:
            posterior.posterior[0] = UINT32_C(429496729);  /* 0.10 */
            posterior.posterior[1] = UINT32_C(1073741824); /* 0.25 */
            posterior.posterior[2] = UINT32_C(2791728742); /* 0.65 */
            posterior.confidence_fp32 = UINT32_C(2018634629); /* 0.47 */
            break;
        default:
            posterior.posterior[0] = UINT32_C(2147483648); /* 0.50 */
            posterior.posterior[1] = UINT32_C(1503238553); /* 0.35 */
            posterior.posterior[2] = UINT32_C(644245094);  /* 0.15 */
            posterior.confidence_fp32 = UINT32_C(1503238553); /* 0.35 */
            break;
        }

        evidence[0].label = "confidence_fp32";
        evidence[0].value_fp32 = posterior.confidence_fp32;
        evidence[1].label = "synthetic_load_fp32";
        evidence[1].value_fp32 = (uint32_t)(((uint64_t)(i % 100u) * UINT32_C(4294967295)) / 100u);

        st = asx_adaptive_decide(&surface, &posterior, evidence, 2u, &decision);
        if (st != ASX_OK) {
            continue;
        }

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
        rpt.mean_confidence_fp32 =
            (uint32_t)(confidence_sum / (uint64_t)rpt.decisions_total);
        rpt.mean_expected_loss_fp16 =
            (uint32_t)(expected_loss_sum / (uint64_t)rpt.decisions_total);
    }

    return rpt;
}

/* -------------------------------------------------------------------
 * Main — run all benchmarks and emit JSON report
 * ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    bench_stats st;
    bench_deadline_report dlr;
    bench_adaptive_report adr;
    int json_only = 0;

    (void)argc;
    (void)argv;

    /* Check for --json flag */
    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        json_only = 1;
    }

    if (!json_only) {
        fprintf(stderr, "[asx-bench] Performance benchmark suite (bd-1md.6)\n");
        fprintf(stderr, "[asx-bench] ASX v%d.%d.%d\n",
                ASX_API_VERSION_MAJOR, ASX_API_VERSION_MINOR,
                ASX_API_VERSION_PATCH);
        fprintf(stderr, "[asx-bench] Running benchmarks...\n\n");
    }

    printf("{\n");
    printf("  \"version\": \"%d.%d.%d\",\n",
           ASX_API_VERSION_MAJOR, ASX_API_VERSION_MINOR,
           ASX_API_VERSION_PATCH);
    printf("  \"profile\": \"");
#if defined(ASX_PROFILE_EMBEDDED_ROUTER)
    printf("EMBEDDED_ROUTER");
#elif defined(ASX_PROFILE_HFT)
    printf("HFT");
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    printf("AUTOMOTIVE");
#elif defined(ASX_PROFILE_POSIX)
    printf("POSIX");
#elif defined(ASX_PROFILE_WIN32)
    printf("WIN32");
#elif defined(ASX_PROFILE_FREESTANDING)
    printf("FREESTANDING");
#else
    printf("CORE");
#endif
    printf("\",\n");

    printf("  \"deterministic\": %d,\n", ASX_DETERMINISTIC);

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
    bench_print_stats_json("embedded_pressure", &st, 1);

    printf("  },\n");

    /* Deadline miss report */
    if (!json_only) fprintf(stderr, "  deadline_miss_10us... ");
    dlr = bench_deadline_miss();
    if (!json_only) {
        fprintf(stderr, "done (misses=%"PRIu32"/%"PRIu32")\n",
                dlr.deadline_misses, dlr.total_ops);
    }

    printf("  \"deadline_report\": {\n");
    printf("    \"target_ns\": 10000,\n");
    printf("    \"total_ops\": %" PRIu32 ",\n", dlr.total_ops);
    printf("    \"deadline_misses\": %" PRIu32 ",\n", dlr.deadline_misses);
    printf("    \"miss_rate\": %.6f,\n",
           dlr.total_ops > 0
               ? (double)dlr.deadline_misses / (double)dlr.total_ops
               : 0.0);
    printf("    \"max_overshoot_ns\": %" PRIu64 ",\n",
           dlr.max_overshoot_ns);
    printf("    \"mean_overshoot_ns\": %" PRIu64 "\n",
           dlr.mean_overshoot_ns);
    printf("  },\n");

    if (!json_only) fprintf(stderr, "  adaptive_decision_surface... ");
    adr = bench_adaptive_metrics();
    if (!json_only) {
        fprintf(stderr,
                "done (fallback=%" PRIu32 "/%" PRIu32 ", confidence_fp32=%" PRIu32 ")\n",
                adr.fallback_exercise_count, adr.decisions_total,
                adr.mean_confidence_fp32);
    }

    printf("  \"adaptive_report\": {\n");
    printf("    \"decisions_total\": %" PRIu32 ",\n", adr.decisions_total);
    printf("    \"fallback_exercise_count\": %" PRIu32 ",\n",
           adr.fallback_exercise_count);
    printf("    \"fallback_rate\": %.6f,\n", adr.fallback_rate);
    printf("    \"confidence_threshold_fp32\": %" PRIu32 ",\n",
           adr.confidence_threshold_fp32);
    printf("    \"mean_confidence_fp32\": %" PRIu32 ",\n",
           adr.mean_confidence_fp32);
    printf("    \"mean_expected_loss_fp16\": %" PRIu32 ",\n",
           adr.mean_expected_loss_fp16);
    printf("    \"ledger_digest\": \"0x%016" PRIx64 "\",\n", adr.ledger_digest);
    printf("    \"ledger_count\": %" PRIu32 ",\n", adr.ledger_count);
    printf("    \"ledger_overflowed\": %s\n",
           adr.ledger_overflowed ? "true" : "false");
    printf("  }\n");

    printf("}\n");

    if (!json_only) {
        fprintf(stderr, "\n[asx-bench] All benchmarks complete.\n");
    }

    return 0;
}
