/*
 * test_automotive_instrument.c — automotive instrumentation tests (bd-j4m.4)
 *
 * Exercises deadline tracking, watchdog monitoring, audit ring,
 * compliance gates, and global instrumentation state.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_log.h"
#include "test_harness.h"
#include <asx/runtime/automotive_instrument.h>

/* -------------------------------------------------------------------
 * Deadline tracker tests
 * ------------------------------------------------------------------- */

TEST(deadline_init_zeroes)
{
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);
    ASSERT_EQ(dt.total_deadlines, 0u);
    ASSERT_EQ(dt.deadline_hits, 0u);
    ASSERT_EQ(dt.deadline_misses, 0u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(&dt), 0u);
}

TEST(deadline_hit_records_correctly)
{
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);

    /* Task completes at t=800, deadline at t=1000 → hit */
    asx_auto_deadline_record(&dt, 1000, 800);

    ASSERT_EQ(dt.total_deadlines, 1u);
    ASSERT_EQ(dt.deadline_hits, 1u);
    ASSERT_EQ(dt.deadline_misses, 0u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(&dt), 0u);
}

TEST(deadline_miss_records_correctly)
{
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);

    /* Task completes at t=1200, deadline at t=1000 → miss */
    asx_auto_deadline_record(&dt, 1000, 1200);

    ASSERT_EQ(dt.total_deadlines, 1u);
    ASSERT_EQ(dt.deadline_hits, 0u);
    ASSERT_EQ(dt.deadline_misses, 1u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(&dt), 10000u); /* 100% */
}

TEST(deadline_exact_is_hit)
{
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);

    /* Exact deadline: actual == deadline → hit */
    asx_auto_deadline_record(&dt, 1000, 1000);
    ASSERT_EQ(dt.deadline_hits, 1u);
    ASSERT_EQ(dt.deadline_misses, 0u);
}

TEST(deadline_miss_rate_mixed)
{
    asx_auto_deadline_tracker dt;
    uint32_t i;

    asx_auto_deadline_init(&dt);

    /* 8 hits, 2 misses → 20% miss rate = 2000 in pct*100 */
    for (i = 0; i < 8; i++) {
        asx_auto_deadline_record(&dt, 1000, 500 + i * 10);
    }
    for (i = 0; i < 2; i++) {
        asx_auto_deadline_record(&dt, 1000, 1100 + i * 10);
    }

    ASSERT_EQ(dt.total_deadlines, 10u);
    ASSERT_EQ(dt.deadline_misses, 2u);
    ASSERT_EQ(asx_auto_deadline_miss_rate(&dt), 2000u); /* 20% */
}

TEST(deadline_worst_margin_tracks_misses)
{
    asx_auto_deadline_tracker dt;
    asx_auto_deadline_init(&dt);

    asx_auto_deadline_record(&dt, 1000, 800);  /* margin +200 */
    asx_auto_deadline_record(&dt, 1000, 1300); /* margin -300 */
    asx_auto_deadline_record(&dt, 1000, 1100); /* margin -100 */

    ASSERT_TRUE(dt.worst_margin_ns < 0);
    ASSERT_TRUE(dt.best_margin_ns > 0);
}

TEST(deadline_null_safety)
{
    asx_auto_deadline_init(NULL);
    asx_auto_deadline_record(NULL, 100, 200);
    ASSERT_EQ(asx_auto_deadline_miss_rate(NULL), 0u);
    asx_auto_deadline_reset(NULL);
}

/* -------------------------------------------------------------------
 * Watchdog monitor tests
 * ------------------------------------------------------------------- */

TEST(watchdog_init_with_period)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 5000);

    ASSERT_EQ(wd.watchdog_period_ns, (uint64_t)5000);
    ASSERT_EQ(wd.total_checkpoints, 0u);
    ASSERT_EQ(wd.violations, 0u);
    ASSERT_EQ(wd.armed, 0);
}

