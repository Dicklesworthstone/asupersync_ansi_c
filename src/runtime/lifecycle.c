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
#include <asx/sync/semaphore.h>
#include <asx/time/timer_wheel.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Global arenas (walking skeleton: fixed-size, no dynamic allocation)
 * ------------------------------------------------------------------- */

asx_region_slot g_regions[ASX_MAX_REGIONS];
uint32_t g_region_count;
uint32_t g_region_live;

asx_task_slot g_tasks[ASX_MAX_TASKS];
uint32_t g_task_count;
uint32_t g_task_live;

asx_obligation_slot g_obligations[ASX_MAX_OBLIGATIONS];
uint32_t g_obligation_count;
uint32_t g_obligation_live;

/* LIFO free lists of released slots (ASX_SLOT_NONE when empty). Reuse
 * order depends only on the release sequence, so replay is stable. */
static uint32_t g_task_free_head = ASX_SLOT_NONE;
static uint32_t g_obligation_free_head = ASX_SLOT_NONE;

/* Obligation leak policy (runtime config) and cumulative leak count. */
static asx_leak_response g_leak_response = ASX_LEAK_LOG;
static asx_leak_escalation_config g_leak_escalation;
static int g_leak_escalation_set = 0;
static uint64_t g_leak_count = 0;

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

void asx_runtime_reset(void) {
    uint32_t i;
    uint32_t j;
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        g_regions[i].state = ASX_REGION_OPEN;
        g_regions[i].parent_id = ASX_INVALID_ID;
        g_regions[i].child_count = 0;
        for (j = 0; j < ASX_MAX_REGION_CHILDREN; j++) { g_regions[i].children[j] = ASX_INVALID_ID; }
        g_regions[i].task_count = 0;
        g_regions[i].task_total = 0;
        g_regions[i].generation = 0;
        g_regions[i].alive = 0;
        asx_cleanup_init(&g_regions[i].cleanup);
        g_regions[i].budget = asx_budget_infinite();
        g_regions[i].capture_used = 0;
    }
    g_region_count = 0;
    g_region_live = 0;
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        g_tasks[i].state = ASX_TASK_CREATED;
        g_tasks[i].region = ASX_INVALID_ID;
        g_tasks[i].poll_fn = NULL;
        g_tasks[i].user_data = NULL;
        g_tasks[i].outcome = asx_outcome_make(ASX_OUTCOME_OK);
        g_tasks[i].generation = 0;
        g_tasks[i].alive = 0;
        g_tasks[i].captured_state = NULL;
        g_tasks[i].captured_size = 0;
        g_tasks[i].captured_dtor = NULL;
        g_tasks[i].cancel_phase = 0;
        g_tasks[i].cancel_pending = 0;
        g_tasks[i].cancel_epoch = 0;
        g_tasks[i].cleanup_polls_remaining = 0;
        g_tasks[i].detached = 0;
        g_tasks[i].next_free = ASX_SLOT_NONE;
        g_tasks[i].budget = asx_budget_infinite();
        g_tasks[i].first_held = ASX_SLOT_NONE;
        asx_task_sched_init_internal(&g_tasks[i]);
        memset(&g_tasks[i].cancel_reason, 0, sizeof(g_tasks[i].cancel_reason));
    }
    g_task_count = 0;
    g_task_live = 0;
    g_task_free_head = ASX_SLOT_NONE;
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        g_obligations[i].state = ASX_OBLIGATION_RESERVED;
        g_obligations[i].region = ASX_INVALID_ID;
        g_obligations[i].generation = 0;
        g_obligations[i].alive = 0;
        g_obligations[i].next_free = ASX_SLOT_NONE;
        g_obligations[i].kind = ASX_OBLIGATION_KIND_GENERIC;
        g_obligations[i].holder = ASX_INVALID_ID;
        g_obligations[i].next_held = ASX_SLOT_NONE;
        g_obligations[i].abort_reason = ASX_OBLIGATION_ABORT_NONE;
    }
    g_obligation_count = 0;
    g_obligation_live = 0;
    g_obligation_free_head = ASX_SLOT_NONE;
    g_leak_response = ASX_LEAK_LOG;
    g_leak_escalation_set = 0;
    g_leak_count = 0;

    /* Reset ghost safety monitors */
    asx_ghost_reset();
    asx_scheduler_event_reset();
    asx_scheduler_reset_internal();
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
    asx_trace_reset();
    asx_telemetry_reset();
    asx_hindsight_reset();
    asx_adapter_reset_all();
    asx_fault_clear();
    asx_runtime_hooks_reset_internal();
    asx_event_log_reset();
    asx_error_ledger_reset();
    asx_notify_reset();
    asx_semaphore_reset();
    asx_barrier_reset();
    asx_once_reset();
    asx_actor_reset();
    asx_supervisor_reset();
    asx_net_reset();
