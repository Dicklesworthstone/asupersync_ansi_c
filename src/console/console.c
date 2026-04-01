/*
 * console.c — operator-facing console/report helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/console/console.h>
#include <stdio.h>
#include <string.h>

static void console_append_json_escaped(asx_report_buf *out, const char *text) {
    const unsigned char *p;

    if (out == NULL || text == NULL) return;

    p = (const unsigned char *)text;
    while (*p != '\0') {
        switch (*p) {
        case '"': asx_report_buf_append(out, "\\\""); break;
        case '\\': asx_report_buf_append(out, "\\\\"); break;
        case '\b': asx_report_buf_append(out, "\\b"); break;
        case '\f': asx_report_buf_append(out, "\\f"); break;
        case '\n': asx_report_buf_append(out, "\\n"); break;
        case '\r': asx_report_buf_append(out, "\\r"); break;
        case '\t': asx_report_buf_append(out, "\\t"); break;
        default:
            if (*p < 0x20u) {
                static const char hex[] = "0123456789abcdef";
                char esc[7];
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(*p >> 4) & 0x0fu];
                esc[5] = hex[*p & 0x0fu];
                esc[6] = '\0';
                asx_report_buf_append(out, esc);
            } else {
                char ch[2];
                ch[0] = (char)*p;
                ch[1] = '\0';
                asx_report_buf_append(out, ch);
            }
            break;
        }
        p++;
    }
}

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
    console_append_json_escaped(out, source);
    asx_report_buf_append(out, "\",\"message\":\"");
    console_append_json_escaped(out, message);
    asx_report_buf_append(out, "\"}");
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Console style and color                                             */
/* ------------------------------------------------------------------ */

static asx_color_mode g_color_mode = ASX_COLOR_MODE_NONE;

asx_color_mode asx_console_detect_color_mode(void) {
    /* In deterministic core, default to no color. Real detection
     * would check isatty() and TERM environment variable. */
    return ASX_COLOR_MODE_NONE;
}

void asx_console_set_color_mode(asx_color_mode mode) { g_color_mode = mode; }

asx_color_mode asx_console_get_color_mode(void) { return g_color_mode; }

void asx_console_style_init(asx_console_style *style) {
    if (style == NULL) return;
    memset(style, 0, sizeof(*style));
    style->fg = ASX_COLOR_DEFAULT;
    style->bg = ASX_COLOR_DEFAULT;
}

static void emit_ansi_start(asx_report_buf *out, const asx_console_style *style) {
    if (g_color_mode == ASX_COLOR_MODE_NONE) return;
    asx_report_buf_append(out, "\033[");
    if (style->bold) asx_report_buf_append(out, "1;");
    if (style->dim) asx_report_buf_append(out, "2;");
    if (style->italic) asx_report_buf_append(out, "3;");
    if (style->underline) asx_report_buf_append(out, "4;");
    /* Emit foreground color: 30-37 for named colors, 39 for default */
    {
        char fg_code[4];
        fg_code[0] = '3';
        fg_code[1] = (char)('0' + (int)style->fg);
        fg_code[2] = '\0';
        asx_report_buf_append(out, fg_code);
    }
    asx_report_buf_append(out, "m");
}

static void emit_ansi_reset(asx_report_buf *out) {
    if (g_color_mode == ASX_COLOR_MODE_NONE) return;
    asx_report_buf_append(out, "\033[0m");
}

asx_status asx_console_styled_write(asx_report_buf *out, const asx_console_style *style,
                                    const char *text) {
    if (out == NULL || text == NULL) return ASX_E_INVALID_ARGUMENT;
    if (style != NULL) emit_ansi_start(out, style);
    asx_report_buf_append(out, text);
    if (style != NULL) emit_ansi_reset(out);
    return ASX_OK;
}

asx_status asx_console_bold(asx_report_buf *out, const char *text) {
    asx_console_style s;
    asx_console_style_init(&s);
    s.bold = 1u;
    return asx_console_styled_write(out, &s, text);
}

asx_status asx_console_colored(asx_report_buf *out, asx_ansi_color fg, const char *text) {
    asx_console_style s;
    asx_console_style_init(&s);
    s.fg = fg;
    return asx_console_styled_write(out, &s, text);
}

asx_status asx_console_clear(asx_report_buf *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_report_buf_append(out, "\033[2J\033[H");
    return ASX_OK;
}

asx_status asx_console_cursor_hide(asx_report_buf *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_report_buf_append(out, "\033[?25l");
    return ASX_OK;
}

asx_status asx_console_cursor_show(asx_report_buf *out) {
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    asx_report_buf_append(out, "\033[?25h");
    return ASX_OK;
}

uint32_t asx_console_str_width(const char *text) {
    uint32_t width = 0u;
    if (text == NULL) return 0u;
    /* ASCII-only width measurement in core profile */
    while (*text != '\0') {
        if ((unsigned char)*text >= 0x20u) width++;
        text++;
    }
    return width;
}

void asx_console_eprintln(const char *text) {
    if (text != NULL) {
        fputs(text, stderr);
        fputc('\n', stderr);
    }
}
