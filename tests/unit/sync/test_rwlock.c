/*
 * test_rwlock.c — unit tests for async read-write lock
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/sync/rwlock.h>

static void setup(void) { asx_rwlock_reset(); }

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

TEST(create_and_close) {
    asx_rwlock_handle h;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 0u);
    ASSERT_FALSE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_close(h), ASX_OK);
}

TEST(create_null_fails) {
    setup();
    ASSERT_EQ(asx_rwlock_create(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(close_stale_fails) {
    asx_rwlock_handle h;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_close(h), ASX_OK);
    ASSERT_EQ(asx_rwlock_close(h), ASX_E_STALE_HANDLE);
}

/* ------------------------------------------------------------------ */
/* Read lock (non-blocking)                                            */
/* ------------------------------------------------------------------ */

TEST(try_read_succeeds_when_unlocked) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard g;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &g), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 1u);
    ASSERT_EQ(asx_rwlock_read_unlock(g), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 0u);
    asx_rwlock_close(h);
}

TEST(multiple_concurrent_readers) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard g1, g2, g3;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &g1), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &g2), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &g3), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 3u);
    ASSERT_EQ(asx_rwlock_read_unlock(g1), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(g2), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(g3), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 0u);
    asx_rwlock_close(h);
}

TEST(try_read_null_out_fails) {
    asx_rwlock_handle h;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, NULL), ASX_E_INVALID_ARGUMENT);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Write lock (non-blocking)                                           */
/* ------------------------------------------------------------------ */

TEST(try_write_succeeds_when_unlocked) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard g;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &g), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_write_unlock(g), ASX_OK);
    ASSERT_FALSE(asx_rwlock_is_write_locked(h));
    asx_rwlock_close(h);
}

TEST(try_write_fails_when_read_locked) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard rg;
    asx_rwlock_write_guard wg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(try_write_fails_when_write_locked) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard g1, g2;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &g1), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &g2), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_rwlock_write_unlock(g1), ASX_OK);
    asx_rwlock_close(h);
}

TEST(try_read_fails_when_write_locked) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard wg;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Writer-preference fairness                                          */
/* ------------------------------------------------------------------ */

TEST(writer_preference_blocks_new_readers) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard rg1, rg2;
    asx_rwlock_waiter ww;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);

    /* Acquire a read lock */
    ASSERT_EQ(asx_rwlock_try_read(h, &rg1), ASX_OK);

    /* Writer begins waiting */
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);

    /* New reader should be blocked due to writer-preference */
    ASSERT_EQ(asx_rwlock_try_read(h, &rg2), ASX_E_WOULD_BLOCK);

    /* Release reader, writer should get lock on next poll */
    ASSERT_EQ(asx_rwlock_read_unlock(rg1), ASX_OK);

    /* Cancel the writer waiter to clean up */
    ASSERT_EQ(asx_rwlock_waiter_cancel(&ww), ASX_OK);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Async read (poll-based)                                             */
/* ------------------------------------------------------------------ */

TEST(async_read_immediate) {
    asx_rwlock_handle h;
    asx_rwlock_waiter w;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_begin(h, &w), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&w, &rg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 1u);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(async_read_pending_when_write_locked) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter rw;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);

    /* Release write lock — reader should be woken */
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 1u);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Async write (poll-based)                                            */
/* ------------------------------------------------------------------ */

TEST(async_write_immediate) {
    asx_rwlock_handle h;
    asx_rwlock_waiter w;
    asx_rwlock_write_guard wg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &w), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&w, &wg, NULL), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(async_write_pending_when_read_locked) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard rg;
    asx_rwlock_waiter ww;
    asx_rwlock_write_guard wg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);

    /* Release read lock — writer should be woken */
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Waiter cancellation                                                 */
/* ------------------------------------------------------------------ */

TEST(cancel_read_waiter) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter rw;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw), ASX_OK);
    ASSERT_EQ(asx_rwlock_waiter_cancel(&rw), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(cancel_write_waiter) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard rg;
    asx_rwlock_waiter ww;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_waiter_cancel(&ww), ASX_OK);
    /* After cancel, new readers should succeed (no writer waiting) */
    {
        asx_rwlock_read_guard rg2;
        ASSERT_EQ(asx_rwlock_try_read(h, &rg2), ASX_OK);
        ASSERT_EQ(asx_rwlock_read_unlock(rg2), ASX_OK);
    }
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(cancel_null_fails) { ASSERT_EQ(asx_rwlock_waiter_cancel(NULL), ASX_E_INVALID_ARGUMENT); }

