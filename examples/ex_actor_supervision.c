/*
 * ex_actor_supervision.c — Actor mailbox and supervision walkthrough
 *
 * Demonstrates:
 *   1. Spawning an actor through the umbrella header surface
 *   2. Cast + call mailbox semantics with deterministic replies
 *   3. Graceful stop with terminate callback observation
 *   4. one_for_one supervisor restart after child failure
 *
 * Output: SCENARIO lines for smoke-test validation plus ARTIFACT lines
 * carrying restart details for log retention.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

static void pump_region(asx_region_id region, uint32_t polls) {
    asx_budget budget = asx_budget_infinite();
    budget.poll_quota = polls;
    IGNORE_RC(asx_scheduler_run(region, &budget));
}

typedef struct {
    uint64_t last_cast;
    uint32_t cast_count;
    int terminate_called;
    asx_status terminate_reason;
} echo_actor_state;

static asx_status echo_cast(void *state, uint64_t msg, asx_actor_handle self) {
    echo_actor_state *actor = (echo_actor_state *)state;
    (void)self;
    actor->last_cast = msg;
    actor->cast_count++;
    return ASX_OK;
}

static asx_status echo_call(void *state, uint64_t request, uint64_t *reply, asx_actor_handle self) {
    echo_actor_state *actor = (echo_actor_state *)state;
    (void)self;
    actor->last_cast = request;
    *reply = request + 100u;
    return ASX_OK;
}

static void echo_terminate(void *state, asx_status reason, asx_actor_handle self) {
    echo_actor_state *actor = (echo_actor_state *)state;
    (void)self;
    actor->terminate_called = 1;
    actor->terminate_reason = reason;
}

static asx_actor_behavior echo_behavior(void) {
    asx_actor_behavior behavior;
    behavior.init = NULL;
    behavior.handle_cast = echo_cast;
    behavior.handle_call = echo_call;
    behavior.terminate = echo_terminate;
    return behavior;
}

typedef struct {
    asx_actor_handle handle;
    uint32_t starts;
} fragile_child_ctx;

static asx_status fragile_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    return ASX_E_INVALID_STATE;
}

static asx_status fragile_child_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    fragile_child_ctx *ctx = (fragile_child_ctx *)user_data;
    asx_actor_behavior behavior;
    asx_status st;

    behavior.init = NULL;
    behavior.handle_cast = fragile_cast;
    behavior.handle_call = NULL;
    behavior.terminate = NULL;

    st = asx_actor_spawn(out, region, &behavior, NULL);
    if (st == ASX_OK) {
        ctx->handle = *out;
        ctx->starts++;
    }
    return st;
}

static void scenario_actor_mailbox_roundtrip(void) {
    asx_region_id region;
    asx_actor_handle actor;
    asx_actor_behavior behavior;
    asx_call_token token;
    echo_actor_state state;
    uint64_t reply = 0u;

    SCENARIO_BEGIN("actor.mailbox_roundtrip");

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    behavior = echo_behavior();

    SCENARIO_CHECK(asx_region_open(&region) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_actor_spawn(&actor, region, &behavior, &state) == ASX_OK, "actor_spawn");

    pump_region(region, 1u);

    SCENARIO_CHECK(asx_actor_cast(actor, 7u) == ASX_OK, "actor_cast");
    SCENARIO_CHECK(asx_actor_call(actor, 42u, &token) == ASX_OK, "actor_call");

    pump_region(region, 2u);

    SCENARIO_CHECK(asx_call_token_poll(token, &reply) == ASX_OK, "call_reply");
    SCENARIO_CHECK(reply == 142u, "reply_value");
    SCENARIO_CHECK(state.cast_count == 1u, "cast_count");
    SCENARIO_CHECK(state.last_cast == 42u, "last_value_after_call");

    SCENARIO_CHECK(asx_actor_stop(actor) == ASX_OK, "actor_stop");
    pump_region(region, 2u);

    SCENARIO_CHECK(!asx_actor_is_alive(actor), "actor_dead_after_stop");
    SCENARIO_CHECK(state.terminate_called, "terminate_called");
    SCENARIO_CHECK(state.terminate_reason == ASX_OK, "terminate_reason_ok");

    SCENARIO_END();
}

static void scenario_supervisor_restart(void) {
    asx_region_id region;
    asx_supervisor_handle supervisor;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    fragile_child_ctx ctx;
    asx_actor_handle original;

    SCENARIO_BEGIN("actor.supervisor_restart");

    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));

    cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg.max_restarts = 2u;
    spec.start_fn = fragile_child_start;
    spec.user_data = &ctx;
    spec.restart = ASX_CHILD_PERMANENT;

    SCENARIO_CHECK(asx_region_open(&region) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_supervisor_start(&supervisor, region, &cfg, &spec, 1u) == ASX_OK,
                   "supervisor_start");

    pump_region(region, 2u);
    SCENARIO_CHECK(asx_supervisor_child_alive(supervisor, 0u), "child_alive_initial");

    original = ctx.handle;
    SCENARIO_CHECK(asx_actor_cast(ctx.handle, 1u) == ASX_OK, "kill_child_cast");

    pump_region(region, 4u);

    SCENARIO_CHECK(asx_supervisor_is_alive(supervisor), "supervisor_alive_after_restart");
    SCENARIO_CHECK(asx_supervisor_restart_count(supervisor) == 1u, "restart_count");
    SCENARIO_CHECK(ctx.starts == 2u, "child_restarted");
    SCENARIO_CHECK(asx_supervisor_child_alive(supervisor, 0u), "child_alive_after_restart");
    SCENARIO_CHECK(ctx.handle.generation != original.generation, "generation_changed");

    printf("ARTIFACT supervisor.restart before=%u after=%u restarts=%u\n", original.generation,
           ctx.handle.generation, asx_supervisor_restart_count(supervisor));

    SCENARIO_CHECK(asx_supervisor_stop(supervisor) == ASX_OK, "supervisor_stop");
    pump_region(region, 8u);
    SCENARIO_CHECK(!asx_supervisor_is_alive(supervisor), "supervisor_dead_after_stop");

    SCENARIO_END();
}

int main(void) {
    scenario_actor_mailbox_roundtrip();
    scenario_supervisor_restart();

    printf("SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
