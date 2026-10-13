/*
 * test_delay_queue.c — unit tests for the DelayQueue and the pure interval tick
 *
 * The vectors follow Rust's time/delay_queue.rs and time/interval.rs: entries
 * come out earliest deadline first with ties in insertion order, a reset is a
 * new insertion, a key dies with its entry, and an interval tick advances by
 * its missed-tick behavior.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/virtual_time.h>
#include <asx/time/sleep.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* The queue is large (it carries its own arrays): one shared instance. */
static asx_delay_queue g_q;

static void fresh(void) { MUST_OK(asx_delay_queue_init(&g_q)); }

/* Poll at `now`; returns the state and fills *out. */
static asx_delay_poll_state poll_at(asx_time now, asx_delay_expired *out) {
    asx_delay_poll_state state = ASX_DELAY_EMPTY;
    MUST_OK(asx_delay_queue_poll_at(&g_q, now, &state, out));
    return state;
}

/* The values yielded at `now`, in order, until nothing more is due. */
static uint32_t drain(asx_time now, uint64_t *values, uint32_t max) {
    uint32_t n = 0;
    asx_delay_expired e;
    while (n < max && poll_at(now, &e) == ASX_DELAY_EXPIRED) values[n++] = e.value;
    return n;
}

/* ------------------------------------------------------------------ */
/* Ordering                                                            */
/* ------------------------------------------------------------------ */

TEST(empty_queue_polls_empty) {
    asx_delay_expired e;
    fresh();
    ASSERT_EQ(poll_at(100, &e), ASX_DELAY_EMPTY);
    ASSERT_EQ(asx_delay_queue_len(&g_q), 0u);
}

TEST(entries_come_out_earliest_deadline_first) {
    asx_delay_key k;
    uint64_t got[4];
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 30, &k));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 10, &k));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 3, 20, &k));
    ASSERT_EQ(drain(100, got, 4), 3u);
    ASSERT_EQ(got[0], (uint64_t)2);
    ASSERT_EQ(got[1], (uint64_t)3);
    ASSERT_EQ(got[2], (uint64_t)1);
}

TEST(equal_deadlines_come_out_in_insertion_order) {
    asx_delay_key k;
    uint64_t got[4];
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 7, 10, &k));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 8, 10, &k));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 9, 10, &k));
    ASSERT_EQ(drain(10, got, 4), 3u);
    ASSERT_EQ(got[0], (uint64_t)7);
    ASSERT_EQ(got[1], (uint64_t)8);
    ASSERT_EQ(got[2], (uint64_t)9);
}

TEST(an_entry_waits_until_its_deadline_and_a_deadline_equal_to_now_is_due) {
    asx_delay_key k;
    asx_delay_expired e;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 5, 50, &k));
    ASSERT_EQ(poll_at(49, &e), ASX_DELAY_WAITING);
    ASSERT_EQ(e.deadline, (asx_time)50);
    ASSERT_EQ(asx_delay_queue_len(&g_q), 1u); /* nothing was taken */
    ASSERT_EQ(poll_at(50, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)5);
    ASSERT_EQ(e.deadline, (asx_time)50);
    ASSERT_EQ(e.key.index, k.index);
    ASSERT_EQ(e.key.generation, k.generation);
    ASSERT_EQ(asx_delay_queue_len(&g_q), 0u);
    ASSERT_EQ(poll_at(50, &e), ASX_DELAY_EMPTY);
}

