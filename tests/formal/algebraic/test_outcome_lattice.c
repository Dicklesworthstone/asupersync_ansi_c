/*
 * test_outcome_lattice.c — exhaustive algebraic property verification
 *                          for the outcome severity join semilattice
 *
 * Domain: 4 severity values (OK=0, ERR=1, CANCELLED=2, PANICKED=3)
 * Total: 16 pairs (commutativity), 64 triples (associativity)
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Algebraic property test — exhaustive enumeration") */

#include "../../test_harness.h"
#include <asx/asx.h>

static asx_outcome make_outcome(asx_outcome_severity sev) {
    asx_outcome o;
    o.severity = sev;
    return o;
}

/* --- Commutativity: severity(join(a,b)) == severity(join(b,a)) --- */
TEST(outcome_join_commutativity) {
    int a, b;
    for (a = 0; a <= 3; a++) {
        for (b = 0; b <= 3; b++) {
            asx_outcome oa = make_outcome((asx_outcome_severity)a);
            asx_outcome ob = make_outcome((asx_outcome_severity)b);
            asx_outcome ab = asx_outcome_join(&oa, &ob);
            asx_outcome ba = asx_outcome_join(&ob, &oa);
            ASSERT_EQ((int)ab.severity, (int)ba.severity);
        }
    }
}

/* --- Associativity: severity(join(join(a,b),c)) == severity(join(a,join(b,c))) --- */
TEST(outcome_join_associativity) {
    int a, b, c;
    for (a = 0; a <= 3; a++) {
        for (b = 0; b <= 3; b++) {
            for (c = 0; c <= 3; c++) {
                asx_outcome oa = make_outcome((asx_outcome_severity)a);
                asx_outcome ob = make_outcome((asx_outcome_severity)b);
                asx_outcome oc = make_outcome((asx_outcome_severity)c);
                asx_outcome ab = asx_outcome_join(&oa, &ob);
                asx_outcome ab_c = asx_outcome_join(&ab, &oc);
                asx_outcome bc = asx_outcome_join(&ob, &oc);
                asx_outcome a_bc = asx_outcome_join(&oa, &bc);
                ASSERT_EQ((int)ab_c.severity, (int)a_bc.severity);
            }
        }
    }
}

/* --- Idempotence: join(a,a) == a --- */
TEST(outcome_join_idempotence) {
    int a;
    for (a = 0; a <= 3; a++) {
        asx_outcome oa = make_outcome((asx_outcome_severity)a);
        asx_outcome aa = asx_outcome_join(&oa, &oa);
        ASSERT_EQ((int)aa.severity, a);
    }
}

/* --- Identity: join(OK, x) == x --- */
TEST(outcome_join_identity) {
    int x;
    asx_outcome ok = make_outcome(ASX_OUTCOME_OK);
    for (x = 0; x <= 3; x++) {
        asx_outcome ox = make_outcome((asx_outcome_severity)x);
        asx_outcome result = asx_outcome_join(&ok, &ox);
        ASSERT_EQ((int)result.severity, x);
    }
}

/* --- Absorption: join(PANICKED, x).severity == PANICKED --- */
TEST(outcome_join_absorption) {
    int x;
    asx_outcome panicked = make_outcome(ASX_OUTCOME_PANICKED);
    for (x = 0; x <= 3; x++) {
        asx_outcome ox = make_outcome((asx_outcome_severity)x);
        asx_outcome result = asx_outcome_join(&panicked, &ox);
        ASSERT_EQ((int)result.severity, (int)ASX_OUTCOME_PANICKED);
    }
}

/* --- Monotonicity: severity(join(a,b)) >= max(severity(a), severity(b)) --- */
TEST(outcome_join_monotonicity) {
    int a, b;
    for (a = 0; a <= 3; a++) {
        for (b = 0; b <= 3; b++) {
            asx_outcome oa = make_outcome((asx_outcome_severity)a);
            asx_outcome ob = make_outcome((asx_outcome_severity)b);
            asx_outcome ab = asx_outcome_join(&oa, &ob);
            int expected_max = a > b ? a : b;
            ASSERT_TRUE((int)ab.severity >= expected_max);
        }
    }
}

/* --- Exactness: severity(join(a,b)) == max(severity(a), severity(b)) --- */
TEST(outcome_join_is_max) {
    int a, b;
    for (a = 0; a <= 3; a++) {
        for (b = 0; b <= 3; b++) {
            asx_outcome oa = make_outcome((asx_outcome_severity)a);
            asx_outcome ob = make_outcome((asx_outcome_severity)b);
            asx_outcome ab = asx_outcome_join(&oa, &ob);
            int expected_max = a > b ? a : b;
            ASSERT_EQ((int)ab.severity, expected_max);
        }
    }
}

/* --- NULL handling: join(NULL, x) == x, join(x, NULL) == x, join(NULL,NULL) == OK --- */
TEST(outcome_join_null_handling) {
    int x;
    for (x = 0; x <= 3; x++) {
        asx_outcome ox = make_outcome((asx_outcome_severity)x);
        asx_outcome null_x = asx_outcome_join(NULL, &ox);
        asx_outcome x_null = asx_outcome_join(&ox, NULL);
        ASSERT_EQ((int)null_x.severity, x);
        ASSERT_EQ((int)x_null.severity, x);
    }
    {
        asx_outcome null_null = asx_outcome_join(NULL, NULL);
        ASSERT_EQ((int)null_null.severity, (int)ASX_OUTCOME_OK);
    }
}

int main(void) {
    fprintf(stderr, "[formal] outcome lattice algebraic properties\n");

    RUN_TEST(outcome_join_commutativity);
    RUN_TEST(outcome_join_associativity);
    RUN_TEST(outcome_join_idempotence);
    RUN_TEST(outcome_join_identity);
    RUN_TEST(outcome_join_absorption);
    RUN_TEST(outcome_join_monotonicity);
    RUN_TEST(outcome_join_is_max);
    RUN_TEST(outcome_join_null_handling);

    TEST_REPORT();
    return test_failures;
}
