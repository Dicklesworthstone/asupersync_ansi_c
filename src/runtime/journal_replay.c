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

#include "runtime_internal.h"
#include <asx/runtime/blocking.h>
#include <asx/runtime/hindsight.h>
#include <asx/runtime/journal_replay.h>
#include <asx/runtime/rt.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* The native adapters exist, and a live build asks them, only here. */
#if defined(ASX_PROFILE_POSIX) && !ASX_DETERMINISTIC
#define REPLAY_NATIVE 1
#else
#define REPLAY_NATIVE 0
#endif

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
    rep->hindsight = NULL;
}

/* The hindsight ring as it stood when a diverged replay's program returned
 * (bd-flkg): what the replay served the run, for telling a recorded value the
 * program no longer asks for from a runtime that changed. One buffer, for the
 * one replay that can be active; asx_replay_report.hindsight points here. */
static asx_hindsight_flush_buffer g_replay_hindsight;

/* Flush the ring into the report of a failed replay that diverged. Called
 * on the thread that ran the program, while the runtime is up (its shutdown
 * clears the ring); a ring with nothing in it or a flush policy that says no
 * leaves the report without one. */
static void capture_hindsight(asx_replay *r) {
    if (!r->failed || r->failure.hindsight != NULL) return;
    if (r->failure.error != ASX_REPLAY_E_DIVERGED && r->failure.error != ASX_REPLAY_E_EXHAUSTED) {
        return;
    }
    if (asx_hindsight_flush_on_divergence(NULL, &g_replay_hindsight) == ASX_OK) {
        r->failure.hindsight = g_replay_hindsight.data;
    }
}

/* Record the first failure; later ones are consequences. */
static void fail(asx_replay *r, asx_replay_error error, uint64_t seq, uint64_t trace_seq,
                 uint8_t expected, uint8_t actual, const char *what) {
    if (r->failed) return;
    r->failed = 1;
    report_set(&r->failure, error, seq, seq, trace_seq, expected, actual, what);
}

/* The current record is not what the program asked for. */
static void diverge(asx_replay *r, const char *what) {
    fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq, r->cur.kind, r->cur.kind, what);
}

/* The current record is shorter or longer than its kind allows. */
static void malformed(asx_replay *r, const char *what) {
    fail(r, ASX_REPLAY_E_CORRUPT, r->cur.seq, r->cur.trace_seq, r->cur.kind, r->cur.kind, what);
}

/* ------------------------------------------------------------------ */
/* Open                                                                */
/* ------------------------------------------------------------------ */

/* The kinds this replay answers. */
static int kind_served(uint8_t kind) {
    if (kind == (uint8_t)ASX_JOURNAL_KIND_CLOCK || kind == (uint8_t)ASX_JOURNAL_KIND_ENTROPY ||
        kind == (uint8_t)ASX_JOURNAL_KIND_REACTOR_WAIT ||
        kind == (uint8_t)ASX_JOURNAL_KIND_REACTOR_POLL ||
        kind == (uint8_t)ASX_JOURNAL_KIND_RESOLVE || kind == (uint8_t)ASX_JOURNAL_KIND_SIGNAL) {
        return 1;
    }
#if REPLAY_NATIVE
    if (kind == (uint8_t)ASX_JOURNAL_KIND_NET_ACCEPT ||
        kind == (uint8_t)ASX_JOURNAL_KIND_NET_CONNECT ||
        kind == (uint8_t)ASX_JOURNAL_KIND_NET_READ || kind == (uint8_t)ASX_JOURNAL_KIND_NET_WRITE ||
        kind == (uint8_t)ASX_JOURNAL_KIND_UDP_RECV || kind == (uint8_t)ASX_JOURNAL_KIND_UDP_SEND ||
        kind == (uint8_t)ASX_JOURNAL_KIND_PROCESS_SPAWN ||
        kind == (uint8_t)ASX_JOURNAL_KIND_PROCESS_EXIT ||
        kind == (uint8_t)ASX_JOURNAL_KIND_NET_LISTEN ||
        kind == (uint8_t)ASX_JOURNAL_KIND_UDP_BIND || kind == (uint8_t)ASX_JOURNAL_KIND_NET_CTL ||
        kind == (uint8_t)ASX_JOURNAL_KIND_PROCESS_WRITE ||
        kind == (uint8_t)ASX_JOURNAL_KIND_PROCESS_READ) {
        return 1;
    }
#if ASX_HAS_BLOCKING_SURFACE
    if (kind == (uint8_t)ASX_JOURNAL_KIND_BLOCKING_DONE) return 1;
#endif
#endif
    return 0;
}

/* Open `bytes`. With `prefix` (recovery replay) a stream without a valid
 * END is not refused: the committed prefix, through the last chain that
 * verified, is what the replay serves, and *prefix says what was found. */
