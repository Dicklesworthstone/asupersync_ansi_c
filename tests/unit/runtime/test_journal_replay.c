/*
 * test_journal_replay.c — replaying a native boundary journal (bd-9kll.11.2,
 * RP2)
 *
 * Portable tests drive the replay's hook table directly: a run recorded
 * through wrapped fake hooks is replayed with identical results, and the
 * contract's refusals are each planted: a build mismatch, an incomplete
 * stream, a tampered record (chain break, reported with its window), a kind
 * the replay cannot serve, a reordered call, a changed argument, a request
 * after the last record, a record never requested. On a live POSIX build a
 * real small run (clock, entropy, readiness on a pipe, a sleeping task under
 * the scheduler) is recorded, then replayed into a fresh runtime with
 * identical results and trace digest.
 *
 * SPDX-License-Identifier: MIT
 */

#if defined(ASX_PROFILE_POSIX) && defined(ASX_DETERMINISTIC) && (ASX_DETERMINISTIC == 0)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <unistd.h> /* pipe, write, close */
#endif

#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/runtime/journal.h>
#include <asx/runtime/journal_replay.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* An in-memory sink and a journal over it                             */
/* ------------------------------------------------------------------ */

#define OUT_CAP (128u * 1024u)

static uint8_t g_out[OUT_CAP];
static size_t g_out_len;
static uint8_t g_stage[16u * 1024u];
static uint8_t g_copy[OUT_CAP];

#define REV_HASH UINT64_C(0x5e60b1c4c0000001)
#define BUILD_HASH UINT64_C(0x02cdd96000000002)

static asx_status mem_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    (void)ctx;
    if (g_out_len + len > OUT_CAP) return ASX_E_BUFFER_TOO_SMALL;
    memcpy(g_out + g_out_len, bytes, len);
    g_out_len += len;
    return ASX_OK;
}

static asx_status open_journal(asx_journal *j, uint32_t chain_every) {
    asx_journal_config cfg;
    asx_journal_uninstall();
    g_out_len = 0u;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = mem_sink;
    cfg.buffer = g_stage;
    cfg.buffer_cap = (uint32_t)sizeof(g_stage);
    cfg.chain_every = chain_every;
    cfg.config_fingerprint = UINT64_C(0x1122334455667788);
    cfg.rust_rev_hash = REV_HASH;
    cfg.build_hash = BUILD_HASH;
    return asx_journal_init(j, &cfg);
}

static asx_replay_identity ours(void) {
    asx_replay_identity id;
    id.rust_rev_hash = REV_HASH;
    id.build_hash = BUILD_HASH;
    return id;
}

/* The nth data record of `kind`: its seq and the offset of its payload, or 0. */
static int locate(const uint8_t *bytes, size_t len, uint8_t kind, uint32_t nth, uint64_t *seq,
                  size_t *payload_off) {
    asx_journal_reader r;
    asx_journal_record rec;
    if (asx_journal_reader_init(&r, bytes, len) != ASX_JOURNAL_READ_RECORD) return 0;
    while (asx_journal_reader_next(&r, &rec) == ASX_JOURNAL_READ_RECORD) {
        if (rec.kind != kind) continue;
        if (nth == 0u) {
            *seq = rec.seq;
            *payload_off = (size_t)(rec.payload - bytes);
            return 1;
        }
        nth--;
    }
    return 0;
}

/* The seq of the END record of the stream in g_out: it is the last record
 * (24-byte head, 64-byte payload) and its seq sits at head offset 8. */
static uint64_t end_seq(void) {
    size_t head = g_out_len - ASX_JOURNAL_END_PAYLOAD_BYTES - ASX_JOURNAL_RECORD_HEAD_BYTES;
    return asx_journal_get_u64(g_out + head + 8u);
}

static asx_status null_sink(void *ctx, const uint8_t *bytes, uint32_t len) {
    (void)ctx;
    (void)bytes;
    (void)len;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Fake hooks and the scripted run                                     */
/* ------------------------------------------------------------------ */

static uint64_t g_fake_clock;

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
    *out = 0xABCDu + g_fake_clock;
    return ASX_OK;
}

static asx_status fake_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready) {
    (void)ctx;
    *ready = timeout_ms / 10u;
    return ASX_OK;
}

static asx_status fake_ghost(void *ctx, uint64_t step, uint32_t *ready) {
    (void)ctx;
    (void)step;
    *ready = 0u;
    return ASX_OK;
}

static asx_status fake_poll(void *ctx, uint32_t timeout_ms, asx_reactor_event *events,
                            uint32_t max_events, uint32_t *out_count) {
    (void)ctx;
    (void)timeout_ms;
    if (max_events < 2u) return ASX_E_INVALID_ARGUMENT;
    events[0].token = 7u;
    events[0].ready = ASX_REACTOR_READABLE;
    events[1].token = 9u;
    events[1].ready = ASX_REACTOR_WRITABLE | ASX_REACTOR_ERROR;
    *out_count = 2u;
    return ASX_OK;
}

static void fake_table(asx_runtime_hooks *hooks) {
    memset(hooks, 0, sizeof(*hooks));
    hooks->clock.now_ns_fn = fake_now;
    hooks->clock.logical_now_ns_fn = fake_now;
    hooks->entropy.random_u64_fn = fake_random;
    hooks->entropy.try_u64_fn = fake_try;
    hooks->reactor.wait_fn = fake_wait;
    hooks->reactor.ghost_wait_fn = fake_ghost;
    hooks->reactor.poll_fn = fake_poll;
}

typedef struct {
    asx_time t0;
    uint64_t e0;
    uint32_t waited;
    asx_status wait_st;
    uint32_t polled;
    uint64_t token0;
    uint32_t ready1;
    asx_time t1;
} script_obs;

/* The scripted run: clock, entropy, a wait of 25 ms, a poll of 40 ms, clock.
 * Seqs when recorded with the default chain: 0 1 2 3 4 are the five data
 * records, 5 the final chain, 6 the END. */
static void script(const asx_runtime_hooks *hooks, script_obs *o) {
    asx_reactor_event ev[4];
    memset(o, 0, sizeof(*o));
    o->t0 = hooks->clock.now_ns_fn(hooks->clock.ctx);
    (void)hooks->entropy.try_u64_fn(hooks->entropy.ctx, &o->e0);
    o->wait_st = hooks->reactor.wait_fn(hooks->reactor.ctx, 25u, &o->waited);
    (void)hooks->reactor.poll_fn(hooks->reactor.ctx, 40u, ev, 4u, &o->polled);
    o->token0 = ev[0].token;
    o->ready1 = ev[1].ready;
    o->t1 = hooks->clock.now_ns_fn(hooks->clock.ctx);
}

/* Record the script through wrapped fake hooks; leaves the stream in g_out. */
static int record_script(uint32_t chain_every, script_obs *seen) {
    asx_journal j;
    asx_runtime_hooks hooks;
    g_fake_clock = 0u;
    fake_table(&hooks);
    if (open_journal(&j, chain_every) != ASX_OK) return 0;
    if (asx_journal_wrap_hooks(&j, &hooks) != ASX_OK) return 0;
    script(&hooks, seen);
    asx_journal_uninstall();
    return asx_journal_finish(&j) == ASX_OK;
}

static int open_replay(asx_replay *rp, asx_replay_report *rep) {
    asx_replay_identity id = ours();
    return asx_replay_open(rp, g_out, g_out_len, &id, rep) == ASX_REPLAY_OK;
}

/* ------------------------------------------------------------------ */
/* Replaying a recorded run                                            */
/* ------------------------------------------------------------------ */

TEST(a_recorded_run_replays_with_identical_results) {
    script_obs rec;
    script_obs got;
    asx_replay rp;
    asx_replay_report rep;
    asx_runtime_hooks hooks;

    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(open_replay(&rp, &rep));
    memset(&hooks, 0, sizeof(hooks));
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_OK);
    ASSERT_TRUE(hooks.deterministic_seeded_prng != 0u);
    script(&hooks, &got);

    ASSERT_EQ(got.t0, rec.t0);
    ASSERT_EQ(got.e0, rec.e0);
    ASSERT_EQ(got.wait_st, rec.wait_st);
    ASSERT_EQ(got.waited, rec.waited);
    ASSERT_EQ(got.polled, 2u);
    ASSERT_EQ(got.token0, (uint64_t)7u);
    ASSERT_EQ(got.ready1, rec.ready1);
    ASSERT_EQ(got.t1, rec.t1);
    ASSERT_EQ(asx_replay_served(&rp), (uint64_t)5u);
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
}

TEST(the_replay_hooks_validate_in_both_modes) {
    script_obs rec;
    asx_replay rp;
    asx_replay_report rep;
    asx_runtime_hooks hooks;

    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(open_replay(&rp, &rep));
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 0), ASX_OK);
    ASSERT_EQ(asx_runtime_hooks_validate(&hooks, 1), ASX_OK);
    ASSERT_EQ(asx_replay_hooks(NULL, &hooks), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Refusals at open                                                    */
/* ------------------------------------------------------------------ */

TEST(open_refuses_a_stream_from_another_build_or_revision) {
    script_obs rec;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id;
    asx_runtime_hooks hooks;

    ASSERT_TRUE(record_script(0u, &rec));
    id = ours();
    id.rust_rev_hash ^= 1u;
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep),
              (int)ASX_REPLAY_E_BUILD_MISMATCH);
    ASSERT_EQ((int)rep.error, (int)ASX_REPLAY_E_BUILD_MISMATCH);
    ASSERT_STR_EQ(rep.what, "rust_rev_hash");
    ASSERT_EQ(rep.seq, (uint64_t)0u);
    /* A refused stream serves nothing. */
    memset(&hooks, 0, sizeof(hooks));
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_replay_activate(&rp), ASX_E_INVALID_ARGUMENT);

    id = ours();
    id.build_hash ^= 1u;
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep),
              (int)ASX_REPLAY_E_BUILD_MISMATCH);
    ASSERT_STR_EQ(rep.what, "build_hash");

    /* A stream that did not say what built it fails a hash the caller checks. */
    id = ours();
    memcpy(g_copy, g_out, g_out_len);
    memset(g_copy + 24u, 0, 8u);
    ASSERT_EQ((int)asx_replay_open(&rp, g_copy, g_out_len, &id, &rep),
              (int)ASX_REPLAY_E_BUILD_MISMATCH);
    ASSERT_STR_EQ(rep.what, "rust_rev_hash");

    /* A hash the caller does not check is not compared. */
    id = ours();
    id.build_hash = 0u;
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep), (int)ASX_REPLAY_OK);
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, NULL, &rep), (int)ASX_REPLAY_OK);
}

TEST(open_refuses_an_incomplete_stream_and_names_the_missing_seq) {
    script_obs rec;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id = ours();
    size_t end_bytes = ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_END_PAYLOAD_BYTES;

    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_EQ(end_seq(), (uint64_t)6u); /* five data records, a chain, the END */
    /* The stream without its END record: seq 6 is the first one missing. */
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len - end_bytes, &id, &rep),
              (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(rep.seq, end_seq());
    /* Cut inside a record. */
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len - 10u, &id, &rep),
              (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(rep.seq, end_seq());
    /* An END that says the recorder dropped a record. */
    memcpy(g_copy, g_out, g_out_len);
    g_copy[g_out_len - ASX_JOURNAL_END_PAYLOAD_BYTES + 16u] = 1u;
    ASSERT_EQ((int)asx_replay_open(&rp, g_copy, g_out_len, &id, &rep),
              (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(rep.seq, end_seq());
    ASSERT_EQ(rep.expected_kind, (uint8_t)ASX_JOURNAL_KIND_END);
    /* Not even a header. */
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, 20u, &id, &rep), (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(rep.seq, (uint64_t)0u);
}

TEST(a_tampered_record_breaks_the_chain_and_is_reported_with_its_seq) {
    script_obs rec;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id = ours();
    uint64_t seq = 0u;
    size_t off = 0u;

    /* A chain record after every data record: the window is one record. */
    ASSERT_TRUE(record_script(1u, &rec));
    ASSERT_TRUE(locate(g_out, g_out_len, (uint8_t)ASX_JOURNAL_KIND_ENTROPY, 0u, &seq, &off));
    ASSERT_EQ(seq, (uint64_t)2u); /* clock 0, chain 1, entropy 2 */
    memcpy(g_copy, g_out, g_out_len);
    g_copy[off + 5u] ^= 0xFFu; /* a byte of the entropy value */
    ASSERT_EQ((int)asx_replay_open(&rp, g_copy, g_out_len, &id, &rep), (int)ASX_REPLAY_E_CORRUPT);
    ASSERT_EQ(rep.first_seq, (uint64_t)2u); /* the tampered record */
    ASSERT_EQ(rep.seq, (uint64_t)3u);       /* the chain that caught it */

    /* With the default chain the window is wider and says so. */
    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(locate(g_out, g_out_len, (uint8_t)ASX_JOURNAL_KIND_ENTROPY, 0u, &seq, &off));
    memcpy(g_copy, g_out, g_out_len);
    g_copy[off + 5u] ^= 0xFFu;
    ASSERT_EQ((int)asx_replay_open(&rp, g_copy, g_out_len, &id, &rep), (int)ASX_REPLAY_E_CORRUPT);
    ASSERT_TRUE(rep.first_seq <= seq && seq < rep.seq);
    ASSERT_EQ(rep.first_seq, (uint64_t)0u);
    ASSERT_EQ(rep.seq, end_seq() - 1u); /* the final chain */
}

TEST(open_refuses_records_the_replay_cannot_serve_naming_the_first) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id = ours();
    uint8_t data[4] = {1u, 2u, 3u, 4u};

    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_process_spawn((int32_t)ASX_OK, 5u, 99u);      /* seq 0 */
    asx_journal_note_net_read(5u, 16u, (int32_t)ASX_OK, data, 4u); /* seq 1 */
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep),
              (int)ASX_REPLAY_E_UNSUPPORTED);
    ASSERT_EQ(rep.seq, (uint64_t)0u);
    ASSERT_EQ(rep.expected_kind, (uint8_t)ASX_JOURNAL_KIND_PROCESS_SPAWN);

    /* An integrity failure is reported before replayability. */
    memcpy(g_copy, g_out, g_out_len);
    g_copy[ASX_JOURNAL_HEADER_BYTES + ASX_JOURNAL_RECORD_HEAD_BYTES + 5u] ^= 0x7Fu;
    ASSERT_EQ((int)asx_replay_open(&rp, g_copy, g_out_len, &id, &rep), (int)ASX_REPLAY_E_CORRUPT);
}

/* ------------------------------------------------------------------ */
/* Divergence                                                          */
/* ------------------------------------------------------------------ */

TEST(a_reordered_call_diverges_at_the_seq_of_the_record_it_met) {
    script_obs rec;
    asx_replay rp;
    asx_replay_report rep;
    asx_runtime_hooks hooks;
    uint64_t v = 0u;
    const asx_replay_report *f;

    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(open_replay(&rp, &rep));
    memset(&hooks, 0, sizeof(hooks));
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_OK);

    /* The run recorded a clock read first; this one draws entropy first. */
    ASSERT_EQ(hooks.entropy.try_u64_fn(hooks.entropy.ctx, &v), ASX_E_REPLAY_MISMATCH);
    f = asx_replay_failure(&rp);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ((int)f->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(f->seq, (uint64_t)0u);
    ASSERT_EQ(f->expected_kind, (uint8_t)ASX_JOURNAL_KIND_CLOCK);
    ASSERT_EQ(f->actual_kind, (uint8_t)ASX_JOURNAL_KIND_ENTROPY);
    ASSERT_STR_EQ(f->what, "kind");

    /* Everything after answers nothing and the first report stands. */
    ASSERT_EQ(hooks.clock.now_ns_fn(hooks.clock.ctx), (asx_time)0u);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)0u);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(rep.seq, (uint64_t)0u);
    ASSERT_EQ(asx_replay_served(&rp), (uint64_t)0u);
}

/* A fresh replay of the recorded script with its clock read and entropy draw
 * already served: the next record is the wait (seq 2). */
static int replay_to_wait(asx_replay *rp, asx_runtime_hooks *hooks) {
    asx_replay_report rep;
    uint64_t e = 0u;
    if (!open_replay(rp, &rep)) return 0;
    memset(hooks, 0, sizeof(*hooks));
    if (asx_replay_hooks(rp, hooks) != ASX_OK) return 0;
    (void)hooks->clock.now_ns_fn(hooks->clock.ctx);
    (void)hooks->entropy.try_u64_fn(hooks->entropy.ctx, &e);
    return asx_replay_failure(rp) == NULL;
}

TEST(a_changed_argument_diverges_with_the_field_named) {
    script_obs rec;
    asx_replay rp;
    asx_runtime_hooks hooks;
    uint32_t ready = 0u;
    uint32_t n = 0u;
    asx_reactor_event ev[1];

    ASSERT_TRUE(record_script(0u, &rec));

    /* A wait of 26 ms where 25 was recorded (seq 2). */
    ASSERT_TRUE(replay_to_wait(&rp, &hooks));
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 26u, &ready), ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)2u);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "wait timeout");

    /* A poll with room for one event where two were recorded (seq 3). */
    ASSERT_TRUE(replay_to_wait(&rp, &hooks));
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 25u, &ready), ASX_OK);
    ASSERT_EQ(hooks.reactor.poll_fn(hooks.reactor.ctx, 40u, ev, 1u, &n), ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)3u);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "poll room for the recorded events");

    /* A poll timeout that differs. */
    ASSERT_TRUE(replay_to_wait(&rp, &hooks));
    ASSERT_EQ(hooks.reactor.wait_fn(hooks.reactor.ctx, 25u, &ready), ASX_OK);
    ASSERT_EQ(hooks.reactor.poll_fn(hooks.reactor.ctx, 41u, ev, 1u, &n), ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "poll timeout");
}

TEST(a_request_after_the_last_record_is_exhausted_at_the_end_seq) {
    script_obs rec;
    script_obs got;
    asx_replay rp;
    asx_replay_report rep;
    asx_runtime_hooks hooks;

    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(open_replay(&rp, &rep));
    memset(&hooks, 0, sizeof(hooks));
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_OK);
    script(&hooks, &got);
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);

    (void)hooks.clock.now_ns_fn(hooks.clock.ctx); /* one more than was recorded */
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_EXHAUSTED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, end_seq());
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_END);
    ASSERT_EQ(asx_replay_failure(&rp)->actual_kind, (uint8_t)ASX_JOURNAL_KIND_CLOCK);
}

TEST(records_the_program_never_asked_for_are_reported_at_finish) {
    script_obs rec;
    asx_replay rp;
    asx_replay_report rep;
    asx_runtime_hooks hooks;
    uint64_t e = 0u;
    uint32_t ready = 0u;

    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(open_replay(&rp, &rep));
    memset(&hooks, 0, sizeof(hooks));
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_OK);
    (void)hooks.clock.now_ns_fn(hooks.clock.ctx);
    (void)hooks.entropy.try_u64_fn(hooks.entropy.ctx, &e);
    (void)hooks.reactor.wait_fn(hooks.reactor.ctx, 25u, &ready);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(rep.seq, (uint64_t)3u); /* the poll */
    ASSERT_EQ(rep.expected_kind, (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL);
    ASSERT_EQ(rep.actual_kind, 0u);
    ASSERT_STR_EQ(rep.what, "record never requested");
}

/* ------------------------------------------------------------------ */
/* Adapters: signal drains and name resolution                         */
/* ------------------------------------------------------------------ */

TEST(signal_drains_and_resolutions_are_served_in_order) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id = ours();
    uint32_t none[4] = {0u, 0u, 0u, 0u};
    uint32_t some[4] = {0u, 2u, 0u, 1u};
    uint32_t got[4];
    asx_journal_addr in[2];
    asx_journal_addr out[4];
    int32_t status = 0;
    uint32_t count = 0u;

    asx_replay_deactivate();
    memset(in, 0, sizeof(in));
    in[0].bytes[0] = 10u;
    in[0].port = 80u;
    in[1].family = 1u;
    in[1].bytes[15] = 1u;
    in[1].port = 443u;

    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_signal(none, 4u);
    asx_journal_note_signal(some, 4u);
    asx_journal_note_resolve(asx_journal_text_hash("example.test"), (int32_t)ASX_OK, in, 2u);
    asx_journal_note_signal(none, 4u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep), (int)ASX_REPLAY_OK);
    ASSERT_FALSE(asx_replay_active());
    ASSERT_EQ(asx_replay_activate(&rp), ASX_OK);
    ASSERT_TRUE(asx_replay_active());

    ASSERT_TRUE(asx_replay_serve_signal(got, 4u));
    ASSERT_EQ(memcmp(got, none, sizeof(none)), 0);
    ASSERT_TRUE(asx_replay_serve_signal(got, 4u));
    ASSERT_EQ(memcmp(got, some, sizeof(some)), 0);
    ASSERT_TRUE(
        asx_replay_serve_resolve(asx_journal_text_hash("example.test"), &status, out, 4u, &count));
    ASSERT_EQ(status, (int32_t)ASX_OK);
    ASSERT_EQ(count, 2u);
    ASSERT_EQ(out[0].port, 80u);
    ASSERT_EQ(out[0].bytes[0], 10u);
    ASSERT_EQ(out[1].family, 1u);
    ASSERT_EQ(out[1].port, 443u);
    ASSERT_TRUE(asx_replay_serve_signal(got, 4u));
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    asx_replay_deactivate();
    ASSERT_FALSE(asx_replay_active());
    ASSERT_FALSE(asx_replay_serve_signal(got, 4u)); /* no replay: the adapter does its own */
}

TEST(a_resolution_of_another_host_diverges) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id = ours();
    asx_journal_addr in[1];
    asx_journal_addr out[2];
    int32_t status = 0;
    uint32_t count = 0u;

    asx_replay_deactivate();
    memset(in, 0, sizeof(in));
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_resolve(asx_journal_text_hash("a.example"), (int32_t)ASX_OK, in, 1u);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep), (int)ASX_REPLAY_OK);
    ASSERT_EQ(asx_replay_activate(&rp), ASX_OK);
    ASSERT_TRUE(
        asx_replay_serve_resolve(asx_journal_text_hash("b.example"), &status, out, 2u, &count));
    ASSERT_EQ(count, 0u);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)0u);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "resolve host");
    asx_replay_deactivate();
}

TEST(a_replay_is_not_activated_while_a_journal_records) {
    script_obs rec;
    asx_journal j2;
    asx_journal_config cfg;
    asx_replay rp;
    asx_replay_report rep;
    static uint8_t stage2[1024];

    asx_replay_deactivate();
    ASSERT_TRUE(record_script(0u, &rec));
    ASSERT_TRUE(open_replay(&rp, &rep));

    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = null_sink;
    cfg.buffer = stage2;
    cfg.buffer_cap = (uint32_t)sizeof(stage2);
    ASSERT_EQ(asx_journal_init(&j2, &cfg), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j2), ASX_OK);
    ASSERT_EQ(asx_replay_activate(&rp), ASX_E_INVALID_STATE);
    ASSERT_FALSE(asx_replay_active());
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j2), ASX_OK);

    ASSERT_EQ(asx_replay_activate(&rp), ASX_OK);
    ASSERT_EQ(asx_replay_activate(&rp), ASX_OK); /* the same replay again */
    asx_replay_deactivate();
    ASSERT_FALSE(asx_replay_active());
}

/* ------------------------------------------------------------------ */
/* A real run, recorded and replayed (live POSIX)                      */
/* ------------------------------------------------------------------ */

#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC

#include <asx/platform/posix.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/time/sleep.h>

typedef struct {
    asx_time t0;
    uint64_t e0;
    uint64_t e1;
    uint32_t nev;
    uint64_t token;
    uint32_t ready;
    asx_status run_st;
    asx_time t1;
    uint64_t digest;
} program_obs;

/* The program: clock and entropy through the runtime's accessors, readiness
 * on a pipe, then a task that sleeps 3 ms under the scheduler. Everything it
 * learns about the outside comes through a recorded boundary. */
static asx_status run_program(program_obs *o, int reordered) {
    asx_region_id region;
    asx_task_id task;
    asx_sleep_state sl;
    asx_budget budget;
    asx_reactor_event ev[4];
    int fds[2] = {-1, -1};
    char b = 'x';
    asx_status st;

    memset(o, 0, sizeof(*o));
    if (reordered) {
        /* Entropy where the recording read the clock first. */
        (void)asx_runtime_random_u64(&o->e0);
        return ASX_OK;
    }
    st = asx_runtime_now_ns(&o->t0);
    if (st != ASX_OK) return st;
    st = asx_runtime_random_u64(&o->e0);
    if (st != ASX_OK) return st;
    st = asx_runtime_random_u64(&o->e1);
    if (st != ASX_OK) return st;

    if (pipe(fds) != 0) return ASX_E_INVALID_STATE;
    st = asx_runtime_reactor_register(fds[0], ASX_REACTOR_READABLE, 77u);
    if (st == ASX_OK && write(fds[1], &b, 1u) == 1) {
        st = asx_runtime_reactor_poll(1000u, ev, 4u, &o->nev);
        if (st == ASX_OK && o->nev > 0u) {
            o->token = ev[0].token;
            o->ready = ev[0].ready;
        }
    }
    asx_runtime_reactor_deregister(fds[0]);
    (void)close(fds[0]);
    (void)close(fds[1]);
    if (st != ASX_OK) return st;

    st = asx_region_open(&region);
    if (st != ASX_OK) return st;
    st = asx_sleep_init(&sl, 3000000u);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(region, asx_sleep_poll, &sl, &task);
    if (st != ASX_OK) return st;
    budget = asx_budget_from_polls(1000u);
    o->run_st = asx_scheduler_run(region, &budget);
    st = asx_runtime_now_ns(&o->t1);
    o->digest = asx_trace_digest();
    return st;
}

static asx_runtime g_rt;

/* Record run_program over the live POSIX hooks; leaves the stream in g_out.
 * 1 when the program and the journal both completed. */
static int record_live(program_obs *rec) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_journal j;
    int ok;

    if (asx_posix_hooks_install(&hooks) != ASX_OK) return 0;
    if (open_journal(&j, 0u) != ASX_OK) return 0;
    if (asx_journal_wrap_hooks(&j, &hooks) != ASX_OK) return 0;
    if (asx_runtime_config_init(&cfg) != ASX_OK) {
        asx_journal_uninstall();
        return 0;
    }
    if (asx_runtime_init(&g_rt, &cfg, &hooks) != ASX_OK) {
        asx_journal_uninstall();
        return 0;
    }
    ok = run_program(rec, 0) == ASX_OK;
    asx_runtime_shutdown(&g_rt);
    asx_journal_uninstall();
    return asx_journal_finish(&j) == ASX_OK && ok;
}

/* Run run_program into a fresh runtime whose hooks are the replay's. 1 when
 * the runtime came up and the program returned OK. */
static int run_replayed(asx_replay *rp, program_obs *got, int reordered) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;

    if (asx_runtime_config_init(&cfg) != ASX_OK) return 0;
    if (asx_runtime_hooks_init(&hooks) != ASX_OK) return 0;
    if (asx_replay_hooks(rp, &hooks) != ASX_OK) return 0;
    if (asx_runtime_init(&g_rt, &cfg, &hooks) != ASX_OK) return 0;
    st = run_program(got, reordered);
    asx_runtime_shutdown(&g_rt);
    return st == ASX_OK;
}

TEST(a_live_run_replays_with_identical_results_and_digest) {
    asx_replay rp;
    asx_replay_report rep;
    program_obs rec;
    program_obs got;

    ASSERT_TRUE(record_live(&rec));
    ASSERT_EQ(rec.run_st, ASX_OK);
    ASSERT_EQ(rec.nev, 1u);
    ASSERT_EQ(rec.token, (uint64_t)77u);

    ASSERT_TRUE(open_replay(&rp, &rep));
    ASSERT_TRUE(run_replayed(&rp, &got, 0));
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);

    ASSERT_EQ(got.t0, rec.t0);
    ASSERT_EQ(got.e0, rec.e0);
    ASSERT_EQ(got.e1, rec.e1);
    ASSERT_EQ(got.nev, rec.nev);
    ASSERT_EQ(got.token, rec.token);
    ASSERT_EQ(got.ready, rec.ready);
    ASSERT_EQ(got.run_st, rec.run_st);
    ASSERT_EQ(got.t1, rec.t1);
    ASSERT_EQ(got.digest, rec.digest);
}

TEST(a_live_stream_refuses_a_tampered_byte_and_a_reordered_call) {
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_identity id = ours();
    program_obs rec;
    program_obs got;
    uint64_t seq = 0u;
    size_t off = 0u;
    uint64_t t0_seq = 0u;
    size_t t0_off = 0u;
    uint32_t i;

    ASSERT_TRUE(record_live(&rec));

    /* The record of the program's own entropy draw: flip a byte of it. */
    for (i = 0u;; i++) {
        ASSERT_TRUE(locate(g_out, g_out_len, (uint8_t)ASX_JOURNAL_KIND_ENTROPY, i, &seq, &off));
        if (asx_journal_get_u64(g_out + off + 5u) == rec.e0) break;
    }
    memcpy(g_copy, g_out, g_out_len);
    g_copy[off + 5u] ^= 0x01u;
    ASSERT_EQ((int)asx_replay_open(&rp, g_copy, g_out_len, &id, &rep), (int)ASX_REPLAY_E_CORRUPT);
    ASSERT_TRUE(rep.first_seq <= seq && seq < rep.seq);

    /* A reordered call: entropy where the stream has the program's first
     * clock read, found by its value. */
    for (i = 0u;; i++) {
        ASSERT_TRUE(locate(g_out, g_out_len, (uint8_t)ASX_JOURNAL_KIND_CLOCK, i, &t0_seq, &t0_off));
        if (asx_journal_get_u64(g_out + t0_off + 1u) == (uint64_t)rec.t0) break;
    }
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep), (int)ASX_REPLAY_OK);
    ASSERT_TRUE(run_replayed(&rp, &got, 1));
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, t0_seq);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_CLOCK);
    ASSERT_EQ(asx_replay_failure(&rp)->actual_kind, (uint8_t)ASX_JOURNAL_KIND_ENTROPY);
}

#endif /* ASX_PROFILE_POSIX && !ASX_DETERMINISTIC */

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_journal_replay ===\n");

    RUN_TEST(a_recorded_run_replays_with_identical_results);
    RUN_TEST(the_replay_hooks_validate_in_both_modes);
    RUN_TEST(open_refuses_a_stream_from_another_build_or_revision);
    RUN_TEST(open_refuses_an_incomplete_stream_and_names_the_missing_seq);
    RUN_TEST(a_tampered_record_breaks_the_chain_and_is_reported_with_its_seq);
    RUN_TEST(open_refuses_records_the_replay_cannot_serve_naming_the_first);
    RUN_TEST(a_reordered_call_diverges_at_the_seq_of_the_record_it_met);
    RUN_TEST(a_changed_argument_diverges_with_the_field_named);
    RUN_TEST(a_request_after_the_last_record_is_exhausted_at_the_end_seq);
    RUN_TEST(records_the_program_never_asked_for_are_reported_at_finish);
    RUN_TEST(signal_drains_and_resolutions_are_served_in_order);
    RUN_TEST(a_resolution_of_another_host_diverges);
    RUN_TEST(a_replay_is_not_activated_while_a_journal_records);
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    RUN_TEST(a_live_run_replays_with_identical_results_and_digest);
    RUN_TEST(a_live_stream_refuses_a_tampered_byte_and_a_reordered_call);
#endif

    TEST_REPORT();
    return test_failures;
}
