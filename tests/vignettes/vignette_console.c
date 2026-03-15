/*
 * vignette_console.c — operator-path smoke for console/tracing helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

static void emit_trace_record(const char *record, void *user_data) {
    FILE *out = (FILE *)user_data;
    fprintf(out, "trace=%s\n", record);
}

int main(void) {
    asx_app_args args;
    const char *argv[] = {"asx", "doctor", "--verbose"};
    asx_runtime rt;
    asx_report_buf doctor;
    asx_report_buf log_record;
    uint32_t exported = 0;
    asx_region_id region;

    if (asx_app_parse_args(&args, 3, argv) != ASX_OK) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }

    if (asx_runtime_init_default(&rt) != ASX_OK) {
        fprintf(stderr, "runtime init failed\n");
        return 1;
    }

    if (asx_region_open(&region) != ASX_OK) {
        fprintf(stderr, "region open failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    if (asx_console_run_doctor(&rt, ASX_CONSOLE_FORMAT_TEXT, &doctor) != ASX_OK) {
        fprintf(stderr, "doctor failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    if (strstr(asx_report_buf_cstr(&doctor), "Overall: OK") == NULL) {
        fprintf(stderr, "missing doctor summary\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    if (asx_console_emit_log_record(ASX_LOG_INFO, "operator",
                                    "rerun with rch exec -- make build/tests/vignettes/vignette_console",
                                    &log_record)
        != ASX_OK) {
        fprintf(stderr, "log record failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    printf("console_doctor:\n%s", asx_report_buf_cstr(&doctor));
    printf("console_log:%s\n", asx_report_buf_cstr(&log_record));

    if (asx_tracing_compat_export_current(emit_trace_record, stdout, &exported) != ASX_OK) {
        fprintf(stderr, "trace export failed\n");
        asx_runtime_shutdown(&rt);
        return 1;
    }

    printf("trace_exported=%u\n", (unsigned)exported);
    asx_runtime_shutdown(&rt);
    return 0;
}
