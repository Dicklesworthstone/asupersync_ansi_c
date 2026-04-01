/*
 * codec_equivalence_conformance_test.c — cross-codec semantic equivalence
 *
 * Conformance gate: verifies that encoding the same canonical fixture through
 * JSON and BIN codecs produces semantically identical round-trip results.
 * Tests multiple fixture scenarios with varying field complexity, edge-case
 * strings, and provenance metadata to ensure codec neutrality.
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

static void populate_minimal(asx_canonical_fixture *f, const char *scenario_id, uint64_t seed) {
    f->scenario_id = dup_str(scenario_id);
    f->fixture_schema_version = dup_str("fixture-v1");
    f->scenario_dsl_version = dup_str("dsl-v1");
    f->profile = dup_str("ASX_PROFILE_CORE");
    f->codec = ASX_CODEC_KIND_JSON;
    f->seed = seed;
    f->input_json = dup_str("{\"ops\":[]}");
    f->expected_events_json = dup_str("[]");
    f->expected_final_snapshot_json = dup_str("{}");
    f->expected_error_codes_json = dup_str("[]");
    f->semantic_digest = dup_str("sha256:0123456789abcdef0123456789abcdef"
                                 "0123456789abcdef0123456789abcdef");
    populate_provenance(&f->provenance);
}

/* Verify a fixture survives JSON->BIN->JSON with semantic identity preserved. */
static void assert_cross_codec_identity(const asx_canonical_fixture *original, const char *label) {
    asx_codec_equiv_report report;
    asx_codec_buffer json_buf, bin_buf, json_key, bin_key;
    asx_canonical_fixture decoded_from_json, decoded_from_bin;

    asx_codec_equiv_report_init(&report);
    asx_codec_buffer_init(&json_buf);
    asx_codec_buffer_init(&bin_buf);
    asx_codec_buffer_init(&json_key);
    asx_codec_buffer_init(&bin_key);
    asx_canonical_fixture_init(&decoded_from_json);
    asx_canonical_fixture_init(&decoded_from_bin);

    /* Encode JSON, decode back */
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, original, &json_buf), ASX_OK);
    ASSERT_TRUE(json_buf.len > 0u);
    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, json_buf.data, json_buf.len,
                                       &decoded_from_json),
              ASX_OK);

    /* Encode BIN from original, decode back */
    {
        asx_canonical_fixture bin_original;
        asx_canonical_fixture_init(&bin_original);
        /* Clone original but switch to BIN codec */
        bin_original.scenario_id = dup_str(original->scenario_id);
        bin_original.fixture_schema_version = dup_str(original->fixture_schema_version);
        bin_original.scenario_dsl_version = dup_str(original->scenario_dsl_version);
        bin_original.profile = dup_str(original->profile);
        bin_original.codec = ASX_CODEC_KIND_BIN;
        bin_original.seed = original->seed;
        bin_original.input_json = dup_str(original->input_json);
        bin_original.expected_events_json = dup_str(original->expected_events_json);
        bin_original.expected_final_snapshot_json = dup_str(original->expected_final_snapshot_json);
        bin_original.expected_error_codes_json = dup_str(original->expected_error_codes_json);
        bin_original.semantic_digest = dup_str(original->semantic_digest);
        populate_provenance(&bin_original.provenance);

        ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &bin_original, &bin_buf), ASX_OK);
        ASSERT_TRUE(bin_buf.len > 0u);
        ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, bin_buf.data, bin_buf.len,
                                           &decoded_from_bin),
                  ASX_OK);
        asx_canonical_fixture_reset(&bin_original);
    }

    /* Semantic equivalence: JSON-decoded vs BIN-decoded (ignoring codec field) */
    ASSERT_EQ(asx_codec_fixture_semantic_eq(&decoded_from_json, &decoded_from_bin, &report),
              ASX_OK);

    /* Semantic keys must match */
    ASSERT_EQ(asx_codec_fixture_semantic_key(&decoded_from_json, &json_key), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_semantic_key(&decoded_from_bin, &bin_key), ASX_OK);
    ASSERT_EQ(json_key.len, bin_key.len);
    ASSERT_TRUE(memcmp(json_key.data, bin_key.data, json_key.len) == 0);

    /* Field-level spot checks */
    ASSERT_STR_EQ(decoded_from_json.scenario_id, decoded_from_bin.scenario_id);
    ASSERT_EQ(decoded_from_json.seed, decoded_from_bin.seed);
    ASSERT_STR_EQ(decoded_from_json.semantic_digest, decoded_from_bin.semantic_digest);

    /* Cleanup */
    asx_codec_buffer_reset(&json_key);
    asx_codec_buffer_reset(&bin_key);
    asx_codec_buffer_reset(&json_buf);
    asx_codec_buffer_reset(&bin_buf);
    asx_canonical_fixture_reset(&decoded_from_json);
    asx_canonical_fixture_reset(&decoded_from_bin);

    (void)label;
}

