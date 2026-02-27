/*
 * test_ghost.c — unit tests for ghost safety monitors
 *
 * Tests protocol monitor (lifecycle transition violations),
 * linearity monitor (obligation double-use and leak detection),
 * ring buffer mechanics, and query interface.
 *
 * Requires ASX_DEBUG_GHOST to be defined (default in debug builds).
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/runtime.h>

/* ---- Protocol monitor: region transitions ---- */

TEST(ghost_protocol_region_valid_transition) {
    asx_status st;
    asx_runtime_reset();

    /* Valid transition: Open -> Closing */
    st = asx_ghost_check_region_transition(ASX_INVALID_ID,
                                           ASX_REGION_OPEN,
                                           ASX_REGION_CLOSING);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)0);
}

TEST(ghost_protocol_region_invalid_transition) {
    asx_status st;
    asx_runtime_reset();

    /* Invalid: Open -> Closed (skips intermediate states) */
    st = asx_ghost_check_region_transition(ASX_INVALID_ID,
                                           ASX_REGION_OPEN,
                                           ASX_REGION_CLOSED);
    ASSERT_EQ(st, ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)1);

    /* Verify violation record */
    {
        asx_ghost_violation v;
        int ok = asx_ghost_violation_get(0, &v);
        ASSERT_TRUE(ok);
        ASSERT_EQ(v.kind, ASX_GHOST_PROTOCOL_REGION);
        ASSERT_EQ(v.from_state, (int)ASX_REGION_OPEN);
        ASSERT_EQ(v.to_state, (int)ASX_REGION_CLOSED);
    }
}

/* ---- Protocol monitor: task transitions ---- */

TEST(ghost_protocol_task_valid_transition) {
    asx_status st;
    asx_runtime_reset();

    /* Valid: Created -> Running */
    st = asx_ghost_check_task_transition(ASX_INVALID_ID,
                                         ASX_TASK_CREATED,
                                         ASX_TASK_RUNNING);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)0);
}

TEST(ghost_protocol_task_invalid_transition) {
    asx_status st;
    asx_runtime_reset();

    /* Invalid: Completed -> Running (terminal state, no outgoing edges) */
    st = asx_ghost_check_task_transition(ASX_INVALID_ID,
                                         ASX_TASK_COMPLETED,
                                         ASX_TASK_RUNNING);
    ASSERT_EQ(st, ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)1);

    {
        asx_ghost_violation v;
        int ok = asx_ghost_violation_get(0, &v);
        ASSERT_TRUE(ok);
        ASSERT_EQ(v.kind, ASX_GHOST_PROTOCOL_TASK);
    }
}

/* ---- Protocol monitor: obligation transitions ---- */

TEST(ghost_protocol_obligation_valid_transition) {
    asx_status st;
    asx_runtime_reset();

    /* Valid: Reserved -> Committed */
    st = asx_ghost_check_obligation_transition(ASX_INVALID_ID,
                                               ASX_OBLIGATION_RESERVED,
                                               ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)0);
}

TEST(ghost_protocol_obligation_invalid_transition) {
    asx_status st;
    asx_runtime_reset();

    /* Invalid: Committed -> Reserved (terminal state) */
    st = asx_ghost_check_obligation_transition(ASX_INVALID_ID,
                                               ASX_OBLIGATION_COMMITTED,
                                               ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(st, ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)1);

    {
        asx_ghost_violation v;
        int ok = asx_ghost_violation_get(0, &v);
        ASSERT_TRUE(ok);
        ASSERT_EQ(v.kind, ASX_GHOST_PROTOCOL_OBLIGATION);
    }
}

/* ---- Linearity monitor: double resolution ---- */

TEST(ghost_linearity_double_commit) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    /* First commit: valid */
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    /* Second commit: blocked by transition check, but ghost sees it */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);

    /* Ghost should have recorded the protocol violation for the
     * second commit attempt (Committed -> Committed is invalid). */
    ASSERT_TRUE(asx_ghost_violation_count() >= (uint32_t)1);
}

