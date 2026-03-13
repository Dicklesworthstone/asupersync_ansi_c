/*
 * e2e_continuity_restart.c — e2e scenarios for crash/restart replay continuity
 *
 * Exercises: trace export/import round-trip, continuity check after
 * restart simulation, multi-task trace persistence, digest identity
 * across replays, corrupted trace detection, and snapshot capture.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <stdio.h>
#include <string.h>

/* Suppress warn_unused_result for intentionally-ignored calls */
#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* Yield-once poll */
typedef struct {
    asx_co_state co;
} yield_state;

static asx_status poll_yield_once(void *ud, asx_task_id self) {
    yield_state *s = (yield_state *)ud;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    ASX_CO_YIELD(&s->co);
    ASX_CO_END(&s->co);
}

static void emit_restart_signature(uint64_t scenario_tag, uint32_t task_count) {
    uint32_t i;

    IGNORE_RC(asx_telemetry_set_tier(ASX_TELEMETRY_FORENSIC));
    asx_telemetry_emit(ASX_TRACE_REGION_OPEN, scenario_tag, task_count);
    for (i = 0; i < task_count; i++) {
        uint64_t entity = scenario_tag + (uint64_t)i + 1u;
        asx_telemetry_emit(ASX_TRACE_TASK_SPAWN, entity, scenario_tag);
        asx_telemetry_emit(ASX_TRACE_SCHED_POLL, entity, (uint64_t)i);
        asx_telemetry_emit(ASX_TRACE_SCHED_COMPLETE, entity, 0u);
    }
    asx_telemetry_emit(ASX_TRACE_SCHED_QUIESCENT, scenario_tag, task_count);
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* continuity-crash-midflight-001: trace export → import → continuity check */
static void scenario_crash_restart_simple(void) {
    SCENARIO_BEGIN("continuity-crash-midflight-001.simple_restart");

    /* Phase 1: run a scenario and export trace */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");
    emit_restart_signature(0xC110u, 1u);

    uint64_t pre_crash_digest = asx_trace_digest();
    SCENARIO_CHECK(pre_crash_digest != 0, "pre-crash digest should not be zero");

    /* Export trace to binary */
    uint8_t trace_buf[8192];
    uint32_t trace_len = 0;
    asx_status rc = asx_trace_export_binary(trace_buf, (uint32_t)sizeof(trace_buf), &trace_len);
    SCENARIO_CHECK(rc == ASX_OK, "trace_export");
    SCENARIO_CHECK(trace_len > 0, "exported trace should not be empty");

    /* Phase 2: simulate restart — reset everything, import trace */
    asx_runtime_reset();
    asx_trace_reset();

    rc = asx_trace_import_binary(trace_buf, trace_len);
    SCENARIO_CHECK(rc == ASX_OK, "trace_import");

    /* Replay identical scenario */
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_2");

    budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run_2");
    emit_restart_signature(0xC110u, 1u);

    {
        asx_status continuity_rc = asx_trace_continuity_check(trace_buf, trace_len);
        SCENARIO_CHECK(continuity_rc == ASX_OK, "post-restart continuity should match");
    }

    SCENARIO_END();
}

/* continuity-checkpoint-rollback-002: continuity check detects matching trace */
static void scenario_continuity_check_matching(void) {
    SCENARIO_BEGIN("continuity-checkpoint-rollback-002.matching_check");

    /* Run scenario and capture trace */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");
    emit_restart_signature(0xC120u, 1u);

    /* Export trace */
    uint8_t trace_buf[8192];
    uint32_t trace_len = 0;
    SCENARIO_CHECK(asx_trace_export_binary(trace_buf, (uint32_t)sizeof(trace_buf), &trace_len) ==
                       ASX_OK,
                   "trace_export");

    /* Reset and replay same scenario */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_2");

    budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run_2");
    emit_restart_signature(0xC120u, 1u);

    /* Continuity check should pass (traces match) */
    asx_status rc = asx_trace_continuity_check(trace_buf, trace_len);
    SCENARIO_CHECK(rc == ASX_OK, "continuity_check should pass for matching trace");

    SCENARIO_END();
}

/* continuity-replay-identity-003: different scenario detected as mismatch */
static void scenario_continuity_mismatch_detected(void) {
    SCENARIO_BEGIN("continuity-replay-identity-003.mismatch_detected");

    /* Run scenario A: single task */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_a");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_a");

    asx_budget budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_a");
    emit_restart_signature(0xC130u, 1u);

    /* Export trace A */
    uint8_t trace_buf_a[8192];
    uint32_t trace_len_a = 0;
    SCENARIO_CHECK(asx_trace_export_binary(trace_buf_a, (uint32_t)sizeof(trace_buf_a),
                                           &trace_len_a) == ASX_OK,
                   "export_a");

    /* Run scenario B: two tasks (different event sequence) */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_b");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_b1");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_b2");

    budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_b");
    emit_restart_signature(0xC131u, 2u);

    /* Continuity check against trace A should fail (different scenario) */
    asx_status rc = asx_trace_continuity_check(trace_buf_a, trace_len_a);
    SCENARIO_CHECK(rc != ASX_OK, "continuity_check should detect mismatch");

    SCENARIO_END();
}

