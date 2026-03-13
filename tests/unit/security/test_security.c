/*
 * test_security.c — unit tests for security context, auth key, tag, and
 *                    authenticated symbol
 *
 * Tests accepted and rejected authority flows, symbol authentication,
 * key derivation isolation, and mode behavior.
 *
 * Bead: bd-1eqo.12.1
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/security/security.h>
#include <string.h>

/* ================================================================== */
/* AuthKey tests                                                       */
/* ================================================================== */

TEST(key_from_seed_deterministic) {
    asx_auth_key k1, k2;
    asx_auth_key_from_seed(&k1, 42);
    asx_auth_key_from_seed(&k2, 42);
    ASSERT_TRUE(asx_auth_key_equals(&k1, &k2));
}

TEST(key_different_seeds_differ) {
    asx_auth_key k1, k2;
    asx_auth_key_from_seed(&k1, 1);
    asx_auth_key_from_seed(&k2, 2);
    ASSERT_TRUE(!asx_auth_key_equals(&k1, &k2));
}

TEST(key_seed_zero_is_distinct) {
    asx_auth_key k0, k1;
    asx_auth_key_from_seed(&k0, 0);
    asx_auth_key_from_seed(&k1, 1);
    ASSERT_TRUE(!asx_auth_key_equals(&k0, &k1));
}

TEST(key_from_bytes_roundtrip) {
    uint8_t raw[ASX_AUTH_KEY_SIZE];
    asx_auth_key key;
    unsigned i;
    for (i = 0; i < ASX_AUTH_KEY_SIZE; i++) raw[i] = (uint8_t)i;
    asx_auth_key_from_bytes(&key, raw);
    ASSERT_TRUE(memcmp(key.bytes, raw, ASX_AUTH_KEY_SIZE) == 0);
}

TEST(key_derive_deterministic) {
    asx_auth_key parent, d1, d2;
    asx_auth_key_from_seed(&parent, 100);
    asx_auth_key_derive(&d1, &parent, (const uint8_t *)"transport", 9);
    asx_auth_key_derive(&d2, &parent, (const uint8_t *)"transport", 9);
    ASSERT_TRUE(asx_auth_key_equals(&d1, &d2));
}

TEST(key_derive_different_purposes_differ) {
    asx_auth_key parent, d1, d2;
    asx_auth_key_from_seed(&parent, 100);
    asx_auth_key_derive(&d1, &parent, (const uint8_t *)"transport", 9);
    asx_auth_key_derive(&d2, &parent, (const uint8_t *)"storage", 7);
    ASSERT_TRUE(!asx_auth_key_equals(&d1, &d2));
}

TEST(key_derived_not_equal_to_parent) {
    asx_auth_key parent, child;
    asx_auth_key_from_seed(&parent, 100);
    asx_auth_key_derive(&child, &parent, (const uint8_t *)"test", 4);
    ASSERT_TRUE(!asx_auth_key_equals(&parent, &child));
}

/* ================================================================== */
/* AuthenticationTag tests                                             */
/* ================================================================== */

TEST(tag_compute_deterministic) {
    asx_auth_key key;
    asx_auth_tag t1, t2;
    uint8_t data[] = {1, 2, 3};
    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&t1, &key, data, sizeof(data));
    asx_auth_tag_compute(&t2, &key, data, sizeof(data));
    ASSERT_TRUE(asx_auth_tag_equals(&t1, &t2));
}

TEST(tag_verify_valid) {
    asx_auth_key key;
    asx_auth_tag tag;
    uint8_t data[] = {1, 2, 3};
    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&tag, &key, data, sizeof(data));
    ASSERT_TRUE(asx_auth_tag_verify(&tag, &key, data, sizeof(data)));
}

TEST(tag_verify_fails_different_data) {
    asx_auth_key key;
    asx_auth_tag tag;
    uint8_t d1[] = {1, 2, 3};
    uint8_t d2[] = {1, 2, 4};
    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&tag, &key, d1, sizeof(d1));
    ASSERT_TRUE(!asx_auth_tag_verify(&tag, &key, d2, sizeof(d2)));
}

TEST(tag_verify_fails_different_key) {
    asx_auth_key k1, k2;
    asx_auth_tag tag;
    uint8_t data[] = {1, 2, 3};
    asx_auth_key_from_seed(&k1, 1);
    asx_auth_key_from_seed(&k2, 2);
    asx_auth_tag_compute(&tag, &k1, data, sizeof(data));
    ASSERT_TRUE(!asx_auth_tag_verify(&tag, &k2, data, sizeof(data)));
}

TEST(tag_zero_fails_verification) {
    asx_auth_key key;
    asx_auth_tag tag;
    uint8_t data[] = {1, 2, 3};
    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_zero(&tag);
    ASSERT_TRUE(!asx_auth_tag_verify(&tag, &key, data, sizeof(data)));
}

TEST(tag_data_dependent) {
    asx_auth_key key;
    asx_auth_tag t_empty, t_full;
    uint8_t full[64];
    asx_auth_key_from_seed(&key, 42);
    memset(full, 0xFF, sizeof(full));
    asx_auth_tag_compute(&t_empty, &key, NULL, 0);
    asx_auth_tag_compute(&t_full, &key, full, sizeof(full));
    ASSERT_TRUE(!asx_auth_tag_equals(&t_empty, &t_full));
}

TEST(tag_single_bit_flip_fails) {
    asx_auth_key key;
    asx_auth_tag good;
    uint8_t data[] = {1, 2, 3, 4, 5};
    unsigned byte_idx, bit_idx;
    asx_auth_key_from_seed(&key, 42);
    asx_auth_tag_compute(&good, &key, data, sizeof(data));

    for (byte_idx = 0; byte_idx < ASX_AUTH_TAG_SIZE; byte_idx++) {
        for (bit_idx = 0; bit_idx < 8u; bit_idx++) {
            asx_auth_tag flipped = good;
            flipped.bytes[byte_idx] ^= (uint8_t)(1u << bit_idx);
            ASSERT_TRUE(!asx_auth_tag_verify(&flipped, &key, data, sizeof(data)));
        }
    }
}

/* ================================================================== */
/* SecurityContext tests                                                */
/* ================================================================== */

TEST(context_sign_and_verify) {
    asx_security_context ctx;
    asx_auth_tag tag;
    int verified = 0;
    uint8_t data[] = {1, 2, 3};
    asx_security_context_for_testing(&ctx, 123);
    asx_security_context_sign(&ctx, data, sizeof(data), &tag);
    ASSERT_EQ(asx_security_context_verify(&ctx, data, sizeof(data), &tag, &verified), ASX_OK);
    ASSERT_TRUE(verified);
    ASSERT_EQ(ctx.stats.signed_count, 1u);
    ASSERT_EQ(ctx.stats.verified_ok, 1u);
}

TEST(context_strict_mode_rejects_bad_tag) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    int verified = 0;
    uint8_t data[] = {1, 2, 3};
    asx_security_context_for_testing(&ctx, 123);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_STRICT);
    asx_auth_tag_zero(&bad_tag);
    ASSERT_EQ(asx_security_context_verify(&ctx, data, sizeof(data), &bad_tag, &verified),
              ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(!verified);
    ASSERT_EQ(ctx.stats.verified_fail, 1u);
}

TEST(context_permissive_mode_allows_bad_tag) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    int verified = 0;
    uint8_t data[] = {1, 2, 3};
    asx_security_context_for_testing(&ctx, 123);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_PERMISSIVE);
    asx_auth_tag_zero(&bad_tag);
    ASSERT_EQ(asx_security_context_verify(&ctx, data, sizeof(data), &bad_tag, &verified), ASX_OK);
    ASSERT_TRUE(!verified);
    ASSERT_EQ(ctx.stats.verified_fail, 1u);
    ASSERT_EQ(ctx.stats.failures_allowed, 1u);
}

