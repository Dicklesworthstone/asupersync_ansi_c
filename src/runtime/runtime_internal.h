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

#include "../sync/sync_state.h"
#include "fnv_fold.h"
#include <asx/asx_config.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/cancel.h>
#include <asx/core/cleanup.h>
#include <asx/core/outcome.h>
#include <asx/platform/atomics.h>
#define ASX_INTERNAL_TIMER_WHEEL_ACCESS 1
#include <asx/time/timer_wheel.h>
#define ASX_INTERNAL_WAKER_ACCESS 1
#include <asx/runtime/waker.h>
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

/* A region's async finalizer in flight (Rust active_async_finalizers, the
 * LIFO barrier of runtime/state.rs drain_ready_async_finalizers): the
 * masked cleanup task running the finalizer popped off the cleanup stack.
 * The next finalizer waits for it. The task's poll function is a trampoline
 * over this record, which counts its polls against the finalizer budget
 * (asx_runtime_config.finalizer_poll_budget / _time_budget_ns). */
typedef struct {
    int active;         /* 1 while the finalizer task is live */
    uint32_t task_slot; /* its slot, valid while active */
    union {
        asx_cleanup_poll_fn poll; /* an async finalizer's own poll function */
        asx_cleanup_fn fn;        /* a sync finalizer's callback (sync != 0) */
    } run;
    void *data;           /* its context */
    uint32_t polls;       /* polls the finalizer has been given */
    uint32_t poll_budget; /* polls allowed; the config's, at admission */
    asx_time deadline;    /* its absolute time budget, at admission */
    int exhausted;        /* its budget was found spent (escalated once) */
    int sync;             /* run.fn is the entry: one call, no budget to spend */
} asx_region_finalizer;

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
    asx_budget budget; /* met into child regions; tasks spawned into it from another region */
    asx_capability_budget capability_budget; /* resource envelopes; a child's is the meet (Rust) */
    asx_region_limits limits;                /* admission limits (unlimited by default) */
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
    /* Rust RegionRecord::shutdown_budget (record/region.rs:1203-1222): the
     * ceiling a cancel with a shutdown budget leaves, the tightest of every
     * one that reached the region (its own request's, and its ancestors'
     * carried down by the cancel). Each task such a cancel cancels takes
     * the cleanup budget of its reason met with it (state.rs:7829-7834). */
    int has_shutdown_budget;
    asx_budget shutdown_budget;
    /* Rust RegionRecord::close_outcome (record/region.rs:1139-1150): the
     * outcome of every task that completed in the region, joined. A region
     * that closes with none recorded reports Cancelled if it was cancelled,
     * else Ok (complete_close's default, :1810-1823). */
    int has_close_outcome;
    asx_outcome close_outcome;
    /* Lab dispatch: the entropy of the region's principal, a child region a
     * task opened (Rust mints its Cx with a fork of the runtime source,
     * state.rs:5093); a task spawned into it forks from this. */
    asx_lab_entropy lab_principal;
    /* Lab dispatch: tasks spawned from a poll into this region that the
     * next step has not admitted yet (counted in task_count). Admission
     * checks max_tasks against the admitted ones only, as Rust's mailbox
     * admission counts the region's live members. */
    uint32_t lab_pending_admissions;
    /* The async finalizer running as this region's cleanup task, if any. */
    asx_region_finalizer finalizer;
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
    /* The same for a budget cancel raised once the record already holds a
     * request: Rust strengthens the Cx's reason only (the lab's poll-quota
     * check, lab/runtime.rs:4661-4669; a masked checkpoint), so the record
     * keeps its reason and cleanup budget until a checkpoint acknowledges
     * (reconcile_checkpoint_cancel, record/task.rs:1096-1153) or a new
     * request is strengthened by it (:716-722). Set when the Cx's reason
     * (cx_cancel_reason) is stronger than cancel_reason. */
    uint8_t cx_reason_ahead;
    asx_cancel_reason cx_cancel_reason;
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
    uint8_t cleanup_only; /* explicit cleanup admission, preserved through the lab mailbox */
    /* A region's async finalizer task (asx_task_spawn_finalizer_internal):
     * its outcome is what its poll returned, as Rust's finalizer future's
     * Outcome is, whatever cancel or budget exhaustion it was masked from. */
    uint8_t finalizer;
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
     * region_wait_region. A monitor request (asx_monitor_open_poll, Rust's
     * MonitorOpening) is a command of the same lane: an applied one leaves
     * its status here and the monitor reference in region_wait_region (both
     * are 64-bit ids). */
    uint8_t region_wait;
    asx_status region_wait_status;
    asx_region_id region_wait_region;
    /* The waker priority of the poll that started the close wait: Rust's
     * RegionQuiescence keeps every distinct waker it is polled with
     * (cx/child_region.rs:560-571) and wakes them in order, so the first
     * one schedules the task; later ones find it scheduled (fuzz finding
     * gen-103-136). */
    uint8_t region_wait_prio;
    /* When the task joined its region's membership (spawn, or lab
     * admission for a child spawned in a poll): a region cancel visits its
     * tasks in this order, as Rust's insertion-ordered Membership. */
    uint64_t member_seq;
    asx_status last_error; /* status returned by a failing poll_fn */
    /* Task timer: the task's registration in its kernel's scheduler wheel
     * (asx_kernel.wheel), `timer` valid while `timer_armed`. */
    asx_time wake_at;
    asx_timer_handle timer;
    uint8_t timer_armed;
    /* The wake timer was registered for a deadline past the wheel's
     * maximum timer duration, so it fires early, at that horizon, and the
     * sleep registers again (Rust's registered_partial, time/sleep.rs:935). */
    uint8_t timer_partial;
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
    asx_timer_handle deadline_timer;
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
    uint64_t lab_last_polled_step; /* lab steps at its last poll (Rust last_polled_step) */
    /* Why an open carrying a capability envelope was refused, packed by
     * asx_capability_refusal_pack_internal: written with region_wait_status
     * = ASX_E_CAPABILITY_BUDGET_REFUSED and read only when it is that. */
    uint8_t region_wait_refusal;
} asx_task_slot;

/* The drop of a permit an obligation tracks (asx_obligation_set_drop_internal). */
typedef int (*asx_obligation_drop_fn)(uint64_t a, uint64_t b);

/* Read-only raw-ID application preflight: validates a Reserved obligation's
 * actual scheduler caller and holder/region. A bound primitive's credit
 * requires its owned permit instead. Resolved credits return their typed error;
 * completion and leak-policy settlement remain internal slot operations. */
