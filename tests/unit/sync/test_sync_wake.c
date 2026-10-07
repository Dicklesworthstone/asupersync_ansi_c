/*
 * test_sync_wake.c — wake-driven waiting for sync primitives
 *
 * Waiters polled inside the scheduler park and are woken by the state
 * change that serves them: semaphore/mutex grants in arrival order,
 * mutex handoff, rwlock writer preference, notify, barrier trip, async
 * once initialization, pool returns; close waking everyone; cancel of a
 * queued or woken waiter passing its permit on; determinism across runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_harness.h"
#include <asx/asx.h>
#include <asx/core/oneshot.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/sync/barrier.h>
#include <asx/sync/contended_mutex.h>
#include <asx/sync/mutex.h>
#include <asx/sync/notify.h>
#include <asx/sync/once.h>
#include <asx/sync/pool.h>
#include <asx/sync/rwlock.h>
#include <asx/sync/semaphore.h>
#include <asx/time/sleep.h>
#include <string.h>

#define MS ((uint64_t)1000000u)

static asx_runtime g_rt;
static asx_region_id g_region;

/* Virtual-clock runtime (default stub hooks), fresh sync arenas, and an
 * open region. */
static int setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    asx_rwlock_reset();
    asx_pool_reset();
    asx_contended_mutex_reset();
    if (st == ASX_OK) st = asx_region_open(&g_region);
    return st == ASX_OK;
}

static uint32_t polls_used(uint32_t start, const asx_budget *b) {
    return start - asx_budget_polls(b);
}

static int task_cancelled(asx_task_id t) {
    asx_outcome out;
    if (asx_task_get_outcome(t, &out) != ASX_OK) return 0;
    return asx_outcome_severity_of(&out) == ASX_OUTCOME_CANCELLED;
}

static int cancel_requested(asx_task_id self) {
    asx_checkpoint_result cr;
    if (asx_checkpoint(self, &cr) != ASX_OK) return 0;
    return cr.cancelled;
}

/* One-time sleep helper: ASX_OK once `delay_ns` elapsed (or if 0). */
typedef struct {
    asx_sleep_state sleep;
    int started;
    int done;
} once_sleep;

static asx_status sleep_once(once_sleep *s, uint64_t delay_ns, asx_task_id self) {
    asx_status st;
    if (delay_ns == 0u || s->done) return ASX_OK;
    if (!s->started) {
        st = asx_sleep_init(&s->sleep, delay_ns);
        if (st != ASX_OK) return st;
        s->started = 1;
    }
    st = asx_sleep_poll(&s->sleep, self);
    if (st == ASX_OK) s->done = 1;
    return st;
}

/* Completion order shared by the fixtures. */
static uint32_t g_order[8];
static uint32_t g_order_len;
static uint32_t g_holders;
static uint32_t g_max_holders;

static void record(uint32_t id) {
    if (g_order_len < 8u) g_order[g_order_len++] = id;
}

/* ===================================================================
 * Semaphore / mutex fixtures
 * =================================================================== */

typedef struct {
    uint32_t id;
    asx_semaphore_handle sem;
    asx_semaphore_waiter waiter;
    asx_semaphore_permit permit;
    uint64_t arrive_after; /* sleep before queueing */
    uint64_t hold_for;     /* sleep while holding the permit */
    once_sleep arrive;
    once_sleep hold;
    int begun;
    int acquired;
    int cancel_aware;
    int abandon; /* complete without cancelling the queued acquire */
    uint32_t polls;
    asx_status result;
} sem_task;

/* Arrive (after a delay), acquire, hold (for a while), release. */
static asx_status poll_sem_task(void *ud, asx_task_id self) {
    sem_task *s = (sem_task *)ud;
    asx_status st;
    s->polls++;

    if (s->abandon) return ASX_OK;
    if (s->cancel_aware && !s->acquired && s->begun && cancel_requested(self)) {
        /* Give up the queued acquire: a granted permit passes on. */
        s->result = asx_semaphore_acquire_cancel(&s->waiter);
        return ASX_OK;
    }
    st = sleep_once(&s->arrive, s->arrive_after, self);
    if (st != ASX_OK) return st;
    if (!s->begun) {
        st = asx_semaphore_acquire_begin(s->sem, &s->waiter);
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    if (!s->acquired) {
        st = asx_semaphore_poll_acquire(&s->waiter, &s->permit, NULL);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->result = st;
            return ASX_OK;
        }
        s->acquired = 1;
        record(s->id);
        g_holders++;
        if (g_holders > g_max_holders) g_max_holders = g_holders;
    }
    st = sleep_once(&s->hold, s->hold_for, self);
    if (st != ASX_OK) return st;
    g_holders--;
    return asx_semaphore_release(s->permit);
}

