/*
 * asx/runtime/adapter.h — vertical acceleration adapters (bd-j4m.5)
 *
 * Optional domain-specific adapters for HFT, automotive, and embedded
 * router profiles. Each adapter provides an accelerated code path and
 * a deterministic CORE-equivalent fallback. Isomorphism proof functions
 * verify that adapter and fallback produce identical semantic digests.
 *
 * Key invariants:
 *   - Adapter decisions are pure functions (deterministic, no side effects)
 *   - Fallback paths produce identical outcomes to CORE profile behavior
 *   - Isomorphism proof artifacts are machine-checkable
 *   - Mode switches (adapter <-> fallback) do not alter semantic digests
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_ADAPTER_H
#define ASX_RUNTIME_ADAPTER_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/profile_compat.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Adapter mode — accelerated vs fallback
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ADAPTER_FALLBACK    = 0,  /* CORE-equivalent deterministic path */
    ASX_ADAPTER_ACCELERATED = 1   /* domain-specific optimized path */
} asx_adapter_mode;

/* -------------------------------------------------------------------
 * Adapter domain — which vertical this adapter serves
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ADAPTER_DOMAIN_HFT        = 0,
    ASX_ADAPTER_DOMAIN_AUTOMOTIVE  = 1,
    ASX_ADAPTER_DOMAIN_ROUTER      = 2,
    ASX_ADAPTER_DOMAIN_COUNT       = 3
} asx_adapter_domain;

/* -------------------------------------------------------------------
 * Adapter overload decision
 *
 * Unified result type for both accelerated and fallback paths.
 * Contains the raw decision plus metadata for isomorphism proof.
 * ------------------------------------------------------------------- */

typedef struct {
    int               triggered;      /* 1 if overload condition detected */
    asx_overload_mode mode;           /* which mode was applied */
    uint32_t          load_pct;       /* current load percentage */
    uint32_t          shed_count;     /* tasks shed (SHED modes only) */
    asx_status        admit_status;   /* ASX_OK or rejection status */
    asx_adapter_mode  path_used;      /* which path produced this result */
    uint64_t          decision_hash;  /* FNV-1a of decision fields for proof */
} asx_adapter_decision;

/* -------------------------------------------------------------------
 * Isomorphism proof artifact
 *
 * Records the outcome of running both accelerated and fallback paths
 * on identical input and comparing results. This is the machine-
 * checkable evidence that mode switches are semantically transparent.
 * ------------------------------------------------------------------- */

typedef struct {
    int                     pass;              /* 1 if isomorphic */
    asx_adapter_domain      domain;            /* which adapter was tested */
    asx_adapter_decision    accel_decision;    /* accelerated path result */
    asx_adapter_decision    fallback_decision; /* fallback path result */
    uint64_t                accel_hash;        /* hash of accelerated decision */
    uint64_t                fallback_hash;     /* hash of fallback decision */
    uint32_t                test_load;         /* load used for test */
    uint32_t                test_capacity;     /* capacity used for test */
} asx_adapter_isomorphism;

/* -------------------------------------------------------------------
 * HFT adapter
 *
 * Accelerated: Uses SHED_OLDEST with domain-tuned thresholds from
 *   the overload catalog. Records latency in the HFT histogram.
 * Fallback: Uses REJECT at CORE threshold (90%). No histogram.
 * ------------------------------------------------------------------- */

/* Evaluate HFT overload in accelerated mode.
 * Uses catalog-defined SHED_OLDEST policy with 85% threshold. */
ASX_API void asx_adapter_hft_decide(uint32_t used,
                                     uint32_t capacity,
                                     asx_adapter_decision *out);

/* Evaluate overload using CORE fallback (REJECT at 90%).
 * Semantically equivalent to CORE profile behavior. */
ASX_API void asx_adapter_hft_fallback(uint32_t used,
                                       uint32_t capacity,
                                       asx_adapter_decision *out);

/* -------------------------------------------------------------------
 * Automotive adapter
 *
 * Accelerated: Uses BACKPRESSURE with deadline-aware evaluation.
 *   Records compliance events in the automotive audit ring.
 * Fallback: Uses REJECT at CORE threshold (90%). No audit.
 * ------------------------------------------------------------------- */

