/*
 * test_combinator2.c — unit tests for second-wave combinators
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/channel.h>
#include <asx/core/combinator.h>
#include <asx/core/combinator2.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/virtual_time.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test poll functions                                                  */
/* ------------------------------------------------------------------ */

static asx_status poll_ok(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

static asx_status poll_pending_then_ok(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    if (*count >= 2) return ASX_OK;
    return ASX_E_PENDING;
}

/* Fails N times, then succeeds */
static asx_status poll_fail_n_then_ok(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    if (*count >= 3) return ASX_OK;
    return ASX_E_INVALID_STATE;
}

/* Tracks call count */
static asx_status poll_count(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    return ASX_OK;
}

/* ================================================================== */
/* Retry tests                                                         */
/* ================================================================== */

TEST(retry_success_first_try) {
    /* The default policy: the first attempt has no delay. */
    asx_retry_state rs;
    MUST_OK(asx_retry_init(&rs, poll_ok, NULL, NULL));
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
    ASSERT_EQ(asx_retry_total_delay_ns(&rs), (uint64_t)0u);
}

TEST(retry_fail_then_succeed) {
    asx_retry_state rs;
    asx_retry_policy p = asx_retry_policy_immediate(6);
    int count = 0;
    MUST_OK(asx_retry_init(&rs, poll_fail_n_then_ok, &count, &p));

    /* Two attempts fail and the third succeeds, all in one poll: with no
     * delay Rust's Retry starts the next attempt at once (retry.rs:928-940). */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 3u);
    ASSERT_EQ(count, 3);
}

TEST(retry_exhausted) {
    asx_retry_state rs;
    asx_retry_policy p = asx_retry_policy_immediate(3);
    MUST_OK(asx_retry_init(&rs, poll_fail, NULL, &p));

    /* Three attempts fail in one poll: exhausted */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_retry_attempts(&rs), 3u);
    ASSERT_EQ(asx_retry_last_error(&rs), ASX_E_INVALID_STATE);
}

static asx_status poll_cancelled_count(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    (void)self;
    (*count)++;
    return ASX_E_CANCELLED;
}

TEST(retry_stops_at_a_cancelled_attempt) {
    /* An attempt that ends cancelled ends the retry (retry.rs:945-948). */
    asx_retry_state rs;
    int count = 0;
    MUST_OK(asx_retry_init(&rs, poll_cancelled_count, &count, NULL));
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_CANCELLED);
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_CANCELLED);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

/* Fails after cancelling its own task, counting its attempts. */
static asx_status poll_cancel_owner_then_fail(void *user_data, asx_task_id self) {
    int *count = (int *)user_data;
    asx_status st = asx_task_cancel(self, ASX_CANCEL_USER);
    (*count)++;
    return st == ASX_OK ? ASX_E_INVALID_STATE : st;
}

typedef struct {
    asx_retry_state rs;
    uint32_t polls;
    asx_status result;
} retry_task;

