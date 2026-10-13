/*
 * asx/runtime/journal.h — native boundary journal (bd-9kll.11.1, RP1)
 *
 * A live run's nondeterminism enters at its boundaries: the clock, the
 * entropy source, reactor readiness, what a socket read returns, how a
 * child exited, which signals arrived, which blocking job finished first.
 * The journal records each of those results, in the order the runtime saw
 * them, in a versioned binary stream that a later replay (RP2) can feed
 * back. It records; it never changes a result.
 *
 * Where it listens
 *   - Hook table: asx_journal_wrap_hooks() wraps clock, entropy and reactor
 *     wait/poll in a hook table before the runtime is initialized with it.
 *     Every consumer that calls through the table is recorded, including
 *     the ones that read the table directly.
 *   - Native adapters: src/platform/posix and src/runtime/blocking.c call
 *     the asx_journal_note_*() functions below after each boundary call.
 *     With no journal installed each is one load and a branch.
 *   One journal is active per process, as the runtime is a singleton.
 *
 * Stream format v1 (all integers little-endian)
 *   header   48 bytes: "ASXJRNL\0", u32 version (1), u32 flags
 *            (ASX_JOURNAL_FLAG_*), u64 config fingerprint (the caller's,
 *            e.g. ASX_CONFIG_FINGERPRINT), u64 rust_rev_hash (the pinned
 *            asupersync revision the build targets), u64 build_hash (the
 *            C build: e.g. its git sha), 8 bytes zero (reserved). Both
 *            hashes are the caller's; asx_journal_text_hash() makes one
 *            from a revision string. A replay compares them with its own
 *            and refuses a stream recorded by another build; 0 means the
 *            recorder did not say.
 *   record   24-byte head then payload:
 *              u32 payload_len | u8 kind | u8 flags | u16 reserved (0) |
 *              u64 seq (0.., every record, chain and end included) |
 *              u64 trace_seq (asx_trace_emitted_total() when the result
 *                             was taken; 0 when not sampled)
 *   chain    kind CHAIN, 32-byte payload: SHA-256(previous chain || the
 *            bytes of every record since the previous chain record). The
 *            first chain starts from SHA-256 of the 48 header bytes.
 *   end      kind END, 64-byte payload: u64 data records, u64 stream bytes
 *            before END, u64 dropped records, i32 sticky status, u32 0,
 *            32-byte final chain. A stream without a valid END, with a
 *            broken chain or a gap in seq is not a complete journal.
 *
 * Record kinds and payloads (data records)
 *   CLOCK        u8 which (0 wall, 1 logical) | u64 raw ns
 *   ENTROPY      u8 api (0 random_u64_fn, 1 try_u64_fn) | i32 status | u64 value
 *   REACTOR_WAIT u8 api (0 wait_fn, 1 ghost_wait_fn) | u64 timeout_ms or
 *                logical step | i32 status | u32 ready_count
 *   REACTOR_POLL u32 timeout_ms | i32 status | u32 count | count x
 *                (u64 token, u32 ready)
 *   NET_LISTEN   addr want | i32 status | u64 listener | addr local
 *   UDP_BIND     addr want | i32 status | u64 socket | addr local
 *   NET_CTL      u8 op (ASX_JOURNAL_NET_CTL_*) | u64 handle | i32 status
 *   NET_ACCEPT   u64 listener | i32 status | u64 stream | addr peer | addr local
 *   NET_CONNECT  addr to | i32 status | u64 stream | addr local
 *   NET_READ     u64 stream | u32 cap | i32 status | u32 n | data
 *   NET_WRITE    u64 stream | u32 len | i32 status | u32 written
 *   UDP_RECV     u64 socket | u32 cap | i32 status | u32 n | addr from | data
 *   UDP_SEND     u64 socket | u32 len | i32 status | u32 written
 *   RESOLVE      u64 name_hash (asx_journal_text_hash of the host) | i32
 *                status | u32 count | count x addr
 *   PROCESS_SPAWN i32 status | u64 handle | u32 pid | u64 command_hash | u8
 *                flags (ASX_JOURNAL_SPAWN_*). command_hash covers the
 *                program, its arguments and the stdio modes, so a replay can
 *                tell a different command from the recorded one.
 *   PROCESS_EXIT u64 handle | u32 state | i32 code | i32 signal. One per
 *                reap attempt on a child not yet reaped: state 0 (running)
 *                when it had not exited, else the exit state, code and
 *                signal. A replay answers each attempt from its record, so
 *                the program polls as many times as it did.
 *   PROCESS_WRITE u64 handle | u32 len | i32 status | u32 written
 *   PROCESS_READ u64 handle | u8 which (1 stdout, 2 stderr) | u32 cap | i32
 *                status | u32 n | data
 *   SIGNAL       u8 n | n x (u8 kind, u32 count). One per drain, n == 0
 *                for a drain that found nothing: a replay serves drains in
 *                order, so the empty ones are part of the stream.
 *   BLOCKING_DONE u32 slot | u16 generation | u64 result
 *   addr         u8 family | 16 bytes | u16 port        (19 bytes)
 *   A boundary call made by a blocking job (a name resolution on the pool)
 *   carries record flag ASX_JOURNAL_REC_OFFTHREAD and no trace position: a
 *   replay holds such records until it runs the job again, in the order the
 *   job's DONE record is reached.
 *   data         FULL mode: n bytes, record flag 0.
 *                SHAPE mode, or n above the record limit: u64 FNV-1a-64 of
 *                the n bytes, record flag ASX_JOURNAL_REC_SHAPE.
 *   Values are the raw boundary result, before the runtime's fault
 *   injection, so a replay re-applies injected faults deterministically.
 *   Native handles and reactor tokens are the runtime's own, never fds.
 *
 * Failure policy: recording never fails or blocks the application. A sink
 * error, a full staging buffer or an oversized record is counted and the
 * first failure sticks in asx_journal_status(); the stream is then marked
 * incomplete and a replay must refuse it.
 *
 * Thread-safety: records may come from any thread (a spin lock orders
 * them). Install before the runtime starts and uninstall after it stops.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_JOURNAL_H
#define ASX_RUNTIME_JOURNAL_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/platform/atomics.h>
#include <asx/security/crypto.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_JOURNAL_VERSION 1u
#define ASX_JOURNAL_HEADER_BYTES 48u
#define ASX_JOURNAL_RECORD_HEAD_BYTES 24u
#define ASX_JOURNAL_CHAIN_PAYLOAD_BYTES 32u
#define ASX_JOURNAL_END_PAYLOAD_BYTES 64u
/* Staging space the journal keeps back so a chain and an end record always
 * fit: one chain record and one end record. */
