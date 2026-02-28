/*
 * profile_compat.h — profile compatibility baseline and semantic parity enforcement
 *
 * Defines the runtime-queryable profile identity, operational property
 * descriptors, and semantic parity rules. Profiles may tune operational
 * parameters (wait policy, resource limits, trace retention) but MUST NOT
 * alter semantic behavior (lifecycle transitions, cancel protocol,
 * obligation linearity, deterministic ordering, error codes).
 *
 * The parity contract:
 *   For any shared fixture F and profiles P1, P2:
 *     canonical_digest(F, P1) == canonical_digest(F, P2)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PROFILE_COMPAT_H
#define ASX_PROFILE_COMPAT_H

#include <asx/asx_export.h>
#include <asx/asx_config.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Profile identity (runtime-queryable)
 *
 * Maps compile-time ASX_PROFILE_* macros to an enumeration for
 * use in parity reports, fixture metadata, and CI tooling.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_PROFILE_ID_CORE            = 0,
    ASX_PROFILE_ID_POSIX           = 1,
    ASX_PROFILE_ID_WIN32           = 2,
    ASX_PROFILE_ID_FREESTANDING    = 3,
    ASX_PROFILE_ID_EMBEDDED_ROUTER = 4,
    ASX_PROFILE_ID_HFT             = 5,
    ASX_PROFILE_ID_AUTOMOTIVE      = 6,
    ASX_PROFILE_ID_PARALLEL        = 7,
    ASX_PROFILE_ID_COUNT           = 8
} asx_profile_id;

/* -------------------------------------------------------------------
 * Property classification
 *
 * Every profile property is classified as either OPERATIONAL (may
 * differ between profiles) or SEMANTIC (must be identical). This
 * classification is machine-checkable and enforced by the parity
 * runner.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_PROP_OPERATIONAL = 0,  /* may differ: resource limits, wait policy */
    ASX_PROP_SEMANTIC    = 1   /* must be identical: transitions, protocol */
} asx_property_class;

/* Profile properties that may vary between profiles */
typedef enum {
    ASX_PPROP_WAIT_POLICY        = 0,
    ASX_PPROP_MAX_REGIONS        = 1,
    ASX_PPROP_MAX_TASKS          = 2,
    ASX_PPROP_MAX_OBLIGATIONS    = 3,
    ASX_PPROP_MAX_TIMERS         = 4,
    ASX_PPROP_GHOST_MONITORS     = 5,
    ASX_PPROP_TRACE_CAPACITY     = 6,
    ASX_PPROP_ALLOCATOR_SEALABLE = 7,
    ASX_PPROP_RESOURCE_CLASS     = 8,
    ASX_PPROP_COUNT              = 9
} asx_profile_property;

/* -------------------------------------------------------------------
 * Semantic rules (invariants that hold across ALL profiles)
 *
 * These are the non-negotiable semantic contracts. Any profile that
 * violates these produces a parity failure.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SRULE_LIFECYCLE_TRANSITIONS  = 0,  /* region/task state machines */
    ASX_SRULE_CANCEL_PROTOCOL        = 1,  /* cancel phase ordering */
    ASX_SRULE_OBLIGATION_LINEARITY   = 2,  /* reserve-commit/abort exactly once */
    ASX_SRULE_DETERMINISTIC_ORDERING = 3,  /* poll order, timer tiebreak */
    ASX_SRULE_HANDLE_VALIDATION      = 4,  /* generation-safe, type-tagged */
    ASX_SRULE_ERROR_CODES            = 5,  /* same errors for same misuse */
    ASX_SRULE_QUIESCENCE_DEFINITION  = 6,  /* quiescence iff all tasks+obligations resolved */
    ASX_SRULE_BUDGET_EXHAUSTION      = 7,  /* budget exhaustion semantics */
    ASX_SRULE_COUNT                  = 8
} asx_semantic_rule;

/* -------------------------------------------------------------------
 * Profile operational descriptor
 *
 * Captures the operational parameters for a profile. These MAY
 * differ between profiles without violating parity.
 * ------------------------------------------------------------------- */

typedef struct {
    asx_profile_id   id;
    const char      *name;            /* e.g. "CORE", "HFT" */
    asx_wait_policy  default_wait;
    uint32_t         max_regions;
    uint32_t         max_tasks;
    uint32_t         max_obligations;
    uint32_t         max_timers;
    int              ghost_monitors;  /* 1 if available in debug builds */
    int              allocator_sealable;
    asx_resource_class resource_class; /* R1/R2/R3 (default: R2) */
    uint32_t         trace_capacity;  /* trace ring event slots */
} asx_profile_descriptor;

/* -------------------------------------------------------------------
 * Parity check result
 * ------------------------------------------------------------------- */

typedef struct {
    int             pass;             /* 1 if parity holds, 0 if divergence */
    asx_profile_id  profile_a;
    asx_profile_id  profile_b;
    uint64_t        digest_a;
    uint64_t        digest_b;
    asx_semantic_rule divergence_rule; /* which rule was violated (if !pass) */
    uint32_t        divergence_index; /* first event index of divergence */
} asx_parity_result;

/* -------------------------------------------------------------------
 * API: Profile identity and properties
 * ------------------------------------------------------------------- */

/* Return the compile-time profile ID for the current build. */
ASX_API asx_profile_id asx_profile_active(void);

/* Return the human-readable name for a profile. Never returns NULL. */
ASX_API const char *asx_profile_name(asx_profile_id id);

/* Fill descriptor with operational parameters for the given profile.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if desc is NULL
 * or id is out of range. */
ASX_API asx_status asx_profile_get_descriptor(asx_profile_id id,
                                               asx_profile_descriptor *desc);

/* Fill descriptor scaled by a specific resource class.
 * R1 = tight limits, R2 = balanced, R3 = roomy.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if desc is NULL
 * or id/cls is out of range. */
ASX_API asx_status asx_profile_get_descriptor_for_class(
    asx_profile_id id,
    asx_resource_class cls,
    asx_profile_descriptor *desc);

/* Classify a profile property as operational or semantic. */
ASX_API asx_property_class asx_profile_property_class(asx_profile_property prop);

/* Return the name of a profile property. Never returns NULL. */
ASX_API const char *asx_profile_property_name(asx_profile_property prop);

/* -------------------------------------------------------------------
 * API: Semantic rule enforcement
 * ------------------------------------------------------------------- */

/* Check if a semantic rule is enforced. Always returns 1 (all rules
 * are compile-time guarantees, never disabled). */
ASX_API int asx_profile_semantic_rule_enforced(asx_semantic_rule rule);

/* Return the name of a semantic rule. Never returns NULL. */
ASX_API const char *asx_semantic_rule_name(asx_semantic_rule rule);

/* Return the total count of semantic rules. */
ASX_API uint32_t asx_profile_semantic_rule_count(void);

/* -------------------------------------------------------------------
 * API: Digest-based parity checking
 * ------------------------------------------------------------------- */

/* Compare two canonical digests. Fills *out with the comparison result.
 * Returns 1 if digests match (parity holds), 0 if they diverge. */
ASX_API int asx_profile_digest_compare(uint64_t digest_a,
                                        asx_profile_id profile_a,
                                        uint64_t digest_b,
                                        asx_profile_id profile_b,
                                        asx_parity_result *out);

/* Check parity of the current trace digest against an expected value.
 * Uses the telemetry rolling digest from the current execution.
 * Returns 1 if parity holds, 0 if divergence detected. */
ASX_API int asx_profile_check_parity(uint64_t expected_digest,
                                      asx_parity_result *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_PROFILE_COMPAT_H */
