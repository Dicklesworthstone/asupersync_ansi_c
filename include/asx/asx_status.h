/*
 * asx_status.h — status and error code taxonomy
 *
 * All public API functions return asx_status. Error codes are grouped
 * into stable families. New codes may be added; existing codes never
 * silently change meaning.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_STATUS_H
#define ASX_STATUS_H

#include <asx/asx_export.h>

typedef enum {
    /* Success */
    ASX_OK = 0,

    /* General errors (1xx) */
    ASX_E_INVALID_ARGUMENT     = 100,
    ASX_E_INVALID_STATE        = 101,
    ASX_E_NOT_FOUND            = 102,
    ASX_E_ALREADY_EXISTS       = 103,

    /* Transition errors (2xx) */
    ASX_E_INVALID_TRANSITION   = 200,

    /* Region errors (3xx) */
    ASX_E_REGION_NOT_FOUND     = 300,
    ASX_E_REGION_CLOSED        = 301,
    ASX_E_REGION_AT_CAPACITY   = 302,
    ASX_E_REGION_NOT_OPEN      = 303,
    ASX_E_ADMISSION_CLOSED     = 304,
    ASX_E_ADMISSION_LIMIT      = 305,

    /* Task errors (4xx) */
    ASX_E_TASK_NOT_FOUND       = 400,
    ASX_E_SCHEDULER_UNAVAILABLE = 401,
    ASX_E_NAME_CONFLICT        = 402,

    /* Obligation errors (5xx) */
    ASX_E_OBLIGATION_ALREADY_RESOLVED = 500,
    ASX_E_UNRESOLVED_OBLIGATIONS      = 501,

    /* Cancel errors (6xx) */
    ASX_E_CANCELLED            = 600,
    ASX_E_WITNESS_PHASE_REGRESSION   = 601,
    ASX_E_WITNESS_REASON_WEAKENED    = 602,
    ASX_E_WITNESS_TASK_MISMATCH      = 603,
    ASX_E_WITNESS_REGION_MISMATCH    = 604,
    ASX_E_WITNESS_EPOCH_MISMATCH     = 605,

    /* Channel errors (7xx) */
    ASX_E_DISCONNECTED         = 700,
    ASX_E_WOULD_BLOCK          = 701,
    ASX_E_CHANNEL_FULL         = 702,
    ASX_E_CHANNEL_NOT_DRAINED  = 703,

    /* Timer errors (8xx) */
    ASX_E_TIMER_NOT_FOUND      = 800,
    ASX_E_TIMERS_PENDING       = 801,

    /* Quiescence errors (9xx) */
    ASX_E_TASKS_STILL_ACTIVE   = 900,
    ASX_E_OBLIGATIONS_UNRESOLVED = 901,
    ASX_E_REGIONS_NOT_CLOSED   = 902,
    ASX_E_INCOMPLETE_CHILDREN  = 903,

    /* Resource exhaustion (10xx) */
    ASX_E_RESOURCE_EXHAUSTED   = 1000,

    /* Handle errors (11xx) */
    ASX_E_STALE_HANDLE         = 1100

} asx_status;

/* Returns a human-readable string for a status code. Never returns NULL. */
ASX_API const char *asx_status_str(asx_status s);

/* Returns nonzero if the status represents an error. */
static inline int asx_is_error(asx_status s) {
    return s != ASX_OK;
}

#endif /* ASX_STATUS_H */
