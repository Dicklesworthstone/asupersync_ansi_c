/*
 * once.c — compute-once cell
 *
 * Asynchronous initializers: an init_fn may report ASX_E_PENDING (it waits
 * for something, e.g. a channel). When that happens inside a scheduler
 * poll the calling task becomes the cell's initializer and keeps calling
 * get_or_init until init_fn settles. Other tasks calling get_or_init
 * meanwhile do not run init_fn again: they queue (FIFO) and park, and are
 * woken when the initializer finishes — all of them on success, the oldest
 * one on failure so it can retry (a failed init leaves the cell
 * uninitialized, as before).
 *
 * Cancel safety: an initializer that is cancelled or completes without
 * finishing is detected lazily (the next caller takes over), and
 * asx_once_wait_cancel() lets an initializer abandon explicitly, or a
 * parked waiter stop waiting, passing the turn to the next waiter.
 * Outside a scheduler poll get_or_init keeps its synchronous semantics.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include "wait_queue.h"
#include <asx/runtime/runtime.h>
#include <asx/sync/once.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/* The arenas are the kernels' (asx_kernel.onces, sync_state.h): the slot
 * field of a handle holds the index in its low half and the tag of the
 * runtime that issued it above. NULL when no runtime holds the tag or the
 * index is out of range. */
static asx_once_slot *once_slot_at(uint32_t field) {
    asx_kernel *k = asx_kernel_for_tag((uint16_t)(field >> ASX_SYNC_SLOT_TAG_SHIFT));
    uint32_t idx = field & ASX_SYNC_SLOT_INDEX_MASK;
    if (k == NULL || idx >= ASX_ONCE_MAX) return NULL;
    return &k->onces.slots[idx];
}

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void once_reap_queue(asx_wait_queue *q);

static void once_slot_clear(asx_once_slot *s, uint16_t tag) {
    s->initialized = 0;
    s->value = 0;
    s->initializer = ASX_INVALID_ID;
    asx_wait_queue_init(&s->waiters, once_reap_queue, tag);
}

/* The initialization in progress was abandoned or failed: the cell stays
 * uninitialized and the oldest waiter gets the next turn. */
static void once_pass_turn(asx_once_slot *s) {
    s->initializer = ASX_INVALID_ID;
    if (s->waiters.len > 0u) (void)asx_wait_queue_settle(&s->waiters, 1u);
}

/* A waiter's task finished while still queued: drop the dead entry and,
 * while no initialization is in flight, hand the turn it may have held to
 * the next waiter. */
