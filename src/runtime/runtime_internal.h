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

/* Under lab dispatch, a Rust DetEntropy source (util/entropy.rs:66-150):
 * the seed it was made with (its children fork from it), its DetRng
 * xorshift64 state and its fork counter. ASX_LAB_ENTROPY_OWED: the task's
 * fork from its spawner is still owed (Rust's DeferredFork, forced at the
 * task's first poll). */
#define ASX_LAB_ENTROPY_NONE 0u
#define ASX_LAB_ENTROPY_OWED 1u
#define ASX_LAB_ENTROPY_READY 2u

typedef struct {
    uint64_t seed;
    uint64_t rng;
    uint64_t forks;
    uint8_t state;
} asx_lab_entropy;

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
    asx_region_limits limits;  /* admission limits (unlimited by default) */
    /* Captured task state is carved at 8-byte offsets: the union keeps the
     * arena itself 8-byte aligned whatever fields precede it. */
    union {
        uint8_t bytes[ASX_REGION_CAPTURE_ARENA_BYTES];
        uint64_t align_u64;
        double align_double;
        void *align_ptr;
    } capture_arena;
    uint32_t capture_used;
    /* Region cancellation (Rust RegionRecord cancel reason): set by the
     * first asx_region_cancel reaching the region, strengthened by later
     * ones. Descendants' reasons chain to their parent's via `cause`. */
    int cancel_requested;
    asx_cancel_reason cancel_reason;
    /* Lab dispatch: the entropy of the region's principal, a child region a
     * task opened (Rust mints its Cx with a fork of the runtime source,
     * state.rs:5093); a task spawned into it forks from this. */
    asx_lab_entropy lab_principal;
    /* Lab dispatch: tasks spawned from a poll into this region that the
     * next step has not admitted yet (counted in task_count). Admission
     * checks max_tasks against the admitted ones only, as Rust's mailbox
     * admission counts the region's live members. */
    uint32_t lab_pending_admissions;
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
    /* The cleanup budget the cancel carries (Rust TaskState::CancelRequested
     * cleanup_budget, record/task.rs:730-820): the request's cleanup budget,
     * met with every later request's. Once the task acknowledges, it becomes
     * the task's budget (after that poll, as the lab applies the
     * acknowledgement, lab/runtime.rs:4809, record/task.rs:1340), and it
     * replaces the budget again on any request during cleanup. A spent
     * cleanup quota then only strengthens the reason to POLL_QUOTA. */
    asx_budget cleanup_budget;
    uint8_t cleanup_applied; /* budget == cleanup_budget since acknowledgement */
    /* Opt-in hard bound only (asx_runtime_config.cleanup_hard_bound): polls
     * left, counted from the request, before the task is force-completed. */
    uint32_t cleanup_polls_remaining;
    int cancel_pending; /* 1 if cancel signal delivered */
    /* A budget cancel the task raised itself (its quota, cost or deadline
     * observed at a poll or checkpoint) that its record has not taken yet:
     * Rust sets it on the task's Cx only, the record stays Running, and an
     * acknowledging checkpoint reconciles it into the record
     * (consume_checkpoint_cancel_ack, record/task.rs:1104-1170; bd-mex3).
     * cancel_pending and cancel_reason are set; the state is unchanged. */
    uint8_t cancel_unmaterialized;
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
    uint8_t cancel_polled;  /* polled since its cancel was requested */
    /* Rust's spawn completion policy (task_handle.rs:173-202): a task
     * spawned from inside another task's poll (Rust cx.spawn) keeps the
     * value it returns after acknowledging a cancel that came after its
     * first poll; others are cancellation-dominant. */
    uint8_t spawned_in_poll;
    uint8_t first_polled;
    uint8_t cancel_before_first_poll;
    /* Lab dispatch (lab_dispatch.c): scheduled on a lane; the priority of
     * the waker of its last poll (Rust's cached TaskWaker, LR:4686-4692);
     * spawned from a poll and not yet admitted. */
    uint8_t lab_scheduled;
    uint8_t lab_waker_prio;
    uint8_t lab_admission_pending;
    /* Why its admission refused it (ASX_E_ADMISSION_LIMIT or
     * ASX_E_REGION_CLOSED; ASX_OK if not refused): it never ran. */
    asx_status lab_admission_refusal;
    /* A checkpoint of the current poll observed the cancel (Rust's
     * cancel_acknowledged, consumed after the poll, LR:4809); a budget
     * cancel a checkpoint raised whose cancel waker has not fired yet
     * (Rust's cancel_wakers_pending, cx.rs:2824-2840, fired with the
     * acknowledgement, record/task.rs:1216-1237). */
    uint8_t lab_ack_in_poll;
    uint8_t cancel_wakers_pending;
    /* Bumped whenever a poll gets a new waker (the first poll, or a
     * priority different from the last poll's); the traced sleep timer
     * remembers the epoch it was registered under (Sleep re-registers on a
     * changed waker, time/sleep.rs:883-998). */
    uint32_t lab_waker_epoch;
    uint32_t traced_waker_epoch;
    /* The child-region command this task awaits (asx_region_open_child_poll,
     * asx_region_close_poll; Rust's ChildRegionOpening and
     * RegionQuiescence): ASX_REGION_WAIT_*. An applied open leaves its
     * status and region here for the next poll; a close waits for CLOSED of
     * region_wait_region. */
    uint8_t region_wait;
    asx_status region_wait_status;
    asx_region_id region_wait_region;
    /* When the task joined its region's membership (spawn, or lab
     * admission for a child spawned in a poll): a region cancel visits its
     * tasks in this order, as Rust's insertion-ordered Membership. */
    uint64_t member_seq;
    asx_status last_error; /* status returned by a failing poll_fn */
    /* Task timer (EDF heap keyed by (wake_at, timer_seq)). */
    asx_time wake_at;
    uint64_t timer_seq;
    uint32_t timer_pos; /* heap index, ASX_SLOT_NONE when disarmed */
    /* Lab dispatch: the priority of the waker the timer was (re)armed
     * with, the waker of that poll. A Rust Sleep wakes the waker it last
     * registered, so a task polled again without re-polling its sleep (its
     * waker's priority changed meanwhile) is woken at the old priority. */
    uint8_t timer_waker_prio;
    /* Lab dispatch: the budget-deadline timer Rust arms for the task when
     * it is created or admitted (Cx::arm_budget_deadline), a wheel timer of
     * its own, pending until it fires or the task ends. */
    uint8_t deadline_timer_armed;
    asx_time deadline_timer_at;
    uint64_t deadline_timer_seq;
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
    /* Under lab dispatch, the waker priority of the watcher's poll that
     * registered the watch: a join registration keeps that waker until the
     * join is polled again (task_handle.rs:115-133), so a watcher that has
     * since changed priority is woken at the old one. */
    uint8_t watcher_prio;
    /* Budget: deadline -> DEADLINE cancel, poll quota -> POLL_QUOTA cancel,
     * cost quota -> COST_BUDGET cancel. Inherited from the region. */
    asx_budget budget;
    /* Obligations held by this task (intrusive list of obligation slots). */
    uint32_t first_held;
    /* Lab dispatch: the task's Rust task-arena id (index and generation;
     * assigned at creation, or at admission for a child spawned in a poll,
     * and freed when it completes, as Rust's arena does), its entropy, and,
     * while its fork is owed, the source it forks from: a task slot or
     * (lab_ent_parent_region) a region's principal. */
    uint32_t lab_rust_index;
    uint32_t lab_rust_gen;
    uint8_t lab_rust_live;
    asx_lab_entropy lab_ent;
    uint32_t lab_ent_parent;
    uint16_t lab_ent_parent_gen;
    uint8_t lab_ent_parent_region;
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
    /* Rust's obligation mailbox (bd-2fga): under lab dispatch the
     * operations made inside a poll are posts applied, in order, when the
     * poll returns. counted: it counts against its region's
     * max_obligations; checked: reserved through the checked path, whose
     * admission and resolutions count at the call; posts: posts still to
     * apply that name it. */
    uint8_t counted;
    uint8_t checked;
    uint8_t posts;
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

