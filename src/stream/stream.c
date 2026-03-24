/*
 * stream.c — stream processing primitives implementation
 *
 * Provides poll-based async iterators, combinators, and channel/time
 * source adapters.  Zero dynamic allocation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/stream/stream.h>
#include <asx/asx_config.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Core stream poll                                                    */
/* ------------------------------------------------------------------ */

asx_stream_result asx_stream_poll_next(asx_stream *s, const asx_waker *waker, void **out_item) {
    return s->poll_next(s->state, waker, out_item);
}

/* ------------------------------------------------------------------ */
/* Iter source                                                         */
/* ------------------------------------------------------------------ */

static asx_stream_result iter_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_iter_state *it = (asx_stream_iter_state *)state;
    (void)waker;

    if (it->index >= it->count) { return ASX_STREAM_DONE; }

    *out_item = (void *)((const char *)it->items + it->index * it->item_size);
    it->index++;
    return ASX_STREAM_READY;
}

void asx_stream_iter_init(asx_stream *s, asx_stream_iter_state *state, const void *items,
                          size_t item_size, size_t count) {
    state->items = items;
    state->item_size = item_size;
    state->count = count;
    state->index = 0;
    s->poll_next = iter_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Map combinator                                                      */
/* ------------------------------------------------------------------ */

static asx_stream_result map_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_map_state *ms = (asx_stream_map_state *)state;
    void *inner_item = NULL;
    asx_stream_result r = asx_stream_poll_next(&ms->inner, waker, &inner_item);

    if (r == ASX_STREAM_READY) { *out_item = ms->map_fn(inner_item, ms->user_data); }
    return r;
}

void asx_stream_map_init(asx_stream *s, asx_stream_map_state *state, asx_stream inner,
                         asx_stream_map_fn map_fn, void *user_data) {
    state->inner = inner;
    state->map_fn = map_fn;
    state->user_data = user_data;
    s->poll_next = map_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Filter combinator                                                   */
/* ------------------------------------------------------------------ */

static asx_stream_result filter_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_filter_state *fs = (asx_stream_filter_state *)state;

    for (;;) {
        void *inner_item = NULL;
        asx_stream_result r = asx_stream_poll_next(&fs->inner, waker, &inner_item);

        if (r != ASX_STREAM_READY) { return r; }
        if (fs->filter_fn(inner_item, fs->user_data)) {
            *out_item = inner_item;
            return ASX_STREAM_READY;
        }
        /* Item rejected, try next */
    }
}

void asx_stream_filter_init(asx_stream *s, asx_stream_filter_state *state, asx_stream inner,
                            asx_stream_filter_fn filter_fn, void *user_data) {
    state->inner = inner;
    state->filter_fn = filter_fn;
    state->user_data = user_data;
    s->poll_next = filter_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Take combinator                                                     */
/* ------------------------------------------------------------------ */

static asx_stream_result take_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_take_state *ts = (asx_stream_take_state *)state;

    if (ts->remaining == 0) { return ASX_STREAM_DONE; }

    asx_stream_result r = asx_stream_poll_next(&ts->inner, waker, out_item);
    if (r == ASX_STREAM_READY) { ts->remaining--; }
    return r;
}

void asx_stream_take_init(asx_stream *s, asx_stream_take_state *state, asx_stream inner, size_t n) {
    state->inner = inner;
    state->remaining = n;
    s->poll_next = take_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Skip combinator                                                     */
/* ------------------------------------------------------------------ */

static asx_stream_result skip_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_skip_state *ss = (asx_stream_skip_state *)state;

    /* Skip phase: consume and discard items */
    while (ss->remaining > 0) {
        void *discard = NULL;
        asx_stream_result r = asx_stream_poll_next(&ss->inner, waker, &discard);
        if (r != ASX_STREAM_READY) { return r; }
        ss->remaining--;
    }

    /* Pass-through phase */
    return asx_stream_poll_next(&ss->inner, waker, out_item);
}

void asx_stream_skip_init(asx_stream *s, asx_stream_skip_state *state, asx_stream inner, size_t n) {
    state->inner = inner;
    state->remaining = n;
    s->poll_next = skip_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Chain combinator                                                    */
/* ------------------------------------------------------------------ */

static asx_stream_result chain_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_chain_state *cs = (asx_stream_chain_state *)state;

    if (!cs->first_done) {
        asx_stream_result r = asx_stream_poll_next(&cs->first, waker, out_item);
        if (r != ASX_STREAM_DONE) { return r; }
        cs->first_done = 1;
    }

    return asx_stream_poll_next(&cs->second, waker, out_item);
}

