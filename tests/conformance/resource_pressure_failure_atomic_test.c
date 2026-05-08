/*
 * resource_pressure_failure_atomic_test.c — deterministic resource-pressure
 * scenario pack with machine-readable failure-atomic evidence.
 *
 * The same binary runs as a normal C conformance test and, when
 * ASX_RESOURCE_PRESSURE_REPORT_JSONL is set, appends per-scenario JSONL records
 * for profile/resource-class parity gates.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/core/resource.h>
#include <asx/runtime/snapshot.h>
#include <asx/runtime/trace.h>
#include <asx/time/timer_wheel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pressure_metrics {
    uint32_t regions_used;
    uint32_t tasks_used;
    uint32_t obligations_used;
    uint32_t channel_queue_len;
    uint32_t channel_reserved_count;
    uint32_t timer_active_count;
    uint32_t scheduler_event_count;
    uint32_t cancel_forced_event_count;
    uint32_t final_state_code;
} pressure_metrics;

typedef struct pressure_case_result {
    const char *scenario_id;
    const char *surface;
    asx_status expected_status;
    asx_status actual_status;
    uint32_t failure_atomic_expected;
    uint32_t failure_atomic_observed;
    uint32_t semantic_final_code;
    uint64_t semantic_digest;
    pressure_metrics before;
    pressure_metrics after;
    const char *diagnostic;
    int passed;
} pressure_case_result;

#define FINAL_STATE_UNSET 0u
#define FINAL_STATE_NO_MUTATION 1u
#define FINAL_STATE_FIFO_INTACT 2u
#define FINAL_STATE_TIMER_INTACT 3u
#define FINAL_STATE_BUDGET_EXHAUSTED_DETERMINISTIC 4u
#define FINAL_STATE_CANCEL_FORCED_COMPLETED 5u

static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_checkpoint_forever(void *data, asx_task_id self) {
    asx_checkpoint_result checkpoint;
    asx_status status;

    (void)data;
    status = asx_checkpoint(self, &checkpoint);
    (void)status;
    return ASX_E_PENDING;
}

static void reset_all(void) {
    asx_runtime_reset();
    asx_channel_reset();
    asx_trace_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
}

static void metrics_capture(pressure_metrics *metrics) {
    if (metrics == NULL) return;
    memset(metrics, 0, sizeof(*metrics));
    metrics->regions_used = asx_resource_used(ASX_RESOURCE_REGION);
    metrics->tasks_used = asx_resource_used(ASX_RESOURCE_TASK);
    metrics->obligations_used = asx_resource_used(ASX_RESOURCE_OBLIGATION);
    metrics->timer_active_count = asx_timer_active_count(asx_timer_wheel_global());
    metrics->scheduler_event_count = asx_scheduler_event_count();
}

static uint64_t digest_mix_byte(uint64_t digest, uint8_t byte) {
    digest ^= (uint64_t)byte;
    digest *= UINT64_C(1099511628211);
    return digest;
}

static uint64_t digest_mix_u32(uint64_t digest, uint32_t value) {
    uint32_t shift;

    for (shift = 0u; shift < 32u; shift += 8u) {
        digest = digest_mix_byte(digest, (uint8_t)((value >> shift) & 0xffu));
    }
    return digest;
}

static uint64_t digest_mix_str(uint64_t digest, const char *text) {
    const unsigned char *cursor;

    if (text == NULL) return digest_mix_u32(digest, 0u);
    cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        digest = digest_mix_byte(digest, *cursor);
        cursor++;
    }
    return digest_mix_byte(digest, 0u);
}

static uint64_t pressure_case_digest(const pressure_case_result *result) {
    uint64_t digest = UINT64_C(1469598103934665603);

    digest = digest_mix_str(digest, result->scenario_id);
    digest = digest_mix_str(digest, result->surface);
    digest = digest_mix_u32(digest, (uint32_t)result->expected_status);
    digest = digest_mix_u32(digest, (uint32_t)result->actual_status);
    digest = digest_mix_u32(digest, result->failure_atomic_observed);
    digest = digest_mix_u32(digest, result->semantic_final_code);
    digest = digest_mix_u32(digest, result->passed ? 1u : 0u);
    return digest;
}

static const char *compiled_profile_name(void) {
#if defined(ASX_PROFILE_CORE)
    return "ASX_PROFILE_CORE";
#elif defined(ASX_PROFILE_POSIX)
    return "ASX_PROFILE_POSIX";
#elif defined(ASX_PROFILE_WIN32)
    return "ASX_PROFILE_WIN32";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "ASX_PROFILE_FREESTANDING";
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "ASX_PROFILE_EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_HFT)
    return "ASX_PROFILE_HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "ASX_PROFILE_AUTOMOTIVE";
#elif defined(ASX_PROFILE_PARALLEL)
    return "ASX_PROFILE_PARALLEL";
#elif defined(ASX_PROFILE_BROWSER)
    return "ASX_PROFILE_BROWSER";
#else
    return "ASX_PROFILE_UNKNOWN";
#endif
}

static asx_resource_class report_resource_class(void) {
    const char *value = getenv("ASX_RESOURCE_PRESSURE_RESOURCE_CLASS");

    if (value == NULL) return ASX_CLASS_R3;
    if (strcmp(value, "R1") == 0 || strcmp(value, "ASX_CLASS_R1") == 0) return ASX_CLASS_R1;
    if (strcmp(value, "R2") == 0 || strcmp(value, "ASX_CLASS_R2") == 0) return ASX_CLASS_R2;
    if (strcmp(value, "R3") == 0 || strcmp(value, "ASX_CLASS_R3") == 0) return ASX_CLASS_R3;
    return ASX_CLASS_R3;
}

static const char *report_profile_name(void) {
    const char *value = getenv("ASX_RESOURCE_PRESSURE_PROFILE");
    return (value != NULL && value[0] != '\0') ? value : compiled_profile_name();
}

static const char *report_lane_name(void) {
    const char *value = getenv("ASX_RESOURCE_PRESSURE_LANE");
    return (value != NULL && value[0] != '\0') ? value : "default";
}

static void json_string(FILE *stream, const char *text) {
    const unsigned char *cursor;

    fputc('"', stream);
    if (text != NULL) {
        cursor = (const unsigned char *)text;
        while (*cursor != '\0') {
            if (*cursor == '"' || *cursor == '\\') {
                fputc('\\', stream);
                fputc((int)*cursor, stream);
            } else if (*cursor == '\n') {
                fputs("\\n", stream);
            } else {
                fputc((int)*cursor, stream);
            }
            cursor++;
        }
    }
    fputc('"', stream);
}

static void json_metrics(FILE *stream, const pressure_metrics *metrics) {
    fprintf(stream,
            "{\"regions_used\":%u,\"tasks_used\":%u,\"obligations_used\":%u,"
            "\"channel_queue_len\":%u,\"channel_reserved_count\":%u,"
            "\"timer_active_count\":%u,\"scheduler_event_count\":%u,"
            "\"cancel_forced_event_count\":%u,\"final_state_code\":%u}",
            (unsigned)metrics->regions_used, (unsigned)metrics->tasks_used,
            (unsigned)metrics->obligations_used, (unsigned)metrics->channel_queue_len,
            (unsigned)metrics->channel_reserved_count, (unsigned)metrics->timer_active_count,
            (unsigned)metrics->scheduler_event_count, (unsigned)metrics->cancel_forced_event_count,
            (unsigned)metrics->final_state_code);
}

static void emit_report_record(const pressure_case_result *result) {
    const char *path = getenv("ASX_RESOURCE_PRESSURE_REPORT_JSONL");
    FILE *stream;
    asx_resource_class resource_class = report_resource_class();
    asx_resource_limits limits = asx_resource_limits_for_class(resource_class);

    if (path == NULL || path[0] == '\0') return;
    stream = fopen(path, "a");
    if (stream == NULL) {
        fprintf(stderr, "[resource-pressure] unable to append report: %s\n", path);
        return;
    }

    fputs("{\"schema\":\"asx.resource_pressure.case.v1\"", stream);
    fputs(",\"lane\":", stream);
    json_string(stream, report_lane_name());
    fputs(",\"profile\":", stream);
    json_string(stream, report_profile_name());
    fputs(",\"compiled_profile\":", stream);
    json_string(stream, compiled_profile_name());
    fputs(",\"resource_class\":", stream);
    json_string(stream, asx_resource_class_name(resource_class));
    fprintf(stream,
            ",\"resource_limits\":{\"max_regions\":%u,\"max_tasks\":%u,"
            "\"max_timers\":%u,\"max_obligations\":%u,\"max_channels\":%u,"
            "\"max_trace_events\":%u}",
            (unsigned)limits.max_regions, (unsigned)limits.max_tasks, (unsigned)limits.max_timers,
            (unsigned)limits.max_obligations, (unsigned)limits.max_channels,
            (unsigned)limits.max_trace_events);
    fputs(",\"scenario_id\":", stream);
    json_string(stream, result->scenario_id);
    fputs(",\"surface\":", stream);
    json_string(stream, result->surface);
    fprintf(stream, ",\"status\":\"%s\"", result->passed ? "pass" : "fail");
    fprintf(stream, ",\"expected_status\":{\"code\":%d,\"name\":", (int)result->expected_status);
    json_string(stream, asx_status_str(result->expected_status));
    fprintf(stream, "},\"actual_status\":{\"code\":%d,\"name\":", (int)result->actual_status);
    json_string(stream, asx_status_str(result->actual_status));
    fputs("}", stream);
    fprintf(stream, ",\"failure_atomic_expected\":%s",
            result->failure_atomic_expected ? "true" : "false");
    fprintf(stream, ",\"failure_atomic_observed\":%s",
            result->failure_atomic_observed ? "true" : "false");
    fprintf(stream, ",\"semantic_final_code\":%u", (unsigned)result->semantic_final_code);
    fprintf(stream, ",\"semantic_digest\":\"fnv64:%016llx\"",
            (unsigned long long)result->semantic_digest);
    fputs(",\"before\":", stream);
    json_metrics(stream, &result->before);
    fputs(",\"after\":", stream);
    json_metrics(stream, &result->after);
    fputs(",\"diagnostic\":", stream);
    json_string(stream, result->diagnostic);
    fputs("}\n", stream);
    fclose(stream);
}

static void finalize_result(pressure_case_result *result) {
    if (result->diagnostic == NULL) result->diagnostic = "";
    result->semantic_digest = pressure_case_digest(result);
    emit_report_record(result);
    fprintf(stderr, "  %s: %s\n", result->passed ? "PASS" : "FAIL", result->scenario_id);
    if (!result->passed && result->diagnostic[0] != '\0') {
        fprintf(stderr, "    %s\n", result->diagnostic);
    }
}

static int snapshots_equal(const asx_runtime_snapshot *before, const asx_runtime_snapshot *after) {
    return asx_runtime_snapshot_eq(before, after) == ASX_OK;
}

static void scenario_region_create_exhaustion(pressure_case_result *result) {
    asx_region_id regions[ASX_MAX_REGIONS];
    asx_region_id overflow = ASX_INVALID_ID;
    asx_runtime_snapshot before_snapshot;
    asx_runtime_snapshot after_snapshot;
    asx_region_state state;
    uint32_t index;

    result->scenario_id = "rp-region-create-exhaustion-001";
    result->surface = "region_create";
    result->expected_status = ASX_E_RESOURCE_EXHAUSTED;
    result->failure_atomic_expected = 1u;
    reset_all();

    for (index = 0u; index < (uint32_t)ASX_MAX_REGIONS; index++) {
        if (asx_region_open(&regions[index]) != ASX_OK) {
            result->diagnostic = "failed to fill region arena";
            goto finish;
        }
    }
    metrics_capture(&result->before);
    if (asx_runtime_snapshot_capture(&before_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture before snapshot";
        goto finish;
    }

    result->actual_status = asx_region_open(&overflow);

    metrics_capture(&result->after);
    if (asx_runtime_snapshot_capture(&after_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture after snapshot";
        goto finish;
    }
    result->failure_atomic_observed = snapshots_equal(&before_snapshot, &after_snapshot) ? 1u : 0u;
    result->semantic_final_code = FINAL_STATE_NO_MUTATION;
    result->after.final_state_code = result->semantic_final_code;

    if (asx_region_get_state(regions[0], &state) != ASX_OK || state != ASX_REGION_OPEN) {
        result->diagnostic = "existing region was not queryable after overflow";
        goto finish;
    }
    result->passed =
        (result->actual_status == result->expected_status && result->failure_atomic_observed == 1u);

finish:
    finalize_result(result);
}

static void scenario_task_spawn_exhaustion(pressure_case_result *result) {
    asx_region_id region;
    asx_task_id task = ASX_INVALID_ID;
    asx_task_id last_task = ASX_INVALID_ID;
    asx_runtime_snapshot before_snapshot;
    asx_runtime_snapshot after_snapshot;
    asx_task_state task_state;
    uint32_t index;

    result->scenario_id = "rp-task-spawn-exhaustion-002";
    result->surface = "task_spawn";
    result->expected_status = ASX_E_RESOURCE_EXHAUSTED;
    result->failure_atomic_expected = 1u;
    reset_all();

    if (asx_region_open(&region) != ASX_OK) {
        result->diagnostic = "failed to open region";
        goto finish;
    }
    for (index = 0u; index < (uint32_t)ASX_MAX_TASKS; index++) {
        if (asx_task_spawn(region, poll_pending, NULL, &task) != ASX_OK) {
            result->diagnostic = "failed to fill task arena";
            goto finish;
        }
        last_task = task;
    }
    metrics_capture(&result->before);
    if (asx_runtime_snapshot_capture(&before_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture before snapshot";
        goto finish;
    }

    result->actual_status = asx_task_spawn(region, poll_complete, NULL, &task);

    metrics_capture(&result->after);
    if (asx_runtime_snapshot_capture(&after_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture after snapshot";
        goto finish;
    }
    result->failure_atomic_observed = snapshots_equal(&before_snapshot, &after_snapshot) ? 1u : 0u;
    result->semantic_final_code = FINAL_STATE_NO_MUTATION;
    result->after.final_state_code = result->semantic_final_code;

    if (asx_task_get_state(last_task, &task_state) != ASX_OK || task_state != ASX_TASK_CREATED) {
        result->diagnostic = "last valid task changed after failed spawn";
        goto finish;
    }
    result->passed =
        (result->actual_status == result->expected_status && result->failure_atomic_observed == 1u);

finish:
    finalize_result(result);
}

static void scenario_obligation_reserve_exhaustion(pressure_case_result *result) {
    asx_region_id region;
    asx_obligation_id obligations[ASX_MAX_OBLIGATIONS];
    asx_obligation_id overflow = ASX_INVALID_ID;
    asx_runtime_snapshot before_snapshot;
    asx_runtime_snapshot after_snapshot;
    asx_obligation_state state;
    uint32_t index;

    result->scenario_id = "rp-obligation-reserve-exhaustion-003";
    result->surface = "obligation_reserve";
    result->expected_status = ASX_E_RESOURCE_EXHAUSTED;
    result->failure_atomic_expected = 1u;
    reset_all();

    if (asx_region_open(&region) != ASX_OK) {
        result->diagnostic = "failed to open region";
        goto finish;
    }
    for (index = 0u; index < (uint32_t)ASX_MAX_OBLIGATIONS; index++) {
        if (asx_obligation_reserve(region, &obligations[index]) != ASX_OK) {
            result->diagnostic = "failed to fill obligation arena";
            goto finish;
        }
    }
    metrics_capture(&result->before);
    if (asx_runtime_snapshot_capture(&before_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture before snapshot";
        goto finish;
    }

    result->actual_status = asx_obligation_reserve(region, &overflow);

    metrics_capture(&result->after);
    if (asx_runtime_snapshot_capture(&after_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture after snapshot";
        goto finish;
    }
    result->failure_atomic_observed = snapshots_equal(&before_snapshot, &after_snapshot) ? 1u : 0u;
    result->semantic_final_code = FINAL_STATE_NO_MUTATION;
    result->after.final_state_code = result->semantic_final_code;

    if (asx_obligation_get_state(obligations[0], &state) != ASX_OK ||
        state != ASX_OBLIGATION_RESERVED) {
        result->diagnostic = "first obligation changed after failed reserve";
        goto finish;
    }
    result->passed =
        (result->actual_status == result->expected_status && result->failure_atomic_observed == 1u);

finish:
    finalize_result(result);
}

static void scenario_channel_enqueue_exhaustion(pressure_case_result *result) {
    asx_region_id region;
    asx_channel_id channel;
    asx_send_permit permit;
    asx_send_permit overflow_permit;
    asx_runtime_snapshot before_snapshot;
    asx_runtime_snapshot after_snapshot;
    uint64_t value = 0u;
    uint32_t index;

    result->scenario_id = "rp-channel-enqueue-exhaustion-004";
    result->surface = "channel_enqueue";
    result->expected_status = ASX_E_CHANNEL_FULL;
    result->failure_atomic_expected = 1u;
    reset_all();

    if (asx_region_open(&region) != ASX_OK || asx_channel_create(region, 4u, &channel) != ASX_OK) {
        result->diagnostic = "failed to create bounded channel";
        goto finish;
    }
    for (index = 0u; index < 4u; index++) {
        if (asx_channel_try_reserve(channel, &permit) != ASX_OK ||
            asx_send_permit_send(&permit, (uint64_t)(index + 1u)) != ASX_OK) {
            result->diagnostic = "failed to fill channel";
            goto finish;
        }
    }
    metrics_capture(&result->before);
    if (asx_channel_queue_len(channel, &result->before.channel_queue_len) != ASX_OK ||
        asx_channel_reserved_count(channel, &result->before.channel_reserved_count) != ASX_OK ||
        asx_runtime_snapshot_capture(&before_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture channel before metrics";
        goto finish;
    }

    result->actual_status = asx_channel_try_reserve(channel, &overflow_permit);

    metrics_capture(&result->after);
    if (asx_channel_queue_len(channel, &result->after.channel_queue_len) != ASX_OK ||
        asx_channel_reserved_count(channel, &result->after.channel_reserved_count) != ASX_OK ||
        asx_runtime_snapshot_capture(&after_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture channel after metrics";
        goto finish;
    }
    result->failure_atomic_observed =
        (snapshots_equal(&before_snapshot, &after_snapshot) &&
         result->before.channel_queue_len == result->after.channel_queue_len &&
         result->before.channel_reserved_count == result->after.channel_reserved_count)
            ? 1u
            : 0u;
    result->semantic_final_code = FINAL_STATE_FIFO_INTACT;
    result->after.final_state_code = result->semantic_final_code;

    if (asx_channel_try_recv(channel, &value) != ASX_OK || value != 1u) {
        result->diagnostic = "channel FIFO was not intact after failed reserve";
        goto finish;
    }
    result->passed =
        (result->actual_status == result->expected_status && result->failure_atomic_observed == 1u);

finish:
    finalize_result(result);
}

static void scenario_timer_allocation_exhaustion(pressure_case_result *result) {
    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_handle handles[ASX_MAX_TIMERS];
    asx_timer_handle overflow_handle;
    asx_runtime_snapshot before_snapshot;
    asx_runtime_snapshot after_snapshot;
    void *wakers[1];
    uint32_t index;
    uint32_t fired;

    result->scenario_id = "rp-timer-allocation-exhaustion-005";
    result->surface = "timer_allocation";
    result->expected_status = ASX_E_RESOURCE_EXHAUSTED;
    result->failure_atomic_expected = 1u;
    reset_all();

    for (index = 0u; index < (uint32_t)ASX_MAX_TIMERS; index++) {
        if (asx_timer_register(wheel, (asx_time)(100u + index), NULL, &handles[index]) != ASX_OK) {
            result->diagnostic = "failed to fill timer wheel";
            goto finish;
        }
    }
    metrics_capture(&result->before);
    if (asx_runtime_snapshot_capture(&before_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture timer before snapshot";
        goto finish;
    }

    result->actual_status = asx_timer_register(wheel, 10000u, NULL, &overflow_handle);

    metrics_capture(&result->after);
    if (asx_runtime_snapshot_capture(&after_snapshot) != ASX_OK) {
        result->diagnostic = "failed to capture timer after snapshot";
        goto finish;
    }
    result->failure_atomic_observed =
        (snapshots_equal(&before_snapshot, &after_snapshot) &&
         result->before.timer_active_count == result->after.timer_active_count)
            ? 1u
            : 0u;
    result->semantic_final_code = FINAL_STATE_TIMER_INTACT;
    result->after.final_state_code = result->semantic_final_code;

    fired = asx_timer_collect_expired(wheel, 100u, wakers, 1u);
    if (fired != 1u) {
        result->diagnostic = "oldest timer was not intact after failed allocation";
        goto finish;
    }
    result->passed =
        (result->actual_status == result->expected_status && result->failure_atomic_observed == 1u);

finish:
    finalize_result(result);
}

static asx_status run_budget_once(uint32_t *event_count_out) {
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    asx_status status;

    reset_all();
    if (asx_region_open(&region) != ASX_OK) return ASX_E_INVALID_STATE;
    if (asx_task_spawn(region, poll_pending, NULL, &task) != ASX_OK) { return ASX_E_INVALID_STATE; }
    budget = asx_budget_from_polls(1u);
    status = asx_scheduler_run(region, &budget);
    if (event_count_out != NULL) *event_count_out = asx_scheduler_event_count();
    return status;
}

static void scenario_scheduler_poll_budget_exhaustion(pressure_case_result *result) {
    asx_status first_status;
    asx_status second_status;
    uint32_t first_events = 0u;
    uint32_t second_events = 0u;

    result->scenario_id = "rp-scheduler-poll-budget-exhaustion-006";
    result->surface = "scheduler_poll_budget";
    result->expected_status = ASX_E_POLL_BUDGET_EXHAUSTED;
    result->failure_atomic_expected = 0u;

    first_status = run_budget_once(&first_events);
    metrics_capture(&result->after);
    second_status = run_budget_once(&second_events);

    result->actual_status = first_status;
    result->failure_atomic_observed = 0u;
    result->semantic_final_code = FINAL_STATE_BUDGET_EXHAUSTED_DETERMINISTIC;
    result->after.scheduler_event_count = first_events;
    result->after.final_state_code = result->semantic_final_code;

    if (first_events == 0u || first_status != second_status || first_events != second_events) {
        result->diagnostic = "poll budget exhaustion was not deterministic";
        goto finish;
    }
    result->passed = (result->actual_status == result->expected_status);

finish:
    finalize_result(result);
}

static void scenario_cancel_cleanup_budget_exhaustion(pressure_case_result *result) {
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    asx_checkpoint_result checkpoint;
    asx_task_state task_state;
    asx_cancel_phase phase;
    asx_outcome outcome;
    uint32_t event_index;
    uint32_t forced_events = 0u;
    asx_status scheduler_status;

    result->scenario_id = "rp-cancel-cleanup-budget-exhaustion-007";
    result->surface = "cancel_cleanup_budget";
    result->expected_status = ASX_E_CANCELLED;
    result->failure_atomic_expected = 0u;
    reset_all();

    if (asx_region_open(&region) != ASX_OK ||
        asx_task_spawn(region, poll_checkpoint_forever, NULL, &task) != ASX_OK) {
        result->diagnostic = "failed to create cancellation scenario";
        goto finish;
    }
    budget = asx_budget_from_polls(1u);
    scheduler_status = asx_scheduler_run(region, &budget);
    (void)scheduler_status;
    if (asx_task_cancel(task, ASX_CANCEL_SHUTDOWN) != ASX_OK ||
        asx_checkpoint(task, &checkpoint) != ASX_OK || !checkpoint.cancelled) {
        result->diagnostic = "failed to enter cancelling phase";
        goto finish;
    }

    metrics_capture(&result->before);
    budget = asx_budget_from_polls(200u);
    scheduler_status = asx_scheduler_run(region, &budget);
    (void)scheduler_status;
    metrics_capture(&result->after);

    for (event_index = 0u; event_index < asx_scheduler_event_count(); event_index++) {
        asx_scheduler_event event;
        if (asx_scheduler_event_get(event_index, &event) &&
            event.kind == ASX_SCHED_EVENT_CANCEL_FORCED) {
            forced_events++;
        }
    }
    result->after.cancel_forced_event_count = forced_events;
    result->failure_atomic_observed = 0u;
    result->semantic_final_code = FINAL_STATE_CANCEL_FORCED_COMPLETED;
    result->after.final_state_code = result->semantic_final_code;

    if (asx_task_get_state(task, &task_state) != ASX_OK ||
        asx_task_get_cancel_phase(task, &phase) != ASX_OK ||
        asx_task_get_outcome(task, &outcome) != ASX_OK) {
        result->diagnostic = "failed to inspect forced-cancel final state";
        goto finish;
    }
    if (task_state != ASX_TASK_COMPLETED || phase != ASX_CANCEL_PHASE_COMPLETED ||
        outcome.severity != ASX_OUTCOME_CANCELLED || forced_events == 0u) {
        result->diagnostic = "cleanup budget did not force deterministic cancellation";
        goto finish;
    }
    result->actual_status = ASX_E_CANCELLED;
    result->passed = 1;

finish:
    finalize_result(result);
}

typedef void (*pressure_scenario_fn)(pressure_case_result *result);

static int run_scenario(pressure_scenario_fn scenario_fn) {
    pressure_case_result result;

    memset(&result, 0, sizeof(result));
    result.actual_status = ASX_E_INVALID_STATE;
    result.semantic_final_code = FINAL_STATE_UNSET;
    scenario_fn(&result);
    return result.passed ? 0 : 1;
}

int main(void) {
    int failures = 0;

    fprintf(stderr, "=== resource_pressure_failure_atomic_test ===\n");
    failures += run_scenario(scenario_region_create_exhaustion);
    failures += run_scenario(scenario_task_spawn_exhaustion);
    failures += run_scenario(scenario_obligation_reserve_exhaustion);
    failures += run_scenario(scenario_channel_enqueue_exhaustion);
    failures += run_scenario(scenario_timer_allocation_exhaustion);
    failures += run_scenario(scenario_scheduler_poll_budget_exhaustion);
    failures += run_scenario(scenario_cancel_cleanup_budget_exhaustion);

    fprintf(stderr, "\n%d/7 resource-pressure scenarios passed\n", 7 - failures);
    if (failures > 0) fprintf(stderr, "%d resource-pressure failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
