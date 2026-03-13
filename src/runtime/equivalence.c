/*
 * equivalence.c — cross-codec semantic equivalence engine
 *
 * Compares canonical fixtures across codecs (JSON, BIN) to verify that
 * encoding format does not alter semantic content. Field-level diff
 * reporting supports actionable mismatch triage.
 *
 * SPDX-License-Identifier: MIT
 */

#include "codec_internal.h"
#include <asx/codec/codec.h>
#include <asx/codec/equivalence.h>
#include <asx/codec/schema.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Report helpers                                                      */
/* ------------------------------------------------------------------ */

void asx_codec_equiv_report_init(asx_codec_equiv_report *report) {
    if (report == NULL) return;
    memset(report, 0, sizeof(*report));
}

static void report_add_diff(asx_codec_equiv_report *report, const char *field_name) {
    size_t len;

    if (report == NULL) return;
    if (report->count >= ASX_EQUIV_MAX_DIFFS) return;

    len = strlen(field_name);
    if (len >= ASX_EQUIV_MAX_FIELD_NAME) { len = ASX_EQUIV_MAX_FIELD_NAME - 1u; }
    memcpy(report->diffs[report->count].field_name, field_name, len);
    report->diffs[report->count].field_name[len] = '\0';
    report->count++;
}

/* ------------------------------------------------------------------ */
/* String comparison helper (NULL-safe)                                */
/* ------------------------------------------------------------------ */

