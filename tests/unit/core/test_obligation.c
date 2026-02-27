/*
 * test_obligation.c — unit tests for obligation arena and lifecycle
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>

/* ---- Reserve / commit / abort ---- */

TEST(obligation_reserve_and_get_state) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_obligation_state state;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);
}

TEST(obligation_commit) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_obligation_state state;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
}

TEST(obligation_abort) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_obligation_state state;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_ABORTED);
}

/* ---- Terminal state absorbing ---- */

TEST(obligation_commit_is_terminal) {
    asx_region_id rid;
    asx_obligation_id oid;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(oid), ASX_OK);
    /* Cannot commit again */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
    /* Cannot abort after commit */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);
}

TEST(obligation_abort_is_terminal) {
    asx_region_id rid;
    asx_obligation_id oid;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(oid), ASX_OK);
    /* Cannot abort again */
    ASSERT_NE(asx_obligation_abort(oid), ASX_OK);
    /* Cannot commit after abort */
    ASSERT_NE(asx_obligation_commit(oid), ASX_OK);
}

/* ---- Error cases ---- */

TEST(obligation_reserve_null_out) {
    asx_region_id rid;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(obligation_reserve_invalid_region) {
    asx_obligation_id oid;
    asx_runtime_reset();
    ASSERT_EQ(asx_obligation_reserve(ASX_INVALID_ID, &oid), ASX_E_NOT_FOUND);
}

TEST(obligation_get_state_null_out) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(oid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(obligation_invalid_handle) {
    asx_obligation_state state;
    asx_runtime_reset();
    ASSERT_EQ(asx_obligation_get_state(ASX_INVALID_ID, &state),
              ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_commit(ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_abort(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

/* ---- Region admission ---- */

TEST(obligation_reserve_rejected_after_close) {
    asx_region_id rid;
    asx_obligation_id oid;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_NOT_OPEN);

    /* Also rejected after drain */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_NE(asx_obligation_reserve(rid, &oid), ASX_OK);
}

/* ---- Handle correctness ---- */

TEST(obligation_handle_type_tag) {
    asx_region_id rid;
    asx_obligation_id oid;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_OK);
    ASSERT_EQ(asx_handle_type_tag(oid), ASX_TYPE_OBLIGATION);
}

TEST(obligation_multiple_in_region) {
    asx_region_id rid;
    asx_obligation_id o1, o2, o3;
    asx_obligation_state state;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o1), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o2), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(rid, &o3), ASX_OK);

    /* Each has independent state */
    ASSERT_EQ(asx_obligation_commit(o1), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(o3), ASX_OK);

    ASSERT_EQ(asx_obligation_get_state(o1, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);

    ASSERT_EQ(asx_obligation_get_state(o2, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);

    ASSERT_EQ(asx_obligation_get_state(o3, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_ABORTED);
}

int main(void) {
    fprintf(stderr, "=== test_obligation ===\n");
    RUN_TEST(obligation_reserve_and_get_state);
    RUN_TEST(obligation_commit);
    RUN_TEST(obligation_abort);
    RUN_TEST(obligation_commit_is_terminal);
    RUN_TEST(obligation_abort_is_terminal);
    RUN_TEST(obligation_reserve_null_out);
    RUN_TEST(obligation_reserve_invalid_region);
    RUN_TEST(obligation_get_state_null_out);
    RUN_TEST(obligation_invalid_handle);
    RUN_TEST(obligation_reserve_rejected_after_close);
    RUN_TEST(obligation_handle_type_tag);
    RUN_TEST(obligation_multiple_in_region);
    TEST_REPORT();
    return test_failures;
}
