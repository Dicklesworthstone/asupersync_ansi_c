/*
 * asx/runtime/hindsight.h — hindsight nondeterminism logging ring
 *
 * Bounded ring buffer that captures nondeterministic boundary events
 * (external IO readiness, signal arrivals, clock reads, entropy access,
 * scheduling tie-break decisions). On invariant failure or explicit
 * debug trigger, the ring is flushed to provide complete divergence
 * context for replay diagnostics.
 *
 * Steady-state overhead: one branch per log site (disabled → zero cost).
 * Enabled in ASX_DEBUG builds and CI; controlled by ASX_HINDSIGHT flag.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_HINDSIGHT_H
#define ASX_RUNTIME_HINDSIGHT_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Hindsight ring capacity (configurable at init time)
 * ------------------------------------------------------------------- */

#define ASX_HINDSIGHT_CAPACITY 256u

/* -------------------------------------------------------------------
 * Nondeterminism event kinds
 *
 * Each kind identifies a boundary where external nondeterminism
 * enters the runtime. The integer values are stable for artifact
 * format compatibility.
 * ------------------------------------------------------------------- */

typedef enum {
    /* Clock boundary (0x00–0x0F) */
    ASX_ND_CLOCK_READ = 0x00, /* wall/monotonic clock read */
    ASX_ND_CLOCK_SKEW = 0x01, /* detected clock skew or reversal */

    /* Entropy boundary (0x10–0x1F) */
    ASX_ND_ENTROPY_READ = 0x10, /* PRNG / entropy source read */

    /* IO readiness boundary (0x20–0x2F) */
    ASX_ND_IO_READY = 0x20,   /* reactor reported IO readiness */
    ASX_ND_IO_TIMEOUT = 0x21, /* reactor wait timed out */

    /* Signal boundary (0x30–0x3F) */
    ASX_ND_SIGNAL_ARRIVAL = 0x30, /* OS signal delivered */

    /* Scheduling tie-break (0x40–0x4F) */
    ASX_ND_SCHED_TIE_BREAK = 0x40, /* scheduler chose among equal-priority tasks */

    /* Timer resolution (0x50–0x5F) */
    ASX_ND_TIMER_COALESCE = 0x50, /* timer coalescing decision */

    /* Sentinel */
    ASX_ND_KIND_COUNT
} asx_nd_event_kind;

/* -------------------------------------------------------------------
 * Hindsight event record
 *
 * Each event captures what happened, when (trace sequence at time of
 * logging), what entity was involved, and a kind-specific value.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t sequence;       /* ring-local monotonic sequence */
    asx_nd_event_kind kind;  /* nondeterminism boundary type */
    uint64_t entity_id;      /* handle of affected entity */
    uint64_t observed_value; /* the nondeterministic value observed */
    uint32_t trace_seq;      /* trace event sequence at log time */
} asx_hindsight_event;

/* -------------------------------------------------------------------
 * Flush output buffer
 *
 * Receives the ring contents on flush. Fixed-size buffer avoids
 * dynamic allocation; JSON serialization writes into this.
 * ------------------------------------------------------------------- */

#define ASX_HINDSIGHT_FLUSH_BUFFER_SIZE 8192u

typedef struct {
    char data[ASX_HINDSIGHT_FLUSH_BUFFER_SIZE];
    uint32_t len;
} asx_hindsight_flush_buffer;

/* -------------------------------------------------------------------
 * Hindsight ring API
 * ------------------------------------------------------------------- */

/* Initialize the hindsight ring. Clears all events. */
ASX_API void asx_hindsight_init(void);

/* Reset the ring to empty state (test support). */
ASX_API void asx_hindsight_reset(void);

/* Log a nondeterministic boundary event into the ring.
 * If the ring is full, the oldest entry is overwritten (wrapping). */
ASX_API void asx_hindsight_log(asx_nd_event_kind kind, uint64_t entity_id, uint64_t observed_value);

/* Return the total number of events logged since last reset.
 * May exceed ASX_HINDSIGHT_CAPACITY if the ring has wrapped. */
ASX_API uint32_t asx_hindsight_total_count(void);

/* Return the number of events currently readable in the ring.
 * At most ASX_HINDSIGHT_CAPACITY. */
ASX_API uint32_t asx_hindsight_readable_count(void);

/* Return nonzero if the ring has overwritten older entries. */
ASX_API int asx_hindsight_overflowed(void);

/* Read event at logical index (0 = oldest readable).
 * Returns 1 on success, 0 on out-of-bounds. */
ASX_API int asx_hindsight_get(uint32_t index, asx_hindsight_event *out);

/* -------------------------------------------------------------------
 * Flush API — dump ring contents for diagnostics
 *
 * Serializes all readable events to JSON in the output buffer.
 * Called on invariant failure, replay divergence, or explicit trigger.
 * ------------------------------------------------------------------- */

/* Flush the ring contents as JSON into the output buffer.
 * Returns ASX_OK on success. Does not clear the ring. */
ASX_API asx_status asx_hindsight_flush_json(asx_hindsight_flush_buffer *out);

/* Compute FNV-1a digest over the ring contents (for replay identity). */
ASX_API uint64_t asx_hindsight_digest(void);

/* -------------------------------------------------------------------
 * Convenience: flush on replay divergence
 *
 * If replay verification detects divergence, automatically flush
 * the hindsight ring to provide divergence context.
 * ------------------------------------------------------------------- */

/* Flush hindsight ring if and only if the replay result indicates
 * divergence. Returns ASX_OK if flushed, ASX_E_PENDING if no
 * divergence was detected. */
ASX_API asx_status asx_hindsight_flush_on_divergence(const asx_hindsight_flush_buffer *replay_ctx,
                                                     asx_hindsight_flush_buffer *out);

/* Return human-readable name for a nondeterminism event kind. */
ASX_API const char *asx_nd_event_kind_str(asx_nd_event_kind kind);

/* -------------------------------------------------------------------
 * Flush policy — configurable triggers for automatic ring flush
 *
 * Controls when the ring is automatically flushed. Callers can
 * always trigger a manual flush via asx_hindsight_flush_json().
 * ------------------------------------------------------------------- */

typedef struct {
    int flush_on_invariant;  /* auto-flush when ghost violations detected */
    int flush_on_divergence; /* auto-flush when replay digest mismatches */
} asx_hindsight_policy;

/* Set the active flush policy. */
ASX_API void asx_hindsight_set_policy(const asx_hindsight_policy *policy);

/* Query the active flush policy. */
ASX_API asx_hindsight_policy asx_hindsight_policy_active(void);

/* -------------------------------------------------------------------
 * Invariant-failure flush
 *
 * Flushes the ring if the ghost safety monitors have recorded
 * any violations. Returns ASX_OK if flushed, ASX_E_PENDING if
 * no violations were detected.
 * ------------------------------------------------------------------- */

/* Flush the hindsight ring if ghost safety violations have occurred.
 *
 * Preconditions: out must not be NULL.
 * Returns ASX_OK if flushed, ASX_E_PENDING if no violations detected,
 *   ASX_E_INVALID_ARGUMENT if out is NULL.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API asx_status asx_hindsight_flush_on_invariant(asx_hindsight_flush_buffer *out);

/* Compare the current ring digest against an expected value.
 * Returns 1 if digests differ (divergence detected), 0 otherwise.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API int asx_hindsight_check_divergence(uint64_t expected_digest);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_HINDSIGHT_H */
