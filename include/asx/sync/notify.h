/*
 * asx/sync/notify.h — event signaling primitive
 *
 * Notify provides async-friendly event signaling between tasks.
 * Waiters poll until notified. Supports single-waiter (notify_one)
 * and broadcast (notify_all) modes.
 *
 * Cancel-safe: cancellation during wait is clean, no state corrupted.
 * Cx-aware: checkpoints on each poll if Cx provided.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_NOTIFY_H
#define ASX_SYNC_NOTIFY_H

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

#ifndef ASX_NOTIFY_MAX
#define ASX_NOTIFY_MAX 16u
#endif

#ifndef ASX_NOTIFY_MAX_WAITERS
#define ASX_NOTIFY_MAX_WAITERS 16u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_notify_handle;

typedef struct {
    uint32_t notify_slot;
    uint32_t waiter_slot;
    uint16_t generation;
} asx_notify_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a new notify. */
ASX_API ASX_MUST_USE asx_status asx_notify_create(asx_notify_handle *out);

/* Close a notify, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_notify_close(asx_notify_handle handle);

/* -------------------------------------------------------------------
 * Signal
 * ------------------------------------------------------------------- */

/* Wake one waiter (FIFO). Returns ASX_OK even if no waiters. */
ASX_API asx_status asx_notify_one(asx_notify_handle handle);

/* Wake all waiters. Returns ASX_OK even if no waiters. */
ASX_API asx_status asx_notify_all(asx_notify_handle handle);

/* -------------------------------------------------------------------
 * Wait
 * ------------------------------------------------------------------- */

/* Register as a waiter. Must be followed by poll_wait calls. */
ASX_API ASX_MUST_USE asx_status asx_notify_wait_begin(asx_notify_handle handle,
                                                      asx_notify_waiter *out);

/* Poll for notification. Returns ASX_OK when notified,
 * ASX_E_PENDING when still waiting, ASX_E_CANCELLED if cx cancelled,
 * ASX_E_DISCONNECTED if notify was closed. */
ASX_API asx_status asx_notify_poll_wait(asx_notify_waiter *waiter, asx_cx *cx);

/* Cancel a wait registration (safe to call even if already notified). */
ASX_API asx_status asx_notify_wait_cancel(asx_notify_waiter *waiter);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of pending waiters. */
ASX_API uint32_t asx_notify_waiter_count(asx_notify_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all notify arena state (test support). */
ASX_API void asx_notify_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_NOTIFY_H */