/* Apply the obligation posts the poll that just returned made (Rust's
 * obligation-mailbox drain at the end of a lab poll, bd-2fga). */
void asx_obligation_drain_posts_internal(void);

/* Reset wake-driven scheduler state (timer heap, sequence counters).
 * Called from asx_runtime_reset(). Defined in scheduler.c. */
void asx_scheduler_reset_internal(void);

/* Initialize the scheduling fields of a freshly allocated task slot. */
void asx_task_sched_init_internal(asx_task_slot *task);

/* Make a task runnable (or record the wake if it is mid-poll). */
void asx_task_wake_slot_internal(asx_task_slot *task);

/* Catch a panic of the poll in progress (Rust catch_unwind inside a task
 * body): if `t` is being polled and asx_task_panic was called, the panic
 * is cleared, its message written to *out_message (may be NULL) and 1
 * returned; the task then completes as its poll returns. 0 otherwise. */
int asx_task_catch_panic_internal(asx_task_slot *t, const char **out_message);

/* Timer / join-wait teardown for a task leaving the live set. */
void asx_task_timer_disarm_internal(asx_task_slot *task);
void asx_task_join_detach_internal(asx_task_slot *task);
/* Wakes join waiters and the completion watcher of a completing task. */
void asx_task_join_wake_waiters_internal(asx_task_slot *task);

