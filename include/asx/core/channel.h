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
 * CORE builds are deterministic and single-threaded. POSIX/PARALLEL live
 * builds may select the atomic committed-message backend while preserving
 * the same public reserve/send/abort semantics.
 *
 * Wake-driven waiting: when asx_channel_try_recv() reports
 * ASX_E_WOULD_BLOCK or asx_channel_try_reserve() reports
 * ASX_E_CHANNEL_FULL from inside a scheduler poll, the calling task is
 * queued (FIFO, at most ASX_CHANNEL_MAX_WAITERS per direction) and parked
 * (asx_task_park); the caller just propagates ASX_E_PENDING. A commit
 * wakes one parked receiver, a dequeue or abort wakes the oldest parked
 * producer (no one may jump that line), and closing either side wakes
 * every waiter so it observes the new state.
 * Wakes are FIFO and deterministic. Callers outside a scheduler poll are
 * never queued or parked. Parking and waking are scheduler-thread
 * operations: foreign-thread producers of the lock-free backend must not
 * run concurrently with a scheduler task parked on the same channel.
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

#ifndef ASX_MAX_CHANNELS
#define ASX_MAX_CHANNELS 16u
#endif
#ifndef ASX_CHANNEL_MAX_CAPACITY
#define ASX_CHANNEL_MAX_CAPACITY 64u
#endif
#if (ASX_MAX_CHANNELS) < 1 || (ASX_CHANNEL_MAX_CAPACITY) < 1
#error "ASX_MAX_CHANNELS and ASX_CHANNEL_MAX_CAPACITY must be at least 1"
#endif
/* Parked tasks per wait direction (recv / reserve) per channel. Further
 * waiters are not parked: they yield and are re-polled every round. */
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
 * Returns ASX_E_CHANNEL_FULL if capacity exhausted, or if a parked producer
 *   is ahead in line (FIFO: freed capacity belongs to the oldest parked
 *   producer, no queue jumping); inside a scheduler poll the calling task
 *   is then parked until it is at the head of the line with capacity.
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
 * Returns ASX_E_WOULD_BLOCK if queue is empty but channel is open; inside
 *   a scheduler poll the calling task is then parked until a commit or a
 *   close.
 * Returns ASX_E_DISCONNECTED if queue is empty and sender closed. */
ASX_API ASX_MUST_USE asx_status asx_channel_try_recv(asx_channel_id id, uint64_t *out_value);

/* Withdraw `task` from this channel's recv and reserve wait queues — the
 * analog of dropping a Rust recv/reserve future. Call it when a task that
 * parked on this channel stops waiting for it (a select branch lost, a
 * timeout fired); a wake already delivered to it passes to the next
 * waiter in line. Completed or cancel-pending tasks never absorb a wake,
 * so they need not call this.
 * Returns ASX_OK (also when `task` was not queued), or the channel lookup
 *   error (ASX_E_INVALID_ARGUMENT, ASX_E_NOT_FOUND, ASX_E_STALE_HANDLE).
 * Thread-safety: scheduler thread only. */
ASX_API asx_status asx_channel_wait_cancel(asx_channel_id id, asx_task_id task);

/* ------------------------------------------------------------------ */
/* Reset (test support)                                               */
/* ------------------------------------------------------------------ */

/* Reset all channel state. For tests only. */
ASX_API void asx_channel_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_CHANNEL_H */
