/*
 * e2e_codec_parity.c — end-to-end codec equivalence and parity scenarios
 *
 * Exercises: JSON round-trip, BIN round-trip, cross-codec semantic
 * equivalence, replay key identity, and trace digest stability.
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

static char *dup_str(const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1u);
    if (copy != NULL) { memcpy(copy, s, len + 1u); }
    return copy;
}

/* Build a test fixture with heap-allocated strings (required for reset) */
static void build_test_fixture(asx_canonical_fixture *f, asx_codec_kind codec) {
    asx_canonical_fixture_init(f);
    f->scenario_id = dup_str("lifecycle.region_open_close");
    f->fixture_schema_version = dup_str("fixture-v1");
    f->scenario_dsl_version = dup_str("dsl-v1");
    f->profile = dup_str("ASX_PROFILE_CORE");
    f->codec = codec;
    f->seed = 42;
    f->input_json = dup_str("{\"ops\":[\"open\",\"close\"]}");
    f->expected_events_json = dup_str("[\"region_open\",\"region_close\"]");
    f->expected_final_snapshot_json = dup_str("{\"regions\":0}");
    f->expected_error_codes_json = dup_str("[]");
    f->semantic_digest = dup_str("sha256:0123456789abcdef0123456789abcdef"
                                 "0123456789abcdef0123456789abcdef");
    f->provenance.rust_baseline_commit = dup_str("0123456789abcdef0123456789abcdef01234567");
    f->provenance.rust_toolchain_commit_hash = dup_str("toolchain-abc123");
    f->provenance.rust_toolchain_release = dup_str("rustc 1.90.0");
    f->provenance.rust_toolchain_host = dup_str("x86_64-unknown-linux-gnu");
    f->provenance.cargo_lock_sha256 =
        dup_str("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    f->provenance.capture_run_id = dup_str("capture-run-e2e-001");
}

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

static void scenario_json_roundtrip(void) {
    SCENARIO_BEGIN("codec.json_roundtrip");

    asx_canonical_fixture original;
    asx_canonical_fixture decoded;
    asx_codec_buffer buf;

    build_test_fixture(&original, ASX_CODEC_KIND_JSON);
    asx_codec_buffer_init(&buf);

    asx_status rc = asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, &original, &buf);
    SCENARIO_CHECK(rc == ASX_OK, "json encode failed");
    SCENARIO_CHECK(buf.len > 0, "json payload is empty");

    asx_canonical_fixture_init(&decoded);
    rc = asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, buf.data, buf.len, &decoded);
    SCENARIO_CHECK(rc == ASX_OK, "json decode failed");

    /* Verify semantic fields */
    SCENARIO_CHECK(strcmp(decoded.scenario_id, original.scenario_id) == 0, "scenario_id mismatch");
    SCENARIO_CHECK(decoded.seed == original.seed, "seed mismatch");
    SCENARIO_CHECK(strcmp(decoded.profile, original.profile) == 0, "profile mismatch");
    SCENARIO_CHECK(decoded.codec == ASX_CODEC_KIND_JSON, "codec mismatch");

    asx_codec_buffer_reset(&buf);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&original);

    SCENARIO_END();
}

static void scenario_bin_roundtrip(void) {
    SCENARIO_BEGIN("codec.bin_roundtrip");

    asx_canonical_fixture original;
    asx_canonical_fixture decoded;
    asx_codec_buffer buf;

    build_test_fixture(&original, ASX_CODEC_KIND_BIN);
    asx_codec_buffer_init(&buf);

    asx_status rc = asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, &original, &buf);
    SCENARIO_CHECK(rc == ASX_OK, "bin encode failed");
    SCENARIO_CHECK(buf.len > 0, "bin payload is empty");

    asx_canonical_fixture_init(&decoded);
    rc = asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, buf.data, buf.len, &decoded);
    SCENARIO_CHECK(rc == ASX_OK, "bin decode failed");

    SCENARIO_CHECK(strcmp(decoded.scenario_id, original.scenario_id) == 0, "scenario_id mismatch");
    SCENARIO_CHECK(decoded.seed == original.seed, "seed mismatch");
    SCENARIO_CHECK(decoded.codec == ASX_CODEC_KIND_BIN, "codec mismatch");

    asx_codec_buffer_reset(&buf);
    asx_canonical_fixture_reset(&decoded);
    asx_canonical_fixture_reset(&original);

    SCENARIO_END();
}

