/*
 * lab_dispatch.c — the Rust LabRuntime's dispatch model (bd-9kll.4.2)
 *
 * With lab dispatch on (asx_scheduler_use_lab_dispatch), the scheduler
 * polls one task per step, chosen as asupersync's LabRuntime chooses it
 * with one worker (asupersync 5e60b1c4c; LR = src/lab/runtime.rs, PR =
 * src/runtime/scheduler/priority.rs):
 *
 *   - A seeded xorshift64 (src/util/det_rng.rs) draws one value per step.
 *   - Tasks wait in a cancel lane and a ready lane of entries {task,
 *     priority, generation}. A lane yields its highest priority, lowest
 *     generation first (PR:28-41); among the entries sharing the highest
 *     priority (at most 256, in generation order) the step's value picks
 *     entry `r % n` (pop_entry_with_rng, PR:759-790; tie_break_index,
 *     PR:417-422).
 *   - An entry is live only while its task is scheduled. Promoting a task
 *     to the cancel lane leaves its older entries in place, and a picked
 *     entry whose task is no longer scheduled is dropped and the pick
 *     retried with the same value (PR:674-685, 861-887). Such leftover
 *     entries come back to life when their task is scheduled again.
 *   - The cancel lane goes first for up to 16 consecutive dispatches, then
 *     the ready lane, then the cancel lane again (LR:6324-6373).
 *   - schedule() of a task already scheduled does nothing; every
 *     schedule_cancel() pushes a cancel entry (LR:6176-6239). Generations
 *     come from one counter across both lanes.
 *
 * Bounded static storage: a lane holds LAB_LANE_CAP entries. Overflow is
 * sticky and makes the scheduler fail the run with
 * ASX_E_RESOURCE_EXHAUSTED rather than drop an entry.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/core/transition.h>
#include <asx/runtime/runtime.h>
#include <string.h>

#define LAB_LANE_CAP (8u * (uint32_t)ASX_MAX_TASKS)
#define LAB_GROUP_CAP 256u          /* scratch capacity, PR:425-446 */
#define LAB_CANCEL_STREAK_LIMIT 16u /* DEFAULT_LAB_CANCEL_STREAK_LIMIT, LR:6051 */

typedef struct {
    uint32_t slot;
    uint16_t task_gen; /* the slot generation of the task it was pushed for */
    uint8_t priority;
    uint64_t gen;
} lab_entry;

/* Entries in push order, which is generation order: removing keeps it. */
typedef struct {
    lab_entry e[LAB_LANE_CAP];
    uint32_t n;
} lab_lane;

static int g_lab_active = 0;
static uint64_t g_lab_rng = 0;
static uint64_t g_lab_next_gen = 0;
static uint32_t g_lab_streak = 0;
static uint32_t g_lab_scheduled = 0;
static int g_lab_overflow = 0;
static lab_lane g_lab_cancel;
static lab_lane g_lab_ready;

/* Children spawned from inside a poll (Rust cx.spawn) wait for admission
 * at the start of the next step (LR:3957-4012). */
static uint32_t g_lab_admit[ASX_MAX_TASKS];
static uint32_t g_lab_admit_n = 0;

/* Cancel wakes held back while a region cancel visits its tasks: Rust
 * schedules every task's cancel first and dispatches the cancel wakers
 * after (state.rs:7811-7876, run.rs driver). */
static uint32_t g_lab_wake[ASX_MAX_TASKS];
static uint32_t g_lab_wake_n = 0;
static uint32_t g_lab_batch = 0;

void asx_lab_dispatch_reset_internal(void) {
    g_lab_active = 0;
    g_lab_rng = 0;
    g_lab_next_gen = 0;
    g_lab_streak = 0;
    g_lab_scheduled = 0;
    g_lab_overflow = 0;
    g_lab_cancel.n = 0;
    g_lab_ready.n = 0;
    g_lab_admit_n = 0;
    g_lab_wake_n = 0;
    g_lab_batch = 0;
}

int asx_lab_dispatch_active(void) { return g_lab_active; }

int asx_lab_dispatch_overflowed(void) { return g_lab_overflow; }

