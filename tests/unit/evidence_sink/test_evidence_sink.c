/*
 * test_evidence_sink.c — unit tests for evidence sink helpers
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/evidence/evidence.h>
#include <asx/evidence_sink/evidence_sink.h>
#include <string.h>

static asx_evidence_sink g_sink;

static void setup(void) { asx_evidence_sink_init(&g_sink); }

/* ================================================================== */
/* Summarize                                                           */
/* ================================================================== */

TEST(summarize_empty) {
    asx_evidence_summary sum;
    setup();
    ASSERT_EQ(asx_evidence_sink_summarize(&g_sink, &sum), ASX_OK);
    ASSERT_EQ(sum.count, 0u);
    ASSERT_EQ(sum.pass_count, 0u);
    ASSERT_EQ(sum.fail_count, 0u);
}

TEST(summarize_mixed) {
    asx_evidence_summary sum;
    setup();
    ASSERT_EQ(asx_evidence_record(&g_sink, "test", ASX_EVIDENCE_PASS, "ok", 0), ASX_OK);
    ASSERT_EQ(asx_evidence_record(&g_sink, "test", ASX_EVIDENCE_FAIL, "bad", 0), ASX_OK);
    ASSERT_EQ(asx_evidence_record(&g_sink, "test", ASX_EVIDENCE_WARN, "hmm", 0), ASX_OK);
    ASSERT_EQ(asx_evidence_record(&g_sink, "test", ASX_EVIDENCE_INFO, "fyi", 0), ASX_OK);

    ASSERT_EQ(asx_evidence_sink_summarize(&g_sink, &sum), ASX_OK);
    ASSERT_EQ(sum.count, 4u);
    ASSERT_EQ(sum.pass_count, 1u);
    ASSERT_EQ(sum.fail_count, 1u);
    ASSERT_EQ(sum.warn_count, 1u);
    ASSERT_EQ(sum.info_count, 1u);
    ASSERT_EQ((int)sum.verdict, (int)ASX_EVIDENCE_FAIL);
}

TEST(summarize_null_args) {
    asx_evidence_summary sum;
    setup();
    ASSERT_EQ(asx_evidence_sink_summarize(NULL, &sum), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_evidence_sink_summarize(&g_sink, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Get entry                                                           */
/* ================================================================== */

TEST(get_valid_index) {
    const asx_evidence_entry *e;
    setup();
    ASSERT_EQ(asx_evidence_record(&g_sink, "src1", ASX_EVIDENCE_PASS, "msg1", 42), ASX_OK);

    e = asx_evidence_sink_get(&g_sink, 0);
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ((int)e->level, (int)ASX_EVIDENCE_PASS);
    ASSERT_EQ(e->entity_id, (uint64_t)42);
}

TEST(get_out_of_range) {
    setup();
    ASSERT_TRUE(asx_evidence_sink_get(&g_sink, 0) == NULL);
    ASSERT_TRUE(asx_evidence_sink_get(&g_sink, 999) == NULL);
}

TEST(get_null_sink) { ASSERT_TRUE(asx_evidence_sink_get(NULL, 0) == NULL); }

/* ================================================================== */
/* Render NDJSON                                                       */
/* ================================================================== */

TEST(render_ndjson_empty) {
    asx_report_buf buf;
    setup();
    ASSERT_EQ(asx_evidence_sink_render_ndjson(&g_sink, &buf), ASX_OK);
    /* Empty sink produces empty output */
    ASSERT_EQ(buf.len, 0u);
}

TEST(render_ndjson_with_entries) {
    asx_report_buf buf;
    setup();
    ASSERT_EQ(asx_evidence_record(&g_sink, "oracle", ASX_EVIDENCE_PASS, "all clear", 0), ASX_OK);
    ASSERT_EQ(asx_evidence_sink_render_ndjson(&g_sink, &buf), ASX_OK);
    ASSERT_TRUE(buf.len > 0u);
    /* Should contain JSON fields */
    ASSERT_TRUE(strstr(buf.data, "\"source\":\"oracle\"") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"level\":\"PASS\"") != NULL);
    ASSERT_TRUE(strstr(buf.data, "\"message\":\"all clear\"") != NULL);
}

TEST(render_ndjson_escapes_special_chars) {
    asx_report_buf buf;
    setup();
    ASSERT_EQ(asx_evidence_record(&g_sink, "test", ASX_EVIDENCE_INFO, "line1\nline2", 0), ASX_OK);
    ASSERT_EQ(asx_evidence_sink_render_ndjson(&g_sink, &buf), ASX_OK);
    /* Newline should be escaped as \n in JSON output */
    ASSERT_TRUE(strstr(buf.data, "line1\\nline2") != NULL);
}

TEST(render_ndjson_escapes_all_control_chars) {
    asx_report_buf buf;
    static const char message[] = {'a', '\b', '\f', '\x01', 'z', '\0'};

    setup();
    ASSERT_EQ(asx_evidence_record(&g_sink, "test", ASX_EVIDENCE_INFO, message, 0), ASX_OK);
    ASSERT_EQ(asx_evidence_sink_render_ndjson(&g_sink, &buf), ASX_OK);
    ASSERT_TRUE(strstr(buf.data, "\"message\":\"a\\b\\f\\u0001z\"") != NULL);
}

TEST(render_ndjson_null_args) {
    asx_report_buf buf;
    setup();
    ASSERT_EQ(asx_evidence_sink_render_ndjson(NULL, &buf), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_evidence_sink_render_ndjson(&g_sink, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_evidence_sink ===\n");

    RUN_TEST(summarize_empty);
    RUN_TEST(summarize_mixed);
    RUN_TEST(summarize_null_args);

    RUN_TEST(get_valid_index);
    RUN_TEST(get_out_of_range);
    RUN_TEST(get_null_sink);

    RUN_TEST(render_ndjson_empty);
    RUN_TEST(render_ndjson_with_entries);
    RUN_TEST(render_ndjson_escapes_special_chars);
    RUN_TEST(render_ndjson_escapes_all_control_chars);
    RUN_TEST(render_ndjson_null_args);

    TEST_REPORT();
    return test_failures;
}
