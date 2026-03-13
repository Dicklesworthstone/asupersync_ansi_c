/*
 * e2e_continuity.c — end-to-end crash/restart replay continuity scenarios (bd-1md.18)
 *
 * Exercises: binary trace export/import, replay verification,
 * continuity check across simulated restarts, digest stability,
 * and divergence detection.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/telemetry.h>
#include <asx/runtime/trace.h>
#include <stdio.h>
#include <string.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status rc_ = (expr);                                                                   \
        (void)rc_;                                                                                 \
    } while (0)

/* -------------------------------------------------------------------
 * Scenario macros
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

/* -------------------------------------------------------------------
 * Poll functions
 * ------------------------------------------------------------------- */

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

typedef struct {
    asx_co_state co;
    int done;
} yield_state;

static asx_status poll_yield_once(void *ud, asx_task_id self) {
    yield_state *s = (yield_state *)ud;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    ASX_CO_YIELD(&s->co);
    ASX_CO_END(&s->co);
}

static void emit_continuity_signature(uint64_t scenario_tag, uint32_t task_count) {
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
 * Helper: run a canonical scenario and capture trace
 * ------------------------------------------------------------------- */

static void run_canonical_scenario(void) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;

    IGNORE_RC(asx_region_open(&rid));
    IGNORE_RC(asx_task_spawn(rid, poll_complete, NULL, &tid));
    IGNORE_RC(asx_task_spawn(rid, poll_complete, NULL, &tid));

    budget = asx_budget_from_polls(16);
    IGNORE_RC(asx_scheduler_run(rid, &budget));
    emit_continuity_signature(0xC010u, 2u);
}

/* -------------------------------------------------------------------
 * Scenario: binary trace export round-trip
 * ------------------------------------------------------------------- */

