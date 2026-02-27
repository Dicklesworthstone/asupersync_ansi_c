/*
 * test_codec_equivalence.c — cross-codec semantic equivalence tests (bd-2n0.3)
 *
 * Verifies that encoding through JSON and BIN codecs preserves semantic
 * identity: same fields, same digests, same replay keys. The codec field
 * itself is the only allowed difference.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <stdlib.h>
#include <string.h>

/* ---- Fixture population helpers ---- */

static char *dup_text(const char *text)
{
    size_t len;
    char *copy;

    len = strlen(text);
    copy = (char *)malloc(len + 1u);
    if (copy == NULL) return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

static void populate_fixture(asx_canonical_fixture *f, const char *scenario_id)
{
    f->scenario_id = dup_text(scenario_id);
    f->fixture_schema_version = dup_text("fixture-v1");
    f->scenario_dsl_version = dup_text("dsl-v1");
    f->profile = dup_text("ASX_PROFILE_CORE");
    f->codec = ASX_CODEC_KIND_JSON;
    f->seed = 42u;
    f->input_json = dup_text("{\"ops\":[]}");
    f->expected_events_json = dup_text("[]");
    f->expected_final_snapshot_json = dup_text("{}");
    f->expected_error_codes_json = dup_text("[]");
    f->semantic_digest = dup_text(
        "sha256:0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef");
    f->provenance.rust_baseline_commit = dup_text(
        "0123456789abcdef0123456789abcdef01234567");
    f->provenance.rust_toolchain_commit_hash = dup_text("toolchain-abcdef12");
    f->provenance.rust_toolchain_release = dup_text("rustc 1.90.0");
    f->provenance.rust_toolchain_host = dup_text("x86_64-unknown-linux-gnu");
    f->provenance.cargo_lock_sha256 = dup_text(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    f->provenance.capture_run_id = dup_text("capture-run-0001");
}

/* ---- Cross-codec round-trip ---- */

TEST(equiv_cross_codec_roundtrip_preserves_identity) {
    asx_canonical_fixture fixture;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&fixture);
    populate_fixture(&fixture, "scenario.equiv.roundtrip.001");

    ASSERT_EQ(asx_codec_cross_codec_verify(&fixture, &report), ASX_OK);
    ASSERT_EQ(report.count, (uint32_t)0);

    asx_canonical_fixture_reset(&fixture);
}

TEST(equiv_cross_codec_roundtrip_with_bin_source) {
    asx_canonical_fixture fixture;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&fixture);
    populate_fixture(&fixture, "scenario.equiv.roundtrip.002");
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_cross_codec_verify(&fixture, &report), ASX_OK);
    ASSERT_EQ(report.count, (uint32_t)0);

    asx_canonical_fixture_reset(&fixture);
}

/* ---- Semantic key codec-agnostic ---- */

TEST(equiv_semantic_key_ignores_codec_kind) {
    asx_canonical_fixture json_fixture;
    asx_canonical_fixture bin_fixture;
    asx_codec_buffer key_json;
    asx_codec_buffer key_bin;

    asx_canonical_fixture_init(&json_fixture);
    asx_canonical_fixture_init(&bin_fixture);
    asx_codec_buffer_init(&key_json);
    asx_codec_buffer_init(&key_bin);

    populate_fixture(&json_fixture, "scenario.equiv.key.001");
    populate_fixture(&bin_fixture, "scenario.equiv.key.001");
    json_fixture.codec = ASX_CODEC_KIND_JSON;
    bin_fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_fixture_semantic_key(&json_fixture, &key_json), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_semantic_key(&bin_fixture, &key_bin), ASX_OK);

    /* Semantic keys must be identical regardless of codec */
    ASSERT_STR_EQ(key_json.data, key_bin.data);

    asx_codec_buffer_reset(&key_bin);
    asx_codec_buffer_reset(&key_json);
    asx_canonical_fixture_reset(&bin_fixture);
    asx_canonical_fixture_reset(&json_fixture);
}

TEST(equiv_semantic_key_differs_from_replay_key) {
    asx_canonical_fixture fixture;
    asx_codec_buffer semantic_key;
    asx_codec_buffer replay_key;

    asx_canonical_fixture_init(&fixture);
    asx_codec_buffer_init(&semantic_key);
    asx_codec_buffer_init(&replay_key);
    populate_fixture(&fixture, "scenario.equiv.key.002");

    ASSERT_EQ(asx_codec_fixture_semantic_key(&fixture, &semantic_key), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_replay_key(&fixture, &replay_key), ASX_OK);

    /* Replay key includes codec, semantic key does not — they must differ */
    ASSERT_TRUE(strcmp(semantic_key.data, replay_key.data) != 0);

    /* Semantic key must NOT contain the string "codec" */
    ASSERT_TRUE(strstr(semantic_key.data, "codec") == NULL);

    /* Replay key must contain "codec" */
    ASSERT_TRUE(strstr(replay_key.data, "codec") != NULL);

    asx_codec_buffer_reset(&replay_key);
    asx_codec_buffer_reset(&semantic_key);
    asx_canonical_fixture_reset(&fixture);
}

