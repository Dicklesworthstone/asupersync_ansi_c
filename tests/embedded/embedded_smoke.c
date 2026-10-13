/*
 * embedded_smoke.c — Bare-metal smoke test for asupersync runtime
 *
 * Exercises core lifecycle (region, task, obligation, quiescence) under
 * QEMU semihosting with no OS or libc stdio dependency.
 *
 * Exit code: 0 = all pass, >0 = number of failures
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_runtime.h"
#include <asx/asx.h>

/* ------------------------------------------------------------------ */
/* Minimal assertion infrastructure (no printf, no stdio)              */
/* ------------------------------------------------------------------ */

static int g_fail_count;
static int g_test_count;

#define SMOKE_ASSERT(cond)                                                                         \
    do {                                                                                           \
        g_test_count++;                                                                            \
        if (!(cond)) { g_fail_count++; }                                                           \
    } while (0)

#define SMOKE_ASSERT_EQ(a, b) SMOKE_ASSERT((a) == (b))
#define SMOKE_ASSERT_NE(a, b) SMOKE_ASSERT((a) != (b))

/* ------------------------------------------------------------------ */
/* Test poll functions                                                  */
/* ------------------------------------------------------------------ */

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

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

static asx_status failing_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

/* An obligation has a holder task: it is reserved and settled inside the
 * holder's poll. This task reserves one obligation, reads its state, then
 * commits it. */
typedef struct {
    asx_region_id region;
    asx_obligation_id oid;
    asx_status reserve_status;
    asx_status state_status;
    asx_obligation_state state;
    asx_status commit_status;
} obligation_ctx;

