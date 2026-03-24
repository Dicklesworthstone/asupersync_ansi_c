/*
 * test_telemetry.c — unit tests for multi-tier telemetry modes
 *
 * Tests: tier selection, event retention policy, rolling digest
 * tier-independence, filtered event counting, and statistics.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE

/* ---- Tier configuration ---- */

TEST(telemetry_default_tier_is_forensic) {
    asx_telemetry_reset();
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
}

TEST(telemetry_set_tier_valid) {
    asx_status st;

    asx_telemetry_reset();

    st = asx_telemetry_set_tier(ASX_TELEMETRY_OPS_LIGHT);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_OPS_LIGHT);

    st = asx_telemetry_set_tier(ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_ULTRA_MIN);

    st = asx_telemetry_set_tier(ASX_TELEMETRY_FORENSIC);
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
}

TEST(telemetry_set_tier_invalid) {
    asx_status st;
    asx_telemetry_reset();
    st = asx_telemetry_set_tier((asx_telemetry_tier)99);
    ASSERT_EQ(st, ASX_E_INVALID_ARGUMENT);
    /* Tier should remain unchanged */
    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
}

/* ---- Tier string ---- */

TEST(telemetry_tier_str_coverage) {
    ASSERT_TRUE(strcmp(asx_telemetry_tier_str(ASX_TELEMETRY_FORENSIC), "forensic") == 0);
    ASSERT_TRUE(strcmp(asx_telemetry_tier_str(ASX_TELEMETRY_OPS_LIGHT), "ops_light") == 0);
    ASSERT_TRUE(strcmp(asx_telemetry_tier_str(ASX_TELEMETRY_ULTRA_MIN), "ultra_min") == 0);
    ASSERT_TRUE(strcmp(asx_telemetry_tier_str((asx_telemetry_tier)99), "unknown") == 0);
}

/* ---- Forensic tier: all events recorded ---- */

TEST(telemetry_forensic_records_all) {
    asx_telemetry_reset();
    asx_trace_reset();

    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_CHANNEL_SEND, 0x30, 0);
    asx_telemetry_emit(ASX_TRACE_TIMER_SET, 0x40, 100);
    asx_telemetry_emit(ASX_TRACE_OBLIGATION_RESERVE, 0x50, 0);

    /* All 5 events should be in the trace ring */
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)5);
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)5);
    ASSERT_EQ(asx_telemetry_filtered_count(), (uint32_t)0);
}

/* ---- OPS_LIGHT tier: only lifecycle events retained ---- */

TEST(telemetry_ops_light_filters_polls) {
    asx_status st;

    asx_telemetry_reset();
    asx_trace_reset();

    st = asx_telemetry_set_tier(ASX_TELEMETRY_OPS_LIGHT);
    ASSERT_EQ(st, ASX_OK);

    /* These should be retained */
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_REGION_CLOSE, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_REGION_CLOSED, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_TRANSITION, 0x20, 1);
    asx_telemetry_emit(ASX_TRACE_SCHED_QUIESCENT, 0, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_BUDGET, 0, 10);

    /* These should be filtered */
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_ROUND, 0, 1);
    asx_telemetry_emit(ASX_TRACE_CHANNEL_SEND, 0x30, 0);
    asx_telemetry_emit(ASX_TRACE_CHANNEL_RECV, 0x30, 0);
    asx_telemetry_emit(ASX_TRACE_TIMER_SET, 0x40, 100);
    asx_telemetry_emit(ASX_TRACE_TIMER_FIRE, 0x40, 0);
    asx_telemetry_emit(ASX_TRACE_TIMER_CANCEL, 0x40, 0);
    asx_telemetry_emit(ASX_TRACE_OBLIGATION_RESERVE, 0x50, 0);
    asx_telemetry_emit(ASX_TRACE_OBLIGATION_COMMIT, 0x50, 0);
    asx_telemetry_emit(ASX_TRACE_OBLIGATION_ABORT, 0x50, 0);

    /* 8 retained + 10 filtered = 18 total */
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)18);
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)8);
    ASSERT_EQ(asx_telemetry_filtered_count(), (uint32_t)10);
}

/* ---- ULTRA_MIN tier: no events stored ---- */

TEST(telemetry_ultra_min_stores_nothing) {
    asx_status st;

    asx_telemetry_reset();
    asx_trace_reset();

    st = asx_telemetry_set_tier(ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(st, ASX_OK);

    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);

    /* Zero events in trace ring */
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)0);
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)4);
    ASSERT_EQ(asx_telemetry_filtered_count(), (uint32_t)4);
}

/* ---- Digest tier-independence (critical invariant) ---- */

TEST(telemetry_digest_identical_across_tiers) {
    uint64_t d_forensic, d_ops, d_ultra;
    asx_status st;

    /* Run same scenario at FORENSIC tier */
    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_REGION_CLOSE, 0x10, 0);
    d_forensic = asx_telemetry_digest();

    /* Same scenario at OPS_LIGHT tier */
    asx_telemetry_reset();
    asx_trace_reset();
    st = asx_telemetry_set_tier(ASX_TELEMETRY_OPS_LIGHT);
    ASSERT_EQ(st, ASX_OK);
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_REGION_CLOSE, 0x10, 0);
    d_ops = asx_telemetry_digest();

    /* Same scenario at ULTRA_MIN tier */
    asx_telemetry_reset();
    asx_trace_reset();
    st = asx_telemetry_set_tier(ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(st, ASX_OK);
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_REGION_CLOSE, 0x10, 0);
    d_ultra = asx_telemetry_digest();

    /* All three digests must be identical */
    ASSERT_EQ(d_forensic, d_ops);
    ASSERT_EQ(d_ops, d_ultra);
}

TEST(telemetry_digest_deterministic_across_runs) {
    uint64_t d1, d2;

    /* Run 1 */
    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x1, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x1, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x1, 0);
    d1 = asx_telemetry_digest();

    /* Run 2 — identical */
    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x1, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x1, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x1, 0);
    d2 = asx_telemetry_digest();

    ASSERT_EQ(d1, d2);
}

TEST(telemetry_digest_differs_on_different_input) {
    uint64_t d1, d2;

    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x1, 0);
    d1 = asx_telemetry_digest();

    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x2, 0);
    d2 = asx_telemetry_digest();

    ASSERT_TRUE(d1 != d2);
}

TEST(telemetry_digest_empty_is_fnv_offset) {
    asx_telemetry_reset();
    ASSERT_EQ(asx_telemetry_digest(), (uint64_t)0x517cc1b727220a95ULL);
}

/* ---- Retention query ---- */

TEST(telemetry_retains_forensic_all) {
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_FORENSIC, ASX_TRACE_SCHED_POLL));
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_FORENSIC, ASX_TRACE_CHANNEL_SEND));
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_FORENSIC, ASX_TRACE_TIMER_SET));
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_FORENSIC, ASX_TRACE_OBLIGATION_RESERVE));
}

TEST(telemetry_retains_ops_light_selective) {
    /* Retained */
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_OPS_LIGHT, ASX_TRACE_REGION_OPEN));
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_OPS_LIGHT, ASX_TRACE_TASK_SPAWN));
    ASSERT_TRUE(asx_telemetry_retains(ASX_TELEMETRY_OPS_LIGHT, ASX_TRACE_SCHED_COMPLETE));

    /* Filtered */
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_OPS_LIGHT, ASX_TRACE_SCHED_POLL));
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_OPS_LIGHT, ASX_TRACE_CHANNEL_SEND));
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_OPS_LIGHT, ASX_TRACE_TIMER_SET));
}

TEST(telemetry_retains_ultra_min_none) {
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_ULTRA_MIN, ASX_TRACE_SCHED_POLL));
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_ULTRA_MIN, ASX_TRACE_REGION_OPEN));
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_ULTRA_MIN, ASX_TRACE_TASK_SPAWN));
    ASSERT_FALSE(asx_telemetry_retains(ASX_TELEMETRY_ULTRA_MIN, ASX_TRACE_TIMER_FIRE));
}

