/*
 * asx/core/resource.h — resource contract engine and exhaustion semantics
 *
 * Provides deterministic resource admission, capacity queries, and
 * failure-atomic exhaustion behavior. All resource boundaries produce
 * stable, classified error codes on exhaustion.
 *
 * Resource queries are always available (no compile-time gate) because
 * admission decisions and diagnostics depend on them at all times.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_RESOURCE_H
#define ASX_CORE_RESOURCE_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Resource kinds                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_RESOURCE_REGION = 0,
    ASX_RESOURCE_TASK = 1,
    ASX_RESOURCE_OBLIGATION = 2,
    ASX_RESOURCE_KIND_COUNT = 3
} asx_resource_kind;

/* ------------------------------------------------------------------ */
/* Resource snapshot — point-in-time view for diagnostics/replay       */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_resource_kind kind;
    uint32_t capacity;  /* hard ceiling (compile-time constant) */
    uint32_t used;      /* currently allocated count */
    uint32_t remaining; /* capacity - used */
} asx_resource_snapshot;

/* ------------------------------------------------------------------ */
/* Global resource queries                                             */
/* ------------------------------------------------------------------ */

/* Hard ceiling for a resource kind. Returns 0 for unknown kinds. */
ASX_API ASX_MUST_USE uint32_t asx_resource_capacity(asx_resource_kind kind);

/* Current allocation count for a resource kind. */
ASX_API ASX_MUST_USE uint32_t asx_resource_used(asx_resource_kind kind);

/* Remaining allocation headroom for a resource kind. */
ASX_API ASX_MUST_USE uint32_t asx_resource_remaining(asx_resource_kind kind);

/* Take a point-in-time snapshot of a resource kind.
 * Returns ASX_E_INVALID_ARGUMENT for NULL output or unknown kind. */
ASX_API ASX_MUST_USE asx_status asx_resource_snapshot_get(asx_resource_kind kind,
                                                          asx_resource_snapshot *out);

/* ------------------------------------------------------------------ */
/* Admission gate                                                      */
/* ------------------------------------------------------------------ */

/* Pre-check whether `count` more of `kind` can be allocated.
 * Returns ASX_OK if admission is possible.
 * Returns ASX_E_RESOURCE_EXHAUSTED if insufficient headroom.
 * Returns ASX_E_INVALID_ARGUMENT for count==0 or unknown kind.
 *
 * This is a pure query — no side effects, no reservation. */
ASX_API ASX_MUST_USE asx_status asx_resource_admit(asx_resource_kind kind, uint32_t count);

/* ------------------------------------------------------------------ */
/* Per-region resource queries                                         */
/* ------------------------------------------------------------------ */

/* Remaining capture arena bytes for a specific region.
 * Returns ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE on invalid region. */
ASX_API ASX_MUST_USE asx_status asx_resource_region_capture_remaining(asx_region_id region,
                                                                      uint32_t *out_bytes);

/* Remaining cleanup stack entries for a specific region. */
ASX_API ASX_MUST_USE asx_status asx_resource_region_cleanup_remaining(asx_region_id region,
                                                                      uint32_t *out_slots);

/* ------------------------------------------------------------------ */
/* Diagnostic                                                          */
/* ------------------------------------------------------------------ */

/* Human-readable name for a resource kind. Never returns NULL. */
ASX_API ASX_MUST_USE const char *asx_resource_kind_str(asx_resource_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_RESOURCE_H */