/* Drives a retry from a task's poll; the task itself ends OK. */
static asx_status poll_retry_task(void *user_data, asx_task_id self) {
    retry_task *rt = (retry_task *)user_data;
    rt->polls++;
    rt->result = asx_retry_poll(&rt->rs, self);
    return rt->result == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

TEST(retry_starts_no_attempt_once_its_owner_is_cancelled) {
    /* Rust's Retry checkpoints the owner before each attempt
     * (retry.rs:842-863): the first attempt fails after cancelling the
     * owner, so no second attempt starts and the retry ends cancelled. */
    static retry_task rt;
    asx_runtime runtime;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    asx_retry_policy p = asx_retry_policy_immediate(4);
    int count = 0;

    MUST_OK(asx_runtime_init_default(&runtime));
    memset(&rt, 0, sizeof(rt));
    MUST_OK(asx_retry_init(&rt.rs, poll_cancel_owner_then_fail, &count, &p));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_retry_task, &rt, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(rt.polls, 1u);
    ASSERT_EQ(rt.result, ASX_E_CANCELLED);
    ASSERT_EQ(count, 1);
    asx_runtime_reset();
}

TEST(retry_single_attempt) {
    asx_retry_state rs;
    asx_retry_policy p = asx_retry_policy_immediate(1);
    MUST_OK(asx_retry_init(&rs, poll_fail, NULL, &p));
    /* Only one attempt, no retries */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

TEST(retry_with_pending_inner) {
    asx_retry_state rs;
    asx_retry_policy p = asx_retry_policy_immediate(1);
    int count = 0;
    MUST_OK(asx_retry_init(&rs, poll_pending_then_ok, &count, &p));
    /* First poll: inner returns pending */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_E_PENDING);
    /* Second poll: inner returns OK */
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

TEST(retry_null_fails) {
    ASSERT_EQ(asx_retry_init(NULL, poll_ok, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_retry_init(NULL, NULL, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(retry_policy_constructors_are_rusts) {
    /* RetryPolicy::new and ::immediate (retry.rs:73-82, 147-155). */
    asx_retry_policy p = asx_retry_policy_new();
    ASSERT_EQ(p.max_attempts, 3u);
    ASSERT_EQ(p.initial_delay_ns, (uint64_t)100000000u);
    ASSERT_EQ(p.max_delay_ns, UINT64_C(30000000000));
    ASSERT_TRUE(p.multiplier == 2.0);
    /* The cast rounds the literal to double: x87 (-m32) evaluates a bare
     * 0.1 at long double precision (FLT_EVAL_METHOD 2), and the stored
     * double never equals that. */
    ASSERT_TRUE(p.jitter == (double)0.1);
    p = asx_retry_policy_immediate(0);
    ASSERT_EQ(p.max_attempts, 1u); /* max(1) */
    ASSERT_EQ(p.initial_delay_ns, (uint64_t)0u);
    ASSERT_EQ(p.max_delay_ns, (uint64_t)0u);
    ASSERT_TRUE(p.multiplier == 1.0 && p.jitter == 0.0);
}

TEST(retry_init_refuses_what_rust_validate_refuses) {
    /* RetryPolicy::validate (retry.rs:158-170). */
    asx_retry_state rs;
    asx_retry_policy p;
    double zero = 0.0;

    p = asx_retry_policy_new();
    p.max_attempts = 0;
    ASSERT_EQ(asx_retry_init(&rs, poll_ok, NULL, &p), ASX_E_INVALID_ARGUMENT);
    p = asx_retry_policy_new();
    p.multiplier = 0.5;
    ASSERT_EQ(asx_retry_init(&rs, poll_ok, NULL, &p), ASX_E_INVALID_ARGUMENT);
    p = asx_retry_policy_new();
    p.jitter = 1.5;
    ASSERT_EQ(asx_retry_init(&rs, poll_ok, NULL, &p), ASX_E_INVALID_ARGUMENT);
    p.jitter = -0.1;
    ASSERT_EQ(asx_retry_init(&rs, poll_ok, NULL, &p), ASX_E_INVALID_ARGUMENT);
    p.jitter = zero / zero;
    ASSERT_EQ(asx_retry_init(&rs, poll_ok, NULL, &p), ASX_E_INVALID_ARGUMENT);
    /* A NaN multiplier passes validate, as in Rust. */
    p = asx_retry_policy_new();
    p.multiplier = zero / zero;
    ASSERT_EQ(asx_retry_init(&rs, poll_ok, NULL, &p), ASX_OK);
}

/* Golden delays printed by Rust's own calculate_delay and DetRng (the
 * asupersync 5e60b1c4c source, retry.rs:201-294 and det_rng.rs, compiled
 * standalone): the jitter, exponent and conversion edges. */
typedef struct {
    uint64_t initial_ns;
    uint64_t max_ns;
    double multiplier;
    double jitter;
    uint32_t attempt;
    int has_seed;
    uint64_t seed;
    uint64_t delay_ns;
} retry_delay_golden;

TEST(retry_policy_delay_matches_rust_calculate_delay) {
    static const retry_delay_golden g[] = {
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 0, 0, 0, 0u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 1, 0, 0, 100000000u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 5, 0, 0, 1600000000u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 1, 1, 0, 100000000u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 1, 1, UINT64_MAX, 108597941u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 4, 1, UINT64_MAX, 868783529u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 4, 1, UINT64_C(0x5555555555555555),
         876823608u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 4, 1, 123456789u, 800572895u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 1, 1, UINT64_C(0xDEADBEEF), 102178590u},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 2000, 1, 7u, UINT64_C(30000000001)},
        {100000000u, UINT64_C(30000000000), 2.0, 0.1, 2000, 0, 0, UINT64_C(30000000000)},
        {5000000u, UINT64_C(30000000000), 1.0, 0.0, 70, 0, 0, 5000000u},
        {0u, UINT64_C(30000000000), 2.0, 0.5, 1025, 1, 9u, 0u},
        {UINT64_C(1) << 60, UINT64_MAX, 1.5, 0.5, 3, 1, 99u, UINT64_C(2594073392897801216)},
        {UINT64_C(1) << 60, UINT64_MAX, 1.5, 0.5, 200, 1, 99u, UINT64_MAX},
        {1234567u, UINT64_C(9000000000), 3.7, 1.0, 10, 1, UINT64_C(0xDEADBEEF),
         UINT64_C(10960731619)},
        {1234567u, UINT64_C(9000000000), 3.7, 1.0, 7, 1, 1u, UINT64_C(3167561155)},
        {3u, UINT64_C(1000000000000), 1.1, 0.25, 37, 1, UINT64_C(0xFFFFFFFF00000000), 106u},
    };
    asx_retry_policy p;
    double zero = 0.0;
    uint32_t i;

    for (i = 0; i < (uint32_t)(sizeof(g) / sizeof(g[0])); i++) {
        p.max_attempts = 3u;
        p.initial_delay_ns = g[i].initial_ns;
        p.max_delay_ns = g[i].max_ns;
        p.multiplier = g[i].multiplier;
        p.jitter = g[i].jitter;
        ASSERT_EQ(asx_retry_policy_delay(&p, g[i].attempt, g[i].has_seed ? &g[i].seed : NULL),
                  g[i].delay_ns);
    }
    /* A NaN multiplier: every retry at max_delay. */
    p.initial_delay_ns = 1000u;
    p.max_delay_ns = 50000u;
    p.multiplier = zero / zero;
    p.jitter = 0.0;
    ASSERT_EQ(asx_retry_policy_delay(&p, 2, NULL), (uint64_t)50000u);
    ASSERT_EQ(asx_retry_policy_delay(NULL, 2, NULL), (uint64_t)0u);
}

/* Fails `fail_first` times (counting every attempt), then succeeds. */
typedef struct {
    uint32_t calls;
    uint32_t fail_first;
    asx_time at[8]; /* virtual time of each attempt */
} retry_flaky;

static asx_status poll_flaky(void *user_data, asx_task_id self) {
    retry_flaky *f = (retry_flaky *)user_data;
    (void)self;
    if (f->calls < 8u) f->at[f->calls] = asx_runtime_virtual_now();
    f->calls++;
    return f->calls > f->fail_first ? ASX_OK : ASX_E_INVALID_STATE;
}

/* A runtime on the virtual clock (the default stub hooks), also in live
 * POSIX builds, whose default runtime has a real clock. */
static int init_virtual_runtime(asx_runtime *rt) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_runtime_config_init(&cfg);
    if (asx_runtime_hooks_init(&hooks) != ASX_OK) return 0;
    return asx_runtime_init(rt, &cfg, &hooks) == ASX_OK;
}

TEST(retry_backs_off_between_attempts) {
    /* 1 ms, then 2 ms, then 4 ms before the retries (no jitter): each
     * delay parks the owner on its sleep timer and the attempts start at
     * 0, 1, 3 and 7 ms of virtual time (retry.rs:866-882, 888-911). */
    static retry_task rt;
    static retry_flaky f;
    asx_runtime runtime;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    asx_retry_policy p = asx_retry_policy_new();

    ASSERT_TRUE(init_virtual_runtime(&runtime));
    p.max_attempts = 5u;
    p.initial_delay_ns = 1000000u;
    p.max_delay_ns = 1000000000u;
    p.jitter = 0.0;
    memset(&rt, 0, sizeof(rt));
    memset(&f, 0, sizeof(f));
    f.fail_first = 3u;
    MUST_OK(asx_retry_init(&rt.rs, poll_flaky, &f, &p));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_retry_task, &rt, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(rt.result, ASX_OK);
    ASSERT_EQ(f.calls, 4u);
    ASSERT_EQ(f.at[0], (asx_time)0u);
    ASSERT_EQ(f.at[1], (asx_time)1000000u);
    ASSERT_EQ(f.at[2], (asx_time)3000000u);
    ASSERT_EQ(f.at[3], (asx_time)7000000u);
    ASSERT_EQ(asx_retry_attempts(&rt.rs), 4u);
    ASSERT_EQ(asx_retry_total_delay_ns(&rt.rs), (uint64_t)7000000u);
    ASSERT_EQ(rt.polls, 4u); /* the first poll, then one wake per delay */
    asx_runtime_reset();
}

TEST(retry_jitter_is_drawn_from_the_owner_entropy) {
    /* The jitter seed is a draw from the owner's entropy: the same seeded
     * runtime gives the same delays, within [d, d * (1 + jitter)]. */
    static retry_task rt;
    static retry_flaky f;
    asx_runtime runtime;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    asx_retry_policy p = asx_retry_policy_new();
    uint64_t first_total = 0;
    int run;

    p.max_attempts = 3u;
    p.initial_delay_ns = 1000000u;
    p.max_delay_ns = 1000000000u;
    p.jitter = 0.5;
    for (run = 0; run < 2; run++) {
        ASSERT_TRUE(init_virtual_runtime(&runtime));
        memset(&rt, 0, sizeof(rt));
        memset(&f, 0, sizeof(f));
        f.fail_first = 2u;
        MUST_OK(asx_retry_init(&rt.rs, poll_flaky, &f, &p));
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        ASSERT_EQ(asx_task_spawn(r, poll_retry_task, &rt, &t), ASX_OK);
        budget = asx_budget_from_polls(100);
        ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
        ASSERT_EQ(rt.result, ASX_OK);
        ASSERT_EQ(f.calls, 3u);
        /* 1 ms and 2 ms before jitter. */
        ASSERT_TRUE(asx_retry_total_delay_ns(&rt.rs) >= (uint64_t)3000000u);
        ASSERT_TRUE(asx_retry_total_delay_ns(&rt.rs) <= (uint64_t)4500000u);
        ASSERT_EQ(f.at[2] - f.at[0], (asx_time)asx_retry_total_delay_ns(&rt.rs));
        if (run == 0) {
            first_total = asx_retry_total_delay_ns(&rt.rs);
        } else {
            ASSERT_EQ(asx_retry_total_delay_ns(&rt.rs), first_total);
        }
        asx_runtime_reset();
    }
    /* The seeded stream jitters at least one of the two delays. */
    ASSERT_TRUE(first_total > (uint64_t)3000000u);
}

typedef struct {
    asx_sleep_state sleep;
    asx_task_id target;
} retry_canceller;

/* Sleeps, then cancels the target task. */
static asx_status poll_cancel_after_sleep(void *user_data, asx_task_id self) {
    retry_canceller *c = (retry_canceller *)user_data;
    asx_status st = asx_sleep_poll(&c->sleep, self);
    if (st != ASX_OK) return st;
    return asx_task_cancel(c->target, ASX_CANCEL_USER);
}

TEST(retry_cancel_during_the_delay_starts_no_attempt) {
    /* A cancel while the retry waits out its delay ends it cancelled at
     * once (retry.rs:888-892): no second attempt, and the owner does not
     * wait for the rest of the 100 ms delay. */
    static retry_task rt;
    static retry_flaky f;
    static retry_canceller c;
    asx_runtime runtime;
    asx_region_id r;
    asx_task_id t;
    asx_task_id tc;
    asx_budget budget;
    asx_retry_policy p = asx_retry_policy_new();

    ASSERT_TRUE(init_virtual_runtime(&runtime));
    p.jitter = 0.0; /* 100 ms before the first retry */
    memset(&rt, 0, sizeof(rt));
    memset(&f, 0, sizeof(f));
    memset(&c, 0, sizeof(c));
    f.fail_first = 3u;
    MUST_OK(asx_retry_init(&rt.rs, poll_flaky, &f, &p));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_retry_task, &rt, &t), ASX_OK);
    MUST_OK(asx_sleep_init(&c.sleep, 10000000u));
    c.target = t;
    ASSERT_EQ(asx_task_spawn(r, poll_cancel_after_sleep, &c, &tc), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(rt.result, ASX_E_CANCELLED);
    ASSERT_EQ(f.calls, 1u);
    /* Rust's next_attempt counts attempt 2 when its delay begins. */
    ASSERT_EQ(asx_retry_attempts(&rt.rs), 2u);
    ASSERT_EQ(asx_retry_total_delay_ns(&rt.rs), (uint64_t)100000000u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)10000000u);
    asx_runtime_reset();
}

/* ================================================================== */
/* Bracket tests                                                       */
/* ================================================================== */

static int bracket_acquire_called;
static int bracket_use_called;
static int bracket_release_called;

static asx_status bracket_acquire(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_acquire_called = 1;
    return ASX_OK;
}

static asx_status bracket_use(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_use_called = 1;
    return ASX_OK;
}

static asx_status bracket_release(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_release_called = 1;
    return ASX_OK;
}

static asx_status bracket_use_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_use_called = 1;
    return ASX_E_INVALID_STATE;
}

