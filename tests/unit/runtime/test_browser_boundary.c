/*
 * test_browser_boundary.c — tests for browser profile capability boundary (bd-1eqo.16.1)
 *
 * Verifies:
 *   - Browser profile identity and descriptor
 *   - Surface availability matrix (browser vs non-browser)
 *   - Fail-closed gate returns ASX_E_PERMISSION_DENIED for native surfaces
 *   - All non-browser profiles grant all surfaces
 *   - Surface name lookup
 *   - Blocked/available count queries
 *   - Browser profile satisfies all 8 semantic rules
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/profile_compat.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Browser profile identity
 * ------------------------------------------------------------------- */

TEST(browser_profile_name) {
    const char *name = asx_profile_name(ASX_PROFILE_ID_BROWSER);
    ASSERT_STR_EQ(name, "BROWSER");
}

TEST(browser_profile_descriptor) {
    asx_profile_descriptor desc;
    asx_status s = asx_profile_get_descriptor(ASX_PROFILE_ID_BROWSER, &desc);
    ASSERT_EQ((int)s, (int)ASX_OK);
    ASSERT_EQ((int)desc.id, (int)ASX_PROFILE_ID_BROWSER);
    ASSERT_STR_EQ(desc.name, "BROWSER");
    ASSERT_EQ((int)desc.default_wait, (int)ASX_WAIT_YIELD);
    ASSERT_TRUE(desc.max_regions > 0);
    ASSERT_TRUE(desc.max_tasks > 0);
    ASSERT_TRUE(desc.max_obligations > 0);
    ASSERT_TRUE(desc.max_timers > 0);
    ASSERT_EQ(desc.allocator_sealable, 1);
    ASSERT_EQ(desc.ghost_monitors, 1);
}

TEST(browser_profile_trace_capacity) {
    asx_profile_descriptor desc;
    ASSERT_EQ((int)asx_profile_get_descriptor(ASX_PROFILE_ID_BROWSER, &desc), (int)ASX_OK);
    /* Browser uses reduced trace capacity like freestanding/embedded */
    ASSERT_EQ(desc.trace_capacity, (uint32_t)256);
}

TEST(browser_profile_resource_class_scaling) {
    asx_profile_descriptor r1, r2, r3;

    ASSERT_EQ((int)asx_profile_get_descriptor_for_class(ASX_PROFILE_ID_BROWSER, ASX_CLASS_R1, &r1),
              (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor_for_class(ASX_PROFILE_ID_BROWSER, ASX_CLASS_R2, &r2),
              (int)ASX_OK);
    ASSERT_EQ((int)asx_profile_get_descriptor_for_class(ASX_PROFILE_ID_BROWSER, ASX_CLASS_R3, &r3),
              (int)ASX_OK);

    /* R1 < R2 < R3 */
    ASSERT_TRUE(r1.max_tasks < r3.max_tasks);
    ASSERT_TRUE(r1.trace_capacity < r3.trace_capacity);
    /* All non-zero */
    ASSERT_TRUE(r1.max_regions > 0);
    ASSERT_TRUE(r1.max_tasks > 0);
}

/* -------------------------------------------------------------------
 * Surface availability — browser profile
 * ------------------------------------------------------------------- */

TEST(browser_allows_core_surfaces) {
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_REGION), 1);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_TASK), 1);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_CHANNEL), 1);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_TIMER), 1);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_OBLIGATION), 1);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_TRACE), 1);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_GHOST), 1);
}

TEST(browser_blocks_native_surfaces) {
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_FILESYSTEM), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_PROCESS), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_SIGNAL), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_IO_DRIVER), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_BLOCKING), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_SERVER), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_GRPC), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_MESSAGING), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_TLS), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, ASX_SURFACE_DATABASE), 0);
}

TEST(browser_blocked_count) {
    uint32_t blocked = asx_surface_blocked_count(ASX_PROFILE_ID_BROWSER);
    ASSERT_EQ((int)blocked, 10); /* fs, process, signal, io_driver, blocking, server, grpc, messaging, tls, database */
}

TEST(browser_available_count) {
    uint32_t avail = asx_surface_available_count(ASX_PROFILE_ID_BROWSER);
    ASSERT_EQ((int)avail, 7); /* region, task, channel, timer, obligation, trace, ghost */
}

/* -------------------------------------------------------------------
 * Surface availability — non-browser profiles grant all
 * ------------------------------------------------------------------- */

TEST(core_allows_all_surfaces) {
    int i;
    for (i = 0; i < ASX_SURFACE_COUNT; i++) {
        ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_CORE, (asx_host_surface)i), 1);
    }
}

TEST(non_browser_profiles_allow_all) {
    int p, s;
    for (p = 0; p < (int)ASX_PROFILE_ID_COUNT; p++) {
        if (p == (int)ASX_PROFILE_ID_BROWSER) continue;
        for (s = 0; s < ASX_SURFACE_COUNT; s++) {
            ASSERT_EQ(asx_surface_available((asx_profile_id)p, (asx_host_surface)s), 1);
        }
    }
}

