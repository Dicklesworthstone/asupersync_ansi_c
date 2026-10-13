# Lab oracle catalog (bd-9kll.9.2)

Rust's `LabRuntime` can check structural invariants of a run with *oracles*.
This document maps every oracle the pinned Rust registry knows
(asupersync `5e60b1c4c`, `src/lab/oracle/registry.rs`, `ORACLE_DESCRIPTORS`)
to what the C port has, says where the C check runs and when, and lists the
gap. It is a map made by reading, not by running: every `path::symbol` and
test name below was found in the tree with `rg`; none was executed for this
document. Citations are by symbol, so they do not move with the code.

## 1. What the Rust registry holds

| | Count | Names |
|---|---|---|
| Descriptors in the registry | 29 | all of section 4 |
| Emitted by `OracleSuite::report` with no extra feature | 24 | everything except the next two lines |
| `priority_inversion` | 1 | opt-in, manual instrumentation, not in the report |
| `fabric_*` | 4 | feature `messaging-fabric` |
| **Fed by `LabRuntime`** (`LAB_RUNTIME_FED_ORACLE_NAMES`) | **11** | `task_leak`, `obligation_leak`, `quiescence`, `loser_drain`, `finalizer`, `region_tree`, `deadline_monotone`, `cancellation_protocol`, `down_order`, `supervision`, `reply_linearity` |

The last row matters most. The registry itself says that the other reported
oracles "observe only what a caller feeds them by hand, so in a `LabRuntime`
report they pass without having checked anything"
(`br-asupersync-52hxjz`). A lab run therefore verifies exactly those 11. An
oracle outside the 11 is catalogued here, but a C port of it has no Rust
verdict to be compared with unless a caller feeds Rust's oracle by hand.

The bead's "24" is the count of `OracleSuite::report` entries; the registry has
29 descriptors.

## 2. What the C port has today

- **The suite.** `src/runtime/replay.c::asx_oracle_suite_run` runs up to
  `ASX_ORACLE_SUITE_MAX` (8) functions of type `asx_oracle_fn(const asx_lab *,
  void *ctx)` once, after a run, and returns pass / fail / inconclusive with a
  static message. There is no per-step hook and no entity ids in a verdict
  (`detail_count` is never filled).