static void scenario_export_import_roundtrip(void) {
    uint8_t buf[8192];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.export_import_roundtrip");

    asx_runtime_reset();
    asx_trace_reset();
    run_canonical_scenario();

    uint64_t original_digest = asx_trace_digest();
    uint32_t original_count = asx_trace_event_count();

    SCENARIO_CHECK(original_count > 0, "should have trace events");

    /* Export */
    asx_status rc = asx_trace_export_binary(buf, sizeof(buf), &len);
    SCENARIO_CHECK(rc == ASX_OK, "export should succeed");
    SCENARIO_CHECK(len > ASX_TRACE_BINARY_HEADER, "should have header + events");

    /* Import as replay reference */
    rc = asx_trace_import_binary(buf, len);
    SCENARIO_CHECK(rc == ASX_OK, "import should succeed");

    /* Verify against current trace */
    asx_replay_result rr = asx_replay_verify();
    SCENARIO_CHECK(rr.result == ASX_REPLAY_MATCH, "replay should match");
    SCENARIO_CHECK(rr.actual_digest == original_digest, "digest should match");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: continuity check across simulated restart
 * ------------------------------------------------------------------- */

static void scenario_continuity_across_restart(void) {
    uint8_t buf[8192];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.across_restart");

    /* "Before crash": run scenario, export trace */
    asx_runtime_reset();
    asx_trace_reset();
    run_canonical_scenario();
    uint64_t pre_crash_digest = asx_trace_digest();

    asx_status rc = asx_trace_export_binary(buf, sizeof(buf), &len);
    SCENARIO_CHECK(rc == ASX_OK, "pre-crash export");

    /* "After restart": reset everything, replay same scenario */
    asx_runtime_reset();
    asx_trace_reset();
    run_canonical_scenario();
    uint64_t post_restart_digest = asx_trace_digest();

    /* Digests should match (deterministic) */
    SCENARIO_CHECK(pre_crash_digest == post_restart_digest, "digest should match across restart");

    /* Continuity check should pass */
    rc = asx_trace_continuity_check(buf, len);
    SCENARIO_CHECK(rc == ASX_OK, "continuity check should pass");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: replay divergence detection
 * ------------------------------------------------------------------- */

static void scenario_divergence_detection(void) {
    uint8_t buf[8192];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.divergence_detection");

    /* Run scenario A */
    asx_runtime_reset();
    asx_trace_reset();
    run_canonical_scenario();

    asx_status rc = asx_trace_export_binary(buf, sizeof(buf), &len);
    SCENARIO_CHECK(rc == ASX_OK, "export scenario A");

    /* Run a DIFFERENT scenario */
    asx_runtime_reset();
    asx_trace_reset();
    {
        asx_region_id rid;
        asx_task_id tid;
        asx_budget budget;

        IGNORE_RC(asx_region_open(&rid));
        /* Different: spawn 3 tasks instead of 2 */
        IGNORE_RC(asx_task_spawn(rid, poll_complete, NULL, &tid));
        IGNORE_RC(asx_task_spawn(rid, poll_complete, NULL, &tid));
        IGNORE_RC(asx_task_spawn(rid, poll_complete, NULL, &tid));
        budget = asx_budget_from_polls(16);
        IGNORE_RC(asx_scheduler_run(rid, &budget));
    }

    /* Continuity check should FAIL */
    rc = asx_trace_continuity_check(buf, len);
    SCENARIO_CHECK(rc != ASX_OK, "divergent scenario should fail continuity");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: digest stability across multiple replays
 * ------------------------------------------------------------------- */

static void scenario_digest_stability(void) {
    uint64_t digests[4];
    int i;

    SCENARIO_BEGIN("continuity.digest_stability");

    for (i = 0; i < 4; i++) {
        asx_runtime_reset();
        asx_trace_reset();
        run_canonical_scenario();
        digests[i] = asx_trace_digest();
    }

    SCENARIO_CHECK(digests[0] != 0, "digest should be nonzero");
    SCENARIO_CHECK(digests[0] == digests[1], "replay 1 == replay 2");
    SCENARIO_CHECK(digests[1] == digests[2], "replay 2 == replay 3");
    SCENARIO_CHECK(digests[2] == digests[3], "replay 3 == replay 4");

    printf("DIGEST %016llx\n", (unsigned long long)digests[0]);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: export buffer too small
 * ------------------------------------------------------------------- */

static void scenario_export_buffer_too_small(void) {
    uint8_t tiny[4];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.buffer_too_small");

    asx_runtime_reset();
    asx_trace_reset();
    run_canonical_scenario();

    asx_status rc = asx_trace_export_binary(tiny, sizeof(tiny), &len);
    SCENARIO_CHECK(rc == ASX_E_BUFFER_TOO_SMALL, "tiny buffer should fail");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: import with corrupted magic
 * ------------------------------------------------------------------- */

static void scenario_import_bad_magic(void) {
    uint8_t buf[8192];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.bad_magic");

    asx_runtime_reset();
    asx_trace_reset();
    run_canonical_scenario();

    asx_status rc = asx_trace_export_binary(buf, sizeof(buf), &len);
    SCENARIO_CHECK(rc == ASX_OK, "export");

    /* Corrupt magic bytes */
    buf[0] = 0xFF;
    buf[1] = 0xFF;

    rc = asx_trace_import_binary(buf, len);
    SCENARIO_CHECK(rc != ASX_OK, "corrupted magic should be rejected");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: empty trace export/import
 * ------------------------------------------------------------------- */

static void scenario_empty_trace(void) {
    uint8_t buf[8192];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.empty_trace");

    asx_runtime_reset();
    asx_trace_reset();
    /* No scenario — export empty trace */

    uint32_t ec = asx_trace_event_count();
    SCENARIO_CHECK(ec == 0, "trace should be empty");

    asx_status rc = asx_trace_export_binary(buf, sizeof(buf), &len);
    SCENARIO_CHECK(rc == ASX_OK, "empty export should succeed");
    SCENARIO_CHECK(len == ASX_TRACE_BINARY_HEADER, "empty trace = header only");

    /* Import and verify */
    rc = asx_trace_import_binary(buf, len);
    SCENARIO_CHECK(rc == ASX_OK, "empty import should succeed");

    asx_replay_result rr = asx_replay_verify();
    SCENARIO_CHECK(rr.result == ASX_REPLAY_MATCH, "empty replay should match");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: replay verification with captured-state tasks
 * ------------------------------------------------------------------- */

static void scenario_captured_state_replay(void) {
    uint8_t buf[8192];
    uint32_t len = 0;

    SCENARIO_BEGIN("continuity.captured_state_replay");

    /* Run 1: scenario with yielding tasks (uses captured state) */
    asx_runtime_reset();
    asx_trace_reset();
    {
        asx_region_id rid;
        asx_task_id tid;
        void *st;
        yield_state *ys;
        asx_budget budget;

        IGNORE_RC(asx_region_open(&rid));
        IGNORE_RC(asx_task_spawn_captured(rid, poll_yield_once, (uint32_t)sizeof(yield_state), NULL,
                                          &tid, &st));
        ys = (yield_state *)st;
        ys->co.line = 0;
        ys->done = 0;
        budget = asx_budget_from_polls(16);
        IGNORE_RC(asx_scheduler_run(rid, &budget));
    }

    uint64_t digest1 = asx_trace_digest();
    asx_status rc = asx_trace_export_binary(buf, sizeof(buf), &len);
    SCENARIO_CHECK(rc == ASX_OK, "export run 1");

    /* Run 2: identical */
    asx_runtime_reset();
    asx_trace_reset();
    {
        asx_region_id rid;
        asx_task_id tid;
        void *st;
        yield_state *ys;
        asx_budget budget;

        IGNORE_RC(asx_region_open(&rid));
        IGNORE_RC(asx_task_spawn_captured(rid, poll_yield_once, (uint32_t)sizeof(yield_state), NULL,
                                          &tid, &st));
        ys = (yield_state *)st;
        ys->co.line = 0;
        ys->done = 0;
        budget = asx_budget_from_polls(16);
        IGNORE_RC(asx_scheduler_run(rid, &budget));
    }

    uint64_t digest2 = asx_trace_digest();
    SCENARIO_CHECK(digest1 == digest2, "captured-state replay should match");

    rc = asx_trace_continuity_check(buf, len);
    SCENARIO_CHECK(rc == ASX_OK, "continuity check should pass");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_export_import_roundtrip();
    scenario_continuity_across_restart();
    scenario_divergence_detection();
    scenario_digest_stability();
    scenario_export_buffer_too_small();
    scenario_import_bad_magic();
    scenario_empty_trace();
    scenario_captured_state_replay();

    fprintf(stderr, "[e2e] continuity: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
