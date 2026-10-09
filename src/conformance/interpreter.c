/*
 * conformance/interpreter.c — runs asx.scenario.v2 scenarios through the
 * asx runtime and projects them into vocabulary v2 (bead W1.5,
 * bd-9kll.2.5). The C twin of tools/twin_run/src/run.rs.
 *
 * Contracts: docs/SCENARIO_DSL_V2.md (execution model, per-step C APIs) and
 * docs/CANONICAL_VOCABULARY_V2.md (projection, snapshot, observations).
 *
 * Every event comes from the runtime's own trace stream (a trace observer),
 * never from the interpreter: an interpreter that emitted events would test
 * itself. Where the C runtime cannot express a step yet (a gap listed in
 * DSL §7), the step fails the run with that gap named, so a missing
 * capability is never mistaken for parity.
 *
 * SPDX-License-Identifier: MIT
 */

#include "interpreter.h"

#include "canon.h"

#include <asx/asx.h>
#include <asx/cx/cx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/sync/barrier.h>
#include <asx/sync/mutex.h>
#include <asx/sync/notify.h>
#include <asx/sync/semaphore.h>
#include <asx/time/sleep.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bounds                                                              */
/* ------------------------------------------------------------------ */

#define IT_MAX_TASKS ASX_MAX_TASKS
#define IT_MAX_REGIONS (ASX_MAX_REGIONS + 1u)
#define IT_MAX_LOCAL 16u
#define IT_MAX_EVENTS 4096u
#define IT_TEXT_CAP (64u * 1024u)
#define IT_MAX_OBLIGATION_NAMES 512u

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    asx_obligation_id id;
} it_local_obligation;

typedef struct {
    const char *name;
    asx_region_id id;
} it_local_region;

typedef struct {
    const char *name;
    const char *region_name;
    asx_region_id region;
    asx_task_id id;
    int spawned;           /* asx_task_spawn succeeded */
    uint32_t program;      /* array node in the scenario document */
    uint32_t pc;           /* 0-based index of the current step */
    uint32_t phase;        /* suspension state of the current step */
    asx_sleep_state sleep; /* the current sleep step */
    asx_status end;        /* poll result once the program has ended */
    int joined;            /* outcome taken by a join; the slot is gone */
    uint32_t outcome;      /* projected outcome node (out document) when joined */
    it_local_obligation obligations[IT_MAX_LOCAL];
    uint32_t n_obligations;
    it_local_region regions[IT_MAX_LOCAL];
    uint32_t n_regions;
    /* The task's context for cancel-aware sync waits (cancel check). */
    asx_cx cx;
    /* The current blocking sync step's registration (DSL §3.7). */
    union {
        asx_notify_waiter notify;
        asx_mutex_lock_waiter mutex;
        asx_semaphore_waiter sem;
        asx_barrier_waiter barrier;
    } wait;
    /* Held mutex guards and semaphore permits, by object name. */
    struct {
        const char *name;
        asx_mutex_guard guard;
    } guards[IT_MAX_LOCAL];
    uint32_t n_guards;
    struct {
        const char *name;
        asx_semaphore_permit permit;
    } permits[IT_MAX_LOCAL];
    uint32_t n_permits;
    /* Held mpsc send permits (DSL §3.6), by permit name. */
    struct {
        const char *name;
        uint32_t channel; /* index in g_channels */
        asx_send_permit permit;
    } send_permits[IT_MAX_LOCAL];
    uint32_t n_send_permits;
    /* Projection of the task's sleep timers (vocabulary §2: "<task>/tm<k>"). */
    uint32_t timers;         /* timers registered so far (k of the latest) */
    uint32_t timer_name_off; /* name of the latest, in g_text */
    int timer_pending;       /* the latest is scheduled and not yet fired/cancelled */
    asx_time timer_deadline;
} it_task;

typedef struct {
    const char *name;
    const char *parent; /* NULL for the root */
    asx_region_id id;
} it_region;

/* A declared sync object (DSL §3.7). */
typedef enum { IT_SYNC_MUTEX, IT_SYNC_SEMAPHORE, IT_SYNC_BARRIER, IT_SYNC_NOTIFY } it_sync_type;

typedef struct {
    const char *name;
    it_sync_type type;
    asx_mutex_handle mutex;
    asx_semaphore_handle semaphore;
    asx_barrier_handle barrier;
    asx_notify_handle notify;
} it_sync;

#define IT_MAX_SYNC 16u

/* A declared mpsc channel (DSL §3.6). Its endpoints are bound to their
 * owner tasks, as Rust moves them into the owners' bodies; an endpoint
 * closes when its owner closes it or its owner's body ends. */
typedef struct {
    const char *name;
    asx_channel_id id;
    uint32_t sender;   /* owner, index in g_tasks */
    uint32_t receiver; /* owner, index in g_tasks */
    int sender_open;
    int receiver_open;
} it_channel;

#define IT_MAX_CHANNELS ASX_MAX_CHANNELS

/* A cancel reason captured when its event was emitted. */
typedef struct {
    asx_cancel_kind kind;
    asx_region_id origin_region;
    asx_task_id origin_task;
    asx_time timestamp;
    uint32_t message_off; /* IT_NO_TEXT when absent */
    uint32_t message_len;
    uint32_t cause_chain_len;
    int truncated;
} it_reason;

#define IT_NO_TEXT 0xFFFFFFFFu

typedef struct {
    asx_trace_event_kind kind;
    uint64_t entity;
    uint64_t aux;
    asx_obligation_info obligation; /* obligation kinds */
    it_reason reason;               /* CANCEL_REQUEST */
    int has_reason;
    uint32_t text_off; /* USER */
    uint32_t text_len;
} it_event;

static const asx_json_doc *g_in;
static asx_json_doc *g_out;
static uint32_t g_observations;

static it_task g_tasks[IT_MAX_TASKS];
static uint32_t g_n_tasks;
static it_region g_regions[IT_MAX_REGIONS];
static uint32_t g_n_regions;

static it_sync g_sync[IT_MAX_SYNC];
static uint32_t g_n_sync;

static it_channel g_channels[IT_MAX_CHANNELS];
static uint32_t g_n_channels;

static it_event g_events[IT_MAX_EVENTS];
static uint32_t g_n_events;
static char g_text[IT_TEXT_CAP];
static uint32_t g_text_used;

static char *g_error;
static size_t g_error_cap;
static int g_failed;

/* The run's root region and its max_steps poll budget (DSL §1). */
static asx_region_id g_root;
static asx_budget *g_budget;

static asx_runtime g_rt;

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

/* Record the first harness error; later ones add nothing. */
static void it_fail(const char *what, const char *detail) {
    if (g_failed) return;
    g_failed = 1;
    if (g_error != NULL && g_error_cap > 0u) {
        (void)snprintf(g_error, g_error_cap, "%s%s%s", what, detail != NULL ? ": " : "",
                       detail != NULL ? detail : "");
    }
}

static void it_fail_task(const it_task *t, uint32_t step, const char *what) {
    char buf[256];
    (void)snprintf(buf, sizeof(buf), "task %s step %u", t->name, (unsigned)step);
    if (!g_failed) {
        g_failed = 1;
        if (g_error != NULL && g_error_cap > 0u) {
            (void)snprintf(g_error, g_error_cap, "%s: %s", buf, what);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static const char *it_str(uint32_t node, const char *key) {
    return asx_json_get_string(g_in, node, key);
}

static it_task *task_by_name(const char *name) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_tasks; i++) {
        if (strcmp(g_tasks[i].name, name) == 0) return &g_tasks[i];
    }
    return NULL;
}

/* Handles carry a state bitmask set when they were packed, so the same
 * entity can appear under different handle values; identity is the type
 * tag plus the generation-tagged slot. */
static int same_entity(uint64_t a, uint64_t b) {
    return asx_handle_type_tag(a) == asx_handle_type_tag(b) &&
           asx_handle_index(a) == asx_handle_index(b);
}

static it_task *task_by_id(asx_task_id id) {
    uint32_t i;
    for (i = 0; i < g_n_tasks; i++) {
        if (g_tasks[i].spawned && same_entity(g_tasks[i].id, id)) return &g_tasks[i];
    }
    return NULL;
}

static it_region *region_by_name(const char *name) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_regions; i++) {
        if (strcmp(g_regions[i].name, name) == 0) return &g_regions[i];
    }
    return NULL;
}

static it_region *region_by_id(asx_region_id id) {
    uint32_t i;
    for (i = 0; i < g_n_regions; i++) {
        if (same_entity(g_regions[i].id, id)) return &g_regions[i];
    }
    return NULL;
}

static int add_region(const char *name, const char *parent, asx_region_id id) {
    if (region_by_name(name) != NULL) {
        it_fail("duplicate region name", name);
        return 0;
    }
    if (g_n_regions >= IT_MAX_REGIONS) {
        it_fail("too many regions", name);
        return 0;
    }
    g_regions[g_n_regions].name = name;
    g_regions[g_n_regions].parent = parent;
    g_regions[g_n_regions].id = id;
    g_n_regions++;
    return 1;
}

static it_task *add_task(const char *name, const char *region_name, asx_region_id region,
                         uint32_t program) {
    it_task *t;
    if (name == NULL || task_by_name(name) != NULL) {
        it_fail("missing or duplicate task name", name);
        return NULL;
    }
    if (g_n_tasks >= IT_MAX_TASKS) {
        it_fail("too many tasks", name);
        return NULL;
    }
    if (asx_json_type_of(g_in, program) != ASX_JSON_ARRAY) {
        it_fail("task without a program", name);
        return NULL;
    }
    t = &g_tasks[g_n_tasks++];
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->region_name = region_name;
    t->region = region;
    t->program = program;
    t->end = ASX_OK;
    return t;
}

/* ------------------------------------------------------------------ */
/* Vocabulary encodings                                                */
/* ------------------------------------------------------------------ */

static const char *cancel_kind_name(asx_cancel_kind k) {
    switch (k) {
    case ASX_CANCEL_USER: return "User";
    case ASX_CANCEL_TIMEOUT: return "Timeout";
    case ASX_CANCEL_DEADLINE: return "Deadline";
    case ASX_CANCEL_POLL_QUOTA: return "PollQuota";
    case ASX_CANCEL_COST_BUDGET: return "CostBudget";
    case ASX_CANCEL_FAIL_FAST: return "FailFast";
    case ASX_CANCEL_RACE_LOST: return "RaceLost";
    case ASX_CANCEL_LINKED_EXIT: return "LinkedExit";
    case ASX_CANCEL_PARENT: return "ParentCancelled";
    case ASX_CANCEL_RESOURCE: return "ResourceUnavailable";
    case ASX_CANCEL_SHUTDOWN: return "Shutdown";
    }
    return NULL;
}

static int cancel_kind_parse(const char *name, asx_cancel_kind *out) {
    static const asx_cancel_kind kinds[] = {ASX_CANCEL_USER,        ASX_CANCEL_TIMEOUT,
                                            ASX_CANCEL_DEADLINE,    ASX_CANCEL_POLL_QUOTA,
                                            ASX_CANCEL_COST_BUDGET, ASX_CANCEL_FAIL_FAST,
                                            ASX_CANCEL_RACE_LOST,   ASX_CANCEL_LINKED_EXIT,
                                            ASX_CANCEL_PARENT,      ASX_CANCEL_RESOURCE,
                                            ASX_CANCEL_SHUTDOWN};
    size_t i;
    if (name == NULL) return 0;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (strcmp(cancel_kind_name(kinds[i]), name) == 0) {
            *out = kinds[i];
            return 1;
        }
    }
    return 0;
}

