/*
 * e2e_raptorq_erasure.c — end-to-end RFC 6330 erasure recovery scenario
 *
 * Exercises the full object pipeline: OTI derivation and wire round trip,
 * source-block gather, RaptorQ encoding (source + repair symbols),
 * deterministic symbol loss, decoding from whatever symbols survive, and
 * scatter back into the object. With RFC 6330 any K received symbols
 * (source or repair, any mix) recover a block with high probability; with
 * fewer than K the decoder reports ASX_E_RESOURCE_EXHAUSTED and recovers
 * once more symbols arrive.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static int g_expected_fail = 0;

#define LOG(fmt, ...) fprintf(stderr, "  [raptorq-e2e] " fmt "\n", __VA_ARGS__)
#define LOG0(msg) fprintf(stderr, "  [raptorq-e2e] " msg "\n")
#define SCENARIO(name) fprintf(stderr, "\n=== SCENARIO: %s ===\n", name)

#define E2E_MAX_SYMBOLS 4096u

static void fill_deterministic(uint8_t *buf, size_t len, uint32_t seed) {
    size_t i;
    uint32_t state = seed;
    for (i = 0; i < len; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (uint8_t)((state >> 16) & 0xFFu);
    }
}

static uint64_t fnv1a(const uint8_t *data, size_t len) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* Erasure pattern over the K source + R repair symbols of one block:
 * present[i] == 0 means symbol i (ESI i) was lost. */
typedef void (*erase_fn)(uint8_t *present, uint32_t k, uint32_t total);

static void erase_none(uint8_t *present, uint32_t k, uint32_t total) {
    (void)present;
    (void)k;
    (void)total;
}

static void erase_1_source(uint8_t *present, uint32_t k, uint32_t total) {
    (void)k;
    (void)total;
    present[0] = 0u;
}

static void erase_3_sources(uint8_t *present, uint32_t k, uint32_t total) {
    (void)total;
    present[0] = 0u;
    if (k > 2u) present[2] = 0u;
    if (k > 4u) present[4] = 0u;
}

static void erase_all_repair(uint8_t *present, uint32_t k, uint32_t total) {
    uint32_t i;
    for (i = k; i < total; i++) present[i] = 0u;
}

static void erase_all_source(uint8_t *present, uint32_t k, uint32_t total) {
    uint32_t i;
    (void)total;
    for (i = 0; i < k; i++) present[i] = 0u;
}

static void erase_every_other(uint8_t *present, uint32_t k, uint32_t total) {
    uint32_t i;
    (void)k;
    for (i = 0; i < total; i += 2u) present[i] = 0u;
}

static void erase_periodic_3(uint8_t *present, uint32_t k, uint32_t total) {
    uint32_t i;
    (void)k;
    for (i = 0; i < total; i += 3u) present[i] = 0u;
}

/* Too much loss: keep only K - 1 symbols. */
static void erase_to_k_minus_1(uint8_t *present, uint32_t k, uint32_t total) {
    uint32_t i;
    uint32_t kept = 0u;
    for (i = 0; i < total; i++) {
        if (kept + 1u < k) {
            kept++;
        } else {
            present[i] = 0u;
        }
    }
}

/*
 * Encode an object of `data_len` bytes with symbol size `sym_size` and at
 * most `max_block` symbols per source block, emit `repair` repair symbols
 * per block, apply `erase` to every block and decode.
 */
