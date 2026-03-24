/*
 * tracing_compat.c — compatibility export over trace ring
 *
 * SPDX-License-Identifier: MIT
 */

#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/tracing_compat/tracing_compat.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS

asx_status asx_tracing_compat_format_event(const asx_trace_event *event, asx_report_buf *out) {
    if (event == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_init(out);
    asx_report_buf_append(out, "{\"sequence\":");
    asx_report_buf_append_u32(out, event->sequence);
    asx_report_buf_append(out, ",\"kind\":\"");
    asx_report_buf_append(out, asx_trace_event_kind_str(event->kind));
    asx_report_buf_append(out, "\",\"entity\":");
    asx_report_buf_append_hex64(out, event->entity_id);
    asx_report_buf_append(out, ",\"aux\":");
    asx_report_buf_append_hex64(out, event->aux);
    asx_report_buf_append(out, "}");

    return ASX_OK;
}

asx_status asx_tracing_compat_emit_event(const asx_trace_event *event, asx_tracing_compat_sink_fn sink,
                                         void *user_data) {
    asx_report_buf out;
    asx_status st;

    if (event == NULL || sink == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_tracing_compat_format_event(event, &out);
    if (st != ASX_OK) return st;

    sink(asx_report_buf_cstr(&out), user_data);
    return ASX_OK;
}

asx_status asx_tracing_compat_export_current(asx_tracing_compat_sink_fn sink, void *user_data,
                                             uint32_t *out_count) {
    uint32_t i;
    uint32_t count;
    asx_trace_event event;
    asx_status st;

    if (sink == NULL) return ASX_E_INVALID_ARGUMENT;

    count = asx_trace_event_count();
    for (i = 0; i < count; i++) {
        if (!asx_trace_event_get(i, &event)) return ASX_E_INVALID_STATE;
        st = asx_tracing_compat_emit_event(&event, sink, user_data);
        if (st != ASX_OK) return st;
    }

    if (out_count != NULL) *out_count = count;
    return ASX_OK;
}