/* Multi-task trace round-trip */
static void scenario_multi_task_roundtrip(void) {
    SCENARIO_BEGIN("continuity-multi-task.trace_roundtrip");

    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id t1, t2;
    void *s1, *s2;
    yield_state *ys;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_yield_once, (uint32_t)sizeof(yield_state),
                                           NULL, &t1, &s1) == ASX_OK,
                   "spawn_t1");
    ys = (yield_state *)s1;
    ys->co.line = 0;

    SCENARIO_CHECK(asx_task_spawn_captured(rid, poll_yield_once, (uint32_t)sizeof(yield_state),
                                           NULL, &t2, &s2) == ASX_OK,
                   "spawn_t2");
    ys = (yield_state *)s2;
    ys->co.line = 0;

    asx_budget budget = asx_budget_from_polls(20);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");
    emit_restart_signature(0xC140u, 2u);

    uint32_t event_count = asx_trace_event_count();
    SCENARIO_CHECK(event_count > 0, "should have trace events");

    /* Export and reimport */
    uint8_t trace_buf[8192];
    uint32_t trace_len = 0;
    SCENARIO_CHECK(asx_trace_export_binary(trace_buf, (uint32_t)sizeof(trace_buf), &trace_len) ==
                       ASX_OK,
                   "trace_export");

    /* Verify digest consistency after import as replay reference */
    uint64_t digest_before = asx_trace_digest();

    SCENARIO_CHECK(asx_trace_import_binary(trace_buf, trace_len) == ASX_OK, "trace_import");
    {
        asx_replay_result rr = asx_replay_verify();
        SCENARIO_CHECK(rr.result == ASX_REPLAY_MATCH,
                       "imported trace should replay-match current trace");
        SCENARIO_CHECK(rr.actual_digest == digest_before,
                       "digest should survive export/import round-trip");
    }
    asx_replay_clear_reference();

    SCENARIO_END();
}

/* Corrupted trace detection */
static void scenario_corrupted_trace_detected(void) {
    SCENARIO_BEGIN("continuity-corruption.detected");

    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");
    emit_restart_signature(0xC150u, 1u);

    /* Export trace */
    uint8_t trace_buf[8192];
    uint32_t trace_len = 0;
    SCENARIO_CHECK(asx_trace_export_binary(trace_buf, (uint32_t)sizeof(trace_buf), &trace_len) ==
                       ASX_OK,
                   "trace_export");

    /* Corrupt a byte in the middle of the trace */
    if (trace_len > 30) { trace_buf[30] ^= 0xFF; }

    /* Import must fail validation for corrupted binary trace. */
    asx_trace_reset();
    asx_status rc = asx_trace_import_binary(trace_buf, trace_len);
    SCENARIO_CHECK(rc == ASX_E_INVALID_ARGUMENT, "corrupt trace should fail import validation");

    SCENARIO_END();
}

/* Snapshot capture and digest */
static void scenario_snapshot_capture(void) {
    SCENARIO_BEGIN("continuity-snapshot.capture_digest");

    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_run");
    emit_restart_signature(0xC160u, 1u);

    /* Capture snapshot */
    asx_snapshot_buffer snap;
    asx_status rc = asx_snapshot_capture(&snap);
    SCENARIO_CHECK(rc == ASX_OK, "snapshot_capture");
    SCENARIO_CHECK(snap.len > 0, "snapshot should not be empty");

    /* Snapshot digest should be non-zero */
    uint64_t snap_digest = asx_snapshot_digest(&snap);
    SCENARIO_CHECK(snap_digest != 0, "snapshot digest should not be zero");

    /* Same snapshot digest on identical replay */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open_2");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn_2");

    budget = asx_budget_from_polls(10);
    SCENARIO_CHECK(asx_scheduler_run(rid, &budget) == ASX_OK, "scheduler_2");
    emit_restart_signature(0xC160u, 1u);

    asx_snapshot_buffer snap2;
    SCENARIO_CHECK(asx_snapshot_capture(&snap2) == ASX_OK, "snapshot_capture_2");

    uint64_t snap_digest2 = asx_snapshot_digest(&snap2);
    SCENARIO_CHECK(snap_digest == snap_digest2, "snapshot digest should be deterministic");

    printf("DIGEST %016llx\n", (unsigned long long)snap_digest);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_crash_restart_simple();
    scenario_continuity_check_matching();
    scenario_continuity_mismatch_detected();
    scenario_multi_task_roundtrip();
    scenario_corrupted_trace_detected();
    scenario_snapshot_capture();

    fprintf(stderr, "[e2e] continuity_restart: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
