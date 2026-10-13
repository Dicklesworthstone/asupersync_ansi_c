/*
 * conformance/interpreter.c — runs asx.scenario.v2 scenarios through the
 * asx runtime and projects them into vocabulary v2 (bead W1.5,
 * bd-9kll.2.5). The C twin of tools/twin_run/src/run.rs.
 *
 * Contracts: docs/SCENARIO_DSL_V2.md (execution model, per-step C APIs) and
 * docs/CANONICAL_VOCABULARY_V2.md (projection, snapshot, observations).
 *
 * Every event comes from the runtime's own trace stream (a trace observer), never from the
 * interpreter: an interpreter that emitted events would test itself. Where the C runtime cannot
 * express a step yet (a gap listed in DSL §7), the step fails the run with that gap named, so a
 * missing capability is never mistaken for parity.
 *
 * SPDX-License-Identifier: MIT
 */

#include "interpreter.h"

#include "canon.h"

#include <asx/actor/actor.h>
#include <asx/actor/supervisor.h>
#include <asx/asx.h>
#include <asx/core/broadcast.h>
#include <asx/core/oneshot.h>
#include <asx/core/watch.h>
#include <asx/cx/cx.h>
#include <asx/runtime/overload_catalog.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/task_group.h>
#include <asx/sync/barrier.h>
#include <asx/sync/mutex.h>
#include <asx/sync/notify.h>
#include <asx/sync/rwlock.h>
#include <asx/sync/semaphore.h>
#include <asx/time/sleep.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/lab.h>
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS

#include <stdio.h>
#include <string.h>

/* Private channel destruction boundary; importing runtime_internal.h
 * would collide with this interpreter's independent scenario arenas. */
void asx_send_permit_drop_internal(asx_send_permit *permit);
void asx_region_advance_internal(asx_region_id region);

/* ------------------------------------------------------------------ */
/* Bounds                                                              */
/* ------------------------------------------------------------------ */

/* Names, never reused: a supervisor's generations each name a task and a
 * region of their own while the runtime recycles the slots. */
#define IT_MAX_TASKS (ASX_MAX_TASKS * 2u)
#define IT_MAX_REGIONS (ASX_MAX_REGIONS * 8u + 1u)
#define IT_MAX_LOCAL 16u
#define IT_MAX_EVENTS 4096u
#define IT_MAX_DISPATCHES 16384u
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
    int spawned;                  /* asx_task_spawn succeeded */
    asx_status refused;           /* why asx_task_spawn failed (ASX_OK if it did not) */
    uint32_t refusal;             /* its lab refusal ticket (asx_task_await_refusal) */
    uint32_t program;             /* array node in the scenario document */
    uint32_t pc;                  /* 0-based index of the current step */
    uint32_t phase;               /* suspension state of the current step */
    asx_sleep_state sleep;        /* the current sleep step */
    asx_status end;               /* poll result once the program has ended */
    int panicking;                /* the program ended with a panicked return */
    uint32_t spawn_obs;           /* its spawn step's observation (ASX_JSON_NONE if none) */
    asx_status admission_refusal; /* why its admission refused it (ASX_OK: it did not) */
    int joined;                   /* outcome taken by a join; the slot is gone */
    uint32_t outcome;             /* projected outcome node (out document) when joined */
    it_local_obligation obligations[IT_MAX_LOCAL];
    uint32_t n_obligations;
    it_local_region regions[IT_MAX_LOCAL];
    uint32_t n_regions;
    /* The region the current close_region step waits on: close takes the
     * handle out of `regions` (Rust's ChildRegion::close consumes it). */
    asx_region_id closing;
    /* The task's context for cancel-aware sync waits (cancel check). */
    asx_cx cx;
    /* The current blocking sync step's registration (DSL §3.7). */
    union {
        asx_notify_waiter notify;
        asx_mutex_lock_waiter mutex;
        asx_rwlock_waiter rwlock;
        asx_semaphore_waiter sem;
        asx_barrier_waiter barrier;
        asx_watch_wait watch;
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
    /* Held rwlock guards, by lock name, oldest first. */
    struct {
        const char *name;
        int is_write;
        asx_rwlock_read_guard read;
        asx_rwlock_write_guard write;
    } rw_guards[IT_MAX_LOCAL];
    uint32_t n_rw_guards;
    /* Held mpsc send permits (DSL §3.6), by permit name. */
    struct {
        const char *name;
        uint32_t channel; /* index in g_channels */
        asx_send_permit permit;
    } send_permits[IT_MAX_LOCAL];
    uint32_t n_send_permits;
    /* The current task-group step (DSL §3.5) and its members' tasks. */
    asx_task_group group;
    uint32_t group_members[ASX_TASK_GROUP_MAX]; /* indices in g_tasks */
    uint32_t n_group_members;
    /* As a member of a group: its outcome, captured when it completed (the
     * group releases its slot), and the snapshot leaves it out (Rust's
     * combinators consume members' handles; vocabulary §6). */
    int group_member;
    int member_done;
    uint32_t member_outcome;
    /* A race with deadline_ns (exec_race_deadline). As its owner: the
     * wrapper (index in g_tasks), the deadline, and whether the deadline
     * timer is registered and has neither fired nor been cancelled. As the
     * wrapper: it runs the owner's race step only, names the members after
     * the owner, and keeps the race's value for the owner. */
    uint32_t race_wrapper_task;
    asx_time race_deadline;
    int race_timer_armed;
    int race_wrapper;
    uint32_t race_owner;
    uint32_t race_result; /* ASX_JSON_NONE until the race resolves */
    /* The current cast, call or server_stop step's progress (DSL §3.8). */
    asx_actor_op actor_op;
    int server; /* the task of a server (no program) */
    /* A supervisor's controller (no program), and a generation of a
     * supervised child, left out of the snapshot (vocabulary §6). */
    int supervisor;
    int supervised;
    /* A region finalizer's task (DSL §1, Region finalizers). */
    int finalizer;
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
    /* A supervisor's region, and the unstarted generation regions named
     * under it so far (name_unstarted_regions). */
    int supervisor_region;
    uint32_t unstarted;
} it_region;

/* A declared sync object (DSL §3.7). */
typedef enum {
    IT_SYNC_MUTEX,
    IT_SYNC_RWLOCK,
    IT_SYNC_SEMAPHORE,
    IT_SYNC_BARRIER,
    IT_SYNC_NOTIFY
} it_sync_type;

typedef struct {
    const char *name;
    it_sync_type type;
    asx_mutex_handle mutex;
    asx_rwlock_handle rwlock;
    asx_semaphore_handle semaphore;
    asx_barrier_handle barrier;
    asx_notify_handle notify;
} it_sync;

#define IT_MAX_SYNC 16u

/* A declared channel (DSL §3.6). Its endpoints are bound to their owner
 * tasks, as Rust moves them into the owners' bodies; an endpoint closes
 * when its owner closes it or its owner's body ends. Broadcast and watch
 * channels have one receiver per subscriber; the receivers carry their
 * own cursor / seen version. The type order is the order twin_run's Local
 * drops endpoint maps in. */
typedef enum { IT_CH_MPSC, IT_CH_ONESHOT, IT_CH_BROADCAST, IT_CH_WATCH, IT_CH_TYPES } it_ch_type;

#define IT_MAX_SUBSCRIBERS 8u
#define IT_MAX_SENDERS 8u

typedef struct {
    uint32_t task; /* owner, index in g_tasks */
    int open;
    /* NULL for a declared subscriber; the `as` name of a watch_clone, which
     * the owner's watch steps then select with `receiver`. */
    const char *alias;
    asx_oneshot_receiver oneshot;
    asx_broadcast_receiver broadcast;
    asx_watch_receiver watch;
} it_receiver;

typedef struct {
    const char *name;
    it_ch_type type;
    asx_channel_id id; /* mpsc */
    asx_oneshot_sender oneshot;
    asx_watch_sender watch;
    /* Its senders, each with its owner (index in g_tasks): the declared
     * `sender`, then for an mpsc or a broadcast the `senders` clones (Rust
     * Sender::clone at setup). A broadcast sender is a handle per owner. */
    struct {
        uint32_t task;
        int open;
        asx_broadcast_sender broadcast;
        asx_channel_sender mpsc; /* a clone's identity; slot 0 is the creator's */
    } senders[IT_MAX_SENDERS];
    uint32_t n_senders;
    it_receiver receivers[IT_MAX_SUBSCRIBERS];
    uint32_t n_receivers;
} it_channel;

#define IT_MAX_CHANNELS ASX_MAX_CHANNELS

/* A spawned server (DSL §3.8): its handle, its task's entry in g_tasks
 * (named by `as`), and the state of its built-in behavior. A server_stop
 * consumes the handle, as Rust's join takes the GenServerHandle. */
typedef struct {
    const char *name;
    asx_actor_handle handle;
    uint32_t task; /* index in g_tasks */
    int released;
    uint64_t value; /* counter: the running sum */
} it_server;

#define IT_MAX_SERVERS ASX_MAX_ACTORS
#define IT_NO_SERVER_TASK UINT32_MAX

/* A supervised child's start context (DSL §3.8): its program and name. */
typedef struct {
    const char *name;
    uint32_t program;
    uint32_t sup; /* index in g_sups */
} it_sup_child;

/* A spawned supervisor (DSL §3.8): its handle, its owner, its
 * controller's entry in g_tasks (named by `as`; IT_NO_SERVER_TASK when
 * its spawn was refused) and its children's start contexts. */
typedef struct {
    const char *name;
    asx_supervisor_handle handle;
    uint32_t owner; /* index in g_tasks */
    uint32_t task;
    int joined;
    int dropped;
    int region_named;
    it_sup_child children[ASX_SUPERVISOR_MAX_CHILDREN];
} it_supervisor;

#define IT_MAX_SUPERVISORS ASX_MAX_SUPERVISORS

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
    asx_trace_handoff handoff;
    it_reason reason; /* CANCEL_REQUEST */
    int has_reason;
    uint32_t text_off; /* USER */
    uint32_t text_len;
    int rearm;            /* TIMER_SET: the same sleep timer registered again */
    asx_region_id parent; /* REGION_OPEN: the new region's parent */
} it_event;

static const asx_json_doc *g_in;
/* A private copy of the input document, which g_in points at for the whole
 * run: a supervised child's program is rewritten into it for each
 * generation (DSL 3.8), so the caller's const document is never touched. */
static asx_json_doc g_work;
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

static it_server g_servers[IT_MAX_SERVERS];
static uint32_t g_n_servers;

static it_supervisor g_sups[IT_MAX_SUPERVISORS];
static uint32_t g_n_sups;

static it_event g_events[IT_MAX_EVENTS];
static uint32_t g_n_events;

/* The lab's dispatches (asx_scheduler_record_dispatches). */
static asx_dispatch_record g_dispatches[IT_MAX_DISPATCHES];

/* A recorded schedule to replay instead of the seeded picker, and the
 * scenario task name of each of its dispatches (asx_conformance_force_
 * schedule); g_forced_on while it is installed on the lab. */
static const asx_forced_schedule *g_forced = NULL;
static const char *const *g_forced_names = NULL;
static asx_forced_run_config g_forced_cfg;
static int g_forced_on = 0;
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
    g_regions[g_n_regions].supervisor_region = 0;
    g_regions[g_n_regions].unstarted = 0;
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
    t->spawn_obs = ASX_JSON_NONE;
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
    case ASX_OBLIGATION_ABORT_NONE: return NULL; /* not aborted */
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
    if (len >= (size_t)IT_TEXT_CAP || (size_t)g_text_used + len + 1u > (size_t)IT_TEXT_CAP) {
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
    /* The links the runtime's chain bounds leave: a truncated chain ends
     * where Rust's does (with_cause_limited, types/cancel.rs:706-761),
     * though the stored causes, the ancestors' own reasons that longer
     * chains share, continue past it. */
    out->cause_chain_len = asx_cancel_reason_chain_links(r);
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

/* A server's outcome is its join's (vocabulary §6): an admitted server
 * always runs to its end (ok), unless a callback panicked; its task's own
 * outcome (cancelled when cancelled before its first poll) is not
 * observable in Rust. */
static uint32_t server_outcome_node(asx_task_id id) {
    asx_outcome outcome;
    uint32_t o;
    if (asx_task_get_outcome(id, &outcome) == ASX_OK && outcome.severity == ASX_OUTCOME_PANICKED) {
        return outcome_node(id);
    }
    o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "ok"));
    return o;
}

static asx_status interp_poll(void *user_data, asx_task_id self);

/* ------------------------------------------------------------------ */
/* Supervisor names (vocabulary §2)                                    */
/* ------------------------------------------------------------------ */

static it_supervisor *sup_by_name(const char *name) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_sups; i++) {
        if (strcmp(g_sups[i].name, name) == 0) return &g_sups[i];
    }
    return NULL;
}

/* The supervisor region, named after its supervisor once the controller
 * opened it; its parent is the controller's (its owner's) region. */
static int supervisor_region(it_supervisor *sv) {
    asx_region_id id;
    if (sv->region_named) return 1;
    id = asx_supervisor_region(sv->handle);
    if (id == ASX_INVALID_ID) return 1;
    if (!add_region(sv->name, g_tasks[sv->owner].region_name, id)) return 0;
    g_regions[g_n_regions - 1u].supervisor_region = 1;
    sv->region_named = 1;
    return 1;
}

/* A generation's task and region, both "<supervisor>/<child>#<n>",
 * entered on first sight: when its start function runs, or from the
 * controller's trace for a generation that never started. */
static it_task *generation_entry(it_supervisor *sv, const char *child, unsigned long long number,
                                 asx_region_id region, asx_task_id task) {
    char buf[160];
    const char *name;
    uint32_t off;
    it_task *t;
    int len = snprintf(buf, sizeof(buf), "%s/%s#%llu", sv->name, child, number);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        it_fail("supervised child name too long", child);
        return NULL;
    }
    t = task_by_name(buf);
    if (t != NULL) return t;
    if (!supervisor_region(sv)) return NULL;
    off = text_store(buf, (size_t)len);
    if (off == IT_NO_TEXT) return NULL;
    name = &g_text[off];
    if (region_by_id(region) == NULL && !add_region(name, sv->name, region)) return NULL;
    if (g_n_tasks >= IT_MAX_TASKS) {
        it_fail("too many tasks", name);
        return NULL;
    }
    t = &g_tasks[g_n_tasks++];
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->region_name = name;
    t->region = region;
    t->program = ASX_JSON_NONE;
    t->end = ASX_OK;
    t->spawn_obs = ASX_JSON_NONE;
    t->id = task;
    t->spawned = 1;
    t->supervised = 1;
    (void)asx_cx_init(&t->cx, region, task, ASX_CAP_NONE);
    return t;
}

/* ------------------------------------------------------------------ */
/* Per-generation name scoping (DSL 3.8)                               */
/* ------------------------------------------------------------------ */

/* Every generation of a supervised child runs its program again, so the
 * names the program creates would repeat. Each generation instead gets a
 * copy of the program in which every name its steps create (`as` of
 * spawn, open_region, server_spawn and supervise) is "<generation>/<as>",
 * as are the references to those names (`task`, `region`, `server`,
 * `supervisor`). A name the program does not create (a scenario-level
 * region, task or resource) is left alone. twin_run's scope_program does
 * the same to the same JSON. */
#define IT_SCOPE_MAX_NAMES 32u

typedef struct {
    const char *scope;
    const char *names[IT_SCOPE_MAX_NAMES];
    uint32_t n;
} it_scope;

static int scope_creates(const char *op) {
    return op != NULL && (strcmp(op, "spawn") == 0 || strcmp(op, "open_region") == 0 ||
                          strcmp(op, "server_spawn") == 0 || strcmp(op, "supervise") == 0);
}

static int scope_refers(const char *key) {
    return strcmp(key, "task") == 0 || strcmp(key, "region") == 0 || strcmp(key, "server") == 0 ||
           strcmp(key, "supervisor") == 0;
}

static int scope_is_local(const it_scope *sc, const char *name) {
    uint32_t i;
    for (i = 0; i < sc->n; i++) {
        if (strcmp(sc->names[i], name) == 0) return 1;
    }
    return 0;
}

/* The names the steps of `program` create, into `sc`: those of member
 * programs, masked steps and spawned children's programs included, but not
 * those of a nested supervisor's children, which run generations of their
 * own and are scoped by them. 0 when there are too many. */
