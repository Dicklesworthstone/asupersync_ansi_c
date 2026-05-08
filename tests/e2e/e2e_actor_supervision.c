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

static int run_actor_lifecycle(unsigned long long *digest) {
    asx_actor_handle actor;
    asx_actor_behavior behavior = e2e_actor_behavior();
    asx_call_token token;
    e2e_actor_state state;
    asx_region_id region;
    uint64_t reply = 0u;

    asx_runtime_reset();
    memset(&state, 0, sizeof(state));
    region = make_region();
    if (region == ASX_INVALID_ID) {
        scenario_line("actor.lifecycle", 0, "region_open_failed");
        return 0;
    }
    state.region = region;

    if (asx_actor_spawn(&actor, region, &behavior, &state) != ASX_OK) {
        scenario_line("actor.lifecycle", 0, "spawn_failed");
        return 0;
    }
    drive_region(region, 4u);
    if (state.init_count != 1u || !asx_actor_is_alive(actor)) {
        scenario_line("actor.lifecycle", 0, "init_or_alive_mismatch");
        return 0;
    }

    if (asx_actor_cast(actor, 41u) != ASX_OK || asx_actor_call(actor, 35u, &token) != ASX_OK) {
        scenario_line("actor.lifecycle", 0, "send_failed");
        return 0;
    }
    drive_region(region, 8u);
    if (asx_call_token_poll(token, &reply) != ASX_OK || reply != 42u || state.cast_count != 1u ||
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
    *digest = mix_u64(*digest, reply);
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

static uint32_t g_fail_until_init;
static uint32_t g_init_count;
static uint32_t g_child_start_count;
static uint32_t g_stable_start_count;

static asx_status stable_cast(void *state, uint64_t msg, asx_actor_handle self) {
    (void)state;
    (void)msg;
    (void)self;
    return ASX_OK;
}

static asx_status stable_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    asx_actor_behavior behavior;
    (void)user_data;
    g_stable_start_count++;
    behavior.init = NULL;
    behavior.handle_cast = stable_cast;
    behavior.handle_call = NULL;
    behavior.terminate = NULL;
    return asx_actor_spawn(out, region, &behavior, NULL);
}

static asx_status failing_init(void *state, asx_actor_handle self) {
    (void)state;
    (void)self;
    g_init_count++;
    if (g_init_count <= g_fail_until_init) { return ASX_E_INVALID_STATE; }
    return ASX_OK;
}

static asx_status failing_start(void *user_data, asx_region_id region, asx_actor_handle *out) {
    asx_actor_behavior behavior;
    (void)user_data;
    g_child_start_count++;
    behavior.init = failing_init;
    behavior.handle_cast = stable_cast;
    behavior.handle_call = NULL;
    behavior.terminate = NULL;
    return asx_actor_spawn(out, region, &behavior, NULL);
}

static asx_supervisor_config supervisor_config(asx_supervisor_strategy strategy,
                                               uint32_t max_restarts) {
    asx_supervisor_config cfg;
    cfg.strategy = strategy;
    cfg.max_restarts = max_restarts;
    cfg.restart_window_ns = 0u;
    cfg.shutdown_budget_polls = 0u;
    return cfg;
}

static int run_supervisor_restart_policy(unsigned long long *digest) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[2];
    asx_child_spec temp;
    asx_region_id region;

    asx_runtime_reset();
    g_fail_until_init = 1u;
    g_init_count = 0u;
    g_child_start_count = 0u;
    g_stable_start_count = 0u;
    region = make_region();
    cfg = supervisor_config(ASX_SUPERVISOR_ONE_FOR_ONE, 5u);

    specs[0].start_fn = failing_start;
    specs[0].user_data = NULL;
    specs[0].restart = ASX_CHILD_PERMANENT;
    specs[1].start_fn = stable_start;
    specs[1].user_data = NULL;
    specs[1].restart = ASX_CHILD_PERMANENT;
    if (asx_supervisor_start(&sup, region, &cfg, specs, 2u) != ASX_OK) {
        scenario_line("supervisor.restart_policy", 0, "start_failed");
        return 0;
    }
    drive_region(region, 500u);
    if (!asx_supervisor_is_alive(sup) || g_child_start_count != 2u || g_stable_start_count != 1u ||
        asx_supervisor_restart_count(sup) != 1u || !asx_supervisor_child_alive(sup, 1u)) {
        scenario_line("supervisor.restart_policy", 0, "permanent_restart_mismatch");
        return 0;
    }
    (void)asx_supervisor_stop(sup);
    drive_region(region, 500u);

    asx_runtime_reset();
    g_fail_until_init = 999u;
    g_init_count = 0u;
    g_child_start_count = 0u;
    region = make_region();
    temp.start_fn = failing_start;
    temp.user_data = NULL;
    temp.restart = ASX_CHILD_TEMPORARY;
    if (asx_supervisor_start(&sup, region, &cfg, &temp, 1u) != ASX_OK) {
        scenario_line("supervisor.restart_policy", 0, "temporary_start_failed");
        return 0;
    }
    drive_region(region, 200u);
    if (!asx_supervisor_is_alive(sup) || g_child_start_count != 1u ||
        asx_supervisor_restart_count(sup) != 0u) {
        scenario_line("supervisor.restart_policy", 0, "temporary_restart_mismatch");
        return 0;
    }
    (void)asx_supervisor_stop(sup);
    drive_region(region, 200u);

    scenario_line("supervisor.restart_policy", 1, "permanent=restart temporary=no_restart");
    *digest = mix_u64(*digest, g_child_start_count);
    *digest = mix_u64(*digest, 1u);
    return 1;
}

