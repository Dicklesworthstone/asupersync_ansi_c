/*
 * asx_workloads.c — asx's side of the C-vs-Rust comparison (bd-9kll.14.1)
 *
 * tools/perf_compare is a Rust program that runs the same workloads on
 * asupersync (its current-thread runtime) and on asx, in one process, one
 * sample of each in turn, so both engines see the same host and the same
 * minute. It reaches asx through this file: every function runs one workload
 * for about `n` operations on the library the Makefile built (the same
 * profile, codec, class and flags: this file is compiled with the library's
 * ALL_CFLAGS, because the arena sizes the workloads read are macros), and
 * reports the nanoseconds the operations took and how many it did.
 *
 * What is timed matches what runtime_vs_tokio.rs times on the Rust side:
 * the operations, not the setup. A reset of the runtime between batches is
 * outside the clock. The asx arenas are fixed, so a workload that holds many
 * live objects at once (spawn+join, timers, regions) runs in batches no
 * larger than the arena; the batch sizes are reported by asx_perf_describe,
 * and the Rust side uses the same batch for its timer workload.
 *
 * The functions return 0, or a small nonzero code naming the step that
 * failed (the numbers are then meaningless and the harness says so).
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/core/channel.h>
#include <asx/cx/cx.h>
#include <asx/runtime/runtime.h>
#include <asx/time/timer_wheel.h>

/* The ABI the Rust side declares (src/main.rs, `unsafe extern "C"`). */
int asx_perf_spawn_join(uint64_t n, uint64_t *out_ns, uint64_t *out_ops);
int asx_perf_yield(uint64_t n, uint64_t *out_ns, uint64_t *out_ops);
int asx_perf_mpsc_ping_pong(uint64_t n, uint64_t *out_ns, uint64_t *out_ops);
int asx_perf_cancel_latency(uint64_t n, uint64_t *out_ns, uint64_t *out_ops);
int asx_perf_timer_churn(uint64_t n, uint64_t *out_ns, uint64_t *out_ops);
int asx_perf_region_open_close(uint64_t n, uint64_t *out_ns, uint64_t *out_ops);
uint32_t asx_perf_task_batch(void);
uint32_t asx_perf_timer_batch(void);
uint32_t asx_perf_region_batch(void);
uint32_t asx_perf_describe(char *buf, uint32_t cap);

/* The most tasks a spawn+join batch holds live: the arena, at most 32. */
#if ASX_MAX_TASKS < 32
#define PERF_TASK_BATCH ASX_MAX_TASKS
#else
#define PERF_TASK_BATCH 32
#endif

/* Timers armed and cancelled per batch: the arena. */
#define PERF_TIMER_BATCH ASX_MAX_TIMERS

/* Child regions opened per batch: every region of the arena but the root
 * (a closed region keeps its slot until the runtime is reset). */
#define PERF_REGION_BATCH (ASX_MAX_REGIONS - 1)

/* A zeroed runtime object names the library's static kernel (tag 0). */
static asx_runtime g_rt;

static uint64_t perf_now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* A budget that outlasts `polls` polls, saturating. */
static asx_budget perf_polls(uint64_t polls) {
    return asx_budget_from_polls(polls > UINT32_MAX - 16u ? UINT32_MAX : (uint32_t)polls + 16u);
}

/* ------------------------------------------------------------------ */
/* spawn + join                                                        */
/* ------------------------------------------------------------------ */

