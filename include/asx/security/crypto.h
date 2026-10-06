/*
 * asx/security/crypto.h — portable cryptographic primitives
 *
 * Dependency-free C99 implementations of:
 *
 *   - SHA-1, SHA-256, SHA-512 (FIPS 180-4), one-shot and incremental
 *   - HMAC-SHA256 (RFC 2104 / FIPS 198-1), one-shot and incremental
 *   - HKDF-SHA256 extract/expand (RFC 5869)
 *   - constant-time memory comparison and non-elidable secure zeroization
 *   - Base64 / Base64url (RFC 4648) with strict decoding, and hex
 *   - SHA-256 whitening of an explicitly supplied 64-bit entropy source
 *
 * This module is a dependency leaf with no ambient authority: it never calls
 * into the runtime. Callers that want runtime entropy pass an adapter over
 * asx_runtime_random_u64 as the source.
 *
 * Portability contract (docs/C_PORTABILITY_RULES.md):
 *   - every multi-byte word is loaded/stored byte-by-byte with explicit
 *     big-endian order (no unaligned loads, no type punning),
 *   - all arithmetic is on unsigned fixed-width types (no signed overflow),
 *   - shifts and rotates never reach the operand bit width,
 *   - no dynamic allocation; every context is a caller-owned value type.
 *
 * SHA-1 is provided only for protocol interoperability (e.g. the RFC 6455
 * WebSocket opening handshake). It is not collision resistant and must not
 * be used for new authentication or integrity designs.
 *
 * Thread safety: all functions are reentrant; contexts must not be shared
 * between threads without external synchronization.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SECURITY_CRYPTO_H
#define ASX_SECURITY_CRYPTO_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

#define ASX_SHA1_DIGEST_SIZE 20u
#define ASX_SHA1_BLOCK_SIZE 64u
#define ASX_SHA256_DIGEST_SIZE 32u
#define ASX_SHA256_BLOCK_SIZE 64u
#define ASX_SHA512_DIGEST_SIZE 64u
#define ASX_SHA512_BLOCK_SIZE 128u
#define ASX_HMAC_SHA256_SIZE ASX_SHA256_DIGEST_SIZE
/* RFC 5869: L <= 255 * HashLen */
#define ASX_HKDF_SHA256_MAX_OKM (255u * ASX_SHA256_DIGEST_SIZE)

/* ------------------------------------------------------------------ */
/* SHA-1 (interoperability only)                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state[5];
    uint64_t total_len; /* bytes absorbed so far */
    uint8_t block[ASX_SHA1_BLOCK_SIZE];
    uint32_t block_len;
} asx_sha1_ctx;

/* Initialize a SHA-1 context. ctx must not be NULL. */
ASX_API void asx_sha1_init(asx_sha1_ctx *ctx);

/* Absorb len bytes into a SHA-1 context. data may be NULL if len == 0. */
ASX_API void asx_sha1_update(asx_sha1_ctx *ctx, const void *data, size_t len);

/* Finish a SHA-1 computation and write the 20-byte digest.
 * The context is wiped afterwards and must be re-initialized before reuse. */
ASX_API void asx_sha1_final(asx_sha1_ctx *ctx, uint8_t out[ASX_SHA1_DIGEST_SIZE]);

/* One-shot SHA-1 of a buffer. data may be NULL if len == 0. */
ASX_API void asx_sha1(const void *data, size_t len, uint8_t out[ASX_SHA1_DIGEST_SIZE]);

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state[8];
    uint64_t total_len; /* bytes absorbed so far */
    uint8_t block[ASX_SHA256_BLOCK_SIZE];
    uint32_t block_len;
} asx_sha256_ctx;

/* Initialize a SHA-256 context. ctx must not be NULL. */
ASX_API void asx_sha256_init(asx_sha256_ctx *ctx);

/* Absorb len bytes into a SHA-256 context. data may be NULL if len == 0. */
ASX_API void asx_sha256_update(asx_sha256_ctx *ctx, const void *data, size_t len);

/* Finish a SHA-256 computation and write the 32-byte digest.
 * The context is wiped afterwards and must be re-initialized before reuse. */
ASX_API void asx_sha256_final(asx_sha256_ctx *ctx, uint8_t out[ASX_SHA256_DIGEST_SIZE]);

