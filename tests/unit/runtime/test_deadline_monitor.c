/*
 * test_deadline_monitor.c — unit tests for runtime deadline monitor
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/deadline_monitor.h>
#include <string.h>

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

static void setup(void) {
    asx_deadline_monitor_reset();
    MUST_OK(asx_deadline_monitor_init());
}

static void teardown(void) { asx_deadline_monitor_shutdown(); }

/* ------------------------------------------------------------------ */
/* Lifecycle tests                                                     */
/* ------------------------------------------------------------------ */

TEST(init_shutdown) {
    asx_deadline_monitor_reset();
    ASSERT_FALSE(asx_deadline_monitor_is_initialized());
    ASSERT_EQ(asx_deadline_monitor_init(), ASX_OK);
    ASSERT_TRUE(asx_deadline_monitor_is_initialized());
    asx_deadline_monitor_shutdown();
    ASSERT_FALSE(asx_deadline_monitor_is_initialized());
}

TEST(double_init_resets) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(1000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 1u);

    /* Re-init should reset */
    ASSERT_EQ(asx_deadline_monitor_init(), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Registration tests                                                  */
/* ------------------------------------------------------------------ */

TEST(register_basic) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 42, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 1u);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_PENDING);
    teardown();
}

TEST(register_null_handle) {
    setup();
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(register_not_initialized) {
    asx_deadline_monitor_reset();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_E_INVALID_STATE);
}

TEST(register_exhaustion) {
    setup();
    asx_deadline_monitor_handle handles[ASX_MAX_DEADLINE_MONITORS];
    uint32_t i;
    for (i = 0; i < ASX_MAX_DEADLINE_MONITORS; i++) {
        ASSERT_EQ(asx_deadline_monitor_register(5000 + i, i, NULL, NULL, &handles[i]), ASX_OK);
    }
    asx_deadline_monitor_handle overflow;
    ASSERT_EQ(asx_deadline_monitor_register(9999, 99, NULL, NULL, &overflow),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_deadline_monitor_active_count(), ASX_MAX_DEADLINE_MONITORS);
    teardown();
}

TEST(register_multiple) {
    setup();
    asx_deadline_monitor_handle h1, h2, h3;
    ASSERT_EQ(asx_deadline_monitor_register(1000, 1, NULL, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(2000, 2, NULL, NULL, &h2), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(3000, 3, NULL, NULL, &h3), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 3u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Complete (deadline met) tests                                       */
/* ------------------------------------------------------------------ */

TEST(complete_before_deadline) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 3000), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_MET);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 0u);
    teardown();
}

TEST(complete_at_deadline) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 5000), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_MET);
    teardown();
}

TEST(complete_after_deadline) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 7000), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_MISSED);
    teardown();
}

TEST(complete_null_handle) {
    setup();
    ASSERT_EQ(asx_deadline_monitor_complete(NULL, 1000), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(complete_stale_handle) {
    setup();
    asx_deadline_monitor_handle h;
    h.slot = 99;
    h.generation = 999;
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 1000), ASX_E_NOT_FOUND);
    teardown();
}

TEST(complete_already_resolved) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 3000), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 4000), ASX_E_INVALID_STATE);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Cancel tests                                                        */
/* ------------------------------------------------------------------ */

TEST(cancel_pending) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_cancel(&h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_CANCELLED);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 0u);
    teardown();
}

TEST(cancel_already_resolved) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 3000), ASX_OK);
    /* Cancel on already-resolved deadline is a no-op */
    ASSERT_EQ(asx_deadline_monitor_cancel(&h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_MET);
    teardown();
}

TEST(cancel_null_handle) {
    setup();
    ASSERT_EQ(asx_deadline_monitor_cancel(NULL), ASX_E_INVALID_ARGUMENT);
    teardown();
}

TEST(cancel_stale_handle) {
    setup();
    asx_deadline_monitor_handle h;
    h.slot = 50;
    h.generation = 999;
    ASSERT_EQ(asx_deadline_monitor_cancel(&h), ASX_E_NOT_FOUND);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Check (miss detection) tests                                        */
/* ------------------------------------------------------------------ */

TEST(check_no_misses) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_check(3000), 0u);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_PENDING);
    teardown();
}

TEST(check_single_miss) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_check(6000), 1u);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_MISSED);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 0u);
    teardown();
}

TEST(check_at_exact_deadline) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    /* now_ns >= target_ns triggers miss */
    ASSERT_EQ(asx_deadline_monitor_check(5000), 1u);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h), ASX_DEADLINE_MISSED);
    teardown();
}

