/*
 * mpsc.c — bounded MPSC two-phase channel implementation (bd-2cw.6)
 *
 * CORE builds use a deterministic single-thread ring buffer. Live
 * POSIX/PARALLEL builds can use the atomic committed-message backend
 * while preserving the same reserve/send/abort contract.
 *
 * Two-phase protocol: try_reserve claims capacity and returns a permit;
 * send (via permit) enqueues the value FIFO, or abort returns the capacity.
 *
 * Capacity invariant: queue_len + reserved_count <= capacity
 *
 * Wake-driven waiting: inside a scheduler poll, the waiting operations
 * (recv on empty, reserve / send when full) park the calling task in the
 * channel's FIFO recv / reserve wait queue; try_recv and try_reserve never
 * park, as Rust's try_* register no waker (bd-vc1v). After every state
 * change the queues are settled: each committed message is owed to one
 * parked receiver, and free capacity to the head of the reserve line. As
 * upstream, no reserve jumps the line: with a live producer parked ahead
 * it reports FULL even if capacity is free. Closing either side wakes
 * everyone. asx_channel_wait_cancel() withdraws a task that stops waiting
 * and passes any wake it held on. Outside a scheduler poll nothing parks.
 *
 * Semantics specified in docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md.
 *
 * ASX_PROOF_BLOCK_WAIVER("reason: bug fix for recv after close, no semantic break")
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include "../sync/wait_queue.h"
#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/platform/atomics.h>
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/profile_compat.h>
#include <asx/runtime/runtime.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal channel slot                                              */
/* ------------------------------------------------------------------ */

/* The slot type and the arena are the kernels' (asx_kernel.channels,
 * channel/channel_state.h): a channel id carries the runtime tag of the
 * region it was created in. */
static uint32_t channel_discard_queue(asx_channel_slot *s, uint64_t *out);
static void channel_run_discard(asx_channel_slot *s, const uint64_t *values, uint32_t n);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static asx_status channel_slot_lookup(asx_channel_id id, asx_channel_slot **out) {
    uint16_t slot_idx;
    uint16_t gen;
    asx_kernel *k;
    asx_channel_slot *s;

    if (!asx_handle_is_valid(id)) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_handle_type_tag(id) != ASX_TYPE_CHANNEL) { return ASX_E_INVALID_ARGUMENT; }

    slot_idx = asx_handle_slot(id);
    gen = asx_handle_generation(id);

    k = asx_kernel_of(id);
    if (k == NULL || slot_idx >= ASX_MAX_CHANNELS) { return ASX_E_NOT_FOUND; }

    s = &k->channels.slots[slot_idx];
    if (!s->alive) { return ASX_E_NOT_FOUND; }
    if (s->generation != gen) { return ASX_E_STALE_HANDLE; }

    *out = s;
    return ASX_OK;
}

/* The id of slot `slot_idx` of kernel `k`: its runtime tag in the handle. */
static asx_channel_id channel_make_handle(const asx_kernel *k, uint16_t slot_idx, uint16_t gen) {
    uint32_t index = asx_handle_pack_index(gen, slot_idx);
    return asx_handle_with_runtime_tag(asx_handle_pack(ASX_TYPE_CHANNEL, 0, index), k->tag);
}

/* A channel starts with its creator's sender and no clone. */
static void channel_sender_ids_reset(asx_channel_slot *s) {
    uint32_t i;

    s->primary_dropped = 0;
    s->next_sender_token = 1u;
    for (i = 0; i < ASX_CHANNEL_MAX_SENDERS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_SENDERS constant");
        s->sender_tokens[i] = 0u;
    }
}

static int channel_sender_token_live(const asx_channel_slot *s, uint32_t token) {
    uint32_t i;

    for (i = 0; i < ASX_CHANNEL_MAX_SENDERS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_SENDERS constant");
        if (s->sender_tokens[i] == token) { return 1; }
    }
    return 0;
}

static uint32_t channel_atomic_load(const asx_atomic_u32 *value) {
    return asx_atomic_u32_load(value);
}

static void channel_atomic_store(asx_atomic_u32 *value, uint32_t next) {
    asx_atomic_u32_store(value, next);
}

static void channel_atomic_inc(asx_atomic_u32 *value) { (void)asx_atomic_u32_fetch_add(value, 1u); }

static int channel_atomic_dec(asx_atomic_u32 *value) {
    uint32_t observed;
    uint32_t expected;

    observed = asx_atomic_u32_load(value);
    while (observed > 0u) {
        ASX_CHECKPOINT_WAIVER("bounded: atomic decrement retries only under producer contention");
        expected = observed;
        if (asx_atomic_u32_compare_exchange(value, &expected, observed - 1u)) { return 1; }
        observed = expected;
    }

    return 0;
}

static void channel_token_clear_all(asx_channel_slot *s) {
    uint32_t i;

    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        channel_atomic_store(&s->permit_tokens[i], 0u);
    }
}

static int channel_token_find(const asx_channel_slot *s, uint32_t token, uint32_t *out_idx) {
    uint32_t i;

    if (token == 0u) return 0;

    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        if (channel_atomic_load(&s->permit_tokens[i]) == token) {
            if (out_idx != NULL) { *out_idx = i; }
            return 1;
        }
    }

    return 0;
}

static uint32_t channel_token_allocate(asx_channel_slot *s) {
    uint32_t token;
    uint32_t attempts;

    /* token 0 is reserved as "invalid / free-slot marker" */
    for (attempts = 0; attempts < ASX_CHANNEL_MAX_CAPACITY + 2u; attempts++) {
        ASX_CHECKPOINT_WAIVER("bounded: active permit tokens <= ASX_CHANNEL_MAX_CAPACITY");
        token = asx_atomic_u32_fetch_add(&s->next_token, 1u);
        if (token == 0u) { continue; }
        if (!channel_token_find(s, token, NULL)) { return token; }
    }

    return 0u;
}

static asx_status channel_token_consume(asx_channel_slot *s, uint32_t token) {
    uint32_t i;
    uint32_t expected;

    if (token == 0u) { return ASX_E_INVALID_STATE; }

    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        expected = token;
        if (asx_atomic_u32_compare_exchange(&s->permit_tokens[i], &expected, 0u)) {
            (void)channel_atomic_dec(&s->reserved);
            return ASX_OK;
        }
    }

    return ASX_E_INVALID_STATE;
}

