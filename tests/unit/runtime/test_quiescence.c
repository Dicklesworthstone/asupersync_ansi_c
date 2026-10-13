/*
 * test_quiescence.c — focused unit tests for quiescence.c
 *
 * Exercises branch-level quiescence/finalization behavior that broader
 * lifecycle suites only cover indirectly.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/runtime/runtime_internal.h"
#include "../../test_harness.h"
#include <asx/asx.h>
#include <asx/core/cleanup.h>
#include <asx/core/ghost.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/trace.h>

/* Suppress warn_unused_result for intentionally-ignored scheduler calls. */
#define SCHED_RUN_IGNORE(rid, bud)                                                                 \
    do {                                                                                           \
        asx_status s_ = asx_scheduler_run((rid), (bud));                                           \
        (void)s_;                                                                                  \
    } while (0)

/* A fresh runtime with the opt-in hard cleanup bound
 * (asx_runtime_config.cleanup_hard_bound): draining a tree of tasks that
 * never finish relies on it (Rust never force-completes). */
static asx_status reset_with_hard_cleanup_bound(void) {
    static asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    asx_runtime_config_init(&cfg);
    cfg.cleanup_hard_bound = 1u;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    return st;
}

static asx_status poll_pending(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_E_PENDING;
}

static asx_status poll_complete(void *data, asx_task_id self) {
    (void)data;
    (void)self;
    return ASX_OK;
}

static asx_status poll_checkpoint_then_complete(void *data, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)data;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) { return ASX_OK; }
    return ASX_E_PENDING;
}

/* Every obligation has a holder task and only that task's own poll settles
 * it, so a test that needs a held obligation runs it through this task: it
 * reserves one obligation of `region` on its first poll and parks holding it.
 * Once `release` is set and the task woken, or when it is cancelled, it ends
 * the hold in its own poll, by committing the obligation, aborting it, or
 * completing with it still reserved (a leak under the runtime's policy). */
enum { HOLDER_LEAKS, HOLDER_COMMITS, HOLDER_ABORTS };

typedef struct {
    asx_region_id region;
    asx_obligation_id obligation;
    int ends;
    int release;
    asx_status reserve_status;
    asx_status settle_status;
} obligation_holder_ctx;

static void obligation_holder_init(obligation_holder_ctx *holder, asx_region_id region, int ends,
                                   int release) {
    holder->region = region;
    holder->obligation = ASX_INVALID_ID;
    holder->ends = ends;
    holder->release = release;
    holder->reserve_status = ASX_E_PENDING;
    holder->settle_status = ASX_E_PENDING;
}

static asx_status poll_obligation_holder(void *data, asx_task_id self) {
    obligation_holder_ctx *holder = (obligation_holder_ctx *)data;
    asx_checkpoint_result cr;

    if (holder->obligation == ASX_INVALID_ID) {
        holder->reserve_status = asx_obligation_reserve(holder->region, &holder->obligation);
        if (holder->reserve_status != ASX_OK) return holder->reserve_status;
    }
    if (!holder->release && !(asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled)) {
        return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    }
    if (holder->ends == HOLDER_COMMITS) {
        holder->settle_status = asx_obligation_commit(holder->obligation);
    } else if (holder->ends == HOLDER_ABORTS) {
        holder->settle_status = asx_obligation_abort(holder->obligation);
    }
    return ASX_OK;
}

static void cleanup_mark(void *ctx) {
    int *flag = (int *)ctx;
    *flag += 1;
}

typedef struct {
    asx_region_id region;
    asx_status spawn_status;
    asx_task_id task_id;
} cleanup_spawn_task_ctx;

static void cleanup_spawn_immediate_task(void *ctx) {
    cleanup_spawn_task_ctx *spawn = (cleanup_spawn_task_ctx *)ctx;

    if (spawn == NULL) return;
    spawn->task_id = ASX_INVALID_ID;
    spawn->spawn_status =
        asx_task_spawn_cleanup(spawn->region, poll_complete, NULL, &spawn->task_id);
}

static void finalizer_spawn_ordinary_task(void *ctx) {
    cleanup_spawn_task_ctx *spawn = (cleanup_spawn_task_ctx *)ctx;
    spawn->spawn_status = asx_task_spawn(spawn->region, poll_complete, NULL, &spawn->task_id);
}

typedef struct {
    int *polls;
    int *drops;
} captured_cleanup_state;

typedef struct {
    cleanup_spawn_task_ctx spawn;
    int polls;
    int drops;
} captured_cleanup_ctx;

static asx_status poll_captured_cleanup(void *data, asx_task_id self) {
    captured_cleanup_state *state = (captured_cleanup_state *)data;
    (void)self;
    (*state->polls)++;
    return ASX_OK;
}

static void drop_captured_cleanup(void *data, uint32_t size) {
    captured_cleanup_state *state = (captured_cleanup_state *)data;
    if (size == sizeof(*state)) (*state->drops)++;
}

static void finalizer_spawn_captured_cleanup(void *data) {
    captured_cleanup_ctx *ctx = (captured_cleanup_ctx *)data;
    captured_cleanup_state *state = NULL;
    ctx->spawn.spawn_status =
        asx_task_spawn_cleanup_captured(ctx->spawn.region, poll_captured_cleanup, sizeof(*state),
                                        drop_captured_cleanup, &ctx->spawn.task_id,
                                        (void **)&state);
    if (ctx->spawn.spawn_status == ASX_OK) {
        state->polls = &ctx->polls;
        state->drops = &ctx->drops;
    }
}

static void log_child_summary(const char *label, asx_region_id parent,
                              asx_region_slot *parent_slot) {
    fprintf(stderr, "[child-test] %s parent=0x%016llx child_count=%u state=%d\n", label,
            (unsigned long long)parent, parent_slot->child_count, (int)parent_slot->state);
}

static void log_child_link(const char *label, asx_region_id region, asx_region_slot *region_slot,
                           asx_region_id expected_parent) {
    fprintf(stderr, "[child-test] %s region=0x%016llx parent=0x%016llx state=%d\n", label,
            (unsigned long long)region, (unsigned long long)expected_parent,
            (int)region_slot->state);
}

TEST(quiescence_closed_region_with_live_tasks_reports_tasks_live) {
    asx_region_id rid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 1;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_TASKS_LIVE);
}

TEST(quiescence_closed_region_with_unresolved_obligation_reports_error) {
    asx_region_id rid;
    asx_task_id holder;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* The obligation's holder is a live task that is never polled. */
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &holder), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, holder, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_OBLIGATIONS_UNRESOLVED);
}

TEST(region_drain_advances_draining_region_to_closed_and_runs_cleanup) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;
    asx_budget budget;
    asx_trace_event ev;
    uint32_t event_count;
    int cleaned = 0;

    asx_runtime_reset();
    asx_ghost_reset();
    asx_trace_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_DRAINING;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(cleaned, 1);
    event_count = asx_trace_event_count();
    ASSERT_TRUE(event_count > 0u);
    ASSERT_TRUE(asx_trace_event_get(event_count - 1u, &ev));
    ASSERT_EQ(ev.kind, ASX_TRACE_REGION_CLOSED);
    ASSERT_EQ(ev.entity_id, rid);
}

TEST(region_drain_recancels_uncancelled_tasks_before_scheduler_run) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);

    budget = asx_budget_from_polls(1);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_RUNNING);

    budget = asx_budget_from_polls(16);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(region_drain_budget_exhaustion_leaves_region_closing) {
    asx_region_id rid;
    asx_task_id tid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    budget = asx_budget_from_polls(0);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(region->state, ASX_REGION_CLOSING);
    ASSERT_TRUE(region->task_count > 0);
}

TEST(region_finalizer_cannot_spawn_into_its_finalizing_region) {
    /* Ordinary spawn stays Open-only even inside a finalizer. */
    asx_region_id rid;
    asx_region_slot *region = NULL;
    cleanup_spawn_task_ctx spawn_ctx;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    spawn_ctx.region = rid;
    spawn_ctx.spawn_status = ASX_E_INVALID_STATE;
    spawn_ctx.task_id = ASX_INVALID_ID;
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_spawn_ordinary_task, &spawn_ctx), ASX_OK);

    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(spawn_ctx.spawn_status, ASX_E_REGION_CLOSED);
    ASSERT_EQ(spawn_ctx.task_id, ASX_INVALID_ID);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(region->task_count, 0u);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(public_finalizer_cleanup_task_delays_close_until_it_completes) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    cleanup_spawn_task_ctx spawn_ctx;
    asx_task_state task_state;
    asx_outcome outcome;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    spawn_ctx.region = rid;
    spawn_ctx.spawn_status = ASX_E_INVALID_STATE;
    spawn_ctx.task_id = ASX_INVALID_ID;
    ASSERT_EQ(asx_region_defer_sync(rid, cleanup_spawn_immediate_task, &spawn_ctx), ASX_OK);
    budget = asx_budget_from_polls(8);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(spawn_ctx.spawn_status, ASX_OK);
    ASSERT_NE(spawn_ctx.task_id, ASX_INVALID_ID);
    ASSERT_EQ(region->state, ASX_REGION_FINALIZING);
    ASSERT_EQ(region->task_count, 1u);
    ASSERT_EQ(asx_task_get_state(spawn_ctx.task_id, &task_state), ASX_OK);
    ASSERT_EQ(task_state, ASX_TASK_CREATED);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(spawn_ctx.task_id, &task_state), ASX_OK);
    ASSERT_EQ(task_state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_outcome(spawn_ctx.task_id, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(spawn_admission_per_region_state) {
    /* Rust add_task admits only into an Open region; every other state
     * refuses with SpawnError::RegionClosed. */
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_task_id tid;
    static const asx_region_state refused[4] = {ASX_REGION_CLOSING, ASX_REGION_DRAINING,
                                                ASX_REGION_FINALIZING, ASX_REGION_CLOSED};
    uint32_t i;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    for (i = 0; i < 4u; i++) {
        asx_region_state saved = region->state;
        region->state = refused[i];
        ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_E_REGION_CLOSED);
        region->state = saved;
    }
}

/* -------------------------------------------------------------------
 * asx_region_defer_sync: Rust's Scope::defer_sync (bd-9kll.3.4)
 * ------------------------------------------------------------------- */

typedef struct {
    int order[8];
    int n;
} finalizer_log;

static finalizer_log g_finalizer_log;

static void finalizer_record_1(void *ud) {
    (void)ud;
    if (g_finalizer_log.n < 8) g_finalizer_log.order[g_finalizer_log.n++] = 1;
}
static void finalizer_record_2(void *ud) {
    (void)ud;
    if (g_finalizer_log.n < 8) g_finalizer_log.order[g_finalizer_log.n++] = 2;
}
static void finalizer_record_3(void *ud) {
    (void)ud;
    if (g_finalizer_log.n < 8) g_finalizer_log.order[g_finalizer_log.n++] = 3;
}

TEST(defer_sync_finalizers_run_once_in_lifo_order_at_close) {
    asx_region_id rid;
    asx_region_state st;
    asx_budget budget;

    asx_runtime_reset();
    memset(&g_finalizer_log, 0, sizeof(g_finalizer_log));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_1, NULL), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_2, NULL), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_3, NULL), ASX_OK);
    ASSERT_EQ(g_finalizer_log.n, 0); /* nothing runs before the region finalizes */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &st), ASX_OK);
    ASSERT_EQ(st, ASX_REGION_CLOSED);
    ASSERT_EQ(g_finalizer_log.n, 3);
    ASSERT_EQ(g_finalizer_log.order[0], 3);
    ASSERT_EQ(g_finalizer_log.order[1], 2);
    ASSERT_EQ(g_finalizer_log.order[2], 1);
    /* A drain of the closed region runs nothing again. */
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(g_finalizer_log.n, 3);
}

TEST(defer_sync_runs_after_the_region_tasks_complete) {
    /* Finalizers run in Finalizing, after the region's tasks are done
     * (Rust register_sync_finalizer docs, runtime/state.rs:9032-9045). */
    asx_region_id rid;
    asx_task_id tid;
    asx_task_state ts;
    asx_budget budget;

    asx_runtime_reset();
    memset(&g_finalizer_log, 0, sizeof(g_finalizer_log));
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_1, NULL), ASX_OK);
    budget = asx_budget_from_polls(16);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &ts), ASX_OK);
    ASSERT_EQ(ts, ASX_TASK_COMPLETED);
    ASSERT_EQ(g_finalizer_log.n, 1);
}

TEST(defer_sync_refused_once_the_region_is_closing) {
    /* Rust refuses a finalizer for a closing or terminal region
     * (runtime/state.rs:9056-9062). */
    asx_region_id rid;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_1, NULL), ASX_E_REGION_CLOSED);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_1, NULL), ASX_E_REGION_CLOSED);
}

/* -------------------------------------------------------------------
 * asx_region_defer_async: Rust's Scope::defer_async (bd-9kll.3.24)
 * ------------------------------------------------------------------- */

