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

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_notify_handle;

/* A waiter names a record in the runtime's shared wait-node pool, so a
 * notify has no waiter limit of its own (Rust's is unbounded);
 * wait_begin reports ASX_E_RESOURCE_EXHAUSTED only when the whole pool
 * (ASX_WAIT_NODE_CAPACITY) is in use. */
typedef struct {
    uint32_t notify_slot;
    uint32_t waiter_slot;       /* wait-node index */
    uint16_t generation;        /* the notify's */
    uint16_t waiter_generation; /* the wait node's */
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

/* Wake one waiter (FIFO by arrival; cancel-pending waiters are skipped).
 * If no waiter can take it, the notification is stored and the next
 * waiter consumes it immediately; stored notifications accumulate. */
ASX_API asx_status asx_notify_one(asx_notify_handle handle);

/* Wake all current waiters. Stores nothing: waiters that arrive later
 * are not affected. */
ASX_API asx_status asx_notify_all(asx_notify_handle handle);

/* -------------------------------------------------------------------
 * Wait
 * ------------------------------------------------------------------- */

/* Register as a waiter. Must be followed by poll_wait calls. */
ASX_API ASX_MUST_USE asx_status asx_notify_wait_begin(asx_notify_handle handle,
                                                      asx_notify_waiter *out);

/* Poll for notification. Returns ASX_OK when notified,
 * ASX_E_PENDING when still waiting, ASX_E_CANCELLED if cx cancelled,
 * ASX_E_DISCONNECTED if notify was closed. Inside a scheduler poll an
 * ASX_E_PENDING result parks the calling task until it is notified (or
 * the notify is closed). */
ASX_API asx_status asx_notify_poll_wait(asx_notify_waiter *waiter, asx_cx *cx);

/* Cancel a wait registration (safe to call even if already notified). A
 * notify_one notification the waiter had not consumed passes on to the
 * next waiter in arrival order. */
ASX_API asx_status asx_notify_wait_cancel(asx_notify_waiter *waiter);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of pending waiters. */
ASX_API uint32_t asx_notify_waiter_count(asx_notify_handle handle);

/* Get the number of stored notify_one notifications awaiting a waiter. */
ASX_API uint32_t asx_notify_stored_count(asx_notify_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all notify arena state (test support). */
ASX_API void asx_notify_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_NOTIFY_H */
