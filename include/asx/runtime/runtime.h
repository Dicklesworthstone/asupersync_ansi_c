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
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/outcome.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena capacity (fixed-size static arenas).
 *
 * Capacities are compile-time resource-plane knobs: override them with
 * -DASX_MAX_TASKS=4096 (etc.) to size the arenas for a deployment.
 * They bound how many records may be *resident at once*, not how many
 * may be created over the runtime's lifetime: slots are reclaimed with a
 * generation bump (see "Slot reclamation" below), so stale handles fail
 * closed with ASX_E_STALE_HANDLE instead of aliasing new records.
 *
 * Slot indices are 16-bit inside handles, so each arena is capped at
 * 65535 slots.
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_REGIONS
#define ASX_MAX_REGIONS 8
#endif
#ifndef ASX_MAX_TASKS
#define ASX_MAX_TASKS 64
#endif
#ifndef ASX_MAX_OBLIGATIONS
#define ASX_MAX_OBLIGATIONS 128
#endif
#ifndef ASX_REGION_CAPTURE_ARENA_BYTES
#define ASX_REGION_CAPTURE_ARENA_BYTES 16384u
#endif

#if (ASX_MAX_REGIONS) < 1 || (ASX_MAX_REGIONS) > 65535
#error "ASX_MAX_REGIONS must be in [1, 65535]"
#endif
#if (ASX_MAX_TASKS) < 1 || (ASX_MAX_TASKS) > 65535
#error "ASX_MAX_TASKS must be in [1, 65535]"
#endif
#if (ASX_MAX_OBLIGATIONS) < 1 || (ASX_MAX_OBLIGATIONS) > 65535
#error "ASX_MAX_OBLIGATIONS must be in [1, 65535]"
#endif

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

#define ASX_CO_STATE_INIT {0u}

/* Protothread helpers:
 *  - ASX_CO_BEGIN must be paired with ASX_CO_END in the same function.
 *  - ASX_CO_YIELD returns ASX_E_PENDING and resumes at the next call.
 *  - State must live in region-owned captured task memory. */
#define ASX_CO_BEGIN(CO_STATE_PTR)                                                                 \
    switch ((CO_STATE_PTR)->line) {                                                                \
    case 0u:

#define ASX_CO_YIELD(CO_STATE_PTR)                                                                 \
    do {                                                                                           \
        (CO_STATE_PTR)->line = (uint32_t)__LINE__;                                                 \
        return ASX_E_PENDING;                                                                      \
    case __LINE__:;                                                                                \
    } while (0)

#define ASX_CO_END(CO_STATE_PTR)                                                                   \
    }                                                                                              \
    (CO_STATE_PTR)->line = 0u;                                                                     \
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

/* Open a child region under an existing OPEN parent region.
 *
 * Preconditions: parent must be a valid OPEN, unpoisoned region handle;
 *   out_child must not be NULL.
 * Postconditions: on success, *out_child holds a valid OPEN child region.
 *   The child's parent_id is set to parent and the child is appended to the
 *   parent's bounded children[] list.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_child is NULL,
 *   ASX_E_NOT_FOUND if parent is invalid, ASX_E_STALE_HANDLE if generation
 *   mismatch, ASX_E_REGION_CLOSED if parent is not OPEN (closing or closed),
 *   ASX_E_REGION_POISONED if parent is poisoned, or
 *   ASX_E_RESOURCE_EXHAUSTED if the parent's child list or region arena is full.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_region_open_child(asx_region_id parent,
                                                      asx_region_id *out_child);

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
ASX_API ASX_MUST_USE asx_status asx_region_get_state(asx_region_id id, asx_region_state *out_state);

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
ASX_API ASX_MUST_USE asx_status asx_region_is_poisoned(asx_region_id id, int *out);

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
ASX_API ASX_MUST_USE asx_status asx_region_contain_fault(asx_region_id id, asx_status fault);

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
 *   ASX_E_REGION_CLOSED if region is closing or closed,
 *   ASX_E_REGION_POISONED if region is poisoned,
 *   ASX_E_RESOURCE_EXHAUSTED if the task arena is full.
 * Ownership: user_data is borrowed (caller retains ownership).
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_spawn(asx_region_id region, asx_task_poll_fn poll_fn,
                                               void *user_data, asx_task_id *out_id);

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
 *   ASX_E_REGION_CLOSED if region is closing or closed,
 *   ASX_E_REGION_POISONED if poisoned,
 *   ASX_E_RESOURCE_EXHAUSTED if task or capture arena is full.
 * Ownership: state memory is region-owned; freed on region drain.
 *   state_dtor (if non-NULL) is called before deallocation.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_spawn_captured(asx_region_id region,
                                                        asx_task_poll_fn poll_fn,
                                                        uint32_t state_size,
                                                        asx_task_state_dtor_fn state_dtor,
                                                        asx_task_id *out_id, void **out_state);

/* -------------------------------------------------------------------
 * Budgets (deadlines, poll quotas, cost quotas)
 *
 * Every region carries a budget, inherited by its tasks and narrowed for
 * child regions with asx_budget_meet(). A task's budget is enforced by the
 * scheduler before each poll:
 *   - deadline reached     -> cancel with ASX_CANCEL_DEADLINE (a parked
 *                             task is woken by a timer at the deadline);
 *   - poll quota used up   -> cancel with ASX_CANCEL_POLL_QUOTA;
 *   - cost quota exceeded  -> asx_task_consume_cost() cancels with
 *                             ASX_CANCEL_COST_BUDGET.
 * Cancelled tasks then run their bounded cleanup as usual. Deadlines use
 * the runtime clock (virtual time in deterministic builds). The default
 * budget is asx_budget_infinite(): nothing is enforced.
 * ------------------------------------------------------------------- */

