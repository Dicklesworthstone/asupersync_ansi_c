/*
 * e2e_actor_supervision.c -- deterministic actor/supervision semantic harness
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static asx_status g_status_sink;

static unsigned long long mix_u64(unsigned long long state, unsigned long long value) {
    state ^= value + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
    return state;
}

static const char *env_or_default(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value != NULL ? value : fallback;
}

static const char *compile_profile_name(void) {
#if defined(ASX_PROFILE_POSIX)
    return "POSIX";
#elif defined(ASX_PROFILE_WIN32)
    return "WIN32";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "FREESTANDING";
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_HFT)
    return "HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "AUTOMOTIVE";
#elif defined(ASX_PROFILE_PARALLEL)
    return "PARALLEL";
#elif defined(ASX_PROFILE_BROWSER)
    return "BROWSER";
#else
    return "CORE";
#endif
}

static const char *compile_codec_name(void) {
#if defined(ASX_CODEC_BIN)
    return "bin";
#else
    return "json";
#endif
}

static const char *compile_deterministic_name(void) {
#if ASX_DETERMINISTIC
    return "1";
#else
    return "0";
#endif
}

static void scenario_line(const char *id, int pass, const char *detail) {
    printf("SCENARIO %s %s%s%s\n", id, pass ? "pass" : "fail", detail != NULL ? " " : "",
           detail != NULL ? detail : "");
}

static asx_region_id make_region(void) {
    asx_region_id region = ASX_INVALID_ID;
    if (asx_region_open(&region) != ASX_OK) { return ASX_INVALID_ID; }
    return region;
}

static void drive_region(asx_region_id region, uint32_t polls) {
    asx_budget budget = asx_budget_infinite();
    budget.poll_quota = polls;
    g_status_sink = asx_scheduler_run(region, &budget);
    (void)g_status_sink;
}

static unsigned long long record_config(unsigned long long digest) {
    const char *seed = env_or_default("ASX_E2E_SEED", "42");
    const char *profile = env_or_default("ASX_E2E_PROFILE", compile_profile_name());
    const char *codec = env_or_default("ASX_E2E_CODEC", compile_codec_name());
    const char *deterministic =
        env_or_default("ASX_E2E_DETERMINISTIC", compile_deterministic_name());
    const char *resource_class = env_or_default("ASX_E2E_RESOURCE_CLASS", "R3");
    char detail[384];
    int n;

    n = snprintf(detail, sizeof(detail),
                 "seed=%s profile=%s codec=%s deterministic=%s resource_class=%s "
                 "command=make_test-e2e-actor-supervision",
                 seed, profile, codec, deterministic, resource_class);
    if (n < 0 || (size_t)n >= sizeof(detail)) {
        scenario_line("actor_supervision.config", 0, "detail_truncated");
        return digest;
    }

    scenario_line("actor_supervision.config", 1, detail);
    printf("TRACE actor_supervision.config %s\n", detail);
    printf("REPLAY ASX_E2E_SEED=%s ASX_E2E_PROFILE=%s ASX_E2E_CODEC=%s "
           "ASX_E2E_DETERMINISTIC=%s make test-e2e-actor-supervision\n",
           seed, profile, codec, deterministic);

    digest = mix_u64(digest, (unsigned long long)ASX_MAX_ACTORS);
    digest = mix_u64(digest, (unsigned long long)ASX_MAX_SUPERVISORS);
    return digest;
}

typedef struct {
    asx_region_id region;
    uint64_t last_cast;
    uint32_t cast_count;
    uint32_t init_count;
    uint32_t terminate_count;
    asx_status terminate_reason;
    asx_obligation_id obligation;
    int reserve_obligation;
} e2e_actor_state;

static asx_status e2e_actor_init(void *state, asx_actor_handle self) {
    e2e_actor_state *s = (e2e_actor_state *)state;
    (void)self;
    s->init_count++;
    if (s->reserve_obligation) {
        asx_status st = asx_obligation_reserve(s->region, &s->obligation);
        if (st != ASX_OK) return st;
    }
    return ASX_OK;
}

static asx_status e2e_actor_cast(void *state, uint64_t msg, asx_actor_handle self) {
    e2e_actor_state *s = (e2e_actor_state *)state;
    (void)self;
    s->last_cast = msg;
    s->cast_count++;
    return ASX_OK;
}

static asx_status e2e_actor_call(void *state, uint64_t request, uint64_t *reply,
                                 asx_actor_handle self) {
    (void)state;
    (void)self;
    *reply = request + 7u;
    return ASX_OK;
}

static void e2e_actor_terminate(void *state, asx_status reason, asx_actor_handle self) {
    e2e_actor_state *s = (e2e_actor_state *)state;
    (void)self;
    s->terminate_count++;
    s->terminate_reason = reason;
    if (s->reserve_obligation) {
        asx_obligation_state os;
        if (asx_obligation_get_state(s->obligation, &os) == ASX_OK &&
            os == ASX_OBLIGATION_RESERVED) {
            g_status_sink = asx_obligation_abort(s->obligation);
            (void)g_status_sink;
        }
    }
}

static asx_actor_behavior e2e_actor_behavior(void) {
    asx_actor_behavior behavior;
    behavior.init = e2e_actor_init;
    behavior.handle_cast = e2e_actor_cast;
    behavior.handle_call = e2e_actor_call;
    behavior.terminate = e2e_actor_terminate;
    return behavior;
}

/* A task calling the server once (a call needs a task outside the root
 * region, as in Rust). */