asx_status asx_scheduler_use_lab_dispatch(uint64_t seed) {
    uint32_t i;
    for (i = 0; i < g_task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_task_count <= ASX_MAX_TASKS");
        if (g_tasks[i].alive) return ASX_E_INVALID_STATE;
    }
    asx_lab_dispatch_reset_internal();
    /* DetRng::new (det_rng.rs:63-68, 129-158): seed 0 becomes 1; the
     * degenerate seeds are offset by the golden-ratio constant. */
    if (seed == 0u) {
        seed = 1u;
    } else if (seed == UINT64_C(0xFFFFFFFFFFFFFFFF) || seed == UINT64_C(0x00000000FFFFFFFF) ||
               seed == UINT64_C(0xFFFFFFFF00000000) || seed == UINT64_C(0x5555555555555555) ||
               seed == UINT64_C(0xAAAAAAAAAAAAAAAA)) {
        seed += UINT64_C(0x9E3779B97F4A7C15);
    }
    g_lab_rng = seed;
    g_lab_active = 1;
    return ASX_OK;
}

uint64_t asx_lab_rng_next(void) {
    /* xorshift64 (det_rng.rs:267-276) */
    uint64_t x = g_lab_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_lab_rng = x;
    return x;
}

uint32_t asx_lab_scheduled_count(void) { return g_lab_scheduled; }

int asx_lab_is_scheduled(const asx_task_slot *t) { return t->lab_scheduled != 0u; }

static void lab_push(lab_lane *lane, const asx_task_slot *t, uint8_t priority) {
    lab_entry *e;
    if (lane->n >= LAB_LANE_CAP) {
        g_lab_overflow = 1;
        return;
    }
    e = &lane->e[lane->n++];
    e->slot = (uint32_t)(t - g_tasks);
    e->task_gen = t->generation;
    e->priority = priority;
    e->gen = g_lab_next_gen++;
}

/* The task is now scheduled: it will be dispatched, so it neither stays
 * parked nor parks at the end of a poll it is in. */
static void lab_mark_scheduled(asx_task_slot *t) {
    if (t->lab_scheduled) return;
    t->lab_scheduled = 1u;
    g_lab_scheduled++;
    if (t->in_poll) {
        t->notified = 1;
    } else {
        t->parked = 0;
    }
}

void asx_lab_schedule(asx_task_slot *t, uint8_t priority) {
    if (t == NULL || !t->alive || asx_task_is_terminal(t->state)) return;
    if (t->lab_scheduled) return;
    lab_mark_scheduled(t);
    lab_push(&g_lab_ready, t, priority);
}

void asx_lab_schedule_cancel(asx_task_slot *t, uint8_t priority) {
    if (t == NULL || !t->alive || asx_task_is_terminal(t->state)) return;
    lab_mark_scheduled(t);
    lab_push(&g_lab_cancel, t, priority);
}

static int lab_entry_live(const lab_entry *e) {
    const asx_task_slot *t;
    if (e->slot >= g_task_count) return 0;
    t = &g_tasks[e->slot];
    return t->alive && t->generation == e->task_gen && t->lab_scheduled;
}

static void lab_remove_at(lab_lane *lane, uint32_t k) {
    if (k + 1u < lane->n) {
        memmove(&lane->e[k], &lane->e[k + 1u], (size_t)(lane->n - k - 1u) * sizeof(lab_entry));
    }
    lane->n--;
}

/* pop_cancel / pop_ready (PR:674-685, 985-998): the group of entries at the
 * highest priority, in generation order and capped at 256, yields entry
 * r % n; a dead one is dropped and the pick retried with the same r. */
static int lab_pop(lab_lane *lane, uint64_t r, uint32_t *out_slot) {
    uint32_t group[LAB_GROUP_CAP];
    for (;;) {
        uint32_t k;
        uint32_t n = 0;
        uint8_t top = 0;
        uint32_t chosen;
        lab_entry e;
        ASX_CHECKPOINT_WAIVER("bounded: each pass removes one entry of a finite lane");
        if (lane->n == 0u) return 0;
        for (k = 0; k < lane->n; k++) {
            ASX_CHECKPOINT_WAIVER("bounded: lane length <= LAB_LANE_CAP");
            if (lane->e[k].priority > top) top = lane->e[k].priority;
        }
        for (k = 0; k < lane->n && n < LAB_GROUP_CAP; k++) {
            ASX_CHECKPOINT_WAIVER("bounded: lane length <= LAB_LANE_CAP");
            if (lane->e[k].priority == top) group[n++] = k;
        }
        chosen = group[(uint32_t)(r % (uint64_t)n)];
        e = lane->e[chosen];
        lab_remove_at(lane, chosen);
        if (lab_entry_live(&e)) {
            g_tasks[e.slot].lab_scheduled = 0u;
            g_lab_scheduled--;
            *out_slot = e.slot;
            return 1;
        }
    }
}

