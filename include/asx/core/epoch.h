/*
 * asx/core/epoch.h — epoch-based execution phases and barrier triggers
 *
 * An epoch represents a bounded execution phase. Tasks within an epoch
 * share a phase counter and can synchronize at epoch boundaries via
 * barrier triggers. Epochs provide scoped fairness, failure containment,
 * and deterministic phase transitions.
 *
 * Walking skeleton: single-threaded, synchronous epoch advancement.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_EPOCH_H
#define ASX_CORE_EPOCH_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_EPOCHS 16u
#define ASX_MAX_EPOCH_OBSERVERS 8u

/* -------------------------------------------------------------------
 * Epoch state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_EPOCH_CREATED = 0,
    ASX_EPOCH_ACTIVE = 1,
    ASX_EPOCH_ADVANCING = 2,
    ASX_EPOCH_CLOSED = 3
} asx_epoch_state;

/* -------------------------------------------------------------------
 * Epoch handle
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_epoch_handle;

/* -------------------------------------------------------------------
 * Epoch policy — controls advancement and failure behavior
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_EPOCH_ADVANCE_MANUAL = 0,    /* caller-driven advance */
    ASX_EPOCH_ADVANCE_BARRIER = 1,   /* advance when all observers arrive */
    ASX_EPOCH_ADVANCE_THRESHOLD = 2  /* advance when N observers arrive */
} asx_epoch_advance_mode;

typedef struct {
    asx_epoch_advance_mode mode;
    uint32_t threshold;  /* for THRESHOLD mode: required arrivals */
    uint32_t max_phases; /* 0 = unlimited */
} asx_epoch_policy;

/* -------------------------------------------------------------------
 * Epoch observer callback — notified on phase transitions
 * ------------------------------------------------------------------- */

typedef void (*asx_epoch_observer_fn)(uint64_t epoch_id, uint64_t old_phase,
                                      uint64_t new_phase, void *user_data);

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

ASX_API ASX_MUST_USE asx_status asx_epoch_create(const asx_epoch_policy *policy,
                                                  asx_epoch_handle *out);

ASX_API asx_status asx_epoch_close(asx_epoch_handle handle);

ASX_API asx_epoch_state asx_epoch_get_state(asx_epoch_handle handle);

/* -------------------------------------------------------------------
 * API: Phase management
 * ------------------------------------------------------------------- */

/* Get the current phase number (starts at 0). */
ASX_API uint64_t asx_epoch_current_phase(asx_epoch_handle handle);

/* Manually advance to the next phase (MANUAL mode).
 * Returns ASX_E_INVALID_STATE if not in MANUAL mode or epoch closed.
 * Returns ASX_E_RESOURCE_EXHAUSTED if max_phases reached. */
ASX_API ASX_MUST_USE asx_status asx_epoch_advance(asx_epoch_handle handle);

/* Signal arrival at the epoch barrier (BARRIER/THRESHOLD mode).
 * entity_id identifies the arriving task/scope.
 * Returns ASX_OK if arrival recorded.
 * Returns ASX_E_INVALID_STATE if epoch closed or advance mode mismatch. */
ASX_API ASX_MUST_USE asx_status asx_epoch_arrive(asx_epoch_handle handle, uint64_t entity_id);

/* Get the number of arrivals in the current phase. */
ASX_API uint32_t asx_epoch_arrival_count(asx_epoch_handle handle);

/* -------------------------------------------------------------------
 * API: Observers
 * ------------------------------------------------------------------- */

/* Register an observer to be notified on phase transitions.
 * Returns ASX_E_RESOURCE_EXHAUSTED if observer slots full. */
ASX_API ASX_MUST_USE asx_status asx_epoch_observe(asx_epoch_handle handle,
                                                   asx_epoch_observer_fn fn, void *user_data);

/* -------------------------------------------------------------------
 * API: Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_epoch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_EPOCH_H */
