/*
 * test_actor.c — unit tests for the GenServer port (Rust gen_server.rs):
 * a server task on an mpsc mailbox, casts and calls from client tasks,
 * stop and join, cancellation and drain, failing callbacks.
 *
 * Rust parity of the traces, obligations and schedules is covered by the
 * conformance fixtures fixtures/rust_reference_v2/actor-*.json; these tests
 * cover the C API's own contract.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/actor.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
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
        int before_ = g_fail;                                                                      \
        printf("  " #fn "...\n");                                                                  \
        fn();                                                                                      \
        if (g_fail == before_) g_pass++;                                                           \
    } while (0)

/* ------------------------------------------------------------------ */
/* Regions and scheduling                                              */
/* ------------------------------------------------------------------ */

static asx_region_id g_root;

/* A fresh runtime with one root region. */
static void setup(void) {
    asx_runtime_reset();
    MUST_OK(asx_region_open(&g_root));
}

static asx_region_id child_region(void) {
    asx_region_id r = ASX_INVALID_ID;
    MUST_OK(asx_region_open_child(g_root, &r));
    return r;
}

/* Run a region's subtree until nothing is runnable. */
static void run(asx_region_id region) {
    asx_budget b = asx_budget_from_polls(1000);
    st_sink_ = asx_scheduler_run(region, &b);
    (void)st_sink_;
}

/* Exactly one poll in a region's subtree. */
static void run_one_poll(asx_region_id region) {
    asx_budget b = asx_budget_from_polls(1);
    st_sink_ = asx_scheduler_run(region, &b);
    (void)st_sink_;
}

static asx_outcome_severity task_outcome(asx_actor_handle h) {
    asx_task_id tid = ASX_INVALID_ID;
    asx_outcome o;
    MUST_OK(asx_actor_task_id(h, &tid));
    o.severity = ASX_OUTCOME_OK;
    MUST_OK(asx_task_get_outcome(tid, &o));
    return o.severity;
}

/* ------------------------------------------------------------------ */
/* Behaviors                                                           */
/* ------------------------------------------------------------------ */

/* Records what it was given: casts in order, the call count, init and
 * terminate. A call replies with the sum of the casts plus the request. */
typedef struct {
    uint64_t log[80];
    uint32_t casts;
    uint32_t calls;
    uint64_t sum;
    int init_called;
    int term_called;
    asx_status term_reason;
    uint32_t fail_cast_at; /* 1-based cast that fails, 0 for none */
    int fail_init;
} rec_state;

static asx_status rec_init(void *state, asx_actor_handle self) {
    rec_state *s = (rec_state *)state;
    (void)self;
    s->init_called++;
    return s->fail_init ? ASX_E_INVALID_STATE : ASX_OK;
}

