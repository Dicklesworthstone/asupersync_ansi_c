/*
 * test_security_audit.c — ambient-authority audit and negative-test
 *                          regression suite
 *
 * Proves the C port refuses forbidden authority patterns by exercising
 * every must-fail scenario as a first-class product behavior.
 *
 * Bead: bd-1eqo.12.2
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/diagnostic.h>
#include <asx/security/audit.h>
#include <asx/security/security.h>
#include <string.h>

/* ================================================================== */
/* Ambient authority catalog tests                                     */
/* ================================================================== */

TEST(catalog_has_entries) {
    size_t count = 0;
    const asx_ambient_finding *catalog = asx_ambient_known_findings(&count);
    ASSERT_TRUE(catalog != NULL);
    ASSERT_TRUE(count > 0);
}

TEST(catalog_entries_have_required_fields) {
    size_t count = 0;
    size_t i;
    const asx_ambient_finding *catalog = asx_ambient_known_findings(&count);

    for (i = 0; i < count; i++) {
        ASSERT_TRUE(catalog[i].file != NULL);
        ASSERT_TRUE(catalog[i].evidence != NULL);
        /* Exempted entries must have justification */
        if (catalog[i].exempted) { ASSERT_TRUE(catalog[i].justification != NULL); }
    }
}

TEST(catalog_all_exempted_are_providers) {
    /*
     * Every exempted finding must be in a provider module (time, entropy,
     * IO driver).  Non-provider modules must NEVER have exemptions.
     */
    size_t count = 0;
    size_t i;
    const asx_ambient_finding *catalog = asx_ambient_known_findings(&count);

    for (i = 0; i < count; i++) {
        if (catalog[i].exempted) {
            /* Must be in a known provider path */
            int is_provider = (strstr(catalog[i].file, "time") != NULL) ||
                              (strstr(catalog[i].file, "entropy") != NULL) ||
                              (strstr(catalog[i].file, "io") != NULL) ||
                              (strstr(catalog[i].file, "driver") != NULL);
            ASSERT_TRUE(is_provider);
        }
    }
}

TEST(violation_ceiling_is_zero) {
    /*
     * The ceiling of non-exempted ambient authority must be zero.
     * Any non-provider ambient use is a regression.
     */
    ASSERT_EQ(asx_ambient_violation_ceiling(), 0u);
}

/* ================================================================== */
/* Ambient audit runner tests                                          */
/* ================================================================== */

TEST(audit_runner_produces_evidence) {
    asx_evidence_sink sink;
    asx_audit_summary summary;

    asx_evidence_sink_init(&sink);
    summary = asx_ambient_audit(&sink);

    /* Should have at least one evidence entry per finding + verdict */
    ASSERT_TRUE(sink.count >= summary.total_findings + 1);
    /* Current catalog: all exempted, zero violations */
    ASSERT_EQ(summary.unexempted_count, 0u);
    ASSERT_TRUE(!summary.ceiling_exceeded);
}

TEST(audit_runner_verdict_is_pass) {
    asx_evidence_sink sink;

    asx_evidence_sink_init(&sink);
    (void)asx_ambient_audit(&sink);

    ASSERT_EQ(asx_evidence_verdict(&sink), ASX_EVIDENCE_PASS);
}

/* ================================================================== */
/* Pristine module list tests                                          */
/* ================================================================== */

TEST(pristine_modules_listed) {
    size_t count = 0;
    const char *const *modules = asx_audit_pristine_modules(&count);
    ASSERT_TRUE(modules != NULL);
    ASSERT_TRUE(count > 0);
}

TEST(pristine_modules_include_security) {
    size_t count = 0;
    size_t i;
    int found = 0;
    const char *const *modules = asx_audit_pristine_modules(&count);

    for (i = 0; i < count; i++) {
        if (strstr(modules[i], "security") != NULL) {
            found = 1;
            break;
        }
    }
    ASSERT_TRUE(found);
}

TEST(pristine_modules_include_core) {
    size_t count = 0;
    size_t i;
    int found_channel = 0;
    const char *const *modules = asx_audit_pristine_modules(&count);

    for (i = 0; i < count; i++) {
        if (strstr(modules[i], "channel") != NULL) { found_channel = 1; }
    }
    ASSERT_TRUE(found_channel);
}

