/*
 * test_log.h — structured JSONL test log emission (bd-1md.11)
 *
 * Provides a minimal, zero-dependency structured log emitter for all
 * test layers (unit, invariant, conformance, e2e, bench). Each test
 * run emits JSONL records to a log file conforming to
 * schemas/test_log.schema.json.
 *
 * Usage:
 *   #include "test_log.h"
 *
 *   int main(void) {
 *       test_log_open("unit", "core/budget", "test_budget");
 *       // ... run tests via test_harness.h ...
 *       test_log_close();
 *   }
 *
 * The log file is written to the ASX_TEST_LOG_DIR directory
 * (default: build/test-logs/) with naming convention:
 *   {layer}-{suite}-{run_id}.jsonl
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TEST_LOG_H
#define ASX_TEST_LOG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------- */

#ifndef ASX_TEST_LOG_DIR
#define ASX_TEST_LOG_DIR "build/test-logs"
#endif

/* -------------------------------------------------------------------
 * Log state (file-scoped static; one log per test binary)
 * ------------------------------------------------------------------- */

static FILE *tlog_fp = NULL;
static uint32_t tlog_event_idx = 0;
static const char *tlog_run_id = NULL;
static const char *tlog_layer = NULL;
static const char *tlog_subsystem = NULL;
static const char *tlog_suite = NULL;
static char tlog_ts_buf[64];

/* -------------------------------------------------------------------
 * Timestamp helper
 * ------------------------------------------------------------------- */

__attribute__((unused)) static void tlog_now(void) {
    time_t t = time(NULL);
    struct tm *gm = gmtime(&t);
    if (gm != NULL) {
        strftime(tlog_ts_buf, sizeof(tlog_ts_buf), "%Y-%m-%dT%H:%M:%SZ", gm);
    } else {
        strncpy(tlog_ts_buf, "1970-01-01T00:00:00Z", sizeof(tlog_ts_buf));
        tlog_ts_buf[sizeof(tlog_ts_buf) - 1] = '\0';
    }
}

/* -------------------------------------------------------------------
 * JSON string escaping (minimal: handles \, ", newlines)
 * ------------------------------------------------------------------- */

__attribute__((unused)) static void tlog_write_json_str(FILE *f, const char *s) {
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

/* -------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------- */

__attribute__((unused)) static void test_log_open(const char *layer, const char *subsystem,
                                                  const char *suite) {
    char path[512];
    char run_id_buf[128];

    tlog_now();

    /* Build run_id: layer-YYYYMMDDTHHMMSSZ */
    snprintf(run_id_buf, sizeof(run_id_buf), "%s-%s", layer, tlog_ts_buf);
    /* Replace colons with nothing for filesystem safety */
    {
        char *p = run_id_buf;
        while (*p) {
            if (*p == ':') *p = '-';
            p++;
        }
    }

    /* Static storage for run_id (persists for duration) */
    {
        static char run_id_store[128];
        strncpy(run_id_store, run_id_buf, sizeof(run_id_store) - 1);
        run_id_store[sizeof(run_id_store) - 1] = '\0';
        tlog_run_id = run_id_store;
    }

    tlog_layer = layer;
    tlog_subsystem = subsystem;
    tlog_suite = suite;
    tlog_event_idx = 0;

    /* Open file only if ASX_TEST_LOG environment is set or directory exists */
    {
        const char *log_dir = ASX_TEST_LOG_DIR;
        const char *env_dir = NULL;
        /* Check environment override */
        env_dir = getenv("ASX_TEST_LOG_DIR");
        if (env_dir != NULL && env_dir[0] != '\0') { log_dir = env_dir; }

        snprintf(path, sizeof(path), "%s/%s-%s.jsonl", log_dir, layer, suite);
        tlog_fp = fopen(path, "a");
        /* Silently skip if directory doesn't exist (no failure) */
    }
}

__attribute__((unused)) static void test_log_close(void) {
    if (tlog_fp != NULL) {
        fclose(tlog_fp);
        tlog_fp = NULL;
    }
}

/* -------------------------------------------------------------------
 * Record emission: test result
 * ------------------------------------------------------------------- */

__attribute__((unused)) static void test_log_result(const char *test_name, const char *status,
                                                    const char *err_file, int err_line,
                                                    const char *err_assertion) {
    if (tlog_fp == NULL) return;

    tlog_now();

    fprintf(tlog_fp, "{\"ts\":");
    tlog_write_json_str(tlog_fp, tlog_ts_buf);
    fprintf(tlog_fp, ",\"run_id\":");
    tlog_write_json_str(tlog_fp, tlog_run_id);
    fprintf(tlog_fp, ",\"layer\":");
    tlog_write_json_str(tlog_fp, tlog_layer);
    fprintf(tlog_fp, ",\"subsystem\":");
    tlog_write_json_str(tlog_fp, tlog_subsystem);
    fprintf(tlog_fp, ",\"suite\":");
    tlog_write_json_str(tlog_fp, tlog_suite);
    fprintf(tlog_fp, ",\"test\":");
    tlog_write_json_str(tlog_fp, test_name);
    fprintf(tlog_fp, ",\"status\":");
    tlog_write_json_str(tlog_fp, status);
    fprintf(tlog_fp, ",\"event_index\":%u", (unsigned)tlog_event_idx);
    tlog_event_idx++;

    if (err_file != NULL && strcmp(status, "fail") == 0) {
        fprintf(tlog_fp, ",\"error\":{\"file\":");
        tlog_write_json_str(tlog_fp, err_file);
        fprintf(tlog_fp, ",\"line\":%d", err_line);
        if (err_assertion != NULL) {
            fprintf(tlog_fp, ",\"assertion\":");
            tlog_write_json_str(tlog_fp, err_assertion);
        }
        fprintf(tlog_fp, "}");
    }

    fprintf(tlog_fp, "}\n");
    fflush(tlog_fp);
}

/* -------------------------------------------------------------------
 * Record emission: suite summary
 * ------------------------------------------------------------------- */

__attribute__((unused)) static void test_log_summary(int total, int passed, int failed) {
    if (tlog_fp == NULL) return;

    tlog_now();

    fprintf(tlog_fp, "{\"ts\":");
    tlog_write_json_str(tlog_fp, tlog_ts_buf);
    fprintf(tlog_fp, ",\"run_id\":");
    tlog_write_json_str(tlog_fp, tlog_run_id);
    fprintf(tlog_fp, ",\"layer\":");
    tlog_write_json_str(tlog_fp, tlog_layer);
    fprintf(tlog_fp, ",\"subsystem\":");
    tlog_write_json_str(tlog_fp, tlog_subsystem);
    fprintf(tlog_fp, ",\"suite\":");
    tlog_write_json_str(tlog_fp, tlog_suite);
    fprintf(tlog_fp, ",\"status\":");
    tlog_write_json_str(tlog_fp, failed > 0 ? "fail" : "pass");
    fprintf(tlog_fp, ",\"event_index\":%u", (unsigned)tlog_event_idx);
    tlog_event_idx++;
    fprintf(tlog_fp, ",\"test\":\"_summary\"");
    fprintf(tlog_fp, ",\"metrics\":{\"count\":%d", total);
    fprintf(tlog_fp, ",\"passed\":%d,\"failed\":%d}", passed, failed);
    fprintf(tlog_fp, "}\n");
    fflush(tlog_fp);
}

#endif /* ASX_TEST_LOG_H */