static asx_status obligation_poll(void *user_data, asx_task_id self) {
    obligation_ctx *ctx = (obligation_ctx *)user_data;
    (void)self;

    ctx->reserve_status = asx_obligation_reserve(ctx->region, &ctx->oid);
    if (ctx->reserve_status != ASX_OK) return ASX_OK;
    ctx->state_status = asx_obligation_get_state(ctx->oid, &ctx->state);
    ctx->commit_status = asx_obligation_commit(ctx->oid);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Cleanup tracking                                                    */
/* ------------------------------------------------------------------ */

static int g_cleanup_called;

static void cleanup_fn(void *user_data) {
    (void)user_data;
    g_cleanup_called++;
}

/* ------------------------------------------------------------------ */
/* Test: region open and close                                         */
/* ------------------------------------------------------------------ */

static void test_region_open_close(void) {
    asx_region_id rid;
    asx_region_state state;
    asx_status st;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_region_get_state(rid, &state);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(state, ASX_REGION_OPEN);

    st = asx_region_close(rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_region_get_state(rid, &state);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(state, ASX_REGION_CLOSING);
}

/* ------------------------------------------------------------------ */
/* Test: task spawn and immediate completion                           */
/* ------------------------------------------------------------------ */

static void test_noop_task(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    asx_task_state tstate;
    st = asx_task_get_state(tid, &tstate);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(tstate, ASX_TASK_COMPLETED);
}

/* ------------------------------------------------------------------ */
/* Test: countdown task (multiple polls)                                */
/* ------------------------------------------------------------------ */

static void test_countdown_task(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;
    countdown_ctx ctx;

    ctx.remaining = 3;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, countdown_poll, &ctx, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    SMOKE_ASSERT_EQ(ctx.remaining, 0);

    asx_task_state tstate;
    st = asx_task_get_state(tid, &tstate);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(tstate, ASX_TASK_COMPLETED);
}

/* ------------------------------------------------------------------ */
/* Test: failing task produces error outcome                           */
/* ------------------------------------------------------------------ */

static void test_failing_task(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, failing_poll, NULL, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    /* Scheduler returns OK after draining all tasks, even with failures */
    (void)st;

    asx_task_state tstate;
    st = asx_task_get_state(tid, &tstate);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(tstate, ASX_TASK_COMPLETED);
}

/* ------------------------------------------------------------------ */
/* Test: full region drain lifecycle                                    */
/* ------------------------------------------------------------------ */

static void test_region_drain(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    asx_region_state rstate;
    st = asx_region_get_state(rid, &rstate);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(rstate, ASX_REGION_CLOSED);
}

/* ------------------------------------------------------------------ */
/* Test: quiescence check after drain                                  */
/* ------------------------------------------------------------------ */

static void test_quiescence(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_quiescence_check(rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Test: obligation reserve and commit                                 */
/* ------------------------------------------------------------------ */

static void test_obligation(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_obligation_state ostate;
    asx_status st;
    asx_budget budget;
    obligation_ctx ctx;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    /* A sentinel status until the holder's poll runs */
    ctx.region = rid;
    ctx.oid = ASX_INVALID_ID;
    ctx.reserve_status = ASX_E_INVALID_STATE;
    ctx.state_status = ASX_E_INVALID_STATE;
    ctx.state = ASX_OBLIGATION_LEAKED;
    ctx.commit_status = ASX_E_INVALID_STATE;

    st = asx_task_spawn(rid, obligation_poll, &ctx, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    SMOKE_ASSERT_EQ(ctx.reserve_status, ASX_OK);

    SMOKE_ASSERT_EQ(ctx.state_status, ASX_OK);
    SMOKE_ASSERT_EQ(ctx.state, ASX_OBLIGATION_RESERVED);

    SMOKE_ASSERT_EQ(ctx.commit_status, ASX_OK);

    st = asx_obligation_get_state(ctx.oid, &ostate);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(ostate, ASX_OBLIGATION_COMMITTED);
}

/* ------------------------------------------------------------------ */
/* Test: cleanup stack fires during drain                              */
/* ------------------------------------------------------------------ */

static void test_cleanup(void) {
    asx_cleanup_stack stack;
    asx_cleanup_handle handle;
    asx_status st;

    g_cleanup_called = 0;
    asx_cleanup_init(&stack);

    st = asx_cleanup_push(&stack, cleanup_fn, NULL, &handle);
    SMOKE_ASSERT_EQ(st, ASX_OK);
    SMOKE_ASSERT_EQ(asx_cleanup_pending(&stack), 1u);

    asx_cleanup_drain(&stack);
    SMOKE_ASSERT_EQ(g_cleanup_called, 1);
    SMOKE_ASSERT_EQ(asx_cleanup_pending(&stack), 0u);
}

/* ------------------------------------------------------------------ */
/* Test: budget exhaustion stops scheduler                             */
/* ------------------------------------------------------------------ */

static void test_budget_exhaustion(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_status st;
    asx_budget budget;
    countdown_ctx ctx;

    ctx.remaining = 100;

    st = asx_region_open(test_runtime(), &rid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    st = asx_task_spawn(rid, countdown_poll, &ctx, &tid);
    SMOKE_ASSERT_EQ(st, ASX_OK);

    /* Only allow 5 polls */
    budget = asx_budget_from_polls(5);
    st = asx_scheduler_run(rid, &budget);
    /* Should exhaust budget before task completes */
    SMOKE_ASSERT_EQ(st, ASX_E_POLL_BUDGET_EXHAUSTED);
    SMOKE_ASSERT(ctx.remaining > 0);
}

/* ------------------------------------------------------------------ */
/* Main — run all tests                                                */
/* ------------------------------------------------------------------ */

/* Per-test fail tracking for debugging */
static volatile int g_test_marker;
static volatile int g_pre_fail;

static void run_test(void (*fn)(void), int marker) {
    g_test_marker = marker;
    g_pre_fail = g_fail_count;
    asx_runtime_reset(test_runtime());
    fn();
}

int main(void) {
    g_fail_count = 0;
    g_test_count = 0;

    run_test(test_region_open_close, 1);
    run_test(test_noop_task, 2);
    run_test(test_countdown_task, 3);
    run_test(test_failing_task, 4);
    run_test(test_region_drain, 5);
    run_test(test_quiescence, 6);
    run_test(test_obligation, 7);
    run_test(test_cleanup, 8);
    run_test(test_budget_exhaustion, 9);

    /* Return fail count as exit code (0 = all pass) */
    return g_fail_count;
}