static asx_status bracket_acquire_fail(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    bracket_acquire_called = 1;
    return ASX_E_INVALID_STATE;
}

TEST(bracket_success) {
    asx_bracket_state bs;
    bracket_acquire_called = 0;
    bracket_use_called = 0;
    bracket_release_called = 0;

    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_OK);
    ASSERT_TRUE(bracket_acquire_called);
    ASSERT_TRUE(bracket_use_called);
    ASSERT_TRUE(bracket_release_called);
}

TEST(bracket_use_fails_release_still_called) {
    asx_bracket_state bs;
    bracket_acquire_called = 0;
    bracket_use_called = 0;
    bracket_release_called = 0;

    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use_fail, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_E_INVALID_STATE);
    ASSERT_TRUE(bracket_acquire_called);
    ASSERT_TRUE(bracket_use_called);
    ASSERT_TRUE(bracket_release_called); /* cleanup always runs */
}

TEST(bracket_acquire_fails_no_release) {
    asx_bracket_state bs;
    bracket_acquire_called = 0;
    bracket_use_called = 0;
    bracket_release_called = 0;

    MUST_OK(asx_bracket_init(&bs, bracket_acquire_fail, bracket_use, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_E_INVALID_STATE);
    ASSERT_TRUE(bracket_acquire_called);
    ASSERT_FALSE(bracket_use_called);
    ASSERT_FALSE(bracket_release_called); /* nothing to clean up */
    /* Done: a later poll reports the same failure, not success. */
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_bracket_current_phase(&bs), ASX_BRACKET_PHASE_DONE);
    ASSERT_FALSE(bracket_release_called);
}

TEST(bracket_phase_tracking) {
    asx_bracket_state bs;
    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use, bracket_release, NULL));
    ASSERT_EQ(asx_bracket_current_phase(&bs), ASX_BRACKET_PHASE_ACQUIRE);
    MUST_OK(asx_bracket_poll(&bs, 0));
    ASSERT_EQ(asx_bracket_current_phase(&bs), ASX_BRACKET_PHASE_DONE);
}

