/*
 * hindsight.c — hindsight nondeterminism logging ring
 *
 * Bounded wrapping ring buffer for nondeterministic boundary events.
 * Provides JSON flush for replay diagnostics and FNV-1a digest for
 * deterministic identity comparison.
 *
 * ASX_CHECKPOINT_WAIVER_FILE("hindsight-ring: all loops are bounded by "
 *   "ASX_HINDSIGHT_CAPACITY (256) or integer-conversion constants. "
 *   "Flush/digest functions are diagnostic-only, not on task poll path.")
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/ghost.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Ring buffer state
 * ------------------------------------------------------------------- */

static asx_hindsight_event g_ring[ASX_HINDSIGHT_CAPACITY];
static uint32_t g_write_index;   /* next slot to write (wrapping) */
static uint32_t g_total_count;   /* total events ever logged */
static uint32_t g_next_sequence; /* monotonic per-event sequence */

/* Default flush policy: both triggers enabled */
static asx_hindsight_policy g_policy = {1, 1};

/* -------------------------------------------------------------------
 * Init / Reset
 * ------------------------------------------------------------------- */

void asx_hindsight_init(void) {
    memset(g_ring, 0, sizeof(g_ring));
    g_write_index = 0;
    g_total_count = 0;
    g_next_sequence = 0;
}

void asx_hindsight_reset(void) { asx_hindsight_init(); }

/* -------------------------------------------------------------------
 * Event logging
 * ------------------------------------------------------------------- */

void asx_hindsight_log(asx_nd_event_kind kind, uint64_t entity_id, uint64_t observed_value) {
    uint32_t idx = g_write_index % ASX_HINDSIGHT_CAPACITY;
    asx_hindsight_event *e = &g_ring[idx];

    e->sequence = g_next_sequence;
    if (g_next_sequence < UINT32_MAX) g_next_sequence++;
    e->kind = kind;
    e->entity_id = entity_id;
    e->observed_value = observed_value;
    e->trace_seq = asx_trace_event_count();

    g_write_index++;
    if (g_total_count < UINT32_MAX) g_total_count++;
}

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

uint32_t asx_hindsight_total_count(void) { return g_total_count; }

uint32_t asx_hindsight_readable_count(void) {
    if (g_total_count <= ASX_HINDSIGHT_CAPACITY) { return g_total_count; }
    return ASX_HINDSIGHT_CAPACITY;
}

int asx_hindsight_overflowed(void) { return g_total_count > ASX_HINDSIGHT_CAPACITY; }

int asx_hindsight_get(uint32_t index, asx_hindsight_event *out) {
    uint32_t readable;
    uint32_t oldest_slot;
    uint32_t actual_slot;

    if (out == NULL) return 0;

    readable = asx_hindsight_readable_count();
    if (index >= readable) return 0;

    if (g_total_count <= ASX_HINDSIGHT_CAPACITY) {
        /* Ring has not wrapped — direct index */
        *out = g_ring[index];
    } else {
        /* Ring has wrapped — oldest is at write_index */
        oldest_slot = g_write_index % ASX_HINDSIGHT_CAPACITY;
        actual_slot = (oldest_slot + index) % ASX_HINDSIGHT_CAPACITY;
        *out = g_ring[actual_slot];
    }

    return 1;
}

/* -------------------------------------------------------------------
 * JSON flush helpers (zero-allocation, inline serialization)
 * ------------------------------------------------------------------- */

static uint32_t flush_append(asx_hindsight_flush_buffer *buf, const char *text, uint32_t text_len) {
    uint32_t avail;
    uint32_t copy;

    if (buf->len >= ASX_HINDSIGHT_FLUSH_BUFFER_SIZE) return 0;
    avail = ASX_HINDSIGHT_FLUSH_BUFFER_SIZE - buf->len - 1u;
    copy = text_len < avail ? text_len : avail;
    memcpy(buf->data + buf->len, text, copy);
    buf->len += copy;
    buf->data[buf->len] = '\0';
    return copy;
}

static void flush_str(asx_hindsight_flush_buffer *buf, const char *s) {
    uint32_t len = 0;
    while (s[len] != '\0') len++;
    flush_append(buf, s, len);
}

static void flush_u32(asx_hindsight_flush_buffer *buf, uint32_t val) {
    char digits[16];
    int n = 0;
    int i;
    char tmp[16];

    if (val == 0) {
        flush_append(buf, "0", 1);
        return;
    }
    while (val > 0 && n < 16) {
        digits[n++] = (char)('0' + (int)(val % 10u));
        val /= 10u;
    }
    for (i = n - 1; i >= 0; i--) { tmp[n - 1 - i] = digits[i]; }
    flush_append(buf, tmp, (uint32_t)n);
}

static void flush_u64(asx_hindsight_flush_buffer *buf, uint64_t val) {
    char digits[24];
    int n = 0;
    int i;
    char tmp[24];

    if (val == 0) {
        flush_append(buf, "0", 1);
        return;
    }
    while (val > 0 && n < 24) {
        digits[n++] = (char)('0' + (int)(val % 10u));
        val /= 10u;
    }
    for (i = n - 1; i >= 0; i--) { tmp[n - 1 - i] = digits[i]; }
    flush_append(buf, tmp, (uint32_t)n);
}

static void flush_hex64(asx_hindsight_flush_buffer *buf, uint64_t val) {
    static const char hextab[] = "0123456789abcdef";
    char hex[1];
    int h;

    flush_str(buf, "\"0x");
    for (h = 15; h >= 0; h--) {
        hex[0] = hextab[(val >> (uint64_t)(h * 4)) & 0xFu];
        flush_append(buf, hex, 1);
    }
    flush_str(buf, "\"");
}

