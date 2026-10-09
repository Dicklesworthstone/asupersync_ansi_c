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

/* Region commands tasks queued (Rust RegionCommand, LR:4135-4217): a
 * Create mints a child of `region` for its opener; a Cancel (Rust's Cancel,
 * and Close with its fixed reason) cancels `region` with `reason`. FIFO; a
 * step applies at most LAB_REGION_BATCH (REGION_COMMAND_BATCH, LR:4136). */
#define LAB_REGION_BATCH 8u
#define LAB_REGION_CMD_CAP ((uint32_t)ASX_MAX_TASKS + (uint32_t)ASX_MAX_REGIONS)

typedef struct {
    uint8_t cancel;
    uint16_t opener_gen;
    uint32_t opener; /* Create: the opener's slot */
    asx_region_id region;
    int has_budget;
    asx_budget budget;
    asx_cancel_reason reason; /* Cancel */
} lab_region_cmd;

static lab_region_cmd g_lab_rcmd[LAB_REGION_CMD_CAP];
static uint32_t g_lab_rcmd_n = 0;

/* Join-handle aborts (Rust JoinHandle::abort_with_reason,
 * runtime/task_handle.rs:1005, 470-542): the runtime applies them at the
 * start of the next step, after spawn admissions and around the region
 * commands, at most LAB_HANDLE_BATCH per drain, requests for one task
 * coalesced into the first (drain_handle_cancel_requests, LR:4018-4128;
 * coalesce_handle_cancel_requests, spawn_mailbox.rs:1470). A target still
 * awaiting admission takes its aborts as it is admitted, onto the cancel
 * lane only (drain_spawn_admissions, LR:3972-3986). */
#define LAB_HANDLE_BATCH 16u
#define LAB_HANDLE_CAP (2u * (uint32_t)ASX_MAX_TASKS)

typedef struct {
    uint32_t slot;
    uint16_t task_gen;
    asx_cancel_reason reason;
} lab_handle_cancel;

static lab_handle_cancel g_lab_hc[LAB_HANDLE_CAP];
static uint32_t g_lab_hc_n = 0;

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
    g_lab_rcmd_n = 0;
    g_lab_hc_n = 0;
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

/* cancel_task_for_handle (state.rs:3462): fails only for a task that is
 * gone, whose abort Rust ignores. */
static void lab_apply_handle_cancel(const lab_handle_cancel *h) {
    const asx_task_slot *t = &g_tasks[h->slot];
    asx_status st;
    if (!t->alive || t->generation != h->task_gen) return;
    st = asx_task_cancel_reason_internal(asx_task_handle_for_slot(h->slot), &h->reason,
                                         ASX_CANCEL_SRC_HANDLE);
    (void)st;
}

/* The queued aborts of the task in `slot`, taken off the queue and applied
 * as one, strengthened in order. */
static void lab_take_handle_cancels_for(uint32_t slot) {
    lab_handle_cancel merged;
    int found = 0;
    uint32_t k = 0;
    memset(&merged, 0, sizeof(merged));
    while (k < g_lab_hc_n) {
        ASX_CHECKPOINT_WAIVER("bounded: queue length <= LAB_HANDLE_CAP");
        if (g_lab_hc[k].slot != slot) {
            k++;
            continue;
        }
        if (!found) {
            merged = g_lab_hc[k];
            found = 1;
        } else {
            merged.reason = asx_cancel_strengthen(&merged.reason, &g_lab_hc[k].reason);
        }
        if (k + 1u < g_lab_hc_n) {
            memmove(&g_lab_hc[k], &g_lab_hc[k + 1u],
                    (size_t)(g_lab_hc_n - k - 1u) * sizeof(lab_handle_cancel));
        }
        g_lab_hc_n--;
    }
    if (found) lab_apply_handle_cancel(&merged);
}

/* drain_spawn_admissions (LR:3957-4012, state.rs:5212-5217): FIFO, each
 * scheduled at its budget priority, or on the cancel lane when it was
 * aborted before admission (the abort applied now schedules it there). */
void asx_lab_admit_pending(void) {
    uint32_t i;
    uint32_t n = g_lab_admit_n;
    g_lab_admit_n = 0;
    for (i = 0; i < n; i++) {
        asx_task_slot *t = &g_tasks[g_lab_admit[i]];
        ASX_CHECKPOINT_WAIVER("bounded: admissions <= ASX_MAX_TASKS");
        if (!t->alive || !t->lab_admission_pending) continue;
        t->lab_admission_pending = 0u;
        lab_take_handle_cancels_for(g_lab_admit[i]);
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

asx_status asx_lab_region_open_command(asx_task_slot *opener, asx_region_id parent,
                                       const asx_budget *budget) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    c->cancel = 0u;
    c->opener = (uint32_t)(opener - g_tasks);
    c->opener_gen = opener->generation;
    c->region = parent;
    c->has_budget = budget != NULL;
    if (budget != NULL) c->budget = *budget;
    return ASX_OK;
}

asx_status asx_lab_region_cancel_command(asx_region_id region, const asx_cancel_reason *reason) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    c->cancel = 1u;
    c->opener = ASX_SLOT_NONE;
    c->opener_gen = 0u;
    c->region = region;
    c->has_budget = 0;
    c->reason = *reason;
    return ASX_OK;
}

