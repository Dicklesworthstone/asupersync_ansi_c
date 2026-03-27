/*
 * asx/net/pipe.h — anonymous pipe pairs
 *
 * Provides in-memory anonymous pipe pairs for inter-task communication.
 * Each pipe has a read end and a write end with bounded buffering.
 * In deterministic core, pipes are fully in-memory.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_NET_PIPE_H
#define ASX_NET_PIPE_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/bytes/buf.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_MAX_PIPES
#define ASX_MAX_PIPES 8u
#endif

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_pipe_read;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_pipe_write;

/* Create an anonymous pipe pair. */
ASX_API ASX_MUST_USE asx_status asx_pipe_open(asx_pipe_read *rd, asx_pipe_write *wr);

/* Poll-read from pipe. Returns ASX_E_PENDING if empty, ASX_E_DISCONNECTED if writer closed. */
ASX_API ASX_MUST_USE asx_status asx_pipe_poll_read(asx_pipe_read rd, asx_buf_mut *dst,
                                                     uint32_t *bytes_read);

/* Poll-write to pipe. Returns ASX_E_DISCONNECTED if reader closed. */
ASX_API ASX_MUST_USE asx_status asx_pipe_poll_write(asx_pipe_write wr, const asx_buf *src,
                                                      uint32_t *bytes_written);

/* Close the read end. */
ASX_API asx_status asx_pipe_close_read(asx_pipe_read rd);

/* Close the write end. */
ASX_API asx_status asx_pipe_close_write(asx_pipe_write wr);

/* Check if read/write end is alive. */
ASX_API int asx_pipe_read_is_alive(asx_pipe_read rd);
/* Check if the write end of a pipe is still alive. */
ASX_API int asx_pipe_write_is_alive(asx_pipe_write wr);

/* Reset all pipe state (test support). */
ASX_API void asx_pipe_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_NET_PIPE_H */