/* ================================================================== */
/* Individual negative-test check tests                                */
/* ================================================================== */

TEST(neg_forged_tag_rejected) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_forged_tag_rejected(&sink, 42));
    ASSERT_EQ(asx_evidence_verdict(&sink), ASX_EVIDENCE_PASS);
}

TEST(neg_forged_tag_rejected_seed_zero) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_forged_tag_rejected(&sink, 0));
}

TEST(neg_forged_tag_rejected_seed_max) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_forged_tag_rejected(&sink, UINT64_MAX));
}

TEST(neg_cross_context_transport_storage) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_cross_context_denied(&sink, 100, "transport", "storage"));
}

TEST(neg_cross_context_sign_verify) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_cross_context_denied(&sink, 100, "sign", "verify"));
}

TEST(neg_cross_context_empty_vs_nonempty) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_cross_context_denied(&sink, 42, "a", "ab"));
}

TEST(neg_tamper_detected) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_tamper_detected(&sink, 42));
}

TEST(neg_tamper_detected_seed_zero) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_tamper_detected(&sink, 0));
}

TEST(neg_unverified_symbol) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_unverified_symbol_not_verified(&sink));
}

TEST(neg_disabled_not_verified) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_disabled_not_verified(&sink, 42));
}

TEST(neg_disabled_not_verified_seed_zero) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_disabled_not_verified(&sink, 0));
}

TEST(neg_permissive_no_false_positive) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_permissive_no_false_positive(&sink, 42));
}

TEST(neg_permissive_no_false_positive_different_seed) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_permissive_no_false_positive(&sink, 99));
}

TEST(neg_wrong_key_rejected) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_wrong_key_rejected(&sink, 1, 2));
}

TEST(neg_wrong_key_rejected_boundary) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_wrong_key_rejected(&sink, 0, UINT64_MAX));
}

TEST(neg_zero_tag_rejected) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_zero_tag_rejected(&sink, 42));
}

TEST(neg_zero_tag_rejected_seed_zero) {
    asx_evidence_sink sink;
    asx_evidence_sink_init(&sink);
    ASSERT_TRUE(asx_audit_check_zero_tag_rejected(&sink, 0));
}

/* ================================================================== */
/* Full negative-test suite runner                                     */
/* ================================================================== */

TEST(full_negative_suite_passes) {
    asx_evidence_sink sink;
    asx_evidence_level verdict;

    asx_evidence_sink_init(&sink);
    verdict = asx_audit_run_negative_suite(&sink);

    ASSERT_EQ(verdict, ASX_EVIDENCE_PASS);
    /* Should have recorded many entries */
    ASSERT_TRUE(sink.count >= 15);
    /* No failures */
    ASSERT_EQ(sink.fail_count, 0u);
}

TEST(full_negative_suite_all_pass_entries) {
    asx_evidence_sink sink;
    uint32_t i;

    asx_evidence_sink_init(&sink);
    (void)asx_audit_run_negative_suite(&sink);

    for (i = 0; i < sink.count; i++) { ASSERT_EQ(sink.entries[i].level, ASX_EVIDENCE_PASS); }
}

/* ================================================================== */
/* Regression: specific authority failure scenarios                     */
/* ================================================================== */

TEST(regression_no_ambient_key_leaks) {
    /*
     * Two independently seeded contexts must produce different tags
     * for the same data.  If they match, there's an ambient key leak.
     */
    asx_security_context a, b;
    asx_auth_tag tag_a, tag_b;
    uint8_t data[] = {1, 2, 3, 4, 5};

    asx_security_context_for_testing(&a, 42);
    asx_security_context_for_testing(&b, 99);

    asx_security_context_sign(&a, data, sizeof(data), &tag_a);
    asx_security_context_sign(&b, data, sizeof(data), &tag_b);

    ASSERT_TRUE(!asx_auth_tag_equals(&tag_a, &tag_b));
}

