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
 * An oracle reads one kernel: the one a lab's runtime holds (asx_oracle_*
 * with a lab), or the default runtime's (asx_oracle_check_named).
 *
 * SPDX-License-Identifier: MIT
 */

#include "runtime_internal.h"
#include <asx/core/cancel.h>
#include <asx/core/transition.h>
/* The minimal browser profile hides the replay and trace families from the
 * public surface; the oracles implement over them, as replay.c does. */
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/replay.h>
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
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

static asx_oracle_result check_closed_regions(const asx_kernel *k, const char *name,
                                              uint32_t checks) {
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

static asx_oracle_result check_task_leak(const asx_kernel *k) {
    return check_closed_regions(k, "task_leak", ORACLE_TASKS);
}

/* Reserved obligations whose holder is a task that has completed or is gone:
 * the kernel resolves a holder's obligations in the one completion path
 * (asx_task_on_complete_internal), so none can outlive its holder. An
 * obligation of no task (holder ASX_INVALID_ID) or of another runtime's
 * task is not read. */
static uint32_t obligations_of_ended_holders(const asx_kernel *k) {
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->obligation_count; i++) {
        const asx_obligation_slot *o = &k->obligations[i];
        uint32_t slot;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!o->alive || o->state != ASX_OBLIGATION_RESERVED) continue;
        if (o->holder == ASX_INVALID_ID || asx_handle_runtime_tag(o->holder) != k->tag) continue;
        slot = (uint32_t)asx_handle_slot(o->holder);
        if (slot >= k->task_count || !k->tasks[slot].alive ||
            k->tasks[slot].generation != asx_handle_generation(o->holder) ||
            asx_task_is_terminal(k->tasks[slot].state)) {
            bad++;
        }
    }
    return bad;
}

/* Rust's obligation_leak: no obligation outlives its owner. A closed region
 * holds no reserved obligation, and a reserved one's holder has not
 * completed. */
static asx_oracle_result check_obligation_leak(const asx_kernel *k) {
    asx_oracle_result r = check_closed_regions(k, "obligation_leak", ORACLE_OBLIGATIONS);
    uint32_t ended = obligations_of_ended_holders(k);
    if (ended == 0u) return r;
    if (r.verdict == ASX_ORACLE_FAIL) {
        r.detail_count += ended;
        return r;
    }
    return oracle_verdict(ASX_ORACLE_FAIL, "obligation_leak",
                          "a reserved obligation's holder has completed", ended);
}

static asx_oracle_result check_quiescence(const asx_kernel *k) {
    return check_closed_regions(k, "quiescence",
                                ORACLE_TASKS | ORACLE_CHILDREN | ORACLE_OBLIGATIONS |
                                    ORACLE_CLEANUP);
}

static asx_oracle_result check_finalizer(const asx_kernel *k) {
    return check_closed_regions(k, "finalizer", ORACLE_CLEANUP);
}

/* INV-TREE: every region but the one root names a live parent that lists it,
 * every listed child is a live region that names the region back, and a
 * parent walk ends. A closed region is no longer listed by its parent. */
