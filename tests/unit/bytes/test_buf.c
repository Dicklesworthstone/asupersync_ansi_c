/*
 * test_buf.c — unit tests for byte buffer abstractions
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/bytes/buf.h>

static asx_status st_sink_;
#define MUST_OK(expr) do { st_sink_ = (expr); (void)st_sink_; } while(0)

/* ------------------------------------------------------------------ */
/* asx_buf tests                                                       */
/* ------------------------------------------------------------------ */

TEST(buf_from_data) {
    uint8_t data[] = {1, 2, 3, 4};
    asx_buf b = asx_buf_from(data, 4);
    ASSERT_EQ(b.len, 4u);
    ASSERT_EQ(b.ptr[0], 1);
    ASSERT_EQ(b.ptr[3], 4);
}

TEST(buf_from_null) {
    asx_buf b = asx_buf_from(NULL, 10);
    ASSERT_EQ(b.len, 0u);
}

TEST(buf_from_cstr) {
    asx_buf b = asx_buf_from_cstr("hello");
    ASSERT_EQ(b.len, 5u);
    ASSERT_EQ(b.ptr[0], (uint8_t)'h');
}

TEST(buf_from_cstr_null) {
    asx_buf b = asx_buf_from_cstr(NULL);
    ASSERT_EQ(b.len, 0u);
}

TEST(buf_empty) {
    asx_buf b = asx_buf_empty();
    ASSERT_EQ(b.len, 0u);
    ASSERT_TRUE(b.ptr == NULL);
}

TEST(buf_slice_basic) {
    asx_buf src = asx_buf_from_cstr("abcdef");
    asx_buf s = asx_buf_slice(src, 2, 3);
    ASSERT_EQ(s.len, 3u);
    ASSERT_EQ(s.ptr[0], (uint8_t)'c');
    ASSERT_EQ(s.ptr[2], (uint8_t)'e');
}

TEST(buf_slice_out_of_bounds) {
    asx_buf src = asx_buf_from_cstr("abc");
    asx_buf s = asx_buf_slice(src, 2, 5);
    ASSERT_EQ(s.len, 0u);
}

TEST(buf_slice_at_end) {
    asx_buf src = asx_buf_from_cstr("abc");
    asx_buf s = asx_buf_slice(src, 3, 0);
    ASSERT_EQ(s.len, 0u);
}

TEST(buf_eq_same) {
    asx_buf a = asx_buf_from_cstr("hello");
    asx_buf b = asx_buf_from_cstr("hello");
    ASSERT_TRUE(asx_buf_eq(a, b));
}

TEST(buf_eq_different) {
    asx_buf a = asx_buf_from_cstr("hello");
    asx_buf b = asx_buf_from_cstr("world");
    ASSERT_FALSE(asx_buf_eq(a, b));
}

TEST(buf_eq_different_len) {
    asx_buf a = asx_buf_from_cstr("hi");
    asx_buf b = asx_buf_from_cstr("hello");
    ASSERT_FALSE(asx_buf_eq(a, b));
}

TEST(buf_eq_both_empty) {
    asx_buf a = asx_buf_empty();
    asx_buf b = asx_buf_empty();
    ASSERT_TRUE(asx_buf_eq(a, b));
}

/* ------------------------------------------------------------------ */
/* asx_buf_mut init/clear/compact tests                                */
/* ------------------------------------------------------------------ */

TEST(buf_mut_init) {
    asx_buf_mut buf;
    asx_buf_mut_init(&buf);
    ASSERT_EQ(buf.rd_pos, 0u);
    ASSERT_EQ(buf.wr_pos, 0u);
    ASSERT_EQ(buf.capacity, (uint32_t)ASX_BUF_CAPACITY);
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 0u);
    ASSERT_EQ(asx_buf_mut_writable(&buf), (uint32_t)ASX_BUF_CAPACITY);
}