/* One-shot SHA-256 of a buffer. data may be NULL if len == 0. */
ASX_API void asx_sha256(const void *data, size_t len, uint8_t out[ASX_SHA256_DIGEST_SIZE]);

/* ------------------------------------------------------------------ */
/* SHA-512                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t state[8];
    uint64_t total_len; /* bytes absorbed so far (128-bit length encodes high bits) */
    uint8_t block[ASX_SHA512_BLOCK_SIZE];
    uint32_t block_len;
} asx_sha512_ctx;

/* Initialize a SHA-512 context. ctx must not be NULL. */
ASX_API void asx_sha512_init(asx_sha512_ctx *ctx);

/* Absorb len bytes into a SHA-512 context. data may be NULL if len == 0. */
ASX_API void asx_sha512_update(asx_sha512_ctx *ctx, const void *data, size_t len);

/* Finish a SHA-512 computation and write the 64-byte digest.
 * The context is wiped afterwards and must be re-initialized before reuse. */
ASX_API void asx_sha512_final(asx_sha512_ctx *ctx, uint8_t out[ASX_SHA512_DIGEST_SIZE]);

/* One-shot SHA-512 of a buffer. data may be NULL if len == 0. */
ASX_API void asx_sha512(const void *data, size_t len, uint8_t out[ASX_SHA512_DIGEST_SIZE]);

/* ------------------------------------------------------------------ */
/* HMAC-SHA256                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_sha256_ctx inner;
    asx_sha256_ctx outer;
} asx_hmac_sha256_ctx;

/* Initialize an HMAC-SHA256 context with a key of any length.
 * Keys longer than the 64-byte block are first hashed (RFC 2104).
 * key may be NULL if key_len == 0. */
ASX_API void asx_hmac_sha256_init(asx_hmac_sha256_ctx *ctx, const void *key, size_t key_len);

/* Absorb message bytes into an HMAC-SHA256 context. */
ASX_API void asx_hmac_sha256_update(asx_hmac_sha256_ctx *ctx, const void *data, size_t len);

/* Finish an HMAC-SHA256 computation and write the 32-byte tag.
 * The context (including derived key pads) is wiped afterwards. */
ASX_API void asx_hmac_sha256_final(asx_hmac_sha256_ctx *ctx, uint8_t out[ASX_HMAC_SHA256_SIZE]);

/* One-shot HMAC-SHA256(key, data). */
ASX_API void asx_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                             uint8_t out[ASX_HMAC_SHA256_SIZE]);

/* ------------------------------------------------------------------ */
/* HKDF-SHA256 (RFC 5869)                                              */
/* ------------------------------------------------------------------ */

/* HKDF-Extract: PRK = HMAC-SHA256(salt, IKM).
 * A NULL/empty salt is replaced by 32 zero bytes as the RFC specifies. */
ASX_API void asx_hkdf_sha256_extract(const void *salt, size_t salt_len, const void *ikm,
                                     size_t ikm_len, uint8_t prk[ASX_SHA256_DIGEST_SIZE]);

/* HKDF-Expand: derive okm_len bytes of output keying material from a PRK.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULL pointers, a PRK shorter
 * than 32 bytes, or okm_len above ASX_HKDF_SHA256_MAX_OKM (RFC 5869 limit).
 * okm is untouched on error. */
ASX_API ASX_MUST_USE asx_status asx_hkdf_sha256_expand(const uint8_t *prk, size_t prk_len,
                                                       const void *info, size_t info_len,
                                                       uint8_t *okm, size_t okm_len);

/* Full HKDF-SHA256 (extract then expand). Same error contract as expand. */
ASX_API ASX_MUST_USE asx_status asx_hkdf_sha256(const void *salt, size_t salt_len, const void *ikm,
                                                size_t ikm_len, const void *info, size_t info_len,
                                                uint8_t *okm, size_t okm_len);

/* ------------------------------------------------------------------ */
/* Constant-time helpers                                               */
/* ------------------------------------------------------------------ */

/* Compare two buffers in time that depends only on len.
 * Returns 1 when equal, 0 otherwise. NULL pointers compare unequal
 * unless len == 0. */