static const char *obligation_kind_name(asx_obligation_kind k) {
    switch (k) {
    case ASX_OBLIGATION_KIND_SEND_PERMIT: return "SendPermit";
    case ASX_OBLIGATION_KIND_ACK: return "Ack";
    case ASX_OBLIGATION_KIND_LEASE: return "Lease";
    case ASX_OBLIGATION_KIND_IO_OP: return "IoOp";
    case ASX_OBLIGATION_KIND_SEMAPHORE_PERMIT: return "SemaphorePermit";
    case ASX_OBLIGATION_KIND_TRANSACTION: return "Transaction";
    case ASX_OBLIGATION_KIND_GENERIC: return NULL; /* no Rust counterpart */
    }
    return NULL;
}

static int obligation_kind_parse(const char *name, asx_obligation_kind *out) {
    static const asx_obligation_kind kinds[] = {ASX_OBLIGATION_KIND_SEND_PERMIT,
                                                ASX_OBLIGATION_KIND_ACK,
                                                ASX_OBLIGATION_KIND_LEASE,
                                                ASX_OBLIGATION_KIND_IO_OP,
                                                ASX_OBLIGATION_KIND_SEMAPHORE_PERMIT,
                                                ASX_OBLIGATION_KIND_TRANSACTION};
    size_t i;
    if (name == NULL) return 0;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (strcmp(obligation_kind_name(kinds[i]), name) == 0) {
            *out = kinds[i];
            return 1;
        }
    }
    return 0;
}

static const char *abort_reason_name(asx_obligation_abort_reason r) {
    switch (r) {
    case ASX_OBLIGATION_ABORT_EXPLICIT: return "Explicit";
    case ASX_OBLIGATION_ABORT_CANCEL: return "Cancel";
    case ASX_OBLIGATION_ABORT_ERROR: return "Error";
    case ASX_OBLIGATION_ABORT_NONE:
    case ASX_OBLIGATION_ABORT_LEAK_RECOVERED: return NULL; /* no Rust counterpart */
    }
    return NULL;
}

static const char *task_state_name(asx_task_state s) {
    switch (s) {
    case ASX_TASK_CREATED: return "Created";
    case ASX_TASK_RUNNING: return "Running";
    case ASX_TASK_CANCEL_REQUESTED: return "CancelRequested";
    case ASX_TASK_CANCELLING: return "Cancelling";
    case ASX_TASK_FINALIZING: return "Finalizing";
    case ASX_TASK_COMPLETED: return "Completed";
    }
    return NULL;
}

static const char *region_state_name(asx_region_state s) {
    switch (s) {
    case ASX_REGION_OPEN: return "Open";
    case ASX_REGION_CLOSING: return "Closing";
    case ASX_REGION_DRAINING: return "Draining";
    case ASX_REGION_FINALIZING: return "Finalizing";
    case ASX_REGION_CLOSED: return "Closed";
    }
    return NULL;
}

/* A status as its vocabulary name; an unnamed value fails the run. */
static uint32_t status_node(asx_status st) {
    const char *name = asx_status_name(st);
    if (name == NULL) {
        it_fail("status without a name", NULL);
        return asx_json_new_null(g_out);
    }
    return asx_json_new_string(g_out, name);
}

static uint32_t text_store(const char *s, size_t len) {
    uint32_t off;
    if (len >= IT_TEXT_CAP || (size_t)g_text_used + len + 1u > IT_TEXT_CAP) {
        it_fail("trace text arena exhausted", NULL);
        return IT_NO_TEXT;
    }
    off = g_text_used;
    if (len > 0u) memcpy(&g_text[off], s, len);
    g_text[off + (uint32_t)len] = '\0';
    g_text_used += (uint32_t)len + 1u;
    return off;
}

static void capture_reason(const asx_cancel_reason *r, it_reason *out) {
    const asx_cancel_reason *c;
    out->kind = r->kind;
    out->origin_region = r->origin_region;
    out->origin_task = r->origin_task;
    out->timestamp = r->timestamp;
    out->message_off = IT_NO_TEXT;
    out->message_len = 0;
    if (r->message != NULL) {
        size_t len = strlen(r->message);
        out->message_off = text_store(r->message, len);
        out->message_len = (uint32_t)len;
    }
    out->cause_chain_len = 0;
    for (c = r->cause; c != NULL && out->cause_chain_len < 1024u; c = c->cause) {
        out->cause_chain_len++;
    }
    out->truncated = r->truncated;
}

/* Vocabulary §4 reason object. */
static uint32_t reason_node(const it_reason *r) {
    uint32_t o = asx_json_new_object(g_out);
    const char *kind = cancel_kind_name(r->kind);
    const it_region *origin = region_by_id(r->origin_region);
    const it_task *task = r->origin_task == ASX_INVALID_ID ? NULL : task_by_id(r->origin_task);
    if (kind == NULL) it_fail("cancel reason with an unknown kind", NULL);
    if (r->origin_task != ASX_INVALID_ID && task == NULL) {
        it_fail("cancel reason names an unknown origin task", NULL);
    }
    asx_json_set(g_out, o, "kind", asx_json_new_string(g_out, kind));
    /* C reasons without an origin region (a gap, DSL §7) project as null,
     * which the comparison reports. */
    asx_json_set(g_out, o, "origin_region",
                 origin != NULL ? asx_json_new_string(g_out, origin->name)
                                : asx_json_new_null(g_out));
    asx_json_set(g_out, o, "origin_task",
                 task != NULL ? asx_json_new_string(g_out, task->name) : asx_json_new_null(g_out));
    asx_json_set(g_out, o, "timestamp_ns", asx_json_new_u64(g_out, r->timestamp));
    asx_json_set(g_out, o, "message",
                 r->message_off != IT_NO_TEXT
                     ? asx_json_new_string_len(g_out, &g_text[r->message_off], r->message_len)
                     : asx_json_new_null(g_out));
    asx_json_set(g_out, o, "cause_chain_len", asx_json_new_u64(g_out, r->cause_chain_len));
    asx_json_set(g_out, o, "truncated", asx_json_new_bool(g_out, r->truncated));
    return o;
}

/* Vocabulary §4 outcome of a completed task whose slot is still held. */
static uint32_t outcome_node(asx_task_id id) {
    asx_outcome outcome;
    uint32_t o;
    if (asx_task_get_outcome(id, &outcome) != ASX_OK) {
        it_fail("outcome of a completed task is unreadable", NULL);
        return asx_json_new_null(g_out);
    }
    o = asx_json_new_object(g_out);
    switch (outcome.severity) {
    case ASX_OUTCOME_OK: asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "ok")); break;
    case ASX_OUTCOME_ERR: {
        asx_status err = ASX_OK;
        if (asx_task_get_error(id, &err) != ASX_OK) it_fail("task error is unreadable", NULL);
        asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "err"));
        asx_json_set(g_out, o, "status", status_node(err));
        break;
    }
    case ASX_OUTCOME_CANCELLED: {
        asx_cancel_reason r;
        it_reason captured;
        if (asx_task_get_cancel_reason(id, &r) != ASX_OK) {
            it_fail("cancelled task has no cancel reason", NULL);
            return o;
        }
        capture_reason(&r, &captured);
        asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "cancelled"));
        asx_json_set(g_out, o, "reason", reason_node(&captured));
        break;
    }
    case ASX_OUTCOME_PANICKED: {
        const char *message = NULL;
        if (asx_task_get_panic_message(id, &message) != ASX_OK || message == NULL) {
            it_fail("panicked task without a panic message", NULL);
            break;
        }
        asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "panicked"));
        asx_json_set(g_out, o, "message", asx_json_new_string(g_out, message));
        break;
    }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Trace observer: the runtime's events, captured as they are emitted   */
/* ------------------------------------------------------------------ */

static void it_observe(void *ctx, const asx_trace_event *ev, const asx_trace_payload *payload) {
    it_event *e;
    (void)ctx;
    switch (ev->kind) {
    /* Not projected (vocabulary §11): their information reaches the
     * comparison through the snapshot, the observations or not at all. */
    case ASX_TRACE_SCHED_POLL:
    case ASX_TRACE_SCHED_BUDGET:
    case ASX_TRACE_SCHED_QUIESCENT:
    case ASX_TRACE_SCHED_ROUND:
    case ASX_TRACE_TASK_TRANSITION:
    case ASX_TRACE_CHANNEL_SEND:
    case ASX_TRACE_CHANNEL_RECV: return;
    case ASX_TRACE_SCHED_COMPLETE:
    case ASX_TRACE_REGION_OPEN:
    case ASX_TRACE_REGION_CLOSE:
    case ASX_TRACE_REGION_CLOSED:
    case ASX_TRACE_TASK_SPAWN:
    case ASX_TRACE_REGION_CANCELLED:
    case ASX_TRACE_CANCEL_REQUEST:
    case ASX_TRACE_OBLIGATION_RESERVE:
    case ASX_TRACE_OBLIGATION_COMMIT:
    case ASX_TRACE_OBLIGATION_ABORT:
    case ASX_TRACE_OBLIGATION_LEAK:
    case ASX_TRACE_TIMER_SET:
    case ASX_TRACE_TIMER_FIRE:
    case ASX_TRACE_TIMER_CANCEL:
    case ASX_TRACE_USER: break;
    default:
        it_fail("unknown runtime trace event kind", asx_trace_event_kind_str(ev->kind));
        return;
    }
    if (g_n_events >= IT_MAX_EVENTS) {
        it_fail("too many runtime events", NULL);
        return;
    }
    e = &g_events[g_n_events++];
    memset(e, 0, sizeof(*e));
    e->kind = ev->kind;
    e->entity = ev->entity_id;
    e->aux = ev->aux;
    e->text_off = IT_NO_TEXT;
    /* Capture what the event refers to now: the slot may be gone later. */
    if (ev->kind == ASX_TRACE_OBLIGATION_RESERVE || ev->kind == ASX_TRACE_OBLIGATION_COMMIT ||
        ev->kind == ASX_TRACE_OBLIGATION_ABORT || ev->kind == ASX_TRACE_OBLIGATION_LEAK) {
        if (asx_obligation_get_info((asx_obligation_id)ev->entity_id, &e->obligation) != ASX_OK) {
            it_fail("obligation event for an unreadable obligation", NULL);
        }
    } else if (ev->kind == ASX_TRACE_CANCEL_REQUEST || ev->kind == ASX_TRACE_REGION_CANCELLED) {
        /* The reason the event was emitted with, not a later strengthened
         * one. */
        if (payload->reason == NULL) {
            it_fail("cancel event without its reason", asx_trace_event_kind_str(ev->kind));
            return;
        }
        capture_reason(payload->reason, &e->reason);
        e->has_reason = 1;
    } else if (ev->kind == ASX_TRACE_USER) {
        const char *text = payload->text != NULL ? payload->text : "";
        size_t len = strlen(text);
        e->text_off = text_store(text, len);
        e->text_len = (uint32_t)len;
    }
}

/* ------------------------------------------------------------------ */
/* Observations                                                        */
/* ------------------------------------------------------------------ */

