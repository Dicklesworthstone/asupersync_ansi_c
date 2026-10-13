/*
 * test_journal.c — the native boundary journal (bd-9kll.11.1, RP1)
 *
 * Portable tests: the stream format, every record kind, the chain and its
 * tamper detection, SHAPE mode, sink failure, batching, the hook-table
 * wrapper (over fake hooks), the ring sink. On a live POSIX build one more
 * test records a small real run (clock, entropy, reactor readiness on a
 * pipe, a TCP round trip, a signal, a child's exit, a blocking job) and
 * reads the stream back.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ASX_PROFILE_POSIX)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>   /* nanosleep */
#include <unistd.h> /* pipe, write, close */
#endif

#include "../../test_harness.h"
#include <asx/asx.h>
#if defined(ASX_PROFILE_POSIX)
#include <asx/platform/posix.h>
#endif
#include <asx/runtime/journal.h>
#include <asx/runtime/trace.h>
#include <asx/runtime/virtual_time.h>
#include <asx/time/sleep.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* An in-memory sink                                                   */
/* ------------------------------------------------------------------ */

#define OUT_CAP (256u * 1024u)

static uint8_t g_out[OUT_CAP];
static size_t g_out_len;
static uint32_t g_sink_calls;
static uint32_t g_fail_on_call; /* 1-based sink call that fails; 0 = never */
static uint8_t g_stage[64u * 1024u];

static asx_status mem_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    (void)ctx;
    g_sink_calls++;
    if (g_fail_on_call != 0u && g_sink_calls >= g_fail_on_call) return ASX_E_RESOURCE_EXHAUSTED;
    if (g_out_len + len > OUT_CAP) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(g_out + g_out_len, bytes, len);
    g_out_len += len;
    return ASX_OK;
}

static void sink_reset(void) {
    asx_journal_uninstall();
    g_out_len = 0u;
    g_sink_calls = 0u;
    g_fail_on_call = 0u;
}

static asx_status open_journal(asx_journal *j, uint32_t buffer_cap, uint32_t flags,
                               uint32_t chain_every) {
    asx_journal_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = mem_sink;
    cfg.buffer = g_stage;
    cfg.buffer_cap = buffer_cap;
    cfg.flags = flags;
    cfg.chain_every = chain_every;
    cfg.config_fingerprint = UINT64_C(0x1122334455667788);
    cfg.rust_rev_hash = asx_journal_text_hash("5e60b1c4c");
    cfg.build_hash = asx_journal_text_hash("02cdd96");
    return asx_journal_init(j, &cfg);
}

/* The nth data record of `kind` in the stream, or 0. */
static int find_record(const uint8_t *bytes, size_t len, uint8_t kind, uint32_t nth,
                       asx_journal_record *out) {
    asx_journal_reader r;
    asx_journal_read_result rr = asx_journal_reader_init(&r, bytes, len);
    if (rr != ASX_JOURNAL_READ_RECORD) return 0;
    for (;;) {
        rr = asx_journal_reader_next(&r, out);
        if (rr != ASX_JOURNAL_READ_RECORD) return 0;
        if (out->kind == kind) {
            if (nth == 0u) return 1;
            nth--;
        }
    }
}

/* One encoded address in a payload: family, 16 bytes, port. */
#define JADDR_LEN_FOR_TEST 19u

static uint64_t fnv1a64_ref(const uint8_t *data, uint32_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)data[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Format                                                              */
/* ------------------------------------------------------------------ */

TEST(init_rejects_a_missing_sink_buffer_or_a_small_buffer) {
    asx_journal j;
    asx_journal_config cfg;
    sink_reset();
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(asx_journal_init(&j, &cfg), ASX_E_INVALID_ARGUMENT);
    cfg.sink_fn = mem_sink;
    ASSERT_EQ(asx_journal_init(&j, &cfg), ASX_E_INVALID_ARGUMENT);
    cfg.buffer = g_stage;
    cfg.buffer_cap = ASX_JOURNAL_MIN_BUFFER - 1u;
    ASSERT_EQ(asx_journal_init(&j, &cfg), ASX_E_BUFFER_TOO_SMALL);
    cfg.buffer_cap = ASX_JOURNAL_MIN_BUFFER;
    ASSERT_EQ(asx_journal_init(&j, &cfg), ASX_OK);
    ASSERT_EQ(asx_journal_init(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(an_empty_stream_is_a_header_and_an_end) {
    asx_journal j;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_stats st;

    sink_reset();
    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ(g_out_len, (size_t)(ASX_JOURNAL_HEADER_BYTES + ASX_JOURNAL_RECORD_HEAD_BYTES +
                                  ASX_JOURNAL_END_PAYLOAD_BYTES));
    ASSERT_EQ(memcmp(g_out, "ASXJRNL", 7), 0);
    ASSERT_EQ(g_out[7], 0u);
    ASSERT_EQ(asx_journal_get_u32(g_out + 8), ASX_JOURNAL_VERSION);
    ASSERT_EQ(asx_journal_get_u64(g_out + 16), UINT64_C(0x1122334455667788));

    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
    ASSERT_EQ(r.config_fingerprint, UINT64_C(0x1122334455667788));
    /* The build identity a replay compares before it trusts the stream. */
    ASSERT_EQ(r.rust_rev_hash, asx_journal_text_hash("5e60b1c4c"));
    ASSERT_EQ(r.build_hash, asx_journal_text_hash("02cdd96"));
    ASSERT_TRUE(r.rust_rev_hash != 0u);
    ASSERT_TRUE(r.build_hash != r.rust_rev_hash);
    ASSERT_EQ(asx_journal_text_hash(NULL), (uint64_t)0u);
    ASSERT_EQ(asx_journal_get_u64(g_out + 24), r.rust_rev_hash);
    ASSERT_EQ(asx_journal_get_u64(g_out + 32), r.build_hash);
    ASSERT_EQ((int)asx_journal_reader_next(&r, &rec), (int)ASX_JOURNAL_READ_END);
    ASSERT_EQ(r.data_records, (uint64_t)0u);
    asx_journal_get_stats(&j, &st);
    ASSERT_EQ(st.records, (uint64_t)0u);
    ASSERT_EQ(st.bytes, (uint64_t)g_out_len);
}

TEST(a_bad_header_is_refused) {
    asx_journal j;
    asx_journal_reader r;
    uint8_t copy[256];

    sink_reset();
    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_TRUE(g_out_len <= sizeof(copy));
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, 20u), (int)ASX_JOURNAL_READ_TRUNCATED);
    memcpy(copy, g_out, g_out_len);
    copy[0] = 'X';
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, g_out_len), (int)ASX_JOURNAL_READ_BAD_HEADER);
    memcpy(copy, g_out, g_out_len);
    copy[8] = (uint8_t)(ASX_JOURNAL_VERSION + 1u); /* a version this reader does not know */
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, g_out_len), (int)ASX_JOURNAL_READ_BAD_HEADER);
    memcpy(copy, g_out, g_out_len);
    copy[44] = 1u; /* reserved bytes are zero */
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, g_out_len), (int)ASX_JOURNAL_READ_BAD_HEADER);
}

/* ------------------------------------------------------------------ */
/* Every kind                                                          */
/* ------------------------------------------------------------------ */

