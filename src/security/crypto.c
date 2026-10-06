/*
 * crypto.c — portable SHA-1/SHA-256/SHA-512, HMAC-SHA256, HKDF-SHA256,
 *            constant-time helpers, Base64/Base64url and hex codecs
 *
 * Reference specifications:
 *   FIPS 180-4 (SHA family), RFC 2104 / FIPS 198-1 (HMAC), RFC 5869 (HKDF),
 *   RFC 4648 (Base16/Base64/Base64url).
 *
 * Every multi-byte word moves through the byte-wise big-endian helpers in
 * asx/portable.h, so the code is independent of host alignment and byte
 * order. All arithmetic is unsigned; every rotate amount is in 1..(width-1).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/portable.h>
#include <asx/security/crypto.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bit helpers                                                         */
/* ------------------------------------------------------------------ */

/* n must be in 1..31 */
static uint32_t rotl32(uint32_t x, unsigned n) { return (uint32_t)((x << n) | (x >> (32u - n))); }

/* n must be in 1..31 */
static uint32_t rotr32(uint32_t x, unsigned n) { return (uint32_t)((x >> n) | (x << (32u - n))); }

/* n must be in 1..63 */
static uint64_t rotr64(uint64_t x, unsigned n) { return (uint64_t)((x >> n) | (x << (64u - n))); }

/* ------------------------------------------------------------------ */
/* Constant-time helpers and zeroization                               */
/* ------------------------------------------------------------------ */

void asx_crypto_secure_zero(void *buf, size_t len) {
    volatile uint8_t *p;

    if (buf == NULL) return;
    /* Writes through a volatile lvalue are observable side effects, so the
     * compiler may not drop them as dead stores even when buf is about to go
     * out of scope. */
    p = (volatile uint8_t *)buf;
    while (len > 0u) {
        *p = 0u;
        p++;
        len--;
    }
}

int asx_crypto_ct_equal(const void *a, const void *b, size_t len) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    volatile uint8_t diff = 0u;
    size_t i;

    if (len == 0u) return 1;
    if (pa == NULL || pb == NULL) return 0;
    for (i = 0u; i < len; i++) { diff = (uint8_t)(diff | (uint8_t)(pa[i] ^ pb[i])); }
    /* diff == 0 -> (0 - 1) >> 8 has bit 0 set; diff in 1..255 -> 0. */
    return (int)(((((uint32_t)diff) - 1u) >> 8) & 1u);
}

int asx_crypto_ct_is_zero(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    volatile uint8_t acc = 0u;
    size_t i;

    if (len == 0u) return 1;
    if (p == NULL) return 0;
    for (i = 0u; i < len; i++) { acc = (uint8_t)(acc | p[i]); }
    return (int)(((((uint32_t)acc) - 1u) >> 8) & 1u);
}

/* ------------------------------------------------------------------ */
/* Shared Merkle-Damgard buffering                                     */
/* ------------------------------------------------------------------ */

typedef void (*md_compress_fn)(void *state, const uint8_t *block);

/* Absorb bytes into a block buffer, compressing every full block. */
static void md_absorb(void *state, uint8_t *block, uint32_t *block_len, uint32_t block_size,
                      md_compress_fn compress, const uint8_t *p, size_t len) {
    if (*block_len > 0u) {
        size_t take = (size_t)(block_size - *block_len);
        if (take > len) take = len;
        memcpy(block + *block_len, p, take);
        *block_len += (uint32_t)take;
        p += take;
        len -= take;
        if (*block_len == block_size) {
            compress(state, block);
            *block_len = 0u;
        }
    }
    while (len >= block_size) {
        compress(state, p);
        p += block_size;
        len -= block_size;
    }
    if (len > 0u) {
        memcpy(block, p, len);
        *block_len = (uint32_t)len;
    }
}

/* ------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-4 §6.1)                                             */
/* ------------------------------------------------------------------ */

