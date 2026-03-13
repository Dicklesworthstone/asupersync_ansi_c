/*
 * test_actor.c — unit tests for actor model and gen_server semantics
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/actor.h>
#include <asx/core/budget.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helpers: region + scheduler                                         */
/* ------------------------------------------------------------------ */

static asx_region_id make_region(void) {
    asx_region_id r;
    MUST_OK(asx_region_open(&r));
    return r;
}

static void pump_region(asx_region_id region, uint32_t rounds) {
    asx_budget budget = asx_budget_infinite();
    uint32_t i;
    budget.poll_quota = 1000;
    for (i = 0; i < rounds; i++) {
        asx_budget b = budget;
        MUST_OK(asx_scheduler_run(region, &b));
    }
}

/* Single-poll: advance exactly one poll unit */
static void pump_one_poll(asx_region_id region) {
    asx_budget b = asx_budget_infinite();
    b.poll_quota = 1;
    st_sink_ = asx_scheduler_run(region, &b);
    (void)st_sink_;
}

/* ------------------------------------------------------------------ */
/* Test behaviors                                                      */
/* ------------------------------------------------------------------ */

/* Simple echo actor: stores last received cast message */
typedef struct {
    uint64_t last_msg;
    uint32_t msg_count;
    int init_called;
    int term_called;
    asx_status term_reason;
} echo_state;

static asx_status echo_init(void *state, asx_actor_handle self) {
    echo_state *s = (echo_state *)state;
    (void)self;
    s->init_called = 1;
    return ASX_OK;
}

static asx_status echo_cast(void *state, uint64_t msg, asx_actor_handle self) {
    echo_state *s = (echo_state *)state;
    (void)self;
    s->last_msg = msg;
    s->msg_count++;
    return ASX_OK;
}

static asx_status echo_call(void *state, uint64_t request, uint64_t *reply, asx_actor_handle self) {
    (void)state;
    (void)self;
    /* Echo back the request doubled */
    *reply = request * 2;
    return ASX_OK;
}

static void echo_terminate(void *state, asx_status reason, asx_actor_handle self) {
    echo_state *s = (echo_state *)state;
    (void)self;
    s->term_called = 1;
    s->term_reason = reason;
}

static asx_actor_behavior echo_behavior(void) {
    asx_actor_behavior b;
    b.init = echo_init;
    b.handle_cast = echo_cast;
    b.handle_call = echo_call;
    b.terminate = echo_terminate;
    return b;
}

/* Minimal actor: cast only, no init/call/terminate */
static asx_status noop_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    return ASX_OK;
}

static asx_actor_behavior minimal_behavior(void) {
    asx_actor_behavior b;
    b.init = NULL;
    b.handle_cast = noop_cast;
    b.handle_call = NULL;
    b.terminate = NULL;
    return b;
}

/* Failing init actor */
static asx_status fail_init(void *state, asx_actor_handle self) {
    (void)state;
    (void)self;
    return ASX_E_INVALID_STATE;
}

/* Failing cast actor */
static uint32_t g_fail_after_count;
static uint32_t g_fail_cast_count;

static asx_status fail_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    g_fail_cast_count++;
    if (g_fail_cast_count >= g_fail_after_count) { return ASX_E_INVALID_STATE; }
    return ASX_OK;
}

/* Accumulator actor: sums all cast messages */
typedef struct {
    uint64_t sum;
} accum_state;

