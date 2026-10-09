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

#include "runtime_internal.h"
#include <asx/asx.h>
#include <asx/core/cancel.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/runtime/runtime.h>

static uint64_t asx_trace_task_transition_aux(asx_task_state from, asx_task_state to) {
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

/* -------------------------------------------------------------------
 * Task cancel request
 * ------------------------------------------------------------------- */

/* The time a cancel reason is stamped with: the runtime clock, or the
 * virtual clock when no clock hook is installed. */
asx_time asx_cancel_now_internal(void) {
    asx_time now;
    if (asx_runtime_now_ns(&now) != ASX_OK) now = asx_runtime_virtual_now();
    return now;
}

asx_cancel_reason asx_cancel_reason_testing_default(asx_cancel_kind kind, const char *message) {
    asx_cancel_reason r;
    r.kind = kind;
    r.origin_region = asx_region_handle_for_slot(0u);
    r.origin_task = ASX_INVALID_ID;
    r.timestamp = (asx_time)1000000000u;
    r.message = message;
    r.cause = NULL;
    r.truncated = 0;
    return r;
}

void asx_region_trace_cancel_of_gone_internal(asx_region_id region,
                                              const asx_cancel_reason *reason) {
    asx_trace_payload payload;
    payload.text = NULL;
    payload.reason = reason;
    asx_trace_emit_payload_internal(ASX_TRACE_REGION_CANCELLED, region, (uint64_t)reason->kind,
                                    &payload);
}

asx_cancel_reason asx_region_close_reason_internal(void) {
    static const char message[] = "owned child region body finished";
    return asx_cancel_reason_testing_default(ASX_CANCEL_USER, message);
}

static int reason_same(const asx_cancel_reason *a, const asx_cancel_reason *b) {
    return a->kind == b->kind && a->timestamp == b->timestamp &&
           a->origin_region == b->origin_region && a->origin_task == b->origin_task &&
           a->message == b->message && a->cause == b->cause && a->truncated == b->truncated;
}

static int budget_same(const asx_budget *a, const asx_budget *b) {
    return a->deadline == b->deadline && a->poll_quota == b->poll_quota &&
           a->cost_quota == b->cost_quota && a->priority == b->priority;
}

void asx_task_materialize_cancel_internal(asx_task_slot *t) {
    asx_task_id id = asx_task_handle_for_slot((uint32_t)(t - g_tasks));
    if (!t->cancel_unmaterialized) return;
    t->cancel_unmaterialized = 0;
    if (t->state == ASX_TASK_CREATED) {
        (void)asx_ghost_check_task_transition(id, t->state, ASX_TASK_RUNNING);
        t->state = ASX_TASK_RUNNING;
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)id,
                       asx_trace_task_transition_aux(ASX_TASK_CREATED, ASX_TASK_RUNNING));
    }
    if (t->state == ASX_TASK_RUNNING) {
        (void)asx_ghost_check_task_transition(id, t->state, ASX_TASK_CANCEL_REQUESTED);
        t->state = ASX_TASK_CANCEL_REQUESTED;
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)id,
                       asx_trace_task_transition_aux(ASX_TASK_RUNNING, ASX_TASK_CANCEL_REQUESTED));
    }
    t->cancel_epoch++;
    {
        asx_cancel_witness_id witness = ASX_INVALID_ID;
        asx_status w_st_ = asx_cancel_witness_create(&witness, id, &t->cancel_reason);
        (void)w_st_;
        t->cancel_witness = witness;
    }
}

