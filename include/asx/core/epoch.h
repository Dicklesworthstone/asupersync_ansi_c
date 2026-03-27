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
#include <asx/core/combinator.h>
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

/* Create an epoch with the given advancement policy. */
ASX_API ASX_MUST_USE asx_status asx_epoch_create(const asx_epoch_policy *policy,
                                                  asx_epoch_handle *out);

/* Close an epoch, preventing further advances. */
ASX_API asx_status asx_epoch_close(asx_epoch_handle handle);

/* Get the current state of an epoch. */
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
 * API: Epoch queries
 * ------------------------------------------------------------------- */

/* Get the elapsed time since epoch creation (ns). */
ASX_API uint64_t asx_epoch_duration_ns(asx_epoch_handle handle);

/* Check if the epoch's operation budget is exhausted. */
ASX_API int asx_epoch_is_budget_exhausted(asx_epoch_handle handle);

/* Record an operation against the epoch's budget. */
ASX_API ASX_MUST_USE asx_status asx_epoch_record_operation(asx_epoch_handle handle);

/* Get the number of operations used. */
ASX_API uint32_t asx_epoch_operations_used(asx_epoch_handle handle);

/* Get remaining operations in budget (0 if exhausted or unlimited). */
ASX_API uint32_t asx_epoch_remaining_operations(asx_epoch_handle handle);

/* Check if the epoch has expired (timed out). */
ASX_API int asx_epoch_is_expired(asx_epoch_handle handle);

/* Get the epoch's policy. */
ASX_API asx_status asx_epoch_get_policy(asx_epoch_handle handle, asx_epoch_policy *out);

/* -------------------------------------------------------------------
 * API: Epoch policy builders (fluent configuration)
 * ------------------------------------------------------------------- */

/* Create a strict epoch policy (barrier, no tolerance for missed arrivals). */
ASX_API asx_epoch_policy asx_epoch_policy_strict(uint32_t max_phases);

/* Create a lenient epoch policy (threshold-based, tolerant). */
ASX_API asx_epoch_policy asx_epoch_policy_lenient(uint32_t threshold, uint32_t max_phases);

/* Create a single-phase epoch (one advance then done). */
ASX_API asx_epoch_policy asx_epoch_policy_single(void);

/* Create an infinite epoch (no phase limit). */
ASX_API asx_epoch_policy asx_epoch_policy_infinite(void);

/* -------------------------------------------------------------------
 * API: Epoch-scoped combinators
 *
 * Execute combinator operations within an epoch phase. The operation
 * runs and the epoch advances only after the combinator completes.
 * ------------------------------------------------------------------- */

/* Run a join within an epoch: poll all branches, advance epoch on completion. */
ASX_API ASX_MUST_USE asx_status asx_epoch_join(asx_epoch_handle epoch,
                                                 asx_combinator_poll_fn *poll_fns,
                                                 void **user_datas, uint32_t count,
                                                 asx_task_id self);

/* Run a race within an epoch: first branch wins, losers drained, epoch advances. */
ASX_API ASX_MUST_USE asx_status asx_epoch_race(asx_epoch_handle epoch,
                                                 asx_combinator_poll_fn *poll_fns,
                                                 void **user_datas, uint32_t count,
                                                 asx_task_id self, int32_t *winner);

/* Run a select within an epoch: round-robin fairness race, epoch advances. */
ASX_API ASX_MUST_USE asx_status asx_epoch_select(asx_epoch_handle epoch,
                                                   asx_combinator_poll_fn *poll_fns,
                                                   void **user_datas, uint32_t count,
                                                   asx_task_id self, int32_t *winner);

/* Call a service function through a bulkhead within an epoch. */
ASX_API ASX_MUST_USE asx_status asx_bulkhead_call_in_epoch(asx_epoch_handle epoch,
                                                             asx_combinator_poll_fn poll_fn,
                                                             void *user_data, asx_task_id self);

/* Call a service function through a circuit breaker within an epoch. */
ASX_API ASX_MUST_USE asx_status asx_circuit_breaker_call_in_epoch(asx_epoch_handle epoch,
                                                                    asx_combinator_poll_fn poll_fn,
                                                                    void *user_data,
                                                                    asx_task_id self);

/* -------------------------------------------------------------------
 * API: Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all epoch arena state. For tests only. */
ASX_API void asx_epoch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_EPOCH_H */
