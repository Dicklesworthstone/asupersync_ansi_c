/*
 * test_det_hash.c — unit tests for deterministic hash map
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/det_hash.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TEST(init_empty) {
    asx_det_hash_map m;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_count(&m), 0u);
    ASSERT_TRUE(asx_det_hash_is_empty(&m));
}

TEST(clear_resets) {
    asx_det_hash_map m;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 1, 100), ASX_OK);
    ASSERT_EQ(asx_det_hash_count(&m), 1u);
    asx_det_hash_clear(&m);
    ASSERT_EQ(asx_det_hash_count(&m), 0u);
}

/* ------------------------------------------------------------------ */
/* Insert / Get                                                        */
/* ------------------------------------------------------------------ */

TEST(insert_and_get) {
    asx_det_hash_map m;
    uint64_t val = 0;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 42, 999), ASX_OK);
    ASSERT_EQ(asx_det_hash_get(&m, 42, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)999);
    ASSERT_EQ(asx_det_hash_count(&m), 1u);
}

TEST(insert_update) {
    asx_det_hash_map m;
    uint64_t val = 0;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 10, 100), ASX_OK);
    ASSERT_EQ(asx_det_hash_insert(&m, 10, 200), ASX_OK); /* update */
    ASSERT_EQ(asx_det_hash_get(&m, 10, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)200);
    ASSERT_EQ(asx_det_hash_count(&m), 1u); /* still 1 entry */
}

TEST(get_not_found) {
    asx_det_hash_map m;
    uint64_t val = 0;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_get(&m, 99, &val), ASX_E_NOT_FOUND);
}

TEST(multiple_inserts) {
    asx_det_hash_map m;
    uint64_t val = 0;
    uint32_t i;
    asx_det_hash_init(&m);
    for (i = 0; i < 20; i++) {
        ASSERT_EQ(asx_det_hash_insert(&m, (uint64_t)i, (uint64_t)(i * 10)), ASX_OK);
    }
    ASSERT_EQ(asx_det_hash_count(&m), 20u);
    for (i = 0; i < 20; i++) {
        ASSERT_EQ(asx_det_hash_get(&m, (uint64_t)i, &val), ASX_OK);
        ASSERT_EQ(val, (uint64_t)(i * 10));
    }
}

/* ------------------------------------------------------------------ */
/* Contains                                                            */
/* ------------------------------------------------------------------ */

TEST(contains_present) {
    asx_det_hash_map m;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 7, 77), ASX_OK);
    ASSERT_TRUE(asx_det_hash_contains(&m, 7));
    ASSERT_FALSE(asx_det_hash_contains(&m, 8));
}

/* ------------------------------------------------------------------ */
/* Remove                                                              */
/* ------------------------------------------------------------------ */

TEST(remove_existing) {
    asx_det_hash_map m;
    uint64_t val = 0;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 1, 10), ASX_OK);
    ASSERT_EQ(asx_det_hash_insert(&m, 2, 20), ASX_OK);
    ASSERT_EQ(asx_det_hash_remove(&m, 1), ASX_OK);
    ASSERT_FALSE(asx_det_hash_contains(&m, 1));
    ASSERT_EQ(asx_det_hash_get(&m, 2, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)20);
    ASSERT_EQ(asx_det_hash_count(&m), 1u);
}

TEST(remove_not_found) {
    asx_det_hash_map m;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_remove(&m, 99), ASX_E_NOT_FOUND);
}

TEST(remove_and_reinsert) {
    asx_det_hash_map m;
    uint64_t val = 0;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 5, 50), ASX_OK);
    ASSERT_EQ(asx_det_hash_remove(&m, 5), ASX_OK);
    ASSERT_EQ(asx_det_hash_insert(&m, 5, 55), ASX_OK);
    ASSERT_EQ(asx_det_hash_get(&m, 5, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)55);
}

/* ------------------------------------------------------------------ */
/* Deterministic iteration                                             */
/* ------------------------------------------------------------------ */

