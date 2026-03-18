/*
 * ex_evidence_monitoring.c — Evidence collection and runtime monitoring walkthrough
 *
 * Demonstrates:
 *   1. Configuring a monitor policy with custom thresholds
 *   2. Evaluating runtime state against the policy
 *   3. Evidence accumulation and verdict derivation
 *   4. Rendering evidence as NDJSON for log retention
 *   5. Observability snapshot capture with digest tracking
 *
 * Output: SCENARIO lines for smoke-test validation plus ARTIFACT lines
 * carrying evidence and monitoring results for log retention.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

#define IGNORE_RC(expr)                                                                            \
    do {                                                                                           \
        asx_status ignore_rc_ = (expr);                                                            \
        (void)ignore_rc_;                                                                          \
    } while (0)

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        printf("SCENARIO %s ...\n", (id));                                                         \
    } while (0)

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("  OK: %s\n", (msg));                                                           \
            g_pass++;                                                                              \
        } else {                                                                                   \
            printf("  FAIL: %s\n", (msg));                                                         \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ */
/* Scenario 1: Healthy runtime passes monitor evaluation               */
/* ------------------------------------------------------------------ */

static void scenario_healthy_runtime(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;
    asx_evidence_summary summary;
    asx_status st;

    SCENARIO_BEGIN("healthy-runtime-monitor");

    st = asx_runtime_init_default(&rt);
    SCENARIO_CHECK(st == ASX_OK, "runtime init");

    asx_monitor_policy_init_default(&policy);
    st = asx_monitor_evaluate(&rt, &policy, &report, &sink);
    SCENARIO_CHECK(st == ASX_OK, "monitor evaluate");

    IGNORE_RC(asx_evidence_sink_summarize(&sink, &summary));
    SCENARIO_CHECK(summary.fail_count == 0, "no failures in healthy runtime");
    SCENARIO_CHECK(report.verdict == ASX_EVIDENCE_PASS || report.verdict == ASX_EVIDENCE_INFO,
                   "verdict is PASS or INFO");

    printf("ARTIFACT monitor_report: triggered_mask=0x%x verdict=%s\n", report.triggered_mask,
           asx_evidence_level_str(report.verdict));
    printf("ARTIFACT evidence_summary: pass=%u warn=%u fail=%u info=%u\n", summary.pass_count,
           summary.warn_count, summary.fail_count, summary.info_count);

    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* Scenario 2: Watchdog violation triggers FAIL                        */
/* ------------------------------------------------------------------ */

static void scenario_watchdog_violation(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;
    asx_auto_watchdog *wd;
    asx_report_buf ndjson;
    asx_status st;

    SCENARIO_BEGIN("watchdog-violation");

    IGNORE_RC(asx_runtime_init_default(&rt));

    /* Configure watchdog with tight period, then violate it */
    wd = asx_auto_watchdog_global();
    asx_auto_watchdog_init(wd, 10u); /* 10ns period */
    asx_auto_watchdog_checkpoint(wd, 0u);
    asx_auto_watchdog_checkpoint(wd, 25u); /* 25ns gap > 10ns period = violation */

    asx_monitor_policy_init_default(&policy);
    st = asx_monitor_evaluate(&rt, &policy, &report, &sink);
    SCENARIO_CHECK(st == ASX_OK, "monitor evaluate with watchdog violation");
    SCENARIO_CHECK(report.verdict == ASX_EVIDENCE_FAIL, "verdict is FAIL due to watchdog");
    SCENARIO_CHECK((report.triggered_mask & ASX_MONITOR_WATCHDOG_VIOLATIONS) != 0,
                   "watchdog trigger bit set");

    /* Render evidence as NDJSON */
    st = asx_evidence_sink_render_ndjson(&sink, &ndjson);
    SCENARIO_CHECK(st == ASX_OK, "NDJSON render");

    printf("ARTIFACT evidence_ndjson:\n%s", asx_report_buf_cstr(&ndjson));

    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* Scenario 3: Observability snapshot                                  */
/* ------------------------------------------------------------------ */

static void scenario_observability_snapshot(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;
    asx_observability_snapshot snapshot;
    asx_status st;

    SCENARIO_BEGIN("observability-snapshot");

    IGNORE_RC(asx_runtime_init_default(&rt));

    asx_monitor_policy_init_default(&policy);
    IGNORE_RC(asx_monitor_evaluate(&rt, &policy, &report, &sink));

    st = asx_observability_capture(&rt, &sink, &snapshot);
    SCENARIO_CHECK(st == ASX_OK, "observability capture");
    SCENARIO_CHECK(snapshot.evidence.verdict == report.verdict, "snapshot verdict matches monitor");

    printf("ARTIFACT observability: trace_digest=0x%016llx telemetry_digest=0x%016llx "
           "event_log_digest=0x%016llx\n",
           (unsigned long long)snapshot.trace_digest,
           (unsigned long long)snapshot.telemetry_digest,
           (unsigned long long)snapshot.event_log_digest);

    asx_runtime_shutdown(&rt);
}

/* ------------------------------------------------------------------ */
/* Scenario 4: Evidence helpers                                        */
/* ------------------------------------------------------------------ */

static void scenario_evidence_helpers(void) {
    asx_evidence_entry entry;

    SCENARIO_BEGIN("evidence-helpers");

    SCENARIO_CHECK(strcmp(asx_evidence_level_str(ASX_EVIDENCE_INFO), "INFO") == 0,
                   "level str INFO");
    SCENARIO_CHECK(strcmp(asx_evidence_level_str(ASX_EVIDENCE_PASS), "PASS") == 0,
                   "level str PASS");
    SCENARIO_CHECK(strcmp(asx_evidence_level_str(ASX_EVIDENCE_WARN), "WARN") == 0,
                   "level str WARN");
    SCENARIO_CHECK(strcmp(asx_evidence_level_str(ASX_EVIDENCE_FAIL), "FAIL") == 0,
                   "level str FAIL");

    entry.level = ASX_EVIDENCE_FAIL;
    entry.entity_id = 42;
    SCENARIO_CHECK(asx_evidence_is_failure(&entry), "FAIL entry is failure");
    SCENARIO_CHECK(asx_evidence_entry_has_entity(&entry), "entry with entity_id=42 has entity");

    entry.entity_id = 0;
    SCENARIO_CHECK(!asx_evidence_entry_has_entity(&entry), "entry with entity_id=0 has no entity");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("=== ex_evidence_monitoring ===\n\n");

    scenario_healthy_runtime();
    printf("\n");
    scenario_watchdog_violation();
    printf("\n");
    scenario_observability_snapshot();
    printf("\n");
    scenario_evidence_helpers();

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    printf("rerun: make examples && ./build/examples/ex_evidence_monitoring\n");
    return g_fail ? 1 : 0;
}
