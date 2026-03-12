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
#define ASX_REGION_CAPTURE_ARENA_BYTES 16384u

/* -------------------------------------------------------------------
 * Task poll function signature
 *
 * A task's poll function is called by the scheduler. It returns:
 *   ASX_OK         — task completed successfully
 *   ASX_E_PENDING  — task needs more polls (not yet ready)
 *   any error      — task failed
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_task_poll_fn)(void *user_data, asx_task_id self);

/* Optional destructor for region-owned captured task state. */
typedef void (*asx_task_state_dtor_fn)(void *state, uint32_t state_size);

/* Coroutine resume token embedded in captured task state structs. */
typedef struct asx_co_state {
    uint32_t line;
} asx_co_state;

#define ASX_CO_STATE_INIT { 0u }

/* Protothread helpers:
 *  - ASX_CO_BEGIN must be paired with ASX_CO_END in the same function.
 *  - ASX_CO_YIELD returns ASX_E_PENDING and resumes at the next call.
 *  - State must live in region-owned captured task memory. */
#define ASX_CO_BEGIN(CO_STATE_PTR)                 \
    switch ((CO_STATE_PTR)->line) {                \
    case 0u:

#define ASX_CO_YIELD(CO_STATE_PTR)                 \
    do {                                           \
        (CO_STATE_PTR)->line = (uint32_t)__LINE__; \
        return ASX_E_PENDING;                      \
        case __LINE__:;                            \
    } while (0)

#define ASX_CO_END(CO_STATE_PTR)                   \
    }                                              \
    (CO_STATE_PTR)->line = 0u;                     \
    return ASX_OK

/* -------------------------------------------------------------------
 * Region lifecycle
 * ------------------------------------------------------------------- */

/* Open a new region.
 *
 * Preconditions: out_id must not be NULL.
 * Postconditions: on success, *out_id holds a valid region handle in OPEN state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_id is NULL,
 *   ASX_E_RESOURCE_EXHAUSTED if the region arena is full.
 * Ownership: caller owns the returned handle; must close via asx_region_close
 *   or asx_region_drain.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Region Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_region_open(asx_region_id *out_id);

/* Initiate region close. Transitions: Open → Closing → Closed.
 *
 * Preconditions: id must be a valid region handle for an OPEN region.
 * Postconditions: region transitions toward CLOSED; tasks are drained.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_STALE_HANDLE if generation mismatch, ASX_E_REGION_POISONED
 *   if the region is poisoned, ASX_E_INVALID_TRANSITION if already closed.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Region Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_region_close(asx_region_id id);

/* Query the current state of a region.
 *
 * Preconditions: out_state must not be NULL; id must be a valid handle.
 * Postconditions: on success, *out_state holds the current region state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_state is NULL,
 *   ASX_E_NOT_FOUND if id is invalid or wrong type tag,
 *   ASX_E_STALE_HANDLE if generation mismatch.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Region Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_region_get_state(asx_region_id id,
                                                     asx_region_state *out_state);

/* -------------------------------------------------------------------
 * Region poison / containment
 *
 * When a region is poisoned, all mutating operations (spawn, close,
 * schedule) return ASX_E_REGION_POISONED. Read-only queries (get_state,
 * is_poisoned) remain available for diagnostics. Poisoning is
 * irreversible within a region lifetime — the region must be drained
 * or abandoned. This provides deterministic containment after
 * invariant violations without undefined behavior.
 * ------------------------------------------------------------------- */

