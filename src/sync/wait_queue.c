/*
 * wait_queue.c — FIFO wait queues over a shared pool of wait nodes
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

/* ------------------------------------------------------------------ */
/* Pool                                                                */
/* ------------------------------------------------------------------ */

static asx_wait_node g_nodes[ASX_WAIT_NODE_CAPACITY];
static uint32_t g_free_head = ASX_WAIT_NIL; /* released nodes, most recent first */
static uint32_t g_high;                     /* nodes [g_high, capacity) never handed out */
static uint32_t g_in_use;
/* Queues built before the last pool reset are empty. Zero-initialized
 * queues carry epoch 0, so the first epoch is 1. */
static uint32_t g_epoch = 1u;

void asx_wait_pool_reset(void) {
    g_free_head = ASX_WAIT_NIL;
    g_high = 0u;
    g_in_use = 0u;
    g_epoch++;
    if (g_epoch == 0u) g_epoch = 1u;
}

uint32_t asx_wait_nodes_in_use(void) { return g_in_use; }

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
/* Links                                                               */
/* ------------------------------------------------------------------ */

static int wq_current(const asx_wait_queue *q) { return q->epoch == g_epoch; }

/* A queue built before the last pool reset holds nothing. */
static void wq_sync(asx_wait_queue *q) {
    if (wq_current(q)) return;
    q->head = ASX_WAIT_NIL;
    q->tail = ASX_WAIT_NIL;
    q->len = 0u;
    q->epoch = g_epoch;
}

static void wq_link_tail(asx_wait_queue *q, uint32_t i) {
    asx_wait_node *n = &g_nodes[i];
    n->queue = q;
    n->prev = q->tail;
    n->next = ASX_WAIT_NIL;
    if (q->tail == ASX_WAIT_NIL) {
        q->head = i;
    } else {
        g_nodes[q->tail].next = i;
    }
    q->tail = i;
    q->len++;
}

static void wq_unlink(asx_wait_queue *q, uint32_t i) {
    asx_wait_node *n = &g_nodes[i];
    if (n->prev == ASX_WAIT_NIL) {
        q->head = n->next;
    } else {
        g_nodes[n->prev].next = n->next;
    }
    if (n->next == ASX_WAIT_NIL) {
        q->tail = n->prev;
    } else {
        g_nodes[n->next].prev = n->prev;
    }
    n->prev = ASX_WAIT_NIL;
    n->next = ASX_WAIT_NIL;
    q->len--;
}

/* Unlink node i from q and return it to the pool. */
static void wq_release(asx_wait_queue *q, uint32_t i) {
    asx_wait_node *n = &g_nodes[i];
    wq_unlink(q, i);
    n->queue = NULL;
    n->task = ASX_INVALID_ID;
    n->woken = 0u;
    n->flags = 0u;
    n->value = 0u;
    n->generation++;
    n->next = g_free_head;
    g_free_head = i;
    g_in_use--;
}

/* Release every node of q (a current queue). */
static void wq_release_all(asx_wait_queue *q) {
    while (q->head != ASX_WAIT_NIL) {
        ASX_CHECKPOINT_WAIVER("bounded: each pass releases a node");
        wq_release(q, q->head);
    }
}

/* Give the pool back every node whose task died, through its queue:
 * record queues retire their dead waiters with their own semantics, plain
 * queue entries are dropped. */
static void wq_reclaim(void) {
    uint32_t i;
    for (i = 0; i < g_high; i++) {
        asx_wait_node *n = &g_nodes[i];
        asx_wait_queue *q = n->queue;
        ASX_CHECKPOINT_WAIVER("bounded: i < ASX_WAIT_NODE_CAPACITY");
        if (q == NULL || !wq_current(q) || !asx_handle_is_valid(n->task)) continue;
        if (asx_wait_task_liveness(n->task) != ASX_WAIT_TASK_DEAD) continue;
        if (q->reap != NULL) {
            q->reap(q);
        } else {
            wq_release(q, i);
        }
    }
}

