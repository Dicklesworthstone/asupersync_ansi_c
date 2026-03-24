/*
 * encoding.c — encoding pipeline implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/encoding/encoding.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

/* ------------------------------------------------------------------ */
/* Error kind strings                                                  */
/* ------------------------------------------------------------------ */

const char *asx_encoding_error_kind_str(asx_encoding_error_kind kind) {
    switch (kind) {
    case ASX_ENCODING_ERR_NONE:      return "none";
    case ASX_ENCODING_ERR_CODEC:     return "codec";
    case ASX_ENCODING_ERR_IO:        return "io";
    case ASX_ENCODING_ERR_OVERFLOW:  return "overflow";
    case ASX_ENCODING_ERR_CANCELLED: return "cancelled";
    default:                         return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

void asx_encoding_config_init(asx_encoding_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
}

/* ------------------------------------------------------------------ */
/* Pipeline lifecycle                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_encoding_pipeline_init(asx_encoding_pipeline *p, asx_codec codec,
                                       asx_write_adapter writer,
                                       const asx_encoding_config *cfg) {
    if (p == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(p, 0, sizeof(*p));
    p->codec = codec;
    p->writer = writer;

    if (cfg != NULL) {
        p->config = *cfg;
    } else {
        asx_encoding_config_init(&p->config);
    }

    asx_buf_mut_init(&p->staging);
    memset(&p->stats, 0, sizeof(p->stats));
    p->last_error = ASX_ENCODING_ERR_NONE;
    p->cancelled = 0;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Encode one frame                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_encoding_pipeline_encode(asx_encoding_pipeline *p, const void *data,
                                         uint32_t len, asx_encoded_symbol *out_symbol) {
    asx_status st;
    asx_buf frozen;
    uint32_t bytes_before;
    uint32_t frame_bytes;
    uint32_t written_total;
    uint32_t bytes_written;
    asx_write_result wr;

    if (p == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->cancelled) {
        p->last_error = ASX_ENCODING_ERR_CANCELLED;
        return ASX_E_CANCELLED;
    }
    if (len > 0 && data == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Check max_frame_bytes */
    if (p->config.max_frame_bytes > 0 && len > p->config.max_frame_bytes) {
        p->stats.frames_rejected++;
        p->last_error = ASX_ENCODING_ERR_OVERFLOW;
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    /* Clear staging for this frame */
    asx_buf_mut_clear(&p->staging);

    /* Encode through codec into staging buffer */
    bytes_before = p->staging.wr_pos;
    st = p->codec.encode(p->codec.state, data, len, &p->staging);
    if (st != ASX_OK) {
        p->stats.frames_rejected++;
        if (st == ASX_E_RESOURCE_EXHAUSTED) {
            p->last_error = ASX_ENCODING_ERR_OVERFLOW;
        } else {
            p->last_error = ASX_ENCODING_ERR_CODEC;
        }
        return st;
    }

    frame_bytes = p->staging.wr_pos - bytes_before;

    /* Check max_total_bytes */
    if (p->config.max_total_bytes > 0 &&
        p->stats.bytes_output + frame_bytes > p->config.max_total_bytes) {
        p->stats.frames_rejected++;
        p->last_error = ASX_ENCODING_ERR_OVERFLOW;
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    /* Write staging buffer contents to adapter */
    frozen = asx_buf_mut_freeze(&p->staging);
    written_total = 0;

    while (written_total < frozen.len) {
        asx_buf remaining_view;
        remaining_view.ptr = frozen.ptr + written_total;
        remaining_view.len = frozen.len - written_total;

        bytes_written = 0;
        wr = p->writer.poll_write(p->writer.state, &remaining_view, NULL, &bytes_written);

        if (wr == ASX_WRITE_READY) {
            written_total += bytes_written;
        } else if (wr == ASX_WRITE_PENDING) {
            /* In synchronous/deterministic mode, PENDING should not happen
             * with memory adapters. Treat as transient and retry. */
            continue;
        } else {
            /* CLOSED or ERROR */
            p->last_error = ASX_ENCODING_ERR_IO;
            return ASX_E_INVALID_STATE;
        }
    }

    /* Flush if configured */
    if (p->config.flush_after_each && p->writer.poll_flush != NULL) {
        asx_write_result fr = p->writer.poll_flush(p->writer.state, NULL);
        if (fr != ASX_WRITE_READY && fr != ASX_WRITE_PENDING) {
            p->last_error = ASX_ENCODING_ERR_IO;
            return ASX_E_INVALID_STATE;
        }
    }

    /* Update stats */
    p->stats.frames_encoded++;
    p->stats.bytes_input += len;
    p->stats.bytes_output += frame_bytes;

    /* Fill out_symbol if requested */
    if (out_symbol != NULL) {
        out_symbol->data = p->staging.data + bytes_before;
        out_symbol->len = frame_bytes;
        out_symbol->sequence = p->stats.frames_encoded;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Flush                                                               */
/* ------------------------------------------------------------------ */

asx_status asx_encoding_pipeline_flush(asx_encoding_pipeline *p) {
    asx_write_result fr;

    if (p == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->writer.poll_flush == NULL) return ASX_OK;

    fr = p->writer.poll_flush(p->writer.state, NULL);
    if (fr == ASX_WRITE_READY || fr == ASX_WRITE_PENDING) return ASX_OK;

    p->last_error = ASX_ENCODING_ERR_IO;
    return ASX_E_INVALID_STATE;
}

/* ------------------------------------------------------------------ */
/* Cancel                                                              */
/* ------------------------------------------------------------------ */

void asx_encoding_pipeline_cancel(asx_encoding_pipeline *p) {
    if (p == NULL) return;
    p->cancelled = 1;
}

/* ------------------------------------------------------------------ */
/* Stats / error query                                                 */
/* ------------------------------------------------------------------ */

void asx_encoding_pipeline_stats(const asx_encoding_pipeline *p, asx_encoding_stats *out) {
    if (p == NULL || out == NULL) return;
    *out = p->stats;
}

asx_encoding_error_kind asx_encoding_pipeline_last_error(const asx_encoding_pipeline *p) {
    if (p == NULL) return ASX_ENCODING_ERR_NONE;
    return p->last_error;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_encoding_pipeline_reset(asx_encoding_pipeline *p) {
    if (p == NULL) return;
    asx_buf_mut_clear(&p->staging);
    memset(&p->stats, 0, sizeof(p->stats));
    p->last_error = ASX_ENCODING_ERR_NONE;
    p->cancelled = 0;
}

#endif /* !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO */
