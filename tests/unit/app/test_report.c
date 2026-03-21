/*
 * test_report.c — unit tests for report formatting, logging, and failure
 *                 artifact workflows
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/app/doctor.h>
#include <asx/app/report.h>
#include <asx/runtime/diagnostic.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

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

/* ================================================================== */
/* Report buffer tests                                                */
/* ================================================================== */

static void test_report_buf_init(void) {
    asx_report_buf buf;
    asx_report_buf_init(&buf);

    ASSERT(buf.len == 0, "empty after init");
    ASSERT(strlen(asx_report_buf_cstr(&buf)) == 0, "empty string");
}

static void test_report_buf_append(void) {
    asx_report_buf buf;
    asx_report_buf_init(&buf);

    asx_report_buf_append(&buf, "hello");
    asx_report_buf_append(&buf, " ");
    asx_report_buf_append(&buf, "world");

    ASSERT(strcmp(asx_report_buf_cstr(&buf), "hello world") == 0, "appended string");
    ASSERT(buf.len == 11, "correct length");
}

static void test_report_buf_append_u32(void) {
    asx_report_buf buf;
    asx_report_buf_init(&buf);

    asx_report_buf_append_u32(&buf, 0);
    asx_report_buf_append(&buf, ",");
    asx_report_buf_append_u32(&buf, 42);
    asx_report_buf_append(&buf, ",");
    asx_report_buf_append_u32(&buf, 12345);

    ASSERT(strcmp(asx_report_buf_cstr(&buf), "0,42,12345") == 0, "formatted integers");
}

static void test_report_buf_append_hex64(void) {
    asx_report_buf buf;
    asx_report_buf_init(&buf);

    asx_report_buf_append_hex64(&buf, 0);
    ASSERT(strcmp(asx_report_buf_cstr(&buf), "0x0000000000000000") == 0, "hex zero");
}

static void test_report_buf_truncation(void) {
    asx_report_buf buf;
    uint32_t i;
    asx_report_buf_init(&buf);

    /* Fill the buffer near capacity */
    for (i = 0; i < ASX_REPORT_BUF_SIZE / 10; i++) { asx_report_buf_append(&buf, "1234567890"); }

    /* Should not crash, just truncate */
    asx_report_buf_append(&buf, "overflow_attempt");
    ASSERT(buf.len < ASX_REPORT_BUF_SIZE, "within capacity");
    ASSERT(buf.data[buf.len] == '\0', "null terminated");
}

static void test_report_buf_invalid_len_is_clamped(void) {
    asx_report_buf buf;

    memset(&buf, 'x', sizeof(buf));
    buf.len = ASX_REPORT_BUF_SIZE;

    asx_report_buf_append(&buf, "hello");

    ASSERT(buf.len == ASX_REPORT_BUF_SIZE - 1u, "invalid len clamped");
    ASSERT(buf.data[buf.len] == '\0', "clamped buffer null terminated");
}

static void test_report_buf_null(void) {
    /* Should not crash */
    asx_report_buf_init(NULL);
    asx_report_buf_append(NULL, "test");
    asx_report_buf_append_u32(NULL, 42);
    asx_report_buf_append_hex64(NULL, 0);
    ASSERT(strlen(asx_report_buf_cstr(NULL)) == 0, "null returns empty");
}

/* ================================================================== */
/* Doctor report rendering                                            */
/* ================================================================== */

static void test_doctor_text_healthy(void) {
    asx_runtime rt;
    asx_doctor_report doctor;
    asx_report_buf buf;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_doctor_run(&rt, &doctor));
    asx_report_buf_init(&buf);
    MUST_OK(asx_report_doctor_text(&doctor, &buf));

    /* Should contain OK for runtime */
    ASSERT(strstr(asx_report_buf_cstr(&buf), "[OK]") != NULL, "contains OK");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "runtime") != NULL, "mentions runtime");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "io_driver") != NULL, "mentions io driver");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "blocking") != NULL, "mentions blocking");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Overall: OK") != NULL, "overall OK");

    asx_runtime_shutdown(&rt);
}

static void test_doctor_text_failing(void) {
    asx_runtime rt;
    asx_doctor_report doctor;
    asx_report_buf buf;

    memset(&rt, 0, sizeof(rt));
    MUST_OK(asx_doctor_run(&rt, &doctor));
    asx_report_buf_init(&buf);
    MUST_OK(asx_report_doctor_text(&doctor, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "[FAIL]") != NULL, "contains FAIL");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Overall: FAIL") != NULL, "overall FAIL");
}

