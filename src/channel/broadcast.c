/*
 * broadcast.c — bounded broadcast channel
 *
 * Fixed-size arena, single-threaded. Ring buffer with per-receiver
 * cursors; senders and receivers are counted as Rust's clones are
 * (channel/broadcast.rs:228-233).
 *
 * Wake-driven waiting: asx_broadcast_recv finding nothing new inside a
 * scheduler poll parks the calling task (try_recv never parks, bd-vc1v).
 * Every publication (each receiver has its own cursor, so every receiver
 * gains a message) and the last sender's drop wake all parked tasks, in
 * FIFO order.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include "../sync/wait_queue.h"
#include <asx/core/broadcast.h>
#include <asx/runtime/runtime.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal slot                                                       */
/* ------------------------------------------------------------------ */

/* Rust keeps a parked receiver's waker in an Arena (broadcast.rs:200,
 * util/arena.rs): a registration takes the most recently freed slot, or a
 * new one at the end; a removal frees its slot onto the free list; a
 * publication or the last sender's drop drains every waker in slot order
 * (drain_values, arena.rs:530-589), so the wake order is slot order, not
 * arrival order, once a removal has left a hole. C mirrors that arena to
 * wake in the same order. One registration per receiver (a Rust recv
 * borrows it mutably), so the receiver limit bounds it. */
#define BC_NIL UINT32_MAX
typedef struct {
    asx_task_id task;   /* registered task, ASX_INVALID_ID when vacant */
    uint32_t next_free; /* free list link while vacant */
} bc_waker_slot;

typedef struct {
    uint16_t generation;
    uint32_t sender_count; /* live senders; the channel closes at 0 */
    uint32_t capacity;
    uint64_t buffer[ASX_BROADCAST_MAX_CAPACITY];
    uint64_t write_seq; /* messages published so far (Rust total_sent) */
    /* Sequence of the oldest message the ring still owns: those below it
     * were dropped (the last receiver's drop takes the whole buffer), so a
     * later overwrite of their slots drops nothing. */
    uint64_t live_from;
    asx_broadcast_discard_fn discard_fn; /* asx_broadcast_set_discard */
    void *discard_ctx;
    uint32_t receiver_count;
    asx_wait_queue waiters; /* tasks parked in asx_broadcast_recv */
    bc_waker_slot wakers[ASX_BROADCAST_MAX_RECEIVERS];
    uint32_t wakers_len;  /* slots in use or freed (Rust slots.len()) */
    uint32_t wakers_live; /* occupied slots */
    uint32_t wakers_free; /* free list head, BC_NIL when empty */
} asx_broadcast_slot;

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

static asx_broadcast_slot g_slots[ASX_MAX_BROADCASTS];
static uint32_t g_slot_count = 0;
static uint32_t g_permit_serial = 0;

/* Physical credits survive a refused obligation registration. Without
 * them a copied, already-spent untracked permit could publish again. */
typedef struct {
    uint32_t serial; /* zero marks a free credit */
    uint32_t slot;
    uint16_t generation;
    asx_task_id holder;
} bc_credit;

static bc_credit g_credits[ASX_MAX_OBLIGATIONS];
static uint32_t g_credit_live;

static asx_status broadcast_credit(const asx_broadcast_permit *permit, uint32_t *out) {
    uint32_t i;
    if (permit->serial == 0u || permit->serial > g_permit_serial) return ASX_E_PERMISSION_DENIED;
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_MAX_OBLIGATIONS physical broadcast credits");
        if (g_credits[i].serial != permit->serial) continue;
        if (g_credits[i].slot != permit->slot || g_credits[i].generation != permit->generation) {
            return ASX_E_PERMISSION_DENIED;
        }
        *out = i;
        return ASX_OK;
    }
    return ASX_E_OBLIGATION_ALREADY_RESOLVED;
}

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void broadcast_reap(asx_wait_queue *q);

static void broadcast_waiters_init(asx_broadcast_slot *s) {
    asx_wait_queue_init(&s->waiters, broadcast_reap, 0u);
    s->wakers_len = 0;
    s->wakers_live = 0;
    s->wakers_free = BC_NIL;
}

