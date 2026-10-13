/*
 * test_lab.c — unit tests for seeded lab runtime
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../test_harness.h"
#include <asx/runtime/lab.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>

#if !defined(ASX_PROFILE_BROWSER) || ASX_HAS_BROWSER_TRACE

static asx_status st_sink_;
#define MUST_OK(expr)                                                                              \
    do {                                                                                           \
        st_sink_ = (expr);                                                                         \
        (void)st_sink_;                                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Config tests                                                        */
/* ------------------------------------------------------------------ */

TEST(config_init_defaults) {
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    ASSERT_EQ(cfg.seed, (uint64_t)42); /* Rust LabConfig::default() */
    ASSERT_EQ(cfg.auto_advance, 0u);   /* Rust LabConfig::auto_advance */
    ASSERT_EQ(cfg.start_time_ns, (asx_time)0);
    ASSERT_EQ(cfg.max_polls, 1024u);
    ASSERT_EQ(cfg.scheduler, ASX_LAB_SCHEDULER_DISPATCH); /* Rust's lab dispatch (bd-9kll.9.5) */
}

TEST(init_refuses_an_unknown_scheduler) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    cfg.scheduler = (uint8_t)(ASX_LAB_SCHEDULER_SWEEP + 1u);
    ASSERT_EQ(asx_lab_init(&lab, &cfg), ASX_E_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Lifecycle tests                                                     */
/* ------------------------------------------------------------------ */

TEST(init_shutdown) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    ASSERT_EQ(asx_lab_init(&lab, &cfg), ASX_OK);
    ASSERT_TRUE(lab.initialized);
    asx_lab_shutdown(&lab);
    ASSERT_FALSE(lab.initialized);
}

TEST(init_null_fails) {
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    ASSERT_EQ(asx_lab_init(NULL, &cfg), ASX_E_INVALID_ARGUMENT);
}

TEST(init_null_config_fails) {
    asx_lab lab;
    ASSERT_EQ(asx_lab_init(&lab, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(shutdown_null_safe) { asx_lab_shutdown(NULL); /* should not crash */ }

TEST(double_shutdown_safe) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_shutdown(&lab);
    asx_lab_shutdown(&lab); /* safe to call twice */
}

/* ------------------------------------------------------------------ */
/* Virtual time tests                                                  */
/* ------------------------------------------------------------------ */

TEST(time_starts_at_zero) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0);
    asx_lab_shutdown(&lab);
}

TEST(time_starts_at_custom) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    cfg.start_time_ns = 5000000000ULL; /* 5 seconds */
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_lab_now(&lab), 5000000000ULL);
    asx_lab_shutdown(&lab);
}

TEST(advance_time) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_advance_time(&lab, 10000000ULL);
    ASSERT_EQ(asx_lab_now(&lab), 10000000ULL); /* 10ms */
    asx_lab_shutdown(&lab);
}

/* The runtime reads the lab clock, and reading never moves it (Rust's
 * frozen LabRuntime clock; it used to gain a tick per read). */
TEST(time_integrates_with_runtime) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_time now;
    asx_time again;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));

    asx_lab_advance_time(&lab, 5000000ULL);
    MUST_OK(asx_runtime_now_ns(&lab.rt, &now));
    MUST_OK(asx_runtime_now_ns(&lab.rt, &again));
    ASSERT_EQ(now, (asx_time)5000000ULL);
    ASSERT_EQ(again, now);
    ASSERT_EQ(asx_lab_now(&lab), now);
    ASSERT_TRUE(asx_runtime_clock_is_virtual());
    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Seeded entropy tests                                                */
/* ------------------------------------------------------------------ */

TEST(entropy_deterministic) {
    asx_lab lab1, lab2;
    asx_lab_config cfg;
    uint64_t a1, a2, b1, b2;

    asx_lab_config_init(&cfg);
    cfg.seed = 42;

    MUST_OK(asx_lab_init(&lab1, &cfg));
    a1 = asx_lab_random_u64(&lab1);
    a2 = asx_lab_random_u64(&lab1);
    asx_lab_shutdown(&lab1);

    MUST_OK(asx_lab_init(&lab2, &cfg));
    b1 = asx_lab_random_u64(&lab2);
    b2 = asx_lab_random_u64(&lab2);
    asx_lab_shutdown(&lab2);

    /* Same seed → same sequence */
    ASSERT_EQ(a1, b1);
    ASSERT_EQ(a2, b2);
}

