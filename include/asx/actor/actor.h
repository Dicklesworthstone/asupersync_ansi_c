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
 * Phases run under a tightened task budget (Rust PhaseBudgetGuard,
 * gen_server.rs:96): init under the budget the behavior's on_start_budget
 * returns, met with the task's, and restored afterwards minus what the
 * phase consumed; the drain and terminate under on_stop_budget (default
 * Budget::MINIMAL, 100 polls), met with the task's and never restored.
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

#include <asx/asx_config.h>
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

/* The largest mailbox capacity a spawn may ask for. Rust takes any and
 * defaults GenServers to 64 (DEFAULT_GENSERVER_MAILBOX_CAPACITY,
 * gen_server.rs:2071; checked by make check-rust-constants). Each server
 * reserves an envelope table of this size plus one. */
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
 * on_start_budget: the budget of the init phase (Rust on_start_budget,
 *               gen_server.rs:581). NULL: asx_budget_infinite(), which
 *               leaves the task's budget as it is.
 * on_stop_budget: the budget of the drain and terminate (Rust
 *               on_stop_budget, :593). NULL: Rust's Budget::MINIMAL, 100
 *               polls, no deadline or cost limit, priority 128.
 * cast_overflow_policy: what asx_actor_try_cast does when the mailbox is
 *               full (Rust cast_overflow_policy, :603). Zero is REJECT.
 *
 * init, handle_cast, handle_call and terminate receive the user state and
 * the server's own handle; the budget callbacks only the user state.
 * ------------------------------------------------------------------- */

typedef asx_status (*asx_actor_init_fn)(void *state, asx_actor_handle self);

typedef asx_status (*asx_actor_cast_fn)(void *state, uint64_t msg, asx_actor_handle self);

typedef asx_status (*asx_actor_call_fn)(void *state, uint64_t request, uint64_t *reply,
                                        asx_actor_handle self);

typedef void (*asx_actor_terminate_fn)(void *state, asx_status reason, asx_actor_handle self);

typedef asx_budget (*asx_actor_budget_fn)(void *state);

/* What asx_actor_try_cast does with a cast when the mailbox is full (Rust
 * CastOverflowPolicy, gen_server.rs:212). */
typedef enum {
    ASX_ACTOR_OVERFLOW_REJECT = 0,     /* refuse it: ASX_E_CHANNEL_FULL */
    ASX_ACTOR_OVERFLOW_DROP_OLDEST = 1 /* evict the oldest queued cast */
} asx_actor_overflow_policy;

typedef struct {
    asx_actor_init_fn init;
    asx_actor_cast_fn handle_cast;
    asx_actor_call_fn handle_call;
    asx_actor_terminate_fn terminate;
    asx_actor_budget_fn on_start_budget;
    asx_actor_budget_fn on_stop_budget;
    asx_actor_overflow_policy cast_overflow_policy;
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
 * waits.
 *
 * Under ASX_ACTOR_OVERFLOW_DROP_OLDEST a full mailbox does not refuse: the
 * oldest queued *cast* is evicted and this one queued (ASX_OK), counted by
 * asx_actor_evicted_count and traced "gen_server::cast_evicted_oldest" on
 * the calling task. Queued calls are never evicted: a mailbox of calls
 * and reservations only, or with a waiting sender ahead, still gives
 * ASX_E_CHANNEL_FULL. Only try_cast applies the policy; asx_actor_cast
 * waits for room. With the lock-free channel backend call it from the
 * scheduler thread (the eviction is a consumer-side operation). */
ASX_API ASX_MUST_USE asx_status asx_actor_try_cast(asx_actor_handle actor, uint64_t msg);

/* Casts evicted under ASX_ACTOR_OVERFLOW_DROP_OLDEST (Rust
 * GenServerHandle::evicted_count, :1649). 0 for a stale handle. */
ASX_API uint64_t asx_actor_evicted_count(asx_actor_handle actor);

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
 * Names (Rust cx/registry.rs NameRegistry and NameLease;
 * gen_server.rs spawn_named_gen_server, NamedGenServerHandle)
 *
 * A registry is an object the caller owns, as the `&mut NameRegistry` Rust
 * passes in; the runtime keeps none. It maps a name to the task holding it.
 * A registration returns a lease, which stands for the name's ownership
 * (Rust: a drop-bomb obligation token): it is resolved once, by release or
 * abort, and resolving it does not touch the registry; the entry is freed
 * by asx_name_unregister_owned (or a cleanup). Ported: registration with
 * Rust's default collision policy (a taken name is refused), whereis,
 * unregister, cleanup by region or task, and the named server. Not ported:
 * the Wait and Replace collision policies and their waiter grants, name
 * watchers and ownership notifications, and reserve/commit permits. The
 * registry is bounded: Rust's is a HashMap.
 * ------------------------------------------------------------------- */

#ifndef ASX_NAME_REGISTRY_CAPACITY
#define ASX_NAME_REGISTRY_CAPACITY 16u
#endif
#ifndef ASX_NAME_MAX_LEN
#define ASX_NAME_MAX_LEN 63u
#endif

typedef struct {
    char name[ASX_NAME_MAX_LEN + 1u];
    asx_task_id holder;
    asx_region_id region;
    asx_time acquired_at;
    int in_use;
} asx_name_entry;

typedef struct {
    asx_name_entry entries[ASX_NAME_REGISTRY_CAPACITY];
    uint32_t count;
} asx_name_registry;

/* Rust NameLease (registry.rs:106): holder, region and acquisition time
 * identify the registration; active until released or aborted. */
typedef struct {
    char name[ASX_NAME_MAX_LEN + 1u];
    asx_task_id holder;
    asx_region_id region;
    asx_time acquired_at;
    int active;
} asx_name_lease;

/* Rust NamedGenServerHandle (gen_server.rs:2617): a server and the lease of
 * its name. */
typedef struct {
    asx_actor_handle actor;
    asx_name_lease lease;
} asx_named_actor;

/* An empty registry. ASX_E_INVALID_ARGUMENT for NULL. */
ASX_API asx_status asx_name_registry_init(asx_name_registry *registry);

/* `registry.register(name, holder, region, now)` (registry.rs:802).
 * ASX_OK writes the lease. ASX_E_NAME_CONFLICT when the name is held (the
 * holder is written to *current_holder when it is not NULL, as
 * NameTaken's `current_holder`), ASX_E_INVALID_ARGUMENT for a NULL
 * argument, a name over ASX_NAME_MAX_LEN, or a holder in a root region
 * (Rust's lease mint panics there, [ASUP-E103], with the registry
 * unchanged), ASX_E_RESOURCE_EXHAUSTED when the registry is full. Every
 * refusal changes nothing. */
ASX_API ASX_MUST_USE asx_status asx_name_register(asx_name_registry *registry, const char *name,
                                                  asx_task_id holder, asx_region_id region,
                                                  asx_time now, asx_name_lease *out_lease,
                                                  asx_task_id *current_holder);

/* `registry.whereis(name)` (:1471): ASX_OK and the holder, or
 * ASX_E_NOT_FOUND. */
ASX_API ASX_MUST_USE asx_status asx_name_whereis(const asx_name_registry *registry,
                                                 const char *name, asx_task_id *out_holder);

/* `registry.unregister(name, caller)` (:1173): the name's holder frees it.
 * ASX_E_NOT_FOUND, or ASX_E_PERMISSION_DENIED for another caller. */
ASX_API ASX_MUST_USE asx_status asx_name_unregister(asx_name_registry *registry, const char *name,
                                                    asx_task_id caller);

/* `registry.unregister_owned_and_grant(&lease, now)` (:1317): frees the
 * entry the lease names, if its holder, region and acquisition time are the
 * lease's. ASX_E_NOT_FOUND, or ASX_E_PERMISSION_DENIED when the entry is
 * someone else's. (There are no waiters to grant to.) */
ASX_API ASX_MUST_USE asx_status asx_name_unregister_owned(asx_name_registry *registry,
                                                          const asx_name_lease *lease,
                                                          asx_time now);

/* `registry.cleanup_region(region)` / `cleanup_task(task)` (:1508, :1581):
 * drop every entry the region's or the task's holders have; the number
 * dropped. Leases are not touched. */
ASX_API uint32_t asx_name_cleanup_region(asx_name_registry *registry, asx_region_id region);
ASX_API uint32_t asx_name_cleanup_task(asx_name_registry *registry, asx_task_id task);

/* Entries held (Rust len()). */
ASX_API uint32_t asx_name_registry_count(const asx_name_registry *registry);

/* `lease.is_active()`, `lease.release()` (commit) and `lease.abort()`
 * (registry.rs:186-218). A resolved lease gives
 * ASX_E_OBLIGATION_ALREADY_RESOLVED. */
ASX_API int asx_name_lease_is_active(const asx_name_lease *lease);
ASX_API ASX_MUST_USE asx_status asx_name_lease_release(asx_name_lease *lease);
ASX_API ASX_MUST_USE asx_status asx_name_lease_abort(asx_name_lease *lease);

/* `scope.spawn_named_gen_server(state, cx, registry, name, server, cap,
 * now)` (gen_server.rs:2382): the server, then its name registered to its
 * task and region. A taken name refuses with ASX_E_NAME_CONFLICT and no
 * server exists: Rust creates and discards the never-scheduled task, which
 * leaves nothing a run could see, so C checks the name first and spawns
 * nothing. (A spawn that both conflicts and would be refused therefore
 * reports the conflict, where Rust reports the spawn error.) Other errors
 * are asx_actor_spawn's, plus ASX_E_RESOURCE_EXHAUSTED for a full registry
 * and ASX_E_INVALID_ARGUMENT for a root region (the lease cannot be minted
 * there); a spawn the lab refuses at once registers nothing and returns
 * ASX_E_REGION_CLOSED. */
ASX_API ASX_MUST_USE asx_status asx_actor_spawn_named(
    asx_named_actor *out, asx_name_registry *registry, const char *name, asx_region_id region,
    const asx_actor_behavior *behavior, void *state, uint32_t mailbox_capacity, asx_time now);

/* `named.release_name(&mut registry, now)` (:2670): after the server has
 * finished, free the name and commit the lease. ASX_E_TASKS_STILL_ACTIVE
 * while the server runs (Rust StillRunning; the lease stays active),
 * ASX_E_OBLIGATION_ALREADY_RESOLVED for a resolved lease. When the
 * registry no longer holds the lease's entry (ASX_E_NOT_FOUND, or
 * ASX_E_PERMISSION_DENIED after another registration) the lease is aborted
 * and that status returned. */
ASX_API ASX_MUST_USE asx_status asx_named_actor_release_name(asx_named_actor *named,
                                                             asx_name_registry *registry,
                                                             asx_time now);

/* `named.abort_lease(&mut registry, now)` (:2712): roll the registration
 * back without stopping the server. Statuses as release_name, without the
 * running check. */
ASX_API ASX_MUST_USE asx_status asx_named_actor_abort_lease(asx_named_actor *named,
                                                            asx_name_registry *registry,
                                                            asx_time now);

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
