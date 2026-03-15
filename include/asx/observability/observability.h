/*
 * asx/observability/observability.h — public observability snapshot family
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_OBSERVABILITY_OBSERVABILITY_H
#define ASX_OBSERVABILITY_OBSERVABILITY_H

#include <asx/evidence_sink/evidence_sink.h>
#include <asx/runtime/diagnostic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    asx_inspection_report inspection;
    asx_evidence_summary evidence;
    uint64_t trace_digest;
    uint64_t telemetry_digest;
    uint64_t event_log_digest;
} asx_observability_snapshot;

ASX_API ASX_MUST_USE asx_status asx_observability_capture(const asx_runtime *rt,
                                                          const asx_evidence_sink *sink,
                                                          asx_observability_snapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_OBSERVABILITY_OBSERVABILITY_H */