/* After a task's completion is recorded (ASX_TRACE_SCHED_COMPLETE):
 * advance its region if it is closing (to DRAINING while it has child
 * regions; to finalization once that was its last task). The region event
 * must follow the completion it depends on. */
void asx_region_settle_internal(asx_region_id rid);

/* Tasks of the region admitted and not yet completed (under lab dispatch,
 * spawns still waiting for admission are left out). */
uint32_t asx_region_live_admitted_internal(const asx_region_slot *r);

/* The task, spawned from a poll under lab dispatch, is refused by its
 * admission: the region is closed (`why` ASX_E_REGION_CLOSED) or at
 * max_tasks (ASX_E_ADMISSION_LIMIT). It completes without running or
 * leaving a trace, Cancelled as Rust resolves the refused handle, waking
 * its joiners, and the region is re-advanced. */
void asx_task_refuse_admission_internal(asx_task_slot *t, asx_region_slot *r, asx_status why);

/* The earliest armed task timer (task wake timers and, under lab
 * dispatch, budget-deadline timers): 1 and *out_next, or 0 when none is
 * armed. Defined in scheduler.c. */
int asx_scheduler_next_timer_internal(asx_time *out_next);

/* Fire the task timers due now (waking their tasks); the number woken.
 * Defined in scheduler.c. */
uint32_t asx_scheduler_fire_due_timers_internal(void);

/* Set the runtime virtual clock, backwards too (a lab snapshot restore).
 * Defined in hooks.c. */
void asx_runtime_virtual_set_internal(asx_time to);

/* Finalize a closing region whose tasks have all completed, then its
 * closing ancestors in turn (Rust advance_region_state). No-op for an Open
 * or Closed region. Defined in quiescence.c. */
void asx_region_advance_internal(asx_region_id id);

/* Emit an event whose observer call carries `payload` (trace.c). */
void asx_trace_emit_payload_internal(asx_trace_event_kind kind, uint64_t entity_id, uint64_t aux,
                                     const asx_trace_payload *payload);

/* The time a cancel reason is stamped with (runtime clock, else virtual). */
asx_time asx_cancel_now_internal(void);

/* Where a cancel request comes from. It decides the trace event and, under
 * lab dispatch, how the task is scheduled (Rust: run.rs driver cancels,
 * state.rs:7811-7876, cx.rs:2824-2836):
 *   DIRECT — a task cancel (RuntimeState::cancel_task): no cancel.requested
 *            event; lab: the cleanup-priority cancel entry only when newly
 *            cancelled.
 *   REGION — a region or policy cancel (cancel_request): cancel.requested
 *            for a newly cancelled task; lab: the cleanup-priority entry
 *            whenever the request changed the reason or cleanup budget, the
 *            cancel wakes held until the whole region is visited.
 *   HANDLE — a join handle's abort, as the runtime applies it
 *            (cancel_task_for_handle, state.rs:3462; record/task.rs:1019):
 *            no event; lab: the cleanup-priority entry whenever the request
 *            changed the reason or cleanup budget.
 *   BUDGET — budget exhaustion the task observes itself: no event and no
 *            scheduling (Rust raises it on the task's Cx only). */
typedef enum {
    ASX_CANCEL_SRC_DIRECT = 0,
    ASX_CANCEL_SRC_REGION = 1,
    ASX_CANCEL_SRC_BUDGET = 2,
    ASX_CANCEL_SRC_HANDLE = 3
} asx_cancel_source;

/* Move a budget cancel the record has not taken (cancel_unmaterialized)
 * into the record: Running → CancelRequested, no trace event (Rust
 * reconcile_checkpoint_cancel). No-op otherwise. */
void asx_task_materialize_cancel_internal(asx_task_slot *t);