static void observe(const it_task *t, uint32_t step, const char *op, uint32_t status,
                    uint32_t value) {
    uint32_t o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "task", asx_json_new_string(g_out, t->name));
    asx_json_set(g_out, o, "step", asx_json_new_u64(g_out, step));
    asx_json_set(g_out, o, "op", asx_json_new_string(g_out, op));
    asx_json_set(g_out, o, "status", status);
    asx_json_set(g_out, o, "value", value == ASX_JSON_NONE ? asx_json_new_null(g_out) : value);
    asx_json_push(g_out, g_observations, o);
}

static void observe_status(const it_task *t, uint32_t step, const char *op, asx_status st) {
    observe(t, step, op, status_node(st), ASX_JSON_NONE);
}

/* ------------------------------------------------------------------ */
/* Budgets                                                             */
/* ------------------------------------------------------------------ */

/* DSL §1 budget: null is infinite; a partial budget starts from
 * Budget::new() (priority 128) and applies each present field. Returns 0
 * when the node is absent or null (use the region's budget alone). */
static int parse_budget(uint32_t node, asx_budget *out) {
    uint64_t v;
    if (node == ASX_JSON_NONE || asx_json_is_null(g_in, node)) return 0;
    *out = asx_budget_new();
    if (asx_json_u64(g_in, asx_json_get(g_in, node, "deadline_ns"), &v)) out->deadline = v;
    if (asx_json_u64(g_in, asx_json_get(g_in, node, "poll_quota"), &v)) {
        if (v > UINT32_MAX) it_fail("poll_quota out of range", NULL);
        out->poll_quota = (uint32_t)v;
    }
    if (asx_json_u64(g_in, asx_json_get(g_in, node, "cost_quota"), &v)) out->cost_quota = v;
    if (asx_json_u64(g_in, asx_json_get(g_in, node, "priority"), &v)) {
        if (v > 255u) it_fail("priority out of range", NULL);
        out->priority = (uint8_t)v;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Steps                                                               */
/* ------------------------------------------------------------------ */

typedef enum { STEP_NEXT, STEP_PENDING, STEP_END } step_result;

static asx_status interp_poll(void *user_data, asx_task_id self);

static it_local_obligation *local_obligation(it_task *t, const char *name) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < t->n_obligations; i++) {
        if (t->obligations[i].name != NULL && strcmp(t->obligations[i].name, name) == 0) {
            return &t->obligations[i];
        }
    }
    return NULL;
}

static it_local_region *local_region(it_task *t, const char *name) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < t->n_regions; i++) {
        if (strcmp(t->regions[i].name, name) == 0) return &t->regions[i];
    }
    return NULL;
}

static it_sync *sync_by_name(const char *name, it_sync_type type) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_sync; i++) {
        if (strcmp(g_sync[i].name, name) == 0) return g_sync[i].type == type ? &g_sync[i] : NULL;
    }
    return NULL;
}

/* The channel `name` if task `t` owns its still-open sender (`sender` 1)
 * or receiver (0), else NULL: an endpoint is usable only in its owner's
 * body (DSL §3.6). */
static it_channel *owned_endpoint(const it_task *t, const char *name, int sender) {
    uint32_t i;
    uint32_t me = (uint32_t)(t - g_tasks);
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_channels; i++) {
        it_channel *c = &g_channels[i];
        if (strcmp(c->name, name) != 0) continue;
        if (sender) return c->sender == me && c->sender_open ? c : NULL;
        return c->receiver == me && c->receiver_open ? c : NULL;
    }
    return NULL;
}

/* Rust's SendPermit drop without send (mpsc.rs:1663): the slot is
 * released and the obligation aborted with reason Cancel, where an
 * explicit abort says Explicit; C has no destructor, so the interpreter
 * settles the obligation itself and releases the slot untracked. */
static void drop_send_permit(asx_send_permit *permit) {
    asx_obligation_id ob = permit->obligation;
    asx_status st;
    permit->obligation = ASX_INVALID_ID;
    asx_send_permit_abort(permit);
    if (ob == ASX_INVALID_ID) return;
    st = asx_obligation_abort_with_reason(ob, ASX_OBLIGATION_ABORT_CANCEL);
    (void)st;
}

static void close_endpoint(it_channel *c, int sender) {
    asx_status st;
    if (sender) {
        c->sender_open = 0;
        st = asx_channel_close_sender(c->id);
    } else {
        c->receiver_open = 0;
        st = asx_channel_close_receiver(c->id);
    }
    (void)st; /* closing an already half-closed channel side cannot fail */
}

/* Release held guard / permit i, keeping the others in acquisition order
 * (a name's most recent permit is the one sem_release pops, as Rust's
 * Vec::pop). */
static asx_status release_guard(it_task *t, uint32_t i) {
    asx_status st = asx_mutex_unlock(t->guards[i].guard);
    for (; i + 1u < t->n_guards; i++) t->guards[i] = t->guards[i + 1u];
    t->n_guards--;
    return st;
}

static asx_status release_permit(it_task *t, uint32_t i) {
    asx_status st = asx_semaphore_release(t->permits[i].permit);
    for (; i + 1u < t->n_permits; i++) t->permits[i] = t->permits[i + 1u];
    t->n_permits--;
    return st;
}

/* The task body ended (returned, acknowledged a cancel, or panicked): what
 * it still holds is released the way Rust drops the body's locals
 * (twin_run's Local, fields in declaration order): guards, then semaphore
 * permits, each by object name, a name's permits oldest first; then send
 * permits by permit name; then owned senders, then receivers, each by
 * channel name. A dropped Rust semaphore permit commits its obligation
 * (sync/semaphore.rs:1081); C has no destructors, so the interpreter
 * releases explicitly. */
static void drop_locals(it_task *t) {
    uint32_t me = (uint32_t)(t - g_tasks);
    int sender;
    while (t->n_guards > 0u) {
        uint32_t i;
        uint32_t first = 0;
        for (i = 1; i < t->n_guards; i++) {
            if (strcmp(t->guards[i].name, t->guards[first].name) < 0) first = i;
        }
        (void)release_guard(t, first);
    }
    while (t->n_permits > 0u) {
        uint32_t i;
        uint32_t first = 0;
        for (i = 1; i < t->n_permits; i++) {
            if (strcmp(t->permits[i].name, t->permits[first].name) < 0) first = i;
        }
        (void)release_permit(t, first);
    }
    while (t->n_send_permits > 0u) {
        uint32_t i;
        uint32_t first = 0;
        for (i = 1; i < t->n_send_permits; i++) {
            if (strcmp(t->send_permits[i].name, t->send_permits[first].name) < 0) first = i;
        }
        drop_send_permit(&t->send_permits[first].permit);
        for (i = first; i + 1u < t->n_send_permits; i++) {
            t->send_permits[i] = t->send_permits[i + 1u];
        }
        t->n_send_permits--;
    }
    for (sender = 1; sender >= 0; sender--) {
        for (;;) {
            it_channel *next = NULL;
            uint32_t i;
            for (i = 0; i < g_n_channels; i++) {
                it_channel *c = &g_channels[i];
                int mine = sender ? (c->sender == me && c->sender_open)
                                  : (c->receiver == me && c->receiver_open);
                if (mine && (next == NULL || strcmp(c->name, next->name) < 0)) next = c;
            }
            if (next == NULL) break;
            close_endpoint(next, sender);
        }
    }
}

/* A blocking sync wait's outcome: a status that ends the step, or
 * pending. A cancelled wait gives up its registration, as dropping the
 * Rust future does. */
typedef enum { WAIT_DONE, WAIT_PENDING } wait_result;

static wait_result sync_wait(it_task *t, uint32_t idx, const char *op, asx_status st,
                             asx_status (*cancel_fn)(void *), void *waiter) {
    if (st == ASX_E_PENDING) return WAIT_PENDING;
    if (st != ASX_OK && cancel_fn != NULL) (void)cancel_fn(waiter);
    observe_status(t, idx, op, st);
    return WAIT_DONE;
}

static asx_status cancel_mutex_wait(void *w) {
    return asx_mutex_lock_cancel((asx_mutex_lock_waiter *)w);
}
static asx_status cancel_sem_wait(void *w) {
    return asx_semaphore_acquire_cancel((asx_semaphore_waiter *)w);
}
static asx_status cancel_barrier_wait(void *w) {
    return asx_barrier_wait_cancel((asx_barrier_waiter *)w);
}
static asx_status cancel_notify_wait(void *w) {
    return asx_notify_wait_cancel((asx_notify_waiter *)w);
}

/* Index of the held send permit `name`, or n_send_permits. */
static uint32_t send_permit_index(const it_task *t, const char *name) {
    uint32_t i;
    for (i = 0; name != NULL && i < t->n_send_permits; i++) {
        if (strcmp(t->send_permits[i].name, name) == 0) return i;
    }
    return t->n_send_permits;
}

/* Blocking mpsc steps (DSL §3.6). Returns 1 when `op` is one of them. A
 * call is one poll of the Rust future: the library checks cancellation
 * first and ASX_E_PENDING keeps the step waiting (the task parked in the
 * channel's FIFO line). */
static int exec_channel_wait(it_task *t, uint32_t step, uint32_t idx, const char *op,
                             step_result *out) {
    asx_status st;
    uint64_t v = 0;
    *out = STEP_NEXT;
    if (strcmp(op, "reserve_send") == 0) {
        it_channel *c = owned_endpoint(t, it_str(step, "channel"), 1);
        const char *as = it_str(step, "as");
        asx_send_permit permit;
        if (c == NULL || as == NULL || t->n_send_permits >= IT_MAX_LOCAL ||
            send_permit_index(t, as) < t->n_send_permits) {
            it_fail_task(t, idx, "reserve_send needs an owned sender and a fresh permit name");
            *out = STEP_END;
            return 1;
        }
        st = asx_channel_reserve(c->id, &t->cx, &permit);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        if (st == ASX_OK) {
            t->send_permits[t->n_send_permits].name = as;
            t->send_permits[t->n_send_permits].channel = (uint32_t)(c - g_channels);
            t->send_permits[t->n_send_permits].permit = permit;
            t->n_send_permits++;
        }
        observe_status(t, idx, op, st);
        return 1;
    }
    if (strcmp(op, "send") == 0) {
        it_channel *c = owned_endpoint(t, it_str(step, "channel"), 1);
        if (c == NULL || !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &v)) {
            it_fail_task(t, idx, "send needs an owned sender and a value");
            *out = STEP_END;
            return 1;
        }
        st = asx_channel_send(c->id, &t->cx, v);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        observe_status(t, idx, op, st);
        return 1;
    }
    if (strcmp(op, "recv") == 0) {
        it_channel *c = owned_endpoint(t, it_str(step, "channel"), 0);
        if (c == NULL) {
            it_fail_task(t, idx, "recv needs an owned receiver");
            *out = STEP_END;
            return 1;
        }
        st = asx_channel_recv(c->id, &t->cx, &v);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        observe(t, idx, op, status_node(st),
                st == ASX_OK ? asx_json_new_u64(g_out, v) : ASX_JSON_NONE);
        return 1;
    }
    return 0;
}

