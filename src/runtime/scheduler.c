/*
 * scheduler.c — deterministic scheduler loop with event sequencing
 *
 * Round-robin scheduler that polls all non-completed tasks in arena
 * index order (deterministic tie-break). Emits a monotonic event
 * sequence for replay identity verification.
 *
 * Tie-break rule: tasks are polled in ascending arena index within
 * each round. This ordering is stable and deterministic for any
 * given input and seed.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/asx.h>
#include <asx/asx_config.h>
#include <asx/core/cancel.h>
#include <asx/core/transition.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>

/* -------------------------------------------------------------------
 * Event log (ring buffer for deterministic sequencing)
 * ------------------------------------------------------------------- */

#define ASX_SCHED_EVENT_LOG_CAPACITY 256u

static asx_scheduler_event g_event_log[ASX_SCHED_EVENT_LOG_CAPACITY];
static uint32_t g_event_count = 0;

static void sched_emit(asx_scheduler_event_kind kind, asx_task_id tid, uint32_t round) {
    if (g_event_count < ASX_SCHED_EVENT_LOG_CAPACITY) {
        asx_scheduler_event *e = &g_event_log[g_event_count];
        e->kind = kind;
        e->task_id = tid;
        e->sequence = g_event_count;
        e->round = round;
    }
    /* Saturate at UINT32_MAX to prevent wrap-around losing event history */
    if (g_event_count < UINT32_MAX) { g_event_count++; }
}