/* The waker slot `task` holds, or BC_NIL. */
static uint32_t bc_waker_find(const asx_broadcast_slot *s, asx_task_id task) {
    uint32_t i;
    for (i = 0; task != ASX_INVALID_ID && i < s->wakers_len; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_BROADCAST_MAX_RECEIVERS waker slots");
        if (s->wakers[i].task != ASX_INVALID_ID && asx_wait_same_task(s->wakers[i].task, task)) {
            return i;
        }
    }
    return BC_NIL;
}

/* Arena::remove: the slot goes to the head of the free list. */
static void bc_waker_remove(asx_broadcast_slot *s, uint32_t i) {
    s->wakers[i].task = ASX_INVALID_ID;
    s->wakers[i].next_free = s->wakers_free;
    s->wakers_free = i;
    s->wakers_live--;
}

/* The current task waits: register it (Arena::insert) unless it is. */
static void bc_waker_register(asx_broadcast_slot *s) {
    asx_task_id me = asx_task_current();
    uint32_t i;
    if (me == ASX_INVALID_ID || bc_waker_find(s, me) != BC_NIL) return;
    if (s->wakers_free != BC_NIL) {
        i = s->wakers_free;
        s->wakers_free = s->wakers[i].next_free;
    } else if (s->wakers_len < ASX_BROADCAST_MAX_RECEIVERS) {
        i = s->wakers_len++;
    } else {
        return; /* more waiters than receivers: woken in arrival order */
    }
    s->wakers[i].task = me;
    s->wakers_live++;
}

/* The current task stopped waiting (its recv resolved or was cancelled):
 * a registration still held is removed. */
static void bc_waker_leave(asx_broadcast_slot *s) {
    uint32_t i = bc_waker_find(s, asx_task_current());
    if (i != BC_NIL) bc_waker_remove(s, i);
    asx_wait_queue_leave_current(&s->waiters);
}

/* A task that finished while parked drops its recv: its registration is
 * removed, as Recv's drop does (broadcast.rs:1060-1071). */
static void broadcast_reap(asx_wait_queue *q) {
    uint32_t c;
    uint32_t i;
    for (c = 0; c < g_slot_count; c++) {
        asx_broadcast_slot *s = &g_slots[c];
        ASX_CHECKPOINT_WAIVER("bounded: ASX_MAX_BROADCASTS channels");
        if (q != &s->waiters) continue;
        for (i = 0; i < s->wakers_len; i++) {
            ASX_CHECKPOINT_WAIVER("bounded: ASX_BROADCAST_MAX_RECEIVERS waker slots");
            if (s->wakers[i].task != ASX_INVALID_ID &&
                asx_wait_task_liveness(s->wakers[i].task) == ASX_WAIT_TASK_DEAD) {
                bc_waker_remove(s, i);
            }
        }
        asx_wait_queue_drop_dead(q);
        return;
    }
}

/* New message or last sender gone: every parked receiver task re-polls,
 * in slot order. The drained slots join the free list in ascending order
 * ahead of the slots already free (arena.rs:555-572). */
static void broadcast_wake_waiters(asx_broadcast_slot *s) {
    uint32_t i;
    uint32_t last = BC_NIL;
    for (i = 0; i < s->wakers_len && s->wakers_live > 0u; i++) {
        asx_task_id task = s->wakers[i].task;
        ASX_CHECKPOINT_WAIVER("bounded: ASX_BROADCAST_MAX_RECEIVERS waker slots");
        if (task == ASX_INVALID_ID) continue;
        asx_wait_wake_task(task);
        s->wakers[i].task = ASX_INVALID_ID;
        if (last == BC_NIL) {
            s->wakers[i].next_free = s->wakers_free;
            s->wakers_free = i;
        } else {
            s->wakers[i].next_free = s->wakers[last].next_free;
            s->wakers[last].next_free = i;
        }
        last = i;
        s->wakers_live--;
    }
    /* Any waiter the arena did not hold (none in normal use). */
    if (s->waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->waiters);
}

