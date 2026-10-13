/*
 * bench_sched_lanes.c — what the sweep's dispatch order costs (bd-9kll.4.6)
 *
 * The sweep scheduler (asx_scheduler_run, no lab dispatch) polls every
 * runnable task once per round. Since bd-9kll.4.6 it does so by lane: the
 * cancel lane, then the timed lane (a due wake timer's task, earliest
 * deadline first), then the ready lane by budget priority. This program times
 * four workloads whose polls are the same in every build and only their order
 * can differ, and prints ns per poll for each:
 *
 *   ready_equal   T tasks of one priority, each yielding Y times
 *   ready_mixed   the same with eight priorities spread over the arena
 *   timed         T tasks each sleeping K times to staggered deadlines, so
 *                 every round wakes a group of timers (the timed lane)
 *   cancel        the ready_equal tasks with every other one cancelled
 *
 * Each workload is repeated --reps times and the median repetition is kept.
 * tests/bench/sched_lanes_ab.sh builds this against the library three ways
 * (the arena-order walk of before 4.6, the lanes without the timed lane, and
 * the lanes with it) and compares the medians of interleaved runs.
 *
 *   build/bench-sched-ab/<variant>/bench_sched_lanes [--reps N] [--tasks T]
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

#include "../test_runtime.h"
#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/runtime/runtime.h>

#define WL_COUNT 4u
#define YIELDS 32
#define SLEEPS 16

static const char *const g_wl_name[WL_COUNT] = {"ready_equal", "ready_mixed", "timed", "cancel"};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

typedef struct {
    int yields_left;
    int sleeps_left;
    int armed; /* a sleep is under way */
    asx_time at;
    uint32_t index;
} work;

static uint64_t g_polls;

static asx_status poll_yield(void *data, asx_task_id self) {
    work *w = (work *)data;
    (void)self;
    g_polls++;
    if (w->yields_left > 0) {
        w->yields_left--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* Sleeps SLEEPS times, each to the next of eight staggered deadlines: the
 * timer fires, the task is woken (the timed lane) and starts its next sleep. */
static asx_status poll_sleep(void *data, asx_task_id self) {
    work *w = (work *)data;
    asx_time now = asx_runtime_virtual_now(test_runtime());
    g_polls++;
    if (w->armed) {
        if (now < w->at) return asx_task_wait_until(self, w->at);
        w->armed = 0;
        w->sleeps_left--;
    }
    if (w->sleeps_left <= 0) return ASX_OK;
    w->at = now + (asx_time)(1u + (w->index % 8u)) * 1000u;
    w->armed = 1;
    return asx_task_wait_until(self, w->at);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* One run of workload `wl` with `tasks` tasks; ns per poll times 1000. */
static uint64_t run_once(uint32_t wl, uint32_t tasks, uint64_t *polls_out) {
    static work w[ASX_MAX_TASKS];
    asx_region_id rid;
    asx_task_id ids[ASX_MAX_TASKS];
    asx_budget budget;
    uint64_t t0;
    uint64_t t1;
    asx_status run_st;
    uint32_t i;

    asx_runtime_reset(test_runtime());
    if (asx_region_open(test_runtime(), &rid) != ASX_OK) return 0u;
    g_polls = 0u;
    for (i = 0; i < tasks; i++) {
        asx_budget b = asx_budget_new();
        w[i].yields_left = YIELDS;
        w[i].sleeps_left = SLEEPS;
        w[i].armed = 0;
        w[i].at = 0u;
        w[i].index = i;
        if (wl == 1u) b.priority = (uint8_t)((i % 8u) * 32u);
        if (asx_task_spawn_with_budget(rid, wl == 2u ? poll_sleep : poll_yield, &w[i], &b,
                                       &ids[i]) != ASX_OK) {
            return 0u;
        }
    }
    if (wl == 3u) {
        for (i = 0; i < tasks; i += 2u) {
            asx_status cst = asx_task_cancel(ids[i], ASX_CANCEL_USER);
            if (cst != ASX_OK) return 0u;
        }
    }
    budget = asx_budget_infinite();
    t0 = now_ns();
    run_st = asx_scheduler_run(rid, &budget);
    t1 = now_ns();
    if (run_st != ASX_OK) return 0u;
    *polls_out = g_polls;
    return g_polls == 0u ? 0u : (t1 - t0) * 1000u / g_polls;
}

int main(int argc, char **argv) {
    uint32_t reps = 15u;
    uint32_t tasks = ASX_MAX_TASKS > 49u ? 48u : ASX_MAX_TASKS - 1u;
    const char *only = NULL;
    uint32_t wl;
    uint32_t printed = 0u;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--tasks") == 0 && i + 1 < argc) {
            tasks = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--reps N] [--tasks T] [--only WORKLOAD]\n", argv[0]);
            return 2;
        }
    }
    if (reps == 0u || reps > 1001u || tasks == 0u || tasks > ASX_MAX_TASKS) {
        fprintf(stderr, "bench_sched_lanes: --reps 1..1001, --tasks 1..%u\n",
                (unsigned)ASX_MAX_TASKS);
        return 2;
    }

    printf("{\"schema\":\"asx.bench.sched_lanes.v1\",\"tasks\":%u,\"reps\":%u,\"workloads\":{",
           (unsigned)tasks, (unsigned)reps);
    for (wl = 0; wl < WL_COUNT; wl++) {
        uint64_t *samples;
        uint64_t polls = 0u;
        uint32_t r;
        if (only != NULL && strcmp(only, g_wl_name[wl]) != 0) continue;
        samples = (uint64_t *)calloc((size_t)reps, sizeof(uint64_t));
        if (samples == NULL) return 1;
        for (r = 0; r < reps; r++) samples[r] = run_once(wl, tasks, &polls);
        qsort(samples, (size_t)reps, sizeof(uint64_t), cmp_u64);
        printf("%s\"%s\":{\"polls\":%" PRIu64 ",\"ns_per_poll\":%.3f}", printed++ == 0u ? "" : ",",
               g_wl_name[wl], polls, (double)samples[reps / 2u] / 1000.0);
        free(samples);
    }
    printf("}}\n");
    if (printed == 0u) {
        fprintf(stderr, "bench_sched_lanes: no workload named %s\n", only != NULL ? only : "?");
        return 2;
    }
    return 0;
}
