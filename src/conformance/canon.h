/*
 * conformance/canon.h — canonical form of vocabulary v2 runs
 * (docs/CANONICAL_VOCABULARY_V2.md §8-9): event footprints, the
 * independence relation, Foata layering and sha256 digests.
 *
 * The C twin of tools/twin_run/src/canon.rs: both engines' runs are put in
 * this form before they are compared, so the two implementations must agree
 * byte for byte. tests/unit/conformance/test_canon.c checks this one
 * against the same normative vectors canon.rs is tested with.
 *
 * Not part of the public API: the conformance tools and tests only.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CONFORMANCE_CANON_H
#define ASX_CONFORMANCE_CANON_H

#include "json.h"

/* "sha256:" + 64 lowercase hex digits + NUL. */
#define ASX_CANON_DIGEST_LEN 72u

/* Bounds of one canonicalization. Exceeding either is an error. */
#ifndef ASX_CANON_MAX_EVENTS
#define ASX_CANON_MAX_EVENTS 8192u
#endif
#ifndef ASX_CANON_MAX_RESOURCES
#define ASX_CANON_MAX_RESOURCES 4096u
#endif

/* Diagnostic for the last failed call (static string), or NULL. */
const char *asx_canon_error(void);

/* Whether events `a` and `b` (vocabulary objects in `doc`) commute.
 * Returns ASX_E_INVALID_ARGUMENT for an unknown kind or a missing field. */
ASX_MUST_USE asx_status asx_canon_independent(const asx_json_doc *doc, uint32_t a, uint32_t b,
                                              int *out_independent);

/* Foata canonical form of the emission-ordered event array `events` in
 * `src`: each event is layered by the single-pass rule, then each layer is
 * sorted by the canonical bytes of its events. The layers are built in
 * `dst` (which may be `src`) as an array of arrays. */
ASX_MUST_USE asx_status asx_canon_trace(const asx_json_doc *src, uint32_t events, asx_json_doc *dst,
                                        uint32_t *out_layers);

/* "sha256:<hex>" of the canonical bytes of `node`. */
ASX_MUST_USE asx_status asx_canon_digest(const asx_json_doc *doc, uint32_t node,
                                         char out[ASX_CANON_DIGEST_LEN]);

#endif /* ASX_CONFORMANCE_CANON_H */
