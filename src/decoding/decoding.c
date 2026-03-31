/*
 * decoding.c — decoding pipeline implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/decoding/decoding.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

/* ------------------------------------------------------------------ */
/* Error kind strings                                                  */
/* ------------------------------------------------------------------ */

const char *asx_decoding_error_kind_str(asx_decoding_error_kind kind) {
    switch (kind) {
    case ASX_DECODING_ERR_NONE: return "none";
    case ASX_DECODING_ERR_CODEC: return "codec";
    case ASX_DECODING_ERR_IO: return "io";
    case ASX_DECODING_ERR_OVERFLOW: return "overflow";
    case ASX_DECODING_ERR_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

void asx_decoding_config_init(asx_decoding_config *cfg) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
}

/* ------------------------------------------------------------------ */
/* Pipeline lifecycle                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_decoding_pipeline_init(asx_decoding_pipeline *p, asx_codec codec,
                                      asx_read_adapter reader, const asx_decoding_config *cfg) {
    if (p == NULL) return ASX_E_INVALID_ARGUMENT;

    memset(p, 0, sizeof(*p));
    p->codec = codec;
    p->reader = reader;

    if (cfg != NULL) {
        p->config = *cfg;
    } else {
        asx_decoding_config_init(&p->config);
    }

    asx_buf_mut_init(&p->recv_buf);
    memset(&p->progress, 0, sizeof(p->progress));
    p->last_error = ASX_DECODING_ERR_NONE;
    p->cancelled = 0;

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Internal: try to read more data from adapter into recv_buf          */
/* ------------------------------------------------------------------ */

static asx_status decode_fill(asx_decoding_pipeline *p) {
    uint32_t bytes_read = 0;
    asx_read_result rr;

    /* Compact the buffer to make room if possible */
    asx_buf_mut_compact(&p->recv_buf);

    if (asx_buf_mut_writable(&p->recv_buf) == 0) {
        p->last_error = ASX_DECODING_ERR_OVERFLOW;
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    rr = p->reader.poll_read(p->reader.state, &p->recv_buf, NULL, &bytes_read);

    switch (rr) {
    case ASX_READ_READY:
        p->progress.bytes_consumed += bytes_read;
        p->progress.bytes_buffered = asx_buf_mut_remaining(&p->recv_buf);
        return ASX_OK;

    case ASX_READ_EOF:
        p->progress.eof_reached = 1;
        if (asx_buf_mut_remaining(&p->recv_buf) == 0) { p->progress.complete = 1; }
        return ASX_E_NOT_FOUND;

    case ASX_READ_PENDING: return ASX_E_PENDING;

    case ASX_READ_ERROR:
    default: p->last_error = ASX_DECODING_ERR_IO; return ASX_E_INVALID_STATE;
    }
}

/* ------------------------------------------------------------------ */
/* Decode one frame                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_decoding_pipeline_decode(asx_decoding_pipeline *p, asx_frame *out_frame) {
    asx_decode_result dr;
    asx_status fill_st;

    if (p == NULL || out_frame == NULL) return ASX_E_INVALID_ARGUMENT;
    if (p->cancelled) {
        p->last_error = ASX_DECODING_ERR_CANCELLED;
        return ASX_E_CANCELLED;
    }

    /* Check frame limit */
    if (p->config.max_frames > 0 && p->progress.frames_decoded >= p->config.max_frames) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    /* Check byte limit */
    if (p->config.max_total_bytes > 0 && p->progress.bytes_consumed >= p->config.max_total_bytes) {
        return ASX_E_RESOURCE_EXHAUSTED;
    }

    /* Try to decode from current buffer contents first */
    for (;;) {
        if (asx_buf_mut_remaining(&p->recv_buf) > 0) {
            uint32_t rd_pos_before = p->recv_buf.rd_pos;
            dr = p->codec.decode(p->codec.state, &p->recv_buf, out_frame);

            if (dr == ASX_DECODE_FRAME) {
                /* Check max_frame_bytes */
                if (p->config.max_frame_bytes > 0 && out_frame->len > p->config.max_frame_bytes) {
                    p->last_error = ASX_DECODING_ERR_OVERFLOW;
                    return ASX_E_RESOURCE_EXHAUSTED;
                }

                p->progress.frames_decoded++;
                p->progress.bytes_buffered = asx_buf_mut_remaining(&p->recv_buf);
                return ASX_OK;
            }

            if (dr == ASX_DECODE_ERROR) {
                /* Restore buffer position so the error doesn't leave
                 * the pipeline in an inconsistent state. */
                p->recv_buf.rd_pos = rd_pos_before;
                p->last_error = ASX_DECODING_ERR_CODEC;
                return ASX_E_INVALID_STATE;
            }

            /* ASX_DECODE_NEED_MORE — fall through to read more */
        }

        /* If EOF was already reached and we still need more data, we're done */
        if (p->progress.eof_reached) {
            p->progress.bytes_buffered = asx_buf_mut_remaining(&p->recv_buf);
            if (p->progress.bytes_buffered == 0) { p->progress.complete = 1; }
            return ASX_E_NOT_FOUND;
        }

        /* Try to fill the buffer */
        fill_st = decode_fill(p);
        if (fill_st == ASX_E_NOT_FOUND) {
            /* EOF — loop back to try decoding whatever remains */
            continue;
        }
        if (fill_st != ASX_OK) { return fill_st; }
        /* Got more data, loop back to try decoding */
    }
}

/* ------------------------------------------------------------------ */
/* Cancel                                                              */
/* ------------------------------------------------------------------ */

void asx_decoding_pipeline_cancel(asx_decoding_pipeline *p) {
    if (p == NULL) return;
    p->cancelled = 1;
}

/* ------------------------------------------------------------------ */
/* Progress / error query                                              */
/* ------------------------------------------------------------------ */

void asx_decoding_pipeline_progress(const asx_decoding_pipeline *p, asx_decoding_progress *out) {
    if (p == NULL || out == NULL) return;
    *out = p->progress;
}

asx_decoding_error_kind asx_decoding_pipeline_last_error(const asx_decoding_pipeline *p) {
    if (p == NULL) return ASX_DECODING_ERR_NONE;
    return p->last_error;
}

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void asx_decoding_pipeline_reset(asx_decoding_pipeline *p) {
    if (p == NULL) return;
    asx_buf_mut_clear(&p->recv_buf);
    memset(&p->progress, 0, sizeof(p->progress));
    p->last_error = ASX_DECODING_ERR_NONE;
    p->cancelled = 0;
}

#endif /* !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO */
