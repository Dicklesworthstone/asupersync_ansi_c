/*
 * ex_actor_supervision.c — Actor mailbox and supervision walkthrough
 *
 * Demonstrates:
 *   1. Spawning an actor through the umbrella header surface
 *   2. Cast + call mailbox semantics with deterministic replies
 *   3. Graceful stop with terminate callback observation
 *   4. A one_for_one supervisor replacing a failed child generation
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

/* A supervised child: each generation is a task running this body. The
 * first generation fails; the ones after it finish their work. */
typedef struct {
    uint32_t starts;
    uint64_t last_generation;
} fragile_child_ctx;

static uint64_t g_generation_numbers[8];

static asx_status fragile_body(void *user_data, asx_task_id self) {
    uint64_t number = *(const uint64_t *)user_data;
    (void)self;
    return number == 1u ? ASX_E_INVALID_STATE : ASX_OK;
}

static asx_status fragile_child_start(void *user_data, const asx_supervisor_generation *gen,
                                      asx_task_poll_fn *out_poll, void **out_data) {
    fragile_child_ctx *ctx = (fragile_child_ctx *)user_data;
    uint64_t *slot = &g_generation_numbers[ctx->starts % 8u];
    ctx->starts++;
    ctx->last_generation = gen->number;
    *slot = gen->number;
    *out_poll = fragile_body;
    *out_data = slot;
    return ASX_OK;
}

/* The task that spawns the supervisor in its own region and joins it. */
typedef struct {
    asx_region_id region;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_supervisor_handle supervisor;
    int spawned;
    asx_status spawn_status;
    asx_status join_status;
    asx_supervisor_report report;
} supervisor_owner;

static asx_status supervisor_owner_poll(void *user_data, asx_task_id self) {
    supervisor_owner *o = (supervisor_owner *)user_data;
    if (!o->spawned) {
        o->spawned = 1;
        o->spawn_status = asx_supervisor_spawn(&o->supervisor, o->region, &o->cfg, &o->spec, 1u);
        if (o->spawn_status != ASX_OK) return ASX_OK;
    }
    o->join_status = asx_supervisor_join(o->supervisor, self, &o->report);
    return o->join_status == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

/* A task that calls the server once. A call is polled like the Rust
 * future: repeat it with the same op until it stops returning PENDING. */
typedef struct {
    asx_actor_handle server;
    asx_cx cx;
    asx_actor_op op;
    asx_status status;
    uint64_t reply;
} caller_task;

static asx_status caller_poll(void *user_data, asx_task_id self) {
    caller_task *c = (caller_task *)user_data;
    asx_status st;
    (void)self;
    st = asx_actor_call(c->server, &c->cx, 42u, &c->op, &c->reply);
    if (st == ASX_E_PENDING) return st;
    c->status = st;
    return ASX_OK;
}

static void scenario_actor_mailbox_roundtrip(void) {
    asx_region_id region;
    asx_region_id callers;
    asx_actor_handle actor;
    asx_actor_behavior behavior;
    asx_task_id caller_id;
    caller_task caller;
    echo_actor_state state;

    SCENARIO_BEGIN("actor.mailbox_roundtrip");

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    memset(&caller, 0, sizeof(caller));
    behavior = echo_behavior();

    SCENARIO_CHECK(asx_region_open(&region) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_actor_spawn(&actor, region, &behavior, &state, 4u) == ASX_OK, "actor_spawn");

    pump_region(region, 1u);

    /* A cast needs no task; a call comes from a task outside the root
     * region (its reply is an obligation scoped to the caller's region). */
    SCENARIO_CHECK(asx_actor_try_cast(actor, 7u) == ASX_OK, "actor_cast");
    SCENARIO_CHECK(asx_region_open_child(region, &callers) == ASX_OK, "caller_region");
    caller.server = actor;
    caller.status = ASX_E_PENDING;
    SCENARIO_CHECK(asx_task_spawn(callers, caller_poll, &caller, &caller_id) == ASX_OK,
                   "caller_spawn");
    SCENARIO_CHECK(asx_cx_init(&caller.cx, callers, caller_id, ASX_CAP_CANCEL_CHECK) == ASX_OK,
                   "caller_cx");

    pump_region(region, 8u);

    SCENARIO_CHECK(caller.status == ASX_OK, "call_reply");
    SCENARIO_CHECK(caller.reply == 142u, "reply_value");
    SCENARIO_CHECK(state.cast_count == 1u, "cast_count");
    SCENARIO_CHECK(state.last_cast == 42u, "last_value_after_call");

    SCENARIO_CHECK(asx_actor_stop(actor) == ASX_OK, "actor_stop");
    pump_region(region, 4u);

    SCENARIO_CHECK(!asx_actor_is_alive(actor), "actor_dead_after_stop");
    SCENARIO_CHECK(state.terminate_called, "terminate_called");
    SCENARIO_CHECK(state.terminate_reason == ASX_OK, "terminate_reason_ok");

    SCENARIO_END();
}

static void scenario_supervisor_restart(void) {
    asx_region_id root;
    asx_task_id owner_id;
    supervisor_owner owner;
    fragile_child_ctx ctx;
    const asx_supervisor_completion *latest;

    SCENARIO_BEGIN("actor.supervisor_restart");

    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    memset(&owner, 0, sizeof(owner));

    /* A one_for_one supervisor allowing two restarts a minute; the child
     * is transient: replaced after an error, not after success. */
    asx_supervisor_config_init(&owner.cfg, "workers", 2u, 60000000000u);
    owner.cfg.backoff.kind = ASX_RESTART_BACKOFF_NONE;
    asx_child_spec_init(&owner.spec, "fragile", ASX_CHILD_TRANSIENT, fragile_child_start, &ctx);

    SCENARIO_CHECK(asx_region_open(&root) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_region_open_child(root, &owner.region) == ASX_OK, "owner_region");
    SCENARIO_CHECK(asx_task_spawn(owner.region, supervisor_owner_poll, &owner, &owner_id) == ASX_OK,
                   "owner_spawn");

    /* The controller starts the child in a region of its own; the first
     * generation fails, its region is drained and closed, and a second
     * generation replaces it and finishes. With no child left running
     * the supervisor closes its region and reports. */
    pump_region(root, 200u);

    SCENARIO_CHECK(owner.spawn_status == ASX_OK, "supervisor_spawn");
    SCENARIO_CHECK(owner.join_status == ASX_OK, "supervisor_join");
    SCENARIO_CHECK(ctx.starts == 2u, "child_restarted");
    SCENARIO_CHECK(owner.report.restart_batches == 1u, "restart_batches");
    SCENARIO_CHECK(owner.report.outcome == ASX_OUTCOME_OK, "supervisor_outcome");
    latest = &owner.report.completions[0];
    SCENARIO_CHECK(owner.report.completion_count == 1u && latest->generation.number == 2u &&
                       latest->outcome == ASX_OUTCOME_OK,
                   "second_generation_succeeded");
    SCENARIO_CHECK(!asx_supervisor_is_alive(owner.supervisor), "supervisor_finished");

    printf("ARTIFACT supervisor.restart generations=%u restarts=%llu joined=%llu\n", ctx.starts,
           (unsigned long long)owner.report.restart_batches,
           (unsigned long long)owner.report.joined);

    SCENARIO_END();
}

int main(void) {
    scenario_actor_mailbox_roundtrip();
    scenario_supervisor_restart();

    printf("SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
