/*
 * test_decoding.c — unit tests for decoding pipeline
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/decoding/decoding.h>
#include <asx/bytes/codec.h>
#include <asx/bytes/io_adapter.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Error kind string tests                                             */
/* ------------------------------------------------------------------ */

TEST(decoding_error_kind_str) {
    ASSERT_STR_EQ(asx_decoding_error_kind_str(ASX_DECODING_ERR_NONE), "none");
    ASSERT_STR_EQ(asx_decoding_error_kind_str(ASX_DECODING_ERR_CODEC), "codec");
    ASSERT_STR_EQ(asx_decoding_error_kind_str(ASX_DECODING_ERR_IO), "io");
    ASSERT_STR_EQ(asx_decoding_error_kind_str(ASX_DECODING_ERR_OVERFLOW), "overflow");
    ASSERT_STR_EQ(asx_decoding_error_kind_str(ASX_DECODING_ERR_CANCELLED), "cancelled");
    ASSERT_STR_EQ(asx_decoding_error_kind_str((asx_decoding_error_kind)99), "unknown");
}

/* ------------------------------------------------------------------ */
/* Config tests                                                        */
/* ------------------------------------------------------------------ */

TEST(decoding_config_defaults) {
    asx_decoding_config cfg;
    asx_decoding_config_init(&cfg);
    ASSERT_EQ(cfg.max_frame_bytes, 0u);
    ASSERT_EQ(cfg.max_total_bytes, 0u);
    ASSERT_EQ(cfg.max_frames, 0u);
}

TEST(decoding_config_null_safe) {
    asx_decoding_config_init(NULL); /* should not crash */
}

/* ------------------------------------------------------------------ */
/* Pipeline init tests                                                 */
/* ------------------------------------------------------------------ */

TEST(decoding_pipeline_init_null) {
    ASSERT_EQ(asx_decoding_pipeline_init(NULL, (asx_codec){0}, (asx_read_adapter){0}, NULL),
              ASX_E_INVALID_ARGUMENT);
}

TEST(decoding_pipeline_init_ok) {
    asx_decoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&source);
    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    ASSERT_EQ(asx_decoding_pipeline_init(&p, codec, reader, NULL), ASX_OK);
    ASSERT_EQ(p.progress.frames_decoded, 0u);
    ASSERT_EQ(p.last_error, ASX_DECODING_ERR_NONE);
    ASSERT_EQ(p.cancelled, 0);
}

/* ------------------------------------------------------------------ */
/* Decode single frame                                                 */
/* ------------------------------------------------------------------ */

TEST(decoding_decode_single_frame) {
    asx_decoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_decoding_progress prog;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&source);

    /* Pre-fill source with one length-delimited frame: len=5, "hello" */
    MUST_OK(asx_buf_mut_put_u32_be(&source, 5));
    MUST_OK(asx_buf_mut_put(&source, "hello", 5));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);

    ASSERT_EQ(frame.len, 5u);
    ASSERT_EQ(memcmp(frame.data, "hello", 5), 0);

    asx_decoding_pipeline_progress(&p, &prog);
    ASSERT_EQ(prog.frames_decoded, 1u);
    ASSERT_EQ(prog.bytes_consumed, 9u); /* 4 + 5 */
}

/* ------------------------------------------------------------------ */
/* Decode multiple frames                                              */
/* ------------------------------------------------------------------ */

TEST(decoding_decode_multiple_frames) {
    asx_decoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_decoding_progress prog;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&source);

    /* Two frames */
    MUST_OK(asx_buf_mut_put_u32_be(&source, 2));
    MUST_OK(asx_buf_mut_put(&source, "ab", 2));
    MUST_OK(asx_buf_mut_put_u32_be(&source, 3));
    MUST_OK(asx_buf_mut_put(&source, "cde", 3));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));

    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(memcmp(frame.data, "ab", 2), 0);

    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    ASSERT_EQ(frame.len, 3u);
    ASSERT_EQ(memcmp(frame.data, "cde", 3), 0);

    asx_decoding_pipeline_progress(&p, &prog);
    ASSERT_EQ(prog.frames_decoded, 2u);
}

/* ------------------------------------------------------------------ */
/* Decode returns EOF when source is drained                           */
/* ------------------------------------------------------------------ */

TEST(decoding_eof) {
    asx_decoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_decoding_progress prog;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&source);

    /* One frame */
    MUST_OK(asx_buf_mut_put_u32_be(&source, 1));
    MUST_OK(asx_buf_mut_put_u8(&source, 'x'));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    ASSERT_EQ(frame.len, 1u);

    /* Next decode should hit EOF */
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_E_NOT_FOUND);

    asx_decoding_pipeline_progress(&p, &prog);
    ASSERT_NE(prog.eof_reached, 0);
    ASSERT_NE(prog.complete, 0);
}

/* ------------------------------------------------------------------ */
/* Max frames limit                                                    */
/* ------------------------------------------------------------------ */

TEST(decoding_max_frames) {
    asx_decoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_decoding_config cfg;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&source);

    /* Three frames */
    MUST_OK(asx_buf_mut_put_u32_be(&source, 1));
    MUST_OK(asx_buf_mut_put_u8(&source, 'a'));
    MUST_OK(asx_buf_mut_put_u32_be(&source, 1));
    MUST_OK(asx_buf_mut_put_u8(&source, 'b'));
    MUST_OK(asx_buf_mut_put_u32_be(&source, 1));
    MUST_OK(asx_buf_mut_put_u8(&source, 'c'));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    asx_decoding_config_init(&cfg);
    cfg.max_frames = 2;

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, &cfg));
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    /* Third frame blocked by limit */
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Max frame bytes limit                                               */
/* ------------------------------------------------------------------ */

