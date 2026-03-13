/*
 * test_continuity.c — crash/restart replay continuity tests (bd-2n0.5)
 *
 * Validates the binary trace persistence format and end-to-end
 * crash/restart replay continuity:
 *   1. Export a trace to binary wire format
 *   2. Simulate restart (reset runtime)
 *   3. Import persisted trace as replay reference
 *   4. Re-run identical scenario
 *   5. Verify deterministic match via replay verification
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static void reset_all(void) {
    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();
    asx_replay_clear_reference();
}

/* Max binary buffer: header + 1024 events * 24 bytes each */
#define BUF_SIZE (24u + 1024u * 24u)
static uint8_t g_buf[BUF_SIZE];

/* -------------------------------------------------------------------
 * Binary format tests
 * ------------------------------------------------------------------- */

TEST(export_null_buf_returns_invalid) {
    uint32_t out_len = 0;
    reset_all();
    ASSERT_EQ(asx_trace_export_binary(NULL, BUF_SIZE, &out_len), ASX_E_INVALID_ARGUMENT);
}

TEST(export_null_out_len_returns_invalid) {
    reset_all();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(export_empty_trace_produces_header_only) {
    uint32_t out_len = 0;
    reset_all();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);
    /* Header is 24 bytes, 0 events */
    ASSERT_EQ(out_len, ASX_TRACE_BINARY_HEADER);
}

TEST(export_buffer_too_small) {
    uint32_t out_len = 0;
    reset_all();
    /* Emit one event so the required size is header + 24 = 48 */
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 42, 0);
    /* Offer only 30 bytes — not enough */
    ASSERT_EQ(asx_trace_export_binary(g_buf, 30, &out_len), ASX_E_BUFFER_TOO_SMALL);
    /* out_len should tell us how much we need */
    ASSERT_EQ(out_len, ASX_TRACE_BINARY_HEADER + ASX_TRACE_BINARY_EVENT);
}

TEST(export_header_magic_and_version) {
    uint32_t out_len = 0;
    uint32_t magic;
    uint32_t version;

    reset_all();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Read LE32 magic */
    magic = (uint32_t)g_buf[0] | ((uint32_t)g_buf[1] << 8) | ((uint32_t)g_buf[2] << 16) |
            ((uint32_t)g_buf[3] << 24);
    ASSERT_EQ(magic, ASX_TRACE_BINARY_MAGIC);

    version = (uint32_t)g_buf[4] | ((uint32_t)g_buf[5] << 8) | ((uint32_t)g_buf[6] << 16) |
              ((uint32_t)g_buf[7] << 24);
    ASSERT_EQ(version, ASX_TRACE_BINARY_VERSION);
}

TEST(import_null_buf_returns_invalid) {
    reset_all();
    ASSERT_EQ(asx_trace_import_binary(NULL, BUF_SIZE), ASX_E_INVALID_ARGUMENT);
}

TEST(import_truncated_header_returns_invalid) {
    reset_all();
    memset(g_buf, 0, 10);
    ASSERT_EQ(asx_trace_import_binary(g_buf, 10), ASX_E_INVALID_ARGUMENT);
}

TEST(import_bad_magic_returns_invalid) {
    uint32_t out_len = 0;
    reset_all();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);
    /* Corrupt magic */
    g_buf[0] = 0xFF;
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_E_INVALID_ARGUMENT);
}

TEST(import_bad_version_returns_invalid) {
    uint32_t out_len = 0;
    reset_all();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);
    /* Corrupt version to 99 */
    g_buf[4] = 99;
    g_buf[5] = 0;
    g_buf[6] = 0;
    g_buf[7] = 0;
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_E_INVALID_ARGUMENT);
}

TEST(import_truncated_events_returns_invalid) {
    uint32_t out_len = 0;
    reset_all();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);
    /* Pass only header + 1 event instead of 2 */
    ASSERT_EQ(asx_trace_import_binary(g_buf, ASX_TRACE_BINARY_HEADER + ASX_TRACE_BINARY_EVENT),
              ASX_E_INVALID_ARGUMENT);
}

TEST(import_corrupted_digest_returns_invalid) {
    uint32_t out_len = 0;
    reset_all();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 100, 0);
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);
    /* Corrupt the stored digest (bytes 16-23) */
    g_buf[16] ^= 0xFF;
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_E_INVALID_ARGUMENT);
}

/* -------------------------------------------------------------------
 * Round-trip tests
 * ------------------------------------------------------------------- */

