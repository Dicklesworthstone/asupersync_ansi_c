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
    int cancel_pending; /* 1 if cancel signal delivered */
    int detached;       /* 1 if the slot is released at completion */
    uint32_t next_free; /* free-list link while !alive */
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
    /* Join waiters: intrusive singly-linked list of slot indices. */
    uint32_t first_waiter; /* first task parked in join on this one */
    uint32_t next_waiter;  /* link while waiting on another task */
    uint32_t waiting_on;   /* slot index of join target, or ASX_SLOT_NONE */
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
void asx_task_join_wake_waiters_internal(asx_task_slot *task);

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

/* Reset private hook installation state during runtime teardown. */
void asx_runtime_hooks_reset_internal(void);

#endif /* ASX_RUNTIME_INTERNAL_H */
