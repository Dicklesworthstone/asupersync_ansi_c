/*
 * test_bounded_model.c — Bounded model-check for state machine properties (bd-66l.10)
 *
 * Exhaustively enumerates all state pairs for region, task, and obligation
 * lifecycles and verifies critical safety invariants:
 *
 *   1. Terminal states have no legal outgoing transitions
 *   2. All legal transitions preserve forward progress
 *   3. Cancel phase monotonicity (severity never regresses)
 *   4. Obligation linearity (no double-resolve)
 *   5. Region spawn prohibition in closing/terminal states
 *   6. Transition table completeness (every pair has a verdict)
 *
 * This is a compile-and-run exhaustive check, not a symbolic model checker,
 * but the state spaces are small enough (5×5, 6×6, 4×4) for full enumeration.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() — bounded iteration over small state enums */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/cancel.h>
#include <asx/core/transition.h>

/* -------------------------------------------------------------------
 * State count constants
 * ------------------------------------------------------------------- */

#define REGION_STATE_COUNT 5 /* OPEN..CLOSED */
#define TASK_STATE_COUNT 6   /* CREATED..COMPLETED */
#define OBLIG_STATE_COUNT 4  /* RESERVED..LEAKED */

/* -------------------------------------------------------------------
 * Invariant 1: Terminal states have no legal outgoing transitions
 *
 * Region CLOSED, Task COMPLETED, Obligation COMMITTED/ABORTED/LEAKED
 * must reject all transitions to any state.
 * ------------------------------------------------------------------- */

TEST(region_terminal_no_outgoing) {
    int to;
    for (to = 0; to < REGION_STATE_COUNT; to++) {
        asx_status st = asx_region_transition_check(ASX_REGION_CLOSED, (asx_region_state)to);
        if (to == ASX_REGION_CLOSED) {
            /* Self-transition on terminal is also illegal */
            ASSERT_NE(st, ASX_OK);
        } else {
            ASSERT_NE(st, ASX_OK);
        }
    }
}

TEST(task_terminal_no_outgoing) {
    int to;
    for (to = 0; to < TASK_STATE_COUNT; to++) {
        asx_status st = asx_task_transition_check(ASX_TASK_COMPLETED, (asx_task_state)to);
        ASSERT_NE(st, ASX_OK);
    }
}

TEST(obligation_terminal_no_outgoing) {
    /* COMMITTED, ABORTED, and LEAKED are terminal */
    asx_obligation_state terminals[] = {ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_ABORTED,
                                        ASX_OBLIGATION_LEAKED};
    int t, to;
    for (t = 0; t < 3; t++) {
        for (to = 0; to < OBLIG_STATE_COUNT; to++) {
            asx_status st = asx_obligation_transition_check(terminals[t], (asx_obligation_state)to);
            ASSERT_NE(st, ASX_OK);
        }
    }
}

/* -------------------------------------------------------------------
 * Invariant 2: Forward progress — legal transitions go forward in
 * the lifecycle, never backward (except cancel phase expansion)
 *
 * Region: OPEN(0) → CLOSING(1) → DRAINING(2) → FINALIZING(3) → CLOSED(4)
 * All legal transitions have to > from.
 * ------------------------------------------------------------------- */

TEST(region_transitions_always_forward) {
    int from, to;
    for (from = 0; from < REGION_STATE_COUNT; from++) {
        for (to = 0; to < REGION_STATE_COUNT; to++) {
            asx_status st =
                asx_region_transition_check((asx_region_state)from, (asx_region_state)to);
            if (st == ASX_OK) {
                /* Legal transitions must go forward */
                ASSERT_TRUE(to > from);
            }
        }
    }
}

/* -------------------------------------------------------------------
 * Invariant 3: Task cancel phase monotonicity
 *
 * The task lifecycle has a linear ordering where cancel states
 * are "higher" than normal states. Once in a cancel state,
 * the task can only move forward through cancel phases or complete.
 *
 * CREATED(0) → RUNNING(1) → CANCEL_REQUESTED(2) →
 *   CANCELLING(3) → FINALIZING(4) → COMPLETED(5)
 * ------------------------------------------------------------------- */