/* Spawn a task whose budget is meet(region budget, *budget).
 * Same contract and errors as asx_task_spawn; budget may be NULL
 * (region budget only). */
ASX_API ASX_MUST_USE asx_status asx_task_spawn_with_budget(asx_region_id region,
                                                           asx_task_poll_fn poll_fn,
                                                           void *user_data,
                                                           const asx_budget *budget,
                                                           asx_task_id *out_id);

/* Open a child region whose budget is meet(parent budget, *budget); its
 * tasks and descendants inherit it (e.g. a deadline for a whole subtree).
 * Same contract and errors as asx_region_open_child; budget may be NULL. */
ASX_API ASX_MUST_USE asx_status asx_region_open_child_with_budget(asx_region_id parent,
                                                                  const asx_budget *budget,
                                                                  asx_region_id *out_child);

/* Read a region's budget. Returns ASX_OK, ASX_E_INVALID_ARGUMENT if out is
 * NULL, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for invalid handles. */
ASX_API ASX_MUST_USE asx_status asx_region_get_budget(asx_region_id id, asx_budget *out);

/* Read a task's remaining budget. Same errors as asx_region_get_budget. */
ASX_API ASX_MUST_USE asx_status asx_task_get_budget(asx_task_id id, asx_budget *out);

/* Charge `cost` units against the task's cost quota. When the quota cannot
 * cover it, the task is cancelled with ASX_CANCEL_COST_BUDGET and
 * ASX_E_COST_QUOTA_EXHAUSTED is returned (the quota is left untouched).
 * Returns ASX_OK when charged, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for
 * invalid handles. */
ASX_API ASX_MUST_USE asx_status asx_task_consume_cost(asx_task_id self, uint64_t cost);

/* Query the current state of a task.
 *
 * Preconditions: out_state must not be NULL; id must be a valid handle.
 * Postconditions: on success, *out_state holds the current task state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_state is NULL,
 *   ASX_E_NOT_FOUND if id is invalid or wrong type tag.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_get_state(asx_task_id id, asx_task_state *out_state);

/* Query the outcome of a completed task.
 *
 * Preconditions: out_outcome must not be NULL; task must be COMPLETED.
 * Postconditions: on success, *out_outcome holds the task's final outcome.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_outcome is NULL,
 *   ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_TASK_NOT_COMPLETED if the task has not finished.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_get_outcome(asx_task_id id, asx_outcome *out_outcome);

/* -------------------------------------------------------------------
 * Slot reclamation: join / detach
 *
 * A completed task keeps its arena slot (and therefore its outcome)
 * until one of the following releases it:
 *   - asx_task_join() consumes the outcome and frees the slot;
 *   - asx_task_detach() marks the task so its slot is freed the moment
 *     it completes (or immediately if it already has);
 *   - its region reaches CLOSED and the arena later needs the capacity
 *     (allocation-pressure reclamation, lowest slot index first), or the
 *     CLOSED region slot itself is recycled.
 *
 * Reuse order is deterministic for identical spawn/release sequences.
 * After release, every old handle reports ASX_E_STALE_HANDLE or
 * ASX_E_NOT_FOUND; it never observes the slot's next occupant.
 *
 * Long-lived regions that keep spawning work (servers, supervisors)
 * must join or detach their tasks, exactly as a Rust JoinHandle must be
 * awaited or dropped.
 * ------------------------------------------------------------------- */

