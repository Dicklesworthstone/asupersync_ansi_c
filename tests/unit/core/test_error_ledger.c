/*
 * test_error_ledger.c — tests for must-use manifest and error ledger helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>

static asx_status leaf_failure(void)
{
    return ASX_E_INVALID_TRANSITION;
}

static asx_status mid_failure(void)
{
    ASX_TRY(leaf_failure());
    return ASX_OK;
}

static asx_status top_failure(void)
{
    ASX_TRY(mid_failure());
    return ASX_OK;
}

static asx_status explicit_leaf_failure(void)
{
    return ASX_E_CANCELLED;
}

static asx_status explicit_mid_failure(asx_task_id task_id)
{
    ASX_TRY_TASK(task_id, explicit_leaf_failure());
    return ASX_OK;
}

static int manifest_contains(const char *needle)
{
    uint32_t i;
    uint32_t n = asx_must_use_surface_count();
    for (i = 0; i < n; i++) {
        const char *name = asx_must_use_surface_name(i);
        if (name != NULL && strcmp(name, needle) == 0) {
            return 1;
        }
    }
    return 0;
}

TEST(ledger_records_bound_task_context) {
    asx_error_ledger_entry entry;
    asx_task_id tid = asx_handle_pack(
        ASX_TYPE_TASK, 0, asx_handle_pack_index(1u, 3u)
    );

    asx_error_ledger_reset();
    asx_error_ledger_bind_task(tid);
    asx_error_ledger_record_current(ASX_E_INVALID_ARGUMENT, "setup", "unit-test", 42u);

    ASSERT_EQ(asx_error_ledger_count(tid), 1u);
    ASSERT_TRUE(asx_error_ledger_get(tid, 0u, &entry));
    ASSERT_EQ(entry.task_id, tid);
    ASSERT_EQ(entry.status, ASX_E_INVALID_ARGUMENT);
    ASSERT_STR_EQ(entry.operation, "setup");
    ASSERT_STR_EQ(entry.file, "unit-test");
    ASSERT_EQ(entry.line, 42u);
    ASSERT_EQ(entry.sequence, 0u);
}

TEST(try_captures_multi_hop_breadcrumbs) {
    asx_error_ledger_entry first;
    asx_error_ledger_entry second;
    asx_task_id tid = asx_handle_pack(
        ASX_TYPE_TASK, 0, asx_handle_pack_index(2u, 4u)
    );
    asx_status st;

    asx_error_ledger_reset();
    asx_error_ledger_bind_task(tid);
    st = top_failure();
    ASSERT_EQ(st, ASX_E_INVALID_TRANSITION);

    ASSERT_EQ(asx_error_ledger_count(tid), 2u);
    ASSERT_TRUE(asx_error_ledger_get(tid, 0u, &first));
    ASSERT_TRUE(asx_error_ledger_get(tid, 1u, &second));
    ASSERT_STR_EQ(first.operation, "leaf_failure()");
    ASSERT_STR_EQ(second.operation, "mid_failure()");
    ASSERT_EQ(first.sequence, 0u);
    ASSERT_EQ(second.sequence, 1u);
}

TEST(try_task_uses_explicit_task_context) {
    asx_error_ledger_entry entry;
    asx_task_id tid = asx_handle_pack(
        ASX_TYPE_TASK, 0, asx_handle_pack_index(7u, 9u)
    );
    asx_status st;

    asx_error_ledger_reset();
    st = explicit_mid_failure(tid);
    ASSERT_EQ(st, ASX_E_CANCELLED);

    ASSERT_EQ(asx_error_ledger_count(tid), 1u);
    ASSERT_TRUE(asx_error_ledger_get(tid, 0u, &entry));
    ASSERT_EQ(entry.task_id, tid);
    ASSERT_STR_EQ(entry.operation, "explicit_leaf_failure()");
}

TEST(ledger_overflow_is_deterministic_ring) {
    asx_error_ledger_entry oldest;
    asx_error_ledger_entry newest;
    asx_task_id tid = asx_handle_pack(
        ASX_TYPE_TASK, 0, asx_handle_pack_index(5u, 1u)
    );
    uint32_t i;
    const uint32_t extra = 3u;
    const uint32_t total = ASX_ERROR_LEDGER_DEPTH + extra;

    asx_error_ledger_reset();
    for (i = 0; i < total; i++) {
        asx_error_ledger_record_for_task(
            tid, ASX_E_INVALID_STATE, "overflow_step", "ring", i + 1u
        );
    }

    ASSERT_EQ(asx_error_ledger_count(tid), ASX_ERROR_LEDGER_DEPTH);
    ASSERT_TRUE(asx_error_ledger_overflowed(tid));
    ASSERT_TRUE(asx_error_ledger_get(tid, 0u, &oldest));
    ASSERT_TRUE(asx_error_ledger_get(tid, ASX_ERROR_LEDGER_DEPTH - 1u, &newest));
    ASSERT_EQ(oldest.sequence, extra);
    ASSERT_EQ(newest.sequence, total - 1u);
}

TEST(must_use_manifest_covers_transition_and_acquisition_surfaces) {
    ASSERT_TRUE(asx_must_use_surface_count() >= 10u);
    ASSERT_TRUE(manifest_contains("asx_region_transition_check"));
    ASSERT_TRUE(manifest_contains("asx_task_transition_check"));
    ASSERT_TRUE(manifest_contains("asx_obligation_transition_check"));
    ASSERT_TRUE(manifest_contains("asx_region_slot_lookup"));
    ASSERT_TRUE(manifest_contains("asx_task_slot_lookup"));
    ASSERT_TRUE(manifest_contains("asx_region_open"));
    ASSERT_TRUE(manifest_contains("asx_task_spawn"));
    ASSERT_TRUE(manifest_contains("asx_cleanup_push"));
    ASSERT_TRUE(manifest_contains("asx_cleanup_pop"));
    ASSERT_TRUE(asx_must_use_surface_name(asx_must_use_surface_count()) == NULL);
}

int main(void) {
    fprintf(stderr, "=== test_error_ledger ===\n");
    RUN_TEST(ledger_records_bound_task_context);
    RUN_TEST(try_captures_multi_hop_breadcrumbs);
    RUN_TEST(try_task_uses_explicit_task_context);
    RUN_TEST(ledger_overflow_is_deterministic_ring);
    RUN_TEST(must_use_manifest_covers_transition_and_acquisition_surfaces);
    TEST_REPORT();
    return test_failures;
}
