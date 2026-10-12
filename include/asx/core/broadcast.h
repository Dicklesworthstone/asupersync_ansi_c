/*
 * asx/core/broadcast.h — bounded broadcast channel
 *
 * A broadcast channel delivers each sent message to all active
 * receivers. Uses a shared ring buffer with per-receiver read cursors.
 *
 * Semantics (Rust channel/broadcast.rs):
 *   - Counted senders and receivers: asx_broadcast_sender_clone and
 *     asx_broadcast_receiver_clone add one, the drops remove one, as
 *     Rust's Sender / Receiver clones (:577-608, :1073-1105). Each handle
 *     is one endpoint: copying the struct does not clone it, and a dropped
 *     handle is refused afterwards. The receivers see the channel closed
 *     once the last sender is dropped and they have read everything.
 *   - FIFO ordering per receiver; message indices are 64-bit (:198)
 *   - Bounded capacity: a send never fails for space; it overwrites the
 *     oldest message
 *   - Lagging receivers: when a receiver falls behind by more than capacity,
 *     its next recv returns ASX_E_LAGGED with the number missed and the
 *     cursor advances to the oldest available message
 *
 * Single-threaded (scheduler thread).
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
    int dropped; /* 1 once asx_broadcast_sender_drop released this sender */
} asx_broadcast_sender;

typedef struct {
    uint32_t slot;
    uint16_t generation;
    int dropped;     /* 1 once asx_broadcast_receiver_drop released it */
    uint64_t cursor; /* index of the next message to read */
} asx_broadcast_receiver;

/* A reserved send (Rust broadcast SendPermit, :610). It holds no ring
 * space (a broadcast never waits for room), only the SendPermit
 * obligation of the reserving Cx's task, if any. */
typedef struct {
    uint32_t slot;
    uint16_t generation;
    int consumed;                 /* 1 once sent or aborted */
    asx_obligation_id obligation; /* ASX_INVALID_ID when untracked */
    uint32_t serial;              /* authenticates this individual reserved permit */
} asx_broadcast_permit;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Create a broadcast channel with the given capacity.
 * capacity must be > 0 and <= ASX_BROADCAST_MAX_CAPACITY.
 * Returns ASX_OK on success. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_create(uint32_t capacity,
                                                     asx_broadcast_sender *out_sender,
                                                     asx_broadcast_receiver *out_receiver);

/* Subscribe a new receiver to a broadcast (Rust Sender::subscribe, :556).
 * New receivers start from the next message (no backfill).
 * Returns ASX_OK on success, ASX_E_INVALID_STATE for a dropped sender,
 * ASX_E_RESOURCE_EXHAUSTED at ASX_BROADCAST_MAX_RECEIVERS. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_subscribe(const asx_broadcast_sender *sender,
                                                        asx_broadcast_receiver *out_receiver);

/* Clone a sender (Rust Sender::clone, :577): one more live sender.
 * Returns ASX_E_INVALID_STATE for a dropped one. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_sender_clone(const asx_broadcast_sender *sender,
                                                           asx_broadcast_sender *out_sender);

/* Clone a receiver (Rust Receiver::clone, :1073): a new receiver at the
 * same cursor, which reads the same remaining messages. Returns
 * ASX_E_INVALID_STATE for a dropped one, ASX_E_RESOURCE_EXHAUSTED at
 * ASX_BROADCAST_MAX_RECEIVERS. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_receiver_clone(const asx_broadcast_receiver *receiver,
                                                             asx_broadcast_receiver *out_receiver);

/* Drop a sender (Rust Sender::drop, :586). The last one closes the
 * channel and wakes the parked receivers, which drain what remains and
 * then see ASX_E_DISCONNECTED. A second drop of the same handle does
 * nothing. */
ASX_API void asx_broadcast_sender_drop(asx_broadcast_sender *sender);

/* Drop a receiver (Rust Receiver::drop, :1089). With none left, sends
 * report ASX_E_DISCONNECTED. A second drop of the same handle does
 * nothing. */
ASX_API void asx_broadcast_receiver_drop(asx_broadcast_receiver *receiver);

/* -------------------------------------------------------------------
 * API: Send
 * ------------------------------------------------------------------- */

/* Reserve a send (Rust `tx.reserve(&cx)`, channel/broadcast.rs:409).
 * Never waits: a broadcast overwrites instead of filling up.
 * Cancellation is checked first: a cancelled Cx gives ASX_E_CANCELLED (no
 * trace, :418). With no live receiver it fails with ASX_E_DISCONNECTED
 * (Rust Closed, :423). Otherwise a Cx with a task registers a SendPermit
 * obligation (:430), committed by asx_broadcast_permit_send and aborted
 * with Cancel by asx_broadcast_permit_abort or when the task completes
 * still holding it (the permit's drop, :624). A NULL cx skips the
 * cancellation check and the obligation.
 * Returns ASX_E_INVALID_STATE for a dropped sender and
 * ASX_E_RESOURCE_EXHAUSTED when the process-wide permit serial is exhausted
 * or all ASX_MAX_OBLIGATIONS physical broadcast credits are outstanding.
 * Physical credits also authenticate permits whose registration was refused;
 * a credit reserved with a task is dropped when that task completes. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_reserve(const asx_broadcast_sender *sender,
                                                      asx_cx *cx, asx_broadcast_permit *out_permit);

/* Send through a permit (Rust SendPermit::send, :709): commits its
 * obligation, then publishes `value` to the receivers live now and sets
 * *out_receivers (may be NULL) to their number. If every receiver dropped
 * after the reserve, nothing is published and the count is 0 (still
 * ASX_OK: the permit was honoured). Returns ASX_E_INVALID_STATE for a
 * consumed permit, or when the last sender dropped since the reserve (a
 * Rust permit borrows its sender, so C refuses that and aborts the
 * obligation with Cancel). A genuine permit may move to another caller;
 * changing its tracked resource binding is refused before consumption or
 * publication. A retained copy whose credit was sent, aborted or dropped
 * with its holder returns ASX_E_OBLIGATION_ALREADY_RESOLVED without publishing.
 * Raw obligation IDs cannot settle tracked permits. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_permit_send(asx_broadcast_permit *permit,
                                                          uint64_t value, uint32_t *out_receivers);

/* Give a permit up unsent: its obligation is aborted with Cancel, as the
 * Rust permit's drop does. A consumed permit is left alone. */
ASX_API void asx_broadcast_permit_abort(asx_broadcast_permit *permit);

/* Send a value to all receivers (Rust `tx.send(&cx, v)`, :494): reserve,
 * then send through the permit. Statuses as the two; *out_receivers (may
 * be NULL) is the number of receivers that were live at the publication. */
ASX_API ASX_MUST_USE asx_status asx_broadcast_send(const asx_broadcast_sender *sender, asx_cx *cx,
                                                   uint64_t value, uint32_t *out_receivers);

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

/* Get the number of active receivers (Rust Sender::receiver_count, :528). */
ASX_API uint32_t asx_broadcast_receiver_count(const asx_broadcast_sender *sender);

/* Get the total number of messages published (monotonic, 64-bit). */
ASX_API uint64_t asx_broadcast_total_sent(const asx_broadcast_sender *sender);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all broadcast channel state (test support). */
ASX_API void asx_broadcast_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_BROADCAST_H */
