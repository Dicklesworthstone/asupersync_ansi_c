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

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/core/budget.h>
#include <stdint.h>

/* Cancel kind enum, cancel phase enum, and per-kind severity/quota/priority
 * query functions are defined in asx_ids.h. */

/* Cancel reason with attribution chain */
typedef struct asx_cancel_reason {
    asx_cancel_kind kind;
    asx_region_id origin_region;
    asx_task_id origin_task;
    asx_time timestamp;
    const char *message;
    struct asx_cancel_reason *cause; /* parent cause (bounded chain) */
    int truncated;                   /* chain was truncated at limit */
} asx_cancel_reason;

/* Return the cleanup budget for a cancel kind */
ASX_API asx_budget asx_cancel_cleanup_budget(asx_cancel_kind kind);

/* Strengthen: returns the reason with higher severity. Equal severity: earlier timestamp wins. */
ASX_API asx_cancel_reason asx_cancel_strengthen(const asx_cancel_reason *a,
                                                const asx_cancel_reason *b);

#endif /* ASX_CORE_CANCEL_H */
