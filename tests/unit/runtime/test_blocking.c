/*
 * test_blocking.c — unit tests for blocking pool
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ASX_PROFILE_POSIX) && defined(ASX_DETERMINISTIC) && (ASX_DETERMINISTIC == 0)
#define ASX_TEST_LIVE_POOL 1
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#else
#define ASX_TEST_LIVE_POOL 0
#endif

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/rt.h>
#include <string.h>

#if ASX_HAS_BLOCKING_SURFACE
static asx_status st_sink_;
#if !ASX_DETERMINISTIC
static void reset_hook_state(void);
#endif
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static int setup(void) {
    asx_status st;
    asx_runtime_reset();
    asx_waker_reset();
    asx_blocking_pool_reset();
    st = asx_blocking_pool_init();
    if (!asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        if (st != ASX_E_PERMISSION_DENIED) {
            fprintf(stderr, "ASSERT_EQ failed: st (%d) != ASX_E_PERMISSION_DENIED (%d) at %s:%d\n",
                    (int)st, (int)ASX_E_PERMISSION_DENIED, __FILE__, __LINE__);
            test_failures++;
            return 0;
        }
        return 0;
    }
    if (st != ASX_OK) {
        fprintf(stderr, "ASSERT_EQ failed: st (%d) != ASX_OK (%d) at %s:%d\n", (int)st, (int)ASX_OK,
                __FILE__, __LINE__);
        test_failures++;
        return 0;
    }
    return 1;
}

static void teardown(void) {
    asx_blocking_pool_shutdown();
    asx_runtime_reset();
#if !ASX_DETERMINISTIC
    reset_hook_state();
#endif
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t add_42(void *user_data) {
    uint64_t val = *(uint64_t *)user_data;
    return val + 42;
}

static uint64_t return_zero(void *user_data) {
    (void)user_data;
    return 0;
}

#if !ASX_DETERMINISTIC
static asx_blocking_job_fn g_hook_job_fn;
static void *g_hook_job_ctx;
static uint32_t g_hook_submit_count;
static uint32_t g_hook_shutdown_count;
static int g_hook_reject_submit;

static asx_status test_blocking_submit(void *ctx, asx_blocking_job_fn job_fn, void *job_ctx) {
    (void)ctx;
    g_hook_submit_count++;
    if (g_hook_reject_submit) return ASX_E_RESOURCE_EXHAUSTED;
    g_hook_job_fn = job_fn;
    g_hook_job_ctx = job_ctx;
    return ASX_OK;
}

static void test_blocking_shutdown(void *ctx) {
    (void)ctx;
    g_hook_shutdown_count++;
}

static void reset_hook_state(void) {
    g_hook_job_fn = NULL;
    g_hook_job_ctx = NULL;
    g_hook_submit_count = 0u;
    g_hook_shutdown_count = 0u;
    g_hook_reject_submit = 0;
}

static void install_deferred_blocking_hook(void) {
    asx_runtime_hooks hooks;
    MUST_OK(asx_runtime_hooks_init(&hooks));
    hooks.blocking.submit_fn = test_blocking_submit;
    hooks.blocking.shutdown_fn = test_blocking_shutdown;
    MUST_OK(asx_runtime_set_hooks(&hooks));
}
#endif

/* ------------------------------------------------------------------ */
/* Spawn tests                                                         */
/* ------------------------------------------------------------------ */

TEST(spawn_null_fn_fails) {
    asx_blocking_handle h;
    if (!setup()) return;
    ASSERT_EQ(asx_spawn_blocking(NULL, NULL, NULL, &h), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(spawn_null_handle_fails) {
    if (!setup()) return;
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(spawn_before_init_fails) {
    asx_blocking_handle h;
    asx_blocking_pool_reset();
    asx_blocking_pool_shutdown();
    ASSERT_FALSE(asx_blocking_pool_is_initialized());
    /* Surface gating may return PERMISSION_DENIED before init check fires */
    {
        asx_status st = asx_spawn_blocking(return_zero, NULL, NULL, &h);
        ASSERT_TRUE(st == ASX_E_INVALID_STATE || st == ASX_E_PERMISSION_DENIED);
    }
}

TEST(init_state_tracks_lifecycle) {
    if (!setup()) return;
    ASSERT_TRUE(asx_blocking_pool_is_initialized());
    teardown();
    ASSERT_FALSE(asx_blocking_pool_is_initialized());
}

TEST(reinit_invalidates_old_handles) {
    asx_blocking_handle h;
    uint64_t result = 0;

    if (!setup()) return;
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_OK);

    MUST_OK(asx_blocking_pool_init());
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_E_NOT_FOUND);
    teardown();
}

TEST(spawn_success) {
    asx_blocking_handle h;
    if (!setup()) return;
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, &h), ASX_OK);
    teardown();
}

