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
    ASSERT_EQ(asx_notify_one(h), ASX_OK); /* no-op, no waiters */
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
}

TEST(notify_all_no_waiters) {
    asx_notify_handle h;
    setup();
    MUST_OK(asx_notify_create(&h));
    ASSERT_EQ(asx_notify_all(h), ASX_OK);
    ASSERT_EQ(asx_notify_close(h), ASX_OK);
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

    w.waiter_slot = ASX_NOTIFY_MAX_WAITERS;
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

    /* First arrives, pending */
    MUST_OK(asx_barrier_wait_begin(h, &w1));
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);

    /* Second arrives, both released */
    MUST_OK(asx_barrier_wait_begin(h, &w2));
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_OK);
    ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_OK);

    /* Last to arrive is leader */
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
    ASSERT_EQ(asx_barrier_poll_wait(&w1, NULL), ASX_OK);
    ASSERT_EQ(asx_barrier_poll_wait(&w2, NULL), ASX_OK);
    ASSERT_EQ(asx_barrier_poll_wait(&w3, NULL), ASX_OK);
    ASSERT_TRUE(w3.is_leader);

    ASSERT_EQ(asx_barrier_close(h), ASX_OK);
}

TEST(barrier_wait_cancel) {
    asx_barrier_handle h;
    asx_barrier_waiter w;
    setup();
    MUST_OK(asx_barrier_create(2, &h));
    MUST_OK(asx_barrier_wait_begin(h, &w));
    ASSERT_EQ(asx_barrier_waiting_count(h), 1u);
    MUST_OK(asx_barrier_wait_cancel(&w));
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

    /* Mutex */
    RUN_TEST(mutex_create_close);
    RUN_TEST(mutex_try_lock_unlock);
    RUN_TEST(mutex_double_lock_blocks);
    RUN_TEST(mutex_async_lock);
    RUN_TEST(mutex_lock_cancel);

    /* Barrier */
    RUN_TEST(barrier_create_close);
    RUN_TEST(barrier_zero_fails);
    RUN_TEST(barrier_single);
    RUN_TEST(barrier_two_tasks);
    RUN_TEST(barrier_three_tasks);
    RUN_TEST(barrier_wait_cancel);
    RUN_TEST(barrier_close_disconnects);

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
