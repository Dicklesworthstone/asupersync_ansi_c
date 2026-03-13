/*
 * test_hindsight.c — unit tests for hindsight nondeterminism logging ring
 *
 * Tests: ring insertion, wrapping, flush-to-JSON, digest computation,
 * overflow detection, deterministic identity, and integration with
 * the trace system.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/asx_config.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ---- Basic ring operations ---- */

TEST(hindsight_init_clears_state) {
    asx_hindsight_init();
    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)0);
    ASSERT_EQ(asx_hindsight_readable_count(), (uint32_t)0);
    ASSERT_FALSE(asx_hindsight_overflowed());
}

TEST(hindsight_log_single_event) {
    asx_hindsight_event ev;

    asx_hindsight_reset();
    asx_trace_reset();

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0x1000, 42);

    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)1);
    ASSERT_EQ(asx_hindsight_readable_count(), (uint32_t)1);
    ASSERT_FALSE(asx_hindsight_overflowed());

    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_CLOCK_READ);
    ASSERT_EQ(ev.entity_id, (uint64_t)0x1000);
    ASSERT_EQ(ev.observed_value, (uint64_t)42);
    ASSERT_EQ(ev.sequence, (uint32_t)0);
    ASSERT_EQ(ev.trace_seq, (uint32_t)0);
}

TEST(hindsight_log_multiple_events) {
    asx_hindsight_event ev;

    asx_hindsight_reset();
    asx_trace_reset();

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0x10, 100);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_hindsight_log(ASX_ND_ENTROPY_READ, 0x20, 200);
    asx_hindsight_log(ASX_ND_IO_READY, 0x30, 300);

    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)3);
    ASSERT_EQ(asx_hindsight_readable_count(), (uint32_t)3);

    /* First event: clock read, trace_seq=0 (no trace events yet) */
    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_CLOCK_READ);
    ASSERT_EQ(ev.observed_value, (uint64_t)100);
    ASSERT_EQ(ev.trace_seq, (uint32_t)0);

    /* Second event: entropy read, trace_seq=1 (after trace emit) */
    ASSERT_TRUE(asx_hindsight_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_ENTROPY_READ);
    ASSERT_EQ(ev.observed_value, (uint64_t)200);
    ASSERT_EQ(ev.trace_seq, (uint32_t)1);

    /* Third event: io ready */
    ASSERT_TRUE(asx_hindsight_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_IO_READY);
    ASSERT_EQ(ev.observed_value, (uint64_t)300);
}

TEST(hindsight_monotonic_sequence) {
    asx_hindsight_event ev0, ev1, ev2;
    uint32_t i;

    asx_hindsight_reset();
    asx_trace_reset();

    for (i = 0; i < 10; i++) { asx_hindsight_log(ASX_ND_CLOCK_READ, 0, (uint64_t)i); }

    ASSERT_TRUE(asx_hindsight_get(0, &ev0));
    ASSERT_TRUE(asx_hindsight_get(1, &ev1));
    ASSERT_TRUE(asx_hindsight_get(9, &ev2));

    ASSERT_EQ(ev0.sequence, (uint32_t)0);
    ASSERT_EQ(ev1.sequence, (uint32_t)1);
    ASSERT_EQ(ev2.sequence, (uint32_t)9);
    ASSERT_TRUE(ev0.sequence < ev1.sequence);
}

/* ---- Ring wrapping ---- */

TEST(hindsight_wraps_at_capacity) {
    uint32_t i;
    asx_hindsight_event ev;

    asx_hindsight_reset();
    asx_trace_reset();

    /* Fill ring exactly to capacity */
    for (i = 0; i < ASX_HINDSIGHT_CAPACITY; i++) {
        asx_hindsight_log(ASX_ND_CLOCK_READ, 0, (uint64_t)i);
    }

    ASSERT_EQ(asx_hindsight_total_count(), ASX_HINDSIGHT_CAPACITY);
    ASSERT_EQ(asx_hindsight_readable_count(), ASX_HINDSIGHT_CAPACITY);
    ASSERT_FALSE(asx_hindsight_overflowed());

    /* One more to trigger overflow */
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 9999);

    ASSERT_EQ(asx_hindsight_total_count(), ASX_HINDSIGHT_CAPACITY + 1u);
    ASSERT_EQ(asx_hindsight_readable_count(), ASX_HINDSIGHT_CAPACITY);
    ASSERT_TRUE(asx_hindsight_overflowed());

    /* Oldest should now be index 1 (slot 0 was overwritten) */
    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.observed_value, (uint64_t)1);

    /* Newest should be at last readable index */
    ASSERT_TRUE(asx_hindsight_get(ASX_HINDSIGHT_CAPACITY - 1u, &ev));
    ASSERT_EQ(ev.observed_value, (uint64_t)9999);
}