void asx_stream_chain_init(asx_stream *s, asx_stream_chain_state *state, asx_stream first,
                           asx_stream second) {
    state->first = first;
    state->second = second;
    state->first_done = 0;
    s->poll_next = chain_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Enumerate combinator                                                */
/* ------------------------------------------------------------------ */

static asx_stream_result enumerate_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_enumerate_state *es = (asx_stream_enumerate_state *)state;
    void *inner_item = NULL;
    asx_stream_result r = asx_stream_poll_next(&es->inner, waker, &inner_item);

    if (r == ASX_STREAM_READY) {
        es->current.index = es->index;
        es->current.item = inner_item;
        *out_item = &es->current;
        es->index++;
    }
    return r;
}

void asx_stream_enumerate_init(asx_stream *s, asx_stream_enumerate_state *state, asx_stream inner) {
    state->inner = inner;
    state->index = 0;
    memset(&state->current, 0, sizeof(state->current));
    s->poll_next = enumerate_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Merge combinator                                                    */
/* ------------------------------------------------------------------ */

static asx_stream_result merge_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_merge_state *ms = (asx_stream_merge_state *)state;
    asx_stream *first, *second;
    int *first_done, *second_done;

    /* Deterministic fairness: alternate which stream is polled first */
    if (ms->prefer_a) {
        first = &ms->a;
        second = &ms->b;
        first_done = &ms->a_done;
        second_done = &ms->b_done;
    } else {
        first = &ms->b;
        second = &ms->a;
        first_done = &ms->b_done;
        second_done = &ms->a_done;
    }
    ms->prefer_a = !ms->prefer_a;

    /* Try preferred stream first */
    if (!*first_done) {
        asx_stream_result r = asx_stream_poll_next(first, waker, out_item);
        if (r == ASX_STREAM_READY) return ASX_STREAM_READY;
        if (r == ASX_STREAM_DONE) *first_done = 1;
    }

    /* Try other stream */
    if (!*second_done) {
        asx_stream_result r = asx_stream_poll_next(second, waker, out_item);
        if (r == ASX_STREAM_READY) return ASX_STREAM_READY;
        if (r == ASX_STREAM_DONE) *second_done = 1;
    }

    if (ms->a_done && ms->b_done) return ASX_STREAM_DONE;
    return ASX_STREAM_PENDING;
}

void asx_stream_merge_init(asx_stream *s, asx_stream_merge_state *state, asx_stream a,
                           asx_stream b) {
    state->a = a;
    state->b = b;
    state->a_done = 0;
    state->b_done = 0;
    state->prefer_a = 1;
    s->poll_next = merge_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Zip combinator                                                      */
/* ------------------------------------------------------------------ */

static asx_stream_result zip_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_zip_state *zs = (asx_stream_zip_state *)state;
    void *item_a = NULL, *item_b = NULL;

    if (zs->has_pending_a) {
        item_a = zs->pending_a;
    } else {
        asx_stream_result ra = asx_stream_poll_next(&zs->a, waker, &item_a);
        if (ra != ASX_STREAM_READY) return ra;
        zs->pending_a = item_a;
        zs->has_pending_a = 1;
    }

    {
        asx_stream_result rb = asx_stream_poll_next(&zs->b, waker, &item_b);
        if (rb != ASX_STREAM_READY) {
            if (rb == ASX_STREAM_DONE) {
                zs->pending_a = NULL;
                zs->has_pending_a = 0;
            }
            return rb;
        }
    }

    zs->current.a = item_a;
    zs->current.b = item_b;
    zs->pending_a = NULL;
    zs->has_pending_a = 0;
    *out_item = &zs->current;
    return ASX_STREAM_READY;
}

void asx_stream_zip_init(asx_stream *s, asx_stream_zip_state *state, asx_stream a, asx_stream b) {
    state->a = a;
    state->b = b;
    state->pending_a = NULL;
    state->has_pending_a = 0;
    memset(&state->current, 0, sizeof(state->current));
    s->poll_next = zip_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Terminal operations                                                 */
/* ------------------------------------------------------------------ */

asx_status asx_stream_fold(asx_stream *s, void *accumulator, asx_stream_fold_fn fold_fn,
                           void *user_data) {
    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);

        if (r == ASX_STREAM_DONE) return ASX_OK;
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        fold_fn(accumulator, item, user_data);
    }
}

