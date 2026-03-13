/*
 * ghost.c — ghost safety monitors for protocol and linearity invariants
 *
 * Compile-time gated: entire file is a no-op unless ASX_DEBUG_GHOST is
 * defined. When active, provides deterministic violation recording with
 * zero heap allocation (fixed-size ring buffer and tracking table).
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/ghost.h>
#include <asx/core/transition.h>

#ifdef ASX_DEBUG_GHOST

#include <string.h>

/* -------------------------------------------------------------------
 * Violation ring buffer
 * ------------------------------------------------------------------- */

static asx_ghost_violation g_ghost_ring[ASX_GHOST_RING_CAPACITY];
static uint32_t g_ghost_ring_write; /* next write position */
static uint32_t g_ghost_ring_count; /* total violations recorded */
static int g_ghost_ring_overflow;   /* set once ring wraps */

/* -------------------------------------------------------------------
 * Linearity tracking table
 *
 * Tracks which obligation IDs have been reserved and/or resolved.
 * Uses a small fixed table keyed by slot index. Each slot stores:
 *   - obligation handle (for identity verification)
 *   - reserved flag
 *   - resolved flag
 * ------------------------------------------------------------------- */

#define ASX_GHOST_LINEARITY_CAPACITY 256u

typedef struct {
    asx_obligation_id id;
    int reserved;
    int resolved;
} asx_ghost_linearity_entry;

static asx_ghost_linearity_entry g_ghost_linearity[ASX_GHOST_LINEARITY_CAPACITY];
static uint32_t g_ghost_linearity_count;

/* -------------------------------------------------------------------
 * Borrow ledger state (forward declarations; full API below)
 * ------------------------------------------------------------------- */

typedef struct {
    uint64_t entity_id;
    uint32_t shared_count;
    int exclusive;
    int occupied;
} asx_ghost_borrow_entry;

static asx_ghost_borrow_entry g_ghost_borrows[ASX_GHOST_BORROW_TABLE_CAPACITY];
static uint32_t g_ghost_borrow_count;

/* -------------------------------------------------------------------
 * Determinism monitor state (forward declarations; full API below)
 * ------------------------------------------------------------------- */

static uint64_t g_ghost_det_events[ASX_GHOST_DETERMINISM_CAPACITY];
static uint32_t g_ghost_det_count;

static uint64_t g_ghost_det_reference[ASX_GHOST_DETERMINISM_CAPACITY];
static uint32_t g_ghost_det_ref_count;
static int g_ghost_det_sealed;

/* Forward declaration for use in asx_ghost_reset */
static void ghost_determinism_reset_impl(void);

/* -------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------- */

static void ghost_record_violation(asx_ghost_violation_kind kind, uint64_t entity_id,
                                   int from_state, int to_state) {
    asx_ghost_violation *v = &g_ghost_ring[g_ghost_ring_write % ASX_GHOST_RING_CAPACITY];
    v->kind = kind;
    v->entity_id = entity_id;
    v->from_state = from_state;
    v->to_state = to_state;
    v->sequence = g_ghost_ring_count;

    g_ghost_ring_write = (g_ghost_ring_write + 1u) % ASX_GHOST_RING_CAPACITY;
    /* Saturate count at UINT32_MAX to prevent wrap-around that would
     * cause asx_ghost_violation_count() to return 0 after overflow */
    if (g_ghost_ring_count < UINT32_MAX) { g_ghost_ring_count++; }
    if (g_ghost_ring_count > ASX_GHOST_RING_CAPACITY) { g_ghost_ring_overflow = 1; }
}

/* Find linearity entry by obligation ID. Returns NULL if not tracked. */
static asx_ghost_linearity_entry *ghost_linearity_find(asx_obligation_id id) {
    uint32_t i;
    for (i = 0; i < g_ghost_linearity_count; i++) {
        if (g_ghost_linearity[i].id == id) { return &g_ghost_linearity[i]; }
    }
    return NULL;
}

/* -------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

void asx_ghost_reset(void) {
    memset(g_ghost_ring, 0, sizeof(g_ghost_ring));
    g_ghost_ring_write = 0;
    g_ghost_ring_count = 0;
    g_ghost_ring_overflow = 0;

    memset(g_ghost_linearity, 0, sizeof(g_ghost_linearity));
    g_ghost_linearity_count = 0;

    memset(g_ghost_borrows, 0, sizeof(g_ghost_borrows));
    g_ghost_borrow_count = 0;

    ghost_determinism_reset_impl();
}

/* --- Protocol monitor --- */