TEST(decoding_max_frame_bytes) {
    asx_decoding_pipeline p;
    asx_length_delimited_codec ldc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_decoding_config cfg;

    asx_length_delimited_codec_init(&ldc);
    codec = asx_length_delimited_as_codec(&ldc);
    asx_buf_mut_init(&source);

    /* Frame with 10 bytes of payload */
    MUST_OK(asx_buf_mut_put_u32_be(&source, 10));
    MUST_OK(asx_buf_mut_put(&source, "0123456789", 10));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    asx_decoding_config_init(&cfg);
    cfg.max_frame_bytes = 5; /* only allow frames up to 5 bytes */

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, &cfg));
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_decoding_pipeline_last_error(&p), ASX_DECODING_ERR_OVERFLOW);
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

TEST(decoding_cancel) {
    asx_decoding_pipeline p;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&source);
    MUST_OK(asx_buf_mut_put(&source, "data", 4));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));

    /* Decode one frame successfully */
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);

    /* Cancel */
    asx_decoding_pipeline_cancel(&p);

    /* Subsequent decode fails */
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_E_CANCELLED);
    ASSERT_EQ(asx_decoding_pipeline_last_error(&p), ASX_DECODING_ERR_CANCELLED);
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

TEST(decoding_reset) {
    asx_decoding_pipeline p;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_decoding_progress prog;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&source);
    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));
    asx_decoding_pipeline_cancel(&p);
    asx_decoding_pipeline_reset(&p);

    ASSERT_EQ(p.cancelled, 0);
    asx_decoding_pipeline_progress(&p, &prog);
    ASSERT_EQ(prog.frames_decoded, 0u);
    ASSERT_EQ(prog.bytes_consumed, 0u);
    ASSERT_EQ(prog.eof_reached, 0);
    ASSERT_EQ(prog.complete, 0);
    ASSERT_EQ(asx_decoding_pipeline_last_error(&p), ASX_DECODING_ERR_NONE);
}

/* ------------------------------------------------------------------ */
/* Lines codec through pipeline                                        */
/* ------------------------------------------------------------------ */

TEST(decoding_lines_codec) {
    asx_decoding_pipeline p;
    asx_lines_codec lc;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;
    asx_decoding_progress prog;

    asx_lines_codec_init(&lc);
    codec = asx_lines_as_codec(&lc);
    asx_buf_mut_init(&source);

    MUST_OK(asx_buf_mut_put(&source, "hello\nworld\n", 12));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));

    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    ASSERT_EQ(frame.len, 5u);
    ASSERT_EQ(memcmp(frame.data, "hello", 5), 0);

    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_OK);
    ASSERT_EQ(frame.len, 5u);
    ASSERT_EQ(memcmp(frame.data, "world", 5), 0);

    asx_decoding_pipeline_progress(&p, &prog);
    ASSERT_EQ(prog.frames_decoded, 2u);
}

/* ------------------------------------------------------------------ */
/* Empty source returns EOF immediately                                */
/* ------------------------------------------------------------------ */

TEST(decoding_empty_source) {
    asx_decoding_pipeline p;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_frame frame;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&source);
    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, &frame), ASX_E_NOT_FOUND);
}

/* ------------------------------------------------------------------ */
/* Null argument rejection                                             */
/* ------------------------------------------------------------------ */

TEST(decoding_null_args) {
    asx_decoding_pipeline p;
    asx_codec codec;
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&source);
    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    MUST_OK(asx_decoding_pipeline_init(&p, codec, reader, NULL));
    ASSERT_EQ(asx_decoding_pipeline_decode(&p, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_decoding_pipeline_decode(NULL, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Null-safe query functions                                           */
/* ------------------------------------------------------------------ */

TEST(decoding_null_queries) {
    asx_decoding_progress prog;

    asx_decoding_pipeline_progress(NULL, &prog);  /* should not crash */
    asx_decoding_pipeline_progress(NULL, NULL);
    ASSERT_EQ(asx_decoding_pipeline_last_error(NULL), ASX_DECODING_ERR_NONE);
    asx_decoding_pipeline_cancel(NULL);
    asx_decoding_pipeline_reset(NULL);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_decoding ===\n");

    RUN_TEST(decoding_error_kind_str);
    RUN_TEST(decoding_config_defaults);
    RUN_TEST(decoding_config_null_safe);
    RUN_TEST(decoding_pipeline_init_null);
    RUN_TEST(decoding_pipeline_init_ok);
    RUN_TEST(decoding_decode_single_frame);
    RUN_TEST(decoding_decode_multiple_frames);
    RUN_TEST(decoding_eof);
    RUN_TEST(decoding_max_frames);
    RUN_TEST(decoding_max_frame_bytes);
    RUN_TEST(decoding_cancel);
    RUN_TEST(decoding_reset);
    RUN_TEST(decoding_lines_codec);
    RUN_TEST(decoding_empty_source);
    RUN_TEST(decoding_null_args);
    RUN_TEST(decoding_null_queries);

    TEST_REPORT();
    return test_failures;
}