asx_status asx_task_cancel_reason_internal(asx_task_id id, const asx_cancel_reason *reason,
                                           asx_cancel_source source) {
    asx_task_slot *t;
    asx_status st;
    asx_budget cleanup;
    asx_cancel_reason merged;
    asx_budget prior_cleanup = asx_budget_infinite();

    if (reason == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    /* Already cancelled or terminal — strengthen if in cancel phase */
    if (asx_task_is_terminal(t->state)) { return ASX_OK; /* no-op for completed tasks */ }

    /* A budget cancel the task raises itself stays off its record until a
     * checkpoint acknowledges it (Rust sets it on the Cx only; bd-mex3). */
    if (!t->cancel_pending && source == ASX_CANCEL_SRC_BUDGET &&
        (t->state == ASX_TASK_RUNNING || t->state == ASX_TASK_CREATED)) {
        t->cancel_pending = 1;
        t->cancel_unmaterialized = 1;
        /* Raised by the task itself: no cancel waker fires, so idle
         * handling must not wake it either. */
        t->cancel_polled = 1;
        t->cancel_reason = *reason;
        cleanup = asx_cancel_cleanup_budget(reason->kind);
        t->cleanup_budget = cleanup;
        t->cleanup_applied = 0;
        t->cleanup_polls_remaining = asx_budget_polls(&cleanup);
        return ASX_OK;
    }

    /* A request from outside meets such a cancel while the record is still
     * Running: it newly cancels the record, its reason strengthened by the
     * pending one, the cleanup budgets met (request_cancel_with_budget_and_
     * publication, record/task.rs:716-722). */
    if (t->cancel_pending && t->cancel_unmaterialized && source != ASX_CANCEL_SRC_BUDGET) {
        merged = asx_cancel_strengthen(reason, &t->cancel_reason);
        prior_cleanup = t->cleanup_budget;
        reason = &merged;
        t->cancel_pending = 0;
        t->cancel_unmaterialized = 0;
    }

    if (t->cancel_pending) {
        /* Strengthen: the winning reason replaces the current one whole
         * (Rust CancelReason::strengthen). Every request's cleanup budget
         * is met into the task's, whether or not the reason changes, and
         * a task already cleaning up takes the met budget as its budget,
         * its quota refilled (record/task.rs:735-800). Strengthening
         * records no trace event: Rust has no strengthen event
         * (state.rs:7794). */
        asx_cancel_reason winner = asx_cancel_strengthen(&t->cancel_reason, reason);
        asx_budget met;
        int reason_changed = !reason_same(&winner, &t->cancel_reason);
        int budget_changed;
        cleanup = asx_cancel_cleanup_budget(reason->kind);
        met = asx_budget_meet(&t->cleanup_budget, &cleanup);
        budget_changed = !budget_same(&met, &t->cleanup_budget);
        t->cleanup_budget = met;
        if (t->cleanup_applied) t->budget = t->cleanup_budget;
        if (asx_budget_polls(&t->cleanup_budget) < t->cleanup_polls_remaining) {
            t->cleanup_polls_remaining = asx_budget_polls(&t->cleanup_budget);
        }
        t->cancel_reason = winner;
        t->cancel_epoch++;
        /* Lab dispatch: a region cancel or handle abort that changed
         * anything schedules the task on the cancel lane at its request's
         * cleanup priority; a changed reason also reaches its cancel
         * waker, as does a wake a budget cancel left due
         * (record/task.rs:840-866). */
        if (asx_lab_dispatch_active() && source != ASX_CANCEL_SRC_BUDGET) {
            if ((source == ASX_CANCEL_SRC_REGION || source == ASX_CANCEL_SRC_HANDLE) &&
                (reason_changed || budget_changed)) {
                asx_lab_schedule_cancel(t, cleanup.priority);
            }
            if (reason_changed || t->cancel_wakers_pending) {
                t->cancel_wakers_pending = 0u;
                asx_lab_cancel_wake(t);
            }
        }
        return ASX_OK;
    }

    /* First cancel signal — transition Running → CancelRequested */
    if (t->state != ASX_TASK_RUNNING && t->state != ASX_TASK_CREATED) {
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
                       asx_trace_task_transition_aux(from, ASX_TASK_CANCEL_REQUESTED));
    }

    t->cancel_pending = 1;
    t->cancel_polled = 0;
    t->cancel_reason = *reason;
    t->cancel_epoch = 1;

    cleanup = asx_cancel_cleanup_budget(reason->kind);
    /* Met with a merged pending budget cancel's (identity otherwise). */
    t->cleanup_budget = asx_budget_meet(&cleanup, &prior_cleanup);
    t->cleanup_applied = 0;
    t->cleanup_polls_remaining = asx_budget_polls(&t->cleanup_budget);

    /* Cancellation must be observed: a parked task becomes runnable so it
     * can reach a checkpoint and run its bounded cleanup. Under lab
     * dispatch the task goes to the cancel lane at its cleanup priority,
     * then its cancel waker fires (Rust: the driver's or the region's
     * schedule_cancel, then the CancelTaskWaker); a budget cancel the task
     * raised itself schedules nothing. */
    if (asx_lab_dispatch_active()) {
        if (source != ASX_CANCEL_SRC_BUDGET) {
            asx_lab_schedule_cancel(t, cleanup.priority);
            asx_lab_cancel_wake(t);
        }
    } else {
        asx_task_wake_slot_internal(t);
    }

    /* Create a cancel witness to track this cancellation's lifecycle */
    {
        asx_cancel_witness_id witness = ASX_INVALID_ID;
        asx_status w_st_ = asx_cancel_witness_create(&witness, id, &t->cancel_reason);
        (void)w_st_;
        t->cancel_witness = witness;
    }

    /* A newly cancelled task (vocabulary cancel.requested). Rust records it
     * only where a region or policy cancels tasks (cancel_request,
     * state.rs:7864; cancel_sibling_tasks, :7502); a direct task cancel
     * (RuntimeState::cancel_task, :3429) or a handle abort records none. */
    if (source == ASX_CANCEL_SRC_REGION) {
        asx_trace_payload payload;
        payload.text = NULL;
        payload.reason = &t->cancel_reason;
        asx_trace_emit_payload_internal(ASX_TRACE_CANCEL_REQUEST, (uint64_t)id,
                                        (uint64_t)reason->kind, &payload);
    }

    return ASX_OK;
}