ASX_MUST_USE asx_status asx_obligation_check_access_internal(asx_obligation_id id);

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
    /* The permit's drop, run if its holder completes with the obligation
     * still reserved (NULL: a plain obligation, which leaks then). */
    asx_obligation_drop_fn drop_fn;
    uint64_t drop_a;
    uint64_t drop_b;
    uint32_t drop_serial; /* authenticates a distinct semaphore permit */
    /* Rust's obligation mailbox (bd-2fga): under lab dispatch the
     * operations made inside a poll are posts applied, in order, when the
     * poll returns. counted: it counts against its region's
     * max_obligations; checked: reserved through the checked path, whose
     * admission and resolutions count at the call; posts: posts still to
     * apply that name it. */
    uint8_t counted;
    uint8_t checked;
    uint8_t posts;
    /* Rust ObligationRecord::reserved_at and the duration its commit, abort
     * or leak event carries (runtime/state.rs:5597, held_duration_ns =
     * now - reserved_at): the clock when the reserve was applied (a posted
     * reserve is applied at the end of its poll), and how long the
     * obligation had been held when its resolution was applied. held_ns is
     * 0 while it is reserved. */
    asx_time reserved_at;
    asx_time held_ns;
} asx_obligation_slot;

/* Free-list terminator for arena slot links. */
#define ASX_SLOT_NONE 0xFFFFFFFFu

/* -------------------------------------------------------------------
 * Kernel (K18): a runtime's arenas and the lifecycle state that belongs to
 * it and no other. The block is one runtime-owned storage block: the
 * library's static block, or the caller's asx_runtime_config.storage while
 * a runtime that was given one is live.
 *
 * *_count is the high-water mark: every slot index below it has been used
 * at least once, so arena scans iterate [0, *_count) and skip !alive
 * slots. *_live counts currently-allocated slots.
 * ------------------------------------------------------------------- */

/* One runtime's trace: the ring of its most recent events, the rolling
 * digest over every event emitted, the counters and the observer (trace.c
 * owns the semantics). A zeroed trace is a reset one: `started` is set by
 * the first record or reset, which seeds the digest. */
#define ASX_TRACE_KIND_SLOTS 0x61u

typedef struct {
    asx_trace_event ring[ASX_TRACE_CAPACITY];
    uint64_t emitted;
    /* [0, folded) are in the digest; [folded, emitted) are unfolded, in the ring */
    uint64_t folded;
    asx_fnv_fold digest;
    uint64_t kind_totals[ASX_TRACE_KIND_SLOTS];
    asx_trace_observer_fn observer;
    void *observer_ctx;
    int observing; /* guards against an observer that emits */
    int started;
#if !ASX_LOCKFREE_SINGLE_THREAD
    asx_atomic_u32 lock; /* live builds emit from several threads */
#endif
} asx_trace_state;

/* Rust's obligation mailbox (bd-2fga): under lab dispatch a Cx's
 * obligation operations made inside a poll are posts, applied in order
 * when the poll returns (lab/runtime.rs:4797-4808). */
typedef enum {
    OB_POST_RESERVE = 1,
    OB_POST_COMMIT = 2,
    OB_POST_ABORT = 3,
    OB_POST_LEAK = 4,
    OB_POST_HANDOFF = 5
} ob_post_kind;

typedef struct {
    uint8_t kind;
    uint32_t idx;
    uint16_t generation;
    asx_obligation_info reservation;
    asx_trace_handoff handoff;
} ob_post;

/* A reservation and one resolution per slot. */
#define OB_POST_CAP (2u * (uint32_t)(ASX_MAX_OBLIGATIONS))

/* One runtime's kernel: its arenas, and the state that belongs to it and
 * no other. The block is what asx_runtime_storage_size() measures. */
typedef struct asx_kernel {
    asx_region_slot regions[ASX_MAX_REGIONS];
    asx_task_slot tasks[ASX_MAX_TASKS];
    asx_obligation_slot obligations[ASX_MAX_OBLIGATIONS];
    uint32_t region_count;
    uint32_t region_live;
    uint32_t task_count;
    uint32_t task_live;
    uint32_t obligation_count;
    uint32_t obligation_live;
    /* LIFO free lists of released slots: the head is the index + 1 of the
     * last one released, 0 when empty, so a zeroed kernel (the static block
     * in .bss, a caller's cleared one) is a valid empty one. Reuse order
     * depends only on the release sequence, so replay is stable. */
    uint32_t task_free_head;
    uint32_t obligation_free_head;
    ob_post ob_posts[OB_POST_CAP];
    uint32_t ob_post_n;
    /* Obligation leak policy (runtime config) and cumulative leak count. */
    asx_leak_response leak_response;
    asx_leak_escalation_config leak_escalation;
    int leak_escalation_set;
    uint64_t leak_count;
    /* Opt-in hard cleanup bound (runtime config); off is Rust's semantics. */
    int cleanup_hard_bound;
    uint64_t member_seq;
    /* Fault raised during completion bookkeeping (PANIC leak policy) that
     * the scheduler must report, mirroring a failing poll under FAIL_FAST. */
    asx_status pending_fault;
    /* Rust's finalizing_regions (state.rs:2101): the regions in FINALIZING,
     * as region slots, in the order they entered it; and the finalizer
     * budget and escalation of asx_runtime_config, which are Rust's
     * defaults until finalizer_configured is set. */
    uint32_t finalizing[ASX_MAX_REGIONS];
    uint32_t finalizing_count;
    int finalizer_configured;
    uint32_t finalizer_poll_budget;
    uint64_t finalizer_time_budget_ns;
    asx_finalizer_escalation finalizer_escalation;
    /* The default clock hooks' virtual time and the default entropy hook's
     * stream: this runtime's own, so one runtime's sleeps and draws do not
     * move another's. entropy_state is the LCG state xor the default seed
     * (hooks.c), so a zeroed kernel draws from the seed. */
    asx_time virtual_now;
    uint64_t entropy_state;
    /* The scheduler's timer wheel and its arena: a task's wake timer and
     * its lab budget deadline, two per task slot, tokened by slot index of
     * this kernel. Set up by asx_runtime_clear_internal (the wheel's arena
     * pointers point into this block); zero until then. */
    asx_timer_wheel wheel;
    tw_entry wheel_entries[TW_RUNTIME_CAPACITY];
    uint16_t wheel_overflow[TW_RUNTIME_CAPACITY];
    /* The application timer wheel (asx_timer_wheel_global) and its arena:
     * set up on first use (app_ready), reset by a clear. */
    asx_timer_wheel app_wheel;
    tw_entry app_entries[TW_APP_CAPACITY];
    uint16_t app_overflow[TW_APP_CAPACITY];
    int app_ready;
    /* The wake registrations (waker.c): a waker names this kernel by the
     * tag in its slot. */
    asx_waker_state wakers;
    /* The wait-node pool (sync/wait_queue.c): a queue names this kernel's
     * pool by tag, a node id carries it. */
    asx_wait_pool waits;
    /* The sync primitives' arenas (sync/sync_state.h), each moved in as its
     * primitive became per runtime. */
    asx_sem_state sems;
    /* The runtime's trace: ring, digest, counters, observer. */
    asx_trace_state trace;
    uint32_t rt_generation; /* nonzero while an initialized runtime holds it */
    uint16_t tag;           /* its index in g_kernels; stamped on its handles */
    uint8_t arenas_clear;   /* the arenas were cleared in full once: ASX_ARENA_CLEAR_N */
} asx_kernel;

