/*
 * scheduler.c — deterministic scheduler loop (walking skeleton)
 *
 * Minimal round-robin scheduler for bd-ix8.8: polls all non-completed
 * tasks in index order (deterministic tie-break) until all complete
 * or budget is exhausted.
 *
 * Phase 3 will add priority scheduling, fairness, and preemption
 * (bd-2cw.2).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/transition.h>
#include "runtime_internal.h"

/* -------------------------------------------------------------------
 * Scheduler: run all tasks in a region until completion or budget
 * ------------------------------------------------------------------- */

asx_status asx_scheduler_run(asx_region_id region, asx_budget *budget)
{
    asx_region_slot *rslot;
    asx_status st;
    uint32_t active;
    uint32_t i;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(region, &rslot);
    if (st != ASX_OK) return st;

    /* Scheduler loop: round-robin poll until all tasks complete */
    for (;;) {
        /* Check budget exhaustion */
        if (asx_budget_is_exhausted(budget)) {
            return ASX_E_POLL_BUDGET_EXHAUSTED;
        }

        active = 0;

        for (i = 0; i < g_task_count; i++) {
            asx_task_slot *t = &g_tasks[i];
            asx_task_id tid;
            asx_status poll_result;

            if (!t->alive) continue;
            if (t->region != region) continue;
            if (asx_task_is_terminal(t->state)) continue;

            active++;

            /* Consume one poll unit */
            if (asx_budget_consume_poll(budget) == 0) {
                return ASX_E_POLL_BUDGET_EXHAUSTED;
            }

            /* Transition Created → Running on first poll */
            if (t->state == ASX_TASK_CREATED) {
                t->state = ASX_TASK_RUNNING;
            }

            /* Build task handle for poll callback */
            tid = asx_handle_pack(ASX_TYPE_TASK,
                                  (uint16_t)(1u << (unsigned)t->state),
                                  asx_handle_pack_index(
                                      t->generation, (uint16_t)i));

            /* Call the task's poll function */
            asx_error_ledger_bind_task(tid);
            poll_result = t->poll_fn(t->user_data, tid);
            asx_error_ledger_bind_task(ASX_INVALID_ID);

            if (poll_result == ASX_OK) {
                /* Task completed successfully */
                t->state = ASX_TASK_COMPLETED;
                t->outcome = asx_outcome_make(ASX_OUTCOME_OK);
                rslot->task_count--;
            } else if (poll_result != ASX_E_PENDING) {
                /* Task failed — mark as completed with error */
                t->state = ASX_TASK_COMPLETED;
                t->outcome = asx_outcome_make(ASX_OUTCOME_ERR);
                rslot->task_count--;
            }
            /* ASX_E_PENDING: task not ready, continue polling next task */
        }

        /* No active tasks left — quiescent */
        if (active == 0) {
            return ASX_OK;
        }
    }
}