asx_status asx_task_cancel(asx_task_id id, asx_cancel_kind kind) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    /* No requester given: the cancel originates at the task's own region. */
    return asx_task_cancel_with_origin(id, kind, t->region, ASX_INVALID_ID);
}

/* -------------------------------------------------------------------
 * Cancel with origin attribution (for propagation traceability)
 * ------------------------------------------------------------------- */

asx_status asx_task_cancel_with_origin(asx_task_id id, asx_cancel_kind kind,
                                       asx_region_id origin_region, asx_task_id origin_task) {
    asx_cancel_reason reason;
    reason.kind = kind;
    reason.origin_region = origin_region;
    reason.origin_task = origin_task;
    reason.timestamp = asx_cancel_now_internal();
    reason.message = NULL;
    reason.cause = NULL;
    reason.truncated = 0;
    return asx_task_cancel_reason_internal(id, &reason, ASX_CANCEL_SRC_DIRECT);
}

asx_status asx_task_cancel_with_reason(asx_task_id id, const asx_cancel_reason *reason) {
    return asx_task_cancel_reason_internal(id, reason, ASX_CANCEL_SRC_DIRECT);
}

asx_status asx_task_abort_request(asx_task_id target, const asx_cancel_reason *reason) {
    asx_task_slot *t;
    asx_status st;
    if (reason == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(target, &t);
    if (st != ASX_OK) return st;
    if (asx_task_is_terminal(t->state)) {
        /* Rust strengthens the target Cx's reason at once
         * (apply_or_defer_cancel_reason, task_handle.rs:518-520): a task
         * that ended cancelled and is not joined yet reports the stronger
         * reason through its join (closed_reason, :1016). */
        if (asx_outcome_severity_of(&t->outcome) == ASX_OUTCOME_CANCELLED) {
            t->cancel_reason = asx_cancel_strengthen(&t->cancel_reason, reason);
        }
        return ASX_OK;
    }
    if (asx_lab_dispatch_active()) return asx_lab_handle_cancel_command(t, reason);
    return asx_task_cancel_reason_internal(target, reason, ASX_CANCEL_SRC_HANDLE);
}

asx_status asx_task_cancel_budget_internal(asx_task_id id, asx_cancel_kind kind, asx_time at) {
    asx_task_slot *t;
    asx_cancel_reason reason;
    asx_status st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    reason.kind = kind;
    reason.origin_region = t->region;
    reason.origin_task = id;
    reason.timestamp = at;
    reason.message = NULL;
    reason.cause = NULL;
    reason.truncated = 0;
    return asx_task_cancel_reason_internal(id, &reason, ASX_CANCEL_SRC_BUDGET);
}

/* -------------------------------------------------------------------
 * Region-wide propagation
 * ------------------------------------------------------------------- */

/* Region subtree scratch: parent-first slot order (see lifecycle.c), and
 * the reason each subtree region's tasks are cancelled with. */
static uint32_t g_propagate_slots[ASX_MAX_REGIONS];
static asx_cancel_reason g_propagate_reasons[ASX_MAX_REGIONS];

/* Cancel every live task of the subtree (rooted at subtree entry 0) with
 * its region's reason from g_propagate_reasons. Under lab dispatch the
 * tasks' cancel wakes fire after all of them are cancelled, as Rust
 * dispatches a region cancel's wakes after the cancel (run.rs driver). */
/* Task slots of the live tasks in a region, in membership order. */
static uint32_t g_member_order[ASX_MAX_TASKS];

static uint32_t cancel_subtree_tasks(uint32_t n) {
    uint32_t r;
    uint32_t i;
    uint32_t count = 0;
    asx_lab_cancel_batch_begin();
    for (r = 0; r < n; r++) {
        uint32_t key = asx_handle_index(asx_region_handle_for_slot(g_propagate_slots[r]));
        uint32_t m = 0;
        uint32_t k;
        /* Rust visits a region's tasks in its Membership's insertion order
         * (record/region.rs:341-350, state.rs:7811-7830), not by slot. */
        for (i = 0; i < g_task_count; i++) {
            ASX_CHECKPOINT_WAIVER("kernel-propagation: single-pass cancel sweep bounded by "
                                  "g_task_count <= ASX_MAX_TASKS; O(1) per iteration");
            asx_task_slot *t = &g_tasks[i];
            uint32_t j;

            if (!t->alive) continue;
            if (asx_handle_index(t->region) != key) continue;
            if (asx_task_is_terminal(t->state)) continue;
            for (j = m; j > 0u && g_tasks[g_member_order[j - 1u]].member_seq > t->member_seq; j--) {
                ASX_CHECKPOINT_WAIVER("bounded: insertion into <= ASX_MAX_TASKS entries");
                g_member_order[j] = g_member_order[j - 1u];
            }
            g_member_order[j] = i;
            m++;
        }
        for (k = 0; k < m; k++) {
            ASX_CHECKPOINT_WAIVER("bounded: m <= ASX_MAX_TASKS");
            if (asx_task_cancel_reason_internal(asx_task_handle_for_slot(g_member_order[k]),
                                                &g_propagate_reasons[r],
                                                ASX_CANCEL_SRC_REGION) == ASX_OK) {
                count++;
            }
        }
    }
    asx_lab_cancel_batch_end();
    return count;
}

/* A descendant's reason (state.rs:7737-7748): ParentCancelled, attributed
 * to its immediate parent, stamped with the request's time, caused by the
 * parent's reason. */
static void parent_cancelled_reason(asx_cancel_reason *out, asx_region_id parent,
                                    const asx_cancel_reason *parent_reason, asx_time timestamp) {
    out->kind = ASX_CANCEL_PARENT;
    out->origin_region = parent;
    out->origin_task = ASX_INVALID_ID;
    out->timestamp = timestamp;
    out->message = NULL;
    out->cause = (asx_cancel_reason *)parent_reason;
    out->truncated = 0;
}

uint32_t asx_cancel_propagate(asx_region_id region, asx_cancel_kind kind) {
    uint32_t r;
    uint32_t n;
    asx_time now = asx_cancel_now_internal();

    /* Task-only structured cancellation: the region's own tasks receive
     * `kind`, attributed to the region; tasks of each descendant region a
     * ParentCancelled reason caused by its parent's. Regions stay open (see
     * asx_region_cancel for the region-level cancel). Parent-first order
     * keeps attribution and trace order deterministic. */
    n = asx_region_subtree_internal(region, g_propagate_slots, ASX_MAX_REGIONS);
    for (r = 0; r < n; r++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        if (r == 0u) {
            g_propagate_reasons[0].kind = kind;
            g_propagate_reasons[0].origin_region = region;
            g_propagate_reasons[0].origin_task = ASX_INVALID_ID;
            g_propagate_reasons[0].timestamp = now;
            g_propagate_reasons[0].message = NULL;
            g_propagate_reasons[0].cause = NULL;
            g_propagate_reasons[0].truncated = 0;
        } else {
            /* No cause chain here: these reasons live in scratch storage
             * that the next propagation reuses. */
            parent_cancelled_reason(&g_propagate_reasons[r],
                                    g_regions[g_propagate_slots[r]].parent_id, NULL, now);
        }
    }
    return cancel_subtree_tasks(n);
}

asx_status asx_region_cancel(asx_region_id region, const asx_cancel_reason *reason,
                             uint32_t *out_cancelled) {
    asx_region_slot *root;
    uint32_t r;
    uint32_t n;
    uint32_t count;
    asx_status st;

    if (reason == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(region, &root);
    if (st != ASX_OK) return st;

    /* Rust RuntimeState::cancel_request (state.rs:7625-7900). First pass,
     * parent-first: each region's reason, its region.cancelled event (every
     * call), and the close transition or a strengthened reason. */
    n = asx_region_subtree_internal(region, g_propagate_slots, ASX_MAX_REGIONS);
    for (r = 0; r < n; r++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        uint32_t slot = g_propagate_slots[r];
        asx_region_slot *rs = &g_regions[slot];
        asx_region_id rid = asx_region_handle_for_slot(slot);
        asx_trace_payload payload;

        if (r == 0u) {
            g_propagate_reasons[0] = *reason;
        } else {
            /* The cause is the parent's stored reason, which outlives this
             * call; parents precede children in the subtree order. */
            asx_region_slot *parent = &g_regions[asx_handle_slot(rs->parent_id)];
            parent_cancelled_reason(&g_propagate_reasons[r], rs->parent_id, &parent->cancel_reason,
                                    reason->timestamp);
        }
        payload.text = NULL;
        payload.reason = &g_propagate_reasons[r];
        asx_trace_emit_payload_internal(ASX_TRACE_REGION_CANCELLED, rid,
                                        (uint64_t)g_propagate_reasons[r].kind, &payload);

        if (rs->state == ASX_REGION_CLOSED) continue;
        if (!rs->cancel_requested) {
            rs->cancel_reason = g_propagate_reasons[r];
            rs->cancel_requested = 1;
        } else {
            rs->cancel_reason = asx_cancel_strengthen(&rs->cancel_reason, &g_propagate_reasons[r]);
        }
        if (rs->state == ASX_REGION_OPEN && !rs->poisoned) {
            (void)asx_ghost_check_region_transition(rid, rs->state, ASX_REGION_CLOSING);
            rs->state = ASX_REGION_CLOSING;
            asx_trace_emit(ASX_TRACE_REGION_CLOSE, rid, 0);
        }
    }

    /* Second pass: every live task takes its region's reason. */
    count = cancel_subtree_tasks(n);

    /* Third pass: regions already without live work finalize now; the
     * others close as their last task completes. */
    for (r = 0; r < n; r++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        asx_region_slot *rs = &g_regions[g_propagate_slots[r]];
        if (rs->task_count == 0u && rs->child_count == 0u) {
            asx_region_advance_internal(asx_region_handle_for_slot(g_propagate_slots[r]));
        }
    }

    if (out_cancelled != NULL) *out_cancelled = count;
    return ASX_OK;
}

asx_status asx_region_get_cancel_reason(asx_region_id region, asx_cancel_reason *out) {
    asx_region_slot *rs;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(region, &rs);
    if (st != ASX_OK) return st;
    if (!rs->cancel_requested) return ASX_E_NOT_FOUND;
    *out = rs->cancel_reason;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Task checkpoint
 * ------------------------------------------------------------------- */

/* Cleanup polls left: the cleanup budget's quota until the acknowledgement
 * applies it, then what is left of the task's budget; under the opt-in
 * hard bound, the force-completion counter. */
static uint32_t cleanup_polls_left(const asx_task_slot *t) {
    if (asx_cleanup_hard_bound_internal()) return t->cleanup_polls_remaining;
    return t->cleanup_applied ? t->budget.poll_quota : asx_budget_polls(&t->cleanup_budget);
}

asx_status asx_checkpoint(asx_task_id self, asx_checkpoint_result *out) {
    asx_task_slot *t;
    asx_status st;
    asx_cancel_reason before;
    int was_pending;
    int budget_cancel = 0;
    int reason_changed;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    before = t->cancel_reason;
    was_pending = t->cancel_pending;

    /* Budget deadline observed inline, between scheduler polls. Under lab
     * dispatch, as Rust's checkpoint does (checkpoint_budget_exhaustion,
     * cx.rs:3112-3130): a passed deadline is a DEADLINE candidate stamped
     * now, strengthening a cancel already pending; the budget-deadline
     * timer has stamped the first one with the deadline itself. */
    if (t->budget.deadline != 0u && !asx_task_is_terminal(t->state) &&
        (asx_lab_dispatch_active() || !t->cancel_pending)) {
        asx_time now = asx_cancel_now_internal();
        if (now >= t->budget.deadline) {
            asx_time stamp = asx_lab_dispatch_active() ? now : t->budget.deadline;
            st = asx_task_cancel_budget_internal(self, ASX_CANCEL_DEADLINE, stamp);
            (void)st;
            budget_cancel = 1;
        }
    }

    /* A quota already spent to zero (the current poll took the last unit:
     * the scheduler charges before polling, as Rust's lab does) or a spent
     * cost quota cancels the task, attributed to it and stamped now, even
     * inside a masked section, and strengthens a cancel already pending
     * (Rust Cx::checkpoint, cx.rs:2824-2836, checkpoint_budget_exhaustion,
     * :3112-3160; fuzz findings gen-1-16 and gen-1-73, bd-ij9w). */
    if (!asx_task_is_terminal(t->state)) {
        if (t->budget.poll_quota == 0u) {
            st = asx_task_cancel_budget_internal(self, ASX_CANCEL_POLL_QUOTA,
                                                 asx_cancel_now_internal());
            (void)st;
            budget_cancel = 1;
        } else if (t->budget.cost_quota == 0u) {
            st = asx_task_cancel_budget_internal(self, ASX_CANCEL_COST_BUDGET,
                                                 asx_cancel_now_internal());
            (void)st;
            budget_cancel = 1;
        }
    }
    /* A budget cancel that changed the reason leaves the task's cancel
     * waker due; it fires with the poll's acknowledgement, even when the
     * poll completes the task (cx.rs:2834-2840, record/task.rs:1227-1233,
     * lab/runtime.rs:5011). */
    reason_changed = !was_pending || !reason_same(&before, &t->cancel_reason);
    if (budget_cancel && t->cancel_pending && reason_changed) t->cancel_wakers_pending = 1u;

    out->masked = 0;

    /* Not cancelled — report clean status */
    if (!t->cancel_pending) {
        out->cancelled = 0;
        out->phase = ASX_CANCEL_PHASE_REQUESTED; /* unused when not cancelled */
        out->polls_remaining = 0;
        out->kind = ASX_CANCEL_USER;
        return ASX_OK;
    }

    /* Masked: the cancel stays pending and unacknowledged. */
    if (t->mask_depth > 0u) {
        out->cancelled = 0;
        out->masked = 1;
        out->phase = t->cancel_phase;
        out->polls_remaining = cleanup_polls_left(t);
        out->kind = t->cancel_reason.kind;
        return ASX_OK;
    }

    /* A budget cancel the record has not taken: acknowledging reconciles
     * it into the record first (Running → CancelRequested, no event). */
    asx_task_materialize_cancel_internal(t);

    /* Transition CancelRequested → Cancelling on first checkpoint */
    if (t->state == ASX_TASK_CANCEL_REQUESTED) {
        asx_task_state from = t->state;
        (void)asx_ghost_check_task_transition(self, t->state, ASX_TASK_CANCELLING);
        t->state = ASX_TASK_CANCELLING;
        t->cancel_phase = ASX_CANCEL_PHASE_CANCELLING;
        {
            asx_status w_st_ =
                asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_CANCELLING);
            (void)w_st_;
        }
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)self,
                       asx_trace_task_transition_aux(from, ASX_TASK_CANCELLING));
    }

    out->cancelled = 1;
    out->phase = t->cancel_phase;
    out->polls_remaining = cleanup_polls_left(t);
    out->kind = t->cancel_reason.kind;
    /* Every unmasked observation acknowledges (Rust's checkpoint sets
     * cancel_acknowledged each time, cx.rs:2842-2844); the scheduler
     * consumes it after the poll. */
    if (t->in_poll) t->lab_ack_in_poll = 1u;

    /* The cleanup budget becomes the task's budget when this poll
     * returns (asx_task_apply_cleanup_budget_internal), as Rust applies
     * the acknowledgement after the poll. */

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Cancel masking (deferred acknowledgement for critical sections)
 * ------------------------------------------------------------------- */

asx_status asx_task_mask(asx_task_id self) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (asx_task_is_terminal(t->state)) return ASX_E_INVALID_STATE;
    if (t->mask_depth >= ASX_MAX_MASK_DEPTH) return ASX_E_INVALID_STATE;
    t->mask_depth++;
    return ASX_OK;
}