static uint32_t wq_take_free(void) {
    uint32_t i;
    if (g_free_head != ASX_WAIT_NIL) {
        i = g_free_head;
        g_free_head = g_nodes[i].next;
    } else if (g_high < ASX_WAIT_NODE_CAPACITY) {
        i = g_high++;
    } else {
        return ASX_WAIT_NIL;
    }
    g_in_use++;
    return i;
}

/* A fresh node linked at the tail of q, or ASX_WAIT_NIL. */
static uint32_t wq_add(asx_wait_queue *q, asx_task_id task) {
    uint32_t i = wq_take_free();
    asx_wait_node *n;
    if (i == ASX_WAIT_NIL) {
        wq_reclaim();
        i = wq_take_free();
        if (i == ASX_WAIT_NIL) return ASX_WAIT_NIL;
    }
    n = &g_nodes[i];
    n->task = task;
    n->woken = 0u;
    n->flags = 0u;
    n->value = 0u;
    wq_link_tail(q, i);
    return i;
}

/* ------------------------------------------------------------------ */
/* Plain queues                                                        */
/* ------------------------------------------------------------------ */

void asx_wait_queue_init(asx_wait_queue *q) { asx_wait_records_init(q, NULL); }

void asx_wait_queue_clear(asx_wait_queue *q) {
    if (q == NULL) return;
    wq_sync(q);
    wq_release_all(q);
}

uint32_t asx_wait_queue_len(const asx_wait_queue *q) {
    if (q == NULL || !wq_current(q)) return 0u;
    return q->len;
}

/* Index of `task` in the queue, or ASX_WAIT_NIL. */
static uint32_t wq_find(const asx_wait_queue *q, asx_task_id task) {
    uint32_t i;
    for (i = q->head; i != ASX_WAIT_NIL; i = g_nodes[i].next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (asx_wait_same_task(g_nodes[i].task, task)) return i;
    }
    return ASX_WAIT_NIL;
}

/* Drop every entry whose task is gone. */
static void wq_drop_dead(asx_wait_queue *q) {
    uint32_t i = q->head;
    while (i != ASX_WAIT_NIL) {
        uint32_t next = g_nodes[i].next;
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (asx_wait_task_liveness(g_nodes[i].task) == ASX_WAIT_TASK_DEAD) wq_release(q, i);
        i = next;
    }
}

int asx_wait_queue_park_current(asx_wait_queue *q) {
    asx_task_id self;
    uint32_t i;

    if (q == NULL) return 0;
    self = asx_task_current();
    if (self == ASX_INVALID_ID) return 0;
    wq_sync(q);

    i = wq_find(q, self);
    if (i != ASX_WAIT_NIL) {
        /* Still waiting: re-arm in place (keeps the FIFO position). */
        g_nodes[i].task = self;
        g_nodes[i].woken = 0u;
        return wq_park(self);
    }

    wq_drop_dead(q);
    /* Pool exhausted: yield cooperatively (re-polled next round). */
    if (wq_add(q, self) == ASX_WAIT_NIL) return 0;
    return wq_park(self);
}

void asx_wait_queue_leave_current(asx_wait_queue *q) {
    asx_task_id self;
    uint32_t i;

    if (q == NULL || !wq_current(q) || q->len == 0u) return;
    self = asx_task_current();
    if (self == ASX_INVALID_ID) return;
    i = wq_find(q, self);
    if (i != ASX_WAIT_NIL) wq_release(q, i);
}

