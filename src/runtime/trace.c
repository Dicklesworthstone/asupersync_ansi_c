/*
 * trace.c — deterministic event trace, replay verification, and snapshot
 *
 * Records runtime events into a ring buffer with monotonic sequencing.
 * Provides FNV-1a digest for deterministic identity, replay comparison
 * against reference sequences, and JSON snapshot export.
 *
 * ASX_CHECKPOINT_WAIVER_FILE("trace-and-snapshot: all loops are bounded by "
 *   "ASX_TRACE_CAPACITY, ASX_MAX_REGIONS/TASKS/OBLIGATIONS, or integer "
 *   "conversion limits. Snapshot/export functions are observability-only, "
 *   "never called from the task poll hot path.")
 *
 * ASX_PROOF_BLOCK_WAIVER("reason: stack optimization, no semantic change")
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/asx.h>
#include <asx/portable.h>
#include <asx/runtime/snapshot.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* -------------------------------------------------------------------
 * Trace ring buffer
 * ------------------------------------------------------------------- */

static asx_trace_event g_trace_ring[ASX_TRACE_CAPACITY];
static uint32_t g_trace_count;

void asx_trace_emit(asx_trace_event_kind kind, uint64_t entity_id, uint64_t aux) {
    if (g_trace_count < ASX_TRACE_CAPACITY) {
        asx_trace_event *e = &g_trace_ring[g_trace_count];
        e->sequence = g_trace_count;
        e->kind = kind;
        e->entity_id = entity_id;
        e->aux = aux;
    }
    g_trace_count++;
}

uint32_t asx_trace_event_count(void) {
    /* Return stored count, not total emitted. Events beyond capacity
     * are silently dropped; reporting the unbounded count misleads
     * callers into iterating past readable entries. */
    return g_trace_count < ASX_TRACE_CAPACITY ? g_trace_count : ASX_TRACE_CAPACITY;
}

int asx_trace_event_get(uint32_t index, asx_trace_event *out) {
    if (out == NULL) return 0;
    if (index >= g_trace_count) return 0;
    if (index >= ASX_TRACE_CAPACITY) return 0;
    *out = g_trace_ring[index];
    return 1;
}

void asx_trace_reset(void) { g_trace_count = 0; }

/* -------------------------------------------------------------------
 * FNV-1a 64-bit digest over the trace event stream
 * ------------------------------------------------------------------- */