static asx_oracle_result check_region_tree(const asx_kernel *k) {
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
static asx_oracle_result check_deadline_monotone(const asx_kernel *k) {
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
static asx_oracle_result check_cancel_propagation(const asx_kernel *k) {
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
 * record names that is no longer active is released and is not read. */
static asx_oracle_result check_cancel_correctness(const asx_kernel *k) {
    const char *first = NULL;
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->task_count; i++) {
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
static asx_oracle_result check_cancel_debt(const asx_kernel *k) {
    uint32_t bad = 0;
    uint32_t i;
    if (!asx_lab_dispatch_active()) {
        return oracle_verdict(ASX_ORACLE_PASS, "cancel_debt",
                              "no lab dispatch: the sweep's idle handling delivers a cancel", 0u);
    }
    for (i = 0; i < k->task_count; i++) {
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
 * ground under task_leak and quiescence. */
static asx_oracle_result check_task_counters(const asx_kernel *k) {
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        uint32_t j;
        uint32_t n = 0;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive) continue;
        for (j = 0; j < k->task_count; j++) {
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

/* Walk a released-slot free list: every entry is a released slot below the
 * high-water mark, the walk ends, and it holds every released slot (the
 * kernel pushes each release and pops each reuse, so the list is exactly the
 * released slots). Returns the defects found and sets *why for the first. */
static uint32_t free_list_defects(const asx_kernel *k, int tasks, const char **why) {
    uint32_t count = tasks ? k->task_count : k->obligation_count;
    uint32_t head = tasks ? k->task_free_head : k->obligation_free_head;
    uint32_t idx = head == 0u ? ASX_SLOT_NONE : head - 1u;
    uint32_t steps = 0;
    uint32_t released = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!(tasks ? k->tasks[i].alive : k->obligations[i].alive)) released++;
    }
    while (idx != ASX_SLOT_NONE) {
        /* ASX_CHECKPOINT_WAIVER("bounded: a walk of at most count entries") */
        if (idx >= count || (tasks ? k->tasks[idx].alive : k->obligations[idx].alive)) {
            *why = "a free-list entry is not a released slot";
            return 1u;
        }
        if (++steps > count) {
            *why = "a free list has a cycle";
            return 1u;
        }
        idx = tasks ? k->tasks[idx].next_free : k->obligations[idx].next_free;
    }
    if (steps != released) {
        *why = "a free list does not hold exactly the released slots";
        return 1u;
    }
    return 0u;
}

/* The kernel's counters against the arenas they summarize: the live count of
 * each of the three arenas against its live slots, and the task and
 * obligation free lists. (Each region's own live-task counter is
 * task_counters'.) Rust has no oracle of this name; the live counters are what
 * the resource gauges read (asx_runtime_*_live, resource.c) and the free
 * lists are where a spawn or a reserve finds its slot. */
static asx_oracle_result check_arena_counters(const asx_kernel *k) {
    const char *first = NULL;
    uint32_t bad = 0;
    uint32_t live_regions = 0;
    uint32_t live_tasks = 0;
    uint32_t live_obligations = 0;
    uint32_t i;
    const char *why = NULL;
    uint32_t d;
    for (i = 0; i < k->region_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (k->regions[i].alive) live_regions++;
    }
    for (i = 0; i < k->task_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (k->tasks[i].alive) live_tasks++;
    }
    for (i = 0; i < k->obligation_count; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (k->obligations[i].alive) live_obligations++;
    }
    if (live_regions != k->region_live) {
        bad++;
        first = "the live region counter disagrees with the live regions";
    }
    if (live_tasks != k->task_live) {
        bad++;
        if (first == NULL) first = "the live task counter disagrees with the live tasks";
    }
    if (live_obligations != k->obligation_live) {
        bad++;
        if (first == NULL) {
            first = "the live obligation counter disagrees with the live obligations";
        }
    }
    d = free_list_defects(k, 1, &why);
    if (d != 0u) {
        bad += d;
        if (first == NULL) first = why;
    }
    d = free_list_defects(k, 0, &why);
    if (d != 0u) {
        bad += d;
        if (first == NULL) first = why;
    }
    if (bad != 0u) return oracle_verdict(ASX_ORACLE_FAIL, "arena_counters", first, bad);
    return oracle_verdict(ASX_ORACLE_PASS, "arena_counters",
                          "the kernel's live counters and free lists match its arenas", 0u);
}

/* Rust's region_leak watches regions stuck in Closing, Draining or
 * Finalizing past wall-clock thresholds (RegionLeakConfig). State has no
 * clock, so this is the part of it that needs none: a region that is closing
 * and has nothing left to wait for. The kernel advances a closing region the
 * moment its last task completes, its last child closes or its cleanup
 * drains (asx_region_advance_internal), so a region found in CLOSING,
 * DRAINING or FINALIZING with no live task, no child that is not closed, no
 * cleanup pending or running, no async finalizer in flight and no reserved
 * obligation (which the PANIC leak policy waits on) is a missed advance: it
 * will never close. A poisoned region is exempt (containment keeps it open
 * on purpose). */
static asx_oracle_result check_region_leak(const asx_kernel *k) {
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive || r->state == ASX_REGION_OPEN || r->state == ASX_REGION_CLOSED) continue;
        if (r->poisoned || r->task_count != 0u || r->child_count != 0u) continue;
        if (asx_cleanup_pending(&r->cleanup) != 0u) continue;
        if (r->finalizer.active) continue;
        if (region_has_live_task(k, i, r->generation) ||
            region_has_live_child(k, i, r->generation) ||
            region_holds_reserved_obligation(k, i, r->generation)) {
            continue;
        }
        bad++;
    }
    if (bad != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "region_leak",
                              "a closing region has nothing left to wait for and is not closed",
                              bad);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "region_leak",
                          "every closing region still has something to wait for", 0u);
}

/* The retained trace index of the first region.cancelled event of `region`,
 * or UINT32_MAX when the ring holds none. */
static uint32_t first_region_cancelled_event(const asx_kernel *k, uint32_t events,
                                             asx_region_id region) {
    uint32_t i;
    for (i = 0; i < events; i++) {
        asx_trace_event e;
        /* ASX_CHECKPOINT_WAIVER("bounded: events <= ASX_TRACE_CAPACITY") */
        if (asx_trace_event_get_internal(k, i, &e) == 0) break;
        if (e.kind == ASX_TRACE_REGION_CANCELLED && e.entity_id == region) return i;
    }
    return UINT32_MAX;
}

/* Rust's cancel_signal_ordering: a cancel reaches a parent before its
 * children, and no child is cancelled by a parent that was not. Two of its
 * violations are visible here. Orphaned: a region whose cancel reason is a
 * propagated ParentCancelled (kind PARENT with a cause: asx_region_cancel
 * chains each descendant's reason to its parent's) while its parent has no
 * cancel requested. Child before parent: such a region's first
 * region.cancelled event is retained and its parent's is not before it
 * (asx_region_cancel emits a subtree parent first). The second reads the
 * trace ring, so it is skipped when the ring dropped events. Rust's third,
 * a child's cancel time earlier than its parent's, is not state: a parent's
 * stored reason can be strengthened later with a more severe one. */
static asx_oracle_result check_cancel_signal_ordering(const asx_kernel *k) {
    uint32_t orphaned = 0;
    uint32_t reordered = 0;
    uint32_t events = asx_trace_event_count_internal(k);
    int ring_whole = asx_trace_dropped_internal(k) == 0u;
    uint32_t i;
    for (i = 0; i < k->region_count; i++) {
        const asx_region_slot *r = &k->regions[i];
        const asx_region_slot *p;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!r->alive || !r->cancel_requested) continue;
        if (r->cancel_reason.kind != ASX_CANCEL_PARENT || r->cancel_reason.cause == NULL) continue;
        p = oracle_region(k, r->parent_id);
        if (p == NULL || !p->cancel_requested) {
            orphaned++;
            continue;
        }
        if (ring_whole) {
            uint32_t at_child =
                first_region_cancelled_event(k, events, asx_region_handle_for_slot(k, i));
            uint32_t at_parent = first_region_cancelled_event(k, events, r->parent_id);
            if (at_child != UINT32_MAX && at_parent > at_child) reordered++;
        }
    }
    if (orphaned != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "cancel_signal_ordering",
                              "a region was cancelled by its parent, which was not cancelled",
                              orphaned + reordered);
    }
    if (reordered != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "cancel_signal_ordering",
                              "a region's cancel was recorded before its parent's", reordered);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "cancel_signal_ordering",
                          "every propagated cancel followed its parent's", 0u);
}

/* Rust's channel_atomicity oracle follows reserve, send and abort events and
 * fails a reservation that was not atomic with its capacity claim. The
 * channel's counters show the same thing as state: a message and a permit
 * each hold one slot of the capacity, a permit is a token in the permit
 * table, and the sender count is the creator's sender (until dropped) plus
 * the live clones. A mailbox is a bounded channel, so its capacity and
 * backpressure accounting is this one's. */
const char *asx_channel_accounting_defect_internal(const asx_channel_accounting *a) {
    if (a->queued > a->capacity) return "a channel holds more messages than its capacity";
    if (a->queued + a->reserved > a->capacity) {
        return "a channel's queued messages and reserved permits exceed its capacity";
    }
    if (a->reserved != a->permit_tokens) {
        return "a channel's reserved count disagrees with its live permit tokens";
    }
    if (a->sender_count != a->sender_tokens + (a->primary_dropped != 0u ? 0u : 1u)) {
        return "a channel's sender count disagrees with its live sender identities";
    }
    if (a->has_in_use != 0u && a->in_use != a->queued + a->reserved) {
        return "a channel's in-use count is not its messages plus its permits";
    }
    return NULL;
}

/* The channels of this kernel's runtime (the channel table is the channel
 * module's, process-wide; a channel names the region it was made in, and a
 * region handle carries its runtime's tag). */
static asx_oracle_result check_channel_atomicity(const asx_kernel *k) {
    const char *first = NULL;
    uint32_t bad = 0;
    uint32_t n = asx_channel_slot_count_internal(k);
    uint32_t i;
    for (i = 0; i < n; i++) {
        asx_channel_accounting a;
        const char *why;
        /* ASX_CHECKPOINT_WAIVER("bounded: the channel table's high-water mark") */
        if (!asx_channel_accounting_get_internal(k, i, &a)) continue;
        if (a.region != ASX_INVALID_ID && asx_handle_runtime_tag(a.region) != k->tag) continue;
        why = asx_channel_accounting_defect_internal(&a);
        if (why != NULL) {
            bad++;
            if (first == NULL) first = why;
        }
    }
    if (bad != 0u) return oracle_verdict(ASX_ORACLE_FAIL, "channel_atomicity", first, bad);
    return oracle_verdict(ASX_ORACLE_PASS, "channel_atomicity",
                          "every channel's permits, messages and senders are accounted for", 0u);
}

/* Rust's down_order: DOWN notifications are delivered in a deterministic
 * order, by completion time, then by monitored task and reference
 * (DownBatch::into_sorted). The monitor module keeps the order it fired in
 * (asx_monitor_delivery_get_internal), and a delivery that fired right after
 * another breaks it if its completion time is earlier, or if it is a monitor
 * of the same exit with a reference not above the one before it
 * (asx_monitor_task_finished fires the lowest reference left each time). */
const char *asx_monitor_delivery_defect_internal(const asx_monitor_delivery *prev,
                                                 const asx_monitor_delivery *next) {
    if (next->vt < prev->vt) {
        return "a DOWN was delivered with an earlier completion time than one before it";
    }
    if (asx_handle_same_entity(next->monitored, prev->monitored) && next->ref <= prev->ref) {
        return "the monitors of one exit were not delivered by ascending reference";
    }
    return NULL;
}

/* The monitor table is the monitor module's and process-wide: the kernel is
 * not read. */
static asx_oracle_result check_down_order(const asx_kernel *k) {
    asx_monitor_delivery prev;
    uint32_t n = asx_monitor_delivery_count_internal();
    uint32_t bad = 0;
    uint32_t i;
    const char *first = NULL;
    (void)k;
    if (n < 2u || !asx_monitor_delivery_get_internal(0u, &prev)) {
        return oracle_verdict(ASX_ORACLE_PASS, "down_order", "DOWNs were delivered in order", 0u);
    }
    for (i = 1u; i < n; i++) {
        asx_monitor_delivery next;
        const char *why;
        /* ASX_CHECKPOINT_WAIVER("bounded: n <= ASX_MONITOR_CAPACITY") */
        if (!asx_monitor_delivery_get_internal(i, &next)) break;
        why = asx_monitor_delivery_defect_internal(&prev, &next);
        if (why != NULL) {
            bad++;
            if (first == NULL) first = why;
        }
        prev = next;
    }
    if (bad != 0u) return oracle_verdict(ASX_ORACLE_FAIL, "down_order", first, bad);
    return oracle_verdict(ASX_ORACLE_PASS, "down_order", "DOWNs were delivered in order", 0u);
}

/* Rust's reply_linearity: a Spork reply is a permit that resolves exactly
 * once. C has no reply-specific obligation: a call's reply is a SEND_PERMIT
 * obligation, so the property is checked for every obligation, which is the
 * superset. The trace ring has each reservation and each resolution (commit,
 * abort, leak) with the obligation's handle: an obligation that resolved
 * twice is a violation, and so is a resolution with no reservation before it
 * (checked only while the ring dropped nothing). Obligation slots are few,
 * so one pass keeps a state per slot, keyed by the handle's generation. */
