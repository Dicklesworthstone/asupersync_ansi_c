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
#include <asx/asx_ids.h>
#include <stdint.h>

typedef enum {
    /* Success */
    ASX_OK = 0,

    /* Non-error status (1-9) */
    ASX_E_PENDING = 1, /* operation not yet complete */

    /* General errors (1xx) */
    ASX_E_INVALID_ARGUMENT = 100,
    ASX_E_INVALID_STATE = 101,
    ASX_E_NOT_FOUND = 102,
    ASX_E_ALREADY_EXISTS = 103,
    ASX_E_BUFFER_TOO_SMALL = 104,

    /* Transition errors (2xx) */
    ASX_E_INVALID_TRANSITION = 200,

    /* Region errors (3xx) */
    ASX_E_REGION_NOT_FOUND = 300,
    ASX_E_REGION_CLOSED = 301,
    ASX_E_REGION_AT_CAPACITY = 302,
    ASX_E_REGION_NOT_OPEN = 303,
    ASX_E_ADMISSION_CLOSED = 304,
    ASX_E_ADMISSION_LIMIT = 305,
    ASX_E_REGION_POISONED = 306, /* region poisoned after safety violation */

    /* Task errors (4xx) */
    ASX_E_TASK_NOT_FOUND = 400,
    ASX_E_SCHEDULER_UNAVAILABLE = 401,
    ASX_E_NAME_CONFLICT = 402,
    ASX_E_TASK_NOT_COMPLETED = 403,
    ASX_E_POLL_BUDGET_EXHAUSTED = 404,

    /* Obligation errors (5xx) */
    ASX_E_OBLIGATION_ALREADY_RESOLVED = 500,
    ASX_E_UNRESOLVED_OBLIGATIONS = 501,

    /* Cancel errors (6xx) */
    ASX_E_CANCELLED = 600,
    ASX_E_WITNESS_PHASE_REGRESSION = 601,
    ASX_E_WITNESS_REASON_WEAKENED = 602,
    ASX_E_WITNESS_TASK_MISMATCH = 603,
    ASX_E_WITNESS_REGION_MISMATCH = 604,
    ASX_E_WITNESS_EPOCH_MISMATCH = 605,

    /* Channel errors (7xx) */
    ASX_E_DISCONNECTED = 700,
    ASX_E_WOULD_BLOCK = 701,
    ASX_E_CHANNEL_FULL = 702,
    ASX_E_CHANNEL_NOT_DRAINED = 703,
    ASX_E_LAGGED = 704, /* broadcast receiver fell behind */

    /* Timer errors (8xx) */
    ASX_E_TIMER_NOT_FOUND = 800,
    ASX_E_TIMERS_PENDING = 801,
    ASX_E_TIMER_DURATION_EXCEEDED = 802,
    ASX_E_TIMED_OUT = 803, /* operation exceeded deadline */

    /* Quiescence errors (9xx) */
    ASX_E_TASKS_STILL_ACTIVE = 900,
    ASX_E_OBLIGATIONS_UNRESOLVED = 901,
    ASX_E_REGIONS_NOT_CLOSED = 902,
    ASX_E_INCOMPLETE_CHILDREN = 903,
    ASX_E_QUIESCENCE_NOT_REACHED = 904,
    ASX_E_QUIESCENCE_TASKS_LIVE = 905,

    /* Resource exhaustion (10xx) */
    ASX_E_RESOURCE_EXHAUSTED = 1000,
    ASX_E_OVERLOADED = 1001, /* service rejected due to backpressure/load shedding */

    /* Handle errors (11xx) */
    ASX_E_STALE_HANDLE = 1100,

    /* Hook/runtime contract errors (12xx) */
    ASX_E_HOOK_MISSING = 1200,
    ASX_E_HOOK_INVALID = 1201,
    ASX_E_DETERMINISM_VIOLATION = 1202,
    ASX_E_ALLOCATOR_SEALED = 1203,

    /* Thread-affinity errors (13xx) */
    ASX_E_AFFINITY_VIOLATION = 1300,         /* access from wrong domain */
    ASX_E_AFFINITY_NOT_BOUND = 1301,         /* entity has no domain binding */
    ASX_E_AFFINITY_ALREADY_BOUND = 1302,     /* entity already bound to a domain */
    ASX_E_AFFINITY_TRANSFER_REQUIRED = 1303, /* cross-domain access without transfer */
    ASX_E_AFFINITY_TABLE_FULL = 1304,        /* tracking table exhausted */

    /* Codec equivalence errors (14xx) */
    ASX_E_EQUIVALENCE_MISMATCH = 1400, /* cross-codec semantic mismatch */

    /* Trace/replay errors (15xx) */
    ASX_E_REPLAY_MISMATCH = 1500, /* replay continuity check failed */

    /* 16xx: config reload errors */
    ASX_E_CONFIG_FROZEN = 1600,      /* attempted to change frozen field */
    ASX_E_CONFIG_RESTART_REQ = 1601, /* field requires restart to change */

    /* Permission / capability errors (17xx) */
    ASX_E_PERMISSION_DENIED = 1700 /* capability not granted */

} asx_status;

