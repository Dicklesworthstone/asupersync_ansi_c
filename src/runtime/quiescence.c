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

            /* Keep the siblings in insertion order, as Rust's Membership
             * remove does (record/region.rs:416-425): a cancel walks the
             * children in this order (asx_region_subtree_internal). */
            parent->child_count--;
            for (; i < parent->child_count; i++) {
                /* ASX_CHECKPOINT_WAIVER("bounded shift over parent's child array") */
                parent->children[i] = parent->children[i + 1u];
            }
            parent->children[parent->child_count] = ASX_INVALID_ID;
            break;
        }
    } else {
        (void)asx_runtime_log_write(ASX_LOG_WARN,
                                    "child region closed after parent slot became unavailable");
    }
    /* r->parent_id stays: a closed child still names its parent, so its
     * slot is not recycled while the parent lives (asx_region_open). */
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
        if (t->cleanup_only && g_regions[asx_handle_slot(id)].state == ASX_REGION_FINALIZING) {
            continue;
        }
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
        asx_region_finalizing_enter_internal(id);
    }

    if (r->state == ASX_REGION_FINALIZING) {
        /* A callback may resolve an obligation, recursively advancing this
         * region. Its active drain owns finalization until every callback
         * has returned, even when the remaining stack is already empty. */
        if (r->cleanup.draining) return ASX_E_PENDING;
        /* The running async finalizer is the LIFO barrier: nothing beneath
         * it runs before it retires. An async entry on top ends the drain;
         * it runs as a task the next lab step spawns (the admission). */
        if (r->finalizer.active) return ASX_E_QUIESCENCE_TASKS_LIVE;
        asx_cleanup_drain(&r->cleanup);
        if (asx_cleanup_async_pending(&r->cleanup) > 0u) return ASX_E_PENDING;
        if (r->task_count > 0u) return ASX_E_QUIESCENCE_TASKS_LIVE;

        /* Audit only after finalizers and their cleanup tasks have finished,
         * so they can resolve obligations before the leak policy applies. */
        (void)asx_ghost_check_obligation_leaks(id);
        st = asx_region_audit_leaks_internal(id);
        if (st != ASX_OK) return st;
        st = asx_region_set_state(id, r, ASX_REGION_CLOSED);
        if (st != ASX_OK) return st;
        asx_region_finalizing_leave_internal(id);
        asx_region_unlink_from_parent(id, r);
        asx_trace_emit(ASX_TRACE_REGION_CLOSED, id, 0);
        asx_region_wake_close_waiters_internal(id);
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
        /* A closing region with child regions waits in DRAINING whether or
         * not its own tasks are done (Rust advance_region_state calls
         * begin_drain whenever child_count > 0; bd-kovo). */
        if (r->state == ASX_REGION_CLOSING && r->child_count > 0u &&
            asx_region_set_state(id, r, ASX_REGION_DRAINING) != ASX_OK) {
            return;
        }
        if (r->poisoned || r->task_count > 0u) return;
        parent = r->parent_id;
        if (asx_region_finalize_one(id, r) != ASX_OK) return; /* children remain, or PANIC leaks */
        if (r->state != ASX_REGION_CLOSED) return;
        id = parent;
    }
}

/* One pass of the shared execution/finalization machinery. The caller has
 * already chosen whether to cancel; retries here never request or strengthen
 * cancellation. force_run runs the scheduler even with no live task, for the
 * async finalizers that finalization found. A region whose async finalizer
 * is outstanding is no error when `retry` is given: *retry is set and the
 * caller makes another pass, which runs the finalizers. */
static asx_status asx_region_close_pass(asx_region_id id, asx_budget *budget,
                                        const asx_region_id *drain_regions, uint32_t n,
                                        int force_run, int *retry) {
    asx_status st;
    uint32_t i;
    uint32_t live;

    /* Step 3: run the scheduler over the subtree. */
    live = 0;
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        asx_region_slot *rs;
        if (asx_region_slot_lookup(drain_regions[i], &rs) == ASX_OK) live += rs->task_count;
    }
    if (live > 0u || force_run) {
        st = asx_scheduler_run(id, budget);
        if (st != ASX_OK) return st;
        for (i = 0; i < n; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
            asx_region_slot *rs;
            if (asx_region_slot_lookup(drain_regions[i], &rs) == ASX_OK && rs->task_count > 0u) {
                return ASX_E_QUIESCENCE_TASKS_LIVE;
            }
        }
    }

    /* Step 4: finalize bottom-up so children close before parents. Validate
     * the original handles: callbacks may recycle a closed sibling's slot. */
    for (i = n; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        asx_region_slot *rs;
        if (asx_region_slot_lookup(drain_regions[i - 1u], &rs) != ASX_OK) continue;
        st = asx_region_finalize_one(drain_regions[i - 1u], rs);
        if (st != ASX_OK) {
            /* An ancestor of a region with a finalizer outstanding waits
             * for it in DRAINING (PENDING): it too is for the next pass. */
            if (retry != NULL && (asx_region_finalizer_outstanding_internal(rs) ||
                                  (*retry && st == ASX_E_PENDING))) {
                *retry = 1;
                continue;
            }
            return st;
        }
    }

    return ASX_OK;
}

