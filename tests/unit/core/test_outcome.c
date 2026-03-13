/*
 * test_outcome.c — unit tests for outcome severity lattice
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/core/outcome.h>

TEST(outcome_severity_of_null) { ASSERT_EQ(asx_outcome_severity_of(NULL), ASX_OUTCOME_OK); }

TEST(outcome_severity_of_ok) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_OK);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_OK);
}

TEST(outcome_severity_of_err) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_ERR);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_ERR);
}

TEST(outcome_severity_of_cancelled) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_CANCELLED);
}

TEST(outcome_severity_of_panicked) {
    asx_outcome o = asx_outcome_make(ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&o), ASX_OUTCOME_PANICKED);
}

TEST(outcome_lattice_order) {
    ASSERT_TRUE(ASX_OUTCOME_OK < ASX_OUTCOME_ERR);
    ASSERT_TRUE(ASX_OUTCOME_ERR < ASX_OUTCOME_CANCELLED);
    ASSERT_TRUE(ASX_OUTCOME_CANCELLED < ASX_OUTCOME_PANICKED);
}

TEST(outcome_join_max_severity) {
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome result = asx_outcome_join(&ok, &err);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
}

TEST(outcome_join_left_bias) {
    /* Equal severity: left operand wins */
    asx_outcome a = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome b = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome result = asx_outcome_join(&a, &b);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
}

TEST(outcome_join_null_identity) {
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome result = asx_outcome_join(NULL, &err);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
    result = asx_outcome_join(&err, NULL);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_ERR);
}

TEST(outcome_join_both_null) {
    asx_outcome result = asx_outcome_join(NULL, NULL);
    ASSERT_EQ(asx_outcome_severity_of(&result), ASX_OUTCOME_OK);
}

TEST(outcome_join_associative) {
    /* (a ⊔ b) ⊔ c == a ⊔ (b ⊔ c) */
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome can = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome ab = asx_outcome_join(&ok, &err);
    asx_outcome lhs = asx_outcome_join(&ab, &can);
    asx_outcome bc = asx_outcome_join(&err, &can);
    asx_outcome rhs = asx_outcome_join(&ok, &bc);
    ASSERT_EQ(asx_outcome_severity_of(&lhs), asx_outcome_severity_of(&rhs));
}

TEST(outcome_join_commutative_severity) {
    /* join(a,b) and join(b,a) yield same severity */
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome pan = asx_outcome_make(ASX_OUTCOME_PANICKED);
    asx_outcome ab = asx_outcome_join(&err, &pan);
    asx_outcome ba = asx_outcome_join(&pan, &err);
    ASSERT_EQ(asx_outcome_severity_of(&ab), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&ba), ASX_OUTCOME_PANICKED);
}

TEST(outcome_join_panicked_dominates) {
    /* Panicked wins against every other severity */
    asx_outcome pan = asx_outcome_make(ASX_OUTCOME_PANICKED);
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome err = asx_outcome_make(ASX_OUTCOME_ERR);
    asx_outcome can = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome r1, r2, r3, r4;
    r1 = asx_outcome_join(&pan, &ok);
    r2 = asx_outcome_join(&pan, &err);
    r3 = asx_outcome_join(&pan, &can);
    r4 = asx_outcome_join(&ok, &pan);
    ASSERT_EQ(asx_outcome_severity_of(&r1), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&r2), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&r3), ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_outcome_severity_of(&r4), ASX_OUTCOME_PANICKED);
}

TEST(outcome_join_idempotent) {
    /* join(x, x) == x for all severity levels */
    asx_outcome_severity levels[] = {ASX_OUTCOME_OK, ASX_OUTCOME_ERR, ASX_OUTCOME_CANCELLED,
                                     ASX_OUTCOME_PANICKED};
    int i;
    for (i = 0; i < 4; i++) {
        asx_outcome x = asx_outcome_make(levels[i]);
        asx_outcome r = asx_outcome_join(&x, &x);
        ASSERT_EQ(asx_outcome_severity_of(&r), levels[i]);
    }
}

int main(void) {
    fprintf(stderr, "=== test_outcome ===\n");
    RUN_TEST(outcome_severity_of_null);
    RUN_TEST(outcome_severity_of_ok);
    RUN_TEST(outcome_severity_of_err);
    RUN_TEST(outcome_severity_of_cancelled);
    RUN_TEST(outcome_severity_of_panicked);
    RUN_TEST(outcome_lattice_order);
    RUN_TEST(outcome_join_max_severity);
    RUN_TEST(outcome_join_left_bias);
    RUN_TEST(outcome_join_null_identity);
    RUN_TEST(outcome_join_both_null);
    RUN_TEST(outcome_join_associative);
    RUN_TEST(outcome_join_commutative_severity);
    RUN_TEST(outcome_join_panicked_dominates);
    RUN_TEST(outcome_join_idempotent);
    TEST_REPORT();
    return test_failures;
}
