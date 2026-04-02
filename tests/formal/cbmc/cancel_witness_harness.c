/*
 * cancel_witness_harness.c — CBMC harness for cancel witness monotonicity
 *
 * Proves that the cancel witness phase protocol enforces monotone
 * non-decreasing phase transitions:
 *   REQUESTED(0) → CANCELLING(1) → FINALIZING(2) → COMPLETED(3)
 *
 * Properties verified:
 *   1. Phase after successful advance is strictly greater than before.
 *   2. Backward transitions are always rejected.
 *   3. Same-phase transitions are always rejected.
 *   4. Released witnesses reject all subsequent operations.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>

#define PHASE_COUNT 4

#ifdef CBMC

int main(void) {
    asx_cancel_witness_id w;
    asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "cbmc", NULL, 0};
    asx_cancel_phase current, target;

    asx_runtime_reset();

    /* Create witness — starts at REQUESTED */
    VERIFY(asx_cancel_witness_create(&w, 1, &reason) == ASX_OK);

    /* Property 1: advance to nondeterministic phase */
    {
        unsigned t = NONDET_UINT();
        ASSUME(t < PHASE_COUNT);
        target = (asx_cancel_phase)t;

        asx_cancel_witness_phase(w, &current);
        asx_status st = asx_cancel_witness_advance(w, target);
        if (st == ASX_OK) {
            asx_cancel_phase after;
            asx_cancel_witness_phase(w, &after);
            VERIFY((int)after > (int)current);
            VERIFY(after == target);
        } else {
            VERIFY(st == ASX_E_INVALID_TRANSITION);
            VERIFY((int)target <= (int)current);
        }
    }

    asx_cancel_witness_release(w);
    return 0;
}

#else /* Standalone exhaustive enumeration */

static int passes = 0;
static int failures = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL: %s\n", msg);
        failures++;
    } else {
        passes++;
    }
}

int main(void) {
    unsigned start_phase, target;

    fprintf(stderr, "[cancel-witness-harness] CBMC-compatible verification\n");

    /* Exhaustive: for each starting phase, try all target phases */
    for (start_phase = 0; start_phase < PHASE_COUNT; start_phase++) {
        for (target = 0; target < PHASE_COUNT; target++) {
            asx_cancel_witness_id w;
            asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "test", NULL, 0};
            asx_cancel_phase current_phase;
            unsigned step;

            asx_runtime_reset();
            check(asx_cancel_witness_create(&w, 1, &reason) == ASX_OK, "create ok");

            /* Advance to start_phase */
            for (step = 0; step < start_phase; step++) {
                asx_status adv = asx_cancel_witness_advance(w, (asx_cancel_phase)(step + 1));
                check(adv == ASX_OK, "advance to start phase");
            }

            check(asx_cancel_witness_phase(w, &current_phase) == ASX_OK, "phase query");
            check(current_phase == (asx_cancel_phase)start_phase, "at expected start");

            /* Try advancing to target */
            {
                asx_status st = asx_cancel_witness_advance(w, (asx_cancel_phase)target);
                if (target > start_phase) {
                    check(st == ASX_OK, "forward advance accepted");
                    check(asx_cancel_witness_phase(w, &current_phase) == ASX_OK, "phase after");
                    check(current_phase == (asx_cancel_phase)target, "phase matches target");
                } else {
                    check(st == ASX_E_INVALID_TRANSITION, "non-forward rejected");
                }
            }

            asx_cancel_witness_release(w);
        }
    }

    /* Property: released witness rejects all operations */
    {
        asx_cancel_witness_id w;
        asx_cancel_reason reason = {ASX_CANCEL_USER, 0, 0, 1, "rel", NULL, 0};
        asx_cancel_phase p;

        asx_runtime_reset();
        check(asx_cancel_witness_create(&w, 1, &reason) == ASX_OK, "create for release");
        check(asx_cancel_witness_release(w) == ASX_OK, "release ok");
        check(asx_cancel_witness_advance(w, ASX_CANCEL_PHASE_CANCELLING) == ASX_E_NOT_FOUND,
              "advance after release rejected");
        check(asx_cancel_witness_phase(w, &p) == ASX_E_NOT_FOUND, "phase after release rejected");
        check(!asx_cancel_witness_is_valid(w), "not valid after release");
    }

    fprintf(stderr, "[cancel-witness-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
