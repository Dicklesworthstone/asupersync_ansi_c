/*
 * io_adapter.c — read/write adapter implementations
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/bytes/io_adapter.h>
#include <string.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

/* ------------------------------------------------------------------ */
/* Memory read adapter                                                 */
/* ------------------------------------------------------------------ */

static asx_read_result mem_poll_read(void *adapter_state, asx_buf_mut *dst, const asx_waker *waker,
                                     uint32_t *out_bytes_read) {
    asx_mem_read_state *s = (asx_mem_read_state *)adapter_state;
    uint32_t avail;
    uint32_t space;
    uint32_t to_copy;
    asx_status st;

    (void)waker;

    if (s == NULL || dst == NULL || out_bytes_read == NULL) return ASX_READ_ERROR;

    if (s->eof) return ASX_READ_EOF;
    if (s->source == NULL) return ASX_READ_EOF;

    avail = asx_buf_mut_remaining(s->source);
    if (avail == 0) {
        s->eof = 1;
        return ASX_READ_EOF;
    }

    space = asx_buf_mut_writable(dst);
    to_copy = (avail < space) ? avail : space;
    if (to_copy == 0) {
        *out_bytes_read = 0;
        return ASX_READ_READY;
    }

    st = asx_buf_mut_put(dst, s->source->data + s->source->rd_pos, to_copy);
    if (st != ASX_OK) return ASX_READ_ERROR;

    st = asx_buf_mut_advance(s->source, to_copy);
    if (st != ASX_OK) return ASX_READ_ERROR;

    *out_bytes_read = to_copy;
    return ASX_READ_READY;
}

void asx_mem_read_init(asx_mem_read_state *s, asx_buf_mut *source) {
    if (s == NULL) return;
    s->source = source;
    s->eof = 0;
}

asx_read_adapter asx_mem_read_adapter(asx_mem_read_state *s) {
    asx_read_adapter a;
    a.poll_read = mem_poll_read;
    a.state = s;
    return a;
}

/* ------------------------------------------------------------------ */
/* Memory write adapter                                                */
/* ------------------------------------------------------------------ */

static asx_write_result mem_poll_write(void *adapter_state, const asx_buf *src,
                                       const asx_waker *waker, uint32_t *out_bytes_written) {
    asx_mem_write_state *s = (asx_mem_write_state *)adapter_state;
    uint32_t space;
    uint32_t to_copy;
    asx_status st;

    (void)waker;

    if (s == NULL || src == NULL || out_bytes_written == NULL) return ASX_WRITE_ERROR;

    if (s->closed) return ASX_WRITE_CLOSED;
    if (s->sink == NULL) return ASX_WRITE_CLOSED;

    space = asx_buf_mut_writable(s->sink);
    to_copy = (src->len < space) ? src->len : space;
    if (to_copy == 0) {
        *out_bytes_written = 0;
        return ASX_WRITE_READY;
    }

    st = asx_buf_mut_put(s->sink, src->ptr, to_copy);
    if (st != ASX_OK) return ASX_WRITE_ERROR;

    *out_bytes_written = to_copy;
    return ASX_WRITE_READY;
}

static asx_write_result mem_poll_flush(void *adapter_state, const asx_waker *waker) {
    (void)adapter_state;
    (void)waker;
    return ASX_WRITE_READY; /* memory adapter is always flushed */
}

static asx_write_result mem_poll_shutdown(void *adapter_state, const asx_waker *waker) {
    asx_mem_write_state *s = (asx_mem_write_state *)adapter_state;
    (void)waker;
    if (s != NULL) s->closed = 1;
    return ASX_WRITE_READY;
}

void asx_mem_write_init(asx_mem_write_state *s, asx_buf_mut *sink) {
    if (s == NULL) return;
    s->sink = sink;
    s->closed = 0;
}

asx_write_adapter asx_mem_write_adapter(asx_mem_write_state *s) {
    asx_write_adapter a;
    a.poll_write = mem_poll_write;
    a.poll_flush = mem_poll_flush;
    a.poll_shutdown = mem_poll_shutdown;
    a.state = s;
    return a;
}

