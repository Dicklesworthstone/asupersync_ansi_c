/*
 * lane_selection_harness.c — CBMC harness for the lab's lane selection
 * (asx_lab_pick, src/runtime/lab_dispatch.c; bd-9kll.4.2)
 *
 * Rust's lab scheduler (scheduler/three_lane.rs, priority.rs) serves two
 * lanes, cancel and ready, and the C replica must serve them the same way:
 *
 *   1. The cancel lane is served while the streak of cancel picks is below
 *      LAB_CANCEL_STREAK_LIMIT (16) and the lane is not empty.
 *   2. Otherwise the ready lane is served, which resets the streak.
 *   3. Otherwise (the ready lane is empty) the cancel lane is served again
 *      and the streak restarts at 1.
 *   4. Within a lane the entries of the highest priority form the group, in
 *      generation order; the step's value r picks entry r % n of it.
 *   5. An entry whose task is no longer scheduled (it ran from the other
 *      lane) is dropped and the pick retried with the same r.
 *
 * The harness drives the real lanes with up to three tasks, each in no lane,
 * the ready lane, the cancel lane or both, at priorities 0 to 2, and checks
 * every pick of a run against an independent model of the five rules above:
 * the same task, the same lane, the same scheduled count. A second scenario
 * keeps a cancel storm going against one ready entry for 40 picks and checks
 * the streak rule on its own: the ready entry is served exactly at the 17th
 * pick.
 *
 * Under CBMC (-DCBMC) the configuration and the step values are
 * nondeterministic; standalone it enumerates the bounded space.
 *
 * SPDX-License-Identifier: MIT
 */

/* ASX_CHECKPOINT_WAIVER_FILE("CBMC harness — formal verification only") */

#include "../../../src/runtime/runtime_internal.h"
#include "cbmc_compat.h"
#include <asx/asx.h>
#include <stdio.h>
#include <string.h>

#define HARNESS_TASKS 3u
#define HARNESS_PRIOS 3u
#define HARNESS_PICKS (2u * HARNESS_TASKS + 1u)
#define STORM_PICKS 40u

#define LANE_NONE 0u
#define LANE_READY 1u
#define LANE_CANCEL 2u
#define LANE_BOTH 3u

static int passes = 0;
static int failures = 0;

static asx_runtime g_rt;

/* Why the last failing check failed, for the report. */
static const char *g_why = "";
#define REFUSE(why)                                                                                \
    do {                                                                                           \
        g_why = (why);                                                                             \
        return 0;                                                                                  \
    } while (0)

static asx_status poll_noop(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

/* ---- the model: rules 1 to 5, written from the contract ---------------- */

typedef struct {
    uint8_t task;
    uint8_t prio;
} m_entry;

typedef struct {
    m_entry e[2u * HARNESS_TASKS + 2u];
    uint32_t n;
} m_lane;

static m_lane m_cancel;
static m_lane m_ready;
static int m_scheduled[HARNESS_TASKS];
static uint32_t m_streak;

static void m_push(m_lane *l, uint32_t task, uint8_t prio) {
    l->e[l->n].task = (uint8_t)task;
    l->e[l->n].prio = prio;
    l->n++;
}

/* Rule 4 and 5. */
static int m_pop(m_lane *l, uint64_t r, uint32_t *task) {
    for (;;) {
        uint32_t group[2u * HARNESS_TASKS + 2u];
        uint32_t n = 0;
        uint32_t k;
        uint8_t top = 0;
        m_entry e;

        if (l->n == 0u) return 0;
        for (k = 0; k < l->n; k++) {
            if (l->e[k].prio > top) top = l->e[k].prio;
        }
        for (k = 0; k < l->n; k++) {
            if (l->e[k].prio == top) group[n++] = k;
        }
        e = l->e[group[(uint32_t)(r % (uint64_t)n)]];
        for (k = group[(uint32_t)(r % (uint64_t)n)]; k + 1u < l->n; k++) l->e[k] = l->e[k + 1u];
        l->n--;
        if (m_scheduled[e.task]) {
            m_scheduled[e.task] = 0;
            *task = e.task;
            return 1;
        }
    }
}

/* Rules 1 to 3. */
static int m_pick(uint64_t r, uint32_t *task, int *cancel_lane) {
    *cancel_lane = 0;
    if (m_streak < 16u && m_pop(&m_cancel, r, task)) {
        m_streak++;
        *cancel_lane = 1;
        return 1;
    }
    if (m_pop(&m_ready, r, task)) {
        m_streak = 0;
        return 1;
    }
    if (m_pop(&m_cancel, r, task)) {
        m_streak = 1;
        *cancel_lane = 1;
        return 1;
    }
    m_streak = 0;
    return 0;
}

static uint32_t m_scheduled_count(void) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < HARNESS_TASKS; i++) n += m_scheduled[i] ? 1u : 0u;
    return n;
}

