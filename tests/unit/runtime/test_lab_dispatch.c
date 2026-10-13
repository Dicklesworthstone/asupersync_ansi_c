/*
 * test_lab_dispatch.c — the Rust LabRuntime dispatch model
 * (asx_scheduler_use_lab_dispatch, bd-9kll.4.2).
 *
 * Expected values come from asupersync 5e60b1c4c: xorshift64 outputs for a
 * seed (src/util/det_rng.rs) and the pick of entry r % n among the
 * highest-priority entries in generation order (scheduler/priority.rs).
 * The Rust-vs-C fixtures (make conformance, make fuzz-differential) check
 * the same model end to end against captured Rust runs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../src/conformance/json.h"
#include "../../../src/runtime/runtime_internal.h"
#include "../../../src/sync/wait_queue.h"
#include "test_harness.h"
#include <asx/actor/supervisor.h>
#include <asx/asx.h>
#include <asx/core/broadcast.h>
#include <asx/core/channel.h>
#include <asx/core/oneshot.h>
#include <asx/runtime/lab.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/security/crypto.h>
#include <stdlib.h>
#include <string.h>

static asx_runtime g_rt;

/* A runtime on the virtual clock (default stub hooks). The obligation post
 * tests below end with a task completing while it holds a checked
 * obligation, so the leak response is LOG explicitly, not the default. */
static asx_status setup(void) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;
    if (asx_runtime_is_initialized(&g_rt)) asx_runtime_shutdown(&g_rt);
    asx_runtime_config_init(&cfg);
    cfg.leak_response = ASX_LEAK_LOG;
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    return st;
}

/* Poll order record. */
static char g_order[32];
static uint32_t g_order_n = 0;

typedef struct {
    char name;
    uint32_t yields; /* polls that yield before completing */
    int checkpoint;  /* complete when a checkpoint reports a cancel */
} probe;

static asx_status poll_probe(void *ud, asx_task_id self) {
    probe *p = (probe *)ud;
    asx_checkpoint_result cr;
    if (g_order_n + 1u < sizeof(g_order)) g_order[g_order_n++] = p->name;
    if (p->checkpoint && asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    if (p->yields > 0u) {
        p->yields--;
        return ASX_E_PENDING; /* not parked: a yield */
    }
    return ASX_OK;
}

static void order_reset(void) {
    memset(g_order, 0, sizeof(g_order));
    g_order_n = 0;
}

TEST(rng_replicates_rust_xorshift64) {
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    /* DetRng::new(42).next_u64() x5 */
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(45454805674));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(11532217803599905471));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(10021416941527320954));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(2899061411254629736));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(5661411637479084162));
}

TEST(rng_remaps_degenerate_seeds) {
    ASSERT_EQ(setup(), ASX_OK);
    /* Seed 0 starts from state 1. */
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(0u), ASX_OK);
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(1082269761));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(1152992998833853505));
    /* All ones is offset by the golden-ratio constant. */
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(UINT64_C(0xFFFFFFFFFFFFFFFF)), ASX_OK);
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(15860402103189073388));
    ASSERT_EQ(asx_lab_rng_next(), UINT64_C(8426428137925997623));
}

/* The first 1000 outputs of Rust's DetRng for nine seeds, value for value:
 * schemas/rust_det_rng.json, which `twin_run rng` derives from the pinned
 * asupersync (make rust-rng-fresh re-derives it). The seeds are zero, all
 * five patterns DetRng corrects, and three ordinary ones. Fails closed: a
 * missing or trimmed document is a failure, not a pass over fewer seeds. */
static asx_json_doc g_rng_doc;
static char g_rng_file[1u << 20];

TEST(rng_replicates_rust_det_rng_for_every_seed_and_value_of_the_derived_document) {
    FILE *f = fopen("schemas/rust_det_rng.json", "rb");
    size_t n;
    uint32_t root;
    uint32_t seqs;
    uint32_t i;
    uint64_t count = 0;
    unsigned compared = 0;
    int saw_zero = 0;
    int saw_max = 0;

    ASSERT_TRUE(f != NULL);
    n = fread(g_rng_file, 1, sizeof(g_rng_file), f);
    fclose(f);
    ASSERT_TRUE(n > 0u && n < sizeof(g_rng_file));
    asx_json_doc_init(&g_rng_doc);
    ASSERT_EQ(asx_json_parse(&g_rng_doc, g_rng_file, n, &root), ASX_OK);
    ASSERT_STR_EQ(asx_json_get_string(&g_rng_doc, root, "schema"), "asx.rust_det_rng.v1");
    ASSERT_TRUE(asx_json_u64(&g_rng_doc, asx_json_get(&g_rng_doc, root, "count"), &count));
    ASSERT_EQ(count, (uint64_t)1000u);
    seqs = asx_json_get(&g_rng_doc, root, "sequences");
    ASSERT_TRUE(seqs != ASX_JSON_NONE);
    ASSERT_TRUE(asx_json_count(&g_rng_doc, seqs) >= 9u);

    for (i = 0; i < asx_json_count(&g_rng_doc, seqs); i++) {
        uint32_t seq = asx_json_item(&g_rng_doc, seqs, i);
        uint32_t values = asx_json_get(&g_rng_doc, seq, "values");
        uint64_t seed = 0;
        uint64_t k;
        uint64_t bad = UINT64_MAX;

        ASSERT_TRUE(asx_json_u64(&g_rng_doc, asx_json_get(&g_rng_doc, seq, "seed"), &seed));
        ASSERT_EQ(asx_json_count(&g_rng_doc, values), (uint32_t)count);
        ASSERT_EQ(setup(), ASX_OK);
        ASSERT_EQ(asx_scheduler_use_lab_dispatch(seed), ASX_OK);
        for (k = 0; k < count && bad == UINT64_MAX; k++) {
            uint64_t want = 0;
            ASSERT_TRUE(
                asx_json_u64(&g_rng_doc, asx_json_item(&g_rng_doc, values, (uint32_t)k), &want));
            if (asx_lab_rng_next() != want) bad = k;
        }
        /* The seed is in `seed`, the first mismatching output index in `bad`. */
        ASSERT_EQ(bad, UINT64_MAX);
        if (seed == 0u) saw_zero = 1;
        if (seed == UINT64_MAX) saw_max = 1;
        compared++;
    }
    ASSERT_TRUE(saw_zero);
    ASSERT_TRUE(saw_max);
    ASSERT_TRUE(compared >= 9u);
}

/* No heap allocation on the dispatch hot path (bd-9kll.4.2): the lab's lanes,
 * tie groups, retired-id table and certificate are static arrays. The test
 * installs an allocator that counts its calls, runs 16 tasks of 10 polls each
 * under lab dispatch (some 190 steps, every one a pick, a poll and a wake)
 * and requires the count not to move across the run. */
typedef struct {
    uint32_t calls;
} alloc_counter;

static void *counting_malloc(void *ctx, size_t n) {
    ((alloc_counter *)ctx)->calls++;
    return malloc(n);
}

static void *counting_realloc(void *ctx, void *p, size_t n) {
    ((alloc_counter *)ctx)->calls++;
    return realloc(p, n);
}

static void counting_free(void *ctx, void *p) {
    (void)ctx;
    free(p);
}

TEST(a_lab_dispatch_run_allocates_nothing) {
    enum { TASKS = 16 };
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    alloc_counter ctr;
    probe probes[TASKS];
    asx_region_id r;
    asx_budget budget;
    uint32_t i;
    uint32_t before;

    memset(&ctr, 0, sizeof(ctr));
    if (asx_runtime_is_initialized(&g_rt)) asx_runtime_shutdown(&g_rt);
    asx_runtime_config_init(&cfg);
    cfg.leak_response = ASX_LEAK_LOG;
    ASSERT_EQ(asx_runtime_hooks_init(&hooks), ASX_OK);
    hooks.allocator.ctx = &ctr;
    hooks.allocator.malloc_fn = counting_malloc;
    hooks.allocator.realloc_fn = counting_realloc;
    hooks.allocator.free_fn = counting_free;
    ASSERT_EQ(asx_runtime_init(&g_rt, &cfg, &hooks), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    order_reset();
    for (i = 0; i < (uint32_t)TASKS; i++) {
        asx_task_id t;
        probes[i].name = (char)('a' + i);
        probes[i].yields = 9u;
        probes[i].checkpoint = 0;
        ASSERT_EQ(asx_task_spawn(r, poll_probe, &probes[i], &t), ASX_OK);
    }

    before = ctr.calls;
    budget = asx_budget_from_polls(16u * 12u);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(ctr.calls, before);
    /* The run really dispatched (the order log keeps the first 31 polls). */
    ASSERT_EQ(g_order_n, 31u);
}

/* Seed 42, three host-spawned tasks at priority 0 (generations 0, 1, 2).
 * Step 1: r1 % 3 = 1 picks B, which yields (generation 3). Step 2:
 * [A, C, B], r2 % 3 = 1 picks C. Step 3: [A, B], r3 % 2 = 0 picks A.
 * Step 4: B. */
TEST(ties_are_broken_by_the_step_value_in_generation_order) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    probe a = {'A', 0u, 0};
    probe b = {'B', 1u, 0};
    probe c = {'C', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &t), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &c, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
}

/* First `n` Cx::random_usize(bound) draws of task `t`'s entropy stream. */
static int entropy_draws(asx_task_id t, uint32_t bound, uint32_t n, uint32_t *out) {
    asx_task_slot *slot;
    uint32_t i;
    if (asx_task_slot_lookup(t, &slot) != ASX_OK) return 0;
    for (i = 0; i < n; i++) {
        if (!asx_lab_entropy_index_internal(slot, bound, &out[i])) return 0;
    }
    return 1;
}

/* Host-spawned tasks fork the runtime's DetEntropy (seeded with the lab
 * seed) in creation order: task k (arena index k, fork counter k) draws
 * from xorshift64 seeded with mix_seed(seed + 0x9e3779b97f4a7c15 + k + k)
 * (util/entropy.rs:66-150). Expected values computed from Rust's
 * formulas; seed 10, task 0, bound 2 starts with 1, the pick the
 * Rust-captured fixture task-groups-race-same-round-tie-001 shows. */
TEST(host_tasks_draw_from_rust_detentropy_streams) {
    static const uint32_t want10[3][3] = {{1u, 1u, 1u}, {1u, 0u, 1u}, {0u, 0u, 1u}};
    static const uint32_t want42[3][2] = {{1u, 2u}, {2u, 0u}, {2u, 1u}};
    asx_region_id r;
    asx_task_id t[3];
    probe p = {'P', 0u, 0};
    uint32_t got[3];
    uint32_t i;
    uint32_t k;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(10u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(asx_task_spawn(r, poll_probe, &p, &t[i]), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_TRUE(entropy_draws(t[i], 2u, 3u, got));
        for (k = 0; k < 3u; k++) ASSERT_EQ(got[k], want10[i][k]);
    }

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    for (i = 0; i < 3u; i++) ASSERT_EQ(asx_task_spawn(r, poll_probe, &p, &t[i]), ASX_OK);
    for (i = 0; i < 3u; i++) {
        ASSERT_TRUE(entropy_draws(t[i], 3u, 2u, got));
        for (k = 0; k < 2u; k++) ASSERT_EQ(got[k], want42[i][k]);
    }
}

/* Rust's task arena frees a completed task's index (state.rs:8497-8503):
 * the next task reuses it with the generation bumped, and its entropy is
 * keyed by (1 << 32) | 1 at the runtime's third fork. Without lab
 * dispatch there is no stream and the caller decides. */
TEST(completed_tasks_free_their_arena_index_for_the_next_task) {
    asx_region_id r;
    asx_task_id a;
    asx_task_id b;
    asx_task_id c;
    asx_task_slot *slot;
    asx_budget budget;
    probe pa = {'A', 30u, 0};
    probe pb = {'B', 0u, 0};
    probe pc = {'C', 0u, 0};
    uint32_t got[3];
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pa, &a), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pb, &b), ASX_OK);
    budget = asx_budget_from_polls(10);
    /* B completes within these polls; A keeps yielding. */
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_task_slot_lookup(b, &slot), ASX_OK);
    ASSERT_EQ(slot->state, ASX_TASK_COMPLETED);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pc, &c), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(c, &slot), ASX_OK);
    ASSERT_EQ(slot->lab_rust_index, 1u);
    ASSERT_EQ(slot->lab_rust_gen, 1u);
    ASSERT_TRUE(entropy_draws(c, 5u, 3u, got));
    ASSERT_EQ(got[0], 2u);
    ASSERT_EQ(got[1], 4u);
    ASSERT_EQ(got[2], 4u);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);

    ASSERT_EQ(setup(), ASX_OK); /* no lab dispatch */
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &pb, &b), ASX_OK);
    ASSERT_EQ(asx_task_slot_lookup(b, &slot), ASX_OK);
    i = 7u;
    ASSERT_FALSE(asx_lab_entropy_index_internal(slot, 2u, &i));
    ASSERT_EQ(i, 7u);
}

/* A cancelled task goes to the cancel lane, which is served before the
 * ready lane whatever the step value. */
TEST(cancel_lane_is_served_first) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget budget;
    probe a = {'A', 0u, 1};
    probe b = {'B', 0u, 1};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    ASSERT_EQ(asx_task_cancel(tb, ASX_CANCEL_USER), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BA"), 0);
}

/* The cancel streak (LR:6324-6373, LAB_CANCEL_STREAK_LIMIT): the cancel
 * lane is served first for up to 16 consecutive dispatches, then the ready
 * lane gets one when it has an entry, then the cancel lane again. The
 * Rust corpus never reached it (its longest run is 10), so this pins it.
 * Ten parkers wait on a wait queue nothing wakes (a timer would let an
 * idle run's auto-advance wake them). Each is dispatched from the cancel
 * lane twice: the poll that acknowledges the cancel stays parked, so no
 * yield entry reaches the ready lane, and the acknowledgement puts it on
 * the cancel lane again, where its next poll completes it. Twenty
 * cancel-lane dispatches from eleven tasks, so a class of 16 tasks (R1)
 * reaches the streak too; with a spinner `R` the ready lane keeps an
 * entry. The order assertions hold for any seed: every lane here holds one
 * priority. */
#define STORM_LOG_MAX 200u
#define STORM_PARKERS 10u
#define STORM_CANCEL_DISPATCHES (2u * STORM_PARKERS)
static char g_storm_log[STORM_LOG_MAX];
static uint32_t g_storm_n;
static asx_wait_queue g_storm_q;

typedef struct {
    char name;
    uint32_t yields; /* the spinner's remaining yields */
    int acked;       /* a parker: its acknowledging poll has run */
} storm_task;

static asx_status poll_storm(void *ud, asx_task_id self) {
    storm_task *p = (storm_task *)ud;
    asx_checkpoint_result cr;
    if (g_storm_n < STORM_LOG_MAX) g_storm_log[g_storm_n++] = p->name;
    if (p->name == 'R') {
        if (p->yields > 0u) {
            p->yields--;
            return ASX_E_PENDING; /* a yield: back to the ready lane */
        }
        return ASX_OK;
    }
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) {
        if (!p->acked) {
            /* The acknowledging poll: still parked, so it is no yield (no
             * ready-lane entry); the acknowledgement alone puts it back on
             * the cancel lane. */
            p->acked = 1;
            (void)asx_wait_queue_park_current(&g_storm_q);
            return ASX_E_PENDING;
        }
        asx_wait_queue_leave_current(&g_storm_q);
        return ASX_OK; /* completed: its second cancel-lane dispatch */
    }
    (void)asx_wait_queue_park_current(&g_storm_q);
    return ASX_E_PENDING; /* parked until a wake: only a cancel comes */
}

/* Run in short slices until every parker has been polled once (so each is
 * parked, with no ready entry left); returns the distinct parkers polled. */
static uint32_t storm_settle(asx_region_id r) {
    uint32_t guard;
    for (guard = 0; guard < 80u; guard++) {
        uint32_t seen = 0;
        uint32_t k;
        asx_budget budget = asx_budget_from_polls(5u);
        asx_status st = asx_scheduler_run(r, &budget);
        (void)st;
        for (k = 0; k < STORM_PARKERS; k++) {
            uint32_t j;
            for (j = 0; j < g_storm_n; j++) {
                if (g_storm_log[j] == (char)('a' + k)) {
                    seen++;
                    break;
                }
            }
        }
        if (seen == STORM_PARKERS) return seen;
    }
    return 0;
}

static void storm_setup(asx_region_id *r, asx_task_id *parkers, storm_task *tasks, int spin,
                        uint32_t spin_yields) {
    uint32_t k;
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    memset(g_storm_log, 0, sizeof(g_storm_log));
    g_storm_n = 0;
    asx_wait_queue_init(&g_storm_q, NULL);
    ASSERT_EQ(asx_region_open(&g_rt, r), ASX_OK);
    for (k = 0; k < STORM_PARKERS; k++) {
        tasks[k].name = (char)('a' + k);
        tasks[k].yields = 0u;
        tasks[k].acked = 0;
        ASSERT_EQ(asx_task_spawn(*r, poll_storm, &tasks[k], &parkers[k]), ASX_OK);
    }
    if (spin) {
        asx_task_id spinner;
        tasks[STORM_PARKERS].name = 'R';
        tasks[STORM_PARKERS].yields = spin_yields;
        tasks[STORM_PARKERS].acked = 0;
        ASSERT_EQ(asx_task_spawn(*r, poll_storm, &tasks[STORM_PARKERS], &spinner), ASX_OK);
    }
}

TEST(a_cancel_storm_yields_to_the_ready_lane_after_sixteen_dispatches) {
    asx_region_id r;
    asx_task_id parkers[STORM_PARKERS];
    storm_task tasks[STORM_PARKERS + 1u];
    asx_budget budget;
    uint32_t start;
    uint32_t k;

    storm_setup(&r, parkers, tasks, 1, 400u);
    ASSERT_EQ(storm_settle(r), STORM_PARKERS);
    start = g_storm_n;
    for (k = 0; k < STORM_PARKERS; k++) {
        ASSERT_EQ(asx_task_cancel(parkers[k], ASX_CANCEL_USER), ASX_OK);
    }
    budget = asx_budget_from_polls(30u);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_TRUE(g_storm_n >= start + STORM_CANCEL_DISPATCHES + 4u);
    /* 16 cancel-lane dispatches ... */
    for (k = 0; k < 16u; k++) ASSERT_TRUE(g_storm_log[start + k] != 'R');
    /* ... then the spinner, although four cancels are still waiting ... */
    ASSERT_EQ(g_storm_log[start + 16u], 'R');
    /* ... then the cancel lane again for the rest of the storm ... */
    for (k = 17u; k <= STORM_CANCEL_DISPATCHES; k++) ASSERT_TRUE(g_storm_log[start + k] != 'R');
    /* ... and only the spinner after it. */
    for (k = STORM_CANCEL_DISPATCHES + 1u; k < STORM_CANCEL_DISPATCHES + 4u; k++) {
        ASSERT_EQ(g_storm_log[start + k], 'R');
    }
}

