/*
 * asx/encoding/encoding.h — encoding pipeline abstraction
 *
 * Provides a staged encoding pipeline that composes an asx_codec encoder
 * with an asx_write_adapter to produce framed output with progress
 * tracking, error classification, and deterministic statistics.
 *
 * The encoding pipeline accepts raw data, encodes it through the
 * configured codec, and writes framed output to the destination adapter.
 * Each step is observable: callers can query progress, stats, and the
 * first error that halted the pipeline.
 *
 * Upstream Rust parity: EncodingPipeline, EncodingStats, EncodedSymbol,
 * EncodingError.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ENCODING_H
#define ASX_ENCODING_H

#include <asx/asx_export.h>
#include <asx/asx_config.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/bytes/codec.h>
#include <asx/bytes/io_adapter.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

/* -------------------------------------------------------------------
 * Encoding error classification
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ENCODING_ERR_NONE = 0,       /* no error */
    ASX_ENCODING_ERR_CODEC = 1,      /* codec rejected the input */
    ASX_ENCODING_ERR_IO = 2,         /* write adapter returned error/closed */
    ASX_ENCODING_ERR_OVERFLOW = 3,   /* staging buffer exhausted */
    ASX_ENCODING_ERR_CANCELLED = 4   /* pipeline was cancelled */
} asx_encoding_error_kind;

/* Returns a human-readable string for an encoding error kind. */
ASX_API ASX_MUST_USE const char *asx_encoding_error_kind_str(asx_encoding_error_kind kind);

/* -------------------------------------------------------------------
 * Encoding statistics
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t frames_encoded;   /* frames successfully encoded */
    uint32_t bytes_input;      /* total raw input bytes accepted */
    uint32_t bytes_output;     /* total framed bytes written to adapter */
    uint32_t frames_rejected;  /* frames rejected by codec or size limits */
} asx_encoding_stats;

/* -------------------------------------------------------------------
 * Encoded symbol (one encoded output frame)
 * ------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;  /* pointer into staging buffer (valid until next encode) */
    uint32_t len;         /* encoded frame length in bytes */
    uint32_t sequence;    /* monotonic sequence number within pipeline */
} asx_encoded_symbol;

/* -------------------------------------------------------------------
 * Encoding pipeline configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_frame_bytes;  /* max single frame size (0 = no limit) */
    uint32_t max_total_bytes;  /* max cumulative output (0 = no limit) */
    int flush_after_each;      /* if nonzero, flush adapter after each frame */
} asx_encoding_config;

/* Initialize config with defaults (no limits, no auto-flush). */
ASX_API void asx_encoding_config_init(asx_encoding_config *cfg);

/* -------------------------------------------------------------------
 * Encoding pipeline
 * ------------------------------------------------------------------- */

typedef struct {
    asx_codec codec;
    asx_write_adapter writer;
    asx_encoding_config config;
    asx_buf_mut staging;          /* internal staging buffer for codec output */
    asx_encoding_stats stats;
    asx_encoding_error_kind last_error;
    int cancelled;
} asx_encoding_pipeline;

/* Initialize an encoding pipeline with a codec, write adapter, and config.
 * The staging buffer is cleared and ready for use.
 * Returns ASX_E_INVALID_ARGUMENT if pipeline, codec, or writer is NULL. */
ASX_API ASX_MUST_USE asx_status asx_encoding_pipeline_init(asx_encoding_pipeline *p,
                                                            asx_codec codec,
                                                            asx_write_adapter writer,
                                                            const asx_encoding_config *cfg);

/* Encode a single frame of raw data through the pipeline.
 * The data is first encoded by the codec into the staging buffer,
 * then the staging buffer contents are written to the adapter.
 *
 * On success, if out_symbol is non-NULL it is filled with the
 * encoded frame details.
 *
 * Returns:
 *   ASX_OK                    — frame encoded and written
 *   ASX_E_INVALID_ARGUMENT    — NULL pipeline or (NULL data with len > 0)
 *   ASX_E_RESOURCE_EXHAUSTED  — staging buffer or max_total_bytes exceeded
 *   ASX_E_CANCELLED           — pipeline was cancelled
 *   ASX_E_INVALID_STATE       — write adapter returned error/closed */
ASX_API ASX_MUST_USE asx_status asx_encoding_pipeline_encode(asx_encoding_pipeline *p,
                                                              const void *data, uint32_t len,
                                                              asx_encoded_symbol *out_symbol);

/* Flush the write adapter, ensuring all buffered output is transmitted.
 * Returns ASX_OK when flush completes, or an error if the adapter fails. */
ASX_API ASX_MUST_USE asx_status asx_encoding_pipeline_flush(asx_encoding_pipeline *p);

/* Cancel the pipeline. All subsequent encode calls return ASX_E_CANCELLED. */
ASX_API void asx_encoding_pipeline_cancel(asx_encoding_pipeline *p);

/* Query current pipeline statistics. */
ASX_API void asx_encoding_pipeline_stats(const asx_encoding_pipeline *p,
                                          asx_encoding_stats *out);

/* Query the last error kind (ASX_ENCODING_ERR_NONE if no error). */
ASX_API ASX_MUST_USE asx_encoding_error_kind
asx_encoding_pipeline_last_error(const asx_encoding_pipeline *p);

/* Reset pipeline state for reuse (clears stats, staging, error, cancel).
 * Codec and writer are preserved. */
ASX_API void asx_encoding_pipeline_reset(asx_encoding_pipeline *p);

#endif /* !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO */

#ifdef __cplusplus
}
#endif

#endif /* ASX_ENCODING_H */
