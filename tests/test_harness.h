/*
 * test_harness.h — minimal test harness for asx unit/invariant tests
 *
 * No external dependencies. Uses only standard C.
 * Provides assertion macros, test registration, and result reporting.
 *
 * Structured JSONL logging (bd-1md.11):
 *   When ASX_TEST_LOG_DIR is set (env or build/test-logs by default),
 *   the harness emits per-test JSONL records conforming to
 *   schemas/test_log.schema.json alongside the human-readable stderr
 *   output. Include test_log.h before this header to enable.
 *
 * Usage:
 *   #include "test_harness.h"
 *
 *   TEST(test_name) {
 *       ASSERT_EQ(asx_status_str(ASX_OK), "OK");
 *       ASSERT_TRUE(asx_is_error(ASX_E_CANCELLED));
 *   }
 *
 *   int main(void) {
 *       RUN_TEST(test_name);
 *       TEST_REPORT();
 *       return test_failures;
 *   }
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_TEST_HARNESS_H
#define ASX_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int test_count = 0;
static int test_failures = 0;
static int test_current_failed = 0;
static const char *test_fail_file = NULL;
static int test_fail_line = 0;
static const char *test_fail_expr = NULL;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        test_count++;                                                                              \
        test_current_failed = 0;                                                                   \
        test_fail_file = NULL;                                                                     \
        test_fail_line = 0;                                                                        \
        test_fail_expr = NULL;                                                                     \
        test_##name();                                                                             \
        if (test_current_failed) {                                                                 \
            fprintf(stderr, "  FAIL: %s\n", #name);                                                \
        } else {                                                                                   \
            fprintf(stderr, "  PASS: %s\n", #name);                                                \
        }                                                                                          \
        TEST_LOG_RESULT_(#name);                                                                   \
    } while (0)

/* Structured log hook — no-op unless test_log.h is included before this. */
#ifdef ASX_TEST_LOG_H
#define TEST_LOG_RESULT_(name)                                                                     \
    test_log_result(name, test_current_failed ? "fail" : "pass", test_fail_file, test_fail_line,   \
                    test_fail_expr)
#define TEST_LOG_SUMMARY_() test_log_summary(test_count, test_count - test_failures, test_failures)
#else
#define TEST_LOG_RESULT_(name) ((void)0)
#define TEST_LOG_SUMMARY_() ((void)0)
#endif

#define ASSERT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "    ASSERT_TRUE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__);    \
            test_current_failed = 1;                                                               \
            test_fail_file = __FILE__;                                                             \
            test_fail_line = __LINE__;                                                             \
            test_fail_expr = #expr;                                                                \
            test_failures++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            fprintf(stderr, "    ASSERT_EQ failed: %s != %s (%s:%d)\n", #a, #b, __FILE__,          \
                    __LINE__);                                                                     \
            test_current_failed = 1;                                                               \
            test_fail_file = __FILE__;                                                             \
            test_fail_line = __LINE__;                                                             \
            test_fail_expr = #a " == " #b;                                                         \
            test_failures++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NE(a, b)                                                                            \
    do {                                                                                           \
        if ((a) == (b)) {                                                                          \
            fprintf(stderr, "    ASSERT_NE failed: %s == %s (%s:%d)\n", #a, #b, __FILE__,          \
                    __LINE__);                                                                     \
            test_current_failed = 1;                                                               \
            test_fail_file = __FILE__;                                                             \
            test_fail_line = __LINE__;                                                             \
            test_fail_expr = #a " != " #b;                                                         \
            test_failures++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                        \
    do {                                                                                           \
        if (strcmp((a), (b)) != 0) {                                                               \
            fprintf(stderr, "    ASSERT_STR_EQ failed: \"%s\" != \"%s\" (%s:%d)\n", (a), (b),      \
                    __FILE__, __LINE__);                                                           \
            test_current_failed = 1;                                                               \
            test_fail_file = __FILE__;                                                             \
            test_fail_line = __LINE__;                                                             \
            test_fail_expr = #a " streq " #b;                                                      \
            test_failures++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_REPORT()                                                                              \
    do {                                                                                           \
        fprintf(stderr, "\n%d/%d tests passed\n", test_count - test_failures, test_count);         \
        if (test_failures > 0) { fprintf(stderr, "%d FAILURES\n", test_failures); }                \
        TEST_LOG_SUMMARY_();                                                                       \
    } while (0)

#endif /* ASX_TEST_HARNESS_H */