asx_status asx_stream_count(asx_stream *s, size_t *out_count) {
    size_t count = 0;
    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);

        if (r == ASX_STREAM_DONE) {
            *out_count = count;
            return ASX_OK;
        }
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        count++;
    }
}

asx_status asx_stream_for_each(asx_stream *s, asx_stream_foreach_fn fn, void *user_data) {
    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);

        if (r == ASX_STREAM_DONE) return ASX_OK;
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        fn(item, user_data);
    }
}

/* ------------------------------------------------------------------ */
/* Channel source adapters                                             */
/* ------------------------------------------------------------------ */

/*
 * Note: These adapters store the channel/watch/broadcast identifiers
 * but delegate actual receive operations to the existing channel APIs.
 * The poll functions call the appropriate try_recv and translate the
 * result to stream semantics.
 *
 * Because channel_try_recv/watch_recv/broadcast_try_recv are defined
 * in the core library (already linked), we forward-declare them here
 * rather than including all channel headers to avoid circular deps.
 */

/* Forward declarations — defined in core/channel.c, core/watch.c, etc. */
extern asx_status asx_channel_try_recv(uint16_t id, uint64_t *out_value);
extern asx_status asx_watch_recv(void *rx, uint64_t *out_value);
extern int asx_watch_has_changed(const void *rx);
extern asx_status asx_broadcast_try_recv(void *rx, uint64_t *out_value);

static asx_stream_result receiver_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_receiver_state *rs = (asx_stream_receiver_state *)state;
    asx_status s;
    (void)waker;

    s = asx_channel_try_recv(rs->channel_id, (uint64_t *)rs->recv_buf);
    if (s == ASX_OK) {
        *out_item = rs->recv_buf;
        return ASX_STREAM_READY;
    }
    if (s == ASX_E_DISCONNECTED) { return ASX_STREAM_DONE; }
    /* ASX_E_WOULD_BLOCK = pending */
    return ASX_STREAM_PENDING;
}

void asx_stream_from_receiver(asx_stream *s, asx_stream_receiver_state *state, uint16_t channel_id,
                              void *recv_buf) {
    state->channel_id = channel_id;
    state->recv_buf = recv_buf;
    s->poll_next = receiver_poll;
    s->state = state;
}

static asx_stream_result watch_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_watch_state *ws = (asx_stream_watch_state *)state;
    (void)waker;

    if (!asx_watch_has_changed(ws->watch_rx)) { return ASX_STREAM_PENDING; }

    {
        asx_status s = asx_watch_recv(ws->watch_rx, (uint64_t *)ws->value_buf);
        if (s == ASX_OK) {
            *out_item = ws->value_buf;
            return ASX_STREAM_READY;
        }
        return ASX_STREAM_DONE;
    }
}

void asx_stream_from_watch(asx_stream *s, asx_stream_watch_state *state, void *watch_rx,
                           void *value_buf) {
    state->watch_rx = watch_rx;
    state->value_buf = value_buf;
    s->poll_next = watch_poll;
    s->state = state;
}

static asx_stream_result broadcast_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_broadcast_state *bs = (asx_stream_broadcast_state *)state;
    asx_status s;
    (void)waker;

    s = asx_broadcast_try_recv(bs->broadcast_rx, (uint64_t *)bs->recv_buf);
    if (s == ASX_OK) {
        *out_item = bs->recv_buf;
        return ASX_STREAM_READY;
    }
    if (s == ASX_E_DISCONNECTED) { return ASX_STREAM_DONE; }
    return ASX_STREAM_PENDING;
}

void asx_stream_from_broadcast(asx_stream *s, asx_stream_broadcast_state *state, void *broadcast_rx,
                               void *recv_buf) {
    state->broadcast_rx = broadcast_rx;
    state->recv_buf = recv_buf;
    s->poll_next = broadcast_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* FilterMap combinator                                                */
/* ------------------------------------------------------------------ */

static asx_stream_result filter_map_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_filter_map_state *fm = (asx_stream_filter_map_state *)state;
    for (;;) {
        void *inner_item = NULL;
        void *mapped;
        asx_stream_result r = asx_stream_poll_next(&fm->inner, waker, &inner_item);
        if (r != ASX_STREAM_READY) return r;
        mapped = fm->fn(inner_item, fm->user_data);
        if (mapped != NULL) { *out_item = mapped; return ASX_STREAM_READY; }
    }
}

void asx_stream_filter_map_init(asx_stream *s, asx_stream_filter_map_state *state,
                                 asx_stream inner, asx_stream_filter_map_fn fn, void *user_data) {
    state->inner = inner;
    state->fn = fn;
    state->user_data = user_data;
    s->poll_next = filter_map_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* TakeWhile combinator                                                */
/* ------------------------------------------------------------------ */

static asx_stream_result take_while_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_take_while_state *tw = (asx_stream_take_while_state *)state;
    void *inner_item = NULL;
    asx_stream_result r;
    if (tw->done) return ASX_STREAM_DONE;
    r = asx_stream_poll_next(&tw->inner, waker, &inner_item);
    if (r != ASX_STREAM_READY) return r;
    if (tw->predicate(inner_item, tw->user_data)) { *out_item = inner_item; return ASX_STREAM_READY; }
    tw->done = 1;
    return ASX_STREAM_DONE;
}

void asx_stream_take_while_init(asx_stream *s, asx_stream_take_while_state *state,
                                 asx_stream inner, asx_stream_filter_fn predicate, void *user_data) {
    state->inner = inner;
    state->predicate = predicate;
    state->user_data = user_data;
    state->done = 0;
    s->poll_next = take_while_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Scan combinator                                                     */
/* ------------------------------------------------------------------ */

static asx_stream_result scan_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_scan_state *sc = (asx_stream_scan_state *)state;
    void *inner_item = NULL;
    asx_stream_result r = asx_stream_poll_next(&sc->inner, waker, &inner_item);
    if (r != ASX_STREAM_READY) return r;
    *out_item = sc->scan_fn(sc->accumulator, inner_item, sc->user_data);
    return ASX_STREAM_READY;
}

void asx_stream_scan_init(asx_stream *s, asx_stream_scan_state *state, asx_stream inner,
                           asx_stream_scan_fn scan_fn, void *accumulator, void *user_data) {
    state->inner = inner;
    state->scan_fn = scan_fn;
    state->accumulator = accumulator;
    state->user_data = user_data;
    s->poll_next = scan_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Peekable adapter                                                    */
/* ------------------------------------------------------------------ */

static asx_stream_result peekable_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_peekable_state *pk = (asx_stream_peekable_state *)state;
    if (pk->has_peeked) { *out_item = pk->peeked; pk->has_peeked = 0; pk->peeked = NULL; return ASX_STREAM_READY; }
    if (pk->inner_done) return ASX_STREAM_DONE;
    return asx_stream_poll_next(&pk->inner, waker, out_item);
}

void asx_stream_peekable_init(asx_stream *s, asx_stream_peekable_state *state, asx_stream inner) {
    state->inner = inner;
    state->peeked = NULL;
    state->has_peeked = 0;
    state->inner_done = 0;
    s->poll_next = peekable_poll;
    s->state = state;
}

asx_stream_result asx_stream_peek(asx_stream_peekable_state *state, const asx_waker *waker,
                                   void **out_item) {
    asx_stream_result r;
    if (state->has_peeked) { *out_item = state->peeked; return ASX_STREAM_READY; }
    if (state->inner_done) return ASX_STREAM_DONE;
    r = asx_stream_poll_next(&state->inner, waker, &state->peeked);
    if (r == ASX_STREAM_READY) { state->has_peeked = 1; *out_item = state->peeked; }
    else if (r == ASX_STREAM_DONE) { state->inner_done = 1; }
    return r;
}

/* ------------------------------------------------------------------ */
/* Inspect combinator                                                  */
/* ------------------------------------------------------------------ */

static asx_stream_result inspect_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_inspect_state *is = (asx_stream_inspect_state *)state;
    void *inner_item = NULL;
    asx_stream_result r = asx_stream_poll_next(&is->inner, waker, &inner_item);
    if (r == ASX_STREAM_READY) { is->inspect_fn(inner_item, is->user_data); *out_item = inner_item; }
    return r;
}

void asx_stream_inspect_init(asx_stream *s, asx_stream_inspect_state *state, asx_stream inner,
                              asx_stream_inspect_fn inspect_fn, void *user_data) {
    state->inner = inner;
    state->inspect_fn = inspect_fn;
    state->user_data = user_data;
    s->poll_next = inspect_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Any terminal                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_stream_any(asx_stream *s, asx_stream_filter_fn predicate, void *user_data,
                           int *out_result) {
    if (out_result == NULL) return ASX_E_INVALID_ARGUMENT;
    *out_result = 0;
    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);
        if (r == ASX_STREAM_DONE) return ASX_OK;
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        if (predicate(item, user_data)) { *out_result = 1; return ASX_OK; }
    }
}

