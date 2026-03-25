/*
 * asx/runtime/regression_localize.h — Replay-guided regression localization
 * and deterministic circuit-breaker strategy (bd-3vt.5)
 *
 * Provides:
 * - Trace-based regression detection by correlating replay divergence
 *   with performance counter deltas
 * - Deterministic circuit-breaker that trips/recovers based on
 *   fixture-testable pure-function thresholds
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_REGRESSION_LOCALIZE_H
#define ASX_RUNTIME_REGRESSION_LOCALIZE_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE || \
    defined(ASX_INTERNAL_TRACE_FAMILY_ACCESS)

#include <asx/runtime/trace.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Subsystem identifiers for regression blame                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_SUBSYS_SCHEDULER = 0,
    ASX_SUBSYS_LIFECYCLE = 1,
    ASX_SUBSYS_OBLIGATION = 2,
    ASX_SUBSYS_CHANNEL = 3,
    ASX_SUBSYS_TIMER = 4,
    ASX_SUBSYS_CANCEL = 5,
    ASX_SUBSYS_UNKNOWN = 6
} asx_subsystem_id;

#define ASX_SUBSYS_COUNT 7u

/* ------------------------------------------------------------------ */
/* Per-subsystem performance snapshot                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t event_count; /* trace events attributed to this subsystem */
    uint64_t total_aux;   /* sum of aux payloads (subsystem-specific) */
} asx_subsys_counters;

typedef struct {
    asx_subsys_counters subsystems[ASX_SUBSYS_COUNT];
    uint32_t total_events;
    uint64_t trace_digest;
} asx_perf_snapshot;

/* ------------------------------------------------------------------ */
/* Regression localization result                                      */
/* ------------------------------------------------------------------ */

#define ASX_MAX_SUSPECTS 4u

typedef struct {
    asx_subsystem_id id;
    int32_t event_delta;  /* positive = more events than baseline */
    int64_t aux_delta;    /* positive = higher aux sum */
    uint32_t blame_score; /* 0-100, higher = more likely cause */
} asx_regression_suspect;

typedef struct {
    int regressed; /* 1 if regression detected */
    uint32_t suspect_count;
    asx_regression_suspect suspects[ASX_MAX_SUSPECTS];
    uint32_t divergence_index; /* first divergent trace event */
    asx_replay_result_kind divergence_kind;
} asx_regression_report;

/* ------------------------------------------------------------------ */
/* Deterministic circuit-breaker                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CB_CLOSED = 0,   /* Normal: all requests pass through */
    ASX_CB_OPEN = 1,     /* Tripped: reject/shed until recovery */
    ASX_CB_HALF_OPEN = 2 /* Probing: allow limited traffic */
} asx_cb_state;

typedef struct {
    uint32_t failure_threshold; /* failures before trip (e.g., 3) */
    uint32_t recovery_probes;   /* successful probes to close (e.g., 2) */
    uint32_t cooldown_events;   /* events to wait in OPEN before HALF_OPEN */
} asx_cb_config;

typedef struct {
    asx_cb_state state;
    uint32_t consecutive_failures;
    uint32_t consecutive_successes; /* in HALF_OPEN only */
    uint32_t events_since_trip;
    uint32_t total_trips;
    uint32_t total_recoveries;
} asx_cb_context;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Build a performance snapshot from the current trace ring.
 * Classifies each event by subsystem and accumulates counters. */
ASX_API void asx_perf_snapshot_build(asx_perf_snapshot *snap);

/* Compare two snapshots and produce a regression report.
 * baseline = known-good snapshot, current = potentially regressed.
 * Optionally uses replay divergence info if available. */
ASX_API void asx_regression_localize(const asx_perf_snapshot *baseline,
                                     const asx_perf_snapshot *current,
                                     const asx_replay_result *replay,
                                     asx_regression_report *report);

/* Classify a trace event kind into a subsystem. */
ASX_API asx_subsystem_id asx_trace_event_subsystem(asx_trace_event_kind kind);

/* Return a human-readable name for a subsystem. */
ASX_API const char *asx_subsystem_name(asx_subsystem_id id);

/* Initialize circuit-breaker with default config. */
ASX_API void asx_cb_init(asx_cb_context *ctx);

/* Configure circuit-breaker thresholds. */
ASX_API void asx_cb_configure(asx_cb_context *ctx, const asx_cb_config *cfg);

/* Record a success signal (e.g., probe passed gate). */
ASX_API void asx_cb_record_success(asx_cb_context *ctx, const asx_cb_config *cfg);

/* Record a failure signal (e.g., gate violation, replay divergence). */
ASX_API void asx_cb_record_failure(asx_cb_context *ctx, const asx_cb_config *cfg);

/* Advance the cooldown timer (call once per event/tick in OPEN state). */
ASX_API void asx_cb_tick(asx_cb_context *ctx, const asx_cb_config *cfg);

/* Query whether the breaker allows traffic. */
ASX_API int asx_cb_allows(const asx_cb_context *ctx);

/* Return a human-readable name for a breaker state. */
ASX_API const char *asx_cb_state_name(asx_cb_state state);

#ifdef __cplusplus
}
#endif

#endif /* trace family public contract */

#endif /* ASX_RUNTIME_REGRESSION_LOCALIZE_H */