- **Three built-ins.** `asx_oracle_quiescence` (any live task not completed
  fails), `asx_oracle_leak` (any live region not closed fails; its header used
  to say obligations too, which the code never scanned, and now says regions
  only: a closed region that holds a reserved obligation is
  `obligation_leak`'s) and `asx_oracle_replay_match` (two runs agree). Only
  their PASS and null-lab paths are unit-tested
  (`tests/unit/runtime/test_replay.c`).
- **Kernel-state oracles** (step 2, `src/runtime/oracle.c`, tests
  `tests/unit/runtime/test_oracle.c`): `task_leak`, `obligation_leak`,
  `quiescence`, `finalizer`, `region_tree`, `deadline_monotone`,
  `cancellation_protocol` (the region part), `cancel_correctness`,
  `cancel_debt`, `region_leak` (the part that needs no clock),
  `cancel_signal_ordering` (orphaned and out-of-order propagated cancels) and
  `channel_atomicity` (a channel's counters against its permits and senders),
  `down_order` (the monitor module's delivery log), `reply_linearity`
  (obligations resolved exactly once, from the trace ring) and `loser_drain`
  (a race loser that outlived its collector), by Rust's registry names, and
  two with no Rust twin:
  `task_counters` (a region's live-task counter against its tasks, the ground
  under `task_leak` and `quiescence`) and `arena_counters` (the kernel's live
  counts and free lists against the arenas). Each is one read-only pass over the
  arenas, run once at the end of a run: `asx_oracle_*(lab, ctx)` for the lab
  suite, `asx_oracle_check_named(name)` for a caller without a lab, and the
  conformance interpreter's `lab.oracles` / `ASX_ORACLES` (off by default, so
  the corpus gates nothing until it has been measured). Each has a planted
  violation in the unit test. They are a regression net for what the kernel
  enforces by construction: a FAIL is a kernel bug.
- **The kernel enforces most invariants by construction.** Regions cannot
  close with live tasks, children, pending cleanup or reserved obligations
  (`src/runtime/quiescence.c::asx_region_finalize_one`), a child's budget is
  the meet with its parent's, task states move only through the transition
  table, and a region cancel walks its subtree parent first. These are why most
  rows below are PRESENT, and why the oracles exist as a *regression net* for
  them, not as the thing that makes them true.
- **Ghost monitors** (`src/core/ghost.c`, `ASX_DEBUG_GHOST`) record illegal
  region, task and obligation transitions, double resolves and leaks. They are
  debug-only and are asserted clean only by `make conformance-ghost`
  (`tools/conformance/runner.c::ghost_clean`).
- **The lab step hook** (cc52020): `asx_lab_set_futurelock_check` is the only
  per-step check, and it emits a trace event, not a verdict.

## 3. Status vocabulary and totals

- **PRESENT**: the C kernel enforces or checks the same invariant, at runtime
  or by construction (the row says which). An independent oracle may still be
  missing; that is in the gap column.
- **PARTIAL**: part of the invariant is checked.
- **MISSING**: C has the concept and no check.
- **N/A**: the Rust concept does not exist in C (the row says why).

| Status | Count |
|---|---|
| PRESENT | 14 |
| PARTIAL | 8 |
| MISSING | 1 |
| N/A | 6 |

Of the 11 oracles `LabRuntime` feeds, 10 are PRESENT and 1 (`region_tree`) is
PARTIAL. Ten of the 11 (`task_leak`, `obligation_leak`, `quiescence`,
`finalizer`, `region_tree`, `deadline_monotone`, `cancellation_protocol`,
`loser_drain`, `down_order`, `reply_linearity`) now also have an independent
end-of-run check that can fail a run with the oracle's name (section 2).
One has none: `supervision`, whose evidence is the controller's actions as
free-form user-trace text, of which the trace ring keeps only a hash.
Nothing is checked per step.

## 4. The catalog

"Fed" is whether `LabRuntime` feeds the oracle (section 1). Tests are
`path::name`, the name being the `TEST()` argument.

### 4.1 Fed by `LabRuntime` (the 11 a lab run actually verifies)

| Oracle | Rust statement | C | C mechanism and when | Test | Gap |
|---|---|---|---|---|---|
| `task_leak` | structured concurrency: every task is owned and completed before region close | PRESENT | `src/runtime/quiescence.c::asx_region_finalize_one` never sets CLOSED while the region has live tasks (`ASX_E_QUIESCENCE_TASKS_LIVE`): by construction at every close. `src/runtime/replay.c::asx_oracle_quiescence` scans the task arena at the end of a run; `src/runtime/oracle.c::asx_oracle_task_leak` checks every closed region for a live task. | `tests/unit/runtime/test_quiescence.c::quiescence_closed_region_with_live_tasks_reports_tasks_live`, `tests/unit/runtime/test_oracle.c::task_leak_names_a_closed_region_with_a_live_task` | The verdict counts violating regions and names none; no spawn/complete/close pairing. |
| `obligation_leak` | no obligation leaks: permits, acks, and leases resolve before close | PRESENT | `src/runtime/lifecycle.c::asx_task_resolve_held_obligations_internal` at holder completion and `::asx_region_audit_leaks_internal` at FINALIZING, before CLOSED, under the leak policy (PANIC refuses to close); ghost `LINEARITY_LEAK` in debug builds; `src/runtime/oracle.c::asx_oracle_obligation_leak` checks every closed region for a reserved obligation and every reserved obligation for a holder that completed or is gone. | `tests/unit/runtime/test_budget_obligation.c::region_close_audit_leaks_obligations_still_reserved`, `tests/unit/runtime/test_oracle.c::obligation_leak_names_a_closed_region_with_a_reserved_obligation`, `::obligation_leak_names_a_reserved_obligation_whose_holder_completed` | Policy-driven: LOG and RECOVER do not fail the run, a caller must assert the leak count. Name leases and untracked permits are not in the ledger. |
| `quiescence` | region close => quiescence | PRESENT | `asx_region_finalize_one` (CLOSED only with no children, drained cleanup stack, no tasks, then the leak audit); `src/runtime/quiescence.c::asx_quiescence_check_detailed` gives Q1-Q4 on request; `src/runtime/rt.c::asx_runtime_is_quiescent` for the runtime; `src/runtime/oracle.c::asx_oracle_region_quiescence` checks every closed region for tasks, children, reserved obligations and pending cleanup at the end of a run. | `tests/unit/runtime/test_quiescence.c::quiescence_closed_region_with_unresolved_obligation_reports_error`, `tests/unit/runtime/test_oracle.c::quiescence_names_a_closed_region_with_a_child_that_is_not_closed` | Q1-Q4 are evaluated when asked, not recorded at each close or checked per step; a violation gives counts, not ids. |
| `loser_drain` | race losers are canceled and fully drained | PRESENT | `src/runtime/task_group.c::asx_task_group_poll`: any decision moves the group to DRAINING and DONE is reached only when every loser is cancelled and joined; by construction for every task-group race. `src/runtime/oracle.c::asx_oracle_loser_drain` fails a live task cancelled with RACE_LOST whose completion watcher (the group's owner, which registers on each member while it collects them) has completed or is gone. | `tests/unit/runtime/test_task_group.c::race_cancels_and_drains_losers_before_resolving`, `tests/unit/runtime/test_oracle.c::loser_drain_names_a_race_loser_that_outlived_the_task_that_raced_it` | The inline `asx_race` (`src/core/combinator.c`) drops losers undrained, as Rust's inline `Cx::race` does, and a task joined by another task is a watcher too, so only a RACE_LOST-cancelled task is read. No race start/complete record: a loser whose owner is still running is not a violation. |
| `finalizer` | registered finalizers run before close completes | PRESENT | `asx_region_finalize_one` waits for the cleanup stack to empty (every entry, sync or async, runs as a masked cleanup task) before the leak audit and CLOSED; `asx_quiescence_check` reports `ASX_E_QUIESCENCE_NOT_REACHED` with cleanup pending; `src/runtime/oracle.c::asx_oracle_finalizer` fails a closed region with cleanup still pending. | `tests/unit/runtime/test_quiescence.c::defer_sync_finalizers_run_once_in_lifo_order_at_close`, `tests/unit/runtime/test_oracle.c::finalizer_names_a_closed_region_with_cleanup_pending` | Finalizers have no id and emit no register/run events, so "registered set vs ran set" cannot be checked after the fact. |
| `region_tree` | regions form one rooted ownership tree | PARTIAL | `src/runtime/lifecycle.c::asx_region_open_child` (parent must exist and be OPEN; child appended, `parent_id` set) and `src/runtime/quiescence.c::asx_region_unlink_from_parent`: links and acyclicity hold by construction. `src/runtime/oracle.c::asx_oracle_region_tree` scans the arena at the end of a run: one root, a live parent that lists each region, each listed child live and naming its parent, no cycle. | `tests/unit/runtime/test_quiescence.c::region_open_child_sets_parent_and_tracks_child`, `tests/unit/runtime/test_oracle.c::region_tree_names_a_second_root_and_an_unlisted_child` | Rust's exactly-one-root rule is not a kernel rule (`asx_region_open` mints a parentless root on every call), so a program that opens two roots fails this oracle where Rust's single-root runtime could not have; the lab interpreter opens one. |
| `deadline_monotone` | child deadlines are no looser than parent deadlines | PRESENT | `asx_region_open_child` copies the parent budget and `::asx_region_open_child_with_budget` applies `src/core/budget.c::asx_budget_meet` (minimum deadline): by construction at region open; no region budget is written after open; `src/runtime/oracle.c::asx_oracle_deadline_monotone` scans every region against its parent at the end of a run. | `tests/unit/runtime/test_budget_obligation.c::budgets_tighten_down_the_region_tree`, `tests/unit/runtime/test_oracle.c::deadline_monotone_names_a_child_with_a_later_deadline_than_its_parent` | No budget-update event; the scan sees the end state only, so a deadline that was loosened and tightened again between steps is not seen. |
| `cancellation_protocol` | cancellation follows request, drain, finalize | PRESENT | `src/core/transition_tables.c::asx_task_transition_check` applied on every task state write by `src/runtime/lifecycle.c::asx_task_set_state_internal` in every build; `src/runtime/cancellation.c::asx_checkpoint` acknowledges only at mask depth 0, masks are bounded, `::asx_region_cancel` propagates parent first; `src/runtime/oracle.c::asx_oracle_cancel_propagation` fails a cancelled region with a live, not closed child that has no cancel requested (the region part; the task phase order is the transition table's). | `tests/unit/runtime/test_cancellation.c::task_state_write_applies_the_transition_table_in_every_build`, `tests/unit/runtime/test_oracle.c::cancellation_protocol_names_a_child_a_region_cancel_did_not_reach` | No bound on request -> acknowledge -> complete: C, like Rust, never force-completes an unacknowledged cancel (the hard bound is opt-in); `cancel_debt` reports the lost-wake case only. |
| `down_order` | DOWN messages are delivered in deterministic order | PRESENT | `src/monitor/monitor.c::asx_monitor_task_finished` fires a finished task's monitors lowest reference first, one completion at a time; by construction at each completion. The module keeps the last `ASX_MONITOR_CAPACITY` deliveries in firing order (`asx_monitor_delivery_get_internal`), and `src/runtime/oracle.c::asx_oracle_down_order` fails a delivery with an earlier completion time than the one before it, or a monitor of the same exit not above the reference before it. | `tests/unit/monitor/test_monitor.c::test_downs_are_delivered_by_ascending_reference`, `tests/unit/runtime/test_oracle.c::down_order_names_monitors_fired_out_of_order` | The log holds the last `ASX_MONITOR_CAPACITY` deliveries, so an older violation is overwritten; the order across exits at one completion time is the completion order, which the oracle does not know (Rust sorts by task id too). The unit test plants the violation in the values the check is a function of (the log is module-static). |
| `supervision` | supervision restarts and escalations follow policy | PRESENT | `src/actor/supervisor.c`: the controller state machine enforces restart mode, `max_restarts` within a window, backoff, the three sibling policies and escalation, in the one place that restarts. | `tests/unit/actor/test_supervisor.c::test_rest_for_one_restarts_the_later_ones` | No independent observer comparing failure, restart and escalation events with the declared policy; the controller's trace is free-form user-trace text. |
| `reply_linearity` | Spork replies resolve exactly once | PRESENT | A call reply is a oneshot permit reserved with the caller's `Cx` (a `SEND_PERMIT` obligation); the actor loop always sends or aborts it, including on handler failure, drain and cell drop (`src/actor/actor.c`): structural exactly-once, a leak shows in the ledger. `src/runtime/oracle.c::asx_oracle_reply_linearity` reads the trace ring's obligation events: an obligation (a reply is one of kind SEND_PERMIT) resolved by a second commit, abort or leak, or resolved with no reservation before it (checked while the ring dropped nothing). | `tests/unit/actor/test_actor.c::test_cancelled_server_drops_queued_call`, `tests/unit/runtime/test_oracle.c::reply_linearity_names_an_obligation_resolved_twice_or_never_reserved` | No reply-specific obligation kind in the ring (the kind is in the event's payload, which the ring does not keep), so every obligation is read, which is the superset; the API refuses a second resolution and records nothing, so the unit test plants the second event by hand. |

### 4.2 Reported by Rust but not fed by `LabRuntime` (13)

| Oracle | Rust statement | C | C mechanism and when | Test | Gap |
|---|---|---|---|---|---|
| `region_leak` | regions do not remain stuck or leaked past lifecycle bounds | PARTIAL | `asx_oracle_leak` (end of run: a live region not CLOSED fails); `src/runtime/lab_dispatch.c::asx_lab_futurelocks` / `asx_scheduler_stuck_tasks` (a stuck obligation holder, a run that ended WOULD_BLOCK); `src/runtime/oracle.c::asx_oracle_region_leak`: a region in CLOSING, DRAINING or FINALIZING with no live task, no child not closed, no cleanup pending or running, no async finalizer and no reserved obligation has nothing to wait for and would never close (a missed `asx_region_advance_internal`). | `tests/unit/runtime/test_lab_dispatch.c::a_parked_obligation_holder_is_a_futurelock_past_the_threshold`, `tests/unit/runtime/test_oracle.c::region_leak_names_a_closing_region_with_nothing_left_to_wait_for` | Rust's thresholds are wall-clock (a region Created, Closing or Finalizing longer than a limit, a task never polled); state has no clock, so a region that is merely slow is not seen, and an OPEN region left open is not a leak here (`asx_oracle_leak` reports it). |
| `ambient_authority` | effects require explicit Cx capabilities | PARTIAL | Runtime: capability bits checked at the `Cx`-taking APIs (`src/cx/cx.c::asx_cx_now`, `::asx_cx_random_u64`, `src/cx/scope.c::asx_scope_init`; narrowing refuses escalation). Static: `tools/ci/check_ambient_authority.py` against the catalog in `src/security/audit.c`. | `tests/unit/cx/test_cx.c::narrow_escalation_fails` | No effect/capability event stream, so "effect without grant" is never recorded; the kernel's raw APIs take no `Cx` by design; the lint covers only clock and entropy. |
| `cancel_correctness` | cancel-correctness witnesses start from valid initial states | PARTIAL | `src/core/cancel.c::asx_cancel_witness_validate_initial`, `::asx_cancel_witness_validate_transition` and `::asx_cancel_witness_advance` exist and match Rust's validators; the runtime creates and advances a witness per cancelled task. `src/runtime/oracle.c::asx_oracle_cancel_correctness` reads every task's active witness at the end of a run: non-zero epoch, the task's own id, region and epoch, a phase not ahead of the task's, none held by a completed task. | `tests/unit/runtime/test_oracle.c::cancel_correctness_names_a_witness_that_is_not_its_tasks` | The runtime still discards the `advance` status, and a single witness per task has no history, so `validate_transition` between two successive witnesses is never run; a witness that lags its task's phase is not reported. |
| `cancel_debt` | cancellation debt remains bounded and drains | PARTIAL | `src/runtime/oracle.c::asx_oracle_cancel_debt`: under lab dispatch a task with a request it has not acknowledged, not polled since, not masked, not scheduled and with no timer is a lost wake. Counterparts: per-kind cleanup budgets and the on-request `asx_region_drain_progress`. | `tests/unit/runtime/test_oracle.c::cancel_debt_names_a_requested_cancel_that_nothing_will_run` | No queue depth, arrival-vs-completion or overdue-cleanup accounting and no threshold; without lab dispatch the oracle does not look (the sweep's idle handling unparks a cancelled task). |
| `cancel_signal_ordering` | cancel signal order is deterministic and causally valid | PRESENT | `src/runtime/cancellation.c::asx_region_cancel` walks `asx_region_subtree_internal` parent first, emits `ASX_TRACE_REGION_CANCELLED` per region in that order and cancels every live task with its region's reason; `::asx_cancel_propagate` for tasks. By construction per cancel. `src/runtime/oracle.c::asx_oracle_cancel_signal_ordering`: a region whose reason is a propagated ParentCancelled (kind PARENT with a cause) has a parent with a cancel requested, and its first region.cancelled event follows its parent's in the trace ring (skipped when the ring dropped events); the region part of "no uncancelled child" is `cancellation_protocol`'s. | `tests/unit/runtime/test_cancellation.c::region_cancel_closes_subtree_and_chains_parent_reasons`, `tests/unit/runtime/test_oracle.c::cancel_signal_ordering_names_a_child_cancelled_by_a_parent_that_was_not` | Rust's third violation, a child's cancel time earlier than its parent's, is not state (a parent's stored reason can be strengthened later by a more severe one), and task-level cancels (`asx_cancel_propagate` marks no region) are not read. |
| `runtime_epoch` | runtime module epochs advance consistently | N/A | Rust feeds it from per-module epoch notifications (scheduler, region, task and obligation tables, timer wheel, reactor). C has one static arena set under one scheduler and no per-module epochs; `src/core/epoch.c` is an unrelated phase API. | none | The concept is absent; slot generations and the kernel generation are handle validity, not module-epoch consistency. |
| `channel_atomicity` | channel reservations and wakes are atomic | PARTIAL | `src/channel/mpsc.c::asx_channel_try_reserve`, `::asx_send_permit_send`, `::asx_send_permit_abort` (queue plus reserved never exceeds capacity); a `Cx`-bound permit is a `SEND_PERMIT` obligation, so double send, stale permit and leak are refused or counted. Same shape in `src/channel/oneshot.c`. `src/runtime/oracle.c::asx_oracle_channel_atomicity` reads every channel's counters at the end of a run (`asx_channel_accounting_get_internal`): queued plus reserved within the capacity, the reserved count equal to the permit table's live tokens, the sender count equal to the creator's sender (until dropped) plus the live clones, and on the lock-free backend its in-use counter equal to messages plus permits. | `tests/unit/channel/test_mpsc.c::double_send_same_permit`, `tests/unit/runtime/test_oracle.c::channel_atomicity_names_counters_that_disagree_with_a_channels_permits_and_senders` | The check is of counters, not of events: a reserve/commit/abort that was not atomic but left the counters consistent is not seen. Permits reserved without a `Cx` register no obligation, so their leak is invisible; no data-loss-on-cancel or waker accounting; `oneshot` is not read. The unit test plants the violation in a copy of the real view (the table is module-static). |
| `waker_dedup` | waker registration and wake delivery are deduplicated | PARTIAL | `src/runtime/waker.c::asx_waker_wake` (signals only if not signaled; a deregistered waker is `ASX_E_NOT_FOUND`) and `src/sync/wait_queue.c::asx_wait_queue_park_current` (a task at most once per queue; exactly the available units handed out). | `tests/unit/runtime/test_waker.c::duplicate_wake_keeps_first_signal_order` | No registered / wake-requested / woken ledger, so lost wakeup, spurious wakeup and waker leak are not detected; only the futurelock scan sees a wake that never arrives. |
| `actor_leak` | actors stop before their owning region closes | PRESENT | An actor is a region task (`src/actor/actor.c::asx_actor_spawn` uses `asx_task_spawn`), so the region cannot close while it is live; region cancel stops it (begin_stop, drain, terminate). By construction, and checked as `task_leak` (a closed region has no live task: the actor's server task is one) and `quiescence`. | `tests/unit/actor/test_actor.c::test_region_cancel_skips_queued_casts`, `tests/unit/runtime/test_oracle.c::task_leak_names_a_closed_region_with_a_live_task` | No actor spawn/stop ledger: an actor slot's own state (stopped, refused) is not read, only its task's. |
| `mailbox` | mailbox capacity and backpressure invariants hold | PRESENT | The mailbox is a bounded `src/channel/mpsc.c` channel (capacity by construction); `src/actor/actor.c::actor_send` parks senders and `asx_actor_try_cast` returns `ASX_E_CHANNEL_FULL`; a stopped mailbox is left empty; its capacity accounting is `channel_atomicity` over the channel table. | `tests/unit/actor/test_actor.c::test_try_cast_reports_a_full_mailbox`, `tests/unit/runtime/test_oracle.c::channel_atomicity_names_counters_that_disagree_with_a_channels_permits_and_senders` | No sent / received / high-water counters, so "sent = received + evicted + discarded" is not independently asserted; no map from an actor to its mailbox channel is read. |
| `rref_access` | RRef access respects region and witness boundaries | N/A | There is no RRef or region-heap type in C (no such symbol). The nearest analogs are generation-stamped, runtime-tagged handles (`ASX_E_STALE_HANDLE`) and the region capture arena released at task completion. | none | Cross-region, post-close and witness-mismatch access to region-owned values cannot occur: no such values exist. |
| `registry_lease` | Spork registry leases are committed or aborted | PARTIAL | `src/actor/actor.c::asx_name_lease_release` / `::asx_name_lease_abort` (a second resolution is `ASX_E_OBLIGATION_ALREADY_RESOLVED`), `::asx_name_cleanup_region` and `::asx_name_cleanup_task` drop names when the caller invokes them. | `tests/unit/actor/test_actor.c::test_unregister_checks_the_caller_and_the_lease_identity` | A lease is a plain struct with an `active` flag, not a ledger obligation, and nothing in `src` calls the cleanup at region close or task completion: a never-resolved lease, or a name held by a completed task or closed region, is not detected. |
| `supervisor_quiescence` | supervisor regions close with no active children | PRESENT | `src/actor/supervisor.c::sub_drain` and `::sub_close_root` cancel, join and close each generation's region (via `asx_region_close_poll`) before the generation is released or a successor starts, and close the controller root the same way. A generation is a kernel region, so a closed one with a live task or child is `quiescence`'s. | `tests/unit/actor/test_supervisor.c::test_abort_drains_without_restarting`, `tests/unit/runtime/test_oracle.c::quiescence_names_a_closed_region_with_a_child_that_is_not_closed` | No supervisor-specific close-time assertion listing still-active children; a close failure surfaces only as `ASX_SUPERVISOR_ERR_REGION`. |

### 4.3 Not reported by default (5)

| Oracle | Rust statement | C | C mechanism and when | Test | Gap |
|---|---|---|---|---|---|
| `priority_inversion` | higher-priority work is not blocked indefinitely by lower-priority holders | MISSING | Counterparts only: budget priority, the lab's cancel / timed / ready lanes, FIFO wait queues, mutex and semaphore holders; no detection and no inheritance. Rust's oracle is itself opt-in (manual instrumentation). | none | No record of holders and waiters with priorities; sync waiters are served in arrival order regardless of priority. |
| `fabric_publish` | FABRIC committed publishes reach matching subscribers | N/A | Native FABRIC has no C port; the nearest is the in-memory lab model `src/net/messaging.c::asx_msg_topic_publish` (excluded from the kernel archive). | `tests/unit/net/test_messaging.c::broker_pubsub` (model only) | No committed-publish or per-subscriber receipt record; the model has no delivery guarantee by design. |
| `fabric_reply` | FABRIC obligation-backed replies resolve before close | N/A | No FABRIC request/reply layer; reply obligations exist only for actor calls (`reply_linearity`). | none | Nothing to attach to. |
| `fabric_quiescence` | FABRIC cells are quiescent when regions close | N/A | No FABRIC cells; generic region quiescence is `asx_quiescence_check_detailed`. | none | The messaging model's queues are not tied to region close. |
| `fabric_redelivery` | FABRIC redelivery remains within its explicit bound | N/A | No redelivery, attempt counting or bound in the C messaging model. | none | Nothing to attach to. |

## 5. What this says about the work

Step 2 of the bead ports "every kernel-relevant oracle". Read against the
tables above, the kernel-relevant set is the 11 fed oracles plus the cancel
pair, and the cheapest checks, each over state the kernel already keeps, are:

1. **Tree and deadline scan** (`region_tree`, `deadline_monotone`): DONE,
   `asx_oracle_region_tree` and `asx_oracle_deadline_monotone` (a live
   region's parent is alive and lists it, each child's `parent_id` matches, a
   child's deadline does not exceed its parent's, a bounded parent walk finds
   no cycle, and the single-root rule).
2. **Cancel propagation** (`cancellation_protocol`, `cancel_signal_ordering`):
   the region part is DONE (`asx_oracle_cancel_propagation`: every live child
   of a cancel-requested region has a cancel requested) and so is the
   ordering of a propagated cancel (`asx_oracle_cancel_signal_ordering`: a
   child cancelled by its parent has a parent that was cancelled, and the
   parent's region.cancelled event comes first in the trace). Not done: every
   live non-cleanup task of it has a pending cancel (a task-level
   `asx_cancel_propagate` marks no region, so the task side is not state we
   can tie to a region), and a parent's cancel time not later than its
   child's (a stored reason can be strengthened later, so time is not an
   invariant of state).
3. **Counter consistency** (`task_leak`, `quiescence`): DONE as
   `task_counters` (a region's live-task count equals the live, non-completed
   tasks that name it; a lab admission still pending is counted in the region
   and is a live slot) and `arena_counters` (the kernel's live counts of
   regions, tasks and obligations equal their live slots, and the task and
   obligation free lists hold exactly the released slots). Both read the
   kernel K18 (bd-9kll.3.20) moved the counters into, so a lab's oracle reads
   its own runtime's.
4. **End-of-run completeness** (`obligation_leak`, `region_leak`): DONE for
   what state gives. `obligation_leak` also fails a reserved obligation whose
   holder completed or is gone; `region_leak` fails a closing region with
   nothing left to wait for (Rust's "stuck for longer than a bound" needs a
   clock: a run that stops mid-drain is not a leak). Still to do: the
   wait-node and waker counts, and ids in the verdict (`detail_count` is a
   count).
5. **Stuck cancel** (`cancellation_protocol`, `cancel_debt`): DONE as far as
   state goes, under lab dispatch only (`asx_oracle_cancel_debt`); the sweep's
   idle handling unparks a cancelled task, so there a parked cancelled task
   is not yet a violation.
6. **Witness enforcement** (`cancel_correctness`): DONE for what one witness
   per task can show (`asx_oracle_cancel_correctness`). Still to do: stop
   discarding the witness `advance` status, and validate each transition,
   which needs the previous witness (a per-step hook).
7. **Registry and actors** (`registry_lease`, `mailbox`, `actor_leak`,
   `supervisor_quiescence`): `actor_leak` and `supervisor_quiescence` need no
   module check: an actor is a region task and a supervisor generation is a
   region, so `task_leak` and `quiescence` over the kernel arenas are those
   checks. A mailbox is a channel: `mailbox` is `channel_atomicity` over the
   channel table's read-only accounting view (DONE). `registry_lease` is not
   done: a name registry is caller-owned storage with no registry to scan
   (the lease check would also show the missing wiring of the name cleanup
   at close).
8. **Task groups, monitors and replies** (`loser_drain`, `down_order`,
   `reply_linearity`): DONE without the registry the first thought needed.
   `loser_drain` reads the watcher a group's owner leaves on each member and
   the RACE_LOST cancel the member carries; `down_order` reads a short
   delivery log the monitor module now keeps; `reply_linearity` reads the
   obligation events the trace ring already has. `supervision` is not done:
   it is judged by the controller's actions against the declared policy, and
   those are user-trace text of which the ring keeps a hash (an observer
   would have to record them).

Step 3 needs a Rust-side change that is not in this document: the twin_run
fixtures carry no oracle report today, so nothing yet compares C's verdicts
with Rust's. `LabRuntime` can produce one (`OracleSuite::report`), and it
covers the 11 fed oracles.
