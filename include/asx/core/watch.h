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
 * Walking skeleton: single-threaded, non-blocking.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CORE_WATCH_H
#define ASX_CORE_WATCH_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASX_MAX_WATCHES 16u
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
    uint32_t last_seen_version; /* version last observed by this receiver */
} asx_watch_receiver;

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

/* Drop the sender. Receivers can still read the last value
 * but has_changed will eventually return 0. */
ASX_API void asx_watch_sender_drop(asx_watch_sender *sender);

/* Drop a receiver. */
ASX_API void asx_watch_receiver_drop(asx_watch_receiver *receiver);

/* -------------------------------------------------------------------
 * API: Send (update value)
 * ------------------------------------------------------------------- */

/* Update the watched value. Overwrites the previous value.
 * Returns ASX_OK on success.
 * Returns ASX_E_INVALID_STATE if sender was dropped. */
ASX_API ASX_MUST_USE asx_status asx_watch_send(asx_watch_sender *sender, uint64_t value);

/* -------------------------------------------------------------------
 * API: Receive (observe value)
 * ------------------------------------------------------------------- */

/* Read the current value. Always succeeds if receiver is valid.
 * Returns ASX_OK and fills *out_value.
 * Marks the current version as seen by this receiver. */
ASX_API ASX_MUST_USE asx_status asx_watch_recv(asx_watch_receiver *receiver, uint64_t *out_value);

/* Check if the value has changed since this receiver last read it.
 * Returns 1 if changed, 0 if not (or on error). */
ASX_API int asx_watch_has_changed(const asx_watch_receiver *receiver);

/* -------------------------------------------------------------------
 * API: Query
 * ------------------------------------------------------------------- */

/* Get the current receiver count for a watch channel. */
ASX_API uint32_t asx_watch_receiver_count(const asx_watch_sender *sender);

/* Check if the sender is still alive. */
ASX_API int asx_watch_sender_is_alive(uint32_t slot, uint16_t generation);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

ASX_API void asx_watch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_CORE_WATCH_H */
