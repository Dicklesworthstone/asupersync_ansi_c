/*
 * test_raptorq.c — unit tests for XOR-based erasure coding
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/raptorq/raptorq.h>
#include <string.h>

#define RAPTORQ_TEST_MAX_SOURCE_BYTES 4096u
#define RAPTORQ_TEST_MAX_SYMBOL_BYTES 8192u
#define RAPTORQ_TEST_MAX_PRESENT 8u

static void raptorq_fill_source(uint8_t *buf, uint32_t len, uint8_t seed) {
    uint32_t i;
    for (i = 0u; i < len; i++) { buf[i] = (uint8_t)(seed + (uint8_t)i); }
}

static void raptorq_init_case(asx_raptorq_config *cfg, uint32_t symbol_size,
                              uint32_t repair_overhead_pct) {
    asx_raptorq_config_init(cfg);
    cfg->symbol_size = symbol_size;
    cfg->repair_overhead_pct = repair_overhead_pct;
}

static void raptorq_set_present_all(uint8_t *present, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; i++) { present[i] = 1u; }
}

static int raptorq_buffers_match(const uint8_t *a, const uint8_t *b, uint32_t len) {
    return memcmp(a, b, len) == 0;
}

TEST(raptorq_config_defaults) {
    asx_raptorq_config cfg;
    asx_raptorq_config_init(&cfg);
    ASSERT_EQ(cfg.symbol_size, 1024u);
    ASSERT_EQ(cfg.source_block_count, 1u);
    ASSERT_EQ(cfg.sub_block_count, 1u);
    ASSERT_EQ(cfg.repair_overhead_pct, 20u);
    ASSERT_NE(cfg.max_source_symbols, 0u);
}

TEST(raptorq_config_null_safe) { asx_raptorq_config_init(NULL); }

TEST(raptorq_available) { ASSERT_EQ(asx_raptorq_available(), ASX_OK); }

TEST(raptorq_deferral_reason_null) { ASSERT_TRUE(asx_raptorq_deferral_reason() == NULL); }

TEST(raptorq_encode_basic) {
    asx_raptorq_config cfg;
    uint8_t source[64];
    uint8_t symbols[8192];
    uint32_t count = 0;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 32;
    memset(source, 0xAB, sizeof(source));

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, 64, symbols, sizeof(symbols), &count), ASX_OK);
    /* 64 bytes / 32 per symbol = 2 source + repair symbols */
    ASSERT_TRUE(count >= 2u);
    /* First source symbol should contain our data */
    ASSERT_EQ(symbols[0], 0xAB);
}

