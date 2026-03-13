/*
 * asx/runtime/event.h — deterministic event recording with hash-chain
 *
 * Extends the scheduler event model with operation-level lifecycle events
 * and a running FNV-1a hash chain for deterministic replay verification.
 * The hash chain digests event (kind, entity, sequence) tuples, producing
 * a single 64-bit fingerprint that captures the full execution ordering.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_EVENT_H
#define ASX_RUNTIME_EVENT_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/codec/codec.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Operation-level event kinds                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_EVENT_REGION_OPEN = 0,
    ASX_EVENT_REGION_CLOSE = 1,
    ASX_EVENT_TASK_SPAWN = 2,
    ASX_EVENT_TASK_POLL = 3,
    ASX_EVENT_TASK_COMPLETE = 4,
    ASX_EVENT_OBLIGATION_CREATE = 5,
    ASX_EVENT_OBLIGATION_COMMIT = 6,
    ASX_EVENT_OBLIGATION_ABORT = 7,
    ASX_EVENT_BUDGET_EXHAUSTED = 8,
    ASX_EVENT_QUIESCENT = 9,
    ASX_EVENT_DRAIN_BEGIN = 10,
    ASX_EVENT_DRAIN_END = 11,
    ASX_EVENT_KIND_COUNT = 12
} asx_event_kind;

/* ------------------------------------------------------------------ */
/* Event record                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_event_kind kind;
    uint64_t entity_id; /* region, task, or obligation handle */
    uint64_t parent_id; /* parent entity (region for task, etc.) */
    uint32_t sequence;  /* global monotonic sequence */
    asx_status status;  /* ASX_OK for success events */
} asx_event_record;

enum { ASX_EVENT_LOG_CAPACITY = 512u };

/* ------------------------------------------------------------------ */
/* Event log API                                                       */
/* ------------------------------------------------------------------ */

/* Reset event log and hash chain. Call before scenario execution. */
ASX_API void asx_event_log_reset(void);

/* Emit an event into the log. Returns the sequence number assigned. */
ASX_API uint32_t asx_event_emit(asx_event_kind kind, uint64_t entity_id, uint64_t parent_id,
                                asx_status status);

/* Total events emitted since last reset. */
ASX_API uint32_t asx_event_log_count(void);

/* Read event at index (0-based). Returns 1 on success, 0 on OOB. */
ASX_API int asx_event_log_get(uint32_t index, asx_event_record *out);

/* ------------------------------------------------------------------ */
/* Hash chain for deterministic replay verification                    */
/* ------------------------------------------------------------------ */

/* Read the running hash chain digest. */
ASX_API uint64_t asx_event_hash_chain(void);

/* ------------------------------------------------------------------ */
/* Event serialization                                                 */
/* ------------------------------------------------------------------ */

/* Serialize all recorded events to JSON array.
 * Returns ASX_OK on success, ASX_E_INVALID_ARGUMENT if out is NULL. */
ASX_API ASX_MUST_USE asx_status asx_event_log_to_json(asx_codec_buffer *out);

/* Return human-readable string for an event kind. */
ASX_API const char *asx_event_kind_str(asx_event_kind kind);

/* ------------------------------------------------------------------ */
/* Replay verification                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t first_divergence_index; /* index of first mismatched event */
    asx_event_record expected;       /* what was expected */
    asx_event_record actual;         /* what was observed */
    int diverged;                    /* 1 if mismatch found */
    int count_mismatch;              /* 1 if event counts differ */
    uint32_t expected_count;
    uint32_t actual_count;
} asx_replay_divergence;

/*
 * Compare recorded events against an expected sequence.
 * Returns ASX_OK if identical, ASX_E_EQUIVALENCE_MISMATCH if not.
 * Populates divergence with first-divergence diagnostics.
 */
ASX_API ASX_MUST_USE asx_status asx_event_replay_verify(const asx_event_record *expected,
                                                        uint32_t expected_count,
                                                        asx_replay_divergence *divergence);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_EVENT_H */