TEST(a_late_poll_reports_each_entry_with_its_own_deadline) {
    asx_delay_key k;
    asx_delay_expired e;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 10, &k));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 20, &k));
    ASSERT_EQ(poll_at(1000, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.deadline, (asx_time)10);
    ASSERT_EQ(poll_at(1000, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.deadline, (asx_time)20);
}

/* ------------------------------------------------------------------ */
/* insert / reset / remove                                             */
/* ------------------------------------------------------------------ */

TEST(insert_is_due_a_timeout_after_now_and_saturates) {
    asx_delay_key k;
    asx_time d = 0;
    fresh();
    MUST_OK(asx_delay_queue_insert(&g_q, 1000, 7, 250, &k));
    MUST_OK(asx_delay_queue_deadline(&g_q, &k, &d));
    ASSERT_EQ(d, (asx_time)1250);
    MUST_OK(asx_delay_queue_insert(&g_q, UINT64_MAX - 5u, 8, 100, &k));
    MUST_OK(asx_delay_queue_deadline(&g_q, &k, &d));
    ASSERT_EQ(d, (asx_time)UINT64_MAX);
}

TEST(reset_reorders_the_queue_the_rust_doc_example) {
    asx_delay_key later;
    asx_delay_key soon;
    asx_delay_key dropped;
    asx_delay_expired e;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 30, &later));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 10, &soon));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 3, 20, &dropped));
    MUST_OK(asx_delay_queue_reset_at(&g_q, &soon, 5));
    ASSERT_EQ(poll_at(100, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)2);
    ASSERT_EQ(e.deadline, (asx_time)5);
    ASSERT_EQ(poll_at(100, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)3);
    ASSERT_EQ(asx_delay_queue_len(&g_q), 1u);
    ASSERT_TRUE(asx_delay_queue_contains(&g_q, &later));
    ASSERT_FALSE(asx_delay_queue_contains(&g_q, &soon));
}

TEST(reset_keeps_the_key_and_the_old_deadline_never_fires) {
    asx_delay_key k;
    asx_delay_expired e;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 4, 10, &k));
    MUST_OK(asx_delay_queue_reset_at(&g_q, &k, 90));
    ASSERT_TRUE(asx_delay_queue_contains(&g_q, &k));
    ASSERT_EQ(poll_at(50, &e), ASX_DELAY_WAITING); /* not at its first deadline */
    ASSERT_EQ(e.deadline, (asx_time)90);
    ASSERT_EQ(poll_at(90, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)4);
    ASSERT_EQ(poll_at(1000, &e), ASX_DELAY_EMPTY); /* no second, stale expiry */
}

TEST(a_reset_orders_after_entries_already_due_at_the_same_time) {
    asx_delay_key x;
    asx_delay_key y;
    uint64_t got[3];
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 10, &x));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 10, &y));
    MUST_OK(asx_delay_queue_reset_at(&g_q, &x, 10)); /* the same deadline, a new schedule */
    ASSERT_EQ(drain(10, got, 3), 2u);
    ASSERT_EQ(got[0], (uint64_t)2);
    ASSERT_EQ(got[1], (uint64_t)1);
}

TEST(reset_after_now_saturates) {
    asx_delay_key k;
    asx_time d = 0;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 10, &k));
    MUST_OK(asx_delay_queue_reset(&g_q, &k, 500, 40));
    MUST_OK(asx_delay_queue_deadline(&g_q, &k, &d));
    ASSERT_EQ(d, (asx_time)540);
    MUST_OK(asx_delay_queue_reset(&g_q, &k, UINT64_MAX, 40));
    MUST_OK(asx_delay_queue_deadline(&g_q, &k, &d));
    ASSERT_EQ(d, (asx_time)UINT64_MAX);
}

TEST(remove_returns_the_entry_and_it_is_never_yielded) {
    asx_delay_key a;
    asx_delay_key b;
    asx_delay_expired gone;
    asx_delay_expired e;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 10, &a));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 20, &b));
    MUST_OK(asx_delay_queue_remove(&g_q, &a, &gone));
    ASSERT_EQ(gone.value, (uint64_t)1);
    ASSERT_EQ(gone.deadline, (asx_time)10);
    ASSERT_FALSE(asx_delay_queue_contains(&g_q, &a));
    ASSERT_EQ(asx_delay_queue_len(&g_q), 1u);
    ASSERT_EQ(asx_delay_queue_remove(&g_q, &a, &gone), ASX_E_NOT_FOUND); /* Rust: None */
    ASSERT_EQ(poll_at(100, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)2);
    ASSERT_EQ(poll_at(100, &e), ASX_DELAY_EMPTY);
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

