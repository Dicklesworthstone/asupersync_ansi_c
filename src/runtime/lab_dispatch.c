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
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/lab.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#include <asx/runtime/runtime.h>
#include <string.h>

#define LAB_LANE_CAP (8u * (uint32_t)ASX_MAX_TASKS)
/* LAB_GROUP_CAP, LAB_CANCEL_STREAK_LIMIT, LAB_REGION_BATCH and
 * LAB_HANDLE_BATCH are Rust's (runtime_internal.h). */

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

/* Lab dispatch is one runtime's: its queues name task slots by index and
 * live in this file, so it drives kernel 0 (a bridge until its state moves
 * into the kernel; the init guard refuses a second runtime while it is
 * active). */
static asx_kernel *lab_kernel(void) { return g_kernels[0]; }

/* The index of `t` in its kernel's task arena. */
static uint32_t lab_slot_of(const asx_task_slot *t) {
    return (uint32_t)(t - asx_kernel_of(t->region)->tasks);
}

/* Entropy (Rust DetEntropy, util/entropy.rs:66-150): the runtime's source
 * is seeded with the lab seed as given (LabConfig::entropy_seed, set by
 * LabConfig::new(seed), lab/config.rs:190) and counts its forks. A task's
 * fork is keyed by its task-arena id (the arena shadow below); the spawn
 * mailbox numbers its provisional ids TaskId(i:2^31) (spawn_mailbox.rs:1935)
 * for in-poll spawns and region principals. */
#define LAB_GOLDEN UINT64_C(0x9E3779B97F4A7C15)
#define LAB_MAILBOX_GENERATION (UINT64_C(0x80000000) << 32)

static uint64_t g_lab_ent_seed = 0;
static uint64_t g_lab_ent_forks = 0;
static uint32_t g_lab_mailbox_ids = 0;

/* Rust's task arena (util/arena.rs:151-280): a slot per index with its
 * generation; insertion takes the most recently freed index (LIFO free
 * list), else a new one; removal bumps the generation. Rust removes a task
 * when it completes (state.rs:8497-8503). The live tasks bound the
 * indices, so LAB_ARENA_CAP is never reached unless C keeps more tasks
 * than Rust can; overflow fails the run like a lane overflow. */
#define LAB_ARENA_CAP (2u * (uint32_t)ASX_MAX_TASKS)
#define LAB_ARENA_NONE UINT32_MAX

static uint32_t g_lab_arena_gen[LAB_ARENA_CAP];
static uint32_t g_lab_arena_next[LAB_ARENA_CAP];
static uint32_t g_lab_arena_len = 0;
static uint32_t g_lab_arena_free = LAB_ARENA_NONE;
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

/* Spawns from a poll into a closing or closed region. C refuses them at
 * once, Rust queues them in the spawn mailbox and the next step's admission
 * refuses them (state.rs:1702), in FIFO order with the other admissions:
 * only then does a join of one resolve, waking its joiner. g_lab_admit
 * holds LAB_ADMIT_REFUSAL | record for them. */
#define LAB_ADMIT_REFUSAL 0x80000000u

typedef struct {
    uint8_t delivered;
    uint8_t has_waiter;
    uint8_t waiter_prio;
    uint16_t waiter_gen;
    uint32_t waiter;
} lab_refusal;

static lab_refusal g_lab_refusal[ASX_MAX_TASKS];
static uint32_t g_lab_refusal_n = 0;
static uint32_t g_lab_last_refusal = 0; /* ticket of the last spawn's refusal */

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
#define LAB_REGION_CMD_CAP ((uint32_t)ASX_MAX_TASKS + (uint32_t)ASX_MAX_REGIONS)

typedef struct {
    uint8_t cancel;
    uint16_t opener_gen;
    uint32_t opener;       /* Create: the opener's slot */
    uint32_t principal_id; /* Create: the principal's mailbox id */
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
 * awaiting admission keeps its aborts until it is admitted, which queues
 * them as one abort behind the pending ones, its first lane publication
 * (drain_spawn_admissions, LR:3972-3986; spawn_mailbox.rs:629-660). */
#define LAB_HANDLE_CAP (2u * (uint32_t)ASX_MAX_TASKS)

/* A region command's cancel (close_region_command, state.rs:4936-4955) is
 * not dispatched as it is applied: its tasks' cancel lane entries and then
 * their cancel wakes wait until drain_deferred_cancel_dispatches
 * (LR:4219-4239) publishes every deferred batch, lane entries first, after
 * the step's second handle-abort drain. A region the cancel closed has
 * woken its close waiters by then, onto the ready lane. */
#define LAB_DEFER_CAP (4u * (uint32_t)ASX_MAX_TASKS)

typedef struct {
    uint32_t slot;
    uint16_t task_gen;
    uint8_t priority;
    uint8_t wake; /* a cancel wake (at the waker's priority), else a lane entry */
} lab_deferred;

static lab_deferred g_lab_deferred[LAB_DEFER_CAP];
static uint32_t g_lab_deferred_n = 0;
static int g_lab_deferring = 0;

typedef struct {
    uint32_t slot;
    uint16_t task_gen;
    asx_cancel_reason reason;
} lab_handle_cancel;

static lab_handle_cancel g_lab_hc[LAB_HANDLE_CAP];
static uint32_t g_lab_hc_n = 0;
/* Aborts of tasks still awaiting admission: Rust caches them in the
 * handle (task_handle.rs:492-497) and the admission turns them into one
 * command; a refused admission never publishes them. */
static lab_handle_cancel g_lab_cached[LAB_HANDLE_CAP];
static uint32_t g_lab_cached_n = 0;

/* Retired tasks a cancel waker scheduled after they completed: Rust's
 * scheduled set holds task ids, so the next pick of one is a dispatch that
 * polls nothing (a stale wake names a retired future, LR:4772-4790). By
 * slot and generation, so a released and reused slot is not confused with
 * its old task. */
#define LAB_RETIRED_CAP ((uint32_t)ASX_MAX_TASKS)

typedef struct {
    uint32_t slot;
    uint16_t task_gen;
    uint64_t rust_task; /* its CompactTaskId, kept past the slot's reuse */
} lab_retired;

static lab_retired g_lab_retired[LAB_RETIRED_CAP];
static uint32_t g_lab_retired_n = 0;

/* Rust's LabRuntime::steps (LR:4484), and the caller's dispatch record
 * buffer (asx_scheduler_record_dispatches). */
static uint64_t g_lab_steps = 0;
static asx_dispatch_record *g_lab_rec = NULL;
static uint32_t g_lab_rec_cap = 0;
static uint32_t g_lab_rec_n = 0;
static uint64_t g_lab_fl_max = 0; /* the per-step futurelock check's threshold, 0 = off */

/* Forced replay (asx_lab_run_forced_schedule, bd-9kll.9.3): the seed asx_scheduler_use_lab_dispatch
 * was given and, while a replay runs, the schedule, the dispatches consumed, the running
 * certificate and the first divergence (ASX_FORCED_OK until there is one). */
static uint64_t g_lab_seed = 0;
static const asx_forced_schedule *g_lab_forced = NULL;
static const asx_forced_run_config *g_lab_forced_cfg = NULL;
static uint32_t g_lab_forced_next = 0;
static asx_forced_error g_lab_forced_err;

/* Rust's ScheduleCertificate and its decision count (priority.rs:1303), folded at every
 * pick whether or not anything records; the CompactTaskId of the task being picked, set by
 * the pick; and the forced-schedule recorder's buffer (asx_lab_start_forced_schedule_recording). */
static uint64_t g_lab_cert = 0;
static uint64_t g_lab_decisions = 0;
static uint64_t g_lab_pick_task = 0;
static asx_forced_dispatch *g_lab_frec = NULL;
static uint32_t g_lab_frec_cap = 0;
static uint32_t g_lab_frec_n = 0;

/* A task's Rust arena id as Rust's CompactTaskId packs it. */
static uint64_t lab_compact_task(const asx_task_slot *t) {
    return ((uint64_t)t->lab_rust_index << 32) | (uint64_t)t->lab_rust_gen;
}

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
    g_lab_refusal_n = 0;
    g_lab_last_refusal = 0;
    g_lab_wake_n = 0;
    g_lab_batch = 0;
    g_lab_rcmd_n = 0;
    g_lab_hc_n = 0;
    g_lab_cached_n = 0;
    g_lab_deferred_n = 0;
    g_lab_deferring = 0;
    g_lab_retired_n = 0;
    g_lab_steps = 0;
    g_lab_rec = NULL;
    g_lab_rec_cap = 0;
    g_lab_rec_n = 0;
    g_lab_fl_max = 0;
    g_lab_ent_seed = 0;
    g_lab_ent_forks = 0;
    g_lab_mailbox_ids = 0;
    g_lab_arena_len = 0;
    g_lab_arena_free = LAB_ARENA_NONE;
    g_lab_seed = 0;
    g_lab_forced = NULL;
    g_lab_forced_cfg = NULL;
    g_lab_forced_next = 0;
    memset(&g_lab_forced_err, 0, sizeof(g_lab_forced_err));
    g_lab_cert = 0;
    g_lab_decisions = 0;
    g_lab_pick_task = 0;
    g_lab_frec = NULL;
    g_lab_frec_cap = 0;
    g_lab_frec_n = 0;
}

/* Insert task `t` into the arena shadow: its Rust index and generation. */
static void lab_arena_insert(asx_task_slot *t) {
    uint32_t i;
    if (g_lab_arena_free != LAB_ARENA_NONE) {
        i = g_lab_arena_free;
        g_lab_arena_free = g_lab_arena_next[i];
    } else if (g_lab_arena_len < LAB_ARENA_CAP) {
        i = g_lab_arena_len++;
        g_lab_arena_gen[i] = 0;
    } else {
        g_lab_overflow = 1;
        t->lab_rust_live = 0u;
        return;
    }
    t->lab_rust_index = i;
    t->lab_rust_gen = g_lab_arena_gen[i];
    t->lab_rust_live = 1u;
}

