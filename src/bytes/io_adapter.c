/*
 * io_adapter.c — read/write adapter implementations
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/bytes/io_adapter.h>
#include <string.h>

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
