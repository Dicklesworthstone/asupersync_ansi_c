/*
 * codec_json_baseline_test.c — conformance-shape smoke for JSON baseline
 *
 * SPDX-License-Identifier: MIT
 */

#include "../test_harness.h"
#include <asx/asx.h>
#include <string.h>

TEST(canonical_fixture_decode_encode_smoke) {
    const char *fixture_json =
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
        "\"scenario_id\":\"scenario.codec.json.baseline\","
        "\"seed\":7,"
        "\"semantic_digest\":\"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\""
        "}";
    asx_canonical_fixture fixture;
    asx_canonical_fixture decoded_bin;
    asx_codec_buffer encoded;
    asx_codec_buffer encoded_bin;

    asx_canonical_fixture_init(&fixture);
    asx_canonical_fixture_init(&decoded_bin);
    asx_codec_buffer_init(&encoded);
    asx_codec_buffer_init(&encoded_bin);

    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_JSON,
                                       fixture_json,
                                       strlen(fixture_json),
                                       &fixture),
              ASX_OK);
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &fixture, &encoded), ASX_OK);
    ASSERT_NE(encoded.data, NULL);
    ASSERT_TRUE(encoded.len > 0u);
    ASSERT_STR_EQ(asx_codec_kind_str(fixture.codec), "json");

    fixture.codec = ASX_CODEC_KIND_BIN;
    ASSERT_EQ(asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &fixture, &encoded_bin), ASX_OK);
    ASSERT_TRUE(encoded_bin.len > 0u);
    ASSERT_EQ(asx_codec_decode_fixture(ASX_CODEC_KIND_BIN,
                                       encoded_bin.data,
                                       encoded_bin.len,
                                       &decoded_bin),
              ASX_OK);
    ASSERT_STR_EQ(decoded_bin.scenario_id, fixture.scenario_id);
    ASSERT_EQ(decoded_bin.codec, ASX_CODEC_KIND_BIN);

    asx_codec_buffer_reset(&encoded_bin);
    asx_codec_buffer_reset(&encoded);
    asx_canonical_fixture_reset(&decoded_bin);
    asx_canonical_fixture_reset(&fixture);
}

int main(void) {
    fprintf(stderr, "=== codec_json_baseline_test ===\n");
    RUN_TEST(canonical_fixture_decode_encode_smoke);
    TEST_REPORT();
    return test_failures;
}