#if ASX_HAS_NATIVE_RUNTIME_SURFACES
    asx_fs_reset();
    asx_process_reset();
    asx_signal_reset();
#endif
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
    uint16_t tag, slot_idx, handle_gen;

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    tag = asx_handle_type_tag(id);
    if (tag != ASX_TYPE_REGION) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_REGIONS) return ASX_E_NOT_FOUND;
    if (!g_regions[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != g_regions[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &g_regions[slot_idx];
    return ASX_OK;
}

asx_status asx_task_slot_lookup(asx_task_id id, asx_task_slot **out) {
    uint16_t tag, slot_idx, handle_gen;

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    tag = asx_handle_type_tag(id);
    if (tag != ASX_TYPE_TASK) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_TASKS) return ASX_E_NOT_FOUND;
    if (!g_tasks[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != g_tasks[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &g_tasks[slot_idx];
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
    return (void *)&region->capture_arena[start];
}

/* -------------------------------------------------------------------
 * Slot reclamation
 *
 * Task and obligation slots move between "live" and a LIFO free list.
 * Releasing a slot bumps its generation, so every outstanding handle to
 * the old occupant fails closed (ASX_E_STALE_HANDLE / ASX_E_NOT_FOUND).
 * ------------------------------------------------------------------- */

asx_task_id asx_task_handle_for_slot(uint32_t slot_idx) {
    const asx_task_slot *t = &g_tasks[slot_idx];
    return asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)t->state),
                           asx_handle_pack_index(t->generation, (uint16_t)slot_idx));
}

static void asx_task_slot_release(uint32_t idx) {
    asx_task_slot *t = &g_tasks[idx];

    if (!t->alive) return;
    asx_task_release_capture_internal(t);
    t->alive = 0;
    t->generation++;
    t->detached = 0;
    t->poll_fn = NULL;
    t->user_data = NULL;
    t->region = ASX_INVALID_ID;
    t->cancel_pending = 0;
    t->next_free = g_task_free_head;
    g_task_free_head = idx;
    if (g_task_live > 0u) g_task_live--;
}

/* Resolve a holder handle to its live task slot (NULL if gone). */
static asx_task_slot *asx_obligation_holder_slot(asx_task_id holder) {
    asx_task_slot *t;
    if (holder == ASX_INVALID_ID) return NULL;
    if (asx_task_slot_lookup(holder, &t) != ASX_OK) return NULL;
    return t;
}

/* Remove obligation `idx` from its holder's held list (if linked). */
static void asx_obligation_unlink_holder(uint32_t idx) {
    asx_obligation_slot *o = &g_obligations[idx];
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
        link = &g_obligations[*link].next_held;
    }
    o->next_held = ASX_SLOT_NONE;
}

static void asx_obligation_slot_release(uint32_t idx) {
    asx_obligation_slot *o = &g_obligations[idx];

    if (!o->alive) return;
    if (o->state == ASX_OBLIGATION_RESERVED) asx_obligation_unlink_holder(idx);
    o->holder = ASX_INVALID_ID;
    o->alive = 0;
    o->generation++;
    o->region = ASX_INVALID_ID;
    o->next_free = g_obligation_free_head;
    g_obligation_free_head = idx;
    if (g_obligation_live > 0u) g_obligation_live--;
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
static uint32_t asx_task_reclaim_closed(void) {
    uint32_t i;
    uint32_t freed = 0;

    for (i = g_task_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        asx_task_slot *t = &g_tasks[i - 1u];
        if (!t->alive || !asx_task_is_terminal(t->state)) continue;
        if (!asx_region_records_reclaimable(t->region)) continue;
        /* Released in descending order so the LIFO free list hands the
         * lowest slot index out first. */
        asx_task_slot_release(i - 1u);
        freed++;
    }
    return freed;
}

/* Allocation-pressure reclamation for obligations: free every resolved
 * (committed/aborted/leaked) obligation, in ascending slot order. */
static uint32_t asx_obligation_reclaim_resolved(void) {
    uint32_t i;
    uint32_t freed = 0;

    for (i = g_obligation_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: g_obligation_count <= ASX_MAX_OBLIGATIONS");
        asx_obligation_slot *o = &g_obligations[i - 1u];
        if (!o->alive || o->state == ASX_OBLIGATION_RESERVED) continue;
        asx_obligation_slot_release(i - 1u);
        freed++;
    }
    return freed;
}

static asx_status asx_task_slot_alloc(uint32_t *out_idx) {
    if (g_task_free_head == ASX_SLOT_NONE && g_task_count >= ASX_MAX_TASKS) {
        (void)asx_task_reclaim_closed();
    }
    if (g_task_free_head != ASX_SLOT_NONE) {
        *out_idx = g_task_free_head;
        g_task_free_head = g_tasks[*out_idx].next_free;
    } else if (g_task_count < ASX_MAX_TASKS) {
        *out_idx = g_task_count++;
    } else {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    g_tasks[*out_idx].next_free = ASX_SLOT_NONE;
    g_task_live++;
    return ASX_OK;
}

static asx_status asx_obligation_slot_alloc(uint32_t *out_idx) {
    if (g_obligation_free_head == ASX_SLOT_NONE && g_obligation_count >= ASX_MAX_OBLIGATIONS) {
        (void)asx_obligation_reclaim_resolved();
    }
    if (g_obligation_free_head != ASX_SLOT_NONE) {
        *out_idx = g_obligation_free_head;
        g_obligation_free_head = g_obligations[*out_idx].next_free;
    } else if (g_obligation_count < ASX_MAX_OBLIGATIONS) {
        *out_idx = g_obligation_count++;
    } else {
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    g_obligations[*out_idx].next_free = ASX_SLOT_NONE;
    g_obligation_live++;
    return ASX_OK;
}

/* Free every task and obligation record still bound to a region whose
 * slot is about to be recycled. */
static void asx_region_release_records(asx_region_id region) {
    uint32_t i;
    uint32_t key = asx_handle_index(region); /* slot + generation */

    for (i = g_task_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        if (g_tasks[i - 1u].alive && asx_handle_index(g_tasks[i - 1u].region) == key) {
            asx_task_slot_release(i - 1u);
        }
    }
    for (i = g_obligation_count; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: g_obligation_count <= ASX_MAX_OBLIGATIONS");
        if (g_obligations[i - 1u].alive && asx_handle_index(g_obligations[i - 1u].region) == key) {
            asx_obligation_slot_release(i - 1u);
        }
    }
}

void asx_runtime_set_leak_policy_internal(asx_leak_response response,
                                          const asx_leak_escalation_config *escalation) {
    g_leak_response = response;
    if (escalation != NULL) {
        g_leak_escalation = *escalation;
        g_leak_escalation_set = 1;
    } else {
        g_leak_escalation_set = 0;
    }
}

uint64_t asx_obligation_leak_count(void) { return g_leak_count; }

static asx_leak_response asx_leak_policy_effective(void) {
    if (g_leak_escalation_set && g_leak_count >= g_leak_escalation.threshold) {
        return g_leak_escalation.escalate_to;
    }
    return g_leak_response;
}

uint32_t asx_task_resolve_held_obligations_internal(asx_task_slot *task, int *out_fail_fast) {
    int cancelled = asx_outcome_severity_of(&task->outcome) == ASX_OUTCOME_CANCELLED;
    uint32_t leaks = 0;
    uint32_t idx = task->first_held;

    if (out_fail_fast != NULL) *out_fail_fast = 0;
    task->first_held = ASX_SLOT_NONE;
    while (idx != ASX_SLOT_NONE) {
        ASX_CHECKPOINT_WAIVER("bounded: held list length <= ASX_MAX_OBLIGATIONS");
        asx_obligation_slot *o = &g_obligations[idx];
        uint32_t next = o->next_held;
        asx_obligation_id oid =
            asx_handle_pack(ASX_TYPE_OBLIGATION,
                            (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                            asx_handle_pack_index(o->generation, (uint16_t)idx));

        o->next_held = ASX_SLOT_NONE;
        if (o->alive && o->state == ASX_OBLIGATION_RESERVED) {
            asx_leak_response policy = asx_leak_policy_effective();
            if (cancelled || policy == ASX_LEAK_RECOVER) {
                /* Orphaned by cancellation (or recovered leak): abort. */
                o->state = ASX_OBLIGATION_ABORTED;
                o->abort_reason =
                    cancelled ? ASX_OBLIGATION_ABORT_CANCEL : ASX_OBLIGATION_ABORT_LEAK_RECOVERED;
                asx_ghost_obligation_resolved(oid);
                (void)asx_event_emit(ASX_EVENT_OBLIGATION_ABORT, oid, 0u, ASX_OK);
                asx_trace_emit(ASX_TRACE_OBLIGATION_ABORT, oid, 0);
                if (!cancelled) {
                    if (g_leak_count < UINT64_MAX) g_leak_count++;
                    leaks++;
                }
            } else {
                o->state = ASX_OBLIGATION_LEAKED;
                /* Vocabulary obligation.leaked (Rust ObligationLeak). */
                asx_trace_emit(ASX_TRACE_OBLIGATION_LEAK, oid, 0);
                if (g_leak_count < UINT64_MAX) g_leak_count++;
                leaks++;
                if (policy == ASX_LEAK_LOG) {
                    (void)asx_runtime_log_write(
                        ASX_LOG_WARN, "obligation leaked: holder task completed with it reserved");
                } else if (policy == ASX_LEAK_PANIC && out_fail_fast != NULL) {
                    *out_fail_fast = 1;
                }
            }
        }
        idx = next;
    }
    return leaks;
}

/* Fault raised during completion bookkeeping (PANIC leak policy) that the
 * scheduler must report, mirroring a failing poll under FAIL_FAST. */
static asx_status g_pending_fault = ASX_OK;

asx_status asx_runtime_take_pending_fault_internal(void) {
    asx_status st = g_pending_fault;
    g_pending_fault = ASX_OK;
    return st;
}

void asx_task_on_complete_internal(asx_task_slot *task, asx_region_slot *region) {
    if (task->first_held != ASX_SLOT_NONE) {
        int fail_fast = 0;
        (void)asx_task_resolve_held_obligations_internal(task, &fail_fast);
        if (fail_fast && region != NULL) {
            /* PANIC policy: route the leak through the region's
             * containment policy (fail-fast / poison / error-only). */
            asx_status fc = asx_region_contain_fault(task->region, ASX_E_UNRESOLVED_OBLIGATIONS);
            if (fc != ASX_OK && asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
                g_pending_fault = fc;
            }
        }
    }
    asx_task_timer_disarm_internal(task);
    asx_task_join_detach_internal(task);
    asx_task_join_wake_waiters_internal(task);
    task->parked = 0;
    asx_task_release_capture_internal(task);
    if (region != NULL && region->task_count > 0u) region->task_count--;
    if (task->detached) { asx_task_slot_release((uint32_t)(task - g_tasks)); }
}

void asx_region_settle_internal(asx_region_id rid) {
    asx_region_slot *region;
    if (asx_region_slot_lookup(rid, &region) != ASX_OK) return;
    /* The last task of a closing region lets it finalize. */
    if (region->task_count == 0u && region->state != ASX_REGION_OPEN) {
        asx_region_advance_internal(rid);
    }
}

asx_region_id asx_region_handle_for_slot(uint32_t slot_idx) {
    return asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                           asx_handle_pack_index(g_regions[slot_idx].generation,
                                                 (uint16_t)slot_idx));
}

uint32_t asx_region_subtree_internal(asx_region_id root, uint32_t *out_slots, uint32_t max) {
    asx_region_slot *r;
    uint32_t head = 0;
    uint32_t count = 0;

    if (out_slots == NULL || max == 0u) return 0u;
    if (asx_region_slot_lookup(root, &r) != ASX_OK) return 0u;

    out_slots[count++] = asx_handle_slot(root);
    /* Breadth-first over the bounded children[] lists: parents always
     * precede their descendants in the output. */
    while (head < count) {
        ASX_CHECKPOINT_WAIVER("bounded: count <= max <= ASX_MAX_REGIONS");
        asx_region_slot *cur = &g_regions[out_slots[head++]];
        uint32_t c;
        for (c = 0; c < cur->child_count && count < max; c++) {
            ASX_CHECKPOINT_WAIVER("bounded: child_count <= ASX_MAX_REGION_CHILDREN");
            asx_region_slot *child;
            if (asx_region_slot_lookup(cur->children[c], &child) != ASX_OK) continue;
            out_slots[count++] = asx_handle_slot(cur->children[c]);
        }
    }
    return count;
}

/* -------------------------------------------------------------------
 * Region lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_region_open(asx_region_id *out_id) {
    uint32_t idx;
    int reclaim;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Scan for a recyclable slot: unused (alive=0) or CLOSED with no tasks.
     * When ASX_DEBUG_QUARANTINE is defined, CLOSED slots are never recycled
     * so that any stale-handle dereference surfaces as RESOURCE_EXHAUSTED
     * instead of silently aliasing a new region. Zero-cost when disabled. */
    reclaim = 0;
    for (idx = 0; idx < ASX_MAX_REGIONS; idx++) {
        if (!g_regions[idx].alive) break;
#ifndef ASX_DEBUG_QUARANTINE
        if (g_regions[idx].state == ASX_REGION_CLOSED && g_regions[idx].task_count == 0) {
            reclaim = 1;
            break;
        }
#endif
    }
    if (idx >= ASX_MAX_REGIONS) return ASX_E_RESOURCE_EXHAUSTED;

    /* Increment generation on slot reclaim to invalidate stale handles.
     * The recycled region's completed tasks and resolved obligations go
     * with it: their handles become stale together with the region's. */
    if (reclaim) {
        asx_region_release_records(
            asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                            asx_handle_pack_index(g_regions[idx].generation, (uint16_t)idx)));
        g_regions[idx].generation++;
    } else {
        g_region_live++;
    }

    g_regions[idx].state = ASX_REGION_OPEN;
    g_regions[idx].parent_id = ASX_INVALID_ID;
    g_regions[idx].child_count = 0;
    for (uint32_t child_idx = 0; child_idx < ASX_MAX_REGION_CHILDREN; child_idx++) {
        g_regions[idx].children[child_idx] = ASX_INVALID_ID;
    }
    g_regions[idx].task_count = 0;
    g_regions[idx].task_total = 0;
    g_regions[idx].alive = 1;
    g_regions[idx].poisoned = 0;
    asx_cleanup_init(&g_regions[idx].cleanup);
    g_regions[idx].budget = asx_budget_infinite();
    g_regions[idx].capture_used = 0;
    g_regions[idx].cancel_requested = 0;
    g_regions[idx].cancel_reason.kind = ASX_CANCEL_USER;
    g_regions[idx].cancel_reason.origin_region = ASX_INVALID_ID;
    g_regions[idx].cancel_reason.origin_task = ASX_INVALID_ID;
    g_regions[idx].cancel_reason.timestamp = 0;
    g_regions[idx].cancel_reason.message = NULL;
    g_regions[idx].cancel_reason.cause = NULL;
    g_regions[idx].cancel_reason.truncated = 0;

    if (idx >= g_region_count) { g_region_count = idx + 1; }

    *out_id = asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                              asx_handle_pack_index(g_regions[idx].generation, (uint16_t)idx));

    (void)asx_event_emit(ASX_EVENT_REGION_OPEN, *out_id, 0u, ASX_OK);
    asx_trace_emit(ASX_TRACE_REGION_OPEN, *out_id, 0);
    return ASX_OK;
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
    if (parent_slot->child_count >= ASX_MAX_REGION_CHILDREN) return ASX_E_RESOURCE_EXHAUSTED;

    st = asx_region_open(out_child);
    if (st != ASX_OK) return st;

    st = asx_region_slot_lookup(*out_child, &child_slot);
    if (st != ASX_OK) return st;

    child_slot->parent_id = parent;
    child_slot->budget = parent_slot->budget; /* children inherit the budget */
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

asx_status asx_region_get_budget(asx_region_id id, asx_budget *out) {
    asx_region_slot *r;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    *out = r->budget;
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
    asx_trace_emit(ASX_TRACE_REGION_CLOSE, id, 0);
    return ASX_OK;
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

asx_status asx_task_spawn(asx_region_id region, asx_task_poll_fn poll_fn, void *user_data,
                          asx_task_id *out_id) {
    asx_region_slot *r;
    asx_status st;
    uint32_t idx;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Finalizing regions may still spawn cleanup tasks; broader admissions
     * like obligations remain OPEN-only. A rejected spawn is Rust's
     * SpawnError::RegionClosed ("closed or draining"; vocabulary §5). */
    if (!asx_region_can_accept_work(r->state)) return ASX_E_REGION_CLOSED;

    st = asx_task_slot_alloc(&idx);
    if (st != ASX_OK) return st;

    g_tasks[idx].state = ASX_TASK_CREATED;
    g_tasks[idx].region = region;
    g_tasks[idx].poll_fn = poll_fn;
    g_tasks[idx].user_data = user_data;
    g_tasks[idx].outcome = asx_outcome_make(ASX_OUTCOME_OK);
    g_tasks[idx].alive = 1;
    g_tasks[idx].detached = 0;
    g_tasks[idx].budget = r->budget;
    g_tasks[idx].first_held = ASX_SLOT_NONE;
    asx_task_sched_init_internal(&g_tasks[idx]);
    g_tasks[idx].captured_state = NULL;
    g_tasks[idx].captured_size = 0;
    g_tasks[idx].captured_dtor = NULL;
    g_tasks[idx].cancel_phase = 0;
    g_tasks[idx].cancel_pending = 0;
    g_tasks[idx].cancel_epoch = 0;
    g_tasks[idx].cleanup_polls_remaining = 0;
    memset(&g_tasks[idx].cancel_reason, 0, sizeof(g_tasks[idx].cancel_reason));

    r->task_count++;
    r->task_total++;

    *out_id = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                              asx_handle_pack_index(g_tasks[idx].generation, (uint16_t)idx));

    (void)asx_event_emit(ASX_EVENT_TASK_SPAWN, *out_id, (uint64_t)region, ASX_OK);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, *out_id, (uint64_t)region);
    return ASX_OK;
}

