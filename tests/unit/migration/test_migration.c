/*
 * test_migration.c — unit tests for version migration and compatibility
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/migration/migration.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Version info                                                        */
/* ------------------------------------------------------------------ */

TEST(version_current) {
    asx_version v = asx_version_current();
    /* Should be 0.1.0 */
    ASSERT_EQ(v.major, 0u);
    ASSERT_EQ(v.minor, 1u);
    ASSERT_EQ(v.patch, 0u);
}

TEST(version_str) {
    const char *s = asx_version_str();
    ASSERT_NE(s, NULL);
    ASSERT_STR_EQ(s, "0.1.0");
}

TEST(wire_format_version) {
    ASSERT_EQ(asx_wire_format_version(), 1u);
}

/* ------------------------------------------------------------------ */
/* Compatibility checking                                              */
/* ------------------------------------------------------------------ */

TEST(compat_level_str) {
    ASSERT_STR_EQ(asx_compat_level_str(ASX_COMPAT_FULL), "full");
    ASSERT_STR_EQ(asx_compat_level_str(ASX_COMPAT_FORWARD), "forward");
    ASSERT_STR_EQ(asx_compat_level_str(ASX_COMPAT_BACKWARD), "backward");
    ASSERT_STR_EQ(asx_compat_level_str(ASX_COMPAT_INCOMPATIBLE), "incompatible");
    ASSERT_STR_EQ(asx_compat_level_str((asx_compat_level)99), "unknown");
}

TEST(check_wire_compat_current) {
    ASSERT_EQ(asx_check_wire_compat(1), ASX_COMPAT_FULL);
}

TEST(check_wire_compat_older) {
    ASSERT_EQ(asx_check_wire_compat(0), ASX_COMPAT_FORWARD);
}

TEST(check_wire_compat_future) {
    ASSERT_EQ(asx_check_wire_compat(999), ASX_COMPAT_INCOMPATIBLE);
}

/* ------------------------------------------------------------------ */
/* Migration reports                                                   */
/* ------------------------------------------------------------------ */

TEST(migration_check_current) {
    asx_migration_report report;
    ASSERT_EQ(asx_migration_check(1, &report), ASX_OK);
    ASSERT_EQ(report.level, ASX_COMPAT_FULL);
    ASSERT_EQ(report.migration_action, NULL);
    ASSERT_EQ(report.source_version, 1u);
    ASSERT_EQ(report.target_version, 1u);
}

TEST(migration_check_older) {
    asx_migration_report report;
    ASSERT_EQ(asx_migration_check(0, &report), ASX_OK);
    ASSERT_EQ(report.level, ASX_COMPAT_FORWARD);
    ASSERT_NE(report.migration_action, NULL);
}

TEST(migration_check_incompatible) {
    asx_migration_report report;
    ASSERT_EQ(asx_migration_check(999, &report), ASX_OK);
    ASSERT_EQ(report.level, ASX_COMPAT_INCOMPATIBLE);
    ASSERT_NE(report.migration_action, NULL);
}

TEST(migration_check_null_report) {
    ASSERT_EQ(asx_migration_check(1, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Feature availability                                                */
/* ------------------------------------------------------------------ */

TEST(feature_available_implemented) {
    ASSERT_NE(asx_feature_available("actor"), 0);
    ASSERT_NE(asx_feature_available("encoding"), 0);
    ASSERT_NE(asx_feature_available("decoding"), 0);
    ASSERT_NE(asx_feature_available("trace"), 0);
    ASSERT_NE(asx_feature_available("replay"), 0);
    ASSERT_NE(asx_feature_available("evidence"), 0);
    ASSERT_NE(asx_feature_available("monitor"), 0);
    ASSERT_NE(asx_feature_available("distributed"), 0);
    ASSERT_NE(asx_feature_available("grpc"), 0);
    ASSERT_NE(asx_feature_available("http"), 0);
    ASSERT_NE(asx_feature_available("tls"), 0);
    ASSERT_NE(asx_feature_available("websocket"), 0);
    ASSERT_NE(asx_feature_available("quic"), 0);
    ASSERT_NE(asx_feature_available("web"), 0);
    ASSERT_NE(asx_feature_available("db"), 0);
    ASSERT_NE(asx_feature_available("messaging"), 0);
    ASSERT_NE(asx_feature_available("server"), 0);
}

TEST(feature_available_deferred) {
    /* All features are now implemented — nothing is deferred */
    ASSERT_NE(asx_feature_available("raptorq"), 0);
}

TEST(feature_available_unknown) {
    ASSERT_EQ(asx_feature_available("nonexistent"), 0);
    ASSERT_EQ(asx_feature_available(NULL), 0);
}

TEST(feature_deferral_reason) {
    /* All features now implemented — deferral reasons are NULL */
    ASSERT_EQ(asx_feature_deferral_reason("raptorq"), NULL);
    ASSERT_EQ(asx_feature_deferral_reason("actor"), NULL);

    /* Unknown features return NULL */
    ASSERT_EQ(asx_feature_deferral_reason("nonexistent"), NULL);
    ASSERT_EQ(asx_feature_deferral_reason(NULL), NULL);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_migration ===\n");

    RUN_TEST(version_current);
    RUN_TEST(version_str);
    RUN_TEST(wire_format_version);
    RUN_TEST(compat_level_str);
    RUN_TEST(check_wire_compat_current);
    RUN_TEST(check_wire_compat_older);
    RUN_TEST(check_wire_compat_future);
    RUN_TEST(migration_check_current);
    RUN_TEST(migration_check_older);
    RUN_TEST(migration_check_incompatible);
    RUN_TEST(migration_check_null_report);
    RUN_TEST(feature_available_implemented);
    RUN_TEST(feature_available_deferred);
    RUN_TEST(feature_available_unknown);
    RUN_TEST(feature_deferral_reason);

    TEST_REPORT();
    return test_failures;
}