TEST(bracket_null_fails) {
    asx_bracket_state bs;
    ASSERT_EQ(asx_bracket_init(NULL, bracket_acquire, bracket_use, bracket_release, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_bracket_init(&bs, NULL, bracket_use, bracket_release, NULL),
              ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Pipeline tests                                                      */
/* ================================================================== */

TEST(pipeline_empty) {
    asx_pipeline_state ps;
    MUST_OK(asx_pipeline_init(&ps));
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 0u);
}

TEST(pipeline_single_stage) {
    asx_pipeline_state ps;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_ok, NULL));
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 1u);
}

TEST(pipeline_three_stages) {
    asx_pipeline_state ps;
    int c1 = 0, c2 = 0, c3 = 0;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c1));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c2));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c3));

    /* One poll runs all three stages, each starting as the one before it
     * completes (Rust's pipeline! awaits them in one async block). */
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(c1, 1);
    ASSERT_EQ(c2, 1);
    ASSERT_EQ(c3, 1);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 3u);
}

TEST(pipeline_stops_on_failure) {
    asx_pipeline_state ps;
    int c1 = 0;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_fail, NULL));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_count, &c1));

    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(c1, 0); /* stage 2 never ran */
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 0u);
}

TEST(pipeline_with_pending_stage) {
    asx_pipeline_state ps;
    int count = 0;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_pending_then_ok, &count));

    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
    ASSERT_EQ(asx_pipeline_completed_stages(&ps), 1u);
}

TEST(pipeline_overflow) {
    asx_pipeline_state ps;
    uint32_t i;
    MUST_OK(asx_pipeline_init(&ps));
    for (i = 0; i < ASX_PIPELINE_MAX_STAGES; i++) {
        MUST_OK(asx_pipeline_add_stage(&ps, poll_ok, NULL));
    }
    ASSERT_EQ(asx_pipeline_add_stage(&ps, poll_ok, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ================================================================== */
/* Bulkhead tests                                                      */
/* ================================================================== */

TEST(bulkhead_basic) {
    asx_bulkhead_state bs;
    MUST_OK(asx_bulkhead_init(&bs, 2));
    ASSERT_EQ(asx_bulkhead_active(&bs), 0u);
    ASSERT_EQ(asx_bulkhead_remaining(&bs), 2u);

    MUST_OK(asx_bulkhead_try_enter(&bs));
    ASSERT_EQ(asx_bulkhead_active(&bs), 1u);
    ASSERT_EQ(asx_bulkhead_remaining(&bs), 1u);

    MUST_OK(asx_bulkhead_try_enter(&bs));
    ASSERT_EQ(asx_bulkhead_active(&bs), 2u);

    /* At capacity */
    ASSERT_EQ(asx_bulkhead_try_enter(&bs), ASX_E_WOULD_BLOCK);

    /* Leave one */
    MUST_OK(asx_bulkhead_leave(&bs));
    ASSERT_EQ(asx_bulkhead_active(&bs), 1u);
    MUST_OK(asx_bulkhead_try_enter(&bs));
}

TEST(bulkhead_leave_empty_fails) {
    asx_bulkhead_state bs;
    MUST_OK(asx_bulkhead_init(&bs, 2));
    ASSERT_EQ(asx_bulkhead_leave(&bs), ASX_E_INVALID_STATE);
}

TEST(bulkhead_zero_fails) {
    asx_bulkhead_state bs;
    ASSERT_EQ(asx_bulkhead_init(&bs, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(bulkhead_null_fails) { ASSERT_EQ(asx_bulkhead_init(NULL, 2), ASX_E_INVALID_ARGUMENT); }

/* ================================================================== */
/* RateLimit tests                                                     */
/* ================================================================== */

TEST(rate_limit_basic) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 3, 1));
    ASSERT_EQ(asx_rate_limit_available(&rs), 3u);

    MUST_OK(asx_rate_limit_try_acquire(&rs));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    ASSERT_EQ(asx_rate_limit_available(&rs), 0u);

    /* Rate limited */
    ASSERT_EQ(asx_rate_limit_try_acquire(&rs), ASX_E_WOULD_BLOCK);
}

TEST(rate_limit_refill) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 2, 1));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    MUST_OK(asx_rate_limit_try_acquire(&rs));
    ASSERT_EQ(asx_rate_limit_try_acquire(&rs), ASX_E_WOULD_BLOCK);

    /* Refill one token */
    asx_rate_limit_refill(&rs);
    ASSERT_EQ(asx_rate_limit_available(&rs), 1u);
    MUST_OK(asx_rate_limit_try_acquire(&rs));
}

TEST(rate_limit_refill_caps_at_capacity) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 3, 10));
    MUST_OK(asx_rate_limit_try_acquire(&rs));

    /* Refill with more than capacity */
    asx_rate_limit_refill(&rs);
    ASSERT_EQ(asx_rate_limit_available(&rs), 3u); /* capped */
}

TEST(rate_limit_zero_capacity_fails) {
    asx_rate_limit_state rs;
    ASSERT_EQ(asx_rate_limit_init(&rs, 0, 1), ASX_E_INVALID_ARGUMENT);
}

TEST(rate_limit_null_fails) { ASSERT_EQ(asx_rate_limit_init(NULL, 3, 1), ASX_E_INVALID_ARGUMENT); }

/* ================================================================== */
/* Integration tests                                                   */
/* ================================================================== */

