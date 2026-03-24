/*
 * test_regression_localize.c — Replay-guided regression localization
 * and deterministic circuit-breaker tests (bd-3vt.5)
 *
 * Validates:
 * - Subsystem classification of trace events
 * - Snapshot building from trace ring
 * - Regression detection and blame ranking
 * - Circuit-breaker state machine transitions
 * - Circuit-breaker determinism (identical inputs -> identical state)
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- regression localize test, no checkpoint coverage needed */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/regression_localize.h>
#include <asx/runtime/trace.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void emit_scheduler_events(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) { asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, (uint64_t)i); }
}

static void emit_lifecycle_events(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) { asx_trace_emit(ASX_TRACE_REGION_OPEN, (uint64_t)i, 0); }
}

static void emit_channel_events(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) { asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)i, 0); }
}

static uint64_t transition_aux(asx_task_state from, asx_task_state to) {
    return ((uint64_t)(uint32_t)from << 32) | (uint64_t)(uint32_t)to;
}

static void emit_cancel_transition_events(uint32_t count) {
    uint32_t i;
    for (i = 0; i < count; i++) {
        asx_trace_emit(ASX_TRACE_TASK_TRANSITION, (uint64_t)i,
                       transition_aux(ASX_TASK_RUNNING, ASX_TASK_CANCEL_REQUESTED));
    }
}

/* ------------------------------------------------------------------ */
/* Test: subsystem classification                                      */
/* ------------------------------------------------------------------ */

TEST(classify_sched_poll) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_SCHED_POLL), (int)ASX_SUBSYS_SCHEDULER);
}

TEST(classify_sched_complete) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_SCHED_COMPLETE), (int)ASX_SUBSYS_SCHEDULER);
}

TEST(classify_region_open) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_REGION_OPEN), (int)ASX_SUBSYS_LIFECYCLE);
}

TEST(classify_task_spawn) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_TASK_SPAWN), (int)ASX_SUBSYS_LIFECYCLE);
}

TEST(classify_obligation) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_OBLIGATION_RESERVE),
              (int)ASX_SUBSYS_OBLIGATION);
}

TEST(classify_channel) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_CHANNEL_SEND), (int)ASX_SUBSYS_CHANNEL);
}

TEST(classify_timer) {
    ASSERT_EQ((int)asx_trace_event_subsystem(ASX_TRACE_TIMER_SET), (int)ASX_SUBSYS_TIMER);
}

/* ------------------------------------------------------------------ */
/* Test: subsystem names                                               */
/* ------------------------------------------------------------------ */

TEST(subsystem_names) {
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_SCHEDULER), "scheduler");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_LIFECYCLE), "lifecycle");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_OBLIGATION), "obligation");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_CHANNEL), "channel");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_TIMER), "timer");
    ASSERT_STR_EQ(asx_subsystem_name(ASX_SUBSYS_UNKNOWN), "unknown");
}

/* ------------------------------------------------------------------ */
/* Test: snapshot building                                             */
/* ------------------------------------------------------------------ */

TEST(snapshot_empty_trace) {
    asx_perf_snapshot snap;

    asx_trace_reset();
    asx_perf_snapshot_build(&snap);

    ASSERT_EQ(snap.total_events, (uint32_t)0);
}

TEST(snapshot_counts_events) {
    asx_perf_snapshot snap;

    asx_trace_reset();
    emit_scheduler_events(10);
    emit_lifecycle_events(5);
    emit_channel_events(3);

    asx_perf_snapshot_build(&snap);

    ASSERT_EQ(snap.total_events, (uint32_t)18);
    ASSERT_EQ(snap.subsystems[ASX_SUBSYS_SCHEDULER].event_count, (uint32_t)10);
    ASSERT_EQ(snap.subsystems[ASX_SUBSYS_LIFECYCLE].event_count, (uint32_t)5);
    ASSERT_EQ(snap.subsystems[ASX_SUBSYS_CHANNEL].event_count, (uint32_t)3);
}

TEST(snapshot_accumulates_aux) {
    asx_perf_snapshot snap;

    asx_trace_reset();
    /* SCHED_POLL events with aux = index: 0+1+2+3+4 = 10 */
    emit_scheduler_events(5);

    asx_perf_snapshot_build(&snap);

    ASSERT_EQ(snap.subsystems[ASX_SUBSYS_SCHEDULER].total_aux, (uint64_t)10);
}