TEST(watchdog_no_violation_within_period)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 1000);

    asx_auto_watchdog_checkpoint(&wd, 100);
    asx_auto_watchdog_checkpoint(&wd, 600);  /* interval 500 < 1000 */
    asx_auto_watchdog_checkpoint(&wd, 1100); /* interval 500 < 1000 */

    ASSERT_EQ(wd.total_checkpoints, 3u);
    ASSERT_EQ(wd.violations, 0u);
}

TEST(watchdog_violation_exceeds_period)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 1000);

    asx_auto_watchdog_checkpoint(&wd, 100);
    asx_auto_watchdog_checkpoint(&wd, 1200); /* interval 1100 > 1000 */

    ASSERT_EQ(wd.total_checkpoints, 2u);
    ASSERT_EQ(wd.violations, 1u);
}

TEST(watchdog_worst_interval_tracks)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 5000);

    asx_auto_watchdog_checkpoint(&wd, 100);
    asx_auto_watchdog_checkpoint(&wd, 400);  /* interval 300 */
    asx_auto_watchdog_checkpoint(&wd, 2000); /* interval 1600 */
    asx_auto_watchdog_checkpoint(&wd, 2200); /* interval 200 */

    ASSERT_EQ(wd.worst_interval_ns, (uint64_t)1600);
}

TEST(watchdog_would_trigger)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 1000);

    asx_auto_watchdog_checkpoint(&wd, 100);

    ASSERT_EQ(asx_auto_watchdog_would_trigger(&wd, 500), 0);  /* 400 < 1000 */
    ASSERT_EQ(asx_auto_watchdog_would_trigger(&wd, 1200), 1); /* 1100 > 1000 */
}

TEST(watchdog_first_checkpoint_no_violation)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 100);

    /* First checkpoint — no prior reference, no violation possible */
    asx_auto_watchdog_checkpoint(&wd, 99999);
    ASSERT_EQ(wd.violations, 0u);
    ASSERT_EQ(wd.armed, 1);
}

TEST(watchdog_null_safety)
{
    asx_auto_watchdog_init(NULL, 1000);
    asx_auto_watchdog_checkpoint(NULL, 100);
    ASSERT_EQ(asx_auto_watchdog_would_trigger(NULL, 200), 0);
    asx_auto_watchdog_reset(NULL);
}

TEST(watchdog_reset_preserves_period)
{
    asx_auto_watchdog wd;
    asx_auto_watchdog_init(&wd, 5000);

    asx_auto_watchdog_checkpoint(&wd, 100);
    asx_auto_watchdog_checkpoint(&wd, 10000);
    ASSERT_EQ(wd.violations, 1u);

    asx_auto_watchdog_reset(&wd);
    ASSERT_EQ(wd.watchdog_period_ns, (uint64_t)5000);
    ASSERT_EQ(wd.violations, 0u);
    ASSERT_EQ(wd.armed, 0);
}

/* -------------------------------------------------------------------
 * Audit ring tests
 * ------------------------------------------------------------------- */

TEST(audit_init_empty)
{
    asx_auto_audit_ring ring;
    asx_auto_audit_init(&ring);

    ASSERT_EQ(asx_auto_audit_count(&ring), 0u);
    ASSERT_EQ(asx_auto_audit_total(&ring), 0u);
    ASSERT_TRUE(asx_auto_audit_get(&ring, 0) == NULL);
}

TEST(audit_record_and_get)
{
    asx_auto_audit_ring ring;
    const asx_audit_entry *e;

    asx_auto_audit_init(&ring);

    asx_auto_audit_record(&ring, ASX_AUDIT_DEADLINE_MISS,
                           1000, 42, -200);

    ASSERT_EQ(asx_auto_audit_count(&ring), 1u);
    e = asx_auto_audit_get(&ring, 0);
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ(e->kind, ASX_AUDIT_DEADLINE_MISS);
    ASSERT_EQ(e->timestamp_ns, (uint64_t)1000);
    ASSERT_EQ(e->entity_id, (uint64_t)42);
    ASSERT_EQ(e->seq, 0u);
}

