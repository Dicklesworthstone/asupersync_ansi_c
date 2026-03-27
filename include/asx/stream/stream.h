/*
 * asx/stream/stream.h — async stream processing primitives
 *
 * Provides the stream "trait" (via function pointers) and concrete adapter
 * types for processing asynchronous sequences of values.
 *
 * Design:
 *   - Stream trait is a poll_next function pointer + opaque state pointer.
 *   - Each combinator is a concrete struct holding its own state.
 *   - Zero dynamic allocation — all types are stack-allocable value types.
 *   - Items are passed as void* with caller-managed lifetimes.
 *   - Channel/watch/broadcast source adapters bridge existing primitives.
 *
 * Poll result convention:
 *   ASX_STREAM_READY   — item available, written to *out_item
 *   ASX_STREAM_PENDING — would block, waker will be signaled
 *   ASX_STREAM_DONE    — stream exhausted, no more items
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_STREAM_STREAM_H
#define ASX_STREAM_STREAM_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/runtime/waker.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Stream poll result                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    ASX_STREAM_READY = 0,   /* item available */
    ASX_STREAM_PENDING = 1, /* would block */
    ASX_STREAM_DONE = 2     /* stream exhausted */
} asx_stream_result;

/* ------------------------------------------------------------------ */
/* Stream trait — poll-based async iterator                            */
/* ------------------------------------------------------------------ */

/* poll_next: attempt to produce the next item.
 * On ASX_STREAM_READY, *out_item points to the produced item.
 * On ASX_STREAM_PENDING, the waker will be signaled when ready.
 * On ASX_STREAM_DONE, no more items will be produced. */
typedef asx_stream_result (*asx_stream_poll_fn)(void *state, const asx_waker *waker,
                                                void **out_item);

typedef struct {
    asx_stream_poll_fn poll_next;
    void *state;
} asx_stream;

/* Poll the stream for the next item. */
ASX_API ASX_MUST_USE asx_stream_result asx_stream_poll_next(asx_stream *s, const asx_waker *waker,
                                                            void **out_item);

/* ------------------------------------------------------------------ */
/* Iter source — stream from a fixed array                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const void *items; /* pointer to item array */
    size_t item_size;  /* size of each item */
    size_t count;      /* total number of items */
    size_t index;      /* current position */
} asx_stream_iter_state;

/* Initialize an iter stream over an array of items.
 * items must remain valid for the lifetime of the stream. */
ASX_API void asx_stream_iter_init(asx_stream *s, asx_stream_iter_state *state, const void *items,
                                  size_t item_size, size_t count);

/* ------------------------------------------------------------------ */
/* Map combinator — transforms each item                              */
/* ------------------------------------------------------------------ */

/* Map function: transforms in_item, returns pointer to transformed item.
 * The returned pointer must remain valid until the next poll_next call.
 * user_data is the closure context. */
typedef void *(*asx_stream_map_fn)(void *in_item, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_map_fn map_fn;
    void *user_data;
} asx_stream_map_state;

/* Create a map stream wrapping an inner stream. */
ASX_API void asx_stream_map_init(asx_stream *s, asx_stream_map_state *state, asx_stream inner,
                                 asx_stream_map_fn map_fn, void *user_data);

/* ------------------------------------------------------------------ */
/* Filter combinator — yields only matching items                     */
/* ------------------------------------------------------------------ */

/* Filter predicate: returns nonzero if item should be yielded. */
typedef int (*asx_stream_filter_fn)(const void *item, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_filter_fn filter_fn;
    void *user_data;
} asx_stream_filter_state;

/* Create a filter stream wrapping an inner stream. */
ASX_API void asx_stream_filter_init(asx_stream *s, asx_stream_filter_state *state, asx_stream inner,
                                    asx_stream_filter_fn filter_fn, void *user_data);

/* ------------------------------------------------------------------ */
/* Take combinator — limits stream to n items                         */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    size_t remaining;
} asx_stream_take_state;