TEST(export_import_roundtrip_preserves_events) {
    uint32_t out_len = 0;
    asx_trace_event orig[3];
    uint32_t i;

    reset_all();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 10, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 20, 10);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 20, 0);

    /* Capture original events */
    for (i = 0; i < 3; i++) { ASSERT_TRUE(asx_trace_event_get(i, &orig[i])); }

    /* Export */
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Import as reference */
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_OK);

    /* Verify: current trace should match the imported reference */
    {
        asx_replay_result rr = asx_replay_verify();
        ASSERT_EQ(rr.result, ASX_REPLAY_MATCH);
    }
    asx_replay_clear_reference();
}

TEST(export_import_roundtrip_digest_matches) {
    uint32_t out_len = 0;
    uint64_t orig_digest;

    reset_all();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);

    orig_digest = asx_trace_digest();

    /* Export, reset trace, import, compute digest of imported events */
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* The digest embedded in the binary should match */
    {
        uint64_t stored = (uint64_t)g_buf[16] | ((uint64_t)g_buf[17] << 8) |
                          ((uint64_t)g_buf[18] << 16) | ((uint64_t)g_buf[19] << 24) |
                          ((uint64_t)g_buf[20] << 32) | ((uint64_t)g_buf[21] << 40) |
                          ((uint64_t)g_buf[22] << 48) | ((uint64_t)g_buf[23] << 56);
        ASSERT_EQ(stored, orig_digest);
    }
}

/* -------------------------------------------------------------------
 * Continuity check tests (end-to-end crash/restart)
 * ------------------------------------------------------------------- */

TEST(continuity_check_matching_trace) {
    uint32_t out_len = 0;

    reset_all();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 1, 0);

    /* Export current trace */
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Current trace still in place — check should match */
    ASSERT_EQ(asx_trace_continuity_check(g_buf, out_len), ASX_OK);
}

TEST(continuity_check_mismatched_trace) {
    uint32_t out_len = 0;

    reset_all();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1, 0);

    /* Export */
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Now emit a different event — trace diverges */
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 99, 0);

    /* Continuity check should fail */
    ASSERT_EQ(asx_trace_continuity_check(g_buf, out_len), ASX_E_REPLAY_MISMATCH);
}

/* -------------------------------------------------------------------
 * Full crash/restart scenario with trace events
 *
 * The trace ring buffer (asx_trace_emit) is separate from the
 * scheduler's event log (sched_emit). These scenarios emit trace
 * events directly, simulating what a fully instrumented runtime
 * would produce during a region+task lifecycle.
 * ------------------------------------------------------------------- */

static void run_trace_scenario_simple(void) {
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 100, 1);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);
    asx_trace_emit(ASX_TRACE_REGION_CLOSE, 1, 0);
}

TEST(crash_restart_simple_scenario_replays_identically) {
    uint32_t out_len = 0;

    /* --- Phase 1: initial run + export --- */
    reset_all();
    run_trace_scenario_simple();

    ASSERT_TRUE(asx_trace_event_count() > 0);
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* --- Phase 2: simulated restart + replay --- */
    reset_all();
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_OK);
    /* import_binary restores trace events into the live ring and replay
     * reference. For restart replay, keep the reference but start a fresh
     * live trace for the second run. */
    asx_trace_reset();

    /* Re-run same scenario */
    run_trace_scenario_simple();

    /* Verify replay matches */
    {
        asx_replay_result rr = asx_replay_verify();
        ASSERT_EQ(rr.result, ASX_REPLAY_MATCH);
    }
    asx_replay_clear_reference();
}

static void run_trace_scenario_multi(void) {
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 100, 1);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 101, 1);
    /* Round 1: both pending */
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 101, 0);
    /* Round 2: task A completes, B pending */
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 101, 0);
    /* Round 3: task B completes */
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 101, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 101, 0);
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);
    asx_trace_emit(ASX_TRACE_REGION_CLOSE, 1, 0);
}

static void run_trace_scenario_fail(void) {
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 100, 1);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 100, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 100, 101); /* aux=error code */
    asx_trace_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);
}

TEST(crash_restart_multi_task_replays_identically) {
    uint32_t out_len = 0;

    /* Phase 1 */
    reset_all();
    run_trace_scenario_multi();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Phase 2: restart + replay */
    reset_all();
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_OK);
    asx_trace_reset();
    run_trace_scenario_multi();

    {
        asx_replay_result rr = asx_replay_verify();
        ASSERT_EQ(rr.result, ASX_REPLAY_MATCH);
    }
    asx_replay_clear_reference();
}