TEST(audit_monotonic_sequence)
{
    asx_auto_audit_ring ring;
    const asx_audit_entry *e0;
    const asx_audit_entry *e1;
    const asx_audit_entry *e2;

    asx_auto_audit_init(&ring);

    asx_auto_audit_record(&ring, ASX_AUDIT_REGION_POISONED, 100, 1, 0);
    asx_auto_audit_record(&ring, ASX_AUDIT_CANCEL_FORCED, 200, 2, 0);
    asx_auto_audit_record(&ring, ASX_AUDIT_DEGRADED_ENTER, 300, 3, 0);

    e0 = asx_auto_audit_get(&ring, 0);
    e1 = asx_auto_audit_get(&ring, 1);
    e2 = asx_auto_audit_get(&ring, 2);

    ASSERT_TRUE(e0 != NULL);
    ASSERT_TRUE(e1 != NULL);
    ASSERT_TRUE(e2 != NULL);
    ASSERT_TRUE(e0->seq < e1->seq);
    ASSERT_TRUE(e1->seq < e2->seq);
}

TEST(audit_ring_wraparound)
{
    asx_auto_audit_ring ring;
    uint32_t i;
    const asx_audit_entry *oldest;
    const asx_audit_entry *newest;

    asx_auto_audit_init(&ring);

    /* Write more than ring capacity */
    for (i = 0; i < ASX_AUTO_AUDIT_RING_SIZE + 10u; i++) {
        asx_auto_audit_record(&ring, ASX_AUDIT_CHECKPOINT_OK,
                               (uint64_t)(i * 100), (uint64_t)i, 0);
    }

    ASSERT_EQ(asx_auto_audit_count(&ring), (uint32_t)ASX_AUTO_AUDIT_RING_SIZE);
    ASSERT_EQ(asx_auto_audit_total(&ring),
              (uint32_t)(ASX_AUTO_AUDIT_RING_SIZE + 10u));

    /* Oldest should be entry 10 (first 10 were overwritten) */
    oldest = asx_auto_audit_get(&ring, 0);
    ASSERT_TRUE(oldest != NULL);
    ASSERT_EQ(oldest->entity_id, (uint64_t)10);

    /* Newest should be the last entry */
    newest = asx_auto_audit_get(&ring,
                                 ASX_AUTO_AUDIT_RING_SIZE - 1u);
    ASSERT_TRUE(newest != NULL);
    ASSERT_EQ(newest->entity_id,
              (uint64_t)(ASX_AUTO_AUDIT_RING_SIZE + 10u - 1u));
}

TEST(audit_kind_str)
{
    ASSERT_STR_EQ(asx_audit_kind_str(ASX_AUDIT_REGION_POISONED),
                  "REGION_POISONED");
    ASSERT_STR_EQ(asx_audit_kind_str(ASX_AUDIT_DEADLINE_MISS),
                  "DEADLINE_MISS");
    ASSERT_STR_EQ(asx_audit_kind_str(ASX_AUDIT_WATCHDOG_VIOLATION),
                  "WATCHDOG_VIOLATION");
    ASSERT_STR_EQ(asx_audit_kind_str((asx_audit_kind)99),
                  "UNKNOWN");
}

TEST(audit_null_safety)
{
    asx_auto_audit_init(NULL);
    asx_auto_audit_record(NULL, ASX_AUDIT_DEADLINE_MISS, 0, 0, 0);
    ASSERT_TRUE(asx_auto_audit_get(NULL, 0) == NULL);
    ASSERT_EQ(asx_auto_audit_count(NULL), 0u);
    ASSERT_EQ(asx_auto_audit_total(NULL), 0u);
}

/* -------------------------------------------------------------------
 * Compliance gate tests
 * ------------------------------------------------------------------- */

TEST(compliance_gate_default_thresholds)
{
    asx_auto_compliance_gate gate;
    asx_auto_compliance_gate_init(&gate);

    ASSERT_EQ(gate.max_miss_rate_pct100, 100u);   /* 1.0% */
    ASSERT_EQ(gate.max_watchdog_violations, 0u);
    ASSERT_EQ(gate.min_checkpoints, 1u);
}

