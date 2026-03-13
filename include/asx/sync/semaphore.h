/*
 * asx/sync/semaphore.h — counting semaphore with permit obligations
 *
 * Async-friendly counting semaphore. Permits are tracked obligations
 * that must be released. Cancel-safe: cancellation during acquire
 * does not consume a permit.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_SEMAPHORE_H
#define ASX_SYNC_SEMAPHORE_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_SEMAPHORE_MAX
#define ASX_SEMAPHORE_MAX 16u
#endif

#ifndef ASX_SEMAPHORE_MAX_WAITERS
#define ASX_SEMAPHORE_MAX_WAITERS 16u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_semaphore_handle;

typedef struct {
    uint32_t sem_slot;
    uint16_t generation;
} asx_semaphore_permit;

typedef struct {
    uint32_t sem_slot;
    uint32_t waiter_slot;
    uint16_t generation;
} asx_semaphore_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a counting semaphore with initial permits. */
ASX_API ASX_MUST_USE asx_status asx_semaphore_create(uint32_t initial_permits,
                                                     asx_semaphore_handle *out);

/* Close a semaphore, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_semaphore_close(asx_semaphore_handle handle);

/* -------------------------------------------------------------------
 * Acquire (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to acquire a permit immediately. Returns ASX_OK + permit if
 * available, ASX_E_WOULD_BLOCK if none available. */
ASX_API asx_status asx_semaphore_try_acquire(asx_semaphore_handle handle,
                                             asx_semaphore_permit *out);

/* -------------------------------------------------------------------
 * Acquire (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async acquire. Must be followed by poll_acquire calls. */
ASX_API ASX_MUST_USE asx_status asx_semaphore_acquire_begin(asx_semaphore_handle handle,
                                                            asx_semaphore_waiter *out);

/* Poll for permit. Returns ASX_OK + permit when acquired,
 * ASX_E_PENDING when waiting. */
ASX_API asx_status asx_semaphore_poll_acquire(asx_semaphore_waiter *waiter,
                                              asx_semaphore_permit *out, asx_cx *cx);

/* Cancel an async acquire (safe even if already acquired). */
ASX_API asx_status asx_semaphore_acquire_cancel(asx_semaphore_waiter *waiter);

/* -------------------------------------------------------------------
 * Release
 * ------------------------------------------------------------------- */

/* Release a permit back to the semaphore. Wakes one waiter if any. */
ASX_API asx_status asx_semaphore_release(asx_semaphore_permit permit);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of available permits. */
ASX_API uint32_t asx_semaphore_available(asx_semaphore_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

ASX_API void asx_semaphore_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_SEMAPHORE_H */