static asx_status accum_cast(void *state, uint64_t msg, asx_actor_handle self) {
    accum_state *s = (accum_state *)state;
    (void)self;
    s->sum += msg;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Tests: Spawn and lifecycle                                          */
/* ------------------------------------------------------------------ */

static void test_spawn_basic(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    st = asx_actor_spawn(&h, r, &b, &state);
    ASSERT(st == ASX_OK, "spawn should succeed");
    ASSERT(asx_actor_is_alive(h), "actor should be alive");
    ASSERT(h.generation != 0, "generation should be non-zero");
}

static void test_spawn_null_args(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    st = asx_actor_spawn(NULL, r, &b, NULL);
    ASSERT(st == ASX_E_INVALID_ARGUMENT, "null out should fail");

    st = asx_actor_spawn(&h, r, NULL, NULL);
    ASSERT(st == ASX_E_INVALID_ARGUMENT, "null behavior should fail");

    /* handle_cast is required */
    {
        asx_actor_behavior bad;
        bad.init = NULL;
        bad.handle_cast = NULL;
        bad.handle_call = NULL;
        bad.terminate = NULL;
        st = asx_actor_spawn(&h, r, &bad, NULL);
        ASSERT(st == ASX_E_INVALID_ARGUMENT, "null handle_cast should fail");
    }
}

static void test_spawn_arena_exhaustion(void) {
    asx_actor_handle handles[ASX_MAX_ACTORS + 1];
    asx_actor_behavior b = minimal_behavior();
    asx_region_id r;
    uint32_t i;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    for (i = 0; i < ASX_MAX_ACTORS; i++) {
        st = asx_actor_spawn(&handles[i], r, &b, NULL);
        ASSERT(st == ASX_OK, "spawn within limit should succeed");
    }

    st = asx_actor_spawn(&handles[ASX_MAX_ACTORS], r, &b, NULL);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "spawn beyond limit should fail");
}

static void test_init_callback(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    ASSERT(!state.init_called, "init should not be called before polling");

    pump_region(r, 1);
    ASSERT(state.init_called, "init should be called on first poll");
}

static void test_init_failure_terminates(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    b.init = fail_init;
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 2);

    ASSERT(state.term_called, "terminate should be called on init failure");
    ASSERT(state.term_reason == ASX_E_INVALID_STATE, "terminate reason should be init error");
    ASSERT(!asx_actor_is_alive(h), "actor should be dead after init failure");
}

static void test_minimal_no_init_no_terminate(void) {
    asx_actor_handle h;
    asx_actor_behavior b = minimal_behavior();
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    st = asx_actor_spawn(&h, r, &b, NULL);
    ASSERT(st == ASX_OK, "minimal actor should spawn");

    /* Init poll (no init callback) */
    pump_region(r, 1);
    ASSERT(asx_actor_is_alive(h), "minimal actor should survive init");
}

/* ------------------------------------------------------------------ */
/* Tests: Cast (fire-and-forget)                                       */
/* ------------------------------------------------------------------ */

static void test_cast_basic(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    st = asx_actor_cast(h, 42);
    ASSERT(st == ASX_OK, "cast should succeed");
    ASSERT(asx_actor_mailbox_count(h) == 1, "mailbox should have 1 message");

    pump_region(r, 1); /* process cast */
    ASSERT(state.last_msg == 42, "should have received message 42");
    ASSERT(state.msg_count == 1, "should have processed 1 message");
}

static void test_cast_ordering(void) {
    asx_actor_handle h;
    asx_actor_behavior b;
    accum_state state;
    asx_region_id r;
    uint32_t i;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    b.init = NULL;
    b.handle_cast = accum_cast;
    b.handle_call = NULL;
    b.terminate = NULL;
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    /* Send multiple messages */
    for (i = 1; i <= 5; i++) { MUST_OK(asx_actor_cast(h, (uint64_t)i)); }
    ASSERT(asx_actor_mailbox_count(h) == 5, "mailbox should have 5 messages");

    /* Process all (one per poll) */
    pump_region(r, 5);
    ASSERT(state.sum == 15, "sum should be 1+2+3+4+5=15");
}

static void test_cast_mailbox_full(void) {
    asx_actor_handle h;
    asx_actor_behavior b = minimal_behavior();
    asx_region_id r;
    uint32_t i;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, NULL));
    pump_region(r, 1); /* init */

    /* Fill mailbox */
    for (i = 0; i < ASX_ACTOR_MAILBOX_CAPACITY; i++) {
        st = asx_actor_cast(h, (uint64_t)i);
        ASSERT(st == ASX_OK, "cast within capacity should succeed");
    }

    /* Next cast should fail */
    st = asx_actor_cast(h, 999);
    ASSERT(st == ASX_E_WOULD_BLOCK, "cast to full mailbox should block");
}