/* Consume a completed task's outcome and release its slot.
 *
 * Preconditions: id must be a valid task handle.
 * Postconditions: on success, *out_outcome (if non-NULL) holds the final
 *   outcome and the handle is invalidated.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for
 *   invalid handles, ASX_E_TASK_NOT_COMPLETED if the task is still live
 *   (the slot is left untouched).
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_join(asx_task_id id, asx_outcome *out_outcome);

/* Detach a task: its slot is released as soon as it completes.
 * Detaching an already-completed task releases it immediately.
 * Detaching twice is a no-op while the task is live.
 *
 * Returns ASX_OK on success, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for
 *   invalid handles.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_detach(asx_task_id id);

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
    asx_cancel_phase phase;   /* current phase (or 0 if not cancelled) */
    int cancelled;            /* nonzero if cancel is active */
    uint32_t polls_remaining; /* cleanup budget left */
    asx_cancel_kind kind;     /* cancel kind (if cancelled or masked) */
    int masked;               /* cancel pending but deferred by asx_task_mask() */
} asx_checkpoint_result;

/* Maximum nesting of asx_task_mask() sections (INV-MASK-BOUNDED). */
#ifndef ASX_MAX_MASK_DEPTH
#define ASX_MAX_MASK_DEPTH 64u
#endif

/* Request cancellation of a task. Transitions Running → CancelRequested.
 * No-op if already in cancel or terminal state.
 *
 * Preconditions: id must be a valid task handle.
 * Postconditions: task enters CancelRequested phase (if Running).
 * Returns ASX_OK on success or if already cancelling/completed,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_task_cancel(asx_task_id id, asx_cancel_kind kind);

/* Cancel with explicit origin attribution (for propagation tracing).
 *
 * Preconditions: id must be a valid task handle.
 * Postconditions: same as asx_task_cancel; origin recorded for tracing.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_cancel_with_origin(asx_task_id id, asx_cancel_kind kind,
                                                            asx_region_id origin_region,
                                                            asx_task_id origin_task);

/* Cancel with a complete reason: kind, origin region and task, timestamp,
 * message and cause (Rust CancelReason). A task already cancelled is
 * strengthened: the winning reason replaces the current one whole (see
 * asx_cancel_strengthen). The message and cause are borrowed and must
 * outlive the task's record.
 *
 * asx_task_cancel stamps the current time and attributes the cancel to the
 * task's own region; asx_task_cancel_with_origin stamps the current time.
 * A direct task cancel records no ASX_TRACE_CANCEL_REQUEST, as Rust's
 * RuntimeState::cancel_task and task-handle aborts record none
 * (state.rs:3429); region cancellation does (asx_region_cancel).
 *
 * Returns ASX_OK (also for a completed task), ASX_E_INVALID_ARGUMENT if
 * reason is NULL, a lookup error for a bad handle.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_cancel_with_reason(asx_task_id id,
                                                            const asx_cancel_reason *reason);

/* Propagate cancellation to all tasks in a region.
 *
 * Preconditions: region must be a valid region handle.
 * Postconditions: all running tasks in the region enter CancelRequested.
 * Returns the number of tasks that received the cancel signal.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_cancel_propagate(asx_region_id region, asx_cancel_kind kind);

/* Cancel a region (Rust RuntimeState::cancel_request). Every region of
 * the subtree, parent first:
 *   - records ASX_TRACE_REGION_CANCELLED with its reason: `reason` for
 *     `region`; for a descendant, ParentCancelled attributed to its parent,
 *     stamped with reason->timestamp and caused by the parent's reason;
 *   - begins closing (Open -> Closing, ASX_TRACE_REGION_CLOSE), or, if
 *     already closing, strengthens its stored reason (asx_cancel_strengthen).
 * Then every live task of the subtree is cancelled with its region's
 * reason (a newly cancelled task records ASX_TRACE_CANCEL_REQUEST). Regions
 * already without live work finalize at once; the others close as their
 * last task completes. Closing regions admit no new tasks.
 *
 * The message and cause of `reason` are borrowed and must outlive the
 * region. *out_cancelled (if non-NULL) receives the number of tasks
 * reached.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT if reason is NULL, or a lookup
 * error for a bad handle.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_region_cancel(asx_region_id region,
                                                  const asx_cancel_reason *reason,
                                                  uint32_t *out_cancelled);

/* Read a region's (strengthened) cancel reason. Returns ASX_OK,
 * ASX_E_INVALID_ARGUMENT if out is NULL, a lookup error for a bad handle,
 * or ASX_E_NOT_FOUND if the region was never cancelled. */
ASX_API ASX_MUST_USE asx_status asx_region_get_cancel_reason(asx_region_id region,
                                                             asx_cancel_reason *out);

/* Task checkpoint: observe cancel status and advance phase.
 * If in CancelRequested, transitions to Cancelling and applies
 * cleanup budget. Returns cancel status in *out.
 *
 * A checkpoint also observes the task's budget deadline: once it has
 * passed, the task is cancelled with ASX_CANCEL_DEADLINE right here,
 * without waiting for the scheduler's next pre-poll check.
 *
 * While the task is masked (asx_task_mask), a pending cancel is not
 * acknowledged: *out reports cancelled = 0, masked = 1 and the pending
 * kind, and the phase stays CancelRequested.
 *
 * Preconditions: self must be a valid task handle; out must not be NULL.
 * Postconditions: *out contains current cancel phase and budget.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND if self is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Task Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_checkpoint(asx_task_id self, asx_checkpoint_result *out);

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
ASX_API ASX_MUST_USE asx_status asx_task_get_cancel_phase(asx_task_id id, asx_cancel_phase *out);

/* Read the reason a task was cancelled with (the strengthened reason once
 * several cancels arrived). Available while the task's slot is held, so
 * also for a completed, not yet joined task: a cancelled outcome's reason.
 *
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT if out is NULL, a lookup error
 * for a bad handle, or ASX_E_NOT_FOUND if the task was never cancelled.
 * The message and cause pointers are borrowed from the runtime.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_get_cancel_reason(asx_task_id id, asx_cancel_reason *out);

/* Enter a cancel-masked critical section (nestable).
 *
 * While a task's mask depth is nonzero, cancellation is deferred: it is
 * still recorded (and strengthened) but asx_checkpoint() does not
 * acknowledge it, and the scheduler neither consumes cleanup polls nor
 * force-completes the task. Use for short sections that must not be
 * interrupted (committing a transaction, flushing a buffer, finalizers);
 * the mask may span polls. Every mask must be paired with
 * asx_task_unmask().
 *
 * Preconditions: self must be a live, non-completed task handle.
 * Returns ASX_OK, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for a bad handle,
 *   ASX_E_INVALID_STATE for a completed task or when the depth would
 *   exceed ASX_MAX_MASK_DEPTH.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_mask(asx_task_id self);

/* Leave the innermost masked section. When the depth returns to zero a
 * pending cancel becomes observable at the next checkpoint.
 *
 * Returns ASX_OK, a lookup error for a bad handle, or ASX_E_INVALID_STATE
 *   when the task is not masked.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_unmask(asx_task_id self);

/* Query a task's current mask depth.
 *
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT if out_depth is NULL, or a
 *   lookup error for a bad handle.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_mask_depth(asx_task_id id, uint32_t *out_depth);

/* -------------------------------------------------------------------
 * Obligation lifecycle
 *
 * An obligation is a linear resource (send permit, ack, lease, ...) that
 * must be committed or aborted exactly once. Each obligation records its
 * kind and its holder task. When a holder completes with obligations still
 * RESERVED, each is a leak, whether or not the holder was cancelled (as
 * Rust, where a body ending with an unresolved token drops it and the drop
 * posts a Leak), handled by the runtime's leak_response policy: PANIC
 * (marked LEAKED, fault reported through the region's containment policy),
 * LOG (LEAKED + log record), SILENT (LEAKED), or RECOVER (aborted with
 * ASX_OBLIGATION_ABORT_LEAK_RECOVERED). The optional leak_escalation
 * threshold switches policy after N leaks.
 * Obligations without a holder (reserved outside any task) are not
 * auto-resolved: an unresolved one blocks its region's finalization with
 * ASX_E_OBLIGATIONS_UNRESOLVED.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_OBLIGATION_KIND_GENERIC = 0,
    ASX_OBLIGATION_KIND_SEND_PERMIT = 1,
    ASX_OBLIGATION_KIND_ACK = 2,
    ASX_OBLIGATION_KIND_LEASE = 3,
    ASX_OBLIGATION_KIND_IO_OP = 4,
    ASX_OBLIGATION_KIND_SEMAPHORE_PERMIT = 5,
    ASX_OBLIGATION_KIND_TRANSACTION = 6
} asx_obligation_kind;

typedef enum {
    ASX_OBLIGATION_ABORT_NONE = 0,          /* not aborted */
    ASX_OBLIGATION_ABORT_EXPLICIT = 1,      /* asx_obligation_abort() */
    ASX_OBLIGATION_ABORT_CANCEL = 2,        /* aborted because of a cancellation */
    ASX_OBLIGATION_ABORT_ERROR = 3,         /* aborted on an error path */
    ASX_OBLIGATION_ABORT_LEAK_RECOVERED = 4 /* leak resolved by RECOVER policy */
} asx_obligation_abort_reason;

typedef struct {
    asx_obligation_state state;
    asx_obligation_kind kind;
    asx_region_id region;
    asx_task_id holder; /* ASX_INVALID_ID if unowned */
    asx_obligation_abort_reason abort_reason;
} asx_obligation_info;

/* Reserve an obligation with an explicit kind and holder task.
 * `holder` may be ASX_INVALID_ID (unowned) or a live task.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for NULL out_id / bad kind,
 *   ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for a bad region or holder,
 *   ASX_E_INVALID_STATE if the holder already completed, plus the region
 *   errors of asx_obligation_reserve.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_obligation_reserve_ex(asx_region_id region,
                                                          asx_obligation_kind kind,
                                                          asx_task_id holder,
                                                          asx_obligation_id *out_id);

/* Query kind/holder/state/abort reason of an obligation.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT if out is NULL, or
 *   ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for invalid handles. */
ASX_API ASX_MUST_USE asx_status asx_obligation_get_info(asx_obligation_id id,
                                                        asx_obligation_info *out);

/* Cumulative number of obligations recorded as leaked since reset. */
ASX_API uint64_t asx_obligation_leak_count(void);

/* Reserve an obligation within a region. The obligation starts in
 * the RESERVED state and must eventually be committed or aborted.
 * Kind is GENERIC; the holder is the task currently being polled
 * (asx_task_current()), or none when called outside a poll.
 *
 * Preconditions: region must be OPEN and not poisoned; out_id must
 *   not be NULL.
 * Postconditions: on success, *out_id holds a valid obligation handle
 *   in RESERVED state.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out_id is NULL,
 *   ASX_E_NOT_FOUND if region is invalid, ASX_E_STALE_HANDLE if
 *   generation mismatch, ASX_E_REGION_POISONED if poisoned,
 *   ASX_E_REGION_CLOSED if the region is not OPEN,
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

/* Abort a reserved obligation, recording why (Rust ObligationToken::abort
 * with ObligationAbortReason): ASX_OBLIGATION_ABORT_EXPLICIT (what
 * asx_obligation_abort records), _CANCEL or _ERROR.
 * Returns the errors of asx_obligation_abort, or ASX_E_INVALID_ARGUMENT for
 * any other reason (NONE and LEAK_RECOVERED are recorded by the runtime).
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status
asx_obligation_abort_with_reason(asx_obligation_id id, asx_obligation_abort_reason reason);

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
 *
 * The scheduler drives every task in the region *subtree* (the region
 * and all of its descendants), in ascending arena-index order within a
 * round. Scheduling is wake-driven:
 *
 *   - a task that returns ASX_E_PENDING stays runnable and is re-polled
 *     next round (cooperative yield), UNLESS it called asx_task_park()
 *     during that poll — then it is not polled again until something
 *     calls asx_task_wake() on it (a timer, a join target completing, a
 *     waker/IO readiness signal, a cancel request, or user code);
 *   - when every live task in scope is parked, the scheduler goes idle:
 *     it fires due task timers; with the virtual clock (deterministic
 *     builds without a real clock hook) it jumps time straight to the
 *     earliest armed timer; with a real clock it blocks in the reactor
 *     wait hook until the timer deadline or I/O readiness;
 *   - if nothing can ever wake the parked tasks, it returns
 *     ASX_E_WOULD_BLOCK instead of spinning.
 *
 * Idle waiting never consumes poll budget; only actual polls do.
 * ------------------------------------------------------------------- */

