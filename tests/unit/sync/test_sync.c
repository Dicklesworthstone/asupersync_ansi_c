/*
 * test_sync.c — unit tests for sync primitives
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/core/budget.h>
#include <asx/cx/cx.h>
#include <asx/sync/barrier.h>
#include <asx/sync/mutex.h>
#include <asx/sync/notify.h>
#include <asx/sync/once.h>
#include <asx/sync/semaphore.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static void setup(void) {
    asx_notify_reset();
    asx_semaphore_reset();
    asx_barrier_reset();
    asx_once_reset();
}

/* ================================================================== */
/* Notify tests                                                        */
/* ================================================================== */

TEST(notify_create_close) {
    asx_notify_handle h;
    setup();
    ASSERT_EQ(asx_notify_create(&h), ASX_OK);
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_create_null_fails) {
    setup();
    ASSERT_EQ(asx_notify_create(NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(notify_one_no_waiters) {
    asx_notify_handle h;
    setup();
    MUST_OK(asx_notify_create(&h));
    ASSERT_EQ(asx_notify_one(h), ASX_OK); /* no waiter: stored */
    ASSERT_EQ(asx_notify_stored_count(h), 1u);
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_all_no_waiters) {
    asx_notify_handle h;
    setup();
    MUST_OK(asx_notify_create(&h));
    ASSERT_EQ(asx_notify_all(h), ASX_OK);
    ASSERT_EQ(asx_notify_stored_count(h), 0u); /* notify_all stores nothing */
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

/* Y2 regression: a notify_one that arrives before the waiter must not be
 * lost (Rust sync/notify.rs stores it; the C port used to drop it and the
 * waiter parked forever). Stored notifications accumulate. */
TEST(notify_one_before_wait_is_stored) {
    asx_notify_handle h;
    asx_notify_waiter w1, w2, w3;
    setup();
    MUST_OK(asx_notify_create(&h));

    MUST_OK(asx_notify_one(h));
    MUST_OK(asx_notify_one(h));
    ASSERT_EQ(asx_notify_stored_count(h), 2u);

    MUST_OK(asx_notify_wait_begin(h, &w1));
    ASSERT_EQ(asx_notify_poll_wait(&w1, NULL), ASX_OK);
    MUST_OK(asx_notify_wait_begin(h, &w2));
    ASSERT_EQ(asx_notify_poll_wait(&w2, NULL), ASX_OK);
    ASSERT_EQ(asx_notify_stored_count(h), 0u);

    /* The third waiter has nothing stored and waits */
    MUST_OK(asx_notify_wait_begin(h, &w3));
    ASSERT_EQ(asx_notify_poll_wait(&w3, NULL), ASX_E_PENDING);
    MUST_OK(asx_notify_one(h));
    ASSERT_EQ(asx_notify_poll_wait(&w3, NULL), ASX_OK);
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_stored_permit_survives_abandoned_waiter) {
    asx_notify_handle h;
    asx_notify_waiter w1, w2;
    setup();
    MUST_OK(asx_notify_create(&h));

    MUST_OK(asx_notify_one(h));
    /* w1 claims the stored permit, then gives up before consuming it */
    MUST_OK(asx_notify_wait_begin(h, &w1));
    ASSERT_EQ(asx_notify_stored_count(h), 0u);
    MUST_OK(asx_notify_wait_cancel(&w1));
    /* Nobody else is waiting, so it is stored again, not lost */
    ASSERT_EQ(asx_notify_stored_count(h), 1u);

    MUST_OK(asx_notify_wait_begin(h, &w2));
    ASSERT_EQ(asx_notify_poll_wait(&w2, NULL), ASX_OK);

    /* Close drops stored notifications */
    MUST_OK(asx_notify_one(h));
    ASSERT_EQ(asx_notify_stored_count(h), 1u);
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
    ASSERT_EQ(asx_notify_stored_count(h), 0u);
}

TEST(notify_wait_then_signal) {
    asx_notify_handle h;
    asx_notify_waiter w;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w));
    ASSERT_EQ(asx_notify_waiter_count(h), 1u);

    /* Not yet notified */
    ASSERT_EQ(asx_notify_poll_wait(&w, NULL), ASX_E_PENDING);

    /* Signal */
    MUST_OK(asx_notify_one(h));
    ASSERT_EQ(asx_notify_poll_wait(&w, NULL), ASX_OK);
    ASSERT_EQ(asx_notify_waiter_count(h), 0u);

    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_all_wakes_multiple) {
    asx_notify_handle h;
    asx_notify_waiter w1, w2, w3;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w1));
    MUST_OK(asx_notify_wait_begin(h, &w2));
    MUST_OK(asx_notify_wait_begin(h, &w3));
    ASSERT_EQ(asx_notify_waiter_count(h), 3u);

    MUST_OK(asx_notify_all(h));
    ASSERT_EQ(asx_notify_poll_wait(&w1, NULL), ASX_OK);
    ASSERT_EQ(asx_notify_poll_wait(&w2, NULL), ASX_OK);
    ASSERT_EQ(asx_notify_poll_wait(&w3, NULL), ASX_OK);

    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_one_fifo) {
    asx_notify_handle h;
    asx_notify_waiter w1, w2;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w1));
    MUST_OK(asx_notify_wait_begin(h, &w2));

    /* Signal one — should wake w1 (FIFO) */
    MUST_OK(asx_notify_one(h));
    ASSERT_EQ(asx_notify_poll_wait(&w1, NULL), ASX_OK);
    ASSERT_EQ(asx_notify_poll_wait(&w2, NULL), ASX_E_PENDING);

    /* Signal again — wakes w2 */
    MUST_OK(asx_notify_one(h));
    ASSERT_EQ(asx_notify_poll_wait(&w2, NULL), ASX_OK);

    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_wait_cancel) {
    asx_notify_handle h;
    asx_notify_waiter w;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w));
    ASSERT_EQ(asx_notify_waiter_count(h), 1u);
    MUST_OK(asx_notify_wait_cancel(&w));
    ASSERT_EQ(asx_notify_waiter_count(h), 0u);
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_close_disconnects_waiters) {
    asx_notify_handle h;
    asx_notify_waiter w;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w));
    MUST_OK(asx_notify_close(h));
    ASSERT_EQ(asx_notify_poll_wait(&w, NULL), ASX_E_DISCONNECTED);
}

