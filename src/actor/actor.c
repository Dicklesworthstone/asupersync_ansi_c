/*
 * actor.c — GenServer (Rust src/gen_server.rs) on the runtime's tasks,
 * mpsc mailbox and oneshot replies
 *
 * The mailbox carries envelope indices: a server's envelope table holds
 * each queued cast or call (its payload and, for a call, the caller's
 * reply permit). The channel bounds queued plus reserved messages by the
 * mailbox capacity and an envelope is released when it is received, so a
 * table of capacity + 1 entries (one for a cast between its envelope and
 * its send) never runs out.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../runtime/runtime_internal.h"
#include <asx/actor/actor.h>
#include <asx/runtime/runtime.h>
#include <asx/runtime/trace.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef enum { ENV_CAST = 1, ENV_CALL = 2 } actor_env_kind;

typedef struct {
    uint8_t kind; /* 0: free */
    uint64_t payload;
    asx_oneshot_permit reply; /* ENV_CALL */
} actor_envelope;

#define ACTOR_ENVELOPES ((ASX_ACTOR_MAILBOX_CAPACITY) + 1u)

/* ACTOR_YIELD_INTERVAL: messages served (or drained) between two yields
 * (runtime_internal.h). */

/* Where the server task's next poll resumes. */
typedef enum {
    PH_START = 0, /* first poll: init */
    PH_TOP = 1,   /* top of the message loop: cancel check */
    PH_RECV = 2,  /* waiting in the mailbox receive */
    PH_DRAIN = 3, /* draining after the loop */
    PH_DONE = 4
} actor_phase;

typedef struct {
    uint32_t generation;
    int in_use;
    asx_actor_state state;
    asx_actor_behavior behavior;
    void *user_state;
    asx_task_id task_id; /* ASX_INVALID_ID while refused (see refusal) */
    /* A spawn refused at once under lab dispatch (closed region): Rust
     * queues the task anyway and its admission refuses it at the next
     * step, dropping the server's cell then. The refusal's ticket, 0 for
     * none (asx_scheduler_last_spawn_refusal). */
    uint32_t refusal;
    asx_cx cx; /* the server task's */
    asx_channel_id mailbox;
    actor_envelope envelopes[ACTOR_ENVELOPES];
    actor_phase phase;
    uint32_t batch;   /* messages since the last yield */
    uint64_t drained; /* messages the drain took */
    int aborted;      /* cancelled when the loop ended: drained casts skipped */
    int masked;       /* the stop phase's cancellation mask is held */
    int ran;          /* the task was polled */
    int panicked;     /* a callback failed */
    asx_status exit_reason;
} asx_actor_slot;

static asx_actor_slot g_actors[ASX_MAX_ACTORS];

/* Servers spawned and not yet Stopped: asx_actor_task_finished runs on
 * every task completion and returns at once while there are none. */
static uint32_t g_live_servers;

/* Ops: call phases (cast uses 0 and 1). */
#define OP_CHECK 0u
#define OP_SEND 1u
#define OP_REPLY 2u
#define OP_DONE 3u

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t next_gen(uint32_t g) {
    g++;
    if (g == 0) g = 1;
    return g;
}

static void actor_cell_drop(asx_actor_slot *s);

/* Look up a handle's server, applying a refusal delivered since. */
static asx_actor_slot *actor_lookup(asx_actor_handle h) {
    asx_actor_slot *s;
    if (h.slot >= ASX_MAX_ACTORS) return NULL;
    s = &g_actors[h.slot];
    if (!s->in_use || s->generation != h.generation) return NULL;
    if (s->refusal != 0u && s->state != ASX_ACTOR_STOPPED &&
        asx_task_refusal_delivered(s->refusal)) {
        actor_cell_drop(s);
    }
    return s;
}

/* Inside its poll, park the caller until a refused server's refusal is
 * delivered too, so the cell drop it causes wakes the caller. */
static void actor_await_refusal(const asx_actor_slot *s) {
    asx_task_id self = asx_task_current();
    asx_status st;
    if (s->refusal == 0u || s->state == ASX_ACTOR_STOPPED || self == ASX_INVALID_ID) return;
    st = asx_task_await_refusal(self, s->refusal);
    (void)st; /* pending: parked; delivered: the next lookup drops the cell */
}