/* ------------------------------------------------------------------ */
/* Buffered reader                                                     */
/* ------------------------------------------------------------------ */

void asx_buf_reader_init(asx_buf_reader *br, asx_read_adapter inner) {
    if (br == NULL) return;
    br->inner = inner;
    asx_buf_mut_init(&br->buffer);
    br->eof = 0;
}

asx_read_result asx_buf_reader_read(asx_buf_reader *br, asx_buf_mut *dst, uint32_t *bytes_read) {
    asx_read_result rr;
    asx_buf readable;
    uint32_t to_copy;
    asx_status st;

    if (br == NULL || dst == NULL || bytes_read == NULL) return ASX_READ_ERROR;
    *bytes_read = 0u;

    /* First try to serve from buffer */
    if (asx_buf_mut_remaining(&br->buffer) > 0u) {
        readable = asx_buf_mut_readable(&br->buffer);
        to_copy = readable.len;
        if (to_copy > asx_buf_mut_writable(dst)) to_copy = asx_buf_mut_writable(dst);
        st = asx_buf_mut_put(dst, readable.ptr, to_copy);
        if (st != ASX_OK) return ASX_READ_ERROR;
        st = asx_buf_mut_advance(&br->buffer, to_copy);
        if (st != ASX_OK) return ASX_READ_ERROR;
        if (asx_buf_mut_remaining(&br->buffer) == 0u) asx_buf_mut_clear(&br->buffer);
        *bytes_read = to_copy;
        return ASX_READ_READY;
    }

    if (br->eof) return ASX_READ_EOF;

    /* Buffer empty — refill from inner */
    asx_buf_mut_clear(&br->buffer);
    rr = br->inner.poll_read(br->inner.state, &br->buffer, NULL, bytes_read);

    if (rr == ASX_READ_EOF) {
        br->eof = 1;
        if (*bytes_read == 0u) return ASX_READ_EOF;
    }
    if (rr == ASX_READ_ERROR) return ASX_READ_ERROR;

    /* Copy from buffer to dst */
    if (asx_buf_mut_remaining(&br->buffer) > 0u) {
        readable = asx_buf_mut_readable(&br->buffer);
        to_copy = readable.len;
        if (to_copy > asx_buf_mut_writable(dst)) to_copy = asx_buf_mut_writable(dst);
        st = asx_buf_mut_put(dst, readable.ptr, to_copy);
        if (st != ASX_OK) return ASX_READ_ERROR;
        st = asx_buf_mut_advance(&br->buffer, to_copy);
        if (st != ASX_OK) return ASX_READ_ERROR;
        if (asx_buf_mut_remaining(&br->buffer) == 0u) asx_buf_mut_clear(&br->buffer);
        *bytes_read = to_copy;
    }
    return ASX_READ_READY;
}

