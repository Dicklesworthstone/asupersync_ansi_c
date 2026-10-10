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

#include "wait_queue.h"
#include <asx/runtime/runtime.h>
#include <asx/sync/once.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t generation;
    int alive;
    int initialized;
    uint64_t value;
    asx_task_id initializer; /* task running a pending init, or ASX_INVALID_ID */
    asx_wait_queue waiters;  /* tasks parked behind the initializer */
} once_slot;

static once_slot g_slots[ASX_ONCE_MAX];
static uint32_t g_slot_count;

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void once_slot_clear(once_slot *s) {
    s->initialized = 0;
    s->value = 0;
    s->initializer = ASX_INVALID_ID;
    asx_wait_queue_init(&s->waiters);
}

/* The initialization in progress was abandoned or failed: the cell stays
 * uninitialized and the oldest waiter gets the next turn. */
static void once_pass_turn(once_slot *s) {
    s->initializer = ASX_INVALID_ID;
    if (s->waiters.len > 0u) (void)asx_wait_queue_settle(&s->waiters, 1u);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_once_create(asx_once_handle *out) {
    uint32_t i;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;

    for (i = 0; i < ASX_ONCE_MAX; i++) {
        if (!g_slots[i].alive) {
            g_slots[i].alive = 1;
            g_slots[i].generation = next_gen(g_slots[i].generation);
            once_slot_clear(&g_slots[i]);
            out->slot = i;
            out->generation = g_slots[i].generation;
            if (i >= g_slot_count) g_slot_count = i + 1;
            return ASX_OK;
        }
    }
    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_once_close(asx_once_handle handle) {
    once_slot *s;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
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
    once_slot *s;
    asx_status st;
    asx_task_id self;

    if (init_fn == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
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
    once_slot *s;
    int abandoned_init;
    int left_queue;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
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
    once_slot *s;
    if (out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (handle.slot >= ASX_ONCE_MAX) return ASX_E_INVALID_ARGUMENT;
    s = &g_slots[handle.slot];
    if (!s->alive || s->generation != handle.generation) return ASX_E_STALE_HANDLE;

    if (!s->initialized) return ASX_E_INVALID_STATE;

    *out_value = s->value;
    return ASX_OK;
}

int asx_once_is_initialized(asx_once_handle handle) {
    if (handle.slot >= ASX_ONCE_MAX) return 0;
    if (!g_slots[handle.slot].alive || g_slots[handle.slot].generation != handle.generation)
        return 0;
    return g_slots[handle.slot].initialized;
}

/* ------------------------------------------------------------------ */
/* Arena management                                                    */
/* ------------------------------------------------------------------ */

void asx_once_reset(void) {
    uint32_t i;
    for (i = 0; i < g_slot_count; i++) {
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].alive = 0;
        once_slot_clear(&g_slots[i]);
    }
    g_slot_count = 0;
}