/* The core of every task cancel: a newly cancelled task takes `reason`
 * whole and records ASX_TRACE_CANCEL_REQUEST for a REGION source; an
 * already cancelled one is strengthened (asx_cancel_strengthen) and
 * records nothing. A terminal task is left alone (ASX_OK). */
ASX_MUST_USE asx_status asx_task_cancel_reason_internal(asx_task_id id,
                                                        const asx_cancel_reason *reason,
                                                        asx_cancel_source source);

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

/* The next region-membership sequence number (asx_task_slot.member_seq). */
uint64_t asx_task_next_member_seq_internal(void);

/* Opt-in hard cleanup bound (asx_runtime_config.cleanup_hard_bound; off
 * after a bare asx_runtime_reset). */
void asx_runtime_set_cleanup_hard_bound_internal(int on);
int asx_cleanup_hard_bound_internal(void);

/* A task that acknowledged its cancel takes its cleanup budget as its
 * budget, once (see asx_task_slot.cleanup_budget). */
void asx_task_apply_cleanup_budget_internal(asx_task_slot *t);

/* Lab dispatch (lab_dispatch.c, bd-9kll.4.2): the Rust LabRuntime's
 * one-task-per-step dispatch model, on when asx_scheduler_use_lab_dispatch
 * was called (off after asx_runtime_reset). */
void asx_lab_dispatch_reset_internal(void);
int asx_lab_dispatch_active(void);
int asx_lab_dispatch_overflowed(void);
uint64_t asx_lab_rng_next(void);
uint32_t asx_lab_scheduled_count(void);
int asx_lab_is_scheduled(const asx_task_slot *t);
/* Ready lane (no-op when already scheduled) / cancel lane (always pushes). */
void asx_lab_schedule(asx_task_slot *t, uint8_t priority);
void asx_lab_schedule_cancel(asx_task_slot *t, uint8_t priority);
/* The step's pick for value r: 1 with the slot and the generation of the
 * task picked, or 0 when nothing is scheduled. A generation that is not
 * the slot's live task's, or a terminal task, is a retired task: its
 * dispatch polls nothing. */
int asx_lab_pick(uint64_t r, uint32_t *out_slot, uint16_t *out_task_gen, int *out_cancel_lane);
/* Arm a task's budget-deadline timer as Rust does when the task is created
 * or admitted (lab dispatch only; scheduler.c). */
void asx_lab_arm_budget_deadline_internal(asx_task_slot *t);
/* A cancel waker that fires after its task (slot, generation) completed:
 * Rust schedules the retired task id, and the next pick of it dispatches
 * nothing. */
void asx_lab_schedule_cancel_retired(uint32_t slot, uint16_t task_gen, uint8_t priority);
/* A step begins: its number, from 1 (Rust's LabRuntime::steps). */
uint64_t asx_lab_step_begin_internal(void);
/* A pick, for asx_scheduler_record_dispatches. */
void asx_lab_record_dispatch_internal(uint32_t slot, uint16_t task_gen, int cancel_lane,
                                      uint64_t step, asx_time at);
/* A completed task: purge its entries if it is still scheduled. */
void asx_lab_forget(asx_task_slot *t);
void asx_lab_defer_admission(asx_task_slot *t);
/* A spawn call starts (clears the last refusal ticket); one refused from a
 * poll because its region is closing is queued for the next step's
 * admission, as Rust's spawn mailbox refuses it then. */
void asx_lab_note_spawn_internal(void);
void asx_lab_defer_refused_admission(void);
/* Register `t`, at its current poll's waker priority, to be woken when
 * the refusal is delivered (one registration per refusal: the latest). */
void asx_lab_refusal_watch(uint32_t ticket, const asx_task_slot *t);
int asx_lab_admissions_pending(void);
void asx_lab_admit_pending(void);
/* Per-task entropy under lab dispatch (Rust DetEntropy, see
 * asx_lab_entropy). A task the host creates takes the next task-arena
 * index and forks the runtime source at once (state.rs:4356); one spawned
 * in a poll owes a fork from `parent_region`'s principal (a task spawning
 * into a child region it opened: Rust's child.cx().spawn) or else from its
 * spawner, made at its first poll (DeferredFork, cx.rs:239-286, forced in
 * overlay_parent_inheritance, :5876-5879). */
void asx_lab_entropy_direct_internal(asx_task_slot *t);
/* A completed task leaves Rust's task arena (recycle_task, state.rs:8497):
 * its index is reused next, with the generation bumped. */
