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
  fails), `asx_oracle_leak` (any live region not closed fails; the header says
  obligations too, the code does not scan them) and `asx_oracle_replay_match`
  (two runs agree). Only their PASS and null-lab paths are unit-tested
  (`tests/unit/runtime/test_replay.c`).
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
| PARTIAL | 7 |
| MISSING | 2 |
| N/A | 6 |

Of the 11 oracles `LabRuntime` feeds, 10 are PRESENT and 1 (`region_tree`) is
PARTIAL. None of the 11 has an independent per-step or end-of-run check in C
that can fail a lab run with a named violation, apart from the partial
coverage of `asx_oracle_quiescence`.

## 4. The catalog

"Fed" is whether `LabRuntime` feeds the oracle (section 1). Tests are
`path::name`, the name being the `TEST()` argument.

### 4.1 Fed by `LabRuntime` (the 11 a lab run actually verifies)

| Oracle | Rust statement | C | C mechanism and when | Test | Gap |
|---|---|---|---|---|---|
| `task_leak` | structured concurrency: every task is owned and completed before region close | PRESENT | `src/runtime/quiescence.c::asx_region_finalize_one` never sets CLOSED while the region has live tasks (`ASX_E_QUIESCENCE_TASKS_LIVE`): by construction at every close. `src/runtime/replay.c::asx_oracle_quiescence` scans the task arena at the end of a run. | `tests/unit/runtime/test_quiescence.c::quiescence_closed_region_with_live_tasks_reports_tasks_live` | The oracle is global and names no task or region; its FAIL path with a live task is untested; no spawn/complete/close pairing. |
| `obligation_leak` | no obligation leaks: permits, acks, and leases resolve before close | PRESENT | `src/runtime/lifecycle.c::asx_task_resolve_held_obligations_internal` at holder completion and `::asx_region_audit_leaks_internal` at FINALIZING, before CLOSED, under the leak policy (PANIC refuses to close); ghost `LINEARITY_LEAK` in debug builds. | `tests/unit/runtime/test_budget_obligation.c::region_close_audit_leaks_obligations_still_reserved` | Policy-driven: LOG and RECOVER do not fail the run, a caller must assert the leak count. `asx_oracle_leak` scans regions, not obligations. Name leases and untracked permits are not in the ledger. |
| `quiescence` | region close => quiescence | PRESENT | `asx_region_finalize_one` (CLOSED only with no children, drained cleanup stack, no tasks, then the leak audit); `src/runtime/quiescence.c::asx_quiescence_check_detailed` gives Q1-Q4 on request; `src/runtime/rt.c::asx_runtime_is_quiescent` for the runtime. | `tests/unit/runtime/test_quiescence.c::quiescence_closed_region_with_unresolved_obligation_reports_error` | Q1-Q4 are evaluated when asked, not recorded at each close or checked per step; a violation gives counts, not ids. |
| `loser_drain` | race losers are canceled and fully drained | PRESENT | `src/runtime/task_group.c::asx_task_group_poll`: any decision moves the group to DRAINING and DONE is reached only when every loser is cancelled and joined; by construction for every task-group race. | `tests/unit/runtime/test_task_group.c::race_cancels_and_drains_losers_before_resolving` | The inline `asx_race` (`src/core/combinator.c`) drops losers undrained, as Rust's inline `Cx::race` does; no race start/complete record, so a group abandoned mid-drain is caught only at region close. |
| `finalizer` | registered finalizers run before close completes | PRESENT | `asx_region_finalize_one` drains the cleanup stack (async entries run as masked cleanup tasks) before the leak audit and CLOSED; `asx_quiescence_check` reports `ASX_E_QUIESCENCE_NOT_REACHED` with cleanup pending. | `tests/unit/runtime/test_quiescence.c::defer_sync_finalizers_run_once_in_lifo_order_at_close` | Finalizers have no id and emit no register/run events, so "registered set vs ran set" cannot be checked after the fact. |
| `region_tree` | regions form one rooted ownership tree | PARTIAL | `src/runtime/lifecycle.c::asx_region_open_child` (parent must exist and be OPEN; child appended, `parent_id` set) and `src/runtime/quiescence.c::asx_region_unlink_from_parent`: links and acyclicity hold by construction. No tree scan exists. | `tests/unit/runtime/test_quiescence.c::region_open_child_sets_parent_and_tracks_child` | Rust's exactly-one-root rule is not enforced (`asx_region_open` mints a parentless root on every call); no `parent_id` / `children[]` consistency scan. |
| `deadline_monotone` | child deadlines are no looser than parent deadlines | PRESENT | `asx_region_open_child` copies the parent budget and `::asx_region_open_child_with_budget` applies `src/core/budget.c::asx_budget_meet` (minimum deadline): by construction at region open; no region budget is written after open. | `tests/unit/runtime/test_budget_obligation.c::budgets_tighten_down_the_region_tree` | No runtime scan and no budget-update event; a future budget-mutating API would be unguarded. |
| `cancellation_protocol` | cancellation follows request, drain, finalize | PRESENT | `src/core/transition_tables.c::asx_task_transition_check` applied on every task state write by `src/runtime/lifecycle.c::asx_task_set_state_internal` in every build; `src/runtime/cancellation.c::asx_checkpoint` acknowledges only at mask depth 0, masks are bounded, `::asx_region_cancel` propagates parent first. | `tests/unit/runtime/test_cancellation.c::task_state_write_applies_the_transition_table_in_every_build` | No bound on request -> acknowledge -> complete: C, like Rust, never force-completes an unacknowledged cancel (the hard bound is opt-in) and nothing reports it. |
| `down_order` | DOWN messages are delivered in deterministic order | PRESENT | `src/monitor/monitor.c::asx_monitor_task_finished` fires a finished task's monitors lowest reference first, one completion at a time; by construction at each completion. | `tests/unit/monitor/test_monitor.c::test_downs_are_delivered_by_ascending_reference` | No per-monitor delivery log to re-verify order afterwards (Rust records delivery time). |
| `supervision` | supervision restarts and escalations follow policy | PRESENT | `src/actor/supervisor.c`: the controller state machine enforces restart mode, `max_restarts` within a window, backoff, the three sibling policies and escalation, in the one place that restarts. | `tests/unit/actor/test_supervisor.c::test_rest_for_one_restarts_the_later_ones` | No independent observer comparing failure, restart and escalation events with the declared policy; the controller's trace is free-form user-trace text. |
| `reply_linearity` | Spork replies resolve exactly once | PRESENT | A call reply is a oneshot permit reserved with the caller's `Cx` (a `SEND_PERMIT` obligation); the actor loop always sends or aborts it, including on handler failure, drain and cell drop (`src/actor/actor.c`): structural exactly-once, a leak shows in the ledger. | `tests/unit/actor/test_actor.c::test_cancelled_server_drops_queued_call` | No reply-specific ledger or obligation kind; correctness rests on the handler's signature and the generic obligation ledger. |