static asx_oracle_result check_reply_linearity(const asx_kernel *k) {
    uint16_t gen[ASX_MAX_OBLIGATIONS];
    uint8_t seen[ASX_MAX_OBLIGATIONS]; /* 0 none, 1 reserved, 2 resolved */
    uint32_t events = asx_trace_event_count_internal(k);
    int ring_whole = asx_trace_dropped_internal(k) == 0u;
    uint32_t twice = 0;
    uint32_t unreserved = 0;
    uint32_t i;
    memset(gen, 0, sizeof(gen));
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < events; i++) {
        asx_trace_event e;
        uint32_t slot;
        uint16_t g;
        /* ASX_CHECKPOINT_WAIVER("bounded: events <= ASX_TRACE_CAPACITY") */
        if (asx_trace_event_get_internal(k, i, &e) == 0) break;
        if (e.kind != ASX_TRACE_OBLIGATION_RESERVE && e.kind != ASX_TRACE_OBLIGATION_COMMIT &&
            e.kind != ASX_TRACE_OBLIGATION_ABORT && e.kind != ASX_TRACE_OBLIGATION_LEAK) {
            continue;
        }
        slot = (uint32_t)asx_handle_slot(e.entity_id);
        g = asx_handle_generation(e.entity_id);
        if (slot >= (uint32_t)ASX_MAX_OBLIGATIONS) continue;
        if (e.kind == ASX_TRACE_OBLIGATION_RESERVE) {
            gen[slot] = g;
            seen[slot] = 1u;
            continue;
        }
        if (gen[slot] == g && seen[slot] == 2u) {
            twice++;
        } else if ((gen[slot] != g || seen[slot] == 0u) && ring_whole) {
            unreserved++;
        }
        gen[slot] = g;
        seen[slot] = 2u;
    }
    if (twice != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "reply_linearity",
                              "an obligation was resolved more than once", twice + unreserved);
    }
    if (unreserved != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "reply_linearity",
                              "an obligation was resolved with no reservation before it",
                              unreserved);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "reply_linearity",
                          "every obligation resolved at most once, after its reservation", 0u);
}

/* Rust's loser_drain: the losers of a race are cancelled and fully drained
 * before the race completes. A task group is caller-owned and unregistered,
 * but each of its members names the group's owner as its completion watcher
 * while the owner collects them, and a loser carries the race-lost cancel
 * (ASX_CANCEL_RACE_LOST) it was aborted with. So a live task whose cancel
 * is race-lost, and whose watcher task is gone or completed, is a loser that
 * outlived the race that cancelled it. The owner waits for its members in
 * DRAINING before it completes, so this is a group abandoned, or a drain
 * that returned early. */
static asx_oracle_result check_loser_drain(const asx_kernel *k) {
    uint32_t bad = 0;
    uint32_t i;
    for (i = 0; i < k->task_count; i++) {
        const asx_task_slot *t = &k->tasks[i];
        const asx_task_slot *owner;
        /* ASX_CHECKPOINT_WAIVER("bounded arena scan") */
        if (!t->alive || asx_task_is_terminal(t->state)) continue;
        if (!t->cancel_pending || t->cancel_reason.kind != ASX_CANCEL_RACE_LOST) continue;
        if (t->watcher == ASX_SLOT_NONE || t->watcher >= k->task_count) continue;
        owner = &k->tasks[t->watcher];
        if (!owner->alive || owner->generation != t->watcher_gen ||
            asx_task_is_terminal(owner->state)) {
            bad++;
        }
    }
    if (bad != 0u) {
        return oracle_verdict(ASX_ORACLE_FAIL, "loser_drain",
                              "a race loser is still running after the task that raced it ended",
                              bad);
    }
    return oracle_verdict(ASX_ORACLE_PASS, "loser_drain",
                          "no race loser outlived the task that raced it", 0u);
}

typedef asx_oracle_result (*oracle_check_fn)(const asx_kernel *k);

/* The lab contract the older oracles have: NULL fails, an uninitialized lab
 * is inconclusive; the state read is the lab's runtime's kernel. */