TEST(a_cancel_storm_with_an_empty_ready_lane_runs_on_past_sixteen) {
    asx_region_id r;
    asx_task_id parkers[STORM_PARKERS];
    storm_task tasks[STORM_PARKERS + 1u];
    asx_budget budget;
    uint32_t start;
    uint32_t k;

    storm_setup(&r, parkers, tasks, 0, 0u);
    ASSERT_EQ(storm_settle(r), STORM_PARKERS);
    start = g_storm_n;
    for (k = 0; k < STORM_PARKERS; k++) {
        ASSERT_EQ(asx_task_cancel(parkers[k], ASX_CANCEL_USER), ASX_OK);
    }
    budget = asx_budget_from_polls(100u);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    /* All twenty dispatches, back to back: past 16 the ready lane has
     * nothing to yield to (no parker left a yield entry), so the cancel
     * lane is served again. */
    ASSERT_EQ(g_storm_n, start + STORM_CANCEL_DISPATCHES);
    for (k = 0; k < STORM_CANCEL_DISPATCHES; k++) ASSERT_TRUE(g_storm_log[start + k] != 'R');
}

/* pop_cancel / pop_ready take the entries at the highest priority in
 * generation order, at most 256 of them (LAB_GROUP_CAP, PR:759-790), and
 * the step's value picks entry `r % n` of that group. Live tasks can never
 * form a 256-entry group (ASX_MAX_TASKS bounds them), but entries outlive
 * their task's turn: every schedule_cancel pushes another,
 * and a task cancelled again leaves its older ones behind. The test pushes
 * them directly: `n` entries on the cancel lane, the last of them task B's
 * and every other one task A's. Returns 1 when the pick for `r` is B, 0
 * when it is A, -1 when the setup or the pick failed. Without the cap,
 * r = n - 1 would always pick the last entry. */
static int cap_picks_b(uint32_t n, uint64_t r) {
    asx_region_id reg;
    asx_task_id ta;
    asx_task_id tb;
    asx_task_slot *sa;
    asx_task_slot *sb;
    probe pa = {'A', 0u, 0};
    probe pb = {'B', 0u, 0};
    uint32_t slot = 0;
    uint16_t gen = 0;
    int cancel = 0;
    uint32_t i;

    if (setup() != ASX_OK || asx_scheduler_use_lab_dispatch(42u) != ASX_OK) return -1;
    if (asx_region_open(&g_rt, &reg) != ASX_OK) return -1;
    if (asx_task_spawn(reg, poll_probe, &pa, &ta) != ASX_OK) return -1;
    if (asx_task_spawn(reg, poll_probe, &pb, &tb) != ASX_OK) return -1;
    if (asx_task_slot_lookup(ta, &sa) != ASX_OK || asx_task_slot_lookup(tb, &sb) != ASX_OK) {
        return -1;
    }
    for (i = 0; i + 1u < n; i++) asx_lab_schedule_cancel(sa, 0u);
    asx_lab_schedule_cancel(sb, 0u);
    if (asx_lab_dispatch_overflowed()) return -1;
    if (asx_lab_pick(r, &slot, &gen, &cancel) != 1 || !cancel) return -1;
    return asx_task_handle_for_slot(g_kernels[0], slot) == tb ? 1 : 0;
}

/* Run only where the lane (LAB_LANE_CAP, 8 * ASX_MAX_TASKS) can hold the
 * 300 entries the test pushes; main reports a SKIP otherwise. */
#define CAP_TEST_ENTRIES 300u

TEST(a_priority_group_is_capped_at_256_entries) {
    ASSERT_EQ(cap_picks_b(100u, 99u), 1);  /* below the cap: the last entry */
    ASSERT_EQ(cap_picks_b(256u, 255u), 1); /* a full group: still the last */
    /* Over the cap the group is the oldest 256, all of them A's, so no
     * step value reaches B while 257 or more entries wait. */
    ASSERT_EQ(cap_picks_b(257u, 256u), 0);
    ASSERT_EQ(cap_picks_b(257u, 5u), 0);
    ASSERT_EQ(cap_picks_b(300u, 299u), 0);
}

/* run_with_auto_advance: with nothing scheduled the clock moves to the next
 * timer, which wakes the sleeper. */
typedef struct {
    asx_time woke_at;
    int armed;
} sleeper;

static asx_status poll_sleeper(void *ud, asx_task_id self) {
    sleeper *s = (sleeper *)ud;
    asx_status st = asx_task_wait_until(self, 100u);
    s->armed = 1;
    if (st == ASX_OK) {
        s->woke_at = asx_runtime_virtual_now(&g_rt);
        return ASX_OK;
    }
    return st;
}

TEST(auto_advance_moves_the_clock_to_the_next_timer) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    sleeper s;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_sleeper, &s, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    /* run_until_idle stops with the sleeper parked and the clock at 0. */
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &budget), ASX_E_PENDING);
    ASSERT_TRUE(s.armed);
    ASSERT_EQ(asx_runtime_virtual_now(&g_rt), (asx_time)0);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(s.woke_at, (asx_time)100);
}

/* Spends its one-poll quota yielding, then (pre-poll quota cancel on poll
 * 2) either checkpoints or just completes. */
typedef struct {
    uint32_t polls;
    int checkpoint;
} quota_probe;

static asx_status poll_quota_probe(void *ud, asx_task_id self) {
    quota_probe *q = (quota_probe *)ud;
    asx_checkpoint_result cr;
    q->polls++;
    if (q->polls == 1u) return ASX_E_PENDING;
    if (q->checkpoint && asx_checkpoint(self, &cr) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

static asx_status quota_reason(int checkpoint, asx_region_id *out_root, asx_cancel_reason *out) {
    asx_task_id t = ASX_INVALID_ID;
    asx_budget tb;
    asx_budget run;
    quota_probe q;
    asx_outcome o;
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    if (st == ASX_OK) st = asx_region_open(&g_rt, out_root);
    memset(&q, 0, sizeof(q));
    q.checkpoint = checkpoint;
    tb = asx_budget_from_polls(1);
    if (st == ASX_OK) st = asx_task_spawn_with_budget(*out_root, poll_quota_probe, &q, &tb, &t);
    run = asx_budget_from_polls(20);
    if (st == ASX_OK) st = asx_scheduler_run(*out_root, &run);
    if (st == ASX_OK) st = asx_task_get_outcome(t, &o);
    if (st == ASX_OK) st = asx_task_get_cancel_reason(t, out);
    return st;
}

/* Rust's lab stamps the pre-poll quota cancel with CancelReason::poll_quota()'s
 * testing defaults (lab/runtime.rs:4667): region at arena index 0, no task,
 * 1 s. A checkpoint afterwards re-attributes it (earlier timestamp wins). */
TEST(pre_poll_quota_cancel_has_rust_lab_attribution) {
    asx_region_id root;
    asx_cancel_reason r;
    ASSERT_EQ(quota_reason(0, &root, &r), ASX_OK);
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_EQ(r.origin_region, root);
    ASSERT_EQ(r.origin_task, ASX_INVALID_ID);
    ASSERT_EQ(r.timestamp, (asx_time)1000000000u);
    ASSERT_EQ(quota_reason(1, &root, &r), ASX_OK);
    ASSERT_EQ((int)r.kind, (int)ASX_CANCEL_POLL_QUOTA);
    ASSERT_TRUE(r.origin_task != ASX_INVALID_ID);
    ASSERT_EQ(r.timestamp, (asx_time)0);
}

/* Dispatch records (asx_scheduler_record_dispatches): Rust's
 * ForcedDispatch fields, steps counted from 1 across runs. Two tasks that
 * yield once each: four ready-lane dispatches at time 0. A buffer too
 * small keeps the first records and still counts every dispatch. */
TEST(dispatch_records_carry_task_lane_step_and_time) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget run;
    probe a = {'a', 1u, 0};
    probe b = {'b', 1u, 0};
    asx_dispatch_record rec[3];
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    memset(rec, 0, sizeof(rec));
    asx_scheduler_record_dispatches(rec, 3u);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    run = asx_budget_from_polls(50);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 4u);
    for (i = 0; i < 3u; i++) {
        ASSERT_EQ(rec[i].step, (uint64_t)(i + 1u));
        ASSERT_EQ(rec[i].at, (asx_time)0);
        ASSERT_EQ(rec[i].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
        ASSERT_TRUE(asx_handle_index(rec[i].task) == asx_handle_index(ta) ||
                    asx_handle_index(rec[i].task) == asx_handle_index(tb));
    }
    /* A new lab stops recording. */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 0u);
}

/* A checkpoint that raises a budget cancel leaves the task's cancel waker
 * due; it fires with the acknowledgement, after the poll, even when that
 * poll completes the task: Rust then schedules the retired task id and the
 * next step dispatches it, polling nothing (record/task.rs:1227-1233,
 * lab/runtime.rs:5011, 4772-4790; fixture budget-poll-quota-exhaustion-001).
 * Without the checkpoint nothing is due. */
static asx_status run_quota_probe(int checkpoint, quota_probe *q, asx_dispatch_record *rec,
                                  uint32_t cap, asx_task_id *out_t) {
    asx_region_id root = ASX_INVALID_ID;
    asx_budget tb;
    asx_budget run;
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    asx_scheduler_record_dispatches(rec, cap);
    if (st == ASX_OK) st = asx_region_open(&g_rt, &root);
    memset(q, 0, sizeof(*q));
    q->checkpoint = checkpoint;
    tb = asx_budget_from_polls(1);
    if (st == ASX_OK) st = asx_task_spawn_with_budget(root, poll_quota_probe, q, &tb, out_t);
    run = asx_budget_from_polls(20);
    if (st == ASX_OK) st = asx_scheduler_run(root, &run);
    return st;
}

TEST(budget_cancel_at_a_completing_checkpoint_dispatches_the_retired_task) {
    asx_cancel_reason reason;
    asx_dispatch_record rec[8];
    asx_task_id t;
    quota_probe q;

    memset(rec, 0, sizeof(rec));
    ASSERT_EQ(run_quota_probe(1, &q, rec, 8u, &t), ASX_OK);
    ASSERT_EQ(q.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 3u);
    ASSERT_EQ(rec[0].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
    ASSERT_EQ(rec[2].lane, (uint8_t)ASX_DISPATCH_LANE_CANCEL);
    ASSERT_EQ(rec[2].step, (uint64_t)3);
    ASSERT_TRUE(asx_handle_index(rec[2].task) == asx_handle_index(t));
    ASSERT_EQ(asx_task_get_cancel_reason(t, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_POLL_QUOTA);
    /* No checkpoint, no acknowledgement: two dispatches. */
    ASSERT_EQ(run_quota_probe(0, &q, rec, 8u, &t), ASX_OK);
    ASSERT_EQ(q.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 2u);
}

/* The budget-deadline timer (Rust Cx::arm_budget_deadline, armed at
 * creation): at the deadline it cancels with DEADLINE stamped with the
 * deadline and wakes the task on the cancel lane, though the task waits
 * for a later time (fixture budget-deadline-sleep-checkpoint-001). It is a
 * pending timer: auto-advance moves the clock to it for a task parked with
 * nothing else to wake it. */
typedef struct {
    uint32_t polls;
    asx_time wait_until; /* 0: park */
} deadline_probe;

static asx_status poll_deadline_probe(void *ud, asx_task_id self) {
    deadline_probe *d = (deadline_probe *)ud;
    asx_checkpoint_result cr;
    asx_status st;
    d->polls++;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    if (d->wait_until != 0u) return asx_task_wait_until(self, d->wait_until);
    st = asx_task_park(self);
    return st == ASX_OK ? ASX_E_PENDING : st;
}

static asx_status run_deadline_probe(deadline_probe *d, asx_dispatch_record *rec, uint32_t cap,
                                     asx_task_id *out_t) {
    asx_region_id r = ASX_INVALID_ID;
    asx_budget b = asx_budget_infinite();
    asx_budget run;
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(9u);
    asx_scheduler_record_dispatches(rec, cap);
    if (st == ASX_OK) st = asx_region_open(&g_rt, &r);
    b.deadline = 100u;
    if (st == ASX_OK) st = asx_task_spawn_with_budget(r, poll_deadline_probe, d, &b, out_t);
    run = asx_budget_from_polls(50);
    if (st == ASX_OK) st = asx_scheduler_run(r, &run);
    return st;
}

TEST(budget_deadline_timer_cancels_on_the_cancel_lane) {
    deadline_probe d;
    asx_dispatch_record rec[4];
    asx_task_id t;
    asx_cancel_reason reason;

    memset(&d, 0, sizeof(d));
    d.wait_until = 200u;
    memset(rec, 0, sizeof(rec));
    ASSERT_EQ(run_deadline_probe(&d, rec, 4u, &t), ASX_OK);
    ASSERT_EQ(d.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 2u);
    ASSERT_EQ(rec[1].at, (asx_time)100);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_CANCEL);
    ASSERT_EQ(asx_task_get_cancel_reason(t, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_DEADLINE);
    ASSERT_EQ(reason.timestamp, (asx_time)100);

    /* Parked with no timer of its own: the clock moves to the deadline. */
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(run_deadline_probe(&d, rec, 4u, &t), ASX_OK);
    ASSERT_EQ(d.polls, 2u);
    ASSERT_EQ(asx_runtime_virtual_now(&g_rt), (asx_time)100);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_CANCEL);
}

/* Child-region commands (Rust RegionCommand, LR:4135-4217). The opener
 * opens a child region of `parent`, optionally spawns a kid into it that
 * yields until a checkpoint reports its cancel, then closes the region. */
typedef struct {
    asx_region_id parent;
    int spawn_kid;
    uint32_t polls;
    uint32_t phase;
    asx_status first; /* the first open call */
    asx_region_id child;
    asx_task_id kid;
} opener;

static asx_status poll_kid(void *ud, asx_task_id self) {
    asx_checkpoint_result cr;
    (void)ud;
    if (asx_checkpoint(self, &cr) == ASX_OK && cr.cancelled) return ASX_OK;
    return ASX_E_PENDING; /* a yield */
}

static asx_status poll_opener(void *ud, asx_task_id self) {
    opener *o = (opener *)ud;
    asx_status st;
    o->polls++;
    if (o->phase == 0u) {
        st = asx_region_open_child_poll(self, o->parent, NULL, &o->child);
        if (o->polls == 1u) o->first = st;
        if (st != ASX_OK) return st;
        if (o->spawn_kid && asx_task_spawn(o->child, poll_kid, NULL, &o->kid) != ASX_OK) {
            return ASX_E_INVALID_STATE;
        }
        o->phase = 1u;
    }
    return asx_region_close_poll(self, o->child);
}

static asx_status run_opener(int lab, int spawn_kid, opener *o) {
    asx_task_id t;
    asx_budget budget;
    asx_status st = setup();
    if (st == ASX_OK && lab) st = asx_scheduler_use_lab_dispatch(42u);
    memset(o, 0, sizeof(*o));
    o->spawn_kid = spawn_kid;
    o->child = ASX_INVALID_ID;
    if (st == ASX_OK) st = asx_region_open(&g_rt, &o->parent);
    if (st == ASX_OK) st = asx_task_spawn(o->parent, poll_opener, o, &t);
    budget = asx_budget_from_polls(100);
    if (st == ASX_OK) st = asx_scheduler_run(o->parent, &budget);
    return st;
}

/* The open is applied at the start of the next step: the first call parks
 * the opener; it is woken once with the region. The close is a command
 * too; the opener stays parked until the CLOSED transition wakes it, so it
 * is polled exactly three times. The kid is cancelled with the reason
 * Rust's Close carries. */
TEST(child_region_open_and_close_are_next_step_commands) {
    opener o;
    asx_region_state rs;
    asx_outcome out;
    asx_cancel_reason reason;

    ASSERT_EQ(run_opener(1, 1, &o), ASX_OK);
    ASSERT_EQ(o.first, ASX_E_PENDING);
    ASSERT_EQ(o.polls, 3u);
    ASSERT_EQ(asx_region_get_state(o.child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_get_outcome(o.kid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(o.kid, &reason), ASX_OK);
    ASSERT_EQ((int)reason.kind, (int)ASX_CANCEL_USER);
    ASSERT_EQ(strcmp(reason.message, "owned child region body finished"), 0);
    ASSERT_EQ(reason.origin_task, ASX_INVALID_ID);
    ASSERT_EQ(reason.timestamp, (asx_time)1000000000u);
}

/* Closing an empty child region: the Close command finalizes it at once
 * when applied, and that wakes the parked closer. */
TEST(closing_an_empty_child_region_wakes_the_closer) {
    opener o;
    asx_region_state rs;
    ASSERT_EQ(run_opener(1, 0, &o), ASX_OK);
    ASSERT_EQ(o.polls, 3u);
    ASSERT_EQ(asx_region_get_state(o.child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
}

/* Without lab dispatch the commands apply at once: the open completes in
 * the first call, the close cancels the kid at once and the closer waits
 * parked for CLOSED only. */
TEST(child_region_commands_apply_at_once_without_lab_dispatch) {
    opener o;
    asx_region_state rs;
    asx_outcome out;
    ASSERT_EQ(run_opener(0, 1, &o), ASX_OK);
    ASSERT_EQ(o.first, ASX_OK);
    ASSERT_EQ(o.polls, 2u);
    ASSERT_EQ(asx_region_get_state(o.child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_task_get_outcome(o.kid, &out), ASX_OK);
    ASSERT_EQ((int)out.severity, (int)ASX_OUTCOME_CANCELLED);
}

/* A child-region open that carries an envelope (Rust Cx::open_child_region
 * with a ChildRegionSpec's capability_budget and requirements, bd-9kll.3.27)
 * is a command like any open: planned when the scheduler applies it, so a
 * refusal reaches the opener after it parked. The opener tries the open
 * (phase 0), then a plain one through its Cx (phase 1). */
typedef struct {
    asx_region_id parent;
    asx_capability_budget spec;
    asx_capability_requirements requirements;
    int narrow_cx;
    asx_capability_budget narrow;
    asx_cx cx;
    uint32_t polls;
    uint32_t phase;
    asx_status first;  /* the first call of phase 0 */
    asx_status result; /* phase 0's result */
    asx_capability_refusal refusal;
    asx_region_id child;
    asx_status plain_first;
    asx_status plain_result;
    asx_region_id plain_child;
} cap_opener;

static asx_status poll_cap_opener(void *ud, asx_task_id self) {
    cap_opener *o = (cap_opener *)ud;
    asx_status st;
    o->polls++;
    if (o->polls == 1u) {
        st = asx_cx_init(&o->cx, o->parent, self, ASX_CAP_SPAWN);
        if (st == ASX_OK && o->narrow_cx) {
            st = asx_cx_apply_child_capability_budget(&o->cx, &o->narrow, NULL, NULL, NULL);
        }
        if (st != ASX_OK) return st;
    }
    if (o->phase == 0u) {
        st = asx_cx_open_child_region(&o->cx, NULL, &o->spec, &o->requirements, &o->child,
                                      &o->refusal);
        if (o->first == ASX_OK && o->polls == 1u) o->first = st;
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        o->result = st;
        o->phase = 1u;
    }
    if (o->phase == 1u) {
        st = asx_cx_open_child_region(&o->cx, NULL, NULL, NULL, &o->plain_child, NULL);
        if (o->plain_first == ASX_OK) o->plain_first = st;
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        o->plain_result = st;
        o->phase = 2u;
    }
    return ASX_OK;
}

static asx_status run_cap_opener(cap_opener *o, asx_region_id *region_out) {
    asx_task_id t;
    asx_capability_budget env = asx_capability_budget_unspecified();
    asx_budget budget = asx_budget_from_polls(100);
    asx_status st = setup();
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    env = asx_capability_budget_with_memory_bytes(env, 1024u);
    env = asx_capability_budget_with_cpu_units(env, 100u);
    o->child = ASX_INVALID_ID;
    o->plain_child = ASX_INVALID_ID;
    o->first = ASX_OK;
    o->plain_first = ASX_OK;
    if (st == ASX_OK) st = asx_region_open_with_capability_budget(&g_rt, &env, &o->parent);
    if (st == ASX_OK) st = asx_task_spawn(o->parent, poll_cap_opener, o, &t);
    if (st == ASX_OK) st = asx_scheduler_run(o->parent, &budget);
    *region_out = o->parent;
    return st;
}

/* The refusal arrives with the second call, after the opener parked, and
 * nothing was minted: the region count is the parent's alone, as after any
 * other refused open. The plain open that follows is admitted, and the
 * spent dimension is not replenished by it (Rust
 * child_region_cannot_replenish_an_exhausted_caller_envelope). */
TEST(capability_open_refusal_reaches_the_opener_after_it_parked) {
    cap_opener o;
    asx_region_id parent;
    asx_capability_budget got;

    memset(&o, 0, sizeof(o));
    o.narrow_cx = 1;
    o.narrow = asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 0u);
    o.spec = asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 4096u);
    o.requirements.cpu_units = 1u;
    ASSERT_EQ(run_cap_opener(&o, &parent), ASX_OK);

    ASSERT_EQ(o.first, ASX_E_PENDING);
    ASSERT_EQ(o.result, ASX_E_CAPABILITY_BUDGET_REFUSED);
    ASSERT_EQ(o.refusal.kind, ASX_CAPABILITY_EXHAUSTED);
    ASSERT_EQ(o.refusal.dimension, ASX_CAPABILITY_CPU_UNITS);
    ASSERT_EQ(o.child, ASX_INVALID_ID);

    /* Plain open: PENDING, then admitted, carrying the Cx's zero. */
    ASSERT_EQ(o.plain_first, ASX_E_PENDING);
    ASSERT_EQ(o.plain_result, ASX_OK);
    ASSERT_EQ(asx_region_capability_budget(o.plain_child, &got), ASX_OK);
    ASSERT_TRUE(got.has_cpu_units);
    ASSERT_EQ(got.cpu_units, (uint64_t)0);
    ASSERT_EQ(got.memory_bytes, (uint64_t)1024);
    ASSERT_EQ(asx_region_capability_budget(parent, &got), ASX_OK);
    ASSERT_EQ(got.cpu_units, (uint64_t)100);        /* the region's own is not touched */
    ASSERT_EQ(asx_runtime_region_count(&g_rt), 2u); /* the parent and the plain child */
}

/* The permitted near-identical case: the Cx has not spent its CPU, so the
 * same open with the same requirement is admitted at the same step, with
 * the meet of the Cx's, the region's and the spec's. */
TEST(capability_open_is_admitted_at_the_same_step_with_the_meet) {
    cap_opener o;
    asx_region_id parent;
    asx_capability_budget got;

    memset(&o, 0, sizeof(o));
    o.spec = asx_capability_budget_with_cpu_units(asx_capability_budget_unspecified(), 4096u);
    o.requirements.cpu_units = 1u;
    ASSERT_EQ(run_cap_opener(&o, &parent), ASX_OK);

    ASSERT_EQ(o.first, ASX_E_PENDING);
    ASSERT_EQ(o.result, ASX_OK);
    ASSERT_EQ(asx_region_capability_budget(o.child, &got), ASX_OK);
    ASSERT_EQ(got.cpu_units, (uint64_t)100); /* the region's, not the spec's 4096 */
    ASSERT_EQ(got.memory_bytes, (uint64_t)1024);
    ASSERT_EQ(asx_runtime_region_count(&g_rt), 3u); /* the parent and two children */
}

/* The poll quota a task spawned from a poll inherits is what its spawner
 * has left when it spawns (Cx::inherited_budget, cx.rs:2504). The lab's
 * region-open command costs the owner a dispatch, so an owner with a quota
 * of 4 that opens a region and then spawns has used 2 polls under lab
 * dispatch and 1 without it: its members inherit 2 and 3. That is the one
 * difference between the lab's run of task-groups-join-all-region-cancel-001
 * and the sweep's (bd-9kll.4.16). With a members' quota of 2 the sweep's
 * observations equal Rust's (measured with the quota forced on a scratch
 * build: the fixture then matches under compare-sweep, and fails under lab
 * dispatch with the quota forced to 3); Rust's own dispatch log shows the
 * owner's two dispatches at 0 before the members' first. */
typedef struct {
    asx_region_id region;
    asx_region_id child;
    uint32_t polls;
    asx_task_id member;
    uint32_t member_quota;
} quota_owner;

static asx_status poll_quota_member(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static asx_status poll_quota_owner(void *ud, asx_task_id self) {
    quota_owner *o = (quota_owner *)ud;
    asx_status st;
    asx_budget b;
    o->polls++;
    st = asx_region_open_child_poll(self, o->region, NULL, &o->child);
    if (st != ASX_OK) return st;
    st = asx_task_spawn(o->region, poll_quota_member, NULL, &o->member);
    if (st != ASX_OK) return st;
    if (asx_task_get_budget(o->member, &b) != ASX_OK) return ASX_E_INVALID_STATE;
    o->member_quota = b.poll_quota;
    return ASX_OK;
}

static asx_status run_quota_owner(int lab, quota_owner *o) {
    asx_task_id t;
    asx_budget owner_budget = asx_budget_from_polls(4);
    asx_budget run = asx_budget_from_polls(50);
    asx_status st = setup();
    if (st == ASX_OK && lab) st = asx_scheduler_use_lab_dispatch(42u);
    memset(o, 0, sizeof(*o));
    o->child = ASX_INVALID_ID;
    if (st == ASX_OK) st = asx_region_open(&g_rt, &o->region);
    if (st == ASX_OK) {
        st = asx_task_spawn_with_budget(o->region, poll_quota_owner, o, &owner_budget, &t);
    }
    if (st == ASX_OK) st = asx_scheduler_run(o->region, &run);
    return st;
}

TEST(spawned_members_inherit_what_the_owner_has_left_after_its_region_command) {
    quota_owner o;

    ASSERT_EQ(run_quota_owner(1, &o), ASX_OK);
    ASSERT_EQ(o.polls, 2u); /* parked at the open, woken with the region */
    ASSERT_EQ(o.member_quota, 2u);

    ASSERT_EQ(run_quota_owner(0, &o), ASX_OK);
    ASSERT_EQ(o.polls, 1u); /* the open applies at once */
    ASSERT_EQ(o.member_quota, 3u);
}

/* A region cancel request from a task (Rust ChildRegion::cancel) leaves
 * the region open until the next step applies it. */
typedef struct {
    asx_region_id target;
    uint32_t polls;
    asx_status request;
    asx_region_state at_request;
    asx_region_state next_poll;
} canceller;

static asx_status poll_canceller(void *ud, asx_task_id self) {
    canceller *c = (canceller *)ud;
    (void)self;
    c->polls++;
    if (c->polls == 1u) {
        asx_cancel_reason reason;
        memset(&reason, 0, sizeof(reason));
        reason.kind = ASX_CANCEL_SHUTDOWN;
        reason.origin_region = c->target;
        reason.origin_task = ASX_INVALID_ID;
        c->request = asx_region_cancel_request(c->target, &reason);
        if (asx_region_get_state(c->target, &c->at_request) != ASX_OK) return ASX_E_INVALID_STATE;
        return ASX_E_PENDING; /* a yield */
    }
    if (asx_region_get_state(c->target, &c->next_poll) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_OK;
}

TEST(region_cancel_request_applies_at_the_next_step) {
    asx_region_id root;
    asx_task_id t;
    asx_budget budget;
    canceller c;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    memset(&c, 0, sizeof(c));
    ASSERT_EQ(asx_region_open(&g_rt, &root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &c.target), ASX_OK);
    ASSERT_EQ(asx_task_spawn(root, poll_canceller, &c, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(root, &budget), ASX_OK);
    ASSERT_EQ(c.request, ASX_OK);
    ASSERT_EQ((int)c.at_request, (int)ASX_REGION_OPEN);
    ASSERT_EQ((int)c.next_poll, (int)ASX_REGION_CLOSED);
}

/* A full command queue refuses the request and changes nothing; the
 * queued commands still apply, at most eight per step. */
TEST(region_command_queue_full_is_failure_atomic) {
    asx_region_id root;
    asx_region_id child;
    asx_region_state rs;
    asx_budget budget;
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(5u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &root), ASX_OK);
    ASSERT_EQ(asx_region_open_child(root, &child), ASX_OK);
    for (i = 0; i < (uint32_t)ASX_MAX_TASKS + (uint32_t)ASX_MAX_REGIONS; i++) {
        ASSERT_EQ(asx_region_close_request(child), ASX_OK);
    }
    ASSERT_EQ(asx_region_close_request(child), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_region_get_state(child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_OPEN);
    budget = asx_budget_from_polls(1000);
    ASSERT_EQ(asx_scheduler_run_until_idle(root, &budget), ASX_OK);
    ASSERT_EQ(asx_region_get_state(child, &rs), ASX_OK);
    ASSERT_EQ((int)rs, (int)ASX_REGION_CLOSED);
    ASSERT_EQ(asx_lab_region_commands_pending(), 0);
}

/* Join-handle aborts (Rust JoinHandle::abort_with_reason): applied at the
 * next step. The aborter aborts `target` (or a child it spawns first) on
 * its first poll, records the target's state right after, and yields. */
typedef struct {
    asx_region_id region;
    asx_task_id target;
    int spawn_child;
    int twice;
    uint32_t polls;
    asx_status request;
    asx_task_state at_request;
} aborter;

static asx_status poll_aborter(void *ud, asx_task_id self) {
    aborter *a = (aborter *)ud;
    asx_cancel_reason reason;
    a->polls++;
    if (a->polls > 1u) return ASX_OK;
    if (a->spawn_child && asx_task_spawn(a->region, poll_kid, NULL, &a->target) != ASX_OK) {
        return ASX_E_INVALID_STATE;
    }
    memset(&reason, 0, sizeof(reason));
    reason.kind = ASX_CANCEL_USER;
    reason.origin_region = a->region;
    reason.origin_task = self;
    a->request = asx_task_abort_request(a->target, &reason);
    if (a->twice && a->request == ASX_OK) {
        reason.kind = ASX_CANCEL_SHUTDOWN;
        a->request = asx_task_abort_request(a->target, &reason);
    }
    if (asx_task_get_state(a->target, &a->at_request) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING; /* a yield */
}

static asx_status run_aborter(int lab, int spawn_child, int twice, aborter *a) {
    asx_task_id t;
    asx_budget budget;
    asx_status st = setup();
    if (st == ASX_OK && lab) st = asx_scheduler_use_lab_dispatch(9u);
    memset(a, 0, sizeof(*a));
    a->spawn_child = spawn_child;
    a->twice = twice;
    if (st == ASX_OK) st = asx_region_open(&g_rt, &a->region);
    if (st == ASX_OK && !spawn_child) st = asx_task_spawn(a->region, poll_kid, NULL, &a->target);
    if (st == ASX_OK) st = asx_task_spawn(a->region, poll_aborter, a, &t);
    budget = asx_budget_from_polls(100);
    if (st == ASX_OK) st = asx_scheduler_run(a->region, &budget);
    return st;
}

static int cancelled_with(asx_task_id t, asx_cancel_kind kind) {
    asx_outcome out;
    asx_cancel_reason reason;
    if (asx_task_get_outcome(t, &out) != ASX_OK) return 0;
    if (out.severity != ASX_OUTCOME_CANCELLED) return 0;
    if (asx_task_get_cancel_reason(t, &reason) != ASX_OK) return 0;
    return reason.kind == kind;
}

/* The abort leaves the target's record alone until the next step applies
 * it; the target then ends cancelled with the abort's reason. */
TEST(handle_abort_applies_at_the_next_step) {
    aborter a;
    ASSERT_EQ(run_aborter(1, 0, 0, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_TRUE(a.at_request != ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_USER));
}

/* A child aborted in the poll that spawned it takes the abort as it is
 * admitted: its first poll already sees the cancel. */
TEST(handle_abort_of_an_unadmitted_child_applies_at_admission) {
    aborter a;
    ASSERT_EQ(run_aborter(1, 1, 0, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_USER));
}

/* Two aborts of one task in a step coalesce; the stronger reason wins. */
TEST(handle_aborts_in_one_step_coalesce_to_the_strongest) {
    aborter a;
    ASSERT_EQ(run_aborter(1, 0, 1, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_TRUE(a.at_request != ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_SHUTDOWN));
}

/* Without lab dispatch the abort applies at once. */
TEST(handle_abort_applies_at_once_without_lab_dispatch) {
    aborter a;
    ASSERT_EQ(run_aborter(0, 0, 0, &a), ASX_OK);
    ASSERT_EQ(a.request, ASX_OK);
    ASSERT_EQ((int)a.at_request, (int)ASX_TASK_CANCEL_REQUESTED);
    ASSERT_TRUE(cancelled_with(a.target, ASX_CANCEL_USER));
}

/* A spawn from a poll into a closed region: refused at once, but Rust's
 * spawn mailbox refuses it at the next step's admission, and a join of it
 * waits for that (fixture spawn-into-cancelled-region-step-001). */
typedef struct {
    asx_region_id closed;
    uint32_t polls;
    uint32_t ticket;
    asx_status spawn;
} refuser;

static asx_status poll_noop(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    return ASX_OK;
}

static asx_status poll_refuser(void *ud, asx_task_id self) {
    refuser *f = (refuser *)ud;
    f->polls++;
    if (f->polls == 1u) {
        asx_task_id kid = ASX_INVALID_ID;
        f->spawn = asx_task_spawn(f->closed, poll_noop, NULL, &kid);
        f->ticket = asx_scheduler_last_spawn_refusal();
    }
    return asx_task_await_refusal(self, f->ticket);
}

TEST(spawn_refusal_reaches_a_join_at_the_next_step) {
    asx_region_id r;
    asx_task_id t;
    asx_budget run;
    asx_cancel_reason reason;
    asx_dispatch_record rec[4];
    refuser f;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(5u), ASX_OK);
    asx_scheduler_record_dispatches(rec, 4u);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    memset(&f, 0, sizeof(f));
    ASSERT_EQ(asx_region_open_child(r, &f.closed), ASX_OK);
    memset(&reason, 0, sizeof(reason));
    reason.kind = ASX_CANCEL_USER;
    reason.origin_region = f.closed;
    ASSERT_EQ(asx_region_cancel(f.closed, &reason, NULL), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_refuser, &f, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_last_spawn_refusal(), 0u);
    run = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(f.spawn, ASX_E_REGION_CLOSED);
    ASSERT_TRUE(f.ticket != 0u);
    /* Poll 1 refuses and waits; step 2's admission delivers the refusal
     * and wakes it on the ready lane; poll 2 sees it delivered. */
    ASSERT_EQ(f.polls, 2u);
    ASSERT_EQ(asx_scheduler_dispatches_recorded(), 2u);
    ASSERT_EQ(rec[1].step, (uint64_t)2);
    ASSERT_EQ(rec[1].lane, (uint8_t)ASX_DISPATCH_LANE_READY);
    ASSERT_EQ(asx_task_await_refusal(t, f.ticket), ASX_OK);
    ASSERT_EQ(asx_task_await_refusal(t, 0u), ASX_OK);
}

/* bd-orxy: under lab dispatch a spawn from a poll is admitted at the next
 * step, as Rust's spawn mailbox is, and the admission checks the region:
 * max_tasks against the live tasks then (the spawner, completed in the
 * same poll, no longer counts; spawns admitted earlier in the step do),
 * and whether the region still accepts work. A refused child never runs
 * and completes Cancelled. */
typedef struct {
    asx_region_id into;
    uint32_t spawns;
    int cancel_into; /* cancel `into` right after spawning (synchronously) */
    int cleanup_only;
    asx_task_id kids[3];
    asx_status spawned[3];
    asx_status pending[3]; /* asx_task_admission_status right after the spawn */
} admitter;

static uint32_t g_kid_polls;

static asx_status poll_counted_kid(void *ud, asx_task_id self) {
    (void)ud;
    (void)self;
    g_kid_polls++;
    return ASX_OK;
}

static asx_status poll_admitter(void *ud, asx_task_id self) {
    admitter *a = (admitter *)ud;
    uint32_t i;
    (void)self;
    for (i = 0; i < a->spawns; i++) {
        a->spawned[i] = a->cleanup_only
                            ? asx_task_spawn_cleanup(a->into, poll_counted_kid, NULL, &a->kids[i])
                            : asx_task_spawn(a->into, poll_counted_kid, NULL, &a->kids[i]);
        a->pending[i] = asx_task_admission_status(a->kids[i]);
    }
    if (a->cancel_into) {
        asx_cancel_reason reason;
        memset(&reason, 0, sizeof(reason));
        reason.kind = ASX_CANCEL_USER;
        reason.origin_region = a->into;
        if (asx_region_cancel(a->into, &reason, NULL) != ASX_OK) return ASX_E_INVALID_STATE;
    }
    return ASX_OK; /* completes in the spawning poll */
}

/* Arms a wake timer at 100 on its first poll; a later poll before then
 * parks without polling that sleep again; at 100 it completes. */
typedef struct {
    char name;
    int phase;
} stale_sleeper;

static asx_status poll_stale_sleeper(void *ud, asx_task_id self) {
    stale_sleeper *s = (stale_sleeper *)ud;
    asx_time now = 0;
    if (g_order_n + 1u < sizeof(g_order)) g_order[g_order_n++] = s->name;
    if (asx_runtime_now_ns(&g_rt, &now) == ASX_OK && now >= 100u) return ASX_OK;
    if (s->phase == 0) {
        s->phase = 1;
        return asx_task_wait_until(self, 100u);
    }
    if (asx_task_park(self) != ASX_OK) return ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

/* A Rust Sleep wakes the waker it registered last, at that waker's
 * priority. S arms its timer at priority 0; its budget priority then
 * becomes 200 (in Rust a cancel's cleanup budget does that; set directly
 * here so no cancel lane is involved) and S is polled again, getting a new
 * waker, without polling its sleep. At 100 the timer still wakes S at 0,
 * behind M (150), although S's timer is first in registration order (fuzz
 * gen-17-81, bd-1maj). */
TEST(a_timer_wakes_the_waker_it_was_armed_with) {
    asx_region_id r;
    asx_task_id ts;
    asx_task_id tm;
    asx_task_slot *slot;
    asx_budget run;
    asx_budget high = asx_budget_infinite();
    stale_sleeper s = {'S', 0};
    stale_sleeper m = {'M', 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(5u), ASX_OK);
    order_reset();
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_stale_sleeper, &s, &ts), ASX_OK);
    high.priority = 150u;
    ASSERT_EQ(asx_task_spawn_with_budget(r, poll_stale_sleeper, &m, &high, &tm), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &run), ASX_E_PENDING);
    ASSERT_EQ(g_order_n, 2u); /* both armed their timers at 100 */

    ASSERT_EQ(asx_task_slot_lookup(ts, &slot), ASX_OK);
    slot->budget.priority = 200u;
    ASSERT_EQ(asx_task_wake(ts), ASX_OK);
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &run), ASX_E_PENDING);
    ASSERT_EQ(g_order_n, 3u); /* S polled again, with a new waker at 200 */
    ASSERT_EQ(slot->lab_waker_prio, 200u);

    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(g_order_n, 5u);
    ASSERT_EQ(g_order[3], 'M');
    ASSERT_EQ(g_order[4], 'S');
}

/* Obligation posts (bd-2fga): a poll's obligation operations, under lab
 * dispatch, are applied in order when the poll returns, as Rust's
 * obligation mailbox does. */
static uint32_t g_ob_events;
static uint32_t g_ob_events_in_poll;
static asx_trace_event_kind g_ob_kinds[8];

static void observe_obligations(void *ctx, const asx_trace_event *ev,
                                const asx_trace_payload *payload) {
    (void)ctx;
    (void)payload;
    if (ev->kind != ASX_TRACE_OBLIGATION_RESERVE && ev->kind != ASX_TRACE_OBLIGATION_COMMIT &&
        ev->kind != ASX_TRACE_OBLIGATION_ABORT && ev->kind != ASX_TRACE_OBLIGATION_LEAK) {
        return;
    }
    if (g_ob_events < 8u) g_ob_kinds[g_ob_events] = ev->kind;
    g_ob_events++;
}

typedef struct {
    asx_region_id region;
    int permit_first; /* register the permit before the checked reserve */
    asx_obligation_id checked;
    asx_obligation_id permit;
    asx_status checked_st;
    asx_status permit_st;
} oblig_poster;

static asx_status poll_oblig_poster(void *ud, asx_task_id self) {
    oblig_poster *p = (oblig_poster *)ud;
    if (p->permit_first) {
        p->permit_st =
            asx_obligation_register(p->region, ASX_OBLIGATION_KIND_SEND_PERMIT, self, &p->permit);
        p->checked_st =
            asx_obligation_reserve_ex(p->region, ASX_OBLIGATION_KIND_LEASE, self, &p->checked);
    } else {
        p->checked_st =
            asx_obligation_reserve_ex(p->region, ASX_OBLIGATION_KIND_LEASE, self, &p->checked);
        p->permit_st =
            asx_obligation_register(p->region, ASX_OBLIGATION_KIND_SEND_PERMIT, self, &p->permit);
        /* A checked token's commit releases its count at once. */
        if (p->checked_st == ASX_OK && asx_obligation_commit(p->checked) != ASX_OK) {
            return ASX_E_INVALID_STATE;
        }
        if (p->permit_st == ASX_OK && asx_obligation_commit(p->permit) != ASX_OK) {
            return ASX_E_INVALID_STATE;
        }
    }
    g_ob_events_in_poll = g_ob_events;
    return ASX_OK;
}

static asx_status oblig_region(asx_region_id *r) {
    asx_region_limits limits;
    asx_status st = asx_region_open(&g_rt, r);
    if (st != ASX_OK) return st;
    limits.max_tasks = ASX_REGION_UNLIMITED;
    limits.max_children = ASX_REGION_UNLIMITED;
    limits.max_obligations = 1u;
    return asx_region_set_limits(*r, &limits);
}

/* max_obligations 1: reserve (checked), register a permit, commit both, in
 * one poll. The permit is admitted when the poll returns, after the
 * checked commit released the count; nothing is traced during the poll,
 * then the four events in call order (probe p1). */
TEST(a_permit_registration_is_admitted_when_the_poll_returns) {
    asx_region_id r;
    asx_task_id t;
    asx_budget run;
    asx_obligation_state s;
    oblig_poster p;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_OK);
    ASSERT_EQ(oblig_region(&r), ASX_OK);
    memset(&p, 0, sizeof(p));
    p.region = r;
    g_ob_events = 0;
    asx_trace_set_observer(&g_rt, observe_obligations, NULL);
    ASSERT_EQ(asx_task_spawn(r, poll_oblig_poster, &p, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    asx_trace_set_observer(&g_rt, NULL, NULL);

    ASSERT_EQ(p.checked_st, ASX_OK);
    ASSERT_EQ(p.permit_st, ASX_OK);
    ASSERT_EQ(g_ob_events_in_poll, 0u);
    ASSERT_EQ(g_ob_events, 4u);
    ASSERT_EQ(g_ob_kinds[0], ASX_TRACE_OBLIGATION_RESERVE);
    ASSERT_EQ(g_ob_kinds[1], ASX_TRACE_OBLIGATION_RESERVE);
    ASSERT_EQ(g_ob_kinds[2], ASX_TRACE_OBLIGATION_COMMIT);
    ASSERT_EQ(g_ob_kinds[3], ASX_TRACE_OBLIGATION_COMMIT);
    ASSERT_EQ(asx_obligation_get_state(p.permit, &s), ASX_OK);
    ASSERT_EQ(s, ASX_OBLIGATION_COMMITTED);
}

/* max_obligations 1: register a permit, then reserve (checked) in one
 * poll. The checked reserve is admitted at the call (the permit does not
 * count yet); at the drain the region is full, so the permit is refused:
 * untracked, its id no longer resolves, nothing traced for it. */
TEST(a_permit_registration_past_the_limit_is_refused_at_the_drain) {
    asx_region_id r;
    asx_task_id t;
    asx_budget run;
    asx_obligation_state s;
    oblig_poster p;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_OK);
    ASSERT_EQ(oblig_region(&r), ASX_OK);
    memset(&p, 0, sizeof(p));
    p.region = r;
    p.permit_first = 1;
    g_ob_events = 0;
    asx_trace_set_observer(&g_rt, observe_obligations, NULL);
    ASSERT_EQ(asx_task_spawn(r, poll_oblig_poster, &p, &t), ASX_OK);
    run = asx_budget_from_polls(10);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    asx_trace_set_observer(&g_rt, NULL, NULL);

    ASSERT_EQ(p.permit_st, ASX_OK);  /* posted */
    ASSERT_EQ(p.checked_st, ASX_OK); /* admitted at the call */
    ASSERT_TRUE(asx_obligation_get_state(p.permit, &s) != ASX_OK);
    /* The checked reservation, then its leak when its holder completed. */
    ASSERT_EQ(g_ob_events, 2u);
    ASSERT_EQ(g_ob_kinds[0], ASX_TRACE_OBLIGATION_RESERVE);
    ASSERT_EQ(g_ob_kinds[1], ASX_TRACE_OBLIGATION_LEAK);
}

/* bd-6ncn: a permit that outlives its reserving poll must still settle
 * physically when its deferred obligation registration was refused. A
 * real checked lease fills the quota; no ledger or channel state is forged. */
typedef struct {
    asx_region_id region;
    int kind;   /* 0: MPSC, 1: oneshot, 2: broadcast */
    int action; /* 0: send, 1: abort, 2: receiver disconnected, 3: holder ends */
    uint32_t polls;
    asx_channel_id channel;
    asx_send_permit mpsc;
    asx_oneshot_sender sender;
    asx_oneshot_receiver receiver;
    asx_oneshot_permit oneshot;
    asx_broadcast_sender broadcast_sender;
    asx_broadcast_receiver broadcast_receiver;
    asx_broadcast_permit broadcast;
    asx_obligation_id registration;
    asx_obligation_id lease;
} quota_channel;

static void quota_channel_step(quota_channel *p, asx_task_id self) {
    asx_obligation_state state;
    uint64_t value = 0u;
    uint32_t count = 99u;

    if (p->polls == 1u) {
        asx_cx cx;
        ASSERT_EQ(asx_cx_init(&cx, p->region, self, ASX_CAP_NONE), ASX_OK);
        if (p->kind == 0) {
            ASSERT_EQ(asx_channel_reserve(p->channel, &cx, &p->mpsc), ASX_OK);
            p->registration = p->mpsc.obligation;
        } else if (p->kind == 1) {
            ASSERT_EQ(asx_oneshot_reserve(&p->sender, &cx, &p->oneshot), ASX_OK);
            p->registration = p->oneshot.obligation;
        } else {
            ASSERT_EQ(asx_broadcast_reserve(&p->broadcast_sender, &cx, &p->broadcast), ASX_OK);
            p->registration = p->broadcast.obligation;
        }
        ASSERT_NE(p->registration, ASX_INVALID_ID);
        ASSERT_EQ(asx_obligation_get_state(p->registration, &state), ASX_OK);
        ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);
        ASSERT_EQ(asx_obligation_reserve_ex(p->region, ASX_OBLIGATION_KIND_LEASE, self, &p->lease),
                  ASX_OK);
        return;
    }

    ASSERT_EQ(p->polls, 2u);
    ASSERT_NE(asx_obligation_get_state(p->registration, &state), ASX_OK);
    ASSERT_EQ(asx_obligation_get_state(p->lease, &state), ASX_OK);
    ASSERT_EQ(state, ASX_OBLIGATION_RESERVED);
    if (p->kind == 0) {
        asx_send_permit copy = p->mpsc;
        ASSERT_EQ(asx_channel_reserved_count(p->channel, &count), ASX_OK);
        ASSERT_EQ(count, 1u);
        if (p->action == 1) {
            asx_send_permit_abort(&p->mpsc);
        } else {
            if (p->action == 2) ASSERT_EQ(asx_channel_close_receiver(p->channel), ASX_OK);
            ASSERT_EQ(asx_send_permit_send(&p->mpsc, 77u),
                      p->action == 2 ? ASX_E_DISCONNECTED : ASX_OK);
        }
        ASSERT_EQ(p->mpsc.consumed, 1);
        ASSERT_NE(asx_send_permit_send(&copy, 99u), ASX_OK);
        ASSERT_EQ(asx_channel_reserved_count(p->channel, &count), ASX_OK);
        ASSERT_EQ(count, 0u);
        ASSERT_EQ(asx_channel_queue_len(p->channel, &count), ASX_OK);
        ASSERT_EQ(count, p->action == 0 ? 1u : 0u);
        if (p->action == 0) {
            ASSERT_EQ(asx_channel_try_recv(p->channel, &value), ASX_OK);
            ASSERT_EQ(value, 77u);
        }
        if (p->action != 2) {
            ASSERT_EQ(asx_channel_try_recv(p->channel, &value), ASX_E_WOULD_BLOCK);
            ASSERT_EQ(asx_channel_try_reserve(p->channel, &p->mpsc), ASX_OK);
            asx_send_permit_abort(&p->mpsc);
            ASSERT_EQ(asx_channel_reserved_count(p->channel, &count), ASX_OK);
            ASSERT_EQ(count, 0u);
        }
    } else if (p->kind == 1) {
        asx_oneshot_permit copy = p->oneshot;
        if (p->action == 1) {
            asx_oneshot_permit_abort(&p->oneshot);
        } else {
            if (p->action == 2) asx_oneshot_receiver_drop(&p->receiver);
            ASSERT_EQ(asx_oneshot_permit_send(&p->oneshot, 77u),
                      p->action == 2 ? ASX_E_DISCONNECTED : ASX_OK);
        }
        ASSERT_EQ(p->oneshot.live, 0);
        ASSERT_NE(asx_oneshot_permit_send(&copy, 99u), ASX_OK);
        if (p->action != 2) {
            ASSERT_EQ(asx_oneshot_try_recv(&p->receiver, &value),
                      p->action == 0 ? ASX_OK : ASX_E_DISCONNECTED);
            ASSERT_EQ(value, p->action == 0 ? 77u : 0u);
        }
    } else {
        asx_broadcast_permit copy = p->broadcast;
        if (p->action == 3) {
            /* Holder completion below drops the refused physical credit. */
        } else if (p->action == 1) {
            asx_broadcast_permit_abort(&p->broadcast);
        } else {
            if (p->action == 2) asx_broadcast_receiver_drop(&p->broadcast_receiver);
            ASSERT_EQ(asx_broadcast_permit_send(&p->broadcast, 77u, &count), ASX_OK);
            ASSERT_EQ(count, p->action == 0 ? 1u : 0u);
        }
        if (p->action != 3) {
            ASSERT_EQ(p->broadcast.consumed, 1);
            ASSERT_NE(asx_broadcast_permit_send(&copy, 99u, &count), ASX_OK);
        }
        ASSERT_EQ(asx_broadcast_total_sent(&p->broadcast_sender), p->action == 0 ? 1u : 0u);
        if (p->action == 0) {
            ASSERT_EQ(asx_broadcast_try_recv(&p->broadcast_receiver, &value), ASX_OK);
            ASSERT_EQ(value, 77u);
        }
        if (p->action != 2) {
            ASSERT_EQ(asx_broadcast_try_recv(&p->broadcast_receiver, &value), ASX_E_WOULD_BLOCK);
        }
    }
    ASSERT_EQ(asx_obligation_commit(p->lease), ASX_OK);
}

static asx_status poll_quota_channel(void *data, asx_task_id self) {
    quota_channel *p = (quota_channel *)data;
    p->polls++;
    quota_channel_step(p, self);
    if (test_current_failed) return ASX_E_INVALID_STATE;
    return p->polls == 1u ? ASX_E_PENDING : ASX_OK;
}

static void check_quota_channel(int kind) {
    int action;
    for (action = 0; action < (kind == 2 ? 4 : 3); action++) {
        quota_channel p;
        asx_task_id task;
        asx_outcome outcome;
        asx_budget run;
        asx_obligation_state state;
        asx_status status;

        ASSERT_EQ(setup(), ASX_OK);
        ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_OK);
        memset(&p, 0, sizeof(p));
        p.kind = kind;
        p.action = action;
        ASSERT_EQ(oblig_region(&p.region), ASX_OK);
        if (kind == 0) {
            ASSERT_EQ(asx_channel_create(p.region, 1u, &p.channel), ASX_OK);
        } else if (kind == 1) {
            ASSERT_EQ(asx_oneshot_create(&p.sender, &p.receiver), ASX_OK);
        } else {
            ASSERT_EQ(asx_broadcast_create(1u, &p.broadcast_sender, &p.broadcast_receiver), ASX_OK);
        }
        ASSERT_EQ(asx_task_spawn(p.region, poll_quota_channel, &p, &task), ASX_OK);
        run = asx_budget_from_polls(10u);
        status = asx_scheduler_run(p.region, &run);
        if (test_current_failed) return;
        ASSERT_EQ(status, ASX_OK);
        ASSERT_EQ(p.polls, 2u);
        ASSERT_EQ(asx_task_join(task, &outcome), ASX_OK);
        ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
        if (action == 3) {
            uint32_t count = 99u;
            ASSERT_EQ(asx_broadcast_permit_send(&p.broadcast, 99u, &count),
                      ASX_E_OBLIGATION_ALREADY_RESOLVED);
            ASSERT_EQ(count, 0u);
            ASSERT_EQ(asx_broadcast_total_sent(&p.broadcast_sender), 0u);
        }
        ASSERT_EQ(asx_obligation_get_state(p.lease, &state), ASX_OK);
        ASSERT_EQ(state, ASX_OBLIGATION_COMMITTED);
        ASSERT_EQ(asx_obligation_leak_count(&g_rt), (uint64_t)0u);
        run = asx_budget_from_polls(10u);
        ASSERT_EQ(asx_region_drain(p.region, &run), ASX_OK);
        ASSERT_EQ(asx_quiescence_check(p.region), ASX_OK);
    }
}

TEST(mpsc_quota_refused_permit_settles_on_a_later_poll) { check_quota_channel(0); }

TEST(oneshot_quota_refused_permit_settles_on_a_later_poll) { check_quota_channel(1); }

TEST(broadcast_quota_refused_permit_settles_on_a_later_poll) { check_quota_channel(2); }

TEST(spawn_limit_is_checked_at_the_next_step_admission) {
    asx_region_id r;
    asx_region_limits limits;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget run;
    asx_outcome outcome;
    asx_cancel_reason reason;
    admitter a;
    probe b = {'B', 10u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    memset(&a, 0, sizeof(a));
    a.into = r;
    a.spawns = 2u;
    g_kid_polls = 0;
    ASSERT_EQ(asx_task_spawn(r, poll_admitter, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &tb), ASX_OK);
    limits.max_tasks = 2u;
    limits.max_children = ASX_REGION_UNLIMITED;
    limits.max_obligations = ASX_REGION_UNLIMITED;
    ASSERT_EQ(asx_region_set_limits(r, &limits), ASX_OK);
    ASSERT_EQ(asx_task_admission_status(ta), ASX_OK); /* host tasks are not deferred */
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    /* Both spawns returned their task: the call checks nothing. */
    ASSERT_EQ(a.spawned[0], ASX_OK);
    ASSERT_EQ(a.spawned[1], ASX_OK);
    ASSERT_EQ(a.pending[0], ASX_E_PENDING);
    ASSERT_EQ(a.pending[1], ASX_E_PENDING);
    /* At admission the spawner had completed: B and the first child make
     * 2 live tasks, so the second child is refused, without running. */
    ASSERT_EQ(asx_task_admission_status(a.kids[0]), ASX_OK);
    ASSERT_EQ(asx_task_admission_status(a.kids[1]), ASX_E_ADMISSION_LIMIT);
    ASSERT_EQ(g_kid_polls, 1u);
    ASSERT_EQ(asx_task_get_outcome(a.kids[1], &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_CANCELLED);
    ASSERT_EQ(asx_task_get_cancel_reason(a.kids[1], &reason), ASX_OK);
    ASSERT_EQ(reason.kind, ASX_CANCEL_USER);
    ASSERT_TRUE(reason.message != NULL &&
                strcmp(reason.message, "[ASUP-E006] region admission limit reached") == 0);
    ASSERT_EQ(reason.timestamp, (asx_time)1000000000u);
    ASSERT_TRUE(asx_runtime_is_quiescent(&g_rt));
}

TEST(spawn_into_a_region_closed_before_admission_is_refused) {
    asx_region_id r;
    asx_region_id c;
    asx_task_id ta;
    asx_budget run;
    asx_cancel_reason reason;
    admitter a;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_region_open_child(r, &c), ASX_OK);
    memset(&a, 0, sizeof(a));
    a.into = c;
    a.spawns = 1u;
    a.cancel_into = 1;
    g_kid_polls = 0;
    ASSERT_EQ(asx_task_spawn(r, poll_admitter, &a, &ta), ASX_OK);
    run = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);

    ASSERT_EQ(a.spawned[0], ASX_OK);
    /* The region closed before the next step: Rust's admission refuses the
     * spawn (SpawnError::RegionClosed), resolving it ParentCancelled. */
    ASSERT_EQ(asx_task_admission_status(a.kids[0]), ASX_E_REGION_CLOSED);
    ASSERT_EQ(g_kid_polls, 0u);
    ASSERT_EQ(asx_task_get_cancel_reason(a.kids[0], &reason), ASX_OK);
    ASSERT_EQ(reason.kind, ASX_CANCEL_PARENT);
    ASSERT_TRUE(asx_runtime_is_quiescent(&g_rt));
}

TEST(deferred_cleanup_admission_preserves_finalizing_quota_exception) {
    asx_region_id r;
    asx_region_id c;
    asx_region_slot *region;
    asx_region_limits limits;
    asx_task_id ta;
    asx_budget run;
    asx_outcome outcome;
    admitter a;
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_region_open_child(r, &c), ASX_OK);
    ASSERT_EQ(asx_region_get_limits(c, &limits), ASX_OK);
    limits.max_tasks = 0u;
    ASSERT_EQ(asx_region_set_limits(c, &limits), ASX_OK);
    ASSERT_EQ(asx_region_slot_lookup(c, &region), ASX_OK);
    region->state = ASX_REGION_FINALIZING;
    memset(&a, 0, sizeof(a));
    a.into = c;
    a.spawns = 1u;
    a.cleanup_only = 1;
    g_kid_polls = 0;
    ASSERT_EQ(asx_task_spawn(r, poll_admitter, &a, &ta), ASX_OK);
    run = asx_budget_from_polls(20u);
    ASSERT_EQ(asx_scheduler_run(r, &run), ASX_OK);
    ASSERT_EQ(a.spawned[0], ASX_OK);
    ASSERT_EQ(a.pending[0], ASX_E_PENDING);
    ASSERT_EQ(asx_task_admission_status(a.kids[0]), ASX_OK);
    ASSERT_EQ(g_kid_polls, 1u);
    ASSERT_EQ(asx_task_get_outcome(a.kids[0], &outcome), ASX_OK);
    ASSERT_EQ(outcome.severity, ASX_OUTCOME_OK);
    ASSERT_EQ(region->state, ASX_REGION_CLOSED);
}

TEST(admission_status_of_a_bad_handle_is_a_lookup_error) {
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_TRUE(asx_task_admission_status(ASX_INVALID_ID) != ASX_OK);
}

TEST(use_lab_dispatch_requires_no_live_task) {
    asx_region_id r;
    asx_task_id t;
    probe a = {'A', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(1u), ASX_E_INVALID_STATE);
    /* A runtime reset turns it off again. */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_lab_dispatch_active(), 0);
}

/* ------------------------------------------------------------------ */
/* Forced schedules (bd-9kll.9.3)                                      */
/* ------------------------------------------------------------------ */

/* Artifacts Rust recorded (twin_run, asupersync 5e60b1c4c), as stored in
 * each fixture's schedule.forced_schedule; certificate_hash is the same
 * fixture's schedule.certificate_hash. */

/* fixtures/rust_reference_v2/budget-cancel-is-requested-001.json,
 * certificate_hash 3cab0825147f5102 */
static const char g_fixture_budget_cancel[] =
    "415355504653430001000000010000002a0000000000000090d2dff9ac13f4de"
    "0100000000000000000000000000000000000000020100000000000000000000"
    "00000000000100000000000000000000000000000002517f142508ab3c010026"
    "f516a32a53fdd583a15a3bb22b083ff0a43dacd96523100d2050780e9a7c37";

/* fixtures/rust_reference_v2/obligation-cancelled-holder-leaks-001.json,
 * certificate_hash 0f5abfedcc170262 */
static const char g_fixture_cancel_lane[] =
    "415355504653430001000000010000002a0000000000000090d2dff9ac13f4de"
    "0200000000000000000000000000000000000000020100000000000000000000"
    "0000000000000000000000000000000000000200000000000000320000000000"
    "000002000000000000003200000000000000620217ccedbf5a0f010070dd1ff2"
    "8ab8011fc346d4e08d775410dbe898d1c7b301675995a283a82e0235";

/* fixtures/rust_reference_v2/sync-mutex-fifo-001.json,
 * certificate_hash 3f9e6d4a78eeebef */
static const char g_fixture_mutex_fifo[] =
    "415355504653430001000000010000002a000000000000002b5a4cf3950579ae"
    "0300000000000000000000000000000000000000020100000000000000000000"
    "0000000000000000000000000000000000020200000000000000000000000000"
    "0000000000000100000000000000020300000000000000000000000000000003"
    "000000000000000000000000000000efebee784a6d9e3f01002405fdd5791a8b"
    "4199f0d46363d6638b57a074aa1114620cac667f9fd229b9e8";

#define FS_TEST_MAX_BYTES 512u
#define FS_TEST_MAX_DISPATCHES 16u

static size_t fs_hex_decode(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex) / 2u;
    size_t i;

    if (n > cap) return 0u;
    for (i = 0u; i < n; i++) {
        unsigned hi = (unsigned)(hex[2u * i] <= '9' ? hex[2u * i] - '0' : hex[2u * i] - 'a' + 10);
        unsigned lo = (unsigned)(hex[2u * i + 1u] <= '9' ? hex[2u * i + 1u] - '0'
                                                         : hex[2u * i + 1u] - 'a' + 10);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static asx_forced_decode_limits fs_limits(void) {
    asx_forced_decode_limits l;
    l.max_encoded_bytes = FS_TEST_MAX_BYTES;
    l.max_dispatches = FS_TEST_MAX_DISPATCHES;
    return l;
}

/* The checksum a canonical artifact carries, recomputed after a test edits
 * a byte, so only the edit's own check can refuse it. */
static void fs_restamp(uint8_t *bytes, size_t len) {
    static const char domain[] = "asupersync.lab.forced-schedule.artifact.v1";
    asx_sha256_ctx ctx;

    asx_sha256_init(&ctx);
    asx_sha256_update(&ctx, domain, sizeof(domain));
    asx_sha256_update(&ctx, bytes, len - 32u);
    asx_sha256_final(&ctx, bytes + len - 32u);
}

typedef struct {
    const char *hex;
    uint32_t count;
    uint64_t certificate;
    uint64_t config_hash;
    uint64_t steps;
    uint64_t nanos;
} fs_fixture;

TEST(forced_schedule_decodes_rust_recorded_artifacts_and_reproduces_their_certificate) {
    static const fs_fixture fixtures[3] = {
        {g_fixture_budget_cancel, 1u, UINT64_C(0x3cab0825147f5102), UINT64_C(0xdef413acf9dfd290),
         1u, 0u},
        {g_fixture_cancel_lane, 2u, UINT64_C(0x0f5abfedcc170262), UINT64_C(0xdef413acf9dfd290), 2u,
         50u},
        {g_fixture_mutex_fifo, 3u, UINT64_C(0x3f9e6d4a78eeebef), UINT64_C(0xae790595f34c5a2b), 3u,
         0u},
    };
    uint32_t f;

    for (f = 0u; f < 3u; f++) {
        uint8_t bytes[FS_TEST_MAX_BYTES];
        uint8_t again[FS_TEST_MAX_BYTES];
        asx_forced_dispatch storage[FS_TEST_MAX_DISPATCHES];
        asx_forced_decode_limits limits = fs_limits();
        asx_forced_schedule s;
        asx_forced_error err;
        size_t len = fs_hex_decode(fixtures[f].hex, bytes, sizeof(bytes));
        size_t out_len = 0u;

        ASSERT_TRUE(len > 0u);
        ASSERT_EQ(len, asx_forced_schedule_encoded_size(fixtures[f].count));
        ASSERT_EQ(asx_forced_schedule_decode(bytes, len, &limits, storage, FS_TEST_MAX_DISPATCHES,
                                             &s, &err),
                  ASX_OK);
        ASSERT_EQ(s.version, ASX_FORCED_SCHEDULE_SCHEMA_VERSION);
        ASSERT_EQ(s.seed, (uint64_t)42u);
        ASSERT_EQ(s.config_hash, fixtures[f].config_hash);
        ASSERT_EQ(s.dispatch_count, fixtures[f].count);
        ASSERT_EQ(s.terminal_steps, fixtures[f].steps);
        ASSERT_EQ(s.terminal_nanos, fixtures[f].nanos);
        ASSERT_EQ(s.terminal_quiescent, (uint8_t)1u);
        ASSERT_EQ(s.truncated, (uint8_t)0u);

        /* The certificate asx folds from the dispatches is Rust's. */
        ASSERT_EQ(s.terminal_schedule_hash, fixtures[f].certificate);
        ASSERT_EQ(asx_forced_schedule_certificate(s.dispatches, s.dispatch_count),
                  fixtures[f].certificate);

        /* And the canonical bytes round-trip exactly. */
        ASSERT_EQ(asx_forced_schedule_encode(&s, again, sizeof(again), &out_len, &err), ASX_OK);
        ASSERT_EQ(out_len, len);
        ASSERT_EQ(memcmp(again, bytes, len), 0);
    }
}

TEST(forced_schedule_decoded_dispatch_fields_match_the_recorded_run) {
    uint8_t bytes[FS_TEST_MAX_BYTES];
    asx_forced_dispatch storage[FS_TEST_MAX_DISPATCHES];
    asx_forced_decode_limits limits = fs_limits();
    asx_forced_schedule s;
    asx_forced_error err;
    size_t len = fs_hex_decode(g_fixture_mutex_fifo, bytes, sizeof(bytes));

    ASSERT_EQ(asx_forced_schedule_decode(bytes, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_OK);
    /* "1@0 t.a ready", "2@0 t.a ready", "3@0 t.b ready": arena index 0, 0, 1 */
    ASSERT_EQ(s.dispatches[0].task, (uint64_t)0u);
    ASSERT_EQ(s.dispatches[1].task, (uint64_t)0u);
    ASSERT_EQ(s.dispatches[2].task, (uint64_t)1u << 32);
    ASSERT_EQ(s.dispatches[0].lane, (uint8_t)ASX_FORCED_LANE_READY);
    ASSERT_EQ(s.dispatches[0].at_step, (uint64_t)1u);
    ASSERT_EQ(s.dispatches[1].at_step, (uint64_t)2u);
    ASSERT_EQ(s.dispatches[2].at_step, (uint64_t)3u);
    ASSERT_EQ(s.dispatches[2].worker, 0u);
    ASSERT_EQ(s.dispatches[2].at_nanos, (uint64_t)0u);

    /* The cancel lane tag and the virtual time of a timed cancel. */
    len = fs_hex_decode(g_fixture_cancel_lane, bytes, sizeof(bytes));
    ASSERT_EQ(asx_forced_schedule_decode(bytes, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_OK);
    ASSERT_EQ(s.dispatches[1].lane, (uint8_t)ASX_FORCED_LANE_CANCEL);
    ASSERT_EQ(s.dispatches[1].at_step, (uint64_t)2u);
    ASSERT_EQ(s.dispatches[1].at_nanos, (uint64_t)50u);
}

TEST(forced_schedule_decode_refuses_what_rust_refuses) {
    uint8_t good[FS_TEST_MAX_BYTES];
    uint8_t bad[FS_TEST_MAX_BYTES];
    asx_forced_dispatch storage[FS_TEST_MAX_DISPATCHES];
    asx_forced_decode_limits limits = fs_limits();
    asx_forced_schedule s;
    asx_forced_error err;
    size_t len = fs_hex_decode(g_fixture_mutex_fifo, good, sizeof(good));
    const size_t terminal = 40u + 3u * 29u;

    ASSERT_EQ(len, (size_t)185u);

    /* A flipped payload byte: the checksum binds every field. */
    memcpy(bad, good, len);
    bad[50] ^= 0x01u;
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_CHECKSUM);

    /* Short, long. */
    ASSERT_EQ(asx_forced_schedule_decode(good, len - 1u, &limits, storage, FS_TEST_MAX_DISPATCHES,
                                         &s, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TRUNCATED);
    ASSERT_EQ(asx_forced_schedule_decode(good, 39u, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TRUNCATED);
    memcpy(bad, good, len);
    bad[len] = 0u;
    ASSERT_EQ(asx_forced_schedule_decode(bad, len + 1u, &limits, storage, FS_TEST_MAX_DISPATCHES,
                                         &s, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TRAILING_BYTES);

    /* Magic and versions are checked before the checksum. */
    memcpy(bad, good, len);
    bad[0] ^= 0xFFu;
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_MAGIC);
    memcpy(bad, good, len);
    bad[8] = 2u;
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_ARTIFACT_VERSION);
    memcpy(bad, good, len);
    bad[12] = 2u;
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_SCHEMA_VERSION);

    /* Caller limits are checked before anything is read. */
    limits.max_encoded_bytes = len - 1u;
    ASSERT_EQ(asx_forced_schedule_decode(good, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_BYTE_LIMIT);
    limits = fs_limits();
    limits.max_dispatches = 2u;
    ASSERT_EQ(asx_forced_schedule_decode(good, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_DISPATCH_LIMIT);
    limits = fs_limits();
    ASSERT_EQ(asx_forced_schedule_decode(good, len, &limits, storage, 2u, &s, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_DISPATCH_LIMIT);

    /* With a valid checksum, each remaining check refuses on its own. */
    memcpy(bad, good, len);
    bad[40u + 29u + 12u] = 9u; /* dispatch 1 lane */
    fs_restamp(bad, len);
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_LANE_TAG);
    ASSERT_EQ(err.index, 1u);

    memcpy(bad, good, len);
    bad[terminal + 24u] = 2u; /* quiescent flag */
    fs_restamp(bad, len);
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_BOOL_TAG);

    memcpy(bad, good, len);
    bad[terminal + 25u] = 1u; /* truncated */
    fs_restamp(bad, len);
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_PARTIAL_SOURCE);

    memcpy(bad, good, len);
    bad[terminal + 24u] = 0u; /* not quiescent */
    fs_restamp(bad, len);
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_PARTIAL_SOURCE);

    memcpy(bad, good, len);
    bad[40u + 29u + 13u] = 1u; /* dispatch 1 at_step 2 -> 1: not increasing */
    fs_restamp(bad, len);
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_STEP_ORDER);
    ASSERT_EQ(err.index, 1u);

    memcpy(bad, good, len);
    bad[40u + 21u] = 5u; /* dispatch 0 at_nanos 5, dispatch 1 at 0: time moves back */
    fs_restamp(bad, len);
    ASSERT_EQ(asx_forced_schedule_decode(bad, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TIME_ORDER);
    ASSERT_EQ(err.index, 1u);

    /* The untouched artifact still decodes (the edits above were copies). */
    ASSERT_EQ(asx_forced_schedule_decode(good, len, &limits, storage, FS_TEST_MAX_DISPATCHES, &s,
                                         &err),
              ASX_OK);
}

TEST(forced_schedule_encode_refuses_partial_and_bad_input_and_sizes_its_output) {
    asx_forced_dispatch d[2];
    asx_forced_schedule s;
    asx_forced_error err;
    uint8_t out[FS_TEST_MAX_BYTES];
    size_t need = 0u;

    memset(d, 0, sizeof(d));
    d[0].task = 0u;
    d[0].lane = ASX_FORCED_LANE_READY;
    d[0].at_step = 1u;
    d[1].task = (uint64_t)1u << 32;
    d[1].lane = ASX_FORCED_LANE_CANCEL;
    d[1].at_step = 2u;
    d[1].at_nanos = 7u;
    memset(&s, 0, sizeof(s));
    s.version = ASX_FORCED_SCHEDULE_SCHEMA_VERSION;
    s.seed = 42u;
    s.dispatches = d;
    s.dispatch_count = 2u;
    s.terminal_steps = 2u;
    s.terminal_nanos = 7u;
    s.terminal_quiescent = 1u;
    s.terminal_schedule_hash = asx_forced_schedule_certificate(d, 2u);

    ASSERT_EQ(asx_forced_schedule_encoded_size(0u), (size_t)98u);
    ASSERT_EQ(asx_forced_schedule_encoded_size(2u), (size_t)156u);

    ASSERT_EQ(asx_forced_schedule_encode(&s, out, 155u, &need, &err), ASX_E_BUFFER_TOO_SMALL);
    ASSERT_EQ(need, (size_t)156u);
    ASSERT_EQ(asx_forced_schedule_encode(&s, out, sizeof(out), &need, &err), ASX_OK);
    ASSERT_EQ(need, (size_t)156u);

    /* What it wrote decodes back to the same schedule. */
    {
        asx_forced_dispatch back[4];
        asx_forced_schedule r;
        asx_forced_decode_limits limits = fs_limits();

        ASSERT_EQ(asx_forced_schedule_decode(out, need, &limits, back, 4u, &r, &err), ASX_OK);
        ASSERT_EQ(r.dispatch_count, 2u);
        ASSERT_EQ(r.terminal_schedule_hash, s.terminal_schedule_hash);
        ASSERT_EQ(back[1].task, d[1].task);
        ASSERT_EQ(back[1].lane, (uint8_t)ASX_FORCED_LANE_CANCEL);
        ASSERT_EQ(back[1].at_nanos, (uint64_t)7u);
    }

    /* Rust's to_canonical_bytes refuses a partial receipt. */
    s.truncated = 1u;
    ASSERT_EQ(asx_forced_schedule_encode(&s, out, sizeof(out), &need, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_PARTIAL_SOURCE);
    s.truncated = 0u;
    s.terminal_quiescent = 0u;
    ASSERT_EQ(asx_forced_schedule_encode(&s, out, sizeof(out), &need, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_PARTIAL_SOURCE);
    s.terminal_quiescent = 1u;

    d[1].at_step = 1u;
    ASSERT_EQ(asx_forced_schedule_encode(&s, out, sizeof(out), &need, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_STEP_ORDER);
    d[1].at_step = 2u;
    d[1].lane = 4u;
    ASSERT_EQ(asx_forced_schedule_encode(&s, out, sizeof(out), &need, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_LANE_TAG);
    ASSERT_EQ(err.index, 1u);
    d[1].lane = ASX_FORCED_LANE_CANCEL;
    s.terminal_steps = 1u; /* last dispatch past the terminal step */
    ASSERT_EQ(asx_forced_schedule_encode(&s, out, sizeof(out), &need, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_PARTIAL_SOURCE);
    ASSERT_EQ(asx_forced_schedule_encode(NULL, out, sizeof(out), &need, &err),
              ASX_E_INVALID_ARGUMENT);
}

/* The seed-42 scene of ties_are_broken_by_the_step_value...: A, B (one
 * yield), C host-spawned in that order. The seeded picker polls B, C, A, B
 * in lab steps 1..4. */
typedef struct {
    probe a;
    probe b;
    probe c;
    asx_task_id ta;
    asx_task_id tb;
    asx_task_id tc;
    asx_region_id region;
} fs_scene;

static asx_status fs_scene_build(fs_scene *sc) {
    asx_status st = setup();

    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    order_reset();
    memset(sc, 0, sizeof(*sc));
    sc->a.name = 'A';
    sc->b.name = 'B';
    sc->b.yields = 1u;
    sc->c.name = 'C';
    if (st == ASX_OK) st = asx_region_open(&g_rt, &sc->region);
    if (st == ASX_OK) st = asx_task_spawn(sc->region, poll_probe, &sc->a, &sc->ta);
    if (st == ASX_OK) st = asx_task_spawn(sc->region, poll_probe, &sc->b, &sc->tb);
    if (st == ASX_OK) st = asx_task_spawn(sc->region, poll_probe, &sc->c, &sc->tc);
    return st;
}

/* Record the seeded run into `schedule` (dispatches in `storage`). */
static int fs_scene_record(asx_forced_schedule *schedule, asx_forced_dispatch *storage) {
    fs_scene sc;
    asx_budget budget = asx_budget_from_polls(100);

    if (fs_scene_build(&sc) != ASX_OK) return 0;
    if (asx_lab_start_forced_schedule_recording(storage, 8u) != ASX_OK) return 0;
    if (asx_scheduler_run(sc.region, &budget) != ASX_OK) return 0;
    if (strcmp(g_order, "BCAB") != 0) return 0;
    return asx_lab_finish_forced_schedule_recording(UINT64_C(0xC0FFEE), 1, schedule) == ASX_OK;
}

TEST(forced_schedule_recording_captures_rust_arena_ids_and_the_running_certificate) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;

    ASSERT_TRUE(fs_scene_record(&s, d));
    ASSERT_EQ(s.dispatch_count, 4u);
    ASSERT_EQ(s.seed, (uint64_t)42u);
    ASSERT_EQ(s.config_hash, (uint64_t)0xC0FFEEu);
    ASSERT_EQ(s.terminal_steps, (uint64_t)4u);
    ASSERT_EQ(s.terminal_nanos, (uint64_t)0u);
    ASSERT_EQ(s.terminal_quiescent, (uint8_t)1u);
    ASSERT_EQ(s.terminal_schedule_hash, asx_forced_schedule_certificate(d, 4u));
    ASSERT_EQ(s.terminal_schedule_hash, asx_scheduler_schedule_certificate());
    ASSERT_EQ(asx_scheduler_schedule_decisions(), (uint64_t)4u);
    /* B, C, A, B: arena indices 1, 2, 0, 1 (generation 0) */
    ASSERT_EQ(d[0].task, (uint64_t)1u << 32);
    ASSERT_EQ(d[1].task, (uint64_t)2u << 32);
    ASSERT_EQ(d[2].task, (uint64_t)0u);
    ASSERT_EQ(d[3].task, (uint64_t)1u << 32);
    ASSERT_EQ(d[0].at_step, (uint64_t)1u);
    ASSERT_EQ(d[3].at_step, (uint64_t)4u);
    ASSERT_EQ(d[0].lane, (uint8_t)ASX_FORCED_LANE_READY);
    ASSERT_EQ(asx_forced_schedule_validate(&s, NULL), ASX_OK);
}

/* The certificate folds at every pick whether or not anything records. */
TEST(schedule_certificate_folds_without_a_recorder) {
    fs_scene sc;
    asx_budget budget = asx_budget_from_polls(100);
    asx_forced_dispatch want[4];
    uint32_t i;

    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_scheduler_schedule_certificate(), (uint64_t)0u);
    ASSERT_EQ(asx_scheduler_schedule_decisions(), (uint64_t)0u);
    ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);

    /* B, C, A, B: arena indices 1, 2, 0, 1 on the ready lane, steps 1..4 */
    memset(want, 0, sizeof(want));
    want[0].task = (uint64_t)1u << 32;
    want[1].task = (uint64_t)2u << 32;
    want[2].task = 0u;
    want[3].task = (uint64_t)1u << 32;
    for (i = 0u; i < 4u; i++) {
        want[i].lane = ASX_FORCED_LANE_READY;
        want[i].at_step = (uint64_t)(i + 1u);
    }
    ASSERT_EQ(asx_scheduler_schedule_decisions(), (uint64_t)4u);
    ASSERT_EQ(asx_scheduler_schedule_certificate(), asx_forced_schedule_certificate(want, 4u));

    /* A new lab starts the count over. */
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_scheduler_schedule_certificate(), (uint64_t)0u);
    ASSERT_EQ(asx_scheduler_schedule_decisions(), (uint64_t)0u);
}

#ifdef ASX_DEBUG_GHOST
/* fs_scene_build on `seed`. */
static asx_status ghost_scene(uint64_t seed, fs_scene *sc) {
    asx_status st = setup();

    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(seed);
    order_reset();
    memset(sc, 0, sizeof(*sc));
    sc->a.name = 'A';
    sc->b.name = 'B';
    sc->b.yields = 1u;
    sc->c.name = 'C';
    if (st == ASX_OK) st = asx_region_open(&g_rt, &sc->region);
    if (st == ASX_OK) st = asx_task_spawn(sc->region, poll_probe, &sc->a, &sc->ta);
    if (st == ASX_OK) st = asx_task_spawn(sc->region, poll_probe, &sc->b, &sc->tb);
    if (st == ASX_OK) st = asx_task_spawn(sc->region, poll_probe, &sc->c, &sc->tc);
    return st;
}

/* The ghost determinism monitor takes the schedule certificate at every pick
 * (bd-flkg). Sealed after one run, it finds a rerun of the same program on
 * the same seed repeating the picks and one that picks another order
 * drifting; the runtime's reset between the runs keeps the reference. */
TEST(the_ghost_determinism_monitor_sees_every_pick_and_a_rerun_repeats_them) {
    fs_scene sc;
    asx_budget budget = asx_budget_from_polls(100);
    uint64_t seed;
    int reordered = 0;

    ASSERT_EQ(ghost_scene(42u, &sc), ASX_OK);
    asx_ghost_determinism_reset();
    ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
    ASSERT_EQ(asx_scheduler_schedule_decisions(), (uint64_t)4u);
    ASSERT_EQ(asx_ghost_determinism_event_count(), 4u);
    asx_ghost_determinism_seal();

    /* Same program, same seed: the new runtime cleared the run, not the
     * reference, and the picks repeat. */
    ASSERT_EQ(ghost_scene(42u, &sc), ASX_OK);
    ASSERT_EQ(asx_ghost_determinism_event_count(), 0u);
    ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_OK);
    ASSERT_EQ(asx_ghost_determinism_event_count(), 4u);
    ASSERT_EQ(asx_ghost_determinism_check(), 0u);
    ASSERT_EQ(asx_ghost_violation_count(), 0u);

    /* A seed that picks another order does not. */
    for (seed = 1u; seed <= 32u && !reordered; seed++) {
        ASSERT_EQ(ghost_scene(seed, &sc), ASX_OK);
        ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_OK);
        if (strcmp(g_order, "BCAB") != 0) {
            reordered = 1;
            ASSERT_TRUE(asx_ghost_determinism_check() > 0u);
        } else {
            ASSERT_EQ(asx_ghost_determinism_check(), 0u);
        }
    }
    ASSERT_TRUE(reordered);
    asx_ghost_determinism_reset();
}
#endif

/* Rust's arena hands a completed task's index to the next task with the
 * generation advanced; the recorded id carries that, not the C slot's. */
TEST(forced_schedule_recording_follows_rust_arena_index_reuse) {
    asx_forced_dispatch d[4];
    asx_forced_schedule s;
    asx_region_id r;
    asx_task_id t;
    asx_budget budget = asx_budget_from_polls(20);
    probe a = {'a', 0u, 0};
    probe b = {'b', 0u, 0};

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(42u), ASX_OK);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 4u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &a, &t), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &b, &t), ASX_OK);
    budget = asx_budget_from_polls(20);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_lab_finish_forced_schedule_recording(0u, 1, &s), ASX_OK);
    ASSERT_EQ(s.dispatch_count, 2u);
    ASSERT_EQ(d[0].task, (uint64_t)0u);      /* index 0, generation 0 */
    ASSERT_EQ(d[1].task, (uint64_t)0u | 1u); /* index 0 again, generation 1 */
}

