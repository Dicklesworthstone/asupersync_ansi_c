/*
 * asx/bytes/buf.h — fixed-size byte buffer with read/write cursors
 *
 * Provides the foundational buffer abstraction for the async runtime:
 *   asx_buf      — immutable byte slice (pointer + length)
 *   asx_buf_mut  — mutable fixed-size byte buffer with reader/writer cursors
 *
 * All storage is inline (no dynamic allocation). Buffer capacity is
 * a compile-time constant suitable for embedded/constrained profiles.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_BYTES_BUF_H
#define ASX_BYTES_BUF_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Compile-time buffer capacity
 * ------------------------------------------------------------------- */

#ifndef ASX_BUF_CAPACITY
#define ASX_BUF_CAPACITY 4096u
#endif

/* -------------------------------------------------------------------
 * asx_buf — immutable byte slice
 * ------------------------------------------------------------------- */

typedef struct {
    const uint8_t *ptr;
    uint32_t len;
} asx_buf;

/* Create a buf from a pointer and length. */
ASX_API asx_buf asx_buf_from(const void *data, uint32_t len);

/* Create a buf from a NUL-terminated C string (excluding NUL). */
ASX_API asx_buf asx_buf_from_cstr(const char *cstr);

/* Create an empty buf. */
ASX_API asx_buf asx_buf_empty(void);

/* Return a sub-slice of buf starting at offset with given length.
 * Returns empty buf if offset+len exceeds the source. */
ASX_API asx_buf asx_buf_slice(asx_buf src, uint32_t offset, uint32_t len);

/* Compare two bufs for byte equality. Returns 1 if equal, 0 otherwise. */
ASX_API int asx_buf_eq(asx_buf a, asx_buf b);

/* -------------------------------------------------------------------
 * asx_buf_mut — mutable fixed-size byte buffer
 *
 * Layout:  [consumed bytes | readable bytes | writable space]
 *           0..rd_pos       rd_pos..wr_pos   wr_pos..capacity
 *
 * rd_pos: next byte to read
 * wr_pos: next byte to write
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t data[ASX_BUF_CAPACITY];
    uint32_t rd_pos;
    uint32_t wr_pos;
    uint32_t capacity;
} asx_buf_mut;

/* Initialize a mutable buffer (empty, full capacity). */
ASX_API void asx_buf_mut_init(asx_buf_mut *buf);

/* Reset a buffer to empty state. */
ASX_API void asx_buf_mut_clear(asx_buf_mut *buf);

/* Compact: move unread data to the front, reclaiming consumed space. */
ASX_API void asx_buf_mut_compact(asx_buf_mut *buf);

/* -------------------------------------------------------------------
 * Write operations (append to writable region)
 * ------------------------------------------------------------------- */

/* Write bytes into the buffer. Returns ASX_E_RESOURCE_EXHAUSTED if
 * there isn't enough writable space. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_put(asx_buf_mut *buf, const void *data, uint32_t len);

/* Write a single byte. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_put_u8(asx_buf_mut *buf, uint8_t val);

/* Write a uint16 in big-endian. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_put_u16_be(asx_buf_mut *buf, uint16_t val);

/* Write a uint32 in big-endian. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_put_u32_be(asx_buf_mut *buf, uint32_t val);

/* Write a uint64 in big-endian. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_put_u64_be(asx_buf_mut *buf, uint64_t val);

/* -------------------------------------------------------------------
 * Read operations (consume from readable region)
 * ------------------------------------------------------------------- */

/* Get a read-only view of the readable region (does not consume). */
ASX_API asx_buf asx_buf_mut_readable(const asx_buf_mut *buf);

/* Number of readable bytes. */
ASX_API uint32_t asx_buf_mut_remaining(const asx_buf_mut *buf);

/* Number of writable bytes. */
ASX_API uint32_t asx_buf_mut_writable(const asx_buf_mut *buf);

/* Advance the read cursor by n bytes (consume). Returns
 * ASX_E_INVALID_ARGUMENT if n > remaining. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_advance(asx_buf_mut *buf, uint32_t n);

/* Read and consume a single byte. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_get_u8(asx_buf_mut *buf, uint8_t *out);

/* Read and consume a uint16 in big-endian. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_get_u16_be(asx_buf_mut *buf, uint16_t *out);

/* Read and consume a uint32 in big-endian. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_get_u32_be(asx_buf_mut *buf, uint32_t *out);

/* Read and consume a uint64 in big-endian. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_get_u64_be(asx_buf_mut *buf, uint64_t *out);

/* Copy n bytes from readable region into dest without consuming.
 * Returns ASX_E_INVALID_ARGUMENT if n > remaining. */
ASX_API ASX_MUST_USE asx_status asx_buf_mut_peek(const asx_buf_mut *buf, void *dest, uint32_t n);

/* -------------------------------------------------------------------
 * Freeze: convert mutable buffer contents to immutable buf
 * ------------------------------------------------------------------- */

/* Return an immutable view of the readable region. The returned buf
 * is valid only as long as the underlying buf_mut is not modified. */
ASX_API asx_buf asx_buf_mut_freeze(const asx_buf_mut *buf);

#ifdef __cplusplus
}
#endif

#endif /* ASX_BYTES_BUF_H */
