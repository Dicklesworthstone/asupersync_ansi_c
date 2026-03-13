/*
 * obligation_linearity_harness.c — CBMC harness for obligation linearity
 *
 * Verifies:
 * 1. Only RESERVED has outgoing transitions (linearity)
 * 2. All terminal states are absorbing (no double-resolution)
 * 3. RESERVED can reach exactly 3 terminals
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>

static const int expected[4][4] = {
    /* To:    Res   Comm  Abort Leaked */
    /*Res*/ {0, 1, 1, 1},
    /*Comm*/ {0, 0, 0, 0},
    /*Abort*/ {0, 0, 0, 0},
    /*Leaked*/ {0, 0, 0, 0}};

#ifdef CBMC

int main(void) {
    unsigned from = NONDET_UINT();
    unsigned to = NONDET_UINT();

    /* Transition table agreement */
    if (from <= 3 && to <= 3) {
        asx_status st =
            asx_obligation_transition_check((asx_obligation_state)from, (asx_obligation_state)to);
        if (expected[from][to]) {
            VERIFY(st == ASX_OK);
        } else {
            VERIFY(st == ASX_E_INVALID_TRANSITION);
        }
    }

    /* Linearity: terminal states have NO outgoing transitions */
    {
        unsigned term = NONDET_UINT();
        unsigned t = NONDET_UINT();
        ASSUME(term >= 1 && term <= 3); /* COMMITTED, ABORTED, LEAKED */
        ASSUME(t <= 3);
        asx_status st =
            asx_obligation_transition_check((asx_obligation_state)term, (asx_obligation_state)t);
        VERIFY(st != ASX_OK);
    }

    /* Terminal predicate agreement */
    {
        unsigned s = NONDET_UINT();
        ASSUME(s <= 3);
        int is_term = asx_obligation_is_terminal((asx_obligation_state)s);
        /* Terminal iff not RESERVED */
        VERIFY(is_term == (s != 0));
    }

    return 0;
}

#else /* Standalone */

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

    fprintf(stderr, "[obligation-linearity-harness] CBMC-compatible verification\n");

    /* Exhaustive transition table */
    for (from = 0; from <= 3; from++) {
        for (to = 0; to <= 3; to++) {
            asx_status st = asx_obligation_transition_check((asx_obligation_state)from,
                                                            (asx_obligation_state)to);
            if (expected[from][to]) {
                check(st == ASX_OK, "legal transition accepted", from, to);
            } else {
                check(st == ASX_E_INVALID_TRANSITION, "forbidden transition rejected", from, to);
            }
        }
    }

    /* Linearity: each terminal state is absorbing */
    for (from = 1; from <= 3; from++) {
        for (to = 0; to <= 3; to++) {
            asx_status st = asx_obligation_transition_check((asx_obligation_state)from,
                                                            (asx_obligation_state)to);
            check(st != ASX_OK, "terminal absorbing", from, to);
        }
    }

    /* RESERVED can reach exactly 3 terminals */
    {
        int reachable = 0;
        for (to = 0; to <= 3; to++) {
            if (asx_obligation_transition_check(ASX_OBLIGATION_RESERVED,
                                                (asx_obligation_state)to) == ASX_OK) {
                reachable++;
            }
        }
        check(reachable == 3, "RESERVED reaches exactly 3 terminals", 0, 0);
    }

    /* Terminal predicate consistency */
    for (from = 0; from <= 3; from++) {
        int is_term = asx_obligation_is_terminal((asx_obligation_state)from);
        check(is_term == (from != 0), "terminal predicate matches", from, 0);
    }

    fprintf(stderr, "[obligation-linearity-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
