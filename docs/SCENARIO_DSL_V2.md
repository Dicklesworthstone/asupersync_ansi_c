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
  `scheduler`, `timers`, `channel`, `mpsc`, `oneshot`, `broadcast`, `watch`, `sync`,
  `combinators`, `task_groups`, `actor`, `supervision` or `leak_policy`.
  `channel` groups channel-family scenarios; the specific primitive labels
  remain available. An mpsc declaration admits at most seven extra senders.
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
  - `futurelock_max_idle_steps` (optional, at least 1) maps to
    `LabConfig::futurelock_max_idle_steps` (`lab/config.rs:143`) and switches
    `panic_on_futurelock` off for the run, so Rust's `FuturelockDetected`
    events are compared instead of aborting it. In C it is
    `asx_lab_set_futurelock_check`. Without it the run keeps Rust's defaults
    (threshold 10000, panic on), a futurelock is a harness failure, and the
    C lab runs no check (`VOCABULARY_EXCLUSIONS.md`). §5 says what the
    scenario then expects.
  - `oracles` (optional) names C-side kernel-state oracles to run once when
    the run has ended, by Rust's registry names (`task_leak`,
    `obligation_leak`, `quiescence`, `finalizer`, `region_tree`,
    `deadline_monotone`, `cancellation_protocol`, `cancel_correctness`,
    `cancel_debt`, `region_leak`, `cancel_signal_ordering`,
    `channel_atomicity`, and the C-only `task_counters` and `arena_counters`)
    or `"all"`
    (`docs/LAB_ORACLE_CATALOG.md`). A violation fails the run with the
    oracle's message. Rust ignores the field (its `LabRuntime` runs the oracles
    it feeds regardless). `ASX_ORACLES=all` in the runner's environment does
    the same for every scenario of a run without editing them.
- **`requires`** (optional) lists capabilities the scenario needs that a
  C profile may forbid; the only one is `"unbounded_queue"`, for a scenario
  that declares an unbounded mpsc channel (§3.6, DEC-008). Rust ignores it.
  A C profile whose overload catalog forbids that capability
  (`ASX_FORBID_UNBOUNDED_QUEUE`: FREESTANDING, EMBEDDED_ROUTER, HFT and
  BROWSER) does not run the scenario: the runner skips it and counts it in
  `skipped_forbidden` of its summary. A skip is legal only for a capability
  the scenario declares and that the profile forbids; an unknown name is an
  error, a declared capability the profile allows runs, and a scenario that
  uses an unbounded channel without declaring it fails in a forbidding
  profile like any other refused creation.
- **`expect`** is `{"kind":"ok"}`, `{"kind":"futurelock"}` or
  `{"kind":"must_fail","error":{"task","step","status"}}`. `must_fail`
  asserts that the named step of the named task returns that status (§5).
  It is how the forbidden-behaviour catalog (`docs/FORBIDDEN_BEHAVIOR_CATALOG.md`)
  is covered. `futurelock` asserts that the run reports at least one
  futurelock, and needs `lab.futurelock_max_idle_steps` (§5).

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

### Region finalizers

A `regions` entry may declare `finalizers`, async finalizers registered on
that region (Rust `Scope::defer_async`):

```json
{"name": "r.a", "parent": "root", "budget": null,
 "finalizers": [
   {"name": "f.close", "program": [{"op": "yield"}, {"op": "sleep", "ns": 1000}]},
   {"name": "f.flush", "program": []}
 ]}
```

- **`name`** is the finalizer task's canonical name (vocabulary §2) and
  must be unique with every other region, task and finalizer name.
- **`program`** is an ordinary task program (§3), run with the finalizer
  task's own `Cx`. Rust's finalizer future takes none, so the harness
  reads it ambiently: the lab installs the polled task's Cx for the poll
  (`Cx::current()`, `lab/runtime.rs:4735`).
- **Registration.** At setup, right after the region is created and before
  any task, each entry is registered in array order: Rust
  `lab.state.register_async_finalizer(region, future)`
  (`runtime/state.rs:9101`; refused unless the region is neither closing
  nor closed), C `asx_region_defer_async(region, poll, data)`. The
  finalizer stack is LIFO, so the last entry declared runs first.
- **Execution.** When the region reaches Finalizing (its tasks and child
  regions are done), its finalizers run **one at a time**, each as a
  **masked task of that region**: Rust `drain_ready_async_finalizers`
  (`runtime/state.rs:8578`), called by the lab step after timers and I/O
  and before the pick (`lab/runtime.rs:4513`), which spawns the top
  finalizer with `spawn_finalizer_task_in` (`state.rs:8857`): admitted as
  cleanup work, budget `finalizer_budget()` (poll quota 100, priority
  128) with a deadline 5 s after the spawn
  (`FINALIZER_POLL_BUDGET`, `FINALIZER_TIME_BUDGET_NANOS`,
  `record/finalizer.rs:42-70`), scheduled at that priority, mask depth 1
  for its whole run (`MaskedFinalizer`, `state.rs:1781`). The next
  finalizer is spawned at a later step, after this one's task completed.
  The region closes when its stack is empty and its tasks are done.
- **Admission.** A normal spawn into a Finalizing region is refused
  (`SpawnError::RegionClosed`, `record/region.rs:134-142`, Rust commit
  `59bdc2baa`): a finalizer program that `spawn`s a child observes the
  refusal through `join` as `ASX_E_REGION_CLOSED` (§3.4), the child has no
  trace events and no snapshot entry. The finalizer's own task is admitted
  as cleanup work, and a task it never completes keeps the region
  Finalizing.
- **Observations and projection.** Each executed finalizer step is one
  observation under the finalizer's name (§2). The finalizer task is a
  task of the region: `task.spawned` and `task.completed` carry its
  name, and the snapshot lists it with its state and outcome. Its outcome
  follows the ordinary rules: a budget cancel (poll quota, deadline) is
  raised and not delivered while the mask holds, so a finalizer that
  outlasts its budget still runs to its end.
- **Return.** A finalizer program ends `ok` or, with `return err`, `err`;
  that body result is the task's outcome in both engines (a masked
  finalizer is never delivered a cancel). `return panicked` is a harness
  error: Rust reports a finalizer future's panic as its task's panic,
  which twin_run does not catch.
- **Driving it.** A finalizer is spawned by a lab *step*, so a scenario
  with finalizers finishes or cancels its region (`finish_region`,
  `cancel_region`) and lets the closing run (`run_with_auto_advance`)
  step. The driver op `close_region` ends each of its rounds in
  `run_until_idle`, which takes no step with an empty scheduler, so it
  never spawns a finalizer and fails with "did not close within 1000 idle
  rounds" in both engines.

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
     (`lab/runtime.rs:3428`), C `asx_scheduler_run` until no task is ready.
     Only a lab *step* fires due timers (`step_inner`, `:4509`), and an
     idle run takes none with an empty scheduler: while every task is
     parked, neither an entry's clock advance nor the next entry's idle run
     fires a timer that has come due. It fires once some task is runnable
     (a driver `cancel_task`, `cancel_region` or `close_region` schedules
     one) or in the finish phase's auto-advance;
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
    is 1-based. A `masked` step inside one extends the path, so a step in
    a nested mask is observed as e.g. `masked/2/masked/1/checkpoint`.
  - Programs of spawned children and of group members are separate tasks
    with their own names, so their steps are observed under those names.
  - A step that never runs (because the program ended earlier) produces
    nothing.

## 3. Task steps

Every step is an object with `"op"` and the op's fields. "Blocks" means the
step can suspend. "Cancel" says what a pending, unmasked cancellation does.

**Resources the task does not hold.** Some steps name something an earlier
step of the same task acquired:
- `commit`, `abort`, `leak` name an obligation (from `reserve`);
- `mutex_unlock` names a guard (from `mutex_lock`);
- `sem_release` names a permit (from `sem_acquire`);
- `permit_send`, `permit_abort` name a send permit (from `reserve_send`);
- `cancel_region`, `close_region` and a `spawn` with `region` name a child
  region (from `open_region`).
When the acquiring step failed (for example a reserve refused, a lock wait
cancelled, or an `open_region` refused because the task's region is
closing), or the resource was already resolved or released (a region
already closed), the step does nothing and is observed as `ASX_E_NOT_FOUND`
in both engines. A `spawn` into such a region spawns nothing, and `join`,
`try_join` and `abort_task` of its child observe `ASX_E_NOT_FOUND` too.
`lint-scenarios-v2` fails a curated scenario that names an obligation or
send permit no `reserve` / `reserve_send` in it acquires, so a misspelt name
cannot pass this way. A commit after an abort of the same obligation
observes `ASX_E_NOT_FOUND`: the abort consumed the token.

### 3.1 Control

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `yield` | — | `runtime::yield_now().await` (`runtime/yield_now.rs:36`) | return `ASX_E_PENDING` with the task left runnable | once | ignored |
| `checkpoint` | `on_cancel`: `"return"` (default) or `"continue"` | `cx.checkpoint()` (`cx.rs:2749`) | `asx_checkpoint(self, &cr)` | no | Acknowledges the cancel and returns `ASX_E_CANCELLED`. With `"return"` the program ends; with `"continue"` it goes on. A masked task gets `ASX_OK`. |
| `is_cancel_requested` | — | `cx.is_cancel_requested()` (`cx.rs:2641`) | `asx_cx_is_cancelled(&cx)` with the task's Cx | no | Observes `ASX_OK` with `value` `true` once a cancel was requested of the task, a budget cancel a checkpoint raised included, masked or not; acknowledges nothing. |
| `sleep` | `ns` | `time::sleep(cx.now(), Duration::from_nanos(ns)).await` (`time/sleep.rs:1182`) | `asx_task_wait_until(self, now + ns)` | until virtual time ≥ now+ns | If the kind is neither `Timeout` nor `Deadline` and the task is unmasked, the step checkpoints (acknowledging) and completes early with `ASX_OK`. If it is first polled after the cancel, it takes one extra scheduler trip (`sleep.rs:789-812`). Otherwise it keeps sleeping. |
| `sleep_until` | `at_ns` | `time::sleep_until(Time::from_nanos(at_ns)).await` (`sleep.rs:1203`) | `asx_task_wait_until(self, at_ns)` | until virtual time ≥ at_ns | as `sleep` |
| `trace` | `message` | `cx.trace(message)` (`cx.rs:3362`) | `asx_trace_user(self, message)`: vocabulary `user.trace` | no | ignored |
| `return` | `outcome`: `{"tag":"ok"}`, `{"tag":"err","status":S}` or `{"tag":"panicked","message":M}` | body returns `Body::Ok` / `Body::Err(code)` (a value), or `panic!(M)`; a supervised child's factory returns `Outcome::Ok(())` / `Outcome::Err(code)` itself, as does a group member's | poll function completes with the outcome; panicked through `asx_task_panic(self, M)` | no | The cancellation-wins rule above applies. **An `err` return of a declared task or a `spawn`ed child is a value, not an error exit**: Rust completes the task `Ok` (`run_state_task_to_terminal`, `state.rs:94-129`) and its handle's join yields the error, so the task's monitors and links see a normal exit (`DownReason::from_task_outcome`, `monitor.rs:136`). C ends such a task Ok too and keeps the status as the program's value (`it_task.value_err`), reported as the outcome `err`; a supervised child's, a group member's and a race wrapper's `err` is a runtime error. A scenario that needs an abnormal exit for a watcher uses `panicked`, a cancellation, or a supervised child. |

