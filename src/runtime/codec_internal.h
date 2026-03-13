/*
 * codec_internal.h — shared buffer helpers for codec implementations
 *
 * Internal header for functions shared between hooks.c (JSON/BIN codecs)
 * and equivalence.c (cross-codec verification). Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CODEC_INTERNAL_H
#define ASX_CODEC_INTERNAL_H

#include <asx/asx_status.h>
#include <asx/codec/codec.h>
#include <stddef.h>

asx_status asx_codec_buffer_append_bytes(asx_codec_buffer *buf, const char *data, size_t len);
asx_status asx_codec_buffer_append_cstr(asx_codec_buffer *buf, const char *text);
asx_status asx_codec_buffer_append_char(asx_codec_buffer *buf, char ch);
asx_status asx_codec_buffer_append_u64(asx_codec_buffer *buf, uint64_t value);
asx_status asx_codec_buffer_append_json_string(asx_codec_buffer *buf, const char *text);
asx_status asx_codec_buffer_append_field_prefix(asx_codec_buffer *buf, int *is_first);
asx_status asx_codec_buffer_append_string_field(asx_codec_buffer *buf, int *is_first,
                                                const char *key, const char *value);
asx_status asx_codec_buffer_append_u64_field(asx_codec_buffer *buf, int *is_first, const char *key,
                                             uint64_t value);

#endif /* ASX_CODEC_INTERNAL_H */
