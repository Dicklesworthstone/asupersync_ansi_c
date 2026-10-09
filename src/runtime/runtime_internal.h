/*
 * runtime_internal.h — shared internal state for runtime kernel
 *
 * NOT part of the public API. Used only by runtime .c translation units.
 * Static arenas; dynamic allocation requires platform threading hooks.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_INTERNAL_H
#define ASX_RUNTIME_INTERNAL_H

#include <asx/asx_config.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/cancel.h>
#include <asx/core/cleanup.h>
#include <asx/core/outcome.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <asx/runtime/runtime.h>

/* -------------------------------------------------------------------
 * Arena slot types (walking skeleton: fixed-size)
 * ------------------------------------------------------------------- */

typedef struct {
    asx_region_state state;
    asx_region_id parent_id;
    uint32_t child_count;
    asx_region_id children[ASX_MAX_REGION_CHILDREN];
    uint32_t task_count;       /* live (non-completed) tasks */
    uint32_t task_total;       /* total spawned tasks */
    uint16_t generation;       /* increments on slot reclaim */
    int alive;                 /* 1 if slot in use */
    int poisoned;              /* 1 if region has been poisoned (containment) */
    asx_cleanup_stack cleanup; /* LIFO cleanup for finalization */
    asx_budget budget;         /* inherited by tasks and child regions */
    uint8_t capture_arena[ASX_REGION_CAPTURE_ARENA_BYTES];
    uint32_t capture_used;
    /* Region cancellation (Rust RegionRecord cancel reason): set by the
     * first asx_region_cancel reaching the region, strengthened by later
     * ones. Descendants' reasons chain to their parent's via `cause`. */
    int cancel_requested;
    asx_cancel_reason cancel_reason;
} asx_region_slot;

typedef struct {
    asx_task_state state;
    asx_region_id region;
    asx_task_poll_fn poll_fn;
    void *user_data;
    asx_outcome outcome;
    uint16_t generation; /* increments on slot reclaim */
    int alive;
    void *captured_state;
    uint32_t captured_size;
    asx_task_state_dtor_fn captured_dtor;
    /* Cancellation tracking (bd-2cw.3) */
    asx_cancel_phase cancel_phase;
    asx_cancel_reason cancel_reason;
    asx_cancel_witness_id cancel_witness;
    uint32_t cancel_epoch;
    uint32_t cleanup_polls_remaining;
    int cancel_pending;  /* 1 if cancel signal delivered */
    uint32_t mask_depth; /* asx_task_mask() nesting; cancel deferred while > 0 */
    int detached;        /* 1 if the slot is released at completion */
    uint32_t next_free;  /* free-list link while !alive */
    /* Wake-driven scheduling. A task that returns ASX_E_PENDING stays
     * runnable (yield) unless it called asx_task_park() during that poll
     * and was not woken before the poll returned. */
    uint8_t in_poll;        /* 1 while the scheduler is inside poll_fn */
    uint8_t park_requested; /* asx_task_park() called during this poll */
    uint8_t notified;       /* woken while in_poll */
    uint8_t parked;         /* not runnable until asx_task_wake() */
    asx_status last_error;  /* status returned by a failing poll_fn */
    /* Task timer (EDF heap keyed by (wake_at, timer_seq)). */
    asx_time wake_at;
    uint64_t timer_seq;
    uint32_t timer_pos; /* heap index, ASX_SLOT_NONE when disarmed */
    /* Deadline of the task's traced sleep timer (asx_task_wait_until),
     * 0 when none: ASX_TRACE_TIMER_SET when registered, TIMER_FIRE when
     * due, TIMER_CANCEL when the task completes first. Internal wakeups
     * (budget deadlines, asx_task_arm_timer) are not traced timers. */
    asx_time traced_deadline;
    /* asx_task_panic() during the current poll: the task completes as
     * PANICKED with this (borrowed) message when the poll returns. */
    int panicked;
    const char *panic_message;
    /* Join waiters: intrusive singly-linked list of slot indices. */
    uint32_t first_waiter; /* first task parked in join on this one */
    uint32_t next_waiter;  /* link while waiting on another task */
    uint32_t waiting_on;   /* slot index of join target, or ASX_SLOT_NONE */
    /* Completion watcher (task-group owner): woken when this task
     * completes, if that slot still holds generation `watcher_gen`. */
    uint32_t watcher;
    uint16_t watcher_gen;
    /* Budget: deadline -> DEADLINE cancel, poll quota -> POLL_QUOTA cancel,
     * cost quota -> COST_BUDGET cancel. Inherited from the region. */
    asx_budget budget;
    /* Obligations held by this task (intrusive list of obligation slots). */
    uint32_t first_held;
} asx_task_slot;

typedef struct {
    asx_obligation_state state;
    asx_region_id region;
    uint16_t generation;
    int alive;
    uint32_t next_free; /* free-list link while !alive */
    asx_obligation_kind kind;
    asx_task_id holder;                       /* ASX_INVALID_ID if unowned */
    uint32_t next_held;                       /* link in the holder's list */
    asx_obligation_abort_reason abort_reason; /* why it was aborted */
} asx_obligation_slot;

/* Free-list terminator for arena slot links. */
#define ASX_SLOT_NONE 0xFFFFFFFFu

/* -------------------------------------------------------------------
 * Global arenas (defined in lifecycle.c)
 *
 * g_*_count is the high-water mark: every slot index below it has been
 * used at least once, so arena scans iterate [0, g_*_count) and skip
 * !alive slots. g_*_live counts currently-allocated slots.
 * ------------------------------------------------------------------- */

extern asx_region_slot g_regions[ASX_MAX_REGIONS];
extern uint32_t g_region_count;
extern uint32_t g_region_live;

extern asx_task_slot g_tasks[ASX_MAX_TASKS];
extern uint32_t g_task_count;
extern uint32_t g_task_live;

extern asx_obligation_slot g_obligations[ASX_MAX_OBLIGATIONS];
extern uint32_t g_obligation_count;
extern uint32_t g_obligation_live;

/* -------------------------------------------------------------------
 * Shared lookup functions (generation-safe, used across TUs)
 * ------------------------------------------------------------------- */

ASX_MUST_USE asx_status asx_region_slot_lookup(asx_region_id id, asx_region_slot **out);
ASX_MUST_USE asx_status asx_task_slot_lookup(asx_task_id id, asx_task_slot **out);
ASX_MUST_USE asx_status asx_obligation_slot_lookup(asx_obligation_id id, asx_obligation_slot **out);

/* Release captured state for a task exactly once. */
void asx_task_release_capture_internal(asx_task_slot *task);

/* Build the public handle for an occupied task slot. */
asx_task_id asx_task_handle_for_slot(uint32_t slot_idx);

/* Completion bookkeeping shared by every path that drives a task to
 * COMPLETED: disarms its timer, wakes join waiters, releases captured
 * state, decrements the region's live-task count, and frees the slot if
 * the task was detached. The caller must already have set
 * state/outcome. */
void asx_task_on_complete_internal(asx_task_slot *task, asx_region_slot *region);

/* Reset wake-driven scheduler state (timer heap, sequence counters).
 * Called from asx_runtime_reset(). Defined in scheduler.c. */
void asx_scheduler_reset_internal(void);

/* Initialize the scheduling fields of a freshly allocated task slot. */
void asx_task_sched_init_internal(asx_task_slot *task);

/* Make a task runnable (or record the wake if it is mid-poll). */
void asx_task_wake_slot_internal(asx_task_slot *task);

/* Timer / join-wait teardown for a task leaving the live set. */
void asx_task_timer_disarm_internal(asx_task_slot *task);
void asx_task_join_detach_internal(asx_task_slot *task);
/* Wakes join waiters and the completion watcher of a completing task. */
void asx_task_join_wake_waiters_internal(asx_task_slot *task);

/* After a task's completion is recorded (ASX_TRACE_SCHED_COMPLETE): let
 * its region finalize if it is closing and that was its last task. The
 * region event must follow the completion it depends on. */
void asx_region_settle_internal(asx_region_id rid);

/* Finalize a closing region whose tasks have all completed, then its
 * closing ancestors in turn (Rust advance_region_state). No-op for an Open
 * or Closed region. Defined in quiescence.c. */
void asx_region_advance_internal(asx_region_id id);

/* Emit an event whose observer call carries `payload` (trace.c). */
void asx_trace_emit_payload_internal(asx_trace_event_kind kind, uint64_t entity_id, uint64_t aux,
                                     const asx_trace_payload *payload);

/* The time a cancel reason is stamped with (runtime clock, else virtual). */
asx_time asx_cancel_now_internal(void);

/* The core of every task cancel: a newly cancelled task takes `reason`
 * whole and records ASX_TRACE_CANCEL_REQUEST when `trace_request` is set;
 * an already cancelled one is strengthened (asx_cancel_strengthen) and
 * records nothing. A terminal task is left alone (ASX_OK). */
ASX_MUST_USE asx_status asx_task_cancel_reason_internal(asx_task_id id,
                                                        const asx_cancel_reason *reason,
                                                        int trace_request);

/* Budget exhaustion observed for task `id` (deadline, poll quota, cost):
 * attributed to the task and its region and stamped `at`, recorded on the
 * task only, as Rust's checkpoint budget check does (cx.rs:3112-3160): no
 * cancel.requested event. */
ASX_MUST_USE asx_status asx_task_cancel_budget_internal(asx_task_id id, asx_cancel_kind kind,
                                                        asx_time at);

/* Collect the region subtree rooted at `root` in parent-first (BFS)
 * order as region slot indices. Returns the count written (bounded by
 * `max`), or 0 if root does not resolve. Defined in lifecycle.c. */
uint32_t asx_region_subtree_internal(asx_region_id root, uint32_t *out_slots, uint32_t max);

/* Build the public handle for an occupied region slot. */
asx_region_id asx_region_handle_for_slot(uint32_t slot_idx);

/* Active obligation-leak policy (set by asx_runtime_init / reload; LOG
 * after a bare asx_runtime_reset). */
void asx_runtime_set_leak_policy_internal(asx_leak_response response,
                                          const asx_leak_escalation_config *escalation);

/* Resolve the obligations a completing task still holds: aborted with
 * reason CANCEL if the task was cancelled, otherwise handled as leaks per
 * the active policy. Returns the number of leaks recorded. Sets
 * *out_fail_fast when the policy demands fail-fast containment. */
uint32_t asx_task_resolve_held_obligations_internal(asx_task_slot *task, int *out_fail_fast);

/* Take (and clear) a fault raised during completion bookkeeping, e.g. an
 * obligation leak under the PANIC policy. ASX_OK when none is pending. */
asx_status asx_runtime_take_pending_fault_internal(void);

/* Scheduler block handshake with cross-thread wakers (waker.c). Call
 * prepare right before a blocking reactor wait: it returns 0 (do not
 * block) when a waker is already signaled, else marks the scheduler as
 * blocking so later wakes call asx_runtime_reactor_notify(). Call finish
 * after the wait returns. */
int asx_waker_prepare_block_internal(void);
void asx_waker_finish_block_internal(void);

/* Reset private hook installation state during runtime teardown. */
void asx_runtime_hooks_reset_internal(void);

#endif /* ASX_RUNTIME_INTERNAL_H */
