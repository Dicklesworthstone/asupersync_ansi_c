/*
 * test_browser_diagnostic.c — tests for browser diagnostics and DX taxonomy (bd-1eqo.16.3)
 *
 * Verifies:
 *   - DX error taxonomy: lookup, surface mapping, catalog completeness
 *   - Browser evidence collector: capture and sink recording
 *   - Flake governance: snapshot capture and matching
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/runtime/browser_diagnostic.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* -------------------------------------------------------------------
 * DX error taxonomy tests
 * ------------------------------------------------------------------- */

TEST(dx_lookup_none_returns_null) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NONE);
    ASSERT_TRUE(entry == NULL);
}

TEST(dx_lookup_out_of_range_returns_null) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup((asx_browser_dx_class)99);
    ASSERT_TRUE(entry == NULL);
}

TEST(dx_lookup_native_fs) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_FS);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(entry->title != NULL);
    ASSERT_TRUE(entry->explanation != NULL);
    ASSERT_TRUE(entry->remediation != NULL);
    ASSERT_EQ((int)entry->dx_class, (int)ASX_BROWSER_DX_NATIVE_FS);
}

TEST(dx_lookup_native_process) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_PROCESS);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "process") != NULL);
}

TEST(dx_lookup_native_signal) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_SIGNAL);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "signal") != NULL);
}

TEST(dx_lookup_native_server) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_SERVER);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "server") != NULL);
}

TEST(dx_lookup_native_grpc) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_GRPC);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "gRPC") != NULL);
}

TEST(dx_lookup_native_messaging) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_MESSAGING);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "messaging") != NULL);
}

TEST(dx_lookup_native_tls) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_TLS);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "TLS") != NULL);
}

TEST(dx_lookup_native_database) {
    const asx_browser_dx_entry *entry = asx_browser_dx_lookup(ASX_BROWSER_DX_NATIVE_DATABASE);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(strstr(entry->title, "database") != NULL);
}

TEST(dx_lookup_all_have_remediation) {
    int i;
    for (i = 1; i < (int)ASX_BROWSER_DX_COUNT; i++) {
        const asx_browser_dx_entry *entry = asx_browser_dx_lookup((asx_browser_dx_class)i);
        ASSERT_TRUE(entry != NULL);
        ASSERT_TRUE(entry->remediation != NULL);
        ASSERT_TRUE(strlen(entry->remediation) > 0);
    }
}

TEST(dx_count) { ASSERT_EQ((int)asx_browser_dx_count(), (int)ASX_BROWSER_DX_COUNT); }

/* -------------------------------------------------------------------
 * DX surface mapping tests
 * ------------------------------------------------------------------- */

TEST(dx_for_surface_fs) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_FILESYSTEM),
              (int)ASX_BROWSER_DX_NATIVE_FS);
}

TEST(dx_for_surface_process) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_PROCESS),
              (int)ASX_BROWSER_DX_NATIVE_PROCESS);
}

TEST(dx_for_surface_signal) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_SIGNAL),
              (int)ASX_BROWSER_DX_NATIVE_SIGNAL);
}

TEST(dx_for_surface_io_driver) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_IO_DRIVER),
              (int)ASX_BROWSER_DX_NATIVE_IO);
}

TEST(dx_for_surface_blocking) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_BLOCKING),
              (int)ASX_BROWSER_DX_NATIVE_BLOCKING);
}

TEST(dx_for_surface_server) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_SERVER),
              (int)ASX_BROWSER_DX_NATIVE_SERVER);
}

TEST(dx_for_surface_grpc) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_GRPC), (int)ASX_BROWSER_DX_NATIVE_GRPC);
}

TEST(dx_for_surface_messaging) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_MESSAGING),
              (int)ASX_BROWSER_DX_NATIVE_MESSAGING);
}

TEST(dx_for_surface_tls) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_TLS), (int)ASX_BROWSER_DX_NATIVE_TLS);
}

TEST(dx_for_surface_database) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_DATABASE),
              (int)ASX_BROWSER_DX_NATIVE_DATABASE);
}

TEST(dx_for_surface_region_none) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_REGION), (int)ASX_BROWSER_DX_NONE);
}

TEST(dx_for_surface_task_none) {
    ASSERT_EQ((int)asx_browser_dx_for_surface(ASX_SURFACE_TASK), (int)ASX_BROWSER_DX_NONE);
}

/* -------------------------------------------------------------------
 * Browser evidence collector tests
 * ------------------------------------------------------------------- */