/* ---- the implementation under test ------------------------------------- */

typedef struct {
    asx_task_id id[HARNESS_TASKS];
    asx_task_slot *slot[HARNESS_TASKS];
} fixture;

static int fixture_open(fixture *f) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_region_id region;
    uint32_t i;

    if (asx_runtime_is_initialized(&g_rt)) asx_runtime_shutdown(&g_rt);
    asx_runtime_config_init(&cfg);
    if (asx_runtime_hooks_init(&hooks) != ASX_OK) REFUSE("hooks init");
    if (asx_runtime_init(&g_rt, &cfg, &hooks) != ASX_OK) REFUSE("runtime init");
    if (asx_scheduler_use_lab_dispatch(42u) != ASX_OK) REFUSE("use_lab_dispatch");
    if (asx_region_open(&g_rt, &region) != ASX_OK) REFUSE("region open");
    for (i = 0; i < HARNESS_TASKS; i++) {
        if (asx_task_spawn(region, poll_noop, NULL, &f->id[i]) != ASX_OK) REFUSE("task spawn");
        if (asx_task_slot_lookup(f->id[i], &f->slot[i]) != ASX_OK) REFUSE("task lookup");
    }
    /* A spawn schedules its task on the ready lane. Serve those entries so
     * the lanes start empty and the configuration below is all there is. */
    {
        uint32_t slot = 0;
        uint16_t gen = 0;
        int cancel = 0;
        for (i = 0; i < HARNESS_TASKS + 1u && asx_lab_pick(0u, &slot, &gen, &cancel); i++) {}
        if (asx_lab_scheduled_count() != 0u) REFUSE("tasks are still scheduled after the spawns");
    }
    memset(&m_cancel, 0, sizeof(m_cancel));
    memset(&m_ready, 0, sizeof(m_ready));
    memset(m_scheduled, 0, sizeof(m_scheduled));
    m_streak = 0;
    return 1;
}

static int task_index(const fixture *f, uint32_t slot) {
    uint32_t i;
    asx_task_id id = asx_task_handle_for_slot(g_kernels[0], slot);
    for (i = 0; i < HARNESS_TASKS; i++) {
        if (f->id[i] == id) return (int)i;
    }
    return -1;
}

static void schedule_ready(fixture *f, uint32_t i, uint8_t prio) {
    asx_lab_schedule(f->slot[i], prio);
    m_scheduled[i] = 1;
    m_push(&m_ready, i, prio);
}

static void schedule_cancel(fixture *f, uint32_t i, uint8_t prio) {
    asx_lab_schedule_cancel(f->slot[i], prio);
    m_scheduled[i] = 1;
    m_push(&m_cancel, i, prio);
}

/* One pick of the implementation against the model: 1 when they agree. */
static int pick_agrees(fixture *f, uint64_t r) {
    uint32_t slot = 0;
    uint16_t gen = 0;
    int cancel = 0;
    int found;
    uint32_t mtask = 0;
    int mcancel = 0;
    int mfound = m_pick(r, &mtask, &mcancel);

    found = asx_lab_pick(r, &slot, &gen, &cancel);
    if (found != mfound) REFUSE("a pick was found by one side only");
    if (found) {
        if (cancel != mcancel) REFUSE("the lanes differ");
        if (task_index(f, slot) != (int)mtask) REFUSE("the tasks differ");
    }
    if (asx_lab_scheduled_count() != m_scheduled_count()) REFUSE("scheduled counts differ");
    return 1;
}

/* One configuration: lane membership and priorities of the tasks, and the
 * step values r, r+1, ... of the picks. Returns 1 when every pick agrees. */
static int run_config(const unsigned lane[HARNESS_TASKS], const unsigned prio[HARNESS_TASKS],
                      uint64_t r) {
    fixture f;
    uint32_t i;
    uint32_t k;

    if (!fixture_open(&f)) return 0;
    for (i = 0; i < HARNESS_TASKS; i++) {
        if ((lane[i] & LANE_READY) != 0u) schedule_ready(&f, i, (uint8_t)prio[i]);
        if ((lane[i] & LANE_CANCEL) != 0u) schedule_cancel(&f, i, (uint8_t)prio[i]);
    }
    if (asx_lab_dispatch_overflowed()) REFUSE("a lane overflowed");
    if (asx_lab_scheduled_count() != m_scheduled_count())
        REFUSE("scheduled counts differ at start");
    for (k = 0; k < HARNESS_PICKS; k++) {
        if (!pick_agrees(&f, r + (uint64_t)k)) return 0;
    }
    /* Everything scheduled has been served or dropped, and nothing is left. */
    if (asx_lab_scheduled_count() != 0u) REFUSE("entries left after the picks");
    return 1;
}

