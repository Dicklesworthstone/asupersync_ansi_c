/*
 * arena_locality_spike.c — Cache-oblivious arena layout evaluation (bd-3vt.3)
 *
 * Evaluates two layout strategies against the baseline AoS (Array of Structures):
 *
 * 1. Hot/Cold split: Separate frequently-accessed scheduler fields from
 *    cold cancel/capture data to pack more hot fields per cache line.
 *
 * 2. SoA (Structure of Arrays): Columnar layout where each field is a
 *    contiguous array. Maximizes prefetcher efficiency for single-field scans.
 *
 * Both are compared to the baseline asx_task_slot linear scan.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE() -- arena locality spike, no checkpoint coverage needed */

#include <asx/asx.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Baseline: current AoS layout (mirrors runtime_internal.h)          */
/* ------------------------------------------------------------------ */

#define SPIKE_MAX_TASKS 64

typedef struct {
    asx_task_state state;
    asx_region_id region;
    asx_task_poll_fn poll_fn;
    void *user_data;
    asx_outcome outcome;
    uint16_t generation;
    int alive;
    void *captured_state;
    uint32_t captured_size;
    asx_task_state_dtor_fn captured_dtor;
    asx_cancel_phase cancel_phase;
    asx_cancel_reason cancel_reason; /* 48 bytes */
    uint32_t cancel_epoch;
    uint32_t cleanup_polls_remaining;
    int cancel_pending;
} spike_task_slot_aos;

/* ------------------------------------------------------------------ */
/* Strategy 1: Hot/Cold split                                         */
/* ------------------------------------------------------------------ */

/*
 * Hot fields (scheduler scan): state, region, alive, poll_fn, user_data
 * Size: ~32 bytes — fits in half a cache line.
 *
 * Cold fields: everything else (cancel, outcome, capture, generation)
 */

typedef struct {
    asx_task_state state;     /* 4 */
    int alive;                /* 4 */
    asx_region_id region;     /* 8 */
    asx_task_poll_fn poll_fn; /* 8 */
    void *user_data;          /* 8 */
    /* 32 bytes total — exactly half a cache line */
} spike_task_hot;

typedef struct {
    asx_outcome outcome;
    uint16_t generation;
    void *captured_state;
    uint32_t captured_size;
    asx_task_state_dtor_fn captured_dtor;
    asx_cancel_phase cancel_phase;
    asx_cancel_reason cancel_reason;
    uint32_t cancel_epoch;
    uint32_t cleanup_polls_remaining;
    int cancel_pending;
} spike_task_cold;

/* ------------------------------------------------------------------ */
/* Strategy 2: SoA (Structure of Arrays)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_task_state states[SPIKE_MAX_TASKS];
    int alive[SPIKE_MAX_TASKS];
    asx_region_id regions[SPIKE_MAX_TASKS];
    asx_task_poll_fn poll_fns[SPIKE_MAX_TASKS];
    void *user_data[SPIKE_MAX_TASKS];
    asx_outcome outcomes[SPIKE_MAX_TASKS];
    uint16_t generations[SPIKE_MAX_TASKS];
    void *captured_states[SPIKE_MAX_TASKS];
    uint32_t captured_sizes[SPIKE_MAX_TASKS];
    asx_task_state_dtor_fn captured_dtors[SPIKE_MAX_TASKS];
    asx_cancel_phase cancel_phases[SPIKE_MAX_TASKS];
    asx_cancel_reason cancel_reasons[SPIKE_MAX_TASKS];
    uint32_t cancel_epochs[SPIKE_MAX_TASKS];
    uint32_t cleanup_polls[SPIKE_MAX_TASKS];
    int cancel_pendings[SPIKE_MAX_TASKS];
} spike_task_soa;

/* ------------------------------------------------------------------ */
/* Forward declarations (satisfy -Wmissing-prototypes)                */
/* ------------------------------------------------------------------ */

uint32_t spike_sizeof_aos(void);
uint32_t spike_sizeof_hot(void);
uint32_t spike_sizeof_cold(void);
uint32_t spike_sizeof_soa_total(void);
uint32_t spike_scan_aos(const spike_task_slot_aos *slots, uint32_t count, asx_region_id target);
uint32_t spike_scan_hotcold(const spike_task_hot *hot, uint32_t count, asx_region_id target);
uint32_t spike_scan_soa(const spike_task_soa *soa, uint32_t count, asx_region_id target);
uint32_t spike_cachelines_aos(uint32_t count);
uint32_t spike_cachelines_hotcold(uint32_t count);
uint32_t spike_cachelines_soa(uint32_t count);

