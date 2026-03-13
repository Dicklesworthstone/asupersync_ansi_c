/*
 * asx/security/audit.h — ambient-authority audit and security regression
 *
 * Provides runtime audit facilities for proving that the C port refuses
 * forbidden authority patterns.  Integrates with the evidence sink so
 * that audit results are structured and consumable by CI and diagnostics.
 *
 * Design:
 *   - Known ambient findings: a static catalog of intentional ambient
 *     authority uses with file, line, category, and exemption status.
 *   - Audit runner: walks the catalog and records pass/fail to an
 *     evidence sink.
 *   - Authority violation ceiling: max tolerated ambient authority count,
 *     fails if new violations appear without catalog updates.
 *   - Negative-test helpers: composable checks for must-fail scenarios.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SECURITY_AUDIT_H
#define ASX_SECURITY_AUDIT_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/diagnostic.h>
#include <asx/security/security.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Ambient authority categories                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_AMBIENT_TIME = 0,    /* direct clock access without Cx */
    ASX_AMBIENT_SPAWN = 1,   /* direct thread/task creation    */
    ASX_AMBIENT_ENTROPY = 2, /* direct RNG without capability  */
    ASX_AMBIENT_IO = 3       /* direct IO without capability   */
} asx_ambient_category;

/* ------------------------------------------------------------------ */
/* Ambient finding — one cataloged ambient authority use               */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_AMBIENT_INFO = 0,
    ASX_AMBIENT_WARNING = 1,
    ASX_AMBIENT_CRITICAL = 2
} asx_ambient_severity;

typedef struct {
    const char *file;              /* source file path */
    uint32_t line;                 /* source line number */
    asx_ambient_category category; /* authority type */
    asx_ambient_severity severity; /* severity level */
    int exempted;                  /* nonzero if intentionally exempted */
    const char *justification;     /* why this is exempted (or NULL) */
    const char *evidence;          /* grep pattern to verify existence */
} asx_ambient_finding;

/* ------------------------------------------------------------------ */
/* Known findings catalog                                              */
/* ------------------------------------------------------------------ */

/* Get the known ambient findings catalog.
 * Returns pointer to static array, sets *out_count to its length. */
ASX_API const asx_ambient_finding *asx_ambient_known_findings(size_t *out_count);

/* Maximum tolerated ambient authority count.  The audit fails if the
 * actual count exceeds this ceiling, forcing catalog updates. */
ASX_API size_t asx_ambient_violation_ceiling(void);

/* ------------------------------------------------------------------ */
/* Audit runner                                                        */
/* ------------------------------------------------------------------ */

/* Audit result summary */
typedef struct {
    uint32_t total_findings;   /* catalog entries examined */
    uint32_t exempted_count;   /* entries with valid exemptions */
    uint32_t unexempted_count; /* entries without exemptions */
    uint32_t ceiling;          /* violation ceiling */
    int ceiling_exceeded;      /* nonzero if unexempted > ceiling */
} asx_audit_summary;

/* Run the ambient authority audit, recording results to an evidence sink.
 * Returns the audit summary.  Sets evidence entries for each finding and
 * an overall verdict entry. */
ASX_API asx_audit_summary asx_ambient_audit(asx_evidence_sink *sink);

/* ------------------------------------------------------------------ */
/* Negative-test check helpers                                         */
/* ------------------------------------------------------------------ */

/* Check: strict mode rejects a forged tag.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_forged_tag_rejected(asx_evidence_sink *sink, uint64_t seed);

/* Check: cross-context verification is denied.
 * Signs with context derived for purpose_a, verifies with purpose_b.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_cross_context_denied(asx_evidence_sink *sink, uint64_t seed,
                                                 const char *purpose_a, const char *purpose_b);

/* Check: data tampering is detected.
 * Signs data, flips one byte, verifies must fail.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_tamper_detected(asx_evidence_sink *sink, uint64_t seed);

/* Check: unverified symbol cannot claim verified status.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_unverified_symbol_not_verified(asx_evidence_sink *sink);

/* Check: disabled mode does not mark symbols as verified.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_disabled_not_verified(asx_evidence_sink *sink, uint64_t seed);

/* Check: permissive mode allows but does NOT mark as verified.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_permissive_no_false_positive(asx_evidence_sink *sink, uint64_t seed);

/* Check: wrong-key verification fails in strict mode.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_wrong_key_rejected(asx_evidence_sink *sink, uint64_t seed_a,
                                               uint64_t seed_b);

/* Check: zero-tag never verifies for any data.
 * Records PASS/FAIL to sink.  Returns nonzero if check passed. */
ASX_API int asx_audit_check_zero_tag_rejected(asx_evidence_sink *sink, uint64_t seed);

/* Run ALL negative-test checks and return the evidence verdict.
 * This is the entry point for the full security regression suite. */
ASX_API asx_evidence_level asx_audit_run_negative_suite(asx_evidence_sink *sink);

/* ------------------------------------------------------------------ */
/* Pristine module check                                               */
/* ------------------------------------------------------------------ */

/* List of module paths that MUST have zero ambient authority.
 * Returns pointer to static array, sets *out_count to its length. */
ASX_API const char *const *asx_audit_pristine_modules(size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SECURITY_AUDIT_H */
