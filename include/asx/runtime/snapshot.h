/*
 * asx/runtime/snapshot.h — deterministic runtime state snapshot
 *
 * Captures a point-in-time view of all live runtime entities
 * (regions, tasks, obligations) for replay validation and
 * conformance testing. Snapshots are serializable to JSON for
 * comparison against fixture expected_final_snapshot fields.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_SNAPSHOT_H
#define ASX_RUNTIME_SNAPSHOT_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/codec/codec.h>
#include <asx/runtime/runtime.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Snapshot entity records                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_region_id id;
    asx_region_state state;
    uint32_t task_count;
    uint32_t task_total;
    int poisoned;
} asx_snapshot_region;

typedef struct {
    asx_task_id id;
    asx_task_state state;
    asx_region_id region;
    asx_status outcome_status;
} asx_snapshot_task;

typedef struct {
    asx_obligation_id id;
    asx_obligation_state state;
    asx_region_id region;
} asx_snapshot_obligation;

/* ------------------------------------------------------------------ */
/* Aggregate snapshot                                                   */
/* ------------------------------------------------------------------ */

enum {
    ASX_SNAPSHOT_MAX_REGIONS = 8u,
    ASX_SNAPSHOT_MAX_TASKS = 64u,
    ASX_SNAPSHOT_MAX_OBLIGATIONS = 128u
};

typedef struct {
    uint32_t region_count;
    asx_snapshot_region regions[ASX_SNAPSHOT_MAX_REGIONS];
    uint32_t task_count;
    asx_snapshot_task tasks[ASX_SNAPSHOT_MAX_TASKS];
    uint32_t obligation_count;
    asx_snapshot_obligation obligations[ASX_SNAPSHOT_MAX_OBLIGATIONS];
    uint64_t event_hash; /* hash chain at capture time */
} asx_runtime_snapshot;

/* ------------------------------------------------------------------ */
/* Snapshot API                                                        */
/* ------------------------------------------------------------------ */

/* Initialize snapshot to empty state. */
ASX_API void asx_runtime_snapshot_init(asx_runtime_snapshot *snap);

/* Capture current runtime state into snapshot.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if snap is NULL. */
ASX_API ASX_MUST_USE asx_status asx_runtime_snapshot_capture(asx_runtime_snapshot *snap);

/* Serialize snapshot to JSON.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if snap or out is NULL. */
ASX_API ASX_MUST_USE asx_status asx_runtime_snapshot_to_json(const asx_runtime_snapshot *snap,
                                                             asx_codec_buffer *out);

/* Compare two snapshots for equality.
 * Returns ASX_OK if identical, ASX_E_EQUIVALENCE_MISMATCH if not. */
ASX_API ASX_MUST_USE asx_status asx_runtime_snapshot_eq(const asx_runtime_snapshot *a,
                                                        const asx_runtime_snapshot *b);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_SNAPSHOT_H */
