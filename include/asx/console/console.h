/*
 * asx/console/console.h — operator-facing console/report helpers
 *
 * Provides a thin public console family over the shipped app/report
 * surface so operator workflows are reachable without bespoke glue.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CONSOLE_CONSOLE_H
#define ASX_CONSOLE_CONSOLE_H

#include <asx/app/report.h>
#include <asx/asx_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ASX_CONSOLE_FORMAT_TEXT = 0, ASX_CONSOLE_FORMAT_JSON = 1 } asx_console_format;

ASX_API ASX_MUST_USE asx_status asx_console_render_doctor(const asx_doctor_report *report,
                                                          asx_console_format format,
                                                          asx_report_buf *out);

ASX_API ASX_MUST_USE asx_status asx_console_render_inspection(const asx_inspection_report *report,
                                                              asx_console_format format,
                                                              asx_report_buf *out);

ASX_API ASX_MUST_USE asx_status asx_console_render_evidence(const asx_evidence_sink *sink,
                                                            asx_console_format format,
                                                            asx_report_buf *out);

ASX_API ASX_MUST_USE asx_status asx_console_run_doctor(const asx_runtime *rt,
                                                       asx_console_format format,
                                                       asx_report_buf *out);

ASX_API ASX_MUST_USE asx_status asx_console_run_inspection(const asx_runtime *rt,
                                                           asx_console_format format,
                                                           asx_report_buf *out);

ASX_API ASX_MUST_USE asx_status asx_console_emit_log_record(asx_log_level level, const char *source,
                                                            const char *message,
                                                            asx_report_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CONSOLE_CONSOLE_H */
