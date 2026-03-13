/*
 * audit.c — ambient-authority audit and security negative-test runner
 *
 * Implements the ambient authority catalog, audit runner, and composable
 * negative-test checks that prove the C port refuses forbidden patterns.
 *
 * Bead: bd-1eqo.12.2
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/security/audit.h>
#include <string.h>

/* Suppress warn_unused_result for best-effort evidence recording. */
#define RECORD(sink, src, lvl, msg, eid)                                                           \
    do {                                                                                           \
        asx_status rc_ = asx_evidence_record((sink), (src), (lvl), (msg), (eid));                  \
        (void)rc_;                                                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* Known ambient findings catalog                                      */
/* ------------------------------------------------------------------ */

/*
 * Each entry documents one intentional use of ambient authority.
 * Provider modules (e.g., time driver, entropy source) ARE the
 * capability boundary, so their ambient access is exempted.
 *
 * Non-provider modules must NOT appear here; their ambient access
 * is a violation.
 */
static const asx_ambient_finding s_known_findings[] = {
    /* Time driver — IS the clock provider */
    {"src/runtime/time.c", 0, ASX_AMBIENT_TIME, ASX_AMBIENT_INFO, 1,
     "time driver is the clock provider", "clock_gettime"},
    /* Entropy provider — IS the RNG boundary */
    {"src/runtime/entropy.c", 0, ASX_AMBIENT_ENTROPY, ASX_AMBIENT_INFO, 1,
     "entropy module is the RNG provider", "getrandom"}};

#define KNOWN_COUNT (sizeof(s_known_findings) / sizeof(s_known_findings[0]))

/*
 * Violation ceiling: the number of non-exempted ambient authority
 * uses we tolerate.  If new ambient accesses appear in non-provider
 * code, the ceiling must be explicitly raised (and justified).
 */
#define AMBIENT_VIOLATION_CEILING 0u

const asx_ambient_finding *asx_ambient_known_findings(size_t *out_count) {
    *out_count = KNOWN_COUNT;
    return s_known_findings;
}

size_t asx_ambient_violation_ceiling(void) { return AMBIENT_VIOLATION_CEILING; }

/* ------------------------------------------------------------------ */
/* Pristine module list                                                 */
/* ------------------------------------------------------------------ */

static const char *s_pristine_modules[] = {"src/core/channel.c",      "src/core/obligation.c",
                                           "src/core/region.c",       "src/core/task.c",
                                           "src/security/security.c", "src/stream/stream.c"};

#define PRISTINE_COUNT (sizeof(s_pristine_modules) / sizeof(s_pristine_modules[0]))

const char *const *asx_audit_pristine_modules(size_t *out_count) {
    *out_count = PRISTINE_COUNT;
    return s_pristine_modules;
}

/* ------------------------------------------------------------------ */
/* Audit runner                                                        */
/* ------------------------------------------------------------------ */

asx_audit_summary asx_ambient_audit(asx_evidence_sink *sink) {
    asx_audit_summary summary;
    size_t count = 0;
    size_t i;
    const asx_ambient_finding *catalog = asx_ambient_known_findings(&count);

    memset(&summary, 0, sizeof(summary));
    summary.total_findings = (uint32_t)count;
    summary.ceiling = (uint32_t)asx_ambient_violation_ceiling();

    for (i = 0; i < count; i++) {
        const asx_ambient_finding *f = &catalog[i];
        if (f->exempted) {
            summary.exempted_count++;
            RECORD(sink, "audit:ambient", ASX_EVIDENCE_PASS,
                   f->justification ? f->justification : "exempted", (uint64_t)i);
        } else {
            summary.unexempted_count++;
            RECORD(sink, "audit:ambient",
                   f->severity == ASX_AMBIENT_CRITICAL ? ASX_EVIDENCE_FAIL : ASX_EVIDENCE_WARN,
                   f->file, (uint64_t)i);
        }
    }

    summary.ceiling_exceeded = (summary.unexempted_count > summary.ceiling) ? 1 : 0;

    if (summary.ceiling_exceeded) {
        RECORD(sink, "audit:ambient:verdict", ASX_EVIDENCE_FAIL,
               "ambient authority ceiling exceeded", 0);
    } else {
        RECORD(sink, "audit:ambient:verdict", ASX_EVIDENCE_PASS, "ambient authority within ceiling",
               0);
    }

    return summary;
}

/* ------------------------------------------------------------------ */
/* Negative-test check helpers                                         */
/* ------------------------------------------------------------------ */

int asx_audit_check_forged_tag_rejected(asx_evidence_sink *sink, uint64_t seed) {
    asx_security_context ctx;
    asx_auth_tag forged;
    int verified = 0;
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    asx_status st;

    asx_security_context_for_testing(&ctx, seed);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_STRICT);
    asx_auth_tag_zero(&forged);

    st = asx_security_context_verify(&ctx, data, sizeof(data), &forged, &verified);

    if (st == ASX_E_PERMISSION_DENIED && !verified) {
        RECORD(sink, "audit:neg:forged-tag", ASX_EVIDENCE_PASS, "strict mode rejects forged tag",
               seed);
        return 1;
    }

    RECORD(sink, "audit:neg:forged-tag", ASX_EVIDENCE_FAIL, "strict mode did NOT reject forged tag",
           seed);
    return 0;
}

