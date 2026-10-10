/*
 * mpsc.c — bounded MPSC two-phase channel implementation (bd-2cw.6)
 *
 * CORE builds use a deterministic single-thread ring buffer. Live
 * POSIX/PARALLEL builds can use the atomic committed-message backend
 * while preserving the same reserve/send/abort contract.
 *
 * Two-phase protocol:
 *   1. try_reserve — claims capacity, returns permit
 *   2. send (via permit) — enqueues value FIFO
 *      OR abort (via permit) — returns capacity without enqueuing
 *
 * Capacity invariant: queue_len + reserved_count <= capacity
 *
 * Wake-driven waiting: inside a scheduler poll, the waiting operations
 * asx_channel_recv (empty) and asx_channel_reserve / asx_channel_send
 * (full) park the calling task in the channel's FIFO recv / reserve wait
 * queue. try_recv and try_reserve never park, as Rust's try_* register no
 * waker (bd-vc1v). After every state change the queues are settled: each
 * committed message is owed to one parked receiver, and free capacity to
 * the head of the reserve line (a commit wakes one receiver, a dequeue or
 * abort wakes the oldest producer). As upstream, no reserve jumps the
 * line: with a live producer parked ahead it reports FULL even if capacity
 * is free. Closing either side wakes everyone.
 * asx_channel_wait_cancel() withdraws a task that stops waiting and passes
 * any wake it held on. Outside a scheduler poll nothing parks.
 *
 * Semantics specified in docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md.
 *
 * ASX_PROOF_BLOCK_WAIVER("reason: bug fix for recv after close, no semantic break")
 *
 * SPDX-License-Identifier: MIT
 */

#include "../sync/wait_queue.h"
#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/platform/atomics.h>
#include <asx/runtime/runtime.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <string.h>

#ifndef ASX_CHANNEL_BACKEND_LOCKFREE
#if (defined(ASX_PROFILE_POSIX) || defined(ASX_PROFILE_PARALLEL)) && !ASX_LOCKFREE_SINGLE_THREAD
#define ASX_CHANNEL_BACKEND_LOCKFREE 1
#else
#define ASX_CHANNEL_BACKEND_LOCKFREE 0
#endif
#endif

/* ------------------------------------------------------------------ */
/* Internal channel slot                                              */
/* ------------------------------------------------------------------ */

#if ASX_CHANNEL_BACKEND_LOCKFREE
typedef struct {
    asx_atomic_u32 sequence;
    uint64_t value;
} asx_channel_lf_cell;

typedef struct {
    asx_channel_lf_cell cells[ASX_CHANNEL_MAX_CAPACITY];
    asx_atomic_u32 enqueue_pos;
    uint32_t dequeue_pos;
    asx_atomic_u32 committed;
    asx_atomic_u32 in_use; /* committed messages + outstanding reserves */
} asx_channel_lf_queue;
#endif

typedef struct {
    asx_channel_state state;
    asx_region_id region;
    uint16_t generation;
    int alive;

    /* Bounded ring buffer */
    uint32_t capacity;
    uint64_t queue[ASX_CHANNEL_MAX_CAPACITY];
    uint32_t queue_head; /* next read position */
    uint32_t queue_len;  /* committed messages in queue */

    /* Two-phase accounting */
    asx_atomic_u32 reserved;                                /* outstanding permits */
    asx_atomic_u32 next_token;                              /* monotonic permit token */
    asx_atomic_u32 permit_tokens[ASX_CHANNEL_MAX_CAPACITY]; /* 0 = free */

#if ASX_CHANNEL_BACKEND_LOCKFREE
    asx_channel_lf_queue lf_queue;
#endif

    /* Parked tasks (scheduler-thread state; FIFO by arrival) */
    asx_wait_queue recv_waiters;    /* waiting for a committed message */
    asx_wait_queue reserve_waiters; /* waiting for free capacity */
} asx_channel_slot;

static asx_channel_slot g_channels[ASX_MAX_CHANNELS];
static uint32_t g_channel_count;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static asx_status channel_slot_lookup(asx_channel_id id, asx_channel_slot **out) {
    uint16_t slot_idx;
    uint16_t gen;
    asx_channel_slot *s;

    if (!asx_handle_is_valid(id)) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_handle_type_tag(id) != ASX_TYPE_CHANNEL) { return ASX_E_INVALID_ARGUMENT; }

    slot_idx = asx_handle_slot(id);
    gen = asx_handle_generation(id);

    if (slot_idx >= ASX_MAX_CHANNELS) { return ASX_E_NOT_FOUND; }

    s = &g_channels[slot_idx];
    if (!s->alive) { return ASX_E_NOT_FOUND; }
    if (s->generation != gen) { return ASX_E_STALE_HANDLE; }

    *out = s;
    return ASX_OK;
}

static asx_channel_id channel_make_handle(uint16_t slot_idx, uint16_t gen) {
    uint32_t index = asx_handle_pack_index(gen, slot_idx);
    return asx_handle_pack(ASX_TYPE_CHANNEL, 0, index);
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

static asx_status channel_lf_dequeue(asx_channel_slot *s, uint64_t *out_value) {
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
    channel_lf_release_capacity(s);

    return ASX_OK;
}
#endif

/* ------------------------------------------------------------------ */
/* Wait queues                                                        */
/* ------------------------------------------------------------------ */

/* Empty both queues, returning their nodes to the pool. */
static void channel_waiters_init(asx_channel_slot *s) {
    asx_wait_queue_init(&s->recv_waiters);
    asx_wait_queue_init(&s->reserve_waiters);
}

/* Committed messages ready for try_recv. */
static uint32_t channel_message_count(asx_channel_slot *s) {
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

/* Close / disconnect: every waiter must observe the new state. */
static void channel_wake_everyone(asx_channel_slot *s) {
    if (s->recv_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->recv_waiters);
    if (s->reserve_waiters.len > 0u) (void)asx_wait_queue_wake_all(&s->reserve_waiters);
}

/* ------------------------------------------------------------------ */
/* Channel lifecycle                                                  */
/* ------------------------------------------------------------------ */

asx_status asx_channel_create(asx_region_id region, uint32_t capacity, asx_channel_id *out_id) {
    uint16_t i;
    asx_channel_slot *s;
    asx_region_state region_state;
    asx_status st;

    if (out_id == NULL) { return ASX_E_INVALID_ARGUMENT; }
    if (capacity == 0 || capacity > ASX_CHANNEL_MAX_CAPACITY) { return ASX_E_INVALID_ARGUMENT; }
    if (!asx_handle_is_valid(region)) { return ASX_E_INVALID_ARGUMENT; }
    if (asx_handle_type_tag(region) != ASX_TYPE_REGION) { return ASX_E_INVALID_ARGUMENT; }

    st = asx_region_get_state(region, &region_state);
    if (st != ASX_OK) { return st; }
    if (region_state != ASX_REGION_OPEN) { return ASX_E_INVALID_STATE; }

    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        if (!g_channels[i].alive || g_channels[i].state == ASX_CHANNEL_FULLY_CLOSED) {
            s = &g_channels[i];

            s->generation++;
            if (s->generation == 0) s->generation = 1;
            s->state = ASX_CHANNEL_OPEN;
            s->region = region;
            s->alive = 1;
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
            channel_waiters_init(s);

            g_channel_count++;
            *out_id = channel_make_handle(i, s->generation);
            return ASX_OK;
        }
    }

    return ASX_E_RESOURCE_EXHAUSTED;
}

asx_status asx_channel_close_sender(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    switch (s->state) {
    case ASX_CHANNEL_OPEN:
        s->state = ASX_CHANNEL_SENDER_CLOSED;
        channel_wake_everyone(s);
        return ASX_OK;
    case ASX_CHANNEL_RECEIVER_CLOSED:
        s->state = ASX_CHANNEL_FULLY_CLOSED;
        channel_wake_everyone(s);
        /* Nobody waits on a fully closed channel again. */
        channel_waiters_init(s);
        return ASX_OK;
    case ASX_CHANNEL_SENDER_CLOSED:
    case ASX_CHANNEL_FULLY_CLOSED: return ASX_E_INVALID_STATE;
    }

    return ASX_E_INVALID_STATE;
}

asx_status asx_channel_close_receiver(asx_channel_id id) {
    asx_channel_slot *s;
    asx_status st;

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    switch (s->state) {
    case ASX_CHANNEL_OPEN: s->state = ASX_CHANNEL_RECEIVER_CLOSED;
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_discard_committed(s);
#else
        s->queue_len = 0;
        s->queue_head = 0;
#endif
        channel_wake_everyone(s);
        return ASX_OK;
    case ASX_CHANNEL_SENDER_CLOSED: s->state = ASX_CHANNEL_FULLY_CLOSED;
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_discard_committed(s);
#else
        s->queue_len = 0;
        s->queue_head = 0;
#endif
        channel_wake_everyone(s);
        /* Nobody waits on a fully closed channel again. */
        channel_waiters_init(s);
        return ASX_OK;
    case ASX_CHANNEL_RECEIVER_CLOSED:
    case ASX_CHANNEL_FULLY_CLOSED: return ASX_E_INVALID_STATE;
    }

    return ASX_E_INVALID_STATE;
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
static void channel_permit_resolve(asx_send_permit *permit, int delivered,
                                   asx_obligation_abort_reason reason) {
    asx_status st;
    if (permit->obligation == ASX_INVALID_ID) return;
    st = delivered ? asx_obligation_commit(permit->obligation)
                   : asx_obligation_abort_with_reason(permit->obligation, reason);
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
    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)permit->channel_id, value);
    channel_permit_resolve(permit, 1, ASX_OBLIGATION_ABORT_NONE);
    channel_settle(s); /* one more message for a parked receiver */

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: abort (return capacity)                            */
/* ------------------------------------------------------------------ */

void asx_send_permit_abort(asx_send_permit *permit) {
    asx_channel_slot *s;
    asx_status st;

    if (permit == NULL || permit->consumed) { return; }

    permit->consumed = 1;

    st = channel_slot_lookup(permit->channel_id, &s);
    if (st != ASX_OK) { return; }

    if (channel_token_consume(s, permit->token) == ASX_OK) {
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_release_capacity(s);
#endif
        channel_permit_resolve(permit, 0, ASX_OBLIGATION_ABORT_EXPLICIT);
        channel_settle(s); /* the slot is free again */
        /* With the sender side closed, the last outstanding permit going
         * away can turn an empty queue into a disconnect: re-check them. */
        if (s->state == ASX_CHANNEL_SENDER_CLOSED && s->recv_waiters.len > 0u) {
            (void)asx_wait_queue_wake_all(&s->recv_waiters);
        }
    }
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

/* Receive one message. Empty: a waiting receive (asx_channel_recv) parks
 * the calling task until a commit or a close; a try_recv never parks (Rust
 * try_recv registers no waker, mpsc.rs:1956-1983). */
static asx_status channel_recv_impl(asx_channel_id id, uint64_t *out_value, int park) {
    asx_channel_slot *s;
    asx_status st;

    if (out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    st = channel_lf_dequeue(s, out_value);
    if (st == ASX_OK) {
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)id, *out_value);
        channel_recv_done(s);
        return ASX_OK;
    }
    if (st != ASX_E_WOULD_BLOCK) { return st; }
#else
    if (s->queue_len > 0) {
        *out_value = s->queue[s->queue_head];
        s->queue_head = (s->queue_head + 1u) % s->capacity;
        s->queue_len--;
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)id, *out_value);
        channel_recv_done(s);
        return ASX_OK;
    }