static void once_reap_queue(asx_wait_queue *q) {
    asx_kernel *k = asx_kernel_for_tag(q->tag);
    uint32_t i;
    if (k == NULL) return;
    for (i = 0; i < ASX_ONCE_MAX; i++) {
        asx_once_slot *s = &k->onces.slots[i];
        ASX_CHECKPOINT_WAIVER("bounded: ASX_ONCE_MAX constant");
        if (q != &s->waiters) continue;
        asx_wait_queue_drop_dead(q);
        if (!s->initialized && !asx_handle_is_valid(s->initializer)) once_pass_turn(s);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_once_create(const asx_runtime *rt, asx_once_handle *out) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    uint32_t i;
    if (out == NULL || rt == NULL) return ASX_E_INVALID_ARGUMENT;
    if (k == NULL) return ASX_E_INVALID_STATE;

    for (i = 0; i < ASX_ONCE_MAX; i++) {
        asx_once_slot *s = &k->onces.slots[i];
        if (!s->alive) {
            s->alive = 1;
            s->generation = next_gen(s->generation);
            once_slot_clear(s, k->tag);
            out->slot = ((uint32_t)k->tag << ASX_SYNC_SLOT_TAG_SHIFT) | i;
            out->generation = s->generation;
            if (i >= k->onces.slot_count) k->onces.slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_once_close(asx_once_handle handle) {
    asx_once_slot *s = once_slot_at(handle.slot);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    s->alive = 0;
    /* Parked waiters re-poll and observe the stale handle; they never come
     * back to this queue, so its nodes return to the pool. */
    if (s->waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->waiters);
    asx_wait_queue_clear(&s->waiters);
    s->initializer = ASX_INVALID_ID;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Access                                                              */
/* ------------------------------------------------------------------ */

asx_status asx_once_get_or_init(asx_once_handle handle, asx_once_init_fn init_fn, void *user_data,
                                uint64_t *out_value) {
    asx_once_slot *s = once_slot_at(handle.slot);
    asx_status st;
    asx_task_id self;

    if (init_fn == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    if (s->initialized) {
        asx_wait_queue_leave_current(&s->waiters);
        *out_value = s->value;
        return ASX_OK;
    }

    self = asx_task_current();

    /* Another task's initialization is in flight: wait for it, unless its
     * task is gone or being cancelled — then the turn passes to us. */
    if (asx_handle_is_valid(s->initializer) && !asx_wait_same_task(s->initializer, self)) {
        if (asx_wait_task_liveness(s->initializer) == ASX_WAIT_TASK_LIVE) {
            (void)asx_wait_queue_park_current(&s->waiters);
            return ASX_E_PENDING;
        }
        s->initializer = ASX_INVALID_ID;
    }

    /* Initialize */
    st = init_fn(user_data, &s->value);
    if (st == ASX_OK) {
        s->initialized = 1;
        s->initializer = ASX_INVALID_ID;
        asx_wait_queue_leave_current(&s->waiters);
        if (s->waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->waiters);
        *out_value = s->value;
        return ASX_OK;
    }

    if (st == ASX_E_PENDING && self != ASX_INVALID_ID) {
        /* Asynchronous initializer: this task owns the cell until init_fn
         * settles. init_fn parks the task on whatever it waits for. */
        s->initializer = self;
        asx_wait_queue_leave_current(&s->waiters);
        return ASX_E_PENDING;
    }

    /* Failed: the cell stays uninitialized and the next caller retries. */
    asx_wait_queue_leave_current(&s->waiters);
    once_pass_turn(s);
    return st;
}

asx_status asx_once_wait_cancel(asx_once_handle handle, asx_task_id task) {
    asx_once_slot *s = once_slot_at(handle.slot);
    int abandoned_init;
    int left_queue;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    /* An abandoned initialization leaves the cell uninitialized; a waiter
     * leaving while no initializer runs may have held the retry turn.
     * Either way the oldest remaining waiter gets the turn. */
    abandoned_init = asx_wait_same_task(s->initializer, task);
    left_queue = asx_wait_queue_remove(&s->waiters, task);
    if (abandoned_init || (left_queue && !s->initialized && !asx_handle_is_valid(s->initializer))) {
        once_pass_turn(s);
    }
    return ASX_OK;
}

asx_status asx_once_get(asx_once_handle handle, uint64_t *out_value) {
    asx_once_slot *s = once_slot_at(handle.slot);
    if (out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    if (!s->initialized) return ASX_E_INVALID_STATE;

    *out_value = s->value;
    return ASX_OK;
}

int asx_once_is_initialized(asx_once_handle handle) {
    const asx_once_slot *s = once_slot_at(handle.slot);
    if (s == NULL) return 0;
    if (!s->alive || s->generation != handle.generation) return 0;
    return s->initialized;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_once_reset_internal(asx_kernel *k) {
    asx_once_state *st = &k->onces;
    uint32_t i;
    for (i = 0; i < st->slot_count; i++) {
        st->slots[i].generation = next_gen(st->slots[i].generation);
        st->slots[i].alive = 0;
        once_slot_clear(&st->slots[i], k->tag);
    }
    st->slot_count = 0;
}

void asx_once_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_once_reset_internal(k);
}
