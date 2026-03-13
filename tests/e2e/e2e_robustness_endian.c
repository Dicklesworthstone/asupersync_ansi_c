/*
 * e2e_robustness_endian.c — robustness e2e for endian/unaligned boundary safety
 *
 * Exercises: LE/BE round-trip for all integer sizes, unaligned buffer
 * access, endian canary validation, cross-format identity checks,
 * and boundary edge cases (zero, max, byte-pattern).
 *
 * Output: one line per scenario in the format:
 *   SCENARIO <id> <pass|fail> [diagnostic]
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/portable.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* -------------------------------------------------------------------
 * Scenarios
 * ------------------------------------------------------------------- */

/* robust-endian-001: LE u16 round-trip */
static void scenario_le_u16_roundtrip(void) {
    SCENARIO_BEGIN("robust-endian-001.le_u16_roundtrip");

    uint16_t test_vals[] = {0, 1, 0x00FF, 0xFF00, 0x1234, 0xFFFF};
    uint8_t buf[2];
    uint32_t i;

    for (i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        asx_store_le_u16(buf, test_vals[i]);
        uint16_t loaded = asx_load_le_u16(buf);
        SCENARIO_CHECK(loaded == test_vals[i], "LE u16 round-trip mismatch");
    }

    SCENARIO_END();
}

/* robust-endian-002: LE u32 round-trip */
static void scenario_le_u32_roundtrip(void) {
    SCENARIO_BEGIN("robust-endian-002.le_u32_roundtrip");

    uint32_t test_vals[] = {0, 1, 0x000000FFu, 0xFF000000u, 0x12345678u, 0xFFFFFFFFu};
    uint8_t buf[4];
    uint32_t i;

    for (i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        asx_store_le_u32(buf, test_vals[i]);
        uint32_t loaded = asx_load_le_u32(buf);
        SCENARIO_CHECK(loaded == test_vals[i], "LE u32 round-trip mismatch");
    }

    SCENARIO_END();
}

/* robust-endian-003: LE u64 round-trip */
static void scenario_le_u64_roundtrip(void) {
    SCENARIO_BEGIN("robust-endian-003.le_u64_roundtrip");

    uint64_t test_vals[] = {0,
                            1,
                            0x00000000000000FFULL,
                            0xFF00000000000000ULL,
                            0x123456789ABCDEF0ULL,
                            0xFFFFFFFFFFFFFFFFULL};
    uint8_t buf[8];
    uint32_t i;

    for (i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        asx_store_le_u64(buf, test_vals[i]);
        uint64_t loaded = asx_load_le_u64(buf);
        SCENARIO_CHECK(loaded == test_vals[i], "LE u64 round-trip mismatch");
    }

    SCENARIO_END();
}

/* robust-endian-004: BE u16 round-trip */
static void scenario_be_u16_roundtrip(void) {
    SCENARIO_BEGIN("robust-endian-004.be_u16_roundtrip");

    uint16_t test_vals[] = {0, 1, 0x00FF, 0xFF00, 0x1234, 0xFFFF};
    uint8_t buf[2];
    uint32_t i;

    for (i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        asx_store_be_u16(buf, test_vals[i]);
        uint16_t loaded = asx_load_be_u16(buf);
        SCENARIO_CHECK(loaded == test_vals[i], "BE u16 round-trip mismatch");
    }

    SCENARIO_END();
}

/* robust-endian-005: BE u32 round-trip */
static void scenario_be_u32_roundtrip(void) {
    SCENARIO_BEGIN("robust-endian-005.be_u32_roundtrip");

    uint32_t test_vals[] = {0, 1, 0x000000FFu, 0xFF000000u, 0x12345678u, 0xFFFFFFFFu};
    uint8_t buf[4];
    uint32_t i;

    for (i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        asx_store_be_u32(buf, test_vals[i]);
        uint32_t loaded = asx_load_be_u32(buf);
        SCENARIO_CHECK(loaded == test_vals[i], "BE u32 round-trip mismatch");
    }

    SCENARIO_END();
}

/* robust-endian-006: BE u64 round-trip */
static void scenario_be_u64_roundtrip(void) {
    SCENARIO_BEGIN("robust-endian-006.be_u64_roundtrip");

    uint64_t test_vals[] = {0,
                            1,
                            0x00000000000000FFULL,
                            0xFF00000000000000ULL,
                            0x123456789ABCDEF0ULL,
                            0xFFFFFFFFFFFFFFFFULL};
    uint8_t buf[8];
    uint32_t i;

    for (i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        asx_store_be_u64(buf, test_vals[i]);
        uint64_t loaded = asx_load_be_u64(buf);
        SCENARIO_CHECK(loaded == test_vals[i], "BE u64 round-trip mismatch");
    }

    SCENARIO_END();
}

