/*
 * asx/core/transition.h — state machine transition authority tables
 *
 * Transition legality is table-driven. Every state transition is validated
 * against the authority table before execution. Illegal transitions produce
 * ASX_E_INVALID_TRANSITION.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_TRANSITION_H
#define ASX_CORE_TRANSITION_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>

/* Region lifecycle states */
typedef enum {
    ASX_REGION_OPEN       = 0,
    ASX_REGION_CLOSING    = 1,
    ASX_REGION_DRAINING   = 2,
    ASX_REGION_FINALIZING = 3,
    ASX_REGION_CLOSED     = 4
} asx_region_state;

/* Task lifecycle states */
typedef enum {
    ASX_TASK_CREATED          = 0,
    ASX_TASK_RUNNING          = 1,
    ASX_TASK_CANCEL_REQUESTED = 2,
    ASX_TASK_CANCELLING       = 3,
    ASX_TASK_FINALIZING       = 4,
    ASX_TASK_COMPLETED        = 5
} asx_task_state;

/* Obligation lifecycle states */
typedef enum {
    ASX_OBLIGATION_RESERVED  = 0,
    ASX_OBLIGATION_COMMITTED = 1,
    ASX_OBLIGATION_ABORTED   = 2,
    ASX_OBLIGATION_LEAKED    = 3
} asx_obligation_state;

/* Validate a region state transition. Returns ASX_OK or ASX_E_INVALID_TRANSITION. */
ASX_API asx_status asx_region_transition_check(asx_region_state from, asx_region_state to);

/* Validate a task state transition. Returns ASX_OK or ASX_E_INVALID_TRANSITION. */
ASX_API asx_status asx_task_transition_check(asx_task_state from, asx_task_state to);

/* Validate an obligation state transition. Returns ASX_OK or ASX_E_INVALID_TRANSITION. */
ASX_API asx_status asx_obligation_transition_check(asx_obligation_state from, asx_obligation_state to);

/* Query functions */
ASX_API int asx_region_can_spawn(asx_region_state s);
ASX_API int asx_region_can_accept_work(asx_region_state s);
ASX_API int asx_region_is_closing(asx_region_state s);
ASX_API int asx_region_is_terminal(asx_region_state s);
ASX_API int asx_task_is_terminal(asx_task_state s);
ASX_API int asx_obligation_is_terminal(asx_obligation_state s);

/* Human-readable state names */
ASX_API const char *asx_region_state_str(asx_region_state s);
ASX_API const char *asx_task_state_str(asx_task_state s);
ASX_API const char *asx_obligation_state_str(asx_obligation_state s);

#endif /* ASX_CORE_TRANSITION_H */
