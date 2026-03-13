/*
 * actor.c — actor model and gen_server implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/actor/actor.h>
#include <asx/runtime/runtime.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    asx_actor_msg_kind kind;
    uint64_t           payload;
    uint32_t           call_slot;   /* valid only for CALL messages */
} asx_actor_envelope;

typedef struct {
    uint64_t reply;
    int      active;
    int      replied;
} asx_actor_call_slot;

typedef struct {
    /* Identity */
    uint32_t           generation;
    int                alive;

    /* Behavior */
    asx_actor_behavior behavior;
    void              *state;

    /* Mailbox (ring buffer) */
    asx_actor_envelope mailbox[ASX_ACTOR_MAILBOX_CAPACITY];
    uint32_t           mb_head;     /* next read position */
    uint32_t           mb_tail;     /* next write position */
    uint32_t           mb_count;    /* messages in buffer */

    /* Pending call/reply slots */
    asx_actor_call_slot pending_calls[ASX_ACTOR_MAX_PENDING_CALLS];

    /* Task integration */
    asx_task_id        task_id;
    int                initialized;
    int                stop_requested;
    asx_status         exit_reason;
} asx_actor_slot;

/* ------------------------------------------------------------------ */
/* Global arena                                                        */
/* ------------------------------------------------------------------ */

static asx_actor_slot g_actors[ASX_MAX_ACTORS];
static uint32_t       g_actor_count;

/* ------------------------------------------------------------------ */
/* Generation helper (skip 0)                                          */
/* ------------------------------------------------------------------ */

static uint32_t next_gen(uint32_t g)
{
    g++;
    if (g == 0) g = 1;
    return g;
}

/* ------------------------------------------------------------------ */
/* Slot lookup (generation-safe)                                       */
/* ------------------------------------------------------------------ */

static asx_actor_slot *actor_lookup(asx_actor_handle h)
{
    asx_actor_slot *s;
    if (h.slot >= ASX_MAX_ACTORS) return NULL;
    s = &g_actors[h.slot];
    if (!s->alive) return NULL;
    if (s->generation != h.generation) return NULL;
    return s;
}

/* ------------------------------------------------------------------ */
/* Mailbox operations                                                  */
/* ------------------------------------------------------------------ */

static int mb_full(const asx_actor_slot *s)
{
    return s->mb_count >= ASX_ACTOR_MAILBOX_CAPACITY;
}

static asx_status mb_push(asx_actor_slot *s, const asx_actor_envelope *env)
{
    if (mb_full(s)) return ASX_E_WOULD_BLOCK;
    s->mailbox[s->mb_tail] = *env;
    s->mb_tail = (s->mb_tail + 1) % ASX_ACTOR_MAILBOX_CAPACITY;
    s->mb_count++;
    return ASX_OK;
}

