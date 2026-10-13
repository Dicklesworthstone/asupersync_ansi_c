/*
 * asx/link/link.h — task links and exit signals
 *
 * Port of asupersync's link module (src/link.rs). Two tasks are linked: when
 * one of them exits abnormally the other is cancelled with a LinkedExit
 * reason, or, if it holds the link as trapping, is told with an exit signal
 * and keeps running. A normal exit removes the link silently, except that a
 * trapping task also hears of it (as in OTP).
 *
 * Rust contracts kept here:
 *   EXIT-ORDER   the actions of one exit are applied by (exit time, the
 *                exiting task, the peer, the link reference), which for one
 *                exit is peer then reference; the runtime's lab orders peers
 *                by their Rust arena index and generation;
 *   a link is held by the task that made it: only that task is ever sent an
 *                exit signal; the peer's side always propagates;
 *   a task's links end with it, and a self-link is one edge;
 *   a LinkedExit cancel's reason carries the message "link exit", and as
 *                its cause the exiting task's cancel reason when it was
 *                cancelled.
 *
 * Establishing and firing are separate steps, as for monitors (asx/monitor/
 * monitor.h): a link is registered when the runtime applies the request
 * (asx_link_establish*), and the runtime fires it when a task completes
 * (asx_link_task_finished).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_LINK_LINK_H
#define ASX_LINK_LINK_H

#include <asx/actor/actor.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>
#include <asx/monitor/monitor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Links that exist at once: active ones, plus polled ones whose exit signal
 * is stored and not released. Rust's set is unbounded; establishing past
 * this is ASX_E_RESOURCE_EXHAUSTED and drops nothing. Together with
 * ASX_MONITOR_CAPACITY it bounds a server's system lane: a server holds at
 * most that many monitors and links, each fires once, so a lane at least
 * their sum never loses a message. */
#ifndef ASX_LINK_CAPACITY
#define ASX_LINK_CAPACITY 8u
#endif
#if (ASX_LINK_CAPACITY) < 1
#error "ASX_LINK_CAPACITY must be at least 1"
#endif
#if (ASX_MONITOR_CAPACITY) + (ASX_LINK_CAPACITY) > (ASX_ACTOR_SYSTEM_LANE_CAPACITY)
#error "ASX_MONITOR_CAPACITY + ASX_LINK_CAPACITY must not exceed ASX_ACTOR_SYSTEM_LANE_CAPACITY"
#endif

/* Rust LinkRef: allocated from 1. 0 is never a reference. (Rust's counter is
 * process-wide; this one restarts with each runtime reset.) */
typedef uint64_t asx_link_ref;

/* Rust ExitPolicy of the side that holds the link. Propagate cancels the
 * holder when its peer exits abnormally; Trap delivers an exit signal
 * instead and the holder keeps running. The peer's side always propagates
 * (WatchCommand::Link, monitor.rs:1366). Rust's third policy, Ignore, is
 * not reachable through the runtime's requests and is not offered. */
typedef enum { ASX_EXIT_PROPAGATE = 0, ASX_EXIT_TRAP = 1 } asx_exit_policy;

/* Rust ExitSignal plus the virtual time of the exit behind it
 * (SystemMsg::Exit's exit_vt). `reason` is the exiting task's outcome as
 * for asx_down: OK is Normal (only a trapping holder hears of it). */
typedef struct {
    asx_task_id from;
    asx_outcome reason;
    asx_time vt;
    asx_link_ref ref;
} asx_exit_signal;

/* Apply a link request whose exit signal, if the link traps, the caller
 * polls for: `task` holds a link to `peer` under `policy`. Writes the
 * reference to *out_ref. A peer that is not live is ASX_E_NOT_FOUND (Rust
 * WatchError::NotFound); a holder that is not live is ASX_E_STALE_HANDLE
 * (WatcherNotFound); ASX_E_RESOURCE_EXHAUSTED when ASX_LINK_CAPACITY links
 * exist; ASX_E_INVALID_ARGUMENT for a NULL out_ref or an unknown policy.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_link_establish(asx_task_id task, asx_task_id peer,
                                                   asx_exit_policy policy, asx_link_ref *out_ref);

/* Apply a link request held by a GenServer (Rust GenServerHandle::link /
 * link_trapping): the server's task is the holder, and a trapped exit
 * reaches its handle_info as an EXIT message through its system lane.
 * Statuses as asx_link_establish; a stale or stopped server handle is
 * ASX_E_STALE_HANDLE.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_link_establish_server(asx_actor_handle server, asx_task_id peer,
                                                          asx_exit_policy policy,
                                                          asx_link_ref *out_ref);

/* Cx::link / Cx::link_trapping (Rust LinkOpening), polled by task `self`
 * until it stops returning ASX_E_PENDING: under lab dispatch the request is
 * a command of the region-command lane, applied by the next step's drain
 * (asx_link_establish's checks and statuses); without it the request is
 * applied at once. *out_ref is written on ASX_OK.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_link_open_poll(asx_task_id self, asx_task_id peer,
                                                   asx_exit_policy policy, asx_link_ref *out_ref);

/* GenServerHandle::link / link_trapping: as asx_link_open_poll, for the
 * server to hold the link; `self` is the task awaiting the opening.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_link_open_server_poll(asx_task_id self, asx_actor_handle server,
                                                          asx_task_id peer, asx_exit_policy policy,
                                                          asx_link_ref *out_ref);

/* Link::exit, polled by task `self` until it stops returning ASX_E_PENDING
 * (a trapping link made with asx_link_open_poll): ASX_OK with *out_signal
 * once the peer has exited; ASX_E_CANCELLED if `cx` is cancelled first
 * (checked after the stored signal); otherwise the task parks and the exit
 * wakes it. ASX_E_NOT_FOUND for a reference that is unknown, released, a
 * server's, or not trapping.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_link_exit_poll(asx_task_id self, asx_cx *cx, asx_link_ref ref,
                                                   asx_exit_signal *out_signal);

/* Remove a link (Rust Link::unlink, GenServerHandle::unlink): its peers are
 * no longer cancelled or signalled by it. Under lab dispatch the removal is
 * a command of the region-command lane, applied by the next drain;
 * otherwise it is immediate. An unknown or already removed reference is
 * ignored.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_unlink(asx_link_ref ref);

/* Release a link made with asx_link_open_poll (Rust drops the Link): a
 * stored exit signal frees its capacity. It does not remove the link: only
 * Link::unlink does, so the link keeps acting until a task exits.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_link_release(asx_link_ref ref);

/* The runtime's side of a task's exit (Rust TaskWatches::on_task_completed,
 * links only; monitors fire first, with asx_monitor_task_finished): `task`
 * finished with `outcome` at virtual time `vt`. A normal exit signals the
 * trapping holders that are its peers; an abnormal one cancels each
 * propagating peer with a LinkedExit reason, in peer then reference order,
 * and signals each trapping holder, signals first. Then every link the task
 * had ends. A NULL outcome is an error exit.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_link_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt);

/* The number of links that exist: active ones and unreleased stored signals.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_link_count(void);

/* Forget every link and restart references at 1 (the runtime's reset).
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_link_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_LINK_LINK_H */
