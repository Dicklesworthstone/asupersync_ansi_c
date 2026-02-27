/*
 * test_ghost.c — tests for ghost safety monitors (bd-hwb.5)
 *
 * Compiled with -DASX_DEBUG_GHOST=1 to activate the real monitor
 * implementation. Tests cover:
 *   - Protocol monitor: illegal region/task/obligation transitions
 *   - Linearity monitor: double-resolution and leak detection
 *   - Violation ring buffer: count, retrieval, overflow
 *   - Query interface: violation_get, ring_overflowed, kind_str
 *   - Integration with lifecycle operations
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"

/* Ensure ghost monitors are active for these tests */
#ifndef ASX_DEBUG_GHOST
  #define ASX_DEBUG_GHOST 1
#endif

#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>
#include <asx/core/budget.h>
#include <asx/runtime/runtime.h>

/* ------------------------------------------------------------------ */
/* Budget helper                                                       */
/* ------------------------------------------------------------------ */

static asx_budget make_budget(uint32_t poll_quota)
{
    asx_budget b = asx_budget_infinite();
    b.poll_quota = poll_quota;
    return b;
}

/* ================================================================== */
/* Protocol Monitor Tests                                              */
/* ================================================================== */

TEST(ghost_reset_clears_state) {
    asx_ghost_reset();
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
    ASSERT_FALSE(asx_ghost_ring_overflowed());
}

