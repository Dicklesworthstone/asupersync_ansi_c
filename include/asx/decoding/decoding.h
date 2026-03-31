/*
 * asx/decoding/decoding.h — decoding pipeline abstraction
 *
 * Provides a staged decoding pipeline that composes an asx_read_adapter
 * with an asx_codec decoder to consume framed input with progress
 * tracking, error classification, and deterministic statistics.
 *
 * The decoding pipeline reads raw bytes from the adapter into an
 * internal receive buffer, then decodes frames through the configured
 * codec. Each step is observable: callers can query progress, stats,
 * and the first error that halted the pipeline.
 *
 * Upstream Rust parity: DecodingPipeline, DecodingConfig,
 * DecodingProgress, DecodingError.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_DECODING_H
#define ASX_DECODING_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/bytes/codec.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

#include <asx/bytes/io_adapter.h>

/* -------------------------------------------------------------------
 * Decoding error classification
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_DECODING_ERR_NONE = 0,     /* no error */
    ASX_DECODING_ERR_CODEC = 1,    /* codec reported a decode error */
    ASX_DECODING_ERR_IO = 2,       /* read adapter returned error */
    ASX_DECODING_ERR_OVERFLOW = 3, /* receive buffer exhausted before a frame was complete */
    ASX_DECODING_ERR_CANCELLED = 4 /* pipeline was cancelled */
} asx_decoding_error_kind;

/* Returns a human-readable string for a decoding error kind. */
ASX_API ASX_MUST_USE const char *asx_decoding_error_kind_str(asx_decoding_error_kind kind);

/* -------------------------------------------------------------------
 * Decoding progress
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t frames_decoded; /* frames successfully decoded */
    uint32_t bytes_consumed; /* raw bytes consumed from adapter */
    uint32_t bytes_buffered; /* bytes currently in receive buffer awaiting decode */
    int eof_reached;         /* nonzero if read adapter reported EOF */
    int complete;            /* nonzero if EOF reached and buffer fully drained */
} asx_decoding_progress;

/* -------------------------------------------------------------------
 * Decoding pipeline configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_frame_bytes; /* reject frames larger than this (0 = no limit) */
    uint32_t max_total_bytes; /* stop after consuming this many bytes (0 = no limit) */
    uint32_t max_frames;      /* stop after decoding this many frames (0 = no limit) */
} asx_decoding_config;

/* Initialize config with defaults (no limits). */
ASX_API void asx_decoding_config_init(asx_decoding_config *cfg);

/* -------------------------------------------------------------------
 * Decoding pipeline
 * ------------------------------------------------------------------- */

typedef struct {
    asx_codec codec;
    asx_read_adapter reader;
    asx_decoding_config config;
    asx_buf_mut recv_buf; /* internal receive buffer */
    asx_decoding_progress progress;
    asx_decoding_error_kind last_error;
    int cancelled;
} asx_decoding_pipeline;

/* Initialize a decoding pipeline with a codec, read adapter, and config.
 * The receive buffer is cleared and ready for use.
 * Returns ASX_E_INVALID_ARGUMENT if pipeline is NULL. */
ASX_API ASX_MUST_USE asx_status asx_decoding_pipeline_init(asx_decoding_pipeline *p,
                                                           asx_codec codec, asx_read_adapter reader,
                                                           const asx_decoding_config *cfg);

/* Attempt to decode the next frame from the pipeline.
 *
 * Reads from the adapter as needed to fill the receive buffer, then
 * attempts to decode one frame via the codec. If the codec needs more
 * data, another read is attempted.
 *
 * On success, out_frame is filled with the decoded frame data.
 * The frame data pointer is valid until the next decode call.
 *
 * Returns:
 *   ASX_OK                    — frame decoded into out_frame
 *   ASX_E_PENDING             — adapter returned PENDING (async: try later)
 *   ASX_E_NOT_FOUND           — EOF reached, no more frames available
 *   ASX_E_INVALID_ARGUMENT    — NULL pipeline or out_frame
 *   ASX_E_RESOURCE_EXHAUSTED  — receive buffer full, max_total_bytes, or max_frames
 *   ASX_E_CANCELLED           — pipeline was cancelled
 *   ASX_E_INVALID_STATE       — codec error or adapter error */
ASX_API ASX_MUST_USE asx_status asx_decoding_pipeline_decode(asx_decoding_pipeline *p,
                                                             asx_frame *out_frame);

/* Cancel the pipeline. All subsequent decode calls return ASX_E_CANCELLED. */
ASX_API void asx_decoding_pipeline_cancel(asx_decoding_pipeline *p);

/* Query current pipeline progress. */
ASX_API void asx_decoding_pipeline_progress(const asx_decoding_pipeline *p,
                                            asx_decoding_progress *out);

/* Query the last error kind (ASX_DECODING_ERR_NONE if no error). */
ASX_API ASX_MUST_USE asx_decoding_error_kind
asx_decoding_pipeline_last_error(const asx_decoding_pipeline *p);

/* Reset pipeline state for reuse (clears progress, recv_buf, error, cancel).
 * Codec and reader are preserved. */
ASX_API void asx_decoding_pipeline_reset(asx_decoding_pipeline *p);

#endif /* !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO */

#ifdef __cplusplus
}
#endif

#endif /* ASX_DECODING_H */