### 3.2 Masking

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `masked` | `steps`, optional `depth` (1 to 80, default 1) | `cx.masked(\|\| { …steps… })` (`cx.rs:3315`), `depth` times nested around the steps | `asx_task_mask(self)` `depth` - 1 times, then `asx_task_masked` around the steps (mask, steps, unmask); the outer masks are unmasked after | no: `steps` may contain only non-blocking steps, `masked` among them | Inside, `checkpoint` returns `ASX_OK` (`rule.cancel.checkpoint_masked`), also after a nested `masked` ends while an outer one still holds (`cancel-masked-nested-001`). `depth` nests that many masks without nesting the document (the C parser's JSON depth limit, 64 levels, allows only about 29 nested `masked` steps). A mask beyond 64 on the task is refused: Rust asserts `mask_depth < MAX_MASK_DEPTH` on entry (`cx.rs:3323`), which panics the task before any step of the section runs (no observation for the step; outcome Panicked with the assertion's message, `cancel-mask-depth-65-panics-001`), and C's `asx_task_mask` returns `ASX_E_INVALID_STATE`, which the interpreter ends the task on with the same message. Depth 64 is allowed (`cancel-mask-depth-64-allowed-001`). |

Rust has no mask that spans an await (`MaskGuard` is private, `cx.rs:706`),
so a blocking step inside `masked` is a schema error.

### 3.3 Obligations

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `reserve` | `kind` (`SendPermit`, `Ack`, `Lease`, `IoOp`, `SemaphorePermit`, `Transaction`), `as` | `cx.try_register_obligation_checked(kind, cx.task_id())` → `Ok(Some(token))` (`cx.rs:1746`) | `asx_obligation_reserve_ex(own_region, kind, self, &id)` | no | Ignored. A region that is no longer Open gives `ASX_E_REGION_CLOSED` (Rust `ObligationAdmissionError::RegionClosed`, `runtime/obligation_mailbox.rs:68`). |
| `commit` | `obligation` | `token.commit()` (`obligation_mailbox.rs:879`) | `asx_obligation_commit(id)` | no | ignored |
| `abort` | `obligation`, `reason` (`Cancel`, `Error`, `Explicit`) | `token.abort(reason)` (`:888`) | `asx_obligation_abort_with_reason(id, reason)` | no | ignored |
| `leak` | `obligation` | `drop(token)`, which posts a Leak (`:897`) | `asx_obligation_drop(id)`: leaked when the poll returns, under the leak policy | no | ignored |
| `handoff` | `obligation`, destination `task`, destination-local `as` | `token.try_transfer(&destination_cx)` (`obligation_mailbox.rs:706`); refusal recovers the original token with `into_parts()` | `asx_obligation_handoff(id, destination)` | no | A cancelled destination refuses with `ASX_E_CANCELLED`, including after it completed. |

`kind` is required: C's `ASX_OBLIGATION_KIND_GENERIC` has no Rust counterpart
and is not allowed. Both runtimes apply a poll's obligation operations, in
order, when the poll returns (Rust's obligation mailbox; C's posts under lab
dispatch, bd-2fga): the trace events come then. A `reserve` step, and the
commit, abort or drop of its token, count against the region's
`max_obligations` at the call; a permit's registration (semaphore, channel,
oneshot, a server call) and its resolutions count when applied. A body that
ends holding tokens drops them first, as twin_run's locals do.

An accepted `handoff` consumes the source-local name and delivers the same
logical obligation under the destination-local name. The original canonical
name (`source/oN`) stays unchanged. Destination token stores belong to their
task futures, including before first poll; shared lookup holds only weak
references. Destination body destruction therefore drops a received token
with that body, rather than retaining it in the driver. An existing destination
name is a harness error. A missing source-local token observes `ASX_E_NOT_FOUND`.
Same-holder transfer refuses with `ASX_E_INVALID_ARGUMENT`; a completed
uncancelled destination gives `ASX_E_INVALID_STATE`; closed destination
admission gives `ASX_E_REGION_CLOSED`; a full cross-region quota gives
`ASX_E_ADMISSION_LIMIT`. Refusal preserves the source token and emits no handoff.
Same-region handoff reuses its existing quota credit. Declared tasks have
their actual task-record Cx available before first poll; spawn-factory contexts
are published when their admitted futures are constructed.

The C raw-ID application API requires the actual holder's current poll.
Tracked primitive credits instead require the genuine physical permit, which
may move between tasks without changing holder liability, as a plain Rust
owned-token move does. Only explicit checked-token handoff reassigns liability.
Wrong-task raw-ID misuse is direct-C coverage: Rust's affine token API cannot
express it. These captures cover the cooperative Lab model, not native
concurrent transfer interleavings.

