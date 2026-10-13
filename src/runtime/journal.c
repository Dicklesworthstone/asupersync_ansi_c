/*
 * journal.c — native boundary journal (bd-9kll.11.1, RP1)
 *
 * The stream format and the API are documented in
 * include/asx/runtime/journal.h. This file stages records into a
 * caller-owned buffer, chains them with SHA-256, hands whole buffers to a
 * sink, wraps the hook table's nondeterministic entry points, and reads a
 * stream back.
 *
 * Guarantee Impact: recording only observes. A wrapped hook returns what
 * the original returned; a note function returns nothing; no runtime state,
 * trace or digest depends on the journal.
 *
 * SPDX-License-Identifier: MIT
 */

#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/journal.h>
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian field access                                          */
/* ------------------------------------------------------------------ */

static uint8_t *put_u8(uint8_t *p, uint8_t v) {
    *p++ = v;
    return p;
}

static uint8_t *put_u16(uint8_t *p, uint16_t v) {
    *p++ = (uint8_t)(v & 0xFFu);
    *p++ = (uint8_t)((v >> 8) & 0xFFu);
    return p;
}

static uint8_t *put_u32(uint8_t *p, uint32_t v) {
    p = put_u16(p, (uint16_t)(v & 0xFFFFu));
    return put_u16(p, (uint16_t)((v >> 16) & 0xFFFFu));
}