typedef struct {
    int id;
    int polls_to_finish; /* PENDING until this many polls, then complete */
    int polls;
    asx_task_id task;
    uint32_t mask_depth; /* the mask depth its first poll saw */
    asx_task_state state;
    asx_status ordinary_spawn;
    asx_status cleanup_spawn;
} async_fin;

static int g_async_events[16];
static int g_async_event_n;

static void async_event(int e) {
    if (g_async_event_n < 16) g_async_events[g_async_event_n++] = e;
}

/* Event id*10 on its first poll, id*10+1 on its last. */
static asx_status async_fin_poll(void *ud, asx_task_id self) {
    async_fin *f = (async_fin *)ud;
    if (f->polls == 0) {
        asx_status st = asx_task_mask_depth(self, &f->mask_depth);
        (void)st;
        f->task = self;
        async_event(f->id * 10);
    }
    f->polls++;
    if (f->polls < f->polls_to_finish) return ASX_E_PENDING;
    async_event(f->id * 10 + 1);
    return ASX_OK;
}

static void async_sync_finalizer(void *ud) { async_event(*(int *)ud); }

/* A finalizer that tries to spawn work into its own FINALIZING region. */
static asx_status async_fin_spawner_poll(void *ud, asx_task_id self) {
    async_fin *f = (async_fin *)ud;
    asx_task_id child = ASX_INVALID_ID;
    asx_region_id region;
    asx_status st = asx_task_get_region(self, &region);
    if (st != ASX_OK) return st;
    f->ordinary_spawn = asx_task_spawn(region, poll_complete, NULL, &child);
    f->cleanup_spawn = asx_task_spawn_cleanup(region, poll_complete, NULL, &child);
    f->polls++;
    return ASX_OK;
}

/* The log lines the finalizer escalation wrote. */
static int g_fin_log_lines;

static void fin_log_sink(void *ctx, int level, const char *message) {
    (void)ctx;
    (void)level;
    if (message != NULL && strstr(message, "async finalizer") != NULL) g_fin_log_lines++;
}

/* A runtime whose config carries the finalizer budget and escalation, and
 * whose log sink counts the finalizer's lines. */
static asx_status init_with_finalizer_budget(uint32_t polls, asx_finalizer_escalation esc) {
    static asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    if (asx_runtime_is_initialized(&rt)) asx_runtime_shutdown(&rt);
    asx_runtime_config_init(&cfg);
    cfg.finalizer_poll_budget = polls;
    cfg.finalizer_escalation = esc;
    st = asx_runtime_hooks_init(&hooks);
    hooks.log.write_fn = fin_log_sink;
    g_fin_log_lines = 0;
    if (st == ASX_OK) st = asx_runtime_init(&rt, &cfg, &hooks);
    return st;
}

TEST(defer_async_finalizer_runs_as_a_masked_cleanup_task_before_close) {
    asx_region_id rid;
    asx_region_state rs;
    asx_budget budget;
    async_fin f;
    asx_outcome out;

    asx_runtime_reset();
    memset(&f, 0, sizeof(f));
    f.id = 1;
    f.polls_to_finish = 3;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_OK);
    ASSERT_EQ(f.polls, 0); /* nothing runs before the region finalizes */
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED);
    ASSERT_EQ(f.polls, 3);
    ASSERT_EQ(f.mask_depth, 1u);
    ASSERT_NE(f.task, ASX_INVALID_ID);
    ASSERT_EQ(asx_task_get_state(f.task, &f.state), ASX_OK);
    ASSERT_EQ(f.state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_outcome(f.task, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
}

TEST(defer_async_finalizers_run_one_at_a_time_in_lifo_order_with_sync_ones) {
    asx_region_id rid;
    asx_budget budget;
    async_fin first;
    async_fin third;
    int sync_mark = 2;

    asx_runtime_reset();
    g_async_event_n = 0;
    memset(&first, 0, sizeof(first));
    memset(&third, 0, sizeof(third));
    first.id = 1;
    first.polls_to_finish = 2;
    third.id = 3;
    third.polls_to_finish = 2;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &first), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, async_sync_finalizer, &sync_mark), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &third), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    /* LIFO: the last registered runs first and finishes before the next one
     * starts (the barrier); the sync one runs between the two. */
    ASSERT_EQ(g_async_event_n, 5);
    ASSERT_EQ(g_async_events[0], 30);
    ASSERT_EQ(g_async_events[1], 31);
    ASSERT_EQ(g_async_events[2], 2);
    ASSERT_EQ(g_async_events[3], 10);
    ASSERT_EQ(g_async_events[4], 11);
    ASSERT_EQ(asx_cleanup_pending(&g_kernels[0]->regions[asx_handle_slot(rid)].cleanup), 0u);
}

TEST(defer_async_region_finalizing_refuses_ordinary_spawn_but_takes_cleanup) {
    asx_region_id rid;
    asx_region_state rs;
    asx_budget budget;
    async_fin f;

    asx_runtime_reset();
    memset(&f, 0, sizeof(f));
    f.ordinary_spawn = ASX_OK;
    f.cleanup_spawn = ASX_E_INVALID_STATE;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_spawner_poll, &f), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(f.ordinary_spawn, ASX_E_REGION_CLOSED);
    ASSERT_EQ(f.cleanup_spawn, ASX_OK);
    /* The close waited for the cleanup task the finalizer spawned. */
    ASSERT_EQ(asx_region_get_state(rid, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED);
}

TEST(defer_async_runs_after_the_region_tasks_complete) {
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    async_fin f;
    asx_task_state ts;

    asx_runtime_reset();
    memset(&f, 0, sizeof(f));
    f.id = 1;
    f.polls_to_finish = 1;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_OK);
    budget = asx_budget_from_polls(32);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_state(tid, &ts), ASX_OK);
    ASSERT_EQ(ts, ASX_TASK_COMPLETED);
    ASSERT_EQ(f.polls, 1);
}

TEST(defer_async_wait_closed_drives_a_region_with_nothing_but_finalizers) {
    asx_region_id rid;
    asx_region_state rs;
    asx_budget budget;
    async_fin f;

    asx_runtime_reset();
    memset(&f, 0, sizeof(f));
    f.id = 1;
    f.polls_to_finish = 2;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_wait_closed(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_state(rid, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED);
    ASSERT_EQ(f.polls, 2);
}

TEST(defer_async_default_escalation_lets_a_finalizer_run_past_its_poll_budget) {
    asx_region_id rid;
    asx_budget budget;
    async_fin f;
    asx_outcome out;
    asx_finalizer_escalation modes[2];
    uint32_t m;

    modes[0] = ASX_FINALIZER_BOUNDED_LOG;
    modes[1] = ASX_FINALIZER_SOFT;
    for (m = 0; m < 2u; m++) {
        ASSERT_EQ(init_with_finalizer_budget(3u, modes[m]), ASX_OK);
        memset(&f, 0, sizeof(f));
        f.id = 1;
        f.polls_to_finish = 6;
        ASSERT_EQ(asx_region_open(&rid), ASX_OK);
        ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_OK);
        budget = asx_budget_infinite();
        ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
        /* Rust's finalizer budget is advisory: it runs to completion, Ok.
         * BOUNDED_LOG notes the first poll past the budget once, SOFT is
         * silent. */
        ASSERT_EQ(f.polls, 6);
        ASSERT_EQ(g_fin_log_lines, modes[m] == ASX_FINALIZER_BOUNDED_LOG ? 1 : 0);
        ASSERT_EQ(asx_task_get_outcome(f.task, &out), ASX_OK);
        ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
        ASSERT_EQ(asx_region_get_close_outcome(rid, &out), ASX_OK);
        ASSERT_EQ(out.severity, ASX_OUTCOME_OK);
    }
}

TEST(defer_async_bounded_panic_retires_the_finalizer_panicked_at_its_poll_budget) {
    asx_region_id rid;
    asx_region_state rs;
    asx_budget budget;
    async_fin f;
    asx_outcome out;

    ASSERT_EQ(init_with_finalizer_budget(3u, ASX_FINALIZER_BOUNDED_PANIC), ASX_OK);
    memset(&f, 0, sizeof(f));
    f.id = 1;
    f.polls_to_finish = 10;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    /* Three polls were its budget; the fourth never reached the finalizer,
     * and the log noted it. */
    ASSERT_EQ(f.polls, 3);
    ASSERT_EQ(g_fin_log_lines, 1);
    ASSERT_EQ(asx_region_get_state(rid, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_get_outcome(f.task, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_PANICKED);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &out), ASX_OK);
    ASSERT_EQ(out.severity, ASX_OUTCOME_PANICKED);
}

TEST(defer_async_argument_errors_and_registration_states) {
    asx_region_id rid;
    asx_budget budget;
    async_fin f;
    uint32_t i;

    asx_runtime_reset();
    memset(&f, 0, sizeof(f));
    f.polls_to_finish = 1;
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_NE(asx_region_defer_async(ASX_INVALID_ID, async_fin_poll, &f), ASX_OK);
    /* The stack is full at its capacity. Its entries are never run: a
     * resource class may have fewer tasks than the stack has entries. */
    for (i = 0; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_OK);
    }
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_cleanup_async_pending(&g_kernels[0]->regions[asx_handle_slot(rid)].cleanup),
              (uint32_t)ASX_CLEANUP_STACK_CAPACITY);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_E_REGION_CLOSED);

    /* A closed region refuses one too. */
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_close(rid), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_E_REGION_CLOSED);
    ASSERT_EQ(f.polls, 0);

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_async(rid, async_fin_poll, &f), ASX_E_REGION_POISONED);
}

/* -------------------------------------------------------------------
 * asx_region_get_close_outcome: Rust's region_close_outcome (bd-9kll.3.4)
 * ------------------------------------------------------------------- */

static asx_status poll_fails(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_INVALID_STATE;
}

static asx_status poll_completes(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

TEST(close_outcome_joins_the_task_outcomes) {
    /* Every completed task's outcome joins the region's (record_close_
     * outcome, record/region.rs:1144-1150): Ok and Err give Err. */
    asx_region_id rid;
    asx_task_id t1;
    asx_task_id t2;
    asx_outcome o;
    asx_budget budget;
    asx_status st;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &o), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_task_spawn(rid, poll_completes, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_fails, NULL, &t2), ASX_OK);
    budget = asx_budget_from_polls(16);
    st = asx_scheduler_run(rid, &budget); /* the failure may surface (fail-fast) */
    (void)st;
    ASSERT_EQ(asx_region_get_close_outcome(rid, &o), ASX_OK); /* known before close */
    ASSERT_EQ(o.severity, ASX_OUTCOME_ERR);
    budget = asx_budget_infinite();
    st = asx_region_drain(rid, &budget);
    (void)st;
    ASSERT_EQ(asx_region_get_close_outcome(rid, &o), ASX_OK);
    ASSERT_EQ(o.severity, ASX_OUTCOME_ERR);
}

TEST(close_outcome_of_a_cancelled_empty_region_is_cancelled) {
    /* complete_close's default (record/region.rs:1810-1823): no task
     * outcome and a cancel reason give Cancelled; no cancel gives Ok. */
    asx_region_id cancelled;
    asx_region_id plain;
    asx_cancel_reason reason;
    asx_region_state rs;
    asx_outcome o;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&cancelled), ASX_OK);
    reason = asx_cancel_reason_default(ASX_CANCEL_USER, "test");
    ASSERT_EQ(asx_region_cancel(cancelled, &reason, NULL), ASX_OK);
    ASSERT_EQ(asx_region_get_state(cancelled, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED); /* no live work: it finalizes at once */
    ASSERT_EQ(asx_region_get_close_outcome(cancelled, &o), ASX_OK);
    ASSERT_EQ(o.severity, ASX_OUTCOME_CANCELLED);

    ASSERT_EQ(asx_region_open(&plain), ASX_OK);
    ASSERT_EQ(asx_region_close(plain), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(plain, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_close_outcome(plain, &o), ASX_OK);
    ASSERT_EQ(o.severity, ASX_OUTCOME_OK);
}

TEST(close_outcome_of_a_cancelled_region_keeps_its_task_outcomes) {
    /* The cancel default applies only when no task outcome was recorded
     * (get_or_insert, record/region.rs:1823): a task that completed Ok
     * before the region was cancelled leaves it Ok, as in Rust. */
    asx_region_id rid;
    asx_task_id t;
    asx_cancel_reason reason;
    asx_outcome o;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_completes, NULL, &t), ASX_OK);
    budget = asx_budget_from_polls(4);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    reason = asx_cancel_reason_default(ASX_CANCEL_USER, "test");
    ASSERT_EQ(asx_region_cancel(rid, &reason, NULL), ASX_OK);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &o), ASX_OK);
    ASSERT_EQ(o.severity, ASX_OUTCOME_OK);
}

