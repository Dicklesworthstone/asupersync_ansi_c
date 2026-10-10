/*
 * supervisor.c — the managed supervisor (Rust src/supervision.rs, the
 * managed module, :1795-3506) on the runtime's tasks and regions
 *
 * The controller task runs Rust's Controller (execute, then finish) as a
 * state machine: `pc` is where the controller resumes. Its operations on
 * one child (start, drain), its wait for an exit, its backoff and the
 * supervisor region's open and close are sub-operations, one at a time,
 * each resuming at its own step; every point where Rust awaits is a step
 * a later poll resumes at, every other point runs on in the same poll.
 *
 * A generation's task runs gen_poll, the body Rust spawns around the
 * factory (:2646-2726): it publishes its identity and its start, polls the
 * body the start function returned, and publishes the body's end.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include <asx/actor/supervisor.h>
#include <asx/core/transition.h>
#include <asx/cx/cx.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <stdio.h>
#include <string.h>

#define SUP_NONE 0xFFFFFFFFu
#define SUP_CHILDREN (ASX_SUPERVISOR_MAX_CHILDREN)

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

/* A compiled child. Children are kept in start order (bind_managed
 * reorders the specs, :2164-2184); positions index everything else. */
typedef struct {
    char name[ASX_SUPERVISOR_NAME_MAX];
    uint32_t spec_index;
    asx_child_start_fn start;
    void *user_data;
    asx_child_restart restart;
    uint32_t deps[ASX_SUPERVISOR_MAX_DEPS]; /* positions */
    uint32_t dep_count;
    uint8_t required;
    uint8_t start_immediately;
} sup_child;

/* A running generation (Rust RunningChild, :2206-2216) and the state its
 * task shares with the controller (ChildPublication, :2197-2204). */
typedef struct {
    int present;
    uint64_t number;
    asx_region_id region;
    int region_held; /* its ChildRegion is still owned */
    asx_task_id task;
    int handle_held; /* its TaskHandle is not joined yet */
    int cancellation_sent;
    int start_observed;
    int terminal_observed;
    /* The publication. */
    int started;
    int has_identity;
    asx_task_id identity_task;
    int has_terminal;
    asx_time terminal_at;
    asx_outcome_severity terminal_outcome;
    asx_status terminal_status;
    const char *terminal_panic;
    int terminal_shutdown;
    int shutdown_requested;
    int has_waiter;
    asx_task_id waiter;
    uint8_t waiter_prio;
    /* The body the start function returned. */
    int constructed;
    asx_task_poll_fn body;
    void *body_data;
} sup_gen;

/* A child's latest ended generation, with its task's position in Rust's
 * TaskId order (arena index, then generation) for the ready order. */
typedef struct {
    int present;
    asx_supervisor_completion c;
    uint64_t task_key;
} sup_latest;

/* A sub-operation's error (Rust ManagedSupervisorError), NONE for Ok. */
typedef struct {
    asx_supervisor_error kind;
    uint32_t child; /* position, SUP_NONE for none */
    asx_status status;
} sup_err;

typedef enum {
    SUB_NONE = 0,
    SUB_OPEN_ROOT,
    SUB_START,
    SUB_DRAIN,
    SUB_WAIT_EXIT,
    SUB_BACKOFF,
    SUB_CLOSE_ROOT
} sup_sub;

typedef enum {
    PC_EXEC = 0,
    PC_ROOT_OPENED,
    PC_BOOT,
    PC_BOOT_STARTED,
    PC_BOOT_DRAINED,
    PC_BOOT_CHECK,
    PC_WAIT,
    PC_WAITED,
    PC_STOPPED_DRAINED,
    PC_DEADLINE_DRAINED,
    PC_AFFECTED_DRAIN,
    PC_AFFECTED_DRAINED,
    PC_AFFECTED_DONE,
    PC_BACKED_OFF,
    PC_RESTART,
    PC_RESTART_STARTED,
    PC_RESTART_DRAINED,
    PC_RESTART_CHECK,
    PC_FINISH,
    PC_FINISH_DRAIN,
    PC_FINISH_DRAINED,
    PC_FINISH_CLOSE,
    PC_FINISH_CLOSED,
    PC_FINISH_END,
    PC_DONE
} sup_pc;

typedef struct {
    uint32_t generation;
    int in_use;
    /* Configuration. */
    char name[ASX_SUPERVISOR_NAME_MAX];
    asx_supervisor_strategy strategy;
    uint32_t max_restarts;
    uint64_t window_ns;
    asx_restart_backoff backoff;
    asx_supervisor_escalation escalation;
    int has_budget;
    asx_budget budget;
    uint32_t n;
    sup_child children[SUP_CHILDREN];
    /* The controller. */
    asx_task_id task;
    uint32_t refusal; /* its spawn's refusal ticket, 0 for none */
    asx_region_id owner_region;
    asx_cx cx;
    asx_region_id root;
    int root_held;
    sup_gen running[SUP_CHILDREN];
    sup_latest latest[SUP_CHILDREN];
    uint64_t numbers[SUP_CHILDREN];
    /* Terminals waiting to be handled (Rust Controller::ready). */
    uint32_t ready[SUP_CHILDREN];
    asx_supervisor_generation ready_id[SUP_CHILDREN];
    uint32_t n_ready;
    /* Restart batches in the window (Rust RestartHistory). */
    uint64_t restarts[ASX_SUPERVISOR_MAX_RESTARTS];
    uint32_t n_restarts;
    asx_supervisor_report report;
    /* Where the controller resumes. */
    sup_pc pc;
    sup_sub sub;
    uint8_t sub_step;
    uint32_t sub_pos;
    sup_err sub_err;
    int sub_flag;       /* SUB_BACKOFF: it slept to the end */
    uint32_t sub_found; /* SUB_WAIT_EXIT: the ended child, or SUP_NONE */
    asx_region_id sub_region;
    uint64_t sub_number;
    asx_time sub_deadline;
    uint32_t boot_pos;
    uint32_t failed;
    uint32_t affected[SUP_CHILDREN];
    uint32_t n_affected;
    uint32_t aff_pos;
    int cancelled_ok;
    int drained_ok;
    int has_delay;
    uint64_t delay_ns;
    uint32_t restart[SUP_CHILDREN];
    uint32_t n_restart;
    uint32_t rs_pos;
    int counted;
    uint32_t fin_pos;
    /* The handle's side. */
    int finished; /* the report is published */
    int joined;
    asx_cancel_reason abort_reason;
    asx_cancel_reason escalate_reason;
} sup_slot;

static sup_slot g_sups[ASX_MAX_SUPERVISORS];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t next_gen(uint32_t g) {
    g++;
    if (g == 0u) g = 1u;
    return g;
}

static asx_time sup_now(void) {
    asx_time now;
    if (asx_runtime_now_ns(&now) != ASX_OK) now = asx_runtime_virtual_now();
    return now;
}

static sup_slot *sup_lookup(asx_supervisor_handle h) {
    sup_slot *s;
    if (h.slot >= ASX_MAX_SUPERVISORS) return NULL;
    s = &g_sups[h.slot];
    if (!s->in_use || s->generation != h.generation) return NULL;
    return s;
}

/* Its controller published its report, was joined, or its task is gone
 * or completed (it never ran: refused or cancelled before its first poll). */
static int sup_ended(const sup_slot *s) {
    asx_task_state ts;
    if (s->finished || s->joined) return 1;
    if (s->task == ASX_INVALID_ID) return asx_task_refusal_delivered(s->refusal);
    return asx_task_get_state(s->task, &ts) != ASX_OK || ts == ASX_TASK_COMPLETED;
}

static const char *outcome_word(asx_outcome_severity o) {
    switch (o) {
    case ASX_OUTCOME_OK: return "ok";
    case ASX_OUTCOME_ERR: return "err";
    case ASX_OUTCOME_CANCELLED: return "cancelled";
    case ASX_OUTCOME_PANICKED: return "panicked";
    }
    return "pending";
}

/* Handles carry a state bitmask, so one entity can appear under different
 * values: its identity is the generation-tagged slot. */
static int same_id(uint64_t a, uint64_t b) { return asx_handle_index(a) == asx_handle_index(b); }

static int same_generation(const asx_supervisor_generation *a, const asx_supervisor_generation *b) {
    return a->number == b->number && same_id(a->region, b->region) && same_id(a->task, b->task);
}

/* Controller::trace (:2458-2480). Names are printed as given (Rust prints
 * their Debug form, the same for names that need no escaping). */
static void sup_trace(const sup_slot *s, const char *action, uint32_t pos,
                      const asx_supervisor_generation *id) {
    char buf[96 + 2 * ASX_SUPERVISOR_NAME_MAX + 3 * 24];
    const char *outcome = "pending";
    if (s->latest[pos].present && same_generation(&s->latest[pos].c.generation, id)) {
        outcome = outcome_word(s->latest[pos].c.outcome);
    }
    if (snprintf(buf, sizeof(buf),
                 "managed_supervisor_v1 action=%s supervisor=\"%s\" child=\"%s\" "
                 "generation=%llu region=0x%016llx task=0x%016llx outcome=%s",
                 action, s->name, s->children[pos].name, (unsigned long long)id->number,
                 (unsigned long long)id->region, (unsigned long long)id->task, outcome) < 0) {
        return;
    }
    asx_trace_user(s->task, buf);
}

/* Controller::cancelled (:2389): a checkpoint of the controller's Cx. */
static int cancelled(sup_slot *s) { return asx_cx_checkpoint(&s->cx) != ASX_OK; }

static void set_err(sup_slot *s, asx_supervisor_error kind, uint32_t child, asx_status status) {
    s->sub_err.kind = kind;
    s->sub_err.child = child;
    s->sub_err.status = status;
}

static void report_err(sup_slot *s, const sup_err *e) {
    s->report.outcome = ASX_OUTCOME_ERR;
    s->report.error = e->kind;
    s->report.error_child = e->child == SUP_NONE ? SUP_NONE : s->children[e->child].spec_index;
    s->report.error_status = e->status;
}

static void report_err_at(sup_slot *s, asx_supervisor_error kind, uint32_t pos) {
    sup_err e;
    e.kind = kind;
    e.child = pos;
    e.status = ASX_OK;
    report_err(s, &e);
}

/* Controller::record_error (:2429): a panic already recorded stays. */
static void record_error(sup_slot *s, const sup_err *e) {
    if (s->report.outcome != ASX_OUTCOME_PANICKED) report_err(s, e);
}

/* Controller::record_cancel (:2419): the controller's own reason. */
static void record_cancel(sup_slot *s) {
    asx_cancel_reason r;
    if (s->report.outcome == ASX_OUTCOME_PANICKED) return;
    if (asx_task_get_cancel_reason(s->task, &r) != ASX_OK) {
        r = asx_cancel_reason_default(ASX_CANCEL_USER, "managed supervisor cancelled");
    }
    s->report.outcome = ASX_OUTCOME_CANCELLED;
    s->report.error = ASX_SUPERVISOR_ERR_NONE;
    s->report.cancel_reason = r;
}

/* Controller::deadline_passed (:2396) and ::record_deadline (:2405). */
static int deadline_passed(const sup_slot *s) {
    return s->has_budget && asx_budget_is_past_deadline(&s->budget, sup_now());
}

static void record_deadline(sup_slot *s) {
    asx_cancel_reason r;
    if (s->report.outcome != ASX_OUTCOME_OK) return;
    r = asx_cancel_reason_default(ASX_CANCEL_DEADLINE, "managed supervisor budget deadline passed");
    r.origin_region = s->report.region != ASX_INVALID_ID ? s->report.region : s->owner_region;
    r.timestamp = sup_now();
    s->report.outcome = ASX_OUTCOME_CANCELLED;
    s->report.cancel_reason = r;
}

/* Wake `task` through the waker of the poll that registered it. */
static void wake_at(asx_task_id task, uint8_t prio) {
    asx_task_slot *t;
    if (asx_task_slot_lookup(task, &t) != ASX_OK) return;
    if (asx_lab_dispatch_active()) {
        if (t->alive && !asx_task_is_terminal(t->state)) asx_lab_schedule(t, prio);
    } else {
        asx_task_wake_slot_internal(t);
    }
}

static void park(asx_task_id self) {
    asx_status st = asx_task_park(self);
    (void)st; /* only called from the controller's own poll */
}

/* The restart history (RestartHistory, :5310-5420): batches recorded in
 * the window ending now. */
static uint64_t window_cutoff(const sup_slot *s, uint64_t now) {
    return now > s->window_ns ? now - s->window_ns : 0u;
}

static uint32_t recent_count(const sup_slot *s, uint64_t now) {
    uint64_t cutoff = window_cutoff(s, now);
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < s->n_restarts; i++) {
        if (s->restarts[i] >= cutoff) n++;
    }
    return n;
}

static void record_restart(sup_slot *s, uint64_t now) {
    uint64_t cutoff = window_cutoff(s, now);
    uint32_t i;
    uint32_t kept = 0;
    for (i = 0; i < s->n_restarts; i++) {
        if (s->restarts[i] >= cutoff) s->restarts[kept++] = s->restarts[i];
    }
    s->n_restarts = kept;
    /* An allowed batch leaves fewer than max_restarts in the window. */
    if (s->n_restarts < ASX_SUPERVISOR_MAX_RESTARTS) s->restarts[s->n_restarts++] = now;
}

/* BackoffStrategy::delay_for_attempt (:5265-5302). */
static int backoff_delay(const asx_restart_backoff *b, uint32_t attempt, uint64_t *out) {
    uint64_t d;
    uint32_t exp;
    uint32_t i;
    switch (b->kind) {
    case ASX_RESTART_BACKOFF_FIXED: *out = b->initial_ns; return 1;
    case ASX_RESTART_BACKOFF_EXPONENTIAL:
        exp = attempt < 30u ? attempt : 30u;
        d = b->initial_ns;
        for (i = 0; i < exp && d < b->max_ns; i++) {
            if (b->multiplier == 0u) {
                d = 0;
                break;
            }
            d = d > UINT64_MAX / b->multiplier ? UINT64_MAX : d * b->multiplier;
        }
        *out = d < b->max_ns ? d : b->max_ns;
        return 1;
    case ASX_RESTART_BACKOFF_NONE: break;
    }
    return 0;
}

/* ManagedRestartMode::eligible (:1833-1844). */
static int eligible(asx_child_restart mode, const asx_supervisor_completion *c) {
    switch (mode) {
    case ASX_CHILD_PERMANENT: return 1;
    case ASX_CHILD_TRANSIENT:
        return c->outcome == ASX_OUTCOME_PANICKED || c->task_outcome == ASX_OUTCOME_PANICKED ||
               (c->task_outcome == ASX_OUTCOME_OK && c->outcome == ASX_OUTCOME_ERR);
    case ASX_CHILD_TEMPORARY: break;
    }
    return 0;
}

/* Controller::dependency_unavailable (:2904): the first dependency not
 * running, SUP_NONE if all are. */