/* -------------------------------------------------------------------
 * JSON flush
 * ------------------------------------------------------------------- */

asx_status asx_hindsight_flush_json(asx_hindsight_flush_buffer *out) {
    uint32_t readable;
    uint32_t i;
    asx_hindsight_event ev;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    out->len = 0;
    out->data[0] = '\0';

    readable = asx_hindsight_readable_count();

    flush_str(out, "{\"total_count\":");
    flush_u32(out, g_total_count);
    flush_str(out, ",\"overflowed\":");
    flush_str(out, asx_hindsight_overflowed() ? "true" : "false");
    flush_str(out, ",\"capacity\":");
    flush_u32(out, ASX_HINDSIGHT_CAPACITY);
    flush_str(out, ",\"digest\":");
    flush_hex64(out, asx_hindsight_digest());
    flush_str(out, ",\"events\":[");

    for (i = 0; i < readable; i++) {
        if (!asx_hindsight_get(i, &ev)) break;

        if (i > 0) flush_str(out, ",");
        flush_str(out, "{\"seq\":");
        flush_u32(out, ev.sequence);
        flush_str(out, ",\"kind\":\"");
        flush_str(out, asx_nd_event_kind_str(ev.kind));
        flush_str(out, "\",\"kind_code\":");
        flush_u32(out, (uint32_t)ev.kind);
        flush_str(out, ",\"entity_id\":");
        flush_u64(out, ev.entity_id);
        flush_str(out, ",\"observed_value\":");
        flush_u64(out, ev.observed_value);
        flush_str(out, ",\"trace_seq\":");
        flush_u32(out, ev.trace_seq);
        flush_str(out, "}");
    }

    flush_str(out, "]}");

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * FNV-1a digest
 * ------------------------------------------------------------------- */

static uint64_t hs_fnv1a_mix(uint64_t hash, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    for (i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

static uint64_t hs_fnv1a_mix_u32(uint64_t hash, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    return hs_fnv1a_mix(hash, bytes, 4);
}

static uint64_t hs_fnv1a_mix_u64(uint64_t hash, uint64_t v) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    bytes[4] = (uint8_t)((v >> 32) & 0xFFu);
    bytes[5] = (uint8_t)((v >> 40) & 0xFFu);
    bytes[6] = (uint8_t)((v >> 48) & 0xFFu);
    bytes[7] = (uint8_t)((v >> 56) & 0xFFu);
    return hs_fnv1a_mix(hash, bytes, 8);
}

uint64_t asx_hindsight_digest(void) {
    uint64_t hash = 0x517cc1b727220a95ULL;
    uint32_t readable;
    uint32_t i;
    asx_hindsight_event ev;

    readable = asx_hindsight_readable_count();

    for (i = 0; i < readable; i++) {
        uint32_t k;
        if (!asx_hindsight_get(i, &ev)) break;
        k = (uint32_t)ev.kind;
        hash = hs_fnv1a_mix_u32(hash, ev.sequence);
        hash = hs_fnv1a_mix_u32(hash, k);
        hash = hs_fnv1a_mix_u64(hash, ev.entity_id);
        hash = hs_fnv1a_mix_u64(hash, ev.observed_value);
        hash = hs_fnv1a_mix_u32(hash, ev.trace_seq);
    }

    return hash;
}

/* -------------------------------------------------------------------
 * Flush on divergence
 * ------------------------------------------------------------------- */

asx_status asx_hindsight_flush_on_divergence(const asx_hindsight_flush_buffer *replay_ctx,
                                             asx_hindsight_flush_buffer *out) {
    (void)replay_ctx;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Only flush if there are events to report */
    if (g_total_count == 0) { return ASX_E_PENDING; }

    /* Respect flush policy */
    if (!g_policy.flush_on_divergence) { return ASX_E_PENDING; }

    return asx_hindsight_flush_json(out);
}

/* -------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------- */

const char *asx_nd_event_kind_str(asx_nd_event_kind kind) {
    switch (kind) {
    case ASX_ND_CLOCK_READ: return "clock_read";
    case ASX_ND_CLOCK_SKEW: return "clock_skew";
    case ASX_ND_ENTROPY_READ: return "entropy_read";
    case ASX_ND_IO_READY: return "io_ready";
    case ASX_ND_IO_TIMEOUT: return "io_timeout";
    case ASX_ND_SIGNAL_ARRIVAL: return "signal_arrival";
    case ASX_ND_SCHED_TIE_BREAK: return "sched_tie_break";
    case ASX_ND_TIMER_COALESCE: return "timer_coalesce";
    case ASX_ND_KIND_COUNT: return "unknown";
    default: return "unknown";
    }
}

/* -------------------------------------------------------------------
 * Flush policy
 * ------------------------------------------------------------------- */

void asx_hindsight_set_policy(const asx_hindsight_policy *policy) {
    if (policy != NULL) { g_policy = *policy; }
}

asx_hindsight_policy asx_hindsight_policy_active(void) { return g_policy; }

/* -------------------------------------------------------------------
 * Invariant-failure flush
 * ------------------------------------------------------------------- */

asx_status asx_hindsight_flush_on_invariant(asx_hindsight_flush_buffer *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Check whether ghost safety monitors have recorded violations */
    if (asx_ghost_violation_count() == 0) { return ASX_E_PENDING; }

    /* Only flush if policy allows it */
    if (!g_policy.flush_on_invariant) { return ASX_E_PENDING; }

    return asx_hindsight_flush_json(out);
}

/* -------------------------------------------------------------------
 * Divergence detection
 * ------------------------------------------------------------------- */

int asx_hindsight_check_divergence(uint64_t expected_digest) {
    return asx_hindsight_digest() != expected_digest;
}
