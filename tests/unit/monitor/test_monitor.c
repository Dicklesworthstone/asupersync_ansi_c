/*
 * test_monitor.c — unit tests for monitor and observability families
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/monitor/monitor.h>
#include <asx/observability/observability.h>
#include <asx/runtime/browser_boundary.h>
#include <asx/runtime/io_driver.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        asx_runtime_reset();                                                                       \
        asx_auto_instrument_reset();                                                               \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

static void test_monitor_healthy(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;

    MUST_OK(asx_runtime_init_default(&rt));
    asx_monitor_policy_init_default(&policy);
    MUST_OK(asx_monitor_evaluate(&rt, &policy, &report, &sink));

    ASSERT(report.triggered_mask == 0u, "healthy mask");
    ASSERT(report.verdict == ASX_EVIDENCE_PASS, "healthy verdict");
    ASSERT(sink.count == 1u, "healthy pass entry");
    asx_runtime_shutdown(&rt);
}

static void test_monitor_watchdog_trigger(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;
    asx_auto_watchdog *wd;

    MUST_OK(asx_runtime_init_default(&rt));
    wd = asx_auto_watchdog_global();
    asx_auto_watchdog_init(wd, 10u);
    asx_auto_watchdog_checkpoint(wd, 0u);
    asx_auto_watchdog_checkpoint(wd, 25u);

    asx_monitor_policy_init_default(&policy);
    MUST_OK(asx_monitor_evaluate(&rt, &policy, &report, &sink));

    ASSERT((report.triggered_mask & ASX_MONITOR_WATCHDOG_VIOLATIONS) != 0u, "watchdog mask");
    ASSERT(report.verdict == ASX_EVIDENCE_FAIL, "watchdog fail verdict");
    asx_runtime_shutdown(&rt);
}

static void test_monitor_io_threshold_trigger(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;

    MUST_OK(asx_runtime_init_default(&rt));
    asx_monitor_policy_init_default(&policy);
    policy.max_io_utilization_pct = 0u;

    if (asx_surface_available_active(ASX_SURFACE_IO_DRIVER)) {
#if ASX_HAS_NATIVE_IO_DRIVER
        asx_waker w;
        asx_io_token tok;
        MUST_OK(asx_waker_register(88u, &w));
        MUST_OK(asx_io_register(88, ASX_IO_READABLE, &w, &tok));
        MUST_OK(asx_monitor_evaluate(&rt, &policy, &report, &sink));
        ASSERT((report.triggered_mask & ASX_MONITOR_IO_HIGH) != 0u, "io mask");
        ASSERT(report.verdict == ASX_EVIDENCE_WARN, "io threshold warns");
        ASSERT(strcmp(sink.entries[0].source, "monitor:io_driver") == 0, "io evidence source");
        asx_io_deregister(&tok);
#else
        ASSERT(0, "io surface should not be active when io-driver types are compile-hidden");
#endif
    } else {
        MUST_OK(asx_monitor_evaluate(&rt, &policy, &report, &sink));
        ASSERT((report.triggered_mask & ASX_MONITOR_IO_HIGH) == 0u, "no io mask when unsupported");
    }

    asx_runtime_shutdown(&rt);
}

static void test_observability_capture(void) {
    asx_runtime rt;
    asx_evidence_sink sink;
    asx_observability_snapshot snapshot;

    MUST_OK(asx_runtime_init_default(&rt));
    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "obs:test", ASX_EVIDENCE_WARN, "operator-visible", 0));
    MUST_OK(asx_observability_capture(&rt, &sink, &snapshot));

    ASSERT(snapshot.evidence.verdict == ASX_EVIDENCE_WARN, "snapshot verdict");
    ASSERT(snapshot.trace_digest == snapshot.inspection.trace.digest, "trace digest mirrored");
#if ASX_HAS_NATIVE_IO_DRIVER
    ASSERT(snapshot.inspection.io_driver.capacity > 0u, "io capacity mirrored");
#else
    ASSERT(snapshot.inspection.io_driver.capacity == 0u, "io capacity hidden when unsupported");
#endif
#if ASX_HAS_BLOCKING_SURFACE
    ASSERT(snapshot.inspection.blocking.capacity > 0u, "blocking capacity mirrored");
#else
    ASSERT(snapshot.inspection.blocking.capacity == 0u,
           "blocking capacity hidden when unsupported");
#endif
    asx_runtime_shutdown(&rt);
}

static void test_monitor_uninitialized_runtime_fails_closed(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;

    memset(&rt, 0, sizeof(rt));
    asx_monitor_policy_init_default(&policy);
    MUST_OK(asx_monitor_evaluate(&rt, &policy, &report, &sink));

    ASSERT(report.verdict == ASX_EVIDENCE_FAIL, "uninitialized runtime verdict");
    ASSERT(report.triggered_mask == 0u, "no threshold mask for runtime state failure");
    ASSERT(sink.count == 1u, "runtime failure evidence");
    ASSERT(strcmp(sink.entries[0].source, "monitor:runtime") == 0, "runtime evidence source");
}

static void test_monitor_stale_runtime_fails_closed(void) {
    asx_runtime a;
    asx_runtime b;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;

    MUST_OK(asx_runtime_init_default(&a));
    MUST_OK(asx_runtime_init_default(&b));
    asx_monitor_policy_init_default(&policy);
    MUST_OK(asx_monitor_evaluate(&a, &policy, &report, &sink));

    ASSERT(report.verdict == ASX_EVIDENCE_FAIL, "stale runtime verdict");
    ASSERT(report.triggered_mask == 0u, "no threshold mask for stale runtime");
    ASSERT(sink.count == 1u, "stale runtime evidence");
    ASSERT(strcmp(sink.entries[0].source, "monitor:runtime") == 0, "stale evidence source");

    asx_runtime_shutdown(&b);
}

int main(void) {
    printf("test_monitor:\n");

    RUN(test_monitor_healthy);
    RUN(test_monitor_watchdog_trigger);
    RUN(test_monitor_io_threshold_trigger);
    RUN(test_observability_capture);
    RUN(test_monitor_uninitialized_runtime_fails_closed);
    RUN(test_monitor_stale_runtime_fails_closed);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
