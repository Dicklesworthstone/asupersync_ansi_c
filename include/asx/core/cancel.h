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

/* -------------------------------------------------------------------
 * Cancel witness protocol — lifecycle and query
 * ------------------------------------------------------------------- */

/* Create a cancel witness for the given task and reason.
 * The witness tracks the cancellation's progression through the
 * requested → cancelling → finalizing → completed phases. */
ASX_API ASX_MUST_USE asx_status asx_cancel_witness_create(asx_cancel_witness_id *out,
                                                            asx_task_id task,
                                                            const asx_cancel_reason *reason);

/* Advance a witness to the next phase. */
ASX_API ASX_MUST_USE asx_status asx_cancel_witness_advance(asx_cancel_witness_id w,
                                                             asx_cancel_phase new_phase);

/* Release a witness (mark as inactive). */
ASX_API asx_status asx_cancel_witness_release(asx_cancel_witness_id w);

/* Query the current phase of a cancel witness.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_phase is NULL,
 * ASX_E_NOT_FOUND if witness is invalid/unknown. */
ASX_API ASX_MUST_USE asx_status asx_cancel_witness_phase(asx_cancel_witness_id w,
                                                          asx_cancel_phase *out_phase);

/* Query the cancel reason associated with a witness.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_reason is NULL,
 * ASX_E_NOT_FOUND if witness is invalid/unknown. */
ASX_API ASX_MUST_USE asx_status asx_cancel_witness_reason(asx_cancel_witness_id w,
                                                           asx_cancel_reason *out_reason);

/* Check if a cancel witness is still valid (not stale/completed).
 * Returns 1 if valid, 0 otherwise. */
ASX_API int asx_cancel_witness_is_valid(asx_cancel_witness_id w);

/* -------------------------------------------------------------------
 * User-facing cancellation request
 * ------------------------------------------------------------------- */

/* Request cancellation of a task.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if reason is NULL,
 * ASX_E_NOT_FOUND if task is unknown,
 * ASX_E_INVALID_STATE if task is already in a terminal state. */
ASX_API ASX_MUST_USE asx_status asx_cancel_request(asx_task_id task,
                                                    const asx_cancel_reason *reason);

/* Return the human-readable name for a cancel kind. */
ASX_API const char *asx_cancel_kind_str(asx_cancel_kind kind);

/* Return the human-readable name for a cancel phase. */
ASX_API const char *asx_cancel_phase_str(asx_cancel_phase phase);

#endif /* ASX_CORE_CANCEL_H */
