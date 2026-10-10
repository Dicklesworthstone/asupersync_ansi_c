/*
 * asx/actor/actor.h — GenServer: a task serving casts and calls from a
 * bounded mailbox (Rust src/gen_server.rs)
 *
 * A server is a task in a region. Its mailbox is an mpsc channel of the
 * capacity given at spawn; a call's reply travels through a oneshot whose
 * permit the caller reserves (a SendPermit obligation the caller holds)
 * and the server resolves. The server task runs Rust's
 * run_gen_server_loop (gen_server.rs:2074):
 *
 *   1. init: skipped when the task is already cancelled or a stop came
 *      first ("gen_server::init_skipped_cancelled"), else
 *      "gen_server::init" and the init callback (Rust on_start);
 *   2. the message loop: a cancel check at the top of each round
 *      ("gen_server::cancel_requested" ends it), then a receive: casts and
 *      calls are dispatched in arrival order, a call's reply committed
 *      ("gen_server::reply_committed", or "gen_server::reply_caller_gone"
 *      when the caller stopped waiting); every 8 messages the task yields
 *      ("gen_server::yield_after_ready_batch"). The loop ends when the
 *      receive is cancelled ("gen_server::recv_cancelled") or the mailbox
 *      is empty after a stop request ("gen_server::mailbox_disconnected");
 *   3. the stop: the server is Stopping, the mailbox is sealed and drained
 *      under a cancellation mask: queued calls get no reply
 *      ("gen_server::drain_abort_call"), queued casts are handled unless
 *      the server was cancelled; "gen_server::yield_during_drain" every 8
 *      and "gen_server::mailbox_drained" when anything was drained;
 *   4. "gen_server::terminate", the terminate callback (Rust on_stop), and
 *      the server is Stopped.
 *
 * Callbacks return ASX_OK. Any other status is the C form of a panic in
 * the callback, the only way a Rust handler fails: the server stops at
 * once without draining or terminating, the replies of queued calls are
 * aborted (their callers see no reply) and its task completes PANICKED.
 *
 * The client operations take a Cx and are polled like the Rust futures
 * they port: ASX_E_PENDING means "not yet" (inside a scheduler poll the
 * caller is parked until it can progress) and the call is repeated with
 * the same asx_actor_op until it returns anything else. A NULL Cx skips
 * cancellation checks, traces and obligations.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_ACTOR_ACTOR_H
#define ASX_ACTOR_ACTOR_H

#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/channel.h>
#include <asx/core/oneshot.h>
#include <asx/cx/cx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------
 * Arena limits
 * ------------------------------------------------------------------- */

#ifndef ASX_MAX_ACTORS
#define ASX_MAX_ACTORS 16u
#endif

/* The largest mailbox capacity a spawn may ask for (Rust takes any; its
 * plain actors default to 64, actor.rs:275-286). Each server reserves an
 * envelope table of this size plus one. */
#ifndef ASX_ACTOR_MAILBOX_CAPACITY
#define ASX_ACTOR_MAILBOX_CAPACITY 64u
#endif

#if (ASX_MAX_ACTORS) < 1 || (ASX_ACTOR_MAILBOX_CAPACITY) < 1
#error "ASX_MAX_ACTORS and ASX_ACTOR_MAILBOX_CAPACITY must be at least 1"
#endif
#if (ASX_ACTOR_MAILBOX_CAPACITY) > (ASX_CHANNEL_MAX_CAPACITY)
#error "ASX_ACTOR_MAILBOX_CAPACITY must not exceed ASX_CHANNEL_MAX_CAPACITY"
#endif

/* -------------------------------------------------------------------
 * Handles and state
 * ------------------------------------------------------------------- */

/* Value type, generation-tagged for stale detection. */
typedef struct {
    uint32_t slot;
    uint32_t generation;
} asx_actor_handle;

/* Rust ActorState (actor.rs), as GenServerStateCell holds it. */
typedef enum {
    ASX_ACTOR_CREATED = 0,  /* spawned, not yet polled */
    ASX_ACTOR_RUNNING = 1,  /* serving its mailbox */
    ASX_ACTOR_STOPPING = 2, /* stop requested, or draining */
    ASX_ACTOR_STOPPED = 3   /* finished */
} asx_actor_state;