typedef struct {
    asx_actor_handle server;
    asx_cx cx;
    asx_actor_op op;
    asx_status status;
    uint64_t reply;
} e2e_caller;

static asx_status e2e_caller_poll(void *user_data, asx_task_id self) {
    e2e_caller *c = (e2e_caller *)user_data;
    asx_status st;
    (void)self;
    st = asx_actor_call(c->server, &c->cx, 35u, &c->op, &c->reply);
    if (st == ASX_E_PENDING) return st;
    c->status = st;
    return ASX_OK;
}

static int start_caller(e2e_caller *c, asx_actor_handle server, asx_region_id parent) {
    asx_region_id child = ASX_INVALID_ID;
    asx_task_id id = ASX_INVALID_ID;
    memset(c, 0, sizeof(*c));
    c->server = server;
    c->status = ASX_E_PENDING;
    if (asx_region_open_child(parent, &child) != ASX_OK) return 0;
    if (asx_task_spawn(child, e2e_caller_poll, c, &id) != ASX_OK) return 0;
    return asx_cx_init(&c->cx, child, id, ASX_CAP_CANCEL_CHECK) == ASX_OK;
}

static int run_actor_lifecycle(unsigned long long *digest) {
    asx_actor_handle actor;
    asx_actor_behavior behavior = e2e_actor_behavior();
    e2e_caller caller;
    e2e_actor_state state;
    asx_region_id region;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    region = make_region();
    if (region == ASX_INVALID_ID) {
        scenario_line("actor.lifecycle", 0, "region_open_failed");
        return 0;
    }
    state.region = region;

    if (asx_actor_spawn(&actor, region, &behavior, &state, 4u) != ASX_OK) {
        scenario_line("actor.lifecycle", 0, "spawn_failed");
        return 0;
    }
    drive_region(region, 4u);
    if (state.init_count != 1u || !asx_actor_is_alive(actor)) {
        scenario_line("actor.lifecycle", 0, "init_or_alive_mismatch");
        return 0;
    }

    if (asx_actor_try_cast(actor, 41u) != ASX_OK || !start_caller(&caller, actor, region)) {
        scenario_line("actor.lifecycle", 0, "send_failed");
        return 0;
    }
    drive_region(region, 8u);
    if (caller.status != ASX_OK || caller.reply != 42u || state.cast_count != 1u ||
        state.last_cast != 41u) {
        scenario_line("actor.lifecycle", 0, "reply_or_cast_mismatch");
        return 0;
    }
    if (asx_actor_stop(actor) != ASX_OK) {
        scenario_line("actor.lifecycle", 0, "stop_failed");
        return 0;
    }
    drive_region(region, 8u);
    if (asx_actor_is_alive(actor) || state.terminate_count != 1u ||
        state.terminate_reason != ASX_OK) {
        scenario_line("actor.lifecycle", 0, "terminate_mismatch");
        return 0;
    }

    scenario_line("actor.lifecycle", 1, "casts=1 reply=42 terminate=1");
    *digest = mix_u64(*digest, caller.reply);
    *digest = mix_u64(*digest, state.cast_count);
    return 1;
}

