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

/* Render a doctor report to the given buffer in the specified format. */
ASX_API ASX_MUST_USE asx_status asx_console_render_doctor(const asx_doctor_report *report,
                                                          asx_console_format format,
                                                          asx_report_buf *out);

/* Render an inspection report to the given buffer in the specified format. */
ASX_API ASX_MUST_USE asx_status asx_console_render_inspection(const asx_inspection_report *report,
                                                              asx_console_format format,
                                                              asx_report_buf *out);

/* Render evidence sink contents to the given buffer in the specified format. */
ASX_API ASX_MUST_USE asx_status asx_console_render_evidence(const asx_evidence_sink *sink,
                                                            asx_console_format format,
                                                            asx_report_buf *out);

/* Run the doctor check on a runtime and render results to the buffer. */
ASX_API ASX_MUST_USE asx_status asx_console_run_doctor(const asx_runtime *rt,
                                                       asx_console_format format,
                                                       asx_report_buf *out);

/* Run a runtime inspection and render results to the buffer. */
ASX_API ASX_MUST_USE asx_status asx_console_run_inspection(const asx_runtime *rt,
                                                           asx_console_format format,
                                                           asx_report_buf *out);

/* Emit a formatted log record at the given level to the buffer. */
ASX_API ASX_MUST_USE asx_status asx_console_emit_log_record(asx_log_level level, const char *source,
                                                            const char *message,
                                                            asx_report_buf *out);

/* -------------------------------------------------------------------
 * Console style and color
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_COLOR_MODE_NONE = 0,
    ASX_COLOR_MODE_ANSI = 1,
    ASX_COLOR_MODE_256  = 2,
    ASX_COLOR_MODE_TRUE = 3
} asx_color_mode;

typedef enum {
    ASX_COLOR_BLACK   = 0,
    ASX_COLOR_RED     = 1,
    ASX_COLOR_GREEN   = 2,
    ASX_COLOR_YELLOW  = 3,
    ASX_COLOR_BLUE    = 4,
    ASX_COLOR_MAGENTA = 5,
    ASX_COLOR_CYAN    = 6,
    ASX_COLOR_WHITE   = 7,
    ASX_COLOR_DEFAULT = 9
} asx_ansi_color;

typedef struct {
    asx_ansi_color fg;
    asx_ansi_color bg;
    uint8_t bold;
    uint8_t dim;
    uint8_t italic;
    uint8_t underline;
} asx_console_style;

/* Detect the best color mode for stdout. */
ASX_API asx_color_mode asx_console_detect_color_mode(void);

/* Set the global color mode. */
ASX_API void asx_console_set_color_mode(asx_color_mode mode);

/* Get the current color mode. */
ASX_API asx_color_mode asx_console_get_color_mode(void);

/* Initialize a style to defaults (no styling). */
ASX_API void asx_console_style_init(asx_console_style *style);

/* Write styled text to the report buffer. */
ASX_API asx_status asx_console_styled_write(asx_report_buf *out, const asx_console_style *style,
                                              const char *text);

/* Write bold text. */
ASX_API asx_status asx_console_bold(asx_report_buf *out, const char *text);

/* Write colored text. */
ASX_API asx_status asx_console_colored(asx_report_buf *out, asx_ansi_color fg, const char *text);

/* Clear the screen (emit ANSI clear sequence). */
ASX_API asx_status asx_console_clear(asx_report_buf *out);

/* Hide/show cursor. */
ASX_API asx_status asx_console_cursor_hide(asx_report_buf *out);
ASX_API asx_status asx_console_cursor_show(asx_report_buf *out);

/* Measure the display width of a string (ASCII only in core profile). */
ASX_API uint32_t asx_console_str_width(const char *text);

/* Print directly to stderr (convenience for operator output). */
ASX_API void asx_console_eprintln(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CONSOLE_CONSOLE_H */