TEST(forced_schedule_recording_refuses_misuse_and_overflow) {
    asx_forced_dispatch d[2];
    asx_forced_schedule s;
    fs_scene sc;
    asx_budget budget = asx_budget_from_polls(100);

    /* off without the lab dispatcher */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 2u), ASX_E_INVALID_STATE);

    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(NULL, 2u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 0u), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(asx_lab_finish_forced_schedule_recording(0u, 1, &s), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 2u), ASX_OK);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 2u), ASX_E_INVALID_STATE);

    /* four dispatches into two entries: no partial receipt */
    ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_OK);
    ASSERT_EQ(asx_lab_finish_forced_schedule_recording(0u, 1, &s), ASX_E_RESOURCE_EXHAUSTED);
    ASSERT_EQ(asx_lab_finish_forced_schedule_recording(0u, 1, &s), ASX_E_INVALID_STATE);

    /* recording must start before the first step */
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    budget = asx_budget_from_polls(1);
    ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 2u), ASX_E_INVALID_STATE);
}

TEST(forced_schedule_replays_the_recorded_run_exactly) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    fs_scene sc;

    ASSERT_TRUE(fs_scene_record(&s, d));
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    memset(&rep, 0, sizeof(rep));
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
    ASSERT_EQ(rep.dispatches, 4u);
    ASSERT_EQ(rep.steps, (uint64_t)4u);
    ASSERT_EQ(rep.schedule_hash, s.terminal_schedule_hash);
}

