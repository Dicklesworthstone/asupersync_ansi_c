/*
 * test_codec_json.c — JSON codec baseline tests (bd-2n0.1)
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_SLICE_EQ(slice_value, expected_text) do { \
    size_t _expected_len = strlen((expected_text)); \
    ASSERT_EQ((slice_value).len, _expected_len); \
    ASSERT_TRUE(memcmp((slice_value).ptr, (expected_text), _expected_len) == 0); \
} while (0)

static char *dup_text(const char *text)
{
    size_t len;
    char *copy;

    len = strlen(text);
    copy = (char *)malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1u);
    return copy;
}

static void populate_fixture(asx_canonical_fixture *fixture)
{
    fixture->scenario_id = dup_text("scenario.codec.json.001");
    fixture->fixture_schema_version = dup_text("fixture-v1");
    fixture->scenario_dsl_version = dup_text("dsl-v1");
    fixture->profile = dup_text("ASX_PROFILE_CORE");
    fixture->codec = ASX_CODEC_KIND_JSON;
    fixture->seed = 42u;
    fixture->input_json = dup_text("{\"ops\":[]}");
    fixture->expected_events_json = dup_text("[]");
    fixture->expected_final_snapshot_json = dup_text("{}");
    fixture->expected_error_codes_json = dup_text("[]");
    fixture->semantic_digest = dup_text(
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    fixture->provenance.rust_baseline_commit = dup_text("0123456789abcdef0123456789abcdef01234567");
    fixture->provenance.rust_toolchain_commit_hash = dup_text("toolchain-abcdef12");
    fixture->provenance.rust_toolchain_release = dup_text("rustc 1.90.0");
    fixture->provenance.rust_toolchain_host = dup_text("x86_64-unknown-linux-gnu");
    fixture->provenance.cargo_lock_sha256 = dup_text(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    fixture->provenance.capture_run_id = dup_text("capture-run-0001");
}

static uint32_t load_be32_at(const unsigned char *bytes, size_t offset)
{
    uint32_t v = 0u;
    v |= ((uint32_t)bytes[offset]) << 24;
    v |= ((uint32_t)bytes[offset + 1u]) << 16;
    v |= ((uint32_t)bytes[offset + 2u]) << 8;
    v |= ((uint32_t)bytes[offset + 3u]);
    return v;
}

TEST(json_round_trip_is_stable) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer json_a;
    asx_codec_buffer json_b;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&json_a);
    asx_codec_buffer_init(&json_b);

    populate_fixture(&fixture);

    ASSERT_EQ(asx_canonical_fixture_validate(&fixture), ASX_OK);
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &fixture, &json_a), ASX_OK);
    ASSERT_NE(json_a.data, NULL);
    ASSERT_TRUE(json_a.len > 0u);

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, json_a.data, json_a.len, &decoded), ASX_OK);
    ASSERT_STR_EQ(decoded.scenario_id, fixture.scenario_id);
    ASSERT_STR_EQ(decoded.profile, fixture.profile);
    ASSERT_EQ(decoded.seed, fixture.seed);
    ASSERT_EQ(decoded.codec, ASX_CODEC_KIND_JSON);

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &decoded, &json_b), ASX_OK);
    ASSERT_STR_EQ(json_a.data, json_b.data);

    asx_codec_buffer_reset(&json_b);
    asx_codec_buffer_reset(&json_a);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&fixture);
}

TEST(decode_rejects_missing_required_field) {
    const char *missing_scenario =
        "{"
        "\"codec\":\"json\","
        "\"expected_error_codes\":[],"
        "\"expected_events\":[],"
        "\"expected_final_snapshot\":{},"
        "\"fixture_schema_version\":\"fixture-v1\","
        "\"input\":{\"ops\":[]},"
        "\"profile\":\"ASX_PROFILE_CORE\","
        "\"provenance\":{"
          "\"cargo_lock_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
          "\"capture_run_id\":\"capture-run-0001\","
          "\"rust_baseline_commit\":\"0123456789abcdef0123456789abcdef01234567\","
          "\"rust_toolchain_commit_hash\":\"toolchain-abcdef12\","
          "\"rust_toolchain_host\":\"x86_64-unknown-linux-gnu\","
          "\"rust_toolchain_release\":\"rustc 1.90.0\""
        "},"
        "\"scenario_dsl_version\":\"dsl-v1\","
        "\"seed\":42,"
        "\"semantic_digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\""
        "}";
    asx_canonical_fixture fixture;

    asx_canonical_fixture_init(&fixture);
    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON,
                                       missing_scenario,
                                       strlen(missing_scenario),
                                       &fixture),
              ASX_E_INVALID_ARGUMENT);
    asx_canonical_fixture_reset(&fixture);
}

TEST(decode_rejects_invalid_digest_pattern) {
    const char *bad_digest =
        "{"
        "\"codec\":\"json\","
        "\"expected_error_codes\":[],"
        "\"expected_events\":[],"
        "\"expected_final_snapshot\":{},"
        "\"fixture_schema_version\":\"fixture-v1\","
        "\"input\":{\"ops\":[]},"
        "\"profile\":\"ASX_PROFILE_CORE\","
        "\"provenance\":{"
          "\"cargo_lock_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
          "\"capture_run_id\":\"capture-run-0001\","
          "\"rust_baseline_commit\":\"0123456789abcdef0123456789abcdef01234567\","
          "\"rust_toolchain_commit_hash\":\"toolchain-abcdef12\","
          "\"rust_toolchain_host\":\"x86_64-unknown-linux-gnu\","
          "\"rust_toolchain_release\":\"rustc 1.90.0\""
        "},"
        "\"scenario_dsl_version\":\"dsl-v1\","
        "\"scenario_id\":\"scenario.codec.json.001\","
        "\"seed\":42,"
        "\"semantic_digest\":\"sha256:XYZ\""
        "}";
    asx_canonical_fixture fixture;

    asx_canonical_fixture_init(&fixture);
    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON,
                                       bad_digest,
                                       strlen(bad_digest),
                                       &fixture),
              ASX_E_INVALID_ARGUMENT);
    asx_canonical_fixture_reset(&fixture);
}

TEST(replay_key_is_deterministic) {
    asx_canonical_fixture fixture;
    asx_codec_buffer key_a;
    asx_codec_buffer key_b;

    asx_canonical_fixture_init(&fixture);
    asx_codec_buffer_init(&key_a);
    asx_codec_buffer_init(&key_b);

    populate_fixture(&fixture);

    ASSERT_EQ(asx_codec_fixture_replay_key(&fixture, &key_a), ASX_OK);
    ASSERT_EQ(asx_codec_fixture_replay_key(&fixture, &key_b), ASX_OK);
    ASSERT_STR_EQ(key_a.data, key_b.data);

    asx_codec_buffer_reset(&key_b);
    asx_codec_buffer_reset(&key_a);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_round_trip_and_zero_copy_view_are_stable) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer out;
    asx_codec_bin_fixture_view view;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&out);
    asx_codec_bin_fixture_view_init(&view);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &out), ASX_OK);
    ASSERT_TRUE(out.len > 0u);

    ASSERT_EQ(asx_codec_decode_fixture_bin_view(out.data, out.len, &view), ASX_OK);
    ASSERT_EQ(view.frame_schema_version, ASX_CODEC_BIN_FRAME_SCHEMA_VERSION_V1);
    ASSERT_EQ(view.message_type, ASX_CODEC_BIN_FRAME_MESSAGE_FIXTURE);
    ASSERT_EQ(view.codec, ASX_CODEC_KIND_BIN);
    ASSERT_EQ(view.seed, fixture.seed);
    ASSERT_SLICE_EQ(view.scenario_id, fixture.scenario_id);
    ASSERT_SLICE_EQ(view.fixture_schema_version, fixture.fixture_schema_version);
    ASSERT_SLICE_EQ(view.scenario_dsl_version, fixture.scenario_dsl_version);
    ASSERT_SLICE_EQ(view.profile, fixture.profile);
    ASSERT_SLICE_EQ(view.input_json, fixture.input_json);
    ASSERT_SLICE_EQ(view.expected_events_json, fixture.expected_events_json);
    ASSERT_SLICE_EQ(view.expected_final_snapshot_json, fixture.expected_final_snapshot_json);
    ASSERT_SLICE_EQ(view.expected_error_codes_json, fixture.expected_error_codes_json);
    ASSERT_SLICE_EQ(view.semantic_digest, fixture.semantic_digest);

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, out.data, out.len, &decoded), ASX_OK);
    ASSERT_STR_EQ(decoded.scenario_id, fixture.scenario_id);
    ASSERT_STR_EQ(decoded.fixture_schema_version, fixture.fixture_schema_version);
    ASSERT_STR_EQ(decoded.scenario_dsl_version, fixture.scenario_dsl_version);
    ASSERT_STR_EQ(decoded.profile, fixture.profile);
    ASSERT_EQ(decoded.codec, ASX_CODEC_KIND_BIN);
    ASSERT_EQ(decoded.seed, fixture.seed);
    ASSERT_STR_EQ(decoded.input_json, fixture.input_json);
    ASSERT_STR_EQ(decoded.expected_events_json, fixture.expected_events_json);
    ASSERT_STR_EQ(decoded.expected_final_snapshot_json, fixture.expected_final_snapshot_json);
    ASSERT_STR_EQ(decoded.expected_error_codes_json, fixture.expected_error_codes_json);
    ASSERT_STR_EQ(decoded.semantic_digest, fixture.semantic_digest);

    asx_canonical_fixture_reset(&decoded);
    asx_codec_buffer_reset(&out);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_decode_rejects_frame_schema_mismatch) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer out;
    unsigned char *bytes;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &out), ASX_OK);
    ASSERT_TRUE(out.len > 4u);
    bytes = (unsigned char *)out.data;
    bytes[4] = (unsigned char)(bytes[4] + 1u);

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, out.data, out.len, &decoded),
              ASX_E_INVALID_ARGUMENT);

    asx_codec_buffer_reset(&out);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_decode_rejects_checksum_corruption) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer out;
    unsigned char *bytes;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &out), ASX_OK);
    ASSERT_TRUE(out.len > 0u);
    bytes = (unsigned char *)out.data;
    bytes[out.len - 1u] ^= 0x01u;

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, out.data, out.len, &decoded),
              ASX_E_INVALID_ARGUMENT);

    asx_codec_buffer_reset(&out);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_decode_rejects_truncated_payload) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer out;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &out), ASX_OK);
    ASSERT_TRUE(out.len > 0u);

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, out.data, out.len - 1u, &decoded),
              ASX_E_INVALID_ARGUMENT);

    asx_codec_buffer_reset(&out);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_payload_is_more_compact_than_json_for_same_fixture) {
    asx_canonical_fixture fixture;
    asx_codec_buffer json_out;
    asx_codec_buffer bin_out;

    asx_canonical_fixture_init(&fixture);
    asx_codec_buffer_init(&json_out);
    asx_codec_buffer_init(&bin_out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &fixture, &json_out), ASX_OK);
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &bin_out), ASX_OK);
    ASSERT_TRUE(bin_out.len < json_out.len);

    asx_codec_buffer_reset(&bin_out);
    asx_codec_buffer_reset(&json_out);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_header_payload_length_is_big_endian) {
    asx_canonical_fixture fixture;
    asx_codec_buffer bin_out;
    uint32_t declared_len;
    size_t expected_payload_len;
    const unsigned char *bytes;

    asx_canonical_fixture_init(&fixture);
    asx_codec_buffer_init(&bin_out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &bin_out), ASX_OK);
    ASSERT_TRUE(bin_out.len > 15u);
    bytes = (const unsigned char *)bin_out.data;

    declared_len = load_be32_at(bytes, 7u);
    expected_payload_len = bin_out.len - 11u - 4u;
    ASSERT_EQ((size_t)declared_len, expected_payload_len);

    asx_codec_buffer_reset(&bin_out);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_decode_accepts_unaligned_input_pointer) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer bin_out;
    unsigned char *padded;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&bin_out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &bin_out), ASX_OK);
    padded = (unsigned char *)malloc(bin_out.len + 1u);
    ASSERT_NE(padded, NULL);
    memcpy(padded + 1u, bin_out.data, bin_out.len);

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, padded + 1u, bin_out.len, &decoded), ASX_OK);
    ASSERT_STR_EQ(decoded.semantic_digest, fixture.semantic_digest);

    free(padded);
    asx_codec_buffer_reset(&bin_out);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&fixture);
}

TEST(bin_decode_rejects_little_endian_length_mutation) {
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded;
    asx_codec_buffer bin_out;
    unsigned char *bytes;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded);
    asx_codec_buffer_init(&bin_out);
    populate_fixture(&fixture);
    fixture.codec = ASX_CODEC_KIND_BIN;

    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &bin_out), ASX_OK);
    ASSERT_TRUE(bin_out.len > 15u);

    bytes = (unsigned char *)bin_out.data;
    {
        unsigned char b0 = bytes[7u];
        unsigned char b1 = bytes[8u];
        unsigned char b2 = bytes[9u];
        unsigned char b3 = bytes[10u];
        bytes[7u] = b3;
        bytes[8u] = b2;
        bytes[9u] = b1;
        bytes[10u] = b0;
    }

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, bin_out.data, bin_out.len, &decoded),
              ASX_E_INVALID_ARGUMENT);

    asx_codec_buffer_reset(&bin_out);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&fixture);
}

int main(void) {
    fprintf(stderr, "=== test_codec_json ===\n");
    RUN_TEST(json_round_trip_is_stable);
    RUN_TEST(decode_rejects_missing_required_field);
    RUN_TEST(decode_rejects_invalid_digest_pattern);
    RUN_TEST(replay_key_is_deterministic);
    RUN_TEST(bin_round_trip_and_zero_copy_view_are_stable);
    RUN_TEST(bin_decode_rejects_frame_schema_mismatch);
    RUN_TEST(bin_decode_rejects_checksum_corruption);
    RUN_TEST(bin_decode_rejects_truncated_payload);
    RUN_TEST(bin_payload_is_more_compact_than_json_for_same_fixture);
    RUN_TEST(bin_header_payload_length_is_big_endian);
    RUN_TEST(bin_decode_accepts_unaligned_input_pointer);
    RUN_TEST(bin_decode_rejects_little_endian_length_mutation);
    TEST_REPORT();
    return test_failures;
}
