/*
 * test_io_adapter.c — unit tests for read/write adapters
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/asx_config.h>
#include <asx/bytes/io_adapter.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Memory read adapter tests                                           */
/* ------------------------------------------------------------------ */

TEST(mem_read_basic) {
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_buf_mut dst;
    uint32_t bytes_read;

    asx_buf_mut_init(&source);
    MUST_OK(asx_buf_mut_put(&source, "hello", 5));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(reader.poll_read(reader.state, &dst, NULL, &bytes_read),
              (asx_read_result)ASX_READ_READY);
    ASSERT_EQ(bytes_read, 5u);
    ASSERT_EQ(asx_buf_mut_remaining(&dst), 5u);
}

TEST(mem_read_eof_when_empty) {
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_buf_mut dst;
    uint32_t bytes_read;

    asx_buf_mut_init(&source); /* empty source */
    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(reader.poll_read(reader.state, &dst, NULL, &bytes_read),
              (asx_read_result)ASX_READ_EOF);
}

TEST(mem_read_partial) {
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_buf_mut dst;
    uint32_t bytes_read;

    asx_buf_mut_init(&source);
    MUST_OK(asx_buf_mut_put(&source, "abcdefghij", 10));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    /* dst has limited space — fill it almost to capacity, leave 3 bytes */
    asx_buf_mut_init(&dst);
    dst.wr_pos = ASX_BUF_CAPACITY - 3;

    ASSERT_EQ(reader.poll_read(reader.state, &dst, NULL, &bytes_read),
              (asx_read_result)ASX_READ_READY);
    ASSERT_EQ(bytes_read, 3u);
    /* Source should still have 7 bytes left */
    ASSERT_EQ(asx_buf_mut_remaining(&source), 7u);
}

TEST(mem_read_drains_source) {
    asx_buf_mut source;
    asx_mem_read_state rs;
    asx_read_adapter reader;
    asx_buf_mut dst;
    uint32_t bytes_read;

    asx_buf_mut_init(&source);
    MUST_OK(asx_buf_mut_put(&source, "abc", 3));

    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    asx_buf_mut_init(&dst);
    ASSERT_EQ(reader.poll_read(reader.state, &dst, NULL, &bytes_read),
              (asx_read_result)ASX_READ_READY);
    ASSERT_EQ(bytes_read, 3u);

    /* Source is now drained, next read returns EOF */
    ASSERT_EQ(reader.poll_read(reader.state, &dst, NULL, &bytes_read),
              (asx_read_result)ASX_READ_EOF);
}

/* ------------------------------------------------------------------ */
/* Memory write adapter tests                                          */
/* ------------------------------------------------------------------ */

TEST(mem_write_basic) {
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_buf data;
    uint32_t bytes_written;

    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    data = asx_buf_from_cstr("hello");
    ASSERT_EQ(writer.poll_write(writer.state, &data, NULL, &bytes_written),
              (asx_write_result)ASX_WRITE_READY);
    ASSERT_EQ(bytes_written, 5u);
    ASSERT_EQ(asx_buf_mut_remaining(&sink), 5u);
}

TEST(mem_write_flush) {
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;

    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    /* Flush always succeeds for memory adapter */
    ASSERT_EQ(writer.poll_flush(writer.state, NULL), (asx_write_result)ASX_WRITE_READY);
}

TEST(mem_write_shutdown) {
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_buf data;
    uint32_t bytes_written;

    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    ASSERT_EQ(writer.poll_shutdown(writer.state, NULL), (asx_write_result)ASX_WRITE_READY);

    /* Write after shutdown returns CLOSED */
    data = asx_buf_from_cstr("data");
    ASSERT_EQ(writer.poll_write(writer.state, &data, NULL, &bytes_written),
              (asx_write_result)ASX_WRITE_CLOSED);
}

TEST(mem_write_partial) {
    asx_buf_mut sink;
    asx_mem_write_state ws;
    asx_write_adapter writer;
    asx_buf data;
    uint32_t bytes_written;

    asx_buf_mut_init(&sink);
    /* Fill sink almost to capacity */
    sink.wr_pos = ASX_BUF_CAPACITY - 2;

    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    data = asx_buf_from_cstr("hello");
    ASSERT_EQ(writer.poll_write(writer.state, &data, NULL, &bytes_written),
              (asx_write_result)ASX_WRITE_READY);
    ASSERT_EQ(bytes_written, 2u); /* only 2 bytes of space */
}

/* ------------------------------------------------------------------ */
/* Integration: read -> write round-trip                                */
/* ------------------------------------------------------------------ */

TEST(roundtrip_read_write) {
    asx_buf_mut source, sink;
    asx_mem_read_state rs;
    asx_mem_write_state ws;
    asx_read_adapter reader;
    asx_write_adapter writer;
    asx_buf_mut relay;
    uint32_t bytes_read;
    uint32_t bytes_written;
    asx_buf src_view;

    /* Set up source with data */
    asx_buf_mut_init(&source);
    MUST_OK(asx_buf_mut_put(&source, "roundtrip", 9));
    asx_mem_read_init(&rs, &source);
    reader = asx_mem_read_adapter(&rs);

    /* Set up sink */
    asx_buf_mut_init(&sink);
    asx_mem_write_init(&ws, &sink);
    writer = asx_mem_write_adapter(&ws);

    /* Read from source into relay buffer */
    asx_buf_mut_init(&relay);
    ASSERT_EQ(reader.poll_read(reader.state, &relay, NULL, &bytes_read),
              (asx_read_result)ASX_READ_READY);
    ASSERT_EQ(bytes_read, 9u);

    /* Write from relay into sink */
    src_view = asx_buf_mut_freeze(&relay);
    ASSERT_EQ(writer.poll_write(writer.state, &src_view, NULL, &bytes_written),
              (asx_write_result)ASX_WRITE_READY);
    ASSERT_EQ(bytes_written, 9u);

    /* Verify sink contents match */
    ASSERT_TRUE(asx_buf_eq(asx_buf_mut_freeze(&sink), asx_buf_from_cstr("roundtrip")));
}

#else

TEST(io_adapter_surface_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_IO, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_IO_SUBPROFILE_SPLIT, 1);
}

#endif

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_io_adapter ===\n");

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO
    RUN_TEST(mem_read_basic);
    RUN_TEST(mem_read_eof_when_empty);
    RUN_TEST(mem_read_partial);
    RUN_TEST(mem_read_drains_source);

    RUN_TEST(mem_write_basic);
    RUN_TEST(mem_write_flush);
    RUN_TEST(mem_write_shutdown);
    RUN_TEST(mem_write_partial);

    RUN_TEST(roundtrip_read_write);
#else
    RUN_TEST(io_adapter_surface_compile_time_hidden_in_minimal_browser);
#endif

    TEST_REPORT();
    return test_failures;
}