/* The schedule, not the seeded picker, chooses: the same seed draws "BCAB",
 * a schedule that swaps the first and third dispatches runs "ACBB" (its
 * own certificate, recomputed) and succeeds. */
TEST(forced_schedule_overrides_the_seeded_pick) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    fs_scene sc;
    uint64_t t0;

    ASSERT_TRUE(fs_scene_record(&s, d));
    t0 = d[0].task;
    d[0].task = d[2].task; /* A first */
    d[2].task = t0;        /* B's first poll third */
    s.terminal_schedule_hash = asx_forced_schedule_certificate(d, 4u);

    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    memset(&rep, 0, sizeof(rep));
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "ACBB"), 0);
    ASSERT_EQ(rep.dispatches, 4u);
    ASSERT_EQ(rep.schedule_hash, s.terminal_schedule_hash);
}

static asx_status fs_resolve_table(void *ctx, uint64_t compact, asx_task_id *out) {
    const fs_scene *sc = (const fs_scene *)ctx;

    if (compact == ((uint64_t)40u << 32))
        *out = sc->tb;
    else if (compact == ((uint64_t)41u << 32))
        *out = sc->tc;
    else if (compact == ((uint64_t)42u << 32))
        *out = sc->ta;
    else
        return ASX_E_NOT_FOUND;
    return ASX_OK;
}

