/*
 * asx/core/watch.h — single-value observable state channel
 *
 * A watch channel holds a single uint64_t value that the sender can
 * update at any time. Receivers observe the latest value. When the
 * sender updates, all receivers can detect the change.
 *
 * Semantics:
 *   - Latest-value-wins: old values are overwritten, not queued
 *   - Multiple receivers: each tracks their own "seen" version
 *   - Changed detection: receivers know if value changed since last read
 *
 * Walking skeleton: single-threaded, non-blocking. Wake-driven waiting for
 * changes goes through asx_watch_poll_changed() and asx_watch_poll_wait_for().
 *
 * Operation map (Rust channel/watch.rs at the pinned asupersync rev):
 *   Sender::send            asx_watch_send
 *   Sender::send_modify     asx_watch_send_modify
 *   Sender::subscribe       asx_watch_subscribe
 *   Sender::receiver_count  asx_watch_receiver_count
 *   Sender::is_closed       asx_watch_sender_is_closed
 *   Receiver::changed       asx_watch_changed_begin + asx_watch_poll_changed
 *   Receiver::wait_for      asx_watch_poll_wait_for
 *   Receiver::borrow        asx_watch_borrow (does NOT mark the value seen)
 *   Receiver::borrow_and_update  asx_watch_recv (marks it seen)
 *   Receiver::mark_seen     asx_watch_mark_seen
 *   Receiver::has_changed   asx_watch_has_changed
 *   Receiver::is_closed     asx_watch_receiver_is_closed
 *   Receiver::clone         asx_watch_receiver_clone
 * The values are uint64_t, so Rust's `borrow_and_clone` /
 * `borrow_and_update_clone` are the same calls as `borrow` /
 * `borrow_and_update`, and `mark_changed` does not exist at that rev.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_WATCH_H
#define ASX_CORE_WATCH_H

#include <asx/asx_config.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (ASX_MAX_WATCHES) < 1
#error "ASX_MAX_WATCHES must be at least 1"
#endif
#define ASX_WATCH_MAX_RECEIVERS 8u

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_watch_sender;

typedef struct {
    uint32_t slot;
    uint16_t generation;
    uint64_t last_seen_version; /* version last observed by this receiver */
} asx_watch_receiver;

/* Predicate of asx_watch_poll_wait_for: nonzero accepts `value`. */
typedef int (*asx_watch_predicate_fn)(void *ctx, uint64_t value);

/* Modification of asx_watch_send_modify: Rust's `FnOnce(&mut T)`. */
typedef void (*asx_watch_modify_fn)(void *ctx, uint64_t *value);

/* One in-flight wait_for. Zero-initialise it before the first poll and keep
 * it, untouched, until the poll that ends the wait. */
typedef struct {
    uint64_t checked; /* version of the value the predicate last rejected */
    int waiting;      /* a changed() wait is open on `checked` */
    int cloned;       /* the wait's clone of the receiver exists */
} asx_watch_wait;

/* -------------------------------------------------------------------
 * API: Lifecycle
 * ------------------------------------------------------------------- */

/* Create a watch channel with an initial value.
 * Returns ASX_OK on success.
 * Returns ASX_E_RESOURCE_EXHAUSTED if arena is full. */
ASX_API ASX_MUST_USE asx_status asx_watch_create(uint64_t initial_value,
                                                 asx_watch_sender *out_sender,
                                                 asx_watch_receiver *out_receiver);

/* Subscribe a new receiver to an existing watch.
 * Returns ASX_OK on success.
 * Returns ASX_E_RESOURCE_EXHAUSTED if max receivers reached.
 * Returns ASX_E_DISCONNECTED if sender was dropped. */
ASX_API ASX_MUST_USE asx_status asx_watch_subscribe(const asx_watch_sender *sender,
                                                    asx_watch_receiver *out_receiver);

/* Clone a receiver (Rust `Receiver::clone`, watch.rs:1057). The clone has
 * seen exactly what `receiver` has seen, so it continues from the same
 * version; a receiver from asx_watch_subscribe starts at the current one.
 * Allowed after the sender was dropped.
 * Returns ASX_E_RESOURCE_EXHAUSTED if max receivers reached (a resource
 *   limit; Rust's count is unbounded).
 * Returns ASX_E_INVALID_ARGUMENT for a NULL argument, ASX_E_NOT_FOUND /
 *   ASX_E_STALE_HANDLE for an invalid handle. */