static asx_status rec_cast(void *state, uint64_t msg, asx_actor_handle self) {
    rec_state *s = (rec_state *)state;
    (void)self;
    if (s->casts < 80u) s->log[s->casts] = msg;
    s->casts++;
    s->sum += msg;
    if (s->fail_cast_at != 0u && s->casts == s->fail_cast_at) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

static asx_status rec_call(void *state, uint64_t request, uint64_t *reply, asx_actor_handle self) {
    rec_state *s = (rec_state *)state;
    (void)self;
    s->calls++;
    *reply = s->sum + request;
    return ASX_OK;
}

static void rec_terminate(void *state, asx_status reason, asx_actor_handle self) {
    rec_state *s = (rec_state *)state;
    (void)self;
    s->term_called++;
    s->term_reason = reason;
}

static const asx_actor_behavior g_rec = {rec_init, rec_cast, rec_call, rec_terminate};

static asx_actor_handle spawn_rec(asx_region_id region, rec_state *state, uint32_t capacity) {
    asx_actor_handle h;
    memset(state, 0, sizeof(*state));
    h.slot = UINT32_MAX;
    h.generation = 0;
    MUST_OK(asx_actor_spawn(&h, region, &g_rec, state, capacity));
    return h;
}

/* ------------------------------------------------------------------ */
/* A client task running a short script of server operations          */
/* ------------------------------------------------------------------ */

typedef enum { CL_CAST, CL_CALL, CL_STOP_JOIN } cl_kind;

typedef struct {
    cl_kind kind;
    uint64_t arg;
    asx_status status; /* ASX_E_PENDING until the step returned */
    uint64_t reply;
} cl_step;

typedef struct {
    asx_actor_handle server;
    asx_cx cx;
    cl_step steps[8];
    uint32_t n;
    uint32_t pc;
    asx_actor_op op;
    int stop_sent;
} client;

static asx_status client_poll(void *user_data, asx_task_id self) {
    client *c = (client *)user_data;
    (void)self;
    while (c->pc < c->n) {
        cl_step *s = &c->steps[c->pc];
        asx_status st = ASX_E_INVALID_STATE;
        switch (s->kind) {
        case CL_CAST: st = asx_actor_cast(c->server, &c->cx, s->arg, &c->op); break;
        case CL_CALL: st = asx_actor_call(c->server, &c->cx, s->arg, &c->op, &s->reply); break;
        case CL_STOP_JOIN:
            if (!c->stop_sent) {
                MUST_OK(asx_actor_stop(c->server));
                c->stop_sent = 1;
            }
            st = asx_actor_join(c->server, &c->cx);
            break;
        }
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        s->status = st;
        c->pc++;
        memset(&c->op, 0, sizeof(c->op));
        c->stop_sent = 0;
    }
    return ASX_OK;
}

static void client_add(client *c, cl_kind kind, uint64_t arg) {
    c->steps[c->n].kind = kind;
    c->steps[c->n].arg = arg;
    c->steps[c->n].status = ASX_E_PENDING;
    c->steps[c->n].reply = 0;
    c->n++;
}

static void client_start(client *c, asx_actor_handle server, asx_region_id region) {
    asx_task_id id = ASX_INVALID_ID;
    c->server = server;
    c->pc = 0;
    memset(&c->op, 0, sizeof(c->op));
    c->stop_sent = 0;
    MUST_OK(asx_task_spawn(region, client_poll, c, &id));
    MUST_OK(asx_cx_init(&c->cx, region, id, ASX_CAP_CANCEL_CHECK));
}

/* ------------------------------------------------------------------ */
/* Spawn                                                               */
/* ------------------------------------------------------------------ */

static void test_spawn_basic(void) {
    rec_state s;
    asx_actor_handle h;
    asx_task_id tid = ASX_INVALID_ID;
    setup();
    h = spawn_rec(g_root, &s, 4);
    ASSERT(asx_actor_is_alive(h), "a spawned server is alive");
    ASSERT(asx_actor_get_state(h) == ASX_ACTOR_CREATED, "Created until first polled");
    ASSERT(asx_actor_task_id(h, &tid) == ASX_OK && tid != ASX_INVALID_ID, "it has a task");
    ASSERT(asx_actor_mailbox_count(h) == 0u, "empty mailbox");
    ASSERT(asx_actor_exit_reason(h) == ASX_OK, "no exit reason while alive");
    ASSERT(s.init_called == 0, "init waits for the first poll");
}

static void test_spawn_rejects_bad_arguments(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    ASSERT(asx_actor_spawn(NULL, g_root, &g_rec, &s, 4) == ASX_E_INVALID_ARGUMENT, "NULL out");
    ASSERT(asx_actor_spawn(&h, g_root, NULL, &s, 4) == ASX_E_INVALID_ARGUMENT, "NULL behavior");
    ASSERT(asx_actor_spawn(&h, g_root, &g_rec, &s, 0) == ASX_E_INVALID_ARGUMENT, "capacity 0");
    ASSERT(asx_actor_spawn(&h, g_root, &g_rec, &s, ASX_ACTOR_MAILBOX_CAPACITY + 1u) ==
               ASX_E_INVALID_ARGUMENT,
           "capacity above the maximum");
}

static void test_spawn_arena_exhaustion(void) {
    rec_state s;
    asx_actor_handle h;
    asx_status st = ASX_OK;
    uint32_t n = 0;
    setup();
    while (n <= ASX_MAX_ACTORS) {
        st = asx_actor_spawn(&h, g_root, &g_rec, &s, 1);
        if (st != ASX_OK) break;
        n++;
    }
    ASSERT(n >= 1u && n <= ASX_MAX_ACTORS, "at most ASX_MAX_ACTORS live servers");
    ASSERT(st == ASX_E_RESOURCE_EXHAUSTED, "a full arena refuses with RESOURCE_EXHAUSTED");
}

static void test_spawn_refused_by_region_limit(void) {
    rec_state s;
    asx_actor_handle h;
    asx_region_limits limits;
    setup();
    MUST_OK(asx_region_get_limits(g_root, &limits));
    limits.max_tasks = 0;
    MUST_OK(asx_region_set_limits(g_root, &limits));
    ASSERT(asx_actor_spawn(&h, g_root, &g_rec, &s, 2) == ASX_E_ADMISSION_LIMIT,
           "the region's task limit refuses the server");
    limits.max_tasks = ASX_REGION_UNLIMITED;
    MUST_OK(asx_region_set_limits(g_root, &limits));
    ASSERT(asx_actor_spawn(&h, g_root, &g_rec, &s, 2) == ASX_OK,
           "the refused spawn kept no actor slot or mailbox");
}

/* ------------------------------------------------------------------ */
/* Init and the message loop                                           */
/* ------------------------------------------------------------------ */

static void test_init_runs_once_on_first_poll(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 4);
    run(g_root);
    ASSERT(s.init_called == 1, "init ran on the first poll");
    ASSERT(asx_actor_get_state(h) == ASX_ACTOR_RUNNING, "Running while it waits");
    MUST_OK(asx_actor_try_cast(h, 1));
    run(g_root);
    ASSERT(s.init_called == 1, "init runs once");
    ASSERT(s.casts == 1u, "the cast woke the waiting server");
}