TEST(task_transitions_never_regress) {
    int from, to;
    for (from = 0; from < TASK_STATE_COUNT; from++) {
        for (to = 0; to < TASK_STATE_COUNT; to++) {
            asx_status st = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
            if (st == ASX_OK) {
                /* Legal transitions must go forward or self-loop (strengthen) */
                ASSERT_TRUE(to >= from);
            }
        }
    }
}

/* -------------------------------------------------------------------
 * Invariant 4: Obligation linearity — RESERVED can go to exactly
 * one of {COMMITTED, ABORTED, LEAKED}. No double-resolve.
 * ------------------------------------------------------------------- */

TEST(obligation_exactly_one_terminal) {
    /* RESERVED can transition to COMMITTED or ABORTED (or LEAKED on cleanup) */
    int to;
    int legal_count = 0;
    for (to = 0; to < OBLIG_STATE_COUNT; to++) {
        asx_status st =
            asx_obligation_transition_check(ASX_OBLIGATION_RESERVED, (asx_obligation_state)to);
        if (st == ASX_OK) {
            legal_count++;
            /* Must transition to a terminal state */
            ASSERT_TRUE(asx_obligation_is_terminal((asx_obligation_state)to));
        }
    }
    /* At least 2 legal terminals (COMMITTED and ABORTED); LEAKED may also be legal */
    ASSERT_TRUE(legal_count >= 2);
    ASSERT_TRUE(legal_count <= 3); /* COMMITTED, ABORTED, optionally LEAKED */
}

TEST(obligation_no_double_resolve) {
    /* Once in COMMITTED, cannot go to ABORTED and vice versa */
    asx_status st;
    st = asx_obligation_transition_check(ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_ABORTED);
    ASSERT_NE(st, ASX_OK);

    st = asx_obligation_transition_check(ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_COMMITTED);
    ASSERT_NE(st, ASX_OK);

    /* No transition from COMMITTED back to RESERVED */
    st = asx_obligation_transition_check(ASX_OBLIGATION_COMMITTED, ASX_OBLIGATION_RESERVED);
    ASSERT_NE(st, ASX_OK);

    /* No transition from ABORTED back to RESERVED */
    st = asx_obligation_transition_check(ASX_OBLIGATION_ABORTED, ASX_OBLIGATION_RESERVED);
    ASSERT_NE(st, ASX_OK);
}

/* -------------------------------------------------------------------
 * Invariant 5: Region spawn prohibition
 *
 * Spawning tasks is only allowed in OPEN state.
 * All other states must reject spawn.
 * ------------------------------------------------------------------- */

TEST(region_spawn_only_in_open) {
    int s;
    for (s = 0; s < REGION_STATE_COUNT; s++) {
        int can = asx_region_can_spawn((asx_region_state)s);
        if (s == ASX_REGION_OPEN) {
            ASSERT_TRUE(can);
        } else {
            ASSERT_TRUE(!can);
        }
    }
}

TEST(region_work_acceptance) {
    /* Work is accepted in OPEN (normal) and FINALIZING (late arrivals) */
    int s;
    for (s = 0; s < REGION_STATE_COUNT; s++) {
        int can = asx_region_can_accept_work((asx_region_state)s);
        if (s == ASX_REGION_OPEN || s == ASX_REGION_FINALIZING) {
            ASSERT_TRUE(can);
        } else {
            ASSERT_TRUE(!can);
        }
    }
}

/* -------------------------------------------------------------------
 * Invariant 6: Transition table completeness
 *
 * Every (from, to) pair must return either ASX_OK or
 * ASX_E_INVALID_TRANSITION. No other status codes allowed.
 * ------------------------------------------------------------------- */

TEST(region_table_completeness) {
    int from, to;
    for (from = 0; from < REGION_STATE_COUNT; from++) {
        for (to = 0; to < REGION_STATE_COUNT; to++) {
            asx_status st =
                asx_region_transition_check((asx_region_state)from, (asx_region_state)to);
            ASSERT_TRUE(st == ASX_OK || st == ASX_E_INVALID_TRANSITION);
        }
    }
}