int asx_lab_region_commands_pending(void) { return g_lab_rcmd_n > 0u; }

/* asx_region_cancel fails only for a region that is gone (closed and
 * reused): as in Rust, a late cancel of one only records region.cancelled. */
static void lab_cancel_region(asx_region_id region, const asx_cancel_reason *reason) {
    asx_status st = asx_region_cancel(region, reason, NULL);
    if (st == ASX_E_STALE_HANDLE) asx_region_trace_cancel_of_gone_internal(region, reason);
}

/* drain_region_commands (LR:4135-4217): up to 8 commands in order. A
 * Cancel cancels its region now (a closed or gone region is left alone);
 * its tasks' cancel lane entries and wakes are dispatched together after
 * the batch (drain_deferred_cancel_dispatches, LR:4219-4239). Each
 * Create's result is published, waking its opener, after every command of
 * the batch is applied. A Create whose opener is gone still mints the
 * region, which then closes, as Rust's abandoned slot closes a region
 * minted for it (child_region.rs:264-275). */
void asx_lab_drain_region_commands(void) {
    lab_region_cmd batch[LAB_REGION_BATCH];
    uint32_t opened[LAB_REGION_BATCH];
    uint32_t n_opened = 0;
    uint32_t n = g_lab_rcmd_n < LAB_REGION_BATCH ? g_lab_rcmd_n : LAB_REGION_BATCH;
    uint32_t i;

    if (n == 0u) return;
    memcpy(batch, g_lab_rcmd, (size_t)n * sizeof(lab_region_cmd));
    if (g_lab_rcmd_n > n) {
        memmove(&g_lab_rcmd[0], &g_lab_rcmd[n],
                (size_t)(g_lab_rcmd_n - n) * sizeof(lab_region_cmd));
    }
    g_lab_rcmd_n -= n;

    asx_lab_cancel_batch_begin();
    for (i = 0; i < n; i++) {
        const lab_region_cmd *c = &batch[i];
        asx_task_slot *t;
        asx_region_id id = ASX_INVALID_ID;
        asx_status st;
        ASX_CHECKPOINT_WAIVER("bounded: n <= LAB_REGION_BATCH");
        if (c->cancel) {
            lab_cancel_region(c->region, &c->reason);
            continue;
        }
        st = asx_region_open_child_with_budget(c->region, c->has_budget ? &c->budget : NULL, &id);
        t = &g_tasks[c->opener];
        if (!t->alive || t->generation != c->opener_gen || t->region_wait != ASX_REGION_WAIT_OPEN) {
            if (st == ASX_OK) {
                asx_cancel_reason close = asx_region_close_reason_internal();
                lab_cancel_region(id, &close);
            }
            continue;
        }
        t->region_wait = ASX_REGION_WAIT_OPENED;
        t->region_wait_status = st;
        t->region_wait_region = id;
        opened[n_opened++] = c->opener;
    }
    asx_lab_cancel_batch_end();
    for (i = 0; i < n_opened; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n_opened <= LAB_REGION_BATCH");
        asx_task_wake_slot_internal(&g_tasks[opened[i]]);
    }
}

asx_status asx_lab_handle_cancel_command(const asx_task_slot *t, const asx_cancel_reason *reason) {
    lab_handle_cancel *h;
    if (g_lab_hc_n >= LAB_HANDLE_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    h = &g_lab_hc[g_lab_hc_n++];
    h->slot = (uint32_t)(t - g_tasks);
    h->task_gen = t->generation;
    h->reason = *reason;
    return ASX_OK;
}

int asx_lab_handle_cancels_pending(void) { return g_lab_hc_n > 0u; }

/* drain_handle_cancel_requests (LR:4018-4128): up to 16 requests, those for
 * one task strengthened into the first, applied in order; the cancel lane
 * entries come in that order and the cancel wakes after the batch. */
void asx_lab_drain_handle_cancels(void) {
    lab_handle_cancel batch[LAB_HANDLE_BATCH];
    uint32_t n = g_lab_hc_n < LAB_HANDLE_BATCH ? g_lab_hc_n : LAB_HANDLE_BATCH;
    uint32_t kept = 0;
    uint32_t i;
    uint32_t j;

    if (n == 0u) return;
    memset(batch, 0, sizeof(batch));
    for (i = 0; i < n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n <= LAB_HANDLE_BATCH");
        for (j = 0; j < kept; j++) {
            ASX_CHECKPOINT_WAIVER("bounded: kept <= LAB_HANDLE_BATCH");
            if (batch[j].slot == g_lab_hc[i].slot && batch[j].task_gen == g_lab_hc[i].task_gen) {
                break;
            }
        }
        if (j < kept) {
            batch[j].reason = asx_cancel_strengthen(&batch[j].reason, &g_lab_hc[i].reason);
        } else {
            batch[kept++] = g_lab_hc[i];
        }
    }
    if (g_lab_hc_n > n) {
        memmove(&g_lab_hc[0], &g_lab_hc[n], (size_t)(g_lab_hc_n - n) * sizeof(lab_handle_cancel));
    }
    g_lab_hc_n -= n;

    asx_lab_cancel_batch_begin();
    for (i = 0; i < kept; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: kept <= LAB_HANDLE_BATCH");
        lab_apply_handle_cancel(&batch[i]);
    }
    asx_lab_cancel_batch_end();
}
