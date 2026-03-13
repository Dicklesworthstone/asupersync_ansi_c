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

    TEST_REPORT();
    return test_failures;
}
