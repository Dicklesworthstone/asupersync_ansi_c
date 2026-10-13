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

/* One reference run: its first `stored` events, total and hash chain, as a
 * caller would keep them. */
typedef struct {
    asx_event_record events[ASX_EVENT_LOG_CAPACITY];
    uint32_t stored;
    uint32_t total;
    uint64_t chain;
} reference_run;

/* A run of `n` events from a fresh log; the event at `diverge_at` (if < n)
 * names another entity. */
static void emit_run(uint32_t n, uint32_t diverge_at) {
    uint32_t i;

    asx_event_log_reset();
    for (i = 0; i < n; i++) {
        asx_event_kind kind = (i % 3u == 0u) ? ASX_EVENT_TASK_POLL : ASX_EVENT_TASK_SPAWN;
        uint64_t entity = (i == diverge_at) ? 0xBADu : (uint64_t)i + 1u;

        asx_event_emit(kind, entity, 1u, ASX_OK);
    }
}

static void capture_reference(reference_run *ref) {
    uint32_t i;

    ref->total = asx_event_log_count();
    ref->stored = ref->total < (uint32_t)ASX_EVENT_LOG_CAPACITY ? ref->total
                                                                : (uint32_t)ASX_EVENT_LOG_CAPACITY;
    ref->chain = asx_event_hash_chain();
    for (i = 0; i < ref->stored; i++) (void)asx_event_log_get(i, &ref->events[i]);
}

TEST(replay_verify_identical) {
    static reference_run ref;
    asx_replay_divergence div;
    asx_status s;

    asx_runtime_reset();
    emit_run(3u, UINT32_MAX);
    capture_reference(&ref);

    /* the same run again, from a fresh log */
    emit_run(3u, UINT32_MAX);
    s = asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div);
    ASSERT_EQ(s, ASX_OK);
    ASSERT_FALSE(div.diverged);
    ASSERT_FALSE(div.count_mismatch);
    ASSERT_FALSE(div.chain_mismatch);
    ASSERT_EQ(div.compared, 3u);
}

TEST(replay_verify_count_mismatch) {
    static reference_run ref;
    asx_replay_divergence div;
    asx_status s;

    asx_runtime_reset();
    emit_run(1u, UINT32_MAX);
    capture_reference(&ref);

    emit_run(2u, UINT32_MAX);
    s = asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div);
    ASSERT_EQ(s, ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.count_mismatch);
    ASSERT_TRUE(div.diverged);
    ASSERT_EQ(div.expected_count, 1u);
    ASSERT_EQ(div.actual_count, 2u);
    ASSERT_EQ(div.first_divergence_index, 1u); /* the first event not compared */
}

TEST(replay_verify_content_mismatch) {
    static reference_run ref;
    asx_replay_divergence div;
    asx_status s;

    asx_runtime_reset();
    emit_run(2u, UINT32_MAX);
    capture_reference(&ref);

    /* Tamper with expected */
    ref.events[1].kind = ASX_EVENT_TASK_COMPLETE;

    s = asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div);
    ASSERT_EQ(s, ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.diverged);
    ASSERT_FALSE(div.count_mismatch);
    ASSERT_FALSE(div.chain_mismatch);
    ASSERT_EQ(div.first_divergence_index, 1u);
    ASSERT_EQ(div.expected.kind, ASX_EVENT_TASK_COMPLETE);
    ASSERT_EQ(div.actual.kind, ASX_EVENT_TASK_SPAWN);
}

TEST(replay_verify_null_divergence_returns_error) {
    asx_status s;
    asx_runtime_reset();
    asx_event_log_reset();
    s = asx_event_replay_verify(NULL, 0, 0, 0, NULL);
    ASSERT_EQ(s, ASX_E_INVALID_ARGUMENT);
}

TEST(replay_verify_rejects_inconsistent_arguments) {
    asx_event_record one;
    asx_replay_divergence div;

    asx_runtime_reset();
    asx_event_log_reset();
    memset(&one, 0, sizeof(one));
    /* entries promised but no array */
    ASSERT_EQ(asx_event_replay_verify(NULL, 1, 1, 0, &div), ASX_E_INVALID_ARGUMENT);
    /* more entries than the reference run emitted */
    ASSERT_EQ(asx_event_replay_verify(&one, 2, 1, 0, &div), ASX_E_INVALID_ARGUMENT);
}

/* RB1 class: the log stores the first ASX_EVENT_LOG_CAPACITY events only
 * (512 by default, 64 in R1). A run that matches the stored window but
 * differs after it must not verify. */