/* Run the scheduler loop until all tasks in the region subtree complete,
 * the budget is exhausted, or every live task is parked with no wake
 * source.
 *
 * Preconditions: region must be a valid handle; budget must not be NULL
 *   and must have remaining polls > 0.
 * Postconditions: tasks are polled in arena-index order; event log is
 *   populated; budget is decremented once per poll.
 * Returns ASX_OK when all tasks complete (quiescent),
 *   ASX_E_POLL_BUDGET_EXHAUSTED if polls ran out before completion,
 *   ASX_E_TIMED_OUT if the budget's deadline passed before completion
 *     (the deadline also bounds time spent blocked waiting for I/O,
 *     timers or cross-thread wakes),
 *   ASX_E_WOULD_BLOCK if all live tasks are parked and no timer, waker,
 *   or I/O registration can wake them,
 *   ASX_E_NOT_FOUND if region is invalid,
 *   ASX_E_STALE_HANDLE if generation mismatch,
 *   ASX_E_INVALID_ARGUMENT if budget is NULL.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Scheduler. */
ASX_API ASX_MUST_USE asx_status asx_scheduler_run(asx_region_id region, asx_budget *budget);

/* Run the region subtree until no task is runnable, without moving the
 * clock: timers already due fire, later ones wait. This is the lab
 * runtime's run_until_idle; asx_scheduler_run is run_with_auto_advance.
 * A caller drives virtual time itself (asx_runtime_virtual_advance) between
 * calls, as a deterministic test or replay driver does.
 *
 * Returns ASX_OK when the subtree is quiescent, ASX_E_PENDING when it is
 * idle but a timer or external wake source remains, ASX_E_WOULD_BLOCK when
 * nothing can ever wake the parked tasks, and otherwise the errors of
 * asx_scheduler_run.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_scheduler_run_until_idle(asx_region_id region,
                                                             asx_budget *budget);

/* -------------------------------------------------------------------
 * Wake-driven waiting (park / wake / timers / join)
 * ------------------------------------------------------------------- */

