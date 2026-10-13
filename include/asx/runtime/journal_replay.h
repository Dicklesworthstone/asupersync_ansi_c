/*
 * asx/runtime/journal_replay.h — replay a native boundary journal (RP2)
 *
 * A journal (journal.h) holds the results of a live run's nondeterministic
 * boundaries in the order the runtime saw them. A replay hands those
 * results back, in the same order, to a run of the same program: the hook
 * table it installs answers clock reads, entropy draws and reactor
 * wait/poll from the stream, and the native adapters that call
 * asx_replay_serve_*() answer from it instead of from the OS.
 *
 * The contract, in the order it is enforced:
 *   1. Identity. asx_replay_open() refuses a stream whose rust_rev_hash or
 *      build_hash differs from the caller's (ASX_REPLAY_E_BUILD_MISMATCH;
 *      a stream that did not say fails a hash the caller does check).
 *   2. Integrity. It refuses a stream that is truncated, has no END, was
 *      recorded with dropped records or a failed sink
 *      (ASX_REPLAY_E_INCOMPLETE, seq = the first missing record or END),
 *      or whose header, sequence, chain or END totals are broken
 *      (ASX_REPLAY_E_CORRUPT: seq is the record or chain at which the
 *      check failed, first_seq the first record of that chain window; a
 *      chain break lies in [first_seq, seq)).
 *   3. Replayability. It refuses, naming the first such record
 *      (ASX_REPLAY_E_UNSUPPORTED), a record the replay cannot serve:
 *      a kind outside the supported set, a poll stored truncated, data
 *      stored as a hash. Supported kinds: CLOCK, ENTROPY, REACTOR_WAIT,
 *      REACTOR_POLL, RESOLVE, SIGNAL. Network, process and blocking
 *      records are not served yet and refuse a stream that holds them.
 *   4. Fidelity. Each request must match the next record: its kind, and
 *      its shape (a wait's timeout or logical step, a poll's timeout and
 *      room for its events, a resolve's host). The first request that does
 *      not stops the replay with ASX_REPLAY_E_DIVERGED and that record's
 *      seq, the kind it held and the kind asked for; a request after the
 *      last record is ASX_REPLAY_E_EXHAUSTED. Every later request fails
 *      the same way and the report keeps the first.
 *   5. Completeness. asx_replay_finish() reports ASX_REPLAY_E_DIVERGED for
 *      the first record the program never asked for.
 *
 * A request whose answer carries a status returns ASX_E_REPLAY_MISMATCH
 * once the replay has failed; the clock and an entropy draw that cannot
 * report return 0. The caller checks asx_replay_failure().
 *
 * Replayed values are the raw boundary results, before the runtime's fault
 * injection: the accessors apply injected faults to them as they did when
 * recording. The clock hooks serve CLOCK records whichever clock the run
 * asked (wall or logical), and the wait hooks serve REACTOR_WAIT records
 * whichever form (live or ghost), so a stream recorded in a live build
 * replays in a deterministic one.
 *
 * Thread-safety: one thread serves a replay; the adapters call from the
 * runtime thread.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_JOURNAL_REPLAY_H
#define ASX_RUNTIME_JOURNAL_REPLAY_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/journal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ASX_REPLAY_OK = 0,
    ASX_REPLAY_E_BUILD_MISMATCH = 1, /* rust_rev_hash or build_hash differs */
    ASX_REPLAY_E_INCOMPLETE = 2,     /* truncated, no END, dropped or failed recorder */
    ASX_REPLAY_E_CORRUPT = 3,        /* header, seq, chain or END totals broken */
    ASX_REPLAY_E_UNSUPPORTED = 4,    /* a record the replay cannot serve */
    ASX_REPLAY_E_DIVERGED = 5,       /* a request differs from the next record */
    ASX_REPLAY_E_EXHAUSTED = 6       /* a request after the last record */
} asx_replay_error;

