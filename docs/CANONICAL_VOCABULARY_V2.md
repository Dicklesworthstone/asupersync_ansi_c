# Canonical semantic vocabulary v2 (`asx.vocab.v2`)

**Normative.** Bead W1.2 (bd-9kll.2.2). Both engines project into this
vocabulary before any comparison: the Rust reference (asupersync 0.6.0 at
`5e60b1c4c53d62aaddae68de3ee7de4732f1755b`, through `tools/twin_run`) and the
C runtime (through `asx_conformance_run`). A comparison is meaningful only
when it is made in these terms. Citations are `file:line` in `/dp/asupersync`
at the pinned rev, unless a path starts with `src/` of this repo and says so.

Companion files:
- `docs/VOCABULARY_EXCLUSIONS.md`: every Rust `TraceEventKind` that is not
  projected, with the reason.
- `schemas/canonical_vocabulary_v2.json`: JSON Schema for events, reasons,
  outcomes and the snapshot.

## 1. What is compared

A scenario run produces three artifacts, all in canonical JSON (§9):

| Artifact | Content | Compared by |
|---|---|---|
| `trace` | vocabulary events (§3), in Foata canonical form (§8) | comparator mode (b) |
| `snapshot` | end-of-scenario state (§6) | comparator mode (a) |
| `observations` | result of every scenario API call (§7) | modes (a) and (b) |

Scheduling order is **not** part of the trace. The lab runtime records no
`Schedule`/`Poll`/`Yield`/`Wake`/`CancelAck` events; those come only from the
production three-lane scheduler's opt-in capture
(`src/runtime/scheduler/three_lane.rs:8643-8691`). Exact dispatch order is
compared in mode (c) through the forced-schedule receipt and the schedule
certificate (`docs/TWIN_RUN_FEASIBILITY.md`, finding 2).

Channel traffic is not in the trace either: Rust has no channel event kinds,
and a successful `mpsc::Sender::send` registers no obligation and emits
nothing (`src/channel/mpsc.rs:715-727`, `:1190-1201`). Channel effects are
compared through the snapshot (`channels`, §6) and the observations.

## 2. Entity names

Runtime identifiers are never compared. Rust ids are arena indices
(`ArenaIndex { index: u32, generation: u32 }`, `src/util/arena.rs:18`) and C ids
are generation-tagged handles; neither is stable across engines. Every entity
gets a canonical name instead:

| Entity | Name |
|---|---|
| region created by a scenario step | the step's `name` (DSL v2), e.g. `"r.main"` |
| the root region | `"root"` |
| task spawned by a scenario step | the step's `name`, e.g. `"t.producer"` |
| task spawned by another task's program (`Cx::spawn`) | `"<parent>/<k>"`, k = 1-based spawn index within the parent's program |
| obligation | `"<holder>/o<k>"`, k = 1-based reservation index within the holder task |
| timer | `"<owner>/tm<k>"`, k = 1-based timer index within the owner task |
| channel / sync primitive | the step's `name` |

Each name depends only on the scenario program, so it is the same in both
engines, whatever order the scheduler runs independent work in. The harness
of each engine keeps the id → name map. An event naming an id the map
doesn't know is a harness defect, not a divergence.

## 3. Event kinds

Every event is a JSON object with `"k"` (kind) and the kind's fields. Field
values are canonical names (§2), enumeration names, or integers. Events
carry **no** `seq`, `time` or logical time. Event time differs structurally
between engines (Rust stamps obligation events when its obligation mailbox
drains, `src/runtime/obligation_mailbox.rs:936`; C stamps them at the API
call), and times that matter are compared through the snapshot (`now_ns`,
timer deadlines).

