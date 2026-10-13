/*
 * oracle.c — Rust's lab oracles over the kernel's arenas (bd-9kll.9.2)
 *
 * docs/LAB_ORACLE_CATALOG.md says which of Rust's oracles these are and what
 * each reads. Each is one pass over the region, task or obligation arena that
 * writes nothing and answers pass or fail with a static message and the number
 * of violating entities. They check what the kernel enforces by construction
 * (a region cannot close with live tasks, a child's budget is its parent's
 * met, ...), as a regression net: a FAIL is a kernel bug, not a program error.
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/core/cancel.h>
#include <asx/core/transition.h>
#include <asx/runtime/replay.h>
#include <string.h>

static asx_oracle_result oracle_verdict(asx_oracle_verdict verdict, const char *name,
                                        const char *message, uint32_t violations) {
    asx_oracle_result r;
    r.verdict = verdict;
    r.oracle_name = name;
    r.message = message;
    r.detail_count = violations;
    return r;
}

/* The live region slot a region handle names (its generation current), or
 * NULL: ASX_INVALID_ID, an index past the high-water mark, a reclaimed slot. */
static const asx_region_slot *oracle_region(const asx_kernel *k, asx_region_id id) {
    uint32_t slot;
    if (id == ASX_INVALID_ID) return NULL;
    slot = (uint32_t)asx_handle_slot(id);
    if (slot >= k->region_count) return NULL;
    if (!k->regions[slot].alive) return NULL;
    if (k->regions[slot].generation != asx_handle_generation(id)) return NULL;
    return &k->regions[slot];
}

static int oracle_names_region(asx_region_id id, uint32_t slot, uint16_t generation) {
    return id != ASX_INVALID_ID && (uint32_t)asx_handle_slot(id) == slot &&
           asx_handle_generation(id) == generation;
}

static int region_has_live_task(const asx_kernel *k, uint32_t slot, uint16_t generation) {
    uint32_t i;
    for (i = 0; i < k->task_count; i++) {
        const asx_task_slot *t = &k->tasks[i];
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!t->alive || asx_task_is_terminal(t->state)) continue;
        if (oracle_names_region(t->region, slot, generation)) return 1;
    }
    return 0;
}

static int region_has_live_child(const asx_kernel *k, uint32_t slot, uint16_t generation) {
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *c = &k->regions[i];
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!c->alive || c->state == ASX_REGION_CLOSED) continue;
        if (oracle_names_region(c->parent_id, slot, generation)) return 1;
    }
    return 0;
}

static int region_holds_reserved_obligation(const asx_kernel *k, uint32_t slot,
                                            uint16_t generation) {
    uint32_t i;
    for (i = 0; i < k->obligation_count; i++) {
        const asx_obligation_slot *o = &k->obligations[i];
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!o->alive || o->state != ASX_OBLIGATION_RESERVED) continue;
        if (oracle_names_region(o->region, slot, generation)) return 1;
    }
    return 0;
}

/* What a closed region must have behind it (Rust: tasks, quiescence and
 * finalizer oracles over closed regions). */
#define ORACLE_TASKS 1u
#define ORACLE_CHILDREN 2u
#define ORACLE_OBLIGATIONS 4u
#define ORACLE_CLEANUP 8u

static asx_oracle_result check_closed_regions(const char *name, uint32_t checks) {
    const asx_kernel *k = g_kernels[0];
    const char *first = NULL;
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        const char *why = NULL;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive || r->state != ASX_REGION_CLOSED) continue;
        if ((checks & ORACLE_TASKS) != 0u &&
            (r->task_count != 0u || region_has_live_task(k, i, r->generation))) {
            why = "a closed region has a live task";
        } else if ((checks & ORACLE_CHILDREN) != 0u &&
                   (r->child_count != 0u || region_has_live_child(k, i, r->generation))) {
            why = "a closed region has a child region that is not closed";
        } else if ((checks & ORACLE_OBLIGATIONS) != 0u &&
                   region_holds_reserved_obligation(k, i, r->generation)) {
            why = "a closed region holds a reserved obligation";
        } else if ((checks & ORACLE_CLEANUP) != 0u && asx_cleanup_pending(&r->cleanup) != 0u) {
            why = "a closed region has cleanup still pending";
        }
        if (why != NULL) {
            bad++;
            if (first == NULL) first = why;
        }
    }
    if (bad != 0u) return oracle_verdict(ASX_ORACLE_FAIL, name, first, bad);
    return oracle_verdict(ASX_ORACLE_PASS, name, "every closed region is quiescent", 0u);
}

static asx_oracle_result check_task_leak(void) {
    return check_closed_regions("task_leak", ORACLE_TASKS);
}

static asx_oracle_result check_obligation_leak(void) {
    return check_closed_regions("obligation_leak", ORACLE_OBLIGATIONS);
}

static asx_oracle_result check_quiescence(void) {
    return check_closed_regions("quiescence", ORACLE_TASKS | ORACLE_CHILDREN | ORACLE_OBLIGATIONS |
                                                  ORACLE_CLEANUP);
}

static asx_oracle_result check_finalizer(void) {
    return check_closed_regions("finalizer", ORACLE_CLEANUP);
}

/* INV-TREE: every region but the one root names a live parent that lists it,
 * every listed child is a live region that names the region back, and a
 * parent walk ends. A closed region is no longer listed by its parent. */
static asx_oracle_result check_region_tree(void) {
    const asx_kernel *k = g_kernels[0];
    const char *first = NULL;
    uint32_t bad = 0;
    uint32_t roots = 0;
    uint32_t live = 0;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        const char *why = NULL;
        const asx_region_slot *p;
        uint32_t j;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive) continue;
        live++;
        if (r->parent_id == ASX_INVALID_ID) {
            roots++;
        } else if (r->state != ASX_REGION_CLOSED) {
            /* A closed region is history: its parent may be gone. */
            uint32_t steps = 0;
            p = oracle_region(k, r->parent_id);
            if (p == NULL) {
                why = "a region's parent is not a live region";
            } else {
                int listed = 0;
                for (j = 0; j < p->child_count && j < ASX_MAX_REGION_CHILDREN; j++) {
                    /* ASX_CHECKPOINT_WAIVER("bounded: child_count <= ASX_MAX_REGION_CHILDREN") */
                    if (oracle_names_region(p->children[j], i, r->generation)) listed = 1;
                }
                if (!listed) why = "a region is not listed by its parent";
            }
            while (why == NULL && p != NULL && p->parent_id != ASX_INVALID_ID) {
                /* ASX_CHECKPOINT_WAIVER("bounded: at most k->region_count steps") */
                if (++steps > k->region_count) why = "the region tree has a cycle";
                p = oracle_region(k, p->parent_id);
            }
        }
        for (j = 0; why == NULL && j < r->child_count && j < ASX_MAX_REGION_CHILDREN; j++) {
            const asx_region_slot *c = oracle_region(k, r->children[j]);
            /* ASX_CHECKPOINT_WAIVER("bounded: child_count <= ASX_MAX_REGION_CHILDREN") */
            if (c == NULL || !oracle_names_region(c->parent_id, i, r->generation)) {
                why = "a listed child is not a live region that names its parent";
            }
        }
        if (why != NULL) {
            bad++;
            if (first == NULL) first = why;
        }
    }
    if (bad == 0u && live != 0u && roots == 0u) {
        bad = 1u;
        first = "the region tree has no root";
    } else if (bad == 0u && roots > 1u) {
        bad = roots - 1u;
        first = "the region tree has more than one root";
    }
    if (bad != 0u) return oracle_verdict(ASX_ORACLE_FAIL, "region_tree", first, bad);
    return oracle_verdict(ASX_ORACLE_PASS, "region_tree", "regions form one rooted tree", 0u);
}

/* INV-DEADLINE-MONOTONE: a child's deadline is no later than its parent's. */
static asx_oracle_result check_deadline_monotone(void) {
    const asx_kernel *k = g_kernels[0];
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        const asx_region_slot *p;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive) continue;
        p = oracle_region(k, r->parent_id);
        if (p != NULL && r->budget.deadline > p->budget.deadline) bad++;
    }
    if (bad != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "deadline_monotone",
                              "a region's deadline is later than its parent's", bad);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "deadline_monotone",
                          "no deadline is later than its parent's", 0u);
}

/* INV-CANCEL-PROPAGATES: every live, not closed child of a region with a
 * cancel requested has a cancel requested too (asx_region_cancel walks the
 * subtree; a cancelled region takes no new child). The task-level request,
 * drain, finalize order is the transition table's. */
static asx_oracle_result check_cancel_propagation(void) {
    const asx_kernel *k = g_kernels[0];
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        uint32_t j;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive || !r->cancel_requested) continue;
        for (j = 0; j < r->child_count && j < ASX_MAX_REGION_CHILDREN; j++) {
            const asx_region_slot *c = oracle_region(k, r->children[j]);
            /* ASX_CHECKPOINT_WAIVER("bounded: child_count <= ASX_MAX_REGION_CHILDREN") */
            if (c != NULL && c->state != ASX_REGION_CLOSED && !c->cancel_requested) bad++;
        }
    }
    if (bad != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "cancellation_protocol",
                              "a cancelled region has a child that was not cancelled", bad);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "cancellation_protocol",
                          "every cancel reached its descendant regions", 0u);
}

