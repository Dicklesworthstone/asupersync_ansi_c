/*
 * overload_catalog.c — per-profile overload policy catalog (bd-j4m.8)
 *
 * Static catalog of deterministic overload policies for each runtime
 * profile. Machine-checkable with structural validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/overload_catalog.h>
#include <stdio.h>
#include <string.h>

/* ASX_CHECKPOINT_WAIVER_FILE("overload-catalog: all loops bounded by ASX_PROFILE_ID_COUNT (8)") */

/* -------------------------------------------------------------------
 * Per-profile overload catalog
 *
 * Design rationale for each profile's overload policy:
 *
 * CORE/POSIX/WIN32: General-purpose profiles use REJECT at 90%.
 *   Simple, deterministic, safe default. No shedding complexity.
 *
 * FREESTANDING: Constrained profile rejects at 80%.
 *   Earlier rejection protects limited resources.
 *
 * EMBEDDED_ROUTER: Very constrained, rejects at 75%.
 *   Router workloads need fast, predictable rejection to avoid
 *   buffer bloat and packet loss cascades.
 *
 * HFT: Sheds oldest tasks at 85%.
 *   Latency-sensitive: newer work has tighter deadlines.
 *   Shedding stale work preserves tail latency.
 *
 * AUTOMOTIVE: Backpressure at 90%.
 *   Safety-critical: must not drop work silently.
 *   Bounded backpressure allows deterministic degradation with
 *   watchdog-monitored recovery.
 *
 * PARALLEL: Same as CORE (REJECT at 90%).
 *   Parallel profile inherits core overload behavior.
 * ------------------------------------------------------------------- */

static const asx_overload_catalog_entry g_catalog[ASX_PROFILE_ID_COUNT] = {
    /* CORE */
    {
        ASX_PROFILE_ID_CORE,
        ASX_OVERLOAD_REJECT,
        90, 0,
        ASX_DEGRADE_NONE,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC,
        "General-purpose: hard reject at 90% prevents resource exhaustion",
        { { "fixture-overload-core", "fixture-overload-baseline", NULL, NULL }, 2, "GATE-OVERLOAD-CORE" }
    },
    /* POSIX */
    {
        ASX_PROFILE_ID_POSIX,
        ASX_OVERLOAD_REJECT,
        90, 0,
        ASX_DEGRADE_NONE,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC,
        "POSIX: same as CORE, reject at 90%",
        { { "fixture-overload-posix", "fixture-overload-baseline", NULL, NULL }, 2, "GATE-OVERLOAD-POSIX" }
    },
    /* WIN32 */
    {
        ASX_PROFILE_ID_WIN32,
        ASX_OVERLOAD_REJECT,
        90, 0,
        ASX_DEGRADE_NONE,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC,
        "WIN32: same as CORE, reject at 90%",
        { { "fixture-overload-win32", "fixture-overload-baseline", NULL, NULL }, 2, "GATE-OVERLOAD-WIN32" }
    },
    /* FREESTANDING */
    {
        ASX_PROFILE_ID_FREESTANDING,
        ASX_OVERLOAD_REJECT,
        80, 0,
        ASX_DEGRADE_NONE,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC | ASX_FORBID_UNBOUNDED_QUEUE,
        "Freestanding: earlier rejection (80%) protects constrained targets",
        { { "fixture-overload-freestanding", "fixture-overload-baseline", NULL, NULL }, 2, "GATE-OVERLOAD-FREESTANDING" }
    },
    /* EMBEDDED_ROUTER */
    {
        ASX_PROFILE_ID_EMBEDDED_ROUTER,
        ASX_OVERLOAD_REJECT,
        75, 0,
        ASX_DEGRADE_NONE,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC | ASX_FORBID_UNBOUNDED_QUEUE | ASX_FORBID_LATENCY_SPIKE,
        "Embedded router: aggressive rejection (75%) prevents buffer bloat",
        { { "fixture-overload-embedded", "fixture-overload-baseline", NULL, NULL }, 2, "GATE-OVERLOAD-EMBEDDED" }
    },
    /* HFT */
    {
        ASX_PROFILE_ID_HFT,
        ASX_OVERLOAD_SHED_OLDEST,
        85, 2,
        ASX_DEGRADE_SHED_TAIL,
        ASX_FORBID_NONDETERMINISTIC | ASX_FORBID_LATENCY_SPIKE | ASX_FORBID_UNBOUNDED_QUEUE,
        "HFT: shed oldest at 85% to preserve tail latency for newest work",
        { { "fixture-overload-hft", "fixture-hft-microburst", "fixture-overload-baseline", NULL }, 3, "GATE-OVERLOAD-HFT" }
    },
    /* AUTOMOTIVE */
    {
        ASX_PROFILE_ID_AUTOMOTIVE,
        ASX_OVERLOAD_BACKPRESSURE,
        90, 0,
        ASX_DEGRADE_WATCHDOG_TRIP,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC | ASX_FORBID_DEADLINE_MISS,
        "Automotive: backpressure at 90% with watchdog-monitored recovery",
        { { "fixture-overload-automotive", "fixture-automotive-watchdog", "fixture-overload-baseline", NULL }, 3, "GATE-OVERLOAD-AUTOMOTIVE" }
    },
    /* PARALLEL */
    {
        ASX_PROFILE_ID_PARALLEL,
        ASX_OVERLOAD_REJECT,
        90, 0,
        ASX_DEGRADE_NONE,
        ASX_FORBID_SILENT_DROP | ASX_FORBID_NONDETERMINISTIC,
        "Parallel: inherits CORE reject policy at 90%",
        { { "fixture-overload-parallel", "fixture-overload-baseline", NULL, NULL }, 2, "GATE-OVERLOAD-PARALLEL" }
    }
};

