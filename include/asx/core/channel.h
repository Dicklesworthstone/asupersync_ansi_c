/*
 * asx/core/channel.h — bounded MPSC two-phase channel
 *
 * Provides a bounded, multi-producer single-consumer channel with a
 * two-phase send protocol: reserve → send/abort. The reserve step
 * claims capacity; send commits the value to the queue; abort returns
 * the capacity without enqueuing.
 *
 * Messages are uint64_t tokens (opaque to the channel). FIFO ordering
 * is guaranteed for committed messages.
 *
 * Walking skeleton: single-threaded, non-blocking (try_reserve/try_recv).
 * Blocking variants will be added when the scheduler gains channel
 * awareness (future phase).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_CHANNEL_H
#define ASX_CORE_CHANNEL_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Capacity limits (walking skeleton: fixed-size arenas)              */
/* ------------------------------------------------------------------ */

#define ASX_MAX_CHANNELS 16u
#define ASX_CHANNEL_MAX_CAPACITY 64u
#define ASX_CHANNEL_MAX_WAITERS 32u

/* ------------------------------------------------------------------ */
/* Channel lifecycle states                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_CHANNEL_OPEN = 0,
    ASX_CHANNEL_SENDER_CLOSED = 1,
    ASX_CHANNEL_RECEIVER_CLOSED = 2,
    ASX_CHANNEL_FULLY_CLOSED = 3
} asx_channel_state;

/* ------------------------------------------------------------------ */
/* Send permit (two-phase protocol token)                             */
/* ------------------------------------------------------------------ */

typedef struct asx_send_permit {
    asx_channel_id channel_id;
    uint32_t token; /* monotonic token for linearity check */
    int consumed;   /* 1 if already sent or aborted */
} asx_send_permit;

/* ------------------------------------------------------------------ */
/* Channel lifecycle API                                              */
/* ------------------------------------------------------------------ */

/* Create a bounded channel within a region.
 * capacity must be > 0 and <= ASX_CHANNEL_MAX_CAPACITY.
 * Returns ASX_OK and sets *out_id on success. */
ASX_API ASX_MUST_USE asx_status asx_channel_create(asx_region_id region, uint32_t capacity,
                                                   asx_channel_id *out_id);

/* Close the sender side. No new reserves will succeed.
 * Pending messages remain available for recv.
 * Open → SenderClosed; ReceiverClosed → FullyClosed. */
ASX_API ASX_MUST_USE asx_status asx_channel_close_sender(asx_channel_id id);

/* Close the receiver side. Pending messages are discarded.
 * Future sends via permits return ASX_E_DISCONNECTED.
 * Open → ReceiverClosed; SenderClosed → FullyClosed. */
ASX_API ASX_MUST_USE asx_status asx_channel_close_receiver(asx_channel_id id);

/* ------------------------------------------------------------------ */
/* Channel query API                                                  */
/* ------------------------------------------------------------------ */

/* Query the current state of a channel. */
ASX_API ASX_MUST_USE asx_status asx_channel_get_state(asx_channel_id id, asx_channel_state *out);

/* Number of committed messages waiting in the queue. */
ASX_API ASX_MUST_USE asx_status asx_channel_queue_len(asx_channel_id id, uint32_t *out);

/* Number of outstanding reservations (capacity claimed but not sent). */
ASX_API ASX_MUST_USE asx_status asx_channel_reserved_count(asx_channel_id id, uint32_t *out);

/* ------------------------------------------------------------------ */
/* Two-phase send protocol                                            */
/* ------------------------------------------------------------------ */

/* Try to reserve a send slot. Non-blocking.
 * Returns ASX_OK and fills *out_permit on success.
 * Returns ASX_E_CHANNEL_FULL if capacity exhausted.
 * Returns ASX_E_DISCONNECTED if receiver closed.
 * Returns ASX_E_INVALID_STATE if sender side closed. */
ASX_API ASX_MUST_USE asx_status asx_channel_try_reserve(asx_channel_id id, asx_send_permit *out);

/* Commit a reserved permit by sending a value.
 * Consumes the permit. The value is enqueued FIFO.
 * Returns ASX_E_DISCONNECTED if receiver closed (value NOT enqueued). */
ASX_API ASX_MUST_USE asx_status asx_send_permit_send(asx_send_permit *permit, uint64_t value);

/* Abort a reserved permit without sending.
 * Returns the capacity to the pool. */
ASX_API void asx_send_permit_abort(asx_send_permit *permit);

/* ------------------------------------------------------------------ */
/* Receive API                                                        */
/* ------------------------------------------------------------------ */

/* Try to receive a message. Non-blocking.
 * Returns ASX_OK and fills *out_value on success.
 * Returns ASX_E_WOULD_BLOCK if queue is empty but channel is open.
 * Returns ASX_E_DISCONNECTED if queue is empty and sender closed. */
ASX_API ASX_MUST_USE asx_status asx_channel_try_recv(asx_channel_id id, uint64_t *out_value);

/* ------------------------------------------------------------------ */
/* Reset (test support)                                               */
/* ------------------------------------------------------------------ */

/* Reset all channel state. For tests only. */
ASX_API void asx_channel_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_CHANNEL_H */