/* ------------------------------------------------------------------ */
/* Size reporting                                                     */
/* ------------------------------------------------------------------ */

uint32_t spike_sizeof_aos(void) { return (uint32_t)sizeof(spike_task_slot_aos); }

uint32_t spike_sizeof_hot(void) { return (uint32_t)sizeof(spike_task_hot); }

uint32_t spike_sizeof_cold(void) { return (uint32_t)sizeof(spike_task_cold); }

uint32_t spike_sizeof_soa_total(void) { return (uint32_t)sizeof(spike_task_soa); }

/* ------------------------------------------------------------------ */
/* Simulated scheduler scan — baseline AoS                            */
/* ------------------------------------------------------------------ */

uint32_t spike_scan_aos(const spike_task_slot_aos *slots, uint32_t count,
                        asx_region_id target_region) {
    uint32_t i;
    uint32_t ready = 0;

    for (i = 0; i < count; i++) {
        if (!slots[i].alive) continue;
        if (slots[i].region != target_region) continue;
        if (slots[i].state == ASX_TASK_COMPLETED) continue;
        ready++;
    }
    return ready;
}

/* ------------------------------------------------------------------ */
/* Simulated scheduler scan — hot/cold split                          */
/* ------------------------------------------------------------------ */

uint32_t spike_scan_hotcold(const spike_task_hot *hot, uint32_t count,
                            asx_region_id target_region) {
    uint32_t i;
    uint32_t ready = 0;

    for (i = 0; i < count; i++) {
        if (!hot[i].alive) continue;
        if (hot[i].region != target_region) continue;
        if (hot[i].state == ASX_TASK_COMPLETED) continue;
        ready++;
    }
    return ready;
}

/* ------------------------------------------------------------------ */
/* Simulated scheduler scan — SoA                                     */
/* ------------------------------------------------------------------ */

uint32_t spike_scan_soa(const spike_task_soa *soa, uint32_t count, asx_region_id target_region) {
    uint32_t i;
    uint32_t ready = 0;

    for (i = 0; i < count; i++) {
        if (!soa->alive[i]) continue;
        if (soa->regions[i] != target_region) continue;
        if (soa->states[i] == ASX_TASK_COMPLETED) continue;
        ready++;
    }
    return ready;
}

/* ------------------------------------------------------------------ */
/* Cache line utilization analysis                                    */
/* ------------------------------------------------------------------ */

/*
 * Returns the number of cache lines touched during a full scan of
 * `count` task slots for a given layout strategy.
 *
 * Assumes 64-byte cache lines.
 */
#define CACHE_LINE_SIZE 64u

uint32_t spike_cachelines_aos(uint32_t count) {
    /* Each slot is sizeof(spike_task_slot_aos). Hot fields span first ~32 bytes,
     * but the slot strides by full sizeof, so each slot touches at least 1 line,
     * potentially straddling lines. */
    uint32_t slot_size = (uint32_t)sizeof(spike_task_slot_aos);
    uint32_t total_bytes = count * slot_size;
    return (total_bytes + CACHE_LINE_SIZE - 1u) / CACHE_LINE_SIZE;
}

uint32_t spike_cachelines_hotcold(uint32_t count) {
    /* Only hot array is scanned. Cold array untouched during scan. */
    uint32_t hot_size = (uint32_t)sizeof(spike_task_hot);
    uint32_t total_bytes = count * hot_size;
    return (total_bytes + CACHE_LINE_SIZE - 1u) / CACHE_LINE_SIZE;
}

uint32_t spike_cachelines_soa(uint32_t count) {
    /* Three arrays scanned: alive[], regions[], states[]
     * Each is count * element_size. */
    uint32_t alive_bytes = count * (uint32_t)sizeof(int);
    uint32_t region_bytes = count * (uint32_t)sizeof(asx_region_id);
    uint32_t state_bytes = count * (uint32_t)sizeof(asx_task_state);
    uint32_t alive_lines = (alive_bytes + CACHE_LINE_SIZE - 1u) / CACHE_LINE_SIZE;
    uint32_t region_lines = (region_bytes + CACHE_LINE_SIZE - 1u) / CACHE_LINE_SIZE;
    uint32_t state_lines = (state_bytes + CACHE_LINE_SIZE - 1u) / CACHE_LINE_SIZE;
    return alive_lines + region_lines + state_lines;
}
