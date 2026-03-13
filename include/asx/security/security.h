/*
 * asx/security/security.h — symbol authentication and security context
 *
 * Provides authentication primitives for the RaptorQ-based distributed
 * layer.  Enables verification of symbol integrity and authenticity
 * during transmission across untrusted networks.
 *
 * Design principles:
 *   1. Determinism-compatible — all operations are deterministic for lab runtime
 *   2. Interface-first — clean function API, swappable implementation
 *   3. No ambient keys — keys must be explicitly provided (capability security)
 *   4. Fail-safe defaults — invalid/missing auth fails closed
 *   5. Zero dynamic allocation — all types are value types, no malloc
 *
 * Phase 0 status:
 *   The current implementation uses a deterministic keyed hash that is NOT
 *   cryptographically secure.  Production deployments MUST use a proper HMAC.
 *
 * Architecture:
 *
 *   SecurityContext
 *   ├── AuthKey (256-bit key material)
 *   │   └── derive_subkey(purpose) → child AuthKey
 *   ├── AuthMode (Strict / Permissive / Disabled)
 *   └── sign / verify
 *       ├── sign_symbol(data, len) → AuthenticationTag
 *       └── verify_symbol(data, len, tag) → status
 *
 *   AuthenticatedSymbol
 *   ├── data pointer + length (borrowed, not owned)
 *   ├── AuthenticationTag (32 bytes)
 *   └── verified flag
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SECURITY_SECURITY_H
#define ASX_SECURITY_SECURITY_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define ASX_AUTH_KEY_SIZE 32u /* 256-bit key */
#define ASX_AUTH_TAG_SIZE 32u /* 256-bit tag */

/* ------------------------------------------------------------------ */
/* AuthKey — 256-bit authentication key                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t bytes[ASX_AUTH_KEY_SIZE];
} asx_auth_key;

/* Create a key from a deterministic 64-bit seed.
 * Seed 0 is remapped to a fixed non-zero value. */
ASX_API void asx_auth_key_from_seed(asx_auth_key *key, uint64_t seed);

/* Create a key from raw bytes. */
ASX_API void asx_auth_key_from_bytes(asx_auth_key *key, const uint8_t bytes[ASX_AUTH_KEY_SIZE]);

/* Derive a subkey for a specific purpose.
 * Construction: derived = keyed_hash(self, purpose, purpose_len).
 * Different purpose strings produce incompatible keys. */
ASX_API void asx_auth_key_derive(asx_auth_key *derived, const asx_auth_key *parent,
                                 const uint8_t *purpose, size_t purpose_len);

/* Returns nonzero if two keys are equal (constant-time). */
ASX_API ASX_MUST_USE int asx_auth_key_equals(const asx_auth_key *a, const asx_auth_key *b);

/* ------------------------------------------------------------------ */
/* AuthenticationTag — 32-byte MAC for symbol verification             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t bytes[ASX_AUTH_TAG_SIZE];
} asx_auth_tag;

/* Compute an authentication tag for data using the given key.
 * Phase 0: deterministic keyed hash (not cryptographically secure).
 * data may be NULL if data_len == 0. */
ASX_API void asx_auth_tag_compute(asx_auth_tag *tag, const asx_auth_key *key, const uint8_t *data,
                                  size_t data_len);

/* Verify that a tag matches the computed tag for the given data+key.
 * Returns nonzero if valid. Uses constant-time comparison. */
ASX_API ASX_MUST_USE int asx_auth_tag_verify(const asx_auth_tag *tag, const asx_auth_key *key,
                                             const uint8_t *data, size_t data_len);

/* Returns a zeroed tag (for testing or placeholders). */
ASX_API void asx_auth_tag_zero(asx_auth_tag *tag);

/* Returns nonzero if two tags are equal (constant-time). */
ASX_API ASX_MUST_USE int asx_auth_tag_equals(const asx_auth_tag *a, const asx_auth_tag *b);

