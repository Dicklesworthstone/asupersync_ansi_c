/*
 * test_console.c — unit tests for console render helpers and public test log
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/console/console.h>
#include <asx/runtime/runtime.h>
#include <asx/testing/log.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                       \
            g_fail++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        printf("  " #fn "...\n");                                                                  \
        asx_runtime_reset();                                                                       \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

static void test_console_run_doctor_text(void) {
    asx_runtime rt;
    asx_report_buf out;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_console_run_doctor(&rt, ASX_CONSOLE_FORMAT_TEXT, &out));

    ASSERT(strstr(asx_report_buf_cstr(&out), "Overall: OK") != NULL, "doctor text rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "io_driver") != NULL, "io driver rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "blocking") != NULL, "blocking rendered");
    asx_runtime_shutdown(&rt);
}

static void test_console_run_doctor_json(void) {
    asx_runtime rt;
    asx_report_buf out;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_console_run_doctor(&rt, ASX_CONSOLE_FORMAT_JSON, &out));

    ASSERT(strstr(asx_report_buf_cstr(&out), "\"overall\":\"OK\"") != NULL, "doctor json rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"name\":\"io_driver\"") != NULL,
           "io driver json rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"name\":\"blocking\"") != NULL,
           "blocking json rendered");
    asx_runtime_shutdown(&rt);
}

static void test_console_run_inspection_text(void) {
    asx_runtime rt;
    asx_report_buf out;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_console_run_inspection(&rt, ASX_CONSOLE_FORMAT_TEXT, &out));

    ASSERT(strstr(asx_report_buf_cstr(&out), "Runtime Inspection:") != NULL,
           "inspection text rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "io_driver") != NULL, "inspection io rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "blocking") != NULL, "inspection blocking rendered");
    asx_runtime_shutdown(&rt);
}

static void test_console_run_inspection_json_rejected(void) {
    asx_runtime rt;
    asx_report_buf out;

    MUST_OK(asx_runtime_init_default(&rt));
    ASSERT(asx_console_run_inspection(&rt, ASX_CONSOLE_FORMAT_JSON, &out) == ASX_E_INVALID_ARGUMENT,
           "inspection json rejected");
    asx_runtime_shutdown(&rt);
}

static void test_console_emit_log_record(void) {
    asx_report_buf out;

    MUST_OK(asx_console_emit_log_record(ASX_LOG_WARN, "operator", "rerun with --verbose", &out));
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"level\":\"WARN\"") != NULL, "warn level rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"source\":\"operator\"") != NULL, "source rendered");
}

static void test_console_emit_log_record_escapes_json(void) {
    asx_report_buf out;
    static const char message[] = {'l', 'i', 'n', 'e', '1', '\n', '\t', '"', 'x', '"', '\x01', '\0'};

    MUST_OK(asx_console_emit_log_record(ASX_LOG_INFO, "ops\"core\\pipe", message, &out));
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"source\":\"ops\\\"core\\\\pipe\"") != NULL,
           "source escaped");
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"message\":\"line1\\n\\t\\\"x\\\"\\u0001\"") != NULL,
           "message escaped");
}

static void test_public_test_log_header_compiles_and_noops(void) {
    asx_test_log_open("unit", "console", "test_console");
    asx_test_log_result("smoke", "pass", NULL, 0, NULL);
    asx_test_log_summary(1, 1, 0);
    asx_test_log_close();
    ASSERT(1, "public test log helper usable");
}

static void test_console_null_args(void) {
    asx_report_buf out;

    ASSERT(asx_console_run_doctor(NULL, ASX_CONSOLE_FORMAT_TEXT, &out) == ASX_E_INVALID_ARGUMENT,
           "doctor null runtime rejected");
    ASSERT(asx_console_run_inspection(NULL, ASX_CONSOLE_FORMAT_TEXT, &out) ==
               ASX_E_INVALID_ARGUMENT,
           "inspection null runtime rejected");
    ASSERT(asx_console_emit_log_record(ASX_LOG_INFO, NULL, "x", &out) == ASX_E_INVALID_ARGUMENT,
           "null source rejected");
}

static void test_console_color_mode(void) {
    asx_color_mode mode;
    asx_console_set_color_mode(ASX_COLOR_MODE_NONE);
    mode = asx_console_get_color_mode();
    ASSERT(mode == ASX_COLOR_MODE_NONE, "color mode set to none");

    asx_console_set_color_mode(ASX_COLOR_MODE_256);
    mode = asx_console_get_color_mode();
    ASSERT(mode == ASX_COLOR_MODE_256, "color mode set to 256");
}

static void test_console_styled_write(void) {
    asx_report_buf out;
    asx_console_style style;

    asx_console_set_color_mode(ASX_COLOR_MODE_256);
    asx_console_style_init(&style);
    style.fg = ASX_COLOR_RED;
    style.bold = 1;

    MUST_OK(asx_console_styled_write(&out, &style, "error text"));
    ASSERT(strstr(asx_report_buf_cstr(&out), "error text") != NULL, "styled text present");
}

static void test_console_bold_and_colored(void) {
    asx_report_buf out;

    asx_console_set_color_mode(ASX_COLOR_MODE_256);
    MUST_OK(asx_console_bold(&out, "important"));
    ASSERT(strstr(asx_report_buf_cstr(&out), "important") != NULL, "bold text present");

    MUST_OK(asx_console_colored(&out, ASX_COLOR_GREEN, "success"));
    ASSERT(strstr(asx_report_buf_cstr(&out), "success") != NULL, "colored text present");
}

static void test_console_str_width(void) {
    ASSERT(asx_console_str_width("hello") == 5, "ASCII width = length");
    ASSERT(asx_console_str_width("") == 0, "empty string width = 0");
    ASSERT(asx_console_str_width(NULL) == 0, "NULL width = 0");
}

static void test_console_clear_and_cursor(void) {
    asx_report_buf out;
    MUST_OK(asx_console_clear(&out));
    MUST_OK(asx_console_cursor_hide(&out));
    MUST_OK(asx_console_cursor_show(&out));
}

int main(void) {
    printf("test_console:\n");

    RUN(test_console_run_doctor_text);
    RUN(test_console_run_doctor_json);
    RUN(test_console_run_inspection_text);
    RUN(test_console_run_inspection_json_rejected);
    RUN(test_console_emit_log_record);
    RUN(test_console_emit_log_record_escapes_json);
    RUN(test_public_test_log_header_compiles_and_noops);
    RUN(test_console_null_args);
    RUN(test_console_color_mode);
    RUN(test_console_styled_write);
    RUN(test_console_bold_and_colored);
    RUN(test_console_str_width);
    RUN(test_console_clear_and_cursor);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