static void sha1_compress(void *state_v, const uint8_t *block) {
    uint32_t *state = (uint32_t *)state_v;
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, temp;
    unsigned t;

    for (t = 0u; t < 16u; t++) w[t] = asx_load_be_u32(block + (4u * t));
    for (t = 16u; t < 80u; t++) w[t] = rotl32(w[t - 3u] ^ w[t - 8u] ^ w[t - 14u] ^ w[t - 16u], 1u);

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (t = 0u; t < 80u; t++) {
        if (t < 20u) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (t < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        temp = rotl32(a, 5u) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl32(b, 30u);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void asx_sha1_init(asx_sha1_ctx *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xEFCDAB89u;
    ctx->state[2] = 0x98BADCFEu;
    ctx->state[3] = 0x10325476u;
    ctx->state[4] = 0xC3D2E1F0u;
}

void asx_sha1_update(asx_sha1_ctx *ctx, const void *data, size_t len) {
    if (ctx == NULL || len == 0u || data == NULL) return;
    ctx->total_len += (uint64_t)len;
    md_absorb(ctx->state, ctx->block, &ctx->block_len, ASX_SHA1_BLOCK_SIZE, sha1_compress,
              (const uint8_t *)data, len);
}

void asx_sha1_final(asx_sha1_ctx *ctx, uint8_t out[ASX_SHA1_DIGEST_SIZE]) {
    uint64_t bit_len;
    unsigned i;

    if (ctx == NULL || out == NULL) return;
    bit_len = ctx->total_len << 3;
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        memset(ctx->block + ctx->block_len, 0, ASX_SHA1_BLOCK_SIZE - ctx->block_len);
        sha1_compress(ctx->state, ctx->block);
        ctx->block_len = 0u;
    }
    memset(ctx->block + ctx->block_len, 0, 56u - ctx->block_len);
    asx_store_be_u64(ctx->block + 56, bit_len);
    sha1_compress(ctx->state, ctx->block);
    for (i = 0u; i < 5u; i++) asx_store_be_u32(out + (4u * i), ctx->state[i]);
    asx_crypto_secure_zero(ctx, sizeof(*ctx));
}

void asx_sha1(const void *data, size_t len, uint8_t out[ASX_SHA1_DIGEST_SIZE]) {
    asx_sha1_ctx ctx;
    asx_sha1_init(&ctx);
    asx_sha1_update(&ctx, data, len);
    asx_sha1_final(&ctx, out);
}

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4 §6.2)                                           */
/* ------------------------------------------------------------------ */

static const uint32_t k_sha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static void sha256_compress(void *state_v, const uint8_t *block) {
    uint32_t *state = (uint32_t *)state_v;
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    unsigned t;

    for (t = 0u; t < 16u; t++) w[t] = asx_load_be_u32(block + (4u * t));
    for (t = 16u; t < 64u; t++) {
        uint32_t s0 = rotr32(w[t - 15u], 7u) ^ rotr32(w[t - 15u], 18u) ^ (w[t - 15u] >> 3);
        uint32_t s1 = rotr32(w[t - 2u], 17u) ^ rotr32(w[t - 2u], 19u) ^ (w[t - 2u] >> 10);
        w[t] = w[t - 16u] + s0 + w[t - 7u] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (t = 0u; t < 64u; t++) {
        uint32_t big_s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t big_s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + big_s1 + ch + k_sha256[t] + w[t];
        t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void asx_sha256_init(asx_sha256_ctx *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

void asx_sha256_update(asx_sha256_ctx *ctx, const void *data, size_t len) {
    if (ctx == NULL || len == 0u || data == NULL) return;
    ctx->total_len += (uint64_t)len;
    md_absorb(ctx->state, ctx->block, &ctx->block_len, ASX_SHA256_BLOCK_SIZE, sha256_compress,
              (const uint8_t *)data, len);
}

void asx_sha256_final(asx_sha256_ctx *ctx, uint8_t out[ASX_SHA256_DIGEST_SIZE]) {
    uint64_t bit_len;
    unsigned i;

    if (ctx == NULL || out == NULL) return;
    bit_len = ctx->total_len << 3;
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        memset(ctx->block + ctx->block_len, 0, ASX_SHA256_BLOCK_SIZE - ctx->block_len);
        sha256_compress(ctx->state, ctx->block);
        ctx->block_len = 0u;
    }
    memset(ctx->block + ctx->block_len, 0, 56u - ctx->block_len);
    asx_store_be_u64(ctx->block + 56, bit_len);
    sha256_compress(ctx->state, ctx->block);
    for (i = 0u; i < 8u; i++) asx_store_be_u32(out + (4u * i), ctx->state[i]);
    asx_crypto_secure_zero(ctx, sizeof(*ctx));
}

void asx_sha256(const void *data, size_t len, uint8_t out[ASX_SHA256_DIGEST_SIZE]) {
    asx_sha256_ctx ctx;
    asx_sha256_init(&ctx);
    asx_sha256_update(&ctx, data, len);
    asx_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------ */
/* SHA-512 (FIPS 180-4 §6.4)                                           */
/* ------------------------------------------------------------------ */

static const uint64_t k_sha512[80] = {
    UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd), UINT64_C(0xb5c0fbcfec4d3b2f),
    UINT64_C(0xe9b5dba58189dbbc), UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019),
    UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118), UINT64_C(0xd807aa98a3030242),
    UINT64_C(0x12835b0145706fbe), UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1), UINT64_C(0x9bdc06a725c71235),
    UINT64_C(0xc19bf174cf692694), UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3),
    UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65), UINT64_C(0x2de92c6f592b0275),
    UINT64_C(0x4a7484aa6ea6e483), UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210), UINT64_C(0xb00327c898fb213f),
    UINT64_C(0xbf597fc7beef0ee4), UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725),
    UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70), UINT64_C(0x27b70a8546d22ffc),
    UINT64_C(0x2e1b21385c26c926), UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8), UINT64_C(0x81c2c92e47edaee6),
    UINT64_C(0x92722c851482353b), UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001),
    UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30), UINT64_C(0xd192e819d6ef5218),
    UINT64_C(0xd69906245565a910), UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53), UINT64_C(0x2748774cdf8eeb99),
    UINT64_C(0x34b0bcb5e19b48a8), UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb),
    UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3), UINT64_C(0x748f82ee5defb2fc),
    UINT64_C(0x78a5636f43172f60), UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9), UINT64_C(0xbef9a3f7b2c67915),
    UINT64_C(0xc67178f2e372532b), UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207),
    UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178), UINT64_C(0x06f067aa72176fba),
    UINT64_C(0x0a637dc5a2c898a6), UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493), UINT64_C(0x3c9ebe0a15c9bebc),
    UINT64_C(0x431d67c49c100d4c), UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a),
    UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817)};