asx_status asx_buf_reader_read_line(asx_buf_reader *br, asx_buf_mut *dst, uint32_t *bytes_read) {
    uint32_t chunk_read;
    asx_read_result rr;
    asx_buf readable;
    uint32_t i;

    if (br == NULL || dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    *bytes_read = 0u;

    for (;;) {
        /* Scan buffer for newline */
        readable = asx_buf_mut_readable(&br->buffer);
        for (i = 0u; i < readable.len; i++) {
            if (readable.ptr[i] == '\n') {
                /* Found newline — copy up to (not including) it */
                uint32_t line_len = i;
                if (line_len > 0u && readable.ptr[line_len - 1u] == '\r') line_len--;
                if (line_len > 0u) {
                    asx_status st = asx_buf_mut_put(dst, readable.ptr, line_len);
                    if (st != ASX_OK) return st;
                }
                /* Advance past the newline */
                { asx_status adv_ = asx_buf_mut_advance(&br->buffer, i + 1u); (void)adv_; }
                if (asx_buf_mut_remaining(&br->buffer) == 0u) asx_buf_mut_clear(&br->buffer);
                *bytes_read = line_len;
                return ASX_OK;
            }
        }

        /* No newline found — fill more from inner */
        if (br->eof) {
            /* EOF and no newline — return whatever's left as last line */
            if (readable.len > 0u) {
                asx_status st = asx_buf_mut_put(dst, readable.ptr, readable.len);
                if (st != ASX_OK) return st;
                *bytes_read = readable.len;
                asx_buf_mut_clear(&br->buffer);
                return ASX_OK;
            }
            return ASX_E_NOT_FOUND; /* EOF with no more data */
        }

        asx_buf_mut_compact(&br->buffer);
        chunk_read = 0u;
        rr = br->inner.poll_read(br->inner.state, &br->buffer, NULL, &chunk_read);
        if (rr == ASX_READ_EOF) {
            br->eof = 1;
        } else if (rr == ASX_READ_ERROR) {
            return ASX_E_INVALID_STATE;
        } else if (rr == ASX_READ_PENDING) {
            return ASX_E_PENDING;
        }
    }
}

int asx_buf_reader_is_eof(const asx_buf_reader *br) {
    if (br == NULL) return 1;
    return br->eof && asx_buf_mut_remaining(&br->buffer) == 0u;
}

/* ------------------------------------------------------------------ */
/* Buffered writer                                                     */
/* ------------------------------------------------------------------ */

void asx_buf_writer_init(asx_buf_writer *bw, asx_write_adapter inner) {
    if (bw == NULL) return;
    bw->inner = inner;
    asx_buf_mut_init(&bw->buffer);
    bw->closed = 0;
}

asx_status asx_buf_writer_write(asx_buf_writer *bw, const asx_buf *src, uint32_t *bytes_written) {
    asx_status st;

    if (bw == NULL || src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    if (bw->closed) return ASX_E_INVALID_STATE;
    *bytes_written = 0u;

    /* If buffer has room, buffer the data */
    if (src->len <= asx_buf_mut_writable(&bw->buffer)) {
        st = asx_buf_mut_put(&bw->buffer, src->ptr, src->len);
        if (st != ASX_OK) return st;
        *bytes_written = src->len;
        return ASX_OK;
    }

    /* Buffer full — flush first */
    st = asx_buf_writer_flush(bw);
    if (st != ASX_OK) return st;

    /* Try again after flush */
    if (src->len <= asx_buf_mut_writable(&bw->buffer)) {
        st = asx_buf_mut_put(&bw->buffer, src->ptr, src->len);
        if (st != ASX_OK) return st;
        *bytes_written = src->len;
        return ASX_OK;
    }

    /* Data larger than buffer — write directly */
    {
        asx_write_result wr;
        uint32_t written = 0u;
        wr = bw->inner.poll_write(bw->inner.state, src, NULL, &written);
        if (wr != ASX_WRITE_READY) return ASX_E_INVALID_STATE;
        *bytes_written = written;
    }
    return ASX_OK;
}

asx_status asx_buf_writer_flush(asx_buf_writer *bw) {
    asx_buf readable;
    asx_write_result wr;
    uint32_t written;

    if (bw == NULL) return ASX_E_INVALID_ARGUMENT;
    if (asx_buf_mut_remaining(&bw->buffer) == 0u) return ASX_OK;

    readable = asx_buf_mut_readable(&bw->buffer);
    written = 0u;
    wr = bw->inner.poll_write(bw->inner.state, &readable, NULL, &written);
    if (wr != ASX_WRITE_READY) return ASX_E_INVALID_STATE;

    { asx_status adv_ = asx_buf_mut_advance(&bw->buffer, written); (void)adv_; }
    if (asx_buf_mut_remaining(&bw->buffer) == 0u) asx_buf_mut_clear(&bw->buffer);

    if (bw->inner.poll_flush != NULL) {
        wr = bw->inner.poll_flush(bw->inner.state, NULL);
        if (wr != ASX_WRITE_READY && wr != ASX_WRITE_PENDING) return ASX_E_INVALID_STATE;
    }
    return ASX_OK;
}

asx_status asx_buf_writer_shutdown(asx_buf_writer *bw) {
    asx_status st;
    asx_write_result wr;

    if (bw == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_buf_writer_flush(bw);
    if (st != ASX_OK) return st;

    if (bw->inner.poll_shutdown != NULL) {
        wr = bw->inner.poll_shutdown(bw->inner.state, NULL);
        if (wr != ASX_WRITE_READY) return ASX_E_INVALID_STATE;
    }
    bw->closed = 1;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Copy utilities                                                      */
/* ------------------------------------------------------------------ */

asx_status asx_io_copy(asx_read_adapter *src, asx_write_adapter *dst, uint64_t *bytes_copied) {
    return asx_io_copy_n(src, dst, UINT64_MAX, bytes_copied);
}

asx_status asx_io_copy_n(asx_read_adapter *src, asx_write_adapter *dst, uint64_t max_bytes,
                           uint64_t *bytes_copied) {
    asx_buf_mut chunk;
    uint64_t total;
    uint32_t read_n;
    asx_read_result rr;

    if (src == NULL || dst == NULL || bytes_copied == NULL) return ASX_E_INVALID_ARGUMENT;
    total = 0u;

    for (;;) {
        asx_buf frozen;
        uint32_t written;
        asx_write_result wr;

        if (total >= max_bytes) break;

        asx_buf_mut_init(&chunk);
        read_n = 0u;
        rr = src->poll_read(src->state, &chunk, NULL, &read_n);

        if (rr == ASX_READ_EOF && read_n == 0u) break;
        if (rr == ASX_READ_ERROR) {
            *bytes_copied = total;
            return ASX_E_INVALID_STATE;
        }

        frozen = asx_buf_mut_readable(&chunk);
        if (frozen.len > 0u) {
            written = 0u;
            wr = dst->poll_write(dst->state, &frozen, NULL, &written);
            if (wr != ASX_WRITE_READY) {
                *bytes_copied = total;
                return ASX_E_INVALID_STATE;
            }
            total += written;
        }

        if (rr == ASX_READ_EOF) break;
    }

    *bytes_copied = total;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Read extensions                                                     */
/* ------------------------------------------------------------------ */

asx_status asx_io_read_exact(asx_read_adapter *src, asx_buf_mut *dst, size_t n,
                              size_t *bytes_read) {
    size_t total = 0;

    if (src == NULL || dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;

    while (total < n) {
        uint32_t chunk = 0;
        asx_read_result rr = src->poll_read(src->state, dst, NULL, &chunk);
        total += chunk;
        if (rr == ASX_READ_EOF) {
            *bytes_read = total;
            return (total == n) ? ASX_OK : ASX_E_BUFFER_TOO_SMALL;
        }
        if (rr == ASX_READ_PENDING) {
            *bytes_read = total;
            return ASX_E_WOULD_BLOCK;
        }
        if (rr == ASX_READ_ERROR || (rr == ASX_READ_READY && chunk == 0)) {
            *bytes_read = total;
            return ASX_E_INVALID_STATE;
        }
    }

    *bytes_read = total;
    return ASX_OK;
}

asx_status asx_io_read_to_end(asx_read_adapter *src, asx_buf_mut *dst, size_t *bytes_read) {
    size_t total = 0;

    if (src == NULL || dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;

    for (;;) {
        uint32_t chunk = 0;
        asx_read_result rr = src->poll_read(src->state, dst, NULL, &chunk);
        total += chunk;
        if (rr == ASX_READ_EOF) break;
        if (rr == ASX_READ_PENDING) {
            *bytes_read = total;
            return ASX_E_WOULD_BLOCK;
        }
        if (rr == ASX_READ_ERROR || (rr == ASX_READ_READY && chunk == 0)) {
            *bytes_read = total;
            return ASX_E_INVALID_STATE;
        }
    }

    *bytes_read = total;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Write extensions                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_io_write_all(asx_write_adapter *dst, const asx_buf *src) {
    asx_buf remaining;
    if (dst == NULL || src == NULL) return ASX_E_INVALID_ARGUMENT;

    remaining = *src;

    while (remaining.len > 0) {
        uint32_t written = 0;
        asx_write_result wr = dst->poll_write(dst->state, &remaining, NULL, &written);
        if (wr == ASX_WRITE_CLOSED) return ASX_E_DISCONNECTED;
        if (wr == ASX_WRITE_PENDING) return ASX_E_WOULD_BLOCK;
        if (wr == ASX_WRITE_ERROR) return ASX_E_INVALID_STATE;
        if (written == 0) return ASX_E_INVALID_STATE; /* no progress — prevent infinite loop */
        remaining.ptr = remaining.ptr + written;
        remaining.len -= written;
    }

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Chain reader                                                        */
/* ------------------------------------------------------------------ */

static asx_read_result chain_poll_read(void *adapter_state, asx_buf_mut *dst,
                                        const asx_waker *waker, uint32_t *out_bytes_read) {
    asx_chain_reader_state *s = (asx_chain_reader_state *)adapter_state;

    if (!s->first_done) {
        asx_read_result rr = s->first.poll_read(s->first.state, dst, waker, out_bytes_read);
        if (rr != ASX_READ_EOF) return rr;
        s->first_done = 1;
        /* If the final read from first produced bytes, return them before
         * switching to second. The caller will poll again to get second's data. */
        if (*out_bytes_read > 0) return ASX_READ_READY;
    }

    return s->second.poll_read(s->second.state, dst, waker, out_bytes_read);
}

void asx_chain_reader_init(asx_chain_reader_state *state, asx_read_adapter first,
                            asx_read_adapter second) {
    state->first = first;
    state->second = second;
    state->first_done = 0;
}

asx_read_adapter asx_chain_reader_adapter(asx_chain_reader_state *state) {
    asx_read_adapter a;
    a.poll_read = chain_poll_read;
    a.state = state;
    return a;
}

/* ------------------------------------------------------------------ */
/* Take reader                                                         */
/* ------------------------------------------------------------------ */

static asx_read_result take_poll_read(void *adapter_state, asx_buf_mut *dst,
                                       const asx_waker *waker, uint32_t *out_bytes_read) {
    asx_take_reader_state *s = (asx_take_reader_state *)adapter_state;
    asx_read_result rr;
    uint32_t read_count;
    uint32_t saved_capacity;

    if (s->remaining == 0) {
        *out_bytes_read = 0;
        return ASX_READ_EOF;
    }

    /* Temporarily limit dst writable space to remaining bytes */
    saved_capacity = dst->capacity;
    {
        uint32_t writable = dst->capacity - dst->wr_pos;
        if (writable > (uint32_t)s->remaining) {
            dst->capacity = dst->wr_pos + (uint32_t)s->remaining;
        }
    }

    rr = s->inner.poll_read(s->inner.state, dst, waker, &read_count);
    dst->capacity = saved_capacity; /* restore */
    *out_bytes_read = read_count;

    if (rr == ASX_READ_READY || rr == ASX_READ_EOF) {
        if (read_count <= s->remaining) {
            s->remaining -= read_count;
        } else {
            s->remaining = 0;
        }
    }

    if (s->remaining == 0 && rr == ASX_READ_READY) {
        return ASX_READ_EOF; /* limit reached */
    }

    return rr;
}

void asx_take_reader_init(asx_take_reader_state *state, asx_read_adapter inner, size_t limit) {
    state->inner = inner;
    state->remaining = limit;
}

asx_read_adapter asx_take_reader_adapter(asx_take_reader_state *state) {
    asx_read_adapter a;
    a.poll_read = take_poll_read;
    a.state = state;
    return a;
}

#endif /* !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO */
