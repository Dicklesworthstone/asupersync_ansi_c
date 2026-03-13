/*
 * test_budget.c — unit tests for budget algebra
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include "../../test_log.h"
#include <asx/core/budget.h>

TEST(budget_infinite_is_identity) {
    asx_budget a = {100, 50, 1000, 128};
    asx_budget inf = asx_budget_infinite();
    asx_budget result = asx_budget_meet(&a, &inf);
    ASSERT_EQ(result.deadline, a.deadline);
    ASSERT_EQ(result.poll_quota, a.poll_quota);
    ASSERT_EQ(result.cost_quota, a.cost_quota);
    ASSERT_EQ(result.priority, a.priority);
}

TEST(budget_zero_is_absorbing) {
    asx_budget a = {100, 50, 1000, 128};
    asx_budget zero = asx_budget_zero();
    asx_budget result = asx_budget_meet(&a, &zero);
    ASSERT_EQ(result.poll_quota, (uint32_t)0);
    ASSERT_EQ(result.cost_quota, (uint64_t)0);
    ASSERT_EQ(result.priority, (uint8_t)0);
}

TEST(budget_meet_commutative) {
    asx_budget a = {100, 50, 1000, 128};
    asx_budget b = {200, 30, 2000, 64};
    asx_budget ab = asx_budget_meet(&a, &b);
    asx_budget ba = asx_budget_meet(&b, &a);
    ASSERT_EQ(ab.deadline, ba.deadline);
    ASSERT_EQ(ab.poll_quota, ba.poll_quota);
    ASSERT_EQ(ab.cost_quota, ba.cost_quota);
    ASSERT_EQ(ab.priority, ba.priority);
}

TEST(budget_meet_tightens) {
    asx_budget a = {100, 50, 1000, 128};
    asx_budget b = {200, 30, 2000, 64};
    asx_budget result = asx_budget_meet(&a, &b);
    ASSERT_EQ(result.deadline, (asx_time)100);
    ASSERT_EQ(result.poll_quota, (uint32_t)30);
    ASSERT_EQ(result.cost_quota, (uint64_t)1000);
    ASSERT_EQ(result.priority, (uint8_t)64);
}

TEST(budget_consume_poll) {
    asx_budget b = {0, 2, UINT64_MAX, 128};
    ASSERT_EQ(asx_budget_consume_poll(&b), (uint32_t)2);
    ASSERT_EQ(b.poll_quota, (uint32_t)1);
    ASSERT_EQ(asx_budget_consume_poll(&b), (uint32_t)1);
    ASSERT_EQ(b.poll_quota, (uint32_t)0);
    ASSERT_EQ(asx_budget_consume_poll(&b), (uint32_t)0); /* exhausted */
    ASSERT_EQ(b.poll_quota, (uint32_t)0);                /* stays at 0 */
}

TEST(budget_consume_cost) {
    asx_budget b = {0, 100, 50, 128};
    ASSERT_TRUE(asx_budget_consume_cost(&b, 30));
    ASSERT_EQ(b.cost_quota, (uint64_t)20);
    ASSERT_FALSE(asx_budget_consume_cost(&b, 21)); /* insufficient */
    ASSERT_EQ(b.cost_quota, (uint64_t)20);         /* no mutation on failure */
    ASSERT_TRUE(asx_budget_consume_cost(&b, 20));  /* exact */
    ASSERT_EQ(b.cost_quota, (uint64_t)0);
}

TEST(budget_exhaustion) {
    asx_budget b = {0, 0, UINT64_MAX, 128};
    ASSERT_TRUE(asx_budget_is_exhausted(&b));
    b.poll_quota = 1;
    ASSERT_FALSE(asx_budget_is_exhausted(&b));
    b.cost_quota = 0;
    ASSERT_TRUE(asx_budget_is_exhausted(&b));
}

TEST(budget_deadline) {
    asx_budget b = {100, 50, 1000, 128};
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 99));
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, 100)); /* now == deadline */
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, 101));
    b.deadline = 0; /* unconstrained */
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 999));
}

