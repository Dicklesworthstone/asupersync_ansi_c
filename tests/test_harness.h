/*
 * test_harness.h — minimal test harness for asx unit/invariant tests
 *
 * No external dependencies. Uses only standard C.
 * Provides assertion macros, test registration, and result reporting.
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

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do { \
    test_count++; \
    test_current_failed = 0; \
    test_##name(); \
    if (test_current_failed) { \
        fprintf(stderr, "  FAIL: %s\n", #name); \
    } else { \
        fprintf(stderr, "  PASS: %s\n", #name); \
    } \
} while (0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "    ASSERT_TRUE failed: %s (%s:%d)\n", \
                #expr, __FILE__, __LINE__); \
        test_current_failed = 1; \
        test_failures++; \
        return; \
    } \
} while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "    ASSERT_EQ failed: %s != %s (%s:%d)\n", \
                #a, #b, __FILE__, __LINE__); \
        test_current_failed = 1; \
        test_failures++; \
        return; \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    if ((a) == (b)) { \
        fprintf(stderr, "    ASSERT_NE failed: %s == %s (%s:%d)\n", \
                #a, #b, __FILE__, __LINE__); \
        test_current_failed = 1; \
        test_failures++; \
        return; \
    } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "    ASSERT_STR_EQ failed: \"%s\" != \"%s\" (%s:%d)\n", \
                (a), (b), __FILE__, __LINE__); \
        test_current_failed = 1; \
        test_failures++; \
        return; \
    } \
} while (0)

#define TEST_REPORT() do { \
    fprintf(stderr, "\n%d/%d tests passed\n", \
            test_count - test_failures, test_count); \
    if (test_failures > 0) { \
        fprintf(stderr, "%d FAILURES\n", test_failures); \
    } \
} while (0)

#endif /* ASX_TEST_HARNESS_H */