static uint32_t dependency_unavailable(const sup_slot *s, uint32_t pos) {
    uint32_t i;
    for (i = 0; i < s->children[pos].dep_count; i++) {
        if (!s->running[s->children[pos].deps[i]].present) return s->children[pos].deps[i];
    }
    return SUP_NONE;
}

static asx_supervisor_generation gen_identity(const sup_slot *s, uint32_t pos) {
    asx_supervisor_generation id;
    const sup_gen *g = &s->running[pos];
    id.child = s->children[pos].spec_index;
    id.number = g->number;
    id.region = g->region;
    id.task = g->has_identity ? g->identity_task : g->task;
    return id;
}

/* ------------------------------------------------------------------ */
/* A generation's task (:2646-2726)                                    */
/* ------------------------------------------------------------------ */

static void *gen_code(const sup_slot *s, uint32_t pos) {
    return (void *)(uintptr_t)((uint32_t)(s - g_sups) * SUP_CHILDREN + pos);
}

/* The start wakes the controller through the waker its start wait last
 * registered (the publication's waiter). */
static void gen_wake_waiter(sup_gen *g) {
    if (!g->has_waiter) return;
    g->has_waiter = 0;
    wake_at(g->waiter, g->waiter_prio);
}

static void gen_publish(sup_gen *g, asx_outcome_severity outcome, asx_status status,
                        const char *panic_message) {
    g->has_terminal = 1;
    g->terminal_at = sup_now();
    g->terminal_outcome = outcome;
    g->terminal_status = status;
    g->terminal_panic = panic_message;
    g->terminal_shutdown = g->shutdown_requested;
}

static asx_status gen_poll(void *user_data, asx_task_id self) {
    uint32_t code = (uint32_t)(uintptr_t)user_data;
    uint32_t slot = code / SUP_CHILDREN;
    uint32_t pos = code % SUP_CHILDREN;
    sup_slot *s;
    sup_gen *g;
    asx_task_slot *t;
    const char *message;
    asx_status st;

    if (slot >= ASX_MAX_SUPERVISORS) return ASX_E_INVALID_STATE;
    s = &g_sups[slot];
    g = &s->running[pos];
    /* A task its generation no longer owns has nothing to run. */
    if (!s->in_use || !g->present || !same_id(g->task, self) || g->has_terminal) return ASX_OK;
    if (!g->constructed) {
        asx_supervisor_generation id;
        g->constructed = 1;
        g->has_identity = 1;
        g->identity_task = self;
        id = gen_identity(s, pos);
        g->body = NULL;
        g->body_data = NULL;
        st = s->children[pos].start(s->children[pos].user_data, &id, &g->body, &g->body_data);
        g->started = 1;
        gen_wake_waiter(g);
        if (st != ASX_OK || g->body == NULL) {
            gen_publish(g, ASX_OUTCOME_PANICKED, ASX_OK, "child start function failed");
            return ASX_OK;
        }
    }
    st = g->body(g->body_data, self);
    /* The body's panic is caught (CatchUnwind, :2700): the generation's
     * task completes normally. */
    if (asx_task_slot_lookup(self, &t) == ASX_OK && asx_task_catch_panic_internal(t, &message)) {
        gen_publish(g, ASX_OUTCOME_PANICKED, ASX_OK, message);
        return ASX_OK;
    }
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    gen_publish(g, st == ASX_OK ? ASX_OUTCOME_OK : ASX_OUTCOME_ERR, st, NULL);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* The controller's view of a generation                               */
/* ------------------------------------------------------------------ */

/* A joined generation's task result (Rust TaskHandle::poll_join). */
typedef struct {
    asx_outcome_severity outcome;
    asx_cancel_reason reason;
    const char *panic_message;
    uint64_t task_key;
} sup_join;

/* Poll the join of a generation's task: 1 when it completed (its result
 * in *out, its slot released), else 0 with the controller registered to
 * be woken when it completes, at this poll's priority. */
static int gen_poll_join(sup_slot *s, uint32_t pos, asx_task_id self, sup_join *out) {
    sup_gen *g = &s->running[pos];
    asx_task_slot *t;
    asx_outcome o;
    asx_status st;

    if (asx_task_slot_lookup(g->task, &t) != ASX_OK) {
        /* Gone without a join: nothing the controller started does that. */
        out->outcome = ASX_OUTCOME_CANCELLED;
        out->reason = asx_cancel_reason_default(ASX_CANCEL_USER, NULL);
        out->panic_message = NULL;
        out->task_key = (uint64_t)g->task;
        return 1;
    }
    if (t->state != ASX_TASK_COMPLETED) {
        st = asx_task_watch(g->task, self);
        (void)st; /* both tasks are live */
        return 0;
    }
    out->outcome = t->outcome.severity;
    out->panic_message = t->panic_message;
    out->task_key = asx_lab_dispatch_active()
                        ? ((uint64_t)t->lab_rust_index << 32) | (uint64_t)t->lab_rust_gen
                        : (uint64_t)g->task;
    if (out->outcome != ASX_OUTCOME_CANCELLED ||
        asx_task_get_cancel_reason(g->task, &out->reason) != ASX_OK) {
        memset(&out->reason, 0, sizeof(out->reason));
    }
    st = asx_task_join(g->task, &o);
    (void)st; /* completed: the join consumes it */
    return 1;
}

/* Controller::observe_start (:2482). */
static void observe_start(sup_slot *s, uint32_t pos) {
    sup_gen *g = &s->running[pos];
    asx_supervisor_generation id;
    if (!g->present || g->start_observed || !g->started) return;
    g->start_observed = 1;
    s->report.started++;
    id = gen_identity(s, pos);
    sup_trace(s, "started", pos, &id);
}

/* Controller::joined (:2518-2586): the completion the controller keeps. */
static void gen_joined(sup_slot *s, uint32_t pos, const sup_join *j) {
    sup_gen *g = &s->running[pos];
    sup_latest *l = &s->latest[pos];
    asx_supervisor_completion c;

    observe_start(s, pos);
    g->terminal_observed = 1;
    g->handle_held = 0;
    memset(&c, 0, sizeof(c));
    c.child = s->children[pos].spec_index;
    c.generation = gen_identity(s, pos);
    c.task_outcome = j->outcome;
    if (j->outcome == ASX_OUTCOME_PANICKED) {
        c.completed_at = sup_now();
        c.outcome = ASX_OUTCOME_PANICKED;
        c.panic_message = j->panic_message;
        c.shutdown_requested_before_completion = g->shutdown_requested;
    } else if (g->has_terminal) {
        /* A later cancel of the task cannot rewrite a published end. */
        c.completed_at = g->terminal_at;
        c.outcome = g->terminal_outcome;
        c.status = g->terminal_status;
        c.panic_message = g->terminal_panic;
        c.shutdown_requested_before_completion = g->terminal_shutdown;
    } else if (j->outcome == ASX_OUTCOME_CANCELLED) {
        c.completed_at = sup_now();
        c.outcome = ASX_OUTCOME_CANCELLED;
        c.cancel_reason = j->reason;
        c.shutdown_requested_before_completion = g->shutdown_requested;
    } else {
        c.completed_at = sup_now();
        c.outcome = ASX_OUTCOME_PANICKED;
        c.panic_message = "managed child returned without its terminal publication";
        c.shutdown_requested_before_completion = g->shutdown_requested;
    }
    g->has_terminal = 0;
    s->report.joined++;
    l->present = 1;
    l->c = c;
    l->task_key = j->task_key;
    sup_trace(s, "terminal", pos, &c.generation);
}

static void queue_terminal(sup_slot *s, uint32_t pos) {
    if (s->n_ready >= SUP_CHILDREN) return; /* one per child: never full */
    s->ready[s->n_ready] = pos;
    s->ready_id[s->n_ready] = s->latest[pos].c.generation;
    s->n_ready++;
}

/* Controller::accepts_terminal (:2508). */
static int accepts_terminal(const sup_slot *s, uint32_t pos, const asx_supervisor_generation *id) {
    const sup_gen *g = &s->running[pos];
    return g->present && g->terminal_observed && g->number == id->number &&
           s->latest[pos].present && same_generation(&s->latest[pos].c.generation, id);
}

/* RunningChild::cancel (:2219-2239): the generation's region, cancelled
 * once with the drain reason. */
static asx_status gen_cancel(sup_slot *s, uint32_t pos) {
    sup_gen *g = &s->running[pos];
    asx_cancel_reason r;
    if (g->cancellation_sent) return ASX_OK;
    g->cancellation_sent = 1;
    g->shutdown_requested = 1;
    if (!g->region_held) return ASX_OK;
    r = asx_cancel_reason_default(ASX_CANCEL_USER, "managed supervisor generation drain");
    r.origin_region = g->region;
    r.timestamp = sup_now();
    r.origin_task = g->handle_held ? g->task : ASX_INVALID_ID;
    return asx_region_cancel_request(g->region, &r);
}

/* Controller::cancel_children (:2891), over positions in the order given. */
static int cancel_children(sup_slot *s, const uint32_t *positions, uint32_t n) {
    int accepted = 1;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t pos = positions[i];
        asx_status st;
        if (!s->running[pos].present) continue;
        st = gen_cancel(s, pos);
        if (st != ASX_OK) {
            sup_err e;
            e.kind = ASX_SUPERVISOR_ERR_REGION;
            e.child = SUP_NONE;
            e.status = st;
            accepted = 0;
            record_error(s, &e);
        }
    }
    return accepted;
}

/* Controller::escalate (:3015-3062): cancel the controller's own region. */
static void escalate(sup_slot *s, uint32_t failed) {
    asx_supervisor_generation id = s->latest[failed].c.generation;
    asx_status st;
    if (s->report.escalations != 0u) return;
    s->escalate_reason =
        asx_cancel_reason_default(ASX_CANCEL_FAIL_FAST,
                                  "managed supervisor restart intensity exhausted");
    s->escalate_reason.origin_region = id.region;
    s->escalate_reason.origin_task = id.task;
    s->escalate_reason.timestamp = sup_now();
    st = asx_region_cancel_request(s->owner_region, &s->escalate_reason);
    if (st == ASX_OK) {
        s->report.escalations = 1u;
        sup_trace(s, "parent_escalated", failed, &id);
    } else {
        s->report.outcome = ASX_OUTCOME_ERR;
        s->report.error = ASX_SUPERVISOR_ERR_ESCALATION;
        s->report.error_child = SUP_NONE;
        s->report.error_status = st;
    }
}

/* ------------------------------------------------------------------ */
/* Sub-operations: ASX_E_PENDING, or done with their result set         */
/* ------------------------------------------------------------------ */

static asx_status sub_open_root(sup_slot *s, asx_task_id self) {
    asx_region_id r = ASX_INVALID_ID;
    asx_status st =
        asx_region_open_child_poll(self, s->owner_region, s->has_budget ? &s->budget : NULL, &r);
    if (st == ASX_E_PENDING) return st;
    if (st != ASX_OK) {
        set_err(s, ASX_SUPERVISOR_ERR_REGION, SUP_NONE, st);
        return ASX_OK;
    }
    s->sub_region = r;
    return ASX_OK;
}

enum { ST_BEGIN = 0, ST_OPEN, ST_CLOSE_CANCELLED, ST_WAIT, ST_AFTER };

/* Controller::start (:2588-2831). */
static asx_status sub_start(sup_slot *s, asx_task_id self) {
    uint32_t pos = s->sub_pos;
    sup_gen *g = &s->running[pos];
    asx_status st;
    for (;;) {
        switch (s->sub_step) {
        case ST_BEGIN:
            if (cancelled(s)) {
                record_cancel(s);
                return ASX_OK;
            }
            if (s->numbers[pos] == UINT64_MAX) {
                set_err(s, ASX_SUPERVISOR_ERR_GENERATION_EXHAUSTED, pos, ASX_OK);
                return ASX_OK;
            }
            s->sub_number = s->numbers[pos] + 1u;
            s->sub_step = ST_OPEN;
            break;
        case ST_OPEN: {
            asx_region_id r = ASX_INVALID_ID;
            asx_task_id tid = ASX_INVALID_ID;
            asx_task_slot *ct;
            asx_budget inherited;
            /* Opened through the supervisor region's principal
             * (root.cx()), whose inherited budget is that region's. */
            st = asx_task_slot_lookup(self, &ct);
            if (st == ASX_OK) st = asx_region_get_budget(s->root, &inherited);
            if (st == ASX_OK) st = asx_region_open_child_poll_internal(ct, s->root, &inherited, &r);
            if (st == ASX_E_PENDING) return st;
            if (st != ASX_OK) {
                set_err(s, ASX_SUPERVISOR_ERR_REGION, SUP_NONE, st);
                return ASX_OK;
            }
            s->sub_region = r;
            if (cancelled(s)) {
                record_cancel(s);
                s->sub_step = ST_CLOSE_CANCELLED;
                break;
            }
            st = asx_task_spawn(r, gen_poll, gen_code(s, pos), &tid);
            if (st != ASX_OK) {
                /* Rust's spawn goes through the mailbox, and only its
                 * admission refuses a child, for a region that is closing;
                 * the checkpoint just above leaves no window for that (the
                 * region can only be cancelled through the controller's),
                 * so a failure here is the gateway's own, a SpawnError. The
                 * dropped ChildRegion requests its close. */
                asx_status cs = asx_region_close_request(r);
                (void)cs;
                set_err(s, ASX_SUPERVISOR_ERR_SPAWN, SUP_NONE, st);
                return ASX_OK;
            }
            s->numbers[pos] = s->sub_number;
            memset(g, 0, sizeof(*g));
            g->present = 1;
            g->number = s->sub_number;
            g->region = r;
            g->region_held = 1;
            g->task = tid;
            g->handle_held = 1;
            s->sub_step = ST_WAIT;
            break;
        }
        case ST_CLOSE_CANCELLED:
            st = asx_region_close_poll(self, s->sub_region);
            if (st == ASX_E_PENDING) return st;
            if (st != ASX_OK) set_err(s, ASX_SUPERVISOR_ERR_REGION, SUP_NONE, st);
            return ASX_OK;
        case ST_WAIT: {
            sup_join j;
            asx_task_slot *ct;
            if (cancelled(s)) {
                record_cancel(s);
                s->sub_step = ST_AFTER;
                break;
            }
            if (gen_poll_join(s, pos, self, &j)) {
                gen_joined(s, pos, &j);
                queue_terminal(s, pos);
                s->sub_step = ST_AFTER;
                break;
            }
            if (asx_task_slot_lookup(self, &ct) == ASX_OK) {
                g->has_waiter = 1;
                g->waiter = self;
                g->waiter_prio = ct->lab_waker_prio;
            }
            if (g->started) {
                observe_start(s, pos);
                s->sub_step = ST_AFTER;
                break;
            }
            park(self);
            return ASX_E_PENDING;
        }
        default: /* ST_AFTER */
            if (cancelled(s)) {
                record_cancel(s);
                return ASX_OK;
            }
            if (g->terminal_observed && !g->start_observed) {
                set_err(s, ASX_SUPERVISOR_ERR_CHILD_NOT_STARTED, pos, ASX_OK);
            }
            return ASX_OK;
        }
    }
}

enum { DR_BEGIN = 0, DR_JOIN, DR_TAKE, DR_CLOSE };

/* Controller::drain (:2833-2889). */
static asx_status sub_drain(sup_slot *s, asx_task_id self) {
    uint32_t pos = s->sub_pos;
    sup_gen *g = &s->running[pos];
    asx_status st;
    for (;;) {
        switch (s->sub_step) {
        case DR_BEGIN:
            if (!g->present) return ASX_OK;
            st = gen_cancel(s, pos);
            if (st != ASX_OK) {
                set_err(s, ASX_SUPERVISOR_ERR_REGION, SUP_NONE, st);
                return ASX_OK;
            }
            s->sub_step = g->handle_held ? DR_JOIN : DR_TAKE;
            break;
        case DR_JOIN: {
            sup_join j;
            if (!gen_poll_join(s, pos, self, &j)) {
                park(self);
                return ASX_E_PENDING;
            }
            gen_joined(s, pos, &j);
            s->sub_step = DR_TAKE;
            break;
        }
        case DR_TAKE:
            s->sub_region = g->region;
            g->present = 0;
            g->region_held = 0;
            s->sub_step = DR_CLOSE;
            break;
        default: /* DR_CLOSE */
            st = asx_region_close_poll(self, s->sub_region);
            if (st == ASX_E_PENDING) return st;
            if (st != ASX_OK) {
                set_err(s, ASX_SUPERVISOR_ERR_REGION, SUP_NONE, st);
                return ASX_OK;
            }
            sup_trace(s, "drained", pos, &s->latest[pos].c.generation);
            return ASX_OK;
        }
    }
}

/* Controller::wait_exit (:2920-2982): the next ended child to handle, in
 * (end time, task) order, or none once nothing runs. The C runtime wakes
 * the controller for each completion it registered for, so the
 * termination tally Rust rescans with has nothing to add. */
static asx_status sub_wait_exit(sup_slot *s, asx_task_id self) {
    uint32_t pos;
    uint32_t i;
    uint32_t kept;
    s->sub_found = SUP_NONE;
    if (cancelled(s)) {
        record_cancel(s);
        return ASX_OK;
    }
    if (s->report.outcome != ASX_OUTCOME_OK) return ASX_OK;
    for (pos = 0; pos < s->n; pos++) {
        sup_join j;
        if (s->running[pos].present && s->running[pos].handle_held &&
            gen_poll_join(s, pos, self, &j)) {
            gen_joined(s, pos, &j);
            queue_terminal(s, pos);
        }
    }
    if (s->report.outcome != ASX_OUTCOME_OK) return ASX_OK;
    kept = 0;
    for (i = 0; i < s->n_ready; i++) {
        if (accepts_terminal(s, s->ready[i], &s->ready_id[i])) {
            s->ready[kept] = s->ready[i];
            s->ready_id[kept] = s->ready_id[i];
            kept++;
        }
    }
    s->n_ready = kept;
    /* A stable insertion sort by (completed_at, task). */
    for (i = 1; i < s->n_ready; i++) {
        uint32_t p = s->ready[i];
        asx_supervisor_generation pid = s->ready_id[i];
        uint32_t k = i;
        while (k > 0u) {
            const sup_latest *a = &s->latest[s->ready[k - 1u]];
            const sup_latest *b = &s->latest[p];
            if (a->c.completed_at < b->c.completed_at ||
                (a->c.completed_at == b->c.completed_at && a->task_key <= b->task_key)) {
                break;
            }
            s->ready[k] = s->ready[k - 1u];
            s->ready_id[k] = s->ready_id[k - 1u];
            k--;
        }
        s->ready[k] = p;
        s->ready_id[k] = pid;
    }
    if (s->n_ready > 0u) {
        s->sub_found = s->ready[0];
        for (i = 1; i < s->n_ready; i++) {
            s->ready[i - 1u] = s->ready[i];
            s->ready_id[i - 1u] = s->ready_id[i];
        }
        s->n_ready--;
        return ASX_OK;
    }
    for (pos = 0; pos < s->n; pos++) {
        if (s->running[pos].present) {
            park(self);
            return ASX_E_PENDING;
        }
    }
    return ASX_OK;
}

enum { BO_BEGIN = 0, BO_SLEEP, BO_END };

/* Controller::backoff (:2984-3013): 1 in sub_flag when it slept through
 * without the controller being cancelled. */
static asx_status sub_backoff(sup_slot *s, asx_task_id self) {
    asx_status st;
    for (;;) {
        switch (s->sub_step) {
        case BO_BEGIN:
            s->sub_flag = 0;
            if (cancelled(s)) {
                record_cancel(s);
                return ASX_OK;
            }
            if (s->has_delay && s->delay_ns > 0u) {
                asx_time now = sup_now();
                s->sub_deadline = s->delay_ns > UINT64_MAX - now ? UINT64_MAX : now + s->delay_ns;
                s->sub_step = BO_SLEEP;
            } else {
                s->sub_step = BO_END;
            }
            break;
        case BO_SLEEP:
            if (cancelled(s)) {
                /* The pending sleep is dropped: its timer is cancelled. */
                record_cancel(s);
                st = asx_task_cancel_timer(self);
                (void)st;
                return ASX_OK;
            }
            st = asx_task_wait_until(self, s->sub_deadline);
            if (st == ASX_E_PENDING) return st;
            st = asx_task_complete_timer(self);
            (void)st;
            s->sub_step = BO_END;
            break;
        default: /* BO_END */
            if (cancelled(s)) {
                record_cancel(s);
                return ASX_OK;
            }
            s->sub_flag = 1;
            return ASX_OK;
        }
    }
}

static asx_status sub_close_root(sup_slot *s, asx_task_id self) {
    asx_status st = asx_region_close_poll(self, s->root);
    if (st == ASX_E_PENDING) return st;
    s->root_held = 0;
    if (st != ASX_OK) set_err(s, ASX_SUPERVISOR_ERR_REGION, SUP_NONE, st);
    return ASX_OK;
}

static asx_status run_sub(sup_slot *s, asx_task_id self) {
    switch (s->sub) {
    case SUB_OPEN_ROOT: return sub_open_root(s, self);
    case SUB_START: return sub_start(s, self);
    case SUB_DRAIN: return sub_drain(s, self);
    case SUB_WAIT_EXIT: return sub_wait_exit(s, self);
    case SUB_BACKOFF: return sub_backoff(s, self);
    case SUB_CLOSE_ROOT: return sub_close_root(s, self);
    case SUB_NONE: break;
    }
    return ASX_OK;
}

/* Begin a sub-operation; the controller continues at `then` once it is
 * done. */
static void call(sup_slot *s, sup_sub sub, uint32_t pos, sup_pc then) {
    s->sub = sub;
    s->sub_step = 0;
    s->sub_pos = pos;
    s->sub_err.kind = ASX_SUPERVISOR_ERR_NONE;
    s->sub_err.child = SUP_NONE;
    s->sub_err.status = ASX_OK;
    s->sub_flag = 0;
    s->sub_found = SUP_NONE;
    s->pc = then;
}

static int sub_failed(const sup_slot *s) { return s->sub_err.kind != ASX_SUPERVISOR_ERR_NONE; }

/* ------------------------------------------------------------------ */
/* The controller (Controller::execute and ::finish, :3064-3387)        */
/* ------------------------------------------------------------------ */

/* A start that failed: a required child stops the supervisor, an optional
 * one is drained. */