int asx_audit_check_cross_context_denied(asx_evidence_sink *sink, uint64_t seed,
                                         const char *purpose_a, const char *purpose_b) {
    asx_security_context primary, ctx_a, ctx_b;
    asx_auth_tag tag;
    int verified = 0;
    uint8_t data[] = {1, 2, 3, 4};
    asx_status st;

    asx_security_context_for_testing(&primary, seed);
    asx_security_context_derive(&ctx_a, &primary, (const uint8_t *)purpose_a, strlen(purpose_a));
    asx_security_context_derive(&ctx_b, &primary, (const uint8_t *)purpose_b, strlen(purpose_b));

    asx_security_context_sign(&ctx_a, data, sizeof(data), &tag);

    st = asx_security_context_verify(&ctx_b, data, sizeof(data), &tag, &verified);

    if (st == ASX_E_PERMISSION_DENIED && !verified) {
        RECORD(sink, "audit:neg:cross-context", ASX_EVIDENCE_PASS,
               "cross-context verification denied", seed);
        return 1;
    }

    RECORD(sink, "audit:neg:cross-context", ASX_EVIDENCE_FAIL,
           "cross-context verification NOT denied", seed);
    return 0;
}

int asx_audit_check_tamper_detected(asx_evidence_sink *sink, uint64_t seed) {
    asx_security_context ctx;
    asx_authenticated_symbol sym;
    uint8_t original[] = {0xCA, 0xFE, 0xBA, 0xBE};
    uint8_t tampered[] = {0xCA, 0xFE, 0xBA, 0x00};
    asx_status st;

    asx_security_context_for_testing(&ctx, seed);
    asx_authenticated_symbol_sign(&sym, &ctx, original, sizeof(original));

    asx_authenticated_symbol_from_parts(&sym, tampered, sizeof(tampered), &sym.tag);

    st = asx_authenticated_symbol_verify(&sym, &ctx);

    if (st == ASX_E_PERMISSION_DENIED && !asx_authenticated_symbol_is_verified(&sym)) {
        RECORD(sink, "audit:neg:tamper", ASX_EVIDENCE_PASS, "data tampering detected", seed);
        return 1;
    }

    RECORD(sink, "audit:neg:tamper", ASX_EVIDENCE_FAIL, "data tampering NOT detected", seed);
    return 0;
}

int asx_audit_check_unverified_symbol_not_verified(asx_evidence_sink *sink) {
    asx_auth_tag tag;
    asx_authenticated_symbol sym;
    uint8_t data[] = {1, 2, 3};

    asx_auth_tag_zero(&tag);
    asx_authenticated_symbol_from_parts(&sym, data, sizeof(data), &tag);

    if (!asx_authenticated_symbol_is_verified(&sym)) {
        RECORD(sink, "audit:neg:unverified", ASX_EVIDENCE_PASS,
               "from_parts symbol correctly unverified", 0);
        return 1;
    }

    RECORD(sink, "audit:neg:unverified", ASX_EVIDENCE_FAIL,
           "from_parts symbol falsely claims verified", 0);
    return 0;
}

int asx_audit_check_disabled_not_verified(asx_evidence_sink *sink, uint64_t seed) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    int verified = 0;
    uint8_t data[] = {1, 2, 3};
    asx_status st;

    asx_security_context_for_testing(&ctx, seed);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_DISABLED);
    asx_auth_tag_zero(&bad_tag);

    st = asx_security_context_verify(&ctx, data, sizeof(data), &bad_tag, &verified);
    (void)st;

    if (!verified) {
        RECORD(sink, "audit:neg:disabled-not-verified", ASX_EVIDENCE_PASS,
               "disabled mode does not mark verified", seed);
        return 1;
    }

    RECORD(sink, "audit:neg:disabled-not-verified", ASX_EVIDENCE_FAIL,
           "disabled mode falsely marked verified", seed);
    return 0;
}

