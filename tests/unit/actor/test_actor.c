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
#include <asx/runtime/lab.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/time/sleep.h>
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

static const asx_actor_behavior g_rec = {
    rec_init, rec_cast, rec_call, rec_terminate, NULL, NULL, ASX_ACTOR_OVERFLOW_REJECT};

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
    MUST_OK(asx_cx_init(&c->cx, region, id, ASX_CAP_NONE));
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

typedef struct {
    asx_cx cx;
    asx_actor_handle server;
    rec_state state;
    asx_actor_op op;
    asx_sleep_state sleep;
    uint32_t phase;
    uint32_t polls;
    asx_status spawn_status;
    asx_status call_status;
} refused_call_client;

static asx_lab g_refused_lab;

static asx_status refused_call_poll(void *user_data, asx_task_id self) {
    refused_call_client *c = (refused_call_client *)user_data;
    asx_status st;
    uint64_t reply = 0;

    c->polls++;
    if (c->phase == 0u) {
        st = asx_sleep_poll(&c->sleep, self);
        if (st != ASX_OK) return st;
        c->phase = 1u;
    }
    if (c->phase == 1u) {
        c->spawn_status = asx_actor_spawn(&c->server, c->cx.region_id, &g_rec, &c->state, 1u);
        if (c->spawn_status != ASX_OK) return c->spawn_status;
        c->phase = 2u;
    }
    if (c->phase == 2u) {
        st = asx_actor_call(c->server, &c->cx, 49u, &c->op, &reply);
        if (st == ASX_E_PENDING) return st;
        c->call_status = st;
        st = asx_sleep_init(&c->sleep, 150u);
        if (st != ASX_OK) return st;
        c->phase = 3u;
    }
    return asx_sleep_poll(&c->sleep, self);
}

/* Real minimized Rust witness gen-1238-119-min: a normal body return
 * closes admission without cancelling its caller. Dropping the refused
 * server must wake the reply before that caller resumes, so the reply
 * cannot leave an in-poll wake queued behind its following sleep. */
static void test_refused_server_reply_does_not_repoll_following_sleep(void) {
    refused_call_client c;
    asx_region_id region;
    asx_task_id task = ASX_INVALID_ID;
    asx_dispatch_record dispatches[8];
    asx_budget budget;
    asx_outcome body = {ASX_OUTCOME_OK};
    asx_outcome outcome;
    asx_lab_config config;
    uint32_t i;

    setup();
    asx_lab_config_init(&config);
    config.seed = 502117u;
    ASSERT(asx_lab_init(&g_refused_lab, &config) == ASX_OK, "real virtual-clock runtime");
    ASSERT(asx_region_open(&g_root) == ASX_OK, "root in the initialized lab");
    region = child_region();
    ASSERT(asx_scheduler_use_lab_dispatch(config.seed) == ASX_OK, "the real witness dispatch seed");
    memset(&c, 0, sizeof(c));
    c.spawn_status = ASX_E_PENDING;
    c.call_status = ASX_E_PENDING;
    ASSERT(asx_sleep_init(&c.sleep, 150u) == ASX_OK, "first genuine sleep");
    ASSERT(asx_task_spawn(region, refused_call_poll, &c, &task) == ASX_OK, "live caller");
    ASSERT(asx_cx_init(&c.cx, region, task, ASX_CAP_NONE) == ASX_OK, "caller's context");
    memset(dispatches, 0, sizeof(dispatches));
    asx_scheduler_record_dispatches(dispatches, 8u);
    budget = asx_budget_from_polls(100u);
    ASSERT(asx_scheduler_run_until_idle(g_root, &budget) == ASX_E_PENDING,
           "caller sleeps at time zero");
    ASSERT(c.polls == 1u, "exactly the initial sleep poll");
    asx_runtime_virtual_advance(250u);
    ASSERT(asx_lab_advance_to_next_timer(&g_refused_lab, NULL) == ASX_OK,
           "the actual first timer wakes the caller at the driver time");
    ASSERT(asx_region_close_with_outcome(region, &body, NULL) == ASX_OK,
           "normal body return closes without cancellation");
    budget = asx_budget_from_polls(100u);
    ASSERT(asx_scheduler_run_until_idle(g_root, &budget) == ASX_E_PENDING,
           "caller waits for its following sleep");
    ASSERT(c.spawn_status == ASX_OK && c.call_status == ASX_E_INVALID_STATE,
           "refused server's queued reply was genuinely aborted");
    ASSERT(c.polls == 3u && asx_scheduler_dispatches_recorded() == 3u,
           "reply cleanup leaves no extra caller poll at time 250");
    ASSERT(asx_actor_get_state(c.server) == ASX_ACTOR_STOPPED &&
               asx_actor_mailbox_count(c.server) == 0u,
           "refusal dropped the real server cell and its queued envelope");
    ASSERT(c.state.init_called == 0 && c.state.calls == 0u && c.state.term_called == 0,
           "a refused server never runs user callbacks");
    ASSERT(asx_actor_join(c.server, &c.cx) == ASX_E_CANCELLED &&
               asx_actor_join(c.server, &c.cx) == ASX_E_CANCELLED,
           "repeated lookup and join preserve the refused outcome");
    budget = asx_budget_from_polls(100u);
    ASSERT(asx_scheduler_run(g_root, &budget) == ASX_OK, "the actual timer completes the caller");
    ASSERT(c.polls == 4u && asx_scheduler_dispatches_recorded() == 4u,
           "only four dispatches, as captured from Rust");
    ASSERT(dispatches[0].at == 0u && dispatches[1].at == 250u && dispatches[2].at == 250u &&
               dispatches[3].at == 400u,
           "captured dispatch times 0,250,250,400");
    for (i = 0; i < 4u; i++) {
        ASSERT(asx_handle_type_tag(dispatches[i].task) == ASX_TYPE_TASK &&
                   asx_handle_index(dispatches[i].task) == asx_handle_index(task) &&
                   dispatches[i].lane == ASX_DISPATCH_LANE_READY &&
                   dispatches[i].step == (uint64_t)(i + 1u),
               "every numbered ready dispatch belongs to the caller's live generation");
    }
    ASSERT(asx_task_get_outcome(task, &outcome) == ASX_OK && outcome.severity == ASX_OUTCOME_OK,
           "body close did not cancel the caller");
    budget = asx_budget_from_polls(100u);
    ASSERT(asx_region_wait_closed(region, &budget) == ASX_OK && asx_obligation_leak_count() == 0u,
           "real reply settlement permits a leak-free close");
    asx_lab_shutdown(&g_refused_lab);
}

