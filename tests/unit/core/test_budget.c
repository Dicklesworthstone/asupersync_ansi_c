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
    ASSERT_EQ(result.priority, (uint8_t)255); /* ZERO priority absorbs under max */
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
    /* K1: priority meet is max (Rust types/budget.rs combine_untraced:
     * `priority: self.priority.max(other.priority)`); it was min. */
    ASSERT_EQ(result.priority, (uint8_t)128);
}

TEST(budget_new_and_from_polls_default_priority) {
    asx_budget n = asx_budget_new();
    asx_budget p = asx_budget_from_polls(7);
    ASSERT_EQ(n.priority, (uint8_t)ASX_BUDGET_DEFAULT_PRIORITY);
    ASSERT_EQ(n.priority, (uint8_t)128);
    ASSERT_EQ(n.poll_quota, UINT32_MAX);
    ASSERT_EQ(n.cost_quota, UINT64_MAX);
    ASSERT_EQ(n.deadline, ASX_TIME_INFINITE); /* Rust Budget::new: deadline None */
    ASSERT_EQ(p.priority, (uint8_t)128);
    ASSERT_EQ(p.poll_quota, (uint32_t)7);
}

TEST(budget_priority_inherits_most_urgent) {
    /* A child can only become more urgent than its parent, never less. */
    asx_budget parent = asx_budget_new();
    asx_budget child = asx_budget_new();
    asx_budget eff;
    parent.priority = 200;
    child.priority = 50;
    eff = asx_budget_meet(&parent, &child);
    ASSERT_EQ(eff.priority, (uint8_t)200);
    child.priority = 250;
    eff = asx_budget_meet(&parent, &child);
    ASSERT_EQ(eff.priority, (uint8_t)250);
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
    b.deadline = ASX_TIME_INFINITE; /* unconstrained (Rust None) */
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 999));
    /* A deadline of 0 is a deadline (Rust Some(Time::ZERO)): due at once. */
    b.deadline = 0;
    ASSERT_TRUE(asx_budget_is_past_deadline(&b, 0));
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
    ASSERT_EQ(inf.deadline, ASX_TIME_INFINITE); /* Rust Budget::INFINITE: None */
    ASSERT_EQ(inf.poll_quota, UINT32_MAX);
    ASSERT_EQ(inf.cost_quota, UINT64_MAX);
    ASSERT_EQ(inf.priority, (uint8_t)0); /* Rust Budget::INFINITE */
    ASSERT_FALSE(asx_budget_is_exhausted(&inf));
}

TEST(budget_zero_fields) {
    asx_budget zero = asx_budget_zero();
    ASSERT_EQ(zero.poll_quota, (uint32_t)0);
    ASSERT_EQ(zero.cost_quota, (uint64_t)0);
    ASSERT_EQ(zero.priority, (uint8_t)255); /* Rust Budget::ZERO */
    ASSERT_EQ(zero.deadline, (asx_time)0);  /* Some(Time::ZERO): due at once */
    ASSERT_TRUE(asx_budget_is_past_deadline(&zero, 0));
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
    /* UINT64_MAX (ASX_TIME_INFINITE) is no deadline: never past, and the
     * meet keeps the other side's deadline (Rust None vs Some). */
    asx_budget b = {UINT64_MAX, 50, 1000, 128};
    asx_budget c = {0, 50, 1000, 128};
    asx_budget m;
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, 0));
    ASSERT_FALSE(asx_budget_is_past_deadline(&b, UINT64_MAX));
    m = asx_budget_meet(&b, &c);
    ASSERT_EQ(m.deadline, (asx_time)0);
    ASSERT_TRUE(asx_budget_is_past_deadline(&m, 0));
}

/* -------------------------------------------------------------------
 * CapabilityBudget (bd-9kll.3.27): meet, validate, plan_child. The first
 * five mirror Rust's tests in types/budget.rs:1738-1820 (pinned rev
 * 5e60b1c); the rest pin what Rust's planner leaves implicit.
 * ------------------------------------------------------------------- */

/* All five dimensions present: 1024 bytes of memory, 100 CPU units, 10000
 * I/O bytes, a 50-poll cleanup budget, 4096 artifact bytes. */