/* Task ids a schedule names need not be this run's arena slots: a resolver
 * maps them (the harness maps Rust ids to tasks by name). */
TEST(forced_schedule_resolver_maps_recorded_task_ids) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    asx_forced_run_config cfg;
    fs_scene sc;

    ASSERT_TRUE(fs_scene_record(&s, d));
    /* B, C, A, B under invented ids 40 (B), 41 (C), 42 (A). The receipt's
     * hash stays the recorded run's: the lab folds its own arena ids, not
     * the ids the schedule names the tasks by. */
    d[0].task = (uint64_t)40u << 32;
    d[1].task = (uint64_t)41u << 32;
    d[2].task = (uint64_t)42u << 32;
    d[3].task = (uint64_t)40u << 32;

    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolve = fs_resolve_table;
    cfg.resolve_ctx = &sc;
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, &cfg, &rep, &err), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);

    /* An id neither the resolver nor the lab's arena knows is a task that
     * is not runnable. */
    d[1].task = (uint64_t)99u << 32;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    cfg.resolve_ctx = &sc;
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, &cfg, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TASK_UNAVAILABLE);
    ASSERT_EQ(err.index, 1u);
    ASSERT_EQ(err.step, (uint64_t)2u);
}

/* A resolver names only the tasks its caller knows. The ids it declines
 * resolve through the lab's arena shadow: here it knows C alone, and A and
 * B (arena indices 0 and 1) are found by their Rust ids. */
typedef struct {
    const fs_scene *scene;
    uint32_t declined;
} fs_partial_ctx;

static asx_status fs_resolve_only_c(void *ctx, uint64_t compact, asx_task_id *out) {
    fs_partial_ctx *p = (fs_partial_ctx *)ctx;

    if (compact == ((uint64_t)2u << 32)) {
        *out = p->scene->tc;
        return ASX_OK;
    }
    p->declined++;
    return ASX_E_NOT_FOUND;
}

TEST(forced_schedule_resolver_declines_fall_back_to_the_arena_shadow) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    asx_forced_run_config cfg;
    fs_partial_ctx pc;
    fs_scene sc;

    ASSERT_TRUE(fs_scene_record(&s, d));
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    pc.scene = &sc;
    pc.declined = 0u;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolve = fs_resolve_only_c;
    cfg.resolve_ctx = &pc;
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, &cfg, &rep, &err), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
    /* B, A, B were declined by the resolver and found by arena id */
    ASSERT_EQ(pc.declined, 3u);
    ASSERT_EQ(rep.schedule_hash, s.terminal_schedule_hash);
}

/* A supervisor's generation is a task the runtime creates: no scenario
 * names it. Replaying a recorded supervised run with a resolver that knows
 * only the owner resolves the controller and the generation by arena id. */
typedef struct {
    asx_region_id region; /* the owner's: the supervisor is spawned here */
    asx_supervisor_config cfg;
    asx_child_spec specs[1];
    asx_supervisor_handle h;
    asx_supervisor_report report;
    asx_status spawn_st;
    int spawned;
    int joined;
} sup_owner;

typedef struct {
    sup_owner owner;
    asx_region_id root;
    asx_region_id owner_region;
    asx_task_id owner_task;
    uint32_t declined;
} sup_scene;

static asx_status sup_child_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

static asx_status sup_child_start(void *user_data, const asx_supervisor_generation *gen,
                                  asx_task_poll_fn *out_poll, void **out_data) {
    (void)user_data;
    (void)gen;
    *out_poll = sup_child_poll;
    *out_data = NULL;
    return ASX_OK;
}

static asx_status sup_owner_poll(void *user_data, asx_task_id self) {
    sup_owner *o = (sup_owner *)user_data;
    asx_status st;

    if (!o->spawned) {
        o->spawned = 1;
        o->spawn_st = asx_supervisor_spawn(&o->h, o->region, &o->cfg, o->specs, 1u);
        if (o->spawn_st != ASX_OK) return ASX_OK;
    }
    st = asx_supervisor_join(o->h, self, &o->report);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    o->joined = 1;
    return ASX_OK;
}

/* root, the owner's region, the supervisor's, one generation's: 4 regions,
 * the most the smallest resource class has. */
