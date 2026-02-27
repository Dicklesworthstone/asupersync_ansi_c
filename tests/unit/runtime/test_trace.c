/*
 * test_trace.c — unit tests for deterministic event trace, replay, and snapshot
 *
 * Tests: trace emission, digest computation, replay verification,
 * snapshot export, and deterministic identity across runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/trace.h>
#include <asx/core/ghost.h>

/* ---- Trace emission ---- */

TEST(trace_emit_records_events) {
    asx_trace_event ev;

    asx_trace_reset();

    asx_trace_emit(ASX_TRACE_REGION_OPEN, 0x1000, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 0x2000, 0x1000);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0x2000, 0);

    ASSERT_EQ(asx_trace_event_count(), (uint32_t)3);

    ASSERT_TRUE(asx_trace_event_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_OPEN);
    ASSERT_EQ(ev.entity_id, (uint64_t)0x1000);
    ASSERT_EQ(ev.sequence, (uint32_t)0);

    ASSERT_TRUE(asx_trace_event_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_TASK_SPAWN);
    ASSERT_EQ(ev.aux, (uint64_t)0x1000);
    ASSERT_EQ(ev.sequence, (uint32_t)1);

    ASSERT_TRUE(asx_trace_event_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_SCHED_POLL);
    ASSERT_EQ(ev.sequence, (uint32_t)2);
}

TEST(trace_reset_clears) {
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0, 0);
    ASSERT_TRUE(asx_trace_event_count() > (uint32_t)0);

    asx_trace_reset();
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)0);
}

TEST(trace_get_out_of_bounds) {
    asx_trace_event ev;
    asx_trace_reset();

    ASSERT_FALSE(asx_trace_event_get(0, &ev));
    ASSERT_FALSE(asx_trace_event_get(0, NULL));
}

TEST(trace_monotonic_sequence) {
    asx_trace_event e0, e1, e2;
    asx_trace_reset();

    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 2, 1);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 2, 0);

    ASSERT_TRUE(asx_trace_event_get(0, &e0));
    ASSERT_TRUE(asx_trace_event_get(1, &e1));
    ASSERT_TRUE(asx_trace_event_get(2, &e2));

    ASSERT_TRUE(e0.sequence < e1.sequence);
    ASSERT_TRUE(e1.sequence < e2.sequence);
}

/* ---- Digest computation ---- */

TEST(trace_digest_deterministic) {
    uint64_t d1, d2;

    /* Run 1 */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    d1 = asx_trace_digest();

    /* Run 2 (identical) */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    d2 = asx_trace_digest();

    ASSERT_EQ(d1, d2);
}

TEST(trace_digest_differs_on_different_events) {
    uint64_t d1, d2;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    d1 = asx_trace_digest();

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    d2 = asx_trace_digest();

    ASSERT_TRUE(d1 != d2);
}

TEST(trace_digest_empty_is_stable) {
    uint64_t d1, d2;

    asx_trace_reset();
    d1 = asx_trace_digest();

    asx_trace_reset();
    d2 = asx_trace_digest();

    ASSERT_EQ(d1, d2);
}

/* ---- Replay verification ---- */

TEST(replay_match_identical_sequence) {
    asx_trace_event ref[3];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    /* Copy trace as reference */
    asx_trace_event_get(0, &ref[0]);
    asx_trace_event_get(1, &ref[1]);
    asx_trace_event_get(2, &ref[2]);

    ASSERT_EQ(asx_replay_load_reference(ref, 3), ASX_OK);

    /* Replay with same events */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 42, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_MATCH);

    asx_replay_clear_reference();
}

TEST(replay_detects_length_mismatch) {
    asx_trace_event ref[2];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);

    asx_trace_event_get(0, &ref[0]);
    asx_trace_event_get(1, &ref[1]);

    ASSERT_EQ(asx_replay_load_reference(ref, 2), ASX_OK);

    /* Replay with extra event */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_LENGTH_MISMATCH);

    asx_replay_clear_reference();
}

