# Scenario DSL v2 (`asx.scenario.v2`)

**Normative.** Bead W1.3 (bd-9kll.2.3). This supersedes `docs/SCENARIO_DSL.md`
(DSL v1). DSL v1's ops could not express task bodies, so no Rust capture ever
polled a task.

A DSL v2 scenario runs unchanged on both engines:

- **Rust:** `tools/twin_run` (W1.4) interprets it inside asupersync's
  `LabRuntime`, pinned at `5e60b1c4c53d62aaddae68de3ee7de4732f1755b`.
- **C:** `asx_conformance_run` (W1.5) interprets it through the asx runtime.

Both project the run into `docs/CANONICAL_VOCABULARY_V2.md`. Every step below
names the Rust API and the C API that implement it, so neither interpreter
has to guess. Rust paths are under `/dp/asupersync/src/`, cited at the pinned
rev. C names are in this repo's `include/asx/`.

Companion files:

- `schemas/scenario_dsl_v2.json`: the JSON Schema.
- `tests/conformance/scenarios_v2/`: the example scenarios, one or more per
  unit of W1.7.

## 1. Document shape

```json
{
  "schema": "asx.scenario.v2",
  "id": "obligation-reserve-commit-001",
  "description": "A task reserves a Lease, commits it and completes ok.",
  "unit": "obligation",
  "rules": ["rule.obligation.reserve", "rule.obligation.commit"],
  "seed": 42,
  "lab": {"panic_on_leak": false, "max_steps": 10000},
  "regions": [{"name": "r.main", "parent": "root", "budget": null}],
  "channels": [],
  "sync": [],
  "tasks": [{"name": "t.a", "region": "r.main", "budget": null, "program": [ ...steps... ]}],
  "script": [],
  "expect": {"kind": "ok"}
}
```

- **`unit`** names the semantic unit (W1.7, bd-9kll.2.8): `budget`, `cancel`,
  `task_lifecycle`, `region_lifecycle`, `obligation`, `quiescence`,
  `scheduler`, `timers`, `mpsc`, `oneshot`, `broadcast`, `watch`, `sync`,
  `combinators`, `task_groups`, `actor`, `supervision` or `leak_policy`.
- **`rules`** lists the canonical rule IDs exercised, taken from the Canonical
  Rule Index of `/dp/asupersync/asupersync_v4_formal_semantics.md` (lines
  23-73, for example `rule.cancel.request`). The coverage report (W1.7)
  counts fixtures per rule.
- **`seed`** is `LabConfig::new(seed)` (`lab/config.rs:187`) in Rust. In C it
  seeds the lab dispatch model (`asx_scheduler_use_lab_dispatch`,
  `src/runtime/lab_dispatch.c`, bd-9kll.4.2): one task per step, picked by
  lane, priority and wake order with ties broken by the step's xorshift64
  value, and due timers firing in the timer wheel's order. The two runtimes
  dispatch in the same order, so contention between tasks resolves the
  same way on both sides.
- **`lab`** configures the run:
  - `worker_count` is always 1.
  - `panic_on_leak` maps to `LabConfig::panic_on_leak` (`lab/config.rs:258`).
    It defaults to `false`, so leak scenarios complete and report a leak
    instead of aborting.
  - `max_steps` is the total step budget. Exceeding it is a **harness
    failure**, never an expected result.
- **`expect`** is either `{"kind":"ok"}` or
  `{"kind":"must_fail","error":{"task","step","status"}}`. `must_fail`
  asserts that the named step of the named task returns that status (§5).
  It is how the forbidden-behaviour catalog (`docs/FORBIDDEN_BEHAVIOR_CATALOG.md`)
  is covered.

Every name in a scenario must be unique, and every name becomes the entity's
canonical name in the vocabulary (`CANONICAL_VOCABULARY_V2.md` §2).

### Budgets

A budget is written as:

```json
{"deadline_ns": 1000, "poll_quota": 5, "cost_quota": 100, "priority": 128}
```

- A field that is absent is unconstrained.
- `null` means `Budget::INFINITE` (Rust) / `asx_budget_infinite()` (C).
- A partial budget starts from `Budget::new()` (priority 128, `types/budget.rs:252`)
  and applies each present field (`with_deadline` `:340`,
  `with_poll_quota` `:406`, `with_cost_quota` `:414`, `with_priority` `:422`).

