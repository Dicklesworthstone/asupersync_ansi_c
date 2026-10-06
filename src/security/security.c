/*
 * security.c — symbol authentication and security context implementation
 *
 * Keys, subkeys, and tags are built from SHA-256 / HMAC-SHA256
 * (src/security/crypto.c) using the same constructions as the upstream Rust
 * `security` module:
 *
 *   AuthKey::from_seed     SHA-256(FROM_SEED_DOMAIN || u64le(seed))
 *   AuthKey::derive_subkey HMAC-SHA256(key, purpose)
 *   AuthKey::from_hkdf     HKDF-SHA256(salt, ikm, info), L = 32
 *   tag(key, data)         HMAC-SHA256(key, TAG_DOMAIN || u64le(len) || data)
 *
 * Verification always recomputes the HMAC and folds the zero-sentinel check
 * into the constant-time result (no early returns on tag content).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/portable.h>
#include <asx/security/crypto.h>
#include <asx/security/security.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Domain labels                                                       */
/* ------------------------------------------------------------------ */

static const char k_from_seed_domain[] = "asupersync::security::AuthKey::from_seed:v1";
static const char k_tag_domain[] = "asupersync::security::AuthenticationTag::bytes::v1";

/* ------------------------------------------------------------------ */
/* AuthKey                                                             */
/* ------------------------------------------------------------------ */

void asx_auth_key_from_seed(asx_auth_key *key, uint64_t seed) {
    asx_sha256_ctx ctx;
    uint8_t seed_le[8];

    if (key == NULL) return;
    asx_store_le_u64(seed_le, seed);
    asx_sha256_init(&ctx);
    asx_sha256_update(&ctx, k_from_seed_domain, sizeof(k_from_seed_domain) - 1u);
    asx_sha256_update(&ctx, seed_le, sizeof(seed_le));
    asx_sha256_final(&ctx, key->bytes);
}

void asx_auth_key_from_bytes(asx_auth_key *key, const uint8_t bytes[ASX_AUTH_KEY_SIZE]) {
    if (key == NULL || bytes == NULL) return;
    memcpy(key->bytes, bytes, ASX_AUTH_KEY_SIZE);
}

asx_status asx_auth_key_from_hkdf(asx_auth_key *key, const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *salt, size_t salt_len, const uint8_t *info,
                                  size_t info_len) {
    uint8_t okm[ASX_AUTH_KEY_SIZE];
    asx_status st;

    if (key == NULL) return ASX_E_INVALID_ARGUMENT;
    if (salt == NULL && salt_len > 0u) return ASX_E_INVALID_ARGUMENT;
    st = asx_hkdf_sha256(salt, salt_len, ikm, ikm_len, info, info_len, okm, sizeof(okm));
    if (st != ASX_OK) return st;
    memcpy(key->bytes, okm, sizeof(okm));
    asx_crypto_secure_zero(okm, sizeof(okm));
    return ASX_OK;
}

void asx_auth_key_derive(asx_auth_key *derived, const asx_auth_key *parent, const uint8_t *purpose,
                         size_t purpose_len) {
    uint8_t out[ASX_AUTH_KEY_SIZE];

    if (derived == NULL || parent == NULL) return;
    if (purpose == NULL) purpose_len = 0u;
    /* Compute into a temporary so derived may alias parent. */
    asx_hmac_sha256(parent->bytes, ASX_AUTH_KEY_SIZE, purpose, purpose_len, out);
    memcpy(derived->bytes, out, sizeof(out));
    asx_crypto_secure_zero(out, sizeof(out));
}

int asx_auth_key_equals(const asx_auth_key *a, const asx_auth_key *b) {
    if (a == NULL || b == NULL) return 0;
    return asx_crypto_ct_equal(a->bytes, b->bytes, ASX_AUTH_KEY_SIZE);
}

void asx_auth_key_wipe(asx_auth_key *key) {
    if (key == NULL) return;
    asx_crypto_secure_zero(key->bytes, ASX_AUTH_KEY_SIZE);
}

/* ------------------------------------------------------------------ */
/* AuthenticationTag                                                   */
/* ------------------------------------------------------------------ */

void asx_auth_tag_compute(asx_auth_tag *tag, const asx_auth_key *key, const uint8_t *data,
                          size_t data_len) {
    asx_hmac_sha256_ctx mac;
    uint8_t len_le[8];

    if (tag == NULL || key == NULL) return;
    if (data == NULL) data_len = 0u;
    asx_store_le_u64(len_le, (uint64_t)data_len);

    asx_hmac_sha256_init(&mac, key->bytes, ASX_AUTH_KEY_SIZE);
    asx_hmac_sha256_update(&mac, k_tag_domain, sizeof(k_tag_domain) - 1u);
    asx_hmac_sha256_update(&mac, len_le, sizeof(len_le));
    asx_hmac_sha256_update(&mac, data, data_len);
    asx_hmac_sha256_final(&mac, tag->bytes);
}