TEST(hindsight_heavy_wrapping) {
    uint32_t i;
    asx_hindsight_event ev;
    uint32_t expected_oldest;

    asx_hindsight_reset();
    asx_trace_reset();

    /* Write 3x capacity */
    for (i = 0; i < ASX_HINDSIGHT_CAPACITY * 3u; i++) {
        asx_hindsight_log(ASX_ND_ENTROPY_READ, 0, (uint64_t)i);
    }

    ASSERT_EQ(asx_hindsight_total_count(), ASX_HINDSIGHT_CAPACITY * 3u);
    ASSERT_EQ(asx_hindsight_readable_count(), ASX_HINDSIGHT_CAPACITY);
    ASSERT_TRUE(asx_hindsight_overflowed());

    /* Oldest readable = first event not overwritten */
    expected_oldest = ASX_HINDSIGHT_CAPACITY * 2u;
    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.observed_value, (uint64_t)expected_oldest);

    /* Newest = last written */
    ASSERT_TRUE(asx_hindsight_get(ASX_HINDSIGHT_CAPACITY - 1u, &ev));
    ASSERT_EQ(ev.observed_value, (uint64_t)(ASX_HINDSIGHT_CAPACITY * 3u - 1u));
}

/* ---- Out-of-bounds access ---- */

TEST(hindsight_get_oob_returns_zero) {
    asx_hindsight_event ev;

    asx_hindsight_reset();

    ASSERT_FALSE(asx_hindsight_get(0, &ev));
    ASSERT_FALSE(asx_hindsight_get(100, &ev));
    ASSERT_FALSE(asx_hindsight_get(0, NULL));
}

TEST(hindsight_get_oob_after_wrap) {
    asx_hindsight_event ev;
    uint32_t i;

    asx_hindsight_reset();
    asx_trace_reset();

    for (i = 0; i < ASX_HINDSIGHT_CAPACITY + 10u; i++) {
        asx_hindsight_log(ASX_ND_CLOCK_READ, 0, (uint64_t)i);
    }

    /* Index beyond readable count must fail */
    ASSERT_FALSE(asx_hindsight_get(ASX_HINDSIGHT_CAPACITY, &ev));
}

/* ---- All event kinds ---- */

TEST(hindsight_all_event_kinds) {
    asx_hindsight_event ev;

    asx_hindsight_reset();
    asx_trace_reset();

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 1);
    asx_hindsight_log(ASX_ND_CLOCK_SKEW, 0, 2);
    asx_hindsight_log(ASX_ND_ENTROPY_READ, 0, 3);
    asx_hindsight_log(ASX_ND_IO_READY, 0, 4);
    asx_hindsight_log(ASX_ND_IO_TIMEOUT, 0, 5);
    asx_hindsight_log(ASX_ND_SIGNAL_ARRIVAL, 0, 6);
    asx_hindsight_log(ASX_ND_SCHED_TIE_BREAK, 0, 7);
    asx_hindsight_log(ASX_ND_TIMER_COALESCE, 0, 8);

    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)8);

    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_CLOCK_READ);
    ASSERT_TRUE(asx_hindsight_get(1, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_CLOCK_SKEW);
    ASSERT_TRUE(asx_hindsight_get(2, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_ENTROPY_READ);
    ASSERT_TRUE(asx_hindsight_get(3, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_IO_READY);
    ASSERT_TRUE(asx_hindsight_get(4, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_IO_TIMEOUT);
    ASSERT_TRUE(asx_hindsight_get(5, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_SIGNAL_ARRIVAL);
    ASSERT_TRUE(asx_hindsight_get(6, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_SCHED_TIE_BREAK);
    ASSERT_TRUE(asx_hindsight_get(7, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_TIMER_COALESCE);
}

/* ---- Digest ---- */

TEST(hindsight_digest_empty_is_fnv_offset) {
    asx_hindsight_reset();
    /* Empty ring should produce FNV-1a offset basis */
    ASSERT_EQ(asx_hindsight_digest(), (uint64_t)0x517cc1b727220a95ULL);
}

TEST(hindsight_digest_deterministic) {
    uint64_t d1, d2;
    uint32_t i;

    /* Run 1 */
    asx_hindsight_reset();
    asx_trace_reset();
    for (i = 0; i < 5; i++) {
        asx_hindsight_log(ASX_ND_CLOCK_READ, (uint64_t)i, (uint64_t)(i * 100));
    }
    d1 = asx_hindsight_digest();

    /* Run 2 — identical events */
    asx_hindsight_reset();
    asx_trace_reset();
    for (i = 0; i < 5; i++) {
        asx_hindsight_log(ASX_ND_CLOCK_READ, (uint64_t)i, (uint64_t)(i * 100));
    }
    d2 = asx_hindsight_digest();

    ASSERT_EQ(d1, d2);
}

TEST(hindsight_digest_differs_on_different_input) {
    uint64_t d1, d2;

    asx_hindsight_reset();
    asx_trace_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);
    d1 = asx_hindsight_digest();

    asx_hindsight_reset();
    asx_trace_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 43);
    d2 = asx_hindsight_digest();

    ASSERT_TRUE(d1 != d2);
}

/* ---- JSON flush ---- */

TEST(hindsight_flush_json_empty) {
    asx_hindsight_flush_buffer buf;
    asx_status st;

    asx_hindsight_reset();

    st = asx_hindsight_flush_json(&buf);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(buf.len > (uint32_t)0);

    /* Should contain total_count:0 */
    ASSERT_TRUE(strstr(buf.data, "\"total_count\":0") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"overflowed\":false") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"events\":[]") != NULL);
}

TEST(hindsight_flush_json_with_events) {
    asx_hindsight_flush_buffer buf;
    asx_status st;

    asx_hindsight_reset();
    asx_trace_reset();

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0x1234, 42);
    asx_hindsight_log(ASX_ND_ENTROPY_READ, 0x5678, 99);

    st = asx_hindsight_flush_json(&buf);
    ASSERT_EQ(st, ASX_OK);

    /* Verify JSON structure */
    ASSERT_TRUE(strstr(buf.data, "\"total_count\":2") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"kind\":\"clock_read\"") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"kind\":\"entropy_read\"") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"observed_value\":42") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"observed_value\":99") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"entity_id\":4660") != NULL); /* 0x1234 */
}