#define ASX_JOURNAL_RESERVE_BYTES                                                                  \
    ((ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_CHAIN_PAYLOAD_BYTES) +                           \
     (ASX_JOURNAL_RECORD_HEAD_BYTES + ASX_JOURNAL_END_PAYLOAD_BYTES))
/* Smallest staging buffer: the header, the reserve and room for a record. */
#define ASX_JOURNAL_MIN_BUFFER 512u
/* Largest data a record carries in FULL mode; more is stored as SHAPE. */
#define ASX_JOURNAL_MAX_DATA 16384u
#define ASX_JOURNAL_DEFAULT_CHAIN_EVERY 64u
#define ASX_JOURNAL_MAX_SIGNAL_KINDS 32u

/* Header flags. */
#define ASX_JOURNAL_FLAG_SHAPE 0x1u /* reads and receives stored as length + hash */

/* NET_CTL operations. */
#define ASX_JOURNAL_NET_CTL_SHUTDOWN_WRITE 0u /* TCP shutdown(SHUT_WR) */
#define ASX_JOURNAL_NET_CTL_NODELAY 1u        /* TCP_NODELAY */
#define ASX_JOURNAL_NET_CTL_UDP_CONNECT 2u    /* UDP connect */

/* PROCESS_SPAWN flags. */
#define ASX_JOURNAL_SPAWN_PIDFD 0x1u /* the exit is awaited on a pidfd, not a timer */