static asx_replay_error replay_open_impl(asx_replay *r, const uint8_t *bytes, size_t len,
                                         const asx_replay_identity *identity,
                                         asx_replay_report *report, asx_replay_prefix *prefix) {
    asx_journal_reader v;
    asx_journal_record rec;
    asx_journal_read_result rr;
    asx_replay_report local;
    asx_replay_report unsupported;
    int have_unsupported = 0;
    int ended = 0;
    size_t serve_len = len;
    uint64_t cut_seq = 0u;
    asx_replay_error cut_reason = ASX_REPLAY_OK;

    memset(&local, 0, sizeof(local));
    memset(&unsupported, 0, sizeof(unsupported));
    if (prefix != NULL) memset(prefix, 0, sizeof(*prefix));
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
                } else if ((rec.flags & ASX_JOURNAL_REC_OFFTHREAD) != 0u &&
                           rec.kind != (uint8_t)ASX_JOURNAL_KIND_RESOLVE &&
                           rec.kind != (uint8_t)ASX_JOURNAL_KIND_BLOCKING_DONE) {
                    why = "off-thread record the replay cannot attribute to a job";
                }
                if (why != NULL) {
                    have_unsupported = 1;
                    report_set(&unsupported, ASX_REPLAY_E_UNSUPPORTED, rec.seq, rec.seq,
                               rec.trace_seq, rec.kind, 0u, why);
                }
            }
            continue;
        }
        if (rr == ASX_JOURNAL_READ_END) {
            ended = 1;
            break;
        }
        if (prefix != NULL) {
            /* A cut or damaged tail: the prefix is what verified. */
            cut_reason =
                rr == ASX_JOURNAL_READ_TRUNCATED ? ASX_REPLAY_E_INCOMPLETE : ASX_REPLAY_E_CORRUPT;
            serve_len = v.chain_end_pos;
            cut_seq = v.chain_end_seq;
            break;
        }
        if (rr == ASX_JOURNAL_READ_TRUNCATED) {
            report_set(&local, ASX_REPLAY_E_INCOMPLETE, v.fail_seq, v.fail_seq, 0u, 0u, 0u,
                       "stream ends before its END record");
        } else {
            report_set(&local, ASX_REPLAY_E_CORRUPT, v.fail_seq, v.fail_first_seq, 0u, 0u, 0u,
                       "sequence, chain or END totals");
        }
        goto refused;
    }
    if (ended && (v.end_dropped != 0u || v.end_status != 0)) {
        /* The END record is the last seq issued. */
        report_set(&local, ASX_REPLAY_E_INCOMPLETE, v.next_seq - 1u, v.next_seq - 1u, 0u,
                   (uint8_t)ASX_JOURNAL_KIND_END, 0u, "the recorder dropped records or failed");
        goto refused;
    }
    /* Only a record the replay would serve can make the stream unreplayable:
     * in a cut stream those are the ones before the cut. */
    if (have_unsupported && (ended || unsupported.seq < cut_seq)) {
        local = unsupported;
        goto refused;
    }

    (void)asx_journal_reader_init(&r->reader, bytes, serve_len);
    r->opened = 1;
    if (prefix != NULL) {
        r->prefix = 1;
        r->prefix_cut = !ended;
        r->cut_seq = cut_seq;
        prefix->complete = ended;
        prefix->committed_bytes = (uint64_t)serve_len;
        prefix->committed_records = ended ? v.data_records : v.chain_end_records;
        prefix->cut_seq = ended ? v.next_seq : cut_seq;
        prefix->cut_reason = cut_reason;
        prefix->stream_bytes = (uint64_t)len;
    }
    if (report != NULL) memset(report, 0, sizeof(*report));
    return ASX_REPLAY_OK;

refused:
    if (report != NULL) *report = local;
    return local.error;
}

asx_replay_error asx_replay_open(asx_replay *r, const uint8_t *bytes, size_t len,
                                 const asx_replay_identity *identity, asx_replay_report *report) {
    return replay_open_impl(r, bytes, len, identity, report, NULL);
}

asx_replay_error asx_replay_open_prefix(asx_replay *r, const uint8_t *bytes, size_t len,
                                        const asx_replay_identity *identity,
                                        asx_replay_report *report, asx_replay_prefix *prefix) {
    if (prefix == NULL) {
        asx_replay_report local;
        memset(&local, 0, sizeof(local));
        report_set(&local, ASX_REPLAY_E_CORRUPT, 0u, 0u, 0u, 0u, 0u, "no prefix result");
        if (report != NULL) *report = local;
        return local.error;
    }
    return replay_open_impl(r, bytes, len, identity, report, prefix);
}

void asx_replay_prefix_progress(const asx_replay *r, asx_replay_prefix *prefix) {
    if (r == NULL || prefix == NULL) return;
    prefix->served = r->served;
    prefix->checkpoints = r->cp_verified;
    prefix->checkpoint_seq = r->cp_seq;
    prefix->checkpoint_events = r->cp_events;
    prefix->checkpoint_digest = r->cp_digest;
}