TEST(a_key_dies_with_its_entry_even_when_the_slot_is_reused) {
    asx_delay_key old;
    asx_delay_key fresh_key;
    asx_delay_expired e;
    asx_time d = 0;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 10, &old));
    ASSERT_EQ(poll_at(10, &e), ASX_DELAY_EXPIRED);
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 20, &fresh_key));
    ASSERT_EQ(fresh_key.index, old.index);               /* the freed slot is reused ... */
    ASSERT_TRUE(fresh_key.generation != old.generation); /* ... under a new identity */
    ASSERT_FALSE(asx_delay_queue_contains(&g_q, &old));
    ASSERT_EQ(asx_delay_queue_deadline(&g_q, &old, &d), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_delay_queue_reset_at(&g_q, &old, 99), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_delay_queue_remove(&g_q, &old, &e), ASX_E_NOT_FOUND);
    ASSERT_TRUE(asx_delay_queue_contains(&g_q, &fresh_key));
    ASSERT_EQ(asx_delay_queue_len(&g_q), 1u);
}

TEST(clear_empties_the_queue_and_invalidates_every_key) {
    asx_delay_key a;
    asx_delay_key b;
    asx_delay_expired e;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 10, &a));
    MUST_OK(asx_delay_queue_insert_at(&g_q, 2, 20, &b));
    MUST_OK(asx_delay_queue_clear(&g_q));
    ASSERT_EQ(asx_delay_queue_len(&g_q), 0u);
    ASSERT_FALSE(asx_delay_queue_contains(&g_q, &a));
    ASSERT_EQ(poll_at(1000, &e), ASX_DELAY_EMPTY);
    MUST_OK(asx_delay_queue_insert_at(&g_q, 3, 30, &b));
    ASSERT_FALSE(asx_delay_queue_contains(&g_q, &a)); /* still not valid */
    ASSERT_EQ(poll_at(1000, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)3);
}

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

TEST(a_full_queue_refuses_an_insert_and_changes_nothing) {
    asx_delay_key k;
    asx_delay_key extra;
    asx_delay_expired e;
    uint32_t i;
    fresh();
    for (i = 0; i < ASX_DELAY_QUEUE_CAPACITY; i++) {
        MUST_OK(asx_delay_queue_insert_at(&g_q, i, 100u + i, &k));
    }
    ASSERT_EQ(asx_delay_queue_insert_at(&g_q, 99, 1, &extra), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_delay_queue_len(&g_q), ASX_DELAY_QUEUE_CAPACITY);
    ASSERT_EQ(poll_at(1000, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)0);                         /* the earliest of the sixteen */
    MUST_OK(asx_delay_queue_insert_at(&g_q, 99, 1, &extra)); /* room again */
}

TEST(many_resets_keep_the_heap_proportional_to_the_entries) {
    asx_delay_key k;
    asx_delay_expired e;
    uint32_t i;
    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 1, 1000, &k));
    for (i = 0; i < 500u; i++) {
        MUST_OK(asx_delay_queue_reset_at(&g_q, &k, 2000u + i));
        ASSERT_TRUE(g_q.heap_len <= 2u * g_q.len + 64u);
    }
    ASSERT_EQ(poll_at(2499, &e), ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.deadline, (asx_time)2499);
    ASSERT_EQ(poll_at(100000, &e), ASX_DELAY_EMPTY);
}