TEST(integration_join_with_retry_and_pipeline) {
    asx_retry_state retry;
    asx_pipeline_state pipeline;
    asx_join_state join;
    asx_retry_policy p = asx_retry_policy_immediate(6);
    int retry_count = 0;
    int pipeline_count = 0;

    MUST_OK(asx_retry_init(&retry, poll_fail_n_then_ok, &retry_count, &p));
    MUST_OK(asx_pipeline_init(&pipeline));
    MUST_OK(asx_pipeline_add_stage(&pipeline, poll_pending_then_ok, &pipeline_count));
    MUST_OK(asx_pipeline_add_stage(&pipeline, poll_ok, NULL));

    MUST_OK(asx_join_init(&join));
    MUST_OK(asx_join_add(&join, asx_retry_poll, &retry));
    MUST_OK(asx_join_add(&join, asx_pipeline_poll, &pipeline));

    /* Poll 1: the retry's three attempts run (two fail, the third
     * succeeds); the pipeline's first stage is pending. Poll 2: that stage
     * completes and the second runs at once. */
    ASSERT_EQ(asx_join_poll(&join, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_join_poll(&join, 0), ASX_OK);

    ASSERT_EQ(asx_retry_attempts(&retry), 3u);
    ASSERT_EQ(asx_pipeline_completed_stages(&pipeline), 2u);
    ASSERT_EQ(asx_join_outcome(&join).severity, ASX_OUTCOME_OK);
}

/* ================================================================== */
/* Law tests — algebraic properties of combinators                     */
/* ================================================================== */

/* Law: retry(0) ≡ single attempt */
/* ================================================================== */
/* Adaptive hedge tests                                                */
/* ================================================================== */

TEST(adaptive_hedge_init_defaults) {
    asx_adaptive_hedge_policy p;
    asx_adaptive_hedge_init(&p, 5, 1000, 100000);
    ASSERT_EQ(p.alpha_pct, 5u);
    ASSERT_EQ(asx_adaptive_hedge_observations(&p), 0u);
}

TEST(adaptive_hedge_cold_start_returns_max) {
    asx_adaptive_hedge_policy p;
    asx_adaptive_hedge_init(&p, 5, 1000, 50000);
    /* With < 10 observations, should return max_delay */
    asx_adaptive_hedge_record(&p, 100);
    asx_adaptive_hedge_record(&p, 200);
    ASSERT_EQ(asx_adaptive_hedge_delay(&p), (uint64_t)50000);
}

TEST(adaptive_hedge_calibrates_from_history) {
    asx_adaptive_hedge_policy p;
    uint32_t i;
    uint64_t delay;

    asx_adaptive_hedge_init(&p, 5, 0, UINT64_MAX);

    /* Record 20 latencies: 100, 200, 300, ..., 2000 */
    for (i = 0; i < 20; i++) { asx_adaptive_hedge_record(&p, (uint64_t)((i + 1u) * 100u)); }

    delay = asx_adaptive_hedge_delay(&p);
    /* P95 of [100..2000] should be near 1900-2000 */
    ASSERT_TRUE(delay >= 1800);
    ASSERT_TRUE(delay <= 2000);
}

TEST(adaptive_hedge_clamps_to_bounds) {
    asx_adaptive_hedge_policy p;
    uint32_t i;
    uint64_t delay;

    asx_adaptive_hedge_init(&p, 5, 5000, 10000);

    /* Record 20 tiny latencies (all 1ns) */
    for (i = 0; i < 20; i++) { asx_adaptive_hedge_record(&p, 1); }

    delay = asx_adaptive_hedge_delay(&p);
    /* Conformal bound is 1ns, but clamped to min=5000 */
    ASSERT_EQ(delay, (uint64_t)5000);
}

/* ================================================================== */
/* Laws                                                                */
/* ================================================================== */

TEST(law_retry_once_is_single) {
    asx_retry_state rs;
    asx_retry_policy p = asx_retry_policy_immediate(1);
    MUST_OK(asx_retry_init(&rs, poll_ok, NULL, &p));
    ASSERT_EQ(asx_retry_poll(&rs, 0), ASX_OK);
    ASSERT_EQ(asx_retry_attempts(&rs), 1u);
}

/* Law: bracket with all-ok is transparent */
TEST(law_bracket_identity) {
    asx_bracket_state bs;
    MUST_OK(asx_bracket_init(&bs, poll_ok, poll_ok, poll_ok, NULL));
    ASSERT_EQ(asx_bracket_poll(&bs, 0), ASX_OK);
}

/* Law: pipeline(single stage f) ≡ f */
TEST(law_pipeline_single_identity) {
    asx_pipeline_state ps;
    MUST_OK(asx_pipeline_init(&ps));
    MUST_OK(asx_pipeline_add_stage(&ps, poll_ok, NULL));
    ASSERT_EQ(asx_pipeline_poll(&ps, 0), ASX_OK);
}

/* Law: bulkhead(1) admits exactly one */
TEST(law_bulkhead_one_is_mutex_like) {
    asx_bulkhead_state bs;
    MUST_OK(asx_bulkhead_init(&bs, 1));
    MUST_OK(asx_bulkhead_try_enter(&bs));
    ASSERT_EQ(asx_bulkhead_try_enter(&bs), ASX_E_WOULD_BLOCK);
    MUST_OK(asx_bulkhead_leave(&bs));
    MUST_OK(asx_bulkhead_try_enter(&bs));
}

/* Law: rate_limit starts full */
TEST(law_rate_limit_starts_full) {
    asx_rate_limit_state rs;
    MUST_OK(asx_rate_limit_init(&rs, 5, 1));
    ASSERT_EQ(asx_rate_limit_available(&rs), 5u);
}

/* Law: bracket release always called even on use failure */
TEST(law_bracket_release_guarantee) {
    asx_bracket_state bs;
    bracket_release_called = 0;
    MUST_OK(asx_bracket_init(&bs, bracket_acquire, bracket_use_fail, bracket_release, NULL));
    /* Ignore result, just verify release was called */
    st_sink_ = asx_bracket_poll(&bs, 0);
    (void)st_sink_;
    ASSERT_TRUE(bracket_release_called);
}

/* ================================================================== */
/* Hedge tests                                                         */
/* ================================================================== */

/* Primary completes immediately — should win */
TEST(hedge_primary_wins_immediately) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_ok, NULL, poll_fail, NULL, 1000000));
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_DECIDED);
}

/* Primary is pending, backup completes — backup wins.
 * In deterministic mode, deadline expires immediately, so both
 * branches race on first poll. */
static int hedge_primary_pending_count;
static asx_status hedge_primary_pending(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    hedge_primary_pending_count++;
    return ASX_E_PENDING;
}

TEST(hedge_backup_wins_when_primary_pending) {
    asx_hedge_state hs;
    hedge_primary_pending_count = 0;
    MUST_OK(asx_hedge_init(&hs, hedge_primary_pending, NULL, poll_ok, NULL, 0));
    /* With 0ns deadline, should go to RACING immediately */
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 1); /* backup won */
    /* The primary is polled once in that poll, then the backup starts, as
     * Rust's HedgeFuture (hedge.rs:698-765); before, it was polled twice. */
    ASSERT_EQ(hedge_primary_pending_count, 1);
}

TEST(hedge_null_fails) {
    asx_hedge_state hs;
    ASSERT_EQ(asx_hedge_init(NULL, poll_ok, NULL, poll_ok, NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hedge_init(&hs, NULL, NULL, poll_ok, NULL, 0), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hedge_init(&hs, poll_ok, NULL, NULL, NULL, 0), ASX_E_INVALID_ARGUMENT);
}

TEST(hedge_phase_tracking) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_ok, NULL, poll_ok, NULL, 1000000));
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_PRIMARY_ONLY);
    asx_hedge_poll(&hs, 0);
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_DECIDED);
}