### 3.4 Spawning, joining, child regions

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `spawn` | `as`, `program`, optional `budget`, optional `region` (a child region this task opened) | `cx.spawn(…)` (`cx.rs:4880`). A `budget` uses `cx.spawn_in(&cx.scope_with_budget(b), …)` (`:4976`, `:4547`): its deadline and quotas are clamped to the spawner's, its priority is `b`'s. `region` uses `child.cx().spawn(…)` (`cx/child_region.rs:334`), and a `budget` beside it is ignored. | `asx_task_spawn_with_budget(region, interpreter_poll, program, budget, &id)` | no | ignored. A region that is closing or closed refuses the child, observed as `ASX_E_REGION_CLOSED`: C refuses at the spawn; Rust accepts the spawn into its mailbox and admission refuses it later (`SpawnError::RegionClosed`, `runtime/state.rs:1702`), resolving the handle as `JoinError::Cancelled`, which twin_run projects onto the spawn step. A refused child has no trace events and no snapshot entry. |
| `join` | `task` | `handle.join(&cx).await` (`runtime/task_handle.rs:793`) | `asx_task_join_poll(self, target, &outcome)` until done | until the child completes | Ignored: join is uninterruptible (`task_handle.rs:1080-1126`). The observation `value` is the child's outcome (vocabulary §4); a refused child (`spawn` above) is observed as `ASX_E_REGION_CLOSED` with no value. |
| `try_join` | `task` | `handle.try_join()` (`:863`) | `asx_task_join(target, &outcome)` | no | ignored. A running child gives `ASX_E_TASK_NOT_COMPLETED`; a refused child `ASX_E_REGION_CLOSED` once the next lab step's admission refused it, and before that (in the poll that spawned it) `ASX_E_TASK_NOT_COMPLETED`, as Rust's `try_join` reports not-ready while the admission is pending (`asx_task_refusal_delivered`). |
| `abort_task` | `task`, `kind`, optional `message` | `handle.abort_with_reason(reason)` (`task_handle.rs:1005`), reason attributed to the requester (§4, "Reason attribution") | `asx_task_abort_request(target, &reason)` | no; the next lab step applies it (`drain_handle_cancel_requests`). A child spawned in the same step caches it until its admission, which queues it as one handle-cancel command behind those already queued (`runtime/spawn_mailbox.rs:629-660`). A refused child's handle only caches it: `ASX_OK`, and its spawn and joins then observe `ASX_E_REGION_CLOSED` (§4, `region_limits`). `twin_run generate` aborts only a child its parent spawned before the parent's latest yielding step (`yield` or `sleep`): an abort of a spawn still pending admission, whose region closed first, leaves Rust's handle unresolved for good and the capture fails closed (bd-c129; gen-275-53). | ignored |
| `open_region` | `as`, optional `budget` | `cx.open_child_region(ChildRegionSpec::inherit().with_budget(b)).await` (`cx.rs:4491`; `child_region.rs:76`, `:87`) | `asx_region_open_child_poll(self, own_region, budget, &id)` until done | parked until the next lab step applies the Create command and wakes it | ignored. A closing parent gives `ASX_E_REGION_CLOSED`. |
| `cancel_region` | `region` (one this task opened), `kind`, optional `message` | `child.cancel(reason)` (`child_region.rs:381`), reason attributed to the requester (§4, "Reason attribution") | `asx_region_cancel_request(region, &reason)` | no; the next lab step applies the Cancel command | ignored |
| `close_region` | `region` (one this task opened) | `child.close().await` (`child_region.rs:424`): a Close command, then a wait for Closed | `asx_region_close_poll(self, region)` until done: the next lab step applies the Close (a region cancel with the User reason Rust's Close carries, `"owned child region body finished"`, `lab/runtime.rs:4195`); the region's Closed transition wakes the task | parked until Closed | Ignored: the closer's own cancellation is not observed. |

A task can cancel or close only regions it opened itself. Rust's region
command queue is crate-private (`runtime/spawn_mailbox.rs:2311`).
Cancelling or closing any other region is a driver operation (§4).

Region commands take effect at a lab step boundary, as in Rust
(`drain_region_commands`, `lab/runtime.rs:4135-4217`): a step first admits
the children spawned during the previous one, then applies up to 8 queued
commands in order, then draws its value and picks a task. A child region a
task still holds when its program ends is closed by the drop backstop
(`ChildRegion`'s `Drop`, `child_region.rs:517`): a Close command with no
wait, queued for each held region in name order (twin_run keeps them in a
`BTreeMap`).

### 3.5 Task groups

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `race` | `members` (list of programs), optional `deadline_ns` | spawn each member with `cx.spawn`, then `cx.scope().race_all(&cx, handles).await` (`cx/scope.rs:1306`). A `deadline_ns` runs this inside `cx.scope().timeout(&cx, d, …)` (`:2130`). | `asx_task_group_init(g, RACE, 1)`, `asx_task_group_spawn` per member, then `asx_task_group_poll` | until a winner, or until all are drained | The owner's cancel before a winner, once its checkpoint observes it, aborts every unfinished member with the owner's own reason and joins them in order; the outcome is that cancellation (or the first member panic). Losers are always cancelled and drained (`inv.combinator.loser_drained`). The status is `ASX_OK`; the `value` is `{winner_index, outcome}`, the winner's outcome, with `winner_index` null when the winner was cancelled or panicked (Rust's `Err` carries no index); a winner whose program returned, ok or err, has its index (Rust's `Ok((body, index))`). |
| `join_all` | `members` | `cx.scope().join_all(&cx, handles).await` (`:1479`) | group mode JOIN_ALL: the owner waits on one member at a time, in order | until all complete | Ignored: join_all joins one by one and its joins are uninterruptible, so the owner's cancel reaches no member. The `value` is the list of member outcomes. |
| `first_ok` | `members` | `cx.scope().first_ok(&cx, factories).await` (`:2023`): one attempt at a time, in order | group mode FIRST_OK with `asx_task_group_add_attempt` per member | until an attempt succeeds, one ends cancelled or panicked, or all fail | The owner's cancel is passed to the running attempt (which is still awaited) and stops further attempts. Status: `ASX_OK`; all failed: the first attempt's error; `ASX_E_CANCELLED`; panicked: `ASX_E_INVALID_STATE`. No value. Attempts that never start have no name. |
| `quorum` | `members`, `needed` | `cx.scope().quorum(&cx, needed, branches).await` (`:1811`) | `asx_task_group_init(g, QUORUM, needed)` | until `needed` succeed or that becomes impossible | The owner's cancel drains the members with the owner's own reason; status `ASX_E_CANCELLED`. Met: `ASX_OK`, `value` = the number of members that succeeded. Members run cancellation-dominant. A `QuorumError` (vocabulary §5): a panicked member gives `ASX_E_INVALID_STATE` even when met; otherwise not met gives `ASX_E_CANCELLED` if a member was cancelled by anything but the quorum's own drain, else the first failing member's error; no value. `needed` of 0 or above the member count: `ASX_E_INVALID_ARGUMENT` and nothing is spawned. |

A member program ends with `return` to give its outcome. The default is Ok.
Members are named `"<owner>/g<s>.<i>"` (vocabulary §2) and are not in the
snapshot (vocabulary §6).

A member spawned into the owner's region after that region was cancelled
or began closing is refused, but it is still a member. Rust's spawn mailbox
keeps it until the next step's admission refuses it (`runtime/state.rs:1702`).
The denial resolves its join as `Cancelled` with a `ParentCancelled`
reason, testing-default attribution (`lab/runtime.rs:3994-4008`). The abort
reasons the group sent it strengthen that reason (`task_handle.rs:544-554`).
The group counts it as a cancelled member: `join_all` lists it, `quorum`
counts a failure, and `first_ok` ends cancelled. C:
`asx_task_group_member_refusal`.

A `race` with `deadline_ns` runs as Rust's `Scope::timeout` runs it
(`cx/scope.rs:2130-2196`):
- The race runs in a wrapper task spawned into the owner's region, named
  `"<owner>/g<s>.timeout"` (vocabulary §2). The members keep their
  plain-race names, and the snapshot lists neither the wrapper nor the
  members.
- Each owner poll checks the wrapper's join, then the owner's own cancel (a
  checkpoint), then the deadline. The deadline is a sleep timer of the
  owner's (`"<owner>/tm<k>"`), registered at the first poll.
- At the deadline the owner aborts the wrapper with `CancelReason::timeout()`
  (default attribution). On the owner's own cancel it aborts the wrapper
  with the owner's reason. Either way the owner then joins it.
- The wrapper's race sees that abort as its owner's cancel and drains the
  members with that reason. A `Timeout` reason does not end a member's
  sleep, so the members finish their sleeps.
- The observation is the race's (status `ASX_OK`, `{winner_index,
  outcome}`). A wrapper that ended cancelled without running the race, and
  only after the deadline, is `TimedOut`: status `ASX_E_TIMED_OUT`, no
  value.
- A deadline timer still pending when the step ends is cancelled.

C: `exec_race_deadline` in the interpreter.

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

An mpsc or a broadcast may list more producers in `senders` (top-level
tasks, distinct from `sender` and each other): each gets a clone of the
sender at setup, in order (Rust `tx.clone()`, `channel/mpsc.rs:1231`,
`channel/broadcast.rs:577`; C `asx_channel_sender_clone`,
`asx_broadcast_sender_clone`). Each owner's `close_sender`, or the end of
its body, drops its own clone (C `asx_channel_sender_drop` by the clone's
identity, `asx_channel_close_sender` for the declared sender); the sender side closes at the last one
(`Sender::drop`, `mpsc.rs:1241-1255`, `broadcast.rs:586-608`).

```json
{"name": "ch.jobs", "type": "mpsc", "capacity": 2, "sender": "t.a", "senders": ["t.b"], "receiver": "t.r"}
```

An mpsc channel may be unbounded instead of having a `capacity`:
`{"name": "ch.q", "type": "mpsc", "unbounded": true, "sender": "t.p",
"receiver": "t.c"}` (and `senders` as above). Rust: `mpsc::unbounded_channel()`,
which is `channel(usize::MAX)`, so every mpsc step below applies and a send
never waits. C: `asx_channel_create_unbounded`, bounded only by the arena
(`ASX_CHANNEL_MAX_CAPACITY` messages; a scenario queues fewer: running out
reports `ASX_E_RESOURCE_EXHAUSTED`, which Rust cannot return, so the compare
fails). `send_evict_oldest`
and `closed` do not exist on Rust's `UnboundedSender`, so they are harness
errors on such a channel. A scenario declaring one lists
`"requires": ["unbounded_queue"]` (§1).

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `reserve_send` | `channel`, `as` | `tx.reserve(&cx).await` (`channel/mpsc.rs:637`) | `asx_channel_reserve(id, cx, &permit)` until done | while full, FIFO | Checked on every poll: `ASX_E_CANCELLED` even when there is room (`mpsc.rs:1061`). The permit registers a `SendPermit` obligation. |
| `permit_send` | `permit`, `value` | `permit.send(v)` (`:1565`) | `asx_send_permit_send(&permit, v)` | no | ignored |
| `permit_abort` | `permit` | `permit.abort()` (`:1633`) | `asx_send_permit_abort(&permit)` | no | ignored. The obligation is aborted with reason `Explicit`. |
| `send` | `channel`, `value` | `tx.send(&cx, v).await` (`:715`) | `asx_channel_send(ch, &cx, v)`: no obligation, as Rust's transient permit | while full | `ASX_E_CANCELLED`. No obligation and no trace event. |
| `try_send` | `channel`, `value` | `tx.try_send(v)` (`:773`) | `asx_channel_try_reserve` + `asx_send_permit_send`, without an obligation | no | ignored. Full gives `ASX_E_CHANNEL_FULL`. |
| `recv` | `channel` | `rx.recv(&cx).await` (`:1719`) | `asx_channel_recv(id, cx, &v)` until done | while empty | Checked first: `ASX_E_CANCELLED` even when a value is queued (`:1786`). The observation `value` is the received integer. |
| `try_recv` | `channel` | `rx.try_recv()` (`:1956`) | `asx_channel_try_recv` (never parks) | no | ignored. Empty gives `ASX_E_CHANNEL_EMPTY`; closed gives `ASX_E_DISCONNECTED`. |
| `send_evict_oldest` | `channel`, `value` | `tx.send_evict_oldest(v)` (`:872`) | `asx_channel_send_evict_oldest` (never parks, no permit) | no | ignored. The observation `value` is the evicted message, or null when there was room. A free slot owned by a parked producer, or a channel whose every slot is reserved, gives `ASX_E_CHANNEL_FULL`. |
| `recv_many` | `channel`, `limit` | `rx.recv_many(&cx, &mut buf, limit).await` (`:1754`) | `asx_channel_recv_many(id, cx, buf, limit, &n)` until done | while empty | Checked first, after a `limit` of 0 (which gives `[]` at once): `ASX_E_CANCELLED`, tracing `"mpsc::recv_many cancelled"` (`:1882`). The observation `value` is the array of received integers, oldest first; a channel that can get no more messages gives `ASX_OK` with `[]`. |
| `closed` | `channel` (the sender's) | `tx.closed().await` (`channel/mpsc_closed.rs:39`) | `asx_channel_closed` until done | until the receiver is closed or dropped | ignored: no `Cx`. Queued messages and reservations are untouched. |
| `close_sender` | `channel` | `drop(tx)` (`:1241`). With a permit of the channel held, a Rust body cannot drop the sender it borrows (`SendPermit<'a>`, `:1461`): the drop is projected after the permit, twin_run keeping the closed sender until the last permit the task holds on the channel is sent, aborted or dropped (or the body ends, permits first), and the task's steps see the sender closed from the step on | `asx_channel_close_sender`; with a permit held, the interpreter does the same, calling it when the last permit of the channel is gone | no | ignored. A receiver is not told the channel is closed while the drop waits (`mpsc-sender-closed-with-permit-held-send-001`, `-abort-001`, `-body-end-001`) |
| `close_receiver` | `channel`, optional `receiver` (a watch clone) | `drop(rx)` (`:2122`) | `asx_channel_close_receiver` | no | ignored. Discards what the queue holds, also after a `seal`. |
| `seal` | `channel` (mpsc, the receiver's) | `rx.close()` (`:1698`) | `asx_channel_seal` | no | ignored. Sends and reservations fail with `ASX_E_DISCONNECTED` and parked producers and `closed` waiters wake; the receiver stays held and the queue receivable, then receives see `ASX_E_DISCONNECTED` (`mpsc-seal-drains-001`). A second `seal` is a harness error: Rust's `close()` would return without effect, C's `asx_channel_seal` refuses it. |
| `oneshot_send` | `channel`, `value` | `tx.send(&cx, v)` (`channel/oneshot.rs:556`): reserves a SendPermit, traces `"oneshot::reserve creating permit"`, commits | `asx_oneshot_send(&tx, &cx, v)`: reserves the SendPermit obligation, traces, commits | no | Checked first: `ASX_E_CANCELLED`. |
| `oneshot_recv` | `channel` | `rx.recv(&cx).await` (`oneshot.rs:1250`): traces `"oneshot::recv received value"` / `"…channel closed"` | `asx_oneshot_recv(rx, cx, &v)` until done | until sent or dropped | `ASX_E_CANCELLED`, tracing `"oneshot::recv cancelled while waiting"`. |
| `oneshot_poll_closed` | `channel`, `end` (`sender` / `receiver`) | `poll_fn(\|c\| tx.poll_closed(c)).await` / the receiver's (`oneshot.rs:619`, `:1373`) | `asx_oneshot_poll_closed` / `asx_oneshot_receiver_poll_closed` until done | the sender's until the receiver is dropped; the receiver's until the channel is closed (sender gone, no permit, no value) | ignored: no `Cx`. The sender's wait is woken only by the receiver drop; the receiver's by the close and by the value received, never by a send. |
| `oneshot_is_closed` | `channel`, `end` | `tx.is_closed()` (`:596`) / `rx.is_closed()` (`:1357`) | `asx_oneshot_sender_is_closed` / `asx_oneshot_receiver_is_closed` | no | ignored. Observes `ASX_OK` with a boolean `value`. |
| `oneshot_is_ready` | `channel` (the receiver's) | `rx.is_ready()` (`:1350`) | `asx_oneshot_receiver_is_ready` | no | ignored. Observes `ASX_OK` with a boolean `value`. |
| `broadcast_send` | `channel`, `value` | `tx.send(&cx, v)` (`channel/broadcast.rs:494`) | `asx_broadcast_send` | no | `ASX_E_CANCELLED` (`broadcast.rs:418`). The observation `value` is the number of receivers reached; no receiver left gives `ASX_E_DISCONNECTED` (Rust `Closed`). |
| `broadcast_recv` | `channel` | `rx.recv(&cx).await` (`:894`) | `asx_broadcast_recv(rx, cx, &v)` until done | while empty | `ASX_E_CANCELLED`. Lagged gives `ASX_E_LAGGED` with `value` = n. |
| `watch_send` | `channel`, `value` | `tx.send(v)` (`channel/watch.rs:488`) | `asx_watch_send` | no | ignored (no `Cx`) |
| `watch_changed` | `channel`, optional `receiver` | `rx.changed(&cx).await` (`:722`), then `borrow_and_update` (`:917`) | `asx_watch_poll_changed` / `asx_watch_recv` | until a newer version | `ASX_E_CANCELLED` (`:736`). The `value` is the new value. A closed sender with no unseen version gives `ASX_E_DISCONNECTED`. |
| `watch_wait_for` | `channel`, `cmp`, `value`, optional `receiver` | `rx.wait_for(&cx, \|v\| *v <cmp> value).await` (`:858`); `cmp` is `eq`, `ne`, `lt`, `le`, `gt` or `ge` | `asx_watch_poll_wait_for` | until a published value passes | The predicate runs on the current value first and under no cancellation check, so a matching value returns at once even for a cancelled task, with no trace. Otherwise each rejected value opens a `changed()` on a clone of the receiver (traces `"watch::changed starting wait"`, then `"received update"` per newer version, `"cancelled"` / `"sender dropped"` at the end). `ASX_E_CANCELLED`; closed sender and no match: `ASX_E_DISCONNECTED`. The `value` is the accepted one; only that version is marked seen, by the receiver. While the wait is open the clone counts in `watch_receiver_count`. |
| `watch_send_modify` | `channel`, `add` | `tx.send_modify(\|v\| *v = v.wrapping_add(add))` (`:530`) | `asx_watch_send_modify` | no | ignored (no `Cx`). One new version even when `add` is 0. |
| `watch_borrow` | `channel`, optional `receiver` | `rx.borrow()` (`:904`) | `asx_watch_borrow` | no | ignored. The `value` is the current value; nothing is marked seen. |
| `watch_borrow_and_update` | `channel`, optional `receiver` | `rx.borrow_and_update()` (`:917`) | `asx_watch_recv` | no | ignored. The `value` is the current value, marked seen. |
| `watch_mark_seen` | `channel`, optional `receiver` | `rx.mark_seen()` (`:961`) | `asx_watch_mark_seen` | no | ignored. Acknowledges the latest version, not an earlier borrow. No value. |
| `watch_has_changed` | `channel`, optional `receiver` | `rx.has_changed()` (`:970`) | `asx_watch_has_changed` | no | ignored. Observes `ASX_OK` with a boolean `value`. |
| `watch_is_closed` | `channel`, `end` (`sender` / `receiver`), optional `receiver` | `tx.is_closed()` (`:634`, no receiver left) / `rx.is_closed()` (`:977`, sender dropped) | `asx_watch_sender_is_closed` / `asx_watch_receiver_is_closed` | no | ignored. Observes `ASX_OK` with a boolean `value`. |
| `watch_receiver_count` | `channel` (the sender's) | `tx.receiver_count()` (`:627`) | `asx_watch_receiver_count` | no | ignored. The `value` is the count, a `wait_for` clone included. |
| `watch_clone` | `channel`, `as`, optional `receiver` | `rx.clone()` (`:1057`) | `asx_watch_receiver_clone` | no | ignored. The clone has seen what its source has seen and is held by the task under the name `as`; later watch steps select it with `receiver`. A name is used once per channel. |

The `receiver` field of the watch receiver steps (`watch_changed`,
`watch_wait_for`, `watch_borrow`, `watch_borrow_and_update`,
`watch_mark_seen`, `watch_has_changed`, `watch_is_closed` with `end`
`receiver`, `watch_clone`) and of `close_receiver` names a clone made by
`watch_clone` instead of the task's declared receiver. A task's clones are
dropped with the rest of its endpoints when its body ends. `mark_changed`
is not in the pinned Rust (`channel/watch.rs`), and `borrow_and_clone` /
`borrow_and_update_clone` are `watch_borrow` / `watch_borrow_and_update` for
integer values.

### 3.7 Synchronization

Sync objects are declared at the top level:

```json
{"name": "m", "type": "mutex"}
{"name": "rw", "type": "rwlock"}
{"name": "s", "type": "semaphore", "permits": 2}
{"name": "b", "type": "barrier", "parties": 2}
{"name": "n", "type": "notify"}
```

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `mutex_lock` | `mutex` | `m.lock(&cx).await` (`sync/mutex.rs:191`), guard kept | `asx_mutex_lock_begin` + `asx_mutex_poll_lock` | while held, FIFO | Checked on every poll: `ASX_E_CANCELLED` even when free (`mutex.rs:554-558`). |
| `mutex_lock_until` | `mutex`, `deadline_ns` | `m.lock_until(&cx, Time::from_nanos(deadline_ns)).await` (`sync/mutex.rs:207`); the guard (borrowed, not `Send`) is dropped in the poll that takes it | `asx_mutex_lock_until_begin` + `asx_mutex_poll_lock`, then `asx_mutex_unlock` | as `mutex_lock`, until the deadline | as `mutex_lock`. The deadline Sleep is polled right after the checkpoint (`mutex.rs:560-564`): `ASX_E_TIMED_OUT` at or past `deadline_ns`, even when the lock is free; before it, its timer is scheduled on the first poll and cancelled when the wait ends otherwise. |
| `mutex_unlock` | `mutex` | `drop(guard)` (`:758`) | `asx_mutex_unlock` | no | ignored |
| `rwlock_read` / `rwlock_write` | `rwlock` | `OwnedRwLockReadGuard::read(lock, &cx).await` / `OwnedRwLockWriteGuard::write(lock, &cx).await` (`sync/rwlock.rs:1155`, `:1212`), guard kept | `asx_rwlock_read_begin` / `asx_rwlock_write_begin` + `asx_rwlock_poll_read` / `asx_rwlock_poll_write` | while a writer holds or (for a read) waits; release order is bounded writer-preference by arrival (`release_writer`, `:531`) | Checked on every poll: `ASX_E_CANCELLED` (`:1339`, `:1464`). A granted lock the waiter gives up passes on. |
| `rwlock_unlock` | `rwlock` | drops the task's most recent guard on the lock (`release_reader` / `release_writer`) | `asx_rwlock_read_unlock` / `asx_rwlock_write_unlock` | no | ignored |
| `rwlock_downgrade` | `rwlock` | the task's most recent write guard on the lock becomes a read guard in place (`OwnedRwLockWriteGuard::downgrade`, `sync/rwlock.rs:1287`) | `asx_rwlock_downgrade` | no | ignored. No write guard held on it: `ASX_E_NOT_FOUND`. Queued readers ahead of the first queued writer are granted with it. |
| `sem_acquire` | `semaphore`, `count` | `s.acquire(&cx, n).await` (`sync/semaphore.rs:430`), permit kept | `asx_semaphore_acquire_many_begin(s, n)` + `asx_semaphore_poll_acquire` | until n are available (all-or-nothing, FIFO) | `ASX_E_CANCELLED` (`:847`); count 0 succeeds at once with an empty permit and no obligation. Otherwise the permit registers a `SemaphorePermit` obligation. |
| `sem_release` | `semaphore` | `drop(permit)` (`:1080`) | `asx_semaphore_release` | no | ignored |
| `sem_forget` | `semaphore` | `permit.forget()` on the task's latest permit (`:1041`): the permits never return and its obligation is aborted (Explicit) | `asx_semaphore_forget` | no | ignored |
| `sem_add_permits` | `semaphore`, `count` | `s.add_permits(count)` (`:625`): saturating; wakes the front waiter if it can now run | `asx_semaphore_add_permits` | no | ignored |
| `barrier_wait` | `barrier` | `b.wait(&cx).await` (`sync/barrier.rs:135`) | `asx_barrier_wait_begin` + `asx_barrier_poll_wait` | until `parties` have arrived | `ASX_E_CANCELLED` (`:304`) |
| `notify_wait` | `notify` | `n.notified().await` (`sync/notify.rs:347`) | `asx_notify_wait_begin` + `asx_notify_poll_wait` | until notified | **Ignored**: a cancelled waiter stays parked until notified. Avoid pairing it with cancellation unless that is the point. |
| `notify_one` | `notify` | `n.notify_one()` (`:436`); stores a permit if nobody waits | `asx_notify_one` | no | ignored |
| `notify_all` | `notify` | `n.notify_waiters()` (`:471`) | `asx_notify_all` | no | ignored |

### 3.8 Actors and supervision

These steps use built-in behaviours, so programs never define handlers.

- **`counter`:** state is a u64 starting at 0; a cast adds its value; a call
  returns the state.
- **`echo`:** a call returns its request; a cast does nothing.
- **`latest`:** a `counter` whose `cast_overflow_policy` is `DropOldest`
  (`gen_server.rs:212`): `try_cast` into a full mailbox evicts the oldest
  queued cast instead of refusing.
- **`budgets`:** a `counter` whose `on_start_budget` is 7 polls at priority
  200 and whose `on_stop_budget` is the trait's default (`Budget::MINIMAL`,
  100 polls at priority 128). Its start and stop hooks trace the task
  budget they run under, `probe:start polls=<n> priority=<p>` and
  `probe:stop polls=<n> priority=<p>`, so a budget the phase installed (met
  with the task's, `gen_server.rs:96-160`) shows in the trace.
- **`budgets_stop`:** `budgets` with `on_stop_budget` 42 polls at priority
  250.

`counter` and `echo` define no `on_start` or `on_stop`, so their trace shows
the server loop's own `gen_server::*` user traces
(`include/asx/actor/actor.h`).
A server is only usable by the task that spawned it (twin_run keeps the
handle in that task's locals), and its task is named by `as`. Its
snapshot outcome is its join's (vocabulary §6).

| op | Fields | Rust | C | Blocks | Cancel |
|---|---|---|---|---|---|
| `server_spawn` | `as`, `behavior`, `mailbox` | `cx.spawn_gen_server(server, cap)` (`gen_server.rs:2511`) | `asx_actor_spawn(&h, own_region, behavior, state, mailbox)` | no | ignored; a spawn its admission refuses is `ASX_OK`, its server never runs |
| `cast` | `server`, `value` | `h.cast(&cx, msg).await` (`gen_server.rs:1268`) | `asx_actor_cast(h, &cx, value, &op)` | while the mailbox is full | Checked first: `ASX_E_CANCELLED`; a stopping server: `ASX_E_DISCONNECTED`. |
| `try_cast` | `server`, `value` | `h.try_cast(msg)` (`:1305`): applies the server's overflow policy, never waits | `asx_actor_try_cast(h, value)` | no | ignored (it checks no cancellation). Status: `ASX_OK`, `ASX_E_CHANNEL_FULL` (Reject policy, full mailbox), `ASX_E_DISCONNECTED` (a stopping server). Under `DropOldest` a full mailbox evicts the oldest queued cast and succeeds, tracing `gen_server::cast_evicted_oldest` on the calling task. |
| `server_evicted` | `server` | `h.evicted_count()` (`:1153`) | `asx_actor_evicted_count(h)` | no | ignored. The observation `value` is the number of casts evicted under `DropOldest`. |
| `call` | `server`, `request` | `h.call(&cx, req).await` (`:1171`) | `asx_actor_call(h, &cx, request, &op, &reply)` | until the reply arrives | Checked first: `ASX_E_CANCELLED`; a stopping server: `ASX_E_DISCONNECTED`; a dropped call: `ASX_E_INVALID_STATE` (NoReply). The caller must not be in `"root"` (Rust rejects root-region callers, `gen_server.rs:1086-1094`); both interpreters reject such a scenario as a scenario error before running it. |
| `server_stop` | `server` | `h.stop()` (`:1453`), then `h.join(&cx)` (`:1489`) | `asx_actor_stop`, then `asx_actor_join` | until its task finished | ignored; a server that never ran: `ASX_E_CANCELLED`. Later steps may not name the server. |
| `supervise` | `as`, `policy` (`one_for_one`, `one_for_all`, `rest_for_one`), optional `escalation` (`stop`, the default, or `escalate`), `max_restarts` (at most 32), `window_ns`, optional `storm_threshold_milli` (a positive integer: restarts per second times 1000), `children` (1 to 8 `{name, mode: permanent\|transient\|temporary, program, optional depends_on: [sibling names], optional required: bool, optional registration: {name, collision: fail\|replace}}`) | `SupervisorBuilder::new(name).with_restart_policy(p)`, one `.child(ChildSpec::new(child, …)[.depends_on(dep)…][.with_required(false)][.with_registration(Register{name, collision})])` each (`:694`), `.compile()?` (`supervision.rs:894`, `:924`, `:655`, `:973`); `.bind_managed(bindings, SupervisionConfig::new(max, window).with_restart_policy(p).with_escalation(e)[.with_storm_threshold(milli / 1000.0)])` (`:2093`, `:470`, `:491`, `:505`, `:483`) with one `ManagedChildBinding::new(child, mode, factory)` each (`:1900`); then `ManagedSupervisor::spawn(&cx)` (`:3497`), which twin_run performs itself to keep the report | `asx_supervisor_spawn(&h, own_region, &config, specs, n)`, the config's backoff and budget left at `asx_supervisor_config_init`'s (Rust's defaults) and its `storm_threshold` set to `milli / 1000.0` | no | ignored |
| `supervisor_join` | `supervisor` | `handle.join().await` (`supervision.rs:3429`) | `asx_supervisor_join(h, self, &report)` until it stops returning `ASX_E_PENDING` | until the controller published its report | Ignored: the join is not interrupted. The observation `value` is the report (below); a controller that never ran gives `ASX_E_CANCELLED` and no value. |

A supervised child's program runs once per generation, as the task
`<supervisor>/<child>#<n>` in a region of the same name (vocabulary §2);
its `return` is the generation's outcome (`ok`, `err` with the step's
status, `cancelled`, or `panicked`, which both engines catch inside the
generation), and a program that ends without one returns `ok`.
`return cancelled` is the factory future's `Outcome::Cancelled(reason)`
(`ManagedRestartMode::eligible`, `supervision.rs:1833-1844`, does not
replace a transient child for it) and is defined only in a supervised
child's own program; a group member's, or any other task's, is a scenario
error. Its reason is `cx.cancel_reason()` when a cancel reached the
generation, else `CancelReason::with_origin(User, <generation region>,
now).with_task(<generation task>)` (§4, "Reason attribution"). In C the body
ends `ASX_E_CANCELLED` (`include/asx/actor/supervisor.h`), so a supervised
child's `return err` with status `ASX_E_CANCELLED` is a scenario error too:
the two outcomes would be the same status.

Every generation reruns the same program, so the names it creates would
repeat. A generation's program may use `spawn`, `open_region`,
`server_spawn` and `supervise` (nested supervision trees included); each
name it creates with `as` is scoped under the generation's name,
`<supervisor>/<child>#<n>/<as>`, and the references to such a name in
the same program (`task`, `region`, `server`, `supervisor`) are scoped
alike, wherever they sit: in group members, `masked` steps and spawned
children's programs. A name the program does not create (a scenario-level
region, task or resource) is left alone. A nested supervisor's children
are scoped by their own generations, whose names start with the nested
supervisor's scoped name. Both interpreters rewrite the program the same
way when the generation starts (twin_run's `scope_program`, the
interpreter's `scope_steps`). Group members are named after the
generation, so groups are allowed.

A child with a `registration` takes that name in its supervisor's own
registry (twin_run binds it with `bind_managed_with_registry`, a registry of
its own per `supervise` step; C's `asx_supervisor_config.registry`). Each of
its generations does so inside its own task, before its factory runs
(`supervision/managed_names.rs`, `GenerationName::acquire`): `fail` refuses
a name that is taken, and the supervisor stops with a `Registration` error
naming the child while its generation ends cancelled with the User reason
"managed child registration failed" and never starts; `replace` takes the
name and cancels the displaced generation's task with the User reason
"managed supervisor name replaced". A generation gives its name up once it
has been drained and its region closed, before a successor starts. Naming a
generation costs the controller one lab step: it registers a finalizer on
the generation's region through the region-command lane and waits for the
acknowledgement (`ChildRegion::retain_until_finalized`,
`child_region.rs:342`; `RegionCommand::RegisterFinalizer`,
`lab/runtime.rs:4173`), which the numbering of later dispatches shows.
Rust promotes that finalizer, a sync one, to a masked cleanup task like any
other (`drain_ready_async_finalizers` wraps it in `async move {
finalizer() }`, `state.rs:8612`): when the generation's region finalizes the
lab spawns it at a later step, dispatches it once and completes it, and the
region closes after it. That task is named `<generation>/reg` (a generation's
region, or `<supervisor>/unstarted<k>`'s when it never started); it has
`task.spawned` and `task.completed` events and a dispatch, and like the
generation's own task it is left out of the snapshot. A registered child's
program may therefore not create the name `reg`. C registers the finalizer
as an async one (`lab_apply_retain`) so that it is a task too. The
registry's third policy, `wait`, has no DSL form: a waiting generation
sleeps 1 ms between looks for its grant with Sleeps made inside the
library, whose Timer events twin_run cannot attribute to a task, so a
scenario using it could not be captured. It is ported and unit-tested in C
only (`test_supervisor`).

A child's `depends_on` names siblings of the same `supervise` (an unknown
name, or a cycle, is refused at compile time in both engines). The
controller starts children in compiled order, dependencies before
dependents, whatever the declaration order (`supervision-dependency-orders-
the-start-001`). When a failed child is to be restarted and one of its
dependencies has no running generation (a temporary dependency that ended,
say), `dependency_unavailable` (`supervision.rs:2904`) finds it: a required
child (the default) stops the supervisor with a `DependencyUnavailable`
error naming that child, traced `restart_dependency_unavailable`, with no
restart batch counted (`supervision-dependency-unavailable-stops-required-
child-001`); with `required: false` the restart is skipped, still traced,
and the child stays stopped while the supervisor goes on (`supervision-
optional-child-stays-stopped-without-dependency-001`). `GenerationExhausted`
has no scenario: a generation number is a u64 the controller counts from 1,
so the error needs 2^64 restarts and neither engine's API seeds the count.

The `supervisor_join` value is
`{"outcome", "started", "joined", "restart_batches", "escalations", "children"}`:
`outcome` is `{"tag": "ok"}`, `{"tag": "err", "error": <kind>, "child": <name> | null}`
with the `ManagedSupervisorError` kind (`RestartLimit`,
`DependencyUnavailable`, `ChildNotStarted`, `GenerationExhausted`,
`Region`, `Spawn`, `Registration` or `Escalation`), or a cancelled or panicked outcome
(vocabulary §4); `children` holds each child's latest finished
generation, in start order, as `{"child", "generation", "outcome"}`. When a
storm threshold was set and a counted restart batch crossed it, the value
also holds `"restart_storm_detected": true` (the field is absent
otherwise), and the controller traced `action=restart_storm` once, for the
failed child's latest generation (`supervision.rs:3294-3305`). The check is
the restarts in the restart window per second of window above the
threshold (`RestartIntensityWindow::is_storm`), read after each counted
batch is recorded; a `RESET_COUNTER` reset forgets them. Rust's e-process
alert (`RestartStormMonitor`) is not modelled: it can alert only at a batch
whose intensity check has already fired, so no report differs. A
supervisor its owner did not join is aborted when the owner's program
ends, as dropping a `ManagedSupervisorHandle` aborts the controller
(`supervision.rs:3446`); the controller then drains every generation.

### 3.9 Interval and DelayQueue

Rust's `Interval` and `DelayQueue` (`time/interval.rs`,
`time/delay_queue.rs`) are held by the task that makes them under a name.
None of these steps suspends: they read the clock (`cx.now()` for the
interval; the queue's `wall_now()`, virtual under the lab) and leave waiting
to the program's own `sleep` / `sleep_until` steps. So no timer is
registered on their behalf and no timer event appears for them. (Rust's
queue owns a `Sleep` that a pending poll would register; C's queue shares
the task's one sleep timer slot, so a pending poll is outside the DSL.)

| op | Fields | Rust | C | Observation |
|---|---|---|---|---|
| `interval_new` | `as`, `period_ns` (at least 1), optional `start_ns`, optional `behavior` (`burst` default, `delay`, `skip`) | `interval(cx.now(), period)` or `interval_at(Time::from_nanos(start_ns), period)` (`interval.rs:443`, `:470`), then `set_missed_tick_behavior` (`:228`) | `asx_interval_init_at` (start = now without `start_ns`) + `asx_interval_set_missed_tick_behavior` | `ASX_OK`, no value |
| `interval_tick` | `interval` | `iv.poll_tick(cx.now())` (`:261`): `Some(t)` is due, `None` leaves the deadline unmoved (what `tick` returns, `:293`) | `asx_interval_tick_at` | `ASX_OK`; `value` `{"time_ns": <the deadline before the call>, "due": <bool>}`. A due tick moves the deadline by the behavior: Burst one period on (a late interval is ticked once per missed tick), Delay a period after `now`, Skip to the first period boundary after `now`. |
| `delay_queue_new` | `as` | `DelayQueue::new()` | `asx_delay_queue_init` | `ASX_OK` |
| `delay_queue_insert` | `queue`, `as` (the key's name), `value`, `timeout_ns` | `q.insert(value, Duration::from_nanos(timeout_ns))` (`:139`), saturating at the last representable time | `asx_delay_queue_insert` | `ASX_OK`. C holds `ASX_DELAY_QUEUE_CAPACITY` (16) entries; a scenario stays under it (a harness error otherwise). |
| `delay_queue_insert_at` | `queue`, `as`, `value`, `deadline_ns` | `q.insert_at(value, Time::from_nanos(deadline_ns))` (`:144`) | `asx_delay_queue_insert_at` | `ASX_OK` |
| `delay_queue_reset` / `delay_queue_reset_at` | `queue`, `key`, `timeout_ns` / `deadline_ns` | `q.reset(&key, d)` / `q.reset_at(&key, t)` (`:190`, `:202`): the key stays valid and the entry orders as a new insertion | `asx_delay_queue_reset` / `asx_delay_queue_reset_at` | `ASX_OK`. A dead key panics in Rust: a harness error. |
| `delay_queue_remove` | `queue`, `key` | `q.try_remove(&key)` (`:170`) | `asx_delay_queue_remove` | `ASX_OK` with `{"value", "deadline_ns"}`, or `ASX_E_NOT_FOUND` for a dead key |
| `delay_queue_contains` | `queue`, `key` | `q.contains(&key)` (`:231`) | `asx_delay_queue_contains` | `ASX_OK`, boolean `value` |
| `delay_queue_deadline` | `queue`, `key` | `q.deadline(&key)` (`:221`) | `asx_delay_queue_deadline` | `ASX_OK`, u64 `value`; a dead key panics in Rust: a harness error |
| `delay_queue_len` | `queue` | `q.len()` (`:237`) | `asx_delay_queue_len` | `ASX_OK`, u64 `value` |
| `delay_queue_clear` | `queue` | `q.clear()` (`:248`) | `asx_delay_queue_clear` | `ASX_OK`; every key is dead afterwards |
| `delay_queue_poll` | `queue` | `poll_expired(cx)` once (`:260`) | `asx_delay_queue_poll_at` | the next due entry earliest first, ties in insertion order: `ASX_OK` with `{"value", "deadline_ns"}`; an empty queue: `ASX_E_CHANNEL_EMPTY` (Rust `Ready(None)`). Nothing due yet is a harness error: sleep until an entry is due first. |

A key's name stays bound after its entry expired or was removed (the key is
dead, not unknown), and a name is bound once per task. Scenarios:
`interval-missed-tick-burst-001`, `interval-missed-tick-delay-001`,
`interval-missed-tick-skip-001`, `interval-start-at-001`,
`delay-queue-order-reset-remove-001`, `delay-queue-reset-ties-saturate-001`.
A standalone `time::timeout` is not a step yet: its inner future and its own
`Sleep` register two timers that twin_run names as one, and C waits for both
on the task's one sleep timer slot.

### 3.10 Monitors

A task, or a GenServer, watches another task and is told once when it
finishes (`monitor.rs`, `gen_server.rs`). A request is a command of the
region-command lane (§2, `WatchCommand::Monitor`, `monitor.rs:1198`), so a
`monitor` or `server_monitor` step takes at least one scheduler step: the
task parks, the next step's drain applies the request (eight commands per
step, with the region opens and cancels), and the task wakes with the result.
A DOWN is fired by the exit itself: monitors on the task leave the set by
ascending reference (DOWN-ORDER) and the task's own monitors end silently.
The reason is the task's runtime outcome: a declared task's or a spawned
child's `err` return is a value, so its exit is `normal` (§3.1, `return`);
`error` is a supervised child's runtime error. A handle dropped before its
DOWN was delivered queues a Demonitor command (`Monitor::drop`,
`monitor.rs:1133`), so a watcher that exits while still waiting leaves one
for the lane: a no-op when applied, but the step that drains it is a lab step
of its own, and the dispatch numbers after it shift by one.

| op | Fields | Rust | C | Observation |
|---|---|---|---|---|
| `monitor` | `task` (a task name, as `join` takes it), `as` (the monitor's name) | `cx.monitor(target).await` (`monitor.rs:1090`): `target` is the task's handle when the harness holds one, else its id | `asx_monitor_open_poll` until done | `ASX_OK` with the monitor's reference (`MonitorRef::id`, from 1); `ASX_E_NOT_FOUND` when the task is not live when the request is applied (finished, or its spawn was refused). A name is used once per task. |
| `await_down` | `monitor` | `monitor.down(&cx).await` (`:1170`) | `asx_monitor_down_poll` until done | until the task finishes. `ASX_OK` with `{"monitored": <task name>, "reason": "normal" \| "error" \| "cancelled" \| "panicked", "ref": <reference>}` (the error text and the cancel reason are not projected); `ASX_E_CANCELLED` when this task is cancelled first, checked after the stored notice. The same DOWN again on a later call. |
| `demonitor` | `monitor` | `drop(monitor)` (`Monitor::drop`, `:1203`): a Demonitor command unless the DOWN was delivered | `asx_monitor_release` | no. `ASX_OK`. The monitor is gone from the task's names. |
| `server_monitor` | `server`, `task`, `as` | `handle.monitor(&cx, target).await` (`gen_server.rs:1946`): the server's task holds the monitor | `asx_monitor_open_server_poll` until done | as `monitor`: the reference, or `ASX_E_NOT_FOUND`. The DOWN reaches the server's `handle_info` through its system lane (§3.8). |
| `server_demonitor` | `server`, `monitor` (a `server_monitor`'s name) | `handle.demonitor(&cx, ref)` (`:1978`): a Demonitor command | `asx_demonitor` | no. `ASX_OK`. |

The built-in servers (`counter`, `echo` and the budget probes) answer every
system message with one user trace, so a server's DOWNs are in the trace and
compare as events: `down:ref=<reference> reason=<kind> vt=<ns>` (`vt` is the
virtual time of the exit, `SystemMsg::Down`'s `completion_vt`;
`exit:reason=<kind> vt=<ns>` and `timeout` for the other two kinds). No new
event kind is involved. A
server's handler runs only if the server runs: a cancelled server drops its
system lane, as it does its mailbox (§3.8).

### 3.11 Links

A task, or a GenServer, links itself with another task (`link.rs`,
`gen_server.rs`): when either finishes abnormally (a runtime error, a
cancellation or a panic) the runtime requests cancellation of the other, or,
on a trapping link, tells the holder and lets it run on. A declared task's or
a spawned child's `err` return is a value and a normal exit (§3.1, `return`),
so an abnormal exit here is a panic, a cancellation or a supervised child's
error. A request is a command
of the region-command lane like a monitor's (§3.10, `WatchCommand::Link`,
`monitor.rs:1338`), so a `link` or `server_link` step takes at least one
scheduler step. The side that makes the link, the holder, propagates
(`link`) or traps (`link_trapping`); the peer's side always propagates
(`monitor.rs:1368`). An exit (`TaskWatches::on_task_completed`,
`monitor.rs:1386`) fires the task's monitors first (§3.10). Then a normal
exit signals only the trapping holders, and an abnormal one signals each
trapping holder and cancels each propagating side with a `LinkedExit` reason
(the message "link exit", with the exiting task's own cancel reason as its
cause), in the order of the receiver's task identity and then the link
reference (EXIT-ORDER); signals are delivered before cancels. Then every
link of the task ends, and the handles it held go with it.

| op | Fields | Rust | C | Observation |
|---|---|---|---|---|
| `link`, `link_trapping` | `task` (a task name), `as` (the link's name) | `cx.link(peer).await`, `cx.link_trapping(peer).await` (`link.rs:915`, `:923`): `peer` is the task's handle when the harness holds one, else its id | `asx_link_open_poll` with `ASX_EXIT_PROPAGATE` / `ASX_EXIT_TRAP` until done | `ASX_OK` with no value: Rust's `LinkRef` is a process-wide counter, so its number is not comparable (a monitor's reference is, §3.10); `ASX_E_NOT_FOUND` when the peer is not live when the request is applied. A name is used once per task. |
| `await_exit` | `link` (a `link_trapping`'s name) | `link.exit(&cx).await` (`link.rs:882`) | `asx_link_exit_poll` until done | until the peer finishes. `ASX_OK` with `{"from": <task name>, "reason": "normal" \| "error" \| "cancelled" \| "panicked"}`; `ASX_E_CANCELLED` when this task is cancelled first, checked after the stored signal. The same signal again on a later call. Only a trapping link can be awaited: on a propagating one Rust's wait ends in the holder's own cancellation, so it is a harness error on both sides. |
| `unlink` | `link` | `link.unlink()` (`link.rs:891`): an Unlink command; the handle is consumed | `asx_unlink`, then `asx_link_release` | no. `ASX_OK`. The link is gone from the task's names; an exit after the command is applied acts on nothing. |
| `server_link`, `server_link_trapping` | `server`, `task`, `as` | `handle.link(&cx, peer).await`, `handle.link_trapping(&cx, peer).await` (`gen_server.rs:1991`, `:2003`): the server's task holds the link | `asx_link_open_server_poll` until done | as `link`. A trapped exit reaches the server's `handle_info` through its system lane and is traced as `exit:reason=<kind> vt=<ns>` (§3.10); a propagating link cancels the server's task like any other. |
| `server_unlink` | `server`, `link` (a `server_link`'s name) | `handle.unlink(&cx, ref)` (`gen_server.rs:2012`): an Unlink command | `asx_unlink` | no. `ASX_OK`. |

`unlink` and `server_unlink` never suspend, so they are allowed inside
`masked`; the others wait for the lane or for an exit. Linking a task with itself is one
edge, as in Rust, and is covered by the C unit tests, not by a scenario.

## 4. Driver operations (`script`)

Each entry is `{"at_ns": t, "op": {...}}`. It is applied after the run goes
idle and virtual time is advanced to `t` (§2).

| op | Fields | Rust | C |
|---|---|---|---|
| `cancel_region` | `region`, `kind`, optional `message` | `lab.state.cancel_request(region, &reason, None)` (`state.rs:7547`); the effects are routed with `into_parts()`, `scheduler.schedule_cancel` and `dispatch()` (pattern: `tests/api_v2_integration.rs:347-358`) | `asx_region_cancel(region, &reason, NULL)` |
| `cancel_task` | `task` (a task's canonical name, vocabulary §2, so also a supervised child's generation `<supervisor>/<child>#<n>` once it started), `kind`, optional `message` | `lab.state.cancel_task(task, &reason)` (`state.rs:3429`), effects routed as above | `asx_task_cancel_with_origin(task, kind, task_region, ASX_INVALID_ID)` plus message |
| `close_region` | `region`, `kind` | `cancel_request(region, kind)`, then `advance_region_state(region)` (`state.rs:10057`) after each idle until Closed | `asx_region_cancel(region, &reason, NULL)`, then `asx_scheduler_run_until_idle` until Closed |
| `finish_region` | `region`, `body` (`ok`, `err`, `cancelled`, `panicked`); Cancelled requires `kind`, optional `message` | Scope's body-return branch (`scope.rs:877-920`): Ok only `begin_close(None)`; Cancelled `cancel_request` with the supplied reason; Err/Panicked `CancelReason::fail_fast().with_region(region)`; then `advance_region_state` once | `asx_region_close_with_outcome`, then the runtime's ordinary close advancement; subsequent lab steps finish existing work without a new cancellation |
| `advance` | `ns` | `lab.advance_time(ns)` (`lab/runtime.rs:3076`; a forward jump, the only clock fault Rust offers) | `asx_lab_advance_time(ns)` |
| `region_limits` | `region`, optional `max_tasks`, `max_children`, `max_obligations` | `lab.state.set_region_limits(region, RegionLimits{…})` (`state.rs:4103`; `record/region.rs:208`); a region already gone has nothing to limit: no effect | `asx_region_set_limits(region, &limits)`; an omitted or null field is `ASX_REGION_UNLIMITED`; a region already gone: no effect |
| `region_close_outcome` | `region` | `lab.state.region_close_outcome(region)` (`state.rs:3972`; folded as each task completes, `state.rs:8794`): the outcomes of the tasks that completed in the region, joined, read live or after its close | `asx_region_get_close_outcome(region, &outcome)` |

`finish_region` is a primitive body-branch witness executed against the real
Rust runtime. It does not run `Scope::region_with_child_admission`'s typed
factory or return its body payload. Its body tag is separate from the region
snapshot's child-derived close outcome. An Ok parent waits for still-Open
child regions to finish their own bodies. Err/Panicked use the native
Cx-free FailFast constructor, including its testing-default timestamp;
Cancelled uses the driver's supplied reason and virtual-time attribution.

`region_close_outcome` is the one driver operation that observes: it is
recorded under the task name `driver`, with the script entry's 1-based index
as its step and `region_close_outcome` as its op. The status is `ASX_OK` and
the value the outcome (vocabulary §4: `ok`, `cancelled` with its reason, or
`panicked` with its message), or `ASX_E_NOT_FOUND` and no value while no
task of the region has completed and the region is not Closed. The outcome
is the join of the tasks' outcomes, folded as each completes (so readable
before the close), with the severity order Ok < Err < Cancelled < Panicked
deciding; with nothing folded, a closed region reports Cancelled with its own
cancel reason if it was cancelled and Ok otherwise (`record/region.rs:1820-1835`).
An `err` outcome has no status on the C side, so the interpreter refuses a
scenario that reads one. The name `driver` is reserved.

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
| supervised child `return cancelled`, with no cancel pending | the generation's region | the generation's task |

`CancelReason::new(kind)` must not be used. It is a testing default that
stamps `RegionId::testing_default()` and a fixed 1 s timestamp
(`types/cancel.rs:590-605`). Captures made with it recorded placeholder
attribution, and the placeholder region happened to be named `"root"`.
C builds the same reason and passes it whole: `asx_region_cancel(region,
&reason, …)` and `asx_task_cancel_with_reason(task, &reason)`.

Three reasons Rust itself creates do carry that testing default, and the C
side reproduces them (bd-wxep, an upstream issue):

- **An Err/Panicked scope body.** Scope constructs
  `CancelReason::fail_fast().with_region(child_region)` (`scope.rs:910-914`).
  The origin is the child region, with no task and the constructor's 1 s
  timestamp. `finish_region` reproduces this actual native branch rather
  than substituting the driver's current virtual time.

- **The step `close_region`.** Rust's Close command uses
  `CancelReason::user("owned child region body finished")`
  (`lab/runtime.rs:4195`). C's Close command (`asx_region_close_request`,
  `asx_region_close_poll`) uses the same reason: origin `"root"`, no
  task, 1 s.
- **A pre-poll poll-quota cancel.** It is raised when a task is
  dispatched with its quota already spent, using `CancelReason::poll_quota()`
  (`lab/runtime.rs:4667`). C stamps the same reason under lab dispatch
  (`asx_scheduler_use_lab_dispatch`); the default sweep scheduler
  attributes it to the task. A checkpoint that later observes the spent
  quota re-attributes it in both engines (earlier timestamp wins).

There is no allocation-failure fault and no backward or per-task clock skew:
Rust has neither (`lab/runtime.rs:3357`). `region_limits` is the scripted
stand-in for resource exhaustion. A spawn, child region or reservation past
a limit is observed as `ASX_E_ADMISSION_LIMIT`: Rust resolves a denied
spawn's handle `Cancelled(User)` with the `RegionAtCapacity` error
(`[ASUP-E006]`) as its message, which twin_run projects onto the spawn step
and the child's joins, as it projects a closed-region refusal as
`ASX_E_REGION_CLOSED`. Both harnesses classify a refusal by the reason
its handle resolves with, so a child aborted before its admission refused
it resolves with the abort's reason and is observed as
`ASX_E_REGION_CLOSED`, not `ASX_E_ADMISSION_LIMIT`.

## 5. Must-fail scenarios

`{"kind":"must_fail","error":{"task":"t.x","step":3,"status":"ASX_E_..."}}`
asserts that step 3 of `t.x` returns that status. Both engines must produce
that observation; the rest of the run is compared as usual. A must-fail
scenario whose step succeeds is a divergence in the failing engine.

The status names come from `CANONICAL_VOCABULARY_V2.md` §5. Typical uses:

- commit after abort of the same obligation: `ASX_E_NOT_FOUND` (§3: the
  abort consumed the token, so the runtime's
  `ASX_E_OBLIGATION_ALREADY_RESOLVED` is not reachable from the DSL);
- reserve in a closed region: `ASX_E_REGION_CLOSED`;
- send on a closed channel: `ASX_E_DISCONNECTED`;
- reserve over a region limit: `ASX_E_ADMISSION_LIMIT`.

### Futurelock scenarios

`{"kind":"futurelock"}` with `lab.futurelock_max_idle_steps` asserts that
the run reports a futurelock: a task that holds a pending obligation and has
not been polled for more than that many steps (Rust's lab,
`src/lab/runtime.rs:5612`). The reports are the run's `futurelock.detected`
events (`CANONICAL_VOCABULARY_V2.md` §3), one per violating task at every
step it stays violating, and they are compared like any other event. A
scenario that declares the knob and expects `ok` asserts the opposite: no
report at all. Declaring `futurelock` without the knob is invalid, and both
harnesses refuse it.

Because the same report repeats at every following step, a scenario should
end soon after the idle task is polled again, as the deliberate scenarios
do: one task holds an obligation and parks, a second keeps the lab stepping
and then wakes the first, which commits and completes. A holder that parks
for good ends the run only at the 1000-step stuck bound, with a report at
each of those steps.

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

Open: none.

Closed (each verified by a fixture that now matches):

- **Managed supervision** (bd-g652): C's supervisor was a state machine
  that restarted GenServers, which no oracle could compare. It is now
  Rust's managed supervisor (`supervision.rs:1795-3506`,
  `src/actor/supervisor.c`): a controller task with a region of its own
  that runs each child generation as a task in a child region, starts
  them in compiled (dependency) order, takes simultaneous ends by (end
  time, task), restarts by strategy and restart mode within the restart
  intensity, drains the restart set in reverse order with the "managed
  supervisor generation drain" reason, sleeps the backoff, stops, resets
  or escalates (a FailFast cancel of its own region) when the intensity
  refuses, and traces `managed_supervisor_v1` as Rust does. New runtime
  pieces: `asx_task_catch_panic_internal` (the controller catches a
  generation's panic, as Rust's factory future does) and
  `asx_region_get_parent`. Fixtures `supervision-*` (16); the generator
  starts supervisors (60 seeds x 200: no divergence, 4781 scenarios with
  one). Its scenarios exposed four lab dispatch differences, each fixed
  in C and pinned by a fixture that fails without the fix:
  an abort of a child not yet admitted is cached and queued at admission
  behind the handle cancels already queued
  (`lab-dispatch-cached-abort-at-admission-001`); a region command's
  cancel-lane publications and wakes wait for the end of the command
  drain, lanes first (`lab-dispatch-region-command-deferred-wakes-001`);
  a region command advances the region it cancels
  (`close_region_command` is `cancel_request`, `advance_region_state`,
  then `defer_cancel_dispatch`, `runtime/state.rs:4936-4940`;
  `lab-dispatch-region-command-advances-region-001`);
  and a refused child aborted before its admission is observed as
  `ASX_E_REGION_CLOSED` (`region-limits-refused-spawn-aborted-001`).

- **When a permit's obligation counts against `max_obligations`**
  (bd-2fga). Rust routes a Cx's obligation operations through its
  obligation mailbox and applies them, in post order, when the poll
  returns (`lab/runtime.rs:4797-4808`): a permit's registration
  (`Cx::try_register_obligation`, `cx/cx.rs:1808`) is admitted against
  the region's limit only then, and its commit or abort releases the count
  then; a checked registration (`try_register_obligation_checked`,
  `:1746`, the DSL `reserve`) is admitted at the call, and its commit,
  abort or drop releases the count at the call. C now does the same under
  lab dispatch: obligation operations made inside a poll are posts drained
  after the poll (`asx_obligation_drain_posts_internal`), permits register
  through `asx_obligation_register`, a refused registration leaves the
  permit untracked, and the interpreter drops a body's leftover tokens
  first, as twin_run's locals do. Fixtures
  `obligation-limit-permit-admitted-at-poll-end-001`,
  `obligation-limit-permit-release-at-poll-end-001` (fuzz gen-9-120),
  `obligation-limit-token-drop-releases-count-001` (fuzz gen-10-146); the
  generator sets `max_obligations` with permits again (30 seeds x 200: no
  divergence, 452 scenarios with `max_obligations`).

- **GenServers** (bd-g652): `src/actor/actor.c` runs Rust's
  `run_gen_server_loop` as a task on an mpsc mailbox (the C actor was a
  ring buffer served one message per poll, with a synchronous call
  token). The init, the loop's cancel check, the receive (a stop with an
  empty mailbox ends it as a disconnect), the batch yield after eight
  messages, the sealed-mailbox drain (casts skipped once cancelled, calls'
  replies aborted), terminate, and the client side (cast's transient send
  without an obligation, call's mailbox and reply permits, each a
  SendPermit held by the caller) follow `gen_server.rs` step for step with
  its user traces. New primitives: `asx_channel_seal` (Rust
  `Receiver::close`, keeping the queue), `asx_channel_wake_receiver`, and
  `asx_oneshot_reserve` / `asx_oneshot_permit_send` / `_abort` (the
  tracked reply permit). A spawn the lab refuses at once (a closing
  region) keeps its handle, as Rust's queued spawn does, and the refusal
  drops its cell. Fixtures `actor-*` (11), among them
  `actor-cancel-before-start-drains-001` and
  `actor-cancel-caller-and-server-001`. The generator emits server steps
  (bd-86np): a task spawns up to two servers and casts to, calls and
  stops them; twin_run names a server by the id it records from its own
  hooks, as a spawned child records its own.

- **When `max_tasks` is checked under lab dispatch** (bd-orxy): as Rust's
  spawn mailbox does, a spawn from a poll returns its task at once and the
  next step's admission checks it, FIFO, against the region's live tasks
  then: the spawner, completed in the same poll, no longer counts; spawns
  admitted before it in that step do (`asx_lab_admit_pending`). A refused
  child never runs, leaves no trace (its `task.spawned` is recorded at
  admission, as Rust's Spawn event is, state.rs:1119) and completes
  Cancelled(User) with the default attribution and the message
  `[ASUP-E006] region admission limit reached` (Rust's text, which spells
  the RegionId, limit and live count, is projected to that prefix);
  `asx_task_admission_status` reports why. The spawn step and joins
  observe `ASX_E_ADMISSION_LIMIT`, as twin_run's `project_denied_spawns`
  projects Rust's; a refused task-group member is a cancelled member.
  twin_run names admitted children by the canonical id each records when
  it first runs (an enqueue whose spawn was refused has no TaskAdmitted
  to pair with), and keeps a refused child's result when an abort names
  it, before the abort strengthens its reason. Fixtures
  `region-limits-late-admission-001`,
  `region-limits-group-member-refused-001`; the generator sets `max_tasks`
  again (30 seeds x 200: no divergence, 304 scenarios with `max_tasks`).

- **Lock poisoning** (bd-9kll.6.5): a Rust mutex guard, or rwlock write
  guard, dropped while its task panics poisons the lock (`sync/mutex.rs:758-763`,
  `sync/rwlock.rs:1083-1090`); later acquires, and queued waiters at
  their next poll, fail with `Poisoned` (`ASX_E_INVALID_STATE`). A
  poisoned rwlock release wakes every queued waiter, readers then writers,
  granting nothing; a poisoned mutex unlock hands the lock on as usual and
  each waiter passes it on as it fails. The interpreter releases a
  panicking task's guards with `asx_mutex_unlock_poisoned` /
  `asx_rwlock_write_unlock_poisoned`. Fixtures
  `sync-mutex-poisoned-by-panic-001`,
  `sync-mutex-poisoned-hand-off-chain-001`,
  `sync-rwlock-poisoned-by-panic-001` (a read guard does not poison),
  `sync-rwlock-poisoned-wakes-queued-001`. Generated scenarios still never
  panic while holding a lock.

- **Mutex lock_until** (bd-9kll.6.5): the deadline is a traced Sleep, as
  Rust's `LockFuture` (`sync/mutex.rs:445-452`, `:517-523`): polled after
  the checkpoint, it registers its timer on the first poll before the
  deadline (`timer.scheduled`), fires when the wait times out, and is
  cancelled when the wait ends otherwise (taken, cancelled, poisoned). A
  deadline already due registers nothing. Fixtures
  `sync-mutex-lock-until-001` (timeout while queued, a grant before the
  deadline, a free lock past its deadline, a free lock before it) and
  `sync-mutex-lock-until-cancel-001` (cancelled while queued); generated
  scenarios use `mutex_lock_until` too.

- **Race ties within one round** (bd-g652): Rust picks the winner among
  the members ready at the owner's poll with `cx.random_usize` over the
  owner's entropy (`Scope::race_all`, `cx/scope.rs:1340-1365`). Under lab
  dispatch C keeps Rust's per-task DetEntropy: the runtime source seeded
  with the lab seed, forked for each host-created task (`state.rs:4356`)
  and each child region's principal (`:5093`), a spawned task forked from
  its spawner (or the region's principal) at its first poll
  (`DeferredFork`, `cx.rs:239-286`), each fork keyed by the task's id in a
  shadow of Rust's task arena (LIFO reuse of completed tasks' indices,
  `util/arena.rs`, `state.rs:8497`) and the source's fork counter
  (`src/runtime/lab_dispatch.c`). Fixtures
  `task-groups-race-same-round-tie-001` and
  `task-groups-race-deadline-tie-001` (the tie drawn in Scope::timeout's
  wrapper); generated scenarios include race groups again.

- **RwLock** (`rwlock_read` / `rwlock_write` / `rwlock_unlock`): C's
  rwlock follows Rust's grant policy: an acquire joins the line at its
  first poll that has to wait and is only granted after that; a release
  serves the head writer unless readers queued before it, who go first;
  16 writer hand-offs in a row with a reader queued force one reader turn;
  grants go to the front whatever its task's cancel state; giving up
  follows `abandon_read_waiter` / `abandon_write_waiter`; `try_write`
  fails while a writer waits (`sync-rwlock-arrival-order-001`,
  `sync-rwlock-forced-reader-turn-001`,
  `sync-rwlock-cancelled-front-writer-001`; generated scenarios declare an
  rwlock at times, and rwlock crowds). Downgrade: `rwlock_downgrade`
  (`asx_rwlock_downgrade`; `sync-rwlock-downgrade-001`; generated
  scenarios downgrade at times).

- **Region admission limits** for `region_limits`: `asx_region_set_limits`
  caps a region's live tasks, live child regions and pending obligations;
  an admission past a cap is refused with `ASX_E_ADMISSION_LIMIT` and
  changes nothing; a Finalizing region admits no task at all, as Rust
  admits only its async finalizers' cleanup tasks there
  (`region-limits-admission-001`, `region-limits-late-admission-001`;
  about one generated scenario in ten sets task, child and obligation
  limits; a spawn's `max_tasks` is checked at its admission, see Closed).

- **Multi-permit semaphore acquire**: `asx_semaphore_acquire_many_begin` /
  `asx_semaphore_try_acquire_many` take `count` permits all or nothing, at
  the front of the line, and a permit releases its whole count; a count of
  0 succeeds at once with an empty permit (`sync-semaphore-acquire-many-001`;
  generated scenarios ask for 2 permits at times).

- **Non-parking `try_send` / `try_recv`** (bd-vc1v): C's mpsc, oneshot and
  broadcast `try_*` functions no longer park; the waiting
  `asx_channel_reserve` / `asx_channel_send` / `asx_channel_recv`,
  `asx_oneshot_recv` and `asx_broadcast_recv` do. Verified by generated
  scenarios using both steps (`make fuzz-differential`).

- **Oneshot, broadcast and watch**: `asx_oneshot_send` / `asx_oneshot_recv`
  (SendPermit obligation; the exact oneshot traces; value and close before
  cancel), `asx_broadcast_send` with a Cx (cancel first, Closed with no
  receiver, SendPermit) and `asx_broadcast_recv` (cancel first and traced,
  lag count), `asx_watch_changed_begin` / `asx_watch_poll_changed` with a
  Cx (the watch traces) (`oneshot-send-recv-001`, `broadcast-fanout-001`,
  `watch-changed-001`).
- **Watch parity** (bd-9kll.5.11): `send_modify`, `wait_for` (predicate
  first, `changed()` on a counted clone), `borrow` vs `borrow_and_update`,
  `mark_seen`, `has_changed`, receiver clone, `is_closed`, `receiver_count`
  (`watch-send-modify-001`, `watch-wait-for-001`, `watch-wait-for-cancel-001`,
  `watch-wait-for-sender-dropped-001`, `watch-wait-for-clone-counts-001`,
  `watch-borrow-vs-borrow-and-update-001`, `watch-receiver-clone-continues-001`,
  `watch-is-closed-receiver-count-001`, `watch-receiver-is-closed-001`).
- **Oneshot closed notifications**: `oneshot_poll_closed`,
  `oneshot_is_closed`, `oneshot_is_ready` (bd-9kll.5.8): each end's wait is
  woken only by what Rust's separate closed waker is woken by
  (`oneshot-closed-001`; a cancelled send closes the channel,
  `oneshot-closed-cancelled-send-001`; a send wakes no closed waiter,
  `oneshot-closed-send-no-wake-001`). Generated scenarios use the three
  steps before an end is used.
- **Mpsc evict-oldest, batch receive and closed wait**: `send_evict_oldest`,
  `recv_many`, `closed` (bd-9kll.5.7): `asx_channel_send_evict_oldest`,
  `asx_channel_recv_many` (one parked producer woken per freed slot) and
  `asx_channel_closed` (woken by `asx_channel_seal` and
  `asx_channel_close_receiver`). Generated scenarios use all three.
- **`seal`** (bd-9kll.5.4): Rust's `rx.close()` as `asx_channel_seal`
  (`mpsc-seal-drains-001`; generated consumers seal at times and keep
  receiving). The receiver's drop after a seal now discards the queue, as
  Rust's does; C had kept it, so a sealed channel dropped with messages
  queued never freed its slot.
- **A permit whose obligation `max_obligations` refused is untracked**:
  the post drain releases the refused reservation's slot, and the permit's
  token check (`asx_obligation_check_token_internal`) treats an id with
  no obligation behind it as untracked, so semaphore releases, oneshot
  sends and channel and broadcast permits settle physically as Rust's do.
  819502b's check had refused them, which held semaphore permits and lost
  GenServer replies under a quota (`obligation-quota-refused-semaphore-permit-releases-001`,
  `obligation-quota-refused-call-reply-delivered-001`; fuzz seeds
  1100-1238).
- **Owner-cancelled race and quorum take the owner Cx's reason**
  (bd-lfwh, bd-uyd3): Rust's `cx.cancel_reason()`, which can be ahead of
  the task record's after a tie (`race-owner-cancel-cx-reason-001`,
  `race-deadline-wrapper-cx-reason-001`, from fuzz cases gen-622-100 and
  gen-667-113).
- **Broadcast senders, receiver counts and wake order** (bd-9kll.5.6):
  broadcast `senders` clones; `broadcast_send` observes the receivers it
  reached; parked receivers are woken in the order of Rust's waker Arena
  slots, not arrival order (`broadcast-senders-receiver-count-001`,
  `broadcast-wake-order-slot-reuse-001`, from fuzz case gen-622-10).

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
  `cancel-strengthen-severity-001`). Masks nest as a depth count, so the
  cancel is observed only once the outermost one ends
  (`cancel-masked-nested-001`; the generator nests masks at times).
- **Closed-region admission** reports `ASX_E_REGION_CLOSED`
  (`obligation-reserve-closed-region-must-fail-001`).

- **Cancel message and origin**: `asx_task_cancel_with_reason` and
  `asx_region_cancel` take a whole reason; budget cancels are attributed to
  the task and stamped (`budget-poll-quota-exhaustion-001`).
- **A budget cancel on a cancelled task reaches its Cx only** (fuzz
  findings gen-113-192, gen-125-92): the lab's poll-quota check before a
  poll, or a masked checkpoint, strengthens the Cx's reason; the record
  keeps the reason and cleanup budget of the request it holds until a
  checkpoint acknowledges, which reconciles them
  (`reconcile_checkpoint_cancel`, `record/task.rs:1096-1153`), a later
  request is strengthened by the Cx's reason (`:716-722`), or the task
  completes: its outcome takes the Cx's reason sampled at the terminal
  poll (`classify_spawn_completion`, `runtime/task_handle.rs:173-200`). The
  snapshot shows the record (`budget-quota-after-deadline-cancel-001`,
  `budget-quota-cx-reason-reconciled-001`,
  `budget-quota-cx-reason-at-completion-001`). The Cx takes a reason only
  if it strengthens its own (`record/task.rs:839-850`), so on a tie (equal
  severity and timestamp) the record and the Cx keep different reasons:
  no cancel waker fires, and the outcome reports the Cx's
  (`cancel-cx-reason-tie-keeps-own-001`,
  `cancel-tie-outcome-keeps-cx-reason-001`).
- **A region close wakes with the waiter's first waker** (fuzz finding
  gen-103-136): Rust's RegionQuiescence keeps every distinct waker it is
  polled with and wakes them in order, so a waiter re-polled at another
  priority is scheduled at the priority of its first close poll
  (`cx/child_region.rs:560-571`; `region-close-wakes-first-waker-001`).
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