/* A cancel storm against one ready entry: the cancel task is pushed again
 * after every cancel pick, so the cancel lane is never empty. Rule 1 and 2:
 * picks 1 to 16 are cancel picks, pick 17 is the ready entry, and (rule 3
 * having nothing to fall back on but the cancel lane) every later pick is a
 * cancel pick again. */
static int run_storm(uint64_t r) {
    fixture f;
    uint32_t k;
    int ready_at = -1;

    if (!fixture_open(&f)) return 0;
    schedule_cancel(&f, 0u, 0u);
    schedule_ready(&f, 1u, 0u);
    for (k = 0; k < STORM_PICKS; k++) {
        uint32_t slot = 0;
        uint16_t gen = 0;
        int cancel = 0;
        int found = asx_lab_pick(r + (uint64_t)k, &slot, &gen, &cancel);
        uint32_t mtask = 0;
        int mcancel = 0;
        int mfound = m_pick(r + (uint64_t)k, &mtask, &mcancel);

        if (!found || !mfound || cancel != mcancel || task_index(&f, slot) != (int)mtask) {
            REFUSE("storm: the implementation and the model part");
        }
        if (!cancel) {
            if (ready_at >= 0) REFUSE("storm: the ready entry was served twice");
            ready_at = (int)k;
        } else {
            schedule_cancel(&f, 0u, 0u);
        }
    }
    /* The 17th pick, index 16. */
    if (ready_at != 16) REFUSE("storm: the ready entry was not the 17th pick");
    return 1;
}

#ifdef CBMC

int main(void) {
    unsigned lane[HARNESS_TASKS];
    unsigned prio[HARNESS_TASKS];
    uint64_t r = NONDET_UINT64();
    unsigned i;

    for (i = 0; i < HARNESS_TASKS; i++) {
        lane[i] = NONDET_UINT();
        prio[i] = NONDET_UINT();
        ASSUME(lane[i] <= LANE_BOTH);
        ASSUME(prio[i] < HARNESS_PRIOS);
    }
    VERIFY(run_config(lane, prio, r));
    VERIFY(run_storm(r));
    return 0;
}

#else

int main(void) {
    unsigned lane[HARNESS_TASKS];
    unsigned prio[HARNESS_TASKS];
    unsigned lanes = 1;
    unsigned prios = 1;
    unsigned c;
    unsigned p;
    unsigned i;
    unsigned rb;

    fprintf(stderr, "[lane-selection-harness] CBMC-compatible verification\n");
    for (i = 0; i < HARNESS_TASKS; i++) {
        lanes *= 4u;
        prios *= HARNESS_PRIOS;
    }

    for (c = 0; c < lanes; c++) {
        unsigned t = c;
        for (i = 0; i < HARNESS_TASKS; i++) {
            lane[i] = t % 4u;
            t /= 4u;
        }
        for (p = 0; p < prios; p++) {
            unsigned u = p;
            for (i = 0; i < HARNESS_TASKS; i++) {
                prio[i] = u % HARNESS_PRIOS;
                u /= HARNESS_PRIOS;
            }
            for (rb = 0; rb < 5u; rb++) {
                if (run_config(lane, prio, (uint64_t)rb)) {
                    passes++;
                } else {
                    failures++;
                    fprintf(stderr, "  FAIL: lanes %u%u%u priorities %u%u%u r %u: %s\n", lane[0],
                            lane[1], lane[2], prio[0], prio[1], prio[2], rb, g_why);
                    if (failures > 20) return 1;
                }
            }
        }
    }
    for (rb = 0; rb < 8u; rb++) {
        if (run_storm((uint64_t)rb)) {
            passes++;
        } else {
            failures++;
            fprintf(stderr, "  FAIL: cancel storm, r %u: %s\n", rb, g_why);
        }
    }
    /* A harness that checked nothing would pass: the space is what it is. */
    if (passes + failures != (int)(lanes * prios * 5u + 8u)) {
        fprintf(stderr, "  FAIL: the harness did not cover its space\n");
        failures++;
    }

    fprintf(stderr, "[lane-selection-harness] %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}

#endif /* CBMC */