/* Record flags. */
#define ASX_JOURNAL_REC_SHAPE 0x1u     /* data replaced by its FNV-1a-64 */
#define ASX_JOURNAL_REC_OFFTHREAD 0x2u /* taken off the runtime thread: trace_seq 0 */
#define ASX_JOURNAL_REC_TRUNC 0x4u     /* reactor poll: fewer events stored than count */

typedef enum {
    ASX_JOURNAL_KIND_CLOCK = 1,
    ASX_JOURNAL_KIND_ENTROPY = 2,
    ASX_JOURNAL_KIND_REACTOR_WAIT = 3,
    ASX_JOURNAL_KIND_REACTOR_POLL = 4,
    ASX_JOURNAL_KIND_NET_ACCEPT = 5,
    ASX_JOURNAL_KIND_NET_CONNECT = 6,
    ASX_JOURNAL_KIND_NET_READ = 7,
    ASX_JOURNAL_KIND_NET_WRITE = 8,
    ASX_JOURNAL_KIND_UDP_RECV = 9,
    ASX_JOURNAL_KIND_UDP_SEND = 10,
    ASX_JOURNAL_KIND_RESOLVE = 11,
    ASX_JOURNAL_KIND_PROCESS_SPAWN = 12,
    ASX_JOURNAL_KIND_PROCESS_EXIT = 13,
    ASX_JOURNAL_KIND_SIGNAL = 14,
    ASX_JOURNAL_KIND_BLOCKING_DONE = 15,
    ASX_JOURNAL_KIND_NET_LISTEN = 16,
    ASX_JOURNAL_KIND_UDP_BIND = 17,
    ASX_JOURNAL_KIND_NET_CTL = 18,
    ASX_JOURNAL_KIND_PROCESS_WRITE = 19,
    ASX_JOURNAL_KIND_PROCESS_READ = 20,
    ASX_JOURNAL_KIND_CHAIN = 0xF0,
    ASX_JOURNAL_KIND_END = 0xFF
} asx_journal_kind;

/* Receives the stream in order, in chunks that end on record boundaries.
 * ASX_OK, or the status that says why the bytes were not kept. It runs
 * under the journal's lock, possibly on any thread that records: it must
 * not call anything the journal itself records (the clock, entropy,
 * reactor or network adapters). */
typedef asx_status (*asx_journal_sink_fn)(void *ctx, const uint8_t *bytes, uint32_t len);

typedef struct {
    asx_journal_sink_fn sink_fn; /* required */
    void *sink_ctx;
    uint8_t *buffer;             /* caller-owned staging storage, required */
    uint32_t buffer_cap;         /* >= ASX_JOURNAL_MIN_BUFFER */
    uint32_t flags;              /* ASX_JOURNAL_FLAG_* */
    uint32_t chain_every;        /* data records per chain record; 0 = default */
    uint64_t config_fingerprint; /* stored in the header */
    uint64_t rust_rev_hash;      /* asx_journal_text_hash of the asupersync rev; 0 = unsaid */
    uint64_t build_hash;         /* asx_journal_text_hash of the C build's git sha; 0 = unsaid */
} asx_journal_config;

typedef struct {
    uint64_t records;    /* data records accepted */
    uint64_t bytes;      /* stream bytes produced, staged ones included */
    uint64_t dropped;    /* data records not recorded (sink failure, size) */
    uint64_t chains;     /* chain records written */
    uint32_t sink_calls; /* calls made to the sink */
} asx_journal_stats;

