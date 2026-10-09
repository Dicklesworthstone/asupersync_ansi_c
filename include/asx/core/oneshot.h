/*
 * asx/core/oneshot.h — single-value, single-use channel
 *
 * A oneshot channel transmits exactly one uint64_t value from sender
 * to receiver. After the value is sent, the sender side is automatically
 * closed. After the value is received, the receiver side is closed.
 *
 * Cancel-safe: dropping the sender without sending produces
 * ASX_E_DISCONNECTED on the receiver side. Dropping the receiver
 * without receiving returns ASX_E_DISCONNECTED on send.
 *
 * Walking skeleton: single-threaded, non-blocking. Wake-driven: a receive
 * that would block inside a scheduler poll parks the calling task until
 * the value is sent or either side is dropped.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_ONESHOT_H
#define ASX_CORE_ONESHOT_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASX_MAX_ONESHOTS
#define ASX_MAX_ONESHOTS 32u
#endif
#if (ASX_MAX_ONESHOTS) < 1
#error "ASX_MAX_ONESHOTS must be at least 1"
#endif

/* -------------------------------------------------------------------
 * Oneshot state
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ONESHOT_EMPTY = 0,           /* created, no value yet */
    ASX_ONESHOT_FILLED = 1,          /* sender has deposited a value */
    ASX_ONESHOT_CONSUMED = 2,        /* receiver has taken the value */
    ASX_ONESHOT_SENDER_DROPPED = 3,  /* sender dropped without sending */
    ASX_ONESHOT_RECEIVER_DROPPED = 4 /* receiver dropped without consuming */
} asx_oneshot_state;

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_oneshot_sender;

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_oneshot_receiver;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Create a oneshot channel pair.
 * Returns ASX_OK on success, filling sender and receiver handles.
 * Returns ASX_E_RESOURCE_EXHAUSTED if arena is full. */
ASX_API ASX_MUST_USE asx_status asx_oneshot_create(asx_oneshot_sender *out_sender,
                                                   asx_oneshot_receiver *out_receiver);

/* Drop the sender without sending a value.
 * Receiver will get ASX_E_DISCONNECTED on try_recv. */
ASX_API void asx_oneshot_sender_drop(asx_oneshot_sender *sender);

/* Drop the receiver without consuming.
 * Sender will get ASX_E_DISCONNECTED on try_send. */
ASX_API void asx_oneshot_receiver_drop(asx_oneshot_receiver *receiver);

/* -------------------------------------------------------------------
 * API: Send / Receive
 * ------------------------------------------------------------------- */

/* Send a value through the oneshot channel. Consumes the sender.
 * Returns ASX_OK on success.
 * Returns ASX_E_DISCONNECTED if receiver was dropped.
 * Returns ASX_E_INVALID_STATE if already sent. */
ASX_API ASX_MUST_USE asx_status asx_oneshot_try_send(asx_oneshot_sender *sender, uint64_t value);

/* Try to receive the value. Non-blocking.
 * Returns ASX_OK and fills *out_value if a value is available.
 * Returns ASX_E_WOULD_BLOCK if sender hasn't sent yet; inside a scheduler
 *   poll the calling task is then parked until the send or a drop.
 * Returns ASX_E_DISCONNECTED if sender dropped without sending. */
ASX_API ASX_MUST_USE asx_status asx_oneshot_try_recv(asx_oneshot_receiver *receiver,
                                                     uint64_t *out_value);

/* -------------------------------------------------------------------
 * API: Cx-aware send / receive (Rust Sender::send, Receiver::recv)
 *
 * With a Cx, these record the user traces Rust's oneshot records
 * (vocabulary §3) for the Cx's task; a NULL cx skips cancellation checks,
 * traces and obligations.
 * ------------------------------------------------------------------- */

/* `tx.send(&cx, v)` (channel/oneshot.rs:556): consumes the sender. A
 * cancelled Cx gives ASX_E_CANCELLED, traces "oneshot::reserve cancelled"
 * and closes the channel (the receiver sees ASX_E_DISCONNECTED). Otherwise
 * traces "oneshot::reserve creating permit", reserves a SendPermit
 * obligation for the Cx's task and delivers: the obligation is committed,
 * or, with the receiver gone, aborted with reason ERROR and the result is
 * ASX_E_DISCONNECTED (:492-535, :731-772). A refused reservation leaves
 * the send untracked. */
ASX_API ASX_MUST_USE asx_status asx_oneshot_send(asx_oneshot_sender *sender, asx_cx *cx,
                                                 uint64_t value);

/* `rx.recv(&cx)` (:1250), one poll. A sent value comes first (ASX_OK,
 * trace "oneshot::recv received value"), then a closed channel
 * (ASX_E_DISCONNECTED, "oneshot::recv channel closed"), then cancellation
 * (ASX_E_CANCELLED, "oneshot::recv cancelled while waiting"; :1084-1180).
 * Otherwise ASX_E_PENDING: inside a scheduler poll the task is parked
 * until the send or a drop. */
ASX_API ASX_MUST_USE asx_status asx_oneshot_recv(asx_oneshot_receiver *receiver, asx_cx *cx,
                                                 uint64_t *out_value);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the current state of the oneshot at the sender's slot.
 * Returns ASX_ONESHOT_EMPTY if state cannot be determined. */
ASX_API asx_oneshot_state asx_oneshot_get_state(uint32_t slot, uint16_t generation);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all oneshot channel state (test support). */
ASX_API void asx_oneshot_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_ONESHOT_H */