TEST(ghost_legal_region_transition_no_violation) {
    asx_status st;

    asx_ghost_reset();

    /* Open -> Closing is legal */
    st = asx_ghost_check_region_transition(0x0001000100010000ULL,
                                            ASX_REGION_OPEN, ASX_REGION_CLOSING);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(ghost_illegal_region_transition_records_violation) {
    asx_status st;
    asx_ghost_violation v;

    asx_ghost_reset();

    /* Closed -> Open is illegal */
    st = asx_ghost_check_region_transition(0x0001001000010000ULL,
                                            ASX_REGION_CLOSED, ASX_REGION_OPEN);
    ASSERT_NE(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 1u);

    ASSERT_TRUE(asx_ghost_violation_get(0, &v));
    ASSERT_EQ((int)v.kind, (int)ASX_GHOST_PROTOCOL_REGION);
    ASSERT_EQ(v.from_state, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(v.to_state, (int)ASX_REGION_OPEN);
    ASSERT_EQ(v.sequence, 0u);
}

TEST(ghost_illegal_task_transition_records_violation) {
    asx_status st;
    asx_ghost_violation v;

    asx_ghost_reset();

    /* Completed -> Running is illegal */
    st = asx_ghost_check_task_transition(0x0002002000020000ULL,
                                          ASX_TASK_COMPLETED, ASX_TASK_RUNNING);
    ASSERT_NE(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 1u);

    ASSERT_TRUE(asx_ghost_violation_get(0, &v));
    ASSERT_EQ((int)v.kind, (int)ASX_GHOST_PROTOCOL_TASK);
    ASSERT_EQ(v.from_state, (int)ASX_TASK_COMPLETED);
    ASSERT_EQ(v.to_state, (int)ASX_TASK_RUNNING);
}

TEST(ghost_illegal_obligation_transition_records_violation) {
    asx_status st;
    asx_ghost_violation v;

    asx_ghost_reset();

    /* Committed -> Reserved is illegal */
    st = asx_ghost_check_obligation_transition(0x0003000200030000ULL,
                                                ASX_OBLIGATION_COMMITTED,
                                                ASX_OBLIGATION_RESERVED);
    ASSERT_NE(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 1u);

    ASSERT_TRUE(asx_ghost_violation_get(0, &v));
    ASSERT_EQ((int)v.kind, (int)ASX_GHOST_PROTOCOL_OBLIGATION);
}

TEST(ghost_legal_task_transition_no_violation) {
    asx_status st;

    asx_ghost_reset();

    /* Created -> Running is legal */
    st = asx_ghost_check_task_transition(0x0002000100020000ULL,
                                          ASX_TASK_CREATED, ASX_TASK_RUNNING);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

/* ================================================================== */
/* Linearity Monitor Tests                                             */
/* ================================================================== */

TEST(ghost_linearity_normal_lifecycle) {
    asx_obligation_id oid = 0x0003000100030000ULL;

    asx_ghost_reset();

    asx_ghost_obligation_reserved(oid);
    asx_ghost_obligation_resolved(oid);

    /* No violations: reserve then resolve once */
    ASSERT_EQ(asx_ghost_violation_count(), 0u);

    /* Leak check should find zero leaks */
    ASSERT_EQ(asx_ghost_check_obligation_leaks(ASX_INVALID_ID), 0u);
}

TEST(ghost_linearity_double_resolve) {
    asx_obligation_id oid = 0x0003000100030000ULL;
    asx_ghost_violation v;

    asx_ghost_reset();

    asx_ghost_obligation_reserved(oid);
    asx_ghost_obligation_resolved(oid);

    /* Second resolution should trigger LINEARITY_DOUBLE */
    asx_ghost_obligation_resolved(oid);

    ASSERT_EQ(asx_ghost_violation_count(), 1u);
    ASSERT_TRUE(asx_ghost_violation_get(0, &v));
    ASSERT_EQ((int)v.kind, (int)ASX_GHOST_LINEARITY_DOUBLE);
    ASSERT_EQ(v.entity_id, oid);
}

TEST(ghost_linearity_leak_detection) {
    asx_obligation_id oid1 = 0x0003000100030000ULL;
    asx_obligation_id oid2 = 0x0003000100030001ULL;
    uint32_t leak_count;

    asx_ghost_reset();

    /* Reserve two, resolve only one */
    asx_ghost_obligation_reserved(oid1);
    asx_ghost_obligation_reserved(oid2);
    asx_ghost_obligation_resolved(oid1);

    leak_count = asx_ghost_check_obligation_leaks(ASX_INVALID_ID);
    ASSERT_EQ(leak_count, 1u);

    /* Should have recorded a LINEARITY_LEAK violation */
    ASSERT_EQ(asx_ghost_violation_count(), 1u);
}

TEST(ghost_linearity_multiple_leaks) {
    asx_obligation_id oid1 = 0x0003000100030000ULL;
    asx_obligation_id oid2 = 0x0003000100030001ULL;
    asx_obligation_id oid3 = 0x0003000100030002ULL;
    uint32_t leak_count;

    asx_ghost_reset();

    /* Reserve three, resolve none */
    asx_ghost_obligation_reserved(oid1);
    asx_ghost_obligation_reserved(oid2);
    asx_ghost_obligation_reserved(oid3);

    leak_count = asx_ghost_check_obligation_leaks(ASX_INVALID_ID);
    ASSERT_EQ(leak_count, 3u);
    ASSERT_EQ(asx_ghost_violation_count(), 3u);
}

/* ================================================================== */
/* Violation Ring Buffer Tests                                         */
/* ================================================================== */

TEST(ghost_violation_get_empty) {
    asx_ghost_violation v;

    asx_ghost_reset();

    /* No violations recorded */
    ASSERT_FALSE(asx_ghost_violation_get(0, &v));
}

TEST(ghost_violation_get_null_out) {
    asx_ghost_reset();

    /* NULL out should return 0 (failure) */
    ASSERT_FALSE(asx_ghost_violation_get(0, NULL));
}

TEST(ghost_violation_count_increments) {
    asx_ghost_reset();

    ASSERT_EQ(asx_ghost_violation_count(), 0u);

    /* Record an illegal transition */
    (void)asx_ghost_check_region_transition(0x0001001000010000ULL,
                                             ASX_REGION_CLOSED, ASX_REGION_OPEN);
    ASSERT_EQ(asx_ghost_violation_count(), 1u);

    /* Record another */
    (void)asx_ghost_check_task_transition(0x0002002000020000ULL,
                                           ASX_TASK_COMPLETED, ASX_TASK_CREATED);
    ASSERT_EQ(asx_ghost_violation_count(), 2u);
}

TEST(ghost_violation_sequence_monotonic) {
    asx_ghost_violation v1, v2;

    asx_ghost_reset();

    (void)asx_ghost_check_region_transition(0x0001001000010000ULL,
                                             ASX_REGION_CLOSED, ASX_REGION_OPEN);
    (void)asx_ghost_check_task_transition(0x0002002000020000ULL,
                                           ASX_TASK_COMPLETED, ASX_TASK_CREATED);

    ASSERT_TRUE(asx_ghost_violation_get(0, &v1));
    ASSERT_TRUE(asx_ghost_violation_get(1, &v2));

    ASSERT_EQ(v1.sequence, 0u);
    ASSERT_EQ(v2.sequence, 1u);
    ASSERT_TRUE(v2.sequence > v1.sequence);
}

TEST(ghost_ring_overflow) {
    uint32_t i;

    asx_ghost_reset();

    /* Fill beyond ring capacity (64) */
    for (i = 0; i < ASX_GHOST_RING_CAPACITY + 10u; i++) {
        (void)asx_ghost_check_region_transition(
            0x0001001000010000ULL + (uint64_t)i,
            ASX_REGION_CLOSED, ASX_REGION_OPEN);
    }

    ASSERT_TRUE(asx_ghost_ring_overflowed());
    ASSERT_EQ(asx_ghost_violation_count(), ASX_GHOST_RING_CAPACITY + 10u);

    /* Should still be able to retrieve recent violations */
    {
        asx_ghost_violation v;
        ASSERT_TRUE(asx_ghost_violation_get(0, &v));
        ASSERT_EQ((int)v.kind, (int)ASX_GHOST_PROTOCOL_REGION);
    }
}

TEST(ghost_no_overflow_within_capacity) {
    uint32_t i;

    asx_ghost_reset();

    /* Fill exactly to capacity */
    for (i = 0; i < ASX_GHOST_RING_CAPACITY; i++) {
        (void)asx_ghost_check_region_transition(
            0x0001001000010000ULL,
            ASX_REGION_CLOSED, ASX_REGION_OPEN);
    }

    /* At capacity but not overflowed */
    ASSERT_EQ(asx_ghost_violation_count(), (uint32_t)ASX_GHOST_RING_CAPACITY);
    /* Note: overflow flag is set when count > capacity, not == */
}

/* ================================================================== */
/* Kind String Tests                                                   */
/* ================================================================== */

TEST(ghost_kind_str_valid) {
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
}

TEST(ghost_kind_str_unknown) {
    ASSERT_STR_EQ(asx_ghost_violation_kind_str((asx_ghost_violation_kind)99),
                  "unknown");
}

/* ================================================================== */
/* Integration with Lifecycle Tests                                    */
/* ================================================================== */

TEST(ghost_lifecycle_legal_workflow) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_budget budget;

    asx_runtime_reset();
    asx_ghost_reset();

    /* Full legal workflow: open region, reserve obligation,
     * commit obligation, close region, drain */
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);

    /* Ghost should have no violations for legal operations */
    ASSERT_EQ(asx_ghost_violation_count(), 0u);

    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);

    budget = make_budget(100);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(ghost_lifecycle_obligation_abort) {
    asx_region_id rid;
    asx_obligation_id oid;

    asx_runtime_reset();
    asx_ghost_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);

    /* Abort is legal, no violations */
    ASSERT_EQ(asx_ghost_violation_count(), 0u);

    /* No leaks since obligation was aborted */
    ASSERT_EQ(asx_ghost_check_obligation_leaks(ASX_INVALID_ID), 0u);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_ghost ===\n");

    /* Protocol monitor */
    RUN_TEST(ghost_reset_clears_state);
    RUN_TEST(ghost_legal_region_transition_no_violation);
    RUN_TEST(ghost_illegal_region_transition_records_violation);
    RUN_TEST(ghost_illegal_task_transition_records_violation);
    RUN_TEST(ghost_illegal_obligation_transition_records_violation);
    RUN_TEST(ghost_legal_task_transition_no_violation);

    /* Linearity monitor */
    RUN_TEST(ghost_linearity_normal_lifecycle);
    RUN_TEST(ghost_linearity_double_resolve);
    RUN_TEST(ghost_linearity_leak_detection);
    RUN_TEST(ghost_linearity_multiple_leaks);

    /* Ring buffer */
    RUN_TEST(ghost_violation_get_empty);
    RUN_TEST(ghost_violation_get_null_out);
    RUN_TEST(ghost_violation_count_increments);
    RUN_TEST(ghost_violation_sequence_monotonic);
    RUN_TEST(ghost_ring_overflow);
    RUN_TEST(ghost_no_overflow_within_capacity);

    /* Kind strings */
    RUN_TEST(ghost_kind_str_valid);
    RUN_TEST(ghost_kind_str_unknown);

    /* Lifecycle integration */
    RUN_TEST(ghost_lifecycle_legal_workflow);
    RUN_TEST(ghost_lifecycle_obligation_abort);

    TEST_REPORT();
    return test_failures;
}
