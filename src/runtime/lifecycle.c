/*
 * lifecycle.c — region/task/obligation lifecycle engine (walking skeleton)
 *
 * Provides generation-safe handle validation, cleanup-stack primitives,
 * and region/task lifecycle operations.
 *
 * Uses fixed-size static arenas. Hook-backed dynamic allocation
 * requires platform threading hooks (bd-hwb.3, bd-2cw.1).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../sync/wait_queue.h"
#include "runtime_internal.h"
#include <asx/actor/actor.h>
#include <asx/actor/supervisor.h>
#include <asx/asx.h>
#include <asx/core/broadcast.h>
#include <asx/core/ghost.h>
#include <asx/core/oneshot.h>
#include <asx/core/session.h>
#include <asx/core/transition.h>
#include <asx/core/watch.h>
#include <asx/fs/fs.h>
#include <asx/net/net.h>
#include <asx/process/process.h>
#include <asx/runtime/automotive_instrument.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/event.h>
#include <asx/runtime/hft_instrument.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/parallel.h>
#include <asx/runtime/runtime.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/telemetry.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <asx/runtime/vertical_adapter.h>
#include <asx/runtime/waker.h>
#include <asx/signal/signal.h>
#include <asx/sync/barrier.h>
#include <asx/sync/notify.h>
#include <asx/sync/once.h>
#include <asx/sync/rwlock.h>
#include <asx/sync/semaphore.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

/* The arenas and the lifecycle state they carry are the kernel's
 * (runtime_internal.h): fixed-size, no dynamic allocation. */

uint64_t asx_task_next_member_seq_internal(asx_kernel *k) { return ++k->member_seq; }

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

void asx_runtime_clear_internal(asx_kernel *k) {
    uint32_t i, j;
    asx_runtime_object_reset_internal(k);
    for (i = 0; i < ASX_ARENA_CLEAR_N(k, k->region_count, ASX_MAX_REGIONS); i++) {
        k->regions[i].state = ASX_REGION_OPEN;
        k->regions[i].parent_id = ASX_INVALID_ID;
        k->regions[i].child_count = 0;
        for (j = 0; j < ASX_MAX_REGION_CHILDREN; j++) {
            k->regions[i].children[j] = ASX_INVALID_ID;
        }
        k->regions[i].task_count = 0;
        k->regions[i].task_total = 0;
        k->regions[i].generation = 0;
        k->regions[i].alive = 0;
        asx_cleanup_init(&k->regions[i].cleanup);
        asx_region_budgets_reset_internal(&k->regions[i]);
        k->regions[i].capture_used = 0;
    }
    k->region_count = 0;
    k->region_live = 0;
    for (i = 0; i < ASX_ARENA_CLEAR_N(k, k->task_count, ASX_MAX_TASKS); i++) {
        k->tasks[i].state = ASX_TASK_CREATED;
        k->tasks[i].region = ASX_INVALID_ID;
        k->tasks[i].poll_fn = NULL;
        k->tasks[i].user_data = NULL;
        k->tasks[i].outcome = asx_outcome_make(ASX_OUTCOME_OK);
        k->tasks[i].generation = 0;
        k->tasks[i].alive = 0;
        k->tasks[i].captured_state = NULL;
        k->tasks[i].captured_size = 0;
        k->tasks[i].captured_dtor = NULL;
        k->tasks[i].cancel_phase = 0;
        k->tasks[i].cancel_pending = 0;
        k->tasks[i].cancel_epoch = 0;
        k->tasks[i].cleanup_budget = asx_budget_infinite();
        k->tasks[i].cleanup_applied = 0;
        k->tasks[i].cleanup_polls_remaining = 0;
        k->tasks[i].cancel_unmaterialized = 0;
        k->tasks[i].cx_reason_ahead = 0;
        k->tasks[i].detached = 0;
        k->tasks[i].next_free = ASX_SLOT_NONE;
        k->tasks[i].budget = asx_budget_infinite();
        k->tasks[i].first_held = ASX_SLOT_NONE;
        asx_task_sched_init_internal(&k->tasks[i]);
        memset(&k->tasks[i].cancel_reason, 0, sizeof(k->tasks[i].cancel_reason));
    }
    k->task_count = 0;
    k->task_live = 0;
    k->task_free_head = 0u;
    for (i = 0; i < ASX_ARENA_CLEAR_N(k, k->obligation_count, ASX_MAX_OBLIGATIONS); i++) {
        k->obligations[i].state = ASX_OBLIGATION_RESERVED;
        k->obligations[i].region = ASX_INVALID_ID;
        k->obligations[i].generation = 0;
        k->obligations[i].alive = 0;
        k->obligations[i].next_free = ASX_SLOT_NONE;
        k->obligations[i].kind = ASX_OBLIGATION_KIND_GENERIC;
        k->obligations[i].holder = ASX_INVALID_ID;
        k->obligations[i].next_held = ASX_SLOT_NONE;
        k->obligations[i].abort_reason = ASX_OBLIGATION_ABORT_NONE;
        k->obligations[i].drop_fn = NULL;
        k->obligations[i].drop_a = 0u;
        k->obligations[i].drop_b = 0u;
        k->obligations[i].drop_serial = 0u;
        k->obligations[i].counted = 0;
        k->obligations[i].checked = 0;
        k->obligations[i].posts = 0;
    }
    k->obligation_count = 0;
    k->obligation_live = 0;
    k->obligation_free_head = 0u;
    k->ob_post_n = 0;
    k->leak_response = ASX_LEAK_PANIC;
    k->leak_escalation_set = 0;
    k->leak_count = 0;
    k->cleanup_hard_bound = 0;
    k->member_seq = 0;
    k->arenas_clear = 1u; /* the three arena loops above covered the used prefix or all of it */
    asx_kernel_clock_reset_internal(k);
    asx_scheduler_reset_internal(k);
    asx_error_ledger_reset_runtime_internal(k->tag);
    asx_cancel_reset_internal(k->tag);
    asx_region_finalizers_reset_internal(k);
    if (asx_kernel_other_held(k)) { /* the subsystems below are process-wide */
        asx_trace_reset_internal(k);
        return;
    }
    asx_cancel_chain_limits_reset_internal();
    asx_ghost_reset_run(); /* the sealed determinism reference survives the reset */
    asx_scheduler_event_reset();
    asx_lab_dispatch_reset_internal();
    /* Before the primitives: every wait queue is empty afterwards. */
    asx_wait_pool_reset();
    asx_parallel_reset();
    asx_channel_reset();
    asx_oneshot_reset();
    asx_watch_reset();
    asx_broadcast_reset();
    asx_session_reset();
    asx_timer_wheel_reset(asx_timer_wheel_global());
    asx_waker_reset();
#if ASX_HAS_NATIVE_IO_DRIVER
    asx_io_driver_reset();
#endif
#if ASX_HAS_BLOCKING_SURFACE
    asx_blocking_pool_reset();
#endif
    asx_trace_reset_internal(k);
    asx_telemetry_reset();
    asx_hindsight_reset();
    asx_adapter_reset_all();
    asx_fault_clear();
    asx_runtime_hooks_reset_internal();
    asx_event_log_reset();
    asx_error_ledger_reset();
    asx_notify_reset();
    asx_semaphore_reset();
    asx_rwlock_reset();
    asx_barrier_reset();
    asx_once_reset();
    asx_actor_reset();
    asx_supervisor_reset();
    /* net, fs, process and signal: each registered its reset the first time it held state, and
     * the hooks run here in slot order, where the four resets were called by name (bd-dkfs). The
     * kernel names none of them, so an application that never uses one does not link it; a
     * subsystem never used has nothing a reset would clear. */
    asx_runtime_reset_hooks_run_internal();
    asx_diagnostic_reset();
}

/* -------------------------------------------------------------------
 * Generation-safe lookup helpers (shared across TUs)
 *
 * Returns ASX_OK on success, ASX_E_STALE_HANDLE when the slot is
 * alive but the handle's generation doesn't match, or ASX_E_NOT_FOUND
 * for all other failures (invalid handle, wrong type tag, dead slot).
 * ------------------------------------------------------------------- */

