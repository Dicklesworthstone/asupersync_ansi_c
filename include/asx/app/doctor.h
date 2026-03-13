/*
 * asx/app/doctor.h — runtime diagnostic checks and health reports
 *
 * Provides structured diagnostic inspection of the runtime:
 *   - Health checks for runtime subsystems
 *   - Structured report with pass/warn/fail classification
 *   - Arena utilization and capacity queries
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_APP_DOCTOR_H
#define ASX_APP_DOCTOR_H

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

typedef enum { ASX_DOCTOR_OK = 0, ASX_DOCTOR_WARN = 1, ASX_DOCTOR_FAIL = 2 } asx_doctor_severity;

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

#ifndef ASX_DOCTOR_MAX_CHECKS
#define ASX_DOCTOR_MAX_CHECKS 16u
#endif

typedef struct {
    asx_doctor_check_result checks[ASX_DOCTOR_MAX_CHECKS];
    uint32_t check_count;
    uint32_t pass_count;
    uint32_t warn_count;
    uint32_t fail_count;
} asx_doctor_report;

/* -------------------------------------------------------------------
 * Doctor API
 * ------------------------------------------------------------------- */

/* Run all diagnostic checks and populate the report.
 * Checks include: runtime initialized, region/task/obligation
 * utilization, safety profile, containment policy. */
ASX_API ASX_MUST_USE asx_status asx_doctor_run(const asx_runtime *rt, asx_doctor_report *report);

/* Check if the report indicates a healthy runtime (no FAILs). */
ASX_API int asx_doctor_is_healthy(const asx_doctor_report *report);

/* Get the overall severity (worst across all checks). */
ASX_API asx_doctor_severity asx_doctor_overall(const asx_doctor_report *report);

#ifdef __cplusplus
}
#endif

#endif /* ASX_APP_DOCTOR_H */
