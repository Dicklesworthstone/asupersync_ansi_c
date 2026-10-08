/*
 * asx/runtime/trace.h — deterministic event trace, replay, and snapshot
 *
 * Provides a unified trace system that records all runtime events into
 * a deterministic sequence with hash-chain digest. Supports replay
 * verification (compare emitted events against expected) and runtime
 * snapshot export for conformance testing.
 *
 * Event ordering is deterministic for identical input and seed —
 * suitable for replay identity verification across runs and platforms.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_TRACE_H
#define ASX_RUNTIME_TRACE_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE ||                                      \
    defined(ASX_INTERNAL_TRACE_FAMILY_ACCESS)

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Trace event kinds — comprehensive runtime event taxonomy
 *
 * Covers scheduler, resource-plane, and lifecycle events.
 * The integer values are stable for wire format compatibility.
 * ------------------------------------------------------------------- */

typedef enum {
    /* Scheduler events (0x00–0x0F) */
    ASX_TRACE_SCHED_POLL = 0x00,      /* task polled */
    ASX_TRACE_SCHED_COMPLETE = 0x01,  /* task completed (OK or error) */
    ASX_TRACE_SCHED_BUDGET = 0x02,    /* poll budget exhausted */
    ASX_TRACE_SCHED_QUIESCENT = 0x03, /* all tasks complete */
    ASX_TRACE_SCHED_ROUND = 0x04,     /* new scheduler round begins */

    /* Lifecycle events (0x10–0x1F) */
    ASX_TRACE_REGION_OPEN = 0x10,
    ASX_TRACE_REGION_CLOSE = 0x11,
    ASX_TRACE_REGION_CLOSED = 0x12,
    ASX_TRACE_TASK_SPAWN = 0x13,
    ASX_TRACE_TASK_TRANSITION = 0x14,

    /* Obligation events (0x20–0x2F) */
    ASX_TRACE_OBLIGATION_RESERVE = 0x20,
    ASX_TRACE_OBLIGATION_COMMIT = 0x21,
    ASX_TRACE_OBLIGATION_ABORT = 0x22,

    /* Channel events (0x30–0x3F) */
    ASX_TRACE_CHANNEL_SEND = 0x30,
    ASX_TRACE_CHANNEL_RECV = 0x31,

    /* Timer events (0x40–0x4F) */
    ASX_TRACE_TIMER_SET = 0x40,
    ASX_TRACE_TIMER_FIRE = 0x41,
    ASX_TRACE_TIMER_CANCEL = 0x42
} asx_trace_event_kind;

/* -------------------------------------------------------------------
 * Trace event record
 *
 * Each event carries a monotonic sequence number, the event kind,
 * an entity handle (task/region/obligation/channel/timer ID), and
 * an auxiliary payload field whose meaning depends on the kind.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t sequence; /* monotonic, 0-based per trace */
    asx_trace_event_kind kind;
    uint64_t entity_id; /* handle of primary entity */
    uint64_t aux;       /* kind-dependent payload */
} asx_trace_event;

/* -------------------------------------------------------------------
 * Trace event schema contract
 *
 * Versioned schema metadata is used by conformance fixtures, incident
 * bundles, and minimizers to reject ambiguous trace-event changes before
 * they can be compared as semantic evidence.
 * ------------------------------------------------------------------- */

#define ASX_TRACE_SCHEMA_NAME "asx.trace_event"
#define ASX_TRACE_SCHEMA_VERSION "asx.trace_event.v1"
#define ASX_TRACE_SCHEMA_VERSION_MAJOR 1u
#define ASX_TRACE_SCHEMA_VERSION_MINOR 0u
#define ASX_TRACE_SCHEMA_VERSION_PATCH 0u

typedef enum {
    ASX_TRACE_SCHEMA_FIELD_SEQUENCE = 1u << 0,
    ASX_TRACE_SCHEMA_FIELD_KIND = 1u << 1,
    ASX_TRACE_SCHEMA_FIELD_ENTITY_ID = 1u << 2,
    ASX_TRACE_SCHEMA_FIELD_AUX = 1u << 3,
    ASX_TRACE_SCHEMA_FIELD_SCHEMA_VERSION = 1u << 4,
    ASX_TRACE_SCHEMA_FIELD_PRODUCER = 1u << 5,
    ASX_TRACE_SCHEMA_FIELD_RUST_CORRELATION = 1u << 6
} asx_trace_schema_field;

#define ASX_TRACE_SCHEMA_REQUIRED_EVENT_FIELDS                                                     \
    (ASX_TRACE_SCHEMA_FIELD_SEQUENCE | ASX_TRACE_SCHEMA_FIELD_KIND |                               \
     ASX_TRACE_SCHEMA_FIELD_ENTITY_ID | ASX_TRACE_SCHEMA_FIELD_AUX)

#define ASX_TRACE_SCHEMA_OPTIONAL_EVENT_FIELDS                                                     \
    (ASX_TRACE_SCHEMA_FIELD_SCHEMA_VERSION | ASX_TRACE_SCHEMA_FIELD_PRODUCER |                     \
     ASX_TRACE_SCHEMA_FIELD_RUST_CORRELATION)

#define ASX_TRACE_SCHEMA_DIGEST_EVENT_FIELDS ASX_TRACE_SCHEMA_REQUIRED_EVENT_FIELDS

typedef enum {
    ASX_TRACE_SCHEMA_COMPAT_EXACT = 0,
    ASX_TRACE_SCHEMA_COMPAT_ADDITIVE_OPTIONAL = 1,
    ASX_TRACE_SCHEMA_COMPAT_INCOMPATIBLE = 2
} asx_trace_schema_compat;

typedef struct {
    const char *schema_name;
    const char *schema_version;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t required_event_fields;
    uint32_t optional_event_fields;
    uint32_t digest_event_fields;
} asx_trace_schema_descriptor;

/* Read the current trace event schema descriptor. */
ASX_API ASX_MUST_USE asx_status asx_trace_schema_current(asx_trace_schema_descriptor *out);

/* Compare producer and consumer schema descriptors for event compatibility. */
ASX_API ASX_MUST_USE asx_trace_schema_compat asx_trace_schema_compatibility(
    const asx_trace_schema_descriptor *producer, const asx_trace_schema_descriptor *consumer);

/* Return human-readable name for a trace schema compatibility result. */
ASX_API const char *asx_trace_schema_compat_str(asx_trace_schema_compat compat);

/* -------------------------------------------------------------------
 * Trace ring buffer capacity
 * ------------------------------------------------------------------- */

/* Number of most-recent events the ring retains. The digest and the
 * per-kind totals cover every emitted event regardless of this size. */
#ifndef ASX_TRACE_CAPACITY
#define ASX_TRACE_CAPACITY 1024u
#endif
#if (ASX_TRACE_CAPACITY) < 1
#error "ASX_TRACE_CAPACITY must be at least 1"
#endif

/* -------------------------------------------------------------------
 * Trace emission API
 *
 * Events are recorded into a global ring buffer that keeps the most
 * recent ASX_TRACE_CAPACITY events. Scheduler and parallel-run
 * invocations append to the current trace until the caller explicitly
 * resets it with asx_trace_reset().
 * ------------------------------------------------------------------- */

/* Emit a trace event. Thread-safe: none (single-threaded runtime). */
ASX_API void asx_trace_emit(asx_trace_event_kind kind, uint64_t entity_id, uint64_t aux);

/* Number of events currently retained in the ring (<= ASX_TRACE_CAPACITY). */
ASX_API uint32_t asx_trace_event_count(void);

/* Total events emitted since the last reset, retained or not. */
ASX_API uint64_t asx_trace_emitted_total(void);

/* Events emitted since the last reset that the ring no longer retains. */
ASX_API uint64_t asx_trace_dropped(void);

/* Events of one kind emitted since the last reset (exact, never dropped). */
ASX_API uint64_t asx_trace_kind_total(asx_trace_event_kind kind);

/* Read the retained event at index (0 = oldest retained). Returns 1 on
 * success, 0 when index >= asx_trace_event_count(). */
ASX_API int asx_trace_event_get(uint32_t index, asx_trace_event *out);

/* Read the event with absolute sequence number seq (0 = first event since
 * reset). Returns 0 if seq was not emitted yet or is no longer retained. */
ASX_API int asx_trace_event_get_seq(uint64_t seq, asx_trace_event *out);

/* Reset trace state before starting a fresh scenario or replay window. */
ASX_API void asx_trace_reset(void);

/* -------------------------------------------------------------------
 * Rolling digest
 *
 * Every emitted event is folded into the digest when it is emitted, so
 * the digest covers the whole trace, not just the retained ring. It is
 * deterministic for identical event sequences: FNV-1a mixing of each
 * event's (sequence, kind, entity_id, aux), little-endian, starting from
 * the asx digest seed 0x517cc1b727220a95. This is the C runtime's fast
 * self-consistency digest; cross-engine parity with the Rust reference
 * uses the canonical vocabulary digest instead.
 * ------------------------------------------------------------------- */

/* Current rolling digest over every event emitted since the last reset. */
ASX_API uint64_t asx_trace_digest(void);

/* -------------------------------------------------------------------
 * Replay verification mode
 *
 * Load a reference event sequence, then run the scenario. After
 * completion, check for divergence between emitted and expected
 * events. The first divergence index is reported.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_REPLAY_MATCH = 0,           /* sequences match */
    ASX_REPLAY_LENGTH_MISMATCH = 1, /* different event counts */
    ASX_REPLAY_KIND_MISMATCH = 2,   /* event kind differs */
    ASX_REPLAY_ENTITY_MISMATCH = 3, /* entity ID differs */
    ASX_REPLAY_AUX_MISMATCH = 4,    /* aux payload differs */
    ASX_REPLAY_DIGEST_MISMATCH = 5  /* final digest differs */
} asx_replay_result_kind;