TEST(every_native_boundary_kind_round_trips) {
    asx_journal j;
    asx_journal_record rec;
    asx_journal_reader r;
    asx_journal_addr peer;
    asx_journal_addr addrs[2];
    uint32_t counts[4] = {0u, 2u, 0u, 1u};
    uint32_t kinds_seen = 0u;
    uint64_t last_seq = 0u;
    asx_journal_read_result rr;
    const uint64_t stream = UINT64_C(0x8000000100000002);

    sink_reset();
    memset(&peer, 0, sizeof(peer));
    peer.family = 0u;
    peer.bytes[0] = 127u;
    peer.bytes[3] = 1u;
    peer.port = 8080u;
    addrs[0] = peer;
    addrs[1] = peer;
    addrs[1].port = 9090u;

    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    ASSERT_TRUE(asx_journal_active());
    asx_journal_note_net_listen(&peer, (int32_t)ASX_OK, 7u, &addrs[1]);
    asx_journal_note_udp_bind(&peer, (int32_t)ASX_E_ALREADY_EXISTS, 0u, &peer);
    asx_journal_note_net_ctl((uint8_t)ASX_JOURNAL_NET_CTL_NODELAY, stream, (int32_t)ASX_OK);
    asx_journal_note_net_accept(7u, (int32_t)ASX_OK, stream, &peer, &addrs[1]);
    asx_journal_note_net_connect(&peer, (int32_t)ASX_E_PENDING, 0u, &addrs[1]);
    asx_journal_note_net_read(stream, 64u, (int32_t)ASX_OK, (const uint8_t *)"hello", 5u);
    asx_journal_note_net_write(stream, 10u, (int32_t)ASX_OK, 7u);
    asx_journal_note_udp_recv(stream, 128u, (int32_t)ASX_OK, (const uint8_t *)"dgram", 5u, &peer);
    asx_journal_note_udp_send(stream, 5u, (int32_t)ASX_OK, 5u);
    asx_journal_note_resolve(asx_journal_text_hash("example.test"), (int32_t)ASX_OK, addrs, 2u);
    asx_journal_note_resolve_job(asx_journal_text_hash("job.test"), (int32_t)ASX_E_NOT_FOUND, NULL,
                                 0u);
    asx_journal_note_process_spawn((int32_t)ASX_OK, stream, 4242u, UINT64_C(0xC0FFEE),
                                   (uint8_t)ASX_JOURNAL_SPAWN_PIDFD);
    asx_journal_note_process_write(stream, 6u, (int32_t)ASX_OK, 6u);
    asx_journal_note_process_read(stream, 1u, 32u, (int32_t)ASX_OK, (const uint8_t *)"out", 3u);
    asx_journal_note_process_exit(stream, 1u, 7, 0);
    asx_journal_note_signal(counts, 4u);
    asx_journal_note_blocking_done(3u, 9u, UINT64_C(0xABCDEF));
    asx_journal_uninstall();
    ASSERT_FALSE(asx_journal_active());
    asx_journal_note_net_write(stream, 1u, (int32_t)ASX_OK, 1u); /* no journal: ignored */
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
        ASSERT_TRUE(rec.seq >= last_seq);
        last_seq = rec.seq;
        kinds_seen++;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
    ASSERT_EQ(kinds_seen, 17u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_LISTEN, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 19u + 4u + 8u + 19u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 17), 8080u); /* what was asked */
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 19), (uint32_t)ASX_OK);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 23), (uint64_t)7u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 31 + 17), 9090u); /* what it got */

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_UDP_BIND, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 19), (uint32_t)ASX_E_ALREADY_EXISTS);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_CTL, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 13u);
    ASSERT_EQ(rec.payload[0], (uint8_t)ASX_JOURNAL_NET_CTL_NODELAY);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 1), stream);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 9), (uint32_t)ASX_OK);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_ACCEPT, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 58u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload), (uint64_t)7u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), (uint32_t)ASX_OK);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 12), stream);
    ASSERT_EQ(rec.payload[20], 0u);
    ASSERT_EQ(rec.payload[21], 127u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 37), 8080u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 39 + 17), 9090u); /* the local address */

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_CONNECT, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 19u + 4u + 8u + 19u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 19), (uint32_t)ASX_E_PENDING);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 31 + 17), 9090u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_READ, 0u, &rec));
    ASSERT_EQ(rec.flags & ASX_JOURNAL_REC_SHAPE, 0u);
    ASSERT_EQ(rec.payload_len, 25u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), 64u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 16), 5u);
    ASSERT_EQ(memcmp(rec.payload + 20, "hello", 5u), 0);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_WRITE, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), 10u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 16), 7u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_UDP_RECV, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 16), 5u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 20 + 17), 8080u);
    ASSERT_EQ(memcmp(rec.payload + 20 + JADDR_LEN_FOR_TEST, "dgram", 5u), 0);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_UDP_SEND, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 16), 5u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_RESOLVE, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u64(rec.payload), asx_journal_text_hash("example.test"));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 12), 2u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 16 + 17), 8080u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 16 + JADDR_LEN_FOR_TEST + 17), 9090u);

    /* A resolution made by a pool job carries the off-thread flag and no
     * trace position. */
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_RESOLVE, 1u, &rec));
    ASSERT_TRUE((rec.flags & ASX_JOURNAL_REC_OFFTHREAD) != 0u);
    ASSERT_EQ(rec.trace_seq, (uint64_t)0u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload), asx_journal_text_hash("job.test"));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), (uint32_t)ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 12), 0u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_PROCESS_SPAWN, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 4u + 8u + 4u + 8u + 1u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 4), stream);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 12), 4242u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 16), UINT64_C(0xC0FFEE));
    ASSERT_EQ(rec.payload[24], (uint8_t)ASX_JOURNAL_SPAWN_PIDFD);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_PROCESS_WRITE, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u64(rec.payload), stream);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), 6u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 16), 6u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_PROCESS_READ, 0u, &rec));
    ASSERT_EQ(rec.flags & ASX_JOURNAL_REC_SHAPE, 0u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload), stream);
    ASSERT_EQ(rec.payload[8], 1u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 9), 32u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 17), 3u);
    ASSERT_EQ(memcmp(rec.payload + 21, "out", 3u), 0);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_PROCESS_EXIT, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 12), 7u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_SIGNAL, 0u, &rec));
    ASSERT_EQ(rec.payload[0], 2u); /* only the kinds with a count */
    ASSERT_EQ(rec.payload[1], 1u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 2), 2u);
    ASSERT_EQ(rec.payload[6], 3u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 7), 1u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_BLOCKING_DONE, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload), 3u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 4), 9u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 6), UINT64_C(0xABCDEF));
    ASSERT_TRUE((rec.flags & ASX_JOURNAL_REC_OFFTHREAD) != 0u);
    ASSERT_EQ(rec.trace_seq, (uint64_t)0u);
}

/* ------------------------------------------------------------------ */
/* Chain                                                               */
/* ------------------------------------------------------------------ */

static void note_many(uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) { asx_journal_note_blocking_done(i, 1u, (uint64_t)i * 3u); }
}

TEST(the_chain_verifies_a_complete_stream_and_catches_edits) {
    asx_journal j;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_read_result rr;
    size_t first_data = ASX_JOURNAL_HEADER_BYTES;
    size_t rec_len = ASX_JOURNAL_RECORD_HEAD_BYTES + 14u;
    size_t whole;
    static uint8_t copy[OUT_CAP];

    sink_reset();
    ASSERT_EQ(open_journal(&j, 4096u, 0u, 8u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(100u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    whole = g_out_len;

    /* 100 records, a chain per 8 and one for the remaining 4. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, whole), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
    ASSERT_EQ(r.data_records, (uint64_t)100u);
    ASSERT_EQ(r.chains, (uint64_t)13u);

    /* A payload byte changed in the first window: the first chain fails. */
    memcpy(copy, g_out, whole);
    copy[first_data + ASX_JOURNAL_RECORD_HEAD_BYTES + 3u] ^= 0x55u;
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, whole), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_CORRUPT);

    /* A record removed: its successor's sequence number is not the next. */
    memcpy(copy, g_out, whole);
    memmove(copy + first_data + rec_len * 2u, copy + first_data + rec_len * 3u,
            whole - (first_data + rec_len * 3u));
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, whole - rec_len),
              (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_CORRUPT);

    /* Cut inside the END record, and cut before it: neither is complete. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, whole - 10u), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_TRUNCATED);
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out,
                                           whole - (ASX_JOURNAL_RECORD_HEAD_BYTES +
                                                    ASX_JOURNAL_END_PAYLOAD_BYTES)),
              (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_TRUNCATED);

    /* Bytes after END are not part of the stream. */
    memcpy(copy, g_out, whole);
    copy[whole] = 0u;
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, whole + 1u), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_CORRUPT);
}

/* ------------------------------------------------------------------ */
/* Data: FULL, SHAPE, too large                                        */
/* ------------------------------------------------------------------ */

TEST(shape_mode_and_oversized_reads_store_length_and_hash) {
    asx_journal j;
    asx_journal_record rec;
    static uint8_t big[20000];
    uint8_t small[100];
    uint32_t i;

    for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7u);
    for (i = 0; i < sizeof(small); i++) small[i] = (uint8_t)(i + 1u);

    /* FULL mode: a small read carries its bytes, a 20 000-byte one its hash. */
    sink_reset();
    ASSERT_EQ(open_journal(&j, (uint32_t)sizeof(g_stage), 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_net_read(1u, 4096u, (int32_t)ASX_OK, small, (uint32_t)sizeof(small));
    asx_journal_note_net_read(1u, 32768u, (int32_t)ASX_OK, big, (uint32_t)sizeof(big));
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_READ, 0u, &rec));
    ASSERT_EQ(rec.flags & ASX_JOURNAL_REC_SHAPE, 0u);
    ASSERT_EQ(rec.payload_len, 20u + (uint32_t)sizeof(small));
    ASSERT_EQ(memcmp(rec.payload + 20, small, sizeof(small)), 0);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_READ, 1u, &rec));
    ASSERT_TRUE((rec.flags & ASX_JOURNAL_REC_SHAPE) != 0u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 16), (uint32_t)sizeof(big));
    ASSERT_EQ(rec.payload_len, 28u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 20), fnv1a64_ref(big, (uint32_t)sizeof(big)));

    /* SHAPE mode: even a small read keeps only its length and hash. */
    sink_reset();
    ASSERT_EQ(open_journal(&j, 4096u, ASX_JOURNAL_FLAG_SHAPE, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_net_read(1u, 4096u, (int32_t)ASX_OK, small, (uint32_t)sizeof(small));
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_READ, 0u, &rec));
    ASSERT_TRUE((rec.flags & ASX_JOURNAL_REC_SHAPE) != 0u);
    ASSERT_EQ(rec.payload_len, 28u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 20), fnv1a64_ref(small, (uint32_t)sizeof(small)));
    ASSERT_EQ(asx_journal_get_u32(g_out + 12), ASX_JOURNAL_FLAG_SHAPE);
}

/* ------------------------------------------------------------------ */
/* Fake hooks (the wrapper tests below use them)                       */
/* ------------------------------------------------------------------ */

static uint64_t g_fake_clock;
static uint32_t g_fake_poll_n = 2u;

static asx_time fake_now(void *ctx) {
    (void)ctx;
    g_fake_clock += 5u;
    return (asx_time)g_fake_clock;
}

static uint64_t fake_random(void *ctx) {
    (void)ctx;
    return UINT64_C(0x1111222233334444);
}

static asx_status fake_try(void *ctx, uint64_t *out) {
    (void)ctx;
    *out = 99u;
    return ASX_OK;
}

static asx_status fake_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready) {
    (void)ctx;
    (void)timeout_ms;
    *ready = 3u;
    return ASX_OK;
}

static asx_status fake_ghost(void *ctx, uint64_t step, uint32_t *ready) {
    (void)ctx;
    (void)step;
    *ready = 1u;
    return ASX_OK;
}

/* Returns g_fake_poll_n events: token i, ready READABLE for even i and
 * ERROR for odd i. */
