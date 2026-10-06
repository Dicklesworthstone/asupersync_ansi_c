/*
 * test_raptorq.c — unit tests for the RFC 6330 RaptorQ encoder/decoder
 *
 * Golden vectors were produced by the independent crates.io `raptorq`
 * crate (cberner/raptorq 2.0.1, RFC 6330 conformant) from the same
 * deterministic inputs used here:
 *   - source bytes: LCG s = s * 1664525 + 1013904223, byte = s >> 24,
 *   - digests: FNV-1a 64 over the concatenated symbol bytes.
 * Rand/Tuple spot values come from an independent model of RFC 6330
 * sections 5.3.5.1-5.3.5.4 driven by the crate's V0..V3 tables.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/raptorq/raptorq.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define RQ_FNV_OFFSET UINT64_C(0xcbf29ce484222325)

static void rq_fill(uint32_t seed, uint8_t *buf, size_t len) {
    uint32_t s = seed;
    size_t i;
    for (i = 0u; i < len; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 24);
    }
}

static uint64_t rq_fnv(uint64_t h, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0u; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

static uint8_t rq_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFFu;
}

/* Decode a lowercase hex string; returns the byte count or 0 on error. */
static size_t rq_hex_decode(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex) / 2u;
    size_t i;
    if (n > cap || strlen(hex) % 2u != 0u) return 0u;
    for (i = 0u; i < n; i++) {
        uint8_t hi = rq_hex_nibble(hex[2u * i]);
        uint8_t lo = rq_hex_nibble(hex[2u * i + 1u]);
        if (hi > 15u || lo > 15u) return 0u;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

/* Allocate and initialize an encoder; returns NULL on failure. */
static void *rq_open_encoder(asx_raptorq_encoder *enc, uint32_t k, uint32_t t, const uint8_t *src,
                             size_t src_len) {
    size_t need = 0u;
    void *ws;
    if (asx_raptorq_encoder_workspace_size(k, t, &need) != ASX_OK) return NULL;
    ws = malloc(need);
    if (ws == NULL) return NULL;
    if (asx_raptorq_encoder_init(enc, k, t, src, src_len, ws, need) != ASX_OK) {
        free(ws);
        return NULL;
    }
    return ws;
}

/* Allocate and initialize a decoder; returns NULL on failure. */
static void *rq_open_decoder(asx_raptorq_decoder *dec, uint32_t k, uint32_t t, uint32_t cap) {
    size_t need = 0u;
    void *ws;
    if (asx_raptorq_decoder_workspace_size(k, t, cap, &need) != ASX_OK) return NULL;
    ws = malloc(need);
    if (ws == NULL) return NULL;
    if (asx_raptorq_decoder_init(dec, k, t, cap, ws, need) != ASX_OK) {
        free(ws);
        return NULL;
    }
    return ws;
}

static uint32_t rq_lcg(uint32_t *state) {
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

/*
 * Encode K symbols, lose `loss_pct` percent of the source symbols (or all of
 * them when repair_only), replace them with repair symbols plus `overhead`
 * extras, feed everything in a scrambled order and decode.
 * Returns 1 when the decoded block is byte-identical to the source.
 */
static int rq_roundtrip(uint32_t k, uint32_t t, uint32_t seed, uint32_t loss_pct, uint32_t overhead,
                        int repair_only) {
    asx_raptorq_encoder enc;
    asx_raptorq_decoder dec;
    size_t bytes = (size_t)k * t;
    uint8_t *src = (uint8_t *)malloc(bytes);
    uint8_t *out = (uint8_t *)malloc(bytes);
    uint8_t *sym = (uint8_t *)malloc(t);
    uint8_t *lost = (uint8_t *)calloc(k, 1u);
    void *ews = NULL;
    void *dws = NULL;
    uint32_t state = seed;
    uint32_t nlost = 0u;
    uint32_t i;
    int ok = 0;

    if (src == NULL || out == NULL || sym == NULL || lost == NULL) goto done;
    rq_fill(seed, src, bytes);
    ews = rq_open_encoder(&enc, k, t, src, bytes);
    dws = rq_open_decoder(&dec, k, t, k + overhead);
    if (ews == NULL || dws == NULL) goto done;

    for (i = 0u; i < k; i++) {
        if (repair_only || rq_lcg(&state) % 100u < loss_pct) {
            lost[i] = 1u;
            nlost++;
        }
    }
    /* Repair symbols first (from a far ESI window), then surviving source
     * symbols in reverse order: arbitrary arrival order must not matter. */
    for (i = 0u; i < nlost + overhead; i++) {
        uint32_t esi = k + 7u * i + (seed % 5u);
        if (asx_raptorq_encoder_symbol(&enc, esi, sym, t) != ASX_OK) goto done;
        if (asx_raptorq_decoder_add_symbol(&dec, esi, sym, t) != ASX_OK) goto done;
    }
    for (i = k; i-- > 0u;) {
        if (lost[i]) continue;
        if (asx_raptorq_decoder_add_symbol(&dec, i, src + (size_t)i * t, t) != ASX_OK) goto done;
    }
    memset(out, 0xA5, bytes);
    if (asx_raptorq_decoder_decode(&dec, out, bytes) != ASX_OK) goto done;
    ok = memcmp(out, src, bytes) == 0;
done:
    if (!ok) {
        fprintf(stderr, "    roundtrip failed: k=%u t=%u loss=%u%% overhead=%u repair_only=%d\n", k,
                t, loss_pct, overhead, repair_only);
    }
    free(ews);
    free(dws);
    free(src);
    free(out);
    free(sym);
    free(lost);
    return ok;
}

/* ------------------------------------------------------------------ */
/* GF(256), Rand, Deg, Tuple and the parameter table                   */
/* ------------------------------------------------------------------ */

TEST(raptorq_gf256_field_identities) {
    uint32_t a;
    uint32_t b;
    for (a = 0u; a < 256u; a++) {
        uint8_t x = (uint8_t)a;
        ASSERT_EQ(asx_raptorq_gf256_mul(x, 1u), x);
        ASSERT_EQ(asx_raptorq_gf256_mul(x, 0u), 0u);
        ASSERT_EQ(asx_raptorq_gf256_div(x, 0u), 0u);
        if (x != 0u) {
            uint8_t inv = asx_raptorq_gf256_div(1u, x);
            ASSERT_EQ(asx_raptorq_gf256_mul(x, inv), 1u);
            ASSERT_EQ(asx_raptorq_gf256_div(x, x), 1u);
        }
        for (b = 0u; b < 256u; b++) {
            uint8_t y = (uint8_t)b;
            uint8_t xy = asx_raptorq_gf256_mul(x, y);
            ASSERT_EQ(xy, asx_raptorq_gf256_mul(y, x));
            if (y != 0u) ASSERT_EQ(asx_raptorq_gf256_div(xy, y), x);
        }
    }
    /* Distributivity and associativity on a deterministic sample. */
    for (a = 1u; a < 256u; a += 7u) {
        for (b = 3u; b < 256u; b += 11u) {
            uint8_t x = (uint8_t)a;
            uint8_t y = (uint8_t)b;
            uint8_t z = (uint8_t)(a * 13u + b);
            ASSERT_EQ(asx_raptorq_gf256_mul(x, (uint8_t)(y ^ z)),
                      (uint8_t)(asx_raptorq_gf256_mul(x, y) ^ asx_raptorq_gf256_mul(x, z)));
            ASSERT_EQ(asx_raptorq_gf256_mul(asx_raptorq_gf256_mul(x, y), z),
                      asx_raptorq_gf256_mul(x, asx_raptorq_gf256_mul(y, z)));
        }
    }
    /* OCT_EXP spot values: alpha^0 = 1, alpha^1 = 2, alpha^8 = 0x1D. */
    ASSERT_EQ(asx_raptorq_gf256_exp(0u), 1u);
    ASSERT_EQ(asx_raptorq_gf256_exp(1u), 2u);
    ASSERT_EQ(asx_raptorq_gf256_exp(8u), 29u);
    ASSERT_EQ(asx_raptorq_gf256_exp(254u), 142u);
    ASSERT_EQ(asx_raptorq_gf256_exp(255u), 1u);
    ASSERT_EQ(asx_raptorq_gf256_mul(asx_raptorq_gf256_exp(100u), asx_raptorq_gf256_exp(200u)),
              asx_raptorq_gf256_exp(300u));
}

TEST(raptorq_rand_and_deg_reference_values) {
    ASSERT_EQ(asx_raptorq_rand(0u, 0u, 4294967295u), 415381529u);
    ASSERT_EQ(asx_raptorq_rand(0u, 0u, 1000u), 529u);
    ASSERT_EQ(asx_raptorq_rand(123456789u, 7u, 1048576u), 882289u);
    ASSERT_EQ(asx_raptorq_rand(4294967295u, 255u, 97u), 90u);
    ASSERT_EQ(asx_raptorq_rand(42u, 6u, 10u), 6u);
    ASSERT_EQ(asx_raptorq_rand(42u, 6u, 0u), 0u);

    ASSERT_EQ(asx_raptorq_deg(0u, 1000u), 1u);
    ASSERT_EQ(asx_raptorq_deg(5242u, 1000u), 1u);
    ASSERT_EQ(asx_raptorq_deg(5243u, 1000u), 2u);
    ASSERT_EQ(asx_raptorq_deg(529531u, 1000u), 3u);
    ASSERT_EQ(asx_raptorq_deg(1017661u, 1000u), 29u);
    ASSERT_EQ(asx_raptorq_deg(1048575u, 1000u), 30u);
    ASSERT_EQ(asx_raptorq_deg(1048575u, 17u), 15u);  /* capped at W - 2 */
    ASSERT_EQ(asx_raptorq_deg(1048576u, 1000u), 0u); /* v out of range */
    ASSERT_EQ(asx_raptorq_deg(10u, 2u), 0u);         /* W too small */
}

TEST(raptorq_tuple_reference_values) {
    static const uint32_t k10[][7] = {{0u, 2u, 4u, 9u, 2u, 5u, 1u},
                                      {1u, 7u, 6u, 12u, 2u, 1u, 3u},
                                      {9u, 6u, 3u, 16u, 2u, 6u, 4u},
                                      {10u, 2u, 15u, 15u, 2u, 10u, 7u},
                                      {1000u, 4u, 11u, 3u, 2u, 6u, 7u}};
    static const uint32_t k1002[][7] = {{0u, 2u, 420u, 169u, 2u, 45u, 38u},
                                        {999u, 2u, 490u, 736u, 3u, 24u, 40u},
                                        {16777215u, 2u, 38u, 16u, 3u, 50u, 48u}};
    asx_raptorq_params prm;
    asx_raptorq_tuple tp;
    size_t i;

    ASSERT_EQ(asx_raptorq_params_init(10u, &prm), ASX_OK);
    for (i = 0u; i < sizeof(k10) / sizeof(k10[0]); i++) {
        ASSERT_EQ(asx_raptorq_tuple_compute(&prm, k10[i][0], &tp), ASX_OK);
        ASSERT_EQ(tp.d, k10[i][1]);
        ASSERT_EQ(tp.a, k10[i][2]);
        ASSERT_EQ(tp.b, k10[i][3]);
        ASSERT_EQ(tp.d1, k10[i][4]);
        ASSERT_EQ(tp.a1, k10[i][5]);
        ASSERT_EQ(tp.b1, k10[i][6]);
    }
    ASSERT_EQ(asx_raptorq_params_init(1000u, &prm), ASX_OK);
    for (i = 0u; i < sizeof(k1002) / sizeof(k1002[0]); i++) {
        ASSERT_EQ(asx_raptorq_tuple_compute(&prm, k1002[i][0], &tp), ASX_OK);
        ASSERT_EQ(tp.d, k1002[i][1]);
        ASSERT_EQ(tp.a, k1002[i][2]);
        ASSERT_EQ(tp.b, k1002[i][3]);
        ASSERT_EQ(tp.d1, k1002[i][4]);
        ASSERT_EQ(tp.a1, k1002[i][5]);
        ASSERT_EQ(tp.b1, k1002[i][6]);
    }
    ASSERT_EQ(asx_raptorq_tuple_compute(NULL, 0u, &tp), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_tuple_compute(&prm, 0u, NULL), ASX_E_INVALID_ARGUMENT);
    prm.p1 = 0u;
    ASSERT_EQ(asx_raptorq_tuple_compute(&prm, 0u, &tp), ASX_E_INVALID_ARGUMENT);
}

static int rq_is_prime_u32(uint32_t n) {
    uint32_t d;
    if (n < 2u) return 0;
    for (d = 2u; d <= n / d; d++) {
        if (n % d == 0u) return 0;
    }
    return 1;
}

TEST(raptorq_params_table_spot_checks) {
    asx_raptorq_params prm;
    uint32_t k;
    uint32_t prev_kp = 0u;

    /* K'=10 -> J=254, S=7, H=10, W=17 (first row of RFC 6330 Table 2). */
    ASSERT_EQ(asx_raptorq_params_init(10u, &prm), ASX_OK);
    ASSERT_EQ(prm.k_prime, 10u);
    ASSERT_EQ(prm.j, 254u);
    ASSERT_EQ(prm.s, 7u);
    ASSERT_EQ(prm.h, 10u);
    ASSERT_EQ(prm.w, 17u);
    ASSERT_EQ(prm.l, 27u);
    ASSERT_EQ(prm.p, 10u);
    ASSERT_EQ(prm.p1, 11u);
    ASSERT_EQ(prm.u, 0u);
    ASSERT_EQ(prm.b, 10u);

    /* K below the first row pads up to K'=10. */
    ASSERT_EQ(asx_raptorq_params_init(1u, &prm), ASX_OK);
    ASSERT_EQ(prm.k, 1u);
    ASSERT_EQ(prm.k_prime, 10u);

    ASSERT_EQ(asx_raptorq_params_init(11u, &prm), ASX_OK);
    ASSERT_EQ(prm.k_prime, 12u);
    ASSERT_EQ(prm.j, 630u);
    ASSERT_EQ(prm.w, 19u);

    ASSERT_EQ(asx_raptorq_params_init(1000u, &prm), ASX_OK);
    ASSERT_EQ(prm.k_prime, 1002u);
    ASSERT_EQ(prm.j, 299u);
    ASSERT_EQ(prm.s, 59u);
    ASSERT_EQ(prm.h, 10u);
    ASSERT_EQ(prm.w, 1021u);
    ASSERT_EQ(prm.l, 1071u);
    ASSERT_EQ(prm.p, 50u);
    ASSERT_EQ(prm.p1, 53u);

    ASSERT_EQ(asx_raptorq_params_init(8192u, &prm), ASX_OK);
    ASSERT_EQ(prm.k_prime, 8194u);
    ASSERT_EQ(prm.j, 212u);
    ASSERT_EQ(prm.s, 211u);
    ASSERT_EQ(prm.h, 11u);
    ASSERT_EQ(prm.w, 8273u);

    /* Last row: K'max = 56403. */
    ASSERT_EQ(asx_raptorq_params_init(ASX_RAPTORQ_MAX_SOURCE_SYMBOLS, &prm), ASX_OK);
    ASSERT_EQ(prm.k_prime, 56403u);
    ASSERT_EQ(prm.j, 471u);
    ASSERT_EQ(prm.s, 907u);
    ASSERT_EQ(prm.h, 16u);
    ASSERT_EQ(prm.w, 56951u);
    ASSERT_EQ(prm.l, 57326u);
    ASSERT_EQ(prm.p, 375u);
    ASSERT_EQ(prm.p1, 379u);

    ASSERT_EQ(asx_raptorq_params_init(0u, &prm), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_params_init(ASX_RAPTORQ_MAX_SOURCE_SYMBOLS + 1u, &prm),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_params_init(10u, NULL), ASX_E_INVALID_ARGUMENT);

    /* Structural invariants for every K' row (S and W prime, P1 prime). */
    for (k = 1u; k <= ASX_RAPTORQ_MAX_SOURCE_SYMBOLS; k = prm.k_prime + 1u) {
        ASSERT_EQ(asx_raptorq_params_init(k, &prm), ASX_OK);
        ASSERT_TRUE(prm.k_prime > prev_kp);
        ASSERT_TRUE(rq_is_prime_u32(prm.s));
        ASSERT_TRUE(rq_is_prime_u32(prm.w));
        ASSERT_TRUE(rq_is_prime_u32(prm.p1));
        ASSERT_TRUE(prm.p1 >= prm.p && prm.p >= prm.h); /* U = P - H >= 0 */
        ASSERT_EQ(prm.l, prm.k_prime + prm.s + prm.h);
        prev_kp = prm.k_prime;
    }
    ASSERT_EQ(prev_kp, ASX_RAPTORQ_MAX_SOURCE_SYMBOLS);
}

/* ------------------------------------------------------------------ */
/* Golden encoder vectors (independent RFC 6330 implementation)        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t k;
    uint32_t t;
    uint32_t seed;
    uint32_t digest_count; /* repair ESIs K .. K + digest_count - 1 */
    uint64_t digest;
    const char *first_repair; /* ESI K */
    uint64_t far_digest;      /* ESI K + 100000 then ESI 2^24 - 1 */
} rq_block_golden;

static const rq_block_golden g_block_golden[] = {
    {1u, 16u, 1u, 8u, UINT64_C(0x6d056193a18e0880), "3c5e81b40c5ec68e04a3406c97d63cfb",
     UINT64_C(0x0840b659f53ebb4f)},
    {2u, 8u, 2u, 8u, UINT64_C(0x4e9240cfb6100188), "aa3f1234781344b3",
     UINT64_C(0x468fdcc361409e09)},
    {10u, 16u, 3u, 16u, UINT64_C(0xc9bd70d1d56a06f1), "e39a283a99cbcb50941f1328a558abf5",
     UINT64_C(0xdf38e5674ce4cbd4)},
    {101u, 8u, 4u, 64u, UINT64_C(0x8544005660687715), "686c1792c99dc8d8",
     UINT64_C(0xe6d7de5f1a207dca)},
    {1000u, 4u, 5u, 64u, UINT64_C(0xde952cc671f2d650), "a2fbc12c", UINT64_C(0x8556455691f8f73e)},
    {1024u, 8u, 6u, 64u, UINT64_C(0x890677e9a1ca8add), "a6d1be12d5904082",
     UINT64_C(0xb5e26fa5587b8eba)},
    {8192u, 4u, 7u, 32u, UINT64_C(0x63f5d8a982bcbdc8), "c7371c15", UINT64_C(0xb23df9382aa1e53f)},
};

TEST(raptorq_golden_repair_symbols_match_reference) {
    size_t gi;
    for (gi = 0u; gi < sizeof(g_block_golden) / sizeof(g_block_golden[0]); gi++) {
        const rq_block_golden *g = &g_block_golden[gi];
        asx_raptorq_encoder enc;
        size_t bytes = (size_t)g->k * g->t;
        uint8_t *src = (uint8_t *)malloc(bytes);
        uint8_t sym[16];
        uint8_t expect[16];
        uint64_t h = RQ_FNV_OFFSET;
        uint64_t hf = RQ_FNV_OFFSET;
        void *ws;
        uint32_t i;
        int systematic_ok = 1;

        ASSERT_TRUE(src != NULL);
        rq_fill(g->seed, src, bytes);
        ws = rq_open_encoder(&enc, g->k, g->t, src, bytes);
        ASSERT_TRUE(ws != NULL);
        /* Systematic: ESIs 0..K-1 reproduce the source symbols. */
        for (i = 0u; i < g->k; i++) {
            ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, i, sym, sizeof(sym)), ASX_OK);
            if (memcmp(sym, src + (size_t)i * g->t, g->t) != 0) systematic_ok = 0;
        }
        for (i = 0u; i < g->digest_count; i++) {
            ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, g->k + i, sym, sizeof(sym)), ASX_OK);
            if (i == 0u) {
                ASSERT_EQ(rq_hex_decode(g->first_repair, expect, sizeof(expect)), (size_t)g->t);
                ASSERT_TRUE(memcmp(sym, expect, g->t) == 0);
            }
            h = rq_fnv(h, sym, g->t);
        }
        ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, g->k + 100000u, sym, sizeof(sym)), ASX_OK);
        hf = rq_fnv(hf, sym, g->t);
        ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, ASX_RAPTORQ_MAX_ESI, sym, sizeof(sym)), ASX_OK);
        hf = rq_fnv(hf, sym, g->t);
        fprintf(stderr, "    golden k=%u t=%u digest=%s far=%s systematic=%s\n", g->k, g->t,
                h == g->digest ? "match" : "MISMATCH", hf == g->far_digest ? "match" : "MISMATCH",
                systematic_ok ? "ok" : "BROKEN");
        free(ws);
        free(src);
        ASSERT_TRUE(systematic_ok);
        ASSERT_EQ(h, g->digest);
        ASSERT_EQ(hf, g->far_digest);
    }
}

