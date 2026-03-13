/*
 * test_codec.c — unit tests for framed codecs
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/bytes/codec.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

/* ------------------------------------------------------------------ */
/* Length-delimited codec tests                                        */
/* ------------------------------------------------------------------ */

TEST(ld_init_defaults) {
    asx_length_delimited_codec c;
    asx_length_delimited_codec_init(&c);
    ASSERT_EQ(c.length_field_size, 4);
    ASSERT_EQ(c.max_frame_len, 0u);
}

TEST(ld_init_with_valid) {
    asx_length_delimited_codec c;
    ASSERT_EQ(asx_length_delimited_codec_init_with(&c, 2, 1024), ASX_OK);
    ASSERT_EQ(c.length_field_size, 2);
    ASSERT_EQ(c.max_frame_len, 1024u);
}

TEST(ld_init_with_invalid_size) {
    asx_length_delimited_codec c;
    ASSERT_EQ(asx_length_delimited_codec_init_with(&c, 3, 0),
              ASX_E_INVALID_ARGUMENT);
}

TEST(ld_encode_decode_u32) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;
    asx_decode_result dr;

    asx_length_delimited_codec_init(&c);
    asx_buf_mut_init(&buf);

    /* Encode "hello" */
    MUST_OK(asx_length_delimited_encode(&c, "hello", 5, &buf));
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 9u);  /* 4 + 5 */

    /* Decode */
    dr = asx_length_delimited_decode(&c, &buf, &frame);
    ASSERT_EQ(dr, (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 5u);
    ASSERT_EQ(frame.data[0], (uint8_t)'h');
    ASSERT_EQ(frame.data[4], (uint8_t)'o');
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 0u);
}

TEST(ld_encode_decode_u16) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    MUST_OK(asx_length_delimited_codec_init_with(&c, 2, 0));
    asx_buf_mut_init(&buf);

    MUST_OK(asx_length_delimited_encode(&c, "ab", 2, &buf));
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 4u);  /* 2 + 2 */
    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 2u);
}

TEST(ld_encode_decode_u8) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    MUST_OK(asx_length_delimited_codec_init_with(&c, 1, 0));
    asx_buf_mut_init(&buf);

    MUST_OK(asx_length_delimited_encode(&c, "x", 1, &buf));
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 2u);  /* 1 + 1 */
    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 1u);
    ASSERT_EQ(frame.data[0], (uint8_t)'x');
}

TEST(ld_decode_need_more) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_length_delimited_codec_init(&c);
    asx_buf_mut_init(&buf);

    /* Only write 2 bytes of a 4-byte length field */
    MUST_OK(asx_buf_mut_put_u8(&buf, 0));
    MUST_OK(asx_buf_mut_put_u8(&buf, 0));
    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_NEED_MORE);
}

TEST(ld_decode_need_more_payload) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_length_delimited_codec_init(&c);
    asx_buf_mut_init(&buf);

    /* Write length=10 but only 3 bytes of payload */
    MUST_OK(asx_buf_mut_put_u32_be(&buf, 10));
    MUST_OK(asx_buf_mut_put(&buf, "abc", 3));
    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_NEED_MORE);
}

TEST(ld_max_frame_exceeded) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    MUST_OK(asx_length_delimited_codec_init_with(&c, 4, 8));
    asx_buf_mut_init(&buf);

    /* Write length=100, exceeds max_frame_len=8 */
    MUST_OK(asx_buf_mut_put_u32_be(&buf, 100));
    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_ERROR);
}

TEST(ld_encode_empty_frame) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_length_delimited_codec_init(&c);
    asx_buf_mut_init(&buf);

    MUST_OK(asx_length_delimited_encode(&c, NULL, 0, &buf));
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 4u);  /* just the length field */
    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 0u);
}

TEST(ld_multiple_frames) {
    asx_length_delimited_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_length_delimited_codec_init(&c);
    asx_buf_mut_init(&buf);

    MUST_OK(asx_length_delimited_encode(&c, "aa", 2, &buf));
    MUST_OK(asx_length_delimited_encode(&c, "bbb", 3, &buf));

    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 2u);

    ASSERT_EQ(asx_length_delimited_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 3u);
    ASSERT_EQ(frame.data[0], (uint8_t)'b');
}