TEST(non_browser_blocked_count_zero) {
    int p;
    for (p = 0; p < (int)ASX_PROFILE_ID_COUNT; p++) {
        if (p == (int)ASX_PROFILE_ID_BROWSER) continue;
        ASSERT_EQ((int)asx_surface_blocked_count((asx_profile_id)p), 0);
    }
}

/* -------------------------------------------------------------------
 * Active-profile gate
 *
 * The gate should match the currently active profile exactly. For
 * browser-profile builds this means native-only surfaces fail closed;
 * for core builds the full shipped surface remains available.
 * ------------------------------------------------------------------- */

TEST(gate_matches_active_profile) {
    int i;
    for (i = 0; i < ASX_SURFACE_COUNT; i++) {
        asx_status expected = asx_surface_available_active((asx_host_surface)i)
                                  ? ASX_OK
                                  : ASX_E_PERMISSION_DENIED;
        ASSERT_EQ((int)asx_surface_gate((asx_host_surface)i), (int)expected);
    }
}

/* -------------------------------------------------------------------
 * Surface names
 * ------------------------------------------------------------------- */

TEST(surface_name_region) { ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_REGION), "region"); }

TEST(surface_name_filesystem) {
    ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_FILESYSTEM), "filesystem");
}

TEST(surface_name_process) { ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_PROCESS), "process"); }

TEST(surface_name_signal) { ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_SIGNAL), "signal"); }

TEST(surface_name_server) { ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_SERVER), "server"); }

TEST(surface_name_grpc) { ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_GRPC), "grpc"); }

TEST(surface_name_messaging) {
    ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_MESSAGING), "messaging");
}

TEST(surface_name_tls) { ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_TLS), "tls"); }

TEST(surface_name_database) {
    ASSERT_STR_EQ(asx_surface_name(ASX_SURFACE_DATABASE), "database");
}

TEST(surface_name_out_of_range) {
    ASSERT_STR_EQ(asx_surface_name((asx_host_surface)99), "unknown");
}

/* -------------------------------------------------------------------
 * Edge cases
 * ------------------------------------------------------------------- */

TEST(surface_available_out_of_range) {
    /* Out-of-range surface should return 0 (fail-closed) */
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_CORE, (asx_host_surface)99), 0);
    ASSERT_EQ(asx_surface_available(ASX_PROFILE_ID_BROWSER, (asx_host_surface)99), 0);
}

/* -------------------------------------------------------------------
 * Semantic parity — browser profile enforces all 8 semantic rules
 * ------------------------------------------------------------------- */

TEST(browser_enforces_all_semantic_rules) {
    int i;
    for (i = 0; i < (int)ASX_SRULE_COUNT; i++) {
        int enforced = asx_profile_semantic_rule_enforced((asx_semantic_rule)i);
        ASSERT_EQ(enforced, 1);
    }
}

TEST(browser_profile_in_count) {
    /* Browser is included in ASX_PROFILE_ID_COUNT */
    ASSERT_TRUE((int)ASX_PROFILE_ID_BROWSER < (int)ASX_PROFILE_ID_COUNT);
    ASSERT_EQ((int)ASX_PROFILE_ID_COUNT, 9);
}

/* -------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------- */

int main(void) {
    /* Browser profile identity */
    RUN_TEST(browser_profile_name);
    RUN_TEST(browser_profile_descriptor);
    RUN_TEST(browser_profile_trace_capacity);
    RUN_TEST(browser_profile_resource_class_scaling);

    /* Surface availability — browser */
    RUN_TEST(browser_allows_core_surfaces);
    RUN_TEST(browser_blocks_native_surfaces);
    RUN_TEST(browser_blocked_count);
    RUN_TEST(browser_available_count);

    /* Surface availability — non-browser */
    RUN_TEST(core_allows_all_surfaces);
    RUN_TEST(non_browser_profiles_allow_all);
    RUN_TEST(non_browser_blocked_count_zero);

    /* Fail-closed gate */
    RUN_TEST(gate_matches_active_profile);

    /* Surface names */
    RUN_TEST(surface_name_region);
    RUN_TEST(surface_name_filesystem);
    RUN_TEST(surface_name_process);
    RUN_TEST(surface_name_signal);
    RUN_TEST(surface_name_server);
    RUN_TEST(surface_name_grpc);
    RUN_TEST(surface_name_messaging);
    RUN_TEST(surface_name_tls);
    RUN_TEST(surface_name_database);
    RUN_TEST(surface_name_out_of_range);

    /* Edge cases */
    RUN_TEST(surface_available_out_of_range);

    /* Semantic parity */
    RUN_TEST(browser_enforces_all_semantic_rules);
    RUN_TEST(browser_profile_in_count);

    TEST_REPORT();
    return test_failures > 0 ? 1 : 0;
}