ASX_API ASX_MUST_USE int asx_crypto_ct_equal(const void *a, const void *b, size_t len);

/* Returns 1 when every byte of buf is zero, in time that depends only on len. */
ASX_API ASX_MUST_USE int asx_crypto_ct_is_zero(const void *buf, size_t len);

/* Overwrite len bytes with zeros through a volatile path the optimizer
 * cannot elide (dead-store elimination safe). NULL is a no-op. */
ASX_API void asx_crypto_secure_zero(void *buf, size_t len);

/* ------------------------------------------------------------------ */
/* Base64 / Base64url (RFC 4648)                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_BASE64_STANDARD = 0, /* RFC 4648 §4 alphabet, '=' padding required */
    ASX_BASE64_URL = 1,      /* RFC 4648 §5 alphabet, '=' padding required */
    ASX_BASE64_URL_NOPAD = 2 /* RFC 4648 §5 alphabet, padding forbidden */
} asx_base64_variant;

/* Number of characters (excluding the NUL terminator) that encoding
 * in_len bytes produces under the given variant. Returns 0 for in_len == 0
 * and also when the result would overflow size_t. */
ASX_API size_t asx_base64_encoded_len(size_t in_len, asx_base64_variant variant);

/* Encode bytes as Base64 text and NUL-terminate the result.
 * out_cap must be at least asx_base64_encoded_len() + 1.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for NULL pointers or an unknown
 * variant, or ASX_E_BUFFER_TOO_SMALL. out_len (optional) receives the
 * character count excluding the NUL. */
ASX_API ASX_MUST_USE asx_status asx_base64_encode(asx_base64_variant variant, const uint8_t *in,
                                                  size_t in_len, char *out, size_t out_cap,
                                                  size_t *out_len);

/* Strictly decode Base64 text: only alphabet characters of the chosen
 * variant, exact padding rules, no whitespace, and zero unused trailing
 * bits (canonical encoding only). Returns ASX_OK, ASX_E_INVALID_ARGUMENT for
 * malformed input or NULL pointers, or ASX_E_BUFFER_TOO_SMALL. The output
 * buffer contents are unspecified on error. */
ASX_API ASX_MUST_USE asx_status asx_base64_decode(asx_base64_variant variant, const char *in,
                                                  size_t in_len, uint8_t *out, size_t out_cap,
                                                  size_t *out_len);

/* ------------------------------------------------------------------ */
/* Hex                                                                 */
/* ------------------------------------------------------------------ */

/* Encode bytes as lowercase hex and NUL-terminate the result.
 * out_cap must be at least 2 * in_len + 1. Returns ASX_OK,
 * ASX_E_INVALID_ARGUMENT, or ASX_E_BUFFER_TOO_SMALL. */
ASX_API ASX_MUST_USE asx_status asx_hex_encode(const uint8_t *in, size_t in_len, char *out,
                                               size_t out_cap);

/* Decode hex text (either case, even length, no separators).
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for malformed input, or
 * ASX_E_BUFFER_TOO_SMALL. out_len (optional) receives the byte count. */
ASX_API ASX_MUST_USE asx_status asx_hex_decode(const char *in, size_t in_len, uint8_t *out,
                                               size_t out_cap, size_t *out_len);

/* ------------------------------------------------------------------ */
/* Entropy whitening                                                   */
/* ------------------------------------------------------------------ */

/* A 64-bit entropy source: writes one word and returns ASX_OK, or returns an
 * error status (for example an adapter over asx_runtime_random_u64). */
typedef asx_status (*asx_crypto_entropy_fn)(void *ctx, uint64_t *out);

/* Fill out with len bytes derived from an explicit entropy source.
 * Each 32-byte output block is SHA-256(domain || u64le(block) || 256 bits of
 * source output), so raw source words (e.g. a PRNG's internal state) are
 * never exposed. Deterministic when the source is deterministic.
 * Returns ASX_OK, ASX_E_INVALID_ARGUMENT for a NULL source or NULL out with
 * len > 0, or the source's error status; out is wiped on failure. */
ASX_API ASX_MUST_USE asx_status asx_crypto_random_bytes(asx_crypto_entropy_fn source, void *ctx,
                                                        uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SECURITY_CRYPTO_H */
