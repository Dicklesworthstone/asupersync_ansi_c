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

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>

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
    ASX_TRACE_SCHED_POLL       = 0x00,  /* task polled */
    ASX_TRACE_SCHED_COMPLETE   = 0x01,  /* task completed (OK or error) */
    ASX_TRACE_SCHED_BUDGET     = 0x02,  /* poll budget exhausted */
    ASX_TRACE_SCHED_QUIESCENT  = 0x03,  /* all tasks complete */
    ASX_TRACE_SCHED_ROUND      = 0x04,  /* new scheduler round begins */

    /* Lifecycle events (0x10–0x1F) */
    ASX_TRACE_REGION_OPEN      = 0x10,
    ASX_TRACE_REGION_CLOSE     = 0x11,
    ASX_TRACE_REGION_CLOSED    = 0x12,
    ASX_TRACE_TASK_SPAWN       = 0x13,
    ASX_TRACE_TASK_TRANSITION  = 0x14,

    /* Obligation events (0x20–0x2F) */
    ASX_TRACE_OBLIGATION_RESERVE = 0x20,
    ASX_TRACE_OBLIGATION_COMMIT  = 0x21,
    ASX_TRACE_OBLIGATION_ABORT   = 0x22,

    /* Channel events (0x30–0x3F) */
    ASX_TRACE_CHANNEL_SEND     = 0x30,
    ASX_TRACE_CHANNEL_RECV     = 0x31,

    /* Timer events (0x40–0x4F) */
    ASX_TRACE_TIMER_SET        = 0x40,
    ASX_TRACE_TIMER_FIRE       = 0x41,
    ASX_TRACE_TIMER_CANCEL     = 0x42
} asx_trace_event_kind;

/* -------------------------------------------------------------------
 * Trace event record
 *
 * Each event carries a monotonic sequence number, the event kind,
 * an entity handle (task/region/obligation/channel/timer ID), and
 * an auxiliary payload field whose meaning depends on the kind.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t              sequence;    /* monotonic, 0-based per trace */
    asx_trace_event_kind  kind;
    uint64_t              entity_id;   /* handle of primary entity */
    uint64_t              aux;         /* kind-dependent payload */
} asx_trace_event;

/* -------------------------------------------------------------------
 * Trace ring buffer capacity
 * ------------------------------------------------------------------- */

#define ASX_TRACE_CAPACITY 1024u

/* -------------------------------------------------------------------
 * Trace emission API
 *
 * Events are recorded into a global ring buffer. Scheduler and
 * parallel-run invocations append to the current trace until the
 * caller explicitly resets it with asx_trace_reset().
 * ------------------------------------------------------------------- */

/* Emit a trace event. Thread-safe: none (single-threaded runtime). */
ASX_API void asx_trace_emit(asx_trace_event_kind kind,
                             uint64_t entity_id,
                             uint64_t aux);

/* Read total event count since last reset. */
ASX_API uint32_t asx_trace_event_count(void);

/* Read event at index (0 = oldest). Returns 1 on success, 0 on OOB. */
ASX_API int asx_trace_event_get(uint32_t index, asx_trace_event *out);

/* Reset trace state before starting a fresh scenario or replay window. */
ASX_API void asx_trace_reset(void);

/* -------------------------------------------------------------------
 * Hash-chain digest
 *
 * Computes a rolling digest over the trace event stream. The digest
 * is deterministic for identical event sequences. Uses FNV-1a 64-bit
 * for the walking skeleton; Phase 4 will add SHA-256 for fixture
 * parity with the Rust reference implementation.
 * ------------------------------------------------------------------- */

/* Compute the current trace digest (FNV-1a over all events). */
ASX_API uint64_t asx_trace_digest(void);

/* -------------------------------------------------------------------
 * Replay verification mode
 *
 * Load a reference event sequence, then run the scenario. After
 * completion, check for divergence between emitted and expected
 * events. The first divergence index is reported.
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_REPLAY_MATCH           = 0,  /* sequences match */
    ASX_REPLAY_LENGTH_MISMATCH = 1,  /* different event counts */
    ASX_REPLAY_KIND_MISMATCH   = 2,  /* event kind differs */
    ASX_REPLAY_ENTITY_MISMATCH = 3,  /* entity ID differs */
    ASX_REPLAY_AUX_MISMATCH    = 4,  /* aux payload differs */
    ASX_REPLAY_DIGEST_MISMATCH = 5   /* final digest differs */
} asx_replay_result_kind;

typedef struct {
    asx_replay_result_kind  result;
    uint32_t                divergence_index;  /* first mismatch position */
    uint64_t                expected_digest;
    uint64_t                actual_digest;
} asx_replay_result;

/* Load reference events for replay comparison. The events array is
 * copied internally. Returns ASX_OK on success. */
ASX_API asx_status asx_replay_load_reference(const asx_trace_event *events,
                                              uint32_t count);

/* Clear reference events. */
ASX_API void asx_replay_clear_reference(void);

/* Compare the current trace against the loaded reference.
 * Returns the comparison result. */
ASX_API asx_replay_result asx_replay_verify(void);

/* -------------------------------------------------------------------
 * Snapshot export
 *
 * Capture a point-in-time snapshot of the runtime state into a
 * machine-readable buffer. Used for conformance testing and
 * expected_final_snapshot fixture fields.
 * ------------------------------------------------------------------- */

#define ASX_SNAPSHOT_BUFFER_SIZE 4096u

typedef struct {
    char     data[ASX_SNAPSHOT_BUFFER_SIZE];
    uint32_t len;
} asx_snapshot_buffer;

/* Capture a snapshot of the current runtime state.
 * Writes a JSON object with regions, tasks, and obligations. */
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
 *   Header (24 bytes):
 *     [0..3]   magic      "ASXt" (0x41535874)
 *     [4..7]   version    1
 *     [8..11]  event_count
 *     [12..15] reserved   0
 *     [16..23] trace_digest (FNV-1a 64-bit)
 *
 *   Per event (24 bytes each):
 *     [0..3]   sequence   (uint32)
 *     [4..7]   kind       (uint32)
 *     [8..15]  entity_id  (uint64)
 *     [16..23] aux        (uint64)
 * ------------------------------------------------------------------- */

#define ASX_TRACE_BINARY_MAGIC    0x41535874u  /* "ASXt" */
#define ASX_TRACE_BINARY_VERSION  1u
#define ASX_TRACE_BINARY_HEADER   24u
#define ASX_TRACE_BINARY_EVENT    24u

/* Export the current trace to a binary buffer.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if buf/out_len
 * is NULL, ASX_E_BUFFER_TOO_SMALL if capacity insufficient.
 * On success, *out_len receives the number of bytes written. */
ASX_API asx_status asx_trace_export_binary(uint8_t *buf,
                                            uint32_t capacity,
                                            uint32_t *out_len);

/* Import a binary trace buffer as the replay reference.
 * Validates header magic, version, and digest. On success, the
 * events are loaded as the replay reference (same as
 * asx_replay_load_reference). Returns ASX_OK on success. */
ASX_API asx_status asx_trace_import_binary(const uint8_t *buf,
                                            uint32_t len);

/* Check whether the current trace matches a persisted binary buffer.
 * Combines import + verify in one call. Returns ASX_OK if the
 * current trace matches the persisted events. */
ASX_API asx_status asx_trace_continuity_check(const uint8_t *buf,
                                               uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_TRACE_H */