static void test_cast_to_dead_actor(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_status st;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    b.init = fail_init;
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 2); /* init fails, actor dies */

    st = asx_actor_cast(h, 42);
    ASSERT(st == ASX_E_INVALID_ARGUMENT, "cast to dead actor should fail");
}

/* ------------------------------------------------------------------ */
/* Tests: Call (request/reply)                                         */
/* ------------------------------------------------------------------ */

static void test_call_basic(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_call_token token;
    uint64_t reply;
    asx_status st;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    st = asx_actor_call(h, 21, &token);
    ASSERT(st == ASX_OK, "call should succeed");

    /* Reply not ready yet */
    st = asx_call_token_poll(token, &reply);
    ASSERT(st == ASX_E_PENDING, "reply should be pending before processing");

    pump_region(r, 1); /* process call */

    st = asx_call_token_poll(token, &reply);
    ASSERT(st == ASX_OK, "reply should be available");
    ASSERT(reply == 42, "reply should be 21*2=42");
}

static void test_call_multiple(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_call_token tokens[3];
    uint64_t reply;
    uint32_t i;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    for (i = 0; i < 3; i++) { MUST_OK(asx_actor_call(h, (uint64_t)(i + 1), &tokens[i])); }

    pump_region(r, 3); /* process all 3 calls */

    for (i = 0; i < 3; i++) {
        MUST_OK(asx_call_token_poll(tokens[i], &reply));
        ASSERT(reply == (uint64_t)(i + 1) * 2, "each reply should be request*2");
    }
}

static void test_call_pending_exhaustion(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_call_token tokens[ASX_ACTOR_MAX_PENDING_CALLS + 1];
    uint32_t i;
    asx_status st;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    for (i = 0; i < ASX_ACTOR_MAX_PENDING_CALLS; i++) {
        st = asx_actor_call(h, (uint64_t)i, &tokens[i]);
        ASSERT(st == ASX_OK, "call within pending limit should succeed");
    }

    st = asx_actor_call(h, 999, &tokens[ASX_ACTOR_MAX_PENDING_CALLS]);
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "call beyond pending limit should fail");
}

static void test_call_null_handler(void) {
    asx_actor_handle h;
    asx_actor_behavior b = minimal_behavior(); /* no call handler */
    asx_region_id r;
    asx_call_token token;
    uint64_t reply;
    asx_status st;

    asx_runtime_reset();
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, NULL));
    pump_region(r, 1); /* init */

    MUST_OK(asx_actor_call(h, 42, &token));
    pump_region(r, 1); /* process — no handler, slot released */

    st = asx_call_token_poll(token, &reply);
    ASSERT(st == ASX_E_INVALID_STATE, "call with no handler should return invalid state");
}

/* ------------------------------------------------------------------ */
/* Tests: Stop (graceful shutdown)                                     */
/* ------------------------------------------------------------------ */

static void test_stop_basic(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */
    ASSERT(asx_actor_is_alive(h), "should be alive after init");

    MUST_OK(asx_actor_stop(h));
    pump_region(r, 2); /* process stop, then terminate */

    ASSERT(!asx_actor_is_alive(h), "should be dead after stop");
    ASSERT(state.term_called, "terminate should be called");
    ASSERT(state.term_reason == ASX_OK, "graceful stop reason should be OK");
}

static void test_stop_processes_remaining_messages(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    /* Send messages then stop */
    MUST_OK(asx_actor_cast(h, 10));
    MUST_OK(asx_actor_cast(h, 20));
    MUST_OK(asx_actor_stop(h));

    /* Messages should be processed before stop takes effect */
    pump_region(r, 5);
    ASSERT(!asx_actor_is_alive(h), "should be dead after draining");
    ASSERT(state.msg_count == 2, "both messages should be processed");
    ASSERT(state.last_msg == 20, "last message should be 20");
}

