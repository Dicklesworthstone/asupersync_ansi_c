/*
 * asx/bytes/codec.h — framed codec abstraction and built-in codecs
 *
 * Provides the Encoder/Decoder trait equivalent via function pointers,
 * plus built-in codecs: length-delimited, lines, and pass-through bytes.
 *
 * Codecs operate on asx_buf_mut buffers. The decoder reads from a
 * source buffer and produces frames. The encoder writes frames into
 * a destination buffer.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_BYTES_CODEC_H
#define ASX_BYTES_CODEC_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Codec decode result
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DECODE_FRAME    = 0,  /* a complete frame was decoded */
    ASX_DECODE_NEED_MORE = 1, /* not enough data, need more input */
    ASX_DECODE_ERROR    = 2   /* decode error */
} asx_decode_result;

/* A decoded frame: pointer into the source buffer + length. */
typedef struct {
    const uint8_t *data;
    uint32_t       len;
} asx_frame;

/* -------------------------------------------------------------------
 * Codec trait (function pointers)
 * ------------------------------------------------------------------- */

/* Decoder: attempt to decode one frame from src buffer.
 * On ASX_DECODE_FRAME, out_frame is filled and src is advanced past
 * the consumed bytes.
 * On ASX_DECODE_NEED_MORE, src is not modified.
 * On ASX_DECODE_ERROR, the codec is in an error state. */
typedef asx_decode_result (*asx_decode_fn)(void *codec_state,
                                            asx_buf_mut *src,
                                            asx_frame *out_frame);

/* Encoder: encode a frame (data + len) into dst buffer.
 * Returns ASX_OK on success, ASX_E_RESOURCE_EXHAUSTED if dst is full. */
typedef asx_status (*asx_encode_fn)(void *codec_state,
                                     const void *data,
                                     uint32_t len,
                                     asx_buf_mut *dst);

typedef struct {
    asx_decode_fn  decode;
    asx_encode_fn  encode;
    void          *state;
} asx_codec;

/* -------------------------------------------------------------------
 * Length-delimited codec
 *
 * Wire format: [length:N bytes big-endian] [payload:length bytes]
 * N is configurable (1, 2, or 4 bytes).
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t  length_field_size;  /* 1, 2, or 4 */
    uint32_t max_frame_len;      /* 0 = no limit (up to buffer capacity) */
} asx_length_delimited_codec;

/* Initialize a length-delimited codec with default settings (4-byte length). */
ASX_API void asx_length_delimited_codec_init(asx_length_delimited_codec *c);

/* Initialize with specific length field size (1, 2, or 4). */
ASX_API ASX_MUST_USE asx_status asx_length_delimited_codec_init_with(
    asx_length_delimited_codec *c,
    uint8_t length_field_size,
    uint32_t max_frame_len);

/* Decode/encode functions for use in asx_codec. */
ASX_API asx_decode_result asx_length_delimited_decode(void *codec_state,
                                                       asx_buf_mut *src,
                                                       asx_frame *out_frame);
ASX_API asx_status asx_length_delimited_encode(void *codec_state,
                                                const void *data,
                                                uint32_t len,
                                                asx_buf_mut *dst);

/* Convenience: build an asx_codec from a length_delimited_codec. */
ASX_API asx_codec asx_length_delimited_as_codec(asx_length_delimited_codec *c);

/* -------------------------------------------------------------------
 * Lines codec
 *
 * Frames delimited by '\n'. Strips trailing '\r\n' or '\n'.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_line_len;  /* 0 = no limit */
} asx_lines_codec;

/* Initialize a lines codec. */
ASX_API void asx_lines_codec_init(asx_lines_codec *c);

/* Initialize with max line length. */
ASX_API void asx_lines_codec_init_with(asx_lines_codec *c, uint32_t max_line_len);

/* Decode/encode functions. */
ASX_API asx_decode_result asx_lines_decode(void *codec_state,
                                            asx_buf_mut *src,
                                            asx_frame *out_frame);
ASX_API asx_status asx_lines_encode(void *codec_state,
                                     const void *data,
                                     uint32_t len,
                                     asx_buf_mut *dst);

/* Convenience: build an asx_codec from a lines_codec. */
ASX_API asx_codec asx_lines_as_codec(asx_lines_codec *c);

/* -------------------------------------------------------------------
 * Bytes codec (pass-through)
 *
 * Each decode returns all available bytes as one frame.
 * Each encode writes bytes directly.
 * ------------------------------------------------------------------- */

ASX_API asx_decode_result asx_bytes_decode(void *codec_state,
                                            asx_buf_mut *src,
                                            asx_frame *out_frame);
ASX_API asx_status asx_bytes_encode(void *codec_state,
                                     const void *data,
                                     uint32_t len,
                                     asx_buf_mut *dst);

/* Build a pass-through codec (no state needed). */
ASX_API asx_codec asx_bytes_as_codec(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_BYTES_CODEC_H */
