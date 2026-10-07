/*
 * wait_queue.c — bounded FIFO wait queues for wake-driven primitives
 *
 * See wait_queue.h for the contract. Everything here is built on the
 * public scheduler API (asx_task_current / asx_task_park / asx_task_wake /
 * asx_task_get_state), so primitives never touch kernel internals.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include <asx/asx_config.h>
#include <asx/runtime/runtime.h>

static uint64_t wq_bit(uint32_t i) { return (uint64_t)1u << i; }

int asx_wait_same_task(asx_task_id a, asx_task_id b) {
    if (!asx_handle_is_valid(a) || !asx_handle_is_valid(b)) return 0;
    return asx_handle_type_tag(a) == asx_handle_type_tag(b) &&
           asx_handle_index(a) == asx_handle_index(b);
}

asx_wait_task_liveness_kind asx_wait_task_liveness(asx_task_id task) {
    asx_task_state state;

    if (!asx_handle_is_valid(task)) return ASX_WAIT_TASK_DEAD;
    if (asx_task_get_state(task, &state) != ASX_OK) return ASX_WAIT_TASK_DEAD;

    switch (state) {
    case ASX_TASK_CREATED:
    case ASX_TASK_RUNNING: return ASX_WAIT_TASK_LIVE;
    case ASX_TASK_CANCEL_REQUESTED:
    case ASX_TASK_CANCELLING:
    case ASX_TASK_FINALIZING: return ASX_WAIT_TASK_DOOMED;
    case ASX_TASK_COMPLETED: return ASX_WAIT_TASK_DEAD;
    }
    return ASX_WAIT_TASK_DEAD;
}

static int wq_park(asx_task_id self) {
    asx_status st = asx_task_park(self);
    return st == ASX_OK;
}

int asx_wait_park_current(asx_task_id *slot) {
    asx_task_id self = asx_task_current();
    if (slot == NULL) return 0;
    *slot = self;
    if (self == ASX_INVALID_ID) return 0;
    return wq_park(self);
}

void asx_wait_wake_task(asx_task_id task) {
    asx_status st;
    if (!asx_handle_is_valid(task)) return;
    st = asx_task_wake(task);
    (void)st; /* stale handles fail harmlessly */
}

/* ------------------------------------------------------------------ */
/* Queue maintenance                                                   */
/* ------------------------------------------------------------------ */

void asx_wait_queue_init(asx_wait_queue *q, asx_task_id *storage, uint32_t cap) {
    if (q == NULL) return;
    if (storage == NULL) cap = 0u;
    if (cap > ASX_WAIT_QUEUE_MAX_CAPACITY) cap = ASX_WAIT_QUEUE_MAX_CAPACITY;
    q->tasks = storage;
    q->cap = cap;
    q->len = 0u;
    q->woken = 0u;
}

uint32_t asx_wait_queue_len(const asx_wait_queue *q) {
    if (q == NULL) return 0u;
    return q->len;
}

/* Remove entry i, preserving the FIFO order of the remaining entries and
 * their woken flags. */
static void wq_remove_at(asx_wait_queue *q, uint32_t i) {
    uint64_t below = wq_bit(i) - 1u;
    uint32_t j;

    for (j = i; j + 1u < q->len; j++) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_QUEUE_MAX_CAPACITY");
        q->tasks[j] = q->tasks[j + 1u];
    }
    q->woken = (q->woken & below) | ((q->woken >> 1) & ~below);
    q->len--;
}

/* Index of `task` in the queue, or q->len if absent. */
static uint32_t wq_find(const asx_wait_queue *q, asx_task_id task) {
    uint32_t i;
    for (i = 0; i < q->len; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_QUEUE_MAX_CAPACITY");
        if (asx_wait_same_task(q->tasks[i], task)) return i;
    }
    return q->len;
}

/* Drop every entry whose task is gone. */
static void wq_reap_dead(asx_wait_queue *q) {
    uint32_t i = 0;
    while (i < q->len) {
        ASX_CHECKPOINT_WAIVER("bounded: each pass removes an entry or advances");
        if (asx_wait_task_liveness(q->tasks[i]) == ASX_WAIT_TASK_DEAD) {
            wq_remove_at(q, i);
        } else {
            i++;
        }
    }
}

/* Evict the oldest entry that already holds a wake (it re-registers if it
 * comes back). Returns 1 if an entry was evicted. */