asx_status asx_ghost_check_region_transition(asx_region_id id, asx_region_state from,
                                             asx_region_state to) {
    asx_status st = asx_region_transition_check(from, to);
    if (st != ASX_OK) { ghost_record_violation(ASX_GHOST_PROTOCOL_REGION, id, (int)from, (int)to); }
    return st;
}

asx_status asx_ghost_check_task_transition(asx_task_id id, asx_task_state from, asx_task_state to) {
    asx_status st = asx_task_transition_check(from, to);
    if (st != ASX_OK) { ghost_record_violation(ASX_GHOST_PROTOCOL_TASK, id, (int)from, (int)to); }
    return st;
}

asx_status asx_ghost_check_obligation_transition(asx_obligation_id id, asx_obligation_state from,
                                                 asx_obligation_state to) {
    asx_status st = asx_obligation_transition_check(from, to);
    if (st != ASX_OK) {
        ghost_record_violation(ASX_GHOST_PROTOCOL_OBLIGATION, id, (int)from, (int)to);
    }
    return st;
}

/* --- Linearity monitor --- */

void asx_ghost_obligation_reserved(asx_obligation_id id) {
    asx_ghost_linearity_entry *entry;

    if (g_ghost_linearity_count >= ASX_GHOST_LINEARITY_CAPACITY) {
        return; /* table full — silent cap */
    }

    entry = &g_ghost_linearity[g_ghost_linearity_count++];
    entry->id = id;
    entry->reserved = 1;
    entry->resolved = 0;
}

void asx_ghost_obligation_resolved(asx_obligation_id id) {
    asx_ghost_linearity_entry *entry = ghost_linearity_find(id);
    if (entry == NULL) { return; /* not tracked (table was full or ID never reserved) */ }

    if (entry->resolved) {
        /* Double resolution — linearity violation */
        ghost_record_violation(ASX_GHOST_LINEARITY_DOUBLE, id, -1, -1);
        return;
    }

    entry->resolved = 1;
}

uint32_t asx_ghost_check_obligation_leaks(asx_region_id region) {
    uint32_t i;
    uint32_t leak_count = 0;

    for (i = 0; i < g_ghost_linearity_count; i++) {
        asx_ghost_linearity_entry *entry = &g_ghost_linearity[i];
        if (!entry->reserved || entry->resolved) { continue; }

        /*
         * Check if this obligation belongs to the given region.
         * The obligation handle encodes the slot index; we use
         * a lightweight check via handle inspection only.
         * For the walking skeleton, obligations store their region
         * in the arena slot, but we don't have access to that here
         * without coupling to runtime_internal.h. Instead, we
         * report ALL unresolved obligations when region is
         * ASX_INVALID_ID, or filter by region handle prefix
         * comparison if the caller provides a valid region.
         */
        (void)region; /* walking skeleton: report all unresolved */

        ghost_record_violation(ASX_GHOST_LINEARITY_LEAK, entry->id, -1, -1);
        leak_count++;
    }

    return leak_count;
}

/* --- Query interface --- */

uint32_t asx_ghost_violation_count(void) { return g_ghost_ring_count; }

int asx_ghost_violation_get(uint32_t index, asx_ghost_violation *out) {
    uint32_t avail;
    uint32_t ring_idx;

    if (out == NULL) return 0;

    avail =
        g_ghost_ring_count < ASX_GHOST_RING_CAPACITY ? g_ghost_ring_count : ASX_GHOST_RING_CAPACITY;

    if (index >= avail) return 0;

    if (g_ghost_ring_overflow) {
        /* Ring wrapped: oldest entry is at write position */
        ring_idx = (g_ghost_ring_write + index) % ASX_GHOST_RING_CAPACITY;
    } else {
        ring_idx = index;
    }

    *out = g_ghost_ring[ring_idx];
    return 1;
}

int asx_ghost_ring_overflowed(void) { return g_ghost_ring_overflow; }

