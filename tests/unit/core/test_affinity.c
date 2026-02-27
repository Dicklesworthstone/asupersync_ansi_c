/*
 * test_affinity.c — tests for thread-affinity guards (bd-hwb.11)
 *
 * Compiled with -DASX_DEBUG=1 which auto-enables ASX_DEBUG_AFFINITY.
 * Tests cover:
 *   - Domain binding and query
 *   - Same-domain access (legal)
 *   - Cross-domain access violation
 *   - Explicit transfer protocol
 *   - Transfer from wrong domain (violation)
 *   - Unbind on destruction
 *   - Domain ANY passthrough
 *   - Table capacity
 *   - Stale-domain access after transfer
 *   - Double-transfer (transfer then re-transfer)
 *   - Missing-transfer path
 *   - Status string coverage for new error codes
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"

#ifndef ASX_DEBUG_AFFINITY
  #define ASX_DEBUG_AFFINITY
#endif

#include <asx/asx.h>
#include <asx/core/affinity.h>

/* ================================================================== */
/* Domain Binding and Query Tests                                      */
/* ================================================================== */

TEST(affinity_reset_clears_state) {
    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_tracked_count(), 0u);
    ASSERT_EQ(asx_affinity_current_domain(), ASX_AFFINITY_DOMAIN_ANY);
}

TEST(affinity_set_and_get_domain) {
    asx_affinity_reset();
    asx_affinity_set_domain(42u);
    ASSERT_EQ(asx_affinity_current_domain(), 42u);
}

TEST(affinity_bind_and_query) {
    uint64_t eid = 0x0001000100010001ULL;
    asx_affinity_domain dom;

    asx_affinity_reset();

    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);
    ASSERT_EQ(asx_affinity_tracked_count(), 1u);

    ASSERT_EQ(asx_affinity_get_domain(eid, &dom), ASX_OK);
    ASSERT_EQ(dom, 10u);
}

TEST(affinity_bind_invalid_id) {
    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_bind(ASX_INVALID_ID, 10u), ASX_E_INVALID_ARGUMENT);
}

TEST(affinity_bind_already_bound_same_domain) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* Re-binding to same domain is OK */
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);
    ASSERT_EQ(asx_affinity_tracked_count(), 1u);
}

TEST(affinity_bind_already_bound_different_domain) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* Re-binding to different domain fails */
    ASSERT_EQ(asx_affinity_bind(eid, 20u), ASX_E_AFFINITY_ALREADY_BOUND);
}

/* ================================================================== */
/* Access Check Tests                                                  */
/* ================================================================== */

TEST(affinity_check_same_domain_passes) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    ASSERT_EQ(asx_affinity_check(eid), ASX_OK);
}

TEST(affinity_check_wrong_domain_fails) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);
    ASSERT_EQ(asx_affinity_bind(eid, 20u), ASX_OK);

    ASSERT_EQ(asx_affinity_check(eid), ASX_E_AFFINITY_VIOLATION);
}

TEST(affinity_check_entity_domain_any_passes) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(99u);
    ASSERT_EQ(asx_affinity_bind(eid, ASX_AFFINITY_DOMAIN_ANY), ASX_OK);

    /* Entity bound to ANY passes from any domain */
    ASSERT_EQ(asx_affinity_check(eid), ASX_OK);
}

TEST(affinity_check_current_domain_any_passes) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(ASX_AFFINITY_DOMAIN_ANY);
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* Current domain ANY can access anything */
    ASSERT_EQ(asx_affinity_check(eid), ASX_OK);
}

TEST(affinity_check_untracked_entity_passes) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);

    /* Entity not in tracking table: opt-in binding, so passes */
    ASSERT_EQ(asx_affinity_check(eid), ASX_OK);
}

TEST(affinity_check_invalid_id) {
    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_check(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Transfer Tests                                                      */
/* ================================================================== */

TEST(affinity_transfer_from_same_domain) {
    uint64_t eid = 0x0001000100010001ULL;
    asx_affinity_domain dom;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* Transfer from matching domain succeeds */
    ASSERT_EQ(asx_affinity_transfer(eid, 20u), ASX_OK);

    ASSERT_EQ(asx_affinity_get_domain(eid, &dom), ASX_OK);
    ASSERT_EQ(dom, 20u);
}

TEST(affinity_transfer_from_wrong_domain_fails) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);
    ASSERT_EQ(asx_affinity_bind(eid, 20u), ASX_OK);

    /* Transfer from wrong domain fails */
    ASSERT_EQ(asx_affinity_transfer(eid, 30u), ASX_E_AFFINITY_VIOLATION);
}

