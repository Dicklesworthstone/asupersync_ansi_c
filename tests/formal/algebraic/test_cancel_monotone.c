/*
 * test_cancel_monotone.c — exhaustive algebraic property verification
 *                          for cancel severity and strengthen operation
 *
 * Domain: 11 cancel kinds (USER=0 .. SHUTDOWN=10)
 * Total: 121 pairs for severity, 121 for strengthen
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("Algebraic property test — exhaustive enumeration") */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <string.h>

static asx_cancel_reason make_reason(asx_cancel_kind kind, asx_time ts) {
    asx_cancel_reason r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.timestamp = ts;
    return r;
}

/* --- Severity is monotone non-decreasing with enum order --- */
TEST(cancel_severity_monotone) {
    int k;
    int prev_sev = -1;
    for (k = 0; k <= 10; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        ASSERT_TRUE(sev >= prev_sev);
        prev_sev = sev;
    }
}

/* --- Severity is bounded [0, 5] --- */
TEST(cancel_severity_bounded) {
    int k;
    for (k = 0; k <= 10; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        ASSERT_TRUE(sev >= 0);
        ASSERT_TRUE(sev <= 5);
    }
}

/* --- Out-of-range severity returns 5 (max) --- */
TEST(cancel_severity_out_of_range) {
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)(-1)), 5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)11), 5);
    ASSERT_EQ(asx_cancel_severity((asx_cancel_kind)99), 5);
}

/* --- Strengthen: result severity >= max(severity(a), severity(b)) --- */
TEST(cancel_strengthen_monotone_severity) {
    int a, b;
    for (a = 0; a <= 10; a++) {
        for (b = 0; b <= 10; b++) {
            asx_cancel_reason ra = make_reason((asx_cancel_kind)a, 100);
            asx_cancel_reason rb = make_reason((asx_cancel_kind)b, 200);
            asx_cancel_reason result = asx_cancel_strengthen(&ra, &rb);
            int sev_a = asx_cancel_severity((asx_cancel_kind)a);
            int sev_b = asx_cancel_severity((asx_cancel_kind)b);
            int expected_max = sev_a > sev_b ? sev_a : sev_b;
            int result_sev = asx_cancel_severity(result.kind);
            ASSERT_TRUE(result_sev >= expected_max);
        }
    }
}

/* --- Strengthen: equal severity uses earlier timestamp --- */
TEST(cancel_strengthen_deterministic_tiebreak) {
    int a, b;
    for (a = 0; a <= 10; a++) {
        for (b = 0; b <= 10; b++) {
            int sev_a = asx_cancel_severity((asx_cancel_kind)a);
            int sev_b = asx_cancel_severity((asx_cancel_kind)b);
            if (sev_a != sev_b) continue;

            /* a has earlier timestamp => a should win */
            asx_cancel_reason ra = make_reason((asx_cancel_kind)a, 100);
            asx_cancel_reason rb = make_reason((asx_cancel_kind)b, 200);
            asx_cancel_reason result = asx_cancel_strengthen(&ra, &rb);
            ASSERT_EQ((int)result.timestamp, 100);

            /* b has earlier timestamp => b should win */
            ra = make_reason((asx_cancel_kind)a, 200);
            rb = make_reason((asx_cancel_kind)b, 100);
            result = asx_cancel_strengthen(&ra, &rb);
            ASSERT_EQ((int)result.timestamp, 100);
        }
    }
}

/* --- Strengthen: idempotent (strengthen(x,x) == x) --- */
TEST(cancel_strengthen_idempotent) {
    int k;
    for (k = 0; k <= 10; k++) {
        asx_cancel_reason r = make_reason((asx_cancel_kind)k, 42);
        asx_cancel_reason result = asx_cancel_strengthen(&r, &r);
        ASSERT_EQ((int)result.kind, k);
        ASSERT_EQ((int)result.timestamp, 42);
    }
}