/* ---- Reset ---- */

TEST(telemetry_reset_clears_all) {
    asx_status st;

    asx_telemetry_reset();
    asx_trace_reset();

    st = asx_telemetry_set_tier(ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(st, ASX_OK);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0, 0);

    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)1);

    asx_telemetry_reset();

    ASSERT_EQ(asx_telemetry_get_tier(), ASX_TELEMETRY_FORENSIC);
    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)0);
    ASSERT_EQ(asx_telemetry_filtered_count(), (uint32_t)0);
    ASSERT_EQ(asx_telemetry_digest(), (uint64_t)0x517cc1b727220a95ULL);
}

/* ---- Mixed-tier scenario ---- */

TEST(telemetry_mid_scenario_tier_switch) {
    uint64_t d_pure, d_switched;
    asx_status st;

    /* Pure forensic run */
    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);
    d_pure = asx_telemetry_digest();

    /* Switch tier mid-scenario — digest must still match */
    asx_telemetry_reset();
    asx_trace_reset();
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, 0x10, 0);
    st = asx_telemetry_set_tier(ASX_TELEMETRY_OPS_LIGHT);
    ASSERT_EQ(st, ASX_OK);
    asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, 0x20, 0x10);
    st = asx_telemetry_set_tier(ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(st, ASX_OK);
    asx_telemetry_emit(ASX_TRACE_SCHED_POLL, 0x20, 0);
    asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, 0x20, 0);
    d_switched = asx_telemetry_digest();

    /* Rolling digest is tier-independent */
    ASSERT_EQ(d_pure, d_switched);
}

/* ---- Large-volume filtered scenario ---- */

TEST(telemetry_high_volume_filtered) {
    uint32_t i;
    asx_status st;

    asx_telemetry_reset();
    asx_trace_reset();

    st = asx_telemetry_set_tier(ASX_TELEMETRY_ULTRA_MIN);
    ASSERT_EQ(st, ASX_OK);

    for (i = 0; i < 1000; i++) { asx_telemetry_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, 0); }

    ASSERT_EQ(asx_telemetry_emitted_count(), (uint32_t)1000);
    ASSERT_EQ(asx_telemetry_filtered_count(), (uint32_t)1000);
    ASSERT_EQ(asx_trace_event_count(), (uint32_t)0);

    /* Digest must still be non-trivial (not the offset basis) */
    ASSERT_TRUE(asx_telemetry_digest() != (uint64_t)0x517cc1b727220a95ULL);
}

/* ---- Suite runner ---- */

int main(void) {
    RUN_TEST(telemetry_default_tier_is_forensic);
    RUN_TEST(telemetry_set_tier_valid);
    RUN_TEST(telemetry_set_tier_invalid);
    RUN_TEST(telemetry_tier_str_coverage);
    RUN_TEST(telemetry_forensic_records_all);
    RUN_TEST(telemetry_ops_light_filters_polls);
    RUN_TEST(telemetry_ultra_min_stores_nothing);
    RUN_TEST(telemetry_digest_identical_across_tiers);
    RUN_TEST(telemetry_digest_deterministic_across_runs);
    RUN_TEST(telemetry_digest_differs_on_different_input);
    RUN_TEST(telemetry_digest_empty_is_fnv_offset);
    RUN_TEST(telemetry_retains_forensic_all);
    RUN_TEST(telemetry_retains_ops_light_selective);
    RUN_TEST(telemetry_retains_ultra_min_none);
    RUN_TEST(telemetry_reset_clears_all);
    RUN_TEST(telemetry_mid_scenario_tier_switch);
    RUN_TEST(telemetry_high_volume_filtered);
    TEST_REPORT();
    return test_failures;
}

#else

TEST(telemetry_family_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT, 1);
}

int main(void) {
    RUN_TEST(telemetry_family_compile_time_hidden_in_minimal_browser);
    TEST_REPORT();
    return test_failures;
}

#endif
