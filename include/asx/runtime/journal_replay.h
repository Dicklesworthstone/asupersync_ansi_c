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
 *      stored as a hash, an off-thread record that is not a blocking
 *      completion or a resolution. Supported kinds: CLOCK, ENTROPY,
 *      REACTOR_WAIT, REACTOR_POLL, RESOLVE, SIGNAL everywhere, and, in a
 *      live POSIX build, NET_*, UDP_*, PROCESS_*, FS and BLOCKING_DONE (the
 *      native adapters serve them while a replay is active).
 *   4. Fidelity. Each request must match the next record: its kind, and
 *      its shape (a wait's timeout or logical step, a poll's timeout and
 *      room for its events, a resolve's host, a socket call's handle and
 *      buffer length, a spawn's command). The first request that does not
 *      stops the replay with ASX_REPLAY_E_DIVERGED and that record's
 *      seq, the kind it held and the kind asked for; a request after the
 *      last record is ASX_REPLAY_E_EXHAUSTED. Every later request fails
 *      the same way and the report keeps the first.
 *   5. Completeness. asx_replay_finish() reports ASX_REPLAY_E_DIVERGED for
 *      the first record the program never asked for.
 *
 * A request whose answer carries a status returns ASX_E_REPLAY_MISMATCH
 * once the replay has failed; the clock and an entropy draw that cannot
 * report return 0. The caller checks asx_replay_failure(). The scheduler
 * also ends a run whose replay has failed, at its next round, with
 * ASX_E_REPLAY_MISMATCH: a program that waits for answers it will no
 * longer get does not spin.
 *
 * Recovery replay (asx_replay_open_prefix). A run that was killed leaves a
 * stream with no END. A recovery replay serves the committed prefix, every
 * record up to the last CHAIN that verifies, and ignores what follows (a
 * torn or unverified tail; a chain that does not verify ends the prefix
 * like a cut). It differs from the contract above in three ways:
 *   - the stream need not be complete: integrity (step 2) is checked over
 *     the prefix, and the report says why the prefix ended (INCOMPLETE:
 *     the bytes ran out; CORRUPT: a record or chain failed) and where;
 *   - a request after the prefix's last record is ASX_REPLAY_E_CUT, with
 *     seq the first uncommitted record, not EXHAUSTED: it is the expected
 *     end of the replay. A program that finishes inside the prefix ends
 *     the same way, since the prefix cannot show the recorded run did;
 *   - every committed window's trace checkpoint is verified as the run
 *     reaches it: the run's trace position and rolling digest, when the
 *     window's last record is consumed, must be the ones the recording
 *     stored, or the replay stops with DIVERGED at that record ("trace
 *     checkpoint"). That is what "the replay reproduces every committed
 *     step" means; asx_replay_prefix reports how many were verified and
 *     the last one.
 * A complete stream replays as in the contract, plus the checkpoints.
 *
 * Replayed values are the raw boundary results, before the runtime's fault
 * injection: the accessors apply injected faults to them as they did when
 * recording. The clock hooks serve CLOCK records whichever clock the run
 * asked (wall or logical), and the wait hooks serve REACTOR_WAIT records
 * whichever form (live or ghost), so a stream recorded in a live build
 * replays in a deterministic one.
 *
 * Native handles. A replay opens no socket and starts no process. The
 * adapters keep their handle tables as they were, with a descriptor number
 * at or above ASX_REPLAY_VIRTUAL_FD_BASE standing for the resource. A
 * handle's generation is the history of the process that issued it, so a
 * replayed socket or child answers to the name the recording gave it, and
 * a request naming another handle, buffer length or command is a
 * divergence. A result that was PENDING parks the task as it did, on the
 * virtual descriptor, and wakes when the recorded reactor poll returns its
 * token (the replay answers with this run's generation of that slot).
 *
 * Blocking jobs. A job is not run when it is submitted. Its BLOCKING_DONE
 * record is reached at some boundary request; the replay then runs the job
 * on the runtime thread, serving the boundary calls the job makes (its
 * name resolution) from the records the pool thread noted before the DONE,
 * and requires the result to be the recorded one. A program that learns a
 * job finished by polling its state with no boundary call in between is
 * not replayable (the replay delivers completions only at boundary
 * requests), and a job must not call the hook-wrapped clock or entropy.
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
    ASX_REPLAY_E_EXHAUSTED = 6,      /* a request after the last record */
    ASX_REPLAY_E_RUNTIME = 7,        /* asx_journal_replay could not start the run */
    ASX_REPLAY_E_CUT = 8             /* recovery replay: the committed prefix is over */
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
    /* DIVERGED and EXHAUSTED only: the hindsight ring (asx/runtime/hindsight.h)
     * as JSON, the clock, entropy and readiness values the run was served up
     * to the end of the program, or NULL when the ring was empty or the
     * divergence was found after the runtime shut down (a record never
     * requested). It is read when the program returns, before the runtime
     * shuts down, and kept in static storage the next diverging replay
     * overwrites. */
    const char *hindsight;
} asx_replay_report;

/* What the caller's build is, to compare with the stream's header; 0 in a
 * field means do not check it. */
typedef struct {
    uint64_t rust_rev_hash;
    uint64_t build_hash;
} asx_replay_identity;

/* Descriptor numbers at or above this belong to no open file: a replay
 * hands them to the IO driver in place of the descriptors it did not open,
 * and the adapters never close or poll them. */
#define ASX_REPLAY_VIRTUAL_FD_BASE 0x40000000

static inline int asx_replay_fd_is_virtual(int fd) { return fd >= ASX_REPLAY_VIRTUAL_FD_BASE; }

/* Off-thread records a replay holds until the blocking job that made them
 * is run again (a name resolution on the pool). */
#define ASX_REPLAY_HELD_MAX 16u

typedef struct {
    const uint8_t *payload;
    uint32_t payload_len;
    uint64_t seq;
    uint64_t trace_seq;
} asx_replay_held;

typedef struct {
    asx_journal_reader reader;
    asx_journal_record cur;
    int have_cur;
    int opened;
    int failed;
    uint64_t served;
    asx_replay_report failure;
    asx_replay_held held[ASX_REPLAY_HELD_MAX];
    uint32_t held_count;
    int in_job;       /* a blocking job is being run again */
    uint64_t job_seq; /* the DONE record that started it */
    uint64_t last_seq;
    uint64_t last_trace_seq;
    uint8_t last_kind; /* the record most recently consumed */
    /* Recovery replay (asx_replay_open_prefix). */
    int prefix;           /* serving a committed prefix, checkpoints verified */
    int prefix_cut;       /* the stream was cut: its end is a CUT, not END */
    uint64_t cut_seq;     /* the first record that was not committed */
    uint64_t cp_verified; /* trace checkpoints verified so far */
    uint64_t cp_seq;      /* the last one: its chain's seq */
    uint64_t cp_events;
    uint64_t cp_digest;
    /* The generation each IO-driver slot had when this run last armed it.
     * A recorded readiness token names the generation of the run that
     * recorded it, which is process history rather than program
     * behaviour; the replay answers with this run's own. */
    uint16_t io_gen[ASX_MAX_IO_TOKENS];
    uint8_t io_gen_known[ASX_MAX_IO_TOKENS];
} asx_replay;

/* Validate `bytes` (the whole stream, which must outlive the replay) against
 * the contract's identity, integrity and replayability steps and position
 * at its first record. Returns ASX_REPLAY_OK, or the error with *report
 * filled (report may be NULL). Nothing is served from a refused stream. */
ASX_API asx_replay_error asx_replay_open(asx_replay *r, const uint8_t *bytes, size_t len,
                                         const asx_replay_identity *identity,
                                         asx_replay_report *report);

/* Overlay `hooks` with the replay's clock, entropy, reactor and blocking
 * hooks (the clock, both entropy forms, wait, ghost wait, register,
 * deregister, poll and notify; a blocking submit that holds the job until
 * its DONE record is reached), set deterministic_seeded_prng and leave the
 * other families as they are. `hooks` is a table initialized with
 * asx_runtime_hooks_init.
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULLs or a replay that was
 * not opened. The replay must outlive the runtime initialized with them. */
ASX_API ASX_MUST_USE asx_status asx_replay_hooks(asx_replay *r, asx_runtime_hooks *hooks);

/* What a recovery replay found in the stream and, once the run is over,
 * how far it got. */
typedef struct {
    int complete;                /* the stream has a valid END: nothing was lost */
    uint64_t committed_bytes;    /* length of the prefix served (the stream's, when complete) */
    uint64_t committed_records;  /* data records in it */
    uint64_t cut_seq;            /* the first record not in it (the seq after its last chain) */
    asx_replay_error cut_reason; /* why it ended: INCOMPLETE or CORRUPT; OK when complete */
    uint64_t stream_bytes;       /* bytes the stream had: the tail beyond the prefix is these
                                  * less committed_bytes */
    /* Filled by asx_journal_replay_prefix / asx_replay_prefix_progress. */
    uint64_t served;         /* records the program consumed */
    uint64_t checkpoints;    /* trace checkpoints verified */
    uint64_t checkpoint_seq; /* the last one verified: its chain's seq */
    uint64_t checkpoint_events;
    uint64_t checkpoint_digest;
} asx_replay_prefix;

/* As asx_replay_open, for a stream that may have been cut short: instead
 * of refusing for a missing END or a broken tail it serves the committed
 * prefix (see "Recovery replay" above). Identity, replayability of the
 * records in the prefix and, when an END is present, its totals are
 * checked as for asx_replay_open; a stream whose header is bad or cut is
 * still refused. *prefix (required) is filled. Returns ASX_REPLAY_OK, or
 * the error with *report filled. */
ASX_API asx_replay_error asx_replay_open_prefix(asx_replay *r, const uint8_t *bytes, size_t len,
                                                const asx_replay_identity *identity,
                                                asx_replay_report *report,
                                                asx_replay_prefix *prefix);

/* Fill the progress fields of *prefix from `r` (a recovery replay). */
ASX_API void asx_replay_prefix_progress(const asx_replay *r, asx_replay_prefix *prefix);

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
 * `name_hash`: *status and up to `cap` addresses with their count. Made by
 * a blocking job being run again, it is served from the records held for
 * that job. */
ASX_API int asx_replay_serve_resolve(uint64_t name_hash, int32_t *status, asx_journal_addr *addrs,
                                     uint32_t cap, uint32_t *count);

/* A TCP listen on `want` (net_listen) or a UDP bind (udp_bind): *status,
 * the *handle it created and the address it was bound to. */
ASX_API int asx_replay_serve_net_listen(const asx_journal_addr *want, int32_t *status,
                                        uint64_t *handle, asx_journal_addr *local);
ASX_API int asx_replay_serve_udp_bind(const asx_journal_addr *want, int32_t *status,
                                      uint64_t *handle, asx_journal_addr *local);

/* A socket control call (`op` is an ASX_JOURNAL_NET_CTL_*) on `handle`. */
ASX_API int asx_replay_serve_net_ctl(uint8_t op, uint64_t handle, int32_t *status);

/* A TCP accept on `listener`: *status, the *stream and its peer and local
 * address. */
ASX_API int asx_replay_serve_net_accept(uint64_t listener, int32_t *status, uint64_t *stream,
                                        asx_journal_addr *peer, asx_journal_addr *local);

/* A TCP connect to `to`: *status, the *stream and its local address. */
ASX_API int asx_replay_serve_net_connect(const asx_journal_addr *to, int32_t *status,
                                         uint64_t *stream, asx_journal_addr *local);

/* A TCP read of up to `cap` bytes from `stream`: *status and *n bytes into
 * `dst`. */
ASX_API int asx_replay_serve_net_read(uint64_t stream, uint32_t cap, int32_t *status, uint8_t *dst,
                                      uint32_t *n);

/* A TCP write of `len` bytes to `stream`: *status and *written. */
ASX_API int asx_replay_serve_net_write(uint64_t stream, uint32_t len, int32_t *status,
                                       uint32_t *written);

/* A UDP receive of up to `cap` bytes on `socket`: *status, *n bytes into
 * `dst` and the sender in `from`. */
ASX_API int asx_replay_serve_udp_recv(uint64_t socket, uint32_t cap, int32_t *status, uint8_t *dst,
                                      uint32_t *n, asx_journal_addr *from);

/* A UDP send of `len` bytes on `socket`: *status and *written. */
ASX_API int asx_replay_serve_udp_send(uint64_t socket, uint32_t len, int32_t *status,
                                      uint32_t *written);

/* A child spawn of the command whose hash is `command_hash`: *status, the
 * *handle, *pid and *flags (ASX_JOURNAL_SPAWN_*) it had. */
ASX_API int asx_replay_serve_process_spawn(uint64_t command_hash, int32_t *status, uint64_t *handle,
                                           uint32_t *pid, uint8_t *flags);

/* A reap attempt on the child `handle`: the next record is its answer,
 * *exited 1 with the *state, *code and *signal the child had exited with,
 * or 0 when it was still running. */
ASX_API int asx_replay_serve_process_exit(uint64_t handle, uint32_t *state, int32_t *code,
                                          int32_t *signal, int *exited);

/* Nonzero when the active replay has failed: it answers nothing from then
 * on, and a loop that waits for an answer must stop. */
ASX_API int asx_replay_failed(void);

/* A write of `len` bytes to a child's stdin: *status and *written. */
ASX_API int asx_replay_serve_process_write(uint64_t handle, uint32_t len, int32_t *status,
                                           uint32_t *written);

/* A read of up to `cap` bytes from a child's stdout (`which` 1) or stderr
 * (2): *status and *n bytes into `dst`. */
ASX_API int asx_replay_serve_process_read(uint64_t handle, uint8_t which, uint32_t cap,
                                          int32_t *status, uint8_t *dst, uint32_t *n);

/* A native filesystem call (the FS record): `op` (ASX_JOURNAL_FS_*), `key`
 * (the path's asx_journal_text_hash, or the handle), and the inputs `arg`
 * and `len`, which must be the recording's. *status and *result are its
 * answer; up to `cap` bytes of its data go to `dst` and *n is their count
 * (`dst` may be NULL with `cap` 0 for a call that returns none). */
ASX_API int asx_replay_serve_fs(uint8_t op, uint64_t key, uint64_t arg, uint32_t len,
                                int32_t *status, uint64_t *result, uint8_t *dst, uint32_t cap,
                                uint32_t *n);

/* An adapter's check of a table entry it needs against what the recording
 * had: 1 when `recorded` equals `actual`, else the replay fails with
 * ASX_REPLAY_E_DIVERGED at the record last served, naming `what`, and 0. */
ASX_API int asx_replay_check_handle(uint64_t recorded, uint64_t actual, const char *what);

/* -------------------------------------------------------------------
 * One call: replay a program against a stream
 * ------------------------------------------------------------------- */

/* The program a replay runs: the caller's own code, which must make the
 * boundary calls the recording's did, in the same order, through the
 * runtime asx_journal_replay starts for it. Its status is reported, not
 * judged. */
typedef asx_status (*asx_replay_program_fn)(void *user);

/* Open `bytes` (identity, integrity and replayability as asx_replay_open),
 * initialize a runtime with `config` (NULL: the defaults) over the replay's
 * hooks, activate the adapters, run `program(user)`, shut the runtime down
 * and finish. Returns ASX_REPLAY_OK when the stream was accepted, the
 * program asked for exactly its records and no more, or the first error
 * with *report filled (report may be NULL). ASX_REPLAY_E_RUNTIME with
 * *program_status set when the runtime could not start; *program_status
 * (may be NULL) is otherwise what `program` returned. Not usable while a
 * runtime is initialized or a journal is active. */
ASX_API asx_replay_error asx_journal_replay(const uint8_t *bytes, size_t len,
                                            const asx_replay_identity *identity,
                                            const asx_runtime_config *config,
                                            asx_replay_program_fn program, void *user,
                                            asx_status *program_status, asx_replay_report *report);

/* asx_journal_replay over the committed prefix of a stream that may have
 * been cut short (asx_replay_open_prefix). The outcome of a crashed
 * recording is ASX_REPLAY_E_CUT with report->seq the first uncommitted
 * record and *prefix saying what was verified; ASX_REPLAY_OK only for a
 * complete stream the program consumed exactly. Any other error is as for
 * asx_journal_replay, DIVERGED naming the first committed record whose
 * trace checkpoint or request did not match. *prefix is filled whenever the
 * stream was opened. */
ASX_API asx_replay_error asx_journal_replay_prefix(
    const uint8_t *bytes, size_t len, const asx_replay_identity *identity,
    const asx_runtime_config *config, asx_replay_program_fn program, void *user,
    asx_status *program_status, asx_replay_report *report, asx_replay_prefix *prefix);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_JOURNAL_REPLAY_H */
