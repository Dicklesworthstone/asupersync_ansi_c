/*
 * test_evidence.c — unit tests for explicit evidence families
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>
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
        fn();                                                                                      \
        g_pass++;                                                                                  \
    } while (0)

static void test_evidence_level_str(void) {
    ASSERT(strcmp(asx_evidence_level_str(ASX_EVIDENCE_PASS), "PASS") == 0, "pass string");
    ASSERT(strcmp(asx_evidence_level_str(ASX_EVIDENCE_FAIL), "FAIL") == 0, "fail string");
}

static void test_evidence_entry_helpers(void) {
    asx_evidence_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.level = ASX_EVIDENCE_FAIL;
    entry.entity_id = 9u;

    ASSERT(asx_evidence_entry_has_entity(&entry), "has entity");
    ASSERT(asx_evidence_is_failure(&entry), "failure detected");
}

static void test_evidence_sink_summary(void) {
    asx_evidence_sink sink;
    asx_evidence_summary summary;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "t:a", ASX_EVIDENCE_PASS, "ok", 0));
    MUST_OK(asx_evidence_record(&sink, "t:b", ASX_EVIDENCE_WARN, "warn", 7));
    MUST_OK(asx_evidence_sink_summarize(&sink, &summary));

    ASSERT(summary.count == 2u, "count");
    ASSERT(summary.warn_count == 1u, "warn count");
    ASSERT(summary.verdict == ASX_EVIDENCE_WARN, "warn verdict");
}

static void test_evidence_sink_get_and_ndjson(void) {
    asx_evidence_sink sink;
    asx_report_buf out;
    const asx_evidence_entry *entry;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "t:a", ASX_EVIDENCE_INFO, "note", 11u));
    entry = asx_evidence_sink_get(&sink, 0u);
    ASSERT(entry != NULL, "entry returned");
    ASSERT(entry->entity_id == 11u, "entity preserved");

    MUST_OK(asx_evidence_sink_render_ndjson(&sink, &out));
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"source\":\"t:a\"") != NULL, "ndjson source");
    ASSERT(strstr(asx_report_buf_cstr(&out), "\"level\":\"INFO\"") != NULL, "ndjson level");
}

static void test_evidence_sink_ndjson_escapes_strings(void) {
    asx_evidence_sink sink;
    asx_report_buf out;
    const char *rendered;

    asx_evidence_sink_init(&sink);
    MUST_OK(asx_evidence_record(&sink, "obs\"core\\pipe", ASX_EVIDENCE_WARN,
                                "line1\nline2\t\"quoted\"", 0));

    MUST_OK(asx_evidence_sink_render_ndjson(&sink, &out));
    rendered = asx_report_buf_cstr(&out);

    ASSERT(strstr(rendered, "\"source\":\"obs\\\"core\\\\pipe\"") != NULL, "source escaped");
    ASSERT(strstr(rendered, "\"message\":\"line1\\nline2\\t\\\"quoted\\\"\"") != NULL,
           "message escaped");
}

int main(void) {
    printf("test_evidence:\n");

    RUN(test_evidence_level_str);
    RUN(test_evidence_entry_helpers);
    RUN(test_evidence_sink_summary);
    RUN(test_evidence_sink_get_and_ndjson);
    RUN(test_evidence_sink_ndjson_escapes_strings);

    printf("\n  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
