/*
 * asx/core/cancel.h — cancellation kinds, reasons, and witness protocol
 *
 * Cancellation severity is monotone non-decreasing.
 * Each cancel kind carries a bounded cleanup budget.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_CANCEL_H
#define ASX_CORE_CANCEL_H

#include <stdint.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/core/budget.h>

/* Cancellation kinds (11 variants, severity-ordered) */
typedef enum {
    ASX_CANCEL_USER         = 0,   /* severity 0 */
    ASX_CANCEL_TIMEOUT      = 1,   /* severity 1 */
    ASX_CANCEL_DEADLINE     = 2,   /* severity 1 */
    ASX_CANCEL_POLL_QUOTA   = 3,   /* severity 2 */
    ASX_CANCEL_COST_BUDGET  = 4,   /* severity 2 */
    ASX_CANCEL_FAIL_FAST    = 5,   /* severity 3 */
    ASX_CANCEL_RACE_LOST    = 6,   /* severity 3 */
    ASX_CANCEL_LINKED_EXIT  = 7,   /* severity 3 */
    ASX_CANCEL_PARENT       = 8,   /* severity 4 */
    ASX_CANCEL_RESOURCE     = 9,   /* severity 4 */
    ASX_CANCEL_SHUTDOWN     = 10   /* severity 5 */
} asx_cancel_kind;

/* Cancellation protocol phases */
typedef enum {
    ASX_CANCEL_PHASE_REQUESTED  = 0,
    ASX_CANCEL_PHASE_CANCELLING = 1,
    ASX_CANCEL_PHASE_FINALIZING = 2,
    ASX_CANCEL_PHASE_COMPLETED  = 3
} asx_cancel_phase;

/* Cancel reason with attribution chain */
typedef struct asx_cancel_reason {
    asx_cancel_kind kind;
    asx_region_id   origin_region;
    asx_task_id     origin_task;
    asx_time        timestamp;
    const char     *message;
    struct asx_cancel_reason *cause; /* parent cause (bounded chain) */
    int             truncated;       /* chain was truncated at limit */
} asx_cancel_reason;

/* Return the severity level for a cancel kind (0-5) */
ASX_API int asx_cancel_severity(asx_cancel_kind kind);

/* Return the cleanup budget for a cancel kind */
ASX_API asx_budget asx_cancel_cleanup_budget(asx_cancel_kind kind);

/* Strengthen: returns the reason with higher severity. Equal severity: earlier timestamp wins. */
ASX_API asx_cancel_reason asx_cancel_strengthen(
    const asx_cancel_reason *a,
    const asx_cancel_reason *b
);

#endif /* ASX_CORE_CANCEL_H */
