/*
 * asx/core/broadcast.h — bounded broadcast channel
 *
 * A broadcast channel delivers each sent message to all active
 * receivers. Uses a shared ring buffer with per-receiver read cursors.
 *
 * Semantics:
 *   - Single sender, multiple receivers
 *   - FIFO ordering per receiver
 *   - Bounded capacity: send fails with ASX_E_CHANNEL_FULL when buffer full
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

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_BROADCASTS 8u
#define ASX_BROADCAST_MAX_CAPACITY 32u
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

/* Send a value to all receivers.
 * Returns ASX_OK on success.
 * Returns ASX_E_CHANNEL_FULL if buffer is full (all receiver cursors
 *   must advance before more sends are possible).
 * Returns ASX_E_INVALID_STATE if sender was dropped. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_send(asx_broadcast_sender *sender, uint64_t value);

/* -------------------------------------------------------------------
 * API: Receive
 * ------------------------------------------------------------------- */

/* Try to receive the next message for this receiver.
 * Returns ASX_OK and fills *out_value on success.
 * Returns ASX_E_WOULD_BLOCK if no new messages.
 * Returns ASX_E_DISCONNECTED if sender dropped and no messages remain.
 * Returns ASX_E_LAGGED if this receiver fell behind — cursor is
 *   advanced to oldest available message (call recv again). */
ASX_API ASX_MUST_USE asx_status asx_broadcast_try_recv(asx_broadcast_receiver *receiver,
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