ASX_API ASX_MUST_USE asx_status asx_watch_receiver_clone(const asx_watch_receiver *receiver,
                                                         asx_watch_receiver *out_receiver);

/* Drop the sender. Receivers can still read the last value
 * but has_changed will eventually return 0. */
ASX_API void asx_watch_sender_drop(asx_watch_sender *sender);

/* Drop a receiver. With the sender gone too, the channel's last value is
 * dropped (see asx_watch_set_discard). */
ASX_API void asx_watch_receiver_drop(asx_watch_receiver *receiver);

/* Called once, with the value token, for each value the channel drops. It
 * runs after the operation's wakes and must not use this channel. */
typedef void (*asx_watch_discard_fn)(void *ctx, uint64_t value);

/* Install (or with fn NULL, remove) the channel's discard callback. Rust's
 * watch cell owns its `T`: every send and send_modify drops the value it
 * replaces (watch.rs:497-503, :567-571), and the last value is dropped with
 * the channel; a C value is a uint64_t token, often a pointer, so without
 * this a replaced token leaks. `fn` runs exactly once per such value:
 * on asx_watch_send and asx_watch_send_modify (for the value replaced,
 * including the initial one; sending the token the channel already holds
 * replaces nothing, so it is not called), when the channel is left with no
 * sender and
 * no receiver (for the last value), and on asx_watch_reset (for the value
 * a live channel still holds). A receiver that read a value holds the
 * token copy it was given; releasing it is the application's. With no
 * callback nothing changes; a create clears it.
 * Returns ASX_E_NOT_FOUND / ASX_E_STALE_HANDLE for an invalid handle. */
ASX_API ASX_MUST_USE asx_status asx_watch_set_discard(const asx_watch_sender *sender,
                                                      asx_watch_discard_fn fn, void *ctx);

/* -------------------------------------------------------------------
 * API: Send (update value)
 * ------------------------------------------------------------------- */

/* Update the watched value. Overwrites the previous value.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_STATE if sender was dropped. */
ASX_API ASX_MUST_USE asx_status asx_watch_send(asx_watch_sender *sender, uint64_t value);

/* Modify the watched value in place (Rust `Sender::send_modify`,
 * watch.rs:530): `modify` runs on a copy of the current value, which is then
 * written back as one new version, so receivers see a change even when
 * `modify` leaves the value as it was. `modify` runs before anything is
 * published and must not call this API on the same channel.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_STATE if sender was dropped (Rust `ModifyError`). */
ASX_API ASX_MUST_USE asx_status asx_watch_send_modify(asx_watch_sender *sender,
                                                      asx_watch_modify_fn modify, void *ctx);

/* -------------------------------------------------------------------
 * API: Receive (observe value)
 * ------------------------------------------------------------------- */

/* Read the current value and mark it seen: Rust `borrow_and_update`
 * (watch.rs:917). Always succeeds if receiver is valid.
 * Returns ASX_OK and fills *out_value.
 * Marks the current version as seen by this receiver. */
ASX_API ASX_MUST_USE asx_status asx_watch_recv(asx_watch_receiver *receiver, uint64_t *out_value);

/* Read the current value WITHOUT marking it seen: Rust `borrow`
 * (watch.rs:904). has_changed and the next change wait are unaffected.
 * Returns ASX_OK and fills *out_value. */
ASX_API ASX_MUST_USE asx_status asx_watch_borrow(const asx_watch_receiver *receiver,
                                                 uint64_t *out_value);

/* Mark the current version seen (Rust `mark_seen`, watch.rs:961): the
 * latest published version, not a value read earlier, so an unread send
 * between a borrow and this call is skipped. */
ASX_API ASX_MUST_USE asx_status asx_watch_mark_seen(asx_watch_receiver *receiver);

/* Check if the value has changed since this receiver last read it.
 * Returns 1 if changed, 0 if not (or on error). A pure query: never parks.
 */
