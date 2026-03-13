/*
 * asx/core/adaptive.h — adaptive-decision contract with expected-loss model
 *
 * Adaptive runtime controllers (cancel-lane tuning, wait-graph monitors)
 * use an expected-loss decision layer with auditable evidence. Every
 * adaptive decision is logged, replayable, and subject to deterministic
 * fallback when confidence drops below threshold.
 *
 * From plan section 6.11: no adaptive controller ships without fallback
 * mode and replayable decision logs.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_ADAPTIVE_H
#define ASX_CORE_ADAPTIVE_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------- */

/* Maximum number of actions in a decision surface. */
#define ASX_ADAPTIVE_MAX_ACTIONS 8u

/* Maximum number of evidence terms per decision record. */
#define ASX_ADAPTIVE_MAX_EVIDENCE 8u

/* Evidence ledger ring capacity (decisions, not bytes). */
#define ASX_ADAPTIVE_LEDGER_DEPTH 64u

/* -------------------------------------------------------------------
 * Decision surface: an adaptive controller's action space
 *
 * Each surface declares a fixed set of actions with loss functions.
 * The runtime evaluates actions against current evidence to pick
 * the minimum expected-loss action.
 * ------------------------------------------------------------------- */

/* Opaque action identifier within a surface. */
typedef uint8_t asx_adaptive_action;

/* Loss function callback: given action and state index, return loss.
 * Loss values are fixed-point 16.16 (multiply by 65536). */
typedef uint32_t (*asx_adaptive_loss_fn)(void *ctx, asx_adaptive_action action,
                                         uint8_t state_index);

/* Decision surface descriptor (immutable after registration). */
typedef struct {
    const char *name;             /* human-readable surface name */
    uint8_t action_count;         /* number of actions (1..MAX_ACTIONS) */
    uint8_t state_count;          /* number of environment states */
    asx_adaptive_loss_fn loss_fn; /* loss function L(action, state) */
    void *loss_ctx;               /* context for loss_fn */
    asx_adaptive_action fallback; /* deterministic fallback action */
} asx_adaptive_surface;

/* -------------------------------------------------------------------
 * Evidence: posterior probability over environment states
 *
 * Callers provide a probability vector P(state | evidence) as
 * fixed-point 0.32 values (multiply by 2^32 for 1.0). The sum
 * should be approximately 2^32.
 * ------------------------------------------------------------------- */

/* Single evidence term for ledger records. */
typedef struct {
    const char *label;   /* human-readable evidence label */
    uint32_t value_fp32; /* observed value, fixed-point 0.32 */
} asx_adaptive_evidence_term;

/* Posterior distribution over states. */
typedef struct {
    uint32_t posterior[ASX_ADAPTIVE_MAX_ACTIONS]; /* P(state_i | evidence), fp 0.32 */
    uint8_t state_count;                          /* valid entries in posterior[] */
    uint32_t confidence_fp32;                     /* overall confidence, fp 0.32 */
} asx_adaptive_posterior;

/* -------------------------------------------------------------------
 * Decision result
 * ------------------------------------------------------------------- */

typedef struct {
    asx_adaptive_action selected;       /* chosen action */
    uint32_t expected_loss_fp16;        /* E[loss] of selected, fp 16.16 */
    asx_adaptive_action counterfactual; /* next-best action */
    uint32_t cf_loss_fp16;              /* E[loss] of counterfactual */
    int used_fallback;                  /* nonzero if fallback was used */
    uint32_t confidence_fp32;           /* decision confidence, fp 0.32 */
} asx_adaptive_decision;

/* -------------------------------------------------------------------
 * Evidence ledger record (written to ring on every decision)
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t sequence;              /* monotonic decision counter */
    const char *surface;            /* surface name */
    asx_adaptive_decision decision; /* selected + counterfactual */
    uint8_t evidence_count;
    asx_adaptive_evidence_term evidence[ASX_ADAPTIVE_MAX_EVIDENCE];
} asx_adaptive_ledger_entry;

/* -------------------------------------------------------------------
 * Confidence threshold policy
 *
 * When posterior confidence drops below threshold, the adaptive
 * controller switches to deterministic fallback (no adaptation).
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t confidence_threshold_fp32; /* below this → fallback mode */
    uint32_t budget_remaining;          /* decisions left before forced fallback (0 = unlimited) */
} asx_adaptive_policy;

/* -------------------------------------------------------------------
 * Adaptive decision API
 * ------------------------------------------------------------------- */

/* Initialize the adaptive subsystem. Clears ledger and resets policy. */
ASX_API void asx_adaptive_init(void);

/* Reset all adaptive state (test support). */
ASX_API void asx_adaptive_reset(void);

/* Set the confidence/budget policy.
 * Preconditions: policy must not be NULL.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if policy is NULL. */
ASX_API asx_status asx_adaptive_set_policy(const asx_adaptive_policy *policy);

/* Query the active policy. */
ASX_API asx_adaptive_policy asx_adaptive_policy_active(void);

/* Evaluate a decision surface against a posterior and evidence.
 * Selects the minimum expected-loss action, or falls back to the
 * surface's fallback action if confidence is below threshold or
 * budget is exhausted.
 *
 * Preconditions: surface, posterior, and out_decision must not be NULL.
 *   surface->action_count must be in [1, ASX_ADAPTIVE_MAX_ACTIONS].
 *   posterior->state_count must equal surface->state_count.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_ARGUMENT if any precondition fails.
 * The decision is logged to the evidence ledger. */
ASX_API ASX_MUST_USE asx_status asx_adaptive_decide(const asx_adaptive_surface *surface,
                                                    const asx_adaptive_posterior *posterior,
                                                    const asx_adaptive_evidence_term *evidence,
                                                    uint8_t evidence_count,
                                                    asx_adaptive_decision *out_decision);

/* -------------------------------------------------------------------
 * Evidence ledger queries
 * ------------------------------------------------------------------- */

/* Return the total number of decisions logged since last reset. */
ASX_API uint32_t asx_adaptive_ledger_count(void);

/* Return nonzero if the ledger has overflowed (wrapped). */
ASX_API int asx_adaptive_ledger_overflowed(void);

/* Read entry at logical index (0 = oldest readable).
 * Returns nonzero on success, 0 on out-of-bounds.
 * The returned entry's pointers (surface, evidence labels) are valid
 * only until the next asx_adaptive_decide() or asx_adaptive_reset(). */
ASX_API int asx_adaptive_ledger_get(uint32_t index, asx_adaptive_ledger_entry *out);

/* Compute FNV-1a digest over ledger contents (for replay identity). */
ASX_API uint64_t asx_adaptive_ledger_digest(void);

/* -------------------------------------------------------------------
 * Fallback state queries
 * ------------------------------------------------------------------- */

/* Return nonzero if the adaptive subsystem is currently in fallback
 * mode (confidence below threshold or budget exhausted). */
ASX_API int asx_adaptive_in_fallback(void);

/* Return the number of decisions that used the fallback path. */
ASX_API uint32_t asx_adaptive_fallback_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_ADAPTIVE_H */