/* ------------------------------------------------------------------ */
/* Tests: Error handling                                               */
/* ------------------------------------------------------------------ */

static void test_cast_handler_failure(void) {
    asx_actor_handle h;
    asx_actor_behavior b;
    echo_state state;
    asx_region_id r;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    g_fail_after_count = 2;
    g_fail_cast_count = 0;

    b.init = NULL;
    b.handle_cast = fail_cast;
    b.handle_call = NULL;
    b.terminate = echo_terminate;
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    MUST_OK(asx_actor_cast(h, 1));
    MUST_OK(asx_actor_cast(h, 2));
    MUST_OK(asx_actor_cast(h, 3));

    pump_region(r, 5);

    ASSERT(!asx_actor_is_alive(h), "should be dead after handler failure");
    ASSERT(state.term_called, "terminate should be called");
    ASSERT(state.term_reason == ASX_E_INVALID_STATE, "terminate reason should be handler error");
    ASSERT(g_fail_cast_count == 2, "should have processed 2 messages before failure");
}

static void test_stale_handle(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_actor_handle stale;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    stale = h;
    pump_region(r, 1); /* init */

    /* Kill actor */
    b.init = fail_init;
    MUST_OK(asx_actor_stop(h));
    pump_region(r, 2);
    ASSERT(!asx_actor_is_alive(stale), "stale handle should report dead");

    /* Spawn new actor in same slot */
    b.init = echo_init;
    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    ASSERT(h.slot == stale.slot, "should reuse same slot");
    ASSERT(h.generation != stale.generation, "generation should differ");

    /* Stale handle should not work */
    ASSERT(!asx_actor_is_alive(stale), "stale handle should not find new actor");
    ASSERT(asx_actor_cast(stale, 1) == ASX_E_INVALID_ARGUMENT,
           "cast with stale handle should fail");
}

/* ------------------------------------------------------------------ */
/* Tests: Task integration                                             */
/* ------------------------------------------------------------------ */

static void test_task_id_retrieval(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_task_id tid;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));

    MUST_OK(asx_actor_task_id(h, &tid));
    ASSERT(asx_handle_is_valid(tid), "task id should be valid");
}

/* ------------------------------------------------------------------ */
/* Tests: Reset                                                        */
/* ------------------------------------------------------------------ */

static void test_reset(void) {
    asx_actor_handle h;
    asx_actor_behavior b = minimal_behavior();
    asx_region_id r;

    asx_runtime_reset();
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, NULL));
    ASSERT(asx_actor_is_alive(h), "should be alive before reset");

    asx_actor_reset();
    ASSERT(!asx_actor_is_alive(h), "should be dead after reset");
}

/* ------------------------------------------------------------------ */
/* Tests: Law/invariant tests                                          */
/* ------------------------------------------------------------------ */

/* Law: FIFO ordering — messages processed in send order */
static uint64_t g_order_log[16];
static uint32_t g_order_count;

static asx_status order_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)self;
    if (g_order_count < 16) { g_order_log[g_order_count++] = msg; }
    return ASX_OK;
}

static void test_law_fifo_ordering(void) {
    asx_actor_handle h;
    asx_actor_behavior b;
    asx_region_id r;
    uint32_t i;

    asx_runtime_reset();
    g_order_count = 0;
    b.init = NULL;
    b.handle_cast = order_cast;
    b.handle_call = NULL;
    b.terminate = NULL;
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, NULL));
    pump_region(r, 1); /* init */

    for (i = 0; i < 8; i++) { MUST_OK(asx_actor_cast(h, (uint64_t)(i * 10))); }

    pump_region(r, 8);

    ASSERT(g_order_count == 8, "should have received 8 messages");
    for (i = 0; i < 8; i++) {
        ASSERT(g_order_log[i] == (uint64_t)(i * 10), "messages should be in FIFO order");
    }
}