void asx_lab_task_retired_internal(asx_task_slot *t) {
    uint32_t i = t->lab_rust_index;
    if (!t->lab_rust_live || i >= g_lab_arena_len) return;
    t->lab_rust_live = 0u;
    g_lab_arena_gen[i]++;
    g_lab_arena_next[i] = g_lab_arena_free;
    g_lab_arena_free = i;
}

/* A task's arena id packed as Rust's task_seed: (generation << 32) | index. */
static uint64_t lab_task_seed(const asx_task_slot *t) {
    return ((uint64_t)t->lab_rust_gen << 32) | (uint64_t)t->lab_rust_index;
}

/* DetRng::new's seed correction (det_rng.rs:63-68, 129-158): 0 becomes 1;
 * the degenerate seeds are offset by the golden-ratio constant. */
static uint64_t lab_rng_seed(uint64_t seed) {
    if (seed == 0u) return 1u;
    if (seed == UINT64_C(0xFFFFFFFFFFFFFFFF) || seed == UINT64_C(0x00000000FFFFFFFF) ||
        seed == UINT64_C(0xFFFFFFFF00000000) || seed == UINT64_C(0x5555555555555555) ||
        seed == UINT64_C(0xAAAAAAAAAAAAAAAA)) {
        return seed + LAB_GOLDEN;
    }
    return seed;
}