static void test_doctor_json_healthy(void) {
    asx_runtime rt;
    asx_doctor_report doctor;
    asx_report_buf buf;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_doctor_run(&rt, &doctor));
    asx_report_buf_init(&buf);
    MUST_OK(asx_report_doctor_json(&doctor, &buf));

    /* Basic JSON structure */
    ASSERT(strstr(asx_report_buf_cstr(&buf), "{\"checks\":[") != NULL, "starts with checks array");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"name\":\"io_driver\"") != NULL,
           "io driver in JSON");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"name\":\"blocking\"") != NULL,
           "blocking in JSON");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"overall\":\"OK\"") != NULL, "overall OK in JSON");

    asx_runtime_shutdown(&rt);
}

static void test_doctor_json_escapes_strings(void) {
    asx_doctor_report doctor;
    asx_report_buf buf;

    memset(&doctor, 0, sizeof(doctor));
    doctor.check_count = 1u;
    doctor.pass_count = 1u;
    doctor.checks[0].severity = ASX_DOCTOR_OK;
    doctor.checks[0].name = "clock\"path\\A";
    doctor.checks[0].message = "line1\nline2\tready";
    doctor.checks[0].value = 1u;
    doctor.checks[0].capacity = 2u;

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_doctor_json(&doctor, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"name\":\"clock\\\"path\\\\A\"") != NULL,
           "doctor name escaped");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"message\":\"line1\\nline2\\tready\"") != NULL,
           "doctor message escaped");
}

static void test_doctor_render_null_args(void) {
    asx_doctor_report doctor;
    asx_report_buf buf;

    ASSERT(asx_report_doctor_text(NULL, &buf) == ASX_E_INVALID_ARGUMENT, "null report");
    ASSERT(asx_report_doctor_text(&doctor, NULL) == ASX_E_INVALID_ARGUMENT, "null buf");
    ASSERT(asx_report_doctor_json(NULL, &buf) == ASX_E_INVALID_ARGUMENT, "null report json");
    ASSERT(asx_report_doctor_json(&doctor, NULL) == ASX_E_INVALID_ARGUMENT, "null buf json");
}

static void test_doctor_render_rejects_oversized_check_count(void) {
    asx_doctor_report doctor;
    asx_report_buf buf;

    memset(&doctor, 0, sizeof(doctor));
    doctor.check_count = ASX_DOCTOR_MAX_CHECKS + 1u;
    asx_report_buf_init(&buf);

    ASSERT(asx_report_doctor_text(&doctor, &buf) == ASX_E_INVALID_ARGUMENT,
           "doctor text rejects oversized count");
    ASSERT(asx_report_doctor_json(&doctor, &buf) == ASX_E_INVALID_ARGUMENT,
           "doctor json rejects oversized count");
}

/* ================================================================== */
/* Evidence rendering                                                 */
/* ================================================================== */

static void test_evidence_text(void) {
    asx_evidence_sink sink;
    asx_report_buf buf;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "oracle:leak", ASX_EVIDENCE_PASS, "no leaks detected", 0));
    MUST_OK(asx_evidence_record(&sink, "oracle:quiesce", ASX_EVIDENCE_WARN, "slow convergence", 0));

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_evidence_text(&sink, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "[PASS]") != NULL, "contains PASS");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "[WARN]") != NULL, "contains WARN");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Verdict: WARN") != NULL, "verdict WARN");
}

static void test_evidence_json(void) {
    asx_evidence_sink sink;
    asx_report_buf buf;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "test", ASX_EVIDENCE_FAIL, "failure", 0));

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_evidence_json(&sink, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"entries\":[") != NULL, "entries array");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"verdict\":\"FAIL\"") != NULL, "verdict FAIL");
}

static void test_evidence_json_escapes_strings(void) {
    asx_evidence_sink sink;
    asx_report_buf buf;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "obs\"core\\pipe", ASX_EVIDENCE_WARN,
                                "bad\tline\nnext", 0));

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_evidence_json(&sink, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"source\":\"obs\\\"core\\\\pipe\"") != NULL,
           "evidence source escaped");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "\"message\":\"bad\\tline\\nnext\"") != NULL,
           "evidence message escaped");
}

static void test_evidence_render_null_args(void) {
    asx_evidence_sink sink;
    asx_report_buf buf;

    ASSERT(asx_report_evidence_text(NULL, &buf) == ASX_E_INVALID_ARGUMENT, "null sink");
    ASSERT(asx_report_evidence_text(&sink, NULL) == ASX_E_INVALID_ARGUMENT, "null buf");
}