#if ASX_CHANNEL_BACKEND_LOCKFREE
static void channel_lf_init(asx_channel_slot *s) {
    uint32_t i;

    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        asx_atomic_u32_init(&s->lf_queue.cells[i].sequence, i);
        s->lf_queue.cells[i].value = 0u;
    }

    asx_atomic_u32_init(&s->lf_queue.enqueue_pos, 0u);
    s->lf_queue.dequeue_pos = 0u;
    asx_atomic_u32_init(&s->lf_queue.committed, 0u);
    asx_atomic_u32_init(&s->lf_queue.in_use, 0u);
}

static int channel_lf_claim_capacity(asx_channel_slot *s) {
    uint32_t observed;
    uint32_t expected;

    observed = asx_atomic_u32_load(&s->lf_queue.in_use);
    while (observed < s->capacity) {
        ASX_CHECKPOINT_WAIVER(
            "bounded: atomic capacity claim retries only under producer contention");
        expected = observed;
        if (asx_atomic_u32_compare_exchange(&s->lf_queue.in_use, &expected, observed + 1u)) {
            return 1;
        }
        observed = expected;
    }

    return 0;
}

static void channel_lf_release_capacity(asx_channel_slot *s) {
    (void)channel_atomic_dec(&s->lf_queue.in_use);
}

static void channel_lf_discard_committed(asx_channel_slot *s) {
    uint32_t i;
    uint32_t reserved;

    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        asx_atomic_u32_store(&s->lf_queue.cells[i].sequence, i);
        s->lf_queue.cells[i].value = 0u;
    }

    reserved = channel_atomic_load(&s->reserved);
    asx_atomic_u32_store(&s->lf_queue.enqueue_pos, 0u);
    s->lf_queue.dequeue_pos = 0u;
    asx_atomic_u32_store(&s->lf_queue.committed, 0u);
    asx_atomic_u32_store(&s->lf_queue.in_use, reserved);
}

static asx_status channel_lf_enqueue(asx_channel_slot *s, uint64_t value) {
    uint32_t pos;
    uint32_t expected;
    uint32_t seq;
    int32_t diff;
    asx_channel_lf_cell *cell;

    for (;;) {
        ASX_CHECKPOINT_WAIVER("bounded: CAS loop makes progress when a producer claims a slot");
        pos = asx_atomic_u32_load(&s->lf_queue.enqueue_pos);
        cell = &s->lf_queue.cells[pos % s->capacity];
        seq = asx_atomic_u32_load(&cell->sequence);
        diff = (int32_t)(seq - pos);

        if (diff < 0) { return ASX_E_CHANNEL_FULL; }
        if (diff == 0) {
            expected = pos;
            if (asx_atomic_u32_compare_exchange(&s->lf_queue.enqueue_pos, &expected, pos + 1u)) {
                break;
            }
        }
    }

    channel_atomic_inc(&s->lf_queue.committed);
    cell->value = value;
    asx_atomic_fence_release();
    asx_atomic_u32_store(&cell->sequence, pos + 1u);

    return ASX_OK;
}

/* Take the oldest committed message. `release` 0 keeps its capacity
 * claimed for the caller (an eviction hands the slot to the new message). */
static asx_status channel_lf_dequeue(asx_channel_slot *s, uint64_t *out_value, int release) {
    asx_channel_lf_cell *cell;
    uint32_t seq;
    int32_t diff;

    cell = &s->lf_queue.cells[s->lf_queue.dequeue_pos % s->capacity];
    seq = asx_atomic_u32_load(&cell->sequence);
    diff = (int32_t)(seq - (s->lf_queue.dequeue_pos + 1u));
    if (diff < 0) { return ASX_E_WOULD_BLOCK; }

    asx_atomic_fence_acquire();
    *out_value = cell->value;
    asx_atomic_u32_store(&cell->sequence, s->lf_queue.dequeue_pos + s->capacity);
    s->lf_queue.dequeue_pos++;
    (void)channel_atomic_dec(&s->lf_queue.committed);
    if (release) channel_lf_release_capacity(s);

    return ASX_OK;
}
#endif

/* Take the oldest committed message (FIFO head) into *out_value, freeing
 * its slot. Returns 0 when none is ready. Receiver-side operation. */
static int channel_take_oldest(asx_channel_slot *s, uint64_t *out_value) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
    return channel_lf_dequeue(s, out_value, 1) == ASX_OK;
#else
    if (s->queue_len == 0u) return 0;
    *out_value = s->queue[s->queue_head];
    s->queue_head = (s->queue_head + 1u) % s->capacity;
    s->queue_len--;
    return 1;
#endif
}

/* ------------------------------------------------------------------ */
/* Wait queues                                                        */
/* ------------------------------------------------------------------ */

static void channel_reap_queue(asx_wait_queue *q);

/* Empty every queue, returning their nodes to the pool. */
static void channel_waiters_init(asx_channel_slot *s, uint16_t tag) {
    asx_wait_queue_init(&s->recv_waiters, channel_reap_queue, tag);
    asx_wait_queue_init(&s->reserve_waiters, channel_reap_queue, tag);
    asx_wait_queue_init(&s->closed_waiters, channel_reap_queue, tag);
}

/* Committed messages ready for try_recv. */
static uint32_t channel_message_count(const asx_channel_slot *s) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
    return channel_atomic_load(&s->lf_queue.committed);
#else
    return s->queue_len;
#endif
}

/* Capacity try_reserve could claim right now. */
static uint32_t channel_free_capacity(asx_channel_slot *s) {
    uint32_t used;
#if ASX_CHANNEL_BACKEND_LOCKFREE
    used = channel_atomic_load(&s->lf_queue.in_use);
#else
    used = s->queue_len + channel_atomic_load(&s->reserved);
#endif
    return used < s->capacity ? s->capacity - used : 0u;
}

/* Re-balance both wait queues after a state change: one queued receiver
 * per committed message holds a wake, and the head of the reserve line
 * holds one while capacity is free (only the head may claim it; when it
 * does, the next head is woken). The length checks keep queue-free
 * channels (the common case, and every foreign-thread producer of the
 * lock-free backend) off the queue code. */
static void channel_settle(asx_channel_slot *s) {
    if (s->recv_waiters.len > 0u) {
        (void)asx_wait_queue_settle(&s->recv_waiters, channel_message_count(s));
    }
    if (s->reserve_waiters.len > 0u) {
        (void)asx_wait_queue_settle(&s->reserve_waiters, channel_free_capacity(s) > 0u ? 1u : 0u);
    }
}

/* A waiter's task finished while still queued (Rust drops its wait
 * future, whose cleanup passes a wake on): drop the dead entry and
 * re-settle. */