/* robust-endian-007: unaligned buffer access via offset */
static void scenario_unaligned_access(void) {
    SCENARIO_BEGIN("robust-endian-007.unaligned_access");

    /* Buffer with odd alignment offset */
    uint8_t buf[32];
    uint8_t *unaligned;
    uint32_t offset;

    memset(buf, 0, sizeof(buf));

    /* Test at each possible misalignment offset (1-7) */
    for (offset = 1; offset <= 7; offset++) {
        unaligned = buf + offset;

        /* LE u32 at unaligned offset */
        asx_store_le_u32(unaligned, 0xDEADBEEFu);
        SCENARIO_CHECK(asx_load_le_u32(unaligned) == 0xDEADBEEFu, "unaligned LE u32 failed");

        /* BE u32 at unaligned offset */
        asx_store_be_u32(unaligned, 0xCAFEBABEu);
        SCENARIO_CHECK(asx_load_be_u32(unaligned) == 0xCAFEBABEu, "unaligned BE u32 failed");

        /* LE u64 at unaligned offset */
        asx_store_le_u64(unaligned, 0x0102030405060708ULL);
        SCENARIO_CHECK(asx_load_le_u64(unaligned) == 0x0102030405060708ULL,
                       "unaligned LE u64 failed");

        /* BE u64 at unaligned offset */
        asx_store_be_u64(unaligned, 0x0807060504030201ULL);
        SCENARIO_CHECK(asx_load_be_u64(unaligned) == 0x0807060504030201ULL,
                       "unaligned BE u64 failed");
    }

    SCENARIO_END();
}

/* robust-endian-008: LE/BE cross-format wire byte verification */
static void scenario_cross_format_wire_bytes(void) {
    SCENARIO_BEGIN("robust-endian-008.cross_format_wire_bytes");

    uint8_t le_buf[4], be_buf[4];
    uint32_t val = 0x01020304u;

    asx_store_le_u32(le_buf, val);
    asx_store_be_u32(be_buf, val);

    /* LE: least significant byte first */
    SCENARIO_CHECK(le_buf[0] == 0x04 && le_buf[1] == 0x03 && le_buf[2] == 0x02 && le_buf[3] == 0x01,
                   "LE wire bytes incorrect");

    /* BE: most significant byte first */
    SCENARIO_CHECK(be_buf[0] == 0x01 && be_buf[1] == 0x02 && be_buf[2] == 0x03 && be_buf[3] == 0x04,
                   "BE wire bytes incorrect");

    /* Cross-load: LE bytes read as BE should differ */
    uint32_t cross = asx_load_be_u32(le_buf);
    SCENARIO_CHECK(cross != val, "cross-endian load should produce different value");

    SCENARIO_END();
}

/* robust-endian-009: endian canary store and verify */
static void scenario_endian_canary(void) {
    SCENARIO_BEGIN("robust-endian-009.endian_canary");

    uint8_t le_canary[4], be_canary[4];

    asx_store_endian_canary_le(le_canary);
    SCENARIO_CHECK(asx_verify_endian_canary_le(le_canary) == 1, "LE canary verification failed");

    asx_store_endian_canary_be(be_canary);
    SCENARIO_CHECK(asx_verify_endian_canary_be(be_canary) == 1, "BE canary verification failed");

    /* Mismatched canary should fail */
    SCENARIO_CHECK(asx_verify_endian_canary_be(le_canary) == 0,
                   "LE canary should not verify as BE");
    SCENARIO_CHECK(asx_verify_endian_canary_le(be_canary) == 0,
                   "BE canary should not verify as LE");

    SCENARIO_END();
}

/* robust-endian-010: byte pattern identity (all bytes distinct) */
static void scenario_byte_pattern_identity(void) {
    SCENARIO_BEGIN("robust-endian-010.byte_pattern_identity");

    /* Store u64 with all distinct bytes, verify no byte swizzling */
    uint8_t buf[8];
    uint64_t val = 0x0102030405060708ULL;

    asx_store_le_u64(buf, val);
    SCENARIO_CHECK(buf[0] == 0x08, "LE byte[0]");
    SCENARIO_CHECK(buf[1] == 0x07, "LE byte[1]");
    SCENARIO_CHECK(buf[7] == 0x01, "LE byte[7]");

    asx_store_be_u64(buf, val);
    SCENARIO_CHECK(buf[0] == 0x01, "BE byte[0]");
    SCENARIO_CHECK(buf[1] == 0x02, "BE byte[1]");
    SCENARIO_CHECK(buf[7] == 0x08, "BE byte[7]");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void) {
    scenario_le_u16_roundtrip();
    scenario_le_u32_roundtrip();
    scenario_le_u64_roundtrip();
    scenario_be_u16_roundtrip();
    scenario_be_u32_roundtrip();
    scenario_be_u64_roundtrip();
    scenario_unaligned_access();
    scenario_cross_format_wire_bytes();
    scenario_endian_canary();
    scenario_byte_pattern_identity();

    fprintf(stderr, "[e2e] robustness_endian: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
