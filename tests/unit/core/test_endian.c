/*
 * test_endian.c — unit tests for portable endian and unaligned-access helpers
 *
 * Tests: roundtrip load/store for LE/BE u16/u32/u64, unaligned buffer
 * access, edge cases (0, MAX), byte-order canaries, cross-endian
 * fixture validation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/portable.h>
#include <stdlib.h>
#include <string.h>

/* ---- Little-endian roundtrip tests ---- */

TEST(le_u16_roundtrip) {
    uint8_t buf[2];
    asx_store_le_u16(buf, 0x0102u);
    ASSERT_EQ(buf[0], (uint8_t)0x02);
    ASSERT_EQ(buf[1], (uint8_t)0x01);
    ASSERT_EQ(asx_load_le_u16(buf), (uint16_t)0x0102u);
}

TEST(le_u32_roundtrip) {
    uint8_t buf[4];
    asx_store_le_u32(buf, 0x01020304u);
    ASSERT_EQ(buf[0], (uint8_t)0x04);
    ASSERT_EQ(buf[1], (uint8_t)0x03);
    ASSERT_EQ(buf[2], (uint8_t)0x02);
    ASSERT_EQ(buf[3], (uint8_t)0x01);
    ASSERT_EQ(asx_load_le_u32(buf), (uint32_t)0x01020304u);
}

TEST(le_u64_roundtrip) {
    uint8_t buf[8];
    uint64_t val = 0x0102030405060708ULL;
    asx_store_le_u64(buf, val);
    ASSERT_EQ(buf[0], (uint8_t)0x08);
    ASSERT_EQ(buf[7], (uint8_t)0x01);
    ASSERT_EQ(asx_load_le_u64(buf), val);
}

/* ---- Big-endian roundtrip tests ---- */

TEST(be_u16_roundtrip) {
    uint8_t buf[2];
    asx_store_be_u16(buf, 0x0102u);
    ASSERT_EQ(buf[0], (uint8_t)0x01);
    ASSERT_EQ(buf[1], (uint8_t)0x02);
    ASSERT_EQ(asx_load_be_u16(buf), (uint16_t)0x0102u);
}

TEST(be_u32_roundtrip) {
    uint8_t buf[4];
    asx_store_be_u32(buf, 0x01020304u);
    ASSERT_EQ(buf[0], (uint8_t)0x01);
    ASSERT_EQ(buf[1], (uint8_t)0x02);
    ASSERT_EQ(buf[2], (uint8_t)0x03);
    ASSERT_EQ(buf[3], (uint8_t)0x04);
    ASSERT_EQ(asx_load_be_u32(buf), (uint32_t)0x01020304u);
}

TEST(be_u64_roundtrip) {
    uint8_t buf[8];
    uint64_t val = 0x0102030405060708ULL;
    asx_store_be_u64(buf, val);
    ASSERT_EQ(buf[0], (uint8_t)0x01);
    ASSERT_EQ(buf[7], (uint8_t)0x08);
    ASSERT_EQ(asx_load_be_u64(buf), val);
}

/* ---- Edge cases: zero and MAX ---- */

TEST(le_u16_zero_and_max) {
    uint8_t buf[2];

    asx_store_le_u16(buf, 0);
    ASSERT_EQ(asx_load_le_u16(buf), (uint16_t)0);

    asx_store_le_u16(buf, 0xFFFFu);
    ASSERT_EQ(asx_load_le_u16(buf), (uint16_t)0xFFFFu);
}

TEST(le_u32_zero_and_max) {
    uint8_t buf[4];

    asx_store_le_u32(buf, 0);
    ASSERT_EQ(asx_load_le_u32(buf), (uint32_t)0);

    asx_store_le_u32(buf, 0xFFFFFFFFu);
    ASSERT_EQ(asx_load_le_u32(buf), (uint32_t)0xFFFFFFFFu);
}

TEST(le_u64_zero_and_max) {
    uint8_t buf[8];

    asx_store_le_u64(buf, 0);
    ASSERT_EQ(asx_load_le_u64(buf), (uint64_t)0);

    asx_store_le_u64(buf, 0xFFFFFFFFFFFFFFFFULL);
    ASSERT_EQ(asx_load_le_u64(buf), (uint64_t)0xFFFFFFFFFFFFFFFFULL);
}