/* ------------------------------------------------------------------ */
/* Serving                                                             */
/* ------------------------------------------------------------------ */

/* The record just consumed closed a chain window whose last record was
 * taken on the runtime thread: the chain stored the trace position and
 * rolling digest the recording had at that moment, and this run, at the
 * same moment, must have the same. Recovery replay only: the recording's
 * run never reached an END to compare digests at. (The reader is
 * positioned right after the record, so the chain, if any, is next.) */
static void check_checkpoint(asx_replay *r) {
    const uint8_t *head;
    const uint8_t *trace;
    uint64_t events;
    uint64_t digest;
    if (!r->prefix || r->failed) return;
    if (r->reader.len - r->reader.pos <
        ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_PAYLOAD_BYTES) {
        return;
    }
    head = r->reader.bytes + r->reader.pos;
    if (head[4] != (uint8_t)ASX_JOURNAL_KIND_CHAIN) return;
    if ((head[5] & (uint8_t)ASX_JOURNAL_REC_TRACE_CHECKPOINT) == 0u) return;
    trace = head + ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_TRACE_OFFSET;
    events = asx_journal_get_u64(trace);
    digest = asx_journal_get_u64(trace + 8);
    if (asx_trace_emitted_total() != events || asx_trace_digest() != digest) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq, r->cur.kind, r->cur.kind,
             "trace checkpoint");
        return;
    }
    r->cp_verified++;
    r->cp_seq = asx_journal_get_u64(head + 8);
    r->cp_events = events;
    r->cp_digest = digest;
}

static void consume(asx_replay *r) {
    r->last_seq = r->cur.seq;
    r->last_trace_seq = r->cur.trace_seq;
    r->last_kind = r->cur.kind;
    r->have_cur = 0;
    r->served++;
    check_checkpoint(r);
}

/* A blocking job's completion record was reached: run the job again, here,
 * with its boundary calls served from the records held for it, and hold
 * its result to the recorded one. `rec` is already out of the stream. */
static void deliver_done(asx_replay *r, const asx_journal_record *rec) {
#if ASX_HAS_BLOCKING_SURFACE
    const uint8_t *p = rec->payload;
    uint64_t got = 0u;
    uint64_t want;
    uint32_t slot;
    asx_status st;

    if (rec->payload_len < 14u) {
        fail(r, ASX_REPLAY_E_CORRUPT, rec->seq, rec->trace_seq, rec->kind, rec->kind,
             "blocking completion record");
        return;
    }
    slot = asx_journal_get_u32(p);
    want = asx_journal_get_u64(p + 6);
    r->last_seq = rec->seq;
    r->last_trace_seq = rec->trace_seq;
    r->last_kind = rec->kind;
    r->served++;
    r->in_job = 1;
    r->job_seq = rec->seq;
    st = asx_blocking_replay_complete(slot, &got);
    r->in_job = 0;
    if (r->failed) return;
    if (st != ASX_OK) {
        fail(r, ASX_REPLAY_E_DIVERGED, rec->seq, rec->trace_seq, rec->kind, rec->kind,
             "completion of a blocking job that is not pending");
    } else if (got != want) {
        fail(r, ASX_REPLAY_E_DIVERGED, rec->seq, rec->trace_seq, rec->kind, rec->kind,
             "blocking job result");
    }
#else
    fail(r, ASX_REPLAY_E_UNSUPPORTED, rec->seq, rec->trace_seq, rec->kind, 0u,
         "blocking surface is not built");
#endif
}

/* The current record was made off the runtime thread. A blocking
 * completion runs its job; a resolution is held until that job asks for
 * it. Either way the record leaves the stream's main order. */
static void take_offthread(asx_replay *r) {
    asx_journal_record rec = r->cur;
    r->have_cur = 0;
    if (rec.kind == (uint8_t)ASX_JOURNAL_KIND_BLOCKING_DONE) {
        deliver_done(r, &rec);
        return;
    }
    if (rec.kind == (uint8_t)ASX_JOURNAL_KIND_RESOLVE) {
        asx_replay_held *h;
        if (r->held_count >= ASX_REPLAY_HELD_MAX) {
            fail(r, ASX_REPLAY_E_UNSUPPORTED, rec.seq, rec.trace_seq, rec.kind, 0u,
                 "too many off-thread records held for a job");
            return;
        }
        h = &r->held[r->held_count++];
        h->payload = rec.payload;
        h->payload_len = rec.payload_len;
        h->seq = rec.seq;
        h->trace_seq = rec.trace_seq;
        return;
    }
    fail(r, ASX_REPLAY_E_DIVERGED, rec.seq, rec.trace_seq, rec.kind, 0u,
         "off-thread record of a kind with no job to run");
}

/* Make the next main-order record current, taking off-thread records out
 * of the way. 1: r->cur is a record. 0: the replay has failed or the
 * stream is at its END (*at_end); `asked` is the kind the caller wants, for
 * the report. */
