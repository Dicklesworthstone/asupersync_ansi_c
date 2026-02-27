/*
 * asx/codec/codec.h — codec abstraction and canonical fixture IO
 *
 * JSON is the bring-up baseline. BIN is present as an explicit surface
 * but may return unsupported until implemented.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CODEC_CODEC_H
#define ASX_CODEC_CODEC_H

#include <stddef.h>
#include <asx/asx_export.h>
#include <asx/codec/schema.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} asx_codec_buffer;

typedef struct asx_codec_vtable {
    asx_codec_kind codec;
    asx_status (*encode_fixture)(const asx_canonical_fixture *fixture,
                                 asx_codec_buffer *out_json);
    asx_status (*decode_fixture)(const char *json,
                                 asx_canonical_fixture *out_fixture);
} asx_codec_vtable;

/* Buffer lifecycle helpers */
ASX_API void asx_codec_buffer_init(asx_codec_buffer *buf);
ASX_API void asx_codec_buffer_reset(asx_codec_buffer *buf);

/* Codec dispatch table lookup */
ASX_API ASX_MUST_USE const asx_codec_vtable *asx_codec_vtable_for(asx_codec_kind codec);

/* Generic wrappers that dispatch via codec vtable */
ASX_API ASX_MUST_USE asx_status asx_codec_encode_fixture(asx_codec_kind codec,
                                                         const asx_canonical_fixture *fixture,
                                                         asx_codec_buffer *out_payload);
ASX_API ASX_MUST_USE asx_status asx_codec_decode_fixture(asx_codec_kind codec,
                                                         const char *payload,
                                                         asx_canonical_fixture *out_fixture);

/* JSON baseline helpers (codec vtable backing functions) */
ASX_API ASX_MUST_USE asx_status asx_codec_encode_fixture_json(const asx_canonical_fixture *fixture,
                                                              asx_codec_buffer *out_json);
ASX_API ASX_MUST_USE asx_status asx_codec_decode_fixture_json(const char *json,
                                                              asx_canonical_fixture *out_fixture);

/* Build deterministic replay key from canonical semantic fields. */
ASX_API ASX_MUST_USE asx_status asx_codec_fixture_replay_key(const asx_canonical_fixture *fixture,
                                                             asx_codec_buffer *out_key);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CODEC_CODEC_H */