static asx_oracle_result lab_oracle(const asx_lab *lab, const char *name, oracle_check_fn check) {
    const asx_kernel *k;
    if (lab == NULL) return oracle_verdict(ASX_ORACLE_FAIL, name, "null lab", 0u);
    if (!lab->initialized) {
        return oracle_verdict(ASX_ORACLE_INCONCLUSIVE, name, "lab not initialized", 0u);
    }
    k = asx_kernel_for_tag(lab->rt.tag);
    if (k == NULL) {
        return oracle_verdict(ASX_ORACLE_INCONCLUSIVE, name, "the lab's runtime is not held", 0u);
    }
    return check(k);
}

asx_oracle_result asx_oracle_task_leak(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "task_leak", check_task_leak);
}

asx_oracle_result asx_oracle_obligation_leak(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "obligation_leak", check_obligation_leak);
}

asx_oracle_result asx_oracle_region_quiescence(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "quiescence", check_quiescence);
}

asx_oracle_result asx_oracle_finalizer(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "finalizer", check_finalizer);
}

asx_oracle_result asx_oracle_region_tree(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "region_tree", check_region_tree);
}

asx_oracle_result asx_oracle_deadline_monotone(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "deadline_monotone", check_deadline_monotone);
}

asx_oracle_result asx_oracle_cancel_propagation(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "cancellation_protocol", check_cancel_propagation);
}

asx_oracle_result asx_oracle_cancel_correctness(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "cancel_correctness", check_cancel_correctness);
}

asx_oracle_result asx_oracle_cancel_debt(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "cancel_debt", check_cancel_debt);
}

asx_oracle_result asx_oracle_task_counters(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "task_counters", check_task_counters);
}

asx_oracle_result asx_oracle_arena_counters(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "arena_counters", check_arena_counters);
}

asx_oracle_result asx_oracle_channel_atomicity(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "channel_atomicity", check_channel_atomicity);
}

asx_oracle_result asx_oracle_down_order(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "down_order", check_down_order);
}

asx_oracle_result asx_oracle_reply_linearity(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "reply_linearity", check_reply_linearity);
}

asx_oracle_result asx_oracle_loser_drain(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "loser_drain", check_loser_drain);
}

asx_oracle_result asx_oracle_region_leak(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "region_leak", check_region_leak);
}

asx_oracle_result asx_oracle_cancel_signal_ordering(const asx_lab *lab, void *ctx) {
    (void)ctx;
    return lab_oracle(lab, "cancel_signal_ordering", check_cancel_signal_ordering);
}

typedef struct {
    const char *name;
    oracle_check_fn check;
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
    {"arena_counters", check_arena_counters},
    {"region_leak", check_region_leak},
    {"cancel_signal_ordering", check_cancel_signal_ordering},
    {"channel_atomicity", check_channel_atomicity},
    {"down_order", check_down_order},
    {"reply_linearity", check_reply_linearity},
    {"loser_drain", check_loser_drain},
};

#define ORACLE_TABLE_COUNT (sizeof(g_oracle_table) / sizeof(g_oracle_table[0]))

uint32_t asx_oracle_kernel_count(void) { return (uint32_t)ORACLE_TABLE_COUNT; }

const char *asx_oracle_kernel_name(uint32_t index) {
    return index < ORACLE_TABLE_COUNT ? g_oracle_table[index].name : NULL;
}

asx_oracle_result asx_oracle_check_named(const asx_runtime *rt, const char *name) {
    const asx_kernel *k = asx_kernel_of_runtime(rt);
    uint32_t i;
    if (name == NULL) return oracle_verdict(ASX_ORACLE_FAIL, "unknown", "null oracle name", 0u);
    if (k == NULL) return oracle_verdict(ASX_ORACLE_FAIL, "unknown", "no runtime", 0u);
    for (i = 0; i < ORACLE_TABLE_COUNT; i++) {
        /* ASX_CHECKPOINT_WAIVER("bounded: a constant-size table") */
        if (strcmp(g_oracle_table[i].name, name) == 0) return g_oracle_table[i].check(k);
    }
    return oracle_verdict(ASX_ORACLE_FAIL, "unknown", "unknown oracle", 0u);
}
