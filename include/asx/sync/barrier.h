/*
 * asx/sync/barrier.h — N-way rendezvous with leader election
 *
 * All N tasks must arrive before any proceed. The last task to
 * arrive is elected leader (returns ASX_OK with is_leader=1).
 * Cancel-safe: cancellation during wait removes the waiter.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_BARRIER_H
#define ASX_SYNC_BARRIER_H

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

#ifndef ASX_BARRIER_MAX
#define ASX_BARRIER_MAX 8u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_barrier_handle;

/* A waiter names a record in the runtime's shared wait-node pool, so a
 * barrier has no waiter limit of its own (any party count can trip it);
 * wait_begin reports ASX_E_RESOURCE_EXHAUSTED only when the whole pool
 * (ASX_WAIT_NODE_CAPACITY) is in use. */
typedef struct {
    uint32_t barrier_slot;
    uint32_t waiter_slot;       /* wait-node index */
    uint16_t generation;        /* the barrier's */
    uint16_t waiter_generation; /* the wait node's */
    int is_leader;              /* set when barrier releases */
} asx_barrier_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a barrier for N tasks. N must be > 0. */
ASX_API ASX_MUST_USE asx_status asx_barrier_create(uint32_t count, asx_barrier_handle *out);

/* Close a barrier, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_barrier_close(asx_barrier_handle handle);

/* -------------------------------------------------------------------
 * Wait
 * ------------------------------------------------------------------- */

/* Create a waiter (Rust's Barrier::wait future). It has not arrived yet:
 * it arrives at its first poll_wait. Must be followed by poll_wait calls. */
ASX_API ASX_MUST_USE asx_status asx_barrier_wait_begin(asx_barrier_handle handle,
                                                       asx_barrier_waiter *out);

/* Poll the waiter, as Rust's BarrierWaitFuture::poll: with a Cx, first
 * its checkpoint, at every poll. A failed checkpoint withdraws the
 * waiter's arrival and returns the checkpoint status, unless its round
 * already tripped: release wins, and the waiter completes with ASX_OK
 * (not as leader) although its Cx is now cancelled. Otherwise the first
 * poll arrives; the arrival that completes N trips the barrier, releases
 * the round and returns ASX_OK with waiter->is_leader set. A released
 * waiter returns ASX_OK; one still waiting ASX_E_PENDING, and inside a
 * scheduler poll its task is parked until the trip (or close) wakes it. */
ASX_API asx_status asx_barrier_poll_wait(asx_barrier_waiter *waiter, asx_cx *cx);

/* Cancel a barrier wait. */
ASX_API asx_status asx_barrier_wait_cancel(asx_barrier_waiter *waiter);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of waiters that arrived in the current round. */
ASX_API uint32_t asx_barrier_waiting_count(asx_barrier_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

/* Reset all barrier arena state (test support). */
ASX_API void asx_barrier_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_BARRIER_H */