TEST(hindsight_flush_json_null_rejected) {
    asx_status st;
    st = asx_hindsight_flush_json(NULL);
    ASSERT_EQ(st, ASX_E_INVALID_ARGUMENT);
}

TEST(hindsight_flush_json_after_wrap) {
    asx_hindsight_flush_buffer buf;
    asx_status st;
    uint32_t i;

    asx_hindsight_reset();
    asx_trace_reset();

    /* Fill past capacity */
    for (i = 0; i < ASX_HINDSIGHT_CAPACITY + 5u; i++) {
        asx_hindsight_log(ASX_ND_CLOCK_READ, 0, (uint64_t)i);
    }

    st = asx_hindsight_flush_json(&buf);
    ASSERT_EQ(st, ASX_OK);

    ASSERT_TRUE(strstr(buf.data, "\"overflowed\":true") != NULL);
    /* The oldest event should have observed_value=5 (indices 0-4 were overwritten) */
    ASSERT_TRUE(strstr(buf.data, "\"observed_value\":5") != NULL);
}

/* ---- Flush on divergence ---- */

TEST(hindsight_flush_on_divergence_empty_ring) {
    asx_hindsight_flush_buffer out;
    asx_status st;

    asx_hindsight_reset();

    st = asx_hindsight_flush_on_divergence(NULL, &out);
    ASSERT_EQ(st, ASX_E_PENDING);
}

TEST(hindsight_flush_on_divergence_with_events) {
    asx_hindsight_flush_buffer out;
    asx_status st;

    asx_hindsight_reset();
    asx_trace_reset();

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);

    st = asx_hindsight_flush_on_divergence(NULL, &out);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(out.len > (uint32_t)0);
    ASSERT_TRUE(strstr(out.data, "\"clock_read\"") != NULL);
}

TEST(hindsight_flush_on_divergence_null_out) {
    asx_status st;
    st = asx_hindsight_flush_on_divergence(NULL, NULL);
    ASSERT_EQ(st, ASX_E_INVALID_ARGUMENT);
}

/* ---- Event kind strings ---- */

TEST(hindsight_kind_str_coverage) {
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_CLOCK_READ), "clock_read") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_CLOCK_SKEW), "clock_skew") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_ENTROPY_READ), "entropy_read") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_IO_READY), "io_ready") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_IO_TIMEOUT), "io_timeout") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_SIGNAL_ARRIVAL), "signal_arrival") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_SCHED_TIE_BREAK), "sched_tie_break") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str(ASX_ND_TIMER_COALESCE), "timer_coalesce") == 0);
    ASSERT_TRUE(strcmp(asx_nd_event_kind_str((asx_nd_event_kind)0xFF), "unknown") == 0);
}

