/*
 * buf.c — fixed-size byte buffer with read/write cursors
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/bytes/buf.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* asx_buf — immutable byte slice                                      */
/* ------------------------------------------------------------------ */

asx_buf asx_buf_from(const void *data, uint32_t len)
{
    asx_buf b;
    b.ptr = (const uint8_t *)data;
    b.len = (data != NULL) ? len : 0;
    return b;
}

asx_buf asx_buf_from_cstr(const char *cstr)
{
    asx_buf b;
    if (cstr == NULL) {
        b.ptr = NULL;
        b.len = 0;
    } else {
        b.ptr = (const uint8_t *)cstr;
        b.len = (uint32_t)strlen(cstr);
    }
    return b;
}

asx_buf asx_buf_empty(void)
{
    asx_buf b;
    b.ptr = NULL;
    b.len = 0;
    return b;
}

asx_buf asx_buf_slice(asx_buf src, uint32_t offset, uint32_t len)
{
    asx_buf b;
    if (src.ptr == NULL || offset >= src.len) {
        b.ptr = NULL;
        b.len = 0;
        return b;
    }
    if (offset + len > src.len) {
        b.ptr = NULL;
        b.len = 0;
        return b;
    }
    b.ptr = src.ptr + offset;
    b.len = len;
    return b;
}