TEST(buf_mut_clear) {
    asx_buf_mut buf;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 42));
    MUST_OK(asx_buf_mut_put_u8(&buf, 43));
    asx_buf_mut_clear(&buf);
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 0u);
}

TEST(buf_mut_compact) {
    asx_buf_mut buf;
    uint8_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 1));
    MUST_OK(asx_buf_mut_put_u8(&buf, 2));
    MUST_OK(asx_buf_mut_put_u8(&buf, 3));
    MUST_OK(asx_buf_mut_get_u8(&buf, &val));  /* consume byte 1 */
    ASSERT_EQ(val, 1);
    ASSERT_EQ(buf.rd_pos, 1u);
    asx_buf_mut_compact(&buf);
    ASSERT_EQ(buf.rd_pos, 0u);
    ASSERT_EQ(buf.wr_pos, 2u);
    MUST_OK(asx_buf_mut_get_u8(&buf, &val));
    ASSERT_EQ(val, 2);
}

/* ------------------------------------------------------------------ */
/* Write operations                                                    */
/* ------------------------------------------------------------------ */

TEST(buf_mut_put_bytes) {
    asx_buf_mut buf;
    uint8_t data[] = {10, 20, 30};
    asx_buf_mut_init(&buf);
    ASSERT_EQ(asx_buf_mut_put(&buf, data, 3), ASX_OK);
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 3u);
}

TEST(buf_mut_put_null_fails) {
    asx_buf_mut buf;
    asx_buf_mut_init(&buf);
    ASSERT_EQ(asx_buf_mut_put(&buf, NULL, 5), ASX_E_INVALID_ARGUMENT);
}

TEST(buf_mut_put_zero_len_ok) {
    asx_buf_mut buf;
    asx_buf_mut_init(&buf);
    ASSERT_EQ(asx_buf_mut_put(&buf, NULL, 0), ASX_OK);
}

TEST(buf_mut_put_u16_be) {
    asx_buf_mut buf;
    uint16_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u16_be(&buf, 0x0102));
    ASSERT_EQ(buf.data[0], 0x01);
    ASSERT_EQ(buf.data[1], 0x02);
    MUST_OK(asx_buf_mut_get_u16_be(&buf, &val));
    ASSERT_EQ(val, (uint16_t)0x0102);
}

TEST(buf_mut_put_u32_be) {
    asx_buf_mut buf;
    uint32_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u32_be(&buf, 0x01020304));
    MUST_OK(asx_buf_mut_get_u32_be(&buf, &val));
    ASSERT_EQ(val, (uint32_t)0x01020304);
}

TEST(buf_mut_put_u64_be) {
    asx_buf_mut buf;
    uint64_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u64_be(&buf, 0x0102030405060708ULL));
    MUST_OK(asx_buf_mut_get_u64_be(&buf, &val));
    ASSERT_EQ(val, 0x0102030405060708ULL);
}

/* ------------------------------------------------------------------ */
/* Read operations                                                     */
/* ------------------------------------------------------------------ */

TEST(buf_mut_get_u8) {
    asx_buf_mut buf;
    uint8_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 0xAB));
    MUST_OK(asx_buf_mut_get_u8(&buf, &val));
    ASSERT_EQ(val, 0xAB);
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 0u);
}

TEST(buf_mut_get_underflow) {
    asx_buf_mut buf;
    uint32_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 1));
    ASSERT_EQ(asx_buf_mut_get_u32_be(&buf, &val), ASX_E_INVALID_ARGUMENT);
}

TEST(buf_mut_advance) {
    asx_buf_mut buf;
    uint8_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 1));
    MUST_OK(asx_buf_mut_put_u8(&buf, 2));
    MUST_OK(asx_buf_mut_put_u8(&buf, 3));
    MUST_OK(asx_buf_mut_advance(&buf, 2));
    MUST_OK(asx_buf_mut_get_u8(&buf, &val));
    ASSERT_EQ(val, 3);
}

