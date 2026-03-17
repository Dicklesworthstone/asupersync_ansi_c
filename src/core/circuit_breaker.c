/*
 * circuit_breaker.c — circuit breaker for failure containment
 *
 * Walking skeleton: single-threaded, synchronous state transitions.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/core/circuit_breaker.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void asx_breaker_init(asx_circuit_breaker *cb) {
    if (cb == NULL) return;
    memset(cb, 0, sizeof(*cb));
    cb->config.failure_threshold = 5;
    cb->config.success_threshold = 2;
    cb->config.half_open_max_calls = 1;
    cb->state = ASX_BREAKER_CLOSED;
}

asx_status asx_breaker_init_with(asx_circuit_breaker *cb, const asx_breaker_config *config) {
    if (cb == NULL || config == NULL) return ASX_E_INVALID_ARGUMENT;
    if (config->failure_threshold == 0) return ASX_E_INVALID_ARGUMENT;
    if (config->success_threshold == 0) return ASX_E_INVALID_ARGUMENT;
    memset(cb, 0, sizeof(*cb));
    cb->config = *config;
    cb->state = ASX_BREAKER_CLOSED;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Call lifecycle                                                       */
/* ------------------------------------------------------------------ */

asx_status asx_breaker_allow(asx_circuit_breaker *cb) {
    if (cb == NULL) return ASX_E_INVALID_ARGUMENT;
    cb->total_calls++;

    switch (cb->state) {
    case ASX_BREAKER_CLOSED:
        return ASX_OK;

    case ASX_BREAKER_OPEN:
        cb->total_rejected++;
        return ASX_E_ADMISSION_CLOSED;

    case ASX_BREAKER_HALF_OPEN:
        if (cb->half_open_active >= cb->config.half_open_max_calls) {
            cb->total_rejected++;
            return ASX_E_WOULD_BLOCK;
        }
        cb->half_open_active++;
        return ASX_OK;
    }

    return ASX_E_INVALID_STATE;
}

void asx_breaker_record_success(asx_circuit_breaker *cb) {
    if (cb == NULL) return;

    switch (cb->state) {
    case ASX_BREAKER_CLOSED:
        cb->failure_count = 0;
        break;

    case ASX_BREAKER_HALF_OPEN:
        cb->success_count++;
        if (cb->half_open_active > 0) cb->half_open_active--;
        if (cb->success_count >= cb->config.success_threshold) {
            /* Transition to CLOSED */
            cb->state = ASX_BREAKER_CLOSED;
            cb->failure_count = 0;
            cb->success_count = 0;
            cb->half_open_active = 0;
        }
        break;

    case ASX_BREAKER_OPEN:
        break; /* ignore success in OPEN (shouldn't happen) */
    }
}

void asx_breaker_record_failure(asx_circuit_breaker *cb) {
    if (cb == NULL) return;

    switch (cb->state) {
    case ASX_BREAKER_CLOSED:
        cb->failure_count++;
        if (cb->failure_count >= cb->config.failure_threshold) {
            cb->state = ASX_BREAKER_OPEN;
            cb->total_trips++;
        }
        break;

    case ASX_BREAKER_HALF_OPEN:
        /* Immediately trip back to OPEN */
        cb->state = ASX_BREAKER_OPEN;
        cb->total_trips++;
        cb->success_count = 0;
        if (cb->half_open_active > 0) cb->half_open_active--;
        break;

    case ASX_BREAKER_OPEN:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Manual transitions                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_breaker_half_open(asx_circuit_breaker *cb) {
    if (cb == NULL) return ASX_E_INVALID_ARGUMENT;
    if (cb->state != ASX_BREAKER_OPEN) return ASX_E_INVALID_STATE;
    cb->state = ASX_BREAKER_HALF_OPEN;
    cb->success_count = 0;
    cb->half_open_active = 0;
    return ASX_OK;
}

void asx_breaker_reset(asx_circuit_breaker *cb) {
    if (cb == NULL) return;
    cb->state = ASX_BREAKER_CLOSED;
    cb->failure_count = 0;
    cb->success_count = 0;
    cb->half_open_active = 0;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

asx_breaker_state asx_breaker_get_state(const asx_circuit_breaker *cb) {
    if (cb == NULL) return ASX_BREAKER_CLOSED;
    return cb->state;
}

uint32_t asx_breaker_failure_count(const asx_circuit_breaker *cb) {
    if (cb == NULL) return 0;
    return cb->failure_count;
}

uint32_t asx_breaker_total_trips(const asx_circuit_breaker *cb) {
    if (cb == NULL) return 0;
    return cb->total_trips;
}

uint32_t asx_breaker_total_rejected(const asx_circuit_breaker *cb) {
    if (cb == NULL) return 0;
    return cb->total_rejected;
}

const char *asx_breaker_state_str(asx_breaker_state state) {
    switch (state) {
    case ASX_BREAKER_CLOSED: return "closed";
    case ASX_BREAKER_OPEN: return "open";
    case ASX_BREAKER_HALF_OPEN: return "half_open";
    default: return "unknown";
    }
}
