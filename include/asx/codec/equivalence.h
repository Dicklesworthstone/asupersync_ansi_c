/*
 * asx/codec/equivalence.h — cross-codec semantic equivalence checking
 *
 * Verifies that encoding the same canonical fixture through different
 * codecs (JSON, BIN) produces semantically identical round-trip results.
 * The codec field itself may differ; all other semantic fields must match.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CODEC_EQUIVALENCE_H
#define ASX_CODEC_EQUIVALENCE_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/codec/codec.h>
#include <asx/codec/schema.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ASX_EQUIV_MAX_FIELD_NAME = 48u, ASX_EQUIV_MAX_DIFFS = 16u };

/* A single field-level mismatch between two fixtures. */
typedef struct {
    char field_name[ASX_EQUIV_MAX_FIELD_NAME];
} asx_codec_equiv_diff;

/* Aggregate mismatch report from a semantic comparison. */
typedef struct {
    uint32_t count;
    asx_codec_equiv_diff diffs[ASX_EQUIV_MAX_DIFFS];
} asx_codec_equiv_report;

/* Initialize a report to empty state. */
ASX_API void asx_codec_equiv_report_init(asx_codec_equiv_report *report);

/*
 * Compare two fixtures for semantic equivalence.
 *
 * All fields are compared except `codec`. Returns ASX_OK if all semantic
 * fields match, ASX_E_EQUIVALENCE_MISMATCH if any differ. When report
 * is non-NULL, mismatched field names are recorded for diagnostics.
 */
ASX_API ASX_MUST_USE asx_status asx_codec_fixture_semantic_eq(const asx_canonical_fixture *a,
                                                              const asx_canonical_fixture *b,
                                                              asx_codec_equiv_report *report);

/*
 * Compute a codec-agnostic canonical key from semantic fixture fields.
 *
 * Like asx_codec_fixture_replay_key() but excludes the codec field,
 * producing a key that is identical regardless of encoding format.
 */
ASX_API ASX_MUST_USE asx_status asx_codec_fixture_semantic_key(const asx_canonical_fixture *fixture,
                                                               asx_codec_buffer *out_key);

/*
 * Cross-codec round-trip verification.
 *
 * Encodes the fixture through both JSON and BIN codecs, decodes both
 * back into fixtures, and verifies semantic equivalence. Returns ASX_OK
 * if the round-trip preserves semantic identity, or the first error
 * encountered (encode/decode failure or equivalence mismatch).
 *
 * When report is non-NULL, it is reset before verification begins and any
 * mismatched fields are recorded if semantic comparison is reached.
 */
ASX_API ASX_MUST_USE asx_status asx_codec_cross_codec_verify(const asx_canonical_fixture *fixture,
                                                             asx_codec_equiv_report *report);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CODEC_EQUIVALENCE_H */
