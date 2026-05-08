/*
 * e2e_parallel_swarm.c -- large-swarm parallel runtime e2e scenarios
 *
 * Exercises short-lived task waves, cancellation waves, MPSC pressure,
 * equal-deadline timer bursts, blocking-work completion, and deterministic
 * admission pressure. Each scenario emits a machine-readable DETAIL record
 * plus the SCENARIO line consumed by harness.sh.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SMOKE_WORKERS 8u
#define SMOKE_WAVES 4u
#define LARGE_WAVES 32u
#define TIMER_SMOKE_TASKS 8u
#define TIMER_LARGE_TASKS 32u
#define BLOCKING_SMOKE_TASKS 8u
#define BLOCKING_LARGE_TASKS 16u

typedef struct {
    const char *scenario_id;
    int pass;
    const char *diagnostic;
    asx_status status;
    uint32_t worker_count;
    uint32_t task_count;
    uint32_t completed_tasks;
    uint32_t queue_depth;
    uint32_t lane_depth_ready;
    uint32_t lane_depth_cancel;
    uint32_t lane_depth_timed;
    uint32_t max_lane_depth;
    uint32_t max_worker_queue_depth;
    uint32_t pressure_pct;
    uint32_t blocking_backlog;
    uint32_t channel_queue_depth;
    uint32_t channel_reserved;
    uint32_t timer_ready;
    asx_scheduling_metrics metrics;
    asx_parallel_admission_decision admission;
    asx_parallel_telemetry_snapshot telemetry;
    int admission_seen;
    int telemetry_seen;
    uint64_t semantic_digest;
    uint64_t trace_digest;
    uint32_t trace_event_count;
} swarm_result;

typedef struct {
    uint32_t polls;
} pending_state;

static uint32_t g_seed = 42u;
static uint32_t g_worker_count = DEFAULT_SMOKE_WORKERS;
static const char *g_run_id = "unknown";
static const char *g_profile = "PARALLEL";
static const char *g_scale = "smoke";
static int g_large_scale = 0;
static uint64_t g_aggregate_digest = UINT64_C(1469598103934665603);
static uint32_t g_order_count = 0u;
static uint32_t g_order_values[ASX_MAX_TASKS];
static int g_pass = 0;
static int g_fail = 0;

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static uint32_t parse_u32_env(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') { return fallback; }
    parsed = strtoul(value, &end, 10);
    if (end == value || parsed > UINT32_MAX) { return fallback; }
    return (uint32_t)parsed;
}

static uint64_t digest_mix_byte(uint64_t digest, uint8_t byte) {
    digest ^= (uint64_t)byte;
    digest *= UINT64_C(1099511628211);
    return digest;
}

static uint64_t digest_mix_u32(uint64_t digest, uint32_t value) {
    uint32_t i;

    for (i = 0u; i < 4u; i++) {
        digest = digest_mix_byte(digest, (uint8_t)((value >> (i * 8u)) & 0xffu));
    }
    return digest;
}

static uint64_t digest_mix_u64(uint64_t digest, uint64_t value) {
    uint32_t i;

    for (i = 0u; i < 8u; i++) {
        digest = digest_mix_byte(digest, (uint8_t)((value >> (i * 8u)) & UINT64_C(0xff)));
    }
    return digest;
}

static uint64_t digest_mix_str(uint64_t digest, const char *value) {
    const unsigned char *p;

    if (value == NULL) { return digest_mix_byte(digest, 0u); }
    p = (const unsigned char *)value;
    while (*p != '\0') {
        digest = digest_mix_byte(digest, *p);
        p++;
    }
    return digest_mix_byte(digest, 0u);
}

static void json_string(const char *value) {
    const unsigned char *p;

    putchar('"');
    if (value != NULL) {
        p = (const unsigned char *)value;
        while (*p != '\0') {
            switch (*p) {
            case '\\': printf("\\\\"); break;
            case '"': printf("\\\""); break;
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            default:
                if (*p < 0x20u) {
                    putchar('?');
                } else {
                    putchar((int)*p);
                }
                break;
            }
            p++;
        }
    }
    putchar('"');
}

static const char *active_profile_name(void) {
#if defined(ASX_PROFILE_CORE)
    return "CORE";
#elif defined(ASX_PROFILE_POSIX)
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
    return "UNKNOWN";
#endif
}

static int should_run(const char *scenario_id) {
    const char *pack = getenv("ASX_E2E_SCENARIO_PACK");
    size_t n;

    if (pack == NULL || pack[0] == '\0' || strcmp(pack, "all") == 0) { return 1; }
    n = strlen(pack);
    return strncmp(scenario_id, pack, n) == 0;
}

static void reset_order(void) {
    uint32_t i;

    g_order_count = 0u;
    for (i = 0u; i < ASX_MAX_TASKS; i++) { g_order_values[i] = 0u; }
}

static void record_order(uint32_t value) {
    if (g_order_count < ASX_MAX_TASKS) {
        g_order_values[g_order_count] = value;
        g_order_count++;
    }
}

static asx_parallel_config default_parallel_config(uint32_t worker_count) {
    asx_parallel_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.worker_count = worker_count;
    cfg.fairness = ASX_FAIRNESS_ROUND_ROBIN;
    cfg.lane_weights[ASX_LANE_READY] = 1u;
    cfg.lane_weights[ASX_LANE_CANCEL] = 1u;
    cfg.lane_weights[ASX_LANE_TIMED] = 1u;
    cfg.starvation_limit = 8u;
    asx_parallel_admission_policy_init(&cfg.admission_policy);
    return cfg;
}

static asx_status reset_parallel(uint32_t worker_count) {
    asx_parallel_config cfg = default_parallel_config(worker_count);

    asx_runtime_reset();
    asx_trace_reset();
    reset_order();
    return asx_parallel_init(&cfg);
}

static void result_init(swarm_result *result, const char *scenario_id, uint32_t worker_count) {
    memset(result, 0, sizeof(*result));
    result->scenario_id = scenario_id;
    result->pass = 1;
    result->diagnostic = "";
    result->status = ASX_OK;
    result->worker_count = worker_count;
    result->semantic_digest = UINT64_C(1469598103934665603);
}

static void result_fail(swarm_result *result, const char *diagnostic, asx_status status) {
    if (result->pass) {
        result->pass = 0;
        result->diagnostic = diagnostic;
        result->status = status;
    }
}

static void add_metric_u32(uint32_t *target, uint32_t value) {
    if (UINT32_MAX - *target < value) {
        *target = UINT32_MAX;
    } else {
        *target += value;
    }
}

static void result_note_pressure(swarm_result *result,
                                 const asx_parallel_telemetry_snapshot *snapshot) {
    result->queue_depth = max_u32(result->queue_depth, snapshot->total_queue_depth);
    result->lane_depth_ready =
        max_u32(result->lane_depth_ready, snapshot->lane_depths[ASX_LANE_READY]);
    result->lane_depth_cancel =
        max_u32(result->lane_depth_cancel, snapshot->lane_depths[ASX_LANE_CANCEL]);
    result->lane_depth_timed =
        max_u32(result->lane_depth_timed, snapshot->lane_depths[ASX_LANE_TIMED]);
    result->max_lane_depth = max_u32(result->max_lane_depth, snapshot->max_lane_depth);
    result->max_worker_queue_depth =
        max_u32(result->max_worker_queue_depth, snapshot->max_worker_queue_depth);
    result->pressure_pct = max_u32(result->pressure_pct, snapshot->pressure_pct);
    result->blocking_backlog = max_u32(result->blocking_backlog, snapshot->blocking_backlog);
    if (snapshot->admission.triggered || !result->admission_seen) {
        result->admission = snapshot->admission;
        result->admission_seen = 1;
    }
    result->telemetry = *snapshot;
    result->telemetry_seen = 1;
}

static void result_add_metrics(swarm_result *result,
                               const asx_parallel_telemetry_snapshot *snapshot) {
    add_metric_u32(&result->metrics.cancel_dispatches, snapshot->metrics.cancel_dispatches);
    add_metric_u32(&result->metrics.timed_dispatches, snapshot->metrics.timed_dispatches);
    add_metric_u32(&result->metrics.ready_dispatches, snapshot->metrics.ready_dispatches);
    result->metrics.cancel_streak =
        max_u32(result->metrics.cancel_streak, snapshot->metrics.cancel_streak);
    result->metrics.cancel_streak_max =
        max_u32(result->metrics.cancel_streak_max, snapshot->metrics.cancel_streak_max);
    add_metric_u32(&result->metrics.fairness_yields, snapshot->metrics.fairness_yields);
    add_metric_u32(&result->metrics.steal_attempts, snapshot->metrics.steal_attempts);
    add_metric_u32(&result->metrics.steals_succeeded, snapshot->metrics.steals_succeeded);
    add_metric_u32(&result->metrics.steals_failed, snapshot->metrics.steals_failed);
    add_metric_u32(&result->metrics.worker_yields, snapshot->metrics.worker_yields);
    add_metric_u32(&result->metrics.commit_sequence, snapshot->metrics.commit_sequence);
    add_metric_u32(&result->metrics.reactor_polls, snapshot->metrics.reactor_polls);
    add_metric_u32(&result->metrics.reactor_ready, snapshot->metrics.reactor_ready);
    add_metric_u32(&result->metrics.waker_ready, snapshot->metrics.waker_ready);
    add_metric_u32(&result->metrics.timed_promotions, snapshot->metrics.timed_promotions);
    add_metric_u32(&result->metrics.timed_wake_latency_rounds_total,
                   snapshot->metrics.timed_wake_latency_rounds_total);
    result->metrics.timed_wake_latency_rounds_max =
        max_u32(result->metrics.timed_wake_latency_rounds_max,
                snapshot->metrics.timed_wake_latency_rounds_max);
    add_metric_u32(&result->metrics.admission_rejects, snapshot->metrics.admission_rejects);
    add_metric_u32(&result->metrics.admission_backpressure,
                   snapshot->metrics.admission_backpressure);
    add_metric_u32(&result->metrics.admission_sheds, snapshot->metrics.admission_sheds);
    add_metric_u32(&result->metrics.pressure_transitions, snapshot->metrics.pressure_transitions);
    result_note_pressure(result, snapshot);
}

static uint32_t completed_by_workers(uint32_t worker_count) {
    uint32_t i;
    uint32_t total = 0u;

    for (i = 0u; i < worker_count; i++) {
        asx_worker_state worker;
        if (asx_worker_get_state(i, &worker) == ASX_OK) { total += worker.tasks_completed; }
    }
    return total;
}

static void result_mix(swarm_result *result, uint32_t value) {
    result->semantic_digest = digest_mix_u32(result->semantic_digest, value);
}

static void result_mix_trace(swarm_result *result) {
    result->trace_digest = asx_trace_digest();
    result->trace_event_count = asx_trace_event_count();
    result->semantic_digest = digest_mix_u64(result->semantic_digest, result->trace_digest);
    result->semantic_digest = digest_mix_u32(result->semantic_digest, result->trace_event_count);
}

static void result_incident_snapshot(const swarm_result *result,
                                     asx_parallel_telemetry_snapshot *out) {
    if (result->telemetry_seen) {
        *out = result->telemetry;
    } else {
        memset(out, 0, sizeof(*out));
        out->commit_authority.native_live_status = ASX_E_PERMISSION_DENIED;
    }

    out->worker_count = result->worker_count;
    out->total_queue_depth = result->queue_depth;
    out->lane_depths[ASX_LANE_READY] = result->lane_depth_ready;
    out->lane_depths[ASX_LANE_CANCEL] = result->lane_depth_cancel;
    out->lane_depths[ASX_LANE_TIMED] = result->lane_depth_timed;
    out->max_lane_depth = result->max_lane_depth;
    out->max_worker_queue_depth = result->max_worker_queue_depth;
    out->pressure_pct = result->pressure_pct;
    out->blocking_backlog = result->blocking_backlog;
    out->metrics = result->metrics;
    out->admission = result->admission;
}

static void emit_incident_bundle_record(const swarm_result *result, const char *rerun) {
    asx_parallel_telemetry_snapshot telemetry;
    asx_incident_bundle bundle;
    asx_report_buf out;
    const char *message = "";

    result_incident_snapshot(result, &telemetry);
    asx_incident_bundle_init(&bundle);
    bundle.run_id = g_run_id;
    bundle.scenario_id = result->scenario_id;
    bundle.profile = g_profile;
    bundle.compiled_profile = active_profile_name();
    bundle.scale = g_scale;
    bundle.seed = g_seed;
    bundle.worker_count = result->worker_count;
    bundle.max_workers = (uint32_t)ASX_MAX_WORKERS;
    bundle.pass = result->pass;
    bundle.status = result->status;
    bundle.failure_class =
        asx_incident_bundle_failure_class(result->status,
                                          result->status == ASX_E_PERMISSION_DENIED);
    if (!result->pass && result->diagnostic != NULL && result->diagnostic[0] != '\0') {
        message = result->diagnostic;
    } else if (result->status == ASX_E_PERMISSION_DENIED) {
        message = "surface unsupported in compiled profile";
    }
    bundle.failure_message = message;
    bundle.semantic_digest = result->semantic_digest;
    bundle.trace_digest = result->trace_digest;
    bundle.trace_event_count = result->trace_event_count;
    bundle.replay_command = rerun;
    bundle.details_path = "parallel_swarm.details.jsonl";
    bundle.telemetry = &telemetry;

    if (asx_incident_bundle_render_json(&bundle, &out) == ASX_OK) {
        printf("INCIDENT %s\n", asx_report_buf_cstr(&out));
    }
}

static void emit_detail(const swarm_result *result) {
    char rerun[512];
    int written;

    written = snprintf(rerun, sizeof(rerun),
                       "ASX_E2E_SEED=%" PRIu32 " ASX_E2E_PROFILE=%s "
                       "ASX_E2E_PARALLEL_SCALE=%s ASX_E2E_PARALLEL_WORKERS=%" PRIu32
                       " ASX_E2E_SCENARIO_PACK=%s tests/e2e/parallel_swarm.sh",
                       g_seed, g_profile, g_scale, result->worker_count, result->scenario_id);
    if (written < 0 || (size_t)written >= sizeof(rerun)) {
        strcpy(rerun, "tests/e2e/parallel_swarm.sh");
    }

    printf("DETAIL {");
    printf("\"kind\":\"parallel_swarm_detail\",");
    printf("\"run_id\":");
    json_string(g_run_id);
    printf(",\"seed\":%" PRIu32, g_seed);
    printf(",\"profile\":");
    json_string(g_profile);
    printf(",\"compiled_profile\":");
    json_string(active_profile_name());
    printf(",\"scale\":");
    json_string(g_scale);
    printf(",\"scenario_id\":");
    json_string(result->scenario_id);
    printf(",\"status\":");
    json_string(result->pass ? "pass" : "fail");
    printf(",\"status_code\":%d", (int)result->status);
    printf(",\"status_text\":");
    json_string(asx_status_str(result->status));
    printf(",\"worker_count\":%" PRIu32, result->worker_count);
    printf(",\"max_workers\":%" PRIu32, (uint32_t)ASX_MAX_WORKERS);
    printf(",\"task_count\":%" PRIu32, result->task_count);
    printf(",\"completed_tasks\":%" PRIu32, result->completed_tasks);
    printf(",\"queue_depths\":{\"total\":%" PRIu32 ",\"ready\":%" PRIu32 ",\"cancel\":%" PRIu32
           ",\"timed\":%" PRIu32 ",\"channel\":%" PRIu32 ",\"channel_reserved\":%" PRIu32 "}",
           result->queue_depth, result->lane_depth_ready, result->lane_depth_cancel,
           result->lane_depth_timed, result->channel_queue_depth, result->channel_reserved);
    printf(",\"max_lane_depth\":%" PRIu32, result->max_lane_depth);
    printf(",\"max_worker_queue_depth\":%" PRIu32, result->max_worker_queue_depth);
    printf(",\"pressure_pct\":%" PRIu32, result->pressure_pct);
    printf(",\"blocking_backlog\":%" PRIu32, result->blocking_backlog);
    printf(",\"timer_ready\":%" PRIu32, result->timer_ready);
    printf(",\"metrics\":{\"ready_dispatches\":%" PRIu32 ",\"cancel_dispatches\":%" PRIu32
           ",\"timed_dispatches\":%" PRIu32 ",\"cancel_streak_max\":%" PRIu32
           ",\"fairness_yields\":%" PRIu32 ",\"steal_attempts\":%" PRIu32
           ",\"steals_succeeded\":%" PRIu32 ",\"steals_failed\":%" PRIu32
           ",\"worker_yields\":%" PRIu32 ",\"commit_sequence\":%" PRIu32
           ",\"reactor_ready\":%" PRIu32 ",\"waker_ready\":%" PRIu32
           ",\"timed_promotions\":%" PRIu32 ",\"timed_wake_latency_rounds_max\":%" PRIu32
           ",\"admission_rejects\":%" PRIu32 ",\"admission_backpressure\":%" PRIu32
           ",\"admission_sheds\":%" PRIu32 ",\"pressure_transitions\":%" PRIu32 "}",
           result->metrics.ready_dispatches, result->metrics.cancel_dispatches,
           result->metrics.timed_dispatches, result->metrics.cancel_streak_max,
           result->metrics.fairness_yields, result->metrics.steal_attempts,
           result->metrics.steals_succeeded, result->metrics.steals_failed,
           result->metrics.worker_yields, result->metrics.commit_sequence,
           result->metrics.reactor_ready, result->metrics.waker_ready,
           result->metrics.timed_promotions, result->metrics.timed_wake_latency_rounds_max,
           result->metrics.admission_rejects, result->metrics.admission_backpressure,
           result->metrics.admission_sheds, result->metrics.pressure_transitions);
    printf(",\"admission\":{\"triggered\":%d,\"mode\":", result->admission.triggered ? 1 : 0);
    json_string(asx_parallel_admission_mode_str(result->admission.mode));
    printf(",\"pressure_pct\":%" PRIu32 ",\"queued\":%" PRIu32 ",\"capacity\":%" PRIu32
           ",\"shed_count\":%" PRIu32 ",\"status_code\":%d,\"status_text\":",
           result->admission.pressure_pct, result->admission.queued, result->admission.capacity,
           result->admission.shed_count, (int)result->admission.admit_status);
    json_string(asx_status_str(result->admission.admit_status));
    printf("}");
    printf(",\"semantic_digest\":\"fnv64:%016" PRIx64 "\"", result->semantic_digest);
    printf(",\"trace_digest\":\"fnv64:%016" PRIx64 "\"", result->trace_digest);
    printf(",\"rerun\":");
    json_string(rerun);
    if (!result->pass && result->diagnostic != NULL && result->diagnostic[0] != '\0') {
        printf(",\"error\":{\"message\":");
        json_string(result->diagnostic);
        printf("}");
    }
    printf("}\n");

    emit_incident_bundle_record(result, rerun);

    printf("SCENARIO %s %s", result->scenario_id, result->pass ? "pass" : "fail");
    if (!result->pass && result->diagnostic != NULL && result->diagnostic[0] != '\0') {
        printf(" %s", result->diagnostic);
    }
    printf("\n");
}

static void finish_result(swarm_result *result) {
    result_mix(result, result->worker_count);
    result_mix(result, result->task_count);
    result_mix(result, result->completed_tasks);
    result_mix(result, result->queue_depth);
    result_mix(result, result->max_lane_depth);
    result_mix(result, result->max_worker_queue_depth);
    result_mix(result, result->pressure_pct);
    result_mix(result, result->channel_queue_depth);
    result_mix(result, result->channel_reserved);
    result_mix(result, result->timer_ready);
    result_mix(result, (uint32_t)result->status);
    result->semantic_digest = digest_mix_str(result->semantic_digest, result->scenario_id);
    g_aggregate_digest = digest_mix_u64(g_aggregate_digest, result->semantic_digest);
    emit_detail(result);
    if (result->pass) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_pending(void *data, asx_task_id self) {
    (void)self;
    if (data != NULL) { ((pending_state *)data)->polls++; }
    return ASX_E_PENDING;
}

static asx_status poll_cancel_aware(void *data, asx_task_id self) {
    pending_state *state = (pending_state *)data;
    asx_checkpoint_result cp;

    if (state != NULL) { state->polls++; }
    if (asx_checkpoint(self, &cp) == ASX_OK && cp.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

static asx_status poll_record_order(void *data, asx_task_id self) {
    uint32_t *value = (uint32_t *)data;
    (void)self;

    if (value != NULL) { record_order(*value); }
    return ASX_OK;
}

static uint64_t blocking_checksum_job(void *data) {
    uint64_t *value = (uint64_t *)data;

    if (value == NULL) { return UINT64_C(0); }
    return (*value * UINT64_C(37)) + UINT64_C(11);
}

static void scenario_short_lived_batches(void) {
    swarm_result result;
    uint32_t waves = g_large_scale ? LARGE_WAVES : SMOKE_WAVES;
    uint32_t per_wave = min_u32((uint32_t)ASX_MAX_TASKS, ASX_LANE_TASK_CAPACITY);
    uint32_t wave;

    result_init(&result, "parallel_swarm.short_lived_batches", g_worker_count);
    result.task_count = waves * per_wave;

    for (wave = 0u; wave < waves; wave++) {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;
        asx_parallel_telemetry_snapshot pre;
        asx_parallel_telemetry_snapshot post;
        uint32_t i;
        asx_status st;

        st = reset_parallel(g_worker_count);
        if (st != ASX_OK) {
            result_fail(&result, "parallel_init", st);
            break;
        }
        st = asx_region_open(&rid);
        if (st != ASX_OK) {
            result_fail(&result, "region_open", st);
            break;
        }
        for (i = 0u; i < per_wave; i++) {
            st = asx_task_spawn(rid, poll_complete, NULL, &tid);
            if (st != ASX_OK) {
                result_fail(&result, "task_spawn", st);
                break;
            }
            st = asx_inject_ready(tid);
            if (st != ASX_OK) {
                result_fail(&result, "inject_ready", st);
                break;
            }
        }
        if (!result.pass) { break; }
        if (asx_parallel_get_telemetry_snapshot(&pre) == ASX_OK) {
            result_note_pressure(&result, &pre);
        }
        budget = asx_budget_from_polls(per_wave * 4u);
        st = asx_parallel_run(rid, &budget);
        if (st != ASX_OK) {
            result_fail(&result, "parallel_run", st);
            break;
        }
        result.completed_tasks += completed_by_workers(g_worker_count);
        if (asx_parallel_get_telemetry_snapshot(&post) == ASX_OK) {
            result_add_metrics(&result, &post);
        }
        result_mix(&result, wave);
        result_mix_trace(&result);
    }

    finish_result(&result);
}

static void scenario_cancel_wave(void) {
    swarm_result result;
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    void *states[ASX_MAX_TASKS];
    asx_budget budget;
    asx_parallel_telemetry_snapshot pre;
    asx_parallel_telemetry_snapshot post;
    uint32_t count = g_large_scale ? (uint32_t)ASX_MAX_TASKS : 32u;
    uint32_t i;
    uint32_t cancelled;
    asx_status st;

    result_init(&result, "parallel_swarm.cancel_wave", g_worker_count);
    result.task_count = count;

    st = reset_parallel(g_worker_count);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_init", st);
        finish_result(&result);
        return;
    }
    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        result_fail(&result, "region_open", st);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < count; i++) {
        pending_state *state;
        st = asx_task_spawn_captured(rid, poll_cancel_aware, (uint32_t)sizeof(pending_state), NULL,
                                     &tids[i], &states[i]);
        if (st != ASX_OK) {
            result_fail(&result, "task_spawn_captured", st);
            finish_result(&result);
            return;
        }
        state = (pending_state *)states[i];
        state->polls = 0u;
        st = asx_inject_ready(tids[i]);
        if (st != ASX_OK) {
            result_fail(&result, "inject_ready", st);
            finish_result(&result);
            return;
        }
    }

    budget = asx_budget_from_polls(count);
    st = asx_parallel_run(rid, &budget);
    if (st != ASX_E_POLL_BUDGET_EXHAUSTED && st != ASX_OK) {
        result_fail(&result, "initial_parallel_run", st);
        finish_result(&result);
        return;
    }

    cancelled = asx_cancel_propagate(rid, ASX_CANCEL_SHUTDOWN);
    if (cancelled != count) {
        result_fail(&result, "cancel_propagate_count", ASX_E_INVALID_STATE);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < count; i++) {
        st = asx_inject_cancel(tids[i]);
        if (st != ASX_OK) {
            result_fail(&result, "inject_cancel", st);
            finish_result(&result);
            return;
        }
    }

    if (asx_parallel_get_telemetry_snapshot(&pre) == ASX_OK) {
        result_note_pressure(&result, &pre);
    }
    budget = asx_budget_from_polls(count * 6u);
    st = asx_parallel_run(rid, &budget);
    if (st != ASX_OK) {
        result_fail(&result, "cancel_drain", st);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < count; i++) {
        asx_task_state state;
        st = asx_task_get_state(tids[i], &state);
        if (st != ASX_OK || state != ASX_TASK_COMPLETED) {
            result_fail(&result, "cancel_task_state", st == ASX_OK ? ASX_E_INVALID_STATE : st);
            finish_result(&result);
            return;
        }
        result.completed_tasks++;
    }
    if (asx_parallel_get_telemetry_snapshot(&post) == ASX_OK) {
        result_add_metrics(&result, &post);
    }
    result_mix(&result, cancelled);
    result_mix_trace(&result);
    finish_result(&result);
}

static void scenario_mpsc_pressure(void) {
    swarm_result result;
    asx_region_id rid;
    asx_channel_id channel;
    asx_send_permit permit;
    asx_parallel_telemetry_snapshot pre;
    asx_parallel_telemetry_snapshot post;
    asx_budget budget;
    uint32_t capacity = g_large_scale ? ASX_CHANNEL_MAX_CAPACITY : 32u;
    uint32_t service_tasks = g_large_scale ? 16u : 8u;
    uint32_t i;
    uint32_t queue_len = 0u;
    uint32_t reserved = 0u;
    uint64_t checksum = UINT64_C(0);
    asx_status st;

    result_init(&result, "parallel_swarm.mpsc_pressure", g_worker_count);
    result.task_count = capacity + service_tasks;

    st = reset_parallel(g_worker_count);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_init", st);
        finish_result(&result);
        return;
    }
    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        result_fail(&result, "region_open", st);
        finish_result(&result);
        return;
    }
    st = asx_channel_create(rid, capacity, &channel);
    if (st != ASX_OK) {
        result_fail(&result, "channel_create", st);
        finish_result(&result);
        return;
    }

    for (i = 0u; i < capacity; i++) {
        st = asx_channel_try_reserve(channel, &permit);
        if (st != ASX_OK) {
            result_fail(&result, "channel_reserve", st);
            finish_result(&result);
            return;
        }
        st = asx_send_permit_send(&permit, ((uint64_t)g_seed << 16) ^ (uint64_t)i);
        if (st != ASX_OK) {
            result_fail(&result, "channel_send", st);
            finish_result(&result);
            return;
        }
    }
    st = asx_channel_try_reserve(channel, &permit);
    if (st != ASX_E_CHANNEL_FULL) {
        result_fail(&result, "channel_overflow", st);
        finish_result(&result);
        return;
    }
    st = asx_channel_queue_len(channel, &queue_len);
    if (st != ASX_OK) {
        result_fail(&result, "channel_queue_len", st);
        finish_result(&result);
        return;
    }
    st = asx_channel_reserved_count(channel, &reserved);
    if (st != ASX_OK) {
        result_fail(&result, "channel_reserved_count", st);
        finish_result(&result);
        return;
    }
    result.channel_queue_depth = queue_len;
    result.channel_reserved = reserved;

    for (i = 0u; i < service_tasks; i++) {
        asx_task_id tid;
        st = asx_task_spawn(rid, poll_complete, NULL, &tid);
        if (st != ASX_OK) {
            result_fail(&result, "service_task_spawn", st);
            finish_result(&result);
            return;
        }
        st = asx_inject_ready(tid);
        if (st != ASX_OK) {
            result_fail(&result, "service_task_inject", st);
            finish_result(&result);
            return;
        }
    }
    if (asx_parallel_get_telemetry_snapshot(&pre) == ASX_OK) {
        result_note_pressure(&result, &pre);
    }
    budget = asx_budget_from_polls(service_tasks * 4u);
    st = asx_parallel_run(rid, &budget);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_run", st);
        finish_result(&result);
        return;
    }
    result.completed_tasks = completed_by_workers(g_worker_count);
    for (i = 0u; i < capacity; i++) {
        uint64_t value;
        st = asx_channel_try_recv(channel, &value);
        if (st != ASX_OK) {
            result_fail(&result, "channel_recv", st);
            finish_result(&result);
            return;
        }
        checksum ^= digest_mix_u64(UINT64_C(1469598103934665603), value + (uint64_t)i);
    }
    if (asx_parallel_get_telemetry_snapshot(&post) == ASX_OK) {
        result_add_metrics(&result, &post);
    }
    result_mix(&result, queue_len);
    result_mix(&result, reserved);
    result.semantic_digest = digest_mix_u64(result.semantic_digest, checksum);
    result_mix_trace(&result);
    finish_result(&result);
}

static void scenario_timer_burst(void) {
    swarm_result result;
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    asx_waker wakers[ASX_MAX_TASKS];
    asx_timer_handle handles[ASX_MAX_TASKS];
    void *ready_wakers[ASX_MAX_TASKS];
    uint32_t order_values[ASX_MAX_TASKS];
    asx_parallel_telemetry_snapshot pre;
    asx_parallel_telemetry_snapshot post;
    asx_budget budget;
    uint32_t count = g_large_scale ? TIMER_LARGE_TASKS : TIMER_SMOKE_TASKS;
    uint32_t ready_count;
    uint32_t i;
    asx_status st;

    result_init(&result, "parallel_swarm.timer_equal_deadline_burst", g_worker_count);
    result.task_count = count;

    st = reset_parallel(g_worker_count);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_init", st);
        finish_result(&result);
        return;
    }
    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        result_fail(&result, "region_open", st);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < count; i++) {
        order_values[i] = i + 1u;
        st = asx_task_spawn(rid, poll_record_order, &order_values[i], &tids[i]);
        if (st != ASX_OK) {
            result_fail(&result, "timer_task_spawn", st);
            finish_result(&result);
            return;
        }
        st = asx_waker_register(tids[i], &wakers[i]);
        if (st != ASX_OK) {
            result_fail(&result, "waker_register", st);
            finish_result(&result);
            return;
        }
        st = asx_inject_timed(tids[i]);
        if (st != ASX_OK) {
            result_fail(&result, "inject_timed", st);
            finish_result(&result);
            return;
        }
        st = asx_timer_register(asx_timer_wheel_global(), 100u, &wakers[i], &handles[i]);
        if (st != ASX_OK) {
            result_fail(&result, "timer_register", st);
            finish_result(&result);
            return;
        }
    }
    if (asx_parallel_get_telemetry_snapshot(&pre) == ASX_OK) {
        result_note_pressure(&result, &pre);
    }
    ready_count = asx_timer_collect_expired(asx_timer_wheel_global(), 100u, ready_wakers, count);
    result.timer_ready = ready_count;
    if (ready_count != count) {
        result_fail(&result, "timer_ready_count", ASX_E_INVALID_STATE);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < ready_count; i++) {
        st = asx_waker_wake((const asx_waker *)ready_wakers[i]);
        if (st != ASX_OK) {
            result_fail(&result, "waker_wake", st);
            finish_result(&result);
            return;
        }
    }
    budget = asx_budget_from_polls(count * 4u);
    st = asx_parallel_run(rid, &budget);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_run", st);
        finish_result(&result);
        return;
    }
    result.completed_tasks = completed_by_workers(g_worker_count);
    if (asx_parallel_get_telemetry_snapshot(&post) == ASX_OK) {
        result_add_metrics(&result, &post);
    }
    result_mix(&result, ready_count);
    result_mix(&result, g_order_count);
    for (i = 0u; i < g_order_count; i++) { result_mix(&result, g_order_values[i]); }
    result_mix_trace(&result);
    finish_result(&result);
}

static void scenario_blocking_storm(void) {
    swarm_result result;
#if ASX_HAS_BLOCKING_SURFACE
    asx_region_id rid;
    asx_task_id tids[BLOCKING_LARGE_TASKS];
    asx_waker wakers[BLOCKING_LARGE_TASKS];
    asx_blocking_handle handles[BLOCKING_LARGE_TASKS];
    uint32_t order_values[BLOCKING_LARGE_TASKS];
    uint64_t inputs[BLOCKING_LARGE_TASKS];
    asx_parallel_telemetry_snapshot pre;
    asx_parallel_telemetry_snapshot post;
    asx_budget budget;
    uint32_t count = g_large_scale ? BLOCKING_LARGE_TASKS : BLOCKING_SMOKE_TASKS;
    uint64_t result_checksum = UINT64_C(0);
    uint32_t i;
    asx_status st;
#endif

    result_init(&result, "parallel_swarm.blocking_storm", g_worker_count);
    result.task_count = g_large_scale ? BLOCKING_LARGE_TASKS : BLOCKING_SMOKE_TASKS;

#if ASX_HAS_BLOCKING_SURFACE
    st = reset_parallel(g_worker_count);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_init", st);
        finish_result(&result);
        return;
    }
    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        result_fail(&result, "region_open", st);
        finish_result(&result);
        return;
    }
    st = asx_blocking_pool_init();
    if (st != ASX_OK) {
        result_fail(&result, "blocking_pool_init", st);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < count; i++) {
        order_values[i] = 100u + i;
        inputs[i] = ((uint64_t)g_seed << 8) + (uint64_t)i;
        st = asx_task_spawn(rid, poll_record_order, &order_values[i], &tids[i]);
        if (st != ASX_OK) {
            result_fail(&result, "blocking_task_spawn", st);
            finish_result(&result);
            return;
        }
        st = asx_waker_register(tids[i], &wakers[i]);
        if (st != ASX_OK) {
            result_fail(&result, "blocking_waker_register", st);
            finish_result(&result);
            return;
        }
        st = asx_inject_timed(tids[i]);
        if (st != ASX_OK) {
            result_fail(&result, "blocking_inject_timed", st);
            finish_result(&result);
            return;
        }
        st = asx_spawn_blocking(blocking_checksum_job, &inputs[i], &wakers[i], &handles[i]);
        if (st != ASX_OK) {
            result_fail(&result, "spawn_blocking", st);
            finish_result(&result);
            return;
        }
        {
            uint64_t blocking_result = 0u;
            st = asx_blocking_get_result(&handles[i], &blocking_result);
            if (st != ASX_OK) {
                result_fail(&result, "blocking_result", st);
                finish_result(&result);
                return;
            }
            result_checksum ^= digest_mix_u64(UINT64_C(1469598103934665603), blocking_result);
        }
    }
    if (asx_parallel_get_telemetry_snapshot(&pre) == ASX_OK) {
        result_note_pressure(&result, &pre);
    }
    budget = asx_budget_from_polls(count * 4u);
    st = asx_parallel_run(rid, &budget);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_run", st);
        finish_result(&result);
        return;
    }
    result.completed_tasks = completed_by_workers(g_worker_count);
    if (asx_parallel_get_telemetry_snapshot(&post) == ASX_OK) {
        result_add_metrics(&result, &post);
    }
    result.semantic_digest = digest_mix_u64(result.semantic_digest, result_checksum);
    result_mix(&result, g_order_count);
    result_mix_trace(&result);
#else
    result.completed_tasks = 0u;
    result.status = ASX_E_PERMISSION_DENIED;
    result_mix(&result, result.status);
#endif
    finish_result(&result);
}

static void scenario_admission_pressure(void) {
    swarm_result result;
    asx_region_id rid;
    asx_task_id tid;
    pending_state states[ASX_MAX_TASKS];
    asx_parallel_config cfg = default_parallel_config(g_worker_count);
    asx_parallel_telemetry_snapshot pre;
    asx_parallel_admission_policy enforced;
    asx_parallel_admission_decision decision;
    uint32_t i;
    asx_status st;

    result_init(&result, "parallel_swarm.admission_pressure", g_worker_count);
    result.task_count = ASX_LANE_TASK_CAPACITY;

    cfg.admission_policy.mode = ASX_PARALLEL_ADMISSION_REJECT;
    cfg.admission_policy.pressure_threshold_pct = 80u;
    cfg.admission_policy.enforce = 0;
    asx_runtime_reset();
    asx_trace_reset();
    reset_order();
    st = asx_parallel_init(&cfg);
    if (st != ASX_OK) {
        result_fail(&result, "parallel_init", st);
        finish_result(&result);
        return;
    }
    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        result_fail(&result, "region_open", st);
        finish_result(&result);
        return;
    }
    for (i = 0u; i < ASX_LANE_TASK_CAPACITY; i++) {
        states[i].polls = 0u;
        st = asx_task_spawn(rid, poll_pending, &states[i], &tid);
        if (st != ASX_OK) {
            result_fail(&result, "pressure_task_spawn", st);
            finish_result(&result);
            return;
        }
        st = asx_inject_ready(tid);
        if (st != ASX_OK) {
            result_fail(&result, "pressure_inject_ready", st);
            finish_result(&result);
            return;
        }
    }
    if (asx_parallel_get_telemetry_snapshot(&pre) == ASX_OK) {
        result_note_pressure(&result, &pre);
        result_add_metrics(&result, &pre);
    }

    enforced = cfg.admission_policy;
    enforced.enforce = 1;
    st = asx_parallel_evaluate_admission(&enforced, ASX_LANE_TASK_CAPACITY, ASX_LANE_TASK_CAPACITY,
                                         &decision);
    if (st != ASX_OK) {
        result_fail(&result, "evaluate_admission", st);
        finish_result(&result);
        return;
    }
    result.admission = decision;
    result.admission_seen = 1;
    if (!decision.triggered || decision.admit_status != ASX_E_ADMISSION_CLOSED) {
        result_fail(&result, "admission_decision", ASX_E_INVALID_STATE);
        finish_result(&result);
        return;
    }
    result_mix(&result, decision.pressure_pct);
    result_mix(&result, (uint32_t)decision.admit_status);
    result_mix_trace(&result);
    finish_result(&result);
}

static void configure_from_env(void) {
    g_run_id = getenv("ASX_E2E_RUN_ID");
    if (g_run_id == NULL || g_run_id[0] == '\0') { g_run_id = "unknown"; }

    g_profile = getenv("ASX_E2E_PROFILE");
    if (g_profile == NULL || g_profile[0] == '\0') { g_profile = active_profile_name(); }

    g_scale = getenv("ASX_E2E_PARALLEL_SCALE");
    if (g_scale == NULL || g_scale[0] == '\0') { g_scale = "smoke"; }
    g_large_scale = (strcmp(g_scale, "large") == 0 || strcmp(g_scale, "nightly") == 0 ||
                     strcmp(g_scale, "profile") == 0);

    g_seed = parse_u32_env("ASX_E2E_SEED", 42u);
    g_worker_count = parse_u32_env("ASX_E2E_PARALLEL_WORKERS", DEFAULT_SMOKE_WORKERS);
    if (g_worker_count == 0u) { g_worker_count = 1u; }
    if (g_worker_count > ASX_MAX_WORKERS) { g_worker_count = ASX_MAX_WORKERS; }
}

int main(void) {
    uint32_t executed = 0u;

    configure_from_env();
    g_aggregate_digest = digest_mix_str(g_aggregate_digest, g_run_id);
    g_aggregate_digest = digest_mix_u32(g_aggregate_digest, g_seed);
    g_aggregate_digest = digest_mix_u32(g_aggregate_digest, g_worker_count);

    if (should_run("parallel_swarm.short_lived_batches")) {
        scenario_short_lived_batches();
        executed++;
    }
    if (should_run("parallel_swarm.cancel_wave")) {
        scenario_cancel_wave();
        executed++;
    }
    if (should_run("parallel_swarm.mpsc_pressure")) {
        scenario_mpsc_pressure();
        executed++;
    }
    if (should_run("parallel_swarm.timer_equal_deadline_burst")) {
        scenario_timer_burst();
        executed++;
    }
    if (should_run("parallel_swarm.blocking_storm")) {
        scenario_blocking_storm();
        executed++;
    }
    if (should_run("parallel_swarm.admission_pressure")) {
        scenario_admission_pressure();
        executed++;
    }

    if (executed == 0u) {
        printf("DETAIL {\"kind\":\"parallel_swarm_detail\",\"run_id\":");
        json_string(g_run_id);
        printf(",\"scenario_id\":\"parallel_swarm.no_match\",\"status\":\"fail\","
               "\"error\":{\"message\":\"scenario pack matched no parallel swarm scenarios\"}}\n");
        printf("SCENARIO parallel_swarm.no_match fail scenario pack matched no scenarios\n");
        g_fail++;
    }

    (void)g_pass;
    printf("DIGEST %016" PRIx64 "\n", g_aggregate_digest);
    return g_fail == 0 ? 0 : 1;
}
