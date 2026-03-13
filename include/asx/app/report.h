/*
 * asx/app/report.h — structured report formatting and logging
 *
 * Provides user-facing diagnostic output:
 *   - Doctor report rendering (text and JSON)
 *   - Evidence sink rendering
 *   - Structured logging for scenario runs
 *   - Failure artifact packaging
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_APP_REPORT_H
#define ASX_APP_REPORT_H

#include <asx/app/doctor.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/rt.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Report output buffer (fixed-size, no allocation)
 * ------------------------------------------------------------------- */

#ifndef ASX_REPORT_BUF_SIZE
#define ASX_REPORT_BUF_SIZE 4096u
#endif

typedef struct {
    char data[ASX_REPORT_BUF_SIZE];
    uint32_t len;
} asx_report_buf;

/* Initialize a report buffer. */
ASX_API void asx_report_buf_init(asx_report_buf *buf);

/* Append a string to the buffer. Truncates if full. */
ASX_API void asx_report_buf_append(asx_report_buf *buf, const char *str);

/* Append a formatted integer. */
ASX_API void asx_report_buf_append_u32(asx_report_buf *buf, uint32_t val);

/* Append a formatted 64-bit hex value. */
ASX_API void asx_report_buf_append_hex64(asx_report_buf *buf, uint64_t val);

/* Get the buffer contents as a NUL-terminated string. */
ASX_API const char *asx_report_buf_cstr(const asx_report_buf *buf);

/* -------------------------------------------------------------------
 * Doctor report rendering
 * ------------------------------------------------------------------- */

/* Render a doctor report as human-readable text.
 * Format:
 *   [OK]   runtime: runtime initialized
 *   [WARN] regions: region arena >75% utilized (6/8)
 *   ---
 *   Overall: OK (6 pass, 0 warn, 0 fail)
 */
ASX_API ASX_MUST_USE asx_status asx_report_doctor_text(const asx_doctor_report *report,
                                                       asx_report_buf *out);

/* Render a doctor report as JSON. */
ASX_API ASX_MUST_USE asx_status asx_report_doctor_json(const asx_doctor_report *report,
                                                       asx_report_buf *out);

/* -------------------------------------------------------------------
 * Evidence sink rendering
 * ------------------------------------------------------------------- */

/* Render an evidence sink as human-readable text.
 * Format:
 *   [PASS] inspect:runtime — runtime initialized
 *   [WARN] inspect:regions — region arena >75% utilized
 *   ---
 *   Verdict: PASS (4 pass, 1 warn, 0 fail, 1 info)
 */
ASX_API ASX_MUST_USE asx_status asx_report_evidence_text(const asx_evidence_sink *sink,
                                                         asx_report_buf *out);

/* Render an evidence sink as JSON. */
ASX_API ASX_MUST_USE asx_status asx_report_evidence_json(const asx_evidence_sink *sink,
                                                         asx_report_buf *out);

/* -------------------------------------------------------------------
 * Inspection report rendering
 * ------------------------------------------------------------------- */

/* Render an inspection report as human-readable text. */
ASX_API ASX_MUST_USE asx_status asx_report_inspection_text(const asx_inspection_report *report,
                                                           asx_report_buf *out);

/* -------------------------------------------------------------------
 * Structured logging
 * ------------------------------------------------------------------- */

/* Log level for structured output */
typedef enum {
    ASX_LOG_DEBUG = 0,
    ASX_LOG_INFO = 1,
    ASX_LOG_WARN = 2,
    ASX_LOG_ERROR = 3
} asx_log_level;

/* Log callback type. Set via asx_log_set_callback. */
typedef void (*asx_log_fn)(asx_log_level level, const char *source, const char *message,
                           void *user_data);

/* Set the global log callback (NULL to disable logging). */
ASX_API void asx_log_set_callback(asx_log_fn fn, void *user_data);

/* Emit a structured log message. No-op if no callback set. */
ASX_API void asx_log_emit(asx_log_level level, const char *source, const char *message);

/* Get the string name for a log level. */
ASX_API const char *asx_log_level_str(asx_log_level level);

/* -------------------------------------------------------------------
 * Failure artifact: one-shot diagnostic bundle
 * ------------------------------------------------------------------- */

/* Capture a failure artifact: runs doctor + inspect + evidence,
 * formats as text into the output buffer. Designed for use when
 * a scenario or task fails — provides full diagnostic context. */
ASX_API ASX_MUST_USE asx_status asx_report_failure_artifact(const asx_runtime *rt,
                                                            asx_report_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* ASX_APP_REPORT_H */