TEST(drain_after_region_cancel_closes_with_the_requested_reason) {
    /* Rust's close command is cancel_request(reason) (close_region_command,
     * runtime/state.rs:4936-4940): the region's own tasks take that reason,
     * descendants' tasks ParentCancelled. In C, asx_region_cancel then
     * asx_region_drain: drain finds every task cancelled already and adds
     * no PARENT cancel of its own, only runs them out and closes. */
    asx_region_id rid;
    asx_region_id child;
    asx_task_id root_task;
    asx_task_id child_task;
    asx_cancel_reason reason;
    asx_cancel_reason seen;
    asx_outcome o;
    asx_region_state rs;
    asx_budget budget;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_open_child(rid, &child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &root_task), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, poll_checkpoint_then_complete, NULL, &child_task), ASX_OK);
    reason = asx_cancel_reason_default(ASX_CANCEL_SHUTDOWN, NULL);
    ASSERT_EQ(asx_region_cancel(rid, &reason, NULL), ASX_OK);
    budget = asx_budget_infinite();
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_region_get_state(rid, &rs), ASX_OK);
    ASSERT_EQ(rs, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_get_cancel_reason(root_task, &seen), ASX_OK);
    ASSERT_EQ((int)seen.kind, (int)ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(asx_task_get_cancel_reason(child_task, &seen), ASX_OK);
    ASSERT_EQ((int)seen.kind, (int)ASX_CANCEL_PARENT);
    ASSERT_EQ(asx_region_get_close_outcome(rid, &o), ASX_OK);
    ASSERT_EQ(o.severity, ASX_OUTCOME_CANCELLED);
}

TEST(close_outcome_argument_errors) {
    asx_region_id rid;
    asx_outcome o;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_get_close_outcome(rid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_NE(asx_region_get_close_outcome(ASX_INVALID_ID, &o), ASX_OK);
}

TEST(defer_sync_argument_errors_and_capacity) {
    asx_region_id rid;
    uint32_t i;

    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_NE(asx_region_defer_sync(ASX_INVALID_ID, finalizer_record_1, NULL), ASX_OK);
    for (i = 0; i < (uint32_t)ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_1, NULL), ASX_OK);
    }
    ASSERT_EQ(asx_region_defer_sync(rid, finalizer_record_1, NULL), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(finalizing_region_refuses_task_spawn) {
    /* Rust can_accept_work is Open only (record/region.rs:134-142). */
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_task_id tid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_task_spawn(rid, poll_complete, NULL, &tid), ASX_E_REGION_CLOSED);
    ASSERT_EQ(region->task_count, 0u);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_OK);
    ASSERT_EQ(region->task_count, 1u);
}

TEST(finalizing_region_refuses_captured_task_spawn) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_task_id tid;
    uint32_t *captured = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_task_spawn_captured(rid, poll_complete, sizeof(*captured), NULL, &tid,
                                      (void **)&captured),
              ASX_E_REGION_CLOSED);
    ASSERT_TRUE(captured == NULL);
    ASSERT_EQ(region->task_count, 0u);
    ASSERT_EQ(region->capture_used, 0u); /* nothing allocated */
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, sizeof(*captured), NULL, &tid,
                                              (void **)&captured),
              ASX_OK);
    ASSERT_TRUE(captured != NULL);
    ASSERT_EQ(*captured, 0u);
    ASSERT_EQ(region->task_count, 1u);
}

TEST(public_finalizer_captured_cleanup_executes_and_drops_once) {
    captured_cleanup_ctx ctx;
    asx_budget budget;
    asx_outcome outcome;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&ctx.spawn.region), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.spawn.region, finalizer_spawn_captured_cleanup, &ctx),
              ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(ctx.spawn.region, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(ctx.spawn.spawn_status, ASX_OK);
    ASSERT_EQ(ctx.polls, 0);
    ASSERT_EQ(ctx.drops, 0);
    ASSERT_EQ(asx_region_drain(ctx.spawn.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.drops, 1);
    ASSERT_EQ(asx_task_get_outcome(ctx.spawn.task_id, &outcome), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&outcome), ASX_OUTCOME_OK);
    ASSERT_EQ(asx_quiescence_check(ctx.spawn.region), ASX_OK);
    ASSERT_EQ(asx_region_drain(ctx.spawn.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.drops, 1);
    asx_runtime_reset();
    ASSERT_EQ(ctx.drops, 1);
}

TEST(finalizing_region_still_rejects_obligation_reserve) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_obligation_id oid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;

    ASSERT_EQ(asx_obligation_reserve(rid, &oid), ASX_E_REGION_CLOSED);
}

TEST(region_open_child_null_out_fails) {
    asx_region_id parent;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(region_open_child_invalid_parent_returns_not_found) {
    asx_region_id child;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open_child(ASX_INVALID_ID, &child), ASX_E_NOT_FOUND);
}

TEST(region_open_child_rejects_non_open_parent) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->state = ASX_REGION_CLOSING;

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_CLOSED);
}

TEST(region_open_child_rejects_closed_parent) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->state = ASX_REGION_CLOSED;
    log_child_summary("closed-parent", parent, parent_slot);

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_CLOSED);
}

TEST(region_open_child_rejects_poisoned_parent) {
    asx_region_id parent;
    asx_region_id child;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_poison(parent), ASX_OK);

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_REGION_POISONED);
}

TEST(region_open_child_rejects_full_child_list) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->child_count = ASX_MAX_REGION_CHILDREN;

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(region_open_child_sets_parent_and_tracks_child) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(child_slot->state, ASX_REGION_OPEN);
}

TEST(region_open_multiple_children_tracks_all_children) {
    asx_region_id parent;
    asx_region_id child1;
    asx_region_id child2;
    asx_region_id child3;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child1), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child2), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child3), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    log_child_summary("multi-open", parent, parent_slot);

    ASSERT_EQ(parent_slot->child_count, 3u);
    ASSERT_EQ(parent_slot->children[0], child1);
    ASSERT_EQ(parent_slot->children[1], child2);
    ASSERT_EQ(parent_slot->children[2], child3);

    ASSERT_EQ(asx_region_slot_lookup(child1, &child_slot), ASX_OK);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(asx_region_slot_lookup(child2, &child_slot), ASX_OK);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(asx_region_slot_lookup(child3, &child_slot), ASX_OK);
    ASSERT_EQ(child_slot->parent_id, parent);
}

TEST(region_open_child_real_capacity_exhaustion) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    uint32_t i;
    uint32_t opened = 0u;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    for (i = 0; i < ASX_MAX_REGION_CHILDREN; i++) {
        if (asx_region_open_child(parent, &child) != ASX_OK) break;
        opened++;
    }
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    log_child_summary("real-max", parent, parent_slot);
    /* Whichever runs out first: the region arena (the parent holds one
     * slot) or the parent's child slots (a classed build can have more
     * regions than child slots). */
    ASSERT_EQ(opened, (uint32_t)(ASX_MAX_REGIONS - 1) < (uint32_t)ASX_MAX_REGION_CHILDREN
                          ? (uint32_t)(ASX_MAX_REGIONS - 1)
                          : (uint32_t)ASX_MAX_REGION_CHILDREN);
    ASSERT_EQ(parent_slot->child_count, opened);

    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_E_RESOURCE_EXHAUSTED);
}

TEST(region_drain_closed_child_unlinks_from_parent) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    child_slot->state = ASX_REGION_FINALIZING;
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);

    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    /* Unlinked from the parent's children; it still names its parent,
     * which keeps its slot from being recycled while the parent lives. */
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(parent_slot->child_count, 0u);
    ASSERT_EQ(parent_slot->children[0], ASX_INVALID_ID);
}

TEST(region_drain_closed_child_tolerates_missing_parent_slot) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    parent_slot->alive = 0;
    child_slot->state = ASX_REGION_FINALIZING;
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);

    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
}

TEST(region_drain_parent_closes_open_child_first) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);

    /* Draining the parent is a structured shutdown of its subtree: the
     * open child is closed and finalized first, then the parent. */
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(parent_slot->child_count, 0u);
    ASSERT_EQ(parent_slot->state, ASX_REGION_CLOSED);
}

static asx_status q_poll_pending(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_E_PENDING;
}

TEST(region_drain_cancels_descendant_tasks_with_parent_attribution) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_id grandchild;
    asx_task_id t_child;
    asx_task_id t_grand;
    asx_outcome out;
    asx_budget budget;

    ASSERT_EQ(reset_with_hard_cleanup_bound(), ASX_OK);

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_open_child(child, &grandchild), ASX_OK);
    ASSERT_EQ(asx_task_spawn(child, q_poll_pending, NULL, &t_child), ASX_OK);
    ASSERT_EQ(asx_task_spawn(grandchild, q_poll_pending, NULL, &t_grand), ASX_OK);

    /* Never-finishing tasks deep in the tree are cancelled and, under the
     * hard bound, force-completed within their cleanup budget; every
     * region closes. */
    budget = asx_budget_from_polls(10000);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_outcome(t_child, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_outcome(t_grand, &out), ASX_OK);
    ASSERT_EQ(asx_outcome_severity_of(&out), ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_region_is_quiescent(parent), 1);
}

/* A closed child of a live parent keeps its slot while others are free, so
 * the handle its opener holds still names a closed region (Rust refuses a
 * spawn into a recently closed region as RegionClosed; fuzz gen-26-180);
 * only a full arena recycles it. */
TEST(closed_child_of_live_parent_keeps_its_slot) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_id other;
    asx_region_state state;
    asx_task_id tid;
    asx_budget budget;
    uint32_t i;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&other), ASX_OK);

    ASSERT_EQ(asx_region_get_state(child, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_spawn(child, q_poll_pending, NULL, &tid), ASX_E_REGION_CLOSED);

    /* Fill the arena: the closed child is recycled only when no other slot
     * is left, and its handle then goes stale. */
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        asx_region_id r;
        ASSERT_EQ(asx_region_open(&r), ASX_OK);
        if (asx_region_get_state(child, &state) == ASX_E_STALE_HANDLE) break;
    }
    ASSERT_TRUE(i < ASX_MAX_REGIONS);
    ASSERT_EQ(asx_task_spawn(child, q_poll_pending, NULL, &tid), ASX_E_STALE_HANDLE);
}

TEST(region_drain_nested_grandchild_closes_inside_out) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_id grandchild;
    asx_region_slot *parent_slot = NULL;
    asx_region_slot *child_slot = NULL;
    asx_region_slot *grandchild_slot = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_open_child(child, &grandchild), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(child, &child_slot), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(grandchild, &grandchild_slot), ASX_OK);
    log_child_summary("nested-parent-before", parent, parent_slot);
    log_child_summary("nested-child-before", child, child_slot);
    log_child_link("nested-child-link", child, child_slot, parent);
    log_child_link("nested-grandchild-link", grandchild, grandchild_slot, child);

    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(grandchild_slot->parent_id, child);
    ASSERT_EQ(parent_slot->child_count, 1u);
    ASSERT_EQ(child_slot->child_count, 1u);
    ASSERT_EQ(parent_slot->children[0], child);
    ASSERT_EQ(child_slot->children[0], grandchild);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(grandchild, &budget), ASX_OK);
    log_child_summary("nested-child-after-grandchild", child, child_slot);
    ASSERT_EQ(grandchild_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(grandchild_slot->parent_id, child);
    ASSERT_EQ(child_slot->child_count, 0u);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child, &budget), ASX_OK);
    log_child_summary("nested-parent-after-child", parent, parent_slot);
    ASSERT_EQ(child_slot->state, ASX_REGION_CLOSED);
    ASSERT_EQ(child_slot->parent_id, parent);
    ASSERT_EQ(parent_slot->child_count, 0u);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(parent_slot->state, ASX_REGION_CLOSED);
}

/* --- New tests covering additional branches --- */

TEST(quiescence_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;

    asx_runtime_reset();

    ASSERT_EQ(asx_quiescence_check(bad), ASX_E_NOT_FOUND);
}

TEST(quiescence_open_region_returns_not_reached) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_NOT_REACHED);
}