TEST(snapshot_classifies_cancel_transitions) {
    asx_perf_snapshot snap;

    asx_trace_reset();
    emit_cancel_transition_events(4);

    asx_perf_snapshot_build(&snap);

    ASSERT_EQ(snap.subsystems[ASX_SUBSYS_CANCEL].event_count, (uint32_t)4);
    ASSERT_EQ(snap.subsystems[ASX_SUBSYS_LIFECYCLE].event_count, (uint32_t)0);
}

TEST(snapshot_null_safety) { asx_perf_snapshot_build(NULL); /* should not crash */ }

/* ------------------------------------------------------------------ */
/* Test: regression localization                                       */
/* ------------------------------------------------------------------ */

TEST(no_regression_on_identical_snapshots) {
    asx_perf_snapshot baseline;
    asx_perf_snapshot current;
    asx_regression_report report;

    asx_trace_reset();
    emit_scheduler_events(10);
    asx_perf_snapshot_build(&baseline);

    /* Same trace — build again */
    memcpy(&current, &baseline, sizeof(current));
    asx_regression_localize(&baseline, &current, NULL, &report);

    ASSERT_EQ(report.regressed, 0);
    ASSERT_EQ(report.suspect_count, (uint32_t)0);
}

TEST(regression_detected_on_different_digests) {
    asx_perf_snapshot baseline;
    asx_perf_snapshot current;
    asx_regression_report report;

    /* Build baseline */
    asx_trace_reset();
    emit_scheduler_events(10);
    asx_perf_snapshot_build(&baseline);

    /* Build different current */
    asx_trace_reset();
    emit_scheduler_events(15);
    emit_lifecycle_events(3);
    asx_perf_snapshot_build(&current);

    asx_regression_localize(&baseline, &current, NULL, &report);

    ASSERT_EQ(report.regressed, 1);
    ASSERT_TRUE(report.suspect_count > 0);
}

TEST(regression_blames_scheduler_for_extra_polls) {
    asx_perf_snapshot baseline;
    asx_perf_snapshot current;
    asx_regression_report report;

    /* Baseline: 10 scheduler events */
    asx_trace_reset();
    emit_scheduler_events(10);
    asx_perf_snapshot_build(&baseline);

    /* Current: 20 scheduler events (regression) */
    asx_trace_reset();
    emit_scheduler_events(20);
    asx_perf_snapshot_build(&current);

    asx_regression_localize(&baseline, &current, NULL, &report);

    ASSERT_EQ(report.regressed, 1);
    ASSERT_TRUE(report.suspect_count > 0);
    ASSERT_EQ((int)report.suspects[0].id, (int)ASX_SUBSYS_SCHEDULER);
    ASSERT_EQ(report.suspects[0].event_delta, 10);
}

TEST(regression_includes_replay_divergence) {
    asx_perf_snapshot baseline;
    asx_perf_snapshot current;
    asx_replay_result replay;
    asx_regression_report report;

    /* Build baseline */
    asx_trace_reset();
    emit_scheduler_events(10);
    asx_perf_snapshot_build(&baseline);

    /* Build different current */
    asx_trace_reset();
    emit_scheduler_events(10);
    emit_lifecycle_events(5);
    asx_perf_snapshot_build(&current);

    /* Simulate replay divergence */
    memset(&replay, 0, sizeof(replay));
    replay.result = ASX_REPLAY_KIND_MISMATCH;
    replay.divergence_index = 10;

    asx_regression_localize(&baseline, &current, &replay, &report);

    ASSERT_EQ(report.regressed, 1);
    ASSERT_EQ(report.divergence_index, (uint32_t)10);
    ASSERT_EQ((int)report.divergence_kind, (int)ASX_REPLAY_KIND_MISMATCH);
}

TEST(regression_blames_cancel_subsystem_for_cancel_growth) {
    asx_perf_snapshot baseline;
    asx_perf_snapshot current;
    asx_regression_report report;

    asx_trace_reset();
    emit_cancel_transition_events(1);
    asx_perf_snapshot_build(&baseline);

    asx_trace_reset();
    emit_cancel_transition_events(6);
    asx_perf_snapshot_build(&current);

    asx_regression_localize(&baseline, &current, NULL, &report);

    ASSERT_EQ(report.regressed, 1);
    ASSERT_TRUE(report.suspect_count > 0);
    ASSERT_EQ((int)report.suspects[0].id, (int)ASX_SUBSYS_CANCEL);
    ASSERT_EQ(report.suspects[0].event_delta, 5);
}

