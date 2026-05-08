/*
 * public_api_misuse_conformance_test.c - public API misuse negative conformance
 *
 * Freezes exact status taxonomy for invalid public API calls and proves
 * rejected calls do not mutate runtime snapshots or emit semantic events.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/event.h>
#include <asx/runtime/snapshot.h>
#include <asx/runtime/trace.h>
#include <asx/time/deadline.h>

typedef struct misuse_guard {
    asx_runtime_snapshot snapshot;
    uint32_t event_log_count;
    uint32_t scheduler_event_count;
    uint32_t trace_event_count;
    uint64_t event_hash;
    uint32_t ghost_violation_count;
} misuse_guard;

static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static void reset_all(void) {
    asx_runtime_reset();
    asx_ghost_reset();
    asx_scheduler_event_reset();
    asx_event_log_reset();
    asx_trace_reset();
}

static void guard_capture(misuse_guard *guard) {
    ASSERT_EQ(asx_runtime_snapshot_capture(&guard->snapshot), ASX_OK);
    guard->event_log_count = asx_event_log_count();
    guard->scheduler_event_count = asx_scheduler_event_count();
    guard->trace_event_count = asx_trace_event_count();
    guard->event_hash = asx_event_hash_chain();
    guard->ghost_violation_count = asx_ghost_violation_count();
}

static void guard_assert_no_semantic_mutation(const misuse_guard *before) {
    asx_runtime_snapshot after;

    ASSERT_EQ(asx_runtime_snapshot_capture(&after), ASX_OK);
    ASSERT_EQ(asx_runtime_snapshot_eq(&before->snapshot, &after), ASX_OK);
    ASSERT_EQ(asx_event_log_count(), before->event_log_count);
    ASSERT_EQ(asx_scheduler_event_count(), before->scheduler_event_count);
    ASSERT_EQ(asx_trace_event_count(), before->trace_event_count);
    ASSERT_EQ(asx_event_hash_chain(), before->event_hash);
    ASSERT_TRUE(asx_ghost_violation_count() >= before->ghost_violation_count);
}

TEST(unknown_and_wrong_type_handles_fail_without_events) {
    misuse_guard guard;
    asx_region_state region_state;
    asx_task_state task_state;
    asx_obligation_state obligation_state;
    asx_outcome outcome;
    asx_task_id task_out;
    asx_obligation_id obligation_out;
    asx_budget budget;
    asx_region_id wrong_region;
    asx_task_id wrong_task;
    asx_task_id out_of_range_task;

    reset_all();
    budget = asx_budget_infinite();
    wrong_region = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                                   asx_handle_pack_index(0u, 0u));
    wrong_task = asx_handle_pack(ASX_TYPE_REGION, (uint16_t)(1u << (unsigned)ASX_REGION_OPEN),
                                 asx_handle_pack_index(0u, 0u));
    out_of_range_task = asx_handle_pack(ASX_TYPE_TASK, (uint16_t)(1u << (unsigned)ASX_TASK_CREATED),
                                        asx_handle_pack_index(0u, (uint16_t)ASX_MAX_TASKS));

    guard_capture(&guard);

    ASSERT_EQ(asx_region_close(ASX_INVALID_ID), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_get_state(wrong_region, &region_state), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_get_state(wrong_task, &task_state), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_get_outcome(ASX_INVALID_ID, &outcome), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_spawn(ASX_INVALID_ID, poll_pending, NULL, &task_out), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_cancel(out_of_range_task, ASX_CANCEL_USER), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_get_state(ASX_INVALID_ID, &obligation_state), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_obligation_reserve(ASX_INVALID_ID, &obligation_out), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_scheduler_run(ASX_INVALID_ID, &budget), ASX_E_NOT_FOUND);

    guard_assert_no_semantic_mutation(&guard);
}

TEST(stale_region_handles_fail_without_events) {
    misuse_guard guard;
    asx_region_id stale_region;
    asx_region_id fresh_region;
    asx_task_id task_out;
    asx_obligation_id obligation_out;
    asx_budget budget;

    reset_all();
    ASSERT_EQ(asx_region_open(&stale_region), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(stale_region, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&fresh_region), ASX_OK);
    ASSERT_NE(stale_region, fresh_region);

    budget = asx_budget_infinite();
    guard_capture(&guard);

    ASSERT_EQ(asx_region_close(stale_region), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_task_spawn(stale_region, poll_pending, NULL, &task_out), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_obligation_reserve(stale_region, &obligation_out), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_scheduler_run(stale_region, &budget), ASX_E_STALE_HANDLE);

    guard_assert_no_semantic_mutation(&guard);
}

TEST(double_close_and_finalize_fail_without_events) {
    misuse_guard guard;
    asx_region_id region;
    asx_task_id task;
    asx_checkpoint_result checkpoint;

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_region_close(region), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_region_close(region), ASX_E_INVALID_TRANSITION);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, poll_pending, NULL, &task), ASX_OK);
    ASSERT_EQ(asx_task_cancel(task, ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_checkpoint(task, &checkpoint), ASX_OK);
    ASSERT_EQ(asx_task_finalize(task), ASX_OK);

    guard_capture(&guard);
    ASSERT_EQ(asx_task_finalize(task), ASX_E_INVALID_STATE);
    guard_assert_no_semantic_mutation(&guard);
}

TEST(obligation_illegal_transitions_fail_without_events) {
    misuse_guard guard;
    asx_region_id region;
    asx_obligation_id obligation;

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(region, &obligation), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(obligation), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_obligation_commit(obligation), ASX_E_INVALID_TRANSITION);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(region, &obligation), ASX_OK);
    ASSERT_EQ(asx_obligation_commit(obligation), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_obligation_abort(obligation), ASX_E_INVALID_TRANSITION);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve(region, &obligation), ASX_OK);
    ASSERT_EQ(asx_obligation_abort(obligation), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_obligation_commit(obligation), ASX_E_INVALID_TRANSITION);
    ASSERT_EQ(asx_obligation_abort(obligation), ASX_E_INVALID_TRANSITION);
    guard_assert_no_semantic_mutation(&guard);
}

TEST(channel_misuse_failures_are_exact_and_non_emitting) {
    misuse_guard guard;
    asx_region_id region;
    asx_channel_id channel;
    asx_send_permit permit;
    uint64_t value;

    reset_all();
    ASSERT_EQ(asx_channel_try_reserve(ASX_INVALID_ID, &permit), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_channel_try_recv(ASX_INVALID_ID, &value), ASX_E_INVALID_ARGUMENT);
    guard_capture(&guard);
    ASSERT_EQ(asx_channel_try_reserve(ASX_INVALID_ID, &permit), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_channel_try_recv(ASX_INVALID_ID, &value), ASX_E_INVALID_ARGUMENT);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_channel_create(region, 1u, &channel), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(channel), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_channel_try_reserve(channel, &permit), ASX_E_INVALID_STATE);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_channel_create(region, 1u, &channel), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(channel), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_channel_try_reserve(channel, &permit), ASX_E_DISCONNECTED);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_channel_create(region, 1u, &channel), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(channel, &permit), ASX_OK);
    ASSERT_EQ(asx_send_permit_send(&permit, 42u), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_send_permit_send(&permit, 43u), ASX_E_INVALID_STATE);
    guard_assert_no_semantic_mutation(&guard);
}

TEST(deadline_and_cancellation_misuse_are_exact_and_non_emitting) {
    misuse_guard guard;
    asx_deadline deadline;
    asx_region_id region;
    asx_task_id task;
    asx_checkpoint_result checkpoint;
    asx_cancel_phase phase;

    reset_all();
    guard_capture(&guard);
    ASSERT_EQ(asx_deadline_init(NULL, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_deadline_after(NULL, 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_deadline_arm(NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_deadline_disarm(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_FALSE(asx_deadline_is_expired(NULL));
    ASSERT_FALSE(asx_deadline_is_expired_at(NULL, 1u));
    ASSERT_EQ(asx_deadline_remaining_ns(NULL, 1u), (uint64_t)0u);
    ASSERT_EQ(asx_deadline_target(NULL), (asx_time)0u);
    ASSERT_FALSE(asx_deadline_is_armed(NULL));
    ASSERT_EQ(asx_task_cancel(ASX_INVALID_ID, ASX_CANCEL_USER), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_checkpoint(ASX_INVALID_ID, &checkpoint), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_get_cancel_phase(ASX_INVALID_ID, &phase), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_checkpoint(ASX_INVALID_ID, NULL), ASX_E_INVALID_ARGUMENT);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_deadline_init(&deadline, 10u), ASX_OK);
    ASSERT_EQ(asx_deadline_arm(&deadline, NULL), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_deadline_arm(&deadline, NULL), ASX_E_INVALID_STATE);
    guard_assert_no_semantic_mutation(&guard);

    reset_all();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, poll_pending, NULL, &task), ASX_OK);
    guard_capture(&guard);
    ASSERT_EQ(asx_checkpoint(task, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_get_cancel_phase(task, NULL), ASX_E_INVALID_ARGUMENT);
    guard_assert_no_semantic_mutation(&guard);
}

int main(void) {
    fprintf(stderr, "=== public_api_misuse_conformance_test ===\n");

    RUN_TEST(unknown_and_wrong_type_handles_fail_without_events);
    RUN_TEST(stale_region_handles_fail_without_events);
    RUN_TEST(double_close_and_finalize_fail_without_events);
    RUN_TEST(obligation_illegal_transitions_fail_without_events);
    RUN_TEST(channel_misuse_failures_are_exact_and_non_emitting);
    RUN_TEST(deadline_and_cancellation_misuse_are_exact_and_non_emitting);

    TEST_REPORT();
    return test_failures;
}