/* Evaluate automotive overload in accelerated mode.
 * Uses catalog-defined BACKPRESSURE policy with 90% threshold.
 * Also evaluates deadline compliance if dt is non-NULL. */
ASX_API void asx_adapter_auto_decide(uint32_t used,
                                      uint32_t capacity,
                                      const asx_auto_deadline_tracker *dt,
                                      asx_adapter_decision *out);

/* Evaluate overload using CORE fallback (REJECT at 90%).
 * Ignores deadline state — semantically equivalent to CORE. */
ASX_API void asx_adapter_auto_fallback(uint32_t used,
                                        uint32_t capacity,
                                        asx_adapter_decision *out);

/* -------------------------------------------------------------------
 * Router adapter
 *
 * Accelerated: Uses REJECT with aggressive threshold (75%) and
 *   resource-class-aware capacity scaling for constrained targets.
 * Fallback: Uses REJECT at CORE threshold (90%).
 * ------------------------------------------------------------------- */

/* Evaluate router overload in accelerated mode.
 * Uses catalog-defined REJECT policy with 75% threshold. */
ASX_API void asx_adapter_router_decide(uint32_t used,
                                        uint32_t capacity,
                                        asx_resource_class rclass,
                                        asx_adapter_decision *out);

/* Evaluate overload using CORE fallback (REJECT at 90%). */
ASX_API void asx_adapter_router_fallback(uint32_t used,
                                          uint32_t capacity,
                                          asx_adapter_decision *out);

/* -------------------------------------------------------------------
 * Unified adapter dispatch
 *
 * Dispatches to the correct adapter based on domain and mode.
 * ------------------------------------------------------------------- */

/* Dispatch an overload decision to the appropriate adapter.
 * When mode is FALLBACK, uses CORE-equivalent path regardless of domain.
 * domain_ctx: NULL for HFT/router; asx_auto_deadline_tracker* for auto. */
ASX_API void asx_adapter_dispatch(asx_adapter_domain domain,
                                   asx_adapter_mode mode,
                                   uint32_t used,
                                   uint32_t capacity,
                                   const void *domain_ctx,
                                   asx_adapter_decision *out);

/* -------------------------------------------------------------------
 * Isomorphism proof API
 *
 * Run both accelerated and fallback paths on identical input, then
 * compare the semantic-equivalence-relevant fields. Produces a
 * machine-checkable proof artifact.
 * ------------------------------------------------------------------- */

/* Run isomorphism proof for a single (load, capacity) pair.
 * For automotive domain, domain_ctx is asx_auto_deadline_tracker*.
 * For router domain, domain_ctx is asx_resource_class* (pointer to class).
 * For HFT domain, domain_ctx is NULL. */
ASX_API void asx_adapter_prove_isomorphism(
    asx_adapter_domain domain,
    uint32_t load,
    uint32_t capacity,
    const void *domain_ctx,
    asx_adapter_isomorphism *proof);

/* Run isomorphism proof over a sweep of load values [0..capacity].
 * Returns 1 if all proofs pass, 0 if any diverges.
 * If failed_proof is non-NULL, the first failing proof is stored there. */
ASX_API int asx_adapter_prove_isomorphism_sweep(
    asx_adapter_domain domain,
    uint32_t capacity,
    const void *domain_ctx,
    asx_adapter_isomorphism *failed_proof);

/* -------------------------------------------------------------------
 * Adapter info and diagnostics
 * ------------------------------------------------------------------- */

/* Return the human-readable name for an adapter domain. */
ASX_API const char *asx_adapter_domain_str(asx_adapter_domain domain);

/* Return the human-readable name for an adapter mode. */
ASX_API const char *asx_adapter_mode_str(asx_adapter_mode mode);

/* Return the adapter version (bumped on behavioral changes). */
ASX_API uint32_t asx_adapter_version(void);

#define ASX_ADAPTER_VERSION 1u

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_ADAPTER_H */
