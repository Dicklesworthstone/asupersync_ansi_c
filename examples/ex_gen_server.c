/*
 * ex_gen_server.c — GenServer example
 *
 * A counter server is a task with a bounded mailbox (asupersync's
 * GenServer). A client task calls and casts to it, then stops it and joins
 * its task. The client operations are polled like the Rust futures:
 * ASX_E_PENDING means "not yet", and the operation is repeated with the
 * same asx_actor_op until it returns anything else. A call needs a task
 * outside the root region, as in Rust.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

/* A zeroed runtime object names the library's static kernel (tag 0). */
static asx_runtime g_rt;

/* --- Counter server behavior --- */

typedef struct {
    int64_t count;
} counter_state;

static asx_status counter_call(void *state, uint64_t request, uint64_t *reply,
                               asx_actor_handle self) {
    counter_state *cs = (counter_state *)state;
    (void)self;
    switch (request) {
    case 0: /* get */ *reply = (uint64_t)cs->count; break;
    case 1: /* increment */
        cs->count++;
        *reply = (uint64_t)cs->count;
        break;
    case 2: /* decrement */
        cs->count--;
        *reply = (uint64_t)cs->count;
        break;
    default:
        /* A callback that fails is the C form of a Rust panic: the server
         * stops at once and its task completes PANICKED. */
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

static asx_status counter_cast(void *state, uint64_t message, asx_actor_handle self) {
    counter_state *cs = (counter_state *)state;
    (void)self;
    cs->count += (int64_t)message;
    return ASX_OK;
}

static void counter_terminate(void *state, asx_status reason, asx_actor_handle self) {
    (void)state;
    (void)reason;
    (void)self;
    printf("SCENARIO gen_server_terminated pass\n");
}

/* --- A client task running a short script against the server --- */

typedef enum { STEP_CALL, STEP_CAST, STEP_STOP_JOIN } step_kind;

typedef struct {
    step_kind kind;
    uint64_t arg;
    asx_status status;
    uint64_t reply;
} script_step;

#define SCRIPT_STEPS 7u

typedef struct {
    asx_actor_handle server;
    asx_cx cx;
    asx_actor_op op;
    script_step steps[SCRIPT_STEPS];
    uint32_t pc;
    int stop_sent;
} client;

static asx_status client_poll(void *user_data, asx_task_id self) {
    client *c = (client *)user_data;
    (void)self;
    while (c->pc < SCRIPT_STEPS) {
        script_step *s = &c->steps[c->pc];
        asx_status st = ASX_E_INVALID_STATE;
        switch (s->kind) {
        case STEP_CALL: st = asx_actor_call(c->server, &c->cx, s->arg, &c->op, &s->reply); break;
        case STEP_CAST: st = asx_actor_cast(c->server, &c->cx, s->arg, &c->op); break;
        case STEP_STOP_JOIN:
            if (!c->stop_sent) {
                st = asx_actor_stop(c->server);
                if (st != ASX_OK) break;
                c->stop_sent = 1;
            }
            st = asx_actor_join(c->server, &c->cx);
            break;
        }
        if (st == ASX_E_PENDING) return ASX_E_PENDING; /* resumed when it can progress */
        s->status = st;
        c->pc++;
        memset(&c->op, 0, sizeof(c->op)); /* a fresh op per step */
    }
    return ASX_OK;
}

static void set_step(client *c, uint32_t i, step_kind kind, uint64_t arg) {
    c->steps[i].kind = kind;
    c->steps[i].arg = arg;
    c->steps[i].status = ASX_E_PENDING;
    c->steps[i].reply = 0u;
}

int main(void) {
    static counter_state state;
    static client c;
    asx_actor_behavior behavior;
    asx_actor_handle server;
    asx_region_id root;
    asx_region_id region;
    asx_task_id client_task = ASX_INVALID_ID;
    asx_budget budget = asx_budget_from_polls(1000u);
    asx_status st;
    uint32_t i;

    printf("SCENARIO gen_server_example\n");

    asx_runtime_reset(&g_rt);
    st = asx_region_open(&g_rt, &root);
    if (st == ASX_OK) st = asx_region_open_child(root, &region);
    if (st != ASX_OK) {
        printf("SCENARIO gen_server_start fail\n");
        return 1;
    }

    memset(&behavior, 0, sizeof(behavior));
    behavior.handle_call = counter_call;
    behavior.handle_cast = counter_cast;
    behavior.terminate = counter_terminate;
    st = asx_actor_spawn(&server, region, &behavior, &state, 8u);
    if (st != ASX_OK) {
        printf("SCENARIO gen_server_start fail\n");
        return 1;
    }
    printf("SCENARIO gen_server_start pass\n");

    memset(&c, 0, sizeof(c));
    c.server = server;
    set_step(&c, 0u, STEP_CALL, 1u); /* increment: 1 */
    set_step(&c, 1u, STEP_CALL, 1u); /* increment: 2 */
    set_step(&c, 2u, STEP_CALL, 1u); /* increment: 3 */
    set_step(&c, 3u, STEP_CALL, 0u); /* get */
    set_step(&c, 4u, STEP_CAST, 10u);
    set_step(&c, 5u, STEP_CALL, 0u); /* get */
    set_step(&c, 6u, STEP_STOP_JOIN, 0u);

    st = asx_task_spawn(region, client_poll, &c, &client_task);
    if (st == ASX_OK) st = asx_cx_init(&c.cx, region, client_task, ASX_CAP_NONE);
    if (st != ASX_OK) {
        printf("SCENARIO gen_server_client fail\n");
        return 1;
    }

    /* Drive the client and the server until neither can run. */
    st = asx_scheduler_run(region, &budget);
    (void)st;

    for (i = 0u; i < SCRIPT_STEPS; i++) {
        if (c.steps[i].status != ASX_OK) {
            printf("SCENARIO gen_server_step_%u fail status=%d\n", (unsigned)i,
                   (int)c.steps[i].status);
            return 1;
        }
    }

    printf("SCENARIO gen_server_count_%llu %s\n", (unsigned long long)c.steps[3].reply,
           c.steps[3].reply == 3u ? "pass" : "fail");
    printf("SCENARIO gen_server_after_cast_%llu %s\n", (unsigned long long)c.steps[5].reply,
           c.steps[5].reply == 13u ? "pass" : "fail");
    printf("SCENARIO gen_server_stopped %s\n",
           asx_actor_get_state(server) == ASX_ACTOR_STOPPED ? "pass" : "fail");

    return 0;
}
