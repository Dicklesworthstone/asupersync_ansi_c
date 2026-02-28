/*
 * asx/runtime/vertical_adapter.h — vertical acceleration adapters (bd-j4m.5)
 *
 * Optional domain-specific adapters for HFT, automotive, and embedded
 * router profiles. Each adapter provides an accelerated code path with
 * domain annotations and a catalog-aligned fallback. Isomorphism proof
 * functions verify that both paths produce identical overload decisions.
 *
 * Key invariants:
 *   - Adapter decisions are pure functions (deterministic, no side effects)
 *   - Fallback paths use catalog-defined policies for each profile
 *   - Isomorphism proofs are machine-checkable
 *   - Mode switches (accelerated <-> fallback) produce identical decisions
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_VERTICAL_ADAPTER_H
#define ASX_RUNTIME_VERTICAL_ADAPTER_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/profile_compat.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Adapter identity
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ADAPTER_HFT        = 0,
    ASX_ADAPTER_AUTOMOTIVE  = 1,
    ASX_ADAPTER_ROUTER      = 2,
    ASX_ADAPTER_COUNT       = 3
} asx_adapter_id;

typedef enum {
    ASX_ADAPTER_MODE_FALLBACK    = 0,
    ASX_ADAPTER_MODE_ACCELERATED = 1
} asx_adapter_mode;

/* -------------------------------------------------------------------
 * Adapter descriptor
 * ------------------------------------------------------------------- */

typedef struct {
    asx_adapter_id   id;
    asx_profile_id   profile;
    const char      *name;
    const char      *description;
} asx_adapter_descriptor;

/* -------------------------------------------------------------------
 * Adapter annotations — domain-specific diagnostics
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t p99_ns;
    uint32_t overflow_count;
} asx_adapter_hft_annotations;

typedef struct {
    uint32_t miss_rate_pct100;
    uint32_t audit_count;
} asx_adapter_auto_annotations;

typedef struct {
    uint32_t queue_depth;
    uint32_t headroom;
    uint32_t reject_streak;
} asx_adapter_router_annotations;

typedef union {
    asx_adapter_hft_annotations    hft;
    asx_adapter_auto_annotations   automotive;
    asx_adapter_router_annotations router;
} asx_adapter_annotations;

/* -------------------------------------------------------------------
 * Adapter result
 * ------------------------------------------------------------------- */

typedef struct {
    asx_adapter_id        adapter;
    asx_adapter_mode      mode;
    asx_overload_decision decision;
    int                   has_annotations;
    asx_adapter_annotations annotations;
    uint64_t              decision_digest;
} asx_adapter_result;

/* -------------------------------------------------------------------
 * Isomorphism types
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t used;
    uint32_t capacity;
} asx_adapter_scenario;

typedef struct {
    int             pass;
    asx_adapter_id  adapter;
    uint32_t        evaluations;
    uint32_t        matches;
    uint32_t        divergence_index;
} asx_isomorphism_result;

/* -------------------------------------------------------------------
 * Adapter query API
 * ------------------------------------------------------------------- */

/* Return the number of adapters (== ASX_ADAPTER_COUNT). */
ASX_API uint32_t asx_adapter_count(void);

/* Get the descriptor for an adapter. */
ASX_API asx_status asx_adapter_get_descriptor(asx_adapter_id id,
                                               asx_adapter_descriptor *out);

/* Return the human-readable name for an adapter. */
ASX_API const char *asx_adapter_name(asx_adapter_id id);

/* Return the human-readable name for an adapter mode. */
ASX_API const char *asx_adapter_mode_str(asx_adapter_mode mode);

/* Return the profile associated with an adapter. */
ASX_API asx_profile_id asx_adapter_profile(asx_adapter_id id);

/* -------------------------------------------------------------------
 * Evaluation API
 * ------------------------------------------------------------------- */

/* Evaluate overload using the specified adapter and mode.
 * In FALLBACK mode, uses catalog-defined policy.
 * In ACCELERATED mode, uses domain-tuned policy with annotations. */
ASX_API asx_status asx_adapter_evaluate(asx_adapter_id id,
                                         asx_adapter_mode mode,
                                         uint32_t used,
                                         uint32_t capacity,
                                         asx_adapter_result *out);

/* Evaluate both fallback and accelerated modes. */
ASX_API asx_status asx_adapter_evaluate_both(asx_adapter_id id,
                                              uint32_t used,
                                              uint32_t capacity,
                                              asx_adapter_result *fallback_out,
                                              asx_adapter_result *accel_out);

/* Reset all adapter state (global HFT/automotive instrument state). */
ASX_API void asx_adapter_reset_all(void);

/* -------------------------------------------------------------------
 * Isomorphism proof API
 * ------------------------------------------------------------------- */

/* Run isomorphism proof using built-in scenarios [0..100] capacity 100. */
ASX_API int asx_adapter_isomorphism_builtin(asx_adapter_id id,
                                             asx_isomorphism_result *out);

/* Run isomorphism proof over custom scenarios. */
ASX_API int asx_adapter_isomorphism_check(asx_adapter_id id,
                                           const asx_adapter_scenario *scenarios,
                                           uint32_t count,
                                           asx_isomorphism_result *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_VERTICAL_ADAPTER_H */
