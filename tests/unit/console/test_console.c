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

int main(void) {
    printf("test_console:\n");

    RUN(test_console_run_doctor_text);
    RUN(test_console_run_doctor_json);
    RUN(test_console_run_inspection_text);
    RUN(test_console_run_inspection_json_rejected);
    RUN(test_console_emit_log_record);
    RUN(test_public_test_log_header_compiles_and_noops);
    RUN(test_console_null_args);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