/* The library's static block: the kernel of tag 0 when no caller block is. */
extern asx_kernel g_kernel_static;

/* The registry, by runtime tag: the kernel a handle's tag names, or NULL
 * for a tag no runtime holds. Slot 0 is never NULL: the static block when
 * no runtime holds it. */
extern asx_kernel *g_kernels[ASX_MAX_RUNTIMES];

static inline asx_kernel *asx_kernel_for_tag(uint16_t tag) {
    return tag < ASX_MAX_RUNTIMES ? g_kernels[tag] : NULL;
}

/* The kernel that issued `handle`, or NULL when no runtime holds the tag it
 * carries: every slot lookup answers ASX_E_NOT_FOUND for such a handle. */
static inline asx_kernel *asx_kernel_of(uint64_t handle) {
    return asx_kernel_for_tag(asx_handle_runtime_tag(handle));
}

/* Which runtime object holds which kernel. An object holds the kernel of
 * its tag while that kernel records the object's generation. */
static inline int asx_kernel_holds(uint16_t tag, uint32_t generation) {
    const asx_kernel *k = asx_kernel_for_tag(tag);
    return k != NULL && k->rt_generation == generation;
}

/* Whether a kernel other than `except` (NULL: any kernel) is held by an
 * initialized runtime. A clear or a shutdown with no other kernel held is
 * the last runtime's, and only that one also resets the process-wide
 * subsystems no kernel owns yet. */
static inline int asx_kernel_other_held(const asx_kernel *except) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_RUNTIMES; i++) {
        if (g_kernels[i] != NULL && g_kernels[i] != except && g_kernels[i]->rt_generation != 0u) {
            return 1;
        }
    }
    return 0;
}

/* Whether any kernel is held by an initialized runtime. */
static inline int asx_kernel_any_held(void) { return asx_kernel_other_held(NULL); }

/* `rt` (its generation set) takes the kernel registered at `tag`. */
static inline void asx_kernel_hold(asx_runtime *rt, uint16_t tag) {
    rt->tag = tag;
    g_kernels[tag]->rt_generation = rt->generation;
}

/* asx_handle_pack for a handle kernel `k` issues: its runtime tag rides in
 * the handle, so no other runtime's lookup resolves it. */
static inline uint64_t asx_handle_mint(const asx_kernel *k, uint16_t type_tag, uint16_t state_mask,
                                       uint32_t index) {
    return asx_handle_with_runtime_tag(asx_handle_pack(type_tag, state_mask, index), k->tag);
}

/* Bind kernel 0 (g_kernels[0]) to `storage` (an
 * asx_runtime_storage_size() block aligned to asx_runtime_storage_align(),
 * already validated), zeroing it; NULL binds the library's static block and
 * leaves it as it is. Counters and the rest of the kernel state are cleared
 * by asx_runtime_clear_internal. */
void asx_kernel_bind_internal(void *storage);

/* Register the caller's `storage` block (already validated, not the
 * library's) as the kernel of `tag` (1 .. ASX_MAX_RUNTIMES - 1), zeroing it
 * and stamping the tag. Counters and the rest of the kernel state are
 * cleared by asx_runtime_clear_internal. */
void asx_kernel_bind_slot_internal(uint16_t tag, void *storage);

/* End the runtime of `tag` (asx_runtime_reset, shutdown, a failed init): tag 0
 * returns to the static block, cleared; another slot is cleared, unregistered. */
void asx_kernel_release_internal(uint16_t tag);

/* Second runtimes (K18): whether the still process-wide subsystem holds
 * state a second runtime would share. Each is defined with its subsystem;
 * rt.c evaluates them at a second initialization and names the first that
 * answers nonzero. */
int asx_rwlock_in_use_internal(void);
int asx_barrier_in_use_internal(void);
int asx_once_in_use_internal(void);
int asx_notify_in_use_internal(void);
int asx_channel_in_use_internal(void);
int asx_oneshot_in_use_internal(void);
int asx_watch_in_use_internal(void);
int asx_broadcast_in_use_internal(void);
int asx_session_in_use_internal(void);
int asx_actor_in_use_internal(void);
int asx_supervisor_in_use_internal(void);

/* Whether `hooks` is the hook table the live runtimes installed (the
 * allocator seal aside): the table is process-wide. Defined in hooks.c. */
int asx_runtime_hooks_match_installed_internal(const asx_runtime_hooks *hooks);

/* The reset of asx_runtime_reset without the rebind to the static block:
 * what init runs after it bound the runtime's storage (lifecycle.c). */
void asx_runtime_clear_internal(asx_kernel *k);

/* Whether `storage` (NULL: the library's block) can be asx_runtime_config.storage
 * of size `size`: ASX_OK, or ASX_E_INVALID_ARGUMENT for a size without a
 * block, a block smaller than asx_runtime_storage_size(), a misaligned one,
 * or one overlapping the library's static block. Pure. */
asx_status asx_kernel_storage_validate_internal(const void *storage, size_t size);

/* -------------------------------------------------------------------
 * Shared lookup functions (generation-safe, used across TUs)
 * ------------------------------------------------------------------- */

ASX_MUST_USE asx_status asx_region_slot_lookup(asx_region_id id, asx_region_slot **out);
ASX_MUST_USE asx_status asx_task_slot_lookup(asx_task_id id, asx_task_slot **out);
ASX_MUST_USE asx_status asx_obligation_slot_lookup(asx_obligation_id id, asx_obligation_slot **out);

/* Release captured state for a task exactly once. */
void asx_task_release_capture_internal(asx_task_slot *task);

/* Build the public handle for an occupied task slot. */
asx_task_id asx_task_handle_for_slot(const asx_kernel *k, uint32_t slot_idx);

/* The checked task-state write (bd-9kll.3.22): every transition the
 * runtime makes goes through it. It checks the task transition table
 * (asx_task_transition_check) in every build, not only under the ghost
 * monitor. An illegal move -- or a state word outside the enum -- changes
 * nothing: the state stays as it was, the ghost protocol monitor records
 * the violation, and ASX_E_INVALID_TRANSITION (ASX_E_INVALID_ARGUMENT for
 * an out-of-range state) is returned for the caller to stop on and report.
 * Rust's TaskRecord transitions refuse the same way, matching the current
 * state and returning false/None without writing (start_running, complete,
 * acknowledge_cancel, cleanup_done, finalize_done_with_witness;
 * record/task.rs:1078-1460). The caller emits the transition trace. */
