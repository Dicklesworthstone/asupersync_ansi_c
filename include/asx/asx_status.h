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

#include <stdint.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>

typedef enum {
    /* Success */
    ASX_OK = 0,

    /* Non-error status (1-9) */
    ASX_E_PENDING              = 1,   /* operation not yet complete */

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
    ASX_E_TASK_NOT_COMPLETED   = 403,
    ASX_E_POLL_BUDGET_EXHAUSTED = 404,

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
    ASX_E_TASKS_STILL_ACTIVE     = 900,
    ASX_E_OBLIGATIONS_UNRESOLVED = 901,
    ASX_E_REGIONS_NOT_CLOSED     = 902,
    ASX_E_INCOMPLETE_CHILDREN    = 903,
    ASX_E_QUIESCENCE_NOT_REACHED = 904,
    ASX_E_QUIESCENCE_TASKS_LIVE  = 905,

    /* Resource exhaustion (10xx) */
    ASX_E_RESOURCE_EXHAUSTED   = 1000,

    /* Handle errors (11xx) */
    ASX_E_STALE_HANDLE         = 1100,

    /* Hook/runtime contract errors (12xx) */
    ASX_E_HOOK_MISSING         = 1200,
    ASX_E_HOOK_INVALID         = 1201,
    ASX_E_DETERMINISM_VIOLATION = 1202,
    ASX_E_ALLOCATOR_SEALED     = 1203

} asx_status;

/* Returns a human-readable string for a status code. Never returns NULL. */
ASX_API ASX_MUST_USE const char *asx_status_str(asx_status s);

/* Returns nonzero if the status represents an error. */
static inline int asx_is_error(asx_status s) {
    return s != ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Deterministic task-local error ledger (zero-allocation)            */
/* ------------------------------------------------------------------ */

#define ASX_ERROR_LEDGER_DEPTH      16u
#define ASX_ERROR_LEDGER_TASK_SLOTS 64u

typedef struct asx_error_ledger_entry {
    asx_task_id task_id;
    asx_status  status;
    const char *operation;
    const char *file;
    uint32_t    line;
    uint32_t    sequence;
} asx_error_ledger_entry;

/* Reset all ledger state. Primarily for tests and deterministic replay setup. */
ASX_API void asx_error_ledger_reset(void);

/* Bind the implicit task context used by ASX_TRY(). */
ASX_API void asx_error_ledger_bind_task(asx_task_id task_id);

/* Return the currently bound task id (ASX_INVALID_ID if unbound). */
ASX_API ASX_MUST_USE asx_task_id asx_error_ledger_bound_task(void);

/* Record an error under the currently bound task context. */
ASX_API void asx_error_ledger_record_current(asx_status status,
                                             const char *operation,
                                             const char *file,
                                             uint32_t line);

/* Record an error for an explicit task id. */
ASX_API void asx_error_ledger_record_for_task(asx_task_id task_id,
                                              asx_status status,
                                              const char *operation,
                                              const char *file,
                                              uint32_t line);

/* Query entry count for a task-local ledger. */
ASX_API ASX_MUST_USE uint32_t asx_error_ledger_count(asx_task_id task_id);

/* Query whether a ledger has overwritten older entries (ring overflow). */
ASX_API ASX_MUST_USE int asx_error_ledger_overflowed(asx_task_id task_id);

/* Read entry i in chronological order (0 oldest). Returns nonzero on success. */
ASX_API ASX_MUST_USE int asx_error_ledger_get(asx_task_id task_id,
                                              uint32_t index,
                                              asx_error_ledger_entry *out_entry);

/* ------------------------------------------------------------------ */
/* Must-use coverage manifest                                         */
/* ------------------------------------------------------------------ */

ASX_API ASX_MUST_USE uint32_t asx_must_use_surface_count(void);
ASX_API ASX_MUST_USE const char *asx_must_use_surface_name(uint32_t index);

/* ------------------------------------------------------------------ */
/* Ergonomic propagation helpers                                      */
/* ------------------------------------------------------------------ */

#define ASX_TRY(EXPR)                                                      \
    do {                                                                   \
        asx_status _asx_try_status = (EXPR);                               \
        if (_asx_try_status != ASX_OK) {                                   \
            asx_error_ledger_record_current(_asx_try_status, #EXPR,        \
                                            __FILE__, (uint32_t)__LINE__); \
            return _asx_try_status;                                        \
        }                                                                  \
    } while (0)

#define ASX_TRY_TASK(TASK_ID, EXPR)                                        \
    do {                                                                    \
        asx_status _asx_try_status = (EXPR);                                \
        if (_asx_try_status != ASX_OK) {                                    \
            asx_error_ledger_record_for_task((TASK_ID), _asx_try_status,    \
                                             #EXPR, __FILE__,               \
                                             (uint32_t)__LINE__);           \
            return _asx_try_status;                                         \
        }                                                                   \
    } while (0)

#endif /* ASX_STATUS_H */