TEST(context_permissive_mode_valid_tag_marks_verified) {
    asx_security_context ctx;
    asx_auth_tag tag;
    int verified = 0;
    uint8_t data[] = {5, 6, 7};
    asx_security_context_for_testing(&ctx, 42);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_PERMISSIVE);
    asx_security_context_sign(&ctx, data, sizeof(data), &tag);
    ASSERT_EQ(asx_security_context_verify(&ctx, data, sizeof(data), &tag, &verified), ASX_OK);
    ASSERT_TRUE(verified);
    ASSERT_EQ(ctx.stats.verified_ok, 1u);
    ASSERT_EQ(ctx.stats.failures_allowed, 0u);
}

TEST(context_disabled_mode_skips) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    int verified = 0;
    uint8_t data[] = {1, 2, 3};
    asx_security_context_for_testing(&ctx, 123);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_DISABLED);
    asx_auth_tag_zero(&bad_tag);
    ASSERT_EQ(asx_security_context_verify(&ctx, data, sizeof(data), &bad_tag, &verified), ASX_OK);
    ASSERT_TRUE(!verified); /* disabled does not mark verified */
    ASSERT_EQ(ctx.stats.skipped, 1u);
}

TEST(context_cross_context_isolation) {
    asx_security_context primary, transport_ctx, storage_ctx;
    asx_auth_tag tag;
    int verified = 0;
    uint8_t data[] = {10, 20, 30};

    asx_security_context_for_testing(&primary, 99);
    asx_security_context_derive(&transport_ctx, &primary, (const uint8_t *)"transport", 9);
    asx_security_context_derive(&storage_ctx, &primary, (const uint8_t *)"storage", 7);

    /* Sign with transport context */
    asx_security_context_sign(&transport_ctx, data, sizeof(data), &tag);

    /* Verify under storage context must fail (strict mode) */
    ASSERT_EQ(asx_security_context_verify(&storage_ctx, data, sizeof(data), &tag, &verified),
              ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(!verified);

    /* Verify under same transport context must succeed */
    verified = 0;
    ASSERT_EQ(asx_security_context_verify(&transport_ctx, data, sizeof(data), &tag, &verified),
              ASX_OK);
    ASSERT_TRUE(verified);
}

TEST(context_derived_has_independent_stats) {
    asx_security_context primary, child;
    asx_auth_tag tag;
    uint8_t data[] = {1};

    asx_security_context_for_testing(&primary, 42);
    asx_security_context_derive(&child, &primary, (const uint8_t *)"child", 5);

    asx_security_context_sign(&primary, data, sizeof(data), &tag);
    ASSERT_EQ(asx_security_context_stats(&primary)->signed_count, 1u);
    ASSERT_EQ(asx_security_context_stats(&child)->signed_count, 0u);
}

/* ================================================================== */
/* AuthenticatedSymbol tests                                           */
/* ================================================================== */

TEST(authenticated_symbol_sign_is_verified) {
    asx_security_context ctx;
    asx_authenticated_symbol sym;
    uint8_t data[] = {1, 2, 3};
    asx_security_context_for_testing(&ctx, 123);
    asx_authenticated_symbol_sign(&sym, &ctx, data, sizeof(data));
    ASSERT_TRUE(asx_authenticated_symbol_is_verified(&sym));
    ASSERT_TRUE(sym.data == data);
    ASSERT_EQ(sym.data_len, sizeof(data));
}

TEST(authenticated_symbol_from_parts_unverified) {
    asx_auth_tag tag;
    asx_authenticated_symbol sym;
    uint8_t data[] = {1, 2, 3};
    asx_auth_tag_zero(&tag);
    asx_authenticated_symbol_from_parts(&sym, data, sizeof(data), &tag);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&sym));
}

