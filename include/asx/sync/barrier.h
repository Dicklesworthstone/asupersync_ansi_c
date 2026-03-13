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

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>
#include <asx/asx_config.h>
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

typedef struct {
    uint32_t barrier_slot;
    uint32_t waiter_slot;
    uint16_t generation;
    int      is_leader;   /* set when barrier releases */
} asx_barrier_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a barrier for N tasks. N must be > 0. */
ASX_API ASX_MUST_USE asx_status asx_barrier_create(
    uint32_t count,
    asx_barrier_handle *out);

/* Close a barrier, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_barrier_close(asx_barrier_handle handle);

/* -------------------------------------------------------------------
 * Wait
 * ------------------------------------------------------------------- */

/* Register at the barrier. Must be followed by poll_wait calls. */
ASX_API ASX_MUST_USE asx_status asx_barrier_wait_begin(
    asx_barrier_handle handle,
    asx_barrier_waiter *out);

/* Poll for barrier release. Returns ASX_OK when all N arrived
 * (check waiter->is_leader for leader election),
 * ASX_E_PENDING when still waiting. */
ASX_API asx_status asx_barrier_poll_wait(
    asx_barrier_waiter *waiter,
    asx_cx *cx);

/* Cancel a barrier wait. */
ASX_API asx_status asx_barrier_wait_cancel(asx_barrier_waiter *waiter);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of tasks currently waiting. */
ASX_API uint32_t asx_barrier_waiting_count(asx_barrier_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

ASX_API void asx_barrier_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_BARRIER_H */
