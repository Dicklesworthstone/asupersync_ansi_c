/*
 * ex_timeout_deadline.c — Timer wheel, deadlines, and budget-bounded work
 *
 * Demonstrates time-related concepts:
 *   1. Timer wheel: register, collect expired, cancel
 *   2. Budget-bounded polling: work within poll limits
 *   3. Budget algebra: meet (tighten) and exhaustion detection
 *   4. Timer cancellation before expiry
 *
 * In asupersync, time is a semantic control plane — deadlines and
 * budgets bound all computation, ensuring bounded cleanup and
 * predictable cancellation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx.h>
#include <asx/core/budget.h>
#include <asx/time/timer_wheel.h>
#include <stdio.h>

static int g_pass = 0;
static int g_fail = 0;

#define SCENARIO_BEGIN(id)                                                                         \
    do {                                                                                           \
        const char *_scenario_id = (id);                                                           \
        int _scenario_ok = 1;                                                                      \
    (void)0

#define SCENARIO_CHECK(cond, msg)                                                                  \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("SCENARIO %s fail %s\n", _scenario_id, (msg));                                  \
            _scenario_ok = 0;                                                                      \
            g_fail++;                                                                              \
            goto _scenario_end;                                                                    \
        }                                                                                          \
    } while (0)

#define SCENARIO_END()                                                                             \
    _scenario_end:                                                                                 \
    if (_scenario_ok) {                                                                            \
        printf("SCENARIO %s pass\n", _scenario_id);                                                \
        g_pass++;                                                                                  \
    }                                                                                              \
    }                                                                                              \
    while (0)

/* -------------------------------------------------------------------
 * Scenario: timeout.timer_register_expire
 *
 * Registers a timer at deadline 5, advances time past it, and
 * collects the expired timer. Waker data pointer confirms the
 * correct timer fired.
 * ------------------------------------------------------------------- */

static void scenario_timer_expire(void) {
    SCENARIO_BEGIN("timeout.timer_register_expire");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    int marker = 0;
    asx_timer_handle handle;
    SCENARIO_CHECK(asx_timer_register(wheel, 5, &marker, &handle) == ASX_OK, "register");

    /* Collect expired timers at time=6 (past deadline=5) */
    void *wakers[4];
    uint32_t count = asx_timer_collect_expired(wheel, 6, wakers, 4);
    SCENARIO_CHECK(count == 1, "one_timer_expired");
    SCENARIO_CHECK(wakers[0] == &marker, "correct_waker_data");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: timeout.timer_cancel
 *
 * Registers a timer, cancels it before expiry, and verifies no
 * timers fire when collecting. This models deadline cancellation.
 * ------------------------------------------------------------------- */

static void scenario_timer_cancel(void) {
    SCENARIO_BEGIN("timeout.timer_cancel");

    asx_timer_wheel *wheel = asx_timer_wheel_global();
    asx_timer_wheel_reset(wheel);

    int marker = 0;
    asx_timer_handle handle;
    SCENARIO_CHECK(asx_timer_register(wheel, 5, &marker, &handle) == ASX_OK, "register");

    /* Cancel before expiry */
    int cancelled = asx_timer_cancel(wheel, &handle);
    SCENARIO_CHECK(cancelled == 1, "cancel_succeeded");

    /* Collect at time=10 — nothing should fire */
    void *wakers[4];
    uint32_t count = asx_timer_collect_expired(wheel, 10, wakers, 4);
    SCENARIO_CHECK(count == 0, "no_timers_fired");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: timeout.budget_bounded_work
 *
 * Demonstrates poll budgets: a loop that consumes budget and stops
 * when exhausted. Budgets bound all work in asupersync.
 * ------------------------------------------------------------------- */

static void scenario_budget_bounded(void) {
    SCENARIO_BEGIN("timeout.budget_bounded_work");

    asx_budget budget = asx_budget_from_polls(5);
    uint32_t work_done = 0;

    while (!asx_budget_is_exhausted(&budget)) {
        asx_budget_consume_poll(&budget);
        work_done++;
    }

    SCENARIO_CHECK(work_done == 5, "exactly_5_polls");
    SCENARIO_CHECK(asx_budget_is_exhausted(&budget), "budget_exhausted");

    SCENARIO_END();
}

/* -------------------------------------------------------------------
 * Scenario: timeout.budget_meet_tightens
 *
 * The meet operation (lattice infimum) tightens a budget — the
 * result is bounded by the stricter of the two inputs. This is how
 * cleanup budgets are derived from cancellation severity.
 * ------------------------------------------------------------------- */

static void scenario_budget_meet(void) {
    SCENARIO_BEGIN("timeout.budget_meet_tightens");

    asx_budget large = asx_budget_from_polls(100);
    asx_budget small = asx_budget_from_polls(3);
    asx_budget result = asx_budget_meet(&large, &small);

    /* Meet result should be bounded by the smaller budget */
    uint32_t work_done = 0;
    while (!asx_budget_is_exhausted(&result)) {
        asx_budget_consume_poll(&result);
        work_done++;
    }

    SCENARIO_CHECK(work_done == 3, "meet_tightens_to_3");

    SCENARIO_END();
}

int main(void) {
    scenario_timer_expire();
    scenario_timer_cancel();
    scenario_budget_bounded();
    scenario_budget_meet();

    printf("SCENARIO _summary %s total=%d pass=%d fail=%d\n", g_fail > 0 ? "fail" : "pass",
           g_pass + g_fail, g_pass, g_fail);

    return g_fail > 0 ? 1 : 0;
}