static asx_status sup_scene_build(sup_scene *sc) {
    asx_status st = setup();

    memset(sc, 0, sizeof(*sc));
    if (st == ASX_OK) st = asx_scheduler_use_lab_dispatch(42u);
    asx_supervisor_reset();
    if (st == ASX_OK) st = asx_region_open(&g_rt, &sc->root);
    if (st == ASX_OK) st = asx_region_open_child(sc->root, &sc->owner_region);
    sc->owner.region = sc->owner_region;
    asx_supervisor_config_init(&sc->owner.cfg, "sup", 0u, 60000000000u);
    sc->owner.cfg.backoff.kind = ASX_RESTART_BACKOFF_NONE;
    asx_child_spec_init(&sc->owner.specs[0], "worker", ASX_CHILD_TRANSIENT, sup_child_start, NULL);
    if (st == ASX_OK) {
        st = asx_task_spawn(sc->owner_region, sup_owner_poll, &sc->owner, &sc->owner_task);
    }
    return st;
}

static asx_status sup_resolve_owner_only(void *ctx, uint64_t compact, asx_task_id *out) {
    sup_scene *sc = (sup_scene *)ctx;

    if (compact == 0u) { /* the owner is the first task: arena index 0, generation 0 */
        *out = sc->owner_task;
        return ASX_OK;
    }
    sc->declined++;
    return ASX_E_NOT_FOUND;
}

TEST(forced_schedule_replays_a_supervisor_generation_by_arena_id) {
    asx_forced_dispatch d[32];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    asx_forced_run_config cfg;
    asx_budget budget = asx_budget_from_polls(1000);
    sup_scene sc;
    uint32_t i;
    uint32_t others = 0u;

    if ((unsigned)ASX_MAX_REGIONS < 4u || (unsigned)ASX_MAX_TASKS < 4u) return;

    /* record the supervised run on the seeded picker */
    ASSERT_EQ(sup_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_start_forced_schedule_recording(d, 32u), ASX_OK);
    ASSERT_EQ(asx_scheduler_run(sc.root, &budget), ASX_OK);
    ASSERT_TRUE(sc.owner.joined);
    ASSERT_EQ(asx_lab_finish_forced_schedule_recording(0u, 1, &s), ASX_OK);
    for (i = 0u; i < s.dispatch_count; i++) {
        if (s.dispatches[i].task != 0u) others++;
    }
    /* the controller and the generation are dispatched, and are not the owner */
    ASSERT_TRUE(others >= 2u);

    /* replay it with a resolver that knows the owner only */
    ASSERT_EQ(sup_scene_build(&sc), ASX_OK);
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolve = sup_resolve_owner_only;
    cfg.resolve_ctx = &sc;
    memset(&rep, 0, sizeof(rep));
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.root, &cfg, &rep, &err), ASX_OK);
    ASSERT_TRUE(sc.owner.joined);
    ASSERT_EQ(rep.dispatches, s.dispatch_count);
    ASSERT_EQ(rep.schedule_hash, s.terminal_schedule_hash);
    ASSERT_TRUE(sc.declined >= others);
}

/* Each divergence ends the run at its own step, with that step's task left
 * unpolled (g_order holds only what ran before it). */
TEST(forced_schedule_divergences_report_their_step_and_leave_the_task_unpolled) {
    asx_forced_dispatch d[8];
    asx_forced_dispatch keep[8];
    asx_forced_schedule s;
    asx_forced_schedule base;
    asx_forced_error err;
    asx_forced_report rep;
    fs_scene sc;
    asx_forced_run_config cfg;
    uint32_t i;

    ASSERT_TRUE(fs_scene_record(&base, keep));
    ASSERT_EQ(base.dispatch_count, 4u);

    /* the recorded task is not runnable at step 2 */
    memcpy(d, keep, sizeof(keep));
    s = base;
    s.dispatches = d;
    d[1].task = (uint64_t)(ASX_MAX_TASKS - 1u) << 32; /* never spawned */
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TASK_UNAVAILABLE);
    ASSERT_EQ(err.index, 1u);
    ASSERT_EQ(err.step, (uint64_t)2u);
    ASSERT_EQ(strcmp(g_order, "B"), 0);

    /* runnable, but in the ready lane, not the cancel lane */
    memcpy(d, keep, sizeof(keep));
    d[0].lane = ASX_FORCED_LANE_CANCEL;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_LANE_MISMATCH);
    ASSERT_EQ(err.index, 0u);
    ASSERT_EQ(err.step, (uint64_t)1u);
    ASSERT_EQ(g_order_n, 0u);

    /* the timed and stolen lanes are not modelled by asx's lab */
    memcpy(d, keep, sizeof(keep));
    d[0].lane = ASX_FORCED_LANE_TIMED;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_LANE_UNSUPPORTED);
    ASSERT_EQ(g_order_n, 0u);

    /* one worker only */
    memcpy(d, keep, sizeof(keep));
    d[2].worker = 1u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_WORKER_MISMATCH);
    ASSERT_EQ(err.index, 2u);
    ASSERT_EQ(err.step, (uint64_t)3u);

    /* work is runnable before the first recorded step */
    memcpy(d, keep, sizeof(keep));
    for (i = 0u; i < 4u; i++) d[i].at_step += 2u;
    s.terminal_steps = base.terminal_steps + 2u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_EARLY_RUNNABLE);
    ASSERT_EQ(err.index, 0u);
    ASSERT_EQ(err.step, (uint64_t)1u);
    ASSERT_EQ(err.expected, (uint64_t)3u);
    ASSERT_EQ(g_order_n, 0u);
    s.terminal_steps = base.terminal_steps;

    /* the recorded virtual time is not the clock's */
    memcpy(d, keep, sizeof(keep));
    for (i = 0u; i < 4u; i++) d[i].at_nanos = 99u;
    s.terminal_nanos = 99u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TIME_MISMATCH);
    ASSERT_EQ(err.expected, (uint64_t)99u);
    ASSERT_EQ(err.actual, (uint64_t)0u);
    s.terminal_nanos = base.terminal_nanos;

    /* work left after the last recorded dispatch */
    memcpy(d, keep, sizeof(keep));
    s.dispatch_count = 3u;
    s.terminal_steps = 3u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_SCHEDULE_EXHAUSTED);
    ASSERT_EQ(err.index, 3u);          /* consumed */
    ASSERT_EQ(err.step, (uint64_t)3u); /* the receipt's terminal step */
    ASSERT_EQ(strcmp(g_order, "BCA"), 0);
    s.dispatch_count = 4u;
    s.terminal_steps = base.terminal_steps;

    /* exact, but the run ends at another step than the receipt says */
    s.terminal_steps = 9u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TERMINAL_MISMATCH);
    ASSERT_EQ(err.expected, (uint64_t)9u);
    ASSERT_EQ(err.actual, (uint64_t)4u);
    s.terminal_steps = base.terminal_steps;

    /* exact, but the receipt's schedule hash is not the run's */
    s.terminal_schedule_hash ^= 1u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_CERTIFICATE_MISMATCH);
    ASSERT_EQ(err.expected, base.terminal_schedule_hash ^ 1u);
    ASSERT_EQ(err.actual, base.terminal_schedule_hash);
    s.terminal_schedule_hash = base.terminal_schedule_hash;

    /* the caller's step limit */
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_steps = 2u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, &cfg, &rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_WORK_LIMIT);
    ASSERT_EQ(strcmp(g_order, "BC"), 0);
}

/* A runner that drives the scheduler its own way (the conformance
 * interpreter runs to idle, then with timer auto-advance): begin, any
 * number of scheduler runs, end. */
TEST(forced_schedule_begin_end_replays_a_runner_driven_run) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    asx_budget budget;
    fs_scene sc;

    ASSERT_TRUE(fs_scene_record(&s, d));
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    memset(&rep, 0, sizeof(rep));
    ASSERT_EQ(asx_lab_forced_schedule_begin(&s, NULL, &err), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run_until_idle(sc.region, &budget), ASX_OK);
    ASSERT_EQ(strcmp(g_order, "BCAB"), 0);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(sc.region, &budget), ASX_OK);
    ASSERT_EQ(asx_lab_forced_schedule_end(&rep, &err), ASX_OK);
    ASSERT_EQ(rep.dispatches, 4u);
    ASSERT_EQ(rep.steps, (uint64_t)4u);
    ASSERT_EQ(rep.schedule_hash, s.terminal_schedule_hash);

    /* the schedule is off again */
    ASSERT_EQ(asx_lab_forced_schedule_end(&rep, &err), ASX_E_INVALID_STATE);
}

TEST(forced_schedule_begin_end_reports_the_divergence_that_stopped_the_run) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    asx_budget budget;
    fs_scene sc;
    asx_status st;

    ASSERT_TRUE(fs_scene_record(&s, d));
    d[1].task = (uint64_t)(ASX_MAX_TASKS - 1u) << 32; /* never spawned */
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_forced_schedule_begin(&s, NULL, &err), ASX_OK);
    budget = asx_budget_from_polls(100);
    st = asx_scheduler_run_until_idle(sc.region, &budget);
    ASSERT_TRUE(st != ASX_OK);
    ASSERT_EQ(strcmp(g_order, "B"), 0);
    ASSERT_EQ(asx_lab_forced_schedule_end(&rep, &err), ASX_E_REPLAY_MISMATCH);
    ASSERT_EQ(err.kind, ASX_FORCED_E_TASK_UNAVAILABLE);
    ASSERT_EQ(err.index, 1u);
    ASSERT_EQ(err.step, (uint64_t)2u);
}

TEST(forced_schedule_begin_refuses_a_second_schedule_and_another_seed) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    fs_scene sc;

    ASSERT_TRUE(fs_scene_record(&s, d));
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_forced_schedule_end(&rep, &err), ASX_E_INVALID_STATE); /* none installed */
    ASSERT_EQ(asx_lab_forced_schedule_begin(&s, NULL, &err), ASX_OK);
    ASSERT_EQ(asx_lab_forced_schedule_begin(&s, NULL, &err), ASX_E_INVALID_STATE);
    ASSERT_EQ(err.kind, ASX_FORCED_E_NOT_PRISTINE);

    /* a fresh lab with another seed refuses a schedule of seed 42 */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    ASSERT_EQ(asx_lab_forced_schedule_begin(&s, NULL, &err), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_SEED_MISMATCH);
}

TEST(forced_error_kinds_have_distinct_names) {
    uint32_t a;
    uint32_t b;

    for (a = (uint32_t)ASX_FORCED_OK; a <= (uint32_t)ASX_FORCED_E_CERTIFICATE_MISMATCH; a++) {
        const char *na = asx_forced_error_kind_name((asx_forced_error_kind)a);
        ASSERT_TRUE(na != NULL && strcmp(na, "unknown") != 0);
        for (b = a + 1u; b <= (uint32_t)ASX_FORCED_E_CERTIFICATE_MISMATCH; b++) {
            const char *nb = asx_forced_error_kind_name((asx_forced_error_kind)b);
            ASSERT_TRUE(strcmp(na, nb) != 0);
        }
    }
    ASSERT_EQ(strcmp(asx_forced_error_kind_name(ASX_FORCED_E_TASK_UNAVAILABLE), "task_unavailable"),
              0);
}

TEST(forced_schedule_refuses_a_run_that_is_not_pristine_or_has_another_seed) {
    asx_forced_dispatch d[8];
    asx_forced_schedule s;
    asx_forced_error err;
    asx_forced_report rep;
    fs_scene sc;

    ASSERT_TRUE(fs_scene_record(&s, d));

    /* another seed: the schedule is not for this run, nothing polls */
    s.seed = 7u;
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_SEED_MISMATCH);
    ASSERT_EQ(g_order_n, 0u);
    s.seed = 42u;

    /* a second replay on a run that already stepped */
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_INVALID_STATE);
    ASSERT_EQ(err.kind, ASX_FORCED_E_NOT_PRISTINE);

    /* the seeded dispatcher is off */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, ASX_INVALID_ID, NULL, &rep, &err),
              ASX_E_INVALID_STATE);
    ASSERT_EQ(err.kind, ASX_FORCED_E_NOT_PRISTINE);

    /* a malformed schedule is refused before anything runs */
    ASSERT_EQ(asx_lab_run_forced_schedule(NULL, ASX_INVALID_ID, NULL, &rep, &err),
              ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(fs_scene_build(&sc), ASX_OK);
    s.truncated = 1u;
    ASSERT_EQ(asx_lab_run_forced_schedule(&s, sc.region, NULL, &rep, &err), ASX_E_INVALID_ARGUMENT);
    ASSERT_EQ(err.kind, ASX_FORCED_E_PARTIAL_SOURCE);
    ASSERT_EQ(g_order_n, 0u);
}

/* ---- Stuck tasks and futurelocks (bd-9kll.9.6) ---- */

/* Waits on a channel nothing sends on. */
typedef struct {
    asx_region_id region;
    asx_channel_id rx;
    asx_cx cx;
    int cx_ready;
    uint64_t got;
} chan_waiter;

static asx_status poll_chan_waiter(void *ud, asx_task_id self) {
    chan_waiter *w = (chan_waiter *)ud;
    if (!w->cx_ready) {
        asx_status st = asx_cx_init(&w->cx, w->region, self, ASX_CAP_NONE);
        if (st != ASX_OK) return st;
        w->cx_ready = 1;
    }
    return asx_channel_recv(w->rx, &w->cx, &w->got);
}

/* Parks and is never woken. */
static asx_status poll_parks_forever(void *ud, asx_task_id self) {
    (void)ud;
    return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
}

/* Joins `target`. */
static asx_status poll_joiner(void *ud, asx_task_id self) {
    asx_outcome out;
    return asx_task_join_poll(self, *(asx_task_id *)ud, &out);
}

/* Reserves one obligation at its first poll, then parks for good (`park`)
 * or yields on every poll. */
typedef struct {
    asx_region_id region;
    asx_obligation_id id;
    int reserved;
    int park;
} holder_state;

static asx_status poll_holder(void *ud, asx_task_id self) {
    holder_state *h = (holder_state *)ud;
    if (!h->reserved) {
        asx_status st = asx_obligation_reserve(h->region, &h->id);
        if (st != ASX_OK) return st;
        h->reserved = 1;
    }
    if (h->park) return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    return ASX_E_PENDING;
}

/* Two tasks wait, each on a channel the other would send on: a deadlock.
 * The run ends ASX_E_WOULD_BLOCK, as Rust's StuckBailout, and both tasks are
 * named, in arena order, with their region. */
TEST(a_run_that_would_block_names_both_tasks_of_a_channel_deadlock) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_channel_id ca;
    asx_channel_id cb;
    asx_budget budget;
    chan_waiter a;
    chan_waiter b;
    asx_stuck_task st[4];

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_channel_create(r, 2u, &ca), ASX_OK);
    ASSERT_EQ(asx_channel_create(r, 2u, &cb), ASX_OK);
    a.region = r;
    a.rx = ca;
    b.region = r;
    b.rx = cb;
    ASSERT_EQ(asx_task_spawn(r, poll_chan_waiter, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_chan_waiter, &b, &tb), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 4u), 2u);
    ASSERT_TRUE(asx_handle_index(st[0].task) == asx_handle_index(ta));
    ASSERT_TRUE(asx_handle_index(st[1].task) == asx_handle_index(tb));
    ASSERT_TRUE(asx_handle_index(st[0].region) == asx_handle_index(r));
    ASSERT_EQ((int)st[0].state, (int)ASX_TASK_RUNNING);
    ASSERT_EQ((int)st[0].wait, (int)ASX_STUCK_PARKED);
    ASSERT_EQ((int)st[1].wait, (int)ASX_STUCK_PARKED);
    ASSERT_EQ(st[0].held, 0u);
    ASSERT_EQ((int)st[0].cancel_requested, 0);
    /* The stuck steps went by while they waited. */
    ASSERT_TRUE(st[0].idle_steps > 100u);
    /* A short buffer still counts them all. */
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 1u), 2u);
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, NULL, 0u), 2u);
}

/* A task joining another that parks for good: both stuck, the joiner on
 * the task it awaits. */
TEST(a_stuck_joiner_names_the_task_it_awaits) {
    asx_region_id r;
    asx_task_id target;
    asx_task_id joiner;
    asx_budget budget;
    asx_stuck_task st[4];

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(3u), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_parks_forever, NULL, &target), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_joiner, &target, &joiner), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 4u), 2u);
    ASSERT_EQ((int)st[0].wait, (int)ASX_STUCK_PARKED);
    ASSERT_TRUE(asx_handle_index(st[0].task) == asx_handle_index(target));
    ASSERT_EQ((int)st[1].wait, (int)ASX_STUCK_JOIN);
    ASSERT_TRUE(asx_handle_index(st[1].task) == asx_handle_index(joiner));
    ASSERT_TRUE(asx_handle_index(st[1].wait_task) == asx_handle_index(target));
}

/* The planted futurelock: a task holds an obligation and parks for good
 * while another keeps the lab stepping. With the threshold Rust's own test
 * uses (3) the holder is named, with its obligation and kind; with Rust's
 * default (10000) the same run reports nothing, because the 1000 stuck steps
 * that end it never reach it; a threshold of 0 switches the check off. */
TEST(a_parked_obligation_holder_is_a_futurelock_past_the_threshold) {
    asx_region_id r;
    asx_task_id th;
    asx_task_id tk;
    asx_budget budget;
    holder_state h;
    probe k;
    asx_futurelock fl[2];
    asx_stuck_task st[2];

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    memset(&h, 0, sizeof(h));
    memset(&k, 0, sizeof(k));
    order_reset();
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    h.region = r;
    h.park = 1;
    k.name = 'k';
    k.yields = 12u;
    ASSERT_EQ(asx_task_spawn(r, poll_holder, &h, &th), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_probe, &k, &tk), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);

    ASSERT_EQ(asx_lab_futurelocks(3u, fl, 2u), 1u);
    ASSERT_TRUE(asx_handle_index(fl[0].task) == asx_handle_index(th));
    ASSERT_TRUE(asx_handle_index(fl[0].region) == asx_handle_index(r));
    ASSERT_TRUE(fl[0].idle_steps > 3u);
    ASSERT_EQ(fl[0].held_count, 1u);
    ASSERT_TRUE(asx_handle_index(fl[0].held[0]) == asx_handle_index(h.id));
    ASSERT_EQ((int)fl[0].held_kind[0], (int)ASX_OBLIGATION_KIND_GENERIC);
    ASSERT_EQ(asx_lab_futurelocks(10000u, fl, 2u), 0u);
    ASSERT_EQ(asx_lab_futurelocks(0u, fl, 2u), 0u);

    /* The same task is the stuck one, holding the obligation. */
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 2u), 1u);
    ASSERT_TRUE(asx_handle_index(st[0].task) == asx_handle_index(th));
    ASSERT_EQ(st[0].held, 1u);
}