### 4.2 Reported by Rust but not fed by `LabRuntime` (13)

| Oracle | Rust statement | C | C mechanism and when | Test | Gap |
|---|---|---|---|---|---|
| `region_leak` | regions do not remain stuck or leaked past lifecycle bounds | PARTIAL | `asx_oracle_leak` (end of run: a live region not CLOSED fails); `src/runtime/lab_dispatch.c::asx_lab_futurelocks` / `asx_scheduler_stuck_tasks` (a stuck obligation holder, a run that ended WOULD_BLOCK). | `tests/unit/runtime/test_lab_dispatch.c::a_parked_obligation_holder_is_a_futurelock_past_the_threshold` | No stuck-in-state thresholds (created-never-active, stuck Closing / Draining / Finalizing, a task never polled). |
| `ambient_authority` | effects require explicit Cx capabilities | PARTIAL | Runtime: capability bits checked at the `Cx`-taking APIs (`src/cx/cx.c::asx_cx_now`, `::asx_cx_random_u64`, `src/cx/scope.c::asx_scope_init`; narrowing refuses escalation). Static: `tools/ci/check_ambient_authority.py` against the catalog in `src/security/audit.c`. | `tests/unit/cx/test_cx.c::narrow_escalation_fails` | No effect/capability event stream, so "effect without grant" is never recorded; the kernel's raw APIs take no `Cx` by design; the lint covers only clock and entropy. |
| `cancel_correctness` | cancel-correctness witnesses start from valid initial states | PARTIAL | `src/core/cancel.c::asx_cancel_witness_validate_initial`, `::asx_cancel_witness_validate_transition` and `::asx_cancel_witness_advance` exist and match Rust's validators; the runtime creates and advances a witness per cancelled task. | `tests/unit/core/test_cancel.c::witness_validate_phase_regression` | The runtime never validates its own witnesses and discards the `advance` status, so a bad witness is neither ghost-recorded nor reported. |
| `cancel_debt` | cancellation debt remains bounded and drains | MISSING | Counterparts only: per-kind cleanup budgets and the on-request `asx_region_drain_progress`. | `tests/unit/runtime/test_quiescence.c::drain_progress_after_partial_drain` | No queue depth, arrival-vs-completion or overdue-cleanup accounting and no threshold. |
| `cancel_signal_ordering` | cancel signal order is deterministic and causally valid | PRESENT | `src/runtime/cancellation.c::asx_region_cancel` walks `asx_region_subtree_internal` parent first, emits `ASX_TRACE_REGION_CANCELLED` per region in that order and cancels every live task with its region's reason; `::asx_cancel_propagate` for tasks. By construction per cancel. | `tests/unit/runtime/test_cancellation.c::region_cancel_closes_subtree_and_chains_parent_reasons` | No oracle over trace or state asserting no child cancel precedes its parent's, or that a cancelled region has no uncancelled child. |
| `runtime_epoch` | runtime module epochs advance consistently | N/A | Rust feeds it from per-module epoch notifications (scheduler, region, task and obligation tables, timer wheel, reactor). C has one static arena set under one scheduler and no per-module epochs; `src/core/epoch.c` is an unrelated phase API. | none | The concept is absent; slot generations and the kernel generation are handle validity, not module-epoch consistency. |
| `channel_atomicity` | channel reservations and wakes are atomic | PARTIAL | `src/channel/mpsc.c::asx_channel_try_reserve`, `::asx_send_permit_send`, `::asx_send_permit_abort` (queue plus reserved never exceeds capacity); a `Cx`-bound permit is a `SEND_PERMIT` obligation, so double send, stale permit and leak are refused or counted. Same shape in `src/channel/oneshot.c`. | `tests/unit/channel/test_mpsc.c::double_send_same_permit` | Permits reserved without a `Cx` register no obligation, so their leak is invisible; no reserve/commit/abort event stream; no data-loss-on-cancel or waker accounting. |
| `waker_dedup` | waker registration and wake delivery are deduplicated | PARTIAL | `src/runtime/waker.c::asx_waker_wake` (signals only if not signaled; a deregistered waker is `ASX_E_NOT_FOUND`) and `src/sync/wait_queue.c::asx_wait_queue_park_current` (a task at most once per queue; exactly the available units handed out). | `tests/unit/runtime/test_waker.c::duplicate_wake_keeps_first_signal_order` | No registered / wake-requested / woken ledger, so lost wakeup, spurious wakeup and waker leak are not detected; only the futurelock scan sees a wake that never arrives. |
| `actor_leak` | actors stop before their owning region closes | PRESENT | An actor is a region task (`src/actor/actor.c::asx_actor_spawn` uses `asx_task_spawn`), so the region cannot close while it is live; region cancel stops it (begin_stop, drain, terminate). By construction. | `tests/unit/actor/test_actor.c::test_region_cancel_skips_queued_casts` | No actor spawn/stop ledger; "stopped before close" is implied by task completion, not asserted. |
| `mailbox` | mailbox capacity and backpressure invariants hold | PRESENT | The mailbox is a bounded `src/channel/mpsc.c` channel (capacity by construction); `src/actor/actor.c::actor_send` parks senders and `asx_actor_try_cast` returns `ASX_E_CHANNEL_FULL`; a stopped mailbox is left empty. | `tests/unit/actor/test_actor.c::test_try_cast_reports_a_full_mailbox` | No sent / received / high-water counters, so "sent = received + evicted + discarded" is not independently asserted. |
| `rref_access` | RRef access respects region and witness boundaries | N/A | There is no RRef or region-heap type in C (no such symbol). The nearest analogs are generation-stamped, runtime-tagged handles (`ASX_E_STALE_HANDLE`) and the region capture arena released at task completion. | none | Cross-region, post-close and witness-mismatch access to region-owned values cannot occur: no such values exist. |
| `registry_lease` | Spork registry leases are committed or aborted | PARTIAL | `src/actor/actor.c::asx_name_lease_release` / `::asx_name_lease_abort` (a second resolution is `ASX_E_OBLIGATION_ALREADY_RESOLVED`), `::asx_name_cleanup_region` and `::asx_name_cleanup_task` drop names when the caller invokes them. | `tests/unit/actor/test_actor.c::test_unregister_checks_the_caller_and_the_lease_identity` | A lease is a plain struct with an `active` flag, not a ledger obligation, and nothing in `src` calls the cleanup at region close or task completion: a never-resolved lease, or a name held by a completed task or closed region, is not detected. |
| `supervisor_quiescence` | supervisor regions close with no active children | PRESENT | `src/actor/supervisor.c::sub_drain` and `::sub_close_root` cancel, join and close each generation's region (via `asx_region_close_poll`) before the generation is released or a successor starts, and close the controller root the same way. | `tests/unit/actor/test_supervisor.c::test_abort_drains_without_restarting` | No supervisor-specific close-time assertion listing still-active children; a close failure surfaces only as `ASX_SUPERVISOR_ERR_REGION`. |

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

1. **Tree and deadline scan** (`region_tree`, `deadline_monotone`): one pass
   over the region arena: a live region's parent is alive and lists it, each
   child's `parent_id` matches, a child's deadline does not exceed its
   parent's, and a bounded parent walk finds no cycle. It also adds the
   single-root rule, which is the one PARTIAL gap in the fed set.
2. **Cancel propagation** (`cancellation_protocol`, `cancel_signal_ordering`):
   every live child of a cancel-requested region has a cancel requested, every
   live non-cleanup task of it has a pending cancel, and a parent's cancel
   timestamp is not later than its child's.
3. **Counter consistency** (`task_leak`, `quiescence`): between steps a
   region's live-task count equals the live, non-completed tasks that name it
   (lab admissions pending are counted in the region and must be included
   deliberately), and likewise for obligations.
4. **End-of-run completeness** (`obligation_leak`, `region_leak`): extend
   `asx_oracle_leak` to the obligation arena and to the wait-node and waker
   counts, and fill `detail_count`.
5. **Stuck cancel** (`cancellation_protocol`, `cancel_debt`): at idle or
   WOULD_BLOCK, a non-terminal task with a pending cancel at mask depth 0 is a
   violation; `asx_scheduler_stuck_tasks` already reports it.
6. **Witness enforcement** (`cancel_correctness`): stop discarding the
   witness `advance` status and validate each transition.
7. **Registry and actors** (`registry_lease`, `mailbox`, `actor_leak`): the
   lease check also shows the missing wiring of the name cleanup at close.

Step 3 needs a Rust-side change that is not in this document: the twin_run
fixtures carry no oracle report today, so nothing yet compares C's verdicts
with Rust's. `LabRuntime` can produce one (`OracleSuite::report`), and it
covers the 11 fed oracles.