int asx_audit_check_permissive_no_false_positive(asx_evidence_sink *sink, uint64_t seed) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    int verified = 0;
    uint8_t data[] = {5, 6, 7};
    asx_status st;

    asx_security_context_for_testing(&ctx, seed);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_PERMISSIVE);
    asx_auth_tag_zero(&bad_tag);

    st = asx_security_context_verify(&ctx, data, sizeof(data), &bad_tag, &verified);

    if (st == ASX_OK && !verified && ctx.stats.failures_allowed == 1) {
        RECORD(sink, "audit:neg:permissive-no-fp", ASX_EVIDENCE_PASS,
               "permissive mode allows but does not verify", seed);
        return 1;
    }

    RECORD(sink, "audit:neg:permissive-no-fp", ASX_EVIDENCE_FAIL,
           "permissive mode behavior incorrect", seed);
    return 0;
}

int asx_audit_check_wrong_key_rejected(asx_evidence_sink *sink, uint64_t seed_a, uint64_t seed_b) {
    asx_security_context sender, receiver;
    asx_authenticated_symbol sent, received;
    uint8_t payload[] = "wrong key audit check";
    asx_status st;

    asx_security_context_for_testing(&sender, seed_a);
    asx_security_context_for_testing(&receiver, seed_b);

    asx_authenticated_symbol_sign(&sent, &sender, payload, sizeof(payload));

    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);

    st = asx_authenticated_symbol_verify(&received, &receiver);

    if (st == ASX_E_PERMISSION_DENIED && !asx_authenticated_symbol_is_verified(&received)) {
        RECORD(sink, "audit:neg:wrong-key", ASX_EVIDENCE_PASS, "wrong key correctly rejected",
               seed_a ^ seed_b);
        return 1;
    }

    RECORD(sink, "audit:neg:wrong-key", ASX_EVIDENCE_FAIL, "wrong key NOT rejected",
           seed_a ^ seed_b);
    return 0;
}

int asx_audit_check_zero_tag_rejected(asx_evidence_sink *sink, uint64_t seed) {
    asx_security_context ctx;
    asx_auth_key key;
    asx_auth_tag zero_tag;
    int verified = 0;
    uint8_t data[] = {0xFF, 0xFE, 0xFD};
    asx_status st;

    asx_auth_key_from_seed(&key, seed);
    asx_auth_tag_zero(&zero_tag);

    if (!asx_auth_tag_verify(&zero_tag, &key, data, sizeof(data))) {
        asx_security_context_for_testing(&ctx, seed);
        st = asx_security_context_verify(&ctx, data, sizeof(data), &zero_tag, &verified);
        (void)st;

        if (!verified) {
            RECORD(sink, "audit:neg:zero-tag", ASX_EVIDENCE_PASS,
                   "zero tag rejected at both layers", seed);
            return 1;
        }
    }

    RECORD(sink, "audit:neg:zero-tag", ASX_EVIDENCE_FAIL, "zero tag accepted", seed);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Full negative-test suite runner                                     */
/* ------------------------------------------------------------------ */

asx_evidence_level asx_audit_run_negative_suite(asx_evidence_sink *sink) {
    asx_audit_check_forged_tag_rejected(sink, 42);
    asx_audit_check_forged_tag_rejected(sink, 0);
    asx_audit_check_forged_tag_rejected(sink, UINT64_MAX);

    asx_audit_check_cross_context_denied(sink, 100, "transport", "storage");
    asx_audit_check_cross_context_denied(sink, 100, "sign", "verify");
    asx_audit_check_cross_context_denied(sink, 100, "alpha", "beta");

    asx_audit_check_tamper_detected(sink, 42);
    asx_audit_check_tamper_detected(sink, 0);

    asx_audit_check_unverified_symbol_not_verified(sink);

    asx_audit_check_disabled_not_verified(sink, 42);
    asx_audit_check_disabled_not_verified(sink, 0);

    asx_audit_check_permissive_no_false_positive(sink, 42);
    asx_audit_check_permissive_no_false_positive(sink, 99);

    asx_audit_check_wrong_key_rejected(sink, 1, 2);
    asx_audit_check_wrong_key_rejected(sink, 0, UINT64_MAX);

    asx_audit_check_zero_tag_rejected(sink, 42);
    asx_audit_check_zero_tag_rejected(sink, 0);

    return asx_evidence_verdict(sink);
}
