/*
 * asx/runtime/diagnostic.h — structured diagnostic context, inspection,
 *                            and evidence sink
 *
 * Provides three capabilities:
 *   1. Diagnostic context: propagate task/region/operation identity
 *      through inspection chains for correlated diagnostic output.
 *   2. Runtime inspection: snapshot the full runtime state into a
 *      structured report covering all subsystems.
 *   3. Evidence sink: a write-once, structured output target that
 *      lab/oracle/doctor can write findings to.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_DIAGNOSTIC_H
#define ASX_RUNTIME_DIAGNOSTIC_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/runtime/rt.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Diagnostic context — propagated identity for correlated output
 * ------------------------------------------------------------------- */

typedef struct {
    asx_task_id task;      /* active task (0 = none) */
    asx_region_id region;  /* active region (0 = none) */
    const char *operation; /* current operation name */
    uint32_t depth;        /* nesting depth */
} asx_diagnostic_ctx;

/* Push a diagnostic context onto the thread-local stack.
 * Returns ASX_OK on success, ASX_E_RESOURCE_EXHAUSTED if stack full. */
ASX_API ASX_MUST_USE asx_status asx_diagnostic_push(const asx_diagnostic_ctx *ctx);

/* Pop the most recent diagnostic context. */
ASX_API void asx_diagnostic_pop(void);

/* Get the current diagnostic context (top of stack).
 * Returns NULL if no context is active. */
ASX_API const asx_diagnostic_ctx *asx_diagnostic_current(void);

/* Reset the diagnostic context stack (test support). */
ASX_API void asx_diagnostic_reset(void);

/* -------------------------------------------------------------------
 * Runtime inspection — unified subsystem state snapshot
 * ------------------------------------------------------------------- */

/* Subsystem health summary */
typedef struct {
    uint32_t active_count;
    uint32_t capacity;
    uint32_t peak_count; /* high water mark since last reset */
} asx_subsystem_gauge;

/* Trace subsystem summary */
typedef struct {
    uint32_t event_count;
    uint32_t capacity;
    uint64_t digest; /* FNV-1a trace digest */
} asx_trace_gauge;

/* Error ledger summary */
typedef struct {
    uint32_t total_errors; /* across all tasks */
    uint32_t overflowed;   /* 1 if any task ring overflowed */
} asx_error_gauge;

/* Ghost monitor summary (debug builds only) */
typedef struct {
    uint32_t violation_count;
    uint32_t overflowed;
    uint32_t obligation_leaks;
} asx_ghost_gauge;

/* Full runtime inspection report */
typedef struct {
    /* Entity gauges */
    asx_subsystem_gauge regions;
    asx_subsystem_gauge tasks;
    asx_subsystem_gauge obligations;
    asx_subsystem_gauge io_driver;
    asx_subsystem_gauge blocking;

    /* Subsystem gauges */
    asx_trace_gauge trace;
    asx_error_gauge errors;
    asx_ghost_gauge ghosts;

    /* Runtime state */
    asx_safety_profile safety_profile;
    asx_containment_policy containment_policy;
    int initialized;
    int any_poisoned; /* 1 if any region poisoned */

    /* Telemetry */
    uint32_t telemetry_emitted;
    uint32_t telemetry_filtered;
    uint64_t telemetry_digest;

    /* Event log */
    uint32_t event_log_count;
    uint64_t event_log_digest;
} asx_inspection_report;

/* Capture a full runtime inspection report.
 * Reads state from all subsystems without mutation. */
ASX_API ASX_MUST_USE asx_status asx_inspect(const asx_runtime *rt, asx_inspection_report *out);

/* -------------------------------------------------------------------
 * Evidence sink — structured output for lab/oracle/doctor findings
 * ------------------------------------------------------------------- */

/* Evidence severity levels */
typedef enum {
    ASX_EVIDENCE_INFO = 0,
    ASX_EVIDENCE_PASS = 1,
    ASX_EVIDENCE_WARN = 2,
    ASX_EVIDENCE_FAIL = 3
} asx_evidence_level;

/* Evidence entry */
typedef struct {
    const char *source; /* emitter name (e.g., "oracle:leak") */
    asx_evidence_level level;
    const char *message;
    uint64_t entity_id; /* related entity (0 = none) */
    uint32_t sequence;  /* monotonic within sink */
} asx_evidence_entry;

/* Evidence sink capacity */
#ifndef ASX_EVIDENCE_SINK_CAPACITY
#define ASX_EVIDENCE_SINK_CAPACITY 64u
#endif

/* Evidence sink — collects structured findings */
typedef struct {
    asx_evidence_entry entries[ASX_EVIDENCE_SINK_CAPACITY];
    uint32_t count;
    uint32_t pass_count;
    uint32_t warn_count;
    uint32_t fail_count;
    uint32_t info_count;
    uint32_t next_seq;
} asx_evidence_sink;

/* Initialize an evidence sink (zeroes all state). */
ASX_API void asx_evidence_sink_init(asx_evidence_sink *sink);

/* Record an evidence entry.
 * Returns ASX_OK on success, ASX_E_RESOURCE_EXHAUSTED if full. */
ASX_API ASX_MUST_USE asx_status asx_evidence_record(asx_evidence_sink *sink, const char *source,
                                                    asx_evidence_level level, const char *message,
                                                    uint64_t entity_id);

/* Query the overall verdict: PASS if no FAILs, WARN if warns but no fails,
 * FAIL if any fails. INFO-only returns PASS. */
ASX_API asx_evidence_level asx_evidence_verdict(const asx_evidence_sink *sink);

/* Check if sink has capacity for at least one more entry. */
ASX_API int asx_evidence_sink_has_room(const asx_evidence_sink *sink);

/* Reset an evidence sink to empty. */
ASX_API void asx_evidence_sink_reset(asx_evidence_sink *sink);

/* -------------------------------------------------------------------
 * Convenience: inspect-and-evidence pipeline
 * ------------------------------------------------------------------- */

/* Run inspection and automatically record findings to an evidence sink.
 * Records one entry per subsystem with appropriate severity. */
ASX_API ASX_MUST_USE asx_status asx_inspect_to_evidence(const asx_runtime *rt,
                                                        asx_evidence_sink *sink);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_DIAGNOSTIC_H */