TEST(budget_meet_associative) {
    /* (a ∩ b) ∩ c == a ∩ (b ∩ c) */
    asx_budget a = {100, 50, 1000, 128};
    asx_budget b = {200, 30, 2000, 64};
    asx_budget c = {150, 40, 500, 100};
    asx_budget ab = asx_budget_meet(&a, &b);
    asx_budget lhs = asx_budget_meet(&ab, &c);
    asx_budget bc = asx_budget_meet(&b, &c);
    asx_budget rhs = asx_budget_meet(&a, &bc);
    ASSERT_EQ(lhs.deadline, rhs.deadline);
    ASSERT_EQ(lhs.poll_quota, rhs.poll_quota);
    ASSERT_EQ(lhs.cost_quota, rhs.cost_quota);
    ASSERT_EQ(lhs.priority, rhs.priority);
}

TEST(budget_meet_idempotent) {
    /* a ∩ a == a */
    asx_budget a = {100, 50, 1000, 128};
    asx_budget result = asx_budget_meet(&a, &a);
    ASSERT_EQ(result.deadline, a.deadline);
    ASSERT_EQ(result.poll_quota, a.poll_quota);
    ASSERT_EQ(result.cost_quota, a.cost_quota);
    ASSERT_EQ(result.priority, a.priority);
}

TEST(budget_infinite_fields) {
    asx_budget inf = asx_budget_infinite();
    ASSERT_EQ(inf.deadline, (asx_time)0);
    ASSERT_EQ(inf.poll_quota, UINT32_MAX);
    ASSERT_EQ(inf.cost_quota, UINT64_MAX);
    ASSERT_EQ(inf.priority, (uint8_t)255);
    ASSERT_FALSE(asx_budget_is_exhausted(&inf));
}

TEST(budget_zero_fields) {
    asx_budget zero = asx_budget_zero();
    ASSERT_EQ(zero.poll_quota, (uint32_t)0);
    ASSERT_EQ(zero.cost_quota, (uint64_t)0);
    ASSERT_EQ(zero.priority, (uint8_t)0);
    ASSERT_TRUE(asx_budget_is_exhausted(&zero));
}

TEST(budget_consume_cost_zero) {
    /* Consuming 0 cost should always succeed */
    asx_budget b = {0, 100, 50, 128};
    ASSERT_TRUE(asx_budget_consume_cost(&b, 0));
    ASSERT_EQ(b.cost_quota, (uint64_t)50);
}

TEST(budget_consume_cost_exact_boundary) {
    /* Consume exactly what remains */
    asx_budget b = {0, 100, 1, 128};
    ASSERT_TRUE(asx_budget_consume_cost(&b, 1));
    ASSERT_EQ(b.cost_quota, (uint64_t)0);
    ASSERT_TRUE(asx_budget_is_exhausted(&b));
}

TEST(budget_deadline_max) {
    /* UINT64_MAX deadline */
    asx_budget b = {UINT64_MAX, 50, 1000, 128};
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 0));
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, UINT64_MAX));
}

int main(void) {
    test_log_open("unit", "core/budget", "test_budget");
    fprintf(stderr, "=== test_budget ===\n");
    RUN_TEST(budget_infinite_is_identity);
    RUN_TEST(budget_zero_is_absorbing);
    RUN_TEST(budget_meet_commutative);
    RUN_TEST(budget_meet_tightens);
    RUN_TEST(budget_consume_poll);
    RUN_TEST(budget_consume_cost);
    RUN_TEST(budget_exhaustion);
    RUN_TEST(budget_deadline);
    RUN_TEST(budget_meet_associative);
    RUN_TEST(budget_meet_idempotent);
    RUN_TEST(budget_infinite_fields);
    RUN_TEST(budget_zero_fields);
    RUN_TEST(budget_consume_cost_zero);
    RUN_TEST(budget_consume_cost_exact_boundary);
    RUN_TEST(budget_deadline_max);
    TEST_REPORT();
    test_log_close();
    return test_failures;
}