TEST(entropy_different_seeds) {
    asx_lab lab1, lab2;
    asx_lab_config cfg;
    uint64_t a, b;

    asx_lab_config_init(&cfg);
    cfg.seed = 1;
    MUST_OK(asx_lab_init(&lab1, &cfg));
    a = asx_lab_random_u64(&lab1);
    asx_lab_shutdown(&lab1);

    cfg.seed = 2;
    MUST_OK(asx_lab_init(&lab2, &cfg));
    b = asx_lab_random_u64(&lab2);
    asx_lab_shutdown(&lab2);

    /* Different seeds → different values (with overwhelming probability) */
    ASSERT_NE(a, b);
}

/* ------------------------------------------------------------------ */
/* Reset tests                                                         */
/* ------------------------------------------------------------------ */

TEST(reset_restores_state) {
    asx_lab lab;
    asx_lab_config cfg;
    uint64_t before_r, after_r;

    asx_lab_config_init(&cfg);
    cfg.seed = 99;
    MUST_OK(asx_lab_init(&lab, &cfg));

    before_r = asx_lab_random_u64(&lab);
    asx_lab_advance_time(&lab, 100);
    asx_lab_reset(&lab);

    /* After reset, entropy and time should restart */
    after_r = asx_lab_random_u64(&lab);
    ASSERT_EQ(before_r, after_r);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0);

    asx_lab_shutdown(&lab);
}

TEST(reset_restores_runtime_hooks) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_time now = 0;
    uint64_t entropy = 0;

    asx_lab_config_init(&cfg);
    cfg.seed = 123u;
    MUST_OK(asx_lab_init(&lab, &cfg));

    asx_lab_advance_time(&lab, 7000000u);
    MUST_OK(asx_runtime_now_ns(&lab.rt, &now));
    ASSERT_TRUE(now > 0);
    MUST_OK(asx_runtime_random_u64(&lab.rt, &entropy));
    ASSERT_TRUE(entropy != 0u);

    asx_lab_reset(&lab);

    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0);
    ASSERT_EQ(asx_runtime_now_ns(&lab.rt, &now), ASX_OK);
    ASSERT_EQ(now, (asx_time)0);
    ASSERT_EQ(asx_runtime_random_u64(&lab.rt, &entropy), ASX_OK);

    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Scenario execution tests                                            */
/* ------------------------------------------------------------------ */

static asx_status step_noop(asx_lab *lab, void *user_data) {
    (void)lab;
    (void)user_data;
    return ASX_OK;
}

static asx_status step_advance_5(asx_lab *lab, void *user_data) {
    (void)user_data;
    asx_lab_advance_time(lab, 5000000u); /* 5 ms */
    return ASX_OK;
}

static asx_status step_fail(asx_lab *lab, void *user_data) {
    (void)lab;
    (void)user_data;
    return ASX_E_INVALID_STATE;
}

static asx_status step_open_close_region(asx_lab *lab, void *user_data) {
    asx_region_id rid;
    asx_status st;
    (void)user_data;
    st = asx_lab_open_region(lab, &rid);
    if (st != ASX_OK) return st;
    return asx_region_close(rid);
}

typedef struct {
    int polls_done;
} poll_twice_state;

static asx_status poll_twice_then_complete(void *user_data, asx_task_id self) {
    poll_twice_state *state = (poll_twice_state *)user_data;
    (void)self;
    state->polls_done++;
    if (state->polls_done < 2) return ASX_E_PENDING;
    return ASX_OK;
}

static asx_status step_run_two_polls(asx_lab *lab, void *user_data) {
    poll_twice_state *state = (poll_twice_state *)user_data;
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    st = asx_lab_open_region(lab, &rid);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(rid, poll_twice_then_complete, state, &tid);
    if (st != ASX_OK) return st;
    budget = asx_budget_infinite();
    st = asx_scheduler_run(rid, &budget);
    if (st != ASX_OK) return st;
    return asx_region_close(rid);
}

