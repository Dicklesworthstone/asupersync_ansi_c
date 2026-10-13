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

#include <asx/asx_config.h>
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

/* ASX_EVENT_LOG_CAPACITY, the events the log stores, is a capacity macro of
 * asx_config.h: a resource class sizes it. */

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
    /* Index of the first differing event when one of the compared events
     * differs; otherwise (the total or the chain differs) the first index
     * not compared, the runs differing at or after it. */
    uint32_t first_divergence_index;
    asx_event_record expected; /* what was expected (a differing event) */
    asx_event_record actual;   /* what was observed */
    int diverged;              /* 1 if any mismatch was found */
    int count_mismatch;        /* 1 if the total number of events differs */
    int chain_mismatch;        /* 1 if the rolling hash chains differ */
    uint32_t expected_count;   /* events the expected run emitted */
    uint32_t actual_count;     /* events this run emitted, stored or not */
    uint64_t expected_chain;
    uint64_t actual_chain;
    uint32_t compared; /* events compared one by one (the stored window) */
} asx_replay_divergence;

/*
 * Verify this run against a reference run.
 *
 * The log stores only the first ASX_EVENT_LOG_CAPACITY events of a run;
 * the count and the hash chain cover every event it emitted. So a run is
 * verified by three things together, and ASX_OK needs all of them:
 *   - the total number of events (`expected_total`, the reference run's
 *     asx_event_log_count());
 *   - the rolling hash chain (`expected_chain`, its asx_event_hash_chain()),
 *     which covers each event's kind, entity and sequence, stored or not,
 *     but not its parent or status;
 *   - the stored events, compared one by one: `expected` holds the first
 *     `expected_events` (<= expected_total) events of the reference run,
 *     of which min(expected_events, stored) are compared.
 * A reference prefix that matches therefore does not verify a run that
 * emits more or different events after it: the total or the chain differs.
 * Returns ASX_OK if all three agree, ASX_E_EQUIVALENCE_MISMATCH if not
 * (with the first differing stored event in `divergence`, or the first
 * index not compared), ASX_E_INVALID_ARGUMENT for bad arguments.
 */
ASX_API ASX_MUST_USE asx_status asx_event_replay_verify(const asx_event_record *expected,
                                                        uint32_t expected_events,
                                                        uint32_t expected_total,
                                                        uint64_t expected_chain,
                                                        asx_replay_divergence *divergence);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_EVENT_H */