asx_status asx_region_slot_lookup(asx_region_id id, asx_region_slot **out) {
    uint16_t slot_idx, handle_gen;
    asx_kernel *k = asx_kernel_of(id);

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    if (asx_handle_type_tag(id) != ASX_TYPE_REGION || k == NULL) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_REGIONS) return ASX_E_NOT_FOUND;
    if (!k->regions[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != k->regions[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &k->regions[slot_idx];
    return ASX_OK;
}

asx_status asx_task_slot_lookup(asx_task_id id, asx_task_slot **out) {
    uint16_t slot_idx, handle_gen;
    asx_kernel *k = asx_kernel_of(id);

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    if (asx_handle_type_tag(id) != ASX_TYPE_TASK || k == NULL) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_TASKS) return ASX_E_NOT_FOUND;
    if (!k->tasks[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != k->tasks[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &k->tasks[slot_idx];
    return ASX_OK;
}

asx_status asx_task_set_state_internal(asx_task_slot *t, asx_task_id id, asx_task_state to) {
    asx_status st = asx_task_transition_check(t->state, to);
    if (st != ASX_OK) {
        (void)asx_ghost_check_task_transition(id, t->state, to);
        return st;
    }
    t->state = to;
    return ASX_OK;
}

static uint32_t asx_align_up_u32(uint32_t value, uint32_t align) {
    uint32_t rem = value % align;
    if (rem == 0u) return value;
    return value + (align - rem);
}

static void *asx_region_capture_alloc(asx_region_slot *region, uint32_t size,
                                      uint32_t *old_used_out) {
    uint32_t start;
    uint32_t aligned_size;
    uint32_t end;

    if (size == 0u || old_used_out == NULL) return NULL;
    /* Guard against overflow in alignment: reject sizes that would wrap */
    if (size > ASX_REGION_CAPTURE_ARENA_BYTES) return NULL;

    *old_used_out = region->capture_used;
    start = asx_align_up_u32(region->capture_used, 8u);
    aligned_size = asx_align_up_u32(size, 8u);

    if (start > ASX_REGION_CAPTURE_ARENA_BYTES) return NULL;
    if (aligned_size > (ASX_REGION_CAPTURE_ARENA_BYTES - start)) return NULL;

    end = start + aligned_size;
    if (end > ASX_REGION_CAPTURE_ARENA_BYTES) return NULL;

    region->capture_used = end;
    return (void *)&region->capture_arena.bytes[start];
}

/* -------------------------------------------------------------------
 * Slot reclamation
 *
 * Task and obligation slots move between "live" and a LIFO free list.
 * Releasing a slot bumps its generation, so every outstanding handle to
 * the old occupant fails closed (ASX_E_STALE_HANDLE / ASX_E_NOT_FOUND).
 * ------------------------------------------------------------------- */

asx_task_id asx_task_handle_for_slot(const asx_kernel *k, uint32_t slot_idx) {
    const asx_task_slot *t = &k->tasks[slot_idx];
    return asx_handle_mint(k, ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)t->state),
                           asx_handle_pack_index(t->generation, (uint16_t)slot_idx));
}

static void asx_task_slot_release(asx_kernel *k, uint32_t idx) {
    asx_task_slot *t = &k->tasks[idx];

    if (!t->alive) return;
    asx_task_release_capture_internal(t);
    t->alive = 0;
    t->generation++;
    t->detached = 0;
    t->poll_fn = NULL;
    t->user_data = NULL;
    t->region = ASX_INVALID_ID;
    t->cancel_pending = 0;
    t->next_free = k->task_free_head == 0u ? ASX_SLOT_NONE : k->task_free_head - 1u;
    k->task_free_head = idx + 1u;
    if (k->task_live > 0u) k->task_live--;
}

/* Resolve a holder handle to its live task slot (NULL if gone). */
static asx_task_slot *asx_obligation_holder_slot(asx_task_id holder) {
    asx_task_slot *t;
    if (holder == ASX_INVALID_ID) return NULL;
    if (asx_task_slot_lookup(holder, &t) != ASX_OK) return NULL;
    return t;
}

/* Remove obligation `idx` from its holder's held list (if linked). */
static void asx_obligation_unlink_holder(asx_kernel *k, uint32_t idx) {
    asx_obligation_slot *o = &k->obligations[idx];
    asx_task_slot *t = asx_obligation_holder_slot(o->holder);
    uint32_t *link;

    if (t == NULL) {
        o->next_held = ASX_SLOT_NONE;
        return;
    }
    link = &t->first_held;
    while (*link != ASX_SLOT_NONE) {
        ASX_CHECKPOINT_WAIVER("bounded: held list length <= ASX_MAX_OBLIGATIONS");
        if (*link == idx) {
            *link = o->next_held;
            break;
        }
        link = &k->obligations[*link].next_held;
    }
    o->next_held = ASX_SLOT_NONE;
}

static void asx_obligation_slot_release(asx_kernel *k, uint32_t idx) {
    asx_obligation_slot *o = &k->obligations[idx];

    if (!o->alive) return;
    if (o->state == ASX_OBLIGATION_RESERVED) asx_obligation_unlink_holder(k, idx);
    o->holder = ASX_INVALID_ID;
    o->alive = 0;
    o->generation++;
    o->region = ASX_INVALID_ID;
    o->next_free = k->obligation_free_head == 0u ? ASX_SLOT_NONE : k->obligation_free_head - 1u;
    k->obligation_free_head = idx + 1u;
    if (k->obligation_live > 0u) k->obligation_live--;
}

/* A region's records are reclaimable once the region is CLOSED or its
 * slot has been recycled (the stored handle no longer resolves). */
static int asx_region_records_reclaimable(asx_region_id region) {
    asx_region_slot *r;
    if (asx_region_slot_lookup(region, &r) != ASX_OK) return 1;
    return r->state == ASX_REGION_CLOSED;
}

/* Allocation-pressure reclamation: free every completed task whose
 * region is CLOSED (or gone), in ascending slot order. */
static uint32_t asx_task_reclaim_closed(asx_kernel *k) {
    uint32_t i;
    uint32_t freed = 0;

    for (i = k->task_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: k->task_count <= ASX_MAX_TASKS");
        asx_task_slot *t = &k->tasks[i - 1u];
        if (!t->alive || !asx_task_is_terminal(t->state)) continue;
        if (!asx_region_records_reclaimable(t->region)) continue;
        /* Released in descending order so the LIFO free list hands the
         * lowest slot index out first. */
        asx_task_slot_release(k, i - 1u);
        freed++;
    }
    return freed;
}

/* Allocation-pressure reclamation for obligations: free every resolved
 * (committed/aborted/leaked) obligation, in ascending slot order. */
static uint32_t asx_obligation_reclaim_resolved(asx_kernel *k) {
    uint32_t i;
    uint32_t freed = 0;

    for (i = k->obligation_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: k->obligation_count <= ASX_MAX_OBLIGATIONS");
        asx_obligation_slot *o = &k->obligations[i - 1u];
        /* A slot a pending post names stays until the post is applied. */
        if (!o->alive || o->state == ASX_OBLIGATION_RESERVED || o->posts > 0u) continue;
        asx_obligation_slot_release(k, i - 1u);
        freed++;
    }
    return freed;
}

static asx_status asx_task_slot_alloc(asx_kernel *k, uint32_t *out_idx) {
    if (k->task_free_head == 0u && k->task_count >= ASX_MAX_TASKS) {
        (void)asx_task_reclaim_closed(k);
    }
    if (k->task_free_head != 0u) {
        *out_idx = k->task_free_head - 1u;
        k->task_free_head =
            k->tasks[*out_idx].next_free == ASX_SLOT_NONE ? 0u : k->tasks[*out_idx].next_free + 1u;
    } else if (k->task_count < ASX_MAX_TASKS) {
        *out_idx = k->task_count++;
    } else {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    k->tasks[*out_idx].next_free = ASX_SLOT_NONE;
    k->task_live++;
    return ASX_OK;
}

static asx_status asx_obligation_slot_alloc(asx_kernel *k, uint32_t *out_idx) {
    if (k->obligation_free_head == 0u && k->obligation_count >= ASX_MAX_OBLIGATIONS) {
        (void)asx_obligation_reclaim_resolved(k);
    }
    if (k->obligation_free_head != 0u) {
        *out_idx = k->obligation_free_head - 1u;
        k->obligation_free_head = k->obligations[*out_idx].next_free == ASX_SLOT_NONE
                                      ? 0u
                                      : k->obligations[*out_idx].next_free + 1u;
    } else if (k->obligation_count < ASX_MAX_OBLIGATIONS) {
        *out_idx = k->obligation_count++;
    } else {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    k->obligations[*out_idx].next_free = ASX_SLOT_NONE;
    k->obligation_live++;
    return ASX_OK;
}

/* Free every task and obligation record still bound to a region whose
 * slot is about to be recycled. */
static void asx_region_release_records(asx_kernel *k, asx_region_id region) {
    uint32_t i;
    uint32_t key = asx_handle_index(region); /* slot + generation */

    for (i = k->task_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: k->task_count <= ASX_MAX_TASKS");
        if (k->tasks[i - 1u].alive && asx_handle_index(k->tasks[i - 1u].region) == key) {
            asx_task_slot_release(k, i - 1u);
        }
    }
    for (i = k->obligation_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: k->obligation_count <= ASX_MAX_OBLIGATIONS");
        if (k->obligations[i - 1u].alive &&
            asx_handle_index(k->obligations[i - 1u].region) == key) {
            asx_obligation_slot_release(k, i - 1u);
        }
    }
}

void asx_runtime_set_leak_policy_internal(asx_kernel *k, asx_leak_response response,
                                          const asx_leak_escalation_config *escalation) {
    k->leak_response = response;
    if (escalation != NULL) {
        k->leak_escalation = *escalation;
        k->leak_escalation_set = 1;
    } else {
        k->leak_escalation_set = 0;
    }
}

/* handle-less: kernel 0 until the query takes its runtime */
uint64_t asx_obligation_leak_count(void) { return g_kernels[0]->leak_count; }

void asx_runtime_set_cleanup_hard_bound_internal(asx_kernel *k, int on) {
    k->cleanup_hard_bound = on ? 1 : 0;
}

int asx_cleanup_hard_bound_internal(const asx_kernel *k) { return k->cleanup_hard_bound; }

void asx_task_apply_cleanup_budget_internal(asx_task_slot *t) {
    if (t->cleanup_applied || t->state != ASX_TASK_CANCELLING) return;
    t->budget = t->cleanup_budget;
    t->cleanup_applied = 1u;
}

/* The response to a batch of `n` leaks found together: Rust counts the
 * batch first, then picks one response for all of it, escalated once the
 * cumulative count reaches the threshold (handle_obligation_leaks,
 * runtime/state.rs:5711-5717). A threshold of 0 therefore acts as 1, as
 * LeakEscalation::new makes it (runtime/config.rs:1354). */
static asx_leak_response obligation_leak_batch_policy(asx_kernel *k, uint64_t n) {
    k->leak_count = n > UINT64_MAX - k->leak_count ? UINT64_MAX : k->leak_count + n;
    if (k->leak_escalation_set && k->leak_count >= k->leak_escalation.threshold) {
        return k->leak_escalation.escalate_to;
    }
    return k->leak_response;
}

/* The kernel's clock, read without the side effects of asx_runtime_now_ns
 * (its fault-injection call counts and the hindsight log): an obligation's
 * held time is bookkeeping and must not shift either. Deterministic builds
 * read the logical clock, as the scheduler's own reading does, so the value
 * is the virtual time Rust's lab reports. */
static asx_time obligation_clock(const asx_kernel *k) { return asx_kernel_clock_peek_internal(k); }

/* How long the obligation has been held, as of the resolution being
 * applied (Rust's held_duration_ns = now - reserved_at). Taken before the
 * resolution's trace event, so an observer reads it with the event. */
static void obligation_note_held(const asx_kernel *k, asx_obligation_slot *o) {
    asx_time now = obligation_clock(k);
    o->held_ns = now >= o->reserved_at ? now - o->reserved_at : 0u;
}

/* A holder completed with obligations still reserved: each plain one is a
 * leak, whether or not the holder was cancelled. This is Rust's common
 * path: a task body that ends holding an unresolved ObligationToken drops
 * it, and the drop posts a Leak (runtime/obligation_mailbox.rs:897) before
 * the completion phase runs, cancelled or not (fuzz finding gen-1-20 /
 * gen-1-37, bd-ij9w; fixture obligation-cancelled-holder-leaks-001). Rust's
 * completion-time abort with Cancel (abort_orphaned_obligations_for_holder,
 * state.rs:8454) only reaches tokens that were never dropped, which C, with
 * no destructors, cannot tell apart. The leak policy then applies; RECOVER
 * aborts with ASX_OBLIGATION_ABORT_ERROR. A permit's obligation is dropped
 * with the body instead (obligation_drop_permit): its Drop gives the
 * resource back and leaks nothing. */
/* Leak the reserved obligation in slot `idx` under `policy`: RECOVER
 * aborts it with ASX_OBLIGATION_ABORT_ERROR, as Rust's Recover aborts with
 * ObligationAbortReason::Error (state.rs:5781-5797), the others mark it
 * LEAKED (vocabulary obligation.leaked, Rust ObligationLeak). Returns 1
 * when the policy (PANIC) demands fail-fast containment. */
static int obligation_leak_apply(asx_kernel *k, uint32_t idx, asx_leak_response policy) {
    asx_obligation_slot *o = &k->obligations[idx];
    asx_obligation_id oid =
        asx_handle_mint(k, ASX_TYPE_OBLIGATION, (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                        asx_handle_pack_index(o->generation, (uint16_t)idx));
    o->counted = 0;
    obligation_note_held(k, o);
    if (policy == ASX_LEAK_RECOVER) {
        /* Recovered leak: abort, still counted as a leak. */
        o->state = ASX_OBLIGATION_ABORTED;
        o->abort_reason = ASX_OBLIGATION_ABORT_ERROR;
        asx_ghost_obligation_resolved(oid);
        (void)asx_event_emit(ASX_EVENT_OBLIGATION_ABORT, oid, 0u, ASX_OK);
        asx_trace_emit_internal(k, ASX_TRACE_OBLIGATION_ABORT, oid, 0);
        return 0;
    }
    o->state = ASX_OBLIGATION_LEAKED;
    asx_trace_emit_internal(k, ASX_TRACE_OBLIGATION_LEAK, oid, 0);
    return policy == ASX_LEAK_PANIC;
}

/* One leak as a batch of its own, as each dropped token posts its own
 * leak (state.rs:7036-7042): it is counted before the policy is picked, so
 * the leak that reaches the escalation threshold is the first escalated
 * one; LOG warns with `log_message`. Returns 1 when the policy (PANIC)
 * demands fail-fast containment. */
static int obligation_leak_slot(asx_kernel *k, uint32_t idx, const char *log_message) {
    asx_leak_response policy = obligation_leak_batch_policy(k, 1u);
    int fail_fast = obligation_leak_apply(k, idx, policy);
    if (policy == ASX_LEAK_LOG) (void)asx_runtime_log_write(ASX_LOG_WARN, log_message);
    return fail_fast;
}

static void obligation_apply_resolution(asx_kernel *k, uint32_t idx, asx_obligation_state to);

/* A permit dropped with its holder's body: its drop gives the resource
 * back, then the obligation resolves as that permit's Rust Drop resolves
 * it, applied at once (the holder's polls are over): SendPermit aborts
 * with Cancel (channel/mpsc.rs:1663-1673, channel/oneshot.rs SendPermit
 * drop), SemaphorePermit commits (sync/semaphore.rs:1081). No leak:
 * Rust reports one only for a plain ObligationToken. */
static void obligation_drop_permit(asx_kernel *k, uint32_t idx) {
    asx_obligation_slot *o = &k->obligations[idx];
    asx_obligation_drop_fn fn = o->drop_fn;
    asx_obligation_state to;
    o->drop_fn = NULL;
    to = fn(o->drop_a, o->drop_b) ? ASX_OBLIGATION_COMMITTED : ASX_OBLIGATION_ABORTED;
    if (to == ASX_OBLIGATION_ABORTED) o->abort_reason = ASX_OBLIGATION_ABORT_CANCEL;
    o->state = to;
    o->counted = 0;
    obligation_apply_resolution(k, idx, to);
}

void asx_obligation_set_drop_internal(asx_obligation_id id, asx_obligation_drop_fn fn, uint64_t a,
                                      uint64_t b, uint32_t serial) {
    asx_obligation_slot *o;
    if (asx_obligation_slot_lookup(id, &o) != ASX_OK) return;
    if (o->state != ASX_OBLIGATION_RESERVED) return;
    o->drop_fn = fn;
    o->drop_a = a;
    o->drop_b = b;
    o->drop_serial = serial;
}

uint32_t asx_task_resolve_held_obligations_internal(asx_task_slot *task, int *out_fail_fast) {
    asx_kernel *k = asx_kernel_of(task->region);
    uint32_t leaks = 0;
    uint32_t idx = task->first_held;

    if (out_fail_fast != NULL) *out_fail_fast = 0;
    task->first_held = ASX_SLOT_NONE;
    while (idx != ASX_SLOT_NONE) {
        ASX_CHECKPOINT_WAIVER("bounded: held list length <= ASX_MAX_OBLIGATIONS");
        asx_obligation_slot *o = &k->obligations[idx];
        uint32_t next = o->next_held;

        o->next_held = ASX_SLOT_NONE;
        if (o->alive && o->state == ASX_OBLIGATION_RESERVED && o->drop_fn != NULL) {
            obligation_drop_permit(k, idx);
        } else if (o->alive && o->state == ASX_OBLIGATION_RESERVED) {
            if (obligation_leak_slot(k, idx,
                                     "obligation leaked: holder task completed with it "
                                     "reserved") &&
                out_fail_fast != NULL) {
                *out_fail_fast = 1;
            }
            leaks++;
        }
        idx = next;
    }
    return leaks;
}

/* The fault raised during completion bookkeeping (PANIC leak policy) that
 * the scheduler must report, mirroring a failing poll under FAIL_FAST. */
asx_status asx_runtime_take_pending_fault_internal(asx_kernel *k) {
    asx_status st = k->pending_fault;
    k->pending_fault = ASX_OK;
    return st;
}

asx_status asx_region_audit_leaks_internal(asx_region_id region) {
    asx_kernel *k = asx_kernel_of(region);
    uint32_t i;
    uint32_t n = 0;
    int fail_fast = 0;
    asx_leak_response policy;

    if (k == NULL) return ASX_OK;
    /* Rust's Finalizing-arm leak audit (state.rs:10266-10295): the
     * obligations of the region still Reserved once its tasks are gone are
     * one leak batch (collect_obligation_leaks, then handle_obligation_leaks
     * with no task): counted together, one response for all of them,
     * logged once under LOG (state.rs:5711-5760). */
    for (i = 0; i < k->obligation_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: k->obligation_count <= ASX_MAX_OBLIGATIONS");
        if (k->obligations[i].alive && k->obligations[i].region == region &&
            k->obligations[i].state == ASX_OBLIGATION_RESERVED) {
            n++;
        }
    }
    if (n == 0u) return ASX_OK;
    policy = obligation_leak_batch_policy(k, n);
    for (i = 0; i < k->obligation_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: k->obligation_count <= ASX_MAX_OBLIGATIONS");
        if (k->obligations[i].alive && k->obligations[i].region == region &&
            k->obligations[i].state == ASX_OBLIGATION_RESERVED &&
            obligation_leak_apply(k, i, policy)) {
            fail_fast = 1;
        }
    }
    if (policy == ASX_LEAK_LOG) {
        (void)asx_runtime_log_write(ASX_LOG_WARN,
                                    "obligation leaked: still reserved when its region closed");
    }
    if (!fail_fast) return ASX_OK;
    /* PANIC: Rust marks them leaked, then fails fast (state.rs:5735-5756)
     * before the region closes. C routes the fault through the region's
     * containment policy, as a leak at a holder's completion does. */
    {
        asx_status fc = asx_region_contain_fault(region, ASX_E_UNRESOLVED_OBLIGATIONS);
        if (fc != ASX_OK && asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
            k->pending_fault = fc;
        }
    }
    return ASX_E_UNRESOLVED_OBLIGATIONS;
}

void asx_task_on_complete_internal(asx_task_slot *task, asx_region_slot *region) {
    asx_kernel *k = asx_kernel_of(task->region);
    asx_outcome record_outcome;
    (void)asx_cancel_witness_release(task->cancel_witness); /* its cancel, if any, is over */
    /* Rust's outcome carries its payload (Cancelled(reason), Panicked(payload)); every completion
     * path sets the severity and the slot's reason or message first, so it is taken here, once. */
    if (task->outcome.severity == ASX_OUTCOME_CANCELLED) {
        task->outcome.cancel_reason = task->cancel_reason;
    } else if (task->outcome.severity == ASX_OUTCOME_PANICKED) {
        task->outcome.panic_message = task->panic_message;
    }
    record_outcome = asx_task_record_outcome_internal(task); /* the record's, not the join's */
    if (task->first_held != ASX_SLOT_NONE) {
        int fail_fast = 0;
        (void)asx_task_resolve_held_obligations_internal(task, &fail_fast);
        if (fail_fast && region != NULL) {
            /* PANIC: route the leak through the region's containment policy. */
            asx_status fc = asx_region_contain_fault(task->region, ASX_E_UNRESOLVED_OBLIGATIONS);
            if (fc != ASX_OK && asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
                k->pending_fault = fc;
            }
        }
    }
    asx_task_timer_disarm_internal(task);
    /* Drop captured servers and remaining broadcast credits; pass on any wake or grant held. */
    asx_broadcast_task_finished_internal(asx_task_handle_for_slot(k, (uint32_t)(task - k->tasks)));
    asx_actor_task_finished(asx_task_handle_for_slot(k, (uint32_t)(task - k->tasks)));
    asx_wait_task_finished(asx_task_handle_for_slot(k, (uint32_t)(task - k->tasks)));
    asx_task_join_detach_internal(task);
    asx_task_join_wake_waiters_internal(task);
    task->parked = 0;
    asx_task_release_capture_internal(task);
    if (region != NULL) {
        /* unlink_completed_task_from_region_in (state.rs:8509-8522): the record's outcome joins. */
        region->close_outcome = region->has_close_outcome
                                    ? asx_outcome_join(&region->close_outcome, &record_outcome)
                                    : record_outcome;
        region->has_close_outcome = 1;
        asx_region_finalizer_task_done_internal(region, task);
    }
    if (region != NULL && region->task_count > 0u) region->task_count--;
    /* Watch effects dispatch after the join waiters (lab/runtime.rs:5010). */
    asx_watches_task_finished(asx_task_handle_for_slot(k, (uint32_t)(task - k->tasks)),
                              &record_outcome, obligation_clock(k));
    if (task->detached) { asx_task_slot_release(k, (uint32_t)(task - k->tasks)); }
}

void asx_region_settle_internal(asx_region_id rid) {
    asx_region_slot *region;
    if (asx_region_slot_lookup(rid, &region) != ASX_OK) return;
    /* Every task completion advances a closing region, as Rust's
     * complete_task does: a region with child regions moves to DRAINING
     * even while tasks remain, and the last task lets it finalize. */
    if (region->state != ASX_REGION_OPEN) asx_region_advance_internal(rid);
}

asx_region_id asx_region_handle_for_slot(const asx_kernel *k, uint32_t slot_idx) {
    return asx_handle_mint(k, ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                           asx_handle_pack_index(k->regions[slot_idx].generation,
                                                 (uint16_t)slot_idx));
}

/* Scratch for asx_region_subtree_internal: the walk's stack and the depth
 * of each output entry. */
static uint32_t g_subtree_stack[ASX_MAX_REGIONS];
static uint32_t g_subtree_stack_depth[ASX_MAX_REGIONS];
static uint32_t g_subtree_depth[ASX_MAX_REGIONS];

uint32_t asx_region_subtree_internal(asx_region_id root, uint32_t *out_slots, uint32_t max) {
    asx_kernel *k = asx_kernel_of(root);
    asx_region_slot *r;
    uint32_t top = 0;
    uint32_t count = 0;
    uint32_t i;

    if (out_slots == NULL || max == 0u) return 0u;
    if (asx_region_slot_lookup(root, &r) != ASX_OK) return 0u;
    if (max > ASX_MAX_REGIONS) max = ASX_MAX_REGIONS;

    /* Rust's cancel_request order (state.rs:7647-7652, :7904-7931): a
     * depth-first walk with a stack, children pushed in insertion order
     * (so the last child is visited first), then a stable sort by depth.
     * Parents precede their descendants; regions at one depth keep the
     * walk's order. A cancel visits regions, and their tasks, in this
     * order. */
    g_subtree_stack[top] = asx_handle_slot(root);
    g_subtree_stack_depth[top] = 0u;
    top++;
    while (top > 0u && count < max) {
        ASX_CHECKPOINT_WAIVER("bounded: each live region is pushed once, top <= ASX_MAX_REGIONS");
        asx_region_slot *cur;
        uint32_t depth;
        uint32_t c;
        top--;
        depth = g_subtree_stack_depth[top];
        out_slots[count] = g_subtree_stack[top];
        g_subtree_depth[count] = depth;
        cur = &k->regions[out_slots[count]];
        count++;
        for (c = 0; c < cur->child_count && top < ASX_MAX_REGIONS; c++) {
            ASX_CHECKPOINT_WAIVER("bounded: child_count <= ASX_MAX_REGION_CHILDREN");
            asx_region_slot *child;
            if (asx_region_slot_lookup(cur->children[c], &child) != ASX_OK) continue;
            g_subtree_stack[top] = asx_handle_slot(cur->children[c]);
            g_subtree_stack_depth[top] = depth + 1u;
            top++;
        }
    }
    for (i = 1; i < count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: insertion sort over count <= ASX_MAX_REGIONS");
        uint32_t slot = out_slots[i];
        uint32_t depth = g_subtree_depth[i];
        uint32_t j = i;
        while (j > 0u && g_subtree_depth[j - 1u] > depth) {
            ASX_CHECKPOINT_WAIVER("bounded: j <= count <= ASX_MAX_REGIONS");
            out_slots[j] = out_slots[j - 1u];
            g_subtree_depth[j] = g_subtree_depth[j - 1u];
            j--;
        }
        out_slots[j] = slot;
        g_subtree_depth[j] = depth;
    }
    return count;
}

/* -------------------------------------------------------------------
 * Region lifecycle
 * ------------------------------------------------------------------- */

/* Whether a task slot still holds a record of the region in slot `idx`: a
 * live task, or a completed one whose outcome nobody has joined. */
#ifndef ASX_DEBUG_QUARANTINE
static int region_has_task_records(const asx_kernel *k, uint32_t idx) {
    uint32_t i;
    for (i = 0; i < k->task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: k->task_count <= ASX_MAX_TASKS");
        if (k->tasks[i].alive && asx_handle_slot(k->tasks[i].region) == idx &&
            asx_handle_generation(k->tasks[i].region) == k->regions[idx].generation) {
            return 1;
        }
    }
    return 0;
}

/* A CLOSED region slot no task record still names. */
static int region_slot_reclaimable(const asx_kernel *k, uint32_t idx) {
    return k->regions[idx].alive && k->regions[idx].state == ASX_REGION_CLOSED &&
           k->regions[idx].task_count == 0 && !region_has_task_records(k, idx);
}

/* 1 if the region in slot idx has a parent that has not closed. */
static int region_parent_live(const asx_kernel *k, uint32_t idx) {
    asx_region_slot *parent;
    if (k->regions[idx].parent_id == ASX_INVALID_ID) return 0;
    if (asx_region_slot_lookup(k->regions[idx].parent_id, &parent) != ASX_OK) return 0;
    return parent->state != ASX_REGION_CLOSED;
}
#endif

/* Open a region whose parent is `parent` (ASX_INVALID_ID for a root),
 * recorded before its ASX_TRACE_REGION_OPEN, so an observer can read it. */
static asx_status region_open_with_parent(asx_kernel *k, asx_region_id parent,
                                          asx_region_id *out_id) {
    uint32_t idx;
    int reclaim;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Scan for a recyclable slot: unused (alive=0), or CLOSED with no task
     * record left. A completed task nobody joined keeps its region's slot:
     * its outcome stays joinable, as a Rust JoinHandle keeps the result
     * after the region's record is gone. A closed child of a live parent
     * keeps its slot too while another slot is free, so the handle its
     * opener holds still names a closed region: Rust remembers closed
     * regions (recently_closed_regions, state.rs:10469) and refuses a
     * spawn into one as RegionClosed, where a recycled C slot would make
     * the handle stale (fuzz gen-26-180).
     * When ASX_DEBUG_QUARANTINE is defined, CLOSED slots are never recycled
     * so that any stale-handle dereference surfaces as RESOURCE_EXHAUSTED
     * instead of silently aliasing a new region. Zero-cost when disabled. */
    reclaim = 0;
    for (idx = 0; idx < ASX_MAX_REGIONS; idx++) {
        if (!k->regions[idx].alive) break;
#ifndef ASX_DEBUG_QUARANTINE
        if (region_slot_reclaimable(k, idx) && !region_parent_live(k, idx)) {
            reclaim = 1;
            break;
        }
#endif
    }
#ifndef ASX_DEBUG_QUARANTINE
    if (idx >= ASX_MAX_REGIONS) {
        /* Arena full: closed children of live parents go too. */
        for (idx = 0; idx < ASX_MAX_REGIONS; idx++) {
            if (region_slot_reclaimable(k, idx)) {
                reclaim = 1;
                break;
            }
        }
    }
#endif
    if (idx >= ASX_MAX_REGIONS) return ASX_E_RESOURCE_EXHAUSTED;

    /* Increment generation on slot reclaim to invalidate stale handles.
     * The recycled region's completed tasks and resolved obligations go
     * with it: their handles become stale together with the region's. */
    if (reclaim) {
        asx_region_release_records(
            k, asx_handle_mint(k, ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                               asx_handle_pack_index(k->regions[idx].generation, (uint16_t)idx)));
        k->regions[idx].generation++;
    } else {
        k->region_live++;
    }

    k->regions[idx].state = ASX_REGION_OPEN;
    k->regions[idx].parent_id = parent;
    k->regions[idx].child_count = 0;
    for (uint32_t child_idx = 0; child_idx < ASX_MAX_REGION_CHILDREN; child_idx++) {
        k->regions[idx].children[child_idx] = ASX_INVALID_ID;
    }
    k->regions[idx].task_count = 0;
    k->regions[idx].task_total = 0;
    k->regions[idx].alive = 1;
    k->regions[idx].poisoned = 0;
    asx_cleanup_init(&k->regions[idx].cleanup);
    asx_region_budgets_reset_internal(&k->regions[idx]);
    k->regions[idx].limits.max_tasks = ASX_REGION_UNLIMITED;
    k->regions[idx].limits.max_children = ASX_REGION_UNLIMITED;
    k->regions[idx].limits.max_obligations = ASX_REGION_UNLIMITED;
    memset(&k->regions[idx].lab_principal, 0, sizeof(k->regions[idx].lab_principal));
    k->regions[idx].lab_pending_admissions = 0;
    memset(&k->regions[idx].finalizer, 0, sizeof(k->regions[idx].finalizer));
    k->regions[idx].capture_used = 0;
    k->regions[idx].has_close_outcome = 0;
    k->regions[idx].close_outcome = asx_outcome_make(ASX_OUTCOME_OK);
    k->regions[idx].cancel_requested = 0;
    k->regions[idx].cancel_reason.kind = ASX_CANCEL_USER;
    k->regions[idx].cancel_reason.origin_region = ASX_INVALID_ID;
    k->regions[idx].cancel_reason.origin_task = ASX_INVALID_ID;
    k->regions[idx].cancel_reason.timestamp = 0;
    k->regions[idx].cancel_reason.message = NULL;
    k->regions[idx].cancel_reason.cause = NULL;
    k->regions[idx].cancel_reason.truncated = 0;

    if (idx >= k->region_count) { k->region_count = idx + 1; }

    *out_id = asx_handle_mint(k, ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                              asx_handle_pack_index(k->regions[idx].generation, (uint16_t)idx));

    (void)asx_event_emit(ASX_EVENT_REGION_OPEN, *out_id, 0u, ASX_OK);
    asx_trace_emit_internal(k, ASX_TRACE_REGION_OPEN, *out_id, 0);
    return ASX_OK;
}

asx_status asx_region_open(const asx_runtime *rt, asx_region_id *out_id) {
    /* The tag rt holds names the kernel: a zeroed object is tag 0, the
     * library's static kernel, which needs no initialization. */
    if (rt == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_kernel_for_tag(rt->tag) == NULL) return ASX_E_INVALID_STATE;
    return region_open_with_parent(asx_kernel_for_tag(rt->tag), ASX_INVALID_ID, out_id);
}

asx_status asx_region_open_child(asx_region_id parent, asx_region_id *out_child) {
    asx_region_slot *parent_slot;
    asx_region_slot *child_slot;
    asx_status st;

    if (out_child == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(parent, &parent_slot);
    if (st != ASX_OK) return st;
    if (parent_slot->poisoned) return ASX_E_REGION_POISONED;
    /* A closing or closed parent rejects children as Rust's
     * RegionCreateError::ParentClosed does (vocabulary §5: REGION_CLOSED). */
    if (parent_slot->state != ASX_REGION_OPEN) return ASX_E_REGION_CLOSED;
    /* Rust RegionCreateError::ParentAtCapacity (vocabulary §5). */
    if (parent_slot->child_count >= parent_slot->limits.max_children) {
        return ASX_E_ADMISSION_LIMIT;
    }
    if (parent_slot->child_count >= ASX_MAX_REGION_CHILDREN) return ASX_E_RESOURCE_EXHAUSTED;

    st = region_open_with_parent(asx_kernel_of(parent), parent, out_child);
    if (st != ASX_OK) return st;

    st = asx_region_slot_lookup(*out_child, &child_slot);
    if (st != ASX_OK) return st;

    asx_region_budgets_inherit_internal(child_slot, parent_slot); /* children inherit both */
    parent_slot->children[parent_slot->child_count] = *out_child;
    parent_slot->child_count++;
    return ASX_OK;
}

asx_status asx_region_open_child_with_budget(asx_region_id parent, const asx_budget *budget,
                                             asx_region_id *out_child) {
    asx_region_slot *child_slot;
    asx_status st = asx_region_open_child(parent, out_child);
    if (st != ASX_OK) return st;
    st = asx_region_slot_lookup(*out_child, &child_slot);
    if (st != ASX_OK) return st;
    if (budget != NULL) child_slot->budget = asx_budget_meet(&child_slot->budget, budget);
    return ASX_OK;
}

asx_status asx_region_open_child_poll(asx_task_id self, asx_region_id parent,
                                      const asx_budget *budget, asx_region_id *out_child) {
    asx_task_slot *t;
    asx_status st;
    asx_budget inherited;

    if (out_child == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (asx_kernel_of(parent) != asx_kernel_of(self)) return ASX_E_NOT_FOUND;
    /* The child's budget starts from the opener's inherited budget
     * (Cx::inherited_budget, an unbounded poll quota in cleanup), met with
     * the requested one; the mint then meets the parent region's
     * (cx.rs:4505-4510, 2504-2511). */
    inherited = t->budget;
    if (t->cleanup_applied) inherited.poll_quota = UINT32_MAX;
    if (budget != NULL) inherited = asx_budget_meet(&inherited, budget);
    return asx_region_open_child_poll_internal(t, parent, &inherited, out_child);
}

asx_status asx_region_open_child_poll_internal(asx_task_slot *t, asx_region_id parent,
                                               const asx_budget *inherited,
                                               asx_region_id *out_child) {
    asx_region_slot *p;
    asx_status st;
    if (!asx_lab_dispatch_active()) {
        return asx_region_open_child_with_budget(parent, inherited, out_child);
    }
    switch (t->region_wait) {
    case ASX_REGION_WAIT_OPENED:
        t->region_wait = ASX_REGION_WAIT_NONE;
        *out_child = t->region_wait_region;
        return t->region_wait_status;
    case ASX_REGION_WAIT_OPEN: break;
    case ASX_REGION_WAIT_NONE:
        st = asx_region_slot_lookup(parent, &p);
        if (st != ASX_OK) return st;
        st = asx_lab_region_open_command(t, parent, inherited);
        if (st != ASX_OK) return st;
        t->region_wait = ASX_REGION_WAIT_OPEN;
        break;
    default: return ASX_E_INVALID_STATE; /* awaiting a close */
    }
    if (t->in_poll) t->park_requested = 1;
    return ASX_E_PENDING;
}

asx_status asx_region_cancel_request(asx_region_id region, const asx_cancel_reason *reason) {
    asx_region_slot *r;
    asx_status st;
    if (reason == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK && st != ASX_E_STALE_HANDLE) return st;
    /* The drain (or here, without lab dispatch) handles a region that is
     * gone, closed and its slot reused, as Rust does: region.cancelled
     * only. */
    if (asx_lab_dispatch_active()) return asx_lab_region_cancel_command(region, reason);
    if (st == ASX_E_STALE_HANDLE) {
        asx_region_trace_cancel_of_gone_internal(region, reason);
        return ASX_OK;
    }
    return asx_region_cancel(region, reason, NULL);
}

asx_status asx_region_close_request(asx_region_id region) {
    asx_cancel_reason close = asx_region_close_reason_internal();
    return asx_region_cancel_request(region, &close);
}

asx_status asx_region_close_poll(asx_task_id self, asx_region_id region) {
    asx_task_slot *t;
    asx_region_slot *r;
    asx_status st = asx_task_slot_lookup(self, &t);

    if (st != ASX_OK) return st;
    if (asx_kernel_of(region) != asx_kernel_of(self)) return ASX_E_NOT_FOUND;
    if (t->region_wait == ASX_REGION_WAIT_OPEN || t->region_wait == ASX_REGION_WAIT_OPENED) {
        return ASX_E_INVALID_STATE;
    }
    if (t->region_wait != ASX_REGION_WAIT_CLOSE || t->region_wait_region != region) {
        st = asx_region_close_request(region);
        if (st != ASX_OK) return st;
        t->region_wait = ASX_REGION_WAIT_CLOSE;
        t->region_wait_region = region;
        t->region_wait_prio = t->lab_waker_prio;
    }
    st = asx_region_slot_lookup(region, &r);
    if (st == ASX_OK && r->state != ASX_REGION_CLOSED) {
        if (t->in_poll) t->park_requested = 1;
        return ASX_E_PENDING;
    }
    t->region_wait = ASX_REGION_WAIT_NONE;
    t->region_wait_region = ASX_INVALID_ID;
    return st == ASX_E_STALE_HANDLE ? ASX_OK : st;
}

asx_status asx_region_get_budget(asx_region_id id, asx_budget *out) {
    asx_region_slot *r;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    *out = r->budget;
    return ASX_OK;
}

asx_status asx_region_get_parent(asx_region_id id, asx_region_id *out) {
    asx_region_slot *r;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    *out = r->parent_id;
    return ASX_OK;
}

asx_status asx_region_set_limits(asx_region_id id, const asx_region_limits *limits) {
    asx_region_slot *r;
    asx_status st;
    if (limits == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    r->limits = *limits;
    return ASX_OK;
}

asx_status asx_region_get_limits(asx_region_id id, asx_region_limits *out) {
    asx_region_slot *r;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    *out = r->limits;
    return ASX_OK;
}

asx_status asx_region_close(asx_region_id id) {
    asx_region_slot *r;
    asx_status st;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Ghost protocol monitor: record transition for diagnostics */
    (void)asx_ghost_check_region_transition(id, r->state, ASX_REGION_CLOSING);

    /* Transition Open -> Closing */
    st = asx_region_transition_check(r->state, ASX_REGION_CLOSING);
    if (st != ASX_OK) return st;

    r->state = ASX_REGION_CLOSING;
    (void)asx_event_emit(ASX_EVENT_REGION_CLOSE, id, 0u, ASX_OK);
    asx_trace_emit_internal(asx_kernel_of(id), ASX_TRACE_REGION_CLOSE, id, 0);
    return ASX_OK;
}

asx_status asx_region_close_with_outcome(asx_region_id id, const asx_outcome *body,
                                         const asx_cancel_reason *reason) {
    asx_region_slot *r;
#if !defined(ASX_MUTANT_EXCEPTIONAL_BODY_NO_CANCEL) || defined(ASX_MUTANT_OK_BODY_CANCELS)
    asx_cancel_reason failure;
#endif
    asx_status st;

    if (body == NULL || (int)body->severity < (int)ASX_OUTCOME_OK ||
        (int)body->severity > (int)ASX_OUTCOME_PANICKED) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (body->severity == ASX_OUTCOME_CANCELLED) {
        if (reason == NULL || (int)reason->kind < (int)ASX_CANCEL_USER ||
            (int)reason->kind > (int)ASX_CANCEL_SHUTDOWN) {
            return ASX_E_INVALID_ARGUMENT;
        }
    } else if (reason != NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;
    /* Exceptional bodies still emit cancellation receipts for a Closed region. */

    switch (body->severity) {
    case ASX_OUTCOME_OK:
#ifdef ASX_MUTANT_OK_BODY_CANCELS
        failure = asx_cancel_reason_default(ASX_CANCEL_FAIL_FAST, NULL);
        failure.origin_region = id;
        return asx_region_cancel(id, &failure, NULL);
#else
        if (r->state != ASX_REGION_OPEN) return ASX_OK;
        (void)asx_ghost_check_region_transition(id, r->state, ASX_REGION_CLOSING);
        st = asx_region_transition_check(r->state, ASX_REGION_CLOSING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_CLOSING;
        (void)asx_event_emit(ASX_EVENT_REGION_CLOSE, id, 0u, ASX_OK);
        /* Scope's Ok branch calls RegionRecord::begin_close directly.
         * Unlike RuntimeState::cancel_request, that native branch emits
         * no RegionCloseBegin trace event (scope.rs:879-881). */
        return ASX_OK;
#endif
    case ASX_OUTCOME_CANCELLED: return asx_region_cancel(id, reason, NULL);
    case ASX_OUTCOME_ERR:
    case ASX_OUTCOME_PANICKED:
#ifdef ASX_MUTANT_EXCEPTIONAL_BODY_NO_CANCEL
        return r->state == ASX_REGION_OPEN ? asx_region_close(id) : ASX_OK;
#else
        /* Rust CancelReason::fail_fast().with_region(id), including its
         * Cx-free constructor's default attribution and testing timestamp. */
        failure = asx_cancel_reason_default(ASX_CANCEL_FAIL_FAST, NULL);
        failure.origin_region = id;
        return asx_region_cancel(id, &failure, NULL);
#endif
    }
    return ASX_E_INVALID_ARGUMENT;
}

asx_status asx_region_get_state(asx_region_id id, asx_region_state *out_state) {
    asx_region_slot *r;
    asx_status st;

    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    *out_state = r->state;
    return ASX_OK;
}

asx_status asx_region_poison(asx_region_id id) {
    asx_region_slot *r;
    asx_status st;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    r->poisoned = 1;
    return ASX_OK;
}

asx_status asx_region_is_poisoned(asx_region_id id, int *out) {
    asx_region_slot *r;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    *out = r->poisoned;
    return ASX_OK;
}

asx_status asx_region_contain_fault(asx_region_id id, asx_status fault) {
    asx_containment_policy policy;

    if (fault == ASX_OK) return ASX_OK;

    policy = asx_containment_policy_active();
    switch (policy) {
    case ASX_CONTAIN_FAIL_FAST: return fault;
    case ASX_CONTAIN_POISON_REGION: {
        asx_status ps_ = asx_region_poison(id);
        (void)ps_;
        /* Propagate cancellation to all live tasks in the region.
         * ASX_CANCEL_RESOURCE is used because the fault represents
         * a resource/invariant violation requiring bounded cleanup.
         * This ensures no task continues with partially-corrupt state. */
        (void)asx_cancel_propagate(id, ASX_CANCEL_RESOURCE);
        return fault;
    }
    case ASX_CONTAIN_ERROR_ONLY:
    default: return fault;
    }
}

void asx_task_release_capture_internal(asx_task_slot *task) {
    if (task == NULL) return;

    if (task->captured_dtor != NULL && task->captured_state != NULL) {
        task->captured_dtor(task->captured_state, task->captured_size);
    }

    task->captured_dtor = NULL;
    task->captured_state = NULL;
    task->captured_size = 0;
}

/* -------------------------------------------------------------------
 * Task lifecycle
 * ------------------------------------------------------------------- */

/* finalizer_budget non-NULL: the task is a region's async finalizer (Rust's
 * cleanup task, create_task_infrastructure_in with cleanup_task set,
 * runtime/state.rs:4235-4401): cleanup work with that budget, masked, and
 * scheduled at the budget's priority without a deadline timer of its own. */
static asx_status task_spawn_impl(asx_region_id region, asx_task_poll_fn poll_fn, void *user_data,
                                  asx_task_id *out_id, int cleanup_only,
                                  const asx_budget *finalizer_budget) {
    asx_kernel *k = asx_kernel_of(region);
    asx_region_slot *r;
    asx_status st;
    uint32_t idx;
    /* Under lab dispatch a spawn from a poll waits for the next step's
     * admission (Rust's spawn mailbox), which checks max_tasks. */
    int deferred = asx_lab_dispatch_active() && asx_task_current() != ASX_INVALID_ID;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_lab_note_spawn_internal();

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Rust RegionState distinguishes normal work from required cleanup.
     * Only the explicit cleanup entry may admit into Finalizing. */
    if (!(cleanup_only ? asx_region_can_accept_cleanup_work(r->state)
                       : asx_region_can_accept_work(r->state))) {
        if (deferred) asx_lab_defer_refused_admission();
        return ASX_E_REGION_CLOSED;
    }
    /* At its live-task limit: Rust's admission refuses the child with
     * SpawnError::RegionAtCapacity; cleanup work in a Finalizing region is
     * exempt (record/region.rs add_task_internal). A deferred spawn is
     * checked at its admission instead (asx_lab_admit_pending): tasks that
     * complete before it, the spawner included, no longer count. */
    if (!deferred && !(cleanup_only && r->state == ASX_REGION_FINALIZING) &&
        asx_region_live_admitted_internal(r) >= r->limits.max_tasks) {
        return ASX_E_ADMISSION_LIMIT;
    }

    st = asx_task_slot_alloc(k, &idx);
    if (st != ASX_OK) return st;

    k->tasks[idx].state = ASX_TASK_CREATED;
    k->tasks[idx].region = region;
    k->tasks[idx].poll_fn = poll_fn;
    k->tasks[idx].user_data = user_data;
    k->tasks[idx].outcome = asx_outcome_make(ASX_OUTCOME_OK);
    k->tasks[idx].alive = 1;
    k->tasks[idx].detached = 0;
    k->tasks[idx].cleanup_only = cleanup_only ? 1u : 0u;
    k->tasks[idx].finalizer = finalizer_budget != NULL ? 1u : 0u;
    /* Created by the host: Rust's RuntimeState::create_task, whose task
     * takes exactly the budget it is given (state.rs:4461, 4274-4275); the
     * region's budget is not met into it. */
    k->tasks[idx].budget = finalizer_budget != NULL ? *finalizer_budget : asx_budget_infinite();
    k->tasks[idx].first_held = ASX_SLOT_NONE;
    asx_task_sched_init_internal(&k->tasks[idx]);
    /* MaskedFinalizer holds its mask from the first poll to completion. */
    if (finalizer_budget != NULL) k->tasks[idx].mask_depth = 1u;
    memset(&k->tasks[idx].lab_ent, 0, sizeof(k->tasks[idx].lab_ent));
    k->tasks[idx].lab_rust_index = 0;
    k->tasks[idx].lab_rust_gen = 0;
    k->tasks[idx].lab_rust_live = 0;
    /* Spawned by a running task: Rust's cx.spawn. It takes cx.spawn's
     * completion policy, and, in the spawner's own region, the spawner's
     * current budget (Cx::inherited_budget, cx.rs:2504), with an unbounded
     * poll quota once the spawner is in its cleanup (fuzz finding
     * gen-1-16, bd-ij9w). */
    {
        asx_task_slot *spawner;
        asx_task_id cur = asx_task_current();
        if (cur != ASX_INVALID_ID && asx_kernel_of(cur) == k &&
            asx_task_slot_lookup(cur, &spawner) == ASX_OK) {
            k->tasks[idx].spawned_in_poll = 1u;
            /* Its entropy forks from the spawning Cx: a child region's
             * principal for a spawn into a region the spawner opened
             * (child.cx().spawn), else the spawner's own. */
            if (asx_lab_dispatch_active()) {
                asx_lab_entropy_spawned_internal(&k->tasks[idx], spawner,
                                                 asx_handle_index(spawner->region) !=
                                                             asx_handle_index(region) &&
                                                         r->lab_principal.state ==
                                                             ASX_LAB_ENTROPY_READY
                                                     ? r
                                                     : NULL);
            }
            if (asx_handle_index(spawner->region) == asx_handle_index(region)) {
                k->tasks[idx].budget = spawner->budget;
                /* Rust's cleanup_phase starts when the acknowledgement is
                 * applied after the poll, not at the checkpoint that
                 * acknowledges (fuzz gen-7-54, bd-ij9w). */
                if (spawner->cleanup_applied) k->tasks[idx].budget.poll_quota = UINT32_MAX;
            } else {
                /* Into a region the spawner opened: child.cx().spawn, whose
                 * Cx carries that region's budget. */
                k->tasks[idx].budget = r->budget;
            }
        }
    }
    k->tasks[idx].captured_state = NULL;
    k->tasks[idx].captured_size = 0;
    k->tasks[idx].captured_dtor = NULL;
    k->tasks[idx].cancel_phase = 0;
    k->tasks[idx].cancel_pending = 0;
    k->tasks[idx].cancel_epoch = 0;
    k->tasks[idx].cleanup_budget = asx_budget_infinite();
    k->tasks[idx].cleanup_applied = 0;
    k->tasks[idx].member_seq = asx_task_next_member_seq_internal(k);
    k->tasks[idx].cleanup_polls_remaining = 0;
    k->tasks[idx].cancel_unmaterialized = 0;
    k->tasks[idx].cx_reason_ahead = 0;
    memset(&k->tasks[idx].cancel_reason, 0, sizeof(k->tasks[idx].cancel_reason));
    /* Lab dispatch: a task the host creates is scheduled at once at
     * priority 0, as the Rust lab driver does (run.rs:1648); one spawned
     * from a poll is admitted at the start of the next step (cx.spawn's
     * mailbox, LR:3957-4012). */
    if (asx_lab_dispatch_active()) {
        if (k->tasks[idx].spawned_in_poll) {
            asx_lab_defer_admission(&k->tasks[idx]);
        } else {
            asx_lab_entropy_direct_internal(&k->tasks[idx]);
            if (finalizer_budget == NULL) asx_lab_arm_budget_deadline_internal(&k->tasks[idx]);
            asx_lab_schedule(&k->tasks[idx],
                             finalizer_budget != NULL ? finalizer_budget->priority : 0u);
        }
    }

    r->task_count++;
    r->task_total++;

    *out_id = asx_handle_mint(k, ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                              asx_handle_pack_index(k->tasks[idx].generation, (uint16_t)idx));
    /* A parallel run in progress admits the task to its lanes (bd-9kll.4.11). */
    asx_parallel_task_admitted_internal(*out_id);

    (void)asx_event_emit(ASX_EVENT_TASK_SPAWN, *out_id, (uint64_t)region, ASX_OK);
    /* A deferred spawn is traced when it is admitted, as Rust records its
     * Spawn event at admission (runtime/state.rs:1119); a refused one
     * never is. */
    if (!deferred) asx_trace_emit_internal(k, ASX_TRACE_TASK_SPAWN, *out_id, (uint64_t)region);
    return ASX_OK;
}

asx_status asx_task_spawn(asx_region_id region, asx_task_poll_fn poll_fn, void *user_data,
                          asx_task_id *out_id) {
    return task_spawn_impl(region, poll_fn, user_data, out_id, 0, NULL);
}

asx_status asx_task_spawn_cleanup(asx_region_id region, asx_task_poll_fn poll_fn, void *user_data,
                                  asx_task_id *out_id) {
    return task_spawn_impl(region, poll_fn, user_data, out_id, 1, NULL);
}

asx_status asx_task_spawn_finalizer_internal(asx_region_id region, asx_task_poll_fn poll_fn,
                                             void *user_data, const asx_budget *budget,
                                             asx_task_id *out_id) {
    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;
    return task_spawn_impl(region, poll_fn, user_data, out_id, 1, budget);
}

asx_status asx_task_admission_status(asx_task_id id) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (t->lab_admission_pending) return ASX_E_PENDING;
    return t->lab_admission_refusal;
}

uint32_t asx_region_live_admitted_internal(const asx_region_slot *r) {
    return r->task_count >= r->lab_pending_admissions ? r->task_count - r->lab_pending_admissions
                                                      : 0u;
}

asx_status asx_task_refuse_admission_internal(asx_task_slot *t, asx_region_slot *r,
                                              asx_status why) {
    asx_region_id region = t->region;
    asx_kernel *k = asx_kernel_of(region);
    asx_status st;
    /* Rust resolves the refused handle Cancelled: a closed region with
     * CancelReason::new(ParentCancelled) (lab/runtime.rs:3999-4003), the
     * live-task limit with CancelReason::user carrying the error's text
     * (cx/cx.rs:5526-5534), both with the default attribution
     * (types/cancel.rs:596-623). The text is its fixed prefix: Rust's
     * spells the region's RegionId, its limit and live count. */
    st = asx_task_set_state_internal(t, asx_task_handle_for_slot(k, (uint32_t)(t - k->tasks)),
                                     ASX_TASK_COMPLETED);
    if (st != ASX_OK) return st;
    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    t->cancel_reason =
        why == ASX_E_REGION_CLOSED
            ? asx_cancel_reason_default(ASX_CANCEL_PARENT, NULL)
            : asx_cancel_reason_default(ASX_CANCEL_USER,
                                        "[ASUP-E006] region admission limit reached");
    t->cancel_pending = 1; /* its reason reads as any cancelled task's */
    t->lab_admission_refusal = why;
    asx_task_on_complete_internal(t, r);
    /* Releasing the pending-spawn credit re-advances the region
     * (lab/runtime.rs drain_spawn_admissions). */
    asx_region_settle_internal(region);
    return ASX_OK;
}

static asx_status task_spawn_captured_impl(asx_region_id region, asx_task_poll_fn poll_fn,
                                           uint32_t state_size, asx_task_state_dtor_fn state_dtor,
                                           asx_task_id *out_id, void **out_state,
                                           int cleanup_only) {
    asx_region_slot *r;
    asx_task_slot *t;
    asx_status st;
    void *captured;
    uint32_t old_capture_used;

    if (out_id == NULL || out_state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL || state_size == 0u) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;
    if (!(cleanup_only ? asx_region_can_accept_cleanup_work(r->state)
                       : asx_region_can_accept_work(r->state))) {
        return ASX_E_REGION_CLOSED;
    }

    captured = asx_region_capture_alloc(r, state_size, &old_capture_used);
    if (captured == NULL) return ASX_E_RESOURCE_EXHAUSTED;
    memset(captured, 0, state_size);

    st = task_spawn_impl(region, poll_fn, captured, out_id, cleanup_only, NULL);
    if (st != ASX_OK) {
        r->capture_used = old_capture_used;
        return st;
    }

    st = asx_task_slot_lookup(*out_id, &t);
    if (st != ASX_OK) {
        r->capture_used = old_capture_used;
        return st;
    }

    t->captured_state = captured;
    t->captured_size = state_size;
    t->captured_dtor = state_dtor;
    *out_state = captured;
    return ASX_OK;
}

asx_status asx_task_spawn_captured(asx_region_id region, asx_task_poll_fn poll_fn,
                                   uint32_t state_size, asx_task_state_dtor_fn state_dtor,
                                   asx_task_id *out_id, void **out_state) {
    return task_spawn_captured_impl(region, poll_fn, state_size, state_dtor, out_id, out_state, 0);
}

asx_status asx_task_spawn_cleanup_captured(asx_region_id region, asx_task_poll_fn poll_fn,
                                           uint32_t state_size, asx_task_state_dtor_fn state_dtor,
                                           asx_task_id *out_id, void **out_state) {
    return task_spawn_captured_impl(region, poll_fn, state_size, state_dtor, out_id, out_state, 1);
}

asx_status asx_task_get_state(asx_task_id id, asx_task_state *out_state) {
    asx_task_slot *t;
    asx_status st;

    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    *out_state = t->state;
    return ASX_OK;
}

asx_status asx_task_get_outcome(asx_task_id id, asx_outcome *out_outcome) {
    asx_task_slot *t;
    asx_status st;

    if (out_outcome == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (!asx_task_is_terminal(t->state)) return ASX_E_TASK_NOT_COMPLETED;

    *out_outcome = t->outcome;
    return ASX_OK;
}

asx_status asx_task_join(asx_task_id id, asx_outcome *out_outcome) {
    asx_task_slot *t;
    asx_status st;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (!asx_task_is_terminal(t->state)) return ASX_E_TASK_NOT_COMPLETED;

    if (out_outcome != NULL) *out_outcome = t->outcome;
    asx_task_slot_release(asx_kernel_of(id), (uint32_t)(t - asx_kernel_of(id)->tasks));
    return ASX_OK;
}

asx_status asx_task_detach(asx_task_id id) {
    asx_task_slot *t;
    asx_status st;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    t->detached = 1;
    if (asx_task_is_terminal(t->state)) {
        asx_task_slot_release(asx_kernel_of(id), (uint32_t)(t - asx_kernel_of(id)->tasks));
    }
    return ASX_OK;
}

asx_status asx_task_spawn_with_budget(asx_region_id region, asx_task_poll_fn poll_fn,
                                      void *user_data, const asx_budget *budget,
                                      asx_task_id *out_id) {
    asx_task_slot *t;
    asx_status st = asx_task_spawn(region, poll_fn, user_data, out_id);
    if (st != ASX_OK) return st;
    st = asx_task_slot_lookup(*out_id, &t);
    if (st != ASX_OK) return st;
    if (budget != NULL) {
        t->budget = asx_budget_meet(&t->budget, budget);
        /* From a task: Rust's cx.spawn_in(&cx.scope_with_budget(b)) clamps
         * the deadline and quotas to the spawner's inherited budget but
         * takes b's priority unclamped, boosting or lowering it
         * (cx.rs:4547-4586; fuzz finding gen-247-192). */
        if (t->spawned_in_poll) t->budget.priority = budget->priority;
        /* Lab dispatch: a host-created task's budget-deadline timer runs
         * on the met budget (a child's is armed at admission). */
        if (asx_lab_dispatch_active() && !t->spawned_in_poll) {
            asx_lab_arm_budget_deadline_internal(t);
        }
    }
    return ASX_OK;
}

asx_status asx_task_get_budget(asx_task_id id, asx_budget *out) {
    asx_task_slot *t;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    *out = t->budget;
    return ASX_OK;
}

asx_status asx_task_get_cleanup_budget(asx_task_id id, asx_budget *out) {
    asx_task_slot *t;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    if (!t->cancel_pending || asx_task_is_terminal(t->state)) return ASX_E_INVALID_STATE;
    *out = t->cleanup_budget;
    return ASX_OK;
}

asx_status asx_task_consume_cost(asx_task_id self, uint64_t cost) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    /* Rust's Budget::consume_cost (types/budget.rs:596-625): a charge the
     * quota cannot cover is refused and changes nothing. The task is
     * cancelled with COST_BUDGET only by a checkpoint that finds the quota
     * spent to exactly 0 (Cx::checkpoint, cx/cx.rs:3145; bd-9kll.3.10). */
    if (asx_budget_consume_cost(&t->budget, cost)) return ASX_OK;
    return ASX_E_COST_QUOTA_EXHAUSTED;
}

/* -------------------------------------------------------------------
 * Obligation lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_obligation_slot_lookup(asx_obligation_id id, asx_obligation_slot **out) {
    uint16_t slot_idx, handle_gen;
    asx_kernel *k = asx_kernel_of(id);

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    if (asx_handle_type_tag(id) != ASX_TYPE_OBLIGATION || k == NULL) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_OBLIGATIONS) return ASX_E_NOT_FOUND;
    if (!k->obligations[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != k->obligations[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &k->obligations[slot_idx];
    return ASX_OK;
}

/* Obligations counted against `region`'s max_obligations: reserved and
 * not resolved, as the region's pending count sees them (a checked
 * reservation and its resolution count at the call, a permit's at the
 * drain of its post). */
static uint32_t region_pending_obligations(const asx_kernel *k, asx_region_id region) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < k->obligation_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: k->obligation_count <= ASX_MAX_OBLIGATIONS");
        if (k->obligations[i].alive && k->obligations[i].counted &&
            asx_handle_index(k->obligations[i].region) == asx_handle_index(region)) {
            n++;
        }
    }
    return n;
}

/* Whether obligation operations are posts: lab dispatch, inside a poll. */
static int obligation_posting(void) {
    return asx_lab_dispatch_active() && asx_task_current() != ASX_INVALID_ID;
}

static asx_obligation_id obligation_handle(const asx_kernel *k, uint32_t idx) {
    return asx_handle_mint(k, ASX_TYPE_OBLIGATION,
                           (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                           asx_handle_pack_index(k->obligations[idx].generation, (uint16_t)idx));
}

/* The applied effects of each operation: record visible, ghost monitor,
 * event log and trace. */
static void obligation_apply_reserve(asx_kernel *k, uint32_t idx,
                                     const asx_obligation_info *original) {
    asx_obligation_id id = obligation_handle(k, idx);
    asx_region_id region = original != NULL ? original->region : k->obligations[idx].region;
    asx_trace_payload payload = {0};
    /* Rust stamps the record when the reserve is applied: a posted
     * reserve's clock is the end of its poll, not the call. */
    k->obligations[idx].reserved_at = obligation_clock(k);
    asx_ghost_obligation_reserved(id, region);
    (void)asx_event_emit(ASX_EVENT_OBLIGATION_CREATE, id, (uint64_t)region, ASX_OK);
    payload.obligation = original;
    asx_trace_emit_payload_internal(k, ASX_TRACE_OBLIGATION_RESERVE, id, (uint64_t)region,
                                    &payload);
}

static void obligation_apply_resolution(asx_kernel *k, uint32_t idx, asx_obligation_state to) {
    asx_obligation_id id = obligation_handle(k, idx);
    obligation_note_held(k, &k->obligations[idx]);
    (void)asx_ghost_check_obligation_transition(id, ASX_OBLIGATION_RESERVED, to);
    asx_ghost_obligation_resolved(id);
    if (to == ASX_OBLIGATION_COMMITTED) {
        (void)asx_event_emit(ASX_EVENT_OBLIGATION_COMMIT, id, 0u, ASX_OK);
        asx_trace_emit_internal(k, ASX_TRACE_OBLIGATION_COMMIT, id, 0);
    } else {
        (void)asx_event_emit(ASX_EVENT_OBLIGATION_ABORT, id, 0u, ASX_OK);
        asx_trace_emit_internal(k, ASX_TRACE_OBLIGATION_ABORT, id, 0);
    }
}

/* A dropped token's leak: the leak policy's effects. */
static void obligation_apply_leak(asx_kernel *k, uint32_t idx) {
    if (obligation_leak_slot(k, idx, "obligation leaked: dropped unresolved")) {
        /* PANIC policy: as for a leak at completion. */
        asx_status fc =
            asx_region_contain_fault(k->obligations[idx].region, ASX_E_UNRESOLVED_OBLIGATIONS);
        if (fc != ASX_OK && asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
            k->pending_fault = fc;
        }
    }
}

/* Queue a post; a full queue is drained first, keeping the order. */
static void obligation_post(asx_kernel *k, ob_post_kind kind, uint32_t idx) {
    if (k->ob_post_n >= OB_POST_CAP) asx_obligation_drain_posts_internal(k);
    k->ob_posts[k->ob_post_n].kind = (uint8_t)kind;
    k->ob_posts[k->ob_post_n].idx = idx;
    k->ob_posts[k->ob_post_n].generation = k->obligations[idx].generation;
    k->ob_posts[k->ob_post_n].reservation.state = k->obligations[idx].state;
    k->ob_posts[k->ob_post_n].reservation.kind = k->obligations[idx].kind;
    k->ob_posts[k->ob_post_n].reservation.region = k->obligations[idx].region;
    k->ob_posts[k->ob_post_n].reservation.holder = k->obligations[idx].holder;
    k->ob_posts[k->ob_post_n].reservation.abort_reason = k->obligations[idx].abort_reason;
    k->ob_posts[k->ob_post_n].reservation.held_ns = 0u;
    k->ob_post_n++;
    k->obligations[idx].posts++;
}

/* Apply the posts in order (Rust's drain at the end of the poll). A
 * permit's reservation is admitted now, against the region's count, and a
 * refused one is dropped with the posts that name it: the permit is
 * untracked. A permit's resolution releases its count now. */
void asx_obligation_drain_posts_internal(asx_kernel *k) {
    uint32_t i;
    uint32_t n = k->ob_post_n;
    k->ob_post_n = 0;
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= OB_POST_CAP");
        const ob_post *p = &k->ob_posts[i];
        asx_obligation_slot *o = &k->obligations[p->idx];
        if (!o->alive || o->generation != p->generation) continue; /* refused */
        if (o->posts > 0u) o->posts--;
        switch ((ob_post_kind)p->kind) {
        case OB_POST_RESERVE:
            if (!o->checked) {
                asx_region_slot *r;
                if (asx_region_slot_lookup(o->region, &r) == ASX_OK &&
                    r->limits.max_obligations != ASX_REGION_UNLIMITED &&
                    region_pending_obligations(k, o->region) >= r->limits.max_obligations) {
                    o->posts = 0;
                    asx_obligation_unlink_holder(k, p->idx);
                    asx_obligation_slot_release(k, p->idx);
                    continue;
                }
                o->counted = 1;
            }
            obligation_apply_reserve(k, p->idx, &p->reservation);
            break;
        case OB_POST_COMMIT:
            if (!o->checked) o->counted = 0;
            obligation_apply_resolution(k, p->idx, ASX_OBLIGATION_COMMITTED);
            break;
        case OB_POST_ABORT:
            if (!o->checked) o->counted = 0;
            obligation_apply_resolution(k, p->idx, ASX_OBLIGATION_ABORTED);
            break;
        case OB_POST_LEAK: obligation_apply_leak(k, p->idx); break;
        case OB_POST_HANDOFF: {
            asx_trace_payload payload = {0};
            payload.handoff = &p->handoff;
            asx_trace_emit_payload_internal(k, ASX_TRACE_OBLIGATION_HANDOFF,
                                            obligation_handle(k, p->idx), p->handoff.to_task,
                                            &payload);
            break;
        }
        }
    }
}

/* checked: Rust's try_register_obligation_checked, admitted against the
 * region's limit at the call; otherwise try_register_obligation, whose
 * post (inside a poll under lab dispatch) is admitted at the drain. */
static asx_status asx_obligation_reserve_impl(asx_region_id region, asx_obligation_kind kind,
                                              asx_task_slot *holder_slot, asx_task_id holder,
                                              int checked, asx_obligation_id *out_id) {
    asx_kernel *k = asx_kernel_of(region);
    asx_region_slot *r;
    asx_status st;
    uint32_t idx;
    int posting = obligation_posting();

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    /* A holder of another runtime is no task of this one. */
    if (holder_slot != NULL && asx_kernel_of(holder) != k) return ASX_E_NOT_FOUND;
    if (r->poisoned) return ASX_E_REGION_POISONED;
    /* The holder must be a task of this region (Rust create_obligation_in,
     * runtime/state.rs:6130-6135: ErrorKind::TaskNotOwned, which the
     * vocabulary maps to ASX_E_INVALID_STATE). */
    if (holder_slot != NULL && asx_handle_index(holder_slot->region) != asx_handle_index(region)) {
        return ASX_E_INVALID_STATE;
    }

    /* Only open regions can reserve obligations (Rust
     * ObligationAdmissionError::RegionClosed; vocabulary §5). */
    if (!asx_region_can_spawn(r->state)) return ASX_E_REGION_CLOSED;
    /* Every obligation has a live holder task: Rust creates a token only
     * through a task's Cx (create_obligation takes the holder's TaskId,
     * state.rs:6130), so a reserve with no polled task has no authority
     * to create one. Refused before anything is allocated or traced. */
    if (holder_slot == NULL) return ASX_E_PERMISSION_DENIED;
    /* At its pending-obligation limit (Rust try_reserve_obligation): at
     * the call, unless a permit's post is admitted at the drain. */
    if ((checked || !posting) && r->limits.max_obligations != ASX_REGION_UNLIMITED &&
        region_pending_obligations(k, region) >= r->limits.max_obligations) {
        return ASX_E_ADMISSION_LIMIT;
    }

    st = asx_obligation_slot_alloc(k, &idx);
    if (st != ASX_OK) return st;

    k->obligations[idx].state = ASX_OBLIGATION_RESERVED;
    k->obligations[idx].region = region;
    k->obligations[idx].alive = 1;
    k->obligations[idx].kind = kind;
    k->obligations[idx].abort_reason = ASX_OBLIGATION_ABORT_NONE;
    k->obligations[idx].holder = holder;
    k->obligations[idx].drop_fn = NULL;
    k->obligations[idx].drop_serial = 0u;
    k->obligations[idx].checked = checked ? 1u : 0u;
    k->obligations[idx].counted = (checked || !posting) ? 1u : 0u;
    k->obligations[idx].posts = 0;
    k->obligations[idx].reserved_at = 0u; /* stamped when the reserve is applied */
    k->obligations[idx].held_ns = 0u;
    /* Link into the holder's held list (resolved at its completion). */
    k->obligations[idx].next_held = holder_slot->first_held;
    holder_slot->first_held = idx;

    *out_id = obligation_handle(k, idx);
    if (posting) {
        obligation_post(k, OB_POST_RESERVE, idx);
    } else {
        obligation_apply_reserve(k, idx, NULL);
    }
    return ASX_OK;
}

asx_status asx_obligation_reserve(asx_region_id region, asx_obligation_id *out_id) {
    /* Implicit holder: the task being polled, if any and still live. */
    asx_task_id holder = asx_task_current();
    asx_task_slot *t = NULL;

    if (holder != ASX_INVALID_ID &&
        (asx_task_slot_lookup(holder, &t) != ASX_OK || asx_task_is_terminal(t->state) ||
         asx_kernel_of(holder) != asx_kernel_of(region))) {
        t = NULL; /* no polled task of this runtime */
    }
    return asx_obligation_reserve_impl(region, ASX_OBLIGATION_KIND_GENERIC, t,
                                       t != NULL ? holder : ASX_INVALID_ID, 1, out_id);
}

static asx_status obligation_reserve_kind(asx_region_id region, asx_obligation_kind kind,
                                          asx_task_id holder, int checked,
                                          asx_obligation_id *out_id) {
    asx_task_slot *t = NULL;
    asx_status st;

    if ((int)kind < (int)ASX_OBLIGATION_KIND_GENERIC ||
        (int)kind > (int)ASX_OBLIGATION_KIND_TRANSACTION) {
        return ASX_E_INVALID_ARGUMENT;
    }
    /* The holder is named, never defaulted: an obligation with none is not
     * representable (see asx_obligation_reserve_impl). */
    if (holder == ASX_INVALID_ID) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(holder, &t);
    if (st != ASX_OK) return st;
    if (asx_task_is_terminal(t->state)) return ASX_E_INVALID_STATE;
    return asx_obligation_reserve_impl(region, kind, t, holder, checked, out_id);
}

asx_status asx_obligation_reserve_ex(asx_region_id region, asx_obligation_kind kind,
                                     asx_task_id holder, asx_obligation_id *out_id) {
    return obligation_reserve_kind(region, kind, holder, 1, out_id);
}

asx_status asx_obligation_register(asx_region_id region, asx_obligation_kind kind,
                                   asx_task_id holder, asx_obligation_id *out_id) {
    return obligation_reserve_kind(region, kind, holder, 0, out_id);
}

asx_status asx_obligation_get_info(asx_obligation_id id, asx_obligation_info *out) {
    asx_obligation_slot *o;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;
    out->state = o->state;
    out->kind = o->kind;
    out->region = o->region;
    out->holder = o->holder;
    out->abort_reason = o->abort_reason;
    out->held_ns = o->held_ns;
    return ASX_OK;
}

/* Resolve a reserved obligation: its state changes now; a checked token
 * releases its region count now and a permit at the drain of its post;
 * the record's trace is applied now or at the drain (bd-2fga). */
static asx_status obligation_resolve(asx_kernel *k, uint32_t idx, asx_obligation_state to) {
    asx_obligation_slot *o = &k->obligations[idx];
    int posting = obligation_posting();
    asx_obligation_unlink_holder(k, idx);
    o->state = to;
    if (o->checked || !posting) o->counted = 0;
    if (posting) {
        obligation_post(k, to == ASX_OBLIGATION_COMMITTED ? OB_POST_COMMIT : OB_POST_ABORT, idx);
    } else {
        obligation_apply_resolution(k, idx, to);
    }
    return ASX_OK;
}

/* Application authority comes from the scheduler, never a caller-supplied
 * holder ID. Runtime completion/leak settlement uses its slot helpers and
 * does not call this application boundary. */
asx_status asx_obligation_check_access_internal(asx_obligation_id id) {
    asx_obligation_slot *o;
    asx_task_slot *caller;
    asx_task_slot *holder;
    asx_region_slot *region;
    asx_status st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;
    if (o->state != ASX_OBLIGATION_RESERVED) return ASX_E_OBLIGATION_ALREADY_RESOLVED;
#ifdef ASX_MUTANT_OBLIGATION_AUTH_BYPASS
    /* Isolated authorization negative control, never a release define. */
    return ASX_OK;
#endif
    asx_task_id current = asx_task_current();
    /* A bound primitive's credit is private to its owned permit: even its
     * holder must release/send that permit rather than settle by raw ID. */
    if (o->drop_fn != NULL) return ASX_E_PERMISSION_DENIED;
    st = asx_region_slot_lookup(o->region, &region);
    if (st != ASX_OK) return st;
    if (region->state == ASX_REGION_CLOSED) return ASX_E_REGION_CLOSED;
    /* Every obligation has a holder, and only its poll settles it. */
    if (current == ASX_INVALID_ID) return ASX_E_PERMISSION_DENIED;
    st = asx_task_slot_lookup(current, &caller);
    if (st != ASX_OK) return st;
    /* The poll of another runtime's task holds no authority here. */
    if (asx_kernel_of(current) != asx_kernel_of(id)) return ASX_E_PERMISSION_DENIED;
    if (!caller->in_poll || asx_task_is_terminal(caller->state)) return ASX_E_INVALID_STATE;
    if (asx_handle_index(caller->region) != asx_handle_index(o->region)) {
        return ASX_E_PERMISSION_DENIED;
    }
    st = asx_task_slot_lookup(o->holder, &holder);
    if (st != ASX_OK) return st;
    /* State-rights bits may differ between the first and later polls;
     * validated slot/generation identity supplies holder authority. */
    if (holder != caller) return ASX_E_PERMISSION_DENIED;
    return ASX_OK;
}

asx_status asx_obligation_check_token_internal(asx_obligation_id id, asx_obligation_drop_fn fn,
                                               uint64_t a, uint64_t b, uint32_t serial) {
    asx_obligation_slot *o;
    asx_region_slot *region;
    asx_status st = asx_obligation_slot_lookup(id, &o);
    /* No obligation behind the id: the post drain refused the permit's
     * reservation against max_obligations and released its slot
     * (asx_obligation_drain_posts_internal). The permit is untracked, as
     * Rust's whose registration was refused, and settles physically with
     * nothing to resolve; its primitive still authenticates it. */
    if (st == ASX_E_NOT_FOUND || st == ASX_E_STALE_HANDLE) return ASX_OK;
    if (st != ASX_OK) return st;
    if (fn == NULL || o->drop_a != a || o->drop_b != b || o->drop_serial != serial ||
        (o->drop_fn != fn && (o->state == ASX_OBLIGATION_RESERVED || o->drop_fn != NULL))) {
        return ASX_E_PERMISSION_DENIED;
    }
    if (o->state != ASX_OBLIGATION_RESERVED) return ASX_E_OBLIGATION_ALREADY_RESOLVED;
    st = asx_region_slot_lookup(o->region, &region);
    if (st != ASX_OK) return st;
    return region->state == ASX_REGION_CLOSED ? ASX_E_REGION_CLOSED : ASX_OK;
}

asx_status asx_obligation_resolve_token_internal(asx_obligation_id id, asx_obligation_drop_fn fn,
                                                 uint64_t a, uint64_t b, uint32_t serial,
                                                 asx_obligation_state to,
                                                 asx_obligation_abort_reason reason) {
    asx_obligation_slot *o;
    asx_status st;
    if (to != ASX_OBLIGATION_COMMITTED && to != ASX_OBLIGATION_ABORTED) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (to == ASX_OBLIGATION_ABORTED && reason != ASX_OBLIGATION_ABORT_EXPLICIT &&
        reason != ASX_OBLIGATION_ABORT_CANCEL && reason != ASX_OBLIGATION_ABORT_ERROR) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = asx_obligation_check_token_internal(id, fn, a, b, serial);
    if (st != ASX_OK) return st;
    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;
    if (to == ASX_OBLIGATION_ABORTED) o->abort_reason = reason;
    return obligation_resolve(asx_kernel_of(id), (uint32_t)(o - asx_kernel_of(id)->obligations),
                              to);
}

asx_status asx_obligation_commit(asx_obligation_id id) {
    asx_obligation_slot *o;
    asx_status st;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    /* Rust: committing an obligation that is no longer pending is
     * ObligationAlreadyResolved (runtime/obligation_table.rs:577). The
     * ghost monitor sees the invalid attempt; a valid transition is
     * checked when it is applied. */
    if (o->state != ASX_OBLIGATION_RESERVED) {
        (void)asx_ghost_check_obligation_transition(id, o->state, ASX_OBLIGATION_COMMITTED);
        return ASX_E_OBLIGATION_ALREADY_RESOLVED;
    }
    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_COMMITTED);
    if (st != ASX_OK) return st;
    st = asx_obligation_check_access_internal(id);
    if (st != ASX_OK) return st;
    return obligation_resolve(asx_kernel_of(id), (uint32_t)(o - asx_kernel_of(id)->obligations),
                              ASX_OBLIGATION_COMMITTED);
}

asx_status asx_obligation_abort(asx_obligation_id id) {
    return asx_obligation_abort_with_reason(id, ASX_OBLIGATION_ABORT_EXPLICIT);
}

/* A dropped Rust ObligationToken posts a Leak (obligation_mailbox.rs:897),
 * drained right after the poll that dropped it: the obligation is leaked
 * there, not when its holder completes (fuzz gen-12-2, bd-ij9w). Outside
 * a poll under lab dispatch it leaks at once. */
asx_status asx_obligation_drop(asx_obligation_id id) {
    asx_kernel *k = asx_kernel_of(id);
    asx_obligation_slot *o;
    uint32_t idx;
    asx_status st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;
#ifdef ASX_MUTANT_OBLIGATION_DROP_COMMITS
    /* Oracle self-test mutant (tests/gates/test_oracle_mutants.sh): a
     * dropped token counts as committed, its leak lost. Never in a release
     * build. */
    return asx_obligation_commit(id);
#endif
    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_LEAKED);
    if (st != ASX_OK) return st;
    st = asx_obligation_check_access_internal(id);
    if (st != ASX_OK) return st;
    idx = (uint32_t)(o - k->obligations);
    asx_obligation_unlink_holder(k, idx);
    if (obligation_posting()) {
        /* A checked token's drop releases its region count at once, as its
         * commit or abort does (fuzz gen-10-146). The state is no longer
         * Reserved from now on, so a second drop or a resolution fails; the
         * drain applies the leak policy (Leaked, or a recovered abort). */
        if (o->checked) o->counted = 0;
        o->state = ASX_OBLIGATION_LEAKED;
        obligation_post(k, OB_POST_LEAK, idx);
    } else {
        obligation_apply_leak(k, idx);
    }
    return ASX_OK;
}

asx_status asx_obligation_abort_with_reason(asx_obligation_id id,
                                            asx_obligation_abort_reason reason) {
    asx_obligation_slot *o;
    asx_status st;

    /* The reasons a caller may give (Rust ObligationAbortReason); NONE is
     * the runtime's own. */
    if (reason != ASX_OBLIGATION_ABORT_EXPLICIT && reason != ASX_OBLIGATION_ABORT_CANCEL &&
        reason != ASX_OBLIGATION_ABORT_ERROR) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    /* Rust: aborting an obligation that is no longer pending is
     * ObligationAlreadyResolved (runtime/obligation_table.rs:630). */
    if (o->state != ASX_OBLIGATION_RESERVED) {
        (void)asx_ghost_check_obligation_transition(id, o->state, ASX_OBLIGATION_ABORTED);
        return ASX_E_OBLIGATION_ALREADY_RESOLVED;
    }
    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_ABORTED);
    if (st != ASX_OK) return st;
    st = asx_obligation_check_access_internal(id);
    if (st != ASX_OK) return st;
    o->abort_reason = reason;
    return obligation_resolve(asx_kernel_of(id), (uint32_t)(o - asx_kernel_of(id)->obligations),
                              ASX_OBLIGATION_ABORTED);
}

asx_status asx_obligation_handoff(asx_obligation_id id, asx_task_id destination) {
    asx_kernel *k = asx_kernel_of(id);
    asx_obligation_slot *o;
    asx_task_slot *target;
    asx_region_slot *target_region;
    asx_region_id source_region;
    asx_trace_handoff handoff;
    asx_trace_payload payload = {0};
    uint32_t idx;
    asx_status st = asx_obligation_check_access_internal(id);
    if (st != ASX_OK) return st;
    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;
    /* Rust ObligationToken::try_transfer rejects legacy (unchecked)
     * sources, a same holder, a cancelled destination, and closed
     * destination admission (obligation_mailbox.rs:721-790;
     * record/region.rs:555). */
    if (!o->checked) return ASX_E_INVALID_STATE;
    st = asx_task_slot_lookup(destination, &target);
    if (st != ASX_OK) return st;
    /* A destination of another runtime is no task of this one. */
    if (asx_kernel_of(destination) != k) return ASX_E_NOT_FOUND;
    if (asx_handle_index(destination) == asx_handle_index(o->holder)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (target->cancel_pending) return ASX_E_CANCELLED;
    st = asx_region_slot_lookup(target->region, &target_region);
    if (st != ASX_OK) return st;
    if (target_region->poisoned) return ASX_E_REGION_POISONED;
    if (!asx_region_can_accept_work(target_region->state)) return ASX_E_REGION_CLOSED;
    if (asx_task_is_terminal(target->state)) return ASX_E_INVALID_STATE;
    if (asx_handle_index(target->region) != asx_handle_index(o->region) &&
        target_region->limits.max_obligations != ASX_REGION_UNLIMITED &&
        region_pending_obligations(k, target->region) >= target_region->limits.max_obligations) {
        return ASX_E_ADMISSION_LIMIT;
    }

    /* Every refusal precedes the authority change. The same slot/generation
     * and checked credit move, so no temporary double counting or extra
     * arena capacity is needed. Observers see the completed rebinding. */
    idx = (uint32_t)(o - k->obligations);
    source_region = o->region;
    handoff.from_task = o->holder;
    handoff.to_task = destination;
    handoff.from_region = source_region;
    handoff.to_region = target->region;
    asx_obligation_unlink_holder(k, idx);
    o->holder = destination;
    o->region = target->region;
    asx_ghost_obligation_moved(obligation_handle(k, idx), target->region);
    o->next_held = target->first_held;
    target->first_held = idx;
    payload.handoff = &handoff;
    if (obligation_posting()) {
        obligation_post(k, OB_POST_HANDOFF, idx);
        k->ob_posts[k->ob_post_n - 1u].handoff = handoff;
    } else {
        asx_trace_emit_payload_internal(k, ASX_TRACE_OBLIGATION_HANDOFF, id, destination, &payload);
    }
    /* Releasing a source region's final credit may permit its close. */
    if (asx_handle_index(source_region) != asx_handle_index(o->region) && !obligation_posting()) {
        asx_region_advance_internal(source_region);
    }
    return ASX_OK;
}

asx_status asx_obligation_get_state(asx_obligation_id id, asx_obligation_state *out_state) {
    asx_obligation_slot *o;
    asx_status st;

    if (out_state == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    *out_state = o->state;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Reset hooks of the optional subsystems (bd-dkfs)
 *
 * One slot per subsystem the kernel does not name, filled by the subsystem
 * itself the first time it can hold state and kept across resets: the
 * table is process state like the subsystems' own arenas. Slot order is
 * the order the resets were called in, so a registered subsystem is reset
 * exactly where it was.
 * ------------------------------------------------------------------- */

static void (*g_reset_hooks[ASX_RESET_SLOT_COUNT])(void);

void asx_runtime_reset_hook_register_internal(asx_reset_slot slot, void (*hook)(void)) {
    if ((unsigned)slot >= (unsigned)ASX_RESET_SLOT_COUNT || hook == NULL) return;
    g_reset_hooks[slot] = hook;
}

int asx_runtime_reset_hook_registered_internal(asx_reset_slot slot) {
    if ((unsigned)slot >= (unsigned)ASX_RESET_SLOT_COUNT) return 0;
    return g_reset_hooks[slot] != NULL;
}

void asx_runtime_reset_hooks_forget_internal(void) {
    uint32_t i;
    for (i = 0; i < (uint32_t)ASX_RESET_SLOT_COUNT; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded: one slot per optional subsystem") */
        g_reset_hooks[i] = NULL;
    }
}

void asx_runtime_reset_hooks_run_internal(void) {
    uint32_t i;
    for (i = 0; i < (uint32_t)ASX_RESET_SLOT_COUNT; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded: one slot per optional subsystem") */
        if (g_reset_hooks[i] != NULL) g_reset_hooks[i]();
    }
}

/* -------------------------------------------------------------------
 * Capability budgets of regions (bd-9kll.3.27)
 *
 * A region carries a resource envelope its children inherit and can only
 * tighten (Rust RegionRecord::capability_budget, record/region.rs:1080).
 * Admission of a child that supplies its own is
 * RegionTable::create_child_with_capability_budget (runtime/
 * region_table.rs:292-325): the parent's envelope met with the child's and
 * validated against the requirements, a refusal taken before the child
 * record is inserted, so nothing changes (its own invariant comment).
 * ------------------------------------------------------------------- */

asx_status asx_region_open_with_capability_budget(const asx_runtime *rt,
                                                  const asx_capability_budget *capability_budget,
                                                  asx_region_id *out_id) {
    asx_region_slot *slot;
    asx_status st = asx_region_open(rt, out_id);
    if (st != ASX_OK) return st;
    st = asx_region_slot_lookup(*out_id, &slot);
    if (st != ASX_OK) return st;
    if (capability_budget != NULL) slot->capability_budget = *capability_budget;
    return ASX_OK;
}

asx_status asx_region_open_child_with_capability_budget(
    asx_region_id parent, const asx_budget *budget, const asx_capability_budget *capability_budget,
    const asx_capability_requirements *requirements, asx_region_id *out_child,
    asx_capability_refusal *refusal) {
    asx_region_slot *parent_slot;
    asx_region_slot *child_slot;
    asx_capability_budget effective;
    asx_status st;

    if (out_child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (refusal != NULL) {
        refusal->kind = ASX_CAPABILITY_REFUSAL_NONE;
        refusal->dimension = ASX_CAPABILITY_MEMORY_BYTES;
    }
    st = asx_region_slot_lookup(parent, &parent_slot);
    if (st != ASX_OK) return st;

#ifdef ASX_MUTANT_CAPABILITY_SLOT_BEFORE_REFUSAL
    {
        /* Gate mutant (tests/gates/test_oracle_mutants.sh): the slot is
         * taken before the plan, so a refusal leaves a child behind. */
        asx_region_id taken;
        st = asx_region_open_child(parent, &taken);
        if (st != ASX_OK) return st;
    }
#endif
    /* Planned before any slot is taken: a refusal leaves the arena, the
     * parent's children and every counter as they were. */
    st = asx_capability_budget_plan_child(&parent_slot->capability_budget, capability_budget,
                                          requirements, &effective, refusal);
    if (st != ASX_OK) return st;

    st = asx_region_open_child_with_budget(parent, budget, out_child);
    if (st != ASX_OK) return st;
    st = asx_region_slot_lookup(*out_child, &child_slot);
    if (st != ASX_OK) return st;
    child_slot->capability_budget = effective;
    return ASX_OK;
}

asx_status asx_region_capability_budget(asx_region_id region, asx_capability_budget *out) {
    asx_region_slot *slot;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(region, &slot);
    if (st != ASX_OK) return st;
    *out = slot->capability_budget;
    return ASX_OK;
}

/* A task's open of a child region that carries an envelope (Rust
 * Cx::open_child_region with a ChildRegionSpec's capability_budget and
 * requirements, cx.rs:4491-4529). The budget is asx_region_open_child_poll's.
 * Under lab dispatch the open is a command and its plan happens when it is
 * applied (state.rs:5058): the first call parks the opener, and a refusal
 * comes with the second, after nothing was minted. The wait states are
 * asx_region_open_child_poll_internal's; this adds only the queueing of a
 * command that carries the envelope and the refusal's detail. */
asx_status asx_region_open_child_poll_with_capability_budget(
    asx_task_id self, asx_region_id parent, const asx_budget *budget,
    const asx_capability_budget *capability_budget, const asx_capability_requirements *requirements,
    asx_region_id *out_child, asx_capability_refusal *refusal) {
    asx_capability_budget none = asx_capability_budget_unspecified();
    asx_capability_requirements no_requirements = {0, 0, 0, 0, 0};
    asx_task_slot *t;
    asx_region_slot *p;
    asx_budget inherited;
    asx_status st;

    if (out_child == NULL) return ASX_E_INVALID_ARGUMENT;
    if (refusal != NULL) {
        refusal->kind = ASX_CAPABILITY_REFUSAL_NONE;
        refusal->dimension = ASX_CAPABILITY_MEMORY_BYTES;
    }
    if (capability_budget == NULL) capability_budget = &none;
    if (requirements == NULL) requirements = &no_requirements;
    st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (asx_kernel_of(parent) != asx_kernel_of(self)) return ASX_E_NOT_FOUND;
    inherited = t->budget;
    if (t->cleanup_applied) inherited.poll_quota = UINT32_MAX;
    if (budget != NULL) inherited = asx_budget_meet(&inherited, budget);

    if (!asx_lab_dispatch_active()) {
        return asx_region_open_child_with_capability_budget(parent, &inherited, capability_budget,
                                                            requirements, out_child, refusal);
    }
    if (t->region_wait == ASX_REGION_WAIT_NONE) {
        st = asx_region_slot_lookup(parent, &p);
        if (st != ASX_OK) return st;
        st = asx_lab_region_open_capability_command(t, parent, &inherited, capability_budget,
                                                    requirements);
        if (st != ASX_OK) return st;
        t->region_wait = ASX_REGION_WAIT_OPEN;
    }
    st = asx_region_open_child_poll_internal(t, parent, &inherited, out_child);
    if (st == ASX_E_CAPABILITY_BUDGET_REFUSED && refusal != NULL) {
        *refusal = asx_capability_refusal_unpack_internal(t->region_wait_refusal);
    }
    return st;
}