typedef struct {
    asx_region_id region;
    asx_actor_handle server;
    rec_state state;
    uint32_t polls;
    asx_status spawn_status;
} refused_spawn_client;

static asx_status refused_spawn_poll(void *user_data, asx_task_id self) {
    refused_spawn_client *c = (refused_spawn_client *)user_data;
    (void)self;
    c->polls++;
    if (c->polls == 1u) return ASX_E_PENDING;
    c->spawn_status = asx_actor_spawn(&c->server, c->region, &g_rec, &c->state, 1u);
    return c->spawn_status;
}

/* A completed caller leaves a real refused admission for the next lab
 * step. There is no live task after the host consumes its outcome, but
 * that step still owns the server's captured cell and its cleanup. */
static void test_reseed_refuses_pending_actor_admission_then_cleanup_completes(void) {
    refused_spawn_client c;
    asx_task_id task = ASX_INVALID_ID;
    asx_budget budget;
    asx_outcome body = {ASX_OUTCOME_OK};
    asx_outcome outcome;
    asx_lab_config config;

    setup();
    asx_lab_config_init(&config);
    config.seed = 42u;
    ASSERT(asx_lab_init(&g_refused_lab, &config) == ASX_OK, "real lab runtime");
    ASSERT(asx_region_open(&g_root) == ASX_OK, "root after lab initialization");
    ASSERT(asx_scheduler_use_lab_dispatch(42u) == ASX_OK, "initial dispatch admission");
    memset(&c, 0, sizeof(c));
    c.region = child_region();
    c.spawn_status = ASX_E_PENDING;
    ASSERT(asx_task_spawn(c.region, refused_spawn_poll, &c, &task) == ASX_OK, "real caller");
    budget = asx_budget_from_polls(1u);
    ASSERT(asx_scheduler_run_until_idle(g_root, &budget) == ASX_E_POLL_BUDGET_EXHAUSTED &&
               c.polls == 1u,
           "one actual caller poll before body close");
    ASSERT(asx_region_close_with_outcome(c.region, &body, NULL) == ASX_OK,
           "normal body closes admission without cancelling caller");
    budget = asx_budget_from_polls(1u);
    ASSERT(asx_scheduler_run_until_idle(g_root, &budget) == ASX_E_POLL_BUDGET_EXHAUSTED &&
               c.polls == 2u && c.spawn_status == ASX_OK,
           "completed caller genuinely creates a refused server");
    ASSERT(asx_task_join(task, &outcome) == ASX_OK && outcome.severity == ASX_OUTCOME_OK,
           "host consumes the last live task");
    ASSERT(asx_actor_get_state(c.server) == ASX_ACTOR_CREATED &&
               asx_actor_join(c.server, NULL) == ASX_E_PENDING,
           "server cleanup still awaits real admission delivery");
    ASSERT(asx_scheduler_use_lab_dispatch(43u) == ASX_E_INVALID_STATE,
           "reseed cannot erase a pending captured actor cell");
    ASSERT(asx_actor_get_state(c.server) == ASX_ACTOR_CREATED &&
               asx_actor_join(c.server, NULL) == ASX_E_PENDING,
           "refused reseed preserves the queued lifecycle");
    budget = asx_budget_from_polls(100u);
    ASSERT(asx_scheduler_run_until_idle(g_root, &budget) == ASX_OK,
           "ordinary next lab step delivers the refusal");
    ASSERT(asx_actor_get_state(c.server) == ASX_ACTOR_STOPPED &&
               asx_actor_join(c.server, NULL) == ASX_E_CANCELLED,
           "actual admission cleanup resolves the join");
    ASSERT(c.state.init_called == 0 && c.state.calls == 0u && c.state.term_called == 0,
           "refused server runs no user callbacks");
    ASSERT(asx_scheduler_use_lab_dispatch(43u) == ASX_OK,
           "reseed succeeds after all pending admissions settle");
    asx_lab_shutdown(&g_refused_lab);
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

/* ------------------------------------------------------------------ */
/* Names (cx/registry.rs, gen_server.rs spawn_named_gen_server)        */
/* ------------------------------------------------------------------ */

/* A task id that no server or client uses: registry tests need only a
 * holder identity, as Rust's tests use TaskId::new_for_test. */
static asx_task_id fake_task(uint32_t n) { return (asx_task_id)(0x7000u + n); }

static void test_register_and_whereis(void) {
    asx_name_registry reg;
    asx_name_lease lease;
    asx_region_id r;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    ASSERT(asx_name_registry_init(&reg) == ASX_OK, "init");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_E_NOT_FOUND, "nothing registered");
    ASSERT(asx_name_register(&reg, "svc", fake_task(1), r, 5u, &lease, NULL) == ASX_OK,
           "a free name registers");
    ASSERT(asx_name_lease_is_active(&lease), "its lease is active");
    ASSERT(strcmp(lease.name, "svc") == 0 && lease.holder == fake_task(1) && lease.region == r &&
               lease.acquired_at == 5u,
           "and records the holder, region and time");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_OK && holder == fake_task(1),
           "whereis finds the holder");
    ASSERT(asx_name_registry_count(&reg) == 1u, "one entry");
}

