/*
 * evidence.c — explicit evidence-family helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>
#include <string.h>

static int nonempty(const char *value) { return value != NULL && value[0] != '\0'; }

static void append_json_control_escape(asx_report_buf *out, unsigned char byte) {
    static const char hex[] = "0123456789abcdef";
    char escape[7];

    if (out == NULL) return;

    escape[0] = '\\';
    escape[1] = 'u';
    escape[2] = '0';
    escape[3] = '0';
    escape[4] = hex[(byte >> 4) & 0x0f];
    escape[5] = hex[byte & 0x0f];
    escape[6] = '\0';
    asx_report_buf_append(out, escape);
}

static void append_json_escaped(asx_report_buf *out, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    char ch[2];

    if (out == NULL || text == NULL) return;
    ch[1] = '\0';

    while (*p != '\0') {
        switch (*p) {
        case '\\': asx_report_buf_append(out, "\\\\"); break;
        case '"': asx_report_buf_append(out, "\\\""); break;
        case '\n': asx_report_buf_append(out, "\\n"); break;
        case '\r': asx_report_buf_append(out, "\\r"); break;
        case '\t': asx_report_buf_append(out, "\\t"); break;
        case '\b': asx_report_buf_append(out, "\\b"); break;
        case '\f': asx_report_buf_append(out, "\\f"); break;
        default:
            if (*p < 0x20u) {
                append_json_control_escape(out, *p);
                break;
            }
            ch[0] = (char)*p;
            asx_report_buf_append(out, ch);
            break;
        }
        p++;
    }
}

static void append_json_string(asx_report_buf *out, const char *value) {
    asx_report_buf_append(out, "\"");
    append_json_escaped(out, value ? value : "");
    asx_report_buf_append(out, "\"");
}

static void append_json_bool(asx_report_buf *out, int value) {
    asx_report_buf_append(out, value ? "true" : "false");
}

static void append_fnv64(asx_report_buf *out, uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    char tmp[17];
    int i;

    for (i = 15; i >= 0; i--) {
        tmp[i] = hex[value & UINT64_C(0x0f)];
        value >>= 4;
    }
    tmp[16] = '\0';
    asx_report_buf_append(out, "\"fnv64:");
    asx_report_buf_append(out, tmp);
    asx_report_buf_append(out, "\"");
}

static asx_status validate_incident_bundle(const asx_incident_bundle *bundle) {
    if (bundle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!nonempty(bundle->schema_name) ||
        strcmp(bundle->schema_name, ASX_INCIDENT_BUNDLE_SCHEMA_NAME) != 0) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!nonempty(bundle->schema_version) ||
        strcmp(bundle->schema_version, ASX_INCIDENT_BUNDLE_SCHEMA_VERSION) != 0) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!nonempty(bundle->run_id) || !nonempty(bundle->scenario_id) || !nonempty(bundle->profile) ||
        !nonempty(bundle->compiled_profile) || !nonempty(bundle->scale) ||
        !nonempty(bundle->codec) || !nonempty(bundle->trace_schema_version) ||
        !nonempty(bundle->replay_command) || !nonempty(bundle->details_path) ||
        !nonempty(bundle->conformance_status) || !nonempty(bundle->profile_parity_status) ||
        bundle->telemetry == NULL || bundle->worker_count == 0u || bundle->max_workers == 0u ||
        bundle->worker_count > bundle->max_workers) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

const char *asx_evidence_level_str(asx_evidence_level level) {
    switch (level) {
    case ASX_EVIDENCE_INFO: return "INFO";
    case ASX_EVIDENCE_PASS: return "PASS";
    case ASX_EVIDENCE_WARN: return "WARN";
    case ASX_EVIDENCE_FAIL: return "FAIL";
    }
    return "UNKNOWN";
}

int asx_evidence_entry_has_entity(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->entity_id != 0 ? 1 : 0;
}

int asx_evidence_is_failure(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->level == ASX_EVIDENCE_FAIL ? 1 : 0;
}

int asx_evidence_is_warning(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->level == ASX_EVIDENCE_WARN ? 1 : 0;
}

int asx_evidence_is_pass(const asx_evidence_entry *entry) {
    if (entry == NULL) return 0;
    return entry->level == ASX_EVIDENCE_PASS ? 1 : 0;
}

uint32_t asx_evidence_count_by_level(const asx_evidence_sink *sink, asx_evidence_level level) {
    uint32_t i, count;

    if (sink == NULL) return 0u;
    count = 0u;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].level == level) count++;
    }
    return count;
}

uint32_t asx_evidence_count_for_source(const asx_evidence_sink *sink, const char *source_prefix) {
    uint32_t i, count;
    size_t prefix_len;

    if (sink == NULL || source_prefix == NULL) return 0u;
    prefix_len = strlen(source_prefix);
    count = 0u;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].source != NULL &&
            strncmp(sink->entries[i].source, source_prefix, prefix_len) == 0) {
            count++;
        }
    }
    return count;
}

const asx_evidence_entry *asx_evidence_first_failure(const asx_evidence_sink *sink) {
    uint32_t i;

    if (sink == NULL) return NULL;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].level == ASX_EVIDENCE_FAIL) return &sink->entries[i];
    }
    return NULL;
}

int asx_evidence_has_entity(const asx_evidence_sink *sink, uint64_t entity_id) {
    uint32_t i;

    if (sink == NULL) return 0;
    for (i = 0u; i < sink->count; i++) {
        if (sink->entries[i].entity_id == entity_id) return 1;
    }
    return 0;
}

const char *asx_evidence_verdict_str(asx_evidence_level verdict) {
    switch (verdict) {
    case ASX_EVIDENCE_PASS: return "PASS";
    case ASX_EVIDENCE_WARN: return "WARN";
    case ASX_EVIDENCE_FAIL: return "FAIL";
    case ASX_EVIDENCE_INFO: return "INFO";
    }
    return "UNKNOWN";
}

void asx_incident_bundle_init(asx_incident_bundle *bundle) {
    if (bundle == NULL) return;
    memset(bundle, 0, sizeof(*bundle));
    bundle->schema_name = ASX_INCIDENT_BUNDLE_SCHEMA_NAME;
    bundle->schema_version = ASX_INCIDENT_BUNDLE_SCHEMA_VERSION;
    bundle->codec = "json";
    bundle->pass = 1;
    bundle->status = ASX_OK;
    bundle->failure_class = ASX_INCIDENT_BUNDLE_FAILURE_NONE;
    bundle->failure_message = "";
    bundle->trace_schema_version = ASX_TRACE_SCHEMA_VERSION;
    bundle->conformance_status = ASX_INCIDENT_BUNDLE_STATUS_NOT_RUN;
    bundle->profile_parity_status = ASX_INCIDENT_BUNDLE_STATUS_NOT_RUN;
}

const char *asx_incident_bundle_failure_class(asx_status status, int unsupported_profile) {
    asx_error_category category;

    if (status == ASX_OK) return ASX_INCIDENT_BUNDLE_FAILURE_NONE;
    if (unsupported_profile || status == ASX_E_PERMISSION_DENIED) {
        return ASX_INCIDENT_BUNDLE_FAILURE_UNSUPPORTED;
    }
    if (status == ASX_E_RESOURCE_EXHAUSTED || status == ASX_E_OVERLOADED ||
        status == ASX_E_COST_QUOTA_EXHAUSTED || status == ASX_E_POLL_QUOTA_EXHAUSTED) {
        return ASX_INCIDENT_BUNDLE_FAILURE_RESOURCE;
    }
    if (status == ASX_E_EQUIVALENCE_MISMATCH || status == ASX_E_REPLAY_MISMATCH) {
        return ASX_INCIDENT_BUNDLE_FAILURE_CONFORMANCE;
    }

    category = asx_status_category(status);
    if (category == ASX_ERROR_CATEGORY_RESOURCE) return ASX_INCIDENT_BUNDLE_FAILURE_RESOURCE;
    if (category == ASX_ERROR_CATEGORY_EQUIVALENCE || category == ASX_ERROR_CATEGORY_REPLAY) {
        return ASX_INCIDENT_BUNDLE_FAILURE_CONFORMANCE;
    }
    return ASX_INCIDENT_BUNDLE_FAILURE_RUNTIME;
}

asx_status asx_incident_bundle_render_json(const asx_incident_bundle *bundle, asx_report_buf *out) {
    const asx_parallel_telemetry_snapshot *telemetry;
    const asx_parallel_admission_decision *admission;
    const asx_parallel_locality_snapshot *locality;
    const asx_parallel_commit_authority_snapshot *commit_authority;
    const char *failure_class;
    asx_report_buf tmp;
    asx_status valid;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    valid = validate_incident_bundle(bundle);
    if (valid != ASX_OK) return valid;

    telemetry = bundle->telemetry;
    admission = &telemetry->admission;
    locality = &telemetry->locality;
    commit_authority = &telemetry->commit_authority;
    failure_class = nonempty(bundle->failure_class)
                        ? bundle->failure_class
                        : asx_incident_bundle_failure_class(bundle->status, 0);

    asx_report_buf_init(&tmp);
    asx_report_buf_append(&tmp, "{\"schema_name\":");
    append_json_string(&tmp, bundle->schema_name);
    asx_report_buf_append(&tmp, ",\"schema_version\":");
    append_json_string(&tmp, bundle->schema_version);

    asx_report_buf_append(&tmp, ",\"run\":{\"run_id\":");
    append_json_string(&tmp, bundle->run_id);
    asx_report_buf_append(&tmp, ",\"scenario_id\":");
    append_json_string(&tmp, bundle->scenario_id);
    asx_report_buf_append(&tmp, ",\"seed\":");
    asx_report_buf_append_u32(&tmp, bundle->seed);
    asx_report_buf_append(&tmp, ",\"profile\":");
    append_json_string(&tmp, bundle->profile);
    asx_report_buf_append(&tmp, ",\"compiled_profile\":");
    append_json_string(&tmp, bundle->compiled_profile);
    asx_report_buf_append(&tmp, ",\"scale\":");
    append_json_string(&tmp, bundle->scale);
    asx_report_buf_append(&tmp, ",\"codec\":");
    append_json_string(&tmp, bundle->codec);
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"status\":{\"pass\":");
    append_json_bool(&tmp, bundle->pass);
    asx_report_buf_append(&tmp, ",\"result\":");
    append_json_string(&tmp, bundle->pass ? "pass" : "fail");
    asx_report_buf_append(&tmp, ",\"status_code\":");
    asx_report_buf_append_u32(&tmp, (uint32_t)bundle->status);
    asx_report_buf_append(&tmp, ",\"status_text\":");
    append_json_string(&tmp, asx_status_str(bundle->status));
    asx_report_buf_append(&tmp, ",\"failure_class\":");
    append_json_string(&tmp, failure_class);
    asx_report_buf_append(&tmp, ",\"failure_message\":");
    append_json_string(&tmp, bundle->failure_message ? bundle->failure_message : "");
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"telemetry\":{\"worker_count\":");
    asx_report_buf_append_u32(&tmp, bundle->worker_count);
    asx_report_buf_append(&tmp, ",\"max_workers\":");
    asx_report_buf_append_u32(&tmp, bundle->max_workers);
    asx_report_buf_append(&tmp, ",\"queue_depths\":{\"total\":");
    asx_report_buf_append_u32(&tmp, telemetry->total_queue_depth);
    asx_report_buf_append(&tmp, ",\"ready\":");
    asx_report_buf_append_u32(&tmp, telemetry->lane_depths[ASX_LANE_READY]);
    asx_report_buf_append(&tmp, ",\"cancel\":");
    asx_report_buf_append_u32(&tmp, telemetry->lane_depths[ASX_LANE_CANCEL]);
    asx_report_buf_append(&tmp, ",\"timed\":");
    asx_report_buf_append_u32(&tmp, telemetry->lane_depths[ASX_LANE_TIMED]);
    asx_report_buf_append(&tmp, "},\"pressure_pct\":");
    asx_report_buf_append_u32(&tmp, telemetry->pressure_pct);
    asx_report_buf_append(&tmp, ",\"blocking_backlog\":");
    asx_report_buf_append_u32(&tmp, telemetry->blocking_backlog);
    asx_report_buf_append(&tmp, ",\"max_lane_depth\":");
    asx_report_buf_append_u32(&tmp, telemetry->max_lane_depth);
    asx_report_buf_append(&tmp, ",\"max_worker_queue_depth\":");
    asx_report_buf_append_u32(&tmp, telemetry->max_worker_queue_depth);
    asx_report_buf_append(&tmp, ",\"hot_worker\":");
    asx_report_buf_append_u32(&tmp, telemetry->hot_worker);

    asx_report_buf_append(&tmp, ",\"metrics\":{\"ready_dispatches\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.ready_dispatches);
    asx_report_buf_append(&tmp, ",\"cancel_dispatches\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.cancel_dispatches);
    asx_report_buf_append(&tmp, ",\"timed_dispatches\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.timed_dispatches);
    asx_report_buf_append(&tmp, ",\"cancel_streak_max\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.cancel_streak_max);
    asx_report_buf_append(&tmp, ",\"fairness_yields\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.fairness_yields);
    asx_report_buf_append(&tmp, ",\"worker_yields\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.worker_yields);
    asx_report_buf_append(&tmp, ",\"commit_sequence\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.commit_sequence);
    asx_report_buf_append(&tmp, ",\"admission_rejects\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.admission_rejects);
    asx_report_buf_append(&tmp, ",\"admission_backpressure\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.admission_backpressure);
    asx_report_buf_append(&tmp, ",\"admission_sheds\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.admission_sheds);
    asx_report_buf_append(&tmp, ",\"pressure_transitions\":");
    asx_report_buf_append_u32(&tmp, telemetry->metrics.pressure_transitions);
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"admission\":{\"triggered\":");
    append_json_bool(&tmp, admission->triggered);
    asx_report_buf_append(&tmp, ",\"mode\":");
    append_json_string(&tmp, asx_parallel_admission_mode_str(admission->mode));
    asx_report_buf_append(&tmp, ",\"pressure_pct\":");
    asx_report_buf_append_u32(&tmp, admission->pressure_pct);
    asx_report_buf_append(&tmp, ",\"queued\":");
    asx_report_buf_append_u32(&tmp, admission->queued);
    asx_report_buf_append(&tmp, ",\"capacity\":");
    asx_report_buf_append_u32(&tmp, admission->capacity);
    asx_report_buf_append(&tmp, ",\"shed_count\":");
    asx_report_buf_append_u32(&tmp, admission->shed_count);
    asx_report_buf_append(&tmp, ",\"status_code\":");
    asx_report_buf_append_u32(&tmp, (uint32_t)admission->admit_status);
    asx_report_buf_append(&tmp, ",\"status_text\":");
    append_json_string(&tmp, asx_status_str(admission->admit_status));
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"locality\":{\"mode\":");
    append_json_string(&tmp, asx_parallel_locality_mode_str(locality->mode));
    asx_report_buf_append(&tmp, ",\"shard_count\":");
    asx_report_buf_append_u32(&tmp, locality->shard_count);
    asx_report_buf_append(&tmp, ",\"tasks_per_shard\":");
    asx_report_buf_append_u32(&tmp, locality->tasks_per_shard);
    asx_report_buf_append(&tmp, ",\"hot_shard\":");
    asx_report_buf_append_u32(&tmp, locality->hot_shard);
    asx_report_buf_append(&tmp, ",\"max_shard_tasks\":");
    asx_report_buf_append_u32(&tmp, locality->max_shard_tasks);
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"commit_authority\":{\"commit_sequence\":");
    asx_report_buf_append_u32(&tmp, commit_authority->commit_sequence);
    asx_report_buf_append(&tmp, ",\"total_worker_commits\":");
    asx_report_buf_append_u32(&tmp, commit_authority->total_worker_commits);
    asx_report_buf_append(&tmp, ",\"max_worker_commit_sequence\":");
    asx_report_buf_append_u32(&tmp, commit_authority->max_worker_commit_sequence);
    asx_report_buf_append(&tmp, ",\"drift_detected\":");
    append_json_bool(&tmp, commit_authority->drift_detected != 0u);
    asx_report_buf_append(&tmp, ",\"native_live_enabled\":");
    append_json_bool(&tmp, commit_authority->native_live_enabled != 0u);
    asx_report_buf_append(&tmp, ",\"native_live_status\":");
    append_json_string(&tmp, asx_status_str(commit_authority->native_live_status));
    asx_report_buf_append(&tmp, "}}");

    asx_report_buf_append(&tmp, ",\"trace\":{\"schema_version\":");
    append_json_string(&tmp, bundle->trace_schema_version);
    asx_report_buf_append(&tmp, ",\"event_count\":");
    asx_report_buf_append_u32(&tmp, bundle->trace_event_count);
    asx_report_buf_append(&tmp, ",\"semantic_digest\":");
    append_fnv64(&tmp, bundle->semantic_digest);
    asx_report_buf_append(&tmp, ",\"trace_digest\":");
    append_fnv64(&tmp, bundle->trace_digest);
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"parity\":{\"conformance\":");
    append_json_string(&tmp, bundle->conformance_status);
    asx_report_buf_append(&tmp, ",\"profile_parity\":");
    append_json_string(&tmp, bundle->profile_parity_status);
    asx_report_buf_append(&tmp, "}");

    asx_report_buf_append(&tmp, ",\"artifacts\":{\"details_path\":");
    append_json_string(&tmp, bundle->details_path);
    asx_report_buf_append(&tmp, ",\"replay_command\":");
    append_json_string(&tmp, bundle->replay_command);
    asx_report_buf_append(&tmp, "}}");

    if (tmp.len >= ASX_REPORT_BUF_SIZE - 1u) return ASX_E_BUFFER_TOO_SMALL;
    *out = tmp;
    return ASX_OK;
}