/* --- Strengthen: higher severity always wins regardless of timestamp --- */
TEST(cancel_strengthen_higher_severity_wins) {
    int a, b;
    for (a = 0; a <= 10; a++) {
        for (b = 0; b <= 10; b++) {
            int sev_a = asx_cancel_severity((asx_cancel_kind)a);
            int sev_b = asx_cancel_severity((asx_cancel_kind)b);
            if (sev_a == sev_b) continue;

            /* Give the weaker one an earlier timestamp — shouldn't matter */
            asx_cancel_reason ra = make_reason((asx_cancel_kind)a, 1);
            asx_cancel_reason rb = make_reason((asx_cancel_kind)b, 1000);
            asx_cancel_reason result = asx_cancel_strengthen(&ra, &rb);
            int result_sev = asx_cancel_severity(result.kind);

            if (sev_a > sev_b) {
                ASSERT_EQ(result_sev, sev_a);
            } else {
                ASSERT_EQ(result_sev, sev_b);
            }
        }
    }
}

/* --- Associativity: strengthen(strengthen(a,b),c) == strengthen(a,strengthen(b,c)) --- */
TEST(cancel_strengthen_associative) {
    int a, b, c;
    for (a = 0; a <= 10; a++) {
        for (b = 0; b <= 10; b++) {
            for (c = 0; c <= 10; c++) {
                asx_cancel_reason ra = make_reason((asx_cancel_kind)a, 100);
                asx_cancel_reason rb = make_reason((asx_cancel_kind)b, 200);
                asx_cancel_reason rc = make_reason((asx_cancel_kind)c, 300);
                asx_cancel_reason ab = asx_cancel_strengthen(&ra, &rb);
                asx_cancel_reason ab_c = asx_cancel_strengthen(&ab, &rc);
                asx_cancel_reason bc = asx_cancel_strengthen(&rb, &rc);
                asx_cancel_reason a_bc = asx_cancel_strengthen(&ra, &bc);
                /* Severity of result must match */
                ASSERT_EQ(asx_cancel_severity(ab_c.kind), asx_cancel_severity(a_bc.kind));
            }
        }
    }
}

/* --- Cleanup budget: quota monotone decreasing with severity --- */
TEST(cancel_cleanup_budget_monotone_quota) {
    int k;
    uint32_t prev_quota = UINT32_MAX;
    int prev_sev = -1;
    for (k = 0; k <= 10; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        asx_budget b = asx_cancel_cleanup_budget((asx_cancel_kind)k);
        if (sev > prev_sev) {
            ASSERT_TRUE(b.poll_quota <= prev_quota);
            prev_quota = b.poll_quota;
            prev_sev = sev;
        }
    }
}

/* --- Cleanup budget: priority monotone increasing with severity --- */
TEST(cancel_cleanup_budget_monotone_priority) {
    int k;
    uint8_t prev_priority = 0;
    int prev_sev = -1;
    for (k = 0; k <= 10; k++) {
        int sev = asx_cancel_severity((asx_cancel_kind)k);
        asx_budget b = asx_cancel_cleanup_budget((asx_cancel_kind)k);
        if (sev > prev_sev) {
            ASSERT_TRUE(b.priority >= prev_priority);
            prev_priority = b.priority;
            prev_sev = sev;
        }
    }
}

int main(void) {
    fprintf(stderr, "[formal] cancel severity/strengthen algebraic properties\n");

    RUN_TEST(cancel_severity_monotone);
    RUN_TEST(cancel_severity_bounded);
    RUN_TEST(cancel_severity_out_of_range);
    RUN_TEST(cancel_strengthen_monotone_severity);
    RUN_TEST(cancel_strengthen_deterministic_tiebreak);
    RUN_TEST(cancel_strengthen_idempotent);
    RUN_TEST(cancel_strengthen_higher_severity_wins);
    RUN_TEST(cancel_strengthen_associative);
    RUN_TEST(cancel_cleanup_budget_monotone_quota);
    RUN_TEST(cancel_cleanup_budget_monotone_priority);

    TEST_REPORT();
    return test_failures;
}