static int mb_pop(asx_actor_slot *s, asx_actor_envelope *out)
{
    if (s->mb_count == 0) return 0;
    *out = s->mailbox[s->mb_head];
    s->mb_head = (s->mb_head + 1) % ASX_ACTOR_MAILBOX_CAPACITY;
    s->mb_count--;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Call slot operations                                                 */
/* ------------------------------------------------------------------ */

static int call_slot_alloc(asx_actor_slot *s, uint32_t *out)
{
    uint32_t i;
    for (i = 0; i < ASX_ACTOR_MAX_PENDING_CALLS; i++) {
        if (!s->pending_calls[i].active) {
            s->pending_calls[i].active = 1;
            s->pending_calls[i].replied = 0;
            s->pending_calls[i].reply = 0;
            *out = i;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Actor poll function (the gen_server run loop)                        */
/* ------------------------------------------------------------------ */

static asx_status actor_poll(void *user_data, asx_task_id self)
{
    /* user_data is the actor slot index, packed as a pointer */
    uint32_t slot_idx = (uint32_t)(uintptr_t)user_data;
    asx_actor_slot *s;
    asx_actor_handle handle;
    asx_actor_envelope env;
    asx_status st;

    (void)self;

    if (slot_idx >= ASX_MAX_ACTORS) return ASX_E_INVALID_STATE;
    s = &g_actors[slot_idx];
    if (!s->alive) return ASX_E_INVALID_STATE;

    handle.slot = slot_idx;
    handle.generation = s->generation;

    /* Phase 1: Initialize on first poll */
    if (!s->initialized) {
        s->initialized = 1;
        if (s->behavior.init != NULL) {
            st = s->behavior.init(s->state, handle);
            if (st != ASX_OK) {
                s->exit_reason = st;
                goto terminate;
            }
        }
        /* Don't process messages on init poll — return pending */
        return ASX_E_PENDING;
    }

    /* Phase 2: Process one message per poll (cooperative) */
    if (mb_pop(s, &env)) {
        switch (env.kind) {
        case ASX_ACTOR_MSG_CAST:
            if (s->behavior.handle_cast != NULL) {
                st = s->behavior.handle_cast(s->state, env.payload, handle);
                if (st != ASX_OK) {
                    s->exit_reason = st;
                    goto terminate;
                }
            }
            break;

        case ASX_ACTOR_MSG_CALL:
            if (s->behavior.handle_call != NULL &&
                env.call_slot < ASX_ACTOR_MAX_PENDING_CALLS) {
                uint64_t reply = 0;
                st = s->behavior.handle_call(
                    s->state, env.payload, &reply, handle);
                if (st == ASX_OK) {
                    s->pending_calls[env.call_slot].reply = reply;
                    s->pending_calls[env.call_slot].replied = 1;
                } else {
                    /* Handler failed — mark slot inactive so caller
                     * gets ASX_E_INVALID_STATE on poll */
                    s->pending_calls[env.call_slot].active = 0;
                    s->exit_reason = st;
                    goto terminate;
                }
            } else {
                /* No call handler — release slot */
                if (env.call_slot < ASX_ACTOR_MAX_PENDING_CALLS) {
                    s->pending_calls[env.call_slot].active = 0;
                }
            }
            break;

        case ASX_ACTOR_MSG_STOP:
            s->stop_requested = 1;
            break;
        }
    }

    /* Phase 3: Check for shutdown */
    if (s->stop_requested && s->mb_count == 0) {
        s->exit_reason = ASX_OK;
        goto terminate;
    }

    return ASX_E_PENDING;

terminate:
    if (s->behavior.terminate != NULL) {
        s->behavior.terminate(s->state, s->exit_reason, handle);
    }
    s->alive = 0;
    /* Always return ASX_OK to the scheduler — the actor completed its
     * lifecycle normally from the task's perspective. The actor-level
     * exit reason is tracked in exit_reason for supervisor queries.
     * This prevents fault containment from poisoning the region when
     * a supervised child fails. */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Public API: Lifecycle                                               */
/* ------------------------------------------------------------------ */

asx_status asx_actor_spawn(
    asx_actor_handle *out,
    asx_region_id region,
    const asx_actor_behavior *behavior,
    void *state)
{
    uint32_t idx;
    asx_actor_slot *s;
    asx_task_id tid;
    asx_status st;

    if (out == NULL || behavior == NULL) return ASX_E_INVALID_ARGUMENT;
    if (behavior->handle_cast == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Find free slot */
    for (idx = 0; idx < ASX_MAX_ACTORS; idx++) {
        if (!g_actors[idx].alive) break;
    }
    if (idx >= ASX_MAX_ACTORS) return ASX_E_RESOURCE_EXHAUSTED;

    s = &g_actors[idx];

    /* Spawn the actor's task */
    st = asx_task_spawn(region, actor_poll,
                        (void *)(uintptr_t)idx, &tid);
    if (st != ASX_OK) return st;

    /* Initialize actor slot */
    s->generation = next_gen(s->generation);
    s->alive = 1;
    s->behavior = *behavior;
    s->state = state;
    s->mb_head = 0;
    s->mb_tail = 0;
    s->mb_count = 0;
    memset(s->pending_calls, 0, sizeof(s->pending_calls));
    s->task_id = tid;
    s->initialized = 0;
    s->stop_requested = 0;
    s->exit_reason = ASX_OK;

    if (idx >= g_actor_count) g_actor_count = idx + 1;

    out->slot = idx;
    out->generation = s->generation;
    return ASX_OK;
}

asx_status asx_actor_stop(asx_actor_handle actor)
{
    asx_actor_slot *s;
    asx_actor_envelope env;

    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Enqueue stop message */
    env.kind = ASX_ACTOR_MSG_STOP;
    env.payload = 0;
    env.call_slot = 0;
    return mb_push(s, &env);
}

int asx_actor_is_alive(asx_actor_handle actor)
{
    asx_actor_slot *s = actor_lookup(actor);
    return s != NULL;
}

asx_status asx_actor_task_id(
    asx_actor_handle actor, asx_task_id *out)
{
    asx_actor_slot *s;
    if (out == NULL) return ASX_E_INVALID_ARGUMENT;
    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;
    *out = s->task_id;
    return ASX_OK;
}

uint32_t asx_actor_mailbox_count(asx_actor_handle actor)
{
    asx_actor_slot *s = actor_lookup(actor);
    if (s == NULL) return 0;
    return s->mb_count;
}

asx_status asx_actor_exit_reason(asx_actor_handle actor)
{
    asx_actor_slot *s;
    if (actor.slot >= ASX_MAX_ACTORS) return ASX_OK;
    s = &g_actors[actor.slot];
    /* Works even on dead actors if generation matches */
    if (s->generation != actor.generation) return ASX_OK;
    return s->exit_reason;
}

/* ------------------------------------------------------------------ */
/* Public API: Message passing                                         */
/* ------------------------------------------------------------------ */

asx_status asx_actor_cast(asx_actor_handle actor, uint64_t msg)
{
    asx_actor_slot *s;
    asx_actor_envelope env;

    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    env.kind = ASX_ACTOR_MSG_CAST;
    env.payload = msg;
    env.call_slot = 0;
    return mb_push(s, &env);
}

asx_status asx_actor_call(
    asx_actor_handle actor, uint64_t request,
    asx_call_token *out_token)
{
    asx_actor_slot *s;
    asx_actor_envelope env;
    uint32_t cs;

    if (out_token == NULL) return ASX_E_INVALID_ARGUMENT;

    s = actor_lookup(actor);
    if (s == NULL) return ASX_E_INVALID_ARGUMENT;

    /* Allocate a pending call slot */
    if (!call_slot_alloc(s, &cs))
        return ASX_E_RESOURCE_EXHAUSTED;

    /* Enqueue call message */
    env.kind = ASX_ACTOR_MSG_CALL;
    env.payload = request;
    env.call_slot = cs;

    {
        asx_status st = mb_push(s, &env);
        if (st != ASX_OK) {
            s->pending_calls[cs].active = 0;
            return st;
        }
    }

    out_token->actor_slot = actor.slot;
    out_token->call_slot = cs;
    out_token->generation = actor.generation;
    return ASX_OK;
}

asx_status asx_call_token_poll(asx_call_token token, uint64_t *reply)
{
    asx_actor_slot *s;
    asx_actor_call_slot *cs;

    if (reply == NULL) return ASX_E_INVALID_ARGUMENT;
    if (token.actor_slot >= ASX_MAX_ACTORS) return ASX_E_INVALID_ARGUMENT;

    s = &g_actors[token.actor_slot];

    /* Actor died or was recycled */
    if (!s->alive || s->generation != token.generation) {
        return ASX_E_INVALID_STATE;
    }

    if (token.call_slot >= ASX_ACTOR_MAX_PENDING_CALLS)
        return ASX_E_INVALID_ARGUMENT;

    cs = &s->pending_calls[token.call_slot];
    if (!cs->active) return ASX_E_INVALID_STATE;

    if (!cs->replied) return ASX_E_PENDING;

    *reply = cs->reply;
    cs->active = 0;  /* consume the reply */
    return ASX_OK;
}

/* ------------------------------------------------------------------ */
/* Reset (test support)                                                */
/* ------------------------------------------------------------------ */

void asx_actor_reset(void)
{
    memset(g_actors, 0, sizeof(g_actors));
    g_actor_count = 0;
}