/* Also verify the one-call cross-codec API */
static void assert_cross_codec_verify(const asx_canonical_fixture *fixture, const char *label) {
    asx_codec_equiv_report report;
    asx_codec_equiv_report_init(&report);
    ASSERT_EQ(asx_codec_cross_codec_verify(fixture, &report), ASX_OK);
    (void)label;
}

/* ---- Test: minimal fixture ---- */

TEST(equiv_minimal_fixture) {
    asx_canonical_fixture f;
    asx_canonical_fixture_init(&f);
    populate_minimal(&f, "conformance.equiv.minimal.001", 42u);

    assert_cross_codec_identity(&f, "minimal");
    assert_cross_codec_verify(&f, "minimal");

    asx_canonical_fixture_reset(&f);
}

/* ---- Test: high seed value (uint64 boundary) ---- */

TEST(equiv_high_seed) {
    asx_canonical_fixture f;
    asx_canonical_fixture_init(&f);
    populate_minimal(&f, "conformance.equiv.high-seed.001", 0xFFFFFFFFFFFFFFFFull);

    assert_cross_codec_identity(&f, "high-seed");
    assert_cross_codec_verify(&f, "high-seed");

    asx_canonical_fixture_reset(&f);
}

/* ---- Test: zero seed ---- */

TEST(equiv_zero_seed) {
    asx_canonical_fixture f;
    asx_canonical_fixture_init(&f);
    populate_minimal(&f, "conformance.equiv.zero-seed.001", 0u);

    assert_cross_codec_identity(&f, "zero-seed");
    assert_cross_codec_verify(&f, "zero-seed");

    asx_canonical_fixture_reset(&f);
}

/* ---- Test: complex JSON fields ---- */

TEST(equiv_complex_json_fields) {
    asx_canonical_fixture f;
    asx_canonical_fixture_init(&f);
    populate_minimal(&f, "conformance.equiv.complex-json.001", 7u);

    /* Override with more complex JSON payloads */
    free(f.input_json);
    f.input_json = dup_str("{\"ops\":[{\"kind\":\"spawn\",\"region\":0,\"task\":1},"
                           "{\"kind\":\"cancel\",\"task\":1,\"severity\":3}]}");

    free(f.expected_events_json);
    f.expected_events_json = dup_str("[{\"type\":\"task_spawned\",\"id\":1},"
                                     "{\"type\":\"cancel_requested\",\"id\":1,\"severity\":3},"
                                     "{\"type\":\"task_completed\",\"id\":1}]");

    free(f.expected_final_snapshot_json);
    f.expected_final_snapshot_json = dup_str("{\"regions\":[{\"id\":0,\"state\":\"closed\"}],"
                                             "\"tasks\":[{\"id\":1,\"state\":\"completed\"}]}");

    free(f.expected_error_codes_json);
    f.expected_error_codes_json = dup_str("[\"ASX_E_CANCELLED\"]");

    assert_cross_codec_identity(&f, "complex-json");
    assert_cross_codec_verify(&f, "complex-json");

    asx_canonical_fixture_reset(&f);
}

/* ---- Test: each deployment profile ---- */

TEST(equiv_across_profiles) {
    static const char *profiles[] = {
        "ASX_PROFILE_CORE",         "ASX_PROFILE_POSIX",           "ASX_PROFILE_WIN32",
        "ASX_PROFILE_FREESTANDING", "ASX_PROFILE_EMBEDDED_ROUTER", "ASX_PROFILE_HFT",
        "ASX_PROFILE_AUTOMOTIVE",   "ASX_PROFILE_PARALLEL",        "ASX_PROFILE_BROWSER"};
    size_t i;

    for (i = 0u; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        asx_canonical_fixture f;
        char scenario_id[128];

        asx_canonical_fixture_init(&f);
        snprintf(scenario_id, sizeof(scenario_id), "conformance.equiv.profile-%s.001", profiles[i]);
        populate_minimal(&f, scenario_id, (uint64_t)(i + 1u));

        free(f.profile);
        f.profile = dup_str(profiles[i]);

        assert_cross_codec_identity(&f, profiles[i]);
        assert_cross_codec_verify(&f, profiles[i]);

        asx_canonical_fixture_reset(&f);
    }
}

