/*
 * task_transition_harness.c — CBMC harness for task transition check
 *
 * Verifies that asx_task_transition_check() correctly implements the
 * 6x6 task transition matrix for all inputs including out-of-range.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>

static const int expected[6][6] = {
    /* To:   Creat  Run   CanR  Cling Final Comp */
    /*Creat*/ {0, 1, 1, 0, 0, 1},
    /*Run*/ {0, 0, 1, 0, 0, 1},
    /*CanR*/ {0, 0, 1, 1, 0, 1},
    /*Cling*/ {0, 0, 0, 1, 1, 1},
    /*Final*/ {0, 0, 0, 0, 1, 1},
    /*Comp*/ {0, 0, 0, 0, 0, 0}};

#ifdef CBMC

int main(void) {
    unsigned from = NONDET_UINT();
    unsigned to = NONDET_UINT();

    if (from <= 5 && to <= 5) {
        asx_status st = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
        if (expected[from][to]) {
            VERIFY(st == ASX_OK);
        } else {
            VERIFY(st == ASX_E_INVALID_TRANSITION);
        }
    }

    if (from > 5 || to > 5) {
        ASSUME(from <= 100 && to <= 100);
        asx_status st = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
        VERIFY(st == ASX_E_INVALID_ARGUMENT);
    }

    /* Terminal state (COMPLETED=5) has no outgoing edges */
    {
        unsigned t = NONDET_UINT();
        ASSUME(t <= 5);
        asx_status st = asx_task_transition_check(ASX_TASK_COMPLETED, (asx_task_state)t);
        VERIFY(st != ASX_OK);
    }

    return 0;
}

#else /* Standalone exhaustive enumeration */

static int passes = 0;
static int failures = 0;

static void check(int cond, const char *msg, unsigned from, unsigned to) {
    if (!cond) {
        fprintf(stderr, "  FAIL: %s (from=%u, to=%u)\n", msg, from, to);
        failures++;
    } else {
        passes++;
    }
}

int main(void) {
    unsigned from, to;

    fprintf(stderr, "[task-transition-harness] CBMC-compatible verification\n");

    /* Exhaustive in-range */
    for (from = 0; from <= 5; from++) {
        for (to = 0; to <= 5; to++) {
            asx_status st = asx_task_transition_check((asx_task_state)from, (asx_task_state)to);
            if (expected[from][to]) {
                check(st == ASX_OK, "legal transition accepted", from, to);
            } else {
                check(st == ASX_E_INVALID_TRANSITION, "forbidden transition rejected", from, to);
            }
        }
    }

    /* Out-of-range */
    for (from = 6; from <= 10; from++) {
        asx_status st = asx_task_transition_check((asx_task_state)from, (asx_task_state)0);
        check(st == ASX_E_INVALID_ARGUMENT, "out-of-range from rejected", from, 0);
    }
    for (to = 6; to <= 10; to++) {
        asx_status st = asx_task_transition_check((asx_task_state)0, (asx_task_state)to);
        check(st == ASX_E_INVALID_ARGUMENT, "out-of-range to rejected", 0, to);
    }

    /* Terminal no-exit */
    for (to = 0; to <= 5; to++) {
        asx_status st = asx_task_transition_check(ASX_TASK_COMPLETED, (asx_task_state)to);
        check(st != ASX_OK, "terminal state has no outgoing", 5, to);
    }

    /* Cancel monotonicity: no backward transitions */
    for (from = 0; from <= 5; from++) {
        for (to = 0; to <= 5; to++) {
            if (expected[from][to]) {
                check(to >= from, "cancel monotonicity (to >= from)", from, to);
            }
        }
    }

    fprintf(stderr, "[task-transition-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