asx_status asx_task_unmask(asx_task_id self) {
    asx_task_slot *t;
    asx_status st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (t->mask_depth == 0u) return ASX_E_INVALID_STATE;
    t->mask_depth--;
    return ASX_OK;
}

asx_status asx_task_mask_depth(asx_task_id id, uint32_t *out_depth) {
    asx_task_slot *t;
    asx_status st;
    if (out_depth == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;
    *out_depth = t->mask_depth;
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Task finalize (Cancelling → Finalizing)
 * ------------------------------------------------------------------- */

asx_status asx_task_finalize(asx_task_id id) {
    asx_task_slot *t;
    asx_status st;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    if (t->state != ASX_TASK_CANCELLING) { return ASX_E_INVALID_STATE; }

    {
        asx_task_state from = t->state;
        (void)asx_ghost_check_task_transition(id, t->state, ASX_TASK_FINALIZING);
        t->state = ASX_TASK_FINALIZING;
        t->cancel_phase = ASX_CANCEL_PHASE_FINALIZING;
        {
            asx_status w_st_ =
                asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_FINALIZING);
            (void)w_st_;
        }
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)id,
                       asx_trace_task_transition_aux(from, ASX_TASK_FINALIZING));
    }

    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Cancel phase query
 * ------------------------------------------------------------------- */

asx_status asx_task_get_cancel_reason(asx_task_id id, asx_cancel_reason *out) {
    asx_task_slot *t;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    if (!t->cancel_pending) return ASX_E_NOT_FOUND;
    *out = t->cancel_reason;
    return ASX_OK;
}

asx_status asx_task_get_cancel_phase(asx_task_id id, asx_cancel_phase *out) {
    asx_task_slot *t;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_task_slot_lookup(id, &t);
    if (st != ASX_OK) return st;

    *out = t->cancel_phase;
    return ASX_OK;
}