TEST(notify_stale_handle) {
    asx_notify_handle h;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_close(h));
    ASSERT_EQ(asx_notify_one(h), ASX_E_STALE_HANDLE);
}

TEST(notify_exhaustion) {
    asx_notify_handle handles[ASX_NOTIFY_MAX];
    asx_notify_handle extra;
    uint32_t i;
    setup();
    for (i = 0; i < ASX_NOTIFY_MAX; i++) { MUST_OK(asx_notify_create(&handles[i])); }
    ASSERT_EQ(asx_notify_create(&extra), ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0; i < ASX_NOTIFY_MAX; i++) { MUST_OK(asx_notify_close(handles[i])); }
}

TEST(notify_waiter_slot_bounds_checked) {
    asx_notify_handle h;
    asx_notify_waiter w;
    setup();
    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w));

    w.waiter_slot = UINT32_MAX; /* beyond the wait-node pool */
    ASSERT_EQ(asx_notify_poll_wait(&w, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_notify_wait_cancel(&w), ASX_E_INVALID_ARGUMENT);

    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

/* ================================================================== */
/* Semaphore tests                                                     */
/* ================================================================== */

TEST(sem_create_close) {
    asx_semaphore_handle h;
    setup();
    ASSERT_EQ(asx_semaphore_create(3, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 3u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_try_acquire_release) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    setup();
    MUST_OK(asx_semaphore_create(1, &h));
    ASSERT_EQ(asx_semaphore_try_acquire(h, &p), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 0u);
    ASSERT_EQ(asx_semaphore_release(p), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_try_acquire_would_block) {
    asx_semaphore_handle h;
    asx_semaphore_permit p1, p2;
    setup();
    MUST_OK(asx_semaphore_create(1, &h));
    MUST_OK(asx_semaphore_try_acquire(h, &p1));
    ASSERT_EQ(asx_semaphore_try_acquire(h, &p2), ASX_E_WOULD_BLOCK);
    MUST_OK(asx_semaphore_release(p1));
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_multiple_permits) {
    asx_semaphore_handle h;
    asx_semaphore_permit p1, p2, p3, p4;
    setup();
    MUST_OK(asx_semaphore_create(3, &h));
    MUST_OK(asx_semaphore_try_acquire(h, &p1));
    MUST_OK(asx_semaphore_try_acquire(h, &p2));
    MUST_OK(asx_semaphore_try_acquire(h, &p3));
    ASSERT_EQ(asx_semaphore_try_acquire(h, &p4), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_semaphore_available(h), 0u);

    MUST_OK(asx_semaphore_release(p2));
    ASSERT_EQ(asx_semaphore_available(h), 1u);
    MUST_OK(asx_semaphore_release(p1));
    MUST_OK(asx_semaphore_release(p3));
    ASSERT_EQ(asx_semaphore_available(h), 3u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_async_acquire) {
    asx_semaphore_handle h;
    asx_semaphore_permit p1, p2;
    asx_semaphore_waiter w;
    setup();
    MUST_OK(asx_semaphore_create(1, &h));
    MUST_OK(asx_semaphore_try_acquire(h, &p1));

    /* All permits taken, async acquire should pend */
    MUST_OK(asx_semaphore_acquire_begin(h, &w));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w, &p2, NULL), ASX_E_PENDING);

    /* Release p1 — waiter should get permit on next poll */
    MUST_OK(asx_semaphore_release(p1));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w, &p2, NULL), ASX_OK);

    MUST_OK(asx_semaphore_release(p2));
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_fifo_fairness) {
    asx_semaphore_handle h;
    asx_semaphore_permit p, pw1, pw2;
    asx_semaphore_waiter w1, w2;
    setup();
    MUST_OK(asx_semaphore_create(1, &h));
    MUST_OK(asx_semaphore_try_acquire(h, &p));

    /* Two waiters queue up */
    MUST_OK(asx_semaphore_acquire_begin(h, &w1));
    MUST_OK(asx_semaphore_acquire_begin(h, &w2));

    /* Release — w1 should get it (FIFO) */
    MUST_OK(asx_semaphore_release(p));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &pw1, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &pw2, NULL), ASX_E_PENDING);

    /* Release w1's permit — w2 gets it */
    MUST_OK(asx_semaphore_release(pw1));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &pw2, NULL), ASX_OK);

    MUST_OK(asx_semaphore_release(pw2));
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_acquire_cancel) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    asx_semaphore_waiter w;
    setup();
    MUST_OK(asx_semaphore_create(1, &h));
    MUST_OK(asx_semaphore_try_acquire(h, &p));
    MUST_OK(asx_semaphore_acquire_begin(h, &w));
    MUST_OK(asx_semaphore_acquire_cancel(&w));
    /* Cancelled waiter doesn't consume a permit */
    MUST_OK(asx_semaphore_release(p));
    ASSERT_EQ(asx_semaphore_available(h), 1u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

TEST(sem_close_disconnects) {
    asx_semaphore_handle h;
    asx_semaphore_permit p_dummy;
    asx_semaphore_waiter w;
    setup();
    MUST_OK(asx_semaphore_create(0, &h));
    MUST_OK(asx_semaphore_acquire_begin(h, &w));
    MUST_OK(asx_semaphore_close(h));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w, &p_dummy, NULL), ASX_E_DISCONNECTED);
}

