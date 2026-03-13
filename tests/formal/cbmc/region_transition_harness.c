/*
 * region_transition_harness.c — CBMC harness for region transition check
 *
 * Verifies that asx_region_transition_check() correctly implements the
 * 5x5 region transition matrix for all inputs including out-of-range.
 *
 * CBMC:      cbmc region_transition_harness.c ../../src/core/transition_tables.c
 *            -I ../../../include -DCBMC --unwind 1
 * Standalone: gcc -std=c99 -I ../../../include -o region_harness
 *            region_transition_harness.c ../../src/core/transition_tables.c
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>

/* Encode the expected transition table directly in the harness
 * for independent cross-checking against the implementation. */
static const int expected[5][5] = {
    /* To:  Open  Clos  Drain Final Closed */
    /*Open*/ {0, 1, 0, 0, 0},
    /*Clos*/ {0, 0, 1, 1, 0},
    /*Drain*/ {0, 0, 0, 1, 0},
    /*Final*/ {0, 0, 0, 0, 1},
    /*Closed*/ {0, 0, 0, 0, 0}};

#ifdef CBMC

int main(void) {
    unsigned from = NONDET_UINT();
    unsigned to = NONDET_UINT();

    /* Case 1: in-range inputs */
    if (from <= 4 && to <= 4) {
        asx_status st = asx_region_transition_check((asx_region_state)from, (asx_region_state)to);
        if (expected[from][to]) {
            VERIFY(st == ASX_OK);
        } else {
            VERIFY(st == ASX_E_INVALID_TRANSITION);
        }
    }

    /* Case 2: out-of-range inputs */
    if (from > 4 || to > 4) {
        ASSUME(from <= 100 && to <= 100); /* bound for CBMC */
        asx_status st = asx_region_transition_check((asx_region_state)from, (asx_region_state)to);
        VERIFY(st == ASX_E_INVALID_ARGUMENT);
    }

    /* Case 3: terminal state has no outgoing edges */
    ASSUME(from == 4); /* CLOSED */
    {
        unsigned t = NONDET_UINT();
        ASSUME(t <= 4);
        asx_status st = asx_region_transition_check(ASX_REGION_CLOSED, (asx_region_state)t);
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

    fprintf(stderr, "[region-transition-harness] CBMC-compatible verification\n");

    /* Exhaustive in-range check */
    for (from = 0; from <= 4; from++) {
        for (to = 0; to <= 4; to++) {
            asx_status st =
                asx_region_transition_check((asx_region_state)from, (asx_region_state)to);
            if (expected[from][to]) {
                check(st == ASX_OK, "legal transition accepted", from, to);
            } else {
                check(st == ASX_E_INVALID_TRANSITION, "forbidden transition rejected", from, to);
            }
        }
    }

    /* Out-of-range check */
    for (from = 5; from <= 10; from++) {
        asx_status st = asx_region_transition_check((asx_region_state)from, (asx_region_state)0);
        check(st == ASX_E_INVALID_ARGUMENT, "out-of-range from rejected", from, 0);
    }
    for (to = 5; to <= 10; to++) {
        asx_status st = asx_region_transition_check((asx_region_state)0, (asx_region_state)to);
        check(st == ASX_E_INVALID_ARGUMENT, "out-of-range to rejected", 0, to);
    }

    /* Terminal state no-exit check */
    for (to = 0; to <= 4; to++) {
        asx_status st = asx_region_transition_check(ASX_REGION_CLOSED, (asx_region_state)to);
        check(st != ASX_OK, "terminal state has no outgoing", 4, to);
    }

    /* Forward progress: all legal transitions go forward */
    for (from = 0; from <= 4; from++) {
        for (to = 0; to <= 4; to++) {
            if (expected[from][to]) { check(to > from, "forward progress", from, to); }
        }
    }

    fprintf(stderr, "[region-transition-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
