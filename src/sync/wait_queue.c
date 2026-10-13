/*
 * wait_queue.c — FIFO wait queues over a pool of wait nodes, one pool per
 * runtime
 *
 * See wait_queue.h for the contract. Everything here is built on the
 * public scheduler API (asx_task_current / asx_task_park / asx_task_wake /
 * asx_task_get_state), so primitives never touch kernel internals.
 *
 * A queue names the pool its nodes come from by the runtime tag it was
 * initialized with (zeroed: tag 0), and a node index handed to a primitive
 * carries that tag above its low 24 bits, so a primitive's calls on
 * indices need no runtime argument. With one runtime every id is the plain
 * index it always was.
 *
 * SPDX-License-Identifier: MIT
 */

#include "wait_queue.h"
#include "../runtime/runtime_internal.h"
#include <asx/asx_config.h>
#include <asx/runtime/runtime.h>

#if (ASX_WAIT_NODE_CAPACITY) > 0xFFFFFF
#error "ASX_WAIT_NODE_CAPACITY must fit the low 24 bits of a wait-node id"
#endif

/* ------------------------------------------------------------------ */
/* Pool                                                                */
/* ------------------------------------------------------------------ */

#define WQ_TAG_SHIFT 24u
#define WQ_INDEX_MASK 0xFFFFFFu

static uint32_t wq_id(uint16_t tag, uint32_t index) {
    return ((uint32_t)tag << WQ_TAG_SHIFT) | index;
}
static uint16_t wq_tag_of(uint32_t id) { return (uint16_t)(id >> WQ_TAG_SHIFT); }
static uint32_t wq_index_of(uint32_t id) { return id & WQ_INDEX_MASK; }

/* The pool of runtime `tag`, NULL when no runtime holds it. */
static asx_wait_pool *wq_pool(uint16_t tag) {
    asx_kernel *k = asx_kernel_for_tag(tag);
    return k != NULL ? &k->waits : NULL;
}

/* Where a node id of a pool that is gone resolves: nothing links to it. */
static asx_wait_node g_wq_gone = {ASX_INVALID_ID, NULL, ASX_WAIT_NIL, ASX_WAIT_NIL, 0u, 0u, 0u, 0u};

static asx_wait_node *wq_node(uint32_t id) {
    asx_wait_pool *p = wq_pool(wq_tag_of(id));
    if (p == NULL || wq_index_of(id) >= ASX_WAIT_NODE_CAPACITY) return &g_wq_gone;
    return &p->nodes[wq_index_of(id)];
}

/* Queues built before the last reset of their pool are empty. A zeroed
 * pool has epoch 0, so the first effective epoch is 1 and zero-initialized
 * queues (epoch 0) are never current. */
static uint32_t wq_epoch(const asx_wait_pool *p) { return p->epoch + 1u; }

void asx_wait_pool_reset_internal(asx_kernel *k) {
    asx_wait_pool *p = &k->waits;
    p->free_head = 0u;
    p->high = 0u;
    p->in_use = 0u;
    p->epoch++;
    if (p->epoch == UINT32_MAX) p->epoch = 0u;
}