static void test_try_cast_delivers_in_order(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 4);
    MUST_OK(asx_actor_try_cast(h, 10));
    MUST_OK(asx_actor_try_cast(h, 20));
    MUST_OK(asx_actor_try_cast(h, 30));
    ASSERT(asx_actor_mailbox_count(h) == 3u, "three queued");
    run(g_root);
    ASSERT(s.casts == 3u, "all three handled");
    ASSERT(s.log[0] == 10u && s.log[1] == 20u && s.log[2] == 30u, "in arrival order");
    ASSERT(asx_actor_mailbox_count(h) == 0u, "mailbox empty again");
}

static void test_try_cast_reports_a_full_mailbox(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 2);
    MUST_OK(asx_actor_try_cast(h, 1));
    MUST_OK(asx_actor_try_cast(h, 2));
    ASSERT(asx_actor_try_cast(h, 3) == ASX_E_CHANNEL_FULL, "capacity 2 holds two");
    run(g_root);
    ASSERT(s.casts == 2u, "the queued two were handled");
    ASSERT(asx_actor_try_cast(h, 4) == ASX_OK, "room again");
}

static void test_ready_batch_yields_after_eight(void) {
    rec_state s;
    asx_actor_handle h;
    uint32_t i;
    setup();
    h = spawn_rec(g_root, &s, 16);
    for (i = 1; i <= 10u; i++) MUST_OK(asx_actor_try_cast(h, i));
    run_one_poll(g_root);
    ASSERT(s.casts == 8u, "one poll serves a batch of eight, then yields");
    ASSERT(asx_actor_mailbox_count(h) == 2u, "two left for the next poll");
    run(g_root);
    ASSERT(s.casts == 10u && s.sum == 55u, "the rest after the yield");
}

/* ------------------------------------------------------------------ */
/* Calls and casts from tasks                                          */
/* ------------------------------------------------------------------ */

static void test_call_from_task_gets_reply(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id cr;
    setup();
    cr = child_region();
    h = spawn_rec(g_root, &s, 4);
    MUST_OK(asx_actor_try_cast(h, 5));
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CALL, 37);
    client_start(&c, h, cr);
    run(g_root);
    ASSERT(c.steps[0].status == ASX_OK, "the call returned");
    ASSERT(c.steps[0].reply == 42u, "reply = casts so far (5) + request (37)");
    ASSERT(s.calls == 1u, "handled once");
}