int asx_buf_eq(asx_buf a, asx_buf b)
{
    if (a.len != b.len) return 0;
    if (a.len == 0) return 1;
    if (a.ptr == NULL || b.ptr == NULL) return (a.ptr == b.ptr);
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

/* ------------------------------------------------------------------ */
/* asx_buf_mut — mutable fixed-size buffer                             */
/* ------------------------------------------------------------------ */

void asx_buf_mut_init(asx_buf_mut *buf)
{
    if (buf == NULL) return;
    buf->rd_pos = 0;
    buf->wr_pos = 0;
    buf->capacity = ASX_BUF_CAPACITY;
}

void asx_buf_mut_clear(asx_buf_mut *buf)
{
    if (buf == NULL) return;
    buf->rd_pos = 0;
    buf->wr_pos = 0;
}

void asx_buf_mut_compact(asx_buf_mut *buf)
{
    uint32_t remaining;
    if (buf == NULL) return;
    if (buf->rd_pos == 0) return;

    remaining = buf->wr_pos - buf->rd_pos;
    if (remaining > 0) {
        memmove(buf->data, buf->data + buf->rd_pos, remaining);
    }
    buf->rd_pos = 0;
    buf->wr_pos = remaining;
}

/* ------------------------------------------------------------------ */
/* Write operations                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_buf_mut_put(asx_buf_mut *buf, const void *data, uint32_t len)
{
    if (buf == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len == 0) return ASX_OK;
    if (data == NULL) return ASX_E_INVALID_ARGUMENT;
    if (buf->wr_pos + len > buf->capacity)
        return ASX_E_RESOURCE_EXHAUSTED;

    memcpy(buf->data + buf->wr_pos, data, len);
    buf->wr_pos += len;
    return ASX_OK;
}

asx_status asx_buf_mut_put_u8(asx_buf_mut *buf, uint8_t val)
{
    return asx_buf_mut_put(buf, &val, 1);
}

asx_status asx_buf_mut_put_u16_be(asx_buf_mut *buf, uint16_t val)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(val >> 8);
    bytes[1] = (uint8_t)(val & 0xFF);
    return asx_buf_mut_put(buf, bytes, 2);
}

asx_status asx_buf_mut_put_u32_be(asx_buf_mut *buf, uint32_t val)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(val >> 24);
    bytes[1] = (uint8_t)((val >> 16) & 0xFF);
    bytes[2] = (uint8_t)((val >> 8) & 0xFF);
    bytes[3] = (uint8_t)(val & 0xFF);
    return asx_buf_mut_put(buf, bytes, 4);
}

asx_status asx_buf_mut_put_u64_be(asx_buf_mut *buf, uint64_t val)
{
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(val >> 56);
    bytes[1] = (uint8_t)((val >> 48) & 0xFF);
    bytes[2] = (uint8_t)((val >> 40) & 0xFF);
    bytes[3] = (uint8_t)((val >> 32) & 0xFF);
    bytes[4] = (uint8_t)((val >> 24) & 0xFF);
    bytes[5] = (uint8_t)((val >> 16) & 0xFF);
    bytes[6] = (uint8_t)((val >> 8) & 0xFF);
    bytes[7] = (uint8_t)(val & 0xFF);
    return asx_buf_mut_put(buf, bytes, 8);
}

/* ------------------------------------------------------------------ */
/* Read operations                                                     */
/* ------------------------------------------------------------------ */

asx_buf asx_buf_mut_readable(const asx_buf_mut *buf)
{
    asx_buf b;
    if (buf == NULL) {
        b.ptr = NULL;
        b.len = 0;
        return b;
    }
    b.ptr = buf->data + buf->rd_pos;
    b.len = buf->wr_pos - buf->rd_pos;
    return b;
}

uint32_t asx_buf_mut_remaining(const asx_buf_mut *buf)
{
    if (buf == NULL) return 0;
    return buf->wr_pos - buf->rd_pos;
}

uint32_t asx_buf_mut_writable(const asx_buf_mut *buf)
{
    if (buf == NULL) return 0;
    return buf->capacity - buf->wr_pos;
}

asx_status asx_buf_mut_advance(asx_buf_mut *buf, uint32_t n)
{
    if (buf == NULL) return ASX_E_INVALID_ARGUMENT;
    if (n > buf->wr_pos - buf->rd_pos)
        return ASX_E_INVALID_ARGUMENT;
    buf->rd_pos += n;
    return ASX_OK;
}

asx_status asx_buf_mut_get_u8(asx_buf_mut *buf, uint8_t *out)
{
    if (buf == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (buf->wr_pos - buf->rd_pos < 1) return ASX_E_INVALID_ARGUMENT;
    *out = buf->data[buf->rd_pos];
    buf->rd_pos += 1;
    return ASX_OK;
}

asx_status asx_buf_mut_get_u16_be(asx_buf_mut *buf, uint16_t *out)
{
    if (buf == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (buf->wr_pos - buf->rd_pos < 2) return ASX_E_INVALID_ARGUMENT;
    *out = (uint16_t)((uint16_t)buf->data[buf->rd_pos] << 8)
         | (uint16_t)buf->data[buf->rd_pos + 1];
    buf->rd_pos += 2;
    return ASX_OK;
}

asx_status asx_buf_mut_get_u32_be(asx_buf_mut *buf, uint32_t *out)
{
    if (buf == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (buf->wr_pos - buf->rd_pos < 4) return ASX_E_INVALID_ARGUMENT;
    *out = ((uint32_t)buf->data[buf->rd_pos] << 24)
         | ((uint32_t)buf->data[buf->rd_pos + 1] << 16)
         | ((uint32_t)buf->data[buf->rd_pos + 2] << 8)
         | (uint32_t)buf->data[buf->rd_pos + 3];
    buf->rd_pos += 4;
    return ASX_OK;
}

asx_status asx_buf_mut_get_u64_be(asx_buf_mut *buf, uint64_t *out)
{
    if (buf == NULL || out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (buf->wr_pos - buf->rd_pos < 8) return ASX_E_INVALID_ARGUMENT;
    *out = ((uint64_t)buf->data[buf->rd_pos] << 56)
         | ((uint64_t)buf->data[buf->rd_pos + 1] << 48)
         | ((uint64_t)buf->data[buf->rd_pos + 2] << 40)
         | ((uint64_t)buf->data[buf->rd_pos + 3] << 32)
         | ((uint64_t)buf->data[buf->rd_pos + 4] << 24)
         | ((uint64_t)buf->data[buf->rd_pos + 5] << 16)
         | ((uint64_t)buf->data[buf->rd_pos + 6] << 8)
         | (uint64_t)buf->data[buf->rd_pos + 7];
    buf->rd_pos += 8;
    return ASX_OK;
}

asx_status asx_buf_mut_peek(const asx_buf_mut *buf, void *dest, uint32_t n)
{
    if (buf == NULL || dest == NULL) return ASX_E_INVALID_ARGUMENT;
    if (n > buf->wr_pos - buf->rd_pos) return ASX_E_INVALID_ARGUMENT;
    memcpy(dest, buf->data + buf->rd_pos, n);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Freeze                                                              */
/* ------------------------------------------------------------------ */

asx_buf asx_buf_mut_freeze(const asx_buf_mut *buf)
{
    return asx_buf_mut_readable(buf);
}