TEST(evidence_capture_active_profile) {
    asx_browser_evidence_report report;
    asx_browser_evidence_capture(&report);

    ASSERT_EQ((int)report.active_profile, (int)asx_profile_active());
    ASSERT_EQ(report.is_browser, report.active_profile == ASX_PROFILE_ID_BROWSER ? 1 : 0);
    ASSERT_EQ((int)report.surfaces_available + (int)report.surfaces_blocked,
              (int)ASX_SURFACE_COUNT);
    ASSERT_EQ(report.all_semantic_rules_hold, 1);
}

TEST(evidence_to_sink_core) {
    asx_browser_evidence_report report;
    asx_evidence_sink sink;

    asx_browser_evidence_capture(&report);
    asx_evidence_sink_init(&sink);

    asx_status rc = asx_browser_evidence_to_sink(&report, &sink);
    ASSERT_EQ((int)rc, (int)ASX_OK);

    /* 4 entries: profile, boundary, parity, allocator */
    ASSERT_EQ(sink.count, (uint32_t)4);
}

TEST(evidence_to_sink_null_args) {
    asx_evidence_sink sink;
    asx_browser_evidence_report report;

    asx_evidence_sink_init(&sink);

    ASSERT_EQ((int)asx_browser_evidence_to_sink(NULL, &sink), (int)ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ((int)asx_browser_evidence_to_sink(&report, NULL), (int)ASX_E_INVALID_ARGUMENT);
}

TEST(evidence_verdict_core_no_failures) {
    asx_browser_evidence_report report;
    asx_evidence_sink sink;

    asx_browser_evidence_capture(&report);
    asx_evidence_sink_init(&sink);
    asx_browser_evidence_to_sink(&report, &sink);

    /* CORE profile: no failures expected */
    asx_evidence_level verdict = asx_evidence_verdict(&sink);
    ASSERT_TRUE(verdict != ASX_EVIDENCE_FAIL);
}

TEST(evidence_to_sink_resource_exhausted_is_failure_atomic) {
    asx_browser_evidence_report report;
    asx_evidence_sink sink;
    asx_evidence_sink before;
    asx_status rc;

    asx_browser_evidence_capture(&report);
    asx_evidence_sink_init(&sink);

    while (sink.count + 3u < ASX_EVIDENCE_SINK_CAPACITY) {
        ASSERT_EQ((int)asx_evidence_record(&sink, "fill", ASX_EVIDENCE_INFO, "x", 0), (int)ASX_OK);
    }

    before = sink;
    rc = asx_browser_evidence_to_sink(&report, &sink);
    ASSERT_EQ((int)rc, (int)ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(sink.count, before.count);
    ASSERT_EQ(sink.pass_count, before.pass_count);
    ASSERT_EQ(sink.warn_count, before.warn_count);
    ASSERT_EQ(sink.fail_count, before.fail_count);
    ASSERT_EQ(sink.info_count, before.info_count);
    ASSERT_EQ(sink.next_seq, before.next_seq);
}

/* -------------------------------------------------------------------
 * Flake governance tests
 * ------------------------------------------------------------------- */

TEST(flake_capture_basic) {
    asx_browser_flake_snapshot snap;
    asx_trace_reset();
    asx_ghost_reset();

    asx_browser_flake_capture(&snap, NULL);

    ASSERT_EQ((int)snap.profile, (int)asx_profile_active());
    ASSERT_EQ(snap.is_deterministic, 1);
    ASSERT_EQ(snap.evidence_fail_count, (uint32_t)0);
    ASSERT_EQ(snap.evidence_warn_count, (uint32_t)0);
}

TEST(flake_capture_with_sink) {
    asx_browser_flake_snapshot snap;
    asx_evidence_sink sink;

    asx_trace_reset();
    asx_ghost_reset();
    asx_evidence_sink_init(&sink);

    /* Record a warning */
    {
        asx_status rc = asx_evidence_record(&sink, "test", ASX_EVIDENCE_WARN, "test warn", 0);
        (void)rc;
    }

    asx_browser_flake_capture(&snap, &sink);

    ASSERT_EQ(snap.evidence_warn_count, (uint32_t)1);
    ASSERT_EQ(snap.evidence_fail_count, (uint32_t)0);
}

TEST(flake_match_identical) {
    asx_browser_flake_snapshot a, b;

    asx_trace_reset();
    asx_ghost_reset();

    asx_browser_flake_capture(&a, NULL);
    asx_browser_flake_capture(&b, NULL);

    ASSERT_EQ(asx_browser_flake_match(&a, &b), 1);
}

TEST(flake_match_different_digest) {
    asx_browser_flake_snapshot a, b;

    asx_trace_reset();
    asx_ghost_reset();

    asx_browser_flake_capture(&a, NULL);

    /* Emit a trace event to change the digest */
    asx_trace_reset();
    {
        asx_runtime_reset();
        asx_region_id rid;
        asx_status rc = asx_region_open(&rid);
        (void)rc;
        asx_budget budget = asx_budget_from_polls(8);
        rc = asx_region_drain(rid, &budget);
        (void)rc;
    }

    asx_browser_flake_capture(&b, NULL);

    /* Different trace digests → no match */
    ASSERT_EQ(asx_browser_flake_match(&a, &b), 0);
}

TEST(flake_match_different_ghosts) {
    asx_browser_flake_snapshot a, b;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.trace_digest = 1u;
    b.trace_digest = 1u;
    a.profile = ASX_PROFILE_ID_CORE;
    b.profile = ASX_PROFILE_ID_CORE;
    a.surfaces_blocked = 0u;
    b.surfaces_blocked = 0u;
    a.ghost_violations = 0u;
    b.ghost_violations = 1u;

    ASSERT_EQ(asx_browser_flake_match(&a, &b), 0);
}

TEST(flake_match_different_evidence_counts) {
    asx_browser_flake_snapshot a, b;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.trace_digest = 1u;
    b.trace_digest = 1u;
    a.profile = ASX_PROFILE_ID_CORE;
    b.profile = ASX_PROFILE_ID_CORE;
    a.surfaces_blocked = 0u;
    b.surfaces_blocked = 0u;
    a.evidence_fail_count = 0u;
    b.evidence_fail_count = 1u;

    ASSERT_EQ(asx_browser_flake_match(&a, &b), 0);
}

TEST(flake_match_null_returns_zero) {
    asx_browser_flake_snapshot a;
    memset(&a, 0, sizeof(a));
    ASSERT_EQ(asx_browser_flake_match(&a, NULL), 0);
    ASSERT_EQ(asx_browser_flake_match(NULL, &a), 0);
}

/* -------------------------------------------------------------------
 * Runner
 * ------------------------------------------------------------------- */

int main(void) {
    /* DX taxonomy */
    RUN_TEST(dx_lookup_none_returns_null);
    RUN_TEST(dx_lookup_out_of_range_returns_null);
    RUN_TEST(dx_lookup_native_fs);
    RUN_TEST(dx_lookup_native_process);
    RUN_TEST(dx_lookup_native_signal);
    RUN_TEST(dx_lookup_native_server);
    RUN_TEST(dx_lookup_native_grpc);
    RUN_TEST(dx_lookup_native_messaging);
    RUN_TEST(dx_lookup_native_tls);
    RUN_TEST(dx_lookup_native_database);
    RUN_TEST(dx_lookup_all_have_remediation);
    RUN_TEST(dx_count);

    /* DX surface mapping */
    RUN_TEST(dx_for_surface_fs);
    RUN_TEST(dx_for_surface_process);
    RUN_TEST(dx_for_surface_signal);
    RUN_TEST(dx_for_surface_io_driver);
    RUN_TEST(dx_for_surface_blocking);
    RUN_TEST(dx_for_surface_server);
    RUN_TEST(dx_for_surface_grpc);
    RUN_TEST(dx_for_surface_messaging);
    RUN_TEST(dx_for_surface_tls);
    RUN_TEST(dx_for_surface_database);
    RUN_TEST(dx_for_surface_region_none);
    RUN_TEST(dx_for_surface_task_none);

    /* Evidence collector */
    RUN_TEST(evidence_capture_active_profile);
    RUN_TEST(evidence_to_sink_core);
    RUN_TEST(evidence_to_sink_null_args);
    RUN_TEST(evidence_verdict_core_no_failures);
    RUN_TEST(evidence_to_sink_resource_exhausted_is_failure_atomic);

    /* Flake governance */
    RUN_TEST(flake_capture_basic);
    RUN_TEST(flake_capture_with_sink);
    RUN_TEST(flake_match_identical);
    RUN_TEST(flake_match_different_digest);
    RUN_TEST(flake_match_different_ghosts);
    RUN_TEST(flake_match_different_evidence_counts);
    RUN_TEST(flake_match_null_returns_zero);

    TEST_REPORT();
    return test_failures > 0 ? 1 : 0;
}