int asx_auth_tag_verify(const asx_auth_tag *tag, const asx_auth_key *key, const uint8_t *data,
                        size_t data_len) {
    asx_auth_tag computed;
    int matches;
    int is_zero;

    if (tag == NULL || key == NULL) return 0;
    asx_auth_tag_compute(&computed, key, data, data_len);
    matches = asx_crypto_ct_equal(tag->bytes, computed.bytes, ASX_AUTH_TAG_SIZE);
    is_zero = asx_crypto_ct_is_zero(tag->bytes, ASX_AUTH_TAG_SIZE);
    asx_crypto_secure_zero(&computed, sizeof(computed));
    /* Both are 0/1 values: combine without branching on tag content. */
    return matches & (is_zero ^ 1);
}

void asx_auth_tag_zero(asx_auth_tag *tag) {
    if (tag == NULL) return;
    memset(tag->bytes, 0, ASX_AUTH_TAG_SIZE);
}

int asx_auth_tag_equals(const asx_auth_tag *a, const asx_auth_tag *b) {
    if (a == NULL || b == NULL) return 0;
    return asx_crypto_ct_equal(a->bytes, b->bytes, ASX_AUTH_TAG_SIZE);
}

/* ------------------------------------------------------------------ */
/* AuthStats                                                           */
/* ------------------------------------------------------------------ */

void asx_auth_stats_reset(asx_auth_stats *stats) { memset(stats, 0, sizeof(*stats)); }

/* ------------------------------------------------------------------ */
/* SecurityContext                                                     */
/* ------------------------------------------------------------------ */

void asx_security_context_init(asx_security_context *ctx, const asx_auth_key *key) {
    ctx->key = *key;
    ctx->mode = ASX_AUTH_MODE_STRICT;
    asx_auth_stats_reset(&ctx->stats);
}

void asx_security_context_for_testing(asx_security_context *ctx, uint64_t seed) {
    asx_auth_key key;
    asx_auth_key_from_seed(&key, seed);
    asx_security_context_init(ctx, &key);
    asx_auth_key_wipe(&key);
}

void asx_security_context_set_mode(asx_security_context *ctx, asx_auth_mode mode) {
    ctx->mode = mode;
}

void asx_security_context_sign(asx_security_context *ctx, const uint8_t *data, size_t data_len,
                               asx_auth_tag *out_tag) {
    asx_auth_tag_compute(out_tag, &ctx->key, data, data_len);
    ctx->stats.signed_count++;
}

asx_status asx_security_context_verify(asx_security_context *ctx, const uint8_t *data,
                                       size_t data_len, const asx_auth_tag *tag,
                                       int *out_verified) {
    int valid;

    if (out_verified) *out_verified = 0;

    if (ctx->mode == ASX_AUTH_MODE_DISABLED) {
        ctx->stats.skipped++;
        return ASX_OK;
    }

    valid = asx_auth_tag_verify(tag, &ctx->key, data, data_len);

    if (valid) {
        if (out_verified) *out_verified = 1;
        ctx->stats.verified_ok++;
        return ASX_OK;
    }

    ctx->stats.verified_fail++;

    if (ctx->mode == ASX_AUTH_MODE_STRICT) { return ASX_E_PERMISSION_DENIED; }

    /* Permissive: allow the failure */
    ctx->stats.failures_allowed++;
    return ASX_OK;
}

void asx_security_context_derive(asx_security_context *child, const asx_security_context *parent,
                                 const uint8_t *purpose, size_t purpose_len) {
    asx_auth_key_derive(&child->key, &parent->key, purpose, purpose_len);
    child->mode = parent->mode;
    asx_auth_stats_reset(&child->stats);
}

const asx_auth_stats *asx_security_context_stats(const asx_security_context *ctx) {
    return &ctx->stats;
}

/* ------------------------------------------------------------------ */
/* AuthenticatedSymbol                                                 */
/* ------------------------------------------------------------------ */

void asx_authenticated_symbol_sign(asx_authenticated_symbol *sym, asx_security_context *ctx,
                                   const uint8_t *data, size_t data_len) {
    sym->data = data;
    sym->data_len = data_len;
    sym->verified = 1;
    asx_security_context_sign(ctx, data, data_len, &sym->tag);
}

void asx_authenticated_symbol_from_parts(asx_authenticated_symbol *sym, const uint8_t *data,
                                         size_t data_len, const asx_auth_tag *tag) {
    sym->data = data;
    sym->data_len = data_len;
    sym->tag = *tag;
    sym->verified = 0;
}

asx_status asx_authenticated_symbol_verify(asx_authenticated_symbol *sym,
                                           asx_security_context *ctx) {
    int verified = 0;
    asx_status s = asx_security_context_verify(ctx, sym->data, sym->data_len, &sym->tag, &verified);
    if (verified) { sym->verified = 1; }
    return s;
}

int asx_authenticated_symbol_is_verified(const asx_authenticated_symbol *sym) {
    return sym->verified;
}