/* ASX_ANALYZER_WAIVER("bounded recursion: step nesting is capped by ASX_JSON_MAX_DEPTH") */
static int scope_collect(uint32_t program, it_scope *sc) {
    uint32_t count = asx_json_count(g_in, program);
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t step = asx_json_item(g_in, program, i);
        const char *op = it_str(step, "op");
        const char *as = it_str(step, "as");
        uint32_t members = asx_json_get(g_in, step, "members");
        uint32_t steps = asx_json_get(g_in, step, "steps");
        uint32_t child = asx_json_get(g_in, step, "program");
        uint32_t k;
        if (scope_creates(op) && as != NULL) {
            if (sc->n >= IT_SCOPE_MAX_NAMES) return 0;
            sc->names[sc->n++] = as;
        }
        if (members != ASX_JSON_NONE) {
            for (k = 0; k < asx_json_count(g_in, members); k++) {
                if (!scope_collect(asx_json_item(g_in, members, k), sc)) return 0;
            }
        }
        if (steps != ASX_JSON_NONE && !scope_collect(steps, sc)) return 0;
        if (op != NULL && strcmp(op, "spawn") == 0 && child != ASX_JSON_NONE &&
            !scope_collect(child, sc)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t scope_steps(uint32_t program, const it_scope *sc);

static uint32_t scope_name(const it_scope *sc, const char *name) {
    char buf[200];
    int len = snprintf(buf, sizeof(buf), "%s/%s", sc->scope, name);
    if (len < 0 || (size_t)len >= sizeof(buf)) return ASX_JSON_NONE;
    return asx_json_new_string_len(&g_work, buf, (size_t)len);
}

/* A copy of one step with its names scoped. */
/* ASX_ANALYZER_WAIVER("bounded recursion: step nesting is capped by ASX_JSON_MAX_DEPTH") */
static uint32_t scope_step(uint32_t step, const it_scope *sc) {
    uint32_t out = asx_json_new_object(&g_work);
    const char *op = it_str(step, "op");
    uint32_t count = asx_json_count(g_in, step);
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t value = asx_json_item(g_in, step, i);
        const char *key = asx_json_key(g_in, value);
        const char *s = asx_json_string(g_in, value);
        uint32_t copy;
        if (key == NULL) continue;
        if (s != NULL && strcmp(key, "as") == 0 && scope_creates(op)) {
            copy = scope_name(sc, s);
        } else if (s != NULL && scope_refers(key) && scope_is_local(sc, s)) {
            copy = scope_name(sc, s);
        } else if (strcmp(key, "members") == 0) {
            uint32_t k;
            copy = asx_json_new_array(&g_work);
            for (k = 0; k < asx_json_count(g_in, value); k++) {
                asx_json_push(&g_work, copy, scope_steps(asx_json_item(g_in, value, k), sc));
            }
        } else if (strcmp(key, "steps") == 0 ||
                   (strcmp(key, "program") == 0 && op != NULL && strcmp(op, "spawn") == 0)) {
            copy = scope_steps(value, sc);
        } else {
            copy = asx_json_copy(&g_work, g_in, value);
        }
        asx_json_set(&g_work, out, key, copy);
    }
    return out;
}

/* A copy of an array of steps with its names scoped. */
/* ASX_ANALYZER_WAIVER("bounded recursion: step nesting is capped by ASX_JSON_MAX_DEPTH") */
static uint32_t scope_steps(uint32_t program, const it_scope *sc) {
    uint32_t out = asx_json_new_array(&g_work);
    uint32_t count = asx_json_count(g_in, program);
    uint32_t i;
    for (i = 0; i < count; i++) {
        asx_json_push(&g_work, out, scope_step(asx_json_item(g_in, program, i), sc));
    }
    return out;
}

/* A supervised child's start function: its generation runs the child's
 * program, with the names it creates scoped under the generation's. */
/* ASX_ANALYZER_WAIVER("callback signature: const user_data would need a function-pointer cast") */
static asx_status it_start_generation(void *user_data, const asx_supervisor_generation *gen,
                                      asx_task_poll_fn *out_poll, void **out_data) {
    const it_sup_child *c = (const it_sup_child *)user_data;
    it_task *t = generation_entry(&g_sups[c->sup], c->name, (unsigned long long)gen->number,
                                  gen->region, gen->task);
    it_scope sc;
    if (t == NULL) return ASX_E_RESOURCE_EXHAUSTED;
    t->program = c->program;
    sc.scope = t->name;
    sc.n = 0u;
    if (!scope_collect(c->program, &sc)) {
        it_fail("a supervised child's program creates too many names", t->name);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    if (sc.n > 0u) {
        t->program = scope_steps(c->program, &sc);
        if (t->program == ASX_JSON_NONE || !asx_json_doc_ok(&g_work)) {
            it_fail("a generation's scoped program does not fit the document pool", t->name);
            return ASX_E_RESOURCE_EXHAUSTED;
        }
    }
    *out_poll = interp_poll;
    *out_data = t;
    return ASX_OK;
}

/* A controller's "managed_supervisor_v1 ..." trace (supervisor.h), split. */
typedef struct {
    char action[40];
    char supervisor[64];
    char child[64];
    unsigned long long generation;
    unsigned long long region;
    unsigned long long task;
    char outcome[16];
} it_sup_trace;

static int parse_sup_trace(const char *text, it_sup_trace *out) {
    if (strncmp(text, "managed_supervisor_v1 ", 22u) != 0) return 0;
    /* ASX_ANALYZER_WAIVER("trusted input: the supervisor's own trace line, bounded fields") */
    return sscanf(text,
                  "managed_supervisor_v1 action=%39s supervisor=\"%63[^\"]\" child=\"%63[^\"]\" "
                  "generation=%llu region=0x%llx task=0x%llx outcome=%15s",
                  out->action, out->supervisor, out->child, &out->generation, &out->region,
                  &out->task, out->outcome) == 7;
}

/* Add a region named `name` (copied) under `parent`, a supervisor region. */
static int add_supervisor_region(const char *name, const char *parent, asx_region_id id) {
    uint32_t off = text_store(name, strlen(name));
    if (off == IT_NO_TEXT || !add_region(&g_text[off], parent, id)) return 0;
    g_regions[g_n_regions - 1u].supervisor_region = 1;
    return 1;
}

/* The region of a supervisor that started no generation, as twin_run
 * names it: the supervisor's own name when it is the one unnamed region
 * created under its controller's region and no other such supervisor
 * runs there; else every unnamed region created there is
 * "<region>/supervisor<k>", k in creation order (the trace cannot tell
 * whose is whose). Its controller learns it only once the open is
 * applied, so its handle cannot settle every case. */
static void name_generationless_regions(void) {
    int unnamed[IT_MAX_SUPERVISORS];
    uint32_t k;
    for (k = 0; k < g_n_sups; k++) unnamed[k] = !g_sups[k].region_named;
    for (k = 0; k < g_n_sups && !g_failed; k++) {
        const it_task *owner = &g_tasks[g_sups[k].owner];
        uint32_t i;
        uint32_t siblings = 0;
        uint32_t found = 0;
        asx_region_id region = ASX_INVALID_ID;
        if (!unnamed[k]) continue;
        for (i = 0; i < g_n_sups; i++) {
            if (unnamed[i] && same_entity(g_tasks[g_sups[i].owner].region, owner->region)) {
                siblings++;
            }
        }
        for (i = 0; i < g_n_events; i++) {
            const it_event *e = &g_events[i];
            if (e->kind == ASX_TRACE_REGION_OPEN && e->parent != ASX_INVALID_ID &&
                same_entity(e->parent, owner->region) &&
                region_by_id((asx_region_id)e->entity) == NULL) {
                found++;
                region = (asx_region_id)e->entity;
            }
        }
        if (found == 1u && siblings == 1u) {
            if (add_supervisor_region(g_sups[k].name, owner->region_name, region)) {
                g_sups[k].region_named = 1;
            }
            continue;
        }
        found = 0;
        for (i = 0; i < g_n_events && !g_failed; i++) {
            const it_event *e = &g_events[i];
            char buf[160];
            if (e->kind != ASX_TRACE_REGION_OPEN || e->parent == ASX_INVALID_ID ||
                !same_entity(e->parent, owner->region) ||
                region_by_id((asx_region_id)e->entity) != NULL) {
                continue;
            }
            found++;
            (void)snprintf(buf, sizeof(buf), "%s/supervisor%u", owner->region_name,
                           (unsigned)found);
            (void)add_supervisor_region(buf, owner->region_name, (asx_region_id)e->entity);
        }
    }
}

/* A region a controller opened for a generation it then did not start (it
 * was cancelled first) appears in no trace of it:
 * "<supervisor region>/unstarted<k>", k in creation order. */
static void name_unstarted_regions(void) {
    uint32_t i;
    for (i = 0; i < g_n_events && !g_failed; i++) {
        const it_event *e = &g_events[i];
        it_region *parent;
        char buf[160];
        if (e->kind != ASX_TRACE_REGION_OPEN || e->parent == ASX_INVALID_ID ||
            region_by_id((asx_region_id)e->entity) != NULL) {
            continue;
        }
        parent = region_by_id(e->parent);
        if (parent == NULL || !parent->supervisor_region) continue;
        parent->unstarted++;
        (void)snprintf(buf, sizeof(buf), "%s/unstarted%u", parent->name,
                       (unsigned)parent->unstarted);
        {
            uint32_t off = text_store(buf, strlen(buf));
            if (off != IT_NO_TEXT)
                (void)add_region(&g_text[off], parent->name, (asx_region_id)e->entity);
        }
    }
}

/* Name the generation a controller's trace identifies. */
static void name_generation(const char *text) {
    it_sup_trace p;
    it_supervisor *sv;
    if (strncmp(text, "managed_supervisor_v1 ", 22u) != 0) return;
    if (!parse_sup_trace(text, &p)) {
        it_fail("malformed supervisor trace", text);
        return;
    }
    sv = sup_by_name(p.supervisor);
    if (sv == NULL) {
        it_fail("trace of an unknown supervisor", p.supervisor);
        return;
    }
    (void)generation_entry(sv, p.child, p.generation, (asx_region_id)p.region, (asx_task_id)p.task);
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
    case ASX_TRACE_OBLIGATION_HANDOFF:
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
        ev->kind == ASX_TRACE_OBLIGATION_ABORT || ev->kind == ASX_TRACE_OBLIGATION_LEAK ||
        ev->kind == ASX_TRACE_OBLIGATION_HANDOFF) {
        if (payload->obligation != NULL) {
            e->obligation = *payload->obligation;
        } else if (asx_obligation_get_info((asx_obligation_id)ev->entity_id, &e->obligation) !=
                   ASX_OK) {
            it_fail("obligation event for an unreadable obligation", NULL);
        }
        if (ev->kind == ASX_TRACE_OBLIGATION_HANDOFF) {
            if (payload->handoff == NULL) {
                it_fail("handoff event without its ownership metadata", NULL);
                return;
            }
            e->handoff = *payload->handoff;
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
    } else if (ev->kind == ASX_TRACE_TIMER_SET) {
        e->rearm = payload != NULL && payload->text != NULL && strcmp(payload->text, "rearm") == 0;
    } else if (ev->kind == ASX_TRACE_USER) {
        const char *text = payload->text != NULL ? payload->text : "";
        size_t len = strlen(text);
        e->text_off = text_store(text, len);
        e->text_len = (uint32_t)len;
        name_generation(text);
    } else if (ev->kind == ASX_TRACE_REGION_OPEN) {
        if (asx_region_get_parent((asx_region_id)ev->entity_id, &e->parent) != ASX_OK) {
            e->parent = ASX_INVALID_ID;
        }
    } else if (ev->kind == ASX_TRACE_SCHED_COMPLETE) {
        /* A group member's outcome, read while its slot still exists: the
         * group joins it (releasing the slot) when it collects it. */
        it_task *m = task_by_id((asx_task_id)ev->entity_id);
        if (m != NULL && m->group_member && !m->member_done) {
            m->member_outcome = outcome_node(m->id);
            m->member_done = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Observations                                                        */
/* ------------------------------------------------------------------ */

/* Returns the observation's node. */
static uint32_t observe(const it_task *t, uint32_t step, const char *op, uint32_t status,
                        uint32_t value) {
    uint32_t o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "task", asx_json_new_string(g_out, t->name));
    asx_json_set(g_out, o, "step", asx_json_new_u64(g_out, step));
    asx_json_set(g_out, o, "op", asx_json_new_string(g_out, op));
    asx_json_set(g_out, o, "status", status);
    asx_json_set(g_out, o, "value", value == ASX_JSON_NONE ? asx_json_new_null(g_out) : value);
    asx_json_push(g_out, g_observations, o);
    return o;
}

static uint32_t observe_status(const it_task *t, uint32_t step, const char *op, asx_status st) {
    return observe(t, step, op, status_node(st), ASX_JSON_NONE);
}

/* A refused child's status as its join reports it, which is what twin_run's
 * project_denied_spawns reads from Rust's refused handles: the admission
 * limit's User reason ("[ASUP-E006] ...") is ASX_E_ADMISSION_LIMIT, any
 * other ASX_E_REGION_CLOSED. An abort requested before the admission has
 * already strengthened that reason (Rust caches it in the handle, which
 * strengthens the refusal it resolves with, task_handle.rs:470-542). */
static asx_status refusal_as_joined(asx_task_id id) {
    asx_cancel_reason r;
    if (asx_task_get_cancel_reason(id, &r) == ASX_OK && r.kind == ASX_CANCEL_USER &&
        r.message != NULL && strncmp(r.message, "[ASUP-E006]", 11u) == 0) {
        return ASX_E_ADMISSION_LIMIT;
    }
    return ASX_E_REGION_CLOSED;
}

/* Whether the admission of a child spawned from a poll refused it (lab
 * dispatch), and then its status as its join reports it; ASX_OK if it was
 * admitted or is still pending. */
static asx_status admission_refusal(asx_task_id id) {
    asx_status st = asx_task_admission_status(id);
    if (st != ASX_E_ADMISSION_LIMIT && st != ASX_E_REGION_CLOSED) return ASX_OK;
    return refusal_as_joined(id);
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

static it_channel *channel_by_name(const char *name) {
    uint32_t i;
    for (i = 0; name != NULL && i < g_n_channels; i++) {
        if (strcmp(g_channels[i].name, name) == 0) return &g_channels[i];
    }
    return NULL;
}

/* The index in c->senders of task `me`'s still-open sender, or -1. */
static int sender_slot(const it_channel *c, uint32_t me) {
    uint32_t i;
    for (i = 0; i < c->n_senders; i++) {
        if (c->senders[i].task == me && c->senders[i].open) return (int)i;
    }
    return -1;
}

/* The channel `name` of `type` if task `t` owns a still-open sender of it,
 * else NULL: an endpoint is usable only in its owner's body (DSL §3.6). */
static it_channel *owned_sender(const it_task *t, const char *name, it_ch_type type) {
    it_channel *c = channel_by_name(name);
    if (c == NULL || c->type != type) return NULL;
    return sender_slot(c, (uint32_t)(t - g_tasks)) >= 0 ? c : NULL;
}

/* Task `t`'s still-open receiver on channel `name` of `type`: its declared
 * one (`alias` NULL), or the watch clone it named `alias`. Else NULL. */
static it_receiver *owned_receiver_as(const it_task *t, const char *name, it_ch_type type,
                                      const char *alias) {
    it_channel *c = channel_by_name(name);
    uint32_t i;
    if (c == NULL || c->type != type) return NULL;
    for (i = 0; i < c->n_receivers; i++) {
        const it_receiver *r = &c->receivers[i];
        if (r->task != (uint32_t)(t - g_tasks) || !r->open) continue;
        if (alias == NULL ? r->alias == NULL : r->alias != NULL && strcmp(r->alias, alias) == 0) {
            return &c->receivers[i];
        }
    }
    return NULL;
}

/* Task `t`'s still-open declared receiver on channel `name` of `type`. */
static it_receiver *owned_receiver(const it_task *t, const char *name, it_ch_type type) {
    return owned_receiver_as(t, name, type, NULL);
}

/* Rust's SendPermit drop without send (mpsc.rs:1663): the slot is
 * released and the obligation aborted with reason Cancel, where an
 * explicit abort says Explicit; C has no destructor, so the interpreter
 * uses the channel's authenticated permit destruction boundary. */
static void drop_send_permit(asx_send_permit *permit) { asx_send_permit_drop_internal(permit); }

/* Drop task `owner`'s sender of `c` (`r` NULL) or receiver `r`, as Rust
 * drops the endpoint: an mpsc sender lowers the sender count, which
 * closes the sender side at the last one. */
static void close_endpoint(it_channel *c, it_receiver *r, uint32_t owner) {
    asx_status st = ASX_OK;
    if (r == NULL) {
        int k = sender_slot(c, owner);
        if (k >= 0) c->senders[k].open = 0;
        switch (c->type) {
        case IT_CH_MPSC:
            /* Slot 0 is the declared sender, the creator's; the others are
             * clones, each dropped by its own identity. */
            st = k > 0 ? asx_channel_sender_drop(&c->senders[k].mpsc)
                       : asx_channel_close_sender(c->id);
            break;
        case IT_CH_ONESHOT: asx_oneshot_sender_drop(&c->oneshot); break;
        case IT_CH_BROADCAST:
            if (k >= 0) asx_broadcast_sender_drop(&c->senders[k].broadcast);
            break;
        case IT_CH_WATCH: asx_watch_sender_drop(&c->watch); break;
        case IT_CH_TYPES: break;
        }
    } else {
        r->open = 0;
        switch (c->type) {
        case IT_CH_MPSC: st = asx_channel_close_receiver(c->id); break;
        case IT_CH_ONESHOT: asx_oneshot_receiver_drop(&r->oneshot); break;
        case IT_CH_BROADCAST: asx_broadcast_receiver_drop(&r->broadcast); break;
        case IT_CH_WATCH: asx_watch_receiver_drop(&r->watch); break;
        case IT_CH_TYPES: break;
        }
    }
    (void)st; /* closing an already half-closed mpsc side cannot fail */
}

/* Release held guard / permit i, keeping the others in acquisition order
 * (a name's most recent permit is the one sem_release pops, as Rust's
 * Vec::pop). */
static asx_status release_guard(it_task *t, uint32_t i) {
    /* A guard dropped while the task panics poisons the mutex (Rust's
     * MutexGuard drop, sync/mutex.rs:758-763). */
    asx_status st = t->panicking ? asx_mutex_unlock_poisoned(t->guards[i].guard)
                                 : asx_mutex_unlock(t->guards[i].guard);
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

static asx_status release_rw_guard(it_task *t, uint32_t i) {
    /* Only a write guard dropped while the task panics poisons the lock
     * (rwlock.rs:1083-1090); read guards never do. */
    asx_status st;
    if (!t->rw_guards[i].is_write) {
        st = asx_rwlock_read_unlock(t->rw_guards[i].read);
    } else if (t->panicking) {
        st = asx_rwlock_write_unlock_poisoned(t->rw_guards[i].write);
    } else {
        st = asx_rwlock_write_unlock(t->rw_guards[i].write);
    }
    for (; i + 1u < t->n_rw_guards; i++) t->rw_guards[i] = t->rw_guards[i + 1u];
    t->n_rw_guards--;
    return st;
}

/* The endpoint task `me` still owns that drops first: of channel type
 * `type`, its sender (`sender` 1) or a receiver (0), the least channel
 * name first. Returns the channel (and *out_r, NULL for a sender), or
 * NULL when none is left. */
static it_channel *next_owned(uint32_t me, it_ch_type type, int sender, it_receiver **out_r) {
    it_channel *best = NULL;
    uint32_t i;
    uint32_t k;
    *out_r = NULL;
    for (i = 0; i < g_n_channels; i++) {
        it_channel *c = &g_channels[i];
        it_receiver *mine = NULL;
        if (c->type != type) continue;
        if (best != NULL && strcmp(c->name, best->name) >= 0) continue;
        if (sender) {
            if (sender_slot(c, me) < 0) continue;
        } else {
            for (k = 0; k < c->n_receivers && mine == NULL; k++) {
                if (c->receivers[k].task == me && c->receivers[k].open) mine = &c->receivers[k];
            }
            if (mine == NULL) continue;
        }
        best = c;
        *out_r = mine;
    }
    return best;
}

/* The task body ended (returned, acknowledged a cancel, or panicked): what
 * it still holds is released the way Rust drops the body's locals
 * (twin_run's Local, fields in declaration order): guards, then semaphore
 * permits, then rwlock guards, each by object name, a name's permits and
 * rwlock guards oldest first; then send permits by permit name; then, per
 * channel type (mpsc, oneshot,
 * broadcast, watch), owned senders, then receivers, each by channel name.
 * A dropped Rust semaphore permit commits its obligation
 * (sync/semaphore.rs:1081); C has no destructors, so the interpreter
 * releases explicitly. */
static void drop_locals(it_task *t) {
    uint32_t me = (uint32_t)(t - g_tasks);
    int type;
    int sender;
    /* Unresolved obligation tokens drop first (twin_run's Local declares
     * them first): each posts a Leak (obligation_mailbox.rs:897), and a
     * checked token releases its region count at once (bd-2fga). Leaks of
     * different obligations commute, so their order is not observable. */
    while (t->n_obligations > 0u) {
        asx_status st = asx_obligation_drop(t->obligations[--t->n_obligations].id);
        (void)st; /* an obligation resolved some other way stays as it is */
    }
    /* Child regions still open are closed, by name: Rust's ChildRegion drop
     * backstop requests a Close without waiting, and its Local drops
     * `regions` before the guards. */
    while (t->n_regions > 0u) {
        uint32_t i;
        uint32_t first = 0;
        for (i = 1; i < t->n_regions; i++) {
            if (strcmp(t->regions[i].name, t->regions[first].name) < 0) first = i;
        }
        if (asx_region_close_request(t->regions[first].id) != ASX_OK) {
            it_fail("the close of a dropped child region failed", t->regions[first].name);
        }
        t->regions[first] = t->regions[--t->n_regions];
    }
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
    while (t->n_rw_guards > 0u) {
        uint32_t i;
        uint32_t first = 0;
        for (i = 1; i < t->n_rw_guards; i++) {
            if (strcmp(t->rw_guards[i].name, t->rw_guards[first].name) < 0) first = i;
        }
        (void)release_rw_guard(t, first);
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
    for (type = 0; type < (int)IT_CH_TYPES; type++) {
        for (sender = 1; sender >= 0; sender--) {
            for (;;) {
                it_receiver *r;
                it_channel *c = next_owned(me, (it_ch_type)type, sender, &r);
                if (c == NULL) break;
                close_endpoint(c, r, me);
            }
        }
    }
    /* Supervisor handles last, joined or not, by name: each one's drop
     * aborts its controller (ManagedSupervisorHandle's Drop). */
    for (;;) {
        it_supervisor *first = NULL;
        uint32_t i;
        for (i = 0; i < g_n_sups; i++) {
            it_supervisor *sv = &g_sups[i];
            if (sv->owner != me || sv->dropped) continue;
            if (first == NULL || strcmp(sv->name, first->name) < 0) first = sv;
        }
        if (first == NULL) break;
        first->dropped = 1;
        if (asx_supervisor_abort(first->handle) != ASX_OK) {
            it_fail("the abort of a dropped supervisor failed", first->name);
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
static asx_status cancel_rwlock_wait(void *w) {
    return asx_rwlock_waiter_cancel((asx_rwlock_waiter *)w);
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

/* A receive step's poll result: pending keeps the step waiting; anything
 * else is observed, with the received value (or a lag count) when `value`
 * is set. */
static void channel_received(it_task *t, uint32_t idx, const char *op, asx_status st, int value,
                             uint64_t v, step_result *out) {
    if (st == ASX_E_PENDING) {
        *out = STEP_PENDING;
        return;
    }
    observe(t, idx, op, status_node(st), value ? asx_json_new_u64(g_out, v) : ASX_JSON_NONE);
}

/* A watch_wait_for predicate (DSL §3.6): `value <cmp> than`. */
typedef struct {
    const char *cmp;
    uint64_t than;
} it_watch_pred;

static int watch_cmp_known(const char *cmp) {
    static const char *const names[] = {"eq", "ne", "lt", "le", "gt", "ge"};
    uint32_t i;
    for (i = 0; cmp != NULL && i < 6u; i++) {
        if (strcmp(cmp, names[i]) == 0) return 1;
    }
    return 0;
}

/* ASX_ANALYZER_WAIVER("callback signature: a const ctx would need a function-pointer cast") */
static int watch_pred_fn(void *ctx, uint64_t value) {
    const it_watch_pred *p = (const it_watch_pred *)ctx;
    if (strcmp(p->cmp, "eq") == 0) return value == p->than;
    if (strcmp(p->cmp, "ne") == 0) return value != p->than;
    if (strcmp(p->cmp, "lt") == 0) return value < p->than;
    if (strcmp(p->cmp, "le") == 0) return value <= p->than;
    if (strcmp(p->cmp, "gt") == 0) return value > p->than;
    return strcmp(p->cmp, "ge") == 0 && value >= p->than;
}

/* Blocking channel steps (DSL §3.6). Returns 1 when `op` is one of them.
 * A call is one poll of the Rust future (the library orders the checks as
 * Rust does) and ASX_E_PENDING keeps the step waiting, the task parked on
 * the channel. */
static int exec_channel_wait(it_task *t, uint32_t step, uint32_t idx, const char *op,
                             step_result *out) {
    asx_status st;
    uint64_t v = 0;
    const char *ch = it_str(step, "channel");
    *out = STEP_NEXT;
    if (strcmp(op, "oneshot_recv") == 0 || strcmp(op, "broadcast_recv") == 0 ||
        strcmp(op, "watch_changed") == 0) {
        it_ch_type type = op[0] == 'o'   ? IT_CH_ONESHOT
                          : op[0] == 'b' ? IT_CH_BROADCAST
                                         : IT_CH_WATCH;
        it_receiver *r = owned_receiver_as(t, ch, type, it_str(step, "receiver"));
        if (r == NULL) {
            it_fail_task(t, idx, "receive on a channel this task holds no receiver of");
            *out = STEP_END;
            return 1;
        }
        if (type == IT_CH_ONESHOT) {
            st = asx_oneshot_recv(&r->oneshot, &t->cx, &v);
            channel_received(t, idx, op, st, st == ASX_OK, v, out);
        } else if (type == IT_CH_BROADCAST) {
            st = asx_broadcast_recv(&r->broadcast, &t->cx, &v);
            channel_received(t, idx, op, st, st == ASX_OK || st == ASX_E_LAGGED, v, out);
        } else {
            /* Rust's changed() traces when the wait is created (watch.rs:723);
             * the value is read with borrow_and_update once it resolves. */
            if (t->phase == 0u) {
                asx_watch_changed_begin(&r->watch, &t->cx);
                t->phase = 1u;
            }
            st = asx_watch_poll_changed(&r->watch, &t->cx);
            if (st == ASX_OK && asx_watch_recv(&r->watch, &v) != ASX_OK) {
                it_fail_task(t, idx, "asx_watch_recv failed after a change");
                *out = STEP_END;
                return 1;
            }
            channel_received(t, idx, op, st, st == ASX_OK, v, out);
        }
        return 1;
    }
    if (strcmp(op, "watch_wait_for") == 0) {
        /* rx.wait_for(&cx, |v| v <cmp> than).await (watch.rs:858): the
         * predicate first, then changed() on a clone until one passes; the
         * value is the accepted one, borrowed and marked seen. */
        it_receiver *r = owned_receiver_as(t, ch, IT_CH_WATCH, it_str(step, "receiver"));
        it_watch_pred pred;
        pred.cmp = it_str(step, "cmp");
        pred.than = 0u;
        if (r == NULL || !watch_cmp_known(pred.cmp) ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &pred.than)) {
            it_fail_task(t, idx, "watch_wait_for needs a receiver, a cmp and a value");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            memset(&t->wait.watch, 0, sizeof(t->wait.watch));
            t->phase = 1u;
        }
        st = asx_watch_poll_wait_for(&r->watch, &t->cx, &t->wait.watch, watch_pred_fn, &pred, &v);
        channel_received(t, idx, op, st, st == ASX_OK, v, out);
        return 1;
    }
    if (strcmp(op, "oneshot_poll_closed") == 0) {
        /* poll_fn(|c| tx.poll_closed(c)) / rx.poll_closed (oneshot.rs:619,
         * :1373): no Cx, so a cancel does not end the wait. */
        const char *end = it_str(step, "end");
        int sender = end != NULL && strcmp(end, "sender") == 0;
        it_channel *c = sender ? owned_sender(t, ch, IT_CH_ONESHOT) : NULL;
        it_receiver *r = !sender && end != NULL && strcmp(end, "receiver") == 0
                             ? owned_receiver(t, ch, IT_CH_ONESHOT)
                             : NULL;
        if (c == NULL && r == NULL) {
            it_fail_task(t, idx, "oneshot_poll_closed on an end this task does not hold");
            *out = STEP_END;
            return 1;
        }
        st = c != NULL ? asx_oneshot_poll_closed(&c->oneshot)
                       : asx_oneshot_receiver_poll_closed(&r->oneshot);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        observe_status(t, idx, op, st);
        return 1;
    }
    if (strcmp(op, "reserve_send") == 0) {
        it_channel *c = owned_sender(t, ch, IT_CH_MPSC);
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
        it_channel *c = owned_sender(t, ch, IT_CH_MPSC);
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
        it_channel *c = channel_by_name(ch);
        if (c == NULL || owned_receiver(t, ch, IT_CH_MPSC) == NULL) {
            it_fail_task(t, idx, "recv needs an owned receiver");
            *out = STEP_END;
            return 1;
        }
        st = asx_channel_recv(c->id, &t->cx, &v);
        channel_received(t, idx, op, st, st == ASX_OK, v, out);
        return 1;
    }
    if (strcmp(op, "recv_many") == 0) {
        it_channel *c = channel_by_name(ch);
        uint64_t batch[ASX_CHANNEL_MAX_CAPACITY];
        uint64_t limit = 0;
        uint32_t n = 0;
        uint32_t list;
        uint32_t i;
        if (c == NULL || owned_receiver(t, ch, IT_CH_MPSC) == NULL ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "limit"), &limit)) {
            it_fail_task(t, idx, "recv_many needs an owned receiver and a limit");
            *out = STEP_END;
            return 1;
        }
        /* rx.recv_many(&cx, &mut buf, limit) (mpsc.rs:1754): a batch is at
         * most what is queued, which the channel capacity bounds. */
        st = asx_channel_recv_many(c->id, &t->cx, batch,
                                   limit < ASX_CHANNEL_MAX_CAPACITY ? (uint32_t)limit
                                                                    : ASX_CHANNEL_MAX_CAPACITY,
                                   &n);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        if (st != ASX_OK) {
            observe_status(t, idx, op, st);
            return 1;
        }
        list = asx_json_new_array(g_out);
        for (i = 0; i < n; i++) asx_json_push(g_out, list, asx_json_new_u64(g_out, batch[i]));
        observe(t, idx, op, status_node(st), list);
        return 1;
    }
    if (strcmp(op, "closed") == 0) {
        it_channel *c = owned_sender(t, ch, IT_CH_MPSC);
        if (c == NULL || asx_channel_is_unbounded(c->id)) {
            it_fail_task(t, idx, "closed needs an owned sender of a bounded channel");
            *out = STEP_END;
            return 1;
        }
        /* tx.closed().await (mpsc_closed.rs:39): no Cx, so a cancel does
         * not end the wait. */
        st = asx_channel_closed(c->id);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        observe_status(t, idx, op, st);
        return 1;
    }
    return 0;
}

/* Spawn the members of the group step `step` (1-based `idx` in `t`'s
 * program) into t->group: interpreter tasks named "<owner>/g<idx>.<i>"
 * (vocabulary §2), run in the owner's region (a deadline wrapper's owner
 * is the task whose step it runs). Returns 0 on failure. */
static int spawn_group_members(it_task *t, uint32_t step, uint32_t idx) {
    uint32_t members = asx_json_get(g_in, step, "members");
    uint32_t n = asx_json_count(g_in, members);
    /* A deadline wrapper's members keep their plain-race names. */
    const char *base = t->race_wrapper ? g_tasks[t->race_owner].name : t->name;
    uint32_t i;
    if (n == 0u || n > ASX_TASK_GROUP_MAX) {
        it_fail_task(t, idx, "group needs 1..ASX_TASK_GROUP_MAX members");
        return 0;
    }
    t->n_group_members = 0;
    for (i = 0; i < n; i++) {
        char buf[160];
        uint32_t off;
        it_task *m;
        asx_task_id id = ASX_INVALID_ID;
        int len = snprintf(buf, sizeof(buf), "%s/g%u.%u", base, (unsigned)idx, (unsigned)(i + 1u));
        if (len < 0 || (size_t)len >= sizeof(buf)) {
            it_fail_task(t, idx, "group member name too long");
            return 0;
        }
        off = text_store(buf, (size_t)len);
        if (off == IT_NO_TEXT) return 0;
        m = add_task(&g_text[off], t->region_name, t->region, asx_json_item(g_in, members, i));
        if (m == NULL) return 0;
        m->group_member = 1;
        t->group_members[t->n_group_members++] = (uint32_t)(m - g_tasks);
        if (t->group.mode == ASX_TASK_GROUP_FIRST_OK) {
            /* Spawned by the group when its turn comes (bind_attempts). */
            if (asx_task_group_add_attempt(&t->group, t->region, interp_poll, m) != ASX_OK) {
                it_fail_task(t, idx, "asx_task_group_add_attempt failed");
                return 0;
            }
            continue;
        }
        if (asx_task_group_spawn(&t->group, t->region, interp_poll, m, &id) != ASX_OK) {
            it_fail_task(t, idx, "asx_task_group_spawn failed");
            return 0;
        }
        if (id == ASX_INVALID_ID) {
            /* Refused by its closing region: the group still joins it
             * (asx_task_group_member_refusal). */
            m->refused = ASX_E_REGION_CLOSED;
            continue;
        }
        m->id = id;
        m->spawned = 1;
        (void)asx_cx_init(&m->cx, m->region, id, ASX_CAP_NONE);
    }
    return 1;
}

/* FIRST_OK attempts the group spawned during its last poll: bind each to
 * its task. An attempt runs only from a later step, so it is bound before
 * any of its events can be projected. */
static void bind_attempts(it_task *t) {
    uint32_t i;
    for (i = 0; i < t->group.spawned && i < t->n_group_members; i++) {
        it_task *m = &g_tasks[t->group_members[i]];
        if (m->spawned || m->refused != ASX_OK) continue;
        if (t->group.members[i] == ASX_INVALID_ID) {
            m->refused = ASX_E_REGION_CLOSED; /* refused by its region */
            continue;
        }
        m->id = t->group.members[i];
        m->spawned = 1;
        (void)asx_cx_init(&m->cx, m->region, m->id, ASX_CAP_NONE);
    }
}

/* Members their admission refused at max_tasks complete without a
 * completion event (nothing ran, lab dispatch): their outcome, the
 * refusal's Cancelled(User), is read before the group poll that may
 * collect them and release their slots. */
static void capture_refused_members(it_task *t) {
    uint32_t i;
    for (i = 0; i < t->n_group_members; i++) {
        it_task *m = &g_tasks[t->group_members[i]];
        asx_status why;
        if (!m->spawned || m->member_done) continue;
        why = admission_refusal(m->id);
        if (why == ASX_OK) continue;
        m->member_outcome = outcome_node(m->id);
        m->member_done = 1;
        m->admission_refusal = why;
    }
}

/* Member i's outcome, captured at its completion; for a member its region
 * refused, the cancellation its join reports. */
static uint32_t group_member_outcome(it_task *t, uint32_t i) {
    it_task *m = &g_tasks[t->group_members[i]];
    asx_cancel_reason r;
    if (!m->member_done && m->refused != ASX_OK &&
        asx_task_group_member_refusal(&t->group, i, &r) == ASX_OK) {
        it_reason captured;
        capture_reason(&r, &captured);
        m->member_outcome = asx_json_new_object(g_out);
        asx_json_set(g_out, m->member_outcome, "tag", asx_json_new_string(g_out, "cancelled"));
        asx_json_set(g_out, m->member_outcome, "reason", reason_node(&captured));
        m->member_done = 1;
    }
    if (!m->member_done) {
        it_fail("group resolved with an unfinished member", m->name);
        return asx_json_new_null(g_out);
    }
    return asx_json_copy(g_out, g_out, m->member_outcome);
}

static void exec_race_deadline(it_task *t, asx_task_id self, uint32_t step, uint32_t idx,
                               step_result *out);

/* A resolved race's value, as Rust's race_all returns it: {winner_index,
 * outcome}, the index only for a winner whose join succeeded: its program
 * returned, ok or err (Rust's Ok((body, index))); a cancelled or panicked
 * winner is race_all's Err, without its index. An owner-cancelled
 * race has no winner: its outcome is the first member panic, else the
 * cancellation with the owner's reason (drain_owner_cancelled_race).
 * ASX_JSON_NONE after a harness failure. */
static uint32_t race_value(it_task *t, uint32_t idx) {
    int32_t w = asx_task_group_winner(&t->group);
    uint32_t outcome = ASX_JSON_NONE;
    uint32_t tag;
    const char *tag_text;
    uint32_t value;
    uint32_t i;
    if (w < 0) {
        if (!t->group.owner_cancelled) {
            it_fail_task(t, idx, "race resolved without a winner");
            return ASX_JSON_NONE;
        }
        for (i = 0; i < t->n_group_members && outcome == ASX_JSON_NONE; i++) {
            asx_outcome o;
            asx_status st;
            if (asx_task_group_member_result(&t->group, i, &o, &st) == ASX_OK &&
                asx_outcome_severity_of(&o) == ASX_OUTCOME_PANICKED) {
                outcome = group_member_outcome(t, i);
            }
        }
        if (outcome == ASX_JSON_NONE) {
            it_reason captured;
            capture_reason(&t->group.owner_reason, &captured);
            outcome = asx_json_new_object(g_out);
            asx_json_set(g_out, outcome, "tag", asx_json_new_string(g_out, "cancelled"));
            asx_json_set(g_out, outcome, "reason", reason_node(&captured));
        }
    } else {
        outcome = group_member_outcome(t, (uint32_t)w);
    }
    tag = asx_json_get(g_out, outcome, "tag");
    tag_text = tag != ASX_JSON_NONE ? asx_json_string(g_out, tag) : NULL;
    value = asx_json_new_object(g_out);
    asx_json_set(g_out, value, "winner_index",
                 w >= 0 && tag_text != NULL &&
                         (strcmp(tag_text, "ok") == 0 || strcmp(tag_text, "err") == 0)
                     ? asx_json_new_u64(g_out, (uint64_t)w)
                     : asx_json_new_null(g_out));
    asx_json_set(g_out, value, "outcome", outcome);
    return value;
}

/* Task-group steps (DSL §3.5). Returns 1 when `op` is one of them. The
 * observation is what the Rust combinator returns: race, {winner_index,
 * outcome} (the index only for a winner whose program returned, ok or
 * err, as Rust reports it); join_all, every member's outcome; quorum, the
 * number of members
 * that succeeded. */
static int exec_group(it_task *t, asx_task_id self, uint32_t step, uint32_t idx, const char *op,
                      step_result *out) {
    asx_task_group_mode mode;
    uint64_t needed = 1;
    asx_status st;
    uint32_t value;
    uint32_t i;
    *out = STEP_NEXT;
    if (strcmp(op, "race") == 0) {
        mode = ASX_TASK_GROUP_RACE;
    } else if (strcmp(op, "join_all") == 0) {
        mode = ASX_TASK_GROUP_JOIN_ALL;
    } else if (strcmp(op, "quorum") == 0) {
        mode = ASX_TASK_GROUP_QUORUM;
    } else if (strcmp(op, "first_ok") == 0) {
        mode = ASX_TASK_GROUP_FIRST_OK;
    } else {
        return 0;
    }
    if (!t->race_wrapper && asx_json_get(g_in, step, "deadline_ns") != ASX_JSON_NONE) {
        if (mode != ASX_TASK_GROUP_RACE) {
            it_fail_task(t, idx, "deadline_ns is only defined for race");
            *out = STEP_END;
            return 1;
        }
        exec_race_deadline(t, self, step, idx, out);
        return 1;
    }
    if (t->phase == 0u) {
        if (mode == ASX_TASK_GROUP_QUORUM &&
            (!asx_json_u64(g_in, asx_json_get(g_in, step, "needed"), &needed) ||
             needed > UINT32_MAX)) {
            it_fail_task(t, idx, "quorum needs `needed`");
            *out = STEP_END;
            return 1;
        }
        /* Rust's quorum refuses an impossible threshold before spawning
         * anything (QuorumError::InvalidQuorum, cx/scope.rs:1831). */
        if (mode == ASX_TASK_GROUP_QUORUM &&
            (needed == 0u || needed > asx_json_count(g_in, asx_json_get(g_in, step, "members")))) {
            observe(t, idx, op, status_node(ASX_E_INVALID_ARGUMENT), asx_json_new_null(g_out));
            return 1;
        }
        if (asx_task_group_init(&t->group, mode, (uint32_t)needed) != ASX_OK ||
            !spawn_group_members(t, step, idx)) {
            if (!g_failed) it_fail_task(t, idx, "task group setup failed");
            *out = STEP_END;
            return 1;
        }
        t->phase = 1u;
    }
    capture_refused_members(t);
    st = asx_task_group_poll(&t->group, self);
    if (mode == ASX_TASK_GROUP_FIRST_OK) bind_attempts(t);
    if (st == ASX_E_PENDING) {
        *out = STEP_PENDING;
        return 1;
    }
    if (mode == ASX_TASK_GROUP_FIRST_OK) {
        /* Ok(value) or a FirstOkError (vocabulary §5): the status is the
         * group result, the value null. */
        observe(t, idx, op, status_node(st), asx_json_new_null(g_out));
        return 1;
    }
    if (mode == ASX_TASK_GROUP_RACE) {
        value = race_value(t, idx);
        if (value == ASX_JSON_NONE) {
            *out = STEP_END;
            return 1;
        }
        if (t->race_wrapper) {
            /* The race inside a deadline wrapper: its owner observes it,
             * and the wrapper returns the race's result (Ok, or Err with
             * the failed winner's JoinError). */
            t->race_result = value;
            t->end = st;
            *out = STEP_END;
            return 1;
        }
    } else if (mode == ASX_TASK_GROUP_JOIN_ALL) {
        value = asx_json_new_array(g_out);
        for (i = 0; i < t->n_group_members; i++) {
            asx_json_push(g_out, value, group_member_outcome(t, i));
        }
    } else {
        /* A QuorumError (vocabulary §5): the status, no value. */
        if (st != ASX_OK) {
            observe(t, idx, op, status_node(st), asx_json_new_null(g_out));
            return 1;
        }
        value = asx_json_new_u64(g_out, asx_task_group_ok_count(&t->group));
    }
    observe(t, idx, op, status_node(ASX_OK), value);
    return 1;
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
    if (strcmp(op, "mutex_lock_until") == 0) {
        /* Rust's lock_until guard is borrowed (not Send), so twin_run drops
         * it in the poll that takes it: the lock is released at once. */
        it_sync *s = sync_by_name(it_str(step, "mutex"), IT_SYNC_MUTEX);
        asx_mutex_guard guard;
        uint64_t deadline;
        if (s == NULL || !asx_json_u64(g_in, asx_json_get(g_in, step, "deadline_ns"), &deadline)) {
            it_fail_task(t, idx,
                         "mutex_lock_until on an undeclared mutex (or without its deadline)");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            if (asx_mutex_lock_until_begin(s->mutex, (asx_time)deadline, &t->wait.mutex) !=
                ASX_OK) {
                it_fail_task(t, idx, "asx_mutex_lock_until_begin failed");
                *out = STEP_END;
                return 1;
            }
            t->phase = 1u;
        }
        st = asx_mutex_poll_lock(&t->wait.mutex, &guard, &t->cx);
        if (st == ASX_OK && asx_mutex_unlock(guard) != ASX_OK) {
            it_fail_task(t, idx, "the unlock after mutex_lock_until failed");
            *out = STEP_END;
            return 1;
        }
        if (sync_wait(t, idx, op, st, cancel_mutex_wait, &t->wait.mutex) == WAIT_PENDING) {
            *out = STEP_PENDING;
        }
        return 1;
    }
    if (strcmp(op, "rwlock_read") == 0 || strcmp(op, "rwlock_write") == 0) {
        const char *name = it_str(step, "rwlock");
        it_sync *s = sync_by_name(name, IT_SYNC_RWLOCK);
        int is_write = strcmp(op, "rwlock_write") == 0;
        uint32_t k = t->n_rw_guards;
        if (s == NULL || k >= IT_MAX_LOCAL) {
            it_fail_task(t, idx, "rwlock step on an undeclared rwlock (or too many guards)");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            st = is_write ? asx_rwlock_write_begin(s->rwlock, &t->wait.rwlock)
                          : asx_rwlock_read_begin(s->rwlock, &t->wait.rwlock);
            if (st != ASX_OK) {
                it_fail_task(t, idx, "asx_rwlock_*_begin failed");
                *out = STEP_END;
                return 1;
            }
            t->phase = 1u;
        }
        st = is_write ? asx_rwlock_poll_write(&t->wait.rwlock, &t->rw_guards[k].write, &t->cx)
                      : asx_rwlock_poll_read(&t->wait.rwlock, &t->rw_guards[k].read, &t->cx);
        if (st == ASX_OK) {
            t->rw_guards[k].name = name;
            t->rw_guards[k].is_write = is_write;
            t->n_rw_guards++;
        }
        if (sync_wait(t, idx, op, st, cancel_rwlock_wait, &t->wait.rwlock) == WAIT_PENDING) {
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
        if (count > UINT32_MAX) {
            it_fail_task(t, idx, "sem_acquire count out of range");
            *out = STEP_END;
            return 1;
        }
        if (t->phase == 0u) {
            /* All or nothing, as Rust's Semaphore::acquire(cx, count); a
             * count of 0 yields an empty permit at once. */
            if (asx_semaphore_acquire_many_begin(s->semaphore, (uint32_t)count, &t->wait.sem) !=
                ASX_OK) {
                it_fail_task(t, idx, "asx_semaphore_acquire_many_begin failed");
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

/* A race with deadline_ns (DSL §3.5), as Rust's Scope::timeout runs it
 * (cx/scope.rs:2130-2196). The race runs in a wrapper task spawned into
 * the owner's region and named "<owner>/g<idx>.timeout" (vocabulary §2):
 * it runs the owner's race step and nothing else, its members keep their
 * plain-race names, and the snapshot lists none of them (vocabulary §6).
 * Each owner poll checks the wrapper's join, then the owner's own cancel
 * (a checkpoint), then the deadline: a sleep timer of the owner's
 * ("<owner>/tm<k>"), registered at the first poll. The deadline aborts
 * the wrapper with CancelReason::timeout() (default attribution), the
 * owner's cancel with the owner's reason, and the owner joins it. The
 * observation is the race's, as a plain race's; a wrapper that ended
 * cancelled without running the race, after the deadline, is TimedOut
 * (ASX_E_TIMED_OUT). A deadline timer still pending at the end is
 * cancelled (Rust drops the sleep). */
static void race_refused_wrapper(it_task *t, asx_task_id self, uint32_t idx, it_task *w,
                                 step_result *out);

static void exec_race_deadline(it_task *t, asx_task_id self, uint32_t step, uint32_t idx,
                               step_result *out) {
    it_task *w;
    asx_outcome ignored;
    asx_status st;
    *out = STEP_PENDING;
    if (t->phase == 0u) {
        uint64_t d;
        asx_time now;
        char buf[160];
        int len;
        uint32_t off;
        asx_task_id id = ASX_INVALID_ID;
        if (!asx_json_u64(g_in, asx_json_get(g_in, step, "deadline_ns"), &d) ||
            asx_runtime_now_ns(&now) != ASX_OK || d > UINT64_MAX - now) {
            it_fail_task(t, idx, "race deadline_ns must be a reachable integer");
            *out = STEP_END;
            return;
        }
        len = snprintf(buf, sizeof(buf), "%s/g%u.timeout", t->name, (unsigned)idx);
        if (len < 0 || (size_t)len >= sizeof(buf)) {
            it_fail_task(t, idx, "race wrapper name too long");
            *out = STEP_END;
            return;
        }
        off = text_store(buf, (size_t)len);
        w = off == IT_NO_TEXT ? NULL
                              : add_task(&g_text[off], t->region_name, t->region, t->program);
        if (w == NULL) {
            *out = STEP_END;
            return;
        }
        w->pc = t->pc;
        w->race_wrapper = 1;
        w->race_owner = (uint32_t)(t - g_tasks);
        w->race_result = ASX_JSON_NONE;
        w->group_member = 1;
        st = asx_task_spawn(w->region, interp_poll, w, &id);
        if (st == ASX_E_REGION_CLOSED && asx_scheduler_last_spawn_refusal() != 0u) {
            /* Scope::timeout spawns through the mailbox: the spawn goes
             * through and the next step's admission refuses it, resolving
             * its join as Cancelled(ParentCancelled). */
            w->refused = st;
            w->refusal = asx_scheduler_last_spawn_refusal();
        } else if (st != ASX_OK) {
            it_fail_task(t, idx, "race timeout wrapper spawn refused");
            *out = STEP_END;
            return;
        } else {
            w->id = id;
            w->spawned = 1;
            (void)asx_cx_init(&w->cx, w->region, id, ASX_CAP_NONE);
        }
        t->race_wrapper_task = (uint32_t)(w - g_tasks);
        t->race_deadline = now + d;
        t->race_timer_armed = 0;
        t->phase = 1u;
    }
    w = &g_tasks[t->race_wrapper_task];
    if (w->refused != ASX_OK) {
        race_refused_wrapper(t, self, idx, w, out);
        return;
    }
    if (!task_completed(w->id)) {
        asx_checkpoint_result cr;
        asx_cancel_reason r;
        asx_time now;
        if (asx_task_join_poll(self, w->id, &ignored) != ASX_E_PENDING) {
            it_fail_task(t, idx, "join of the race wrapper did not wait");
            *out = STEP_END;
            return;
        }
        if (t->phase == 2u) return;
        if (asx_checkpoint(self, &cr) != ASX_OK || asx_runtime_now_ns(&now) != ASX_OK) {
            it_fail_task(t, idx, "race deadline checkpoint failed");
            *out = STEP_END;
            return;
        }
        if (cr.cancelled) {
            if (asx_task_get_cancel_reason(self, &r) != ASX_OK) {
                it_fail_task(t, idx, "cancelled owner without a reason");
                *out = STEP_END;
                return;
            }
        } else if (now >= t->race_deadline) {
            if (t->race_timer_armed) {
                st = asx_task_complete_timer(self);
                (void)st;
                t->race_timer_armed = 0;
            }
            r = asx_cancel_reason_default(ASX_CANCEL_TIMEOUT, NULL);
        } else {
            st = asx_task_wait_until(self, t->race_deadline);
            (void)st;
            t->race_timer_armed = 1;
            return;
        }
        st = asx_task_abort_request(w->id, &r);
        (void)st;
        t->phase = 2u;
        return;
    }
    /* The wrapper completed: join it, drop the deadline timer, observe. A
     * wrapper its admission refused never ran: it is done, as a member. */
    {
        int in_time = t->phase == 1u;
        uint32_t wrapper_outcome;
        w->admission_refusal = admission_refusal(w->id);
        if (w->admission_refusal != ASX_OK) w->member_done = 1;
        wrapper_outcome = take_outcome(w, &st);
        uint32_t value;
        if (st != ASX_OK) {
            it_fail_task(t, idx, "join of the race wrapper failed");
            *out = STEP_END;
            return;
        }
        if (t->race_timer_armed) {
            st = asx_task_cancel_timer(self);
            (void)st;
            t->race_timer_armed = 0;
        }
        *out = STEP_NEXT;
        /* make_timed_result (combinator/timeout.rs:257-279) over the
         * wrapper's join: a wrapper whose race returned (its task ok or err)
         * gives the race's result; a cancelled one (cancelled before it ran,
         * or cancellation-dominated: spawn_in keeps a value only after an
         * acknowledged cancel, task_handle.rs:173-198) or a panicked one is
         * a failed winner without an index, except that a cancelled one
         * after the deadline is TimedOut. */
        {
            uint32_t tag = asx_json_get(g_out, wrapper_outcome, "tag");
            const char *tag_text = tag != ASX_JSON_NONE ? asx_json_string(g_out, tag) : NULL;
            int ended_cancelled = tag_text != NULL && strcmp(tag_text, "cancelled") == 0;
            int ended_panicked = tag_text != NULL && strcmp(tag_text, "panicked") == 0;
            if (ended_cancelled && !in_time) {
                observe_status(t, idx, "race", ASX_E_TIMED_OUT);
                return;
            }
            if (!ended_cancelled && !ended_panicked && w->race_result != ASX_JSON_NONE) {
                value = w->race_result;
            } else {
                value = asx_json_new_object(g_out);
                asx_json_set(g_out, value, "winner_index", asx_json_new_null(g_out));
                asx_json_set(g_out, value, "outcome", wrapper_outcome);
            }
        }
        observe(t, idx, "race", status_node(ASX_OK), value);
    }
}

/* A race with deadline_ns whose wrapper's spawn the region refused: Rust's
 * Scope::timeout still waits on the wrapper's join, which resolves as
 * Cancelled(ParentCancelled) when the next step's admission refuses it.
 * Each owner poll checks that join, then the owner's cancel, then the
 * deadline (cx/scope.rs:2152-2170). The join resolved first: Completed
 * with that cancellation, a failed winner without an index. The owner's
 * cancel or the deadline first: TimedOut once the refusal resolves (the
 * wrapper never ran, so it never acknowledged the abort). */
static void race_refused_wrapper(it_task *t, asx_task_id self, uint32_t idx, it_task *w,
                                 step_result *out) {
    asx_checkpoint_result cr;
    asx_time now;
    asx_status st;
    *out = STEP_PENDING;
    if (asx_task_refusal_delivered(w->refusal)) {
        if (t->race_timer_armed) {
            st = asx_task_cancel_timer(self);
            (void)st;
            t->race_timer_armed = 0;
        }
        *out = STEP_NEXT;
        if (t->phase == 1u) {
            asx_cancel_reason r = asx_cancel_reason_default(ASX_CANCEL_PARENT, NULL);
            it_reason captured;
            uint32_t outcome = asx_json_new_object(g_out);
            uint32_t value = asx_json_new_object(g_out);
            capture_reason(&r, &captured);
            asx_json_set(g_out, outcome, "tag", asx_json_new_string(g_out, "cancelled"));
            asx_json_set(g_out, outcome, "reason", reason_node(&captured));
            asx_json_set(g_out, value, "winner_index", asx_json_new_null(g_out));
            asx_json_set(g_out, value, "outcome", outcome);
            observe(t, idx, "race", status_node(ASX_OK), value);
        } else {
            observe_status(t, idx, "race", ASX_E_TIMED_OUT);
        }
        return;
    }
    if (t->phase == 1u) {
        if (asx_checkpoint(self, &cr) != ASX_OK || asx_runtime_now_ns(&now) != ASX_OK) {
            it_fail_task(t, idx, "race deadline checkpoint failed");
            *out = STEP_END;
            return;
        }
        if (cr.cancelled || now >= t->race_deadline) {
            if (!cr.cancelled && t->race_timer_armed) {
                st = asx_task_complete_timer(self);
                (void)st;
                t->race_timer_armed = 0;
            }
            t->phase = 2u;
        } else if (!t->race_timer_armed) {
            st = asx_task_wait_until(self, t->race_deadline);
            (void)st;
            t->race_timer_armed = 1;
        }
    }
    st = asx_task_await_refusal(self, w->refusal);
    (void)st;
}

/* Interval and DelayQueue objects (DSL §3.9), held by the task that made
 * them under a name. They live in global pools so a task's frame stays
 * small; a key name stays bound after its entry expired (the key is then
 * stale, not unknown). */
#define IT_MAX_INTERVALS 8u
#define IT_MAX_DELAY_QUEUES 4u
#define IT_MAX_DELAY_KEYS 32u

typedef struct {
    const char *name;
    uint32_t owner; /* index in g_tasks */
    asx_interval_state state;
} it_interval;

typedef struct {
    const char *name;
    uint32_t owner;
    asx_delay_queue queue;
} it_delay_queue;

typedef struct {
    const char *name;
    uint32_t owner;
    uint32_t queue; /* index in g_delay_queues */
    asx_delay_key key;
} it_delay_key;

static it_interval g_intervals[IT_MAX_INTERVALS];
static uint32_t g_n_intervals;
static it_delay_queue g_delay_queues[IT_MAX_DELAY_QUEUES];
static uint32_t g_n_delay_queues;
static it_delay_key g_delay_keys[IT_MAX_DELAY_KEYS];
static uint32_t g_n_delay_keys;

static it_interval *interval_of(const it_task *t, const char *name) {
    uint32_t i;
    for (i = 0; name != NULL && i < g_n_intervals; i++) {
        if (g_intervals[i].owner == (uint32_t)(t - g_tasks) &&
            strcmp(g_intervals[i].name, name) == 0) {
            return &g_intervals[i];
        }
    }
    return NULL;
}

static it_delay_queue *delay_queue_of(const it_task *t, const char *name) {
    uint32_t i;
    for (i = 0; name != NULL && i < g_n_delay_queues; i++) {
        if (g_delay_queues[i].owner == (uint32_t)(t - g_tasks) &&
            strcmp(g_delay_queues[i].name, name) == 0) {
            return &g_delay_queues[i];
        }
    }
    return NULL;
}

static it_delay_key *delay_key_of(const it_task *t, const char *name) {
    uint32_t i;
    for (i = 0; name != NULL && i < g_n_delay_keys; i++) {
        if (g_delay_keys[i].owner == (uint32_t)(t - g_tasks) &&
            strcmp(g_delay_keys[i].name, name) == 0) {
            return &g_delay_keys[i];
        }
    }
    return NULL;
}

/* An expired or removed entry as an observation value. */
static uint32_t delay_expired_value(const asx_delay_expired *e) {
    uint32_t o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "value", asx_json_new_u64(g_out, e->value));
    asx_json_set(g_out, o, "deadline_ns", asx_json_new_u64(g_out, e->deadline));
    return o;
}

/* The interval and DelayQueue steps (DSL §3.9), none of which suspends:
 * they read the clock (Rust cx.now() / wall_now(), virtual under the lab)
 * and leave any waiting to the program's own sleep steps. Returns 1 when
 * `op` is one of them, with the step's result in *out. */
static int exec_time_op(it_task *t, uint32_t step, uint32_t idx, const char *label, const char *op,
                        step_result *out) {
    uint32_t me = (uint32_t)(t - g_tasks);
    asx_time now = 0;
    uint64_t n = 0;
    asx_status st;

    *out = STEP_NEXT;
    if (strncmp(op, "interval_", 9) != 0 && strncmp(op, "delay_queue_", 12) != 0) return 0;
    if (asx_runtime_now_ns(&now) != ASX_OK) {
        it_fail_task(t, idx, "the runtime clock is not readable");
        *out = STEP_END;
        return 1;
    }

    if (strcmp(op, "interval_new") == 0) {
        /* interval(cx.now(), period) / interval_at(start_ns, period), then
         * set_missed_tick_behavior (time/interval.rs:443, :470, :228). */
        const char *as = it_str(step, "as");
        const char *behavior = it_str(step, "behavior");
        asx_missed_tick_behavior mode = ASX_MISSED_TICK_BURST;
        asx_time start = now;
        it_interval *iv;
        if (as == NULL || g_n_intervals >= IT_MAX_INTERVALS || interval_of(t, as) != NULL ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "period_ns"), &n) || n == 0u) {
            it_fail_task(t, idx, "interval_new needs a fresh name and a period_ns above 0");
            *out = STEP_END;
            return 1;
        }
        (void)asx_json_u64(g_in, asx_json_get(g_in, step, "start_ns"), &start);
        if (behavior != NULL && strcmp(behavior, "delay") == 0) mode = ASX_MISSED_TICK_DELAY;
        if (behavior != NULL && strcmp(behavior, "skip") == 0) mode = ASX_MISSED_TICK_SKIP;
        iv = &g_intervals[g_n_intervals];
        st = asx_interval_init_at(&iv->state, start, n, 0u);
        if (st == ASX_OK) st = asx_interval_set_missed_tick_behavior(&iv->state, mode);
        if (st != ASX_OK) {
            it_fail_task(t, idx, "asx_interval_init_at failed");
            *out = STEP_END;
            return 1;
        }
        iv->name = as;
        iv->owner = me;
        g_n_intervals++;
        observe_status(t, idx, label, ASX_OK);
        return 1;
    }
    if (strcmp(op, "interval_tick") == 0) {
        /* poll_tick(now), or the unmoved deadline when nothing is due
         * (interval.rs:261, :293): {time_ns, due} */
        it_interval *iv = interval_of(t, it_str(step, "interval"));
        asx_time tick_time = 0;
        int due = 0;
        uint32_t value;
        if (iv == NULL || asx_interval_tick_at(&iv->state, now, &tick_time, &due) != ASX_OK) {
            it_fail_task(t, idx, "interval_tick on an interval this task does not hold");
            *out = STEP_END;
            return 1;
        }
        value = asx_json_new_object(g_out);
        asx_json_set(g_out, value, "time_ns", asx_json_new_u64(g_out, tick_time));
        asx_json_set(g_out, value, "due", asx_json_new_bool(g_out, due ? 1 : 0));
        observe(t, idx, label, status_node(ASX_OK), value);
        return 1;
    }
    if (strcmp(op, "delay_queue_new") == 0) {
        const char *as = it_str(step, "as");
        if (as == NULL || g_n_delay_queues >= IT_MAX_DELAY_QUEUES ||
            delay_queue_of(t, as) != NULL) {
            it_fail_task(t, idx, "delay_queue_new needs a fresh name and a free queue");
            *out = STEP_END;
            return 1;
        }
        st = asx_delay_queue_init(&g_delay_queues[g_n_delay_queues].queue);
        g_delay_queues[g_n_delay_queues].name = as;
        g_delay_queues[g_n_delay_queues].owner = me;
        g_n_delay_queues++;
        observe_status(t, idx, label, st);
        return 1;
    }

    /* Every other step names a queue the task holds. */
    {
        it_delay_queue *dq = delay_queue_of(t, it_str(step, "queue"));
        it_delay_key *named = delay_key_of(t, it_str(step, "key"));
        const char *keyword = op + 12;
        asx_delay_expired e;
        asx_delay_poll_state state = ASX_DELAY_EMPTY;
        if (dq == NULL) {
            it_fail_task(t, idx, "a DelayQueue step names a queue this task does not hold");
            *out = STEP_END;
            return 1;
        }
        if (strcmp(keyword, "insert") == 0 || strcmp(keyword, "insert_at") == 0) {
            /* insert(value, timeout) / insert_at(value, deadline) (delay_queue.rs:139, :144) */
            const char *as = it_str(step, "as");
            const int at = strcmp(keyword, "insert_at") == 0;
            uint64_t value = 0;
            asx_delay_key key;
            if (as == NULL || delay_key_of(t, as) != NULL || g_n_delay_keys >= IT_MAX_DELAY_KEYS ||
                !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &value) ||
                !asx_json_u64(g_in, asx_json_get(g_in, step, at ? "deadline_ns" : "timeout_ns"),
                              &n)) {
                it_fail_task(t, idx,
                             "a DelayQueue insert needs a fresh key name, a value and a time");
                *out = STEP_END;
                return 1;
            }
            st = at ? asx_delay_queue_insert_at(&dq->queue, value, n, &key)
                    : asx_delay_queue_insert(&dq->queue, now, value, n, &key);
            if (st != ASX_OK) {
                it_fail_task(t, idx, "the DelayQueue is full (C holds ASX_DELAY_QUEUE_CAPACITY)");
                *out = STEP_END;
                return 1;
            }
            g_delay_keys[g_n_delay_keys].name = as;
            g_delay_keys[g_n_delay_keys].owner = me;
            g_delay_keys[g_n_delay_keys].queue = (uint32_t)(dq - g_delay_queues);
            g_delay_keys[g_n_delay_keys].key = key;
            g_n_delay_keys++;
            observe_status(t, idx, label, ASX_OK);
            return 1;
        }
        if (strcmp(keyword, "len") == 0) {
            observe(t, idx, label, status_node(ASX_OK),
                    asx_json_new_u64(g_out, asx_delay_queue_len(&dq->queue)));
            return 1;
        }
        if (strcmp(keyword, "clear") == 0) {
            observe_status(t, idx, label, asx_delay_queue_clear(&dq->queue));
            return 1;
        }
        if (strcmp(keyword, "poll") == 0) {
            /* poll_expired(cx) (delay_queue.rs:260): a due entry, the end of
             * an empty queue (ASX_E_CHANNEL_EMPTY), or a wait the scenario
             * must not ask for: it would register the queue's own Sleep. */
            st = asx_delay_queue_poll_at(&dq->queue, now, &state, &e);
            if (st != ASX_OK || state == ASX_DELAY_WAITING) {
                it_fail_task(t, idx,
                             "delay_queue_poll with nothing due: sleep until an entry is due");
                *out = STEP_END;
                return 1;
            }
            if (state == ASX_DELAY_EMPTY) {
                observe_status(t, idx, label, ASX_E_CHANNEL_EMPTY);
            } else {
                observe(t, idx, label, status_node(ASX_OK), delay_expired_value(&e));
            }
            return 1;
        }
        if (named == NULL || named->queue != (uint32_t)(dq - g_delay_queues)) {
            it_fail_task(t, idx, "a DelayQueue step names a key of another queue or none");
            *out = STEP_END;
            return 1;
        }
        if (strcmp(keyword, "remove") == 0) {
            /* try_remove(&key) (:170): the entry, or None for a dead key */
            st = asx_delay_queue_remove(&dq->queue, &named->key, &e);
            observe(t, idx, label, status_node(st),
                    st == ASX_OK ? delay_expired_value(&e) : ASX_JSON_NONE);
            return 1;
        }
        if (strcmp(keyword, "contains") == 0) {
            observe(t, idx, label, status_node(ASX_OK),
                    asx_json_new_bool(g_out, asx_delay_queue_contains(&dq->queue, &named->key)));
            return 1;
        }
        if (strcmp(keyword, "deadline") == 0) {
            /* deadline(&key) (:221) panics on a dead key: a harness error */
            asx_time d = 0;
            if (asx_delay_queue_deadline(&dq->queue, &named->key, &d) != ASX_OK) {
                it_fail_task(t, idx, "delay_queue_deadline of a dead key (Rust panics)");
                *out = STEP_END;
                return 1;
            }
            observe(t, idx, label, status_node(ASX_OK), asx_json_new_u64(g_out, d));
            return 1;
        }
        if (strcmp(keyword, "reset") == 0 || strcmp(keyword, "reset_at") == 0) {
            /* reset(&key, timeout) / reset_at(&key, deadline) (:190, :202); a
             * dead key panics in Rust: a harness error */
            const int at = strcmp(keyword, "reset_at") == 0;
            if (!asx_json_u64(g_in, asx_json_get(g_in, step, at ? "deadline_ns" : "timeout_ns"),
                              &n)) {
                it_fail_task(t, idx, "a DelayQueue reset needs a time");
                *out = STEP_END;
                return 1;
            }
            st = at ? asx_delay_queue_reset_at(&dq->queue, &named->key, n)
                    : asx_delay_queue_reset(&dq->queue, &named->key, now, n);
            if (st != ASX_OK) {
                it_fail_task(t, idx, "delay_queue_reset of a dead key (Rust panics)");
                *out = STEP_END;
                return 1;
            }
            observe_status(t, idx, label, ASX_OK);
            return 1;
        }
    }
    it_fail_task(t, idx, "unknown interval or DelayQueue step");
    *out = STEP_END;
    return 1;
}

/* watch_send_modify's closure: `*value += add`, wrapping (Rust's
 * wrapping_add on u64). */
/* ASX_ANALYZER_WAIVER("callback signature: a const ctx would need a function-pointer cast") */
static void watch_add_fn(void *ctx, uint64_t *value) { *value += *(const uint64_t *)ctx; }

/* The watch steps that never suspend (DSL §3.6). Returns 1 when `op` is one
 * of them, with the step's result in *out. Sender steps use the task's
 * sender; receiver steps its declared receiver, or the clone `receiver`
 * names. */
static int exec_watch_op(it_task *t, uint32_t step, uint32_t idx, const char *label, const char *op,
                         step_result *out) {
    const char *ch = it_str(step, "channel");
    const char *alias = it_str(step, "receiver");
    asx_status st;
    uint64_t v = 0;

    *out = STEP_NEXT;
    if (strcmp(op, "watch_send_modify") == 0 || strcmp(op, "watch_receiver_count") == 0) {
        it_channel *c = owned_sender(t, ch, IT_CH_WATCH);
        uint64_t add = 0;
        int modify = strcmp(op, "watch_send_modify") == 0;
        if (c == NULL || (modify && !asx_json_u64(g_in, asx_json_get(g_in, step, "add"), &add))) {
            it_fail_task(t, idx, "watch sender step needs an owned sender (and an add)");
            *out = STEP_END;
            return 1;
        }
        if (modify) {
            /* tx.send_modify(|v| *v = v.wrapping_add(add)) (watch.rs:530) */
            observe_status(t, idx, label, asx_watch_send_modify(&c->watch, watch_add_fn, &add));
        } else {
            /* tx.receiver_count() (:627): the value is the count */
            observe(t, idx, label, status_node(ASX_OK),
                    asx_json_new_u64(g_out, asx_watch_receiver_count(&c->watch)));
        }
        return 1;
    }
    if (strcmp(op, "watch_is_closed") == 0) {
        const char *end = it_str(step, "end");
        int sender = end != NULL && strcmp(end, "sender") == 0;
        it_channel *c = sender ? owned_sender(t, ch, IT_CH_WATCH) : NULL;
        it_receiver *r = !sender && end != NULL && strcmp(end, "receiver") == 0
                             ? owned_receiver_as(t, ch, IT_CH_WATCH, alias)
                             : NULL;
        if (c == NULL && r == NULL) {
            it_fail_task(t, idx, "watch_is_closed on an end this task does not hold");
            *out = STEP_END;
            return 1;
        }
        /* tx.is_closed() (:634) / rx.is_closed() (:977), a boolean value */
        v = c != NULL ? (uint64_t)asx_watch_sender_is_closed(&c->watch)
                      : (uint64_t)asx_watch_receiver_is_closed(&r->watch);
        observe(t, idx, label, status_node(ASX_OK), asx_json_new_bool(g_out, v != 0u ? 1 : 0));
        return 1;
    }
    if (strcmp(op, "watch_borrow") == 0 || strcmp(op, "watch_borrow_and_update") == 0 ||
        strcmp(op, "watch_mark_seen") == 0 || strcmp(op, "watch_has_changed") == 0 ||
        strcmp(op, "watch_clone") == 0) {
        it_channel *c = channel_by_name(ch);
        it_receiver *r = owned_receiver_as(t, ch, IT_CH_WATCH, alias);
        if (c == NULL || r == NULL) {
            it_fail_task(t, idx, "watch receiver step on a receiver this task does not hold");
            *out = STEP_END;
            return 1;
        }
        if (strcmp(op, "watch_borrow") == 0) {
            /* rx.borrow() (:904): marks nothing seen */
            st = asx_watch_borrow(&r->watch, &v);
            observe(t, idx, label, status_node(st),
                    st == ASX_OK ? asx_json_new_u64(g_out, v) : ASX_JSON_NONE);
        } else if (strcmp(op, "watch_borrow_and_update") == 0) {
            /* rx.borrow_and_update() (:917) */
            st = asx_watch_recv(&r->watch, &v);
            observe(t, idx, label, status_node(st),
                    st == ASX_OK ? asx_json_new_u64(g_out, v) : ASX_JSON_NONE);
        } else if (strcmp(op, "watch_mark_seen") == 0) {
            /* rx.mark_seen() (:961) */
            observe_status(t, idx, label, asx_watch_mark_seen(&r->watch));
        } else if (strcmp(op, "watch_has_changed") == 0) {
            /* rx.has_changed() (:970), a boolean value */
            observe(t, idx, label, status_node(ASX_OK),
                    asx_json_new_bool(g_out, asx_watch_has_changed(&r->watch) ? 1 : 0));
        } else {
            /* rx.clone() (:1057), kept by the task under the name `as` */
            const char *as = it_str(step, "as");
            if (as == NULL || c->n_receivers >= IT_MAX_SUBSCRIBERS ||
                owned_receiver_as(t, ch, IT_CH_WATCH, as) != NULL) {
                it_fail_task(t, idx, "watch_clone needs a fresh name and room for a receiver");
                *out = STEP_END;
                return 1;
            }
            st = asx_watch_receiver_clone(&r->watch, &c->receivers[c->n_receivers].watch);
            if (st == ASX_OK) {
                it_receiver *clone = &c->receivers[c->n_receivers++];
                clone->task = (uint32_t)(t - g_tasks);
                clone->open = 1;
                clone->alias = as;
            }
            observe_status(t, idx, label, st);
        }
        return 1;
    }
    return 0;
}

static it_server *server_by_name(const char *name);

/* Monitors (DSL §3.10). A task's `monitor` and a server's `server_monitor`
 * are held by name until `demonitor` / `server_demonitor`; the table is
 * bounded by the runtime's own (ASX_MONITOR_CAPACITY). */
typedef struct {
    const char *name;
    uint32_t owner;     /* index in g_tasks of the task that made it */
    const char *target; /* the monitored task's name */
    asx_monitor_ref ref;
    int server; /* made by server_monitor */
    int dropped;
} it_monitor;

static it_monitor g_monitors[ASX_MONITOR_CAPACITY];
static uint32_t g_n_monitors = 0;

static it_monitor *monitor_of(const it_task *t, const char *name, int server) {
    uint32_t i;
    uint32_t owner = (uint32_t)(t - g_tasks);
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_monitors; i++) {
        const it_monitor *m = &g_monitors[i];
        if (m->owner == owner && m->server == server && !m->dropped && strcmp(m->name, name) == 0) {
            return &g_monitors[i];
        }
    }
    return NULL;
}

/* A DOWN's or an EXIT's reason as the DSL names it (twin_run's
 * down_reason_kind). */
static const char *down_reason_kind(asx_outcome_severity severity) {
    switch (severity) {
    case ASX_OUTCOME_OK: return "normal";
    case ASX_OUTCOME_ERR: return "error";
    case ASX_OUTCOME_CANCELLED: return "cancelled";
    case ASX_OUTCOME_PANICKED: return "panicked";
    }
    return "error";
}

/* handle_info of the built-in servers: one user trace per system message,
 * the text twin_run's trace_system_msg writes (DSL §3.10). */
static asx_status server_info(void *state, const asx_actor_info_msg *m, asx_actor_handle self) {
    asx_task_id task = ASX_INVALID_ID;
    char message[96];
    (void)state;
    if (m->kind == ASX_ACTOR_INFO_USER || asx_actor_task_id(self, &task) != ASX_OK) return ASX_OK;
    if (m->kind == ASX_ACTOR_INFO_DOWN) {
        (void)snprintf(message, sizeof(message), "down:ref=%llu reason=%s vt=%llu",
                       (unsigned long long)m->ref, down_reason_kind(m->reason.severity),
                       (unsigned long long)m->vt);
    } else if (m->kind == ASX_ACTOR_INFO_EXIT) {
        (void)snprintf(message, sizeof(message), "exit:reason=%s vt=%llu",
                       down_reason_kind(m->reason.severity), (unsigned long long)m->vt);
    } else {
        (void)snprintf(message, sizeof(message), "timeout");
    }
    asx_trace_user(task, message);
    return ASX_OK;
}

/* The monitor steps. Returns 1 when `op` is one; `may_wait` is 0 where a
 * step may not suspend (inside `masked`): the suspending ones are then not
 * handled. `label` is the observation's op. */
static int exec_monitor_op(it_task *t, asx_task_id self, uint32_t step, uint32_t idx,
                           const char *label, const char *op, int may_wait, step_result *out) {
    asx_status st;
    it_monitor *m;

    *out = STEP_NEXT;
    if (strcmp(op, "demonitor") == 0 || strcmp(op, "server_demonitor") == 0) {
        int server = op[0] == 's';
        m = monitor_of(t, it_str(step, "monitor"), server);
        if (m == NULL || (server && server_by_name(it_str(step, "server")) == NULL)) {
            it_fail_task(t, idx, "demonitor of a monitor this task does not hold");
            *out = STEP_END;
            return 1;
        }
        /* Dropping the Monitor / GenServerHandle::demonitor: a command of
         * the region-command lane (DSL §3.10). */
        if (server) {
            asx_demonitor(m->ref);
        } else {
            asx_monitor_release(m->ref);
        }
        m->dropped = 1;
        observe_status(t, idx, label, ASX_OK);
        return 1;
    }
    if (!may_wait) return 0;
    if (strcmp(op, "monitor") == 0 || strcmp(op, "server_monitor") == 0) {
        int server = op[0] == 's';
        const char *as = it_str(step, "as");
        it_task *target = task_by_name(it_str(step, "task"));
        it_server *srv = server ? server_by_name(it_str(step, "server")) : NULL;
        asx_monitor_ref ref = 0;
        if (as == NULL || target == NULL || !target->spawned || monitor_of(t, as, server) != NULL ||
            g_n_monitors >= ASX_MONITOR_CAPACITY || (server && (srv == NULL || srv->released))) {
            it_fail_task(t, idx, "monitor needs a new `as`, a spawned task and room for a monitor");
            *out = STEP_END;
            return 1;
        }
        st = server ? asx_monitor_open_server_poll(self, srv->handle, target->id, &ref)
                    : asx_monitor_open_poll(self, target->id, &ref);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        if (st == ASX_OK) {
            m = &g_monitors[g_n_monitors++];
            m->name = as;
            m->owner = (uint32_t)(t - g_tasks);
            m->target = target->name;
            m->ref = ref;
            m->server = server;
            m->dropped = 0;
        }
        observe(t, idx, label, status_node(st),
                st == ASX_OK ? asx_json_new_u64(g_out, ref) : ASX_JSON_NONE);
        return 1;
    }
    if (strcmp(op, "await_down") == 0) {
        asx_down down;
        m = monitor_of(t, it_str(step, "monitor"), 0);
        if (m == NULL) {
            it_fail_task(t, idx, "await_down of a monitor this task does not hold");
            *out = STEP_END;
            return 1;
        }
        st = asx_monitor_down_poll(self, &t->cx, m->ref, &down);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        if (st == ASX_OK) {
            uint32_t value = asx_json_new_object(g_out);
            asx_json_set(g_out, value, "monitored", asx_json_new_string(g_out, m->target));
            asx_json_set(g_out, value, "reason",
                         asx_json_new_string(g_out, down_reason_kind(down.reason.severity)));
            asx_json_set(g_out, value, "ref", asx_json_new_u64(g_out, down.ref));
            observe(t, idx, label, status_node(ASX_OK), value);
        } else {
            observe_status(t, idx, label, st);
        }
        return 1;
    }
    return 0;
}

/* Steps that never suspend; the only ones allowed inside `masked`. */
static step_result exec_sync(it_task *t, asx_task_id self, uint32_t step, uint32_t idx,
                             const char *label);

typedef struct {
    it_task *task;
    uint32_t steps;
    uint32_t index;
    const char *label;
    step_result result;
} it_masked_run;

static asx_status exec_masked_steps(void *data, asx_task_id self) {
    it_masked_run *run = (it_masked_run *)data;
    uint32_t n = asx_json_count(g_in, run->steps);
    uint32_t j;

    for (j = 0; j < n && run->result == STEP_NEXT; j++) {
        uint32_t inner = asx_json_item(g_in, run->steps, j);
        char inner_label[512];
        const char *inner_op = it_str(inner, "op");
        int len = snprintf(inner_label, sizeof(inner_label), "%s/%u/%s", run->label,
                           (unsigned)(j + 1u), inner_op != NULL ? inner_op : "");
        if (len < 0 || (size_t)len >= sizeof(inner_label)) {
            it_fail_task(run->task, run->index, "masked nesting too deep for its label");
            run->result = STEP_END;
            break;
        }
        run->result = exec_sync(run->task, self, inner, run->index, inner_label);
    }
    return ASX_OK;
}

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
                /* The body returns Ok, as Rust's; the runtime decides the
                 * outcome (Cancelled, unless a spawned child keeps it). */
                t->end = ASX_OK;
                return STEP_END;
            }
            return STEP_NEXT;
        }
        observe_status(t, idx, label, ASX_OK);
        return STEP_NEXT;
    }
    if (strcmp(op, "is_cancel_requested") == 0) {
        /* cx.is_cancel_requested() (cx.rs:2641): the task's Cx, read only. */
        asx_cx cx;
        (void)asx_cx_init(&cx, t->region, self, ASX_CAP_NONE);
        observe(t, idx, label, status_node(ASX_OK),
                asx_json_new_bool(g_out, asx_cx_is_cancelled(&cx) ? 1 : 0));
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
    if (strcmp(op, "handoff") == 0) {
        const char *name = it_str(step, "obligation");
        const char *destination = it_str(step, "task");
        const char *alias = it_str(step, "as");
        it_local_obligation *o = local_obligation(t, name);
        it_task *target = task_by_name(destination);
        uint32_t slot = 0u;
        if (name == NULL || destination == NULL || alias == NULL) {
            it_fail_task(t, idx, "handoff needs obligation, task and as");
            return STEP_END;
        }
        if (o == NULL || target == NULL) {
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        if (target == t) {
            st = asx_obligation_handoff(o->id, target->id);
            observe_status(t, idx, label, st);
            return STEP_NEXT;
        }
        if (local_obligation(target, alias) != NULL) {
            it_fail_task(t, idx, "handoff destination token already exists");
            return STEP_END;
        }
        while (slot < target->n_obligations && target->obligations[slot].name != NULL) slot++;
        if (slot >= IT_MAX_LOCAL) {
            it_fail_task(t, idx, "too many obligations in handoff destination");
            return STEP_END;
        }
        st = asx_obligation_handoff(o->id, target->id);
        if (st == ASX_OK) {
            target->obligations[slot].name = alias;
            target->obligations[slot].id = o->id;
            if (slot == target->n_obligations) target->n_obligations++;
            o->name = NULL;
            o->id = ASX_INVALID_ID;
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "commit") == 0 || strcmp(op, "abort") == 0 || strcmp(op, "leak") == 0) {
        it_local_obligation *o = local_obligation(t, it_str(step, "obligation"));
        if (o == NULL) {
            /* Its reserve failed, or a step resolved it (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
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
            /* drop(token): leaked at once (Rust posts the Leak when the
             * token drops). */
            st = asx_obligation_drop(o->id);
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
            /* Its mutex_lock or sem_acquire failed (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        i--;
        st = is_mutex ? release_guard(t, i) : release_permit(t, i);
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "sem_forget") == 0) {
        /* Forget the most recent permit held on the semaphore: its
         * permits never return (Rust SemaphorePermit::forget). */
        const char *name = it_str(step, "semaphore");
        uint32_t i = t->n_permits;
        while (i > 0u && (name == NULL || strcmp(t->permits[i - 1u].name, name) != 0)) i--;
        if (i == 0u) {
            /* Its sem_acquire failed (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        i--;
        st = asx_semaphore_forget(t->permits[i].permit);
        for (; i + 1u < t->n_permits; i++) t->permits[i] = t->permits[i + 1u];
        t->n_permits--;
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "sem_add_permits") == 0) {
        it_sync *s = sync_by_name(it_str(step, "semaphore"), IT_SYNC_SEMAPHORE);
        uint64_t count = 0;
        if (s == NULL || !asx_json_u64(g_in, asx_json_get(g_in, step, "count"), &count) ||
            count > UINT32_MAX) {
            it_fail_task(t, idx, "sem_add_permits needs a declared semaphore and a count");
            return STEP_END;
        }
        st = asx_semaphore_add_permits(s->semaphore, (uint32_t)count);
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "rwlock_unlock") == 0) {
        /* Release the most recent guard (read or write) held on the lock. */
        const char *name = it_str(step, "rwlock");
        uint32_t i = t->n_rw_guards;
        while (i > 0u && (name == NULL || strcmp(t->rw_guards[i - 1u].name, name) != 0)) i--;
        if (i == 0u) {
            /* Its rwlock_read or rwlock_write failed (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        st = release_rw_guard(t, i - 1u);
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "rwlock_downgrade") == 0) {
        /* The most recent write guard held on the lock becomes a read
         * guard in place (RwLockWriteGuard::downgrade, rwlock.rs:1110). */
        const char *name = it_str(step, "rwlock");
        uint32_t i = t->n_rw_guards;
        while (i > 0u && (name == NULL || strcmp(t->rw_guards[i - 1u].name, name) != 0 ||
                          !t->rw_guards[i - 1u].is_write)) {
            i--;
        }
        if (i == 0u) {
            /* No write guard held on it (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        st = asx_rwlock_downgrade(t->rw_guards[i - 1u].write, &t->rw_guards[i - 1u].read);
        if (st == ASX_OK) t->rw_guards[i - 1u].is_write = 0;
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
        if (i >= t->n_send_permits) {
            /* Its reserve_send failed (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        if (strcmp(op, "permit_send") == 0 &&
            !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &v)) {
            it_fail_task(t, idx, "permit_send needs a value");
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
        const char *ch = it_str(step, "channel");
        it_channel *c = channel_by_name(ch);
        it_receiver *r = NULL;
        uint32_t i;
        if (c != NULL && !sender) r = owned_receiver_as(t, ch, c->type, it_str(step, "receiver"));
        if (c == NULL || (sender ? owned_sender(t, ch, c->type) == NULL : r == NULL)) {
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
        close_endpoint(c, r, (uint32_t)(t - g_tasks));
        observe_status(t, idx, label, ASX_OK);
        return STEP_NEXT;
    }
    if (strcmp(op, "seal") == 0) {
        /* rx.close() (mpsc.rs:1698): unlike close_receiver (the drop), the
         * receiver stays held and the queue receivable. asx_channel_seal
         * refuses only a second seal, which is outside the DSL (§3.6). */
        const char *ch = it_str(step, "channel");
        it_channel *c = channel_by_name(ch);
        if (c == NULL || owned_receiver(t, ch, IT_CH_MPSC) == NULL) {
            it_fail_task(t, idx, "seal of an mpsc receiver this task does not own");
            return STEP_END;
        }
        if (asx_channel_seal(c->id) != ASX_OK) {
            it_fail_task(t, idx, "a second seal (outside DSL §3.6)");
            return STEP_END;
        }
        observe_status(t, idx, label, ASX_OK);
        return STEP_NEXT;
    }
    /* Oneshot queries (DSL §3.6): tx.is_closed() / rx.is_closed()
     * (oneshot.rs:596, :1357) and rx.is_ready() (:1350), observed as a
     * boolean value. */
    if (strcmp(op, "oneshot_is_closed") == 0 || strcmp(op, "oneshot_is_ready") == 0) {
        const char *ch = it_str(step, "channel");
        int ready = strcmp(op, "oneshot_is_ready") == 0;
        const char *end = ready ? "receiver" : it_str(step, "end");
        int sender = end != NULL && strcmp(end, "sender") == 0;
        it_channel *c = sender ? owned_sender(t, ch, IT_CH_ONESHOT) : NULL;
        it_receiver *r = !sender && end != NULL && strcmp(end, "receiver") == 0
                             ? owned_receiver(t, ch, IT_CH_ONESHOT)
                             : NULL;
        int value;
        if (c == NULL && r == NULL) {
            it_fail_task(t, idx, "oneshot query on an end this task does not hold");
            return STEP_END;
        }
        value = c != NULL ? asx_oneshot_sender_is_closed(&c->oneshot)
                : ready   ? asx_oneshot_receiver_is_ready(&r->oneshot)
                          : asx_oneshot_receiver_is_closed(&r->oneshot);
        observe(t, idx, label, status_node(ASX_OK), asx_json_new_bool(g_out, value ? 1 : 0));
        return STEP_NEXT;
    }
    /* Watch receiver and sender operations that never suspend (DSL §3.6):
     * borrow, borrow_and_update, mark_seen, has_changed, is_closed, clone,
     * send_modify, receiver_count (channel/watch.rs:530, :634, :904-977,
     * :1057). A receiver step selects a clone with `receiver`. */
    if (strncmp(op, "watch_", 6) == 0) {
        step_result res;
        if (exec_watch_op(t, step, idx, label, op, &res)) return res;
    }
    /* Interval and DelayQueue steps (DSL §3.9). */
    {
        step_result res;
        if (exec_time_op(t, step, idx, label, op, &res)) return res;
    }
    /* The monitor steps that never suspend (DSL §3.10). */
    {
        step_result res;
        if (exec_monitor_op(t, self, step, idx, label, op, 0, &res)) return res;
    }
    /* Non-blocking oneshot, broadcast and watch sends (DSL §3.6). */
    if (strcmp(op, "oneshot_send") == 0 || strcmp(op, "broadcast_send") == 0 ||
        strcmp(op, "watch_send") == 0) {
        it_ch_type type = op[0] == 'o'   ? IT_CH_ONESHOT
                          : op[0] == 'b' ? IT_CH_BROADCAST
                                         : IT_CH_WATCH;
        it_channel *c = owned_sender(t, it_str(step, "channel"), type);
        uint64_t v = 0;
        if (c == NULL || !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &v)) {
            it_fail_task(t, idx, "send needs an owned sender and a value");
            return STEP_END;
        }
        if (type == IT_CH_ONESHOT) {
            st = asx_oneshot_send(&c->oneshot, &t->cx, v);
            /* Rust's send consumes the sender */
            c->senders[sender_slot(c, (uint32_t)(t - g_tasks))].open = 0;
        } else if (type == IT_CH_BROADCAST) {
            /* tx.send(&cx, v) (broadcast.rs:494) returns the receivers it
             * reached: the observation's value. */
            uint32_t reached = 0;
            int k = sender_slot(c, (uint32_t)(t - g_tasks));
            st = asx_broadcast_send(&c->senders[k].broadcast, &t->cx, v, &reached);
            observe(t, idx, label, status_node(st),
                    st == ASX_OK ? asx_json_new_u64(g_out, reached) : ASX_JSON_NONE);
            return STEP_NEXT;
        } else {
            /* Rust's watch send takes no Cx (watch.rs:488). */
            st = asx_watch_send(&c->watch, v);
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "try_send") == 0) {
        const char *ch = it_str(step, "channel");
        it_channel *c = owned_sender(t, ch, IT_CH_MPSC);
        asx_send_permit permit;
        uint64_t v = 0;
        if (c == NULL || !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &v)) {
            it_fail_task(t, idx, "try_send needs an owned sender and a value");
            return STEP_END;
        }
        /* tx.try_send(v) (mpsc.rs:773): never parks, no permit obligation. */
        st = asx_channel_try_reserve(c->id, &permit);
        if (st == ASX_OK) st = asx_send_permit_send(&permit, v);
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "send_evict_oldest") == 0) {
        it_channel *c = owned_sender(t, it_str(step, "channel"), IT_CH_MPSC);
        uint64_t v = 0;
        uint64_t oldest = 0;
        int evicted = 0;
        if (c == NULL || asx_channel_is_unbounded(c->id) ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &v)) {
            it_fail_task(t, idx, "send_evict_oldest needs an owned bounded sender and a value");
            return STEP_END;
        }
        /* tx.send_evict_oldest(v) (mpsc.rs:872): never parks; observes the evicted one, if any. */
        st = asx_channel_send_evict_oldest(c->id, v, &evicted, &oldest);
        observe(t, idx, label, status_node(st),
                st == ASX_OK && evicted ? asx_json_new_u64(g_out, oldest) : ASX_JSON_NONE);
        return STEP_NEXT;
    }
    if (strcmp(op, "try_recv") == 0) {
        const char *ch = it_str(step, "channel");
        it_channel *c = channel_by_name(ch);
        uint64_t v = 0;
        if (c == NULL || owned_receiver(t, ch, IT_CH_MPSC) == NULL) {
            it_fail_task(t, idx, "try_recv needs an owned receiver");
            return STEP_END;
        }
        /* rx.try_recv() (mpsc.rs:1956): never parks; empty is
         * RecvError::Empty. */
        st = asx_channel_try_recv(c->id, &v);
        if (st == ASX_E_WOULD_BLOCK) st = ASX_E_CHANNEL_EMPTY;
        observe(t, idx, label, status_node(st),
                st == ASX_OK ? asx_json_new_u64(g_out, v) : ASX_JSON_NONE);
        return STEP_NEXT;
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
                /* A region the task does not hold (its open failed, or it
                 * was closed): the spawn observes ASX_E_NOT_FOUND, spawns
                 * nothing, and so do the child's later steps (DSL §3). */
                uint32_t program = asx_json_get(g_in, step, "program");
                child = add_task(name, in_region, ASX_INVALID_ID, program);
                if (child == NULL) return STEP_END;
                child->refused = ASX_E_NOT_FOUND;
                observe_status(t, idx, label, ASX_E_NOT_FOUND);
                return STEP_NEXT;
            }
        }
        child = add_task(name, lr != NULL ? lr->name : t->region_name,
                         lr != NULL ? lr->id : t->region, asx_json_get(g_in, step, "program"));
        if (child == NULL) return STEP_END;
        /* A spawn into a held region is child.cx().spawn, which takes no
         * budget (twin_run ignores one there, run.rs:1059-1069). */
        st = asx_task_spawn_with_budget(child->region, interp_poll, child,
                                        has_budget && lr == NULL ? &budget : NULL, &id);
        if (st == ASX_OK) {
            child->id = id;
            child->spawned = 1;
            (void)asx_cx_init(&child->cx, child->region, id, ASX_CAP_NONE);
        } else {
            child->refused = st;
            child->refusal = asx_scheduler_last_spawn_refusal();
        }
        /* Kept: an admission that refuses the child later turns this into
         * ASX_E_ADMISSION_LIMIT (project_refused_admissions). */
        child->spawn_obs = observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "try_join") == 0) {
        it_task *target = task_by_name(it_str(step, "task"));
        /* A refused spawn has no task; joining it observes the refusal, as
         * Rust's join of a child admission refused does (DSL §3.4), but
         * only once the next step's admission refused it: until then
         * Rust's try_join reports the child not ready. */
        if (target != NULL && target->refused != ASX_OK) {
            observe_status(t, idx, label,
                           asx_task_refusal_delivered(target->refusal) ? target->refused
                                                                       : ASX_E_TASK_NOT_COMPLETED);
            return STEP_NEXT;
        }
        if (target == NULL || !target->spawned || target->joined) {
            it_fail_task(t, idx, "try_join of an unknown or already joined task");
            return STEP_END;
        }
        if (!task_completed(target->id)) {
            observe_status(t, idx, label, ASX_E_TASK_NOT_COMPLETED);
            return STEP_NEXT;
        }
        {
            asx_status why = target->admission_refusal != ASX_OK ? target->admission_refusal
                                                                 : admission_refusal(target->id);
            uint32_t value = take_outcome(target, &st);
            if (why != ASX_OK && st == ASX_OK) {
                target->admission_refusal = why;
                observe_status(t, idx, label, why);
            } else {
                observe(t, idx, label, status_node(st), value);
            }
        }
        return STEP_NEXT;
    }
    if (strcmp(op, "abort_task") == 0) {
        it_task *target = task_by_name(it_str(step, "task"));
        asx_cancel_kind kind;
        if (!cancel_kind_parse(it_str(step, "kind"), &kind)) {
            it_fail_task(t, idx, "abort_task with an unknown cancel kind");
            return STEP_END;
        }
        /* A refused child's handle never gets a task: the abort is only
         * cached (task_handle.rs:492-497). A child never spawned (its
         * region not held) has no handle at all (DSL §3). */
        if (target != NULL && target->refused != ASX_OK) {
            observe_status(t, idx, label,
                           target->refused == ASX_E_NOT_FOUND ? ASX_E_NOT_FOUND : ASX_OK);
            return STEP_NEXT;
        }
        if (target == NULL || !target->spawned) {
            it_fail_task(t, idx, "abort_task of an unknown task");
            return STEP_END;
        }
        {
            /* The requesting task initiates the cancel (DSL §4). A handle
             * abort is a command the scheduler applies at its next step. A
             * child its admission already refused keeps the refusal it
             * showed before the abort strengthened its reason, as twin_run
             * takes Rust's (`denied`). */
            asx_cancel_reason r = make_reason(kind, t->region, self, it_str(step, "message"));
            if (target->admission_refusal == ASX_OK && !target->joined) {
                target->admission_refusal = admission_refusal(target->id);
            }
            st = asx_task_abort_request(target->id, &r);
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "cancel_region") == 0) {
        it_local_region *lr = local_region(t, it_str(step, "region"));
        asx_cancel_kind kind;
        if (!cancel_kind_parse(it_str(step, "kind"), &kind)) {
            it_fail_task(t, idx, "cancel_region with an unknown cancel kind");
            return STEP_END;
        }
        if (lr == NULL) {
            /* Not held (DSL §3). */
            observe_status(t, idx, label, ASX_E_NOT_FOUND);
            return STEP_NEXT;
        }
        {
            /* ChildRegion::cancel: a command the scheduler applies at its
             * next step. */
            asx_cancel_reason r = make_reason(kind, t->region, self, it_str(step, "message"));
            st = asx_region_cancel_request(lr->id, &r);
        }
        observe_status(t, idx, label, st);
        return STEP_NEXT;
    }
    if (strcmp(op, "masked") == 0) {
        /* Cx::masked (cx.rs:3315). A masked step inside masked nests; each
         * inner observation's op is its path, e.g. masked/2/masked/1/checkpoint
         * (DSL §3.2). */
        it_masked_run run;
        run.task = t;
        run.steps = asx_json_get(g_in, step, "steps");
        run.index = idx;
        run.label = label;
        run.result = STEP_NEXT;
        if (asx_task_masked(self, exec_masked_steps, &run) != ASX_OK) {
            it_fail_task(t, idx, "asx_task_masked failed");
            return STEP_END;
        }
        return run.result;
    }
    {
        char what[160];
        (void)snprintf(what, sizeof(what), "op \"%s\" is not interpreted (or blocks inside masked)",
                       op);
        it_fail_task(t, idx, what);
    }
    return STEP_END;
}

/* ------------------------------------------------------------------ */
/* Actors (DSL §3.8)                                                   */
/* ------------------------------------------------------------------ */

/* The built-in behaviors, twin_run's CounterServer and EchoServer. */
static asx_status counter_cast(void *state, uint64_t msg, asx_actor_handle self) {
    uint64_t *sum = (uint64_t *)state;
    (void)self;
    *sum += msg; /* wrapping, as Rust's wrapping_add */
    return ASX_OK;
}

/* ASX_ANALYZER_WAIVER("callback signature: a const state would need a function-pointer cast") */
static asx_status counter_call(void *state, uint64_t request, uint64_t *reply,
                               asx_actor_handle self) {
    (void)request;
    (void)self;
    *reply = *(const uint64_t *)state;
    return ASX_OK;
}

static asx_status echo_call(void *state, uint64_t request, uint64_t *reply, asx_actor_handle self) {
    (void)state;
    (void)self;
    *reply = request;
    return ASX_OK;
}

/* `budgets`: a counter whose start and stop hooks trace the task budget
 * they run under (twin_run's probe_trace, the same text). Its init phase
 * is 7 polls at priority 200; the stop phase is the default
 * (Budget::MINIMAL) for `budgets` and 42 polls at priority 250 for
 * `budgets_stop`. */
static void budget_probe_trace(asx_actor_handle self, const char *hook) {
    asx_task_id task = ASX_INVALID_ID;
    asx_budget budget;
    char message[64];
    if (asx_actor_task_id(self, &task) != ASX_OK || asx_task_get_budget(task, &budget) != ASX_OK) {
        it_fail("budget probe: the server's task budget is unreadable", hook);
        return;
    }
    (void)snprintf(message, sizeof(message), "probe:%s polls=%lu priority=%u", hook,
                   (unsigned long)budget.poll_quota, (unsigned)budget.priority);
    asx_trace_user(task, message);
}

static asx_status probe_init(void *state, asx_actor_handle self) {
    (void)state;
    budget_probe_trace(self, "start");
    return ASX_OK;
}

static void probe_terminate(void *state, asx_status reason, asx_actor_handle self) {
    (void)state;
    (void)reason;
    budget_probe_trace(self, "stop");
}

static asx_budget probe_start_budget(void *state) {
    asx_budget budget = asx_budget_from_polls(7u);
    (void)state;
    budget.priority = 200u;
    return budget;
}

static asx_budget probe_stop_budget(void *state) {
    asx_budget budget = asx_budget_from_polls(42u);
    (void)state;
    budget.priority = 250u;
    return budget;
}

static const asx_actor_behavior g_counter_behavior = {
    NULL, counter_cast, counter_call, NULL, NULL, NULL, ASX_ACTOR_OVERFLOW_REJECT, server_info};
/* `latest`: a counter that drops its oldest queued cast when full. */
static const asx_actor_behavior g_latest_behavior = {
    NULL,       counter_cast, counter_call, NULL, NULL, NULL, ASX_ACTOR_OVERFLOW_DROP_OLDEST,
    server_info};
static const asx_actor_behavior g_budgets_behavior = {probe_init,
                                                      counter_cast,
                                                      counter_call,
                                                      probe_terminate,
                                                      probe_start_budget,
                                                      NULL,
                                                      ASX_ACTOR_OVERFLOW_REJECT,
                                                      server_info};
static const asx_actor_behavior g_budgets_stop_behavior = {probe_init,
                                                           counter_cast,
                                                           counter_call,
                                                           probe_terminate,
                                                           probe_start_budget,
                                                           probe_stop_budget,
                                                           ASX_ACTOR_OVERFLOW_REJECT,
                                                           server_info};
static const asx_actor_behavior g_echo_behavior = {
    NULL, NULL, echo_call, NULL, NULL, NULL, ASX_ACTOR_OVERFLOW_REJECT, server_info};

static it_server *server_by_name(const char *name) {
    uint32_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < g_n_servers; i++) {
        if (strcmp(g_servers[i].name, name) == 0) return &g_servers[i];
    }
    return NULL;
}

/* server_spawn, cast, try_cast, call, server_evicted and server_stop.
 * Returns 0 for other ops. */
static int exec_server(it_task *t, uint32_t step, uint32_t idx, const char *op, step_result *out) {
    asx_status st;
    it_server *srv;
    asx_task_id id;

    *out = STEP_NEXT;
    if (strcmp(op, "server_spawn") == 0) {
        const char *name = it_str(step, "as");
        const char *behavior = it_str(step, "behavior");
        const asx_actor_behavior *b = NULL;
        uint64_t mailbox = 0;
        if (behavior != NULL && strcmp(behavior, "counter") == 0) b = &g_counter_behavior;
        if (behavior != NULL && strcmp(behavior, "latest") == 0) b = &g_latest_behavior;
        if (behavior != NULL && strcmp(behavior, "budgets") == 0) b = &g_budgets_behavior;
        if (behavior != NULL && strcmp(behavior, "budgets_stop") == 0) {
            b = &g_budgets_stop_behavior;
        }
        if (behavior != NULL && strcmp(behavior, "echo") == 0) b = &g_echo_behavior;
        if (name == NULL || b == NULL ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "mailbox"), &mailbox) ||
            mailbox > UINT32_MAX || server_by_name(name) != NULL || g_n_servers >= IT_MAX_SERVERS) {
            it_fail_task(t, idx, "server_spawn needs a new `as`, a known behavior and a mailbox");
            *out = STEP_END;
            return 1;
        }
        srv = &g_servers[g_n_servers];
        memset(srv, 0, sizeof(*srv));
        srv->name = name;
        srv->task = IT_NO_SERVER_TASK;
        st = asx_actor_spawn(&srv->handle, t->region, b, &srv->value, (uint32_t)mailbox);
        if (st == ASX_OK) g_n_servers++;
        if (st == ASX_OK && asx_actor_task_id(srv->handle, &id) == ASX_OK) {
            /* The server's task, named by `as` (vocabulary §2). A server
             * whose spawn the lab refused has none, as Rust never admits
             * its task. */
            it_task *task;
            if (g_n_tasks >= IT_MAX_TASKS || task_by_name(name) != NULL) {
                it_fail("too many tasks, or a server named like a task", name);
                *out = STEP_END;
                return 1;
            }
            task = &g_tasks[g_n_tasks];
            memset(task, 0, sizeof(*task));
            task->name = name;
            task->region_name = t->region_name;
            task->region = t->region;
            task->program = ASX_JSON_NONE;
            task->end = ASX_OK;
            task->spawn_obs = ASX_JSON_NONE;
            task->id = id;
            task->spawned = 1;
            task->server = 1;
            srv->task = g_n_tasks++;
        }
        observe_status(t, idx, op, st);
        return 1;
    }
    if (strcmp(op, "cast") != 0 && strcmp(op, "call") != 0 && strcmp(op, "server_stop") != 0 &&
        strcmp(op, "try_cast") != 0 && strcmp(op, "server_evicted") != 0) {
        return 0;
    }
    srv = server_by_name(it_str(step, "server"));
    if (srv == NULL || srv->released) {
        it_fail_task(t, idx, "unknown or stopped server");
        *out = STEP_END;
        return 1;
    }
    if (strcmp(op, "try_cast") == 0) {
        /* h.try_cast(msg) (gen_server.rs:1305): never waits and never
         * checks the task's cancellation. */
        uint64_t value = 0;
        if (!asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &value)) {
            it_fail_task(t, idx, "try_cast without a value");
            *out = STEP_END;
            return 1;
        }
        observe_status(t, idx, op, asx_actor_try_cast(srv->handle, value));
        return 1;
    }
    if (strcmp(op, "server_evicted") == 0) {
        /* h.evicted_count() (:1153): the casts DropOldest evicted. */
        observe(t, idx, op, status_node(ASX_OK),
                asx_json_new_u64(g_out, asx_actor_evicted_count(srv->handle)));
        return 1;
    }
    if (t->phase == 0u) {
        memset(&t->actor_op, 0, sizeof(t->actor_op));
        t->phase = 1u;
    }
    if (strcmp(op, "cast") == 0) {
        uint64_t value = 0;
        if (!asx_json_u64(g_in, asx_json_get(g_in, step, "value"), &value)) {
            it_fail_task(t, idx, "cast without a value");
            *out = STEP_END;
            return 1;
        }
        st = asx_actor_cast(srv->handle, &t->cx, value, &t->actor_op);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        observe_status(t, idx, op, st);
        return 1;
    }
    if (strcmp(op, "call") == 0) {
        uint64_t request = 0;
        uint64_t reply = 0;
        if (!asx_json_u64(g_in, asx_json_get(g_in, step, "request"), &request)) {
            it_fail_task(t, idx, "call without a request");
            *out = STEP_END;
            return 1;
        }
        if (strcmp(t->region_name, "root") == 0) {
            /* A scenario error in both interpreters (DSL §3.8). */
            it_fail_task(t, idx, "call from a root-region task");
            *out = STEP_END;
            return 1;
        }
        st = asx_actor_call(srv->handle, &t->cx, request, &t->actor_op, &reply);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        observe(t, idx, op, status_node(st),
                st == ASX_OK ? asx_json_new_u64(g_out, reply) : ASX_JSON_NONE);
        return 1;
    }
    /* server_stop: h.stop(), then h.join(&cx). */
    if (t->phase == 1u) {
        (void)asx_actor_stop(srv->handle);
        t->phase = 2u;
    }
    st = asx_actor_join(srv->handle, &t->cx);
    if (st == ASX_E_PENDING) {
        *out = STEP_PENDING;
        return 1;
    }
    srv->released = 1;
    if (srv->task != IT_NO_SERVER_TASK) {
        /* The join retires the server's task. One its admission refused
         * never ran and stays out of the snapshot, as in Rust. */
        it_task *task = &g_tasks[srv->task];
        uint32_t value = server_outcome_node(task->id);
        asx_outcome ignored;
        task->admission_refusal = admission_refusal(task->id);
        if (asx_task_join(task->id, &ignored) != ASX_OK) {
            it_fail_task(t, idx, "server task outcome not joinable");
        } else {
            task->joined = 1;
            task->outcome = value;
        }
    }
    observe_status(t, idx, op, st);
    return 1;
}

