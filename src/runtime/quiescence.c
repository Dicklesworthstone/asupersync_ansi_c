/*
 * quiescence.c — close/finalize/quiescence driver (walking skeleton)
 *
 * Minimal implementation for bd-ix8.8: region drain and quiescence
 * assertion. Drives region through Close → Drain → Finalize → Closed.
 *
 * Extended obligation tracking, finalizer chains, and leak detection
 * tracked in bd-2cw.1. Semantics from QUIESCENCE_FINALIZATION_INVARIANTS.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/app/report.h>
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/runtime/runtime.h>

static void asx_region_unlink_from_parent(asx_region_id id, asx_region_slot *r) {
    asx_region_slot *parent = NULL;
    asx_status st;
    uint32_t i;

    if (r->parent_id == ASX_INVALID_ID) return;

    st = asx_region_slot_lookup(r->parent_id, &parent);
    if (st == ASX_OK) {
        for (i = 0; i < parent->child_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded scan over parent's child array") */
            if (parent->children[i] != id) continue;

            parent->child_count--;
            parent->children[i] = parent->children[parent->child_count];
            parent->children[parent->child_count] = ASX_INVALID_ID;
            break;
        }
    } else {
        (void)asx_runtime_log_write(ASX_LOG_WARN,
                                    "child region closed after parent slot became unavailable");
    }

    r->parent_id = ASX_INVALID_ID;
}

static asx_status asx_region_obligations_resolved(asx_region_id id) {
    uint32_t i;

    for (i = 0; i < g_obligation_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_obligation_count <= MAX_OBLIGATIONS");
        if (!g_obligations[i].alive) continue;
        if (g_obligations[i].region != id) continue;
        if (g_obligations[i].state == ASX_OBLIGATION_RESERVED) {
            return ASX_E_OBLIGATIONS_UNRESOLVED;
        }
    }

    return ASX_OK;
}

static int asx_region_has_uncancelled_tasks(asx_region_id id) {
    uint32_t i;

    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= MAX_TASKS");
        asx_task_slot *t = &g_tasks[i];
        if (!t->alive) continue;
        if (t->region != id) continue;
        if (asx_task_is_terminal(t->state)) continue;
        if (!t->cancel_pending) return 1;
    }

    return 0;
}

/* -------------------------------------------------------------------
 * Drain progress snapshot (bd-1eqo.5.2)
 * ------------------------------------------------------------------- */

static void fill_drain_progress(asx_region_id id, asx_region_slot *r, asx_drain_progress *out) {
    uint32_t i;

    out->region_state = r->state;
    out->tasks_total = r->task_total;
    out->tasks_live = r->task_count;
    out->tasks_completed = r->task_total - r->task_count;
    out->poisoned = r->poisoned;
    out->cleanup_drained = (asx_cleanup_pending(&r->cleanup) == 0) ? 1 : 0;

    /* Count cancelled tasks */
    out->tasks_cancelled = 0;
    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= MAX_TASKS");
        asx_task_slot *t = &g_tasks[i];
        if (!t->alive) continue;
        if (t->region != id) continue;
        if (t->cancel_pending) out->tasks_cancelled++;
    }

    /* Count obligations */
    out->obligations_total = 0;
    out->obligations_reserved = 0;
    out->obligations_resolved = 0;
    for (i = 0; i < g_obligation_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_obligation_count <= MAX_OBLIGATIONS");
        if (!g_obligations[i].alive) continue;
        if (g_obligations[i].region != id) continue;
        out->obligations_total++;
        if (g_obligations[i].state == ASX_OBLIGATION_RESERVED) {
            out->obligations_reserved++;
        } else {
            out->obligations_resolved++;
        }
    }
}

