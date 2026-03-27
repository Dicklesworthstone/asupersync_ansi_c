/*
 * asx/evidence_sink/evidence_sink.h — explicit evidence sink helpers
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_EVIDENCE_SINK_EVIDENCE_SINK_H
#define ASX_EVIDENCE_SINK_EVIDENCE_SINK_H

#include <asx/app/report.h>
#include <asx/runtime/diagnostic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t count;
    uint32_t pass_count;
    uint32_t warn_count;
    uint32_t fail_count;
    uint32_t info_count;
    asx_evidence_level verdict;
} asx_evidence_summary;

/* Summarize evidence sink into counts and verdict. */
ASX_API ASX_MUST_USE asx_status asx_evidence_sink_summarize(const asx_evidence_sink *sink,
                                                            asx_evidence_summary *out);

/* Get evidence entry at the given index. */
ASX_API const asx_evidence_entry *asx_evidence_sink_get(const asx_evidence_sink *sink,
                                                        uint32_t index);

/* Render the sink as newline-delimited JSON.
 * Source and message fields are JSON-escaped before emission. */
ASX_API ASX_MUST_USE asx_status asx_evidence_sink_render_ndjson(const asx_evidence_sink *sink,
                                                                asx_report_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_EVIDENCE_SINK_EVIDENCE_SINK_H */