typedef struct {
    asx_replay_result_kind result;
    uint32_t divergence_index; /* first mismatch position */
    uint64_t expected_digest;
    uint64_t actual_digest;
} asx_replay_result;

/* Load reference events for replay comparison. The events array is
 * copied internally. Returns ASX_OK on success. */
ASX_API asx_status asx_replay_load_reference(const asx_trace_event *events, uint32_t count);

/* Clear reference events. */
ASX_API void asx_replay_clear_reference(void);

/* Compare the current trace against the loaded reference: equal emitted
 * totals, equal events wherever both sides retain them (aligned by
 * sequence), and equal rolling digests. divergence_index is the sequence
 * of the first mismatching event. */
ASX_API asx_replay_result asx_replay_verify(void);

/* Number of reference events held (the reference window). */
ASX_API uint32_t asx_replay_reference_event_count(void);

/* Read reference event at window index (0 = oldest held). Returns 1 on
 * success, 0 on OOB. */
ASX_API int asx_replay_reference_event_get(uint32_t index, asx_trace_event *out);

/* Read the reference event with absolute sequence seq. Returns 0 if the
 * reference window does not hold it. */
ASX_API int asx_replay_reference_event_get_seq(uint64_t seq, asx_trace_event *out);

/* -------------------------------------------------------------------
 * Snapshot export
 *
 * Capture a point-in-time snapshot of the runtime state into a
 * machine-readable buffer. Used for conformance testing and
 * expected_final_snapshot fixture fields.
 * ------------------------------------------------------------------- */

#define ASX_SNAPSHOT_BUFFER_SIZE 4096u

typedef struct {
    char data[ASX_SNAPSHOT_BUFFER_SIZE];
    uint32_t len;
} asx_snapshot_buffer;

/* Capture a snapshot of the current runtime state.
 * Writes a JSON object with subsystem state plus regions, tasks, and
 * obligations. */
ASX_API asx_status asx_snapshot_capture(asx_snapshot_buffer *out);

/* Compute digest of a snapshot buffer (FNV-1a). */
ASX_API uint64_t asx_snapshot_digest(const asx_snapshot_buffer *snap);

/* Return human-readable name for a trace event kind. */
ASX_API const char *asx_trace_event_kind_str(asx_trace_event_kind kind);

/* Return human-readable name for a replay result kind. */
ASX_API const char *asx_replay_result_kind_str(asx_replay_result_kind kind);

/* -------------------------------------------------------------------
 * Binary trace persistence (bd-2n0.5)
 *
 * Provides a platform-independent binary wire format for persisting
 * trace event sequences across process boundaries. Used for crash/
 * restart replay continuity: export before shutdown, import after
 * restart, then verify the replayed scenario matches.
 *
 * Wire format (little-endian):
 *   Header (32 bytes):
 *     [0..3]   magic         "ASXt" (0x41535874)
 *     [4..7]   version       2
 *     [8..11]  event_count   events in this file (the retained window)
 *     [12..15] flags         bit 0: TRUNCATED (older events were dropped)
 *     [16..23] trace_digest  rolling digest over the whole trace
 *     [24..31] total_emitted events emitted in the whole trace
 *
 *   Per event (24 bytes each), oldest first; sequences are the
 *   contiguous tail [total_emitted - event_count, total_emitted):
 *     [0..3]   sequence   (uint32)
 *     [4..7]   kind       (uint32)
 *     [8..15]  entity_id  (uint64)
 *     [16..23] aux        (uint64)
 * ------------------------------------------------------------------- */

#define ASX_TRACE_BINARY_MAGIC 0x41535874u /* "ASXt" */
#define ASX_TRACE_BINARY_VERSION 2u
#define ASX_TRACE_BINARY_HEADER 32u
#define ASX_TRACE_BINARY_EVENT 24u
#define ASX_TRACE_BINARY_FLAG_TRUNCATED 0x1u

/* Export the current trace to a binary buffer.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if buf/out_len
 * is NULL, ASX_E_BUFFER_TOO_SMALL if capacity insufficient.
 * On success, *out_len receives the number of bytes written. */
ASX_API asx_status asx_trace_export_binary(uint8_t *buf, uint32_t capacity, uint32_t *out_len);

/* Import a binary trace buffer as the replay reference.
 * Validates header magic, version, and digest. On success, the
 * events are loaded as the replay reference (same as
 * asx_replay_load_reference). Returns ASX_OK on success. */
ASX_API asx_status asx_trace_import_binary(const uint8_t *buf, uint32_t len);

/* Check whether the current trace matches a persisted binary buffer.
 * Combines import + verify in one call. Returns ASX_OK if the
 * current trace matches the persisted events. */
ASX_API asx_status asx_trace_continuity_check(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* trace family public contract */

#endif /* ASX_RUNTIME_TRACE_H */
