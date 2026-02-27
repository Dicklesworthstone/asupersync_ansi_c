/*
 * trace.c — deterministic event trace, replay verification, and snapshot
 *
 * Records runtime events into a ring buffer with monotonic sequencing.
 * Provides FNV-1a digest for deterministic identity, replay comparison
 * against reference sequences, and JSON snapshot export.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/trace.h>
#include <string.h>
#include "runtime_internal.h"

/* -------------------------------------------------------------------
 * Trace ring buffer
 * ------------------------------------------------------------------- */

static asx_trace_event g_trace_ring[ASX_TRACE_CAPACITY];
static uint32_t g_trace_count;

void asx_trace_emit(asx_trace_event_kind kind,
                     uint64_t entity_id,
                     uint64_t aux)
{
    if (g_trace_count < ASX_TRACE_CAPACITY) {
        asx_trace_event *e = &g_trace_ring[g_trace_count];
        e->sequence  = g_trace_count;
        e->kind      = kind;
        e->entity_id = entity_id;
        e->aux       = aux;
    }
    g_trace_count++;
}

uint32_t asx_trace_event_count(void)
{
    return g_trace_count;
}

int asx_trace_event_get(uint32_t index, asx_trace_event *out)
{
    if (out == NULL) return 0;
    if (index >= g_trace_count) return 0;
    if (index >= ASX_TRACE_CAPACITY) return 0;
    *out = g_trace_ring[index];
    return 1;
}

void asx_trace_reset(void)
{
    g_trace_count = 0;
}

/* -------------------------------------------------------------------
 * FNV-1a 64-bit digest over the trace event stream
 * ------------------------------------------------------------------- */

static uint64_t fnv1a_mix(uint64_t hash, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    for (i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x00000100000001B3ULL;
    }
    return hash;
}

uint64_t asx_trace_digest(void)
{
    uint64_t hash = 0x517cc1b727220a95ULL; /* FNV-1a offset basis */
    uint32_t count;
    uint32_t i;

    count = g_trace_count < ASX_TRACE_CAPACITY
            ? g_trace_count
            : ASX_TRACE_CAPACITY;

    for (i = 0; i < count; i++) {
        asx_trace_event *e = &g_trace_ring[i];
        uint32_t k = (uint32_t)e->kind;
        hash = fnv1a_mix(hash, &e->sequence, sizeof(e->sequence));
        hash = fnv1a_mix(hash, &k, sizeof(k));
        hash = fnv1a_mix(hash, &e->entity_id, sizeof(e->entity_id));
        hash = fnv1a_mix(hash, &e->aux, sizeof(e->aux));
    }

    return hash;
}

/* -------------------------------------------------------------------
 * Replay verification
 * ------------------------------------------------------------------- */

static asx_trace_event g_replay_ref[ASX_TRACE_CAPACITY];
static uint32_t g_replay_ref_count;
static int      g_replay_loaded;

asx_status asx_replay_load_reference(const asx_trace_event *events,
                                      uint32_t count)
{
    uint32_t copy_count;

    if (events == NULL && count > 0) {
        return ASX_E_INVALID_ARGUMENT;
    }

    copy_count = count < ASX_TRACE_CAPACITY ? count : ASX_TRACE_CAPACITY;
    if (copy_count > 0) {
        memcpy(g_replay_ref, events, copy_count * sizeof(asx_trace_event));
    }
    g_replay_ref_count = count;
    g_replay_loaded = 1;
    return ASX_OK;
}

void asx_replay_clear_reference(void)
{
    g_replay_ref_count = 0;
    g_replay_loaded = 0;
}