/* -------------------------------------------------------------------
 * Behavior callbacks (Rust trait GenServer)
 *
 * init:         on_start, run once before the first message (optional).
 * handle_cast:  a cast (optional: Rust's default does nothing).
 * handle_call:  a call; write the reply to *reply. Without one a call
 *               fails the server, as a Rust handle_call that drops its
 *               reply panics it.
 * terminate:    on_stop, run once after the drain with the server's exit
 *               reason (optional). Not run after a failed callback.
 *
 * All callbacks receive the user state and the server's own handle.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_actor_init_fn)(void *state, asx_actor_handle self);

typedef asx_status (*asx_actor_cast_fn)(void *state, uint64_t msg, asx_actor_handle self);

typedef asx_status (*asx_actor_call_fn)(void *state, uint64_t request, uint64_t *reply,
                                        asx_actor_handle self);

typedef void (*asx_actor_terminate_fn)(void *state, asx_status reason, asx_actor_handle self);

typedef struct {
    asx_actor_init_fn init;
    asx_actor_cast_fn handle_cast;
    asx_actor_call_fn handle_call;
    asx_actor_terminate_fn terminate;
} asx_actor_behavior;

/* The progress of one cast or call. Zero it (or ASX_ACTOR_OP_INIT) before
 * the first poll and keep it until the operation returns. */
typedef struct {
    uint32_t phase;
    asx_send_permit permit;     /* call: the mailbox slot */
    asx_oneshot_receiver reply; /* call: the reply channel */
} asx_actor_op;

#define ASX_ACTOR_OP_INIT                                                                          \
    {                                                                                              \
        0u, {0u, 0u, 0, 0u}, { 0u, 0u }                                                            \
    }

/* -------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* `cx.spawn_gen_server(server, mailbox_capacity)` (gen_server.rs:2511):
 * spawn the server task in `region` with a mailbox of mailbox_capacity
 * messages (1..ASX_ACTOR_MAILBOX_CAPACITY). The task is admitted like any
 * spawn (under lab dispatch: at the scheduler's next step); a server whose
 * admission is refused never runs and is Stopped.
 * Returns ASX_OK and writes the handle to *out, ASX_E_INVALID_ARGUMENT for
 * a NULL argument or a bad capacity,
 * ASX_E_RESOURCE_EXHAUSTED when the actor, channel or task arena is full,
 * or the region's spawn error. */
ASX_API ASX_MUST_USE asx_status asx_actor_spawn(asx_actor_handle *out, asx_region_id region,
                                                const asx_actor_behavior *behavior, void *state,
                                                uint32_t mailbox_capacity);

/* `h.stop()` (:1453): ask the server to stop once its mailbox is empty,
 * waking it if it waits for a message. Stopping a finished server changes
 * nothing. Returns ASX_OK, or ASX_E_INVALID_ARGUMENT for a stale handle. */
ASX_API asx_status asx_actor_stop(asx_actor_handle actor);

/* `h.join(&cx)` (:1489), one poll: ASX_OK once the server task has
 * finished after running, ASX_E_CANCELLED when its task finished without
 * ever running (admission refused), ASX_E_INVALID_STATE when a callback
 * failed (Rust JoinError::Panicked), else ASX_E_PENDING (the caller is
 * parked until the task finishes). The wait is not interrupted by the
 * caller's cancellation. ASX_E_INVALID_ARGUMENT for a stale handle. */
ASX_API ASX_MUST_USE asx_status asx_actor_join(asx_actor_handle actor, asx_cx *cx);

/* Whether the server has not yet finished (spawned and not Stopped). */
ASX_API int asx_actor_is_alive(asx_actor_handle actor);

/* The server's state; ASX_ACTOR_STOPPED for a stale handle. */
ASX_API asx_actor_state asx_actor_get_state(asx_actor_handle actor);

