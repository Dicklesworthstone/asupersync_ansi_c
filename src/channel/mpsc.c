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
 * Semantics specified in docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md.
 *
 * ASX_PROOF_BLOCK_WAIVER("reason: bug fix for recv after close, no semantic break")
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/channel.h>
#include <asx/platform/atomics.h>
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
    case ASX_CHANNEL_OPEN: s->state = ASX_CHANNEL_SENDER_CLOSED; return ASX_OK;
    case ASX_CHANNEL_RECEIVER_CLOSED: s->state = ASX_CHANNEL_FULLY_CLOSED; return ASX_OK;
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
        return ASX_OK;
    case ASX_CHANNEL_SENDER_CLOSED: s->state = ASX_CHANNEL_FULLY_CLOSED;
#if ASX_CHANNEL_BACKEND_LOCKFREE
        channel_lf_discard_committed(s);
#else
        s->queue_len = 0;
        s->queue_head = 0;
#endif
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

asx_status asx_channel_try_reserve(asx_channel_id id, asx_send_permit *out) {
    asx_channel_slot *s;
    asx_status st;

    if (out == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_INVALID_STATE;
    }

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED) { return ASX_E_DISCONNECTED; }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    if (!channel_lf_claim_capacity(s)) { return ASX_E_CHANNEL_FULL; }
#else
    if (s->queue_len + channel_atomic_load(&s->reserved) >= s->capacity) {
        return ASX_E_CHANNEL_FULL;
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

    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Two-phase send: commit (send value)                                */
/* ------------------------------------------------------------------ */

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
        return ASX_E_DISCONNECTED;
    }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    st = channel_lf_enqueue(s, value);
    if (st != ASX_OK) {
        channel_lf_release_capacity(s);
        return st;
    }
#else
    if (s->queue_len >= s->capacity) { return ASX_E_CHANNEL_FULL; }
    write_pos = (s->queue_head + s->queue_len) % s->capacity;
    s->queue[write_pos] = value;
    s->queue_len++;
#endif
    asx_trace_emit(ASX_TRACE_CHANNEL_SEND, (uint64_t)permit->channel_id, value);

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
    }
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

asx_status asx_channel_try_recv(asx_channel_id id, uint64_t *out_value) {
    asx_channel_slot *s;
    asx_status st;

    if (out_value == NULL) { return ASX_E_INVALID_ARGUMENT; }

    st = channel_slot_lookup(id, &s);
    if (st != ASX_OK) { return st; }

#if ASX_CHANNEL_BACKEND_LOCKFREE
    st = channel_lf_dequeue(s, out_value);
    if (st == ASX_OK) {
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)id, *out_value);
        return ASX_OK;
    }
    if (st != ASX_E_WOULD_BLOCK) { return st; }
#else
    if (s->queue_len > 0) {
        *out_value = s->queue[s->queue_head];
        s->queue_head = (s->queue_head + 1u) % s->capacity;
        s->queue_len--;
        asx_trace_emit(ASX_TRACE_CHANNEL_RECV, (uint64_t)id, *out_value);
        return ASX_OK;
    }
#endif

    if (s->state == ASX_CHANNEL_RECEIVER_CLOSED || s->state == ASX_CHANNEL_FULLY_CLOSED) {
        return ASX_E_DISCONNECTED;
    }

    if (s->state == ASX_CHANNEL_SENDER_CLOSED) {
        if (channel_atomic_load(&s->reserved) == 0u) { return ASX_E_DISCONNECTED; }
    }

    return ASX_E_WOULD_BLOCK;
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
    }
    g_channel_count = 0;
}