typedef struct {
    asx_journal_config cfg;
    asx_atomic_u32 lock;
    uint32_t used;        /* staged bytes */
    uint32_t staged_data; /* data records staged and not yet handed over */
    uint32_t since_chain; /* data records since the last chain record */
    uint32_t chain_every;
    uint64_t seq;
    asx_journal_stats stats;
    asx_status status; /* first failure, sticky */
    int started;
    int finished;
    uint8_t chain[ASX_SHA256_DIGEST_SIZE];
    asx_sha256_ctx window;
} asx_journal;

/* Start a stream: validates `cfg` and stages the header. ASX_OK, or
 * ASX_E_INVALID_ARGUMENT for a missing sink or buffer, or
 * ASX_E_BUFFER_TOO_SMALL below ASX_JOURNAL_MIN_BUFFER. */
ASX_API ASX_MUST_USE asx_status asx_journal_init(asx_journal *j, const asx_journal_config *cfg);

/* Make `j` the process's active journal. Install before the runtime starts.
 * Returns ASX_OK (also when `j` is already active), ASX_E_INVALID_ARGUMENT
 * for NULL or a journal not initialized, ASX_E_INVALID_STATE when another
 * journal is active. */
ASX_API ASX_MUST_USE asx_status asx_journal_install(asx_journal *j);

/* Stop recording: no journal is active afterwards. Uninstall after the
 * runtime stops; records already staged stay in `j`. */
ASX_API void asx_journal_uninstall(void);

/* Whether a journal is active (nonzero) now. */
ASX_API int asx_journal_active(void);

/* Wrap clock, entropy and reactor wait/poll of `hooks` with recording
 * shims that forward to the originals; installs `j` as active. Reactor
 * register/deregister/notify and the blocking hooks are left alone.
 * ASX_E_ALREADY_EXISTS when `hooks` already carries the shims,
 * ASX_E_INVALID_ARGUMENT for NULLs. */
ASX_API ASX_MUST_USE asx_status asx_journal_wrap_hooks(asx_journal *j, asx_runtime_hooks *hooks);

/* Hand the staged bytes to the sink. */
ASX_API void asx_journal_flush(asx_journal *j);

/* Write the final chain and END records, flush, and stop accepting
 * records. Returns the journal's status: ASX_OK for a complete stream. */
ASX_API ASX_MUST_USE asx_status asx_journal_finish(asx_journal *j);

/* Returns ASX_OK, or the first failure (a sink's status, or
 * ASX_E_BUFFER_TOO_SMALL); the stream is incomplete when not OK.
 * ASX_E_INVALID_ARGUMENT for NULL. */
ASX_API asx_status asx_journal_status(const asx_journal *j);

/* Copy the counters into *out (all zero for a NULL journal). */
ASX_API void asx_journal_get_stats(const asx_journal *j, asx_journal_stats *out);

/* RAM ring sink: keeps the most recent bytes of the stream. For
 * diagnostics and bounded flash writes, not for replay (the head may be
 * gone). */
typedef struct {
    uint8_t *storage;
    uint32_t cap;
    uint32_t head;  /* offset of the oldest byte */
    uint32_t len;   /* bytes held */
    uint64_t total; /* bytes ever offered */
} asx_journal_ring;

/* Start an empty ring over `storage` (caller-owned, `cap` bytes).
 * Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for NULLs or a zero cap. */
ASX_API ASX_MUST_USE asx_status asx_journal_ring_init(asx_journal_ring *r, uint8_t *storage,
                                                      uint32_t cap);

/* An asx_journal_sink_fn; `ctx` is the asx_journal_ring. Keeps the last
 * `cap` bytes offered. Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for a NULL
 * ring or bytes. */
ASX_API asx_status asx_journal_ring_sink(void *ctx, const uint8_t *bytes, uint32_t len);

/* Copy up to `cap` of the held bytes, oldest first; returns the count. */
ASX_API uint32_t asx_journal_ring_read(const asx_journal_ring *r, uint8_t *dst, uint32_t cap);

