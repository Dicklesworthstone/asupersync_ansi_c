/*
 * security.c — symbol authentication and security context implementation
 *
 * Phase 0: deterministic keyed hash.  NOT cryptographically secure.
 * The mixing function matches the Rust reference implementation's Phase 0
 * tag computation for cross-language determinism.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/security/security.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* xorshift64* for deterministic key expansion (matches Rust DetRng) */
static uint64_t xorshift64_star(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(0x2545F4914F6CDD1D);
}

/* Fill a byte buffer from a xorshift64* state */
static void det_fill_bytes(uint64_t *state, uint8_t *buf, size_t len) {
    size_t i;
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t v = xorshift64_star(state);
        memcpy(buf + i, &v, 8);
    }
    if (i < len) {
        uint64_t v = xorshift64_star(state);
        memcpy(buf + i, &v, len - i);
    }
}

/* Constant-time byte comparison */
static int constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < len; i++) { diff |= a[i] ^ b[i]; }
    return diff == 0;
}

/* Rotate left for uint8_t */
static uint8_t rotate_left_u8(uint8_t v, unsigned n) {
    n &= 7u;
    return (uint8_t)((v << n) | (v >> (8u - n)));
}

/* ------------------------------------------------------------------ */
/* AuthKey                                                             */
/* ------------------------------------------------------------------ */

void asx_auth_key_from_seed(asx_auth_key *key, uint64_t seed) {
    uint64_t state;
    if (seed == 0) {
        state = UINT64_C(0x9E3779B97F4A7C15);
    } else {
        state = seed;
    }
    det_fill_bytes(&state, key->bytes, ASX_AUTH_KEY_SIZE);
}

void asx_auth_key_from_bytes(asx_auth_key *key, const uint8_t bytes[ASX_AUTH_KEY_SIZE]) {
    memcpy(key->bytes, bytes, ASX_AUTH_KEY_SIZE);
}

void asx_auth_key_derive(asx_auth_key *derived, const asx_auth_key *parent, const uint8_t *purpose,
                         size_t purpose_len) {
    /*
     * Phase 0 subkey derivation: keyed mixing of parent key with purpose.
     * This is NOT HMAC-SHA256 — it uses a deterministic mix that maintains
     * cross-language compatibility with the Rust Phase 0 implementation.
     *
     * Construction: mix parent key bytes with purpose bytes using XOR + rotate.
     */
    uint8_t mixed[ASX_AUTH_KEY_SIZE];
    size_t i;

    memcpy(mixed, parent->bytes, ASX_AUTH_KEY_SIZE);

    /* Mix purpose bytes into the key */
    for (i = 0; i < purpose_len; i++) {
        size_t idx = i % ASX_AUTH_KEY_SIZE;
        mixed[idx] ^= purpose[i];
        mixed[idx] = rotate_left_u8(mixed[idx], 3);
        mixed[(idx + 1) % ASX_AUTH_KEY_SIZE] =
            (uint8_t)(mixed[(idx + 1) % ASX_AUTH_KEY_SIZE] + purpose[i]);
    }

    /* Avalanche pass */
    for (i = 0; i < ASX_AUTH_KEY_SIZE; i++) {
        mixed[i] = (uint8_t)(mixed[i] + mixed[(i + 1) % ASX_AUTH_KEY_SIZE]);
        mixed[i] ^= parent->bytes[i];
    }

    /* Expand through PRNG for better diffusion */
    {
        uint64_t state = 0;
        memcpy(&state, mixed, sizeof(state));
        if (state == 0) state = UINT64_C(0x9E3779B97F4A7C15);
        det_fill_bytes(&state, derived->bytes, ASX_AUTH_KEY_SIZE);
    }
}

int asx_auth_key_equals(const asx_auth_key *a, const asx_auth_key *b) {
    return constant_time_eq(a->bytes, b->bytes, ASX_AUTH_KEY_SIZE);
}

/* ------------------------------------------------------------------ */
/* AuthenticationTag                                                   */
/* ------------------------------------------------------------------ */

void asx_auth_tag_compute(asx_auth_tag *tag, const asx_auth_key *key, const uint8_t *data,
                          size_t data_len) {
    size_t i;

    /* Initialize tag with key */
    memcpy(tag->bytes, key->bytes, ASX_AUTH_TAG_SIZE);

    /* Mix data */
    for (i = 0; i < data_len; i++) {
        size_t idx = i % ASX_AUTH_TAG_SIZE;
        tag->bytes[idx] = rotate_left_u8((uint8_t)(tag->bytes[idx] + data[i]), 3);
        tag->bytes[idx] ^= key->bytes[(i + 5) % ASX_AUTH_KEY_SIZE];
    }

    /* Final avalanche */
    for (i = 0; i < ASX_AUTH_TAG_SIZE; i++) {
        tag->bytes[i] = (uint8_t)(tag->bytes[i] + tag->bytes[(i + 1) % ASX_AUTH_TAG_SIZE]);
        tag->bytes[i] ^= key->bytes[i % ASX_AUTH_KEY_SIZE];
    }
}

int asx_auth_tag_verify(const asx_auth_tag *tag, const asx_auth_key *key, const uint8_t *data,
                        size_t data_len) {
    asx_auth_tag computed;
    asx_auth_tag_compute(&computed, key, data, data_len);
    return constant_time_eq(tag->bytes, computed.bytes, ASX_AUTH_TAG_SIZE);
}

void asx_auth_tag_zero(asx_auth_tag *tag) { memset(tag->bytes, 0, ASX_AUTH_TAG_SIZE); }

int asx_auth_tag_equals(const asx_auth_tag *a, const asx_auth_tag *b) {
    return constant_time_eq(a->bytes, b->bytes, ASX_AUTH_TAG_SIZE);
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
