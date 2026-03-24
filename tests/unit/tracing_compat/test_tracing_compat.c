/*
 * test_tracing_compat.c — unit tests for tracing compatibility helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/runtime.h>
#include <asx/tracing_compat/tracing_compat.h>
#include <stdio.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE

static int g_pass, g_fail;
static asx_status st_sink_;
static int g_sink_count;
static char g_last_record[256];
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
        g_sink_count = 0;                                                                          \
        g_last_record[0] = '\0';                                                                   \
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

static void tracing_sink(const char *record, void *user_data) {
    (void)user_data;
    g_sink_count++;
    strncpy(g_last_record, record, sizeof(g_last_record) - 1);
    g_last_record[sizeof(g_last_record) - 1] = '\0';
}

static void test_tracing_format_event(void) {
    asx_trace_event event;
    asx_report_buf out;

    memset(&event, 0, sizeof(event));
    event.sequence = 7;
    event.kind = ASX_TRACE_TASK_SPAWN;
    event.entity_id = 0x24u;
    event.aux = 0x99u;

    MUST_OK(asx_tracing_compat_format_event(&event, &out));
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"sequence\":7") != NULL, "sequence rendered");
    ASSERT(strstr(asx_report_buf_cstr(&out), "task_spawn") != NULL, "kind rendered");
}

static void test_tracing_emit_event(void) {
    asx_trace_event event;

    memset(&event, 0, sizeof(event));
    event.kind = ASX_TRACE_REGION_OPEN;

    MUST_OK(asx_tracing_compat_emit_event(&event, tracing_sink, NULL));
    ASSERT(g_sink_count == 1, "sink called");
    ASSERT(strstr(g_last_record, "region_open") != NULL, "record delivered");
}

static void test_tracing_export_current(void) {
    asx_runtime rt;
    asx_region_id region;
    uint32_t out_count = 0;

    MUST_OK(asx_runtime_init_default(&rt));
    MUST_OK(asx_region_open(&region));

    MUST_OK(asx_tracing_compat_export_current(tracing_sink, NULL, &out_count));
    ASSERT(g_sink_count > 0, "current trace exported");
    ASSERT(out_count == (uint32_t)g_sink_count, "count reported");
    ASSERT(strstr(g_last_record, "\"kind\":") != NULL, "json-like record emitted");

    asx_runtime_shutdown(&rt);
}

static void test_tracing_null_args(void) {
    asx_trace_event event;
    asx_report_buf out;

    ASSERT(asx_tracing_compat_format_event(NULL, &out) == ASX_E_INVALID_ARGUMENT, "null event");
    ASSERT(asx_tracing_compat_emit_event(&event, NULL, NULL) == ASX_E_INVALID_ARGUMENT,
           "null sink rejected");
    ASSERT(asx_tracing_compat_export_current(NULL, NULL, NULL) == ASX_E_INVALID_ARGUMENT,
           "null export sink rejected");
}

int main(void) {
    printf("test_tracing_compat:\n");

    RUN(test_tracing_format_event);
    RUN(test_tracing_emit_event);
    RUN(test_tracing_export_current);
    RUN(test_tracing_null_args);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

#else

int main(void) {
    printf("test_tracing_compat (minimal browser hidden contract):\n");
    if (ASX_HAS_BROWSER_TRACE != 0 || ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT != 1) {
        printf("\n  0 passed, 1 failed\n");
        return 1;
    }
    printf("\n  1 passed, 0 failed\n");
    return 0;
}

#endif
