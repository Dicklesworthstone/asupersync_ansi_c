/*
 * channel_state.h — the state of the channels that lives in a runtime's
 * kernel (internal, NOT part of the public API)
 *
 * The bounded MPSC channel's arena moves here from mpsc.c as the channels
 * become per runtime (K18, bd-9kll.3.20): runtime_internal.h embeds the
 * arena in asx_kernel, a channel id carries the runtime tag of the region
 * it was created in, and a zeroed arena is empty.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_CHANNEL_CHANNEL_STATE_H
#define ASX_CHANNEL_CHANNEL_STATE_H

#include "../sync/wait_queue.h"
#include <asx/asx_config.h>
#include <asx/core/channel.h>
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

#endif /* ASX_CHANNEL_CHANNEL_STATE_H */