typedef struct {
    uint64_t count;
    uint32_t terminated;
} e2e_server_state;

static e2e_server_state g_server_state;

static asx_status server_init(void *args, void **out_state) {
    (void)args;
    memset(&g_server_state, 0, sizeof(g_server_state));
    *out_state = &g_server_state;
    return ASX_OK;
}

static asx_status server_call(void *state, uint64_t request, uint64_t *reply) {
    e2e_server_state *s = (e2e_server_state *)state;
    if (request != 0u) { s->count += request; }
    *reply = s->count;
    return ASX_OK;
}

static asx_status server_cast(void *state, uint64_t message) {
    e2e_server_state *s = (e2e_server_state *)state;
    s->count += message;
    return ASX_OK;
}

static void server_terminate(void *state, asx_status reason) {
    e2e_server_state *s = (e2e_server_state *)state;
    (void)reason;
    s->terminated = 1u;
}

static int run_gen_server(unsigned long long *digest) {
    const asx_gen_server_callbacks callbacks = {server_init, server_call, server_cast,
                                                server_terminate};
    asx_gen_server_ref ref;
    uint64_t reply = 0u;

    asx_gen_server_reset();
    if (asx_gen_server_start_named(&ref, "counter", &callbacks, NULL) != ASX_OK) {
        scenario_line("gen_server.request_reply", 0, "start_failed");
        return 0;
    }
    if (strcmp(asx_gen_server_name(ref), "counter") != 0) {
        scenario_line("gen_server.request_reply", 0, "name_mismatch");
        return 0;
    }
    if (asx_gen_server_call(ref, 5u, &reply) != ASX_OK || reply != 5u ||
        asx_gen_server_cast(ref, 7u) != ASX_OK || asx_gen_server_call(ref, 0u, &reply) != ASX_OK ||
        reply != 12u || asx_gen_server_calls_handled(ref) != 2u ||
        asx_gen_server_casts_handled(ref) != 1u) {
        scenario_line("gen_server.request_reply", 0, "dispatch_mismatch");
        return 0;
    }
    if (asx_gen_server_stop(ref) != ASX_OK || !g_server_state.terminated ||
        !asx_gen_server_is_finished(ref)) {
        scenario_line("gen_server.request_reply", 0, "stop_mismatch");
        return 0;
    }

    scenario_line("gen_server.request_reply", 1, "calls=2 casts=1 count=12 name=counter");
    *digest = mix_u64(*digest, reply);
    *digest = mix_u64(*digest, asx_gen_server_calls_handled(ref));
    return 1;
}

/* A supervised child (asx/actor/supervisor.h): each generation yields
 * `yields` times, then ends with `fail_status` while its number is below
 * `fail_until` and ASX_OK afterwards; `block` waits until cancelled. */
typedef struct {
    uint32_t yields;
    uint64_t fail_until;
    asx_status fail_status;
    int block;
    uint32_t starts;
    uint32_t order;
} e2e_child;

typedef struct {
    e2e_child *child;
    uint64_t number;
    uint32_t polls;
} e2e_body;

static e2e_body g_e2e_bodies[64];
static uint32_t g_e2e_bodies_used;
static uint32_t g_e2e_start_order;

static asx_status e2e_body_poll(void *user_data, asx_task_id self) {
    e2e_body *b = (e2e_body *)user_data;
    if (b->child->block) {
        asx_checkpoint_result cr;
        if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_E_CANCELLED;
        g_status_sink = asx_task_park(self);
        return ASX_E_PENDING;
    }
    if (b->polls++ < b->child->yields) return ASX_E_PENDING;
    return b->number < b->child->fail_until ? b->child->fail_status : ASX_OK;
}