/* Create a take stream that yields at most n items. */
ASX_API void asx_stream_take_init(asx_stream *s, asx_stream_take_state *state, asx_stream inner,
                                  size_t n);

/* ------------------------------------------------------------------ */
/* Skip combinator — skips the first n items                          */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    size_t remaining; /* items left to skip */
} asx_stream_skip_state;

/* Create a skip stream that skips the first n items. */
ASX_API void asx_stream_skip_init(asx_stream *s, asx_stream_skip_state *state, asx_stream inner,
                                  size_t n);

/* ------------------------------------------------------------------ */
/* Chain combinator — first stream then second                        */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream first;
    asx_stream second;
    int first_done; /* nonzero when first stream is exhausted */
} asx_stream_chain_state;

/* Create a chain stream: all items from first, then all from second. */
ASX_API void asx_stream_chain_init(asx_stream *s, asx_stream_chain_state *state, asx_stream first,
                                   asx_stream second);

/* ------------------------------------------------------------------ */
/* Enumerate combinator — adds index to items                         */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t index;
    void *item;
} asx_stream_indexed_item;

typedef struct {
    asx_stream inner;
    size_t index;
    asx_stream_indexed_item current;
} asx_stream_enumerate_state;

/* Create an enumerate stream that pairs each item with its index. */
ASX_API void asx_stream_enumerate_init(asx_stream *s, asx_stream_enumerate_state *state,
                                       asx_stream inner);

/* ------------------------------------------------------------------ */
/* Fold terminal — reduces stream to a single value                   */
/* ------------------------------------------------------------------ */

/* Fold function: accumulator = fold_fn(accumulator, item, user_data) */
typedef void (*asx_stream_fold_fn)(void *accumulator, void *item, void *user_data);

/* Synchronously fold a stream.  Polls until DONE, applying fold_fn.
 * Returns ASX_OK on success, ASX_E_WOULD_BLOCK if any poll returns PENDING. */
ASX_API ASX_MUST_USE asx_status asx_stream_fold(asx_stream *s, void *accumulator,
                                                asx_stream_fold_fn fold_fn, void *user_data);

/* ------------------------------------------------------------------ */
/* Count terminal — counts items in a stream                          */
/* ------------------------------------------------------------------ */

/* Synchronously count all items.
 * Returns ASX_OK on success, ASX_E_WOULD_BLOCK if any poll returns PENDING. */
ASX_API ASX_MUST_USE asx_status asx_stream_count(asx_stream *s, size_t *out_count);

/* ------------------------------------------------------------------ */
/* ForEach terminal — executes a function for each item               */
/* ------------------------------------------------------------------ */

typedef void (*asx_stream_foreach_fn)(void *item, void *user_data);

/* Synchronously consume a stream, calling fn for each item.
 * Returns ASX_OK on success, ASX_E_WOULD_BLOCK if any poll returns PENDING. */
ASX_API ASX_MUST_USE asx_status asx_stream_for_each(asx_stream *s, asx_stream_foreach_fn fn,
                                                    void *user_data);

/* ------------------------------------------------------------------ */
/* Channel source adapters                                             */
/* ------------------------------------------------------------------ */

/* ReceiverStream: wraps asx_channel_id receiver into a stream.
 * Each poll_next calls asx_channel_try_recv. */
typedef struct {
    uint16_t channel_id; /* asx_channel_id */
    void *recv_buf;      /* pointer to buffer for received item */
} asx_stream_receiver_state;

/* Initialize a stream that reads from a channel receiver.
 * recv_buf must be large enough to hold one channel item. */
ASX_API void asx_stream_from_receiver(asx_stream *s, asx_stream_receiver_state *state,
                                      uint16_t channel_id, void *recv_buf);

/* WatchStream: wraps asx_watch into a stream of value changes. */
typedef struct {
    void *watch_rx;  /* pointer to asx_watch_receiver */
    void *value_buf; /* pointer to buffer for current value */
} asx_stream_watch_state;

/* Initialize a stream that yields on watch value changes. */
ASX_API void asx_stream_from_watch(asx_stream *s, asx_stream_watch_state *state, void *watch_rx,
                                   void *value_buf);

