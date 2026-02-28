/*
 * asx/runtime/overload_catalog.h — per-profile overload policy catalog (bd-j4m.8)
 *
 * Machine-checkable catalog of deterministic overload policies for each
 * runtime profile. Every entry specifies the overload mode, thresholds,
 * degraded-mode transition class, forbidden behavior flags, and linked
 * fixture IDs for parity verification.
 *
 * Key invariants:
 *   - Catalog is indexed by asx_profile_id — one entry per profile
 *   - Overload decisions are pure functions of (policy, load, capacity)
 *   - Forbidden behaviors are statically checked at catalog validation
 *   - Every entry has at least one linked fixture ID
 *   - Catalog version changes require linked artifact updates
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_OVERLOAD_CATALOG_H
#define ASX_RUNTIME_OVERLOAD_CATALOG_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/profile_compat.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Catalog version
 *
 * Bump when adding profiles, changing policy defaults, or modifying
 * fixture mappings. CI gates fail on version mismatch.
 * ------------------------------------------------------------------- */

#define ASX_OVERLOAD_CATALOG_VERSION  1u

/* -------------------------------------------------------------------
 * Degraded-mode transition class
 *
 * Specifies what happens when overload triggers a mode change.
 * Each profile selects exactly one transition class.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DEGRADE_NONE          = 0,  /* no transition — hard reject only */
    ASX_DEGRADE_SHED_TAIL     = 1,  /* shed oldest tasks to free capacity */
    ASX_DEGRADE_BACKPRESSURE  = 2,  /* block callers until capacity frees */
    ASX_DEGRADE_WATCHDOG_TRIP = 3   /* trigger watchdog + audit log entry */
} asx_degrade_class;

/* -------------------------------------------------------------------
 * Forbidden behavior flags
 *
 * Bitmask of behaviors that a profile MUST NOT exhibit under
 * overload. Violation of any flag is a catalog conformance failure.
 * ------------------------------------------------------------------- */

#define ASX_FORBID_NONE              0u
#define ASX_FORBID_SILENT_DROP       (1u << 0)  /* must not drop work silently */
#define ASX_FORBID_UNBOUNDED_QUEUE   (1u << 1)  /* must not queue without limit */
#define ASX_FORBID_NONDETERMINISTIC  (1u << 2)  /* must not produce random outcomes */
#define ASX_FORBID_LATENCY_SPIKE     (1u << 3)  /* must not stall scheduling loop */
#define ASX_FORBID_DEADLINE_MISS     (1u << 4)  /* must not violate deadline SLO */

/* -------------------------------------------------------------------
 * Fixture coverage record
 *
 * Links a catalog entry to one or more fixture IDs and the parity
 * gate that validates it.
 * ------------------------------------------------------------------- */

#define ASX_CATALOG_MAX_FIXTURES  4u

typedef struct {
    const char *fixture_ids[ASX_CATALOG_MAX_FIXTURES]; /* fixture family IDs */
    uint32_t    fixture_count;                          /* populated count */
    const char *parity_gate;                            /* gate name, e.g. "GATE-OVERLOAD-HFT" */
} asx_catalog_fixture_map;

/* -------------------------------------------------------------------
 * Catalog entry — one per profile
 * ------------------------------------------------------------------- */

typedef struct {
    asx_profile_id          profile;          /* which profile this governs */
    asx_overload_mode       mode;             /* default overload mode */
    uint32_t                threshold_pct;    /* overload trigger threshold (0-100) */
    uint32_t                shed_max;         /* max tasks shed per decision (SHED modes) */
    asx_degrade_class       degrade;          /* degraded-mode transition class */
    uint32_t                forbidden;        /* bitmask of forbidden behaviors */
    const char             *rationale;        /* human-readable design rationale */
    asx_catalog_fixture_map fixtures;         /* linked fixture coverage */
} asx_overload_catalog_entry;

/* -------------------------------------------------------------------
 * Catalog query API
 * ------------------------------------------------------------------- */

/* Return the catalog version. */
ASX_API uint32_t asx_overload_catalog_version(void);

/* Return the number of entries in the catalog (== ASX_PROFILE_ID_COUNT). */
ASX_API uint32_t asx_overload_catalog_count(void);

/* Look up the catalog entry for a given profile.
 * Returns ASX_OK and fills *out on success.
 * Returns ASX_E_INVALID_ARGUMENT for invalid profile or NULL out. */
ASX_API asx_status asx_overload_catalog_get(asx_profile_id id,
                                             const asx_overload_catalog_entry **out);

/* Construct an asx_overload_policy from the catalog entry for a profile.
 * Convenience helper that copies mode/threshold/shed_max. */
ASX_API asx_status asx_overload_catalog_to_policy(asx_profile_id id,
                                                   asx_overload_policy *out);

/* -------------------------------------------------------------------
 * Catalog validation
 *
 * Structural validation checks:
 *   - Every profile has an entry
 *   - No entry has empty fixture coverage
 *   - Forbidden behaviors are consistent with mode
 *   - Thresholds are in valid range (0-100)
 *   - SHED mode entries have shed_max > 0
 *   - REJECT mode entries have shed_max == 0
 *   - Degraded-mode class matches overload mode
 * ------------------------------------------------------------------- */

typedef struct {
    int      valid;                 /* 1 if all checks pass */
    uint32_t violation_count;       /* number of violations found */
    char     first_violation[128];  /* human-readable first violation */
} asx_catalog_validation_result;

/* Validate the entire catalog. */
ASX_API void asx_overload_catalog_validate(
    asx_catalog_validation_result *result);

/* Validate a single entry for structural consistency. */
ASX_API int asx_overload_catalog_entry_valid(
    const asx_overload_catalog_entry *entry);

/* -------------------------------------------------------------------
 * Deterministic outcome verification
 *
 * Given a catalog entry and load state, verify that the decision
 * matches the catalog's expected behavior.
 * ------------------------------------------------------------------- */

/* Check that a decision from asx_overload_evaluate matches the
 * catalog entry's declared mode. Returns 1 if consistent, 0 if not. */
ASX_API int asx_overload_catalog_decision_consistent(
    const asx_overload_catalog_entry *entry,
    const asx_overload_decision *decision);

/* Return the human-readable name for a degraded-mode class. */
ASX_API const char *asx_degrade_class_str(asx_degrade_class cls);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_OVERLOAD_CATALOG_H */
