/*
 * asx_ids.h — generation-tagged handle types and packing helpers
 *
 * All externally visible runtime entities use opaque generation-safe handles.
 * Handle format: [16-bit type_tag | 16-bit state_mask | 32-bit arena_index]
 *
 * Handle validation requires:
 *   - type tag match
 *   - slot index bounds
 *   - generation match (stored in arena slot, not in handle)
 *   - liveness/state legality
 *   - runtime tag match (the issuing runtime, see below)
 *
 * The 16-bit mask field is split:
 *   bits 0-5   lifecycle state when the handle was issued, one bit per state
 *              (1 << state); O(1) admission gating: state_mask & expected_mask
 *   bits 6-9   runtime tag: which live runtime issued the handle (K18). The
 *              first runtime is tag 0, so its handles are byte-identical to
 *              handles issued before the tag existed
 *   bits 10-15 reserved, zero
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_IDS_H
#define ASX_IDS_H

#include <asx/asx_export.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Opaque handle types                                                */
/* ------------------------------------------------------------------ */

/*
 * Internal structure: [type_tag:16 | state_mask:16 | arena_index:32].
 * Users must not inspect or construct these directly.
 * Use asx_handle_* helpers for all operations.
 */
typedef uint64_t asx_region_id;
typedef uint64_t asx_task_id;
typedef uint64_t asx_obligation_id;
typedef uint64_t asx_timer_id;
typedef uint64_t asx_channel_id;

/* Sentinel value for invalid/uninitialized handles */
#define ASX_INVALID_ID ((uint64_t)0)

/* ------------------------------------------------------------------ */
/* Type tags (high 16 bits of handle)                                 */
/* Per Appendix B of LIFECYCLE_TRANSITION_TABLES.md                   */
/* ------------------------------------------------------------------ */

#define ASX_TYPE_REGION ((uint16_t)0x0001)
#define ASX_TYPE_TASK ((uint16_t)0x0002)
#define ASX_TYPE_OBLIGATION ((uint16_t)0x0003)
#define ASX_TYPE_CANCEL_WITNESS ((uint16_t)0x0004)
#define ASX_TYPE_TIMER ((uint16_t)0x0005)
#define ASX_TYPE_CHANNEL ((uint16_t)0x0006)

/* ------------------------------------------------------------------ */
/* Handle packing/unpacking helpers                                   */
/* ------------------------------------------------------------------ */

static inline uint64_t asx_handle_pack(uint16_t type_tag, uint16_t state_mask, uint32_t index) {
    return ((uint64_t)type_tag << 48) | ((uint64_t)state_mask << 32) | (uint64_t)index;
}

static inline uint16_t asx_handle_type_tag(uint64_t h) { return (uint16_t)(h >> 48); }

static inline uint16_t asx_handle_state_mask(uint64_t h) { return (uint16_t)((h >> 32) & 0xFFFF); }

static inline uint32_t asx_handle_index(uint64_t h) { return (uint32_t)(h & 0xFFFFFFFF); }

static inline int asx_handle_is_valid(uint64_t h) { return h != ASX_INVALID_ID; }

/* The mask field's parts (see the header comment). */
#define ASX_HANDLE_STATE_BITS 6u
#define ASX_HANDLE_STATE_BITS_MASK ((uint16_t)0x003F)
#define ASX_HANDLE_RT_TAG_SHIFT 6u
#define ASX_HANDLE_RT_TAG_BITS 4u
#define ASX_HANDLE_RT_TAG_MASK ((uint16_t)0x03C0)
/* Runtime tags 0 .. ASX_HANDLE_RT_TAG_LIMIT - 1 name a runtime: at most 15
 * live runtimes; the highest tag value is reserved. */
#define ASX_HANDLE_RT_TAG_LIMIT 15u

/* O(1) state admission check: returns nonzero if current state is allowed.
 * Only the state bits take part; the runtime tag never admits. */
static inline int asx_handle_state_allowed(uint64_t h, uint16_t allowed_mask) {
    return (asx_handle_state_mask(h) & allowed_mask & ASX_HANDLE_STATE_BITS_MASK) != 0;
}

/* The runtime tag of a handle: 0 for the first runtime. */
static inline uint16_t asx_handle_runtime_tag(uint64_t h) {
    return (uint16_t)((asx_handle_state_mask(h) & ASX_HANDLE_RT_TAG_MASK) >>
                      ASX_HANDLE_RT_TAG_SHIFT);
}

/* h with its runtime tag set to `tag` (only the tag bits change). */
static inline uint64_t asx_handle_with_runtime_tag(uint64_t h, uint16_t tag) {
    uint64_t tag_field = (uint64_t)ASX_HANDLE_RT_TAG_MASK << 32;
    uint64_t value = ((uint64_t)tag << ASX_HANDLE_RT_TAG_SHIFT) << 32;

    return (h & ~tag_field) | (value & tag_field);
}

/*
 * Generation-safe handle decomposition.
 *
 * The 32-bit index field is a composite: [generation:16 | slot_index:16].
 * This enables stale-handle detection: when a slot is recycled, its
 * generation counter increments. Old handles carry the old generation
 * and will fail validation against the new slot generation.
 */
static inline uint16_t asx_handle_slot(uint64_t h) { return (uint16_t)(h & 0xFFFF); }

static inline uint16_t asx_handle_generation(uint64_t h) { return (uint16_t)((h >> 16) & 0xFFFF); }

/* Pack a composite index from generation + slot index */
static inline uint32_t asx_handle_pack_index(uint16_t generation, uint16_t slot_index) {
    return ((uint32_t)generation << 16) | (uint32_t)slot_index;
}

/* ------------------------------------------------------------------ */
/* Time representation                                                */
/* ------------------------------------------------------------------ */

typedef uint64_t asx_time;

/* ------------------------------------------------------------------ */
/* Region lifecycle states                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_REGION_OPEN = 0,
    ASX_REGION_CLOSING = 1,
    ASX_REGION_DRAINING = 2,
    ASX_REGION_FINALIZING = 3,
    ASX_REGION_CLOSED = 4
} asx_region_state;

/* ------------------------------------------------------------------ */
/* Task lifecycle states                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_TASK_CREATED = 0,
    ASX_TASK_RUNNING = 1,
    ASX_TASK_CANCEL_REQUESTED = 2,
    ASX_TASK_CANCELLING = 3,
    ASX_TASK_FINALIZING = 4,
    ASX_TASK_COMPLETED = 5
} asx_task_state;

/* ------------------------------------------------------------------ */
/* Obligation lifecycle states                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_OBLIGATION_RESERVED = 0,
    ASX_OBLIGATION_COMMITTED = 1,
    ASX_OBLIGATION_ABORTED = 2,
    ASX_OBLIGATION_LEAKED = 3
} asx_obligation_state;

/*
 * Compile-time layout checks: every lifecycle state a handle's mask encodes
 * (1 << state) lies in the state bits, below the runtime tag, and the tag
 * bits neither overlap them nor run out of values. A state added past bit 5
 * fails the build here rather than corrupting a runtime tag.
 */
#define ASX_HANDLE_LAYOUT_ASSERT(name, expr) typedef char asx_handle_assert_##name[(expr) ? 1 : -1]

ASX_HANDLE_LAYOUT_ASSERT(region_states_fit, (unsigned)ASX_REGION_CLOSED < ASX_HANDLE_STATE_BITS);
ASX_HANDLE_LAYOUT_ASSERT(task_states_fit, (unsigned)ASX_TASK_COMPLETED < ASX_HANDLE_STATE_BITS);
ASX_HANDLE_LAYOUT_ASSERT(obligation_states_fit,
                         (unsigned)ASX_OBLIGATION_LEAKED < ASX_HANDLE_STATE_BITS);
ASX_HANDLE_LAYOUT_ASSERT(state_and_tag_bits_disjoint,
                         (ASX_HANDLE_STATE_BITS_MASK & ASX_HANDLE_RT_TAG_MASK) == 0u &&
                             ASX_HANDLE_STATE_BITS == ASX_HANDLE_RT_TAG_SHIFT &&
                             ASX_HANDLE_RT_TAG_LIMIT < (1u << ASX_HANDLE_RT_TAG_BITS));

/* ------------------------------------------------------------------ */
/* Outcome severity lattice                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_OUTCOME_OK = 0,
    ASX_OUTCOME_ERR = 1,
    ASX_OUTCOME_CANCELLED = 2,
    ASX_OUTCOME_PANICKED = 3
} asx_outcome_severity;

/* ------------------------------------------------------------------ */
/* Cancellation kinds (11 variants, Rust's CancelKind order)          */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CANCEL_USER = 0,        /* severity 0 */
    ASX_CANCEL_TIMEOUT = 1,     /* severity 1 */
    ASX_CANCEL_DEADLINE = 2,    /* severity 1 */
    ASX_CANCEL_POLL_QUOTA = 3,  /* severity 2 */
    ASX_CANCEL_COST_BUDGET = 4, /* severity 2 */
    ASX_CANCEL_FAIL_FAST = 5,   /* severity 3 */
    ASX_CANCEL_RACE_LOST = 6,   /* severity 3 */
    ASX_CANCEL_PARENT = 7,      /* severity 4 */
    ASX_CANCEL_RESOURCE = 8,    /* severity 4 */
    ASX_CANCEL_SHUTDOWN = 9,    /* severity 5 */
    ASX_CANCEL_LINKED_EXIT = 10 /* severity 3, declared last as in Rust */
} asx_cancel_kind;

/* Cancellation protocol phases */
typedef enum {
    ASX_CANCEL_PHASE_REQUESTED = 0,
    ASX_CANCEL_PHASE_CANCELLING = 1,
    ASX_CANCEL_PHASE_FINALIZING = 2,
    ASX_CANCEL_PHASE_COMPLETED = 3
} asx_cancel_phase;

/* Return the severity level for a cancel kind (0-5) */
static inline int asx_cancel_severity(asx_cancel_kind kind) {
    static const int severity[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 3};
    return (unsigned int)kind <= 10u ? severity[(unsigned int)kind] : 5;
}

/* ------------------------------------------------------------------ */
/* Cancel witness handle                                               */
/* ------------------------------------------------------------------ */

typedef uint64_t asx_cancel_witness_id;

/* ------------------------------------------------------------------ */
/* Handle type classification helpers                                  */
/* ------------------------------------------------------------------ */

static inline int asx_handle_is_region(uint64_t h) {
    return asx_handle_type_tag(h) == ASX_TYPE_REGION;
}

static inline int asx_handle_is_task(uint64_t h) { return asx_handle_type_tag(h) == ASX_TYPE_TASK; }

static inline int asx_handle_is_obligation(uint64_t h) {
    return asx_handle_type_tag(h) == ASX_TYPE_OBLIGATION;
}

static inline int asx_handle_is_cancel_witness(uint64_t h) {
    return asx_handle_type_tag(h) == ASX_TYPE_CANCEL_WITNESS;
}

static inline int asx_handle_is_timer(uint64_t h) {
    return asx_handle_type_tag(h) == ASX_TYPE_TIMER;
}

static inline int asx_handle_is_channel(uint64_t h) {
    return asx_handle_type_tag(h) == ASX_TYPE_CHANNEL;
}

/* Return the human-readable name for a handle type tag. */
static inline const char *asx_handle_type_str(uint16_t tag) {
    switch (tag) {
    case ASX_TYPE_REGION: return "region";
    case ASX_TYPE_TASK: return "task";
    case ASX_TYPE_OBLIGATION: return "obligation";
    case ASX_TYPE_CANCEL_WITNESS: return "cancel_witness";
    case ASX_TYPE_TIMER: return "timer";
    case ASX_TYPE_CHANNEL: return "channel";
    default: return "unknown";
    }
}

#endif /* ASX_IDS_H */