TEST(compliance_pass_all_within_thresholds)
{
    asx_auto_compliance_gate gate;
    asx_auto_deadline_tracker dt;
    asx_auto_watchdog wd;
    asx_auto_compliance_result result;
    uint32_t i;

    asx_auto_compliance_gate_init(&gate);
    asx_auto_deadline_init(&dt);
    asx_auto_watchdog_init(&wd, 1000);

    /* 100 hits, 0 misses */
    for (i = 0; i < 100; i++) {
        asx_auto_deadline_record(&dt, 1000, 500);
    }

    /* 5 checkpoints, no violations */
    asx_auto_watchdog_checkpoint(&wd, 100);
    asx_auto_watchdog_checkpoint(&wd, 200);
    asx_auto_watchdog_checkpoint(&wd, 300);
    asx_auto_watchdog_checkpoint(&wd, 400);
    asx_auto_watchdog_checkpoint(&wd, 500);

    asx_auto_compliance_evaluate(&gate, &dt, &wd, &result);
    ASSERT_EQ(result.pass, 1);
    ASSERT_EQ(result.violation_mask, 0u);
}

TEST(compliance_fail_high_miss_rate)
{
    asx_auto_compliance_gate gate;
    asx_auto_deadline_tracker dt;
    asx_auto_watchdog wd;
    asx_auto_compliance_result result;
    uint32_t i;

    asx_auto_compliance_gate_init(&gate);
    asx_auto_deadline_init(&dt);
    asx_auto_watchdog_init(&wd, 10000);

    /* 5 hits, 5 misses → 50% miss rate */
    for (i = 0; i < 5; i++) {
        asx_auto_deadline_record(&dt, 1000, 500);
    }
    for (i = 0; i < 5; i++) {
        asx_auto_deadline_record(&dt, 1000, 2000);
    }

    asx_auto_watchdog_checkpoint(&wd, 100);

    asx_auto_compliance_evaluate(&gate, &dt, &wd, &result);
    ASSERT_EQ(result.pass, 0);
    ASSERT_TRUE((result.violation_mask & ASX_COMPLIANCE_DEADLINE_RATE) != 0);
}

TEST(compliance_fail_watchdog_violations)
{
    asx_auto_compliance_gate gate;
    asx_auto_deadline_tracker dt;
    asx_auto_watchdog wd;
    asx_auto_compliance_result result;

    asx_auto_compliance_gate_init(&gate);
    asx_auto_deadline_init(&dt);
    asx_auto_watchdog_init(&wd, 100);

    /* No deadline data */
    asx_auto_deadline_record(&dt, 1000, 500);

    /* Checkpoint with violation */
    asx_auto_watchdog_checkpoint(&wd, 100);
    asx_auto_watchdog_checkpoint(&wd, 300); /* interval 200 > 100 */

    asx_auto_compliance_evaluate(&gate, &dt, &wd, &result);
    ASSERT_EQ(result.pass, 0);
    ASSERT_TRUE((result.violation_mask & ASX_COMPLIANCE_WATCHDOG) != 0);
}

TEST(compliance_fail_insufficient_checkpoints)
{
    asx_auto_compliance_gate gate;
    asx_auto_watchdog wd;
    asx_auto_compliance_result result;

    asx_auto_compliance_gate_init(&gate);
    asx_auto_watchdog_init(&wd, 1000);

    /* No checkpoints recorded */
    asx_auto_compliance_evaluate(&gate, NULL, &wd, &result);
    ASSERT_EQ(result.pass, 0);
    ASSERT_TRUE((result.violation_mask & ASX_COMPLIANCE_CHECKPOINT_MIN) != 0);
}

/* -------------------------------------------------------------------
 * Global state tests
 * ------------------------------------------------------------------- */

TEST(global_reset_clears_state)
{
    asx_auto_deadline_tracker *dt;
    asx_auto_watchdog *wd;
    asx_auto_audit_ring *ring;

    asx_auto_instrument_reset();

    dt = asx_auto_deadline_global();
    wd = asx_auto_watchdog_global();
    ring = asx_auto_audit_global();

    ASSERT_TRUE(dt != NULL);
    ASSERT_TRUE(wd != NULL);
    ASSERT_TRUE(ring != NULL);

    ASSERT_EQ(dt->total_deadlines, 0u);
    ASSERT_EQ(wd->total_checkpoints, 0u);
    ASSERT_EQ(asx_auto_audit_count(ring), 0u);
}