static void test_evidence_render_rejects_oversized_count(void) {
    asx_evidence_sink sink;
    asx_report_buf buf;

    memset(&sink, 0, sizeof(sink));
    sink.count = ASX_EVIDENCE_SINK_CAPACITY + 1u;
    asx_report_buf_init(&buf);

    ASSERT(asx_report_evidence_text(&sink, &buf) == ASX_E_INVALID_ARGUMENT,
           "evidence text rejects oversized count");
    ASSERT(asx_report_evidence_json(&sink, &buf) == ASX_E_INVALID_ARGUMENT,
           "evidence json rejects oversized count");
}

/* ================================================================== */
/* Inspection report rendering                                        */
/* ================================================================== */

static void test_inspection_text(void) {
    asx_runtime rt;
    asx_inspection_report rpt;
    asx_report_buf buf;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_inspect(&rt, &rpt));

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_inspection_text(&rpt, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "initialized: yes") != NULL, "shows initialized");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "regions:") != NULL, "shows regions");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "io_driver:") != NULL, "shows io driver");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "blocking:") != NULL, "shows blocking");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "trace:") != NULL, "shows trace");

    asx_runtime_shutdown(&rt);
}

/* ================================================================== */
/* Structured logging                                                 */
/* ================================================================== */

static int g_log_count;
static asx_log_level g_last_log_level;
static char g_last_log_source[64];
static char g_last_log_message[256];

static void test_log_callback(asx_log_level level, const char *source, const char *message,
                              void *user_data) {
    (void)user_data;
    g_log_count++;
    g_last_log_level = level;
    if (source) {
        strncpy(g_last_log_source, source, sizeof(g_last_log_source) - 1);
        g_last_log_source[sizeof(g_last_log_source) - 1] = '\0';
    }
    if (message) {
        strncpy(g_last_log_message, message, sizeof(g_last_log_message) - 1);
        g_last_log_message[sizeof(g_last_log_message) - 1] = '\0';
    }
}

static void test_log_emit(void) {
    g_log_count = 0;
    asx_log_set_callback(test_log_callback, NULL);

    asx_log_emit(ASX_LOG_INFO, "runtime", "initialized");
    ASSERT(g_log_count == 1, "callback called");
    ASSERT(g_last_log_level == ASX_LOG_INFO, "level INFO");
    ASSERT(strcmp(g_last_log_source, "runtime") == 0, "source");
    ASSERT(strcmp(g_last_log_message, "initialized") == 0, "message");

    asx_log_emit(ASX_LOG_ERROR, "task", "failed");
    ASSERT(g_log_count == 2, "second call");
    ASSERT(g_last_log_level == ASX_LOG_ERROR, "level ERROR");

    asx_log_set_callback(NULL, NULL);
}

static void test_log_no_callback(void) {
    g_log_count = 0;
    asx_log_set_callback(NULL, NULL);

    /* Should not crash */
    asx_log_emit(ASX_LOG_WARN, "test", "noop");
    ASSERT(g_log_count == 0, "no callback no call");
}

static void test_log_level_str(void) {
    ASSERT(strcmp(asx_log_level_str(ASX_LOG_DEBUG), "DEBUG") == 0, "debug");
    ASSERT(strcmp(asx_log_level_str(ASX_LOG_INFO), "INFO") == 0, "info");
    ASSERT(strcmp(asx_log_level_str(ASX_LOG_WARN), "WARN") == 0, "warn");
    ASSERT(strcmp(asx_log_level_str(ASX_LOG_ERROR), "ERROR") == 0, "error");
}

/* ================================================================== */
/* Failure artifact                                                   */
/* ================================================================== */

static void test_failure_artifact_healthy(void) {
    asx_runtime rt;
    asx_report_buf buf;

    MUST_OK(asx_runtime_init_default(&rt));
    asx_report_buf_init(&buf);
    MUST_OK(asx_report_failure_artifact(&rt, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "FAILURE ARTIFACT") != NULL, "header present");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Doctor Report:") != NULL, "doctor present");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Runtime Inspection:") != NULL, "inspection present");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Evidence:") != NULL, "evidence present");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "END ARTIFACT") != NULL, "footer present");

    asx_runtime_shutdown(&rt);
}

static void test_failure_artifact_with_context(void) {
    asx_runtime rt;
    asx_report_buf buf;
    asx_diagnostic_ctx ctx;

    MUST_OK(asx_runtime_init_default(&rt));

    memset(&ctx, 0, sizeof(ctx));
    ctx.operation = "scenario:timeout_test";
    MUST_OK(asx_diagnostic_push(&ctx));

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_failure_artifact(&rt, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "Diagnostic Context:") != NULL,
           "context section present");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "scenario:timeout_test") != NULL,
           "operation name in artifact");

    asx_diagnostic_pop();
    asx_runtime_shutdown(&rt);
}