TEST(iteration_order) {
    asx_det_hash_map m;
    uint64_t key, val;
    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, 100, 1), ASX_OK);
    ASSERT_EQ(asx_det_hash_insert(&m, 200, 2), ASX_OK);
    ASSERT_EQ(asx_det_hash_insert(&m, 300, 3), ASX_OK);

    ASSERT_TRUE(asx_det_hash_entry_at(&m, 0, &key, &val));
    ASSERT_EQ(key, (uint64_t)100);
    ASSERT_EQ(val, (uint64_t)1);

    ASSERT_TRUE(asx_det_hash_entry_at(&m, 1, &key, &val));
    ASSERT_EQ(key, (uint64_t)200);
    ASSERT_EQ(val, (uint64_t)2);

    ASSERT_TRUE(asx_det_hash_entry_at(&m, 2, &key, &val));
    ASSERT_EQ(key, (uint64_t)300);
    ASSERT_EQ(val, (uint64_t)3);

    ASSERT_FALSE(asx_det_hash_entry_at(&m, 3, &key, &val));
}

/* ------------------------------------------------------------------ */
/* Capacity exhaustion                                                 */
/* ------------------------------------------------------------------ */

TEST(capacity_exhaustion) {
    asx_det_hash_map m;
    uint32_t i;
    asx_det_hash_init(&m);
    for (i = 0; i < ASX_DET_HASH_MAX_ENTRIES; i++) {
        ASSERT_EQ(asx_det_hash_insert(&m, (uint64_t)(i + 1000), (uint64_t)i), ASX_OK);
    }
    ASSERT_EQ(asx_det_hash_insert(&m, 9999, 0), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_det_hash_count(&m), ASX_DET_HASH_MAX_ENTRIES);
}

/* ------------------------------------------------------------------ */
/* Hash functions                                                      */
/* ------------------------------------------------------------------ */

TEST(hash_str_deterministic) {
    uint64_t h1 = asx_det_hash_str("hello");
    uint64_t h2 = asx_det_hash_str("hello");
    uint64_t h3 = asx_det_hash_str("world");
    ASSERT_EQ(h1, h2);
    ASSERT_NE(h1, h3);
}

TEST(hash_null_str) {
    uint64_t h = asx_det_hash_str(NULL);
    ASSERT_EQ(h, (uint64_t)0xcbf29ce484222325ULL); /* FNV offset basis */
}

/* ------------------------------------------------------------------ */
/* String-keyed usage pattern                                          */
/* ------------------------------------------------------------------ */

TEST(string_keyed_pattern) {
    asx_det_hash_map m;
    uint64_t val = 0;
    uint64_t k_hello = asx_det_hash_str("hello");
    uint64_t k_world = asx_det_hash_str("world");

    asx_det_hash_init(&m);
    ASSERT_EQ(asx_det_hash_insert(&m, k_hello, 42), ASX_OK);
    ASSERT_EQ(asx_det_hash_insert(&m, k_world, 99), ASX_OK);
    ASSERT_EQ(asx_det_hash_get(&m, k_hello, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);
    ASSERT_EQ(asx_det_hash_get(&m, k_world, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)99);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_det_hash ===\n");

    RUN_TEST(init_empty);
    RUN_TEST(clear_resets);

    RUN_TEST(insert_and_get);
    RUN_TEST(insert_update);
    RUN_TEST(get_not_found);
    RUN_TEST(multiple_inserts);

    RUN_TEST(contains_present);

    RUN_TEST(remove_existing);
    RUN_TEST(remove_not_found);
    RUN_TEST(remove_and_reinsert);

    RUN_TEST(iteration_order);
    RUN_TEST(capacity_exhaustion);

    RUN_TEST(hash_str_deterministic);
    RUN_TEST(hash_null_str);
    RUN_TEST(string_keyed_pattern);

    TEST_REPORT();
    return test_failures;
}