TEST(scenario_empty) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "empty");

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_total, 0u);
    ASSERT_EQ(result.steps_completed, 0u);

    asx_lab_shutdown(&lab);
}

TEST(scenario_single_step) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "single");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.last_status, ASX_OK);

    asx_lab_shutdown(&lab);
}

TEST(scenario_multi_step) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "multi");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 2u);
    /* 10 ticks × 1ms = 10ms elapsed */
    ASSERT_EQ(result.elapsed_ns, 10000000ULL);

    asx_lab_shutdown(&lab);
}

TEST(scenario_stops_on_failure) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "fail-early");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_fail, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_E_INVALID_STATE);
    ASSERT_EQ(result.steps_completed, 2u); /* ran 2, stopped at failure */
    ASSERT_EQ(result.steps_total, 3u);
    ASSERT_EQ(result.last_status, ASX_E_INVALID_STATE);

    asx_lab_shutdown(&lab);
}

TEST(scenario_with_region_ops) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "region-ops");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_open_close_region, NULL));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 1u);

    asx_lab_shutdown(&lab);
}

TEST(scenario_tracks_scheduler_polls) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;
    poll_twice_state state;

    memset(&state, 0, sizeof(state));
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_trace_reset();
    asx_lab_scenario_init(&sc, "poll-count");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_run_two_polls, &state));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.polls_total, (uint64_t)2u);

    asx_lab_shutdown(&lab);
}

TEST(scenario_enforces_max_polls_per_step) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;
    poll_twice_state state;

    memset(&state, 0, sizeof(state));
    asx_lab_config_init(&cfg);
    cfg.max_polls = 1u;
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_trace_reset();
    asx_lab_scenario_init(&sc, "poll-budget");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_run_two_polls, &state));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.last_status, ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(result.polls_total, (uint64_t)2u);

    asx_lab_shutdown(&lab);
}

TEST(scenario_null_step_fails_closed) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "null-step");
    sc.step_count = 1u;
    sc.steps[0] = NULL;

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_E_INVALID_STATE);
    ASSERT_EQ(result.steps_completed, 0u);
    ASSERT_EQ(result.steps_total, 1u);
    ASSERT_EQ(result.last_status, ASX_E_INVALID_STATE);

    asx_lab_shutdown(&lab);
}

TEST(scenario_corrupt_step_count_fails_closed) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_lab_scenario_init(&sc, "corrupt-count");
    sc.step_count = ASX_LAB_MAX_STEPS + 1u;

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_E_INVALID_STATE);

    asx_lab_shutdown(&lab);
}

TEST(scenario_overflow) {
    asx_lab_scenario sc;
    uint32_t i;
    asx_lab_scenario_init(&sc, "overflow");
    for (i = 0; i < ASX_LAB_MAX_STEPS; i++) {
        MUST_OK(asx_lab_scenario_add_step(&sc, step_noop, NULL));
    }
    ASSERT_EQ(asx_lab_scenario_add_step(&sc, step_noop, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

/* ------------------------------------------------------------------ */
/* Deterministic replay test                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Clock control: frozen, advanced explicitly or by auto_advance      */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_time until;
    int done;
} lab_sleeper;

static asx_status poll_lab_sleeper(void *ud, asx_task_id self) {
    lab_sleeper *s = (lab_sleeper *)ud;
    asx_status st = asx_task_wait_until(self, s->until);
    if (st != ASX_OK) return st;
    s->done = 1;
    return ASX_OK;
}

/* Without auto_advance (Rust's default) the run leaves a sleeper waiting
 * and the clock where it was. advance_time only moves the clock (the due
 * timer still waits for a step); advance_to_next_timer moves it and fires
 * the timer, and the next run completes the sleeper. */
TEST(run_without_auto_advance_keeps_the_clock) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_region_id r;
    asx_task_id t;
    asx_budget b;
    asx_time next = 0;
    uint32_t woken = 0;
    lab_sleeper s = {1000000u, 0};

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_open_region(&lab, &r));
    MUST_OK(asx_task_spawn(r, poll_lab_sleeper, &s, &t));
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_lab_run(&lab, r, &b), ASX_E_PENDING);
    ASSERT_EQ(s.done, 0);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)0);
    MUST_OK(asx_lab_next_timer_deadline(&lab, &next));
    ASSERT_EQ(next, (asx_time)1000000u);

    asx_lab_advance_time(&lab, 500000u);
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_lab_run(&lab, r, &b), ASX_E_PENDING);
    ASSERT_EQ(s.done, 0);

    MUST_OK(asx_lab_advance_to_next_timer(&lab, &woken));
    ASSERT_EQ(woken, 1u);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)1000000u);
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_lab_run(&lab, r, &b), ASX_OK);
    ASSERT_EQ(s.done, 1);
    ASSERT_EQ(asx_lab_advance_to_next_timer(&lab, &woken), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_lab_next_timer_deadline(&lab, &next), ASX_E_NOT_FOUND);
    asx_lab_shutdown(&lab);
}