TEST(quiescence_success_after_drain_no_tasks) {
    asx_region_id rid;
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_success_with_committed_obligation) {
    asx_region_id rid;
    asx_task_id holder_task;
    obligation_holder_ctx holder;
    asx_budget budget = asx_budget_from_polls(8u);
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    obligation_holder_init(&holder, rid, HOLDER_COMMITS, 1);
    ASSERT_EQ(asx_task_spawn(rid, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ASSERT_EQ(holder.settle_status, ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_success_with_aborted_obligation) {
    asx_region_id rid;
    asx_task_id holder_task;
    obligation_holder_ctx holder;
    asx_budget budget = asx_budget_from_polls(8u);
    asx_region_slot *region = NULL;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    obligation_holder_init(&holder, rid, HOLDER_ABORTS, 1);
    ASSERT_EQ(asx_task_spawn(rid, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_OK);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ASSERT_EQ(holder.settle_status, ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(quiescence_closed_region_with_pending_cleanup_returns_not_reached) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, NULL, &handle), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    ASSERT_EQ(asx_quiescence_check(rid), ASX_E_QUIESCENCE_NOT_REACHED);
}

TEST(drain_null_budget_returns_invalid_argument) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_drain(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(drain_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;
    asx_budget budget;

    asx_runtime_reset();

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(bad, &budget), ASX_E_NOT_FOUND);
}

TEST(drain_multiple_tasks_all_complete) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_task_state state;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t3), ASX_OK);

    /* Prime all tasks to RUNNING state */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_task_get_state(t1, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t2, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_get_state(t3, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_COMPLETED);

    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

TEST(drain_cleanup_lifo_multiple) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h1 = ASX_CLEANUP_INVALID;
    asx_cleanup_handle h2 = ASX_CLEANUP_INVALID;
    asx_cleanup_handle h3 = ASX_CLEANUP_INVALID;
    asx_budget budget;
    int c1 = 0, c2 = 0, c3 = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_DRAINING;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c1, &h1), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c2, &h2), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &c3, &h3), ASX_OK);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(c1, 1);
    ASSERT_EQ(c2, 1);
    ASSERT_EQ(c3, 1);
}

/* --- Drain progress snapshot tests (bd-1eqo.5.2) --- */

TEST(drain_progress_null_out_returns_invalid_argument) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_drain_progress(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(drain_progress_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_drain_progress(bad, &prog), ASX_E_NOT_FOUND);
}

TEST(drain_progress_open_region_with_tasks) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t2), ASX_OK);

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.region_state, ASX_REGION_OPEN);
    ASSERT_EQ(prog.tasks_total, 2u);
    ASSERT_EQ(prog.tasks_live, 2u);
    ASSERT_EQ(prog.tasks_completed, 0u);
    ASSERT_EQ(prog.tasks_cancelled, 0u);
    ASSERT_EQ(prog.obligations_total, 0u);
    ASSERT_EQ(prog.poisoned, 0);
}

TEST(drain_progress_with_obligations) {
    asx_region_id rid;
    asx_task_id tids[3];
    obligation_holder_ctx holders[3];
    asx_budget budget = asx_budget_from_polls(8u);
    asx_drain_progress prog;
    uint32_t i;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* Three held obligations: the first holder commits and completes, the
     * other two park holding theirs. */
    for (i = 0; i < 3u; i++) {
        obligation_holder_init(&holders[i], rid, HOLDER_COMMITS, i == 0u ? 1 : 0);
        ASSERT_EQ(asx_task_spawn(rid, poll_obligation_holder, &holders[i], &tids[i]), ASX_OK);
    }
    ASSERT_EQ(asx_scheduler_run(rid, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(holders[0].settle_status, ASX_OK);
    ASSERT_EQ(holders[1].reserve_status, ASX_OK);
    ASSERT_EQ(holders[2].reserve_status, ASX_OK);

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.obligations_total, 3u);
    ASSERT_EQ(prog.obligations_reserved, 2u);
    ASSERT_EQ(prog.obligations_resolved, 1u);
}

TEST(drain_progress_after_partial_drain) {
    asx_region_id rid;
    asx_task_id t1, t2;
    asx_budget budget;
    asx_drain_progress prog;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &t2), ASX_OK);

    /* Prime both tasks */
    budget = asx_budget_from_polls(2);
    SCHED_RUN_IGNORE(rid, &budget);

    /* Start drain — t1 will complete (checkpoint-aware), t2 stays pending */
    budget = asx_budget_from_polls(8);
    {
        asx_status drain_st = asx_region_drain(rid, &budget);
        (void)drain_st;
    }

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    /* t1 completed, t2 still live with cancel_pending */
    ASSERT_TRUE(prog.tasks_completed > 0u);
    ASSERT_TRUE(prog.tasks_total >= prog.tasks_live + prog.tasks_completed);
}

TEST(drain_progress_cleanup_drained_flag) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h = ASX_CLEANUP_INVALID;
    asx_drain_progress prog;
    int cleaned = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &h), ASX_OK);

    /* Before drain: cleanup not drained */
    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.cleanup_drained, 0);

    /* Pop cleanup manually */
    asx_cleanup_drain(&region->cleanup);

    ASSERT_EQ(asx_region_drain_progress(rid, &prog), ASX_OK);
    ASSERT_EQ(prog.cleanup_drained, 1);
    ASSERT_EQ(cleaned, 1);
}

/* --- Quiescence check detailed tests (bd-1eqo.5.2) --- */

TEST(quiescence_detailed_null_out_returns_invalid_argument) {
    asx_region_id rid;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_quiescence_check_detailed(rid, NULL), ASX_E_INVALID_ARGUMENT);
}

TEST(quiescence_detailed_invalid_handle_returns_not_found) {
    asx_region_id bad = 0xDEADBEEFu;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_quiescence_check_detailed(bad, &report), ASX_E_NOT_FOUND);
}

TEST(region_is_quiescent_invalid_handle_returns_false) {
    asx_runtime_reset();
    ASSERT_FALSE(asx_region_is_quiescent(0xDEADBEEFu));
}

TEST(quiescence_detailed_fully_quiescent) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 1);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q2_children_closed, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_EQ(report.region_state, ASX_REGION_CLOSED);
    ASSERT_TRUE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_live_tasks_q1_fails) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 3;
    asx_cleanup_init(&region->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 0);
    ASSERT_EQ(report.q2_children_closed, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_open_child_q2_fails) {
    asx_region_id parent;
    asx_region_id child;
    asx_region_slot *parent_slot = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    parent_slot->state = ASX_REGION_CLOSED;
    parent_slot->task_count = 0;
    asx_cleanup_init(&parent_slot->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(parent, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q2_children_closed, 0);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_FALSE(asx_region_is_quiescent(parent));
}

TEST(quiescence_detailed_all_children_closed_q2_passes) {
    asx_region_id parent;
    asx_region_id child1;
    asx_region_id child2;
    asx_region_slot *parent_slot = NULL;
    asx_quiescence_report report;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child1), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &child2), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(parent, &parent_slot), ASX_OK);
    log_child_summary("q2-before-close", parent, parent_slot);

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child1, &budget), ASX_OK);
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(child2, &budget), ASX_OK);

    parent_slot->state = ASX_REGION_CLOSED;
    parent_slot->task_count = 0;
    asx_cleanup_init(&parent_slot->cleanup);
    log_child_summary("q2-after-close", parent, parent_slot);

    ASSERT_EQ(asx_quiescence_check_detailed(parent, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 1);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q2_children_closed, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 1);
    ASSERT_TRUE(asx_region_is_quiescent(parent));
}

TEST(quiescence_detailed_unresolved_obligations_q3_fails) {
    asx_region_id rid;
    asx_task_id holder;
    asx_obligation_id oid;
    asx_region_slot *region = NULL;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    /* The obligation's holder is a live task that is never polled. */
    ASSERT_EQ(asx_task_spawn(rid, poll_pending, NULL, &holder), ASX_OK);
    ASSERT_EQ(asx_obligation_reserve_ex(rid, ASX_OBLIGATION_KIND_GENERIC, holder, &oid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q3_obligations_resolved, 0);
    ASSERT_EQ(report.progress.obligations_reserved, 1u);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_undrained_cleanup_q4_fails) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_cleanup_handle h = ASX_CLEANUP_INVALID;
    asx_quiescence_report report;
    int cleaned = 0;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;
    asx_cleanup_init(&region->cleanup);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &h), ASX_OK);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.q1_tasks_complete, 1);
    ASSERT_EQ(report.q4_cleanup_drained, 0);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(quiescence_detailed_not_closed_not_quiescent) {
    asx_region_id rid;
    asx_quiescence_report report;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);

    ASSERT_EQ(asx_quiescence_check_detailed(rid, &report), ASX_OK);
    ASSERT_EQ(report.quiescent, 0);
    ASSERT_EQ(report.region_state, ASX_REGION_OPEN);
    ASSERT_FALSE(asx_region_is_quiescent(rid));
}

TEST(drain_progress_monotonicity_during_drain) {
    asx_region_id rid;
    asx_task_id t1, t2, t3;
    asx_budget budget;
    asx_drain_progress before, after;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t1), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t2), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &t3), ASX_OK);

    /* Prime tasks */
    budget = asx_budget_from_polls(3);
    SCHED_RUN_IGNORE(rid, &budget);

    ASSERT_EQ(asx_region_drain_progress(rid, &before), ASX_OK);

    /* Full drain */
    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);

    ASSERT_EQ(asx_region_drain_progress(rid, &after), ASX_OK);

    /* Monotonicity: tasks_live decreased, tasks_completed increased */
    ASSERT_TRUE(after.tasks_live <= before.tasks_live);
    ASSERT_TRUE(after.tasks_completed >= before.tasks_completed);
}

TEST(drain_already_closed_is_idempotent) {
    asx_region_id rid;
    asx_region_slot *region = NULL;
    asx_budget budget;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);

    region->state = ASX_REGION_CLOSED;
    region->task_count = 0;

    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
}

TEST(drain_full_lifecycle_tasks_obligations_cleanup) {
    asx_region_id rid;
    asx_task_id tid;
    asx_task_id holder_task;
    obligation_holder_ctx holder;
    asx_region_slot *region = NULL;
    asx_budget budget;
    int cleaned = 0;
    asx_cleanup_handle handle = ASX_CLEANUP_INVALID;

    asx_runtime_reset();

    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn(rid, poll_checkpoint_then_complete, NULL, &tid), ASX_OK);
    /* The obligation is reserved and committed by a task's own poll. */
    obligation_holder_init(&holder, rid, HOLDER_COMMITS, 1);
    ASSERT_EQ(asx_task_spawn(rid, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ(asx_cleanup_push(&region->cleanup, cleanup_mark, &cleaned, &handle), ASX_OK);

    /* Prime both tasks: the first to RUNNING, the holder to completion */
    budget = asx_budget_from_polls(2);
    SCHED_RUN_IGNORE(rid, &budget);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ASSERT_EQ(holder.settle_status, ASX_OK);

    /* Full drain */
    budget = asx_budget_from_polls(64);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
    ASSERT_EQ(cleaned, 1);
    ASSERT_EQ(asx_quiescence_check(rid), ASX_OK);
}

/* Admission expectations are the pinned Rust RegionState predicates
 * (record/region.rs:140-151). State injection exercises transient phases;
 * the finalizer tests below reach FINALIZING through the public close path. */
TEST(task_spawn_admission_matrix) {
    unsigned state;
    unsigned cleanup;
    unsigned captured;
    for (state = ASX_REGION_OPEN; state <= ASX_REGION_CLOSED; state++) {
        for (cleanup = 0u; cleanup < 2u; cleanup++) {
            for (captured = 0u; captured < 2u; captured++) {
                asx_region_id rid;
                asx_region_slot *r;
                asx_task_id tid = ASX_INVALID_ID;
                void *data = NULL;
                asx_status st;
                int accepts =
                    state == ASX_REGION_OPEN || (cleanup && state == ASX_REGION_FINALIZING);
                asx_runtime_reset();
                ASSERT_EQ(asx_region_open(&rid), ASX_OK);
                ASSERT_EQ(asx_region_slot_lookup(rid, &r), ASX_OK);
                r->state = (asx_region_state)state;
                if (captured) {
                    st = cleanup
                             ? asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid,
                                                               &data)
                             : asx_task_spawn_captured(rid, poll_complete, 4u, NULL, &tid, &data);
                } else {
                    st = cleanup ? asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid)
                                 : asx_task_spawn(rid, poll_complete, NULL, &tid);
                }
                ASSERT_EQ(st, accepts ? ASX_OK : ASX_E_REGION_CLOSED);
                ASSERT_EQ(r->task_count, accepts ? 1u : 0u);
                ASSERT_EQ(r->capture_used, accepts && captured ? 8u : 0u);
                if (!accepts) {
                    ASSERT_EQ(tid, ASX_INVALID_ID);
                    ASSERT_TRUE(data == NULL);
                }
            }
        }
    }
}

TEST(cleanup_spawn_limits_and_failure_atomicity) {
    asx_region_id rid;
    asx_region_slot *r;
    asx_region_limits limits;
    asx_task_id tid = ASX_INVALID_ID;
    void *data = NULL;
    uint32_t i;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(rid, &r), ASX_OK);
    ASSERT_EQ(asx_region_get_limits(rid, &limits), ASX_OK);
    limits.max_tasks = 0u;
    ASSERT_EQ(asx_region_set_limits(rid, &limits), ASX_OK);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, &data),
              ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(r->capture_used, 0u);
    ASSERT_EQ(r->task_count, 0u);
    ASSERT_EQ(tid, ASX_INVALID_ID);
    ASSERT_TRUE(data == NULL);

    r->state = ASX_REGION_FINALIZING;
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete,
                                              ASX_REGION_CAPTURE_ARENA_BYTES + 1u, NULL, &tid,
                                              &data),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(r->capture_used, 0u);
    for (i = 0u; i < ASX_MAX_TASKS; i++) {
        ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_OK);
    }
    tid = ASX_INVALID_ID;
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, &data),
              ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(r->task_count, ASX_MAX_TASKS);
    ASSERT_EQ(r->capture_used, 0u);
    ASSERT_EQ(tid, ASX_INVALID_ID);
    ASSERT_TRUE(data == NULL);
}

