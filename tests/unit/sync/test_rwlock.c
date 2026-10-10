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
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter ww;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);

    /* Acquire a read lock */
    ASSERT_EQ(asx_rwlock_try_read(h, &rg1), ASX_OK);

    /* A writer that has not been polled is not waiting yet (Rust's write
     * future joins the line at its first poll). */
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg2), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg2), ASX_OK);

    /* Once it waits, new readers are blocked (writer-preference), and so
     * is a try_write. */
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg2), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg), ASX_E_WOULD_BLOCK);

    /* The last reader's release hands the writer the lock. */
    ASSERT_EQ(asx_rwlock_read_unlock(rg1), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
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
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter ww;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &rg), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);
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
/* Release order (Rust release_writer / release_reader)                */
/* ------------------------------------------------------------------ */

TEST(queued_writer_ahead_of_reader_goes_first) {
    asx_rwlock_handle h;
    asx_rwlock_write_guard wg1, wg2;
    asx_rwlock_waiter ww, rw;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &wg1), ASX_OK);

    /* A writer, then a reader, queue */
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg2, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);

    /* The writer arrived first, so the release hands it the lock */
    ASSERT_EQ(asx_rwlock_write_unlock(wg1), ASX_OK);
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg2, NULL), ASX_OK);

    /* Release second writer — now reader is granted */
    ASSERT_EQ(asx_rwlock_write_unlock(wg2), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 1u);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(reader_queued_before_writer_goes_first) {
    /* Rust's should_wake_writer compares arrival: readers queued before
     * the first queued writer are admitted ahead of it. */
    asx_rwlock_handle h;
    asx_rwlock_write_guard held, wg;
    asx_rwlock_waiter ww, rw1, rw2;
    asx_rwlock_read_guard rg1, rg2;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &held), ASX_OK);

    /* Arrival order: reader 1, writer, reader 2 */
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw1), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw1, &rg1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw2), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_E_PENDING);

    /* Only reader 1, older than the writer, is admitted */
    ASSERT_EQ(asx_rwlock_write_unlock(held), ASX_OK);
    ASSERT_FALSE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_reader_count(h), 1u);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_poll_read(&rw1, &rg1, NULL), ASX_OK);

    /* Its release hands the writer the lock, then reader 2 follows */
    ASSERT_EQ(asx_rwlock_read_unlock(rg1), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg2), ASX_OK);
    asx_rwlock_close(h);
}

TEST(forced_reader_turn_after_sixteen_writer_handoffs) {
    /* Rust's MAX_CONSECUTIVE_WRITERS_BEFORE_READER_BATCH: after 16 writer
     * hand-offs in a row while a reader waits, the oldest queued reader
     * gets a turn before the head writer, even though it arrived after
     * that writer. */
    asx_rwlock_handle h;
    asx_rwlock_write_guard held, wg;
    asx_rwlock_waiter ww[18];
    asx_rwlock_waiter rw;
    asx_rwlock_read_guard rg;
    uint32_t i;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &held), ASX_OK);

    /* Arrival order: writers 0..16, the reader, writer 17 */
    for (i = 0; i < 18u; i++) {
        if (i == 17u) {
            ASSERT_EQ(asx_rwlock_read_begin(h, &rw), ASX_OK);
            ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);
        }
        ASSERT_EQ(asx_rwlock_write_begin(h, &ww[i]), ASX_OK);
        ASSERT_EQ(asx_rwlock_poll_write(&ww[i], &wg, NULL), ASX_E_PENDING);
    }
    ASSERT_EQ(asx_rwlock_write_unlock(held), ASX_OK);

    /* Writers 0..15 in turn: sixteen hand-offs while the reader waits */
    for (i = 0; i < 16u; i++) {
        ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);
        ASSERT_EQ(asx_rwlock_poll_write(&ww[i], &wg, NULL), ASX_OK);
        ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    }

    /* The reader's forced turn comes before writer 16 */
    ASSERT_EQ(asx_rwlock_poll_write(&ww[16], &wg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww[16], &wg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww[17], &wg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_unlock(wg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(abandoned_last_writer_admits_queued_readers) {
    /* Rust's abandon_write_waiter: when the last waiting writer gives up
     * and no writer holds the lock, every queued reader is admitted. */
    asx_rwlock_handle h;
    asx_rwlock_read_guard held, rg1, rg2;
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter ww, rw1, rw2;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &held), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw1), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw1, &rg1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw2), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_E_PENDING);

    ASSERT_EQ(asx_rwlock_waiter_cancel(&ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_reader_count(h), 3u);
    ASSERT_EQ(asx_rwlock_poll_read(&rw1, &rg1, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw2, &rg2, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(held), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg1), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg2), ASX_OK);
    asx_rwlock_close(h);
}

TEST(abandoned_granted_writer_passes_the_lock_on) {
    /* A writer granted the lock that gives up before taking it releases
     * the lock as its guard would (Rust abandon_write_waiter). */
    asx_rwlock_handle h;
    asx_rwlock_read_guard held, rg;
    asx_rwlock_write_guard wg;
    asx_rwlock_waiter ww, rw;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_read(h, &held), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_write(&ww, &wg, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_rwlock_read_begin(h, &rw), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_E_PENDING);

    ASSERT_EQ(asx_rwlock_read_unlock(held), ASX_OK); /* grants the writer */
    ASSERT_TRUE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_waiter_cancel(&ww), ASX_OK);
    ASSERT_FALSE(asx_rwlock_is_write_locked(h));
    ASSERT_EQ(asx_rwlock_poll_read(&rw, &rg, NULL), ASX_OK);
    ASSERT_EQ(asx_rwlock_read_unlock(rg), ASX_OK);
    asx_rwlock_close(h);
}

TEST(poll_of_the_other_kind_is_refused) {
    asx_rwlock_handle h;
    asx_rwlock_waiter ww;
    asx_rwlock_read_guard rg;
    setup();
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_write_begin(h, &ww), ASX_OK);
    ASSERT_EQ(asx_rwlock_poll_read(&ww, &rg, NULL), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_rwlock_waiter_cancel(&ww), ASX_OK);
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
    RUN_TEST(queued_writer_ahead_of_reader_goes_first);
    RUN_TEST(reader_queued_before_writer_goes_first);
    RUN_TEST(forced_reader_turn_after_sixteen_writer_handoffs);
    RUN_TEST(abandoned_last_writer_admits_queued_readers);
    RUN_TEST(abandoned_granted_writer_passes_the_lock_on);
    RUN_TEST(poll_of_the_other_kind_is_refused);

    TEST_REPORT();
    return test_failures;
}