/* BroadcastStream: wraps asx_broadcast_receiver into a stream. */
typedef struct {
    void *broadcast_rx; /* pointer to asx_broadcast_receiver */
    void *recv_buf;     /* pointer to buffer for received item */
} asx_stream_broadcast_state;

/* Initialize a stream that reads from a broadcast receiver. */
ASX_API void asx_stream_from_broadcast(asx_stream *s, asx_stream_broadcast_state *state,
                                       void *broadcast_rx, void *recv_buf);

/* ------------------------------------------------------------------ */
/* Merge combinator — interleaves items from two streams              */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream a;
    asx_stream b;
    int a_done;
    int b_done;
    int prefer_a; /* alternating preference for fairness */
} asx_stream_merge_state;

/* Create a merge stream interleaving items from two streams.
 * Deterministic: alternates preference between a and b. */
ASX_API void asx_stream_merge_init(asx_stream *s, asx_stream_merge_state *state, asx_stream a,
                                   asx_stream b);

/* ------------------------------------------------------------------ */
/* Zip combinator — pairs items from two streams                      */
/* ------------------------------------------------------------------ */

typedef struct {
    void *a;
    void *b;
} asx_stream_zip_pair;

typedef struct {
    asx_stream a;
    asx_stream b;
    void *pending_a;
    int has_pending_a;
    asx_stream_zip_pair current;
} asx_stream_zip_state;

/* Create a zip stream pairing items from two streams.
 * Done when either stream is done. */
ASX_API void asx_stream_zip_init(asx_stream *s, asx_stream_zip_state *state, asx_stream a,
                                 asx_stream b);

/* ------------------------------------------------------------------ */
/* FilterMap combinator — filters and maps in one pass                */
/* ------------------------------------------------------------------ */

typedef void *(*asx_stream_filter_map_fn)(void *in_item, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_filter_map_fn fn;
    void *user_data;
} asx_stream_filter_map_state;

/* Initialize a filter-map stream wrapping an inner stream. */
ASX_API void asx_stream_filter_map_init(asx_stream *s, asx_stream_filter_map_state *state,
                                         asx_stream inner, asx_stream_filter_map_fn fn,
                                         void *user_data);

/* ------------------------------------------------------------------ */
/* TakeWhile combinator — yields while predicate holds                */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    asx_stream_filter_fn predicate;
    void *user_data;
    int done;
} asx_stream_take_while_state;

/* Create a take-while stream that yields items while the predicate holds. */
ASX_API void asx_stream_take_while_init(asx_stream *s, asx_stream_take_while_state *state,
                                         asx_stream inner, asx_stream_filter_fn predicate,
                                         void *user_data);

/* ------------------------------------------------------------------ */
/* Scan combinator — stateful map with accumulator                    */
/* ------------------------------------------------------------------ */

typedef void *(*asx_stream_scan_fn)(void *accumulator, void *item, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_scan_fn scan_fn;
    void *accumulator;
    void *user_data;
} asx_stream_scan_state;

/* Create a scan stream that applies an accumulating function to each item. */
ASX_API void asx_stream_scan_init(asx_stream *s, asx_stream_scan_state *state, asx_stream inner,
                                   asx_stream_scan_fn scan_fn, void *accumulator, void *user_data);

/* ------------------------------------------------------------------ */
/* Peekable adapter — lookahead without consuming                     */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    void *peeked;
    int has_peeked;
    int inner_done;
} asx_stream_peekable_state;

/* Initialize a peekable adapter wrapping an inner stream. */
ASX_API void asx_stream_peekable_init(asx_stream *s, asx_stream_peekable_state *state,
                                       asx_stream inner);

/* Poll for the next item without consuming it. */
ASX_API asx_stream_result asx_stream_peek(asx_stream_peekable_state *state, const asx_waker *waker,
                                           void **out_item);

/* ------------------------------------------------------------------ */
/* Inspect combinator — observe items without modifying               */
/* ------------------------------------------------------------------ */

