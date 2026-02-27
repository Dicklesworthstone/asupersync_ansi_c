/*
 * asx/codec/schema.h — canonical fixture schema model for codec surfaces
 *
 * Provides an in-memory representation of canonical fixture payloads
 * consumed by JSON/BIN codec implementations.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CODEC_SCHEMA_H
#define ASX_CODEC_SCHEMA_H

#include <stdint.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ASX_CODEC_KIND_JSON = 0,
    ASX_CODEC_KIND_BIN  = 1
} asx_codec_kind;

typedef struct {
    char *rust_baseline_commit;
    char *rust_toolchain_commit_hash;
    char *rust_toolchain_release;
    char *rust_toolchain_host;
    char *cargo_lock_sha256;
    char *capture_run_id;
} asx_fixture_provenance;

typedef struct {
    char *scenario_id;
    char *fixture_schema_version;
    char *scenario_dsl_version;
    char *profile;
    asx_codec_kind codec;
    uint64_t seed;
    char *input_json;
    char *expected_events_json;
    char *expected_final_snapshot_json;
    char *expected_error_codes_json;
    char *semantic_digest;
    asx_fixture_provenance provenance;
} asx_canonical_fixture;

/* Initialize fixture fields to zero/empty defaults. */
ASX_API void asx_canonical_fixture_init(asx_canonical_fixture *fixture);

/* Free owned fixture strings and reset to defaults. */
ASX_API void asx_canonical_fixture_reset(asx_canonical_fixture *fixture);

/* Validate required canonical schema fields and deterministic constraints. */
ASX_API ASX_MUST_USE asx_status asx_canonical_fixture_validate(const asx_canonical_fixture *fixture);

/* Map enum to canonical codec string ("json" / "bin"). */
ASX_API ASX_MUST_USE const char *asx_codec_kind_str(asx_codec_kind codec);

/* Parse canonical codec string into enum representation. */
ASX_API ASX_MUST_USE asx_status asx_codec_kind_parse(const char *text, asx_codec_kind *out_codec);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CODEC_SCHEMA_H */
