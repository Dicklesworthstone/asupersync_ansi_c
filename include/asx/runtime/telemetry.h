/*
 * asx/runtime/telemetry.h — multi-tier telemetry modes
 *
 * Provides three telemetry tiers with different observability costs but
 * identical canonical semantic digest behavior:
 *
 *   FORENSIC    — full event recording, all fields retained
 *   OPS_LIGHT   — key lifecycle events only (spawn, complete, region)
 *   ULTRA_MIN   — no event storage, rolling digest only
 *
 * Tier selection affects storage overhead and diagnostic detail, never
 * runtime semantics. Canonical digest computation is independent of
 * the active tier: all tiers produce the same digest for the same
 * input scenario.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_TELEMETRY_H
#define ASX_RUNTIME_TELEMETRY_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/trace.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Telemetry tiers
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_TELEMETRY_FORENSIC = 0,  /* full: all events recorded */
    ASX_TELEMETRY_OPS_LIGHT = 1, /* reduced: key lifecycle only */
    ASX_TELEMETRY_ULTRA_MIN = 2  /* minimal: rolling digest only */
} asx_telemetry_tier;

/* -------------------------------------------------------------------
 * Tier configuration API
 * ------------------------------------------------------------------- */

/* Set the active telemetry tier. Takes effect immediately.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if tier is unknown. */
ASX_API asx_status asx_telemetry_set_tier(asx_telemetry_tier tier);

/* Query the current telemetry tier. */
ASX_API asx_telemetry_tier asx_telemetry_get_tier(void);

/* Return human-readable name for a telemetry tier. */
ASX_API const char *asx_telemetry_tier_str(asx_telemetry_tier tier);

/* -------------------------------------------------------------------
 * Tier-aware event emission
 *
 * asx_telemetry_emit() replaces direct asx_trace_emit() calls when
 * telemetry filtering is desired. It:
 *   1. Always updates the rolling digest (all tiers)
 *   2. Records the event into the trace ring only if the tier retains it
 *
 * The rolling digest ensures canonical identity is tier-independent.
 * ------------------------------------------------------------------- */

/* Emit an event through the telemetry filter.
 * The event is always included in the rolling digest.
 * Whether it is recorded in the trace ring depends on the active tier. */
ASX_API void asx_telemetry_emit(asx_trace_event_kind kind, uint64_t entity_id, uint64_t aux);

/* -------------------------------------------------------------------
 * Rolling digest (tier-independent)
 *
 * Maintained across all tiers. Incorporates every event regardless
 * of whether the trace ring stores it. This is the canonical digest
 * used for replay identity verification and conformance testing.
 * ------------------------------------------------------------------- */

/* Return the rolling digest. Identical to asx_trace_digest() when
 * tier is FORENSIC; includes filtered events when tier is higher. */
ASX_API uint64_t asx_telemetry_digest(void);

/* Reset the rolling digest to the FNV-1a offset basis. */
ASX_API void asx_telemetry_digest_reset(void);

/* -------------------------------------------------------------------
 * Tier event retention query
 *
 * Allows callers to check if a given event kind is retained (stored
 * in the trace ring) under the current tier.
 * ------------------------------------------------------------------- */

/* Returns 1 if the event kind is retained in the trace ring under
 * the given tier, 0 if filtered out. */
ASX_API int asx_telemetry_retains(asx_telemetry_tier tier, asx_trace_event_kind kind);

/* -------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------- */

/* Return the number of events emitted through telemetry (all tiers). */
ASX_API uint32_t asx_telemetry_emitted_count(void);

/* Return the number of events filtered out (not stored in trace ring). */
ASX_API uint32_t asx_telemetry_filtered_count(void);

/* Reset telemetry state (counters, digest, tier to FORENSIC). */
ASX_API void asx_telemetry_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_TELEMETRY_H */