TEST(equiv_semantic_key_is_deterministic) {
    asx_canonical_fixture fixture;
    asx_codec_buffer key_a;
    asx_codec_buffer key_b;

    asx_canonical_fixture_init(&fixture);
    asx_codec_buffer_init(&key_a);
    asx_codec_buffer_init(&key_b);
    populate_fixture(&fixture, "scenario.equiv.key.003");

    ASSERT_EQ(asx_codec_fixture_semantic_key(&fixture, &key_a), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_semantic_key(&fixture, &key_b), ASX_OK);
    ASSERT_STR_EQ(key_a.data, key_b.data);

    asx_codec_buffer_reset(&key_b);
    asx_codec_buffer_reset(&key_a);
    asx_canonical_fixture_reset(&fixture);
}

/* ---- Semantic eq field comparison ---- */

TEST(equiv_semantic_eq_identical_fixtures) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.001");
    populate_fixture(&b, "scenario.equiv.eq.001");

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report), ASX_OK);
    ASSERT_EQ(report.count, (uint32_t)0);

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

TEST(equiv_semantic_eq_different_codec_is_ok) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.002");
    populate_fixture(&b, "scenario.equiv.eq.002");
    a.codec = ASX_CODEC_KIND_JSON;
    b.codec = ASX_CODEC_KIND_BIN;

    /* Different codec is NOT a semantic mismatch */
    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report), ASX_OK);
    ASSERT_EQ(report.count, (uint32_t)0);

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

TEST(equiv_semantic_eq_detects_seed_mismatch) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.003");
    populate_fixture(&b, "scenario.equiv.eq.003");
    b.seed = 999u;

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_EQ(report.count, (uint32_t)1);
    ASSERT_STR_EQ(report.diffs[0].field_name, "seed");

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

TEST(equiv_semantic_eq_detects_scenario_id_mismatch) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.004a");
    populate_fixture(&b, "scenario.equiv.eq.004b");

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_EQ(report.count, (uint32_t)1);
    ASSERT_STR_EQ(report.diffs[0].field_name, "scenario_id");

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

TEST(equiv_semantic_eq_detects_digest_mismatch) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.005");
    populate_fixture(&b, "scenario.equiv.eq.005");

    free(b.semantic_digest);
    b.semantic_digest = dup_text(
        "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_EQ(report.count, (uint32_t)1);
    ASSERT_STR_EQ(report.diffs[0].field_name, "semantic_digest");

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

TEST(equiv_semantic_eq_detects_multiple_mismatches) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.006a");
    populate_fixture(&b, "scenario.equiv.eq.006b");
    b.seed = 999u;
    free(b.profile);
    b.profile = dup_text("ASX_PROFILE_HFT");

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report),
              ASX_E_EQUIVALENCE_MISMATCH);
    /* scenario_id + profile + seed = 3 mismatches */
    ASSERT_TRUE(report.count >= (uint32_t)3);

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

TEST(equiv_semantic_eq_detects_provenance_mismatch) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.eq.007");
    populate_fixture(&b, "scenario.equiv.eq.007");

    free(b.provenance.capture_run_id);
    b.provenance.capture_run_id = dup_text("capture-run-9999");

    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, &report),
              ASX_E_EQUIVALENCE_MISMATCH);
    ASSERT_EQ(report.count, (uint32_t)1);
    ASSERT_STR_EQ(report.diffs[0].field_name, "provenance.capture_run_id");

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

/* ---- Null argument handling ---- */

TEST(equiv_semantic_eq_rejects_null) {
    asx_canonical_fixture f;
    asx_codec_equiv_report report;

    asx_canonical_fixture_init(&f);
    populate_fixture(&f, "scenario.equiv.null.001");

    ASSERT_EQ(asx_codec_fixture_semantic_eq(NULL, &f, &report),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_codec_fixture_semantic_eq(&f, NULL, &report),
              ASX_E_INVALID_ARGUMENT);

    asx_canonical_fixture_reset(&f);
}

TEST(equiv_semantic_key_rejects_null) {
    asx_codec_buffer buf;
    asx_codec_buffer_init(&buf);

    ASSERT_EQ(asx_codec_fixture_semantic_key(NULL, &buf),
              ASX_E_INVALID_ARGUMENT);

    asx_codec_buffer_reset(&buf);
}

TEST(equiv_cross_codec_verify_rejects_null) {
    ASSERT_EQ(asx_codec_cross_codec_verify(NULL, NULL),
              ASX_E_INVALID_ARGUMENT);
}

/* ---- Report without report pointer ---- */