TEST(regression_stats_track_all_failures) {
    /*
     * After multiple failed verifications, stats must accurately
     * reflect every failure — not lose count.
     */
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    int verified;
    uint8_t data[] = {1};
    unsigned i;

    asx_security_context_for_testing(&ctx, 42);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_PERMISSIVE);
    asx_auth_tag_zero(&bad_tag);

    for (i = 0; i < 10; i++) {
        asx_status st_v;
        verified = 0;
        st_v = asx_security_context_verify(&ctx, data, sizeof(data), &bad_tag, &verified);
        (void)st_v;
        ASSERT_TRUE(!verified);
    }

    ASSERT_EQ(ctx.stats.verified_fail, 10u);
    ASSERT_EQ(ctx.stats.failures_allowed, 10u);
    ASSERT_EQ(ctx.stats.verified_ok, 0u);
}

TEST(regression_derived_key_not_reversible) {
    /*
     * A derived key must not equal the parent.  This prevents
     * privilege escalation by deriving back to the parent key.
     */
    asx_auth_key parent, child, grandchild;

    asx_auth_key_from_seed(&parent, 42);
    asx_auth_key_derive(&child, &parent, (const uint8_t *)"child", 5);
    asx_auth_key_derive(&grandchild, &child, (const uint8_t *)"grandchild", 10);

    ASSERT_TRUE(!asx_auth_key_equals(&parent, &child));
    ASSERT_TRUE(!asx_auth_key_equals(&parent, &grandchild));
    ASSERT_TRUE(!asx_auth_key_equals(&child, &grandchild));
}

TEST(regression_empty_data_distinguishable) {
    /*
     * Tags for empty data vs non-empty data must differ.
     * This prevents a trivial bypass where missing data matches.
     */
    asx_auth_key key;
    asx_auth_tag tag_empty, tag_nonempty;
    uint8_t data[] = {1};

    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&tag_empty, &key, NULL, 0);
    asx_auth_tag_compute(&tag_nonempty, &key, data, sizeof(data));

    ASSERT_TRUE(!asx_auth_tag_equals(&tag_empty, &tag_nonempty));
}

TEST(regression_mode_transition_strict_to_permissive) {
    /*
     * Switching from strict to permissive mid-stream must not
     * retroactively validate previously failed symbols.
     */
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    asx_authenticated_symbol sym;
    uint8_t data[] = {1, 2, 3};

    asx_security_context_for_testing(&ctx, 42);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_STRICT);

    asx_auth_tag_zero(&bad_tag);
    asx_authenticated_symbol_from_parts(&sym, data, sizeof(data), &bad_tag);

    /* Fails in strict mode */
    ASSERT_EQ(asx_authenticated_symbol_verify(&sym, &ctx), ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&sym));

    /* Switch to permissive and try again */
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_PERMISSIVE);
    ASSERT_EQ(asx_authenticated_symbol_verify(&sym, &ctx), ASX_OK);
    /* Must still NOT be marked verified — tag is invalid */
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&sym));
}

TEST(regression_distinct_data_distinct_tag) {
    /*
     * Tags for meaningfully different data must differ.
     * Verifies the hash distinguishes payloads that share no
     * structural relationship.
     */
    asx_auth_key key;
    asx_auth_tag tag_a, tag_b;
    uint8_t data_a[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t data_b[] = {0x04, 0x03, 0x02, 0x01};

    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&tag_a, &key, data_a, sizeof(data_a));
    asx_auth_tag_compute(&tag_b, &key, data_b, sizeof(data_b));

    ASSERT_TRUE(!asx_auth_tag_equals(&tag_a, &tag_b));
}

TEST(regression_sign_does_not_leak_key_to_tag) {
    /*
     * The authentication tag must not contain the raw key bytes.
     * Simple check: tag bytes must not match any 32-byte window
     * of the key.
     */
    asx_auth_key key;
    asx_auth_tag tag;
    uint8_t data[] = {0};

    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&tag, &key, data, sizeof(data));

    ASSERT_TRUE(memcmp(tag.bytes, key.bytes, ASX_AUTH_TAG_SIZE) != 0);
}

/* ================================================================== */
/* Evidence integration tests                                          */
/* ================================================================== */