static void scenario_cross_codec_verify(void) {
    SCENARIO_BEGIN("codec.cross_codec_verify");

    asx_canonical_fixture fixture;
    asx_codec_equiv_report report;

    build_test_fixture(&fixture, ASX_CODEC_KIND_JSON);
    asx_codec_equiv_report_init(&report);

    asx_status rc = asx_codec_cross_codec_verify(&fixture, &report);
    SCENARIO_CHECK(rc == ASX_OK, "cross_codec_verify failed");
    SCENARIO_CHECK(report.count == 0, "equivalence mismatches found");

    asx_canonical_fixture_reset(&fixture);

    SCENARIO_END();
}

static void scenario_semantic_equivalence(void) {
    SCENARIO_BEGIN("codec.semantic_equivalence");

    asx_canonical_fixture a, b;
    asx_codec_equiv_report report;

    build_test_fixture(&a, ASX_CODEC_KIND_JSON);
    build_test_fixture(&b, ASX_CODEC_KIND_BIN);

    asx_codec_equiv_report_init(&report);
    asx_status rc = asx_codec_fixture_semantic_eq(&a, &b, &report);
    SCENARIO_CHECK(rc == ASX_OK, "semantic_eq should ignore codec field");
    SCENARIO_CHECK(report.count == 0, "unexpected mismatches");

    asx_canonical_fixture_reset(&a);
    asx_canonical_fixture_reset(&b);

    SCENARIO_END();
}

static void scenario_replay_key_identity(void) {
    SCENARIO_BEGIN("codec.replay_key_identity");

    asx_canonical_fixture fixture;
    asx_codec_buffer key1, key2;

    build_test_fixture(&fixture, ASX_CODEC_KIND_JSON);
    asx_codec_buffer_init(&key1);
    asx_codec_buffer_init(&key2);

    asx_status rc = asx_codec_fixture_replay_key(&fixture, &key1);
    SCENARIO_CHECK(rc == ASX_OK, "replay_key_1 failed");

    /* Same fixture should produce identical key */
    rc = asx_codec_fixture_replay_key(&fixture, &key2);
    SCENARIO_CHECK(rc == ASX_OK, "replay_key_2 failed");

    SCENARIO_CHECK(key1.len == key2.len && key1.len > 0, "key length mismatch");
    SCENARIO_CHECK(memcmp(key1.data, key2.data, key1.len) == 0, "replay key not deterministic");

    asx_codec_buffer_reset(&key1);
    asx_codec_buffer_reset(&key2);
    asx_canonical_fixture_reset(&fixture);

    SCENARIO_END();
}

static void scenario_semantic_key_codec_agnostic(void) {
    SCENARIO_BEGIN("codec.semantic_key_codec_agnostic");

    asx_canonical_fixture a, b;
    asx_codec_buffer key_a, key_b;

    build_test_fixture(&a, ASX_CODEC_KIND_JSON);
    build_test_fixture(&b, ASX_CODEC_KIND_BIN);

    asx_codec_buffer_init(&key_a);
    asx_codec_buffer_init(&key_b);

    asx_status rc = asx_codec_fixture_semantic_key(&a, &key_a);
    SCENARIO_CHECK(rc == ASX_OK, "semantic_key_a failed");

    rc = asx_codec_fixture_semantic_key(&b, &key_b);
    SCENARIO_CHECK(rc == ASX_OK, "semantic_key_b failed");

    SCENARIO_CHECK(key_a.len == key_b.len && key_a.len > 0, "semantic key length mismatch");
    SCENARIO_CHECK(memcmp(key_a.data, key_b.data, key_a.len) == 0,
                   "semantic key differs between codecs");

    asx_codec_buffer_reset(&key_a);
    asx_codec_buffer_reset(&key_b);
    asx_canonical_fixture_reset(&a);
    asx_canonical_fixture_reset(&b);

    SCENARIO_END();
}

static void scenario_fixture_validation(void) {
    SCENARIO_BEGIN("codec.fixture_validation");

    asx_canonical_fixture valid;
    build_test_fixture(&valid, ASX_CODEC_KIND_JSON);

    asx_status rc = asx_canonical_fixture_validate(&valid);
    SCENARIO_CHECK(rc == ASX_OK, "valid fixture should pass validation");

    /* Empty fixture should fail validation */
    asx_canonical_fixture empty;
    asx_canonical_fixture_init(&empty);
    rc = asx_canonical_fixture_validate(&empty);
    SCENARIO_CHECK(rc != ASX_OK, "empty fixture should fail validation");

    asx_canonical_fixture_reset(&valid);

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_json_roundtrip();
    scenario_bin_roundtrip();
    scenario_cross_codec_verify();
    scenario_semantic_equivalence();
    scenario_replay_key_identity();
    scenario_semantic_key_codec_agnostic();
    scenario_fixture_validation();

    fprintf(stderr, "[e2e] codec_parity: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
