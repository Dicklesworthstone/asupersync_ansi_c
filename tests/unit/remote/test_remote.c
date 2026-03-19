/*
 * test_remote.c — unit tests for remote execution primitives
 *
 * Tests remote capability, handle lifecycle, idempotency, saga,
 * lease, and spawn request/ack protocol.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/remote/remote.h>
#include <string.h>

/* ================================================================== */
/* Remote capability tests                                             */
/* ================================================================== */

TEST(cap_init_and_flags) {
    asx_remote_cap cap;
    asx_remote_cap_init(&cap, 1, ASX_REMOTE_CAP_SPAWN | ASX_REMOTE_CAP_CANCEL);

    ASSERT_TRUE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_SPAWN));
    ASSERT_TRUE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_CANCEL));
    ASSERT_FALSE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_QUERY));
    ASSERT_FALSE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_LEASE));
}

TEST(cap_all_flags) {
    asx_remote_cap cap;
    asx_remote_cap_init(&cap, 2, ASX_REMOTE_CAP_ALL);

    ASSERT_TRUE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_SPAWN));
    ASSERT_TRUE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_CANCEL));
    ASSERT_TRUE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_QUERY));
    ASSERT_TRUE(asx_remote_cap_allows(&cap, ASX_REMOTE_CAP_LEASE));
}

/* ================================================================== */
/* Remote handle lifecycle tests                                       */
/* ================================================================== */

TEST(handle_init_pending) {
    asx_remote_handle h;
    asx_remote_handle_init(&h, 42);

    ASSERT_EQ(h.status, ASX_REMOTE_PENDING);
    ASSERT_FALSE(asx_remote_handle_is_terminal(&h));
}

TEST(handle_transition_pending_to_running) {
    asx_remote_handle h;
    asx_remote_handle_init(&h, 1);
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_RUNNING), ASX_OK);
    ASSERT_EQ(h.status, ASX_REMOTE_RUNNING);
}

TEST(handle_transition_running_to_completed) {
    asx_remote_handle h;
    asx_remote_handle_init(&h, 1);
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_RUNNING), ASX_OK);
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_COMPLETED), ASX_OK);
    ASSERT_TRUE(asx_remote_handle_is_terminal(&h));
}

TEST(handle_illegal_transition) {
    asx_remote_handle h;
    asx_remote_handle_init(&h, 1);
    /* PENDING -> COMPLETED is illegal (must go through RUNNING). */
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_COMPLETED), ASX_E_INVALID_TRANSITION);
}

TEST(handle_complete_with_outcome) {
    asx_remote_handle h;
    asx_outcome outcome;
    asx_remote_handle_init(&h, 1);
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_RUNNING), ASX_OK);

    memset(&outcome, 0, sizeof(outcome));
    ASSERT_EQ(asx_remote_handle_complete(&h, outcome), ASX_OK);
    ASSERT_TRUE(asx_remote_handle_is_terminal(&h));
}

TEST(handle_terminal_cannot_transition) {
    asx_remote_handle h;
    asx_remote_handle_init(&h, 1);
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_CANCELLED), ASX_OK);
    ASSERT_EQ(asx_remote_handle_transition(&h, ASX_REMOTE_RUNNING), ASX_E_INVALID_TRANSITION);
}

/* ================================================================== */
/* Remote message tests                                                */
/* ================================================================== */

TEST(message_init_and_payload) {
    asx_remote_message msg;
    const char *data = "hello";

    asx_remote_message_init(&msg, ASX_REMOTE_MSG_SPAWN, 99);
    ASSERT_EQ(msg.kind, ASX_REMOTE_MSG_SPAWN);
    ASSERT_EQ(msg.correlation_id, 99u);

    ASSERT_EQ(asx_remote_message_set_payload(&msg, data, 5), ASX_OK);
    ASSERT_EQ(msg.payload_len, 5u);
    ASSERT_TRUE(memcmp(msg.payload, "hello", 5) == 0);
}

TEST(message_payload_too_large) {
    asx_remote_message msg;
    uint8_t big[ASX_REMOTE_MSG_PAYLOAD_SIZE + 1];

    asx_remote_message_init(&msg, ASX_REMOTE_MSG_RESULT, 1);
    ASSERT_EQ(asx_remote_message_set_payload(&msg, big, sizeof(big)), ASX_E_BUFFER_TOO_SMALL);
}

/* ================================================================== */
/* Idempotency tests                                                   */
/* ================================================================== */

TEST(idempotency_key_equality) {
    asx_idempotency_key a, b, c;
    asx_idempotency_key_init(&a, 123, 1);
    asx_idempotency_key_init(&b, 123, 1);
    asx_idempotency_key_init(&c, 456, 1);

    ASSERT_TRUE(asx_idempotency_key_eq(&a, &b));
    ASSERT_FALSE(asx_idempotency_key_eq(&a, &c));
}

TEST(idempotency_record_lifecycle) {
    asx_idempotency_key key;
    asx_idempotency_record rec;

    asx_idempotency_key_init(&key, 100, 0);
    asx_idempotency_record_init(&rec, key, 5000);

    ASSERT_EQ(rec.state, ASX_IDEMPOTENCY_PENDING);
    ASSERT_EQ(asx_idempotency_record_commit(&rec, ASX_OK), ASX_OK);
    ASSERT_EQ(rec.state, ASX_IDEMPOTENCY_COMMITTED);

    /* Double commit fails. */
    ASSERT_EQ(asx_idempotency_record_commit(&rec, ASX_OK), ASX_E_INVALID_TRANSITION);
}

TEST(idempotency_record_expiry) {
    asx_idempotency_key key;
    asx_idempotency_record rec;

    asx_idempotency_key_init(&key, 200, 0);
    asx_idempotency_record_init(&rec, key, 1000);
    rec.created_ns = 500;

    ASSERT_FALSE(asx_idempotency_record_expired(&rec, 1000));
    ASSERT_TRUE(asx_idempotency_record_expired(&rec, 1500));
}

TEST(idempotency_store_lookup_and_upsert) {
    asx_idempotency_store store;
    asx_idempotency_key key;
    asx_idempotency_record rec;
    const asx_idempotency_record *found;

    asx_idempotency_store_init(&store);

    asx_idempotency_key_init(&key, 42, 0);
    asx_idempotency_record_init(&rec, key, 10000);

    ASSERT_TRUE(asx_idempotency_store_lookup(&store, &key) == NULL);

    ASSERT_EQ(asx_idempotency_store_upsert(&store, &rec), ASX_OK);
    found = asx_idempotency_store_lookup(&store, &key);
    ASSERT_TRUE(found != NULL);
    ASSERT_EQ(found->key.key_hash, 42u);
}

TEST(idempotency_store_eviction) {
    asx_idempotency_store store;
    asx_idempotency_key key;
    asx_idempotency_record rec;
    uint32_t evicted;

    asx_idempotency_store_init(&store);

    asx_idempotency_key_init(&key, 1, 0);
    asx_idempotency_record_init(&rec, key, 100);
    rec.created_ns = 0;
    ASSERT_EQ(asx_idempotency_store_upsert(&store, &rec), ASX_OK);

    evicted = asx_idempotency_store_evict_expired(&store, 50);
    ASSERT_EQ(evicted, 0u);

    evicted = asx_idempotency_store_evict_expired(&store, 200);
    ASSERT_EQ(evicted, 1u);
    ASSERT_TRUE(asx_idempotency_store_lookup(&store, &key) == NULL);
}

/* ================================================================== */
/* Saga tests                                                          */
/* ================================================================== */

static int step_counter = 0;
static int compensate_counter = 0;

static asx_status saga_step_ok(void *user_data) {
    (void)user_data;
    step_counter++;
    return ASX_OK;
}

static asx_status saga_step_fail(void *user_data) {
    (void)user_data;
    step_counter++;
    return ASX_E_DISCONNECTED;
}

static asx_status saga_compensate_fn(void *user_data) {
    (void)user_data;
    compensate_counter++;
    return ASX_OK;
}

TEST(saga_all_steps_succeed) {
    asx_saga saga;
    step_counter = 0;

    asx_saga_init(&saga);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, saga_compensate_fn, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, saga_compensate_fn, NULL), ASX_OK);

    ASSERT_EQ(asx_saga_execute(&saga), ASX_OK);
    ASSERT_EQ(step_counter, 2);
    ASSERT_EQ(asx_saga_get_state(&saga), ASX_SAGA_COMPLETED);
}

TEST(saga_failure_triggers_compensation) {
    asx_saga saga;
    step_counter = 0;
    compensate_counter = 0;

    asx_saga_init(&saga);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, saga_compensate_fn, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_fail, saga_compensate_fn, NULL), ASX_OK);

    ASSERT_EQ(asx_saga_execute(&saga), ASX_E_DISCONNECTED);
    ASSERT_EQ(step_counter, 2);
    ASSERT_EQ(compensate_counter, 1); /* only step 0 completed, so only it is compensated */
    ASSERT_EQ(asx_saga_get_state(&saga), ASX_SAGA_FAILED);
}

TEST(saga_step_count) {
    asx_saga saga;
    asx_saga_init(&saga);

    ASSERT_EQ(asx_saga_step_count(&saga), 0u);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, NULL, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_step_count(&saga), 1u);
}

TEST(saga_rejects_null_execute) {
    asx_saga saga;
    asx_saga_init(&saga);

    ASSERT_EQ(asx_saga_add_step(&saga, NULL, saga_compensate_fn, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_saga_step_count(&saga), 0u);
}

/* ================================================================== */
/* Lease tests                                                         */
/* ================================================================== */

TEST(lease_init_active) {
    asx_lease lease;
    asx_lease_init(&lease, 1, 10, 5000);

    ASSERT_TRUE(asx_lease_is_active(&lease, 0));
    ASSERT_TRUE(asx_lease_is_active(&lease, 4999));
}

TEST(lease_expires) {
    asx_lease lease;
    asx_lease_init(&lease, 1, 10, 5000);
    lease.granted_ns = 0;

    ASSERT_FALSE(asx_lease_is_active(&lease, 5000));
    ASSERT_FALSE(asx_lease_is_active(&lease, 10000));
}

TEST(lease_renew) {
    asx_lease lease;
    asx_lease_init(&lease, 1, 10, 1000);
    lease.granted_ns = 0;

    ASSERT_EQ(asx_lease_renew(&lease, 500, 2000), ASX_OK);
    ASSERT_TRUE(asx_lease_is_active(&lease, 2000));
}

TEST(lease_revoke) {
    asx_lease lease;
    asx_lease_init(&lease, 1, 10, 5000);
    asx_lease_revoke(&lease);
    ASSERT_FALSE(asx_lease_is_active(&lease, 0));
}

TEST(lease_fence_validation) {
    asx_lease lease;
    asx_lease_init(&lease, 1, 10, 5000);

    ASSERT_TRUE(asx_lease_validate_fence(&lease, 10));
    ASSERT_TRUE(asx_lease_validate_fence(&lease, 15));
    ASSERT_FALSE(asx_lease_validate_fence(&lease, 5));
}

/* ================================================================== */
/* Spawn request/ack tests                                             */
/* ================================================================== */

TEST(spawn_request_init) {
    asx_spawn_request req;
    asx_remote_cap cap;
    asx_idempotency_key key;

    asx_remote_cap_init(&cap, 1, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 999, 0);
    asx_spawn_request_init(&req, 42, cap, key);

    ASSERT_EQ(req.request_id, 42u);
    ASSERT_TRUE(asx_remote_cap_allows(&req.cap, ASX_REMOTE_CAP_SPAWN));
}

TEST(spawn_ack_accepted) {
    asx_spawn_ack ack;
    asx_spawn_ack_accepted(&ack, 42, 100);

    ASSERT_EQ(ack.request_id, 42u);
    ASSERT_EQ(ack.status, ASX_SPAWN_ACK_ACCEPTED);
    ASSERT_EQ(ack.handle_id, 100u);
}

TEST(spawn_ack_rejected) {
    asx_spawn_ack ack;
    asx_spawn_ack_rejected(&ack, 42, ASX_SPAWN_REJECT_UNAUTHORIZED);

    ASSERT_EQ(ack.request_id, 42u);
    ASSERT_EQ(ack.status, ASX_SPAWN_ACK_REJECTED);
    ASSERT_EQ(ack.reject_reason, ASX_SPAWN_REJECT_UNAUTHORIZED);
}

TEST(spawn_ack_queued) {
    asx_spawn_ack ack;
    memset(&ack, 0xFF, sizeof(ack));

    asx_spawn_ack_queued(&ack, 88);

    ASSERT_EQ(ack.request_id, 88u);
    ASSERT_EQ(ack.status, ASX_SPAWN_ACK_QUEUED);
    ASSERT_EQ(ack.handle_id, 0u);
}

TEST(spawn_ack_helpers_clear_stale_fields) {
    asx_spawn_ack ack;
    memset(&ack, 0xFF, sizeof(ack));

    asx_spawn_ack_accepted(&ack, 42, 100);
    ASSERT_EQ(ack.request_id, 42u);
    ASSERT_EQ(ack.status, ASX_SPAWN_ACK_ACCEPTED);
    ASSERT_EQ(ack.handle_id, 100u);

    memset(&ack, 0xFF, sizeof(ack));
    asx_spawn_ack_rejected(&ack, 77, ASX_SPAWN_REJECT_DUPLICATE);
    ASSERT_EQ(ack.request_id, 77u);
    ASSERT_EQ(ack.status, ASX_SPAWN_ACK_REJECTED);
    ASSERT_EQ(ack.reject_reason, ASX_SPAWN_REJECT_DUPLICATE);
    ASSERT_EQ(ack.handle_id, 0u);
}

/* ================================================================== */
/* Operator report tests                                               */
/* ================================================================== */

TEST(remote_report_summarizes_active_execution) {
    asx_spawn_request req;
    asx_spawn_ack ack;
    asx_remote_handle handle;
    asx_idempotency_key key;
    asx_idempotency_record rec;
    asx_lease lease;
    asx_saga saga;
    asx_remote_cap cap;
    asx_remote_report report;
    char text[ASX_REMOTE_REPORT_TEXT_SIZE];

    asx_remote_cap_init(&cap, 7, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 555, 2);
    asx_spawn_request_init(&req, 42, cap, key);
    asx_spawn_ack_accepted(&ack, 42, 9001);
    asx_remote_handle_init(&handle, 9001);
    ASSERT_EQ(asx_remote_handle_transition(&handle, ASX_REMOTE_RUNNING), ASX_OK);
    asx_idempotency_record_init(&rec, key, 5000);
    ASSERT_EQ(asx_idempotency_record_commit(&rec, ASX_OK), ASX_OK);
    asx_lease_init(&lease, 99, 12, 5000);
    lease.granted_ns = 100;
    asx_saga_init(&saga);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, saga_compensate_fn, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, saga_compensate_fn, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_execute(&saga), ASX_OK);

    asx_remote_report_init(&report, &req, &ack, &handle, &rec, &lease, &saga, 200);

    ASSERT_EQ(report.request_id, 42u);
    ASSERT_EQ(report.handle_id, 9001u);
    ASSERT_TRUE(report.ack_present != 0u);
    ASSERT_TRUE(report.handle_present != 0u);
    ASSERT_TRUE(report.saga_present != 0u);
    ASSERT_TRUE(report.lease_present != 0u);
    ASSERT_EQ(report.idempotency_hash, 555u);
    ASSERT_EQ(report.completed_steps, 2u);
    ASSERT_EQ(report.total_steps, 2u);
    ASSERT_TRUE(report.idempotency_committed != 0u);
    ASSERT_TRUE(report.lease_active != 0u);
    ASSERT_TRUE(report.handle_terminal == 0u);
    ASSERT_EQ(asx_remote_report_format(&report, text, sizeof(text)), ASX_OK);
    ASSERT_TRUE(strstr(text, "req=42") != NULL);
    ASSERT_TRUE(strstr(text, "ack=accepted") != NULL);
    ASSERT_TRUE(strstr(text, "remote=running") != NULL);
    ASSERT_TRUE(strstr(text, "steps=2/2") != NULL);
}

TEST(remote_report_uses_failure_sources_and_bounds_checks) {
    asx_spawn_request req;
    asx_spawn_ack ack;
    asx_remote_handle handle;
    asx_idempotency_key key;
    asx_idempotency_record rec;
    asx_lease lease;
    asx_saga saga;
    asx_remote_cap cap;
    asx_remote_report report;
    char tiny[8];

    asx_remote_cap_init(&cap, 9, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 777, 3);
    asx_spawn_request_init(&req, 77, cap, key);
    asx_spawn_ack_rejected(&ack, 77, ASX_SPAWN_REJECT_CAPACITY);
    asx_remote_handle_init(&handle, 1001);
    ASSERT_EQ(asx_remote_handle_transition(&handle, ASX_REMOTE_RUNNING), ASX_OK);
    ASSERT_EQ(asx_remote_handle_transition(&handle, ASX_REMOTE_FAILED), ASX_OK);
    asx_idempotency_record_init(&rec, key, 100);
    asx_lease_init(&lease, 5, 1, 10);
    lease.granted_ns = 0;
    asx_saga_init(&saga);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_ok, saga_compensate_fn, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_add_step(&saga, saga_step_fail, saga_compensate_fn, NULL), ASX_OK);
    ASSERT_EQ(asx_saga_execute(&saga), ASX_E_DISCONNECTED);

    asx_remote_report_init(&report, &req, &ack, &handle, &rec, &lease, &saga, 1000);

    ASSERT_EQ(report.ack_status, ASX_SPAWN_ACK_REJECTED);
    ASSERT_EQ(report.reject_reason, ASX_SPAWN_REJECT_CAPACITY);
    ASSERT_EQ(report.final_status, ASX_E_DISCONNECTED);
    ASSERT_TRUE(report.handle_terminal != 0u);
    ASSERT_TRUE(report.lease_active == 0u);
    ASSERT_EQ(asx_remote_report_format(&report, tiny, sizeof(tiny)), ASX_E_BUFFER_TOO_SMALL);
}