/* Park the calling task: after the current poll returns ASX_E_PENDING it
 * is not polled again until woken. Must be called from inside the task's
 * own poll function, after arranging a wake source (timer, join, waker).
 * A wake that arrives before the poll returns cancels the park.
 *
 * Returns ASX_OK on success, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for
 *   invalid handles, ASX_E_INVALID_STATE if `self` is not currently being
 *   polled by the scheduler (the call is then a harmless no-op).
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_park(asx_task_id self);

/* The task whose poll function the scheduler is currently running, or
 * ASX_INVALID_ID outside a poll. Lets I/O and sync primitives park the
 * caller without threading its handle through every call.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API asx_task_id asx_task_current(void);

/* Make a parked task runnable again. Waking a task that is mid-poll makes
 * its pending park request void; waking a runnable task is a no-op.
 *
 * Returns ASX_OK on success (including for completed tasks),
 *   ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for invalid handles.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_wake(asx_task_id id);

/* Ensure the task is woken no later than `deadline` (nanoseconds on the
 * runtime clock). Each task has one timer that keeps the earliest armed
 * deadline; it disarms when it fires or the task completes. A wake that
 * turns out to be early is harmless: poll functions re-check their own
 * deadline and re-arm.
 *
 * Returns ASX_OK on success, ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for
 *   invalid handles, ASX_E_INVALID_STATE for completed tasks.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_arm_timer(asx_task_id self, asx_time deadline);

/* Wait until the runtime clock reaches `deadline`: returns ASX_OK if it
 * already has; otherwise arms the task timer, parks `self`, and returns
 * ASX_E_PENDING (propagate that from the poll function).
 *
 * Outside a scheduler poll (e.g. invalid `self`) nothing is armed or
 * parked and the call simply reports ASX_OK / ASX_E_PENDING.
 * Returns ASX_E_HOOK_MISSING-class errors only if the clock fails.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_wait_until(asx_task_id self, asx_time deadline);

/* A sleep's timer is traced by the sleep itself, as Rust's Sleep does
 * (sleep.rs:660-690): the scheduler firing a due timer only wakes the
 * task; the sleep then records how it ended.
 *
 * asx_task_cancel_timer: the sleep ended without completing at its
 * deadline (cancelled, dropped): records ASX_TRACE_TIMER_CANCEL if a
 * traced timer is registered, even one that already woke the task, and
 * disarms the task's wake.
 * asx_task_complete_timer: the sleep completed at its deadline: records
 * ASX_TRACE_TIMER_FIRE if a traced timer is registered (a sleep ready at
 * its first poll registered none) and disarms the wake.
 * Both are no-ops when nothing is registered.
 * Return ASX_OK or a lookup error for a bad handle.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_cancel_timer(asx_task_id self);
ASX_API ASX_MUST_USE asx_status asx_task_complete_timer(asx_task_id self);

/* Wake `watcher` when `target` completes, without joining it (a monitor:
 * supervisors, task groups). A task has at most one watcher; a new call
 * replaces the previous one. If `target` has already completed, the
 * watcher is woken immediately. The watcher must still park itself
 * (asx_task_park) to wait.
 *
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT if target == watcher, or a lookup
 *   error (ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE) for either handle.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_watch(asx_task_id target, asx_task_id watcher);

/* Await another task from inside a poll function.
 *
 * If `target` has completed, writes its outcome to *out_outcome (if
 * non-NULL), releases the target's slot (consuming join, like awaiting a
 * Rust JoinHandle), and returns ASX_OK. Otherwise registers `self` as a
 * join waiter, parks it, and returns ASX_E_PENDING; the scheduler wakes
 * `self` when `target` completes.
 *
 * Returns ASX_E_INVALID_ARGUMENT if self == target,
 *   ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE if either handle is invalid
 *   (e.g. the target was already joined or detached).
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_join_poll(asx_task_id self, asx_task_id target,
                                                   asx_outcome *out_outcome);

/* Panic the calling task (Rust: a panic in the task body, caught at the
 * poll boundary). Call from the task's own poll function; when that poll
 * returns, whatever it returns, the task completes with outcome PANICKED
 * and `message` (borrowed: it must outlive the task's record; NULL means
 * "unknown panic"). A panic dominates a pending cancel and is not a
 * containment fault.
 *
 * Returns ASX_OK, a lookup error for a bad handle, or ASX_E_INVALID_STATE
 * if `self` is not being polled.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_panic(asx_task_id self, const char *message);

/* Read a panicked task's message (while its slot is held). Returns ASX_OK,
 * ASX_E_INVALID_ARGUMENT if out is NULL, a lookup error, or
 * ASX_E_NOT_FOUND if the task did not panic. */
ASX_API ASX_MUST_USE asx_status asx_task_get_panic_message(asx_task_id id, const char **out);

/* Report the error status a task's poll function returned when it
 * failed (ASX_OK if it completed successfully or was cancelled).
 *
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for invalid handles,
 *   ASX_E_TASK_NOT_COMPLETED if the task is still live.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_task_get_error(asx_task_id id, asx_status *out_error);

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
    ASX_SCHED_EVENT_POLL = 0,         /* task polled */
    ASX_SCHED_EVENT_COMPLETE = 1,     /* task completed (OK or error) */
    ASX_SCHED_EVENT_BUDGET = 2,       /* budget exhausted */
    ASX_SCHED_EVENT_QUIESCENT = 3,    /* all tasks complete */
    ASX_SCHED_EVENT_CANCEL_FORCED = 4 /* task force-completed: cleanup budget exhausted */
} asx_scheduler_event_kind;

typedef struct {
    asx_scheduler_event_kind kind;
    asx_task_id task_id; /* ASX_INVALID_ID for non-task events */
    uint32_t sequence;   /* monotonic per scheduler_run call */
    uint32_t round;      /* scheduler round (0-based) */
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
 * all obligations resolved, cleanup stack drained, region is CLOSED).
 *
 * Preconditions: id must be a valid region handle.
 * Returns ASX_OK if quiescent, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_QUIESCENCE_TASKS_LIVE if tasks remain,
 *   ASX_E_QUIESCENCE_NOT_REACHED if region is not closed.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_quiescence_check(asx_region_id id);