/* Reference repair symbols ESI 10..21 for K=10, T=16, seed=3. */
static const char *const g_k10_repair[12] = {
    "e39a283a99cbcb50941f1328a558abf5", "e4cd704c6af2d9ee82ffee4dd91014aa",
    "b7bf0d9e7e73d8ad06754a1d0ed9da60", "015b9548e85ad4be944cba402c45e276",
    "6bacecbdfa22b8c1000c35800eca4161", "9401004db33214b0e53b0d8e92cf47c8",
    "83b83a8932221c384f25be8aa326c8d1", "64c4cf3949fa1f3edf36b308bac76fb0",
    "548fbc5cac232298a1889144f586c7e6", "17d787b0e9341b5685968857e9fd2070",
    "6b27a8ff2754f5cde3662267ada064a8", "9fba6f5ffa2b18d80830a9d4dcb36c38"};

TEST(raptorq_golden_k10_repair_symbols_exact) {
    asx_raptorq_encoder enc;
    uint8_t src[160];
    uint8_t ws[8192];
    uint8_t sym[16];
    uint8_t expect[16];
    size_t need = 0u;
    uint32_t i;

    rq_fill(3u, src, sizeof(src));
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(10u, 16u, &need), ASX_OK);
    ASSERT_TRUE(need <= sizeof(ws));
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 10u, 16u, src, sizeof(src), ws, sizeof(ws)), ASX_OK);
    for (i = 0u; i < 12u; i++) {
        ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, 10u + i, sym, sizeof(sym)), ASX_OK);
        ASSERT_EQ(rq_hex_decode(g_k10_repair[i], expect, sizeof(expect)), 16u);
        ASSERT_TRUE(memcmp(sym, expect, sizeof(sym)) == 0);
    }
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/*
 * Repair-only ESI set for K=10, T=16, seed=3 that is rank-deficient: the
 * independent reference decoder also fails on exactly these 10 symbols and
 * succeeds once ESI 11 is added. Symbol bytes were produced by the reference
 * encoder, so this also decodes foreign-encoded data.
 */