static asx_capability_budget cap_full(void) {
    asx_budget cleanup = asx_budget_from_polls(50);
    asx_capability_budget b = asx_capability_budget_unspecified();
    b = asx_capability_budget_with_memory_bytes(b, 1024u);
    b = asx_capability_budget_with_cpu_units(b, 100u);
    b = asx_capability_budget_with_io_bytes(b, 10000u);
    b = asx_capability_budget_with_cleanup_budget(b, &cleanup);
    b = asx_capability_budget_with_artifact_bytes(b, 4096u);
    return b;
}

/* `b` with dimension `d` set to its smallest spent value (0 / 0 polls). */
static asx_capability_budget cap_spent(asx_capability_budget b, asx_capability_dimension d) {
    asx_budget none = asx_budget_from_polls(0);
    switch (d) {
    case ASX_CAPABILITY_MEMORY_BYTES: return asx_capability_budget_with_memory_bytes(b, 0u);
    case ASX_CAPABILITY_CPU_UNITS: return asx_capability_budget_with_cpu_units(b, 0u);
    case ASX_CAPABILITY_IO_BYTES: return asx_capability_budget_with_io_bytes(b, 0u);
    case ASX_CAPABILITY_CLEANUP: return asx_capability_budget_with_cleanup_budget(b, &none);
    case ASX_CAPABILITY_ARTIFACT_BYTES: return asx_capability_budget_with_artifact_bytes(b, 0u);
    case ASX_CAPABILITY_DIMENSION_COUNT: break;
    }
    return b;
}

/* Requirements naming only dimension `d`. */
static asx_capability_requirements cap_require(asx_capability_dimension d) {
    asx_capability_requirements r = {0, 0, 0, 0, 0};
    switch (d) {
    case ASX_CAPABILITY_MEMORY_BYTES: r.memory_bytes = 1u; break;
    case ASX_CAPABILITY_CPU_UNITS: r.cpu_units = 1u; break;
    case ASX_CAPABILITY_IO_BYTES: r.io_bytes = 1u; break;
    case ASX_CAPABILITY_CLEANUP: r.cleanup = 1u; break;
    case ASX_CAPABILITY_ARTIFACT_BYTES: r.artifact_bytes = 1u; break;
    case ASX_CAPABILITY_DIMENSION_COUNT: break;
    }
    return r;
}

static int cap_equal(const asx_capability_budget *a, const asx_capability_budget *b) {
    if (a->has_memory_bytes != b->has_memory_bytes || a->memory_bytes != b->memory_bytes) return 0;
    if (a->has_cpu_units != b->has_cpu_units || a->cpu_units != b->cpu_units) return 0;
    if (a->has_io_bytes != b->has_io_bytes || a->io_bytes != b->io_bytes) return 0;
    if (a->has_artifact_bytes != b->has_artifact_bytes || a->artifact_bytes != b->artifact_bytes) {
        return 0;
    }
    if (a->has_cleanup_budget != b->has_cleanup_budget) return 0;
    if (a->has_cleanup_budget) {
        if (a->cleanup_budget.deadline != b->cleanup_budget.deadline) return 0;
        if (a->cleanup_budget.poll_quota != b->cleanup_budget.poll_quota) return 0;
        if (a->cleanup_budget.cost_quota != b->cleanup_budget.cost_quota) return 0;
        if (a->cleanup_budget.priority != b->cleanup_budget.priority) return 0;
    }
    return 1;
}

TEST(capability_child_inherits_parent_and_tightens_overrides) {
    /* Rust capability_budget_child_inherits_parent_and_tightens_overrides */
    asx_budget parent_cleanup = asx_budget_from_polls(50);
    asx_budget child_cleanup = asx_budget_from_polls(10);
    asx_capability_budget parent = asx_capability_budget_unspecified();
    asx_capability_budget child = asx_capability_budget_unspecified();
    asx_capability_budget effective;

    parent = asx_capability_budget_with_memory_bytes(parent, 1024u);
    parent = asx_capability_budget_with_cpu_units(parent, 100u);
    parent = asx_capability_budget_with_io_bytes(parent, 10000u);
    parent = asx_capability_budget_with_cleanup_budget(parent, &parent_cleanup);
    parent = asx_capability_budget_with_artifact_bytes(parent, 4096u);
    child = asx_capability_budget_with_memory_bytes(child, 2048u);
    child = asx_capability_budget_with_cpu_units(child, 25u);
    child = asx_capability_budget_with_io_bytes(child, 1000u);
    child = asx_capability_budget_with_cleanup_budget(child, &child_cleanup);

    effective = asx_capability_budget_meet(&parent, &child);
    ASSERT_TRUE(effective.has_memory_bytes);
    ASSERT_EQ(effective.memory_bytes, (uint64_t)1024); /* the child asked for more: parent's */
    ASSERT_TRUE(effective.has_cpu_units);
    ASSERT_EQ(effective.cpu_units, (uint64_t)25);
    ASSERT_TRUE(effective.has_io_bytes);
    ASSERT_EQ(effective.io_bytes, (uint64_t)1000);
    ASSERT_TRUE(effective.has_cleanup_budget);
    ASSERT_EQ(effective.cleanup_budget.poll_quota, (uint32_t)10);
    ASSERT_TRUE(effective.has_artifact_bytes);
    ASSERT_EQ(effective.artifact_bytes, (uint64_t)4096); /* the child has none: inherited */
}