/* The task backing a server. */
ASX_API asx_status asx_actor_task_id(asx_actor_handle actor, asx_task_id *out);

/* Messages queued in the server's mailbox (0 for a stale handle). */
ASX_API uint32_t asx_actor_mailbox_count(asx_actor_handle actor);

/* Why a finished server finished, while its slot is not reused: ASX_OK for
 * a stop, ASX_E_CANCELLED for a cancellation, or the failing callback's
 * status. ASX_OK for a live server or a stale handle. */
ASX_API asx_status asx_actor_exit_reason(asx_actor_handle actor);

/* -------------------------------------------------------------------
 * Messages
 * ------------------------------------------------------------------- */

/* `h.cast(&cx, msg)` (:1268), one poll. Checks first: a cancelled Cx gives
 * ASX_E_CANCELLED ("gen_server::cast_rejected_cancelled"), a stopping or
 * stopped server ASX_E_DISCONNECTED ("gen_server::cast_rejected_stopped").
 * Then the send, ASX_E_PENDING while the mailbox is full; it ends with
 * ASX_OK, ASX_E_CANCELLED ("gen_server::cast_send_cancelled") or
 * ASX_E_DISCONNECTED when the mailbox closed ("gen_server::cast_send_failed").
 * A cast's mailbox slot is not an obligation. */
ASX_API ASX_MUST_USE asx_status asx_actor_cast(asx_actor_handle actor, asx_cx *cx, uint64_t msg,
                                               asx_actor_op *op);

/* `h.try_cast(msg)` (:1305): ASX_OK, ASX_E_CHANNEL_FULL when the mailbox
 * is full, ASX_E_DISCONNECTED when the server stops or stopped. Never
 * waits. */
ASX_API ASX_MUST_USE asx_status asx_actor_try_cast(asx_actor_handle actor, uint64_t msg);

/* `h.call(&cx, request)` (:1171), one poll; *out_reply is written on
 * ASX_OK. Checks first: a cancelled Cx gives ASX_E_CANCELLED
 * ("gen_server::call_rejected_cancelled"), a stopping or stopped server
 * ASX_E_DISCONNECTED ("gen_server::call_rejected_stopped"), a caller in a
 * root region ASX_E_CANCELLED ("gen_server::call_rejected_root_region";
 * Rust's [ASUP-E103]). Then the mailbox reservation (a SendPermit
 * obligation), ASX_E_PENDING while full, ending with ASX_E_CANCELLED
 * ("gen_server::call_send_cancelled") or ASX_E_DISCONNECTED
 * ("gen_server::call_send_failed"); the reply permit (a second SendPermit
 * obligation); the enqueue ("gen_server::call_enqueued"); then the wait
 * for the reply: ASX_OK, ASX_E_INVALID_STATE when the server dropped the
 * call ("gen_server::call_no_reply"), or ASX_E_CANCELLED
 * ("gen_server::call_reply_cancelled"). ASX_E_RESOURCE_EXHAUSTED when the
 * oneshot arena is full. */
ASX_API ASX_MUST_USE asx_status asx_actor_call(asx_actor_handle actor, asx_cx *cx, uint64_t request,
                                               asx_actor_op *op, uint64_t *out_reply);

/* Abandon an operation that has not returned (the analog of dropping the
 * Rust future): a pending call stops waiting for its reply, so the
 * server's reply finds the caller gone. */
ASX_API void asx_actor_op_drop(asx_actor_handle actor, asx_actor_op *op);

/* -------------------------------------------------------------------
 * Runtime integration
 * ------------------------------------------------------------------- */

/* The runtime calls this when a task finishes. A server whose task
 * finished without reaching Stopped (its admission was refused, or it was
 * force-completed) is stopped as Rust drops its cell (GenServerCell's
 * Drop, gen_server.rs:805): its mailbox is sealed and the replies of its
 * queued calls are aborted. */
ASX_API void asx_actor_task_finished(asx_task_id task);

/* Reset all actor state. Called by asx_runtime_reset(). */
ASX_API void asx_actor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_ACTOR_ACTOR_H */
