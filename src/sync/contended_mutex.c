/*
 * contended_mutex.c — instrumented mutex with contention tracking
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/sync/contended_mutex.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    asx_mutex_handle inner;
    uint64_t acquisitions;
    uint64_t contentions;
    uint64_t current_run; /* current consecutive contention streak */
    uint64_t max_contention_run;
} cm_slot;

static cm_slot g_slots[ASX_CONTENDED_MUTEX_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static cm_slot *slot_lookup(uint32_t idx, uint16_t gen) {
    cm_slot *s;
    if (idx >= ASX_CONTENDED_MUTEX_MAX) return NULL;
    s = &g_slots[idx];
    if (!s->alive || s->generation != gen) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_create(asx_contended_mutex_handle *out) {
    uint32_t i;
    asx_status st;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_CONTENDED_MUTEX_MAX; i++) {
        if (!g_slots[i].alive) {
            st = asx_mutex_create(&g_slots[i].inner);
            if (st != ASX_OK) return st;

            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            g_slots[i].acquisitions = 0;
            g_slots[i].contentions = 0;
            g_slots[i].current_run = 0;
            g_slots[i].max_contention_run = 0;
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_contended_mutex_close(asx_contended_mutex_handle handle) {
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    asx_mutex_close(s->inner);
    s->alive = 0;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lock (non-blocking, instrumented)                                   */
/* ------------------------------------------------------------------ */

asx_status asx_contended_mutex_try_lock(asx_contended_mutex_handle handle, asx_mutex_guard *out) {
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
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
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    return asx_mutex_lock_begin(s->inner, out);
}

asx_status asx_contended_mutex_poll_lock(asx_contended_mutex_handle handle,
                                         asx_mutex_lock_waiter *waiter, asx_mutex_guard *out,
                                         asx_cx *cx) {
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
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
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return ASX_E_STALE_HANDLE;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    out->acquisitions = s->acquisitions;
    out->contentions = s->contentions;
    out->max_contention_run = s->max_contention_run;
    return ASX_OK;
}

asx_status asx_contended_mutex_reset_metrics(asx_contended_mutex_handle handle) {
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
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
    cm_slot *s = slot_lookup(handle.slot, handle.generation);
    if (s == NULL) return 0;
    return asx_mutex_is_locked(s->inner);
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_contended_mutex_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        if (g_slots[i].alive) { asx_mutex_close(g_slots[i].inner); }
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        g_slots[i].acquisitions = 0;
        g_slots[i].contentions = 0;
        g_slots[i].current_run = 0;
        g_slots[i].max_contention_run = 0;
    }
    g_slot_count = 0;
}