TEST(ld_as_codec) {
    asx_length_delimited_codec c;
    asx_codec codec;
    asx_buf_mut buf;
    asx_frame frame;

    asx_length_delimited_codec_init(&c);
    codec = asx_length_delimited_as_codec(&c);
    asx_buf_mut_init(&buf);

    MUST_OK(codec.encode(codec.state, "test", 4, &buf));
    ASSERT_EQ(codec.decode(codec.state, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 4u);
}

/* ------------------------------------------------------------------ */
/* Lines codec tests                                                   */
/* ------------------------------------------------------------------ */

TEST(lines_decode_basic) {
    asx_lines_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init(&c);
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "hello\nworld\n", 12));

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 5u);  /* "hello" */

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 5u);  /* "world" */
}

TEST(lines_decode_crlf) {
    asx_lines_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init(&c);
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "line\r\n", 6));

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 4u);  /* "line" without \r */
}

TEST(lines_decode_need_more) {
    asx_lines_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init(&c);
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "no-newline", 10));

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_NEED_MORE);
}

TEST(lines_decode_empty_line) {
    asx_lines_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init(&c);
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "\n", 1));

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 0u);
}

TEST(lines_max_exceeded) {
    asx_lines_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init_with(&c, 3);
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "toolong\n", 8));

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_ERROR);
}

TEST(lines_encode) {
    asx_lines_codec c;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init(&c);
    asx_buf_mut_init(&buf);

    MUST_OK(asx_lines_encode(&c, "hello", 5, &buf));
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 6u);  /* "hello\n" */

    ASSERT_EQ(asx_lines_decode(&c, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 5u);
}

TEST(lines_as_codec) {
    asx_lines_codec c;
    asx_codec codec;
    asx_buf_mut buf;
    asx_frame frame;

    asx_lines_codec_init(&c);
    codec = asx_lines_as_codec(&c);
    asx_buf_mut_init(&buf);

    MUST_OK(codec.encode(codec.state, "test", 4, &buf));
    ASSERT_EQ(codec.decode(codec.state, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 4u);
}

/* ------------------------------------------------------------------ */
/* Bytes codec tests                                                   */
/* ------------------------------------------------------------------ */

TEST(bytes_decode_returns_all) {
    asx_buf_mut buf;
    asx_frame frame;

    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "abcde", 5));

    ASSERT_EQ(asx_bytes_decode(NULL, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 5u);
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 0u);
}

TEST(bytes_decode_empty_need_more) {
    asx_buf_mut buf;
    asx_frame frame;

    asx_buf_mut_init(&buf);
    ASSERT_EQ(asx_bytes_decode(NULL, &buf, &frame),
              (asx_decode_result)ASX_DECODE_NEED_MORE);
}

TEST(bytes_encode) {
    asx_buf_mut buf;
    asx_buf_mut_init(&buf);
    ASSERT_EQ(asx_bytes_encode(NULL, "xyz", 3, &buf), ASX_OK);
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 3u);
}

TEST(bytes_as_codec) {
    asx_codec codec;
    asx_buf_mut buf;
    asx_frame frame;

    codec = asx_bytes_as_codec();
    asx_buf_mut_init(&buf);

    MUST_OK(codec.encode(codec.state, "data", 4, &buf));
    ASSERT_EQ(codec.decode(codec.state, &buf, &frame),
              (asx_decode_result)ASX_DECODE_FRAME);
    ASSERT_EQ(frame.len, 4u);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_codec ===\n");

    RUN_TEST(ld_init_defaults);
    RUN_TEST(ld_init_with_valid);
    RUN_TEST(ld_init_with_invalid_size);
    RUN_TEST(ld_encode_decode_u32);
    RUN_TEST(ld_encode_decode_u16);
    RUN_TEST(ld_encode_decode_u8);
    RUN_TEST(ld_decode_need_more);
    RUN_TEST(ld_decode_need_more_payload);
    RUN_TEST(ld_max_frame_exceeded);
    RUN_TEST(ld_encode_empty_frame);
    RUN_TEST(ld_multiple_frames);
    RUN_TEST(ld_as_codec);

    RUN_TEST(lines_decode_basic);
    RUN_TEST(lines_decode_crlf);
    RUN_TEST(lines_decode_need_more);
    RUN_TEST(lines_decode_empty_line);
    RUN_TEST(lines_max_exceeded);
    RUN_TEST(lines_encode);
    RUN_TEST(lines_as_codec);

    RUN_TEST(bytes_decode_returns_all);
    RUN_TEST(bytes_decode_empty_need_more);
    RUN_TEST(bytes_encode);
    RUN_TEST(bytes_as_codec);

    TEST_REPORT();
    return test_failures;
}