int asx_lab_pick(uint64_t r, uint32_t *out_slot, int *out_cancel_lane) {
    *out_cancel_lane = 0;
    if (g_lab_streak < LAB_CANCEL_STREAK_LIMIT && lab_pop(&g_lab_cancel, r, out_slot)) {
        g_lab_streak++;
        *out_cancel_lane = 1;
        return 1;
    }
    /* The timed lane is never fed by the lab at the pinned rev (LR:7658). */
    if (lab_pop(&g_lab_ready, r, out_slot)) {
        g_lab_streak = 0;
        return 1;
    }
    if (lab_pop(&g_lab_cancel, r, out_slot)) {
        g_lab_streak = 1;
        *out_cancel_lane = 1;
        return 1;
    }
    g_lab_streak = 0;
    return 0;
}

static void lab_purge(lab_lane *lane, uint32_t slot, uint16_t task_gen) {
    uint32_t k = 0;
    while (k < lane->n) {
        ASX_CHECKPOINT_WAIVER("bounded: lane length <= LAB_LANE_CAP");
        if (lane->e[k].slot == slot && lane->e[k].task_gen == task_gen) {
            lab_remove_at(lane, k);
        } else {
            k++;
        }
    }
}

/* forget_task (LR:6495-6509, PR:828-835): a completed task that is still
 * scheduled loses every entry; one that is not keeps its leftovers. */
void asx_lab_forget(asx_task_slot *t) {
    uint32_t slot = (uint32_t)(t - g_tasks);
    if (!t->lab_scheduled) return;
    lab_purge(&g_lab_cancel, slot, t->generation);
    lab_purge(&g_lab_ready, slot, t->generation);
    t->lab_scheduled = 0u;
    g_lab_scheduled--;
}

void asx_lab_defer_admission(asx_task_slot *t) {
    if (g_lab_admit_n >= ASX_MAX_TASKS) {
        g_lab_overflow = 1;
        return;
    }
    t->lab_admission_pending = 1u;
    g_lab_admit[g_lab_admit_n++] = (uint32_t)(t - g_tasks);
}

int asx_lab_admissions_pending(void) { return g_lab_admit_n > 0u; }

/* drain_spawn_admissions (LR:3957-4012, state.rs:5212-5217): FIFO, each
 * scheduled at its budget priority. */
void asx_lab_admit_pending(void) {
    uint32_t i;
    uint32_t n = g_lab_admit_n;
    g_lab_admit_n = 0;
    for (i = 0; i < n; i++) {
        asx_task_slot *t = &g_tasks[g_lab_admit[i]];
        ASX_CHECKPOINT_WAIVER("bounded: admissions <= ASX_MAX_TASKS");
        if (!t->alive || !t->lab_admission_pending) continue;
        t->lab_admission_pending = 0u;
        asx_lab_schedule(t, t->budget.priority);
    }
}

/* A cancel request reaches a task's CancelTaskWaker, which schedules it on
 * the cancel lane at the priority of its last poll (LR:6547-6553). Only a
 * task polled at least once has one. */
void asx_lab_cancel_wake(asx_task_slot *t) {
    if (!t->first_polled) return;
    if (g_lab_batch > 0u) {
        if (g_lab_wake_n >= ASX_MAX_TASKS) {
            g_lab_overflow = 1;
            return;
        }
        g_lab_wake[g_lab_wake_n++] = (uint32_t)(t - g_tasks);
        return;
    }
    asx_lab_schedule_cancel(t, t->lab_waker_prio);
}

void asx_lab_cancel_batch_begin(void) { g_lab_batch++; }

void asx_lab_cancel_batch_end(void) {
    uint32_t i;
    uint32_t n;
    if (g_lab_batch == 0u) return;
    g_lab_batch--;
    if (g_lab_batch > 0u) return;
    n = g_lab_wake_n;
    g_lab_wake_n = 0;
    for (i = 0; i < n; i++) {
        asx_task_slot *t = &g_tasks[g_lab_wake[i]];
        ASX_CHECKPOINT_WAIVER("bounded: wakes <= ASX_MAX_TASKS");
        asx_lab_schedule_cancel(t, t->lab_waker_prio);
    }
}