TEST(cleanup_spawn_invalid_arguments_and_poison) {
    asx_region_id rid;
    asx_task_id tid = ASX_INVALID_ID;
    void *data = NULL;
    asx_runtime_reset();
    ASSERT_EQ(asx_task_spawn_cleanup(ASX_INVALID_ID, poll_complete, NULL, &tid), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, NULL, NULL, &tid), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 0u, NULL, &tid, &data),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, NULL),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_task_spawn_cleanup(rid, poll_complete, NULL, &tid), ASX_E_REGION_POISONED);
    ASSERT_EQ(asx_task_spawn_cleanup_captured(rid, poll_complete, 4u, NULL, &tid, &data),
              ASX_E_REGION_POISONED);
    ASSERT_EQ(tid, ASX_INVALID_ID);
    ASSERT_TRUE(data == NULL);
}

/* A finalizer runs with no task polling and the region's tasks gone, so it
 * can settle nothing: every obligation was settled or leaked by its holder's
 * own poll or completion first. It observes that, and the reentrancy of a
 * drain begun from inside it. */
typedef struct {
    asx_region_id region;
    asx_region_id other;
    asx_obligation_id obligation;
    asx_obligation_state obligation_state;
    asx_status recursive_drain;
    asx_status other_drain;
    asx_region_state region_state;
    int calls;
} public_finalizer_ctx;

static void finalizer_observe_and_drain(void *data) {
    public_finalizer_ctx *ctx = (public_finalizer_ctx *)data;
    asx_budget budget = asx_budget_from_polls(8u);
    ctx->calls++;
    if (asx_obligation_get_state(ctx->obligation, &ctx->obligation_state) != ASX_OK) {
        ctx->obligation_state = ASX_OBLIGATION_RESERVED;
    }
    if (asx_region_get_state(ctx->region, &ctx->region_state) != ASX_OK) {
        ctx->region_state = ASX_REGION_OPEN;
    }
    ctx->recursive_drain = asx_region_drain(ctx->region, &budget);
    ctx->other_drain = asx_region_drain(ctx->other, &budget);
}

TEST(public_finalizer_runs_after_holder_resolves_and_handles_reentrancy) {
    public_finalizer_ctx ctx;
    obligation_holder_ctx holder;
    asx_task_id holder_task;
    asx_region_id parent;
    asx_region_id child;
    asx_budget budget;
    asx_region_state state;
    int remaining_calls = 0;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&ctx.region), ASX_OK);
    parent = ctx.region;
    ASSERT_EQ(asx_region_open_child(ctx.region, &child), ASX_OK);
    ASSERT_EQ(asx_region_open(&ctx.other), ASX_OK);
    /* The child's obligation is held by a task of the child, which commits
     * it in its own poll when the drain cancels it. */
    obligation_holder_init(&holder, child, HOLDER_COMMITS, 0);
    ASSERT_EQ(asx_task_spawn(child, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(child, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ctx.obligation = holder.obligation;
    ctx.region = child;
    ASSERT_EQ(asx_region_defer_sync(child, cleanup_mark, &remaining_calls), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(child, finalizer_observe_and_drain, &ctx), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(holder.settle_status, ASX_OK);
    ASSERT_EQ(ctx.obligation_state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(ctx.region_state, ASX_REGION_FINALIZING);
    ASSERT_EQ(ctx.recursive_drain, ASX_E_PENDING);
    ASSERT_EQ(ctx.other_drain, ASX_OK);
    ASSERT_EQ(ctx.calls, 1);
    ASSERT_EQ(remaining_calls, 1);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    ASSERT_EQ(asx_region_get_state(parent, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_quiescence_check(ctx.region), ASX_OK);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(ctx.calls, 1);
}

typedef struct {
    asx_region_id region;
    asx_task_id task;
    asx_status spawn_status;
    int polls;
    int finalizer_calls;
} cleanup_task_ctx;

static asx_status poll_cleanup_task(void *data, asx_task_id self) {
    cleanup_task_ctx *ctx = (cleanup_task_ctx *)data;
    (void)self;
    ctx->polls++;
    return ASX_OK;
}

static void finalizer_spawn_cleanup_task(void *data) {
    cleanup_task_ctx *ctx = (cleanup_task_ctx *)data;
    ctx->finalizer_calls++;
    ctx->spawn_status = asx_task_spawn_cleanup(ctx->region, poll_cleanup_task, ctx, &ctx->task);
}

/* Region close audits obligations only after its tasks and the finalizers'
 * cleanup tasks are gone, and a holder settles its obligation in its own
 * poll before it completes, so nothing is left for the audit: the
 * obligation is committed by its holder before the finalizer's cleanup
 * task is spawned and the region closes with no leak. */
TEST(region_leak_audit_waits_for_holder_and_cleanup_task) {
    cleanup_task_ctx ctx;
    obligation_holder_ctx holder;
    asx_task_id holder_task;
    asx_obligation_state state;
    asx_budget budget;
    asx_outcome outcome;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&ctx.region), ASX_OK);
    obligation_holder_init(&holder, ctx.region, HOLDER_COMMITS, 0);
    ASSERT_EQ(asx_task_spawn(ctx.region, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_spawn_cleanup_task, &ctx), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ctx.region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    holder.release = 1;
    ASSERT_EQ(asx_task_wake(holder_task), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(holder.settle_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(ctx.finalizer_calls, 0);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(ctx.region, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(ctx.spawn_status, ASX_OK);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    ASSERT_EQ(ctx.polls, 0);
    ASSERT_EQ(asx_region_drain(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.finalizer_calls, 1);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    ASSERT_EQ(asx_region_get_close_outcome(ctx.region, &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(asx_quiescence_check(ctx.region), ASX_OK);
}

/* A holder that completes with its obligation still reserved leaks it at
 * once, under the policy, not at the region's close audit: by the time the
 * region finalizes the obligation is terminal, the finalizers and their
 * cleanup tasks still run, and the region reaches quiescence. */
TEST(holder_completing_with_obligation_reserved_leaks_it_before_region_close) {
    cleanup_task_ctx ctx;
    obligation_holder_ctx holder;
    asx_task_id holder_task;
    asx_obligation_state state;
    asx_budget budget;
    asx_runtime_reset();
    /* LOG named: under the default PANIC the leak also fails the holder's
     * completion. */
    asx_runtime_set_leak_policy_internal(g_kernels[0], ASX_LEAK_LOG, NULL);
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&ctx.region), ASX_OK);
    obligation_holder_init(&holder, ctx.region, HOLDER_LEAKS, 0);
    ASSERT_EQ(asx_task_spawn(ctx.region, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_spawn_cleanup_task, &ctx), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ctx.region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    holder.release = 1;
    ASSERT_EQ(asx_task_wake(holder_task), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), 1u);
    ASSERT_EQ(ctx.finalizer_calls, 0);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(ctx.region, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(ctx.spawn_status, ASX_OK);
    ASSERT_EQ(ctx.polls, 0);
    ASSERT_EQ(asx_region_drain(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.finalizer_calls, 1);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_LEAKED);
    ASSERT_EQ(asx_obligation_leak_count(), 1u);
    ASSERT_EQ(asx_quiescence_check(ctx.region), ASX_OK);
}

TEST(defer_sync_refuses_poisoned_region_without_registering_callback) {
    asx_region_id rid;
    asx_region_slot *region;
    int calls = 0;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_poison(rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, cleanup_mark, &calls), ASX_E_REGION_POISONED);
    ASSERT_EQ(asx_region_slot_lookup(rid, &region), ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&region->cleanup), 0u);
    ASSERT_EQ(calls, 0);
}

TEST(public_finalizer_capacity_and_errors) {
    asx_region_id rid;
    asx_budget budget;
    uint32_t i;
    int calls = 0;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_defer_sync(ASX_INVALID_ID, cleanup_mark, &calls), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_open(&rid), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(rid, NULL, &calls), ASX_E_INVALID_ARGUMENT);
    for (i = 0u; i < ASX_CLEANUP_STACK_CAPACITY; i++) {
        ASSERT_EQ(asx_region_defer_sync(rid, cleanup_mark, &calls), ASX_OK);
    }
    ASSERT_EQ(asx_region_defer_sync(rid, cleanup_mark, &calls), ASX_E_RESOURCE_EXHAUSTED);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(rid, &budget), ASX_OK);
    ASSERT_EQ(calls, (int)ASX_CLEANUP_STACK_CAPACITY);
    ASSERT_EQ(asx_region_defer_sync(rid, cleanup_mark, &calls), ASX_E_REGION_CLOSED);
}

/* A task registering a finalizer, sync and async, on `target`. */
typedef struct {
    asx_region_id target;
    asx_status sync_status;
    asx_status async_status;
    int calls;
} register_from_task_ctx;

static asx_status poll_register_finalizers(void *data, asx_task_id self) {
    register_from_task_ctx *c = (register_from_task_ctx *)data;
    (void)self;
    c->sync_status = asx_region_defer_sync(c->target, cleanup_mark, &c->calls);
    c->async_status = asx_region_defer_async(c->target, poll_complete, NULL);
    return ASX_OK;
}

TEST(public_finalizer_registration_is_for_the_host_and_the_regions_own_tasks) {
    /* Rust's Scope::defer_sync takes the region's own Scope and the runtime
     * state: a task of another region cannot reach it, and a refusal leaves
     * the stack as it was. */
    asx_region_id mine;
    asx_region_id other;
    asx_task_id tid;
    asx_region_slot *region = NULL;
    asx_budget budget;
    register_from_task_ctx foreign;
    register_from_task_ctx own;

    asx_runtime_reset();
    memset(&foreign, 0, sizeof(foreign));
    memset(&own, 0, sizeof(own));
    ASSERT_EQ(asx_region_open(&mine), ASX_OK);
    ASSERT_EQ(asx_region_open(&other), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(mine, &region), ASX_OK);
    foreign.target = mine;
    own.target = mine;
    ASSERT_EQ(asx_task_spawn(other, poll_register_finalizers, &foreign, &tid), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(other, &budget), ASX_OK);
    ASSERT_EQ(foreign.sync_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(foreign.async_status, ASX_E_PERMISSION_DENIED);
    ASSERT_EQ(asx_cleanup_pending(&region->cleanup), 0u);

    ASSERT_EQ(asx_task_spawn(mine, poll_register_finalizers, &own, &tid), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(mine, &budget), ASX_OK);
    ASSERT_EQ(own.sync_status, ASX_OK);
    ASSERT_EQ(own.async_status, ASX_OK);
    ASSERT_EQ(asx_cleanup_pending(&region->cleanup), 2u);
    ASSERT_EQ(asx_cleanup_async_pending(&region->cleanup), 1u);

    /* The host registers on any region it holds. */
    ASSERT_EQ(asx_region_defer_sync(other, cleanup_mark, &own.calls), ASX_OK);
}

typedef struct {
    asx_region_id other;
    asx_cancel_reason reason;
    asx_status status;
    int calls;
} finalizer_cancel_ctx;

static void finalizer_cancel_other_tree(void *data) {
    finalizer_cancel_ctx *ctx = (finalizer_cancel_ctx *)data;
    ctx->calls++;
    ctx->status = asx_region_cancel(ctx->other, &ctx->reason, NULL);
}

TEST(public_finalizer_nested_cancel_preserves_outer_subtree) {
    finalizer_cancel_ctx ctx;
    asx_region_id parent;
    asx_region_id first;
    asx_region_id second;
    asx_region_id other_children[2];
    uint32_t children = 0u;
    uint32_t i;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &first), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &second), ASX_OK);
    ASSERT_EQ(asx_region_open(&ctx.other), ASX_OK);
    /* R1 has four region slots. Exercise nested cancellation there too;
     * with six available slots, the second tree overwrites every entry of
     * the original three-region propagation scratch in the old code. */
    for (i = 4u; i < ASX_MAX_REGIONS && children < 2u; i++) {
        ASSERT_EQ(asx_region_open_child(ctx.other, &other_children[children]), ASX_OK);
        children++;
    }
    ctx.reason.kind = ASX_CANCEL_USER;
    ctx.reason.origin_region = parent;
    ctx.reason.origin_task = ASX_INVALID_ID;
    /* Cancel visits the last child first; its callback must not replace
     * the outer walk before the older sibling has been finalized. */
    ASSERT_EQ(asx_region_defer_sync(second, finalizer_cancel_other_tree, &ctx), ASX_OK);
    ASSERT_EQ(asx_region_cancel(parent, &ctx.reason, NULL), ASX_OK);
    ASSERT_EQ(ctx.calls, 1);
    ASSERT_EQ(ctx.status, ASX_OK);
    ASSERT_EQ(asx_quiescence_check(first), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(second), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(parent), ASX_OK);
    for (i = 0u; i < children; i++) { ASSERT_EQ(asx_quiescence_check(other_children[i]), ASX_OK); }
    ASSERT_EQ(asx_quiescence_check(ctx.other), ASX_OK);
}

#ifndef ASX_DEBUG_QUARANTINE
typedef struct {
    asx_region_id replacement;
    asx_task_id task;
    asx_task_id first_task;
    asx_status open_status;
    asx_status spawn_status;
    asx_status join_status;
    int polls;
} finalizer_recycle_ctx;

static asx_status poll_replacement_region(void *data, asx_task_id self) {
    finalizer_recycle_ctx *ctx = (finalizer_recycle_ctx *)data;
    (void)self;
    ctx->polls++;
    return ASX_OK;
}

static void finalizer_recycle_closed_sibling(void *data) {
    finalizer_recycle_ctx *ctx = (finalizer_recycle_ctx *)data;
    asx_outcome outcome;
    ctx->join_status = asx_task_join(ctx->first_task, &outcome);
    ctx->open_status = asx_region_open(&ctx->replacement);
    if (ctx->open_status == ASX_OK) {
        ctx->spawn_status =
            asx_task_spawn(ctx->replacement, poll_replacement_region, ctx, &ctx->task);
    }
}

TEST(public_finalizer_recycled_sibling_does_not_extend_outer_drain) {
    finalizer_recycle_ctx ctx;
    asx_region_id parent, first, second, extra;
    asx_task_id second_task;
    asx_region_state state;
    asx_budget budget;
    asx_outcome outcome;
    uint32_t i;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.replacement = ASX_INVALID_ID;
    ctx.task = ASX_INVALID_ID;
    ctx.spawn_status = ASX_E_PENDING;
    ASSERT_EQ(asx_region_open(&parent), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &first), ASX_OK);
    ASSERT_EQ(asx_region_open_child(parent, &second), ASX_OK);
    for (i = 3u; i < ASX_MAX_REGIONS; i++) { ASSERT_EQ(asx_region_open(&extra), ASX_OK); }
    ASSERT_EQ(asx_task_spawn(first, poll_complete, NULL, &ctx.first_task), ASX_OK);
    ASSERT_EQ(asx_task_spawn(second, poll_complete, NULL, &second_task), ASX_OK);
    /* The first task completes before its sibling. Retire its joinable
     * record in the sibling's finalizer, then recycle the closed region
     * for an unrelated root with live work while the outer scheduler runs. */
    ASSERT_EQ(asx_region_defer_sync(second, finalizer_recycle_closed_sibling, &ctx), ASX_OK);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_drain(parent, &budget), ASX_OK);
    ASSERT_EQ(ctx.join_status, ASX_OK);
    ASSERT_EQ(ctx.open_status, ASX_OK);
    ASSERT_EQ(ctx.spawn_status, ASX_OK);
    ASSERT_EQ(asx_handle_slot(ctx.replacement), asx_handle_slot(first));
    ASSERT_EQ(asx_region_get_state(parent, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSED);
    ASSERT_EQ(asx_region_get_state(first, &state), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_region_get_state(ctx.replacement, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    ASSERT_EQ(ctx.polls, 0);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_scheduler_run(ctx.replacement, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(asx_task_get_outcome(ctx.task, &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
}
#endif

/* Body-driven close waits without turning normal children into cancellation
 * cleanup. Values below are ordinary user state, not C outcome payloads. */
typedef struct {
    int polls;
    int goal;
    int cancelled;
    int value;
    int park_first;
} body_close_task_ctx;

static asx_status poll_body_close_task(void *data, asx_task_id self) {
    body_close_task_ctx *ctx = (body_close_task_ctx *)data;
    asx_checkpoint_result checkpoint;
    asx_status st = asx_checkpoint(self, &checkpoint);
    if (st != ASX_OK) return st;
    ctx->polls++;
    if (checkpoint.cancelled) {
        ctx->cancelled++;
        return ASX_OK;
    }
    if (ctx->park_first && ctx->polls == 1) {
        st = asx_task_park(self);
        return st == ASX_OK ? ASX_E_PENDING : st;
    }
    if (ctx->polls < ctx->goal) return ASX_E_PENDING;
    ctx->value = 42;
    return ASX_OK;
}

static uint32_t body_close_trace_count(asx_trace_event_kind kind) {
    uint32_t i, count = 0u;
    for (i = 0u; i < asx_trace_event_count(); i++) {
        asx_trace_event event;
        if (asx_trace_event_get(i, &event) && event.kind == kind) count++;
    }
    return count;
}

static uint32_t body_close_trace_index(asx_region_id id) {
    uint32_t i;
    for (i = 0u; i < asx_trace_event_count(); i++) {
        asx_trace_event event;
        if (asx_trace_event_get(i, &event) && event.kind == ASX_TRACE_REGION_CLOSED &&
            event.entity_id == id) {
            return i;
        }
    }
    return UINT32_MAX;
}

TEST(body_close_ok_preserves_child_value_and_outcome_without_cancel) {
    body_close_task_ctx ctx;
    asx_region_id region;
    asx_task_id task, refused = ASX_INVALID_ID;
    asx_budget budget = asx_budget_from_polls(8u);
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK), outcome;
    asx_cancel_reason reason;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 2;
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, poll_body_close_task, &ctx, &task), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, poll_complete, NULL, &refused), ASX_E_REGION_CLOSED);
    ASSERT_EQ(refused, ASX_INVALID_ID);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 2);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(ctx.value, 42);
    ASSERT_EQ(asx_task_get_outcome(task, &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(task, &reason), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_get_close_outcome(region, &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(asx_quiescence_check(region), ASX_OK);
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CANCELLED), 0u);
    /* Pinned Scope's Ok body calls RegionRecord::begin_close directly:
     * it changes admission state without a RuntimeState CloseBegin trace. */
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CLOSE), 0u);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 2);
}

TEST(body_close_budget_retry_keeps_normal_task_runnable_without_cancel) {
    body_close_task_ctx ctx;
    asx_region_id region;
    asx_task_id task;
    asx_task_state state;
    asx_budget budget = asx_budget_from_polls(1u);
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 3;
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, poll_body_close_task, &ctx, &task), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(budget.poll_quota, 0u);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(asx_task_get_state(task, &state), ASX_OK);
    ASSERT_EQ(state, ASX_TASK_RUNNING);
    ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_OK);
    /* Neither the native Ok-body branch nor its retry emits CloseBegin. */
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CLOSE), 0u);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 3);
    ASSERT_EQ(ctx.value, 42);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
}

TEST(body_close_parked_task_requires_real_wake_without_cancel) {
    body_close_task_ctx ctx;
    asx_region_id region;
    asx_task_id task;
    asx_budget budget = asx_budget_from_polls(8u);
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ctx.goal = 2;
    ctx.park_first = 1;
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_task_spawn(region, poll_body_close_task, &ctx, &task), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(asx_task_wake(task), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
    ASSERT_EQ(ctx.value, 42);
    ASSERT_EQ(ctx.cancelled, 0);
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
}

TEST(body_close_nested_ok_waits_for_each_body_without_closing_open_descendants) {
    body_close_task_ctx contexts[3];
    asx_region_id regions[3];
    asx_task_id tasks[3];
    asx_region_state state;
    asx_budget budget = asx_budget_from_polls(16u);
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK), outcome;
    uint32_t i;
    asx_runtime_reset();
    memset(contexts, 0, sizeof(contexts));
    ASSERT_EQ(asx_region_open(&regions[0]), ASX_OK);
    ASSERT_EQ(asx_region_open_child(regions[0], &regions[1]), ASX_OK);
    ASSERT_EQ(asx_region_open_child(regions[1], &regions[2]), ASX_OK);
    for (i = 0u; i < 3u; i++) {
        contexts[i].goal = 1;
        ASSERT_EQ(asx_task_spawn(regions[i], poll_body_close_task, &contexts[i], &tasks[i]),
                  ASX_OK);
    }
    ASSERT_EQ(asx_region_close_with_outcome(regions[0], &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(regions[0], &budget), ASX_E_PENDING);
    ASSERT_EQ(asx_region_get_state(regions[1], &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    ASSERT_EQ(asx_region_get_state(regions[2], &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_OPEN);
    ASSERT_EQ(asx_region_close_with_outcome(regions[1], &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(regions[0], &budget), ASX_E_PENDING);
    ASSERT_EQ(asx_region_close_with_outcome(regions[2], &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(regions[0], &budget), ASX_OK);
    for (i = 0u; i < 3u; i++) {
        ASSERT_EQ(contexts[i].value, 42);
        ASSERT_EQ(contexts[i].cancelled, 0);
        ASSERT_EQ(asx_task_get_outcome(tasks[i], &outcome), ASX_OK);
        ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
        ASSERT_EQ(asx_quiescence_check(regions[i]), ASX_OK);
        ASSERT_NE(body_close_trace_index(regions[i]), UINT32_MAX);
    }
    ASSERT_TRUE(body_close_trace_index(regions[2]) < body_close_trace_index(regions[1]));
    ASSERT_TRUE(body_close_trace_index(regions[1]) < body_close_trace_index(regions[0]));
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CANCELLED), 0u);
}

TEST(body_close_err_and_panic_cancel_fail_fast_with_parent_descendants) {
    static const asx_outcome_severity severities[] = {ASX_OUTCOME_ERR, ASX_OUTCOME_PANICKED};
    uint32_t severity, i;
    for (severity = 0u; severity < 2u; severity++) {
        body_close_task_ctx contexts[3];
        asx_region_id regions[3];
        asx_task_id tasks[3];
        asx_outcome body = asx_outcome_make(severities[severity]), outcome;
        asx_cancel_reason seen;
        asx_budget budget = asx_budget_from_polls(16u);
        asx_runtime_reset();
        memset(contexts, 0, sizeof(contexts));
        ASSERT_EQ(asx_region_open(&regions[0]), ASX_OK);
        ASSERT_EQ(asx_region_open_child(regions[0], &regions[1]), ASX_OK);
        ASSERT_EQ(asx_region_open_child(regions[1], &regions[2]), ASX_OK);
        for (i = 0u; i < 3u; i++) {
            contexts[i].goal = 2;
            ASSERT_EQ(asx_task_spawn(regions[i], poll_body_close_task, &contexts[i], &tasks[i]),
                      ASX_OK);
        }
        ASSERT_EQ(asx_region_close_with_outcome(regions[0], &body, NULL), ASX_OK);
        ASSERT_EQ(asx_region_wait_closed(regions[0], &budget), ASX_OK);
        for (i = 0u; i < 3u; i++) {
            ASSERT_EQ(contexts[i].cancelled, 1);
            ASSERT_EQ(contexts[i].value, 0);
            ASSERT_EQ(asx_task_get_cancel_reason(tasks[i], &seen), ASX_OK);
            ASSERT_EQ(seen.kind, i == 0u ? ASX_CANCEL_FAIL_FAST : ASX_CANCEL_PARENT);
            ASSERT_EQ(seen.origin_region, regions[i == 0u ? 0u : i - 1u]);
            ASSERT_EQ(asx_task_get_outcome(tasks[i], &outcome), ASX_OK);
            ASSERT_EQ(outcome.severity, ASX_OUTCOME_CANCELLED);
            ASSERT_EQ(asx_quiescence_check(regions[i]), ASX_OK);
        }
        ASSERT_EQ(asx_region_get_close_outcome(regions[0], &outcome), ASX_OK);
        ASSERT_EQ(outcome.severity, ASX_OUTCOME_CANCELLED);
        ASSERT_EQ(body.severity, severities[severity]);
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 3u);
    }
}

TEST(body_close_cancelled_preserves_requested_reason_and_cause_chain) {
    body_close_task_ctx contexts[3];
    asx_region_id regions[3];
    asx_task_id tasks[3];
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_cancel_reason requested = asx_cancel_reason_default(ASX_CANCEL_SHUTDOWN, "body stopped");
    asx_cancel_reason seen;
    asx_budget budget = asx_budget_from_polls(16u);
    uint32_t i;
    asx_runtime_reset();
    memset(contexts, 0, sizeof(contexts));
    ASSERT_EQ(asx_region_open(&regions[0]), ASX_OK);
    ASSERT_EQ(asx_region_open_child(regions[0], &regions[1]), ASX_OK);
    ASSERT_EQ(asx_region_open_child(regions[1], &regions[2]), ASX_OK);
    requested.origin_region = regions[0];
    requested.timestamp = 123u;
    for (i = 0u; i < 3u; i++) {
        contexts[i].goal = 2;
        ASSERT_EQ(asx_task_spawn(regions[i], poll_body_close_task, &contexts[i], &tasks[i]),
                  ASX_OK);
    }
    ASSERT_EQ(asx_region_close_with_outcome(regions[0], &body, &requested), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(regions[0], &budget), ASX_OK);
    ASSERT_EQ(asx_task_get_cancel_reason(tasks[0], &seen), ASX_OK);
    ASSERT_EQ(seen.kind, ASX_CANCEL_SHUTDOWN);
    ASSERT_EQ(seen.timestamp, 123u);
    ASSERT_STR_EQ(seen.message, "body stopped");
    ASSERT_EQ(asx_task_get_cancel_reason(tasks[2], &seen), ASX_OK);
    ASSERT_EQ(seen.kind, ASX_CANCEL_PARENT);
    ASSERT_EQ(seen.origin_region, regions[1]);
    ASSERT_TRUE(seen.cause != NULL && seen.cause->cause != NULL);
    ASSERT_EQ(seen.cause->kind, ASX_CANCEL_PARENT);
    ASSERT_EQ(seen.cause->cause->kind, ASX_CANCEL_SHUTDOWN);
    for (i = 0u; i < 3u; i++) { ASSERT_EQ(contexts[i].cancelled, 1); }
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 3u);
}

TEST(body_close_wait_runs_lifo_then_cleanup_before_leak_audit) {
    cleanup_task_ctx ctx;
    obligation_holder_ctx holder;
    asx_task_id holder_task;
    cleanup_spawn_task_ctx ordinary;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_obligation_state state;
    asx_budget budget = asx_budget_from_polls(8u);
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    memset(&ordinary, 0, sizeof(ordinary));
    memset(&g_finalizer_log, 0, sizeof(g_finalizer_log));
    ASSERT_EQ(asx_region_open(&ctx.region), ASX_OK);
    ordinary.region = ctx.region;
    /* The obligation's holder commits it in its own poll and completes,
     * before the body closes the region. */
    obligation_holder_init(&holder, ctx.region, HOLDER_COMMITS, 1);
    ASSERT_EQ(asx_task_spawn(ctx.region, poll_obligation_holder, &holder, &holder_task), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(holder.reserve_status, ASX_OK);
    ASSERT_EQ(holder.settle_status, ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_record_1, NULL), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_record_2, NULL), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_record_3, NULL), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_spawn_ordinary_task, &ordinary), ASX_OK);
    ASSERT_EQ(asx_region_defer_sync(ctx.region, finalizer_spawn_cleanup_task, &ctx), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(ctx.region, &body, NULL), ASX_OK);
    budget = asx_budget_from_polls(0u);
    ASSERT_EQ(asx_region_wait_closed(ctx.region, &budget), ASX_E_QUIESCENCE_TASKS_LIVE);
    ASSERT_EQ(ctx.finalizer_calls, 1);
    ASSERT_EQ(ctx.spawn_status, ASX_OK);
    ASSERT_EQ(ctx.polls, 0);
    ASSERT_EQ(ordinary.spawn_status, ASX_E_REGION_CLOSED);
    ASSERT_EQ(ordinary.task_id, ASX_INVALID_ID);
    ASSERT_EQ(g_finalizer_log.n, 3);
    ASSERT_EQ(g_finalizer_log.order[0], 3);
    ASSERT_EQ(g_finalizer_log.order[1], 2);
    ASSERT_EQ(g_finalizer_log.order[2], 1);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_region_wait_closed(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(ctx.polls, 1);
    ASSERT_EQ(ctx.finalizer_calls, 1);
    ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
    ASSERT_EQ(asx_obligation_leak_count(), 0u);
    ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
    ASSERT_EQ(asx_region_wait_closed(ctx.region, &budget), ASX_OK);
    ASSERT_EQ(g_finalizer_log.n, 3);
}