/* Poison a region, preventing further mutating operations.
 *
 * Preconditions: id must be a valid region handle.
 * Postconditions: region is permanently poisoned; mutating APIs return
 *   ASX_E_REGION_POISONED; queries (get_state, is_poisoned) still work.
 * Returns ASX_OK on success or if already poisoned (idempotent),
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Region Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_region_poison(asx_region_id id);

/* Query whether a region is poisoned. Sets *out to 1 if poisoned, 0 if not.
 *
 * Preconditions: out must not be NULL; id must be a valid handle.
 * Postconditions: *out is set to 1 (poisoned) or 0 (not poisoned).
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_region_is_poisoned(asx_region_id id,
                                                        int *out);

/* Apply the active containment policy to a region after a fault.
 *
 * Behavior depends on the active safety profile:
 *   FAIL_FAST: returns the fault status (caller should abort).
 *   POISON_REGION: poisons the region and returns the fault.
 *   ERROR_ONLY: returns the fault status without side effects.
 *
 * Preconditions: id must be a valid region handle; fault should be an error.
 * Postconditions: policy-dependent; region may be poisoned.
 * Returns the fault status (always non-OK).
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Containment After Misuse. */
ASX_API ASX_MUST_USE asx_status asx_region_contain_fault(
    asx_region_id id, asx_status fault);

/* -------------------------------------------------------------------
 * Task lifecycle
 * ------------------------------------------------------------------- */

/* Spawn a task within a region. The poll_fn will be called by the
 * scheduler until it returns ASX_OK or an error.
 *
 * Preconditions: region must be OPEN and not poisoned; poll_fn must
 *   not be NULL; out_id must not be NULL.
 * Postconditions: on success, *out_id holds a valid task handle in
 *   PENDING state; task is queued for scheduling.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if poll_fn or
 *   out_id is NULL, ASX_E_NOT_FOUND if region is invalid,
 *   ASX_E_STALE_HANDLE if generation mismatch,
 *   ASX_E_REGION_NOT_OPEN if region is closed,
 *   ASX_E_REGION_POISONED if region is poisoned,
 *   ASX_E_RESOURCE_EXHAUSTED if the task arena is full.
 * Ownership: user_data is borrowed (caller retains ownership).
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_spawn(asx_region_id region,
                                               asx_task_poll_fn poll_fn,
                                               void *user_data,
                                               asx_task_id *out_id);

/* Spawn a task with captured state allocated from the region arena.
 * The returned state pointer is stable for the task lifetime and is
 * automatically passed as user_data to poll_fn.
 *
 * Preconditions: region must be OPEN and not poisoned; poll_fn and
 *   out_id must not be NULL; state_size must be > 0 if out_state != NULL.
 * Postconditions: on success, *out_id holds a task handle; *out_state
 *   (if non-NULL) points to region-owned memory of state_size bytes.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if poll_fn or
 *   out_id is NULL, ASX_E_NOT_FOUND if region is invalid,
 *   ASX_E_REGION_NOT_OPEN if region is closed,
 *   ASX_E_REGION_POISONED if poisoned,
 *   ASX_E_RESOURCE_EXHAUSTED if task or capture arena is full.
 * Ownership: state memory is region-owned; freed on region drain.
 *   state_dtor (if non-NULL) is called before deallocation.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_spawn_captured(asx_region_id region,
                                                        asx_task_poll_fn poll_fn,
                                                        uint32_t state_size,
                                                        asx_task_state_dtor_fn state_dtor,
                                                        asx_task_id *out_id,
                                                        void **out_state);

/* Query the current state of a task.
 *
 * Preconditions: out_state must not be NULL; id must be a valid handle.
 * Postconditions: on success, *out_state holds the current task state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_state is NULL,
 *   ASX_E_NOT_FOUND if id is invalid or wrong type tag.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_get_state(asx_task_id id,
                                                   asx_task_state *out_state);

/* Query the outcome of a completed task.
 *
 * Preconditions: out_outcome must not be NULL; task must be COMPLETED.
 * Postconditions: on success, *out_outcome holds the task's final outcome.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_outcome is NULL,
 *   ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_TASK_NOT_COMPLETED if the task has not finished.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_get_outcome(asx_task_id id,
                                                     asx_outcome *out_outcome);

/* -------------------------------------------------------------------
 * Cancellation (bd-2cw.3)
 *
 * Tasks can be cancelled through explicit request or propagation
 * from parent regions. Cancellation follows a strict phase protocol:
 *   Running → CancelRequested → Cancelling → Finalizing → Completed
 *
 * The checkpoint API allows tasks to observe and acknowledge
 * cancellation, applying cleanup budgets for bounded completion.
 * ------------------------------------------------------------------- */