TEST(buf_mut_advance_overflow) {
    asx_buf_mut buf;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 1));
    ASSERT_EQ(asx_buf_mut_advance(&buf, 5), ASX_E_INVALID_ARGUMENT);
}

TEST(buf_mut_peek) {
    asx_buf_mut buf;
    uint8_t peeked[2];
    uint8_t val;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 10));
    MUST_OK(asx_buf_mut_put_u8(&buf, 20));
    MUST_OK(asx_buf_mut_peek(&buf, peeked, 2));
    ASSERT_EQ(peeked[0], 10);
    ASSERT_EQ(peeked[1], 20);
    /* Peek doesn't consume */
    ASSERT_EQ(asx_buf_mut_remaining(&buf), 2u);
    MUST_OK(asx_buf_mut_get_u8(&buf, &val));
    ASSERT_EQ(val, 10);
}

TEST(buf_mut_peek_overflow) {
    asx_buf_mut buf;
    uint8_t peeked[4];
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put_u8(&buf, 1));
    ASSERT_EQ(asx_buf_mut_peek(&buf, peeked, 4), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Readable / freeze                                                   */
/* ------------------------------------------------------------------ */

TEST(buf_mut_readable) {
    asx_buf_mut buf;
    asx_buf view;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "hi", 2));
    view = asx_buf_mut_readable(&buf);
    ASSERT_EQ(view.len, 2u);
    ASSERT_EQ(view.ptr[0], (uint8_t)'h');
}

TEST(buf_mut_freeze) {
    asx_buf_mut buf;
    asx_buf frozen;
    asx_buf_mut_init(&buf);
    MUST_OK(asx_buf_mut_put(&buf, "abc", 3));
    frozen = asx_buf_mut_freeze(&buf);
    ASSERT_EQ(frozen.len, 3u);
    ASSERT_TRUE(asx_buf_eq(frozen, asx_buf_from_cstr("abc")));
}

/* ------------------------------------------------------------------ */
/* Null safety                                                         */
/* ------------------------------------------------------------------ */

TEST(buf_mut_null_safety) {
    ASSERT_EQ(asx_buf_mut_remaining(NULL), 0u);
    ASSERT_EQ(asx_buf_mut_writable(NULL), 0u);
    ASSERT_EQ(asx_buf_mut_put(NULL, "x", 1), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_buf_mut_advance(NULL, 1), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "=== test_buf ===\n");

    RUN_TEST(buf_from_data);
    RUN_TEST(buf_from_null);
    RUN_TEST(buf_from_cstr);
    RUN_TEST(buf_from_cstr_null);
    RUN_TEST(buf_empty);
    RUN_TEST(buf_slice_basic);
    RUN_TEST(buf_slice_out_of_bounds);
    RUN_TEST(buf_slice_at_end);
    RUN_TEST(buf_eq_same);
    RUN_TEST(buf_eq_different);
    RUN_TEST(buf_eq_different_len);
    RUN_TEST(buf_eq_both_empty);

    RUN_TEST(buf_mut_init);
    RUN_TEST(buf_mut_clear);
    RUN_TEST(buf_mut_compact);

    RUN_TEST(buf_mut_put_bytes);
    RUN_TEST(buf_mut_put_null_fails);
    RUN_TEST(buf_mut_put_zero_len_ok);
    RUN_TEST(buf_mut_put_u16_be);
    RUN_TEST(buf_mut_put_u32_be);
    RUN_TEST(buf_mut_put_u64_be);

    RUN_TEST(buf_mut_get_u8);
    RUN_TEST(buf_mut_get_underflow);
    RUN_TEST(buf_mut_advance);
    RUN_TEST(buf_mut_advance_overflow);
    RUN_TEST(buf_mut_peek);
    RUN_TEST(buf_mut_peek_overflow);

    RUN_TEST(buf_mut_readable);
    RUN_TEST(buf_mut_freeze);

    RUN_TEST(buf_mut_null_safety);

    TEST_REPORT();
    return test_failures;
}
