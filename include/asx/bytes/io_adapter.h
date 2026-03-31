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

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <asx/runtime/waker.h>

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO

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

/* Initialize a memory read adapter from a buffer. */
ASX_API void asx_mem_read_init(asx_mem_read_state *s, asx_buf_mut *source);

/* Get a read adapter for the memory reader. */
ASX_API asx_read_adapter asx_mem_read_adapter(asx_mem_read_state *s);

/* A memory write adapter backed by an asx_buf_mut.
 * Writes append data to the buffer. Returns CLOSED when full. */
typedef struct {
    asx_buf_mut *sink;
    int closed;
} asx_mem_write_state;

/* Initialize a memory write adapter to a buffer. */
ASX_API void asx_mem_write_init(asx_mem_write_state *s, asx_buf_mut *sink);

/* Get a write adapter for the memory writer. */
ASX_API asx_write_adapter asx_mem_write_adapter(asx_mem_write_state *s);

/* -------------------------------------------------------------------
 * Buffered reader — wraps a read adapter with an internal buffer
 * ------------------------------------------------------------------- */

typedef struct {
    asx_read_adapter inner;
    asx_buf_mut buffer;
    int eof;
} asx_buf_reader;

/* Initialize a buffered reader around a read adapter. */
ASX_API void asx_buf_reader_init(asx_buf_reader *br, asx_read_adapter inner);

/* Read up to dst capacity bytes, buffering internally. */
ASX_API ASX_MUST_USE asx_read_result asx_buf_reader_read(asx_buf_reader *br, asx_buf_mut *dst,
                                                         uint32_t *bytes_read);

/* Read exactly one line (up to newline or EOF). Returns line in dst.
 * Line delimiter is consumed but not included in output. */
ASX_API ASX_MUST_USE asx_status asx_buf_reader_read_line(asx_buf_reader *br, asx_buf_mut *dst,
                                                         uint32_t *bytes_read);

/* Check if EOF has been reached. */
ASX_API int asx_buf_reader_is_eof(const asx_buf_reader *br);

/* -------------------------------------------------------------------
 * Buffered writer — wraps a write adapter with an internal buffer
 * ------------------------------------------------------------------- */

typedef struct {
    asx_write_adapter inner;
    asx_buf_mut buffer;
    int closed;
} asx_buf_writer;

/* Initialize a buffered writer around a write adapter. */
ASX_API void asx_buf_writer_init(asx_buf_writer *bw, asx_write_adapter inner);

/* Write data through the buffer. Flushes when buffer is full. */
ASX_API ASX_MUST_USE asx_status asx_buf_writer_write(asx_buf_writer *bw, const asx_buf *src,
                                                     uint32_t *bytes_written);

/* Flush all buffered data to the underlying writer. */
ASX_API ASX_MUST_USE asx_status asx_buf_writer_flush(asx_buf_writer *bw);

/* Shutdown the writer (flush + close). */
ASX_API asx_status asx_buf_writer_shutdown(asx_buf_writer *bw);

/* -------------------------------------------------------------------
 * Copy utility — copy all data from reader to writer
 * ------------------------------------------------------------------- */

/* Copy all data from src reader to dst writer until EOF.
 * Returns total bytes copied in *bytes_copied. */
ASX_API ASX_MUST_USE asx_status asx_io_copy(asx_read_adapter *src, asx_write_adapter *dst,
                                            uint64_t *bytes_copied);

/* Copy at most max_bytes from src to dst.
 * Returns actual bytes copied in *bytes_copied. */
ASX_API ASX_MUST_USE asx_status asx_io_copy_n(asx_read_adapter *src, asx_write_adapter *dst,
                                              uint64_t max_bytes, uint64_t *bytes_copied);

/* -------------------------------------------------------------------
 * Read extensions — convenience wrappers
 * ------------------------------------------------------------------- */

/* Read exactly n bytes into dst. Returns ASX_OK or error.
 * Partial reads are retried until exactly n bytes are read or EOF/error. */
ASX_API ASX_MUST_USE asx_status asx_io_read_exact(asx_read_adapter *src, asx_buf_mut *dst, size_t n,
                                                  size_t *bytes_read);

/* Read all remaining data into dst until EOF.
 * Returns total bytes read in *bytes_read. */
ASX_API ASX_MUST_USE asx_status asx_io_read_to_end(asx_read_adapter *src, asx_buf_mut *dst,
                                                   size_t *bytes_read);

/* -------------------------------------------------------------------
 * Write extensions — convenience wrappers
 * ------------------------------------------------------------------- */

/* Write all bytes from src to dst. Retries partial writes.
 * Returns ASX_OK when all bytes are written. */
ASX_API ASX_MUST_USE asx_status asx_io_write_all(asx_write_adapter *dst, const asx_buf *src);

/* -------------------------------------------------------------------
 * Chain reader — concatenate two readers
 * ------------------------------------------------------------------- */

typedef struct {
    asx_read_adapter first;
    asx_read_adapter second;
    int first_done;
} asx_chain_reader_state;

/* Initialize a chain reader that reads from first, then second. */
ASX_API void asx_chain_reader_init(asx_chain_reader_state *state, asx_read_adapter first,
                                   asx_read_adapter second);

/* Get a read adapter for the chain reader. */
ASX_API asx_read_adapter asx_chain_reader_adapter(asx_chain_reader_state *state);

/* -------------------------------------------------------------------
 * Take reader — limit bytes from underlying reader
 * ------------------------------------------------------------------- */

typedef struct {
    asx_read_adapter inner;
    size_t remaining;
} asx_take_reader_state;

/* Initialize a take reader that reads at most limit bytes from inner. */
ASX_API void asx_take_reader_init(asx_take_reader_state *state, asx_read_adapter inner,
                                  size_t limit);

/* Get a read adapter for the take reader. */
ASX_API asx_read_adapter asx_take_reader_adapter(asx_take_reader_state *state);

#endif /* !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_IO */

#ifdef __cplusplus
}
#endif

#endif /* ASX_BYTES_IO_ADAPTER_H */
