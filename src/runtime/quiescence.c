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
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/runtime/runtime.h>

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

    return asx_region_obligations_resolved(id);
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

    /* Q2: all child regions closed (walking skeleton: always true) */
    out->q2_children_closed = 1;

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
 * Region drain: scheduler + close protocol
 *
 * Drives the region through its full shutdown sequence:
 *   1. Close (Open → Closing)
 *   2. Run scheduler to completion
 *   3. Drain cleanup stack (Finalizing)
 *   4. Advance (Closing → Draining → Finalizing → Closed)
 * ------------------------------------------------------------------- */

asx_status asx_region_drain(asx_region_id id, asx_budget *budget) {
    asx_region_slot *r;
    asx_status st;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    /* Step 1: Close the region if still open */
    if (r->state == ASX_REGION_OPEN) {
        asx_ghost_check_region_transition(id, ASX_REGION_OPEN, ASX_REGION_CLOSING);
        st = asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_CLOSING;

        /* Propagate PARENT cancel to all active tasks in this region.
         * Tasks observe cancellation via asx_checkpoint() and have
         * bounded cleanup before forced completion. (bd-2cw.3) */
        asx_cancel_propagate(id, ASX_CANCEL_PARENT);
    }

    if (r->state == ASX_REGION_CLOSING && r->task_count > 0 &&
        asx_region_has_uncancelled_tasks(id)) {
        asx_cancel_propagate(id, ASX_CANCEL_PARENT);
    }

    /* Step 2: Run scheduler to drain tasks */
    if (r->task_count > 0) {
        st = asx_scheduler_run(id, budget);
        if (st == ASX_E_POLL_BUDGET_EXHAUSTED) return st;
        if (st != ASX_OK) return st;
        if (r->task_count > 0) { return ASX_E_QUIESCENCE_TASKS_LIVE; }
    }

    /* Step 3: Advance through closing protocol */
    if (r->state == ASX_REGION_CLOSING) {
        /* No children (walking skeleton) — fast path: skip Draining */
        asx_ghost_check_region_transition(id, ASX_REGION_CLOSING, ASX_REGION_FINALIZING);
        st = asx_region_transition_check(ASX_REGION_CLOSING, ASX_REGION_FINALIZING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_FINALIZING;
    }

    if (r->state == ASX_REGION_DRAINING) {
        asx_ghost_check_region_transition(id, ASX_REGION_DRAINING, ASX_REGION_FINALIZING);
        st = asx_region_transition_check(ASX_REGION_DRAINING, ASX_REGION_FINALIZING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_FINALIZING;
    }

    if (r->state == ASX_REGION_FINALIZING) {
        st = asx_region_obligations_resolved(id);
        if (st != ASX_OK) return st;

        /* Ghost linearity monitor: check for leaked obligations before close */
        (void)asx_ghost_check_obligation_leaks(id);

        /* Drain cleanup stack in LIFO order before closing */
        asx_cleanup_drain(&r->cleanup);

        asx_ghost_check_region_transition(id, ASX_REGION_FINALIZING, ASX_REGION_CLOSED);
        st = asx_region_transition_check(ASX_REGION_FINALIZING, ASX_REGION_CLOSED);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_CLOSED;
        asx_trace_emit(ASX_TRACE_REGION_CLOSED, id, 0);
    }

    return ASX_OK;
}