TEST(affinity_transfer_untracked_entity_fails) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_transfer(eid, 10u), ASX_E_AFFINITY_NOT_BOUND);
}

TEST(affinity_stale_domain_after_transfer) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* Legal access before transfer */
    ASSERT_EQ(asx_affinity_check(eid), ASX_OK);

    /* Transfer to domain 20 */
    ASSERT_EQ(asx_affinity_transfer(eid, 20u), ASX_OK);

    /* Old domain 10 can no longer access */
    ASSERT_EQ(asx_affinity_check(eid), ASX_E_AFFINITY_VIOLATION);

    /* Switch to new domain: now access works */
    asx_affinity_set_domain(20u);
    ASSERT_EQ(asx_affinity_check(eid), ASX_OK);
}

TEST(affinity_double_transfer) {
    uint64_t eid = 0x0001000100010001ULL;
    asx_affinity_domain dom;

    asx_affinity_reset();
    asx_affinity_set_domain(10u);
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* First transfer: 10 → 20 */
    ASSERT_EQ(asx_affinity_transfer(eid, 20u), ASX_OK);

    /* Switch to new domain for second transfer */
    asx_affinity_set_domain(20u);

    /* Second transfer: 20 → 30 */
    ASSERT_EQ(asx_affinity_transfer(eid, 30u), ASX_OK);

    ASSERT_EQ(asx_affinity_get_domain(eid, &dom), ASX_OK);
    ASSERT_EQ(dom, 30u);
}

TEST(affinity_transfer_from_any_domain) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(ASX_AFFINITY_DOMAIN_ANY);
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);

    /* Domain ANY can transfer anything */
    ASSERT_EQ(asx_affinity_transfer(eid, 20u), ASX_OK);
}

TEST(affinity_transfer_entity_domain_any) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    asx_affinity_set_domain(99u);
    ASSERT_EQ(asx_affinity_bind(eid, ASX_AFFINITY_DOMAIN_ANY), ASX_OK);

    /* Entity bound to ANY can be transferred by anyone */
    ASSERT_EQ(asx_affinity_transfer(eid, 10u), ASX_OK);
}

/* ================================================================== */
/* Unbind Tests                                                        */
/* ================================================================== */

TEST(affinity_unbind_removes_tracking) {
    uint64_t eid = 0x0001000100010001ULL;
    asx_affinity_domain dom;

    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);
    ASSERT_EQ(asx_affinity_tracked_count(), 1u);

    asx_affinity_unbind(eid);
    ASSERT_EQ(asx_affinity_tracked_count(), 0u);

    /* Query after unbind returns not-bound */
    ASSERT_EQ(asx_affinity_get_domain(eid, &dom), ASX_E_AFFINITY_NOT_BOUND);
}

TEST(affinity_unbind_untracked_is_noop) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();

    /* Unbinding untracked entity doesn't crash */
    asx_affinity_unbind(eid);
    ASSERT_EQ(asx_affinity_tracked_count(), 0u);
}

/* ================================================================== */
/* Table Capacity Tests                                                */
/* ================================================================== */

TEST(affinity_table_capacity) {
    uint32_t i;

    asx_affinity_reset();

    /* Fill tracking table to capacity */
    for (i = 0; i < ASX_AFFINITY_TABLE_CAPACITY; i++) {
        uint64_t eid = 0x0001000100010000ULL + (uint64_t)(i + 1u);
        ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);
    }

    ASSERT_EQ(asx_affinity_tracked_count(), (uint32_t)ASX_AFFINITY_TABLE_CAPACITY);

    /* One more should fail */
    {
        uint64_t overflow_eid = 0x0001000100010000ULL + (uint64_t)(ASX_AFFINITY_TABLE_CAPACITY + 1u);
        ASSERT_EQ(asx_affinity_bind(overflow_eid, 10u), ASX_E_AFFINITY_TABLE_FULL);
    }
}