static void test_call_from_root_region_is_rejected(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    setup();
    h = spawn_rec(g_root, &s, 4);
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CALL, 1);
    client_start(&c, h, g_root);
    run(g_root);
    ASSERT(c.steps[0].status == ASX_E_CANCELLED, "Rust's [ASUP-E103] rejection");
    ASSERT(s.calls == 0u, "nothing reached the server");
}

static void test_cast_from_task_waits_for_capacity(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id cr;
    setup();
    cr = child_region();
    h = spawn_rec(g_root, &s, 1);
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CAST, 1);
    client_add(&c, CL_CAST, 2);
    client_add(&c, CL_CAST, 4);
    client_add(&c, CL_CALL, 0);
    client_start(&c, h, cr);
    run(g_root);
    ASSERT(c.steps[0].status == ASX_OK && c.steps[1].status == ASX_OK &&
               c.steps[2].status == ASX_OK,
           "each cast waited for room and was sent");
    ASSERT(s.log[0] == 1u && s.log[1] == 2u && s.log[2] == 4u, "in order");
    ASSERT(c.steps[3].status == ASX_OK && c.steps[3].reply == 7u, "the call sees all three");
}

static void test_full_mailbox_of_64_parks_the_next_cast(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id cr;
    uint32_t i;
    int in_order = 1;
    setup();
    cr = child_region();
    h = spawn_rec(g_root, &s, 64);
    for (i = 0; i < 64u; i++) MUST_OK(asx_actor_try_cast(h, i));
    ASSERT(asx_actor_try_cast(h, 999) == ASX_E_CHANNEL_FULL, "the 65th try_cast is refused");
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CAST, 64);
    client_start(&c, h, cr);
    run(cr); /* the client's cast waits: the server has not run */
    ASSERT(c.steps[0].status == ASX_E_PENDING, "the 65th cast parks");
    run(g_root);
    ASSERT(c.steps[0].status == ASX_OK, "a receive woke it and it was sent");
    ASSERT(s.casts == 65u, "all 65 handled");
    for (i = 0; i < 65u; i++) in_order = in_order && s.log[i] == i;
    ASSERT(in_order, "in FIFO order, the parked cast last");
}

/* ------------------------------------------------------------------ */
/* Stop and join                                                       */
/* ------------------------------------------------------------------ */

static void test_stop_runs_terminate(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 4);
    run(g_root);
    MUST_OK(asx_actor_stop(h));
    ASSERT(asx_actor_get_state(h) == ASX_ACTOR_STOPPING, "Stopping once asked");
    run(g_root);
    ASSERT(s.term_called == 1 && s.term_reason == ASX_OK, "terminate ran with ASX_OK");
    ASSERT(!asx_actor_is_alive(h) && asx_actor_get_state(h) == ASX_ACTOR_STOPPED, "Stopped");
    ASSERT(asx_actor_exit_reason(h) == ASX_OK, "a stop is a normal exit");
    ASSERT(task_outcome(h) == ASX_OUTCOME_OK, "its task completed OK");
    MUST_OK(asx_actor_stop(h));
    ASSERT(asx_actor_get_state(h) == ASX_ACTOR_STOPPED, "a later stop does not revive it");
}

static void test_stop_before_first_poll_skips_init(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 4);
    MUST_OK(asx_actor_try_cast(h, 2));
    MUST_OK(asx_actor_try_cast(h, 3));
    MUST_OK(asx_actor_stop(h));
    run(g_root);
    ASSERT(s.init_called == 0, "a stop before start skips init");
    ASSERT(s.casts == 2u && s.sum == 5u, "the queued casts are still served");
    ASSERT(s.term_called == 1 && s.term_reason == ASX_OK, "then it terminates");
}

static void test_stop_succeeds_on_a_full_mailbox(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 2);
    run(g_root);
    MUST_OK(asx_actor_try_cast(h, 1));
    MUST_OK(asx_actor_try_cast(h, 2));
    ASSERT(asx_actor_try_cast(h, 3) == ASX_E_CHANNEL_FULL, "full");
    ASSERT(asx_actor_stop(h) == ASX_OK, "a stop is a state change, never a queued message");
    ASSERT(asx_actor_try_cast(h, 4) == ASX_E_DISCONNECTED, "sends after the stop fail");
    run(g_root);
    ASSERT(s.casts == 2u && s.sum == 3u, "the queued messages are still served");
    ASSERT(s.term_called == 1 && !asx_actor_is_alive(h), "then it stops");
}

