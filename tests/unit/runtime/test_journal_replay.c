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
#include <asx/runtime/trace.h>
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

static asx_status open_journal_flags(asx_journal *j, uint32_t chain_every, uint32_t flags) {
    asx_journal_config cfg;
    asx_journal_uninstall();
    g_out_len = 0u;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = mem_sink;
    cfg.buffer = g_stage;
    cfg.buffer_cap = (uint32_t)sizeof(g_stage);
    cfg.chain_every = chain_every;
    cfg.flags = flags;
    cfg.config_fingerprint = UINT64_C(0x1122334455667788);
    cfg.rust_rev_hash = REV_HASH;
    cfg.build_hash = BUILD_HASH;
    return asx_journal_init(j, &cfg);
}

static asx_status open_journal(asx_journal *j, uint32_t chain_every) {
    return open_journal_flags(j, chain_every, 0u);
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

    /* Reads kept as a hash (SHAPE mode) cannot be served. A live POSIX
     * build serves every native kind, so the first record it refuses is
     * the read; elsewhere the spawn before it is a kind it never serves. */
    ASSERT_EQ(open_journal_flags(&j, 0u, ASX_JOURNAL_FLAG_SHAPE), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_process_spawn((int32_t)ASX_OK, 5u, 99u, 7u, 0u); /* seq 0 */
    asx_journal_note_net_read(5u, 16u, (int32_t)ASX_OK, data, 4u);    /* seq 1 */
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep),
              (int)ASX_REPLAY_E_UNSUPPORTED);
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    ASSERT_EQ(rep.seq, (uint64_t)1u);
    ASSERT_EQ(rep.expected_kind, (uint8_t)ASX_JOURNAL_KIND_NET_READ);
    ASSERT_STR_EQ(rep.what, "data stored as its hash");
#else
    ASSERT_EQ(rep.seq, (uint64_t)0u);
    ASSERT_EQ(rep.expected_kind, (uint8_t)ASX_JOURNAL_KIND_PROCESS_SPAWN);
    ASSERT_STR_EQ(rep.what, "kind is not served by the replay");
#endif

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

/* A fresh replay of the recorded script whose first call (an entropy draw
 * where the stream has a clock read) diverges, then its report. */
static asx_replay_error diverge_at_the_first_call(asx_replay_report *rep) {
    asx_replay rp;
    asx_runtime_hooks hooks;
    uint64_t v = 0u;

    if (!open_replay(&rp, rep)) return ASX_REPLAY_E_RUNTIME;
    memset(&hooks, 0, sizeof(hooks));
    if (asx_replay_hooks(&rp, &hooks) != ASX_OK) return ASX_REPLAY_E_RUNTIME;
    if (hooks.entropy.try_u64_fn(hooks.entropy.ctx, &v) != ASX_E_REPLAY_MISMATCH) {
        return ASX_REPLAY_E_RUNTIME;
    }
    return asx_replay_finish(&rp, rep);
}

/* A divergence's report carries the hindsight ring (bd-flkg): the values the
 * run observed, as the ring's JSON, when the ring holds events and the flush
 * policy allows it; a replay that is accepted, or whose ring is empty, has
 * none. */
TEST(a_divergence_carries_the_hindsight_ring_when_it_holds_events) {
    script_obs rec;
    script_obs got;
    asx_replay rp;
    asx_replay_report rep;
    asx_runtime_hooks hooks;
    asx_hindsight_policy saved = asx_hindsight_policy_active();
    asx_hindsight_policy off = saved;

    ASSERT_TRUE(record_script(0u, &rec));

    /* An empty ring: nothing to attach. */
    asx_hindsight_reset();
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)diverge_at_the_first_call(&rep), (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_TRUE(rep.hindsight == NULL);

    /* Events in it: attached, with what was logged. */
    asx_hindsight_reset();
    asx_hindsight_log(ASX_ND_CLOCK_READ, 0u, 1234u);
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)diverge_at_the_first_call(&rep), (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_TRUE(rep.hindsight != NULL);
    ASSERT_TRUE(strstr(rep.hindsight, "\"kind\":\"clock_read\"") != NULL);
    ASSERT_TRUE(strstr(rep.hindsight, "\"observed_value\":1234") != NULL);

    /* The policy says no. */
    off.flush_on_divergence = 0;
    asx_hindsight_set_policy(&off);
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)diverge_at_the_first_call(&rep), (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_TRUE(rep.hindsight == NULL);
    asx_hindsight_set_policy(&saved);

    /* An accepted replay has none, ring or no ring. */
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_TRUE(open_replay(&rp, &rep));
    memset(&hooks, 0, sizeof(hooks));
    ASSERT_EQ(asx_replay_hooks(&rp, &hooks), ASX_OK);
    script(&hooks, &got);
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    ASSERT_TRUE(rep.hindsight == NULL);
    asx_hindsight_reset();
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
/* Recovery replay: the committed prefix of a cut stream (RP4)         */
/* ------------------------------------------------------------------ */

/* A clock record is 24 + 9 bytes, a chain 24 + 48. */
#define CLOCK_REC_LEN (ASX_JOURNAL_RECORD_HEAD_BYTES + 9u)
#define CHAIN_REC_LEN (ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_PAYLOAD_BYTES)

/* `n` clock reads, a trace event after each, recorded with a chain every
 * `chain_every` records. A checkpoint is taken when a window's last read
 * is: after k reads the trace holds k - 1 events. Leaves the stream in
 * g_out; times[i] are the values the run saw. */
static int record_clocks(uint32_t n, uint32_t chain_every, uint64_t *times) {
    asx_journal j;
    asx_runtime_hooks hooks;
    uint32_t i;
    g_fake_clock = 0u;
    fake_table(&hooks);
    asx_trace_reset();
    if (open_journal(&j, chain_every) != ASX_OK) return 0;
    if (asx_journal_wrap_hooks(&j, &hooks) != ASX_OK) return 0;
    for (i = 0; i < n; i++) {
        times[i] = (uint64_t)hooks.clock.now_ns_fn(hooks.clock.ctx);
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, 0u);
    }
    asx_journal_uninstall();
    return asx_journal_finish(&j) == ASX_OK;
}

/* The same program over replay hooks, with `bias` added to the entities of
 * the events it emits (0: the recording's own program). */
static void replay_clocks(const asx_runtime_hooks *hooks, uint32_t n, uint64_t *times,
                          uint64_t bias) {
    uint32_t i;
    asx_trace_reset();
    for (i = 0; i < n; i++) {
        times[i] = (uint64_t)hooks->clock.now_ns_fn(hooks->clock.ctx);
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i + bias, 0u);
    }
}

/* Open g_out[0 .. len) as a recovery replay and fill its replay hooks. */
static asx_replay_error open_prefix_hooks(asx_replay *rp, size_t len, asx_replay_report *rep,
                                          asx_replay_prefix *pfx, asx_runtime_hooks *hooks) {
    asx_replay_identity id = ours();
    asx_replay_error err = asx_replay_open_prefix(rp, g_out, len, &id, rep, pfx);
    memset(hooks, 0, sizeof(*hooks));
    if (err == ASX_REPLAY_OK && asx_replay_hooks(rp, hooks) != ASX_OK) return ASX_REPLAY_E_RUNTIME;
    return err;
}

TEST(a_cut_stream_replays_its_committed_prefix_and_ends_at_the_cut_with_its_seq) {
    uint64_t rec_t[10];
    uint64_t got_t[10];
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_runtime_hooks hooks;
    /* Windows of 3: data 0-2, chain 3, data 4-6, chain 7, data 8-10 ... */
    size_t after_two = (size_t)ASX_JOURNAL_HEADER_BYTES + 6u * CLOCK_REC_LEN + 2u * CHAIN_REC_LEN;
    uint32_t i;

    ASSERT_TRUE(record_clocks(10u, 3u, rec_t));
    /* The process died inside the third window: 11 bytes of its first record. */
    ASSERT_EQ((int)open_prefix_hooks(&rp, after_two + 11u, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.complete, 0);
    ASSERT_EQ(pfx.committed_bytes, (uint64_t)after_two);
    ASSERT_EQ(pfx.committed_records, (uint64_t)6u);
    ASSERT_EQ(pfx.cut_seq, (uint64_t)8u); /* 6 data records and 2 chains */
    ASSERT_EQ((int)pfx.cut_reason, (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(pfx.stream_bytes, (uint64_t)(after_two + 11u));

    /* The program asks for 7 reads: 6 are committed. */
    replay_clocks(&hooks, 7u, got_t, 0u);
    for (i = 0; i < 6u; i++) { ASSERT_EQ(got_t[i], rec_t[i]); }
    ASSERT_EQ(got_t[6], (uint64_t)0u); /* the cut answers nothing */
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_CUT);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)8u);
    ASSERT_EQ(asx_replay_failure(&rp)->actual_kind, (uint8_t)ASX_JOURNAL_KIND_CLOCK);
    asx_replay_prefix_progress(&rp, &pfx);
    ASSERT_EQ(pfx.served, (uint64_t)6u);
    /* Two windows closed on a sampled read: both checkpoints held. The
     * second closed with 5 events in the trace, on its chain at seq 7. */
    ASSERT_EQ(pfx.checkpoints, (uint64_t)2u);
    ASSERT_EQ(pfx.checkpoint_seq, (uint64_t)7u);
    ASSERT_EQ(pfx.checkpoint_events, (uint64_t)5u);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_E_CUT);
    ASSERT_EQ(rep.seq, (uint64_t)8u);
}

/* The log medium of the watchdog store: g_out, append-only. */
static asx_status log_write(void *ctx, uint64_t offset, const uint8_t *bytes, uint32_t len) {
    (void)ctx;
    if (offset != (uint64_t)g_out_len || g_out_len + len > OUT_CAP) return ASX_E_INVALID_STATE;
    memcpy(g_out + g_out_len, bytes, len);
    g_out_len += len;
    return ASX_OK;
}

/* AUTOMOTIVE's persistence (bd-9kll.11.4): the stream goes to a RAM ring and
 * a watchdog checkpoint writes a bounded part of the committed prefix. A
 * process that dies after one checkpoint leaves the log cut inside a record,
 * and recovery replays the committed prefix the log holds, and only it. */
TEST(a_log_the_watchdog_store_wrote_is_cut_between_checkpoints_and_replays_its_prefix) {
    uint64_t rec_t[10];
    uint64_t got_t[10];
    asx_journal j;
    asx_journal_config cfg;
    asx_journal_wdog w;
    static uint8_t ring[4096];
    asx_runtime_hooks hooks;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    size_t first_window = (size_t)ASX_JOURNAL_HEADER_BYTES + 3u * CLOCK_REC_LEN + CHAIN_REC_LEN;
    uint32_t i;

    g_fake_clock = 0u;
    fake_table(&hooks);
    asx_trace_reset();
    asx_journal_uninstall();
    g_out_len = 0u;
    ASSERT_EQ(asx_journal_wdog_init(&w, ring, (uint32_t)sizeof(ring), log_write, NULL,
                                    (uint32_t)first_window + 11u, 0u),
              ASX_OK);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sink_fn = asx_journal_wdog_sink;
    cfg.sink_ctx = &w;
    cfg.buffer = g_stage;
    cfg.buffer_cap = (uint32_t)sizeof(g_stage);
    cfg.chain_every = 3u;
    cfg.persist = ASX_JOURNAL_PERSIST_CHAIN;
    cfg.commit_every_chains = 1u;
    cfg.commit_fn = asx_journal_wdog_commit;
    cfg.config_fingerprint = UINT64_C(0x1122334455667788);
    cfg.rust_rev_hash = REV_HASH;
    cfg.build_hash = BUILD_HASH;
    ASSERT_EQ(asx_journal_init(&j, &cfg), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    for (i = 0; i < 10u; i++) {
        rec_t[i] = (uint64_t)hooks.clock.now_ns_fn(hooks.clock.ctx);
        asx_trace_emit(ASX_TRACE_SCHED_POLL, (uint64_t)i, 0u);
    }
    asx_journal_uninstall(); /* the process dies here: no finish */

    /* Three windows are committed and in the ring; the log has nothing yet. */
    ASSERT_EQ(g_out_len, (size_t)0u);
    ASSERT_EQ(w.committed,
              (uint64_t)ASX_JOURNAL_HEADER_BYTES + 9u * CLOCK_REC_LEN + 3u * CHAIN_REC_LEN);

    /* One checkpoint writes the first window and 11 bytes of the second. */
    ASSERT_EQ(asx_journal_wdog_tick(&w, 1000u), ASX_OK);
    ASSERT_EQ(g_out_len, first_window + 11u);
    ASSERT_EQ(w.largest_write, (uint32_t)first_window + 11u);

    ASSERT_EQ((int)open_prefix_hooks(&rp, g_out_len, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.complete, 0);
    ASSERT_EQ(pfx.committed_bytes, (uint64_t)first_window);
    ASSERT_EQ(pfx.committed_records, (uint64_t)3u);
    ASSERT_EQ(pfx.cut_seq, (uint64_t)4u); /* 3 data records and 1 chain */
    ASSERT_EQ((int)pfx.cut_reason, (int)ASX_REPLAY_E_INCOMPLETE);

    replay_clocks(&hooks, 4u, got_t, 0u);
    for (i = 0; i < 3u; i++) { ASSERT_EQ(got_t[i], rec_t[i]); }
    ASSERT_EQ(got_t[3], (uint64_t)0u); /* the cut answers nothing */
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_CUT);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)4u);
    asx_replay_prefix_progress(&rp, &pfx);
    ASSERT_EQ(pfx.served, (uint64_t)3u);
    ASSERT_EQ(pfx.checkpoints, (uint64_t)1u);
}

TEST(a_program_that_ends_inside_the_prefix_is_a_cut_not_a_pass) {
    uint64_t rec_t[10];
    uint64_t got_t[10];
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_runtime_hooks hooks;
    size_t after_two = (size_t)ASX_JOURNAL_HEADER_BYTES + 6u * CLOCK_REC_LEN + 2u * CHAIN_REC_LEN;

    ASSERT_TRUE(record_clocks(10u, 3u, rec_t));
    /* All 6 committed reads asked for, then the program ends: the prefix
     * cannot show the recorded run did too. */
    ASSERT_EQ((int)open_prefix_hooks(&rp, after_two, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    ASSERT_EQ((int)pfx.cut_reason, (int)ASX_REPLAY_E_INCOMPLETE);
    replay_clocks(&hooks, 6u, got_t, 0u);
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_E_CUT);
    ASSERT_EQ(rep.seq, (uint64_t)8u);

    /* Four of the six asked for: the fifth committed record, seq 5 (seq 3
     * is a chain), was never requested. */
    ASSERT_EQ((int)open_prefix_hooks(&rp, after_two, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    replay_clocks(&hooks, 4u, got_t, 0u);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(rep.seq, (uint64_t)5u);
    ASSERT_STR_EQ(rep.what, "record never requested");
}

TEST(a_trace_checkpoint_that_differs_diverges_at_the_window_s_last_record) {
    uint64_t rec_t[10];
    uint64_t got_t[10];
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_runtime_hooks hooks;
    size_t after_two = (size_t)ASX_JOURNAL_HEADER_BYTES + 6u * CLOCK_REC_LEN + 2u * CHAIN_REC_LEN;

    ASSERT_TRUE(record_clocks(10u, 3u, rec_t));
    ASSERT_EQ((int)open_prefix_hooks(&rp, after_two, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    /* The same boundary calls, but the run's trace differs: its first window
     * closes at the third read, seq 2. */
    replay_clocks(&hooks, 6u, got_t, 100u);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)2u);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "trace checkpoint");
    asx_replay_prefix_progress(&rp, &pfx);
    ASSERT_EQ(pfx.checkpoints, (uint64_t)0u);
}

TEST(a_complete_stream_opens_as_a_prefix_and_passes) {
    uint64_t rec_t[10];
    uint64_t got_t[10];
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_runtime_hooks hooks;
    uint32_t i;

    ASSERT_TRUE(record_clocks(10u, 3u, rec_t));
    ASSERT_EQ((int)open_prefix_hooks(&rp, g_out_len, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.complete, 1);
    ASSERT_EQ(pfx.committed_records, (uint64_t)10u);
    ASSERT_EQ((int)pfx.cut_reason, (int)ASX_REPLAY_OK);
    replay_clocks(&hooks, 10u, got_t, 0u);
    for (i = 0; i < 10u; i++) { ASSERT_EQ(got_t[i], rec_t[i]); }
    asx_replay_prefix_progress(&rp, &pfx);
    /* Three full windows closed on a read; the last one, one record, was
     * closed by finish, which carries no checkpoint. */
    ASSERT_EQ(pfx.checkpoints, (uint64_t)3u);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
}

TEST(a_damaged_window_ends_the_prefix_before_it) {
    uint64_t rec_t[10];
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_runtime_hooks hooks;
    asx_replay_identity id = ours();
    /* A byte of the second window's first record. */
    size_t off = (size_t)ASX_JOURNAL_HEADER_BYTES + 3u * CLOCK_REC_LEN + CHAIN_REC_LEN +
                 ASX_JOURNAL_RECORD_HEAD_BYTES + 2u;

    ASSERT_TRUE(record_clocks(10u, 3u, rec_t));
    g_out[off] ^= 0x40u;
    /* The whole-stream contract refuses it; a recovery replay serves the
     * first window and says the rest did not verify. */
    ASSERT_EQ((int)asx_replay_open(&rp, g_out, g_out_len, &id, &rep), (int)ASX_REPLAY_E_CORRUPT);
    ASSERT_EQ((int)open_prefix_hooks(&rp, g_out_len, &rep, &pfx, &hooks), (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.complete, 0);
    ASSERT_EQ(pfx.committed_records, (uint64_t)3u);
    ASSERT_EQ(pfx.cut_seq, (uint64_t)4u);
    ASSERT_EQ((int)pfx.cut_reason, (int)ASX_REPLAY_E_CORRUPT);
}

TEST(a_stream_cut_before_its_first_chain_has_an_empty_prefix) {
    uint64_t rec_t[10];
    uint64_t got_t[10];
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_runtime_hooks hooks;

    ASSERT_TRUE(record_clocks(10u, 3u, rec_t));
    ASSERT_EQ((int)open_prefix_hooks(&rp,
                                     (size_t)ASX_JOURNAL_HEADER_BYTES + 2u * CLOCK_REC_LEN + 5u,
                                     &rep, &pfx, &hooks),
              (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.committed_records, (uint64_t)0u);
    ASSERT_EQ(pfx.cut_seq, (uint64_t)0u);
    ASSERT_EQ(pfx.committed_bytes, (uint64_t)ASX_JOURNAL_HEADER_BYTES);
    replay_clocks(&hooks, 1u, got_t, 0u);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_CUT);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)0u);

    /* A stream cut inside its header is not a stream at all. */
    ASSERT_EQ((int)open_prefix_hooks(&rp, 30u, &rep, &pfx, &hooks), (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(rep.seq, (uint64_t)0u);
}

TEST(a_recovery_replay_checks_identity_and_only_the_prefix_s_records) {
    asx_journal j;
    asx_runtime_hooks hooks;
    asx_replay rp;
    asx_replay_report rep;
    asx_replay_prefix pfx;
    asx_replay_identity id = ours();
    uint8_t data[4] = {1u, 2u, 3u, 4u};
    size_t after_one = (size_t)ASX_JOURNAL_HEADER_BYTES + 3u * CLOCK_REC_LEN + CHAIN_REC_LEN;
    /* The shaped read: 24-byte head, 8 + 4 + 4 + 4 bytes and its 8-byte hash. */
    size_t read_len = (size_t)ASX_JOURNAL_RECORD_HEAD_BYTES + 28u;
    size_t after_two = after_one + read_len + 2u * CLOCK_REC_LEN + CHAIN_REC_LEN;

    /* clock x3 (seq 0-2), chain (3), a read kept as a hash (4), clock x2
     * (5-6), chain (7). Reads kept as a hash cannot be served. */
    g_fake_clock = 0u;
    fake_table(&hooks);
    asx_trace_reset();
    ASSERT_EQ(open_journal_flags(&j, 3u, ASX_JOURNAL_FLAG_SHAPE), ASX_OK);
    ASSERT_EQ(asx_journal_wrap_hooks(&j, &hooks), ASX_OK);
    (void)hooks.clock.now_ns_fn(hooks.clock.ctx);
    (void)hooks.clock.now_ns_fn(hooks.clock.ctx);
    (void)hooks.clock.now_ns_fn(hooks.clock.ctx);
    asx_journal_note_net_read(5u, 16u, (int32_t)ASX_OK, data, 4u);
    (void)hooks.clock.now_ns_fn(hooks.clock.ctx);
    (void)hooks.clock.now_ns_fn(hooks.clock.ctx);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    /* Cut inside the second window: the unsupported read is past the cut. */
    ASSERT_EQ((int)asx_replay_open_prefix(&rp, g_out, after_one + 30u, &id, &rep, &pfx),
              (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.committed_records, (uint64_t)3u);
    /* Cut after the second chain: the read is committed, so it is refused. */
    ASSERT_EQ((int)asx_replay_open_prefix(&rp, g_out, after_two + 5u, &id, &rep, &pfx),
              (int)ASX_REPLAY_E_UNSUPPORTED);
    ASSERT_EQ(rep.seq, (uint64_t)4u);
    ASSERT_EQ(rep.expected_kind, (uint8_t)ASX_JOURNAL_KIND_NET_READ);

    /* Identity is checked as for any replay. */
    id.build_hash ^= 1u;
    ASSERT_EQ((int)asx_replay_open_prefix(&rp, g_out, after_one + 30u, &id, &rep, &pfx),
              (int)ASX_REPLAY_E_BUILD_MISMATCH);
    ASSERT_EQ((int)asx_replay_open_prefix(&rp, g_out, after_one, &id, &rep, NULL),
              (int)ASX_REPLAY_E_CORRUPT);
}

/* ------------------------------------------------------------------ */
/* A real run, recorded and replayed (live POSIX)                      */
/* ------------------------------------------------------------------ */

#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC

#include "../../../src/fs/fs_native.h"
#include "../../../src/net/net_native.h"
#include "../../../src/process/process_native.h"
#include <asx/fs/fs.h>
#include <asx/net/net.h>
#include <asx/platform/posix.h>
#include <asx/process/process.h>
#include <asx/runtime/blocking.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <asx/runtime/waker.h>
#include <asx/time/sleep.h>
#include <stdio.h>    /* snprintf */
#include <stdlib.h>   /* mkdtemp */
#include <sys/stat.h> /* stat */
#include <time.h>     /* nanosleep */

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

/* ------------------------------------------------------------------ */
/* Network, process and blocking                                       */
/*                                                                     */
/* Each program runs twice: live, with a journal recording its native  */
/* calls, then again with a replay active. The replay opens no socket  */
/* and starts no process: the adapters' real-call counters stay put.   */
/* ------------------------------------------------------------------ */

static void nap(void) {
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = 1000000L; /* 1 ms */
    (void)nanosleep(&t, NULL);
}

/* Open g_out as a replay and make the adapters serve from it. */
static int begin_replay(asx_replay *rp) {
    asx_replay_report rep;
    asx_replay_deactivate();
    if (!open_replay(rp, &rep)) return 0;
    return asx_replay_activate(rp) == ASX_OK;
}

static void clean_natives(void) {
    asx_replay_deactivate();
    asx_native_net_reset();
    asx_native_process_reset();
    asx_io_driver_shutdown();
}

/* --- served by handle and length --- */

TEST(net_results_are_served_by_handle_and_length) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    const uint64_t h = UINT64_C(0x8000000100000001);
    const uint64_t other = UINT64_C(0x8000000200000001);
    asx_journal_addr to;
    asx_journal_addr elsewhere;
    asx_journal_addr got;
    int32_t status = 0;
    uint32_t n = 0u;
    uint8_t buf[32];
    uint64_t stream = 0u;

    memset(&to, 0, sizeof(to));
    to.bytes[0] = 127u;
    to.bytes[3] = 1u;
    to.port = 4242u;
    elsewhere = to;
    elsewhere.port = 4243u;

    clean_natives();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    asx_journal_note_net_connect(&to, (int32_t)ASX_OK, h, &to);                      /* 0 */
    asx_journal_note_net_write(h, 5u, (int32_t)ASX_OK, 5u);                          /* 1 */
    asx_journal_note_net_read(h, 16u, (int32_t)ASX_E_PENDING, NULL, 0u);             /* 2 */
    asx_journal_note_net_read(h, 16u, (int32_t)ASX_OK, (const uint8_t *)"pong", 4u); /* 3 */
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);

    /* In order, by handle and length: every result as it was recorded. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_TRUE(asx_replay_serve_net_connect(&to, &status, &stream, &got));
    ASSERT_EQ(status, (int32_t)ASX_OK);
    ASSERT_EQ(stream, h);
    ASSERT_EQ(got.port, 4242u);
    ASSERT_TRUE(asx_replay_serve_net_write(h, 5u, &status, &n));
    ASSERT_EQ(status, (int32_t)ASX_OK);
    ASSERT_EQ(n, 5u);
    ASSERT_TRUE(asx_replay_serve_net_read(h, 16u, &status, buf, &n));
    ASSERT_EQ(status, (int32_t)ASX_E_PENDING);
    ASSERT_EQ(n, 0u);
    ASSERT_TRUE(asx_replay_serve_net_read(h, 16u, &status, buf, &n));
    ASSERT_EQ(status, (int32_t)ASX_OK);
    ASSERT_EQ(n, 4u);
    ASSERT_EQ(memcmp(buf, "pong", 4u), 0);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    asx_replay_deactivate();

    /* A connect to another address: the first record. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_TRUE(asx_replay_serve_net_connect(&elsewhere, &status, &stream, &got));
    ASSERT_EQ(status, (int32_t)ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)0u);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "connect address");
    asx_replay_deactivate();

    /* A write on another handle: seq 1, the write record. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_TRUE(asx_replay_serve_net_connect(&to, &status, &stream, &got));
    ASSERT_TRUE(asx_replay_serve_net_write(other, 5u, &status, &n));
    ASSERT_EQ(status, (int32_t)ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)1u);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_NET_WRITE);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "write stream handle");
    /* The report keeps the first failure whatever is asked next. */
    ASSERT_TRUE(asx_replay_serve_net_write(h, 5u, &status, &n));
    ASSERT_EQ(status, (int32_t)ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)1u);
    asx_replay_deactivate();

    /* A write of another length: seq 1. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_TRUE(asx_replay_serve_net_connect(&to, &status, &stream, &got));
    ASSERT_TRUE(asx_replay_serve_net_write(h, 6u, &status, &n));
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)1u);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "write length");
    asx_replay_deactivate();

    /* A read into a buffer of another length: seq 2. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_TRUE(asx_replay_serve_net_connect(&to, &status, &stream, &got));
    ASSERT_TRUE(asx_replay_serve_net_write(h, 5u, &status, &n));
    ASSERT_TRUE(asx_replay_serve_net_read(h, 32u, &status, buf, &n));
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)2u);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "read buffer length");
    asx_replay_deactivate();

    /* The wrong kind of request: a read where the stream has the write. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_TRUE(asx_replay_serve_net_connect(&to, &status, &stream, &got));
    ASSERT_TRUE(asx_replay_serve_net_read(h, 16u, &status, buf, &n));
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)1u);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_NET_WRITE);
    ASSERT_EQ(asx_replay_failure(&rp)->actual_kind, (uint8_t)ASX_JOURNAL_KIND_NET_READ);
    asx_replay_deactivate();
}

/* --- UDP over loopback, through the native entry points --- */

typedef struct {
    asx_status bind_a;
    asx_status bind_b;
    asx_status send;
    asx_status recv;
    asx_status close_a;
    asx_status close_b;
    uint16_t port_b;
    uint32_t sent;
    uint32_t got;
    uint32_t from_port;
    uint32_t pendings;
    uint8_t data[16];
} udp_obs;

static void udp_program(udp_obs *o) {
    asx_native_handle a;
    asx_native_handle b;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_socket_addr to;
    asx_socket_addr from;
    uint8_t buf[16];
    uint32_t i;

    memset(o, 0, sizeof(*o));
    memset(&to, 0, sizeof(to));
    memset(&from, 0, sizeof(from));
    o->bind_a = asx_native_udp_bind(&any, &a);
    o->bind_b = asx_native_udp_bind(&any, &b);
    if (o->bind_a != ASX_OK || o->bind_b != ASX_OK) return;
    (void)asx_native_local_addr(b, &to);
    o->port_b = to.port;
    o->send = asx_native_udp_send(a, (const uint8_t *)"ping", 4u, &to, &o->sent);
    for (i = 0u; i < 2000u; i++) {
        o->recv = asx_native_udp_recv(b, buf, (uint32_t)sizeof(buf), &o->got, &from);
        if (o->recv != ASX_E_PENDING) break;
        o->pendings++;
        nap();
    }
    if (o->recv == ASX_OK && o->got <= sizeof(o->data)) {
        memcpy(o->data, buf, o->got);
        o->from_port = from.port;
    }
    o->close_a = asx_native_close(a);
    o->close_b = asx_native_close(b);
}

TEST(udp_over_loopback_replays_with_no_socket_call) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    udp_obs rec;
    udp_obs got;
    uint32_t calls;

    clean_natives();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    udp_program(&rec);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    clean_natives();
    ASSERT_EQ(rec.bind_a, ASX_OK);
    ASSERT_EQ(rec.bind_b, ASX_OK);
    ASSERT_EQ(rec.send, ASX_OK);
    ASSERT_EQ(rec.recv, ASX_OK);
    ASSERT_EQ(rec.got, 4u);
    ASSERT_EQ(memcmp(rec.data, "ping", 4u), 0);
    ASSERT_TRUE(rec.port_b != 0u);
    ASSERT_TRUE(asx_native_net_real_calls() > 0u); /* it really opened sockets */

    ASSERT_TRUE(begin_replay(&rp));
    calls = asx_native_net_real_calls();
    udp_program(&got);
    ASSERT_EQ(asx_native_net_real_calls(), calls); /* and this one opened none */
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    clean_natives();

    ASSERT_EQ(got.bind_a, rec.bind_a);
    ASSERT_EQ(got.bind_b, rec.bind_b);
    ASSERT_EQ(got.port_b, rec.port_b); /* the ephemeral port the recording got */
    ASSERT_EQ(got.send, rec.send);
    ASSERT_EQ(got.sent, rec.sent);
    ASSERT_EQ(got.recv, rec.recv);
    ASSERT_EQ(got.got, rec.got);
    ASSERT_EQ(got.pendings, rec.pendings);
    ASSERT_EQ(got.from_port, rec.from_port);
    ASSERT_EQ(memcmp(got.data, rec.data, sizeof(got.data)), 0);
    ASSERT_EQ(got.close_a, rec.close_a);
    ASSERT_EQ(got.close_b, rec.close_b);
}

/* --- a child process, through the native entry points --- */

typedef struct {
    asx_status spawn;
    asx_status wait;
    asx_status release;
    asx_process_handle h;
    uint32_t pid;
    asx_process_output out;
} proc_obs;

static proc_obs g_proc_rec;
static proc_obs g_proc_got;

static void proc_program(proc_obs *o) {
    asx_process_command cmd;
    memset(o, 0, sizeof(*o));
    asx_process_command_init(&cmd, "/bin/sh");
    o->spawn = asx_process_command_arg(&cmd, "-c");
    if (o->spawn != ASX_OK) return;
    o->spawn = asx_process_command_arg(&cmd, "printf out; printf err 1>&2; exit 3");
    if (o->spawn != ASX_OK) return;
    asx_process_command_stdin(&cmd, ASX_PROCESS_STDIO_NULL);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stderr(&cmd, ASX_PROCESS_STDIO_PIPED);
    o->spawn = asx_native_process_spawn(&cmd, &o->h);
    if (o->spawn != ASX_OK) return;
    o->pid = asx_native_process_id(o->h);
    o->wait = asx_native_process_wait_with_output(o->h, &o->out);
    o->release = asx_native_process_release(o->h);
}

TEST(a_child_replays_its_output_and_exit_with_no_process_started) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    uint32_t calls;

    clean_natives();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    proc_program(&g_proc_rec);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    clean_natives();
    ASSERT_EQ(g_proc_rec.spawn, ASX_OK);
    ASSERT_EQ(g_proc_rec.wait, ASX_OK);
    ASSERT_EQ(g_proc_rec.out.exit_code, 3);
    ASSERT_EQ(g_proc_rec.out.stdout_len, 3u);
    ASSERT_EQ(memcmp(g_proc_rec.out.stdout_data, "out", 3u), 0);
    ASSERT_EQ(g_proc_rec.out.stderr_len, 3u);
    ASSERT_EQ(memcmp(g_proc_rec.out.stderr_data, "err", 3u), 0);
    ASSERT_TRUE(asx_native_process_real_calls() > 0u);

    ASSERT_TRUE(begin_replay(&rp));
    calls = asx_native_process_real_calls();
    proc_program(&g_proc_got);
    ASSERT_EQ(asx_native_process_real_calls(), calls);
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    clean_natives();

    ASSERT_EQ(g_proc_got.spawn, g_proc_rec.spawn);
    /* The table slot is the program's; the handle's generation is this
     * process's history, which the recording's need not equal. */
    ASSERT_EQ(g_proc_got.h.slot, g_proc_rec.h.slot);
    ASSERT_EQ(g_proc_got.pid, g_proc_rec.pid); /* the recorded pid, no child ran */
    ASSERT_EQ(g_proc_got.wait, g_proc_rec.wait);
    ASSERT_EQ(g_proc_got.out.exit_code, g_proc_rec.out.exit_code);
    ASSERT_EQ(g_proc_got.out.term_signal, g_proc_rec.out.term_signal);
    ASSERT_EQ(g_proc_got.out.success, g_proc_rec.out.success);
    ASSERT_EQ(g_proc_got.out.stdout_len, g_proc_rec.out.stdout_len);
    ASSERT_EQ(memcmp(g_proc_got.out.stdout_data, "out", 3u), 0);
    ASSERT_EQ(g_proc_got.out.stderr_len, g_proc_rec.out.stderr_len);
    ASSERT_EQ(memcmp(g_proc_got.out.stderr_data, "err", 3u), 0);
    ASSERT_EQ(g_proc_got.release, g_proc_rec.release);
}

typedef struct {
    asx_status spawn;
    asx_status stdin_write;
    asx_status exit_wait;
    uint32_t written;
    uint32_t out_len;
    uint32_t polls;
    uint8_t out[64];
    asx_process_exit_status exit;
} cat_obs;

static void cat_program(cat_obs *o) {
    asx_process_command cmd;
    asx_process_handle h;
    static const uint8_t msg[] = {'h', 'e', 'l', 'l', 'o', '\n'};
    uint32_t i;
    memset(o, 0, sizeof(*o));
    asx_process_command_init(&cmd, "/bin/cat");
    asx_process_command_stdin(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stderr(&cmd, ASX_PROCESS_STDIO_NULL);
    o->spawn = asx_native_process_spawn(&cmd, &h);
    if (o->spawn != ASX_OK) return;
    while (o->written < (uint32_t)sizeof(msg)) {
        uint32_t n = 0u;
        o->stdin_write = asx_native_process_stdin_write(h, msg + o->written,
                                                        (uint32_t)sizeof(msg) - o->written, &n);
        if (o->stdin_write == ASX_E_PENDING) {
            nap();
            continue;
        }
        if (o->stdin_write != ASX_OK) break;
        o->written += n;
    }
    (void)asx_native_process_stdin_close(h);
    for (i = 0u; i < 5000u; i++) {
        uint32_t n = 0u;
        asx_status st = asx_native_process_read(h, ASX_NATIVE_PROCESS_STDOUT, &o->out[o->out_len],
                                                (uint32_t)sizeof(o->out) - o->out_len, &n);
        o->polls++;
        if (st == ASX_E_PENDING) {
            nap();
            continue;
        }
        if (st != ASX_OK || n == 0u) break; /* EOF, or a failure */
        o->out_len += n;
    }
    for (i = 0u; i < 5000u; i++) {
        o->exit_wait = asx_native_process_poll_status(h, &o->exit, 0);
        o->polls++;
        if (o->exit_wait != ASX_E_PENDING) break;
        nap();
    }
    (void)asx_native_process_release(h);
}

TEST(a_childs_stdin_and_stdout_replay_with_the_same_polling) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    cat_obs rec;
    cat_obs got;
    uint32_t calls;

    clean_natives();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    cat_program(&rec);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    clean_natives();
    ASSERT_EQ(rec.spawn, ASX_OK);
    ASSERT_EQ(rec.written, 6u);
    ASSERT_EQ(rec.out_len, 6u);
    ASSERT_EQ(memcmp(rec.out, "hello\n", 6u), 0);
    ASSERT_EQ(rec.exit_wait, ASX_OK);
    ASSERT_EQ(rec.exit.code, 0);

    ASSERT_TRUE(begin_replay(&rp));
    calls = asx_native_process_real_calls();
    cat_program(&got);
    ASSERT_EQ(asx_native_process_real_calls(), calls);
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    clean_natives();

    ASSERT_EQ(got.spawn, rec.spawn);
    ASSERT_EQ(got.written, rec.written);
    ASSERT_EQ(got.out_len, rec.out_len);
    ASSERT_EQ(memcmp(got.out, rec.out, rec.out_len), 0);
    ASSERT_EQ(got.polls, rec.polls); /* the same PENDING answers, in order */
    ASSERT_EQ(got.exit_wait, rec.exit_wait);
    ASSERT_EQ(got.exit.state, rec.exit.state);
    ASSERT_EQ(got.exit.code, rec.exit.code);
}

TEST(a_spawn_of_another_command_is_a_divergence_at_the_spawn_record) {
    asx_journal j;
    asx_replay rp;
    asx_process_command cmd;
    proc_obs o;

    clean_natives();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    proc_program(&o);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    clean_natives();
    ASSERT_EQ(o.spawn, ASX_OK);

    ASSERT_TRUE(begin_replay(&rp));
    asx_process_command_init(&cmd, "/bin/sh");
    ASSERT_EQ(asx_process_command_arg(&cmd, "-c"), ASX_OK);
    ASSERT_EQ(asx_process_command_arg(&cmd, "exit 0"), ASX_OK); /* not the recorded script */
    asx_process_command_stdin(&cmd, ASX_PROCESS_STDIO_NULL);
    asx_process_command_stdout(&cmd, ASX_PROCESS_STDIO_PIPED);
    asx_process_command_stderr(&cmd, ASX_PROCESS_STDIO_PIPED);
    ASSERT_EQ(asx_native_process_spawn(&cmd, &o.h), ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, (uint64_t)0u);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_PROCESS_SPAWN);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "spawn command");
    clean_natives();
}

/* --- the filesystem: results recorded as the native adapter got them and
 * served back to a program whose world has no such files (bd-9kll.11.2) --- */

typedef struct {
    asx_status st[24];
    uint32_t nst;
    uint32_t wrote;
    uint32_t read_n;
    uint32_t read_at_n;
    uint8_t read_buf[16];
    uint8_t read_at_buf[16];
    uint64_t seek_pos;
    uint64_t size_open;
    uint64_t size_cut;
    uint32_t entries;
    uint32_t name_sum;
    uint32_t kind_sum;
} fs_obs;

static void fs_step(fs_obs *o, asx_status st) {
    if (o->nst < (uint32_t)(sizeof(o->st) / sizeof(o->st[0]))) o->st[o->nst++] = st;
}

/* The program: a directory with two files, written, read, cut, renamed,
 * listed, and removed again, with the refusals a world without the files
 * would give kept in the results. `dir` is its working directory. */
static void fs_program(fs_obs *o, const char *dir) {
    asx_fs_path pd;
    asx_fs_path pa;
    asx_fs_path pb;
    asx_fs_path pc;
    asx_fs_path nope;
    char tmp[ASX_FS_PATH_MAX];
    asx_file_handle f;
    asx_fs_metadata meta;
    asx_fs_dir_handle d;
    asx_fs_dir_entry ent;
    int has = 0;
    uint32_t i;

    memset(o, 0, sizeof(*o));
    fs_step(o, asx_fs_set_backend(ASX_FS_BACKEND_NATIVE));
    fs_step(o, asx_fs_path_from_cstr(&pd, dir));
    (void)snprintf(tmp, sizeof(tmp), "%s/a.txt", dir);
    fs_step(o, asx_fs_path_from_cstr(&pa, tmp));
    (void)snprintf(tmp, sizeof(tmp), "%s/b.txt", dir);
    fs_step(o, asx_fs_path_from_cstr(&pb, tmp));
    (void)snprintf(tmp, sizeof(tmp), "%s/c.txt", dir);
    fs_step(o, asx_fs_path_from_cstr(&pc, tmp));
    (void)snprintf(tmp, sizeof(tmp), "%s/missing.txt", dir);
    fs_step(o, asx_fs_path_from_cstr(&nope, tmp));

    fs_step(o, asx_fs_dir_create_all(&pd));
    fs_step(o, asx_fs_file_open(&f, &pa,
                                (uint32_t)(ASX_FS_OPEN_READ | ASX_FS_OPEN_WRITE |
                                           ASX_FS_OPEN_CREATE | ASX_FS_OPEN_TRUNC)));
    fs_step(o, asx_fs_file_write(f, (const uint8_t *)"hello world", 11u, &o->wrote));
    fs_step(o, asx_fs_file_seek(f, 0, ASX_FS_SEEK_SET, &o->seek_pos));
    fs_step(o, asx_fs_file_read(f, o->read_buf, (uint32_t)sizeof(o->read_buf), &o->read_n));
    fs_step(o, asx_fs_file_read_at(f, 6u, o->read_at_buf, 5u, &o->read_at_n));
    fs_step(o, asx_fs_file_metadata(f, &meta));
    o->size_open = meta.size;
    fs_step(o, asx_fs_file_set_len(f, 5u));
    fs_step(o, asx_fs_file_sync(f));
    fs_step(o, asx_fs_file_close(f));
    fs_step(o, asx_fs_metadata_query(&pa, &meta));
    o->size_cut = meta.size;

    fs_step(o, asx_fs_file_open(&f, &pb, (uint32_t)(ASX_FS_OPEN_WRITE | ASX_FS_OPEN_CREATE)));
    fs_step(o, asx_fs_file_close(f));
    fs_step(o, asx_fs_rename(&pa, &pc));
    fs_step(o, asx_fs_file_open(&f, &nope, (uint32_t)ASX_FS_OPEN_READ)); /* NOT_FOUND */
    fs_step(o, asx_fs_remove_dir(&pd));                                  /* not empty */

    fs_step(o, asx_fs_dir_open(&d, &pd));
    for (i = 0u; i < 8u; i++) {
        has = 0;
        if (asx_fs_dir_next(d, &ent, &has) != ASX_OK || !has) break;
        o->entries++;
        o->name_sum += (uint32_t)(uint8_t)ent.name[0];
        o->kind_sum += (uint32_t)ent.kind;
    }
    fs_step(o, asx_fs_dir_close(d));

    fs_step(o, asx_fs_remove_file(&pc));
    fs_step(o, asx_fs_remove_file(&pb));
    fs_step(o, asx_fs_remove_dir(&pd));
}

/* A scratch root the test owns (mkdtemp) and the program's working
 * directory inside it, `dir`: the path is the same in the recording and the
 * replay, and the program removes everything it makes. The test removes the
 * root. */
static int fs_roots(char *root, char *dir, size_t cap) {
    size_t n = (size_t)snprintf(root, cap, "/tmp/asx-fs-replay-");
    size_t i;
    if (n + 7u > cap) return 0;
    for (i = 0u; i < 6u; i++) root[n + i] = 'X'; /* mkdtemp's template */
    root[n + 6u] = '\0';
    if (mkdtemp(root) == NULL) return 0;
    (void)snprintf(dir, cap, "%s/work", root);
    return 1;
}

static void fs_clean(void) {
    asx_replay_deactivate();
    asx_native_fs_reset();
    asx_fs_reset();
}

TEST(a_filesystem_run_replays_from_the_journal_in_a_world_without_its_files) {
    asx_journal j;
    asx_replay rp;
    asx_replay_report rep;
    fs_obs rec;
    fs_obs got;
    char root[ASX_FS_PATH_MAX];
    char dir[ASX_FS_PATH_MAX];
    struct stat sb;
    uint32_t calls;
    uint32_t i;

    ASSERT_TRUE(fs_roots(root, dir, sizeof(dir)));
    fs_clean();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    fs_program(&rec, dir);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    fs_clean();

    /* What the real filesystem gave. */
    ASSERT_EQ(rec.wrote, 11u);
    ASSERT_EQ(rec.seek_pos, (uint64_t)0u);
    ASSERT_EQ(rec.read_n, 11u);
    ASSERT_EQ(memcmp(rec.read_buf, "hello world", 11u), 0);
    ASSERT_EQ(rec.read_at_n, 5u);
    ASSERT_EQ(memcmp(rec.read_at_buf, "world", 5u), 0);
    ASSERT_EQ(rec.size_open, (uint64_t)11u);
    ASSERT_EQ(rec.size_cut, (uint64_t)5u);
    ASSERT_EQ(rec.entries, 2u);
    for (i = 0u; i < rec.nst; i++) {
        /* Only the two refusals are not OK. */
        if (rec.st[i] != ASX_OK) {
            ASSERT_TRUE(rec.st[i] == ASX_E_NOT_FOUND || rec.st[i] == ASX_E_INVALID_STATE);
        }
    }
    ASSERT_TRUE(asx_native_fs_real_calls() > 0u);
    ASSERT_TRUE(stat(dir, &sb) != 0); /* the program removed what it made */
    ASSERT_EQ(rmdir(root), 0);        /* and the root has nothing in it */

    /* Replayed: the same results, no call reaches the OS, nothing is
     * created, and the program's own removals are served too. */
    ASSERT_TRUE(begin_replay(&rp));
    calls = asx_native_fs_real_calls();
    fs_program(&got, dir);
    ASSERT_EQ(asx_native_fs_real_calls(), calls);
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);
    fs_clean();
    ASSERT_TRUE(stat(dir, &sb) != 0);
    ASSERT_EQ(got.nst, rec.nst);
    ASSERT_EQ(memcmp(&got, &rec, sizeof(got)), 0);
}

TEST(a_filesystem_call_that_differs_diverges_naming_the_path_or_the_field) {
    asx_journal j;
    asx_replay rp;
    fs_obs o;
    asx_fs_path other;
    char root[ASX_FS_PATH_MAX];
    char dir[ASX_FS_PATH_MAX];
    char tmp[ASX_FS_PATH_MAX];
    asx_file_handle f;

    ASSERT_TRUE(fs_roots(root, dir, sizeof(dir)));
    fs_clean();
    ASSERT_EQ(open_journal(&j, 0u), ASX_OK);
    ASSERT_EQ(asx_journal_install(&j), ASX_OK);
    fs_program(&o, dir);
    asx_journal_uninstall();
    ASSERT_EQ(asx_journal_finish(&j), ASX_OK);
    fs_clean();
    ASSERT_EQ(rmdir(root), 0);

    /* The recording's first filesystem call is the directory's creation:
     * another directory is another path. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_NATIVE), ASX_OK);
    (void)snprintf(tmp, sizeof(tmp), "%s-other", dir);
    ASSERT_EQ(asx_fs_path_from_cstr(&other, tmp), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create_all(&other), ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_FS);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "filesystem path or handle");
    fs_clean();

    /* The same path through another call: the call is what differs. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_fs_path_from_cstr(&other, dir), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create(&other), ASX_E_REPLAY_MISMATCH);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "filesystem call");
    fs_clean();

    /* A file opened with other flags than the recorded open: its argument. */
    ASSERT_TRUE(begin_replay(&rp));
    ASSERT_EQ(asx_fs_set_backend(ASX_FS_BACKEND_NATIVE), ASX_OK);
    ASSERT_EQ(asx_fs_path_from_cstr(&other, dir), ASX_OK);
    ASSERT_EQ(asx_fs_dir_create_all(&other), ASX_OK); /* served as recorded */
    (void)snprintf(tmp, sizeof(tmp), "%s/a.txt", dir);
    ASSERT_EQ(asx_fs_path_from_cstr(&other, tmp), ASX_OK);
    ASSERT_EQ(asx_fs_file_open(&f, &other, (uint32_t)(ASX_FS_OPEN_READ)), ASX_E_REPLAY_MISMATCH);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "filesystem argument");
    fs_clean();
}

/* --- generic live programs: recorded under the POSIX hooks, replayed ---
 * into a fresh runtime whose hooks are the replay's --- */

typedef asx_status (*live_program_fn)(void *obs);

static int record_program_chain(live_program_fn fn, void *obs, uint32_t chain_every) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_journal j;
    int ok;

    clean_natives();
    if (asx_posix_hooks_install(&hooks) != ASX_OK) return 0;
    if (open_journal(&j, chain_every) != ASX_OK) return 0;
    if (asx_journal_wrap_hooks(&j, &hooks) != ASX_OK) return 0;
    if (asx_runtime_config_init(&cfg) != ASX_OK) {
        asx_journal_uninstall();
        return 0;
    }
    if (asx_runtime_init(&g_rt, &cfg, &hooks) != ASX_OK) {
        asx_journal_uninstall();
        return 0;
    }
    ok = fn(obs) == ASX_OK;
    asx_runtime_shutdown(&g_rt);
    asx_journal_uninstall();
    return asx_journal_finish(&j) == ASX_OK && ok;
}

static int record_program(live_program_fn fn, void *obs) {
    return record_program_chain(fn, obs, 0u);
}

/* Run `fn` into a fresh runtime under the open replay `rp`, with the
 * adapters serving from it too. */
static int replay_program(asx_replay *rp, live_program_fn fn, void *obs) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;

    asx_replay_deactivate();
    if (asx_runtime_config_init(&cfg) != ASX_OK) return 0;
    if (asx_runtime_hooks_init(&hooks) != ASX_OK) return 0;
    if (asx_replay_hooks(rp, &hooks) != ASX_OK) return 0;
    if (asx_replay_activate(rp) != ASX_OK) return 0;
    if (asx_runtime_init(&g_rt, &cfg, &hooks) != ASX_OK) {
        asx_replay_deactivate();
        return 0;
    }
    st = fn(obs);
    asx_runtime_shutdown(&g_rt);
    asx_replay_deactivate();
    return st == ASX_OK;
}

/* --- a TCP round trip between two tasks --- */

typedef struct {
    asx_co_state co;
    asx_tcp_listener listener;
    asx_tcp_stream conn;
    uint8_t buf[16];
    uint32_t len;
    uint32_t sent;
} tcp_server;

static asx_status tcp_server_poll(void *ud, asx_task_id self) {
    tcp_server *s = (tcp_server *)ud;
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
} tcp_client;

static asx_status tcp_client_poll(void *ud, asx_task_id self) {
    tcp_client *c = (tcp_client *)ud;
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

typedef struct {
    uint16_t port;
    asx_status run_st;
    uint32_t reply_len;
    uint8_t reply[16];
    uint64_t digest;
} tcp_obs;

static asx_status tcp_program(void *obs) {
    tcp_obs *o = (tcp_obs *)obs;
    asx_region_id region;
    asx_task_id ts;
    asx_task_id tc;
    tcp_server *s = NULL;
    tcp_client *c = NULL;
    asx_socket_addr any = asx_socket_addr_loopback(0);
    asx_budget budget;
    asx_status st;

    memset(o, 0, sizeof(*o));
    st = asx_region_open(&region);
    if (st != ASX_OK) return st;
    st = asx_task_spawn_captured(region, tcp_server_poll, (uint32_t)sizeof(tcp_server), NULL, &ts,
                                 (void **)&s);
    if (st != ASX_OK) return st;
    st = asx_tcp_listener_bind(&s->listener, &any);
    if (st != ASX_OK) return st;
    st = asx_task_spawn_captured(region, tcp_client_poll, (uint32_t)sizeof(tcp_client), NULL, &tc,
                                 (void **)&c);
    if (st != ASX_OK) return st;
    st = asx_tcp_listener_local_addr(s->listener, &c->addr);
    if (st != ASX_OK) return st;
    o->port = c->addr.port;
    budget = asx_budget_from_polls(500u);
    o->run_st = asx_scheduler_run(region, &budget);
    o->reply_len = c->reply_len;
    memcpy(o->reply, c->reply, c->reply_len <= sizeof(o->reply) ? c->reply_len : 0u);
    o->digest = asx_trace_digest();
    return asx_tcp_listener_close(s->listener);
}

TEST(a_tcp_round_trip_replays_with_no_socket_opened) {
    asx_replay rp;
    asx_replay_report rep;
    tcp_obs rec;
    tcp_obs got;
    uint32_t calls;

    ASSERT_TRUE(record_program(tcp_program, &rec));
    ASSERT_EQ(rec.run_st, ASX_OK);
    ASSERT_EQ(rec.reply_len, 5u);
    ASSERT_EQ(memcmp(rec.reply, "ping\n", 5u), 0);
    ASSERT_TRUE(rec.port != 0u);
    ASSERT_TRUE(asx_native_net_real_calls() > 0u); /* it really used sockets */

    ASSERT_TRUE(open_replay(&rp, &rep));
    calls = asx_native_net_real_calls();
    ASSERT_TRUE(replay_program(&rp, tcp_program, &got));
    ASSERT_EQ(asx_native_net_real_calls(), calls); /* the replay opened none */
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);

    ASSERT_EQ(got.run_st, rec.run_st);
    ASSERT_EQ(got.port, rec.port); /* the ephemeral port the recording got */
    ASSERT_EQ(got.reply_len, rec.reply_len);
    ASSERT_EQ(memcmp(got.reply, rec.reply, rec.reply_len), 0);
    ASSERT_EQ(got.digest, rec.digest);
}

/* --- a blocking job that resolves a name, woken through its waker --- */

typedef struct {
    asx_waker waker;
    asx_blocking_handle job;
    int have_waker;
    int submitted;
    uint64_t value;
    uint32_t waits;
    asx_status result; /* ASX_E_PENDING until the task ends */
    /* What the job wrote through its argument, on the pool thread. */
    asx_status resolve_status;
    uint32_t addr_count;
    uint16_t addr_port;
} job_waiter;

/* Added to the job's result: a test makes a replay's job return what the
 * recording's did not. */
static uint64_t g_job_bias;

static uint64_t job_body(void *user_data) {
    job_waiter *w = (job_waiter *)user_data;
    asx_resolve_options opts;
    asx_resolve_result res;
    uint32_t i;
    /* Long enough that the scheduler is already waiting on the reactor
     * when the completion arrives, so the recording holds the wait that
     * the completion ended. */
    for (i = 0u; i < 30u; i++) nap();
    asx_resolve_options_init(&opts, 8443u);
    memset(&res, 0, sizeof(res));
    w->resolve_status = asx_native_resolve_job("127.0.0.1", &opts, &res);
    w->addr_count = res.count;
    w->addr_port = res.count > 0u ? res.addrs[0].port : 0u;
    return UINT64_C(0x600DF00D) + res.count + g_job_bias;
}

static asx_status job_waiter_poll(void *ud, asx_task_id self) {
    job_waiter *w = (job_waiter *)ud;
    asx_status st;
    if (!w->submitted) {
        st = asx_waker_register(self, &w->waker);
        if (st != ASX_OK) {
            w->result = st;
            return ASX_OK;
        }
        w->have_waker = 1;
        st = asx_spawn_blocking(job_body, w, &w->waker, &w->job);
        if (st != ASX_OK) {
            w->result = st;
            return ASX_OK;
        }
        w->submitted = 1;
    }
    st = asx_blocking_get_result(&w->job, &w->value);
    if (st == ASX_E_PENDING) {
        w->waits++;
        st = asx_task_park(self);
        if (st != ASX_OK) {
            w->result = st;
            return ASX_OK;
        }
        return ASX_E_PENDING;
    }
    if (w->have_waker) asx_waker_deregister(&w->waker);
    w->result = st;
    return ASX_OK;
}

typedef struct {
    asx_status run_st;
    asx_status result;
    uint64_t value;
    asx_status resolve_status;
    uint32_t addr_count;
    uint16_t addr_port;
    uint64_t digest;
} job_obs;

static asx_status job_program(void *obs) {
    job_obs *o = (job_obs *)obs;
    asx_region_id region;
    asx_task_id t;
    job_waiter *w = NULL;
    asx_budget budget;
    asx_status st;

    memset(o, 0, sizeof(*o));
    st = asx_region_open(&region);
    if (st != ASX_OK) return st;
    st = asx_task_spawn_captured(region, job_waiter_poll, (uint32_t)sizeof(job_waiter), NULL, &t,
                                 (void **)&w);
    if (st != ASX_OK) return st;
    w->result = ASX_E_PENDING;
    budget = asx_budget_from_polls(500u);
    o->run_st = asx_scheduler_run(region, &budget);
    o->result = w->result;
    o->value = w->value;
    o->resolve_status = w->resolve_status;
    o->addr_count = w->addr_count;
    o->addr_port = w->addr_port;
    o->digest = asx_trace_digest();
    return ASX_OK;
}

TEST(a_blocking_job_is_run_again_where_its_completion_was_recorded) {
    asx_replay rp;
    asx_replay_report rep;
    job_obs rec;
    job_obs got;
    uint32_t calls;

    ASSERT_TRUE(record_program(job_program, &rec));
    ASSERT_EQ(rec.run_st, ASX_OK);
    ASSERT_EQ(rec.result, ASX_OK);
    ASSERT_EQ(rec.resolve_status, ASX_OK);
    ASSERT_EQ(rec.addr_count, 1u);
    ASSERT_EQ(rec.addr_port, 8443u);
    ASSERT_EQ(rec.value, UINT64_C(0x600DF00D) + 1u);
    ASSERT_TRUE(asx_native_net_real_calls() > 0u); /* the pool really resolved */

    ASSERT_TRUE(open_replay(&rp, &rep));
    calls = asx_native_net_real_calls();
    ASSERT_TRUE(replay_program(&rp, job_program, &got));
    ASSERT_EQ(asx_native_net_real_calls(), calls); /* the job's lookup was served */
    ASSERT_TRUE(asx_replay_failure(&rp) == NULL);
    ASSERT_EQ((int)asx_replay_finish(&rp, &rep), (int)ASX_REPLAY_OK);

    ASSERT_EQ(got.run_st, rec.run_st);
    ASSERT_EQ(got.result, rec.result);
    ASSERT_EQ(got.value, rec.value);
    ASSERT_EQ(got.resolve_status, rec.resolve_status);
    ASSERT_EQ(got.addr_count, rec.addr_count);
    ASSERT_EQ(got.addr_port, rec.addr_port);
    ASSERT_EQ(got.digest, rec.digest);
}

/* A program that asks for entropy first, where the stream has a listen. */
static asx_status entropy_first_program(void *obs) {
    uint64_t v = 0u;
    (void)obs;
    (void)asx_runtime_random_u64(&v);
    return ASX_OK;
}

TEST(asx_journal_replay_runs_a_program_against_a_stream_in_one_call) {
    asx_replay_report rep;
    asx_replay_identity id = ours();
    asx_status ps = ASX_E_INVALID_STATE;
    tcp_obs rec;
    tcp_obs got;
    tcp_obs other;

    ASSERT_TRUE(record_program(tcp_program, &rec));
    ASSERT_EQ(rec.run_st, ASX_OK);

    /* The program, against its own stream: accepted, same results. */
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)asx_journal_replay(g_out, g_out_len, &id, NULL, tcp_program, &got, &ps, &rep),
              (int)ASX_REPLAY_OK);
    ASSERT_EQ(ps, ASX_OK);
    ASSERT_EQ((int)rep.error, (int)ASX_REPLAY_OK);
    ASSERT_EQ(got.port, rec.port);
    ASSERT_EQ(got.reply_len, rec.reply_len);
    ASSERT_EQ(memcmp(got.reply, rec.reply, rec.reply_len), 0);
    ASSERT_EQ(got.digest, rec.digest);

    /* Another program: refused at its first request, naming the kinds. */
    ASSERT_EQ((int)asx_journal_replay(g_out, g_out_len, &id, NULL, entropy_first_program, &other,
                                      &ps, &rep),
              (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(rep.actual_kind, (uint8_t)ASX_JOURNAL_KIND_ENTROPY);
    ASSERT_STR_EQ(rep.what, "kind");

    /* A stream of another build is refused before any program runs. */
    id.build_hash ^= 1u;
    ps = ASX_E_INVALID_STATE;
    ASSERT_EQ((int)asx_journal_replay(g_out, g_out_len, &id, NULL, tcp_program, &got, &ps, &rep),
              (int)ASX_REPLAY_E_BUILD_MISMATCH);
    ASSERT_EQ(ps, ASX_OK);
    ASSERT_STR_EQ(rep.what, "build_hash");

    /* No program is a run that could not start. */
    ASSERT_EQ((int)asx_journal_replay(g_out, g_out_len, NULL, NULL, NULL, NULL, &ps, &rep),
              (int)ASX_REPLAY_E_RUNTIME);
    ASSERT_EQ(ps, ASX_E_INVALID_ARGUMENT);
}

/* Two clock reads; and one clock read followed by an entropy draw. */
static asx_status clock_twice_program(void *obs) {
    asx_time t = 0u;
    (void)obs;
    (void)asx_runtime_now_ns(&t);
    (void)asx_runtime_now_ns(&t);
    return ASX_OK;
}

static asx_status clock_then_entropy_program(void *obs) {
    asx_time t = 0u;
    uint64_t v = 0u;
    (void)obs;
    (void)asx_runtime_now_ns(&t);
    (void)asx_runtime_random_u64(&v);
    return ASX_OK;
}

/* A divergence the program meets while the runtime is up carries the
 * hindsight ring read before its shutdown clears it (bd-flkg): here the
 * clock read the replay served before the draw the stream does not have. */
TEST(a_diverged_one_call_replay_reports_the_hindsight_it_was_served) {
    asx_replay_report rep;
    asx_replay_identity id = ours();
    asx_status ps = ASX_E_INVALID_STATE;

    ASSERT_TRUE(record_program(clock_twice_program, NULL));

    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)asx_journal_replay(g_out, g_out_len, &id, NULL, clock_twice_program, NULL, &ps,
                                      &rep),
              (int)ASX_REPLAY_OK);
    ASSERT_TRUE(rep.hindsight == NULL);

    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)asx_journal_replay(g_out, g_out_len, &id, NULL, clock_then_entropy_program, NULL,
                                      &ps, &rep),
              (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(rep.actual_kind, (uint8_t)ASX_JOURNAL_KIND_ENTROPY);
    ASSERT_TRUE(rep.hindsight != NULL);
    ASSERT_TRUE(strstr(rep.hindsight, "\"kind\":\"clock_read\"") != NULL);
}

TEST(a_blocking_result_that_differs_from_the_recorded_one_is_reported_at_its_seq) {
    asx_replay rp;
    asx_replay_report rep;
    job_obs rec;
    job_obs got;
    uint64_t seq = 0u;
    size_t off = 0u;

    ASSERT_TRUE(record_program(job_program, &rec));
    ASSERT_EQ(rec.result, ASX_OK);
    ASSERT_TRUE(locate(g_out, g_out_len, (uint8_t)ASX_JOURNAL_KIND_BLOCKING_DONE, 0u, &seq, &off));

    /* Replay the program with a job that returns another value than the
     * stream's completion says: the divergence is that record. */
    ASSERT_TRUE(open_replay(&rp, &rep));
    g_job_bias = 5u;
    ASSERT_TRUE(replay_program(&rp, job_program, &got));
    g_job_bias = 0u;
    ASSERT_TRUE(asx_replay_failure(&rp) != NULL);
    ASSERT_EQ((int)asx_replay_failure(&rp)->error, (int)ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(asx_replay_failure(&rp)->seq, seq);
    ASSERT_EQ(asx_replay_failure(&rp)->expected_kind, (uint8_t)ASX_JOURNAL_KIND_BLOCKING_DONE);
    ASSERT_STR_EQ(asx_replay_failure(&rp)->what, "blocking job result");
}

/* --- a cut live run, and the scheduler run a failed replay drives --- */

/* How many chains the stream in g_out has. */
static uint32_t chains_in_stream(void) {
    asx_journal_reader r;
    asx_journal_record jr;
    if (asx_journal_reader_init(&r, g_out, g_out_len) != ASX_JOURNAL_READ_RECORD) return 0u;
    for (;;) {
        if (asx_journal_reader_next(&r, &jr) != ASX_JOURNAL_READ_RECORD) break;
    }
    return (uint32_t)r.chains;
}

/* The end of the committed prefix of g_out once `want` chains verified, and
 * the data records in it. */
static int prefix_after_chains(uint32_t want, size_t *pos, uint64_t *records) {
    asx_journal_reader r;
    asx_journal_record jr;
    if (asx_journal_reader_init(&r, g_out, g_out_len) != ASX_JOURNAL_READ_RECORD) return 0;
    while (r.chains < (uint64_t)want) {
        if (asx_journal_reader_next(&r, &jr) != ASX_JOURNAL_READ_RECORD) return 0;
    }
    *pos = r.chain_end_pos;
    *records = r.chain_end_records;
    return 1;
}

TEST(a_cut_live_run_replays_its_committed_prefix_and_ends_at_the_cut) {
    asx_replay_report rep;
    asx_replay_identity id = ours();
    asx_replay_prefix pfx;
    asx_status ps = ASX_E_INVALID_STATE;
    tcp_obs rec;
    tcp_obs got;
    size_t cut = 0u;
    uint64_t records = 0u;
    uint32_t total;

    /* A chain every 4 records, so the run has several windows. */
    ASSERT_TRUE(record_program_chain(tcp_program, &rec, 4u));
    ASSERT_EQ(rec.run_st, ASX_OK);
    total = chains_in_stream();
    ASSERT_TRUE(total >= 3u);

    /* The whole stream through the recovery path: accepted as complete,
     * the same results and digest, and the trace checkpoints held. */
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)asx_journal_replay_prefix(g_out, g_out_len, &id, NULL, tcp_program, &got, &ps,
                                             &rep, &pfx),
              (int)ASX_REPLAY_OK);
    ASSERT_EQ(pfx.complete, 1);
    ASSERT_TRUE(pfx.checkpoints > 0u);
    ASSERT_EQ(got.reply_len, rec.reply_len);
    ASSERT_EQ(got.digest, rec.digest);

    /* Killed mid-run: the prefix is the windows that were committed, and
     * the replay of the program ends at the first record that was not. */
    ASSERT_TRUE(prefix_after_chains(total / 2u, &cut, &records));
    memset(&rep, 0xA5, sizeof(rep));
    ASSERT_EQ((int)asx_journal_replay_prefix(g_out, cut + 9u, &id, NULL, tcp_program, &got, &ps,
                                             &rep, &pfx),
              (int)ASX_REPLAY_E_CUT);
    ASSERT_EQ(pfx.complete, 0);
    ASSERT_EQ(pfx.committed_bytes, (uint64_t)cut);
    ASSERT_EQ(pfx.committed_records, records);
    ASSERT_EQ((int)pfx.cut_reason, (int)ASX_REPLAY_E_INCOMPLETE);
    ASSERT_EQ(rep.seq, pfx.cut_seq);
    ASSERT_EQ(pfx.served, records); /* every committed record was reproduced */
    ASSERT_TRUE(pfx.checkpoints > 0u);
}

/* A task that treats every refusal as "try again": polled, it draws
 * entropy and yields without parking, so it is runnable for ever. With a
 * limit it completes at that poll: the recording is the same program, ended
 * early, so the replay serves its first polls from the stream and the
 * program then asks for more than the recording did. (Not a stream of bare
 * entropy draws: a live scheduler reads the clock hook at the start of
 * every round, so such a stream diverges at its first record.) */
static uint32_t g_spin_limit;
static uint32_t g_spin_polls;

static asx_status spin_poll(void *ud, asx_task_id self) {
    uint64_t v = 0u;
    (void)ud;
    (void)self;
    g_spin_polls++;
    (void)asx_runtime_random_u64(&v);
    if (g_spin_limit != 0u && g_spin_polls >= g_spin_limit) return ASX_OK;
    return ASX_E_PENDING;
}

static asx_status spin_program(void *obs) {
    asx_status *run = (asx_status *)obs;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget;
    asx_status st;

    st = asx_region_open(&region);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(region, spin_poll, NULL, &task);
    if (st != ASX_OK) return st;
    /* A generous poll budget as a backstop: the replay's failure must end
     * the run long before it. */
    budget = asx_budget_from_polls(100000u);
    *run = asx_scheduler_run(region, &budget);
    return ASX_OK;
}

TEST(a_failed_replay_ends_the_scheduler_run_it_is_driving) {
    asx_replay_report rep;
    asx_replay_identity id = ours();
    asx_status ps = ASX_E_INVALID_STATE;
    asx_status run = ASX_OK;
    asx_status rec_run = ASX_E_INVALID_STATE;
    asx_replay_error err;

    /* Record the spinner ending at its third poll. */
    g_spin_limit = 3u;
    g_spin_polls = 0u;
    ASSERT_TRUE(record_program(spin_program, &rec_run));
    ASSERT_EQ(rec_run, ASX_OK);
    ASSERT_EQ(g_spin_polls, 3u);

    /* Replay the same program, which never ends: its first polls are
     * served, then it asks for what the recording never did and the replay
     * fails (which request that is, the stream's end or a different kind
     * of boundary call, depends on what the scheduler reads after the third
     * poll). The scheduler must end the run at its next round rather than
     * poll the task for ever, long before the poll budget. */
    g_spin_limit = 0u;
    g_spin_polls = 0u;
    err = asx_journal_replay(g_out, g_out_len, &id, NULL, spin_program, &run, &ps, &rep);
    ASSERT_TRUE(err == ASX_REPLAY_E_EXHAUSTED || err == ASX_REPLAY_E_DIVERGED);
    ASSERT_EQ(ps, ASX_OK);
    ASSERT_EQ(run, ASX_E_REPLAY_MISMATCH);
    ASSERT_TRUE(g_spin_polls >= 3u);
    ASSERT_TRUE(g_spin_polls < 1000u);
    g_spin_limit = 0u;
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
    RUN_TEST(a_divergence_carries_the_hindsight_ring_when_it_holds_events);
    RUN_TEST(a_changed_argument_diverges_with_the_field_named);
    RUN_TEST(a_request_after_the_last_record_is_exhausted_at_the_end_seq);
    RUN_TEST(records_the_program_never_asked_for_are_reported_at_finish);
    RUN_TEST(signal_drains_and_resolutions_are_served_in_order);
    RUN_TEST(a_resolution_of_another_host_diverges);
    RUN_TEST(a_replay_is_not_activated_while_a_journal_records);
    RUN_TEST(a_cut_stream_replays_its_committed_prefix_and_ends_at_the_cut_with_its_seq);
    RUN_TEST(a_log_the_watchdog_store_wrote_is_cut_between_checkpoints_and_replays_its_prefix);
    RUN_TEST(a_program_that_ends_inside_the_prefix_is_a_cut_not_a_pass);
    RUN_TEST(a_trace_checkpoint_that_differs_diverges_at_the_window_s_last_record);
    RUN_TEST(a_complete_stream_opens_as_a_prefix_and_passes);
    RUN_TEST(a_damaged_window_ends_the_prefix_before_it);
    RUN_TEST(a_stream_cut_before_its_first_chain_has_an_empty_prefix);
    RUN_TEST(a_recovery_replay_checks_identity_and_only_the_prefix_s_records);
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
    RUN_TEST(a_live_run_replays_with_identical_results_and_digest);
    RUN_TEST(a_live_stream_refuses_a_tampered_byte_and_a_reordered_call);
    RUN_TEST(net_results_are_served_by_handle_and_length);
    RUN_TEST(udp_over_loopback_replays_with_no_socket_call);
    RUN_TEST(a_child_replays_its_output_and_exit_with_no_process_started);
    RUN_TEST(a_childs_stdin_and_stdout_replay_with_the_same_polling);
    RUN_TEST(a_spawn_of_another_command_is_a_divergence_at_the_spawn_record);
    RUN_TEST(a_filesystem_run_replays_from_the_journal_in_a_world_without_its_files);
    RUN_TEST(a_filesystem_call_that_differs_diverges_naming_the_path_or_the_field);
    RUN_TEST(a_tcp_round_trip_replays_with_no_socket_opened);
    RUN_TEST(a_blocking_job_is_run_again_where_its_completion_was_recorded);
    RUN_TEST(asx_journal_replay_runs_a_program_against_a_stream_in_one_call);
    RUN_TEST(a_diverged_one_call_replay_reports_the_hindsight_it_was_served);
    RUN_TEST(a_blocking_result_that_differs_from_the_recorded_one_is_reported_at_its_seq);
    RUN_TEST(a_cut_live_run_replays_its_committed_prefix_and_ends_at_the_cut);
    RUN_TEST(a_failed_replay_ends_the_scheduler_run_it_is_driving);
#endif

    TEST_REPORT();
    return test_failures;
}