TEST(capability_optional_absent_dimension_admits) {
    /* Rust capability_budget_optional_absent_dimension_admits */
    asx_capability_budget none = asx_capability_budget_unspecified();
    asx_capability_requirements no_req = {0, 0, 0, 0, 0};
    asx_capability_budget out = cap_full();
    asx_capability_refusal refusal;
    asx_capability_budget unspecified = asx_capability_budget_unspecified();

    ASSERT_EQ(asx_capability_budget_plan_child(&none, &none, &no_req, &out, &refusal), ASX_OK);
    ASSERT_TRUE(cap_equal(&out, &unspecified));
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_REFUSAL_NONE);
}

TEST(capability_required_absent_dimension_rejects) {
    /* Rust capability_budget_required_absent_dimension_rejects, every
     * dimension: a required dimension neither side supplies is refused as
     * missing, and names itself. */
    int d;
    for (d = 0; d < (int)ASX_CAPABILITY_DIMENSION_COUNT; d++) {
        asx_capability_budget none = asx_capability_budget_unspecified();
        asx_capability_requirements req = cap_require((asx_capability_dimension)d);
        asx_capability_budget out;
        asx_capability_refusal refusal;
        out = cap_full();
        ASSERT_EQ(asx_capability_budget_plan_child(&none, &none, &req, &out, &refusal),
                  ASX_E_CAPABILITY_BUDGET_REFUSED);
        ASSERT_EQ(refusal.kind, ASX_CAPABILITY_MISSING_REQUIRED);
        ASSERT_EQ(refusal.dimension, (asx_capability_dimension)d);
    }
}

TEST(capability_required_exhausted_dimension_rejects) {
    /* Rust capability_budget_required_exhausted_{dimension,cleanup}_rejects,
     * every dimension: present but spent (0, or a cleanup budget with no
     * polls) is refused as exhausted. */
    int d;
    for (d = 0; d < (int)ASX_CAPABILITY_DIMENSION_COUNT; d++) {
        asx_capability_budget parent = cap_spent(cap_full(), (asx_capability_dimension)d);
        asx_capability_budget none = asx_capability_budget_unspecified();
        asx_capability_requirements req = cap_require((asx_capability_dimension)d);
        asx_capability_budget out;
        asx_capability_refusal refusal;
        ASSERT_EQ(asx_capability_budget_plan_child(&parent, &none, &req, &out, &refusal),
                  ASX_E_CAPABILITY_BUDGET_REFUSED);
        ASSERT_EQ(refusal.kind, ASX_CAPABILITY_EXHAUSTED);
        ASSERT_EQ(refusal.dimension, (asx_capability_dimension)d);
    }
}

TEST(capability_unspent_dimension_with_the_same_requirement_admits) {
    /* The permitted near-identical case of the one above: one unit / one
     * poll left is not exhausted. */
    int d;
    for (d = 0; d < (int)ASX_CAPABILITY_DIMENSION_COUNT; d++) {
        asx_budget one_poll = asx_budget_from_polls(1);
        asx_capability_budget parent = cap_full();
        asx_capability_budget none = asx_capability_budget_unspecified();
        asx_capability_requirements req = cap_require((asx_capability_dimension)d);
        asx_capability_budget out;
        asx_capability_refusal refusal;
        parent = asx_capability_budget_with_memory_bytes(parent, 1u);
        parent = asx_capability_budget_with_cpu_units(parent, 1u);
        parent = asx_capability_budget_with_io_bytes(parent, 1u);
        parent = asx_capability_budget_with_cleanup_budget(parent, &one_poll);
        parent = asx_capability_budget_with_artifact_bytes(parent, 1u);
        ASSERT_EQ(asx_capability_budget_plan_child(&parent, &none, &req, &out, &refusal), ASX_OK);
        ASSERT_TRUE(cap_equal(&out, &parent));
        ASSERT_EQ(refusal.kind, ASX_CAPABILITY_REFUSAL_NONE);
    }
}