static void test_messages_after_stop_are_refused(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id cr;
    setup();
    cr = child_region();
    h = spawn_rec(g_root, &s, 4);
    MUST_OK(asx_actor_stop(h));
    ASSERT(asx_actor_try_cast(h, 1) == ASX_E_DISCONNECTED, "try_cast to a stopping server");
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CAST, 1);
    client_add(&c, CL_CALL, 1);
    client_start(&c, h, cr);
    run(g_root);
    ASSERT(c.steps[0].status == ASX_E_DISCONNECTED, "cast_rejected_stopped");
    ASSERT(c.steps[1].status == ASX_E_DISCONNECTED, "call_rejected_stopped");
    ASSERT(s.casts == 0u && s.calls == 0u, "nothing reached the server");
}

static void test_join_waits_for_the_server(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id cr;
    setup();
    cr = child_region();
    h = spawn_rec(g_root, &s, 4);
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CAST, 5);
    client_add(&c, CL_STOP_JOIN, 0);
    client_start(&c, h, cr);
    run(g_root);
    ASSERT(c.steps[1].status == ASX_OK, "the join returned once the server finished");
    ASSERT(s.casts == 1u && s.term_called == 1, "it served the cast and terminated first");
    ASSERT(!asx_actor_is_alive(h), "finished");
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

static void cancel(asx_region_id region) {
    asx_cancel_reason r = asx_cancel_reason_default(ASX_CANCEL_USER, NULL);
    MUST_OK(asx_region_cancel(region, &r, NULL));
}

static void test_region_cancel_skips_queued_casts(void) {
    rec_state s;
    asx_actor_handle h;
    asx_region_id sr;
    setup();
    sr = child_region();
    h = spawn_rec(sr, &s, 4);
    MUST_OK(asx_actor_try_cast(h, 1));
    MUST_OK(asx_actor_try_cast(h, 2));
    cancel(sr);
    run(g_root);
    ASSERT(s.init_called == 0, "cancelled before its first poll: init skipped");
    ASSERT(s.casts == 0u, "a cancelled server drains its casts without handling them");
    ASSERT(s.term_called == 1 && s.term_reason == ASX_E_CANCELLED, "terminate still runs");
    ASSERT(asx_actor_exit_reason(h) == ASX_E_CANCELLED, "exit reason CANCELLED");
}

static void test_cancelled_server_drops_queued_call(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id sr;
    asx_region_id cr;
    setup();
    sr = child_region();
    cr = child_region();
    h = spawn_rec(sr, &s, 4);
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CALL, 1);
    client_start(&c, h, cr);
    run(cr); /* the call is enqueued; the server has not run */
    ASSERT(c.steps[0].status == ASX_E_PENDING, "waiting for the reply");
    cancel(sr);
    run(g_root);
    ASSERT(s.calls == 0u, "the call was drained, not handled");
    ASSERT(c.steps[0].status == ASX_E_INVALID_STATE, "the caller sees no reply (NoReply)");
    ASSERT(s.term_called == 1 && s.term_reason == ASX_E_CANCELLED, "terminated");
}

static void test_cancel_reaches_a_waiting_server(void) {
    rec_state s;
    asx_actor_handle h;
    asx_region_id sr;
    setup();
    sr = child_region();
    h = spawn_rec(sr, &s, 4);
    run(g_root);
    ASSERT(s.init_called == 1 && asx_actor_is_alive(h), "running, waiting for messages");
    cancel(sr);
    run(g_root);
    ASSERT(s.term_called == 1 && s.term_reason == ASX_E_CANCELLED,
           "the receive saw the cancel and the server terminated");
    ASSERT(task_outcome(h) == ASX_OUTCOME_CANCELLED,
           "a task spawned outside a poll is cancellation-dominant");
}

/* ------------------------------------------------------------------ */
/* Failing callbacks (Rust panics)                                     */
/* ------------------------------------------------------------------ */

