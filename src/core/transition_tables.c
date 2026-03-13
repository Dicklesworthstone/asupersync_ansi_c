/*
 * transition_tables.c — state machine transition authority tables
 *
 * Transition legality is encoded as lookup tables. O(1) validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/transition.h>

/*
 * Region transition authority table.
 * region_transitions[from][to] = 1 if legal, 0 if forbidden.
 *
 * Legal transitions:
 *   Open -> Closing
 *   Closing -> Draining
 *   Closing -> Finalizing  (fast path: no children)
 *   Draining -> Finalizing
 *   Finalizing -> Closed
 */
static const int region_transitions[5][5] = {
    /* To:  Open  Clos  Drain Final Closed */
    /*Open*/ {0, 1, 0, 0, 0},
    /*Clos*/ {0, 0, 1, 1, 0},
    /*Drain*/ {0, 0, 0, 1, 0},
    /*Final*/ {0, 0, 0, 0, 1},
    /*Closed*/ {0, 0, 0, 0, 0}};

/*
 * Task transition authority table.
 *
 * Legal transitions:
 *   Created -> Running          (T1: first poll)
 *   Created -> CancelRequested  (T2: cancel before first poll)
 *   Created -> Completed        (T3: error at spawn)
 *   Running -> CancelRequested  (T4: cancel during run)
 *   Running -> Completed        (T5: natural completion)
 *   CancelRequested -> Cancelling   (T7: acknowledge cancel)
 *   CancelRequested -> Completed    (T8: natural completion before ack)
 *   CancelRequested -> CancelRequested (T6: strengthen, self-transition)
 *   Cancelling -> Finalizing    (T10: cleanup done)
 *   Cancelling -> Completed     (T11: natural completion during cancel)
 *   Cancelling -> Cancelling    (T9: strengthen)
 *   Finalizing -> Completed     (T13: finalize done)
 *   Finalizing -> Finalizing    (T12: strengthen)
 */
static const int task_transitions[6][6] = {
    /* To:   Creat  Run   CanR  Cling Final Comp */
    /*Creat*/ {0, 1, 1, 0, 0, 1},
    /*Run*/ {0, 0, 1, 0, 0, 1},
    /*CanR*/ {0, 0, 1, 1, 0, 1},
    /*Cling*/ {0, 0, 0, 1, 1, 1},
    /*Final*/ {0, 0, 0, 0, 1, 1},
    /*Comp*/ {0, 0, 0, 0, 0, 0}};

/*
 * Obligation transition authority table.
 *
 * Legal transitions:
 *   Reserved -> Committed
 *   Reserved -> Aborted
 *   Reserved -> Leaked
 */
static const int obligation_transitions[4][4] = {
    /* To:    Res   Comm  Abort Leaked */
    /*Res*/ {0, 1, 1, 1},
    /*Comm*/ {0, 0, 0, 0},
    /*Abort*/ {0, 0, 0, 0},
    /*Leaked*/ {0, 0, 0, 0}};

asx_status asx_region_transition_check(asx_region_state from, asx_region_state to) {
    if ((unsigned)from > 4 || (unsigned)to > 4) return ASX_E_INVALID_ARGUMENT;
    return region_transitions[from][to] ? ASX_OK : ASX_E_INVALID_TRANSITION;
}

asx_status asx_task_transition_check(asx_task_state from, asx_task_state to) {
    if ((unsigned)from > 5 || (unsigned)to > 5) return ASX_E_INVALID_ARGUMENT;
    return task_transitions[from][to] ? ASX_OK : ASX_E_INVALID_TRANSITION;
}

asx_status asx_obligation_transition_check(asx_obligation_state from, asx_obligation_state to) {
    if ((unsigned)from > 3 || (unsigned)to > 3) return ASX_E_INVALID_ARGUMENT;
    return obligation_transitions[from][to] ? ASX_OK : ASX_E_INVALID_TRANSITION;
}

int asx_region_can_spawn(asx_region_state s) { return s == ASX_REGION_OPEN; }

int asx_region_can_accept_work(asx_region_state s) {
    return s == ASX_REGION_OPEN || s == ASX_REGION_FINALIZING;
}

int asx_region_is_closing(asx_region_state s) {
    return s == ASX_REGION_CLOSING || s == ASX_REGION_DRAINING || s == ASX_REGION_FINALIZING;
}

int asx_region_is_terminal(asx_region_state s) { return s == ASX_REGION_CLOSED; }

int asx_task_is_terminal(asx_task_state s) { return s == ASX_TASK_COMPLETED; }

int asx_obligation_is_terminal(asx_obligation_state s) {
    return s == ASX_OBLIGATION_COMMITTED || s == ASX_OBLIGATION_ABORTED ||
           s == ASX_OBLIGATION_LEAKED;
}

const char *asx_region_state_str(asx_region_state s) {
    switch (s) {
    case ASX_REGION_OPEN: return "Open";
    case ASX_REGION_CLOSING: return "Closing";
    case ASX_REGION_DRAINING: return "Draining";
    case ASX_REGION_FINALIZING: return "Finalizing";
    case ASX_REGION_CLOSED: return "Closed";
    default: return "Unknown";
    }
}

const char *asx_task_state_str(asx_task_state s) {
    switch (s) {
    case ASX_TASK_CREATED: return "Created";
    case ASX_TASK_RUNNING: return "Running";
    case ASX_TASK_CANCEL_REQUESTED: return "CancelRequested";
    case ASX_TASK_CANCELLING: return "Cancelling";
    case ASX_TASK_FINALIZING: return "Finalizing";
    case ASX_TASK_COMPLETED: return "Completed";
    default: return "Unknown";
    }
}

const char *asx_obligation_state_str(asx_obligation_state s) {
    switch (s) {
    case ASX_OBLIGATION_RESERVED: return "Reserved";
    case ASX_OBLIGATION_COMMITTED: return "Committed";
    case ASX_OBLIGATION_ABORTED: return "Aborted";
    case ASX_OBLIGATION_LEAKED: return "Leaked";
    default: return "Unknown";
    }
}