static int advance(asx_replay *r, uint8_t asked, int *at_end) {
    *at_end = 0;
    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: every pass consumes one record of the stream");
        if (r->failed) return 0;
        if (!r->have_cur) {
            asx_journal_read_result rr = asx_journal_reader_next(&r->reader, &r->cur);
            if (rr == ASX_JOURNAL_READ_END) {
                *at_end = 1;
                return 0;
            }
            /* A recovery replay's stream was opened up to its last verified
             * chain: running out of bytes there is the cut. */
            if (rr == ASX_JOURNAL_READ_TRUNCATED && r->prefix_cut) {
                *at_end = 1;
                return 0;
            }
            if (rr != ASX_JOURNAL_READ_RECORD) {
                fail(r, ASX_REPLAY_E_CORRUPT, r->reader.fail_seq, 0u, 0u, asked,
                     "stream changed since it was opened");
                return 0;
            }
            r->have_cur = 1;
        }
        if ((r->cur.flags & ASX_JOURNAL_REC_OFFTHREAD) == 0u) return 1;
        take_offthread(r);
    }
}

/* The next record, for a request of kind `asked`: the stream's end is a
 * failure. */
static int peek(asx_replay *r, uint8_t asked) {
    int at_end = 0;
    if (advance(r, asked, &at_end)) return 1;
    if (at_end && r->prefix_cut) {
        fail(r, ASX_REPLAY_E_CUT, r->cut_seq, 0u, 0u, asked,
             "the recording was cut before this record was committed");
    } else if (at_end) {
        fail(r, ASX_REPLAY_E_EXHAUSTED, r->reader.next_seq - 1u, 0u, (uint8_t)ASX_JOURNAL_KIND_END,
             asked, "the journal has no more records");
    }
    return 0;
}

/* The next record must be a `kind` with at least `min_payload` bytes. */
static int expect(asx_replay *r, uint8_t kind, uint32_t min_payload) {
    if (!peek(r, kind)) return 0;
    if (r->cur.kind != kind) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq, r->cur.kind, kind, "kind");
        return 0;
    }
    if (r->cur.payload_len < min_payload) {
        malformed(r, "record shorter than its kind's payload");
        return 0;
    }
    return 1;
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
        diverge(r, api == 0u ? "wait timeout" : "wait logical step");
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
        diverge(r, "poll timeout");
        return ASX_E_REPLAY_MISMATCH;
    }
    st = (asx_status)(int32_t)asx_journal_get_u32(p + 4);
    n = asx_journal_get_u32(p + 8);
    if ((r->cur.payload_len - 12u) / 12u != n || (r->cur.payload_len - 12u) % 12u != 0u) {
        malformed(r, "poll event count");
        return ASX_E_REPLAY_MISMATCH;
    }
    if (n > max_events || (n > 0u && events == NULL)) {
        diverge(r, "poll room for the recorded events");
        return ASX_E_REPLAY_MISMATCH;
    }
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= max_events");
        uint64_t token = asx_journal_get_u64(p + 12u + (size_t)i * 12u);
        uint32_t slot = (uint32_t)(token & 0xFFFFFFFFu);
        /* The slot is the run's own; the generation in a recorded token is
         * the recording process's history, so answer with the generation
         * this run armed the slot with. */
        if (slot < ASX_MAX_IO_TOKENS && r->io_gen_known[slot] != 0u) {
            token = ((uint64_t)r->io_gen[slot] << 32) | (uint64_t)slot;
        }
        events[i].token = token;
        events[i].ready = asx_journal_get_u32(p + 20u + (size_t)i * 12u);
    }
    consume(r);
    if (out_count != NULL) *out_count = n;
    return st;
}

/* Registration is not an input: the replay's reactor holds no fds. It
 * notes the generation of the slot being armed, so the recorded events
 * (which carry the recording's) can be answered with this run's tokens. */
static asx_status replay_register(void *ctx, int fd, uint32_t interest, uint64_t token) {
    asx_replay *r = (asx_replay *)ctx;
    uint32_t slot = (uint32_t)(token & 0xFFFFFFFFu);
    (void)fd;
    (void)interest;
    if (slot < ASX_MAX_IO_TOKENS) {
        r->io_gen[slot] = (uint16_t)(token >> 32);
        r->io_gen_known[slot] = 1u;
    }
    return ASX_OK;
}

static void replay_deregister(void *ctx, int fd) {
    (void)ctx;
    (void)fd;
}

static void replay_notify(void *ctx) { (void)ctx; }

/* A job is held where it was submitted and run when its completion record
 * is reached (deliver_done), never here. */
static asx_status replay_blocking_submit(void *ctx, asx_blocking_job_fn job_fn, void *job_ctx) {
    (void)ctx;
    (void)job_fn;
    (void)job_ctx;
    return ASX_OK;
}

static void replay_blocking_shutdown(void *ctx) { (void)ctx; }

