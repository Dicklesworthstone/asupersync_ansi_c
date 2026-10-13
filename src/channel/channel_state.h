/*
 * channel_state.h — the state of the channels that lives in a runtime's
 * kernel (internal, NOT part of the public API)
 *
 * Each channel's arena moves here from its source as it becomes per runtime
 * (K18, bd-9kll.3.20): asx_kernel embeds it, a handle's tag names it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CHANNEL_CHANNEL_STATE_H
#define ASX_CHANNEL_CHANNEL_STATE_H

#include "../sync/wait_queue.h"
#include <asx/asx_config.h>
#include <asx/core/channel.h>
#include <asx/core/oneshot.h>
#include <asx/core/watch.h>
#include <asx/platform/atomics.h>
#include <stdint.h>

#ifndef ASX_CHANNEL_BACKEND_LOCKFREE
#if (defined(ASX_PROFILE_POSIX) || defined(ASX_PROFILE_PARALLEL)) && !ASX_LOCKFREE_SINGLE_THREAD
#define ASX_CHANNEL_BACKEND_LOCKFREE 1
#else
#define ASX_CHANNEL_BACKEND_LOCKFREE 0
#endif
#endif

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
    /* Live senders (Rust ChannelShared::sender_count, mpsc.rs:302): 1 at
     * create, +1 per asx_channel_sender_clone, -1 per
     * asx_channel_close_sender / asx_channel_sender_drop; the sender side
     * closes at 0. Rust's Sender is an owned value, dropped once; the
     * identities below make that true here: the creator's sender is
     * `primary_dropped` once dropped, and each live clone holds a nonzero
     * token in `sender_tokens` until its drop clears it, so a second drop
     * of either is refused before it can lower the count. */
    uint32_t sender_count;
    int primary_dropped;
    uint32_t sender_tokens[ASX_CHANNEL_MAX_SENDERS];
    uint32_t next_sender_token;
    /* The receiver was dropped (asx_channel_close_receiver). A seal closes
     * the receiver side but keeps the receiver (Rust's close vs drop,
     * mpsc.rs:1698, :2122). */
    int receiver_dropped;
    /* Created by asx_channel_create_unbounded: its senders never wait. The
     * ring is the channel's whole arena slice, and exhausting it is
     * ASX_E_RESOURCE_EXHAUSTED rather than a full channel to park on. */
    int unbounded;
    asx_channel_discard_fn discard_fn; /* asx_channel_set_discard */
    void *discard_ctx;

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
    asx_wait_queue closed_waiters;  /* senders waiting for the receiver to close */
} asx_channel_slot;

/* One runtime's MPSC channels. */
typedef struct {
    asx_channel_slot slots[ASX_MAX_CHANNELS];
    uint32_t count;
    /* One past the highest slot created since the last reset. A slot from
     * here up was never created, so it is not alive and asx_channel_create()
     * initialises every field it uses: the reset need not visit it. */
    uint32_t hwm;
} asx_mpsc_arena;

/* ------------------------------------------------------------------ */
/* Oneshot and watch channels                                          */
/* ------------------------------------------------------------------ */

/* A oneshot or watch handle names its arena by the tag of the runtime that
 * issued it in the high half of its slot field, the index in the low half
 * (ASX_SYNC_SLOT_TAG_SHIFT / ASX_SYNC_SLOT_INDEX_MASK, sync_state.h). */

typedef struct {
    asx_oneshot_state state;
    uint16_t generation;
    uint64_t value;
    int sender_alive;
    int receiver_alive;
    int receiver_dropped;              /* asx_oneshot_receiver_drop ran (Rust receiver_dropped) */
    int reserved;                      /* a permit holds the sending side (asx_oneshot_reserve) */
    asx_oneshot_discard_fn discard_fn; /* asx_oneshot_set_discard */
    void *discard_ctx;
    asx_wait_queue waiters;   /* tasks parked in asx_oneshot_recv */
    asx_wait_queue tx_closed; /* parked in asx_oneshot_poll_closed (Rust sender_waker) */
    asx_wait_queue rx_closed; /* parked in asx_oneshot_receiver_poll_closed
                               * (Rust receiver_closed_waker) */
} asx_oneshot_slot;

/* One runtime's oneshot channels. */
typedef struct {
    asx_oneshot_slot slots[ASX_MAX_ONESHOTS];
    uint32_t count; /* slots created since the last reset */
} asx_oneshot_arena;

typedef struct {
    uint16_t generation;
    int sender_alive;
    uint64_t value;
    uint64_t version; /* incremented on each send; 0 = initial value */
    uint32_t receiver_count;
    /* The clones in-flight wait_for calls hold (Rust's wait_for waits on
     * `self.clone()`): counted by receiver_count()/is_closed, outside the
     * receiver limit, since their owners already hold a receiver. */
    uint32_t waiter_clones;
    asx_watch_discard_fn discard_fn; /* asx_watch_set_discard */
    void *discard_ctx;
    int released;           /* the channel died and its last value was dropped */
    asx_wait_queue waiters; /* tasks parked in poll_changed / wait_for */
} asx_watch_slot;

/* One runtime's watch channels. */
typedef struct {
    asx_watch_slot slots[ASX_MAX_WATCHES];
    uint32_t count; /* slots created since the last reset */
} asx_watch_arena;

#endif /* ASX_CHANNEL_CHANNEL_STATE_H */