/* Boolean region-level quiescence helper.
 *
 * Returns nonzero iff the region satisfies the exact four-conjunct
 * quiescence contract exposed by asx_quiescence_check_detailed():
 *   1. all tasks complete,
 *   2. all child regions closed,
 *   3. all obligations resolved,
 *   4. cleanup stack fully drained,
 * and the region state is CLOSED.
 *
 * Returns 0 if id is invalid or any conjunct fails. */
ASX_API int asx_region_is_quiescent(asx_region_id id);

/* Drain a region: run scheduler then close through to CLOSED.
 * This is the high-level "shut down cleanly" operation.
 *
 * Preconditions: id must be a valid region handle; budget must not be NULL.
 * Postconditions: on success, region reaches CLOSED state; all tasks
 *   completed; cleanup destructors called in LIFO order.
 * Returns ASX_OK on success, ASX_E_NOT_FOUND if id is invalid,
 *   ASX_E_INVALID_ARGUMENT if budget is NULL,
 *   ASX_E_BUDGET_EXHAUSTED if not all tasks completed within budget,
 *   ASX_E_PENDING while the region is closing but still waiting for
 *   child regions to close.
 * Thread-safety: not thread-safe; single-threaded mode only.
 * See: API_MISUSE_CATALOG.md § Region Lifecycle. */
ASX_API ASX_MUST_USE asx_status asx_region_drain(asx_region_id id, asx_budget *budget);

/* -------------------------------------------------------------------
 * Drain progress and quiescence evidence (bd-1eqo.5.2)
 *
 * Provides structured observability into drain/close sequences.
 * asx_drain_progress is a point-in-time snapshot; asx_quiescence_report
 * decomposes the quiescence check into its four conjuncts (Q1-Q4).
 * ------------------------------------------------------------------- */

/* Drain progress snapshot: live counters during region drain. */
typedef struct {
    asx_region_state region_state; /* current region state */
    uint32_t tasks_total;          /* total tasks spawned in region */
    uint32_t tasks_live;           /* non-completed tasks */
    uint32_t tasks_cancelled;      /* tasks in cancel phase >= REQUESTED */
    uint32_t tasks_completed;      /* tasks in COMPLETED state */
    uint32_t obligations_total;    /* total obligations in region */
    uint32_t obligations_reserved; /* still-reserved (unresolved) */
    uint32_t obligations_resolved; /* committed + aborted */
    int cleanup_drained;           /* 1 if cleanup stack is empty */
    int poisoned;                  /* 1 if region is poisoned */
} asx_drain_progress;

/* Quiescence conjunct evidence: each field is 1 if the condition holds. */
typedef struct {
    int q1_tasks_complete;         /* Q1: all tasks reached COMPLETED */
    int q2_children_closed;        /* Q2: all child regions closed */
    int q3_obligations_resolved;   /* Q3: no obligations in RESERVED state */
    int q4_cleanup_drained;        /* Q4: cleanup stack fully drained */
    int quiescent;                 /* 1 iff all Q1-Q4 hold and region is CLOSED */
    asx_region_state region_state; /* current region state */
    asx_drain_progress progress;   /* full progress snapshot */
} asx_quiescence_report;

/* Query the drain progress of a region.
 *
 * Preconditions: out must not be NULL; id must be a valid region handle.
 * Postconditions: on success, *out holds a point-in-time progress snapshot.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_region_drain_progress(asx_region_id id,
                                                          asx_drain_progress *out);

/* Detailed quiescence check with decomposed Q1-Q4 evidence.
 *
 * Unlike asx_quiescence_check() which returns a single status code,
 * this function populates a report showing which conjuncts hold and
 * which are blocking quiescence. Always returns ASX_OK (the report
 * itself carries the quiescence verdict).
 *
 * Preconditions: out must not be NULL; id must be a valid region handle.
 * Postconditions: *out contains the decomposed quiescence evidence.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL,
 *   ASX_E_NOT_FOUND if id is invalid.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_quiescence_check_detailed(asx_region_id id,
                                                              asx_quiescence_report *out);

/* Reset all runtime state (test support only).
 * Clears lifecycle arenas plus global scheduler/parallel/channel/timer/
 * trace/telemetry/adapter/fault-injection diagnostic state.
 * Not for production use. */
ASX_API void asx_runtime_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_H */