/* Negative: a task that holds an obligation and is polled at every step is
 * not futurelocked, whatever the threshold; the one that parked is. */
TEST(a_task_polled_every_step_is_not_a_futurelock) {
    asx_region_id r;
    asx_task_id parked;
    asx_task_id spinner;
    asx_budget budget;
    holder_state hp;
    holder_state hs;
    asx_futurelock fl[4];

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(11u), ASX_OK);
    memset(&hp, 0, sizeof(hp));
    memset(&hs, 0, sizeof(hs));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    hp.region = r;
    hp.park = 1;
    hs.region = r;
    ASSERT_EQ(asx_task_spawn(r, poll_holder, &hp, &parked), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_holder, &hs, &spinner), ASX_OK);

    budget = asx_budget_from_polls(40u);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);

    ASSERT_EQ(asx_lab_futurelocks(3u, fl, 4u), 1u);
    ASSERT_TRUE(asx_handle_index(fl[0].task) == asx_handle_index(parked));
    ASSERT_EQ(asx_lab_futurelocks(1000u, fl, 4u), 0u);
}

/* Negative: a sleeper a timer wakes is not stuck, and the run that ends
 * with nothing left reports no stuck task and no futurelock. */
TEST(a_sleeper_that_wakes_is_neither_stuck_nor_futurelocked) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    sleeper s;
    asx_stuck_task st[2];
    asx_futurelock fl[2];

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_sleeper, &s, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    /* Before the timer fires the sleeper waits, but a timer will wake it. */
    ASSERT_EQ(asx_scheduler_run_until_idle(r, &budget), ASX_E_PENDING);
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 2u), 0u);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 2u), 0u);
    ASSERT_EQ(asx_lab_futurelocks(1u, fl, 2u), 0u);
}

/* Outside lab dispatch there are no lab steps: the futurelock check reports
 * nothing, and the stuck query is over the region's parked tasks. */
TEST(the_futurelock_check_needs_lab_dispatch_and_the_sweep_lists_parked_tasks) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    asx_stuck_task st[2];
    asx_futurelock fl[2];

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_parks_forever, NULL, &t), ASX_OK);
    budget = asx_budget_from_polls(100);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    ASSERT_EQ(asx_lab_futurelocks(1u, fl, 2u), 0u);
    ASSERT_EQ(asx_scheduler_stuck_tasks(r, st, 2u), 1u);
    ASSERT_TRUE(asx_handle_index(st[0].task) == asx_handle_index(t));
    ASSERT_EQ(st[0].idle_steps, (uint64_t)0u);
}

/* ---- The per-step futurelock check (bd-9kll.9.6) ---- */

#define FL_CAP 64u

typedef struct {
    uint32_t n;
    uint64_t task[FL_CAP];
    uint64_t idle[FL_CAP];
    uint64_t region[FL_CAP];
    uint32_t held_count[FL_CAP];
    uint64_t held0[FL_CAP];
    int held_kind0[FL_CAP];
} fl_capture;

static fl_capture g_fl;

static void fl_observe(void *ctx, const asx_trace_event *ev, const asx_trace_payload *payload) {
    (void)ctx;
    if (ev->kind != ASX_TRACE_LAB_FUTURELOCK || g_fl.n >= FL_CAP) return;
    g_fl.task[g_fl.n] = ev->entity_id;
    g_fl.idle[g_fl.n] = ev->aux;
    if (payload->futurelock != NULL) {
        g_fl.region[g_fl.n] = payload->futurelock->region;
        g_fl.held_count[g_fl.n] = payload->futurelock->held_count;
        g_fl.held0[g_fl.n] = payload->futurelock->held[0];
        g_fl.held_kind0[g_fl.n] = (int)payload->futurelock->held_kind[0];
    }
    g_fl.n++;
}

static void fl_begin(void) {
    memset(&g_fl, 0, sizeof(g_fl));
    asx_trace_reset(&g_rt);
    asx_trace_set_observer(&g_rt, fl_observe, NULL);
}

/* Reserves one obligation, parks until `released`, then commits it. */
typedef struct {
    asx_region_id region;
    asx_obligation_id id;
    int reserved;
    int released;
} rel_holder;

static asx_status poll_rel_holder(void *ud, asx_task_id self) {
    rel_holder *h = (rel_holder *)ud;
    if (!h->reserved) {
        asx_status st = asx_obligation_reserve(h->region, &h->id);
        if (st != ASX_OK) return st;
        h->reserved = 1;
    }
    if (!h->released) return asx_task_park(self) == ASX_OK ? ASX_E_PENDING : ASX_E_INVALID_STATE;
    return asx_obligation_commit(h->id);
}

/* Yields `yields` times, then releases and wakes its target. */
typedef struct {
    uint32_t yields;
    asx_task_id target;
    rel_holder *holder;
} releaser;

static asx_status poll_releaser(void *ud, asx_task_id self) {
    releaser *r = (releaser *)ud;
    (void)self;
    if (r->yields > 0u) {
        r->yields--;
        return ASX_E_PENDING;
    }
    r->holder->released = 1;
    return asx_task_wake(r->target);
}

/* Rust's check_futurelocks (LR:4508) runs at every step and emits a
 * FuturelockDetected for every task still idle past the threshold, so a
 * holder that stays idle is reported again at each following step, its idle
 * count one higher each time. The first report is at threshold + 1; once the
 * holder is polled and completes there are no more. */
TEST(the_futurelock_check_reports_a_holder_at_every_step_it_stays_idle) {
    asx_region_id r;
    asx_task_id th;
    asx_task_id tw;
    asx_budget budget;
    rel_holder h;
    releaser w;
    uint32_t i;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    ASSERT_EQ(asx_lab_set_futurelock_check(3u), ASX_OK);
    fl_begin();
    memset(&h, 0, sizeof(h));
    memset(&w, 0, sizeof(w));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    h.region = r;
    ASSERT_EQ(asx_task_spawn(r, poll_rel_holder, &h, &th), ASX_OK);
    w.yields = 10u;
    w.holder = &h;
    ASSERT_EQ(asx_task_spawn(r, poll_releaser, &w, &tw), ASX_OK);
    w.target = th;

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_OK);
    asx_trace_set_observer(&g_rt, NULL, NULL);

    ASSERT_TRUE(g_fl.n >= 3u);
    ASSERT_EQ(asx_trace_kind_total(&g_rt, ASX_TRACE_LAB_FUTURELOCK), (uint64_t)g_fl.n);
    ASSERT_EQ(g_fl.idle[0], (uint64_t)4u); /* threshold 3, so the first idle count over it */
    for (i = 0; i < g_fl.n; i++) {
        ASSERT_TRUE(asx_handle_index((asx_task_id)g_fl.task[i]) == asx_handle_index(th));
        ASSERT_TRUE(asx_handle_index((asx_region_id)g_fl.region[i]) == asx_handle_index(r));
        ASSERT_EQ(g_fl.held_count[i], 1u);
        ASSERT_TRUE(asx_handle_index((asx_obligation_id)g_fl.held0[i]) == asx_handle_index(h.id));
        ASSERT_EQ(g_fl.held_kind0[i], (int)ASX_OBLIGATION_KIND_GENERIC);
        ASSERT_EQ(g_fl.idle[i], (uint64_t)(4u + i)); /* one more at each following step */
    }
    /* The holder committed, so no obligation is left held. */
    ASSERT_EQ(asx_lab_futurelocks(3u, NULL, 0u), 0u);
}

/* Two holders that park for good are both reported, and in Rust's TaskId
 * order (the arena index each was given): the query and the events come out
 * first task first, whichever of them was polled last. */
TEST(the_futurelock_check_reports_holders_in_task_id_order) {
    asx_region_id r;
    asx_task_id ta;
    asx_task_id tb;
    asx_budget budget;
    rel_holder a;
    rel_holder b;
    asx_futurelock fl[4];
    uint32_t i;
    int saw_a = 0;
    int saw_b = 0;

    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    ASSERT_EQ(asx_lab_set_futurelock_check(2u), ASX_OK);
    fl_begin();
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    a.region = r;
    b.region = r;
    ASSERT_EQ(asx_task_spawn(r, poll_rel_holder, &a, &ta), ASX_OK);
    ASSERT_EQ(asx_task_spawn(r, poll_rel_holder, &b, &tb), ASX_OK);

    budget = asx_budget_infinite();
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
    asx_trace_set_observer(&g_rt, NULL, NULL);

    ASSERT_EQ(asx_lab_futurelocks(2u, fl, 4u), 2u);
    ASSERT_TRUE(asx_handle_index(fl[0].task) == asx_handle_index(ta));
    ASSERT_TRUE(asx_handle_index(fl[1].task) == asx_handle_index(tb));
    ASSERT_EQ(fl[0].held_count, 1u);
    ASSERT_EQ(fl[1].held_count, 1u);

    ASSERT_TRUE(g_fl.n >= 4u);
    for (i = 0; i < g_fl.n; i++) {
        saw_a |= asx_handle_index((asx_task_id)g_fl.task[i]) == asx_handle_index(ta);
        saw_b |= asx_handle_index((asx_task_id)g_fl.task[i]) == asx_handle_index(tb);
    }
    ASSERT_TRUE(saw_a && saw_b);
    /* The run stays stuck for 1000 steps with both reported at each, far
     * more than the capture holds. */
    ASSERT_TRUE(asx_trace_kind_total(&g_rt, ASX_TRACE_LAB_FUTURELOCK) > (uint64_t)FL_CAP);
}

/* Negative: a holder polled at every step never passes the threshold. The
 * check is also off unless asked for, and a threshold the run never
 * reaches reports nothing. */
TEST(the_futurelock_check_is_silent_for_a_polled_holder_and_when_off) {
    asx_region_id r;
    asx_task_id t;
    asx_budget budget;
    holder_state hs;

    ASSERT_EQ(setup(), ASX_OK);
    /* Lab dispatch is not active yet: nothing to set. */
    ASSERT_EQ(asx_lab_set_futurelock_check(3u), ASX_E_INVALID_STATE);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(11u), ASX_OK);
    ASSERT_EQ(asx_lab_set_futurelock_check(3u), ASX_OK);
    fl_begin();
    memset(&hs, 0, sizeof(hs));
    ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
    hs.region = r;
    ASSERT_EQ(asx_task_spawn(r, poll_holder, &hs, &t), ASX_OK);
    budget = asx_budget_from_polls(40u);
    ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_POLL_BUDGET_EXHAUSTED);
    ASSERT_EQ(g_fl.n, 0u);
    ASSERT_EQ(asx_trace_kind_total(&g_rt, ASX_TRACE_LAB_FUTURELOCK), (uint64_t)0u);
    asx_trace_set_observer(&g_rt, NULL, NULL);

    /* Off by default: a fresh lab reports nothing for a holder idle for
     * 1000 steps, though the on-demand query names it. */
    ASSERT_EQ(setup(), ASX_OK);
    ASSERT_EQ(asx_scheduler_use_lab_dispatch(7u), ASX_OK);
    fl_begin();
    {
        holder_state hp;
        asx_futurelock fl[2];
        memset(&hp, 0, sizeof(hp));
        ASSERT_EQ(asx_region_open(&g_rt, &r), ASX_OK);
        hp.region = r;
        hp.park = 1;
        ASSERT_EQ(asx_task_spawn(r, poll_holder, &hp, &t), ASX_OK);
        budget = asx_budget_infinite();
        ASSERT_EQ(asx_scheduler_run(r, &budget), ASX_E_WOULD_BLOCK);
        ASSERT_EQ(asx_trace_kind_total(&g_rt, ASX_TRACE_LAB_FUTURELOCK), (uint64_t)0u);
        ASSERT_EQ(asx_lab_futurelocks(3u, fl, 2u), 1u);
    }
    asx_trace_set_observer(&g_rt, NULL, NULL);
}

int main(void) {
    fprintf(stderr, "=== test_lab_dispatch ===\n");
    RUN_TEST(rng_replicates_rust_xorshift64);
    RUN_TEST(rng_remaps_degenerate_seeds);
    RUN_TEST(rng_replicates_rust_det_rng_for_every_seed_and_value_of_the_derived_document);
    RUN_TEST(a_lab_dispatch_run_allocates_nothing);
    RUN_TEST(ties_are_broken_by_the_step_value_in_generation_order);
    RUN_TEST(host_tasks_draw_from_rust_detentropy_streams);
    RUN_TEST(completed_tasks_free_their_arena_index_for_the_next_task);
    RUN_TEST(cancel_lane_is_served_first);
    RUN_TEST(auto_advance_moves_the_clock_to_the_next_timer);
    RUN_TEST(pre_poll_quota_cancel_has_rust_lab_attribution);
    RUN_TEST(dispatch_records_carry_task_lane_step_and_time);
    RUN_TEST(budget_cancel_at_a_completing_checkpoint_dispatches_the_retired_task);
    RUN_TEST(budget_deadline_timer_cancels_on_the_cancel_lane);
    RUN_TEST(child_region_open_and_close_are_next_step_commands);
    RUN_TEST(closing_an_empty_child_region_wakes_the_closer);
    RUN_TEST(child_region_commands_apply_at_once_without_lab_dispatch);
    RUN_TEST(capability_open_refusal_reaches_the_opener_after_it_parked);
    RUN_TEST(capability_open_is_admitted_at_the_same_step_with_the_meet);
    RUN_TEST(spawned_members_inherit_what_the_owner_has_left_after_its_region_command);
    RUN_TEST(region_cancel_request_applies_at_the_next_step);
    RUN_TEST(region_command_queue_full_is_failure_atomic);
    RUN_TEST(handle_abort_applies_at_the_next_step);
    RUN_TEST(handle_abort_of_an_unadmitted_child_applies_at_admission);
    RUN_TEST(handle_aborts_in_one_step_coalesce_to_the_strongest);
    RUN_TEST(handle_abort_applies_at_once_without_lab_dispatch);
    RUN_TEST(spawn_refusal_reaches_a_join_at_the_next_step);
    RUN_TEST(a_timer_wakes_the_waker_it_was_armed_with);
    RUN_TEST(a_permit_registration_is_admitted_when_the_poll_returns);
    RUN_TEST(a_permit_registration_past_the_limit_is_refused_at_the_drain);
    RUN_TEST(mpsc_quota_refused_permit_settles_on_a_later_poll);
    RUN_TEST(oneshot_quota_refused_permit_settles_on_a_later_poll);
    RUN_TEST(broadcast_quota_refused_permit_settles_on_a_later_poll);
    RUN_TEST(spawn_limit_is_checked_at_the_next_step_admission);
    RUN_TEST(spawn_into_a_region_closed_before_admission_is_refused);
    RUN_TEST(deferred_cleanup_admission_preserves_finalizing_quota_exception);
    RUN_TEST(admission_status_of_a_bad_handle_is_a_lookup_error);
    RUN_TEST(use_lab_dispatch_requires_no_live_task);
    RUN_TEST(forced_schedule_decodes_rust_recorded_artifacts_and_reproduces_their_certificate);
    RUN_TEST(forced_schedule_decoded_dispatch_fields_match_the_recorded_run);
    RUN_TEST(forced_schedule_decode_refuses_what_rust_refuses);
    RUN_TEST(forced_schedule_encode_refuses_partial_and_bad_input_and_sizes_its_output);
    RUN_TEST(forced_schedule_recording_captures_rust_arena_ids_and_the_running_certificate);
    RUN_TEST(schedule_certificate_folds_without_a_recorder);
#ifdef ASX_DEBUG_GHOST
    RUN_TEST(the_ghost_determinism_monitor_sees_every_pick_and_a_rerun_repeats_them);
#endif
    RUN_TEST(forced_schedule_recording_follows_rust_arena_index_reuse);
    RUN_TEST(forced_schedule_recording_refuses_misuse_and_overflow);
    RUN_TEST(forced_schedule_replays_the_recorded_run_exactly);
    RUN_TEST(forced_schedule_overrides_the_seeded_pick);
    RUN_TEST(forced_schedule_resolver_maps_recorded_task_ids);
    RUN_TEST(forced_schedule_resolver_declines_fall_back_to_the_arena_shadow);
    RUN_TEST(forced_schedule_replays_a_supervisor_generation_by_arena_id);
    RUN_TEST(forced_schedule_divergences_report_their_step_and_leave_the_task_unpolled);
    RUN_TEST(forced_schedule_begin_end_replays_a_runner_driven_run);
    RUN_TEST(forced_schedule_begin_end_reports_the_divergence_that_stopped_the_run);
    RUN_TEST(forced_schedule_begin_refuses_a_second_schedule_and_another_seed);
    RUN_TEST(forced_error_kinds_have_distinct_names);
    RUN_TEST(forced_schedule_refuses_a_run_that_is_not_pristine_or_has_another_seed);
    RUN_TEST(a_cancel_storm_yields_to_the_ready_lane_after_sixteen_dispatches);
    RUN_TEST(a_cancel_storm_with_an_empty_ready_lane_runs_on_past_sixteen);
    RUN_TEST(a_run_that_would_block_names_both_tasks_of_a_channel_deadlock);
    RUN_TEST(a_stuck_joiner_names_the_task_it_awaits);
    RUN_TEST(a_parked_obligation_holder_is_a_futurelock_past_the_threshold);
    RUN_TEST(a_task_polled_every_step_is_not_a_futurelock);
    RUN_TEST(a_sleeper_that_wakes_is_neither_stuck_nor_futurelocked);
    RUN_TEST(the_futurelock_check_needs_lab_dispatch_and_the_sweep_lists_parked_tasks);
    RUN_TEST(the_futurelock_check_reports_a_holder_at_every_step_it_stays_idle);
    RUN_TEST(the_futurelock_check_reports_holders_in_task_id_order);
    RUN_TEST(the_futurelock_check_is_silent_for_a_polled_holder_and_when_off);
    RUN_TEST_IF(8u * (uint32_t)ASX_MAX_TASKS >= CAP_TEST_ENTRIES,
                a_priority_group_is_capped_at_256_entries,
                "this resource class's lab lanes hold fewer than 300 entries");

    TEST_REPORT();
    return test_failures;
}
