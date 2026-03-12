/*
 * cancellation.c — cancellation propagation, checkpoints, and bounded cleanup
 *
 * Implements the cancellation protocol:
 *   Running → CancelRequested → Cancelling → Finalizing → Completed
 *
 * Tasks observe cancellation via asx_checkpoint(), which transitions
 * CancelRequested → Cancelling and applies the cleanup budget.
 * The scheduler enforces budget exhaustion and phase completion.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/transition.h>
#include <asx/core/cancel.h>
#include <asx/core/ghost.h>
#include "runtime_internal.h"

static uint64_t asx_trace_task_transition_aux(asx_task_state from,
                                              asx_task_state to)
{
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

/* -------------------------------------------------------------------
 * Task cancel request
 * ------------------------------------------------------------------- */

asx_status asx_task_cancel(asx_task_id id, asx_cancel_kind kind)
{
    asx_task_slot *t;
    asx_status st;
    asx_budget cleanup;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    /* Already cancelled or terminal — strengthen if in cancel phase */
    if (asx_task_is_terminal(t->state)) {
        return ASX_OK; /* no-op for completed tasks */
    }

    if (t->cancel_pending) {
        /* Strengthen: if new cancel is higher severity, upgrade */
        if (asx_cancel_severity(kind) > asx_cancel_severity(t->cancel_reason.kind)) {
            t->cancel_reason.kind = kind;
            cleanup = asx_cancel_cleanup_budget(kind);
            /* Tighten budget: take the minimum polls remaining */
            if (asx_budget_polls(&cleanup) < t->cleanup_polls_remaining) {
                t->cleanup_polls_remaining = asx_budget_polls(&cleanup);
            }
        }
        t->cancel_epoch++;
        return ASX_OK;
    }

    /* First cancel signal — transition Running → CancelRequested */
    if (t->state != ASX_TASK_RUNNING &&
        t->state != ASX_TASK_CREATED) {
        return ASX_E_INVALID_STATE;
    }

    /* If task hasn't been polled yet (Created), move to Running first */
    if (t->state == ASX_TASK_CREATED) {
        asx_task_state from = t->state;
        (void)asx_ghost_check_task_transition(id, t->state, ASX_TASK_RUNNING);
        t->state = ASX_TASK_RUNNING;
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)id,
                       asx_trace_task_transition_aux(from, ASX_TASK_RUNNING));
    }

    {
        asx_task_state from = t->state;
    (void)asx_ghost_check_task_transition(id, t->state, ASX_TASK_CANCEL_REQUESTED);
    t->state = ASX_TASK_CANCEL_REQUESTED;
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)id,
                       asx_trace_task_transition_aux(from,
                                                    ASX_TASK_CANCEL_REQUESTED));
    }

    t->cancel_pending = 1;
    t->cancel_reason.kind = kind;
    t->cancel_reason.origin_region = ASX_INVALID_ID;
    t->cancel_reason.origin_task = ASX_INVALID_ID;
    t->cancel_reason.timestamp = 0;
    t->cancel_reason.message = NULL;
    t->cancel_reason.cause = NULL;
    t->cancel_reason.truncated = 0;
    t->cancel_epoch = 1;

    cleanup = asx_cancel_cleanup_budget(kind);
    t->cleanup_polls_remaining = asx_budget_polls(&cleanup);

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Cancel with origin attribution (for propagation traceability)
 * ------------------------------------------------------------------- */

asx_status asx_task_cancel_with_origin(asx_task_id id,
                                       asx_cancel_kind kind,
                                       asx_region_id origin_region,
                                       asx_task_id origin_task)
{
    asx_task_slot *t;
    asx_status st;
    int was_pending;
    asx_cancel_kind old_kind;

    /* Capture pre-cancel state to decide if origin should be updated */
    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    was_pending = t->cancel_pending;
    old_kind = t->cancel_reason.kind;

    st = asx_task_cancel(id, kind);
    if (st != ASX_OK) return st;

    /* Only set origin if this was the first cancel or if the cancel
     * was actually strengthened (higher severity). Otherwise, the
     * existing stronger cancel's origin attribution is preserved. */
    if (!was_pending ||
        asx_cancel_severity(kind) > asx_cancel_severity(old_kind)) {
        t->cancel_reason.origin_region = origin_region;
        t->cancel_reason.origin_task = origin_task;
    }

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Region-wide propagation
 * ------------------------------------------------------------------- */

uint32_t asx_cancel_propagate(asx_region_id region, asx_cancel_kind kind)
{
    uint32_t i;
    uint32_t count = 0;

    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("kernel-propagation: single-pass cancel sweep bounded by "
                              "g_task_count <= ASX_MAX_TASKS; O(1) per iteration");
        asx_task_slot *t = &g_tasks[i];
        asx_task_id tid;

        if (!t->alive) continue;
        if (t->region != region) continue;
        if (asx_task_is_terminal(t->state)) continue;

        tid = asx_handle_pack(ASX_TYPE_TASK,
                              (uint16_t)(1u << (unsigned)t->state),
                              asx_handle_pack_index(t->generation, (uint16_t)i));

        if (asx_task_cancel_with_origin(tid, kind, region,
                                        ASX_INVALID_ID) == ASX_OK) {
            count++;
        }
    }

    return count;
}

/* -------------------------------------------------------------------
 * Task checkpoint
 * ------------------------------------------------------------------- */

asx_status asx_checkpoint(asx_task_id self, asx_checkpoint_result *out)
{
    asx_task_slot *t;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;

    /* Not cancelled — report clean status */
    if (!t->cancel_pending) {
        out->cancelled = 0;
        out->phase = ASX_CANCEL_PHASE_REQUESTED; /* unused when not cancelled */
        out->polls_remaining = 0;
        out->kind = ASX_CANCEL_USER;
        return ASX_OK;
    }

    /* Transition CancelRequested → Cancelling on first checkpoint */
    if (t->state == ASX_TASK_CANCEL_REQUESTED) {
        asx_task_state from = t->state;
        (void)asx_ghost_check_task_transition(self, t->state, ASX_TASK_CANCELLING);
        t->state = ASX_TASK_CANCELLING;
        t->cancel_phase = ASX_CANCEL_PHASE_CANCELLING;
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)self,
                       asx_trace_task_transition_aux(from, ASX_TASK_CANCELLING));
    }

    out->cancelled = 1;
    out->phase = t->cancel_phase;
    out->polls_remaining = t->cleanup_polls_remaining;
    out->kind = t->cancel_reason.kind;

    /* Budget is decremented by the scheduler after each poll,
     * not here. Checkpoint only observes and transitions phases. */

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Task finalize (Cancelling → Finalizing)
 * ------------------------------------------------------------------- */

asx_status asx_task_finalize(asx_task_id id)
{
    asx_task_slot *t;
    asx_status st;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    if (t->state != ASX_TASK_CANCELLING) {
        return ASX_E_INVALID_STATE;
    }

    {
        asx_task_state from = t->state;
    (void)asx_ghost_check_task_transition(id, t->state, ASX_TASK_FINALIZING);
    t->state = ASX_TASK_FINALIZING;
    t->cancel_phase = ASX_CANCEL_PHASE_FINALIZING;
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)id,
                       asx_trace_task_transition_aux(from, ASX_TASK_FINALIZING));
    }

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Cancel phase query
 * ------------------------------------------------------------------- */

asx_status asx_task_get_cancel_phase(asx_task_id id, asx_cancel_phase *out)
{
    asx_task_slot *t;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    *out = t->cancel_phase;
    return ASX_OK;
}
