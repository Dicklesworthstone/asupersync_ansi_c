# Vocabulary exclusions (`asx.vocab.v2`)

Companion to `docs/CANONICAL_VOCABULARY_V2.md` (bead W1.2, bd-9kll.2.2).

asupersync at the pinned rev has 45 `TraceEventKind` variants
(`/dp/asupersync/src/trace/event.rs:167-260`). Fifteen are projected into
vocabulary kinds; `UserTrace` yields both `user.trace` and
`obligation.handoff`. The other 30 are listed here.

Both engines drop an excluded kind **before** footprints and Foata layers
are computed, so an exclusion never makes the engines disagree. The
information each one carries is compared elsewhere, or is out of scope, as
stated.

## Projected (for reference)

`Spawn`, `Complete`, `RegionCreated`, `RegionCloseBegin`,
`RegionCloseComplete`, `RegionCancelled`, `CancelRequest`,
`ObligationReserve`, `ObligationCommit`, `ObligationAbort`, `ObligationLeak`,
`TimerScheduled`, `TimerFired`, `TimerCancelled`, `UserTrace`.

## Excluded

| Kind (line) | Why it is excluded | Where its information is compared |
|---|---|---|
| `Schedule` (171), `Yield` (173), `Wake` (175), `Poll` (177), `CancelAck` (183) | The lab runtime never records them in its trace buffer. Only the production three-lane scheduler's opt-in capture emits them (`src/runtime/scheduler/three_lane.rs:8643-8691`, `:4879-4899`, `:9348-9355`). | Dispatch order: forced-schedule receipt and schedule certificate (mode (c)). Cancel acknowledgement: task state and cancel reason in the snapshot. |
| `WorkerCancelRequested` (185), `WorkerCancelAcknowledged` (187), `WorkerDrainStarted` (189), `WorkerDrainCompleted` (191), `WorkerFinalizeCompleted` (193) | No production emitter at the pinned rev; they appear only in tests. | — |
| `TaskSpawnEnqueued` (251), `TaskAdmitted` (253) | Steps of Rust's spawn-gateway pipeline (`src/runtime/spawn_mailbox.rs:1949-1953`; `src/runtime/state.rs:1113`, `:1182-1187`). C spawns synchronously and has no gateway. | `task.spawned` and task state. Both kinds fall to Rust's `write(GlobalState)` fallback footprint (`src/trace/independence.rs:285-289`), so they commute with every projected event anyway. |
| `TimeAdvance` (211) | No production emitter at the pinned rev. | Snapshot `now_ns`; timer deadlines. |
| `IoRequested` (219), `IoReady` (221), `IoResult` (223), `IoError` (225) | I/O is outside DSL v2's kernel scope (W1.3). The lab emits `IoReady`/`IoRequested` only through `poll_io` (`src/lab/runtime.rs:5069`, `:5073`). | — (a later vocabulary version adds I/O with the native-I/O workstream). |
| `RngSeed` (227), `RngValue` (229) | No production emitter at the pinned rev. RNG draws go to the separate replay recorder (`src/trace/recorder.rs:468`, `:598`). | The scenario seed and the forced-schedule receipt. |
| `Checkpoint` (231) | No production emitter at the pinned rev. | — |
| `FuturelockDetected` (233) | A lab diagnostic (`src/lab/runtime.rs:5702-5714`). In a conformance run it is a harness failure, not a semantic event. | The twin-run harness fails the scenario. |
| `ChaosInjection` (235) | Chaos is disabled in conformance runs (`LabConfig` without `with_chaos`). | — (a chaos run is not a conformance run). |
| `MonitorCreated` (239), `MonitorDropped` (241), `DownDelivered` (243), `LinkCreated` (245), `LinkDropped` (247), `ExitDelivered` (249) | No production emitter at the pinned rev. | — (added with the actor/supervision vocabulary when Rust emits them). |
| `BudgetInstalled` (256), `BudgetConsumed` (259) | Emitted only by web request regions (`src/web/request_region.rs:1069`, `:1108`). | Budget effects: task outcomes and statuses (`ASX_E_POLL_QUOTA_EXHAUSTED`, `ASX_E_COST_QUOTA_EXHAUSTED`, `ASX_E_TIMED_OUT`). |

## C trace kinds that are not projected

| C kind (`include/asx/runtime/trace.h`) | Why |
|---|---|
| `ASX_TRACE_SCHED_POLL`, `SCHED_ROUND`, `SCHED_BUDGET`, `SCHED_QUIESCENT` | Scheduling, compared through the schedule receipt (mode (c)) and the snapshot's `quiescent`. |
| `ASX_TRACE_TASK_TRANSITION` | Rust has no task-state event; states are compared in the snapshot. |
| `ASX_TRACE_CHANNEL_SEND`, `ASX_TRACE_CHANNEL_RECV` | Rust has no channel event kinds, and a successful mpsc send is not traced (`/dp/asupersync/src/channel/mpsc.rs:715-727`). Channels are compared through the snapshot and observations. |