TEST(regression_null_safety) {
    asx_regression_report report;

    asx_regression_localize(NULL, NULL, NULL, &report);
    ASSERT_EQ(report.regressed, 0);

    asx_regression_localize(NULL, NULL, NULL, NULL); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Test: circuit-breaker state machine                                 */
/* ------------------------------------------------------------------ */

TEST(cb_starts_closed) {
    asx_cb_context ctx;
    asx_cb_init(&ctx);

    ASSERT_EQ((int)ctx.state, (int)ASX_CB_CLOSED);
    ASSERT_TRUE(asx_cb_allows(&ctx));
}

TEST(cb_trips_after_threshold_failures) {
    asx_cb_context ctx;
    asx_cb_config cfg = {3, 2, 10};

    asx_cb_init(&ctx);

    asx_cb_record_failure(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_CLOSED);

    asx_cb_record_failure(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_CLOSED);

    asx_cb_record_failure(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_OPEN);
    ASSERT_TRUE(!asx_cb_allows(&ctx));
    ASSERT_EQ(ctx.total_trips, (uint32_t)1);
}

TEST(cb_success_resets_failure_count) {
    asx_cb_context ctx;
    asx_cb_config cfg = {3, 2, 10};

    asx_cb_init(&ctx);

    asx_cb_record_failure(&ctx, &cfg);
    asx_cb_record_failure(&ctx, &cfg);
    asx_cb_record_success(&ctx, &cfg);
    ASSERT_EQ(ctx.consecutive_failures, (uint32_t)0);

    /* Need 3 more failures to trip */
    asx_cb_record_failure(&ctx, &cfg);
    asx_cb_record_failure(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_CLOSED);
}

TEST(cb_cooldown_transitions_to_half_open) {
    asx_cb_context ctx;
    asx_cb_config cfg = {1, 2, 5};
    uint32_t i;

    asx_cb_init(&ctx);
    asx_cb_record_failure(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_OPEN);

    /* Tick 4 times — still OPEN */
    for (i = 0; i < 4; i++) asx_cb_tick(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_OPEN);

    /* Tick once more — transitions to HALF_OPEN */
    asx_cb_tick(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_HALF_OPEN);
    ASSERT_TRUE(asx_cb_allows(&ctx));
}

TEST(cb_half_open_recovers_after_probes) {
    asx_cb_context ctx;
    asx_cb_config cfg = {1, 2, 5};
    uint32_t i;

    asx_cb_init(&ctx);
    asx_cb_record_failure(&ctx, &cfg);
    for (i = 0; i < 5; i++) asx_cb_tick(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_HALF_OPEN);

    asx_cb_record_success(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_HALF_OPEN);

    asx_cb_record_success(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_CLOSED);
    ASSERT_EQ(ctx.total_recoveries, (uint32_t)1);
}

TEST(cb_half_open_failure_reopens) {
    asx_cb_context ctx;
    asx_cb_config cfg = {1, 2, 5};
    uint32_t i;

    asx_cb_init(&ctx);
    asx_cb_record_failure(&ctx, &cfg);
    for (i = 0; i < 5; i++) asx_cb_tick(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_HALF_OPEN);

    asx_cb_record_failure(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_OPEN);
    ASSERT_EQ(ctx.total_trips, (uint32_t)2);
}

TEST(cb_determinism) {
    asx_cb_context ctx1, ctx2;
    asx_cb_config cfg = {2, 1, 3};
    uint32_t i;

    /* Identical sequence on two contexts */
    asx_cb_init(&ctx1);
    asx_cb_init(&ctx2);

    asx_cb_record_failure(&ctx1, &cfg);
    asx_cb_record_failure(&ctx2, &cfg);

    asx_cb_record_failure(&ctx1, &cfg);
    asx_cb_record_failure(&ctx2, &cfg);

    for (i = 0; i < 3; i++) {
        asx_cb_tick(&ctx1, &cfg);
        asx_cb_tick(&ctx2, &cfg);
    }

    asx_cb_record_success(&ctx1, &cfg);
    asx_cb_record_success(&ctx2, &cfg);

    /* Must produce identical state */
    ASSERT_EQ((int)ctx1.state, (int)ctx2.state);
    ASSERT_EQ(ctx1.total_trips, ctx2.total_trips);
    ASSERT_EQ(ctx1.total_recoveries, ctx2.total_recoveries);
    ASSERT_EQ(ctx1.consecutive_failures, ctx2.consecutive_failures);
}

TEST(cb_null_safety) {
    asx_cb_config cfg = {3, 2, 10};

    asx_cb_init(NULL);
    asx_cb_record_success(NULL, &cfg);
    asx_cb_record_failure(NULL, &cfg);
    asx_cb_tick(NULL, &cfg);
    ASSERT_EQ(asx_cb_allows(NULL), 0);
}

TEST(cb_state_names) {
    ASSERT_STR_EQ(asx_cb_state_name(ASX_CB_CLOSED), "closed");
    ASSERT_STR_EQ(asx_cb_state_name(ASX_CB_OPEN), "open");
    ASSERT_STR_EQ(asx_cb_state_name(ASX_CB_HALF_OPEN), "half-open");
}

/* ------------------------------------------------------------------ */
/* Test: full workflow — regression triggers circuit-breaker           */
/* ------------------------------------------------------------------ */

TEST(workflow_regression_trips_breaker) {
    asx_perf_snapshot baseline;
    asx_perf_snapshot current;
    asx_regression_report report;
    asx_cb_context ctx;
    asx_cb_config cfg = {2, 1, 5};
    uint32_t i;

    asx_cb_init(&ctx);

    /* Build baseline */
    asx_trace_reset();
    emit_scheduler_events(10);
    asx_perf_snapshot_build(&baseline);

    /* Simulate 2 regressed runs — breaker should trip */
    for (i = 0; i < 2; i++) {
        asx_trace_reset();
        emit_scheduler_events(20);
        asx_perf_snapshot_build(&current);
        asx_regression_localize(&baseline, &current, NULL, &report);

        if (report.regressed) {
            asx_cb_record_failure(&ctx, &cfg);
        } else {
            asx_cb_record_success(&ctx, &cfg);
        }
    }

    ASSERT_EQ((int)ctx.state, (int)ASX_CB_OPEN);
    ASSERT_TRUE(!asx_cb_allows(&ctx));

    /* Cooldown */
    for (i = 0; i < 5; i++) asx_cb_tick(&ctx, &cfg);
    ASSERT_EQ((int)ctx.state, (int)ASX_CB_HALF_OPEN);

    /* Probe with good run */
    asx_trace_reset();
    emit_scheduler_events(10);
    asx_perf_snapshot_build(&current);
    asx_regression_localize(&baseline, &current, NULL, &report);

    if (!report.regressed) { asx_cb_record_success(&ctx, &cfg); }

    ASSERT_EQ((int)ctx.state, (int)ASX_CB_CLOSED);
    ASSERT_EQ(ctx.total_trips, (uint32_t)1);
    ASSERT_EQ(ctx.total_recoveries, (uint32_t)1);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== regression localize tests (bd-3vt.5) ===\n");

    /* Classification */
    RUN_TEST(classify_sched_poll);
    RUN_TEST(classify_sched_complete);
    RUN_TEST(classify_region_open);
    RUN_TEST(classify_task_spawn);
    RUN_TEST(classify_obligation);
    RUN_TEST(classify_channel);
    RUN_TEST(classify_timer);
    RUN_TEST(subsystem_names);

    /* Snapshot */
    RUN_TEST(snapshot_empty_trace);
    RUN_TEST(snapshot_counts_events);
    RUN_TEST(snapshot_accumulates_aux);
    RUN_TEST(snapshot_classifies_cancel_transitions);
    RUN_TEST(snapshot_null_safety);

    /* Regression */
    RUN_TEST(no_regression_on_identical_snapshots);
    RUN_TEST(regression_detected_on_different_digests);
    RUN_TEST(regression_blames_scheduler_for_extra_polls);
    RUN_TEST(regression_includes_replay_divergence);
    RUN_TEST(regression_blames_cancel_subsystem_for_cancel_growth);
    RUN_TEST(regression_null_safety);

    /* Circuit-breaker */
    RUN_TEST(cb_starts_closed);
    RUN_TEST(cb_trips_after_threshold_failures);
    RUN_TEST(cb_success_resets_failure_count);
    RUN_TEST(cb_cooldown_transitions_to_half_open);
    RUN_TEST(cb_half_open_recovers_after_probes);
    RUN_TEST(cb_half_open_failure_reopens);
    RUN_TEST(cb_determinism);
    RUN_TEST(cb_null_safety);
    RUN_TEST(cb_state_names);

    /* Workflow */
    RUN_TEST(workflow_regression_trips_breaker);

    TEST_REPORT();
    return test_failures;
}

#else

TEST(regression_localize_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT, 1);
}

int main(void) {
    fprintf(stderr, "=== regression localize hidden contract ===\n");
    RUN_TEST(regression_localize_compile_time_hidden_in_minimal_browser);
    TEST_REPORT();
    return test_failures;
}

#endif