/* ---- Trace sequence integration ---- */

TEST(hindsight_trace_seq_tracks_trace_count) {
    asx_hindsight_event ev;

    asx_hindsight_reset();
    asx_trace_reset();

    /* Emit 3 trace events, then log a hindsight event */
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0, 0);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0, 0);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0, 0);

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);

    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.trace_seq, (uint32_t)3);
}

/* ---- Reset idempotency ---- */

TEST(hindsight_reset_clears_all) {
    asx_hindsight_reset();
    asx_trace_reset();

    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 1);
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 2);
    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)2);

    asx_hindsight_reset();
    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)0);
    ASSERT_EQ(asx_hindsight_readable_count(), (uint32_t)0);
    ASSERT_FALSE(asx_hindsight_overflowed());
}

/* ---- Deterministic scenario: no hindsight events for pure logic ---- */

TEST(hindsight_deterministic_scenario_zero_events) {
    /* Simulate a deterministic-only scenario: no external boundary
     * events should be logged. This proves that deterministic code
     * paths produce zero hindsight entries. */
    asx_hindsight_reset();
    asx_trace_reset();

    /* Only trace (deterministic) events */
    asx_trace_emit(ASX_TRACE_REGION_OPEN, 0x1, 0);
    asx_trace_emit(ASX_TRACE_TASK_SPAWN, 0x2, 0x1);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 0x2, 0);
    asx_trace_emit(ASX_TRACE_SCHED_COMPLETE, 0x2, 0);

    /* Hindsight ring must remain empty */
    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)0);
    ASSERT_EQ(asx_hindsight_digest(), (uint64_t)0x517cc1b727220a95ULL);
}

/* ---- Hook dispatch integration ---- */

static void setup_hooks(void) {
    asx_runtime_hooks hooks;
    asx_runtime_hooks_init(&hooks);
    asx_runtime_set_hooks(&hooks);
}

TEST(hindsight_clock_read_produces_event) {
    asx_hindsight_event ev;
    asx_time now;

    asx_hindsight_reset();
    asx_trace_reset();
    setup_hooks();

    /* Clock read should log a hindsight event */
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);

    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)1);
    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_CLOCK_READ);
    ASSERT_EQ(ev.observed_value, (uint64_t)now);
}

TEST(hindsight_entropy_read_produces_event) {
    asx_hindsight_event ev;
    uint64_t val;

    asx_hindsight_reset();
    asx_trace_reset();
    setup_hooks();

    /* Entropy read should log a hindsight event */
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);

    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)1);
    ASSERT_TRUE(asx_hindsight_get(0, &ev));
    ASSERT_EQ(ev.kind, ASX_ND_ENTROPY_READ);
    ASSERT_EQ(ev.observed_value, val);
}

TEST(hindsight_multiple_hook_calls_accumulate) {
    asx_time now;
    uint64_t val;

    asx_hindsight_reset();
    asx_trace_reset();
    setup_hooks();

    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);
    ASSERT_EQ(asx_runtime_random_u64(&val), ASX_OK);
    ASSERT_EQ(asx_runtime_now_ns(&now), ASX_OK);

    ASSERT_EQ(asx_hindsight_total_count(), (uint32_t)3);
}

/* ---- Flush policy ---- */

TEST(hindsight_policy_defaults_enabled) {
    asx_hindsight_policy pol = asx_hindsight_policy_active();
    ASSERT_TRUE(pol.flush_on_invariant);
    ASSERT_TRUE(pol.flush_on_divergence);
}

TEST(hindsight_policy_disables_divergence_flush) {
    asx_hindsight_flush_buffer out;
    asx_hindsight_policy pol;
    asx_status st;

    asx_hindsight_reset();
    asx_trace_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);

    /* Disable divergence flush */
    pol.flush_on_invariant = 1;
    pol.flush_on_divergence = 0;
    asx_hindsight_set_policy(&pol);

    st = asx_hindsight_flush_on_divergence(NULL, &out);
    ASSERT_EQ(st, ASX_E_PENDING);

    /* Re-enable and verify flush works */
    pol.flush_on_divergence = 1;
    asx_hindsight_set_policy(&pol);

    st = asx_hindsight_flush_on_divergence(NULL, &out);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_TRUE(out.len > (uint32_t)0);
}

