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
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Runtime-owned state storage (K18, bd-9kll.3.20)                     */
/*                                                                     */
/* The region, task and obligation arenas are one block. The library's */
/* static block is bound until a runtime is initialized with its own   */
/* (asx_runtime_config.storage); lifecycle.c's g_regions, g_tasks and  */
/* g_obligations point into whichever is bound. Binding moves those    */
/* three pointers and nothing else.                                    */
/* ------------------------------------------------------------------ */

asx_kernel_storage g_kernel_static;

/* The runtime tag of the bound kernel: always the first runtime's, 0, until
 * a second runtime may be live. */
uint16_t g_kernel_tag;

typedef struct {
    char pad;
    asx_kernel_storage value;
} kernel_storage_alignment;

size_t asx_runtime_storage_size(void) { return sizeof(asx_kernel_storage); }

size_t asx_runtime_storage_align(void) { return offsetof(kernel_storage_alignment, value); }

asx_status asx_kernel_storage_validate_internal(const void *storage, size_t size) {
    uintptr_t lo;
    uintptr_t static_lo;

    if (storage == NULL) return size == 0u ? ASX_OK : ASX_E_INVALID_ARGUMENT;
    if (size < asx_runtime_storage_size()) return ASX_E_INVALID_ARGUMENT;
    lo = (uintptr_t)storage;
    if (lo % (uintptr_t)asx_runtime_storage_align() != 0u) return ASX_E_INVALID_ARGUMENT;
    static_lo = (uintptr_t)&g_kernel_static;
    if (lo < static_lo + sizeof(g_kernel_static) && lo + asx_runtime_storage_size() > static_lo) {
        return ASX_E_INVALID_ARGUMENT;
    }
    return ASX_OK;
}

void asx_kernel_bind_internal(void *storage) {
    asx_kernel_storage *block = &g_kernel_static;

    if (storage != NULL) {
        block = (asx_kernel_storage *)storage;
        memset(block, 0, sizeof(*block));
    }
    g_regions = block->regions;
    g_tasks = block->tasks;
    g_obligations = block->obligations;
    g_kernel_tag = 0u;
}

/* A raw reset is an explicit teardown: it ends a binding to a caller's
 * storage block (which stays as the runtime left it) and clears the
 * library's. Init binds its own storage first and clears with
 * asx_runtime_clear_internal, which does not rebind. */
void asx_runtime_reset(void) {
    asx_kernel_bind_internal(NULL);
    asx_runtime_clear_internal();
}

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
    case ASX_RESOURCE_REGION: return g_region_live;
    case ASX_RESOURCE_TASK: return g_task_live;
    case ASX_RESOURCE_OBLIGATION: return g_obligation_live;
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
