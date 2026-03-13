/*
 * asx/actor/actor.h — actor model and gen_server semantics
 *
 * Provides:
 *   - Actor handles with mailbox-based message passing
 *   - Fire-and-forget cast messages
 *   - Request/reply call messages with poll-based reply retrieval
 *   - Behavior callbacks (init, handle_cast, handle_call, terminate)
 *   - Stateful server loops built on the structured runtime
 *
 * Actors are backed by tasks: each actor spawns a task that drives
 * its message-processing loop. Messages flow through a fixed-capacity
 * internal mailbox (ring buffer). Call/reply uses a small pending-call
 * arena per actor.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ACTOR_ACTOR_H
#define ASX_ACTOR_ACTOR_H

#include <asx/asx_export.h>
#include <asx/asx_status.h>
#include <asx/asx_ids.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_ACTORS
#define ASX_MAX_ACTORS 16u
#endif

#ifndef ASX_ACTOR_MAILBOX_CAPACITY
#define ASX_ACTOR_MAILBOX_CAPACITY 16u
#endif

#ifndef ASX_ACTOR_MAX_PENDING_CALLS
#define ASX_ACTOR_MAX_PENDING_CALLS 8u
#endif

/* -------------------------------------------------------------------
 * Actor handle — value type, generation-tagged for stale detection
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_actor_handle;

/* -------------------------------------------------------------------
 * Message types
 * ------------------------------------------------------------------- */

typedef enum {
    ASX_ACTOR_MSG_CAST = 0,    /* fire-and-forget */
    ASX_ACTOR_MSG_CALL = 1,    /* request expecting reply */
    ASX_ACTOR_MSG_STOP = 2     /* graceful shutdown request */
} asx_actor_msg_kind;

/* -------------------------------------------------------------------
 * Call token — returned from asx_actor_call, polled for reply
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t actor_slot;
    uint32_t call_slot;
    uint32_t generation;   /* actor generation at call time */
} asx_call_token;

/* -------------------------------------------------------------------
 * Behavior callbacks — the gen_server interface
 *
 * init:         Called once on first poll. Return ASX_OK to proceed,
 *               or error to terminate the actor immediately.
 * handle_cast:  Process a fire-and-forget message.
 * handle_call:  Process a request. Write reply value to *reply.
 *               Return ASX_OK to send the reply.
 * terminate:    Called once when the actor is shutting down, with the
 *               exit reason. Always called, even on error.
 *
 * All callbacks receive the actor's user state and self handle.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_actor_init_fn)(
    void *state, asx_actor_handle self);

typedef asx_status (*asx_actor_cast_fn)(
    void *state, uint64_t msg, asx_actor_handle self);

typedef asx_status (*asx_actor_call_fn)(
    void *state, uint64_t request, uint64_t *reply,
    asx_actor_handle self);

typedef void (*asx_actor_terminate_fn)(
    void *state, asx_status reason, asx_actor_handle self);

typedef struct {
    asx_actor_init_fn      init;           /* optional (may be NULL) */
    asx_actor_cast_fn      handle_cast;    /* required */
    asx_actor_call_fn      handle_call;    /* optional (may be NULL) */
    asx_actor_terminate_fn terminate;      /* optional (may be NULL) */
} asx_actor_behavior;

/* -------------------------------------------------------------------
 * Actor lifecycle
 * ------------------------------------------------------------------- */

/* Spawn a new actor with the given behavior and state.
 * The actor's task is created in the specified region.
 * Returns ASX_OK on success and writes the handle to *out. */
ASX_API ASX_MUST_USE asx_status asx_actor_spawn(
    asx_actor_handle *out,
    asx_region_id region,
    const asx_actor_behavior *behavior,
    void *state);

/* Request graceful shutdown of an actor.
 * The actor will process remaining messages, call terminate,
 * then complete its task. */
ASX_API asx_status asx_actor_stop(asx_actor_handle actor);

/* Check if an actor is alive (spawned and not yet terminated). */
ASX_API int asx_actor_is_alive(asx_actor_handle actor);

/* Get the task ID backing an actor (for scheduler integration). */
ASX_API asx_status asx_actor_task_id(
    asx_actor_handle actor, asx_task_id *out);

/* Get the number of messages currently in an actor's mailbox. */
ASX_API uint32_t asx_actor_mailbox_count(asx_actor_handle actor);

/* Get the exit reason for a dead actor.
 * Works even after the actor has terminated, as long as the slot
 * hasn't been recycled (generation matches).
 * Returns ASX_OK for actors that stopped normally. */
ASX_API asx_status asx_actor_exit_reason(asx_actor_handle actor);

/* -------------------------------------------------------------------
 * Message passing
 * ------------------------------------------------------------------- */

/* Send a fire-and-forget message to an actor.
 * Returns ASX_E_WOULD_BLOCK if the mailbox is full. */
ASX_API ASX_MUST_USE asx_status asx_actor_cast(
    asx_actor_handle actor, uint64_t msg);

/* Send a request and obtain a call token for polling the reply.
 * Returns ASX_E_WOULD_BLOCK if the mailbox is full.
 * Returns ASX_E_RESOURCE_EXHAUSTED if pending call slots are full. */
ASX_API ASX_MUST_USE asx_status asx_actor_call(
    asx_actor_handle actor, uint64_t request,
    asx_call_token *out_token);

/* Poll a call token for a reply.
 * Returns ASX_OK and writes to *reply when the reply is available.
 * Returns ASX_E_PENDING if the actor hasn't processed the call yet.
 * Returns ASX_E_INVALID_STATE if the actor died before replying. */
ASX_API ASX_MUST_USE asx_status asx_call_token_poll(
    asx_call_token token, uint64_t *reply);

/* -------------------------------------------------------------------
 * Reset (test support)
 * ------------------------------------------------------------------- */

/* Reset all actor state. Called by asx_runtime_reset(). */
ASX_API void asx_actor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ACTOR_ACTOR_H */