static asx_status poll_noop(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

/* A parent spawns tasks that return at once and joins every one: n
 * spawn+join pairs, PERF_TASK_BATCH live at a time. Rust's counterpart is
 * Cx::spawn followed by TaskHandle::join. */
int asx_perf_spawn_join(uint64_t n, uint64_t *out_ns, uint64_t *out_ops) {
    asx_region_id rid;
    asx_task_id tids[PERF_TASK_BATCH];
    uint64_t done = 0u;
    uint64_t t0;
    uint64_t t1;

    asx_runtime_reset(&g_rt);
    if (asx_region_open(&g_rt, &rid) != ASX_OK) return 1;
    t0 = perf_now_ns();
    while (done < n) {
        uint32_t batch = (n - done) < (uint64_t)PERF_TASK_BATCH ? (uint32_t)(n - done)
                                                                : (uint32_t)PERF_TASK_BATCH;
        asx_budget budget;
        uint32_t i;
        for (i = 0u; i < batch; i++) {
            if (asx_task_spawn(rid, poll_noop, NULL, &tids[i]) != ASX_OK) return 2;
        }
        budget = perf_polls((uint64_t)batch * 2u);
        if (asx_scheduler_run(rid, &budget) != ASX_OK) return 3;
        for (i = 0u; i < batch; i++) {
            if (asx_task_join(tids[i], NULL) != ASX_OK) return 4;
        }
        done += batch;
    }
    t1 = perf_now_ns();
    *out_ns = t1 - t0;
    *out_ops = done;
    return 0;
}

/* ------------------------------------------------------------------ */
/* yield                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t remaining;
} yield_state;

static asx_status poll_yield(void *data, asx_task_id self) {
    yield_state *s = (yield_state *)data;
    (void)self;
    if (s->remaining == 0u) return ASX_OK;
    s->remaining--;
    return ASX_E_PENDING;
}

/* One task yields n times (returns PENDING, to be polled again). Rust's
 * counterpart is runtime::yield_now().await in a loop. */
int asx_perf_yield(uint64_t n, uint64_t *out_ns, uint64_t *out_ops) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    yield_state st;
    uint64_t t0;
    uint64_t t1;

    asx_runtime_reset(&g_rt);
    if (asx_region_open(&g_rt, &rid) != ASX_OK) return 1;
    st.remaining = n;
    if (asx_task_spawn(rid, poll_yield, &st, &tid) != ASX_OK) return 2;
    budget = perf_polls(n + 1u);
    t0 = perf_now_ns();
    if (asx_scheduler_run(rid, &budget) != ASX_OK) return 3;
    t1 = perf_now_ns();
    *out_ns = t1 - t0;
    *out_ops = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* mpsc ping-pong                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_region_id region;
    asx_channel_id ping;
    asx_channel_id pong;
    uint64_t n;
    uint64_t i;
    uint64_t acc;
    uint32_t phase; /* 0: send the ping, 1: receive the pong */
} pp_main;

typedef struct {
    asx_region_id region;
    asx_channel_id ping;
    asx_channel_id pong;
    uint64_t value;
    uint32_t phase; /* 0: receive a ping, 1: send the pong */
} pp_echo;

static asx_status poll_pp_main(void *data, asx_task_id self) {
    pp_main *m = (pp_main *)data;
    asx_cx cx;
    asx_status st;
    uint64_t v;
    st = asx_cx_init(&cx, m->region, self, ASX_CAP_NONE);
    if (st != ASX_OK) return st;
    while (m->i < m->n) {
        if (m->phase == 0u) {
            st = asx_channel_send(m->ping, &cx, m->i);
            if (st == ASX_E_PENDING) return ASX_E_PENDING;
            if (st != ASX_OK) return st;
            m->phase = 1u;
        }
        st = asx_channel_recv(m->pong, &cx, &v);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) return st;
        m->acc += v;
        m->phase = 0u;
        m->i++;
    }
    /* Close the ping side so the echo task ends and the run quiesces. */
    st = asx_channel_close_sender(m->ping);
    return st;
}

static asx_status poll_pp_echo(void *data, asx_task_id self) {
    pp_echo *e = (pp_echo *)data;
    asx_cx cx;
    asx_status st;
    st = asx_cx_init(&cx, e->region, self, ASX_CAP_NONE);
    if (st != ASX_OK) return st;
    for (;;) {
        if (e->phase == 0u) {
            st = asx_channel_recv(e->ping, &cx, &e->value);
            if (st == ASX_E_PENDING) return ASX_E_PENDING;
            if (st == ASX_E_DISCONNECTED) return ASX_OK;
            if (st != ASX_OK) return st;
            e->phase = 1u;
        }
        st = asx_channel_send(e->pong, &cx, e->value + 1u);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) return st;
        e->phase = 0u;
    }
}

