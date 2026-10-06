/*
 * test_crypto.c — official-vector tests for SHA-1/SHA-256/SHA-512,
 *                 HMAC-SHA256, HKDF-SHA256, Base64/Base64url, hex,
 *                 constant-time helpers, and runtime entropy whitening
 *
 * Vectors:
 *   FIPS 180-4 / NIST CSRC examples ("", "abc", 448-bit, 896-bit, 10^6 x 'a')
 *   RFC 4231 HMAC-SHA256 test cases 1-7
 *   RFC 5869 HKDF-SHA256 test cases 1-3
 *   RFC 4648 §10 Base64 and Base16 vectors
 *   RFC 6455 §1.3 opening-handshake accept computation
 *
 * Note: the RFC 6455 §1.3 example accept value is
 * "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/security/crypto.h>
#include <string.h>

static const char k_msg448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char k_msg896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                               "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

/* Hex-encode a digest into a static buffer for string comparison. */
static const char *to_hex(const uint8_t *data, size_t len) {
    static char buf[2u * 128u + 1u];
    if (asx_hex_encode(data, len, buf, sizeof(buf)) != ASX_OK) return "<hex-error>";
    return buf;
}

/* Deterministic xorshift32 for split points and payload bytes. */
static uint32_t test_rng(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ================================================================== */
/* SHA-1                                                               */
/* ================================================================== */

TEST(sha1_empty) {
    uint8_t d[ASX_SHA1_DIGEST_SIZE];
    asx_sha1("", 0u, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(sha1_abc) {
    uint8_t d[ASX_SHA1_DIGEST_SIZE];
    asx_sha1("abc", 3u, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(sha1_448_bit_message) {
    uint8_t d[ASX_SHA1_DIGEST_SIZE];
    asx_sha1(k_msg448, strlen(k_msg448), d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(sha1_million_a) {
    uint8_t chunk[1000];
    uint8_t d[ASX_SHA1_DIGEST_SIZE];
    asx_sha1_ctx ctx;
    unsigned i;

    memset(chunk, 'a', sizeof(chunk));
    asx_sha1_init(&ctx);
    for (i = 0u; i < 1000u; i++) asx_sha1_update(&ctx, chunk, sizeof(chunk));
    asx_sha1_final(&ctx, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

/* ================================================================== */
/* SHA-256                                                             */
/* ================================================================== */

TEST(sha256_empty) {
    uint8_t d[ASX_SHA256_DIGEST_SIZE];
    asx_sha256(NULL, 0u, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(sha256_abc) {
    uint8_t d[ASX_SHA256_DIGEST_SIZE];
    asx_sha256("abc", 3u, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(sha256_448_bit_message) {
    uint8_t d[ASX_SHA256_DIGEST_SIZE];
    asx_sha256(k_msg448, strlen(k_msg448), d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(sha256_896_bit_message) {
    uint8_t d[ASX_SHA256_DIGEST_SIZE];
    asx_sha256(k_msg896, strlen(k_msg896), d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(sha256_million_a) {
    uint8_t chunk[1000];
    uint8_t d[ASX_SHA256_DIGEST_SIZE];
    asx_sha256_ctx ctx;
    unsigned i;

    memset(chunk, 'a', sizeof(chunk));
    asx_sha256_init(&ctx);
    for (i = 0u; i < 1000u; i++) asx_sha256_update(&ctx, chunk, sizeof(chunk));
    asx_sha256_final(&ctx, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_final_wipes_context) {
    asx_sha256_ctx ctx;
    uint8_t d[ASX_SHA256_DIGEST_SIZE];

    asx_sha256_init(&ctx);
    asx_sha256_update(&ctx, "secret", 6u);
    asx_sha256_final(&ctx, d);
    ASSERT_TRUE(asx_crypto_ct_is_zero(&ctx, sizeof(ctx)));
}

/* ================================================================== */
/* SHA-512                                                             */
/* ================================================================== */

TEST(sha512_empty) {
    uint8_t d[ASX_SHA512_DIGEST_SIZE];
    asx_sha512("", 0u, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

TEST(sha512_abc) {
    uint8_t d[ASX_SHA512_DIGEST_SIZE];
    asx_sha512("abc", 3u, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

TEST(sha512_448_bit_message) {
    uint8_t d[ASX_SHA512_DIGEST_SIZE];
    asx_sha512(k_msg448, strlen(k_msg448), d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
                  "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
}

TEST(sha512_896_bit_message) {
    uint8_t d[ASX_SHA512_DIGEST_SIZE];
    asx_sha512(k_msg896, strlen(k_msg896), d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                  "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
}

TEST(sha512_million_a) {
    uint8_t chunk[1000];
    uint8_t d[ASX_SHA512_DIGEST_SIZE];
    asx_sha512_ctx ctx;
    unsigned i;

    memset(chunk, 'a', sizeof(chunk));
    asx_sha512_init(&ctx);
    for (i = 0u; i < 1000u; i++) asx_sha512_update(&ctx, chunk, sizeof(chunk));
    asx_sha512_final(&ctx, d);
    ASSERT_STR_EQ(to_hex(d, sizeof(d)),
                  "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                  "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
}

/* ================================================================== */
/* Incremental vs one-shot equivalence                                 */
/* ================================================================== */

TEST(incremental_matches_one_shot_random_splits) {
    uint8_t msg[700];
    uint32_t rng = 0x12345678u;
    unsigned trial;
    size_t i;

    for (i = 0u; i < sizeof(msg); i++) msg[i] = (uint8_t)(test_rng(&rng) & 0xFFu);

    for (trial = 0u; trial < 200u; trial++) {
        size_t len = (size_t)(test_rng(&rng) % (uint32_t)(sizeof(msg) + 1u));
        uint8_t one1[ASX_SHA1_DIGEST_SIZE], inc1[ASX_SHA1_DIGEST_SIZE];
        uint8_t one256[ASX_SHA256_DIGEST_SIZE], inc256[ASX_SHA256_DIGEST_SIZE];
        uint8_t one512[ASX_SHA512_DIGEST_SIZE], inc512[ASX_SHA512_DIGEST_SIZE];
        uint8_t onemac[ASX_HMAC_SHA256_SIZE], incmac[ASX_HMAC_SHA256_SIZE];
        asx_sha1_ctx c1;
        asx_sha256_ctx c256;
        asx_sha512_ctx c512;
        asx_hmac_sha256_ctx cmac;
        size_t off = 0u;

        asx_sha1(msg, len, one1);
        asx_sha256(msg, len, one256);
        asx_sha512(msg, len, one512);
        asx_hmac_sha256("split-key", 9u, msg, len, onemac);

        asx_sha1_init(&c1);
        asx_sha256_init(&c256);
        asx_sha512_init(&c512);
        asx_hmac_sha256_init(&cmac, "split-key", 9u);
        while (off < len) {
            size_t chunk = (size_t)(test_rng(&rng) % 150u);
            if (chunk > len - off) chunk = len - off;
            asx_sha1_update(&c1, msg + off, chunk);
            asx_sha256_update(&c256, msg + off, chunk);
            asx_sha512_update(&c512, msg + off, chunk);
            asx_hmac_sha256_update(&cmac, msg + off, chunk);
            off += chunk;
        }
        asx_sha1_final(&c1, inc1);
        asx_sha256_final(&c256, inc256);
        asx_sha512_final(&c512, inc512);
        asx_hmac_sha256_final(&cmac, incmac);

        ASSERT_TRUE(memcmp(one1, inc1, sizeof(one1)) == 0);
        ASSERT_TRUE(memcmp(one256, inc256, sizeof(one256)) == 0);
        ASSERT_TRUE(memcmp(one512, inc512, sizeof(one512)) == 0);
        ASSERT_TRUE(memcmp(onemac, incmac, sizeof(onemac)) == 0);
    }
}

TEST(padding_boundary_lengths_are_stable) {
    /* Lengths around the 55/56/64 and 111/112/128 padding boundaries must
     * agree between one-shot and byte-at-a-time absorption. */
    static const size_t lens[] = {0, 1, 55, 56, 57, 63, 64, 65, 111, 112, 113, 127, 128, 129};
    uint8_t msg[129];
    unsigned li;
    size_t i;

    for (i = 0u; i < sizeof(msg); i++) msg[i] = (uint8_t)(i * 7u + 3u);
    for (li = 0u; li < sizeof(lens) / sizeof(lens[0]); li++) {
        uint8_t a[ASX_SHA512_DIGEST_SIZE], b[ASX_SHA512_DIGEST_SIZE];
        uint8_t a2[ASX_SHA256_DIGEST_SIZE], b2[ASX_SHA256_DIGEST_SIZE];
        asx_sha512_ctx c512;
        asx_sha256_ctx c256;

        asx_sha512(msg, lens[li], a);
        asx_sha256(msg, lens[li], a2);
        asx_sha512_init(&c512);
        asx_sha256_init(&c256);
        for (i = 0u; i < lens[li]; i++) {
            asx_sha512_update(&c512, msg + i, 1u);
            asx_sha256_update(&c256, msg + i, 1u);
        }
        asx_sha512_final(&c512, b);
        asx_sha256_final(&c256, b2);
        ASSERT_TRUE(memcmp(a, b, sizeof(a)) == 0);
        ASSERT_TRUE(memcmp(a2, b2, sizeof(a2)) == 0);
    }
}

/* ================================================================== */
/* HMAC-SHA256 — RFC 4231                                              */
/* ================================================================== */

TEST(hmac_rfc4231_case1) {
    uint8_t key[20];
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    memset(key, 0x0b, sizeof(key));
    asx_hmac_sha256(key, sizeof(key), "Hi There", 8u, mac);
    ASSERT_STR_EQ(to_hex(mac, sizeof(mac)),
                  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(hmac_rfc4231_case2) {
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    const char *data = "what do ya want for nothing?";
    asx_hmac_sha256("Jefe", 4u, data, strlen(data), mac);
    ASSERT_STR_EQ(to_hex(mac, sizeof(mac)),
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(hmac_rfc4231_case3) {
    uint8_t key[20], data[50];
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    memset(key, 0xaa, sizeof(key));
    memset(data, 0xdd, sizeof(data));
    asx_hmac_sha256(key, sizeof(key), data, sizeof(data), mac);
    ASSERT_STR_EQ(to_hex(mac, sizeof(mac)),
                  "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

TEST(hmac_rfc4231_case4) {
    uint8_t key[25], data[50];
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    unsigned i;
    for (i = 0u; i < sizeof(key); i++) key[i] = (uint8_t)(i + 1u);
    memset(data, 0xcd, sizeof(data));
    asx_hmac_sha256(key, sizeof(key), data, sizeof(data), mac);
    ASSERT_STR_EQ(to_hex(mac, sizeof(mac)),
                  "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
}

TEST(hmac_rfc4231_case5_truncated) {
    uint8_t key[20];
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    memset(key, 0x0c, sizeof(key));
    asx_hmac_sha256(key, sizeof(key), "Test With Truncation", 20u, mac);
    /* RFC 4231 specifies only the leftmost 128 bits for this case. */
    ASSERT_STR_EQ(to_hex(mac, 16u), "a3b6167473100ee06e0c796c2955552b");
}

TEST(hmac_rfc4231_case6_long_key) {
    uint8_t key[131];
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    const char *data = "Test Using Larger Than Block-Size Key - Hash Key First";
    memset(key, 0xaa, sizeof(key));
    asx_hmac_sha256(key, sizeof(key), data, strlen(data), mac);
    ASSERT_STR_EQ(to_hex(mac, sizeof(mac)),
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(hmac_rfc4231_case7_long_key_long_data) {
    uint8_t key[131];
    uint8_t mac[ASX_HMAC_SHA256_SIZE];
    const char *data = "This is a test using a larger than block-size key and a larger than "
                       "block-size data. The key needs to be hashed before being used by the "
                       "HMAC algorithm.";
    memset(key, 0xaa, sizeof(key));
    asx_hmac_sha256(key, sizeof(key), data, strlen(data), mac);
    ASSERT_STR_EQ(to_hex(mac, sizeof(mac)),
                  "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

/* ================================================================== */
/* HKDF-SHA256 — RFC 5869                                              */
/* ================================================================== */

TEST(hkdf_rfc5869_case1) {
    uint8_t ikm[22], salt[13], info[10];
    uint8_t prk[ASX_SHA256_DIGEST_SIZE];
    uint8_t okm[42];
    unsigned i;

    memset(ikm, 0x0b, sizeof(ikm));
    for (i = 0u; i < sizeof(salt); i++) salt[i] = (uint8_t)i;
    for (i = 0u; i < sizeof(info); i++) info[i] = (uint8_t)(0xf0u + i);

    asx_hkdf_sha256_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    ASSERT_STR_EQ(to_hex(prk, sizeof(prk)),
                  "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    ASSERT_EQ(asx_hkdf_sha256_expand(prk, sizeof(prk), info, sizeof(info), okm, sizeof(okm)),
              ASX_OK);
    ASSERT_STR_EQ(to_hex(okm, sizeof(okm)), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db0"
                                            "2d56ecc4c5bf34007208d5b887185865");
}

TEST(hkdf_rfc5869_case2_long_inputs) {
    uint8_t ikm[80], salt[80], info[80];
    uint8_t prk[ASX_SHA256_DIGEST_SIZE];
    uint8_t okm[82];
    unsigned i;

    for (i = 0u; i < 80u; i++) {
        ikm[i] = (uint8_t)i;
        salt[i] = (uint8_t)(0x60u + i);
        info[i] = (uint8_t)(0xb0u + i);
    }
    asx_hkdf_sha256_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    ASSERT_STR_EQ(to_hex(prk, sizeof(prk)),
                  "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244");
    ASSERT_EQ(asx_hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm,
                              sizeof(okm)),
              ASX_OK);
    ASSERT_STR_EQ(to_hex(okm, sizeof(okm)),
                  "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
                  "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
                  "cc30c58179ec3e87c14c01d5c1f3434f1d87");
}

TEST(hkdf_rfc5869_case3_empty_salt_info) {
    uint8_t ikm[22];
    uint8_t prk[ASX_SHA256_DIGEST_SIZE];
    uint8_t okm[42];

    memset(ikm, 0x0b, sizeof(ikm));
    asx_hkdf_sha256_extract(NULL, 0u, ikm, sizeof(ikm), prk);
    ASSERT_STR_EQ(to_hex(prk, sizeof(prk)),
                  "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");
    ASSERT_EQ(asx_hkdf_sha256(NULL, 0u, ikm, sizeof(ikm), NULL, 0u, okm, sizeof(okm)), ASX_OK);
    ASSERT_STR_EQ(to_hex(okm, sizeof(okm)), "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec345"
                                            "4e5f3c738d2d9d201395faa4b61a96c8");
}

TEST(hkdf_rejects_oversized_and_bad_args) {
    uint8_t prk[ASX_SHA256_DIGEST_SIZE];
    uint8_t okm[8];

    memset(prk, 1, sizeof(prk));
    memset(okm, 0x5a, sizeof(okm));
    ASSERT_EQ(asx_hkdf_sha256_expand(prk, sizeof(prk), NULL, 0u, okm,
                                     (size_t)ASX_HKDF_SHA256_MAX_OKM + 1u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hkdf_sha256_expand(prk, 16u, NULL, 0u, okm, sizeof(okm)), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hkdf_sha256_expand(NULL, 32u, NULL, 0u, okm, sizeof(okm)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hkdf_sha256_expand(prk, sizeof(prk), NULL, 3u, okm, sizeof(okm)),
              ASX_E_INVALID_ARGUMENT);
    /* failure-atomic: output untouched */
    ASSERT_EQ(okm[0], 0x5a);
    ASSERT_EQ(asx_hkdf_sha256_expand(prk, sizeof(prk), NULL, 0u, NULL, 0u), ASX_OK);
}

/* ================================================================== */
/* Constant-time helpers                                               */
/* ================================================================== */

TEST(ct_equal_and_is_zero) {
    uint8_t a[16], b[16];
    unsigned i;

    memset(a, 0x42, sizeof(a));
    memset(b, 0x42, sizeof(b));
    ASSERT_TRUE(asx_crypto_ct_equal(a, b, sizeof(a)));
    for (i = 0u; i < sizeof(a); i++) {
        b[i] ^= 0x80u;
        ASSERT_FALSE(asx_crypto_ct_equal(a, b, sizeof(a)));
        b[i] ^= 0x80u;
    }
    ASSERT_TRUE(asx_crypto_ct_equal(NULL, NULL, 0u));
    ASSERT_FALSE(asx_crypto_ct_equal(a, NULL, 1u));
    ASSERT_FALSE(asx_crypto_ct_is_zero(a, sizeof(a)));
    memset(a, 0, sizeof(a));
    ASSERT_TRUE(asx_crypto_ct_is_zero(a, sizeof(a)));
    a[15] = 1u;
    ASSERT_FALSE(asx_crypto_ct_is_zero(a, sizeof(a)));
}

TEST(secure_zero_clears_buffer) {
    uint8_t buf[37];
    memset(buf, 0xA5, sizeof(buf));
    asx_crypto_secure_zero(buf, sizeof(buf));
    ASSERT_TRUE(asx_crypto_ct_is_zero(buf, sizeof(buf)));
    asx_crypto_secure_zero(NULL, 10u); /* no-op */
}

/* ================================================================== */
/* Base64 / Base64url — RFC 4648                                       */
/* ================================================================== */

TEST(base64_rfc4648_vectors) {
    static const char *const plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const coded[] = {"",         "Zg==",     "Zm8=",    "Zm9v",
                                        "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    unsigned i;

    for (i = 0u; i < 7u; i++) {
        char enc[16];
        uint8_t dec[16];
        size_t n = 99u;
        size_t plen = strlen(plain[i]);

        ASSERT_EQ(asx_base64_encoded_len(plen, ASX_BASE64_STANDARD), strlen(coded[i]));
        ASSERT_EQ(asx_base64_encode(ASX_BASE64_STANDARD, (const uint8_t *)plain[i], plen, enc,
                                    sizeof(enc), &n),
                  ASX_OK);
        ASSERT_EQ(n, strlen(coded[i]));
        ASSERT_STR_EQ(enc, coded[i]);

        ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, coded[i], strlen(coded[i]), dec,
                                    sizeof(dec), &n),
                  ASX_OK);
        ASSERT_EQ(n, plen);
        ASSERT_TRUE(memcmp(dec, plain[i], plen) == 0);
    }
}

TEST(base64url_alphabet_and_padding) {
    static const uint8_t raw[] = {0xfb, 0xff, 0xbf, 0xfe};
    char enc[16];
    uint8_t dec[8];
    size_t n = 0u;

    ASSERT_EQ(asx_base64_encode(ASX_BASE64_STANDARD, raw, sizeof(raw), enc, sizeof(enc), &n),
              ASX_OK);
    ASSERT_STR_EQ(enc, "+/+//g==");
    ASSERT_EQ(asx_base64_encode(ASX_BASE64_URL, raw, sizeof(raw), enc, sizeof(enc), &n), ASX_OK);
    ASSERT_STR_EQ(enc, "-_-__g==");
    ASSERT_EQ(asx_base64_encode(ASX_BASE64_URL_NOPAD, raw, sizeof(raw), enc, sizeof(enc), &n),
              ASX_OK);
    ASSERT_STR_EQ(enc, "-_-__g");
    ASSERT_EQ(n, 6u);

    ASSERT_EQ(asx_base64_decode(ASX_BASE64_URL_NOPAD, "-_-__g", 6u, dec, sizeof(dec), &n), ASX_OK);
    ASSERT_EQ(n, 4u);
    ASSERT_TRUE(memcmp(dec, raw, 4u) == 0);
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_URL, "-_-__g==", 8u, dec, sizeof(dec), &n), ASX_OK);
    ASSERT_TRUE(memcmp(dec, raw, 4u) == 0);

    /* cross-alphabet characters are rejected */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "-_-__g==", 8u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_URL, "+/+//g==", 8u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
}

TEST(base64_strict_rejections) {
    uint8_t dec[16];
    size_t n = 0u;

    /* missing padding in padded variant */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zg", 2u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* padding forbidden in nopad variant */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_URL_NOPAD, "Zg==", 4u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* impossible length */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_URL_NOPAD, "Zm9vY", 5u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* too much padding */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Z===", 4u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* padding in the middle */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zg==Zm9v", 8u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* whitespace / foreign bytes */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zm9 v", 5u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zm9\n", 4u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* non-canonical trailing bits ("Zh==" decodes to 'f' with stray bits) */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zh==", 4u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zm9=", 4u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
    /* output too small */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, "Zm9vYmFy", 8u, dec, 5u, &n),
              ASX_E_BUFFER_TOO_SMALL);
    /* unknown variant */
    ASSERT_EQ(asx_base64_decode((asx_base64_variant)9, "Zm9v", 4u, dec, sizeof(dec), &n),
              ASX_E_INVALID_ARGUMENT);
}

TEST(base64_encode_buffer_limits) {
    char out[5];
    size_t n = 0u;
    /* "foo" needs 4 chars + NUL = 5 */
    ASSERT_EQ(asx_base64_encode(ASX_BASE64_STANDARD, (const uint8_t *)"foo", 3u, out, 5u, &n),
              ASX_OK);
    ASSERT_EQ(asx_base64_encode(ASX_BASE64_STANDARD, (const uint8_t *)"foo", 3u, out, 4u, &n),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_base64_encode(ASX_BASE64_STANDARD, NULL, 3u, out, sizeof(out), &n),
              ASX_E_INVALID_ARGUMENT);
}

TEST(base64_roundtrip_all_lengths) {
    uint8_t raw[64];
    char enc[100];
    uint8_t dec[64];
    unsigned len;
    unsigned v;

    for (len = 0u; len < sizeof(raw); len++) raw[len] = (uint8_t)(len * 37u + 11u);
    for (v = 0u; v < 3u; v++) {
        for (len = 0u; len <= sizeof(raw); len++) {
            size_t n = 0u;
            size_t m = 0u;
            ASSERT_EQ(asx_base64_encode((asx_base64_variant)v, raw, len, enc, sizeof(enc), &n),
                      ASX_OK);
            ASSERT_EQ(n, asx_base64_encoded_len(len, (asx_base64_variant)v));
            ASSERT_EQ(asx_base64_decode((asx_base64_variant)v, enc, n, dec, sizeof(dec), &m),
                      ASX_OK);
            ASSERT_EQ(m, (size_t)len);
            ASSERT_TRUE(len == 0u || memcmp(raw, dec, len) == 0);
        }
    }
}

/* ================================================================== */
/* Hex — RFC 4648 §8 (Base16)                                          */
/* ================================================================== */

TEST(hex_rfc4648_vectors) {
    char out[16];
    uint8_t dec[8];
    size_t n = 0u;

    ASSERT_EQ(asx_hex_encode((const uint8_t *)"foobar", 6u, out, sizeof(out)), ASX_OK);
    ASSERT_STR_EQ(out, "666f6f626172");
    ASSERT_EQ(asx_hex_decode("666F6F626172", 12u, dec, sizeof(dec), &n), ASX_OK);
    ASSERT_EQ(n, 6u);
    ASSERT_TRUE(memcmp(dec, "foobar", 6u) == 0);
    ASSERT_EQ(asx_hex_encode(NULL, 0u, out, 1u), ASX_OK);
    ASSERT_STR_EQ(out, "");
}

TEST(hex_rejections) {
    char out[4];
    uint8_t dec[4];
    size_t n = 0u;

    ASSERT_EQ(asx_hex_decode("abc", 3u, dec, sizeof(dec), &n), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hex_decode("zz", 2u, dec, sizeof(dec), &n), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hex_decode("0x12", 4u, dec, sizeof(dec), &n), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_hex_decode("0011223344", 10u, dec, sizeof(dec), &n), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_hex_encode((const uint8_t *)"ab", 2u, out, 4u), ASX_E_BUFFER_TOO_SMALL);
}

/* ================================================================== */
/* RFC 6455 §1.3 handshake accept                                      */
/* ================================================================== */

TEST(rfc6455_accept_key_example) {
    static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    asx_sha1_ctx ctx;
    uint8_t digest[ASX_SHA1_DIGEST_SIZE];
    uint8_t nonce[16];
    char accept[32];
    size_t n = 0u;

    /* The client key is the base64 of a 16-byte nonce. */
    ASSERT_EQ(asx_base64_decode(ASX_BASE64_STANDARD, key, strlen(key), nonce, sizeof(nonce), &n),
              ASX_OK);
    ASSERT_EQ(n, 16u);
    ASSERT_TRUE(memcmp(nonce, "the sample nonce", 16u) == 0);

    asx_sha1_init(&ctx);
    asx_sha1_update(&ctx, key, strlen(key));
    asx_sha1_update(&ctx, guid, strlen(guid));
    asx_sha1_final(&ctx, digest);
    ASSERT_EQ(asx_base64_encode(ASX_BASE64_STANDARD, digest, sizeof(digest), accept, sizeof(accept),
                                &n),
              ASX_OK);
    ASSERT_STR_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

/* ================================================================== */
/* Entropy whitening                                                   */
/* ================================================================== */

typedef struct {
    uint64_t next;
    uint32_t calls;
    uint32_t fail_after; /* 0 = never fail */
} counter_source;

static asx_status counter_entropy(void *ctx, uint64_t *out) {
    counter_source *src = (counter_source *)ctx;
    src->calls++;
    if (src->fail_after != 0u && src->calls > src->fail_after) return ASX_E_HOOK_MISSING;
    *out = src->next++;
    return ASX_OK;
}

TEST(random_bytes_deterministic_and_whitened) {
    counter_source s1 = {UINT64_C(0x1122334455667788), 0u, 0u};
    counter_source s2 = {UINT64_C(0x1122334455667788), 0u, 0u};
    uint8_t a[70], b[70];
    uint8_t raw[8];
    unsigned i;

    ASSERT_EQ(asx_crypto_random_bytes(counter_entropy, &s1, a, sizeof(a)), ASX_OK);
    ASSERT_EQ(asx_crypto_random_bytes(counter_entropy, &s2, b, sizeof(b)), ASX_OK);
    ASSERT_TRUE(memcmp(a, b, sizeof(a)) == 0); /* same source -> same output */
    ASSERT_EQ(s1.calls, 12u);                  /* 3 blocks x 4 words */
    for (i = 0u; i < 8u; i++) raw[i] = (uint8_t)(UINT64_C(0x1122334455667788) >> (8u * i));
    ASSERT_FALSE(memcmp(a, raw, sizeof(raw)) == 0); /* raw words never exposed */
    ASSERT_FALSE(memcmp(a, a + 32, 32u) == 0);      /* blocks differ */
}

TEST(random_bytes_propagates_source_failure_and_wipes) {
    counter_source src = {1u, 0u, 5u}; /* fails during the second block */
    uint8_t out[64];

    memset(out, 0xEE, sizeof(out));
    ASSERT_EQ(asx_crypto_random_bytes(counter_entropy, &src, out, sizeof(out)), ASX_E_HOOK_MISSING);
    ASSERT_TRUE(asx_crypto_ct_is_zero(out, sizeof(out)));
    ASSERT_EQ(asx_crypto_random_bytes(NULL, NULL, out, sizeof(out)), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_crypto_random_bytes(counter_entropy, &src, NULL, 4u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_crypto_random_bytes(counter_entropy, &src, NULL, 0u), ASX_OK);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== crypto tests ===\n");

    RUN_TEST(sha1_empty);
    RUN_TEST(sha1_abc);
    RUN_TEST(sha1_448_bit_message);
    RUN_TEST(sha1_million_a);

    RUN_TEST(sha256_empty);
    RUN_TEST(sha256_abc);
    RUN_TEST(sha256_448_bit_message);
    RUN_TEST(sha256_896_bit_message);
    RUN_TEST(sha256_million_a);
    RUN_TEST(sha256_final_wipes_context);

    RUN_TEST(sha512_empty);
    RUN_TEST(sha512_abc);
    RUN_TEST(sha512_448_bit_message);
    RUN_TEST(sha512_896_bit_message);
    RUN_TEST(sha512_million_a);

    RUN_TEST(incremental_matches_one_shot_random_splits);
    RUN_TEST(padding_boundary_lengths_are_stable);

    RUN_TEST(hmac_rfc4231_case1);
    RUN_TEST(hmac_rfc4231_case2);
    RUN_TEST(hmac_rfc4231_case3);
    RUN_TEST(hmac_rfc4231_case4);
    RUN_TEST(hmac_rfc4231_case5_truncated);
    RUN_TEST(hmac_rfc4231_case6_long_key);
    RUN_TEST(hmac_rfc4231_case7_long_key_long_data);

    RUN_TEST(hkdf_rfc5869_case1);
    RUN_TEST(hkdf_rfc5869_case2_long_inputs);
    RUN_TEST(hkdf_rfc5869_case3_empty_salt_info);
    RUN_TEST(hkdf_rejects_oversized_and_bad_args);

    RUN_TEST(ct_equal_and_is_zero);
    RUN_TEST(secure_zero_clears_buffer);

    RUN_TEST(base64_rfc4648_vectors);
    RUN_TEST(base64url_alphabet_and_padding);
    RUN_TEST(base64_strict_rejections);
    RUN_TEST(base64_encode_buffer_limits);
    RUN_TEST(base64_roundtrip_all_lengths);

    RUN_TEST(hex_rfc4648_vectors);
    RUN_TEST(hex_rejections);

    RUN_TEST(rfc6455_accept_key_example);

    RUN_TEST(random_bytes_deterministic_and_whitened);
    RUN_TEST(random_bytes_propagates_source_failure_and_wipes);

    TEST_REPORT();
    return test_failures;
}
