/*
 * codec.c — framed codec implementations
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/bytes/codec.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Length-delimited codec                                               */
/* ------------------------------------------------------------------ */

void asx_length_delimited_codec_init(asx_length_delimited_codec *c)
{
    if (c == NULL) return;
    c->length_field_size = 4;
    c->max_frame_len = 0;
}

asx_status asx_length_delimited_codec_init_with(
    asx_length_delimited_codec *c,
    uint8_t length_field_size,
    uint32_t max_frame_len)
{
    if (c == NULL) return ASX_E_INVALID_ARGUMENT;
    if (length_field_size != 1 && length_field_size != 2 && length_field_size != 4)
        return ASX_E_INVALID_ARGUMENT;
    c->length_field_size = length_field_size;
    c->max_frame_len = max_frame_len;
    return ASX_OK;
}

asx_decode_result asx_length_delimited_decode(void *codec_state,
                                               asx_buf_mut *src,
                                               asx_frame *out_frame)
{
    asx_length_delimited_codec *c = (asx_length_delimited_codec *)codec_state;
    uint32_t remaining;
    uint32_t frame_len = 0;
    uint8_t lfs;

    if (c == NULL || src == NULL || out_frame == NULL)
        return ASX_DECODE_ERROR;

    remaining = asx_buf_mut_remaining(src);
    lfs = c->length_field_size;

    /* Need at least the length field */
    if (remaining < lfs)
        return ASX_DECODE_NEED_MORE;

    /* Read length without consuming */
    if (lfs == 1) {
        frame_len = src->data[src->rd_pos];
    } else if (lfs == 2) {
        frame_len = ((uint32_t)src->data[src->rd_pos] << 8)
                  | (uint32_t)src->data[src->rd_pos + 1];
    } else {
        frame_len = ((uint32_t)src->data[src->rd_pos] << 24)
                  | ((uint32_t)src->data[src->rd_pos + 1] << 16)
                  | ((uint32_t)src->data[src->rd_pos + 2] << 8)
                  | (uint32_t)src->data[src->rd_pos + 3];
    }

    /* Check max frame length */
    if (c->max_frame_len > 0 && frame_len > c->max_frame_len)
        return ASX_DECODE_ERROR;

    /* Need length field + payload */
    if (remaining < lfs + frame_len)
        return ASX_DECODE_NEED_MORE;

    /* Produce frame */
    out_frame->data = src->data + src->rd_pos + lfs;
    out_frame->len = frame_len;
    src->rd_pos += lfs + frame_len;
    return ASX_DECODE_FRAME;
}

asx_status asx_length_delimited_encode(void *codec_state,
                                        const void *data,
                                        uint32_t len,
                                        asx_buf_mut *dst)
{
    asx_length_delimited_codec *c = (asx_length_delimited_codec *)codec_state;
    asx_status st;

    if (c == NULL || dst == NULL) return ASX_E_INVALID_ARGUMENT;
    if (c->max_frame_len > 0 && len > c->max_frame_len)
        return ASX_E_INVALID_ARGUMENT;

    /* Write length field */
    if (c->length_field_size == 1) {
        if (len > 255) return ASX_E_INVALID_ARGUMENT;
        st = asx_buf_mut_put_u8(dst, (uint8_t)len);
    } else if (c->length_field_size == 2) {
        if (len > 65535) return ASX_E_INVALID_ARGUMENT;
        st = asx_buf_mut_put_u16_be(dst, (uint16_t)len);
    } else {
        st = asx_buf_mut_put_u32_be(dst, len);
    }
    if (st != ASX_OK) return st;

    /* Write payload */
    if (len > 0 && data != NULL) {
        st = asx_buf_mut_put(dst, data, len);
        if (st != ASX_OK) return st;
    }

    return ASX_OK;
}

asx_codec asx_length_delimited_as_codec(asx_length_delimited_codec *c)
{
    asx_codec codec;
    codec.decode = asx_length_delimited_decode;
    codec.encode = asx_length_delimited_encode;
    codec.state = c;
    return codec;
}