TEST(be_u32_zero_and_max) {
    uint8_t buf[4];

    asx_store_be_u32(buf, 0);
    ASSERT_EQ(asx_load_be_u32(buf), (uint32_t)0);

    asx_store_be_u32(buf, 0xFFFFFFFFu);
    ASSERT_EQ(asx_load_be_u32(buf), (uint32_t)0xFFFFFFFFu);
}

TEST(be_u64_zero_and_max) {
    uint8_t buf[8];

    asx_store_be_u64(buf, 0);
    ASSERT_EQ(asx_load_be_u64(buf), (uint64_t)0);

    asx_store_be_u64(buf, 0xFFFFFFFFFFFFFFFFULL);
    ASSERT_EQ(asx_load_be_u64(buf), (uint64_t)0xFFFFFFFFFFFFFFFFULL);
}

/* ---- Unaligned buffer access ---- */

TEST(le_u32_unaligned_offset_1) {
    uint8_t raw[8];
    uint8_t *p = raw + 1; /* misaligned by 1 byte */

    memset(raw, 0, sizeof(raw));
    asx_store_le_u32(p, 0xDEADBEEFu);
    ASSERT_EQ(asx_load_le_u32(p), (uint32_t)0xDEADBEEFu);
}

TEST(le_u64_unaligned_offset_3) {
    uint8_t raw[16];
    uint8_t *p = raw + 3; /* misaligned by 3 bytes */

    memset(raw, 0, sizeof(raw));
    asx_store_le_u64(p, 0xCAFEBABE12345678ULL);
    ASSERT_EQ(asx_load_le_u64(p), (uint64_t)0xCAFEBABE12345678ULL);
}

TEST(be_u32_unaligned_offset_1) {
    uint8_t raw[8];
    uint8_t *p = raw + 1;

    memset(raw, 0, sizeof(raw));
    asx_store_be_u32(p, 0xDEADBEEFu);
    ASSERT_EQ(asx_load_be_u32(p), (uint32_t)0xDEADBEEFu);
}

TEST(be_u64_unaligned_offset_5) {
    uint8_t raw[16];
    uint8_t *p = raw + 5;

    memset(raw, 0, sizeof(raw));
    asx_store_be_u64(p, 0xCAFEBABE12345678ULL);
    ASSERT_EQ(asx_load_be_u64(p), (uint64_t)0xCAFEBABE12345678ULL);
}

TEST(unaligned_heap_buffer) {
    /* Allocate odd-sized buffer to force misalignment */
    uint8_t *heap = (uint8_t *)malloc(32);
    uint8_t *p;

    ASSERT_TRUE(heap != NULL);
    p = heap + 1; /* likely misaligned on most platforms */

    asx_store_le_u32(p, 0x11223344u);
    ASSERT_EQ(asx_load_le_u32(p), (uint32_t)0x11223344u);

    asx_store_be_u64(p, 0xAABBCCDDEEFF0011ULL);
    ASSERT_EQ(asx_load_be_u64(p), (uint64_t)0xAABBCCDDEEFF0011ULL);

    free(heap);
}

/* ---- Cross-endian: LE written, read as BE and vice versa ---- */

TEST(cross_endian_le_to_be_u32) {
    uint8_t buf[4];

    asx_store_le_u32(buf, 0x01020304u);
    /* LE layout: 04 03 02 01 => read as BE: 0x04030201 */
    ASSERT_EQ(asx_load_be_u32(buf), (uint32_t)0x04030201u);
}

TEST(cross_endian_be_to_le_u32) {
    uint8_t buf[4];

    asx_store_be_u32(buf, 0x01020304u);
    /* BE layout: 01 02 03 04 => read as LE: 0x04030201 */
    ASSERT_EQ(asx_load_le_u32(buf), (uint32_t)0x04030201u);
}