static void channel_reap_queue(asx_wait_queue *q) {
    asx_kernel *k = asx_kernel_for_tag(q->tag);
    uint16_t i;
    if (k == NULL) return;
    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        asx_channel_slot *s = &k->channels.slots[i];
        ASX_CHECKPOINT_WAIVER("bounded: ASX_MAX_CHANNELS constant");
        if (q != &s->recv_waiters && q != &s->reserve_waiters && q != &s->closed_waiters) {
            continue;
        }
        asx_wait_queue_drop_dead(q);
        channel_settle(s);
        return;
    }
}

/* Close / disconnect: every waiter must observe the new state. */
static void channel_wake_everyone(asx_channel_slot *s) {
    if (s->recv_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->recv_waiters);
    if (s->reserve_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->reserve_waiters);
    if (s->closed_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->closed_waiters);
}

/* ------------------------------------------------------------------ */
/* Channel lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* A channel belongs to the runtime of the region it is created in. */
static asx_status channel_create_impl(asx_region_id region, uint32_t capacity, int unbounded,
                                      asx_channel_id *out_id) {
    uint16_t i;
    asx_kernel *k;
    asx_channel_slot *s;
    asx_region_state region_state;
    asx_status st;

    if (out_id == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (capacity == 0 || capacity > ASX_CHANNEL_MAX_CAPACITY) { return ASX_E_INVALID_ARGUMENT; }
    if (!asx_handle_is_valid(region)) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_handle_type_tag(region) != ASX_TYPE_REGION) { return ASX_E_INVALID_ARGUMENT; }

    st = asx_region_get_state(region, &region_state);
    if (st != ASX_OK) { return st; }
    k = asx_kernel_of(region);
    if (k == NULL) { return ASX_E_NOT_FOUND; }
    /* Rust's channels belong to no region: only a closed one refuses
     * (a server spawned by a task of a closing region still gets its
     * mailbox; its admission then refuses the task). */
    if (region_state == ASX_REGION_CLOSED) { return ASX_E_INVALID_STATE; }

    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        /* A fully closed channel is free once drained: a sealed one keeps
         * its queue receivable (asx_channel_seal). */
        asx_channel_slot *cand = &k->channels.slots[i];
        if (!cand->alive ||
            (cand->state == ASX_CHANNEL_FULLY_CLOSED && channel_message_count(cand) == 0u)) {
            s = cand;
            if ((uint32_t)i >= k->channels.hwm) { k->channels.hwm = (uint32_t)i + 1u; }

            s->generation++;
            if (s->generation == 0) s->generation = 1;
            s->state = ASX_CHANNEL_OPEN;
            s->region = region;
            s->alive = 1;
            s->sender_count = 1u;
            channel_sender_ids_reset(s);
            s->receiver_dropped = 0;
            s->unbounded = unbounded;
            s->discard_fn = NULL;
            s->discard_ctx = NULL;
            s->capacity = capacity;
            s->queue_head = 0;
            s->queue_len = 0;
            channel_atomic_store(&s->reserved, 0u);
            channel_atomic_store(&s->next_token, 1u);
            memset(s->queue, 0, sizeof(s->queue));
            channel_token_clear_all(s);
#if ASX_CHANNEL_BACKEND_LOCKFREE
            channel_lf_init(s);
#endif
            channel_waiters_init(s, k->tag);
            k->channels.count++;
            *out_id = channel_make_handle(k, i, s->generation);
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_channel_create(asx_region_id region, uint32_t capacity, asx_channel_id *out_id) {
    return channel_create_impl(region, capacity, 0, out_id);
}

/* Rust's unbounded_channel (mpsc.rs:599-618): the channel's arena slice
 * stands for usize::MAX; an overload catalog that forbids it refuses (DEC-008). */
asx_status asx_channel_create_unbounded(asx_region_id region, asx_channel_id *out_id) {
    const asx_overload_catalog_entry *entry = NULL;

    if (out_id == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_overload_catalog_get(asx_profile_active(), &entry) == ASX_OK && entry != NULL &&
        (entry->forbidden & ASX_FORBID_UNBOUNDED_QUEUE) != 0u) {
        return ASX_E_PERMISSION_DENIED;
    }
    return channel_create_impl(region, ASX_CHANNEL_MAX_CAPACITY, 1, out_id);
}

int asx_channel_is_unbounded(asx_channel_id id) {
    asx_channel_slot *s;
    if (channel_slot_lookup(id, &s) != ASX_OK) { return 0; }
    return s->unbounded != 0;
}

/* Rust's Sender::clone (mpsc.rs:1231-1239) and WeakSender::upgrade
 * (:1275): one more live sender, with an identity of its own. */
asx_status asx_channel_sender_clone(asx_channel_id id, asx_channel_sender *out_sender) {
    asx_channel_slot *s;
    asx_status st;
    uint32_t i;
    uint32_t free_idx = ASX_CHANNEL_MAX_SENDERS;
    uint32_t token = 0u;

    if (out_sender == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }
    /* Only a live sender can be cloned (Rust Sender::clone, mpsc.rs:1231). */
    if (s->state == ASX_CHANNEL_SENDER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_INVALID_STATE;
    }

    for (i = 0; i < ASX_CHANNEL_MAX_SENDERS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_SENDERS constant");
        if (s->sender_tokens[i] == 0u) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == ASX_CHANNEL_MAX_SENDERS) { return ASX_E_RESOURCE_EXHAUSTED; }

    /* A token no live clone holds, nonzero (it wraps only after 2^32
     * clones, and then skips any that is still live). */
    for (i = 0; i <= ASX_CHANNEL_MAX_SENDERS && token == 0u; i++) {
        uint32_t candidate = s->next_sender_token++;
        ASX_CHECKPOINT_WAIVER("bounded: at most ASX_CHANNEL_MAX_SENDERS live tokens to skip");
        if (candidate != 0u && !channel_sender_token_live(s, candidate)) { token = candidate; }
    }
    if (token == 0u) { return ASX_E_RESOURCE_EXHAUSTED; }

    s->sender_tokens[free_idx] = token;
    s->sender_count++;
    out_sender->channel = id;
    out_sender->token = token;
    return ASX_OK;
}

/* One sender is gone (Rust Sender::drop, mpsc.rs:1241-1255): a sender that
 * is not the last only lowers the count; the last one closes the sender
 * side and wakes the receiver. */
static asx_status channel_sender_dropped(asx_channel_slot *s) {
    if ((s->state == ASX_CHANNEL_OPEN || s->state == ASX_CHANNEL_RECEIVER_CLOSED) &&
        s->sender_count > 1u) {
        s->sender_count--;
        return ASX_OK;
    }

    switch (s->state) {
    case ASX_CHANNEL_OPEN:
        s->sender_count = 0u;
        s->state = ASX_CHANNEL_SENDER_CLOSED;
        channel_wake_everyone(s);
        return ASX_OK;
    case ASX_CHANNEL_RECEIVER_CLOSED:
        s->sender_count = 0u;
        s->state = ASX_CHANNEL_FULLY_CLOSED;
        channel_wake_everyone(s);
        /* Nobody waits on a fully closed channel again. */
        channel_waiters_init(s, s->recv_waiters.tag);
        return ASX_OK;
    case ASX_CHANNEL_SENDER_CLOSED:
    case ASX_CHANNEL_FULLY_CLOSED: return ASX_E_INVALID_STATE;
    }

    return ASX_E_INVALID_STATE;
}

/* Drop a cloned sender. Its token must be live: a second drop of the same
 * handle, or of a copy of it, finds none and is refused with the count
 * untouched, so it cannot remove another producer's sender. */
asx_status asx_channel_sender_drop(asx_channel_sender *sender) {
    asx_channel_slot *s;
    asx_status st;
    uint32_t i;

    if (sender == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (sender->token == 0u) { return ASX_E_INVALID_STATE; }

    st = channel_slot_lookup(sender->channel, &s);
    if (st != ASX_OK) { return st; }

    for (i = 0; i < ASX_CHANNEL_MAX_SENDERS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_SENDERS constant");
        if (s->sender_tokens[i] == sender->token) { break; }
    }
    if (i == ASX_CHANNEL_MAX_SENDERS) { return ASX_E_INVALID_STATE; }

    st = channel_sender_dropped(s);
    if (st != ASX_OK) { return st; }
    s->sender_tokens[i] = 0u;
    sender->token = 0u;
    return ASX_OK;
}

/* Drop the creator's sender, once. A second call is refused with the count
 * untouched: it must not take a clone's sender with it. */
asx_status asx_channel_close_sender(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }
    if (s->primary_dropped) { return ASX_E_INVALID_STATE; }

    st = channel_sender_dropped(s);
    if (st == ASX_OK) { s->primary_dropped = 1; }
    return st;
}

asx_status asx_channel_close_receiver(asx_channel_id id) {
    uint64_t dropped[ASX_CHANNEL_MAX_CAPACITY];
    uint32_t n_dropped;
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }
    if (s->receiver_dropped) { return ASX_E_INVALID_STATE; }

    /* Rust's Receiver::drop (mpsc.rs:2122-2148) closes the receiver side
     * unless a close() (asx_channel_seal) already did, and discards the
     * queue either way: a sealed channel's undrained messages go too. */
    s->receiver_dropped = 1;
    if (s->state == ASX_CHANNEL_OPEN) {
        s->state = ASX_CHANNEL_RECEIVER_CLOSED;
    } else if (s->state == ASX_CHANNEL_SENDER_CLOSED) {
        s->state = ASX_CHANNEL_FULLY_CLOSED;
    }
    /* The taken queue's messages reach the discard callback once the
     * channel is in its final state, as Rust drops them after its wakes. */
    n_dropped = channel_discard_queue(s, dropped);
    channel_wake_everyone(s);
    /* Nobody waits on a fully closed channel again. */
    if (s->state == ASX_CHANNEL_FULLY_CLOSED) channel_waiters_init(s, s->recv_waiters.tag);
    channel_run_discard(s, dropped, n_dropped);
    return ASX_OK;
}

asx_status asx_channel_seal(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    switch (s->state) {
    case ASX_CHANNEL_OPEN: s->state = ASX_CHANNEL_RECEIVER_CLOSED; break;
    case ASX_CHANNEL_SENDER_CLOSED: s->state = ASX_CHANNEL_FULLY_CLOSED; break;
    case ASX_CHANNEL_RECEIVER_CLOSED:
    case ASX_CHANNEL_FULLY_CLOSED: return ASX_E_INVALID_STATE;
    }
    /* Rust's close wakes the parked producers and the closed() observers
     * only; the receiver is the caller (mpsc.rs:1698-1711). The queue stays
     * receivable. */
    if (s->reserve_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->reserve_waiters);
    if (s->closed_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->closed_waiters);
    return ASX_OK;
}

asx_status asx_channel_wake_receiver(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }
    if (s->recv_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->recv_waiters);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Channel queries                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_channel_get_state(asx_channel_id id, asx_channel_state *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    *out = s->state;
    return ASX_OK;
}

asx_status asx_channel_queue_len(asx_channel_id id, uint32_t *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    *out = channel_atomic_load(&s->lf_queue.committed);
#else
    *out = s->queue_len;
#endif
    return ASX_OK;
}

asx_status asx_channel_reserved_count(asx_channel_id id, uint32_t *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    *out = channel_atomic_load(&s->reserved);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: reserve                                            */
/* ------------------------------------------------------------------ */

/* No capacity. A waiting reserve (asx_channel_reserve / asx_channel_send)
 * parks the calling task until a dequeue or abort frees a slot; settling
 * first re-issues any wake that a receiver which died after being woken
 * can no longer act on. A try_reserve never parks (Rust try_reserve and
 * try_send never register a waker, mpsc.rs:732-797). */
static asx_status channel_reserve_full(asx_channel_slot *s, int park) {
    /* An unbounded channel's senders never wait (Rust's UnboundedSender has
     * no backpressure, mpsc.rs:1313-1400): its arena used up is
     * exhaustion, refused with nothing changed and nothing queued. */
    if (s->unbounded) { return ASX_E_RESOURCE_EXHAUSTED; }
    if (park) {
        channel_settle(s);
        (void)asx_wait_queue_park_current(&s->reserve_waiters);
    }
    return ASX_E_CHANNEL_FULL;
}

static asx_status channel_reserve_impl(asx_channel_id id, asx_send_permit *out, int park) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        asx_wait_queue_leave_current(&s->reserve_waiters);
        return ASX_E_INVALID_STATE;
    }

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED) {
        asx_wait_queue_leave_current(&s->reserve_waiters);
        return ASX_E_DISCONNECTED;
    }

    /* FIFO, no queue jumping: capacity freed while producers are parked
     * belongs to the head of the line until it claims it or stops waiting,
     * so anyone with a live producer ahead of them reports FULL. */
    if (s->reserve_waiters.len > 0u && asx_wait_queue_live_ahead(&s->reserve_waiters) > 0u) {
        return channel_reserve_full(s, park);
    }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    if (!channel_lf_claim_capacity(s)) { return channel_reserve_full(s, park); }
#else
    if (s->queue_len + channel_atomic_load(&s->reserved) >= s->capacity) {
        return channel_reserve_full(s, park);
    }
#endif

    {
        uint32_t permit_idx;
        uint32_t token = channel_token_allocate(s);
        int found = 0;

        if (token == 0u) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
            channel_lf_release_capacity(s);
#endif
            return ASX_E_RESOURCE_EXHAUSTED;
        }

        for (permit_idx = 0; permit_idx < ASX_CHANNEL_MAX_CAPACITY; permit_idx++) {
            ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
            if (asx_atomic_u32_cas(&s->permit_tokens[permit_idx], 0u, token)) {
                found = 1;
                out->channel_id = id;
                out->token = token;
                out->consumed = 0;
                out->obligation = ASX_INVALID_ID;
                channel_atomic_inc(&s->reserved);
                break;
            }
        }
        if (!found) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
            channel_lf_release_capacity(s);
#endif
            return ASX_E_RESOURCE_EXHAUSTED;
        }
    }

    /* Reserved: the caller's wait (if any) is over; capacity left over goes
     * to the next parked producer. */
    if (s->reserve_waiters.len > 0u) {
        asx_wait_queue_leave_current(&s->reserve_waiters);
        channel_settle(s);
    }
    return ASX_OK;
}

asx_status asx_channel_try_reserve(asx_channel_id id, asx_send_permit *out) {
    return channel_reserve_impl(id, out, 0);
}

/* ------------------------------------------------------------------ */
/* Two-phase send: commit (send value)                                */
/* ------------------------------------------------------------------ */

/* Settle a genuine permit's SendPermit obligation (callers have consumed
 * its token, so a forged permit never reaches here): commit on delivery,
 * else abort with `reason` (mpsc.rs:1477, :1633). */
static int channel_permit_drop(uint64_t channel_id, uint64_t token);

static void channel_permit_resolve(asx_send_permit *permit, int delivered,
                                   asx_obligation_abort_reason reason) {
    asx_status st;
    if (permit->obligation == ASX_INVALID_ID) return;
    st = asx_obligation_resolve_token_internal(permit->obligation, channel_permit_drop,
                                               (uint64_t)permit->channel_id,
                                               (uint64_t)permit->token, 0u,
                                               delivered ? ASX_OBLIGATION_COMMITTED
                                                         : ASX_OBLIGATION_ABORTED,
                                               reason);
    (void)st; /* refused only for an obligation the runtime already resolved */
    permit->obligation = ASX_INVALID_ID;
}

asx_status asx_send_permit_send(asx_send_permit *permit, uint64_t value) {
    asx_channel_slot *s;
    asx_status st;
#if !ASX_CHANNEL_BACKEND_LOCKFREE
    uint32_t write_pos;
#endif

    if (permit == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (permit->consumed) { return ASX_E_INVALID_STATE; }
    if (permit->obligation != ASX_INVALID_ID) {
        st = asx_obligation_check_token_internal(permit->obligation, channel_permit_drop,
                                                 (uint64_t)permit->channel_id,
                                                 (uint64_t)permit->token, 0u);
        if (st != ASX_OK && st != ASX_E_OBLIGATION_ALREADY_RESOLVED) return st;
    }

    st = channel_slot_lookup(permit->channel_id, &s);
    if (st != ASX_OK) {
        permit->consumed = 1;
        return st;
    }

    st = channel_token_consume(s, permit->token);
    if (st != ASX_OK) {
        permit->consumed = 1;
        return st;
    }

    permit->consumed = 1;

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_release_capacity(s);
#endif
        channel_permit_resolve(permit, 0, ASX_OBLIGATION_ABORT_ERROR);
        return ASX_E_DISCONNECTED;
    }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    st = channel_lf_enqueue(s, value);
    if (st != ASX_OK) {
        channel_lf_release_capacity(s);
        channel_permit_resolve(permit, 0, ASX_OBLIGATION_ABORT_ERROR);
        channel_settle(s); /* the consumed permit freed capacity */
        return st;
    }
#else
    if (s->queue_len >= s->capacity) {
        channel_permit_resolve(permit, 0, ASX_OBLIGATION_ABORT_ERROR);
        channel_settle(s); /* the consumed permit freed capacity */
        return ASX_E_CHANNEL_FULL;
    }
    write_pos = (s->queue_head + s->queue_len) % s->capacity;
    s->queue[write_pos] = value;
    s->queue_len++;
#endif
    asx_trace_emit_internal(asx_kernel_of(permit->channel_id), ASX_TRACE_CHANNEL_SEND,
                            (uint64_t)permit->channel_id, value);
    channel_permit_resolve(permit, 1, ASX_OBLIGATION_ABORT_NONE);
    channel_settle(s); /* one more message for a parked receiver */

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: abort (return capacity)                            */
/* ------------------------------------------------------------------ */

static void channel_permit_abort(asx_send_permit *permit, asx_obligation_abort_reason reason) {
    asx_channel_slot *s;
    asx_status st;

    if (permit == NULL || permit->consumed) { return; }
    if (permit->obligation != ASX_INVALID_ID) {
        st = asx_obligation_check_token_internal(permit->obligation, channel_permit_drop,
                                                 (uint64_t)permit->channel_id,
                                                 (uint64_t)permit->token, 0u);
        if (st != ASX_OK && st != ASX_E_OBLIGATION_ALREADY_RESOLVED) return;
    }

    permit->consumed = 1;

    st = channel_slot_lookup(permit->channel_id, &s);
    if (st != ASX_OK) { return; }

    if (channel_token_consume(s, permit->token) == ASX_OK) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_release_capacity(s);
#endif
        channel_permit_resolve(permit, 0, reason);
        channel_settle(s); /* the slot is free again */
        /* With the sender side closed, the last outstanding permit going
         * away can turn an empty queue into a disconnect: re-check them. */
        if (s->state == ASX_CHANNEL_SENDER_CLOSED && s->recv_waiters.len > 0u) {
            (void)asx_wait_queue_wake_all(&s->recv_waiters);
        }
    }
}

void asx_send_permit_abort(asx_send_permit *permit) {
    channel_permit_abort(permit, ASX_OBLIGATION_ABORT_EXPLICIT);
}

void asx_send_permit_drop_internal(asx_send_permit *permit) {
    channel_permit_abort(permit, ASX_OBLIGATION_ABORT_CANCEL);
}

/* A tracked permit dropped with its holder task, unsent (Rust SendPermit's
 * Drop, mpsc.rs:1663-1673): the slot goes back as an abort gives it back,
 * and the runtime then aborts the obligation with Cancel. The caller's
 * permit value is spent: its token names no reservation any more, so a
 * later send or abort of it changes nothing. */
static int channel_permit_drop(uint64_t channel_id, uint64_t token) {
    asx_channel_slot *s;
    if (channel_slot_lookup((asx_channel_id)channel_id, &s) != ASX_OK) return 0;
    if (channel_token_consume(s, (uint32_t)token) == ASX_OK) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_release_capacity(s);
#endif
        channel_settle(s);
        if (s->state == ASX_CHANNEL_SENDER_CLOSED && s->recv_waiters.len > 0u) {
            (void)asx_wait_queue_wake_all(&s->recv_waiters);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Send, evicting the oldest message when full                        */
/* ------------------------------------------------------------------ */

#if ASX_CHANNEL_BACKEND_LOCKFREE
/* Remove the oldest committed message `pred` accepts, wherever it sits
 * (Rust's queue.iter().position(&mut predicate) and remove(index),
 * mpsc.rs:910-918). The cells from dequeue_pos up to the first one a
 * producer has claimed but not committed hold the committed messages in
 * FIFO order. Only the consumer touches them: a producer works on the
 * cell at enqueue_pos alone, and a cell below it keeps its sequence
 * number (position + 1) for as long as it holds a message. So the
 * messages ahead of the removed one move one cell toward the tail, which
 * keeps every sequence number valid, and the head cell is freed exactly
 * as a dequeue frees it. `release` 0 keeps the capacity claimed for the
 * caller (the eviction hands the slot to the new message). Consumer
 * thread only. Returns ASX_E_WOULD_BLOCK when no committed message is
 * accepted. */
static asx_status channel_lf_dequeue_where(asx_channel_slot *s, asx_channel_evict_pred_fn pred,
                                           void *ctx, uint64_t *out_value, int release) {
    uint32_t head = s->lf_queue.dequeue_pos;
    uint32_t found = 0u;
    uint32_t i;

    for (;;) {
        uint32_t pos = head + found;
        asx_channel_lf_cell *cell = &s->lf_queue.cells[pos % s->capacity];
        ASX_CHECKPOINT_WAIVER("bounded: the committed prefix is at most the channel capacity");
        if (asx_atomic_u32_load(&cell->sequence) != pos + 1u) { return ASX_E_WOULD_BLOCK; }
        asx_atomic_fence_acquire();
        if (pred(ctx, cell->value)) { break; }
        found++;
    }

    *out_value = s->lf_queue.cells[(head + found) % s->capacity].value;
    for (i = found; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: at most the channel capacity cells move");
        s->lf_queue.cells[(head + i) % s->capacity].value =
            s->lf_queue.cells[(head + i - 1u) % s->capacity].value;
    }
    asx_atomic_u32_store(&s->lf_queue.cells[head % s->capacity].sequence, head + s->capacity);
    s->lf_queue.dequeue_pos = head + 1u;
    (void)channel_atomic_dec(&s->lf_queue.committed);
    if (release) channel_lf_release_capacity(s);

    return ASX_OK;
}
#else
/* The ring backend's form of the above: the messages ahead of the
 * removed one move one slot toward the tail and the head slot is freed. */
static int channel_take_oldest_where(asx_channel_slot *s, asx_channel_evict_pred_fn pred, void *ctx,
                                     uint64_t *out_value) {
    uint32_t found;
    uint32_t i;

    for (found = 0u; found < s->queue_len; found++) {
        ASX_CHECKPOINT_WAIVER("bounded: at most the channel capacity committed messages");
        if (pred(ctx, s->queue[(s->queue_head + found) % s->capacity])) { break; }
    }
    if (found >= s->queue_len) { return 0; }

    *out_value = s->queue[(s->queue_head + found) % s->capacity];
    for (i = found; i > 0u; i--) {
        ASX_CHECKPOINT_WAIVER("bounded: at most the channel capacity slots move");
        s->queue[(s->queue_head + i) % s->capacity] =
            s->queue[(s->queue_head + i - 1u) % s->capacity];
    }
    s->queue_head = (s->queue_head + 1u) % s->capacity;
    s->queue_len--;
    return 1;
}
#endif

/* `send_evict_oldest(v)` is `send_evict_oldest_where(v, |_| true)`
 * (mpsc.rs:872-874). */
static int channel_evict_any(void *ctx, uint64_t value) {
    (void)ctx;
    (void)value;
    return 1;
}

asx_status asx_channel_send_evict_oldest(asx_channel_id id, uint64_t value, int *out_evicted,
                                         uint64_t *out_value) {
    return asx_channel_send_evict_oldest_where(id, value, channel_evict_any, NULL, out_evicted,
                                               out_value);
}

/* Rust send_evict_oldest_where (mpsc.rs:889-936): a free slot owned by a
 * parked producer refuses; any other free slot takes the value; a full
 * channel evicts its oldest committed message that `pred` accepts, never
 * a reserved slot, and refuses when there is none. */
asx_status asx_channel_send_evict_oldest_where(asx_channel_id id, uint64_t value,
                                               asx_channel_evict_pred_fn pred, void *ctx,
                                               int *out_evicted, uint64_t *out_value) {
    asx_channel_slot *s;
    asx_status st;
    uint64_t oldest = 0u;
    int evicted = 0;
#if !ASX_CHANNEL_BACKEND_LOCKFREE
    uint32_t write_pos;
#endif

    if (pred == NULL || out_evicted == NULL || out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    /* Rust's UnboundedSender has no send_evict_oldest. */
    if (s->unbounded) { return ASX_E_INVALID_STATE; }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_INVALID_STATE;
    }
    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED) { return ASX_E_DISCONNECTED; }

    if (channel_free_capacity(s) > 0u) {
        /* The free slot is the parked producer's: nothing is evicted. */
        if (s->reserve_waiters.len > 0u && asx_wait_queue_live_ahead(&s->reserve_waiters) > 0u) {
            return ASX_E_CHANNEL_FULL;
        }
#if ASX_CHANNEL_BACKEND_LOCKFREE
        if (!channel_lf_claim_capacity(s)) { return ASX_E_CHANNEL_FULL; }
#endif
    } else {
        /* Every slot holds a committed message or a reservation: the oldest
         * message `pred` accepts makes room; with all of them reserved, or
         * none accepted, there is none. */
#if ASX_CHANNEL_BACKEND_LOCKFREE
        if (channel_lf_dequeue_where(s, pred, ctx, &oldest, 0) != ASX_OK) {
            return ASX_E_CHANNEL_FULL;
        }
#else
        if (!channel_take_oldest_where(s, pred, ctx, &oldest)) { return ASX_E_CHANNEL_FULL; }
#endif
        evicted = 1;
    }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    /* The claimed capacity guarantees the cell (the consumer frees a cell
     * before its capacity). */
    st = channel_lf_enqueue(s, value);
    if (st != ASX_OK) {
        channel_lf_release_capacity(s);
        channel_settle(s);
        return st;
    }
#else
    write_pos = (s->queue_head + s->queue_len) % s->capacity;
    s->queue[write_pos] = value;
    s->queue_len++;
#endif
    asx_trace_emit_internal(asx_kernel_of(id), ASX_TRACE_CHANNEL_SEND, (uint64_t)id, value);
    channel_settle(s); /* one more message for a parked receiver */

    *out_evicted = evicted;
    *out_value = oldest;
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

/* A message was dequeued: the receiver's wait is over and one slot of
 * capacity is free again for a parked producer. */
static void channel_recv_done(asx_channel_slot *s) {
    asx_wait_queue_leave_current(&s->recv_waiters);
    channel_settle(s);
}

/* An empty queue that can get no more messages: the receiver is closed,
 * or the sender side is and no outstanding permit can still commit. */
static int channel_drained_for_good(asx_channel_slot *s) {
    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) return 1;
    return s->state == ASX_CHANNEL_SENDER_CLOSED && channel_atomic_load(&s->reserved) == 0u;
}

/* Receive one message. Empty: a waiting receive (asx_channel_recv) parks
 * the calling task until a commit or a close; a try_recv never parks (Rust
 * try_recv registers no waker, mpsc.rs:1956-1983). */
static asx_status channel_recv_impl(asx_channel_id id, uint64_t *out_value, int park) {
    asx_channel_slot *s;
    asx_status st;

    if (out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    if (channel_take_oldest(s, out_value)) {
        asx_trace_emit_internal(asx_kernel_of(id), ASX_TRACE_CHANNEL_RECV, (uint64_t)id,
                                *out_value);
        channel_recv_done(s);
        return ASX_OK;
    }

    if (channel_drained_for_good(s)) {
        asx_wait_queue_leave_current(&s->recv_waiters);
        return ASX_E_DISCONNECTED;
    }

    /* Empty: park until a commit (or a close). Settling first re-issues any
     * wake that a producer which died after being woken can no longer use. */
    if (park) {
        channel_settle(s);
        (void)asx_wait_queue_park_current(&s->recv_waiters);
    }
    return ASX_E_WOULD_BLOCK;
}

asx_status asx_channel_try_recv(asx_channel_id id, uint64_t *out_value) {
    return channel_recv_impl(id, out_value, 0);
}

/* ------------------------------------------------------------------ */
/* Wait withdrawal                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_channel_wait_cancel(asx_channel_id id, asx_task_id task) {
    asx_channel_slot *s;
    asx_status st;
    int removed;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    removed = asx_wait_queue_remove(&s->recv_waiters, task);
    removed |= asx_wait_queue_remove(&s->reserve_waiters, task);
    removed |= asx_wait_queue_remove(&s->closed_waiters, task);
    /* A wake the withdrawn task held goes to the next waiter in line. */
    if (removed) channel_settle(s);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Cx-aware waits                                                     */
/* ------------------------------------------------------------------ */

/* Rust's cancellation check at the top of every reserve / recv poll: any
 * checkpoint failure ends the wait (mpsc.rs:1061, :1786). Returns 1 when
 * it did: the trace is recorded and the task leaves the wait lines,
 * passing on a wake it held. */
static int channel_wait_cancelled(asx_channel_id id, asx_cx *cx, const char *message) {
    asx_status st;
    if (cx == NULL || asx_cx_checkpoint(cx) == ASX_OK) return 0;
    asx_trace_user(cx->task_id, message);
    st = asx_channel_wait_cancel(id, cx->task_id);
    (void)st; /* a bad handle has no line to leave */
    return 1;
}

asx_status asx_channel_reserve(asx_channel_id id, asx_cx *cx, asx_send_permit *out) {
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (channel_wait_cancelled(id, cx, "mpsc::reserve cancelled")) { return ASX_E_CANCELLED; }

    st = channel_reserve_impl(id, out, 1);
    if (st == ASX_E_CHANNEL_FULL) { return ASX_E_PENDING; }
    if (st != ASX_OK) { return st; }

    /* Registered after the slot is claimed, as Rust's Reserve does
     * (mpsc.rs:1116); a refusal leaves the permit untracked (:1180). A
     * tracked permit still held when its task completes is dropped with
     * it. */
    if (cx != NULL && cx->task_id != ASX_INVALID_ID) {
        asx_obligation_id ob;
        if (asx_obligation_register(cx->region_id, ASX_OBLIGATION_KIND_SEND_PERMIT, cx->task_id,
                                    &ob) == ASX_OK) {
            out->obligation = ob;
            asx_obligation_set_drop_internal(ob, channel_permit_drop, (uint64_t)id,
                                             (uint64_t)out->token, 0u);
        }
    }
    return ASX_OK;
}

asx_status asx_channel_send(asx_channel_id id, asx_cx *cx, uint64_t value) {
    asx_send_permit permit;
    asx_status st;

    if (channel_wait_cancelled(id, cx, "mpsc::reserve cancelled")) { return ASX_E_CANCELLED; }

    st = channel_reserve_impl(id, &permit, 1);
    if (st == ASX_E_CHANNEL_FULL) { return ASX_E_PENDING; }
    if (st != ASX_OK) { return st; }
    return asx_send_permit_send(&permit, value);
}

asx_status asx_channel_recv(asx_channel_id id, asx_cx *cx, uint64_t *out_value) {
    asx_status st;

    if (out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (channel_wait_cancelled(id, cx, "mpsc::recv cancelled")) { return ASX_E_CANCELLED; }

    st = channel_recv_impl(id, out_value, 1);
    return st == ASX_E_WOULD_BLOCK ? ASX_E_PENDING : st;
}

/* Rust poll_recv_many (mpsc.rs:1867-1940): limit 0 first, then the
 * cancellation check, then a batch, a drained-for-good 0, or a park. */
asx_status asx_channel_recv_many(asx_channel_id id, asx_cx *cx, uint64_t *out_values,
                                 uint32_t limit, uint32_t *out_count) {
    asx_channel_slot *s;
    asx_status st;
    uint32_t n = 0u;

    if (out_count == NULL || (limit > 0u && out_values == NULL)) { return ASX_E_INVALID_ARGUMENT; }
    *out_count = 0u;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }
    if (limit == 0u) { return ASX_OK; }
    if (channel_wait_cancelled(id, cx, "mpsc::recv_many cancelled")) { return ASX_E_CANCELLED; }

    while (n < limit && channel_take_oldest(s, &out_values[n])) {
        ASX_CHECKPOINT_WAIVER("bounded: at most the channel capacity is queued");
        asx_trace_emit_internal(asx_kernel_of(id), ASX_TRACE_CHANNEL_RECV, (uint64_t)id,
                                out_values[n]);
        n++;
    }
    if (n > 0u) {
        uint32_t free_slots;
        channel_recv_done(s);
        /* One parked producer per freed slot, as far as capacity is free
         * (sender_wakers_for_freed_slots, mpsc.rs:402-423). */
        free_slots = channel_free_capacity(s);
        if (n > 1u && s->reserve_waiters.len > 0u) {
            (void)asx_wait_queue_settle(&s->reserve_waiters, n < free_slots ? n : free_slots);
        }
        *out_count = n;
        return ASX_OK;
    }

    /* Empty for good: zero messages, not an error (mpsc.rs:1909-1917). */
    if (channel_drained_for_good(s)) {
        asx_wait_queue_leave_current(&s->recv_waiters);
        return ASX_OK;
    }

    channel_settle(s);
    (void)asx_wait_queue_park_current(&s->recv_waiters);
    return ASX_E_PENDING;
}

asx_status asx_channel_closed(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    /* Rust's UnboundedSender has no closed(). */
    if (s->unbounded) { return ASX_E_INVALID_STATE; }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        asx_wait_queue_leave_current(&s->closed_waiters);
        return ASX_E_INVALID_STATE;
    }
    /* Sender::is_closed is the receiver's close or drop (mpsc.rs:799). */
    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED) {
        asx_wait_queue_leave_current(&s->closed_waiters);
        return ASX_OK;
    }
    (void)asx_wait_queue_park_current(&s->closed_waiters);
    return ASX_E_PENDING;
}

/* ------------------------------------------------------------------ */
/* Reset (test support)                                               */
/* ------------------------------------------------------------------ */

void asx_channel_reset_internal(asx_kernel *k) {
    asx_mpsc_arena *ar = &k->channels;
    uint32_t i;

    for (i = 0; i < ar->hwm; i++) {
        asx_channel_slot *s = &ar->slots[i];
        uint64_t dropped[ASX_CHANNEL_MAX_CAPACITY];
        uint32_t n_dropped;
        /* ASX_CHECKPOINT_WAIVER("bounded: hwm <= ASX_MAX_CHANNELS") */
        if (s->alive) {
            /* What a live channel still queues is dropped unreceived. */
            n_dropped = channel_discard_queue(s, dropped);
            channel_run_discard(s, dropped, n_dropped);
            s->generation++;
        }
        s->alive = 0;
        s->state = ASX_CHANNEL_OPEN;
        s->receiver_dropped = 0;
        channel_sender_ids_reset(s);
        s->unbounded = 0;
        s->discard_fn = NULL;
        s->discard_ctx = NULL;
        s->queue_head = 0;
        s->queue_len = 0;
        channel_atomic_store(&s->reserved, 0u);
        channel_atomic_store(&s->next_token, 1u);
        memset(s->queue, 0, sizeof(s->queue));
        channel_token_clear_all(s);
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_init(s);
#endif
        channel_waiters_init(s, k->tag);
    }
    ar->hwm = 0;
    ar->count = 0;
}

void asx_channel_reset(const asx_runtime *rt) {
    asx_kernel *k = asx_kernel_of_runtime(rt);
    if (k != NULL) asx_channel_reset_internal(k);
}

/* ------------------------------------------------------------------ */
/* Discard callback                                                   */
/* ------------------------------------------------------------------ */

asx_status asx_channel_set_discard(asx_channel_id id, asx_channel_discard_fn fn, void *ctx) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }
    s->discard_fn = fn;
    s->discard_ctx = fn != NULL ? ctx : NULL;
    return ASX_OK;
}

/* Empty the queue, as a receiver drop does. When a discard callback is
 * installed the committed messages, oldest first, are moved into `out`
 * (room for ASX_CHANNEL_MAX_CAPACITY) for channel_run_discard; returns how
 * many. Declared above, defined here so the line numbers the docs cite stay
 * put. */
static uint32_t channel_discard_queue(asx_channel_slot *s, uint64_t *out) {
    uint32_t n = 0u;

    while (s->discard_fn != NULL && n < ASX_CHANNEL_MAX_CAPACITY &&
           channel_take_oldest(s, &out[n])) {
        ASX_CHECKPOINT_WAIVER("bounded: at most ASX_CHANNEL_MAX_CAPACITY committed messages");
        n++;
    }
#if ASX_CHANNEL_BACKEND_LOCKFREE
    channel_lf_discard_committed(s);
#else
    s->queue_len = 0;
    s->queue_head = 0;
#endif
    return n;
}

/* Run the channel's discard callback over `n` messages taken from its
 * queue. The callback and its context are read first, so a callback that
 * touches the channel's slot cannot redirect the calls that follow. */
static void channel_run_discard(asx_channel_slot *s, const uint64_t *values, uint32_t n) {
    asx_channel_discard_fn fn = s->discard_fn;
    void *ctx = s->discard_ctx;
    uint32_t i;

    for (i = 0u; fn != NULL && i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: at most ASX_CHANNEL_MAX_CAPACITY committed messages");
        fn(ctx, values[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Accounting view (bd-9kll.9.2, the channel_atomicity oracle)         */
/* ------------------------------------------------------------------ */

uint32_t asx_channel_slot_count_internal(const asx_kernel *k) { return k->channels.hwm; }

int asx_channel_accounting_get_internal(const asx_kernel *k, uint32_t index,
                                        asx_channel_accounting *out) {
    const asx_channel_slot *s;
    uint32_t i;

    if (out == NULL || index >= k->channels.hwm || index >= (uint32_t)ASX_MAX_CHANNELS) {
        return 0;
    }
    s = &k->channels.slots[index];
    if (!s->alive) return 0;

    memset(out, 0, sizeof(*out));
    out->region = s->region;
    out->capacity = s->capacity;
    out->queued = channel_message_count(s);
    out->reserved = channel_atomic_load(&s->reserved);
    for (i = 0; i < ASX_CHANNEL_MAX_CAPACITY; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_CAPACITY constant");
        if (channel_atomic_load(&s->permit_tokens[i]) != 0u) out->permit_tokens++;
    }
    out->sender_count = s->sender_count;
    for (i = 0; i < ASX_CHANNEL_MAX_SENDERS; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: ASX_CHANNEL_MAX_SENDERS constant");
        if (s->sender_tokens[i] != 0u) out->sender_tokens++;
    }
    out->primary_dropped = s->primary_dropped != 0 ? 1u : 0u;
#if ASX_CHANNEL_BACKEND_LOCKFREE
    out->has_in_use = 1u;
    out->in_use = channel_atomic_load(&s->lf_queue.in_use);
#endif
    return 1;
}