TEST(affinity_unbind_frees_slot) {
    uint64_t eid1 = 0x0001000100010001ULL;
    uint64_t eid2 = 0x0001000100010002ULL;
    uint32_t i;

    asx_affinity_reset();

    /* Fill table */
    ASSERT_EQ(asx_affinity_bind(eid1, 10u), ASX_OK);
    for (i = 1; i < ASX_AFFINITY_TABLE_CAPACITY; i++) {
        uint64_t eid = 0x0001000100010000ULL + (uint64_t)(i + 10u);
        ASSERT_EQ(asx_affinity_bind(eid, 10u), ASX_OK);
    }

    /* Table full */
    ASSERT_EQ(asx_affinity_bind(eid2, 10u), ASX_E_AFFINITY_TABLE_FULL);

    /* Unbind one, now there's space */
    asx_affinity_unbind(eid1);
    ASSERT_EQ(asx_affinity_bind(eid2, 10u), ASX_OK);
}

/* ================================================================== */
/* Status String Tests                                                 */
/* ================================================================== */

TEST(affinity_status_strings) {
    ASSERT_STR_EQ(asx_status_str(ASX_E_AFFINITY_VIOLATION),
                  "affinity domain violation");
    ASSERT_STR_EQ(asx_status_str(ASX_E_AFFINITY_NOT_BOUND),
                  "entity not bound to affinity domain");
    ASSERT_STR_EQ(asx_status_str(ASX_E_AFFINITY_ALREADY_BOUND),
                  "entity already bound to different domain");
    ASSERT_STR_EQ(asx_status_str(ASX_E_AFFINITY_TRANSFER_REQUIRED),
                  "cross-domain transfer required");
    ASSERT_STR_EQ(asx_status_str(ASX_E_AFFINITY_TABLE_FULL),
                  "affinity tracking table full");
}

/* ================================================================== */
/* Query Edge Cases                                                    */
/* ================================================================== */

TEST(affinity_get_domain_null_output) {
    uint64_t eid = 0x0001000100010001ULL;

    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_get_domain(eid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(affinity_get_domain_invalid_id) {
    asx_affinity_domain dom;

    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_get_domain(ASX_INVALID_ID, &dom), ASX_E_INVALID_ARGUMENT);
}

TEST(affinity_transfer_invalid_id) {
    asx_affinity_reset();
    ASSERT_EQ(asx_affinity_transfer(ASX_INVALID_ID, 10u), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_affinity ===\n");

    /* Binding and query */
    RUN_TEST(affinity_reset_clears_state);
    RUN_TEST(affinity_set_and_get_domain);
    RUN_TEST(affinity_bind_and_query);
    RUN_TEST(affinity_bind_invalid_id);
    RUN_TEST(affinity_bind_already_bound_same_domain);
    RUN_TEST(affinity_bind_already_bound_different_domain);

    /* Access checks */
    RUN_TEST(affinity_check_same_domain_passes);
    RUN_TEST(affinity_check_wrong_domain_fails);
    RUN_TEST(affinity_check_entity_domain_any_passes);
    RUN_TEST(affinity_check_current_domain_any_passes);
    RUN_TEST(affinity_check_untracked_entity_passes);
    RUN_TEST(affinity_check_invalid_id);

    /* Transfer protocol */
    RUN_TEST(affinity_transfer_from_same_domain);
    RUN_TEST(affinity_transfer_from_wrong_domain_fails);
    RUN_TEST(affinity_transfer_untracked_entity_fails);
    RUN_TEST(affinity_stale_domain_after_transfer);
    RUN_TEST(affinity_double_transfer);
    RUN_TEST(affinity_transfer_from_any_domain);
    RUN_TEST(affinity_transfer_entity_domain_any);

    /* Unbind */
    RUN_TEST(affinity_unbind_removes_tracking);
    RUN_TEST(affinity_unbind_untracked_is_noop);

    /* Table capacity */
    RUN_TEST(affinity_table_capacity);
    RUN_TEST(affinity_unbind_frees_slot);

    /* Status strings */
    RUN_TEST(affinity_status_strings);

    /* Edge cases */
    RUN_TEST(affinity_get_domain_null_output);
    RUN_TEST(affinity_get_domain_invalid_id);
    RUN_TEST(affinity_transfer_invalid_id);

    TEST_REPORT();
    return test_failures;
}