static int str_eq(const char *a, const char *b) {
    if (a == NULL && b == NULL) return 1;
    if (a == NULL || b == NULL) return 0;
    return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------ */
/* Semantic equivalence                                                */
/* ------------------------------------------------------------------ */

asx_status asx_codec_fixture_semantic_eq(const asx_canonical_fixture *a,
                                         const asx_canonical_fixture *b,
                                         asx_codec_equiv_report *report) {
    int mismatch = 0;

    if (a == NULL || b == NULL) return ASX_E_INVALID_ARGUMENT;

    if (report != NULL) { asx_codec_equiv_report_init(report); }

    /* Compare all semantic fields — codec is explicitly excluded */

    if (!str_eq(a->scenario_id, b->scenario_id)) {
        report_add_diff(report, "scenario_id");
        mismatch = 1;
    }
    if (!str_eq(a->fixture_schema_version, b->fixture_schema_version)) {
        report_add_diff(report, "fixture_schema_version");
        mismatch = 1;
    }
    if (!str_eq(a->scenario_dsl_version, b->scenario_dsl_version)) {
        report_add_diff(report, "scenario_dsl_version");
        mismatch = 1;
    }
    if (!str_eq(a->profile, b->profile)) {
        report_add_diff(report, "profile");
        mismatch = 1;
    }
    if (a->seed != b->seed) {
        report_add_diff(report, "seed");
        mismatch = 1;
    }
    if (!str_eq(a->input_json, b->input_json)) {
        report_add_diff(report, "input_json");
        mismatch = 1;
    }
    if (!str_eq(a->expected_events_json, b->expected_events_json)) {
        report_add_diff(report, "expected_events_json");
        mismatch = 1;
    }
    if (!str_eq(a->expected_final_snapshot_json, b->expected_final_snapshot_json)) {
        report_add_diff(report, "expected_final_snapshot_json");
        mismatch = 1;
    }
    if (!str_eq(a->expected_error_codes_json, b->expected_error_codes_json)) {
        report_add_diff(report, "expected_error_codes_json");
        mismatch = 1;
    }
    if (!str_eq(a->semantic_digest, b->semantic_digest)) {
        report_add_diff(report, "semantic_digest");
        mismatch = 1;
    }

    /* Provenance fields */
    if (!str_eq(a->provenance.rust_baseline_commit, b->provenance.rust_baseline_commit)) {
        report_add_diff(report, "provenance.rust_baseline_commit");
        mismatch = 1;
    }
    if (!str_eq(a->provenance.rust_toolchain_commit_hash,
                b->provenance.rust_toolchain_commit_hash)) {
        report_add_diff(report, "provenance.rust_toolchain_commit_hash");
        mismatch = 1;
    }
    if (!str_eq(a->provenance.rust_toolchain_release, b->provenance.rust_toolchain_release)) {
        report_add_diff(report, "provenance.rust_toolchain_release");
        mismatch = 1;
    }
    if (!str_eq(a->provenance.rust_toolchain_host, b->provenance.rust_toolchain_host)) {
        report_add_diff(report, "provenance.rust_toolchain_host");
        mismatch = 1;
    }
    if (!str_eq(a->provenance.cargo_lock_sha256, b->provenance.cargo_lock_sha256)) {
        report_add_diff(report, "provenance.cargo_lock_sha256");
        mismatch = 1;
    }
    if (!str_eq(a->provenance.capture_run_id, b->provenance.capture_run_id)) {
        report_add_diff(report, "provenance.capture_run_id");
        mismatch = 1;
    }

    return mismatch ? ASX_E_EQUIVALENCE_MISMATCH : ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Codec-agnostic semantic key                                         */
/* ------------------------------------------------------------------ */

asx_status asx_codec_fixture_semantic_key(const asx_canonical_fixture *fixture,
                                          asx_codec_buffer *out_key) {
    asx_status st;
    int is_first = 1;

    if (fixture == NULL || out_key == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = asx_canonical_fixture_validate(fixture);
    if (st != ASX_OK) { return st; }

    out_key->len = 0u;
    if (out_key->data != NULL) { out_key->data[0] = '\0'; }

    st = asx_codec_buffer_append_char(out_key, '{');
    if (st != ASX_OK) return st;

    /* Alphabetical field order, codec excluded */
    st = asx_codec_buffer_append_string_field(out_key, &is_first, "profile", fixture->profile);
    if (st != ASX_OK) return st;

    st = asx_codec_buffer_append_string_field(out_key, &is_first, "scenario_id",
                                              fixture->scenario_id);
    if (st != ASX_OK) return st;

    st = asx_codec_buffer_append_u64_field(out_key, &is_first, "seed", fixture->seed);
    if (st != ASX_OK) return st;

    st = asx_codec_buffer_append_string_field(out_key, &is_first, "semantic_digest",
                                              fixture->semantic_digest);
    if (st != ASX_OK) return st;

    return asx_codec_buffer_append_char(out_key, '}');
}

/* ------------------------------------------------------------------ */
/* Cross-codec round-trip verification                                 */
/* ------------------------------------------------------------------ */

asx_status asx_codec_cross_codec_verify(const asx_canonical_fixture *fixture,
                                        asx_codec_equiv_report *report) {
    asx_canonical_fixture from_json;
    asx_canonical_fixture from_bin;
    asx_codec_buffer json_buf;
    asx_codec_buffer bin_buf;
    asx_status st;
    asx_status result;

    if (fixture == NULL) return ASX_E_INVALID_ARGUMENT;

    asx_canonical_fixture_init(&from_json);
    asx_canonical_fixture_init(&from_bin);
    asx_codec_buffer_init(&json_buf);
    asx_codec_buffer_init(&bin_buf);

    /* Encode as JSON */
    st = asx_codec_encode_fixture(ASX_CODEC_KIND_JSON, fixture, &json_buf);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Encode as BIN */
    st = asx_codec_encode_fixture(ASX_CODEC_KIND_BIN, fixture, &bin_buf);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Decode JSON */
    st = asx_codec_decode_fixture(ASX_CODEC_KIND_JSON, json_buf.data, json_buf.len, &from_json);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Decode BIN */
    st = asx_codec_decode_fixture(ASX_CODEC_KIND_BIN, bin_buf.data, bin_buf.len, &from_bin);
    if (st != ASX_OK) {
        result = st;
        goto cleanup;
    }

    /* Compare decoded fixtures for semantic equivalence */
    result = asx_codec_fixture_semantic_eq(&from_json, &from_bin, report);

cleanup:
    asx_codec_buffer_reset(&bin_buf);
    asx_codec_buffer_reset(&json_buf);
    asx_canonical_fixture_reset(&from_bin);
    asx_canonical_fixture_reset(&from_json);
    return result;
}
