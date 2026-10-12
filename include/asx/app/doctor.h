/*
 * asx/app/doctor.h — runtime diagnostic checks and health reports
 *
 * Provides structured diagnostic inspection of the runtime:
 *   - Health checks for runtime subsystems
 *   - Structured report with pass/warn/fail classification
 *   - Arena utilization, runtime-owned IO/blocking state, and capacity queries
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_APP_DOCTOR_H
#define ASX_APP_DOCTOR_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/rt.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Check severity
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DOCTOR_OK = 0,
    ASX_DOCTOR_WARN = 1,
    ASX_DOCTOR_FAIL = 2,
    ASX_DOCTOR_SKIP = 3 /* the build has nothing to check here (no OS sockets, ...) */
} asx_doctor_severity;

/* -------------------------------------------------------------------
 * Individual check result
 * ------------------------------------------------------------------- */

typedef struct {
    const char *name;
    asx_doctor_severity severity;
    const char *message;
    uint32_t value;    /* diagnostic value (e.g., count) */
    uint32_t capacity; /* capacity limit */
} asx_doctor_check_result;

/* -------------------------------------------------------------------
 * Doctor report
 * ------------------------------------------------------------------- */

typedef struct {
    asx_doctor_check_result checks[ASX_DOCTOR_MAX_CHECKS];
    uint32_t check_count;
    uint32_t pass_count;
    uint32_t warn_count;
    uint32_t fail_count;
    uint32_t skip_count;
} asx_doctor_report;

/* Upper bound on the scheduler run that drives the probe tasks of
 * asx_doctor_probe (runtime clock nanoseconds). */
/* -------------------------------------------------------------------
 * Doctor API
 * ------------------------------------------------------------------- */

/* Run all diagnostic checks and populate the report.
 * Checks include: runtime initialized, region/task/obligation
 * utilization, runtime-owned IO driver and blocking-pool state,
 * safety profile, and containment policy. */
ASX_API ASX_MUST_USE asx_status asx_doctor_run(const asx_runtime *rt, asx_doctor_report *report);

/* Exercise the platform behind an initialized runtime and append one
 * check per probe to a report filled by asx_doctor_run:
 *   clock         reads never go backwards, and a real clock advances
 *   entropy       two draws from the entropy stream differ
 *   socket        a TCP listener binds on loopback (native sockets)
 *   reactor       a loopback TCP echo between two tasks completes, each
 *                 task parked on its socket until the live reactor wakes it
 *   blocking_job  a task parks on a blocking-pool job and gets its result
 * A probe the build cannot run (in-memory transport, ghost reactor, no
 * blocking pool) reports ASX_DOCTOR_SKIP. The tasks run in a new region
 * under a scheduler run bounded by ASX_DOCTOR_PROBE_TIMEOUT_NS; the region
 * is cancelled if they are not done, then drained, before return. The
 * probe draws two values from the entropy stream. Call it on an idle
 * runtime.
 * Returns ASX_OK once the probes ran (their verdicts are in the report),
 * ASX_E_INVALID_ARGUMENT for a NULL argument, ASX_E_INVALID_STATE if rt is
 * not initialized, or ASX_E_RESOURCE_EXHAUSTED if the report lacks room
 * for the five checks. */
ASX_API ASX_MUST_USE asx_status asx_doctor_probe(asx_runtime *rt, asx_doctor_report *report);

/* Check if the report indicates a healthy runtime (no FAILs). */
ASX_API int asx_doctor_is_healthy(const asx_doctor_report *report);

/* Get the overall severity (worst across all checks; a SKIP is never
 * worse than OK). */
ASX_API asx_doctor_severity asx_doctor_overall(const asx_doctor_report *report);

#ifdef __cplusplus
}
#endif

#endif /* ASX_APP_DOCTOR_H */