asx_status asx_region_drain_progress(asx_region_id id, asx_drain_progress *out) {
    asx_region_slot *r;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    fill_drain_progress(id, r, out);
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Quiescence check
 * ------------------------------------------------------------------- */

asx_status asx_quiescence_check(asx_region_id id) {
    asx_region_slot *r;
    asx_status st;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    /* Quiescent iff region is CLOSED and no live tasks remain */
    if (r->state != ASX_REGION_CLOSED) { return ASX_E_QUIESCENCE_NOT_REACHED; }
    if (r->task_count > 0) { return ASX_E_QUIESCENCE_TASKS_LIVE; }
    if (asx_cleanup_pending(&r->cleanup) != 0u) { return ASX_E_QUIESCENCE_NOT_REACHED; }

    return asx_region_obligations_resolved(id);
}

int asx_region_is_quiescent(asx_region_id id) {
    asx_quiescence_report report;

    if (asx_quiescence_check_detailed(id, &report) != ASX_OK) return 0;
    return report.quiescent;
}

/* -------------------------------------------------------------------
 * Detailed quiescence check with Q1-Q4 decomposition (bd-1eqo.5.2)
 * ------------------------------------------------------------------- */

asx_status asx_quiescence_check_detailed(asx_region_id id, asx_quiescence_report *out) {
    asx_region_slot *r;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    fill_drain_progress(id, r, &out->progress);
    out->region_state = r->state;

    /* Q1: all tasks complete (no live tasks) */
    out->q1_tasks_complete = (r->task_count == 0) ? 1 : 0;

    /* Q2: all child regions closed */
    out->q2_children_closed = (r->child_count == 0u) ? 1 : 0;

    /* Q3: all obligations resolved (no RESERVED obligations) */
    out->q3_obligations_resolved = (out->progress.obligations_reserved == 0) ? 1 : 0;

    /* Q4: cleanup stack fully drained */
    out->q4_cleanup_drained = out->progress.cleanup_drained;

    /* Overall: quiescent iff region is CLOSED and Q1-Q4 all hold */
    out->quiescent =
        (r->state == ASX_REGION_CLOSED && out->q1_tasks_complete && out->q2_children_closed &&
         out->q3_obligations_resolved && out->q4_cleanup_drained)
            ? 1
            : 0;

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Region drain: structured shutdown of a region subtree
 *
 * Drives the region and every descendant through the shutdown sequence:
 *   1. Close: every OPEN region in the subtree → CLOSING (parent-first)
 *   2. Cancel: PARENT cancel reaches every live task in the subtree;
 *      tasks observe it via asx_checkpoint() and get bounded cleanup
 *   3. Run the scheduler over the subtree until all tasks complete
 *   4. Finalize bottom-up (deepest regions first): Closing → [Draining]
 *      → Finalizing → Closed, draining cleanup stacks in LIFO order and
 *      unlinking each region from its parent
 *
 * A parent therefore never closes before its children: structured
 * concurrency by construction. The call is resumable — after a budget
 * exhaustion, call it again to continue.
 * ------------------------------------------------------------------- */

static uint32_t g_drain_slots[ASX_MAX_REGIONS];

static asx_status asx_region_set_state(asx_region_id id, asx_region_slot *r, asx_region_state to) {
    asx_status st;
    (void)asx_ghost_check_region_transition(id, r->state, to);
    st = asx_region_transition_check(r->state, to);
    if (st != ASX_OK) return st;
    r->state = to;
    return ASX_OK;
}

/* Finalize one region whose tasks have all completed. */
static asx_status asx_region_finalize_one(asx_region_id id, asx_region_slot *r) {
    asx_status st;

    if (r->state == ASX_REGION_CLOSED) return ASX_OK;

    if (r->state == ASX_REGION_CLOSING || r->state == ASX_REGION_DRAINING) {
        if (r->child_count > 0u) {
            /* Children still closing: the region waits in DRAINING. */
            if (r->state == ASX_REGION_CLOSING) {
                st = asx_region_set_state(id, r, ASX_REGION_DRAINING);
                if (st != ASX_OK) return st;
            }
            return ASX_E_PENDING;
        }
        st = asx_region_set_state(id, r, ASX_REGION_FINALIZING);
        if (st != ASX_OK) return st;
    }

    if (r->state == ASX_REGION_FINALIZING) {
        st = asx_region_obligations_resolved(id);
        if (st != ASX_OK) return st;

        /* Ghost linearity monitor: check for leaked obligations before close */
        (void)asx_ghost_check_obligation_leaks(id);

        /* Drain cleanup stack in LIFO order before closing. Finalizers are
         * allowed to spawn cleanup tasks in FINALIZING, so we must confirm
         * the region is still task-free before sealing it CLOSED. */
        asx_cleanup_drain(&r->cleanup);
        if (r->task_count > 0u) return ASX_E_QUIESCENCE_TASKS_LIVE;

        st = asx_region_set_state(id, r, ASX_REGION_CLOSED);
        if (st != ASX_OK) return st;
        asx_region_unlink_from_parent(id, r);
        asx_trace_emit(ASX_TRACE_REGION_CLOSED, id, 0);
    }

    return ASX_OK;
}

void asx_region_advance_internal(asx_region_id id) {
    uint32_t depth;
    /* Rust advance_region_state: a closing region whose live work is gone
     * finalizes without anyone draining it, and its closing parent may then
     * finalize too. An Open region is never advanced here: only a close or
     * a cancel starts closing. Bounded by the region tree's height. */
    for (depth = 0; depth < ASX_MAX_REGIONS && id != ASX_INVALID_ID; depth++) {
        asx_region_slot *r;
        asx_region_id parent;
        if (asx_region_slot_lookup(id, &r) != ASX_OK) return;
        if (r->state == ASX_REGION_OPEN || r->state == ASX_REGION_CLOSED) return;
        if (r->poisoned || r->task_count > 0u) return;
        parent = r->parent_id;
        if (asx_region_finalize_one(id, r) != ASX_OK) return; /* children or obligations remain */
        if (r->state != ASX_REGION_CLOSED) return;
        id = parent;
    }
}

asx_status asx_region_drain(asx_region_id id, asx_budget *budget) {
    asx_region_slot *r;
    asx_status st;
    uint32_t n;
    uint32_t i;
    uint32_t live;
    int need_cancel = 0;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    if (r->state == ASX_REGION_CLOSED) return ASX_OK;

    n = asx_region_subtree_internal(id, g_drain_slots, ASX_MAX_REGIONS);

    /* Step 1: close every open region in the subtree (parent-first). */
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        asx_region_slot *rs = &g_regions[g_drain_slots[i]];
        if (rs->state == ASX_REGION_OPEN) {
            st = asx_region_set_state(asx_region_handle_for_slot(g_drain_slots[i]), rs,
                                      ASX_REGION_CLOSING);
            if (st != ASX_OK) return st;
            need_cancel = 1;
        }
        if (rs->task_count > 0u &&
            asx_region_has_uncancelled_tasks(asx_region_handle_for_slot(g_drain_slots[i]))) {
            need_cancel = 1;
        }
    }

    /* Step 2: PARENT cancel reaches every live task in the subtree.
     * Tasks observe cancellation via asx_checkpoint() and have bounded
     * cleanup before forced completion. (bd-2cw.3) */
    if (need_cancel) (void)asx_cancel_propagate(id, ASX_CANCEL_PARENT);

    /* Step 3: run the scheduler over the subtree. */
    live = 0;
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        live += g_regions[g_drain_slots[i]].task_count;
    }
    if (live > 0u) {
        st = asx_scheduler_run(id, budget);
        if (st != ASX_OK) return st;
        for (i = 0; i < n; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
            if (g_regions[g_drain_slots[i]].task_count > 0u) return ASX_E_QUIESCENCE_TASKS_LIVE;
        }
    }

    /* Step 4: finalize bottom-up so children close before parents. */
    for (i = n; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        uint32_t slot = g_drain_slots[i - 1u];
        st = asx_region_finalize_one(asx_region_handle_for_slot(slot), &g_regions[slot]);
        if (st != ASX_OK) return st;
    }

    return ASX_OK;
}