TEST(sem_stale_handle) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    setup();
    MUST_OK(asx_semaphore_create(1, &h));
    MUST_OK(asx_semaphore_close(h));
    ASSERT_EQ(asx_semaphore_try_acquire(h, &p), ASX_E_STALE_HANDLE);
}

TEST(sem_zero_permits) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    setup();
    MUST_OK(asx_semaphore_create(0, &h));
    ASSERT_EQ(asx_semaphore_available(h), 0u);
    ASSERT_EQ(asx_semaphore_try_acquire(h, &p), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

/* Rust's Semaphore::acquire(cx, n) takes n permits all or nothing, in
 * line: a waiter for 2 at the front holds up a later waiter for 1 even
 * while 1 permit is free. */
TEST(sem_acquire_many_all_or_nothing_in_line) {
    asx_semaphore_handle h;
    asx_semaphore_permit held, p2, p1, extra;
    asx_semaphore_waiter w2, w1;
    setup();
    MUST_OK(asx_semaphore_create(3, &h));
    ASSERT_EQ(asx_semaphore_try_acquire_many(h, 2, &held), ASX_OK);
    ASSERT_EQ(held.count, 2u);
    ASSERT_EQ(asx_semaphore_available(h), 1u);

    MUST_OK(asx_semaphore_acquire_many_begin(h, 2, &w2));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_E_PENDING);       /* 1 free < 2 */
    ASSERT_EQ(asx_semaphore_try_acquire_many(h, 1, &extra), ASX_E_WOULD_BLOCK); /* queued */
    MUST_OK(asx_semaphore_acquire_begin(h, &w1));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &p1, NULL), ASX_E_PENDING); /* behind w2 */
    ASSERT_EQ(asx_semaphore_available(h), 1u);

    /* Releasing the 2 held makes 3: w2 takes 2, then w1 the last one. */
    ASSERT_EQ(asx_semaphore_release(held), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_OK);
    ASSERT_EQ(p2.count, 2u);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &p1, NULL), ASX_OK);
    ASSERT_EQ(p1.count, 1u);
    ASSERT_EQ(asx_semaphore_available(h), 0u);

    ASSERT_EQ(asx_semaphore_release(p2), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(p1), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 3u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

/* Acquiring 0 permits succeeds at once with an empty permit (Rust), and
 * the mutex's semaphore only ever takes 1. */
TEST(sem_acquire_zero_and_mutex_count) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    asx_semaphore_waiter w;
    asx_mutex_handle m;
    setup();
    MUST_OK(asx_semaphore_create(0, &h));
    ASSERT_EQ(asx_semaphore_try_acquire_many(h, 0, &p), ASX_OK);
    ASSERT_EQ(p.count, 0u);
    MUST_OK(asx_semaphore_acquire_many_begin(h, 0, &w));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w, &p, NULL), ASX_OK);
    ASSERT_EQ(p.count, 0u);
    ASSERT_EQ(asx_semaphore_release(p), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 0u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);

    MUST_OK(asx_mutex_create(&m));
    ASSERT_EQ(asx_semaphore_try_acquire_many(m.sem, 2, &p), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_semaphore_acquire_many_begin(m.sem, 2, &w), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_mutex_close(m), ASX_OK);
}

/* A permit is released once: a second release of the same value would
 * return permits nobody holds, so it is refused and changes nothing (Rust
 * consumes the permit on drop). The same holds for a mutex guard. */