static asx_status fake_poll(void *ctx, uint32_t timeout_ms, asx_reactor_event *events,
                            uint32_t max_events, uint32_t *out_count) {
    uint32_t i;
    (void)ctx;
    (void)timeout_ms;
    if (max_events < g_fake_poll_n) return ASX_E_INVALID_ARGUMENT;
    for (i = 0; i < g_fake_poll_n; i++) {
        events[i].token = (uint64_t)i;
        events[i].ready = (i % 2u) == 0u ? ASX_REACTOR_READABLE : ASX_REACTOR_ERROR;
    }
    *out_count = g_fake_poll_n;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Failure policy and batching                                         */
/* ------------------------------------------------------------------ */

TEST(a_sink_failure_sticks_and_never_reaches_the_caller) {
    asx_journal j;
    asx_journal_stats st;
    uint32_t calls_at_failure;

    sink_reset();
    g_fail_on_call = 2u;
    ASSERT_EQ(open_journal(&j, ASX_JOURNAL_MIN_BUFFER, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(400u);
    ASSERT_EQ(asx_journal_status(&j), ASX_E_RESOURCE_EXHAUSTED);
    calls_at_failure = g_sink_calls;
    note_many(50u);
    asx_journal_uninstall();
    ASSERT_EQ(g_sink_calls, calls_at_failure); /* the sink is not asked again */
    asx_journal_get_stats(&j, &st);
    ASSERT_TRUE(st.dropped > 0u);
    ASSERT_TRUE(st.records + st.dropped >= 450u);
    ASSERT_EQ(asx_journal_finish(&j), ASX_E_RESOURCE_EXHAUSTED);
    /* What did reach the sink is not a complete stream. */
    {
        asx_journal_reader r;
        asx_journal_record rec;
        asx_journal_read_result rr;
        ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
        for (;;) {
            rr = asx_journal_reader_next(&r, &rec);
            if (rr != ASX_JOURNAL_READ_RECORD) break;
        }
        ASSERT_TRUE(rr != ASX_JOURNAL_READ_END);
    }
}

/* A poll that returns more events than the staging buffer can hold keeps
 * the first ones that fit and says so: the stream stays well formed and a
 * replay can refuse that record. */
TEST(a_poll_larger_than_the_buffer_is_stored_truncated_and_flagged) {
    asx_journal j;
    asx_runtime_hooks hooks;
    asx_journal_record rec;
    asx_reactor_event ev[64];
    uint32_t n = 0u;
    uint32_t i;

    sink_reset();
    g_fake_poll_n = 30u;
    memset(&hooks, 0, sizeof(hooks));
    hooks.reactor.poll_fn = fake_poll;
    ASSERT_EQ(open_journal(&j, ASX_JOURNAL_MIN_BUFFER, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    ASSERT_EQ(hooks.reactor.poll_fn(NULL, 10u, ev, 64u, &n), ASX_OK);
    ASSERT_EQ(n, 30u); /* the caller saw all of them */
    for (i = 0; i < n; i++) { ASSERT_EQ(ev[i].token, (uint64_t)i); }
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_REACTOR_POLL, 0u, &rec));
    ASSERT_TRUE((rec.flags & ASX_JOURNAL_REC_TRUNC) != 0u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), 30u); /* the real count */
    ASSERT_TRUE(rec.payload_len < 12u + 30u * 12u);
    ASSERT_EQ((rec.payload_len - 12u) % 12u, 0u);
    g_fake_poll_n = 2u;
}

TEST(records_are_batched_into_few_sink_calls) {
    asx_journal j;
    asx_journal_stats st;

    sink_reset();
    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(5000u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    asx_journal_get_stats(&j, &st);
    ASSERT_EQ(st.records, (uint64_t)5000u);
    ASSERT_EQ(st.dropped, (uint64_t)0u);
    /* Each sink call carries a buffer's worth: well under one call per 20
     * records, and the sink saw exactly the bytes the journal produced. */
    ASSERT_TRUE((uint64_t)g_sink_calls * 20u < st.records);
    ASSERT_EQ((uint64_t)g_out_len, st.bytes);
    ASSERT_EQ((uint64_t)g_sink_calls, (uint64_t)st.sink_calls);
}

/* ------------------------------------------------------------------ */
/* Install, ring                                                       */
/* ------------------------------------------------------------------ */

TEST(only_one_journal_is_active_at_a_time) {
    asx_journal a;
    asx_journal b;
    sink_reset();
    ASSERT_EQ(open_journal(&a, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(open_journal(&b, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&a), ASX_OK);
    ASSERT_EQ(asx_journal_install(&a), ASX_OK);
    ASSERT_EQ(asx_journal_install(&b), ASX_E_INVALID_STATE);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_install(&b), ASX_OK);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_install(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(the_ring_sink_keeps_the_most_recent_bytes) {
    asx_journal_ring ring;
    uint8_t storage[16];
    uint8_t first[10];
    uint8_t second[10];
    uint8_t got[16];
    uint32_t i;

    for (i = 0; i < 10u; i++) {
        first[i] = (uint8_t)(i + 1u);
        second[i] = (uint8_t)(i + 11u);
    }
    ASSERT_EQ(asx_journal_ring_init(&ring, storage, (uint32_t)sizeof(storage)), ASX_OK);
    ASSERT_EQ(asx_journal_ring_sink(&ring, first, 10u), ASX_OK);
    ASSERT_EQ(asx_journal_ring_read(&ring, got, (uint32_t)sizeof(got)), 10u);
    ASSERT_EQ(memcmp(got, first, 10u), 0);
    ASSERT_EQ(asx_journal_ring_sink(&ring, second, 10u), ASX_OK);
    ASSERT_EQ(ring.total, (uint64_t)20u);
    ASSERT_EQ(asx_journal_ring_read(&ring, got, (uint32_t)sizeof(got)), 16u);
    for (i = 0; i < 16u; i++) {
        ASSERT_EQ(got[i], (uint8_t)(i + 5u)); /* bytes 5..20 of the 20 offered */
    }
    /* A chunk larger than the ring keeps its tail. */
    {
        uint8_t big[40];
        for (i = 0; i < 40u; i++) big[i] = (uint8_t)(100u + i);
        ASSERT_EQ(asx_journal_ring_sink(&ring, big, 40u), ASX_OK);
        ASSERT_EQ(asx_journal_ring_read(&ring, got, (uint32_t)sizeof(got)), 16u);
        ASSERT_EQ(got[0], (uint8_t)124u);
        ASSERT_EQ(got[15], (uint8_t)139u);
    }
}

/* ------------------------------------------------------------------ */
/* The hook table wrapper, over fake hooks                             */
/* ------------------------------------------------------------------ */

TEST(wrapped_hooks_forward_unchanged_and_record_raw_results) {
    asx_journal j;
    asx_runtime_hooks hooks;
    asx_journal_record rec;
    asx_reactor_event ev[4];
    uint32_t ready = 0u;
    uint32_t n = 0u;
    uint64_t v = 0u;

    sink_reset();
    g_fake_clock = 0u;
    memset(&hooks, 0, sizeof(hooks));
    hooks.clock.now_ns_fn = fake_now;
    hooks.clock.logical_now_ns_fn = fake_now;
    hooks.entropy.random_u64_fn = fake_random;
    hooks.entropy.try_u64_fn = fake_try;
    hooks.reactor.wait_fn = fake_wait;
    hooks.reactor.ghost_wait_fn = fake_ghost;
    hooks.reactor.poll_fn = fake_poll;

    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    ASSERT_TRUE(asx_journal_active());
    ASSERT_TRUE(hooks.clock.now_ns_fn != fake_now);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_E_ALREADY_EXISTS);

    /* The wrapped table answers exactly as the originals do. */
    ASSERT_EQ(hooks.clock.now_ns_fn(NULL), (asx_time)5u);
    ASSERT_EQ(hooks.clock.logical_now_ns_fn(NULL), (asx_time)10u);
    ASSERT_EQ(hooks.entropy.random_u64_fn(NULL), UINT64_C(0x1111222233334444));
    ASSERT_EQ(hooks.entropy.try_u64_fn(NULL, &v), ASX_OK);
    ASSERT_EQ(v, (uint64_t)99u);
    ASSERT_EQ(hooks.reactor.wait_fn(NULL, 25u, &ready), ASX_OK);
    ASSERT_EQ(ready, 3u);
    ASSERT_EQ(hooks.reactor.ghost_wait_fn(NULL, 17u, &ready), ASX_OK);
    ASSERT_EQ(ready, 1u);
    g_fake_poll_n = 2u;
    ASSERT_EQ(hooks.reactor.poll_fn(NULL, 40u, ev, 4u, &n), ASX_OK);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(ev[1].token, (uint64_t)1u);

    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_CLOCK, 0u, &rec));
    ASSERT_EQ(rec.payload[0], 0u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 1), (uint64_t)5u);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_CLOCK, 1u, &rec));
    ASSERT_EQ(rec.payload[0], 1u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 1), (uint64_t)10u);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_ENTROPY, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 5), UINT64_C(0x1111222233334444));
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_ENTROPY, 1u, &rec));
    ASSERT_EQ(rec.payload[0], 1u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 5), (uint64_t)99u);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_REACTOR_WAIT, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 1), (uint64_t)25u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 13), 3u);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_REACTOR_WAIT, 1u, &rec));
    ASSERT_EQ(rec.payload[0], 1u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 1), (uint64_t)17u);
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_REACTOR_POLL, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 12u + 2u * 12u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload), 40u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), 2u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 12), (uint64_t)0u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 20), (uint32_t)ASX_REACTOR_READABLE);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 24), (uint64_t)1u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 32), (uint32_t)ASX_REACTOR_ERROR);
}

/* The journal wraps the clock hooks; a wrapper over the default (virtual)
 * clock is still a virtual clock, so the scheduler moves it to the next timer
 * (bd-lnrc: it did not, and a sleeping task of a journaled run never woke). A
 * wrapper over any other clock is not one. */
typedef struct {
    asx_sleep_state s;
    asx_status result;
} journaled_sleeper;

static asx_status poll_journaled_sleeper(void *user_data, asx_task_id self) {
    journaled_sleeper *t = (journaled_sleeper *)user_data;
    t->result = asx_sleep_poll(&t->s, self);
    return t->result == ASX_E_PENDING ? ASX_E_PENDING : ASX_OK;
}