static const char *supervisor_error_name(asx_supervisor_error e) {
    switch (e) {
    case ASX_SUPERVISOR_ERR_REGION: return "Region";
    case ASX_SUPERVISOR_ERR_SPAWN: return "Spawn";
    case ASX_SUPERVISOR_ERR_CHILD_NOT_STARTED: return "ChildNotStarted";
    case ASX_SUPERVISOR_ERR_DEPENDENCY_UNAVAILABLE: return "DependencyUnavailable";
    case ASX_SUPERVISOR_ERR_RESTART_LIMIT: return "RestartLimit";
    case ASX_SUPERVISOR_ERR_GENERATION_EXHAUSTED: return "GenerationExhausted";
    case ASX_SUPERVISOR_ERR_ESCALATION: return "Escalation";
    case ASX_SUPERVISOR_ERR_NONE: break;
    }
    return NULL;
}

/* A cancelled outcome's reason, captured now: its cause chain is
 * borrowed. */
static uint32_t cancelled_node(const asx_cancel_reason *r) {
    uint32_t o = asx_json_new_object(g_out);
    it_reason captured;
    capture_reason(r, &captured);
    asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "cancelled"));
    asx_json_set(g_out, o, "reason", reason_node(&captured));
    return o;
}

static uint32_t panicked_node(const char *message) {
    uint32_t o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "panicked"));
    asx_json_set(g_out, o, "message",
                 asx_json_new_string(g_out, message != NULL ? message : "unknown panic"));
    return o;
}

