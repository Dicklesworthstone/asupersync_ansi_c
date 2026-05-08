/*
 * pipe.c — anonymous in-memory pipe pairs
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/net/pipe.h>
#include <string.h>

typedef struct {
    asx_buf_mut buffer;
    uint32_t generation;
    uint8_t read_alive;
    uint8_t write_alive;
} pipe_slot;

static pipe_slot g_pipes[ASX_MAX_PIPES];

static uint32_t pipe_next_gen(uint32_t g) {
    g++;
    return g == 0u ? 1u : g;
}

static pipe_slot *pipe_lookup_read(asx_pipe_read h) {
    pipe_slot *s;

    if (h.slot >= ASX_MAX_PIPES) return NULL;
    s = &g_pipes[h.slot];
    if (!s->read_alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

static pipe_slot *pipe_lookup_write(asx_pipe_write h) {
    pipe_slot *s;

    if (h.slot >= ASX_MAX_PIPES) return NULL;
    s = &g_pipes[h.slot];
    if (!s->write_alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

asx_status asx_pipe_open(asx_pipe_read *rd, asx_pipe_write *wr) {
    uint32_t i;
    pipe_slot *s;

    if (rd == NULL || wr == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0u; i < ASX_MAX_PIPES; i++) {
        if (!g_pipes[i].read_alive && !g_pipes[i].write_alive) break;
    }
    if (i >= ASX_MAX_PIPES) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_pipes[i];
    memset(s, 0, sizeof(*s));
    s->generation = pipe_next_gen(s->generation);
    s->read_alive = 1u;
    s->write_alive = 1u;
    asx_buf_mut_init(&s->buffer);

    rd->slot = i;
    rd->generation = s->generation;
    wr->slot = i;
    wr->generation = s->generation;
    return ASX_OK;
}

asx_status asx_pipe_poll_read(asx_pipe_read rd, asx_buf_mut *dst, uint32_t *bytes_read) {
    pipe_slot *s;
    asx_buf readable;
    uint32_t to_copy;
    asx_status st;

    if (dst == NULL || bytes_read == NULL) return ASX_E_INVALID_ARGUMENT;
    s = pipe_lookup_read(rd);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    if (asx_buf_mut_remaining(&s->buffer) == 0u) {
        *bytes_read = 0u;
        return s->write_alive ? ASX_E_PENDING : ASX_E_DISCONNECTED;
    }

    readable = asx_buf_mut_readable(&s->buffer);
    to_copy = readable.len;
    if (to_copy > asx_buf_mut_writable(dst)) to_copy = asx_buf_mut_writable(dst);

    st = asx_buf_mut_put(dst, readable.ptr, to_copy);
    if (st != ASX_OK) return st;
    st = asx_buf_mut_advance(&s->buffer, to_copy);
    if (st != ASX_OK) return st;
    if (asx_buf_mut_remaining(&s->buffer) == 0u) asx_buf_mut_clear(&s->buffer);

    *bytes_read = to_copy;
    return ASX_OK;
}

asx_status asx_pipe_poll_write(asx_pipe_write wr, const asx_buf *src, uint32_t *bytes_written) {
    pipe_slot *s;
    asx_status st;

    if (src == NULL || bytes_written == NULL) return ASX_E_INVALID_ARGUMENT;
    s = pipe_lookup_write(wr);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    if (!s->read_alive) {
        *bytes_written = 0u;
        return ASX_E_DISCONNECTED;
    }
    if (src->len > asx_buf_mut_writable(&s->buffer)) {
        *bytes_written = 0u;
        return ASX_E_WOULD_BLOCK;
    }

    st = asx_buf_mut_put(&s->buffer, src->ptr, src->len);
    if (st != ASX_OK) return st;
    *bytes_written = src->len;
    return ASX_OK;
}

asx_status asx_pipe_close_read(asx_pipe_read rd) {
    pipe_slot *s;

    if (rd.slot >= ASX_MAX_PIPES) return ASX_E_INVALID_ARGUMENT;
    s = &g_pipes[rd.slot];
    if (s->generation != rd.generation) return ASX_E_INVALID_ARGUMENT;
    s->read_alive = 0u;
    return ASX_OK;
}

asx_status asx_pipe_close_write(asx_pipe_write wr) {
    pipe_slot *s;

    if (wr.slot >= ASX_MAX_PIPES) return ASX_E_INVALID_ARGUMENT;
    s = &g_pipes[wr.slot];
    if (s->generation != wr.generation) return ASX_E_INVALID_ARGUMENT;
    s->write_alive = 0u;
    return ASX_OK;
}

int asx_pipe_read_is_alive(asx_pipe_read rd) { return pipe_lookup_read(rd) != NULL; }

int asx_pipe_write_is_alive(asx_pipe_write wr) { return pipe_lookup_write(wr) != NULL; }

void asx_pipe_reset(void) { memset(g_pipes, 0, sizeof(g_pipes)); }