static int wq_evict_woken(asx_wait_queue *q) {
    uint32_t i;
    for (i = 0; i < q->len; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_QUEUE_MAX_CAPACITY");
        if ((q->woken & wq_bit(i)) != 0u) {
            wq_remove_at(q, i);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Park / leave / remove                                               */
/* ------------------------------------------------------------------ */

int asx_wait_queue_park_current(asx_wait_queue *q) {
    asx_task_id self;
    uint32_t i;

    if (q == NULL) return 0;
    self = asx_task_current();
    if (self == ASX_INVALID_ID) return 0;

    i = wq_find(q, self);
    if (i < q->len) {
        /* Still waiting: re-arm in place (keeps the FIFO position). */
        q->tasks[i] = self;
        q->woken &= ~wq_bit(i);
        return wq_park(self);
    }

    if (q->len >= q->cap) {
        wq_reap_dead(q);
        if (q->len >= q->cap) (void)wq_evict_woken(q);
        /* Every slot holds a live un-woken waiter: yield cooperatively. */
        if (q->len >= q->cap) return 0;
    }

    q->tasks[q->len] = self;
    q->woken &= ~wq_bit(q->len);
    q->len++;
    return wq_park(self);
}

void asx_wait_queue_leave_current(asx_wait_queue *q) {
    asx_task_id self;
    uint32_t i;

    if (q == NULL || q->len == 0u) return;
    self = asx_task_current();
    if (self == ASX_INVALID_ID) return;
    i = wq_find(q, self);
    if (i < q->len) wq_remove_at(q, i);
}

int asx_wait_queue_remove(asx_wait_queue *q, asx_task_id task) {
    uint32_t i;

    if (q == NULL || q->len == 0u || !asx_handle_is_valid(task)) return 0;
    i = wq_find(q, task);
    if (i >= q->len) return 0;
    wq_remove_at(q, i);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Wake                                                                */
/* ------------------------------------------------------------------ */

uint32_t asx_wait_queue_settle(asx_wait_queue *q, uint32_t available) {
    uint8_t kind[ASX_WAIT_QUEUE_MAX_CAPACITY];
    uint32_t pending = 0;
    uint32_t woke = 0;
    uint32_t i = 0;

    if (q == NULL || q->len == 0u || available == 0u) return 0u;

    /* Drop dead entries and count wakes still held by live waiters. */
    while (i < q->len) {
        asx_wait_task_liveness_kind k;
        ASX_CHECKPOINT_WAIVER("bounded: each pass removes an entry or advances");
        k = asx_wait_task_liveness(q->tasks[i]);
        if (k == ASX_WAIT_TASK_DEAD) {
            wq_remove_at(q, i);
            continue;
        }
        kind[i] = (uint8_t)k;
        if (k == ASX_WAIT_TASK_LIVE && (q->woken & wq_bit(i)) != 0u) pending++;
        i++;
    }

    /* Hand the remaining units to un-woken waiters, oldest first. A
     * cancel-pending waiter is woken (so it reaches its cleanup) but does
     * not absorb a unit. */
    for (i = 0; i < q->len && pending < available; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_QUEUE_MAX_CAPACITY");
        if ((q->woken & wq_bit(i)) != 0u) continue;
        q->woken |= wq_bit(i);
        asx_wait_wake_task(q->tasks[i]);
        if (kind[i] == (uint8_t)ASX_WAIT_TASK_LIVE) {
            pending++;
            woke++;
        }
    }
    return woke;
}

uint32_t asx_wait_queue_live_ahead(const asx_wait_queue *q) {
    asx_task_id self;
    uint32_t end;
    uint32_t i;
    uint32_t live = 0;

    if (q == NULL || q->len == 0u) return 0u;
    self = asx_task_current();
    end = (self == ASX_INVALID_ID) ? q->len : wq_find(q, self);
    for (i = 0; i < end; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_QUEUE_MAX_CAPACITY");
        if (asx_wait_task_liveness(q->tasks[i]) == ASX_WAIT_TASK_LIVE) live++;
    }
    return live;
}

uint32_t asx_wait_queue_wake_all(asx_wait_queue *q) {
    uint32_t i = 0;
    uint32_t woken = 0;

    if (q == NULL) return 0u;
    while (i < q->len) {
        ASX_CHECKPOINT_WAIVER("bounded: each pass removes an entry or advances");
        if (asx_wait_task_liveness(q->tasks[i]) == ASX_WAIT_TASK_DEAD) {
            wq_remove_at(q, i);
            continue;
        }
        q->woken |= wq_bit(i);
        asx_wait_wake_task(q->tasks[i]);
        woken++;
        i++;
    }
    return woken;
}
