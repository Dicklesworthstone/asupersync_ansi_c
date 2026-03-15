/*
 * evidence_sink.c — explicit evidence sink helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>

asx_status asx_evidence_sink_summarize(const asx_evidence_sink *sink, asx_evidence_summary *out) {
    if (sink == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    out->count = sink->count;
    out->pass_count = sink->pass_count;
    out->warn_count = sink->warn_count;
    out->fail_count = sink->fail_count;
    out->info_count = sink->info_count;
    out->verdict = asx_evidence_verdict(sink);
    return ASX_OK;
}

const asx_evidence_entry *asx_evidence_sink_get(const asx_evidence_sink *sink, uint32_t index) {
    if (sink == NULL || index >= sink->count) return NULL;
    return &sink->entries[index];
}

asx_status asx_evidence_sink_render_ndjson(const asx_evidence_sink *sink, asx_report_buf *out) {
    uint32_t i;

    if (sink == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_init(out);
    for (i = 0; i < sink->count; i++) {
        const asx_evidence_entry *entry = &sink->entries[i];
        asx_report_buf_append(out, "{\"seq\":");
        asx_report_buf_append_u32(out, entry->sequence);
        asx_report_buf_append(out, ",\"source\":\"");
        asx_report_buf_append(out, entry->source ? entry->source : "");
        asx_report_buf_append(out, "\",\"level\":\"");
        asx_report_buf_append(out, asx_evidence_level_str(entry->level));
        asx_report_buf_append(out, "\",\"message\":\"");
        asx_report_buf_append(out, entry->message ? entry->message : "");
        asx_report_buf_append(out, "\",\"entity\":");
        asx_report_buf_append_hex64(out, entry->entity_id);
        asx_report_buf_append(out, "}\n");
    }

    return ASX_OK;
}