TEST(capability_cleanup_spent_by_cost_quota_is_exhausted) {
    /* Budget::is_exhausted is poll quota 0 or cost quota 0 */
    asx_budget cleanup = asx_budget_new();
    asx_capability_budget parent = asx_capability_budget_unspecified();
    asx_capability_budget none = asx_capability_budget_unspecified();
    asx_capability_requirements req = cap_require(ASX_CAPABILITY_CLEANUP);
    asx_capability_budget out;
    asx_capability_refusal refusal;
    cleanup.cost_quota = 0u;
    parent = asx_capability_budget_with_cleanup_budget(parent, &cleanup);
    ASSERT_EQ(asx_capability_budget_plan_child(&parent, &none, &req, &out, &refusal),
              ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_EXHAUSTED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_CLEANUP);
}

TEST(capability_meet_keeps_one_sided_dimensions) {
    asx_capability_budget parent = asx_capability_budget_unspecified();
    asx_capability_budget child = asx_capability_budget_unspecified();
    asx_capability_budget m;
    parent = asx_capability_budget_with_memory_bytes(parent, 7u);
    child = asx_capability_budget_with_cpu_units(child, 9u);
    m = asx_capability_budget_meet(&parent, &child);
    ASSERT_TRUE(m.has_memory_bytes);
    ASSERT_EQ(m.memory_bytes, (uint64_t)7);
    ASSERT_TRUE(m.has_cpu_units);
    ASSERT_EQ(m.cpu_units, (uint64_t)9);
    ASSERT_FALSE(m.has_io_bytes);
    ASSERT_FALSE(m.has_cleanup_budget);
    ASSERT_FALSE(m.has_artifact_bytes);
    ASSERT_EQ(m.io_bytes, (uint64_t)0);
}

TEST(capability_meet_never_loosens) {
    /* meet(p, c) is no greater than either side in any dimension, a zero is
     * absorbing either way (a child cannot revive a spent parent), and the
     * result is the same whichever side is "parent". */
    asx_capability_budget p = cap_full();
    asx_capability_budget c = asx_capability_budget_unspecified();
    asx_capability_budget m;
    asx_capability_budget swapped;

    c = asx_capability_budget_with_memory_bytes(c, 0u);
    c = asx_capability_budget_with_io_bytes(c, 20000u);
    m = asx_capability_budget_meet(&p, &c);
    ASSERT_EQ(m.memory_bytes, (uint64_t)0);
    ASSERT_EQ(m.io_bytes, (uint64_t)10000);
    swapped = asx_capability_budget_meet(&c, &p);
    ASSERT_TRUE(cap_equal(&m, &swapped));

    p = cap_spent(cap_full(), ASX_CAPABILITY_CPU_UNITS);
    c = asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 500u);
    m = asx_capability_budget_meet(&p, &c);
    ASSERT_EQ(m.cpu_units, (uint64_t)0);
}

TEST(capability_meet_is_idempotent_and_associative) {
    asx_capability_budget a = cap_full();
    asx_capability_budget b = asx_capability_budget_with_memory_bytes(
        asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 40u), 512u);
    asx_capability_budget c = asx_capability_budget_with_io_bytes(
        asx_capability_budget_with_memory_bytes(asx_capability_budget_unspecified(), 2048u), 5u);
    asx_capability_budget aa = asx_capability_budget_meet(&a, &a);
    asx_capability_budget ab = asx_capability_budget_meet(&a, &b);
    asx_capability_budget bc = asx_capability_budget_meet(&b, &c);
    asx_capability_budget lhs = asx_capability_budget_meet(&ab, &c);
    asx_capability_budget rhs = asx_capability_budget_meet(&a, &bc);
    ASSERT_TRUE(cap_equal(&aa, &a));
    ASSERT_TRUE(cap_equal(&lhs, &rhs));
}

