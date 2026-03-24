/*
 * test_contended_mutex.c — unit tests for instrumented contended mutex
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/sync/contended_mutex.h>

static void setup(void) {
    asx_semaphore_reset();
    asx_contended_mutex_reset();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TEST(create_and_close) {
    asx_contended_mutex_handle h;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);
    ASSERT_FALSE(asx_contended_mutex_is_locked(h));
    ASSERT_EQ(asx_contended_mutex_close(h), ASX_OK);
}

TEST(create_null_fails) {
    setup();
    ASSERT_EQ(asx_contended_mutex_create(NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Lock / Unlock                                                       */
/* ------------------------------------------------------------------ */

TEST(try_lock_and_unlock) {
    asx_contended_mutex_handle h;
    asx_mutex_guard g;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);
    ASSERT_TRUE(asx_contended_mutex_is_locked(h));
    ASSERT_EQ(asx_contended_mutex_unlock(g), ASX_OK);
    ASSERT_FALSE(asx_contended_mutex_is_locked(h));
    asx_contended_mutex_close(h);
}

TEST(try_lock_contention) {
    asx_contended_mutex_handle h;
    asx_mutex_guard g1, g2;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g1), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);
    asx_contended_mutex_unlock(g1);
    asx_contended_mutex_close(h);
}

/* ------------------------------------------------------------------ */
/* Metrics tracking                                                    */
/* ------------------------------------------------------------------ */

TEST(metrics_track_acquisitions) {
    asx_contended_mutex_handle h;
    asx_mutex_guard g;
    asx_lock_metrics_snapshot snap;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);

    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);
    asx_contended_mutex_unlock(g);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);
    asx_contended_mutex_unlock(g);

    ASSERT_EQ(asx_contended_mutex_metrics(h, &snap), ASX_OK);
    ASSERT_EQ(snap.acquisitions, (uint64_t)2);
    ASSERT_EQ(snap.contentions, (uint64_t)0);
    asx_contended_mutex_close(h);
}

TEST(metrics_track_contentions) {
    asx_contended_mutex_handle h;
    asx_mutex_guard g, g2;
    asx_lock_metrics_snapshot snap;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);

    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);
    /* Three contended attempts */
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_contended_mutex_metrics(h, &snap), ASX_OK);
    ASSERT_EQ(snap.acquisitions, (uint64_t)1);
    ASSERT_EQ(snap.contentions, (uint64_t)3);
    ASSERT_EQ(snap.max_contention_run, (uint64_t)3);

    asx_contended_mutex_unlock(g);
    asx_contended_mutex_close(h);
}

TEST(metrics_contention_run_resets_on_acquire) {
    asx_contended_mutex_handle h;
    asx_mutex_guard g, g2;
    asx_lock_metrics_snapshot snap;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);

    /* Lock, contend twice, unlock, acquire (resets run) */
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);
    asx_contended_mutex_unlock(g);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);

    /* Contend once more */
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_contended_mutex_metrics(h, &snap), ASX_OK);
    ASSERT_EQ(snap.max_contention_run, (uint64_t)2); /* max was 2, not 3 */
    ASSERT_EQ(snap.contentions, (uint64_t)3);

    asx_contended_mutex_unlock(g);
    asx_contended_mutex_close(h);
}

TEST(metrics_reset) {
    asx_contended_mutex_handle h;
    asx_mutex_guard g;
    asx_lock_metrics_snapshot snap;
    setup();
    ASSERT_EQ(asx_contended_mutex_create(&h), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_try_lock(h, &g), ASX_OK);
    asx_contended_mutex_unlock(g);

    ASSERT_EQ(asx_contended_mutex_reset_metrics(h), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_metrics(h, &snap), ASX_OK);
    ASSERT_EQ(snap.acquisitions, (uint64_t)0);
    ASSERT_EQ(snap.contentions, (uint64_t)0);
    ASSERT_EQ(snap.max_contention_run, (uint64_t)0);

    asx_contended_mutex_close(h);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_contended_mutex ===\n");
    RUN_TEST(create_and_close);
    RUN_TEST(create_null_fails);
    RUN_TEST(try_lock_and_unlock);
    RUN_TEST(try_lock_contention);
    RUN_TEST(metrics_track_acquisitions);
    RUN_TEST(metrics_track_contentions);
    RUN_TEST(metrics_contention_run_resets_on_acquire);
    RUN_TEST(metrics_reset);
    TEST_REPORT();
    return test_failures;
}