/* A supervisor_join's report (DSL §3.8): its outcome, its counters and
 * each child's latest ended generation, in start order. */
static uint32_t report_node(const it_supervisor *sv, const asx_supervisor_report *r) {
    uint32_t o = asx_json_new_object(g_out);
    uint32_t outcome = ASX_JSON_NONE;
    uint32_t children = asx_json_new_array(g_out);
    uint32_t i;
    switch (r->outcome) {
    case ASX_OUTCOME_OK:
        outcome = asx_json_new_object(g_out);
        asx_json_set(g_out, outcome, "tag", asx_json_new_string(g_out, "ok"));
        break;
    case ASX_OUTCOME_ERR: {
        const char *kind = supervisor_error_name(r->error);
        if (kind == NULL) {
            it_fail("supervisor error without a kind", sv->name);
            return o;
        }
        outcome = asx_json_new_object(g_out);
        asx_json_set(g_out, outcome, "tag", asx_json_new_string(g_out, "err"));
        asx_json_set(g_out, outcome, "error", asx_json_new_string(g_out, kind));
        asx_json_set(g_out, outcome, "child",
                     r->error_child < ASX_SUPERVISOR_MAX_CHILDREN
                         ? asx_json_new_string(g_out, sv->children[r->error_child].name)
                         : asx_json_new_null(g_out));
        break;
    }
    case ASX_OUTCOME_CANCELLED: outcome = cancelled_node(&r->cancel_reason); break;
    case ASX_OUTCOME_PANICKED: outcome = panicked_node(r->panic_message); break;
    }
    for (i = 0; i < r->completion_count; i++) {
        const asx_supervisor_completion *c = &r->completions[i];
        uint32_t entry = asx_json_new_object(g_out);
        uint32_t co = ASX_JSON_NONE;
        switch (c->outcome) {
        case ASX_OUTCOME_OK:
            co = asx_json_new_object(g_out);
            asx_json_set(g_out, co, "tag", asx_json_new_string(g_out, "ok"));
            break;
        case ASX_OUTCOME_ERR:
            co = asx_json_new_object(g_out);
            asx_json_set(g_out, co, "tag", asx_json_new_string(g_out, "err"));
            asx_json_set(g_out, co, "status", status_node(c->status));
            break;
        case ASX_OUTCOME_CANCELLED: co = cancelled_node(&c->cancel_reason); break;
        case ASX_OUTCOME_PANICKED: co = panicked_node(c->panic_message); break;
        }
        asx_json_set(g_out, entry, "child",
                     asx_json_new_string(g_out, sv->children[c->child].name));
        asx_json_set(g_out, entry, "generation", asx_json_new_u64(g_out, c->generation.number));
        asx_json_set(g_out, entry, "outcome", co);
        asx_json_push(g_out, children, entry);
    }
    asx_json_set(g_out, o, "outcome", outcome);
    asx_json_set(g_out, o, "started", asx_json_new_u64(g_out, r->started));
    asx_json_set(g_out, o, "joined", asx_json_new_u64(g_out, r->joined));
    asx_json_set(g_out, o, "restart_batches", asx_json_new_u64(g_out, r->restart_batches));
    asx_json_set(g_out, o, "escalations", asx_json_new_u64(g_out, r->escalations));
    asx_json_set(g_out, o, "children", children);
    return o;
}