/* With auto_advance the run jumps to each timer in turn: every sleeper
 * completes and the clock ends at the latest deadline. */
TEST(run_with_auto_advance_completes_sleepers) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_region_id r;
    asx_task_id t;
    asx_budget b;
    lab_sleeper s1 = {1000000u, 0};
    lab_sleeper s3 = {3000000u, 0};

    asx_lab_config_init(&cfg);
    cfg.auto_advance = 1u;
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_open_region(&lab, &r));
    MUST_OK(asx_task_spawn(r, poll_lab_sleeper, &s3, &t));
    MUST_OK(asx_task_spawn(r, poll_lab_sleeper, &s1, &t));
    b = asx_budget_from_polls(100);
    ASSERT_EQ(asx_lab_run(&lab, r, &b), ASX_OK);
    ASSERT_EQ(s1.done, 1);
    ASSERT_EQ(s3.done, 1);
    ASSERT_EQ(asx_lab_now(&lab), (asx_time)3000000u);
    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Dispatch model: the seed reaches task dispatch (bd-9kll.9.5)        */
/* ------------------------------------------------------------------ */

/* The lab drives Rust's LabRuntime dispatch with one worker, seeded from
 * config.seed: each step draws one value from the seeded xorshift64 and
 * polls the task at index (draw % n) of the ready tasks of the top priority,
 * in wake order. Tasks spawned by the host are ready together, at priority
 * 0, in spawn order. A task that returns ASX_E_PENDING without parking
 * yields: it is ready again at the back of the line. */

#define RACE_MAX 8u

typedef struct {
    uint32_t order[RACE_MAX]; /* task index of each poll, in poll order */
    uint32_t polls;
    asx_task_id ids[RACE_MAX];
    asx_dispatch_record dispatches[RACE_MAX];
    uint32_t recorded;
    uint64_t certificate;
    uint64_t decisions;
} race_run;

typedef struct {
    race_run *run;
    uint32_t index;
    uint32_t polls_left; /* polls before the task completes; it yields until then */
} race_task;

static asx_status poll_race(void *user_data, asx_task_id self) {
    race_task *t = (race_task *)user_data;
    (void)self;
    if (t->run->polls < RACE_MAX) t->run->order[t->run->polls] = t->index;
    t->run->polls++;
    if (t->polls_left > 1u) {
        t->polls_left--;
        return ASX_E_PENDING;
    }
    return ASX_OK;
}

/* `tasks` tasks in a new region of an initialized lab, spawned in index
 * order, each polled `polls_each` times, run until idle. */
static asx_status race_on(asx_lab *lab, uint32_t tasks, uint32_t polls_each, race_run *out) {
    race_task jobs[RACE_MAX];
    asx_region_id region;
    asx_budget budget;
    asx_status st;
    uint32_t i;

    memset(out, 0, sizeof(*out));
    memset(jobs, 0, sizeof(jobs));
    st = asx_lab_open_region(lab, &region);
    for (i = 0; st == ASX_OK && i < tasks; i++) {
        jobs[i].run = out;
        jobs[i].index = i;
        jobs[i].polls_left = polls_each;
        st = asx_task_spawn(region, poll_race, &jobs[i], &out->ids[i]);
    }
    if (st != ASX_OK) return st;
    asx_scheduler_record_dispatches(out->dispatches, RACE_MAX);
    budget = asx_budget_from_polls(100u);
    st = asx_lab_run(lab, region, &budget);
    out->recorded = asx_scheduler_dispatches_recorded();
    out->certificate = asx_scheduler_schedule_certificate();
    out->decisions = asx_scheduler_schedule_decisions();
    asx_scheduler_record_dispatches(NULL, 0u);
    return st;
}

