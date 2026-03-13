/*
 * asx/bytes/io_adapter.h — read/write adapter interfaces
 *
 * Provides abstract read/write interfaces that bridge the async
 * runtime with buffer-oriented IO. Walking skeleton: synchronous
 * operation with fixed buffers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_BYTES_IO_ADAPTER_H
#define ASX_BYTES_IO_ADAPTER_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/runtime/waker.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Async read result
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_READ_READY = 0,   /* data was read successfully */
    ASX_READ_PENDING = 1, /* would block, waker will be signaled */
    ASX_READ_EOF = 2,     /* end of stream */
    ASX_READ_ERROR = 3    /* error occurred */
} asx_read_result;

/* -------------------------------------------------------------------
 * Async write result
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_WRITE_READY = 0,   /* data was written successfully */
    ASX_WRITE_PENDING = 1, /* would block, waker will be signaled */
    ASX_WRITE_CLOSED = 2,  /* write side closed */
    ASX_WRITE_ERROR = 3    /* error occurred */
} asx_write_result;

/* -------------------------------------------------------------------
 * Read adapter (trait via function pointers)
 * ------------------------------------------------------------------- */

/* poll_read: attempt to read data into dst buffer.
 * On ASX_READ_READY, *out_bytes_read contains the number of bytes read.
 * On ASX_READ_PENDING, the waker will be signaled when data arrives.
 * On ASX_READ_EOF, the stream is finished. */
typedef asx_read_result (*asx_poll_read_fn)(void *adapter_state, asx_buf_mut *dst,
                                            const asx_waker *waker, uint32_t *out_bytes_read);

typedef struct {
    asx_poll_read_fn poll_read;
    void *state;
} asx_read_adapter;

/* -------------------------------------------------------------------
 * Write adapter (trait via function pointers)
 * ------------------------------------------------------------------- */

/* poll_write: attempt to write data from src buffer.
 * On ASX_WRITE_READY, *out_bytes_written contains the bytes written.
 * On ASX_WRITE_PENDING, the waker will be signaled when space is available.
 * On ASX_WRITE_CLOSED, the write side is shut down. */
typedef asx_write_result (*asx_poll_write_fn)(void *adapter_state, const asx_buf *src,
                                              const asx_waker *waker, uint32_t *out_bytes_written);

/* poll_flush: ensure all buffered data is transmitted.
 * Returns ASX_WRITE_READY when flush completes. */
typedef asx_write_result (*asx_poll_flush_fn)(void *adapter_state, const asx_waker *waker);

/* poll_shutdown: gracefully shut down the write side. */
typedef asx_write_result (*asx_poll_shutdown_fn)(void *adapter_state, const asx_waker *waker);

typedef struct {
    asx_poll_write_fn poll_write;
    asx_poll_flush_fn poll_flush;
    asx_poll_shutdown_fn poll_shutdown;
    void *state;
} asx_write_adapter;

/* -------------------------------------------------------------------
 * Memory-backed adapters (for testing and in-process use)
 * ------------------------------------------------------------------- */

/* A memory read adapter backed by an asx_buf_mut.
 * Reads drain data from the buffer. Returns EOF when empty. */
typedef struct {
    asx_buf_mut *source;
    int eof;
} asx_mem_read_state;

ASX_API void asx_mem_read_init(asx_mem_read_state *s, asx_buf_mut *source);
ASX_API asx_read_adapter asx_mem_read_adapter(asx_mem_read_state *s);

/* A memory write adapter backed by an asx_buf_mut.
 * Writes append data to the buffer. Returns CLOSED when full. */
typedef struct {
    asx_buf_mut *sink;
    int closed;
} asx_mem_write_state;

ASX_API void asx_mem_write_init(asx_mem_write_state *s, asx_buf_mut *sink);
ASX_API asx_write_adapter asx_mem_write_adapter(asx_mem_write_state *s);

#ifdef __cplusplus
}
#endif

#endif /* ASX_BYTES_IO_ADAPTER_H */
