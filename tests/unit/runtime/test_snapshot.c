/*
 * test_snapshot.c — unit tests for asx/runtime/snapshot.h API
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/codec/codec.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/snapshot.h>
#include <string.h>

/* Dummy poll function for task spawn */
static asx_status dummy_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status failing_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_E_INVALID_STATE;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

TEST(snapshot_init_zeroes) {
    asx_runtime_snapshot snap;
    memset(&snap, 0xFF, sizeof(snap));
    asx_runtime_snapshot_init(&snap);
    ASSERT_EQ(snap.region_count, 0u);
    ASSERT_EQ(snap.task_count, 0u);
    ASSERT_EQ(snap.obligation_count, 0u);
    ASSERT_EQ(snap.event_hash, 0u);
}

TEST(snapshot_init_null_safe) {
    /* Should not crash */
    asx_runtime_snapshot_init(NULL);
}

/* ------------------------------------------------------------------ */
/* Capture empty runtime                                               */
/* ------------------------------------------------------------------ */

TEST(snapshot_capture_empty) {
    asx_runtime_snapshot snap;
    asx_status s;

    asx_runtime_reset();
    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(snap.region_count, 0u);
    ASSERT_EQ(snap.task_count, 0u);
    ASSERT_EQ(snap.obligation_count, 0u);
}

TEST(snapshot_capture_null_returns_error) {
    asx_status s = asx_runtime_snapshot_capture(NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Capture with live entities                                          */
/* ------------------------------------------------------------------ */

TEST(snapshot_capture_with_region) {
    asx_runtime_snapshot snap;
    asx_region_id rid;
    asx_status s;

    asx_runtime_reset();
    s = asx_region_open(&rid);
    ASSERT_EQ(s, ASX_OK);

    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(snap.region_count, 1u);
    ASSERT_EQ(snap.regions[0].state, ASX_REGION_OPEN);
    ASSERT_EQ(snap.regions[0].task_count, 0u);
    ASSERT_EQ(snap.regions[0].poisoned, 0);
}

TEST(snapshot_capture_with_task) {
    asx_runtime_snapshot snap;
    asx_region_id rid;
    asx_task_id tid;
    asx_status s;

    asx_runtime_reset();
    s = asx_region_open(&rid);
    ASSERT_EQ(s, ASX_OK);
    s = asx_task_spawn(rid, dummy_poll, NULL, &tid);
    ASSERT_EQ(s, ASX_OK);

    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(snap.region_count, 1u);
    ASSERT_EQ(snap.task_count, 1u);
    ASSERT_EQ(snap.regions[0].task_count, 1u);
    ASSERT_EQ(snap.tasks[0].outcome_severity, ASX_OUTCOME_OK);
}

TEST(snapshot_capture_completed_task_preserves_outcome_severity) {
    asx_runtime_snapshot snap;
    asx_budget budget;
    asx_region_id rid;
    asx_task_id tid;
    asx_status s;

    asx_runtime_reset();
    s = asx_region_open(&rid);
    ASSERT_EQ(s, ASX_OK);
    s = asx_task_spawn(rid, failing_poll, NULL, &tid);
    ASSERT_EQ(s, ASX_OK);

    budget = asx_budget_from_polls(4u);
    s = asx_scheduler_run(rid, &budget);
    ASSERT_EQ(s, ASX_E_INVALID_STATE);

    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(snap.task_count, 1u);
    ASSERT_EQ(snap.tasks[0].state, ASX_TASK_COMPLETED);
    ASSERT_EQ(snap.tasks[0].outcome_severity, ASX_OUTCOME_ERR);
}

TEST(snapshot_capture_with_obligation) {
    asx_runtime_snapshot snap;
    asx_region_id rid;
    asx_obligation_id oid;
    asx_status s;

    asx_runtime_reset();
    s = asx_region_open(&rid);
    ASSERT_EQ(s, ASX_OK);
    s = asx_obligation_reserve(rid, &oid);
    ASSERT_EQ(s, ASX_OK);

    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_EQ(snap.obligation_count, 1u);
    ASSERT_EQ(snap.obligations[0].state, ASX_OBLIGATION_RESERVED);
}

/* ------------------------------------------------------------------ */
/* Equality comparison                                                 */
/* ------------------------------------------------------------------ */

TEST(snapshot_eq_identical) {
    asx_runtime_snapshot a, b;
    asx_status s;

    asx_runtime_reset();
    s = asx_runtime_snapshot_capture(&a);
    ASSERT_EQ(s, ASX_OK);
    memcpy(&b, &a, sizeof(b));

    s = asx_runtime_snapshot_eq(&a, &b);
    ASSERT_EQ(s, ASX_OK);
}

TEST(snapshot_eq_different_counts) {
    asx_runtime_snapshot a, b;

    asx_runtime_snapshot_init(&a);
    asx_runtime_snapshot_init(&b);
    a.region_count = 1;
    b.region_count = 0;

    ASSERT_EQ(asx_runtime_snapshot_eq(&a, &b), ASX_E_EQUIVALENCE_MISMATCH);
}

TEST(snapshot_eq_null_returns_error) {
    asx_runtime_snapshot snap;
    asx_runtime_snapshot_init(&snap);

    ASSERT_EQ(asx_runtime_snapshot_eq(NULL, &snap), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_snapshot_eq(&snap, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Deterministic capture                                               */
/* ------------------------------------------------------------------ */

TEST(snapshot_capture_deterministic) {
    asx_runtime_snapshot s1, s2;
    asx_region_id rid;
    asx_status st;

    /* First capture */
    asx_runtime_reset();
    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);
    st = asx_runtime_snapshot_capture(&s1);
    ASSERT_EQ(st, ASX_OK);

    /* Second identical run */
    asx_runtime_reset();
    st = asx_region_open(&rid);
    ASSERT_EQ(st, ASX_OK);
    st = asx_runtime_snapshot_capture(&s2);
    ASSERT_EQ(st, ASX_OK);

    ASSERT_EQ(asx_runtime_snapshot_eq(&s1, &s2), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* JSON serialization                                                  */
/* ------------------------------------------------------------------ */

TEST(snapshot_to_json_empty) {
    asx_runtime_snapshot snap;
    asx_codec_buffer buf;
    asx_status s;

    asx_runtime_reset();
    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    asx_codec_buffer_init(&buf);

    s = asx_runtime_snapshot_to_json(&snap, &buf);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(buf.len > 0);
    ASSERT_EQ(buf.data[0], '{');
    ASSERT_EQ(buf.data[buf.len - 1], '}');

    asx_codec_buffer_reset(&buf);
}

TEST(snapshot_to_json_with_entities) {
    asx_runtime_snapshot snap;
    asx_codec_buffer buf;
    asx_region_id rid;
    asx_status s;

    asx_runtime_reset();
    s = asx_region_open(&rid);
    ASSERT_EQ(s, ASX_OK);
    s = asx_runtime_snapshot_capture(&snap);
    ASSERT_EQ(s, ASX_OK);
    asx_codec_buffer_init(&buf);

    s = asx_runtime_snapshot_to_json(&snap, &buf);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(buf.len > 10);

    asx_codec_buffer_reset(&buf);
}

TEST(snapshot_to_json_null_returns_error) {
    asx_runtime_snapshot snap;
    asx_codec_buffer buf;

    asx_runtime_snapshot_init(&snap);
    asx_codec_buffer_init(&buf);

    ASSERT_EQ(asx_runtime_snapshot_to_json(NULL, &buf), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_runtime_snapshot_to_json(&snap, NULL), ASX_E_INVALID_ARGUMENT);

    asx_codec_buffer_reset(&buf);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== snapshot unit tests ===\n");

    asx_runtime_reset();
    RUN_TEST(snapshot_init_zeroes);
    asx_runtime_reset();
    RUN_TEST(snapshot_init_null_safe);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_empty);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_with_region);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_with_task);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_completed_task_preserves_outcome_severity);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_with_obligation);
    asx_runtime_reset();
    RUN_TEST(snapshot_eq_identical);
    asx_runtime_reset();
    RUN_TEST(snapshot_eq_different_counts);
    asx_runtime_reset();
    RUN_TEST(snapshot_eq_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(snapshot_capture_deterministic);
    asx_runtime_reset();
    RUN_TEST(snapshot_to_json_empty);
    asx_runtime_reset();
    RUN_TEST(snapshot_to_json_with_entities);
    asx_runtime_reset();
    RUN_TEST(snapshot_to_json_null_returns_error);

    TEST_REPORT();
    return test_failures;
}