static uint32_t replay_blocking_capacity(void *ctx) {
    (void)ctx;
    return 8u;
}

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
    hooks->blocking.ctx = r;
    hooks->blocking.submit_fn = replay_blocking_submit;
    hooks->blocking.shutdown_fn = replay_blocking_shutdown;
    hooks->blocking.capacity_fn = replay_blocking_capacity;
    hooks->deterministic_seeded_prng = 1u;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Activation, outcome                                                 */
/* ------------------------------------------------------------------ */

/* Whether `r` may become the active replay: the check without the store, so
 * replay_run can store only after it has passed (a store that some path
 * returns past is what gcc's -Wdangling-pointer reports at -O2 for the stack
 * replay it activates). */
static asx_status replay_activation_status(const asx_replay *r) {
    if (r == NULL || !r->opened) return ASX_E_INVALID_ARGUMENT;
    if ((g_replay != NULL && g_replay != r) || asx_journal_active()) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

asx_status asx_replay_activate(asx_replay *r) {
    asx_status st = replay_activation_status(r);
    if (st == ASX_OK) g_replay = r;
    return st;
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
    if (!r->failed && r->held_count > 0u) {
        /* Read before anything left in the stream: a job asked for none. */
        fail(r, ASX_REPLAY_E_DIVERGED, r->held[0].seq, r->held[0].trace_seq,
             (uint8_t)ASX_JOURNAL_KIND_RESOLVE, 0u, "record never requested");
    }
    if (!r->failed && !r->have_cur) {
        asx_journal_read_result rr = asx_journal_reader_next(&r->reader, &r->cur);
        if (rr == ASX_JOURNAL_READ_RECORD) {
            r->have_cur = 1;
        } else if (rr != ASX_JOURNAL_READ_END &&
                   !(rr == ASX_JOURNAL_READ_TRUNCATED && r->prefix_cut)) {
            fail(r, ASX_REPLAY_E_CORRUPT, r->reader.fail_seq, 0u, 0u, 0u,
                 "stream changed since it was opened");
        }
    }
    if (!r->failed && r->have_cur) {
        fail(r, ASX_REPLAY_E_DIVERGED, r->cur.seq, r->cur.trace_seq, r->cur.kind, 0u,
             "record never requested");
    }
    if (!r->failed && r->prefix_cut) {
        /* Every committed record was asked for and the program ended: the
         * prefix cannot show that the recorded run did too. */
        fail(r, ASX_REPLAY_E_CUT, r->cut_seq, 0u, 0u, 0u,
             "the program ended inside the committed prefix");
    }
    if (r->failed) {
        capture_hindsight(r);
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
        malformed(r, "signal count");
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

/* Decode the RESOLVE payload `p` (record `seq`, `trace_seq`) for
 * `name_hash`. 1 when it was served, else the failure is recorded against
 * that record. */
static int resolve_decode(asx_replay *r, const uint8_t *p, uint32_t len, uint64_t seq,
                          uint64_t trace_seq, uint64_t name_hash, int32_t *status,
                          asx_journal_addr *addrs, uint32_t cap, uint32_t *count) {
    const uint8_t k = (uint8_t)ASX_JOURNAL_KIND_RESOLVE;
    uint32_t n;
    uint32_t i;
    if (len < 16u) {
        fail(r, ASX_REPLAY_E_CORRUPT, seq, trace_seq, k, k,
             "record shorter than its kind's payload");
        return 0;
    }
    if (asx_journal_get_u64(p) != name_hash) {
        fail(r, ASX_REPLAY_E_DIVERGED, seq, trace_seq, k, k, "resolve host");
        return 0;
    }
    n = asx_journal_get_u32(p + 12);
    if (len != 16u + n * 19u) {
        fail(r, ASX_REPLAY_E_CORRUPT, seq, trace_seq, k, k, "resolve address count");
        return 0;
    }
    if (n > cap) {
        fail(r, ASX_REPLAY_E_DIVERGED, seq, trace_seq, k, k, "room for the recorded addresses");
        return 0;
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
    return 1;
}

int asx_replay_serve_resolve(uint64_t name_hash, int32_t *status, asx_journal_addr *addrs,
                             uint32_t cap, uint32_t *count) {
    asx_replay *r = g_replay;
    if (r == NULL) return 0;
    *count = 0u;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    if (r->in_job) {
        /* A blocking job asks: its lookup was noted on the pool thread and
         * held; take the first held record of that host. */
        uint32_t i;
        for (i = 0; i < r->held_count; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: held_count <= ASX_REPLAY_HELD_MAX");
            const asx_replay_held *h = &r->held[i];
            if (h->payload_len >= 8u && asx_journal_get_u64(h->payload) == name_hash) break;
        }
        if (i >= r->held_count) {
            fail(r, ASX_REPLAY_E_DIVERGED, r->job_seq, 0u, (uint8_t)ASX_JOURNAL_KIND_RESOLVE,
                 (uint8_t)ASX_JOURNAL_KIND_RESOLVE,
                 "resolution by a blocking job with no recorded lookup of that host");
            return 1;
        }
        if (resolve_decode(r, r->held[i].payload, r->held[i].payload_len, r->held[i].seq,
                           r->held[i].trace_seq, name_hash, status, addrs, cap, count)) {
            uint32_t j;
            r->last_seq = r->held[i].seq;
            r->last_trace_seq = r->held[i].trace_seq;
            r->last_kind = (uint8_t)ASX_JOURNAL_KIND_RESOLVE;
            r->served++;
            for (j = i + 1u; j < r->held_count; j++) {
                ASX_CHECKPOINT_WAIVER("bounded: held_count <= ASX_REPLAY_HELD_MAX");
                r->held[j - 1u] = r->held[j];
            }
            r->held_count--;
        }
        return 1;
    }
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_RESOLVE, 16u)) return 1;
    if (resolve_decode(r, r->cur.payload, r->cur.payload_len, r->cur.seq, r->cur.trace_seq,
                       name_hash, status, addrs, cap, count)) {
        consume(r);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */

static int addr_matches(const uint8_t *rec, const asx_journal_addr *a) {
    return a != NULL && rec[0] == a->family && memcmp(rec + 1, a->bytes, 16u) == 0 &&
           asx_journal_get_u16(rec + 17) == a->port;
}

static void addr_load(const uint8_t *rec, asx_journal_addr *a) {
    if (a == NULL) return;
    a->family = rec[0];
    memcpy(a->bytes, rec + 1, 16u);
    a->port = asx_journal_get_u16(rec + 17);
}

/* LISTEN and UDP_BIND: addr want | i32 status | u64 handle | addr local. */
static int serve_bind(uint8_t kind, const char *what, const asx_journal_addr *want, int32_t *status,
                      uint64_t *handle, asx_journal_addr *local) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *handle = 0u;
    memset(local, 0, sizeof(*local));
    if (!expect(r, kind, 50u)) return 1;
    p = r->cur.payload;
    if (!addr_matches(p, want)) {
        diverge(r, what);
        return 1;
    }
    *status = (int32_t)asx_journal_get_u32(p + 19);
    *handle = asx_journal_get_u64(p + 23);
    addr_load(p + 31, local);
    consume(r);
    return 1;
}

int asx_replay_serve_net_listen(const asx_journal_addr *want, int32_t *status, uint64_t *handle,
                                asx_journal_addr *local) {
    return serve_bind((uint8_t)ASX_JOURNAL_KIND_NET_LISTEN, "listen address", want, status, handle,
                      local);
}

int asx_replay_serve_udp_bind(const asx_journal_addr *want, int32_t *status, uint64_t *handle,
                              asx_journal_addr *local) {
    return serve_bind((uint8_t)ASX_JOURNAL_KIND_UDP_BIND, "bind address", want, status, handle,
                      local);
}

int asx_replay_serve_net_ctl(uint8_t op, uint64_t handle, int32_t *status) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_NET_CTL, 13u)) return 1;
    p = r->cur.payload;
    if (p[0] != op) {
        diverge(r, "socket control operation");
        return 1;
    }
    if (asx_journal_get_u64(p + 1) != handle) {
        diverge(r, "socket control handle");
        return 1;
    }
    *status = (int32_t)asx_journal_get_u32(p + 9);
    consume(r);
    return 1;
}