static asx_status e2e_child_start(void *user_data, const asx_supervisor_generation *gen,
                                  asx_task_poll_fn *out_poll, void **out_data) {
    e2e_child *c = (e2e_child *)user_data;
    e2e_body *b;
    c->starts++;
    c->order = ++g_e2e_start_order;
    if (g_e2e_bodies_used >= 64u) return ASX_E_RESOURCE_EXHAUSTED;
    b = &g_e2e_bodies[g_e2e_bodies_used++];
    b->child = c;
    b->number = gen->number;
    b->polls = 0u;
    *out_poll = e2e_body_poll;
    *out_data = b;
    return ASX_OK;
}

/* The task that spawns the supervisor in its own region and joins it,
 * aborting it first after `abort_after` polls when that is set. */
typedef struct {
    asx_region_id region;
    asx_supervisor_config cfg;
    asx_child_spec specs[3];
    uint32_t n;
    uint32_t abort_after;
    uint32_t polls;
    int spawned;
    asx_status spawn_status;
    asx_supervisor_handle handle;
    asx_status join_status;
    asx_supervisor_report report;
} e2e_owner;

static asx_status e2e_owner_poll(void *user_data, asx_task_id self) {
    e2e_owner *o = (e2e_owner *)user_data;
    if (!o->spawned) {
        o->spawned = 1;
        o->spawn_status = asx_supervisor_spawn(&o->handle, o->region, &o->cfg, o->specs, o->n);
        if (o->spawn_status != ASX_OK) return ASX_OK;
    }
    o->polls++;
    if (o->abort_after != 0u) {
        if (o->polls < o->abort_after) return ASX_E_PENDING;
        if (o->polls == o->abort_after) g_status_sink = asx_supervisor_abort(o->handle);
    }
    o->join_status = asx_supervisor_join(o->handle, self, &o->report);
    return o->join_status == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

/* A fresh runtime, an owner region under a root, and a supervisor config
 * without backoff. */
static asx_region_id e2e_supervisor_setup(e2e_owner *o, uint32_t max_restarts) {
    asx_region_id root = make_region();
    asx_region_id region = ASX_INVALID_ID;
    if (asx_region_open_child(root, &region) != ASX_OK) return ASX_INVALID_ID;
    memset(o, 0, sizeof(*o));
    o->region = region;
    o->join_status = ASX_E_PENDING;
    asx_supervisor_config_init(&o->cfg, "sup", max_restarts, 60000000000u);
    o->cfg.backoff.kind = ASX_RESTART_BACKOFF_NONE;
    g_e2e_bodies_used = 0u;
    g_e2e_start_order = 0u;
    return root;
}

static void e2e_add_child(e2e_owner *o, const char *name, asx_child_restart restart, e2e_child *c) {
    asx_child_spec_init(&o->specs[o->n++], name, restart, e2e_child_start, c);
}

static void e2e_run_owner(e2e_owner *o, asx_region_id root) {
    asx_task_id id = ASX_INVALID_ID;
    if (asx_task_spawn(o->region, e2e_owner_poll, o, &id) != ASX_OK) return;
    drive_region(root, 5000u);
}

/* A permanent child is replaced after every end, a temporary one never. */
static int run_supervisor_restart_policy(unsigned long long *digest) {
    e2e_owner o;
    e2e_child permanent;
    e2e_child temporary;
    asx_region_id root;

    asx_runtime_reset();
    memset(&permanent, 0, sizeof(permanent));
    memset(&temporary, 0, sizeof(temporary));
    permanent.fail_until = 2u; /* the first generation fails, later ones end ok */
    permanent.fail_status = ASX_E_INVALID_STATE;
    temporary.fail_until = 999u;
    temporary.fail_status = ASX_E_INVALID_STATE;
    root = e2e_supervisor_setup(&o, 1u);
    e2e_add_child(&o, "permanent", ASX_CHILD_PERMANENT, &permanent);
    e2e_add_child(&o, "temporary", ASX_CHILD_TEMPORARY, &temporary);
    e2e_run_owner(&o, root);
    if (o.spawn_status != ASX_OK || o.join_status != ASX_OK) {
        scenario_line("supervisor.restart_policy", 0, "spawn_or_join_failed");
        return 0;
    }
    if (permanent.starts != 2u || temporary.starts != 1u || o.report.restart_batches != 1u ||
        o.report.outcome != ASX_OUTCOME_OK) {
        scenario_line("supervisor.restart_policy", 0, "restart_mismatch");
        return 0;
    }

    scenario_line("supervisor.restart_policy", 1, "permanent=restart temporary=no_restart");
    *digest = mix_u64(*digest, permanent.starts);
    *digest = mix_u64(*digest, o.report.restart_batches);
    return 1;
}

/* Restarts past max_restarts in the window are refused: under STOP the
 * child stays stopped; under ESCALATE the supervisor stops with
 * RestartLimit and cancels its owner's region. */
static int run_restart_intensity(unsigned long long *digest) {
    e2e_owner o;
    e2e_child child;
    asx_region_id root;
    asx_cancel_reason reason;

    asx_runtime_reset();
    memset(&child, 0, sizeof(child));
    child.fail_until = 999u;
    child.fail_status = ASX_E_INVALID_STATE;
    root = e2e_supervisor_setup(&o, 2u);
    e2e_add_child(&o, "worker", ASX_CHILD_PERMANENT, &child);
    e2e_run_owner(&o, root);
    if (o.join_status != ASX_OK || child.starts != 3u || o.report.restart_batches != 2u ||
        o.report.outcome != ASX_OUTCOME_OK) {
        scenario_line("supervisor.restart_intensity", 0, "stop_mismatch");
        return 0;
    }

    asx_runtime_reset();
    memset(&child, 0, sizeof(child));
    child.fail_until = 999u;
    child.fail_status = ASX_E_INVALID_STATE;
    root = e2e_supervisor_setup(&o, 1u);
    o.cfg.escalation = ASX_ESCALATION_ESCALATE;
    e2e_add_child(&o, "worker", ASX_CHILD_PERMANENT, &child);
    e2e_run_owner(&o, root);
    if (o.join_status != ASX_OK || child.starts != 2u || o.report.outcome != ASX_OUTCOME_ERR ||
        o.report.error != ASX_SUPERVISOR_ERR_RESTART_LIMIT || o.report.escalations != 1u ||
        asx_region_get_cancel_reason(o.region, &reason) != ASX_OK ||
        reason.kind != ASX_CANCEL_FAIL_FAST) {
        scenario_line("supervisor.restart_intensity", 0, "escalation_mismatch");
        return 0;
    }

    scenario_line("supervisor.restart_intensity", 1, "stop=stays_stopped escalate=restart_limit");
    *digest = mix_u64(*digest, o.report.restart_batches);
    return 1;
}

/* An abort while a replacement waits for its backoff: the controller
 * stops, drains every generation, and starts no new one. */
static int run_cancel_during_restart(unsigned long long *digest) {
    e2e_owner o;
    e2e_child failing;
    e2e_child stable;
    asx_region_id root;

    asx_runtime_reset();
    memset(&failing, 0, sizeof(failing));
    memset(&stable, 0, sizeof(stable));
    failing.fail_until = 999u;
    failing.fail_status = ASX_E_INVALID_STATE;
    stable.block = 1;
    root = e2e_supervisor_setup(&o, 5u);
    o.cfg.backoff.kind = ASX_RESTART_BACKOFF_FIXED;
    o.cfg.backoff.initial_ns = 1000000u;
    o.abort_after = 6u;
    e2e_add_child(&o, "failing", ASX_CHILD_PERMANENT, &failing);
    e2e_add_child(&o, "stable", ASX_CHILD_PERMANENT, &stable);
    e2e_run_owner(&o, root);
    if (o.join_status != ASX_OK || o.report.outcome != ASX_OUTCOME_CANCELLED ||
        failing.starts != 1u || stable.starts != 1u || o.report.restart_batches != 0u ||
        o.report.joined != 2u) {
        scenario_line("supervisor.cancel_during_restart", 0, "pending_restart_not_cancelled");
        return 0;
    }

    scenario_line("supervisor.cancel_during_restart", 1, "pending_restart_cancelled=1");
    *digest = mix_u64(*digest, o.report.joined);
    return 1;
}

/* Child specs: a dependency starts first whatever the spec order, and the
 * report lists the children in that compiled order. */
static int run_child_specs(unsigned long long *digest) {
    e2e_owner o;
    e2e_child alpha;
    e2e_child beta;
    asx_region_id root;

    asx_runtime_reset();
    memset(&alpha, 0, sizeof(alpha));
    memset(&beta, 0, sizeof(beta));
    root = e2e_supervisor_setup(&o, 3u);
    e2e_add_child(&o, "beta", ASX_CHILD_TRANSIENT, &beta);
    e2e_add_child(&o, "alpha", ASX_CHILD_TRANSIENT, &alpha);
    if (asx_child_spec_depends_on(&o.specs[0], 1u) != ASX_OK) {
        scenario_line("supervisor.child_specs", 0, "depends_on_failed");
        return 0;
    }
    e2e_run_owner(&o, root);
    if (o.join_status != ASX_OK || alpha.order != 1u || beta.order != 2u ||
        o.report.completion_count != 2u || o.report.completions[0].child != 1u) {
        scenario_line("supervisor.child_specs", 0, "order_mismatch");
        return 0;
    }

    scenario_line("supervisor.child_specs", 1, "order=alpha,beta dependency_first=1");
    *digest = mix_u64(*digest, o.report.completion_count);
    return 1;
}

static int run_obligation_cleanup(unsigned long long *digest) {
    asx_actor_handle actor;
    asx_actor_behavior behavior = e2e_actor_behavior();
    e2e_actor_state state;
    asx_region_id region;
    asx_obligation_state os;
    asx_runtime_snapshot snap;
    uint32_t i;
    uint32_t reserved = 0u;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    region = make_region();
    state.region = region;
    state.reserve_obligation = 1;

    if (asx_actor_spawn(&actor, region, &behavior, &state, 4u) != ASX_OK) {
        scenario_line("actor.obligation_cleanup", 0, "spawn_failed");
        return 0;
    }
    drive_region(region, 4u);
    if (state.obligation == ASX_INVALID_ID ||
        asx_obligation_get_state(state.obligation, &os) != ASX_OK ||
        os != ASX_OBLIGATION_RESERVED) {
        scenario_line("actor.obligation_cleanup", 0, "reserve_mismatch");
        return 0;
    }
    if (asx_actor_stop(actor) != ASX_OK) {
        scenario_line("actor.obligation_cleanup", 0, "stop_failed");
        return 0;
    }
    drive_region(region, 16u);
    if (asx_obligation_get_state(state.obligation, &os) != ASX_OK || os != ASX_OBLIGATION_ABORTED ||
        state.terminate_count != 1u) {
        scenario_line("actor.obligation_cleanup", 0, "abort_mismatch");
        return 0;
    }
    if (asx_runtime_snapshot_capture(&snap) != ASX_OK) {
        scenario_line("actor.obligation_cleanup", 0, "snapshot_failed");
        return 0;
    }
    for (i = 0u; i < snap.obligation_count; i++) {
        if (snap.obligations[i].state == ASX_OBLIGATION_RESERVED) { reserved++; }
    }
    if (reserved != 0u) {
        scenario_line("actor.obligation_cleanup", 0, "reserved_obligation_left");
        return 0;
    }

    scenario_line("actor.obligation_cleanup", 1, "reserved=0 aborted=1");
    *digest = mix_u64(*digest, (unsigned long long)os);
    *digest = mix_u64(*digest, snap.obligation_count);
    return 1;
}

int main(void) {
    unsigned long long digest = 0xcbf29ce484222325ULL;

    digest = record_config(digest);

    if (!run_actor_lifecycle(&digest)) return 1;
    if (!run_gen_server(&digest)) return 1;
    if (!run_supervisor_restart_policy(&digest)) return 1;
    if (!run_restart_intensity(&digest)) return 1;
    if (!run_cancel_during_restart(&digest)) return 1;
    if (!run_child_specs(&digest)) return 1;
    if (!run_obligation_cleanup(&digest)) return 1;

    printf("DIGEST %016llx\n", digest);
    return 0;
}