TEST(sem_double_release_is_refused) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    asx_mutex_handle m;
    asx_mutex_guard g;
    asx_mutex_guard g2;
    setup();
    MUST_OK(asx_semaphore_create(2, &h));
    ASSERT_EQ(asx_semaphore_try_acquire_many(h, 2, &p), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(p), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 2u);
    ASSERT_EQ(asx_semaphore_release(p), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_semaphore_available(h), 2u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);

    MUST_OK(asx_mutex_create(&m));
    ASSERT_EQ(asx_mutex_try_lock(m, &g), ASX_OK);
    ASSERT_EQ(asx_mutex_unlock(g), ASX_OK);
    ASSERT_EQ(asx_mutex_unlock(g), ASX_E_INVALID_STATE);
    /* Still one lock: a second locker is refused while the first holds. */
    ASSERT_EQ(asx_mutex_try_lock(m, &g), ASX_OK);
    ASSERT_EQ(asx_mutex_try_lock(m, &g2), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_mutex_unlock(g), ASX_OK);
    ASSERT_EQ(asx_mutex_close(m), ASX_OK);
}

/* A guard copy kept past its unlock names a hold that ended, so it cannot
 * unlock (or poison) the lock that a later holder took, whether that one
 * took it with try_lock or was handed it by the unlock. Rust's guard is
 * consumed by its drop: no stale guard exists. */
TEST(mutex_stale_guard_does_not_unlock_a_later_holder) {
    asx_mutex_handle m;
    asx_mutex_guard first;
    asx_mutex_guard second;
    asx_mutex_guard third;
    asx_mutex_guard other;
    asx_mutex_lock_waiter w;
    setup();
    MUST_OK(asx_mutex_create(&m));
    ASSERT_EQ(asx_mutex_try_lock(m, &first), ASX_OK);
    ASSERT_EQ(asx_mutex_unlock(first), ASX_OK);
    ASSERT_EQ(asx_mutex_try_lock(m, &second), ASX_OK);
    ASSERT_EQ(asx_mutex_unlock(first), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_mutex_unlock_poisoned(first), ASX_E_INVALID_STATE);
    ASSERT_FALSE(asx_mutex_is_poisoned(m));
    ASSERT_TRUE(asx_mutex_is_locked(m));
    ASSERT_EQ(asx_mutex_try_lock(m, &other), ASX_E_WOULD_BLOCK);

    MUST_OK(asx_mutex_lock_begin(m, &w));
    ASSERT_EQ(asx_mutex_poll_lock(&w, &third, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_mutex_unlock(second), ASX_OK); /* handed to the waiter */
    ASSERT_EQ(asx_mutex_unlock(second), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_mutex_poll_lock(&w, &third, NULL), ASX_OK);
    ASSERT_EQ(asx_mutex_unlock(second), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_mutex_unlock(first), ASX_E_INVALID_STATE);
    ASSERT_TRUE(asx_mutex_is_locked(m));
    ASSERT_EQ(asx_mutex_unlock(third), ASX_OK);
    ASSERT_FALSE(asx_mutex_is_locked(m));
    ASSERT_EQ(asx_mutex_close(m), ASX_OK);
}

/* Rust SemaphorePermit::forget: the permits never return to the pool, and
 * forgetting (or releasing) the permit afterwards is refused. */
TEST(sem_forget_keeps_permits_out_of_the_pool) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    asx_mutex_handle m;
    asx_mutex_guard g;
    setup();
    MUST_OK(asx_semaphore_create(3, &h));
    ASSERT_EQ(asx_semaphore_try_acquire_many(h, 2, &p), ASX_OK);
    ASSERT_EQ(asx_semaphore_forget(p), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
    ASSERT_EQ(asx_semaphore_forget(p), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_semaphore_release(p), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
    ASSERT_EQ(asx_semaphore_forget(p), ASX_E_STALE_HANDLE);

    MUST_OK(asx_mutex_create(&m));
    ASSERT_EQ(asx_mutex_try_lock(m, &g), ASX_OK);
    ASSERT_EQ(asx_semaphore_forget(g.permit), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_mutex_unlock(g), ASX_OK);
    ASSERT_EQ(asx_mutex_close(m), ASX_OK);
}

/* Rust Semaphore::add_permits: saturating; the front waiter is woken if it
 * can now take what it asked for, and only it (FIFO: a large waiter at
 * the front is not passed by a smaller one behind it). */
TEST(sem_add_permits_serves_the_front_of_the_line) {
    asx_semaphore_handle h;
    asx_semaphore_permit p2;
    asx_semaphore_permit p1;
    asx_semaphore_waiter w2;
    asx_semaphore_waiter w1;
    asx_mutex_handle m;
    setup();
    MUST_OK(asx_semaphore_create(0, &h));
    MUST_OK(asx_semaphore_acquire_many_begin(h, 2, &w2));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_E_PENDING);
    MUST_OK(asx_semaphore_acquire_begin(h, &w1));
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &p1, NULL), ASX_E_PENDING);

    ASSERT_EQ(asx_semaphore_add_permits(h, 0), ASX_OK);
    ASSERT_EQ(asx_semaphore_add_permits(h, 1), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
    /* One free is not enough for the front (2), and w1 may not pass it. */
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &p1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_semaphore_add_permits(h, 2), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &p1, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 0u);
    ASSERT_EQ(asx_semaphore_release(p2), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(p1), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 3u);

    ASSERT_EQ(asx_semaphore_add_permits(h, UINT32_MAX), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), UINT32_MAX);
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
    ASSERT_EQ(asx_semaphore_add_permits(h, 1), ASX_E_STALE_HANDLE);

    MUST_OK(asx_mutex_create(&m));
    ASSERT_EQ(asx_semaphore_add_permits(m.sem, 1), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_mutex_close(m), ASX_OK);
}

/* ================================================================== */
/* Mutex tests                                                         */
/* ================================================================== */

TEST(mutex_create_close) {
    asx_mutex_handle h;
    setup();
    ASSERT_EQ(asx_mutex_create(&h), ASX_OK);
    ASSERT_FALSE(asx_mutex_is_locked(h));
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
}

TEST(mutex_try_lock_unlock) {
    asx_mutex_handle h;
    asx_mutex_guard g;
    setup();
    MUST_OK(asx_mutex_create(&h));
    ASSERT_EQ(asx_mutex_try_lock(h, &g), ASX_OK);
    ASSERT_TRUE(asx_mutex_is_locked(h));
    ASSERT_EQ(asx_mutex_unlock(g), ASX_OK);
    ASSERT_FALSE(asx_mutex_is_locked(h));
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
}

TEST(mutex_double_lock_blocks) {
    asx_mutex_handle h;
    asx_mutex_guard g1, g2;
    setup();
    MUST_OK(asx_mutex_create(&h));
    MUST_OK(asx_mutex_try_lock(h, &g1));
    ASSERT_EQ(asx_mutex_try_lock(h, &g2), ASX_E_WOULD_BLOCK);
    MUST_OK(asx_mutex_unlock(g1));
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
}

TEST(mutex_async_lock) {
    asx_mutex_handle h;
    asx_mutex_guard g1, g2;
    asx_mutex_lock_waiter w;
    setup();
    MUST_OK(asx_mutex_create(&h));
    MUST_OK(asx_mutex_try_lock(h, &g1));

    MUST_OK(asx_mutex_lock_begin(h, &w));
    ASSERT_EQ(asx_mutex_poll_lock(&w, &g2, NULL), ASX_E_PENDING);

    MUST_OK(asx_mutex_unlock(g1));
    ASSERT_EQ(asx_mutex_poll_lock(&w, &g2, NULL), ASX_OK);
    ASSERT_TRUE(asx_mutex_is_locked(h));
    MUST_OK(asx_mutex_unlock(g2));
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
}

TEST(mutex_lock_cancel) {
    asx_mutex_handle h;
    asx_mutex_guard g1;
    asx_mutex_lock_waiter w;
    setup();
    MUST_OK(asx_mutex_create(&h));
    MUST_OK(asx_mutex_try_lock(h, &g1));
    MUST_OK(asx_mutex_lock_begin(h, &w));
    MUST_OK(asx_mutex_lock_cancel(&w));
    MUST_OK(asx_mutex_unlock(g1));
    ASSERT_FALSE(asx_mutex_is_locked(h));
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
}

/* Rust poisons a mutex whose guard drops while its task panics; try_lock
 * and the queued lock then fail with Poisoned (ASX_E_INVALID_STATE). */
TEST(mutex_unlock_poisoned_poisons) {
    asx_mutex_handle h;
    asx_mutex_guard g1, g2;
    asx_mutex_lock_waiter w;
    setup();
    MUST_OK(asx_mutex_create(&h));
    MUST_OK(asx_mutex_try_lock(h, &g1));
    MUST_OK(asx_mutex_lock_begin(h, &w));
    ASSERT_EQ(asx_mutex_poll_lock(&w, &g2, NULL), ASX_E_PENDING);
    ASSERT_FALSE(asx_mutex_is_poisoned(h));

    ASSERT_EQ(asx_mutex_unlock_poisoned(g1), ASX_OK);
    ASSERT_TRUE(asx_mutex_is_poisoned(h));
    /* The queued waiter was handed the lock and gives it back failing. */
    ASSERT_EQ(asx_mutex_poll_lock(&w, &g2, NULL), ASX_E_INVALID_STATE);
    ASSERT_FALSE(asx_mutex_is_locked(h));
    ASSERT_EQ(asx_mutex_try_lock(h, &g2), ASX_E_INVALID_STATE);
    MUST_OK(asx_mutex_lock_begin(h, &w));
    ASSERT_EQ(asx_mutex_poll_lock(&w, &g2, NULL), ASX_E_INVALID_STATE);
    /* A guard released twice poisons nothing more and is refused. */
    ASSERT_EQ(asx_mutex_unlock_poisoned(g1), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
    ASSERT_FALSE(asx_mutex_is_poisoned(h)); /* closed: stale handle */
}

TEST(mutex_plain_unlock_does_not_poison) {
    asx_mutex_handle h;
    asx_mutex_guard g;
    asx_mutex_guard stale;
    setup();
    MUST_OK(asx_mutex_create(&h));
    MUST_OK(asx_mutex_try_lock(h, &g));
    stale = g;
    MUST_OK(asx_mutex_unlock(g));
    ASSERT_FALSE(asx_mutex_is_poisoned(h));
    /* Releasing an already released guard refuses and poisons nothing. */
    ASSERT_EQ(asx_mutex_unlock_poisoned(stale), ASX_E_INVALID_STATE);
    ASSERT_FALSE(asx_mutex_is_poisoned(h));
    ASSERT_EQ(asx_mutex_try_lock(h, &g), ASX_OK);
    MUST_OK(asx_mutex_unlock(g));
    ASSERT_EQ(asx_mutex_close(h), ASX_OK);
}

/* ================================================================== */
/* Barrier tests                                                       */
/* ================================================================== */

TEST(barrier_create_close) {
    asx_barrier_handle h;
    setup();
    ASSERT_EQ(asx_barrier_create(3, &h), ASX_OK);
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);
    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_zero_fails) {
    asx_barrier_handle h;
    setup();
    ASSERT_EQ(asx_barrier_create(0, &h), ASX_E_INVALID_ARGUMENT);
}