/* The first failure of a replay. */
typedef struct {
    asx_replay_error error;
    uint64_t seq;          /* the record at fault (see the contract above) */
    uint64_t first_seq;    /* CORRUPT: first record of the chain window; else seq */
    uint64_t trace_seq;    /* the trace position stored with that record, 0 if none */
    uint8_t expected_kind; /* the record's kind (an asx_journal_kind), 0 if none */
    uint8_t actual_kind;   /* the kind the program asked for, 0 if none */
    const char *what;      /* the field or reason, a static string */
} asx_replay_report;

/* What the caller's build is, to compare with the stream's header; 0 in a
 * field means do not check it. */
typedef struct {
    uint64_t rust_rev_hash;
    uint64_t build_hash;
} asx_replay_identity;

typedef struct {
    asx_journal_reader reader;
    asx_journal_record cur;
    int have_cur;
    int opened;
    int failed;
    uint64_t served;
    asx_replay_report failure;
} asx_replay;

/* Validate `bytes` (the whole stream, which must outlive the replay) against
 * the contract's identity, integrity and replayability steps and position
 * at its first record. Returns ASX_REPLAY_OK, or the error with *report
 * filled (report may be NULL). Nothing is served from a refused stream. */
ASX_API asx_replay_error asx_replay_open(asx_replay *r, const uint8_t *bytes, size_t len,
                                         const asx_replay_identity *identity,
                                         asx_replay_report *report);

/* Overlay `hooks` with the replay's clock, entropy and reactor hooks (the
 * clock, both entropy forms, wait, ghost wait, register, deregister, poll
 * and notify), set deterministic_seeded_prng and leave the other families
 * as they are. `hooks` is a table initialized with asx_runtime_hooks_init.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULLs or a replay that was
 * not opened. The replay must outlive the runtime initialized with them. */
ASX_API ASX_MUST_USE asx_status asx_replay_hooks(asx_replay *r, asx_runtime_hooks *hooks);

/* Make `r` the replay the native adapters serve from, so they answer from
 * the stream instead of the OS (asx_replay_serve_*); asx_replay_deactivate
 * ends it. Returns ASX_OK, ASX_E_INVALID_ARGUMENT (not opened) or
 * ASX_E_INVALID_STATE (another replay, or a journal, is active). */
ASX_API ASX_MUST_USE asx_status asx_replay_activate(asx_replay *r);
ASX_API void asx_replay_deactivate(void);

/* Nonzero while a replay is active. */
ASX_API int asx_replay_active(void);

/* The replay's first failure, or NULL while it has none. */
ASX_API const asx_replay_report *asx_replay_failure(const asx_replay *r);

/* How many records the program has consumed. */
ASX_API uint64_t asx_replay_served(const asx_replay *r);

/* End of the run: ASX_REPLAY_OK when the replay has not failed and the
 * program consumed every record; otherwise the failure, or
 * ASX_REPLAY_E_DIVERGED for the first record it never asked for. */
ASX_API asx_replay_error asx_replay_finish(asx_replay *r, asx_replay_report *report);

/* -------------------------------------------------------------------
 * Adapter serving: called by the native adapters; each returns 0 when no
 * replay is active (the adapter does its own work) and 1 when the call
 * was answered from the stream, with the outputs filled. After a failure
 * the call is answered with nothing and the replay's report holds why.
 * ------------------------------------------------------------------- */

/* A signal drain: counts[0 .. kinds) filled with the recorded arrivals. */
ASX_API int asx_replay_serve_signal(uint32_t *counts, uint32_t kinds);

/* A name resolution of the host whose asx_journal_text_hash is
 * `name_hash`: *status and up to `cap` addresses with their count. */
ASX_API int asx_replay_serve_resolve(uint64_t name_hash, int32_t *status, asx_journal_addr *addrs,
                                     uint32_t cap, uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_JOURNAL_REPLAY_H */