/* Result from asx_checkpoint(): tells the task its cancel status. */
typedef struct {
    asx_cancel_phase  phase;           /* current phase (or 0 if not cancelled) */
    int               cancelled;       /* nonzero if cancel is active */
    uint32_t          polls_remaining; /* cleanup budget left */
    asx_cancel_kind   kind;            /* cancel kind (if cancelled) */
} asx_checkpoint_result;

/* Request cancellation of a task. Transitions Running → CancelRequested.
 * No-op if already in cancel or terminal state.
 *
 * Preconditions: id must be a valid task handle.
 * Postconditions: task enters CancelRequested phase (if Running).
 * Returns ASX_OK on success or if already cancelling/completed,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_cancel(asx_task_id id,
                                                 asx_cancel_kind kind);

/* Cancel with explicit origin attribution (for propagation tracing).
 *
 * Preconditions: id must be a valid task handle.
 * Postconditions: same as asx_task_cancel; origin recorded for tracing.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_cancel_with_origin(
    asx_task_id id,
    asx_cancel_kind kind,
    asx_region_id origin_region,
    asx_task_id origin_task);

/* Propagate cancellation to all tasks in a region.
 *
 * Preconditions: region must be a valid region handle.
 * Postconditions: all running tasks in the region enter CancelRequested.
 * Returns the number of tasks that received the cancel signal.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_cancel_propagate(asx_region_id region,
                                       asx_cancel_kind kind);

/* Task checkpoint: observe cancel status and advance phase.
 * If in CancelRequested, transitions to Cancelling and applies
 * cleanup budget. Returns cancel status in *out.
 *
 * Preconditions: self must be a valid task handle; out must not be NULL.
 * Postconditions: *out contains current cancel phase and budget.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND if self is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_checkpoint(asx_task_id self,
                                                asx_checkpoint_result *out);

/* Advance from Cancelling → Finalizing. Call when cleanup is done.
 *
 * Preconditions: id must be a valid task handle in Cancelling phase.
 * Postconditions: task transitions to Finalizing phase.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_INVALID_STATE if task is not in the Cancelling phase.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_finalize(asx_task_id id);

/* Query the cancel phase of a task.
 *
 * Preconditions: out must not be NULL; id must be a valid task handle.
 * Postconditions: on success, *out holds the current cancel phase.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_get_cancel_phase(asx_task_id id,
                                                           asx_cancel_phase *out);

/* -------------------------------------------------------------------
 * Obligation lifecycle
 * ------------------------------------------------------------------- */

/* Reserve an obligation within a region. The obligation starts in
 * the RESERVED state and must eventually be committed or aborted.
 *
 * Preconditions: region must be OPEN and not poisoned; out_id must
 *   not be NULL.
 * Postconditions: on success, *out_id holds a valid obligation handle
 *   in RESERVED state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_id is NULL,
 *   ASX_E_NOT_FOUND if region is invalid, ASX_E_STALE_HANDLE if
 *   generation mismatch, ASX_E_REGION_POISONED if poisoned,
 *   ASX_E_RESOURCE_EXHAUSTED if the obligation arena is full.
 * Ownership: caller owns the obligation; must commit or abort.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Obligation Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_obligation_reserve(asx_region_id region,
                                                        asx_obligation_id *out_id);

/* Commit a reserved obligation. Transitions: Reserved → Committed.
 *
 * Preconditions: id must be a valid obligation handle in RESERVED state.
 * Postconditions: obligation transitions to COMMITTED.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_INVALID_TRANSITION if not in RESERVED state (e.g., double commit).
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Obligation Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_obligation_commit(asx_obligation_id id);

/* Abort a reserved obligation. Transitions: Reserved → Aborted.
 *
 * Preconditions: id must be a valid obligation handle in RESERVED state.
 * Postconditions: obligation transitions to ABORTED.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_INVALID_TRANSITION if not in RESERVED state (e.g., after commit).
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Obligation Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_obligation_abort(asx_obligation_id id);

/* Query the current state of an obligation.
 *
 * Preconditions: out_state must not be NULL; id must be a valid handle.
 * Postconditions: on success, *out_state holds the obligation state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_state is NULL,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_obligation_get_state(asx_obligation_id id,
                                                          asx_obligation_state *out_state);

/* -------------------------------------------------------------------
 * Scheduler
 * ------------------------------------------------------------------- */

