/*
 * test_stream.c — unit tests for stream processing primitives
 *
 * Tests iter source, map/filter/take/skip/chain/enumerate/merge/zip
 * combinators, and fold/count/for_each terminal operations.
 *
 * Bead: bd-1eqo.13.1
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/stream/stream.h>
#include <string.h>

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

/* Helper: collect stream items into an int array. Returns count. */
static size_t collect_ints(asx_stream *s, int *out, size_t max) {
    size_t n = 0;
    while (n < max) {
        void *item = NULL;
        asx_stream_result r = asx_stream_poll_next(s, NULL, &item);
        if (r == ASX_STREAM_DONE) break;
        if (r == ASX_STREAM_PENDING) break;
        out[n++] = *(int *)item;
    }
    return n;
}

typedef struct {
    const asx_stream_result *results;
    const int *values;
    size_t len;
    size_t index;
} scripted_int_stream_state;

static asx_stream_result scripted_int_poll(void *state, const asx_waker *waker, void **out_item) {
    scripted_int_stream_state *ss = (scripted_int_stream_state *)state;
    (void)waker;

    if (ss->index >= ss->len) return ASX_STREAM_DONE;

    {
        size_t idx = ss->index++;
        if (ss->results[idx] == ASX_STREAM_READY) {
            *out_item = (void *)&ss->values[idx];
        }
        return ss->results[idx];
    }
}

static void scripted_int_stream_init(asx_stream *s, scripted_int_stream_state *state,
                                     const asx_stream_result *results, const int *values,
                                     size_t len) {
    state->results = results;
    state->values = values;
    state->len = len;
    state->index = 0;
    s->poll_next = scripted_int_poll;
    s->state = state;
}

/* ================================================================== */
/* Iter source tests                                                   */
/* ================================================================== */

TEST(iter_empty) {
    asx_stream s;
    asx_stream_iter_state state;
    void *item = NULL;
    asx_stream_iter_init(&s, &state, NULL, sizeof(int), 0);
    ASSERT_EQ(asx_stream_poll_next(&s, NULL, &item), ASX_STREAM_DONE);
}

TEST(iter_yields_all_items) {
    int data[] = {10, 20, 30, 40, 50};
    asx_stream s;
    asx_stream_iter_state state;
    int collected[5];
    size_t n;

    asx_stream_iter_init(&s, &state, data, sizeof(int), 5);
    n = collect_ints(&s, collected, 5);

    ASSERT_EQ(n, 5u);
    ASSERT_EQ(collected[0], 10);
    ASSERT_EQ(collected[1], 20);
    ASSERT_EQ(collected[2], 30);
    ASSERT_EQ(collected[3], 40);
    ASSERT_EQ(collected[4], 50);
}

TEST(iter_done_after_exhaustion) {
    int data[] = {1};
    asx_stream s;
    asx_stream_iter_state state;
    void *item = NULL;

    asx_stream_iter_init(&s, &state, data, sizeof(int), 1);
    ASSERT_EQ(asx_stream_poll_next(&s, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 1);
    ASSERT_EQ(asx_stream_poll_next(&s, NULL, &item), ASX_STREAM_DONE);
    /* Double DONE is fine */
    ASSERT_EQ(asx_stream_poll_next(&s, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* Map tests                                                           */
/* ================================================================== */

static int map_doubled_val;
static void *map_double(void *item, void *user_data) {
    (void)user_data;
    map_doubled_val = *(int *)item * 2;
    return &map_doubled_val;
}

TEST(map_transforms_items) {
    int data[] = {1, 2, 3};
    asx_stream inner, mapped;
    asx_stream_iter_state iter_state;
    asx_stream_map_state map_state;
    int collected[3];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 3);
    asx_stream_map_init(&mapped, &map_state, inner, map_double, NULL);
    n = collect_ints(&mapped, collected, 3);

    ASSERT_EQ(n, 3u);
    ASSERT_EQ(collected[0], 2);
    ASSERT_EQ(collected[1], 4);
    ASSERT_EQ(collected[2], 6);
}

/* ================================================================== */
/* Filter tests                                                        */
/* ================================================================== */

static int filter_even(const void *item, void *user_data) {
    (void)user_data;
    return (*(const int *)item % 2) == 0;
}

TEST(filter_yields_matching) {
    int data[] = {1, 2, 3, 4, 5, 6};
    asx_stream inner, filtered;
    asx_stream_iter_state iter_state;
    asx_stream_filter_state filter_state;
    int collected[6];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 6);
    asx_stream_filter_init(&filtered, &filter_state, inner, filter_even, NULL);
    n = collect_ints(&filtered, collected, 6);

    ASSERT_EQ(n, 3u);
    ASSERT_EQ(collected[0], 2);
    ASSERT_EQ(collected[1], 4);
    ASSERT_EQ(collected[2], 6);
}

TEST(filter_none_match) {
    int data[] = {1, 3, 5};
    asx_stream inner, filtered;
    asx_stream_iter_state iter_state;
    asx_stream_filter_state filter_state;
    int collected[3];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 3);
    asx_stream_filter_init(&filtered, &filter_state, inner, filter_even, NULL);
    n = collect_ints(&filtered, collected, 3);

    ASSERT_EQ(n, 0u);
}

/* ================================================================== */
/* Take tests                                                          */
/* ================================================================== */

TEST(take_limits_count) {
    int data[] = {10, 20, 30, 40, 50};
    asx_stream inner, taken;
    asx_stream_iter_state iter_state;
    asx_stream_take_state take_state;
    int collected[5];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 5);
    asx_stream_take_init(&taken, &take_state, inner, 3);
    n = collect_ints(&taken, collected, 5);

    ASSERT_EQ(n, 3u);
    ASSERT_EQ(collected[0], 10);
    ASSERT_EQ(collected[1], 20);
    ASSERT_EQ(collected[2], 30);
}

TEST(take_zero_yields_none) {
    int data[] = {1, 2, 3};
    asx_stream inner, taken;
    asx_stream_iter_state iter_state;
    asx_stream_take_state take_state;
    void *item = NULL;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 3);
    asx_stream_take_init(&taken, &take_state, inner, 0);
    ASSERT_EQ(asx_stream_poll_next(&taken, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* Skip tests                                                          */
/* ================================================================== */

TEST(skip_skips_items) {
    int data[] = {10, 20, 30, 40, 50};
    asx_stream inner, skipped;
    asx_stream_iter_state iter_state;
    asx_stream_skip_state skip_state;
    int collected[5];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 5);
    asx_stream_skip_init(&skipped, &skip_state, inner, 2);
    n = collect_ints(&skipped, collected, 5);

    ASSERT_EQ(n, 3u);
    ASSERT_EQ(collected[0], 30);
    ASSERT_EQ(collected[1], 40);
    ASSERT_EQ(collected[2], 50);
}

TEST(skip_all_yields_none) {
    int data[] = {1, 2};
    asx_stream inner, skipped;
    asx_stream_iter_state iter_state;
    asx_stream_skip_state skip_state;
    int collected[2];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 2);
    asx_stream_skip_init(&skipped, &skip_state, inner, 5);
    n = collect_ints(&skipped, collected, 2);

    ASSERT_EQ(n, 0u);
}

/* ================================================================== */
/* Chain tests                                                         */
/* ================================================================== */

TEST(chain_concatenates) {
    int a[] = {1, 2};
    int b[] = {3, 4, 5};
    asx_stream sa, sb, chained;
    asx_stream_iter_state iter_a, iter_b;
    asx_stream_chain_state chain_state;
    int collected[5];
    size_t n;

    asx_stream_iter_init(&sa, &iter_a, a, sizeof(int), 2);
    asx_stream_iter_init(&sb, &iter_b, b, sizeof(int), 3);
    asx_stream_chain_init(&chained, &chain_state, sa, sb);
    n = collect_ints(&chained, collected, 5);

    ASSERT_EQ(n, 5u);
    ASSERT_EQ(collected[0], 1);
    ASSERT_EQ(collected[1], 2);
    ASSERT_EQ(collected[2], 3);
    ASSERT_EQ(collected[3], 4);
    ASSERT_EQ(collected[4], 5);
}

TEST(chain_first_empty) {
    int b[] = {10, 20};
    asx_stream sa, sb, chained;
    asx_stream_iter_state iter_a, iter_b;
    asx_stream_chain_state chain_state;
    int collected[2];
    size_t n;

    asx_stream_iter_init(&sa, &iter_a, NULL, sizeof(int), 0);
    asx_stream_iter_init(&sb, &iter_b, b, sizeof(int), 2);
    asx_stream_chain_init(&chained, &chain_state, sa, sb);
    n = collect_ints(&chained, collected, 2);

    ASSERT_EQ(n, 2u);
    ASSERT_EQ(collected[0], 10);
    ASSERT_EQ(collected[1], 20);
}

/* ================================================================== */
/* Enumerate tests                                                     */
/* ================================================================== */

