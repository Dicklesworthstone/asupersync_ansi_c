/*
 * vignette_security.c — API ergonomics vignette: security and audit flows
 *
 * Exercises: authenticated symbol round-trip, derived-context rejection,
 * and audit/evidence reporting for expected security decisions.
 *
 * bd-yx9r.5.2 — security / audit public-surface validation
 * SPDX-License-Identifier: MIT
 */
/* ASX_CHECKPOINT_WAIVER_FILE("vignette: no kernel loops") */

#include <asx/asx.h>
#include <asx/runtime/diagnostic.h>
#include <stdio.h>
#include <string.h>

static const char *evidence_level_name(asx_evidence_level level) {
    switch (level) {
    case ASX_EVIDENCE_INFO: return "INFO";
    case ASX_EVIDENCE_PASS: return "PASS";
    case ASX_EVIDENCE_WARN: return "WARN";
    case ASX_EVIDENCE_FAIL: return "FAIL";
    default: return "UNKNOWN";
    }
}

static void print_sink(const asx_evidence_sink *sink) {
    uint32_t i;

    for (i = 0; i < sink->count; i++) {
        const asx_evidence_entry *entry = &sink->entries[i];
        printf("  [%s] %s — %s\n", evidence_level_name(entry->level), entry->source,
               entry->message);
    }

    printf("  verdict=%s pass=%u warn=%u fail=%u info=%u\n",
           evidence_level_name(asx_evidence_verdict(sink)), sink->pass_count, sink->warn_count,
           sink->fail_count, sink->info_count);
}

static int scenario_authenticated_roundtrip(void) {
    asx_security_context sender;
    asx_security_context receiver;
    asx_authenticated_symbol sent;
    asx_authenticated_symbol received;
    asx_evidence_sink sink;
    uint8_t payload[] = "operator-visible authenticated flow";
    asx_status st;

    printf("--- scenario: authenticated roundtrip ---\n");

    asx_security_context_for_testing(&sender, 77);
    asx_security_context_for_testing(&receiver, 77);
    asx_evidence_sink_init(&sink);

    asx_authenticated_symbol_sign(&sent, &sender, payload, sizeof(payload));
    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);

    st = asx_authenticated_symbol_verify(&received, &receiver);
    if (st != ASX_OK || !asx_authenticated_symbol_is_verified(&received)) {
        printf("  FAIL: verify returned %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_evidence_record(&sink, "vignette:security:accepted", ASX_EVIDENCE_PASS,
                             "receiver accepted authenticated payload", 77u);
    if (st != ASX_OK) {
        printf("  FAIL: evidence_record returned %s\n", asx_status_str(st));
        return 1;
    }

    print_sink(&sink);
    printf("  PASS: authenticated roundtrip\n");
    return 0;
}

static int scenario_derived_context_rejection(void) {
    asx_security_context primary;
    asx_security_context transport_ctx;
    asx_security_context storage_ctx;
    asx_authenticated_symbol sent;
    asx_authenticated_symbol received;
    asx_evidence_sink sink;
    uint8_t payload[] = "derived context isolation";
    asx_status st;

    printf("--- scenario: derived context rejection ---\n");

    asx_security_context_for_testing(&primary, 42);
    asx_security_context_derive(&transport_ctx, &primary, (const uint8_t *)"transport", 9);
    asx_security_context_derive(&storage_ctx, &primary, (const uint8_t *)"storage", 7);
    asx_evidence_sink_init(&sink);

    asx_authenticated_symbol_sign(&sent, &transport_ctx, payload, sizeof(payload));
    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);

    st = asx_authenticated_symbol_verify(&received, &storage_ctx);
    if (st != ASX_E_PERMISSION_DENIED || asx_authenticated_symbol_is_verified(&received)) {
        printf("  FAIL: expected permission denied, got %s\n", asx_status_str(st));
        return 1;
    }

    st = asx_evidence_record(&sink, "vignette:security:rejected", ASX_EVIDENCE_PASS,
                             "cross-context verification correctly rejected", 42u);
    if (st != ASX_OK) {
        printf("  FAIL: evidence_record returned %s\n", asx_status_str(st));
        return 1;
    }

    print_sink(&sink);
    printf("  PASS: derived context rejection\n");
    return 0;
}

static int scenario_audit_negative_suite(void) {
    asx_evidence_sink sink;
    asx_evidence_level verdict;

    printf("--- scenario: audit negative suite ---\n");

    asx_evidence_sink_init(&sink);
    verdict = asx_audit_run_negative_suite(&sink);

    if (verdict != ASX_EVIDENCE_PASS) {
        printf("  FAIL: audit suite verdict=%s\n", evidence_level_name(verdict));
        return 1;
    }

    print_sink(&sink);
    printf("  PASS: audit negative suite\n");
    return 0;
}

int main(void) {
    int failures = 0;

    printf("=== vignette: security ===\n\n");

    failures += scenario_authenticated_roundtrip();
    failures += scenario_derived_context_rejection();
    failures += scenario_audit_negative_suite();

    printf("\n=== security: %d failures ===\n", failures);
    return failures;
}