TEST(equiv_semantic_eq_works_without_report) {
    asx_canonical_fixture a;
    asx_canonical_fixture b;

    asx_canonical_fixture_init(&a);
    asx_canonical_fixture_init(&b);
    populate_fixture(&a, "scenario.equiv.noreport.001");
    populate_fixture(&b, "scenario.equiv.noreport.001");

    /* NULL report is valid — just no diff details */
    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, NULL), ASX_OK);

    b.seed = 999u;
    ASSERT_EQ(asx_codec_fixture_semantic_eq(&a, &b, NULL),
              ASX_E_EQUIVALENCE_MISMATCH);

    asx_canonical_fixture_reset(&b);
    asx_canonical_fixture_reset(&a);
}

/* ---- Multiple scenario cross-codec verification ---- */

TEST(equiv_multi_scenario_cross_codec) {
    static const char *scenarios[] = {
        "scenario.multi.001",
        "scenario.multi.002",
        "scenario.multi.003"
    };
    uint32_t i;
    uint32_t n = sizeof(scenarios) / sizeof(scenarios[0]);

    for (i = 0; i < n; i++) {
        asx_canonical_fixture fixture;
        asx_codec_equiv_report report;

        asx_canonical_fixture_init(&fixture);
        populate_fixture(&fixture, scenarios[i]);
        fixture.seed = (uint64_t)(i + 1u);

        ASSERT_EQ(asx_codec_cross_codec_verify(&fixture, &report), ASX_OK);
        ASSERT_EQ(report.count, (uint32_t)0);

        asx_canonical_fixture_reset(&fixture);
    }
}

/* ---- Digest identity across codecs ---- */

TEST(equiv_digest_identity_across_codecs) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture from_json;
    asx_canonical_fixture from_bin;
    asx_codec_buffer json_buf;
    asx_codec_buffer bin_buf;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&from_json);
    asx_canonical_fixture_init(&from_bin);
    asx_codec_buffer_init(&json_buf);
    asx_codec_buffer_init(&bin_buf);
    populate_fixture(&fixture, "scenario.equiv.digest.001");

    /* Encode and decode through JSON */
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &fixture, &json_buf), ASX_OK);
    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, json_buf.data,
                                       json_buf.len, &from_json), ASX_OK);

    /* Encode and decode through BIN */
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &bin_buf), ASX_OK);
    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, bin_buf.data,
                                       bin_buf.len, &from_bin), ASX_OK);

    /* Semantic digest must survive both codecs identically */
    ASSERT_STR_EQ(from_json.semantic_digest, from_bin.semantic_digest);
    ASSERT_STR_EQ(from_json.semantic_digest, fixture.semantic_digest);

    /* All semantic content fields survive identically */
    ASSERT_STR_EQ(from_json.scenario_id, from_bin.scenario_id);
    ASSERT_STR_EQ(from_json.input_json, from_bin.input_json);
    ASSERT_STR_EQ(from_json.expected_events_json, from_bin.expected_events_json);
    ASSERT_EQ(from_json.seed, from_bin.seed);

    asx_codec_buffer_reset(&bin_buf);
    asx_codec_buffer_reset(&json_buf);
    asx_canonical_fixture_reset(&from_bin);
    asx_canonical_fixture_reset(&from_json);
    asx_canonical_fixture_reset(&fixture);
}

int main(void)
{
    fprintf(stderr, "=== test_codec_equivalence ===\n");

    /* Cross-codec round-trip */
    RUN_TEST(equiv_cross_codec_roundtrip_preserves_identity);
    RUN_TEST(equiv_cross_codec_roundtrip_with_bin_source);

    /* Semantic key codec-agnostic */
    RUN_TEST(equiv_semantic_key_ignores_codec_kind);
    RUN_TEST(equiv_semantic_key_differs_from_replay_key);
    RUN_TEST(equiv_semantic_key_is_deterministic);

    /* Semantic eq field comparison */
    RUN_TEST(equiv_semantic_eq_identical_fixtures);
    RUN_TEST(equiv_semantic_eq_different_codec_is_ok);
    RUN_TEST(equiv_semantic_eq_detects_seed_mismatch);
    RUN_TEST(equiv_semantic_eq_detects_scenario_id_mismatch);
    RUN_TEST(equiv_semantic_eq_detects_digest_mismatch);
    RUN_TEST(equiv_semantic_eq_detects_multiple_mismatches);
    RUN_TEST(equiv_semantic_eq_detects_provenance_mismatch);

    /* Null argument handling */
    RUN_TEST(equiv_semantic_eq_rejects_null);
    RUN_TEST(equiv_semantic_key_rejects_null);
    RUN_TEST(equiv_cross_codec_verify_rejects_null);

    /* Report-less mode */
    RUN_TEST(equiv_semantic_eq_works_without_report);

    /* Multiple scenarios */
    RUN_TEST(equiv_multi_scenario_cross_codec);

    /* Digest identity */
    RUN_TEST(equiv_digest_identity_across_codecs);

    TEST_REPORT();
    return test_failures;
}