int asx_replay_serve_net_accept(uint64_t listener, int32_t *status, uint64_t *stream,
                                asx_journal_addr *peer, asx_journal_addr *local) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *stream = 0u;
    memset(peer, 0, sizeof(*peer));
    memset(local, 0, sizeof(*local));
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_NET_ACCEPT, 58u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p) != listener) {
        diverge(r, "accept listener handle");
        return 1;
    }
    *status = (int32_t)asx_journal_get_u32(p + 8);
    *stream = asx_journal_get_u64(p + 12);
    addr_load(p + 20, peer);
    addr_load(p + 39, local);
    consume(r);
    return 1;
}

int asx_replay_serve_net_connect(const asx_journal_addr *to, int32_t *status, uint64_t *stream,
                                 asx_journal_addr *local) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *stream = 0u;
    memset(local, 0, sizeof(*local));
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_NET_CONNECT, 50u)) return 1;
    p = r->cur.payload;
    if (!addr_matches(p, to)) {
        diverge(r, "connect address");
        return 1;
    }
    *status = (int32_t)asx_journal_get_u32(p + 19);
    *stream = asx_journal_get_u64(p + 23);
    addr_load(p + 31, local);
    consume(r);
    return 1;
}

/* NET_READ and PROCESS_READ share their tail: u32 cap | i32 status | u32 n
 * then the data, at `base`. */