TEST(barrier_single) {
    asx_barrier_handle h;
    asx_barrier_waiter w;
    setup();
    MUST_OK(asx_barrier_create(1, &h));
    MUST_OK(asx_barrier_wait_begin(h, &w));
    /* N=1, immediately released */
    ASSERT_EQ(asx_barrier_poll_wait(&w, NULL), ASX_OK);
    ASSERT_TRUE(w.is_leader);
    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_two_tasks) {
    asx_barrier_handle h;
    asx_barrier_waiter w1, w2;
    setup();
    MUST_OK(asx_barrier_create(2, &h));

    /* A waiter arrives at its first poll (Rust's BarrierWaitFuture), not
     * at wait_begin. The first arrives and waits. */
    MUST_OK(asx_barrier_wait_begin(h, &w1));
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);

    /* The second arrival trips it and leads; the first is released */
    MUST_OK(asx_barrier_wait_begin(h, &w2));
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);
    ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_OK);
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_OK);

    ASSERT_TRUE(w2.is_leader);
    ASSERT_FALSE(w1.is_leader);

    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_three_tasks) {
    asx_barrier_handle h;
    asx_barrier_waiter w1, w2, w3;
    setup();
    MUST_OK(asx_barrier_create(3, &h));

    MUST_OK(asx_barrier_wait_begin(h, &w1));
    MUST_OK(asx_barrier_wait_begin(h, &w2));
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_E_PENDING);

    MUST_OK(asx_barrier_wait_begin(h, &w3));
    ASSERT_EQ(asx_barrier_poll_wait(&w3, NULL), ASX_OK); /* trips it */
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_OK);
    ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_OK);
    ASSERT_TRUE(w3.is_leader);
    ASSERT_FALSE(w1.is_leader);
    ASSERT_FALSE(w2.is_leader);

    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_wait_cancel) {
    asx_barrier_handle h;
    asx_barrier_waiter w;
    asx_barrier_waiter idle;
    setup();
    MUST_OK(asx_barrier_create(2, &h));
    MUST_OK(asx_barrier_wait_begin(h, &w));
    ASSERT_EQ(asx_barrier_poll_wait(&w, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);
    MUST_OK(asx_barrier_wait_cancel(&w));
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);
    /* A waiter that never polled never arrived: cancelling it changes
     * nothing. */
    MUST_OK(asx_barrier_wait_begin(h, &idle));
    MUST_OK(asx_barrier_wait_cancel(&idle));
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);
    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_close_disconnects) {
    asx_barrier_handle h;
    asx_barrier_waiter w;
    setup();
    MUST_OK(asx_barrier_create(2, &h));
    MUST_OK(asx_barrier_wait_begin(h, &w));
    MUST_OK(asx_barrier_close(h));
    ASSERT_EQ(asx_barrier_poll_wait(&w, NULL), ASX_E_DISCONNECTED);
}

/* Y1 regression: the barrier is cyclic (Rust sync/barrier.rs resets
 * `arrived` and advances the generation on trip). Before the fix,
 * `arrived` stayed at the threshold after the first trip, so every later
 * arrival tripped the barrier alone and was elected leader. */
TEST(barrier_reusable_across_rounds) {
    asx_barrier_handle h;
    asx_barrier_waiter w1, w2, w3;
    int round;
    setup();
    MUST_OK(asx_barrier_create(3, &h));

    for (round = 0; round < 3; round++) {
        MUST_OK(asx_barrier_wait_begin(h, &w1));
        ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_E_PENDING);
        MUST_OK(asx_barrier_wait_begin(h, &w2));
        ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_E_PENDING);
        ASSERT_EQ(asx_barrier_waiting_count(h), 2u);

        MUST_OK(asx_barrier_wait_begin(h, &w3));
        ASSERT_EQ(asx_barrier_poll_wait(&w3, NULL), ASX_OK); /* trips this round */
        ASSERT_EQ(asx_barrier_waiting_count(h), 0u);

        ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_OK);
        ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_OK);
        /* Exactly one leader per round: the arrival that tripped it */
        ASSERT_FALSE(w1.is_leader);
        ASSERT_FALSE(w2.is_leader);
        ASSERT_TRUE(w3.is_leader);
    }

    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_cancel_counts_only_current_round) {
    asx_barrier_handle h;
    asx_barrier_waiter a, b, c, d;
    setup();
    MUST_OK(asx_barrier_create(2, &h));

    /* Round 1 trips at b's arrival; a is released but has not polled
     * since */
    MUST_OK(asx_barrier_wait_begin(h, &a));
    ASSERT_EQ(asx_barrier_poll_wait(&a, NULL), ASX_E_PENDING);
    MUST_OK(asx_barrier_wait_begin(h, &b));
    ASSERT_EQ(asx_barrier_poll_wait(&b, NULL), ASX_OK);
    ASSERT_TRUE(b.is_leader);
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);

    /* Round 2 starts with c */
    MUST_OK(asx_barrier_wait_begin(h, &c));
    ASSERT_EQ(asx_barrier_poll_wait(&c, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);

    /* Withdrawing a released round-1 waiter must not touch round 2 */
    MUST_OK(asx_barrier_wait_cancel(&a));
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);

    /* Cancelling c withdraws its round-2 arrival; round 2 still needs two */
    MUST_OK(asx_barrier_wait_cancel(&c));
    ASSERT_EQ(asx_barrier_waiting_count(h), 0u);
    MUST_OK(asx_barrier_wait_begin(h, &d));
    ASSERT_EQ(asx_barrier_poll_wait(&d, NULL), ASX_E_PENDING);
    MUST_OK(asx_barrier_wait_begin(h, &c));
    ASSERT_EQ(asx_barrier_poll_wait(&c, NULL), ASX_OK);
    ASSERT_EQ(asx_barrier_poll_wait(&d, NULL), ASX_OK);
    ASSERT_FALSE(d.is_leader);
    ASSERT_TRUE(c.is_leader);

    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