static const struct {
    uint32_t esi;
    const char *hex;
} g_singular_set[11] = {
    {47u, "d079094fcf2d1a223a27faadc27a2ce2"}, {10u, "e39a283a99cbcb50941f1328a558abf5"},
    {12u, "b7bf0d9e7e73d8ad06754a1d0ed9da60"}, {30u, "a4712ef96f2f44e917814cb26919961e"},
    {44u, "6d9794852b40b1e91a8308ac60f897c9"}, {13u, "015b9548e85ad4be944cba402c45e276"},
    {35u, "705d01939a8b0b37db96d6afd3dbd8c7"}, {46u, "bb3ad277821c3faef8d32eb494ff58f2"},
    {48u, "317ce42ea59786db66efef5ea8c24c1f"}, {41u, "be1be164237d1b6671900925ae622a03"},
    {11u, "e4cd704c6af2d9ee82ffee4dd91014aa"}};

TEST(raptorq_decode_rank_deficient_then_recover_with_more_symbols) {
    asx_raptorq_decoder dec;
    uint8_t src[160];
    uint8_t out[160];
    uint8_t sym[16];
    void *ws;
    uint32_t i;

    rq_fill(3u, src, sizeof(src));
    ws = rq_open_decoder(&dec, 10u, 16u, 11u);
    ASSERT_TRUE(ws != NULL);
    for (i = 0u; i < 10u; i++) {
        ASSERT_EQ(rq_hex_decode(g_singular_set[i].hex, sym, sizeof(sym)), 16u);
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, g_singular_set[i].esi, sym, sizeof(sym)),
                  ASX_OK);
    }
    memset(out, 0x5A, sizeof(out));
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0u; i < sizeof(out); i++) ASSERT_EQ(out[i], 0x5Au); /* untouched on failure */

    ASSERT_EQ(rq_hex_decode(g_singular_set[10].hex, sym, sizeof(sym)), 16u);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, g_singular_set[10].esi, sym, sizeof(sym)),
              ASX_OK);
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_OK);
    ASSERT_TRUE(memcmp(out, src, sizeof(out)) == 0);
    free(ws);
}