static void after_start(sup_slot *s, uint32_t pos, sup_pc drained, sup_pc check) {
    if (!sub_failed(s)) {
        s->pc = check;
    } else if (s->children[pos].required) {
        record_error(s, &s->sub_err);
        s->pc = PC_FINISH;
    } else {
        call(s, SUB_DRAIN, pos, drained);
    }
}

/* A failed child the restart intensity allows to be replaced: cancel the
 * children the strategy names, then drain them in reverse order. */
static void begin_restart(sup_slot *s, uint32_t failed, uint64_t now) {
    uint32_t pos;
    uint32_t reversed[SUP_CHILDREN] = {0};
    uint32_t i;
    s->has_delay = backoff_delay(&s->backoff, recent_count(s, now), &s->delay_ns);
    s->n_affected = 0;
    for (pos = 0; pos < s->n; pos++) {
        int named;
        if (!s->running[pos].present) continue;
        switch (s->strategy) {
        case ASX_SUPERVISOR_ONE_FOR_ALL: named = 1; break;
        case ASX_SUPERVISOR_REST_FOR_ONE: named = pos >= failed; break;
        case ASX_SUPERVISOR_ONE_FOR_ONE:
        default: named = pos == failed; break;
        }
        if (named) s->affected[s->n_affected++] = pos;
    }
    for (i = 0; i < s->n_affected; i++) reversed[i] = s->affected[s->n_affected - 1u - i];
    s->cancelled_ok = cancel_children(s, reversed, s->n_affected);
    s->drained_ok = 1;
    s->aff_pos = s->n_affected;
    s->pc = PC_AFFECTED_DRAIN;
}

/* The ended child from wait_exit (:3113-3162). */
static void handle_exit(sup_slot *s) {
    uint32_t failed = s->sub_found;
    uint64_t now;
    int allowed;
    if (failed == SUP_NONE) {
        s->pc = PC_FINISH;
        return;
    }
    s->failed = failed;
    if (!eligible(s->children[failed].restart, &s->latest[failed].c)) {
        call(s, SUB_DRAIN, failed, PC_STOPPED_DRAINED);
        return;
    }
    if (deadline_passed(s)) {
        call(s, SUB_DRAIN, failed, PC_DEADLINE_DRAINED);
        return;
    }
    now = sup_now();
    allowed = recent_count(s, now) < s->max_restarts;
    if (!allowed && s->escalation == ASX_ESCALATION_RESET_COUNTER) {
        s->n_restarts = 0;
        allowed = recent_count(s, now) < s->max_restarts;
    }
    if (allowed) {
        begin_restart(s, failed, now);
        return;
    }
    if (s->escalation == ASX_ESCALATION_STOP) {
        call(s, SUB_DRAIN, failed, PC_STOPPED_DRAINED);
        return;
    }
    report_err_at(s, ASX_SUPERVISOR_ERR_RESTART_LIMIT, failed);
    if (s->escalation == ASX_ESCALATION_ESCALATE) escalate(s, failed);
    s->pc = PC_FINISH;
}

/* The drained children to start again (:3240-3250): not TEMPORARY, and
 * shut down before they ended or eligible themselves. */
static void restart_set(sup_slot *s) {
    uint32_t i;
    s->n_restart = 0;
    for (i = 0; i < s->n_affected; i++) {
        uint32_t pos = s->affected[i];
        const asx_supervisor_completion *c = &s->latest[pos].c;
        if (s->children[pos].restart != ASX_CHILD_TEMPORARY &&
            (c->shutdown_requested_before_completion || eligible(s->children[pos].restart, c))) {
            s->restart[s->n_restart++] = pos;
        }
    }
}

/* One replacement of the batch (:3262-3327). */
static void restart_next(sup_slot *s) {
    uint32_t pos;
    if (s->rs_pos >= s->n_restart) {
        s->pc = PC_WAIT;
        return;
    }
    pos = s->restart[s->rs_pos];
    if (cancelled(s)) {
        record_cancel(s);
        s->pc = PC_FINISH;
        return;
    }
    if (deadline_passed(s)) {
        record_deadline(s);
        s->pc = PC_FINISH;
        return;
    }
    if (dependency_unavailable(s, pos) != SUP_NONE) {
        sup_trace(s, "restart_dependency_unavailable", pos, &s->latest[pos].c.generation);
        if (s->children[pos].required) {
            report_err_at(s, ASX_SUPERVISOR_ERR_DEPENDENCY_UNAVAILABLE, pos);
            s->pc = PC_FINISH;
            return;
        }
        s->rs_pos++;
        return;
    }
    if (!s->counted) {
        record_restart(s, sup_now());
        s->report.restart_batches++;
        s->counted = 1;
    }
    call(s, SUB_START, pos, PC_RESTART_STARTED);
}

/* Publish the report (the end of finish, :3378-3386). */
static void publish(sup_slot *s) {
    uint32_t pos;
    if (s->report.outcome == ASX_OUTCOME_OK && cancelled(s)) record_cancel(s);
    s->report.completion_count = 0;
    for (pos = 0; pos < s->n; pos++) {
        if (s->latest[pos].present) {
            s->report.completions[s->report.completion_count++] = s->latest[pos].c;
            s->latest[pos].present = 0;
        }
    }
    s->finished = 1;
    s->pc = PC_DONE;
}