TEST(a_journal_over_the_default_clock_keeps_it_virtual_and_a_sleep_completes) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    asx_runtime_config cfg;
    asx_journal j;
    asx_journal_record rec;
    asx_region_id r;
    asx_task_id id;
    asx_budget budget;
    journaled_sleeper t;

    sink_reset();
    memset(&rt, 0, sizeof(rt));
    asx_runtime_shutdown(&rt);
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_config_init(&cfg), ASX_OK);
    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_TRUE(asx_runtime_clock_is_virtual());

    memset(&t, 0, sizeof(t));
    ASSERT_EQ(asx_sleep_init(&t.s, 5000000ULL), ASX_OK);
    ASSERT_EQ(asx_region_open(&r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_journaled_sleeper, &t, &id), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(t.result, ASX_OK);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)5000000u);

    asx_runtime_shutdown(&rt);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    /* The reads still went through the wrapper: they are in the stream. */
    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_CLOCK, 0u, &rec));
}

TEST(a_journal_over_a_custom_clock_is_not_a_virtual_clock) {
    asx_runtime rt;
    asx_runtime_hooks hooks;
    asx_runtime_config cfg;
    asx_journal j;

    sink_reset();
    memset(&rt, 0, sizeof(rt));
    asx_runtime_shutdown(&rt);
    g_fake_clock = 0u;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.clock.now_ns_fn = fake_now;
    hooks.clock.logical_now_ns_fn = fake_now;
    ASSERT_EQ(asx_runtime_config_init(&cfg), ASX_OK);
    ASSERT_EQ(open_journal(&j, 4096u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_FALSE(asx_runtime_clock_is_virtual());

    asx_runtime_shutdown(&rt);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
}

/* ------------------------------------------------------------------ */
/* Commit points, trace checkpoints, the committed prefix (RP4)        */
/* ------------------------------------------------------------------ */

#define NOTE_REC_LEN (ASX_JOURNAL_RECORD_HEAD_BYTES + 14u) /* a BLOCKING_DONE record */
#define CTL_REC_LEN (ASX_JOURNAL_RECORD_HEAD_BYTES + 13u)  /* a NET_CTL record */
#define CHAIN_REC_LEN (ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_PAYLOAD_BYTES)
#define END_REC_LEN (ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_END_PAYLOAD_BYTES)

typedef struct {
    uint32_t calls;
    uint64_t chain_seq;
    uint64_t data_records;
    uint64_t stream_bytes;
    uint64_t trace_events;
    uint64_t trace_digest;
    int trace_valid;
    int final;
    size_t sink_len; /* what the sink held when commit_fn ran */
    asx_status fail_with;
} commit_log;

static commit_log g_commit;

static asx_status log_commit(void *ctx, const asx_journal_commit *c) {
    (void)ctx;
    g_commit.calls++;
    g_commit.chain_seq = c->chain_seq;
    g_commit.data_records = c->data_records;
    g_commit.stream_bytes = c->stream_bytes;
    g_commit.trace_events = c->trace_events;
    g_commit.trace_digest = c->trace_digest;
    g_commit.trace_valid = c->trace_valid;
    g_commit.final = c->final;
    g_commit.sink_len = g_out_len;
    return g_commit.fail_with;
}

static asx_status open_persist(asx_journal *j, uint32_t chain_every, uint32_t commit_every) {
    asx_journal_config cfg;
    memset(&g_commit, 0, sizeof(g_commit));
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = mem_sink;
    cfg.buffer = g_stage;
    cfg.buffer_cap = 4096u;
    cfg.chain_every = chain_every;
    cfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
    cfg.commit_every_chains = commit_every;
    cfg.commit_fn = log_commit;
    cfg.rust_rev_hash = asx_journal_text_hash("5e60b1c4c");
    cfg.build_hash = asx_journal_text_hash("02cdd96");
    return asx_journal_init(j, &cfg);
}

TEST(commit_points_hand_the_prefix_to_the_sink_before_commit_fn_runs) {
    asx_journal j;
    asx_journal_stats st;
    uint64_t at_commit;

    sink_reset();
    ASSERT_EQ(open_persist(&j, 4u, 2u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);

    /* A chain after 4 records is not a commit point (every second one is):
     * the sink has seen nothing. */
    note_many(7u);
    ASSERT_EQ(g_commit.calls, 0u);
    ASSERT_EQ(g_out_len, (size_t)0u);

    /* The 8th record closes the second chain: header, 8 records and 2
     * chains are in the sink, then commit_fn ran with their length. */
    note_many(1u);
    at_commit = (uint64_t)ASX_JOURNAL_HEADER_BYTES + 8u * NOTE_REC_LEN + 2u * CHAIN_REC_LEN;
    ASSERT_EQ(g_commit.calls, 1u);
    ASSERT_EQ(g_commit.chain_seq, (uint64_t)9u); /* data 0-3, chain 4, data 5-8, chain 9 */
    ASSERT_EQ(g_commit.data_records, (uint64_t)8u);
    ASSERT_EQ(g_commit.stream_bytes, at_commit);
    ASSERT_EQ((uint64_t)g_commit.sink_len, at_commit);
    ASSERT_EQ((uint64_t)g_out_len, at_commit);
    ASSERT_EQ(g_commit.final, 0);
    ASSERT_EQ(g_commit.trace_valid, 0); /* an off-thread record closed it */
    asx_journal_get_stats(&j, &st);
    ASSERT_EQ(st.commits, (uint64_t)1u);
    ASSERT_EQ(st.committed_bytes, at_commit);

    /* Finish writes the last chain and END, then commits once more. */
    note_many(3u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ(g_commit.calls, 2u);
    ASSERT_EQ(g_commit.final, 1);
    ASSERT_EQ(g_commit.trace_valid, 0);
    ASSERT_EQ(g_commit.stream_bytes, (uint64_t)g_out_len);
    ASSERT_EQ((uint64_t)g_commit.sink_len, (uint64_t)g_out_len);
    ASSERT_EQ(g_commit.chain_seq, (uint64_t)(8u + 2u + 3u + 1u)); /* the END seq */
}

TEST(a_buffered_journal_never_calls_commit_fn) {
    asx_journal j;
    asx_journal_config cfg;
    asx_journal_stats st;

    sink_reset();
    memset(&g_commit, 0, sizeof(g_commit));
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = mem_sink;
    cfg.buffer = g_stage;
    cfg.buffer_cap = 4096u;
    cfg.chain_every = 2u;
    cfg.commit_fn = log_commit; /* persist stays BUFFER: not used */
    ASSERT_EQ(asx_journal_init(&j, &cfg), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(10u);
    ASSERT_EQ(g_out_len, (size_t)0u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ(g_commit.calls, 0u);
    asx_journal_get_stats(&j, &st);
    ASSERT_EQ(st.commits, (uint64_t)0u);
}

TEST(a_failing_commit_sticks_and_marks_the_stream_incomplete) {
    asx_journal j;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_stats st;
    asx_journal_read_result rr;

    sink_reset();
    ASSERT_EQ(open_persist(&j, 4u, 1u), ASX_OK);
    g_commit.fail_with = ASX_E_RESOURCE_EXHAUSTED;
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(4u);
    ASSERT_EQ(asx_journal_status(&j), ASX_E_RESOURCE_EXHAUSTED);
    note_many(4u); /* never reach the caller; counted as dropped */
    asx_journal_get_stats(&j, &st);
    ASSERT_EQ(st.dropped, (uint64_t)4u);
    ASSERT_EQ(st.commits, (uint64_t)1u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_E_RESOURCE_EXHAUSTED);

    /* The first window is in the sink, intact: the reader bounds it. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ(r.chain_end_records, (uint64_t)4u);
}

TEST(a_sampled_window_carries_a_trace_checkpoint_the_chain_covers) {
    asx_journal j;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_read_result rr;
    size_t chain_off = (size_t)ASX_JOURNAL_HEADER_BYTES + 4u * CTL_REC_LEN;
    static uint8_t copy[OUT_CAP];
    uint32_t i;
    uint64_t digest;

    sink_reset();
    asx_trace_reset();
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 1u, 0u);
    asx_trace_emit(ASX_TRACE_SCHED_POLL, 2u, 0u);
    ASSERT_EQ(open_persist(&j, 4u, 1u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    for (i = 0; i < 4u; i++) asx_journal_note_net_ctl(0u, (uint64_t)i, (int32_t)ASX_OK);
    digest = asx_trace_digest();

    /* The window's last record was taken on the runtime thread: the commit
     * carries the trace position and digest of that moment. */
    ASSERT_EQ(g_commit.calls, 1u);
    ASSERT_EQ(g_commit.trace_valid, 1);
    ASSERT_EQ(g_commit.trace_events, (uint64_t)2u);
    ASSERT_EQ(g_commit.trace_digest, digest);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    ASSERT_EQ(g_out[chain_off + 4u], (uint8_t)ASX_JOURNAL_KIND_CHAIN);
    ASSERT_TRUE((g_out[chain_off + 5u] & ASX_JOURNAL_REC_TRACE_CHECKPOINT) != 0u);
    ASSERT_EQ(asx_journal_get_u64(g_out + chain_off + 16u), (uint64_t)2u); /* head trace_seq */
    ASSERT_EQ(asx_journal_get_u64(g_out + chain_off + ASX_JOURNAL_RECORD_HEAD_BYTES +
                                  ASX_JOURNAL_CHAIN_TRACE_OFFSET + 8u),
              digest);
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
    ASSERT_EQ(r.chain_trace_valid, 1);
    ASSERT_EQ(r.chain_trace_events, (uint64_t)2u);
    ASSERT_EQ(r.chain_trace_digest, digest);

    /* An edited checkpoint breaks the chain like an edited record. */
    memcpy(copy, g_out, g_out_len);
    copy[chain_off + ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_TRACE_OFFSET + 8u] ^= 0x01u;
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_CORRUPT);
    ASSERT_EQ(r.chain_end_pos, (size_t)ASX_JOURNAL_HEADER_BYTES);
}

TEST(the_reader_bounds_the_committed_prefix_of_a_cut_stream) {
    asx_journal j;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_read_result rr;
    size_t whole;
    size_t after_two = (size_t)ASX_JOURNAL_HEADER_BYTES + 16u * NOTE_REC_LEN + 2u * CHAIN_REC_LEN;
    size_t after_one = (size_t)ASX_JOURNAL_HEADER_BYTES + 8u * NOTE_REC_LEN + 1u * CHAIN_REC_LEN;
    static uint8_t copy[OUT_CAP];

    sink_reset();
    ASSERT_EQ(open_persist(&j, 8u, 1u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(20u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    whole = g_out_len;

    /* Cut inside the third window's first record: the committed prefix is
     * the first two windows, the cut is the third's first seq. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, after_two + 7u),
              (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_TRUNCATED);
    ASSERT_EQ(r.chain_end_pos, after_two);
    ASSERT_EQ(r.chain_end_records, (uint64_t)16u);
    ASSERT_EQ(r.chain_end_seq, (uint64_t)18u); /* 16 data records and 2 chains */

    /* Cut before the first chain: nothing is committed. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out,
                                           ASX_JOURNAL_HEADER_BYTES + 3u * NOTE_REC_LEN + 5u),
              (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_TRUNCATED);
    ASSERT_EQ(r.chain_end_pos, (size_t)ASX_JOURNAL_HEADER_BYTES);
    ASSERT_EQ(r.chain_end_records, (uint64_t)0u);
    ASSERT_EQ(r.chain_end_seq, (uint64_t)0u);

    /* A byte changed in the second window: the first window still stands. */
    memcpy(copy, g_out, whole);
    copy[after_one + 3u * NOTE_REC_LEN + ASX_JOURNAL_RECORD_HEAD_BYTES + 2u] ^= 0x40u;
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, whole), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_CORRUPT);
    ASSERT_EQ(r.chain_end_pos, after_one);
    ASSERT_EQ(r.chain_end_records, (uint64_t)8u);
    ASSERT_EQ(r.chain_end_seq, (uint64_t)9u);

    /* The whole stream: its last chain ends the committed prefix, END follows. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, whole), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
    ASSERT_EQ(r.chain_end_pos, whole - END_REC_LEN);
    ASSERT_EQ(r.chain_end_records, (uint64_t)20u);
}

/* --- the flash store: a RAM ring in front of a write-budgeted log --- */

static uint8_t g_flash[8192];
static uint64_t g_flash_next;
static uint32_t g_flash_writes;
static asx_status g_flash_fail;

static asx_status flash_write(void *ctx, uint64_t offset, const uint8_t *bytes, uint32_t len) {
    (void)ctx;
    if (g_flash_fail != ASX_OK) return g_flash_fail;
    /* An append-only log: every write continues the last. */
    if (offset != g_flash_next || offset + len > sizeof(g_flash)) return ASX_E_INVALID_STATE;
    memcpy(g_flash + offset, bytes, len);
    g_flash_next += len;
    g_flash_writes++;
    return ASX_OK;
}

static asx_status open_flash_journal(asx_journal *j, asx_journal_flash *f, uint8_t *ring,
                                     uint32_t ring_cap, uint64_t wear_budget, uint32_t min_flush,
                                     uint32_t chain_every) {
    asx_journal_config cfg;
    asx_status st;
    sink_reset();
    g_flash_next = 0u;
    g_flash_writes = 0u;
    g_flash_fail = ASX_OK;
    st = asx_journal_flash_init(f, ring, ring_cap, flash_write, NULL, (uint64_t)sizeof(g_flash),
                                wear_budget, min_flush);
    if (st != ASX_OK) return st;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = asx_journal_flash_sink;
    cfg.sink_ctx = f;
    cfg.buffer = g_stage;
    cfg.buffer_cap = 4096u;
    cfg.chain_every = chain_every;
    cfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
    cfg.commit_every_chains = 1u;
    cfg.commit_fn = asx_journal_flash_commit;
    return asx_journal_init(j, &cfg);
}

/* Read what flash holds: the chains that verify and the bytes they cover. */
static void flash_prefix(size_t *pos, uint64_t *records, uint64_t *chains) {
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_read_result rr;
    *pos = 0u;
    *records = 0u;
    *chains = 0u;
    if (asx_journal_reader_init(&r, g_flash, (size_t)g_flash_next) != ASX_JOURNAL_READ_RECORD) {
        return;
    }
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    *pos = r.chain_end_pos;
    *records = r.chain_end_records;
    *chains = r.chains;
}

TEST(the_flash_log_holds_only_the_committed_prefix_when_the_run_dies) {
    asx_journal j;
    asx_journal_flash f;
    static uint8_t ring[2048];
    size_t pos;
    uint64_t records;
    uint64_t chains;
    uint64_t want = (uint64_t)ASX_JOURNAL_HEADER_BYTES + 28u * NOTE_REC_LEN + 7u * CHAIN_REC_LEN;

    ASSERT_EQ(open_flash_journal(&j, &f, ring, (uint32_t)sizeof(ring), 0u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(30u); /* the process dies here: no finish, 2 records still staged */
    asx_journal_uninstall();

    ASSERT_EQ(g_flash_next, want);
    ASSERT_EQ(f.flushed, want);
    ASSERT_EQ(f.len, 0u); /* the ring holds only what flash does not */
    ASSERT_EQ(f.wear_exhausted, 0);
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(pos, (size_t)want);
    ASSERT_EQ(records, (uint64_t)28u);
    ASSERT_EQ(chains, (uint64_t)7u);
}

TEST(a_wrapped_ring_flushes_in_order) {
    asx_journal j;
    asx_journal_flash f;
    static uint8_t ring[300]; /* a window is 224 bytes: the second one wraps */
    size_t pos;
    uint64_t records;
    uint64_t chains;

    ASSERT_EQ(open_flash_journal(&j, &f, ring, (uint32_t)sizeof(ring), 0u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(16u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ(asx_journal_status(&j), ASX_OK);
    ASSERT_TRUE(g_flash_writes >= 5u); /* a wrap makes a flush two writes */
    ASSERT_EQ(f.flushed, g_flash_next);
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(records, (uint64_t)16u);
    ASSERT_EQ(chains, (uint64_t)4u);
}

TEST(the_wear_budget_keeps_flash_to_a_chain_aligned_prefix) {
    asx_journal j;
    asx_journal_flash f;
    static uint8_t ring[600];
    size_t pos;
    uint64_t records;
    uint64_t chains;
    /* Windows: 48 + 4*38 + 72 = 272 bytes, then 224 each. A budget of 700
     * takes two (496) and holds the third (720) back. */
    ASSERT_EQ(open_flash_journal(&j, &f, ring, (uint32_t)sizeof(ring), 700u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(12u);
    ASSERT_EQ(f.flushed, (uint64_t)496u);
    ASSERT_EQ(f.wear_exhausted, 1);
    ASSERT_EQ(asx_journal_status(&j), ASX_OK); /* recording goes on in RAM */
    ASSERT_EQ(f.len, 224u);

    /* Past the ring's room the sink fails, the journal marks the stream
     * incomplete, and flash still holds exactly what it had. */
    note_many(8u);
    ASSERT_EQ(asx_journal_status(&j), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(f.status, ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(f.flushed, (uint64_t)496u);
    asx_journal_uninstall();
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(pos, (size_t)496u);
    ASSERT_EQ(records, (uint64_t)8u);
    ASSERT_EQ(chains, (uint64_t)2u);
}

TEST(min_flush_coalesces_commits_and_the_final_commit_flushes_the_rest) {
    asx_journal j;
    asx_journal_flash f;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_read_result rr;
    static uint8_t ring[4096];

    ASSERT_EQ(open_flash_journal(&j, &f, ring, (uint32_t)sizeof(ring), 0u, 600u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(12u); /* three windows: 272, 496, then 720 >= 600 */
    ASSERT_EQ(g_flash_writes, 1u);
    ASSERT_EQ(f.flushed, (uint64_t)720u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK); /* END, final: flushed whatever the size */
    ASSERT_EQ(f.flushed, g_flash_next);
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_flash, (size_t)g_flash_next),
              (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
}

TEST(a_failed_flash_write_sticks_and_flash_keeps_the_earlier_prefix) {
    asx_journal j;
    asx_journal_flash f;
    static uint8_t ring[1024];
    size_t pos;
    uint64_t records;
    uint64_t chains;

    ASSERT_EQ(open_flash_journal(&j, &f, ring, (uint32_t)sizeof(ring), 0u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(8u);
    ASSERT_EQ(asx_journal_status(&j), ASX_OK);
    g_flash_fail = ASX_E_RESOURCE_EXHAUSTED;
    note_many(4u);
    ASSERT_EQ(asx_journal_status(&j), ASX_E_RESOURCE_EXHAUSTED);
    asx_journal_uninstall();
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(records, (uint64_t)8u);
    ASSERT_EQ(chains, (uint64_t)2u);
    ASSERT_EQ(asx_journal_finish(&j), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(the_flash_store_rejects_bad_arguments) {
    asx_journal_flash f;
    static uint8_t ring[64];
    asx_journal_commit c;
    ASSERT_EQ(asx_journal_flash_init(NULL, ring, 64u, flash_write, NULL, 0u, 0u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_init(&f, NULL, 64u, flash_write, NULL, 0u, 0u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_init(&f, ring, 0u, flash_write, NULL, 0u, 0u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_init(&f, ring, 64u, NULL, NULL, 0u, 0u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_init(&f, ring, 64u, flash_write, NULL, 0u, 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_flash_sink(NULL, ring, 4u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_sink(&f, NULL, 4u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_sink(&f, NULL, 0u), ASX_OK);
    memset(&c, 0, sizeof(c));
    c.stream_bytes = 10u; /* more than was offered */
    ASSERT_EQ(asx_journal_flash_commit(&f, &c), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_journal_flash_commit(NULL, &c), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_flash_commit(&f, NULL), ASX_E_INVALID_ARGUMENT);
}

/* --- the watchdog-aligned store (AUTOMOTIVE, bd-9kll.11.4): a RAM ring
 * written to a log only at watchdog checkpoints, at most once per period and
 * max_tick_bytes at a time, and only the committed prefix --- */

static asx_status open_wdog_journal(asx_journal *j, asx_journal_wdog *f, uint8_t *ring,
                                    uint32_t ring_cap, uint32_t max_tick_bytes, uint64_t period_ns,
                                    uint32_t chain_every) {
    asx_journal_config cfg;
    asx_status st;
    sink_reset();
    g_flash_next = 0u;
    g_flash_writes = 0u;
    g_flash_fail = ASX_OK;
    st = asx_journal_wdog_init(f, ring, ring_cap, flash_write, NULL, max_tick_bytes, period_ns);
    if (st != ASX_OK) return st;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = asx_journal_wdog_sink;
    cfg.sink_ctx = f;
    cfg.buffer = g_stage;
    cfg.buffer_cap = 4096u;
    cfg.chain_every = chain_every;
    cfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
    cfg.commit_every_chains = 1u;
    cfg.commit_fn = asx_journal_wdog_commit;
    return asx_journal_init(j, &cfg);
}

TEST(the_watchdog_store_writes_only_committed_bytes_at_checkpoints_in_bounded_writes) {
    asx_journal j;
    asx_journal_wdog f;
    static uint8_t ring[2048];
    size_t pos;
    uint64_t records;
    uint64_t chains;
    uint32_t i;
    uint64_t want = (uint64_t)ASX_JOURNAL_HEADER_BYTES + 28u * NOTE_REC_LEN + 7u * CHAIN_REC_LEN;

    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 100u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(30u); /* the process dies here: seven chains committed, 2 records staged */
    asx_journal_uninstall();

    /* Between checkpoints the log is not touched: the committed prefix sits
     * in the ring (the staged records never reached it). */
    ASSERT_EQ(g_flash_writes, 0u);
    ASSERT_EQ(g_flash_next, (uint64_t)0u);
    ASSERT_EQ(f.committed, want);
    ASSERT_EQ(f.offered, want);
    ASSERT_EQ(f.len, (uint32_t)want);

    /* Each checkpoint writes at most max_tick_bytes of it. */
    for (i = 0u; i < 17u; i++) {
        ASSERT_EQ(asx_journal_wdog_tick(&f, (uint64_t)i * 1000u), ASX_OK);
        ASSERT_TRUE(f.largest_write <= 100u);
        ASSERT_TRUE(f.flushed <= f.committed);
    }
    ASSERT_EQ(f.flushed, want);
    ASSERT_EQ(f.flushes, (uint64_t)17u);
    ASSERT_EQ(f.log_writes, (uint64_t)17u);
    ASSERT_EQ(f.largest_write, 100u);
    ASSERT_EQ(f.len, 0u);

    /* With nothing pending a checkpoint writes nothing. */
    ASSERT_EQ(asx_journal_wdog_tick(&f, 99000u), ASX_OK);
    ASSERT_EQ(f.flushes, (uint64_t)17u);
    ASSERT_EQ(f.ticks, (uint64_t)18u);

    ASSERT_EQ(g_flash_next, want);
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(pos, (size_t)want);
    ASSERT_EQ(records, (uint64_t)28u);
    ASSERT_EQ(chains, (uint64_t)7u);
}

TEST(a_watchdog_flush_is_at_most_once_per_period_and_a_stepped_back_clock_adds_no_time) {
    asx_journal j;
    asx_journal_wdog f;
    static uint8_t ring[2048];

    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 50u, 1000u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(12u);
    asx_journal_uninstall();

    ASSERT_EQ(asx_journal_wdog_tick(&f, 0u), ASX_OK); /* the first one writes */
    ASSERT_EQ(f.flushes, (uint64_t)1u);
    ASSERT_EQ(f.flushed, (uint64_t)50u);
    ASSERT_EQ(asx_journal_wdog_tick(&f, 500u), ASX_OK);
    ASSERT_EQ(asx_journal_wdog_tick(&f, 999u), ASX_OK);
    ASSERT_EQ(f.flushes, (uint64_t)1u);
    ASSERT_EQ(asx_journal_wdog_tick(&f, 1000u), ASX_OK);
    ASSERT_EQ(f.flushes, (uint64_t)2u);
    ASSERT_EQ(f.flushed, (uint64_t)100u);
    ASSERT_EQ(asx_journal_wdog_tick(&f, 900u), ASX_OK); /* the clock stepped back */
    ASSERT_EQ(f.flushes, (uint64_t)2u);
    ASSERT_EQ(asx_journal_wdog_tick(&f, 2000u), ASX_OK);
    ASSERT_EQ(f.flushes, (uint64_t)3u);
    ASSERT_EQ(f.flushed, (uint64_t)150u);
    ASSERT_EQ(f.ticks, (uint64_t)6u);
    ASSERT_EQ(f.largest_write, 50u);
}

TEST(the_final_commit_drains_the_watchdog_backlog_in_bounded_writes_to_a_complete_stream) {
    asx_journal j;
    asx_journal_wdog f;
    asx_journal_reader r;
    asx_journal_record rec;
    asx_journal_read_result rr;
    static uint8_t ring[4096];

    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 100u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(12u); /* three windows committed, no checkpoint yet */
    asx_journal_uninstall();
    ASSERT_EQ(f.flushed, (uint64_t)0u);

    ASSERT_EQ(asx_journal_finish(&j), ASX_OK); /* END is the final commit */
    ASSERT_EQ(asx_journal_status(&j), ASX_OK);
    ASSERT_TRUE(g_flash_next > 720u);
    ASSERT_EQ(f.flushed, g_flash_next);
    ASSERT_EQ(f.len, 0u);
    ASSERT_TRUE(f.largest_write <= 100u);
    ASSERT_EQ(f.log_writes, (g_flash_next + 99u) / 100u);
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_flash, (size_t)g_flash_next),
              (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
}

TEST(a_wrapped_watchdog_ring_flushes_in_order) {
    asx_journal j;
    asx_journal_wdog f;
    static uint8_t ring[300]; /* a window is 224 bytes: the second one wraps */
    size_t pos;
    uint64_t records;
    uint64_t chains;
    uint32_t w;
    uint32_t k;

    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 64u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    for (w = 0u; w < 4u; w++) {
        note_many(4u);
        for (k = 0u; k < 5u; k++) {
            ASSERT_EQ(asx_journal_wdog_tick(&f, (uint64_t)(w * 5u + k)), ASX_OK);
        }
    }
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ(asx_journal_status(&j), ASX_OK);
    ASSERT_EQ(f.flushed, g_flash_next);
    ASSERT_TRUE(f.largest_write <= 64u);
    ASSERT_TRUE(f.log_writes > (g_flash_next + 63u) / 64u); /* a wrap makes a flush two writes */
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(records, (uint64_t)16u);
    ASSERT_EQ(chains, (uint64_t)4u);
}

TEST(a_producer_that_outruns_the_bound_fails_the_stream_and_the_log_keeps_its_prefix) {
    asx_journal j;
    asx_journal_wdog f;
    static uint8_t ring[600];
    size_t pos;
    uint64_t records;
    uint64_t chains;
    uint32_t i;

    /* Windows: 272, then 224 each. The ring takes two (496) and has 104
     * left for the third. */
    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 64u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(12u);
    ASSERT_EQ(asx_journal_status(&j), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(f.status, ASX_E_RESOURCE_EXHAUSTED);
    asx_journal_uninstall();

    /* The checkpoints still write what was committed. */
    for (i = 0u; i < 8u; i++) ASSERT_EQ(asx_journal_wdog_tick(&f, (uint64_t)i), ASX_OK);
    ASSERT_EQ(f.flushed, (uint64_t)496u);
    flash_prefix(&pos, &records, &chains);
    ASSERT_EQ(pos, (size_t)496u);
    ASSERT_EQ(records, (uint64_t)8u);
    ASSERT_EQ(chains, (uint64_t)2u);
}

TEST(a_failed_log_write_is_tried_again_at_the_next_checkpoint) {
    asx_journal j;
    asx_journal_wdog f;
    static uint8_t ring[1024];

    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 50u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(8u);
    asx_journal_uninstall();

    g_flash_fail = ASX_E_RESOURCE_EXHAUSTED;
    ASSERT_EQ(asx_journal_wdog_tick(&f, 0u), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(f.status, ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(f.flushed, (uint64_t)0u);
    ASSERT_EQ(f.log_writes, (uint64_t)1u);
    g_flash_fail = ASX_OK;
    ASSERT_EQ(asx_journal_wdog_tick(&f, 1u), ASX_OK);
    ASSERT_EQ(f.flushed, (uint64_t)50u);
    ASSERT_EQ(f.status, ASX_E_RESOURCE_EXHAUSTED); /* the first failure is kept */
}

TEST(a_watchdog_checkpoint_services_the_automotive_watchdog_then_flushes) {
    asx_journal j;
    asx_journal_wdog f;
    static uint8_t ring[2048];
    const asx_auto_watchdog *wd;

    asx_auto_instrument_reset();
    wd = asx_auto_watchdog_global();
    ASSERT_EQ(open_wdog_journal(&j, &f, ring, (uint32_t)sizeof(ring), 1000u, 0u, 4u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    note_many(8u);
    asx_journal_uninstall();

    ASSERT_EQ(asx_journal_wdog_checkpoint(&f, 1000000u, 7u), ASX_OK);
    ASSERT_EQ(wd->total_checkpoints, 1u);
    ASSERT_EQ(wd->violations, 0u);
    ASSERT_EQ(f.flushes, (uint64_t)1u);
    ASSERT_EQ(f.flushed, f.committed);

    /* The next one comes 29 ms later: the watchdog's own violation, and the
     * store has nothing left to write. */
    ASSERT_EQ(asx_journal_wdog_checkpoint(&f, 30000000u, 7u), ASX_OK);
    ASSERT_EQ(wd->total_checkpoints, 2u);
    ASSERT_EQ(wd->violations, 1u);
    ASSERT_EQ(f.ticks, (uint64_t)2u);
    ASSERT_EQ(f.flushes, (uint64_t)1u);
    asx_auto_instrument_reset();
}

TEST(the_watchdog_store_rejects_bad_arguments) {
    asx_journal_wdog f;
    static uint8_t ring[64];
    asx_journal_commit c;
    ASSERT_EQ(asx_journal_wdog_init(NULL, ring, 64u, flash_write, NULL, 10u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_init(&f, NULL, 64u, flash_write, NULL, 10u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_init(&f, ring, 0u, flash_write, NULL, 10u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_init(&f, ring, 64u, NULL, NULL, 10u, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_init(&f, ring, 64u, flash_write, NULL, 0u, 0u),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_init(&f, ring, 64u, flash_write, NULL, 10u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_wdog_sink(NULL, ring, 4u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_sink(&f, NULL, 4u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_sink(&f, NULL, 0u), ASX_OK);
    memset(&c, 0, sizeof(c));
    c.stream_bytes = 10u; /* more than was offered */
    ASSERT_EQ(asx_journal_wdog_commit(&f, &c), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_journal_wdog_commit(NULL, &c), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_commit(&f, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_tick(NULL, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_journal_wdog_checkpoint(NULL, 0u, 0u), ASX_E_INVALID_ARGUMENT);
}

TEST(each_profile_names_its_persistence_store) {
#if defined(ASX_PROFILE_POSIX)
    ASSERT_EQ((int)asx_journal_profile_store(), (int)ASX_JOURNAL_STORE_FILE);
#elif defined(ASX_PROFILE_EMBEDDED_ROUTER)
    ASSERT_EQ((int)asx_journal_profile_store(), (int)ASX_JOURNAL_STORE_FLASH);
#elif defined(ASX_PROFILE_AUTOMOTIVE)
    ASSERT_EQ((int)asx_journal_profile_store(), (int)ASX_JOURNAL_STORE_WATCHDOG);
#else
    ASSERT_EQ((int)asx_journal_profile_store(), (int)ASX_JOURNAL_STORE_CALLER);
#endif
}

#if defined(ASX_PROFILE_POSIX)
TEST(the_posix_file_store_writes_every_byte_and_maps_errors) {
    asx_posix_journal_file f;
    asx_journal_commit c;
    int fds[2];
    uint8_t got[8];

    memset(&f, 0, sizeof(f));
    memset(&c, 0, sizeof(c));
    ASSERT_EQ(asx_posix_journal_file_open(NULL, "/tmp/x"), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_posix_journal_file_open(&f, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_posix_journal_file_open(&f, ""), ASX_E_INVALID_ARGUMENT);
    /* An existing path is never overwritten. */
    ASSERT_EQ(asx_posix_journal_file_open(&f, "/dev/null"), ASX_E_ALREADY_EXISTS);
    ASSERT_EQ(asx_posix_journal_file_open(&f, "/nonexistent-asx-dir/journal"), ASX_E_NOT_FOUND);

    /* The sink and commit over a pipe's write end: the bytes arrive whole,
     * and fsync(2) on a pipe is the OS's error, mapped. */
    ASSERT_EQ(pipe(fds), 0);
    f.fd = fds[1];
    ASSERT_EQ(asx_posix_journal_file_sink(&f, (const uint8_t *)"hello", 5u), ASX_OK);
    ASSERT_EQ(f.bytes, (uint64_t)5u);
    ASSERT_EQ((int)read(fds[0], got, sizeof(got)), 5);
    ASSERT_EQ(memcmp(got, "hello", 5u), 0);
    c.stream_bytes = 5u;
    ASSERT_EQ(asx_posix_journal_file_commit(&f, &c), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_posix_journal_file_sink(NULL, (const uint8_t *)"x", 1u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_posix_journal_file_commit(&f, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_posix_journal_file_close(&f), ASX_OK);
    ASSERT_EQ(asx_posix_journal_file_close(&f), ASX_OK); /* idempotent */
    ASSERT_EQ(asx_posix_journal_file_sink(&f, (const uint8_t *)"x", 1u), ASX_E_INVALID_ARGUMENT);
    (void)close(fds[0]);
}
#endif /* ASX_PROFILE_POSIX */

/* ------------------------------------------------------------------ */
/* A live POSIX run                                                    */
/* ------------------------------------------------------------------ */

#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC

#include <asx/net/net.h>
#include <asx/platform/posix.h>
#include <asx/process/process.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/signal/signal.h>
#include <signal.h>

typedef struct {
    asx_co_state co;
    asx_tcp_listener listener;
    asx_tcp_stream conn;
    uint8_t buf[16];
    uint32_t len;
    uint32_t sent;
} jserver;

static asx_status jserver_poll(void *ud, asx_task_id self) {
    jserver *s = (jserver *)ud;
    asx_status st;
    uint32_t n;
    (void)self;
    ASX_CO_BEGIN(&s->co);
    for (;;) {
        st = asx_tcp_listener_poll_accept(s->listener, &s->conn, NULL);
        if (st == ASX_OK) break;
        if (st != ASX_E_PENDING) return st;
        ASX_CO_YIELD(&s->co);
    }
    while (s->len == 0u || s->buf[s->len - 1u] != '\n') {
        st = asx_tcp_stream_read(s->conn, &s->buf[s->len], (uint32_t)sizeof(s->buf) - s->len, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&s->co);
            continue;
        }
        if (st != ASX_OK || n == 0u) return ASX_E_DISCONNECTED;
        s->len += n;
    }
    while (s->sent < s->len) {
        st = asx_tcp_stream_write(s->conn, &s->buf[s->sent], s->len - s->sent, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&s->co);
            continue;
        }
        if (st != ASX_OK) return st;
        s->sent += n;
    }
    (void)asx_tcp_stream_close(s->conn);
    ASX_CO_END(&s->co);
}

typedef struct {
    asx_co_state co;
    asx_socket_addr addr;
    asx_tcp_stream conn;
    uint32_t sent;
    uint8_t reply[16];
    uint32_t reply_len;
} jclient;

static asx_status jclient_poll(void *ud, asx_task_id self) {
    jclient *c = (jclient *)ud;
    asx_status st;
    uint32_t n;
    (void)self;
    ASX_CO_BEGIN(&c->co);
    st = asx_tcp_connect(&c->conn, &c->addr);
    if (st != ASX_OK) return st;
    while (c->sent < 5u) {
        st = asx_tcp_stream_write(c->conn, (const uint8_t *)"ping\n" + c->sent, 5u - c->sent, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&c->co);
            continue;
        }
        if (st != ASX_OK) return st;
        c->sent += n;
    }
    for (;;) {
        st = asx_tcp_stream_read(c->conn, &c->reply[c->reply_len],
                                 (uint32_t)sizeof(c->reply) - c->reply_len, &n);
        if (st == ASX_E_PENDING) {
            ASX_CO_YIELD(&c->co);
            continue;
        }
        if (st != ASX_OK) return st;
        if (n == 0u) break;
        c->reply_len += n;
    }
    (void)asx_tcp_stream_close(c->conn);
    ASX_CO_END(&c->co);
}

static uint64_t blocking_double(void *user_data) { return *(uint64_t *)user_data * 2u; }

/* How many records of `kind` the stream holds. */
static uint32_t count_kind(uint8_t kind) {
    asx_journal_reader r;
    asx_journal_record rec;
    uint32_t n = 0u;
    if (asx_journal_reader_init(&r, g_out, g_out_len) != ASX_JOURNAL_READ_RECORD) return 0u;
    while (asx_journal_reader_next(&r, &rec) == ASX_JOURNAL_READ_RECORD) {
        if (rec.kind == kind) n++;
    }
    return n;
}

TEST(a_small_live_run_is_recorded_and_read_back) {
    static asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_journal j;
    asx_journal_record rec;
    asx_journal_reader r;
    asx_journal_read_result rr;
    asx_time t0 = 0;
    asx_time t1 = 0;
    uint64_t e0 = 0;
    asx_reactor_event ev[4];
    uint32_t nev = 0u;
    int fds[2] = {-1, -1};
    asx_region_id region;
    asx_task_id ts;
    asx_task_id tc;
    jserver *s = NULL;
    jclient *c = NULL;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_budget budget;
    asx_signal_subscription sub;
    uint32_t sig_count = 0u;
    asx_process_command cmd;
    asx_process_handle ph;
    asx_process_output out;
    int found_t0 = 0;
    int found_t1 = 0;
    int found_e0 = 0;
    int found_poll = 0;
    uint64_t payload = 21u;
    asx_blocking_handle bh;
    asx_blocking_state bstate;
    uint32_t spins = 0u;
    uint64_t bresult = 0u;
    char b;

    sink_reset();
    ASSERT_EQ(asx_posix_hooks_install(&hooks), ASX_OK);
    ASSERT_EQ(open_journal(&j, (uint32_t)sizeof(g_stage), 0u, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
    asx_runtime_config_init(&cfg);
    asx_runtime_shutdown(&rt);
    ASSERT_EQ(asx_runtime_init(&rt, &cfg, &hooks), ASX_OK);
    ASSERT_EQ(asx_net_set_backend(ASX_NET_BACKEND_NATIVE), ASX_OK);

    /* Clock and entropy through the runtime's accessors. */
    ASSERT_EQ(asx_runtime_now_ns(&t0), ASX_OK);
    ASSERT_EQ(asx_runtime_now_ns(&t1), ASX_OK);
    ASSERT_TRUE(t1 >= t0);
    ASSERT_EQ(asx_runtime_random_u64(&e0), ASX_OK);

    /* Reactor readiness on a pipe. */
    ASSERT_EQ(pipe(fds), 0);
    ASSERT_EQ(asx_runtime_reactor_register(fds[0], ASX_REACTOR_READABLE, 77u), ASX_OK);
    b = 'x';
    ASSERT_EQ((int)write(fds[1], &b, 1u), 1);
    ASSERT_EQ(asx_runtime_reactor_poll(1000u, ev, 4u, &nev), ASX_OK);
    ASSERT_EQ(nev, 1u);
    ASSERT_EQ(ev[0].token, (uint64_t)77u);
    asx_runtime_reactor_deregister(fds[0]);
    (void)close(fds[0]);
    (void)close(fds[1]);

    /* A TCP round trip between two tasks. */
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(region, jserver_poll, (uint32_t)sizeof(jserver), NULL, &ts,
                                      (void **)&s),
              ASX_OK);
    ASSERT_EQ(asx_tcp_listener_bind(&s->listener, &any), ASX_OK);
    ASSERT_EQ(asx_task_spawn_captured(region, jclient_poll, (uint32_t)sizeof(jclient), NULL, &tc,
                                      (void **)&c),
              ASX_OK);
    ASSERT_EQ(asx_tcp_listener_local_addr(s->listener, &c->addr), ASX_OK);
    budget = asx_budget_from_polls(200u);
    ASSERT_EQ(asx_scheduler_run(region, &budget), ASX_OK);
    ASSERT_EQ(c->reply_len, 5u);
    ASSERT_EQ(memcmp(c->reply, "ping\n", 5u), 0);
    ASSERT_EQ(asx_tcp_listener_close(s->listener), ASX_OK);

    /* A blocking job through the pool. */
    ASSERT_EQ(asx_spawn_blocking(blocking_double, &payload, NULL, &bh), ASX_OK);
    for (spins = 0u; spins < 2000u; spins++) {
        struct timespec nap;
        bstate = asx_blocking_get_state(&bh);
        if (bstate == ASX_BLOCKING_COMPLETED) break;
        nap.tv_sec = 0;
        nap.tv_nsec = 1000000L; /* 1 ms */
        (void)nanosleep(&nap, NULL);
    }
    ASSERT_EQ((int)asx_blocking_get_state(&bh), (int)ASX_BLOCKING_COMPLETED);
    ASSERT_EQ(asx_blocking_get_result(&bh, &bresult), ASX_OK);
    ASSERT_EQ(bresult, (uint64_t)42u);

    /* A signal. */
    asx_signal_reset();
    ASSERT_EQ(asx_signal_set_backend(ASX_SIGNAL_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_signal_subscribe(&sub, ASX_SIGNAL_USR1), ASX_OK);
    ASSERT_EQ(raise(SIGUSR1), 0);
    ASSERT_EQ(asx_signal_poll(sub, &sig_count), ASX_OK);
    ASSERT_EQ(sig_count, 1u);
    ASSERT_EQ(asx_signal_unsubscribe(sub), ASX_OK);
    asx_signal_reset();

    /* A child's exit status. */
    asx_process_reset();
    ASSERT_EQ(asx_process_set_backend(ASX_PROCESS_BACKEND_NATIVE), ASX_OK);
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "exit 7"), ASX_OK);
    ASSERT_EQ(asx_process_command_spawn(&cmd, &ph), ASX_OK);
    ASSERT_EQ(asx_process_wait_with_output(ph, &out), ASX_OK);
    ASSERT_EQ(out.exit_code, 7);
    ASSERT_EQ(asx_process_release(ph), ASX_OK);
    asx_process_reset();

    asx_journal_uninstall();
    asx_runtime_shutdown(&rt);
    asx_net_reset();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    /* The stream is complete and says what the program saw. */
    ASSERT_EQ((int)asx_journal_reader_init(&r, g_out, g_out_len), (int)ASX_JOURNAL_READ_RECORD);
    for (;;) {
        rr = asx_journal_reader_next(&r, &rec);
        if (rr != ASX_JOURNAL_READ_RECORD) break;
        if (rec.kind == ASX_JOURNAL_KIND_CLOCK) {
            uint64_t ns = asx_journal_get_u64(rec.payload + 1);
            if (ns == (uint64_t)t0) found_t0 = 1;
            if (ns == (uint64_t)t1 && found_t0) found_t1 = 1;
        } else if (rec.kind == ASX_JOURNAL_KIND_ENTROPY) {
            if (asx_journal_get_u64(rec.payload + 5) == e0) found_e0 = 1;
        } else if (rec.kind == ASX_JOURNAL_KIND_REACTOR_POLL) {
            if (asx_journal_get_u32(rec.payload + 8) >= 1u &&
                asx_journal_get_u64(rec.payload + 12) == (uint64_t)77u) {
                found_poll = 1;
            }
        }
    }
    ASSERT_EQ((int)rr, (int)ASX_JOURNAL_READ_END);
    ASSERT_TRUE(found_t0);
    ASSERT_TRUE(found_t1);
    ASSERT_TRUE(found_e0);
    ASSERT_TRUE(found_poll);
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_NET_ACCEPT) >= 1u);
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_NET_CONNECT) >= 1u);
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_NET_READ) >= 2u);
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_NET_WRITE) >= 2u);
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_SIGNAL) >= 1u); /* every drain, empty ones too */
    ASSERT_EQ(count_kind(ASX_JOURNAL_KIND_PROCESS_SPAWN), 1u);
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_PROCESS_EXIT) >= 1u); /* every reap attempt */
    ASSERT_TRUE(count_kind(ASX_JOURNAL_KIND_NET_LISTEN) >= 1u);
    ASSERT_EQ(count_kind(ASX_JOURNAL_KIND_BLOCKING_DONE), 1u);

    /* The bytes the server read are in the stream (FULL mode). */
    {
        int saw_ping = 0;
        asx_journal_reader rr2;
        asx_journal_record r2;
        ASSERT_EQ((int)asx_journal_reader_init(&rr2, g_out, g_out_len),
                  (int)ASX_JOURNAL_READ_RECORD);
        while (asx_journal_reader_next(&rr2, &r2) == ASX_JOURNAL_READ_RECORD) {
            if (r2.kind == ASX_JOURNAL_KIND_NET_READ && r2.payload_len >= 20u + 5u &&
                asx_journal_get_u32(r2.payload + 16) == 5u &&
                memcmp(r2.payload + 20, "ping\n", 5u) == 0) {
                saw_ping = 1;
            }
        }
        ASSERT_TRUE(saw_ping);
    }
}

#endif /* ASX_PROFILE_POSIX && !ASX_DETERMINISTIC */

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_journal ===\n");

    RUN_TEST(init_rejects_a_missing_sink_buffer_or_a_small_buffer);
    RUN_TEST(an_empty_stream_is_a_header_and_an_end);
    RUN_TEST(a_bad_header_is_refused);
    RUN_TEST(every_native_boundary_kind_round_trips);
    RUN_TEST(the_chain_verifies_a_complete_stream_and_catches_edits);
    RUN_TEST(shape_mode_and_oversized_reads_store_length_and_hash);
    RUN_TEST(a_sink_failure_sticks_and_never_reaches_the_caller);
    RUN_TEST(records_are_batched_into_few_sink_calls);
    RUN_TEST(only_one_journal_is_active_at_a_time);
    RUN_TEST(the_ring_sink_keeps_the_most_recent_bytes);
    RUN_TEST(wrapped_hooks_forward_unchanged_and_record_raw_results);
    RUN_TEST(a_journal_over_the_default_clock_keeps_it_virtual_and_a_sleep_completes);
    RUN_TEST(a_journal_over_a_custom_clock_is_not_a_virtual_clock);
    RUN_TEST(a_poll_larger_than_the_buffer_is_stored_truncated_and_flagged);
    RUN_TEST(commit_points_hand_the_prefix_to_the_sink_before_commit_fn_runs);
    RUN_TEST(a_buffered_journal_never_calls_commit_fn);
    RUN_TEST(a_failing_commit_sticks_and_marks_the_stream_incomplete);
    RUN_TEST(a_sampled_window_carries_a_trace_checkpoint_the_chain_covers);
    RUN_TEST(the_reader_bounds_the_committed_prefix_of_a_cut_stream);
    RUN_TEST(the_flash_log_holds_only_the_committed_prefix_when_the_run_dies);
    RUN_TEST(a_wrapped_ring_flushes_in_order);
    RUN_TEST(the_wear_budget_keeps_flash_to_a_chain_aligned_prefix);
    RUN_TEST(min_flush_coalesces_commits_and_the_final_commit_flushes_the_rest);
    RUN_TEST(a_failed_flash_write_sticks_and_flash_keeps_the_earlier_prefix);
    RUN_TEST(the_flash_store_rejects_bad_arguments);
    RUN_TEST(the_watchdog_store_writes_only_committed_bytes_at_checkpoints_in_bounded_writes);
    RUN_TEST(a_watchdog_flush_is_at_most_once_per_period_and_a_stepped_back_clock_adds_no_time);
    RUN_TEST(the_final_commit_drains_the_watchdog_backlog_in_bounded_writes_to_a_complete_stream);
    RUN_TEST(a_wrapped_watchdog_ring_flushes_in_order);
    RUN_TEST(a_producer_that_outruns_the_bound_fails_the_stream_and_the_log_keeps_its_prefix);
    RUN_TEST(a_failed_log_write_is_tried_again_at_the_next_checkpoint);
    RUN_TEST(a_watchdog_checkpoint_services_the_automotive_watchdog_then_flushes);
    RUN_TEST(the_watchdog_store_rejects_bad_arguments);
    RUN_TEST(each_profile_names_its_persistence_store);
#if defined(ASX_PROFILE_POSIX)
    RUN_TEST(the_posix_file_store_writes_every_byte_and_maps_errors);
#endif
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    RUN_TEST(a_small_live_run_is_recorded_and_read_back);
#endif

    TEST_REPORT();
    return test_failures;
}