The lab charges one `poll_quota` unit per poll (`lab/runtime.rs:4664`). An
exhausted quota or a passed deadline is turned into a cancel by the next
`checkpoint` (`cx/cx.rs:3112-3160`).

## 2. Execution model

The driver runs a scenario in four phases.

1. **Setup.**
   - Create the root region `"root"` with an infinite budget: Rust
     `lab.state.create_root_region(Budget::INFINITE)`
     (`runtime/state.rs:3874`), C `asx_region_open`.
   - Create each `regions` entry in order: Rust `create_child_region`
     (`state.rs:3941`), C `asx_region_open_child_with_budget`.
   - Create the `channels` and `sync` objects (§3.6, §3.7).
   - Create each `tasks` entry in order: Rust
     `lab.state.create_task(region, budget, body)`
     (`state.rs:4461`), then `lab.scheduler.lock().schedule(task, 0)`.
     C uses `asx_task_spawn_with_budget(region, interpreter_poll, program, budget, &id)`.
   - Tasks become runnable in array order.
2. **Script.** For each `script` entry in order:
   - run until idle (no runnable task): Rust `lab.run_until_idle()`
     (`lab/runtime.rs:3428`), C `asx_scheduler_run` until no task is ready;
   - if `at_ns` is later than now, advance virtual time to it: Rust
     `advance_time_to` (`:3097`), C `asx_lab_advance_time`;
   - apply the entry's driver operation (§4).
3. **Finish.**
   - Run to quiescence with timer auto-advance: Rust
     `run_with_auto_advance()` (`:3224`), C the equivalent loop of
     run-until-idle and advance-to-next-timer.
   - Stop when no task, timer or pending obligation can make progress.
   - Exceeding `lab.max_steps` is a harness failure.
4. **Project.** Emit the vocabulary trace, the snapshot and the observations.

### How task programs run

A task's `program` is a list of steps run by the engine's interpreter. In
Rust that is an `async` body; in C, a poll function with a program counter.

- **One poll runs steps until one suspends.** Within one poll, steps run in
  order until a step suspends (its future returns `Pending` / the C call
  returns `ASX_E_PENDING`) or the program ends. A step that is ready at once
  does **not** end the poll. This is ordinary `async` semantics and is what
  the C interpreter must reproduce, because poll counts are observable
  through `poll_quota`.
- **Steps do not observe cancellation unless stated.** A step observes a
  pending cancellation only where its row in §3 says so (Rust: only
  operations that call `cx.checkpoint()` see it, and `checkpoint` returns
  `Ok` while masked; `cx.rs:2749`).
- **A program ends** when it runs out of steps (the outcome is Ok), at a
  `return` step, or when a step that observed cancellation ends it (§3.1).
- **`create_task` tasks: cancellation wins.** If a cancel was requested by
  the time the body returns, the outcome is `Cancelled`, whatever the body
  returned (`state.rs:104-108`; `runtime/task_handle.rs:172-199`). Children
  spawned with `spawn` keep their returned value if they acknowledged an
  attributed cancel after their first poll (`cx.rs:4901`).
- **Observations.** Every executed step produces exactly one observation
  (`CANONICAL_VOCABULARY_V2.md` §7), `{task, step, op, status, value}`.
  This includes `yield`, `trace` and `return`, so both interpreters record
  the same list.
  - `step` is the 1-based index in the program, and `op` is the step's op.
  - A `masked` step produces one observation per inner step, all with the
    `masked` step's index and with `op` = `masked/<j>/<inner op>`, where j
    is 1-based.
  - Programs of spawned children and of group members are separate tasks
    with their own names, so their steps are observed under those names.
  - A step that never runs (because the program ended earlier) produces
    nothing.

## 3. Task steps

Every step is an object with `"op"` and the op's fields. "Blocks" means the
step can suspend. "Cancel" says what a pending, unmasked cancellation does.

