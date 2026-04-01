/*
 * snapshot.c — deterministic runtime state snapshot
 *
 * Captures point-in-time views of all live runtime entities
 * (regions, tasks, obligations) for replay validation and
 * conformance testing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/runtime/blocking.h>
#include <asx/runtime/event.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/snapshot.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void asx_runtime_snapshot_init(asx_runtime_snapshot *snap) {
    if (snap == NULL) return;
    memset(snap, 0, sizeof(*snap));
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_snapshot_capture(asx_runtime_snapshot *snap) {
    uint32_t i;
    uint32_t ri = 0;
    uint32_t ti = 0;
    uint32_t oi = 0;

    if (snap == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_runtime_snapshot_init(snap);

#if ASX_HAS_NATIVE_IO_DRIVER
    snap->io_driver_initialized = asx_io_driver_is_initialized();
    snap->io_registration_count = asx_io_active_count();
#else
    snap->io_driver_initialized = 0;
    snap->io_registration_count = 0u;
#endif
#if ASX_HAS_BLOCKING_SURFACE
    snap->blocking_pool_initialized = asx_blocking_pool_is_initialized();
    snap->blocking_active_count = asx_blocking_active_count();
#else
    snap->blocking_pool_initialized = 0;
    snap->blocking_active_count = 0u;
#endif

    /* Capture live regions */
    for (i = 0; i < ASX_MAX_REGIONS && ri < ASX_SNAPSHOT_MAX_REGIONS; i++) {
        if (!g_regions[i].alive) continue;
        snap->regions[ri].id =
            asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                            asx_handle_pack_index(g_regions[i].generation, (uint16_t)i));
        snap->regions[ri].state = g_regions[i].state;
        snap->regions[ri].task_count = g_regions[i].task_count;
        snap->regions[ri].task_total = g_regions[i].task_total;
        snap->regions[ri].poisoned = g_regions[i].poisoned;
        ri++;
    }
    snap->region_count = ri;

    /* Capture live tasks */
    for (i = 0; i < ASX_MAX_TASKS && ti < ASX_SNAPSHOT_MAX_TASKS; i++) {
        if (!g_tasks[i].alive) continue;
        snap->tasks[ti].id =
            asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                            asx_handle_pack_index(g_tasks[i].generation, (uint16_t)i));
        snap->tasks[ti].state = g_tasks[i].state;
        snap->tasks[ti].region = g_tasks[i].region;
        snap->tasks[ti].outcome_severity = g_tasks[i].outcome.severity;
        ti++;
    }
    snap->task_count = ti;

    /* Capture live obligations */
    for (i = 0; i < ASX_MAX_OBLIGATIONS && oi < ASX_SNAPSHOT_MAX_OBLIGATIONS; i++) {
        if (!g_obligations[i].alive) continue;
        snap->obligations[oi].id =
            asx_handle_pack(ASX_TYPE_OBLIGATION,
                            (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                            asx_handle_pack_index(g_obligations[i].generation, (uint16_t)i));
        snap->obligations[oi].state = g_obligations[i].state;
        snap->obligations[oi].region = g_obligations[i].region;
        oi++;
    }
    snap->obligation_count = oi;

    /* Capture trace hash chain */
    snap->event_hash = asx_trace_digest();

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* JSON serialization                                                  */
/* ------------------------------------------------------------------ */

static const char *region_state_str(asx_region_state s) {
    switch (s) {
    case ASX_REGION_OPEN: return "open";
    case ASX_REGION_CLOSING: return "closing";
    case ASX_REGION_DRAINING: return "draining";
    case ASX_REGION_FINALIZING: return "finalizing";
    case ASX_REGION_CLOSED: return "closed";
    }
    return "unknown";
}

static const char *task_state_str(asx_task_state s) {
    switch (s) {
    case ASX_TASK_CREATED: return "created";
    case ASX_TASK_RUNNING: return "running";
    case ASX_TASK_CANCEL_REQUESTED: return "cancel_requested";
    case ASX_TASK_CANCELLING: return "cancelling";
    case ASX_TASK_FINALIZING: return "finalizing";
    case ASX_TASK_COMPLETED: return "completed";
    }
    return "unknown";
}

static const char *obligation_state_str(asx_obligation_state s) {
    switch (s) {
    case ASX_OBLIGATION_RESERVED: return "reserved";
    case ASX_OBLIGATION_COMMITTED: return "committed";
    case ASX_OBLIGATION_ABORTED: return "aborted";
    case ASX_OBLIGATION_LEAKED: return "leaked";
    }
    return "unknown";
}

static asx_status validate_snapshot_counts(const asx_runtime_snapshot *snap) {
    if (snap == NULL) return ASX_E_INVALID_ARGUMENT;
    if (snap->region_count > ASX_SNAPSHOT_MAX_REGIONS) return ASX_E_INVALID_ARGUMENT;
    if (snap->task_count > ASX_SNAPSHOT_MAX_TASKS) return ASX_E_INVALID_ARGUMENT;
    if (snap->obligation_count > ASX_SNAPSHOT_MAX_OBLIGATIONS) return ASX_E_INVALID_ARGUMENT;
    return ASX_OK;
}

asx_status asx_runtime_snapshot_to_json(const asx_runtime_snapshot *snap, asx_codec_buffer *out) {
    asx_status s;
    uint32_t i;
    int first;

    if (snap == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = validate_snapshot_counts(snap);
    if (s != ASX_OK) return s;

    s = asx_codec_buffer_append_char(out, '{');
    if (s != ASX_OK) return s;

    first = 1;

    /* event_hash */
    s = asx_codec_buffer_append_u64_field(out, &first, "event_hash", snap->event_hash);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_u64_field(out, &first, "io_driver_initialized",
                                          (uint64_t)snap->io_driver_initialized);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_u64_field(out, &first, "io_registration_count",
                                          (uint64_t)snap->io_registration_count);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_u64_field(out, &first, "blocking_pool_initialized",
                                          (uint64_t)snap->blocking_pool_initialized);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_u64_field(out, &first, "blocking_active_count",
                                          (uint64_t)snap->blocking_active_count);
    if (s != ASX_OK) return s;

    /* regions array */
    s = asx_codec_buffer_append_field_prefix(out, &first);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_cstr(out, "\"regions\":[");
    if (s != ASX_OK) return s;

    for (i = 0; i < snap->region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded JSON serialization") */
        int rf = 1;
        const asx_snapshot_region *r = &snap->regions[i];
        if (i > 0) {
            s = asx_codec_buffer_append_char(out, ',');
            if (s != ASX_OK) return s;
        }
        s = asx_codec_buffer_append_char(out, '{');
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &rf, "id", r->id);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_string_field(out, &rf, "state", region_state_str(r->state));
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &rf, "task_count", (uint64_t)r->task_count);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &rf, "task_total", (uint64_t)r->task_total);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &rf, "poisoned", (uint64_t)r->poisoned);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_char(out, '}');
        if (s != ASX_OK) return s;
    }

    s = asx_codec_buffer_append_char(out, ']');
    if (s != ASX_OK) return s;

    /* tasks array */
    s = asx_codec_buffer_append_field_prefix(out, &first);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_cstr(out, "\"tasks\":[");
    if (s != ASX_OK) return s;

    for (i = 0; i < snap->task_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded JSON serialization") */
        int tf = 1;
        const asx_snapshot_task *t = &snap->tasks[i];
        if (i > 0) {
            s = asx_codec_buffer_append_char(out, ',');
            if (s != ASX_OK) return s;
        }
        s = asx_codec_buffer_append_char(out, '{');
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &tf, "id", t->id);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_string_field(out, &tf, "state", task_state_str(t->state));
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &tf, "region", t->region);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &tf, "outcome_severity",
                                              (uint64_t)(uint32_t)t->outcome_severity);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_char(out, '}');
        if (s != ASX_OK) return s;
    }

    s = asx_codec_buffer_append_char(out, ']');
    if (s != ASX_OK) return s;

    /* obligations array */
    s = asx_codec_buffer_append_field_prefix(out, &first);
    if (s != ASX_OK) return s;
    s = asx_codec_buffer_append_cstr(out, "\"obligations\":[");
    if (s != ASX_OK) return s;

    for (i = 0; i < snap->obligation_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded JSON serialization") */
        int of = 1;
        const asx_snapshot_obligation *o = &snap->obligations[i];
        if (i > 0) {
            s = asx_codec_buffer_append_char(out, ',');
            if (s != ASX_OK) return s;
        }
        s = asx_codec_buffer_append_char(out, '{');
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &of, "id", o->id);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_string_field(out, &of, "state", obligation_state_str(o->state));
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_u64_field(out, &of, "region", o->region);
        if (s != ASX_OK) return s;
        s = asx_codec_buffer_append_char(out, '}');
        if (s != ASX_OK) return s;
    }

    s = asx_codec_buffer_append_char(out, ']');
    if (s != ASX_OK) return s;

    s = asx_codec_buffer_append_char(out, '}');
    return s;
}