TEST(check_multiple_some_miss) {
    setup();
    asx_deadline_monitor_handle h1, h2, h3;
    ASSERT_EQ(asx_deadline_monitor_register(1000, 1, NULL, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(5000, 2, NULL, NULL, &h2), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(2000, 3, NULL, NULL, &h3), ASX_OK);

    ASSERT_EQ(asx_deadline_monitor_check(3000), 2u);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h1), ASX_DEADLINE_MISSED);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h2), ASX_DEADLINE_PENDING);
    ASSERT_EQ(asx_deadline_monitor_get_state(&h3), ASX_DEADLINE_MISSED);
    ASSERT_EQ(asx_deadline_monitor_active_count(), 1u);
    teardown();
}

TEST(check_idempotent) {
    setup();
    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_check(6000), 1u);
    /* Second check should find no new misses */
    ASSERT_EQ(asx_deadline_monitor_check(7000), 0u);
    teardown();
}

TEST(check_not_initialized) {
    asx_deadline_monitor_reset();
    ASSERT_EQ(asx_deadline_monitor_check(9999), 0u);
}

/* ------------------------------------------------------------------ */
/* Callback tests                                                      */
/* ------------------------------------------------------------------ */

static uint64_t g_cb_entity;
static uint64_t g_cb_margin;
static int g_cb_count;

static void miss_callback(uint64_t entity_id, uint64_t margin_ns, void *user_data) {
    (void)user_data;
    g_cb_entity = entity_id;
    g_cb_margin = margin_ns;
    g_cb_count++;
}

TEST(callback_on_miss) {
    setup();
    g_cb_entity = 0;
    g_cb_margin = 0;
    g_cb_count = 0;

    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 42, miss_callback, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_check(8000), 1u);
    ASSERT_EQ(g_cb_count, 1);
    ASSERT_EQ(g_cb_entity, (uint64_t)42);
    ASSERT_EQ(g_cb_margin, (uint64_t)3000);
    teardown();
}

TEST(no_callback_when_met) {
    setup();
    g_cb_count = 0;

    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, miss_callback, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 3000), ASX_OK);
    /* Check should not trigger callback since deadline was already met */
    ASSERT_EQ(asx_deadline_monitor_check(6000), 0u);
    ASSERT_EQ(g_cb_count, 0);
    teardown();
}

TEST(callback_with_user_data) {
    setup();
    int sentinel = 0;
    g_cb_count = 0;

    asx_deadline_monitor_handle h;
    ASSERT_EQ(asx_deadline_monitor_register(1000, 1, miss_callback, &sentinel, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_check(2000), 1u);
    ASSERT_EQ(g_cb_count, 1);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Statistics tests                                                    */
/* ------------------------------------------------------------------ */

TEST(stats_after_met) {
    setup();
    asx_deadline_monitor_handle h;
    asx_deadline_monitor_stats stats;

    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h, 3000), ASX_OK);

    asx_deadline_monitor_get_stats(&stats);
    ASSERT_EQ(stats.total_registered, 1u);
    ASSERT_EQ(stats.met_count, 1u);
    ASSERT_EQ(stats.missed_count, 0u);
    ASSERT_EQ(stats.active_count, 0u);
    ASSERT_TRUE(stats.best_margin_ns > 0); /* 2000 ns margin */
    teardown();
}

TEST(stats_after_miss_via_check) {
    setup();
    asx_deadline_monitor_handle h;
    asx_deadline_monitor_stats stats;

    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    asx_deadline_monitor_check(8000);

    asx_deadline_monitor_get_stats(&stats);
    ASSERT_EQ(stats.total_registered, 1u);
    ASSERT_EQ(stats.met_count, 0u);
    ASSERT_EQ(stats.missed_count, 1u);
    ASSERT_TRUE(stats.worst_margin_ns < 0); /* negative = miss */
    teardown();
}

TEST(stats_after_cancel) {
    setup();
    asx_deadline_monitor_handle h;
    asx_deadline_monitor_stats stats;

    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_cancel(&h), ASX_OK);

    asx_deadline_monitor_get_stats(&stats);
    ASSERT_EQ(stats.total_registered, 1u);
    ASSERT_EQ(stats.cancelled_count, 1u);
    ASSERT_EQ(stats.active_count, 0u);
    teardown();
}

TEST(stats_null) {
    setup();
    /* Should not crash */
    asx_deadline_monitor_get_stats(NULL);
    teardown();
}