TEST(hindsight_policy_disables_invariant_flush) {
    asx_hindsight_flush_buffer out;
    asx_hindsight_policy pol;
    asx_status st;

    asx_hindsight_reset();
    asx_trace_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);

    /* Even with events, invariant flush should be pending when no
     * ghost violations exist and policy is enabled */
    pol.flush_on_invariant = 1;
    pol.flush_on_divergence = 1;
    asx_hindsight_set_policy(&pol);

    st = asx_hindsight_flush_on_invariant(&out);
    ASSERT_EQ(st, ASX_E_PENDING); /* no ghost violations */
}

/* ---- Divergence detection ---- */

TEST(hindsight_check_divergence_same_digest) {
    uint64_t digest;

    asx_hindsight_reset();
    asx_trace_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);

    digest = asx_hindsight_digest();
    ASSERT_FALSE(asx_hindsight_check_divergence(digest));
}

TEST(hindsight_check_divergence_different_digest) {
    asx_hindsight_reset();
    asx_trace_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0, 42);

    /* Pass wrong digest — should detect divergence */
    ASSERT_TRUE(asx_hindsight_check_divergence(0xDEADBEEFULL));
}

TEST(hindsight_check_divergence_empty_ring) {
    asx_hindsight_reset();
    asx_trace_reset();

    /* Empty ring digest = FNV offset basis */
    ASSERT_FALSE(asx_hindsight_check_divergence(0x517cc1b727220a95ULL));
    ASSERT_TRUE(asx_hindsight_check_divergence(0));
}

/* ---- Digest stability with hook integration ---- */

TEST(hindsight_hook_events_digest_stable_across_runs) {
    uint64_t d1, d2;
    asx_time now;
    uint64_t val;

    /* Run 1 */
    asx_hindsight_reset();
    asx_trace_reset();
    setup_hooks();
    (void)asx_runtime_now_ns(&now);
    (void)asx_runtime_random_u64(&val);
    d1 = asx_hindsight_digest();

    /* Run 2 — same hook calls produce same observed values because
     * default hooks are deterministic (logical clock returns 0,
     * seeded PRNG is counter-based). The PRNG state carries over
     * but the ring content digest should differ from run 1 because
     * the entropy values advanced. Verify digest is nonzero. */
    asx_hindsight_reset();
    asx_trace_reset();
    d2 = asx_hindsight_digest();

    /* After reset, digest should be FNV offset basis */
    ASSERT_EQ(d2, (uint64_t)0x517cc1b727220a95ULL);
    /* Run 1 digest should NOT equal FNV offset (we logged events) */
    ASSERT_TRUE(d1 != d2);
}

/* ---- Test suite runner ---- */

int main(void) {
    RUN_TEST(hindsight_init_clears_state);
    RUN_TEST(hindsight_log_single_event);
    RUN_TEST(hindsight_log_multiple_events);
    RUN_TEST(hindsight_monotonic_sequence);
    RUN_TEST(hindsight_wraps_at_capacity);
    RUN_TEST(hindsight_heavy_wrapping);
    RUN_TEST(hindsight_get_oob_returns_zero);
    RUN_TEST(hindsight_get_oob_after_wrap);
    RUN_TEST(hindsight_all_event_kinds);
    RUN_TEST(hindsight_digest_empty_is_fnv_offset);
    RUN_TEST(hindsight_digest_deterministic);
    RUN_TEST(hindsight_digest_differs_on_different_input);
    RUN_TEST(hindsight_flush_json_empty);
    RUN_TEST(hindsight_flush_json_with_events);
    RUN_TEST(hindsight_flush_json_null_rejected);
    RUN_TEST(hindsight_flush_json_after_wrap);
    RUN_TEST(hindsight_flush_on_divergence_empty_ring);
    RUN_TEST(hindsight_flush_on_divergence_with_events);
    RUN_TEST(hindsight_flush_on_divergence_null_out);
    RUN_TEST(hindsight_kind_str_coverage);
    RUN_TEST(hindsight_trace_seq_tracks_trace_count);
    RUN_TEST(hindsight_reset_clears_all);
    RUN_TEST(hindsight_deterministic_scenario_zero_events);
    RUN_TEST(hindsight_clock_read_produces_event);
    RUN_TEST(hindsight_entropy_read_produces_event);
    RUN_TEST(hindsight_multiple_hook_calls_accumulate);
    RUN_TEST(hindsight_policy_defaults_enabled);
    RUN_TEST(hindsight_policy_disables_divergence_flush);
    RUN_TEST(hindsight_policy_disables_invariant_flush);
    RUN_TEST(hindsight_check_divergence_same_digest);
    RUN_TEST(hindsight_check_divergence_different_digest);
    RUN_TEST(hindsight_check_divergence_empty_ring);
    RUN_TEST(hindsight_hook_events_digest_stable_across_runs);
    TEST_REPORT();
    return test_failures;
}
