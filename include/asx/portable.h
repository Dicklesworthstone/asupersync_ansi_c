/*
 * asx/portable.h — portable endian and unaligned-access helpers
 *
 * Centralizes all multi-byte load/store operations into audited,
 * byte-by-byte helpers that are safe on any alignment and endianness.
 * No pointer casts, no platform-specific intrinsics, pure C99.
 *
 * Wire formats:
 *   - Binary fixture codec: big-endian (network byte order)
 *   - Binary trace persistence: little-endian
 *
 * See docs/C_PORTABILITY_RULES.md P-WRAP-003 for requirements.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_PORTABLE_H
#define ASX_PORTABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Compile-time endian detection (informational only)
 *
 * The load/store helpers work correctly regardless of host byte order.
 * These defines exist for diagnostics and compile-time assertions.
 * ------------------------------------------------------------------- */

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ASX_ENDIAN_LITTLE 1
#define ASX_ENDIAN_BIG 0
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ASX_ENDIAN_LITTLE 0
#define ASX_ENDIAN_BIG 1
#else
#error "asx requires little-endian or big-endian byte order"
#endif
#else
/* Fallback: assume little-endian (x86/ARM default) */
#define ASX_ENDIAN_LITTLE 1
#define ASX_ENDIAN_BIG 0
#endif

/* -------------------------------------------------------------------
 * Little-endian load helpers (byte-by-byte, alignment-safe)
 * ------------------------------------------------------------------- */

static inline uint16_t asx_load_le_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t asx_load_le_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t asx_load_le_u64(const uint8_t *p) {
    return (uint64_t)asx_load_le_u32(p) | ((uint64_t)asx_load_le_u32(p + 4) << 32);
}

/* -------------------------------------------------------------------
 * Little-endian store helpers (byte-by-byte, alignment-safe)
 * ------------------------------------------------------------------- */

static inline void asx_store_le_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void asx_store_le_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void asx_store_le_u64(uint8_t *p, uint64_t v) {
    asx_store_le_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    asx_store_le_u32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* -------------------------------------------------------------------
 * Big-endian load helpers (byte-by-byte, alignment-safe)
 * ------------------------------------------------------------------- */

static inline uint16_t asx_load_be_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t asx_load_be_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t asx_load_be_u64(const uint8_t *p) {
    return ((uint64_t)asx_load_be_u32(p) << 32) | (uint64_t)asx_load_be_u32(p + 4);
}

/* -------------------------------------------------------------------
 * Big-endian store helpers (byte-by-byte, alignment-safe)
 * ------------------------------------------------------------------- */

static inline void asx_store_be_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static inline void asx_store_be_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static inline void asx_store_be_u64(uint8_t *p, uint64_t v) {
    asx_store_be_u32(p, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
    asx_store_be_u32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}

/* -------------------------------------------------------------------
 * Byte-order canary for runtime endian verification
 *
 * Stores a known pattern that can be validated at load time to
 * detect byte-order mismatches between producer and consumer.
 * ------------------------------------------------------------------- */

#define ASX_ENDIAN_CANARY_LE 0x04030201u
#define ASX_ENDIAN_CANARY_BE 0x01020304u

static inline void asx_store_endian_canary_le(uint8_t *p) {
    p[0] = 0x01;
    p[1] = 0x02;
    p[2] = 0x03;
    p[3] = 0x04;
}

static inline int asx_verify_endian_canary_le(const uint8_t *p) {
    return p[0] == 0x01 && p[1] == 0x02 && p[2] == 0x03 && p[3] == 0x04;
}

static inline void asx_store_endian_canary_be(uint8_t *p) {
    p[0] = 0x04;
    p[1] = 0x03;
    p[2] = 0x02;
    p[3] = 0x01;
}

static inline int asx_verify_endian_canary_be(const uint8_t *p) {
    return p[0] == 0x04 && p[1] == 0x03 && p[2] == 0x02 && p[3] == 0x01;
}

#ifdef __cplusplus
}
#endif

#endif /* ASX_PORTABLE_H */