/* The live slot a handle names, or NULL with *out_status set. */
static asx_broadcast_slot *broadcast_slot(uint32_t slot, uint16_t generation,
                                          asx_status *out_status) {
    if (slot >= g_slot_count) {
        *out_status = ASX_E_NOT_FOUND;
        return NULL;
    }
    if (g_slots[slot].generation != generation) {
        *out_status = ASX_E_STALE_HANDLE;
        return NULL;
    }
    *out_status = ASX_OK;
    return &g_slots[slot];
}

/* A live sender's slot: a dropped handle, or a channel whose senders are
 * all gone, is ASX_E_INVALID_STATE. */
static asx_broadcast_slot *broadcast_sender_slot(const asx_broadcast_sender *sender,
                                                 asx_status *out_status) {
    asx_broadcast_slot *s;
    if (sender == NULL) {
        *out_status = ASX_E_INVALID_ARGUMENT;
        return NULL;
    }
    s = broadcast_slot(sender->slot, sender->generation, out_status);
    if (s != NULL && (sender->dropped || s->sender_count == 0u)) {
        *out_status = ASX_E_INVALID_STATE;
        return NULL;
    }
    return s;
}

/* The messages the ring still owns, oldest first, are dropped: copied out
 * and handed to the discard callback, after which the ring owns none (so a
 * later overwrite of those slots drops nothing). A no-op without a
 * callback beyond that bookkeeping. */
static void broadcast_release_ring(asx_broadcast_slot *s) {
    uint64_t dropped[ASX_BROADCAST_MAX_CAPACITY];
    asx_broadcast_discard_fn fn = s->discard_fn;
    void *ctx = s->discard_ctx;
    uint64_t first = s->write_seq > s->capacity ? s->write_seq - s->capacity : 0u;
    uint64_t seq;
    uint32_t n = 0u;
    uint32_t i;

    if (first < s->live_from) first = s->live_from;
    for (seq = first; fn != NULL && seq < s->write_seq && n < ASX_BROADCAST_MAX_CAPACITY; seq++) {
        ASX_CHECKPOINT_WAIVER("bounded: at most the ring capacity of messages");
        dropped[n++] = s->buffer[seq % s->capacity];
    }
    s->live_from = s->write_seq;
    for (i = 0u; fn != NULL && i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: at most ASX_BROADCAST_MAX_CAPACITY messages");
        fn(ctx, dropped[i]);
    }
}

void asx_broadcast_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_BROADCASTS; i++) {
        /* What a live channel's ring still owns is dropped with it. */
        if (i < g_slot_count && g_slots[i].capacity > 0u) broadcast_release_ring(&g_slots[i]);
        g_slots[i].generation = next_gen(g_slots[i].generation);
        g_slots[i].sender_count = 0;
        g_slots[i].capacity = 0;
        g_slots[i].write_seq = 0;
        g_slots[i].live_from = 0;
        g_slots[i].discard_fn = NULL;
        g_slots[i].discard_ctx = NULL;
        g_slots[i].receiver_count = 0;
        memset(g_slots[i].buffer, 0, sizeof(g_slots[i].buffer));
        broadcast_waiters_init(&g_slots[i]);
    }
    g_slot_count = 0;
    memset(g_credits, 0, sizeof(g_credits));
    g_credit_live = 0u;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Whether any broadcast channel was created since the last reset. */
int asx_broadcast_in_use_internal(void) { return g_slot_count != 0u; }

asx_status asx_broadcast_create(uint32_t capacity, asx_broadcast_sender *out_sender,
                                asx_broadcast_receiver *out_receiver) {
    uint32_t idx;
    asx_broadcast_slot *s;

    if (out_sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    if (capacity == 0 || capacity > ASX_BROADCAST_MAX_CAPACITY) return ASX_E_INVALID_ARGUMENT;
    if (asx_runtime_shared_state_refused_internal("broadcasts")) return ASX_E_INVALID_STATE;

    /* Find free slot */
    idx = ASX_MAX_BROADCASTS;
    {
        uint32_t i;
        for (i = 0; i < g_slot_count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
            if (g_slots[i].sender_count == 0u && g_slots[i].receiver_count == 0u) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_BROADCASTS) {
        if (g_slot_count >= ASX_MAX_BROADCASTS) return ASX_E_RESOURCE_EXHAUSTED;
        idx = g_slot_count++;
    }

    s = &g_slots[idx];
    s->generation = next_gen(s->generation);
    s->sender_count = 1;
    s->capacity = capacity;
    s->write_seq = 0;
    s->live_from = 0;
    s->discard_fn = NULL;
    s->discard_ctx = NULL;
    s->receiver_count = 1;
    memset(s->buffer, 0, sizeof(s->buffer));
    broadcast_waiters_init(s);

    out_sender->slot = idx;
    out_sender->generation = s->generation;
    out_sender->dropped = 0;
    out_receiver->slot = idx;
    out_receiver->generation = s->generation;
    out_receiver->dropped = 0;
    out_receiver->cursor = 0;

    return ASX_OK;
}

asx_status asx_broadcast_subscribe(const asx_broadcast_sender *sender,
                                   asx_broadcast_receiver *out_receiver) {
    asx_broadcast_slot *s;
    asx_status st;

    if (out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    s = broadcast_sender_slot(sender, &st);
    if (s == NULL) return st;
    if (s->receiver_count >= ASX_BROADCAST_MAX_RECEIVERS) return ASX_E_RESOURCE_EXHAUSTED;

    s->receiver_count++;
    out_receiver->slot = sender->slot;
    out_receiver->generation = s->generation;
    out_receiver->dropped = 0;
    out_receiver->cursor = s->write_seq; /* start from next message */

    return ASX_OK;
}

asx_status asx_broadcast_sender_clone(const asx_broadcast_sender *sender,
                                      asx_broadcast_sender *out_sender) {
    asx_broadcast_slot *s;
    asx_status st;

    if (out_sender == NULL) return ASX_E_INVALID_ARGUMENT;
    s = broadcast_sender_slot(sender, &st);
    if (s == NULL) return st;
    if (s->sender_count == UINT32_MAX) return ASX_E_RESOURCE_EXHAUSTED;

    s->sender_count++;
    out_sender->slot = sender->slot;
    out_sender->generation = s->generation;
    out_sender->dropped = 0;
    return ASX_OK;
}

asx_status asx_broadcast_receiver_clone(const asx_broadcast_receiver *receiver,
                                        asx_broadcast_receiver *out_receiver) {
    asx_broadcast_slot *s;
    asx_status st;

    if (receiver == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    s = broadcast_slot(receiver->slot, receiver->generation, &st);
    if (s == NULL) return st;
    if (receiver->dropped) return ASX_E_INVALID_STATE;
    if (s->receiver_count >= ASX_BROADCAST_MAX_RECEIVERS) return ASX_E_RESOURCE_EXHAUSTED;

    s->receiver_count++;
    out_receiver->slot = receiver->slot;
    out_receiver->generation = s->generation;
    out_receiver->dropped = 0;
    out_receiver->cursor = receiver->cursor; /* the same remaining messages */
    return ASX_OK;
}

void asx_broadcast_sender_drop(asx_broadcast_sender *sender) {
    asx_broadcast_slot *s;
    asx_status st;
    if (sender == NULL || sender->dropped) return;
    s = broadcast_slot(sender->slot, sender->generation, &st);
    if (s == NULL || s->sender_count == 0u) return;
    sender->dropped = 1;
    s->sender_count--;
    /* The last sender closes the channel: parked receivers re-poll. */
    if (s->sender_count == 0u) broadcast_wake_waiters(s);
}

void asx_broadcast_receiver_drop(asx_broadcast_receiver *receiver) {
    asx_broadcast_slot *s;
    asx_status st;
    if (receiver == NULL || receiver->dropped) return;
    s = broadcast_slot(receiver->slot, receiver->generation, &st);
    if (s == NULL) return;
    receiver->dropped = 1;
    if (s->receiver_count > 0) s->receiver_count--;
    /* The last receiver takes the whole buffer with it (broadcast.rs:1089). */
    if (s->receiver_count == 0u) broadcast_release_ring(s);
}

asx_status asx_broadcast_set_discard(const asx_broadcast_sender *sender,
                                     asx_broadcast_discard_fn fn, void *ctx) {
    asx_broadcast_slot *s;
    asx_status st;

    s = broadcast_sender_slot(sender, &st);
    if (s == NULL) return st;
    s->discard_fn = fn;
    s->discard_ctx = fn != NULL ? ctx : NULL;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */

/* A broadcast permit holds an identity credit, but no ring space. When
 * its holder completes, the runtime aborts its tracked obligation with Cancel,
 * as the Rust permit's drop does (broadcast.rs:624-630). */
static int broadcast_permit_drop(uint64_t a, uint64_t b) {
    if (a < ASX_MAX_OBLIGATIONS && g_credits[(uint32_t)a].serial == (uint32_t)b) {
        g_credits[(uint32_t)a].serial = 0u;
        g_credit_live--;
    }
    return 0;
}

void asx_broadcast_task_finished_internal(asx_task_id task) {
    uint32_t i;
    if (g_credit_live == 0u) return;
    for (i = 0; i < ASX_MAX_OBLIGATIONS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_MAX_OBLIGATIONS physical broadcast credits");
        if (g_credits[i].serial != 0u && asx_wait_same_task(g_credits[i].holder, task)) {
            g_credits[i].serial = 0u;
            g_credit_live--;
        }
    }
}

asx_status asx_broadcast_reserve(const asx_broadcast_sender *sender, asx_cx *cx,
                                 asx_broadcast_permit *out_permit) {
    asx_broadcast_slot *s;
    asx_status st;
    asx_obligation_id ob = ASX_INVALID_ID;
    uint32_t credit;
    uint32_t serial;

    if (out_permit == NULL) return ASX_E_INVALID_ARGUMENT;
    s = broadcast_sender_slot(sender, &st);
    if (s == NULL) return st;

    /* Cancellation, then a receiver to send to (broadcast.rs:409-432). */
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) return ASX_E_CANCELLED;
    if (s->receiver_count == 0u) return ASX_E_DISCONNECTED;
    if (g_permit_serial == UINT32_MAX) return ASX_E_RESOURCE_EXHAUSTED;
    for (credit = 0; credit < ASX_MAX_OBLIGATIONS; credit++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_MAX_OBLIGATIONS physical broadcast credits");
        if (g_credits[credit].serial == 0u) break;
    }
    if (credit == ASX_MAX_OBLIGATIONS) return ASX_E_RESOURCE_EXHAUSTED;
    /* Claim before registration can synchronously notify a trace observer. */
    serial = ++g_permit_serial;
    g_credits[credit].serial = serial;
    g_credits[credit].slot = sender->slot;
    g_credits[credit].generation = s->generation;
    g_credits[credit].holder = cx != NULL ? cx->task_id : ASX_INVALID_ID;
    g_credit_live++;
    if (cx != NULL && cx->task_id != ASX_INVALID_ID &&
        asx_obligation_register(cx->region_id, ASX_OBLIGATION_KIND_SEND_PERMIT, cx->task_id, &ob) ==
            ASX_OK) {
        asx_obligation_set_drop_internal(ob, broadcast_permit_drop, (uint64_t)credit,
                                         (uint64_t)serial, serial);
    } else {
        ob = ASX_INVALID_ID; /* untracked, as a refused Rust registration */
    }

    out_permit->slot = sender->slot;
    out_permit->generation = s->generation;
    out_permit->consumed = 0;
    out_permit->obligation = ob;
    out_permit->serial = serial;
    return ASX_OK;
}

asx_status asx_broadcast_permit_send(asx_broadcast_permit *permit, uint64_t value,
                                     uint32_t *out_receivers) {
    asx_broadcast_slot *s;
    asx_status st;
    uint64_t overwritten = 0u;
    int overwrote = 0;
    uint32_t ring_idx;
    uint32_t credit;

    if (out_receivers != NULL) *out_receivers = 0u;
    if (permit == NULL) return ASX_E_INVALID_ARGUMENT;
    if (permit->consumed) return ASX_E_INVALID_STATE;
    st = broadcast_credit(permit, &credit);
    if (st != ASX_OK) return st;
    if (permit->obligation != ASX_INVALID_ID) {
        st = asx_obligation_check_token_internal(permit->obligation, broadcast_permit_drop,
                                                 (uint64_t)credit, (uint64_t)permit->serial,
                                                 permit->serial);
        /* A live tracked registration must also authenticate the credit. */
        if (st != ASX_OK) return st;
    }
    permit->consumed = 1;
    g_credits[credit].serial = 0u;
    g_credit_live--;

    s = broadcast_slot(permit->slot, permit->generation, &st);
    if (s == NULL || s->sender_count == 0u) {
        /* No sender left to have borrowed: the permit is dropped unsent. */
        if (permit->obligation != ASX_INVALID_ID) {
            st = asx_obligation_resolve_token_internal(permit->obligation, broadcast_permit_drop,
                                                       (uint64_t)credit, (uint64_t)permit->serial,
                                                       permit->serial, ASX_OBLIGATION_ABORTED,
                                                       ASX_OBLIGATION_ABORT_CANCEL);
            (void)st; /* only an already-resolved obligation refuses */
            permit->obligation = ASX_INVALID_ID;
        }
        return s == NULL ? ASX_E_STALE_HANDLE : ASX_E_INVALID_STATE;
    }

    /* The send commits first, even with no receiver left (:715-719). */
    if (permit->obligation != ASX_INVALID_ID) {
        st = asx_obligation_resolve_token_internal(permit->obligation, broadcast_permit_drop,
                                                   (uint64_t)credit, (uint64_t)permit->serial,
                                                   permit->serial, ASX_OBLIGATION_COMMITTED,
                                                   ASX_OBLIGATION_ABORT_EXPLICIT);
        (void)st; /* only an already-resolved obligation refuses */
        permit->obligation = ASX_INVALID_ID;
    }
    /* Every receiver dropped since the reserve: nothing is published, and
     * the message is dropped (broadcast.rs:730-738). */
    if (s->receiver_count == 0u) {
        if (s->discard_fn != NULL) s->discard_fn(s->discard_ctx, value);
        return ASX_OK;
    }

    /* Write into ring buffer (overwrites old messages): the one it takes
     * the place of, if the ring still owns it, falls off the ring. */
    ring_idx = (uint32_t)(s->write_seq % s->capacity);
    if (s->write_seq >= s->capacity && s->write_seq - s->capacity >= s->live_from) {
        overwritten = s->buffer[ring_idx];
        overwrote = 1;
    }
    s->buffer[ring_idx] = value;
    s->write_seq++;

    asx_trace_emit_ctx_internal(ASX_TRACE_CHANNEL_SEND, (uint64_t)permit->slot, value);
    broadcast_wake_waiters(s);

    if (out_receivers != NULL) *out_receivers = s->receiver_count;
    if (overwrote && s->discard_fn != NULL) s->discard_fn(s->discard_ctx, overwritten);
    return ASX_OK;
}

void asx_broadcast_permit_abort(asx_broadcast_permit *permit) {
    asx_status st;
    uint32_t credit;
    if (permit == NULL || permit->consumed) return;
    st = broadcast_credit(permit, &credit);
    if (st != ASX_OK) return;
    if (permit->obligation != ASX_INVALID_ID) {
        st = asx_obligation_check_token_internal(permit->obligation, broadcast_permit_drop,
                                                 (uint64_t)credit, (uint64_t)permit->serial,
                                                 permit->serial);
        if (st != ASX_OK) return;
    }
    permit->consumed = 1;
    g_credits[credit].serial = 0u;
    g_credit_live--;
    if (permit->obligation != ASX_INVALID_ID) {
        st = asx_obligation_resolve_token_internal(permit->obligation, broadcast_permit_drop,
                                                   (uint64_t)credit, (uint64_t)permit->serial,
                                                   permit->serial, ASX_OBLIGATION_ABORTED,
                                                   ASX_OBLIGATION_ABORT_CANCEL);
        (void)st; /* only an already-resolved obligation refuses */
        permit->obligation = ASX_INVALID_ID;
    }
}

asx_status asx_broadcast_send(const asx_broadcast_sender *sender, asx_cx *cx, uint64_t value,
                              uint32_t *out_receivers) {
    asx_broadcast_permit permit;
    asx_status st;

    if (out_receivers != NULL) *out_receivers = 0u;
    st = asx_broadcast_reserve(sender, cx, &permit);
    if (st != ASX_OK) return st;
    return asx_broadcast_permit_send(&permit, value, out_receivers);
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

/* Receive the next value. Nothing new: a waiting receive
 * (asx_broadcast_recv) parks the calling task until a send or the sender
 * drops; try_recv never parks (Rust try_recv registers no waker). */
static asx_status broadcast_recv_impl(asx_broadcast_receiver *receiver, uint64_t *out_value,
                                      int park) {
    asx_broadcast_slot *s;
    asx_status st;
    uint64_t oldest_available;
    uint32_t ring_idx;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    s = broadcast_slot(receiver->slot, receiver->generation, &st);
    if (s == NULL) return st;
    if (receiver->dropped) return ASX_E_INVALID_STATE;

    /* Nothing to read */
    if (receiver->cursor >= s->write_seq) {
        if (s->sender_count == 0u) {
            bc_waker_leave(s);
            return ASX_E_DISCONNECTED;
        }
        if (park) {
            bc_waker_register(s);
            (void)asx_wait_queue_park_current(&s->waiters);
        }
        return ASX_E_WOULD_BLOCK;
    }

    /* Something to report: the caller's wait (if any) is over. */
    bc_waker_leave(s);

    /* Check for lag: cursor too far behind write_seq */
    oldest_available = 0;
    if (s->write_seq > s->capacity) { oldest_available = s->write_seq - s->capacity; }

    if (receiver->cursor < oldest_available) {
        /* Receiver has lagged — report how far, advance cursor to oldest
         * available */
        *out_value = oldest_available - receiver->cursor;
        receiver->cursor = oldest_available;
        return ASX_E_LAGGED;
    }

    /* Read the value */
    ring_idx = (uint32_t)(receiver->cursor % s->capacity);
    *out_value = s->buffer[ring_idx];
    receiver->cursor++;

    asx_trace_emit_ctx_internal(ASX_TRACE_CHANNEL_RECV, (uint64_t)receiver->slot, *out_value);

    return ASX_OK;
}

asx_status asx_broadcast_try_recv(asx_broadcast_receiver *receiver, uint64_t *out_value) {
    return broadcast_recv_impl(receiver, out_value, 0);
}

asx_status asx_broadcast_recv(asx_broadcast_receiver *receiver, asx_cx *cx, uint64_t *out_value) {
    asx_status st;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    if (receiver->slot >= g_slot_count) return ASX_E_NOT_FOUND;
    if (g_slots[receiver->slot].generation != receiver->generation) return ASX_E_STALE_HANDLE;

    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        bc_waker_leave(&g_slots[receiver->slot]);
        asx_trace_user(cx->task_id, "broadcast::recv cancelled");
        return ASX_E_CANCELLED;
    }
    st = broadcast_recv_impl(receiver, out_value, 1);
    return st == ASX_E_WOULD_BLOCK ? ASX_E_PENDING : st;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_broadcast_receiver_count(const asx_broadcast_sender *sender) {
    if (sender == NULL) return 0;
    if (sender->slot >= g_slot_count) return 0;
    if (g_slots[sender->slot].generation != sender->generation) return 0;
    return g_slots[sender->slot].receiver_count;
}

uint64_t asx_broadcast_total_sent(const asx_broadcast_sender *sender) {
    if (sender == NULL) return 0;
    if (sender->slot >= g_slot_count) return 0;
    if (g_slots[sender->slot].generation != sender->generation) return 0;
    return g_slots[sender->slot].write_seq;
}
