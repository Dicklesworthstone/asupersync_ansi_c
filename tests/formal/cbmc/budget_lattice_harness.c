/*
 * budget_lattice_harness.c — CBMC harness for budget meet lattice laws
 *
 * Verifies that asx_budget_meet forms a meet-semilattice:
 *   L1. Commutativity: meet(a,b) == meet(b,a)
 *   L2. Associativity: meet(meet(a,b),c) == meet(a,meet(b,c))
 *   L3. Idempotence: meet(a,a) == a
 *   L4. Identity: meet(a,infinite) == a
 *   L5. Absorption: meet(a,zero) == zero
 *   L6. Tightening: meet(a,b).field <= min(a.field, b.field)
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

static int budget_eq(const asx_budget *a, const asx_budget *b) {
    return a->deadline == b->deadline && a->poll_quota == b->poll_quota &&
           a->cost_quota == b->cost_quota && a->priority == b->priority;
}

static uint32_t u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint64_t u64_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

/* Representative budget values */
static asx_budget make_samples(int idx) {
    asx_budget b;
    memset(&b, 0, sizeof(b));
    switch (idx) {
    case 0: return asx_budget_infinite();
    case 1: return asx_budget_zero();
    case 2: return asx_budget_from_polls(100);
    case 3: return asx_budget_from_polls(50);
    case 4:
        b.deadline = 1000;
        b.poll_quota = 200;
        b.cost_quota = 500;
        b.priority = 10;
        return b;
    case 5:
        b.deadline = 2000;
        b.poll_quota = 100;
        b.cost_quota = 1000;
        b.priority = 20;
        return b;
    case 6:
        b.deadline = 500;
        b.poll_quota = 300;
        b.cost_quota = 250;
        b.priority = 5;
        return b;
    default: return asx_budget_infinite();
    }
}

#define N_SAMPLES 7

#ifdef CBMC

int main(void) {
    unsigned ai = NONDET_UINT();
    unsigned bi = NONDET_UINT();
    ASSUME(ai < N_SAMPLES && bi < N_SAMPLES);

    asx_budget a = make_samples((int)ai);
    asx_budget b = make_samples((int)bi);
    asx_budget ab = asx_budget_meet(&a, &b);
    asx_budget ba = asx_budget_meet(&b, &a);

    /* L1: Commutativity */
    VERIFY(budget_eq(&ab, &ba));

    /* L3: Idempotence */
    {
        asx_budget aa = asx_budget_meet(&a, &a);
        VERIFY(budget_eq(&aa, &a));
    }

    /* L6: Tightening */
    VERIFY(ab.poll_quota <= u32_min(a.poll_quota, b.poll_quota));
    VERIFY(ab.cost_quota <= u64_min(a.cost_quota, b.cost_quota));

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
    int ai, bi, ci;
    asx_budget inf = asx_budget_infinite();
    asx_budget zero = asx_budget_zero();

    fprintf(stderr, "[budget-lattice-harness] CBMC-compatible verification\n");

    for (ai = 0; ai < N_SAMPLES; ai++) {
        asx_budget a = make_samples(ai);

        /* L3: Idempotence */
        {
            asx_budget aa = asx_budget_meet(&a, &a);
            check(budget_eq(&aa, &a), "idempotence");
        }

        /* L4: Identity (infinite) */
        {
            asx_budget a_inf = asx_budget_meet(&a, &inf);
            check(budget_eq(&a_inf, &a), "identity (right)");
            asx_budget inf_a = asx_budget_meet(&inf, &a);
            check(budget_eq(&inf_a, &a), "identity (left)");
        }

        /* L5: Absorption (zero) */
        {
            asx_budget a_z = asx_budget_meet(&a, &zero);
            check(budget_eq(&a_z, &zero), "absorption (right)");
            asx_budget z_a = asx_budget_meet(&zero, &a);
            check(budget_eq(&z_a, &zero), "absorption (left)");
        }

        for (bi = 0; bi < N_SAMPLES; bi++) {
            asx_budget b = make_samples(bi);
            asx_budget ab = asx_budget_meet(&a, &b);
            asx_budget ba = asx_budget_meet(&b, &a);

            /* L1: Commutativity */
            check(budget_eq(&ab, &ba), "commutativity");

            /* L6: Tightening */
            check(ab.poll_quota <= u32_min(a.poll_quota, b.poll_quota), "tighten poll_quota");
            check(ab.cost_quota <= u64_min(a.cost_quota, b.cost_quota), "tighten cost_quota");

            /* L2: Associativity */
            for (ci = 0; ci < N_SAMPLES; ci++) {
                /* ASX_CHECKPOINT_WAIVER("bounded enumeration over sample array") */
                asx_budget c = make_samples(ci);
                asx_budget ab_c = asx_budget_meet(&ab, &c);
                asx_budget bc = asx_budget_meet(&b, &c);
                asx_budget a_bc = asx_budget_meet(&a, &bc);
                check(budget_eq(&ab_c, &a_bc), "associativity");
            }
        }
    }

    fprintf(stderr, "[budget-lattice-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