TEST(raptorq_decode_reference_repair_only_symbols) {
    asx_raptorq_decoder dec;
    uint8_t src[160];
    uint8_t out[160];
    uint8_t sym[16];
    void *ws;
    uint32_t i;

    rq_fill(3u, src, sizeof(src));
    ws = rq_open_decoder(&dec, 10u, 16u, 12u);
    ASSERT_TRUE(ws != NULL);
    for (i = 0u; i < 12u; i++) {
        ASSERT_EQ(rq_hex_decode(g_k10_repair[i], sym, sizeof(sym)), 16u);
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 10u + i, sym, sizeof(sym)), ASX_OK);
    }
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_OK);
    ASSERT_TRUE(memcmp(out, src, sizeof(out)) == 0);
    free(ws);
}

TEST(raptorq_roundtrip_random_losses) {
    static const uint32_t ks[] = {1u, 2u, 10u, 101u, 1000u};
    size_t i;
    for (i = 0u; i < sizeof(ks) / sizeof(ks[0]); i++) {
        ASSERT_TRUE(rq_roundtrip(ks[i], 16u, 100u + (uint32_t)i, 25u, 2u, 0));
        ASSERT_TRUE(rq_roundtrip(ks[i], 13u, 200u + (uint32_t)i, 60u, 1u, 0));
        ASSERT_TRUE(rq_roundtrip(ks[i], 64u, 300u + (uint32_t)i, 0u, 0u, 0));
    }
}

