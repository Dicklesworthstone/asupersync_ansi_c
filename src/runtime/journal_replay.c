/*
 * journal_replay.c — replay a native boundary journal (bd-9kll.11.2, RP2)
 *
 * The contract and the API are documented in
 * include/asx/runtime/journal_replay.h. This file validates a stream up
 * front, then answers boundary requests from it in order and reports the
 * first request that does not match.
 *
 * Guarantee Impact: replay serves recorded results to the hooks and
 * adapters a run installs it into; with no replay active every adapter
 * branch is one load and a branch, and no runtime state depends on it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/runtime/journal_replay.h>
#include <string.h>

static asx_replay *g_replay;

/* ------------------------------------------------------------------ */
/* Failure                                                             */
/* ------------------------------------------------------------------ */

static void report_set(asx_replay_report *rep, asx_replay_error error, uint64_t seq,
                       uint64_t first_seq, uint64_t trace_seq, uint8_t expected, uint8_t actual,
                       const char *what) {
    rep->error = error;
    rep->seq = seq;
    rep->first_seq = first_seq;
    rep->trace_seq = trace_seq;
    rep->expected_kind = expected;
    rep->actual_kind = actual;
    rep->what = what;
}

/* Record the first failure; later ones are consequences. */
static void fail(asx_replay *r, asx_replay_error error, uint64_t seq, uint64_t trace_seq,
                 uint8_t expected, uint8_t actual, const char *what) {
    if (r->failed) return;
    r->failed = 1;
    report_set(&r->failure, error, seq, seq, trace_seq, expected, actual, what);
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

/* The kinds this replay answers. */
static int kind_served(uint8_t kind) {
    return kind == (uint8_t)ASX_JOURNAL_KIND_CLOCK || kind == (uint8_t)ASX_JOURNAL_KIND_ENTROPY ||
           kind == (uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT ||
           kind == (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL ||
           kind == (uint8_t)ASX_JOURNAL_KIND_RESOLVE || kind == (uint8_t)ASX_JOURNAL_KIND_SIGNAL;
}

asx_replay_error asx_replay_open(asx_replay *r, const uint8_t *bytes, size_t len,
                                 const asx_replay_identity *identity, asx_replay_report *report) {
    asx_journal_reader v;
    asx_journal_record rec;
    asx_journal_read_result rr;
    asx_replay_report local;
    asx_replay_report unsupported;
    int have_unsupported = 0;

    memset(&local, 0, sizeof(local));
    memset(&unsupported, 0, sizeof(unsupported));
    if (r == NULL || bytes == NULL) {
        report_set(&local, ASX_REPLAY_E_CORRUPT, 0u, 0u, 0u, 0u, 0u, "no stream");
        goto refused;
    }
    memset(r, 0, sizeof(*r));

    rr = asx_journal_reader_init(&v, bytes, len);
    if (rr == ASX_JOURNAL_READ_TRUNCATED) {
        report_set(&local, ASX_REPLAY_E_INCOMPLETE, 0u, 0u, 0u, 0u, 0u,
                   "stream ends inside the header");
        goto refused;
    }
    if (rr != ASX_JOURNAL_READ_RECORD) {
        report_set(&local, ASX_REPLAY_E_CORRUPT, 0u, 0u, 0u, 0u, 0u, "header");
        goto refused;
    }
    if (identity != NULL && identity->rust_rev_hash != 0u &&
        v.rust_rev_hash != identity->rust_rev_hash) {
        report_set(&local, ASX_REPLAY_E_BUILD_MISMATCH, 0u, 0u, 0u, 0u, 0u, "rust_rev_hash");
        goto refused;
    }
    if (identity != NULL && identity->build_hash != 0u && v.build_hash != identity->build_hash) {
        report_set(&local, ASX_REPLAY_E_BUILD_MISMATCH, 0u, 0u, 0u, 0u, 0u, "build_hash");
        goto refused;
    }

    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: every pass consumes one record of the buffer");
        rr = asx_journal_reader_next(&v, &rec);
        if (rr == ASX_JOURNAL_READ_RECORD) {
            if (!have_unsupported) {
                const char *why = NULL;
                if (!kind_served(rec.kind)) {
                    why = "kind is not served by the replay";
                } else if ((rec.flags & ASX_JOURNAL_REC_TRUNC) != 0u) {
                    why = "poll stored truncated";
                } else if ((rec.flags & ASX_JOURNAL_REC_SHAPE) != 0u) {
                    why = "data stored as its hash";
                }
                if (why != NULL) {
                    have_unsupported = 1;
                    report_set(&unsupported, ASX_REPLAY_E_UNSUPPORTED, rec.seq, rec.seq,
                               rec.trace_seq, rec.kind, 0u, why);
                }
            }
            continue;
        }
        if (rr == ASX_JOURNAL_READ_END) break;
        if (rr == ASX_JOURNAL_READ_TRUNCATED) {
            report_set(&local, ASX_REPLAY_E_INCOMPLETE, v.fail_seq, v.fail_seq, 0u, 0u, 0u,
                       "stream ends before its END record");
        } else {
            report_set(&local, ASX_REPLAY_E_CORRUPT, v.fail_seq, v.fail_first_seq, 0u, 0u, 0u,
                       "sequence, chain or END totals");
        }
        goto refused;
    }
    if (v.end_dropped != 0u || v.end_status != 0) {
        /* The END record is the last seq issued. */
        report_set(&local, ASX_REPLAY_E_INCOMPLETE, v.next_seq - 1u, v.next_seq - 1u, 0u,
                   (uint8_t)ASX_JOURNAL_KIND_END, 0u, "the recorder dropped records or failed");
        goto refused;
    }
    if (have_unsupported) {
        local = unsupported;
        goto refused;
    }

    (void)asx_journal_reader_init(&r->reader, bytes, len);
    r->opened = 1;
    if (report != NULL) memset(report, 0, sizeof(*report));
    return ASX_REPLAY_OK;

refused:
    if (report != NULL) *report = local;
    return local.error;
}

/* ------------------------------------------------------------------ */
/* Serving                                                             */
/* ------------------------------------------------------------------ */

/* Make the next data record current. 1 with r->cur set; 0 with the failure
 * recorded (the journal is exhausted, or the stream is not what it was
 * when it was opened). */
static int peek(asx_replay *r, uint8_t asked) {
    if (r->failed) return 0;
    if (!r->have_cur) {
        asx_journal_read_result rr = asx_journal_reader_next(&r->reader, &r->cur);
        if (rr == ASX_JOURNAL_READ_RECORD) {
            r->have_cur = 1;
        } else if (rr == ASX_JOURNAL_READ_END) {
            fail(r, ASX_REPLAY_E_EXHAUSTED, r->reader.next_seq - 1u, 0u,
                 (uint8_t)ASX_JOURNAL_KIND_END, asked, "the journal has no more records");
            return 0;
        } else {
            fail(r, ASX_REPLAY_E_CORRUPT, r->reader.fail_seq, 0u, 0u, asked,
                 "stream changed since it was opened");
            return 0;
        }
    }
    return 1;
}

/* The next record must be a `kind` with at least `min_payload` bytes. */
static int expect(asx_replay *r, uint8_t kind, uint32_t min_payload) {
    if (!peek(r, kind)) return 0;
    if (r->cur.kind != kind) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq, r->cur.kind, kind, "kind");
        return 0;
    }
    if (r->cur.payload_len < min_payload) {
        fail(r, ASX_REPLAY_E_CORRUPT, r->cur.seq, r->cur.trace_seq, kind, kind,
             "record shorter than its kind's payload");
        return 0;
    }
    return 1;
}

static void consume(asx_replay *r) {
    r->have_cur = 0;
    r->served++;
}

static asx_time replay_now(void *ctx) {
    asx_replay *r = (asx_replay *)ctx;
    uint64_t v;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_CLOCK, 9u)) return 0;
    v = asx_journal_get_u64(r->cur.payload + 1);
    consume(r);
    return (asx_time)v;
}

static uint64_t replay_random(void *ctx) {
    asx_replay *r = (asx_replay *)ctx;
    uint64_t v;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_ENTROPY, 13u)) return 0u;
    v = asx_journal_get_u64(r->cur.payload + 5);
    consume(r);
    return v;
}

static asx_status replay_try(void *ctx, uint64_t *out) {
    asx_replay *r = (asx_replay *)ctx;
    asx_status st;
    uint64_t v;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_ENTROPY, 13u)) return ASX_E_REPLAY_MISMATCH;
    st = (asx_status)(int32_t)asx_journal_get_u32(r->cur.payload + 1);
    v = asx_journal_get_u64(r->cur.payload + 5);
    consume(r);
    if (st == ASX_OK && out != NULL) *out = v;
    return st;
}

/* A wait of form `api` (0 live, 1 ghost) with argument `arg` (timeout or
 * logical step). The recorded form may differ (the run is in another
 * build mode): then only the kind is held to the stream. */
static asx_status replay_wait_common(asx_replay *r, uint8_t api, uint64_t arg, uint32_t *ready) {
    const uint8_t *p;
    asx_status st;
    uint32_t n;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT, 17u)) return ASX_E_REPLAY_MISMATCH;
    p = r->cur.payload;
    if (p[0] == api && asx_journal_get_u64(p + 1) != arg) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT, (uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT,
             api == 0u ? "wait timeout" : "wait logical step");
        return ASX_E_REPLAY_MISMATCH;
    }
    st = (asx_status)(int32_t)asx_journal_get_u32(p + 9);
    n = asx_journal_get_u32(p + 13);
    consume(r);
    if (ready != NULL) *ready = n;
    return st;
}

static asx_status replay_wait(void *ctx, uint32_t timeout_ms, uint32_t *ready_count) {
    return replay_wait_common((asx_replay *)ctx, 0u, (uint64_t)timeout_ms, ready_count);
}

static asx_status replay_ghost_wait(void *ctx, uint64_t logical_step, uint32_t *ready_count) {
    return replay_wait_common((asx_replay *)ctx, 1u, logical_step, ready_count);
}

static asx_status replay_poll(void *ctx, uint32_t timeout_ms, asx_reactor_event *events,
                              uint32_t max_events, uint32_t *out_count) {
    asx_replay *r = (asx_replay *)ctx;
    const uint8_t *p;
    asx_status st;
    uint32_t n;
    uint32_t i;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL, 12u)) return ASX_E_REPLAY_MISMATCH;
    p = r->cur.payload;
    if (asx_journal_get_u32(p) != timeout_ms) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL, (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL,
             "poll timeout");
        return ASX_E_REPLAY_MISMATCH;
    }
    st = (asx_status)(int32_t)asx_journal_get_u32(p + 4);
    n = asx_journal_get_u32(p + 8);
    if ((r->cur.payload_len - 12u) / 12u != n || (r->cur.payload_len - 12u) % 12u != 0u) {
        fail(r, ASX_REPLAY_E_CORRUPT, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL, (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL,
             "poll event count");
        return ASX_E_REPLAY_MISMATCH;
    }
    if (n > max_events || (n > 0u && events == NULL)) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL, (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL,
             "poll room for the recorded events");
        return ASX_E_REPLAY_MISMATCH;
    }
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= max_events");
        events[i].token = asx_journal_get_u64(p + 12u + (size_t)i * 12u);
        events[i].ready = asx_journal_get_u32(p + 20u + (size_t)i * 12u);
    }
    consume(r);
    if (out_count != NULL) *out_count = n;
    return st;
}

/* Registration is not an input: the replay's reactor holds no fds and the
 * recorded events carry the tokens the driver armed. */
static asx_status replay_register(void *ctx, int fd, uint32_t interest, uint64_t token) {
    (void)ctx;
    (void)fd;
    (void)interest;
    (void)token;
    return ASX_OK;
}

static void replay_deregister(void *ctx, int fd) {
    (void)ctx;
    (void)fd;
}

static void replay_notify(void *ctx) { (void)ctx; }

asx_status asx_replay_hooks(asx_replay *r, asx_runtime_hooks *hooks) {
    if (r == NULL || hooks == NULL || !r->opened) return ASX_E_INVALID_ARGUMENT;
    hooks->clock.ctx = r;
    hooks->clock.now_ns_fn = replay_now;
    hooks->clock.logical_now_ns_fn = replay_now;
    hooks->entropy.ctx = r;
    hooks->entropy.random_u64_fn = replay_random;
    hooks->entropy.try_u64_fn = replay_try;
    hooks->reactor.ctx = r;
    hooks->reactor.wait_fn = replay_wait;
    hooks->reactor.ghost_wait_fn = replay_ghost_wait;
    hooks->reactor.register_fn = replay_register;
    hooks->reactor.deregister_fn = replay_deregister;
    hooks->reactor.poll_fn = replay_poll;
    hooks->reactor.notify_fn = replay_notify;
    hooks->deterministic_seeded_prng = 1u;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Activation, outcome                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_replay_activate(asx_replay *r) {
    if (r == NULL || !r->opened) return ASX_E_INVALID_ARGUMENT;
    if ((g_replay != NULL && g_replay != r) || asx_journal_active()) return ASX_E_INVALID_STATE;
    g_replay = r;
    return ASX_OK;
}

void asx_replay_deactivate(void) { g_replay = NULL; }

int asx_replay_active(void) { return g_replay != NULL; }

const asx_replay_report *asx_replay_failure(const asx_replay *r) {
    return r != NULL && r->failed ? &r->failure : NULL;
}

uint64_t asx_replay_served(const asx_replay *r) { return r != NULL ? r->served : 0u; }

asx_replay_error asx_replay_finish(asx_replay *r, asx_replay_report *report) {
    asx_replay_report none;
    memset(&none, 0, sizeof(none));
    if (r == NULL || !r->opened) {
        report_set(&none, ASX_REPLAY_E_CORRUPT, 0u, 0u, 0u, 0u, 0u, "replay not opened");
        if (report != NULL) *report = none;
        return ASX_REPLAY_E_CORRUPT;
    }
    if (!r->failed && !r->have_cur) {
        asx_journal_read_result rr = asx_journal_reader_next(&r->reader, &r->cur);
        if (rr == ASX_JOURNAL_READ_RECORD) {
            r->have_cur = 1;
        } else if (rr != ASX_JOURNAL_READ_END) {
            fail(r, ASX_REPLAY_E_CORRUPT, r->reader.fail_seq, 0u, 0u, 0u,
                 "stream changed since it was opened");
        }
    }
    if (!r->failed && r->have_cur) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq, r->cur.kind, 0u,
             "record never requested");
    }
    if (r->failed) {
        if (report != NULL) *report = r->failure;
        return r->failure.error;
    }
    if (report != NULL) *report = none;
    return ASX_REPLAY_OK;
}

/* ------------------------------------------------------------------ */
/* Adapter serving                                                     */
/* ------------------------------------------------------------------ */

int asx_replay_serve_signal(uint32_t *counts, uint32_t kinds) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    uint32_t i;
    uint32_t n;
    if (r == NULL) return 0;
    for (i = 0; i < kinds; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: one pass over the caller's kinds");
        counts[i] = 0u;
    }
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_SIGNAL, 1u)) return 1;
    p = r->cur.payload;
    n = p[0];
    if (r->cur.payload_len != 1u + n * 5u) {
        fail(r, ASX_REPLAY_E_CORRUPT, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_SIGNAL, (uint8_t)ASX_JOURNAL_KIND_SIGNAL, "signal count");
        return 1;
    }
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= 255 recorded kinds");
        uint32_t kind = p[1u + (size_t)i * 5u];
        if (kind < kinds) counts[kind] = asx_journal_get_u32(p + 2u + (size_t)i * 5u);
    }
    consume(r);
    return 1;
}

int asx_replay_serve_resolve(uint64_t name_hash, int32_t *status, asx_journal_addr *addrs,
                             uint32_t cap, uint32_t *count) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    uint32_t n;
    uint32_t i;
    if (r == NULL) return 0;
    *count = 0u;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_RESOLVE, 16u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p) != name_hash) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_RESOLVE, (uint8_t)ASX_JOURNAL_KIND_RESOLVE, "resolve host");
        return 1;
    }
    n = asx_journal_get_u32(p + 12);
    if (r->cur.payload_len != 16u + n * 19u) {
        fail(r, ASX_REPLAY_E_CORRUPT, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_RESOLVE, (uint8_t)ASX_JOURNAL_KIND_RESOLVE,
             "resolve address count");
        return 1;
    }
    if (n > cap) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_RESOLVE, (uint8_t)ASX_JOURNAL_KIND_RESOLVE,
             "room for the recorded addresses");
        return 1;
    }
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= cap");
        const uint8_t *a = p + 16u + (size_t)i * 19u;
        addrs[i].family = a[0];
        memcpy(addrs[i].bytes, a + 1, 16u);
        addrs[i].port = asx_journal_get_u16(a + 17);
    }
    *status = (int32_t)asx_journal_get_u32(p + 8);
    *count = n;
    consume(r);
    return 1;
}
