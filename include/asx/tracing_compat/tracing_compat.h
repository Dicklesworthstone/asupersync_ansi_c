/*
 * asx/tracing_compat/tracing_compat.h — compatibility export over trace ring
 *
 * Provides a lightweight compatibility layer for tooling that expects a
 * tracing-style event feed without depending on an external framework.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TRACING_COMPAT_TRACING_COMPAT_H
#define ASX_TRACING_COMPAT_TRACING_COMPAT_H

#include <asx/asx_config.h>
#include <asx/app/report.h>
#include <asx/asx_export.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE || \
    defined(ASX_INTERNAL_TRACE_FAMILY_ACCESS)

#include <asx/runtime/trace.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*asx_tracing_compat_sink_fn)(const char *record, void *user_data);

/* Initialize the tracing compatibility layer. */
ASX_API ASX_MUST_USE asx_status asx_tracing_compat_format_event(const asx_trace_event *event,
                                                                asx_report_buf *out);

/* Shut down the tracing compatibility layer. */
ASX_API ASX_MUST_USE asx_status asx_tracing_compat_emit_event(const asx_trace_event *event,
                                                              asx_tracing_compat_sink_fn sink,
                                                              void *user_data);

/* Emit a tracing-compatible span event. */
ASX_API ASX_MUST_USE asx_status asx_tracing_compat_export_current(
    asx_tracing_compat_sink_fn sink, void *user_data, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* trace family public contract */

#endif /* ASX_TRACING_COMPAT_TRACING_COMPAT_H */