static asx_status race(uint64_t seed, uint8_t scheduler, uint32_t tasks, uint32_t polls_each,
                       race_run *out) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_status st;

    asx_lab_config_init(&cfg);
    cfg.seed = seed;
    cfg.scheduler = scheduler;
    st = asx_lab_init(&lab, &cfg);
    if (st != ASX_OK) return st;
    st = race_on(&lab, tasks, polls_each, out);
    asx_lab_shutdown(&lab);
    return st;
}

/* The recorded dispatches are the polls, in order: the task polled, from
 * the ready lane, at lab step i + 1 and time 0. Returns 1 when they are. */
static int dispatches_are_the_polls(const race_run *r) {
    uint32_t i;
    if (r->recorded != r->polls || r->polls > RACE_MAX) return 0;
    for (i = 0; i < r->polls; i++) {
        const asx_dispatch_record *d = &r->dispatches[i];
        if (asx_handle_index(d->task) != asx_handle_index(r->ids[r->order[i]])) return 0;
        if (d->lane != ASX_DISPATCH_LANE_READY || d->step != (uint64_t)(i + 1u) || d->at != 0u) {
            return 0;
        }
    }
    return 1;
}

/* Four tasks race. Seed 1 polls them 1,3,2,0 and seed 2 polls 2,0,1,3 (the
 * xorshift64 draws of those seeds, reduced by the ready count at each
 * step); the same seed polls them in the same order again, with the same
 * Rust schedule certificate. Before bd-9kll.9.5 the seed never reached the
 * dispatch and every seed polled them 0,1,2,3. */
TEST(lab_run_dispatches_in_the_seeds_order) {
    static const uint32_t seed_1[4] = {1u, 3u, 2u, 0u};
    static const uint32_t seed_2[4] = {2u, 0u, 1u, 3u};
    race_run a;
    race_run b;
    race_run again;
    uint32_t i;

    ASSERT_EQ(race(1u, ASX_LAB_SCHEDULER_DISPATCH, 4u, 1u, &a), ASX_OK);
    ASSERT_EQ(race(2u, ASX_LAB_SCHEDULER_DISPATCH, 4u, 1u, &b), ASX_OK);
    ASSERT_EQ(a.polls, 4u);
    ASSERT_EQ(b.polls, 4u);
    for (i = 0; i < 4u; i++) {
        ASSERT_EQ(a.order[i], seed_1[i]);
        ASSERT_EQ(b.order[i], seed_2[i]);
    }
    ASSERT_TRUE(dispatches_are_the_polls(&a));
    ASSERT_TRUE(dispatches_are_the_polls(&b));
    ASSERT_EQ(a.decisions, (uint64_t)4u);
    ASSERT_TRUE(a.certificate != 0u);
    ASSERT_TRUE(a.certificate != b.certificate);

    ASSERT_EQ(race(1u, ASX_LAB_SCHEDULER_DISPATCH, 4u, 1u, &again), ASX_OK);
    ASSERT_EQ(again.polls, 4u);
    for (i = 0; i < 4u; i++) { ASSERT_EQ(again.order[i], a.order[i]); }
    ASSERT_EQ(again.certificate, a.certificate);
}

static asx_status step_race(asx_lab *lab, void *user_data) {
    return race_on(lab, 4u, 1u, (race_run *)user_data);
}

/* asx_lab_run_scenario runs its steps on the same seeded dispatch: the race
 * inside a scenario step is polled in seed 1's order, and the step's polls
 * are counted. */