/* -------------------------------------------------------------------
 * Reader: validates a complete stream and walks its records.
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint64_t seq;
    uint64_t trace_seq;
    const uint8_t *payload;
    uint32_t payload_len;
} asx_journal_record;

typedef enum {
    ASX_JOURNAL_READ_RECORD = 0,    /* *out is the next data record */
    ASX_JOURNAL_READ_END = 1,       /* a valid END: the stream is complete and intact */
    ASX_JOURNAL_READ_TRUNCATED = 2, /* bytes end inside a record or before END */
    ASX_JOURNAL_READ_CORRUPT = 3,   /* chain, END totals or seq do not match */
    ASX_JOURNAL_READ_BAD_HEADER = 4
} asx_journal_read_result;

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t pos;
    uint32_t flags;
    uint64_t config_fingerprint;
    uint64_t rust_rev_hash;
    uint64_t build_hash;
    uint64_t next_seq;
    uint64_t data_records;
    uint64_t chains;
    uint32_t since_chain; /* data records since the last chain record */
    int ended;
    /* From the END record, once read. */
    uint64_t end_dropped; /* records the recorder dropped */
    int32_t end_status;   /* the recorder's sticky status */
    /* Where a failed read stopped: the seq of the record that failed (or
     * the first missing one), and the first data record of the chain
     * window it belongs to. A chain break lies in [first, seq). */
    uint64_t window_first_seq;
    uint64_t fail_seq;
    uint64_t fail_first_seq;
    uint8_t chain[ASX_SHA256_DIGEST_SIZE];
    asx_sha256_ctx window;
} asx_journal_reader;

/* Validate the header of `bytes` and position at the first record. Returns
 * ASX_JOURNAL_READ_RECORD (ready) or ASX_JOURNAL_READ_BAD_HEADER /
 * ASX_JOURNAL_READ_TRUNCATED. */
ASX_API asx_journal_read_result asx_journal_reader_init(asx_journal_reader *r, const uint8_t *bytes,
                                                        size_t len);
/* The next data record; CHAIN records are verified and skipped, END is
 * verified and reported. */
ASX_API asx_journal_read_result asx_journal_reader_next(asx_journal_reader *r,
                                                        asx_journal_record *out);

/* FNV-1a-64 of a NUL-terminated string (0 for NULL): the form the header's
 * rust_rev_hash and build_hash take. */
ASX_API uint64_t asx_journal_text_hash(const char *text);

/* Little-endian field access for payload decoding. */
ASX_API uint64_t asx_journal_get_u64(const uint8_t *p);
ASX_API uint32_t asx_journal_get_u32(const uint8_t *p);
ASX_API uint16_t asx_journal_get_u16(const uint8_t *p);

/* -------------------------------------------------------------------
 * Boundary notes: called by the native adapters, no-ops with no active
 * journal. `status` is the asx_status returned to the caller as int32_t.
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t family;
    uint8_t bytes[16];
    uint16_t port;
} asx_journal_addr;

/* A TCP listen on `want` returned `status` (and the `listener` it created,
 * bound to `local`: the port an ephemeral bind got). */
ASX_API void asx_journal_note_net_listen(const asx_journal_addr *want, int32_t status,
                                         uint64_t listener, const asx_journal_addr *local);

/* A UDP bind to `want` returned `status` (and the `socket` it created,
 * bound to `local`). */
ASX_API void asx_journal_note_udp_bind(const asx_journal_addr *want, int32_t status,
                                       uint64_t socket, const asx_journal_addr *local);

/* A socket control call (`op` is an ASX_JOURNAL_NET_CTL_*) on `handle`
 * returned `status`. */
ASX_API void asx_journal_note_net_ctl(uint8_t op, uint64_t handle, int32_t status);

/* A TCP accept returned `status` (and `stream`, its `peer` and `local`
 * address). */