TEST(task_table_completeness) {
    int from, to;
    for (from = 0; from < TASK_STATE_COUNT; from++) {
        for (to = 0; to < TASK_STATE_COUNT; to++) {
            asx_status st = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
            ASSERT_TRUE(st == ASX_OK || st == ASX_E_INVALID_TRANSITION);
        }
    }
}

TEST(obligation_table_completeness) {
    int from, to;
    for (from = 0; from < OBLIG_STATE_COUNT; from++) {
        for (to = 0; to < OBLIG_STATE_COUNT; to++) {
            asx_status st = asx_obligation_transition_check((asx_obligation_state)from,
                                                            (asx_obligation_state)to);
            ASSERT_TRUE(st == ASX_OK || st == ASX_E_INVALID_TRANSITION);
        }
    }
}

/* -------------------------------------------------------------------
 * Invariant 7: Terminal state predicates agree with transition table
 *
 * If is_terminal(s) is true, no outgoing transitions exist.
 * If is_terminal(s) is false, at least one outgoing transition exists.
 * ------------------------------------------------------------------- */

TEST(region_terminal_predicate_consistent) {
    int s;
    for (s = 0; s < REGION_STATE_COUNT; s++) {
        int is_term = asx_region_is_terminal((asx_region_state)s);
        int has_outgoing = 0;
        int to;
        for (to = 0; to < REGION_STATE_COUNT; to++) {
            if (asx_region_transition_check((asx_region_state)s, (asx_region_state)to) == ASX_OK) {
                has_outgoing = 1;
            }
        }
        if (is_term) {
            ASSERT_TRUE(!has_outgoing);
        } else {
            ASSERT_TRUE(has_outgoing);
        }
    }
}

TEST(task_terminal_predicate_consistent) {
    int s;
    for (s = 0; s < TASK_STATE_COUNT; s++) {
        int is_term = asx_task_is_terminal((asx_task_state)s);
        int has_outgoing = 0;
        int to;
        for (to = 0; to < TASK_STATE_COUNT; to++) {
            if (asx_task_transition_check((asx_task_state)s, (asx_task_state)to) == ASX_OK) {
                has_outgoing = 1;
            }
        }
        if (is_term) {
            ASSERT_TRUE(!has_outgoing);
        } else {
            ASSERT_TRUE(has_outgoing);
        }
    }
}

TEST(obligation_terminal_predicate_consistent) {
    int s;
    for (s = 0; s < OBLIG_STATE_COUNT; s++) {
        int is_term = asx_obligation_is_terminal((asx_obligation_state)s);
        int has_outgoing = 0;
        int to;
        for (to = 0; to < OBLIG_STATE_COUNT; to++) {
            if (asx_obligation_transition_check((asx_obligation_state)s,
                                                (asx_obligation_state)to) == ASX_OK) {
                has_outgoing = 1;
            }
        }
        if (is_term) {
            ASSERT_TRUE(!has_outgoing);
        } else {
            ASSERT_TRUE(has_outgoing);
        }
    }
}

/* -------------------------------------------------------------------
 * Invariant 8: Cancel severity is monotonic
 *
 * Cancellation kinds have severities from USER(0) to SHUTDOWN(10).
 * The cancel protocol must never accept a lower-severity cancellation
 * after a higher-severity one was already in effect.
 * ------------------------------------------------------------------- */

TEST(cancel_severity_monotonic_property) {
    /* Verify severity ordering matches enum ordering */
    int i;
    for (i = 0; i < (int)ASX_CANCEL_SHUTDOWN; i++) {
        int sev_i = asx_cancel_severity((asx_cancel_kind)i);
        int sev_next = asx_cancel_severity((asx_cancel_kind)(i + 1));
        /* Each successive kind has >= severity */
        ASSERT_TRUE(sev_next >= sev_i);
    }
}

/* -------------------------------------------------------------------
 * Invariant 9: State name strings are non-null for all valid states
 * ------------------------------------------------------------------- */