/* Blocking sync steps (DSL §3.7). Returns 1 when `op` is one of them. */
static int exec_sync_wait(it_task *t, asx_task_id self, uint32_t step, uint32_t idx, const char *op,
                          step_result *out) {
    asx_status st;
    *out = STEP_NEXT;
    if (strcmp(op, "notify_wait") == 0) {
        it_sync *s = sync_by_name(it_str(step, "notify"), IT_SYNC_NOTIFY);
        if (s == NULL) {
            it_fail_task(t, idx, "notify_wait on an undeclared notify");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            if (asx_notify_wait_begin(s->notify, &t->wait.notify) != ASX_OK) {
                it_fail_task(t, idx, "asx_notify_wait_begin failed");
                *out = STEP_END;
                return 1;
            }
            t->phase = 1u;
        }
        /* Rust's Notified takes no Cx: a cancelled waiter stays parked until
         * notified (DSL §3.7), so the wait is not cancel-checked. */
        st = asx_notify_poll_wait(&t->wait.notify, NULL);
        if (sync_wait(t, idx, op, st, cancel_notify_wait, &t->wait.notify) == WAIT_PENDING) {
            *out = STEP_PENDING;
        }
        return 1;
    }
    if (strcmp(op, "mutex_lock") == 0) {
        const char *name = it_str(step, "mutex");
        it_sync *s = sync_by_name(name, IT_SYNC_MUTEX);
        asx_mutex_guard guard;
        if (s == NULL || t->n_guards >= IT_MAX_LOCAL) {
            it_fail_task(t, idx, "mutex_lock on an undeclared mutex (or too many guards)");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            if (asx_mutex_lock_begin(s->mutex, &t->wait.mutex) != ASX_OK) {
                it_fail_task(t, idx, "asx_mutex_lock_begin failed");
                *out = STEP_END;
                return 1;
            }
            t->phase = 1u;
        }
        st = asx_mutex_poll_lock(&t->wait.mutex, &guard, &t->cx);
        if (st == ASX_OK) {
            t->guards[t->n_guards].name = name;
            t->guards[t->n_guards].guard = guard;
            t->n_guards++;
        }
        if (sync_wait(t, idx, op, st, cancel_mutex_wait, &t->wait.mutex) == WAIT_PENDING) {
            *out = STEP_PENDING;
        }
        return 1;
    }
    if (strcmp(op, "sem_acquire") == 0) {
        const char *name = it_str(step, "semaphore");
        it_sync *s = sync_by_name(name, IT_SYNC_SEMAPHORE);
        asx_semaphore_permit permit;
        uint64_t count = 0;
        if (s == NULL || !asx_json_u64(g_in, asx_json_get(g_in, step, "count"), &count) ||
            t->n_permits >= IT_MAX_LOCAL) {
            it_fail_task(t, idx, "sem_acquire needs a declared semaphore and a count");
            *out = STEP_END;
            return 1;
        }
        if (count == 0u) { /* Rust: count 0 succeeds at once, no permit */
            observe_status(t, idx, op, ASX_OK);
            return 1;
        }
        if (count > 1u) {
            it_fail_task(t, idx,
                         "sem_acquire count > 1: C semaphores grant one permit per acquire "
                         "(Rust acquires n all-or-nothing)");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            if (asx_semaphore_acquire_begin(s->semaphore, &t->wait.sem) != ASX_OK) {
                it_fail_task(t, idx, "asx_semaphore_acquire_begin failed");
                *out = STEP_END;
                return 1;
            }
            t->phase = 1u;
        }
        st = asx_semaphore_poll_acquire(&t->wait.sem, &permit, &t->cx);
        if (st == ASX_OK) {
            t->permits[t->n_permits].name = name;
            t->permits[t->n_permits].permit = permit;
            t->n_permits++;
        }
        if (sync_wait(t, idx, op, st, cancel_sem_wait, &t->wait.sem) == WAIT_PENDING) {
            *out = STEP_PENDING;
        }
        return 1;
    }
    if (strcmp(op, "barrier_wait") == 0) {
        it_sync *s = sync_by_name(it_str(step, "barrier"), IT_SYNC_BARRIER);
        if (s == NULL) {
            it_fail_task(t, idx, "barrier_wait on an undeclared barrier");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            if (asx_barrier_wait_begin(s->barrier, &t->wait.barrier) != ASX_OK) {
                it_fail_task(t, idx, "asx_barrier_wait_begin failed");
                *out = STEP_END;
                return 1;
            }
            t->phase = 1u;
        }
        st = asx_barrier_poll_wait(&t->wait.barrier, &t->cx);
        if (sync_wait(t, idx, op, st, cancel_barrier_wait, &t->wait.barrier) == WAIT_PENDING) {
            *out = STEP_PENDING;
        }
        return 1;
    }
    (void)self;
    return 0;
}

/* A reason attributed as DSL §4 "Reason attribution" prescribes, stamped
 * now, with the step's optional message (scenario text, which outlives
 * the run). */
static asx_cancel_reason make_reason(asx_cancel_kind kind, asx_region_id origin_region,
                                     asx_task_id origin_task, const char *message) {
    asx_cancel_reason r;
    r.kind = kind;
    r.origin_region = origin_region;
    r.origin_task = origin_task;
    r.timestamp = asx_runtime_virtual_now();
    r.message = message;
    r.cause = NULL;
    r.truncated = 0;
    return r;
}

/* Capture a completed target's outcome, then release it with a join. */
static uint32_t take_outcome(it_task *target, asx_status *join_status) {
    uint32_t value = outcome_node(target->id);
    asx_outcome ignored;
    *join_status = asx_task_join(target->id, &ignored);
    if (*join_status == ASX_OK) {
        target->joined = 1;
        target->outcome = value;
    }
    return value;
}

static int task_completed(asx_task_id id) {
    asx_task_state s;
    return asx_task_get_state(id, &s) == ASX_OK && s == ASX_TASK_COMPLETED;
}

