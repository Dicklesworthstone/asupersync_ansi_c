/*
 * embedded_portability_probe.c — compact embedded portability proof probe
 *
 * Emits one JSON object with compile-time profile/codec macros, R-class
 * resource caps, host pointer width, and a deterministic trace digest from a
 * minimal lifecycle scenario.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/trace.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static asx_status poll_complete(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static const char *profile_macro_name(void) {
#if defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return "ASX_PROFILE_EMBEDDED_ROUTER";
#elif defined(ASX_PROFILE_FREESTANDING)
    return "ASX_PROFILE_FREESTANDING";
#elif defined(ASX_PROFILE_CORE)
    return "ASX_PROFILE_CORE";
#elif defined(ASX_PROFILE_HFT)
    return "ASX_PROFILE_HFT";
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    return "ASX_PROFILE_AUTOMOTIVE";
#elif defined(ASX_PROFILE_PARALLEL)
    return "ASX_PROFILE_PARALLEL";
#else
    return "ASX_PROFILE_UNKNOWN";
#endif
}

static const char *codec_macro_name(void) {
#if defined(ASX_CODEC_BIN)
    return "ASX_CODEC_BIN";
#elif defined(ASX_CODEC_JSON)
    return "ASX_CODEC_JSON";
#else
    return "ASX_CODEC_UNKNOWN";
#endif
}

static asx_resource_class parse_resource_class(const char *value) {
    if (value == NULL) return ASX_CLASS_R1;
    if (strcmp(value, "R1") == 0 || strcmp(value, "ASX_CLASS_R1") == 0) return ASX_CLASS_R1;
    if (strcmp(value, "R2") == 0 || strcmp(value, "ASX_CLASS_R2") == 0) return ASX_CLASS_R2;
    if (strcmp(value, "R3") == 0 || strcmp(value, "ASX_CLASS_R3") == 0) return ASX_CLASS_R3;
    return ASX_CLASS_R1;
}

int main(int argc, char **argv) {
    const char *class_arg = argc > 1 ? argv[1] : "R1";
    asx_resource_class resource_class = parse_resource_class(class_arg);
    asx_resource_limits limits = asx_resource_limits_for_class(resource_class);
    asx_region_id region;
    asx_task_id task;
    asx_task_state task_state;
    asx_budget budget;
    asx_status status;
    uint64_t digest = 0u;
    uint32_t event_count = 0u;
    int scenario_pass = 0;

    asx_runtime_reset();
    asx_trace_reset();

    status = asx_region_open(&region);
    if (status == ASX_OK) { status = asx_task_spawn(region, poll_complete, NULL, &task); }
    if (status == ASX_OK) {
        budget = asx_budget_from_polls(8u);
        status = asx_scheduler_run(region, &budget);
    }
    if (status == ASX_OK) {
        status = asx_task_get_state(task, &task_state);
        if (status == ASX_OK && task_state != ASX_TASK_COMPLETED) { status = ASX_E_INVALID_STATE; }
    }
    if (status == ASX_OK) {
        budget = asx_budget_from_polls(8u);
        status = asx_region_drain(region, &budget);
    }
    if (status == ASX_OK) { status = asx_quiescence_check(region); }

    digest = asx_trace_digest();
    event_count = asx_trace_event_count();
    scenario_pass = (status == ASX_OK && digest != 0u && event_count > 0u);

    printf("{\n");
    printf("  \"schema\": \"asx.embedded_portability_probe.v1\",\n");
    printf("  \"profile_macro\": \"%s\",\n", profile_macro_name());
    printf("  \"codec_macro\": \"%s\",\n", codec_macro_name());
    printf("  \"deterministic_macro\": %d,\n", ASX_DETERMINISTIC);
    printf("  \"resource_class\": \"%s\",\n", asx_resource_class_name(resource_class));
    printf("  \"resource_limits\": {\n");
    printf("    \"max_regions\": %" PRIu32 ",\n", limits.max_regions);
    printf("    \"max_tasks\": %" PRIu32 ",\n", limits.max_tasks);
    printf("    \"max_timers\": %" PRIu32 ",\n", limits.max_timers);
    printf("    \"max_obligations\": %" PRIu32 ",\n", limits.max_obligations);
    printf("    \"max_channels\": %" PRIu32 ",\n", limits.max_channels);
    printf("    \"max_trace_events\": %" PRIu32 "\n", limits.max_trace_events);
    printf("  },\n");
    printf("  \"pointer_bits\": %u,\n", (unsigned)(sizeof(void *) * 8u));
    printf("  \"sizeof_size_t\": %u,\n", (unsigned)sizeof(size_t));
    printf("  \"scenario\": {\n");
    printf("    \"id\": \"embedded-portability-minimal-001\",\n");
    printf("    \"status\": \"%s\",\n", scenario_pass ? "pass" : "fail");
    printf("    \"status_code\": %d,\n", (int)status);
    printf("    \"status_text\": \"%s\",\n", asx_status_str(status));
    printf("    \"semantic_digest\": \"fnv64:%016" PRIx64 "\",\n", digest);
    printf("    \"event_count\": %" PRIu32 "\n", event_count);
    printf("  }\n");
    printf("}\n");

    return scenario_pass ? 0 : 1;
}