TEST(global_record_deadline_auto_logs_miss)
{
    asx_auto_deadline_tracker *dt;
    asx_auto_audit_ring *ring;
    const asx_audit_entry *e;

    asx_auto_instrument_reset();
    dt = asx_auto_deadline_global();
    ring = asx_auto_audit_global();

    /* Hit — should not log to audit ring */
    asx_auto_record_deadline(1000, 800, 1);
    ASSERT_EQ(asx_auto_audit_count(ring), 0u);
    ASSERT_EQ(dt->deadline_hits, 1u);

    /* Miss — should auto-log to audit ring */
    asx_auto_record_deadline(1000, 1200, 2);
    ASSERT_EQ(asx_auto_audit_count(ring), 1u);
    e = asx_auto_audit_get(ring, 0);
    ASSERT_TRUE(e != NULL);
    ASSERT_EQ(e->kind, ASX_AUDIT_DEADLINE_MISS);
    ASSERT_EQ(e->entity_id, (uint64_t)2);
}

TEST(global_record_checkpoint_auto_logs_violation)
{
    asx_auto_watchdog *wd;
    asx_auto_audit_ring *ring;

    asx_auto_instrument_reset();
    wd = asx_auto_watchdog_global();
    ring = asx_auto_audit_global();

    /* Set a tight watchdog period */
    wd->watchdog_period_ns = 500;

    /* First checkpoint — no violation (first arm) */
    asx_auto_record_checkpoint(100, 10);
    ASSERT_EQ(asx_auto_audit_count(ring), 0u);

    /* Second checkpoint within period — no violation */
    asx_auto_record_checkpoint(400, 10);
    ASSERT_EQ(asx_auto_audit_count(ring), 0u);

    /* Third checkpoint exceeding period — violation logged */
    asx_auto_record_checkpoint(1200, 10);
    ASSERT_EQ(wd->violations, 1u);
}

/* -------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------- */

int main(void)
{
    test_log_open("unit", "runtime/automotive_instrument",
                  "test_automotive_instrument");

    /* Deadline tracker */
    RUN_TEST(deadline_init_zeroes);
    RUN_TEST(deadline_hit_records_correctly);
    RUN_TEST(deadline_miss_records_correctly);
    RUN_TEST(deadline_exact_is_hit);
    RUN_TEST(deadline_miss_rate_mixed);
    RUN_TEST(deadline_worst_margin_tracks_misses);
    RUN_TEST(deadline_null_safety);

    /* Watchdog monitor */
    RUN_TEST(watchdog_init_with_period);
    RUN_TEST(watchdog_no_violation_within_period);
    RUN_TEST(watchdog_violation_exceeds_period);
    RUN_TEST(watchdog_worst_interval_tracks);
    RUN_TEST(watchdog_would_trigger);
    RUN_TEST(watchdog_first_checkpoint_no_violation);
    RUN_TEST(watchdog_null_safety);
    RUN_TEST(watchdog_reset_preserves_period);

    /* Audit ring */
    RUN_TEST(audit_init_empty);
    RUN_TEST(audit_record_and_get);
    RUN_TEST(audit_monotonic_sequence);
    RUN_TEST(audit_ring_wraparound);
    RUN_TEST(audit_kind_str);
    RUN_TEST(audit_null_safety);

    /* Compliance gate */
    RUN_TEST(compliance_gate_default_thresholds);
    RUN_TEST(compliance_pass_all_within_thresholds);
    RUN_TEST(compliance_fail_high_miss_rate);
    RUN_TEST(compliance_fail_watchdog_violations);
    RUN_TEST(compliance_fail_insufficient_checkpoints);

    /* Global state */
    RUN_TEST(global_reset_clears_state);
    RUN_TEST(global_record_deadline_auto_logs_miss);
    RUN_TEST(global_record_checkpoint_auto_logs_violation);

    TEST_REPORT();
    test_log_close();
    return test_failures;
}