/* Law: call reply is consumed exactly once */
static void test_law_call_consumed_once(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_call_token token;
    uint64_t reply;
    asx_status st;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    MUST_OK(asx_actor_call(h, 5, &token));
    pump_region(r, 1); /* process call */

    /* First poll consumes */
    st = asx_call_token_poll(token, &reply);
    ASSERT(st == ASX_OK, "first poll should succeed");
    ASSERT(reply == 10, "reply should be 5*2=10");

    /* Second poll fails (slot released) */
    st = asx_call_token_poll(token, &reply);
    ASSERT(st == ASX_E_INVALID_STATE, "second poll should fail (consumed)");
}

/* Law: terminate is always called (even on error) */
static void test_law_terminate_always_called(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;

    /* Test 1: graceful stop */
    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1);
    MUST_OK(asx_actor_stop(h));
    pump_region(r, 2);
    ASSERT(state.term_called, "terminate should be called on graceful stop");

    /* Test 2: init failure */
    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    b.init = fail_init;
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 2);
    ASSERT(state.term_called, "terminate should be called on init failure");
}

/* Law: mailbox count is accurate */
static void test_law_mailbox_count(void) {
    asx_actor_handle h;
    asx_actor_behavior b = minimal_behavior();
    asx_region_id r;

    asx_runtime_reset();
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, NULL));
    pump_one_poll(r); /* init (single poll) */
    ASSERT(asx_actor_mailbox_count(h) == 0, "empty mailbox");

    MUST_OK(asx_actor_cast(h, 1));
    MUST_OK(asx_actor_cast(h, 2));
    ASSERT(asx_actor_mailbox_count(h) == 2, "2 messages");

    pump_one_poll(r); /* process 1 message */
    ASSERT(asx_actor_mailbox_count(h) == 1, "1 message after processing");

    pump_one_poll(r); /* process 2nd message */
    ASSERT(asx_actor_mailbox_count(h) == 0, "0 messages after processing all");
}

/* ------------------------------------------------------------------ */
/* Tests: Interleaved cast and call                                    */
/* ------------------------------------------------------------------ */

static void test_interleaved_cast_call(void) {
    asx_actor_handle h;
    asx_actor_behavior b = echo_behavior();
    echo_state state;
    asx_region_id r;
    asx_call_token token;
    uint64_t reply;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    r = make_region();

    MUST_OK(asx_actor_spawn(&h, r, &b, &state));
    pump_region(r, 1); /* init */

    /* Cast, call, cast */
    MUST_OK(asx_actor_cast(h, 100));
    MUST_OK(asx_actor_call(h, 50, &token));
    MUST_OK(asx_actor_cast(h, 200));

    pump_region(r, 3); /* process all 3 */

    ASSERT(state.msg_count == 2, "2 cast messages processed");
    ASSERT(state.last_msg == 200, "last cast was 200");
    MUST_OK(asx_call_token_poll(token, &reply));
    ASSERT(reply == 100, "call reply should be 50*2=100");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_actor:\n");

    /* Spawn and lifecycle */
    RUN(test_spawn_basic);
    RUN(test_spawn_null_args);
    RUN(test_spawn_arena_exhaustion);
    RUN(test_init_callback);
    RUN(test_init_failure_terminates);
    RUN(test_minimal_no_init_no_terminate);

    /* Cast */
    RUN(test_cast_basic);
    RUN(test_cast_ordering);
    RUN(test_cast_mailbox_full);
    RUN(test_cast_to_dead_actor);

    /* Call */
    RUN(test_call_basic);
    RUN(test_call_multiple);
    RUN(test_call_pending_exhaustion);
    RUN(test_call_null_handler);

    /* Stop */
    RUN(test_stop_basic);
    RUN(test_stop_processes_remaining_messages);

    /* Error handling */
    RUN(test_cast_handler_failure);
    RUN(test_stale_handle);

    /* Task integration */
    RUN(test_task_id_retrieval);

    /* Reset */
    RUN(test_reset);

    /* Law tests */
    RUN(test_law_fifo_ordering);
    RUN(test_law_call_consumed_once);
    RUN(test_law_terminate_always_called);
    RUN(test_law_mailbox_count);

    /* Interleaved */
    RUN(test_interleaved_cast_call);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