TEST(authenticated_symbol_verify_roundtrip) {
    asx_security_context ctx;
    asx_authenticated_symbol signed_sym, received;
    uint8_t data[] = {1, 2, 3};

    asx_security_context_for_testing(&ctx, 123);

    /* Sign locally */
    asx_authenticated_symbol_sign(&signed_sym, &ctx, data, sizeof(data));
    ASSERT_TRUE(asx_authenticated_symbol_is_verified(&signed_sym));

    /* Simulate receiving: same data+tag, but unverified */
    asx_authenticated_symbol_from_parts(&received, data, sizeof(data), &signed_sym.tag);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&received));

    /* Verify */
    ASSERT_EQ(asx_authenticated_symbol_verify(&received, &ctx), ASX_OK);
    ASSERT_TRUE(asx_authenticated_symbol_is_verified(&received));
}

TEST(authenticated_symbol_verify_bad_tag_strict) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    asx_authenticated_symbol sym;
    uint8_t data[] = {1, 2, 3};

    asx_security_context_for_testing(&ctx, 123);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_STRICT);
    asx_auth_tag_zero(&bad_tag);
    asx_authenticated_symbol_from_parts(&sym, data, sizeof(data), &bad_tag);

    ASSERT_EQ(asx_authenticated_symbol_verify(&sym, &ctx), ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&sym));
}

TEST(authenticated_symbol_verify_bad_tag_permissive) {
    asx_security_context ctx;
    asx_auth_tag bad_tag;
    asx_authenticated_symbol sym;
    uint8_t data[] = {1, 2, 3};

    asx_security_context_for_testing(&ctx, 123);
    asx_security_context_set_mode(&ctx, ASX_AUTH_MODE_PERMISSIVE);
    asx_auth_tag_zero(&bad_tag);
    asx_authenticated_symbol_from_parts(&sym, data, sizeof(data), &bad_tag);

    ASSERT_EQ(asx_authenticated_symbol_verify(&sym, &ctx), ASX_OK);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&sym));
}

/* ================================================================== */
/* Authority flow integration tests                                    */
/* ================================================================== */

TEST(authority_flow_sign_transmit_verify) {
    /*
     * End-to-end: sender signs, receiver verifies with same key.
     * This is the happy-path authenticated capability flow.
     */
    asx_security_context sender, receiver;
    asx_authenticated_symbol sent, received;
    uint8_t payload[] = "hello authenticated world";

    asx_security_context_for_testing(&sender, 77);
    asx_security_context_for_testing(&receiver, 77);

    /* Sender signs */
    asx_authenticated_symbol_sign(&sent, &sender, payload, sizeof(payload));

    /* Simulate network: receiver gets data + tag */
    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);

    /* Receiver verifies */
    ASSERT_EQ(asx_authenticated_symbol_verify(&received, &receiver), ASX_OK);
    ASSERT_TRUE(asx_authenticated_symbol_is_verified(&received));
}

TEST(authority_flow_wrong_key_rejected) {
    /*
     * End-to-end: sender signs with key A, receiver verifies with key B.
     * This must fail (strict mode) — demonstrates rejected authority flow.
     */
    asx_security_context sender, receiver;
    asx_authenticated_symbol sent, received;
    uint8_t payload[] = "wrong key test";

    asx_security_context_for_testing(&sender, 77);
    asx_security_context_for_testing(&receiver, 88); /* different key */

    asx_authenticated_symbol_sign(&sent, &sender, payload, sizeof(payload));

    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);

    ASSERT_EQ(asx_authenticated_symbol_verify(&received, &receiver), ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&received));
}

