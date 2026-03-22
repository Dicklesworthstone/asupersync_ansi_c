/*
 * ex_browser_boundary.c — Browser capability boundary and fail-closed gating
 *
 * Demonstrates browser profile concepts:
 *   1. Surface availability queries: which surfaces are browser-safe
 *   2. Fail-closed gating: native surfaces return ASX_E_PERMISSION_DENIED
 *   3. Browser-safe operations: region/task/channel/timer all work normally
 *   4. Profile descriptor: browser-specific operational parameters
 *
 * In the browser profile, native host surfaces (filesystem, process,
 * signal, I/O driver, blocking thread pool) are blocked. Only the
 * portable async kernel surfaces are available.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/profile_compat.h>
#include <stdio.h>

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
 * Scenario: browser_boundary.native_surfaces_blocked
 *
 * Verifies that native-only surfaces are blocked in the browser
 * profile. The fail-closed gate returns ASX_E_PERMISSION_DENIED.
 * ------------------------------------------------------------------- */

static void scenario_native_blocked(void) {
    SCENARIO_BEGIN("browser_boundary.native_surfaces_blocked");

    /* Native surfaces must be blocked */
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_FILESYSTEM) == ASX_E_PERMISSION_DENIED,
                   "fs_blocked");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_PROCESS) == ASX_E_PERMISSION_DENIED,
                   "process_blocked");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_SIGNAL) == ASX_E_PERMISSION_DENIED,
                   "signal_blocked");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_IO_DRIVER) == ASX_E_PERMISSION_DENIED,
                   "io_driver_blocked");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_BLOCKING) == ASX_E_PERMISSION_DENIED,
                   "blocking_blocked");

    /* Native-only surfaces blocked (FS, process, signal, IO_driver,
     * blocking, server, gRPC, messaging, TLS, database) */
    SCENARIO_CHECK(asx_surface_blocked_count(ASX_PROFILE_ID_BROWSER) == 10,
                   "native_blocked");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_boundary.safe_surfaces_allowed
 *
 * Verifies that browser-safe surfaces pass the fail-closed gate.
 * These are the portable async kernel surfaces.
 * ------------------------------------------------------------------- */

static void scenario_safe_allowed(void) {
    SCENARIO_BEGIN("browser_boundary.safe_surfaces_allowed");

    /* Core kernel surfaces must be available */
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_REGION) == ASX_OK, "region_ok");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_TASK) == ASX_OK, "task_ok");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_CHANNEL) == ASX_OK, "channel_ok");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_TIMER) == ASX_OK, "timer_ok");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_OBLIGATION) == ASX_OK, "obligation_ok");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_TRACE) == ASX_OK, "trace_ok");
    SCENARIO_CHECK(asx_surface_gate(ASX_SURFACE_GHOST) == ASX_OK, "ghost_ok");

    /* 7 surfaces available */
    SCENARIO_CHECK(asx_surface_available_count(ASX_PROFILE_ID_BROWSER) == 7,
                   "seven_available");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_boundary.profile_identity
 *
 * Verifies that the browser profile is correctly identified and
 * its operational parameters match expectations.
 * ------------------------------------------------------------------- */

static void scenario_profile_identity(void) {
    SCENARIO_BEGIN("browser_boundary.profile_identity");

    /* Active profile should be BROWSER when compiled with ASX_PROFILE_BROWSER */
    asx_profile_id id = asx_profile_active();
    SCENARIO_CHECK(id == ASX_PROFILE_ID_BROWSER, "is_browser");

    const char *name = asx_profile_name(id);
    SCENARIO_CHECK(name[0] == 'B', "name_starts_B");

    /* Descriptor: allocator sealable, yield wait, reduced trace */
    asx_profile_descriptor desc;
    SCENARIO_CHECK(asx_profile_get_descriptor(id, &desc) == ASX_OK, "get_desc");
    SCENARIO_CHECK(desc.allocator_sealable == 1, "sealable");
    SCENARIO_CHECK(desc.default_wait == ASX_WAIT_YIELD, "wait_yield");
    SCENARIO_CHECK(desc.trace_capacity == 256, "trace_256");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_boundary.kernel_lifecycle_works
 *
 * Demonstrates that the full async kernel lifecycle (region, task,
 * drain, quiescence) works normally in the browser profile.
 * ------------------------------------------------------------------- */

static asx_status poll_complete(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static void scenario_kernel_lifecycle(void) {
    SCENARIO_BEGIN("browser_boundary.kernel_lifecycle_works");

    asx_runtime_reset();

    /* Open region, spawn task, drain — standard kernel lifecycle */
    asx_region_id rid;
    SCENARIO_CHECK(asx_region_open(&rid) == ASX_OK, "region_open");

    asx_task_id tid;
    SCENARIO_CHECK(asx_task_spawn(rid, poll_complete, NULL, &tid) == ASX_OK, "task_spawn");

    asx_budget budget = asx_budget_from_polls(64);
    SCENARIO_CHECK(asx_region_drain(rid, &budget) == ASX_OK, "drain");

    /* Quiescence holds — all tasks done, no obligations */
    SCENARIO_CHECK(asx_quiescence_check(rid) == ASX_OK, "quiescent");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: browser_boundary.semantic_parity
 *
 * All 8 semantic rules are enforced in the browser profile —
 * the browser boundary restricts surfaces, not semantics.
 * ------------------------------------------------------------------- */

static void scenario_semantic_parity(void) {
    SCENARIO_BEGIN("browser_boundary.semantic_parity");

    uint32_t i;
    uint32_t rule_count = asx_profile_semantic_rule_count();
    SCENARIO_CHECK(rule_count == 8, "eight_rules");

    for (i = 0; i < rule_count; i++) {
        SCENARIO_CHECK(asx_profile_semantic_rule_enforced((asx_semantic_rule)i) == 1,
                       "rule_enforced");
    }

    SCENARIO_END();
}

int main(void) {
    scenario_native_blocked();
    scenario_safe_allowed();
    scenario_profile_identity();
    scenario_kernel_lifecycle();
    scenario_semantic_parity();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