/* Shared execution/finalization machinery: a pass, and a second one when
 * finalization left async finalizers to run. The scheduler spawns and runs
 * every finalizer of the subtree in its own run, so two passes always
 * suffice (a finalizer cannot register another: its region is not Open). */
static asx_status asx_region_run_to_closed(asx_region_id id, asx_budget *budget,
                                           const asx_region_id *drain_regions, uint32_t n) {
    int retry = 0;
    asx_status st = asx_region_close_pass(id, budget, drain_regions, n, 0, &retry);
    if (st != ASX_OK || !retry) return st;
    return asx_region_close_pass(id, budget, drain_regions, n, 1, NULL);
}

asx_status asx_region_wait_closed(asx_region_id id, asx_budget *budget) {
    asx_region_slot *r;
    asx_status st;
    uint32_t slots[ASX_MAX_REGIONS];
    asx_region_id regions[ASX_MAX_REGIONS] = {0};
    uint32_t n;
    uint32_t i;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    if (r->poisoned) return ASX_E_REGION_POISONED;
    if (asx_task_current() != ASX_INVALID_ID || r->state == ASX_REGION_OPEN) {
        return ASX_E_INVALID_STATE;
    }
    if (r->state == ASX_REGION_CLOSED) return ASX_OK;
    if (r->cleanup.draining) return ASX_E_PENDING;
    n = asx_region_subtree_internal(id, slots, ASX_MAX_REGIONS);
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        regions[i] = asx_region_handle_for_slot(slots[i]);
    }
    st = asx_region_run_to_closed(id, budget, regions, n);
    if (st != ASX_OK) return st;
    st = asx_region_slot_lookup(id, &r);
    if (st == ASX_E_STALE_HANDLE) return ASX_OK;
    if (st != ASX_OK) return st;
    return r->state == ASX_REGION_CLOSED ? ASX_OK : ASX_E_PENDING;
}

asx_status asx_region_drain(asx_region_id id, asx_budget *budget) {
    asx_region_slot *r;
    asx_status st;
    uint32_t n;
    uint32_t i;
    int need_cancel = 0;
    uint32_t drain_slots[ASX_MAX_REGIONS];
    asx_region_id drain_regions[ASX_MAX_REGIONS];

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;
    if (r->state == ASX_REGION_CLOSED) return ASX_OK;
    if (r->cleanup.draining) return ASX_E_PENDING;
    n = asx_region_subtree_internal(id, drain_slots, ASX_MAX_REGIONS);
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MAX_REGIONS");
        asx_region_slot *rs = &g_regions[drain_slots[i]];
        drain_regions[i] = asx_region_handle_for_slot(drain_slots[i]);
        if (rs->state == ASX_REGION_OPEN) {
            st = asx_region_set_state(drain_regions[i], rs, ASX_REGION_CLOSING);
            if (st != ASX_OK) return st;
            need_cancel = 1;
        }
        if (rs->task_count > 0u && asx_region_has_uncancelled_tasks(drain_regions[i])) {
            need_cancel = 1;
        }
    }
    if (need_cancel) (void)asx_cancel_propagate(id, ASX_CANCEL_PARENT);
    return asx_region_run_to_closed(id, budget, drain_regions, n);
}

/* -------------------------------------------------------------------
 * Region finalizers: Rust's Scope::defer_sync
 * (RuntimeState::register_sync_finalizer, runtime/state.rs:9046-9065).
 * A finalizer is an entry of the region's cleanup stack, drained LIFO by
 * asx_region_finalize_one before the leak audit.
 * ------------------------------------------------------------------- */

/* The checks both registrations (defer_sync, defer_async) share. Rust's
 * Scope::defer_sync takes the region's own Scope and the runtime state, so
 * only code that holds the region can register a finalizer on it: here the
 * host (no task is polling) or a task of that region. Then the region must
 * be neither poisoned nor closing (register_sync_finalizer returns false
 * for a closing or terminal region, state.rs:9056-9062). */
static asx_status asx_region_finalizer_target(asx_region_id region, asx_region_slot **out) {
    asx_region_slot *r;
    asx_task_slot *caller;
    asx_task_id cur = asx_task_current();
    asx_status st = asx_region_slot_lookup(region, &r);

    if (st != ASX_OK) return st;
    if (cur != ASX_INVALID_ID && asx_task_slot_lookup(cur, &caller) == ASX_OK &&
        asx_handle_index(caller->region) != asx_handle_index(region)) {
        return ASX_E_PERMISSION_DENIED;
    }
    if (r->poisoned) return ASX_E_REGION_POISONED;
    if (r->state != ASX_REGION_OPEN) return ASX_E_REGION_CLOSED;
    *out = r;
    return ASX_OK;
}