static sem_task g_sem[4];

static void reset_sem_fixtures(void) {
    memset(g_sem, 0, sizeof(g_sem));
    memset(g_order, 0, sizeof(g_order));
    g_order_len = 0;
    g_holders = 0;
    g_max_holders = 0;
}

/* ===================================================================
 * Semaphore
 * =================================================================== */

TEST(sem_grants_by_arrival_not_slot_index) {
    asx_semaphore_handle h;
    asx_semaphore_permit p;
    asx_semaphore_permit pb;
    asx_semaphore_permit pc;
    asx_semaphore_waiter wa;
    asx_semaphore_waiter wb;
    asx_semaphore_waiter wc;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_semaphore_create(1, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_try_acquire(h, &p), ASX_OK);

    /* wa takes waiter slot 0, wb slot 1; wa leaves and the later wc
     * reuses slot 0. The next permit belongs to wb, the oldest waiter. */
    ASSERT_EQ(asx_semaphore_acquire_begin(h, &wa), ASX_OK);
    ASSERT_EQ(asx_semaphore_acquire_begin(h, &wb), ASX_OK);
    ASSERT_EQ(asx_semaphore_acquire_cancel(&wa), ASX_OK);
    ASSERT_EQ(asx_semaphore_acquire_begin(h, &wc), ASX_OK);
    ASSERT_EQ(wc.waiter_slot, wa.waiter_slot);

    ASSERT_EQ(asx_semaphore_release(p), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&wc, &pc, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_semaphore_poll_acquire(&wb, &pb, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(pb), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&wc, &pc, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(pc), ASX_OK);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
}

TEST(sem_later_arrival_cannot_overtake_at_poll) {
    asx_semaphore_handle h;
    asx_semaphore_permit p1;
    asx_semaphore_permit p2;
    asx_semaphore_waiter w1;
    asx_semaphore_waiter w2;

    ASSERT_TRUE(setup());
    ASSERT_EQ(asx_semaphore_create(1, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_acquire_begin(h, &w1), ASX_OK);
    ASSERT_EQ(asx_semaphore_acquire_begin(h, &w2), ASX_OK);

    /* A permit is free, but w1 arrived first: w2 must wait its turn. */
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w1, &p1, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_E_PENDING);
    ASSERT_EQ(asx_semaphore_release(p1), ASX_OK);
    ASSERT_EQ(asx_semaphore_poll_acquire(&w2, &p2, NULL), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(p2), ASX_OK);
}

TEST(sem_tasks_acquire_in_arrival_order) {
    static const uint64_t arrive[3] = {3u * MS, 1u * MS, 2u * MS};
    asx_semaphore_handle h;
    asx_semaphore_permit held;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_sem_fixtures();
    ASSERT_EQ(asx_semaphore_create(1, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_try_acquire(h, &held), ASX_OK);
    for (i = 0; i < 3u; i++) {
        g_sem[i].id = i;
        g_sem[i].sem = h;
        g_sem[i].arrive_after = arrive[i];
        ASSERT_EQ(asx_task_spawn(g_region, poll_sem_task, &g_sem[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 6u); /* sleep + park, per task */

    ASSERT_EQ(asx_semaphore_release(held), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(polls_used(100, &budget), 3u); /* one grant poll each */
    ASSERT_EQ(g_order_len, 3u);
    ASSERT_EQ(g_order[0], 1u);
    ASSERT_EQ(g_order[1], 2u);
    ASSERT_EQ(g_order[2], 0u);
    ASSERT_EQ(g_max_holders, 1u);
}

TEST(sem_cancel_of_woken_waiter_passes_permit_on) {
    asx_semaphore_handle h;
    asx_semaphore_permit held;
    asx_task_id t[2];
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_sem_fixtures();
    ASSERT_EQ(asx_semaphore_create(1, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_try_acquire(h, &held), ASX_OK);
    for (i = 0; i < 2u; i++) {
        g_sem[i].id = i;
        g_sem[i].sem = h;
        g_sem[i].cancel_aware = 1;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sem_task, &g_sem[i], &t[i]), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The permit is granted to the oldest waiter, which is then cancelled
     * before it runs: cancelling its acquire hands the permit on. */
    ASSERT_EQ(asx_semaphore_release(held), ASX_OK);
    ASSERT_EQ(asx_task_cancel(t[0], ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_TRUE(task_cancelled(t[0]));
    ASSERT_EQ(g_sem[0].acquired, 0);
    ASSERT_EQ(g_sem[1].acquired, 1);
    ASSERT_EQ(g_sem[1].polls, 2u);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
}

TEST(sem_release_skips_cancelled_waiter) {
    asx_semaphore_handle h;
    asx_semaphore_permit held;
    asx_task_id t[2];
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_sem_fixtures();
    ASSERT_EQ(asx_semaphore_create(1, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_try_acquire(h, &held), ASX_OK);
    for (i = 0; i < 2u; i++) {
        g_sem[i].id = i;
        g_sem[i].sem = h;
        g_sem[i].cancel_aware = 1;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sem_task, &g_sem[i], &t[i]), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* Cancel the head of the line first: the release skips it. */
    ASSERT_EQ(asx_task_cancel(t[0], ASX_CANCEL_USER), ASX_OK);
    ASSERT_EQ(asx_semaphore_release(held), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_TRUE(task_cancelled(t[0]));
    ASSERT_EQ(g_sem[1].acquired, 1);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
}

TEST(sem_waiter_of_dead_task_is_reclaimed) {
    asx_semaphore_handle h;
    asx_semaphore_permit held;
    asx_task_id t[2];
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    reset_sem_fixtures();
    ASSERT_EQ(asx_semaphore_create(1, &h), ASX_OK);
    ASSERT_EQ(asx_semaphore_try_acquire(h, &held), ASX_OK);
    for (i = 0; i < 2u; i++) {
        g_sem[i].id = i;
        g_sem[i].sem = h;
        ASSERT_EQ(asx_task_spawn(g_region, poll_sem_task, &g_sem[i], &t[i]), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The oldest waiter's task finishes without cancelling its acquire. */
    g_sem[0].abandon = 1;
    ASSERT_EQ(asx_task_wake(t[0]), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The release reclaims the dead waiter and serves the live one. */
    ASSERT_EQ(asx_semaphore_release(held), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(g_sem[0].acquired, 0);
    ASSERT_EQ(g_sem[1].acquired, 1);
    ASSERT_EQ(asx_semaphore_available(h), 1u);
}

/* ===================================================================
 * Mutex
 * =================================================================== */

typedef struct {
    uint32_t id;
    asx_mutex_handle mutex;
    asx_mutex_lock_waiter waiter;
    asx_mutex_guard guard;
    once_sleep hold;
    int begun;
    int locked;
    uint32_t polls;
} mutex_task;

/* Lock, hold the lock across a 1 ms sleep, unlock. */
static asx_status poll_mutex_task(void *ud, asx_task_id self) {
    mutex_task *s = (mutex_task *)ud;
    asx_status st;
    s->polls++;
    if (!s->begun) {
        st = asx_mutex_lock_begin(s->mutex, &s->waiter);
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    if (!s->locked) {
        st = asx_mutex_poll_lock(&s->waiter, &s->guard, NULL);
        if (st != ASX_OK) return st;
        s->locked = 1;
        record(s->id);
        g_holders++;
        if (g_holders > g_max_holders) g_max_holders = g_holders;
    }
    st = sleep_once(&s->hold, 1u * MS, self);
    if (st != ASX_OK) return st;
    g_holders--;
    return asx_mutex_unlock(s->guard);
}

static mutex_task g_mtx[3];

/* Three tasks queue on a held mutex; returns total polls of the run after
 * the initial unlock (or 0 on setup failure). */
static uint32_t run_mutex_handoff(void) {
    asx_mutex_handle m;
    asx_mutex_guard held;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    if (!setup()) return 0;
    memset(g_mtx, 0, sizeof(g_mtx));
    g_order_len = 0;
    g_holders = 0;
    g_max_holders = 0;
    if (asx_mutex_create(&m) != ASX_OK) return 0;
    if (asx_mutex_try_lock(m, &held) != ASX_OK) return 0;
    for (i = 0; i < 3u; i++) {
        g_mtx[i].id = i;
        g_mtx[i].mutex = m;
        if (asx_task_spawn(g_region, poll_mutex_task, &g_mtx[i], &t) != ASX_OK) return 0;
    }
    budget = asx_budget_from_polls(100);
    if (asx_scheduler_run(g_region, &budget) != ASX_E_WOULD_BLOCK) return 0;
    if (asx_mutex_unlock(held) != ASX_OK) return 0;
    budget = asx_budget_from_polls(100);
    if (asx_scheduler_run(g_region, &budget) != ASX_OK) return 0;
    return polls_used(100, &budget);
}

TEST(mutex_handoff_between_three_tasks) {
    uint32_t polls;
    uint32_t i;

    polls = run_mutex_handoff();
    ASSERT_EQ(polls, 6u); /* per task: acquire + finish after the hold */
    ASSERT_EQ(g_order_len, 3u);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(g_order[i], i);
        ASSERT_EQ(g_mtx[i].polls, 3u); /* park, acquire+hold, unlock */
    }
    ASSERT_EQ(g_max_holders, 1u);
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(3u * MS));
}

TEST(mutex_handoff_is_deterministic) {
    uint32_t order1[3];
    uint32_t polls1;
    uint32_t polls2;
    asx_time end1;

    polls1 = run_mutex_handoff();
    memcpy(order1, g_order, sizeof(order1));
    end1 = asx_runtime_virtual_now();
    polls2 = run_mutex_handoff();
    ASSERT_TRUE(polls1 > 0u);
    ASSERT_EQ(polls1, polls2);
    ASSERT_EQ(memcmp(order1, g_order, sizeof(order1)), 0);
    ASSERT_EQ(end1, asx_runtime_virtual_now());
}

typedef struct {
    asx_contended_mutex_handle mutex;
    asx_mutex_lock_waiter waiter;
    asx_mutex_guard guard;
    int begun;
    uint32_t polls;
} cm_task;

static asx_status poll_cm_task(void *ud, asx_task_id self) {
    cm_task *s = (cm_task *)ud;
    asx_status st;
    (void)self;
    s->polls++;
    if (!s->begun) {
        st = asx_contended_mutex_lock_begin(s->mutex, &s->waiter);
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    st = asx_contended_mutex_poll_lock(s->mutex, &s->waiter, &s->guard, NULL);
    if (st != ASX_OK) return st;
    return asx_contended_mutex_unlock(s->guard);
}

TEST(contended_mutex_counts_one_contention_per_park) {
    cm_task task;
    asx_mutex_guard held;
    asx_lock_metrics_snapshot m;
    asx_task_id t;
    asx_budget budget;

    ASSERT_TRUE(setup());
    memset(&task, 0, sizeof(task));
    ASSERT_EQ(asx_contended_mutex_create(&task.mutex), ASX_OK);
    ASSERT_EQ(asx_contended_mutex_try_lock(task.mutex, &held), ASX_OK);
    ASSERT_EQ(asx_task_spawn(g_region, poll_cm_task, &task, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_contended_mutex_unlock(held), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(task.polls, 2u);
    ASSERT_EQ(asx_contended_mutex_metrics(task.mutex, &m), ASX_OK);
    ASSERT_EQ(m.contentions, (uint64_t)1);
    ASSERT_EQ(m.acquisitions, (uint64_t)2);
}

/* ===================================================================
 * Close wakes every waiter
 * =================================================================== */

typedef struct {
    int kind; /* 0 semaphore, 1 notify, 2 rwlock read, 3 barrier */
    asx_semaphore_handle sem;
    asx_semaphore_waiter sw;
    asx_semaphore_permit sp;
    asx_notify_handle nh;
    asx_notify_waiter nw;
    asx_rwlock_handle rh;
    asx_rwlock_waiter rw;
    asx_rwlock_read_guard rg;
    asx_barrier_handle bh;
    asx_barrier_waiter bw;
    int begun;
    uint32_t polls;
    asx_status result;
} close_task;

static asx_status poll_close_task(void *ud, asx_task_id self) {
    close_task *s = (close_task *)ud;
    asx_status st = ASX_OK;
    (void)self;
    s->polls++;
    if (!s->begun) {
        switch (s->kind) {
        case 0: st = asx_semaphore_acquire_begin(s->sem, &s->sw); break;
        case 1: st = asx_notify_wait_begin(s->nh, &s->nw); break;
        case 2: st = asx_rwlock_read_begin(s->rh, &s->rw); break;
        default: st = asx_barrier_wait_begin(s->bh, &s->bw); break;
        }
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    switch (s->kind) {
    case 0: st = asx_semaphore_poll_acquire(&s->sw, &s->sp, NULL); break;
    case 1: st = asx_notify_poll_wait(&s->nw, NULL); break;
    case 2: st = asx_rwlock_poll_read(&s->rw, &s->rg, NULL); break;
    default: st = asx_barrier_poll_wait(&s->bw, NULL); break;
    }
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    s->result = st;
    return ASX_OK;
}

TEST(close_wakes_all_sync_waiters) {
    static close_task tasks[8];
    asx_semaphore_handle sem;
    asx_notify_handle nh;
    asx_rwlock_handle rh;
    asx_rwlock_write_guard wg;
    asx_barrier_handle bh;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(tasks, 0, sizeof(tasks));
    ASSERT_EQ(asx_semaphore_create(0, &sem), ASX_OK);
    ASSERT_EQ(asx_notify_create(&nh), ASX_OK);
    ASSERT_EQ(asx_rwlock_create(&rh), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(rh, &wg), ASX_OK);
    ASSERT_EQ(asx_barrier_create(5, &bh), ASX_OK);
    for (i = 0; i < 8u; i++) {
        tasks[i].kind = (int)(i / 2u);
        tasks[i].sem = sem;
        tasks[i].nh = nh;
        tasks[i].rh = rh;
        tasks[i].bh = bh;
        ASSERT_EQ(asx_task_spawn(g_region, poll_close_task, &tasks[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 8u);

    ASSERT_EQ(asx_semaphore_close(sem), ASX_OK);
    ASSERT_EQ(asx_notify_close(nh), ASX_OK);
    ASSERT_EQ(asx_rwlock_close(rh), ASX_OK);
    ASSERT_EQ(asx_barrier_close(bh), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(polls_used(100, &budget), 8u);
    for (i = 0; i < 8u; i++) {
        ASSERT_EQ(tasks[i].polls, 2u);
        ASSERT_EQ(tasks[i].result, ASX_E_DISCONNECTED);
    }
}

/* ===================================================================
 * Notify
 * =================================================================== */

typedef struct {
    asx_notify_handle h;
    asx_notify_waiter w;
    int begun;
    int give_up;
    int notified;
    uint32_t polls;
} notify_task;

static asx_status poll_notify_task(void *ud, asx_task_id self) {
    notify_task *s = (notify_task *)ud;
    asx_status st;
    (void)self;
    s->polls++;
    if (!s->begun) {
        st = asx_notify_wait_begin(s->h, &s->w);
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    if (s->give_up) return asx_notify_wait_cancel(&s->w);
    st = asx_notify_poll_wait(&s->w, NULL);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    if (st != ASX_OK) return st;
    s->notified = 1;
    return ASX_OK;
}

TEST(notify_one_wakes_oldest_and_passes_on_when_abandoned) {
    notify_task n[3];
    asx_notify_handle h;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(n, 0, sizeof(n));
    ASSERT_EQ(asx_notify_create(&h), ASX_OK);
    for (i = 0; i < 3u; i++) {
        n[i].h = h;
        ASSERT_EQ(asx_task_spawn(g_region, poll_notify_task, &n[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* notify_one reaches the oldest waiter only. */
    ASSERT_EQ(asx_notify_one(h), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 1u);
    ASSERT_EQ(n[0].notified, 1);

    /* The next notification goes to n[1], which abandons the wait: the
     * notification passes on to n[2]. */
    n[1].give_up = 1;
    ASSERT_EQ(asx_notify_one(h), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(n[1].notified, 0);
    ASSERT_EQ(n[2].notified, 1);
    ASSERT_EQ(n[2].polls, 2u);
    ASSERT_EQ(asx_notify_waiter_count(h), 0u);
}

/* ===================================================================
 * RwLock
 * =================================================================== */

typedef struct {
    uint32_t id;
    int is_write;
    asx_rwlock_handle h;
    asx_rwlock_waiter w;
    asx_rwlock_read_guard rg;
    asx_rwlock_write_guard wg;
    once_sleep hold;
    int begun;
    int locked;
    uint32_t polls;
} rw_task;

/* Lock (read or write), hold across a 1 ms sleep, unlock. */
static asx_status poll_rw_task(void *ud, asx_task_id self) {
    rw_task *s = (rw_task *)ud;
    asx_status st;
    s->polls++;
    if (!s->begun) {
        st = s->is_write ? asx_rwlock_write_begin(s->h, &s->w) : asx_rwlock_read_begin(s->h, &s->w);
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    if (!s->locked) {
        st = s->is_write ? asx_rwlock_poll_write(&s->w, &s->wg, NULL)
                         : asx_rwlock_poll_read(&s->w, &s->rg, NULL);
        if (st != ASX_OK) return st;
        s->locked = 1;
        record(s->id);
    }
    st = sleep_once(&s->hold, 1u * MS, self);
    if (st != ASX_OK) return st;
    return s->is_write ? asx_rwlock_write_unlock(s->wg) : asx_rwlock_read_unlock(s->rg);
}

TEST(rwlock_writer_preferred_then_readers_together) {
    rw_task r[4];
    asx_rwlock_handle h;
    asx_rwlock_write_guard held;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(r, 0, sizeof(r));
    g_order_len = 0;
    ASSERT_EQ(asx_rwlock_create(&h), ASX_OK);
    ASSERT_EQ(asx_rwlock_try_write(h, &held), ASX_OK);
    /* Arena order: reader, writer, reader, writer. */
    for (i = 0; i < 4u; i++) {
        r[i].id = i;
        r[i].h = h;
        r[i].is_write = (int)(i % 2u);
        ASSERT_EQ(asx_task_spawn(g_region, poll_rw_task, &r[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 4u);

    ASSERT_EQ(asx_rwlock_write_unlock(held), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);

    /* Writers first, in arrival order, then both readers at once. */
    ASSERT_EQ(g_order_len, 4u);
    ASSERT_EQ(g_order[0], 1u);
    ASSERT_EQ(g_order[1], 3u);
    ASSERT_EQ(g_order[2], 0u);
    ASSERT_EQ(g_order[3], 2u);
    for (i = 0; i < 4u; i++) ASSERT_EQ(r[i].polls, 3u);
    /* 1 ms per writer, readers overlap: 3 ms of virtual time in total. */
    ASSERT_EQ(asx_runtime_virtual_now(), (asx_time)(3u * MS));
}

/* ===================================================================
 * Barrier
 * =================================================================== */

typedef struct {
    asx_barrier_handle h;
    asx_barrier_waiter w;
    uint64_t arrive_after;
    once_sleep arrive;
    int begun;
    int released;
    uint32_t polls;
} barrier_task;

static asx_status poll_barrier_task(void *ud, asx_task_id self) {
    barrier_task *s = (barrier_task *)ud;
    asx_status st;
    s->polls++;
    st = sleep_once(&s->arrive, s->arrive_after, self);
    if (st != ASX_OK) return st;
    if (!s->begun) {
        st = asx_barrier_wait_begin(s->h, &s->w);
        if (st != ASX_OK) return st;
        s->begun = 1;
    }
    st = asx_barrier_poll_wait(&s->w, NULL);
    if (st != ASX_OK) return st;
    s->released = 1;
    return ASX_OK;
}

TEST(barrier_parks_until_last_arrival) {
    barrier_task b[3];
    asx_barrier_handle h;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(b, 0, sizeof(b));
    ASSERT_EQ(asx_barrier_create(3, &h), ASX_OK);
    for (i = 0; i < 3u; i++) {
        b[i].h = h;
        b[i].arrive_after = (i == 1u) ? 5u * MS : 0u;
        ASSERT_EQ(asx_task_spawn(g_region, poll_barrier_task, &b[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(b[i].released, 1);
        ASSERT_EQ(b[i].polls, 2u); /* park once, released once */
    }
    ASSERT_TRUE(b[1].w.is_leader);
    ASSERT_EQ(polls_used(100, &budget), 6u);
}

/* ===================================================================
 * Once (asynchronous initializer)
 * =================================================================== */

static uint32_t g_init_calls;

/* Initializer that waits for a oneshot value. */
static asx_status init_from_oneshot(void *user_data, uint64_t *out_value) {
    asx_oneshot_receiver *rx = (asx_oneshot_receiver *)user_data;
    asx_status st;
    g_init_calls++;
    st = asx_oneshot_try_recv(rx, out_value);
    if (st == ASX_E_WOULD_BLOCK) return ASX_E_PENDING;
    return st;
}

typedef struct {
    asx_once_handle h;
    asx_oneshot_receiver *rx;
    uint64_t value;
    uint32_t polls;
    asx_status result;
} once_task;

static asx_status poll_once_task(void *ud, asx_task_id self) {
    once_task *s = (once_task *)ud;
    asx_status st;
    (void)self;
    s->polls++;
    st = asx_once_get_or_init(s->h, init_from_oneshot, s->rx, &s->value);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    s->result = st;
    return ASX_OK;
}

TEST(once_async_init_parks_other_callers) {
    once_task o[3];
    asx_once_handle h;
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(o, 0, sizeof(o));
    g_init_calls = 0;
    ASSERT_EQ(asx_once_create(&h), ASX_OK);
    ASSERT_EQ(asx_oneshot_create(&tx, &rx), ASX_OK);
    for (i = 0; i < 3u; i++) {
        o[i].h = h;
        o[i].rx = &rx;
        ASSERT_EQ(asx_task_spawn(g_region, poll_once_task, &o[i], &t), ASX_OK);
    }

    /* o[0] starts the initializer (parked on the oneshot); the others
     * park behind it without running init_fn. */
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(g_init_calls, 1u);
    ASSERT_FALSE(asx_once_is_initialized(h));

    ASSERT_EQ(asx_oneshot_try_send(&tx, 42u), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(g_init_calls, 2u);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(o[i].result, ASX_OK);
        ASSERT_EQ(o[i].value, (uint64_t)42);
        ASSERT_EQ(o[i].polls, 2u);
    }
}

TEST(once_abandoned_init_passes_turn_to_next_waiter) {
    once_task o[2];
    asx_once_handle h;
    asx_oneshot_sender tx;
    asx_oneshot_receiver rx;
    asx_task_id t[2];
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(o, 0, sizeof(o));
    g_init_calls = 0;
    ASSERT_EQ(asx_once_create(&h), ASX_OK);
    ASSERT_EQ(asx_oneshot_create(&tx, &rx), ASX_OK);
    for (i = 0; i < 2u; i++) {
        o[i].h = h;
        o[i].rx = &rx;
        ASSERT_EQ(asx_task_spawn(g_region, poll_once_task, &o[i], &t[i]), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(g_init_calls, 1u);

    /* The initializer abandons: the parked waiter takes over at once. */
    ASSERT_EQ(asx_once_wait_cancel(h, t[0]), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(polls_used(100, &budget), 1u);
    ASSERT_EQ(g_init_calls, 2u);

    /* The value arrives: the new initializer completes, the old one (now a
     * plain caller) gets the cached value. */
    ASSERT_EQ(asx_oneshot_try_send(&tx, 9u), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(o[i].result, ASX_OK);
        ASSERT_EQ(o[i].value, (uint64_t)9);
    }
    ASSERT_EQ(g_init_calls, 3u);
}

/* ===================================================================
 * Pool
 * =================================================================== */

static int g_pool_resource;

static asx_status pool_create_dummy(void *factory_data, void **out) {
    (void)factory_data;
    *out = &g_pool_resource;
    return ASX_OK;
}

typedef struct {
    uint32_t id;
    asx_pool_handle h;
    asx_pooled_resource res;
    uint64_t arrive_after;
    once_sleep arrive;
    once_sleep hold;
    int acquired;
    uint32_t polls;
    asx_status result;
} pool_task;

/* Arrive, acquire, hold for 5 ms, return. */
static asx_status poll_pool_task(void *ud, asx_task_id self) {
    pool_task *s = (pool_task *)ud;
    asx_status st;
    s->polls++;
    st = sleep_once(&s->arrive, s->arrive_after, self);
    if (st != ASX_OK) return st;
    if (!s->acquired) {
        st = asx_pool_try_acquire(s->h, &s->res);
        if (st == ASX_E_WOULD_BLOCK) return ASX_E_PENDING;
        if (st != ASX_OK) {
            s->result = st;
            return ASX_OK;
        }
        s->acquired = 1;
        record(s->id);
    }
    st = sleep_once(&s->hold, 5u * MS, self);
    if (st != ASX_OK) return st;
    return asx_pool_return(&s->res);
}

TEST(pool_return_wakes_oldest_waiter) {
    static const uint64_t arrive[3] = {0u, 2u * MS, 1u * MS};
    pool_task p[3];
    asx_pool_config cfg;
    asx_pool_handle h;
    asx_pool_stats stats;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(p, 0, sizeof(p));
    memset(&cfg, 0, sizeof(cfg));
    g_order_len = 0;
    cfg.max_size = 1;
    cfg.create_fn = pool_create_dummy;
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);
    for (i = 0; i < 3u; i++) {
        p[i].id = i;
        p[i].h = h;
        p[i].arrive_after = arrive[i];
        ASSERT_EQ(asx_task_spawn(g_region, poll_pool_task, &p[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);

    /* p[2] arrived (1 ms) before p[1] (2 ms), so it is served first. */
    ASSERT_EQ(g_order_len, 3u);
    ASSERT_EQ(g_order[0], 0u);
    ASSERT_EQ(g_order[1], 2u);
    ASSERT_EQ(g_order[2], 1u);
    ASSERT_EQ(p[0].polls, 2u);
    ASSERT_EQ(p[1].polls, 4u); /* sleep, park, acquire+hold, return */
    ASSERT_EQ(p[2].polls, 4u);
    ASSERT_EQ(asx_pool_get_stats(h, &stats), ASX_OK);
    ASSERT_EQ(stats.waiters, 0u);
    ASSERT_EQ(stats.total_acquisitions, (uint64_t)3);
    ASSERT_EQ(stats.total_creates, (uint64_t)1);
}

TEST(pool_newcomer_cannot_jump_parked_waiter) {
    pool_task p;
    asx_pool_config cfg;
    asx_pool_handle h;
    asx_pooled_resource held;
    asx_pooled_resource other;
    asx_task_id t;
    asx_budget budget;

    ASSERT_TRUE(setup());
    memset(&p, 0, sizeof(p));
    memset(&cfg, 0, sizeof(cfg));
    g_order_len = 0;
    cfg.max_size = 1;
    cfg.create_fn = pool_create_dummy;
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &held), ASX_OK);
    p.h = h;
    ASSERT_EQ(asx_task_spawn(g_region, poll_pool_task, &p, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);

    /* The returned resource belongs to the parked task. */
    ASSERT_EQ(asx_pool_return(&held), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &other), ASX_E_WOULD_BLOCK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    ASSERT_EQ(p.acquired, 1);
    ASSERT_EQ(asx_pool_try_acquire(h, &other), ASX_OK);
    ASSERT_EQ(asx_pool_return(&other), ASX_OK);
}

TEST(pool_close_wakes_waiters) {
    pool_task p[2];
    asx_pool_config cfg;
    asx_pool_handle h;
    asx_pooled_resource held;
    asx_pool_stats stats;
    asx_task_id t;
    asx_budget budget;
    uint32_t i;

    ASSERT_TRUE(setup());
    memset(p, 0, sizeof(p));
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_size = 1;
    cfg.create_fn = pool_create_dummy;
    ASSERT_EQ(asx_pool_create(&cfg, &h), ASX_OK);
    ASSERT_EQ(asx_pool_try_acquire(h, &held), ASX_OK);
    for (i = 0; i < 2u; i++) {
        p[i].h = h;
        ASSERT_EQ(asx_task_spawn(g_region, poll_pool_task, &p[i], &t), ASX_OK);
    }
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_pool_get_stats(h, &stats), ASX_OK);
    ASSERT_EQ(stats.waiters, 2u);

    ASSERT_EQ(asx_pool_close(h), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(g_region, &budget), ASX_OK);
    for (i = 0; i < 2u; i++) {
        ASSERT_EQ(p[i].polls, 2u);
        ASSERT_EQ(p[i].acquired, 0);
        ASSERT_EQ(p[i].result, ASX_E_DISCONNECTED);
    }
    ASSERT_EQ(asx_pool_return(&held), ASX_OK);
}

TEST(pool_wait_cancel_rejects_stale_handle) {
    asx_pool_handle h;
    ASSERT_TRUE(setup());
    h.slot = 0;
    h.generation = 0;
    ASSERT_EQ(asx_pool_wait_cancel(h, ASX_INVALID_ID), ASX_E_STALE_HANDLE);
}

int main(void) {
    fprintf(stderr, "=== test_sync_wake ===\n");

    RUN_TEST(sem_grants_by_arrival_not_slot_index);
    RUN_TEST(sem_later_arrival_cannot_overtake_at_poll);
    RUN_TEST(sem_tasks_acquire_in_arrival_order);
    RUN_TEST(sem_cancel_of_woken_waiter_passes_permit_on);
    RUN_TEST(sem_release_skips_cancelled_waiter);
    RUN_TEST(sem_waiter_of_dead_task_is_reclaimed);
    RUN_TEST(mutex_handoff_between_three_tasks);
    RUN_TEST(mutex_handoff_is_deterministic);
    RUN_TEST(contended_mutex_counts_one_contention_per_park);
    RUN_TEST(close_wakes_all_sync_waiters);
    RUN_TEST(notify_one_wakes_oldest_and_passes_on_when_abandoned);
    RUN_TEST(rwlock_writer_preferred_then_readers_together);
    RUN_TEST(barrier_parks_until_last_arrival);
    RUN_TEST(once_async_init_parks_other_callers);
    RUN_TEST(once_abandoned_init_passes_turn_to_next_waiter);
    RUN_TEST(pool_return_wakes_oldest_waiter);
    RUN_TEST(pool_newcomer_cannot_jump_parked_waiter);
    RUN_TEST(pool_close_wakes_waiters);
    RUN_TEST(pool_wait_cancel_rejects_stale_handle);

    TEST_REPORT();
    return test_failures;
}