TEST(argument_errors) {
    asx_delay_key k;
    asx_delay_expired e;
    asx_delay_poll_state s = ASX_DELAY_EMPTY;
    asx_time d = 0;
    fresh();
    memset(&k, 0, sizeof(k));
    ASSERT_EQ(asx_delay_queue_init(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_insert_at(NULL, 1, 1, &k), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_insert_at(&g_q, 1, 1, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_remove(&g_q, NULL, &e), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_reset_at(&g_q, NULL, 1), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_deadline(&g_q, &k, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_poll_at(NULL, 1, &s, &e), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_poll_at(&g_q, 1, NULL, &e), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_delay_queue_clear(NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_FALSE(asx_delay_queue_contains(NULL, &k));
    ASSERT_EQ(asx_delay_queue_len(NULL), 0u);
    ASSERT_EQ(asx_delay_queue_deadline(&g_q, &k, &d), ASX_E_NOT_FOUND); /* nothing inserted */
}

/* ------------------------------------------------------------------ */
/* Task-level poll on the runtime clock                                 */
/* ------------------------------------------------------------------ */

static asx_runtime g_rt;
static asx_vtime_state g_vt;

TEST(task_poll_waits_for_the_deadline_on_the_runtime_clock) {
    const asx_runtime_hooks *orig;
    asx_runtime_hooks hooks;
    asx_delay_key k;
    asx_delay_expired e;
    asx_delay_poll_state s = ASX_DELAY_EMPTY;
    asx_status st = ASX_E_PENDING;
    int polls = 0;

    MUST_OK(asx_runtime_init_default(&g_rt));
    asx_vtime_init(&g_vt, 0, 1000000ULL); /* 1 ms per clock query */
    orig = asx_runtime_get_hooks();
    memcpy(&hooks, orig, sizeof(hooks));
    hooks.clock.now_ns_fn = asx_vtime_now_ns;
    hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
    hooks.clock.ctx = &g_vt;
    MUST_OK(asx_runtime_set_hooks(&hooks));

    fresh();
    MUST_OK(asx_delay_queue_insert_at(&g_q, 42, 5000000ULL, &k));
    while (st == ASX_E_PENDING && polls < 50) {
        st = asx_delay_queue_poll(&g_q, ASX_INVALID_ID, &s, &e);
        polls++;
    }
    ASSERT_EQ(st, ASX_OK);
    ASSERT_EQ(s, ASX_DELAY_EXPIRED);
    ASSERT_EQ(e.value, (uint64_t)42);
    ASSERT_TRUE(polls > 1); /* it waited */
    ASSERT_EQ(asx_delay_queue_poll(&g_q, ASX_INVALID_ID, &s, &e), ASX_OK);
    ASSERT_EQ(s, ASX_DELAY_EMPTY);
    asx_runtime_shutdown(&g_rt);
}

/* ------------------------------------------------------------------ */
/* Interval::tick as a pure state machine (interval.rs:261-303, :385)   */
/* ------------------------------------------------------------------ */

static asx_time tick(asx_interval_state *s, asx_time now, int *due) {
    asx_time t = 0;
    int d = 0;
    MUST_OK(asx_interval_tick_at(s, now, &t, &d));
    if (due != NULL) *due = d;
    return t;
}

TEST(interval_ticks_at_each_period_from_its_start) {
    asx_interval_state s;
    int due = 0;
    MUST_OK(asx_interval_init_at(&s, 0, 100, 0));
    ASSERT_EQ(tick(&s, 0, &due), (asx_time)0);
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 50, &due), (asx_time)100); /* too early: the deadline, not advanced */
    ASSERT_FALSE(due);
    ASSERT_EQ(tick(&s, 100, &due), (asx_time)100);
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 200, &due), (asx_time)200);
    ASSERT_TRUE(due);
    ASSERT_EQ(asx_interval_ticks(&s), 3u);
}

TEST(interval_first_tick_is_at_now_without_a_start) {
    asx_interval_state s;
    int due = 0;
    MUST_OK(asx_interval_init(&s, 100, 0));
    ASSERT_EQ(tick(&s, 700, &due), (asx_time)700);
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 700, &due), (asx_time)800);
    ASSERT_FALSE(due);
}

TEST(burst_catches_up_one_missed_tick_per_call) {
    asx_interval_state s;
    int due = 0;
    MUST_OK(asx_interval_init_at(&s, 0, 100, 0));
    /* Polled at 350: ticks 0, 100, 200, 300 are due, each returned at once. */
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)0);
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)100);
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)200);
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)300);
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)400); /* caught up */
    ASSERT_FALSE(due);
}