TEST(raptorq_encode_decode_roundtrip) {
    asx_raptorq_config cfg;
    uint8_t source[] = "Hello, erasure coding!";
    uint8_t symbols[4096];
    uint8_t decoded[256];
    uint32_t sym_count = 0;
    uint32_t decoded_len = 0;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 16;

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, (uint32_t)strlen((const char *)source), symbols,
                                 sizeof(symbols), &sym_count),
              ASX_OK);
    ASSERT_TRUE(sym_count > 0u);

    /* Decode — source symbols are first, so direct copy recovers data */
    ASSERT_EQ(asx_raptorq_decode(&cfg, symbols, sym_count, decoded, sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_TRUE(decoded_len > 0u);
    ASSERT_TRUE(memcmp(decoded, source, strlen((const char *)source)) == 0);
}

TEST(raptorq_encode_null_args) {
    asx_raptorq_config cfg;
    uint8_t buf[64];
    uint32_t count;

    asx_raptorq_config_init(&cfg);
    ASSERT_EQ(asx_raptorq_encode(NULL, buf, 4, buf, 64, &count), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encode(&cfg, NULL, 4, buf, 64, &count), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encode(&cfg, buf, 0, buf, 64, &count), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_encode(&cfg, buf, 4, buf, 64, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(raptorq_decode_null_args) {
    asx_raptorq_config cfg;
    uint8_t buf[64];
    uint32_t len;

    asx_raptorq_config_init(&cfg);
    ASSERT_EQ(asx_raptorq_decode(NULL, buf, 1, buf, 64, &len), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode(&cfg, NULL, 1, buf, 64, &len), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode(&cfg, buf, 0, buf, 64, &len), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode(&cfg, buf, 1, buf, 64, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(raptorq_decode_with_erasures_null_args) {
    asx_raptorq_config cfg;
    uint8_t buf[64];
    uint8_t present[4] = {1u, 1u, 1u, 1u};
    uint32_t len;

    asx_raptorq_config_init(&cfg);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(NULL, buf, 1, present, buf, 64, &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, NULL, 1, present, buf, 64, &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, buf, 1, NULL, buf, 64, &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, buf, 1, present, NULL, 64, &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, buf, 0, present, buf, 64, &len),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, buf, 1, present, buf, 64, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(raptorq_decode_with_erasures_fast_path_when_all_sources_present) {
    asx_raptorq_config cfg;
    uint8_t source[] = "Hello, erasure coding!";
    uint8_t symbols[4096];
    uint8_t decoded[256];
    uint8_t present[8];
    uint32_t sym_count = 0;
    uint32_t decoded_len = 0;
    uint32_t i;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 16;

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, (uint32_t)strlen((const char *)source), symbols,
                                 sizeof(symbols), &sym_count),
              ASX_OK);
    ASSERT_TRUE(sym_count <= 8u);
    for (i = 0u; i < sym_count; i++) { present[i] = 1u; }

    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_EQ(decoded_len, 32u);
    ASSERT_TRUE(memcmp(decoded, source, strlen((const char *)source)) == 0);
}

TEST(raptorq_decode_with_erasures_early_failure_when_not_enough_symbols_present) {
    asx_raptorq_config cfg;
    uint8_t source[64];
    uint8_t symbols[4096];
    uint8_t decoded[256];
    uint8_t present[8] = {0};
    uint32_t sym_count = 0;
    uint32_t decoded_len = 0;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 32;
    memset(source, 0xCD, sizeof(source));

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    ASSERT_EQ(sym_count, 3u);

    present[0] = 1u;
    present[1] = 0u;
    present[2] = 0u;

    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(raptorq_decode_with_erasures_recovers_single_missing_source) {
    asx_raptorq_config cfg;
    uint8_t source[128];
    uint8_t symbols[4096];
    uint8_t decoded[256];
    uint8_t present[8] = {0};
    uint32_t sym_count = 0;
    uint32_t decoded_len = 0;
    uint32_t i;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 32;
    cfg.repair_overhead_pct = 50;
    for (i = 0u; i < sizeof(source); i++) { source[i] = (uint8_t)(i + 1u); }

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    ASSERT_EQ(sym_count, 6u);

    present[0] = 0u;
    present[1] = 1u;
    present[2] = 1u;
    present[3] = 1u;
    present[4] = 1u;
    present[5] = 1u;

    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_EQ(decoded_len, sizeof(source));
    ASSERT_TRUE(memcmp(decoded, source, sizeof(source)) == 0);
}

TEST(raptorq_decode_with_erasures_recovers_max_two_losses_for_two_repairs) {
    asx_raptorq_config cfg;
    uint8_t source[128];
    uint8_t symbols[4096];
    uint8_t decoded[256];
    uint8_t present[8] = {0};
    uint32_t sym_count = 0;
    uint32_t decoded_len = 0;
    uint32_t i;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 32;
    cfg.repair_overhead_pct = 50;
    for (i = 0u; i < sizeof(source); i++) { source[i] = (uint8_t)(0xA0u + i); }

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    ASSERT_EQ(sym_count, 6u);

    present[0] = 0u;
    present[1] = 0u;
    present[2] = 1u;
    present[3] = 1u;
    present[4] = 1u;
    present[5] = 1u;

    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_EQ(decoded_len, sizeof(source));
    ASSERT_TRUE(memcmp(decoded, source, sizeof(source)) == 0);
}

TEST(raptorq_decode_with_erasures_fails_when_peeling_gets_stuck) {
    asx_raptorq_config cfg;
    uint8_t source[128];
    uint8_t symbols[4096];
    uint8_t decoded[256];
    uint8_t present[8] = {0};
    uint32_t sym_count = 0;
    uint32_t decoded_len = 0;
    uint32_t i;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 32;
    cfg.repair_overhead_pct = 50;
    for (i = 0u; i < sizeof(source); i++) { source[i] = (uint8_t)(0xF0u - i); }

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    ASSERT_EQ(sym_count, 6u);

    present[0] = 0u;
    present[1] = 1u;
    present[2] = 0u;
    present[3] = 1u;
    present[4] = 1u;
    present[5] = 1u;

    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(raptorq_decode_with_erasures_all_symbols_present_parameterized) {
    static const uint32_t symbol_sizes[] = {64u, 1024u};
    static const uint32_t overheads[] = {20u, 50u};
    asx_raptorq_config cfg;
    uint8_t source[RAPTORQ_TEST_MAX_SOURCE_BYTES];
    uint8_t symbols[RAPTORQ_TEST_MAX_SYMBOL_BYTES];
    uint8_t decoded[RAPTORQ_TEST_MAX_SOURCE_BYTES];
    uint8_t present[RAPTORQ_TEST_MAX_PRESENT];
    uint32_t si;
    uint32_t oi;

    for (si = 0u; si < 2u; si++) {
        for (oi = 0u; oi < 2u; oi++) {
            uint32_t source_len = symbol_sizes[si] * 4u;
            uint32_t sym_count = 0u;
            uint32_t decoded_len = 0u;

            raptorq_init_case(&cfg, symbol_sizes[si], overheads[oi]);
            raptorq_fill_source(source, source_len, (uint8_t)(0x10u + si * 8u + oi));
            ASSERT_EQ(asx_raptorq_encode(&cfg, source, source_len, symbols, sizeof(symbols),
                                         &sym_count),
                      ASX_OK);
            raptorq_set_present_all(present, sym_count);

            fprintf(stderr, "    all_present: symbol_size=%u overhead=%u sym_count=%u\n",
                    symbol_sizes[si], overheads[oi], sym_count);
            ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                                       sizeof(decoded), &decoded_len),
                      ASX_OK);
            ASSERT_EQ(decoded_len, source_len);
            ASSERT_TRUE(raptorq_buffers_match(decoded, source, source_len));
        }
    }
}

TEST(raptorq_decode_with_erasures_repair_only_loss_succeeds) {
    asx_raptorq_config cfg;
    uint8_t source[256];
    uint8_t symbols[RAPTORQ_TEST_MAX_SYMBOL_BYTES];
    uint8_t decoded[256];
    uint8_t present[RAPTORQ_TEST_MAX_PRESENT];
    uint32_t sym_count = 0u;
    uint32_t decoded_len = 0u;

    raptorq_init_case(&cfg, 64u, 50u);
    raptorq_fill_source(source, sizeof(source), 0x33u);
    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    raptorq_set_present_all(present, sym_count);
    present[4] = 0u;
    present[5] = 0u;

    fprintf(stderr, "    repair_only_loss: source_len=%zu sym_count=%u pattern=[1,1,1,1,0,0]\n",
            sizeof(source), sym_count);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_EQ(decoded_len, sizeof(source));
    ASSERT_TRUE(raptorq_buffers_match(decoded, source, sizeof(source)));
}

TEST(raptorq_decode_with_erasures_mixed_loss_recovers_when_equation_survives) {
    asx_raptorq_config cfg;
    uint8_t source[256];
    uint8_t symbols[RAPTORQ_TEST_MAX_SYMBOL_BYTES];
    uint8_t decoded[256];
    uint8_t present[RAPTORQ_TEST_MAX_PRESENT];
    uint32_t sym_count = 0u;
    uint32_t decoded_len = 0u;

    raptorq_init_case(&cfg, 64u, 50u);
    raptorq_fill_source(source, sizeof(source), 0x55u);
    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    raptorq_set_present_all(present, sym_count);
    present[0] = 0u;
    present[5] = 0u;

    fprintf(stderr,
            "    mixed_loss_recoverable: source_len=%zu sym_count=%u pattern=[0,1,1,1,1,0]\n",
            sizeof(source), sym_count);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_EQ(decoded_len, sizeof(source));
    ASSERT_TRUE(raptorq_buffers_match(decoded, source, sizeof(source)));
}

TEST(raptorq_decode_with_erasures_unrecoverable_when_sources_exceed_repairs) {
    asx_raptorq_config cfg;
    uint8_t source[256];
    uint8_t symbols[RAPTORQ_TEST_MAX_SYMBOL_BYTES];
    uint8_t decoded[256];
    uint8_t present[RAPTORQ_TEST_MAX_PRESENT];
    uint32_t sym_count = 0u;
    uint32_t decoded_len = 0u;

    raptorq_init_case(&cfg, 64u, 50u);
    raptorq_fill_source(source, sizeof(source), 0x77u);
    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    raptorq_set_present_all(present, sym_count);
    present[0] = 0u;
    present[1] = 0u;
    present[2] = 0u;

    fprintf(stderr, "    unrecoverable_loss: source_len=%zu sym_count=%u pattern=[0,0,0,1,1,1]\n",
            sizeof(source), sym_count);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_E_RESOURCE_EXHAUSTED);
}

TEST(raptorq_decode_with_erasures_single_symbol_recovers_from_repair) {
    asx_raptorq_config cfg;
    uint8_t source[1] = {0xA5u};
    uint8_t symbols[8];
    uint8_t decoded[4];
    uint8_t present[2] = {0u, 1u};
    uint32_t sym_count = 0u;
    uint32_t decoded_len = 0u;

    raptorq_init_case(&cfg, 1u, 20u);
    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    ASSERT_EQ(sym_count, 2u);

    fprintf(stderr, "    single_symbol: sym_count=%u pattern=[0,1]\n", sym_count);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded,
                                               sizeof(decoded), &decoded_len),
              ASX_OK);
    ASSERT_EQ(decoded_len, 1u);
    ASSERT_EQ(decoded[0], source[0]);
}

TEST(raptorq_decode_with_erasures_repeated_runs_are_deterministic) {
    asx_raptorq_config cfg;
    uint8_t source[256];
    uint8_t symbols[RAPTORQ_TEST_MAX_SYMBOL_BYTES];
    uint8_t decoded_a[256];
    uint8_t decoded_b[256];
    uint8_t present[RAPTORQ_TEST_MAX_PRESENT];
    uint32_t sym_count = 0u;
    uint32_t decoded_len_a = 0u;
    uint32_t decoded_len_b = 0u;

    raptorq_init_case(&cfg, 64u, 50u);
    raptorq_fill_source(source, sizeof(source), 0x21u);
    ASSERT_EQ(asx_raptorq_encode(&cfg, source, sizeof(source), symbols, sizeof(symbols),
                                 &sym_count),
              ASX_OK);
    raptorq_set_present_all(present, sym_count);
    present[1] = 0u;
    present[4] = 0u;

    fprintf(stderr, "    determinism: sym_count=%u pattern=[1,0,1,1,0,1]\n", sym_count);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded_a,
                                               sizeof(decoded_a), &decoded_len_a),
              ASX_OK);
    ASSERT_EQ(asx_raptorq_decode_with_erasures(&cfg, symbols, sym_count, present, decoded_b,
                                               sizeof(decoded_b), &decoded_len_b),
              ASX_OK);
    ASSERT_EQ(decoded_len_a, decoded_len_b);
    ASSERT_TRUE(raptorq_buffers_match(decoded_a, decoded_b, decoded_len_a));
    ASSERT_TRUE(raptorq_buffers_match(decoded_a, source, sizeof(source)));
}

TEST(raptorq_encode_buffer_too_small) {
    asx_raptorq_config cfg;
    uint8_t source[64];
    uint8_t symbols[4];
    uint32_t count;

    asx_raptorq_config_init(&cfg);
    cfg.symbol_size = 32;
    memset(source, 0, sizeof(source));

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, 64, symbols, sizeof(symbols), &count),
              ASX_E_BUFFER_TOO_SMALL);
}