| `k` | Fields | Rust source (`TraceEventKind`, `TraceData`) | C source |
|---|---|---|---|
| `task.spawned` | `task`, `region` | `Spawn`, `Task{task,region}` (`src/trace/event.rs:169`) | `ASX_TRACE_TASK_SPAWN` |
| `task.completed` | `task`, `region` | `Complete`, `Task{task,region}` (`:179`) | `ASX_TRACE_SCHED_COMPLETE` |
| `region.created` | `region`, `parent` (name or `null`) | `RegionCreated`, `Region{region,parent}` (`:199`) | `ASX_TRACE_REGION_OPEN` |
| `region.close_begin` | `region` | `RegionCloseBegin` (`:195`) | `ASX_TRACE_REGION_CLOSE` |
| `region.closed` | `region` | `RegionCloseComplete` (`:197`) | `ASX_TRACE_REGION_CLOSED` |
| `region.cancelled` | `region`, `reason` | `RegionCancelled`, `RegionCancel{region,reason}` (`:201`) | **missing in C** (§11) |
| `cancel.requested` | `task`, `region`, `reason` | `CancelRequest`, `Cancel{task,region,reason}` (`:181`) | **missing in C** (§11) |
| `obligation.reserved` | `obligation`, `task`, `region`, `kind` | `ObligationReserve` (`:203`) | `ASX_TRACE_OBLIGATION_RESERVE` |
| `obligation.committed` | `obligation`, `task`, `region`, `kind` | `ObligationCommit` (`:205`) | `ASX_TRACE_OBLIGATION_COMMIT` |
| `obligation.aborted` | `obligation`, `task`, `region`, `kind`, `abort_reason` | `ObligationAbort` (`:207`) | `ASX_TRACE_OBLIGATION_ABORT` |
| `obligation.leaked` | `obligation`, `task`, `region`, `kind` | `ObligationLeak` (`:209`) | **missing in C** (§11) |
| `obligation.handoff` | `obligation`, `from_task`, `to_task`, `from_region`, `to_region` | `UserTrace` carrying `obligation_handoff_v1 <json>` (`:24-76`) | **missing in C** (§11) |
| `timer.scheduled` | `timer`, `deadline_ns` | `TimerScheduled`, `Timer{timer_id,deadline:Some}` (`:213`) | `ASX_TRACE_TIMER_SET` |
| `timer.fired` | `timer` | `TimerFired` (`:215`) | `ASX_TRACE_TIMER_FIRE` |
| `timer.cancelled` | `timer` | `TimerCancelled` (`:217`) | `ASX_TRACE_TIMER_CANCEL` |
| `user.trace` | `message` | `UserTrace`, `Message(String)` (`:237`), not a handoff | **missing in C** (§11) |

### Emission rules both engines must follow

These are Rust behaviours (with emission sites) that the C side must
reproduce exactly for the projected streams to agree.

- **`region.cancelled` on every call.** A cancel request emits
  `region.cancelled` for **every** region in the target's subtree, depth
  ascending, on **every** call. The target carries the request's reason;
  each descendant carries a `ParentCancelled` reason whose cause is its
  parent's reason (`src/runtime/state.rs:7741-7758`).
- **`cancel.requested` only when newly cancelled.** It is emitted per task
  only when the task becomes newly cancelled (`state.rs:7863-7865`).
  Strengthening the reason of a task or region that is already cancelled
  emits **nothing**: there is no strengthen event (`state.rs:7794`).
- **Fail-fast sibling cancellation** emits `cancel.requested` for each
  sibling (`state.rs:7449`, `:7501-7503`).
- **`region.close_begin`** is emitted when the region actually starts
  closing: on the cancel path only when the close transition happens
  (`state.rs:7782-7792`), and on the sealed-close path from the region state
  machine (`state.rs:10006-10016`). `region.closed` is emitted once, when the
  region reaches `Closed` (`state.rs:9880-9890`).
- **Obligation events** are emitted for obligations registered with the
  runtime. That means explicit `reserve`, `reserve_checked` and
  `permit.send`, plus the oneshot's internal SendPermit
  (`src/channel/oneshot.rs:528`, `:759-765`). **Not** the transient reserve
  of `mpsc::Sender::send`.
- **Oneshot `user.trace` messages:** `"oneshot::reserve creating permit"`
  (`oneshot.rs:512`), `"oneshot::recv received value"` (`:1041`),
  `"oneshot::recv channel closed"` (`:1055`) and
  `"oneshot::recv cancelled while waiting"` (`:1074`).
- **mpsc `user.trace` messages** are emitted on cancellation only:
  `"mpsc::reserve cancelled"` (`mpsc.rs:1028`, `:1063`),
  `"mpsc::recv cancelled"` (`:1787`, `:2052`) and
  `"mpsc::recv_many cancelled"` (`:1882`, `:2102`).
