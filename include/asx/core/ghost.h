/*
 * asx/core/ghost.h — ghost safety monitors for protocol and linearity invariants
 *
 * Ghost monitors provide lightweight runtime checks that recover part of
 * Rust-level protocol confidence in debug and hardened modes. They are
 * compile-time gated behind ASX_DEBUG_GHOST so production builds incur
 * zero overhead.
 *
 * Two monitor classes:
 *   1. Protocol monitor — validates lifecycle state transitions are legal
 *      for regions, tasks, and obligations.
 *   2. Linearity monitor — ensures each obligation has exactly one
 *      resolution (commit or abort); detects double-use and leaks.
 *
 * Violations are recorded into a deterministic ring buffer that can be
 * queried for diagnostics and integrated into test assertions.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_GHOST_H
#define ASX_CORE_GHOST_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

/* Auto-enable ghost monitors in debug builds unless explicitly disabled.
 * Define ASX_DEBUG_GHOST_DISABLE to suppress this auto-enable. */
#if defined(ASX_DEBUG) && ASX_DEBUG && !defined(ASX_DEBUG_GHOST) &&                                \
    !defined(ASX_DEBUG_GHOST_DISABLE)
#define ASX_DEBUG_GHOST
#endif

/* ------------------------------------------------------------------ */
/* Violation types                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_GHOST_PROTOCOL_REGION = 0,     /* illegal region transition */
    ASX_GHOST_PROTOCOL_TASK = 1,       /* illegal task transition */
    ASX_GHOST_PROTOCOL_OBLIGATION = 2, /* illegal obligation transition */
    ASX_GHOST_LINEARITY_DOUBLE = 3,    /* obligation resolved twice */
    ASX_GHOST_LINEARITY_LEAK = 4,      /* obligation never resolved */
    ASX_GHOST_BORROW_EXCLUSIVE = 5,    /* exclusive borrow while shared active */
    ASX_GHOST_BORROW_SHARED = 6,       /* shared borrow while exclusive active */
    ASX_GHOST_DETERMINISM_DRIFT = 7    /* scheduler event ordering changed */
} asx_ghost_violation_kind;

/* ------------------------------------------------------------------ */
/* Violation record                                                    */
/* ------------------------------------------------------------------ */

#define ASX_GHOST_RING_CAPACITY 64u

typedef struct {
    asx_ghost_violation_kind kind;
    uint64_t entity_id; /* handle of violating entity */
    int from_state;     /* state before attempted transition */
    int to_state;       /* attempted target state */
    uint32_t sequence;  /* monotonic violation counter */
} asx_ghost_violation;

/* ------------------------------------------------------------------ */
/* Monitor API                                                         */
/* ------------------------------------------------------------------ */

#ifdef ASX_DEBUG_GHOST

/* Reset all ghost monitor state. Call before each test. */
ASX_API void asx_ghost_reset(void);

/* --- Protocol monitor --- */

/* Record and validate a region state transition.
 * Returns ASX_OK if legal, records violation and returns
 * ASX_E_INVALID_TRANSITION if not.
 * Note: no ASX_MUST_USE — these are primarily side-effect operations
 * for recording violations. Callers may check or ignore the result. */
ASX_API asx_status asx_ghost_check_region_transition(asx_region_id id, asx_region_state from,
                                                     asx_region_state to);

/* Record and validate a task state transition. */
ASX_API asx_status asx_ghost_check_task_transition(asx_task_id id, asx_task_state from,
                                                   asx_task_state to);

/* Record and validate an obligation state transition. */
ASX_API asx_status asx_ghost_check_obligation_transition(asx_obligation_id id,
                                                         asx_obligation_state from,
                                                         asx_obligation_state to);

/* --- Linearity monitor --- */

/* Track a newly reserved obligation. */
ASX_API void asx_ghost_obligation_reserved(asx_obligation_id id);

/* Track obligation resolution (commit or abort). Records violation on
 * double-resolution. */
ASX_API void asx_ghost_obligation_resolved(asx_obligation_id id);

/* Scan for leaked obligations (reserved but never resolved) in a region.
 * Returns the count of leaked obligations found and records violations.
 * No ASX_MUST_USE — primarily a side-effect operation. */
ASX_API uint32_t asx_ghost_check_obligation_leaks(asx_region_id region);

/* --- Query interface --- */

/* Total violation count since last reset. */
ASX_API ASX_MUST_USE uint32_t asx_ghost_violation_count(void);

/* Retrieve violation at index (0 = oldest). Returns nonzero on success. */
ASX_API ASX_MUST_USE int asx_ghost_violation_get(uint32_t index, asx_ghost_violation *out);

/* Check if the ring buffer has overflowed (older entries lost). */
ASX_API ASX_MUST_USE int asx_ghost_ring_overflowed(void);

/* Return human-readable name for a violation kind. Never returns NULL. */
ASX_API ASX_MUST_USE const char *asx_ghost_violation_kind_str(asx_ghost_violation_kind kind);

/* --- Borrow ledger --- */

/* Maximum number of handles tracked simultaneously. */
#define ASX_GHOST_BORROW_TABLE_CAPACITY 128u

/* Acquire a shared borrow on an entity. Records a BORROW_SHARED violation
 * if an exclusive borrow is already active. Returns the new shared count. */
ASX_API uint32_t asx_ghost_borrow_shared(uint64_t entity_id);

/* Acquire an exclusive borrow on an entity. Records a BORROW_EXCLUSIVE
 * violation if any borrow (shared or exclusive) is already active.
 * Returns 1 on success, 0 on conflict. */
ASX_API int asx_ghost_borrow_exclusive(uint64_t entity_id);

/* Release one borrow on an entity (shared or exclusive). */
ASX_API void asx_ghost_borrow_release(uint64_t entity_id);

/* Release all borrows on an entity (e.g. on destruction). */
ASX_API void asx_ghost_borrow_release_all(uint64_t entity_id);

/* Query the number of active shared borrows on an entity. */
ASX_API ASX_MUST_USE uint32_t asx_ghost_borrow_shared_count(uint64_t entity_id);

/* Query whether an exclusive borrow is active on an entity. */
ASX_API ASX_MUST_USE int asx_ghost_borrow_is_exclusive(uint64_t entity_id);

/* --- Determinism monitor --- */

/* Maximum events tracked per determinism check window. */
#define ASX_GHOST_DETERMINISM_CAPACITY 256u

/* Reset determinism monitor state. */
ASX_API void asx_ghost_determinism_reset(void);

/* Record a scheduler event key for ordering validation. */
ASX_API void asx_ghost_determinism_record(uint64_t event_key);

/* Seal the current event sequence as the reference ordering.
 * Subsequent calls to asx_ghost_determinism_record will be compared
 * against this reference. */
ASX_API void asx_ghost_determinism_seal(void);

/* Check if the current event sequence matches the sealed reference.
 * Returns the number of drift violations detected (0 = stable). */
ASX_API ASX_MUST_USE uint32_t asx_ghost_determinism_check(void);

/* Return a simple hash digest of the current event sequence. */
ASX_API ASX_MUST_USE uint64_t asx_ghost_determinism_digest(void);

/* Return the number of events recorded since last reset. */
ASX_API ASX_MUST_USE uint32_t asx_ghost_determinism_event_count(void);

#else /* !ASX_DEBUG_GHOST */

/* Zero-overhead stubs when ghost monitors are disabled. */
#define asx_ghost_reset() ((void)0)
#define asx_ghost_check_region_transition(id, f, t) (ASX_OK)
#define asx_ghost_check_task_transition(id, f, t) (ASX_OK)
#define asx_ghost_check_obligation_transition(id, f, t) (ASX_OK)
#define asx_ghost_obligation_reserved(id) ((void)0)
#define asx_ghost_obligation_resolved(id) ((void)0)
#define asx_ghost_check_obligation_leaks(r) ((uint32_t)0)
#define asx_ghost_violation_count() ((uint32_t)0)
#define asx_ghost_violation_get(i, o) ((int)0)
#define asx_ghost_ring_overflowed() ((int)0)
#define asx_ghost_violation_kind_str(k) ("ghost_disabled")
#define asx_ghost_borrow_shared(id) ((void)(id), (uint32_t)0)
#define asx_ghost_borrow_exclusive(id) ((void)(id), (int)1)
#define asx_ghost_borrow_release(id) ((void)(id))
#define asx_ghost_borrow_release_all(id) ((void)(id))
#define asx_ghost_borrow_shared_count(id) ((void)(id), (uint32_t)0)
#define asx_ghost_borrow_is_exclusive(id) ((void)(id), (int)0)
#define asx_ghost_determinism_reset() ((void)0)
#define asx_ghost_determinism_record(k) ((void)(k))
#define asx_ghost_determinism_seal() ((void)0)
#define asx_ghost_determinism_check() ((uint32_t)0)
#define asx_ghost_determinism_digest() ((uint64_t)0)
#define asx_ghost_determinism_event_count() ((uint32_t)0)

#endif /* ASX_DEBUG_GHOST */

#endif /* ASX_CORE_GHOST_H */
