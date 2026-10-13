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

#include "fnv_fold.h"
#include "runtime_internal.h"
#include <asx/core/ghost.h>
#include <asx/runtime/hindsight.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* -------------------------------------------------------------------
 * FNV-1a mixing for the rolling digest
 *
 * The seed is the asx digest seed shared with the trace and telemetry
 * digests (the FxHash multiplier constant, not the FNV-1a offset basis);
 * only the mixing step is FNV-1a.
 * ------------------------------------------------------------------- */

#define ASX_HINDSIGHT_DIGEST_SEED 0x517cc1b727220a95ULL

/* One event's fields, FNV-1a over each one's little-endian bytes in this
 * order (fnv_fold.h folds them without formatting them into bytes, the
 * same value as a byte loop). The field order is part of the digest
 * contract. */
static void hs_digest_fold(asx_fnv_fold *f, const asx_hindsight_event *e) {
    asx_fnv_fold_le(f, e->sequence, 4u);
    asx_fnv_fold_le(f, (uint32_t)e->kind, 4u);
    asx_fnv_fold_le(f, e->entity_id, 8u);
    asx_fnv_fold_le(f, e->observed_value, 8u);
    asx_fnv_fold_le(f, e->trace_seq, 4u);
}

/* -------------------------------------------------------------------
 * Ring buffer state
 *
 * The ring keeps the most recent ASX_HINDSIGHT_CAPACITY events. The
 * digest is rolling: every logged event is folded into it, so it covers
 * the whole log however many events the ring has already overwritten.
 * For a log that fits in the ring it equals a fold over the stored events.
 *
 * Events are folded lazily, in order: when the ring is about to overwrite
 * one, or when the digest is read. g_digest_folded events (of
 * g_write_index) are in g_digest; the rest, at most the capacity, are in
 * the ring unfolded. A clock read logs one of these events, so a run that
 * never reads the digest does not pay for it, and the value read is the
 * one an eager fold at each log gives (test_hindsight).
 * ------------------------------------------------------------------- */

static asx_hindsight_event g_ring[ASX_HINDSIGHT_CAPACITY];
static uint32_t g_write_index;   /* next slot to write (wrapping) */
static uint32_t g_total_count;   /* total events ever logged */
static uint32_t g_next_sequence; /* monotonic per-event sequence */
static asx_fnv_fold g_digest = {ASX_HINDSIGHT_DIGEST_SEED, 0u};
static uint32_t g_digest_folded; /* in g_write_index's count, wrapping with it */

/* The slot a running event count n lands in. A capacity may be set to 1
 * (asx_config.h accepts anything of at least 1), where `n % 1u` is always 0
 * and a static analyzer rightly flags it (cppcheck moduloofone): a capacity
 * of one is its own case here, so the other expressions stay free of it. */
static uint32_t hs_slot(uint32_t n) {
#if (ASX_HINDSIGHT_CAPACITY) == 1
    (void)n;
    return 0u;
#else
    return n % ASX_HINDSIGHT_CAPACITY;
#endif
}

/* Default flush policy: both triggers enabled */
static asx_hindsight_policy g_policy = {1, 1};

/* -------------------------------------------------------------------
 * Init / Reset
 * ------------------------------------------------------------------- */

void asx_hindsight_init(void) {
    /* Slots are written in order from 0, so only the first min(total,
     * capacity) are dirty; the rest are still zero. */
    uint32_t used = g_total_count < ASX_HINDSIGHT_CAPACITY ? g_total_count : ASX_HINDSIGHT_CAPACITY;

    memset(g_ring, 0, (size_t)used * sizeof(g_ring[0]));
    g_write_index = 0;
    g_total_count = 0;
    g_next_sequence = 0;
    asx_fnv_fold_init(&g_digest, ASX_HINDSIGHT_DIGEST_SEED);
    g_digest_folded = 0;
}

/* Fold the events the ring holds and the digest has not taken, oldest
 * first. */
static void hs_digest_settle(void) {
    while (g_digest_folded != g_write_index) {
        /* ASX_CHECKPOINT_WAIVER("bounded: at most ASX_HINDSIGHT_CAPACITY unfolded events") */
        hs_digest_fold(&g_digest, &g_ring[hs_slot(g_digest_folded)]);
        g_digest_folded++;
    }
}

void asx_hindsight_reset(void) { asx_hindsight_init(); }

/* -------------------------------------------------------------------
 * Event logging
 * ------------------------------------------------------------------- */

void asx_hindsight_log(asx_nd_event_kind kind, uint64_t entity_id, uint64_t observed_value) {
    uint32_t idx = hs_slot(g_write_index);
    asx_hindsight_event *e = &g_ring[idx];

    if (g_write_index - g_digest_folded >= ASX_HINDSIGHT_CAPACITY) {
        /* The ring holds ASX_HINDSIGHT_CAPACITY unfolded events and this
         * slot holds the oldest: fold it before it is overwritten. */
        hs_digest_fold(&g_digest, e);
        g_digest_folded++;
    }

    e->sequence = g_next_sequence;
    if (g_next_sequence < UINT32_MAX) g_next_sequence++;
    e->kind = kind;
    e->entity_id = entity_id;
    e->observed_value = observed_value;
    /* Sequence of the next trace event (low 32 bits, matching the trace
     * event sequence field), so the link survives ring wrap-around. */
    e->trace_seq = (uint32_t)(asx_trace_emitted_total_internal(asx_kernel_context()) & 0xFFFFFFFFu);

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
        oldest_slot = hs_slot(g_write_index);
        actual_slot = hs_slot(oldest_slot + index);
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
 * Rolling digest
 * ------------------------------------------------------------------- */

uint64_t asx_hindsight_digest(void) {
    hs_digest_settle();
    return asx_fnv_fold_value(&g_digest);
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