static int serve_read_body(asx_replay *r, uint32_t base, uint32_t cap, int32_t *status,
                           uint8_t *dst, uint32_t *n) {
    const uint8_t *p = r->cur.payload;
    uint32_t got;
    if (asx_journal_get_u32(p + base) != cap) {
        diverge(r, "read buffer length");
        return 0;
    }
    got = asx_journal_get_u32(p + base + 8u);
    if (got > cap || r->cur.payload_len != base + 12u + got) {
        malformed(r, "read length");
        return 0;
    }
    if (got > 0u && dst != NULL) memcpy(dst, p + base + 12u, got);
    *status = (int32_t)asx_journal_get_u32(p + base + 4u);
    *n = got;
    return 1;
}

int asx_replay_serve_net_read(uint64_t stream, uint32_t cap, int32_t *status, uint8_t *dst,
                              uint32_t *n) {
    asx_replay *r = g_replay;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *n = 0u;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_NET_READ, 20u)) return 1;
    if (asx_journal_get_u64(r->cur.payload) != stream) {
        diverge(r, "read stream handle");
        return 1;
    }
    if (serve_read_body(r, 8u, cap, status, dst, n)) consume(r);
    return 1;
}

/* NET_WRITE, UDP_SEND and PROCESS_WRITE: u64 handle | u32 len | i32 status
 * | u32 written. */
static int serve_write(uint8_t kind, const char *handle_what, const char *len_what, uint64_t handle,
                       uint32_t len, int32_t *status, uint32_t *written) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *written = 0u;
    if (!expect(r, kind, 20u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p) != handle) {
        diverge(r, handle_what);
        return 1;
    }
    if (asx_journal_get_u32(p + 8) != len) {
        diverge(r, len_what);
        return 1;
    }
    *status = (int32_t)asx_journal_get_u32(p + 12);
    *written = asx_journal_get_u32(p + 16);
    consume(r);
    return 1;
}

int asx_replay_serve_net_write(uint64_t stream, uint32_t len, int32_t *status, uint32_t *written) {
    return serve_write((uint8_t)ASX_JOURNAL_KIND_NET_WRITE, "write stream handle", "write length",
                       stream, len, status, written);
}

int asx_replay_serve_udp_send(uint64_t socket, uint32_t len, int32_t *status, uint32_t *written) {
    return serve_write((uint8_t)ASX_JOURNAL_KIND_UDP_SEND, "send socket handle", "send length",
                       socket, len, status, written);
}

int asx_replay_serve_udp_recv(uint64_t socket, uint32_t cap, int32_t *status, uint8_t *dst,
                              uint32_t *n, asx_journal_addr *from) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    uint32_t got;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *n = 0u;
    memset(from, 0, sizeof(*from));
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_UDP_RECV, 39u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p) != socket) {
        diverge(r, "receive socket handle");
        return 1;
    }
    if (asx_journal_get_u32(p + 8) != cap) {
        diverge(r, "receive buffer length");
        return 1;
    }
    got = asx_journal_get_u32(p + 16);
    if (got > cap || r->cur.payload_len != 39u + got) {
        malformed(r, "receive length");
        return 1;
    }
    if (got > 0u && dst != NULL) memcpy(dst, p + 39, got);
    *status = (int32_t)asx_journal_get_u32(p + 12);
    *n = got;
    addr_load(p + 20, from);
    consume(r);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Process                                                             */
/* ------------------------------------------------------------------ */

int asx_replay_serve_process_spawn(uint64_t command_hash, int32_t *status, uint64_t *handle,
                                   uint32_t *pid, uint8_t *flags) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *handle = 0u;
    *pid = 0u;
    *flags = 0u;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_PROCESS_SPAWN, 25u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p + 16) != command_hash) {
        diverge(r, "spawn command");
        return 1;
    }
    *status = (int32_t)asx_journal_get_u32(p);
    *handle = asx_journal_get_u64(p + 4);
    *pid = asx_journal_get_u32(p + 12);
    *flags = p[24];
    consume(r);
    return 1;
}

int asx_replay_serve_process_exit(uint64_t handle, uint32_t *state, int32_t *code, int32_t *signal,
                                  int *exited) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *exited = 0;
    *state = 0u;
    *code = 0;
    *signal = 0;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_PROCESS_EXIT, 20u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p) != handle) {
        diverge(r, "reap attempt process handle");
        return 1;
    }
    *state = asx_journal_get_u32(p + 8);
    *code = (int32_t)asx_journal_get_u32(p + 12);
    *signal = (int32_t)asx_journal_get_u32(p + 16);
    *exited = *state != 0u;
    consume(r);
    return 1;
}

int asx_replay_failed(void) { return g_replay != NULL && g_replay->failed; }

/* The scheduler ends a run at its next round when this is nonzero: a
 * failed replay answers nothing, and a task that treats the refusal as
 * "try again" would otherwise be polled for ever. */
int asx_replay_stop_run_internal(void) { return g_replay != NULL && g_replay->failed; }