TEST(body_close_invalid_arguments_refuse_without_state_or_trace_mutation) {
    asx_region_id region;
    asx_region_slot *slot;
    asx_region_slot before;
    asx_budget budget = asx_budget_from_polls(8u), budget_before;
    asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK);
    asx_outcome cancelled = asx_outcome_make(ASX_OUTCOME_CANCELLED);
    asx_outcome invalid = asx_outcome_make((asx_outcome_severity)99);
    asx_cancel_reason reason = asx_cancel_reason_default(ASX_CANCEL_USER, NULL);
    uint32_t events;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(region, &slot), ASX_OK);
    memcpy(&before, slot, sizeof(before));
    memcpy(&budget_before, &budget, sizeof(budget_before));
    events = asx_trace_event_count();
    ASSERT_EQ(asx_region_close_with_outcome(region, NULL, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_close_with_outcome(region, &invalid, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_close_with_outcome(region, &cancelled, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_close_with_outcome(region, &ok, &reason), ASX_E_INVALID_ARGUMENT);
    reason.kind = (asx_cancel_kind)99;
    ASSERT_EQ(asx_region_close_with_outcome(region, &cancelled, &reason), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_wait_closed(region, NULL), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_E_INVALID_STATE);
    ASSERT_EQ(memcmp(slot, &before, sizeof(before)), 0);
    ASSERT_EQ(memcmp(&budget, &budget_before, sizeof(budget)), 0);
    ASSERT_EQ(asx_trace_event_count(), events);
    ASSERT_EQ(asx_region_close_with_outcome(region, &ok, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
}

TEST(body_close_bad_stale_and_poisoned_handles_refuse_atomically) {
    asx_region_id stale, region;
    asx_region_slot *slot;
    asx_region_slot before;
    asx_budget budget = asx_budget_from_polls(8u), budget_before;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK),
                out = asx_outcome_make(ASX_OUTCOME_PANICKED);
    uint32_t events;
    asx_runtime_reset();
    ASSERT_EQ(asx_region_close_with_outcome(ASX_INVALID_ID, &body, NULL), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_wait_closed(ASX_INVALID_ID, &budget), ASX_E_NOT_FOUND);
    ASSERT_EQ(asx_region_open(&stale), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(stale, &body, NULL), ASX_OK);
    ASSERT_EQ(asx_region_wait_closed(stale, &budget), ASX_OK);
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
#ifndef ASX_DEBUG_QUARANTINE
    ASSERT_EQ(asx_handle_slot(region), asx_handle_slot(stale));
    ASSERT_NE(asx_handle_generation(region), asx_handle_generation(stale));
    ASSERT_EQ(asx_region_close_with_outcome(stale, &body, NULL), ASX_E_STALE_HANDLE);
    ASSERT_EQ(asx_region_wait_closed(stale, &budget), ASX_E_STALE_HANDLE);
#endif
    ASSERT_EQ(asx_region_poison(region), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(region, &slot), ASX_OK);
    memcpy(&before, slot, sizeof(before));
    memcpy(&budget_before, &budget, sizeof(budget_before));
    events = asx_trace_event_count();
    ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_E_REGION_POISONED);
    ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_E_REGION_POISONED);
    ASSERT_EQ(asx_region_get_close_outcome(region, NULL), ASX_E_INVALID_ARGUMENT);
#ifndef ASX_DEBUG_QUARANTINE
    ASSERT_EQ(asx_region_get_close_outcome(stale, &out), ASX_E_STALE_HANDLE);
#endif
    ASSERT_EQ(out.severity, ASX_OUTCOME_PANICKED);
    ASSERT_EQ(memcmp(slot, &before, sizeof(before)), 0);
    ASSERT_EQ(memcmp(&budget, &budget_before, sizeof(budget)), 0);
    ASSERT_EQ(asx_trace_event_count(), events);
}

/* The leak policy applies when the holder completes with its obligation
 * still reserved, which the wait for the closing region drives here: the
 * region's close audit finds only terminal obligations afterwards. Under
 * PANIC the policy's fault surfaces from that completion (poisoning the
 * region instead under the hardened POISON_REGION containment). */
TEST(body_close_wait_applies_real_leak_policy_when_the_holder_completes) {
    static const asx_leak_response policies[] = {ASX_LEAK_LOG, ASX_LEAK_SILENT, ASX_LEAK_RECOVER,
                                                 ASX_LEAK_PANIC};
    static asx_runtime runtime;
    uint32_t i;
    for (i = 0u; i < 4u; i++) {
        asx_runtime_config config;
        asx_runtime_hooks hooks;
        obligation_holder_ctx holder;
        asx_task_id holder_task;
        asx_region_id region;
        asx_obligation_state state;
        asx_region_state region_state;
        asx_budget budget = asx_budget_from_polls(8u);
        asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
        int panics = policies[i] == ASX_LEAK_PANIC;
        int poisons = panics && asx_containment_policy_active() == ASX_CONTAIN_POISON_REGION;
        asx_runtime_reset();
        asx_runtime_config_init(&config);
        config.leak_response = policies[i];
        ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
        ASSERT_EQ(asx_runtime_init(&runtime, &config, &hooks), ASX_OK);
        ASSERT_EQ(asx_region_open(&region), ASX_OK);
        obligation_holder_init(&holder, region, HOLDER_LEAKS, 0);
        ASSERT_EQ(asx_task_spawn(region, poll_obligation_holder, &holder, &holder_task), ASX_OK);
        ASSERT_EQ(asx_scheduler_run(region, &budget), ASX_E_WOULD_BLOCK);
        ASSERT_EQ(holder.reserve_status, ASX_OK);
        ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
        ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);
        ASSERT_EQ(asx_obligation_leak_count(), 0u);
        ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_OK);
        holder.release = 1;
        ASSERT_EQ(asx_task_wake(holder_task), ASX_OK);
        budget = asx_budget_from_polls(8u);
        ASSERT_EQ(asx_region_wait_closed(region, &budget),
                  panics && !poisons ? ASX_E_UNRESOLVED_OBLIGATIONS : ASX_OK);
        ASSERT_EQ(asx_obligation_leak_count(), 1u);
        ASSERT_EQ(asx_obligation_get_state(holder.obligation, &state), ASX_OK);
        ASSERT_EQ(state,
                  policies[i] == ASX_LEAK_RECOVER ? ASX_OBLIGATION_ABORTED : ASX_OBLIGATION_LEAKED);
        ASSERT_EQ(asx_region_get_state(region, &region_state), ASX_OK);
        ASSERT_EQ(region_state, ASX_REGION_CLOSED);
        ASSERT_EQ(asx_quiescence_check(region), ASX_OK);
        if (!poisons) ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
        asx_runtime_shutdown(&runtime);
    }
}

typedef struct {
    asx_scope *scope;
    asx_region_id region;
    asx_budget budget;
    asx_status wait_status;
    asx_status finish_status;
    int unchanged;
} body_close_poll_guard_ctx;

static asx_status poll_body_close_host_guard(void *data, asx_task_id self) {
    body_close_poll_guard_ctx *ctx = (body_close_poll_guard_ctx *)data;
    asx_scope scope_before;
    asx_budget budget_before;
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    uint32_t events = asx_trace_event_count();
    (void)self;
    memcpy(&scope_before, ctx->scope, sizeof(scope_before));
    memcpy(&budget_before, &ctx->budget, sizeof(budget_before));
    ctx->wait_status = asx_region_wait_closed(ctx->region, &ctx->budget);
    ctx->finish_status = asx_scope_finish(ctx->scope, &body, NULL);
    ctx->unchanged = memcmp(ctx->scope, &scope_before, sizeof(scope_before)) == 0 &&
                     memcmp(&ctx->budget, &budget_before, sizeof(budget_before)) == 0 &&
                     asx_trace_event_count() == events;
    return ASX_OK;
}

TEST(body_close_wait_and_scope_finish_refuse_from_real_task_poll) {
    body_close_poll_guard_ctx ctx;
    asx_scope scope;
    asx_cx cx;
    asx_region_id region, caller_region;
    asx_task_id caller;
    asx_budget budget = asx_budget_from_polls(8u);
    asx_outcome body = asx_outcome_make(ASX_OUTCOME_OK);
    asx_region_state state;
    asx_runtime_reset();
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(asx_region_open(&region), ASX_OK);
    ASSERT_EQ(asx_region_open(&caller_region), ASX_OK);
    ASSERT_EQ(asx_cx_init(&cx, region, ASX_INVALID_ID, ASX_CAP_ALL), ASX_OK);
    ASSERT_EQ(asx_scope_init(&scope, region, &cx, asx_budget_from_polls(8u)), ASX_OK);
    ASSERT_EQ(asx_region_close_with_outcome(region, &body, NULL), ASX_OK);
    ctx.region = region;
    ctx.scope = &scope;
    ctx.budget = asx_budget_from_polls(8u);
    ASSERT_EQ(asx_task_spawn(caller_region, poll_body_close_host_guard, &ctx, &caller), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(caller_region, &budget), ASX_OK);
    ASSERT_EQ(ctx.wait_status, ASX_E_INVALID_STATE);
    ASSERT_EQ(ctx.finish_status, ASX_E_INVALID_STATE);
    ASSERT_EQ(ctx.unchanged, 1);
    ASSERT_EQ(asx_region_get_state(region, &state), ASX_OK);
    ASSERT_EQ(state, ASX_REGION_CLOSING);
    ASSERT_EQ(scope.finish_started, 0);
    ASSERT_EQ(asx_scope_finish(&scope, &body, NULL), ASX_OK);
    ASSERT_EQ(asx_quiescence_check(region), ASX_OK);
}