/* ------------------------------------------------------------------ */
/* All terminal                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_stream_all(asx_stream *s, asx_stream_filter_fn predicate, void *user_data,
                           int *out_result) {
    if (out_result == NULL) return ASX_E_INVALID_ARGUMENT;
    *out_result = 1;
    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);
        if (r == ASX_STREAM_DONE) return ASX_OK;
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        if (!predicate(item, user_data)) { *out_result = 0; return ASX_OK; }
    }
}

/* ------------------------------------------------------------------ */
/* Collect terminal                                                    */
/* ------------------------------------------------------------------ */

asx_status asx_stream_collect(asx_stream *s, void **buf, size_t max_items, size_t *out_count) {
    size_t count = 0;
    if (buf == NULL || out_count == NULL) return ASX_E_INVALID_ARGUMENT;
    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);
        if (r == ASX_STREAM_DONE) { *out_count = count; return ASX_OK; }
        if (r == ASX_STREAM_PENDING) { *out_count = count; return ASX_E_WOULD_BLOCK; }
        if (count >= max_items) { *out_count = count; return ASX_E_RESOURCE_EXHAUSTED; }
        buf[count] = item;
        count++;
    }
}

/* ------------------------------------------------------------------ */
/* Fuse combinator                                                     */
/* ------------------------------------------------------------------ */

static asx_stream_result fuse_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_fuse_state *fs = (asx_stream_fuse_state *)state;
    asx_stream_result r;
    if (fs->done) return ASX_STREAM_DONE;
    r = asx_stream_poll_next(&fs->inner, waker, out_item);
    if (r == ASX_STREAM_DONE) fs->done = 1;
    return r;
}

void asx_stream_fuse_init(asx_stream *s, asx_stream_fuse_state *state, asx_stream inner) {
    state->inner = inner;
    state->done = 0;
    s->poll_next = fuse_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Chunks combinator                                                   */
/* ------------------------------------------------------------------ */

static asx_stream_result chunks_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_chunks_state *cs = (asx_stream_chunks_state *)state;
    cs->current.count = 0;

    if (cs->inner_done) return ASX_STREAM_DONE;

    while (cs->current.count < cs->chunk_size) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(&cs->inner, waker, &item);
        if (r == ASX_STREAM_DONE) {
            cs->inner_done = 1;
            break;
        }
        if (r == ASX_STREAM_PENDING) {
            if (cs->current.count > 0) break; /* yield partial chunk */
            return ASX_STREAM_PENDING;
        }
        if (cs->current.count < ASX_STREAM_CHUNK_MAX) {
            cs->current.items[cs->current.count] = item;
            cs->current.count++;
        }
    }

    if (cs->current.count == 0) return ASX_STREAM_DONE;
    *out_item = &cs->current;
    return ASX_STREAM_READY;
}

void asx_stream_chunks_init(asx_stream *s, asx_stream_chunks_state *state, asx_stream inner,
                             size_t chunk_size) {
    state->inner = inner;
    state->chunk_size = chunk_size;
    if (state->chunk_size > ASX_STREAM_CHUNK_MAX) state->chunk_size = ASX_STREAM_CHUNK_MAX;
    if (state->chunk_size == 0) state->chunk_size = 1;
    state->inner_done = 0;
    state->current.count = 0;
    s->poll_next = chunks_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Throttle combinator                                                 */
/* ------------------------------------------------------------------ */

static asx_stream_result throttle_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_throttle_state *ts = (asx_stream_throttle_state *)state;
    asx_stream_result r;
    asx_time now = 0;

    /* Check if enough time has passed */
    if (!ts->first) {
        asx_runtime_now_ns(&now);
        if ((uint64_t)now < ts->last_yield + ts->interval_ns) {
            return ASX_STREAM_PENDING;
        }
    }

    r = asx_stream_poll_next(&ts->inner, waker, out_item);
    if (r == ASX_STREAM_READY) {
        if (ts->first) {
            asx_runtime_now_ns(&now);
            ts->first = 0;
        }
        ts->last_yield = (uint64_t)now;
    }
    return r;
}