ASX_API void asx_journal_note_net_accept(uint64_t listener, int32_t status, uint64_t stream,
                                         const asx_journal_addr *peer,
                                         const asx_journal_addr *local);

/* A TCP connect to `to` returned `status` (and the `stream` it created,
 * bound to `local`). */
ASX_API void asx_journal_note_net_connect(const asx_journal_addr *to, int32_t status,
                                          uint64_t stream, const asx_journal_addr *local);

/* A TCP read into a buffer of `cap` bytes returned `status` and `n` bytes
 * at `data`: stored as FULL bytes or as their length and hash (SHAPE). */
ASX_API void asx_journal_note_net_read(uint64_t stream, uint32_t cap, int32_t status,
                                       const uint8_t *data, uint32_t n);

/* A TCP write of `len` bytes returned `status` and `written` bytes. */
ASX_API void asx_journal_note_net_write(uint64_t stream, uint32_t len, int32_t status,
                                        uint32_t written);

/* A UDP receive into `cap` bytes returned `status`, `n` bytes at `data` and
 * the sender `from`; stored as for a TCP read. */
ASX_API void asx_journal_note_udp_recv(uint64_t socket, uint32_t cap, int32_t status,
                                       const uint8_t *data, uint32_t n,
                                       const asx_journal_addr *from);

/* A UDP send of `len` bytes returned `status` and `written` bytes. */
ASX_API void asx_journal_note_udp_send(uint64_t socket, uint32_t len, int32_t status,
                                       uint32_t written);

/* A resolution of the host whose asx_journal_text_hash is `name_hash`
 * returned `status` and `count` addresses (at most 16 are stored). */
ASX_API void asx_journal_note_resolve(uint64_t name_hash, int32_t status,
                                      const asx_journal_addr *addrs, uint32_t count);

/* A resolution made by a blocking job (on the pool, or inline where there
 * is none): as asx_journal_note_resolve, flagged ASX_JOURNAL_REC_OFFTHREAD
 * and carrying no trace position. */
ASX_API void asx_journal_note_resolve_job(uint64_t name_hash, int32_t status,
                                          const asx_journal_addr *addrs, uint32_t count);

/* A child spawn of the command whose hash is `command_hash` returned
 * `status`, the process `handle` and its `pid`; `flags` are
 * ASX_JOURNAL_SPAWN_*. */
ASX_API void asx_journal_note_process_spawn(int32_t status, uint64_t handle, uint32_t pid,
                                            uint64_t command_hash, uint8_t flags);

/* A write of `len` bytes to a child's stdin returned `status` and `written`
 * bytes. */
ASX_API void asx_journal_note_process_write(uint64_t handle, uint32_t len, int32_t status,
                                            uint32_t written);

/* A read of up to `cap` bytes from a child's stdout (`which` 1) or stderr
 * (2) returned `status` and `n` bytes at `data`: stored as for a TCP read. */
ASX_API void asx_journal_note_process_read(uint64_t handle, uint8_t which, uint32_t cap,
                                           int32_t status, const uint8_t *data, uint32_t n);

/* A reap attempt on the child `handle`: its `state` (0, running, when it
 * had not exited yet), exit `code` and terminating `signal`. */
ASX_API void asx_journal_note_process_exit(uint64_t handle, uint32_t state, int32_t code,
                                           int32_t signal);

/* A signal drain found `counts[kind]` arrivals for each of `kinds` kinds
 * (at most ASX_JOURNAL_MAX_SIGNAL_KINDS); a drain that found nothing is
 * recorded too. */
ASX_API void asx_journal_note_signal(const uint32_t *counts, uint32_t kinds);

/* A blocking-pool job (`slot`, `generation`) returned `result`. Called on
 * the pool thread: the record carries no trace position. */
ASX_API void asx_journal_note_blocking_done(uint32_t slot, uint16_t generation, uint64_t result);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_JOURNAL_H */