TEST(ghost_linearity_double_abort) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    /* First abort: valid */
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    /* Second abort: blocked, ghost records violation */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);

    ASSERT_TRUE(asx_ghost_violation_count() >= (uint32_t)1);
}

TEST(ghost_linearity_commit_after_abort) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    /* Commit after abort: blocked, ghost records */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
    ASSERT_TRUE(asx_ghost_violation_count() >= (uint32_t)1);
}

/* ---- Linearity monitor: leak detection ---- */

TEST(ghost_linearity_leaked_obligation) {
    asx_region_id rid;
    asx_obligation_id oid;
    uint32_t leaks;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    /* Don't resolve the obligation — it will be detected as a leak */
    leaks = asx_ghost_check_obligation_leaks(rid);
    ASSERT_EQ(leaks, (uint32_t)1);
    ASSERT_TRUE(asx_ghost_violation_count() >= (uint32_t)1);

    /* Verify the violation kind */
    {
        asx_ghost_violation v;
        int ok = asx_ghost_violation_get(0, &v);
        ASSERT_TRUE(ok);
        ASSERT_EQ(v.kind, ASX_GHOST_LINEARITY_LEAK);
        ASSERT_EQ(v.entity_id, oid);
    }
}

TEST(ghost_linearity_no_leak_after_commit) {
    asx_region_id rid;
    asx_obligation_id oid;
    uint32_t leaks;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    leaks = asx_ghost_check_obligation_leaks(rid);
    ASSERT_EQ(leaks, (uint32_t)0);
}

TEST(ghost_linearity_no_leak_after_abort) {
    asx_region_id rid;
    asx_obligation_id oid;
    uint32_t leaks;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    leaks = asx_ghost_check_obligation_leaks(rid);
    ASSERT_EQ(leaks, (uint32_t)0);
}

TEST(ghost_linearity_multiple_obligations_mixed) {
    asx_region_id rid;
    asx_obligation_id o1, o2, o3;
    uint32_t leaks;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o1), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o2), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o3), ASX_OK);

    /* Resolve o1 and o3, leave o2 as leaked */
    ASSERT_EQ(asx_obligation_commit(o1), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(o3), ASX_OK);

    leaks = asx_ghost_check_obligation_leaks(rid);
    ASSERT_EQ(leaks, (uint32_t)1);
}

/* ---- Ring buffer mechanics ---- */

TEST(ghost_ring_empty_initially) {
    asx_runtime_reset();
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)0);
    ASSERT_FALSE(asx_ghost_ring_overflowed());
}

TEST(ghost_ring_sequential_access) {
    asx_runtime_reset();

    /* Record two violations (both genuinely invalid transitions) */
    (void)asx_ghost_check_region_transition(ASX_INVALID_ID,
                                            ASX_REGION_OPEN,
                                            ASX_REGION_CLOSED);
    (void)asx_ghost_check_task_transition(ASX_INVALID_ID,
                                          ASX_TASK_COMPLETED,
                                          ASX_TASK_CREATED);

    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)2);

    {
        asx_ghost_violation v0, v1;
        ASSERT_TRUE(asx_ghost_violation_get(0, &v0));
        ASSERT_TRUE(asx_ghost_violation_get(1, &v1));
        ASSERT_EQ(v0.kind, ASX_GHOST_PROTOCOL_REGION);
        ASSERT_EQ(v1.kind, ASX_GHOST_PROTOCOL_TASK);
        ASSERT_EQ(v0.sequence, (uint32_t)0);
        ASSERT_EQ(v1.sequence, (uint32_t)1);
    }
}

TEST(ghost_ring_out_of_bounds_get) {
    asx_ghost_violation v;
    asx_runtime_reset();

    /* No violations recorded — index 0 should fail */
    ASSERT_FALSE(asx_ghost_violation_get(0, &v));

    /* NULL output should fail */
    ASSERT_FALSE(asx_ghost_violation_get(0, NULL));
}