TEST(enumerate_adds_index) {
    int data[] = {100, 200, 300};
    asx_stream inner, enumerated;
    asx_stream_iter_state iter_state;
    asx_stream_enumerate_state enum_state;
    void *item = NULL;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 3);
    asx_stream_enumerate_init(&enumerated, &enum_state, inner);

    ASSERT_EQ(asx_stream_poll_next(&enumerated, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(((asx_stream_indexed_item *)item)->index, 0u);
    ASSERT_EQ(*(int *)((asx_stream_indexed_item *)item)->item, 100);

    ASSERT_EQ(asx_stream_poll_next(&enumerated, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(((asx_stream_indexed_item *)item)->index, 1u);
    ASSERT_EQ(*(int *)((asx_stream_indexed_item *)item)->item, 200);

    ASSERT_EQ(asx_stream_poll_next(&enumerated, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(((asx_stream_indexed_item *)item)->index, 2u);
    ASSERT_EQ(*(int *)((asx_stream_indexed_item *)item)->item, 300);

    ASSERT_EQ(asx_stream_poll_next(&enumerated, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* Merge tests                                                         */
/* ================================================================== */

TEST(merge_interleaves) {
    int a[] = {1, 3, 5};
    int b[] = {2, 4};
    asx_stream sa, sb, merged;
    asx_stream_iter_state iter_a, iter_b;
    asx_stream_merge_state merge_state;
    int collected[5];
    size_t n;

    asx_stream_iter_init(&sa, &iter_a, a, sizeof(int), 3);
    asx_stream_iter_init(&sb, &iter_b, b, sizeof(int), 2);
    asx_stream_merge_init(&merged, &merge_state, sa, sb);
    n = collect_ints(&merged, collected, 5);

    /* All 5 items must be present */
    ASSERT_EQ(n, 5u);

    /* Verify all items appeared (order depends on alternation) */
    {
        int sum = 0;
        size_t i;
        for (i = 0; i < n; i++) sum += collected[i];
        ASSERT_EQ(sum, 15); /* 1+2+3+4+5 */
    }
}

TEST(merge_one_empty) {
    int a[] = {10, 20};
    asx_stream sa, sb, merged;
    asx_stream_iter_state iter_a, iter_b;
    asx_stream_merge_state merge_state;
    int collected[2];
    size_t n;

    asx_stream_iter_init(&sa, &iter_a, a, sizeof(int), 2);
    asx_stream_iter_init(&sb, &iter_b, NULL, sizeof(int), 0);
    asx_stream_merge_init(&merged, &merge_state, sa, sb);
    n = collect_ints(&merged, collected, 2);

    ASSERT_EQ(n, 2u);
}

/* ================================================================== */
/* Zip tests                                                           */
/* ================================================================== */

TEST(zip_pairs_items) {
    int a[] = {1, 2, 3};
    int b[] = {10, 20, 30};
    asx_stream sa, sb, zipped;
    asx_stream_iter_state iter_a, iter_b;
    asx_stream_zip_state zip_state;
    void *item = NULL;

    asx_stream_iter_init(&sa, &iter_a, a, sizeof(int), 3);
    asx_stream_iter_init(&sb, &iter_b, b, sizeof(int), 3);
    asx_stream_zip_init(&zipped, &zip_state, sa, sb);

    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_READY);
    {
        asx_stream_zip_pair *pair = (asx_stream_zip_pair *)item;
        ASSERT_EQ(*(int *)pair->a, 1);
        ASSERT_EQ(*(int *)pair->b, 10);
    }

    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_READY);
    {
        asx_stream_zip_pair *pair = (asx_stream_zip_pair *)item;
        ASSERT_EQ(*(int *)pair->a, 2);
        ASSERT_EQ(*(int *)pair->b, 20);
    }
}

TEST(zip_stops_at_shorter) {
    int a[] = {1, 2, 3};
    int b[] = {10};
    asx_stream sa, sb, zipped;
    asx_stream_iter_state iter_a, iter_b;
    asx_stream_zip_state zip_state;
    void *item = NULL;

    asx_stream_iter_init(&sa, &iter_a, a, sizeof(int), 3);
    asx_stream_iter_init(&sb, &iter_b, b, sizeof(int), 1);
    asx_stream_zip_init(&zipped, &zip_state, sa, sb);

    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_DONE);
}

TEST(zip_retains_first_item_across_pending_second) {
    asx_stream sa, sb, zipped;
    asx_stream_zip_state zip_state;
    scripted_int_stream_state a_state, b_state;
    const asx_stream_result a_results[] = {ASX_STREAM_READY, ASX_STREAM_READY, ASX_STREAM_DONE};
    const asx_stream_result b_results[] = {ASX_STREAM_PENDING, ASX_STREAM_READY, ASX_STREAM_READY};
    const int a_values[] = {1, 2, 0};
    const int b_values[] = {0, 10, 20};
    void *item = NULL;

    scripted_int_stream_init(&sa, &a_state, a_results, a_values, 3);
    scripted_int_stream_init(&sb, &b_state, b_results, b_values, 3);
    asx_stream_zip_init(&zipped, &zip_state, sa, sb);

    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_PENDING);
    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)((asx_stream_zip_pair *)item)->a, 1);
    ASSERT_EQ(*(int *)((asx_stream_zip_pair *)item)->b, 10);
    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)((asx_stream_zip_pair *)item)->a, 2);
    ASSERT_EQ(*(int *)((asx_stream_zip_pair *)item)->b, 20);
    ASSERT_EQ(asx_stream_poll_next(&zipped, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* Terminal operation tests                                            */
/* ================================================================== */

static void sum_fold(void *acc, void *item, void *user_data) {
    (void)user_data;
    *(int *)acc += *(int *)item;
}

TEST(fold_sums) {
    int data[] = {1, 2, 3, 4, 5};
    asx_stream s;
    asx_stream_iter_state state;
    int sum = 0;

    asx_stream_iter_init(&s, &state, data, sizeof(int), 5);
    ASSERT_EQ(asx_stream_fold(&s, &sum, sum_fold, NULL), ASX_OK);
    ASSERT_EQ(sum, 15);
}

TEST(fold_empty) {
    asx_stream s;
    asx_stream_iter_state state;
    int sum = 42;

    asx_stream_iter_init(&s, &state, NULL, sizeof(int), 0);
    ASSERT_EQ(asx_stream_fold(&s, &sum, sum_fold, NULL), ASX_OK);
    ASSERT_EQ(sum, 42); /* unchanged */
}

TEST(count_items) {
    int data[] = {10, 20, 30};
    asx_stream s;
    asx_stream_iter_state state;
    size_t count = 0;

    asx_stream_iter_init(&s, &state, data, sizeof(int), 3);
    ASSERT_EQ(asx_stream_count(&s, &count), ASX_OK);
    ASSERT_EQ(count, 3u);
}

TEST(count_empty) {
    asx_stream s;
    asx_stream_iter_state state;
    size_t count = 99;

    asx_stream_iter_init(&s, &state, NULL, sizeof(int), 0);
    ASSERT_EQ(asx_stream_count(&s, &count), ASX_OK);
    ASSERT_EQ(count, 0u);
}

static int foreach_sum;
static void foreach_add(void *item, void *user_data) {
    (void)user_data;
    foreach_sum += *(int *)item;
}

TEST(for_each_visits_all) {
    int data[] = {1, 2, 3};
    asx_stream s;
    asx_stream_iter_state state;

    foreach_sum = 0;
    asx_stream_iter_init(&s, &state, data, sizeof(int), 3);
    ASSERT_EQ(asx_stream_for_each(&s, foreach_add, NULL), ASX_OK);
    ASSERT_EQ(foreach_sum, 6);
}

/* ================================================================== */
/* Combinator composition tests                                        */
/* ================================================================== */

TEST(filter_then_map_pipeline) {
    /* filter(even) |> map(double): [1,2,3,4,5,6] -> [4,8,12] */
    int data[] = {1, 2, 3, 4, 5, 6};
    asx_stream inner, filtered, mapped;
    asx_stream_iter_state iter_state;
    asx_stream_filter_state filter_state;
    asx_stream_map_state map_state;
    int collected[6];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 6);
    asx_stream_filter_init(&filtered, &filter_state, inner, filter_even, NULL);
    asx_stream_map_init(&mapped, &map_state, filtered, map_double, NULL);
    n = collect_ints(&mapped, collected, 6);

    ASSERT_EQ(n, 3u);
    ASSERT_EQ(collected[0], 4);
    ASSERT_EQ(collected[1], 8);
    ASSERT_EQ(collected[2], 12);
}

TEST(skip_then_take_pipeline) {
    /* skip(2) |> take(3): [10,20,30,40,50,60] -> [30,40,50] */
    int data[] = {10, 20, 30, 40, 50, 60};
    asx_stream inner, skipped, taken;
    asx_stream_iter_state iter_state;
    asx_stream_skip_state skip_state;
    asx_stream_take_state take_state;
    int collected[6];
    size_t n;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 6);
    asx_stream_skip_init(&skipped, &skip_state, inner, 2);
    asx_stream_take_init(&taken, &take_state, skipped, 3);
    n = collect_ints(&taken, collected, 6);

    ASSERT_EQ(n, 3u);
    ASSERT_EQ(collected[0], 30);
    ASSERT_EQ(collected[1], 40);
    ASSERT_EQ(collected[2], 50);
}

TEST(filter_map_fold_pipeline) {
    /* filter(even) |> map(double) |> fold(sum):
     * [1,2,3,4,5] -> filter -> [2,4] -> map -> [4,8] -> fold -> 12 */
    int data[] = {1, 2, 3, 4, 5};
    asx_stream inner, filtered, mapped;
    asx_stream_iter_state iter_state;
    asx_stream_filter_state filter_state;
    asx_stream_map_state map_state;
    int sum = 0;

    asx_stream_iter_init(&inner, &iter_state, data, sizeof(int), 5);
    asx_stream_filter_init(&filtered, &filter_state, inner, filter_even, NULL);
    asx_stream_map_init(&mapped, &map_state, filtered, map_double, NULL);
    ASSERT_EQ(asx_stream_fold(&mapped, &sum, sum_fold, NULL), ASX_OK);
    ASSERT_EQ(sum, 12);
}

/* ================================================================== */
/* FilterMap tests                                                     */
/* ================================================================== */

static int fm_buf;
static void *filter_map_even_double(void *item, void *user_data) {
    int val = *(int *)item;
    (void)user_data;
    if (val % 2 != 0) return NULL;
    fm_buf = val * 2;
    return &fm_buf;
}

TEST(filter_map_basic) {
    int data[] = {1, 2, 3, 4, 5};
    int out[5];
    size_t n;
    asx_stream inner, fm;
    asx_stream_iter_state is;
    asx_stream_filter_map_state fms;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 5);
    asx_stream_filter_map_init(&fm, &fms, inner, filter_map_even_double, NULL);
    n = collect_ints(&fm, out, 5);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ(out[0], 4);
    ASSERT_EQ(out[1], 8);
}

/* ================================================================== */
/* TakeWhile tests                                                     */
/* ================================================================== */

static int less_than_4(const void *item, void *user_data) {
    (void)user_data;
    return *(const int *)item < 4;
}

TEST(take_while_stops_at_predicate) {
    int data[] = {1, 2, 3, 4, 5};
    int out[5];
    size_t n;
    asx_stream inner, tw;
    asx_stream_iter_state is;
    asx_stream_take_while_state tws;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 5);
    asx_stream_take_while_init(&tw, &tws, inner, less_than_4, NULL);
    n = collect_ints(&tw, out, 5);
    ASSERT_EQ(n, (size_t)3);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[2], 3);
}

TEST(take_while_all_match) {
    int data[] = {1, 2, 3};
    int out[5];
    size_t n;
    asx_stream inner, tw;
    asx_stream_iter_state is;
    asx_stream_take_while_state tws;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 3);
    asx_stream_take_while_init(&tw, &tws, inner, less_than_4, NULL);
    n = collect_ints(&tw, out, 5);
    ASSERT_EQ(n, (size_t)3);
}

/* ================================================================== */
/* Scan tests                                                          */
/* ================================================================== */

static int scan_running;
static void *scan_running_sum(void *acc, void *item, void *user_data) {
    (void)user_data;
    scan_running = *(int *)acc + *(int *)item;
    *(int *)acc = scan_running;
    return &scan_running;
}

