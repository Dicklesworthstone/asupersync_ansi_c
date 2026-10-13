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

#if defined(ASX_PROFILE_POSIX) && defined(ASX_DETERMINISTIC) && (ASX_DETERMINISTIC == 0)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>   /* nanosleep */
#include <unistd.h> /* pipe, write, close */
#endif

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/journal.h>
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
    copy[8] = 2u; /* version 2 */
    ASSERT_EQ((int)asx_journal_reader_init(&r, copy, g_out_len), (int)ASX_JOURNAL_READ_BAD_HEADER);
    memcpy(copy, g_out, g_out_len);
    copy[30] = 1u; /* reserved bytes are zero */
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
    asx_journal_note_net_accept(7u, (int32_t)ASX_OK, stream, &peer);
    asx_journal_note_net_connect(&peer, (int32_t)ASX_E_PENDING, 0u);
    asx_journal_note_net_read(stream, 64u, (int32_t)ASX_OK, (const uint8_t *)"hello", 5u);
    asx_journal_note_net_write(stream, 10u, (int32_t)ASX_OK, 7u);
    asx_journal_note_udp_recv(stream, 128u, (int32_t)ASX_OK, (const uint8_t *)"dgram", 5u, &peer);
    asx_journal_note_udp_send(stream, 5u, (int32_t)ASX_OK, 5u);
    asx_journal_note_resolve((int32_t)ASX_OK, addrs, 2u);
    asx_journal_note_process_spawn((int32_t)ASX_OK, stream, 4242u);
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
    ASSERT_EQ(kinds_seen, 11u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_ACCEPT, 0u, &rec));
    ASSERT_EQ(rec.payload_len, 39u);
    ASSERT_EQ(asx_journal_get_u64(rec.payload), (uint64_t)7u);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 8), (uint32_t)ASX_OK);
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 12), stream);
    ASSERT_EQ(rec.payload[20], 0u);
    ASSERT_EQ(rec.payload[21], 127u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 37), 8080u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_NET_CONNECT, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 19), (uint32_t)ASX_E_PENDING);

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
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 4), 2u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 8 + 17), 8080u);
    ASSERT_EQ(asx_journal_get_u16(rec.payload + 8 + JADDR_LEN_FOR_TEST + 17), 9090u);

    ASSERT_TRUE(find_record(g_out, g_out_len, ASX_JOURNAL_KIND_PROCESS_SPAWN, 0u, &rec));
    ASSERT_EQ(asx_journal_get_u64(rec.payload + 4), stream);
    ASSERT_EQ(asx_journal_get_u32(rec.payload + 12), 4242u);

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
    ASSERT_EQ(count_kind(ASX_JOURNAL_KIND_SIGNAL), 1u);
    ASSERT_EQ(count_kind(ASX_JOURNAL_KIND_PROCESS_SPAWN), 1u);
    ASSERT_EQ(count_kind(ASX_JOURNAL_KIND_PROCESS_EXIT), 1u);
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
    RUN_TEST(a_poll_larger_than_the_buffer_is_stored_truncated_and_flagged);
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    RUN_TEST(a_small_live_run_is_recorded_and_read_back);
#endif

    TEST_REPORT();
    return test_failures;
}