static asx_actor_handle actor_handle_of(const asx_actor_slot *s) {
    asx_actor_handle h;
    h.slot = (uint32_t)(s - g_actors);
    h.generation = s->generation;
    return h;
}

static void cx_trace(const asx_cx *cx, const char *message) {
    if (cx != NULL && cx->task_id != ASX_INVALID_ID) asx_trace_user(cx->task_id, message);
}

static int cx_cancelled(asx_cx *cx) { return cx != NULL && asx_cx_checkpoint(cx) != ASX_OK; }

static int actor_stopping(const asx_actor_slot *s) {
    return s->state == ASX_ACTOR_STOPPING || s->state == ASX_ACTOR_STOPPED;
}

static int env_alloc(asx_actor_slot *s, uint32_t *out) {
    uint32_t i;
    for (i = 0; i < ACTOR_ENVELOPES; i++) {
        if (s->envelopes[i].kind == 0u) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

/* Take a received envelope out of the table. */
static int env_take(asx_actor_slot *s, uint64_t index, actor_envelope *out) {
    if (index >= ACTOR_ENVELOPES || s->envelopes[index].kind == 0u) return 0;
    *out = s->envelopes[index];
    s->envelopes[index].kind = 0u;
    return 1;
}

/* Seal the mailbox and empty it, aborting the replies of queued calls
 * without handling anything (publish_server_panic and GenServerCell's
 * drop, gen_server.rs:2449, :805). */
static void actor_discard_mailbox(asx_actor_slot *s) {
    uint64_t v;
    actor_envelope env;
    asx_status st = asx_channel_seal(s->mailbox);
    (void)st; /* already sealed when draining */
    while (asx_channel_try_recv(s->mailbox, &v) == ASX_OK) {
        if (env_take(s, v, &env) && env.kind == ENV_CALL) asx_oneshot_permit_abort(&env.reply);
    }
}

/* The server is Stopped: the cell's sender goes, so its channel is
 * released once drained (publish_server_final, :2438). */
static void actor_publish(asx_actor_slot *s) {
    asx_status st;
    if (s->state != ASX_ACTOR_STOPPED && g_live_servers > 0u) g_live_servers--;
    s->state = ASX_ACTOR_STOPPED;
    s->phase = PH_DONE;
    st = asx_channel_close_sender(s->mailbox);
    (void)st; /* the mailbox is sealed: this closes it fully */
}

/* The server can no longer run (its task finished without stopping it,
 * or its spawn was refused): Rust drops its cell (GenServerCell's Drop,
 * :805), so it is Stopped, its mailbox sealed and the replies of queued
 * calls aborted. */
static void actor_cell_drop(asx_actor_slot *s) {
    if (!s->ran) s->exit_reason = ASX_E_CANCELLED;
    actor_discard_mailbox(s);
    actor_publish(s);
}

/* ------------------------------------------------------------------ */
/* The server task (run_gen_server_loop, gen_server.rs:2074)           */
/* ------------------------------------------------------------------ */

static asx_status actor_start(asx_actor_slot *s) {
    /* start_running: Created → Running unless a stop came first. */
    if (s->state == ASX_ACTOR_CREATED) s->state = ASX_ACTOR_RUNNING;
    if (cx_cancelled(&s->cx) || s->state == ASX_ACTOR_STOPPING) {
        cx_trace(&s->cx, "gen_server::init_skipped_cancelled");
        return ASX_OK;
    }
    cx_trace(&s->cx, "gen_server::init");
    if (s->behavior.init == NULL) return ASX_OK;
    return s->behavior.init(s->user_state, actor_handle_of(s));
}

/* dispatch_envelope (:2227): a call's reply is sent as Reply::send does
 * (:660); a failed handler drops it while panicking, aborting it. */
static asx_status actor_dispatch(asx_actor_slot *s, const actor_envelope *env) {
    actor_envelope e = *env;
    asx_status st;
    uint64_t reply = 0;

    if (e.kind == ENV_CAST) {
        if (s->behavior.handle_cast == NULL) return ASX_OK;
        return s->behavior.handle_cast(s->user_state, e.payload, actor_handle_of(s));
    }
    /* No handle_call: Rust's handle_call that drops its reply, whose drop
     * bomb panics the server. */
    st = s->behavior.handle_call != NULL
             ? s->behavior.handle_call(s->user_state, e.payload, &reply, actor_handle_of(s))
             : ASX_E_INVALID_STATE;
    if (st != ASX_OK) {
        asx_oneshot_permit_abort(&e.reply);
        return st;
    }
    st = asx_oneshot_permit_send(&e.reply, reply);
    cx_trace(&s->cx,
             st == ASX_OK ? "gen_server::reply_committed" : "gen_server::reply_caller_gone");
    return ASX_OK;
}

/* The message loop. ASX_E_PENDING: parked in the receive or yielding;
 * ASX_OK: the loop ended; anything else: a callback failed. */
static asx_status actor_serve(asx_actor_slot *s, asx_task_id self) {
    for (;;) {
        uint64_t v;
        actor_envelope env;
        asx_status st;

        if (s->phase == PH_TOP) {
            if (cx_cancelled(&s->cx)) {
                cx_trace(&s->cx, "gen_server::cancel_requested");
                s->exit_reason = ASX_E_CANCELLED;
                return ASX_OK;
            }
            s->phase = PH_RECV;
        }
        st = asx_channel_recv(s->mailbox, &s->cx, &v);
        if (st == ASX_E_PENDING) {
            if (s->state != ASX_ACTOR_STOPPING) return ASX_E_PENDING;
            /* A stop request with an empty mailbox ends the loop as a
             * disconnect (:2110-2113); the receive's wait is dropped. */
            (void)asx_channel_wait_cancel(s->mailbox, self);
            st = ASX_E_DISCONNECTED;
        }
        if (st == ASX_E_CANCELLED) {
            cx_trace(&s->cx, "gen_server::recv_cancelled");
            s->exit_reason = ASX_E_CANCELLED;
            return ASX_OK;
        }
        if (st != ASX_OK) {
            cx_trace(&s->cx, "gen_server::mailbox_disconnected");
            return ASX_OK;
        }
        s->phase = PH_TOP;
        if (!env_take(s, v, &env)) return ASX_E_INVALID_STATE;
        st = actor_dispatch(s, &env);
        if (st != ASX_OK) return st;
        if (++s->batch >= ACTOR_YIELD_INTERVAL) {
            s->batch = 0;
            cx_trace(&s->cx, "gen_server::yield_after_ready_batch");
            return ASX_E_PENDING; /* runnable: one trip through the scheduler */
        }
    }
}

/* Phase 3 setup: Stopping, the abort status read before the mask, the
 * mask, the sealed mailbox (:2147-2170). */
static void actor_begin_stop(asx_actor_slot *s, asx_task_id self) {
    asx_status st;
    s->state = ASX_ACTOR_STOPPING;
    s->aborted = cx_cancelled(&s->cx);
    s->masked = asx_task_mask(self) == ASX_OK;
    st = asx_channel_seal(s->mailbox);
    (void)st; /* open until now: the cell holds a sender */
    s->batch = 0;
    s->phase = PH_DRAIN;
}

/* The drain (:2172-2206). ASX_E_PENDING: yielding; ASX_OK: empty;
 * anything else: a cast handler failed. */
static asx_status actor_drain(asx_actor_slot *s) {
    uint64_t v;
    actor_envelope env;
    while (asx_channel_try_recv(s->mailbox, &v) == ASX_OK) {
        if (!env_take(s, v, &env)) return ASX_E_INVALID_STATE;
        if (env.kind == ENV_CALL) {
            asx_oneshot_permit_abort(&env.reply);
            cx_trace(&s->cx, "gen_server::drain_abort_call");
        } else if (!s->aborted && s->behavior.handle_cast != NULL) {
            asx_status st = s->behavior.handle_cast(s->user_state, env.payload, actor_handle_of(s));
            if (st != ASX_OK) return st;
        }
        s->drained++;
        if (++s->batch >= ACTOR_YIELD_INTERVAL) {
            s->batch = 0;
            cx_trace(&s->cx, "gen_server::yield_during_drain");
            return ASX_E_PENDING;
        }
    }
    return ASX_OK;
}

static void actor_unmask(asx_actor_slot *s, asx_task_id self) {
    asx_status st;
    if (!s->masked) return;
    s->masked = 0;
    st = asx_task_unmask(self);
    (void)st; /* masked by actor_begin_stop */
}

/* A callback failed: Rust's panic path (publish_server_panic). */
static asx_status actor_fail(asx_actor_slot *s, asx_task_id self, asx_status why) {
    s->exit_reason = why;
    s->panicked = 1;
    actor_discard_mailbox(s);
    actor_unmask(s, self);
    actor_publish(s);
    return asx_task_panic(self, "gen_server callback failed") == ASX_OK ? ASX_OK : why;
}

static asx_status actor_poll(void *user_data, asx_task_id self) {
    uint32_t idx = (uint32_t)(uintptr_t)user_data;
    asx_actor_slot *s;
    asx_status st;

    if (idx >= ASX_MAX_ACTORS) return ASX_E_INVALID_STATE;
    s = &g_actors[idx];
    if (!s->in_use || s->phase == PH_DONE) return ASX_E_INVALID_STATE;
    s->ran = 1;

    if (s->phase == PH_START) {
        st = actor_start(s);
        if (st != ASX_OK) return actor_fail(s, self, st);
        s->phase = PH_TOP;
    }
    if (s->phase == PH_TOP || s->phase == PH_RECV) {
        st = actor_serve(s, self);
        if (st == ASX_E_PENDING) return ASX_E_PENDING;
        if (st != ASX_OK) return actor_fail(s, self, st);
        actor_begin_stop(s, self);
    }
    st = actor_drain(s);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    if (st != ASX_OK) return actor_fail(s, self, st);
    if (s->drained > 0u) cx_trace(&s->cx, "gen_server::mailbox_drained");
    cx_trace(&s->cx, "gen_server::terminate");
    if (s->behavior.terminate != NULL) {
        s->behavior.terminate(s->user_state, s->exit_reason, actor_handle_of(s));
    }
    actor_unmask(s, self);
    actor_publish(s);
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

asx_status asx_actor_spawn(asx_actor_handle *out, asx_region_id region,
                           const asx_actor_behavior *behavior, void *state,
                           uint32_t mailbox_capacity) {
    uint32_t idx;
    asx_actor_slot *s;
    asx_channel_id mailbox;
    asx_task_id tid;
    asx_status st;

    if (out == NULL || behavior == NULL) return ASX_E_INVALID_ARGUMENT;
    if (mailbox_capacity == 0u || mailbox_capacity > ASX_ACTOR_MAILBOX_CAPACITY)
        return ASX_E_INVALID_ARGUMENT;

    /* A slot is free until spawned, and again once its server stopped;
     * unused slots go first, so handles to stopped servers stay valid as
     * long as possible. */
    for (idx = 0; idx < ASX_MAX_ACTORS; idx++) {
        if (!g_actors[idx].in_use) break;
    }
    if (idx >= ASX_MAX_ACTORS) {
        for (idx = 0; idx < ASX_MAX_ACTORS; idx++) {
            if (g_actors[idx].state == ASX_ACTOR_STOPPED) break;
        }
    }
    if (idx >= ASX_MAX_ACTORS) return ASX_E_RESOURCE_EXHAUSTED;

    st = asx_channel_create(region, mailbox_capacity, &mailbox);
    if (st != ASX_OK) return st;
    tid = ASX_INVALID_ID;
    st = asx_task_spawn(region, actor_poll, (void *)(uintptr_t)idx, &tid);
    s = &g_actors[idx];
    s->refusal = 0u;
    if (st != ASX_OK) {
        /* Under lab dispatch Rust's handle exists anyway; its task's
         * admission refuses it at the next step. */
        s->refusal = asx_scheduler_last_spawn_refusal();
        if (s->refusal == 0u) {
            asx_status cs = asx_channel_close_receiver(mailbox);
            if (cs == ASX_OK) cs = asx_channel_close_sender(mailbox);
            (void)cs; /* just created: both sides were open */
            return st;
        }
        tid = ASX_INVALID_ID;
    }

    memset(s->envelopes, 0, sizeof(s->envelopes));
    s->generation = next_gen(s->generation);
    s->in_use = 1;
    s->state = ASX_ACTOR_CREATED;
    g_live_servers++;
    s->behavior = *behavior;
    s->user_state = state;
    s->task_id = tid;
    (void)asx_cx_init(&s->cx, region, tid, ASX_CAP_CANCEL_CHECK);
    s->mailbox = mailbox;
    s->phase = PH_START;
    s->batch = 0;
    s->drained = 0;
    s->aborted = 0;
    s->masked = 0;
    s->ran = 0;
    s->panicked = 0;
    s->exit_reason = ASX_OK;

    *out = actor_handle_of(s);
    return ASX_OK;
}

asx_status asx_actor_stop(asx_actor_handle actor) {
    asx_actor_slot *s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    /* request_stop never revives a Stopped server (:970). */
    if (s->state == ASX_ACTOR_STOPPED) return ASX_OK;
    s->state = ASX_ACTOR_STOPPING;
    return asx_channel_wake_receiver(s->mailbox) == ASX_OK ? ASX_OK : ASX_E_INVALID_STATE;
}

asx_status asx_actor_join(asx_actor_handle actor, asx_cx *cx) {
    asx_actor_slot *s = actor_lookup(actor);
    asx_task_state ts;
    (void)cx; /* RecvUninterruptibleFuture: cancellation does not end it */
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->task_id == ASX_INVALID_ID) {
        /* Refused: the join resolves when the refusal is delivered. */
        if (s->state == ASX_ACTOR_STOPPED) return ASX_E_CANCELLED;
        actor_await_refusal(s);
        return ASX_E_PENDING;
    }
    if (asx_task_get_state(s->task_id, &ts) == ASX_OK && ts != ASX_TASK_COMPLETED) {
        asx_task_id self = asx_task_current();
        if (self != ASX_INVALID_ID) {
            /* Parks the caller until the server's task finishes. */
            asx_outcome ignored;
            asx_status st = asx_task_join_poll(self, s->task_id, &ignored);
            if (st != ASX_E_PENDING) return st;
        }
        return ASX_E_PENDING;
    }
    if (!s->ran) return ASX_E_CANCELLED;
    return s->panicked ? ASX_E_INVALID_STATE : ASX_OK;
}

int asx_actor_is_alive(asx_actor_handle actor) {
    asx_actor_slot *s = actor_lookup(actor);
    return s != NULL && s->state != ASX_ACTOR_STOPPED;
}

asx_actor_state asx_actor_get_state(asx_actor_handle actor) {
    asx_actor_slot *s = actor_lookup(actor);
    return s != NULL ? s->state : ASX_ACTOR_STOPPED;
}

asx_status asx_actor_task_id(asx_actor_handle actor, asx_task_id *out) {
    asx_actor_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (s->task_id == ASX_INVALID_ID) return ASX_E_NOT_FOUND; /* refused */
    *out = s->task_id;
    return ASX_OK;
}

uint32_t asx_actor_mailbox_count(asx_actor_handle actor) {
    asx_actor_slot *s = actor_lookup(actor);
    uint32_t n = 0;
    if (s == NULL || s->state == ASX_ACTOR_STOPPED) return 0;
    if (asx_channel_queue_len(s->mailbox, &n) != ASX_OK) return 0;
    return n;
}

asx_status asx_actor_exit_reason(asx_actor_handle actor) {
    asx_actor_slot *s = actor_lookup(actor);
    if (s == NULL || s->state != ASX_ACTOR_STOPPED) return ASX_OK;
    return s->exit_reason;
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

asx_status asx_actor_cast(asx_actor_handle actor, asx_cx *cx, uint64_t msg, asx_actor_op *op) {
    asx_actor_slot *s;
    uint32_t e;
    asx_status st;

    if (op == NULL) return ASX_E_INVALID_ARGUMENT;
    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    if (op->phase == OP_CHECK) {
        if (cx_cancelled(cx)) {
            cx_trace(cx, "gen_server::cast_rejected_cancelled");
            return ASX_E_CANCELLED;
        }
        if (actor_stopping(s)) {
            cx_trace(cx, "gen_server::cast_rejected_stopped");
            return ASX_E_DISCONNECTED;
        }
        op->phase = OP_SEND;
    }
    /* sender.send(cx, envelope) (:1285): no obligation (TransientReserve). */
    if (!env_alloc(s, &e)) return ASX_E_RESOURCE_EXHAUSTED;
    s->envelopes[e].kind = ENV_CAST;
    s->envelopes[e].payload = msg;
    st = asx_channel_send(s->mailbox, cx, (uint64_t)e);
    if (st == ASX_OK) {
        op->phase = OP_DONE;
        return ASX_OK;
    }
    s->envelopes[e].kind = 0u;
    if (st == ASX_E_PENDING) {
        actor_await_refusal(s);
        return ASX_E_PENDING;
    }
    op->phase = OP_DONE;
    if (st == ASX_E_CANCELLED) {
        cx_trace(cx, "gen_server::cast_send_cancelled");
        return ASX_E_CANCELLED;
    }
    cx_trace(cx, "gen_server::cast_send_failed");
    return ASX_E_DISCONNECTED;
}

asx_status asx_actor_try_cast(asx_actor_handle actor, uint64_t msg) {
    asx_actor_slot *s = actor_lookup(actor);
    asx_send_permit permit;
    uint32_t e;
    asx_status st;

    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    if (actor_stopping(s)) return ASX_E_DISCONNECTED;
    st = asx_channel_try_reserve(s->mailbox, &permit);
    if (st == ASX_E_CHANNEL_FULL) return ASX_E_CHANNEL_FULL;
    if (st != ASX_OK) return ASX_E_DISCONNECTED;
    if (!env_alloc(s, &e)) {
        asx_send_permit_abort(&permit);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    s->envelopes[e].kind = ENV_CAST;
    s->envelopes[e].payload = msg;
    st = asx_send_permit_send(&permit, (uint64_t)e);
    if (st != ASX_OK) {
        s->envelopes[e].kind = 0u;
        return ASX_E_DISCONNECTED;
    }
    return ASX_OK;
}

/* Rust rejects a call from the root region (reject_root_region_caller,
 * :1085): its reply obligation cannot be scoped there. */
static int cx_in_root_region(const asx_cx *cx) {
    asx_region_slot *r;
    if (cx == NULL || asx_region_slot_lookup(cx->region_id, &r) != ASX_OK) return 0;
    return r->parent_id == ASX_INVALID_ID;
}

/* The call's first poll after its checks: reserve the mailbox slot, then
 * the reply permit, then enqueue (:1192-1244). */
static asx_status call_enqueue(asx_actor_slot *s, asx_cx *cx, uint64_t request, asx_actor_op *op) {
    asx_oneshot_sender reply_tx;
    uint32_t e;
    asx_status st;

    st = asx_channel_reserve(s->mailbox, cx, &op->permit);
    if (st == ASX_E_PENDING) return ASX_E_PENDING;
    if (st == ASX_E_CANCELLED) {
        cx_trace(cx, "gen_server::call_send_cancelled");
        return ASX_E_CANCELLED;
    }
    if (st != ASX_OK) {
        cx_trace(cx, "gen_server::call_send_failed");
        return ASX_E_DISCONNECTED;
    }
    /* The C arenas, not Rust: a full oneshot arena refuses the call. The
     * envelope always exists, the reservation holding its place. */
    st = asx_oneshot_create(&reply_tx, &op->reply);
    if (st != ASX_OK) {
        asx_send_permit_abort(&op->permit);
        return st;
    }
    if (!env_alloc(s, &e)) {
        asx_send_permit_abort(&op->permit);
        asx_oneshot_sender_drop(&reply_tx);
        asx_oneshot_receiver_drop(&op->reply);
        return ASX_E_RESOURCE_EXHAUSTED;
    }
    st = asx_oneshot_reserve(&reply_tx, cx, &s->envelopes[e].reply);
    if (st != ASX_OK) {
        cx_trace(cx, "gen_server::call_reply_reserve_cancelled");
        asx_send_permit_abort(&op->permit);
        asx_oneshot_receiver_drop(&op->reply);
        return ASX_E_CANCELLED;
    }
    s->envelopes[e].kind = ENV_CALL;
    s->envelopes[e].payload = request;
    st = asx_send_permit_send(&op->permit, (uint64_t)e);
    if (st != ASX_OK) {
        s->envelopes[e].kind = 0u;
        asx_oneshot_permit_abort(&s->envelopes[e].reply);
        asx_oneshot_receiver_drop(&op->reply);
        cx_trace(cx, "gen_server::call_send_failed");
        return ASX_E_DISCONNECTED;
    }
    cx_trace(cx, "gen_server::call_enqueued");
    op->phase = OP_REPLY;
    return ASX_OK;
}

asx_status asx_actor_call(asx_actor_handle actor, asx_cx *cx, uint64_t request, asx_actor_op *op,
                          uint64_t *out_reply) {
    asx_actor_slot *s;
    asx_status st;

    if (op == NULL || out_reply == NULL) return ASX_E_INVALID_ARGUMENT;
    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    if (op->phase == OP_CHECK) {
        if (cx_cancelled(cx)) {
            cx_trace(cx, "gen_server::call_rejected_cancelled");
            return ASX_E_CANCELLED;
        }
        if (actor_stopping(s)) {
            cx_trace(cx, "gen_server::call_rejected_stopped");
            return ASX_E_DISCONNECTED;
        }
        if (cx_in_root_region(cx)) {
            cx_trace(cx, "gen_server::call_rejected_root_region");
            return ASX_E_CANCELLED;
        }
        op->phase = OP_SEND;
    }
    if (op->phase == OP_SEND) {
        st = call_enqueue(s, cx, request, op);
        if (st == ASX_E_PENDING) {
            actor_await_refusal(s);
            return ASX_E_PENDING;
        }
        if (st != ASX_OK) {
            op->phase = OP_DONE;
            return st;
        }
    }
    if (op->phase != OP_REPLY) return ASX_E_INVALID_STATE;
    st = asx_oneshot_recv(&op->reply, cx, out_reply);
    if (st == ASX_E_PENDING) {
        actor_await_refusal(s);
        return ASX_E_PENDING;
    }
    op->phase = OP_DONE;
    if (st == ASX_OK) return ASX_OK;
    if (st == ASX_E_CANCELLED) {
        cx_trace(cx, "gen_server::call_reply_cancelled");
        asx_oneshot_receiver_drop(&op->reply);
        return ASX_E_CANCELLED;
    }
    cx_trace(cx, "gen_server::call_no_reply");
    return ASX_E_INVALID_STATE;
}

void asx_actor_op_drop(asx_actor_handle actor, asx_actor_op *op) {
    asx_actor_slot *s = actor_lookup(actor);
    if (op == NULL) return;
    if (op->phase == OP_SEND && s != NULL) {
        asx_task_id self = asx_task_current();
        if (self != ASX_INVALID_ID) (void)asx_channel_wait_cancel(s->mailbox, self);
    }
    if (op->phase == OP_REPLY) asx_oneshot_receiver_drop(&op->reply);
    op->phase = OP_DONE;
}

/* ------------------------------------------------------------------ */
/* Runtime integration                                                 */
/* ------------------------------------------------------------------ */

void asx_actor_task_finished(asx_task_id task) {
    uint32_t i;
    if (g_live_servers == 0u) return;
    for (i = 0; i < ASX_MAX_ACTORS; i++) {
        asx_actor_slot *s = &g_actors[i];
        if (!s->in_use || s->state == ASX_ACTOR_STOPPED || s->task_id == ASX_INVALID_ID) continue;
        if (asx_handle_index(s->task_id) != asx_handle_index(task)) continue;
        actor_cell_drop(s);
    }
}

void asx_actor_reset(void) {
    memset(g_actors, 0, sizeof(g_actors));
    g_live_servers = 0;
}