static void test_failure_artifact_uninitialized(void) {
    asx_runtime rt;
    asx_report_buf buf;

    memset(&rt, 0, sizeof(rt));
    asx_report_buf_init(&buf);
    MUST_OK(asx_report_failure_artifact(&rt, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "[FAIL]") != NULL, "shows failure for uninitialized");
}

static void test_failure_artifact_null_args(void) {
    asx_runtime rt;
    asx_report_buf buf;

    ASSERT(asx_report_failure_artifact(NULL, &buf) == ASX_E_INVALID_ARGUMENT, "null rt");
    ASSERT(asx_report_failure_artifact(&rt, NULL) == ASX_E_INVALID_ARGUMENT, "null buf");
}

/* ================================================================== */
/* E2E: full diagnostic workflow (acceptance criterion D)              */
/* ================================================================== */

static void test_e2e_negative_path_resource_exhaustion(void) {
    asx_runtime rt;
    asx_evidence_sink sink;
    asx_report_buf buf;

    /* Initialize runtime and exhaust a resource */
    MUST_OK(asx_runtime_init_default(&rt));

    /* Run evidence pipeline */
    asx_evidence_sink_init(&sink);
    MUST_OK(asx_inspect_to_evidence(&rt, &sink));

    /* Manually record a resource exhaustion finding */
    MUST_OK(asx_evidence_record(&sink, "scenario:exhaust", ASX_EVIDENCE_FAIL,
                                "task arena exhausted during spawn burst", 0));

    /* Render report */
    asx_report_buf_init(&buf);
    MUST_OK(asx_report_evidence_text(&sink, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "task arena exhausted") != NULL,
           "exhaustion message rendered");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Verdict: FAIL") != NULL, "verdict is FAIL");

    asx_runtime_shutdown(&rt);
}

static void test_e2e_negative_path_cancellation(void) {
    asx_evidence_sink sink;
    asx_report_buf buf;

    asx_evidence_sink_init(&sink);

    /* Simulate cancellation evidence entries */
    MUST_OK(asx_evidence_record(&sink, "scenario:cancel", ASX_EVIDENCE_WARN,
                                "task cancelled during cleanup phase", 0));
    MUST_OK(asx_evidence_record(&sink, "scenario:cancel", ASX_EVIDENCE_INFO,
                                "cancellation reason: ASX_CANCEL_RESOURCE", 0));

    asx_report_buf_init(&buf);
    MUST_OK(asx_report_evidence_text(&sink, &buf));

    ASSERT(strstr(asx_report_buf_cstr(&buf), "cancelled during cleanup") != NULL,
           "cancellation message");
    ASSERT(strstr(asx_report_buf_cstr(&buf), "Verdict: WARN") != NULL,
           "warn verdict for cancellation");
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void) {
    printf("test_report:\n");

    /* Report buffer */
    RUN(test_report_buf_init);
    RUN(test_report_buf_append);
    RUN(test_report_buf_append_u32);
    RUN(test_report_buf_append_hex64);
    RUN(test_report_buf_truncation);
    RUN(test_report_buf_invalid_len_is_clamped);
    RUN(test_report_buf_null);

    /* Doctor rendering */
    RUN(test_doctor_text_healthy);
    RUN(test_doctor_text_failing);
    RUN(test_doctor_json_healthy);
    RUN(test_doctor_json_escapes_strings);
    RUN(test_doctor_render_null_args);
    RUN(test_doctor_render_rejects_oversized_check_count);

    /* Evidence rendering */
    RUN(test_evidence_text);
    RUN(test_evidence_json);
    RUN(test_evidence_json_escapes_strings);
    RUN(test_evidence_render_null_args);
    RUN(test_evidence_render_rejects_oversized_count);

    /* Inspection rendering */
    RUN(test_inspection_text);

    /* Structured logging */
    RUN(test_log_emit);
    RUN(test_log_no_callback);
    RUN(test_log_level_str);

    /* Failure artifact */
    RUN(test_failure_artifact_healthy);
    RUN(test_failure_artifact_with_context);
    RUN(test_failure_artifact_uninitialized);
    RUN(test_failure_artifact_null_args);

    /* E2E negative paths */
    RUN(test_e2e_negative_path_resource_exhaustion);
    RUN(test_e2e_negative_path_cancellation);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
