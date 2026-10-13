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

/* Strengthen current reason `a` with candidate `b` and return the winner,
 * whole (Rust CancelReason::strengthen): the more severe kind wins; equal
 * severity: the earlier timestamp wins; equal timestamps: a message beats
 * none and the smaller message (byte order) beats the larger; otherwise
 * `a` is kept. */
ASX_API asx_cancel_reason asx_cancel_strengthen(const asx_cancel_reason *a,
                                                const asx_cancel_reason *b);

/* A reason of `kind` with Rust's default attribution, as its Cx-free
 * constructors make one (CancelReason::new / ::user / ::timeout /
 * ::race_loser / ::poll_quota, types/cancel.rs:596-631): origin the region
 * at arena index 0 (which the lab's root region also is), no task, stamped
 * 1 s, with `message` (may be NULL). Rust's lab stamps a pre-poll quota
 * cancel (lab/runtime.rs:4667), a ChildRegion close, a refused spawn's join
 * and Scope::timeout's abort with it. */
ASX_API asx_cancel_reason asx_cancel_reason_default(asx_cancel_kind kind, const char *message);

/* -------------------------------------------------------------------
 * Cause-chain bound
 *
 * The runtime builds a chain when it cancels a region subtree: each
 * descendant's reason is PARENT, caused by its parent's. Rust bounds that
 * chain with CancelAttributionConfig (types/cancel.rs:184-262) and
 * CancelReason::with_cause_limited (:706-761): a chain holds at most
 * `max_depth` reasons (the reason and its causes) and an estimated
 * 88 * depth - 8 bytes (80 per reason, 8 per link); a cause chain that
 * would exceed either is cut and the reason built on it is flagged
 * `truncated`. The runtime's bounds are asx_runtime_config's
 * max_cancel_chain_depth and max_cancel_chain_memory.
 * ------------------------------------------------------------------- */

/* The cause links a cut chain keeps under the given bounds. A chain meets
 * the depth bound or the memory bound first as it grows, and once cut it
 * is cut at the same length again, so the length is a function of the
 * bounds alone: min(max_depth - 1, (max_memory - 80) / 88), each term
 * floored at 0. (Where a caller hands the runtime a chain already deeper
 * than max_depth, and the memory bound is the tighter one, Rust's depth
 * branch keeps max_depth - 1 links without checking memory; this keeps the
 * tighter length.) */
ASX_API uint32_t asx_cancel_chain_link_limit(uint32_t max_depth, uint32_t max_memory);

/* Make `cause` the cause of `r` under the active bounds, as Rust's
 * with_cause_limited does: a chain that would pass either bound sets
 * `r->truncated`, and a bound that leaves room for no cause leaves
 * `r->cause` NULL. A NULL `cause` clears the cause. `r->truncated` is left
 * as it was when the chain fits. */
ASX_API void asx_cancel_reason_with_cause_limited(asx_cancel_reason *r,
                                                  const asx_cancel_reason *cause);

/* The cause links of `r`'s chain: Rust's chain_depth() - 1, the
 * vocabulary's cause_chain_len. A truncated chain ends after
 * asx_cancel_chain_link_limit() links under the active bounds even where
 * the stored causes continue (they are the ancestors' own reasons, which
 * longer chains share); a caller's own chain is counted whole, up to 1024
 * links. Walk a chain with this count, not until `cause` is NULL. */
ASX_API uint32_t asx_cancel_reason_chain_links(const asx_cancel_reason *r);

/* The runtime installs asx_runtime_config's chain bounds at init; set them
 * through the config, applications do not call this. */
ASX_API void asx_cancel_chain_limits_set_internal(uint32_t max_depth, uint32_t max_memory);
/* The defaults (16 reasons, 4096 bytes) return at runtime reset. */
ASX_API void asx_cancel_chain_limits_reset_internal(void);
/* What the runtime's reset and init call (applications do not): the chain
 * bounds return to their defaults, and every cancel witness of a task of the
 * runtime with `runtime_tag` is released, so a task cancelled and discarded
 * before it completed leaves no slot behind in the fixed witness table. */
ASX_API void asx_cancel_reset_internal(uint16_t runtime_tag);

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

/* -------------------------------------------------------------------
 * Witness values and Rust's validation rules
 *
 * Rust's CancelWitness (types/cancel.rs:323-412) is a value: task, region,
 * cancel epoch, phase and reason, and validate_transition is the rule
 * between two of them. A C witness slot carries the same five, read from
 * the runtime's task record when the witness is created (region
 * ASX_INVALID_ID and epoch 0 for a task the runtime holds no record of).
 * asx_cancel_witness_advance stays the strict step API (a phase moves
 * forward and never repeats); the validation below is Rust's rule between
 * two observed witnesses, where an equal phase is allowed.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_task_id task;
    asx_region_id region;
    uint32_t epoch;
    asx_cancel_phase phase;
    asx_cancel_reason reason;
} asx_cancel_witness_info;

/* The witness's task, region, epoch, phase and reason.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT if out is NULL, or
 * ASX_E_NOT_FOUND for an invalid or released witness. */
ASX_API ASX_MUST_USE asx_status asx_cancel_witness_info_get(asx_cancel_witness_id w,
                                                            asx_cancel_witness_info *out);

/* Rust CancelWitness::validate_initial (types/cancel.rs:369-373): the first
 * witness of a task's stream must carry a non-zero epoch (epoch 0 means no
 * cancel was ever requested). Returns ASX_OK, ASX_E_INVALID_TRANSITION for
 * epoch 0 (Rust's InitialEpochZero) or ASX_E_INVALID_ARGUMENT for NULL. */
ASX_API ASX_MUST_USE asx_status
asx_cancel_witness_validate_initial(const asx_cancel_witness_info *w);

/* Rust CancelWitness::validate_transition (types/cancel.rs:382-411): between
 * `prev` (NULL for the first) and `next`, the same task, region and epoch,
 * a phase that does not regress (an equal phase is fine) and a reason whose
 * severity does not weaken, reported in that order as
 * ASX_E_WITNESS_TASK_MISMATCH, _REGION_MISMATCH, _EPOCH_MISMATCH,
 * _PHASE_REGRESSION or _REASON_WEAKENED (Rust's CancelWitnessError). Returns
 * ASX_OK when valid, ASX_E_INVALID_ARGUMENT for a NULL next. */
ASX_API ASX_MUST_USE asx_status asx_cancel_witness_validate_transition(
    const asx_cancel_witness_info *prev, const asx_cancel_witness_info *next);

#endif /* ASX_CORE_CANCEL_H */