ASX_MUST_USE asx_status asx_task_set_state_internal(asx_task_slot *t, asx_task_id id,
                                                    asx_task_state to);

/* Completion bookkeeping shared by every path that drives a task to
 * COMPLETED: disarms its timer, wakes join waiters, releases captured
 * state, decrements the region's live-task count, and frees the slot if
 * the task was detached. The caller must already have set
 * state/outcome. */
void asx_task_on_complete_internal(asx_task_slot *task, asx_region_slot *region);

/* Apply the obligation posts the poll that just returned made (Rust's
 * obligation-mailbox drain at the end of a lab poll, bd-2fga). */
void asx_obligation_drain_posts_internal(asx_kernel *k);

/* Reset wake-driven scheduler state: kernel `k`'s timer wheel (set up on its
 * first reset) and, with no other kernel held, the current task. Called
 * from the clear. Defined in scheduler.c. */
void asx_scheduler_reset_internal(asx_kernel *k);

/* Reset kernel `k`'s application timer wheel when it was set up (a clear).
 * Defined in timer_wheel.c. */
void asx_timer_wheel_reset_app_internal(asx_kernel *k);

/* Release every node of kernel `k`'s wait pool and empty every queue that
 * names it (a clear). Defined in sync/wait_queue.c. */
void asx_wait_pool_reset_internal(asx_kernel *k);

/* Invalidate every semaphore (and mutex) of kernel `k` and empty its arena
 * (a clear). Defined in sync/semaphore.c. */
void asx_semaphore_reset_internal(asx_kernel *k);

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
 * its joiners, and the region is re-advanced. If its current state cannot
 * transition to Completed, nothing changes and the refusal is returned. */
ASX_MUST_USE asx_status asx_task_refuse_admission_internal(asx_task_slot *t, asx_region_slot *r,
                                                           asx_status why);

/* The earliest armed task timer of kernel `k` (task wake timers and, under
 * lab dispatch, budget-deadline timers): 1 and *out_next, or 0 when none is
 * armed. Defined in scheduler.c. */
int asx_scheduler_next_timer_internal(asx_kernel *k, asx_time *out_next);

/* Fire the task timers of kernel `k` due now (waking their tasks); the
 * number woken. Defined in scheduler.c. */
uint32_t asx_scheduler_fire_due_timers_internal(asx_kernel *k);

/* Run `t`'s poll function as the current task (asx_task_current() names
 * `tid` inside), as the sweep does, for a run loop of its own
 * (asx_parallel_run). Returns the poll function's status. Defined in
 * scheduler.c. */
asx_status asx_task_poll_current_internal(asx_task_slot *t, asx_task_id tid);

/* The virtual clock of kernel `k`: read, advance (no-op unless `to` is in
 * the future) and set (backwards too, a lab snapshot restore). Defined in
 * hooks.c. asx_runtime_virtual_now/advance of the public API take a runtime. */
asx_time asx_kernel_virtual_now(const asx_kernel *k);
void asx_kernel_virtual_advance(asx_kernel *k, asx_time to);
void asx_kernel_virtual_set_internal(asx_kernel *k, asx_time to);

/* asx_runtime_now_ns and asx_runtime_random_u64 as runtime `k` sees them:
 * the default clock and entropy hooks answer from k's own virtual time and
 * stream, any other installed hook as it is. Defined in hooks.c. */
asx_status asx_kernel_now_ns(const asx_kernel *k, asx_time *out_now);
asx_status asx_kernel_random_u64(asx_kernel *k, uint64_t *out_value);

/* The reading scheduling and stamping take: asx_kernel_now_ns, or k's virtual
 * time when no clock hook answers. */
asx_time asx_kernel_now_or_virtual_internal(const asx_kernel *k);

/* The same clock read without asx_kernel_now_ns's side effects (fault
 * injection call counts, the hindsight record): bookkeeping that must shift
 * neither, an obligation's held time. */
asx_time asx_kernel_clock_peek_internal(const asx_kernel *k);

/* Restart k's virtual clock and entropy stream, as a runtime's reset does. */
void asx_kernel_clock_reset_internal(asx_kernel *k);

/* Finalize a closing region whose tasks have all completed, then its
 * closing ancestors in turn (Rust advance_region_state). No-op for an Open
 * or Closed region. Defined in quiescence.c. */
void asx_region_advance_internal(asx_region_id id);

/* The trace of kernel `k` (trace.c). The public handle-less trace API
 * (asx_trace_emit, asx_trace_digest, the getters, the observer, reset) is
 * kernel 0's. Emit an event into k's ring (NULL: kernel 0, for an entity
 * whose handle no runtime holds); the payload variant's observer call
 * carries `payload`. */
void asx_trace_emit_internal(asx_kernel *k, asx_trace_event_kind kind, uint64_t entity_id,
                             uint64_t aux);
void asx_trace_emit_payload_internal(asx_kernel *k, asx_trace_event_kind kind, uint64_t entity_id,
                                     uint64_t aux, const asx_trace_payload *payload);
uint32_t asx_trace_event_count_internal(const asx_kernel *k);
uint64_t asx_trace_emitted_total_internal(const asx_kernel *k);
uint64_t asx_trace_dropped_internal(const asx_kernel *k);
uint64_t asx_trace_kind_total_internal(const asx_kernel *k, asx_trace_event_kind kind);
int asx_trace_event_get_internal(const asx_kernel *k, uint32_t index, asx_trace_event *out);
int asx_trace_event_get_seq_internal(const asx_kernel *k, uint64_t seq, asx_trace_event *out);
uint64_t asx_trace_digest_internal(asx_kernel *k);
/* Restart k's trace: counters, digest and ring cursor (the observer stays). */
void asx_trace_reset_internal(asx_kernel *k);

/* The time a cancel reason is stamped with (runtime clock, else virtual). */
asx_time asx_cancel_now_internal(const asx_kernel *k);

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
 * reconcile_checkpoint_cancel); likewise a Cx reason ahead of the record
 * (cx_reason_ahead): the record's reason strengthened by it, the cleanup
 * budgets met. No-op otherwise. Fails only with the refusal of the task
 * transition (asx_task_set_state_internal), the cancel left unmaterialized. */
ASX_MUST_USE asx_status asx_task_materialize_cancel_internal(asx_task_slot *t);

/* Only the second half: a Cx reason ahead of the record strengthens it;
 * the Cx keeps its own reason when a tie leaves the record's. */
void asx_task_reconcile_cx_reason_internal(asx_task_slot *t);

/* At completion: reconcile, then the record takes the Cx's own reason, as
 * Rust's outcome takes the Cx's reason sampled at the terminal poll
 * (classify_spawn_completion, runtime/task_handle.rs:173-200). */
void asx_task_complete_cx_reason_internal(asx_task_slot *t);