static void test_taken_name_is_refused_and_changes_nothing(void) {
    asx_name_registry reg;
    asx_name_lease first;
    asx_name_lease second;
    asx_region_id r;
    asx_task_id current = ASX_INVALID_ID;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    ASSERT(asx_name_register(&reg, "svc", fake_task(1), r, 0u, &first, NULL) == ASX_OK, "first");
    ASSERT(asx_name_register(&reg, "svc", fake_task(2), r, 9u, &second, &current) ==
               ASX_E_NAME_CONFLICT,
           "a second registration of the name is refused (NameTaken)");
    ASSERT(current == fake_task(1), "with the current holder");
    ASSERT(!asx_name_lease_is_active(&second), "no lease was minted");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_OK && holder == fake_task(1),
           "the entry is the first holder's");
    ASSERT(asx_name_registry_count(&reg) == 1u, "and there is still one");
}

static void test_unregister_checks_the_caller_and_the_lease_identity(void) {
    asx_name_registry reg;
    asx_name_lease lease;
    asx_name_lease forged;
    asx_region_id r;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_name_register(&reg, "svc", fake_task(1), r, 3u, &lease, NULL));

    ASSERT(asx_name_unregister(&reg, "nope", fake_task(1)) == ASX_E_NOT_FOUND, "unknown name");
    ASSERT(asx_name_unregister(&reg, "svc", fake_task(2)) == ASX_E_PERMISSION_DENIED,
           "only the holder unregisters");
    forged = lease;
    forged.acquired_at = 4u;
    ASSERT(asx_name_unregister_owned(&reg, &forged, 0u) == ASX_E_PERMISSION_DENIED,
           "a lease with another identity frees nothing");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_OK, "the entry survived both");

    ASSERT(asx_name_unregister_owned(&reg, &lease, 0u) == ASX_OK, "the lease's owner frees it");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_E_NOT_FOUND, "the name is free");
    ASSERT(asx_name_unregister_owned(&reg, &lease, 0u) == ASX_E_NOT_FOUND, "once");
    ASSERT(asx_name_unregister(&reg, "svc", fake_task(1)) == ASX_E_NOT_FOUND, "and by name too");
}

static void test_a_lease_resolves_once_and_leaves_the_registry_alone(void) {
    asx_name_registry reg;
    asx_name_lease lease;
    asx_region_id r;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_name_register(&reg, "svc", fake_task(1), r, 0u, &lease, NULL));
    ASSERT(asx_name_lease_release(&lease) == ASX_OK, "release commits");
    ASSERT(!asx_name_lease_is_active(&lease), "and resolves");
    ASSERT(asx_name_lease_release(&lease) == ASX_E_OBLIGATION_ALREADY_RESOLVED, "once");
    ASSERT(asx_name_lease_abort(&lease) == ASX_E_OBLIGATION_ALREADY_RESOLVED, "abort too");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_OK,
           "resolving the lease does not free the name (registry.rs:192)");
}

static void test_root_region_holder_and_full_registry_are_refused(void) {
    asx_name_registry reg;
    asx_name_lease lease;
    asx_region_id r;
    char name[8];
    uint32_t i;
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    ASSERT(asx_name_register(&reg, "svc", fake_task(1), g_root, 0u, &lease, NULL) ==
               ASX_E_INVALID_ARGUMENT,
           "a root-region holder cannot mint a lease ([ASUP-E103])");
    ASSERT(asx_name_registry_count(&reg) == 0u, "and the registry is unchanged");

    for (i = 0; i < ASX_NAME_REGISTRY_CAPACITY; i++) {
        (void)snprintf(name, sizeof(name), "n%u", (unsigned)i);
        ASSERT(asx_name_register(&reg, name, fake_task(i), r, 0u, &lease, NULL) == ASX_OK,
               "names up to the capacity register");
    }
    ASSERT(asx_name_register(&reg, "extra", fake_task(99), r, 0u, &lease, NULL) ==
               ASX_E_RESOURCE_EXHAUSTED,
           "the next is refused for want of room");
    ASSERT(asx_name_unregister(&reg, "n0", fake_task(0)) == ASX_OK, "freeing one");
    ASSERT(asx_name_register(&reg, "extra", fake_task(99), r, 0u, &lease, NULL) == ASX_OK,
           "makes room");
}

static void test_name_arguments_are_checked(void) {
    asx_name_registry reg;
    asx_name_lease lease;
    asx_region_id r;
    char long_name[ASX_NAME_MAX_LEN + 2u];
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1u] = '\0';
    ASSERT(asx_name_register(&reg, long_name, fake_task(1), r, 0u, &lease, NULL) ==
               ASX_E_INVALID_ARGUMENT,
           "a name over the maximum length");
    long_name[ASX_NAME_MAX_LEN] = '\0';
    ASSERT(asx_name_register(&reg, long_name, fake_task(1), r, 0u, &lease, NULL) == ASX_OK,
           "a name of exactly the maximum length");
    ASSERT(asx_name_register(NULL, "a", fake_task(1), r, 0u, &lease, NULL) ==
               ASX_E_INVALID_ARGUMENT,
           "NULL registry");
    ASSERT(asx_name_register(&reg, NULL, fake_task(1), r, 0u, &lease, NULL) ==
               ASX_E_INVALID_ARGUMENT,
           "NULL name");
    ASSERT(asx_name_registry_init(NULL) == ASX_E_INVALID_ARGUMENT, "NULL init");
}

static void test_cleanup_drops_a_regions_and_a_tasks_names(void) {
    asx_name_registry reg;
    asx_name_lease lease;
    asx_region_id a;
    asx_region_id b;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    a = child_region();
    b = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_name_register(&reg, "a1", fake_task(1), a, 0u, &lease, NULL));
    MUST_OK(asx_name_register(&reg, "a2", fake_task(2), a, 0u, &lease, NULL));
    MUST_OK(asx_name_register(&reg, "b1", fake_task(3), b, 0u, &lease, NULL));
    MUST_OK(asx_name_register(&reg, "b2", fake_task(3), b, 0u, &lease, NULL));
    ASSERT(asx_name_cleanup_region(&reg, a) == 2u, "region a held two names");
    ASSERT(asx_name_whereis(&reg, "a1", &holder) == ASX_E_NOT_FOUND, "gone");
    ASSERT(asx_name_whereis(&reg, "b1", &holder) == ASX_OK, "region b's stay");
    ASSERT(asx_name_cleanup_task(&reg, fake_task(3)) == 2u, "task 3 held two names");
    ASSERT(asx_name_registry_count(&reg) == 0u, "the registry is empty");
    ASSERT(asx_name_cleanup_task(&reg, fake_task(3)) == 0u, "nothing left to drop");
}