/* xorshift64 (det_rng.rs:267-276). */
static uint64_t lab_xorshift(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* DetEntropy::mix_seed (splitmix64's finalizer). */
static uint64_t lab_mix_seed(uint64_t s) {
    s ^= s >> 30;
    s *= UINT64_C(0xBF58476D1CE4E5B9);
    s ^= s >> 27;
    s *= UINT64_C(0x94D049BB133111EB);
    s ^= s >> 31;
    return s;
}

/* DetEntropy::fork: the child of a source with seed `parent_seed` and
 * fork counter *forks, for the task whose arena id packs to `task_seed`
 * ((generation << 32) | index). */
static void lab_fork(asx_lab_entropy *child, uint64_t parent_seed, uint64_t *forks,
                     uint64_t task_seed) {
    uint64_t seed = lab_mix_seed(parent_seed + LAB_GOLDEN + task_seed + *forks);
    (*forks)++;
    child->seed = seed;
    child->rng = lab_rng_seed(seed);
    child->forks = 0;
    child->state = ASX_LAB_ENTROPY_READY;
}

void asx_lab_entropy_direct_internal(asx_task_slot *t) {
    lab_arena_insert(t);
    lab_fork(&t->lab_ent, g_lab_ent_seed, &g_lab_ent_forks, lab_task_seed(t));
}

void asx_lab_entropy_spawned_internal(asx_task_slot *t, const asx_task_slot *spawner,
                                      asx_region_slot *parent_region) {
    memset(&t->lab_ent, 0, sizeof(t->lab_ent));
    t->lab_ent.state = ASX_LAB_ENTROPY_OWED;
    if (parent_region != NULL) {
        t->lab_ent_parent_region = 1u;
        t->lab_ent_parent = (uint32_t)(parent_region - asx_kernel_of(t->region)->regions);
        t->lab_ent_parent_gen = parent_region->generation;
    } else {
        t->lab_ent_parent_region = 0u;
        t->lab_ent_parent = lab_slot_of(spawner);
        t->lab_ent_parent_gen = spawner->generation;
    }
}

void asx_lab_entropy_first_poll_internal(asx_task_slot *t) {
    asx_lab_entropy *parent = NULL;
    if (t->lab_ent.state != ASX_LAB_ENTROPY_OWED) return;
    if (t->lab_ent_parent_region) {
        asx_region_slot *r = &lab_kernel()->regions[t->lab_ent_parent];
        if (r->alive && r->generation == t->lab_ent_parent_gen) parent = &r->lab_principal;
    } else {
        asx_task_slot *p = &lab_kernel()->tasks[t->lab_ent_parent];
        if (p->alive && p->generation == t->lab_ent_parent_gen) parent = &p->lab_ent;
    }
    /* A source C no longer holds (its task or region slot was reused):
     * the task gets no entropy, and draws fall back. */
    if (parent == NULL || parent->state != ASX_LAB_ENTROPY_READY) {
        t->lab_ent.state = ASX_LAB_ENTROPY_NONE;
        return;
    }
    lab_fork(&t->lab_ent, parent->seed, &parent->forks, lab_task_seed(t));
}

int asx_lab_entropy_index_internal(asx_task_slot *t, uint32_t bound, uint32_t *out) {
    uint64_t threshold;
    if (!g_lab_active || t->lab_ent.state != ASX_LAB_ENTROPY_READY || bound == 0u) return 0;
    threshold = UINT64_MAX - (UINT64_MAX % (uint64_t)bound);
    for (;;) {
        uint64_t v = lab_xorshift(&t->lab_ent.rng);
        ASX_CHECKPOINT_WAIVER("rejection sampling: v < threshold with probability > 1/2");
        if (v < threshold) {
            *out = (uint32_t)(v % (uint64_t)bound);
            return 1;
        }
    }
}

int asx_task_random_u64_internal(asx_task_id self, uint64_t *out) {
    asx_task_slot *t;
    if (asx_task_slot_lookup(self, &t) != ASX_OK) return 0;
    if (g_lab_active && t->lab_ent.state == ASX_LAB_ENTROPY_READY) {
        *out = lab_xorshift(&t->lab_ent.rng);
        return 1;
    }
    return asx_runtime_random_u64(out) == ASX_OK;
}

int asx_lab_dispatch_active(void) { return g_lab_active; }

uint64_t asx_lab_step_begin_internal(void) { return ++g_lab_steps; }

void asx_lab_record_dispatch_internal(uint32_t slot, uint16_t task_gen, int cancel_lane,
                                      uint64_t step, asx_time at) {
    uint8_t lane = (uint8_t)(cancel_lane ? ASX_DISPATCH_LANE_CANCEL : ASX_DISPATCH_LANE_READY);
    /* certificate.record(task, lane, steps) at the pick (LR:4632). */
    g_lab_cert = asx_forced_schedule_certificate_step(g_lab_cert, g_lab_pick_task, lane, step);
    g_lab_decisions++;
    if (g_lab_frec_n < g_lab_frec_cap) {
        asx_forced_dispatch *f = &g_lab_frec[g_lab_frec_n];
        f->task = g_lab_pick_task;
        f->worker = 0u;
        f->lane = lane;
        f->at_step = step;
        f->at_nanos = (uint64_t)at;
    }
    if (g_lab_frec_n < UINT32_MAX) g_lab_frec_n++;
    if (g_lab_rec_n < g_lab_rec_cap) {
        asx_dispatch_record *d = &g_lab_rec[g_lab_rec_n];
        d->task = lab_kernel()->tasks[slot].generation == task_gen
                      ? asx_task_handle_for_slot(lab_kernel(), slot)
                      : asx_handle_mint(lab_kernel(), ASX_TYPE_TASK, 0u,
                                        asx_handle_pack_index(task_gen, (uint16_t)slot));
        d->step = step;
        d->at = at;
        d->lane = (uint8_t)(cancel_lane ? ASX_DISPATCH_LANE_CANCEL : ASX_DISPATCH_LANE_READY);
    }
    if (g_lab_rec_n < UINT32_MAX) g_lab_rec_n++;
    /* TaskRecord::mark_polled(steps) after the poll (LR:4809): the futurelock idle count. */
    if (lab_kernel()->tasks[slot].generation == task_gen)
        lab_kernel()->tasks[slot].lab_last_polled_step = step;
}
void asx_scheduler_record_dispatches(asx_dispatch_record *buf, uint32_t capacity) {
    g_lab_rec = buf;
    g_lab_rec_cap = buf != NULL ? capacity : 0u;
    g_lab_rec_n = 0;
}

uint32_t asx_scheduler_dispatches_recorded(void) { return g_lab_rec_n; }

int asx_lab_dispatch_overflowed(void) { return g_lab_overflow; }

asx_status asx_scheduler_use_lab_dispatch(uint64_t seed) {
    uint32_t i;
    /* A refused spawn can retain its captured actor cell without a live
     * task slot. Do not erase its admission ticket by reseeding. */
    if (g_lab_admit_n > 0u) return ASX_E_INVALID_STATE;
    for (i = 0; i < lab_kernel()->task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: task_count <= ASX_MAX_TASKS");
        if (lab_kernel()->tasks[i].alive) return ASX_E_INVALID_STATE;
    }
    asx_lab_dispatch_reset_internal();
    g_lab_rng = lab_rng_seed(seed);
    g_lab_ent_seed = seed;
    g_lab_seed = seed;
    g_lab_active = 1;
    return ASX_OK;
}

uint64_t asx_lab_rng_next(void) { return lab_xorshift(&g_lab_rng); }

uint32_t asx_lab_scheduled_count(void) { return g_lab_scheduled; }

int asx_lab_is_scheduled(const asx_task_slot *t) { return t->lab_scheduled != 0u; }

static void lab_push(lab_lane *lane, const asx_task_slot *t, uint8_t priority) {
    lab_entry *e;
    if (lane->n >= LAB_LANE_CAP) {
        g_lab_overflow = 1;
        return;
    }
    e = &lane->e[lane->n++];
    e->slot = lab_slot_of(t);
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

static void lab_defer(const asx_task_slot *t, uint8_t priority, uint8_t wake) {
    lab_deferred *d;
    if (g_lab_deferred_n >= LAB_DEFER_CAP) {
        g_lab_overflow = 1;
        return;
    }
    d = &g_lab_deferred[g_lab_deferred_n++];
    d->slot = lab_slot_of(t);
    d->task_gen = t->generation;
    d->priority = priority;
    d->wake = wake;
}

void asx_lab_schedule_cancel(asx_task_slot *t, uint8_t priority) {
    if (t == NULL || !t->alive || asx_task_is_terminal(t->state)) return;
    if (g_lab_deferring) {
        lab_defer(t, priority, 0u);
        return;
    }
    lab_mark_scheduled(t);
    lab_push(&g_lab_cancel, t, priority);
}

int asx_lab_deferred_cancels_pending(void) { return g_lab_deferred_n > 0u; }

void asx_lab_drain_deferred_cancels(void) {
    lab_deferred batch[LAB_DEFER_CAP];
    uint32_t n = g_lab_deferred_n;
    uint32_t pass;
    uint32_t i;
    if (n == 0u) return;
    memcpy(batch, g_lab_deferred, (size_t)n * sizeof(lab_deferred));
    g_lab_deferred_n = 0;
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < n; i++) {
            asx_task_slot *t = &lab_kernel()->tasks[batch[i].slot];
            ASX_CHECKPOINT_WAIVER("bounded: n <= LAB_DEFER_CAP");
            if (batch[i].wake != (uint8_t)pass) continue;
            if (!t->alive || t->generation != batch[i].task_gen) continue;
            asx_lab_schedule_cancel(t, pass == 0u ? batch[i].priority : t->lab_waker_prio);
        }
    }
}

void asx_lab_schedule_cancel_retired(uint32_t slot, uint16_t task_gen, uint8_t priority) {
    uint32_t k;
    lab_entry *e;
    for (k = 0; k < g_lab_retired_n; k++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_lab_retired_n <= LAB_RETIRED_CAP");
        if (g_lab_retired[k].slot == slot && g_lab_retired[k].task_gen == task_gen) break;
    }
    if (k == g_lab_retired_n) {
        if (g_lab_retired_n >= LAB_RETIRED_CAP) {
            g_lab_overflow = 1;
            return;
        }
        g_lab_retired[g_lab_retired_n].slot = slot;
        g_lab_retired[g_lab_retired_n].task_gen = task_gen;
        /* Called as the task completes, while its slot still holds it. */
        g_lab_retired[g_lab_retired_n].rust_task = lab_compact_task(&lab_kernel()->tasks[slot]);
        g_lab_retired_n++;
        g_lab_scheduled++;
    }
    if (g_lab_cancel.n >= LAB_LANE_CAP) {
        g_lab_overflow = 1;
        return;
    }
    e = &g_lab_cancel.e[g_lab_cancel.n++];
    e->slot = slot;
    e->task_gen = task_gen;
    e->priority = priority;
    e->gen = g_lab_next_gen++;
}

/* A retired id leaves the scheduled set as it is picked. */
static int lab_retired_take(uint32_t slot, uint16_t task_gen) {
    uint32_t k;
    for (k = 0; k < g_lab_retired_n; k++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_lab_retired_n <= LAB_RETIRED_CAP");
        if (g_lab_retired[k].slot == slot && g_lab_retired[k].task_gen == task_gen) {
            g_lab_pick_task = g_lab_retired[k].rust_task;
            g_lab_retired[k] = g_lab_retired[--g_lab_retired_n];
            g_lab_scheduled--;
            return 1;
        }
    }
    return 0;
}

static int lab_entry_live(const lab_entry *e) {
    const asx_task_slot *t;
    if (e->slot >= lab_kernel()->task_count) return 0;
    t = &lab_kernel()->tasks[e->slot];
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
 * r % n; a dead one is dropped and the pick retried with the same r. A
 * retired task still in the scheduled set is a pick. */
static int lab_pop(lab_lane *lane, uint64_t r, uint32_t *out_slot, uint16_t *out_task_gen) {
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
#ifdef ASX_MUTANT_SCHED_REVERSE_ORDER
        /* Oracle self-test mutant (tests/gates/test_oracle_mutants.sh): the
         * tie-break mirrored. Never in a release build. */
        chosen = group[n - 1u - (uint32_t)(r % (uint64_t)n)];
#else
        /* ASX_ANALYZER_WAIVER("gcc: group[0..n) holds every top-priority entry, n >= 1") */
        chosen = group[(uint32_t)(r % (uint64_t)n)];
#endif
        e = lane->e[chosen];
        lab_remove_at(lane, chosen);
        if (lab_entry_live(&e)) {
            g_lab_pick_task = lab_compact_task(&lab_kernel()->tasks[e.slot]);
            lab_kernel()->tasks[e.slot].lab_scheduled = 0u;
            g_lab_scheduled--;
            *out_slot = e.slot;
            *out_task_gen = e.task_gen;
            return 1;
        }
        if (lab_retired_take(e.slot, e.task_gen)) {
            *out_slot = e.slot;
            *out_task_gen = e.task_gen;
            return 1;
        }
    }
}

int asx_lab_pick(uint64_t r, uint32_t *out_slot, uint16_t *out_task_gen, int *out_cancel_lane) {
    *out_cancel_lane = 0;
    if (g_lab_streak < LAB_CANCEL_STREAK_LIMIT &&
        lab_pop(&g_lab_cancel, r, out_slot, out_task_gen)) {
        g_lab_streak++;
        *out_cancel_lane = 1;
        return 1;
    }
    /* The timed lane is never fed by the lab at the pinned rev (LR:7658). */
    if (lab_pop(&g_lab_ready, r, out_slot, out_task_gen)) {
        g_lab_streak = 0;
        return 1;
    }
    if (lab_pop(&g_lab_cancel, r, out_slot, out_task_gen)) {
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
    uint32_t slot = lab_slot_of(t);
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
    g_lab_mailbox_ids++; /* its provisional id */
    t->lab_admission_pending = 1u;
    g_lab_admit[g_lab_admit_n++] = lab_slot_of(t);
    {
        asx_region_slot *r;
        if (asx_region_slot_lookup(t->region, &r) == ASX_OK) r->lab_pending_admissions++;
    }
}

void asx_lab_note_spawn_internal(void) { g_lab_last_refusal = 0; }

void asx_lab_defer_refused_admission(void) {
    lab_refusal *rf;
    if (g_lab_admit_n >= ASX_MAX_TASKS || g_lab_refusal_n >= ASX_MAX_TASKS) {
        g_lab_overflow = 1;
        return;
    }
    g_lab_mailbox_ids++; /* the refused spawn's provisional id */
    rf = &g_lab_refusal[g_lab_refusal_n];
    memset(rf, 0, sizeof(*rf));
    g_lab_admit[g_lab_admit_n++] = LAB_ADMIT_REFUSAL | g_lab_refusal_n;
    g_lab_refusal_n++;
    g_lab_last_refusal = g_lab_refusal_n; /* tickets count from 1 */
}

uint32_t asx_scheduler_last_spawn_refusal(void) { return g_lab_last_refusal; }

int asx_task_refusal_delivered(uint32_t ticket) {
    return ticket == 0u || ticket > g_lab_refusal_n || g_lab_refusal[ticket - 1u].delivered;
}

void asx_lab_refusal_watch(uint32_t ticket, const asx_task_slot *t) {
    lab_refusal *rf;
    if (asx_task_refusal_delivered(ticket)) return;
    rf = &g_lab_refusal[ticket - 1u];
    rf->has_waiter = 1u;
    rf->waiter = lab_slot_of(t);
    rf->waiter_gen = t->generation;
    rf->waiter_prio = t->lab_waker_prio;
}

asx_status asx_task_await_refusal(asx_task_id self, uint32_t ticket) {
    asx_task_slot *t;
    asx_status st;
    if (asx_task_refusal_delivered(ticket)) return ASX_OK;
    st = asx_task_slot_lookup(self, &t);
    if (st != ASX_OK) return st;
    if (!t->in_poll) return ASX_E_INVALID_STATE;
    /* The join's registration: this poll's waker. */
    asx_lab_refusal_watch(ticket, t);
    t->park_requested = 1;
    return ASX_E_PENDING;
}

int asx_lab_admissions_pending(void) { return g_lab_admit_n > 0u; }

/* cancel_task_for_handle (state.rs:3462): fails only for a task that is
 * gone, whose abort Rust ignores. */
static void lab_apply_handle_cancel(const lab_handle_cancel *h) {
    asx_task_slot *t = &lab_kernel()->tasks[h->slot];
    asx_status st;
    if (!t->alive || t->generation != h->task_gen) return;
    /* Ended cancelled before the drain: Rust strengthened its Cx reason
     * when the abort was requested, and its join reports that. */
    if (asx_task_is_terminal(t->state)) {
        if (asx_outcome_severity_of(&t->outcome) == ASX_OUTCOME_CANCELLED) {
            t->cancel_reason = asx_cancel_strengthen(&t->cancel_reason, &h->reason);
        }
        return;
    }
    st = asx_task_cancel_reason_internal(asx_task_handle_for_slot(lab_kernel(), h->slot),
                                         &h->reason, ASX_CANCEL_SRC_HANDLE);
    (void)st;
}

/* The cached aborts of the task in `slot`, taken as one, strengthened in
 * order, into *out. Returns 1 if there were any. */
static int lab_take_handle_cancels_for(uint32_t slot, lab_handle_cancel *out) {
    int found = 0;
    uint32_t k = 0;
    memset(out, 0, sizeof(*out));
    while (k < g_lab_cached_n) {
        ASX_CHECKPOINT_WAIVER("bounded: cache length <= LAB_HANDLE_CAP");
        if (g_lab_cached[k].slot != slot) {
            k++;
            continue;
        }
        if (!found) {
            *out = g_lab_cached[k];
            found = 1;
        } else {
            out->reason = asx_cancel_strengthen(&out->reason, &g_lab_cached[k].reason);
        }
        if (k + 1u < g_lab_cached_n) {
            memmove(&g_lab_cached[k], &g_lab_cached[k + 1u],
                    (size_t)(g_lab_cached_n - k - 1u) * sizeof(lab_handle_cancel));
        }
        g_lab_cached_n--;
    }
    return found;
}

/* A refused spawn's aborts strengthen the reason its join reports. */
static void lab_apply_handle_cancels_for(uint32_t slot) {
    lab_handle_cancel merged;
    if (lab_take_handle_cancels_for(slot, &merged)) lab_apply_handle_cancel(&merged);
}

/* drain_spawn_admissions (LR:3957-4012, state.rs:5212-5217): FIFO, each
 * scheduled at its budget priority, or on the cancel lane when it was
 * aborted before admission (the abort applied now schedules it there). */
asx_status asx_lab_admit_pending(void) {
    uint32_t i;
    uint32_t n = g_lab_admit_n;
    g_lab_admit_n = 0;
    for (i = 0; i < n; i++) {
        asx_task_slot *t;
        ASX_CHECKPOINT_WAIVER("bounded: admissions <= ASX_MAX_TASKS");
        if (g_lab_admit[i] & LAB_ADMIT_REFUSAL) {
            /* Drop captured actor endpoints before resolving the refused
             * handle, as Rust drops the spawn payload before its slot. */
            uint32_t refusal = g_lab_admit[i] & ~LAB_ADMIT_REFUSAL;
            lab_refusal *rf = &g_lab_refusal[refusal];
            asx_actor_refusal_delivered_internal(refusal + 1u);
            rf->delivered = 1u;
            if (rf->has_waiter) {
                asx_task_slot *w = &lab_kernel()->tasks[rf->waiter];
                if (w->alive && w->generation == rf->waiter_gen) {
                    asx_lab_schedule(w, rf->waiter_prio);
                }
            }
            continue;
        }
        t = &lab_kernel()->tasks[g_lab_admit[i]];
        if (!t->alive || !t->lab_admission_pending) continue;
        t->lab_admission_pending = 0u;
        {
            asx_region_slot *r;
            if (asx_region_slot_lookup(t->region, &r) == ASX_OK) {
                if (r->lab_pending_admissions > 0u) r->lab_pending_admissions--;
                /* Rust's admit_spawn_record_in: a region that no longer
                 * accepts work refuses the spawn before the task table is
                 * touched (SpawnError::RegionClosed). */
                if (!(t->cleanup_only ? asx_region_can_accept_cleanup_work(r->state)
                                      : asx_region_can_accept_work(r->state))) {
                    asx_status st = asx_task_refuse_admission_internal(t, r, ASX_E_REGION_CLOSED);
                    if (st != ASX_OK) return st;
                    lab_apply_handle_cancels_for(g_lab_admit[i]);
                    continue;
                }
                lab_arena_insert(t); /* its task-arena id */
                /* Region admission (record/region.rs add_task) at its
                 * live-task limit, the task itself left out: Rust recycles
                 * the record it inserted and refuses the spawn
                 * (SpawnError::RegionAtCapacity); cleanup work in a
                 * Finalizing region is exempt. */
                if (!(t->cleanup_only && r->state == ASX_REGION_FINALIZING) &&
                    asx_region_live_admitted_internal(r) > r->limits.max_tasks) {
                    asx_status st;
                    asx_lab_task_retired_internal(t);
                    st = asx_task_refuse_admission_internal(t, r, ASX_E_ADMISSION_LIMIT);
                    if (st != ASX_OK) return st;
                    lab_apply_handle_cancels_for(g_lab_admit[i]);
                    continue;
                }
            } else {
                lab_arena_insert(t);
            }
        }
        /* Admission adds it to its region's membership (state.rs:5212),
         * arms its budget-deadline timer (state.rs:4892), then records its
         * Spawn event (state.rs:1119). */
        t->member_seq = asx_task_next_member_seq_internal(lab_kernel());
        asx_lab_arm_budget_deadline_internal(t);
        asx_trace_emit(ASX_TRACE_TASK_SPAWN,
                       (uint64_t)asx_task_handle_for_slot(lab_kernel(), g_lab_admit[i]),
                       (uint64_t)t->region);
        {
            /* Aborted before admission: admission hands the first lane
             * publication to one abort command carrying the strongest
             * reason, queued behind the aborts already pending, which this
             * step's drain applies in order (AdmissionPublication's managed
             * path, spawn_mailbox.rs:629-660); it does not go on the ready
             * lane. */
            lab_handle_cancel pending;
            if (lab_take_handle_cancels_for(g_lab_admit[i], &pending)) {
                if (g_lab_hc_n >= LAB_HANDLE_CAP) {
                    g_lab_overflow = 1;
                } else {
                    g_lab_hc[g_lab_hc_n++] = pending;
                }
            } else {
                asx_lab_schedule(t, t->budget.priority);
            }
        }
    }
    return ASX_OK;
}

/* A cancel request reaches a task's CancelTaskWaker, which schedules it on
 * the cancel lane at the priority of its last poll (LR:6547-6553). Only a
 * task polled at least once has one. */
void asx_lab_cancel_wake(asx_task_slot *t) {
    if (!t->first_polled) return;
    if (g_lab_deferring) {
        lab_defer(t, 0u, 1u);
        return;
    }
    if (g_lab_batch > 0u) {
        if (g_lab_wake_n >= ASX_MAX_TASKS) {
            g_lab_overflow = 1;
            return;
        }
        g_lab_wake[g_lab_wake_n++] = lab_slot_of(t);
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
        asx_task_slot *t = &lab_kernel()->tasks[g_lab_wake[i]];
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
    c->opener = lab_slot_of(opener);
    c->opener_gen = opener->generation;
    /* The principal's id comes from the spawn mailbox at the request
     * (cx.rs:4502). */
    c->principal_id = g_lab_mailbox_ids++;
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
    c->principal_id = 0u;
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
    if (st == ASX_E_STALE_HANDLE) {
        asx_region_trace_cancel_of_gone_internal(region, reason);
    } else if (st == ASX_OK) {
        /* close_region_command advances the region after the cancel
         * (state.rs:4936-4940): one with child regions starts draining. */
        asx_region_advance_internal(region);
    }
}

/* A Watch command (Rust RegionCommand::Watch) is a lab_region_cmd whose
 * `cancel` field is one of these: the opener's slot and generation as for a
 * Create, `region` the monitored task (a removal: the monitor reference),
 * `principal_id` and `budget.poll_quota` a server's slot and generation. */
#define LAB_CMD_MONITOR_TASK 2u
#define LAB_CMD_MONITOR_SERVER 3u
#define LAB_CMD_DEMONITOR 4u
#define LAB_CMD_LINK_TASK 5u   /* `budget.priority` is the holder's exit policy */
#define LAB_CMD_LINK_SERVER 6u /* likewise; `principal_id`, `budget.poll_quota`: the server */
#define LAB_CMD_UNLINK 7u
/* RegionCommand::RegisterFinalizer (spawn_mailbox.rs:1624): `region` is the
 * region whose sync finalizer is registered; the opener waits for the
 * acknowledgement as for a Watch (retain_until_finalized, child_region.rs:342). */
#define LAB_CMD_RETAIN 8u

/* The poll of the cleanup task a retained value leaves: Rust's closure drops
 * the value and a C holder resolves its own, so there is nothing to do, and
 * the task completes at its first poll as Rust's one-poll wrapper does. */
static asx_status lab_retained_poll(void *unused, asx_task_id self) {
    (void)unused;
    (void)self;
    return ASX_OK;
}

/* RegisterRegionFinalizer::apply (:1640): the finalizer is registered while
 * the region is open (a closing, closed or unknown region refuses it), and
 * the acknowledgement is left on the opener, recorded in `woken`, which wakes
 * once the batch is applied (finalizer_publications, LR:4210). The
 * registration happens whether or not the opener still waits.
 *
 * Rust promotes every finalizer, a sync one included, to a masked cleanup
 * task when its region finalizes (drain_ready_async_finalizers wraps it in
 * `async move { finalizer() }`, state.rs:8612), spawned at a later lab step.
 * The task is as visible as a declared finalizer's: its spawn and completion
 * events, its dispatch and the step the region waits for it. A C sync
 * finalizer runs inline in the drain, so this one is registered as an async
 * finalizer, whose poll completes at once. */
static void lab_apply_retain(const lab_region_cmd *c, uint32_t *woken, uint32_t *n_woken) {
    asx_task_slot *t;
    asx_status st = asx_region_defer_async(c->region, lab_retained_poll, NULL);

    t = &lab_kernel()->tasks[c->opener];
    if (!t->alive || t->generation != c->opener_gen || t->region_wait != ASX_REGION_WAIT_WATCH) {
        return;
    }
    t->region_wait = ASX_REGION_WAIT_WATCHED;
    t->region_wait_status = st;
    t->region_wait_region = ASX_INVALID_ID;
    woken[(*n_woken)++] = c->opener;
}

/* TaskWatches::apply (monitor.rs:1323) for one Watch command. A removal
 * takes effect now. A request establishes only for an opener still waiting
 * on it (Rust's abandoned slot is not established) and leaves its result on
 * the opener for its next poll; the opener is recorded in `woken`, to wake
 * once the batch is applied (watch_wakers, LR:4219). */
static void lab_apply_watch(const lab_region_cmd *c, uint32_t *woken, uint32_t *n_woken) {
    asx_task_slot *t;
    uint64_t ref = 0;
    asx_status st;

    if (c->cancel == LAB_CMD_DEMONITOR) {
        asx_monitor_remove_internal(c->region);
        return;
    }
    if (c->cancel == LAB_CMD_UNLINK) {
        asx_link_remove_internal(c->region);
        return;
    }
    t = &lab_kernel()->tasks[c->opener];
    if (!t->alive || t->generation != c->opener_gen || t->region_wait != ASX_REGION_WAIT_WATCH) {
        return;
    }
    if (c->cancel >= LAB_CMD_LINK_TASK) {
        st = asx_link_apply_internal(asx_task_handle_for_slot(lab_kernel(), c->opener), c->region,
                                     c->cancel == LAB_CMD_LINK_SERVER, c->principal_id,
                                     c->budget.poll_quota, c->budget.priority, &ref);
    } else {
        st = asx_monitor_apply_internal(asx_task_handle_for_slot(lab_kernel(), c->opener),
                                        c->region, c->cancel == LAB_CMD_MONITOR_SERVER,
                                        c->principal_id, c->budget.poll_quota, &ref);
    }
    t->region_wait = ASX_REGION_WAIT_WATCHED;
    t->region_wait_status = st;
    t->region_wait_region = st == ASX_OK ? ref : ASX_INVALID_ID;
    woken[(*n_woken)++] = c->opener;
}

/* drain_region_commands (LR:4135-4217): up to 8 commands in order. A
 * Cancel cancels its region now (a closed or gone region is left alone);
 * its tasks' cancel lane entries and wakes are deferred
 * (asx_lab_drain_deferred_cancels publishes them), while a region it
 * closes wakes its close waiters at once. Each
 * Create's result is published, waking its opener, after every command of
 * the batch is applied. A Create whose opener is gone still mints the
 * region, which then closes, as Rust's abandoned slot closes a region
 * minted for it (child_region.rs:264-275). A Watch is applied in place
 * (lab_apply_watch) and its opener wakes after the Creates' openers. */
void asx_lab_drain_region_commands(void) {
    lab_region_cmd batch[LAB_REGION_BATCH];
    uint32_t opened[LAB_REGION_BATCH];
    uint32_t retained[LAB_REGION_BATCH];
    uint32_t watched[LAB_REGION_BATCH];
    uint32_t n_opened = 0;
    uint32_t n_retained = 0;
    uint32_t n_watched = 0;
    uint32_t n = g_lab_rcmd_n < LAB_REGION_BATCH ? g_lab_rcmd_n : LAB_REGION_BATCH;
    uint32_t i;

    if (n == 0u) return;
    memcpy(batch, g_lab_rcmd, (size_t)n * sizeof(lab_region_cmd));
    if (g_lab_rcmd_n > n) {
        memmove(&g_lab_rcmd[0], &g_lab_rcmd[n],
                (size_t)(g_lab_rcmd_n - n) * sizeof(lab_region_cmd));
    }
    g_lab_rcmd_n -= n;

    g_lab_deferring = 1;
    for (i = 0; i < n; i++) {
        const lab_region_cmd *c = &batch[i];
        asx_task_slot *t;
        asx_region_id id = ASX_INVALID_ID;
        asx_status st;
        ASX_CHECKPOINT_WAIVER("bounded: n <= LAB_REGION_BATCH");
        if (c->cancel == LAB_CMD_RETAIN) {
            lab_apply_retain(c, retained, &n_retained);
            continue;
        }
        if (c->cancel >= LAB_CMD_MONITOR_TASK) {
            lab_apply_watch(c, watched, &n_watched);
            continue;
        }
        if (c->cancel) {
            lab_cancel_region(c->region, &c->reason);
            continue;
        }
        st = asx_region_open_child_with_budget(c->region, c->has_budget ? &c->budget : NULL, &id);
        if (st == ASX_OK) {
            /* Minting gives the principal a fork of the runtime source
             * (mint_child_region_parts, state.rs:5093). */
            asx_region_slot *minted;
            if (asx_region_slot_lookup(id, &minted) == ASX_OK) {
                lab_fork(&minted->lab_principal, g_lab_ent_seed, &g_lab_ent_forks,
                         LAB_MAILBOX_GENERATION | (uint64_t)c->principal_id);
            }
        }
        t = &lab_kernel()->tasks[c->opener];
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
    g_lab_deferring = 0;
    for (i = 0; i < n_opened; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n_opened <= LAB_REGION_BATCH");
        asx_task_wake_slot_internal(&lab_kernel()->tasks[opened[i]]);
    }
    for (i = 0; i < n_retained; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n_retained <= LAB_REGION_BATCH");
        /* ASX_ANALYZER_WAIVER("gcc: retained[0..n_retained) is written by lab_apply_retain") */
        asx_task_wake_slot_internal(&lab_kernel()->tasks[retained[i]]);
    }
    for (i = 0; i < n_watched; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: n_watched <= LAB_REGION_BATCH");
        /* ASX_ANALYZER_WAIVER("gcc: watched[0..n_watched) is written by lab_apply_watch") */
        asx_task_wake_slot_internal(&lab_kernel()->tasks[watched[i]]);
    }
}

asx_status asx_lab_handle_cancel_command(const asx_task_slot *t, const asx_cancel_reason *reason) {
    lab_handle_cancel *h;
    if (t->lab_admission_pending) {
        if (g_lab_cached_n >= LAB_HANDLE_CAP) return ASX_E_RESOURCE_EXHAUSTED;
        h = &g_lab_cached[g_lab_cached_n++];
    } else {
        if (g_lab_hc_n >= LAB_HANDLE_CAP) return ASX_E_RESOURCE_EXHAUSTED;
        h = &g_lab_hc[g_lab_hc_n++];
    }
    h->slot = lab_slot_of(t);
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

/* ------------------------------------------------------------------ */
/* Forced schedules (bd-9kll.9.3)                                      */
/*                                                                     */
/* Rust's run_forced_schedule (LR:2685, step_inner LR:4535-4605) takes  */
/* each step's task from a recorded dispatch instead of the seeded      */
/* picker: removed from the recorded lane, then polled. The RNG is      */
/* still drawn each step (the caller does), but never consulted. Any    */
/* mismatch ends the run with the task unpolled.                        */
/* ------------------------------------------------------------------ */

static int lab_retired_has(uint32_t slot, uint16_t task_gen) {
    uint32_t k;
    for (k = 0; k < g_lab_retired_n; k++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_lab_retired_n <= LAB_RETIRED_CAP");
        if (g_lab_retired[k].slot == slot && g_lab_retired[k].task_gen == task_gen) return 1;
    }
    return 0;
}

/* The entry would be a pick: its task is scheduled, or it names a retired
 * task still in the scheduled set. */
static int lab_entry_runnable(const lab_entry *e) {
    return lab_entry_live(e) || lab_retired_has(e->slot, e->task_gen);
}

static int lab_lane_runnable(const lab_lane *lane) {
    uint32_t k;
    for (k = 0; k < lane->n; k++) {
        ASX_CHECKPOINT_WAIVER("bounded: lane length <= LAB_LANE_CAP");
        if (lab_entry_runnable(&lane->e[k])) return 1;
    }
    return 0;
}

static int lab_lane_holds(const lab_lane *lane, uint32_t slot, uint16_t task_gen) {
    uint32_t k;
    for (k = 0; k < lane->n; k++) {
        ASX_CHECKPOINT_WAIVER("bounded: lane length <= LAB_LANE_CAP");
        if (lane->e[k].slot == slot && lane->e[k].task_gen == task_gen &&
            lab_entry_runnable(&lane->e[k])) {
            return 1;
        }
    }
    return 0;
}

/* Remove the entry for (slot, task_gen) from the lane as a pick would: a
 * live one leaves the scheduled set, a retired id leaves it as it is
 * picked. 1 when there was one. */
static int lab_take_exact(lab_lane *lane, uint32_t slot, uint16_t task_gen) {
    uint32_t k;
    for (k = 0; k < lane->n; k++) {
        ASX_CHECKPOINT_WAIVER("bounded: lane length <= LAB_LANE_CAP");
        if (lane->e[k].slot != slot || lane->e[k].task_gen != task_gen) continue;
        if (lab_entry_live(&lane->e[k])) {
            lab_remove_at(lane, k);
            g_lab_pick_task = lab_compact_task(&lab_kernel()->tasks[slot]);
            lab_kernel()->tasks[slot].lab_scheduled = 0u;
            g_lab_scheduled--;
            return 1;
        }
        if (lab_retired_has(slot, task_gen)) {
            lab_remove_at(lane, k);
            (void)lab_retired_take(slot, task_gen);
            return 1;
        }
    }
    return 0;
}

int asx_lab_forced_active(void) { return g_lab_forced != NULL; }

static asx_status lab_forced_fail(asx_forced_error_kind kind, uint32_t index, uint64_t step,
                                  uint64_t expected, uint64_t actual) {
    if (g_lab_forced_err.kind == ASX_FORCED_OK) {
        g_lab_forced_err.kind = kind;
        g_lab_forced_err.index = index;
        g_lab_forced_err.step = step;
        g_lab_forced_err.expected = expected;
        g_lab_forced_err.actual = actual;
    }
    return ASX_E_REPLAY_MISMATCH;
}

/* A recorded task id (CompactTaskId) to the slot and generation of the
 * task that plays it. */
static int lab_forced_resolve(uint64_t compact, uint32_t *slot, uint16_t *task_gen) {
    uint32_t i;

    if (g_lab_forced_cfg != NULL && g_lab_forced_cfg->resolve != NULL) {
        asx_task_id id = ASX_INVALID_ID;
        if (g_lab_forced_cfg->resolve(g_lab_forced_cfg->resolve_ctx, compact, &id) == ASX_OK &&
            asx_handle_is_valid(id) && asx_handle_type_tag(id) == ASX_TYPE_TASK) {
            *slot = (uint32_t)asx_handle_slot(id);
            *task_gen = asx_handle_generation(id);
            return 1;
        }
        /* The caller knows only the tasks it names. A task the runtime
         * created (a supervisor's generation, a finalizer, a controller)
         * has no name to give: it resolves through the arena shadow, as
         * with no resolver. */
    }
    /* The lab's arena shadow: a live task by its Rust (index, generation),
     * else a retired task a cancel waker scheduled. */
    for (i = 0; i < lab_kernel()->task_count; i++) {
        const asx_task_slot *t = &lab_kernel()->tasks[i];
        ASX_CHECKPOINT_WAIVER("bounded: task_count <= ASX_MAX_TASKS");
        if (t->alive && t->lab_rust_live && lab_compact_task(t) == compact) {
            *slot = i;
            *task_gen = t->generation;
            return 1;
        }
    }
    for (i = 0; i < g_lab_retired_n; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: g_lab_retired_n <= LAB_RETIRED_CAP");
        if (g_lab_retired[i].rust_task == compact) {
            *slot = g_lab_retired[i].slot;
            *task_gen = g_lab_retired[i].task_gen;
            return 1;
        }
    }
    return 0;
}

/* The pick of lab step `step` at clock `now` while a schedule is forced:
 * *out_picked is 1 with the task and lane, or 0 for a step with nothing to
 * dispatch (the schedule has none due and nothing is runnable). A
 * divergence is ASX_E_REPLAY_MISMATCH with the task left in its queue; it
 * stays the answer for every later step. */
asx_status asx_lab_forced_pick(uint64_t step, asx_time now, uint32_t *out_slot,
                               uint16_t *out_task_gen, int *out_cancel_lane, int *out_picked) {
    const asx_forced_schedule *s = g_lab_forced;
    const asx_forced_dispatch *d;
    uint32_t index = g_lab_forced_next;
    uint32_t slot = 0u;
    uint16_t task_gen = 0u;
    int cancel;
    lab_lane *lane;
    lab_lane *other;

    *out_picked = 0;
    *out_cancel_lane = 0;
    if (g_lab_forced_err.kind != ASX_FORCED_OK) return ASX_E_REPLAY_MISMATCH;

    if (index >= s->dispatch_count) {
        if (lab_lane_runnable(&g_lab_cancel) || lab_lane_runnable(&g_lab_ready)) {
            return lab_forced_fail(ASX_FORCED_E_SCHEDULE_EXHAUSTED, index, step,
                                   (uint64_t)s->dispatch_count, (uint64_t)index);
        }
        return ASX_OK;
    }
    d = &s->dispatches[index];

    if (step < d->at_step) {
        if (lab_lane_runnable(&g_lab_cancel) || lab_lane_runnable(&g_lab_ready)) {
            return lab_forced_fail(ASX_FORCED_E_EARLY_RUNNABLE, index, step, d->at_step, step);
        }
        return ASX_OK;
    }
    if (step > d->at_step) {
        return lab_forced_fail(ASX_FORCED_E_STEP_MISMATCH, index, step, d->at_step, step);
    }
    if ((uint64_t)now != d->at_nanos) {
        return lab_forced_fail(ASX_FORCED_E_TIME_MISMATCH, index, step, d->at_nanos, (uint64_t)now);
    }
    if (d->worker != 0u) {
        return lab_forced_fail(ASX_FORCED_E_WORKER_MISMATCH, index, step, 0u, (uint64_t)d->worker);
    }
    if (d->lane != ASX_FORCED_LANE_CANCEL && d->lane != ASX_FORCED_LANE_READY) {
        return lab_forced_fail(ASX_FORCED_E_LANE_UNSUPPORTED, index, step, ASX_FORCED_LANE_READY,
                               (uint64_t)d->lane);
    }
    cancel = d->lane == ASX_FORCED_LANE_CANCEL;
    lane = cancel ? &g_lab_cancel : &g_lab_ready;
    other = cancel ? &g_lab_ready : &g_lab_cancel;
    if (!lab_forced_resolve(d->task, &slot, &task_gen)) {
        return lab_forced_fail(ASX_FORCED_E_TASK_UNAVAILABLE, index, step, d->task, 0u);
    }
    if (!lab_take_exact(lane, slot, task_gen)) {
        if (lab_lane_holds(other, slot, task_gen)) {
            return lab_forced_fail(ASX_FORCED_E_LANE_MISMATCH, index, step, (uint64_t)d->lane,
                                   cancel ? ASX_FORCED_LANE_READY : ASX_FORCED_LANE_CANCEL);
        }
        return lab_forced_fail(ASX_FORCED_E_TASK_UNAVAILABLE, index, step, d->task, 0u);
    }

    g_lab_streak = cancel ? g_lab_streak + 1u : 0u;
    g_lab_forced_next = index + 1u;
    *out_slot = slot;
    *out_task_gen = task_gen;
    *out_cancel_lane = cancel;
    *out_picked = 1;
    return ASX_OK;
}

static asx_time lab_clock_now(void) { return asx_kernel_now_or_virtual_internal(lab_kernel()); }

uint64_t asx_scheduler_schedule_certificate(void) { return g_lab_cert; }

uint64_t asx_scheduler_schedule_decisions(void) { return g_lab_decisions; }

/* Rust's start_forced_schedule_recording / finish_forced_schedule_recording
 * (LR:2618-2683): capture each dispatch with the task's Rust arena id, then
 * bind the capture to the run's terminal step, clock and certificate. */
asx_status asx_lab_start_forced_schedule_recording(asx_forced_dispatch *storage,
                                                   uint32_t capacity) {
    if (storage == NULL || capacity == 0u) return ASX_E_INVALID_ARGUMENT;
    if (!g_lab_active || g_lab_frec != NULL || g_lab_steps != 0u) return ASX_E_INVALID_STATE;
    g_lab_frec = storage;
    g_lab_frec_cap = capacity;
    g_lab_frec_n = 0u;
    return ASX_OK;
}

asx_status asx_lab_finish_forced_schedule_recording(uint64_t config_hash, int quiescent,
                                                    asx_forced_schedule *out) {
    uint32_t count;

    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    if (g_lab_frec == NULL) return ASX_E_INVALID_STATE;
    count = g_lab_frec_n;
    if (count > g_lab_frec_cap) {
        /* The capture dropped dispatches: Rust refuses a partial receipt. */
        g_lab_frec = NULL;
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    out->version = ASX_FORCED_SCHEDULE_SCHEMA_VERSION;
    out->seed = g_lab_seed;
    out->config_hash = config_hash;
    out->dispatches = g_lab_frec;
    out->dispatch_count = count;
    out->terminal_steps = g_lab_steps;
    out->terminal_nanos = (uint64_t)lab_clock_now();
    out->terminal_schedule_hash = g_lab_cert;
    out->terminal_quiescent = quiescent ? 1u : 0u;
    out->truncated = 0u;
    g_lab_frec = NULL;
    return ASX_OK;
}

/* Install `schedule` as the source of every later lab pick. Everything
 * that steps the lab until asx_lab_forced_schedule_end() takes its tasks
 * from the schedule: a runner that drives the scheduler its own way (the
 * conformance interpreter runs to idle between script ops, then with timer
 * auto-advance) installs it, runs, and ends it. */
asx_status asx_lab_forced_schedule_begin(const asx_forced_schedule *schedule,
                                         const asx_forced_run_config *config,
                                         asx_forced_error *err) {
    asx_forced_error local;
    asx_forced_error *e = err != NULL ? err : &local;
    asx_status st;

    memset(e, 0, sizeof(*e));
    if (schedule == NULL) return ASX_E_INVALID_ARGUMENT;
    st = asx_forced_schedule_validate(schedule, e);
    if (st != ASX_OK) return st;
    if (!g_lab_active || g_lab_steps != 0u || g_lab_forced != NULL) {
        e->kind = ASX_FORCED_E_NOT_PRISTINE;
        e->expected = 0u;
        e->actual = g_lab_steps;
        return ASX_E_INVALID_STATE;
    }
    if (g_lab_seed != schedule->seed) {
        e->kind = ASX_FORCED_E_SEED_MISMATCH;
        e->expected = g_lab_seed;
        e->actual = schedule->seed;
        return ASX_E_INVALID_ARGUMENT;
    }
    g_lab_forced = schedule;
    g_lab_forced_cfg = config;
    g_lab_forced_next = 0u;
    memset(&g_lab_forced_err, 0, sizeof(g_lab_forced_err));
    return ASX_OK;
}

/* Rust's LabRuntime::is_quiescent, as the conformance snapshot has it:
 * nothing scheduled, every task terminal, no region still closing, no
 * obligation reserved. An open region with finished work is quiescent. */
static int lab_is_quiescent(void) {
    uint32_t i;

    if (g_lab_scheduled > 0u || g_lab_admit_n > 0u || g_lab_rcmd_n > 0u || g_lab_hc_n > 0u ||
        g_lab_deferred_n > 0u) {
        return 0;
    }
    for (i = 0; i < lab_kernel()->task_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: task_count <= ASX_MAX_TASKS");
        if (lab_kernel()->tasks[i].alive && !asx_task_is_terminal(lab_kernel()->tasks[i].state))
            return 0;
    }
    for (i = 0; i < lab_kernel()->region_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: region_count <= ASX_MAX_REGIONS");
        if (lab_kernel()->regions[i].alive && lab_kernel()->regions[i].state != ASX_REGION_OPEN &&
            lab_kernel()->regions[i].state != ASX_REGION_CLOSED) {
            return 0;
        }
    }
    for (i = 0; i < lab_kernel()->obligation_count; i++) {
        ASX_CHECKPOINT_WAIVER("bounded: obligation_count <= ASX_MAX_OBLIGATIONS");
        if (lab_kernel()->obligations[i].alive &&
            lab_kernel()->obligations[i].state == ASX_OBLIGATION_RESERVED)
            return 0;
    }
    return 1;
}

/* Take the schedule off and judge the run against it, as Rust's
 * run_forced_schedule does after its loop. `run_status` is what the
 * scheduler run returned; a divergence the pick recorded wins over it. */
static asx_status lab_forced_finish(asx_status run_status, int work_limited, uint64_t limit,
                                    asx_forced_report *report, asx_forced_error *e) {
    const asx_forced_schedule *schedule = g_lab_forced;
    uint32_t consumed = g_lab_forced_next;
    uint64_t steps = g_lab_steps;
    uint64_t now = (uint64_t)lab_clock_now();
    int quiescent = lab_is_quiescent();

    *e = g_lab_forced_err;
    g_lab_forced = NULL;
    g_lab_forced_cfg = NULL;

    if (e->kind != ASX_FORCED_OK) return ASX_E_REPLAY_MISMATCH;
    if (work_limited) {
        e->kind = ASX_FORCED_E_WORK_LIMIT;
        e->step = steps;
        e->expected = limit;
        e->actual = steps;
        return ASX_E_REPLAY_MISMATCH;
    }
    if (run_status != ASX_OK) return run_status;
    if (consumed != schedule->dispatch_count || !quiescent) {
        e->kind = ASX_FORCED_E_SCHEDULE_EXHAUSTED;
        e->index = consumed;
        e->step = steps;
        e->expected = schedule->dispatch_count;
        e->actual = consumed;
        return ASX_E_REPLAY_MISMATCH;
    }
    if (steps != schedule->terminal_steps) {
        e->kind = ASX_FORCED_E_TERMINAL_MISMATCH;
        e->index = 0u; /* the step count */
        e->step = steps;
        e->expected = schedule->terminal_steps;
        e->actual = steps;
        return ASX_E_REPLAY_MISMATCH;
    }
    if (now != schedule->terminal_nanos) {
        e->kind = ASX_FORCED_E_TERMINAL_MISMATCH;
        e->index = 1u; /* the clock */
        e->step = steps;
        e->expected = schedule->terminal_nanos;
        e->actual = now;
        return ASX_E_REPLAY_MISMATCH;
    }
    if (g_lab_cert != schedule->terminal_schedule_hash) {
        e->kind = ASX_FORCED_E_CERTIFICATE_MISMATCH;
        e->step = steps;
        e->expected = schedule->terminal_schedule_hash;
        e->actual = g_lab_cert;
        return ASX_E_REPLAY_MISMATCH;
    }
    if (report != NULL) {
        report->dispatches = consumed;
        report->steps = steps;
        report->terminal_nanos = now;
        report->schedule_hash = g_lab_cert;
    }
    return ASX_OK;
}

asx_status asx_lab_forced_schedule_end(asx_forced_report *report, asx_forced_error *err) {
    asx_forced_error local;
    asx_forced_error *e = err != NULL ? err : &local;

    memset(e, 0, sizeof(*e));
    if (g_lab_forced == NULL) return ASX_E_INVALID_STATE;
    return lab_forced_finish(ASX_OK, 0, 0u, report, e);
}

asx_status asx_lab_run_forced_schedule(const asx_forced_schedule *schedule, asx_region_id region,
                                       const asx_forced_run_config *config,
                                       asx_forced_report *report, asx_forced_error *err) {
    asx_forced_error local;
    asx_forced_error *e = err != NULL ? err : &local;
    asx_budget budget;
    asx_status st;
    uint64_t limit;
    int caller_limited;

    st = asx_lab_forced_schedule_begin(schedule, config, e);
    if (st != ASX_OK) return st;

    /* Rust steps while steps < terminal_steps, and refuses the work limit
     * only when it stops the run short of that. The budget counts steps. */
    caller_limited =
        config != NULL && config->max_steps != 0u && config->max_steps < schedule->terminal_steps;
    limit = caller_limited ? config->max_steps : schedule->terminal_steps;
    if (limit == 0u) limit = 1u;
    budget = asx_budget_from_polls(limit > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)limit);

    st = asx_scheduler_run(region, &budget);
    if (st == ASX_E_POLL_BUDGET_EXHAUSTED) {
        /* The receipt's terminal step is reached unless the caller cut the
         * run short: the checks in finish decide whether it ended there. */
        return lab_forced_finish(ASX_OK, caller_limited, limit, report, e);
    }
    return lab_forced_finish(st, 0, limit, report, e);
}

const char *asx_forced_error_kind_name(asx_forced_error_kind kind) {
    switch (kind) {
    case ASX_FORCED_OK: return "ok";
    case ASX_FORCED_E_TRUNCATED: return "truncated";
    case ASX_FORCED_E_MAGIC: return "magic";
    case ASX_FORCED_E_ARTIFACT_VERSION: return "artifact_version";
    case ASX_FORCED_E_SCHEMA_VERSION: return "schema_version";
    case ASX_FORCED_E_BYTE_LIMIT: return "byte_limit";
    case ASX_FORCED_E_DISPATCH_LIMIT: return "dispatch_limit";
    case ASX_FORCED_E_TRAILING_BYTES: return "trailing_bytes";
    case ASX_FORCED_E_CHECKSUM: return "checksum";
    case ASX_FORCED_E_LANE_TAG: return "lane_tag";
    case ASX_FORCED_E_BOOL_TAG: return "bool_tag";
    case ASX_FORCED_E_STEP_ORDER: return "step_order";
    case ASX_FORCED_E_TIME_ORDER: return "time_order";
    case ASX_FORCED_E_PARTIAL_SOURCE: return "partial_source";
    case ASX_FORCED_E_SEED_MISMATCH: return "seed_mismatch";
    case ASX_FORCED_E_NOT_PRISTINE: return "not_pristine";
    case ASX_FORCED_E_WORK_LIMIT: return "work_limit";
    case ASX_FORCED_E_EARLY_RUNNABLE: return "early_runnable";
    case ASX_FORCED_E_STEP_MISMATCH: return "step_mismatch";
    case ASX_FORCED_E_TIME_MISMATCH: return "time_mismatch";
    case ASX_FORCED_E_TASK_UNAVAILABLE: return "task_unavailable";
    case ASX_FORCED_E_LANE_MISMATCH: return "lane_mismatch";
    case ASX_FORCED_E_LANE_UNSUPPORTED: return "lane_unsupported";
    case ASX_FORCED_E_WORKER_MISMATCH: return "worker_mismatch";
    case ASX_FORCED_E_SCHEDULE_EXHAUSTED: return "schedule_exhausted";
    case ASX_FORCED_E_TERMINAL_MISMATCH: return "terminal_mismatch";
    case ASX_FORCED_E_CERTIFICATE_MISMATCH: return "certificate_mismatch";
    }
    return "unknown";
}

/* Watch commands (Rust RegionCommand::Watch with a WatchCommand::Monitor or
 * Demonitor, monitor.rs:1198), queued in the region-command FIFO. A monitor
 * request is the opener's own (`to_server` 0: its task holds the monitor)
 * or a GenServer's, named by slot and generation. */
asx_status asx_lab_watch_monitor_command(asx_task_slot *opener, asx_task_id target, int to_server,
                                         uint32_t server_slot, uint32_t server_gen) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    memset(c, 0, sizeof(*c));
    c->cancel = to_server ? LAB_CMD_MONITOR_SERVER : LAB_CMD_MONITOR_TASK;
    c->opener = lab_slot_of(opener);
    c->opener_gen = opener->generation;
    c->region = target;
    c->principal_id = server_slot;
    c->budget.poll_quota = server_gen;
    return ASX_OK;
}

asx_status asx_lab_watch_demonitor_command(uint64_t ref) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    memset(c, 0, sizeof(*c));
    c->cancel = LAB_CMD_DEMONITOR;
    c->opener = ASX_SLOT_NONE;
    c->region = ref;
    return ASX_OK;
}

/* WatchCommand::Link and Unlink (monitor.rs:1198), the same way. */
asx_status asx_lab_watch_link_command(asx_task_slot *opener, asx_task_id peer, int to_server,
                                      uint32_t server_slot, uint32_t server_gen, uint32_t policy) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    memset(c, 0, sizeof(*c));
    c->cancel = to_server ? LAB_CMD_LINK_SERVER : LAB_CMD_LINK_TASK;
    c->opener = lab_slot_of(opener);
    c->opener_gen = opener->generation;
    c->region = peer;
    c->principal_id = server_slot;
    c->budget.poll_quota = server_gen;
    c->budget.priority = (uint8_t)policy;
    return ASX_OK;
}

asx_status asx_lab_watch_unlink_command(uint64_t ref) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    memset(c, 0, sizeof(*c));
    c->cancel = LAB_CMD_UNLINK;
    c->opener = ASX_SLOT_NONE;
    c->region = ref;
    return ASX_OK;
}

/* RegionCommand::RegisterFinalizer for `region` (ChildRegion::
 * retain_until_finalized, child_region.rs:342): the opener's own, which
 * waits for the acknowledgement. */
asx_status asx_lab_retain_command(asx_task_slot *opener, asx_region_id region) {
    lab_region_cmd *c;
    if (g_lab_rcmd_n >= LAB_REGION_CMD_CAP) return ASX_E_RESOURCE_EXHAUSTED;
    c = &g_lab_rcmd[g_lab_rcmd_n++];
    memset(c, 0, sizeof(*c));
    c->cancel = LAB_CMD_RETAIN;
    c->opener = lab_slot_of(opener);
    c->opener_gen = opener->generation;
    c->region = region;
    return ASX_OK;
}

uint64_t asx_lab_task_order_key_internal(asx_task_id task) {
    asx_task_slot *t;
    if (g_lab_active && asx_task_slot_lookup(task, &t) == ASX_OK) return lab_compact_task(t);
    return (uint64_t)asx_handle_index(task);
}

/* -------------------------------------------------------------------
 * Stuck tasks and futurelocks (bd-9kll.9.6)
 *
 * Rust's lab names no task when it runs out of work with tasks parked:
 * run_with_auto_advance ends with AutoAdvanceTermination::StuckBailout
 * after 1000 steps without a dispatch (LR:3228-3305), which this lab's
 * ASX_E_WOULD_BLOCK mirrors, and the stuck tasks are only in its final
 * state. What it does name is a futurelock (LR:5612): a task that holds a
 * pending obligation and has not been polled for more than
 * futurelock_max_idle_steps steps (default 10000, only once the run has
 * taken more steps than that), checked at every step (check_futurelocks,
 * LR:4508), traced as FuturelockDetected and, by default, a panic.
 * asx_lab_futurelocks evaluates that predicate. asx_scheduler_stuck_tasks
 * lists what a run that ended ASX_E_WOULD_BLOCK was waiting on, with the
 * facts the runtime keeps about each wait.
 * ------------------------------------------------------------------- */

/* Pending obligations `t` holds, counted along its held list. */
static uint32_t lab_held_pending(const asx_task_slot *t) {
    uint32_t n = 0;
    uint32_t bound = 0;
    uint32_t idx = t->first_held;
    while (idx != ASX_SLOT_NONE && idx < lab_kernel()->obligation_count &&
           bound++ < lab_kernel()->obligation_count) {
        ASX_CHECKPOINT_WAIVER("bounded: at most obligation_count links");
        if (lab_kernel()->obligations[idx].alive &&
            lab_kernel()->obligations[idx].state == ASX_OBLIGATION_RESERVED)
            n++;
        idx = lab_kernel()->obligations[idx].next_held;
    }
    return n;
}

/* A live task that waits with nothing to run it: under lab dispatch not
 * scheduled and not awaiting an admission or an applied region open, under
 * the sweep parked; in both, no timer is armed to wake it. */
static int lab_task_waiting(const asx_task_slot *t) {
    if (!t->alive || asx_task_is_terminal(t->state)) return 0;
    if (t->timer_armed || t->deadline_timer_armed) return 0;
    if (t->region_wait == ASX_REGION_WAIT_OPEN) return 0;
    if (g_lab_active) return !t->lab_scheduled && !t->lab_admission_pending;
    return t->parked != 0u;
}

/* What task slot `idx` waits on, where the runtime records it: a join, a
 * completion watch (a task group's owner on its members), a region close. */
static void lab_wait_of(const asx_task_slot *t, uint32_t idx, asx_stuck_task *e) {
    uint32_t i;
    e->wait = ASX_STUCK_PARKED;
    e->wait_task = ASX_INVALID_ID;
    e->wait_region = ASX_INVALID_ID;
    if (t->waiting_on != ASX_SLOT_NONE && t->waiting_on < lab_kernel()->task_count &&
        lab_kernel()->tasks[t->waiting_on].alive) {
        e->wait = ASX_STUCK_JOIN;
        e->wait_task = asx_task_handle_for_slot(lab_kernel(), t->waiting_on);
        return;
    }
    if (t->region_wait == ASX_REGION_WAIT_CLOSE) {
        e->wait = ASX_STUCK_REGION_CLOSE;
        e->wait_region = t->region_wait_region;
        return;
    }
    for (i = 0; i < lab_kernel()->task_count; i++) {
        const asx_task_slot *m = &lab_kernel()->tasks[i];
        ASX_CHECKPOINT_WAIVER("bounded: task_count <= ASX_MAX_TASKS");
        if (m->alive && !asx_task_is_terminal(m->state) && m->watcher == idx &&
            m->watcher_gen == t->generation) {
            e->wait = ASX_STUCK_WATCH;
            e->wait_task = asx_task_handle_for_slot(lab_kernel(), i);
            return;
        }
    }
}

static uint64_t lab_idle_steps(const asx_task_slot *t) {
    return g_lab_steps > t->lab_last_polled_step ? g_lab_steps - t->lab_last_polled_step : 0u;
}

uint32_t asx_scheduler_stuck_tasks(asx_region_id region, asx_stuck_task *out, uint32_t cap) {
    uint32_t subtree[ASX_MAX_REGIONS];
    uint32_t n_sub = 0;
    uint32_t total = 0;
    uint32_t i;

    if (!g_lab_active) n_sub = asx_region_subtree_internal(region, subtree, ASX_MAX_REGIONS);
    for (i = 0; i < lab_kernel()->task_count; i++) {
        const asx_task_slot *t = &lab_kernel()->tasks[i];
        ASX_CHECKPOINT_WAIVER("bounded: task_count <= ASX_MAX_TASKS");
        if (!lab_task_waiting(t)) continue;
        if (!g_lab_active) {
            uint32_t r;
            uint32_t slot = (uint32_t)asx_handle_slot(t->region);
            int inside = 0;
            for (r = 0; r < n_sub && !inside; r++) {
                ASX_CHECKPOINT_WAIVER("bounded: n_sub <= ASX_MAX_REGIONS");
                inside = subtree[r] == slot;
            }
            if (!inside) continue;
        }
        if (out != NULL && total < cap) {
            asx_stuck_task *e = &out[total];
            e->task = asx_task_handle_for_slot(lab_kernel(), i);
            e->region = t->region;
            e->state = t->state;
            e->held = lab_held_pending(t);
            e->cancel_requested = t->cancel_pending ? 1u : 0u;
            e->idle_steps = g_lab_active ? lab_idle_steps(t) : 0u;
            lab_wait_of(t, i, e);
        }
        total++;
    }
    return total;
}

/* The futurelock of task slot `i`: the pending obligations it holds, in
 * obligation-arena order as Rust's obligations_iter gives them. Returns 0
 * when it holds none. */
static int lab_futurelock_of(uint32_t i, asx_futurelock *f) {
    const asx_task_slot *t = &lab_kernel()->tasks[i];
    uint32_t held = 0;
    uint32_t o;

    memset(f, 0, sizeof(*f));
    for (o = 0; o < lab_kernel()->obligation_count; o++) {
        const asx_obligation_slot *ob = &lab_kernel()->obligations[o];
        ASX_CHECKPOINT_WAIVER("bounded: obligation_count <= ASX_MAX_OBLIGATIONS");
        if (!ob->alive || ob->state != ASX_OBLIGATION_RESERVED) continue;
        if (ob->holder == ASX_INVALID_ID || asx_handle_slot(ob->holder) != (uint16_t)i ||
            asx_handle_generation(ob->holder) != t->generation) {
            continue;
        }
        if (held < ASX_FUTURELOCK_HELD_MAX) {
            f->held[held] = asx_handle_mint(lab_kernel(), ASX_TYPE_OBLIGATION,
                                            (uint16_t)(1u << (unsigned)ASX_OBLIGATION_RESERVED),
                                            asx_handle_pack_index(ob->generation, (uint16_t)o));
            f->held_kind[held] = ob->kind;
        }
        held++;
    }
    if (held == 0u) return 0;
    f->task = asx_task_handle_for_slot(lab_kernel(), i);
    f->region = t->region;
    f->idle_steps = lab_idle_steps(t);
    f->held_count = held;
    return 1;
}

typedef void (*lab_futurelock_sink)(void *ctx, const asx_futurelock *f);

/* futurelock_violations (LR:5612-5655): calls `sink` for every task Rust
 * reports now, in its order (a BTreeMap by TaskId, the arena index and
 * generation lab_compact_task keeps), and returns how many. A zero
 * threshold disables the check, and no task can pass the threshold before
 * the run has. */
static uint32_t lab_scan_futurelocks(uint64_t max_idle_steps, lab_futurelock_sink sink, void *ctx) {
    uint32_t total = 0;
    uint64_t after_key = 0;
    uint32_t after_slot = ASX_SLOT_NONE; /* ASX_SLOT_NONE: nothing visited yet */

    if (!g_lab_active || max_idle_steps == 0u || g_lab_steps <= max_idle_steps) return 0u;
    for (;;) {
        uint64_t best_key = 0;
        uint32_t best = ASX_SLOT_NONE;
        uint32_t i;
        asx_futurelock f;
        ASX_CHECKPOINT_WAIVER("bounded: each pass visits one more task, at most task_count");
        for (i = 0; i < lab_kernel()->task_count; i++) {
            const asx_task_slot *t = &lab_kernel()->tasks[i];
            uint64_t key;
            ASX_CHECKPOINT_WAIVER("bounded: task_count <= ASX_MAX_TASKS");
            if (!t->alive || asx_task_is_terminal(t->state)) continue;
            if (lab_idle_steps(t) <= max_idle_steps) continue;
            key = lab_compact_task(t);
            if (after_slot != ASX_SLOT_NONE &&
                (key < after_key || (key == after_key && i <= after_slot))) {
                continue; /* already visited */
            }
            if (best == ASX_SLOT_NONE || key < best_key) {
                best = i;
                best_key = key;
            }
        }
        if (best == ASX_SLOT_NONE) break;
        after_key = best_key;
        after_slot = best;
        if (lab_futurelock_of(best, &f)) {
            sink(ctx, &f);
            total++;
        }
    }
    return total;
}

typedef struct {
    asx_futurelock *out;
    uint32_t cap;
    uint32_t n;
} lab_futurelock_collect;

static void lab_futurelock_collect_sink(void *ctx, const asx_futurelock *f) {
    lab_futurelock_collect *c = (lab_futurelock_collect *)ctx;
    if (c->out != NULL && c->n < c->cap) c->out[c->n] = *f;
    c->n++;
}

uint32_t asx_lab_futurelocks(uint64_t max_idle_steps, asx_futurelock *out, uint32_t cap) {
    lab_futurelock_collect c;
    c.out = out;
    c.cap = cap;
    c.n = 0;
    return lab_scan_futurelocks(max_idle_steps, lab_futurelock_collect_sink, &c);
}

/* FuturelockDetected (LR:5702-5714): the task, its idle steps and, in the
 * payload, its region and held obligations. */
static void lab_futurelock_emit_sink(void *ctx, const asx_futurelock *f) {
    asx_trace_payload payload = {0};
    (void)ctx;
    payload.futurelock = f;
    asx_trace_emit_payload_internal(ASX_TRACE_LAB_FUTURELOCK, (uint64_t)f->task, f->idle_steps,
                                    &payload);
}

asx_status asx_lab_set_futurelock_check(uint64_t max_idle_steps) {
    if (!g_lab_active) return ASX_E_INVALID_STATE;
    g_lab_fl_max = max_idle_steps;
    return ASX_OK;
}

/* The step's draw (LR:4507) and then check_futurelocks (LR:4508), before the
 * timers fire. */
uint64_t asx_lab_step_draw_internal(void) {
    uint64_t r = lab_xorshift(&g_lab_rng);
    if (g_lab_fl_max != 0u) {
        (void)lab_scan_futurelocks(g_lab_fl_max, lab_futurelock_emit_sink, NULL);
    }
    return r;
}