#endif

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        asx_wait_queue_leave_current(&s->recv_waiters);
        return ASX_E_DISCONNECTED;
    }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED) {
        if (channel_atomic_load(&s->reserved) == 0u) {
            asx_wait_queue_leave_current(&s->recv_waiters);
            return ASX_E_DISCONNECTED;
        }
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
     * (mpsc.rs:1116); a refusal leaves the permit untracked (:1180). */
    if (cx != NULL && cx->task_id != ASX_INVALID_ID) {
        asx_obligation_id ob;
        if (asx_obligation_reserve_ex(cx->region_id, ASX_OBLIGATION_KIND_SEND_PERMIT, cx->task_id,
                                      &ob) == ASX_OK) {
            out->obligation = ob;
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

/* ------------------------------------------------------------------ */
/* Reset (test support)                                               */
/* ------------------------------------------------------------------ */

void asx_channel_reset(void) {
    uint16_t i;

    for (i = 0; i < ASX_MAX_CHANNELS; i++) {
        if (g_channels[i].alive) { g_channels[i].generation++; }
        g_channels[i].alive = 0;
        g_channels[i].state = ASX_CHANNEL_OPEN;
        g_channels[i].queue_head = 0;
        g_channels[i].queue_len = 0;
        channel_atomic_store(&g_channels[i].reserved, 0u);
        channel_atomic_store(&g_channels[i].next_token, 1u);
        memset(g_channels[i].queue, 0, sizeof(g_channels[i].queue));
        channel_token_clear_all(&g_channels[i]);
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_init(&g_channels[i]);
#endif
        channel_waiters_init(&g_channels[i]);
    }
    g_channel_count = 0;
}