TEST(lab_run_scenario_dispatches_in_the_seeds_order) {
    static const uint32_t seed_1[4] = {1u, 3u, 2u, 0u};
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result result;
    race_run r;
    uint32_t i;

    memset(&r, 0, sizeof(r));
    asx_lab_config_init(&cfg);
    cfg.seed = 1u;
    MUST_OK(asx_lab_init(&lab, &cfg));
    asx_trace_reset();
    asx_lab_scenario_init(&sc, "race");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_race, &r));

    ASSERT_EQ(asx_lab_run_scenario(&lab, &sc, &result), ASX_OK);
    ASSERT_EQ(result.steps_completed, 1u);
    ASSERT_EQ(result.polls_total, (uint64_t)4u);
    ASSERT_EQ(r.polls, 4u);
    for (i = 0; i < 4u; i++) { ASSERT_EQ(r.order[i], seed_1[i]); }
    asx_lab_shutdown(&lab);
}

/* The sweep scheduler of a live host, selected by config, polls every
 * runnable task each round in arena order whatever the seed, and the lab's
 * dispatcher records nothing. */
TEST(sweep_scheduler_ignores_the_seed) {
    race_run a;
    race_run b;
    uint32_t i;

    ASSERT_EQ(race(1u, ASX_LAB_SCHEDULER_SWEEP, 4u, 1u, &a), ASX_OK);
    ASSERT_EQ(race(2u, ASX_LAB_SCHEDULER_SWEEP, 4u, 1u, &b), ASX_OK);
    ASSERT_EQ(a.polls, 4u);
    ASSERT_EQ(b.polls, 4u);
    for (i = 0; i < 4u; i++) {
        ASSERT_EQ(a.order[i], i);
        ASSERT_EQ(b.order[i], i);
    }
    ASSERT_EQ(a.recorded, 0u);
    ASSERT_EQ(b.recorded, 0u);
}

/* fixtures/rust_reference_v2/scheduler-yield-interleave-001.json through the
 * public harness: seed 7, two tasks (t.a, t.b, spawned in that order) that
 * trace, yield and trace again. Rust's capture dispatches
 *   1@0 t.b ready, 2@0 t.a ready, 3@0 t.a ready, 4@0 t.b ready
 * with certificate_hash 2d915a97cee457d3 over 4 decisions; the C lab
 * dispatches in that order and folds the same certificate. */
TEST(lab_run_dispatches_in_a_fixtures_recorded_order) {
    static const uint32_t recorded[4] = {1u, 0u, 0u, 1u}; /* t.b, t.a, t.a, t.b */
    race_run r;
    uint32_t i;

    ASSERT_EQ(race(7u, ASX_LAB_SCHEDULER_DISPATCH, 2u, 2u, &r), ASX_OK);
    ASSERT_EQ(r.polls, 4u);
    for (i = 0; i < 4u; i++) { ASSERT_EQ(r.order[i], recorded[i]); }
    ASSERT_TRUE(dispatches_are_the_polls(&r));
    ASSERT_EQ(r.decisions, (uint64_t)4u);
    ASSERT_EQ(r.certificate, UINT64_C(0x2d915a97cee457d3));
}

/* A lab reset restarts the dispatch from the seed: the step counter and the
 * draws begin again, so the same tasks are polled in the same order. */
TEST(lab_reset_restarts_the_dispatch_from_the_seed) {
    asx_lab lab;
    asx_lab_config cfg;
    race_run first;
    race_run second;
    uint32_t i;

    asx_lab_config_init(&cfg);
    cfg.seed = 1u;
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(race_on(&lab, 4u, 1u, &first), ASX_OK);
    ASSERT_EQ(first.polls, 4u);

    ASSERT_EQ(asx_lab_reset(&lab), ASX_OK);
    ASSERT_EQ(race_on(&lab, 4u, 1u, &second), ASX_OK);
    ASSERT_EQ(second.polls, 4u);
    for (i = 0; i < 4u; i++) { ASSERT_EQ(second.order[i], first.order[i]); }
    ASSERT_TRUE(dispatches_are_the_polls(&second));
    ASSERT_EQ(second.certificate, first.certificate);
    asx_lab_shutdown(&lab);
}

