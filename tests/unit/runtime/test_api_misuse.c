/*
 * test_api_misuse.c — API misuse catalog regression tests (bd-1md.19)
 *
 * Supplements test_safety_posture.c by covering additional misuse modes
 * identified in docs/API_MISUSE_CATALOG.md. Focus on:
 *   - Channel misuse (creation, close, reserve, recv boundary cases)
 *   - Cancel/checkpoint with invalid handles
 *   - Scheduler with invalid region
 *   - Region drain misuse
 *   - Resource exhaustion boundaries
 *   - Transition check boundary values
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/core/cancel.h>
#include <asx/core/channel.h>
#include <asx/core/outcome.h>
#include <asx/runtime/runtime.h>

/* Suppress warn_unused_result for intentionally-ignored calls. */
#define IGNORE(expr)                                                                               \
    do {                                                                                           \
        asx_status s_ = (expr);                                                                    \
        (void)s_;                                                                                  \
    } while (0)

/* Simple poll: yields forever */
static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}


/* ===================================================================
 * Channel creation misuse
 * =================================================================== */

TEST(channel_create_invalid_region) {
    asx_channel_id cid;
    asx_runtime_reset();
    asx_channel_reset();

    /* Channel slot lookup returns INVALID_ARGUMENT for bad handles */
    ASSERT_EQ(asx_channel_create(ASX_INVALID_ID, 16, &cid), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_create_zero_capacity) {
    asx_region_id rid;
    asx_channel_id cid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 0, &cid), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_create_null_output) {
    asx_region_id rid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_create_exceeds_max_capacity) {
    asx_region_id rid;
    asx_channel_id cid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, ASX_CHANNEL_MAX_CAPACITY + 1, &cid), ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Channel close misuse
 * =================================================================== */

TEST(channel_double_close_sender) {
    asx_region_id rid;
    asx_channel_id cid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);

    ASSERT_EQ(asx_channel_close_sender(cid), ASX_OK);
    /* Channel uses INVALID_STATE for already-closed transitions */
    ASSERT_EQ(asx_channel_close_sender(cid), ASX_E_INVALID_STATE);
}

TEST(channel_double_close_receiver) {
    asx_region_id rid;
    asx_channel_id cid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);

    ASSERT_EQ(asx_channel_close_receiver(cid), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(cid), ASX_E_INVALID_STATE);
}

TEST(channel_close_sender_invalid_handle) {
    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_channel_close_sender(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_close_receiver_invalid_handle) {
    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_channel_close_receiver(ASX_INVALID_ID), ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Channel send/receive misuse
 * =================================================================== */

TEST(channel_reserve_after_sender_close) {
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(cid), ASX_OK);

    /* Reserve after sender close → invalid state */
    ASSERT_EQ(asx_channel_try_reserve(cid, &permit), ASX_E_INVALID_STATE);
}

TEST(channel_reserve_after_receiver_close) {
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permit;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);
    ASSERT_EQ(asx_channel_close_receiver(cid), ASX_OK);

    /* Reserve after receiver close → disconnected */
    ASSERT_EQ(asx_channel_try_reserve(cid, &permit), ASX_E_DISCONNECTED);
}

TEST(channel_recv_invalid_handle) {
    uint64_t val;
    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_channel_try_recv(ASX_INVALID_ID, &val), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_recv_empty_open) {
    asx_region_id rid;
    asx_channel_id cid;
    uint64_t val;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);

    /* Recv on empty open channel → would block */
    ASSERT_EQ(asx_channel_try_recv(cid, &val), ASX_E_WOULD_BLOCK);
}

TEST(channel_recv_empty_sender_closed) {
    asx_region_id rid;
    asx_channel_id cid;
    uint64_t val;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);
    ASSERT_EQ(asx_channel_close_sender(cid), ASX_OK);

    /* Recv on empty channel with sender closed → disconnected */
    ASSERT_EQ(asx_channel_try_recv(cid, &val), ASX_E_DISCONNECTED);
}

