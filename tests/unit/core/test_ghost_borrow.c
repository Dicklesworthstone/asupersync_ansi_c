/*
 * test_ghost_borrow.c — tests for ghost borrow ledger and determinism monitor (bd-hwb.12)
 *
 * Compiled with -DASX_DEBUG=1 which auto-enables ASX_DEBUG_GHOST.
 * Tests cover:
 *   - Borrow ledger: shared/exclusive tracking, conflict detection
 *   - Borrow ledger: release, release_all, slot reclamation
 *   - Determinism monitor: record, seal, replay, drift detection
 *   - Determinism monitor: digest stability
 *   - Integration with violation ring buffer
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"

#ifndef ASX_DEBUG_GHOST
  #define ASX_DEBUG_GHOST 1
#endif

#include <asx/asx.h>
#include <asx/core/ghost.h>

/* ================================================================== */
/* Borrow Ledger — Shared Borrows                                      */
/* ================================================================== */

TEST(borrow_shared_single) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    ASSERT_EQ(asx_ghost_borrow_shared(eid), 1u);
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 1u);
    ASSERT_FALSE(asx_ghost_borrow_is_exclusive(eid));
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(borrow_shared_multiple) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    ASSERT_EQ(asx_ghost_borrow_shared(eid), 1u);
    ASSERT_EQ(asx_ghost_borrow_shared(eid), 2u);
    ASSERT_EQ(asx_ghost_borrow_shared(eid), 3u);
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 3u);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(borrow_shared_release) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    asx_ghost_borrow_shared(eid);
    asx_ghost_borrow_shared(eid);
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 2u);

    asx_ghost_borrow_release(eid);
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 1u);

    asx_ghost_borrow_release(eid);
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 0u);
}

/* ================================================================== */
/* Borrow Ledger — Exclusive Borrows                                   */
/* ================================================================== */

TEST(borrow_exclusive_single) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    ASSERT_TRUE(asx_ghost_borrow_exclusive(eid));
    ASSERT_TRUE(asx_ghost_borrow_is_exclusive(eid));
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 0u);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(borrow_exclusive_release) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    ASSERT_TRUE(asx_ghost_borrow_exclusive(eid));
    asx_ghost_borrow_release(eid);

    ASSERT_FALSE(asx_ghost_borrow_is_exclusive(eid));
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

/* ================================================================== */
/* Borrow Ledger — Conflict Detection                                  */
/* ================================================================== */

TEST(borrow_exclusive_while_shared_active) {
    uint64_t eid = 0x0001000100010001ULL;
    asx_ghost_violation v;

    asx_ghost_reset();

    asx_ghost_borrow_shared(eid);

    /* Exclusive while shared active → violation */
    ASSERT_FALSE(asx_ghost_borrow_exclusive(eid));
    ASSERT_EQ(asx_ghost_violation_count(), 1u);

    ASSERT_TRUE(asx_ghost_violation_get(0, &v));
    ASSERT_EQ((int)v.kind, (int)ASX_GHOST_BORROW_EXCLUSIVE);
    ASSERT_EQ(v.entity_id, eid);
}

TEST(borrow_shared_while_exclusive_active) {
    uint64_t eid = 0x0001000100010001ULL;
    asx_ghost_violation v;

    asx_ghost_reset();

    asx_ghost_borrow_exclusive(eid);

    /* Shared while exclusive active → violation */
    (void)asx_ghost_borrow_shared(eid);
    ASSERT_EQ(asx_ghost_violation_count(), 1u);

    ASSERT_TRUE(asx_ghost_violation_get(0, &v));
    ASSERT_EQ((int)v.kind, (int)ASX_GHOST_BORROW_SHARED);
    ASSERT_EQ(v.entity_id, eid);
}

TEST(borrow_double_exclusive) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    ASSERT_TRUE(asx_ghost_borrow_exclusive(eid));
    ASSERT_FALSE(asx_ghost_borrow_exclusive(eid));
    ASSERT_EQ(asx_ghost_violation_count(), 1u);
}

/* ================================================================== */
/* Borrow Ledger — Release All                                         */
/* ================================================================== */

TEST(borrow_release_all_clears_shared) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    asx_ghost_borrow_shared(eid);
    asx_ghost_borrow_shared(eid);
    asx_ghost_borrow_shared(eid);

    asx_ghost_borrow_release_all(eid);
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 0u);
    ASSERT_FALSE(asx_ghost_borrow_is_exclusive(eid));
}

TEST(borrow_release_all_clears_exclusive) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    asx_ghost_borrow_exclusive(eid);

    asx_ghost_borrow_release_all(eid);
    ASSERT_FALSE(asx_ghost_borrow_is_exclusive(eid));
}

/* ================================================================== */
/* Borrow Ledger — Multiple Entities                                   */
/* ================================================================== */

TEST(borrow_independent_entities) {
    uint64_t eid1 = 0x0001000100010001ULL;
    uint64_t eid2 = 0x0001000100010002ULL;

    asx_ghost_reset();

    /* Independent exclusive borrows on different entities: no conflict */
    ASSERT_TRUE(asx_ghost_borrow_exclusive(eid1));
    ASSERT_TRUE(asx_ghost_borrow_exclusive(eid2));
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

/* ================================================================== */
/* Borrow Ledger — Kind Strings                                        */
/* ================================================================== */

TEST(borrow_kind_strings) {
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_BORROW_EXCLUSIVE),
                  "borrow_exclusive");
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_BORROW_SHARED),
                  "borrow_shared");
    ASSERT_STR_EQ(asx_ghost_violation_kind_str(ASX_GHOST_DETERMINISM_DRIFT),
                  "determinism_drift");
}

/* ================================================================== */
/* Determinism Monitor — Recording and Digest                          */
/* ================================================================== */