static uint64_t fnv1a_mix(uint64_t hash, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    for (i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

static uint64_t fnv1a_mix_u32(uint64_t hash, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    return fnv1a_mix(hash, bytes, 4);
}

static uint64_t fnv1a_mix_u64(uint64_t hash, uint64_t v) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    bytes[4] = (uint8_t)((v >> 32) & 0xFFu);
    bytes[5] = (uint8_t)((v >> 40) & 0xFFu);
    bytes[6] = (uint8_t)((v >> 48) & 0xFFu);
    bytes[7] = (uint8_t)((v >> 56) & 0xFFu);
    return fnv1a_mix(hash, bytes, 8);
}

uint64_t asx_trace_digest(void) {
    uint64_t hash = 0x517cc1b727220a95ULL; /* FNV-1a offset basis */
    uint32_t count;
    uint32_t i;

    count = g_trace_count < ASX_TRACE_CAPACITY ? g_trace_count : ASX_TRACE_CAPACITY;

    for (i = 0; i < count; i++) {
        asx_trace_event *e = &g_trace_ring[i];
        uint32_t k = (uint32_t)e->kind;
        hash = fnv1a_mix_u32(hash, e->sequence);
        hash = fnv1a_mix_u32(hash, k);
        hash = fnv1a_mix_u64(hash, e->entity_id);
        hash = fnv1a_mix_u64(hash, e->aux);
    }

    return hash;
}

/* -------------------------------------------------------------------
 * Replay verification
 * ------------------------------------------------------------------- */

static asx_trace_event g_replay_ref[ASX_TRACE_CAPACITY];
static uint32_t g_replay_ref_count;
static int g_replay_loaded;

asx_status asx_replay_load_reference(const asx_trace_event *events, uint32_t count) {
    uint32_t copy_count;

    if (events == NULL && count > 0) { return ASX_E_INVALID_ARGUMENT; }
    if (count > ASX_TRACE_CAPACITY) { return ASX_E_INVALID_ARGUMENT; }

    copy_count = count;
    if (copy_count > 0) { memcpy(g_replay_ref, events, copy_count * sizeof(asx_trace_event)); }
    g_replay_ref_count = count;
    g_replay_loaded = 1;
    return ASX_OK;
}

void asx_replay_clear_reference(void) {
    g_replay_ref_count = 0;
    g_replay_loaded = 0;
}

asx_replay_result asx_replay_verify(void) {
    asx_replay_result result;
    uint32_t check_count;
    uint32_t i;
    uint64_t expected_digest;
    uint64_t actual_digest;

    memset(&result, 0, sizeof(result));

    if (!g_replay_loaded) {
        result.result = ASX_REPLAY_MATCH;
        return result;
    }

    /* Check event count */
    if (g_trace_count != g_replay_ref_count) {
        result.result = ASX_REPLAY_LENGTH_MISMATCH;
        result.divergence_index =
            g_trace_count < g_replay_ref_count ? g_trace_count : g_replay_ref_count;
        return result;
    }

    /* Element-by-element comparison */
    check_count = g_trace_count < ASX_TRACE_CAPACITY ? g_trace_count : ASX_TRACE_CAPACITY;

    for (i = 0; i < check_count; i++) {
        asx_trace_event *actual = &g_trace_ring[i];
        asx_trace_event *expected = &g_replay_ref[i];

        if (actual->kind != expected->kind) {
            result.result = ASX_REPLAY_KIND_MISMATCH;
            result.divergence_index = i;
            return result;
        }
        if (actual->entity_id != expected->entity_id) {
            result.result = ASX_REPLAY_ENTITY_MISMATCH;
            result.divergence_index = i;
            return result;
        }
        if (actual->aux != expected->aux) {
            result.result = ASX_REPLAY_AUX_MISMATCH;
            result.divergence_index = i;
            return result;
        }
    }

    /* Compute and compare digests */
    actual_digest = asx_trace_digest();

    /* Compute expected digest from reference.
     * Must use fnv1a_mix_u32/u64 (endian-safe) to match asx_trace_digest().
     * Using raw fnv1a_mix() would hash native-endian bytes, producing
     * different digests on big-endian platforms (bd-jlc). */
    {
        uint64_t hash = 0x517cc1b727220a95ULL;
        uint32_t ref_count =
            g_replay_ref_count < ASX_TRACE_CAPACITY ? g_replay_ref_count : ASX_TRACE_CAPACITY;
        for (i = 0; i < ref_count; i++) {
            uint32_t k = (uint32_t)g_replay_ref[i].kind;
            hash = fnv1a_mix_u32(hash, g_replay_ref[i].sequence);
            hash = fnv1a_mix_u32(hash, k);
            hash = fnv1a_mix_u64(hash, g_replay_ref[i].entity_id);
            hash = fnv1a_mix_u64(hash, g_replay_ref[i].aux);
        }
        expected_digest = hash;
    }

    result.expected_digest = expected_digest;
    result.actual_digest = actual_digest;

    if (actual_digest != expected_digest) {
        result.result = ASX_REPLAY_DIGEST_MISMATCH;
        return result;
    }

    result.result = ASX_REPLAY_MATCH;
    return result;
}

uint32_t asx_replay_reference_event_count(void) { return g_replay_loaded ? g_replay_ref_count : 0u; }

int asx_replay_reference_event_get(uint32_t index, asx_trace_event *out) {
    if (out == NULL) return 0;
    if (!g_replay_loaded) return 0;
    if (index >= g_replay_ref_count) return 0;
    if (index >= ASX_TRACE_CAPACITY) return 0;
    *out = g_replay_ref[index];
    return 1;
}

/* -------------------------------------------------------------------
 * Snapshot export
 * ------------------------------------------------------------------- */

static uint32_t snap_append(asx_snapshot_buffer *buf, const char *text, uint32_t text_len) {
    uint32_t avail;
    uint32_t copy;

    if (buf->len >= ASX_SNAPSHOT_BUFFER_SIZE) return 0;
    avail = ASX_SNAPSHOT_BUFFER_SIZE - buf->len - 1u; /* reserve NUL */
    copy = text_len < avail ? text_len : avail;
    memcpy(buf->data + buf->len, text, copy);
    buf->len += copy;
    buf->data[buf->len] = '\0';
    return copy;
}

static void snap_str(asx_snapshot_buffer *buf, const char *s) {
    uint32_t len = 0;
    while (s[len] != '\0') len++;
    snap_append(buf, s, len);
}

static void snap_u32(asx_snapshot_buffer *buf, uint32_t val) {
    char tmp[16];
    int n = 0;
    char digits[16];
    int i;

    if (val == 0) {
        snap_append(buf, "0", 1);
        return;
    }

    while (val > 0 && n < 16) {
        digits[n++] = (char)('0' + (int)(val % 10u));
        val /= 10u;
    }
    for (i = n - 1; i >= 0; i--) { tmp[n - 1 - i] = digits[i]; }
    snap_append(buf, tmp, (uint32_t)n);
}

asx_status asx_snapshot_capture(asx_snapshot_buffer *out) {
    asx_runtime_snapshot snap;
    asx_status status;
    uint32_t i;
    int first;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    status = asx_runtime_snapshot_capture(&snap);
    if (status != ASX_OK) return status;

    out->len = 0;
    out->data[0] = '\0';

    snap_str(out, "{\"io_driver_initialized\":");
    snap_u32(out, (uint32_t)snap.io_driver_initialized);
    snap_str(out, ",\"io_registration_count\":");
    snap_u32(out, snap.io_registration_count);
    snap_str(out, ",\"blocking_pool_initialized\":");
    snap_u32(out, (uint32_t)snap.blocking_pool_initialized);
    snap_str(out, ",\"blocking_active_count\":");
    snap_u32(out, snap.blocking_active_count);
    snap_str(out, ",\"regions\":[");
    first = 1;
    for (i = 0; i < snap.region_count; i++) {
        const asx_snapshot_region *region = &snap.regions[i];
        if (!first) snap_str(out, ",");
        first = 0;
        snap_str(out, "{\"slot\":");
        snap_u32(out, (uint32_t)asx_handle_slot(region->id));
        snap_str(out, ",\"state\":");
        snap_u32(out, (uint32_t)region->state);
        snap_str(out, ",\"tasks\":");
        snap_u32(out, region->task_count);
        snap_str(out, ",\"gen\":");
        snap_u32(out, (uint32_t)asx_handle_generation(region->id));
        snap_str(out, "}");
    }

    snap_str(out, "],\"tasks\":[");
    first = 1;
    for (i = 0; i < snap.task_count; i++) {
        const asx_snapshot_task *task = &snap.tasks[i];
        if (!first) snap_str(out, ",");
        first = 0;
        snap_str(out, "{\"slot\":");
        snap_u32(out, (uint32_t)asx_handle_slot(task->id));
        snap_str(out, ",\"state\":");
        snap_u32(out, (uint32_t)task->state);
        snap_str(out, ",\"gen\":");
        snap_u32(out, (uint32_t)asx_handle_generation(task->id));
        snap_str(out, "}");
    }

    snap_str(out, "],\"obligations\":[");
    first = 1;
    for (i = 0; i < snap.obligation_count; i++) {
        const asx_snapshot_obligation *obligation = &snap.obligations[i];
        if (!first) snap_str(out, ",");
        first = 0;
        snap_str(out, "{\"slot\":");
        snap_u32(out, (uint32_t)asx_handle_slot(obligation->id));
        snap_str(out, ",\"state\":");
        snap_u32(out, (uint32_t)obligation->state);
        snap_str(out, ",\"gen\":");
        snap_u32(out, (uint32_t)asx_handle_generation(obligation->id));
        snap_str(out, "}");
    }

    snap_str(out, "],\"trace_count\":");
    snap_u32(out, g_trace_count);
    snap_str(out, ",\"trace_digest\":");
    {
        uint64_t d = asx_trace_digest();
        char hex[20];
        int h;
        static const char hextab[] = "0123456789abcdef";
        snap_str(out, "\"0x");
        for (h = 15; h >= 0; h--) {
            hex[0] = hextab[(d >> (uint64_t)(h * 4)) & 0xFu];
            snap_append(out, hex, 1);
        }
        snap_str(out, "\"");
    }
    snap_str(out, "}");

    return ASX_OK;
}

uint64_t asx_snapshot_digest(const asx_snapshot_buffer *snap) {
    if (snap == NULL) return 0;
    return fnv1a_mix(0x517cc1b727220a95ULL, snap->data, snap->len);
}

/* -------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------- */

const char *asx_trace_event_kind_str(asx_trace_event_kind kind) {
    switch (kind) {
    case ASX_TRACE_SCHED_POLL: return "sched_poll";
    case ASX_TRACE_SCHED_COMPLETE: return "sched_complete";
    case ASX_TRACE_SCHED_BUDGET: return "sched_budget";
    case ASX_TRACE_SCHED_QUIESCENT: return "sched_quiescent";
    case ASX_TRACE_SCHED_ROUND: return "sched_round";
    case ASX_TRACE_REGION_OPEN: return "region_open";
    case ASX_TRACE_REGION_CLOSE: return "region_close";
    case ASX_TRACE_REGION_CLOSED: return "region_closed";
    case ASX_TRACE_TASK_SPAWN: return "task_spawn";
    case ASX_TRACE_TASK_TRANSITION: return "task_transition";
    case ASX_TRACE_OBLIGATION_RESERVE: return "obligation_reserve";
    case ASX_TRACE_OBLIGATION_COMMIT: return "obligation_commit";
    case ASX_TRACE_OBLIGATION_ABORT: return "obligation_abort";
    case ASX_TRACE_CHANNEL_SEND: return "channel_send";
    case ASX_TRACE_CHANNEL_RECV: return "channel_recv";
    case ASX_TRACE_TIMER_SET: return "timer_set";
    case ASX_TRACE_TIMER_FIRE: return "timer_fire";
    case ASX_TRACE_TIMER_CANCEL: return "timer_cancel";
    default: return "unknown";
    }
}

const char *asx_replay_result_kind_str(asx_replay_result_kind kind) {
    switch (kind) {
    case ASX_REPLAY_MATCH: return "match";
    case ASX_REPLAY_LENGTH_MISMATCH: return "length_mismatch";
    case ASX_REPLAY_KIND_MISMATCH: return "kind_mismatch";
    case ASX_REPLAY_ENTITY_MISMATCH: return "entity_mismatch";
    case ASX_REPLAY_AUX_MISMATCH: return "aux_mismatch";
    case ASX_REPLAY_DIGEST_MISMATCH: return "digest_mismatch";
    default: return "unknown";
    }
}

/* -------------------------------------------------------------------
 * Binary trace persistence (bd-2n0.5)
 *
 * Little-endian wire format for cross-process trace continuity.
 * ------------------------------------------------------------------- */

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void write_le64(uint8_t *p, uint64_t v) {
    write_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    write_le32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

asx_status asx_trace_export_binary(uint8_t *buf, uint32_t capacity, uint32_t *out_len) {
    uint32_t count;
    uint32_t needed;
    uint64_t digest;
    uint32_t i;
    uint8_t *p;

    if (buf == NULL || out_len == NULL) return ASX_E_INVALID_ARGUMENT;

    count = g_trace_count < ASX_TRACE_CAPACITY ? g_trace_count : ASX_TRACE_CAPACITY;

    needed = ASX_TRACE_BINARY_HEADER + count * ASX_TRACE_BINARY_EVENT;
    if (capacity < needed) {
        *out_len = needed;
        return ASX_E_BUFFER_TOO_SMALL;
    }

    digest = asx_trace_digest();

    /* Write header */
    write_le32(buf + 0, ASX_TRACE_BINARY_MAGIC);
    write_le32(buf + 4, ASX_TRACE_BINARY_VERSION);
    write_le32(buf + 8, count);
    write_le32(buf + 12, 0); /* reserved */
    write_le64(buf + 16, digest);

    /* Write events */
    p = buf + ASX_TRACE_BINARY_HEADER;
    for (i = 0; i < count; i++) {
        asx_trace_event *e = &g_trace_ring[i];
        write_le32(p + 0, e->sequence);
        write_le32(p + 4, (uint32_t)e->kind);
        write_le64(p + 8, e->entity_id);
        write_le64(p + 16, e->aux);
        p += ASX_TRACE_BINARY_EVENT;
    }

    *out_len = needed;
    return ASX_OK;
}

asx_status asx_trace_import_binary(const uint8_t *buf, uint32_t len) {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint64_t stored_digest;
    uint32_t needed;
    uint32_t i;
    const uint8_t *p;
    uint64_t computed_digest;
    uint64_t hash;

    /* Invalidate any existing reference before potentially overwriting it */
    g_replay_loaded = 0;

    if (buf == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len < ASX_TRACE_BINARY_HEADER) return ASX_E_INVALID_ARGUMENT;

    magic = read_le32(buf + 0);
    version = read_le32(buf + 4);
    count = read_le32(buf + 8);
    stored_digest = read_le64(buf + 16);

    if (magic != ASX_TRACE_BINARY_MAGIC) return ASX_E_INVALID_ARGUMENT;
    if (version != ASX_TRACE_BINARY_VERSION) return ASX_E_INVALID_ARGUMENT;
    if (count > ASX_TRACE_CAPACITY) return ASX_E_INVALID_ARGUMENT;

    needed = ASX_TRACE_BINARY_HEADER + count * ASX_TRACE_BINARY_EVENT;
    if (len < needed) return ASX_E_INVALID_ARGUMENT;

    /* Decode events directly into g_replay_ref to save stack space */
    p = buf + ASX_TRACE_BINARY_HEADER;
    for (i = 0; i < count; i++) {
        g_replay_ref[i].sequence = read_le32(p + 0);
        g_replay_ref[i].kind = (asx_trace_event_kind)read_le32(p + 4);
        g_replay_ref[i].entity_id = read_le64(p + 8);
        g_replay_ref[i].aux = read_le64(p + 16);
        p += ASX_TRACE_BINARY_EVENT;
    }

    /* Verify digest of decoded events matches stored digest */
    hash = 0x517cc1b727220a95ULL;
    for (i = 0; i < count; i++) {
        uint32_t k = (uint32_t)g_replay_ref[i].kind;
        hash = fnv1a_mix_u32(hash, g_replay_ref[i].sequence);
        hash = fnv1a_mix_u32(hash, k);
        hash = fnv1a_mix_u64(hash, g_replay_ref[i].entity_id);
        hash = fnv1a_mix_u64(hash, g_replay_ref[i].aux);
    }
    computed_digest = hash;

    if (computed_digest != stored_digest) { return ASX_E_INVALID_ARGUMENT; }

    /* Set count and loaded flag on success */
    g_replay_ref_count = count;
    g_replay_loaded = 1;

    return ASX_OK;
}

asx_status asx_trace_continuity_check(const uint8_t *buf, uint32_t len) {
    asx_status st;
    asx_replay_result result;

    /* Import binary loads events as replay reference directly.
     * The trace ring is NOT overwritten. */
    st = asx_trace_import_binary(buf, len);
    if (st != ASX_OK) { return st; }

    result = asx_replay_verify();

    /* Clean up reference */
    asx_replay_clear_reference();

    if (result.result != ASX_REPLAY_MATCH) { return ASX_E_REPLAY_MISMATCH; }

    return ASX_OK;
}