TEST(scan_accumulates) {
    int data[] = {1, 2, 3, 4};
    int out[4];
    size_t n;
    int acc = 0;
    asx_stream inner, sc;
    asx_stream_iter_state is;
    asx_stream_scan_state scs;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 4);
    asx_stream_scan_init(&sc, &scs, inner, scan_running_sum, &acc, NULL);
    n = collect_ints(&sc, out, 4);
    ASSERT_EQ(n, (size_t)4);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[1], 3);
    ASSERT_EQ(out[2], 6);
    ASSERT_EQ(out[3], 10);
}

/* ================================================================== */
/* Peekable tests                                                      */
/* ================================================================== */

TEST(peekable_peek_then_next) {
    int data[] = {10, 20, 30};
    asx_stream inner, pk;
    asx_stream_iter_state is;
    asx_stream_peekable_state pks;
    void *item = NULL;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 3);
    asx_stream_peekable_init(&pk, &pks, inner);
    ASSERT_EQ(asx_stream_peek(&pks, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 10);
    ASSERT_EQ(asx_stream_poll_next(&pk, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 10);
    ASSERT_EQ(asx_stream_poll_next(&pk, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 20);
}

TEST(peekable_double_peek) {
    int data[] = {42};
    asx_stream inner, pk;
    asx_stream_iter_state is;
    asx_stream_peekable_state pks;
    void *item = NULL;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 1);
    asx_stream_peekable_init(&pk, &pks, inner);
    ASSERT_EQ(asx_stream_peek(&pks, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 42);
    ASSERT_EQ(asx_stream_peek(&pks, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 42);
    ASSERT_EQ(asx_stream_poll_next(&pk, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 42);
    ASSERT_EQ(asx_stream_poll_next(&pk, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* Inspect tests                                                       */
/* ================================================================== */

static int inspect_count;
static void inspect_counter(const void *item, void *user_data) {
    (void)item; (void)user_data;
    inspect_count++;
}

TEST(inspect_observes) {
    int data[] = {1, 2, 3};
    int out[3];
    size_t n;
    asx_stream inner, insp;
    asx_stream_iter_state is;
    asx_stream_inspect_state ins;
    inspect_count = 0;
    asx_stream_iter_init(&inner, &is, data, sizeof(int), 3);
    asx_stream_inspect_init(&insp, &ins, inner, inspect_counter, NULL);
    n = collect_ints(&insp, out, 3);
    ASSERT_EQ(n, (size_t)3);
    ASSERT_EQ(inspect_count, 3);
}

/* ================================================================== */
/* Any / All tests                                                     */
/* ================================================================== */

static int is_five(const void *item, void *user_data) {
    (void)user_data;
    return *(const int *)item == 5;
}

static int is_positive(const void *item, void *user_data) {
    (void)user_data;
    return *(const int *)item > 0;
}

TEST(any_found) {
    int data[] = {1, 2, 5, 3};
    int result = 0;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 4);
    ASSERT_EQ(asx_stream_any(&s, is_five, NULL, &result), ASX_OK);
    ASSERT_TRUE(result);
}

TEST(any_not_found) {
    int data[] = {1, 2, 3, 4};
    int result = 1;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 4);
    ASSERT_EQ(asx_stream_any(&s, is_five, NULL, &result), ASX_OK);
    ASSERT_FALSE(result);
}

TEST(all_true) {
    int data[] = {1, 2, 3, 4};
    int result = 0;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 4);
    ASSERT_EQ(asx_stream_all(&s, is_positive, NULL, &result), ASX_OK);
    ASSERT_TRUE(result);
}

TEST(all_false) {
    int data[] = {1, -2, 3};
    int result = 1;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 3);
    ASSERT_EQ(asx_stream_all(&s, is_positive, NULL, &result), ASX_OK);
    ASSERT_FALSE(result);
}

/* ================================================================== */
/* Collect tests                                                       */
/* ================================================================== */

TEST(collect_basic) {
    int data[] = {10, 20, 30};
    void *buf[5];
    size_t count = 0;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 3);
    ASSERT_EQ(asx_stream_collect(&s, buf, 5, &count), ASX_OK);
    ASSERT_EQ(count, (size_t)3);
    ASSERT_EQ(*(int *)buf[0], 10);
    ASSERT_EQ(*(int *)buf[2], 30);
}

TEST(collect_overflow) {
    int data[] = {1, 2, 3, 4, 5};
    void *buf[2];
    size_t count = 0;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 5);
    ASSERT_EQ(asx_stream_collect(&s, buf, 2, &count), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(count, (size_t)2);
}

/* ================================================================== */
/* Fuse tests                                                          */
/* ================================================================== */

/* A buggy stream that yields one item after DONE */
static int fuse_bogus_state;
static int fuse_bogus_val;
static asx_stream_result fuse_bogus_poll(void *state, const asx_waker *waker, void **out_item) {
    (void)state; (void)waker;
    fuse_bogus_state++;
    if (fuse_bogus_state == 1) { fuse_bogus_val = 10; *out_item = &fuse_bogus_val; return ASX_STREAM_READY; }
    if (fuse_bogus_state == 2) return ASX_STREAM_DONE;
    /* Bug: yields again after DONE */
    fuse_bogus_val = 99;
    *out_item = &fuse_bogus_val;
    return ASX_STREAM_READY;
}

TEST(fuse_prevents_post_done) {
    asx_stream bogus, fused;
    asx_stream_fuse_state fs;
    void *item = NULL;

    fuse_bogus_state = 0;
    bogus.poll_next = fuse_bogus_poll;
    bogus.state = NULL;
    asx_stream_fuse_init(&fused, &fs, bogus);

    ASSERT_EQ(asx_stream_poll_next(&fused, NULL, &item), ASX_STREAM_READY);
    ASSERT_EQ(*(int *)item, 10);
    ASSERT_EQ(asx_stream_poll_next(&fused, NULL, &item), ASX_STREAM_DONE);
    /* Fuse should prevent the bogus post-DONE yield */
    ASSERT_EQ(asx_stream_poll_next(&fused, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* Chunks tests                                                        */
/* ================================================================== */

TEST(chunks_exact) {
    int data[] = {1, 2, 3, 4, 5, 6};
    asx_stream inner, chunked;
    asx_stream_iter_state is;
    asx_stream_chunks_state cs;
    void *item = NULL;
    asx_stream_chunk *ch;

    asx_stream_iter_init(&inner, &is, data, sizeof(int), 6);
    asx_stream_chunks_init(&chunked, &cs, inner, 3);

    ASSERT_EQ(asx_stream_poll_next(&chunked, NULL, &item), ASX_STREAM_READY);
    ch = (asx_stream_chunk *)item;
    ASSERT_EQ(ch->count, (size_t)3);
    ASSERT_EQ(*(int *)ch->items[0], 1);
    ASSERT_EQ(*(int *)ch->items[2], 3);

    ASSERT_EQ(asx_stream_poll_next(&chunked, NULL, &item), ASX_STREAM_READY);
    ch = (asx_stream_chunk *)item;
    ASSERT_EQ(ch->count, (size_t)3);
    ASSERT_EQ(*(int *)ch->items[0], 4);

    ASSERT_EQ(asx_stream_poll_next(&chunked, NULL, &item), ASX_STREAM_DONE);
}

TEST(chunks_remainder) {
    int data[] = {1, 2, 3, 4, 5};
    asx_stream inner, chunked;
    asx_stream_iter_state is;
    asx_stream_chunks_state cs;
    void *item = NULL;
    asx_stream_chunk *ch;

    asx_stream_iter_init(&inner, &is, data, sizeof(int), 5);
    asx_stream_chunks_init(&chunked, &cs, inner, 3);

    ASSERT_EQ(asx_stream_poll_next(&chunked, NULL, &item), ASX_STREAM_READY);
    ch = (asx_stream_chunk *)item;
    ASSERT_EQ(ch->count, (size_t)3);

    ASSERT_EQ(asx_stream_poll_next(&chunked, NULL, &item), ASX_STREAM_READY);
    ch = (asx_stream_chunk *)item;
    ASSERT_EQ(ch->count, (size_t)2); /* remainder */

    ASSERT_EQ(asx_stream_poll_next(&chunked, NULL, &item), ASX_STREAM_DONE);
}

/* ================================================================== */
/* SkipWhile tests                                                     */
/* ================================================================== */

TEST(skip_while_basic) {
    int data[] = {1, 2, 3, 4, 5};
    int out[5];
    size_t n;
    asx_stream inner, sw;
    asx_stream_iter_state is;
    asx_stream_skip_while_state sws;

    asx_stream_iter_init(&inner, &is, data, sizeof(int), 5);
    asx_stream_skip_while_init(&sw, &sws, inner, less_than_4, NULL);
    n = collect_ints(&sw, out, 5);
    ASSERT_EQ(n, (size_t)2); /* 4, 5 */
    ASSERT_EQ(out[0], 4);
    ASSERT_EQ(out[1], 5);
}

/* ================================================================== */
/* Flatten tests                                                       */
/* ================================================================== */

TEST(flatten_basic) {
    int data1[] = {1, 2};
    int data2[] = {3, 4, 5};
    asx_stream s1, s2;
    asx_stream_iter_state is1, is2;
    asx_stream *stream_ptrs[2];
    asx_stream outer, flat;
    asx_stream_iter_state outer_is;
    asx_stream_flatten_state fls;
    int out[5];
    size_t n;

    asx_stream_iter_init(&s1, &is1, data1, sizeof(int), 2);
    asx_stream_iter_init(&s2, &is2, data2, sizeof(int), 3);
    stream_ptrs[0] = &s1;
    stream_ptrs[1] = &s2;

    asx_stream_iter_init(&outer, &outer_is, stream_ptrs, sizeof(asx_stream *), 2);
    asx_stream_flatten_init(&flat, &fls, outer);

    n = collect_ints(&flat, out, 5);
    ASSERT_EQ(n, (size_t)5);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[1], 2);
    ASSERT_EQ(out[2], 3);
    ASSERT_EQ(out[4], 5);
}

/* ================================================================== */
/* Dedup tests                                                         */
/* ================================================================== */

static int int_eq(const void *a, const void *b, void *user_data) {
    (void)user_data;
    return *(const int *)a == *(const int *)b;
}

TEST(dedup_basic) {
    int data[] = {1, 1, 2, 2, 2, 3, 1, 1};
    int out[8];
    size_t n;
    asx_stream inner, dd;
    asx_stream_iter_state is;
    asx_stream_dedup_state dds;

    asx_stream_iter_init(&inner, &is, data, sizeof(int), 8);
    asx_stream_dedup_init(&dd, &dds, inner, int_eq, NULL);
    n = collect_ints(&dd, out, 8);
    ASSERT_EQ(n, (size_t)4); /* 1, 2, 3, 1 */
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[1], 2);
    ASSERT_EQ(out[2], 3);
    ASSERT_EQ(out[3], 1);
}

/* ================================================================== */
/* Nth / Last tests                                                    */
/* ================================================================== */

TEST(nth_found) {
    int data[] = {10, 20, 30, 40};
    void *item = NULL;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 4);
    ASSERT_EQ(asx_stream_nth(&s, 2, &item), ASX_OK);
    ASSERT_EQ(*(int *)item, 30);
}

TEST(nth_not_found) {
    int data[] = {10, 20};
    void *item = NULL;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 2);
    ASSERT_EQ(asx_stream_nth(&s, 5, &item), ASX_E_NOT_FOUND);
}

TEST(last_basic) {
    int data[] = {10, 20, 30};
    void *item = NULL;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, data, sizeof(int), 3);
    ASSERT_EQ(asx_stream_last(&s, &item), ASX_OK);
    ASSERT_EQ(*(int *)item, 30);
}

TEST(last_empty) {
    void *item = NULL;
    asx_stream s;
    asx_stream_iter_state is;
    asx_stream_iter_init(&s, &is, NULL, sizeof(int), 0);
    ASSERT_EQ(asx_stream_last(&s, &item), ASX_E_NOT_FOUND);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
    /* Iter source */
    RUN_TEST(iter_empty);
    RUN_TEST(iter_yields_all_items);
    RUN_TEST(iter_done_after_exhaustion);

    /* Map */
    RUN_TEST(map_transforms_items);

    /* Filter */
    RUN_TEST(filter_yields_matching);
    RUN_TEST(filter_none_match);

    /* Take */
    RUN_TEST(take_limits_count);
    RUN_TEST(take_zero_yields_none);

    /* Skip */
    RUN_TEST(skip_skips_items);
    RUN_TEST(skip_all_yields_none);

    /* Chain */
    RUN_TEST(chain_concatenates);
    RUN_TEST(chain_first_empty);

    /* Enumerate */
    RUN_TEST(enumerate_adds_index);

    /* Merge */
    RUN_TEST(merge_interleaves);
    RUN_TEST(merge_one_empty);

    /* Zip */
    RUN_TEST(zip_pairs_items);
    RUN_TEST(zip_stops_at_shorter);
    RUN_TEST(zip_retains_first_item_across_pending_second);

    /* Terminal operations */
    RUN_TEST(fold_sums);
    RUN_TEST(fold_empty);
    RUN_TEST(count_items);
    RUN_TEST(count_empty);
    RUN_TEST(for_each_visits_all);

    /* Combinator composition */
    RUN_TEST(filter_then_map_pipeline);
    RUN_TEST(skip_then_take_pipeline);
    RUN_TEST(filter_map_fold_pipeline);

    /* FilterMap */
    RUN_TEST(filter_map_basic);

    /* TakeWhile */
    RUN_TEST(take_while_stops_at_predicate);
    RUN_TEST(take_while_all_match);

    /* Scan */
    RUN_TEST(scan_accumulates);

    /* Peekable */
    RUN_TEST(peekable_peek_then_next);
    RUN_TEST(peekable_double_peek);

    /* Inspect */
    RUN_TEST(inspect_observes);

    /* Any / All */
    RUN_TEST(any_found);
    RUN_TEST(any_not_found);
    RUN_TEST(all_true);
    RUN_TEST(all_false);

    /* Collect */
    RUN_TEST(collect_basic);
    RUN_TEST(collect_overflow);

    /* Fuse */
    RUN_TEST(fuse_prevents_post_done);

    /* Chunks */
    RUN_TEST(chunks_exact);
    RUN_TEST(chunks_remainder);

    /* SkipWhile */
    RUN_TEST(skip_while_basic);

    /* Flatten */
    RUN_TEST(flatten_basic);

    /* Dedup */
    RUN_TEST(dedup_basic);

    /* Nth / Last */
    RUN_TEST(nth_found);
    RUN_TEST(nth_not_found);
    RUN_TEST(last_basic);
    RUN_TEST(last_empty);

    TEST_REPORT();
    return test_failures;
}
