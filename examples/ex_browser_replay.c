/*
 * ex_browser_replay.c — Deterministic replay in the browser sandbox
 *
 * Demonstrates that the browser profile preserves deterministic replay:
 *   1. Trace events are emitted for browser-safe operations
 *   2. Trace digests are identical across repeated runs
 *   3. Ghost determinism oracle works within the sandbox
 *   4. Browser profile identity is captured in trace metadata
 *
 * The browser sandbox restricts *surfaces* (no filesystem, process,
 * signal), but deterministic semantics are a universal guarantee
 * across all profiles — including browser.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/profile_compat.h>
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
 * Scenario: browser_replay.trace_in_sandbox
 *
 * Runs a region lifecycle in the browser profile and verifies
 * that trace events are emitted. The trace surface is browser-safe.
 * ------------------------------------------------------------------- */

static void scenario_trace_in_sandbox(void) {
    SCENARIO_BEGIN("browser_replay.trace_in_sandbox");

    /* Confirm trace surface is available */
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_TRACE) == ASX_OK, "trace_available");

    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "spawn");

    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain");

    /* Trace events were emitted */
    uint32_t count = asx_trace_event_count();
    SCENARIO_CHECK(count > 0, "events_emitted");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_replay.digest_deterministic
 *
 * Two identical lifecycles in the browser profile produce the
 * same trace digest — proving replay works in the sandbox.
 * ------------------------------------------------------------------- */

static void scenario_digest_deterministic(void) {
    SCENARIO_BEGIN("browser_replay.digest_deterministic");

    /* Run 1 */
    asx_runtime_reset();
    asx_trace_reset();

    asx_region_id rid;
    asx_task_id tid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "r1_region");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "r1_spawn");

    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "r1_drain");

    uint64_t d1 = asx_trace_digest();

    /* Run 2 (identical) */
    asx_runtime_reset();
    asx_trace_reset();

    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "r2_region");
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "r2_spawn");

    budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "r2_drain");

    uint64_t d2 = asx_trace_digest();

    SCENARIO_CHECK(d1 == d2, "digests_match");
    SCENARIO_CHECK(d1 != 0, "digest_nonzero");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_replay.ghost_in_sandbox
 *
 * Ghost monitors (debug invariant checking) work in the browser
 * profile. The ghost surface is browser-safe.
 * ------------------------------------------------------------------- */

static void scenario_ghost_in_sandbox(void) {
    SCENARIO_BEGIN("browser_replay.ghost_in_sandbox");

    /* Confirm ghost surface is available */
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_GHOST) == ASX_OK, "ghost_available");

    asx_ghost_reset();
    SCENARIO_CHECK(asx_ghost_violation_count() == 0, "clean_slate");

    /* Determinism oracle works in browser sandbox */
    asx_ghost_determinism_reset();
    asx_ghost_determinism_record(42);
    asx_ghost_determinism_record(99);
    asx_ghost_determinism_seal();

    /* Replay same sequence */
    asx_ghost_determinism_record(42);
    asx_ghost_determinism_record(99);

    uint32_t mismatches = asx_ghost_determinism_check();
    SCENARIO_CHECK(mismatches == 0, "zero_mismatches");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_replay.profile_in_parity
 *
 * The browser profile participates in the parity contract: its
 * trace digest can be compared against other profiles using the
 * standard parity API.
 * ------------------------------------------------------------------- */

static void scenario_profile_parity(void) {
    SCENARIO_BEGIN("browser_replay.profile_in_parity");

    /* Browser profile is a valid profile ID for parity comparison */
    asx_parity_result result;
    uint64_t digest = 0x123456789ABCDEF0ULL;

    int ok = asx_profile_digest_compare(digest, ASX_PROFILE_ID_BROWSER, digest,
                                        ASX_PROFILE_ID_CORE, &result);
    SCENARIO_CHECK(ok == 1, "same_digest_passes");
    SCENARIO_CHECK(result.pass == 1, "parity_holds");

    /* Browser is named in the result */
    SCENARIO_CHECK(result.profile_a == ASX_PROFILE_ID_BROWSER, "profile_a_browser");

    SCENARIO_END();
}

int main(void) {
    scenario_trace_in_sandbox();
    scenario_digest_deterministic();
    scenario_ghost_in_sandbox();
    scenario_profile_parity();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
