/*
 * asx/codec/codec.h — codec abstraction and canonical fixture IO
 *
 * JSON is the bring-up baseline. BIN provides a compact framed transport
 * with explicit schema/version metadata and optional checksum footer.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CODEC_CODEC_H
#define ASX_CODEC_CODEC_H

#include <asx/asx_export.h>
#include <asx/codec/schema.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} asx_codec_buffer;

typedef struct {
    const char *ptr;
    size_t len;
} asx_codec_slice;

typedef struct {
    uint8_t frame_schema_version;
    uint8_t message_type;
    uint8_t flags;
    asx_codec_kind codec;
    uint64_t seed;
    asx_codec_slice scenario_id;
    asx_codec_slice fixture_schema_version;
    asx_codec_slice scenario_dsl_version;
    asx_codec_slice profile;
    asx_codec_slice input_json;
    asx_codec_slice expected_events_json;
    asx_codec_slice expected_final_snapshot_json;
    asx_codec_slice expected_error_codes_json;
    asx_codec_slice semantic_digest;
    asx_codec_slice rust_baseline_commit;
    asx_codec_slice rust_toolchain_commit_hash;
    asx_codec_slice rust_toolchain_release;
    asx_codec_slice rust_toolchain_host;
    asx_codec_slice cargo_lock_sha256;
    asx_codec_slice capture_run_id;
} asx_codec_bin_fixture_view;

typedef struct asx_codec_vtable {
    asx_codec_kind codec;
    asx_status (*encode_fixture)(const asx_canonical_fixture *fixture, asx_codec_buffer *out_json);
    asx_status (*decode_fixture)(const void *payload, size_t payload_len,
                                 asx_canonical_fixture *out_fixture);
} asx_codec_vtable;

/* Buffer lifecycle helpers */
ASX_API void asx_codec_buffer_init(asx_codec_buffer *buf);
ASX_API void asx_codec_buffer_reset(asx_codec_buffer *buf);

/* Buffer append primitives */
ASX_API asx_status asx_codec_buffer_append_bytes(asx_codec_buffer *buf, const char *bytes,
                                                 size_t len);
ASX_API asx_status asx_codec_buffer_append_cstr(asx_codec_buffer *buf, const char *text);
ASX_API asx_status asx_codec_buffer_append_char(asx_codec_buffer *buf, char ch);
ASX_API asx_status asx_codec_buffer_append_u64(asx_codec_buffer *buf, uint64_t value);
ASX_API asx_status asx_codec_buffer_append_json_string(asx_codec_buffer *buf, const char *text);
/* Append JSON field separator (comma unless first field). */
ASX_API asx_status asx_codec_buffer_append_field_prefix(asx_codec_buffer *buf, int *is_first);
/* Append a key-value string field in JSON format. */
ASX_API asx_status asx_codec_buffer_append_string_field(asx_codec_buffer *buf, int *is_first,
                                                        const char *key, const char *value);
/* Append a key-value uint64 field in JSON format. */
ASX_API asx_status asx_codec_buffer_append_u64_field(asx_codec_buffer *buf, int *is_first,
                                                     const char *key, uint64_t value);

/* Codec dispatch table lookup */
ASX_API ASX_MUST_USE const asx_codec_vtable *asx_codec_vtable_for(asx_codec_kind codec);

/* Generic wrappers that dispatch via codec vtable */
ASX_API ASX_MUST_USE asx_status asx_codec_encode_fixture(asx_codec_kind codec,
                                                         const asx_canonical_fixture *fixture,
                                                         asx_codec_buffer *out_payload);
ASX_API ASX_MUST_USE asx_status asx_codec_decode_fixture(asx_codec_kind codec, const void *payload,
                                                         size_t payload_len,
                                                         asx_canonical_fixture *out_fixture);

/* JSON baseline helpers (codec vtable backing functions) */
ASX_API ASX_MUST_USE asx_status asx_codec_encode_fixture_json(const asx_canonical_fixture *fixture,
                                                              asx_codec_buffer *out_json);
ASX_API ASX_MUST_USE asx_status asx_codec_decode_fixture_json(const char *json,
                                                              asx_canonical_fixture *out_fixture);

/* Binary decode view helper for safe zero-copy payload inspection. */
ASX_API void asx_codec_bin_fixture_view_init(asx_codec_bin_fixture_view *view);
ASX_API ASX_MUST_USE asx_status asx_codec_decode_fixture_bin_view(
    const void *payload, size_t payload_len, asx_codec_bin_fixture_view *out_view);

/* Build deterministic replay key from canonical semantic fields. */
ASX_API ASX_MUST_USE asx_status asx_codec_fixture_replay_key(const asx_canonical_fixture *fixture,
                                                             asx_codec_buffer *out_key);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CODEC_CODEC_H */
