/*
 * test_evidence.c — unit tests for explicit evidence families
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>
#include <stdio.h>
#include <string.h>

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
        printf("  " #fn "...\n");                                                                  \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

static void test_evidence_level_str(void) {
    ASSERT(strcmp(asx_evidence_level_str(ASX_EVIDENCE_PASS), "PASS") == 0, "pass string");
    ASSERT(strcmp(asx_evidence_level_str(ASX_EVIDENCE_FAIL), "FAIL") == 0, "fail string");
}

static void test_evidence_entry_helpers(void) {
    asx_evidence_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.level = ASX_EVIDENCE_FAIL;
    entry.entity_id = 9u;

    ASSERT(asx_evidence_entry_has_entity(&entry), "has entity");
    ASSERT(asx_evidence_is_failure(&entry), "failure detected");
}

static void test_evidence_sink_summary(void) {
    asx_evidence_sink sink;
    asx_evidence_summary summary;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "t:a", ASX_EVIDENCE_PASS, "ok", 0));
    MUST_OK(asx_evidence_record(&sink, "t:b", ASX_EVIDENCE_WARN, "warn", 7));
    MUST_OK(asx_evidence_sink_summarize(&sink, &summary));

    ASSERT(summary.count == 2u, "count");
    ASSERT(summary.warn_count == 1u, "warn count");
    ASSERT(summary.verdict == ASX_EVIDENCE_WARN, "warn verdict");
}

static void test_evidence_sink_get_and_ndjson(void) {
    asx_evidence_sink sink;
    asx_report_buf out;
    const asx_evidence_entry *entry;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "t:a", ASX_EVIDENCE_INFO, "note", 11u));
    entry = asx_evidence_sink_get(&sink, 0u);
    ASSERT(entry != NULL, "entry returned");
    ASSERT(entry->entity_id == 11u, "entity preserved");

    MUST_OK(asx_evidence_sink_render_ndjson(&sink, &out));
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"source\":\"t:a\"") != NULL, "ndjson source");
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"level\":\"INFO\"") != NULL, "ndjson level");
}

static void test_evidence_sink_ndjson_escapes_strings(void) {
    asx_evidence_sink sink;
    asx_report_buf out;
    const char *rendered;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "obs\"core\\pipe", ASX_EVIDENCE_WARN,
                                "line1\nline2\t\"quoted\"", 0));

    MUST_OK(asx_evidence_sink_render_ndjson(&sink, &out));
    rendered = asx_report_buf_cstr(&out);

    ASSERT(strstr(rendered, "\"source\":\"obs\\\"core\\\\pipe\"") != NULL, "source escaped");
    ASSERT(strstr(rendered, "\"message\":\"line1\\nline2\\t\\\"quoted\\\"\"") != NULL,
           "message escaped");
}

static void fill_incident_telemetry(asx_parallel_telemetry_snapshot *telemetry) {
    memset(telemetry, 0, sizeof(*telemetry));
    telemetry->worker_count = 4u;
    telemetry->total_queue_depth = 9u;
    telemetry->lane_depths[ASX_LANE_READY] = 5u;
    telemetry->lane_depths[ASX_LANE_CANCEL] = 3u;
    telemetry->lane_depths[ASX_LANE_TIMED] = 1u;
    telemetry->max_lane_depth = 5u;
    telemetry->max_worker_queue_depth = 4u;
    telemetry->hot_worker = 2u;
    telemetry->pressure_pct = 77u;
    telemetry->blocking_backlog = 6u;
    telemetry->metrics.ready_dispatches = 11u;
    telemetry->metrics.cancel_dispatches = 7u;
    telemetry->metrics.timed_dispatches = 3u;
    telemetry->metrics.cancel_streak_max = 2u;
    telemetry->metrics.fairness_yields = 1u;
    telemetry->metrics.worker_yields = 4u;
    telemetry->metrics.commit_sequence = 19u;
    telemetry->metrics.admission_rejects = 1u;
    telemetry->metrics.pressure_transitions = 2u;
    telemetry->admission.triggered = 1;
    telemetry->admission.mode = ASX_PARALLEL_ADMISSION_REJECT;
    telemetry->admission.pressure_pct = 77u;
    telemetry->admission.queued = 9u;
    telemetry->admission.capacity = 10u;
    telemetry->admission.admit_status = ASX_E_ADMISSION_CLOSED;
    telemetry->locality.mode = ASX_PARALLEL_LOCALITY_WORKER_SHARDED;
    telemetry->locality.shard_count = 4u;
    telemetry->locality.tasks_per_shard = 16u;
    telemetry->locality.hot_shard = 2u;
    telemetry->locality.max_shard_tasks = 5u;
    telemetry->commit_authority.commit_sequence = 19u;
    telemetry->commit_authority.total_worker_commits = 19u;
    telemetry->commit_authority.max_worker_commit_sequence = 8u;
    telemetry->commit_authority.native_live_status = ASX_E_PERMISSION_DENIED;
}

static void fill_incident_bundle(asx_incident_bundle *bundle,
                                 const asx_parallel_telemetry_snapshot *telemetry) {
    asx_incident_bundle_init(bundle);
    bundle->run_id = "unit-run";
    bundle->scenario_id = "parallel_swarm.unit";
    bundle->profile = "PARALLEL";
    bundle->compiled_profile = "PARALLEL";
    bundle->scale = "smoke";
    bundle->seed = 42u;
    bundle->worker_count = 4u;
    bundle->max_workers = 64u;
    bundle->semantic_digest = UINT64_C(0x0123456789abcdef);
    bundle->trace_digest = UINT64_C(0xfedcba9876543210);
    bundle->trace_event_count = 12u;
    bundle->replay_command =
        "ASX_E2E_SCENARIO_PACK=parallel_swarm.unit tests/e2e/parallel_swarm.sh";
    bundle->details_path = "parallel_swarm.details.jsonl";
    bundle->telemetry = telemetry;
}

static void test_incident_bundle_defaults(void) {
    asx_incident_bundle bundle;

    asx_incident_bundle_init(&bundle);
    ASSERT(strcmp(bundle.schema_name, ASX_INCIDENT_BUNDLE_SCHEMA_NAME) == 0, "schema name");
    ASSERT(strcmp(bundle.schema_version, ASX_INCIDENT_BUNDLE_SCHEMA_VERSION) == 0,
           "schema version");
    ASSERT(strcmp(bundle.codec, "json") == 0, "codec");
    ASSERT(strcmp(bundle.trace_schema_version, ASX_TRACE_SCHEMA_VERSION) == 0, "trace schema");
    ASSERT(strcmp(bundle.conformance_status, ASX_INCIDENT_BUNDLE_STATUS_NOT_RUN) == 0,
           "conformance status");
}

static void test_incident_bundle_failure_class(void) {
    ASSERT(strcmp(asx_incident_bundle_failure_class(ASX_OK, 0), ASX_INCIDENT_BUNDLE_FAILURE_NONE) ==
               0,
           "ok class");
    ASSERT(strcmp(asx_incident_bundle_failure_class(ASX_E_RESOURCE_EXHAUSTED, 0),
                  ASX_INCIDENT_BUNDLE_FAILURE_RESOURCE) == 0,
           "resource class");
    ASSERT(strcmp(asx_incident_bundle_failure_class(ASX_E_EQUIVALENCE_MISMATCH, 0),
                  ASX_INCIDENT_BUNDLE_FAILURE_CONFORMANCE) == 0,
           "conformance class");
    ASSERT(strcmp(asx_incident_bundle_failure_class(ASX_E_PERMISSION_DENIED, 0),
                  ASX_INCIDENT_BUNDLE_FAILURE_UNSUPPORTED) == 0,
           "permission unsupported");
    ASSERT(strcmp(asx_incident_bundle_failure_class(ASX_E_INVALID_STATE, 1),
                  ASX_INCIDENT_BUNDLE_FAILURE_UNSUPPORTED) == 0,
           "flag unsupported");
    ASSERT(strcmp(asx_incident_bundle_failure_class(ASX_E_INVALID_STATE, 0),
                  ASX_INCIDENT_BUNDLE_FAILURE_RUNTIME) == 0,
           "runtime class");
}

static void test_incident_bundle_rejects_invalid_required_fields(void) {
    asx_parallel_telemetry_snapshot telemetry;
    asx_incident_bundle bundle;
    asx_report_buf out;

    fill_incident_telemetry(&telemetry);
    fill_incident_bundle(&bundle, &telemetry);

    ASSERT(asx_incident_bundle_render_json(NULL, &out) == ASX_E_INVALID_ARGUMENT,
           "null bundle rejected");
    ASSERT(asx_incident_bundle_render_json(&bundle, NULL) == ASX_E_INVALID_ARGUMENT,
           "null out rejected");

    bundle.scenario_id = "";
    ASSERT(asx_incident_bundle_render_json(&bundle, &out) == ASX_E_INVALID_ARGUMENT,
           "empty scenario rejected");

    fill_incident_bundle(&bundle, &telemetry);
    bundle.telemetry = NULL;
    ASSERT(asx_incident_bundle_render_json(&bundle, &out) == ASX_E_INVALID_ARGUMENT,
           "missing telemetry rejected");
}

static void test_incident_bundle_render_json(void) {
    asx_parallel_telemetry_snapshot telemetry;
    asx_incident_bundle bundle;
    asx_report_buf out;
    const char *rendered;

    fill_incident_telemetry(&telemetry);
    fill_incident_bundle(&bundle, &telemetry);
    bundle.pass = 0;
    bundle.status = ASX_E_RESOURCE_EXHAUSTED;
    bundle.failure_class = asx_incident_bundle_failure_class(bundle.status, 0);
    bundle.failure_message = "resource cap at \"swarm\" boundary";

    ASSERT(asx_incident_bundle_render_json(&bundle, &out) == ASX_OK, "render incident bundle");
    rendered = asx_report_buf_cstr(&out);

    ASSERT(strstr(rendered, "\"schema_name\":\"asx.incident_bundle\"") != NULL, "schema name");
    ASSERT(strstr(rendered, "\"scenario_id\":\"parallel_swarm.unit\"") != NULL, "scenario id");
    ASSERT(strstr(rendered, "\"failure_class\":\"resource\"") != NULL, "failure class");
    ASSERT(strstr(rendered, "resource cap at \\\"swarm\\\" boundary") != NULL, "message escaped");
    ASSERT(strstr(rendered, "\"worker_count\":4") != NULL, "worker count");
    ASSERT(strstr(rendered, "\"pressure_pct\":77") != NULL, "pressure");
    ASSERT(strstr(rendered, "\"semantic_digest\":\"fnv64:0123456789abcdef\"") != NULL,
           "semantic digest");
    ASSERT(strstr(rendered, "\"trace_digest\":\"fnv64:fedcba9876543210\"") != NULL, "trace digest");
    ASSERT(strstr(rendered, "\"replay_command\":") != NULL, "replay command");
}

int main(void) {
    printf("test_evidence:\n");

    RUN(test_evidence_level_str);
    RUN(test_evidence_entry_helpers);
    RUN(test_evidence_sink_summary);
    RUN(test_evidence_sink_get_and_ndjson);
    RUN(test_evidence_sink_ndjson_escapes_strings);
    RUN(test_incident_bundle_defaults);
    RUN(test_incident_bundle_failure_class);
    RUN(test_incident_bundle_rejects_invalid_required_fields);
    RUN(test_incident_bundle_render_json);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
