/*
 * asx/core/broadcast.h — bounded broadcast channel
 *
 * A broadcast channel delivers each sent message to all active
 * receivers. Uses a shared ring buffer with per-receiver read cursors.
 *
 * Semantics:
 *   - Single sender, multiple receivers
 *   - FIFO ordering per receiver
 *   - Bounded capacity: a send never fails for space; it overwrites the
 *     oldest message
 *   - Lagging receivers: when a receiver falls behind by more than capacity,
 *     its next recv returns ASX_E_LAGGED and the cursor advances to the
 *     oldest available message
 *
 * Walking skeleton: single-threaded, non-blocking.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_BROADCAST_H
#define ASX_CORE_BROADCAST_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_MAX_BROADCASTS
#define ASX_MAX_BROADCASTS 8u
#endif
#ifndef ASX_BROADCAST_MAX_CAPACITY
#define ASX_BROADCAST_MAX_CAPACITY 32u
#endif
#if (ASX_MAX_BROADCASTS) < 1 || (ASX_BROADCAST_MAX_CAPACITY) < 1
#error "ASX_MAX_BROADCASTS and ASX_BROADCAST_MAX_CAPACITY must be at least 1"
#endif
#define ASX_BROADCAST_MAX_RECEIVERS 8u

/* -------------------------------------------------------------------
 * Lag error code — added to status system
 * ------------------------------------------------------------------- */

/* ASX_E_LAGGED = 704: receiver fell behind, messages were lost */

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_broadcast_sender;

typedef struct {
    uint32_t slot;
    uint16_t generation;
    uint32_t cursor; /* next message sequence to read */
} asx_broadcast_receiver;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Create a broadcast channel with the given capacity.
 * capacity must be > 0 and <= ASX_BROADCAST_MAX_CAPACITY.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_create(uint32_t capacity,
                                                     asx_broadcast_sender *out_sender,
                                                     asx_broadcast_receiver *out_receiver);

/* Subscribe a new receiver to a broadcast.
 * New receivers start from the next message (no backfill).
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_subscribe(const asx_broadcast_sender *sender,
                                                        asx_broadcast_receiver *out_receiver);

/* Drop the sender. Receivers can drain remaining messages. */
ASX_API void asx_broadcast_sender_drop(asx_broadcast_sender *sender);

/* Drop a receiver. */
ASX_API void asx_broadcast_receiver_drop(asx_broadcast_receiver *receiver);

/* -------------------------------------------------------------------
 * API: Send
 * ------------------------------------------------------------------- */

/* Send a value to all receivers (Rust `tx.send(&cx, v)`,
 * channel/broadcast.rs:494). The ring never refuses a send: the oldest
 * message is overwritten and a receiver that falls behind sees
 * ASX_E_LAGGED. Cancellation is checked first: a cancelled Cx gives
 * ASX_E_CANCELLED (no trace, :418). With no live receiver the send fails
 * with ASX_E_DISCONNECTED (Rust Closed, :423). Otherwise a Cx with a task
 * reserves a SendPermit obligation that the delivery commits (:430, :719).
 * A NULL cx skips the cancellation check and the obligation.
 * Returns ASX_E_INVALID_STATE if the sender was dropped. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_send(asx_broadcast_sender *sender, asx_cx *cx,
                                                   uint64_t value);

/* -------------------------------------------------------------------
 * API: Receive
 * ------------------------------------------------------------------- */

/* Try to receive the next message for this receiver. Never parks (Rust
 *   try_recv); to wait use asx_broadcast_recv, whose parked tasks every
 *   send or the sender drop wakes (FIFO).
 * Returns ASX_OK and fills *out_value on success.
 * Returns ASX_E_WOULD_BLOCK if no new messages.
 * Returns ASX_E_DISCONNECTED if sender dropped and no messages remain.
 * Returns ASX_E_LAGGED if this receiver fell behind — *out_value receives
 *   the number of messages it missed (Rust Lagged(n)) and the cursor is
 *   advanced to the oldest available message (call recv again). */
ASX_API ASX_MUST_USE asx_status asx_broadcast_try_recv(asx_broadcast_receiver *receiver,
                                                       uint64_t *out_value);

/* `rx.recv(&cx)` (broadcast.rs:894), one poll. Cancellation is checked
 * first: ASX_E_CANCELLED with the trace "broadcast::recv cancelled"
 * (:919) for the Cx's task. Otherwise as asx_broadcast_try_recv, with
 * ASX_E_PENDING for no new message. A NULL cx skips the check. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_recv(asx_broadcast_receiver *receiver, asx_cx *cx,
                                                   uint64_t *out_value);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the number of active receivers. */
ASX_API uint32_t asx_broadcast_receiver_count(const asx_broadcast_sender *sender);

/* Get the total number of messages sent (monotonic sequence). */
ASX_API uint32_t asx_broadcast_total_sent(const asx_broadcast_sender *sender);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all broadcast channel state (test support). */
ASX_API void asx_broadcast_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_BROADCAST_H */