### 3.1 Control

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `yield` | — | `runtime::yield_now().await` (`runtime/yield_now.rs:36`) | return `ASX_E_PENDING` with the task left runnable | once | ignored |
| `checkpoint` | `on_cancel`: `"return"` (default) or `"continue"` | `cx.checkpoint()` (`cx.rs:2749`) | `asx_checkpoint(self, &cr)` | no | Acknowledges the cancel and returns `ASX_E_CANCELLED`. With `"return"` the program ends; with `"continue"` it goes on. A masked task gets `ASX_OK`. |
| `sleep` | `ns` | `time::sleep(cx.now(), Duration::from_nanos(ns)).await` (`time/sleep.rs:1182`) | `asx_task_wait_until(self, now + ns)` | until virtual time ≥ now+ns | If the kind is neither `Timeout` nor `Deadline` and the task is unmasked, the step checkpoints (acknowledging) and completes early with `ASX_OK`. If it is first polled after the cancel, it takes one extra scheduler trip (`sleep.rs:789-812`). Otherwise it keeps sleeping. |
| `sleep_until` | `at_ns` | `time::sleep_until(Time::from_nanos(at_ns)).await` (`sleep.rs:1203`) | `asx_task_wait_until(self, at_ns)` | until virtual time ≥ at_ns | as `sleep` |
| `trace` | `message` | `cx.trace(message)` (`cx.rs:3362`) | emits vocabulary `user.trace` (C gap, §7) | no | ignored |
| `return` | `outcome`: `{"tag":"ok"}`, `{"tag":"err","status":S}` or `{"tag":"panicked","message":M}` | body returns `Outcome::Ok(())` / `Outcome::Err(code)`, or `panic!(M)` | poll function completes with the outcome (C gap for panicked, §7) | no | The cancellation-wins rule above applies. |

### 3.2 Masking

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `masked` | `steps` | `cx.masked(\|\| { …steps… })` (`cx.rs:3315`) | `asx_task_mask(self)`; steps; `asx_task_unmask(self)` | no: `steps` may contain only non-blocking steps | Inside, `checkpoint` returns `ASX_OK` (`rule.cancel.checkpoint_masked`). Nesting deeper than 64 is a harness error (`cx.rs:3323`). |

Rust has no mask that spans an await (`MaskGuard` is private, `cx.rs:706`),
so a blocking step inside `masked` is a schema error.

### 3.3 Obligations

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `reserve` | `kind` (`SendPermit`, `Ack`, `Lease`, `IoOp`, `SemaphorePermit`, `Transaction`), `as` | `cx.try_register_obligation_checked(kind, cx.task_id())` → `Ok(Some(token))` (`cx.rs:1746`) | `asx_obligation_reserve_ex(own_region, kind, self, &id)` | no | Ignored. A region that is no longer Open gives `ASX_E_REGION_CLOSED` (Rust `ObligationAdmissionError::RegionClosed`, `runtime/obligation_mailbox.rs:68`). |
| `commit` | `obligation` | `token.commit()` (`obligation_mailbox.rs:879`) | `asx_obligation_commit(id)` | no | ignored |
| `abort` | `obligation`, `reason` (`Cancel`, `Error`, `Explicit`) | `token.abort(reason)` (`:888`) | `asx_obligation_abort(id)` plus reason (C gap, §7) | no | ignored |
| `leak` | `obligation` | `drop(token)`, which posts a Leak (`:897`) | the obligation is left unresolved; C reports a leak (`ASX_TRACE` leak gap, §7) | no | ignored |

`kind` is required: C's `ASX_OBLIGATION_KIND_GENERIC` has no Rust counterpart
and is not allowed. Rust applies reservations at the next lab step
(obligation mailbox). That is invisible to the vocabulary, which carries no
event times.