TEST(replay_detects_kind_mismatch) {
    asx_trace_event ref[2];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);

    asx_trace_event_get(0, &ref[0]);
    asx_trace_event_get(1, &ref[1]);

    ASSERT_EQ(asx_replay_load_reference(ref, 2), ASX_OK);

    /* Replay with different event kind */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_BUDGET, 1, 0); /* wrong kind */

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_KIND_MISMATCH);
    ASSERT_EQ(result.divergence_index, (uint32_t)1);

    asx_replay_clear_reference();
}

TEST(replay_detects_entity_mismatch) {
    asx_trace_event ref[1];
    asx_replay_result result;

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    asx_trace_event_get(0, &ref[0]);

    ASSERT_EQ(asx_replay_load_reference(ref, 1), ASX_OK);

    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 99, 0); /* wrong entity */

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_ENTITY_MISMATCH);
    ASSERT_EQ(result.divergence_index, (uint32_t)0);

    asx_replay_clear_reference();
}

TEST(replay_no_reference_is_match) {
    asx_replay_result result;

    asx_replay_clear_reference();
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);

    result = asx_replay_verify();
    ASSERT_EQ(result.result, ASX_REPLAY_MATCH);
}

/* ---- Snapshot export ---- */

TEST(snapshot_capture_empty) {
    asx_snapshot_buffer snap;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_snapshot_capture(&snap), ASX_OK);
    ASSERT_TRUE(snap.len > (uint32_t)0);
    /* Should contain JSON structure markers */
    ASSERT_TRUE(snap.data[0] == '{');
}

TEST(snapshot_capture_with_region) {
    asx_snapshot_buffer snap;
    asx_region_id rid;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_snapshot_capture(&snap), ASX_OK);

    /* Should mention regions */
    ASSERT_TRUE(snap.len > (uint32_t)20);
}

TEST(snapshot_digest_deterministic) {
    asx_snapshot_buffer s1, s2;
    uint64_t d1, d2;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_snapshot_capture(&s1), ASX_OK);
    d1 = asx_snapshot_digest(&s1);

    /* Same state again */
    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_snapshot_capture(&s2), ASX_OK);
    d2 = asx_snapshot_digest(&s2);

    ASSERT_EQ(d1, d2);
}

TEST(snapshot_null_returns_error) {
    ASSERT_EQ(asx_snapshot_capture(NULL), ASX_E_INVALID_ARGUMENT);
}

/* ---- String helpers ---- */

TEST(trace_event_kind_str_all_kinds) {
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_SCHED_POLL) != NULL);
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_REGION_OPEN) != NULL);
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_OBLIGATION_COMMIT) != NULL);
    ASSERT_TRUE(asx_trace_event_kind_str(ASX_TRACE_TIMER_FIRE) != NULL);
}

TEST(replay_result_kind_str_all_kinds) {
    ASSERT_TRUE(asx_replay_result_kind_str(ASX_REPLAY_MATCH) != NULL);
    ASSERT_TRUE(asx_replay_result_kind_str(ASX_REPLAY_LENGTH_MISMATCH) != NULL);
    ASSERT_TRUE(asx_replay_result_kind_str(ASX_REPLAY_DIGEST_MISMATCH) != NULL);
}

int main(void) {
    fprintf(stderr, "=== test_trace ===\n");

    RUN_TEST(trace_emit_records_events);
    RUN_TEST(trace_reset_clears);
    RUN_TEST(trace_get_out_of_bounds);
    RUN_TEST(trace_monotonic_sequence);
    RUN_TEST(trace_digest_deterministic);
    RUN_TEST(trace_digest_differs_on_different_events);
    RUN_TEST(trace_digest_empty_is_stable);
    RUN_TEST(replay_match_identical_sequence);
    RUN_TEST(replay_detects_length_mismatch);
    RUN_TEST(replay_detects_kind_mismatch);
    RUN_TEST(replay_detects_entity_mismatch);
    RUN_TEST(replay_no_reference_is_match);
    RUN_TEST(snapshot_capture_empty);
    RUN_TEST(snapshot_capture_with_region);
    RUN_TEST(snapshot_digest_deterministic);
    RUN_TEST(snapshot_null_returns_error);
    RUN_TEST(trace_event_kind_str_all_kinds);
    RUN_TEST(replay_result_kind_str_all_kinds);

    TEST_REPORT();
    return test_failures;
}