/* spawn_named_gen_server: the name belongs to the server's task. */
static void test_named_server_registers_its_task(void) {
    asx_name_registry reg;
    asx_named_actor named;
    rec_state s;
    asx_region_id r;
    asx_task_id tid = ASX_INVALID_ID;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    memset(&s, 0, sizeof(s));
    MUST_OK(asx_name_registry_init(&reg));
    ASSERT(asx_actor_spawn_named(&named, &reg, "counter", r, &g_rec, &s, 4u, 7u) == ASX_OK,
           "a named server spawns");
    ASSERT(asx_actor_task_id(named.actor, &tid) == ASX_OK, "it has a task");
    ASSERT(asx_name_whereis(&reg, "counter", &holder) == ASX_OK && holder == tid,
           "whereis names the server's task before it has run");
    ASSERT(named.lease.holder == tid && named.lease.region == r && named.lease.acquired_at == 7u,
           "the lease is the server's");
    run(r);
    MUST_OK(asx_actor_try_cast(named.actor, 4u));
    run(r);
    ASSERT(s.casts == 1u && s.sum == 4u, "the server serves its mailbox as any other");
}

/* How many more servers a region takes before an arena (actors, channels,
 * tasks) refuses one. */
static uint32_t spawnable_servers(asx_region_id region) {
    static rec_state sink;
    asx_actor_handle h;
    uint32_t n = 0u;
    memset(&sink, 0, sizeof(sink));
    while (n <= ASX_MAX_ACTORS && asx_actor_spawn(&h, region, &g_rec, &sink, 1u) == ASX_OK) n++;
    return n;
}

static void test_a_taken_name_spawns_no_server(void) {
    asx_name_registry reg;
    asx_named_actor first;
    asx_named_actor second;
    rec_state s1;
    rec_state s2;
    asx_region_id r;
    asx_task_id holder = ASX_INVALID_ID;
    asx_task_id first_task = ASX_INVALID_ID;
    uint32_t spawnable_without_conflict;
    uint32_t spawnable_after_conflict;
    memset(&s1, 0, sizeof(s1));
    memset(&s2, 0, sizeof(s2));

    /* The baseline: one named server, nothing else tried. */
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_actor_spawn_named(&first, &reg, "svc", r, &g_rec, &s1, 2u, 0u));
    spawnable_without_conflict = spawnable_servers(r);

    /* The same, with a refused second spawn of the name in between. */
    setup();
    r = child_region();
    MUST_OK(asx_name_registry_init(&reg));
    ASSERT(asx_actor_spawn_named(&first, &reg, "svc", r, &g_rec, &s1, 2u, 0u) == ASX_OK, "first");
    MUST_OK(asx_actor_task_id(first.actor, &first_task));
    ASSERT(asx_actor_spawn_named(&second, &reg, "svc", r, &g_rec, &s2, 2u, 1u) ==
               ASX_E_NAME_CONFLICT,
           "the same name again is refused");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_OK && holder == first_task,
           "the first server keeps it");
    spawnable_after_conflict = spawnable_servers(r);
    ASSERT(spawnable_after_conflict == spawnable_without_conflict,
           "the refused spawn took no actor slot, channel or task");
    ASSERT(spawnable_without_conflict >= 1u, "and the arenas held something to compare");
}

static void test_release_name_waits_for_the_server_to_finish(void) {
    asx_name_registry reg;
    asx_named_actor named;
    rec_state s;
    asx_region_id r;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    memset(&s, 0, sizeof(s));
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_actor_spawn_named(&named, &reg, "svc", r, &g_rec, &s, 2u, 0u));
    run(r);
    ASSERT(asx_named_actor_release_name(&named, &reg, 1u) == ASX_E_TASKS_STILL_ACTIVE,
           "a running server keeps its name (StillRunning)");
    ASSERT(asx_name_lease_is_active(&named.lease), "and its lease stays active");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_OK, "still registered");

    MUST_OK(asx_actor_stop(named.actor));
    run(r);
    ASSERT(!asx_actor_is_alive(named.actor), "the server finished");
    ASSERT(asx_named_actor_release_name(&named, &reg, 2u) == ASX_OK, "now the name is released");
    ASSERT(!asx_name_lease_is_active(&named.lease), "the lease is committed");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_E_NOT_FOUND, "and the name is free");
    ASSERT(asx_named_actor_release_name(&named, &reg, 3u) == ASX_E_OBLIGATION_ALREADY_RESOLVED,
           "a second release finds the lease resolved");
}

static void test_abort_lease_frees_the_name_without_stopping_the_server(void) {
    asx_name_registry reg;
    asx_named_actor named;
    asx_named_actor again;
    rec_state s;
    rec_state s2;
    asx_region_id r;
    asx_task_id holder = ASX_INVALID_ID;
    setup();
    r = child_region();
    memset(&s, 0, sizeof(s));
    memset(&s2, 0, sizeof(s2));
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_actor_spawn_named(&named, &reg, "svc", r, &g_rec, &s, 2u, 0u));
    run(r);
    ASSERT(asx_named_actor_abort_lease(&named, &reg, 1u) == ASX_OK, "abort needs no stop");
    ASSERT(asx_name_whereis(&reg, "svc", &holder) == ASX_E_NOT_FOUND, "the name is free");
    ASSERT(asx_actor_is_alive(named.actor), "the server runs on");
    ASSERT(asx_named_actor_abort_lease(&named, &reg, 2u) == ASX_E_OBLIGATION_ALREADY_RESOLVED,
           "once");
    ASSERT(asx_actor_spawn_named(&again, &reg, "svc", r, &g_rec, &s2, 2u, 3u) == ASX_OK,
           "the freed name can be taken by another server");
}