TEST(replay_verify_sees_a_divergence_beyond_the_stored_window) {
    static reference_run ref;
    asx_replay_divergence div;
    const uint32_t total = (uint32_t)ASX_EVENT_LOG_CAPACITY + 188u;          /* 700 by default */
    const uint32_t beyond = (uint32_t)ASX_EVENT_LOG_CAPACITY + 88u;          /* 600 */
    const uint32_t inside = ((uint32_t)ASX_EVENT_LOG_CAPACITY * 25u) / 128u; /* 100 */

    asx_runtime_reset();
    emit_run(total, UINT32_MAX);
    capture_reference(&ref);
    ASSERT_EQ(ref.stored, (uint32_t)ASX_EVENT_LOG_CAPACITY);
    ASSERT_EQ(ref.total, total);

    /* the same events: verified, and the whole window compared */
    emit_run(total, UINT32_MAX);
    ASSERT_EQ(asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div), ASX_OK);
    ASSERT_EQ(div.compared, (uint32_t)ASX_EVENT_LOG_CAPACITY);

    /* an event past the window differs: the stored window and the total
     * match, the chain does not */
    emit_run(total, beyond);
    ASSERT_EQ(asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.chain_mismatch);
    ASSERT_FALSE(div.count_mismatch);
    ASSERT_TRUE(div.diverged);
    ASSERT_EQ(div.compared, (uint32_t)ASX_EVENT_LOG_CAPACITY);
    ASSERT_EQ(div.first_divergence_index, (uint32_t)ASX_EVENT_LOG_CAPACITY);
    ASSERT_TRUE(div.expected_chain != div.actual_chain);

    /* an event inside the window differs: named exactly */
    emit_run(total, inside);
    ASSERT_EQ(asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_EQ(div.first_divergence_index, inside);
    ASSERT_EQ(div.actual.entity_id, (uint64_t)0xBADu);
    ASSERT_EQ(div.expected.entity_id, (uint64_t)inside + 1u);
}

TEST(replay_verify_refuses_a_stored_prefix_of_a_longer_run) {
    static reference_run ref;
    asx_replay_divergence div;
    const uint32_t cap = (uint32_t)ASX_EVENT_LOG_CAPACITY;

    /* a reference of exactly the stored 512 events ... */
    asx_runtime_reset();
    emit_run(cap, UINT32_MAX);
    capture_reference(&ref);
    ASSERT_EQ(asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div), ASX_OK);

    /* ... does not verify a run of 700 that starts the same way */
    emit_run(cap + 188u, UINT32_MAX);
    ASSERT_EQ(asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.count_mismatch);
    ASSERT_EQ(div.expected_count, cap);
    ASSERT_EQ(div.actual_count, cap + 188u);
    ASSERT_EQ(div.compared, cap);

    /* nor a shorter run whose events are all the reference's first ones */
    emit_run(cap - 1u, UINT32_MAX);
    ASSERT_EQ(asx_event_replay_verify(ref.events, ref.stored, ref.total, ref.chain, &div),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(div.count_mismatch);
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

/* The reset clears only the stored prefix. After the log filled and its
 * count ran past the capacity, a short run must read back, and chain, as
 * it does on a fresh log. */
TEST(event_log_reset_after_overflow_matches_fresh_log) {
    asx_event_record rec;
    uint64_t fresh_chain;
    uint32_t i;

    asx_event_log_reset();
    asx_event_emit(ASX_EVENT_REGION_OPEN, 7, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 8, 7, ASX_OK);
    fresh_chain = asx_event_hash_chain();

    asx_event_log_reset();
    for (i = 0; i < (uint32_t)ASX_EVENT_LOG_CAPACITY + 5u; i++) {
        asx_event_emit(ASX_EVENT_TASK_POLL, (uint64_t)i, 1, ASX_E_INVALID_STATE);
    }
    ASSERT_EQ(asx_event_log_count(), (uint32_t)ASX_EVENT_LOG_CAPACITY + 5u);
    ASSERT_FALSE(asx_event_log_get((uint32_t)ASX_EVENT_LOG_CAPACITY, &rec));
    ASSERT_TRUE(asx_event_log_get((uint32_t)ASX_EVENT_LOG_CAPACITY - 1u, &rec));
    ASSERT_EQ(rec.entity_id, (uint64_t)ASX_EVENT_LOG_CAPACITY - 1u);

    asx_event_log_reset();
    ASSERT_EQ(asx_event_log_count(), 0u);
    ASSERT_FALSE(asx_event_log_get(0u, &rec));
    ASSERT_EQ(asx_event_hash_chain(), 14695981039346656037ULL);

    asx_event_emit(ASX_EVENT_REGION_OPEN, 7, 0, ASX_OK);
    asx_event_emit(ASX_EVENT_TASK_SPAWN, 8, 7, ASX_OK);
    ASSERT_EQ(asx_event_hash_chain(), fresh_chain);
    ASSERT_TRUE(asx_event_log_get(0u, &rec));
    ASSERT_EQ(rec.entity_id, 7u);
    ASSERT_EQ(rec.status, ASX_OK);
    ASSERT_TRUE(asx_event_log_get(1u, &rec));
    ASSERT_EQ(rec.entity_id, 8u);
    ASSERT_EQ(rec.parent_id, 7u);
    ASSERT_FALSE(asx_event_log_get(2u, &rec));
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
    RUN_TEST(replay_verify_rejects_inconsistent_arguments);
    asx_runtime_reset();
    RUN_TEST(replay_verify_sees_a_divergence_beyond_the_stored_window);
    asx_runtime_reset();
    RUN_TEST(replay_verify_refuses_a_stored_prefix_of_a_longer_run);
    asx_runtime_reset();
    RUN_TEST(event_log_to_json_empty);
    asx_runtime_reset();
    RUN_TEST(event_log_to_json_with_events);
    asx_runtime_reset();
    RUN_TEST(event_log_to_json_null_returns_error);
    asx_runtime_reset();
    RUN_TEST(runtime_reset_clears_event_log);
    asx_runtime_reset();
    RUN_TEST(event_log_reset_after_overflow_matches_fresh_log);

    TEST_REPORT();
    return test_failures;
}