/* ================================================================== */
/* OnceCell tests                                                      */
/* ================================================================== */

static asx_status init_42(void *user_data, uint64_t *out) {
    (void)user_data;
    *out = 42;
    return ASX_OK;
}

static asx_status init_from_ptr(void *user_data, uint64_t *out) {
    *out = *(uint64_t *)user_data;
    return ASX_OK;
}

static asx_status init_fail(void *user_data, uint64_t *out) {
    (void)user_data;
    (void)out;
    return ASX_E_INVALID_STATE;
}

TEST(once_create_close) {
    asx_once_handle h;
    setup();
    ASSERT_EQ(asx_once_create(&h), ASX_OK);
    ASSERT_FALSE(asx_once_is_initialized(h));
    ASSERT_EQ(asx_once_close(h), ASX_OK);
}

TEST(once_get_uninitialized_fails) {
    asx_once_handle h;
    uint64_t val;
    setup();
    MUST_OK(asx_once_create(&h));
    ASSERT_EQ(asx_once_get(h, &val), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_once_close(h), ASX_OK);
}

TEST(once_get_or_init) {
    asx_once_handle h;
    uint64_t val;
    setup();
    MUST_OK(asx_once_create(&h));
    ASSERT_EQ(asx_once_get_or_init(h, init_42, NULL, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);
    ASSERT_TRUE(asx_once_is_initialized(h));
    ASSERT_EQ(asx_once_close(h), ASX_OK);
}

TEST(once_init_called_once) {
    asx_once_handle h;
    uint64_t val1 = 100, val2 = 200;
    uint64_t result;
    setup();
    MUST_OK(asx_once_create(&h));
    MUST_OK(asx_once_get_or_init(h, init_from_ptr, &val1, &result));
    ASSERT_EQ(result, (uint64_t)100);

    /* Second call should return cached value, not call init again */
    MUST_OK(asx_once_get_or_init(h, init_from_ptr, &val2, &result));
    ASSERT_EQ(result, (uint64_t)100); /* still 100, not 200 */

    ASSERT_EQ(asx_once_close(h), ASX_OK);
}

TEST(once_get_after_init) {
    asx_once_handle h;
    uint64_t val;
    setup();
    MUST_OK(asx_once_create(&h));
    MUST_OK(asx_once_get_or_init(h, init_42, NULL, &val));
    ASSERT_EQ(asx_once_get(h, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);
    ASSERT_EQ(asx_once_close(h), ASX_OK);
}

TEST(once_init_fail_retryable) {
    asx_once_handle h;
    uint64_t val;
    setup();
    MUST_OK(asx_once_create(&h));
    /* First init fails */
    ASSERT_EQ(asx_once_get_or_init(h, init_fail, NULL, &val), ASX_E_INVALID_STATE);
    ASSERT_FALSE(asx_once_is_initialized(h));

    /* Can retry with different init fn */
    ASSERT_EQ(asx_once_get_or_init(h, init_42, NULL, &val), ASX_OK);
    ASSERT_EQ(val, (uint64_t)42);
    ASSERT_EQ(asx_once_close(h), ASX_OK);
}

TEST(once_stale_handle) {
    asx_once_handle h;
    uint64_t val;
    setup();
    MUST_OK(asx_once_create(&h));
    MUST_OK(asx_once_close(h));
    ASSERT_EQ(asx_once_get(h, &val), ASX_E_STALE_HANDLE);
}

TEST(once_exhaustion) {
    asx_once_handle handles[ASX_ONCE_MAX];
    asx_once_handle extra;
    uint32_t i;
    setup();
    for (i = 0; i < ASX_ONCE_MAX; i++) { MUST_OK(asx_once_create(&handles[i])); }
    ASSERT_EQ(asx_once_create(&extra), ASX_E_RESOURCE_EXHAUSTED);
    for (i = 0; i < ASX_ONCE_MAX; i++) { MUST_OK(asx_once_close(handles[i])); }
}

/* ================================================================== */
/* Cx budget exhaustion integration test                               */
/* ================================================================== */

TEST(notify_cx_budget_exhaustion) {
    asx_notify_handle h;
    asx_notify_waiter w;
    asx_cx cx;
    asx_budget budget;
    setup();

    /* Set up a Cx with budget that has 0 polls remaining */
    MUST_OK(asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME));
    budget = asx_budget_zero();
    MUST_OK(asx_cx_bind_budget(&cx, &budget));

    MUST_OK(asx_notify_create(&h));
    MUST_OK(asx_notify_wait_begin(h, &w));

    /* Poll should return budget exhausted via checkpoint */
    ASSERT_EQ(asx_notify_poll_wait(&w, &cx), ASX_E_POLL_BUDGET_EXHAUSTED);
    /* Waiter should be cleaned up */
    ASSERT_EQ(asx_notify_waiter_count(h), 0u);

    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(sem_cx_budget_exhaustion) {
    asx_semaphore_handle h;
    asx_semaphore_permit p, p2;
    asx_semaphore_waiter w;
    asx_cx cx;
    asx_budget budget;
    setup();

    MUST_OK(asx_cx_init(&cx, 1, 1, ASX_CAP_BUDGET_READ | ASX_CAP_BUDGET_CONSUME));
    budget = asx_budget_zero();
    MUST_OK(asx_cx_bind_budget(&cx, &budget));

    MUST_OK(asx_semaphore_create(1, &h));
    MUST_OK(asx_semaphore_try_acquire(h, &p));
    MUST_OK(asx_semaphore_acquire_begin(h, &w));

    ASSERT_EQ(asx_semaphore_poll_acquire(&w, &p2, &cx), ASX_E_POLL_BUDGET_EXHAUSTED);

    MUST_OK(asx_semaphore_release(p));
    ASSERT_EQ(asx_semaphore_close(h), ASX_OK);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void) {
    fprintf(stderr, "=== test_sync ===\n");

    /* Notify */
    RUN_TEST(notify_create_close);
    RUN_TEST(notify_create_null_fails);
    RUN_TEST(notify_one_no_waiters);
    RUN_TEST(notify_all_no_waiters);
    RUN_TEST(notify_one_before_wait_is_stored);
    RUN_TEST(notify_stored_permit_survives_abandoned_waiter);
    RUN_TEST(notify_wait_then_signal);
    RUN_TEST(notify_all_wakes_multiple);
    RUN_TEST(notify_one_fifo);
    RUN_TEST(notify_wait_cancel);
    RUN_TEST(notify_close_disconnects_waiters);
    RUN_TEST(notify_stale_handle);
    RUN_TEST(notify_exhaustion);
    RUN_TEST(notify_waiter_slot_bounds_checked);

    /* Semaphore */
    RUN_TEST(sem_create_close);
    RUN_TEST(sem_try_acquire_release);
    RUN_TEST(sem_try_acquire_would_block);
    RUN_TEST(sem_multiple_permits);
    RUN_TEST(sem_async_acquire);
    RUN_TEST(sem_fifo_fairness);
    RUN_TEST(sem_acquire_cancel);
    RUN_TEST(sem_close_disconnects);
    RUN_TEST(sem_stale_handle);
    RUN_TEST(sem_zero_permits);
    RUN_TEST(sem_acquire_many_all_or_nothing_in_line);
    RUN_TEST(sem_acquire_zero_and_mutex_count);
    RUN_TEST(sem_double_release_is_refused);
    RUN_TEST(mutex_stale_guard_does_not_unlock_a_later_holder);
    RUN_TEST(sem_forget_keeps_permits_out_of_the_pool);
    RUN_TEST(sem_add_permits_serves_the_front_of_the_line);

    /* Mutex */
    RUN_TEST(mutex_create_close);
    RUN_TEST(mutex_try_lock_unlock);
    RUN_TEST(mutex_double_lock_blocks);
    RUN_TEST(mutex_async_lock);
    RUN_TEST(mutex_lock_cancel);
    RUN_TEST(mutex_unlock_poisoned_poisons);
    RUN_TEST(mutex_plain_unlock_does_not_poison);

    /* Barrier */
    RUN_TEST(barrier_create_close);
    RUN_TEST(barrier_zero_fails);
    RUN_TEST(barrier_single);
    RUN_TEST(barrier_two_tasks);
    RUN_TEST(barrier_three_tasks);
    RUN_TEST(barrier_wait_cancel);
    RUN_TEST(barrier_close_disconnects);
    RUN_TEST(barrier_reusable_across_rounds);
    RUN_TEST(barrier_cancel_counts_only_current_round);

    /* OnceCell */
    RUN_TEST(once_create_close);
    RUN_TEST(once_get_uninitialized_fails);
    RUN_TEST(once_get_or_init);
    RUN_TEST(once_init_called_once);
    RUN_TEST(once_get_after_init);
    RUN_TEST(once_init_fail_retryable);
    RUN_TEST(once_stale_handle);
    RUN_TEST(once_exhaustion);

    /* Cx integration */
    RUN_TEST(notify_cx_budget_exhaustion);
    RUN_TEST(sem_cx_budget_exhaustion);

    TEST_REPORT();
    return test_failures;
}
