/*
 * test_raptorq.c — unit tests for XOR-based erasure coding
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/raptorq/raptorq.h>
#include <string.h>

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
    RUN_TEST(raptorq_encode_buffer_too_small);
    TEST_REPORT();
    return test_failures;
}
