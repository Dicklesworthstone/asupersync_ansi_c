/*
 * test_budget_lattice.c — algebraic property verification for budget meet
 *
 * Budget meet is componentwise tightening (min). This test verifies
 * identity, absorption, commutativity, associativity, and idempotence
 * on representative budget values.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Algebraic property test — exhaustive enumeration") */

#include "../../test_harness.h"
#include <asx/asx.h>

/* Representative budget values for property testing */
static asx_budget samples[6];
static int n_samples;

static void init_samples(void) {
    /* 0: infinite (identity) */
    samples[0] = asx_budget_infinite();
    /* 1: zero (absorbing) */
    samples[1] = asx_budget_zero();
    /* 2: from polls */
    samples[2] = asx_budget_from_polls(100);
    /* 3: from polls (different) */
    samples[3] = asx_budget_from_polls(50);
    /* 4: custom */
    samples[4].deadline = 1000;
    samples[4].poll_quota = 200;
    samples[4].cost_quota = 500;
    samples[4].priority = 10;
    /* 5: custom (different) */
    samples[5].deadline = 2000;
    samples[5].poll_quota = 100;
    samples[5].cost_quota = 1000;
    samples[5].priority = 20;
    n_samples = 6;
}

static int budget_eq(const asx_budget *a, const asx_budget *b) {
    return a->deadline == b->deadline && a->poll_quota == b->poll_quota &&
           a->cost_quota == b->cost_quota && a->priority == b->priority;
}

/* --- Identity: meet(infinite, x) == x --- */
TEST(budget_meet_identity) {
    int i;
    asx_budget inf = asx_budget_infinite();
    for (i = 0; i < n_samples; i++) {
        asx_budget result = asx_budget_meet(&inf, &samples[i]);
        ASSERT_TRUE(budget_eq(&result, &samples[i]));
    }
}

/* --- Identity (right): meet(x, infinite) == x --- */
TEST(budget_meet_identity_right) {
    int i;
    asx_budget inf = asx_budget_infinite();
    for (i = 0; i < n_samples; i++) {
        asx_budget result = asx_budget_meet(&samples[i], &inf);
        ASSERT_TRUE(budget_eq(&result, &samples[i]));
    }
}

/* --- Absorption: meet(zero, x) is exhausted --- */
TEST(budget_meet_absorption) {
    int i;
    asx_budget zero = asx_budget_zero();
    for (i = 0; i < n_samples; i++) {
        asx_budget result = asx_budget_meet(&zero, &samples[i]);
        ASSERT_TRUE(asx_budget_is_exhausted(&result));
    }
}

/* --- Commutativity: meet(a,b) == meet(b,a) --- */
TEST(budget_meet_commutativity) {
    int i, j;
    for (i = 0; i < n_samples; i++) {
        for (j = 0; j < n_samples; j++) {
            asx_budget ab = asx_budget_meet(&samples[i], &samples[j]);
            asx_budget ba = asx_budget_meet(&samples[j], &samples[i]);
            ASSERT_TRUE(budget_eq(&ab, &ba));
        }
    }
}

/* --- Associativity: meet(meet(a,b),c) == meet(a,meet(b,c)) --- */
TEST(budget_meet_associativity) {
    int i, j, k;
    for (i = 0; i < n_samples; i++) {
        for (j = 0; j < n_samples; j++) {
            for (k = 0; k < n_samples; k++) {
                asx_budget ab = asx_budget_meet(&samples[i], &samples[j]);
                asx_budget ab_c = asx_budget_meet(&ab, &samples[k]);
                asx_budget bc = asx_budget_meet(&samples[j], &samples[k]);
                asx_budget a_bc = asx_budget_meet(&samples[i], &bc);
                ASSERT_TRUE(budget_eq(&ab_c, &a_bc));
            }
        }
    }
}

/* --- Idempotence: meet(a,a) == a --- */
TEST(budget_meet_idempotence) {
    int i;
    for (i = 0; i < n_samples; i++) {
        asx_budget result = asx_budget_meet(&samples[i], &samples[i]);
        ASSERT_TRUE(budget_eq(&result, &samples[i]));
    }
}

/* --- Narrowing: meet(a,b) is at least as tight as each operand --- */
TEST(budget_meet_narrowing) {
    int i, j;
    for (i = 0; i < n_samples; i++) {
        for (j = 0; j < n_samples; j++) {
            asx_budget result = asx_budget_meet(&samples[i], &samples[j]);
            /* Each component of result <= corresponding component of each operand */
            ASSERT_TRUE(result.poll_quota <= samples[i].poll_quota);
            ASSERT_TRUE(result.poll_quota <= samples[j].poll_quota);
            ASSERT_TRUE(result.cost_quota <= samples[i].cost_quota);
            ASSERT_TRUE(result.cost_quota <= samples[j].cost_quota);
        }
    }
}

/* --- Exhaustion propagation: if either is exhausted, result is exhausted --- */
TEST(budget_meet_exhaustion_propagation) {
    int i;
    asx_budget zero = asx_budget_zero();
    for (i = 0; i < n_samples; i++) {
        asx_budget result = asx_budget_meet(&zero, &samples[i]);
        ASSERT_TRUE(asx_budget_is_exhausted(&result));
        result = asx_budget_meet(&samples[i], &zero);
        ASSERT_TRUE(asx_budget_is_exhausted(&result));
    }
}

int main(void) {
    init_samples();

    fprintf(stderr, "[formal] budget meet lattice algebraic properties\n");

    RUN_TEST(budget_meet_identity);
    RUN_TEST(budget_meet_identity_right);
    RUN_TEST(budget_meet_absorption);
    RUN_TEST(budget_meet_commutativity);
    RUN_TEST(budget_meet_associativity);
    RUN_TEST(budget_meet_idempotence);
    RUN_TEST(budget_meet_narrowing);
    RUN_TEST(budget_meet_exhaustion_propagation);

    TEST_REPORT();
    return test_failures;
}