### 3.4 Spawning, joining, child regions

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `spawn` | `as`, `program`, optional `budget`, optional `region` (a child region this task opened) | `cx.spawn(…)` (`cx.rs:4880`). A `budget` uses `cx.spawn_in(&cx.scope_with_budget(b), …)` (`:4976`, `:4547`). `region` uses `child.cx().spawn(…)` (`cx/child_region.rs:334`). | `asx_task_spawn_with_budget(region, interpreter_poll, program, budget, &id)` | no | ignored. A region that is closing or closed refuses the child, observed as `ASX_E_REGION_CLOSED`: C refuses at the spawn; Rust accepts the spawn into its mailbox and admission refuses it later (`SpawnError::RegionClosed`, `runtime/state.rs:1702`), resolving the handle as `JoinError::Cancelled`, which twin_run projects onto the spawn step. A refused child has no trace events and no snapshot entry. |
| `join` | `task` | `handle.join(&cx).await` (`runtime/task_handle.rs:793`) | `asx_task_join_poll(self, target, &outcome)` until done | until the child completes | Ignored: join is uninterruptible (`task_handle.rs:1080-1126`). The observation `value` is the child's outcome (vocabulary §4); a refused child (`spawn` above) is observed as `ASX_E_REGION_CLOSED` with no value. |
| `try_join` | `task` | `handle.try_join()` (`:863`) | `asx_task_join(target, &outcome)` | no | ignored. A running child gives `ASX_E_TASK_NOT_COMPLETED`; a refused child `ASX_E_REGION_CLOSED`. |
| `abort_task` | `task`, `kind`, optional `message` | `handle.abort_with_reason(reason)` (`task_handle.rs:1005`), reason attributed to the requester (§4, "Reason attribution") | `asx_task_cancel_with_origin(target, kind, own_region, self)` plus message (C gap, §7) | no | ignored |
| `open_region` | `as`, optional `budget` | `cx.open_child_region(ChildRegionSpec::inherit().with_budget(b)).await` (`cx.rs:4491`; `child_region.rs:76`, `:87`) | `asx_region_open_child_with_budget(own_region, budget, &id)` | until the next lab step creates the region | ignored. A closing parent gives `ASX_E_REGION_CLOSED`. |
| `cancel_region` | `region` (one this task opened), `kind`, optional `message` | `child.cancel(reason)` (`child_region.rs:381`), reason attributed to the requester (§4, "Reason attribution") | `asx_region_cancel(region, &reason, NULL)` | no | ignored |
| `close_region` | `region` (one this task opened) | `child.close().await` (`child_region.rs:424`): cancels the remaining tasks, drains, finalizes, waits for Closed | `asx_region_cancel(region, &reason, NULL)` with the User reason the Rust close command carries (`"owned child region body finished"`, `lab/runtime.rs:4195`), then wait for Closed (the region finalizes when its last task completes) | until Closed | Ignored: the closer's own cancellation is not observed. |

A task can cancel or close only regions it opened itself. Rust's region
command queue is crate-private (`runtime/spawn_mailbox.rs:2311`).
Cancelling or closing any other region is a driver operation (§4).