asx_status asx_region_defer_sync(asx_region_id region, asx_region_finalizer_fn fn,
                                 void *user_data) {
    asx_region_slot *r;
    asx_cleanup_handle h;
    asx_status st;

    if (fn == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_finalizer_target(region, &r);
    if (st != ASX_OK) return st;
    return asx_cleanup_push(&r->cleanup, fn, user_data, &h);
}

asx_status asx_region_get_close_outcome(asx_region_id region, asx_outcome *out) {
    asx_region_slot *r;
    asx_status st;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_slot_lookup(region, &r);
    if (st != ASX_OK) return st;
    if (r->has_close_outcome) {
        *out = r->close_outcome;
        return ASX_OK;
    }
    if (r->state != ASX_REGION_CLOSED) return ASX_E_NOT_FOUND;
    /* complete_close's default, fixed once the region is Closed (its
     * cancel state can no longer change): Cancelled with the region's own
     * reason when it was cancelled (record/region.rs:1810-1823). */
    if (r->cancel_requested) {
        *out = asx_outcome_cancelled(&r->cancel_reason);
    } else {
        *out = asx_outcome_make(ASX_OUTCOME_OK);
    }
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Async finalizers: Rust's Scope::defer_async
 * (RuntimeState::register_async_finalizer, runtime/state.rs:9101) and the
 * lab's schedule_async_finalizers (drain_ready_async_finalizers,
 * state.rs:8540).
 *
 * An async finalizer is an async entry of the region's cleanup stack. Once
 * the region is FINALIZING, finalization drains the sync entries above it
 * and stops there. The next lab step then spawns it as the region's cleanup
 * task (asx_region_admit_finalizers_internal): masked, with the finalizer
 * budget, and at most one per region (region->finalizer.active, Rust's
 * active_async_finalizers barrier). When it completes, finalization goes on
 * with the entries beneath it, and the region closes once the stack is
 * empty. Unlike Rust, which also promotes its sync finalizers to tasks, a
 * sync finalizer runs inline in the drain (asx_region_defer_sync).
 * ------------------------------------------------------------------- */

/* Rust's finalizing_regions (state.rs:2101): the regions in FINALIZING, as
 * region slots, in the order they entered it (push, :10241); a close
 * swap-removes its entry (:10340). */
static uint32_t g_finalizing[ASX_MAX_REGIONS];
static uint32_t g_finalizing_count;

/* The finalizer budget and escalation of asx_runtime_config. The defaults
 * are Rust's constants (FINALIZER_POLL_BUDGET and FINALIZER_TIME_BUDGET_NANOS,
 * record/finalizer.rs:42-45). */
static uint32_t g_finalizer_poll_budget = 100u;
static uint64_t g_finalizer_time_budget_ns = 5000000000u;
static asx_finalizer_escalation g_finalizer_escalation = ASX_FINALIZER_BOUNDED_LOG;

void asx_region_finalizers_reset_internal(void) {
    g_finalizing_count = 0u;
    g_finalizer_poll_budget = 100u;
    g_finalizer_time_budget_ns = 5000000000u;
    g_finalizer_escalation = ASX_FINALIZER_BOUNDED_LOG;
}

void asx_region_finalizer_config_set_internal(uint32_t poll_budget, uint64_t time_budget_ns,
                                              asx_finalizer_escalation escalation) {
    g_finalizer_poll_budget = poll_budget;
    g_finalizer_time_budget_ns = time_budget_ns;
    g_finalizer_escalation = escalation;
}

void asx_region_finalizing_enter_internal(asx_region_id id) {
    if (g_finalizing_count < ASX_MAX_REGIONS) {
        g_finalizing[g_finalizing_count++] = (uint32_t)asx_handle_slot(id);
    }
}

void asx_region_finalizing_leave_internal(asx_region_id id) {
    uint32_t i;
    for (i = 0; i < g_finalizing_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_finalizing_count <= ASX_MAX_REGIONS");
        if (g_finalizing[i] != (uint32_t)asx_handle_slot(id)) continue;
        g_finalizing[i] = g_finalizing[--g_finalizing_count];
        return;
    }
}

int asx_region_finalizer_outstanding_internal(const asx_region_slot *r) {
    return r->finalizer.active || asx_cleanup_async_pending(&r->cleanup) > 0u;
}

int asx_region_finalizers_pending_internal(void) {
    uint32_t i;
    for (i = 0; i < g_finalizing_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_finalizing_count <= ASX_MAX_REGIONS");
        const asx_region_slot *r = &g_regions[g_finalizing[i]];
        if (r->alive && !r->poisoned && r->state == ASX_REGION_FINALIZING &&
            asx_region_finalizer_outstanding_internal(r)) {
            return 1;
        }
    }
    return 0;
}

void asx_region_finalizer_task_done_internal(asx_region_slot *r, const asx_task_slot *task) {
    if (r->finalizer.active && r->finalizer.task_slot == (uint32_t)(task - g_tasks)) {
        r->finalizer.active = 0;
    }
}

/* The finalizer task's poll function: the finalizer's own, counted against
 * its budget. user_data is the region slot, which outlives the task: the
 * region cannot close before its finalizer task completes.
 *
 * The budget is advisory in Rust: the task's Cx sees it spent, but the
 * finalizer is masked and runs on (its region shutdown ceiling is the only
 * enforcement, BudgetedFinalizer, record/finalizer.rs:423-640, set by a
 * close with a budget that this runtime has no counterpart of). The
 * escalation is read the same way: ASX_FINALIZER_BOUNDED_LOG notes the
 * first poll past the budget in the log, SOFT stays silent, and the opt-in
 * BOUNDED_PANIC completes the finalizer PANICKED at that poll instead. */
static asx_status asx_finalizer_task_poll(void *user_data, asx_task_id self) {
    asx_region_slot *r = (asx_region_slot *)user_data;
    asx_region_finalizer *f = &r->finalizer;
    int spent = f->polls >= f->poll_budget || asx_cancel_now_internal() >= f->deadline;

    if (spent && !f->exhausted) {
        f->exhausted = 1;
        if (g_finalizer_escalation != ASX_FINALIZER_SOFT) {
            (void)asx_runtime_log_write(ASX_LOG_WARN, "async finalizer ran past its budget");
        }
    }
    if (spent && g_finalizer_escalation == ASX_FINALIZER_BOUNDED_PANIC) {
        return asx_task_panic(self, "async finalizer exceeded its budget");
    }
    f->polls++;
    return f->poll(f->data, self);
}

uint32_t asx_region_admit_finalizers_internal(void) {
    uint32_t admitted = 0u;
    uint32_t i;

    for (i = 0; i < g_finalizing_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_finalizing_count <= ASX_MAX_REGIONS");
        asx_region_slot *r = &g_regions[g_finalizing[i]];
        asx_cleanup_poll_fn poll = NULL;
        void *data = NULL;
        asx_budget budget;
        asx_task_id tid;
        asx_status st;
        asx_time now;

        if (!r->alive || r->poisoned || r->state != ASX_REGION_FINALIZING) continue;
        if (r->finalizer.active || r->cleanup.draining) continue;
        if (asx_cleanup_take_async(&r->cleanup, &poll, &data) != ASX_OK) continue;

        /* finalizer_budget().with_deadline(now + FINALIZER_TIME_BUDGET_NANOS)
         * (state.rs:8878-8881): the poll quota, priority 128, the deadline. */
        now = asx_cancel_now_internal();
        budget = asx_budget_from_polls(g_finalizer_poll_budget);
        budget.deadline = g_finalizer_time_budget_ns > UINT64_MAX - now
                              ? UINT64_MAX
                              : now + g_finalizer_time_budget_ns;
        r->finalizer.active = 1;
        r->finalizer.poll = poll;
        r->finalizer.data = data;
        r->finalizer.polls = 0u;
        r->finalizer.poll_budget = g_finalizer_poll_budget;
        r->finalizer.deadline = budget.deadline;
        r->finalizer.exhausted = 0;
        st = asx_task_spawn_finalizer_internal(asx_region_handle_for_slot(g_finalizing[i]),
                                               asx_finalizer_task_poll, r, &budget, &tid);
        if (st != ASX_OK) {
            /* Rust keeps the barrier and re-queues the finalizer when its
             * task cannot be created (state.rs:8629-8645): it is spawned by
             * a later step. It was just taken, so the stack has room. */
            asx_cleanup_handle h;
            r->finalizer.active = 0;
            st = asx_cleanup_push_async(&r->cleanup, poll, data, &h);
            (void)st;
            continue;
        }
        r->finalizer.task_slot = (uint32_t)asx_handle_slot(tid);
        admitted++;
    }
    return admitted;
}

asx_status asx_region_defer_async(asx_region_id region, asx_task_poll_fn poll_fn, void *user_data) {
    asx_region_slot *r;
    asx_cleanup_handle h;
    asx_status st;

    if (poll_fn == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_region_finalizer_target(region, &r);
    if (st != ASX_OK) return st;
    return asx_cleanup_push_async(&r->cleanup, poll_fn, user_data, &h);
}