static void sha512_compress(void *state_v, const uint8_t *block) {
    uint64_t *state = (uint64_t *)state_v;
    uint64_t w[80];
    uint64_t a, b, c, d, e, f, g, h, t1, t2;
    unsigned t;

    for (t = 0u; t < 16u; t++) w[t] = asx_load_be_u64(block + (8u * t));
    for (t = 16u; t < 80u; t++) {
        uint64_t s0 = rotr64(w[t - 15u], 1u) ^ rotr64(w[t - 15u], 8u) ^ (w[t - 15u] >> 7);
        uint64_t s1 = rotr64(w[t - 2u], 19u) ^ rotr64(w[t - 2u], 61u) ^ (w[t - 2u] >> 6);
        w[t] = w[t - 16u] + s0 + w[t - 7u] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (t = 0u; t < 80u; t++) {
        uint64_t big_s1 = rotr64(e, 14u) ^ rotr64(e, 18u) ^ rotr64(e, 41u);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t big_s0 = rotr64(a, 28u) ^ rotr64(a, 34u) ^ rotr64(a, 39u);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + big_s1 + ch + k_sha512[t] + w[t];
        t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void asx_sha512_init(asx_sha512_ctx *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = UINT64_C(0x6a09e667f3bcc908);
    ctx->state[1] = UINT64_C(0xbb67ae8584caa73b);
    ctx->state[2] = UINT64_C(0x3c6ef372fe94f82b);
    ctx->state[3] = UINT64_C(0xa54ff53a5f1d36f1);
    ctx->state[4] = UINT64_C(0x510e527fade682d1);
    ctx->state[5] = UINT64_C(0x9b05688c2b3e6c1f);
    ctx->state[6] = UINT64_C(0x1f83d9abfb41bd6b);
    ctx->state[7] = UINT64_C(0x5be0cd19137e2179);
}

void asx_sha512_update(asx_sha512_ctx *ctx, const void *data, size_t len) {
    if (ctx == NULL || len == 0u || data == NULL) return;
    ctx->total_len += (uint64_t)len;
    md_absorb(ctx->state, ctx->block, &ctx->block_len, ASX_SHA512_BLOCK_SIZE, sha512_compress,
              (const uint8_t *)data, len);
}

void asx_sha512_final(asx_sha512_ctx *ctx, uint8_t out[ASX_SHA512_DIGEST_SIZE]) {
    uint64_t bit_len_lo;
    uint64_t bit_len_hi;
    unsigned i;

    if (ctx == NULL || out == NULL) return;
    /* 128-bit message length in bits = total_len * 8 */
    bit_len_hi = ctx->total_len >> 61;
    bit_len_lo = ctx->total_len << 3;
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 112u) {
        memset(ctx->block + ctx->block_len, 0, ASX_SHA512_BLOCK_SIZE - ctx->block_len);
        sha512_compress(ctx->state, ctx->block);
        ctx->block_len = 0u;
    }
    memset(ctx->block + ctx->block_len, 0, 112u - ctx->block_len);
    asx_store_be_u64(ctx->block + 112, bit_len_hi);
    asx_store_be_u64(ctx->block + 120, bit_len_lo);
    sha512_compress(ctx->state, ctx->block);
    for (i = 0u; i < 8u; i++) asx_store_be_u64(out + (8u * i), ctx->state[i]);
    asx_crypto_secure_zero(ctx, sizeof(*ctx));
}

void asx_sha512(const void *data, size_t len, uint8_t out[ASX_SHA512_DIGEST_SIZE]) {
    asx_sha512_ctx ctx;
    asx_sha512_init(&ctx);
    asx_sha512_update(&ctx, data, len);
    asx_sha512_final(&ctx, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA256 (RFC 2104)                                              */
/* ------------------------------------------------------------------ */

void asx_hmac_sha256_init(asx_hmac_sha256_ctx *ctx, const void *key, size_t key_len) {
    uint8_t k0[ASX_SHA256_BLOCK_SIZE];
    uint8_t pad[ASX_SHA256_BLOCK_SIZE];
    unsigned i;

    if (ctx == NULL) return;
    memset(k0, 0, sizeof(k0));
    if (key != NULL && key_len > ASX_SHA256_BLOCK_SIZE) {
        asx_sha256(key, key_len, k0);
    } else if (key != NULL && key_len > 0u) {
        memcpy(k0, key, key_len);
    }

    for (i = 0u; i < ASX_SHA256_BLOCK_SIZE; i++) pad[i] = (uint8_t)(k0[i] ^ 0x36u);
    asx_sha256_init(&ctx->inner);
    asx_sha256_update(&ctx->inner, pad, sizeof(pad));

    for (i = 0u; i < ASX_SHA256_BLOCK_SIZE; i++) pad[i] = (uint8_t)(k0[i] ^ 0x5cu);
    asx_sha256_init(&ctx->outer);
    asx_sha256_update(&ctx->outer, pad, sizeof(pad));

    asx_crypto_secure_zero(k0, sizeof(k0));
    asx_crypto_secure_zero(pad, sizeof(pad));
}

void asx_hmac_sha256_update(asx_hmac_sha256_ctx *ctx, const void *data, size_t len) {
    if (ctx == NULL) return;
    asx_sha256_update(&ctx->inner, data, len);
}

void asx_hmac_sha256_final(asx_hmac_sha256_ctx *ctx, uint8_t out[ASX_HMAC_SHA256_SIZE]) {
    uint8_t inner_hash[ASX_SHA256_DIGEST_SIZE];

    if (ctx == NULL || out == NULL) return;
    asx_sha256_final(&ctx->inner, inner_hash);
    asx_sha256_update(&ctx->outer, inner_hash, sizeof(inner_hash));
    asx_sha256_final(&ctx->outer, out);
    asx_crypto_secure_zero(inner_hash, sizeof(inner_hash));
    asx_crypto_secure_zero(ctx, sizeof(*ctx));
}

void asx_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                     uint8_t out[ASX_HMAC_SHA256_SIZE]) {
    asx_hmac_sha256_ctx ctx;
    asx_hmac_sha256_init(&ctx, key, key_len);
    asx_hmac_sha256_update(&ctx, data, data_len);
    asx_hmac_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------ */
/* HKDF-SHA256 (RFC 5869)                                              */
/* ------------------------------------------------------------------ */

void asx_hkdf_sha256_extract(const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                             uint8_t prk[ASX_SHA256_DIGEST_SIZE]) {
    static const uint8_t zero_salt[ASX_SHA256_DIGEST_SIZE] = {0};

    if (prk == NULL) return;
    if (salt == NULL || salt_len == 0u) {
        asx_hmac_sha256(zero_salt, sizeof(zero_salt), ikm, ikm_len, prk);
    } else {
        asx_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }
}

asx_status asx_hkdf_sha256_expand(const uint8_t *prk, size_t prk_len, const void *info,
                                  size_t info_len, uint8_t *okm, size_t okm_len) {
    asx_hmac_sha256_ctx ctx;
    uint8_t t_block[ASX_SHA256_DIGEST_SIZE];
    uint8_t counter;
    size_t done;

    if (prk == NULL || prk_len < ASX_SHA256_DIGEST_SIZE) return ASX_E_INVALID_ARGUMENT;
    if (okm == NULL && okm_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (info == NULL && info_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (okm_len > ASX_HKDF_SHA256_MAX_OKM) return ASX_E_INVALID_ARGUMENT;

    done = 0u;
    counter = 1u;
    while (done < okm_len) {
        size_t take;

        asx_hmac_sha256_init(&ctx, prk, prk_len);
        if (counter > 1u) asx_hmac_sha256_update(&ctx, t_block, sizeof(t_block));
        asx_hmac_sha256_update(&ctx, info, info_len);
        asx_hmac_sha256_update(&ctx, &counter, 1u);
        asx_hmac_sha256_final(&ctx, t_block);

        take = okm_len - done;
        if (take > sizeof(t_block)) take = sizeof(t_block);
        memcpy(okm + done, t_block, take);
        done += take;
        counter++; /* at most 255 iterations, bounded by the MAX_OKM check */
    }
    asx_crypto_secure_zero(t_block, sizeof(t_block));
    return ASX_OK;
}

asx_status asx_hkdf_sha256(const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                           const void *info, size_t info_len, uint8_t *okm, size_t okm_len) {
    uint8_t prk[ASX_SHA256_DIGEST_SIZE];
    asx_status st;

    if (ikm == NULL && ikm_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (okm == NULL && okm_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (info == NULL && info_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (okm_len > ASX_HKDF_SHA256_MAX_OKM) return ASX_E_INVALID_ARGUMENT;

    asx_hkdf_sha256_extract(salt, salt_len, ikm, ikm_len, prk);
    st = asx_hkdf_sha256_expand(prk, sizeof(prk), info, info_len, okm, okm_len);
    asx_crypto_secure_zero(prk, sizeof(prk));
    return st;
}

/* ------------------------------------------------------------------ */
/* Base64 / Base64url (RFC 4648)                                       */
/* ------------------------------------------------------------------ */

static const char b64_std_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64_url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64_variant_valid(asx_base64_variant variant) {
    return variant == ASX_BASE64_STANDARD || variant == ASX_BASE64_URL ||
           variant == ASX_BASE64_URL_NOPAD;
}

/* Map one character to its 6-bit value, or 0xFF when it is not part of the
 * variant's alphabet (padding included). */
static uint8_t b64_value(char ch, asx_base64_variant variant) {
    unsigned c = (unsigned)(unsigned char)ch;

    if (c >= (unsigned)'A' && c <= (unsigned)'Z') return (uint8_t)(c - (unsigned)'A');
    if (c >= (unsigned)'a' && c <= (unsigned)'z') return (uint8_t)(c - (unsigned)'a' + 26u);
    if (c >= (unsigned)'0' && c <= (unsigned)'9') return (uint8_t)(c - (unsigned)'0' + 52u);
    if (variant == ASX_BASE64_STANDARD) {
        if (c == (unsigned)'+') return 62u;
        if (c == (unsigned)'/') return 63u;
    } else {
        if (c == (unsigned)'-') return 62u;
        if (c == (unsigned)'_') return 63u;
    }
    return 0xFFu;
}

size_t asx_base64_encoded_len(size_t in_len, asx_base64_variant variant) {
    size_t groups = in_len / 3u;
    size_t rem = in_len % 3u;
    size_t tail;

    if (groups > (SIZE_MAX - 4u) / 4u) return 0u;
    if (rem == 0u) {
        tail = 0u;
    } else if (variant == ASX_BASE64_URL_NOPAD) {
        tail = rem + 1u;
    } else {
        tail = 4u;
    }
    return (groups * 4u) + tail;
}

asx_status asx_base64_encode(asx_base64_variant variant, const uint8_t *in, size_t in_len,
                             char *out, size_t out_cap, size_t *out_len) {
    const char *alphabet;
    size_t needed;
    size_t i;
    size_t o;

    if (out_len != NULL) *out_len = 0u;
    if (!b64_variant_valid(variant) || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (in == NULL && in_len > 0u) return ASX_E_INVALID_ARGUMENT;
    needed = asx_base64_encoded_len(in_len, variant);
    if (in_len > 0u && needed == 0u) return ASX_E_INVALID_ARGUMENT;
    if (out_cap == 0u || needed > out_cap - 1u) return ASX_E_BUFFER_TOO_SMALL;

    alphabet = (variant == ASX_BASE64_STANDARD) ? b64_std_alphabet : b64_url_alphabet;
    o = 0u;
    for (i = 0u; i + 3u <= in_len; i += 3u) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8) | (uint32_t)in[i + 2u];
        out[o++] = alphabet[(v >> 18) & 0x3Fu];
        out[o++] = alphabet[(v >> 12) & 0x3Fu];
        out[o++] = alphabet[(v >> 6) & 0x3Fu];
        out[o++] = alphabet[v & 0x3Fu];
    }
    if (in_len - i == 1u) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = alphabet[(v >> 18) & 0x3Fu];
        out[o++] = alphabet[(v >> 12) & 0x3Fu];
        if (variant != ASX_BASE64_URL_NOPAD) {
            out[o++] = '=';
            out[o++] = '=';
        }
    } else if (in_len - i == 2u) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1u] << 8);
        out[o++] = alphabet[(v >> 18) & 0x3Fu];
        out[o++] = alphabet[(v >> 12) & 0x3Fu];
        out[o++] = alphabet[(v >> 6) & 0x3Fu];
        if (variant != ASX_BASE64_URL_NOPAD) out[o++] = '=';
    }
    out[o] = '\0';
    if (out_len != NULL) *out_len = o;
    return ASX_OK;
}

asx_status asx_base64_decode(asx_base64_variant variant, const char *in, size_t in_len,
                             uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t data_len;
    size_t rem;
    size_t needed;
    size_t i;
    size_t o;

    if (out_len != NULL) *out_len = 0u;
    if (!b64_variant_valid(variant)) return ASX_E_INVALID_ARGUMENT;
    if (in == NULL && in_len > 0u) return ASX_E_INVALID_ARGUMENT;

    if (variant == ASX_BASE64_URL_NOPAD) {
        data_len = in_len;
    } else {
        /* Padded variants: whole quanta only, at most two trailing '='. */
        if (in_len % 4u != 0u) return ASX_E_INVALID_ARGUMENT;
        data_len = in_len;
        if (data_len > 0u && in[data_len - 1u] == '=') data_len--;
        if (data_len > 0u && in[data_len - 1u] == '=') data_len--;
    }
    rem = data_len % 4u;
    if (rem == 1u) return ASX_E_INVALID_ARGUMENT;

    needed = (data_len / 4u) * 3u + (rem == 0u ? 0u : rem - 1u);
    if (needed > 0u && (out == NULL || needed > out_cap)) {
        return out == NULL ? ASX_E_INVALID_ARGUMENT : ASX_E_BUFFER_TOO_SMALL;
    }

    o = 0u;
    for (i = 0u; i + 4u <= data_len; i += 4u) {
        uint8_t a = b64_value(in[i], variant);
        uint8_t b = b64_value(in[i + 1u], variant);
        uint8_t c = b64_value(in[i + 2u], variant);
        uint8_t d = b64_value(in[i + 3u], variant);
        uint32_t v;
        if ((a | b | c | d) & 0xC0u) return ASX_E_INVALID_ARGUMENT;
        v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        out[o++] = (uint8_t)((v >> 16) & 0xFFu);
        out[o++] = (uint8_t)((v >> 8) & 0xFFu);
        out[o++] = (uint8_t)(v & 0xFFu);
    }
    if (rem == 2u) {
        uint8_t a = b64_value(in[i], variant);
        uint8_t b = b64_value(in[i + 1u], variant);
        if ((a | b) & 0xC0u) return ASX_E_INVALID_ARGUMENT;
        if ((b & 0x0Fu) != 0u) return ASX_E_INVALID_ARGUMENT; /* non-canonical */
        out[o++] = (uint8_t)((uint32_t)(a << 2) | (uint32_t)(b >> 4));
    } else if (rem == 3u) {
        uint8_t a = b64_value(in[i], variant);
        uint8_t b = b64_value(in[i + 1u], variant);
        uint8_t c = b64_value(in[i + 2u], variant);
        if ((a | b | c) & 0xC0u) return ASX_E_INVALID_ARGUMENT;
        if ((c & 0x03u) != 0u) return ASX_E_INVALID_ARGUMENT; /* non-canonical */
        out[o++] = (uint8_t)(((uint32_t)a << 2) | ((uint32_t)b >> 4));
        out[o++] = (uint8_t)((((uint32_t)b & 0x0Fu) << 4) | ((uint32_t)c >> 2));
    }
    if (out_len != NULL) *out_len = o;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Hex                                                                 */
/* ------------------------------------------------------------------ */

static int hex_nibble(char ch, uint8_t *out) {
    unsigned c = (unsigned)(unsigned char)ch;

    if (c >= (unsigned)'0' && c <= (unsigned)'9') {
        *out = (uint8_t)(c - (unsigned)'0');
        return 1;
    }
    if (c >= (unsigned)'a' && c <= (unsigned)'f') {
        *out = (uint8_t)(c - (unsigned)'a' + 10u);
        return 1;
    }
    if (c >= (unsigned)'A' && c <= (unsigned)'F') {
        *out = (uint8_t)(c - (unsigned)'A' + 10u);
        return 1;
    }
    return 0;
}

asx_status asx_hex_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    if (out == NULL || (in == NULL && in_len > 0u)) return ASX_E_INVALID_ARGUMENT;
    if (in_len > (SIZE_MAX - 1u) / 2u) return ASX_E_INVALID_ARGUMENT;
    if (out_cap < (in_len * 2u) + 1u) return ASX_E_BUFFER_TOO_SMALL;
    for (i = 0u; i < in_len; i++) {
        out[2u * i] = digits[(in[i] >> 4) & 0x0Fu];
        out[(2u * i) + 1u] = digits[in[i] & 0x0Fu];
    }
    out[2u * in_len] = '\0';
    return ASX_OK;
}

asx_status asx_hex_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                          size_t *out_len) {
    size_t i;

    if (out_len != NULL) *out_len = 0u;
    if (in == NULL && in_len > 0u) return ASX_E_INVALID_ARGUMENT;
    if (in_len % 2u != 0u) return ASX_E_INVALID_ARGUMENT;
    if (in_len > 0u && out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (in_len / 2u > out_cap) return ASX_E_BUFFER_TOO_SMALL;
    for (i = 0u; i < in_len; i += 2u) {
        uint8_t hi;
        uint8_t lo;
        if (!hex_nibble(in[i], &hi) || !hex_nibble(in[i + 1u], &lo)) {
            return ASX_E_INVALID_ARGUMENT;
        }
        out[i / 2u] = (uint8_t)((uint32_t)(hi << 4) | (uint32_t)lo);
    }
    if (out_len != NULL) *out_len = in_len / 2u;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Entropy whitening                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_crypto_random_bytes(asx_crypto_entropy_fn source, void *ctx, uint8_t *out,
                                   size_t len) {
    static const char domain[] = "asupersync::crypto::random_bytes:v1";
    uint8_t seed[32];
    uint8_t block[ASX_SHA256_DIGEST_SIZE];
    uint8_t counter_le[8];
    uint64_t counter;
    size_t done;

    if (source == NULL || (out == NULL && len > 0u)) return ASX_E_INVALID_ARGUMENT;

    counter = 0u;
    done = 0u;
    while (done < len) {
        asx_sha256_ctx sha;
        size_t take;
        unsigned i;

        /* 256 bits of source output per 32-byte output block */
        for (i = 0u; i < 4u; i++) {
            uint64_t v = 0u;
            asx_status st = source(ctx, &v);
            if (st != ASX_OK) {
                asx_crypto_secure_zero(seed, sizeof(seed));
                asx_crypto_secure_zero(out, len);
                return st;
            }
            asx_store_le_u64(seed + (8u * i), v);
        }
        asx_store_le_u64(counter_le, counter);

        asx_sha256_init(&sha);
        asx_sha256_update(&sha, domain, sizeof(domain) - 1u);
        asx_sha256_update(&sha, counter_le, sizeof(counter_le));
        asx_sha256_update(&sha, seed, sizeof(seed));
        asx_sha256_final(&sha, block);

        take = len - done;
        if (take > sizeof(block)) take = sizeof(block);
        memcpy(out + done, block, take);
        done += take;
        counter++;
    }
    asx_crypto_secure_zero(seed, sizeof(seed));
    asx_crypto_secure_zero(block, sizeof(block));
    return ASX_OK;
}
