/*
 * asx/monitor/monitor.h — task monitors and DOWN notifications
 *
 * Port of asupersync's monitor module (src/monitor.rs). A task, or a
 * GenServer, watches another task and is told exactly once when it finishes,
 * for any reason, with the finished task's outcome (Rust's DownReason) and
 * the virtual time of its completion.
 *
 * Rust contracts kept here:
 *   DOWN-ORDER   notifications of one exit are delivered by ascending
 *                monitor reference (one exit shares the completion time and
 *                the monitored task, so that is the whole sort key);
 *   DOWN-CONTENT a notification carries the monitored task, its outcome and
 *                the reference establish returned;
 *   a watcher's own monitors end with it and generate nothing;
 *   the same watcher may monitor one task more than once: each monitor has
 *   its own reference and produces its own notification.
 *
 * Establishing and firing are separate steps, as Rust's are: a monitor is
 * registered when the runtime applies the request (asx_monitor_establish*),
 * and the runtime fires it when the task completes
 * (asx_monitor_task_finished).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ASX_MONITOR_MONITOR_H
#define ASX_MONITOR_MONITOR_H

#include <asx/actor/actor.h>
#include <asx/asx_export.h>
#include <asx/asx_ids.h>
#include <asx/asx_status.h>
#include <asx/core/outcome.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monitors that exist at once: active ones, plus fired ones whose handle has
 * not been released. Rust's set is unbounded; establishing past this is
 * ASX_E_RESOURCE_EXHAUSTED and drops nothing. It is also a bound on a
 * server's system lane: a server holds at most this many monitors, each
 * fires once, so a lane at least this large never loses a DOWN. */
#ifndef ASX_MONITOR_CAPACITY
#define ASX_MONITOR_CAPACITY 16u
#endif
#if (ASX_MONITOR_CAPACITY) < 1
#error "ASX_MONITOR_CAPACITY must be at least 1"
#endif
#if (ASX_MONITOR_CAPACITY) > (ASX_ACTOR_SYSTEM_LANE_CAPACITY)
#error "ASX_MONITOR_CAPACITY must not exceed ASX_ACTOR_SYSTEM_LANE_CAPACITY"
#endif

/* Rust MonitorRef: unique within a runtime, allocated from 1. 0 is never a
 * reference. */
typedef uint64_t asx_monitor_ref;

/* Rust DownNotification plus the virtual time of the exit behind it
 * (SystemMsg::Down's completion_vt). `reason` is the monitored task's
 * outcome: OK is DownReason::Normal, ERR is Error (its text is not carried),
 * CANCELLED carries the cancel reason, PANICKED the panic message. */
typedef struct {
    asx_task_id monitored;
    asx_outcome reason;
    asx_time vt;
    asx_monitor_ref ref;
} asx_down;

/* Apply a monitor request whose notification the caller polls for: `watcher`
 * will have one DOWN for `target` available through asx_monitor_down.
 * Writes the reference to *out_ref. A monitor on a task that is not live
 * (finished, or never existed) is ASX_E_NOT_FOUND (Rust WatchError::
 * NotFound); a watcher that is not live is ASX_E_STALE_HANDLE (Rust
 * WatcherNotFound); ASX_E_RESOURCE_EXHAUSTED when ASX_MONITOR_CAPACITY
 * monitors exist; ASX_E_INVALID_ARGUMENT for a NULL out_ref.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_monitor_establish(asx_task_id watcher, asx_task_id target,
                                                      asx_monitor_ref *out_ref);

/* Apply a monitor request held by a GenServer (Rust GenServerHandle::
 * monitor): the server's task is the watcher, and when `target` finishes the
 * server's handle_info receives one DOWN message through its system lane.
 * Statuses as asx_monitor_establish; a stale or stopped server handle is
 * ASX_E_STALE_HANDLE.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_monitor_establish_server(asx_actor_handle server,
                                                             asx_task_id target,
                                                             asx_monitor_ref *out_ref);

/* Remove a monitor (Rust demonitor, Monitor drop): no DOWN for it is
 * delivered afterwards. A reference that is unknown, already fired for a
 * server, or already removed is ignored.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_demonitor(asx_monitor_ref ref);

/* The DOWN notification of a monitor made with asx_monitor_establish (Rust
 * Monitor::try_down / down). ASX_OK with *out_down filled once the monitored
 * task has finished, and again on every later call until the monitor is
 * released; ASX_E_PENDING while it has not; ASX_E_NOT_FOUND for a reference
 * that is unknown, released, a server's, or that ended with its watcher.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API ASX_MUST_USE asx_status asx_monitor_down(asx_monitor_ref ref, asx_down *out_down);

/* Release a monitor made with asx_monitor_establish (Rust drops the
 * Monitor): one still waiting is removed and never fires, one that fired
 * frees its capacity. Unknown references are ignored.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_monitor_release(asx_monitor_ref ref);

/* The runtime's side of a task's exit (Rust TaskWatches::on_task_completed,
 * monitors only): `task` finished with `outcome` at virtual time `vt`. Every
 * monitor on it fires, by ascending reference: a server's receives a DOWN
 * message (dropped, as Rust's closed lane drops it, when the server has
 * stopped), a polled monitor stores its notification. Then the monitors the
 * task itself held end silently. A NULL outcome is treated as an error exit,
 * as Rust treats a task that finished without a recorded outcome.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_monitor_task_finished(asx_task_id task, const asx_outcome *outcome, asx_time vt);

/* The number of monitors that exist: waiting ones and unreleased fired ones.
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API uint32_t asx_monitor_count(void);

/* Forget every monitor and restart references at 1 (the runtime's reset).
 * Thread-safety: not thread-safe; single-threaded mode only. */
ASX_API void asx_monitor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ASX_MONITOR_MONITOR_H */