/* Whether a task handle names slot `slot` of kernel `k` at `generation`. */
static int oracle_names_task(const asx_kernel *k, asx_task_id id, uint32_t slot,
                             uint16_t generation) {
    return id != ASX_INVALID_ID && asx_handle_runtime_tag(id) == k->tag &&
           (uint32_t)asx_handle_slot(id) == slot && asx_handle_generation(id) == generation;
}

/* Rust's cancel_correctness oracle validates the stream of witnesses a task's
 * cancel produced (types/cancel.rs CancelWitness::validate_initial and
 * validate_transition). The kernel keeps one witness per cancelled task, so
 * what is checkable from state is: the first-witness rule (a non-zero epoch),
 * the witness's identity against its task's record (the same task, region and
 * epoch: validate_transition's TaskMismatch, RegionMismatch and EpochMismatch
 * cases), that its phase is not ahead of the task's (a regression is the
 * reverse), and that a completed task has released it. A witness the task
 * record names that is no longer active is released and is not read. The
 * scan covers the whole arena: a slot past the high-water mark is never
 * alive. */
static asx_oracle_result check_cancel_correctness(void) {
    const asx_kernel *k = g_kernels[0];
    const char *first = NULL;
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        const asx_task_slot *t = &k->tasks[i];
        asx_cancel_witness_info w;
        const char *why = NULL;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!t->alive || t->cancel_witness == ASX_INVALID_ID) continue;
        if (asx_cancel_witness_info_get(t->cancel_witness, &w) != ASX_OK) continue;
        if (asx_task_is_terminal(t->state)) {
            why = "a completed task still holds an active cancel witness";
        } else if (asx_cancel_witness_validate_initial(&w) != ASX_OK) {
            why = "a cancel witness carries epoch 0";
        } else if (!oracle_names_task(k, w.task, i, t->generation)) {
            why = "a cancel witness names another task";
        } else if (w.region != t->region) {
            why = "a cancel witness names another region than its task's";
        } else if (w.epoch != t->cancel_epoch) {
            why = "a cancel witness carries another epoch than its task's";
        } else if ((int)w.phase > (int)t->cancel_phase) {
            why = "a cancel witness is ahead of its task's cancel phase";
        }
        if (why != NULL) {
            bad++;
            if (first == NULL) first = why;
        }
    }
    if (bad != 0u) return oracle_verdict(ASX_ORACLE_FAIL, "cancel_correctness", first, bad);
    return oracle_verdict(ASX_ORACLE_PASS, "cancel_correctness",
                          "every active cancel witness matches its task", 0u);
}

/* Cancel debt, as far as state can say it: a cancel was requested of a task
 * and nothing is left that will deliver it. Under lab dispatch a request
 * schedules its task (the waker of the task, lab/runtime.rs), so a task whose
 * record holds a request it has not acknowledged (CANCEL_REQUESTED: a budget
 * cancel the task raised on itself is not one, it is not on the record), that
 * has not been polled since, is not masked, and is neither scheduled, in a
 * poll, awaiting an admission or an open, nor armed with a timer, is a lost
 * wake: the cancel is owed and nobody will pay it. The sweep scheduler's idle
 * handling unparks such a task on its own (sched_unpark_cancelled), so there
 * the state is not a violation and the oracle does not look. */
static asx_oracle_result check_cancel_debt(void) {
    const asx_kernel *k = g_kernels[0];
    uint32_t bad = 0;
    uint32_t i;
    if (!asx_lab_dispatch_active()) {
        return oracle_verdict(ASX_ORACLE_PASS, "cancel_debt",
                              "no lab dispatch: the sweep's idle handling delivers a cancel", 0u);
    }
    for (i = 0; i < ASX_MAX_TASKS; i++) {
        const asx_task_slot *t = &k->tasks[i];
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!t->alive || t->state != ASX_TASK_CANCEL_REQUESTED) continue;
        if (t->mask_depth != 0u || t->cancel_polled || t->in_poll) continue;
        if (t->lab_scheduled || t->lab_admission_pending) continue;
        if (t->timer_armed || t->deadline_timer_armed) continue;
        if (t->region_wait == ASX_REGION_WAIT_OPEN) continue;
        bad++;
    }
    if (bad != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "cancel_debt",
                              "a task has a cancel requested and nothing will run it", bad);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "cancel_debt",
                          "every requested cancel has a task that will see it", 0u);
}