TEST(stats_mixed) {
    setup();
    asx_deadline_monitor_handle h1, h2, h3;
    asx_deadline_monitor_stats stats;

    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(3000, 2, NULL, NULL, &h2), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(7000, 3, NULL, NULL, &h3), ASX_OK);

    ASSERT_EQ(asx_deadline_monitor_complete(&h1, 4000), ASX_OK); /* met with 1000 margin */
    asx_deadline_monitor_check(4000);                            /* h2 missed by 1000 */
    ASSERT_EQ(asx_deadline_monitor_cancel(&h3), ASX_OK);

    asx_deadline_monitor_get_stats(&stats);
    ASSERT_EQ(stats.total_registered, 3u);
    ASSERT_EQ(stats.met_count, 1u);
    ASSERT_EQ(stats.missed_count, 1u);
    ASSERT_EQ(stats.cancelled_count, 1u);
    ASSERT_EQ(stats.active_count, 0u);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Slot reuse tests                                                    */
/* ------------------------------------------------------------------ */

TEST(slot_reuse_after_resolve) {
    setup();
    asx_deadline_monitor_handle h1, h2;

    ASSERT_EQ(asx_deadline_monitor_register(1000, 1, NULL, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h1, 500), ASX_OK);

    /* Should reuse slot 0 */
    ASSERT_EQ(asx_deadline_monitor_register(2000, 2, NULL, NULL, &h2), ASX_OK);
    ASSERT_EQ(h2.slot, 0u);
    ASSERT_NE(h2.generation, h1.generation); /* different generation */
    teardown();
}

TEST(stale_handle_returns_cancelled) {
    setup();
    asx_deadline_monitor_handle h1, h2;

    ASSERT_EQ(asx_deadline_monitor_register(1000, 1, NULL, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_complete(&h1, 500), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(2000, 2, NULL, NULL, &h2), ASX_OK);

    /* h1 is now stale (slot reused with new generation) */
    ASSERT_EQ(asx_deadline_monitor_get_state(&h1), ASX_DEADLINE_CANCELLED);
    teardown();
}

/* ------------------------------------------------------------------ */
/* Shutdown tests                                                      */
/* ------------------------------------------------------------------ */

TEST(shutdown_cancels_pending) {
    setup();
    asx_deadline_monitor_handle h1, h2;
    asx_deadline_monitor_stats stats;

    ASSERT_EQ(asx_deadline_monitor_register(5000, 1, NULL, NULL, &h1), ASX_OK);
    ASSERT_EQ(asx_deadline_monitor_register(6000, 2, NULL, NULL, &h2), ASX_OK);

    asx_deadline_monitor_shutdown();

    asx_deadline_monitor_get_stats(&stats);
    ASSERT_EQ(stats.cancelled_count, 2u);
    ASSERT_EQ(stats.active_count, 0u);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "test_deadline_monitor\n");

    /* Lifecycle */
    RUN_TEST(init_shutdown);
    RUN_TEST(double_init_resets);

    /* Registration */
    RUN_TEST(register_basic);
    RUN_TEST(register_null_handle);
    RUN_TEST(register_not_initialized);
    RUN_TEST(register_exhaustion);
    RUN_TEST(register_multiple);

    /* Complete */
    RUN_TEST(complete_before_deadline);
    RUN_TEST(complete_at_deadline);
    RUN_TEST(complete_after_deadline);
    RUN_TEST(complete_null_handle);
    RUN_TEST(complete_stale_handle);
    RUN_TEST(complete_already_resolved);

    /* Cancel */
    RUN_TEST(cancel_pending);
    RUN_TEST(cancel_already_resolved);
    RUN_TEST(cancel_null_handle);
    RUN_TEST(cancel_stale_handle);

    /* Check (miss detection) */
    RUN_TEST(check_no_misses);
    RUN_TEST(check_single_miss);
    RUN_TEST(check_at_exact_deadline);
    RUN_TEST(check_multiple_some_miss);
    RUN_TEST(check_idempotent);
    RUN_TEST(check_not_initialized);

    /* Callbacks */
    RUN_TEST(callback_on_miss);
    RUN_TEST(no_callback_when_met);
    RUN_TEST(callback_with_user_data);

    /* Statistics */
    RUN_TEST(stats_after_met);
    RUN_TEST(stats_after_miss_via_check);
    RUN_TEST(stats_after_cancel);
    RUN_TEST(stats_null);
    RUN_TEST(stats_mixed);

    /* Slot reuse */
    RUN_TEST(slot_reuse_after_resolve);
    RUN_TEST(stale_handle_returns_cancelled);

    /* Shutdown */
    RUN_TEST(shutdown_cancels_pending);

    TEST_REPORT();
    return test_failures;
}