ASX_API int asx_watch_has_changed(const asx_watch_receiver *receiver);

/* Rust `Receiver::is_closed` (watch.rs:977): the sender was dropped. A NULL,
 * stale or unknown handle names no live channel and reads as closed. */
ASX_API int asx_watch_receiver_is_closed(const asx_watch_receiver *receiver);

/* Rust `Sender::is_closed` (watch.rs:634): no receiver is left. A NULL,
 * stale or unknown handle reads as closed. */
ASX_API int asx_watch_sender_is_closed(const asx_watch_sender *sender);

/* Start waiting for a change: Rust's `rx.changed(&cx)` records the trace
 * "watch::changed starting wait" when the wait is created, before its
 * first poll (channel/watch.rs:723). Call once per wait, then poll it with
 * asx_watch_poll_changed. A NULL cx records nothing. */
ASX_API void asx_watch_changed_begin(const asx_watch_receiver *receiver, asx_cx *cx);

/* Wait for a change (upstream Receiver::changed). Poll-style.
 * With a Cx, cancellation is checked first: ASX_E_CANCELLED with the trace
 *   "watch::changed cancelled" (watch.rs:736).
 * Returns ASX_OK if a version this receiver has not seen is available and
 *   marks it seen (read it with asx_watch_recv); trace "watch::changed
 *   received update".
 * Returns ASX_E_PENDING if nothing changed; inside a scheduler poll the
 *   calling task is then parked until the next send or the sender drop
 *   (every send wakes every parked receiver task, FIFO).
 * Returns ASX_E_DISCONNECTED if the sender was dropped and no unseen
 *   version remains; trace "watch::changed sender dropped".
 * Traces are recorded for the Cx's task; a NULL cx skips the cancellation
 *   check and the traces.
 * Returns ASX_E_INVALID_ARGUMENT for a NULL receiver, ASX_E_NOT_FOUND /
 *   ASX_E_STALE_HANDLE for invalid handles.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_watch_poll_changed(asx_watch_receiver *receiver, asx_cx *cx);

/* Wait until the value satisfies `predicate` (Rust `Receiver::wait_for`,
 * watch.rs:858). Poll-style: zero-initialise `*wait`, then call until the
 * result is not ASX_E_PENDING, with the same receiver, cx, wait, predicate
 * and ctx each time.
 * The current value is checked first, with no cancellation check, so a
 * value that already matches returns at once and traces nothing.
 * Otherwise the wait runs on a clone of the receiver, as Rust's does
 * (asx_watch_receiver_count counts it until the wait ends), from the
 * version just rejected: each rejection traces "watch::changed starting
 * wait" and each newer version "watch::changed received update", then the
 * predicate sees the value then current.
 * Returns ASX_OK with the accepted value in *out_value, and marks that
 *   version seen by `receiver` (and by nothing else).
 * Returns ASX_E_CANCELLED if cx is cancelled while waiting (trace
 *   "watch::changed cancelled"); `receiver` is unchanged.
 * Returns ASX_E_PENDING while nothing newer arrived; inside a scheduler poll
 *   the calling task is parked until the next send or the sender drop.
 * Returns ASX_E_DISCONNECTED if the sender was dropped and no newer version
 *   remains (trace "watch::changed sender dropped").
 * Returns ASX_E_INVALID_ARGUMENT for a NULL argument, ASX_E_NOT_FOUND /
 *   ASX_E_STALE_HANDLE for invalid handles. */
ASX_API ASX_MUST_USE asx_status asx_watch_poll_wait_for(asx_watch_receiver *receiver, asx_cx *cx,
                                                        asx_watch_wait *wait,
                                                        asx_watch_predicate_fn predicate, void *ctx,
                                                        uint64_t *out_value);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the current receiver count for a watch channel (Rust
 * `Sender::receiver_count`): every live receiver, including the clones that
 * in-flight wait_for calls hold. */
ASX_API uint32_t asx_watch_receiver_count(const asx_watch_sender *sender);

/* Check if the sender is still alive. */
ASX_API int asx_watch_sender_is_alive(uint32_t slot, uint16_t generation);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all watch channel state (test support). */
ASX_API void asx_watch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_WATCH_H */