int asx_replay_serve_process_write(uint64_t handle, uint32_t len, int32_t *status,
                                   uint32_t *written) {
    return serve_write((uint8_t)ASX_JOURNAL_KIND_PROCESS_WRITE, "stdin write process handle",
                       "stdin write length", handle, len, status, written);
}

int asx_replay_serve_process_read(uint64_t handle, uint8_t which, uint32_t cap, int32_t *status,
                                  uint8_t *dst, uint32_t *n) {
    asx_replay *r = g_replay;
    const uint8_t *p;
    if (r == NULL) return 0;
    *status = (int32_t)ASX_E_REPLAY_MISMATCH;
    *n = 0u;
    if (!expect(r, (uint8_t)ASX_JOURNAL_KIND_PROCESS_READ, 21u)) return 1;
    p = r->cur.payload;
    if (asx_journal_get_u64(p) != handle) {
        diverge(r, "child read process handle");
        return 1;
    }
    if (p[8] != which) {
        diverge(r, "child read stream");
        return 1;
    }
    if (serve_read_body(r, 9u, cap, status, dst, n)) consume(r);
    return 1;
}

int asx_replay_check_handle(uint64_t recorded, uint64_t actual, const char *what) {
    asx_replay *r = g_replay;
    if (r == NULL || recorded == actual) return 1;
    fail(r, ASX_REPLAY_E_DIVERGED, r->last_seq, r->last_trace_seq, r->last_kind, r->last_kind,
         what);
    return 0;
}

/* ------------------------------------------------------------------ */
/* One call                                                            */
/* ------------------------------------------------------------------ */

/* The run could not be started: report it as such, with the status. */
static asx_replay_error replay_run_failed(asx_replay_report *report, asx_status st,
                                          asx_status *program_status, const char *what) {
    if (program_status != NULL) *program_status = st;
    if (report != NULL) {
        memset(report, 0, sizeof(*report));
        report_set(report, ASX_REPLAY_E_RUNTIME, 0u, 0u, 0u, 0u, 0u, what);
    }
    return ASX_REPLAY_E_RUNTIME;
}

static asx_replay_error replay_run(const uint8_t *bytes, size_t len,
                                   const asx_replay_identity *identity,
                                   const asx_runtime_config *config, asx_replay_program_fn program,
                                   void *user, asx_status *program_status,
                                   asx_replay_report *report, asx_replay_prefix *prefix) {
    asx_replay r;
    asx_runtime rt;
    asx_runtime_hooks hooks;
    asx_runtime_config cfg;
    asx_replay_error err;
    asx_status st;

    if (program_status != NULL) *program_status = ASX_OK;
    if (program == NULL) {
        return replay_run_failed(report, ASX_E_INVALID_ARGUMENT, program_status, "no program");
    }
    err = replay_open_impl(&r, bytes, len, identity, report, prefix);
    if (err != ASX_REPLAY_OK) return err;

    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_replay_hooks(&r, &hooks);
    if (st != ASX_OK) return replay_run_failed(report, st, program_status, "replay hooks");
    if (config != NULL) {
        cfg = *config;
    } else {
        st = asx_runtime_config_init(&cfg);
        if (st != ASX_OK) return replay_run_failed(report, st, program_status, "runtime config");
    }
    st = replay_activation_status(&r);
    if (st != ASX_OK) return replay_run_failed(report, st, program_status, "replay activation");
    g_replay = &r; /* cleared by asx_replay_deactivate on every path out of here */
    st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) {
        asx_replay_deactivate();
        return replay_run_failed(report, st, program_status, "runtime initialization");
    }

    st = program(user);
    /* The shutdown clears the hindsight ring: read a divergence's now. */
    capture_hindsight(&r);
    /* Shutdown still makes boundary calls the recording made: serve them. */
    asx_runtime_shutdown(&rt);
    asx_replay_deactivate();
    if (program_status != NULL) *program_status = st;
    err = asx_replay_finish(&r, report);
    if (prefix != NULL) asx_replay_prefix_progress(&r, prefix);
    return err;
}

asx_replay_error asx_journal_replay(const uint8_t *bytes, size_t len,
                                    const asx_replay_identity *identity,
                                    const asx_runtime_config *config, asx_replay_program_fn program,
                                    void *user, asx_status *program_status,
                                    asx_replay_report *report) {
    return replay_run(bytes, len, identity, config, program, user, program_status, report, NULL);
}

asx_replay_error asx_journal_replay_prefix(const uint8_t *bytes, size_t len,
                                           const asx_replay_identity *identity,
                                           const asx_runtime_config *config,
                                           asx_replay_program_fn program, void *user,
                                           asx_status *program_status, asx_replay_report *report,
                                           asx_replay_prefix *prefix) {
    if (prefix == NULL) {
        return replay_run_failed(report, ASX_E_INVALID_ARGUMENT, program_status,
                                 "no prefix result");
    }
    return replay_run(bytes, len, identity, config, program, user, program_status, report, prefix);
}