/* ------------------------------------------------------------------ */
/* Unlock validation                                                   */
/* ------------------------------------------------------------------ */

TEST(read_unlock_without_readers_fails) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard g;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    g.rw_slot = h.slot;
    g.generation = h.generation;
    ASSERT_EQ(asx_rwlock_read_unlock(g), ASX_E_INVALID_STATE);
    asx_rwlock_close(h);
}

TEST(write_unlock_without_writer_fails) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard g;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    g.rw_slot = h.slot;
    g.generation = h.generation;
    ASSERT_EQ(asx_rwlock_write_unlock(g), ASX_E_INVALID_STATE);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Arena exhaustion                                                    */
/* ------------------------------------------------------------------ */

TEST(arena_exhaustion) {
    asx_rwlock_handle handles[ASX_RWLOCK_MAX];
    asx_rwlock_handle overflow;
    uint32_t i;
    setup();
    for (i = 0; i < ASX_RWLOCK_MAX; i++) { ASSERT_EQ(asx_rwlock_create(&handles[i]), ASX_OK); }
    ASSERT_EQ(asx_rwlock_create(&overflow), ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0; i < ASX_RWLOCK_MAX; i++) { asx_rwlock_close(handles[i]); }
}

/* ------------------------------------------------------------------ */
/* Reset invalidates handles                                           */
/* ------------------------------------------------------------------ */

TEST(reset_invalidates_handles) {
    asx_rwlock_handle h;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    asx_rwlock_reset();
    ASSERT_EQ(asx_rwlock_try_read(h, &rg), ASX_E_STALE_HANDLE);
}

/* ------------------------------------------------------------------ */
/* Write unlock wakes multiple readers                                 */
/* ------------------------------------------------------------------ */

TEST(write_unlock_wakes_all_readers) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter rw1, rw2;
    asx_rwlock_read_guard rg1, rg2;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg), ASX_OK);

    /* Two readers begin waiting */
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw1), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw2), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw1, &rg1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_E_PENDING);

    /* Release write lock — both readers should be woken */
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw1, &rg1, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 2u);

    ASSERT_EQ(asx_rwlock_read_unlock(rg1), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg2), ASX_OK);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Writer waiter preference over readers                               */
/* ------------------------------------------------------------------ */

TEST(writer_wakes_before_readers_after_write_unlock) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard wg1, wg2;
    asx_rwlock_waiter ww, rw;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg1), ASX_OK);

    /* Both a writer and reader begin waiting */
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw), ASX_OK);

    /* Release first write lock — writer-preference: writer wakes first */
    ASSERT_EQ(asx_rwlock_write_unlock(wg1), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));

    /* Writer should succeed, reader should still be pending */
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg2, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);

    /* Release second writer — now reader wakes */
    ASSERT_EQ(asx_rwlock_write_unlock(wg2), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_rwlock ===\n");

    RUN_TEST(create_and_close);
    RUN_TEST(create_null_fails);
    RUN_TEST(close_stale_fails);

    RUN_TEST(try_read_succeeds_when_unlocked);
    RUN_TEST(multiple_concurrent_readers);
    RUN_TEST(try_read_null_out_fails);

    RUN_TEST(try_write_succeeds_when_unlocked);
    RUN_TEST(try_write_fails_when_read_locked);
    RUN_TEST(try_write_fails_when_write_locked);
    RUN_TEST(try_read_fails_when_write_locked);

    RUN_TEST(writer_preference_blocks_new_readers);

    RUN_TEST(async_read_immediate);
    RUN_TEST(async_read_pending_when_write_locked);
    RUN_TEST(async_write_immediate);
    RUN_TEST(async_write_pending_when_read_locked);

    RUN_TEST(cancel_read_waiter);
    RUN_TEST(cancel_write_waiter);
    RUN_TEST(cancel_null_fails);

    RUN_TEST(read_unlock_without_readers_fails);
    RUN_TEST(write_unlock_without_writer_fails);

    RUN_TEST(arena_exhaustion);
    RUN_TEST(reset_invalidates_handles);

    RUN_TEST(write_unlock_wakes_all_readers);
    RUN_TEST(writer_wakes_before_readers_after_write_unlock);

    TEST_REPORT();
    return test_failures;
}
