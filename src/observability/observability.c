/*
 * observability.c — public observability snapshot family
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/observability/observability.h>

asx_status asx_observability_capture(const asx_runtime *rt, const asx_evidence_sink *sink,
                                     asx_observability_snapshot *out) {
    asx_status st;

    if (rt == NULL || sink == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_inspect(rt, &out->inspection);
    if (st != ASX_OK) return st;

    st = asx_evidence_sink_summarize(sink, &out->evidence);
    if (st != ASX_OK) return st;

    out->trace_digest = out->inspection.trace.digest;
    out->telemetry_digest = out->inspection.telemetry_digest;
    out->event_log_digest = out->inspection.event_log_digest;
    return ASX_OK;
}