/* ---- Test: replay key determinism ---- */

TEST(equiv_replay_key_determinism) {
    asx_canonical_fixture f1, f2;
    asx_codec_buffer key1, key2;

    asx_canonical_fixture_init(&f1);
    asx_canonical_fixture_init(&f2);
    asx_codec_buffer_init(&key1);
    asx_codec_buffer_init(&key2);

    /* Two identical fixtures must produce identical replay keys */
    populate_minimal(&f1, "conformance.equiv.replay-key.001", 99u);
    populate_minimal(&f2, "conformance.equiv.replay-key.001", 99u);

    ASSERT_EQ(asx_codec_fixture_replay_key(&f1, &key1), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_replay_key(&f2, &key2), ASX_OK);
    ASSERT_EQ(key1.len, key2.len);
    ASSERT_TRUE(memcmp(key1.data, key2.data, key1.len) == 0);

    /* Different seed must produce different key */
    asx_codec_buffer_reset(&key2);
    asx_codec_buffer_init(&key2);
    f2.seed = 100u;
    ASSERT_EQ(asx_codec_fixture_replay_key(&f2, &key2), ASX_OK);
    ASSERT_TRUE(key1.len != key2.len || memcmp(key1.data, key2.data, key1.len) != 0);

    asx_codec_buffer_reset(&key1);
    asx_codec_buffer_reset(&key2);
    asx_canonical_fixture_reset(&f1);
    asx_canonical_fixture_reset(&f2);
}

/* ---- Test: semantic key is codec-agnostic ---- */

TEST(equiv_semantic_key_codec_agnostic) {
    asx_canonical_fixture json_fix, bin_fix;
    asx_codec_buffer json_key, bin_key;

    asx_canonical_fixture_init(&json_fix);
    asx_canonical_fixture_init(&bin_fix);
    asx_codec_buffer_init(&json_key);
    asx_codec_buffer_init(&bin_key);

    populate_minimal(&json_fix, "conformance.equiv.semantic-key.001", 42u);
    populate_minimal(&bin_fix, "conformance.equiv.semantic-key.001", 42u);
    json_fix.codec = ASX_CODEC_KIND_JSON;
    bin_fix.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_fixture_semantic_key(&json_fix, &json_key), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_semantic_key(&bin_fix, &bin_key), ASX_OK);
    ASSERT_EQ(json_key.len, bin_key.len);
    ASSERT_TRUE(memcmp(json_key.data, bin_key.data, json_key.len) == 0);

    asx_codec_buffer_reset(&json_key);
    asx_codec_buffer_reset(&bin_key);
    asx_canonical_fixture_reset(&json_fix);
    asx_canonical_fixture_reset(&bin_fix);
}

/* ---- Test: mismatch detection works ---- */

TEST(equiv_detects_semantic_mismatch) {
    asx_canonical_fixture a, b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    asx_codec_equiv_report_init(&report);

    populate_minimal(&a, "conformance.equiv.mismatch.001", 42u);
    populate_minimal(&b, "conformance.equiv.mismatch.001", 42u);

    /* Introduce a seed mismatch */
    b.seed = 999u;

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report), ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_TRUE(report.count > 0u);

    asx_canonical_fixture_reset(&a);
    asx_canonical_fixture_reset(&b);
}

/* ---- Runner ---- */

int main(void) {
    fprintf(stderr, "=== codec_equivalence_conformance_test ===\n");

    RUN_TEST(equiv_minimal_fixture);
    RUN_TEST(equiv_high_seed);
    RUN_TEST(equiv_zero_seed);
    RUN_TEST(equiv_complex_json_fields);
    RUN_TEST(equiv_across_profiles);
    RUN_TEST(equiv_replay_key_determinism);
    RUN_TEST(equiv_semantic_key_codec_agnostic);
    RUN_TEST(equiv_detects_semantic_mismatch);

    TEST_REPORT();
    return test_failures;
}
