/*
 * asx/cx/scope.h — structured concurrency scope and task handles
 *
 * A Scope is a lightweight capability token for spawning and joining
 * tasks within a region. It provides:
 *   - Scoped task spawning with automatic region association
 *   - Task handles for observing completion and outcomes
 *   - Join semantics with outcome aggregation
 *   - Scope exit guarantee: no orphan tasks
 *
 * Scope does NOT use dynamic allocation. Task handles are value types.
 *
 * Usage:
 *   asx_scope scope;
 *   asx_scope_init(&scope, region, cx, budget);
 *   asx_task_handle h;
 *   asx_scope_spawn(&scope, my_poll_fn, user_data, &h);
 *   asx_scope_run(&scope);  // drive all tasks to completion
 *   asx_outcome out;
 *   asx_task_handle_outcome(&h, &out);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CX_SCOPE_H
#define ASX_CX_SCOPE_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/budget.h>
#include <asx/core/outcome.h>
#include <asx/cx/cx.h>
#include <asx/runtime/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Join error classification                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_JOIN_OK = 0,                   /* task completed normally */
    ASX_JOIN_CANCELLED = 1,            /* task was cancelled */
    ASX_JOIN_PANICKED = 2,             /* task panicked (error outcome) */
    ASX_JOIN_POLLED_AFTER_COMPLETE = 3 /* handle polled after terminal */
} asx_join_error;

/* Return string for join error. Never returns NULL. */
ASX_API const char *asx_join_error_str(asx_join_error err);

/* ------------------------------------------------------------------ */
/* Task handle                                                         */
/*                                                                     */
/* A task handle is a value type that holds a reference to a spawned   */
/* task. It allows checking completion status and retrieving outcomes. */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_task_id task_id;     /* handle to the spawned task */
    asx_region_id region_id; /* owning region */
    int joined;              /* 1 if outcome already consumed */
} asx_task_handle;

/* Check if the task referenced by this handle has finished. */
ASX_API int asx_task_handle_is_finished(const asx_task_handle *h);

/* Get the task ID from a handle. */
ASX_API asx_task_id asx_task_handle_task_id(const asx_task_handle *h);

/* Try to retrieve the outcome of a completed task.
 * Returns ASX_OK and fills *out if the task is complete.
 * Returns ASX_E_PENDING if the task is still running.
 * Returns ASX_E_INVALID_ARGUMENT if h or out is NULL.
 * Marks the handle as joined on success. */
ASX_API ASX_MUST_USE asx_status asx_task_handle_try_join(asx_task_handle *h, asx_outcome *out);

/* Classify a join result into a join error category.
 * Returns ASX_JOIN_OK for successful outcomes. */
ASX_API asx_join_error asx_task_handle_join_error(const asx_outcome *outcome);

/* Request cancellation of the task referenced by this handle. */
ASX_API asx_status asx_task_handle_abort(asx_task_handle *h);

/* Request cancellation with explicit cancel kind. */
ASX_API asx_status asx_task_handle_abort_with_kind(asx_task_handle *h, asx_cancel_kind kind);

/* ------------------------------------------------------------------ */
/* Scope                                                               */
/*                                                                     */
/* A scope is a lightweight capability token that binds a region, a    */
/* capability context, and a budget together for structured task       */
/* spawning.                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_region_id region_id; /* owning region */
    asx_cx cx;               /* capability context for spawned tasks */
    asx_budget budget;       /* execution budget for this scope */
    uint32_t spawned;        /* count of tasks spawned in this scope */
} asx_scope;

/* Initialize a scope.
 *
 * Preconditions: scope, cx must not be NULL; region must be valid.
 * The cx must have ASX_CAP_SPAWN.
 * Postconditions: scope is ready for spawning tasks.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_scope_init(asx_scope *scope, asx_region_id region,
                                               const asx_cx *cx, asx_budget budget);

/* Spawn a task within this scope.
 *
 * Preconditions: scope must be initialized; poll_fn, out_handle must not be NULL.
 * Postconditions: task is spawned in the scope's region; *out_handle
 *   holds a handle for observing completion.
 * Returns ASX_OK on success,
 *   ASX_E_INVALID_STATE if the scope value is uninitialized or corrupted,
 *   ASX_E_PERMISSION_DENIED if cx lacks ASX_CAP_SPAWN,
 *   ASX_E_RESOURCE_EXHAUSTED if arena is full. */
ASX_API ASX_MUST_USE asx_status asx_scope_spawn(asx_scope *scope, asx_task_poll_fn poll_fn,
                                                void *user_data, asx_task_handle *out_handle);

/* Spawn a task and derive an explicit child Cx bound to the spawned task id.
 *
 * child_caps must be a subset of the scope's authority. Returns
 * ASX_E_INVALID_STATE if the scope value is uninitialized or corrupted. On success,
 * *out_cx is a real task-bound capability token for the spawned task. */
ASX_API ASX_MUST_USE asx_status asx_scope_spawn_with_cx(asx_scope *scope, asx_cap_flags child_caps,
                                                        asx_task_poll_fn poll_fn, void *user_data,
                                                        asx_task_handle *out_handle,
                                                        asx_cx *out_cx);

/* Spawn a task with captured state (region-arena allocated).
 *
 * Same as asx_scope_spawn but allocates state_size bytes from the
 * region's capture arena. The returned *out_state pointer is stable
 * for the task's lifetime and automatically passed as user_data.
 * Returns ASX_E_INVALID_STATE if the scope value is uninitialized or corrupted. */
ASX_API ASX_MUST_USE asx_status asx_scope_spawn_captured(asx_scope *scope, asx_task_poll_fn poll_fn,
                                                         uint32_t state_size,
                                                         asx_task_state_dtor_fn state_dtor,
                                                         asx_task_handle *out_handle,
                                                         void **out_state);

/* Run the scope's region to completion (or budget exhaustion).
 *
 * Drives the scheduler for the scope's region using the scope's budget.
 * Returns ASX_OK when all tasks reach terminal state,
 *   ASX_E_INVALID_STATE if the scope value is uninitialized or corrupted,
 *   ASX_E_POLL_BUDGET_EXHAUSTED if budget runs out before completion. */
ASX_API ASX_MUST_USE asx_status asx_scope_run(asx_scope *scope);

/* Drain the scope: cancel all tasks, run to quiescence, close region.
 *
 * This is the structured concurrency exit guarantee: after drain,
 * no tasks are alive in the region.
 * Returns ASX_OK on success or ASX_E_INVALID_STATE if the scope value is
 * uninitialized or corrupted. */
ASX_API ASX_MUST_USE asx_status asx_scope_drain(asx_scope *scope);

/* Get the number of tasks spawned in this scope. */
ASX_API uint32_t asx_scope_spawned_count(const asx_scope *scope);

/* Get the region ID of this scope. */
ASX_API asx_region_id asx_scope_region(const asx_scope *scope);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CX_SCOPE_H */