/* ------------------------------------------------------------------ */
/* Lines codec                                                         */
/* ------------------------------------------------------------------ */

void asx_lines_codec_init(asx_lines_codec *c)
{
    if (c == NULL) return;
    c->max_line_len = 0;
}

void asx_lines_codec_init_with(asx_lines_codec *c, uint32_t max_line_len)
{
    if (c == NULL) return;
    c->max_line_len = max_line_len;
}

asx_decode_result asx_lines_decode(void *codec_state,
                                    asx_buf_mut *src,
                                    asx_frame *out_frame)
{
    asx_lines_codec *c = (asx_lines_codec *)codec_state;
    uint32_t remaining;
    uint32_t i;
    uint32_t line_end;
    uint32_t frame_len;

    if (src == NULL || out_frame == NULL)
        return ASX_DECODE_ERROR;

    remaining = asx_buf_mut_remaining(src);
    if (remaining == 0)
        return ASX_DECODE_NEED_MORE;

    /* Scan for newline */
    for (i = 0; i < remaining; i++) {
        if (src->data[src->rd_pos + i] == '\n') {
            line_end = i;
            frame_len = line_end;

            /* Strip trailing \r */
            if (frame_len > 0 && src->data[src->rd_pos + frame_len - 1] == '\r')
                frame_len--;

            /* Check max line length */
            if (c != NULL && c->max_line_len > 0 && frame_len > c->max_line_len)
                return ASX_DECODE_ERROR;

            out_frame->data = src->data + src->rd_pos;
            out_frame->len = frame_len;
            src->rd_pos += line_end + 1;  /* consume including \n */
            return ASX_DECODE_FRAME;
        }
    }

    /* No newline found — check if we've exceeded max */
    if (c != NULL && c->max_line_len > 0 && remaining > c->max_line_len)
        return ASX_DECODE_ERROR;

    return ASX_DECODE_NEED_MORE;
}

asx_status asx_lines_encode(void *codec_state,
                             const void *data,
                             uint32_t len,
                             asx_buf_mut *dst)
{
    asx_status st;
    (void)codec_state;

    if (dst == NULL) return ASX_E_INVALID_ARGUMENT;

    if (len > 0 && data != NULL) {
        st = asx_buf_mut_put(dst, data, len);
        if (st != ASX_OK) return st;
    }

    /* Append newline */
    st = asx_buf_mut_put_u8(dst, (uint8_t)'\n');
    return st;
}

asx_codec asx_lines_as_codec(asx_lines_codec *c)
{
    asx_codec codec;
    codec.decode = asx_lines_decode;
    codec.encode = asx_lines_encode;
    codec.state = c;
    return codec;
}

/* ------------------------------------------------------------------ */
/* Bytes codec (pass-through)                                          */
/* ------------------------------------------------------------------ */

asx_decode_result asx_bytes_decode(void *codec_state,
                                    asx_buf_mut *src,
                                    asx_frame *out_frame)
{
    uint32_t remaining;
    (void)codec_state;

    if (src == NULL || out_frame == NULL)
        return ASX_DECODE_ERROR;

    remaining = asx_buf_mut_remaining(src);
    if (remaining == 0)
        return ASX_DECODE_NEED_MORE;

    out_frame->data = src->data + src->rd_pos;
    out_frame->len = remaining;
    src->rd_pos = src->wr_pos;
    return ASX_DECODE_FRAME;
}

asx_status asx_bytes_encode(void *codec_state,
                             const void *data,
                             uint32_t len,
                             asx_buf_mut *dst)
{
    (void)codec_state;
    if (dst == NULL) return ASX_E_INVALID_ARGUMENT;
    if (len == 0) return ASX_OK;
    if (data == NULL) return ASX_E_INVALID_ARGUMENT;
    return asx_buf_mut_put(dst, data, len);
}

asx_codec asx_bytes_as_codec(void)
{
    asx_codec codec;
    codec.decode = asx_bytes_decode;
    codec.encode = asx_bytes_encode;
    codec.state = NULL;
    return codec;
}
