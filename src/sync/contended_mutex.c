/*
 * contended_mutex.c — instrumented mutex with contention tracking
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include <asx/runtime/runtime.h>
#include <asx/sync/contended_mutex.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/* The arenas are the kernels' (asx_kernel.cmutexes, sync_state.h): the
 * slot field of a handle holds the index in its low half and the tag of
 * the runtime that issued it above; the inner mutex is that runtime's too. */

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

/* The live slot a slot field and generation name, NULL when no runtime
 * holds the tag, the index is out of range or the handle is stale. */
static asx_cm_slot *slot_lookup(uint32_t field, uint16_t gen) {
    asx_kernel *k = asx_kernel_for_tag((uint16_t)(field >> ASX_SYNC_SLOT_TAG_SHIFT));
    uint32_t idx = field & ASX_SYNC_SLOT_INDEX_MASK;
    asx_cm_slot *s;
    if (k == NULL || idx >= ASX_CONTENDED_MUTEX_MAX) return NULL;
    s = &k->cmutexes.slots[idx];
    if (!s->alive || s->generation != gen) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_create(const asx_runtime *rt, asx_contended_mutex_handle *out) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    uint32_t i;
    asx_status st;
    if (out == NULL || rt == NULL) return ASX_E_INVALID_ARGUMENT;
    if (k == NULL) return ASX_E_INVALID_STATE;

    for (i = 0; i < ASX_CONTENDED_MUTEX_MAX; i++) {
        asx_cm_slot *s = &k->cmutexes.slots[i];
        if (!s->alive) {
            st = asx_semaphore_create_untracked(k, 1u, &s->inner.sem);
            if (st != ASX_OK) return st;

            s->alive = 1;
            s->generation = next_gen(s->generation);
            s->acquisitions = 0;
            s->contentions = 0;
            s->current_run = 0;
            s->max_contention_run = 0;
            out->slot = ((uint32_t)k->tag << ASX_SYNC_SLOT_TAG_SHIFT) | i;
            out->generation = s->generation;
            if (i >= k->cmutexes.slot_count) k->cmutexes.slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_contended_mutex_close(asx_contended_mutex_handle handle) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    asx_mutex_close(s->inner);
    s->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lock (non-blocking, instrumented)                                   */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_try_lock(asx_contended_mutex_handle handle, asx_mutex_guard *out) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    asx_status st;
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    st = asx_mutex_try_lock(s->inner, out);
    if (st == ASX_OK) {
        s->acquisitions++;
        s->current_run = 0; /* reset contention streak */
    } else if (st == ASX_E_WOULD_BLOCK) {
        s->contentions++;
        s->current_run++;
        if (s->current_run > s->max_contention_run) { s->max_contention_run = s->current_run; }
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Unlock (delegates to mutex)                                         */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_unlock(asx_mutex_guard guard) { return asx_mutex_unlock(guard); }

/* ------------------------------------------------------------------ */
/* Async lock (instrumented)                                           */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_lock_begin(asx_contended_mutex_handle handle,
                                          asx_mutex_lock_waiter *out) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    return asx_mutex_lock_begin(s->inner, out);
}

asx_status asx_contended_mutex_poll_lock(asx_contended_mutex_handle handle,
                                         asx_mutex_lock_waiter *waiter, asx_mutex_guard *out,
                                         asx_cx *cx) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    asx_status st;
    if (s == NULL) return ASX_E_STALE_HANDLE;

    st = asx_mutex_poll_lock(waiter, out, cx);
    if (st == ASX_OK) {
        s->acquisitions++;
        s->current_run = 0;
    } else if (st == ASX_E_PENDING) {
        s->contentions++;
        s->current_run++;
        if (s->current_run > s->max_contention_run) { s->max_contention_run = s->current_run; }
    }
    return st;
}

asx_status asx_contended_mutex_lock_cancel(asx_mutex_lock_waiter *waiter) {
    return asx_mutex_lock_cancel(waiter);
}

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_metrics(asx_contended_mutex_handle handle,
                                       asx_lock_metrics_snapshot *out) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    out->acquisitions = s->acquisitions;
    out->contentions = s->contentions;
    out->max_contention_run = s->max_contention_run;
    return ASX_OK;
}

asx_status asx_contended_mutex_reset_metrics(asx_contended_mutex_handle handle) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;

    s->acquisitions = 0;
    s->contentions = 0;
    s->current_run = 0;
    s->max_contention_run = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

int asx_contended_mutex_is_locked(asx_contended_mutex_handle handle) {
    asx_cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return 0;
    return asx_mutex_is_locked(s->inner);
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_contended_mutex_reset_internal(asx_kernel *k) {
    asx_cm_state *st = &k->cmutexes;
    uint32_t i;
    for (i = 0; i < st->slot_count; i++) {
        asx_cm_slot *s = &st->slots[i];
        if (s->alive) { asx_mutex_close(s->inner); }
        s->generation = next_gen(s->generation);
        s->alive = 0;
        s->acquisitions = 0;
        s->contentions = 0;
        s->current_run = 0;
        s->max_contention_run = 0;
    }
    st->slot_count = 0;
}

void asx_contended_mutex_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_contended_mutex_reset_internal(k);
}
