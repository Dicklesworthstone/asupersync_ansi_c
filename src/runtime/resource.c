/*
 * resource.c — resource contract engine and exhaustion semantics
 *
 * Provides deterministic resource admission checks, capacity queries,
 * and point-in-time snapshots for diagnostics and replay validation.
 *
 * All queries inspect the walking-skeleton static arenas without
 * side effects. Admission checks are pure predicates — they never
 * allocate or modify state.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/core/resource.h>
#include <asx/runtime/runtime.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Global resource queries                                             */
/* ------------------------------------------------------------------ */

uint32_t asx_resource_capacity(asx_resource_kind kind) {
    switch (kind) {
    case ASX_RESOURCE_REGION: return ASX_MAX_REGIONS;
    case ASX_RESOURCE_TASK: return ASX_MAX_TASKS;
    case ASX_RESOURCE_OBLIGATION: return ASX_MAX_OBLIGATIONS;
    case ASX_RESOURCE_KIND_COUNT: return 0;
    }
    return 0;
}

uint32_t asx_resource_used(asx_resource_kind kind) {
    switch (kind) {
    case ASX_RESOURCE_REGION: return g_region_count;
    case ASX_RESOURCE_TASK: return g_task_count;
    case ASX_RESOURCE_OBLIGATION: return g_obligation_count;
    case ASX_RESOURCE_KIND_COUNT: return 0;
    }
    return 0;
}

uint32_t asx_resource_remaining(asx_resource_kind kind) {
    uint32_t cap = asx_resource_capacity(kind);
    uint32_t used = asx_resource_used(kind);
    if (used >= cap) return 0;
    return cap - used;
}

asx_status asx_resource_snapshot_get(asx_resource_kind kind, asx_resource_snapshot *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (kind >= ASX_RESOURCE_KIND_COUNT) return ASX_E_INVALID_ARGUMENT;

    out->kind = kind;
    out->capacity = asx_resource_capacity(kind);
    out->used = asx_resource_used(kind);
    out->remaining = (out->used >= out->capacity) ? 0 : out->capacity - out->used;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Admission gate                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_resource_admit(asx_resource_kind kind, uint32_t count) {
    uint32_t remaining;

    if (count == 0) return ASX_E_INVALID_ARGUMENT;
    if (kind >= ASX_RESOURCE_KIND_COUNT) return ASX_E_INVALID_ARGUMENT;

    remaining = asx_resource_remaining(kind);
    if (count > remaining) return ASX_E_RESOURCE_EXHAUSTED;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Per-region resource queries                                         */
/* ------------------------------------------------------------------ */

asx_status asx_resource_region_capture_remaining(asx_region_id region, uint32_t *out_bytes) {
    asx_region_slot *r;
    asx_status st;

    if (out_bytes == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;

    if (r->capture_used >= ASX_REGION_CAPTURE_ARENA_BYTES) {
        *out_bytes = 0;
    } else {
        *out_bytes = ASX_REGION_CAPTURE_ARENA_BYTES - r->capture_used;
    }
    return ASX_OK;
}

asx_status asx_resource_region_cleanup_remaining(asx_region_id region, uint32_t *out_slots) {
    asx_region_slot *r;
    asx_status st;

    if (out_slots == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;

    if (r->cleanup.count >= ASX_CLEANUP_STACK_CAPACITY) {
        *out_slots = 0;
    } else {
        *out_slots = ASX_CLEANUP_STACK_CAPACITY - r->cleanup.count;
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Diagnostic                                                          */
/* ------------------------------------------------------------------ */

const char *asx_resource_kind_str(asx_resource_kind kind) {
    switch (kind) {
    case ASX_RESOURCE_REGION: return "region";
    case ASX_RESOURCE_TASK: return "task";
    case ASX_RESOURCE_OBLIGATION: return "obligation";
    case ASX_RESOURCE_KIND_COUNT: return "unknown";
    }
    return "unknown";
}
