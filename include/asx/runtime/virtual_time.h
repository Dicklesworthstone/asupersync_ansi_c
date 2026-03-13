/*
 * asx/runtime/virtual_time.h — Virtual-time injection layer (bd-3vt.8)
 *
 * Provides controlled clock injection for deterministic replay of
 * jitter, stalls, and clock jumps. Plugs into the existing clock
 * hook infrastructure (asx_clock_hooks.logical_now_ns_fn).
 *
 * Design:
 *   - Monotonic base timeline with programmed anomaly events
 *   - Anomalies: jitter (±ns), stall (hold time), jump (forward leap)
 *   - Each query advances the virtual clock by a configurable tick
 *   - Anomaly schedule is deterministic: same inputs → same outputs
 *   - Falls back to monotonic advance when no anomalies programmed
 *
 * Usage:
 *   asx_vtime_state vt;
 *   asx_vtime_init(&vt, 0, 1000);           // start=0, tick=1µs
 *   asx_vtime_add_jitter(&vt, 5, 500, 200); // at query 5: +200ns jitter
 *   asx_vtime_install(&vt);                  // install as logical clock
 *   // ... runtime queries via asx_runtime_now_ns() ...
 *   asx_vtime_uninstall(&vt);
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_RUNTIME_VIRTUAL_TIME_H
#define ASX_RUNTIME_VIRTUAL_TIME_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Anomaly types                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_VTIME_ANOMALY_JITTER = 0, /* Additive offset (±param ns) */
    ASX_VTIME_ANOMALY_STALL = 1,  /* Hold time constant for N queries */
    ASX_VTIME_ANOMALY_JUMP = 2    /* Forward leap of param ns */
} asx_vtime_anomaly_kind;

/* ------------------------------------------------------------------ */
/* Anomaly event                                                       */
/* ------------------------------------------------------------------ */

#define ASX_VTIME_MAX_ANOMALIES 32u

typedef struct {
    asx_vtime_anomaly_kind kind;
    uint32_t trigger_query; /* Fire at this query index (0-based) */
    int64_t param;          /* Kind-dependent: offset/hold-count/leap */
    uint32_t duration;      /* STALL: number of queries to hold */
} asx_vtime_anomaly;

/* ------------------------------------------------------------------ */
/* Virtual-time state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_time base_time;    /* Starting time (ns) */
    asx_time current_time; /* Current virtual time */
    uint64_t tick_ns;      /* Nanoseconds per query advance */
    uint32_t query_count;  /* Total queries served */

    /* Anomaly schedule */
    asx_vtime_anomaly anomalies[ASX_VTIME_MAX_ANOMALIES];
    uint32_t anomaly_count;

    /* Stall tracking */
    uint32_t stall_remaining; /* Queries remaining in current stall */
} asx_vtime_state;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Initialize virtual-time state.
 * base_ns: starting time in nanoseconds
 * tick_ns: time advance per query (0 = no auto-advance) */
ASX_API void asx_vtime_init(asx_vtime_state *vt, asx_time base_ns, uint64_t tick_ns);

/* Reset to initial state (preserves anomaly schedule). */
ASX_API void asx_vtime_reset(asx_vtime_state *vt);

/* Add a jitter anomaly: at query trigger_query, offset time by delta_ns.
 * delta_ns can be negative (clock reversal simulation).
 * Returns ASX_OK or ASX_E_RESOURCE_EXHAUSTED if schedule is full. */
ASX_API asx_status asx_vtime_add_jitter(asx_vtime_state *vt, uint32_t trigger_query,
                                        int64_t delta_ns);

/* Add a stall anomaly: at query trigger_query, hold time constant
 * for hold_queries queries.
 * Returns ASX_OK or ASX_E_RESOURCE_EXHAUSTED if schedule is full. */
ASX_API asx_status asx_vtime_add_stall(asx_vtime_state *vt, uint32_t trigger_query,
                                       uint32_t hold_queries);

/* Add a jump anomaly: at query trigger_query, leap forward by jump_ns.
 * Returns ASX_OK or ASX_E_RESOURCE_EXHAUSTED if schedule is full. */
ASX_API asx_status asx_vtime_add_jump(asx_vtime_state *vt, uint32_t trigger_query,
                                      uint64_t jump_ns);

/* Query virtual time (intended as logical_now_ns_fn callback).
 * Advances time by tick_ns and applies any matching anomalies.
 * Thread-safe: no — single-threaded only (walking skeleton). */
ASX_API asx_time asx_vtime_now_ns(void *ctx);

/* Install as the runtime logical clock.
 * Saves and replaces the current clock hooks.
 * Call asx_vtime_uninstall() to restore. */
ASX_API asx_status asx_vtime_install(asx_vtime_state *vt);

/* Uninstall and restore previous clock hooks. */
ASX_API void asx_vtime_uninstall(asx_vtime_state *vt);

/* Query: current virtual time without advancing. */
ASX_API asx_time asx_vtime_current(const asx_vtime_state *vt);

/* Query: number of queries served so far. */
ASX_API uint32_t asx_vtime_query_count(const asx_vtime_state *vt);

#ifdef __cplusplus
}
#endif

#endif /* ASX_RUNTIME_VIRTUAL_TIME_H */