/* A completed controller's outcome is its join's (vocabulary §6): ok once
 * it published its report, else its task's (cancelled before it ran). */
static uint32_t supervisor_outcome_node(const it_supervisor *sv) {
    uint32_t o;
    if (!asx_supervisor_finished(sv->handle)) return outcome_node(g_tasks[sv->task].id);
    o = asx_json_new_object(g_out);
    asx_json_set(g_out, o, "tag", asx_json_new_string(g_out, "ok"));
    return o;
}

/* supervise and supervisor_join (DSL §3.8). Returns 0 for other ops. */
static int exec_supervisor(it_task *t, asx_task_id self, uint32_t step, uint32_t idx,
                           const char *op, step_result *out) {
    asx_status st;
    it_supervisor *sv;

    *out = STEP_NEXT;
    if (strcmp(op, "supervise") == 0) {
        const char *name = it_str(step, "as");
        const char *policy = it_str(step, "policy");
        const char *escalation = it_str(step, "escalation");
        uint32_t list = asx_json_get(g_in, step, "children");
        uint32_t n = asx_json_count(g_in, list);
        uint64_t max_restarts = 0;
        uint64_t window = 0;
        asx_supervisor_config cfg;
        asx_child_spec specs[ASX_SUPERVISOR_MAX_CHILDREN];
        asx_task_id id;
        uint32_t i;
        if (name == NULL || policy == NULL || sup_by_name(name) != NULL ||
            g_n_sups >= IT_MAX_SUPERVISORS || n == 0u || n > ASX_SUPERVISOR_MAX_CHILDREN ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "max_restarts"), &max_restarts) ||
            max_restarts > UINT32_MAX ||
            !asx_json_u64(g_in, asx_json_get(g_in, step, "window_ns"), &window)) {
            it_fail_task(t, idx, "supervise needs a new `as`, a policy, limits and children");
            *out = STEP_END;
            return 1;
        }
        sv = &g_sups[g_n_sups];
        memset(sv, 0, sizeof(*sv));
        sv->name = name;
        sv->owner = (uint32_t)(t - g_tasks);
        sv->task = IT_NO_SERVER_TASK;
        asx_supervisor_config_init(&cfg, name, (uint32_t)max_restarts, window);
        if (strcmp(policy, "one_for_one") == 0) {
            cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ONE;
        } else if (strcmp(policy, "one_for_all") == 0) {
            cfg.strategy = ASX_SUPERVISOR_ONE_FOR_ALL;
        } else if (strcmp(policy, "rest_for_one") == 0) {
            cfg.strategy = ASX_SUPERVISOR_REST_FOR_ONE;
        } else {
            it_fail_task(t, idx, "unknown supervise policy");
            *out = STEP_END;
            return 1;
        }
        if (escalation == NULL || strcmp(escalation, "stop") == 0) {
            cfg.escalation = ASX_ESCALATION_STOP;
        } else if (strcmp(escalation, "escalate") == 0) {
            cfg.escalation = ASX_ESCALATION_ESCALATE;
        } else {
            it_fail_task(t, idx, "unknown supervise escalation");
            *out = STEP_END;
            return 1;
        }
        for (i = 0; i < n; i++) {
            uint32_t c = asx_json_item(g_in, list, i);
            const char *mode = it_str(c, "mode");
            asx_child_restart restart = ASX_CHILD_PERMANENT;
            uint32_t program = asx_json_get(g_in, c, "program");
            if (mode != NULL && strcmp(mode, "transient") == 0) restart = ASX_CHILD_TRANSIENT;
            if (mode != NULL && strcmp(mode, "temporary") == 0) restart = ASX_CHILD_TEMPORARY;
            if (it_str(c, "name") == NULL || mode == NULL ||
                (restart == ASX_CHILD_PERMANENT && strcmp(mode, "permanent") != 0) ||
                asx_json_type_of(g_in, program) != ASX_JSON_ARRAY) {
                it_fail_task(t, idx, "a supervised child needs a name, a mode and a program");
                *out = STEP_END;
                return 1;
            }
            sv->children[i].name = it_str(c, "name");
            sv->children[i].program = program;
            sv->children[i].sup = g_n_sups;
            asx_child_spec_init(&specs[i], sv->children[i].name, restart, it_start_generation,
                                &sv->children[i]);
        }
        st = asx_supervisor_spawn(&sv->handle, t->region, &cfg, specs, n);
        if (st != ASX_OK) {
            /* twin_run fails the run on a compile or spawn error too. */
            it_fail_task(t, idx, "supervise could not compile or spawn the supervisor");
            *out = STEP_END;
            return 1;
        }
        g_n_sups++;
        id = asx_supervisor_task(sv->handle);
        if (id != ASX_INVALID_ID) {
            /* The controller's task, named by `as` (vocabulary §2). One
             * whose spawn the lab refused has none, as Rust never admits
             * its task. */
            it_task *task;
            if (g_n_tasks >= IT_MAX_TASKS || task_by_name(name) != NULL) {
                it_fail("too many tasks, or a supervisor named like a task", name);
                *out = STEP_END;
                return 1;
            }
            task = &g_tasks[g_n_tasks];
            memset(task, 0, sizeof(*task));
            task->name = name;
            task->region_name = t->region_name;
            task->region = t->region;
            task->program = ASX_JSON_NONE;
            task->end = ASX_OK;
            task->spawn_obs = ASX_JSON_NONE;
            task->id = id;
            task->spawned = 1;
            task->supervisor = 1;
            sv->task = g_n_tasks++;
        }
        observe_status(t, idx, op, ASX_OK);
        return 1;
    }
    if (strcmp(op, "supervisor_join") == 0) {
        asx_supervisor_report report;
        uint32_t outcome = ASX_JSON_NONE;
        it_task *ctl;
        asx_task_state s;
        sv = sup_by_name(it_str(step, "supervisor"));
        if (sv == NULL || sv->joined || sv->owner != (uint32_t)(t - g_tasks)) {
            it_fail_task(t, idx, "unknown or already joined supervisor");
            *out = STEP_END;
            return 1;
        }
        ctl = sv->task != IT_NO_SERVER_TASK ? &g_tasks[sv->task] : NULL;
        /* The controller's outcome, read before the join retires its task:
         * its join's (vocabulary §6). */
        if (ctl != NULL && asx_task_get_state(ctl->id, &s) == ASX_OK && s == ASX_TASK_COMPLETED) {
            ctl->admission_refusal = admission_refusal(ctl->id);
            outcome = supervisor_outcome_node(sv);
        }
        st = asx_supervisor_join(sv->handle, self, &report);
        if (st == ASX_E_PENDING) {
            *out = STEP_PENDING;
            return 1;
        }
        sv->joined = 1;
        if (ctl != NULL) {
            if (outcome == ASX_JSON_NONE) {
                it_fail_task(t, idx, "supervisor joined before its controller completed");
                *out = STEP_END;
                return 1;
            }
            ctl->joined = 1;
            ctl->outcome = outcome;
        }
        if (st == ASX_OK) {
            observe(t, idx, op, status_node(st), report_node(sv, &report));
        } else {
            observe_status(t, idx, op, st);
        }
        return 1;
    }
    return 0;
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
        if (exec_group(t, self, step, idx, op, &r)) return r;
        if (exec_server(t, step, idx, op, &r)) return r;
        if (exec_monitor_op(t, self, step, idx, op, op, 1, &r)) return r;
        if (exec_supervisor(t, self, step, idx, op, &r)) return r;
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
            if (t->supervised && err == ASX_E_CANCELLED) {
                /* A generation's body ending ASX_E_CANCELLED is Outcome::
                 * Cancelled (supervisor.h), which `err` is not (DSL 3.8). */
                it_fail_task(t, idx,
                             "a supervised child returns Cancelled with tag cancelled, not err");
                return STEP_END;
            }
            t->end = err;
        } else if (strcmp(tag, "cancelled") == 0) {
            /* The factory future's Outcome::Cancelled(reason): only a
             * supervised generation's body has one (DSL 3.8). The body ends
             * ASX_E_CANCELLED and the supervisor builds the reason. */
            if (!t->supervised) {
                it_fail_task(t, idx, "return cancelled outside a supervised child's program");
                return STEP_END;
            }
            t->end = ASX_E_CANCELLED;
        } else if (strcmp(tag, "panicked") == 0) {
            /* Rust reports a finalizer future's panic as its task's panic,
             * which twin_run does not catch (DSL §1, Region finalizers). */
            if (t->finalizer) {
                it_fail_task(t, idx, "a finalizer's program cannot return panicked");
                return STEP_END;
            }
            /* The step is a panic: the observation is recorded first, as in
             * Rust where the step's observation precedes panic_any. The
             * locals then drop while panicking (drop_locals). */
            observe_status(t, idx, op, ASX_OK);
            t->panicking = 1;
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
    if (strcmp(op, "join") == 0) {
        it_task *target = task_by_name(it_str(step, "task"));
        asx_outcome ignored;
        if (target != NULL && target->refused != ASX_OK) {
            /* Rust's join of a child its region refused resolves when the
             * next step's admission refuses it. */
            st = asx_task_await_refusal(self, target->refusal);
            if (st == ASX_E_PENDING) return STEP_PENDING;
            if (st != ASX_OK) {
                it_fail_task(t, idx, "asx_task_await_refusal failed");
                return STEP_END;
            }
            observe_status(t, idx, op, target->refused);
            return STEP_NEXT;
        }
        if (target == NULL || !target->spawned || target->joined) {
            it_fail_task(t, idx, "join of an unknown or already joined task");
            return STEP_END;
        }
        if (task_completed(target->id)) {
            /* A child its admission refused: the join observes the
             * refusal, as twin_run projects Rust's (DSL §3.4). */
            asx_status why = target->admission_refusal != ASX_OK ? target->admission_refusal
                                                                 : admission_refusal(target->id);
            uint32_t value = take_outcome(target, &st);
            if (why != ASX_OK && st == ASX_OK) {
                target->admission_refusal = why;
                observe_status(t, idx, op, why);
                return STEP_NEXT;
            }
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
        /* Cx::open_child_region: a command the scheduler applies at its
         * next step; the task waits parked for the result. */
        st = asx_region_open_child_poll(self, t->region, has_budget ? &budget : NULL, &id);
        if (st == ASX_E_PENDING) return STEP_PENDING;
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
        if (t->phase == 0u) {
            it_local_region *lr = local_region(t, it_str(step, "region"));
            if (lr == NULL) {
                /* Not held (DSL §3). */
                observe_status(t, idx, op, ASX_E_NOT_FOUND);
                return STEP_NEXT;
            }
            t->closing = lr->id;
            *lr = t->regions[--t->n_regions];
            t->phase = 1u;
        }
        /* ChildRegion::close: a Close command (the region is cancelled with
         * Rust's "owned child region body finished" User reason, its
         * remaining tasks cancelled), then a parked wait for CLOSED. */
        st = asx_region_close_poll(self, t->closing);
        if (st == ASX_E_PENDING) return STEP_PENDING;
        observe_status(t, idx, op, st);
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

/* A region finalizer's poll function (DSL §1 "Region finalizers"). The
 * runtime spawns the finalizer's task when its region finalizes, so its
 * entry learns the task's identity at its first poll. */
static asx_status finalizer_poll(void *user_data, asx_task_id self) {
    it_task *t = (it_task *)user_data;
    if (!t->spawned) {
        t->id = self;
        t->spawned = 1;
        (void)asx_cx_init(&t->cx, t->region, self, ASX_CAP_NONE);
    }
    return interp_poll(t, self);
}

/* The `finalizers` of the region declaration `r`, registered in array
 * order: Rust Scope::defer_async, C asx_region_defer_async. */
static void register_finalizers(uint32_t r, const char *region_name, asx_region_id id) {
    uint32_t list = asx_json_get(g_in, r, "finalizers");
    uint32_t k;
    for (k = 0; k < asx_json_count(g_in, list) && !g_failed; k++) {
        uint32_t f = asx_json_item(g_in, list, k);
        it_task *t = add_task(it_str(f, "name"), region_name, id, asx_json_get(g_in, f, "program"));
        if (t == NULL) return;
        t->finalizer = 1;
        if (asx_region_defer_async(id, finalizer_poll, t) != ASX_OK) {
            it_fail("cannot register finalizer", t->name);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

typedef asx_status (*it_run_fn)(asx_region_id region, asx_budget *budget);

/* The receiver owners of a channel declaration: `receiver` (mpsc,
 * oneshot) or `subscribers` (broadcast, watch), all top-level tasks.
 * Returns their count, 0 on a bad declaration. */
static uint32_t channel_receivers(uint32_t d, int subscribed, it_channel *c) {
    uint32_t k;
    uint32_t list = asx_json_get(g_in, d, "subscribers");
    uint32_t n = subscribed ? asx_json_count(g_in, list) : 1u;
    if (n == 0u || n > IT_MAX_SUBSCRIBERS) return 0;
    for (k = 0; k < n; k++) {
        it_task *owner =
            task_by_name(subscribed ? asx_json_string(g_in, asx_json_item(g_in, list, k))
                                    : it_str(d, "receiver"));
        if (owner == NULL) return 0;
        c->receivers[k].task = (uint32_t)(owner - g_tasks);
        c->receivers[k].open = 1;
    }
    return n;
}

/* An mpsc's or a broadcast's optional `senders`: more top-level tasks, each
 * given a clone of the sender at setup, in order (twin_run: tx.clone(); C:
 * asx_channel_sender_clone / asx_broadcast_sender_clone). Returns 0 on a
 * bad list. */
static int channel_extra_senders(uint32_t d, it_channel *c) {
    uint32_t list = asx_json_get(g_in, d, "senders");
    uint32_t k;
    uint32_t n = asx_json_count(g_in, list);
    if (n == 0u) return 1;
    if ((c->type != IT_CH_MPSC && c->type != IT_CH_BROADCAST) || 1u + n > IT_MAX_SENDERS) {
        return 0;
    }
    for (k = 0; k < n; k++) {
        it_task *owner = task_by_name(asx_json_string(g_in, asx_json_item(g_in, list, k)));
        asx_status st;
        if (owner == NULL || sender_slot(c, (uint32_t)(owner - g_tasks)) >= 0) return 0;
        st = c->type == IT_CH_MPSC
                 ? asx_channel_sender_clone(c->id, &c->senders[c->n_senders].mpsc)
                 : asx_broadcast_sender_clone(&c->senders[0].broadcast,
                                              &c->senders[c->n_senders].broadcast);
        if (st != ASX_OK) return 0;
        c->senders[c->n_senders].task = (uint32_t)(owner - g_tasks);
        c->senders[c->n_senders].open = 1;
        c->n_senders++;
    }
    return 1;
}

/* Channels (DSL §3.6), each endpoint bound to its owner, a top-level task.
 * Broadcast and watch subscribers after the first subscribe at setup,
 * before any send, as twin_run does. mpsc channels live in the root
 * region: Rust channels belong to none. */
static void setup_channels(uint32_t scenario, asx_region_id root) {
    uint32_t list = asx_json_get(g_in, scenario, "channels");
    uint32_t i;
    asx_channel_reset();
    asx_oneshot_reset();
    asx_broadcast_reset();
    asx_watch_reset();
    g_n_channels = 0;
    for (i = 0; i < asx_json_count(g_in, list); i++) {
        uint32_t d = asx_json_item(g_in, list, i);
        const char *name = it_str(d, "name");
        const char *type = it_str(d, "type");
        it_task *tx = task_by_name(it_str(d, "sender"));
        uint64_t n = 0;
        uint32_t k;
        int subscribed;
        it_channel *c;
        asx_status st = ASX_E_INVALID_ARGUMENT;
        if (g_n_channels >= IT_MAX_CHANNELS || name == NULL || type == NULL || tx == NULL ||
            channel_by_name(name) != NULL) {
            it_fail("channel declaration needs a fresh name, a type and a top-level sender", name);
            return;
        }
        c = &g_channels[g_n_channels++];
        memset(c, 0, sizeof(*c));
        c->name = name;
        c->senders[0].task = (uint32_t)(tx - g_tasks);
        c->senders[0].open = 1;
        c->n_senders = 1u;
        subscribed = strcmp(type, "broadcast") == 0 || strcmp(type, "watch") == 0;
        c->n_receivers = channel_receivers(d, subscribed, c);
        if (c->n_receivers == 0u) {
            it_fail("channel receivers must be declared top-level tasks", name);
            return;
        }
        if (strcmp(type, "mpsc") == 0) {
            int unbounded = 0;
            c->type = IT_CH_MPSC;
            if (asx_json_bool(g_in, asx_json_get(g_in, d, "unbounded"), &unbounded) && unbounded) {
                /* Rust's unbounded_channel (DSL 3.6). A profile whose overload catalog forbids
                 * unbounded queues refuses it with ASX_E_PERMISSION_DENIED (DEC-008); the
                 * scenario declares "requires" and the runner skips it there. */
                st = asx_channel_create_unbounded(root, &c->id);
            } else if (asx_json_u64(g_in, asx_json_get(g_in, d, "capacity"), &n) &&
                       n <= UINT32_MAX) {
                st = asx_channel_create(root, (uint32_t)n, &c->id);
            }
        } else if (strcmp(type, "oneshot") == 0) {
            c->type = IT_CH_ONESHOT;
            st = asx_oneshot_create(&c->oneshot, &c->receivers[0].oneshot);
        } else if (strcmp(type, "broadcast") == 0) {
            c->type = IT_CH_BROADCAST;
            if (asx_json_u64(g_in, asx_json_get(g_in, d, "capacity"), &n) && n <= UINT32_MAX) {
                st = asx_broadcast_create((uint32_t)n, &c->senders[0].broadcast,
                                          &c->receivers[0].broadcast);
            }
            for (k = 1; st == ASX_OK && k < c->n_receivers; k++) {
                st = asx_broadcast_subscribe(&c->senders[0].broadcast, &c->receivers[k].broadcast);
            }
        } else if (strcmp(type, "watch") == 0) {
            c->type = IT_CH_WATCH;
            if (asx_json_u64(g_in, asx_json_get(g_in, d, "initial"), &n)) {
                st = asx_watch_create(n, &c->watch, &c->receivers[0].watch);
            }
            for (k = 1; st == ASX_OK && k < c->n_receivers; k++) {
                st = asx_watch_subscribe(&c->watch, &c->receivers[k].watch);
            }
        }
        if (st == ASX_E_PERMISSION_DENIED) {
            it_fail("the profile forbids this channel (an unbounded queue, DEC-008)", name);
            return;
        }
        if (st != ASX_OK) {
            it_fail("cannot create channel (unknown type or bad parameters)", name);
            return;
        }
        if (!channel_extra_senders(d, c)) {
            it_fail("channel senders must be distinct top-level tasks of an mpsc or a broadcast",
                    name);
            return;
        }
    }
}

void asx_conformance_force_schedule(const asx_forced_schedule *schedule, const char *const *names) {
    g_forced = schedule;
    g_forced_names = names;
}

/* A recorded task id to the scenario task that plays it: the name the
 * fixture's dispatch list gives the dispatch it first appears in. A task
 * with no scenario name yet (or none: a supervisor's generation, a region
 * finalizer) is declined with ASX_E_NOT_FOUND, and the lab then resolves
 * it through its Rust arena shadow. */
static asx_status forced_resolve(void *ctx, uint64_t compact_task, asx_task_id *out) {
    uint32_t i;
    (void)ctx;
    if (g_forced == NULL || g_forced_names == NULL) return ASX_E_NOT_FOUND;
    for (i = 0; i < g_forced->dispatch_count; i++) {
        const it_task *t;
        if (g_forced->dispatches[i].task != compact_task) continue;
        t = task_by_name(g_forced_names[i]);
        if (t == NULL || !t->spawned) return ASX_E_NOT_FOUND;
        *out = t->id;
        return ASX_OK;
    }
    return ASX_E_NOT_FOUND;
}

/* Fail the run with what a replay diverged on. */
static void forced_report(const asx_forced_error *ferr) {
    char buf[192];
    (void)snprintf(buf, sizeof(buf), "%s at step %llu (dispatch %u): expected %llu, got %llu",
                   asx_forced_error_kind_name(ferr->kind), (unsigned long long)ferr->step,
                   (unsigned)ferr->index, (unsigned long long)ferr->expected,
                   (unsigned long long)ferr->actual);
    it_fail("forced schedule diverged", buf);
}

/* Take a replay off the lab after a scheduler run stopped on it. */
static void forced_fail(void) {
    asx_forced_error ferr;
    memset(&ferr, 0, sizeof(ferr));
    g_forced_on = 0;
    if (asx_lab_forced_schedule_end(NULL, &ferr) == ASX_OK) {
        it_fail("scheduler stopped on a replay mismatch the lab did not record", NULL);
        return;
    }
    forced_report(&ferr);
}

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
        if (g_forced_on && st == ASX_E_REPLAY_MISMATCH) {
            forced_fail();
            return;
        }
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

/* A region_limits field: absent or null is ASX_REGION_UNLIMITED. */
static int parse_region_limit(uint32_t op, const char *field, uint32_t *out) {
    uint32_t node = asx_json_get(g_in, op, field);
    uint64_t v;
    if (node == ASX_JSON_NONE || asx_json_is_null(g_in, node)) {
        *out = ASX_REGION_UNLIMITED;
        return 1;
    }
    if (!asx_json_u64(g_in, node, &v) || v >= ASX_REGION_UNLIMITED) return 0;
    *out = (uint32_t)v;
    return 1;
}

static void apply_driver_op(uint32_t op) {
    const char *name = it_str(op, "op");
    asx_cancel_kind kind;
    if (name == NULL) {
        it_fail("script entry without op", NULL);
        return;
    }
    if (strcmp(name, "finish_region") == 0) {
        it_region *r = region_by_name(it_str(op, "region"));
        const char *body_name = it_str(op, "body");
        asx_outcome body;
        asx_cancel_reason reason;
        const asx_cancel_reason *cancel = NULL;
        if (r == NULL || body_name == NULL) {
            it_fail("finish_region needs a known region and body outcome", NULL);
            return;
        }
        if (strcmp(body_name, "ok") == 0) {
            body = asx_outcome_make(ASX_OUTCOME_OK);
        } else if (strcmp(body_name, "err") == 0) {
            body = asx_outcome_make(ASX_OUTCOME_ERR);
        } else if (strcmp(body_name, "panicked") == 0) {
            body = asx_outcome_make(ASX_OUTCOME_PANICKED);
        } else if (strcmp(body_name, "cancelled") == 0 &&
                   cancel_kind_parse(it_str(op, "kind"), &kind)) {
            body = asx_outcome_make(ASX_OUTCOME_CANCELLED);
            reason = make_reason(kind, r->id, ASX_INVALID_ID, it_str(op, "message"));
            cancel = &reason;
        } else {
            it_fail("finish_region has an invalid body outcome or cancel kind", r->name);
            return;
        }
        if (asx_region_close_with_outcome(r->id, &body, cancel) != ASX_OK) {
            it_fail("asx_region_close_with_outcome failed", r->name);
            return;
        }
        /* Scope advances once after the body branch. Subsequent lab steps
         * complete live work; a successful body never drains/cancels it. */
        asx_region_advance_internal(r->id);
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
    if (strcmp(name, "region_limits") == 0) {
        /* Rust set_region_limits: the limits replace the region's; an
         * omitted or null field is unlimited (DSL §4). */
        it_region *r = region_by_name(it_str(op, "region"));
        asx_region_limits limits;
        asx_status st;
        if (r == NULL || !parse_region_limit(op, "max_tasks", &limits.max_tasks) ||
            !parse_region_limit(op, "max_children", &limits.max_children) ||
            !parse_region_limit(op, "max_obligations", &limits.max_obligations)) {
            it_fail("region_limits needs a known region and counts", NULL);
            return;
        }
        /* A region already gone has nothing left to limit (DSL §4). */
        st = asx_region_set_limits(r->id, &limits);
        if (st != ASX_OK && st != ASX_E_STALE_HANDLE && st != ASX_E_NOT_FOUND) {
            it_fail("asx_region_set_limits failed", r->name);
        }
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

/* The lab's dispatches in order, one "<step>@<ns> <task> <lane>" string
 * each, as twin_run writes Rust's forced-schedule dispatches (bd-9kll.4.8). */
static uint32_t project_dispatches(void) {
    uint32_t list = asx_json_new_array(g_out);
    uint32_t n = asx_scheduler_dispatches_recorded();
    uint32_t i;
    char buf[192];
    if (n > IT_MAX_DISPATCHES) {
        it_fail("more lab dispatches than the recorder holds", NULL);
        return list;
    }
    for (i = 0; i < n && !g_failed; i++) {
        const asx_dispatch_record *d = &g_dispatches[i];
        (void)snprintf(buf, sizeof(buf), "%llu@%llu %s %s", (unsigned long long)d->step,
                       (unsigned long long)d->at, task_name_of(d->task),
                       d->lane == ASX_DISPATCH_LANE_CANCEL ? "cancel" : "ready");
        asx_json_push(g_out, list, asx_json_new_string(g_out, buf));
    }
    return list;
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
            if (e->kind != ASX_TRACE_OBLIGATION_RESERVE) {
                /* Rust's held duration (docs/CANONICAL_VOCABULARY_V2.md section 3) */
                asx_json_set(g_out, ev, "duration_ns",
                             asx_json_new_u64(g_out, e->obligation.held_ns));
            }
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
        case ASX_TRACE_OBLIGATION_HANDOFF: {
            const it_task *from = task_by_id(e->handoff.from_task);
            const it_task *to = task_by_id(e->handoff.to_task);
            const char *kind = obligation_kind_name(e->obligation.kind);
            const char *name;
            uint32_t record;
            if (from == NULL || to == NULL || kind == NULL) {
                it_fail("handoff refers to unknown ownership", NULL);
                break;
            }
            name = obligation_name((asx_obligation_id)e->entity, from);
            ev = event_object("obligation.handoff");
            asx_json_set(g_out, ev, "obligation", asx_json_new_string(g_out, name));
            asx_json_set(g_out, ev, "from_task", asx_json_new_string(g_out, from->name));
            asx_json_set(g_out, ev, "to_task", asx_json_new_string(g_out, to->name));
            asx_json_set(g_out, ev, "from_region",
                         asx_json_new_string(g_out, region_name_of(e->handoff.from_region)));
            asx_json_set(g_out, ev, "to_region",
                         asx_json_new_string(g_out, region_name_of(e->handoff.to_region)));
            record = asx_json_new_object(g_out);
            asx_json_set(g_out, record, "state", asx_json_new_string(g_out, "Reserved"));
            asx_json_set(g_out, record, "kind", asx_json_new_string(g_out, kind));
            asx_json_set(g_out, record, "holder", asx_json_new_string(g_out, to->name));
            asx_json_set(g_out, record, "region",
                         asx_json_new_string(g_out, region_name_of(e->handoff.to_region)));
            asx_json_set(g_out, record, "abort_reason", asx_json_new_null(g_out));
            asx_json_set(g_out, obligations, name, record);
            break;
        }
        case ASX_TRACE_USER: {
            it_sup_trace p;
            ev = event_object("user.trace");
            if (parse_sup_trace(&g_text[e->text_off], &p)) {
                /* Its raw ids replaced by their names (vocabulary §3). */
                char buf[384];
                (void)snprintf(buf, sizeof(buf),
                               "managed_supervisor_v1 action=%s supervisor=\"%s\" child=\"%s\" "
                               "generation=%llu region=%s task=%s outcome=%s",
                               p.action, p.supervisor, p.child, p.generation,
                               region_name_of((asx_region_id)p.region),
                               task_name_of((asx_task_id)p.task), p.outcome);
                asx_json_set(g_out, ev, "message", asx_json_new_string(g_out, buf));
            } else {
                asx_json_set(g_out, ev, "message",
                             asx_json_new_string_len(g_out, &g_text[e->text_off], e->text_len));
            }
            break;
        }
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
                /* A re-registration of the same sleep (a new waker) keeps
                 * its name, as twin_run's NamedSleep does. */
                if (!e->rearm || t->timers == 0u) {
                    char buf[160];
                    t->timers++;
                    (void)snprintf(buf, sizeof(buf), "%s/tm%u", t->name, (unsigned)t->timers);
                    t->timer_name_off = text_store(buf, strlen(buf));
                }
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

/* Children their admission refused at max_tasks after the spawn step
 * observed ASX_OK (lab dispatch): the spawn observation becomes the
 * refusal, as twin_run's project_denied_spawns projects Rust's
 * RegionAtCapacity onto the spawn step; their joins already observed it. */
static void project_refused_admissions(void) {
    uint32_t i;
    for (i = 0; i < g_n_tasks; i++) {
        it_task *t = &g_tasks[i];
        if (!t->spawned) continue;
        if (!t->joined && t->admission_refusal == ASX_OK) {
            t->admission_refusal = admission_refusal(t->id);
        }
        if (t->admission_refusal == ASX_OK || t->spawn_obs == ASX_JSON_NONE) continue;
        asx_json_set(g_out, t->spawn_obs, "status", status_node(t->admission_refusal));
        asx_json_set(g_out, t->spawn_obs, "value", asx_json_new_null(g_out));
    }
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
        int in_cancel = 0;
        /* A refused spawn has no task; one its admission refused never ran
         * and is left out as Rust leaves out a handle never admitted. */
        if (!t->spawned || t->admission_refusal != ASX_OK) continue;
        if (t->group_member) {
            /* Reported through its group step only (vocabulary §6). */
            if (!t->member_done) quiescent = 0;
            continue;
        }
        if (t->supervised) {
            /* Reported through its supervisor's trace and report only. */
            asx_task_state s;
            if (asx_task_get_state(t->id, &s) == ASX_OK && s != ASX_TASK_COMPLETED) quiescent = 0;
            continue;
        }
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
                if (t->server) {
                    outcome = server_outcome_node(t->id);
                } else if (t->supervisor) {
                    uint32_t k;
                    for (k = 0; k < g_n_sups; k++) {
                        if (g_sups[k].task == i) outcome = supervisor_outcome_node(&g_sups[k]);
                    }
                } else {
                    outcome = outcome_node(t->id);
                }
            } else {
                asx_cancel_reason r;
                quiescent = 0;
                /* Only the cancel states carry a reason: a budget cancel a
                 * Running task has not acknowledged is not on its record
                 * (Rust, bd-mex3). */
                in_cancel = s == ASX_TASK_CANCEL_REQUESTED || s == ASX_TASK_CANCELLING ||
                            s == ASX_TASK_FINALIZING;
                if (in_cancel && asx_task_get_cancel_reason(t->id, &r) == ASX_OK) {
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
        {
            /* A cancelled live task's cleanup budget (Rust's TaskState
             * cleanup_budget; vocabulary §6), null otherwise. */
            asx_budget cb;
            uint32_t node = asx_json_new_null(g_out);
            if (in_cancel && asx_task_get_cleanup_budget(t->id, &cb) == ASX_OK) {
                node = asx_json_new_object(g_out);
                asx_json_set(g_out, node, "poll_quota", asx_json_new_u64(g_out, cb.poll_quota));
                asx_json_set(g_out, node, "priority", asx_json_new_u64(g_out, cb.priority));
            }
            asx_json_set(g_out, rec, "cleanup_budget", node);
        }
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

/* One run of a scenario. `lab` selects the scheduler: 1 dispatches as the
 * Rust lab does with the scenario's seed (and records its dispatches and
 * schedule certificate, and replays a forced schedule if one is installed);
 * 0 leaves the runtime on its default sweep scheduler, which is what
 * asx_scheduler_run does for a live user (bd-9kll.4.9). */
static asx_status conformance_run(const asx_json_doc *in, uint32_t scenario, asx_json_doc *out,
                                  uint32_t *out_root, char *error, size_t error_cap, int lab) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_budget budget;
    asx_region_id root = ASX_INVALID_ID;
    uint64_t max_steps = 0;
    uint64_t seed = 0;
    uint32_t list;
    uint32_t i;
    uint32_t raw_events;
    uint32_t obligations;
    uint32_t trace = ASX_JSON_NONE;
    uint32_t snapshot;
    uint32_t observations;
    uint32_t dispatches;
    uint32_t result;
    uint32_t semantic;
    const char *schema;
    asx_status st;

    if (in == NULL || out == NULL || out_root == NULL) return ASX_E_INVALID_ARGUMENT;
    memcpy(&g_work, in, sizeof(g_work));
    g_in = &g_work;
    g_out = out;
    g_error = error;
    g_error_cap = error_cap;
    g_failed = 0;
    g_n_tasks = 0;
    g_n_intervals = 0;
    g_n_delay_queues = 0;
    g_n_delay_keys = 0;
    g_n_servers = 0;
    g_n_monitors = 0;
    g_n_sups = 0;
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

    /* Runtime on its virtual clock (default stub hooks), trace observed,
     * dispatching as the Rust lab does with the scenario's seed
     * (LabConfig::new(seed).worker_count(1); bd-9kll.4.2). */
    if (!asx_json_u64(in, asx_json_get(in, scenario, "seed"), &seed)) {
        it_fail("scenario seed must be an unsigned integer", NULL);
        return ASX_E_INVALID_ARGUMENT;
    }
    /* Each scenario owns its bootstrap object. End the previous scenario
     * explicitly; initialization must not reset someone else's runtime. */
    asx_runtime_shutdown(&g_rt);
    asx_runtime_config_init(&cfg);
    /* DSL section 1: lab.panic_on_leak (default false) is
     * LabConfig::panic_on_leak. The runtime's own default is PANIC (as
     * Rust's), so a scenario that does not ask for it says LOG: a leak is
     * reported and the run completes. */
    {
        int panic_on_leak = 0;
        (void)asx_json_bool(in,
                            asx_json_get(in, asx_json_get(in, scenario, "lab"), "panic_on_leak"),
                            &panic_on_leak);
        cfg.leak_response = panic_on_leak ? ASX_LEAK_PANIC : ASX_LEAK_LOG;
    }
    st = asx_runtime_hooks_init(&hooks);
    if (st == ASX_OK) st = asx_runtime_init(&g_rt, &cfg, &hooks);
    if (st == ASX_OK && lab) st = asx_scheduler_use_lab_dispatch(seed);
    if (st != ASX_OK) {
        it_fail("runtime init failed", NULL);
        return st;
    }
    if (lab) asx_scheduler_record_dispatches(g_dispatches, IT_MAX_DISPATCHES);
    g_forced_on = 0;
    if (lab && g_forced != NULL) {
        /* Replay the recorded schedule: every lab step takes its task from
         * it, the scenario's tasks found by name. */
        asx_forced_error ferr;
        memset(&g_forced_cfg, 0, sizeof(g_forced_cfg));
        g_forced_cfg.resolve = forced_resolve;
        st = asx_lab_forced_schedule_begin(g_forced, &g_forced_cfg, &ferr);
        if (st != ASX_OK) {
            it_fail("forced schedule refused", asx_forced_error_kind_name(ferr.kind));
            return st;
        }
        g_forced_on = 1;
    }
    asx_trace_set_observer(it_observe, NULL);
    g_observations = asx_json_new_array(out);

    /* Sync objects (DSL §3.7); their arenas are not part of the runtime
     * reset. */
    asx_notify_reset();
    asx_semaphore_reset();
    asx_rwlock_reset();
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
        } else if (strcmp(type, "rwlock") == 0) {
            s->type = IT_SYNC_RWLOCK;
            st = asx_rwlock_create(&s->rwlock);
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
        if (add_region(name, parent->name, id)) register_finalizers(r, name, id);
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
        (void)asx_cx_init(&t->cx, t->region, id, ASX_CAP_NONE);
    }

    if (!g_failed) setup_channels(scenario, root);

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
    if (!g_failed && g_forced_on) {
        /* The run must end where the schedule's receipt says: every
         * dispatch consumed, quiescent, same step and clock, same
         * certificate. */
        asx_forced_error ferr;
        asx_forced_report frep;
        memset(&ferr, 0, sizeof(ferr));
        g_forced_on = 0;
        if (asx_lab_forced_schedule_end(&frep, &ferr) != ASX_OK) forced_report(&ferr);
    }
    asx_trace_set_observer(NULL, NULL);
    if (g_failed) return ASX_E_INVALID_STATE;

    /* Supervisor regions no generation named. */
    name_generationless_regions();
    name_unstarted_regions();

    /* Project (DSL §2). */
    obligations = asx_json_new_object(out);
    raw_events = project_events(obligations);
    if (!g_failed && asx_canon_trace(out, raw_events, out, &trace) != ASX_OK) {
        it_fail("canonicalization failed", asx_canon_error());
    }
    project_refused_admissions();
    snapshot = build_snapshot(obligations);
    observations = sorted_observations();
    dispatches = project_dispatches();
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
    asx_json_set(out, result, "dispatches", dispatches);
    {
        /* Rust's ScheduleCertificate and its decision count (bd-9kll.4.8),
         * compared with the fixture's schedule.certificate_hash/decisions. */
        char cert[17];
        (void)snprintf(cert, sizeof(cert), "%016llx",
                       (unsigned long long)asx_scheduler_schedule_certificate());
        asx_json_set(out, result, "certificate_hash", asx_json_new_string(out, cert));
        asx_json_set(out, result, "decisions",
                     asx_json_new_u64(out, asx_scheduler_schedule_decisions()));
    }
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

asx_status asx_conformance_run(const asx_json_doc *in, uint32_t scenario, asx_json_doc *out,
                               uint32_t *out_root, char *error, size_t error_cap) {
    return conformance_run(in, scenario, out, out_root, error, error_cap, 1);
}

asx_status asx_conformance_run_sweep(const asx_json_doc *in, uint32_t scenario, asx_json_doc *out,
                                     uint32_t *out_root, char *error, size_t error_cap) {
    return conformance_run(in, scenario, out, out_root, error, error_cap, 0);
}

const char *asx_conformance_obligation_name(uint64_t entity_id) {
    uint32_t i;
    for (i = 0; i < g_n_obl_names; i++) {
        if (same_entity(g_obl_names[i].id, entity_id)) return &g_text[g_obl_names[i].name_off];
    }
    return NULL;
}

/* The requirement names a scenario's "requires" may carry (DSL section 1),
 * each with the overload-catalog behavior whose prohibition leaves a
 * profile unable to run it. */
static const struct {
    const char *name;
    uint32_t forbidden;
} g_requirement_names[] = {{"unbounded_queue", ASX_FORBID_UNBOUNDED_QUEUE}};

#define IT_REQUIREMENTS (sizeof(g_requirement_names) / sizeof(g_requirement_names[0]))

int asx_conformance_requirement_forbidden(const asx_json_doc *in, uint32_t scenario,
                                          asx_profile_id profile, const char **out_name) {
    const asx_overload_catalog_entry *entry = NULL;
    const char *first = NULL;
    uint32_t list = asx_json_get(in, scenario, "requires");
    uint32_t i;
    *out_name = NULL;
    if (asx_overload_catalog_get(profile, &entry) != ASX_OK) return -1;
    if (list == ASX_JSON_NONE) return 0;
    if (asx_json_type_of(in, list) != ASX_JSON_ARRAY) return -1;
    for (i = 0; i < asx_json_count(in, list); i++) {
        const char *want = asx_json_string(in, asx_json_item(in, list, i));
        size_t k;
        for (k = 0; want != NULL && k < IT_REQUIREMENTS; k++) {
            if (strcmp(want, g_requirement_names[k].name) == 0) break;
        }
        if (want == NULL || k == IT_REQUIREMENTS) return -1;
        if (first == NULL && (entry->forbidden & g_requirement_names[k].forbidden) != 0u) {
            first = g_requirement_names[k].name;
        }
    }
    *out_name = first;
    return first != NULL;
}