static uint8_t *put_u64(uint8_t *p, uint64_t v) {
    p = put_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    return put_u32(p, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint8_t *put_bytes(uint8_t *p, const uint8_t *src, uint32_t n) {
    if (n > 0u) memcpy(p, src, (size_t)n);
    return p + n;
}

uint16_t asx_journal_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

uint32_t asx_journal_get_u32(const uint8_t *p) {
    return (uint32_t)asx_journal_get_u16(p) | ((uint32_t)asx_journal_get_u16(p + 2) << 16);
}

uint64_t asx_journal_get_u64(const uint8_t *p) {
    return (uint64_t)asx_journal_get_u32(p) | ((uint64_t)asx_journal_get_u32(p + 4) << 32);
}

uint64_t asx_journal_text_hash(const char *text) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    if (text == NULL) return 0u;
    while (*text != '\0') {
        ASX_CHECKPOINT_WAIVER("bounded: one pass over a NUL-terminated string");
        h ^= (uint64_t)(unsigned char)*text++;
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* FNV-1a-64 over `n` bytes: the shape of data a record does not carry. */
static uint64_t fnv1a64(const uint8_t *data, uint32_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    uint32_t i;
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: one pass over the n data bytes");
        h ^= (uint64_t)data[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

static const uint8_t k_magic[8] = {'A', 'S', 'X', 'J', 'R', 'N', 'L', 0u};

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

static asx_journal *g_active;

static void j_lock(asx_journal *j) {
    uint32_t expected;
    for (;;) {
        ASX_CHECKPOINT_WAIVER("spinlock: the holder stages one record and releases");
        expected = 0u;
        if (asx_atomic_u32_compare_exchange(&j->lock, &expected, 1u)) return;
    }
}

static void j_unlock(asx_journal *j) { asx_atomic_u32_store(&j->lock, 0u); }

/* Hand the staged bytes to the sink (lock held). After a failure the sink
 * is not called again and what is staged is counted as dropped. */
static void j_sink_locked(asx_journal *j) {
    if (j->used == 0u) return;
    if (j->status == ASX_OK) {
        asx_status st = j->cfg.sink_fn(j->cfg.sink_ctx, j->cfg.buffer, j->used);
        j->stats.sink_calls++;
        if (st != ASX_OK) {
            j->status = st;
            j->stats.dropped += j->staged_data;
        }
    } else {
        j->stats.dropped += j->staged_data;
    }
    j->used = 0u;
    j->staged_data = 0u;
}

static void j_stage(asx_journal *j, const uint8_t *src, uint32_t n) {
    /* ASX_ANALYZER_WAIVER("gcc: every caller's n bytes were written by put_* or put_bytes") */
    memcpy(j->cfg.buffer + j->used, src, (size_t)n);
    j->used += n;
    j->stats.bytes += n;
}

/* A CHAIN record over the window (lock held, room guaranteed by the
 * reserve). Not part of the next window: it carries the digest that the
 * next window starts from. With `sampled` (the window's last record was
 * taken on the runtime thread, where the trace may be read) it also
 * carries the trace position and rolling digest as of that record; the
 * chain value covers those 16 bytes, so a checkpoint cannot be edited. */
static void j_chain_locked(asx_journal *j, int sampled) {
    uint8_t rec[ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_PAYLOAD_BYTES];
    uint8_t trace[ASX_JOURNAL_CHAIN_TRACE_BYTES];
    uint8_t *p = rec;
    uint64_t events = 0u;
    uint64_t digest = 0u;
    uint8_t flags = 0u;
    if (sampled) {
        events = asx_trace_emitted_total();
        digest = asx_trace_digest();
        flags = (uint8_t)ASX_JOURNAL_REC_TRACE_CHECKPOINT;
    }
    (void)put_u64(put_u64(trace, events), digest);
    asx_sha256_update(&j->window, trace, ASX_JOURNAL_CHAIN_TRACE_BYTES);
    asx_sha256_final(&j->window, j->chain);
    p = put_u32(p, ASX_JOURNAL_CHAIN_PAYLOAD_BYTES);
    p = put_u8(p, (uint8_t)ASX_JOURNAL_KIND_CHAIN);
    p = put_u8(p, flags);
    p = put_u16(p, 0u);
    p = put_u64(p, j->seq);
    p = put_u64(p, events);
    p = put_bytes(p, j->chain, ASX_SHA256_DIGEST_SIZE);
    p = put_bytes(p, trace, ASX_JOURNAL_CHAIN_TRACE_BYTES);
    j_stage(j, rec, (uint32_t)(p - rec));
    j->seq++;
    j->stats.chains++;
    j->since_chain = 0u;
    j->chain_trace_valid = sampled;
    j->chain_events = events;
    j->chain_digest = digest;
    asx_sha256_init(&j->window);
    asx_sha256_update(&j->window, j->chain, ASX_SHA256_DIGEST_SIZE);
}

/* A commit point (lock held), reached after a chain (or, `final`, after
 * END) when the config persists at chains: every `commit_every_chains`th
 * chain hands the staged bytes to the sink and tells commit_fn that the
 * stream so far is a verified prefix. A failure of either is the
 * journal's sticky status. */
static void j_commit_locked(asx_journal *j, int final) {
    asx_journal_commit info;
    asx_status st;
    uint32_t every;
    if (j->cfg.persist != ASX_JOURNAL_PERSIST_CHAIN) return;
    every = j->cfg.commit_every_chains != 0u ? j->cfg.commit_every_chains : 1u;
    if (!final) {
        j->chains_since_commit++;
        if (j->chains_since_commit < every) return;
    }
    j->chains_since_commit = 0u;
    j_sink_locked(j);
    if (j->status != ASX_OK) return;
    j->stats.commits++;
    j->stats.committed_bytes = j->stats.bytes;
    if (j->cfg.commit_fn == NULL) return;
    memset(&info, 0, sizeof(info));
    info.chain_seq = j->seq - 1u;
    info.data_records = j->stats.records;
    info.stream_bytes = j->stats.bytes;
    info.final = final;
    if (!final && j->chain_trace_valid) {
        info.trace_valid = 1;
        info.trace_events = j->chain_events;
        info.trace_digest = j->chain_digest;
    }
    st = j->cfg.commit_fn(j->cfg.sink_ctx, &info);
    if (st != ASX_OK) j->status = st;
}

/* The largest payload one record can have in this journal: what is left of
 * the buffer after the chain-and-end reserve and the record head. */
static uint32_t j_max_payload(const asx_journal *j) {
    return j->cfg.buffer_cap - ASX_JOURNAL_RESERVE_BYTES - ASX_JOURNAL_RECORD_HEAD_BYTES;
}

/* Begin one data record of `payload` bytes (lock taken here): stages its
 * head and returns where the payload goes, `payload` bytes the caller must
 * fill before j_end. NULL, with the lock released and the record counted
 * as dropped, when the journal is finished or failed or the record cannot
 * fit the buffer. */
static uint8_t *j_begin(asx_journal *j, uint8_t kind, uint8_t flags, int sample_trace,
                        uint32_t payload) {
    uint8_t rec[ASX_JOURNAL_RECORD_HEAD_BYTES];
    uint8_t *p = rec;
    uint8_t *dst;
    uint32_t need = ASX_JOURNAL_RECORD_HEAD_BYTES + payload;
    uint32_t room;

    j_lock(j);
    if (j->finished || j->status != ASX_OK) {
        j->stats.dropped++;
        j_unlock(j);
        return NULL;
    }
    room = j->cfg.buffer_cap - ASX_JOURNAL_RESERVE_BYTES;
    if (need > room) {
        j->status = ASX_E_BUFFER_TOO_SMALL;
        j->stats.dropped++;
        j_unlock(j);
        return NULL;
    }
    if (j->used + need > room) {
        j_sink_locked(j);
        if (j->status != ASX_OK) {
            j->stats.dropped++;
            j_unlock(j);
            return NULL;
        }
    }
    p = put_u32(p, payload);
    p = put_u8(p, kind);
    p = put_u8(p, flags);
    p = put_u16(p, 0u);
    p = put_u64(p, j->seq);
    p = put_u64(p, sample_trace ? asx_trace_emitted_total() : 0u);
    j_stage(j, rec, (uint32_t)(p - rec));
    dst = j->cfg.buffer + j->used;
    j->used += payload;
    j->stats.bytes += payload;
    j->closing_sampled = sample_trace;
    return dst;
}

/* Finish the record j_begin started (lock held): fold its bytes into the
 * chain window, count it, chain when due, release the lock. */
static void j_end(asx_journal *j, uint32_t payload) {
    uint32_t total = ASX_JOURNAL_RECORD_HEAD_BYTES + payload;
    asx_sha256_update(&j->window, j->cfg.buffer + (j->used - total), total);
    j->seq++;
    j->stats.records++;
    j->staged_data++;
    j->since_chain++;
    if (j->since_chain >= j->chain_every) {
        j_chain_locked(j, j->closing_sampled);
        j_commit_locked(j, 0);
    }
    j_unlock(j);
}

/* Stage one data record whose payload is `head` then `tail`. */
static void j_emit(uint8_t kind, uint8_t flags, int sample_trace, const uint8_t *head,
                   uint32_t head_len, const uint8_t *tail, uint32_t tail_len) {
    asx_journal *j = g_active;
    uint32_t payload = head_len + tail_len;
    uint8_t *dst;

    if (j == NULL) return;
    dst = j_begin(j, kind, flags, sample_trace, payload);
    if (dst == NULL) return;
    if (head_len > 0u) memcpy(dst, head, (size_t)head_len);
    if (tail_len > 0u) memcpy(dst + head_len, tail, (size_t)tail_len);
    j_end(j, payload);
}

asx_status asx_journal_init(asx_journal *j, const asx_journal_config *cfg) {
    uint8_t header[ASX_JOURNAL_HEADER_BYTES];
    uint8_t *p = header;
    if (j == NULL || cfg == NULL || cfg->sink_fn == NULL || cfg->buffer == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (cfg->buffer_cap < ASX_JOURNAL_MIN_BUFFER) return ASX_E_BUFFER_TOO_SMALL;
    memset(j, 0, sizeof(*j));
    j->cfg = *cfg;
    j->chain_every = cfg->chain_every != 0u ? cfg->chain_every : ASX_JOURNAL_DEFAULT_CHAIN_EVERY;
    asx_atomic_u32_init(&j->lock, 0u);
    p = put_bytes(p, k_magic, 8u);
    p = put_u32(p, ASX_JOURNAL_VERSION);
    p = put_u32(p, cfg->flags);
    p = put_u64(p, cfg->config_fingerprint);
    p = put_u64(p, cfg->rust_rev_hash);
    p = put_u64(p, cfg->build_hash);
    memset(p, 0, 8u);
    j_stage(j, header, ASX_JOURNAL_HEADER_BYTES);
    asx_sha256(header, ASX_JOURNAL_HEADER_BYTES, j->chain);
    asx_sha256_init(&j->window);
    asx_sha256_update(&j->window, j->chain, ASX_SHA256_DIGEST_SIZE);
    j->started = 1;
    return ASX_OK;
}

asx_status asx_journal_install(asx_journal *j) {
    if (j == NULL || !j->started) return ASX_E_INVALID_ARGUMENT;
    if (g_active != NULL && g_active != j) return ASX_E_INVALID_STATE;
    g_active = j;
    return ASX_OK;
}

void asx_journal_uninstall(void) { g_active = NULL; }

int asx_journal_active(void) { return g_active != NULL; }

void asx_journal_flush(asx_journal *j) {
    if (j == NULL) return;
    j_lock(j);
    j_sink_locked(j);
    j_unlock(j);
}

asx_status asx_journal_finish(asx_journal *j) {
    uint8_t rec[ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_END_PAYLOAD_BYTES];
    uint8_t *p = rec;
    asx_status st;
    if (j == NULL) return ASX_E_INVALID_ARGUMENT;
    j_lock(j);
    if (!j->finished) {
        /* Not sampled: the last record's trace position is gone by now. */
        if (j->since_chain > 0u) j_chain_locked(j, 0);
        p = put_u32(p, ASX_JOURNAL_END_PAYLOAD_BYTES);
        p = put_u8(p, (uint8_t)ASX_JOURNAL_KIND_END);
        p = put_u8(p, 0u);
        p = put_u16(p, 0u);
        p = put_u64(p, j->seq);
        p = put_u64(p, 0u);
        p = put_u64(p, j->stats.records);
        p = put_u64(p, j->stats.bytes);
        p = put_u64(p, j->stats.dropped);
        p = put_u32(p, (uint32_t)(int32_t)j->status);
        p = put_u32(p, 0u);
        p = put_bytes(p, j->chain, ASX_SHA256_DIGEST_SIZE);
        j_stage(j, rec, (uint32_t)(p - rec));
        j->seq++;
        j->finished = 1;
        j_sink_locked(j);
        j_commit_locked(j, 1);
    }
    st = j->status;
    j_unlock(j);
    return st;
}

asx_status asx_journal_status(const asx_journal *j) {
    return j != NULL ? j->status : ASX_E_INVALID_ARGUMENT;
}

void asx_journal_get_stats(const asx_journal *j, asx_journal_stats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (j != NULL) *out = j->stats;
}

/* ------------------------------------------------------------------ */
/* Ring sink                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_journal_ring_init(asx_journal_ring *r, uint8_t *storage, uint32_t cap) {
    if (r == NULL || storage == NULL || cap == 0u) return ASX_E_INVALID_ARGUMENT;
    memset(r, 0, sizeof(*r));
    r->storage = storage;
    r->cap = cap;
    return ASX_OK;
}

asx_status asx_journal_ring_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    asx_journal_ring *r = (asx_journal_ring *)ctx;
    uint32_t tail;
    uint32_t first;
    if (r == NULL || r->storage == NULL || (bytes == NULL && len > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    r->total += (uint64_t)len;
    /* Nothing to store, and bytes may be NULL here: memcpy's pointers must be
     * non-NULL even for a zero length. */
    if (len == 0u) return ASX_OK;
    if (len >= r->cap) {
        memcpy(r->storage, bytes + (len - r->cap), (size_t)r->cap);
        r->head = 0u;
        r->len = r->cap;
        return ASX_OK;
    }
    if (r->len + len > r->cap) {
        uint32_t over = r->len + len - r->cap;
        r->head = (r->head + over) % r->cap;
        r->len -= over;
    }
    tail = (r->head + r->len) % r->cap;
    first = r->cap - tail;
    if (first > len) first = len;
    memcpy(r->storage + tail, bytes, (size_t)first);
    if (len > first) memcpy(r->storage, bytes + first, (size_t)(len - first));
    r->len += len;
    return ASX_OK;
}

uint32_t asx_journal_ring_read(const asx_journal_ring *r, uint8_t *dst, uint32_t cap) {
    uint32_t n;
    uint32_t first;
    if (r == NULL || dst == NULL || r->storage == NULL) return 0u;
    n = r->len < cap ? r->len : cap;
    first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(dst, r->storage + r->head, (size_t)first);
    if (n > first) memcpy(dst + first, r->storage, (size_t)(n - first));
    return n;
}

/* ------------------------------------------------------------------ */
/* Persistence stores                                                  */
/* ------------------------------------------------------------------ */

asx_journal_store_kind asx_journal_profile_store(void) {
#if defined(ASX_PROFILE_POSIX)
    return ASX_JOURNAL_STORE_FILE;
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    return ASX_JOURNAL_STORE_FLASH;
#else
    return ASX_JOURNAL_STORE_CALLER;
#endif
}

asx_status asx_journal_flash_init(asx_journal_flash *f, uint8_t *ring, uint32_t ring_cap,
                                  asx_journal_flash_write_fn write_fn, void *write_ctx,
                                  uint64_t area_bytes, uint64_t wear_budget, uint32_t min_flush) {
    if (f == NULL || ring == NULL || ring_cap == 0u || write_fn == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    memset(f, 0, sizeof(*f));
    f->write_fn = write_fn;
    f->write_ctx = write_ctx;
    f->ring = ring;
    f->ring_cap = ring_cap;
    f->area_bytes = area_bytes;
    f->wear_budget = wear_budget;
    f->min_flush = min_flush;
    return ASX_OK;
}

asx_status asx_journal_flash_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    asx_journal_flash *f = (asx_journal_flash *)ctx;
    uint32_t tail;
    uint32_t first;
    if (f == NULL || f->ring == NULL || (bytes == NULL && len > 0u)) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (f->status != ASX_OK) return f->status;
    if (len == 0u) return ASX_OK;
    if (len > f->ring_cap - f->len) {
        /* Nothing already in flash is lost, but the stream cannot go on. */
        f->status = ASX_E_RESOURCE_EXHAUSTED;
        return f->status;
    }
    tail = (f->head + f->len) % f->ring_cap;
    first = f->ring_cap - tail;
    if (first > len) first = len;
    memcpy(f->ring + tail, bytes, (size_t)first);
    if (len > first) memcpy(f->ring, bytes + first, (size_t)(len - first));
    f->len += len;
    f->offered += (uint64_t)len;
    return ASX_OK;
}

/* Write `n` bytes from the ring's head, in one flash write, and release
 * them. n <= the bytes before the ring's end. */
static asx_status flash_write_head(asx_journal_flash *f, uint32_t n) {
    asx_status st = f->write_fn(f->write_ctx, f->flushed, f->ring + f->head, n);
    f->flash_writes++;
    if (st != ASX_OK) {
        f->status = st;
        return st;
    }
    f->head = (f->head + n) % f->ring_cap;
    f->len -= n;
    f->flushed += (uint64_t)n;
    return ASX_OK;
}

asx_status asx_journal_flash_commit(void *ctx, const asx_journal_commit *commit) {
    asx_journal_flash *f = (asx_journal_flash *)ctx;
    uint64_t pending;
    uint32_t n;
    uint32_t first;
    asx_status st;
    if (f == NULL || commit == NULL) return ASX_E_INVALID_ARGUMENT;
    if (commit->stream_bytes < f->flushed || commit->stream_bytes > f->offered) {
        return ASX_E_INVALID_STATE;
    }
    f->committed = commit->stream_bytes;
    pending = f->committed - f->flushed;
    if (pending == 0u) return ASX_OK;
    if (!commit->final && pending < (uint64_t)f->min_flush) return ASX_OK;
    /* The whole prefix or none of it: flash must end on a chain. */
    if ((f->wear_budget != 0u && pending > f->wear_budget - f->flushed) ||
        (f->area_bytes != 0u && pending > f->area_bytes - f->flushed)) {
        f->wear_exhausted = 1;
        return ASX_OK;
    }
    n = (uint32_t)pending;
    first = f->ring_cap - f->head;
    if (first > n) first = n;
    st = flash_write_head(f, first);
    if (st == ASX_OK && n > first) st = flash_write_head(f, n - first);
    return st;
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

asx_journal_read_result asx_journal_reader_init(asx_journal_reader *r, const uint8_t *bytes,
                                                size_t len) {
    uint32_t i;
    if (r == NULL || bytes == NULL) return ASX_JOURNAL_READ_BAD_HEADER;
    memset(r, 0, sizeof(*r));
    if (len < ASX_JOURNAL_HEADER_BYTES) return ASX_JOURNAL_READ_TRUNCATED;
    if (memcmp(bytes, k_magic, 8u) != 0) return ASX_JOURNAL_READ_BAD_HEADER;
    if (asx_journal_get_u32(bytes + 8) != ASX_JOURNAL_VERSION) return ASX_JOURNAL_READ_BAD_HEADER;
    for (i = 40u; i < ASX_JOURNAL_HEADER_BYTES; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: the 8 reserved header bytes");
        if (bytes[i] != 0u) return ASX_JOURNAL_READ_BAD_HEADER;
    }
    r->bytes = bytes;
    r->len = len;
    r->pos = ASX_JOURNAL_HEADER_BYTES;
    r->chain_end_pos = ASX_JOURNAL_HEADER_BYTES;
    r->flags = asx_journal_get_u32(bytes + 12);
    r->config_fingerprint = asx_journal_get_u64(bytes + 16);
    r->rust_rev_hash = asx_journal_get_u64(bytes + 24);
    r->build_hash = asx_journal_get_u64(bytes + 32);
    asx_sha256(bytes, ASX_JOURNAL_HEADER_BYTES, r->chain);
    asx_sha256_init(&r->window);
    asx_sha256_update(&r->window, r->chain, ASX_SHA256_DIGEST_SIZE);
    return ASX_JOURNAL_READ_RECORD;
}

static asx_journal_read_result reader_next_impl(asx_journal_reader *r, asx_journal_record *out) {
    for (;;) {
        const uint8_t *head;
        uint32_t payload_len;
        uint8_t kind;
        ASX_CHECKPOINT_WAIVER("bounded: every pass consumes one record of the buffer");
        if (r == NULL || out == NULL || r->bytes == NULL) return ASX_JOURNAL_READ_BAD_HEADER;
        if (r->ended) return ASX_JOURNAL_READ_END;
        if (r->len - r->pos < ASX_JOURNAL_RECORD_HEAD_BYTES) return ASX_JOURNAL_READ_TRUNCATED;
        head = r->bytes + r->pos;
        payload_len = asx_journal_get_u32(head);
        kind = head[4];
        if ((size_t)payload_len > r->len - r->pos - ASX_JOURNAL_RECORD_HEAD_BYTES) {
            return ASX_JOURNAL_READ_TRUNCATED;
        }
        if (head[6] != 0u || head[7] != 0u) return ASX_JOURNAL_READ_CORRUPT;
        if (asx_journal_get_u64(head + 8) != r->next_seq) return ASX_JOURNAL_READ_CORRUPT;
        if (kind == (uint8_t)ASX_JOURNAL_KIND_CHAIN) {
            uint8_t digest[ASX_SHA256_DIGEST_SIZE];
            const uint8_t *trace =
                head + ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_TRACE_OFFSET;
            if (payload_len != ASX_JOURNAL_CHAIN_PAYLOAD_BYTES) return ASX_JOURNAL_READ_CORRUPT;
            /* The chain value covers the trace fields: an edited checkpoint
             * breaks the chain like an edited record. */
            asx_sha256_update(&r->window, trace, ASX_JOURNAL_CHAIN_TRACE_BYTES);
            asx_sha256_final(&r->window, digest);
            if (memcmp(digest, head + ASX_JOURNAL_RECORD_HEAD_BYTES, sizeof(digest)) != 0) {
                return ASX_JOURNAL_READ_CORRUPT;
            }
            memcpy(r->chain, digest, sizeof(digest));
            asx_sha256_init(&r->window);
            asx_sha256_update(&r->window, r->chain, ASX_SHA256_DIGEST_SIZE);
            r->since_chain = 0u;
            r->chains++;
            r->next_seq++;
            r->pos += ASX_JOURNAL_RECORD_HEAD_BYTES + payload_len;
            r->chain_end_pos = r->pos;
            r->chain_end_seq = r->next_seq;
            r->chain_end_records = r->data_records;
            r->chain_trace_valid = (head[5] & (uint8_t)ASX_JOURNAL_REC_TRACE_CHECKPOINT) != 0u;
            r->chain_trace_events = r->chain_trace_valid ? asx_journal_get_u64(trace) : 0u;
            r->chain_trace_digest = r->chain_trace_valid ? asx_journal_get_u64(trace + 8) : 0u;
            continue;
        }
        if (kind == (uint8_t)ASX_JOURNAL_KIND_END) {
            const uint8_t *e = head + ASX_JOURNAL_RECORD_HEAD_BYTES;
            if (payload_len != ASX_JOURNAL_END_PAYLOAD_BYTES) return ASX_JOURNAL_READ_CORRUPT;
            /* finish() chains what is left before it ends the stream. */
            if (r->since_chain != 0u) return ASX_JOURNAL_READ_CORRUPT;
            if (asx_journal_get_u64(e) != r->data_records) return ASX_JOURNAL_READ_CORRUPT;
            if (asx_journal_get_u64(e + 8) != (uint64_t)r->pos) return ASX_JOURNAL_READ_CORRUPT;
            if (memcmp(e + 32, r->chain, ASX_SHA256_DIGEST_SIZE) != 0) {
                return ASX_JOURNAL_READ_CORRUPT;
            }
            r->pos += ASX_JOURNAL_RECORD_HEAD_BYTES + payload_len;
            r->next_seq++;
            if (r->pos != r->len) return ASX_JOURNAL_READ_CORRUPT;
            r->end_dropped = asx_journal_get_u64(e + 16);
            r->end_status = (int32_t)asx_journal_get_u32(e + 24);
            r->ended = 1;
            return ASX_JOURNAL_READ_END;
        }
        asx_sha256_update(&r->window, head, ASX_JOURNAL_RECORD_HEAD_BYTES + payload_len);
        if (r->since_chain == 0u) r->window_first_seq = r->next_seq;
        out->kind = kind;
        out->flags = head[5];
        out->seq = r->next_seq;
        out->trace_seq = asx_journal_get_u64(head + 16);
        out->payload = head + ASX_JOURNAL_RECORD_HEAD_BYTES;
        out->payload_len = payload_len;
        r->next_seq++;
        r->data_records++;
        r->since_chain++;
        r->pos += ASX_JOURNAL_RECORD_HEAD_BYTES + payload_len;
        return ASX_JOURNAL_READ_RECORD;
    }
}

asx_journal_read_result asx_journal_reader_next(asx_journal_reader *r, asx_journal_record *out) {
    asx_journal_read_result rr = reader_next_impl(r, out);
    if (r != NULL && rr != ASX_JOURNAL_READ_RECORD && rr != ASX_JOURNAL_READ_END) {
        r->fail_seq = r->next_seq;
        r->fail_first_seq = r->window_first_seq;
    }
    return rr;
}

/* ------------------------------------------------------------------ */
/* Boundary notes                                                      */
/* ------------------------------------------------------------------ */

static uint8_t *put_addr(uint8_t *p, const asx_journal_addr *a) {
    asx_journal_addr zero;
    if (a == NULL) {
        memset(&zero, 0, sizeof(zero));
        a = &zero;
    }
    p = put_u8(p, a->family);
    p = put_bytes(p, a->bytes, 16u);
    return put_u16(p, a->port);
}

#define JADDR_BYTES 19u

/* The data of a read or receive: FULL bytes when they fit and the mode
 * asks for them, otherwise their FNV-1a-64 with the SHAPE flag. `shape`
 * must hold 8 bytes. */
static const uint8_t *data_part(const uint8_t *data, uint32_t n, uint32_t head_len, uint8_t *flags,
                                uint8_t *shape, uint32_t *out_len) {
    asx_journal *j = g_active;
    if (j != NULL && (j->cfg.flags & ASX_JOURNAL_FLAG_SHAPE) == 0u && n <= ASX_JOURNAL_MAX_DATA &&
        head_len <= j_max_payload(j) && n <= j_max_payload(j) - head_len) {
        *out_len = n;
        return data;
    }
    (void)put_u64(shape, fnv1a64(data, n));
    *flags = (uint8_t)(*flags | ASX_JOURNAL_REC_SHAPE);
    *out_len = 8u;
    return shape;
}

/* The status and count of a boundary call whose bytes were not produced
 * carry no data. */
static uint32_t data_len_for(int32_t status, uint32_t n) {
    return status == (int32_t)ASX_OK ? n : 0u;
}

void asx_journal_note_net_listen(const asx_journal_addr *want, int32_t status, uint64_t listener,
                                 const asx_journal_addr *local) {
    uint8_t b[JADDR_BYTES + 4 + 8 + JADDR_BYTES];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_addr(p, want);
    p = put_u32(p, (uint32_t)status);
    p = put_u64(p, listener);
    p = put_addr(p, local);
    j_emit((uint8_t)ASX_JOURNAL_KIND_NET_LISTEN, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_udp_bind(const asx_journal_addr *want, int32_t status, uint64_t socket,
                               const asx_journal_addr *local) {
    uint8_t b[JADDR_BYTES + 4 + 8 + JADDR_BYTES];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_addr(p, want);
    p = put_u32(p, (uint32_t)status);
    p = put_u64(p, socket);
    p = put_addr(p, local);
    j_emit((uint8_t)ASX_JOURNAL_KIND_UDP_BIND, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_net_ctl(uint8_t op, uint64_t handle, int32_t status) {
    uint8_t b[1 + 8 + 4];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u8(p, op);
    p = put_u64(p, handle);
    p = put_u32(p, (uint32_t)status);
    j_emit((uint8_t)ASX_JOURNAL_KIND_NET_CTL, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_net_accept(uint64_t listener, int32_t status, uint64_t stream,
                                 const asx_journal_addr *peer, const asx_journal_addr *local) {
    uint8_t b[8 + 4 + 8 + JADDR_BYTES + JADDR_BYTES];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u64(p, listener);
    p = put_u32(p, (uint32_t)status);
    p = put_u64(p, stream);
    p = put_addr(p, peer);
    p = put_addr(p, local);
    j_emit((uint8_t)ASX_JOURNAL_KIND_NET_ACCEPT, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_net_connect(const asx_journal_addr *to, int32_t status, uint64_t stream,
                                  const asx_journal_addr *local) {
    uint8_t b[JADDR_BYTES + 4 + 8 + JADDR_BYTES];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_addr(p, to);
    p = put_u32(p, (uint32_t)status);
    p = put_u64(p, stream);
    p = put_addr(p, local);
    j_emit((uint8_t)ASX_JOURNAL_KIND_NET_CONNECT, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_net_read(uint64_t stream, uint32_t cap, int32_t status, const uint8_t *data,
                               uint32_t n) {
    uint8_t b[8 + 4 + 4 + 4];
    uint8_t shape[8];
    uint8_t flags = 0u;
    uint8_t *p = b;
    uint32_t tail_len = 0u;
    const uint8_t *tail;
    if (g_active == NULL) return;
    n = data_len_for(status, n);
    p = put_u64(p, stream);
    p = put_u32(p, cap);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, n);
    tail = data_part(data, n, (uint32_t)(p - b), &flags, shape, &tail_len);
    j_emit((uint8_t)ASX_JOURNAL_KIND_NET_READ, flags, 1, b, (uint32_t)(p - b), tail, tail_len);
}

void asx_journal_note_net_write(uint64_t stream, uint32_t len, int32_t status, uint32_t written) {
    uint8_t b[8 + 4 + 4 + 4];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u64(p, stream);
    p = put_u32(p, len);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, data_len_for(status, written));
    j_emit((uint8_t)ASX_JOURNAL_KIND_NET_WRITE, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_udp_recv(uint64_t socket, uint32_t cap, int32_t status, const uint8_t *data,
                               uint32_t n, const asx_journal_addr *from) {
    uint8_t b[8 + 4 + 4 + 4 + JADDR_BYTES];
    uint8_t shape[8];
    uint8_t flags = 0u;
    uint8_t *p = b;
    uint32_t tail_len = 0u;
    const uint8_t *tail;
    if (g_active == NULL) return;
    n = data_len_for(status, n);
    p = put_u64(p, socket);
    p = put_u32(p, cap);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, n);
    p = put_addr(p, from);
    tail = data_part(data, n, (uint32_t)(p - b), &flags, shape, &tail_len);
    j_emit((uint8_t)ASX_JOURNAL_KIND_UDP_RECV, flags, 1, b, (uint32_t)(p - b), tail, tail_len);
}

void asx_journal_note_udp_send(uint64_t socket, uint32_t len, int32_t status, uint32_t written) {
    uint8_t b[8 + 4 + 4 + 4];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u64(p, socket);
    p = put_u32(p, len);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, data_len_for(status, written));
    j_emit((uint8_t)ASX_JOURNAL_KIND_UDP_SEND, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

static void resolve_emit(uint8_t flags, int sample_trace, uint64_t name_hash, int32_t status,
                         const asx_journal_addr *addrs, uint32_t count) {
    uint8_t b[8 + 4 + 4];
    uint8_t body[16u * JADDR_BYTES];
    uint8_t *p = b;
    uint8_t *q = body;
    uint32_t i;
    if (g_active == NULL) return;
    if (count > 16u) count = 16u;
    p = put_u64(p, name_hash);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, count);
    for (i = 0; i < count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: at most 16 resolved addresses");
        q = put_addr(q, &addrs[i]);
    }
    j_emit((uint8_t)ASX_JOURNAL_KIND_RESOLVE, flags, sample_trace, b, (uint32_t)(p - b), body,
           (uint32_t)(q - body));
}

void asx_journal_note_resolve(uint64_t name_hash, int32_t status, const asx_journal_addr *addrs,
                              uint32_t count) {
    resolve_emit(0u, 1, name_hash, status, addrs, count);
}

void asx_journal_note_resolve_job(uint64_t name_hash, int32_t status, const asx_journal_addr *addrs,
                                  uint32_t count) {
    /* Called by a blocking job: the trace counter belongs to the runtime
     * thread, so the record carries no trace position. */
    resolve_emit(ASX_JOURNAL_REC_OFFTHREAD, 0, name_hash, status, addrs, count);
}

void asx_journal_note_process_spawn(int32_t status, uint64_t handle, uint32_t pid,
                                    uint64_t command_hash, uint8_t flags) {
    uint8_t b[4 + 8 + 4 + 8 + 1];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u32(p, (uint32_t)status);
    p = put_u64(p, handle);
    p = put_u32(p, pid);
    p = put_u64(p, command_hash);
    p = put_u8(p, flags);
    j_emit((uint8_t)ASX_JOURNAL_KIND_PROCESS_SPAWN, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_process_write(uint64_t handle, uint32_t len, int32_t status,
                                    uint32_t written) {
    uint8_t b[8 + 4 + 4 + 4];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u64(p, handle);
    p = put_u32(p, len);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, data_len_for(status, written));
    j_emit((uint8_t)ASX_JOURNAL_KIND_PROCESS_WRITE, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_process_read(uint64_t handle, uint8_t which, uint32_t cap, int32_t status,
                                   const uint8_t *data, uint32_t n) {
    uint8_t b[8 + 1 + 4 + 4 + 4];
    uint8_t shape[8];
    uint8_t flags = 0u;
    uint8_t *p = b;
    uint32_t tail_len = 0u;
    const uint8_t *tail;
    if (g_active == NULL) return;
    n = data_len_for(status, n);
    p = put_u64(p, handle);
    p = put_u8(p, which);
    p = put_u32(p, cap);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, n);
    tail = data_part(data, n, (uint32_t)(p - b), &flags, shape, &tail_len);
    j_emit((uint8_t)ASX_JOURNAL_KIND_PROCESS_READ, flags, 1, b, (uint32_t)(p - b), tail, tail_len);
}

void asx_journal_note_process_exit(uint64_t handle, uint32_t state, int32_t code, int32_t signal) {
    uint8_t b[8 + 4 + 4 + 4];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u64(p, handle);
    p = put_u32(p, state);
    p = put_u32(p, (uint32_t)code);
    p = put_u32(p, (uint32_t)signal);
    j_emit((uint8_t)ASX_JOURNAL_KIND_PROCESS_EXIT, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_signal(const uint32_t *counts, uint32_t kinds) {
    uint8_t b[1u + ASX_JOURNAL_MAX_SIGNAL_KINDS * 5u];
    uint8_t *p = b + 1;
    uint32_t i;
    uint8_t n = 0u;
    if (g_active == NULL || counts == NULL) return;
    if (kinds > ASX_JOURNAL_MAX_SIGNAL_KINDS) kinds = ASX_JOURNAL_MAX_SIGNAL_KINDS;
    for (i = 0; i < kinds; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: at most ASX_JOURNAL_MAX_SIGNAL_KINDS kinds");
        if (counts[i] == 0u) continue;
        p = put_u8(p, (uint8_t)i);
        p = put_u32(p, counts[i]);
        n++;
    }
    /* Recorded even when empty: a replay serves drains in order. */
    (void)put_u8(b, n);
    j_emit((uint8_t)ASX_JOURNAL_KIND_SIGNAL, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

void asx_journal_note_blocking_done(uint32_t slot, uint16_t generation, uint64_t result) {
    uint8_t b[4 + 2 + 8];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u32(p, slot);
    p = put_u16(p, generation);
    p = put_u64(p, result);
    /* Runs on a pool thread: the trace counter belongs to the runtime
     * thread, so the record carries no trace position. */
    j_emit((uint8_t)ASX_JOURNAL_KIND_BLOCKING_DONE, ASX_JOURNAL_REC_OFFTHREAD, 0, b,
           (uint32_t)(p - b), NULL, 0u);
}

/* ------------------------------------------------------------------ */
/* Hook table wrapping                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_clock_now_ns_fn now;
    asx_clock_now_ns_fn logical;
    asx_entropy_u64_fn random;
    asx_entropy_try_u64_fn try_random;
    asx_reactor_wait_fn wait;
    asx_ghost_reactor_wait_fn ghost_wait;
    asx_reactor_poll_fn poll;
} journal_originals;

static journal_originals g_orig;

static void note_clock(uint8_t which, uint64_t ns) {
    uint8_t b[1 + 8];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u8(p, which);
    p = put_u64(p, ns);
    j_emit((uint8_t)ASX_JOURNAL_KIND_CLOCK, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

static void note_entropy(uint8_t api, int32_t status, uint64_t value) {
    uint8_t b[1 + 4 + 8];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u8(p, api);
    p = put_u32(p, (uint32_t)status);
    p = put_u64(p, value);
    j_emit((uint8_t)ASX_JOURNAL_KIND_ENTROPY, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

static void note_wait(uint8_t api, uint64_t arg, int32_t status, uint32_t ready) {
    uint8_t b[1 + 8 + 4 + 4];
    uint8_t *p = b;
    if (g_active == NULL) return;
    p = put_u8(p, api);
    p = put_u64(p, arg);
    p = put_u32(p, (uint32_t)status);
    p = put_u32(p, ready);
    j_emit((uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT, 0u, 1, b, (uint32_t)(p - b), NULL, 0u);
}

static asx_time wrap_clock_now(void *ctx) {
    asx_time t = g_orig.now(ctx);
    note_clock(0u, (uint64_t)t);
    return t;
}

static asx_time wrap_clock_logical(void *ctx) {
    asx_time t = g_orig.logical(ctx);
    note_clock(1u, (uint64_t)t);
    return t;
}

static uint64_t wrap_entropy_random(void *ctx) {
    uint64_t v = g_orig.random(ctx);
    note_entropy(0u, (int32_t)ASX_OK, v);
    return v;
}

static asx_status wrap_entropy_try(void *ctx, uint64_t *out) {
    asx_status st = g_orig.try_random(ctx, out);
    note_entropy(1u, (int32_t)st, st == ASX_OK && out != NULL ? *out : 0u);
    return st;
}

static asx_status wrap_reactor_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    asx_status st = g_orig.wait(ctx, timeout_ms, ready_count);
    note_wait(0u, (uint64_t)timeout_ms, (int32_t)st,
              st == ASX_OK && ready_count != NULL ? *ready_count : 0u);
    return st;
}

static asx_status wrap_reactor_ghost_wait(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    asx_status st = g_orig.ghost_wait(ctx, logical_step, ready_count);
    note_wait(1u, logical_step, (int32_t)st,
              st == ASX_OK && ready_count != NULL ? *ready_count : 0u);
    return st;
}

static asx_status wrap_reactor_poll(void *ctx, uint32_t timeout_ms, asx_reactor_event *events,
                                    uint32_t max_events, uint32_t *out_count) {
    asx_status st = g_orig.poll(ctx, timeout_ms, events, max_events, out_count);
    asx_journal *j = g_active;
    if (j != NULL) {
        uint32_t n = st == ASX_OK && out_count != NULL ? *out_count : 0u;
        uint32_t keep = n;
        uint32_t fit = (j_max_payload(j) - 12u) / 12u;
        uint8_t flags = 0u;
        uint8_t *dst;
        uint32_t i;
        /* The events a record can hold are all stored; a poll beyond the
         * buffer keeps its first events and says so (a replay refuses it). */
        if (keep > fit) {
            keep = fit;
            flags = ASX_JOURNAL_REC_TRUNC;
        }
        dst = j_begin(j, (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL, flags, 1, 12u + keep * 12u);
        if (dst != NULL) {
            dst = put_u32(dst, timeout_ms);
            dst = put_u32(dst, (uint32_t)st);
            dst = put_u32(dst, n);
            for (i = 0; i < keep; i++) {
                ASX_CHECKPOINT_WAIVER("bounded: at most the events that fit the staging buffer");
                dst = put_u64(dst, events[i].token);
                dst = put_u32(dst, events[i].ready);
            }
            j_end(j, 12u + keep * 12u);
        }
    }
    return st;
}

asx_status asx_journal_wrap_hooks(asx_journal *j, asx_runtime_hooks *hooks) {
    asx_status st;
    if (j == NULL || hooks == NULL) return ASX_E_INVALID_ARGUMENT;
    if (hooks->clock.now_ns_fn == wrap_clock_now ||
        hooks->clock.logical_now_ns_fn == wrap_clock_logical ||
        hooks->entropy.random_u64_fn == wrap_entropy_random ||
        hooks->entropy.try_u64_fn == wrap_entropy_try ||
        hooks->reactor.wait_fn == wrap_reactor_wait ||
        hooks->reactor.ghost_wait_fn == wrap_reactor_ghost_wait ||
        hooks->reactor.poll_fn == wrap_reactor_poll) {
        return ASX_E_ALREADY_EXISTS;
    }
    st = asx_journal_install(j);
    if (st != ASX_OK) return st;
    memset(&g_orig, 0, sizeof(g_orig));
    if (hooks->clock.now_ns_fn != NULL) {
        g_orig.now = hooks->clock.now_ns_fn;
        hooks->clock.now_ns_fn = wrap_clock_now;
    }
    if (hooks->clock.logical_now_ns_fn != NULL) {
        g_orig.logical = hooks->clock.logical_now_ns_fn;
        hooks->clock.logical_now_ns_fn = wrap_clock_logical;
    }
    if (hooks->entropy.random_u64_fn != NULL) {
        g_orig.random = hooks->entropy.random_u64_fn;
        hooks->entropy.random_u64_fn = wrap_entropy_random;
    }
    if (hooks->entropy.try_u64_fn != NULL) {
        g_orig.try_random = hooks->entropy.try_u64_fn;
        hooks->entropy.try_u64_fn = wrap_entropy_try;
    }
    if (hooks->reactor.wait_fn != NULL) {
        g_orig.wait = hooks->reactor.wait_fn;
        hooks->reactor.wait_fn = wrap_reactor_wait;
    }
    if (hooks->reactor.ghost_wait_fn != NULL) {
        g_orig.ghost_wait = hooks->reactor.ghost_wait_fn;
        hooks->reactor.ghost_wait_fn = wrap_reactor_ghost_wait;
    }
    if (hooks->reactor.poll_fn != NULL) {
        g_orig.poll = hooks->reactor.poll_fn;
        hooks->reactor.poll_fn = wrap_reactor_poll;
    }
    return ASX_OK;
}
