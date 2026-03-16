/*
 * console.c — operator-facing console/report helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/console/console.h>
#include <string.h>

asx_status asx_console_render_doctor(const asx_doctor_report *report, asx_console_format format,
                                     asx_report_buf *out) {
    if (report == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_report_buf_init(out);
    if (format == ASX_CONSOLE_FORMAT_JSON) { return asx_report_doctor_json(report, out); }
    return asx_report_doctor_text(report, out);
}

asx_status asx_console_render_inspection(const asx_inspection_report *report,
                                         asx_console_format format, asx_report_buf *out) {
    if (report == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (format != ASX_CONSOLE_FORMAT_TEXT) return ASX_E_INVALID_ARGUMENT;
    asx_report_buf_init(out);
    return asx_report_inspection_text(report, out);
}

asx_status asx_console_render_evidence(const asx_evidence_sink *sink, asx_console_format format,
                                       asx_report_buf *out) {
    if (sink == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_report_buf_init(out);
    if (format == ASX_CONSOLE_FORMAT_JSON) { return asx_report_evidence_json(sink, out); }
    return asx_report_evidence_text(sink, out);
}

asx_status asx_console_run_doctor(const asx_runtime *rt, asx_console_format format,
                                  asx_report_buf *out) {
    asx_doctor_report report;
    asx_status st;

    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_doctor_run(rt, &report);
    if (st != ASX_OK) return st;

    return asx_console_render_doctor(&report, format, out);
}

asx_status asx_console_run_inspection(const asx_runtime *rt, asx_console_format format,
                                      asx_report_buf *out) {
    asx_inspection_report report;
    asx_status st;

    if (rt == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_inspect(rt, &report);
    if (st != ASX_OK) return st;

    return asx_console_render_inspection(&report, format, out);
}

asx_status asx_console_emit_log_record(asx_log_level level, const char *source, const char *message,
                                       asx_report_buf *out) {
    if (source == NULL || message == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_report_buf_init(out);
    asx_report_buf_append(out, "{\"level\":\"");
    asx_report_buf_append(out, asx_log_level_str(level));
    asx_report_buf_append(out, "\",\"source\":\"");
    asx_report_buf_append(out, source);
    asx_report_buf_append(out, "\",\"message\":\"");
    asx_report_buf_append(out, message);
    asx_report_buf_append(out, "\"}");
    return ASX_OK;
}