void asx_stream_throttle_init(asx_stream *s, asx_stream_throttle_state *state, asx_stream inner,
                               uint64_t interval_ns) {
    state->inner = inner;
    state->interval_ns = interval_ns;
    state->last_yield = 0;
    state->first = 1;
    s->poll_next = throttle_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* SkipWhile combinator                                                */
/* ------------------------------------------------------------------ */

static asx_stream_result skip_while_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_skip_while_state *sw = (asx_stream_skip_while_state *)state;

    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(&sw->inner, waker, &item);
        if (r != ASX_STREAM_READY) return r;

        if (!sw->skipping) {
            *out_item = item;
            return ASX_STREAM_READY;
        }

        if (!sw->predicate(item, sw->user_data)) {
            sw->skipping = 0;
            *out_item = item;
            return ASX_STREAM_READY;
        }
    }
}

void asx_stream_skip_while_init(asx_stream *s, asx_stream_skip_while_state *state,
                                 asx_stream inner, asx_stream_filter_fn predicate,
                                 void *user_data) {
    state->inner = inner;
    state->predicate = predicate;
    state->user_data = user_data;
    state->skipping = 1;
    s->poll_next = skip_while_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Flatten combinator                                                  */
/* ------------------------------------------------------------------ */

static asx_stream_result flatten_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_flatten_state *fl = (asx_stream_flatten_state *)state;

    for (;;) {
        /* Try current inner stream */
        if (fl->current_inner != NULL) {
            asx_stream_result r = asx_stream_poll_next(fl->current_inner, waker, out_item);
            if (r == ASX_STREAM_READY) return ASX_STREAM_READY;
            if (r == ASX_STREAM_PENDING) return ASX_STREAM_PENDING;
            /* Inner stream done, get next from outer */
            fl->current_inner = NULL;
        }

        if (fl->outer_done) return ASX_STREAM_DONE;

        /* Get next inner stream from outer */
        {
            void *inner_ptr = NULL;
            asx_stream_result r = asx_stream_poll_next(&fl->outer, waker, &inner_ptr);
            if (r == ASX_STREAM_DONE) { fl->outer_done = 1; return ASX_STREAM_DONE; }
            if (r == ASX_STREAM_PENDING) return ASX_STREAM_PENDING;
            fl->current_inner = *(asx_stream **)inner_ptr;
        }
    }
}

void asx_stream_flatten_init(asx_stream *s, asx_stream_flatten_state *state, asx_stream outer) {
    state->outer = outer;
    state->current_inner = NULL;
    state->outer_done = 0;
    s->poll_next = flatten_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Dedup combinator                                                    */
/* ------------------------------------------------------------------ */

static asx_stream_result dedup_poll(void *state, const asx_waker *waker, void **out_item) {
    asx_stream_dedup_state *dd = (asx_stream_dedup_state *)state;

    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(&dd->inner, waker, &item);
        if (r != ASX_STREAM_READY) return r;

        if (dd->has_last && dd->eq_fn(dd->last_item, item, dd->user_data)) {
            continue; /* duplicate, skip */
        }

        dd->last_item = item;
        dd->has_last = 1;
        *out_item = item;
        return ASX_STREAM_READY;
    }
}

void asx_stream_dedup_init(asx_stream *s, asx_stream_dedup_state *state, asx_stream inner,
                            asx_stream_eq_fn eq_fn, void *user_data) {
    state->inner = inner;
    state->eq_fn = eq_fn;
    state->user_data = user_data;
    state->last_item = NULL;
    state->has_last = 0;
    s->poll_next = dedup_poll;
    s->state = state;
}

/* ------------------------------------------------------------------ */
/* Nth terminal                                                        */
/* ------------------------------------------------------------------ */

asx_status asx_stream_nth(asx_stream *s, size_t n, void **out_item) {
    size_t i = 0;
    if (out_item == NULL) return ASX_E_INVALID_ARGUMENT;

    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);
        if (r == ASX_STREAM_DONE) return ASX_E_NOT_FOUND;
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        if (i == n) { *out_item = item; return ASX_OK; }
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* Last terminal                                                       */
/* ------------------------------------------------------------------ */

asx_status asx_stream_last(asx_stream *s, void **out_item) {
    int found = 0;
    if (out_item == NULL) return ASX_E_INVALID_ARGUMENT;

    for (;;) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);
        if (r == ASX_STREAM_DONE) {
            return found ? ASX_OK : ASX_E_NOT_FOUND;
        }
        if (r == ASX_STREAM_PENDING) return ASX_E_WOULD_BLOCK;
        *out_item = item;
        found = 1;
    }
}