asx_status asx_task_spawn_captured(asx_region_id region, asx_task_poll_fn poll_fn,
                                   uint32_t state_size, asx_task_state_dtor_fn state_dtor,
                                   asx_task_id *out_id, void **out_state) {
    asx_region_slot *r;
    asx_task_slot *t;
    asx_status st;
    void *captured;
    uint32_t old_capture_used;

    if (out_id == NULL || out_state == NULL) return ASX_E_INVALID_ARGUMENT;
    if (poll_fn == NULL || state_size == 0u) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (!asx_region_can_accept_work(r->state)) return ASX_E_REGION_CLOSED;

    captured = asx_region_capture_alloc(r, state_size, &old_capture_used);
    if (captured == NULL) return ASX_E_RESOURCE_EXHAUSTED;
    memset(captured, 0, state_size);

    st = asx_task_spawn(region, poll_fn, captured, out_id);
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
    asx_task_slot_release((uint32_t)(t - g_tasks));
    return ASX_OK;
}

asx_status asx_task_detach(asx_task_id id) {
    asx_task_slot *t;
    asx_status st;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    t->detached = 1;
    if (asx_task_is_terminal(t->state)) asx_task_slot_release((uint32_t)(t - g_tasks));
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
    if (budget != NULL) t->budget = asx_budget_meet(&t->budget, budget);
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

asx_status asx_task_consume_cost(asx_task_id self, uint64_t cost) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (asx_budget_consume_cost(&t->budget, cost)) return ASX_OK;
    /* Cost quota cannot cover the charge: COST_BUDGET cancellation. */
    st = asx_task_cancel_budget_internal(self, ASX_CANCEL_COST_BUDGET, asx_cancel_now_internal());
    (void)st;
    return ASX_E_COST_QUOTA_EXHAUSTED;
}