TEST(remote_report_init_allows_null_report) {
    asx_spawn_request req;
    asx_spawn_ack ack;
    asx_remote_cap cap;
    asx_idempotency_key key;

    asx_remote_cap_init(&cap, 1, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 1234, 0);
    asx_spawn_request_init(&req, 11, cap, key);
    asx_spawn_ack_accepted(&ack, 11, 22);

    asx_remote_report_init(NULL, &req, &ack, NULL, NULL, NULL, NULL, 0);
}

TEST(remote_report_rejected_request_is_not_reported_as_success) {
    asx_spawn_request req;
    asx_spawn_ack ack;
    asx_remote_cap cap;
    asx_idempotency_key key;
    asx_remote_report report;

    asx_remote_cap_init(&cap, 3, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 9999, 4);
    asx_spawn_request_init(&req, 88, cap, key);
    asx_spawn_ack_rejected(&ack, 88, ASX_SPAWN_REJECT_UNAUTHORIZED);

    asx_remote_report_init(&report, &req, &ack, NULL, NULL, NULL, NULL, 0);

    ASSERT_EQ(report.request_id, 88u);
    ASSERT_EQ(report.ack_status, ASX_SPAWN_ACK_REJECTED);
    ASSERT_EQ(report.final_status, ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(report.handle_terminal == 0u);
}

TEST(remote_report_active_execution_stays_pending) {
    asx_spawn_request req;
    asx_spawn_ack ack;
    asx_remote_handle handle;
    asx_remote_cap cap;
    asx_idempotency_key key;
    asx_remote_report report;

    asx_remote_cap_init(&cap, 5, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 12345, 1);
    asx_spawn_request_init(&req, 55, cap, key);
    asx_spawn_ack_accepted(&ack, 55, 501);
    asx_remote_handle_init(&handle, 501);
    ASSERT_EQ(asx_remote_handle_transition(&handle, ASX_REMOTE_RUNNING), ASX_OK);

    asx_remote_report_init(&report, &req, &ack, &handle, NULL, NULL, NULL, 0);

    ASSERT_EQ(report.remote_status, ASX_REMOTE_RUNNING);
    ASSERT_EQ(report.final_status, ASX_E_PENDING);
    ASSERT_TRUE(report.handle_terminal == 0u);
}

TEST(remote_report_queued_ack_stays_pending_without_handle) {
    asx_spawn_request req;
    asx_spawn_ack ack;
    asx_remote_cap cap;
    asx_idempotency_key key;
    asx_remote_report report;
    char text[ASX_REMOTE_REPORT_TEXT_SIZE];

    asx_remote_cap_init(&cap, 6, ASX_REMOTE_CAP_SPAWN);
    asx_idempotency_key_init(&key, 54321, 7);
    asx_spawn_request_init(&req, 66, cap, key);
    asx_spawn_ack_queued(&ack, 66);

    asx_remote_report_init(&report, &req, &ack, NULL, NULL, NULL, NULL, 0);

    ASSERT_TRUE(report.ack_present != 0u);
    ASSERT_TRUE(report.handle_present == 0u);
    ASSERT_EQ(report.ack_status, ASX_SPAWN_ACK_QUEUED);
    ASSERT_EQ(report.final_status, ASX_E_PENDING);
    ASSERT_EQ(asx_remote_report_format(&report, text, sizeof(text)), ASX_OK);
    ASSERT_TRUE(strstr(text, "ack=queued") != NULL);
    ASSERT_TRUE(strstr(text, "handle=na") != NULL);
    ASSERT_TRUE(strstr(text, "remote=na") != NULL);
}

TEST(remote_report_absent_components_render_as_na) {
    asx_remote_report report;
    char text[ASX_REMOTE_REPORT_TEXT_SIZE];

    memset(&report, 0, sizeof(report));
    report.request_id = 7u;
    report.final_status = ASX_E_PENDING;

    ASSERT_EQ(asx_remote_report_format(&report, text, sizeof(text)), ASX_OK);
    ASSERT_TRUE(strstr(text, "handle=na") != NULL);
    ASSERT_TRUE(strstr(text, "ack=na") != NULL);
    ASSERT_TRUE(strstr(text, "remote=na") != NULL);
    ASSERT_TRUE(strstr(text, "saga=na") != NULL);
    ASSERT_TRUE(strstr(text, "lease=na") != NULL);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== remote tests ===\n");

    /* Capability */
    RUN_TEST(cap_init_and_flags);
    RUN_TEST(cap_all_flags);

    /* Handle lifecycle */
    RUN_TEST(handle_init_pending);
    RUN_TEST(handle_transition_pending_to_running);
    RUN_TEST(handle_transition_running_to_completed);
    RUN_TEST(handle_illegal_transition);
    RUN_TEST(handle_complete_with_outcome);
    RUN_TEST(handle_terminal_cannot_transition);

    /* Message */
    RUN_TEST(message_init_and_payload);
    RUN_TEST(message_payload_too_large);

    /* Idempotency */
    RUN_TEST(idempotency_key_equality);
    RUN_TEST(idempotency_record_lifecycle);
    RUN_TEST(idempotency_record_expiry);
    RUN_TEST(idempotency_store_lookup_and_upsert);
    RUN_TEST(idempotency_store_eviction);

    /* Saga */
    RUN_TEST(saga_all_steps_succeed);
    RUN_TEST(saga_failure_triggers_compensation);
    RUN_TEST(saga_step_count);
    RUN_TEST(saga_rejects_null_execute);

    /* Lease */
    RUN_TEST(lease_init_active);
    RUN_TEST(lease_expires);
    RUN_TEST(lease_renew);
    RUN_TEST(lease_revoke);
    RUN_TEST(lease_fence_validation);

    /* Spawn */
    RUN_TEST(spawn_request_init);
    RUN_TEST(spawn_ack_accepted);
    RUN_TEST(spawn_ack_rejected);
    RUN_TEST(spawn_ack_queued);
    RUN_TEST(spawn_ack_helpers_clear_stale_fields);

    /* Operator report */
    RUN_TEST(remote_report_summarizes_active_execution);
    RUN_TEST(remote_report_uses_failure_sources_and_bounds_checks);
    RUN_TEST(remote_report_init_allows_null_report);
    RUN_TEST(remote_report_rejected_request_is_not_reported_as_success);
    RUN_TEST(remote_report_active_execution_stays_pending);
    RUN_TEST(remote_report_queued_ack_stays_pending_without_handle);
    RUN_TEST(remote_report_absent_components_render_as_na);

    TEST_REPORT();
    return test_failures;
}