TEST(replay_determinism) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_scenario sc;
    asx_lab_result r1, r2;

    asx_lab_config_init(&cfg);
    cfg.seed = 12345;

    asx_lab_scenario_init(&sc, "replay");
    MUST_OK(asx_lab_scenario_add_step(&sc, step_advance_5, NULL));
    MUST_OK(asx_lab_scenario_add_step(&sc, step_open_close_region, NULL));

    /* Run 1 */
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_run_scenario(&lab, &sc, &r1));
    asx_lab_shutdown(&lab);

    /* Run 2 */
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_run_scenario(&lab, &sc, &r2));
    asx_lab_shutdown(&lab);

    /* Identical results */
    ASSERT_EQ(r1.steps_completed, r2.steps_completed);
    ASSERT_EQ(r1.elapsed_ns, r2.elapsed_ns);
    ASSERT_EQ(r1.last_status, r2.last_status);
}

/* ------------------------------------------------------------------ */
/* Convenience API tests                                               */
/* ------------------------------------------------------------------ */

TEST(open_region) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_region_id rid;

    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    MUST_OK(asx_lab_open_region(&lab, &rid));
    ASSERT_TRUE(asx_handle_is_valid(rid));
    asx_lab_shutdown(&lab);
}

TEST(open_region_null_fails) {
    asx_lab lab;
    asx_lab_config cfg;
    asx_lab_config_init(&cfg);
    MUST_OK(asx_lab_init(&lab, &cfg));
    ASSERT_EQ(asx_lab_open_region(&lab, NULL), ASX_E_INVALID_ARGUMENT);
    asx_lab_shutdown(&lab);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stderr, "=== test_lab ===\n");

    RUN_TEST(config_init_defaults);

    RUN_TEST(init_shutdown);
    RUN_TEST(init_null_fails);
    RUN_TEST(init_null_config_fails);
    RUN_TEST(init_refuses_an_unknown_scheduler);
    RUN_TEST(shutdown_null_safe);
    RUN_TEST(double_shutdown_safe);

    RUN_TEST(time_starts_at_zero);
    RUN_TEST(time_starts_at_custom);
    RUN_TEST(advance_time);
    RUN_TEST(time_integrates_with_runtime);

    RUN_TEST(entropy_deterministic);
    RUN_TEST(entropy_different_seeds);

    RUN_TEST(reset_restores_state);
    RUN_TEST(reset_restores_runtime_hooks);

    RUN_TEST(scenario_empty);
    RUN_TEST(scenario_single_step);
    RUN_TEST(scenario_multi_step);
    RUN_TEST(scenario_stops_on_failure);
    RUN_TEST(scenario_with_region_ops);
    RUN_TEST(scenario_tracks_scheduler_polls);
    RUN_TEST(scenario_enforces_max_polls_per_step);
    RUN_TEST(scenario_null_step_fails_closed);
    RUN_TEST(scenario_corrupt_step_count_fails_closed);
    RUN_TEST(scenario_overflow);

    RUN_TEST(run_without_auto_advance_keeps_the_clock);
    RUN_TEST(run_with_auto_advance_completes_sleepers);
    RUN_TEST(lab_run_dispatches_in_the_seeds_order);
    RUN_TEST(lab_run_scenario_dispatches_in_the_seeds_order);
    RUN_TEST(sweep_scheduler_ignores_the_seed);
    RUN_TEST(lab_run_dispatches_in_a_fixtures_recorded_order);
    RUN_TEST(lab_reset_restarts_the_dispatch_from_the_seed);
    RUN_TEST(replay_determinism);

    RUN_TEST(open_region);
    RUN_TEST(open_region_null_fails);

    TEST_REPORT();
    return test_failures;
}

#else

TEST(lab_family_compile_time_hidden_in_minimal_browser) {
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE, 0);
    ASSERT_EQ(ASX_HAS_BROWSER_TRACE_SUBPROFILE_SPLIT, 1);
}

int main(void) {
    fprintf(stderr, "=== test_lab (minimal browser hidden contract) ===\n");
    RUN_TEST(lab_family_compile_time_hidden_in_minimal_browser);
    TEST_REPORT();
    return test_failures;
}

#endif