static asx_status ctl_poll(void *user_data, asx_task_id self) {
    uint32_t idx = (uint32_t)(uintptr_t)user_data;
    sup_slot *s;

    if (idx >= ASX_MAX_SUPERVISORS) return ASX_E_INVALID_STATE;
    s = &g_sups[idx];
    if (!s->in_use || !same_id(s->task, self) || s->pc == PC_DONE) return ASX_E_INVALID_STATE;
    for (;;) {
        uint32_t pos;
        if (s->sub != SUB_NONE) {
            if (run_sub(s, self) == ASX_E_PENDING) return ASX_E_PENDING;
            s->sub = SUB_NONE;
        }
        switch (s->pc) {
        case PC_EXEC:
            if (cancelled(s)) {
                record_cancel(s);
                s->pc = PC_FINISH;
                break;
            }
            call(s, SUB_OPEN_ROOT, 0, PC_ROOT_OPENED);
            break;
        case PC_ROOT_OPENED:
            if (sub_failed(s)) {
                report_err(s, &s->sub_err);
                s->pc = PC_FINISH;
                break;
            }
            s->root = s->sub_region;
            s->root_held = 1;
            s->report.region = s->root;
            s->boot_pos = 0;
            s->pc = PC_BOOT;
            break;
        case PC_BOOT:
            pos = s->boot_pos;
            if (pos >= s->n) {
                s->pc = PC_WAIT;
                break;
            }
            if (cancelled(s)) {
                record_cancel(s);
                s->pc = PC_FINISH;
                break;
            }
            if (!s->children[pos].start_immediately) {
                s->boot_pos++;
                break;
            }
            if (dependency_unavailable(s, pos) != SUP_NONE) {
                if (s->children[pos].required) {
                    report_err_at(s, ASX_SUPERVISOR_ERR_DEPENDENCY_UNAVAILABLE, pos);
                    s->pc = PC_FINISH;
                    break;
                }
                s->boot_pos++;
                break;
            }
            call(s, SUB_START, pos, PC_BOOT_STARTED);
            break;
        case PC_BOOT_STARTED: after_start(s, s->boot_pos, PC_BOOT_DRAINED, PC_BOOT_CHECK); break;
        case PC_BOOT_DRAINED:
        case PC_RESTART_DRAINED:
            if (sub_failed(s)) {
                record_error(s, &s->sub_err);
                s->pc = PC_FINISH;
                break;
            }
            s->pc = s->pc == PC_BOOT_DRAINED ? PC_BOOT_CHECK : PC_RESTART_CHECK;
            break;
        case PC_BOOT_CHECK:
            if (s->report.outcome != ASX_OUTCOME_OK) {
                s->pc = PC_FINISH;
                break;
            }
            s->boot_pos++;
            s->pc = PC_BOOT;
            break;
        case PC_WAIT: call(s, SUB_WAIT_EXIT, 0, PC_WAITED); break;
        case PC_WAITED: handle_exit(s); break;
        case PC_STOPPED_DRAINED:
            if (sub_failed(s)) {
                record_error(s, &s->sub_err);
                s->pc = PC_FINISH;
                break;
            }
            s->pc = PC_WAIT;
            break;
        case PC_DEADLINE_DRAINED:
            if (sub_failed(s)) {
                record_error(s, &s->sub_err);
            } else {
                record_deadline(s);
            }
            s->pc = PC_FINISH;
            break;
        case PC_AFFECTED_DRAIN:
            if (s->aff_pos == 0u) {
                s->pc = PC_AFFECTED_DONE;
                break;
            }
            s->aff_pos--;
            call(s, SUB_DRAIN, s->affected[s->aff_pos], PC_AFFECTED_DRAINED);
            break;
        case PC_AFFECTED_DRAINED:
            if (sub_failed(s)) {
                record_error(s, &s->sub_err);
                s->drained_ok = 0;
            }
            s->pc = PC_AFFECTED_DRAIN;
            break;
        case PC_AFFECTED_DONE:
            if (!s->cancelled_ok || !s->drained_ok) {
                s->pc = PC_FINISH;
                break;
            }
            restart_set(s);
            call(s, SUB_BACKOFF, 0, PC_BACKED_OFF);
            break;
        case PC_BACKED_OFF:
            if (!s->sub_flag) {
                s->pc = PC_FINISH;
                break;
            }
            s->counted = 0;
            s->rs_pos = 0;
            s->pc = PC_RESTART;
            break;
        case PC_RESTART: restart_next(s); break;
        case PC_RESTART_STARTED:
            after_start(s, s->restart[s->rs_pos], PC_RESTART_DRAINED, PC_RESTART_CHECK);
            break;
        case PC_RESTART_CHECK:
            if (s->report.outcome != ASX_OUTCOME_OK) {
                s->pc = PC_FINISH;
                break;
            }
            s->rs_pos++;
            s->pc = PC_RESTART;
            break;
        case PC_FINISH: {
            uint32_t all[SUP_CHILDREN] = {0};
            for (pos = 0; pos < s->n; pos++) all[pos] = s->n - 1u - pos;
            (void)cancel_children(s, all, s->n);
            s->fin_pos = s->n;
            s->pc = PC_FINISH_DRAIN;
            break;
        }
        case PC_FINISH_DRAIN:
            if (s->fin_pos == 0u) {
                s->pc = PC_FINISH_CLOSE;
                break;
            }
            s->fin_pos--;
            call(s, SUB_DRAIN, s->fin_pos, PC_FINISH_DRAINED);
            break;
        case PC_FINISH_DRAINED:
            if (sub_failed(s)) record_error(s, &s->sub_err);
            s->pc = PC_FINISH_DRAIN;
            break;
        case PC_FINISH_CLOSE:
            if (s->root_held) {
                call(s, SUB_CLOSE_ROOT, 0, PC_FINISH_CLOSED);
            } else {
                s->pc = PC_FINISH_END;
            }
            break;
        case PC_FINISH_CLOSED:
            if (sub_failed(s)) record_error(s, &s->sub_err);
            s->pc = PC_FINISH_END;
            break;
        case PC_FINISH_END: publish(s); return ASX_OK;
        case PC_DONE: return ASX_OK;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Specs and configuration                                             */
/* ------------------------------------------------------------------ */

void asx_child_spec_init(asx_child_spec *spec, const char *name, asx_child_restart restart,
                         asx_child_start_fn start, void *user_data) {
    if (spec == NULL) return;
    memset(spec, 0, sizeof(*spec));
    spec->name = name;
    spec->start = start;
    spec->user_data = user_data;
    spec->restart = restart;
    spec->required = 1;
    spec->start_immediately = 1;
}

asx_status asx_child_spec_depends_on(asx_child_spec *spec, uint32_t dep_index) {
    if (spec == NULL) return ASX_E_INVALID_ARGUMENT;
    if (spec->dep_count >= ASX_SUPERVISOR_MAX_DEPS) return ASX_E_RESOURCE_EXHAUSTED;
    spec->depends_on[spec->dep_count++] = dep_index;
    return ASX_OK;
}

void asx_supervisor_config_init(asx_supervisor_config *cfg, const char *name, uint32_t max_restarts,
                                uint64_t window_ns) {
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->name = name;
    cfg->strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
    cfg->max_restarts = max_restarts;
    cfg->window_ns = window_ns;
    cfg->backoff.kind = ASX_RESTART_BACKOFF_EXPONENTIAL;
    cfg->backoff.initial_ns = 100000000u;
    cfg->backoff.max_ns = 10000000000u;
    cfg->backoff.multiplier = 2u;
    cfg->escalation = ASX_ESCALATION_STOP;
    cfg->budget = NULL;
}

static int copy_name(char *dst, const char *src) {
    size_t len;
    if (src == NULL) return 0;
    len = strlen(src);
    if (len >= ASX_SUPERVISOR_NAME_MAX) return 0;
    memcpy(dst, src, len + 1u);
    return 1;
}

/* SupervisorBuilder::compile (:1202-1305): names unique, dependencies
 * known, a child started at boot depending only on children started at
 * boot, and acyclic; the start order is topological, the earliest spec
 * first among the ready ones. order[k] is the spec index started k-th. */
static asx_status compile_children(const asx_child_spec *children, uint32_t n, uint32_t *order) {
    uint32_t indeg[SUP_CHILDREN];
    uint8_t done[SUP_CHILDREN];
    uint32_t i;
    uint32_t j;
    uint32_t k;

    for (i = 0; i < n; i++) {
        for (j = 0; j < i; j++) {
            if (strcmp(children[i].name, children[j].name) == 0) return ASX_E_NAME_CONFLICT;
        }
    }
    for (i = 0; i < n; i++) {
        indeg[i] = 0;
        done[i] = 0;
        for (j = 0; j < children[i].dep_count; j++) {
            uint32_t d = children[i].depends_on[j];
            uint32_t m;
            int dup = 0;
            if (d >= n || d == i) return ASX_E_INVALID_ARGUMENT;
            for (m = 0; m < j; m++) {
                if (children[i].depends_on[m] == d) dup = 1;
            }
            if (dup) continue; /* counted once (:1215-1220) */
            if (children[i].start_immediately && !children[d].start_immediately) {
                return ASX_E_INVALID_ARGUMENT;
            }
            indeg[i]++;
        }
    }
    for (k = 0; k < n; k++) {
        uint32_t next = SUP_NONE;
        for (i = 0; i < n; i++) {
            if (!done[i] && indeg[i] == 0u) {
                next = i;
                break;
            }
        }
        if (next == SUP_NONE) return ASX_E_INVALID_ARGUMENT; /* a cycle */
        done[next] = 1;
        order[k] = next;
        for (i = 0; i < n; i++) {
            uint32_t m;
            for (j = 0; j < children[i].dep_count; j++) {
                int dup = 0;
                if (children[i].depends_on[j] != next) continue;
                for (m = 0; m < j; m++) {
                    if (children[i].depends_on[m] == next) dup = 1;
                }
                if (!dup && indeg[i] > 0u) indeg[i]--;
            }
        }
    }
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_supervisor_spawn(asx_supervisor_handle *out, asx_region_id region,
                                const asx_supervisor_config *config, const asx_child_spec *children,
                                uint32_t child_count) {
    uint32_t order[SUP_CHILDREN];
    uint32_t position[SUP_CHILDREN];
    uint32_t idx;
    uint32_t i;
    uint32_t j;
    sup_slot *s;
    asx_task_id tid = ASX_INVALID_ID;
    uint32_t refusal = 0u;
    asx_status st;

    if (out == NULL || config == NULL || children == NULL) return ASX_E_INVALID_ARGUMENT;
    if (child_count == 0u || child_count > SUP_CHILDREN) return ASX_E_INVALID_ARGUMENT;
    if (config->max_restarts > ASX_SUPERVISOR_MAX_RESTARTS) return ASX_E_INVALID_ARGUMENT;
    if ((unsigned)config->strategy > (unsigned)ASX_SUPERVISOR_REST_FOR_ONE ||
        (unsigned)config->escalation > (unsigned)ASX_ESCALATION_RESET_COUNTER ||
        (unsigned)config->backoff.kind > (unsigned)ASX_RESTART_BACKOFF_EXPONENTIAL) {
        return ASX_E_INVALID_ARGUMENT;
    }
    if (config->name == NULL || strlen(config->name) >= ASX_SUPERVISOR_NAME_MAX) {
        return ASX_E_INVALID_ARGUMENT;
    }
    for (i = 0; i < child_count; i++) {
        if (children[i].name == NULL || strlen(children[i].name) >= ASX_SUPERVISOR_NAME_MAX ||
            children[i].start == NULL ||
            (unsigned)children[i].restart > (unsigned)ASX_CHILD_TEMPORARY ||
            children[i].dep_count > ASX_SUPERVISOR_MAX_DEPS) {
            return ASX_E_INVALID_ARGUMENT;
        }
    }
    st = compile_children(children, child_count, order);
    if (st != ASX_OK) return st;

    /* Unused slots first, so handles to ended supervisors stay valid as
     * long as possible; then one whose controller ended. */
    for (idx = 0; idx < ASX_MAX_SUPERVISORS; idx++) {
        if (!g_sups[idx].in_use) break;
    }
    if (idx >= ASX_MAX_SUPERVISORS) {
        for (idx = 0; idx < ASX_MAX_SUPERVISORS; idx++) {
            if (sup_ended(&g_sups[idx])) break;
        }
    }
    if (idx >= ASX_MAX_SUPERVISORS) return ASX_E_RESOURCE_EXHAUSTED;

    st = asx_task_spawn(region, ctl_poll, (void *)(uintptr_t)idx, &tid);
    if (st != ASX_OK) {
        /* Under lab dispatch Rust's handle exists anyway; its controller's
         * admission refuses it at the next step. */
        refusal = asx_scheduler_last_spawn_refusal();
        if (refusal == 0u) return st;
        tid = ASX_INVALID_ID;
    }

    s = &g_sups[idx];
    {
        uint32_t generation = next_gen(s->generation);
        memset(s, 0, sizeof(*s));
        s->generation = generation;
    }
    s->in_use = 1;
    (void)copy_name(s->name, config->name);
    s->strategy = config->strategy;
    s->max_restarts = config->max_restarts;
    s->window_ns = config->window_ns;
    s->backoff = config->backoff;
    s->escalation = config->escalation;
    s->has_budget = config->budget != NULL;
    if (config->budget != NULL) s->budget = *config->budget;
    s->n = child_count;
    for (i = 0; i < child_count; i++) position[order[i]] = i;
    for (i = 0; i < child_count; i++) {
        const asx_child_spec *spec = &children[order[i]];
        sup_child *c = &s->children[i];
        (void)copy_name(c->name, spec->name);
        c->spec_index = order[i];
        c->start = spec->start;
        c->user_data = spec->user_data;
        c->restart = spec->restart;
        c->required = spec->required;
        c->start_immediately = spec->start_immediately;
        c->dep_count = 0;
        for (j = 0; j < spec->dep_count; j++) {
            uint32_t m;
            int dup = 0;
            for (m = 0; m < j; m++) {
                if (spec->depends_on[m] == spec->depends_on[j]) dup = 1;
            }
            if (!dup) c->deps[c->dep_count++] = position[spec->depends_on[j]];
        }
    }
    s->task = tid;
    s->refusal = refusal;
    s->owner_region = region;
    (void)asx_cx_init(&s->cx, region, tid, ASX_CAP_CANCEL_CHECK);
    s->root = ASX_INVALID_ID;
    s->report.outcome = ASX_OUTCOME_OK;
    s->report.error = ASX_SUPERVISOR_ERR_NONE;
    s->report.error_child = SUP_NONE;
    s->report.region = ASX_INVALID_ID;
    s->pc = PC_EXEC;
    s->sub = SUB_NONE;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_supervisor_abort(asx_supervisor_handle sup) {
    sup_slot *s = sup_lookup(sup);
    asx_status st;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    /* A refused controller's handle only caches the abort. */
    if (s->task == ASX_INVALID_ID) return ASX_OK;
    s->abort_reason = asx_cancel_reason_default(ASX_CANCEL_USER, "abort");
    st = asx_task_abort_request(s->task, &s->abort_reason);
    /* A joined controller has nothing left to abort. */
    if (st == ASX_E_NOT_FOUND || st == ASX_E_STALE_HANDLE) return ASX_OK;
    return st;
}

asx_status asx_supervisor_join(asx_supervisor_handle sup, asx_task_id self,
                               asx_supervisor_report *out) {
    sup_slot *s = sup_lookup(sup);
    asx_outcome o;
    asx_status st;
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->joined) return ASX_E_INVALID_STATE;
    if (s->task == ASX_INVALID_ID) {
        if (!asx_task_refusal_delivered(s->refusal)) {
            st = asx_task_await_refusal(self, s->refusal);
            if (st == ASX_E_PENDING) return st;
        }
        s->joined = 1;
        return ASX_E_CANCELLED;
    }
    st = asx_task_join_poll(self, s->task, &o);
    if (st != ASX_OK) return st;
    s->joined = 1;
    if (!s->finished) {
        /* It never ran: no report (ManagedSupervisorHandle::join, :3437). */
        return o.severity == ASX_OUTCOME_PANICKED ? ASX_E_INVALID_STATE : ASX_E_CANCELLED;
    }
    if (out != NULL) *out = s->report;
    return ASX_OK;
}

asx_task_id asx_supervisor_task(asx_supervisor_handle sup) {
    const sup_slot *s = sup_lookup(sup);
    return s != NULL ? s->task : ASX_INVALID_ID;
}

asx_region_id asx_supervisor_region(asx_supervisor_handle sup) {
    const sup_slot *s = sup_lookup(sup);
    return s != NULL ? s->report.region : ASX_INVALID_ID;
}

int asx_supervisor_is_alive(asx_supervisor_handle sup) {
    const sup_slot *s = sup_lookup(sup);
    return s != NULL && !sup_ended(s);
}

int asx_supervisor_finished(asx_supervisor_handle sup) {
    const sup_slot *s = sup_lookup(sup);
    return s != NULL && s->finished;
}

void asx_supervisor_reset(void) {
    uint32_t i;
    for (i = 0; i < ASX_MAX_SUPERVISORS; i++) {
        uint32_t generation = g_sups[i].generation;
        memset(&g_sups[i], 0, sizeof(g_sups[i]));
        g_sups[i].generation = generation;
    }
}
