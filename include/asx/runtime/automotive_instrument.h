/*
 * asx/runtime/automotive_instrument.h — automotive profile instrumentation (bd-j4m.4)
 *
 * Provides deadline tracking, watchdog checkpoint monitoring,
 * degraded-mode transition logging, and compliance gate evaluation
 * for the automotive profile. All instrumentation is operational
 * (affects observability, not semantics).
 *
 * Key invariants:
 *   - Deadline decisions are pure functions of (budget, now)
 *   - Degraded-mode transitions are recorded in a bounded audit ring
 *   - Compliance gate evaluates deterministic pass/fail
 *   - Audit entries have monotonic sequence numbers
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_AUTOMOTIVE_INSTRUMENT_H
#define ASX_RUNTIME_AUTOMOTIVE_INSTRUMENT_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Deadline tracker — monitors deadline compliance
 *
 * Tracks deadline hits/misses, worst-case margin, and running
 * statistics for compliance reporting. Margin is defined as
 * (deadline - completion_time) in nanoseconds; negative = miss.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t total_deadlines;   /* total deadlines evaluated */
    uint32_t deadline_hits;     /* completed before deadline */
    uint32_t deadline_misses;   /* completed after deadline */
    int64_t  worst_margin_ns;   /* most negative margin (worst miss) */
    int64_t  best_margin_ns;    /* most positive margin (best hit) */
    uint64_t total_margin_ns;   /* sum of absolute margins for mean */
    uint32_t total_margin_count;/* count for mean margin computation */
} asx_auto_deadline_tracker;

/* Initialize a deadline tracker. */
ASX_API void asx_auto_deadline_init(asx_auto_deadline_tracker *dt);

/* Record a deadline evaluation result.
 * deadline_ns: the deadline timestamp.
 * actual_ns: when the task actually completed.
 * If actual_ns <= deadline_ns, it's a hit; otherwise a miss. */
ASX_API void asx_auto_deadline_record(asx_auto_deadline_tracker *dt,
                                       uint64_t deadline_ns,
                                       uint64_t actual_ns);

/* Get deadline miss rate as percentage * 100 (e.g., 250 = 2.5%).
 * Returns 0 if no deadlines recorded. */
ASX_API uint32_t asx_auto_deadline_miss_rate(
    const asx_auto_deadline_tracker *dt);

/* Reset the deadline tracker. */
ASX_API void asx_auto_deadline_reset(asx_auto_deadline_tracker *dt);

/* -------------------------------------------------------------------
 * Watchdog monitor — checkpoint interval tracking
 *
 * Tracks the interval between successive checkpoint calls for a
 * monitored entity. If the interval exceeds the configured
 * watchdog_period_ns, a watchdog violation is recorded.
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t watchdog_period_ns;     /* max allowed interval between checkpoints */
    uint64_t last_checkpoint_ns;     /* timestamp of last checkpoint */
    uint32_t total_checkpoints;      /* total checkpoints recorded */
    uint32_t violations;             /* intervals exceeding watchdog_period */
    uint64_t worst_interval_ns;      /* longest observed interval */
    int      armed;                  /* 1 if last_checkpoint_ns is valid */
} asx_auto_watchdog;

/* Initialize a watchdog monitor with the given period. */
ASX_API void asx_auto_watchdog_init(asx_auto_watchdog *wd,
                                     uint64_t period_ns);

/* Record a checkpoint at the given timestamp. */
ASX_API void asx_auto_watchdog_checkpoint(asx_auto_watchdog *wd,
                                           uint64_t now_ns);

/* Check if the watchdog would trigger at the given time
 * (without recording a checkpoint). Returns 1 if violation. */
ASX_API int asx_auto_watchdog_would_trigger(const asx_auto_watchdog *wd,
                                             uint64_t now_ns);

/* Reset the watchdog monitor. Preserves the period. */
ASX_API void asx_auto_watchdog_reset(asx_auto_watchdog *wd);

/* -------------------------------------------------------------------
 * Degraded-mode audit ring — bounded event log
 *
 * Records safety-critical events (region poison, forced cancellation,
 * deadline misses, watchdog violations) in a bounded ring buffer.
 * Entries have monotonic sequence numbers for ordering.
 * ------------------------------------------------------------------- */

#define ASX_AUTO_AUDIT_RING_SIZE  64u

typedef enum {
    ASX_AUDIT_REGION_POISONED     = 0,
    ASX_AUDIT_CANCEL_FORCED       = 1,
    ASX_AUDIT_DEADLINE_MISS       = 2,
    ASX_AUDIT_WATCHDOG_VIOLATION  = 3,
    ASX_AUDIT_DEGRADED_ENTER      = 4,
    ASX_AUDIT_DEGRADED_EXIT       = 5,
    ASX_AUDIT_CHECKPOINT_OK       = 6,
    ASX_AUDIT_KIND_COUNT          = 7
} asx_audit_kind;

typedef struct {
    uint32_t       seq;          /* monotonic sequence number */
    asx_audit_kind kind;         /* event type */
    uint64_t       timestamp_ns; /* when the event occurred */
    uint64_t       entity_id;    /* region/task ID involved */
    int64_t        detail;       /* kind-specific detail (margin, interval, etc.) */
} asx_audit_entry;

typedef struct {
    asx_audit_entry entries[ASX_AUTO_AUDIT_RING_SIZE];
    uint32_t        head;        /* next write position */
    uint32_t        count;       /* total entries written (may exceed ring size) */
    uint32_t        next_seq;    /* next sequence number to assign */
} asx_auto_audit_ring;

/* Initialize the audit ring. */
ASX_API void asx_auto_audit_init(asx_auto_audit_ring *ring);

/* Record an audit event. Overwrites oldest entry when full. */
ASX_API void asx_auto_audit_record(asx_auto_audit_ring *ring,
                                    asx_audit_kind kind,
                                    uint64_t timestamp_ns,
                                    uint64_t entity_id,
                                    int64_t detail);

/* Get an audit entry by ring index (0 = oldest available).
 * Returns NULL if index out of range. */
ASX_API const asx_audit_entry *asx_auto_audit_get(
    const asx_auto_audit_ring *ring, uint32_t index);

/* Return the number of entries currently in the ring
 * (capped at ASX_AUTO_AUDIT_RING_SIZE). */
ASX_API uint32_t asx_auto_audit_count(const asx_auto_audit_ring *ring);

/* Return the total number of events ever recorded (may exceed ring size). */
ASX_API uint32_t asx_auto_audit_total(const asx_auto_audit_ring *ring);

/* Return human-readable name for an audit kind. */
ASX_API const char *asx_audit_kind_str(asx_audit_kind kind);

/* Reset the audit ring. */
ASX_API void asx_auto_audit_reset(asx_auto_audit_ring *ring);

/* -------------------------------------------------------------------
 * Compliance gate — deadline and watchdog threshold evaluation
 *
 * Evaluates pass/fail against configurable thresholds for
 * automotive safety compliance reporting.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t max_miss_rate_pct100;   /* max deadline miss rate (pct*100, e.g., 100 = 1.0%) */
    uint32_t max_watchdog_violations;/* max allowed watchdog violations */
    uint32_t min_checkpoints;        /* minimum checkpoints expected */
} asx_auto_compliance_gate;

typedef struct {
    int      pass;                   /* 1 if all thresholds met */
    uint32_t actual_miss_rate;       /* actual deadline miss rate (pct*100) */
    uint32_t actual_violations;      /* actual watchdog violations */
    uint32_t actual_checkpoints;     /* actual checkpoint count */
    uint32_t violation_mask;         /* bitmask of failed checks */
} asx_auto_compliance_result;

/* Violation bitmask bits */
#define ASX_COMPLIANCE_DEADLINE_RATE  (1u << 0)
#define ASX_COMPLIANCE_WATCHDOG       (1u << 1)
#define ASX_COMPLIANCE_CHECKPOINT_MIN (1u << 2)

/* Initialize a compliance gate with default automotive thresholds. */
ASX_API void asx_auto_compliance_gate_init(asx_auto_compliance_gate *gate);

/* Evaluate compliance against deadline tracker and watchdog state. */
ASX_API void asx_auto_compliance_evaluate(
    const asx_auto_compliance_gate *gate,
    const asx_auto_deadline_tracker *dt,
    const asx_auto_watchdog *wd,
    asx_auto_compliance_result *result);

/* -------------------------------------------------------------------
 * Global automotive instrumentation state
 *
 * Singleton for per-process automotive metrics. Reset via
 * asx_auto_instrument_reset().
 * ------------------------------------------------------------------- */

/* Reset all global automotive instrumentation state. */
ASX_API void asx_auto_instrument_reset(void);

/* Get pointer to the global deadline tracker. */
ASX_API asx_auto_deadline_tracker *asx_auto_deadline_global(void);

/* Get pointer to the global watchdog monitor. */
ASX_API asx_auto_watchdog *asx_auto_watchdog_global(void);

/* Get pointer to the global audit ring. */
ASX_API asx_auto_audit_ring *asx_auto_audit_global(void);

/* Convenience: record a deadline result in the global tracker
 * and auto-log misses to the audit ring. */
ASX_API void asx_auto_record_deadline(uint64_t deadline_ns,
                                       uint64_t actual_ns,
                                       uint64_t entity_id);

/* Convenience: record a checkpoint in the global watchdog
 * and auto-log violations to the audit ring. */
ASX_API void asx_auto_record_checkpoint(uint64_t now_ns,
                                         uint64_t entity_id);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_AUTOMOTIVE_INSTRUMENT_H */