TEST(delay_restarts_the_period_from_the_tick) {
    asx_interval_state s;
    int due = 0;
    MUST_OK(asx_interval_init_at(&s, 0, 100, 0));
    MUST_OK(asx_interval_set_missed_tick_behavior(&s, ASX_MISSED_TICK_DELAY));
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)0); /* the next deadline is 350 + 100 */
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 400, &due), (asx_time)450);
    ASSERT_FALSE(due);
    ASSERT_EQ(tick(&s, 450, &due), (asx_time)450);
    ASSERT_TRUE(due);
}

TEST(skip_jumps_to_the_next_aligned_tick_after_now) {
    asx_interval_state s;
    int due = 0;
    MUST_OK(asx_interval_init_at(&s, 0, 100, 0));
    MUST_OK(asx_interval_set_missed_tick_behavior(&s, ASX_MISSED_TICK_SKIP));
    ASSERT_EQ(tick(&s, 350, &due), (asx_time)0); /* the next deadline is 400 */
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 399, &due), (asx_time)400);
    ASSERT_FALSE(due);
    ASSERT_EQ(tick(&s, 400, &due), (asx_time)400);
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, 400, &due), (asx_time)500); /* on the boundary: one period on */
    ASSERT_FALSE(due);
}

TEST(a_tick_at_the_last_representable_time_exhausts_the_interval) {
    asx_interval_state s;
    int due = 0;
    MUST_OK(asx_interval_init_at(&s, UINT64_MAX, 100, 0));
    ASSERT_EQ(tick(&s, UINT64_MAX, &due), (asx_time)UINT64_MAX);
    ASSERT_TRUE(due);
    ASSERT_EQ(tick(&s, UINT64_MAX, &due), (asx_time)UINT64_MAX);
    ASSERT_FALSE(due); /* silent from here on */
}

TEST(interval_tick_argument_errors) {
    asx_interval_state s;
    asx_time t = 0;
    int due = 0;
    MUST_OK(asx_interval_init_at(&s, 0, 100, 0));
    ASSERT_EQ(asx_interval_tick_at(NULL, 0, &t, &due), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_interval_tick_at(&s, 0, NULL, &due), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_interval_tick_at(&s, 0, &t, NULL), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_delay_queue ===\n");

    RUN_TEST(empty_queue_polls_empty);
    RUN_TEST(entries_come_out_earliest_deadline_first);
    RUN_TEST(equal_deadlines_come_out_in_insertion_order);
    RUN_TEST(an_entry_waits_until_its_deadline_and_a_deadline_equal_to_now_is_due);
    RUN_TEST(a_late_poll_reports_each_entry_with_its_own_deadline);

    RUN_TEST(insert_is_due_a_timeout_after_now_and_saturates);
    RUN_TEST(reset_reorders_the_queue_the_rust_doc_example);
    RUN_TEST(reset_keeps_the_key_and_the_old_deadline_never_fires);
    RUN_TEST(a_reset_orders_after_entries_already_due_at_the_same_time);
    RUN_TEST(reset_after_now_saturates);
    RUN_TEST(remove_returns_the_entry_and_it_is_never_yielded);

    RUN_TEST(a_key_dies_with_its_entry_even_when_the_slot_is_reused);
    RUN_TEST(clear_empties_the_queue_and_invalidates_every_key);

    RUN_TEST(a_full_queue_refuses_an_insert_and_changes_nothing);
    RUN_TEST(many_resets_keep_the_heap_proportional_to_the_entries);
    RUN_TEST(argument_errors);

    RUN_TEST(task_poll_waits_for_the_deadline_on_the_runtime_clock);

    RUN_TEST(interval_ticks_at_each_period_from_its_start);
    RUN_TEST(interval_first_tick_is_at_now_without_a_start);
    RUN_TEST(burst_catches_up_one_missed_tick_per_call);
    RUN_TEST(delay_restarts_the_period_from_the_tick);
    RUN_TEST(skip_jumps_to_the_next_aligned_tick_after_now);
    RUN_TEST(a_tick_at_the_last_representable_time_exhausts_the_interval);
    RUN_TEST(interval_tick_argument_errors);

    TEST_REPORT();
    return test_failures;
}
