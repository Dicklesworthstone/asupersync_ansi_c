/*
 * runtime/runtime.h — minimal walking-skeleton runtime API (bd-ix8.8)
 *
 * This header provides the initial public API for region/task lifecycle,
 * scheduling, and quiescence. The implementation is intentionally minimal
 * and explicitly non-final — it exists to prove layer wiring and provide
 * a baseline safety net for Phase 3 expansion.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_H
#define ASX_RUNTIME_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/core/outcome.h>
#include <asx/core/budget.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena capacity (walking skeleton: fixed-size static arenas)
 * Phase 3 will replace with dynamic hook-backed allocation.
 * ------------------------------------------------------------------- */

#define ASX_MAX_REGIONS      8
#define ASX_MAX_TASKS        64
#define ASX_MAX_OBLIGATIONS  128

/* -------------------------------------------------------------------
 * Task poll function signature
 *
 * A task's poll function is called by the scheduler. It returns:
 *   ASX_OK         — task completed successfully
 *   ASX_E_PENDING  — task needs more polls (not yet ready)
 *   any error      — task failed
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_task_poll_fn)(void *user_data, asx_task_id self);

/* -------------------------------------------------------------------
 * Region lifecycle
 * ------------------------------------------------------------------- */

/* Open a new region. Returns ASX_OK and sets *out_id on success. */
ASX_API ASX_MUST_USE asx_status asx_region_open(asx_region_id *out_id);

/* Initiate region close. Transitions: Open → Closing. */
ASX_API ASX_MUST_USE asx_status asx_region_close(asx_region_id id);

/* Query the current state of a region. */
ASX_API ASX_MUST_USE asx_status asx_region_get_state(asx_region_id id,
                                                     asx_region_state *out_state);

/* -------------------------------------------------------------------
 * Task lifecycle
 * ------------------------------------------------------------------- */

/* Spawn a task within a region. The poll_fn will be called by the
 * scheduler until it returns ASX_OK or an error. */
ASX_API ASX_MUST_USE asx_status asx_task_spawn(asx_region_id region,
                                               asx_task_poll_fn poll_fn,
                                               void *user_data,
                                               asx_task_id *out_id);

/* Query the current state of a task. */
ASX_API ASX_MUST_USE asx_status asx_task_get_state(asx_task_id id,
                                                   asx_task_state *out_state);

/* Query the outcome of a completed task. */
ASX_API ASX_MUST_USE asx_status asx_task_get_outcome(asx_task_id id,
                                                     asx_outcome *out_outcome);

/* -------------------------------------------------------------------
 * Obligation lifecycle
 * ------------------------------------------------------------------- */

/* Reserve an obligation within a region. The obligation starts in
 * the RESERVED state and must eventually be committed or aborted. */
ASX_API ASX_MUST_USE asx_status asx_obligation_reserve(asx_region_id region,
                                                        asx_obligation_id *out_id);

/* Commit a reserved obligation. Transitions: Reserved → Committed. */
ASX_API ASX_MUST_USE asx_status asx_obligation_commit(asx_obligation_id id);

/* Abort a reserved obligation. Transitions: Reserved → Aborted. */
ASX_API ASX_MUST_USE asx_status asx_obligation_abort(asx_obligation_id id);

/* Query the current state of an obligation. */
ASX_API ASX_MUST_USE asx_status asx_obligation_get_state(asx_obligation_id id,
                                                          asx_obligation_state *out_state);

/* -------------------------------------------------------------------
 * Scheduler
 * ------------------------------------------------------------------- */

/* Run the scheduler loop until all tasks in the region complete or
 * the budget is exhausted. Returns ASX_OK when quiescent. */
ASX_API ASX_MUST_USE asx_status asx_scheduler_run(asx_region_id region,
                                                  asx_budget *budget);

/* -------------------------------------------------------------------
 * Quiescence
 * ------------------------------------------------------------------- */

/* Check if a region has reached quiescence (all tasks completed,
 * region is CLOSED). Returns ASX_OK if quiescent. */
ASX_API ASX_MUST_USE asx_status asx_quiescence_check(asx_region_id id);

/* Drain a region: run scheduler then close through to CLOSED.
 * This is the high-level "shut down cleanly" operation. */
ASX_API ASX_MUST_USE asx_status asx_region_drain(asx_region_id id,
                                                 asx_budget *budget);

/* Reset all runtime state (test support only).
 * Clears all regions and tasks. Not for production use. */
ASX_API void asx_runtime_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_H */
