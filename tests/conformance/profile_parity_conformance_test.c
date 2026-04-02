/*
 * profile_parity_conformance_test.c — cross-profile semantic parity
 *
 * Conformance gate: verifies that fixtures differing only in the profile
 * field produce identical codec round-trips for all non-profile semantic
 * fields, and that the semantic equivalence API correctly identifies
 * profile as the sole difference.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_harness.h"
#include <asx/asx.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */

static char *dup_str(const char *s) {
    size_t len;
    char *copy;

    if (s == NULL) return NULL;
    len = strlen(s);
    copy = (char *)malloc(len + 1u);
    if (copy == NULL) return NULL;
    memcpy(copy, s, len + 1u);
    return copy;
}

static void populate_provenance(asx_fixture_provenance *p) {
    p->rust_baseline_commit = dup_str("0123456789abcdef0123456789abcdef01234567");
    p->rust_toolchain_commit_hash = dup_str("toolchain-abcdef12");
    p->rust_toolchain_release = dup_str("rustc 1.90.0");
    p->rust_toolchain_host = dup_str("x86_64-unknown-linux-gnu");
    p->cargo_lock_sha256 =
        dup_str("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    p->capture_run_id = dup_str("capture-run-0001");
}

static void populate_fixture(asx_canonical_fixture *f, const char *profile, uint64_t seed) {
    f->scenario_id = dup_str("conformance.profile-parity.001");
    f->fixture_schema_version = dup_str("fixture-v1");
    f->scenario_dsl_version = dup_str("dsl-v1");
    f->profile = dup_str(profile);
    f->codec = ASX_CODEC_KIND_JSON;
    f->seed = seed;
    f->input_json = dup_str("{\"ops\":[{\"kind\":\"spawn\",\"region\":0}]}");
    f->expected_events_json = dup_str("[{\"type\":\"task_spawned\",\"id\":1}]");
    f->expected_final_snapshot_json = dup_str("{\"regions\":[{\"id\":0,\"state\":\"closed\"}]}");
    f->expected_error_codes_json = dup_str("[]");
    f->semantic_digest = dup_str("sha256:0123456789abcdef0123456789abcdef"
                                 "0123456789abcdef0123456789abcdef");
    populate_provenance(&f->provenance);
}

static const char *ALL_PROFILES[] = {
    "ASX_PROFILE_CORE",         "ASX_PROFILE_POSIX",           "ASX_PROFILE_WIN32",
    "ASX_PROFILE_FREESTANDING", "ASX_PROFILE_EMBEDDED_ROUTER", "ASX_PROFILE_HFT",
    "ASX_PROFILE_AUTOMOTIVE",   "ASX_PROFILE_PARALLEL",        "ASX_PROFILE_BROWSER"};

#define PROFILE_COUNT (sizeof(ALL_PROFILES) / sizeof(ALL_PROFILES[0]))

/* ---- Test: JSON round-trip preserves non-profile fields across profiles ---- */

TEST(parity_json_roundtrip_preserves_non_profile_fields) {
    size_t i;
    asx_canonical_fixture ref;
    asx_codec_buffer ref_key;

    /* Establish reference fixture with CORE profile */
    asx_canonical_fixture_init(&ref);
    asx_codec_buffer_init(&ref_key);
    populate_fixture(&ref, "ASX_PROFILE_CORE", 42u);
    ASSERT_EQ(asx_codec_fixture_replay_key(&ref, &ref_key), ASX_OK);

    for (i = 1u; i < PROFILE_COUNT; i++) {
        asx_canonical_fixture f, decoded;
        asx_codec_buffer encoded, decoded_key;

        asx_canonical_fixture_init(&f);
        asx_canonical_fixture_init(&decoded);
        asx_codec_buffer_init(&encoded);
        asx_codec_buffer_init(&decoded_key);

        populate_fixture(&f, ALL_PROFILES[i], 42u);

        /* Encode and decode through JSON */
        ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &f, &encoded), ASX_OK);
        ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, encoded.data, encoded.len,
                                           &decoded),
                  ASX_OK);

        /* Non-profile fields must match */
        ASSERT_STR_EQ(decoded.scenario_id, ref.scenario_id);
        ASSERT_EQ(decoded.seed, ref.seed);
        ASSERT_STR_EQ(decoded.semantic_digest, ref.semantic_digest);
        ASSERT_STR_EQ(decoded.input_json, ref.input_json);
        ASSERT_STR_EQ(decoded.expected_events_json, ref.expected_events_json);
        ASSERT_STR_EQ(decoded.expected_final_snapshot_json, ref.expected_final_snapshot_json);

        /* Profile field must differ (it's the only variable) */
        ASSERT_STR_EQ(decoded.profile, ALL_PROFILES[i]);

        asx_codec_buffer_reset(&encoded);
        asx_codec_buffer_reset(&decoded_key);
        asx_canonical_fixture_reset(&f);
        asx_canonical_fixture_reset(&decoded);
    }

    asx_codec_buffer_reset(&ref_key);
    asx_canonical_fixture_reset(&ref);
}