- **Scenario `user.trace`:** the DSL `trace` step emits `user.trace` with the
  step's message (Rust `Cx::trace`, `cx.rs:3362-3376`).
- **Timers:** re-arming a sleep emits `timer.cancelled` then
  `timer.scheduled` (`src/time/sleep.rs:985-988`). A fire is recorded with
  time `max(now, deadline)` (`sleep.rs:688`); only the event and the snapshot's
  pending set are compared.

## 4. Encodings

### Reason

```json
{"kind": "<CancelKind name>", "origin_region": "<name>", "origin_task": "<name>" | null,
 "timestamp_ns": <u64>, "message": "<text>" | null, "cause_chain_len": <u32>, "truncated": <bool>}
```

These fields project the Rust `CancelReason` (`src/types/cancel.rs:521-549`).
`cause_chain_len` counts the `cause` links. `truncated` mirrors Rust's flag,
whose chains are capped at depth 64 on deserialization.

### Outcome

```json
{"tag": "ok"}
{"tag": "err", "status": "<status name, §5>"}
{"tag": "cancelled", "reason": <reason>}
{"tag": "panicked", "message": "<text>"}
```

This is the Rust `Outcome` (`src/types/outcome.rs:218-227`). Its severity is
`ok`=0, `err`=1, `cancelled`=2, `panicked`=3 (`:164-173`), and `join` keeps
the higher severity, the left operand on ties (`:566-582`). A panic message is
the panic's string payload, or `"unknown panic"` for a non-string payload
(`src/cx/scope.rs:181-187`). A panic during poll is recorded as
`"task panicked during poll"` (`src/runtime/scheduler/worker.rs:416-420`).

### Enumerations (by name, never by ordinal)

| Field | Names |
|---|---|
| obligation `kind` | `SendPermit`, `Ack`, `Lease`, `IoOp`, `SemaphorePermit`, `Transaction` (`src/record/obligation.rs:54`) |
| `abort_reason` | `Cancel`, `Error`, `Explicit` (`obligation.rs:99`) |
| obligation state | `Reserved`, `Committed`, `Aborted`, `Leaked` (`obligation.rs:192`) |
| task state | `Created`, `Running`, `CancelRequested`, `Cancelling`, `Finalizing`, `Completed` (`src/record/task.rs:77`) |
| region state | `Open`, `Closing`, `Draining`, `Finalizing`, `Closed` (`src/record/region.rs:76`) |

The C enums (`include/asx/asx_ids.h`) use the same state names in the same
order. They are still encoded by name.

### Cancel kinds

Cancel kinds are encoded **by name**. The ordinals differ between the
engines: Rust's wire codes follow declaration order
(`src/cancel/symbol_cancel.rs:23-52`), and C's enum is ordered by severity.

| Name | Severity | Rust wire code | C enum (`asx_ids.h:150-160`) |
|---|---|---|---|
| `User` | 0 | 0 | `ASX_CANCEL_USER` = 0 |
| `Timeout` | 1 | 1 | `ASX_CANCEL_TIMEOUT` = 1 |
| `Deadline` | 1 | 2 | `ASX_CANCEL_DEADLINE` = 2 |
| `PollQuota` | 2 | 3 | `ASX_CANCEL_POLL_QUOTA` = 3 |
| `CostBudget` | 2 | 4 | `ASX_CANCEL_COST_BUDGET` = 4 |
| `FailFast` | 3 | 5 | `ASX_CANCEL_FAIL_FAST` = 5 |
| `RaceLost` | 3 | 6 | `ASX_CANCEL_RACE_LOST` = 6 |
| `ParentCancelled` | 4 | 7 | `ASX_CANCEL_PARENT` = 8 |
| `ResourceUnavailable` | 4 | 8 | `ASX_CANCEL_RESOURCE` = 9 |
| `Shutdown` | 5 | 9 | `ASX_CANCEL_SHUTDOWN` = 10 |
| `LinkedExit` | 3 | 10 | `ASX_CANCEL_LINKED_EXIT` = 7 |

Severity comes from `CancelKind::severity()` (`src/types/cancel.rs:473-482`).
`LinkedExit` is severity 3, although that function's doc comment omits it.