/* Steps that never suspend; the only ones allowed inside `masked`. */
static step_result exec_sync(it_task *t, asx_task_id self, uint32_t step, uint32_t idx,
                             const char *label) {
    const char *op = it_str(step, "op");
    asx_status st;

    if (op == NULL) {
        it_fail_task(t, idx, "step without op");
        return STEP_END;
    }
    if (strcmp(op, "checkpoint") == 0) {
        asx_checkpoint_result cr;
        const char *on_cancel = it_str(step, "on_cancel");
        st = asx_checkpoint(self, &cr);
        if (st != ASX_OK) {
            it_fail_task(t, idx, "asx_checkpoint failed");
            return STEP_END;
        }
        if (cr.cancelled) {
            observe_status(t, idx, label, ASX_E_CANCELLED);
            if (on_cancel == NULL || strcmp(on_cancel, "return") == 0) {
                t->end = ASX_OK; /* cancellation wins: the outcome is Cancelled */
                return STEP_END;
            }
            return STEP_NEXT;
        }
        observe_status(t, idx, label, ASX_OK);
        return STEP_NEXT;
    }
    if (strcmp(op, "trace") == 0) {
        const char *message = it_str(step, "message");
        if (message == NULL) {
            it_fail_task(t, idx, "trace without message");
            return STEP_END;
        }
        asx_trace_user(self, message);
        observe_status(t, idx, label, ASX_OK);
        return STEP_NEXT;
    }
    if (strcmp(op, "reserve") == 0) {
        asx_obligation_kind kind;
        asx_obligation_id id = ASX_INVALID_ID;
        const char *name = it_str(step, "as");
        if (!obligation_kind_parse(it_str(step, "kind"), &kind) || name == NULL) {
            it_fail_task(t, idx, "reserve needs a Rust obligation kind and `as`");
            return STEP_END;
        }
        if (t->n_obligations >= IT_MAX_LOCAL) {
            it_fail_task(t, idx, "too many obligations in one task");
            return STEP_END;
        }
        st = asx_obligation_reserve_ex(t->region, kind, self, &id);
        if (st == ASX_OK) {
            t->obligations[t->n_obligations].name = name;
            t->obligations[t->n_obligations].id = id;
            t->n_obligations++;
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "commit") == 0 || strcmp(op, "abort") == 0 || strcmp(op, "leak") == 0) {
        it_local_obligation *o = local_obligation(t, it_str(step, "obligation"));
        if (o == NULL) {
            it_fail_task(t, idx, "unknown obligation");
            return STEP_END;
        }
        if (strcmp(op, "commit") == 0) {
            st = asx_obligation_commit(o->id);
        } else if (strcmp(op, "abort") == 0) {
            const char *reason = it_str(step, "reason");
            asx_obligation_abort_reason why;
            if (reason != NULL && strcmp(reason, "Explicit") == 0) {
                why = ASX_OBLIGATION_ABORT_EXPLICIT;
            } else if (reason != NULL && strcmp(reason, "Cancel") == 0) {
                why = ASX_OBLIGATION_ABORT_CANCEL;
            } else if (reason != NULL && strcmp(reason, "Error") == 0) {
                why = ASX_OBLIGATION_ABORT_ERROR;
            } else {
                it_fail_task(t, idx, "abort needs reason Explicit, Cancel or Error");
                return STEP_END;
            }
            st = asx_obligation_abort_with_reason(o->id, why);
        } else {
            /* Dropped: left reserved, so the runtime resolves it when the
             * holder completes (a leak unless the holder was cancelled). */
            st = ASX_OK;
        }
        o->name = NULL;
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "mutex_unlock") == 0 || strcmp(op, "sem_release") == 0) {
        /* Release the most recent guard or permit held on the object. */
        int is_mutex = strcmp(op, "mutex_unlock") == 0;
        const char *name = it_str(step, is_mutex ? "mutex" : "semaphore");
        uint32_t i = is_mutex ? t->n_guards : t->n_permits;
        while (i > 0u) {
            const char *held = is_mutex ? t->guards[i - 1u].name : t->permits[i - 1u].name;
            if (name != NULL && strcmp(held, name) == 0) break;
            i--;
        }
        if (i == 0u) {
            it_fail_task(t, idx, "release of a guard or permit this task does not hold");
            return STEP_END;
        }
        i--;
        st = is_mutex ? release_guard(t, i) : release_permit(t, i);
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "notify_one") == 0 || strcmp(op, "notify_all") == 0) {
        it_sync *s = sync_by_name(it_str(step, "notify"), IT_SYNC_NOTIFY);
        if (s == NULL) {
            it_fail_task(t, idx, "notify on an undeclared notify");
            return STEP_END;
        }
        st = strcmp(op, "notify_one") == 0 ? asx_notify_one(s->notify) : asx_notify_all(s->notify);
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    /* Non-blocking mpsc steps (DSL §3.6). */
    if (strcmp(op, "permit_send") == 0 || strcmp(op, "permit_abort") == 0) {
        uint32_t i = send_permit_index(t, it_str(step, "permit"));
        uint64_t v = 0;
        if (i >= t->n_send_permits ||
            (strcmp(op, "permit_send") == 0 &&
             !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &v))) {
            it_fail_task(t, idx, "permit step on a permit this task does not hold");
            return STEP_END;
        }
        if (strcmp(op, "permit_send") == 0) {
            st = asx_send_permit_send(&t->send_permits[i].permit, v);
        } else {
            asx_send_permit_abort(&t->send_permits[i].permit);
            st = ASX_OK;
        }
        for (; i + 1u < t->n_send_permits; i++) t->send_permits[i] = t->send_permits[i + 1u];
        t->n_send_permits--;
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "close_sender") == 0 || strcmp(op, "close_receiver") == 0) {
        int sender = strcmp(op, "close_sender") == 0;
        it_channel *c = owned_endpoint(t, it_str(step, "channel"), sender);
        uint32_t i;
        if (c == NULL) {
            it_fail_task(t, idx, "close of an endpoint this task does not own");
            return STEP_END;
        }
        for (i = 0; sender && i < t->n_send_permits; i++) {
            if (t->send_permits[i].channel == (uint32_t)(c - g_channels)) {
                /* A Rust body cannot drop a sender a held permit borrows. */
                it_fail_task(t, idx, "close_sender while holding one of its permits");
                return STEP_END;
            }
        }
        close_endpoint(c, sender);
        observe_status(t, idx, label, ASX_OK);
        return STEP_NEXT;
    }
    if (strcmp(op, "try_send") == 0 || strcmp(op, "try_recv") == 0) {
        it_fail_task(t, idx,
                     "try_send / try_recv: C's asx_channel_try_reserve / try_recv park the "
                     "calling task when full / empty inside a poll; Rust's never park (DSL §7)");
        return STEP_END;
    }
    if (strcmp(op, "spawn") == 0) {
        const char *name = it_str(step, "as");
        const char *in_region = it_str(step, "region");
        it_local_region *lr = NULL;
        it_task *child;
        asx_budget budget;
        int has_budget = parse_budget(asx_json_get(g_in, step, "budget"), &budget);
        asx_task_id id = ASX_INVALID_ID;
        if (in_region != NULL) {
            lr = local_region(t, in_region);
            if (lr == NULL) {
                it_fail_task(t, idx, "spawn into a region this task did not open");
                return STEP_END;
            }
        }
        child = add_task(name, lr != NULL ? lr->name : t->region_name,
                         lr != NULL ? lr->id : t->region, asx_json_get(g_in, step, "program"));
        if (child == NULL) return STEP_END;
        st = asx_task_spawn_with_budget(child->region, interp_poll, child,
                                        has_budget ? &budget : NULL, &id);
        if (st == ASX_OK) {
            child->id = id;
            child->spawned = 1;
            (void)asx_cx_init(&child->cx, child->region, id, ASX_CAP_CANCEL_CHECK);
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "try_join") == 0) {
        it_task *target = task_by_name(it_str(step, "task"));
        if (target == NULL || !target->spawned || target->joined) {
            it_fail_task(t, idx, "try_join of an unknown or already joined task");
            return STEP_END;
        }
        if (!task_completed(target->id)) {
            observe_status(t, idx, label, ASX_E_TASK_NOT_COMPLETED);
            return STEP_NEXT;
        }
        {
            uint32_t value = take_outcome(target, &st);
            observe(t, idx, label, status_node(st), value);
        }
        return STEP_NEXT;
    }
    if (strcmp(op, "abort_task") == 0) {
        it_task *target = task_by_name(it_str(step, "task"));
        asx_cancel_kind kind;
        if (target == NULL || !target->spawned) {
            it_fail_task(t, idx, "abort_task of an unknown task");
            return STEP_END;
        }
        if (!cancel_kind_parse(it_str(step, "kind"), &kind)) {
            it_fail_task(t, idx, "abort_task with an unknown cancel kind");
            return STEP_END;
        }
        {
            /* The requesting task initiates the cancel (DSL §4). */
            asx_cancel_reason r = make_reason(kind, t->region, self, it_str(step, "message"));
            st = asx_task_cancel_with_reason(target->id, &r);
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "cancel_region") == 0) {
        it_local_region *lr = local_region(t, it_str(step, "region"));
        asx_cancel_kind kind;
        if (lr == NULL) {
            it_fail_task(t, idx, "cancel_region of a region this task did not open");
            return STEP_END;
        }
        if (!cancel_kind_parse(it_str(step, "kind"), &kind)) {
            it_fail_task(t, idx, "cancel_region with an unknown cancel kind");
            return STEP_END;
        }
        {
            asx_cancel_reason r = make_reason(kind, t->region, self, it_str(step, "message"));
            st = asx_region_cancel(lr->id, &r, NULL);
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    {
        char what[160];
        (void)snprintf(what, sizeof(what), "op \"%s\" is not interpreted (or blocks inside masked)",
                       op);
        it_fail_task(t, idx, what);
    }
    return STEP_END;
}

static step_result exec_step(it_task *t, asx_task_id self, uint32_t step, uint32_t idx) {
    const char *op = it_str(step, "op");
    asx_status st;

    if (op == NULL) {
        it_fail_task(t, idx, "step without op");
        return STEP_END;
    }
    {
        step_result r;
        if (exec_sync_wait(t, self, step, idx, op, &r)) return r;
        if (exec_channel_wait(t, step, idx, op, &r)) return r;
    }
    if (strcmp(op, "yield") == 0) {
        if (t->phase == 0u) {
            t->phase = 1u; /* stay runnable: one trip through the scheduler */
            return STEP_PENDING;
        }
        observe_status(t, idx, op, ASX_OK);
        return STEP_NEXT;
    }
    if (strcmp(op, "sleep") == 0 || strcmp(op, "sleep_until") == 0) {
        /* The runtime's Sleep (asx_sleep_poll) carries Rust's semantics:
         * traced timer, early completion on an observable cancel, one extra
         * trip when first polled after it. */
        if (t->phase == 0u) {
            uint64_t v;
            int is_sleep = strcmp(op, "sleep") == 0;
            if (!asx_json_u64(g_in, asx_json_get(g_in, step, is_sleep ? "ns" : "at_ns"), &v)) {
                it_fail_task(t, idx, "sleep without its time");
                return STEP_END;
            }
            st = is_sleep ? asx_sleep_init(&t->sleep, v) : asx_sleep_init_until(&t->sleep, v);
            if (st != ASX_OK) {
                it_fail_task(t, idx, "sleep init failed");
                return STEP_END;
            }
            t->phase = 1u;
        }
        st = asx_sleep_poll(&t->sleep, self);
        if (st == ASX_E_PENDING) return STEP_PENDING;
        observe_status(t, idx, op, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "return") == 0) {
        uint32_t outcome = asx_json_get(g_in, step, "outcome");
        const char *tag = it_str(outcome, "tag");
        if (tag == NULL) {
            it_fail_task(t, idx, "return without outcome tag");
            return STEP_END;
        }
        if (strcmp(tag, "ok") == 0) {
            t->end = ASX_OK;
        } else if (strcmp(tag, "err") == 0) {
            asx_status err;
            if (!asx_status_from_name(it_str(outcome, "status"), &err) || err == ASX_OK ||
                err == ASX_E_PENDING) {
                it_fail_task(t, idx, "return err needs an error status name");
                return STEP_END;
            }
            t->end = err;
        } else if (strcmp(tag, "panicked") == 0) {
            /* The step is a panic: the observation is recorded first, as in
             * Rust where the step's observation precedes panic_any. */
            observe_status(t, idx, op, ASX_OK);
            if (asx_task_panic(self, it_str(outcome, "message")) != ASX_OK) {
                it_fail_task(t, idx, "asx_task_panic failed");
            }
            t->end = ASX_OK;
            return STEP_END;
        } else {
            it_fail_task(t, idx, "unknown outcome tag");
            return STEP_END;
        }
        observe_status(t, idx, op, ASX_OK);
        return STEP_END;
    }
    if (strcmp(op, "masked") == 0) {
        uint32_t steps = asx_json_get(g_in, step, "steps");
        uint32_t n = asx_json_count(g_in, steps);
        uint32_t j;
        step_result r = STEP_NEXT;
        if (asx_task_mask(self) != ASX_OK) {
            it_fail_task(t, idx, "asx_task_mask failed");
            return STEP_END;
        }
        for (j = 0; j < n && r == STEP_NEXT; j++) {
            uint32_t inner = asx_json_item(g_in, steps, j);
            char label[96];
            const char *inner_op = it_str(inner, "op");
            (void)snprintf(label, sizeof(label), "masked/%u/%s", (unsigned)(j + 1u),
                           inner_op != NULL ? inner_op : "");
            r = exec_sync(t, self, inner, idx, label);
        }
        if (asx_task_unmask(self) != ASX_OK) it_fail_task(t, idx, "asx_task_unmask failed");
        return r;
    }
    if (strcmp(op, "join") == 0) {
        it_task *target = task_by_name(it_str(step, "task"));
        asx_outcome ignored;
        if (target == NULL || !target->spawned || target->joined) {
            it_fail_task(t, idx, "join of an unknown or already joined task");
            return STEP_END;
        }
        if (task_completed(target->id)) {
            uint32_t value = take_outcome(target, &st);
            observe(t, idx, op, status_node(st), value);
            return STEP_NEXT;
        }
        st = asx_task_join_poll(self, target->id, &ignored);
        if (st != ASX_E_PENDING) {
            it_fail_task(t, idx, "join of a live task did not wait");
            return STEP_END;
        }
        return STEP_PENDING;
    }
    if (strcmp(op, "open_region") == 0) {
        const char *name = it_str(step, "as");
        asx_budget budget;
        int has_budget = parse_budget(asx_json_get(g_in, step, "budget"), &budget);
        asx_region_id id = ASX_INVALID_ID;
        if (name == NULL || t->n_regions >= IT_MAX_LOCAL) {
            it_fail_task(t, idx, "open_region needs `as` (and a free local slot)");
            return STEP_END;
        }
        st = asx_region_open_child_with_budget(t->region, has_budget ? &budget : NULL, &id);
        if (st == ASX_OK) {
            if (!add_region(name, t->region_name, id)) return STEP_END;
            t->regions[t->n_regions].name = name;
            t->regions[t->n_regions].id = id;
            t->n_regions++;
        }
        observe_status(t, idx, op, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "close_region") == 0) {
        it_local_region *lr = local_region(t, it_str(step, "region"));
        asx_region_state rs;
        if (lr == NULL) {
            it_fail_task(t, idx, "close_region of a region this task did not open");
            return STEP_END;
        }
        if (t->phase == 0u) {
            /* ChildRegion::close enqueues a Close command, which the runtime
             * runs as a User cancel of the region with this message
             * (lab/runtime.rs:4195): cancel the remaining tasks, close.
             * Rust stamps it with CancelReason::user's testing defaults;
             * C attributes it to the closing task (DSL §4). */
            asx_cancel_reason r =
                make_reason(ASX_CANCEL_USER, t->region, self, "owned child region body finished");
            st = asx_region_cancel(lr->id, &r, NULL);
            if (st != ASX_OK) {
                observe_status(t, idx, op, st);
                return STEP_NEXT;
            }
            t->phase = 1u;
        }
        /* Wait for Closed: the region finalizes when its last task ends. */
        if (asx_region_get_state(lr->id, &rs) != ASX_OK || rs != ASX_REGION_CLOSED) {
            return STEP_PENDING;
        }
        observe_status(t, idx, op, ASX_OK);
        return STEP_NEXT;
    }
    return exec_sync(t, self, step, idx, op);
}

/* The poll function of every interpreted task: run steps from the program
 * counter until one suspends or the program ends (DSL §2, "one poll runs
 * steps until one suspends"). */
static asx_status interp_poll(void *user_data, asx_task_id self) {
    it_task *t = (it_task *)user_data;
    uint32_t count = asx_json_count(g_in, t->program);
    if (g_failed) return ASX_E_INVALID_STATE;
    while (t->pc < count) {
        step_result r = exec_step(t, self, asx_json_item(g_in, t->program, t->pc), t->pc + 1u);
        if (g_failed) return ASX_E_INVALID_STATE;
        if (r == STEP_PENDING) return ASX_E_PENDING;
        t->pc++;
        t->phase = 0u;
        if (r == STEP_END) {
            drop_locals(t);
            return t->end;
        }
    }
    drop_locals(t);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

typedef asx_status (*it_run_fn)(asx_region_id region, asx_budget *budget);

/* Run the scheduler the way the lab runs: to idle (or quiescence), whatever
 * a task returns. Under fail-fast containment (the DEBUG safety profile)
 * asx_scheduler_run hands a task's error status back to its caller as soon
 * as the task fails; nothing is poisoned or cancelled, and resuming the run
 * continues it (test_scheduler_wake: join_poll_reports_error_outcome). Rust
 * records the Err outcome and keeps running (its FailFast policy action is
 * not applied by the 0.6.0 runtime, types/policy.rs:69-75), so the driver
 * resumes. The HARDENED profile's POISON_REGION policy also cancels the
 * region's other tasks: a semantic divergence from Rust that conformance
 * runs in that profile would report. Running out of polls means
 * lab.max_steps was exceeded (DSL §1). */
static void run_scheduler(it_run_fn run, asx_region_id root, asx_budget *budget,
                          const char *where) {
    uint32_t surfaced = 0;
    for (;;) {
        asx_status st = run(root, budget);
        if (st == ASX_OK || st == ASX_E_PENDING || st == ASX_E_WOULD_BLOCK) return;
        if (st == ASX_E_POLL_BUDGET_EXHAUSTED) {
            it_fail("lab.max_steps exceeded", where);
            return;
        }
        /* Each surfaced fault is one task completing; more than every task
         * failing several times over means the run is not progressing. */
        if (g_failed || ++surfaced > 4u * IT_MAX_TASKS) {
            const char *name = asx_status_name(st);
            char buf[128];
            (void)snprintf(buf, sizeof(buf), "%s keeps returning %s", where,
                           name != NULL ? name : "?");
            it_fail("scheduler run failed", buf);
            return;
        }
    }
}

static void apply_driver_op(uint32_t op) {
    const char *name = it_str(op, "op");
    asx_cancel_kind kind;
    if (name == NULL) {
        it_fail("script entry without op", NULL);
        return;
    }
    if (strcmp(name, "cancel_region") == 0 || strcmp(name, "close_region") == 0) {
        it_region *r = region_by_name(it_str(op, "region"));
        asx_cancel_reason reason;
        if (r == NULL || !cancel_kind_parse(it_str(op, "kind"), &kind)) {
            it_fail("cancel_region/close_region needs a known region and kind", NULL);
            return;
        }
        /* No task requests it: the cancel originates at the target region
         * (DSL §4). A region cancel also begins closing the subtree, so
         * close_region is the same request followed by running until the
         * region is Closed (twin_run: cancel_request + advance). */
        reason = make_reason(kind, r->id, ASX_INVALID_ID, it_str(op, "message"));
        if (asx_region_cancel(r->id, &reason, NULL) != ASX_OK) {
            it_fail("asx_region_cancel failed", r->name);
            return;
        }
        if (strcmp(name, "close_region") == 0) {
            uint32_t round;
            for (round = 0; round < 1000u && !g_failed; round++) {
                asx_region_state s;
                if (asx_region_get_state(r->id, &s) == ASX_OK && s == ASX_REGION_CLOSED) return;
                run_scheduler(asx_scheduler_run_until_idle, g_root, g_budget, "close_region");
            }
            it_fail("close_region: region did not close within 1000 idle rounds", r->name);
        }
        return;
    }
    if (strcmp(name, "cancel_task") == 0) {
        it_task *t = task_by_name(it_str(op, "task"));
        asx_cancel_reason reason;
        if (t == NULL || !t->spawned || !cancel_kind_parse(it_str(op, "kind"), &kind)) {
            it_fail("cancel_task needs a known task and kind", NULL);
            return;
        }
        /* A driver cancel originates at the task's own region (DSL §4). */
        reason = make_reason(kind, t->region, ASX_INVALID_ID, it_str(op, "message"));
        if (asx_task_cancel_with_reason(t->id, &reason) != ASX_OK && !t->joined) {
            it_fail("cancel_task failed", t->name);
        }
        return;
    }
    if (strcmp(name, "advance") == 0) {
        uint64_t ns;
        if (!asx_json_u64(g_in, asx_json_get(g_in, op, "ns"), &ns)) {
            it_fail("advance without ns", NULL);
            return;
        }
        asx_runtime_virtual_advance(asx_runtime_virtual_now() + ns);
        return;
    }
    it_fail("driver op is not interpreted", name);
}

/* ------------------------------------------------------------------ */
/* Projection                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_obligation_id id;
    uint32_t name_off; /* in g_text */
    uint32_t state_node;
} it_obligation_name;

static it_obligation_name g_obl_names[IT_MAX_OBLIGATION_NAMES];
static uint32_t g_n_obl_names;

static const char *obligation_name(asx_obligation_id id, const it_task *holder) {
    uint32_t i;
    uint32_t k = 1;
    char buf[160];
    for (i = 0; i < g_n_obl_names; i++) {
        if (same_entity(g_obl_names[i].id, id)) return &g_text[g_obl_names[i].name_off];
    }
    /* "<holder>/o<k>": k counts this holder's reservations (vocabulary §2). */
    for (i = 0; i < g_n_obl_names; i++) {
        const char *n = &g_text[g_obl_names[i].name_off];
        size_t hl = strlen(holder->name);
        if (strncmp(n, holder->name, hl) == 0 && n[hl] == '/' && n[hl + 1u] == 'o') k++;
    }
    if (g_n_obl_names >= IT_MAX_OBLIGATION_NAMES) {
        it_fail("too many obligations", NULL);
        return "";
    }
    (void)snprintf(buf, sizeof(buf), "%s/o%u", holder->name, (unsigned)k);
    g_obl_names[g_n_obl_names].id = id;
    g_obl_names[g_n_obl_names].name_off = text_store(buf, strlen(buf));
    g_obl_names[g_n_obl_names].state_node = ASX_JSON_NONE;
    g_n_obl_names++;
    if (g_obl_names[g_n_obl_names - 1u].name_off == IT_NO_TEXT) return "";
    return &g_text[g_obl_names[g_n_obl_names - 1u].name_off];
}

static uint32_t event_object(const char *k) {
    uint32_t o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "k", asx_json_new_string(g_out, k));
    return o;
}