TEST(raptorq_roundtrip_repair_only) {
    static const uint32_t ks[] = {1u, 2u, 10u, 101u, 1000u};
    size_t i;
    for (i = 0u; i < sizeof(ks) / sizeof(ks[0]); i++) {
        ASSERT_TRUE(rq_roundtrip(ks[i], 8u, 400u + (uint32_t)i, 100u, 2u, 1));
    }
}

TEST(raptorq_roundtrip_large_block_k8192) { ASSERT_TRUE(rq_roundtrip(8192u, 8u, 77u, 20u, 2u, 0)); }

TEST(raptorq_decode_all_source_fast_path_and_extra_symbols) {
    asx_raptorq_encoder enc;
    asx_raptorq_decoder dec;
    uint8_t src[101u * 8u];
    uint8_t out[101u * 8u];
    uint8_t sym[8];
    void *ews;
    void *dws;
    uint32_t i;

    rq_fill(9u, src, sizeof(src));
    ews = rq_open_encoder(&enc, 101u, 8u, src, sizeof(src));
    dws = rq_open_decoder(&dec, 101u, 8u, 140u);
    ASSERT_TRUE(ews != NULL && dws != NULL);
    for (i = 0u; i < 30u; i++) {
        ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, 500u + i, sym, sizeof(sym)), ASX_OK);
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 500u + i, sym, sizeof(sym)), ASX_OK);
    }
    for (i = 0u; i < 101u; i++) {
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, i, src + i * 8u, 8u), ASX_OK);
    }
    ASSERT_EQ(dec.source_count, 101u);
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_OK);
    ASSERT_TRUE(memcmp(out, src, sizeof(out)) == 0);
    /* Decoding is repeatable and deterministic. */
    memset(out, 0, sizeof(out));
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_OK);
    ASSERT_TRUE(memcmp(out, src, sizeof(out)) == 0);
    free(ews);
    free(dws);
}

TEST(raptorq_decode_fewer_than_k_then_complete) {
    asx_raptorq_encoder enc;
    asx_raptorq_decoder dec;
    uint8_t src[26u * 4u];
    uint8_t out[26u * 4u];
    uint8_t sym[4];
    void *ews;
    void *dws;
    uint32_t i;

    rq_fill(31u, src, sizeof(src));
    ews = rq_open_encoder(&enc, 26u, 4u, src, sizeof(src));
    dws = rq_open_decoder(&dec, 26u, 4u, 40u);
    ASSERT_TRUE(ews != NULL && dws != NULL);
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_E_RESOURCE_EXHAUSTED);
    /* The 13 even source ESIs: still fewer than K = 26. */
    for (i = 0u; i < 13u; i++) {
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, i * 2u, src + i * 2u * 4u, 4u), ASX_OK);
    }
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0u; i < 15u; i++) {
        ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, 26u + i, sym, sizeof(sym)), ASX_OK);
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 26u + i, sym, sizeof(sym)), ASX_OK);
    }
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_OK);
    ASSERT_TRUE(memcmp(out, src, sizeof(out)) == 0);
    free(ews);
    free(dws);
}

TEST(raptorq_decoder_rejects_bad_symbols) {
    asx_raptorq_decoder dec;
    uint8_t sym[8] = {0};
    uint8_t out[16];
    void *ws;
    uint32_t i;

    ws = rq_open_decoder(&dec, 2u, 8u, 3u);
    ASSERT_TRUE(ws != NULL);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(NULL, 0u, sym, 8u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 0u, NULL, 8u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 0u, sym, 7u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, ASX_RAPTORQ_MAX_ESI + 1u, sym, 8u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 5u, sym, 8u), ASX_OK);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 5u, sym, 8u), ASX_E_DUPLICATE_SYMBOL);
    ASSERT_EQ(dec.count, 1u);
    for (i = 0u; i < 2u; i++) {
        ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, ASX_RAPTORQ_MAX_ESI - i, sym, 8u), ASX_OK);
    }
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 9u, sym, 8u), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(dec.count, 3u);
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, 15u), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, NULL, 16u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_decode(NULL, out, 16u), ASX_E_INVALID_ARGUMENT);
    free(ws);

    memset(&dec, 0, sizeof(dec));
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 0u, sym, 8u), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_raptorq_decoder_decode(&dec, out, sizeof(out)), ASX_E_INVALID_STATE);
}

/* ------------------------------------------------------------------ */
/* Encoder argument and workspace contracts                            */
/* ------------------------------------------------------------------ */

TEST(raptorq_workspace_queries_validate_arguments) {
    size_t sz = 1u;
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(10u, 16u, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(0u, 16u, &sz), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(sz, 0u);
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(ASX_RAPTORQ_MAX_SOURCE_SYMBOLS + 1u, 16u, &sz),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(10u, 0u, &sz), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(10u, ASX_RAPTORQ_MAX_SYMBOL_SIZE + 1u, &sz),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(10u, 16u, &sz), ASX_OK);
    ASSERT_TRUE(sz > 27u * 16u); /* at least the L intermediate symbols */

    ASSERT_EQ(asx_raptorq_decoder_workspace_size(10u, 16u, 9u, &sz), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_workspace_size(10u, 16u, ASX_RAPTORQ_MAX_ESI + 2u, &sz),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_workspace_size(10u, 0u, 10u, &sz), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_workspace_size(10u, 16u, 10u, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decoder_workspace_size(10u, 16u, 10u, &sz), ASX_OK);
    ASSERT_TRUE(sz > 10u * 16u);
}

TEST(raptorq_undersized_workspace_fails_closed) {
    asx_raptorq_encoder enc;
    asx_raptorq_decoder dec;
    uint8_t src[64];
    uint8_t sym[16];
    size_t need = 0u;
    uint8_t *ws;

    rq_fill(1u, src, sizeof(src));
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(4u, 16u, &need), ASX_OK);
    ws = (uint8_t *)malloc(need);
    ASSERT_TRUE(ws != NULL);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, src, sizeof(src), ws, need - 1u),
              ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(enc.ready, 0u);
    ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, 0u, sym, sizeof(sym)), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, src, sizeof(src), ws, need), ASX_OK);
    free(ws);

    ASSERT_EQ(asx_raptorq_decoder_workspace_size(4u, 16u, 8u, &need), ASX_OK);
    ws = (uint8_t *)malloc(need);
    ASSERT_TRUE(ws != NULL);
    ASSERT_EQ(asx_raptorq_decoder_init(&dec, 4u, 16u, 8u, ws, need - 1u), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(dec.ready, 0u);
    ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, 0u, sym, sizeof(sym)), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_raptorq_decoder_init(&dec, 4u, 16u, 8u, ws, need), ASX_OK);
    free(ws);
}