TEST(channel_get_state_null) {
    asx_region_id rid;
    asx_channel_id cid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);

    ASSERT_EQ(asx_channel_get_state(cid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_queue_len_null) {
    asx_region_id rid;
    asx_channel_id cid;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_channel_create(rid, 16, &cid), ASX_OK);

    ASSERT_EQ(asx_channel_queue_len(cid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_reserve_invalid_handle) {
    asx_send_permit permit;
    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_channel_try_reserve(ASX_INVALID_ID, &permit), ASX_E_INVALID_ARGUMENT);
}

TEST(channel_capacity_exhaustion) {
    asx_region_id rid;
    asx_channel_id cid;
    asx_send_permit permits[3];
    asx_send_permit overflow;

    asx_runtime_reset();
    asx_channel_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* Capacity 2: can reserve 2, third fails */
    ASSERT_EQ(asx_channel_create(rid, 2, &cid), ASX_OK);

    ASSERT_EQ(asx_channel_try_reserve(cid, &permits[0]), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(cid, &permits[1]), ASX_OK);
    ASSERT_EQ(asx_channel_try_reserve(cid, &overflow), ASX_E_CHANNEL_FULL);

    /* Abort one permit, then reserve should succeed again */
    asx_send_permit_abort(&permits[0]);
    ASSERT_EQ(asx_channel_try_reserve(cid, &permits[2]), ASX_OK);
}

/* ===================================================================
 * Cancel/checkpoint misuse with invalid handles
 * =================================================================== */

TEST(cancel_invalid_handle) {
    asx_runtime_reset();
    ASSERT_EQ(asx_task_cancel(ASX_INVALID_ID, ASX_CANCEL_USER), ASX_E_NOT_FOUND);
}

TEST(cancel_with_origin_invalid_handle) {
    asx_runtime_reset();
    ASSERT_EQ(asx_task_cancel_with_origin(ASX_INVALID_ID, ASX_CANCEL_USER, ASX_INVALID_ID,
                                          ASX_INVALID_ID),
              ASX_E_NOT_FOUND);
}

TEST(checkpoint_invalid_handle) {
    asx_checkpoint_result cr;
    asx_runtime_reset();
    ASSERT_EQ(asx_checkpoint(ASX_INVALID_ID, &cr), ASX_E_NOT_FOUND);
}

TEST(finalize_invalid_handle) {
    asx_runtime_reset();
    ASSERT_EQ(asx_task_finalize(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

TEST(cancel_phase_invalid_handle) {
    asx_cancel_phase phase;
    asx_runtime_reset();
    ASSERT_EQ(asx_task_get_cancel_phase(ASX_INVALID_ID, &phase), ASX_E_NOT_FOUND);
}

/* ===================================================================
 * Scheduler misuse
 * =================================================================== */

TEST(scheduler_invalid_region) {
    asx_budget budget = asx_budget_from_polls(10);
    asx_runtime_reset();
    ASSERT_EQ(asx_scheduler_run(ASX_INVALID_ID, &budget), ASX_E_NOT_FOUND);
}

/* ===================================================================
 * Region drain misuse
 * =================================================================== */

TEST(drain_invalid_region) {
    asx_budget budget = asx_budget_from_polls(10);
    asx_runtime_reset();
    ASSERT_EQ(asx_region_drain(ASX_INVALID_ID, &budget), ASX_E_NOT_FOUND);
}

TEST(drain_null_budget) {
    asx_region_id rid;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_drain(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ===================================================================
 * Transition boundary checks — out-of-range values
 * =================================================================== */

TEST(region_transition_out_of_range_returns_error) {
    /* Both from and to are out of valid enum range */
    ASSERT_NE(asx_region_transition_check(99, 0), ASX_OK);
    ASSERT_NE(asx_region_transition_check(0, 99), ASX_OK);
}

TEST(task_transition_out_of_range_returns_error) {
    ASSERT_NE(asx_task_transition_check(99, 0), ASX_OK);
    ASSERT_NE(asx_task_transition_check(0, 99), ASX_OK);
}

TEST(obligation_transition_out_of_range_returns_error) {
    ASSERT_NE(asx_obligation_transition_check(99, 0), ASX_OK);
    ASSERT_NE(asx_obligation_transition_check(0, 99), ASX_OK);
}

/* ===================================================================
 * Task outcome misuse before completion
 * =================================================================== */

TEST(outcome_before_completion_rejected) {
    asx_region_id rid;
    asx_task_id tid;
    asx_outcome out;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Before any poll — task is Created */
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_E_TASK_NOT_COMPLETED);

    /* After one poll — task is Running */
    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));
    ASSERT_EQ(asx_task_get_outcome(tid, &out), ASX_E_TASK_NOT_COMPLETED);
}

/* ===================================================================
 * Resource exhaustion: spawn at task arena capacity
 * =================================================================== */

TEST(task_arena_exhaustion) {
    asx_region_id rid;
    asx_task_id tids[ASX_MAX_TASKS];
    asx_task_id overflow;
    uint32_t i;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    for (i = 0; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tids[i]), ASX_OK);
    }

    /* One more should fail */
    ASSERT_NE(asx_task_spawn(rid, poll_pending, NULL, &overflow), ASX_OK);
}

TEST(region_arena_exhaustion) {
    asx_region_id rids[ASX_MAX_REGIONS];
    asx_region_id overflow;
    uint32_t i;

    asx_runtime_reset();

    for (i = 0; i < ASX_MAX_REGIONS; i++) { ASSERT_EQ(asx_region_open(&rids[i]), ASX_OK); }

    /* One more should fail */
    ASSERT_NE(asx_region_open(&overflow), ASX_OK);
}

/* ===================================================================
 * Cancel strengthening: equal severity, earlier timestamp wins
 * =================================================================== */

TEST(cancel_strengthen_equal_severity) {
    asx_cancel_reason a, b, result;

    a.kind = ASX_CANCEL_TIMEOUT; /* severity 1 */
    a.origin_region = ASX_INVALID_ID;
    a.origin_task = ASX_INVALID_ID;
    a.timestamp = 100;
    a.message = NULL;
    a.cause = NULL;
    a.truncated = 0;

    b.kind = ASX_CANCEL_DEADLINE; /* severity 1 */
    b.origin_region = ASX_INVALID_ID;
    b.origin_task = ASX_INVALID_ID;
    b.timestamp = 200;
    b.message = NULL;
    b.cause = NULL;
    b.truncated = 0;

    result = asx_cancel_strengthen(&a, &b);
    /* Equal severity → earlier timestamp wins */
    ASSERT_EQ((int)result.kind, (int)ASX_CANCEL_TIMEOUT);
    ASSERT_EQ((uint64_t)result.timestamp, (uint64_t)100);
}

/* ===================================================================
 * Quiescence check misuse
 * =================================================================== */

TEST(quiescence_check_invalid_region) {
    asx_runtime_reset();
    ASSERT_EQ(asx_quiescence_check(ASX_INVALID_ID), ASX_E_NOT_FOUND);
}

TEST(quiescence_fails_before_drain) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);

    /* Run one poll (task is Running but not done) */
    budget = asx_budget_from_polls(1);
    IGNORE(asx_scheduler_run(rid, &budget));

    /* Quiescence should fail — tasks still active */
    ASSERT_NE(asx_quiescence_check(rid), ASX_OK);
}