static const char *region_name_of(asx_region_id id) {
    const it_region *r = region_by_id(id);
    if (r == NULL) {
        it_fail("event names an unknown region", NULL);
        return "";
    }
    return r->name;
}

static const char *task_name_of(asx_task_id id) {
    const it_task *t = task_by_id(id);
    if (t == NULL) {
        it_fail("event names an unknown task", NULL);
        return "";
    }
    return t->name;
}

/* Project the captured runtime events, in emission order, and collect the
 * obligation records for the snapshot. */
static uint32_t project_events(uint32_t obligations) {
    uint32_t events = asx_json_new_array(g_out);
    uint32_t i;
    for (i = 0; i < g_n_events && !g_failed; i++) {
        const it_event *e = &g_events[i];
        uint32_t ev = ASX_JSON_NONE;
        switch (e->kind) {
        case ASX_TRACE_TASK_SPAWN:
            ev = event_object("task.spawned");
            asx_json_set(g_out, ev, "task",
                         asx_json_new_string(g_out, task_name_of((asx_task_id)e->entity)));
            asx_json_set(g_out, ev, "region",
                         asx_json_new_string(g_out, region_name_of((asx_region_id)e->aux)));
            break;
        case ASX_TRACE_SCHED_COMPLETE: {
            const it_task *t = task_by_id((asx_task_id)e->entity);
            ev = event_object("task.completed");
            if (t == NULL) {
                it_fail("completion of an unknown task", NULL);
                break;
            }
            asx_json_set(g_out, ev, "task", asx_json_new_string(g_out, t->name));
            asx_json_set(g_out, ev, "region", asx_json_new_string(g_out, t->region_name));
            break;
        }
        case ASX_TRACE_REGION_OPEN: {
            const it_region *r = region_by_id((asx_region_id)e->entity);
            ev = event_object("region.created");
            if (r == NULL) {
                it_fail("creation of an unknown region", NULL);
                break;
            }
            asx_json_set(g_out, ev, "region", asx_json_new_string(g_out, r->name));
            asx_json_set(g_out, ev, "parent",
                         r->parent != NULL ? asx_json_new_string(g_out, r->parent)
                                           : asx_json_new_null(g_out));
            break;
        }
        case ASX_TRACE_REGION_CLOSE:
            ev = event_object("region.close_begin");
            asx_json_set(g_out, ev, "region",
                         asx_json_new_string(g_out, region_name_of((asx_region_id)e->entity)));
            break;
        case ASX_TRACE_REGION_CLOSED:
            ev = event_object("region.closed");
            asx_json_set(g_out, ev, "region",
                         asx_json_new_string(g_out, region_name_of((asx_region_id)e->entity)));
            break;
        case ASX_TRACE_CANCEL_REQUEST: {
            const it_task *t = task_by_id((asx_task_id)e->entity);
            ev = event_object("cancel.requested");
            if (t == NULL || !e->has_reason) {
                it_fail("cancel.requested of an unknown task", NULL);
                break;
            }
            asx_json_set(g_out, ev, "task", asx_json_new_string(g_out, t->name));
            asx_json_set(g_out, ev, "region", asx_json_new_string(g_out, t->region_name));
            asx_json_set(g_out, ev, "reason", reason_node(&e->reason));
            break;
        }
        case ASX_TRACE_OBLIGATION_RESERVE:
        case ASX_TRACE_OBLIGATION_COMMIT:
        case ASX_TRACE_OBLIGATION_ABORT:
        case ASX_TRACE_OBLIGATION_LEAK: {
            const it_task *holder = task_by_id(e->obligation.holder);
            const char *kind = obligation_kind_name(e->obligation.kind);
            const char *k;
            const char *state;
            const char *name;
            uint32_t record;
            if (holder == NULL || kind == NULL) {
                it_fail("obligation event without a named holder and Rust kind", NULL);
                break;
            }
            if (e->kind == ASX_TRACE_OBLIGATION_RESERVE) {
                k = "obligation.reserved";
                state = "Reserved";
            } else if (e->kind == ASX_TRACE_OBLIGATION_COMMIT) {
                k = "obligation.committed";
                state = "Committed";
            } else if (e->kind == ASX_TRACE_OBLIGATION_ABORT) {
                k = "obligation.aborted";
                state = "Aborted";
            } else {
                k = "obligation.leaked";
                state = "Leaked";
            }
            name = obligation_name((asx_obligation_id)e->entity, holder);
            ev = event_object(k);
            asx_json_set(g_out, ev, "obligation", asx_json_new_string(g_out, name));
            asx_json_set(g_out, ev, "task", asx_json_new_string(g_out, holder->name));
            asx_json_set(g_out, ev, "region",
                         asx_json_new_string(g_out, region_name_of(e->obligation.region)));
            asx_json_set(g_out, ev, "kind", asx_json_new_string(g_out, kind));
            record = asx_json_new_object(g_out);
            asx_json_set(g_out, record, "state", asx_json_new_string(g_out, state));
            asx_json_set(g_out, record, "kind", asx_json_new_string(g_out, kind));
            asx_json_set(g_out, record, "holder", asx_json_new_string(g_out, holder->name));
            asx_json_set(g_out, record, "region",
                         asx_json_new_string(g_out, region_name_of(e->obligation.region)));
            if (e->kind == ASX_TRACE_OBLIGATION_ABORT) {
                const char *why = abort_reason_name(e->obligation.abort_reason);
                if (why == NULL) {
                    it_fail("obligation aborted with a reason Rust does not have", NULL);
                    break;
                }
                asx_json_set(g_out, ev, "abort_reason", asx_json_new_string(g_out, why));
                asx_json_set(g_out, record, "abort_reason", asx_json_new_string(g_out, why));
            } else {
                asx_json_set(g_out, record, "abort_reason", asx_json_new_null(g_out));
            }
            asx_json_set(g_out, obligations, name, record);
            break;
        }
        case ASX_TRACE_USER:
            ev = event_object("user.trace");
            asx_json_set(g_out, ev, "message",
                         asx_json_new_string_len(g_out, &g_text[e->text_off], e->text_len));
            break;
        case ASX_TRACE_REGION_CANCELLED:
            ev = event_object("region.cancelled");
            if (!e->has_reason) {
                it_fail("region.cancelled without a reason", NULL);
                break;
            }
            asx_json_set(g_out, ev, "region",
                         asx_json_new_string(g_out, region_name_of((asx_region_id)e->entity)));
            asx_json_set(g_out, ev, "reason", reason_node(&e->reason));
            break;
        case ASX_TRACE_TIMER_SET:
        case ASX_TRACE_TIMER_FIRE:
        case ASX_TRACE_TIMER_CANCEL: {
            /* Task sleep timers carry the task as entity; timer-wheel
             * timers (a timer handle) have no DSL counterpart yet. */
            it_task *t = asx_handle_type_tag(e->entity) == ASX_TYPE_TASK
                             ? task_by_id((asx_task_id)e->entity)
                             : NULL;
            if (t == NULL) {
                it_fail("timer event that is not a task sleep timer", NULL);
                break;
            }
            if (e->kind == ASX_TRACE_TIMER_SET) {
                char buf[160];
                t->timers++;
                (void)snprintf(buf, sizeof(buf), "%s/tm%u", t->name, (unsigned)t->timers);
                t->timer_name_off = text_store(buf, strlen(buf));
                t->timer_pending = 1;
                t->timer_deadline = e->aux;
                ev = event_object("timer.scheduled");
                asx_json_set(g_out, ev, "timer",
                             asx_json_new_string(g_out, &g_text[t->timer_name_off]));
                asx_json_set(g_out, ev, "deadline_ns", asx_json_new_u64(g_out, e->aux));
                break;
            }
            if (t->timers == 0u) {
                it_fail("timer fired or cancelled before it was scheduled", t->name);
                break;
            }
            t->timer_pending = 0;
            ev = event_object(e->kind == ASX_TRACE_TIMER_FIRE ? "timer.fired" : "timer.cancelled");
            asx_json_set(g_out, ev, "timer",
                         asx_json_new_string(g_out, &g_text[t->timer_name_off]));
            break;
        }
        case ASX_TRACE_SCHED_POLL:
        case ASX_TRACE_SCHED_BUDGET:
        case ASX_TRACE_SCHED_QUIESCENT:
        case ASX_TRACE_SCHED_ROUND:
        case ASX_TRACE_TASK_TRANSITION:
        case ASX_TRACE_CHANNEL_SEND:
        case ASX_TRACE_CHANNEL_RECV:
        default: it_fail("unprojectable event recorded", NULL); break;
        }
        if (ev != ASX_JSON_NONE) asx_json_push(g_out, events, ev);
    }
    return events;
}

static uint32_t build_snapshot(uint32_t obligations) {
    uint32_t snap = asx_json_new_object(g_out);
    uint32_t tasks = asx_json_new_object(g_out);
    uint32_t regions = asx_json_new_object(g_out);
    uint32_t i;
    int quiescent = 1;

    for (i = 0; i < g_n_tasks; i++) {
        it_task *t = &g_tasks[i];
        uint32_t rec = asx_json_new_object(g_out);
        uint32_t outcome = ASX_JSON_NONE;
        uint32_t reason = ASX_JSON_NONE;
        const char *state = "Completed";
        if (!t->spawned) continue; /* a refused spawn has no task */
        if (t->joined) {
            outcome = asx_json_copy(g_out, g_out, t->outcome);
        } else {
            asx_task_state s;
            if (asx_task_get_state(t->id, &s) != ASX_OK) {
                it_fail("task state unreadable", t->name);
                continue;
            }
            state = task_state_name(s);
            if (s == ASX_TASK_COMPLETED) {
                outcome = outcome_node(t->id);
            } else {
                asx_cancel_reason r;
                quiescent = 0;
                if (asx_task_get_cancel_reason(t->id, &r) == ASX_OK) {
                    it_reason captured;
                    capture_reason(&r, &captured);
                    reason = reason_node(&captured);
                }
            }
        }
        if (outcome != ASX_JSON_NONE && reason == ASX_JSON_NONE) {
            uint32_t r = asx_json_get(g_out, outcome, "reason");
            if (r != ASX_JSON_NONE) reason = asx_json_copy(g_out, g_out, r);
        }
        asx_json_set(g_out, rec, "state", asx_json_new_string(g_out, state));
        asx_json_set(g_out, rec, "outcome",
                     outcome != ASX_JSON_NONE ? outcome : asx_json_new_null(g_out));
        asx_json_set(g_out, rec, "cancel_reason",
                     reason != ASX_JSON_NONE ? reason : asx_json_new_null(g_out));
        asx_json_set(g_out, rec, "cleanup_budget", asx_json_new_null(g_out));
        asx_json_set(g_out, tasks, t->name, rec);
    }
    for (i = 0; i < g_n_regions; i++) {
        it_region *r = &g_regions[i];
        uint32_t rec = asx_json_new_object(g_out);
        asx_region_state s;
        const char *state = "Closed";
        if (asx_region_get_state(r->id, &s) == ASX_OK) {
            state = region_state_name(s);
            /* Rust is_quiescent: no region may still be closing
             * (state.rs:7407-7413). */
            if (s != ASX_REGION_OPEN && s != ASX_REGION_CLOSED) quiescent = 0;
        }
        asx_json_set(g_out, rec, "state", asx_json_new_string(g_out, state));
        asx_json_set(g_out, rec, "parent",
                     r->parent != NULL ? asx_json_new_string(g_out, r->parent)
                                       : asx_json_new_null(g_out));
        /* A Closed region's record is gone in Rust (lab.state.region()
         * returns None), so its reason projects as null; a live region's
         * is its strengthened cancel reason. */
        {
            asx_cancel_reason cr;
            uint32_t reason = ASX_JSON_NONE;
            if (strcmp(state, "Closed") != 0 &&
                asx_region_get_cancel_reason(r->id, &cr) == ASX_OK) {
                it_reason captured;
                capture_reason(&cr, &captured);
                reason = reason_node(&captured);
            }
            asx_json_set(g_out, rec, "cancel_reason",
                         reason != ASX_JSON_NONE ? reason : asx_json_new_null(g_out));
        }
        asx_json_set(g_out, regions, r->name, rec);
    }
    {
        uint32_t m;
        for (m = asx_json_item(g_out, obligations, 0); m != ASX_JSON_NONE;
             m = g_out->nodes[m].next) {
            const char *st = asx_json_get_string(g_out, m, "state");
            if (st != NULL && strcmp(st, "Reserved") == 0) quiescent = 0;
        }
    }
    asx_json_set(g_out, snap, "now_ns", asx_json_new_u64(g_out, asx_runtime_virtual_now()));
    asx_json_set(g_out, snap, "quiescent", asx_json_new_bool(g_out, quiescent));
    asx_json_set(g_out, snap, "tasks", tasks);
    asx_json_set(g_out, snap, "regions", regions);
    asx_json_set(g_out, snap, "obligations", obligations);
    {
        /* Scheduled timers that never fired or were cancelled, ordered by
         * (deadline, name) as twin_run orders them. */
        uint32_t pending = asx_json_new_array(g_out);
        uint32_t order[IT_MAX_TASKS];
        uint32_t n = 0;
        uint32_t j;
        for (i = 0; i < g_n_tasks; i++) {
            uint32_t pos;
            if (!g_tasks[i].timer_pending) continue;
            pos = n++;
            while (pos > 0u) {
                const it_task *a = &g_tasks[order[pos - 1u]];
                const it_task *b = &g_tasks[i];
                if (a->timer_deadline < b->timer_deadline ||
                    (a->timer_deadline == b->timer_deadline &&
                     strcmp(&g_text[a->timer_name_off], &g_text[b->timer_name_off]) <= 0)) {
                    break;
                }
                order[pos] = order[pos - 1u];
                pos--;
            }
            order[pos] = i;
        }
        for (j = 0; j < n; j++) {
            const it_task *t = &g_tasks[order[j]];
            uint32_t o = asx_json_new_object(g_out);
            asx_json_set(g_out, o, "timer", asx_json_new_string(g_out, &g_text[t->timer_name_off]));
            asx_json_set(g_out, o, "deadline_ns", asx_json_new_u64(g_out, t->timer_deadline));
            asx_json_push(g_out, pending, o);
        }
        asx_json_set(g_out, snap, "timers_pending", pending);
    }
    asx_json_set(g_out, snap, "channels", asx_json_new_object(g_out));
    return snap;
}

/* Sort observations by (task, step, op), as twin_run does. */
static int observation_less(uint32_t a, uint32_t b) {
    const char *ta = asx_json_get_string(g_out, a, "task");
    const char *tb = asx_json_get_string(g_out, b, "task");
    uint64_t sa = 0;
    uint64_t sb = 0;
    int c = strcmp(ta, tb);
    if (c != 0) return c < 0;
    (void)asx_json_u64(g_out, asx_json_get(g_out, a, "step"), &sa);
    (void)asx_json_u64(g_out, asx_json_get(g_out, b, "step"), &sb);
    if (sa != sb) return sa < sb;
    return strcmp(asx_json_get_string(g_out, a, "op"), asx_json_get_string(g_out, b, "op")) < 0;
}

static uint32_t g_sort[4096];

static uint32_t sorted_observations(void) {
    uint32_t arr = asx_json_new_array(g_out);
    uint32_t n = 0;
    uint32_t m;
    uint32_t i;
    for (m = asx_json_item(g_out, g_observations, 0); m != ASX_JSON_NONE;
         m = g_out->nodes[m].next) {
        uint32_t pos = n;
        if (n >= sizeof(g_sort) / sizeof(g_sort[0])) {
            it_fail("too many observations", NULL);
            return arr;
        }
        while (pos > 0u && observation_less(m, g_sort[pos - 1u])) {
            g_sort[pos] = g_sort[pos - 1u];
            pos--;
        }
        g_sort[pos] = m;
        n++;
    }
    for (i = 0; i < n; i++) asx_json_push(g_out, arr, asx_json_copy(g_out, g_out, g_sort[i]));
    return arr;
}

