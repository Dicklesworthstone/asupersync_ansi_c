/*
 * ex_lab_replay.c — Deterministic replay and trace-based investigation
 *
 * Demonstrates the lab/deterministic features of asupersync:
 *   1. Trace emission: region, task, and timer events are recorded
 *   2. Trace digest: fingerprint for replay verification
 *   3. Seeded determinism: same inputs always produce the same digest
 *   4. Ghost monitors: debug-only invariant checks that vanish in release
 *
 * In lab mode, all scheduling and timer behavior is deterministic,
 * enabling exact replay of failure scenarios.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/trace.h>
#include <stdio.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

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

/* -------------------------------------------------------------------
 * Scenario: lab_replay.trace_events_emitted
 *
 * Runs a region lifecycle and verifies that trace events are emitted
 * for task spawn, completion, and region close.
 * ------------------------------------------------------------------- */

static void scenario_trace_events(void) {
    SCENARIO_BEGIN("lab_replay.trace_events_emitted");

    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn");

    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain");

    /* Verify trace events were emitted */
    uint32_t count = asx_trace_event_count();
    SCENARIO_CHECK(count > 0, "events_emitted");

    /* Last event should be REGION_CLOSED */
    asx_trace_event ev;
    SCENARIO_CHECK(asx_trace_event_get(count - 1u, &ev), "get_last_event");
    SCENARIO_CHECK(ev.kind == ASX_TRACE_REGION_CLOSED, "last_event_region_closed");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: lab_replay.digest_deterministic
 *
 * Runs the same lifecycle twice and verifies the trace digest is
 * identical — proving deterministic replay.
 * ------------------------------------------------------------------- */

static void scenario_digest_deterministic(void) {
    SCENARIO_BEGIN("lab_replay.digest_deterministic");

    /* Run 1 */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "run1_region");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "run1_spawn");

    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "run1_drain");

    uint64_t d1 = asx_trace_digest();

    /* Run 2 (identical inputs) */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "run2_region");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "run2_spawn");

    budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "run2_drain");

    uint64_t d2 = asx_trace_digest();

    /* Digests must match */
    SCENARIO_CHECK(d1 == d2, "digests_match");
    SCENARIO_CHECK(d1 != 0, "digest_nonzero");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: lab_replay.ghost_violation_tracking
 *
 * Ghost monitors track invariant violations in debug builds. After
 * reset, the violation count should be zero. Running valid lifecycles
 * should not produce violations.
 * ------------------------------------------------------------------- */

static void scenario_ghost_violations(void) {
    SCENARIO_BEGIN("lab_replay.ghost_violation_tracking");

    asx_ghost_reset();

    /* Clean slate: no violations */
    SCENARIO_CHECK(asx_ghost_violation_count() == 0, "clean_after_reset");

    /* Run a valid lifecycle — should produce zero violations */
    asx_runtime_reset();
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain");

    SCENARIO_CHECK(asx_ghost_violation_count() == 0, "no_violations");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: lab_replay.ghost_determinism_seal
 *
 * The ghost determinism oracle records events and seals a baseline.
 * Replaying the same events produces zero mismatches.
 * ------------------------------------------------------------------- */

static void scenario_ghost_determinism(void) {
    SCENARIO_BEGIN("lab_replay.ghost_determinism_seal");

    asx_ghost_determinism_reset();

    /* Record a sequence of events */
    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(200);
    asx_ghost_determinism_record(300);

    uint64_t digest1 = asx_ghost_determinism_digest();

    /* Seal the baseline */
    asx_ghost_determinism_seal();

    /* Replay the same sequence — should produce zero mismatches */
    asx_ghost_determinism_record(100);
    asx_ghost_determinism_record(200);
    asx_ghost_determinism_record(300);

    uint32_t mismatches = asx_ghost_determinism_check();
    SCENARIO_CHECK(mismatches == 0, "zero_mismatches");

    uint64_t digest2 = asx_ghost_determinism_digest();
    SCENARIO_CHECK(digest1 == digest2, "digests_match");

    SCENARIO_END();
}

int main(void) {
    scenario_trace_events();
    scenario_digest_deterministic();
    scenario_ghost_violations();
    scenario_ghost_determinism();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