/* ------------------------------------------------------------------ */
/* AuthMode — verification policy                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_AUTH_MODE_STRICT = 0,     /* verification failures are errors */
    ASX_AUTH_MODE_PERMISSIVE = 1, /* failures logged but allowed      */
    ASX_AUTH_MODE_DISABLED = 2    /* verification skipped entirely    */
} asx_auth_mode;

/* ------------------------------------------------------------------ */
/* AuthStats — counters for authentication operations                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t signed_count;     /* symbols signed */
    uint64_t verified_ok;      /* symbols successfully verified */
    uint64_t verified_fail;    /* symbols that failed verification */
    uint64_t failures_allowed; /* failures allowed (permissive mode) */
    uint64_t skipped;          /* verifications skipped (disabled) */
} asx_auth_stats;

/* Reset all counters to zero. */
ASX_API void asx_auth_stats_reset(asx_auth_stats *stats);

/* ------------------------------------------------------------------ */
/* SecurityContext — main entry point for sign/verify operations        */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_auth_key key;
    asx_auth_mode mode;
    asx_auth_stats stats;
} asx_security_context;

/* Initialize a security context with a key and default strict mode. */
ASX_API void asx_security_context_init(asx_security_context *ctx, const asx_auth_key *key);

/* Initialize a security context for testing with a deterministic seed. */
ASX_API void asx_security_context_for_testing(asx_security_context *ctx, uint64_t seed);

/* Set the authentication mode. */
ASX_API void asx_security_context_set_mode(asx_security_context *ctx, asx_auth_mode mode);

/* Sign symbol data, producing an authentication tag. */
ASX_API void asx_security_context_sign(asx_security_context *ctx, const uint8_t *data,
                                       size_t data_len, asx_auth_tag *out_tag);

/* Verify symbol data against a tag.
 * Returns ASX_OK if verification passes (or mode allows).
 * Returns ASX_E_PERMISSION_DENIED in strict mode on failure.
 * Sets *out_verified to nonzero if cryptographically valid. */
ASX_API ASX_MUST_USE asx_status asx_security_context_verify(asx_security_context *ctx,
                                                            const uint8_t *data, size_t data_len,
                                                            const asx_auth_tag *tag,
                                                            int *out_verified);

/* Derive a child context for a specific purpose.
 * Child has independent stats and a derived subkey. */
ASX_API void asx_security_context_derive(asx_security_context *child,
                                         const asx_security_context *parent, const uint8_t *purpose,
                                         size_t purpose_len);

/* Access the stats for this context. Returns a const pointer. */
ASX_API ASX_MUST_USE const asx_auth_stats *asx_security_context_stats(
    const asx_security_context *ctx);

/* ------------------------------------------------------------------ */
/* AuthenticatedSymbol — symbol data bundled with authentication tag    */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data; /* borrowed pointer to symbol payload */
    size_t data_len;     /* length of data */
    asx_auth_tag tag;    /* authentication tag */
    int verified;        /* nonzero if tag has been verified */
} asx_authenticated_symbol;

/* Create a verified authenticated symbol (local signing).
 * Caller must ensure data lifetime exceeds the symbol's. */
ASX_API void asx_authenticated_symbol_sign(asx_authenticated_symbol *sym, asx_security_context *ctx,
                                           const uint8_t *data, size_t data_len);

/* Create an unverified authenticated symbol from received parts.
 * Caller must ensure data lifetime exceeds the symbol's. */
ASX_API void asx_authenticated_symbol_from_parts(asx_authenticated_symbol *sym, const uint8_t *data,
                                                 size_t data_len, const asx_auth_tag *tag);

/* Verify an authenticated symbol using a security context.
 * Returns ASX_OK if verification passes (or mode allows).
 * Sets sym->verified on cryptographic success. */
ASX_API ASX_MUST_USE asx_status asx_authenticated_symbol_verify(asx_authenticated_symbol *sym,
                                                                asx_security_context *ctx);

/* Returns nonzero if the symbol has been verified. */
ASX_API ASX_MUST_USE int asx_authenticated_symbol_is_verified(const asx_authenticated_symbol *sym);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SECURITY_SECURITY_H */
