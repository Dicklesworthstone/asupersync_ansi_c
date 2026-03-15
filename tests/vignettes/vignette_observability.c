/*
 * vignette_observability.c — logged public observability/evidence flow
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/evidence_sink/evidence_sink.h>
#include <asx/monitor/monitor.h>
#include <asx/observability/observability.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    asx_runtime rt;
    asx_monitor_policy policy;
    asx_monitor_report report;
    asx_evidence_sink sink;
    asx_observability_snapshot snapshot;
    asx_report_buf ndjson;
    asx_auto_watchdog *wd;

    printf("=== vignette: observability ===\n\n");

    if (asx_runtime_init_default(&rt) != ASX_OK) {
        fprintf(stderr, "runtime init failed\n");
        return 1;
    }

    wd = asx_auto_watchdog_global();
    asx_auto_watchdog_init(wd, 10u);
    asx_auto_watchdog_checkpoint(wd, 0u);
    asx_auto_watchdog_checkpoint(wd, 25u);

    asx_monitor_policy_init_default(&policy);
    if (asx_monitor_evaluate(&rt, &policy, &report, &sink) != ASX_OK) {
        fprintf(stderr, "monitor evaluate failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    if (asx_observability_capture(&rt, &sink, &snapshot) != ASX_OK) {
        fprintf(stderr, "observability capture failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    if (asx_evidence_sink_render_ndjson(&sink, &ndjson) != ASX_OK) {
        fprintf(stderr, "ndjson render failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    printf("triggered_mask=0x%x verdict=%s trace_digest=0x%016llx event_log_digest=0x%016llx\n",
           report.triggered_mask, asx_evidence_level_str(snapshot.evidence.verdict),
           (unsigned long long)snapshot.trace_digest, (unsigned long long)snapshot.event_log_digest);
    printf("artifact_manifest:\n%s", asx_report_buf_cstr(&ndjson));
    printf("rerun_hint: rch exec -- make build/tests/vignettes/vignette_observability\n");

    asx_runtime_shutdown(&rt);
    return 0;
}