/* One task of one runtime: slot, generation and runtime tag, not state bits. */
int asx_wait_same_task(asx_task_id a, asx_task_id b) {
    if (!asx_handle_is_valid(a) || !asx_handle_is_valid(b)) return 0;
    return asx_handle_same_entity(a, b);
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

static int wq_current(const asx_wait_queue *q) {
    const asx_wait_pool *p = wq_pool(q->tag);
    return p != NULL && q->epoch == wq_epoch(p);
}

/* A queue built before the last pool reset holds nothing. */
static void wq_sync(asx_wait_queue *q) {
    const asx_wait_pool *p;
    if (wq_current(q)) return;
    p = wq_pool(q->tag);
    q->head = ASX_WAIT_NIL;
    q->tail = ASX_WAIT_NIL;
    q->len = 0u;
    q->epoch = p != NULL ? wq_epoch(p) : 0u;
}

static void wq_link_tail(asx_wait_queue *q, uint32_t i) {
    asx_wait_node *n = wq_node(i);
    n->queue = q;
    n->prev = q->tail;
    n->next = ASX_WAIT_NIL;
    if (q->tail == ASX_WAIT_NIL) {
        q->head = i;
    } else {
        wq_node(q->tail)->next = i;
    }
    q->tail = i;
    q->len++;
}

static void wq_unlink(asx_wait_queue *q, uint32_t i) {
    asx_wait_node *n = wq_node(i);
    if (n->prev == ASX_WAIT_NIL) {
        q->head = n->next;
    } else {
        wq_node(n->prev)->next = n->next;
    }
    if (n->next == ASX_WAIT_NIL) {
        q->tail = n->prev;
    } else {
        wq_node(n->next)->prev = n->prev;
    }
    n->prev = ASX_WAIT_NIL;
    n->next = ASX_WAIT_NIL;
    q->len--;
}

/* Unlink node i from q and return it to the pool of q's runtime. */
static void wq_release(asx_wait_queue *q, uint32_t i) {
    asx_wait_pool *p = wq_pool(q->tag);
    asx_wait_node *n = wq_node(i);
    wq_unlink(q, i);
    n->queue = NULL;
    n->task = ASX_INVALID_ID;
    n->woken = 0u;
    n->flags = 0u;
    n->value = 0u;
    n->generation++;
    if (p == NULL) return;
    n->next = p->free_head;
    p->free_head = wq_index_of(i) + 1u;
    p->in_use--;
}

/* Release every node of q (a current queue). */
static void wq_release_all(asx_wait_queue *q) {
    while (q->head != ASX_WAIT_NIL) {
        ASX_CHECKPOINT_WAIVER("bounded: each pass releases a node");
        wq_release(q, q->head);
    }
}

/* Give the pool of runtime `tag` back every node whose task died, through
 * its queue: record queues retire their dead waiters with their own
 * semantics, plain queue entries are dropped. */
static void wq_reclaim(uint16_t tag) {
    asx_wait_pool *p = wq_pool(tag);
    uint32_t i;
    if (p == NULL) return;
    for (i = 0; i < p->high; i++) {
        asx_wait_node *n = &p->nodes[i];
        asx_wait_queue *q = n->queue;
        ASX_CHECKPOINT_WAIVER("bounded: i < ASX_WAIT_NODE_CAPACITY");
        if (q == NULL || !wq_current(q) || !asx_handle_is_valid(n->task)) continue;
        if (asx_wait_task_liveness(n->task) != ASX_WAIT_TASK_DEAD) continue;
        if (q->reap != NULL) {
            q->reap(q);
        } else {
            wq_release(q, wq_id(tag, i));
        }
    }
}

/* A free node index of `p`, or ASX_WAIT_NIL when it is exhausted. */
static uint32_t wq_take_free(asx_wait_pool *p) {
    uint32_t i;
    if (p->free_head != 0u) {
        i = p->free_head - 1u;
        p->free_head = p->nodes[i].next;
    } else if (p->high < ASX_WAIT_NODE_CAPACITY) {
        i = p->high++;
    } else {
        return ASX_WAIT_NIL;
    }
    p->in_use++;
    return i;
}

/* A fresh node linked at the tail of q, or ASX_WAIT_NIL. */
static uint32_t wq_add(asx_wait_queue *q, asx_task_id task) {
    asx_wait_pool *p = wq_pool(q->tag);
    uint32_t i;
    asx_wait_node *n;
    if (p == NULL) return ASX_WAIT_NIL;
    i = wq_take_free(p);
    if (i == ASX_WAIT_NIL) {
        wq_reclaim(q->tag);
        i = wq_take_free(p);
        if (i == ASX_WAIT_NIL) return ASX_WAIT_NIL;
    }
    n = &p->nodes[i];
    n->task = task;
    n->woken = 0u;
    n->flags = 0u;
    n->value = 0u;
    wq_link_tail(q, wq_id(q->tag, i));
    return wq_id(q->tag, i);
}

/* ------------------------------------------------------------------ */
/* Plain queues                                                        */
/* ------------------------------------------------------------------ */

void asx_wait_queue_init(asx_wait_queue *q, asx_wait_reap_fn reap, uint16_t tag) {
    asx_wait_records_init(q, reap, tag);
}

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
    for (i = q->head; i != ASX_WAIT_NIL; i = wq_node(i)->next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (asx_wait_same_task(wq_node(i)->task, task)) return i;
    }
    return ASX_WAIT_NIL;
}

/* Drop every entry whose task is gone. */
static void wq_drop_dead(asx_wait_queue *q) {
    uint32_t i = q->head;
    while (i != ASX_WAIT_NIL) {
        uint32_t next = wq_node(i)->next;
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (asx_wait_task_liveness(wq_node(i)->task) == ASX_WAIT_TASK_DEAD) wq_release(q, i);
        i = next;
    }
}

void asx_wait_queue_drop_dead(asx_wait_queue *q) {
    if (q == NULL || !wq_current(q)) return;
    wq_drop_dead(q);
}

void asx_wait_task_finished(asx_task_id task) {
    uint16_t tag;
    asx_wait_pool *p;
    uint32_t i;
    if (!asx_handle_is_valid(task)) return;
    tag = asx_handle_runtime_tag(task);
    p = wq_pool(tag);
    if (p == NULL || p->in_use == 0u) return;
    for (i = 0; i < p->high; i++) {
        asx_wait_node *n = &p->nodes[i];
        asx_wait_queue *q = n->queue;
        ASX_CHECKPOINT_WAIVER("bounded: i < ASX_WAIT_NODE_CAPACITY");
        if (q == NULL || !wq_current(q) || !asx_wait_same_task(n->task, task)) continue;
        if (q->reap != NULL) {
            q->reap(q);
        } else {
            wq_release(q, wq_id(tag, i));
        }
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
        wq_node(i)->task = self;
        wq_node(i)->woken = 0u;
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

    /* Count wakes still held by waiters whose task has not finished (dead
     * entries never count). */
    for (i = q->head; i != ASX_WAIT_NIL; i = wq_node(i)->next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (wq_node(i)->woken != 0u &&
            asx_wait_task_liveness(wq_node(i)->task) != ASX_WAIT_TASK_DEAD) {
            pending++;
        }
    }

    /* Hand the remaining units to un-woken waiters, oldest first. A
     * cancel-pending waiter keeps its place and absorbs its unit like any
     * other: it passes the unit on when its next poll observes the cancel
     * and it leaves (remove, then the caller re-settles), as a Rust waiter
     * stays queued until its future is polled or dropped (mpsc.rs:363-377,
     * sync/pool.rs:1370-1382; bd-wy6n). A masked one keeps the unit. */
    for (i = q->head; i != ASX_WAIT_NIL && pending < available; i = wq_node(i)->next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (wq_node(i)->woken != 0u) continue;
        if (asx_wait_task_liveness(wq_node(i)->task) == ASX_WAIT_TASK_DEAD) continue;
        wq_node(i)->woken = 1u;
        asx_wait_wake_task(wq_node(i)->task);
        pending++;
        woke++;
    }
    return woke;
}

uint32_t asx_wait_queue_live_ahead(const asx_wait_queue *q) {
    asx_task_id self;
    uint32_t i;
    uint32_t live = 0;

    if (q == NULL || !wq_current(q) || q->len == 0u) return 0u;
    self = asx_task_current();
    for (i = q->head; i != ASX_WAIT_NIL; i = wq_node(i)->next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (self != ASX_INVALID_ID && asx_wait_same_task(wq_node(i)->task, self)) break;
        if (asx_wait_task_liveness(wq_node(i)->task) != ASX_WAIT_TASK_DEAD) live++;
    }
    return live;
}

uint32_t asx_wait_queue_wake_all(asx_wait_queue *q) {
    uint32_t i;
    uint32_t woken = 0;

    if (q == NULL || !wq_current(q)) return 0u;
    for (i = q->head; i != ASX_WAIT_NIL; i = wq_node(i)->next) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= ASX_WAIT_NODE_CAPACITY");
        if (asx_wait_task_liveness(wq_node(i)->task) == ASX_WAIT_TASK_DEAD) continue;
        wq_node(i)->woken = 1u;
        asx_wait_wake_task(wq_node(i)->task);
        woken++;
    }
    return woken;
}

/* ------------------------------------------------------------------ */
/* Waiter records                                                      */
/* ------------------------------------------------------------------ */

void asx_wait_records_init(asx_wait_queue *q, asx_wait_reap_fn reap, uint16_t tag) {
    const asx_wait_pool *p;
    if (q == NULL) return;
    if (wq_current(q)) wq_release_all(q);
    q->tag = tag;
    p = wq_pool(tag);
    q->head = ASX_WAIT_NIL;
    q->tail = ASX_WAIT_NIL;
    q->len = 0u;
    q->epoch = p != NULL ? wq_epoch(p) : 0u;
    q->reap = reap;
}

uint32_t asx_wait_record_add(asx_wait_queue *q) {
    if (q == NULL) return ASX_WAIT_NIL;
    wq_sync(q);
    return wq_add(q, ASX_INVALID_ID);
}

asx_wait_node *asx_wait_record_get(asx_wait_queue *q, uint32_t index, uint16_t generation) {
    asx_wait_node *n;
    if (q == NULL || wq_tag_of(index) != q->tag || wq_index_of(index) >= ASX_WAIT_NODE_CAPACITY) {
        return NULL;
    }
    n = wq_node(index);
    if (n->queue != q || n->generation != generation || !wq_current(q)) return NULL;
    return n;
}

asx_wait_node *asx_wait_node_at(uint32_t index) { return wq_node(index); }

void asx_wait_record_release(asx_wait_queue *q, uint32_t index) { wq_release(q, index); }

void asx_wait_record_move_to_tail(asx_wait_queue *q, uint32_t index) {
    wq_unlink(q, index);
    wq_link_tail(q, index);
}

uint32_t asx_wait_queue_first(const asx_wait_queue *q) {
    if (q == NULL || !wq_current(q)) return ASX_WAIT_NIL;
    return q->head;
}

uint32_t asx_wait_queue_next(uint32_t index) { return wq_node(index)->next; }