static void test_release_resolves_the_lease_when_the_entry_is_gone(void) {
    asx_name_registry reg;
    asx_named_actor named;
    rec_state s;
    asx_region_id r;
    asx_task_id tid = ASX_INVALID_ID;
    setup();
    r = child_region();
    memset(&s, 0, sizeof(s));
    MUST_OK(asx_name_registry_init(&reg));
    MUST_OK(asx_actor_spawn_named(&named, &reg, "svc", r, &g_rec, &s, 2u, 0u));
    MUST_OK(asx_actor_task_id(named.actor, &tid));
    ASSERT(asx_name_cleanup_task(&reg, tid) == 1u, "something dropped the entry");
    MUST_OK(asx_actor_stop(named.actor));
    run(r);
    ASSERT(asx_named_actor_release_name(&named, &reg, 1u) == ASX_E_NOT_FOUND,
           "the registry no longer holds the entry: NotFound");
    ASSERT(!asx_name_lease_is_active(&named.lease),
           "the lease is aborted all the same, never left armed (:2693)");
}

/* ------------------------------------------------------------------ */
/* CastOverflowPolicy::DropOldest (gen_server.rs:1305-1360)            */
/* ------------------------------------------------------------------ */

static asx_actor_handle spawn_rec_policy(asx_region_id region, rec_state *state, uint32_t capacity,
                                         asx_actor_overflow_policy policy) {
    asx_actor_behavior behavior = g_rec;
    asx_actor_handle h;
    behavior.cast_overflow_policy = policy;
    memset(state, 0, sizeof(*state));
    h.slot = UINT32_MAX;
    h.generation = 0;
    MUST_OK(asx_actor_spawn(&h, region, &behavior, state, capacity));
    return h;
}

static void test_drop_oldest_evicts_the_oldest_cast_to_make_room(void) {
    rec_state s;
    asx_actor_handle h;
    uint64_t users_before;
    setup();
    h = spawn_rec_policy(g_root, &s, 2, ASX_ACTOR_OVERFLOW_DROP_OLDEST);
    users_before = asx_trace_kind_total(ASX_TRACE_USER);
    ASSERT(asx_actor_try_cast(h, 1) == ASX_OK && asx_actor_try_cast(h, 2) == ASX_OK, "room");
    ASSERT(asx_actor_evicted_count(h) == 0u, "nothing evicted while there is room");
    ASSERT(asx_actor_try_cast(h, 3) == ASX_OK, "a full mailbox takes the new cast");
    ASSERT(asx_actor_evicted_count(h) == 1u, "by evicting one");
    ASSERT(asx_actor_try_cast(h, 4) == ASX_OK, "again");
    ASSERT(asx_actor_evicted_count(h) == 2u, "the second eviction is counted");
    ASSERT(asx_actor_mailbox_count(h) == 2u, "the mailbox stays at its capacity");
    ASSERT(asx_trace_kind_total(ASX_TRACE_USER) == users_before,
           "from the host there is no task to attribute the eviction to");
    run(g_root);
    ASSERT(s.casts == 2u && s.log[0] == 3u && s.log[1] == 4u,
           "the server serves the two newest casts, in order");
    ASSERT(s.sum == 7u, "the evicted casts never reached the handler");
}

static void test_reject_policy_never_evicts(void) {
    rec_state s;
    asx_actor_handle h;
    setup();
    h = spawn_rec_policy(g_root, &s, 1, ASX_ACTOR_OVERFLOW_REJECT);
    ASSERT(asx_actor_try_cast(h, 1) == ASX_OK, "room");
    ASSERT(asx_actor_try_cast(h, 2) == ASX_E_CHANNEL_FULL, "REJECT refuses a cast when full");
    ASSERT(asx_actor_evicted_count(h) == 0u, "nothing was evicted");
    run(g_root);
    ASSERT(s.casts == 1u && s.log[0] == 1u, "the queued cast is the one served");
}

/* An evicting try_cast from a task traces on that task (Cx::current(),
 * :1347), once per eviction. */
typedef struct {
    asx_actor_handle server;
    asx_status status[3];
} evictor;

static asx_status evictor_poll(void *user_data, asx_task_id self) {
    evictor *e = (evictor *)user_data;
    uint32_t i;
    (void)self;
    for (i = 0; i < 3u; i++) e->status[i] = asx_actor_try_cast(e->server, i + 1u);
    return ASX_OK;
}

static void test_drop_oldest_traces_the_eviction_on_the_calling_task(void) {
    rec_state s;
    asx_actor_handle h;
    evictor e;
    asx_task_id tid = ASX_INVALID_ID;
    uint64_t users_before;
    setup();
    memset(&e, 0, sizeof(e));
    /* The evictor is spawned first, so the one poll below is its own. */
    MUST_OK(asx_task_spawn(g_root, evictor_poll, &e, &tid));
    h = spawn_rec_policy(g_root, &s, 2, ASX_ACTOR_OVERFLOW_DROP_OLDEST);
    e.server = h;
    users_before = asx_trace_kind_total(ASX_TRACE_USER);
    run_one_poll(g_root);
    ASSERT(e.status[0] == ASX_OK && e.status[1] == ASX_OK && e.status[2] == ASX_OK,
           "all three casts were accepted");
    ASSERT(asx_actor_evicted_count(h) == 1u, "the third evicted the first");
    ASSERT(asx_trace_kind_total(ASX_TRACE_USER) == users_before + 1u,
           "one gen_server::cast_evicted_oldest trace, on the task");
}