/* ===================================================================
 * Error determinism: same misuse produces same error code
 * =================================================================== */

TEST(misuse_errors_are_deterministic) {
    asx_task_id tid_out;
    asx_status s1, s2;

    asx_runtime_reset();

    /* Same invalid operation → same error code both times */
    s1 = asx_task_spawn(ASX_INVALID_ID, poll_pending, NULL, &tid_out);
    s2 = asx_task_spawn(ASX_INVALID_ID, poll_pending, NULL, &tid_out);
    ASSERT_EQ(s1, s2);

    s1 = asx_task_cancel(ASX_INVALID_ID, ASX_CANCEL_USER);
    s2 = asx_task_cancel(ASX_INVALID_ID, ASX_CANCEL_USER);
    ASSERT_EQ(s1, s2);

    s1 = asx_region_close(ASX_INVALID_ID);
    s2 = asx_region_close(ASX_INVALID_ID);
    ASSERT_EQ(s1, s2);
}

/* ===================================================================
 * Main
 * =================================================================== */

int main(void) {
    fprintf(stderr, "=== test_api_misuse (bd-1md.19) ===\n");

    /* Channel creation misuse */
    RUN_TEST(channel_create_invalid_region);
    RUN_TEST(channel_create_zero_capacity);
    RUN_TEST(channel_create_null_output);
    RUN_TEST(channel_create_exceeds_max_capacity);

    /* Channel close misuse */
    RUN_TEST(channel_double_close_sender);
    RUN_TEST(channel_double_close_receiver);
    RUN_TEST(channel_close_sender_invalid_handle);
    RUN_TEST(channel_close_receiver_invalid_handle);

    /* Channel send/receive misuse */
    RUN_TEST(channel_reserve_after_sender_close);
    RUN_TEST(channel_reserve_after_receiver_close);
    RUN_TEST(channel_recv_invalid_handle);
    RUN_TEST(channel_recv_empty_open);
    RUN_TEST(channel_recv_empty_sender_closed);
    RUN_TEST(channel_get_state_null);
    RUN_TEST(channel_queue_len_null);
    RUN_TEST(channel_reserve_invalid_handle);
    RUN_TEST(channel_capacity_exhaustion);

    /* Cancel/checkpoint invalid handles */
    RUN_TEST(cancel_invalid_handle);
    RUN_TEST(cancel_with_origin_invalid_handle);
    RUN_TEST(checkpoint_invalid_handle);
    RUN_TEST(finalize_invalid_handle);
    RUN_TEST(cancel_phase_invalid_handle);

    /* Scheduler misuse */
    RUN_TEST(scheduler_invalid_region);

    /* Region drain misuse */
    RUN_TEST(drain_invalid_region);
    RUN_TEST(drain_null_budget);

    /* Transition boundary values */
    RUN_TEST(region_transition_out_of_range_returns_error);
    RUN_TEST(task_transition_out_of_range_returns_error);
    RUN_TEST(obligation_transition_out_of_range_returns_error);

    /* Task outcome misuse */
    RUN_TEST(outcome_before_completion_rejected);

    /* Resource exhaustion */
    RUN_TEST(task_arena_exhaustion);
    RUN_TEST(region_arena_exhaustion);

    /* Cancel algebra */
    RUN_TEST(cancel_strengthen_equal_severity);

    /* Quiescence misuse */
    RUN_TEST(quiescence_check_invalid_region);
    RUN_TEST(quiescence_fails_before_drain);

    /* Error determinism */
    RUN_TEST(misuse_errors_are_deterministic);

    TEST_REPORT();
    return test_failures;
}
