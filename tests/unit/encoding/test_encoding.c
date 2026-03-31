/*
 * test_encoding.c — unit tests for encoding pipeline
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <asx/bytes/codec.h>
#include <asx/bytes/io_adapter.h>
#include <asx/encoding/encoding.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Error kind string tests                                             */
/* ------------------------------------------------------------------ */

TEST(encoding_error_kind_str) {
    ASSERT_STR_EQ(asx_encoding_error_kind_str(ASX_ENCODING_ERR_NONE), "none");
    ASSERT_STR_EQ(asx_encoding_error_kind_str(ASX_ENCODING_ERR_CODEC), "codec");
    ASSERT_STR_EQ(asx_encoding_error_kind_str(ASX_ENCODING_ERR_IO), "io");
    ASSERT_STR_EQ(asx_encoding_error_kind_str(ASX_ENCODING_ERR_OVERFLOW), "overflow");
    ASSERT_STR_EQ(asx_encoding_error_kind_str(ASX_ENCODING_ERR_CANCELLED), "cancelled");
    ASSERT_STR_EQ(asx_encoding_error_kind_str((asx_encoding_error_kind)99), "unknown");
}

/* ------------------------------------------------------------------ */
/* Config tests                                                        */
/* ------------------------------------------------------------------ */

TEST(encoding_config_defaults) {
    asx_encoding_config cfg;
    asx_encoding_config_init(&cfg);
    ASSERT_EQ(cfg.max_frame_bytes, 0u);
    ASSERT_EQ(cfg.max_total_bytes, 0u);
    ASSERT_EQ(cfg.flush_after_each, 0);
}

TEST(encoding_config_null_safe) { asx_encoding_config_init(NULL); /* should not crash */ }

/* ------------------------------------------------------------------ */
/* Pipeline init tests                                                 */
/* ------------------------------------------------------------------ */

TEST(encoding_pipeline_init_null) {
    ASSERT_EQ(asx_encoding_pipeline_init(NULL, (asx_codec){0}, (asx_write_adapter){0}, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(encoding_pipeline_init_ok) {
    asx_encoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    ASSERT_EQ(asx_encoding_pipeline_init(&p, codec, writer, NULL), ASX_OK);
    ASSERT_EQ(p.stats.frames_encoded, 0u);
    ASSERT_EQ(p.last_error, ASX_ENCODING_ERR_NONE);
    ASSERT_EQ(p.cancelled, 0);
}

/* ------------------------------------------------------------------ */
/* Encode single frame                                                 */
/* ------------------------------------------------------------------ */

TEST(encoding_encode_single_frame) {
    asx_encoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoded_symbol sym;
    asx_encoding_stats stats;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "hello", 5, &sym), ASX_OK);

    ASSERT_EQ(sym.len, 9u); /* 4 byte length + 5 byte payload */
    ASSERT_EQ(sym.sequence, 1u);

    asx_encoding_pipeline_stats(&p, &stats);
    ASSERT_EQ(stats.frames_encoded, 1u);
    ASSERT_EQ(stats.bytes_input, 5u);
    ASSERT_EQ(stats.bytes_output, 9u);
    ASSERT_EQ(stats.frames_rejected, 0u);

    /* Verify data landed in sink */
    ASSERT_EQ(asx_buf_mut_remaining(&sink), 9u);
}

/* ------------------------------------------------------------------ */
/* Encode multiple frames                                              */
/* ------------------------------------------------------------------ */

TEST(encoding_encode_multiple_frames) {
    asx_encoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoded_symbol sym;
    asx_encoding_stats stats;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));

    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "aa", 2, &sym), ASX_OK);
    ASSERT_EQ(sym.sequence, 1u);

    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "bbb", 3, &sym), ASX_OK);
    ASSERT_EQ(sym.sequence, 2u);

    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "c", 1, &sym), ASX_OK);
    ASSERT_EQ(sym.sequence, 3u);

    asx_encoding_pipeline_stats(&p, &stats);
    ASSERT_EQ(stats.frames_encoded, 3u);
    ASSERT_EQ(stats.bytes_input, 6u);   /* 2 + 3 + 1 */
    ASSERT_EQ(stats.bytes_output, 18u); /* (4+2) + (4+3) + (4+1) */
}

/* ------------------------------------------------------------------ */
/* Encode empty frame                                                  */
/* ------------------------------------------------------------------ */

TEST(encoding_encode_empty_frame) {
    asx_encoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoded_symbol sym;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, NULL, 0, &sym), ASX_OK);
    ASSERT_EQ(sym.len, 4u); /* just length field */
}

/* ------------------------------------------------------------------ */
/* Max frame bytes limit                                               */
/* ------------------------------------------------------------------ */

TEST(encoding_max_frame_bytes) {
    asx_encoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoding_config cfg;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    asx_encoding_config_init(&cfg);
    cfg.max_frame_bytes = 3;

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, &cfg));

    /* Should succeed for small frame */
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "ab", 2, NULL), ASX_OK);

    /* Should fail for oversized frame */
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "toolong", 7, NULL), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_encoding_pipeline_last_error(&p), ASX_ENCODING_ERR_OVERFLOW);
    ASSERT_EQ(p.stats.frames_rejected, 1u);
}

