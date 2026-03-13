/*
 * test_event_log.c — unit tests for asx/runtime/event.h API
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/codec/codec.h>
#include <asx/runtime/event.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Reset / basic emission                                              */
/* ------------------------------------------------------------------ */

TEST(event_log_reset_clears_state) {
    asx_runtime_reset();
    asx_event_log_reset();
    ASSERT_EQ(asx_event_log_count(), 0u);
    ASSERT_EQ(asx_event_hash_chain(), 14695981039346656037ULL); /* FNV offset */
}

TEST(event_emit_increments_count) {
    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    ASSERT_EQ(asx_event_log_count(), 1u);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 2, 1, ASX_OK);
    ASSERT_EQ(asx_event_log_count(), 2u);
}

TEST(event_emit_returns_sequence) {
    uint32_t seq0, seq1, seq2;
    asx_runtime_reset();
    asx_event_log_reset();
    seq0 = asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    seq1 = asx_event_emit(ASX_EVENT_TASK_SPAWN, 2, 1, ASX_OK);
    seq2 = asx_event_emit(ASX_EVENT_TASK_COMPLETE, 2, 1, ASX_OK);
    ASSERT_EQ(seq0, 0u);
    ASSERT_EQ(seq1, 1u);
    ASSERT_EQ(seq2, 2u);
}

/* ------------------------------------------------------------------ */
/* Get / read-back                                                     */
/* ------------------------------------------------------------------ */

TEST(event_log_get_reads_back) {
    asx_event_record rec;
    int ok;
    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 42, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 99, 42, ASX_OK);

    ok = asx_event_log_get(0, &rec);
    ASSERT_TRUE(ok);
    ASSERT_EQ(rec.kind, ASX_EVENT_REGION_OPEN);
    ASSERT_EQ(rec.entity_id, 42u);
    ASSERT_EQ(rec.parent_id, 0u);
    ASSERT_EQ(rec.sequence, 0u);
    ASSERT_EQ(rec.status, ASX_OK);

    ok = asx_event_log_get(1, &rec);
    ASSERT_TRUE(ok);
    ASSERT_EQ(rec.kind, ASX_EVENT_TASK_SPAWN);
    ASSERT_EQ(rec.entity_id, 99u);
    ASSERT_EQ(rec.parent_id, 42u);
}

TEST(event_log_get_oob_returns_zero) {
    asx_event_record rec;
    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    ASSERT_FALSE(asx_event_log_get(1, &rec));
    ASSERT_FALSE(asx_event_log_get(999, &rec));
}

TEST(event_log_get_null_returns_zero) {
    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    ASSERT_FALSE(asx_event_log_get(0, NULL));
}

/* ------------------------------------------------------------------ */
/* Hash chain determinism                                              */
/* ------------------------------------------------------------------ */

TEST(hash_chain_is_deterministic) {
    uint64_t h1, h2;
    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 10, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 20, 10, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_COMPLETE, 20, 10, ASX_OK);
    h1 = asx_event_hash_chain();

    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 10, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 20, 10, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_COMPLETE, 20, 10, ASX_OK);
    h2 = asx_event_hash_chain();

    ASSERT_EQ(h1, h2);
}

TEST(hash_chain_differs_for_different_events) {
    uint64_t h1, h2;
    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 10, 0, ASX_OK);
    h1 = asx_event_hash_chain();

    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_CLOSE, 10, 0, ASX_OK);
    h2 = asx_event_hash_chain();

    ASSERT_NE(h1, h2);
}

/* ------------------------------------------------------------------ */
/* Kind string table                                                   */
/* ------------------------------------------------------------------ */

TEST(event_kind_str_known) {
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_REGION_OPEN), "region_open");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_REGION_CLOSE), "region_close");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_TASK_SPAWN), "task_spawn");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_TASK_POLL), "task_poll");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_TASK_COMPLETE), "task_complete");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_OBLIGATION_CREATE), "obligation_create");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_OBLIGATION_COMMIT), "obligation_commit");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_OBLIGATION_ABORT), "obligation_abort");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_BUDGET_EXHAUSTED), "budget_exhausted");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_QUIESCENT), "quiescent");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_DRAIN_BEGIN), "drain_begin");
    ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_DRAIN_END), "drain_end");
}

TEST(event_kind_str_unknown) { ASSERT_STR_EQ(asx_event_kind_str(ASX_EVENT_KIND_COUNT), "unknown"); }

/* ------------------------------------------------------------------ */
/* Replay verification                                                 */
/* ------------------------------------------------------------------ */

