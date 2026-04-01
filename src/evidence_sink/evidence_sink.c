/*
 * evidence_sink.c — explicit evidence sink helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>

static void append_json_control_escape(asx_report_buf *out, unsigned char byte) {
    static const char hex[] = "0123456789abcdef";
    char escape[7];

    if (out == NULL) return;

    escape[0] = '\\';
    escape[1] = 'u';
    escape[2] = '0';
    escape[3] = '0';
    escape[4] = hex[(byte >> 4) & 0x0f];
    escape[5] = hex[byte & 0x0f];
    escape[6] = '\0';
    asx_report_buf_append(out, escape);
}

static void append_json_escaped(asx_report_buf *out, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    char ch[2];

    if (out == NULL || text == NULL) return;
    ch[1] = '\0';

    while (*p != '\0') {
        switch (*p) {
        case '\\': asx_report_buf_append(out, "\\\\"); break;
        case '"': asx_report_buf_append(out, "\\\""); break;
        case '\n': asx_report_buf_append(out, "\\n"); break;
        case '\r': asx_report_buf_append(out, "\\r"); break;
        case '\t': asx_report_buf_append(out, "\\t"); break;
        case '\b': asx_report_buf_append(out, "\\b"); break;
        case '\f': asx_report_buf_append(out, "\\f"); break;
        default:
            if (*p < 0x20u) {
                append_json_control_escape(out, *p);
                break;
            }
            ch[0] = (char)*p;
            asx_report_buf_append(out, ch);
            break;
        }
        p++;
    }
}

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
        append_json_escaped(out, entry->source ? entry->source : "");
        asx_report_buf_append(out, "\",\"level\":\"");
        asx_report_buf_append(out, asx_evidence_level_str(entry->level));
        asx_report_buf_append(out, "\",\"message\":\"");
        append_json_escaped(out, entry->message ? entry->message : "");
        asx_report_buf_append(out, "\",\"entity\":");
        asx_report_buf_append_hex64(out, entry->entity_id);
        asx_report_buf_append(out, "}\n");
    }

    return ASX_OK;
}