/* -------------------------------------------------------------------
 * Query API
 * ------------------------------------------------------------------- */

uint32_t asx_overload_catalog_version(void)
{
    return ASX_OVERLOAD_CATALOG_VERSION;
}

uint32_t asx_overload_catalog_count(void)
{
    return ASX_PROFILE_ID_COUNT;
}

asx_status asx_overload_catalog_get(asx_profile_id id,
                                     const asx_overload_catalog_entry **out)
{
    if (!out) return ASX_E_INVALID_ARGUMENT;
    if ((int)id < 0 || (int)id >= ASX_PROFILE_ID_COUNT) {
        return ASX_E_INVALID_ARGUMENT;
    }
    *out = &g_catalog[(int)id];
    return ASX_OK;
}

asx_status asx_overload_catalog_to_policy(asx_profile_id id,
                                           asx_overload_policy *out)
{
    if (!out) return ASX_E_INVALID_ARGUMENT;
    if ((int)id < 0 || (int)id >= ASX_PROFILE_ID_COUNT) {
        return ASX_E_INVALID_ARGUMENT;
    }
    out->mode = g_catalog[(int)id].mode;
    out->threshold_pct = g_catalog[(int)id].threshold_pct;
    out->shed_max = g_catalog[(int)id].shed_max;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Structural validation
 * ------------------------------------------------------------------- */

int asx_overload_catalog_entry_valid(const asx_overload_catalog_entry *entry)
{
    if (!entry) return 0;

    /* Profile ID in range */
    if ((int)entry->profile < 0 || (int)entry->profile >= ASX_PROFILE_ID_COUNT) {
        return 0;
    }

    /* Threshold in valid range */
    if (entry->threshold_pct > 100) return 0;

    /* Mode-specific consistency */
    switch (entry->mode) {
    case ASX_OVERLOAD_REJECT:
        /* REJECT must not shed */
        if (entry->shed_max != 0) return 0;
        /* REJECT degradation is NONE */
        if (entry->degrade != ASX_DEGRADE_NONE) return 0;
        break;
    case ASX_OVERLOAD_SHED_OLDEST:
        /* SHED must have shed_max > 0 */
        if (entry->shed_max == 0) return 0;
        /* SHED degradation is SHED_TAIL */
        if (entry->degrade != ASX_DEGRADE_SHED_TAIL) return 0;
        break;
    case ASX_OVERLOAD_BACKPRESSURE:
        /* BACKPRESSURE must not shed */
        if (entry->shed_max != 0) return 0;
        /* BACKPRESSURE degradation is BACKPRESSURE or WATCHDOG_TRIP */
        if (entry->degrade != ASX_DEGRADE_BACKPRESSURE &&
            entry->degrade != ASX_DEGRADE_WATCHDOG_TRIP) {
            return 0;
        }
        break;
    default:
        return 0;
    }

    /* Must have at least one fixture */
    if (entry->fixtures.fixture_count == 0) return 0;
    if (entry->fixtures.fixture_count > ASX_CATALOG_MAX_FIXTURES) return 0;

    /* Must have a parity gate name */
    if (!entry->fixtures.parity_gate) return 0;

    /* Must have a rationale */
    if (!entry->rationale) return 0;

    return 1;
}

void asx_overload_catalog_validate(asx_catalog_validation_result *result)
{
    uint32_t i;

    if (!result) return;

    memset(result, 0, sizeof(*result));
    result->valid = 1;

    for (i = 0; i < ASX_PROFILE_ID_COUNT; i++) {
        const asx_overload_catalog_entry *entry = &g_catalog[i];

        /* Entry profile must match its index */
        if ((int)entry->profile != (int)i) {
            result->valid = 0;
            result->violation_count++;
            if (result->violation_count == 1) {
                snprintf(result->first_violation,
                         sizeof(result->first_violation),
                         "catalog[%u]: profile mismatch (expected %u, got %d)",
                         i, i, (int)entry->profile);
            }
            continue;
        }

        if (!asx_overload_catalog_entry_valid(entry)) {
            result->valid = 0;
            result->violation_count++;
            if (result->violation_count == 1) {
                snprintf(result->first_violation,
                         sizeof(result->first_violation),
                         "catalog[%u] (%s): structural validation failed",
                         i, asx_profile_name((asx_profile_id)i));
            }
        }
    }
}

/* -------------------------------------------------------------------
 * Decision consistency check
 * ------------------------------------------------------------------- */

int asx_overload_catalog_decision_consistent(
    const asx_overload_catalog_entry *entry,
    const asx_overload_decision *decision)
{
    if (!entry || !decision) return 0;

    /* Mode must match */
    if (decision->mode != entry->mode) return 0;

    /* When not triggered, admit_status must be OK */
    if (!decision->triggered) {
        return decision->admit_status == ASX_OK;
    }

    /* When triggered, check mode-specific outcomes */
    switch (entry->mode) {
    case ASX_OVERLOAD_REJECT:
        return decision->admit_status == ASX_E_ADMISSION_CLOSED;

    case ASX_OVERLOAD_SHED_OLDEST:
        /* Shed count must be > 0 and <= entry shed_max */
        if (decision->shed_count == 0) return 0;
        if (decision->shed_count > entry->shed_max) return 0;
        return decision->admit_status == ASX_OK;

    case ASX_OVERLOAD_BACKPRESSURE:
        return decision->admit_status == ASX_E_WOULD_BLOCK;

    default:
        return 0;
    }
}

/* -------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------- */

const char *asx_degrade_class_str(asx_degrade_class cls)
{
    switch (cls) {
    case ASX_DEGRADE_NONE:          return "NONE";
    case ASX_DEGRADE_SHED_TAIL:     return "SHED_TAIL";
    case ASX_DEGRADE_BACKPRESSURE:  return "BACKPRESSURE";
    case ASX_DEGRADE_WATCHDOG_TRIP: return "WATCHDOG_TRIP";
    default:                        return "UNKNOWN";
    }
}