asx_replay_result asx_replay_verify(void)
{
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
        result.divergence_index = g_trace_count < g_replay_ref_count
                                  ? g_trace_count
                                  : g_replay_ref_count;
        return result;
    }

    /* Element-by-element comparison */
    check_count = g_trace_count < ASX_TRACE_CAPACITY
                  ? g_trace_count
                  : ASX_TRACE_CAPACITY;

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

    /* Compute expected digest from reference */
    {
        uint64_t hash = 0x517cc1b727220a95ULL;
        uint32_t ref_count = g_replay_ref_count < ASX_TRACE_CAPACITY
                             ? g_replay_ref_count
                             : ASX_TRACE_CAPACITY;
        for (i = 0; i < ref_count; i++) {
            uint32_t k = (uint32_t)g_replay_ref[i].kind;
            hash = fnv1a_mix(hash, &g_replay_ref[i].sequence,
                             sizeof(g_replay_ref[i].sequence));
            hash = fnv1a_mix(hash, &k, sizeof(k));
            hash = fnv1a_mix(hash, &g_replay_ref[i].entity_id,
                             sizeof(g_replay_ref[i].entity_id));
            hash = fnv1a_mix(hash, &g_replay_ref[i].aux,
                             sizeof(g_replay_ref[i].aux));
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

/* -------------------------------------------------------------------
 * Snapshot export
 * ------------------------------------------------------------------- */

static uint32_t snap_append(asx_snapshot_buffer *buf,
                             const char *text,
                             uint32_t text_len)
{
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

static void snap_str(asx_snapshot_buffer *buf, const char *s)
{
    uint32_t len = 0;
    while (s[len] != '\0') len++;
    snap_append(buf, s, len);
}

static void snap_u32(asx_snapshot_buffer *buf, uint32_t val)
{
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
    for (i = n - 1; i >= 0; i--) {
        tmp[n - 1 - i] = digits[i];
    }
    snap_append(buf, tmp, (uint32_t)n);
}

asx_status asx_snapshot_capture(asx_snapshot_buffer *out)
{
    uint32_t i;
    int first;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    out->len = 0;
    out->data[0] = '\0';

    snap_str(out, "{\"regions\":[");
    first = 1;
    for (i = 0; i < g_region_count; i++) {
        if (!g_regions[i].alive) continue;
        if (!first) snap_str(out, ",");
        first = 0;
        snap_str(out, "{\"slot\":");
        snap_u32(out, i);
        snap_str(out, ",\"state\":");
        snap_u32(out, (uint32_t)g_regions[i].state);
        snap_str(out, ",\"tasks\":");
        snap_u32(out, g_regions[i].task_count);
        snap_str(out, ",\"gen\":");
        snap_u32(out, (uint32_t)g_regions[i].generation);
        snap_str(out, "}");
    }

    snap_str(out, "],\"tasks\":[");
    first = 1;
    for (i = 0; i < g_task_count; i++) {
        if (!g_tasks[i].alive) continue;
        if (!first) snap_str(out, ",");
        first = 0;
        snap_str(out, "{\"slot\":");
        snap_u32(out, i);
        snap_str(out, ",\"state\":");
        snap_u32(out, (uint32_t)g_tasks[i].state);
        snap_str(out, ",\"gen\":");
        snap_u32(out, (uint32_t)g_tasks[i].generation);
        snap_str(out, "}");
    }

    snap_str(out, "],\"obligations\":[");
    first = 1;
    for (i = 0; i < g_obligation_count; i++) {
        if (!g_obligations[i].alive) continue;
        if (!first) snap_str(out, ",");
        first = 0;
        snap_str(out, "{\"slot\":");
        snap_u32(out, i);
        snap_str(out, ",\"state\":");
        snap_u32(out, (uint32_t)g_obligations[i].state);
        snap_str(out, ",\"gen\":");
        snap_u32(out, (uint32_t)g_obligations[i].generation);
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

uint64_t asx_snapshot_digest(const asx_snapshot_buffer *snap)
{
    if (snap == NULL) return 0;
    return fnv1a_mix(0x517cc1b727220a95ULL, snap->data, snap->len);
}

/* -------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------- */

const char *asx_trace_event_kind_str(asx_trace_event_kind kind)
{
    switch (kind) {
    case ASX_TRACE_SCHED_POLL:         return "sched_poll";
    case ASX_TRACE_SCHED_COMPLETE:     return "sched_complete";
    case ASX_TRACE_SCHED_BUDGET:       return "sched_budget";
    case ASX_TRACE_SCHED_QUIESCENT:    return "sched_quiescent";
    case ASX_TRACE_SCHED_ROUND:        return "sched_round";
    case ASX_TRACE_REGION_OPEN:        return "region_open";
    case ASX_TRACE_REGION_CLOSE:       return "region_close";
    case ASX_TRACE_REGION_CLOSED:      return "region_closed";
    case ASX_TRACE_TASK_SPAWN:         return "task_spawn";
    case ASX_TRACE_TASK_TRANSITION:    return "task_transition";
    case ASX_TRACE_OBLIGATION_RESERVE: return "obligation_reserve";
    case ASX_TRACE_OBLIGATION_COMMIT:  return "obligation_commit";
    case ASX_TRACE_OBLIGATION_ABORT:   return "obligation_abort";
    case ASX_TRACE_CHANNEL_SEND:       return "channel_send";
    case ASX_TRACE_CHANNEL_RECV:       return "channel_recv";
    case ASX_TRACE_TIMER_SET:          return "timer_set";
    case ASX_TRACE_TIMER_FIRE:         return "timer_fire";
    case ASX_TRACE_TIMER_CANCEL:       return "timer_cancel";
    default:                           return "unknown";
    }
}

const char *asx_replay_result_kind_str(asx_replay_result_kind kind)
{
    switch (kind) {
    case ASX_REPLAY_MATCH:             return "match";
    case ASX_REPLAY_LENGTH_MISMATCH:   return "length_mismatch";
    case ASX_REPLAY_KIND_MISMATCH:     return "kind_mismatch";
    case ASX_REPLAY_ENTITY_MISMATCH:   return "entity_mismatch";
    case ASX_REPLAY_AUX_MISMATCH:      return "aux_mismatch";
    case ASX_REPLAY_DIGEST_MISMATCH:   return "digest_mismatch";
    default:                           return "unknown";
    }
}
