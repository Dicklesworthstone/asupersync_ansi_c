/*
 * test_pipe.c — unit tests for anonymous pipe pairs
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/net/pipe.h>
#include <string.h>

TEST(pipe_open_and_write_read) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    uint8_t data[] = "pipe-data";
    asx_buf src;
    asx_buf_mut dst;
    uint32_t written, read_n;

    asx_pipe_reset();
    ASSERT_EQ(asx_pipe_open(&rd, &wr), ASX_OK);
    ASSERT_TRUE(asx_pipe_read_is_alive(rd));
    ASSERT_TRUE(asx_pipe_write_is_alive(wr));

    src = asx_buf_from(data, 9);
    ASSERT_EQ(asx_pipe_poll_write(wr, &src, &written), ASX_OK);
    ASSERT_EQ(written, 9u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_pipe_poll_read(rd, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, 9u);
    ASSERT_TRUE(memcmp(dst.data, "pipe-data", 9) == 0);

    asx_pipe_close_read(rd);
    asx_pipe_close_write(wr);
}

TEST(pipe_read_empty_pending) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    asx_buf_mut dst;
    uint32_t read_n;

    asx_pipe_reset();
    ASSERT_EQ(asx_pipe_open(&rd, &wr), ASX_OK);
    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_pipe_poll_read(rd, &dst, &read_n), ASX_E_PENDING);
    ASSERT_EQ(read_n, 0u);

    asx_pipe_close_read(rd);
    asx_pipe_close_write(wr);
}

TEST(pipe_read_after_writer_closed_disconnected) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    asx_buf_mut dst;
    uint32_t read_n;

    asx_pipe_reset();
    ASSERT_EQ(asx_pipe_open(&rd, &wr), ASX_OK);
    asx_pipe_close_write(wr);
    ASSERT_FALSE(asx_pipe_write_is_alive(wr));

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_pipe_poll_read(rd, &dst, &read_n), ASX_E_DISCONNECTED);

    asx_pipe_close_read(rd);
}

TEST(pipe_write_after_reader_closed_disconnected) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    uint8_t data[] = "x";
    asx_buf src;
    uint32_t written;

    asx_pipe_reset();
    ASSERT_EQ(asx_pipe_open(&rd, &wr), ASX_OK);
    asx_pipe_close_read(rd);

    src = asx_buf_from(data, 1);
    ASSERT_EQ(asx_pipe_poll_write(wr, &src, &written), ASX_E_DISCONNECTED);

    asx_pipe_close_write(wr);
}

TEST(pipe_write_full_buffer_would_block_then_recovers) {
    asx_pipe_read rd;
    asx_pipe_write wr;
    uint8_t full[ASX_BUF_CAPACITY];
    uint8_t one = 'x';
    asx_buf src;
    asx_buf one_src;
    asx_buf_mut dst;
    uint32_t written;
    uint32_t read_n;
    uint32_t i;

    asx_pipe_reset();
    ASSERT_EQ(asx_pipe_open(&rd, &wr), ASX_OK);

    for (i = 0u; i < ASX_BUF_CAPACITY; i++) { full[i] = (uint8_t)('a' + (i % 23u)); }
    src = asx_buf_from(full, ASX_BUF_CAPACITY);
    one_src = asx_buf_from(&one, 1u);

    ASSERT_EQ(asx_pipe_poll_write(wr, &src, &written), ASX_OK);
    ASSERT_EQ(written, ASX_BUF_CAPACITY);

    written = 99u;
    ASSERT_EQ(asx_pipe_poll_write(wr, &one_src, &written), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(written, 0u);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(asx_pipe_poll_read(rd, &dst, &read_n), ASX_OK);
    ASSERT_EQ(read_n, ASX_BUF_CAPACITY);

    ASSERT_EQ(asx_pipe_poll_write(wr, &one_src, &written), ASX_OK);
    ASSERT_EQ(written, 1u);

    asx_pipe_close_read(rd);
    asx_pipe_close_write(wr);
}

TEST(pipe_exhaustion) {
    asx_pipe_read rds[ASX_MAX_PIPES + 1];
    asx_pipe_write wrs[ASX_MAX_PIPES + 1];
    uint32_t i;

    asx_pipe_reset();
    for (i = 0; i < ASX_MAX_PIPES; i++) { ASSERT_EQ(asx_pipe_open(&rds[i], &wrs[i]), ASX_OK); }
    ASSERT_EQ(asx_pipe_open(&rds[ASX_MAX_PIPES], &wrs[ASX_MAX_PIPES]), ASX_E_RESOURCE_EXHAUSTED);

    for (i = 0; i < ASX_MAX_PIPES; i++) {
        asx_pipe_close_read(rds[i]);
        asx_pipe_close_write(wrs[i]);
    }
}

TEST(pipe_null_args) {
    asx_pipe_read rd;
    asx_pipe_write wr;

    ASSERT_EQ(asx_pipe_open(NULL, &wr), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_pipe_open(&rd, NULL), ASX_E_INVALID_ARGUMENT);
}

int main(void) {
    fprintf(stderr, "=== pipe tests ===\n");
    RUN_TEST(pipe_open_and_write_read);
    RUN_TEST(pipe_read_empty_pending);
    RUN_TEST(pipe_read_after_writer_closed_disconnected);
    RUN_TEST(pipe_write_after_reader_closed_disconnected);
    RUN_TEST(pipe_write_full_buffer_would_block_then_recovers);
    RUN_TEST(pipe_exhaustion);
    RUN_TEST(pipe_null_args);
    TEST_REPORT();
    return test_failures;
}