TEST(ghost_ring_overflow_wraps) {
    uint32_t i;
    asx_runtime_reset();

    /* Fill ring beyond capacity */
    for (i = 0; i < ASX_GHOST_RING_CAPACITY + 10u; i++) {
        (void)asx_ghost_check_region_transition(
            (uint64_t)i,
            ASX_REGION_OPEN,
            ASX_REGION_CLOSED);
    }

    ASSERT_EQ(asx_ghost_violation_count(), ASX_GHOST_RING_CAPACITY + 10u);
    ASSERT_TRUE(asx_ghost_ring_overflowed());

    /* Oldest available entry should be the one that wrapped around */
    {
        asx_ghost_violation oldest;
        ASSERT_TRUE(asx_ghost_violation_get(0, &oldest));
        /* The oldest surviving entry has sequence = total - capacity */
        ASSERT_EQ(oldest.sequence, (uint32_t)(ASX_GHOST_RING_CAPACITY + 10u
                                               - ASX_GHOST_RING_CAPACITY));
    }
}

/* ---- Query interface ---- */

TEST(ghost_violation_kind_str_coverage) {
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_PROTOCOL_REGION),
                  "protocol_region");
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_PROTOCOL_TASK),
                  "protocol_task");
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_PROTOCOL_OBLIGATION),
                  "protocol_obligation");
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_LINEARITY_DOUBLE),
                  "linearity_double");
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_LINEARITY_LEAK),
                  "linearity_leak");
    /* Unknown kind */
    ASSERT_STR_EQ(asx_ghost_violation_kind_str((asx_ghost_violation_kind)99),
                  "unknown");
}

/* ---- Integration: ghost reset clears state ---- */

TEST(ghost_reset_clears_violations) {
    asx_runtime_reset();

    /* Generate a violation */
    (void)asx_ghost_check_region_transition(ASX_INVALID_ID,
                                            ASX_REGION_OPEN,
                                            ASX_REGION_CLOSED);
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)1);

    /* Reset should clear */
    asx_ghost_reset();
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)0);
    ASSERT_FALSE(asx_ghost_ring_overflowed());
}

/* ---- Integration: lifecycle calls trigger ghost monitors ---- */

TEST(ghost_integration_lifecycle_region_close) {
    asx_region_id rid;
    asx_budget budget;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    /* Close and drain — all transitions should be clean */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    /* No ghost violations for clean lifecycle */
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)0);
}

TEST(ghost_integration_lifecycle_obligation_leak_on_drain) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_budget budget;
    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);

    /* Drain without resolving obligation — ghost should detect leak */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    /* Ghost should have recorded the leak during finalization */
    ASSERT_TRUE(asx_ghost_violation_count() >= (uint32_t)1);
}

int main(void) {
    fprintf(stderr, "=== test_ghost ===\n");

    /* Protocol monitor */
    RUN_TEST(ghost_protocol_region_valid_transition);
    RUN_TEST(ghost_protocol_region_invalid_transition);
    RUN_TEST(ghost_protocol_task_valid_transition);
    RUN_TEST(ghost_protocol_task_invalid_transition);
    RUN_TEST(ghost_protocol_obligation_valid_transition);
    RUN_TEST(ghost_protocol_obligation_invalid_transition);

    /* Linearity monitor */
    RUN_TEST(ghost_linearity_double_commit);
    RUN_TEST(ghost_linearity_double_abort);
    RUN_TEST(ghost_linearity_commit_after_abort);
    RUN_TEST(ghost_linearity_leaked_obligation);
    RUN_TEST(ghost_linearity_no_leak_after_commit);
    RUN_TEST(ghost_linearity_no_leak_after_abort);
    RUN_TEST(ghost_linearity_multiple_obligations_mixed);

    /* Ring buffer mechanics */
    RUN_TEST(ghost_ring_empty_initially);
    RUN_TEST(ghost_ring_sequential_access);
    RUN_TEST(ghost_ring_out_of_bounds_get);
    RUN_TEST(ghost_ring_overflow_wraps);

    /* Query interface */
    RUN_TEST(ghost_violation_kind_str_coverage);
    RUN_TEST(ghost_reset_clears_violations);

    /* Integration */
    RUN_TEST(ghost_integration_lifecycle_region_close);
    RUN_TEST(ghost_integration_lifecycle_obligation_leak_on_drain);

    TEST_REPORT();
    return test_failures;
}