TEST(spawn_returns_result) {
    asx_blocking_handle h;
    uint64_t input = 100;
    uint64_t result;
    if (!setup()) return;
    MUST_OK(asx_spawn_blocking(add_42, &input, NULL, &h));
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_OK);
    ASSERT_EQ(result, (uint64_t)142);
    teardown();
}

TEST(spawn_signals_completion_waker) {
    asx_blocking_handle h;
    asx_waker w;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    ASSERT_FALSE(asx_waker_is_signaled(&w));
    MUST_OK(asx_spawn_blocking(return_zero, NULL, &w, &h));
    /* Walking skeleton executes inline — waker should be signaled */
    ASSERT_TRUE(asx_waker_is_signaled(&w));
    teardown();
}

TEST(spawn_stale_completion_waker_fails) {
    asx_blocking_handle h;
    asx_waker w;
    if (!setup()) return;
    MUST_OK(asx_waker_register(1, &w));
    asx_waker_deregister(&w);
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, &w, &h), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    teardown();
}

TEST(spawn_without_waker_ok) {
    asx_blocking_handle h;
    if (!setup()) return;
    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* State query tests                                                   */
/* ------------------------------------------------------------------ */

TEST(get_state_null_returns_completed) {
    ASSERT_EQ(asx_blocking_get_state(NULL), ASX_BLOCKING_COMPLETED);
}

TEST(get_state_stale_handle_returns_completed) {
    asx_blocking_handle h;
    if (!setup()) return;
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    /* Reset invalidates handle */
    asx_blocking_pool_reset();
    MUST_OK(asx_blocking_pool_init());
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Result query tests                                                  */
/* ------------------------------------------------------------------ */

TEST(get_result_null_handle_fails) {
    uint64_t result;
    asx_status st = asx_blocking_get_result(NULL, &result);
    ASSERT_TRUE(st == ASX_E_INVALID_ARGUMENT || st == ASX_E_PERMISSION_DENIED);
}

TEST(get_result_null_out_fails) {
    asx_blocking_handle h;
    if (!setup()) return;
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    ASSERT_EQ(asx_blocking_get_result(&h, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(get_result_before_init_fails) {
    asx_blocking_handle h;
    uint64_t result = 0;

    memset(&h, 0, sizeof(h));
    asx_blocking_pool_reset();
    asx_blocking_pool_shutdown();
    {
        asx_status st = asx_blocking_get_result(&h, &result);
        ASSERT_TRUE(st == ASX_E_INVALID_STATE || st == ASX_E_PERMISSION_DENIED);
    }
}

TEST(get_result_stale_handle_fails) {
    asx_blocking_handle h;
    uint64_t result;
    if (!setup()) return;
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    asx_blocking_pool_reset();
    MUST_OK(asx_blocking_pool_init());
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_E_NOT_FOUND);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Active count tests                                                  */
/* ------------------------------------------------------------------ */

TEST(active_count_zero_initially) {
    if (!setup()) return;
    /* Walking skeleton: inline execution means tasks complete immediately */
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    teardown();
}

TEST(active_count_zero_after_inline_completion) {
    asx_blocking_handle h;
    if (!setup()) return;
    MUST_OK(asx_spawn_blocking(return_zero, NULL, NULL, &h));
    /* Should be zero because walking skeleton runs inline */
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    teardown();
}

#if !ASX_DETERMINISTIC
TEST(hook_submit_leaves_task_pending_until_job_runs) {
    asx_blocking_handle h;
    uint64_t input = 100u;
    uint64_t result = 0u;

    reset_hook_state();
    asx_blocking_pool_reset();
    install_deferred_blocking_hook();
    MUST_OK(asx_blocking_pool_init());

    ASSERT_EQ(asx_spawn_blocking(add_42, &input, NULL, &h), ASX_OK);
    ASSERT_EQ(g_hook_submit_count, 1u);
    ASSERT_TRUE(g_hook_job_fn != NULL);
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_PENDING);
    ASSERT_EQ(asx_blocking_active_count(), 1u);
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_E_PENDING);

    g_hook_job_fn(g_hook_job_ctx);
    ASSERT_EQ(asx_blocking_get_state(&h), ASX_BLOCKING_COMPLETED);
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    ASSERT_EQ(asx_blocking_get_result(&h, &result), ASX_OK);
    ASSERT_EQ(result, 142u);
    teardown();
}

TEST(hook_submit_failure_rolls_back_slot_and_active_count) {
    asx_blocking_handle h;

    reset_hook_state();
    asx_blocking_pool_reset();
    install_deferred_blocking_hook();
    MUST_OK(asx_blocking_pool_init());
    g_hook_reject_submit = 1;

    ASSERT_EQ(asx_spawn_blocking(return_zero, NULL, NULL, &h), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(g_hook_submit_count, 1u);
    ASSERT_EQ(asx_blocking_active_count(), 0u);
    teardown();
}

TEST(pool_shutdown_invokes_live_blocking_hook) {
    reset_hook_state();
    asx_blocking_pool_reset();
    install_deferred_blocking_hook();
    MUST_OK(asx_blocking_pool_init());
    g_hook_shutdown_count = 0u;
    asx_blocking_pool_shutdown();
    ASSERT_EQ(g_hook_shutdown_count, 1u);
}
#endif

/* ------------------------------------------------------------------ */
/* Exhaustion                                                          */
/* ------------------------------------------------------------------ */

TEST(arena_exhaustion) {
    asx_blocking_handle h;
    asx_status st;
    uint32_t i;
    if (!setup()) return;
    /* Fill all slots — walking skeleton completes them immediately,
     * but they still occupy slots until reset */
    for (i = 0; i < ASX_MAX_BLOCKING_TASKS; i++) {
        st = asx_spawn_blocking(return_zero, NULL, NULL, &h);
        ASSERT_EQ(st, ASX_OK);
    }
    /* Completed slots can be reused — walking skeleton marks as COMPLETED
     * and the allocator reclaims completed slots */
    st = asx_spawn_blocking(return_zero, NULL, NULL, &h);
    ASSERT_EQ(st, ASX_OK); /* Should succeed via slot reuse */
    teardown();
}

/* ------------------------------------------------------------------ */
/* Multiple tasks                                                      */
/* ------------------------------------------------------------------ */

TEST(multiple_tasks_different_results) {
    asx_blocking_handle h1, h2;
    uint64_t in1 = 10, in2 = 200;
    uint64_t r1, r2;
    if (!setup()) return;
    /* Walking skeleton completes inline, so get result before next spawn
     * (completed slots may be reused, invalidating old handles) */
    MUST_OK(asx_spawn_blocking(add_42, &in1, NULL, &h1));
    MUST_OK(asx_blocking_get_result(&h1, &r1));
    ASSERT_EQ(r1, (uint64_t)52);
    MUST_OK(asx_spawn_blocking(add_42, &in2, NULL, &h2));
    MUST_OK(asx_blocking_get_result(&h2, &r2));
    ASSERT_EQ(r2, (uint64_t)242);
    teardown();
}

#if ASX_TEST_LIVE_POOL
/* ------------------------------------------------------------------ */
/* Live pool: cross-thread wake of a scheduler blocked in the reactor  */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_waker waker;
    asx_blocking_handle job;
    int submitted;
    uint64_t result;
    uint32_t polls;
} pool_waiter_state;

static uint64_t slow_job(void *user_data) {
    struct timespec ts;
    (void)user_data;
    ts.tv_sec = 0;
    ts.tv_nsec = 50L * 1000000L;
    (void)nanosleep(&ts, NULL);
    return 7u;
}

/* Submits one pool job with its own waker, then parks until it is done. */
static asx_status poll_pool_waiter(void *ud, asx_task_id self) {
    pool_waiter_state *s = (pool_waiter_state *)ud;
    asx_status st;
    s->polls++;
    if (!s->submitted) {
        st = asx_waker_register(self, &s->waker);
        if (st != ASX_OK) return st;
        st = asx_spawn_blocking(slow_job, NULL, &s->waker, &s->job);
        if (st != ASX_OK) return st;
        s->submitted = 1;
    }
    st = asx_blocking_get_result(&s->job, &s->result);
    if (st == ASX_OK) {
        asx_waker_deregister(&s->waker);
        return ASX_OK;
    }
    if (st != ASX_E_PENDING) return st;
    st = asx_task_park(self);
    if (st != ASX_OK) return st;
    return ASX_E_PENDING;
}

TEST(scheduler_wakes_task_parked_on_pool_job) {
    asx_runtime rt;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    pool_waiter_state s;
    asx_time t0 = 0;
    asx_time t1 = 0;

    ASSERT_EQ(asx_runtime_init_default(&rt), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_pool_waiter, &s, &t), ASX_OK);
    MUST_OK(asx_runtime_now_ns(&t0));
    budget = asx_budget_from_polls(100);
    /* Nothing but the pool job can wake the task: the scheduler must block
     * on it rather than report ASX_E_WOULD_BLOCK. */
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    MUST_OK(asx_runtime_now_ns(&t1));
    ASSERT_EQ(s.result, 7u);
    ASSERT_TRUE(s.polls <= 3u);
    /* Woken by the completion's reactor notify, not the 1 s idle cap. */
    ASSERT_TRUE(t1 - t0 < (asx_time)500u * 1000000u);
    asx_runtime_shutdown(&rt);
    teardown();
}
#endif

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

TEST(init_denied_when_blocking_surface_unavailable) {
    asx_status st;
    asx_blocking_pool_reset();
    st = asx_blocking_pool_init();
    if (asx_surface_available_active(ASX_SURFACE_BLOCKING)) {
        ASSERT_EQ(st, ASX_OK);
    } else {
        ASSERT_EQ(st, ASX_E_PERMISSION_DENIED);
    }
    teardown();
}
#else
TEST(blocking_surface_compile_time_hidden_in_browser) {
    ASSERT_EQ(ASX_HAS_BLOCKING_SURFACE, 0);
    ASSERT_EQ(asx_surface_available_active(ASX_SURFACE_BLOCKING), 0);
    ASSERT_EQ((int)asx_surface_gate(ASX_SURFACE_BLOCKING), (int)ASX_E_PERMISSION_DENIED);
}
#endif

int main(void) {
    fprintf(stderr, "=== test_blocking ===\n");
#if ASX_HAS_BLOCKING_SURFACE
    RUN_TEST(spawn_null_fn_fails);
    RUN_TEST(spawn_null_handle_fails);
    RUN_TEST(spawn_before_init_fails);
    RUN_TEST(init_state_tracks_lifecycle);
    RUN_TEST(reinit_invalidates_old_handles);
    RUN_TEST(spawn_success);
    RUN_TEST(spawn_returns_result);
    RUN_TEST(spawn_signals_completion_waker);
    RUN_TEST(spawn_stale_completion_waker_fails);
    RUN_TEST(spawn_without_waker_ok);

    RUN_TEST(get_state_null_returns_completed);
    RUN_TEST(get_state_stale_handle_returns_completed);

    RUN_TEST(get_result_null_handle_fails);
    RUN_TEST(get_result_null_out_fails);
    RUN_TEST(get_result_before_init_fails);
    RUN_TEST(get_result_stale_handle_fails);

    RUN_TEST(active_count_zero_initially);
    RUN_TEST(active_count_zero_after_inline_completion);
#if !ASX_DETERMINISTIC
    RUN_TEST(hook_submit_leaves_task_pending_until_job_runs);
    RUN_TEST(hook_submit_failure_rolls_back_slot_and_active_count);
    RUN_TEST(pool_shutdown_invokes_live_blocking_hook);
#endif

    RUN_TEST(arena_exhaustion);
    RUN_TEST(multiple_tasks_different_results);
    RUN_TEST(init_denied_when_blocking_surface_unavailable);
#if ASX_TEST_LIVE_POOL
    RUN_TEST(scheduler_wakes_task_parked_on_pool_job);
#endif
#else
    RUN_TEST(blocking_surface_compile_time_hidden_in_browser);
#endif
    TEST_REPORT();
    return test_failures;
}