TEST(state_names_complete) {
    int s;
    for (s = 0; s < REGION_STATE_COUNT; s++) {
        const char *name = asx_region_state_str((asx_region_state)s);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(name[0] != '\0');
    }
    for (s = 0; s < TASK_STATE_COUNT; s++) {
        const char *name = asx_task_state_str((asx_task_state)s);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(name[0] != '\0');
    }
    for (s = 0; s < OBLIG_STATE_COUNT; s++) {
        const char *name = asx_obligation_state_str((asx_obligation_state)s);
        ASSERT_TRUE(name != NULL);
        ASSERT_TRUE(name[0] != '\0');
    }
}

/* -------------------------------------------------------------------
 * Invariant 10: Reachability — all non-initial states are reachable
 * from the initial state via legal transitions.
 * ------------------------------------------------------------------- */

TEST(region_all_states_reachable) {
    int reached[REGION_STATE_COUNT] = {0};
    int queue[REGION_STATE_COUNT];
    int head = 0, tail = 0;
    int s;

    /* BFS from OPEN */
    reached[ASX_REGION_OPEN] = 1;
    queue[tail++] = ASX_REGION_OPEN;

    while (head < tail) {
        int from = queue[head++];
        int to;
        for (to = 0; to < REGION_STATE_COUNT; to++) {
            if (!reached[to] && asx_region_transition_check((asx_region_state)from,
                                                            (asx_region_state)to) == ASX_OK) {
                reached[to] = 1;
                queue[tail++] = to;
            }
        }
    }

    for (s = 0; s < REGION_STATE_COUNT; s++) { ASSERT_TRUE(reached[s]); }
}

TEST(task_all_states_reachable) {
    int reached[TASK_STATE_COUNT] = {0};
    int queue[TASK_STATE_COUNT];
    int head = 0, tail = 0;
    int s;

    /* BFS from CREATED */
    reached[ASX_TASK_CREATED] = 1;
    queue[tail++] = ASX_TASK_CREATED;

    while (head < tail) {
        int from = queue[head++];
        int to;
        for (to = 0; to < TASK_STATE_COUNT; to++) {
            if (!reached[to] &&
                asx_task_transition_check((asx_task_state)from, (asx_task_state)to) == ASX_OK) {
                reached[to] = 1;
                queue[tail++] = to;
            }
        }
    }

    for (s = 0; s < TASK_STATE_COUNT; s++) { ASSERT_TRUE(reached[s]); }
}

/* -------------------------------------------------------------------
 * Test runner
 * ------------------------------------------------------------------- */

int main(void) {
    /* Invariant 1: Terminal no-outgoing */
    RUN_TEST(region_terminal_no_outgoing);
    RUN_TEST(task_terminal_no_outgoing);
    RUN_TEST(obligation_terminal_no_outgoing);

    /* Invariant 2: Forward progress */
    RUN_TEST(region_transitions_always_forward);

    /* Invariant 3: Cancel monotonicity */
    RUN_TEST(task_transitions_never_regress);

    /* Invariant 4: Obligation linearity */
    RUN_TEST(obligation_exactly_one_terminal);
    RUN_TEST(obligation_no_double_resolve);

    /* Invariant 5: Region spawn prohibition */
    RUN_TEST(region_spawn_only_in_open);
    RUN_TEST(region_work_acceptance);

    /* Invariant 6: Table completeness */
    RUN_TEST(region_table_completeness);
    RUN_TEST(task_table_completeness);
    RUN_TEST(obligation_table_completeness);

    /* Invariant 7: Terminal predicate consistency */
    RUN_TEST(region_terminal_predicate_consistent);
    RUN_TEST(task_terminal_predicate_consistent);
    RUN_TEST(obligation_terminal_predicate_consistent);

    /* Invariant 8: Cancel severity monotonicity */
    RUN_TEST(cancel_severity_monotonic_property);

    /* Invariant 9: State name completeness */
    RUN_TEST(state_names_complete);

    /* Invariant 10: Reachability */
    RUN_TEST(region_all_states_reachable);
    RUN_TEST(task_all_states_reachable);

    TEST_REPORT();
    return test_failures > 0 ? 1 : 0;
}