/* Run the scheduler loop until all tasks in the region complete or
 * the budget is exhausted.
 *
 * Preconditions: region must be a valid handle; budget must not be NULL
 *   and must have remaining polls > 0.
 * Postconditions: tasks are polled in arena-index order; event log is
 *   populated; budget is decremented.
 * Returns ASX_OK when all tasks complete (quiescent),
 *   ASX_E_BUDGET_EXHAUSTED if polls ran out before completion,
 *   ASX_E_NOT_FOUND if region is invalid,
 *   ASX_E_STALE_HANDLE if generation mismatch,
 *   ASX_E_INVALID_ARGUMENT if budget is NULL.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Scheduler. */
ASX_API ASX_MUST_USE asx_status asx_scheduler_run(asx_region_id region,
                                                  asx_budget *budget);

/* -------------------------------------------------------------------
 * Scheduler event sequencing (deterministic replay support)
 *
 * The scheduler emits a monotonically increasing sequence number for
 * each event (task poll, task completion, budget exhaustion). These
 * are deterministic for identical input and seed — suitable for
 * replay identity verification.
 *
 * Tie-break rule: tasks are polled in arena index order within a
 * round. Index order is stable and deterministic.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_SCHED_EVENT_POLL          = 0,  /* task polled */
    ASX_SCHED_EVENT_COMPLETE      = 1,  /* task completed (OK or error) */
    ASX_SCHED_EVENT_BUDGET        = 2,  /* budget exhausted */
    ASX_SCHED_EVENT_QUIESCENT     = 3,  /* all tasks complete */
    ASX_SCHED_EVENT_CANCEL_FORCED = 4   /* task force-completed: cleanup budget exhausted */
} asx_scheduler_event_kind;

typedef struct {
    asx_scheduler_event_kind kind;
    asx_task_id              task_id;   /* ASX_INVALID_ID for non-task events */
    uint32_t                 sequence;  /* monotonic per scheduler_run call */
    uint32_t                 round;     /* scheduler round (0-based) */
} asx_scheduler_event;

/* Read the total event count from the last scheduler_run call.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_scheduler_event_count(void);

/* Read event at index (0-based). Returns 1 on success, 0 on out-of-bounds.
 *
 * Preconditions: out must not be NULL; index < asx_scheduler_event_count().
 * Postconditions: on success (returns 1), *out holds the event.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Scheduler. */
ASX_API int asx_scheduler_event_get(uint32_t index, asx_scheduler_event *out);

/* Reset event log (called automatically by asx_scheduler_run). */
ASX_API void asx_scheduler_event_reset(void);

/* -------------------------------------------------------------------
 * Quiescence
 * ------------------------------------------------------------------- */

/* Check if a region has reached quiescence (all tasks completed,
 * all obligations resolved, region is CLOSED).
 *
 * Preconditions: id must be a valid region handle.
 * Returns ASX_OK if quiescent, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_QUIESCENCE_TASKS_LIVE if tasks remain,
 *   ASX_E_QUIESCENCE_NOT_REACHED if region is not closed.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_quiescence_check(asx_region_id id);

/* Drain a region: run scheduler then close through to CLOSED.
 * This is the high-level "shut down cleanly" operation.
 *
 * Preconditions: id must be a valid region handle; budget must not be NULL.
 * Postconditions: on success, region reaches CLOSED state; all tasks
 *   completed; cleanup destructors called in LIFO order.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_INVALID_ARGUMENT if budget is NULL,
 *   ASX_E_BUDGET_EXHAUSTED if not all tasks completed within budget.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Region Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_region_drain(asx_region_id id,
                                                 asx_budget *budget);

/* Reset all runtime state (test support only).
 * Clears lifecycle arenas plus global scheduler/parallel/channel/timer/
 * trace/telemetry/adapter/fault-injection diagnostic state.
 * Not for production use. */
ASX_API void asx_runtime_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_H */