### 3.5 Task groups

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `race` | `members` (list of programs), optional `deadline_ns` | spawn each member with `cx.spawn`, then `cx.scope().race_all(&cx, handles).await` (`cx/scope.rs:1306`). A `deadline_ns` runs this inside `cx.scope().timeout(&cx, d, …)` (`:2130`). | `asx_task_group_init(g, RACE, 1)`, `asx_task_group_spawn` per member, then `asx_task_group_poll` | until a winner, or until all are drained | The owner's cancel before a winner cancels and drains every member. Losers are always cancelled and drained (`inv.combinator.loser_drained`). The status is `ASX_OK`; the `value` is `{winner_index, outcome}`, the winner's outcome, with `winner_index` null when the winner did not succeed (Rust's `Err` carries no index). |
| `join_all` | `members` | `cx.scope().join_all(&cx, handles).await` (`:1479`) | group mode JOIN | until all complete | Ignored: join_all joins one by one. The `value` is the list of member outcomes. |
| `first_ok` | `members` | `cx.scope().first_ok(&cx, factories).await` (`:2023`) | group mode FIRST_OK | until a member succeeds or all fail | The owner's cancel gives `ASX_E_CANCELLED` after draining. |
| `quorum` | `members`, `needed` | `cx.scope().quorum(&cx, needed, branches).await` (`:1811`) | `asx_task_group_init(g, QUORUM, needed)` | until `needed` succeed or that becomes impossible | as `first_ok`. Met: status `ASX_OK`, `value` = the number of members that succeeded. A `QuorumError` has no vocabulary mapping yet; both interpreters fail such a run closed. |

A member program ends with `return` to give its outcome. The default is Ok.
Members are named `"<owner>/g<s>.<i>"` (vocabulary §2) and are not in the
snapshot (vocabulary §6). Not interpreted yet on either side: `race` with
`deadline_ns` (Rust runs the race inside a separate `scope.timeout` task,
which C has no combinator for) and `first_ok`.

### 3.6 Channels

Channels are declared at the top level, so endpoints have owners:

```json
{"name": "ch.data", "type": "mpsc", "capacity": 2, "sender": "t.producer", "receiver": "t.consumer"}
{"name": "ch.sum", "type": "oneshot", "sender": "t.consumer", "receiver": "t.waiter"}
{"name": "ch.news", "type": "broadcast", "capacity": 4, "sender": "t.a", "subscribers": ["t.b", "t.c"]}
{"name": "ch.cfg", "type": "watch", "initial": 0, "sender": "t.a", "subscribers": ["t.b"]}
```

Each endpoint is moved into its owner's body (Rust) or bound to the owner's
interpreter (C). Values are unsigned 64-bit integers; C channels carry
`uint64_t`.

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `reserve_send` | `channel`, `as` | `tx.reserve(&cx).await` (`channel/mpsc.rs:637`) | `asx_channel_try_reserve`, waiting while full | while full, FIFO | Checked on every poll: `ASX_E_CANCELLED` even when there is room (`mpsc.rs:1061`). The permit registers a `SendPermit` obligation. |
| `permit_send` | `permit`, `value` | `permit.send(v)` (`:1565`) | `asx_send_permit_send(&permit, v)` | no | ignored |
| `permit_abort` | `permit` | `permit.abort()` (`:1633`) | `asx_send_permit_abort(&permit)` | no | ignored. The obligation is aborted with reason `Explicit`. |
| `send` | `channel`, `value` | `tx.send(&cx, v).await` (`:715`) | send without registering an obligation (C gap, §7) | while full | `ASX_E_CANCELLED`. No obligation and no trace event. |
| `try_send` | `channel`, `value` | `tx.try_send(v)` (`:773`) | `asx_channel_try_reserve` + `asx_send_permit_send`, without an obligation | no | ignored. Full gives `ASX_E_CHANNEL_FULL`. |
| `recv` | `channel` | `rx.recv(&cx).await` (`:1719`) | `asx_channel_try_recv`, waiting while empty | while empty | Checked first: `ASX_E_CANCELLED` even when a value is queued (`:1786`). The observation `value` is the received integer. |
| `try_recv` | `channel` | `rx.try_recv()` (`:1956`) | `asx_channel_try_recv` | no | ignored. Empty gives `ASX_E_CHANNEL_EMPTY`; closed gives `ASX_E_DISCONNECTED`. |
| `close_sender` | `channel` | `drop(tx)` (`:1241`) | `asx_channel_close_sender` | no | ignored |
| `close_receiver` | `channel` | `drop(rx)` (`:2122`) | `asx_channel_close_receiver` | no | ignored |
| `oneshot_send` | `channel`, `value` | `tx.send(&cx, v)` (`channel/oneshot.rs:556`): reserves a SendPermit, traces `"oneshot::reserve creating permit"`, commits | `asx_oneshot_try_send` plus the obligation and trace (C gap, §7) | no | Checked first: `ASX_E_CANCELLED`. |
| `oneshot_recv` | `channel` | `rx.recv(&cx).await` (`oneshot.rs:1250`): traces `"oneshot::recv received value"` / `"…channel closed"` | `asx_oneshot_try_recv`, waiting | until sent or dropped | `ASX_E_CANCELLED`, tracing `"oneshot::recv cancelled while waiting"`. |
| `broadcast_send` | `channel`, `value` | `tx.send(&cx, v)` (`channel/broadcast.rs:494`) | `asx_broadcast_send` | no | `ASX_E_CANCELLED` (`broadcast.rs:418`) |
| `broadcast_recv` | `channel` | `rx.recv(&cx).await` (`:894`) | `asx_broadcast_try_recv`, waiting | while empty | `ASX_E_CANCELLED`. Lagged gives `ASX_E_LAGGED` with `value` = n. |
| `watch_send` | `channel`, `value` | `tx.send(v)` (`channel/watch.rs:488`) | `asx_watch_send` | no | ignored (no `Cx`) |
| `watch_changed` | `channel` | `rx.changed(&cx).await` (`:722`), then `borrow_and_update` (`:917`) | `asx_watch_poll_changed` / `asx_watch_recv` | until a newer version | `ASX_E_CANCELLED` (`:736`). The `value` is the new value. |

### 3.7 Synchronization

Sync objects are declared at the top level:

```json
{"name": "m", "type": "mutex"}
{"name": "s", "type": "semaphore", "permits": 2}
{"name": "b", "type": "barrier", "parties": 2}
{"name": "n", "type": "notify"}
```

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `mutex_lock` | `mutex` | `m.lock(&cx).await` (`sync/mutex.rs:191`), guard kept | `asx_mutex_lock_begin` + `asx_mutex_poll_lock` | while held, FIFO | Checked on every poll: `ASX_E_CANCELLED` even when free (`mutex.rs:554-558`). |
| `mutex_unlock` | `mutex` | `drop(guard)` (`:758`) | `asx_mutex_unlock` | no | ignored |
| `sem_acquire` | `semaphore`, `count` | `s.acquire(&cx, n).await` (`sync/semaphore.rs:430`), permit kept | `asx_semaphore_acquire_begin` + `asx_semaphore_poll_acquire` | until n are available (all-or-nothing, FIFO) | `ASX_E_CANCELLED` (`:847`); count 0 succeeds at once. The permit registers a `SemaphorePermit` obligation (C gap, §7). |
| `sem_release` | `semaphore` | `drop(permit)` (`:1080`) | `asx_semaphore_release` | no | ignored |
| `barrier_wait` | `barrier` | `b.wait(&cx).await` (`sync/barrier.rs:135`) | `asx_barrier_wait_begin` + `asx_barrier_poll_wait` | until `parties` have arrived | `ASX_E_CANCELLED` (`:304`) |
| `notify_wait` | `notify` | `n.notified().await` (`sync/notify.rs:347`) | `asx_notify_wait_begin` + `asx_notify_poll_wait` | until notified | **Ignored**: a cancelled waiter stays parked until notified. Avoid pairing it with cancellation unless that is the point. |
| `notify_one` | `notify` | `n.notify_one()` (`:436`); stores a permit if nobody waits | `asx_notify_one` | no | ignored |
| `notify_all` | `notify` | `n.notify_waiters()` (`:471`) | `asx_notify_all` | no | ignored |

### 3.8 Actors and supervision

These steps use built-in behaviours, so programs never define handlers.

- **`counter`:** state is a u64 starting at 0; a cast adds its value; a call
  returns the state.
- **`echo`:** a call returns its request.

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `server_spawn` | `as`, `behavior`, `mailbox` | `cx.spawn_gen_server(server, cap)` (`gen_server.rs:2511`) | `asx_actor_spawn(&h, own_region, behavior, state)` | no | ignored |
| `cast` | `server`, `value` | `h.cast(&cx, msg).await` (`gen_server.rs:1268`) | `asx_actor_cast` | while the mailbox is full | Checked first: `ASX_E_CANCELLED`. |
| `call` | `server`, `request` | `h.call(&cx, req).await` (`:1171`) | `asx_actor_call` + `asx_call_token_poll` | until the reply arrives | Checked first: `ASX_E_CANCELLED`. The caller must not be in `"root"` (Rust rejects root-region callers, `gen_server.rs:1086-1094`); both interpreters reject such a scenario as a scenario error before running it. |
| `server_stop` | `server` | `h.stop()` (`:1453`), then `join` | `asx_actor_stop` | until stopped | ignored |
| `supervise` | `as`, `policy` (`one_for_one`, `one_for_all`, `rest_for_one`), `max_restarts`, `window_ns`, `children` (`{name, mode: permanent\|transient\|temporary, program}`) | `SupervisorBuilder::new(name).with_restart_policy(p).child(…).compile()?.bind_managed(bindings, SupervisionConfig::new(max, window))?.spawn(&cx)` (`supervision.rs:894`, `:924`, `:931`, `:973`, `:2093`, `:3497`) | `asx_supervisor_start(&h, own_region, &config, children, n)` | no | ignored |
| `supervisor_join` | `supervisor` | `handle.join(&cx).await` (`supervision.rs:3429`) | wait for `asx_supervisor_is_alive` to become false | until it exits | ignored |

## 4. Driver operations (`script`)

Each entry is `{"at_ns": t, "op": {...}}`. It is applied after the run goes
idle and virtual time is advanced to `t` (§2).

| op | Fields | Rust | C |
|---|---|---|---|
| `cancel_region` | `region`, `kind`, optional `message` | `lab.state.cancel_request(region, &reason, None)` (`state.rs:7547`); the effects are routed with `into_parts()`, `scheduler.schedule_cancel` and `dispatch()` (pattern: `tests/api_v2_integration.rs:347-358`) | `asx_region_cancel(region, &reason, NULL)` |
| `cancel_task` | `task`, `kind`, optional `message` | `lab.state.cancel_task(task, &reason)` (`state.rs:3429`), effects routed as above | `asx_task_cancel_with_origin(task, kind, task_region, ASX_INVALID_ID)` plus message |
| `close_region` | `region`, `kind` | `cancel_request(region, kind)`, then `advance_region_state(region)` (`state.rs:10057`) after each idle until Closed | `asx_region_cancel(region, &reason, NULL)`, then `asx_scheduler_run_until_idle` until Closed |
| `advance` | `ns` | `lab.advance_time(ns)` (`lab/runtime.rs:3076`; a forward jump, the only clock fault Rust offers) | `asx_lab_advance_time(ns)` |
| `region_limits` | `region`, optional `max_tasks`, `max_children`, `max_obligations` | `lab.state.set_region_limits(region, RegionLimits{…})` (`state.rs:4103`; `record/region.rs:208`) | region admission limits (C gap, §7) |

### Reason attribution

Every cancel reason a scenario creates carries real attribution, built the
way asupersync's own request paths build one:
`CancelReason::with_origin(kind, origin_region, now)`, plus
`.with_task(origin_task)` when a task requested it, plus `.with_message(m)`
(`cx/cx.rs:3940`, `app.rs:348`). `now` is the virtual time of the request.

| Request | `origin_region` | `origin_task` |
|---|---|---|
| driver `cancel_region`, `close_region` | the target region | none |
| driver `cancel_task` | the target task's region | none |
| step `abort_task`, `cancel_region` | the requesting task's region | the requesting task |

`CancelReason::new(kind)` must not be used. It is a testing default that
stamps `RegionId::testing_default()` and a fixed 1 s timestamp
(`types/cancel.rs:590-605`). Captures made with it recorded placeholder
attribution, and the placeholder region happened to be named `"root"`.
C builds the same reason and passes it whole: `asx_region_cancel(region,
&reason, …)` and `asx_task_cancel_with_reason(task, &reason)`.

There is no allocation-failure fault and no backward or per-task clock skew:
Rust has neither (`lab/runtime.rs:3357`). `region_limits` is the scripted
stand-in for resource exhaustion. A denied spawn surfaces as the child's
`Cancelled` outcome, and a denied reservation as `ASX_E_ADMISSION_LIMIT`.

## 5. Must-fail scenarios

`{"kind":"must_fail","error":{"task":"t.x","step":3,"status":"ASX_E_..."}}`
asserts that step 3 of `t.x` returns that status. Both engines must produce
that observation; the rest of the run is compared as usual. A must-fail
scenario whose step succeeds is a divergence in the failing engine.

The status names come from `CANONICAL_VOCABULARY_V2.md` §5. Typical uses:

- commit after abort: `ASX_E_OBLIGATION_ALREADY_RESOLVED`;
- reserve in a closed region: `ASX_E_REGION_CLOSED`;
- send on a closed channel: `ASX_E_DISCONNECTED`;
- reserve over a region limit: `ASX_E_ADMISSION_LIMIT`.

## 6. What DSL v2 cannot express, and why

These are capabilities the bead originally listed. The pinned Rust API has
no public form of them:

| Wanted | Why it is absent | Replacement |
|---|---|---|
| `consume_cost n` | Nothing charges `cost_quota`: `Budget::consume_cost` only edits a copy (`types/budget.rs:613`, `cx.rs:2493`). | A `cost_quota` budget on spawn, exhausted via `checkpoint`. |
| `park_until_woken` | No `Cx` park. | `notify_wait` (cancel-oblivious), or `oneshot_recv` / `watch_changed` (cancel-aware). |
| `mask` / `unmask` spanning awaits | `MaskGuard` is private; only `Cx::masked(closure)` (`cx.rs:3315`) and the poll-bounded `commit_section` (`combinator/bracket.rs:496`). | `masked` over non-blocking steps. |
| cancel or close any region from inside a task | The region command queue is crate-private (`runtime/spawn_mailbox.rs:2311`). | The task's own child regions (§3.4), or driver operations (§4). |
| `call` on a plain actor | The plain `Actor` trait has no call (`actor.rs:211`). | GenServer `server_spawn` / `call`. |
| allocation-failure injection, clock skew backwards or per task, scripted chaos inside a task | Not in the lab (`lab/runtime.rs:3357`; chaos is random, `lab/config.rs:325`). | `region_limits`, `advance`, driver operations between idle points. |
| `drain r` as an operation separate from close | The runtime's close always cancels and drains (`state.rs:4936`, crate-private). | `close_region` (§3.4, §4). |

## 7. Work this DSL requires on the C side (W1.5)

Each gap is a missing capability in the C runtime that W1.5 (bd-9kll.2.5)
must close. Some are also drift findings in their own right. The
interpreter (`src/conformance/interpreter.c`) fails a run that reaches an
open gap with the gap named; `make conformance` lists them as ERROR.

Open:

- **Non-parking `try_send` / `try_recv`.** C's `asx_channel_try_reserve`
  and `asx_channel_try_recv` park the calling task when the channel is
  full or empty inside a scheduler poll; Rust's `try_send` / `try_recv`
  never park. The interpreter fails these steps closed until the C `try_*`
  functions stop parking (their parking callers moving to the Cx-aware
  `asx_channel_reserve` / `asx_channel_recv`).
- **Multi-permit semaphore acquire.** Rust acquires `count` permits
  all-or-nothing; C grants one permit per acquire, so `sem_acquire` with
  `count` > 1 fails closed.
- **Region admission limits** for `region_limits`: no C API sets per-region
  task, child or obligation limits today. `ASX_E_ADMISSION_LIMIT` appears
  only in the status string table.

Closed (each verified by a fixture that now matches):

- **Oneshot, broadcast and watch**: `asx_oneshot_send` / `asx_oneshot_recv`
  (SendPermit obligation; the exact oneshot traces; value and close before
  cancel), `asx_broadcast_send` with a Cx (cancel first, Closed with no
  receiver, SendPermit) and `asx_broadcast_recv` (cancel first and traced,
  lag count), `asx_watch_changed_begin` / `asx_watch_poll_changed` with a
  Cx (the watch traces) (`oneshot-send-recv-001`, `broadcast-fanout-001`,
  `watch-changed-001`).

- **Channel obligations and cancel traces**: `asx_channel_reserve` with a
  task Cx registers a `SendPermit` (committed by send, aborted `Explicit`
  by abort and `Error` by a send to a closed receiver; the interpreter
  aborts a permit its body still holds at the end with `Cancel`, as
  Rust's drop does); `asx_channel_send` registers none; both cancel
  checks come first and record Rust's `user.trace` messages
  (`mpsc-two-phase-send-recv-001`, `mpsc-recv-cancel-first-001`).
- **Semaphore permit obligations**: a permit acquired with a task Cx
  registers a `SemaphorePermit` that release commits; mutex guards
  register none (`sync-semaphore-barrier-001`, `sync-mutex-fifo-001`).
- **Direct task cancels record no `cancel.requested`**, as Rust's
  `RuntimeState::cancel_task` (`cancel-masked-checkpoint-001`).
- **Cancel wakes, masking and Notify**: a cancel wakes a parked task once;
  a masked task does not observe its cancel; Notify skips only
  cancel-aware doomed waiters (`cancel-masked-checkpoint-001`,
  `cancel-strengthen-severity-001`).
- **Closed-region admission** reports `ASX_E_REGION_CLOSED`
  (`obligation-reserve-closed-region-must-fail-001`).

- **Cancel message and origin**: `asx_task_cancel_with_reason` and
  `asx_region_cancel` take a whole reason; budget cancels are attributed to
  the task and stamped (`budget-poll-quota-exhaustion-001`).
- **`region.cancelled` and `cancel.requested` emission** and Rust's
  `cancel_request` semantics: `asx_region_cancel` begins closing the
  subtree, chains ParentCancelled causes to the immediate parent, and
  regions finalize when their last task completes
  (`region-lifecycle-cancel-propagates-001`).
- **`user.trace` emission** for the `trace` step (`asx_trace_user`).
- **Timer events** for sleeps (`timers-same-deadline-001`,
  `quiescence-pending-timer-001`).
- **Sleep as a cancellation point**: `asx_sleep_poll` ends early on a cancel
  that is neither Timeout nor Deadline (one extra scheduler trip when first
  polled after it) and keeps sleeping through a budget deadline; idle
  scheduling no longer forces awake a cancelled task whose timer or join
  wake is pending (`budget-deadline-sleep-checkpoint-001`).
- **Panicked outcome**: `asx_task_panic(self, message)` completes the task
  PANICKED with its message (`task-lifecycle-panic-001`).
- **Abort reason**: `asx_obligation_abort_with_reason` records
  `Explicit`/`Cancel`/`Error` (`obligation-abort-reasons-001`).
- **Obligation leak**: a leaked obligation records `obligation.leaked`
  (`leak-policy-leak-reported-001`).
- **Close semantics**: the `close_region` step is a region cancel followed
  by waiting for Closed. Its remaining difference is Rust's: the lab
  runtime stamps the close with `CancelReason::user`'s testing defaults
  (`lab/runtime.rs:4195`), which `region-lifecycle-close-cancels-children-001`
  reports.