/* The task's cancel reason as its Cx sees it (Rust Cx::cancel_reason):
 * the record's, or the stronger budget reason not yet reconciled into it.
 * ASX_E_NOT_FOUND when no cancel is pending. asx_task_get_cancel_reason
 * reads the record. */
asx_status asx_task_get_cx_cancel_reason_internal(asx_task_id id, asx_cancel_reason *out);

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
asx_region_id asx_region_handle_for_slot(const asx_kernel *k, uint32_t slot_idx);

/* Active obligation-leak policy (set by asx_runtime_init / reload; PANIC
 * after a bare asx_runtime_reset). */
void asx_runtime_set_leak_policy_internal(asx_kernel *k, asx_leak_response response,
                                          const asx_leak_escalation_config *escalation);

/* The next region-membership sequence number (asx_task_slot.member_seq). */
uint64_t asx_task_next_member_seq_internal(asx_kernel *k);

/* Opt-in hard cleanup bound (asx_runtime_config.cleanup_hard_bound; off
 * after a bare asx_runtime_reset). */
void asx_runtime_set_cleanup_hard_bound_internal(asx_kernel *k, int on);
int asx_cleanup_hard_bound_internal(const asx_kernel *k);

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
uint64_t asx_lab_step_draw_internal(void); /* the draw, then check_futurelocks (LR:4507-4508) */
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
/* While a recorded schedule is replayed (asx_lab_run_forced_schedule), the step's pick comes
 * from the schedule instead: *out_picked is 1 with the task and lane, or 0 for a step with
 * nothing to dispatch. A divergence is ASX_E_REPLAY_MISMATCH with the task left unpolled. */
int asx_lab_forced_active(void);
asx_status asx_lab_forced_pick(uint64_t step, asx_time now, uint32_t *out_slot,
                               uint16_t *out_task_gen, int *out_cancel_lane, int *out_picked);
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
/* Drop a refused actor's captured mailbox/replies before its handle is
 * resolved and its waiter wakes. Consumes only the matching live ticket. */
void asx_actor_refusal_delivered_internal(uint32_t ticket);
int asx_lab_admissions_pending(void);
/* Admits the queued spawns. Fails only when a refused admission's task
 * transition is refused (asx_task_set_state_internal): that error, with
 * the admissions after it left unadmitted. */
ASX_MUST_USE asx_status asx_lab_admit_pending(void);
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
/* Rust's Cx::random_u64 (cx.rs:3585-3589) for task `self`: the task's
 * stream under lab dispatch, else the runtime entropy hook. 1 and *out, or
 * 0 when `self` is no live task (no Cx) or no entropy is available. */
int asx_task_random_u64_internal(asx_task_id self, uint64_t *out);
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
/* The same open when it carries an envelope and requirements
 * (ChildRegionSpec::capability_budget/requirements, child_region.rs:55):
 * the request's `envelope` is the caller's met with the spec's. Planned
 * when applied, against the parent's, so a refusal reaches the opener after
 * it has parked (region_wait_status ASX_E_CAPABILITY_BUDGET_REFUSED,
 * region_wait_refusal). */
asx_status asx_lab_region_open_capability_command(asx_task_slot *opener, asx_region_id parent,
                                                  const asx_budget *budget,
                                                  const asx_capability_budget *envelope,
                                                  const asx_capability_requirements *requirements);
asx_status asx_lab_region_cancel_command(asx_region_id region, const asx_cancel_reason *reason);

/* A queued region command's payload: a Cancel's reason, or the envelope and
 * requirements of an open that carries one. */
typedef union {
    asx_cancel_reason reason;
    struct {
        asx_capability_budget envelope;
        asx_capability_requirements requirements;
    } open;
} asx_lab_region_payload;

static inline uint8_t asx_capability_refusal_pack_internal(const asx_capability_refusal *r) {
    return (uint8_t)(((unsigned)r->kind << 4) | ((unsigned)r->dimension & 0x0Fu));
}

static inline asx_capability_refusal asx_capability_refusal_unpack_internal(uint8_t packed) {
    asx_capability_refusal r;
    r.kind = (asx_capability_refusal_kind)(packed >> 4);
    r.dimension = (asx_capability_dimension)(packed & 0x0Fu);
    return r;
}
/* Rust RegionCommand::Watch (monitor.rs WatchCommand): a monitor request
 * for `holder` (the opener's own task, or the GenServer whose slot and
 * generation are given when `to_server`) on `target`, or the removal of the
 * monitor `ref`. Applied in order with the other region commands; a request's
 * result (status, reference) wakes its opener after the batch. Queueing
 * fails with ASX_E_RESOURCE_EXHAUSTED when the queue is full. */
asx_status asx_lab_watch_monitor_command(asx_task_slot *opener, asx_task_id target, int to_server,
                                         uint32_t server_slot, uint32_t server_gen);
asx_status asx_lab_watch_demonitor_command(uint64_t ref);
/* The effect of an applied Watch command (monitor.c): register a monitor
 * for `watcher`, or for the server (slot, generation) when `to_server`, on
 * `target`; or remove the monitor `ref`. The statuses are
 * asx_monitor_establish's. */
asx_status asx_monitor_apply_internal(uint64_t watcher, uint64_t target, int to_server,
                                      uint32_t server_slot, uint32_t server_gen, uint64_t *out_ref);
void asx_monitor_remove_internal(uint64_t ref);
/* The same for links (link.c): `policy` is an asx_exit_policy. A link
 * request is a Watch command of the lane, as a monitor request is; its
 * unlink is the removal. */
asx_status asx_link_apply_internal(uint64_t holder, uint64_t peer, int to_server,
                                   uint32_t server_slot, uint32_t server_gen, uint32_t policy,
                                   uint64_t *out_ref);
void asx_link_remove_internal(uint64_t ref);
asx_status asx_lab_watch_link_command(asx_task_slot *opener, asx_task_id peer, int to_server,
                                      uint32_t server_slot, uint32_t server_gen, uint32_t policy);
asx_status asx_lab_watch_unlink_command(uint64_t ref);
/* RegionCommand::RegisterFinalizer for `region`, the opener's own: queued in
 * the region-command FIFO; the opener's region_wait goes WATCH -> WATCHED
 * with the registration's status when the drain applies it
 * (ChildRegion::retain_until_finalized). */
asx_status asx_lab_retain_command(asx_task_slot *opener, asx_region_id region);
/* Rust's TaskId ordering key for a task (arena index, then generation) under
 * lab dispatch, its slot and generation otherwise: the order Rust sorts a
 * batch of actions on tasks by. */
uint64_t asx_lab_task_order_key_internal(asx_task_id task);
/* A task completes: its monitors fire, then its links (Rust's
 * TaskWatches::on_task_completed). */