static void test_failing_cast_panics_the_server(void) {
    rec_state s;
    asx_actor_handle h;
    client c;
    asx_region_id sr;
    asx_region_id cr;
    setup();
    sr = child_region();
    cr = child_region();
    h = spawn_rec(sr, &s, 4);
    s.fail_cast_at = 1;
    MUST_OK(asx_actor_try_cast(h, 1));
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CALL, 1);
    client_start(&c, h, cr);
    run(cr); /* the call queues behind the failing cast */
    run(g_root);
    ASSERT(task_outcome(h) == ASX_OUTCOME_PANICKED, "its task completed PANICKED");
    ASSERT(s.term_called == 0, "no terminate after a panic");
    ASSERT(asx_actor_exit_reason(h) == ASX_E_INVALID_STATE, "exit reason: the callback's");
    ASSERT(!asx_actor_is_alive(h), "stopped");
    ASSERT(s.calls == 0u && c.steps[0].status == ASX_E_INVALID_STATE,
           "the queued call's reply was aborted: no reply");
}

static void test_call_without_handler_panics_the_server(void) {
    rec_state s;
    asx_actor_handle h;
    asx_actor_behavior no_call = g_rec;
    client c;
    asx_region_id cr;
    setup();
    cr = child_region();
    no_call.handle_call = NULL;
    memset(&s, 0, sizeof(s));
    MUST_OK(asx_actor_spawn(&h, g_root, &no_call, &s, 4));
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CALL, 1);
    client_start(&c, h, cr);
    run(g_root);
    ASSERT(c.steps[0].status == ASX_E_INVALID_STATE, "the dropped reply: no reply");
    ASSERT(task_outcome(h) == ASX_OUTCOME_PANICKED, "Rust's reply drop bomb panics it");
}

static void test_failing_init_panics_the_server(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec(g_root, &s, 4);
    s.fail_init = 1;
    run(g_root);
    ASSERT(s.init_called == 1, "init ran");
    ASSERT(task_outcome(h) == ASX_OUTCOME_PANICKED, "PANICKED");
    ASSERT(s.term_called == 0, "no terminate");
}

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */

static void test_forged_and_reset_handles_are_rejected(void) {
    rec_state s;
    asx_actor_handle h;
    asx_actor_handle forged;
    setup();
    h = spawn_rec(g_root, &s, 4);
    forged = h;
    forged.generation++;
    ASSERT(asx_actor_try_cast(forged, 1) == ASX_E_INVALID_ARGUMENT, "wrong generation");
    ASSERT(asx_actor_stop(forged) == ASX_E_INVALID_ARGUMENT, "stop of a forged handle");
    asx_actor_reset();
    ASSERT(!asx_actor_is_alive(h), "reset forgets every server");
    ASSERT(asx_actor_try_cast(h, 1) == ASX_E_INVALID_ARGUMENT, "stale after reset");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_actor:\n");

    RUN(test_spawn_basic);
    RUN(test_spawn_rejects_bad_arguments);
    RUN(test_spawn_arena_exhaustion);
    RUN(test_spawn_refused_by_region_limit);

    RUN(test_init_runs_once_on_first_poll);
    RUN(test_try_cast_delivers_in_order);
    RUN(test_try_cast_reports_a_full_mailbox);
    RUN(test_ready_batch_yields_after_eight);

    RUN(test_call_from_task_gets_reply);
    RUN(test_call_from_root_region_is_rejected);
    RUN(test_cast_from_task_waits_for_capacity);
    RUN(test_full_mailbox_of_64_parks_the_next_cast);

    RUN(test_stop_runs_terminate);
    RUN(test_stop_before_first_poll_skips_init);
    RUN(test_stop_succeeds_on_a_full_mailbox);
    RUN(test_messages_after_stop_are_refused);
    RUN(test_join_waits_for_the_server);

    RUN(test_region_cancel_skips_queued_casts);
    RUN(test_cancelled_server_drops_queued_call);
    RUN(test_cancel_reaches_a_waiting_server);

    RUN(test_failing_cast_panics_the_server);
    RUN(test_call_without_handler_panics_the_server);
    RUN(test_failing_init_panics_the_server);

    RUN(test_forged_and_reset_handles_are_rejected);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