typedef void (*asx_stream_inspect_fn)(const void *item, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_inspect_fn inspect_fn;
    void *user_data;
} asx_stream_inspect_state;

/* Create an inspect stream that observes each item via a callback. */
ASX_API void asx_stream_inspect_init(asx_stream *s, asx_stream_inspect_state *state,
                                      asx_stream inner, asx_stream_inspect_fn inspect_fn,
                                      void *user_data);

/* ------------------------------------------------------------------ */
/* Any terminal                                                        */
/* ------------------------------------------------------------------ */

/* Check if any item matches the predicate (short-circuits on first match). */
ASX_API ASX_MUST_USE asx_status asx_stream_any(asx_stream *s, asx_stream_filter_fn predicate,
                                                void *user_data, int *out_result);

/* ------------------------------------------------------------------ */
/* All terminal                                                        */
/* ------------------------------------------------------------------ */

/* Check if all items match the predicate (short-circuits on first failure). */
ASX_API ASX_MUST_USE asx_status asx_stream_all(asx_stream *s, asx_stream_filter_fn predicate,
                                                void *user_data, int *out_result);

/* ------------------------------------------------------------------ */
/* Collect terminal                                                    */
/* ------------------------------------------------------------------ */

/* Collect stream items into a caller-provided buffer up to max_items. */
ASX_API ASX_MUST_USE asx_status asx_stream_collect(asx_stream *s, void **buf, size_t max_items,
                                                    size_t *out_count);

/* ------------------------------------------------------------------ */
/* Fuse combinator — once DONE, always DONE                           */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    int done;
} asx_stream_fuse_state;

/* Create a fuse stream that permanently returns DONE after first exhaustion. */
ASX_API void asx_stream_fuse_init(asx_stream *s, asx_stream_fuse_state *state, asx_stream inner);

/* ------------------------------------------------------------------ */
/* Chunks combinator — batches items into fixed-size groups           */
/* ------------------------------------------------------------------ */

#ifndef ASX_STREAM_CHUNK_MAX
#define ASX_STREAM_CHUNK_MAX 16u
#endif

typedef struct {
    void *items[ASX_STREAM_CHUNK_MAX];
    size_t count;
} asx_stream_chunk;

typedef struct {
    asx_stream inner;
    size_t chunk_size;
    asx_stream_chunk current;
    int inner_done;
} asx_stream_chunks_state;

/* Batch items into chunks of chunk_size. The last chunk may be smaller.
 * Each poll_next yields a pointer to an asx_stream_chunk. */
ASX_API void asx_stream_chunks_init(asx_stream *s, asx_stream_chunks_state *state,
                                     asx_stream inner, size_t chunk_size);

/* ------------------------------------------------------------------ */
/* Throttle combinator — rate-limits items via token bucket           */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    uint64_t interval_ns;  /* minimum nanoseconds between items */
    uint64_t last_yield;   /* timestamp of last yielded item */
    int first;             /* 1 if no item has been yielded yet */
} asx_stream_throttle_state;

/* Rate-limit a stream to at most one item per interval_ns.
 * Items arriving too soon return ASX_STREAM_PENDING.
 * First item is always yielded immediately. */
ASX_API void asx_stream_throttle_init(asx_stream *s, asx_stream_throttle_state *state,
                                       asx_stream inner, uint64_t interval_ns);

/* ------------------------------------------------------------------ */
/* SkipWhile combinator — skips items while predicate holds           */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_stream inner;
    asx_stream_filter_fn predicate;
    void *user_data;
    int skipping; /* 1 while still skipping */
} asx_stream_skip_while_state;

/* Create a skip-while stream that discards items while the predicate holds. */
ASX_API void asx_stream_skip_while_init(asx_stream *s, asx_stream_skip_while_state *state,
                                         asx_stream inner, asx_stream_filter_fn predicate,
                                         void *user_data);

/* ------------------------------------------------------------------ */
/* Flatten combinator — flattens a stream of streams                  */
/* ------------------------------------------------------------------ */

/* Each item from the outer stream is itself an asx_stream*.
 * Flatten yields all items from each inner stream in sequence. */
typedef struct {
    asx_stream outer;
    asx_stream *current_inner; /* pointer to active inner stream, or NULL */
    int outer_done;
} asx_stream_flatten_state;

/* Create a flatten stream that yields items from a stream of streams. */
ASX_API void asx_stream_flatten_init(asx_stream *s, asx_stream_flatten_state *state,
                                      asx_stream outer);

/* ------------------------------------------------------------------ */
/* Dedup combinator — suppresses consecutive duplicate items          */
/* ------------------------------------------------------------------ */

typedef int (*asx_stream_eq_fn)(const void *a, const void *b, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_eq_fn eq_fn;
    void *user_data;
    void *last_item;  /* pointer to last yielded item */
    int has_last;
} asx_stream_dedup_state;

/* Suppress consecutive items where eq_fn returns nonzero.
 * Requires that item pointers from the inner stream remain valid
 * across consecutive poll_next calls (true for iter/array-backed
 * streams; not guaranteed for streams that reuse a single buffer). */
ASX_API void asx_stream_dedup_init(asx_stream *s, asx_stream_dedup_state *state,
                                    asx_stream inner, asx_stream_eq_fn eq_fn, void *user_data);

/* ------------------------------------------------------------------ */
/* FlatMap combinator — map then flatten                              */
/* ------------------------------------------------------------------ */

/* flat_map_fn: given an item, returns an asx_stream* to iterate.
 * The returned stream pointer must remain valid until drained. */
typedef asx_stream *(*asx_stream_flat_map_fn)(void *item, void *user_data);

typedef struct {
    asx_stream inner;
    asx_stream_flat_map_fn fn;
    void *user_data;
    asx_stream *current;  /* current sub-stream, or NULL */
    int outer_done;
} asx_stream_flat_map_state;

ASX_API void asx_stream_flat_map_init(asx_stream *s, asx_stream_flat_map_state *state,
                                       asx_stream inner, asx_stream_flat_map_fn fn,
                                       void *user_data);

/* ------------------------------------------------------------------ */
/* Window combinator — sliding window of items                        */
/* ------------------------------------------------------------------ */

#ifndef ASX_STREAM_WINDOW_MAX
#define ASX_STREAM_WINDOW_MAX 16u
#endif

typedef struct {
    void *items[ASX_STREAM_WINDOW_MAX];
    size_t count;
    size_t start; /* index of oldest item in circular buffer */
} asx_stream_window;

typedef struct {
    asx_stream inner;
    size_t window_size;
    asx_stream_window current;
    int filled;   /* 1 once window is full for first time */
    int inner_done;
} asx_stream_window_state;

/* Emit sliding windows of window_size consecutive items.
 * Each poll yields a pointer to asx_stream_window.
 * First yield occurs after window_size items are buffered.
 * Items are stored in a circular buffer: logical item i is at
 * items[(start + i) % count]. If stream is shorter than window_size,
 * a single partial window is yielded. */
ASX_API void asx_stream_window_init(asx_stream *s, asx_stream_window_state *state,
                                     asx_stream inner, size_t window_size);

/* ------------------------------------------------------------------ */
/* Nth terminal — get the nth item (0-indexed)                        */
/* ------------------------------------------------------------------ */

/* Skip n items and return the next one. Sets *out_item on success.
 * Returns ASX_OK if found, ASX_E_NOT_FOUND if stream exhausted. */
ASX_API ASX_MUST_USE asx_status asx_stream_nth(asx_stream *s, size_t n, void **out_item);

/* ------------------------------------------------------------------ */
/* Last terminal — consume stream and return last item                */
/* ------------------------------------------------------------------ */

/* Returns ASX_OK and sets *out_item to last item, or ASX_E_NOT_FOUND if empty. */
ASX_API ASX_MUST_USE asx_status asx_stream_last(asx_stream *s, void **out_item);

#ifdef __cplusplus
}
#endif

#endif /* ASX_STREAM_STREAM_H */
