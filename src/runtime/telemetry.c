/*
 * telemetry.c — multi-tier telemetry modes with semantic-neutral guarantees
 *
 * Implements three telemetry tiers (forensic, ops-light, ultra-minimal)
 * with a tier-independent rolling digest that ensures canonical semantic
 * identity is preserved regardless of observability level.
 *
 * ASX_CHECKPOINT_WAIVER_FILE("telemetry: loops bounded by sizeof() for FNV-1a "
 *   "mixing. Emit/digest functions are observability-only, not on task poll path.")
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>

/* -------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------- */

static asx_telemetry_tier g_tier = ASX_TELEMETRY_FORENSIC;
static uint64_t g_rolling_digest = 0x517cc1b727220a95ULL;
static uint32_t g_emitted_count;
static uint32_t g_filtered_count;
static uint32_t g_rolling_sequence;

/* -------------------------------------------------------------------
 * FNV-1a helpers (local copy to avoid cross-TU dependency)
 * ------------------------------------------------------------------- */

static uint64_t telem_fnv1a_mix(uint64_t hash, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    for (i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

/* Endian-safe FNV-1a helpers — serialize to little-endian bytes before
 * hashing so that digests are identical on LE and BE platforms (bd-3h6). */
static uint64_t telem_fnv1a_mix_u32(uint64_t hash, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    return telem_fnv1a_mix(hash, bytes, 4);
}

static uint64_t telem_fnv1a_mix_u64(uint64_t hash, uint64_t v) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    bytes[4] = (uint8_t)((v >> 32) & 0xFFu);
    bytes[5] = (uint8_t)((v >> 40) & 0xFFu);
    bytes[6] = (uint8_t)((v >> 48) & 0xFFu);
    bytes[7] = (uint8_t)((v >> 56) & 0xFFu);
    return telem_fnv1a_mix(hash, bytes, 8);
}

/* -------------------------------------------------------------------
 * Tier retention policy
 *
 * FORENSIC:  all events retained
 * OPS_LIGHT: lifecycle and scheduler completion events only
 * ULTRA_MIN: no events retained (digest-only)
 * ------------------------------------------------------------------- */

int asx_telemetry_retains(asx_telemetry_tier tier, asx_trace_event_kind kind) {
    if (tier == ASX_TELEMETRY_FORENSIC) { return 1; }

    if (tier == ASX_TELEMETRY_ULTRA_MIN) { return 0; }

    /* OPS_LIGHT: retain lifecycle and scheduler terminal events */
    switch (kind) {
    case ASX_TRACE_REGION_OPEN:
    case ASX_TRACE_REGION_CLOSE:
    case ASX_TRACE_REGION_CLOSED:
    case ASX_TRACE_TASK_SPAWN:
    case ASX_TRACE_TASK_TRANSITION:
    case ASX_TRACE_SCHED_COMPLETE:
    case ASX_TRACE_SCHED_QUIESCENT:
    case ASX_TRACE_SCHED_BUDGET: return 1;

    case ASX_TRACE_SCHED_POLL:
    case ASX_TRACE_SCHED_ROUND:
    case ASX_TRACE_OBLIGATION_RESERVE:
    case ASX_TRACE_OBLIGATION_COMMIT:
    case ASX_TRACE_OBLIGATION_ABORT:
    case ASX_TRACE_CHANNEL_SEND:
    case ASX_TRACE_CHANNEL_RECV:
    case ASX_TRACE_TIMER_SET:
    case ASX_TRACE_TIMER_FIRE:
    case ASX_TRACE_TIMER_CANCEL: return 0;

    default: return 0;
    }
}

/* -------------------------------------------------------------------
 * Tier configuration
 * ------------------------------------------------------------------- */

asx_status asx_telemetry_set_tier(asx_telemetry_tier tier) {
    if (tier != ASX_TELEMETRY_FORENSIC && tier != ASX_TELEMETRY_OPS_LIGHT &&
        tier != ASX_TELEMETRY_ULTRA_MIN) {
        return ASX_E_INVALID_ARGUMENT;
    }
    g_tier = tier;
    return ASX_OK;
}

asx_telemetry_tier asx_telemetry_get_tier(void) { return g_tier; }

const char *asx_telemetry_tier_str(asx_telemetry_tier tier) {
    switch (tier) {
    case ASX_TELEMETRY_FORENSIC: return "forensic";
    case ASX_TELEMETRY_OPS_LIGHT: return "ops_light";
    case ASX_TELEMETRY_ULTRA_MIN: return "ultra_min";
    default: return "unknown";
    }
}

/* -------------------------------------------------------------------
 * Tier-aware emission
 * ------------------------------------------------------------------- */

void asx_telemetry_emit(asx_trace_event_kind kind, uint64_t entity_id, uint64_t aux) {
    uint32_t k;

    /* Always update rolling digest (tier-independent).
     * Use endian-safe helpers so digest is identical on LE/BE (bd-3h6). */
    k = (uint32_t)kind;
    g_rolling_digest = telem_fnv1a_mix_u32(g_rolling_digest, g_rolling_sequence);
    g_rolling_digest = telem_fnv1a_mix_u32(g_rolling_digest, k);
    g_rolling_digest = telem_fnv1a_mix_u64(g_rolling_digest, entity_id);
    g_rolling_digest = telem_fnv1a_mix_u64(g_rolling_digest, aux);
    if (g_rolling_sequence < UINT32_MAX) g_rolling_sequence++;
    if (g_emitted_count < UINT32_MAX) g_emitted_count++;

    /* Record in trace ring only if tier retains this event kind */
    if (asx_telemetry_retains(g_tier, kind)) {
        asx_trace_emit(kind, entity_id, aux);
    } else {
        if (g_filtered_count < UINT32_MAX) g_filtered_count++;
    }
}

/* -------------------------------------------------------------------
 * Rolling digest
 * ------------------------------------------------------------------- */

uint64_t asx_telemetry_digest(void) { return g_rolling_digest; }

void asx_telemetry_digest_reset(void) {
    g_rolling_digest = 0x517cc1b727220a95ULL;
    g_rolling_sequence = 0;
}

/* -------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------- */

uint32_t asx_telemetry_emitted_count(void) { return g_emitted_count; }

uint32_t asx_telemetry_filtered_count(void) { return g_filtered_count; }

/* -------------------------------------------------------------------
 * Reset
 * ------------------------------------------------------------------- */

void asx_telemetry_reset(void) {
    g_tier = ASX_TELEMETRY_FORENSIC;
    g_rolling_digest = 0x517cc1b727220a95ULL;
    g_emitted_count = 0;
    g_filtered_count = 0;
    g_rolling_sequence = 0;
}
