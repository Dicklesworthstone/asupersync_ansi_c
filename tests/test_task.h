/*
 * test_task.h — run test code as the poll of a task
 *
 * An obligation always has a holder task (Rust creates a token only through
 * a task's Cx), and only the holder's own poll can reserve or settle one: the
 * host, outside any poll, holds no authority over it. A test that exercises
 * obligations from "the host" therefore runs that code as the single poll of
 * a task of the region. ASSERT_* inside `fn` return from `fn` and mark the
 * test failed, as in a TEST body.
 *
 *   static void body(void *ctx) { ... asx_obligation_reserve(...) ... }
 *   ...
 *   ASSERT_EQ(test_run_in_task(rid, body, &ctx), ASX_OK);
 *
 * The task completes after `fn` returns, so `fn` resolves every obligation
 * it reserves; one it leaves reserved leaks under the runtime's leak policy.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TEST_TASK_H
#define ASX_TEST_TASK_H

#include <asx/asx.h>

typedef void (*test_task_fn)(void *ctx);

typedef struct {
    test_task_fn fn;
    void *ctx;
} test_task_job;

static asx_status test_task_poll(void *user_data, asx_task_id self) {
    test_task_job *job = (test_task_job *)user_data;
    (void)self;
    job->fn(job->ctx);
    return ASX_OK;
}

/* Spawn a task of `region` whose poll runs fn(ctx), drive the scheduler until
 * it completes, and return the scheduler's status (ASX_OK when the task ran
 * to completion). */
static asx_status test_run_in_task(asx_region_id region, test_task_fn fn, void *ctx) {
    test_task_job job;
    asx_task_id tid;
    asx_budget run;
    asx_status st;

    job.fn = fn;
    job.ctx = ctx;
    st = asx_task_spawn(region, test_task_poll, &job, &tid);
    if (st != ASX_OK) return st;
    run = asx_budget_from_polls(10);
    return asx_scheduler_run(region, &run);
}

#endif /* ASX_TEST_TASK_H */