/* ---- Test: BIN round-trip preserves non-profile fields across profiles ---- */

TEST(parity_bin_roundtrip_preserves_non_profile_fields) {
    size_t i;

    for (i = 0u; i < PROFILE_COUNT; i++) {
        asx_canonical_fixture f, decoded;
        asx_codec_buffer encoded;

        asx_canonical_fixture_init(&f);
        asx_canonical_fixture_init(&decoded);
        asx_codec_buffer_init(&encoded);

        populate_fixture(&f, ALL_PROFILES[i], 42u);
        f.codec = ASX_CODEC_KIND_BIN;

        ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &f, &encoded), ASX_OK);
        ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, encoded.data, encoded.len, &decoded),
                  ASX_OK);

        ASSERT_STR_EQ(decoded.scenario_id, f.scenario_id);
        ASSERT_EQ(decoded.seed, f.seed);
        ASSERT_STR_EQ(decoded.profile, ALL_PROFILES[i]);

        asx_codec_buffer_reset(&encoded);
        asx_canonical_fixture_reset(&f);
        asx_canonical_fixture_reset(&decoded);
    }
}

/* ---- Test: semantic equivalence detects profile-only difference ---- */

TEST(parity_semantic_eq_detects_profile_only_diff) {
    asx_canonical_fixture core, hft;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&core);
    asx_canonical_fixture_init(&hft);
    asx_codec_equiv_report_init(&report);

    populate_fixture(&core, "ASX_PROFILE_CORE", 42u);
    populate_fixture(&hft, "ASX_PROFILE_HFT", 42u);

    /* Semantic eq ignores codec but not profile — profile diff is a mismatch */
    ASSERT_EQ(asx_codec_fixture_semantic_eq(&core, &hft, &report), ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(report.count > 0u);

    asx_canonical_fixture_reset(&core);
    asx_canonical_fixture_reset(&hft);
}

/* ---- Test: identical profiles produce semantic equivalence ---- */

TEST(parity_same_profile_is_equivalent) {
    size_t i;

    for (i = 0u; i < PROFILE_COUNT; i++) {
        asx_canonical_fixture a, b;
        asx_codec_equiv_report report;

        asx_canonical_fixture_init(&a);
        asx_canonical_fixture_init(&b);
        asx_codec_equiv_report_init(&report);

        populate_fixture(&a, ALL_PROFILES[i], 42u);
        populate_fixture(&b, ALL_PROFILES[i], 42u);

        ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report), ASX_OK);

        asx_canonical_fixture_reset(&a);
        asx_canonical_fixture_reset(&b);
    }
}

/* ---- Test: cross-codec round-trip works for every profile ---- */

TEST(parity_cross_codec_per_profile) {
    size_t i;

    for (i = 0u; i < PROFILE_COUNT; i++) {
        asx_canonical_fixture f;
        asx_codec_equiv_report report;

        asx_canonical_fixture_init(&f);
        asx_codec_equiv_report_init(&report);

        populate_fixture(&f, ALL_PROFILES[i], 42u);
        ASSERT_EQ(asx_codec_cross_codec_verify(&f, &report), ASX_OK);

        asx_canonical_fixture_reset(&f);
    }
}

/* ---- Test: semantic key differs by profile (profile is part of key) ---- */

TEST(parity_semantic_key_includes_profile) {
    asx_canonical_fixture core, hft;
    asx_codec_buffer core_key, hft_key;

    asx_canonical_fixture_init(&core);
    asx_canonical_fixture_init(&hft);
    asx_codec_buffer_init(&core_key);
    asx_codec_buffer_init(&hft_key);

    populate_fixture(&core, "ASX_PROFILE_CORE", 42u);
    populate_fixture(&hft, "ASX_PROFILE_HFT", 42u);

    ASSERT_EQ(asx_codec_fixture_semantic_key(&core, &core_key), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_semantic_key(&hft, &hft_key), ASX_OK);

    /* Keys should differ because profile is a semantic field */
    ASSERT_TRUE(core_key.len != hft_key.len ||
                memcmp(core_key.data, hft_key.data, core_key.len) != 0);

    asx_codec_buffer_reset(&core_key);
    asx_codec_buffer_reset(&hft_key);
    asx_canonical_fixture_reset(&core);
    asx_canonical_fixture_reset(&hft);
}

/* ---- Runner ---- */

int main(void) {
    fprintf(stderr, "=== profile_parity_conformance_test ===\n");

    RUN_TEST(parity_json_roundtrip_preserves_non_profile_fields);
    RUN_TEST(parity_bin_roundtrip_preserves_non_profile_fields);
    RUN_TEST(parity_semantic_eq_detects_profile_only_diff);
    RUN_TEST(parity_same_profile_is_equivalent);
    RUN_TEST(parity_cross_codec_per_profile);
    RUN_TEST(parity_semantic_key_includes_profile);

    TEST_REPORT();
    return test_failures;
}
