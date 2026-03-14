/*
 * asx/testing/log.h — public structured JSONL test-log helper
 *
 * Mirrors the repo's internal test logging behavior as a supported
 * public helper for smoke packs and operator-facing verification tools.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TESTING_LOG_H
#define ASX_TESTING_LOG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef ASX_TESTING_LOG_DIR
#define ASX_TESTING_LOG_DIR "build/test-logs"
#endif

__attribute__((unused)) static FILE *asx_test_log_fp = NULL;
__attribute__((unused)) static uint32_t asx_test_log_event_idx = 0;
__attribute__((unused)) static const char *asx_test_log_run_id = NULL;
__attribute__((unused)) static const char *asx_test_log_layer = NULL;
__attribute__((unused)) static const char *asx_test_log_subsystem = NULL;
__attribute__((unused)) static const char *asx_test_log_suite = NULL;
__attribute__((unused)) static char asx_test_log_ts_buf[64];

__attribute__((unused)) static void asx_test_log_now(void) {
    time_t t = time(NULL);
    struct tm *gm = gmtime(&t);
    if (gm != NULL) {
        strftime(asx_test_log_ts_buf, sizeof(asx_test_log_ts_buf), "%Y-%m-%dT%H:%M:%SZ", gm);
    } else {
        strncpy(asx_test_log_ts_buf, "1970-01-01T00:00:00Z", sizeof(asx_test_log_ts_buf));
        asx_test_log_ts_buf[sizeof(asx_test_log_ts_buf) - 1] = '\0';
    }
}

__attribute__((unused)) static void asx_test_log_write_json_str(FILE *f, const char *s) {
    fputc('"', f);
    if (s != NULL) {
        while (*s) {
            switch (*s) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default: fputc(*s, f); break;
            }
            s++;
        }
    }
    fputc('"', f);
}

__attribute__((unused)) static void asx_test_log_open(const char *layer, const char *subsystem,
                                                      const char *suite) {
    char path[512];
    char run_id_buf[128];
    const char *log_dir = ASX_TESTING_LOG_DIR;
    const char *env_dir = NULL;

    asx_test_log_now();
    snprintf(run_id_buf, sizeof(run_id_buf), "%s-%s", layer, asx_test_log_ts_buf);
    {
        char *p = run_id_buf;
        while (*p) {
            if (*p == ':') *p = '-';
            p++;
        }
    }
    {
        static char run_id_store[128];
        strncpy(run_id_store, run_id_buf, sizeof(run_id_store) - 1);
        run_id_store[sizeof(run_id_store) - 1] = '\0';
        asx_test_log_run_id = run_id_store;
    }

    asx_test_log_layer = layer;
    asx_test_log_subsystem = subsystem;
    asx_test_log_suite = suite;
    asx_test_log_event_idx = 0;

    env_dir = getenv("ASX_TEST_LOG_DIR");
    if (env_dir != NULL && env_dir[0] != '\0') log_dir = env_dir;

    snprintf(path, sizeof(path), "%s/%s-%s.jsonl", log_dir, layer, suite);
    asx_test_log_fp = fopen(path, "a");
}

__attribute__((unused)) static void asx_test_log_close(void) {
    if (asx_test_log_fp != NULL) {
        fclose(asx_test_log_fp);
        asx_test_log_fp = NULL;
    }
}

__attribute__((unused)) static void asx_test_log_result(const char *test_name, const char *status,
                                                        const char *err_file, int err_line,
                                                        const char *err_assertion) {
    if (asx_test_log_fp == NULL) return;

    asx_test_log_now();

    fprintf(asx_test_log_fp, "{\"ts\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_ts_buf);
    fprintf(asx_test_log_fp, ",\"run_id\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_run_id);
    fprintf(asx_test_log_fp, ",\"layer\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_layer);
    fprintf(asx_test_log_fp, ",\"subsystem\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_subsystem);
    fprintf(asx_test_log_fp, ",\"suite\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_suite);
    fprintf(asx_test_log_fp, ",\"test\":");
    asx_test_log_write_json_str(asx_test_log_fp, test_name);
    fprintf(asx_test_log_fp, ",\"status\":");
    asx_test_log_write_json_str(asx_test_log_fp, status);
    fprintf(asx_test_log_fp, ",\"event_index\":%u", (unsigned)asx_test_log_event_idx);
    asx_test_log_event_idx++;

    if (err_file != NULL && strcmp(status, "fail") == 0) {
        fprintf(asx_test_log_fp, ",\"error\":{\"file\":");
        asx_test_log_write_json_str(asx_test_log_fp, err_file);
        fprintf(asx_test_log_fp, ",\"line\":%d", err_line);
        if (err_assertion != NULL) {
            fprintf(asx_test_log_fp, ",\"assertion\":");
            asx_test_log_write_json_str(asx_test_log_fp, err_assertion);
        }
        fprintf(asx_test_log_fp, "}");
    }

    fprintf(asx_test_log_fp, "}\n");
    fflush(asx_test_log_fp);
}

__attribute__((unused)) static void asx_test_log_summary(int total, int passed, int failed) {
    if (asx_test_log_fp == NULL) return;

    asx_test_log_now();

    fprintf(asx_test_log_fp, "{\"ts\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_ts_buf);
    fprintf(asx_test_log_fp, ",\"run_id\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_run_id);
    fprintf(asx_test_log_fp, ",\"layer\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_layer);
    fprintf(asx_test_log_fp, ",\"subsystem\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_subsystem);
    fprintf(asx_test_log_fp, ",\"suite\":");
    asx_test_log_write_json_str(asx_test_log_fp, asx_test_log_suite);
    fprintf(asx_test_log_fp, ",\"status\":");
    asx_test_log_write_json_str(asx_test_log_fp, failed > 0 ? "fail" : "pass");
    fprintf(asx_test_log_fp, ",\"event_index\":%u", (unsigned)asx_test_log_event_idx);
    asx_test_log_event_idx++;
    fprintf(asx_test_log_fp, ",\"test\":\"_summary\"");
    fprintf(asx_test_log_fp, ",\"metrics\":{\"count\":%d", total);
    fprintf(asx_test_log_fp, ",\"passed\":%d,\"failed\":%d}", passed, failed);
    fprintf(asx_test_log_fp, "}\n");
    fflush(asx_test_log_fp);
}

#endif /* ASX_TESTING_LOG_H */