TEST(capability_cleanup_budgets_meet_as_budgets) {
    asx_budget pc = {100, 50, 1000, 128};
    asx_budget cc = {200, 30, 2000, 64};
    asx_capability_budget parent =
        asx_capability_budget_with_cleanup_budget(asx_capability_budget_unspecified(), &pc);
    asx_capability_budget child =
        asx_capability_budget_with_cleanup_budget(asx_capability_budget_unspecified(), &cc);
    asx_capability_budget m = asx_capability_budget_meet(&parent, &child);
    asx_budget expected = asx_budget_meet(&pc, &cc);
    ASSERT_TRUE(m.has_cleanup_budget);
    ASSERT_EQ(m.cleanup_budget.deadline, expected.deadline);
    ASSERT_EQ(m.cleanup_budget.poll_quota, expected.poll_quota);
    ASSERT_EQ(m.cleanup_budget.cost_quota, expected.cost_quota);
    ASSERT_EQ(m.cleanup_budget.priority, expected.priority);
}

TEST(capability_null_operands_read_as_unspecified) {
    asx_capability_budget full = cap_full();
    asx_capability_budget unspecified = asx_capability_budget_unspecified();
    asx_capability_budget m = asx_capability_budget_meet(NULL, NULL);
    asx_capability_budget out;
    ASSERT_TRUE(cap_equal(&m, &unspecified));
    m = asx_capability_budget_meet(&full, NULL);
    ASSERT_TRUE(cap_equal(&m, &full));
    m = asx_capability_budget_meet(NULL, &full);
    ASSERT_TRUE(cap_equal(&m, &full));
    ASSERT_EQ(asx_capability_budget_validate(NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(asx_capability_budget_plan_child(NULL, NULL, NULL, NULL, NULL), ASX_OK);
    ASSERT_EQ(asx_capability_budget_plan_child(&full, NULL, NULL, &out, NULL), ASX_OK);
    ASSERT_TRUE(cap_equal(&out, &full));
}

TEST(capability_validate_reports_the_first_dimension_in_rusts_order) {
    /* validate checks memory, cpu, io, cleanup, artifact (budget.rs:1076-1103):
     * with every dimension required and none present, then each dimension
     * supplied in turn, the refused one advances through that order. */
    asx_capability_requirements all = {1, 1, 1, 1, 1};
    asx_capability_budget b = asx_capability_budget_unspecified();
    asx_budget cleanup = asx_budget_from_polls(5);
    asx_capability_refusal refusal;

    ASSERT_EQ(asx_capability_budget_validate(&b, &all, &refusal), ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_MEMORY_BYTES);
    b = asx_capability_budget_with_memory_bytes(b, 1u);
    ASSERT_EQ(asx_capability_budget_validate(&b, &all, &refusal), ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_CPU_UNITS);
    b = asx_capability_budget_with_cpu_units(b, 1u);
    ASSERT_EQ(asx_capability_budget_validate(&b, &all, &refusal), ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_IO_BYTES);
    b = asx_capability_budget_with_io_bytes(b, 1u);
    ASSERT_EQ(asx_capability_budget_validate(&b, &all, &refusal), ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_CLEANUP);
    b = asx_capability_budget_with_cleanup_budget(b, &cleanup);
    ASSERT_EQ(asx_capability_budget_validate(&b, &all, &refusal), ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.dimension, ASX_CAPABILITY_ARTIFACT_BYTES);
    b = asx_capability_budget_with_artifact_bytes(b, 1u);
    ASSERT_EQ(asx_capability_budget_validate(&b, &all, &refusal), ASX_OK);
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_REFUSAL_NONE);
}

TEST(capability_refused_plan_does_not_write_the_output) {
    asx_capability_budget none = asx_capability_budget_unspecified();
    asx_capability_requirements req = cap_require(ASX_CAPABILITY_IO_BYTES);
    asx_capability_budget out = cap_full();
    asx_capability_budget before = out;
    ASSERT_EQ(asx_capability_budget_plan_child(&none, &none, &req, &out, NULL),
              ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_TRUE(cap_equal(&out, &before));
}

TEST(capability_requirement_is_checked_against_the_meet) {
    /* A requirement is admitted against the effective envelope: a child
     * that supplies the required dimension satisfies it, one that supplies
     * zero for it exhausts it, and a spent parent stays spent. */
    asx_capability_budget parent = asx_capability_budget_unspecified();
    asx_capability_budget child = asx_capability_budget_unspecified();
    asx_capability_requirements req = cap_require(ASX_CAPABILITY_MEMORY_BYTES);
    asx_capability_budget out;
    asx_capability_refusal refusal;

    child = asx_capability_budget_with_memory_bytes(child, 64u);
    ASSERT_EQ(asx_capability_budget_plan_child(&parent, &child, &req, &out, &refusal), ASX_OK);
    ASSERT_EQ(out.memory_bytes, (uint64_t)64);

    parent = asx_capability_budget_with_memory_bytes(parent, 128u);
    child = asx_capability_budget_with_memory_bytes(child, 0u);
    ASSERT_EQ(asx_capability_budget_plan_child(&parent, &child, &req, &out, &refusal),
              ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_EXHAUSTED);

    parent = asx_capability_budget_with_memory_bytes(parent, 0u);
    child = asx_capability_budget_with_memory_bytes(child, 64u);
    ASSERT_EQ(asx_capability_budget_plan_child(&parent, &child, &req, &out, &refusal),
              ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(refusal.kind, ASX_CAPABILITY_EXHAUSTED);
}

TEST(capability_builders_set_only_their_dimension) {
    asx_capability_budget b =
        asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 3u);
    asx_budget zero_cleanup = asx_budget_zero();
    ASSERT_TRUE(b.has_cpu_units);
    ASSERT_EQ(b.cpu_units, (uint64_t)3);
    ASSERT_FALSE(b.has_memory_bytes);
    ASSERT_FALSE(b.has_io_bytes);
    ASSERT_FALSE(b.has_cleanup_budget);
    ASSERT_FALSE(b.has_artifact_bytes);
    /* Some(0) is an envelope, not an absent one. */
    b = asx_capability_budget_with_memory_bytes(b, 0u);
    ASSERT_TRUE(b.has_memory_bytes);
    b = asx_capability_budget_with_cleanup_budget(b, &zero_cleanup);
    ASSERT_TRUE(b.has_cleanup_budget);
    /* A NULL cleanup budget sets nothing. */
    b = asx_capability_budget_with_cleanup_budget(asx_capability_budget_unspecified(), NULL);
    ASSERT_FALSE(b.has_cleanup_budget);
}

TEST(capability_dimension_names_are_rusts) {
    ASSERT_STR_EQ(asx_capability_dimension_str(ASX_CAPABILITY_MEMORY_BYTES), "memory_bytes");
    ASSERT_STR_EQ(asx_capability_dimension_str(ASX_CAPABILITY_CPU_UNITS), "cpu_units");
    ASSERT_STR_EQ(asx_capability_dimension_str(ASX_CAPABILITY_IO_BYTES), "io_bytes");
    ASSERT_STR_EQ(asx_capability_dimension_str(ASX_CAPABILITY_CLEANUP), "cleanup");
    ASSERT_STR_EQ(asx_capability_dimension_str(ASX_CAPABILITY_ARTIFACT_BYTES), "artifact_bytes");
    ASSERT_STR_EQ(asx_capability_dimension_str(ASX_CAPABILITY_DIMENSION_COUNT), "unknown");
}

int main(void) {
    test_log_open("unit", "core/budget", "test_budget");
    fprintf(stderr, "=== test_budget ===\n");
    RUN_TEST(budget_infinite_is_identity);
    RUN_TEST(budget_zero_is_absorbing);
    RUN_TEST(budget_meet_commutative);
    RUN_TEST(budget_meet_tightens);
    RUN_TEST(budget_new_and_from_polls_default_priority);
    RUN_TEST(budget_priority_inherits_most_urgent);
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
    RUN_TEST(capability_child_inherits_parent_and_tightens_overrides);
    RUN_TEST(capability_optional_absent_dimension_admits);
    RUN_TEST(capability_required_absent_dimension_rejects);
    RUN_TEST(capability_required_exhausted_dimension_rejects);
    RUN_TEST(capability_unspent_dimension_with_the_same_requirement_admits);
    RUN_TEST(capability_cleanup_spent_by_cost_quota_is_exhausted);
    RUN_TEST(capability_meet_keeps_one_sided_dimensions);
    RUN_TEST(capability_meet_never_loosens);
    RUN_TEST(capability_meet_is_idempotent_and_associative);
    RUN_TEST(capability_cleanup_budgets_meet_as_budgets);
    RUN_TEST(capability_null_operands_read_as_unspecified);
    RUN_TEST(capability_validate_reports_the_first_dimension_in_rusts_order);
    RUN_TEST(capability_refused_plan_does_not_write_the_output);
    RUN_TEST(capability_requirement_is_checked_against_the_meet);
    RUN_TEST(capability_builders_set_only_their_dimension);
    RUN_TEST(capability_dimension_names_are_rusts);
    TEST_REPORT();
    test_log_close();
    return test_failures;
}