**Strengthen** (`cancel.rs:956-1008`) is normative for both engines:
1. A reason of higher severity replaces the current one whole (kind, origins,
   timestamp, message, cause and truncation).
2. A lower severity leaves the current reason unchanged.
3. On equal severity, the earlier `timestamp_ns` wins.
4. On equal timestamps, a reason with a message replaces one without, and of
   two messages the smaller in UTF-8 byte order (Rust `String` ordering)
   replaces the larger.

Otherwise the current reason is kept. A winning reason in steps 3–4 also
replaces the whole reason, so an equal-severity reason can change the kind
(for example `Timeout` to `Deadline`).

## 5. Status vocabulary

Statuses are encoded by C status **name** (`include/asx/asx_status.h`). The
table covers all 67 C codes. Each row is one of three things:
- **exact**: a one-to-one Rust counterpart;
- **lossy**: several Rust values map here, or this code maps to several
  Rust values, with the rule stated;
- **C-only**: no Rust counterpart is reachable through DSL v2. A C-only status
  in a scenario both engines run is a divergence.

| C status | Rust counterpart | Class |
|---|---|---|
| `ASX_OK` (0) | success (`Ok`) | exact |
| `ASX_E_PENDING` (1) | `Poll::Pending`, or `try_join` → `Ok(None)` | exact |
| `ASX_E_INVALID_ARGUMENT` (100) | `ErrorKind::InvalidInput` (`src/error.rs:46`) | exact |
| `ASX_E_INVALID_STATE` (101) | `ErrorKind::InvalidStateTransition`; every `PolledAfterCompletion` variant (oneshot/watch/broadcast `RecvError`, `JoinError`, `LockError`, `AcquireError`, `RwLockError`, `BarrierWaitError`); `LockError::Poisoned` | lossy: all misuse-after-terminal maps here |
| `ASX_E_NOT_FOUND` (102) | — | C-only: handle lookup miss (Rust handles are typed) |
| `ASX_E_ALREADY_EXISTS` (103) | — | C-only |
| `ASX_E_BUFFER_TOO_SMALL` (104) | — | C-only: caller-supplied buffers |
| `ASX_E_INVALID_TRANSITION` (200) | `ErrorKind::InvalidStateTransition` (lifecycle tables) | exact for lifecycle-table rejections |
| `ASX_E_REGION_NOT_FOUND` (300) | `SpawnError::RegionNotFound` (`src/runtime/state.rs:1626`) | exact |
| `ASX_E_REGION_CLOSED` (301) | `ErrorKind::RegionClosed`; `SpawnError::RegionClosed`; `AdmissionError::Closed` (`src/record/region.rs:269`); `ObligationAdmissionError::RegionClosed`; `ErrorKind::RegionFinalized` | lossy: every closed/finalized-region rejection |
| `ASX_E_REGION_AT_CAPACITY` (302) | — | C-only: fixed arena full (Rust allocates) |
| `ASX_E_REGION_NOT_OPEN` (303) | `ErrorKind::AdmissionDenied` on a closing region | lossy |
| `ASX_E_ADMISSION_CLOSED` (304) | `ErrorKind::AdmissionDenied` | exact |
| `ASX_E_ADMISSION_LIMIT` (305) | `AdmissionError::LimitReached{kind,limit,live}`; `ObligationAdmissionError::LimitReached` | lossy: `kind`/`limit` go to the observation's `value` |
| `ASX_E_REGION_POISONED` (306) | — | C-only |
| `ASX_E_TASK_NOT_FOUND` (400) | — | C-only: handle lookup miss |
| `ASX_E_SCHEDULER_UNAVAILABLE` (401) | `SpawnError::RuntimeUnavailable`, `SpawnError::LocalSchedulerUnavailable`; `ObligationAdmissionError::RuntimeUnavailable` | lossy |
| `ASX_E_NAME_CONFLICT` (402) | — | C-only |
| `ASX_E_TASK_NOT_COMPLETED` (403) | `try_join` → `Ok(None)` on a running task | exact (observation-level) |
| `ASX_E_POLL_BUDGET_EXHAUSTED` (404) | the driver's step limit (`LabConfig::max_steps`) | C-only as a status; Rust reports a step-limit harness failure |
| `ASX_E_OBLIGATION_ALREADY_RESOLVED` (500) | `ErrorKind::ObligationAlreadyResolved` | exact |
| `ASX_E_UNRESOLVED_OBLIGATIONS` (501) | `ErrorKind::ObligationLeak` at close | lossy |
| `ASX_E_CANCELLED` (600) | `ErrorKind::Cancelled`; `Cancelled` variants of mpsc/oneshot/broadcast/watch errors, `LockError`, `AcquireError`, `RwLockError`, `BarrierWaitError`, `OnceCellError`, `PoolError`; `JoinError::Cancelled` | exact as a status; the reason is compared separately |
| `ASX_E_WITNESS_PHASE_REGRESSION` … `ASX_E_WITNESS_EPOCH_MISMATCH` (601–605) | — | C-only: cancel-witness checks (Rust enforces these by types) |
| `ASX_E_DISCONNECTED` (700) | `ErrorKind::ChannelClosed`; `SendError::Disconnected`, `RecvError::Disconnected` (mpsc); oneshot `SendError::Disconnected`, `RecvError::Closed`, `TryRecvError::Closed`; broadcast/watch `Closed`; `AcquireError::Closed`; `PoolError::Closed` | lossy: every closed-peer result |
| `ASX_E_WOULD_BLOCK` (701) | `TryLockError::Locked`, `TryAcquireError`, `TryReadError::Locked`, `TryWriteError::Locked` | lossy: sync try-ops only (channels use 702/705) |
| `ASX_E_CHANNEL_FULL` (702) | `ErrorKind::ChannelFull`; mpsc `SendError::Full` | exact |
| `ASX_E_CHANNEL_NOT_DRAINED` (703) | — | C-only |
| `ASX_E_LAGGED` (704) | broadcast `RecvError::Lagged(n)` / `TryRecvError::Lagged(n)` (`src/channel/broadcast.rs:102-139`) | exact; `n` goes to the observation's `value` |
| `ASX_E_CHANNEL_EMPTY` (705) | `ErrorKind::ChannelEmpty`; mpsc `RecvError::Empty`; oneshot/broadcast `TryRecvError::Empty` | exact |
| `ASX_E_TIMER_NOT_FOUND` (800) | — | C-only |
| `ASX_E_TIMERS_PENDING` (801) | — | C-only: quiescence diagnostics (snapshot `timers_pending`) |
| `ASX_E_TIMER_DURATION_EXCEEDED` (802) | `TimerDurationExceeded{duration,max}` (`src/time/wheel.rs:216`) | exact |
| `ASX_E_TIMED_OUT` (803) | `time::Elapsed` (`src/time/elapsed.rs:26`); `TimeoutError`; `TimedError::TimedOut`; `LockError::TimedOut`; `PoolError::Timeout`; `ErrorKind::DeadlineExceeded` | lossy: every deadline expiry |
| `ASX_E_THRESHOLD_TIMEOUT` (804) | `ErrorKind::ThresholdTimeout` | exact |
| `ASX_E_TASKS_STILL_ACTIVE` … `ASX_E_QUIESCENCE_TASKS_LIVE` (900–905) | — | C-only as statuses; quiescence is compared through the snapshot (`quiescent`, live counts) |
| `ASX_E_RESOURCE_EXHAUSTED` (1000) | `ObligationAdmissionError::CapacityExhausted` | lossy: C arena exhaustion has no Rust analogue except the obligation mailbox |
| `ASX_E_OVERLOADED` (1001) | `ErrorKind::RateLimited` | lossy |
| `ASX_E_COST_QUOTA_EXHAUSTED` (1002) | `ErrorKind::CostQuotaExhausted` | exact |
| `ASX_E_POLL_QUOTA_EXHAUSTED` (1003) | `ErrorKind::PollQuotaExhausted` | exact |
| `ASX_E_STALE_HANDLE` (1100) | — | C-only: generation mismatch |
| `ASX_E_HOOK_MISSING` … `ASX_E_ALLOCATOR_SEALED` (1200–1203) | — | C-only: platform hooks |
| `ASX_E_AFFINITY_VIOLATION` … `ASX_E_AFFINITY_TABLE_FULL` (1300–1304) | — | C-only: thread affinity (Rust `Send`/`Sync`) |
| `ASX_E_EQUIVALENCE_MISMATCH` (1400) | — | C-only |
| `ASX_E_DUPLICATE_SYMBOL` (1401) | `ErrorKind::DuplicateSymbol` | exact |
| `ASX_E_OBJECT_MISMATCH` (1402) | `ErrorKind::ObjectMismatch` | exact |
| `ASX_E_CORRUPTED_SYMBOL` (1403) | `ErrorKind::CorruptedSymbol` | exact |
| `ASX_E_REPLAY_MISMATCH` (1500) | — | C-only |
| `ASX_E_CONFIG_FROZEN`, `ASX_E_CONFIG_RESTART_REQ` (1600–1601) | `ErrorKind::ConfigError` | lossy |
| `ASX_E_PERMISSION_DENIED` (1700) | — | C-only: capability masks (Rust's `Cx::spawn` mask returns `RuntimeUnavailable`, row 401) |

Rust values reachable through DSL v2 that have no row above, and how they
are handled:
- `ErrorKind::CancelTimeout` (E301) maps to `ASX_E_TIMED_OUT` (lossy).
- `ErrorKind::TaskNotOwned` maps to `ASX_E_INVALID_STATE` (lossy).
- `ObligationAdmissionError::HolderNotLive` and `HolderMismatch` map to
  `ASX_E_INVALID_STATE` (lossy).
- `JoinError::Panicked` is not a status: it is the `panicked` outcome (§4).
- `OnceCellError::AlreadyInitialized` maps to `ASX_E_ALREADY_EXISTS`. It is
  the one Rust use of that code, so the row stays C-only for everything else.

Encoding, decoding, transport and distributed `ErrorKind`s are outside DSL
v2's kernel scope (W1.3).

## 6. Snapshot

The end-of-scenario canonical state. It covers every entity the scenario
named or that appeared in the trace, including completed ones.

```json
{
  "now_ns": <u64>,
  "quiescent": <bool>,
  "tasks": {"<name>": {"state": "<task state>", "outcome": <outcome> | null,
                       "cancel_reason": <reason> | null,
                       "cleanup_budget": {"poll_quota": <u32>, "priority": <u8>} | null}},
  "regions": {"<name>": {"state": "<region state>", "parent": "<name>" | null,
                         "cancel_reason": <reason> | null}},
  "obligations": {"<name>": {"state": "<obligation state>", "kind": "<kind>",
                             "holder": "<task>", "region": "<region>",
                             "abort_reason": "<abort reason>" | null}},
  "timers_pending": [{"timer": "<name>", "deadline_ns": <u64>}],
  "channels": {"<name>": {"len": <u32>, "reserved": <u32>,
                          "sender_closed": <bool>, "receiver_closed": <bool>}}
}
```

- **Task fields.** A task's `outcome` is set iff its state is `Completed`.
  Its `cancel_reason` is the reason carried by `CancelRequested` /
  `Cancelling` / `Finalizing` (`src/record/task.rs:83-100`), or the
  cancellation reason of a cancelled outcome. `cleanup_budget` is the cleanup
  budget those states carry, per kind (`cancel.rs:1027-1043`).
- **Region fields.** A region's `cancel_reason` is its strongest reason
  after strengthening (§4).
- **`timers_pending`** is sorted by (`deadline_ns`, `timer`).
- **`channels`** is the only place channel effects are compared (§1). `len`
  is the number of queued values and `reserved` the number of outstanding
  send permits.

## 7. Observations

Each scenario step that calls a runtime API produces one observation, in
program order per task:

```json
{"task": "<name>", "step": <u32>, "op": "<DSL op>", "status": "<status name>", "value": <json> | null}
```

`value` carries the returned payload (for example a received integer, or a
`Lagged` count). The list is sorted by (`task`, `step`). Interleaving across
tasks is a scheduling matter (§1).

## 8. Independence and canonical form

### Footprints

Each event has a resource footprint: a list of (resource, mode) accesses,
where mode is `R` or `W`. These are restated from `resource_footprint`
(`src/trace/independence.rs:111-291`) for the projected kinds:

| Event | Footprint |
|---|---|
| `task.spawned`, `task.completed` | W task, R region |
| `cancel.requested` | W task, W region |
| `region.created` | W region, R parent (if any) |
| `region.close_begin`, `region.closed`, `region.cancelled` | W region |
| `obligation.reserved` / `committed` / `aborted` / `leaked` | W obligation, R task, R region |
| `obligation.handoff` | W obligation, W from_task, W to_task, W from_region, W to_region |
| `timer.scheduled`, `timer.fired`, `timer.cancelled` | W timer, R clock |
| `user.trace` | ∅ (empty) |

Resources are typed by entity name: `task:<n>`, `region:<n>`,
`obligation:<n>`, `timer:<n>`, plus the singleton `clock`.

### The independence relation

Two **distinct** events (different stream positions) are independent iff
either footprint is empty, or no pair of accesses touches the same resource
with at least one write (`accesses_conflict`, `independence.rs:101-104`;
`independent`, `:302-335`).

`user.trace` therefore commutes with everything, exactly as in Rust. A
`UserTrace` whose message starts with `obligation_handoff_v` but fails to
decode is a harness defect in this vocabulary, never an event. (Rust treats
it as conflicting with everything, `independence.rs:310-314`.)

The relation is checked by a table test on both sides: a C test under
`tests/unit/runtime/` and a Rust test in `tools/twin_run`. Both use the same
JSON table of event pairs and expected answers,
`tests/conformance/vocab_v2_independence_table.json`. Its 22 cases cover
every footprint rule; `"$reason"` stands for its `reason_fixture`.

### Foata layers

The single pass of `foata_layers` (`src/trace/canonicalize.rs:426-479`), over
the projected event stream in emission order:

```
layer(e) = 1 + max over e's accesses of:
             W access to r: highest layer of any earlier access to r
             R access to r: highest layer of any earlier W access to r
           (0 if there is no such earlier access)
```

Events with an empty footprint land in layer 0.

### Canonical trace

Within each layer, events are sorted by the bytes of their canonical JSON
(§9). The result is a list of layers, each a list of events. Rust orders a
layer by `event_sort_key` over arena ids (`canonicalize.rs:680-692`), which
is not comparable across engines, so this vocabulary uses the canonical bytes
of the named events instead.

## 9. Canonical JSON and digests

- **Canonical JSON** is RFC 8785 (JCS), restricted to null, booleans,
  integers, strings, arrays and objects. Floats are not allowed; integers
  are written in decimal. Object keys are sorted by their UTF-16 code units,
  as JCS specifies; for the ASCII keys used here that is byte order. There is
  no insignificant whitespace.
- **Digests** are `"sha256:" + lowercase hex` of the canonical bytes:
  - `trace_digest` = digest of the canonical trace (§8);
  - `snapshot_digest` = digest of the snapshot (§6);
  - `semantic_digest` = digest of
    `{"observations": [...], "snapshot": {...}, "trace": [[...], ...], "vocabulary": "asx.vocab.v2"}`.

Two runs agree in mode (a) iff their `snapshot_digest`s and observations
match. They agree in mode (b) iff their `semantic_digest`s match.

## 10. Versioning

The vocabulary id is `asx.vocab.v2`. Any change to kinds, fields, footprints,
encodings or the digest algorithm makes a new version, and fixtures record
the version they were captured under.

## 11. Work this vocabulary requires on the C side

The C runtime does not yet emit several projected kinds. W1.5 (bd-9kll.2.5)
must add them, in the runtime or as interpreter projections from runtime
state, following the emission rules in §3:
- `region.cancelled`, for every region of the subtree on every cancel call;
- `cancel.requested`, newly-cancelled tasks only;
- `obligation.leaked`;
- `obligation.handoff`;
- `user.trace`, including the exact oneshot and mpsc messages.

C's `ASX_TRACE_CHANNEL_SEND` / `CHANNEL_RECV`, `ASX_TRACE_TASK_TRANSITION`
and the `ASX_TRACE_SCHED_*` kinds other than `SCHED_COMPLETE` are not
projected. Their information is compared through the snapshot, the
observations or the schedule receipt.

The obligation kinds (§4) need C equivalents, at least `SendPermit` for
channel permits. C channel send must not register an obligation for the
non-reserving send path.