TEST(hedge_primary_fail_result) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_fail, NULL, poll_ok, NULL, 1000000));
    /* Primary fails immediately — that's the result */
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
}

static asx_status hedge_backup_counted(void *user_data, asx_task_id self) {
    (void)self;
    (*(int *)user_data)++;
    return ASX_OK;
}

/* bd-bf6i: on a clock past the maximum timer duration of uptime the hedge
 * could not arm its deadline (the global wheel measured from time 0), so
 * it started the backup on the first poll instead of after the hedge
 * delay. */
TEST(hedge_waits_for_its_delay_after_long_uptime) {
    const asx_runtime_hooks *active = asx_runtime_get_hooks();
    asx_runtime_hooks saved;
    asx_runtime_hooks hooks;
    asx_vtime_state vt;
    asx_hedge_state hs;
    int backup_polls = 0;

    if (active != NULL) {
        memcpy(&saved, active, sizeof(saved));
    } else {
        ASSERT_EQ(asx_runtime_hooks_init(&saved), ASX_OK);
    }
    memcpy(&hooks, &saved, sizeof(hooks));
    asx_vtime_init(&vt, ASX_TIMER_MAX_DURATION_NS + 3600000000000ULL, 1000000ULL);
    hooks.clock.now_ns_fn = asx_vtime_now_ns;
    hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
    hooks.clock.ctx = &vt;
    ASSERT_EQ(asx_runtime_set_hooks(&hooks), ASX_OK);

    hedge_primary_pending_count = 0;
    MUST_OK(asx_hedge_init(&hs, hedge_primary_pending, NULL, hedge_backup_counted, &backup_polls,
                           1000000000ULL));
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_E_PENDING);
    ASSERT_EQ(asx_hedge_current_phase(&hs), ASX_HEDGE_PRIMARY_ONLY);
    ASSERT_EQ(hedge_primary_pending_count, 1);
    ASSERT_EQ(backup_polls, 0);

    MUST_OK(asx_runtime_set_hooks(&saved));
}

/* Parks its task and never completes. */
static asx_status hedge_primary_parks(void *user_data, asx_task_id self) {
    asx_status st = asx_task_park(self);
    (void)user_data;
    (void)st;
    return ASX_E_PENDING;
}

typedef struct {
    asx_hedge_state hs;
    uint32_t polls;
    asx_status result;
} hedge_task;

/* Drives a hedge from a task's poll; the task itself ends OK. */
static asx_status poll_hedge_task(void *user_data, asx_task_id self) {
    hedge_task *h = (hedge_task *)user_data;
    h->polls++;
    h->result = asx_hedge_poll(&h->hs, self);
    return h->result == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

TEST(hedge_delay_wakes_a_parked_owner) {
    /* The primary parks its task; the hedge delay (5 ms) wakes the owner,
     * as the Sleep of Rust's hedge does, and the backup then wins. Before,
     * nothing woke the owner and the run would block. */
    static hedge_task h;
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;

    /* The virtual clock (default stub hooks) even in live POSIX builds,
     * where asx_runtime_init_default installs a real one. */
    asx_runtime_config_init(&cfg);
    MUST_OK(asx_runtime_hooks_init(&hooks));
    MUST_OK(asx_runtime_init(&rt, &cfg, &hooks));
    memset(&h, 0, sizeof(h));
    MUST_OK(asx_hedge_init(&h.hs, hedge_primary_parks, NULL, poll_ok, NULL, 5000000ULL));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_hedge_task, &h, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(h.polls, 2u);
    ASSERT_EQ(h.result, ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&h.hs), 1);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)5000000u);
    asx_runtime_reset();
}

/* Cancels its own task and stays pending. */
static asx_status hedge_primary_cancels_owner(void *user_data, asx_task_id self) {
    asx_status st = asx_task_cancel(self, ASX_CANCEL_USER);
    (void)user_data;
    (void)st;
    return ASX_E_PENDING;
}

TEST(hedge_owner_cancel_drops_both_attempts) {
    /* Rust's HedgeFuture observes the caller's cancel before returning
     * Pending (hedge.rs:772-775): the primary cancels the owner, so the
     * hedge ends cancelled in that poll and the backup never starts. */
    static hedge_task h;
    static int backup_polls;
    asx_runtime rt;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;

    MUST_OK(asx_runtime_init_default(&rt));
    memset(&h, 0, sizeof(h));
    backup_polls = 0;
    MUST_OK(asx_hedge_init(&h.hs, hedge_primary_cancels_owner, NULL, hedge_backup_counted,
                           &backup_polls, 5000000ULL));
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_hedge_task, &h, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(h.polls, 1u);
    ASSERT_EQ(h.result, ASX_E_CANCELLED);
    ASSERT_EQ(asx_hedge_current_phase(&h.hs), ASX_HEDGE_DECIDED);
    ASSERT_EQ(backup_polls, 0);
    asx_runtime_reset();
}

/* ================================================================== */
/* MapReduce tests                                                     */
/* ================================================================== */

static asx_status reduce_count_ok(const asx_status *results, uint32_t count, void *user_data) {
    uint32_t i, ok_count = 0;
    (void)user_data;
    for (i = 0; i < count; i++) {
        if (results[i] == ASX_OK) ok_count++;
    }
    return (ok_count == count) ? ASX_OK : ASX_E_INVALID_STATE;
}

TEST(map_reduce_all_ok) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 3u);
}

static int map_reduce_reduces;

static asx_status reduce_counting(const asx_status *results, uint32_t count, void *user_data) {
    (void)results;
    (void)count;
    (void)user_data;
    map_reduce_reduces++;
    return ASX_OK;
}

static asx_status poll_pending_counted(void *user_data, asx_task_id self) {
    (void)self;
    (*(int *)user_data)++;
    return ASX_E_PENDING;
}

TEST(map_reduce_stops_at_the_first_failure) {
    /* Rust's executing map-reduce stops at the first failure and drains
     * the rest (map_reduce.rs:7-11): branch 1 fails while branch 0 is
     * pending, so both pending branches are dropped (branch 2 is never
     * polled), the failure is the result and nothing is reduced. */
    asx_map_reduce_state ms;
    int first_polls = 0;
    int last_polls = 0;

    map_reduce_reduces = 0;
    MUST_OK(asx_map_reduce_init(&ms, reduce_counting, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_pending_counted, &first_polls));
    MUST_OK(asx_map_reduce_add(&ms, poll_fail, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_pending_counted, &last_polls));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(first_polls, 1);
    ASSERT_EQ(last_polls, 0);
    ASSERT_EQ(map_reduce_reduces, 0);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 1u);
    ASSERT_EQ(ms.dropped, 2u);
    /* Decided: a repoll returns the failure without polling anything. */
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(first_polls, 1);
}

