/*
 * watch.c — single-value observable state channel
 *
 * Walking skeleton: fixed-size arena per runtime kernel, single-threaded.
 *
 * Wake-driven waiting: asx_watch_poll_changed() reporting ASX_E_PENDING
 * inside a scheduler poll parks the calling task; every publish and the
 * sender drop wake all parked tasks (FIFO), since every receiver observes
 * every new version.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include "../sync/wait_queue.h"
#include <asx/core/watch.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/* The arenas are the kernels' (asx_kernel.watches, channel_state.h): the
 * slot field of a handle holds the index in its low half and the tag of the
 * runtime that issued it above. The kernel a field names is where the
 * channel's trace events go, whichever task is polled; NULL when no runtime
 * holds the tag. */
static asx_kernel *watch_kernel(uint32_t field) {
    return asx_kernel_for_tag((uint16_t)(field >> ASX_SYNC_SLOT_TAG_SHIFT));
}

/* The slot a handle names, or why it names none: NOT_FOUND for a tag no
 * runtime holds or an index past the slots created, STALE_HANDLE for a
 * recycled slot. */
static asx_status watch_lookup(uint32_t field, uint16_t generation, asx_watch_slot **out) {
    asx_kernel *k = watch_kernel(field);
    uint32_t idx = field & ASX_SYNC_SLOT_INDEX_MASK;
    if (k == NULL || idx >= k->watches.count) return ASX_E_NOT_FOUND;
    if (k->watches.slots[idx].generation != generation) return ASX_E_STALE_HANDLE;
    *out = &k->watches.slots[idx];
    return ASX_OK;
}

static uint16_t next_gen(uint16_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void watch_waiters_init(asx_watch_slot *s, uint16_t tag) {
    asx_wait_queue_init(&s->waiters, NULL, tag);
}

/* New version or sender gone: every parked receiver task re-polls. */
static void watch_wake_waiters(asx_watch_slot *s) {
    if (s->waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->waiters);
}

/* The channel has no sender and no receiver left (Rust: the last Arc is
 * dropped): its last value is dropped, once. */
static void watch_release_if_dead(asx_watch_slot *s) {
    if (s->sender_alive || s->receiver_count != 0u || s->waiter_clones != 0u || s->released) return;
    s->released = 1;
    if (s->discard_fn != NULL) s->discard_fn(s->discard_ctx, s->value);
}

void asx_watch_reset_internal(asx_kernel *k) {
    asx_watch_arena *a = &k->watches;
    uint32_t i;
    for (i = 0; i < ASX_MAX_WATCHES; i++) {
        asx_watch_discard_fn discard_fn = NULL;
        void *discard_ctx = NULL;
        uint64_t value = a->slots[i].value;
        /* A live channel's value is dropped with it. */
        if (i < a->count && !a->slots[i].released && a->slots[i].discard_fn != NULL &&
            (a->slots[i].sender_alive || a->slots[i].receiver_count != 0u ||
             a->slots[i].waiter_clones != 0u)) {
            discard_fn = a->slots[i].discard_fn;
            discard_ctx = a->slots[i].discard_ctx;
        }
        a->slots[i].generation = next_gen(a->slots[i].generation);
        a->slots[i].sender_alive = 0;
        a->slots[i].value = 0;
        a->slots[i].version = 0;
        a->slots[i].receiver_count = 0;
        a->slots[i].waiter_clones = 0;
        a->slots[i].discard_fn = NULL;
        a->slots[i].discard_ctx = NULL;
        a->slots[i].released = 0;
        watch_waiters_init(&a->slots[i], k->tag);
        if (discard_fn != NULL) discard_fn(discard_ctx, value);
    }
    a->count = 0;
}

void asx_watch_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_watch_reset_internal(k);
}

asx_status asx_watch_set_discard(const asx_watch_sender *sender, asx_watch_discard_fn fn,
                                 void *ctx) {
    asx_watch_slot *s;
    asx_status st;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_lookup(sender->slot, sender->generation, &s);
    if (st != ASX_OK) return st;
    s->discard_fn = fn;
    s->discard_ctx = fn != NULL ? ctx : NULL;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_watch_create(const asx_runtime *rt, uint64_t initial_value,
                            asx_watch_sender *out_sender, asx_watch_receiver *out_receiver) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    asx_watch_arena *a;
    uint32_t idx;
    uint32_t field;
    asx_watch_slot *s;

    if (rt == NULL || out_sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    if (k == NULL) return ASX_E_INVALID_STATE;
    a = &k->watches;

    /* Find free slot */
    idx = ASX_MAX_WATCHES;
    {
        uint32_t i;
        for (i = 0; i < a->count; i++) {
            /* ASX_CHECKPOINT_WAIVER("bounded slot search") */
            if (!a->slots[i].sender_alive && a->slots[i].receiver_count == 0 &&
                a->slots[i].waiter_clones == 0) {
                idx = i;
                break;
            }
        }
    }

    if (idx == ASX_MAX_WATCHES) {
        if (a->count >= ASX_MAX_WATCHES) return ASX_E_RESOURCE_EXHAUSTED;
        idx = a->count++;
    }

    s = &a->slots[idx];
    s->generation = next_gen(s->generation);
    s->sender_alive = 1;
    s->value = initial_value;
    /* Version 0 holds the initial value and the first receiver has seen
     * it: only later sends count as changes (Rust channel/watch.rs). */
    s->version = 0;
    s->receiver_count = 1;
    s->waiter_clones = 0;
    s->discard_fn = NULL;
    s->discard_ctx = NULL;
    s->released = 0;
    watch_waiters_init(s, k->tag);

    field = ((uint32_t)k->tag << ASX_SYNC_SLOT_TAG_SHIFT) | idx;
    out_sender->slot = field;
    out_sender->generation = s->generation;
    out_receiver->slot = field;
    out_receiver->generation = s->generation;
    out_receiver->last_seen_version = 0;

    return ASX_OK;
}

asx_status asx_watch_subscribe(const asx_watch_sender *sender, asx_watch_receiver *out_receiver) {
    asx_watch_slot *s;
    asx_status st;

    if (sender == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_lookup(sender->slot, sender->generation, &s);
    if (st != ASX_OK) return st;
    if (!s->sender_alive) return ASX_E_DISCONNECTED;
    if (s->receiver_count >= ASX_WATCH_MAX_RECEIVERS) return ASX_E_RESOURCE_EXHAUSTED;

    s->receiver_count++;
    out_receiver->slot = sender->slot;
    out_receiver->generation = s->generation;
    /* A new subscriber has seen the current version and observes only
     * later changes (Rust Sender::subscribe). */
    out_receiver->last_seen_version = s->version;

    return ASX_OK;
}

/* The slot a receiver handle names, or why it names none. */
static asx_status watch_receiver_slot(const asx_watch_receiver *receiver, asx_watch_slot **out) {
    return watch_lookup(receiver->slot, receiver->generation, out);
}

/* Rust's Receiver::clone (watch.rs:1057): the new receiver starts at the
 * version this one has seen, not at the current one. */
asx_status asx_watch_receiver_clone(const asx_watch_receiver *receiver,
                                    asx_watch_receiver *out_receiver) {
    asx_watch_slot *s;
    asx_status st;

    if (receiver == NULL || out_receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_receiver_slot(receiver, &s);
    if (st != ASX_OK) return st;
    if (s->receiver_count >= ASX_WATCH_MAX_RECEIVERS) return ASX_E_RESOURCE_EXHAUSTED;

    s->receiver_count++;
    out_receiver->slot = receiver->slot;
    out_receiver->generation = s->generation;
    out_receiver->last_seen_version = receiver->last_seen_version;
    return ASX_OK;
}

void asx_watch_sender_drop(asx_watch_sender *sender) {
    asx_watch_slot *s;
    if (sender == NULL) return;
    if (watch_lookup(sender->slot, sender->generation, &s) != ASX_OK) return;
    s->sender_alive = 0;
    watch_wake_waiters(s);
    watch_release_if_dead(s);
}

void asx_watch_receiver_drop(asx_watch_receiver *receiver) {
    asx_watch_slot *s;
    if (receiver == NULL) return;
    if (watch_lookup(receiver->slot, receiver->generation, &s) != ASX_OK) return;
    if (s->receiver_count > 0) s->receiver_count--;
    watch_release_if_dead(s);
}

/* ------------------------------------------------------------------ */
/* Send                                                                */
/* ------------------------------------------------------------------ */

asx_status asx_watch_send(asx_watch_sender *sender, uint64_t value) {
    asx_watch_slot *s;
    asx_status st;
    uint64_t replaced;

    if (sender == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_lookup(sender->slot, sender->generation, &s);
    if (st != ASX_OK) return st;
    if (!s->sender_alive) return ASX_E_INVALID_STATE;

    replaced = s->value;
    s->value = value;
    s->version++;

    asx_trace_emit_internal(watch_kernel(sender->slot), ASX_TRACE_CHANNEL_SEND,
                            (uint64_t)sender->slot, value);
    watch_wake_waiters(s);
    /* The value this one replaced is dropped (watch.rs:497-503); a token
     * sent again is the same value, not a replacement. */
    if (s->discard_fn != NULL && replaced != value) s->discard_fn(s->discard_ctx, replaced);

    return ASX_OK;
}

/* Rust's send_modify (watch.rs:530): clone, apply, write back as one new
 * version, whether or not the closure changed the value. */
asx_status asx_watch_send_modify(asx_watch_sender *sender, asx_watch_modify_fn modify, void *ctx) {
    asx_watch_slot *s;
    asx_status st;
    uint64_t replaced;
    uint64_t value;

    if (sender == NULL || modify == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_lookup(sender->slot, sender->generation, &s);
    if (st != ASX_OK) return st;
    if (!s->sender_alive) return ASX_E_INVALID_STATE;

    value = s->value;
    replaced = value;
    modify(ctx, &value);
    s->value = value;
    s->version++;

    asx_trace_emit_internal(watch_kernel(sender->slot), ASX_TRACE_CHANNEL_SEND,
                            (uint64_t)sender->slot, value);
    watch_wake_waiters(s);
    /* The written-back value replaces the old one (watch.rs:567); a closure
     * that left the token as it was replaced nothing. */
    if (s->discard_fn != NULL && replaced != value) s->discard_fn(s->discard_ctx, replaced);

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

asx_status asx_watch_recv(asx_watch_receiver *receiver, uint64_t *out_value) {
    asx_watch_slot *s;
    asx_status st;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_receiver_slot(receiver, &s);
    if (st != ASX_OK) return st;

    *out_value = s->value;
    receiver->last_seen_version = s->version;

    return ASX_OK;
}

/* Rust's borrow (watch.rs:904): the value, with the receiver's seen version
 * left alone. */
asx_status asx_watch_borrow(const asx_watch_receiver *receiver, uint64_t *out_value) {
    asx_watch_slot *s;
    asx_status st;

    if (receiver == NULL || out_value == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_receiver_slot(receiver, &s);
    if (st != ASX_OK) return st;

    *out_value = s->value;
#ifdef ASX_MUTANT_WATCH_BORROW_MARKS_SEEN
    /* Planted bug, compiled only by tests/gates/test_oracle_mutants.sh: a
     * borrow marks the version seen, as borrow_and_update does. */
    ((asx_watch_receiver *)receiver)->last_seen_version = s->version;
#endif
    return ASX_OK;
}

/* Rust's mark_seen (watch.rs:961): the current version, whatever was read
 * before. */
asx_status asx_watch_mark_seen(asx_watch_receiver *receiver) {
    asx_watch_slot *s;
    asx_status st;

    if (receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_receiver_slot(receiver, &s);
    if (st != ASX_OK) return st;

    receiver->last_seen_version = s->version;
    return ASX_OK;
}

int asx_watch_receiver_is_closed(const asx_watch_receiver *receiver) {
    asx_watch_slot *s;

    if (receiver == NULL || watch_receiver_slot(receiver, &s) != ASX_OK) return 1;
    return !s->sender_alive;
}

int asx_watch_sender_is_closed(const asx_watch_sender *sender) {
    asx_watch_slot *s;

    if (sender == NULL || watch_lookup(sender->slot, sender->generation, &s) != ASX_OK) return 1;
    return s->receiver_count + s->waiter_clones == 0u;
}

int asx_watch_has_changed(const asx_watch_receiver *receiver) {
    asx_watch_slot *s;

    if (receiver == NULL || watch_receiver_slot(receiver, &s) != ASX_OK) return 0;
    return s->version != receiver->last_seen_version;
}

static void watch_trace(const asx_cx *cx, const char *message) {
    if (cx != NULL) asx_trace_user(cx->task_id, message);
}

void asx_watch_changed_begin(const asx_watch_receiver *receiver, asx_cx *cx) {
    if (receiver == NULL) return;
    watch_trace(cx, "watch::changed starting wait");
}

asx_status asx_watch_poll_changed(asx_watch_receiver *receiver, asx_cx *cx) {
    asx_watch_slot *s;
    asx_status st;

    if (receiver == NULL) return ASX_E_INVALID_ARGUMENT;
    st = watch_receiver_slot(receiver, &s);
    if (st != ASX_OK) return st;

    /* Rust's poll_changed order: cancellation, a newer version, the sender
     * gone (watch.rs:735-761). */
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        asx_wait_queue_leave_current(&s->waiters);
        watch_trace(cx, "watch::changed cancelled");
        return ASX_E_CANCELLED;
    }

    if (s->version != receiver->last_seen_version) {
        receiver->last_seen_version = s->version;
        asx_wait_queue_leave_current(&s->waiters);
        watch_trace(cx, "watch::changed received update");
        return ASX_OK;
    }

    if (!s->sender_alive) {
        asx_wait_queue_leave_current(&s->waiters);
        watch_trace(cx, "watch::changed sender dropped");
        return ASX_E_DISCONNECTED;
    }

    (void)asx_wait_queue_park_current(&s->waiters);
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* wait_for                                                            */
/* ------------------------------------------------------------------ */

/* One poll of the changed() wait wait_for opens on its clone (watch.rs:731),
 * which has seen `wait->checked`: cancellation, then a newer version (the
 * open wait ends: wait_for loops to the predicate), then the sender gone. */
static asx_status watch_wait_poll(asx_watch_slot *s, asx_cx *cx, asx_watch_wait *wait) {
    if (cx != NULL && asx_cx_checkpoint(cx) != ASX_OK) {
        watch_trace(cx, "watch::changed cancelled");
        return ASX_E_CANCELLED;
    }
    if (s->version != wait->checked) {
        watch_trace(cx, "watch::changed received update");
        wait->waiting = 0;
        return ASX_OK;
    }
    if (!s->sender_alive) {
        watch_trace(cx, "watch::changed sender dropped");
        return ASX_E_DISCONNECTED;
    }
    (void)asx_wait_queue_park_current(&s->waiters);
    return ASX_E_PENDING;
}

/* wait_for returns: its clone is dropped and the task leaves the wait line. */
static void watch_wait_end(asx_watch_slot *s, asx_watch_wait *wait) {
    if (wait->cloned && s->waiter_clones > 0u) s->waiter_clones--;
    asx_wait_queue_leave_current(&s->waiters);
    memset(wait, 0, sizeof(*wait));
}

/* Rust's async wait_for (watch.rs:858) as a poll. Its loop head tests the
 * predicate on the current value under no cancellation check; a rejection
 * opens changed() on a clone that has seen the rejected version. A poll
 * resumes inside that wait, and a newer version returns to the loop head.
 * After a rejection the clone has seen the current version, so its first
 * poll cannot report a newer one: this takes at most two steps per call. */
asx_status asx_watch_poll_wait_for(asx_watch_receiver *receiver, asx_cx *cx, asx_watch_wait *wait,
                                   asx_watch_predicate_fn predicate, void *ctx,
                                   uint64_t *out_value) {
    asx_watch_slot *s;
    asx_status st;

    if (receiver == NULL || wait == NULL || predicate == NULL || out_value == NULL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    st = watch_receiver_slot(receiver, &s);
    if (st != ASX_OK) return st;

    if (wait->waiting) {
        st = watch_wait_poll(s, cx, wait);
        if (st == ASX_E_PENDING) return st;
        if (st != ASX_OK) {
            watch_wait_end(s, wait);
            return st;
        }
    }

    if (predicate(ctx, s->value)) {
        receiver->last_seen_version = s->version;
        *out_value = s->value;
        watch_wait_end(s, wait);
        return ASX_OK;
    }

    if (!wait->cloned) {
        wait->cloned = 1;
        s->waiter_clones++;
    }
    wait->checked = s->version;
    wait->waiting = 1;
    watch_trace(cx, "watch::changed starting wait");

    st = watch_wait_poll(s, cx, wait);
    if (st != ASX_E_PENDING) watch_wait_end(s, wait);
    return st;
}

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

uint32_t asx_watch_receiver_count(const asx_watch_sender *sender) {
    asx_watch_slot *s;
    if (sender == NULL || watch_lookup(sender->slot, sender->generation, &s) != ASX_OK) return 0;
    return s->receiver_count + s->waiter_clones;
}

int asx_watch_sender_is_alive(uint32_t slot, uint16_t generation) {
    asx_watch_slot *s;
    if (watch_lookup(slot, generation, &s) != ASX_OK) return 0;
    return s->sender_alive;
}