/* A region's live-task counter (what its close decision reads, and what
 * task_leak trusts) against the live, not completed tasks that name it. The
 * counter counts a task from its spawn, a lab admission still pending
 * included, to its completion, so every such task is a live slot in a state
 * that is not terminal. Rust has no oracle of this name: the counter is the
 * ground under task_leak and quiescence. The scan covers the whole arena. */
static asx_oracle_result check_task_counters(void) {
    const asx_kernel *k = g_kernels[0];
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < ASX_MAX_REGIONS; i++) {
        const asx_region_slot *r = &k->regions[i];
        uint32_t j;
        uint32_t n = 0;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive) continue;
        for (j = 0; j < ASX_MAX_TASKS; j++) {
            const asx_task_slot *t = &k->tasks[j];
            /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
            if (!t->alive || asx_task_is_terminal(t->state)) continue;
            if (oracle_names_region(t->region, i, r->generation)) n++;
        }
        if (n != r->task_count) bad++;
    }
    if (bad != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "task_counters",
                              "a region's live-task counter disagrees with its tasks", bad);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "task_counters",
                          "every region's live-task counter matches its tasks", 0u);
}

/* The lab contract the older oracles have: NULL fails, an uninitialized lab
 * is inconclusive. The state itself is the runtime's, not the lab's. */
static int oracle_lab_refused(const asx_lab *lab, const char *name, asx_oracle_result *out) {
    if (lab == NULL) {
        *out = oracle_verdict(ASX_ORACLE_FAIL, name, "null lab", 0u);
        return 1;
    }
    if (!lab->initialized) {
        *out = oracle_verdict(ASX_ORACLE_INCONCLUSIVE, name, "lab not initialized", 0u);
        return 1;
    }
    return 0;
}

asx_oracle_result asx_oracle_task_leak(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "task_leak", &r)) return r;
    return check_task_leak();
}

asx_oracle_result asx_oracle_obligation_leak(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "obligation_leak", &r)) return r;
    return check_obligation_leak();
}

asx_oracle_result asx_oracle_region_quiescence(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "quiescence", &r)) return r;
    return check_quiescence();
}

asx_oracle_result asx_oracle_finalizer(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "finalizer", &r)) return r;
    return check_finalizer();
}

asx_oracle_result asx_oracle_region_tree(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "region_tree", &r)) return r;
    return check_region_tree();
}

asx_oracle_result asx_oracle_deadline_monotone(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "deadline_monotone", &r)) return r;
    return check_deadline_monotone();
}

asx_oracle_result asx_oracle_cancel_propagation(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "cancellation_protocol", &r)) return r;
    return check_cancel_propagation();
}

asx_oracle_result asx_oracle_cancel_correctness(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "cancel_correctness", &r)) return r;
    return check_cancel_correctness();
}

asx_oracle_result asx_oracle_cancel_debt(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "cancel_debt", &r)) return r;
    return check_cancel_debt();
}

asx_oracle_result asx_oracle_task_counters(const asx_lab *lab, void *ctx) {
    asx_oracle_result r;
    (void)ctx;
    if (oracle_lab_refused(lab, "task_counters", &r)) return r;
    return check_task_counters();
}

typedef struct {
    const char *name;
    asx_oracle_result (*check)(void);
} oracle_entry;

static const oracle_entry g_oracle_table[] = {
    {"task_leak", check_task_leak},
    {"obligation_leak", check_obligation_leak},
    {"quiescence", check_quiescence},
    {"finalizer", check_finalizer},
    {"region_tree", check_region_tree},
    {"deadline_monotone", check_deadline_monotone},
    {"cancellation_protocol", check_cancel_propagation},
    {"cancel_correctness", check_cancel_correctness},
    {"cancel_debt", check_cancel_debt},
    {"task_counters", check_task_counters},
};

#define ORACLE_TABLE_COUNT (sizeof(g_oracle_table) / sizeof(g_oracle_table[0]))

uint32_t asx_oracle_kernel_count(void) { return (uint32_t)ORACLE_TABLE_COUNT; }

const char *asx_oracle_kernel_name(uint32_t index) {
    return index < ORACLE_TABLE_COUNT ? g_oracle_table[index].name : NULL;
}

asx_oracle_result asx_oracle_check_named(const char *name) {
    uint32_t i;
    if (name == NULL) return oracle_verdict(ASX_ORACLE_FAIL, "unknown", "null oracle name", 0u);
    for (i = 0; i < ORACLE_TABLE_COUNT; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded: a constant-size table") */
        if (strcmp(g_oracle_table[i].name, name) == 0) return g_oracle_table[i].check();
    }
    return oracle_verdict(ASX_ORACLE_FAIL, "unknown", "unknown oracle", 0u);
}