/* A queued call is never evicted: only casts are (:1321, :5322 test). */
static void test_drop_oldest_never_evicts_a_queued_call(void) {
    rec_state s;
    asx_actor_handle h;
    asx_actor_handle placeholder;
    client c;
    asx_region_id r;
    setup();
    r = child_region();
    placeholder.slot = 0u;
    placeholder.generation = 0u;
    memset(&c, 0, sizeof(c));
    client_add(&c, CL_CALL, 5u);
    /* The client is spawned first: the single poll below is its, and the
     * server has not run when the casts arrive. */
    client_start(&c, placeholder, r);
    h = spawn_rec_policy(r, &s, 2, ASX_ACTOR_OVERFLOW_DROP_OLDEST);
    c.server = h;
    run_one_poll(r);
    ASSERT(s.init_called == 0, "the server has not run");
    ASSERT(asx_actor_mailbox_count(h) == 1u, "the call is queued");

    ASSERT(asx_actor_try_cast(h, 1) == ASX_OK, "a cast fits beside the call");
    ASSERT(asx_actor_try_cast(h, 2) == ASX_OK, "full: the oldest cast is evicted, not the call");
    ASSERT(asx_actor_evicted_count(h) == 1u, "one eviction");
    ASSERT(asx_actor_mailbox_count(h) == 2u, "the call and the newest cast are queued");

    run(r);
    ASSERT(c.steps[0].status == ASX_OK && c.steps[0].reply == 5u,
           "the call was served: its reply obligation was never dropped");
    ASSERT(s.calls == 1u, "the server handled the call");
    ASSERT(s.casts == 1u && s.log[0] == 2u, "and the newest cast, not the evicted one");
}

static void test_drop_oldest_with_only_calls_queued_is_full(void) {
    rec_state s;
    asx_actor_handle h;
    asx_actor_handle placeholder;
    client c1;
    client c2;
    asx_region_id r;
    setup();
    r = child_region();
    placeholder.slot = 0u;
    placeholder.generation = 0u;
    memset(&c1, 0, sizeof(c1));
    memset(&c2, 0, sizeof(c2));
    client_add(&c1, CL_CALL, 10u);
    client_add(&c2, CL_CALL, 20u);
    client_start(&c1, placeholder, r);
    client_start(&c2, placeholder, r);
    h = spawn_rec_policy(r, &s, 2, ASX_ACTOR_OVERFLOW_DROP_OLDEST);
    c1.server = h;
    c2.server = h;
    run_one_poll(r);
    run_one_poll(r);
    ASSERT(asx_actor_mailbox_count(h) == 2u, "two calls fill the mailbox");

    ASSERT(asx_actor_try_cast(h, 1) == ASX_E_CHANNEL_FULL,
           "no queued cast is evictable: the cast is refused");
    ASSERT(asx_actor_evicted_count(h) == 0u, "nothing was evicted");
    ASSERT(asx_actor_mailbox_count(h) == 2u, "both calls are still queued");

    run(r);
    ASSERT(c1.steps[0].status == ASX_OK && c1.steps[0].reply == 10u, "the first call is served");
    ASSERT(c2.steps[0].status == ASX_OK && c2.steps[0].reply == 20u, "and the second");
    ASSERT(s.casts == 0u && s.calls == 2u, "the refused cast never ran");
}

/* ------------------------------------------------------------------ */
/* Phase budgets (PhaseBudgetGuard, gen_server.rs:96-160)              */
/* ------------------------------------------------------------------ */

/* A server that records the task budget in each phase. The budget
 * callbacks see only the user state, so it holds the task id and reads the
 * budget just before the phase installs its own. */
typedef struct {
    asx_task_id task;
    asx_budget start_phase;  /* what on_start_budget returns */
    asx_budget stop_phase;   /* what on_stop_budget returns */
    uint64_t init_cost;      /* cost units init consumes */
    asx_budget before_start; /* the task's budget when init's phase begins */
    asx_budget at_start;     /* inside init */
    asx_budget after_init;   /* inside the first cast */
    asx_budget before_stop;  /* when the stop phase begins */
    asx_budget at_stop;      /* inside terminate */
    int start_budget_calls;
    int stop_budget_calls;
    int init_calls;
    int casts;
    int term_calls;
} bud_state;

static void bud_read(asx_task_id task, asx_budget *out) { MUST_OK(asx_task_get_budget(task, out)); }

static asx_budget bud_start_budget(void *state) {
    bud_state *b = (bud_state *)state;
    b->start_budget_calls++;
    bud_read(b->task, &b->before_start);
    return b->start_phase;
}

static asx_budget bud_stop_budget(void *state) {
    bud_state *b = (bud_state *)state;
    b->stop_budget_calls++;
    bud_read(b->task, &b->before_stop);
    return b->stop_phase;
}

static asx_status bud_init(void *state, asx_actor_handle self) {
    bud_state *b = (bud_state *)state;
    (void)self;
    b->init_calls++;
    bud_read(b->task, &b->at_start);
    if (b->init_cost > 0u) MUST_OK(asx_task_consume_cost(b->task, b->init_cost));
    return ASX_OK;
}

static asx_status bud_cast(void *state, uint64_t msg, asx_actor_handle self) {
    bud_state *b = (bud_state *)state;
    (void)msg;
    (void)self;
    if (b->casts++ == 0) bud_read(b->task, &b->after_init);
    return ASX_OK;
}

static void bud_terminate(void *state, asx_status reason, asx_actor_handle self) {
    bud_state *b = (bud_state *)state;
    (void)reason;
    (void)self;
    b->term_calls++;
    bud_read(b->task, &b->at_stop);
}

static const asx_actor_behavior g_bud = {bud_init,
                                         bud_cast,
                                         NULL,
                                         bud_terminate,
                                         bud_start_budget,
                                         bud_stop_budget,
                                         ASX_ACTOR_OVERFLOW_REJECT};
static const asx_actor_behavior g_bud_default = {
    bud_init, bud_cast, NULL, bud_terminate, NULL, NULL, ASX_ACTOR_OVERFLOW_REJECT};

static asx_region_id budgeted_region(const asx_budget *budget) {
    asx_region_id r = ASX_INVALID_ID;
    MUST_OK(asx_region_open_child_with_budget(g_root, budget, &r));
    return r;
}