TEST(body_close_closed_exception_emits_receipts_without_reopening_or_rerunning) {
    static const asx_outcome_severity exceptional[] = {ASX_OUTCOME_CANCELLED, ASX_OUTCOME_ERR,
                                                       ASX_OUTCOME_PANICKED};
    uint32_t with_child, i;
    for (with_child = 0u; with_child < 2u; with_child++) {
        asx_region_id region;
        asx_task_id task = ASX_INVALID_ID;
        body_close_task_ctx ctx;
        asx_outcome ok = asx_outcome_make(ASX_OUTCOME_OK), outcome;
        asx_cancel_reason requested = asx_cancel_reason_default(ASX_CANCEL_SHUTDOWN, "after close");
        asx_cancel_reason observed;
        asx_region_state state;
        asx_budget budget = asx_budget_from_polls(8u);
        uint32_t polls;
        asx_runtime_reset();
        memset(&ctx, 0, sizeof(ctx));
        ctx.goal = 2;
        ASSERT_EQ(asx_region_open(&region), ASX_OK);
        if (with_child) {
            ASSERT_EQ(asx_task_spawn(region, poll_body_close_task, &ctx, &task), ASX_OK);
            ASSERT_EQ(asx_scheduler_run(region, &budget), ASX_OK);
            ASSERT_EQ(ctx.polls, 2);
            ASSERT_EQ(ctx.value, 42);
        }
        /* The body now closes an empty region; a completed child's record
         * in the second case proves that terminal outcomes are retained. */
        ASSERT_EQ(asx_region_close_with_outcome(region, &ok, NULL), ASX_OK);
        ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
        ASSERT_EQ(asx_region_get_close_outcome(region, &outcome), ASX_OK);
        ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
        ASSERT_EQ(asx_region_get_cancel_reason(region, &observed), ASX_E_NOT_FOUND);
        polls = body_close_trace_count(ASX_TRACE_SCHED_POLL);
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CLOSED), 1u);
        for (i = 0u; i < 3u; i++) {
            asx_outcome body = asx_outcome_make(exceptional[i]);
            asx_trace_event receipt;
            uint32_t events = asx_trace_event_count();
            ASSERT_EQ(asx_region_close_with_outcome(region, &body, i == 0u ? &requested : NULL),
                      ASX_OK);
            /* RuntimeState emits the receipt before skipping Closed state;
             * it neither records a new reason nor changes terminal work. */
            ASSERT_EQ(asx_trace_event_count(), events + 1u);
            ASSERT_TRUE(asx_trace_event_get(events, &receipt));
            ASSERT_EQ(receipt.kind, ASX_TRACE_REGION_CANCELLED);
            ASSERT_EQ(receipt.entity_id, region);
            ASSERT_EQ(receipt.aux,
                      (uint64_t)(i == 0u ? ASX_CANCEL_SHUTDOWN : ASX_CANCEL_FAIL_FAST));
            ASSERT_EQ(asx_region_get_state(region, &state), ASX_OK);
            ASSERT_EQ(state, ASX_REGION_CLOSED);
            ASSERT_EQ(asx_region_get_close_outcome(region, &outcome), ASX_OK);
            ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
            ASSERT_EQ(asx_region_get_cancel_reason(region, &observed), ASX_E_NOT_FOUND);
            ASSERT_EQ(asx_region_wait_closed(region, &budget), ASX_OK);
            if (with_child) {
                ASSERT_EQ(asx_task_get_outcome(task, &outcome), ASX_OK);
                ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
                ASSERT_EQ(asx_task_get_cancel_reason(task, &observed), ASX_E_NOT_FOUND);
                ASSERT_EQ(ctx.polls, 2);
                ASSERT_EQ(ctx.value, 42);
                ASSERT_EQ(ctx.cancelled, 0);
            }
        }
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CANCELLED), 3u);
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_CANCEL_REQUEST), 0u);
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CLOSE), 0u);
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_REGION_CLOSED), 1u);
        ASSERT_EQ(body_close_trace_count(ASX_TRACE_SCHED_POLL), polls);
        ASSERT_EQ(asx_quiescence_check(region), ASX_OK);
    }
}

int main(void) {
    RUN_TEST(body_close_closed_exception_emits_receipts_without_reopening_or_rerunning);
    RUN_TEST(body_close_ok_preserves_child_value_and_outcome_without_cancel);
    RUN_TEST(body_close_budget_retry_keeps_normal_task_runnable_without_cancel);
    RUN_TEST(body_close_parked_task_requires_real_wake_without_cancel);
    RUN_TEST(body_close_nested_ok_waits_for_each_body_without_closing_open_descendants);
    RUN_TEST(body_close_err_and_panic_cancel_fail_fast_with_parent_descendants);
    RUN_TEST(body_close_cancelled_preserves_requested_reason_and_cause_chain);
    RUN_TEST(body_close_wait_runs_lifo_then_cleanup_before_leak_audit);
    RUN_TEST(body_close_invalid_arguments_refuse_without_state_or_trace_mutation);
    RUN_TEST(body_close_bad_stale_and_poisoned_handles_refuse_atomically);
    RUN_TEST(body_close_wait_applies_real_leak_policy_when_the_holder_completes);
    RUN_TEST(body_close_wait_and_scope_finish_refuse_from_real_task_poll);
    RUN_TEST(task_spawn_admission_matrix);
    RUN_TEST(cleanup_spawn_limits_and_failure_atomicity);
    RUN_TEST(cleanup_spawn_invalid_arguments_and_poison);
    RUN_TEST(public_finalizer_runs_after_holder_resolves_and_handles_reentrancy);
    RUN_TEST(region_leak_audit_waits_for_holder_and_cleanup_task);
    RUN_TEST(holder_completing_with_obligation_reserved_leaks_it_before_region_close);
    RUN_TEST(defer_sync_refuses_poisoned_region_without_registering_callback);
    RUN_TEST(public_finalizer_capacity_and_errors);
    RUN_TEST(public_finalizer_registration_is_for_the_host_and_the_regions_own_tasks);
    RUN_TEST(public_finalizer_captured_cleanup_executes_and_drops_once);
    RUN_TEST(public_finalizer_nested_cancel_preserves_outer_subtree);
#ifndef ASX_DEBUG_QUARANTINE
    RUN_TEST(public_finalizer_recycled_sibling_does_not_extend_outer_drain);
#endif
    fprintf(stderr, "=== test_quiescence ===\n");

    asx_runtime_reset();
    RUN_TEST(quiescence_closed_region_with_live_tasks_reports_tasks_live);
    asx_runtime_reset();
    RUN_TEST(quiescence_closed_region_with_unresolved_obligation_reports_error);
    asx_runtime_reset();
    RUN_TEST(region_drain_advances_draining_region_to_closed_and_runs_cleanup);
    asx_runtime_reset();
    RUN_TEST(region_drain_recancels_uncancelled_tasks_before_scheduler_run);
    asx_runtime_reset();
    RUN_TEST(region_drain_budget_exhaustion_leaves_region_closing);
    asx_runtime_reset();
    RUN_TEST(region_finalizer_cannot_spawn_into_its_finalizing_region);
    RUN_TEST(public_finalizer_cleanup_task_delays_close_until_it_completes);
    RUN_TEST(spawn_admission_per_region_state);
    RUN_TEST(defer_sync_finalizers_run_once_in_lifo_order_at_close);
    RUN_TEST(defer_sync_runs_after_the_region_tasks_complete);
    RUN_TEST(defer_sync_refused_once_the_region_is_closing);
    RUN_TEST(defer_sync_argument_errors_and_capacity);
    RUN_TEST(defer_async_finalizer_runs_as_a_masked_cleanup_task_before_close);
    RUN_TEST(defer_async_finalizers_run_one_at_a_time_in_lifo_order_with_sync_ones);
    RUN_TEST(defer_async_region_finalizing_refuses_ordinary_spawn_but_takes_cleanup);
    RUN_TEST(defer_async_runs_after_the_region_tasks_complete);
    RUN_TEST(defer_async_wait_closed_drives_a_region_with_nothing_but_finalizers);
    RUN_TEST(defer_async_default_escalation_lets_a_finalizer_run_past_its_poll_budget);
    RUN_TEST(defer_async_bounded_panic_retires_the_finalizer_panicked_at_its_poll_budget);
    RUN_TEST(defer_async_argument_errors_and_registration_states);
    RUN_TEST(close_outcome_joins_the_task_outcomes);
    RUN_TEST(close_outcome_of_a_cancelled_empty_region_is_cancelled);
    RUN_TEST(close_outcome_of_a_cancelled_region_keeps_its_task_outcomes);
    RUN_TEST(drain_after_region_cancel_closes_with_the_requested_reason);
    RUN_TEST(close_outcome_argument_errors);
    asx_runtime_reset();
    RUN_TEST(finalizing_region_refuses_task_spawn);
    asx_runtime_reset();
    RUN_TEST(finalizing_region_refuses_captured_task_spawn);
    asx_runtime_reset();
    RUN_TEST(finalizing_region_still_rejects_obligation_reserve);
    asx_runtime_reset();
    RUN_TEST(region_open_child_null_out_fails);
    asx_runtime_reset();
    RUN_TEST(region_open_child_invalid_parent_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_non_open_parent);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_closed_parent);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_poisoned_parent);
    asx_runtime_reset();
    RUN_TEST(region_open_child_rejects_full_child_list);
    asx_runtime_reset();
    RUN_TEST(region_open_child_sets_parent_and_tracks_child);
    asx_runtime_reset();
    RUN_TEST(region_open_multiple_children_tracks_all_children);
    asx_runtime_reset();
    RUN_TEST(region_open_child_real_capacity_exhaustion);
    asx_runtime_reset();
    RUN_TEST(region_drain_closed_child_unlinks_from_parent);
    asx_runtime_reset();
    RUN_TEST(region_drain_closed_child_tolerates_missing_parent_slot);
    asx_runtime_reset();
    RUN_TEST(region_drain_parent_closes_open_child_first);
    RUN_TEST(region_drain_cancels_descendant_tasks_with_parent_attribution);
    asx_runtime_reset();
    RUN_TEST(closed_child_of_live_parent_keeps_its_slot);
    RUN_TEST(region_drain_nested_grandchild_closes_inside_out);
    asx_runtime_reset();
    RUN_TEST(quiescence_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(quiescence_open_region_returns_not_reached);
    asx_runtime_reset();
    RUN_TEST(quiescence_success_after_drain_no_tasks);
    asx_runtime_reset();
    RUN_TEST(quiescence_success_with_committed_obligation);
    asx_runtime_reset();
    RUN_TEST(quiescence_success_with_aborted_obligation);
    asx_runtime_reset();
    RUN_TEST(quiescence_closed_region_with_pending_cleanup_returns_not_reached);
    asx_runtime_reset();
    RUN_TEST(drain_null_budget_returns_invalid_argument);
    asx_runtime_reset();
    RUN_TEST(drain_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(drain_multiple_tasks_all_complete);
    asx_runtime_reset();
    RUN_TEST(drain_cleanup_lifo_multiple);
    asx_runtime_reset();
    RUN_TEST(drain_progress_null_out_returns_invalid_argument);
    asx_runtime_reset();
    RUN_TEST(drain_progress_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(drain_progress_open_region_with_tasks);
    asx_runtime_reset();
    RUN_TEST(drain_progress_with_obligations);
    asx_runtime_reset();
    RUN_TEST(drain_progress_after_partial_drain);
    asx_runtime_reset();
    RUN_TEST(drain_progress_cleanup_drained_flag);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_null_out_returns_invalid_argument);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_invalid_handle_returns_not_found);
    asx_runtime_reset();
    RUN_TEST(region_is_quiescent_invalid_handle_returns_false);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_fully_quiescent);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_live_tasks_q1_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_open_child_q2_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_all_children_closed_q2_passes);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_unresolved_obligations_q3_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_undrained_cleanup_q4_fails);
    asx_runtime_reset();
    RUN_TEST(quiescence_detailed_not_closed_not_quiescent);
    asx_runtime_reset();
    RUN_TEST(drain_progress_monotonicity_during_drain);
    asx_runtime_reset();
    RUN_TEST(drain_already_closed_is_idempotent);
    asx_runtime_reset();
    RUN_TEST(drain_full_lifecycle_tasks_obligations_cleanup);

    TEST_REPORT();
    return test_failures;
}