TEST(evidence_entries_have_source_prefix) {
    /*
     * All entries from the audit runner must have "audit:" source prefix
     * for proper filtering in CI triage.
     */
    asx_evidence_sink sink;
    uint32_t i;

    asx_evidence_sink_init(&sink);
    (void)asx_audit_run_negative_suite(&sink);

    for (i = 0; i < sink.count; i++) {
        ASSERT_TRUE(sink.entries[i].source != NULL);
        ASSERT_TRUE(strncmp(sink.entries[i].source, "audit:", 6) == 0);
    }
}

TEST(evidence_entries_have_monotonic_sequence) {
    asx_evidence_sink sink;
    uint32_t i;

    asx_evidence_sink_init(&sink);
    (void)asx_audit_run_negative_suite(&sink);

    for (i = 1; i < sink.count; i++) {
        ASSERT_TRUE(sink.entries[i].sequence > sink.entries[i - 1].sequence);
    }
}

TEST(evidence_combined_audit_and_negative_suite) {
    /*
     * Running both ambient audit and negative suite into the same
     * evidence sink should produce a unified PASS verdict.
     */
    asx_evidence_sink sink;

    asx_evidence_sink_init(&sink);
    (void)asx_ambient_audit(&sink);
    (void)asx_audit_run_negative_suite(&sink);

    ASSERT_EQ(asx_evidence_verdict(&sink), ASX_EVIDENCE_PASS);
    ASSERT_EQ(sink.fail_count, 0u);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    /* Ambient authority catalog */
    RUN_TEST(catalog_has_entries);
    RUN_TEST(catalog_entries_have_required_fields);
    RUN_TEST(catalog_all_exempted_are_providers);
    RUN_TEST(violation_ceiling_is_zero);

    /* Audit runner */
    RUN_TEST(audit_runner_produces_evidence);
    RUN_TEST(audit_runner_verdict_is_pass);

    /* Pristine modules */
    RUN_TEST(pristine_modules_listed);
    RUN_TEST(pristine_modules_include_security);
    RUN_TEST(pristine_modules_include_core);

    /* Individual negative-test checks */
    RUN_TEST(neg_forged_tag_rejected);
    RUN_TEST(neg_forged_tag_rejected_seed_zero);
    RUN_TEST(neg_forged_tag_rejected_seed_max);
    RUN_TEST(neg_cross_context_transport_storage);
    RUN_TEST(neg_cross_context_sign_verify);
    RUN_TEST(neg_cross_context_empty_vs_nonempty);
    RUN_TEST(neg_tamper_detected);
    RUN_TEST(neg_tamper_detected_seed_zero);
    RUN_TEST(neg_unverified_symbol);
    RUN_TEST(neg_disabled_not_verified);
    RUN_TEST(neg_disabled_not_verified_seed_zero);
    RUN_TEST(neg_permissive_no_false_positive);
    RUN_TEST(neg_permissive_no_false_positive_different_seed);
    RUN_TEST(neg_wrong_key_rejected);
    RUN_TEST(neg_wrong_key_rejected_boundary);
    RUN_TEST(neg_zero_tag_rejected);
    RUN_TEST(neg_zero_tag_rejected_seed_zero);

    /* Full suite runner */
    RUN_TEST(full_negative_suite_passes);
    RUN_TEST(full_negative_suite_all_pass_entries);

    /* Regression tests */
    RUN_TEST(regression_no_ambient_key_leaks);
    RUN_TEST(regression_stats_track_all_failures);
    RUN_TEST(regression_derived_key_not_reversible);
    RUN_TEST(regression_empty_data_distinguishable);
    RUN_TEST(regression_mode_transition_strict_to_permissive);
    RUN_TEST(regression_distinct_data_distinct_tag);
    RUN_TEST(regression_sign_does_not_leak_key_to_tag);

    /* Evidence integration */
    RUN_TEST(evidence_entries_have_source_prefix);
    RUN_TEST(evidence_entries_have_monotonic_sequence);
    RUN_TEST(evidence_combined_audit_and_negative_suite);

    TEST_REPORT();
    return test_failures;
}