/* Two tasks exchange a value n times over capacity-1 channels: one
 * operation is one round trip (a send, the echo's receive and send, the
 * receive). Rust's counterpart is runtime_vs_tokio.rs's mpsc_ping_pong. */
int asx_perf_mpsc_ping_pong(uint64_t n, uint64_t *out_ns, uint64_t *out_ops) {
    asx_region_id rid;
    asx_task_id tmain;
    asx_task_id techo;
    asx_budget budget;
    pp_main m;
    pp_echo e;
    uint64_t t0;
    uint64_t t1;

    asx_runtime_reset(&g_rt);
    if (asx_region_open(&g_rt, &rid) != ASX_OK) return 1;
    memset(&m, 0, sizeof(m));
    memset(&e, 0, sizeof(e));
    m.region = rid;
    e.region = rid;
    if (asx_channel_create(rid, 1u, &m.ping) != ASX_OK) return 2;
    if (asx_channel_create(rid, 1u, &m.pong) != ASX_OK) return 3;
    e.ping = m.ping;
    e.pong = m.pong;
    m.n = n;
    if (asx_task_spawn(rid, poll_pp_echo, &e, &techo) != ASX_OK) return 4;
    if (asx_task_spawn(rid, poll_pp_main, &m, &tmain) != ASX_OK) return 5;
    budget = perf_polls(n * 8u + 64u);
    t0 = perf_now_ns();
    if (asx_scheduler_run(rid, &budget) != ASX_OK) return 6;
    t1 = perf_now_ns();
    if (m.i != n) return 7;
    *out_ns = t1 - t0;
    *out_ops = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* cancel latency                                                      */
/* ------------------------------------------------------------------ */

static asx_status poll_checkpoint_loop(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    return ASX_E_PENDING;
}

/* n times: a task that has started and loops on a checkpoint is asked to
 * cancel; the time from the request to the task being completed (the
 * scheduler run that acknowledges, finalizes and completes it) is the
 * operation. Spawning and joining are outside the clock. Rust's counterpart
 * is TaskHandle::abort_with_reason followed by join. */
int asx_perf_cancel_latency(uint64_t n, uint64_t *out_ns, uint64_t *out_ops) {
    asx_region_id rid;
    uint64_t total = 0u;
    uint64_t i;

    asx_runtime_reset(&g_rt);
    if (asx_region_open(&g_rt, &rid) != ASX_OK) return 1;
    for (i = 0u; i < n; i++) {
        asx_task_id tid;
        asx_budget budget;
        uint64_t t0;
        uint64_t t1;
        asx_status st;
        if (asx_task_spawn(rid, poll_checkpoint_loop, NULL, &tid) != ASX_OK) return 2;
        /* One poll, so the task has started; the run then ends on its budget. */
        budget = asx_budget_from_polls(1u);
        st = asx_scheduler_run(rid, &budget);
        if (st != ASX_E_POLL_BUDGET_EXHAUSTED && st != ASX_OK) return 3;
        t0 = perf_now_ns();
        if (asx_task_cancel(tid, ASX_CANCEL_USER) != ASX_OK) return 4;
        budget = asx_budget_from_polls(16u);
        if (asx_scheduler_run(rid, &budget) != ASX_OK) return 5;
        t1 = perf_now_ns();
        total += t1 - t0;
        if (asx_task_join(tid, NULL) != ASX_OK) return 6;
    }
    *out_ns = total;
    *out_ops = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* timer churn                                                         */
/* ------------------------------------------------------------------ */

/* Arm PERF_TIMER_BATCH timers, then cancel them all, repeated until n timers
 * have been armed and cancelled: one operation is one arm plus one cancel.
 * Rust's counterpart is TimerWheel::register and TimerWheel::cancel. The
 * wheel's own storage is what is exercised (no task, no clock hook). */
int asx_perf_timer_churn(uint64_t n, uint64_t *out_ns, uint64_t *out_ops) {
    asx_timer_wheel *wheel = asx_timer_wheel_global(&g_rt);
    asx_timer_handle handles[PERF_TIMER_BATCH];
    uint64_t total = 0u;
    uint64_t done = 0u;

    while (done < n) {
        uint32_t i;
        uint64_t t0;
        uint64_t t1;
        asx_timer_wheel_reset(wheel);
        t0 = perf_now_ns();
        for (i = 0u; i < (uint32_t)PERF_TIMER_BATCH; i++) {
            asx_time deadline = ((asx_time)100u + (asx_time)i) * UINT64_C(1000000);
            if (asx_timer_register(wheel, deadline, NULL, &handles[i]) != ASX_OK) return 1;
        }
        for (i = 0u; i < (uint32_t)PERF_TIMER_BATCH; i++) {
            if (asx_timer_cancel(wheel, &handles[i]) == 0) return 2;
        }
        t1 = perf_now_ns();
        total += t1 - t0;
        done += (uint64_t)PERF_TIMER_BATCH;
    }
    asx_timer_wheel_reset(wheel);
    *out_ns = total;
    *out_ops = done;
    return 0;
}

/* ------------------------------------------------------------------ */
/* region open + close                                                 */
/* ------------------------------------------------------------------ */

/* Open a child region and close it (drain: nothing in it). A closed region
 * keeps its slot until the runtime is reset, so the arena allows
 * PERF_REGION_BATCH of them per reset; the reset is outside the clock. */
int asx_perf_region_open_close(uint64_t n, uint64_t *out_ns, uint64_t *out_ops) {
    uint64_t total = 0u;
    uint64_t done = 0u;

    while (done < n) {
        asx_region_id root;
        uint32_t i;
        uint64_t t0;
        uint64_t t1;
        asx_runtime_reset(&g_rt);
        if (asx_region_open(&g_rt, &root) != ASX_OK) return 1;
        t0 = perf_now_ns();
        for (i = 0u; i < (uint32_t)PERF_REGION_BATCH; i++) {
            asx_region_id child;
            asx_budget budget = asx_budget_from_polls(8u);
            if (asx_region_open_child(root, &child) != ASX_OK) return 2;
            if (asx_region_drain(child, &budget) != ASX_OK) return 3;
        }
        t1 = perf_now_ns();
        total += t1 - t0;
        done += (uint64_t)PERF_REGION_BATCH;
    }
    *out_ns = total;
    *out_ops = done;
    return 0;
}

/* ------------------------------------------------------------------ */
/* What was built                                                      */
/* ------------------------------------------------------------------ */

uint32_t asx_perf_task_batch(void) { return (uint32_t)PERF_TASK_BATCH; }
uint32_t asx_perf_timer_batch(void) { return (uint32_t)PERF_TIMER_BATCH; }
uint32_t asx_perf_region_batch(void) { return (uint32_t)PERF_REGION_BATCH; }

/* One line of what the library under test is, for the report's host
 * fingerprint: copied into buf (at most cap bytes, NUL-terminated); returns
 * the length written. */
uint32_t asx_perf_describe(char *buf, uint32_t cap) {
    int len;
    if (buf == NULL || cap == 0u) return 0u;
#if defined(__VERSION__)
#define PERF_CC_VERSION __VERSION__
#else
#define PERF_CC_VERSION "unknown"
#endif
    len = snprintf(buf, (size_t)cap,
                   "cc=\"%s\" max_regions=%u max_tasks=%u max_timers=%u max_channels=%u "
                   "task_batch=%u timer_batch=%u region_batch=%u"
#if defined(NDEBUG)
                   " ndebug=1"
#else
                   " ndebug=0"
#endif
                   ,
                   PERF_CC_VERSION, (unsigned)ASX_MAX_REGIONS, (unsigned)ASX_MAX_TASKS,
                   (unsigned)ASX_MAX_TIMERS, (unsigned)ASX_MAX_CHANNELS, (unsigned)PERF_TASK_BATCH,
                   (unsigned)PERF_TIMER_BATCH, (unsigned)PERF_REGION_BATCH);
    if (len < 0) {
        buf[0] = '\0';
        return 0u;
    }
    return (uint32_t)len < cap ? (uint32_t)len : cap - 1u;
}