static asx_actor_handle spawn_bud(asx_region_id region, const asx_actor_behavior *behavior,
                                  bud_state *state) {
    asx_actor_handle h;
    h.slot = UINT32_MAX;
    h.generation = 0;
    MUST_OK(asx_actor_spawn(&h, region, behavior, state, 4));
    MUST_OK(asx_actor_task_id(h, &state->task));
    return h;
}

/* gen_server::on_start_budget: init runs under meet(task, phase) with the
 * larger priority, and the task's budget comes back afterwards less what
 * the phase consumed (gen_server.rs:5800 init_budget_consumption_...). */
static void test_start_budget_tightens_init_and_is_restored(void) {
    bud_state s;
    asx_actor_handle h;
    asx_budget region_budget = asx_budget_from_polls(1000u);
    asx_budget before_cast;
    asx_region_id r;

    setup();
    memset(&s, 0, sizeof(s));
    region_budget.cost_quota = 100u;
    r = budgeted_region(&region_budget);
    s.start_phase = asx_budget_from_polls(7u);
    s.start_phase.priority = 200u;
    s.start_phase.cost_quota = 10u;
    s.stop_phase = asx_budget_infinite();
    s.init_cost = 3u;
    h = spawn_bud(r, &g_bud, &s);
    run(r);

    ASSERT(s.start_budget_calls == 1 && s.init_calls == 1,
           "the phase budget is asked once, at init");
    ASSERT(s.before_start.poll_quota > 7u, "the task's own quota is looser than the phase's");
    ASSERT(s.at_start.poll_quota == 7u, "init runs under the tighter poll quota");
    ASSERT(s.at_start.cost_quota == 10u, "and the tighter cost quota");
    ASSERT(s.at_start.priority == 200u, "and the larger priority");

    bud_read(s.task, &before_cast);
    ASSERT(before_cast.poll_quota == s.before_start.poll_quota,
           "init used no polls: the task's poll quota is back");
    ASSERT(before_cast.cost_quota == 97u, "init's 3 cost units are not refunded: 100 - 3");
    ASSERT(before_cast.priority == 128u, "the task's own priority is back");
    ASSERT(before_cast.deadline == ASX_TIME_INFINITE, "no deadline was invented");

    MUST_OK(asx_actor_try_cast(h, 1));
    run(r);
    ASSERT(s.casts == 1, "the server serves messages after init");
    ASSERT(s.after_init.poll_quota < before_cast.poll_quota && s.after_init.poll_quota > 7u,
           "the message loop spends the task's own quota, not the phase's");
    ASSERT(s.after_init.cost_quota == 97u && s.after_init.priority == 128u,
           "the loop runs under the task's own budget");
}

/* A phase budget looser than the task's changes nothing: it is met, not set. */
static void test_start_budget_never_loosens_the_task_budget(void) {
    bud_state s;
    asx_actor_handle h;
    asx_budget region_budget = asx_budget_from_polls(50u);
    asx_region_id r;

    setup();
    memset(&s, 0, sizeof(s));
    r = budgeted_region(&region_budget);
    s.start_phase = asx_budget_from_polls(500u);
    s.stop_phase = asx_budget_infinite();
    h = spawn_bud(r, &g_bud, &s);
    (void)h;
    run(r);
    ASSERT(s.init_calls == 1, "init ran");
    ASSERT(s.at_start.poll_quota == s.before_start.poll_quota && s.at_start.poll_quota < 50u,
           "the phase's 500 polls do not raise the task's remaining quota");
}

/* gen_server::on_stop_budget defaults to Budget::MINIMAL: 100 polls, no
 * deadline or cost limit, priority 128 (types/budget.rs:242). */
static void test_stop_budget_defaults_to_minimal(void) {
    bud_state s;
    asx_actor_handle h;
    setup();
    memset(&s, 0, sizeof(s));
    h = spawn_bud(g_root, &g_bud_default, &s);
    run(g_root);
    MUST_OK(asx_actor_stop(h));
    run(g_root);
    ASSERT(s.term_calls == 1, "terminate ran");
    ASSERT(s.at_stop.poll_quota == 100u, "the drain and terminate get 100 polls");
    ASSERT(s.at_stop.priority == 128u, "at priority 128");
    ASSERT(s.at_stop.deadline == ASX_TIME_INFINITE && s.at_stop.cost_quota == UINT64_MAX,
           "with no deadline or cost limit");
}

/* gen_server::stop_budget_constrains_stop_phase (gen_server.rs:5905). */
static void test_custom_stop_budget_is_met_with_the_task_budget(void) {
    bud_state s;
    asx_actor_handle h;
    asx_budget region_budget = asx_budget_from_polls(100000u);
    asx_region_id r;

    setup();
    memset(&s, 0, sizeof(s));
    r = budgeted_region(&region_budget);
    s.start_phase = asx_budget_infinite();
    s.stop_phase = asx_budget_from_polls(42u);
    s.stop_phase.priority = 250u;
    h = spawn_bud(r, &g_bud, &s);
    run(r);
    MUST_OK(asx_actor_stop(h));
    run(r);
    ASSERT(s.stop_budget_calls == 1 && s.term_calls == 1, "the stop budget is asked once");
    ASSERT(s.before_stop.poll_quota > 42u, "the task had more than the phase allows");
    ASSERT(s.at_stop.poll_quota == 42u, "terminate runs under the tighter stop quota");
    ASSERT(s.at_stop.priority == 250u, "and the larger priority");
}

static void test_stop_budget_never_loosens_the_task_budget(void) {
    bud_state s;
    asx_actor_handle h;
    asx_budget region_budget = asx_budget_from_polls(30u);
    asx_region_id r;

    setup();
    memset(&s, 0, sizeof(s));
    r = budgeted_region(&region_budget);
    s.start_phase = asx_budget_infinite();
    s.stop_phase = asx_budget_from_polls(500u);
    h = spawn_bud(r, &g_bud, &s);
    run(r);
    MUST_OK(asx_actor_stop(h));
    run(r);
    ASSERT(s.term_calls == 1, "terminate ran");
    ASSERT(s.at_stop.poll_quota == s.before_stop.poll_quota && s.at_stop.poll_quota < 30u,
           "a looser stop budget leaves the task's remaining quota alone");
}