void asx_watches_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt);
int asx_lab_region_commands_pending(void);
void asx_lab_drain_region_commands(void);
/* drain_deferred_cancel_dispatches (LR:4219-4239): publish the cancel lane
 * entries, then the cancel wakes, a region command's cancel deferred. */
int asx_lab_deferred_cancels_pending(void);
void asx_lab_drain_deferred_cancels(void);
/* Join-handle aborts (Rust JoinHandle::abort_with_reason): queued, applied at
 * the start of the next step after admissions and around the region commands
 * (at most 16 per drain, coalesced per task); a target awaiting admission takes
 * its aborts at admission, onto the cancel lane only. Queueing fails with
 * ASX_E_RESOURCE_EXHAUSTED when full, changing nothing. */
asx_status asx_lab_handle_cancel_command(const asx_task_slot *t, const asx_cancel_reason *reason);
int asx_lab_handle_cancels_pending(void);
void asx_lab_drain_handle_cancels(void);

/* asx_task_slot.region_wait */
enum {
    ASX_REGION_WAIT_NONE = 0,
    ASX_REGION_WAIT_OPEN,   /* open queued, not applied yet */
    ASX_REGION_WAIT_OPENED, /* open applied: region_wait_status/_region */
    ASX_REGION_WAIT_CLOSE,  /* close requested: waiting for CLOSED */
    ASX_REGION_WAIT_WATCH,  /* monitor request queued, not applied yet */
    ASX_REGION_WAIT_WATCHED /* applied: region_wait_status, the ref in _region */
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
asx_cancel_reason asx_region_close_reason_internal(const asx_kernel *k);
/* A region command reaching a region that closed and whose slot was
 * reused: Rust's cancel_request still records region.cancelled for the
 * region, whose record is gone (state.rs:7756-7758), and does nothing
 * else. */
void asx_region_trace_cancel_of_gone_internal(asx_region_id region,
                                              const asx_cancel_reason *reason);

/* Resolve the obligations a completing task still holds, cancelled or
 * not. A permit's obligation (one with a drop function) is dropped with
 * the task's body, as Rust drops the body's locals: its resource goes back
 * and the obligation is committed or aborted with Cancel, as that permit's
 * Drop does. The others are leaks per the active policy (RECOVER aborts
 * them with ASX_OBLIGATION_ABORT_ERROR). Returns the number of leaks
 * recorded. Sets *out_fail_fast when the policy demands fail-fast
 * containment. */
uint32_t asx_task_resolve_held_obligations_internal(asx_task_slot *task, int *out_fail_fast);

/* Attach the drop of the permit an obligation tracks: called with (a, b)
 * when the obligation's holder completes with it still reserved, it gives
 * the permit's resource back (channel capacity, semaphore permits) and
 * returns 1 when Rust's drop of that permit commits the obligation
 * (SemaphorePermit), 0 when it aborts it with Cancel (SendPermit). A
 * resolved or unknown obligation is left alone. */
void asx_obligation_set_drop_internal(asx_obligation_id id, asx_obligation_drop_fn fn, uint64_t a,
                                      uint64_t b, uint32_t serial);

/* A genuine owned permit may move while retaining its original holder's
 * liability (Rust ObligationToken). These private boundaries authenticate
 * its primitive-specific resource binding; a raw obligation ID is not
 * sufficient. They do not transfer holder or region ownership. An id with
 * no obligation behind it (a reservation the post drain refused) checks
 * ASX_OK: the permit is untracked and nothing is resolved. */
ASX_MUST_USE asx_status asx_obligation_check_token_internal(asx_obligation_id id,
                                                            asx_obligation_drop_fn fn, uint64_t a,
                                                            uint64_t b, uint32_t serial);
ASX_MUST_USE asx_status asx_obligation_resolve_token_internal(asx_obligation_id id,
                                                              asx_obligation_drop_fn fn, uint64_t a,
                                                              uint64_t b, uint32_t serial,
                                                              asx_obligation_state to,
                                                              asx_obligation_abort_reason reason);

/* Interpreter task-body destruction uses the same authenticated physical
 * permit release as explicit abort, with Rust's Drop reason Cancel. */
struct asx_send_permit;
void asx_send_permit_drop_internal(struct asx_send_permit *permit);

/* Poll a Rust Sleep for `deadline` on task `self`'s traced sleep timer
 * without parking it (a wait with a deadline, such as the mutex's
 * lock_until, parks on what it waits for). Returns 1 once the clock has
 * reached `deadline`: a timer an earlier poll registered records its fire
 * (Sleep::complete_ready_registration). Otherwise registers the timer once
 * (timer.scheduled), arms the task's wake at `deadline` and returns 0.
 * Outside a scheduler poll nothing is registered. Dropping the wait early
 * is asx_task_cancel_timer (timer.cancelled). */
int asx_task_poll_deadline_sleep_internal(asx_task_id self, asx_time deadline);

/* Whether a pending cancel makes a completing task's outcome CANCELLED
 * (Rust classify_spawn_completion, task_handle.rs:173-202). A task spawned
 * from inside another task's poll (Rust cx.spawn,
 * PreserveAcknowledgedCancellationResult) keeps the value it returned when
 * its cancel came after its first poll and it acknowledged it (a checkpoint
 * moved it to CANCELLING). Every other task (Rust create_task, a task
 * group's member) is cancellation-dominant (DSL v2 §2). Both schedulers
 * decide outcomes with it (scheduler.c, parallel.c). */
int asx_task_cancel_dominates_internal(const asx_task_slot *t);

/* Take (and clear) a fault raised during completion bookkeeping, e.g. an
 * obligation leak under the PANIC policy. ASX_OK when none is pending. */
asx_status asx_runtime_take_pending_fault_internal(asx_kernel *k);

/* Leak-audit a finalizing region whose tasks are gone: every obligation of
 * it still Reserved is leaked as one batch under the leak policy (Rust's
 * Finalizing-arm audit). ASX_OK, or ASX_E_UNRESOLVED_OBLIGATIONS when the
 * PANIC policy routed the leaks through the region's containment. */
asx_status asx_region_audit_leaks_internal(asx_region_id region);

/* Scheduler block handshake with cross-thread wakers (waker.c), on kernel
 * `k`'s arena. Call prepare right before a blocking reactor wait: it
 * returns 0 (do not block) when a waker is already signaled, else marks
 * the scheduler as blocking so later wakes call
 * asx_runtime_reactor_notify(). Call finish after the wait returns. The
 * drain is asx_waker_drain_signaled for a kernel at hand; the reset
 * invalidates every waker of the kernel (a clear). */
int asx_waker_prepare_block_internal(asx_kernel *k);
void asx_waker_finish_block_internal(asx_kernel *k);
uint32_t asx_waker_drain_signaled_internal(asx_kernel *k, asx_task_id *out_tasks,
                                           uint32_t max_tasks);
void asx_waker_reset_internal(asx_kernel *k);

/* Reset private hook installation state during runtime teardown. */
void asx_runtime_hooks_reset_internal(void);
/* The native boundary journal names the wrappers it put over the clock hooks
 * and the hooks they forward to, so asx_runtime_clock_is_virtual() sees a
 * default clock under a wrapper (journal.c, asx_journal_wrap_hooks). */
void asx_runtime_note_clock_wrappers_internal(asx_clock_now_ns_fn wrapper_now,
                                              asx_clock_now_ns_fn wrapped_now,
                                              asx_clock_now_ns_fn wrapper_logical,
                                              asx_clock_now_ns_fn wrapped_logical);
/* Invalidate the bootstrap token of the runtime that holds `k` during
 * explicit raw reset; the process-wide io backend choice goes with the last
 * held kernel. */
void asx_runtime_object_reset_internal(asx_kernel *k);

/* ------------------------------------------------------------------ */
/* Constants pinned to Rust's (asupersync 5e60b1c4c). The values are     */
/* private on both sides; `asx-conformance constants` compares them with  */
/* schemas/rust_kernel_constants.json, which twin_run reads from the      */
/* Rust source (bd-9kll.2.13).                                            */
/* ------------------------------------------------------------------ */

/* Lab dispatch (lab_dispatch.c, scheduler.c). The scratch capacity of a
 * tie group (PR:425-446), the cancel-lane streak before a ready pick
 * (DEFAULT_LAB_CANCEL_STREAK_LIMIT, LR:6051), the region commands a step
 * applies (REGION_COMMAND_BATCH, LR:4136), the handle cancels a drain
 * applies (HANDLE_CANCEL_BATCH, LR:4019), and the timer wheel's first-level
 * tick (LEVEL0_RESOLUTION_NS, time/wheel.rs:49). */
#define LAB_GROUP_CAP 256u
#define LAB_CANCEL_STREAK_LIMIT 16u
#define LAB_REGION_BATCH 8u
#define LAB_HANDLE_BATCH 16u
#define LAB_WHEEL_TICK_NS ((asx_time)1000000u)

/* The writer hand-offs in a row an rwlock grants while a reader waits
 * (MAX_CONSECUTIVE_WRITERS_BEFORE_READER_BATCH, sync/rwlock.rs:99). */
#define RW_MAX_WRITER_STREAK 16u

/* Messages a GenServer serves (or drains) between two yields
 * (YIELD_INTERVAL, gen_server.rs:2100). */
#define ACTOR_YIELD_INTERVAL 8u

/* Drop broadcast credits even when their ledger registration was refused. */
void asx_broadcast_task_finished_internal(asx_task_id task);

/* Write COMPLETED for a task whose poll ended (cancellation.c): a task that
 * acknowledged a cancel first goes Cancelling -> Finalizing, as Rust's lab
 * does; *out_from is the state it completes from. */
ASX_MUST_USE asx_status asx_task_complete_state_internal(asx_task_slot *t, asx_task_id tid,
                                                         asx_task_state *out_from);

/* Async finalizers (quiescence.c). A region in FINALIZING runs the async
 * entries of its cleanup stack one at a time, each as a masked cleanup task:
 * Rust's schedule_async_finalizers, which the lab runs in every step after
 * its timers and before its pick (lab/runtime.rs:4509-4513). Spawns the next
 * finalizer of every FINALIZING region that has none running, in the order
 * the regions entered FINALIZING; returns how many it spawned. */
uint32_t asx_region_admit_finalizers_internal(asx_kernel *k);
/* Whether any FINALIZING region still has an async finalizer to run or
 * running: the run is not quiescent while there is one. */
int asx_region_finalizers_pending_internal(const asx_kernel *k);
/* A region enters FINALIZING (finalizing_regions.push) or leaves it for
 * CLOSED (swap_remove), by id. */
void asx_region_finalizing_enter_internal(asx_region_id id);
void asx_region_finalizing_leave_internal(asx_region_id id);
/* Whether the region has an async finalizer running or still to run. */
int asx_region_finalizer_outstanding_internal(const asx_region_slot *r);
/* A task of the region completed: if it was the region's finalizer task,
 * the barrier lifts. */
void asx_region_finalizer_task_done_internal(asx_region_slot *r, const asx_task_slot *task);
/* The finalizer budget and escalation (asx_runtime_config); set by init and
 * reload, back to the defaults by asx_runtime_reset. */
void asx_region_finalizer_config_set_internal(asx_kernel *k, uint32_t poll_budget,
                                              uint64_t time_budget_ns,
                                              asx_finalizer_escalation escalation);
void asx_region_finalizers_reset_internal(asx_kernel *k);
/* Spawn a region's finalizer as its cleanup task (lifecycle.c): into a
 * FINALIZING region, with `budget`, masked, at the budget's priority. */
ASX_MUST_USE asx_status asx_task_spawn_finalizer_internal(asx_region_id region,
                                                          asx_task_poll_fn poll_fn, void *user_data,
                                                          const asx_budget *budget,
                                                          asx_task_id *out_id);
/* Nonzero while a journal replay is active and has failed (journal_replay.c):
 * the run it drives cannot go on faithfully, and the scheduler ends it at its
 * next round with ASX_E_REPLAY_MISMATCH. 0 with no replay active. */
int asx_replay_stop_run_internal(void);

/* One DOWN delivery of the monitor module (src/monitor/monitor.c): the
 * monitor that fired, the monitored task's handle as the completion gave it
 * and the completion time. The module keeps the last ASX_MONITOR_CAPACITY
 * in firing order for the down_order oracle (oracle.c, bd-9kll.9.2). */
typedef struct {
    uint64_t ref;
    uint64_t monitored;
    uint64_t vt;
} asx_monitor_delivery;

/* How many deliveries are retained (at most ASX_MONITOR_CAPACITY). */
uint32_t asx_monitor_delivery_count_internal(void);
/* Retained delivery `index`, 0 the oldest: 1 and *out filled, or 0 past the
 * end. */
int asx_monitor_delivery_get_internal(uint32_t index, asx_monitor_delivery *out);
/* The way delivery `next`, which fired right after `prev`, breaks the
 * order, as a static message, or NULL (oracle.c; pure). */
const char *asx_monitor_delivery_defect_internal(const asx_monitor_delivery *prev,
                                                 const asx_monitor_delivery *next);

/* What one live channel's counters say (src/channel/mpsc.c), read-only, so
 * the channel_atomicity oracle (oracle.c, bd-9kll.9.2) is a pure function of
 * a value a test can make. `queued` is the committed messages whichever
 * backend holds them, `permit_tokens` the permit table's entries in use,
 * `sender_tokens` the live clone identities; `in_use` is the lock-free
 * backend's committed-plus-reserved counter (has_in_use 0 elsewhere). */
typedef struct {
    asx_region_id region;
    uint32_t capacity;
    uint32_t queued;
    uint32_t reserved;
    uint32_t permit_tokens;
    uint32_t sender_count;
    uint32_t sender_tokens;
    uint32_t primary_dropped;
    uint32_t in_use;
    uint32_t has_in_use;
} asx_channel_accounting;

/* One past the highest channel slot created since the last reset. */
uint32_t asx_channel_slot_count_internal(void);
/* The accounting of live channel slot `index`: 1 and *out filled, or 0 for a
 * slot that is not alive or past the high-water mark. */
int asx_channel_accounting_get_internal(uint32_t index, asx_channel_accounting *out);
/* The first way `a` is inconsistent as a static message, or NULL when its
 * counters agree (oracle.c). */
const char *asx_channel_accounting_defect_internal(const asx_channel_accounting *a);

/* How many slots of an arena asx_runtime_clear_internal visits. Once the
 * arenas have been cleared in full (arenas_clear), a slot at or above the
 * arena's count (the high-water mark every scan of it stops at) has not been
 * allocated since, so the used prefix is all a clear needs. A zeroed kernel,
 * the static block or a block a bind zeroed, has the flag 0 and its first
 * clear is the full one. ASX_MUTANT_CLEAR_SKIPS_USED_SLOTS plants the bug of
 * leaving the last used slot as it was (tests/gates/test_oracle_mutants.sh). */
#ifdef ASX_MUTANT_CLEAR_SKIPS_USED_SLOTS
#define ASX_ARENA_CLEAR_N(k, count, max)                                                           \
    ((k)->arenas_clear ? ((count) > 0u ? (count) - 1u : 0u) : (uint32_t)(max))
#else
#define ASX_ARENA_CLEAR_N(k, count, max) ((k)->arenas_clear ? (count) : (uint32_t)(max))
#endif

/* A region's two budgets when it opens (Rust RegionRecord::new_with_time:
 * Budget::INFINITE and CapabilityBudget::UNSPECIFIED), and a child's, which
 * start as its parent's: create_child_with_capability_budget meets the
 * child's own envelope into the parent's (region_table.rs:292-325), and a
 * child that supplies none keeps the parent's. No shutdown ceiling yet. */
static inline void asx_region_budgets_reset_internal(asx_region_slot *r) {
    r->budget = asx_budget_infinite();
    r->capability_budget = asx_capability_budget_unspecified();
    r->has_shutdown_budget = 0;
}

static inline void asx_region_budgets_inherit_internal(asx_region_slot *child,
                                                       const asx_region_slot *parent) {
    child->budget = parent->budget;
    child->capability_budget = parent->capability_budget;
}

/* Whose clock and entropy. The public asx_runtime_now_ns and its kin take the
 * runtime object: asx_kernel_of_runtime is its kernel, NULL for NULL or a tag
 * no runtime holds. The still process-wide subsystems (application timers and
 * deadlines, the network and web surfaces, epochs, streams, supervisors) hold
 * state only while at most one runtime is live (a second init and the
 * acquisition of state while two are live are refused), so their clock is
 * that runtime's: asx_kernel_process_scope is the kernel of the one live
 * runtime, else the library's static kernel (a zeroed runtime object).
 * Code with a task or other handle in hand reads that handle's kernel;
 * asx_kernel_of_or_scope falls back to the process scope for a handle no
 * runtime holds (an invalid id, a host call). Library code that has neither
 * a handle nor a runtime object (a deadline, a throttle, an epoch) reads
 * asx_kernel_context: the kernel of the task being polled
 * (asx_task_current()), else the process scope. */
static inline asx_kernel *asx_kernel_of_runtime(const asx_runtime *rt) {
    return rt != NULL ? asx_kernel_for_tag(rt->tag) : NULL;
}

static inline asx_kernel *asx_kernel_process_scope(void) {
    asx_kernel *only = NULL;
    uint32_t i;
    for (i = 0; i < ASX_MAX_RUNTIMES; i++) {
        if (g_kernels[i] != NULL && g_kernels[i]->rt_generation != 0u) {
            if (only != NULL) return g_kernels[0];
            only = g_kernels[i];
        }
    }
    return only != NULL ? only : g_kernels[0];
}

static inline asx_kernel *asx_kernel_of_or_scope(uint64_t handle) {
    asx_kernel *k = asx_kernel_of(handle);
    return k != NULL ? k : asx_kernel_process_scope();
}

static inline asx_kernel *asx_kernel_context(void) {
    return asx_kernel_of_or_scope(asx_task_current());
}

/* asx_cancel_reason_default for the kernel `k` (origin: its arena slot 0;
 * none for NULL), and for asx_kernel_context() (the runtime of the task being
 * polled, else the live one) where a module has no kernel at hand. Defined
 * in cancellation.c. */
asx_cancel_reason asx_cancel_reason_default_internal(const asx_kernel *k, asx_cancel_kind kind,
                                                     const char *message);
static inline asx_cancel_reason asx_cancel_reason_default_ctx_internal(asx_cancel_kind kind,
                                                                       const char *message) {
    return asx_cancel_reason_default_internal(asx_kernel_context(), kind, message);
}

/* Record an event in the trace of asx_kernel_context(): what the still
 * process-wide subsystems (channels, the application timer wheel, parallel,
 * telemetry) have for a runtime, having no runtime object. */
static inline void asx_trace_emit_ctx_internal(asx_trace_event_kind kind, uint64_t entity_id,
                                               uint64_t aux) {
    asx_trace_emit_internal(asx_kernel_context(), kind, entity_id, aux);
}

/* asx_runtime_random_secure_u64 as runtime `k` sees it. Defined in hooks.c. */
asx_status asx_kernel_random_secure_u64(asx_kernel *k, uint64_t *out_value);

/* A task's wake timer fired in the sweep scheduler: the task is woken, and one whose timer was
 * armed for a deadline that is now due joins the timed lane of the next round. Defined in
 * scheduler.c (bd-9kll.4.6). */
void asx_sweep_timer_woke_internal(asx_kernel *k, asx_task_slot *t, asx_time now);

/* The first request that cancels `t`, outside lab dispatch (cancellation.c):
 * `t` is woken, and the sweep queues it, if its round has not polled it,
 * ahead of the rest of the round's timed lane (Rust puts a request in the
 * cancel lane at once). Defined in scheduler.c. */
void asx_sweep_cancel_woke_internal(asx_kernel *k, asx_task_slot *t);

/* The outcome a completed task's TaskRecord holds, which its monitors, its links and its
 * region's close outcome take; the join's (t->outcome) can differ. Defined in cancellation.c. */
asx_outcome asx_task_record_outcome_internal(const asx_task_slot *t);

#endif /* ASX_RUNTIME_INTERNAL_H */
