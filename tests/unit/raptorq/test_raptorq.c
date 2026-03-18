/*
 * test_raptorq.c — unit tests for RaptorQ erasure coding (fail-closed stub)
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/raptorq/raptorq.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Config defaults                                                     */
/* ------------------------------------------------------------------ */

TEST(raptorq_config_defaults) {
    asx_raptorq_config cfg;
    asx_raptorq_config_init(&cfg);
    ASSERT_EQ(cfg.symbol_size, 1024u);
    ASSERT_EQ(cfg.source_block_count, 1u);
    ASSERT_EQ(cfg.sub_block_count, 1u);
    ASSERT_EQ(cfg.repair_overhead_pct, 20u);
    ASSERT_NE(cfg.max_source_symbols, 0u);
}

TEST(raptorq_config_null_safe) {
    asx_raptorq_config_init(NULL); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Readiness probe                                                     */
/* ------------------------------------------------------------------ */

TEST(raptorq_not_available) {
    ASSERT_EQ(asx_raptorq_available(), ASX_E_NOT_FOUND);
}

TEST(raptorq_deferral_reason_not_null) {
    const char *reason = asx_raptorq_deferral_reason();
    ASSERT_NE(reason, NULL);
    /* Should mention DEF-009 or Wave D */
    ASSERT_NE(strstr(reason, "DEF-009"), NULL);
}

/* ------------------------------------------------------------------ */
/* Stub encode/decode fail-closed                                      */
/* ------------------------------------------------------------------ */

TEST(raptorq_encode_fails_closed) {
    asx_raptorq_config cfg;
    uint8_t source[64];
    uint8_t symbols[256];
    uint32_t count = 99;

    asx_raptorq_config_init(&cfg);
    memset(source, 0xAB, sizeof(source));

    ASSERT_EQ(asx_raptorq_encode(&cfg, source, 64, symbols, 256, &count), ASX_E_NOT_FOUND);
    ASSERT_EQ(count, 0u); /* output zeroed on failure */
}

TEST(raptorq_decode_fails_closed) {
    asx_raptorq_config cfg;
    uint8_t symbols[256];
    uint8_t output[64];
    uint32_t out_len = 99;

    asx_raptorq_config_init(&cfg);
    memset(symbols, 0xCD, sizeof(symbols));

    ASSERT_EQ(asx_raptorq_decode(&cfg, symbols, 4, output, 64, &out_len), ASX_E_NOT_FOUND);
    ASSERT_EQ(out_len, 0u); /* output zeroed on failure */
}

TEST(raptorq_encode_null_count_safe) {
    asx_raptorq_config cfg;
    asx_raptorq_config_init(&cfg);
    /* NULL out_symbol_count should not crash */
    ASSERT_EQ(asx_raptorq_encode(&cfg, "data", 4, NULL, 0, NULL), ASX_E_NOT_FOUND);
}

TEST(raptorq_decode_null_len_safe) {
    asx_raptorq_config cfg;
    asx_raptorq_config_init(&cfg);
    ASSERT_EQ(asx_raptorq_decode(&cfg, "syms", 1, NULL, 0, NULL), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_raptorq ===\n");

    RUN_TEST(raptorq_config_defaults);
    RUN_TEST(raptorq_config_null_safe);
    RUN_TEST(raptorq_not_available);
    RUN_TEST(raptorq_deferral_reason_not_null);
    RUN_TEST(raptorq_encode_fails_closed);
    RUN_TEST(raptorq_decode_fails_closed);
    RUN_TEST(raptorq_encode_null_count_safe);
    RUN_TEST(raptorq_decode_null_len_safe);

    TEST_REPORT();
    return test_failures;
}