/* -------------------------------------------------------------------
 * Obligation lifecycle
 * ------------------------------------------------------------------- */

asx_status asx_obligation_slot_lookup(asx_obligation_id id, asx_obligation_slot **out) {
    uint16_t tag, slot_idx, handle_gen;

    *out = NULL;
    if (!asx_handle_is_valid(id)) return ASX_E_NOT_FOUND;

    tag = asx_handle_type_tag(id);
    if (tag != ASX_TYPE_OBLIGATION) return ASX_E_NOT_FOUND;

    slot_idx = asx_handle_slot(id);
    if (slot_idx >= ASX_MAX_OBLIGATIONS) return ASX_E_NOT_FOUND;
    if (!g_obligations[slot_idx].alive) return ASX_E_NOT_FOUND;

    handle_gen = asx_handle_generation(id);
    if (handle_gen != g_obligations[slot_idx].generation) return ASX_E_STALE_HANDLE;

    *out = &g_obligations[slot_idx];
    return ASX_OK;
}

static asx_status asx_obligation_reserve_impl(asx_region_id region, asx_obligation_kind kind,
                                              asx_task_slot *holder_slot, asx_task_id holder,
                                              asx_obligation_id *out_id) {
    asx_region_slot *r;
    asx_status st;
    uint32_t idx;

    if (out_id == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;

    /* Only open regions can reserve obligations (Rust
     * ObligationAdmissionError::RegionClosed; vocabulary §5). */
    if (!asx_region_can_spawn(r->state)) return ASX_E_REGION_CLOSED;

    st = asx_obligation_slot_alloc(&idx);
    if (st != ASX_OK) return st;

    g_obligations[idx].state = ASX_OBLIGATION_RESERVED;
    g_obligations[idx].region = region;
    g_obligations[idx].alive = 1;
    g_obligations[idx].kind = kind;
    g_obligations[idx].abort_reason = ASX_OBLIGATION_ABORT_NONE;
    g_obligations[idx].next_held = ASX_SLOT_NONE;
    g_obligations[idx].holder = ASX_INVALID_ID;
    if (holder_slot != NULL) {
        /* Link into the holder's held list (resolved at its completion). */
        g_obligations[idx].holder = holder;
        g_obligations[idx].next_held = holder_slot->first_held;
        holder_slot->first_held = idx;
    }

    *out_id =
        asx_handle_pack(ASX_TYPE_OBLIGATION, (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                        asx_handle_pack_index(g_obligations[idx].generation, (uint16_t)idx));

    /* Ghost linearity monitor: track obligation reservation */
    asx_ghost_obligation_reserved(*out_id);

    (void)asx_event_emit(ASX_EVENT_OBLIGATION_CREATE, *out_id, (uint64_t)region, ASX_OK);
    asx_trace_emit(ASX_TRACE_OBLIGATION_RESERVE, *out_id, (uint64_t)region);
    return ASX_OK;
}

asx_status asx_obligation_reserve(asx_region_id region, asx_obligation_id *out_id) {
    /* Implicit holder: the task being polled, if any and still live. */
    asx_task_id holder = asx_task_current();
    asx_task_slot *t = NULL;

    if (holder != ASX_INVALID_ID &&
        (asx_task_slot_lookup(holder, &t) != ASX_OK || asx_task_is_terminal(t->state))) {
        t = NULL;
    }
    return asx_obligation_reserve_impl(region, ASX_OBLIGATION_KIND_GENERIC, t,
                                       t != NULL ? holder : ASX_INVALID_ID, out_id);
}

asx_status asx_obligation_reserve_ex(asx_region_id region, asx_obligation_kind kind,
                                     asx_task_id holder, asx_obligation_id *out_id) {
    asx_task_slot *t = NULL;
    asx_status st;

    if ((int)kind < (int)ASX_OBLIGATION_KIND_GENERIC ||
        (int)kind > (int)ASX_OBLIGATION_KIND_TRANSACTION) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (holder != ASX_INVALID_ID) {
        st = asx_task_slot_lookup(holder, &t);
        if (st != ASX_OK) return st;
        if (asx_task_is_terminal(t->state)) return ASX_E_INVALID_STATE;
    }
    return asx_obligation_reserve_impl(region, kind, t, holder, out_id);
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
    return ASX_OK;
}

asx_status asx_obligation_commit(asx_obligation_id id) {
    asx_obligation_slot *o;
    asx_status st;

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    /* Ghost protocol monitor: validate obligation transition */
    (void)asx_ghost_check_obligation_transition(id, o->state, ASX_OBLIGATION_COMMITTED);

    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_COMMITTED);
    if (st != ASX_OK) return st;

    asx_obligation_unlink_holder((uint32_t)(o - g_obligations));
    o->state = ASX_OBLIGATION_COMMITTED;

    /* Ghost linearity monitor: track obligation resolution */
    asx_ghost_obligation_resolved(id);

    (void)asx_event_emit(ASX_EVENT_OBLIGATION_COMMIT, id, 0u, ASX_OK);
    asx_trace_emit(ASX_TRACE_OBLIGATION_COMMIT, id, 0);
    return ASX_OK;
}

asx_status asx_obligation_abort(asx_obligation_id id) {
    return asx_obligation_abort_with_reason(id, ASX_OBLIGATION_ABORT_EXPLICIT);
}

asx_status asx_obligation_abort_with_reason(asx_obligation_id id,
                                            asx_obligation_abort_reason reason) {
    asx_obligation_slot *o;
    asx_status st;

    /* The reasons a caller may give (Rust ObligationAbortReason); NONE and
     * LEAK_RECOVERED are the runtime's own. */
    if (reason != ASX_OBLIGATION_ABORT_EXPLICIT && reason != ASX_OBLIGATION_ABORT_CANCEL &&
        reason != ASX_OBLIGATION_ABORT_ERROR) {
        return ASX_E_INVALID_ARGUMENT;
    }

    st = asx_obligation_slot_lookup(id, &o);
    if (st != ASX_OK) return st;

    /* Ghost protocol monitor: validate obligation transition */
    (void)asx_ghost_check_obligation_transition(id, o->state, ASX_OBLIGATION_ABORTED);

    st = asx_obligation_transition_check(o->state, ASX_OBLIGATION_ABORTED);
    if (st != ASX_OK) return st;

    asx_obligation_unlink_holder((uint32_t)(o - g_obligations));
    o->state = ASX_OBLIGATION_ABORTED;
    o->abort_reason = reason;

    /* Ghost linearity monitor: track obligation resolution */
    asx_ghost_obligation_resolved(id);

    (void)asx_event_emit(ASX_EVENT_OBLIGATION_ABORT, id, 0u, ASX_OK);
    asx_trace_emit(ASX_TRACE_OBLIGATION_ABORT, id, 0);

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
