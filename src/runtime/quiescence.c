/*
 * quiescence.c — close/finalize/quiescence driver (walking skeleton)
 *
 * Minimal implementation for bd-ix8.8: region drain and quiescence
 * assertion. Drives region through Close → Drain → Finalize → Closed.
 *
 * Phase 3 will add obligation tracking, finalizer chains, and leak
 * detection (bd-2cw.1). Semantics from QUIESCENCE_FINALIZATION_INVARIANTS.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/core/transition.h>
#include <asx/core/cleanup.h>
#include "runtime_internal.h"

/* -------------------------------------------------------------------
 * Quiescence check
 * ------------------------------------------------------------------- */

asx_status asx_quiescence_check(asx_region_id id)
{
    asx_region_slot *r;
    asx_status st;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    /* Quiescent iff region is CLOSED and no live tasks remain */
    if (r->state != ASX_REGION_CLOSED) {
        return ASX_E_QUIESCENCE_NOT_REACHED;
    }
    if (r->task_count > 0) {
        return ASX_E_QUIESCENCE_TASKS_LIVE;
    }

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

asx_status asx_region_drain(asx_region_id id, asx_budget *budget)
{
    asx_region_slot *r;
    asx_status st;

    if (budget == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_region_slot_lookup(id, &r);
    if (st != ASX_OK) return st;

    /* Step 1: Close the region if still open */
    if (r->state == ASX_REGION_OPEN) {
        st = asx_region_transition_check(ASX_REGION_OPEN, ASX_REGION_CLOSING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_CLOSING;
    }

    /* Step 2: Run scheduler to drain tasks */
    if (r->task_count > 0) {
        st = asx_scheduler_run(id, budget);
        if (st != ASX_OK && st != ASX_E_POLL_BUDGET_EXHAUSTED) {
            return st;
        }
        if (r->task_count > 0) {
            return ASX_E_QUIESCENCE_TASKS_LIVE;
        }
    }

    /* Step 3: Advance through closing protocol */
    if (r->state == ASX_REGION_CLOSING) {
        /* No children (walking skeleton) — fast path: skip Draining */
        st = asx_region_transition_check(ASX_REGION_CLOSING,
                                         ASX_REGION_FINALIZING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_FINALIZING;
    }

    if (r->state == ASX_REGION_DRAINING) {
        st = asx_region_transition_check(ASX_REGION_DRAINING,
                                         ASX_REGION_FINALIZING);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_FINALIZING;
    }

    if (r->state == ASX_REGION_FINALIZING) {
        /* Drain cleanup stack in LIFO order before closing */
        asx_cleanup_drain(&r->cleanup);

        st = asx_region_transition_check(ASX_REGION_FINALIZING,
                                         ASX_REGION_CLOSED);
        if (st != ASX_OK) return st;
        r->state = ASX_REGION_CLOSED;
    }

    return ASX_OK;
}