/* A stop before the first poll skips init, so its budget is never asked;
 * the stop phase still runs under its own. */
static void test_skipped_init_does_not_ask_for_the_start_budget(void) {
    bud_state s;
    asx_actor_handle h;
    setup();
    memset(&s, 0, sizeof(s));
    s.start_phase = asx_budget_from_polls(7u);
    s.stop_phase = asx_budget_from_polls(9u);
    h = spawn_bud(g_root, &g_bud, &s);
    MUST_OK(asx_actor_stop(h));
    run(g_root);
    ASSERT(s.init_calls == 0 && s.start_budget_calls == 0, "init and its budget are skipped");
    ASSERT(s.stop_budget_calls == 1 && s.term_calls == 1, "the stop phase ran");
    ASSERT(s.at_stop.poll_quota == 9u, "under the stop budget");
}

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

/* asx_actor_reset() (through asx_runtime_reset) clears only the slots spawn
 * selected since the last reset. After an epoch that leaves evictions
 * counted and a server panicked, a server spawned into any of those slots
 * starts clean, whatever footprint the epochs before and after had. */
#define ACTOR_EPOCH_MAX ((uint32_t)(ASX_MAX_ACTORS < 6u ? ASX_MAX_ACTORS : 6u))

static void actor_epoch_dirty(uint32_t n, rec_state *st, asx_actor_handle *hs) {
    uint32_t i;
    uint32_t k;

    for (i = 0; i < n; i++) {
        if (i % 2u == 0u) {
            hs[i] = spawn_rec_policy(g_root, &st[i], 2, ASX_ACTOR_OVERFLOW_DROP_OLDEST);
            for (k = 1u; k <= 4u; k++) MUST_OK(asx_actor_try_cast(hs[i], k));
        } else {
            hs[i] = spawn_rec(g_root, &st[i], 4);
            st[i].fail_cast_at = 1;
            MUST_OK(asx_actor_try_cast(hs[i], 1));
        }
    }
    run(g_root);
}

/* n servers in the first n slots: Created, no exit reason, nothing
 * evicted, nothing queued, and none has run anything but its own init. */
static int actor_epoch_fresh(uint32_t n) {
    static rec_state st[ACTOR_EPOCH_MAX];
    uint32_t i;

    for (i = 0; i < n; i++) {
        asx_actor_handle h = spawn_rec_policy(g_root, &st[i], 2, ASX_ACTOR_OVERFLOW_DROP_OLDEST);
        if (!asx_actor_is_alive(h)) return 0;
        if (asx_actor_get_state(h) != ASX_ACTOR_CREATED) return 0;
        if (asx_actor_exit_reason(h) != ASX_OK) return 0;
        if (asx_actor_evicted_count(h) != 0u) return 0;
        if (asx_actor_mailbox_count(h) != 0u) return 0;
    }
    run(g_root);
    for (i = 0; i < n; i++) {
        if (st[i].init_called != 1 || st[i].casts != 0u) return 0;
    }
    return 1;
}

static void test_reset_after_heavy_use_leaves_no_stale_state(void) {
    static rec_state dirty[ACTOR_EPOCH_MAX];
    asx_actor_handle hs[ACTOR_EPOCH_MAX];
    uint32_t pass;

    /* heavy, light, heavy */
    for (pass = 0; pass < 3u; pass++) {
        uint32_t n = (pass == 1u) ? 2u : ACTOR_EPOCH_MAX;

        setup();
        actor_epoch_dirty(n, dirty, hs);
        ASSERT(asx_actor_evicted_count(hs[0]) == 2u, "dirty: evictions counted");
        ASSERT(asx_actor_exit_reason(hs[1]) == ASX_E_INVALID_STATE, "dirty: a panicked server");

        setup(); /* asx_runtime_reset clears the actor table */
        ASSERT(actor_epoch_fresh(ACTOR_EPOCH_MAX), "servers spawned after the reset start clean");
    }
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
    RUN(test_refused_server_reply_does_not_repoll_following_sleep);
    RUN(test_reseed_refuses_pending_actor_admission_then_cleanup_completes);

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

    RUN(test_register_and_whereis);
    RUN(test_taken_name_is_refused_and_changes_nothing);
    RUN(test_unregister_checks_the_caller_and_the_lease_identity);
    RUN(test_a_lease_resolves_once_and_leaves_the_registry_alone);
    RUN(test_root_region_holder_and_full_registry_are_refused);
    RUN(test_name_arguments_are_checked);
    RUN(test_cleanup_drops_a_regions_and_a_tasks_names);
    RUN(test_named_server_registers_its_task);
    RUN(test_a_taken_name_spawns_no_server);
    RUN(test_release_name_waits_for_the_server_to_finish);
    RUN(test_abort_lease_frees_the_name_without_stopping_the_server);
    RUN(test_release_resolves_the_lease_when_the_entry_is_gone);

    RUN(test_drop_oldest_evicts_the_oldest_cast_to_make_room);
    RUN(test_reject_policy_never_evicts);
    RUN(test_drop_oldest_traces_the_eviction_on_the_calling_task);
    RUN(test_drop_oldest_never_evicts_a_queued_call);
    RUN(test_drop_oldest_with_only_calls_queued_is_full);

    RUN(test_start_budget_tightens_init_and_is_restored);
    RUN(test_start_budget_never_loosens_the_task_budget);
    RUN(test_stop_budget_defaults_to_minimal);
    RUN(test_custom_stop_budget_is_met_with_the_task_budget);
    RUN(test_stop_budget_never_loosens_the_task_budget);
    RUN(test_skipped_init_does_not_ask_for_the_start_budget);

    RUN(test_forged_and_reset_handles_are_rejected);
    RUN(test_reset_after_heavy_use_leaves_no_stale_state);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