TEST(authority_flow_derived_context_isolation) {
    /*
     * Demonstrates that derived contexts with different purposes produce
     * incompatible keys — a tag signed for "transport" cannot be verified
     * under "storage" even though both derive from the same primary key.
     */
    asx_security_context primary, ctx_a, ctx_b;
    asx_authenticated_symbol sent, received;
    uint8_t payload[] = "isolation test";

    asx_security_context_for_testing(&primary, 42);
    asx_security_context_derive(&ctx_a, &primary, (const uint8_t *)"purpose_a", 9);
    asx_security_context_derive(&ctx_b, &primary, (const uint8_t *)"purpose_b", 9);

    asx_authenticated_symbol_sign(&sent, &ctx_a, payload, sizeof(payload));
    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);

    /* Cross-context verification must fail */
    ASSERT_EQ(asx_authenticated_symbol_verify(&received, &ctx_b), ASX_E_PERMISSION_DENIED);

    /* Same-context verification must succeed */
    asx_authenticated_symbol_from_parts(&received, payload, sizeof(payload), &sent.tag);
    ASSERT_EQ(asx_authenticated_symbol_verify(&received, &ctx_a), ASX_OK);
    ASSERT_TRUE(asx_authenticated_symbol_is_verified(&received));
}

TEST(authority_flow_tampered_data_rejected) {
    /*
     * Demonstrates that tampering with the payload after signing is detected.
     */
    asx_security_context ctx;
    asx_authenticated_symbol sym;
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t tampered[] = {0xDE, 0xAD, 0xBE, 0x00}; /* last byte changed */

    asx_security_context_for_testing(&ctx, 42);
    asx_authenticated_symbol_sign(&sym, &ctx, payload, sizeof(payload));

    /* Replace data pointer with tampered copy, keep original tag */
    asx_authenticated_symbol_from_parts(&sym, tampered, sizeof(tampered), &sym.tag);

    ASSERT_EQ(asx_authenticated_symbol_verify(&sym, &ctx), ASX_E_PERMISSION_DENIED);
    ASSERT_TRUE(!asx_authenticated_symbol_is_verified(&sym));
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    /* AuthKey */
    RUN_TEST(key_from_seed_deterministic);
    RUN_TEST(key_different_seeds_differ);
    RUN_TEST(key_seed_zero_is_distinct);
    RUN_TEST(key_from_bytes_roundtrip);
    RUN_TEST(key_derive_deterministic);
    RUN_TEST(key_derive_different_purposes_differ);
    RUN_TEST(key_derived_not_equal_to_parent);

    /* AuthenticationTag */
    RUN_TEST(tag_compute_deterministic);
    RUN_TEST(tag_verify_valid);
    RUN_TEST(tag_verify_fails_different_data);
    RUN_TEST(tag_verify_fails_different_key);
    RUN_TEST(tag_zero_fails_verification);
    RUN_TEST(tag_data_dependent);
    RUN_TEST(tag_single_bit_flip_fails);

    /* SecurityContext */
    RUN_TEST(context_sign_and_verify);
    RUN_TEST(context_strict_mode_rejects_bad_tag);
    RUN_TEST(context_permissive_mode_allows_bad_tag);
    RUN_TEST(context_permissive_mode_valid_tag_marks_verified);
    RUN_TEST(context_disabled_mode_skips);
    RUN_TEST(context_cross_context_isolation);
    RUN_TEST(context_derived_has_independent_stats);

    /* AuthenticatedSymbol */
    RUN_TEST(authenticated_symbol_sign_is_verified);
    RUN_TEST(authenticated_symbol_from_parts_unverified);
    RUN_TEST(authenticated_symbol_verify_roundtrip);
    RUN_TEST(authenticated_symbol_verify_bad_tag_strict);
    RUN_TEST(authenticated_symbol_verify_bad_tag_permissive);

    /* Authority flow integration */
    RUN_TEST(authority_flow_sign_transmit_verify);
    RUN_TEST(authority_flow_wrong_key_rejected);
    RUN_TEST(authority_flow_derived_context_isolation);
    RUN_TEST(authority_flow_tampered_data_rejected);

    TEST_REPORT();
    return test_failures;
}