int main(void) {
    fprintf(stderr, "=== test_raptorq ===\n");
    RUN_TEST(raptorq_config_defaults);
    RUN_TEST(raptorq_config_null_safe);
    RUN_TEST(raptorq_available);
    RUN_TEST(raptorq_deferral_reason_null);
    RUN_TEST(raptorq_encode_basic);
    RUN_TEST(raptorq_encode_decode_roundtrip);
    RUN_TEST(raptorq_encode_null_args);
    RUN_TEST(raptorq_decode_null_args);
    RUN_TEST(raptorq_decode_with_erasures_null_args);
    RUN_TEST(raptorq_decode_with_erasures_fast_path_when_all_sources_present);
    RUN_TEST(raptorq_decode_with_erasures_early_failure_when_not_enough_symbols_present);
    RUN_TEST(raptorq_decode_with_erasures_recovers_single_missing_source);
    RUN_TEST(raptorq_decode_with_erasures_recovers_max_two_losses_for_two_repairs);
    RUN_TEST(raptorq_decode_with_erasures_fails_when_peeling_gets_stuck);
    RUN_TEST(raptorq_decode_with_erasures_all_symbols_present_parameterized);
    RUN_TEST(raptorq_decode_with_erasures_repair_only_loss_succeeds);
    RUN_TEST(raptorq_decode_with_erasures_mixed_loss_recovers_when_equation_survives);
    RUN_TEST(raptorq_decode_with_erasures_unrecoverable_when_sources_exceed_repairs);
    RUN_TEST(raptorq_decode_with_erasures_single_symbol_recovers_from_repair);
    RUN_TEST(raptorq_decode_with_erasures_repeated_runs_are_deterministic);
    RUN_TEST(raptorq_encode_buffer_too_small);
    TEST_REPORT();
    return test_failures;
}