/* Returns a human-readable string for a status code. Never returns NULL. */
ASX_API ASX_MUST_USE const char *asx_status_str(asx_status s);

/* Returns nonzero if the status represents an error. */
static inline int asx_is_error(asx_status s) { return s != ASX_OK; }

/* ------------------------------------------------------------------ */
/* Public error taxonomy                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_ERROR_CATEGORY_NONE = 0,
    ASX_ERROR_CATEGORY_GENERAL = 1,
    ASX_ERROR_CATEGORY_TRANSITION = 2,
    ASX_ERROR_CATEGORY_REGION = 3,
    ASX_ERROR_CATEGORY_TASK = 4,
    ASX_ERROR_CATEGORY_OBLIGATION = 5,
    ASX_ERROR_CATEGORY_CANCELLATION = 6,
    ASX_ERROR_CATEGORY_CHANNEL = 7,
    ASX_ERROR_CATEGORY_TIMER = 8,
    ASX_ERROR_CATEGORY_QUIESCENCE = 9,
    ASX_ERROR_CATEGORY_RESOURCE = 10,
    ASX_ERROR_CATEGORY_HANDLE = 11,
    ASX_ERROR_CATEGORY_HOOK = 12,
    ASX_ERROR_CATEGORY_AFFINITY = 13,
    ASX_ERROR_CATEGORY_EQUIVALENCE = 14,
    ASX_ERROR_CATEGORY_REPLAY = 15,
    ASX_ERROR_CATEGORY_CONFIG = 16,
    ASX_ERROR_CATEGORY_PERMISSION = 17
} asx_error_category;

typedef enum {
    ASX_RECOVERABILITY_NONE = 0,
    ASX_RECOVERABILITY_TRANSIENT = 1,
    ASX_RECOVERABILITY_PERMANENT = 2,
    ASX_RECOVERABILITY_CONTEXT_DEPENDENT = 3
} asx_recoverability;

typedef enum {
    ASX_RECOVERY_NONE = 0,
    ASX_RECOVERY_RETRY_IMMEDIATELY = 1,
    ASX_RECOVERY_RETRY_WITH_BACKOFF = 2,
    ASX_RECOVERY_REINITIALIZE_CONTEXT = 3,
    ASX_RECOVERY_PROPAGATE = 4,
    ASX_RECOVERY_ESCALATE = 5,
    ASX_RECOVERY_INSPECT_CONFIGURATION = 6
} asx_recovery_action;

typedef struct asx_backoff_hint {
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint8_t max_attempts;
} asx_backoff_hint;

ASX_API ASX_MUST_USE asx_error_category asx_status_category(asx_status s);
ASX_API ASX_MUST_USE const char *asx_error_category_str(asx_error_category category);
ASX_API ASX_MUST_USE asx_recoverability asx_status_recoverability(asx_status s);
ASX_API ASX_MUST_USE const char *asx_recoverability_str(asx_recoverability recoverability);
ASX_API ASX_MUST_USE asx_recovery_action asx_status_recovery_action(asx_status s);
ASX_API ASX_MUST_USE const char *asx_recovery_action_str(asx_recovery_action action);
ASX_API ASX_MUST_USE asx_backoff_hint asx_status_backoff_hint(asx_status s);
ASX_API ASX_MUST_USE int asx_status_is_retryable(asx_status s);
ASX_API ASX_MUST_USE int asx_status_is_cancel(asx_status s);

/* ------------------------------------------------------------------ */
/* Deterministic task-local error ledger (zero-allocation)            */
/* ------------------------------------------------------------------ */

