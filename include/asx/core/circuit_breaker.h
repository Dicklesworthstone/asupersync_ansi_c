/*
 * asx/core/circuit_breaker.h — circuit breaker for failure containment
 *
 * Implements the classic circuit breaker pattern with three states:
 *   CLOSED  — normal operation, tracking failures
 *   OPEN    — tripping threshold exceeded, fast-failing requests
 *   HALF_OPEN — probing recovery with limited traffic
 *
 * Walking skeleton: single-threaded, synchronous state transitions.
 * No timers — caller drives half-open probes explicitly.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_CIRCUIT_BREAKER_H
#define ASX_CORE_CIRCUIT_BREAKER_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Circuit breaker state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_BREAKER_CLOSED = 0,   /* normal: tracking failures */
    ASX_BREAKER_OPEN = 1,     /* tripped: fast-failing */
    ASX_BREAKER_HALF_OPEN = 2 /* probing: limited traffic */
} asx_breaker_state;

/* -------------------------------------------------------------------
 * Circuit breaker configuration
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t failure_threshold;   /* failures before tripping (default: 5) */
    uint32_t success_threshold;   /* successes in half-open to close (default: 2) */
    uint32_t half_open_max_calls; /* max concurrent calls in half-open (default: 1) */
} asx_breaker_config;

/* -------------------------------------------------------------------
 * Circuit breaker instance
 * ------------------------------------------------------------------- */

typedef struct {
    asx_breaker_config config;
    asx_breaker_state state;
    uint32_t failure_count;    /* consecutive failures in CLOSED */
    uint32_t success_count;    /* consecutive successes in HALF_OPEN */
    uint32_t half_open_active; /* active calls in HALF_OPEN */
    uint32_t total_trips;      /* total times breaker has tripped */
    uint32_t total_calls;      /* total call attempts */
    uint32_t total_rejected;   /* total fast-fail rejections */
} asx_circuit_breaker;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Initialize with default config (5 failures to trip, 2 successes to close). */
ASX_API void asx_breaker_init(asx_circuit_breaker *cb);

/* Initialize with custom config.
 * All thresholds and half-open probe capacity must be nonzero. */
ASX_API ASX_MUST_USE asx_status asx_breaker_init_with(asx_circuit_breaker *cb,
                                                      const asx_breaker_config *config);

/* -------------------------------------------------------------------
 * API: Call lifecycle
 * ------------------------------------------------------------------- */

/* Check if a call is allowed (pre-call gate).
 * Returns ASX_OK if allowed, ASX_E_ADMISSION_CLOSED if breaker is OPEN.
 * In HALF_OPEN, returns ASX_OK up to half_open_max_calls, then ASX_E_WOULD_BLOCK. */
ASX_API ASX_MUST_USE asx_status asx_breaker_allow(asx_circuit_breaker *cb);

/* Record a successful call (post-call).
 * In CLOSED: resets failure count.
 * In HALF_OPEN: increments success count; may transition to CLOSED. */
ASX_API void asx_breaker_record_success(asx_circuit_breaker *cb);

/* Record a failed call (post-call).
 * In CLOSED: increments failure count; may transition to OPEN.
 * In HALF_OPEN: transitions immediately to OPEN. */
ASX_API void asx_breaker_record_failure(asx_circuit_breaker *cb);

/* -------------------------------------------------------------------
 * API: Manual state transitions
 * ------------------------------------------------------------------- */

/* Manually transition from OPEN to HALF_OPEN for probing.
 * Returns ASX_E_INVALID_STATE if not currently OPEN. */
ASX_API ASX_MUST_USE asx_status asx_breaker_half_open(asx_circuit_breaker *cb);

/* Manually reset to CLOSED state. */
ASX_API void asx_breaker_reset(asx_circuit_breaker *cb);

/* -------------------------------------------------------------------
 * API: Queries
 * ------------------------------------------------------------------- */

/* Get the current breaker state (closed/open/half-open). */
ASX_API asx_breaker_state asx_breaker_get_state(const asx_circuit_breaker *cb);
/* Get the current consecutive failure count. */
ASX_API uint32_t asx_breaker_failure_count(const asx_circuit_breaker *cb);
/* Get the total number of times the breaker has tripped. */
ASX_API uint32_t asx_breaker_total_trips(const asx_circuit_breaker *cb);
/* Get the total number of rejected (fast-failed) calls. */
ASX_API uint32_t asx_breaker_total_rejected(const asx_circuit_breaker *cb);

/* Return the human-readable name for a breaker state. */
ASX_API const char *asx_breaker_state_str(asx_breaker_state state);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_CIRCUIT_BREAKER_H */