TEST(cross_endian_le_to_be_u64) {
    uint8_t buf[8];

    asx_store_le_u64(buf, 0x0102030405060708ULL);
    /* LE layout: 08 07 06 05 04 03 02 01 => read as BE: 0x0807060504030201 */
    ASSERT_EQ(asx_load_be_u64(buf), (uint64_t)0x0807060504030201ULL);
}

/* ---- Byte-order canary ---- */

TEST(endian_canary_le_roundtrip) {
    uint8_t buf[4];

    asx_store_endian_canary_le(buf);
    ASSERT_TRUE(asx_verify_endian_canary_le(buf));
    ASSERT_FALSE(asx_verify_endian_canary_be(buf));
}

TEST(endian_canary_be_roundtrip) {
    uint8_t buf[4];

    asx_store_endian_canary_be(buf);
    ASSERT_TRUE(asx_verify_endian_canary_be(buf));
    ASSERT_FALSE(asx_verify_endian_canary_le(buf));
}

TEST(endian_canary_corruption_detected) {
    uint8_t buf[4];

    asx_store_endian_canary_le(buf);
    buf[2] = 0xFF; /* corrupt one byte */
    ASSERT_FALSE(asx_verify_endian_canary_le(buf));
}

/* ---- Compile-time endian detection ---- */

TEST(endian_detection_defined) {
    /* At least one must be 1, the other 0 */
    ASSERT_TRUE((ASX_ENDIAN_LITTLE == 1 && ASX_ENDIAN_BIG == 0) ||
                (ASX_ENDIAN_LITTLE == 0 && ASX_ENDIAN_BIG == 1));
}

/* ---- Known wire format vectors ---- */

TEST(le_known_vector_trace_magic) {
    /* Trace format uses LE magic 0x41535874 = "ASXt" */
    uint8_t buf[4];
    asx_store_le_u32(buf, 0x41535874u);
    ASSERT_EQ(buf[0], (uint8_t)0x74); /* 't' */
    ASSERT_EQ(buf[1], (uint8_t)0x58); /* 'X' */
    ASSERT_EQ(buf[2], (uint8_t)0x53); /* 'S' */
    ASSERT_EQ(buf[3], (uint8_t)0x41); /* 'A' */
}

TEST(be_known_vector_fixture_length) {
    /* Fixture codec uses BE payload length */
    uint8_t buf[4];
    asx_store_be_u32(buf, 256u);
    ASSERT_EQ(buf[0], (uint8_t)0x00);
    ASSERT_EQ(buf[1], (uint8_t)0x00);
    ASSERT_EQ(buf[2], (uint8_t)0x01);
    ASSERT_EQ(buf[3], (uint8_t)0x00);
}

/* ---- Test suite runner ---- */

int main(void) {
    RUN_TEST(le_u16_roundtrip);
    RUN_TEST(le_u32_roundtrip);
    RUN_TEST(le_u64_roundtrip);
    RUN_TEST(be_u16_roundtrip);
    RUN_TEST(be_u32_roundtrip);
    RUN_TEST(be_u64_roundtrip);
    RUN_TEST(le_u16_zero_and_max);
    RUN_TEST(le_u32_zero_and_max);
    RUN_TEST(le_u64_zero_and_max);
    RUN_TEST(be_u32_zero_and_max);
    RUN_TEST(be_u64_zero_and_max);
    RUN_TEST(le_u32_unaligned_offset_1);
    RUN_TEST(le_u64_unaligned_offset_3);
    RUN_TEST(be_u32_unaligned_offset_1);
    RUN_TEST(be_u64_unaligned_offset_5);
    RUN_TEST(unaligned_heap_buffer);
    RUN_TEST(cross_endian_le_to_be_u32);
    RUN_TEST(cross_endian_be_to_le_u32);
    RUN_TEST(cross_endian_le_to_be_u64);
    RUN_TEST(endian_canary_le_roundtrip);
    RUN_TEST(endian_canary_be_roundtrip);
    RUN_TEST(endian_canary_corruption_detected);
    RUN_TEST(endian_detection_defined);
    RUN_TEST(le_known_vector_trace_magic);
    RUN_TEST(be_known_vector_fixture_length);
    TEST_REPORT();
    return test_failures;
}