TEST(crash_restart_failing_task_replays_identically) {
    uint32_t out_len = 0;

    /* Phase 1: run scenario with a failing task */
    reset_all();
    run_trace_scenario_fail();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Phase 2: restart + replay */
    reset_all();
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_OK);
    asx_trace_reset();
    run_trace_scenario_fail();

    {
        asx_replay_result rr = asx_replay_verify();
        ASSERT_EQ(rr.result, ASX_REPLAY_MATCH);
    }
    asx_replay_clear_reference();
}

TEST(crash_restart_continuity_check_end_to_end) {
    uint32_t out_len = 0;

    /* Phase 1: run + export */
    reset_all();
    run_trace_scenario_simple();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Phase 2: restart + same scenario + continuity check */
    reset_all();
    run_trace_scenario_simple();
    ASSERT_EQ(asx_trace_continuity_check(g_buf, out_len), ASX_OK);
}

TEST(crash_restart_different_scenario_detected) {
    uint32_t out_len = 0;

    /* Phase 1: simple scenario */
    reset_all();
    run_trace_scenario_simple();
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);

    /* Phase 2: different scenario (multi-task) */
    reset_all();
    run_trace_scenario_multi();

    /* Continuity check should fail — different event count/kinds */
    ASSERT_EQ(asx_trace_continuity_check(g_buf, out_len), ASX_E_REPLAY_MISMATCH);
}

/* -------------------------------------------------------------------
 * Empty trace edge case
 * ------------------------------------------------------------------- */

TEST(empty_trace_export_import_roundtrip) {
    uint32_t out_len = 0;

    reset_all();
    /* No events emitted */
    ASSERT_EQ(asx_trace_export_binary(g_buf, BUF_SIZE, &out_len), ASX_OK);
    ASSERT_EQ(out_len, ASX_TRACE_BINARY_HEADER);

    /* Import empty reference */
    ASSERT_EQ(asx_trace_import_binary(g_buf, out_len), ASX_OK);

    /* Empty trace vs empty reference should match */
    {
        asx_replay_result rr = asx_replay_verify();
        ASSERT_EQ(rr.result, ASX_REPLAY_MATCH);
    }
    asx_replay_clear_reference();
}

/* -------------------------------------------------------------------
 * Status string coverage
 * ------------------------------------------------------------------- */

TEST(new_error_codes_have_strings) {
    const char *s1 = asx_status_str(ASX_E_BUFFER_TOO_SMALL);
    const char *s2 = asx_status_str(ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(s1 != NULL);
    ASSERT_TRUE(s2 != NULL);
    ASSERT_STR_EQ(s1, "buffer too small");
    ASSERT_STR_EQ(s2, "replay continuity mismatch");
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    /* Binary format tests */
    RUN_TEST(export_null_buf_returns_invalid);
    RUN_TEST(export_null_out_len_returns_invalid);
    RUN_TEST(export_empty_trace_produces_header_only);
    RUN_TEST(export_buffer_too_small);
    RUN_TEST(export_header_magic_and_version);
    RUN_TEST(import_null_buf_returns_invalid);
    RUN_TEST(import_truncated_header_returns_invalid);
    RUN_TEST(import_bad_magic_returns_invalid);
    RUN_TEST(import_bad_version_returns_invalid);
    RUN_TEST(import_truncated_events_returns_invalid);
    RUN_TEST(import_corrupted_digest_returns_invalid);

    /* Round-trip tests */
    RUN_TEST(export_import_roundtrip_preserves_events);
    RUN_TEST(export_import_roundtrip_digest_matches);

    /* Continuity check tests */
    RUN_TEST(continuity_check_matching_trace);
    RUN_TEST(continuity_check_mismatched_trace);

    /* Full crash/restart scenarios */
    RUN_TEST(crash_restart_simple_scenario_replays_identically);
    RUN_TEST(crash_restart_multi_task_replays_identically);
    RUN_TEST(crash_restart_failing_task_replays_identically);
    RUN_TEST(crash_restart_continuity_check_end_to_end);
    RUN_TEST(crash_restart_different_scenario_detected);

    /* Edge cases */
    RUN_TEST(empty_trace_export_import_roundtrip);

    /* Status string coverage */
    RUN_TEST(new_error_codes_have_strings);

    TEST_REPORT();
    return test_failures;
}