void asx_lab_task_retired_internal(asx_task_slot *t);
void asx_lab_entropy_spawned_internal(asx_task_slot *t, const asx_task_slot *spawner,
                                      asx_region_slot *parent_region);
void asx_lab_entropy_first_poll_internal(asx_task_slot *t);
/* Rust's Cx::random_usize(bound) (cx.rs:3622-3634) from the task's stream:
 * 1 and *out in [0, bound), or 0 when lab dispatch is off or the task has
 * no entropy (the caller decides deterministically then). bound > 0. */
int asx_lab_entropy_index_internal(asx_task_slot *t, uint32_t bound, uint32_t *out);
/* A cancel request's wake of the task's CancelTaskWaker; held back while a
 * region cancel batch is open. */
void asx_lab_cancel_wake(asx_task_slot *t);
void asx_lab_cancel_batch_begin(void);
void asx_lab_cancel_batch_end(void);
/* Region commands (Rust RegionCommand Create/Cancel/Close, LR:4135-4217):
 * queued by a task's child-region open, cancel or close, applied in order
 * at the start of the next step, after spawn admissions. An open's result
 * wakes its opener once every command of the batch is applied. Queueing
 * fails with ASX_E_RESOURCE_EXHAUSTED when the queue is full, changing
 * nothing. A Cancel's reason (and its message) must outlive the command. */
asx_status asx_lab_region_open_command(asx_task_slot *opener, asx_region_id parent,
                                       const asx_budget *budget);
asx_status asx_lab_region_cancel_command(asx_region_id region, const asx_cancel_reason *reason);
int asx_lab_region_commands_pending(void);
void asx_lab_drain_region_commands(void);
/* drain_deferred_cancel_dispatches (LR:4219-4239): publish the cancel lane
 * entries, then the cancel wakes, a region command's cancel deferred. */
int asx_lab_deferred_cancels_pending(void);
void asx_lab_drain_deferred_cancels(void);
/* Join-handle aborts (Rust JoinHandle::abort_with_reason): queued, applied
 * at the start of the next step after admissions and around the region
 * commands (at most 16 per drain, coalesced per task); a target awaiting
 * admission takes its aborts at admission, onto the cancel lane only.
 * Queueing fails with ASX_E_RESOURCE_EXHAUSTED when full, changing
 * nothing. */
asx_status asx_lab_handle_cancel_command(const asx_task_slot *t, const asx_cancel_reason *reason);
int asx_lab_handle_cancels_pending(void);
void asx_lab_drain_handle_cancels(void);

/* asx_task_slot.region_wait */
enum {
    ASX_REGION_WAIT_NONE = 0,
    ASX_REGION_WAIT_OPEN,   /* open queued, not applied yet */
    ASX_REGION_WAIT_OPENED, /* open applied: region_wait_status/_region */
    ASX_REGION_WAIT_CLOSE   /* close requested: waiting for CLOSED */
};

/* asx_region_open_child_poll for task `t` with the opener's inherited
 * budget given whole: the opener is then another Cx than t's (Rust's
 * ChildRegion::cx() opening a grandchild, whose inherited budget is its
 * region's), while `t` awaits the open. */
asx_status asx_region_open_child_poll_internal(asx_task_slot *t, asx_region_id parent,
                                               const asx_budget *inherited,
                                               asx_region_id *out_child);

/* Wake the tasks waiting for this region to close (Rust RegionCloseState
 * waiters, woken at Finalizing -> Closed, record/region.rs:1845-1853). */
void asx_region_wake_close_waiters_internal(asx_region_id id);
/* The reason Rust's Close command cancels a child region with:
 * CancelReason::user("owned child region body finished") and its testing
 * default attribution (lab/runtime.rs:4193-4196). */
asx_cancel_reason asx_region_close_reason_internal(void);
/* A region command reaching a region that closed and whose slot was
 * reused: Rust's cancel_request still records region.cancelled for the
 * region, whose record is gone (state.rs:7756-7758), and does nothing
 * else. */
void asx_region_trace_cancel_of_gone_internal(asx_region_id region,
                                              const asx_cancel_reason *reason);

/* Resolve the obligations a completing task still holds, cancelled or
 * not, as leaks per the active policy (RECOVER aborts them with
 * LEAK_RECOVERED). Returns the number of leaks recorded. Sets
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