static int run_restart_intensity(unsigned long long *digest) {
    asx_restart_intensity ri;
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec spec;
    asx_region_id region;

    asx_restart_intensity_init(&ri, 2u, 10u);
    if (asx_restart_intensity_record(&ri, 100u) || asx_restart_intensity_record(&ri, 105u) ||
        !asx_restart_intensity_record(&ri, 109u) || asx_restart_intensity_count(&ri, 125u) != 0u) {
        scenario_line("supervisor.restart_intensity", 0, "window_contract_failed");
        return 0;
    }

    asx_runtime_reset();
    g_fail_until_init = 999u;
    g_init_count = 0u;
    g_child_start_count = 0u;
    region = make_region();
    cfg = supervisor_config(ASX_SUPERVISOR_ONE_FOR_ONE, 2u);
    spec.start_fn = failing_start;
    spec.user_data = NULL;
    spec.restart = ASX_CHILD_PERMANENT;
    if (asx_supervisor_start(&sup, region, &cfg, &spec, 1u) != ASX_OK) {
        scenario_line("supervisor.restart_intensity", 0, "supervisor_start_failed");
        return 0;
    }
    drive_region(region, 1000u);
    if (asx_supervisor_is_alive(sup) || !asx_supervisor_intensity_exceeded(sup) ||
        asx_supervisor_exit_reason(sup) != ASX_E_RESOURCE_EXHAUSTED) {
        scenario_line("supervisor.restart_intensity", 0, "escalation_mismatch");
        return 0;
    }

    scenario_line("supervisor.restart_intensity", 1, "window=pass escalation=resource_exhausted");
    *digest = mix_u64(*digest, asx_supervisor_restart_count(sup));
    return 1;
}

static int run_cancel_during_restart(unsigned long long *digest) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec specs[2];
    asx_region_id region;
    uint32_t starts_before_stop;

    asx_runtime_reset();
    g_fail_until_init = 999u;
    g_init_count = 0u;
    g_child_start_count = 0u;
    g_stable_start_count = 0u;
    region = make_region();
    cfg = supervisor_config(ASX_SUPERVISOR_ONE_FOR_ONE, 5u);

    specs[0].start_fn = failing_start;
    specs[0].user_data = NULL;
    specs[0].restart = ASX_CHILD_PERMANENT;
    specs[1].start_fn = stable_start;
    specs[1].user_data = NULL;
    specs[1].restart = ASX_CHILD_PERMANENT;
    if (asx_supervisor_start(&sup, region, &cfg, specs, 2u) != ASX_OK) {
        scenario_line("supervisor.cancel_during_restart", 0, "start_failed");
        return 0;
    }

    drive_region(region, 4u);
    if (asx_supervisor_restart_count(sup) != 1u) {
        scenario_line("supervisor.cancel_during_restart", 0, "restart_not_pending");
        return 0;
    }
    starts_before_stop = g_child_start_count;
    if (asx_supervisor_stop(sup) != ASX_OK) {
        scenario_line("supervisor.cancel_during_restart", 0, "stop_failed");
        return 0;
    }
    drive_region(region, 500u);
    if (asx_supervisor_is_alive(sup) || g_child_start_count != starts_before_stop ||
        asx_supervisor_exit_reason(sup) != ASX_OK) {
        scenario_line("supervisor.cancel_during_restart", 0, "pending_restart_not_cancelled");
        return 0;
    }

    scenario_line("supervisor.cancel_during_restart", 1, "pending_restart_cancelled=1");
    *digest = mix_u64(*digest, starts_before_stop);
    return 1;
}

static int run_child_specs(unsigned long long *digest) {
    asx_supervisor_handle sup;
    asx_supervisor_config cfg;
    asx_child_spec_ext specs[2];
    asx_region_id region;

    asx_runtime_reset();
    region = make_region();
    cfg = supervisor_config(ASX_SUPERVISOR_ONE_FOR_ONE, 3u);
    asx_child_spec_ext_init(&specs[0], "alpha", stable_start, NULL);
    asx_child_spec_ext_init(&specs[1], "beta", stable_start, NULL);
    if (asx_child_spec_ext_depends_on(&specs[1], 0u) != ASX_OK ||
        asx_supervisor_start_ext(&sup, region, &cfg, specs, 2u) != ASX_OK) {
        scenario_line("supervisor.child_specs", 0, "start_ext_failed");
        return 0;
    }
    if (strcmp(asx_supervisor_child_name(sup, 0u), "alpha") != 0 ||
        strcmp(asx_supervisor_child_name(sup, 1u), "beta") != 0 ||
        asx_supervisor_child_count(sup) != 2u) {
        scenario_line("supervisor.child_specs", 0, "name_or_count_mismatch");
        return 0;
    }
    drive_region(region, 100u);
    if (!asx_supervisor_child_alive(sup, 0u) || !asx_supervisor_child_alive(sup, 1u)) {
        scenario_line("supervisor.child_specs", 0, "children_not_alive");
        return 0;
    }
    (void)asx_supervisor_stop(sup);
    drive_region(region, 200u);

    scenario_line("supervisor.child_specs", 1, "names=alpha,beta dep_recorded=1");
    *digest = mix_u64(*digest, asx_supervisor_child_count(sup));
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

    if (asx_actor_spawn(&actor, region, &behavior, &state) != ASX_OK) {
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