TEST(raptorq_encoder_argument_validation) {
    asx_raptorq_encoder enc;
    uint8_t src[64];
    uint8_t sym[16];
    uint8_t ws[8192];

    rq_fill(2u, src, sizeof(src));
    ASSERT_EQ(asx_raptorq_encoder_init(NULL, 4u, 16u, src, 64u, ws, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, NULL, 64u, ws, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, src, 64u, NULL, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 0u, 16u, src, 64u, ws, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 0u, src, 64u, ws, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, src, 0u, ws, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, src, 65u, ws, sizeof(ws)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_init(&enc, 4u, 16u, src, 64u, ws, sizeof(ws)), ASX_OK);
    ASSERT_EQ(asx_raptorq_encoder_symbol(NULL, 0u, sym, sizeof(sym)), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, 0u, NULL, sizeof(sym)), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, ASX_RAPTORQ_MAX_ESI + 1u, sym, sizeof(sym)),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, 0u, sym, 15u), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, ASX_RAPTORQ_MAX_ESI, sym, sizeof(sym)), ASX_OK);
}

TEST(raptorq_partial_final_symbol_is_zero_padded) {
    asx_raptorq_encoder a;
    asx_raptorq_encoder b;
    uint8_t src[50];
    uint8_t padded[64];
    uint8_t wa[8192];
    uint8_t wb[8192];
    uint8_t sa[16];
    uint8_t sb[16];
    uint32_t esi;

    rq_fill(5u, src, sizeof(src));
    memset(padded, 0, sizeof(padded));
    memcpy(padded, src, sizeof(src));
    ASSERT_EQ(asx_raptorq_encoder_init(&a, 4u, 16u, src, sizeof(src), wa, sizeof(wa)), ASX_OK);
    ASSERT_EQ(asx_raptorq_encoder_init(&b, 4u, 16u, padded, sizeof(padded), wb, sizeof(wb)),
              ASX_OK);
    for (esi = 0u; esi < 40u; esi++) {
        ASSERT_EQ(asx_raptorq_encoder_symbol(&a, esi, sa, sizeof(sa)), ASX_OK);
        ASSERT_EQ(asx_raptorq_encoder_symbol(&b, esi, sb, sizeof(sb)), ASX_OK);
        ASSERT_TRUE(memcmp(sa, sb, sizeof(sa)) == 0);
    }
}

TEST(raptorq_deterministic_across_workspace_alignment) {
    asx_raptorq_encoder enc[3];
    uint8_t src[101u * 12u];
    uint8_t *ws;
    uint8_t ref[12];
    uint8_t got[12];
    size_t need = 0u;
    size_t off;
    uint32_t esi;

    rq_fill(42u, src, sizeof(src));
    ASSERT_EQ(asx_raptorq_encoder_workspace_size(101u, 12u, &need), ASX_OK);
    ws = (uint8_t *)malloc((need + 8u) * 3u);
    ASSERT_TRUE(ws != NULL);
    /* Same input, three workspaces at different (mis)alignments. */
    for (off = 0u; off < 3u; off++) {
        uint8_t *base = ws + off * (need + 8u) + off * 3u;
        memset(base, (int)(0x11u * (off + 1u)), need);
        ASSERT_EQ(asx_raptorq_encoder_init(&enc[off], 101u, 12u, src, sizeof(src), base, need),
                  ASX_OK);
    }
    for (esi = 0u; esi < 300u; esi += 3u) {
        ASSERT_EQ(asx_raptorq_encoder_symbol(&enc[0], esi, ref, sizeof(ref)), ASX_OK);
        for (off = 1u; off < 3u; off++) {
            ASSERT_EQ(asx_raptorq_encoder_symbol(&enc[off], esi, got, sizeof(got)), ASX_OK);
            ASSERT_TRUE(memcmp(ref, got, sizeof(ref)) == 0);
        }
    }
    free(ws);
}

/* ------------------------------------------------------------------ */
/* Object-level helpers: OTI, partitioning, multiple source blocks     */
/* ------------------------------------------------------------------ */