/* ------------------------------------------------------------------ */
/* Max total bytes limit                                               */
/* ------------------------------------------------------------------ */

TEST(encoding_max_total_bytes) {
    asx_encoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoding_config cfg;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    asx_encoding_config_init(&cfg);
    cfg.max_total_bytes = 15; /* fits one frame (4+5=9) but not two */

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, &cfg));

    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "hello", 5, NULL), ASX_OK);
    /* Second frame would push total to 18, exceeds 15 */
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "world", 5, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

TEST(encoding_cancel) {
    asx_encoding_pipeline p;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));

    /* Encode one frame successfully */
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "ok", 2, NULL), ASX_OK);

    /* Cancel */
    asx_encoding_pipeline_cancel(&p);

    /* Subsequent encode fails */
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "nope", 4, NULL), ASX_E_CANCELLED);
    ASSERT_EQ(asx_encoding_pipeline_last_error(&p), ASX_ENCODING_ERR_CANCELLED);
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

TEST(encoding_reset) {
    asx_encoding_pipeline p;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoding_stats stats;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "data", 4, NULL), ASX_OK);

    asx_encoding_pipeline_cancel(&p);
    asx_encoding_pipeline_reset(&p);

    /* After reset, should be able to encode again */
    ASSERT_EQ(p.cancelled, 0);
    asx_encoding_pipeline_stats(&p, &stats);
    ASSERT_EQ(stats.frames_encoded, 0u);
    ASSERT_EQ(stats.bytes_input, 0u);
    ASSERT_EQ(asx_encoding_pipeline_last_error(&p), ASX_ENCODING_ERR_NONE);
}

/* ------------------------------------------------------------------ */
/* Null data with nonzero length                                       */
/* ------------------------------------------------------------------ */

TEST(encoding_null_data_nonzero_len) {
    asx_encoding_pipeline p;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, NULL, 10, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Flush                                                               */
/* ------------------------------------------------------------------ */

TEST(encoding_flush) {
    asx_encoding_pipeline p;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));
    ASSERT_EQ(asx_encoding_pipeline_flush(&p), ASX_OK);
}

TEST(encoding_flush_null) { ASSERT_EQ(asx_encoding_pipeline_flush(NULL), ASX_E_INVALID_ARGUMENT); }

/* ------------------------------------------------------------------ */
/* Lines codec through pipeline                                        */
/* ------------------------------------------------------------------ */

TEST(encoding_lines_codec) {
    asx_encoding_pipeline p;
    asx_lines_codec lc;
    asx_codec codec;
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_encoding_stats stats;

    asx_lines_codec_init(&lc);
    codec = asx_lines_as_codec(&lc);
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    MUST_OK(asx_encoding_pipeline_init(&p, codec, writer, NULL));
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "line1", 5, NULL), ASX_OK);
    ASSERT_EQ(asx_encoding_pipeline_encode(&p, "line2", 5, NULL), ASX_OK);

    asx_encoding_pipeline_stats(&p, &stats);
    ASSERT_EQ(stats.frames_encoded, 2u);
    ASSERT_EQ(stats.bytes_input, 10u);
    ASSERT_EQ(stats.bytes_output, 12u); /* "line1\n" + "line2\n" */
}

/* ------------------------------------------------------------------ */
/* Null-safe query functions                                           */
/* ------------------------------------------------------------------ */

TEST(encoding_null_queries) {
    asx_encoding_stats stats;

    asx_encoding_pipeline_stats(NULL, &stats); /* should not crash */
    asx_encoding_pipeline_stats(NULL, NULL);
    ASSERT_EQ(asx_encoding_pipeline_last_error(NULL), ASX_ENCODING_ERR_NONE);
    asx_encoding_pipeline_cancel(NULL);
    asx_encoding_pipeline_reset(NULL);
}

#else

TEST(encoding_surface_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_IO, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_IO_SUBPROFILE_SPLIT, 1);
}

#endif

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_encoding ===\n");

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO
    RUN_TEST(encoding_error_kind_str);
    RUN_TEST(encoding_config_defaults);
    RUN_TEST(encoding_config_null_safe);
    RUN_TEST(encoding_pipeline_init_null);
    RUN_TEST(encoding_pipeline_init_ok);
    RUN_TEST(encoding_encode_single_frame);
    RUN_TEST(encoding_encode_multiple_frames);
    RUN_TEST(encoding_encode_empty_frame);
    RUN_TEST(encoding_max_frame_bytes);
    RUN_TEST(encoding_max_total_bytes);
    RUN_TEST(encoding_cancel);
    RUN_TEST(encoding_reset);
    RUN_TEST(encoding_null_data_nonzero_len);
    RUN_TEST(encoding_flush);
    RUN_TEST(encoding_flush_null);
    RUN_TEST(encoding_lines_codec);
    RUN_TEST(encoding_null_queries);
#else
    RUN_TEST(encoding_surface_compile_time_hidden_in_minimal_browser);
#endif

    TEST_REPORT();
    return test_failures;
}