/* must_fail (DSL §5): the named step returned the named status. */
static void check_expectation(uint32_t scenario, uint32_t observations) {
    uint32_t expect = asx_json_get(g_in, scenario, "expect");
    const char *kind = it_str(expect, "kind");
    uint32_t error;
    const char *task;
    const char *status;
    uint64_t step = 0;
    uint32_t m;
    if (kind == NULL) {
        it_fail("scenario without expect.kind", NULL);
        return;
    }
    if (strcmp(kind, "must_fail") != 0) return;
    error = asx_json_get(g_in, expect, "error");
    task = it_str(error, "task");
    status = it_str(error, "status");
    if (task == NULL || status == NULL ||
        !asx_json_u64(g_in, asx_json_get(g_in, error, "step"), &step)) {
        it_fail("must_fail without task, step and status", NULL);
        return;
    }
    for (m = asx_json_item(g_out, observations, 0); m != ASX_JSON_NONE; m = g_out->nodes[m].next) {
        uint64_t s = 0;
        if (strcmp(asx_json_get_string(g_out, m, "task"), task) != 0) continue;
        if (!asx_json_u64(g_out, asx_json_get(g_out, m, "step"), &s) || s != step) continue;
        if (strcmp(asx_json_get_string(g_out, m, "status"), status) != 0) {
            it_fail("must_fail expectation not met", status);
        }
        return;
    }
    it_fail("must_fail: no observation for the named step", task);
}

static void set_digest(uint32_t obj, const char *key, uint32_t node) {
    char digest[ASX_CANON_DIGEST_LEN];
    if (asx_canon_digest(g_out, node, digest) != ASX_OK) {
        it_fail("digest failed", key);
        return;
    }
    asx_json_set(g_out, obj, key, asx_json_new_string(g_out, digest));
}

asx_status asx_conformance_run(const asx_json_doc *in, uint32_t scenario, asx_json_doc *out,
                               uint32_t *out_root, char *error, size_t error_cap) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_budget budget;
    asx_region_id root = ASX_INVALID_ID;
    uint64_t max_steps = 0;
    uint32_t list;
    uint32_t i;
    uint32_t raw_events;
    uint32_t obligations;
    uint32_t trace = ASX_JSON_NONE;
    uint32_t snapshot;
    uint32_t observations;
    uint32_t result;
    uint32_t semantic;
    const char *schema;
    asx_status st;

    if (in == NULL || out == NULL || out_root == NULL) return ASX_E_INVALID_ARGUMENT;
    g_in = in;
    g_out = out;
    g_error = error;
    g_error_cap = error_cap;
    g_failed = 0;
    g_n_tasks = 0;
    g_n_regions = 0;
    g_n_events = 0;
    g_text_used = 0;
    g_n_obl_names = 0;
    if (error != NULL && error_cap > 0u) error[0] = '\0';

    schema = it_str(scenario, "schema");
    if (schema == NULL || strcmp(schema, "asx.scenario.v2") != 0) {
        it_fail("not an asx.scenario.v2 document", NULL);
        return ASX_E_INVALID_ARGUMENT;
    }
    if (!asx_json_u64(in, asx_json_get(in, asx_json_get(in, scenario, "lab"), "max_steps"),
                      &max_steps) ||
        max_steps == 0u || max_steps > UINT32_MAX) {
        it_fail("lab.max_steps must be a positive 32-bit count", NULL);
        return ASX_E_INVALID_ARGUMENT;
    }

    /* Runtime on its virtual clock (default stub hooks), trace observed. */
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    if (st != ASX_OK) {
        it_fail("runtime init failed", NULL);
        return st;
    }
    asx_trace_set_observer(it_observe, NULL);
    g_observations = asx_json_new_array(out);

    /* Sync objects (DSL §3.7); their arenas are not part of the runtime
     * reset. */
    asx_notify_reset();
    asx_semaphore_reset();
    asx_barrier_reset();
    g_n_sync = 0;
    list = asx_json_get(in, scenario, "sync");
    for (i = 0; i < asx_json_count(in, list) && !g_failed; i++) {
        uint32_t d = asx_json_item(in, list, i);
        const char *type = it_str(d, "type");
        it_sync *s;
        uint64_t n = 0;
        if (g_n_sync >= IT_MAX_SYNC || it_str(d, "name") == NULL || type == NULL) {
            it_fail("sync declaration needs a name and a type (and a free slot)", NULL);
            break;
        }
        s = &g_sync[g_n_sync++];
        memset(s, 0, sizeof(*s));
        s->name = it_str(d, "name");
        if (strcmp(type, "mutex") == 0) {
            s->type = IT_SYNC_MUTEX;
            st = asx_mutex_create(&s->mutex);
        } else if (strcmp(type, "notify") == 0) {
            s->type = IT_SYNC_NOTIFY;
            st = asx_notify_create(&s->notify);
        } else if (strcmp(type, "semaphore") == 0 &&
                   asx_json_u64(in, asx_json_get(in, d, "permits"), &n) && n <= UINT32_MAX) {
            s->type = IT_SYNC_SEMAPHORE;
            st = asx_semaphore_create((uint32_t)n, &s->semaphore);
        } else if (strcmp(type, "barrier") == 0 &&
                   asx_json_u64(in, asx_json_get(in, d, "parties"), &n) && n <= UINT32_MAX) {
            s->type = IT_SYNC_BARRIER;
            st = asx_barrier_create((uint32_t)n, &s->barrier);
        } else {
            it_fail("unknown or incomplete sync declaration", s->name);
            break;
        }
        if (st != ASX_OK) it_fail("cannot create sync object", s->name);
    }
    budget = asx_budget_from_polls((uint32_t)max_steps);

    /* Setup (DSL §2): root, regions, tasks in array order. */
    g_budget = &budget;
    st = asx_region_open(&root);
    g_root = root;
    if (st != ASX_OK || !add_region("root", NULL, root)) {
        it_fail("cannot open the root region", NULL);
    }
    list = asx_json_get(in, scenario, "regions");
    for (i = 0; i < asx_json_count(in, list) && !g_failed; i++) {
        uint32_t r = asx_json_item(in, list, i);
        const char *name = it_str(r, "name");
        it_region *parent = region_by_name(it_str(r, "parent"));
        asx_budget b;
        int has_budget = parse_budget(asx_json_get(in, r, "budget"), &b);
        asx_region_id id = ASX_INVALID_ID;
        if (name == NULL || parent == NULL) {
            it_fail("region needs a name and a declared parent", name);
            break;
        }
        if (asx_region_open_child_with_budget(parent->id, has_budget ? &b : NULL, &id) != ASX_OK) {
            it_fail("cannot open region", name);
            break;
        }
        (void)add_region(name, parent->name, id);
    }
    list = asx_json_get(in, scenario, "tasks");
    for (i = 0; i < asx_json_count(in, list) && !g_failed; i++) {
        uint32_t tn = asx_json_item(in, list, i);
        it_region *r = region_by_name(it_str(tn, "region"));
        it_task *t;
        asx_budget b;
        int has_budget = parse_budget(asx_json_get(in, tn, "budget"), &b);
        asx_task_id id = ASX_INVALID_ID;
        if (r == NULL) {
            it_fail("task in an undeclared region", it_str(tn, "name"));
            break;
        }
        t = add_task(it_str(tn, "name"), r->name, r->id, asx_json_get(in, tn, "program"));
        if (t == NULL) break;
        if (asx_task_spawn_with_budget(r->id, interp_poll, t, has_budget ? &b : NULL, &id) !=
            ASX_OK) {
            it_fail("cannot spawn task", t->name);
            break;
        }
        t->id = id;
        t->spawned = 1;
        (void)asx_cx_init(&t->cx, t->region, id, ASX_CAP_CANCEL_CHECK);
    }

    /* Channels (DSL §3.6), each endpoint bound to its owner, a top-level
     * task. They live in the root region: Rust channels belong to none. */
    asx_channel_reset();
    g_n_channels = 0;
    list = asx_json_get(in, scenario, "channels");
    for (i = 0; i < asx_json_count(in, list) && !g_failed; i++) {
        uint32_t d = asx_json_item(in, list, i);
        const char *name = it_str(d, "name");
        const char *type = it_str(d, "type");
        it_task *tx = task_by_name(it_str(d, "sender"));
        it_task *rx = task_by_name(it_str(d, "receiver"));
        uint64_t cap = 0;
        it_channel *c;
        if (type == NULL || strcmp(type, "mpsc") != 0) {
            it_fail("only mpsc channels are interpreted yet (increment 2c)", name);
            break;
        }
        if (g_n_channels >= IT_MAX_CHANNELS || name == NULL || tx == NULL || rx == NULL ||
            !asx_json_u64(in, asx_json_get(in, d, "capacity"), &cap) || cap > UINT32_MAX) {
            it_fail("mpsc declaration needs a name, a capacity and top-level owner tasks", name);
            break;
        }
        c = &g_channels[g_n_channels++];
        c->name = name;
        c->sender = (uint32_t)(tx - g_tasks);
        c->receiver = (uint32_t)(rx - g_tasks);
        c->sender_open = 1;
        c->receiver_open = 1;
        if (asx_channel_create(root, (uint32_t)cap, &c->id) != ASX_OK) {
            it_fail("cannot create channel", name);
        }
    }

    /* Script (DSL §2): run to idle, advance time, apply the op. */
    list = asx_json_get(in, scenario, "script");
    for (i = 0; i < asx_json_count(in, list) && !g_failed; i++) {
        uint32_t entry = asx_json_item(in, list, i);
        uint64_t at = 0;
        run_scheduler(asx_scheduler_run_until_idle, root, &budget, "run_until_idle");
        if (g_failed) break;
        if (!asx_json_u64(in, asx_json_get(in, entry, "at_ns"), &at)) {
            it_fail("script entry without at_ns", NULL);
            break;
        }
        if (at > asx_runtime_virtual_now()) asx_runtime_virtual_advance(at);
        apply_driver_op(asx_json_get(in, entry, "op"));
    }

    /* Finish (DSL §2): run to quiescence with timer auto-advance. */
    if (!g_failed) run_scheduler(asx_scheduler_run, root, &budget, "run_with_auto_advance");
    asx_trace_set_observer(NULL, NULL);
    if (g_failed) return ASX_E_INVALID_STATE;

    /* Project (DSL §2). */
    obligations = asx_json_new_object(out);
    raw_events = project_events(obligations);
    if (!g_failed && asx_canon_trace(out, raw_events, out, &trace) != ASX_OK) {
        it_fail("canonicalization failed", asx_canon_error());
    }
    snapshot = build_snapshot(obligations);
    observations = sorted_observations();
    if (!g_failed) check_expectation(scenario, observations);
    if (g_failed) return ASX_E_INVALID_STATE;

    result = asx_json_new_object(out);
    semantic = asx_json_new_object(out);
    asx_json_set(out, semantic, "observations", asx_json_copy(out, out, observations));
    asx_json_set(out, semantic, "snapshot", asx_json_copy(out, out, snapshot));
    asx_json_set(out, semantic, "trace", asx_json_copy(out, out, trace));
    asx_json_set(out, semantic, "vocabulary", asx_json_new_string(out, "asx.vocab.v2"));
    asx_json_set(out, result, "scenario_id",
                 asx_json_copy(out, in, asx_json_get(in, scenario, "id")));
    asx_json_set(out, result, "vocabulary", asx_json_new_string(out, "asx.vocab.v2"));
    asx_json_set(out, result, "trace", trace);
    asx_json_set(out, result, "snapshot", snapshot);
    asx_json_set(out, result, "observations", observations);
    set_digest(result, "trace_digest", trace);
    set_digest(result, "snapshot_digest", snapshot);
    set_digest(result, "semantic_digest", semantic);
    if (g_failed) return ASX_E_INVALID_STATE;
    if (!asx_json_doc_ok(out)) {
        it_fail("output document exhausted", out->error);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    *out_root = result;
    return ASX_OK;
}