TEST(map_reduce_stops_at_a_cancelled_branch) {
    asx_map_reduce_state ms;
    int polls = 0;

    map_reduce_reduces = 0;
    MUST_OK(asx_map_reduce_init(&ms, reduce_counting, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_cancelled_count, &polls));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_CANCELLED);
    ASSERT_EQ(map_reduce_reduces, 0);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 2u);
    ASSERT_EQ(ms.dropped, 1u);
}

TEST(map_reduce_reduces_once_all_succeed) {
    asx_map_reduce_state ms;
    int count = 0;

    map_reduce_reduces = 0;
    MUST_OK(asx_map_reduce_init(&ms, reduce_counting, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_pending_then_ok, &count));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_PENDING);
    ASSERT_EQ(map_reduce_reduces, 0);
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
    ASSERT_EQ(map_reduce_reduces, 1);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 2u);
}

TEST(map_reduce_empty) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    /* No branches — reduce called with count=0 */
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
}

TEST(map_reduce_null_fails) {
    asx_map_reduce_state ms;
    ASSERT_EQ(asx_map_reduce_init(NULL, reduce_count_ok, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_map_reduce_init(&ms, NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Dropped attempts release what they hold (bd-9kll.3.11)             */
/* ================================================================== */

/* Counts releases of the branch whose user_data it is. */
static void release_counted(void *user_data) { (*(int *)user_data) += 100; }

TEST(map_reduce_releases_the_branches_a_failure_leaves_pending) {
    /* The counters take +1 per poll and +100 per release. */
    asx_map_reduce_state ms;
    int first = 0;
    int last = 0;
    int failing = 0;

    MUST_OK(asx_map_reduce_init(&ms, reduce_counting, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_pending_counted, &first));
    MUST_OK(asx_map_reduce_add(&ms, poll_fail, &failing));
    MUST_OK(asx_map_reduce_add(&ms, poll_pending_counted, &last));
    ms.branches[0].drop_fn = release_counted;
    ms.branches[1].drop_fn = release_counted;
    ms.branches[2].drop_fn = release_counted;
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(first, 101); /* polled once, released once */
    ASSERT_EQ(last, 100);  /* never polled, released once */
    ASSERT_EQ(failing, 0); /* finished: not released */
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(first, 101);
    ASSERT_EQ(last, 100);
}

/* A dropped branch owns a real channel reservation. Its release also
 * re-polls the combinator, as an application cleanup callback may do. */
typedef struct {
    asx_map_reduce_state *owner;
    asx_send_permit permit;
    uint32_t polls;
    uint32_t drops;
    asx_status nested_result;
} map_release_probe;

static asx_status poll_reserved_pending(void *user_data, asx_task_id self) {
    map_release_probe *p = (map_release_probe *)user_data;
    (void)self;
    p->polls++;
    return ASX_E_PENDING;
}

static void release_reserved_and_repoll(void *user_data) {
    map_release_probe *p = (map_release_probe *)user_data;
    p->drops++;
    asx_send_permit_abort(&p->permit);
    p->nested_result = asx_map_reduce_poll(p->owner, 0);
}

TEST(map_reduce_caches_failure_before_reentrant_resource_release) {
    asx_map_reduce_state ms;
    map_release_probe first;
    map_release_probe last;
    asx_region_id region;
    asx_channel_id channel;
    asx_send_permit next;
    uint32_t reserved;

    asx_runtime_reset();
    memset(&first, 0, sizeof(first));
    memset(&last, 0, sizeof(last));
    first.owner = &ms;
    last.owner = &ms;
    first.nested_result = ASX_E_PENDING;
    last.nested_result = ASX_E_PENDING;
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_channel_create(region, 2u, &channel), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(channel, &first.permit), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(channel, &last.permit), ASX_OK);
    ASSERT_EQ(asx_channel_reserved_count(channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 2u);
    map_reduce_reduces = 0;
    ASSERT_EQ(asx_map_reduce_init(&ms, reduce_counting, NULL), ASX_OK);
    ASSERT_EQ(asx_map_reduce_add(&ms, poll_reserved_pending, &first), ASX_OK);
    ASSERT_EQ(asx_map_reduce_add(&ms, poll_fail, NULL), ASX_OK);
    ASSERT_EQ(asx_map_reduce_add(&ms, poll_reserved_pending, &last), ASX_OK);
    ms.branches[0].drop_fn = release_reserved_and_repoll;
    ms.branches[2].drop_fn = release_reserved_and_repoll;

    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(first.nested_result, ASX_E_INVALID_STATE);
    ASSERT_EQ(last.nested_result, ASX_E_INVALID_STATE);
    ASSERT_EQ(map_reduce_reduces, 0);
    ASSERT_EQ(first.polls, 1u);
    ASSERT_EQ(last.polls, 0u);
    ASSERT_EQ(first.drops, 1u);
    ASSERT_EQ(last.drops, 1u);
    ASSERT_EQ(ms.dropped, 2u);
    ASSERT_EQ(first.permit.consumed, 1);
    ASSERT_EQ(last.permit.consumed, 1);
    ASSERT_EQ(asx_channel_reserved_count(channel, &reserved), ASX_OK);
    ASSERT_EQ(reserved, 0u);
    ASSERT_EQ(asx_channel_try_reserve(channel, &next), ASX_OK);
    asx_send_permit_abort(&next);
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_E_INVALID_STATE);
    ASSERT_EQ(first.drops + last.drops, 2u);
    asx_runtime_reset();
}

TEST(hedge_releases_the_attempt_that_did_not_win) {
    /* Primary wins before the hedge delay: the backup never started and
     * is released once with its own user_data (Rust drops the backup
     * factory). Then a 0 ns delay: the backup wins and the pending
     * primary is released once. */
    asx_hedge_state hs;
    int primary = 0;
    int backup = 0;

    MUST_OK(asx_hedge_init(&hs, poll_ok, &primary, poll_ok, &backup, 1000000));
    hs.primary.drop_fn = release_counted;
    hs.backup.drop_fn = release_counted;
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
    ASSERT_EQ(primary, 0);
    ASSERT_EQ(backup, 100);
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(backup, 100);

    primary = 0;
    backup = 0;
    MUST_OK(asx_hedge_init(&hs, poll_pending_counted, &primary, poll_ok, &backup, 0));
    hs.primary.drop_fn = release_counted;
    hs.backup.drop_fn = release_counted;
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 1);
    ASSERT_EQ(primary, 101); /* polled once, released once */
    ASSERT_EQ(backup, 0);

    /* The owner is cancelled while the primary runs: both attempts are
     * released once, the backup never polled. */
    {
        static hedge_task h;
        asx_runtime rt;
        asx_region_id r;
        asx_task_id t;
        asx_budget budget;

        primary = 0;
        backup = 0;
        MUST_OK(asx_runtime_init_default(&rt));
        memset(&h, 0, sizeof(h));
        MUST_OK(asx_hedge_init(&h.hs, hedge_primary_cancels_owner, &primary, poll_pending_counted,
                               &backup, 5000000ULL));
        h.hs.primary.drop_fn = release_counted;
        h.hs.backup.drop_fn = release_counted;
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        ASSERT_EQ(asx_task_spawn(r, poll_hedge_task, &h, &t), ASX_OK);
        budget = asx_budget_from_polls(100);
        ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
        ASSERT_EQ(h.result, ASX_E_CANCELLED);
        ASSERT_EQ(primary, 100);
        ASSERT_EQ(backup, 100);
        asx_runtime_reset();
    }
}

/* ================================================================== */
/* Additional Laws                                                     */
/* ================================================================== */

/* Law: hedge with immediate primary is equivalent to just running primary */
TEST(law_hedge_immediate_is_primary) {
    asx_hedge_state hs;
    MUST_OK(asx_hedge_init(&hs, poll_ok, NULL, poll_fail, NULL, 999999999));
    ASSERT_EQ(asx_hedge_poll(&hs, 0), ASX_OK);
    ASSERT_EQ(asx_hedge_winner(&hs), 0);
}

/* Law: map_reduce with one branch is identity */
TEST(law_map_reduce_single_is_identity) {
    asx_map_reduce_state ms;
    MUST_OK(asx_map_reduce_init(&ms, reduce_count_ok, NULL));
    MUST_OK(asx_map_reduce_add(&ms, poll_ok, NULL));
    ASSERT_EQ(asx_map_reduce_poll(&ms, 0), ASX_OK);
    ASSERT_EQ(asx_map_reduce_completed(&ms), 1u);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_combinator2 ===\n");

    /* Retry */
    RUN_TEST(retry_success_first_try);
    RUN_TEST(retry_fail_then_succeed);
    RUN_TEST(retry_exhausted);
    RUN_TEST(retry_stops_at_a_cancelled_attempt);
    RUN_TEST(retry_starts_no_attempt_once_its_owner_is_cancelled);
    RUN_TEST(retry_single_attempt);
    RUN_TEST(retry_with_pending_inner);
    RUN_TEST(retry_null_fails);
    RUN_TEST(retry_policy_constructors_are_rusts);
    RUN_TEST(retry_init_refuses_what_rust_validate_refuses);
    RUN_TEST(retry_policy_delay_matches_rust_calculate_delay);
    RUN_TEST(retry_backs_off_between_attempts);
    RUN_TEST(retry_jitter_is_drawn_from_the_owner_entropy);
    RUN_TEST(retry_cancel_during_the_delay_starts_no_attempt);

    /* Bracket */
    RUN_TEST(bracket_success);
    RUN_TEST(bracket_use_fails_release_still_called);
    RUN_TEST(bracket_acquire_fails_no_release);
    RUN_TEST(bracket_phase_tracking);
    RUN_TEST(bracket_null_fails);

    /* Pipeline */
    RUN_TEST(pipeline_empty);
    RUN_TEST(pipeline_single_stage);
    RUN_TEST(pipeline_three_stages);
    RUN_TEST(pipeline_stops_on_failure);
    RUN_TEST(pipeline_with_pending_stage);
    RUN_TEST(pipeline_overflow);

    /* Bulkhead */
    RUN_TEST(bulkhead_basic);
    RUN_TEST(bulkhead_leave_empty_fails);
    RUN_TEST(bulkhead_zero_fails);
    RUN_TEST(bulkhead_null_fails);

    /* RateLimit */
    RUN_TEST(rate_limit_basic);
    RUN_TEST(rate_limit_refill);
    RUN_TEST(rate_limit_refill_caps_at_capacity);
    RUN_TEST(rate_limit_zero_capacity_fails);
    RUN_TEST(rate_limit_null_fails);

    /* Integration */
    RUN_TEST(integration_join_with_retry_and_pipeline);

    /* Hedge */
    RUN_TEST(hedge_primary_wins_immediately);
    RUN_TEST(hedge_backup_wins_when_primary_pending);
    RUN_TEST(hedge_null_fails);
    RUN_TEST(hedge_phase_tracking);
    RUN_TEST(hedge_primary_fail_result);

    /* MapReduce */
    RUN_TEST(map_reduce_all_ok);
    RUN_TEST(map_reduce_stops_at_the_first_failure);
    RUN_TEST(map_reduce_stops_at_a_cancelled_branch);
    RUN_TEST(map_reduce_reduces_once_all_succeed);
    RUN_TEST(map_reduce_empty);
    RUN_TEST(map_reduce_null_fails);
    RUN_TEST(map_reduce_releases_the_branches_a_failure_leaves_pending);
    RUN_TEST(map_reduce_caches_failure_before_reentrant_resource_release);
    RUN_TEST(hedge_releases_the_attempt_that_did_not_win);

    /* Adaptive hedge */
    RUN_TEST(adaptive_hedge_init_defaults);
    RUN_TEST(adaptive_hedge_cold_start_returns_max);
    RUN_TEST(adaptive_hedge_calibrates_from_history);
    RUN_TEST(adaptive_hedge_clamps_to_bounds);

    /* Laws */
    RUN_TEST(law_retry_once_is_single);
    RUN_TEST(law_bracket_identity);
    RUN_TEST(law_pipeline_single_identity);
    RUN_TEST(law_bulkhead_one_is_mutex_like);
    RUN_TEST(law_rate_limit_starts_full);
    RUN_TEST(law_bracket_release_guarantee);
    RUN_TEST(law_hedge_immediate_is_primary);
    RUN_TEST(law_map_reduce_single_is_identity);

    /* Last: installs a clock hook */
    RUN_TEST(hedge_waits_for_its_delay_after_long_uptime);
    RUN_TEST(hedge_delay_wakes_a_parked_owner);
    RUN_TEST(hedge_owner_cancel_drops_both_attempts);

    TEST_REPORT();
    return test_failures;
}