/* ------------------------------------------------------------------ */
/* Equality comparison                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_runtime_snapshot_eq(const asx_runtime_snapshot *a, const asx_runtime_snapshot *b) {
    uint32_t i;
    asx_status s;

    if (a == NULL || b == NULL) return ASX_E_INVALID_ARGUMENT;
    s = validate_snapshot_counts(a);
    if (s != ASX_OK) return s;
    s = validate_snapshot_counts(b);
    if (s != ASX_OK) return s;

    if (a->event_hash != b->event_hash) return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->io_driver_initialized != b->io_driver_initialized) return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->io_registration_count != b->io_registration_count) return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->blocking_pool_initialized != b->blocking_pool_initialized)
        return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->blocking_active_count != b->blocking_active_count) return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->region_count != b->region_count) return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->task_count != b->task_count) return ASX_E_EQUIVALENCE_MISMATCH;
    if (a->obligation_count != b->obligation_count) return ASX_E_EQUIVALENCE_MISMATCH;

    for (i = 0; i < a->region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        if (a->regions[i].id != b->regions[i].id) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->regions[i].state != b->regions[i].state) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->regions[i].task_count != b->regions[i].task_count) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->regions[i].task_total != b->regions[i].task_total) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->regions[i].poisoned != b->regions[i].poisoned) return ASX_E_EQUIVALENCE_MISMATCH;
    }

    for (i = 0; i < a->task_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        if (a->tasks[i].id != b->tasks[i].id) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->tasks[i].state != b->tasks[i].state) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->tasks[i].region != b->tasks[i].region) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->tasks[i].outcome_severity != b->tasks[i].outcome_severity)
            return ASX_E_EQUIVALENCE_MISMATCH;
    }

    for (i = 0; i < a->obligation_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded aggregation") */
        if (a->obligations[i].id != b->obligations[i].id) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->obligations[i].state != b->obligations[i].state) return ASX_E_EQUIVALENCE_MISMATCH;
        if (a->obligations[i].region != b->obligations[i].region) return ASX_E_EQUIVALENCE_MISMATCH;
    }

    return ASX_OK;
}