TEST(replay_verify_identical) {
    asx_event_record expected[3];
    asx_replay_divergence div;
    asx_status s;

    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 2, 1, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_COMPLETE, 2, 1, ASX_OK);

    /* Capture expected */
    asx_event_log_get(0, &expected[0]);
    asx_event_log_get(1, &expected[1]);
    asx_event_log_get(2, &expected[2]);

    s = asx_event_replay_verify(expected, 3, &div);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_FALSE(div.diverged);
    ASSERT_FALSE(div.count_mismatch);
}

TEST(replay_verify_count_mismatch) {
    asx_event_record expected[1];
    asx_replay_divergence div;
    asx_status s;

    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 2, 1, ASX_OK);

    asx_event_log_get(0, &expected[0]);

    s = asx_event_replay_verify(expected, 1, &div);
    ASSERT_EQ(s, ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.count_mismatch);
}

TEST(replay_verify_content_mismatch) {
    asx_event_record expected[2];
    asx_replay_divergence div;
    asx_status s;

    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 2, 1, ASX_OK);

    asx_event_log_get(0, &expected[0]);
    asx_event_log_get(1, &expected[1]);

    /* Tamper with expected */
    expected[1].kind = ASX_EVENT_TASK_COMPLETE;

    s = asx_event_replay_verify(expected, 2, &div);
    ASSERT_EQ(s, ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.diverged);
    ASSERT_EQ(div.first_divergence_index, 1u);
}

TEST(replay_verify_null_divergence_returns_error) {
    asx_status s;
    asx_runtime_reset();
    asx_event_log_reset();
    s = asx_event_replay_verify(NULL, 0, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* JSON serialization                                                  */
/* ------------------------------------------------------------------ */

TEST(event_log_to_json_empty) {
    asx_codec_buffer buf;
    asx_status s;

    asx_runtime_reset();
    asx_event_log_reset();
    asx_codec_buffer_init(&buf);

    s = asx_event_log_to_json(&buf);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(buf.len >= 2);
    ASSERT_EQ(buf.data[0], '[');
    ASSERT_EQ(buf.data[buf.len - 1], ']');

    asx_codec_buffer_reset(&buf);
}

TEST(event_log_to_json_with_events) {
    asx_codec_buffer buf;
    asx_status s;

    asx_runtime_reset();
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 2, 1, ASX_OK);

    asx_codec_buffer_init(&buf);
    s = asx_event_log_to_json(&buf);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_TRUE(buf.len > 2);
    ASSERT_EQ(buf.data[0], '[');
    ASSERT_EQ(buf.data[buf.len - 1], ']');

    asx_codec_buffer_reset(&buf);
}

TEST(event_log_to_json_null_returns_error) {
    asx_status s;
    asx_runtime_reset();
    s = asx_event_log_to_json(NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Reset via asx_runtime_reset clears event log                        */
/* ------------------------------------------------------------------ */

TEST(runtime_reset_clears_event_log) {
    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 1, 0, ASX_OK);
    ASSERT_EQ(asx_event_log_count(), 1u);

    asx_runtime_reset();
    ASSERT_EQ(asx_event_log_count(), 0u);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== event_log unit tests ===\n");

    asx_runtime_reset();
    RUN_TEST(event_log_reset_clears_state);
    asx_runtime_reset();
    RUN_TEST(event_emit_increments_count);
    asx_runtime_reset();
    RUN_TEST(event_emit_returns_sequence);
    asx_runtime_reset();
    RUN_TEST(event_log_get_reads_back);
    asx_runtime_reset();
    RUN_TEST(event_log_get_oob_returns_zero);
    asx_runtime_reset();
    RUN_TEST(event_log_get_null_returns_zero);
    asx_runtime_reset();
    RUN_TEST(hash_chain_is_deterministic);
    asx_runtime_reset();
    RUN_TEST(hash_chain_differs_for_different_events);
    asx_runtime_reset();
    RUN_TEST(event_kind_str_known);
    asx_runtime_reset();
    RUN_TEST(event_kind_str_unknown);
    asx_runtime_reset();
    RUN_TEST(replay_verify_identical);
    asx_runtime_reset();
    RUN_TEST(replay_verify_count_mismatch);
    asx_runtime_reset();
    RUN_TEST(replay_verify_content_mismatch);
    asx_runtime_reset();
    RUN_TEST(replay_verify_null_divergence_returns_error);
    asx_runtime_reset();
    RUN_TEST(event_log_to_json_empty);
    asx_runtime_reset();
    RUN_TEST(event_log_to_json_with_events);
    asx_runtime_reset();
    RUN_TEST(event_log_to_json_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(runtime_reset_clears_event_log);

    TEST_REPORT();
    return test_failures;
}
