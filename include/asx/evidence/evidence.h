/*
 * asx/evidence/evidence.h — explicit evidence-family helpers
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_EVIDENCE_EVIDENCE_H
#define ASX_EVIDENCE_EVIDENCE_H

#include <asx/app/report.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/trace.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the human-readable string for an evidence level. */
ASX_API const char *asx_evidence_level_str(asx_evidence_level level);

/* Check if an evidence entry has an associated entity ID. */
ASX_API int asx_evidence_entry_has_entity(const asx_evidence_entry *entry);

/* Check if an evidence entry is a failure. */
ASX_API int asx_evidence_is_failure(const asx_evidence_entry *entry);

/* Check if an evidence entry is a warning. */
ASX_API int asx_evidence_is_warning(const asx_evidence_entry *entry);

/* Check if an evidence entry is passing. */
ASX_API int asx_evidence_is_pass(const asx_evidence_entry *entry);

/* Count entries by level in a sink. */
ASX_API uint32_t asx_evidence_count_by_level(const asx_evidence_sink *sink,
                                             asx_evidence_level level);

/* Count entries for a specific source prefix (e.g., "monitor:"). */
ASX_API uint32_t asx_evidence_count_for_source(const asx_evidence_sink *sink,
                                               const char *source_prefix);

/* Get the first failure entry, or NULL if no failures. */
ASX_API const asx_evidence_entry *asx_evidence_first_failure(const asx_evidence_sink *sink);

/* Check if the sink contains any entries for a given entity ID. */
ASX_API int asx_evidence_has_entity(const asx_evidence_sink *sink, uint64_t entity_id);

/* Get the overall pass/fail/warn summary as a human-readable string. */
ASX_API const char *asx_evidence_verdict_str(asx_evidence_level verdict);

/* -------------------------------------------------------------------
 * Incident evidence bundle
 * ------------------------------------------------------------------- */

#define ASX_INCIDENT_BUNDLE_SCHEMA_NAME "asx.incident_bundle"
#define ASX_INCIDENT_BUNDLE_SCHEMA_VERSION "asx.incident_bundle.v1"

#define ASX_INCIDENT_BUNDLE_FAILURE_NONE "none"
#define ASX_INCIDENT_BUNDLE_FAILURE_RUNTIME "runtime"
#define ASX_INCIDENT_BUNDLE_FAILURE_CONFORMANCE "conformance"
#define ASX_INCIDENT_BUNDLE_FAILURE_RESOURCE "resource"
#define ASX_INCIDENT_BUNDLE_FAILURE_UNSUPPORTED "unsupported"

#define ASX_INCIDENT_BUNDLE_STATUS_NOT_RUN "not-run"

typedef struct asx_incident_bundle {
    const char *schema_name;
    const char *schema_version;
    const char *run_id;
    const char *scenario_id;
    const char *profile;
    const char *compiled_profile;
    const char *scale;
    const char *codec;
    uint32_t seed;
    uint32_t worker_count;
    uint32_t max_workers;
    int pass;
    asx_status status;
    const char *failure_class;
    const char *failure_message;
    uint64_t semantic_digest;
    uint64_t trace_digest;
    uint32_t trace_event_count;
    const char *trace_schema_version;
    const char *replay_command;
    const char *details_path;
    const char *conformance_status;
    const char *profile_parity_status;
    const asx_parallel_telemetry_snapshot *telemetry;
} asx_incident_bundle;

/* Initialize an incident bundle with the current schema defaults. */
ASX_API void asx_incident_bundle_init(asx_incident_bundle *bundle);

/* Classify a status for operator incident triage. */
ASX_API const char *asx_incident_bundle_failure_class(asx_status status, int unsupported_profile);

/* Render an incident bundle as a single JSON object. */
ASX_API ASX_MUST_USE asx_status asx_incident_bundle_render_json(const asx_incident_bundle *bundle,
                                                                asx_report_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_EVIDENCE_EVIDENCE_H */
