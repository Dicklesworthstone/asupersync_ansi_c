/*
 * asx/sync/rwlock.h — async read-write lock with guard obligations
 *
 * Allows multiple concurrent readers or a single exclusive writer.
 * Writer-preference fairness: when a writer is waiting, new read
 * requests are blocked until the writer completes. Cancel-safe:
 * cancellation during acquisition does not acquire the lock.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_SYNC_RWLOCK_H
#define ASX_SYNC_RWLOCK_H

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

#ifndef ASX_RWLOCK_MAX
#define ASX_RWLOCK_MAX 16u
#endif

#ifndef ASX_RWLOCK_MAX_WAITERS
#define ASX_RWLOCK_MAX_WAITERS 16u
#endif

/* -------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint16_t generation;
} asx_rwlock_handle;

typedef struct {
    uint32_t rw_slot;
    uint16_t generation;
} asx_rwlock_read_guard;

typedef struct {
    uint32_t rw_slot;
    uint16_t generation;
} asx_rwlock_write_guard;

typedef struct {
    uint32_t rw_slot;
    uint32_t waiter_slot;
    uint16_t generation;
    int is_write; /* 1 = write waiter, 0 = read waiter */
} asx_rwlock_waiter;

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Create a read-write lock (initially unlocked). */
ASX_API ASX_MUST_USE asx_status asx_rwlock_create(asx_rwlock_handle *out);

/* Close a rwlock, waking all waiters with ASX_E_DISCONNECTED. */
ASX_API asx_status asx_rwlock_close(asx_rwlock_handle handle);

/* -------------------------------------------------------------------
 * Read lock (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to acquire a read lock immediately. Returns ASX_OK + guard if
 * no writer holds or is waiting, ASX_E_WOULD_BLOCK otherwise. */
ASX_API asx_status asx_rwlock_try_read(asx_rwlock_handle handle, asx_rwlock_read_guard *out);

/* -------------------------------------------------------------------
 * Write lock (non-blocking)
 * ------------------------------------------------------------------- */

/* Try to acquire a write lock immediately. Returns ASX_OK + guard if
 * no readers or writer hold the lock, ASX_E_WOULD_BLOCK otherwise. */
ASX_API asx_status asx_rwlock_try_write(asx_rwlock_handle handle, asx_rwlock_write_guard *out);

/* -------------------------------------------------------------------
 * Read lock (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async read-lock. Must be followed by poll_read calls. */
ASX_API ASX_MUST_USE asx_status asx_rwlock_read_begin(asx_rwlock_handle handle,
                                                       asx_rwlock_waiter *out);

/* Poll for read lock. Returns ASX_OK + guard when acquired,
 * ASX_E_PENDING when waiting. */
ASX_API asx_status asx_rwlock_poll_read(asx_rwlock_waiter *waiter, asx_rwlock_read_guard *out,
                                         asx_cx *cx);

/* -------------------------------------------------------------------
 * Write lock (async / poll-based)
 * ------------------------------------------------------------------- */

/* Begin async write-lock. Must be followed by poll_write calls. */
ASX_API ASX_MUST_USE asx_status asx_rwlock_write_begin(asx_rwlock_handle handle,
                                                        asx_rwlock_waiter *out);

/* Poll for write lock. Returns ASX_OK + guard when acquired,
 * ASX_E_PENDING when waiting. */
ASX_API asx_status asx_rwlock_poll_write(asx_rwlock_waiter *waiter, asx_rwlock_write_guard *out,
                                          asx_cx *cx);

/* -------------------------------------------------------------------
 * Cancel async acquisition
 * ------------------------------------------------------------------- */

/* Cancel an async read or write acquisition (safe even if acquired). */
ASX_API asx_status asx_rwlock_waiter_cancel(asx_rwlock_waiter *waiter);

/* -------------------------------------------------------------------
 * Unlock
 * ------------------------------------------------------------------- */

/* Release a read lock. May wake a waiting writer. */
ASX_API asx_status asx_rwlock_read_unlock(asx_rwlock_read_guard guard);

/* Release a write lock. May wake waiting readers or a writer. */
ASX_API asx_status asx_rwlock_write_unlock(asx_rwlock_write_guard guard);

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

/* Get the number of active readers (0 if write-locked). */
ASX_API uint32_t asx_rwlock_reader_count(asx_rwlock_handle handle);

/* Check if the rwlock is currently write-locked. */
ASX_API int asx_rwlock_is_write_locked(asx_rwlock_handle handle);

/* -------------------------------------------------------------------
 * Arena management
 * ------------------------------------------------------------------- */

ASX_API void asx_rwlock_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_SYNC_RWLOCK_H */