static int run_erasure_scenario(const char *name, uint32_t data_len, uint16_t sym_size,
                                uint32_t max_block, uint32_t repair, erase_fn erase,
                                int expect_recovery, uint64_t *out_digest) {
    asx_raptorq_oti oti;
    asx_raptorq_oti rx_oti;
    uint8_t wire[ASX_RAPTORQ_OTI_SIZE];
    uint8_t *source = NULL;
    uint8_t *rebuilt = NULL;
    uint8_t present[E2E_MAX_SYMBOLS];
    uint32_t sbn;
    uint32_t total_erased = 0u;
    uint32_t total_symbols = 0u;
    asx_status st;
    int ok = 1;

    SCENARIO(name);
    source = (uint8_t *)malloc(data_len);
    rebuilt = (uint8_t *)malloc(data_len);
    if (source == NULL || rebuilt == NULL) {
        LOG0("FAIL: allocation");
        g_fail++;
        free(source);
        free(rebuilt);
        return 0;
    }
    fill_deterministic(source, data_len, 0xDEADBEEFu);
    memset(rebuilt, 0, data_len);
    LOG("source: %u bytes, seed=0xDEADBEEF", data_len);

    /* Sender derives the OTI; receiver parses it from the 12-byte wire form. */
    st = asx_raptorq_oti_init(&oti, data_len, sym_size, 1u, max_block);
    if (st == ASX_OK) st = asx_raptorq_oti_serialize(&oti, wire);
    if (st == ASX_OK) st = asx_raptorq_oti_deserialize(wire, &rx_oti);
    if (st != ASX_OK) {
        LOG("FAIL: OTI setup returned %s", asx_status_str(st));
        g_fail++;
        free(source);
        free(rebuilt);
        return 0;
    }
    LOG("OTI: F=%u T=%u Z=%u N=%u Al=%u", (unsigned)rx_oti.transfer_length,
        (unsigned)rx_oti.symbol_size, (unsigned)rx_oti.source_blocks, (unsigned)rx_oti.sub_blocks,
        (unsigned)rx_oti.alignment);

    for (sbn = 0u; sbn < rx_oti.source_blocks && ok; sbn++) {
        asx_raptorq_block_layout lay;
        asx_raptorq_encoder enc;
        asx_raptorq_decoder dec;
        size_t ews_size = 0u;
        size_t dws_size = 0u;
        void *ews = NULL;
        void *dws = NULL;
        uint8_t *block = NULL;
        uint8_t *decoded = NULL;
        uint8_t *sym = NULL;
        uint32_t k;
        uint32_t total;
        uint32_t i;
        size_t bytes;

        st = asx_raptorq_oti_block_layout(&rx_oti, sbn, &lay);
        if (st != ASX_OK) {
            ok = 0;
            break;
        }
        k = lay.source_symbols;
        total = k + repair;
        if (total > E2E_MAX_SYMBOLS) {
            LOG("FAIL: block %u needs %u symbols", sbn, total);
            ok = 0;
            break;
        }
        bytes = (size_t)k * sym_size;
        block = (uint8_t *)malloc(bytes);
        decoded = (uint8_t *)malloc(bytes);
        sym = (uint8_t *)malloc(sym_size);
        st = asx_raptorq_encoder_workspace_size(k, sym_size, &ews_size);
        if (st == ASX_OK) st = asx_raptorq_decoder_workspace_size(k, sym_size, total, &dws_size);
        if (st == ASX_OK) {
            ews = malloc(ews_size);
            dws = malloc(dws_size);
        }
        if (st != ASX_OK || block == NULL || decoded == NULL || sym == NULL || ews == NULL ||
            dws == NULL) {
            LOG("FAIL: block %u setup (%s)", sbn, asx_status_str(st));
            ok = 0;
        }
        if (ok) st = asx_raptorq_block_gather(&rx_oti, sbn, source, data_len, block, bytes);
        if (ok && st == ASX_OK) {
            st = asx_raptorq_encoder_init(&enc, k, sym_size, block, bytes, ews, ews_size);
        }
        if (ok && st == ASX_OK)
            st = asx_raptorq_decoder_init(&dec, k, sym_size, total, dws, dws_size);
        if (ok && st != ASX_OK) {
            LOG("FAIL: block %u encode setup returned %s", sbn, asx_status_str(st));
            ok = 0;
        }

        if (ok) {
            uint32_t received = 0u;
            memset(present, 1, total);
            erase(present, k, total);
            for (i = 0u; i < total && ok; i++) {
                if (!present[i]) {
                    total_erased++;
                    continue;
                }
                st = asx_raptorq_encoder_symbol(&enc, i, sym, sym_size);
                if (st == ASX_OK) st = asx_raptorq_decoder_add_symbol(&dec, i, sym, sym_size);
                if (st != ASX_OK) {
                    LOG("FAIL: symbol %u transfer returned %s", i, asx_status_str(st));
                    ok = 0;
                }
                received++;
            }
            total_symbols += total;
            LOG("block %u: K=%u, %u source + %u repair emitted, %u received", sbn, k, k, repair,
                received);
        }

        if (ok) {
            st = asx_raptorq_decoder_decode(&dec, decoded, bytes);
            if (st == ASX_E_RESOURCE_EXHAUSTED && !expect_recovery) {
                /* Late arrivals: feed fresh repair symbols until recovery. */
                uint32_t extra = 0u;
                LOG("block %u: decode deferred (%s), requesting more symbols", sbn,
                    asx_status_str(st));
                while (st == ASX_E_RESOURCE_EXHAUSTED && extra < 4u) {
                    uint32_t esi = total + 1000u + extra;
                    asx_status st2 = asx_raptorq_encoder_symbol(&enc, esi, sym, sym_size);
                    if (st2 == ASX_OK)
                        st2 = asx_raptorq_decoder_add_symbol(&dec, esi, sym, sym_size);
                    if (st2 != ASX_OK) break;
                    extra++;
                    st = asx_raptorq_decoder_decode(&dec, decoded, bytes);
                }
                if (st == ASX_OK) {
                    LOG("block %u: recovered after %u late repair symbol(s)", sbn, extra);
                    g_expected_fail++;
                }
            }
            if (st == ASX_OK) {
                st = asx_raptorq_block_scatter(&rx_oti, sbn, decoded, bytes, rebuilt, data_len);
            }
            if (st != ASX_OK) {
                LOG("FAIL: block %u decode returned %s", sbn, asx_status_str(st));
                ok = 0;
            }
        }
        free(ews);
        free(dws);
        free(block);
        free(decoded);
        free(sym);
    }

    if (ok && memcmp(source, rebuilt, data_len) != 0) {
        uint32_t j;
        LOG0("FAIL: decoded data does not match source");
        for (j = 0; j < data_len; j++) {
            if (source[j] != rebuilt[j]) {
                LOG("  first mismatch at byte %u: expected 0x%02X got 0x%02X", j, source[j],
                    rebuilt[j]);
                break;
            }
        }
        ok = 0;
    }
    if (ok) {
        if (out_digest != NULL) *out_digest = fnv1a(rebuilt, data_len);
        LOG("PASS: %u/%u symbols erased, %u bytes recovered byte-exact", total_erased,
            total_symbols, data_len);
        g_pass++;
    } else {
        g_fail++;
    }
    free(source);
    free(rebuilt);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    uint64_t digest_a = 0u;
    uint64_t digest_b = 0u;

    fprintf(stderr, "=== e2e_raptorq_erasure: RFC 6330 encode-erase-decode pipeline ===\n\n");

    run_erasure_scenario("no-loss baseline (1024B, T=128, 2 repair)", 1024u, 128u, 64u, 2u,
                         erase_none, 1, NULL);
    run_erasure_scenario("single source loss (1024B, T=128, 2 repair)", 1024u, 128u, 64u, 2u,
                         erase_1_source, 1, NULL);
    run_erasure_scenario("3 source losses (1024B, T=128, 5 repair)", 1024u, 128u, 64u, 5u,
                         erase_3_sources, 1, NULL);
    run_erasure_scenario("repair-only loss (1024B, T=128, 2 repair)", 1024u, 128u, 64u, 2u,
                         erase_all_repair, 1, NULL);
    run_erasure_scenario("all sources lost, repair-only decode (2048B, T=64, 36 repair)", 2048u,
                         64u, 64u, 36u, erase_all_source, 1, NULL);
    run_erasure_scenario("heavy 50% loss (4000B, T=40, 104 repair)", 4000u, 40u, 1000u, 104u,
                         erase_every_other, 1, NULL);
    run_erasure_scenario("periodic/3 erasure (512B, T=16, 20 repair)", 512u, 16u, 64u, 20u,
                         erase_periodic_3, 1, NULL);
    run_erasure_scenario("multi-block object Z>1 (20000B, T=64, <=100 symbols/block)", 20000u, 64u,
                         100u, 12u, erase_3_sources, 1, NULL);
    run_erasure_scenario("insufficient symbols then late arrivals (1024B, T=32)", 1024u, 32u, 64u,
                         8u, erase_to_k_minus_1, 0, NULL);
    run_erasure_scenario("determinism-A (3000B, T=48, 40 repair)", 3000u, 48u, 1000u, 40u,
                         erase_periodic_3, 1, &digest_a);
    run_erasure_scenario("determinism-B (3000B, T=48, 40 repair)", 3000u, 48u, 1000u, 40u,
                         erase_periodic_3, 1, &digest_b);
    if (digest_a != digest_b) {
        LOG0("FAIL: determinism digests differ");
        g_fail++;
    }

    fprintf(stderr, "\n=================================================================\n");
    fprintf(stderr, " RaptorQ E2E Erasure Summary\n");
    fprintf(stderr, "=================================================================\n");
    fprintf(stderr, "  scenarios:       %d\n", g_pass + g_fail);
    fprintf(stderr, "  passed:          %d\n", g_pass);
    fprintf(stderr, "  failed:          %d\n", g_fail);
    fprintf(stderr, "  deferred_decode: %d\n", g_expected_fail);
    fprintf(stderr, "=================================================================\n");

    if (g_fail > 0) {
        fprintf(stderr, "FAIL: %d unexpected failure(s)\n", g_fail);
        return 1;
    }
    fprintf(stderr, "PASS: all scenarios passed\n");
    return 0;
}