TEST(raptorq_partition_function) {
    asx_raptorq_partition p;
    ASSERT_EQ(asx_raptorq_partition_compute(63u, 4u, &p), ASX_OK);
    ASSERT_EQ(p.large_size, 16u);
    ASSERT_EQ(p.small_size, 15u);
    ASSERT_EQ(p.large_count, 3u);
    ASSERT_EQ(p.small_count, 1u);
    ASSERT_EQ(asx_raptorq_partition_compute(8u, 3u, &p), ASX_OK);
    ASSERT_EQ(p.large_size, 3u);
    ASSERT_EQ(p.small_size, 2u);
    ASSERT_EQ(p.large_count, 2u);
    ASSERT_EQ(p.small_count, 1u);
    ASSERT_EQ(asx_raptorq_partition_compute(64u, 4u, &p), ASX_OK);
    ASSERT_EQ(p.large_size, 16u);
    ASSERT_EQ(p.small_size, 16u);
    ASSERT_EQ(p.large_count, 0u);
    ASSERT_EQ(p.small_count, 4u);
    ASSERT_EQ(asx_raptorq_partition_compute(5u, 0u, &p), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_partition_compute(5u, 1u, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(raptorq_oti_wire_roundtrip_and_validation) {
    asx_raptorq_oti oti;
    asx_raptorq_oti back;
    uint8_t wire[ASX_RAPTORQ_OTI_SIZE];
    uint8_t expect[ASX_RAPTORQ_OTI_SIZE];

    /* Matches the reference crate's default derivation for F=1e6, MTU 1280. */
    ASSERT_EQ(asx_raptorq_oti_init(&oti, 1000000u, 1280u, 8u, ASX_RAPTORQ_MAX_SOURCE_SYMBOLS),
              ASX_OK);
    ASSERT_EQ(oti.source_blocks, 1u);
    ASSERT_EQ(oti.sub_blocks, 1u);
    ASSERT_EQ(asx_raptorq_oti_serialize(&oti, wire), ASX_OK);
    ASSERT_EQ(rq_hex_decode("00000f424000050001000108", expect, sizeof(expect)), 12u);
    ASSERT_TRUE(memcmp(wire, expect, sizeof(wire)) == 0);
    ASSERT_EQ(asx_raptorq_oti_deserialize(wire, &back), ASX_OK);
    ASSERT_EQ(back.transfer_length, oti.transfer_length);
    ASSERT_EQ(back.symbol_size, oti.symbol_size);
    ASSERT_EQ(back.source_blocks, oti.source_blocks);
    ASSERT_EQ(back.sub_blocks, oti.sub_blocks);
    ASSERT_EQ(back.alignment, oti.alignment);

    ASSERT_EQ(asx_raptorq_oti_init(&oti, 10000u, 96u, 8u, ASX_RAPTORQ_MAX_SOURCE_SYMBOLS), ASX_OK);
    ASSERT_EQ(asx_raptorq_oti_serialize(&oti, wire), ASX_OK);
    ASSERT_EQ(rq_hex_decode("000000271000006001000108", expect, sizeof(expect)), 12u);
    ASSERT_TRUE(memcmp(wire, expect, sizeof(wire)) == 0);

    /* Block-size cap drives Z; more than 255 blocks is rejected. */
    ASSERT_EQ(asx_raptorq_oti_init(&oti, 100000u, 100u, 4u, 64u), ASX_OK);
    ASSERT_EQ(oti.source_blocks, 16u);
    ASSERT_EQ(asx_raptorq_oti_init(&oti, 100000u, 100u, 4u, 3u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_oti_init(&oti, 100000u, 100u, 3u, 64u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_oti_init(&oti, 0u, 100u, 4u, 64u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_oti_init(&oti, 100u, 100u, 4u, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_oti_init(NULL, 100u, 100u, 4u, 10u), ASX_E_INVALID_ARGUMENT);

    /* Field-level validation. */
    oti.transfer_length = 5000u;
    oti.symbol_size = 64u;
    oti.source_blocks = 2u;
    oti.sub_blocks = 3u;
    oti.alignment = 8u;
    ASSERT_EQ(asx_raptorq_oti_validate(&oti), ASX_OK);
    back = oti;
    back.transfer_length = 0u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back.transfer_length = ASX_RAPTORQ_MAX_TRANSFER_LENGTH + 1u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.symbol_size = 0u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.alignment = 0u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.alignment = 6u; /* T % Al != 0 */
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.sub_blocks = 0u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back.sub_blocks = 9u; /* N > T / Al */
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.source_blocks = 0u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back.source_blocks = 80u; /* Z > Kt = 79 */
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.transfer_length = (uint64_t)ASX_RAPTORQ_MAX_SOURCE_SYMBOLS * 64u + 1u;
    back.source_blocks = 1u; /* ceil(Kt / Z) > K'max */
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_E_INVALID_ARGUMENT);
    back.source_blocks = 2u;
    ASSERT_EQ(asx_raptorq_oti_validate(&back), ASX_OK);
    ASSERT_EQ(asx_raptorq_oti_validate(NULL), ASX_E_INVALID_ARGUMENT);

    /* Wire decoding fails closed on a reserved octet or invalid fields. */
    ASSERT_EQ(asx_raptorq_oti_serialize(&oti, wire), ASX_OK);
    wire[5] = 1u;
    ASSERT_EQ(asx_raptorq_oti_deserialize(wire, &back), ASX_E_INVALID_ARGUMENT);
    wire[5] = 0u;
    wire[8] = 0u; /* Z = 0 */
    ASSERT_EQ(asx_raptorq_oti_deserialize(wire, &back), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_oti_deserialize(NULL, &back), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_oti_serialize(NULL, wire), ASX_E_INVALID_ARGUMENT);
    back = oti;
    back.symbol_size = 0u;
    ASSERT_EQ(asx_raptorq_oti_serialize(&back, wire), ASX_E_INVALID_ARGUMENT);
}

typedef struct {
    uint32_t sbn;
    uint32_t k;
    uint64_t source_digest; /* FNV-1a over the block's K source symbols */
    uint64_t repair_digest; /* FNV-1a over repair ESIs K .. K + 7 */
} rq_obj_block_golden;

typedef struct {
    uint64_t f;
    uint16_t t;
    uint8_t z;
    uint16_t n;
    uint8_t al;
    uint32_t seed;
    const char *oti_hex;
    const rq_obj_block_golden *blocks;
} rq_obj_golden;

static const rq_obj_block_golden g_obj0[] = {
    {0u, 16u, UINT64_C(0xdf631dd2cee865db), UINT64_C(0xcacf30302c0f2d22)},
    {1u, 16u, UINT64_C(0xc51809c8f17bce73), UINT64_C(0x016816256a6815da)},
    {2u, 16u, UINT64_C(0xe03f4de4dd89d82f), UINT64_C(0x88dcb59b4d8d9334)},
    {3u, 15u, UINT64_C(0x9c51e154cb87c335), UINT64_C(0x78964188da0d6511)}};
static const rq_obj_block_golden g_obj1[] = {{0u, 40u, UINT64_C(0x866ef7d8c0763f89),
                                              UINT64_C(0xffa3e21967db7fb3)},
                                             {1u, 39u, UINT64_C(0xf96edcd01042485b),
                                              UINT64_C(0x6b3c64aac85f7f3e)}};
static const rq_obj_block_golden g_obj2[] = {
    {0u, 98u, UINT64_C(0x8e34276309fdf8f2), UINT64_C(0x685eeda0996143e6)}};
static const rq_obj_block_golden g_obj3[] = {
    {0u, 179u, UINT64_C(0x7a0eebba43ad95db), UINT64_C(0x08659f9cc0428f95)},
    {1u, 179u, UINT64_C(0xb3a2025d2cc644fd), UINT64_C(0x99ad70763ca18519)},
    {2u, 179u, UINT64_C(0x071e0e57fc96604e), UINT64_C(0xa7672906db30f938)},
    {3u, 179u, UINT64_C(0x61df03b251617e6f), UINT64_C(0x1fc41609ada6bfe2)},
    {4u, 178u, UINT64_C(0xeeb126a8f73f8173), UINT64_C(0x2838bdd81c887db3)},
    {5u, 178u, UINT64_C(0xb8ef05df316966ce), UINT64_C(0xd2532d205675e05b)},
    {6u, 178u, UINT64_C(0x7b98c3421c26e804), UINT64_C(0xbbd7a17ed5373e02)}};

static const rq_obj_golden g_obj_golden[] = {
    {1000u, 16u, 4u, 1u, 1u, 11u, "00000003e800001004000101", g_obj0},
    {5000u, 64u, 2u, 3u, 8u, 12u, "000000138800004002000308", g_obj1},
    {777u, 8u, 1u, 1u, 4u, 13u, "000000030900000801000104", g_obj2},
    {40000u, 32u, 7u, 2u, 4u, 14u, "0000009c4000002007000204", g_obj3},
};

/* Encode every block of an object (Z > 1, N >= 1), check against the
 * reference, then decode each block from a lossy symbol set and scatter it
 * back into a fresh object buffer. */
TEST(raptorq_object_multiple_source_blocks_golden_and_roundtrip) {
    size_t ci;
    for (ci = 0u; ci < sizeof(g_obj_golden) / sizeof(g_obj_golden[0]); ci++) {
        const rq_obj_golden *g = &g_obj_golden[ci];
        asx_raptorq_oti oti;
        asx_raptorq_oti parsed;
        uint8_t wire[ASX_RAPTORQ_OTI_SIZE];
        uint8_t expect[ASX_RAPTORQ_OTI_SIZE];
        size_t flen = (size_t)g->f;
        uint8_t *object = (uint8_t *)malloc(flen);
        uint8_t *rebuilt = (uint8_t *)malloc(flen);
        uint32_t sbn;
        uint64_t covered = 0u;

        ASSERT_TRUE(object != NULL && rebuilt != NULL);
        rq_fill(g->seed, object, flen);
        memset(rebuilt, 0xCC, flen);
        oti.transfer_length = g->f;
        oti.symbol_size = g->t;
        oti.source_blocks = g->z;
        oti.sub_blocks = g->n;
        oti.alignment = g->al;
        ASSERT_EQ(asx_raptorq_oti_serialize(&oti, wire), ASX_OK);
        ASSERT_EQ(rq_hex_decode(g->oti_hex, expect, sizeof(expect)), 12u);
        ASSERT_TRUE(memcmp(wire, expect, sizeof(wire)) == 0);
        ASSERT_EQ(asx_raptorq_oti_deserialize(wire, &parsed), ASX_OK);

        for (sbn = 0u; sbn < g->z; sbn++) {
            const rq_obj_block_golden *gb = &g->blocks[sbn];
            asx_raptorq_block_layout lay;
            asx_raptorq_encoder enc;
            asx_raptorq_decoder dec;
            size_t bytes;
            uint8_t *block;
            uint8_t *decoded;
            uint8_t *sym;
            void *ews;
            void *dws;
            uint64_t hr = RQ_FNV_OFFSET;
            uint32_t i;

            ASSERT_EQ(asx_raptorq_oti_block_layout(&parsed, sbn, &lay), ASX_OK);
            ASSERT_EQ(lay.source_symbols, gb->k);
            ASSERT_EQ(lay.offset, covered);
            covered += lay.length;
            bytes = (size_t)lay.source_symbols * g->t;
            block = (uint8_t *)malloc(bytes);
            decoded = (uint8_t *)malloc(bytes);
            sym = (uint8_t *)malloc(g->t);
            ASSERT_TRUE(block != NULL && decoded != NULL && sym != NULL);
            ASSERT_EQ(asx_raptorq_block_gather(&parsed, sbn, object, flen, block, bytes - 1u),
                      ASX_E_BUFFER_TOO_SMALL);
            ASSERT_EQ(asx_raptorq_block_gather(&parsed, sbn, object, flen, block, bytes), ASX_OK);
            ASSERT_EQ(rq_fnv(RQ_FNV_OFFSET, block, bytes), gb->source_digest);

            ews = rq_open_encoder(&enc, lay.source_symbols, g->t, block, bytes);
            dws = rq_open_decoder(&dec, lay.source_symbols, g->t, lay.source_symbols + 2u);
            ASSERT_TRUE(ews != NULL && dws != NULL);
            for (i = 0u; i < 8u; i++) {
                ASSERT_EQ(asx_raptorq_encoder_symbol(&enc, lay.source_symbols + i, sym, g->t),
                          ASX_OK);
                hr = rq_fnv(hr, sym, g->t);
                ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, lay.source_symbols + i, sym, g->t),
                          ASX_OK);
            }
            ASSERT_EQ(hr, gb->repair_digest);
            /* Lose source symbols 0..5: K - 6 sources + 8 repairs = K + 2. */
            for (i = 6u; i < lay.source_symbols; i++) {
                ASSERT_EQ(asx_raptorq_decoder_add_symbol(&dec, i, block + (size_t)i * g->t, g->t),
                          ASX_OK);
            }
            ASSERT_EQ(asx_raptorq_decoder_decode(&dec, decoded, bytes), ASX_OK);
            ASSERT_TRUE(memcmp(decoded, block, bytes) == 0);
            ASSERT_EQ(asx_raptorq_block_scatter(&parsed, sbn, decoded, bytes, rebuilt, flen - 1u),
                      ASX_E_BUFFER_TOO_SMALL);
            ASSERT_EQ(asx_raptorq_block_scatter(&parsed, sbn, decoded, bytes, rebuilt, flen),
                      ASX_OK);
            free(ews);
            free(dws);
            free(block);
            free(decoded);
            free(sym);
        }
        ASSERT_EQ(covered, g->f);
        ASSERT_TRUE(memcmp(rebuilt, object, flen) == 0);
        ASSERT_EQ(asx_raptorq_oti_block_layout(&parsed, g->z, NULL), ASX_E_INVALID_ARGUMENT);
        {
            asx_raptorq_block_layout lay;
            ASSERT_EQ(asx_raptorq_oti_block_layout(&parsed, g->z, &lay), ASX_E_INVALID_ARGUMENT);
        }
        fprintf(stderr, "    object F=%u T=%u Z=%u N=%u Al=%u: golden match, rebuilt ok\n",
                (unsigned)g->f, (unsigned)g->t, (unsigned)g->z, (unsigned)g->n, (unsigned)g->al);
        free(object);
        free(rebuilt);
    }
}

int main(void) {
    fprintf(stderr, "=== test_raptorq (RFC 6330) ===\n");
    RUN_TEST(raptorq_gf256_field_identities);
    RUN_TEST(raptorq_rand_and_deg_reference_values);
    RUN_TEST(raptorq_tuple_reference_values);
    RUN_TEST(raptorq_params_table_spot_checks);
    RUN_TEST(raptorq_golden_repair_symbols_match_reference);
    RUN_TEST(raptorq_golden_k10_repair_symbols_exact);
    RUN_TEST(raptorq_decode_rank_deficient_then_recover_with_more_symbols);
    RUN_TEST(raptorq_decode_reference_repair_only_symbols);
    RUN_TEST(raptorq_roundtrip_random_losses);
    RUN_TEST(raptorq_roundtrip_repair_only);
    RUN_TEST(raptorq_roundtrip_large_block_k8192);
    RUN_TEST(raptorq_decode_all_source_fast_path_and_extra_symbols);
    RUN_TEST(raptorq_decode_fewer_than_k_then_complete);
    RUN_TEST(raptorq_decoder_rejects_bad_symbols);
    RUN_TEST(raptorq_workspace_queries_validate_arguments);
    RUN_TEST(raptorq_undersized_workspace_fails_closed);
    RUN_TEST(raptorq_encoder_argument_validation);
    RUN_TEST(raptorq_partial_final_symbol_is_zero_padded);
    RUN_TEST(raptorq_deterministic_across_workspace_alignment);
    RUN_TEST(raptorq_partition_function);
    RUN_TEST(raptorq_oti_wire_roundtrip_and_validation);
    RUN_TEST(raptorq_object_multiple_source_blocks_golden_and_roundtrip);
    TEST_REPORT();
    return test_failures;
}