int asx_wait_queue_remove(asx_wait_queue *q, asx_task_id task) {
    uint32_t i;

    if (q == NULL || !wq_current(q) || q->len == 0u || !asx_handle_is_valid(task)) return 0;
    i = wq_find(q, task);
    if (i == ASX_WAIT_NIL) return 0;
    wq_release(q, i);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Wake                                                                */
/* ------------------------------------------------------------------ */

uint32_t asx_wait_queue_settle(asx_wait_queue *q, uint32_t available) {
    uint32_t pending = 0;
    uint32_t woke = 0;
    uint32_t i;

    if (q == NULL || !wq_current(q) || q->len == 0u || available == 0u) return 0u;

    /* Count wakes still held by live waiters (dead entries never count). */
    for (i = q->head; i != ASX_WAIT_NIL; i = g_nodes[i].next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (g_nodes[i].woken != 0u &&
            asx_wait_task_liveness(g_nodes[i].task) == ASX_WAIT_TASK_LIVE) {
            pending++;
        }
    }

    /* Hand the remaining units to un-woken waiters, oldest first. A
     * cancel-pending waiter is woken (so it reaches its cleanup) but does
     * not absorb a unit. */
    for (i = q->head; i != ASX_WAIT_NIL && pending < available; i = g_nodes[i].next) {
        asx_wait_task_liveness_kind k;
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (g_nodes[i].woken != 0u) continue;
        k = asx_wait_task_liveness(g_nodes[i].task);
        if (k == ASX_WAIT_TASK_DEAD) continue;
        g_nodes[i].woken = 1u;
        asx_wait_wake_task(g_nodes[i].task);
        if (k == ASX_WAIT_TASK_LIVE) {
            pending++;
            woke++;
        }
    }
    return woke;
}

uint32_t asx_wait_queue_live_ahead(const asx_wait_queue *q) {
    asx_task_id self;
    uint32_t i;
    uint32_t live = 0;

    if (q == NULL || !wq_current(q) || q->len == 0u) return 0u;
    self = asx_task_current();
    for (i = q->head; i != ASX_WAIT_NIL; i = g_nodes[i].next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (self != ASX_INVALID_ID && asx_wait_same_task(g_nodes[i].task, self)) break;
        if (asx_wait_task_liveness(g_nodes[i].task) == ASX_WAIT_TASK_LIVE) live++;
    }
    return live;
}

uint32_t asx_wait_queue_wake_all(asx_wait_queue *q) {
    uint32_t i;
    uint32_t woken = 0;

    if (q == NULL || !wq_current(q)) return 0u;
    for (i = q->head; i != ASX_WAIT_NIL; i = g_nodes[i].next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (asx_wait_task_liveness(g_nodes[i].task) == ASX_WAIT_TASK_DEAD) continue;
        g_nodes[i].woken = 1u;
        asx_wait_wake_task(g_nodes[i].task);
        woken++;
    }
    return woken;
}

/* ------------------------------------------------------------------ */
/* Waiter records                                                      */
/* ------------------------------------------------------------------ */

void asx_wait_records_init(asx_wait_queue *q, asx_wait_reap_fn reap) {
    if (q == NULL) return;
    if (wq_current(q)) wq_release_all(q);
    q->head = ASX_WAIT_NIL;
    q->tail = ASX_WAIT_NIL;
    q->len = 0u;
    q->epoch = g_epoch;
    q->reap = reap;
}

uint32_t asx_wait_record_add(asx_wait_queue *q) {
    if (q == NULL) return ASX_WAIT_NIL;
    wq_sync(q);
    return wq_add(q, ASX_INVALID_ID);
}

asx_wait_node *asx_wait_record_get(asx_wait_queue *q, uint32_t index, uint16_t generation) {
    asx_wait_node *n;
    if (q == NULL || index >= ASX_WAIT_NODE_CAPACITY) return NULL;
    n = &g_nodes[index];
    if (n->queue != q || n->generation != generation || !wq_current(q)) return NULL;
    return n;
}

asx_wait_node *asx_wait_node_at(uint32_t index) { return &g_nodes[index]; }

void asx_wait_record_release(asx_wait_queue *q, uint32_t index) { wq_release(q, index); }

void asx_wait_record_move_to_tail(asx_wait_queue *q, uint32_t index) {
    wq_unlink(q, index);
    wq_link_tail(q, index);
}

uint32_t asx_wait_queue_first(const asx_wait_queue *q) {
    if (q == NULL || !wq_current(q)) return ASX_WAIT_NIL;
    return q->head;
}

uint32_t asx_wait_queue_next(uint32_t index) { return g_nodes[index].next; }
