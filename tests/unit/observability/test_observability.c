/*
 * test_observability.c — unit tests for metrics and observability
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/observability/observability.h>

static asx_metrics g_m;

static void setup(void) { asx_metrics_init(&g_m); }

/* ================================================================== */
/* Metrics init                                                        */
/* ================================================================== */

TEST(metrics_init_empty) {
    setup();
    ASSERT_EQ(asx_metrics_count(&g_m), 0u);
}

/* ================================================================== */
/* Counter                                                             */
/* ================================================================== */

TEST(counter_register_and_increment) {
    const asx_metric_entry *e;
    setup();
    ASSERT_EQ(asx_metrics_counter(&g_m, "requests"), ASX_OK);
    ASSERT_EQ(asx_metrics_count(&g_m), 1u);

    ASSERT_EQ(asx_metrics_increment(&g_m, "requests", 1), ASX_OK);
    ASSERT_EQ(asx_metrics_increment(&g_m, "requests", 5), ASX_OK);

    e = asx_metrics_get(&g_m, "requests");
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ((int)e->type, (int)ASX_METRIC_COUNTER);
    ASSERT_EQ(e->value, (int64_t)6);
}

TEST(counter_not_found) {
    setup();
    ASSERT_EQ(asx_metrics_increment(&g_m, "nonexistent", 1), ASX_E_NOT_FOUND);
}

TEST(counter_increment_clamps_positive_overflow) {
    const asx_metric_entry *e;
    setup();
    ASSERT_EQ(asx_metrics_counter(&g_m, "requests"), ASX_OK);
    ASSERT_EQ(asx_metrics_increment(&g_m, "requests", INT64_MAX - 1), ASX_OK);
    ASSERT_EQ(asx_metrics_increment(&g_m, "requests", 5), ASX_OK);

    e = asx_metrics_get(&g_m, "requests");
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ(e->value, (int64_t)INT64_MAX);
}

TEST(counter_increment_clamps_negative_overflow) {
    const asx_metric_entry *e;
    setup();
    ASSERT_EQ(asx_metrics_counter(&g_m, "requests"), ASX_OK);
    ASSERT_EQ(asx_metrics_increment(&g_m, "requests", INT64_MIN + 1), ASX_OK);
    ASSERT_EQ(asx_metrics_increment(&g_m, "requests", -5), ASX_OK);

    e = asx_metrics_get(&g_m, "requests");
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ(e->value, (int64_t)INT64_MIN);
}

/* ================================================================== */
/* Gauge                                                               */
/* ================================================================== */

TEST(gauge_register_and_set) {
    const asx_metric_entry *e;
    setup();
    ASSERT_EQ(asx_metrics_gauge(&g_m, "connections"), ASX_OK);
    ASSERT_EQ(asx_metrics_set(&g_m, "connections", 42), ASX_OK);

    e = asx_metrics_get(&g_m, "connections");
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ((int)e->type, (int)ASX_METRIC_GAUGE);
    ASSERT_EQ(e->value, (int64_t)42);

    /* Set to new value */
    ASSERT_EQ(asx_metrics_set(&g_m, "connections", 10), ASX_OK);
    e = asx_metrics_get(&g_m, "connections");
    ASSERT_EQ(e->value, (int64_t)10);
}

/* ================================================================== */
/* Histogram                                                           */
/* ================================================================== */

TEST(histogram_register_and_observe) {
    const asx_metric_entry *e;
    setup();
    ASSERT_EQ(asx_metrics_histogram(&g_m, "latency"), ASX_OK);
    ASSERT_EQ(asx_metrics_observe(&g_m, "latency", 100), ASX_OK);
    ASSERT_EQ(asx_metrics_observe(&g_m, "latency", 200), ASX_OK);
    ASSERT_EQ(asx_metrics_observe(&g_m, "latency", 50), ASX_OK);

    e = asx_metrics_get(&g_m, "latency");
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ((int)e->type, (int)ASX_METRIC_HISTOGRAM);
    ASSERT_EQ(e->count, (uint64_t)3);
    ASSERT_EQ(e->min_val, (int64_t)50);
    ASSERT_EQ(e->max_val, (int64_t)200);
    ASSERT_EQ(e->sum, (int64_t)350);
}

TEST(histogram_sum_clamps_positive_overflow) {
    const asx_metric_entry *e;
    setup();
    ASSERT_EQ(asx_metrics_histogram(&g_m, "latency"), ASX_OK);
    ASSERT_EQ(asx_metrics_observe(&g_m, "latency", INT64_MAX - 1), ASX_OK);
    ASSERT_EQ(asx_metrics_observe(&g_m, "latency", 5), ASX_OK);

    e = asx_metrics_get(&g_m, "latency");
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ(e->sum, (int64_t)INT64_MAX);
}

/* ================================================================== */
/* Lookup                                                              */
/* ================================================================== */

TEST(get_returns_null_for_missing) {
    setup();
    ASSERT_TRUE(asx_metrics_get(&g_m, "nope") == NULL);
}

TEST(get_null_metrics) { ASSERT_TRUE(asx_metrics_get(NULL, "x") == NULL); }

/* ================================================================== */
/* Capacity                                                            */
/* ================================================================== */

TEST(capacity_exhaustion) {
    uint32_t i;
    char name[16];
    setup();
    for (i = 0; i < ASX_METRICS_MAX_ENTRIES; i++) {
        snprintf(name, sizeof(name), "m%u", (unsigned)i);
        ASSERT_EQ(asx_metrics_counter(&g_m, name), ASX_OK);
    }
    ASSERT_EQ(asx_metrics_counter(&g_m, "overflow"), ASX_E_RESOURCE_EXHAUSTED);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_observability ===\n");

    RUN_TEST(metrics_init_empty);
    RUN_TEST(counter_register_and_increment);
    RUN_TEST(counter_not_found);
    RUN_TEST(counter_increment_clamps_positive_overflow);
    RUN_TEST(counter_increment_clamps_negative_overflow);
    RUN_TEST(gauge_register_and_set);
    RUN_TEST(histogram_register_and_observe);
    RUN_TEST(histogram_sum_clamps_positive_overflow);
    RUN_TEST(get_returns_null_for_missing);
    RUN_TEST(get_null_metrics);
    RUN_TEST(capacity_exhaustion);

    TEST_REPORT();
    return test_failures;
}