#define ASX_ERROR_LEDGER_DEPTH 16u
#define ASX_ERROR_LEDGER_TASK_SLOTS 64u

typedef struct asx_error_ledger_entry {
    asx_task_id task_id;
    asx_status status;
    const char *operation;
    const char *file;
    uint32_t line;
    uint32_t sequence;
} asx_error_ledger_entry;

/* Reset all ledger state. Primarily for tests and deterministic replay setup. */
ASX_API void asx_error_ledger_reset(void);

/* Bind the implicit task context used by ASX_TRY(). */
ASX_API void asx_error_ledger_bind_task(asx_task_id task_id);

/* Return the currently bound task id (ASX_INVALID_ID if unbound). */
ASX_API ASX_MUST_USE asx_task_id asx_error_ledger_bound_task(void);

/* Record an error under the currently bound task context.
 * Returns void; records the error into the global ledger. */
ASX_API void asx_error_ledger_record_current(asx_status status, const char *operation,
                                             const char *file, uint32_t line);

/* Record an error for an explicit task id. */
ASX_API void asx_error_ledger_record_for_task(asx_task_id task_id, asx_status status,
                                              const char *operation, const char *file,
                                              uint32_t line);

/* Query entry count for a task-local ledger. */
ASX_API ASX_MUST_USE uint32_t asx_error_ledger_count(asx_task_id task_id);

/* Query whether a ledger has overwritten older entries (ring overflow). */
ASX_API ASX_MUST_USE int asx_error_ledger_overflowed(asx_task_id task_id);

/* Read entry i in chronological order (0 oldest). Returns nonzero on success. */
ASX_API ASX_MUST_USE int asx_error_ledger_get(asx_task_id task_id, uint32_t index,
                                              asx_error_ledger_entry *out_entry);

/* ------------------------------------------------------------------ */
/* Must-use coverage manifest                                         */
/* ------------------------------------------------------------------ */

/* Return the number of public API surfaces marked ASX_MUST_USE. */
ASX_API ASX_MUST_USE uint32_t asx_must_use_surface_count(void);

/* Return the name of the ASX_MUST_USE surface at the given index.
 * Returns NULL if index is out of range. */
ASX_API ASX_MUST_USE const char *asx_must_use_surface_name(uint32_t index);

/* ------------------------------------------------------------------ */
/* Ergonomic propagation helpers                                      */
/* ------------------------------------------------------------------ */

#define ASX_TRY(EXPR)                                                                              \
    do {                                                                                           \
        asx_status _asx_try_status = (EXPR);                                                       \
        if (_asx_try_status != ASX_OK) {                                                           \
            asx_error_ledger_record_current(_asx_try_status, #EXPR, __FILE__, (uint32_t)__LINE__); \
            return _asx_try_status;                                                                \
        }                                                                                          \
    } while (0)

#define ASX_TRY_TASK(TASK_ID, EXPR)                                                                \
    do {                                                                                           \
        asx_status _asx_try_status = (EXPR);                                                       \
        if (_asx_try_status != ASX_OK) {                                                           \
            asx_error_ledger_record_for_task((TASK_ID), _asx_try_status, #EXPR, __FILE__,          \
                                             (uint32_t)__LINE__);                                  \
            return _asx_try_status;                                                                \
        }                                                                                          \
    } while (0)

#endif /* ASX_STATUS_H */