static uint64_t asx_trace_task_transition_aux(asx_task_state from, asx_task_state to) {
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

uint32_t asx_scheduler_event_count(void) { return g_event_count; }

int asx_scheduler_event_get(uint32_t index, asx_scheduler_event *out) {
    if (out == NULL) return 0;
    if (index >= g_event_count) return 0;
    if (index >= ASX_SCHED_EVENT_LOG_CAPACITY) return 0;
    *out = g_event_log[index];
    return 1;
}

void asx_scheduler_event_reset(void) { g_event_count = 0; }

/* -------------------------------------------------------------------
 * Scheduler: run all tasks in a region until completion or budget
 *
 * Ordering invariant: tasks are polled in ascending arena index
 * within each round. This produces a deterministic event stream
 * for any given input and seed combination.
 * ------------------------------------------------------------------- */

asx_status asx_scheduler_run(asx_region_id region, asx_budget *budget) {
    asx_region_slot *rslot;
    asx_status st;
    uint32_t active;
    uint32_t round;
    uint32_t i;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &rslot);
    if (st != ASX_OK) return st;

    /* Reset event log for this scheduler invocation */
    asx_scheduler_event_reset();

    /* Scheduler loop: round-robin poll until all tasks complete */
    for (round = 0;; round++) {
        ASX_CHECKPOINT_WAIVER("kernel-scheduler: this IS the scheduler event loop; "
                              "budget exhaustion provides bounded termination");
        asx_trace_emit(ASX_TRACE_SCHED_ROUND, ASX_INVALID_ID, round);

        /* Check budget exhaustion */
        if (asx_budget_is_exhausted(budget)) {
            sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
            return ASX_E_POLL_BUDGET_EXHAUSTED;
        }

        active = 0;

        for (i = 0; i < g_task_count; i++) {
            ASX_CHECKPOINT_WAIVER("kernel-scheduler: inner poll loop bounded by "
                                  "g_task_count <= ASX_MAX_TASKS arena capacity");
            asx_task_slot *t = &g_tasks[i];
            asx_task_id tid;
            asx_status poll_result;

            if (!t->alive) continue;
            if (t->region != region) continue;
            if (asx_task_is_terminal(t->state)) continue;

            active++;

            /* Build task handle for poll callback */
            tid = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)t->state),
                                  asx_handle_pack_index(t->generation, (uint16_t)i));

            /* ----------------------------------------------------------
             * Cancel-phase scheduler integration (bd-2cw.3)
             *
             * FINALIZING: task called asx_task_finalize() — cleanup is
             * done. Complete without consuming a poll unit.
             *
             * CANCELLING + budget exhausted: force-complete with
             * CANCELLED outcome. This ensures bounded cleanup.
             * ---------------------------------------------------------- */
            if (t->state == ASX_TASK_FINALIZING) {
                asx_task_state from = t->state;
                (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                t->state = ASX_TASK_COMPLETED;
                t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                { asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_COMPLETED); (void)w_st_; }
                { asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness); (void)w_st_; }
                asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                               asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                asx_task_release_capture_internal(t);
                if (rslot->task_count > 0) rslot->task_count--;
                active--;
                sched_emit(ASX_SCHED_EVENT_COMPLETE, tid, round);
                asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
                continue;
            }

            if (t->cancel_pending &&
                (t->state == ASX_TASK_CANCELLING || t->state == ASX_TASK_CANCEL_REQUESTED) &&
                t->cleanup_polls_remaining == 0) {
                /* Force-complete: cleanup budget exhausted. The task
                 * either never called checkpoint (CANCEL_REQUESTED)
                 * or ran out of cleanup polls (CANCELLING). */
                if (t->state == ASX_TASK_CANCEL_REQUESTED) {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_CANCELLING);
                    t->state = ASX_TASK_CANCELLING;
                    t->cancel_phase = ASX_CANCEL_PHASE_CANCELLING;
                    { asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_CANCELLING); (void)w_st_; }
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_CANCELLING));
                }
                {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_FINALIZING);
                    t->state = ASX_TASK_FINALIZING;
                    t->cancel_phase = ASX_CANCEL_PHASE_FINALIZING;
                    { asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_FINALIZING); (void)w_st_; }
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_FINALIZING));
                }
                {
                    asx_task_state from = t->state;
                    (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                    t->state = ASX_TASK_COMPLETED;
                    t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                    { asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_COMPLETED); (void)w_st_; }
                    { asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness); (void)w_st_; }
                    asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                                   asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                }
                t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                asx_task_release_capture_internal(t);
                if (rslot->task_count > 0) rslot->task_count--;
                active--;
                sched_emit(ASX_SCHED_EVENT_CANCEL_FORCED, tid, round);
                asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
                continue;
            }

            /* Consume one poll unit */
            if (asx_budget_consume_poll(budget) == 0) {
                sched_emit(ASX_SCHED_EVENT_BUDGET, ASX_INVALID_ID, round);
                asx_trace_emit(ASX_TRACE_SCHED_BUDGET, ASX_INVALID_ID, round);
                return ASX_E_POLL_BUDGET_EXHAUSTED;
            }

            /* Transition Created → Running on first poll */
            if (t->state == ASX_TASK_CREATED) {
                asx_task_state from = t->state;
                (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_RUNNING);
                t->state = ASX_TASK_RUNNING;
                asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                               asx_trace_task_transition_aux(from, ASX_TASK_RUNNING));
            }

            /* Emit poll event */
            sched_emit(ASX_SCHED_EVENT_POLL, tid, round);
            asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)tid, round);

            /* Call the task's poll function */
            asx_error_ledger_bind_task(tid);
            poll_result = t->poll_fn(t->user_data, tid);
            asx_error_ledger_bind_task(ASX_INVALID_ID);

            if (poll_result == ASX_OK) {
                /* Task completed — set outcome based on cancel state */
                asx_task_state from = t->state;
                (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                t->state = ASX_TASK_COMPLETED;
                if (t->cancel_pending) {
                    t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                    { asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_COMPLETED); (void)w_st_; }
                    { asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness); (void)w_st_; }
                    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                } else {
                    t->outcome = asx_outcome_make(ASX_OUTCOME_OK);
                }
                asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                               asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                asx_task_release_capture_internal(t);
                if (rslot->task_count > 0) rslot->task_count--;
                active--;
                sched_emit(ASX_SCHED_EVENT_COMPLETE, tid, round);
                asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);
            } else if (poll_result != ASX_E_PENDING) {
                /* Task failed — mark as completed with error.
                 * If cancel was pending, outcome joins to CANCELLED
                 * since CANCELLED > ERR in the severity lattice. */
                asx_task_state from = t->state;
                (void)asx_ghost_check_task_transition(tid, t->state, ASX_TASK_COMPLETED);
                t->state = ASX_TASK_COMPLETED;
                if (t->cancel_pending) {
                    t->cancel_phase = ASX_CANCEL_PHASE_COMPLETED;
                    { asx_status w_st_ = asx_cancel_witness_advance(t->cancel_witness, ASX_CANCEL_PHASE_COMPLETED); (void)w_st_; }
                    { asx_status w_st_ = asx_cancel_witness_release(t->cancel_witness); (void)w_st_; }
                    t->outcome = asx_outcome_make(ASX_OUTCOME_CANCELLED);
                } else {
                    t->outcome = asx_outcome_make(ASX_OUTCOME_ERR);
                }
                asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)tid,
                               asx_trace_task_transition_aux(from, ASX_TASK_COMPLETED));
                asx_task_release_capture_internal(t);
                if (rslot->task_count > 0) rslot->task_count--;
                active--;
                sched_emit(ASX_SCHED_EVENT_COMPLETE, tid, round);
                asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, (uint64_t)tid, round);

                /* Apply fault containment policy (bd-hwb.15).
                 * In POISON_REGION mode this poisons the region,
                 * blocking further spawn/close. The scheduler
                 * continues draining existing tasks. */
                {
                    asx_status fc_ = asx_region_contain_fault(region, poll_result);
                    if (fc_ != ASX_OK &&
                        asx_containment_policy_active() != ASX_CONTAIN_POISON_REGION) {
                        return fc_;
                    }
                }
            } else if (t->cancel_pending) {
                /* PENDING + cancel active: decrement cleanup budget.
                 * The scheduler is the sole budget enforcer — each
                 * poll of a cancel-phase task consumes one unit. */
                if (t->cleanup_polls_remaining > 0) { t->cleanup_polls_remaining--; }
            }
            /* ASX_E_PENDING without cancel: task not ready, continue */
        }

        /* No active tasks left — quiescent */
        if (active == 0) {
            sched_emit(ASX_SCHED_EVENT_QUIESCENT, ASX_INVALID_ID, round);
            asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, ASX_INVALID_ID, round);
            return ASX_OK;
        }
    }
}