const char *asx_ghost_violation_kind_str(asx_ghost_violation_kind kind) {
    switch (kind) {
    case ASX_GHOST_PROTOCOL_REGION: return "protocol_region";
    case ASX_GHOST_PROTOCOL_TASK: return "protocol_task";
    case ASX_GHOST_PROTOCOL_OBLIGATION: return "protocol_obligation";
    case ASX_GHOST_LINEARITY_DOUBLE: return "linearity_double";
    case ASX_GHOST_LINEARITY_LEAK: return "linearity_leak";
    case ASX_GHOST_BORROW_EXCLUSIVE: return "borrow_exclusive";
    case ASX_GHOST_BORROW_SHARED: return "borrow_shared";
    case ASX_GHOST_DETERMINISM_DRIFT: return "determinism_drift";
    default: return "unknown";
    }
}

/* -------------------------------------------------------------------
 * Borrow ledger
 *
 * Tracks shared/exclusive borrow epochs per entity handle.
 * Emulates Rust's runtime borrow checker: multiple shared (&) OR
 * one exclusive (&mut), never both simultaneously.
 * ------------------------------------------------------------------- */

/* asx_ghost_borrow_entry and g_ghost_borrows/g_ghost_borrow_count
 * are forward-declared above asx_ghost_reset for initialization. */

static asx_ghost_borrow_entry *ghost_borrow_find(uint64_t entity_id) {
    uint32_t i;
    for (i = 0; i < ASX_GHOST_BORROW_TABLE_CAPACITY; i++) {
        if (g_ghost_borrows[i].occupied && g_ghost_borrows[i].entity_id == entity_id) {
            return &g_ghost_borrows[i];
        }
    }
    return NULL;
}

static asx_ghost_borrow_entry *ghost_borrow_alloc(uint64_t entity_id) {
    uint32_t i;
    for (i = 0; i < ASX_GHOST_BORROW_TABLE_CAPACITY; i++) {
        if (!g_ghost_borrows[i].occupied) {
            g_ghost_borrows[i].entity_id = entity_id;
            g_ghost_borrows[i].shared_count = 0;
            g_ghost_borrows[i].exclusive = 0;
            g_ghost_borrows[i].occupied = 1;
            g_ghost_borrow_count++;
            return &g_ghost_borrows[i];
        }
    }
    return NULL; /* table full */
}

uint32_t asx_ghost_borrow_shared(uint64_t entity_id) {
    asx_ghost_borrow_entry *entry = ghost_borrow_find(entity_id);

    if (entry == NULL) {
        entry = ghost_borrow_alloc(entity_id);
        if (entry == NULL) { return 0; /* table full */ }
    }

    if (entry->exclusive) {
        /* Conflict: shared borrow while exclusive is active */
        ghost_record_violation(ASX_GHOST_BORROW_SHARED, entity_id, 1, 0);
        return entry->shared_count;
    }

    entry->shared_count++;
    return entry->shared_count;
}

int asx_ghost_borrow_exclusive(uint64_t entity_id) {
    asx_ghost_borrow_entry *entry = ghost_borrow_find(entity_id);

    if (entry == NULL) {
        entry = ghost_borrow_alloc(entity_id);
        if (entry == NULL) { return 0; /* table full */ }
    }

    if (entry->exclusive || entry->shared_count > 0) {
        /* Conflict: exclusive while any borrow is active */
        ghost_record_violation(ASX_GHOST_BORROW_EXCLUSIVE, entity_id, entry->exclusive ? 1 : 0,
                               (int)entry->shared_count);
        return 0;
    }

    entry->exclusive = 1;
    return 1;
}

void asx_ghost_borrow_release(uint64_t entity_id) {
    asx_ghost_borrow_entry *entry = ghost_borrow_find(entity_id);
    if (entry == NULL) { return; }

    if (entry->exclusive) {
        entry->exclusive = 0;
    } else if (entry->shared_count > 0) {
        entry->shared_count--;
    }

    /* Reclaim slot if no borrows remain */
    if (!entry->exclusive && entry->shared_count == 0) {
        entry->occupied = 0;
        if (g_ghost_borrow_count > 0) { g_ghost_borrow_count--; }
    }
}

void asx_ghost_borrow_release_all(uint64_t entity_id) {
    asx_ghost_borrow_entry *entry = ghost_borrow_find(entity_id);
    if (entry == NULL) { return; }
    entry->shared_count = 0;
    entry->exclusive = 0;
    entry->occupied = 0;
    if (g_ghost_borrow_count > 0) { g_ghost_borrow_count--; }
}

uint32_t asx_ghost_borrow_shared_count(uint64_t entity_id) {
    asx_ghost_borrow_entry *entry = ghost_borrow_find(entity_id);
    if (entry == NULL) { return 0; }
    return entry->shared_count;
}

int asx_ghost_borrow_is_exclusive(uint64_t entity_id) {
    asx_ghost_borrow_entry *entry = ghost_borrow_find(entity_id);
    if (entry == NULL) { return 0; }
    return entry->exclusive;
}

/* -------------------------------------------------------------------
 * Determinism monitor
 *
 * Records scheduler event keys in sequence and compares against a
 * sealed reference ordering. Any divergence records a DETERMINISM_DRIFT
 * violation. The digest function provides a simple hash for quick
 * identity comparison across runs.
 *
 * g_ghost_det_* variables are forward-declared above asx_ghost_reset.
 * ------------------------------------------------------------------- */

static void ghost_determinism_reset_impl(void) {
    memset(g_ghost_det_events, 0, sizeof(g_ghost_det_events));
    g_ghost_det_count = 0;
    memset(g_ghost_det_reference, 0, sizeof(g_ghost_det_reference));
    g_ghost_det_ref_count = 0;
    g_ghost_det_sealed = 0;
}

void asx_ghost_determinism_reset(void) { ghost_determinism_reset_impl(); }

void asx_ghost_determinism_record(uint64_t event_key) {
    if (g_ghost_det_count < ASX_GHOST_DETERMINISM_CAPACITY) {
        g_ghost_det_events[g_ghost_det_count] = event_key;
    }
    g_ghost_det_count++;
}

void asx_ghost_determinism_seal(void) {
    uint32_t copy_count = g_ghost_det_count < ASX_GHOST_DETERMINISM_CAPACITY
                              ? g_ghost_det_count
                              : ASX_GHOST_DETERMINISM_CAPACITY;
    memcpy(g_ghost_det_reference, g_ghost_det_events, copy_count * sizeof(uint64_t));
    g_ghost_det_ref_count = g_ghost_det_count;
    g_ghost_det_sealed = 1;

    /* Reset current sequence for the next run */
    memset(g_ghost_det_events, 0, sizeof(g_ghost_det_events));
    g_ghost_det_count = 0;
}

uint32_t asx_ghost_determinism_check(void) {
    uint32_t drift_count = 0;
    uint32_t check_count;
    uint32_t i;

    if (!g_ghost_det_sealed) { return 0; /* no reference to compare against */ }

    /* Length mismatch is a drift */
    if (g_ghost_det_count != g_ghost_det_ref_count) {
        ghost_record_violation(ASX_GHOST_DETERMINISM_DRIFT, 0, (int)g_ghost_det_ref_count,
                               (int)g_ghost_det_count);
        drift_count++;
    }

    /* Compare element-by-element up to the shorter sequence */
    check_count =
        g_ghost_det_count < g_ghost_det_ref_count ? g_ghost_det_count : g_ghost_det_ref_count;
    if (check_count > ASX_GHOST_DETERMINISM_CAPACITY) {
        check_count = ASX_GHOST_DETERMINISM_CAPACITY;
    }

    for (i = 0; i < check_count; i++) {
        if (g_ghost_det_events[i] != g_ghost_det_reference[i]) {
            ghost_record_violation(ASX_GHOST_DETERMINISM_DRIFT, g_ghost_det_events[i], (int)i, 0);
            drift_count++;
        }
    }

    return drift_count;
}

uint64_t asx_ghost_determinism_digest(void) {
    uint64_t hash = 0x517cc1b727220a95ULL; /* FNV-1a offset basis */
    uint32_t count;
    uint32_t i;

    count = g_ghost_det_count < ASX_GHOST_DETERMINISM_CAPACITY ? g_ghost_det_count
                                                               : ASX_GHOST_DETERMINISM_CAPACITY;

    for (i = 0; i < count; i++) {
        hash ^= g_ghost_det_events[i];
        hash *= 0x00000100000001B3ULL; /* FNV-1a prime */
    }

    return hash;
}

uint32_t asx_ghost_determinism_event_count(void) { return g_ghost_det_count; }

#endif /* ASX_DEBUG_GHOST */