TEST(determinism_empty_digest) {
    uint64_t d1, d2;

    asx_ghost_reset();

    d1 = asx_ghost_determinism_digest();

    asx_ghost_determinism_reset();
    d2 = asx_ghost_determinism_digest();

    /* Same empty sequence → same digest */
    ASSERT_EQ(d1, d2);
    ASSERT_EQ(asx_ghost_determinism_event_count(), 0u);
}

TEST(determinism_record_events) {
    asx_ghost_reset();

    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(200);
    asx_ghost_determinism_record(300);

    ASSERT_EQ(asx_ghost_determinism_event_count(), 3u);
}

TEST(determinism_digest_stability) {
    uint64_t d1, d2;

    asx_ghost_reset();
    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(200);
    asx_ghost_determinism_record(300);
    d1 = asx_ghost_determinism_digest();

    /* Same sequence in a fresh run */
    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(200);
    asx_ghost_determinism_record(300);
    d2 = asx_ghost_determinism_digest();

    ASSERT_EQ(d1, d2);
}

TEST(determinism_digest_differs_on_different_input) {
    uint64_t d1, d2;

    asx_ghost_reset();
    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(200);
    d1 = asx_ghost_determinism_digest();

    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(999);
    d2 = asx_ghost_determinism_digest();

    ASSERT_NE(d1, d2);
}

/* ================================================================== */
/* Determinism Monitor — Seal and Check                                */
/* ================================================================== */

TEST(determinism_seal_and_replay_stable) {
    asx_ghost_reset();

    /* First run */
    asx_ghost_determinism_record(10);
    asx_ghost_determinism_record(20);
    asx_ghost_determinism_record(30);
    asx_ghost_determinism_seal();

    /* Second run: same sequence */
    asx_ghost_determinism_record(10);
    asx_ghost_determinism_record(20);
    asx_ghost_determinism_record(30);

    ASSERT_EQ(asx_ghost_determinism_check(), 0u);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(determinism_seal_and_replay_drift) {
    uint32_t drift;

    asx_ghost_reset();

    /* First run */
    asx_ghost_determinism_record(10);
    asx_ghost_determinism_record(20);
    asx_ghost_determinism_record(30);
    asx_ghost_determinism_seal();

    /* Second run: different order */
    asx_ghost_determinism_record(10);
    asx_ghost_determinism_record(30);  /* swapped */
    asx_ghost_determinism_record(20);  /* swapped */

    drift = asx_ghost_determinism_check();
    ASSERT_TRUE(drift > 0u);
    ASSERT_TRUE(asx_ghost_violation_count() > 0u);
}

TEST(determinism_length_mismatch_detected) {
    uint32_t drift;

    asx_ghost_reset();

    /* First run: 3 events */
    asx_ghost_determinism_record(10);
    asx_ghost_determinism_record(20);
    asx_ghost_determinism_record(30);
    asx_ghost_determinism_seal();

    /* Second run: only 2 events */
    asx_ghost_determinism_record(10);
    asx_ghost_determinism_record(20);

    drift = asx_ghost_determinism_check();
    ASSERT_TRUE(drift > 0u);
}

TEST(determinism_check_without_seal) {
    asx_ghost_reset();

    asx_ghost_determinism_record(10);

    /* No seal → check returns 0 (no reference) */
    ASSERT_EQ(asx_ghost_determinism_check(), 0u);
}

/* ================================================================== */
/* Borrow Ledger — Slot Reclamation                                    */
/* ================================================================== */

TEST(borrow_slot_reclaimed_after_release) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    asx_ghost_borrow_shared(eid);
    asx_ghost_borrow_release(eid);

    /* After releasing all borrows, slot is reclaimed */
    ASSERT_EQ(asx_ghost_borrow_shared_count(eid), 0u);

    /* Can re-borrow (allocates new slot) */
    ASSERT_TRUE(asx_ghost_borrow_exclusive(eid));
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

TEST(borrow_release_untracked_is_noop) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_ghost_reset();

    /* Releasing entity that was never borrowed doesn't crash */
    asx_ghost_borrow_release(eid);
    asx_ghost_borrow_release_all(eid);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_ghost_borrow ===\n");

    /* Shared borrows */
    RUN_TEST(borrow_shared_single);
    RUN_TEST(borrow_shared_multiple);
    RUN_TEST(borrow_shared_release);

    /* Exclusive borrows */
    RUN_TEST(borrow_exclusive_single);
    RUN_TEST(borrow_exclusive_release);

    /* Conflict detection */
    RUN_TEST(borrow_exclusive_while_shared_active);
    RUN_TEST(borrow_shared_while_exclusive_active);
    RUN_TEST(borrow_double_exclusive);

    /* Release all */
    RUN_TEST(borrow_release_all_clears_shared);
    RUN_TEST(borrow_release_all_clears_exclusive);

    /* Multiple entities */
    RUN_TEST(borrow_independent_entities);

    /* Kind strings */
    RUN_TEST(borrow_kind_strings);

    /* Determinism: recording and digest */
    RUN_TEST(determinism_empty_digest);
    RUN_TEST(determinism_record_events);
    RUN_TEST(determinism_digest_stability);
    RUN_TEST(determinism_digest_differs_on_different_input);

    /* Determinism: seal and check */
    RUN_TEST(determinism_seal_and_replay_stable);
    RUN_TEST(determinism_seal_and_replay_drift);
    RUN_TEST(determinism_length_mismatch_detected);
    RUN_TEST(determinism_check_without_seal);

    /* Slot reclamation */
    RUN_TEST(borrow_slot_reclaimed_after_release);
    RUN_TEST(borrow_release_untracked_is_noop);

    TEST_REPORT();
    return test_failures;
}
