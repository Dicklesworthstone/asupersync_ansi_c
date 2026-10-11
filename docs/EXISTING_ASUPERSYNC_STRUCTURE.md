# Existing Asupersync Structure — Canonical Extracted Semantics

> **Bead:** `bd-296.1` (first extraction); re-extracted under `bd-9kll.2.12` (item 3)
> **Status:** Semantic extraction for the C port. Where asupersync's normative small-step semantics now states a rule, the section is marked **Superseded** and names the rule; that spec is authoritative and this document only summarizes it. Sections the spec does not cover (channels, timer wheel, scheduler internals, C-only encodings) remain this document's own extraction, re-checked against the baseline below.
> **Rust baseline commit:** asupersync `5e60b1c4c` (`5e60b1c4c53d62aaddae68de3ee7de4732f1755b`, asupersync 0.6.0, the commit the v2 fixtures record as `rust_baseline_commit`)
> **Normative Rust spec:** `asupersync_v4_formal_semantics.md` at `5e60b1c4c` (Canonical Rule Index; §1 domains, §3 rules, §5 invariants, §6 progress, §7 laws). Rust's own code map: `formal/impl_refinement_map.md` (its line numbers are stale at `5e60b1c4c`). C status per rule: [`docs/C_REFINEMENT_MAP.md`](C_REFINEMENT_MAP.md).
> **Rust toolchain snapshot:** `rustc 1.100.0-nightly (908501772 2026-08-30)`, the toolchain the v2 fixtures were captured with
> **Baseline inventory:** `docs/rust_baseline_inventory.json` (still records `a9e737d8`, with the move to `5e60b1c4c` as `pending_rebase`; see `docs/rebase_records/2026-10-a9e737d8-to-5e60b1c4c.md`)
> **Citations:** a Rust `path:line` is asupersync at `5e60b1c4c` (`git -C /dp/asupersync show 5e60b1c4c:<path>`); Rust test files outside `src/` are written without backticks. A C `path:line` was read in this repository at commit `b412780` on 2026-10-10. Line numbers drift: treat them as grep anchors and look for the symbol named next to them.
> **Last verified:** 2026-10-10 (bd-9kll.2.12)
> **Previous baseline:** `38c152405bd03e2bd9eecf178bfbbe9472fed861` (Feb 2026), toolchain `rustc 1.95.0-nightly (7f99507f5 2026-02-19)`
> **Consolidated by:** BlueCat (claude-code/opus-4.6), 2026-02-27
> **Prior draft by:** CopperSpire, 2026-02-27
> **Consolidated artifact inputs:**
> - `docs/LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15, MossySeal)
> - `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` prior draft (bd-296.16, CopperSpire)
> - `docs/CHANNEL_TIMER_DETERMINISM.md` (bd-296.17, BeigeOtter)
> - `docs/CHANNEL_TIMER_SEMANTICS.md` (bd-296.17, MossySeal)
> - `docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md` (bd-296.17, BlueCat)
> - `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` (bd-296.18, CopperSpire)
> - `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` (bd-296.19, BeigeOtter/MossySeal/GrayKite)
> - `docs/RUST_BASELINE_PROVENANCE.md` + `docs/rust_baseline_inventory.json` (bd-296.12)

This document was the semantic baseline for the ANSI C port; it consolidates the Phase 1 extraction slices. Since asupersync published normative semantics, the authority order is: `asupersync_v4_formal_semantics.md` (the rule), the Rust code at `5e60b1c4c` (how Rust realizes it), then this document (a summary with citations). Downstream work should read the C status of a rule in `docs/C_REFINEMENT_MAP.md`, not here.

---

## Table of Contents

1. [Provenance and Scope](#1-provenance-and-scope)
2. [Core Semantic Model](#2-core-semantic-model)
3. [Region Lifecycle](#3-region-lifecycle)
4. [Task Lifecycle](#4-task-lifecycle)
5. [Obligation Lifecycle](#5-obligation-lifecycle)
6. [Cancellation Protocol](#6-cancellation-protocol)
7. [Outcome Severity Lattice](#7-outcome-severity-lattice)
8. [Budget Algebra](#8-budget-algebra)
9. [MPSC Channel Semantics](#9-mpsc-channel-semantics)
10. [Timer Wheel Semantics](#10-timer-wheel-semantics)
11. [Deterministic Scheduler Semantics](#11-deterministic-scheduler-semantics)
12. [Cross-Domain Interactions](#12-cross-domain-interactions)
13. [Quiescence and Finalization Invariants](#13-quiescence-and-finalization-invariants)
14. [Deterministic Tie-Break Contract](#14-deterministic-tie-break-contract)
15. [Forbidden Behavior Catalog](#15-forbidden-behavior-catalog)
16. [Error Code Reference](#16-error-code-reference)
17. [Handle Encoding Reference](#17-handle-encoding-reference)
18. [Invariant Schema](#18-invariant-schema)
19. [Fixture Family Mapping](#19-fixture-family-mapping)
20. [C Port Implementation Contract](#20-c-port-implementation-contract)

---

## 1. Provenance and Scope

### 1.1 Primary Rust Source Surfaces

Line counts are at `5e60b1c4c` (whole file, tests included).

| Source File | Domain | Lines |
|-------------|--------|-------|
| `asupersync_v4_formal_semantics.md` | **Normative small-step semantics** (rule index, rules, invariants) | 1910 |
| `formal/impl_refinement_map.md` | Rust's own spec-to-code map (line numbers stale) | 226 |
| `src/record/region.rs` | Region lifecycle (record) | 4445 |
| `src/record/task.rs` | Task lifecycle (record) | 3291 |
| `src/record/obligation.rs` | Obligation lifecycle (record) | 1037 |
| `src/runtime/state.rs` | Runtime state: `cancel_request`, `advance_region_state`, obligations, completion | 12448 |
| `src/cx/cx.rs` | `Cx::checkpoint`, masking, budget-deadline timers | 9392 |
| `src/runtime/task_handle.rs` | Join futures, spawn completion policy | 2470 |
| `src/types/outcome.rs` | Outcome lattice | 1323 |
| `src/types/budget.rs` | Budget algebra | 2586 |
| `src/types/cancel.rs` | Cancellation kinds, reasons, witnesses | 2800 |
| `src/channel/mpsc.rs` | MPSC channel core | 2279 |
| `src/channel/session.rs` | Obligation-tracked channel (and tracked oneshot) | 2382 |
| `src/time/wheel.rs` | Hierarchical timer wheel | 3509 |
| `src/time/driver.rs` | Timer driver/integration | 3063 |
| `src/time/deadline.rs` | Deadline propagation helpers (`with_deadline`, `with_timeout`) | 554 |
| `src/runtime/timer.rs` | Runtime timer heap (`TimerHeap`; no caller outside its module) | 903 |
| `src/lab/runtime.rs` | Lab runtime and `LabScheduler`: the deterministic dispatch that C's lab dispatch ports | 12988 |
| `src/runtime/scheduler/three_lane.rs` | Production multi-worker scheduler loop | 10031 |
| `src/runtime/scheduler/priority.rs` | Priority lanes, RNG tie-break, `ScheduleCertificate` | 3970 |
| `src/runtime/scheduler/global_injector.rs` | Global task injection | 1341 |
| `src/runtime/scheduler/stealing.rs` | `steal_task` (used only by the legacy worker) | 434 |
| `src/runtime/scheduler/intrusive.rs` | Intrusive ring and stack (not used by the scheduler) | 1837 |
| `src/combinator/join.rs` | Join combinator | 1242 |
| tests/algebraic_laws.rs | Algebraic law tests | 1097 |

All paths relative to `/data/projects/asupersync/` (`/dp/asupersync`), read at `5e60b1c4c`.

### 1.2 Canonical Scope

This document covers the complete kernel semantic surface:

1. Lifecycle state authorities (region, task, obligation, cancellation)
2. Outcome severity lattice and join semantics
3. Budget meet algebra, exhaustion predicates, and consume rules
4. MPSC channel: two-phase reserve/send/abort, backpressure, fairness, eviction
5. Timer wheel: 4-level hierarchy, insertion, firing, O(1) cancel, coalescing
6. Deterministic scheduler: 3-lane architecture, governor suggestions, fairness
7. Cross-domain interaction contracts
8. Quiescence definition and finalization invariants
9. Deterministic tie-break ordering contract
10. Forbidden behavior catalog with fixture IDs

Items 5 and 6 describe Rust's production structures. C implements neither a hierarchical timer wheel nor the multi-worker three-lane scheduler: it has a flat timer table plus per-task timers (§10) and a port of Rust's single-worker lab scheduler (§11). The kernel rules of items 1–3 and 8 are now stated normatively by `asupersync_v4_formal_semantics.md`; see the superseded notes in §3–§8 and §13.

### 1.3 Determinism Contract

> **Superseded by `asupersync_v4_formal_semantics.md` §7.1 and §8.3 (rules `inv.determinism.replayable` #46 and `def.determinism.seed_equivalence` #47, named only in the index), with §1.8 (trace independence, explanatory); C status: see C_REFINEMENT_MAP.md rows `inv.determinism.replayable` and `def.determinism.seed_equivalence`.** The spec fixes a lab configuration and does not require identical step-by-step schedules (§7.1); the contract below is the C port's stronger one, and it holds only against Rust's `LabRuntime` (one `DetRng` from the config seed, `src/lab/runtime.rs:4499`). Rust's production `ThreeLaneScheduler` runs on OS threads and seeds each worker's RNG with its worker id (`src/runtime/scheduler/three_lane.rs:2365`), so it is not seed-deterministic. In C, lab dispatch is seed-deterministic (`src/runtime/lab_dispatch.c:210`); the round-robin `asx_scheduler_run` is deterministic but ignores the seed (slot order).

For fixed `(scenario_input, seed, profile, codec_schema)` and lab dispatch, all runtime behavior is deterministic:

- Scheduler tie-break sequencing
- Channel waiter/service ordering
- Timer equal-deadline ordering
- Cancellation witness phase progression
- Exhaustion and failure outcomes
- Event journal ordering

---

## 2. Core Semantic Model

> **Superseded by `asupersync_v4_formal_semantics.md` §1 (Domains) and §2 (Global State: `RegionRecord`, `TaskRecord`, `ObligationRecord`, `SchedulerState`); C status: see C_REFINEMENT_MAP.md rows `def.ownership.region_tree` (#35), `inv.ownership.single_owner` (#33) and `inv.ownership.task_owned` (#34).** The spec's machine state is `Σ = ⟨R, T, O, S, τ_now⟩`. The four authorities below are this document's grouping of it.

### 2.1 Runtime Authorities

| Authority | Domain | Scope |
|-----------|--------|-------|
| **Region** | Admission, close progression, child ownership | Who can enter, when to drain/finalize |
| **Task** | Execution, cancel, finalization phase progression | Poll lifecycle, cancel observation |
| **Obligation** | Exactly-once reserve/commit/abort/leak linearity | Resource promise tracking |
| **Cancellation** | Monotone witness phase and reason strengthening | Cancel protocol phases |

### 2.2 Semantic Plane vs Resource Plane

All behavior in this document belongs to the **semantic plane**: outcomes, orderings, and invariants are identical across profiles. The **resource plane** (memory limits, wait policies, queue capacities) varies by profile but never changes semantic behavior.

---

## 3. Region Lifecycle

> **Superseded by `asupersync_v4_formal_semantics.md` §1.6 and §3.3 (rules `CLOSE-BEGIN` = `rule.region.close_begin` #22, `CLOSE-CANCEL-CHILDREN` = `rule.region.close_cancel_children` #23, `CLOSE-CHILDREN-DONE` = `rule.region.close_children_done` #24, `CLOSE-RUN-FINALIZER` = `rule.region.close_run_finalizer` #25, `CLOSE-COMPLETE` = `rule.region.close_complete` #26); C status: see C_REFINEMENT_MAP.md rows `rule.region.close_begin` … `rule.region.close_complete` (#22 and #24 Implemented, #23 Variant, #25 and #26 Partial).** Summary: Open → Closing → [Draining] → Finalizing → Closed. Full per-row Rust and C citations: `docs/LIFECYCLE_TRANSITION_TABLES.md` §1.

### 3.1 States

Rust `RegionState`: `src/record/region.rs:76`. C `asx_region_state`: `include/asx/asx_ids.h:102`.

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Open` | `ASX_REGION_OPEN` | Active; tasks, child regions and obligations may be created |
| `Closing` | `ASX_REGION_CLOSING` | Close requested or region cancelled; no new tasks, child regions or obligations; existing work continues |
| `Draining` | `ASX_REGION_DRAINING` | Entered only while the region still has child **regions** (Rust `src/runtime/state.rs:10220`; C `src/runtime/quiescence.c:233`) |
| `Finalizing` | `ASX_REGION_FINALIZING` | All child tasks Completed and child regions Closed; region finalizers run (LIFO) |
| `Closed` | `ASX_REGION_CLOSED` | Terminal; Rust stores the close outcome on the record (`src/record/region.rs:1139`) and reclaims the region heap |

### 3.2 Legal Transitions

| # | From | To | Trigger | Preconditions | Postconditions |
|---|------|----|---------|---------------|----------------|
| R1 | `Open` | `Closing` | Rust `begin_close(reason)` (`src/record/region.rs:1699`); C `asx_region_close` (`src/runtime/lifecycle.c:893`) | state==Open | No new spawns; Rust sets or strengthens the cancel reason if one is given. C's plain close cancels no task |
| R1a | `Open` | `Closing` | An ancestor region is cancelled | The cancel walks the subtree, parents first (Rust `cancel_request`, `src/runtime/state.rs:7547`; C `asx_region_cancel`, `src/runtime/cancellation.c:421`) | Descendant gets ParentCancelled from its immediate parent; its tasks are cancelled |
| R1b | `Open` | `Closing` | This region is cancelled | Live region | Region reason set or strengthened; its tasks get CancelRequested |
| R2 | `Closing` | `Draining` | Rust `begin_drain()` (`src/record/region.rs:1743`) from `advance_region_state` (`src/runtime/state.rs:10220`); C `src/runtime/quiescence.c:233` | state==Closing and at least one child region | Waits for child regions. Not a cancel step: when the close came from a cancel request, the tasks were already cancelled by it (Variant vs the spec, row `rule.region.close_cancel_children`) |
| R3 | `Closing` | `Finalizing` | Rust `begin_finalize()` (`src/record/region.rs:1756`); C `src/runtime/quiescence.c:238` | Every task terminal, every child region Closed, no pending spawn (`can_region_finalize`, `src/runtime/state.rs:7938`) | **Fast path**: skip drain phase |
| R4 | `Draining` | `Finalizing` | Same as R3 | Same as R3 | Finalizer execution begins (LIFO) |
| R5 | `Finalizing` | `Closed` | Rust `complete_close()` (`src/record/region.rs:1786`); C `src/runtime/quiescence.c:255` | See 3.4 | Rust: close outcome recorded, heap reclaimed, close waiters woken. C: region unlinked from its parent, close waiters woken |

### 3.3 Forbidden Transitions (12 Must-Fail)

C rejects every pair outside its table (`src/core/transition_tables.c:22`) with `ASX_E_INVALID_TRANSITION`. Rust's record transitions are compare-and-swap steps that return `false`.

| From | To | Error | Rationale |
|------|----|-------|-----------|
| `Closing` | `Open` | `ASX_E_INVALID_TRANSITION` | Cannot reopen |
| `Draining` | `Open` | `ASX_E_INVALID_TRANSITION` | Cannot reopen |
| `Draining` | `Closing` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `Open` | `ASX_E_INVALID_TRANSITION` | Cannot reopen |
| `Finalizing` | `Closing` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `Draining` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Closed` | (any) | `ASX_E_INVALID_TRANSITION` | Terminal absorbing |
| `Open` | `Draining` | `ASX_E_INVALID_TRANSITION` | Must pass through Closing |
| `Open` | `Finalizing` | `ASX_E_INVALID_TRANSITION` | Must pass through Closing |
| `Open` | `Closed` | `ASX_E_INVALID_TRANSITION` | Must pass through full sequence |
| `Closing` | `Closed` | `ASX_E_INVALID_TRANSITION` | Must pass through Finalizing |
| `Draining` | `Closed` | `ASX_E_INVALID_TRANSITION` | Must pass through Finalizing |

### 3.4 Close Preconditions (Finalizing -> Closed)

> **Superseded by `asupersync_v4_formal_semantics.md` §3.3 `CLOSE-COMPLETE` (`rule.region.close_complete` #26) and §3.4.5 (`inv.obligation.ledger_empty_on_close` #20); C status: see C_REFINEMENT_MAP.md rows `rule.region.close_complete` (Partial) and `inv.obligation.ledger_empty_on_close`.**

All must hold before transition to Closed:

1. All child tasks in terminal state (`Completed`)
2. All child sub-regions in `Closed` state
3. No obligation of the region is still `Reserved`
4. Finalizers drained (Rust: empty finalizer stack; C: cleanup stack drained LIFO, `src/core/cleanup.c:92`)
5. C only: the ghost linearity monitor reports unresolved obligations (`src/core/ghost.c:182`); it reports and does not block

If item 3 fails, Rust leak-audits the Reserved obligations (once no task is left) and closes (`src/runtime/state.rs:10271`); C keeps the region in `Finalizing` and returns `ASX_E_OBLIGATIONS_UNRESOLVED` (`src/runtime/quiescence.c:243`). C never returns `ASX_E_UNRESOLVED_OBLIGATIONS` or `ASX_E_INCOMPLETE_CHILDREN` here (`bd-9kll.3.4`, `bd-udlh`).

### 3.5 Operations Gated by Region State

> **Superseded by `asupersync_v4_formal_semantics.md` §3.1 `SPAWN` (`rule.ownership.spawn` #36) and §3.4 `RESERVE` (`rule.obligation.reserve` #13); C status: see C_REFINEMENT_MAP.md rows `rule.ownership.spawn` (Partial, `bd-9kll.3.5`) and `rule.obligation.reserve` (Partial).**

| Operation | Allowed States | Error if Wrong |
|-----------|---------------|----------------|
| Create child task | Rust: `Open` for normal tasks (`src/record/region.rs:140`), `Open`/`Finalizing` for cleanup tasks (`src/record/region.rs:150`). C: `Open`/`Finalizing` for every spawn (`src/core/transition_tables.c:89`) | C `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:1007`) |
| Create child region | `Open` | C `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:746`) |
| Create obligation | `Open` | C `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:1286`) |
| Resolve obligation | `Open`, `Closing`, `Draining`, `Finalizing` | N/A while Reserved. Rust rejects a resolve after the region is finalized (`ErrorKind::RegionFinalized`, `src/runtime/obligation_table.rs:569`) |
| Access arena | Not `Closed` | C `ASX_E_REGION_CLOSED` (only user: `asx_task_spawn_captured`, `src/runtime/lifecycle.c:1105`) |
| Query status | Any | Fails only on a bad handle |

**Edge case:** Rust admits only cleanup tasks in `Finalizing` (test `normal_task_admission_rejected_when_finalizing`, `src/record/region.rs:2826`; the earlier `admission_allowed_when_finalizing` test is gone). C admits every spawn there; that is recorded drift (`bd-9kll.3.5`). Child region creation is NOT allowed during `Finalizing` in either.

---

## 4. Task Lifecycle

> **Superseded by `asupersync_v4_formal_semantics.md` §1.5, §3.1 (`SPAWN` = `rule.ownership.spawn` #36, `SCHEDULE`, `COMPLETE-OK`, `COMPLETE-ERR`) and §3.2.5 (canonical cancellation automaton: `rule.cancel.request` #1, `rule.cancel.acknowledge` #2, `rule.cancel.drain` #3, `rule.cancel.finalize` #4, `rule.cancel.checkpoint_masked` #10); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.request` … `rule.cancel.finalize` (#3 and #4 Variant), `rule.cancel.checkpoint_masked` (Variant) and the supplementary §3.1 rows.** Rust's 13 legal pairs (`TaskPhase::is_valid_transition`, `src/record/task.rs:166`) and C's (`src/core/transition_tables.c:48`) are the same. Full per-row Rust and C citations: `docs/LIFECYCLE_TRANSITION_TABLES.md` §2.

### 4.1 States

Rust `TaskState`: `src/record/task.rs:77`. C `asx_task_state`: `include/asx/asx_ids.h:114`.

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Created` | `ASX_TASK_CREATED` | Allocated but not yet polled |
| `Running` | `ASX_TASK_RUNNING` | Actively being polled by scheduler |
| `CancelRequested` | `ASX_TASK_CANCEL_REQUESTED` | Cancel requested; not yet acknowledged at a checkpoint |
| `Cancelling` | `ASX_TASK_CANCELLING` | Cancel acknowledged; cleanup runs under the cleanup budget |
| `Finalizing` | `ASX_TASK_FINALIZING` | Cleanup done; the task is running finalizers |
| `Completed` | `ASX_TASK_COMPLETED` | Terminal; outcome determined |

### 4.2 Transition Matrix (13 Legal of 36 Pairs)

```
From\To        Created  Running  CancelReq  Cancelling  Finalizing  Completed
Created           .       T1       T2           .           .          T3
Running           .        .       T4           .           .          T5
CancelReq         .        .       T6          T7           .          T8
Cancelling        .        .        .          T9          T10         T11
Finalizing        .        .        .           .          T12         T13
Completed         .        .        .           .           .           .
```

### 4.3 Legal Transitions

| # | From | To | Trigger | Postconditions |
|---|------|----|---------|----------------|
| T1 | `Created` | `Running` | Rust `start_running()` (`src/record/task.rs:1078`); C first poll (`src/runtime/scheduler.c:1038`) | Task poll function invoked |
| T2 | `Created` | `CancelRequested` | Rust `request_cancel*` (`src/record/task.rs:803`) | Cancel before first poll; Rust sets `cancel_epoch` to 1. C takes T2 in one step, as does a budget cancel that reaches the record of a task not yet polled (`src/runtime/cancellation.c:186`, `src/runtime/cancellation.c:78`) |
| T3 | `Created` | `Completed` | Rust `complete(outcome)` (`src/record/task.rs:1260`) | Completion before any poll. C never takes T3 (the scheduler always takes T1 first) |
| T4 | `Running` | `CancelRequested` | Rust `request_cancel*`; C `asx_task_cancel_reason_internal` (`src/runtime/cancellation.c:96`) | Cancel delivered; `cancel_epoch` becomes 1 (C `src/runtime/cancellation.c:204`) |
| T5 | `Running` | `Completed` | `complete(outcome)`; C `sched_complete` (`src/runtime/scheduler.c:732`) | Normal completion, error, or panic |
| T6 | `CancelRequested` | `CancelRequested` | `request_cancel*` (`src/record/task.rs:735`) | **Strengthening**: reason strengthened, cleanup budgets met; not a new cancel |
| T7 | `CancelRequested` | `Cancelling` | Rust `acknowledge_cancel()` (`src/record/task.rs:1341`); C `asx_checkpoint` (`src/runtime/cancellation.c:594`) | Unmasked checkpoint; cleanup budget becomes the task's budget, `polls_remaining` set (C applies it after the acknowledging poll, `src/runtime/lifecycle.c:465`) |
| T8 | `CancelRequested` | `Completed` | `complete(outcome)` | **Outcome is `Cancelled(reason)`**: Rust maps `Ok`/`Err` in any cancel state to `Cancelled(reason)` (`src/record/task.rs:1267`); C likewise (`sched_cancel_dominates`, `src/runtime/scheduler.c:402`). `Panicked` passes through. Exception in both: a task spawned inside another task's poll that acknowledged a cancel arriving after its first poll keeps its value (Rust `classify_spawn_completion`, `src/runtime/task_handle.rs:173`) |
| T9 | `Cancelling` | `Cancelling` | `request_cancel*` (`src/record/task.rs:751`) | **Strengthening**: as T6; the met budget also becomes the task's budget |
| T10 | `Cancelling` | `Finalizing` | Rust `cleanup_done()` (`src/record/task.rs:1378`); C `asx_task_finalize` (`src/runtime/cancellation.c:662`) | Cleanup finished. Rust's lab takes it when a task in a cancel state finishes its poll with `Ok` (`src/lab/runtime.rs:4876`; other results go through T11); C only when the task calls `asx_task_finalize`, otherwise T11 (Variant, row `rule.cancel.drain`) |
| T11 | `Cancelling` | `Completed` | `complete(outcome)` | `Ok`/`Err` become `Cancelled(reason)` as in T8; a panic stays `Panicked` |
| T12 | `Finalizing` | `Finalizing` | `request_cancel*` (`src/record/task.rs:777`) | **Strengthening**: as T9 |
| T13 | `Finalizing` | `Completed` | Rust `finalize_done()` (`src/record/task.rs:1416`); C at the next dispatch (`src/runtime/scheduler.c:1222`) | Rust returns a `CancelWitness` (phase Completed); outcome is `Cancelled(reason)` |

**Strengthening (T6, T9, T12):** Not state changes. Cancel reason strengthened via `CancelReason::strengthen` (`src/types/cancel.rs:956`; §6.4). Cleanup budget combined via `Budget::combine` (min on deadline and quotas, max on priority). Both leave `cancel_epoch` unchanged (C `src/runtime/cancellation.c:168`).

### 4.4 Forbidden Transitions (23 Must-Fail)

10 backward + 5 skipped + 2 non-cancel self-loops (`Created → Created`, `Running → Running`) + 6 from-terminal = 23 forbidden (the first extraction wrote 10 + 6 + 7). C returns `ASX_E_INVALID_TRANSITION` from its table (`src/core/transition_tables.c:77`). Rust asserts in debug builds (`TaskPhaseCell::store`, `src/record/task.rs:265`); its record methods return `false`/`None` when the current state does not fit.

### 4.5 Cancellation Observation Rules

> **Superseded by `asupersync_v4_formal_semantics.md` §3.2 `CANCEL-ACKNOWLEDGE` (`rule.cancel.acknowledge` #2), `CHECKPOINT-MASKED` (`rule.cancel.checkpoint_masked` #10) and §6 PROG-CANCEL (`prog.cancel.drains` #9); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.acknowledge`, `rule.cancel.checkpoint_masked` (Variant) and `prog.cancel.drains` (Partial).**

1. A cancel request moves the task to `CancelRequested` and wakes it so it can reach a checkpoint
2. Task does **not** acknowledge the cancel until a checkpoint: Rust `Cx::checkpoint` (`src/cx/cx.rs:2749`), C `asx_checkpoint` (`src/runtime/cancellation.c:512`). C has no `asx_is_cancelled`
3. Between `CancelRequested` and acknowledgement, task continues normal poll logic
4. Once acknowledged at an unmasked checkpoint, the task transitions to `Cancelling` and its cleanup budget becomes its budget (`acknowledge_cancel`, `src/record/task.rs:1341`). Under a mask (depth > 0, bounded by 64 in both) the cancel stays pending; neither engine consumes mask per checkpoint as the spec does (Variant)
5. A spent cleanup budget only strengthens the reason to `PollQuota` (lab, `src/lab/runtime.rs:4664`; advisory in production, `src/runtime/scheduler/three_lane.rs:1273`): Rust never force-completes a task. C's opt-in `cleanup_hard_bound` does (a deviation excluded from parity, bd-9kll.3.2)
6. **A task that completes while `CancelRequested` (or later) does not keep its natural outcome: `Ok`/`Err` become `Cancelled(reason)`** (T8; the first extraction said the opposite). The exception is in T8

### 4.6 Poll Contract

C has no `ASX_POLL_*` codes. A C poll function (`asx_task_poll_fn`, `include/asx/runtime/runtime.h:87`) returns an `asx_status`, mapped in `sched_poll_slot` (`src/runtime/scheduler.c:1025`):

| Poll Return | Meaning | State Effect |
|------------|---------|-------------|
| `ASX_E_PENDING` | Yielded or parked; needs another poll | Remains in current state |
| `ASX_OK` | Completed work | `Completed(Ok)`, or `Completed(Cancelled)` if a pending cancel dominates (T8) |
| any other status | Fatal error | `Completed(Err)` (or `Cancelled` if a pending cancel dominates); region fault containment applies |
| (`asx_task_panic` called) | Panic | `Completed(Panicked)` whatever the poll returned |

---

## 5. Obligation Lifecycle

> **Superseded by `asupersync_v4_formal_semantics.md` §1.7, §1.9 and §3.4 (`RESERVE` = `rule.obligation.reserve` #13, `COMMIT` = `rule.obligation.commit` #14, `ABORT` = `rule.obligation.abort` #15, `LEAK` = `rule.obligation.leak` #16; §3.4.4 = `inv.obligation.linear` #18; §3.4.5 = `inv.obligation.ledger_empty_on_close` #20; §3.4.6 = `inv.obligation.no_leak` #17; §5 = `inv.obligation.bounded` #19); C status: see C_REFINEMENT_MAP.md rows `rule.obligation.reserve` … `prog.obligation.resolves` (#13, #14, #19, #21 Partial).** Full per-row citations: `docs/LIFECYCLE_TRANSITION_TABLES.md` §3.

### 5.1 States

Rust `ObligationState`: `src/record/obligation.rs:192`. C `asx_obligation_state`: `include/asx/asx_ids.h:127`.

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Reserved` | `ASX_OBLIGATION_RESERVED` | Resource/promise reserved; not yet fulfilled; blocks region close |
| `Committed` | `ASX_OBLIGATION_COMMITTED` | Terminal: fulfilled successfully |
| `Aborted` | `ASX_OBLIGATION_ABORTED` | Terminal: released, with an abort reason (Cancel, Error, Explicit) |
| `Leaked` | `ASX_OBLIGATION_LEAKED` | Terminal error: the holder completed (or dropped the token) without resolving it |

### 5.2 Legal Transitions

| From | To | Trigger | Postconditions |
|------|----|---------|----------------|
| `Reserved` | `Committed` | Rust `commit_obligation` (`src/runtime/state.rs:6534`); C `asx_obligation_commit()` (`src/runtime/lifecycle.c:1367`) | Fulfilled; region pending count drops. The spec's holder-only precondition is not checked by C (row `rule.obligation.commit`, Partial) |
| `Reserved` | `Aborted` | Rust `abort_obligation` (`src/runtime/state.rs:6745`); C `asx_obligation_abort()` / `asx_obligation_abort_with_reason()` | Released; abort reason recorded |
| `Reserved` | `Leaked` | The holder completes while holding it (Rust `src/runtime/state.rs:8341`; C `src/runtime/lifecycle.c:515`), or C `asx_obligation_drop` (`src/runtime/lifecycle.c:1398`); Rust also leak-audits what is still Reserved in a Finalizing region with no task left | Leak handled under the leak policy: Rust default Panic (`src/runtime/state.rs:2338`), C default LOG (`src/runtime/lifecycle.c:72`); recorded drift `bd-9kll.3.6` |

### 5.3 Forbidden Transitions (Must-Fail)

C never returns `ASX_E_OBLIGATION_ALREADY_RESOLVED` (declared, unused) and has no `ASX_E_OBLIGATION_LEAKED`. Rust returns `ErrorKind::ObligationAlreadyResolved` (`src/runtime/obligation_table.rs:577`).

| From | To | Error |
|------|----|-------|
| `Committed` | any | C `ASX_E_INVALID_TRANSITION` (`src/runtime/lifecycle.c:1377`); Rust `ObligationAlreadyResolved` |
| `Aborted` | any | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` |
| `Leaked` | any | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` |
| `Reserved` | `Reserved` | `ASX_E_INVALID_TRANSITION` (table) |

Double-resolution is **not** idempotent by design; it indicates a logic error.

### 5.4 Linearity Enforcement

> **Superseded by `asupersync_v4_formal_semantics.md` §3.4.4 and §3.4.6 (`inv.obligation.linear` #18, `inv.obligation.no_leak` #17); C status: see C_REFINEMENT_MAP.md rows `inv.obligation.linear` and `inv.obligation.no_leak`.** Neither engine keeps the bit ledger with a `popcnt` that the first extraction described.

1. `reserve` creates a Reserved obligation (Rust table plus region pending count, `src/record/region.rs:1449`; C slot linked into the holder's held list, `src/runtime/lifecycle.c:1271`)
2. `commit`/`abort` resolve it exactly once (terminal states absorbing, `src/core/transition_tables.c:65`)
3. Leaks are detected when the holder completes: each obligation it still holds becomes `Leaked`
4. At region close Rust leak-audits what is still Reserved and closes; C refuses to close (`ASX_E_OBLIGATIONS_UNRESOLVED`, stays Finalizing; `bd-9kll.3.4`)

### 5.5 Region-Obligation Interaction

| Region State | Creation Allowed | Resolution Allowed |
|-------------|-----------------|-------------------|
| `Open` | Yes | Yes |
| `Closing` | No (C `ASX_E_REGION_CLOSED`) | Yes |
| `Draining` | No | Yes |
| `Finalizing` | No | Yes (Rust until its leak audit; C until close, which it refuses while one is Reserved) |
| `Closed` | No | No Reserved obligation remains; Rust rejects a late resolve with `RegionFinalized` (`src/runtime/obligation_table.rs:569`) |

Also not in the first extraction: Rust requires the holder to belong to the obligation's region (`ErrorKind::TaskNotOwned`, `src/runtime/state.rs:6130`); C checks neither that nor, outside a poll, that a holder exists (`bd-9kll.3.4` item 5 covers only the missing holder).

---

## 6. Cancellation Protocol

> **Superseded by `asupersync_v4_formal_semantics.md` §1.3 (`def.cancel.reason_kinds` #7, `def.cancel.severity_ordering` #8), §3.2 / §3.2.5 (`rule.cancel.request` #1 … `rule.cancel.finalize` #4, `inv.cancel.idempotence` #5, `rule.cancel.checkpoint_masked` #10), `strengthen` (`def.cancel.reason_ordering` #32), §5 INV-CANCEL-PROPAGATES (`inv.cancel.propagates_down` #6) and INV-MASK-BOUNDED (#11, #12); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.request` … `inv.cancel.mask_monotone` (#12 Missing) and `def.cancel.reason_ordering`.** The witness (§6.1–§6.3, §6.5, §6.6) is a Rust runtime artifact the spec does not define (`CancelWitness`, `src/types/cancel.rs:323`); C has a reduced one. Full per-row citations: `docs/LIFECYCLE_TRANSITION_TABLES.md` §4.

### 6.1 Phases

Rust `CancelPhase`: `src/types/cancel.rs:295` (rank at `src/types/cancel.rs:308`). C `asx_cancel_phase`: `include/asx/asx_ids.h:164`.

| Phase | Rank | Enum Value | Description |
|-------|------|-----------|-------------|
| `Requested` | 0 | `ASX_CANCEL_PHASE_REQUESTED` | Cancel requested; not yet acknowledged |
| `Cancelling` | 1 | `ASX_CANCEL_PHASE_CANCELLING` | Acknowledged; cleanup in progress |
| `Finalizing` | 2 | `ASX_CANCEL_PHASE_FINALIZING` | Cleanup done; finalizers running |
| `Completed` | 3 | `ASX_CANCEL_PHASE_COMPLETED` | Task completed `Cancelled` |

No `None` phase. Rust produces no witness while `cancel_epoch == 0` (`src/record/task.rs:1053`), and the first witness must have a non-zero epoch (`validate_initial`, `src/types/cancel.rs:369`).

### 6.2 Legal Phase Transitions

Rust (`validate_transition`, `src/types/cancel.rs:382`): valid when task, region and epoch match, `next.phase.rank() >= prev.phase.rank()`, and severity does not decrease. Same-phase pairs are strengthening. Skip transitions (e.g., `Requested -> Completed`) are allowed because rank is non-decreasing. C's `asx_cancel_witness_advance` (`src/core/cancel.c:141`) accepts only a strictly larger phase, so it rejects a same-phase advance (`bd-9kll.3.7`).

### 6.3 Forbidden Phase Transitions

Any `next.phase.rank() < prev.phase.rank()` is Rust `CancelWitnessError::PhaseRegression`; any `next.reason.severity() < prev.reason.severity()` is `ReasonWeakened` (`src/types/cancel.rs:414`). C returns `ASX_E_INVALID_TRANSITION` for a regression (`src/core/cancel.c:147`) and never returns `ASX_E_WITNESS_PHASE_REGRESSION` or `ASX_E_WITNESS_REASON_WEAKENED` (declared, unused).

### 6.4 Cancellation Kinds (11 Variants)

Severities and cleanup budgets match Rust exactly (`CancelKind::severity`, `src/types/cancel.rs:473`; `CancelReason::cleanup_budget`, `src/types/cancel.rs:1027`; C `include/asx/asx_ids.h:172`, `src/core/cancel.c:35`). C's enum ordinals differ from Rust's declaration order (Rust ParentCancelled=7 … LinkedExit=10; C LinkedExit=7 … Shutdown=10; row `def.cancel.reason_kinds`, `bd-9kll.3.7`); vocabulary v2 encodes kinds by name.

| Kind | Enum Value | Severity | Cleanup Quota | Cleanup Priority | Category |
|------|-----------|----------|--------------|-----------------|----------|
| `User` | `ASX_CANCEL_USER` | 0 | 1000 | 200 | Explicit/gentle |
| `Timeout` | `ASX_CANCEL_TIMEOUT` | 1 | 500 | 210 | Time-based |
| `Deadline` | `ASX_CANCEL_DEADLINE` | 1 | 500 | 210 | Time-based |
| `PollQuota` | `ASX_CANCEL_POLL_QUOTA` | 2 | 300 | 215 | Resource budget |
| `CostBudget` | `ASX_CANCEL_COST_BUDGET` | 2 | 300 | 215 | Resource budget |
| `FailFast` | `ASX_CANCEL_FAIL_FAST` | 3 | 200 | 220 | Sibling/peer |
| `RaceLost` | `ASX_CANCEL_RACE_LOST` | 3 | 200 | 220 | Sibling/peer |
| `LinkedExit` | `ASX_CANCEL_LINKED_EXIT` | 3 | 200 | 220 | Sibling/peer |
| `ParentCancelled` | `ASX_CANCEL_PARENT` | 4 | 200 | 220 | Structural |
| `ResourceUnavailable` | `ASX_CANCEL_RESOURCE` | 4 | 200 | 220 | Structural |
| `Shutdown` | `ASX_CANCEL_SHUTDOWN` | 5 | 50 | 255 | System-level |

**Severity rules** (Rust `CancelReason::strengthen`, `src/types/cancel.rs:956`; C `strengthen_replaces`, `src/core/cancel.c:49`; same order):
- Higher severity always wins in `strengthen()` operations
- Equal severity: earlier timestamp wins; on equal timestamps a present message beats none, then the lexicographically smaller message wins
- The winning reason replaces the old one whole (chains are not merged)
- Severity is monotone non-decreasing across witness transitions

**Budget combination (min-plus algebra):** Rust `Budget::combine` (`src/types/budget.rs:502`), C `asx_budget_meet` (`src/core/budget.c:49`).
- `combined_quota = min(quota1, quota2)` — tighter quota wins; the earlier finite deadline wins
- `combined_priority = max(priority1, priority2)` — higher urgency wins
- Combining never widens the cleanup allowance

### 6.5 Witness Structure

Rust fields (`src/types/cancel.rs:323`). C's witness slot holds only an id, the phase and the reason (`src/core/cancel.c:91`; `bd-9kll.3.7`).

| Field | Type | Description |
|-------|------|-------------|
| `task_id` | `asx_task_id` | The task being cancelled |
| `region_id` | `asx_region_id` | The owning region |
| `epoch` | `uint64_t` | Rust: set to 1 by the first request, unchanged by strengthening. C (on the task slot): bumped by every changed request |
| `phase` | `asx_cancel_phase` | Current protocol phase |
| `reason` | `asx_cancel_reason` | Full reason with kind, origin, timestamp, cause chain |

### 6.6 Witness Validation Rules

A transition from `prev` to `next` is valid when ALL hold (Rust `validate_transition`, `src/types/cancel.rs:382`). C has no witness-to-witness validator; the `ASX_E_WITNESS_*` codes are declared and never returned.

| Rule | Check | Error on Violation |
|------|-------|-------------------|
| Same task | `prev.task_id == next.task_id` | Rust `TaskMismatch` (C `ASX_E_WITNESS_TASK_MISMATCH`, unused) |
| Same region | `prev.region_id == next.region_id` | Rust `RegionMismatch` (C `ASX_E_WITNESS_REGION_MISMATCH`, unused) |
| Same epoch | `prev.epoch == next.epoch` | Rust `EpochMismatch` (C `ASX_E_WITNESS_EPOCH_MISMATCH`, unused) |
| Phase monotone | `next.phase.rank >= prev.phase.rank` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION`, also for an equal phase |
| Severity monotone | `next.reason.severity >= prev.reason.severity` | Rust `ReasonWeakened` (C `ASX_E_WITNESS_REASON_WEAKENED`, unused) |

### 6.7 Attribution Chain

`CancelReason` carries a recursive cause chain (Rust `src/types/cancel.rs:521`):

| Field | Type | Description |
|-------|------|-------------|
| `kind` | `asx_cancel_kind` | The cancellation kind |
| `origin_region` | `asx_region_id` | Region that initiated; for a descendant, its immediate parent |
| `origin_task` | `asx_task_id` (optional) | Task that initiated |
| `timestamp` | `asx_time` | When requested |
| `message` | `const char*` (optional) | Human-readable message (Rust `Option<String>`) |
| `cause` | `asx_cancel_reason*` (optional) | Parent cause in chain (C sets it only in `asx_region_cancel`) |
| `truncated` | `bool` | Whether chain was truncated (Rust also `truncated_at_depth`) |

**Chain limits (configurable):** Rust `max_chain_depth` default 16, `max_chain_memory` default 4096 bytes, estimated at 80 bytes per reason plus 8 per link (`src/types/cancel.rs:184`, `src/types/cancel.rs:237`); truncation in `with_cause_limited` (`src/types/cancel.rs:706`) sets `truncated`. C never truncates; its `max_cancel_chain_*` config fields are not used by cancellation (`bd-9kll.3.7`).

### 6.8 Propagation Rules

> **Superseded by `asupersync_v4_formal_semantics.md` §3.2 `CANCEL-REQUEST` (`rule.cancel.request` #1) and §5 INV-CANCEL-PROPAGATES (`inv.cancel.propagates_down` #6); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.request` and `inv.cancel.propagates_down`.**

1. A region **cancel** propagates to every task of its subtree (Rust `cancel_request`, `src/runtime/state.rs:7547`; C `asx_region_cancel`, `src/runtime/cancellation.c:421`). A plain C `asx_region_close` cancels no task
2. Propagation is parents-before-descendants, **not depth-first**. Rust: stack-based collection, then a stable sort by depth (`src/runtime/state.rs:7904`, `src/runtime/state.rs:7653`), so siblings come in reverse insertion order. C walks the same way (`asx_region_subtree_internal`, `src/runtime/lifecycle.c:593`) over `children[]`, which unlinking keeps in insertion order (`src/runtime/quiescence.c:21`); fixtures `cancel-subtree-order-001` and `cancel-subtree-order-after-child-close-001` check it (bd-e038; C walked breadth-first and swap-removed before 2026-10-10)
3. Each task takes its region's reason: ParentCancelled from the immediate parent for descendants, with the parent's reason as cause. C does not extend its witness with a chain
4. Cancel does NOT propagate to sibling regions (parent-to-child only)
5. Already-cancelled tasks: strengthening only (T6/T9/T12); completed tasks are skipped
6. Propagation is deterministic in each engine: same tree + same membership order + same trigger = same order

---

## 7. Outcome Severity Lattice

> **Superseded by `asupersync_v4_formal_semantics.md` §1.2 (`def.outcome.four_valued` #29, `def.outcome.severity_lattice` #30, `def.outcome.join_semantics` #31), §3.3 `CLOSE-COMPLETE` (#26) and §7 LAW-JOIN-ASSOC (`law.join.assoc` #42); C status: see C_REFINEMENT_MAP.md rows `def.outcome.four_valued`, `def.outcome.severity_lattice`, `def.outcome.join_semantics` (Partial), `rule.region.close_complete` (Partial), `law.join.assoc` (Partial).** Rust `Outcome` (`src/types/outcome.rs:218`) carries a payload; C `asx_outcome` (`include/asx/core/outcome.h:21`) holds only the severity, the cancel reason and panic message staying on the task record (`bd-9kll.3.14`).

### 7.1 Severity Ordering

```
Ok (0) < Err (1) < Cancelled (2) < Panicked (3)
```

### 7.2 Outcome Values

| Outcome | Enum Value | Severity | Description |
|---------|-----------|----------|-------------|
| `Ok` | `ASX_OUTCOME_OK` | 0 | Completed successfully |
| `Err` | `ASX_OUTCOME_ERR` | 1 | Encountered an error |
| `Cancelled` | `ASX_OUTCOME_CANCELLED` | 2 | Cancelled via protocol |
| `Panicked` | `ASX_OUTCOME_PANICKED` | 3 | Unrecoverable failure |

### 7.3 Join Semantics

Rust `Outcome::join` (`src/types/outcome.rs:566`); C `asx_outcome_join` (`src/core/outcome.c:15`).

```
join(a, b) = the operand with the higher severity; on a tie, a (left bias)
             Rust only: Cancelled(r1) join Cancelled(r2) = Cancelled(r1 strengthened by r2)
             when r2 is strictly more severe
```

Properties (in severity; on payloads the left bias makes join non-commutative, as Rust's doc comment says):
- **Commutative:** `join(a, b) == join(b, a)` in severity
- **Associative:** `join(join(a, b), c) == join(a, join(b, c))` in severity
- **Idempotent:** `join(a, a) == a`
- **Identity:** `Ok` — `join(Ok, x)` has the severity of `x`
- **Absorbing:** `Panicked` — `join(Panicked, x) == Panicked`
- **Equal-severity tie:** left-biased at payload level. C carries no reason, so it cannot strengthen a `Cancelled` + `Cancelled` join (`bd-9kll.3.14`)

### 7.4 Join Truth Table

| Left | Right | Result |
|------|-------|--------|
| `Ok` | `Ok` | `Ok` |
| `Ok` | `Err` | `Err` |
| `Ok` | `Cancelled` | `Cancelled` |
| `Ok` | `Panicked` | `Panicked` |
| `Err` | `Err` | `Err` |
| `Err` | `Cancelled` | `Cancelled` |
| `Err` | `Panicked` | `Panicked` |
| `Cancelled` | `Cancelled` | `Cancelled` |
| `Cancelled` | `Panicked` | `Panicked` |
| `Panicked` | `Panicked` | `Panicked` |

### 7.5 Region Outcome Computation

```
region_outcome = fold(join, default, [child_1_outcome, ..., child_n_outcome, finalizer outcomes...])
```

Rust folds each completed task's and finalizer's outcome into the region's `close_outcome` (`record_close_outcome`, `src/record/region.rs:1144`, called at `src/runtime/state.rs:8521`). With nothing folded, the default is `Cancelled(reason)` if the region was cancelled and `Ok` otherwise (`src/record/region.rs:1820`), so an empty region's outcome is not always `Ok`. The spec's form is `R[r].policy.aggregate(child_outcomes, finalizer_outcomes)`. C computes no region outcome (`src/runtime/runtime_internal.h:28`; `bd-9kll.3.4`).

---

## 8. Budget Algebra

> **Partly superseded.** `asupersync_v4_formal_semantics.md` §1.4 states the budget semiring but is marked [Explanatory] and has no rule ID; §5 INV-DEADLINE-MONOTONE (child deadlines no later than the parent's) is normative but not in the Canonical Rule Index. C status: see C_REFINEMENT_MAP.md supplementary row "§5 INV-DEADLINE-MONOTONE" (Implemented). The rest of this section is extraction from `src/types/budget.rs` at `5e60b1c4c`.

### 8.1 Budget Carrier

```
asx_budget = (deadline, poll_quota, cost_quota, priority)
```

Rust `Budget` (`src/types/budget.rs:177`): `deadline: Option<Time>`, `poll_quota: u32`, `cost_quota: Option<u64>`, `priority: u8`. C `asx_budget` (`include/asx/core/budget.h`) encodes "no deadline" as 0 and "no cost quota" as `UINT64_MAX` (`src/core/budget.c:14`).

### 8.2 Meet/Combine (Tightening)

Componentwise tightening (Rust `Budget::combine`/`meet`, `src/types/budget.rs:502`, `src/types/budget.rs:592`; C `asx_budget_meet`, `src/core/budget.c:49`):
- `deadline = min(finite a, finite b)` — earliest finite deadline wins
- `poll_quota = min(a, b)` — tighter quota wins
- `cost_quota = min(finite a, finite b)` — tighter quota wins
- `priority = max(a, b)` — the more urgent wins (the first extraction left this unspecified)

### 8.3 Identities

- `INFINITE` is the identity for meet/tightening (combining with INFINITE preserves the other). Rust: no deadline, `u32::MAX` polls, no cost quota, **priority 0** (`src/types/budget.rs:222`); C `asx_budget_infinite` (`src/core/budget.c:14`)
- `ZERO` is absorbing/tightest bound (combining with ZERO yields ZERO). Rust: deadline `Time::ZERO`, 0 polls, cost 0, **priority 255** (`src/types/budget.rs:230`). C `asx_budget_zero` uses deadline 1, because 0 means "no deadline" in C (`src/core/budget.c:23`)
- An ordinary budget (`Budget::new()`) has priority 128 and no limits (`src/types/budget.rs:252`; C `asx_budget_new`)

### 8.4 Exhaustion Predicates

| Predicate | Trigger | Behavior |
|-----------|---------|----------|
| Poll quota depletion | `poll_quota == 0` (Rust lab charges the poll before polling, `src/lab/runtime.rs:4664`) | Cancel with `PollQuota` kind; C `src/runtime/scheduler.c:755`, `src/runtime/cancellation.c:512` |
| Cost quota depletion | Rust: the remaining cost quota is exactly 0 at a checkpoint (`Cx::checkpoint_budget_exhaustion`, `src/cx/cx.rs:3112`) | Cancel with `CostBudget` kind. C also cancels at once when a charge exceeds the remainder (`asx_task_consume_cost`, `src/runtime/lifecycle.c:1222`); open bead `bd-9kll.3.10` |
| Deadline expiration | `now >= deadline` | Cancel with `Deadline` kind: the budget-deadline timer stamps it with the deadline (`src/cx/cx.rs:357`), a checkpoint with `now` (`src/cx/cx.rs:3122`); C does the same under lab dispatch (`src/runtime/scheduler.c:224`, `src/runtime/cancellation.c:512`) |

### 8.5 Consume Semantics

Consumption is **failure-atomic**:
- No underflow behavior
- Insufficient quota returns explicit failure without partial mutation
- Exhaustion paths map to explicit cancellation reasons and diagnostics

**Poll quota consume** (the first extraction's `ASX_E_BUDGET_EXHAUSTED` does not exist in C):
```
Rust Budget::consume_poll(&mut self) -> Option<u32>    // src/types/budget.rs:447
  if poll_quota > 0: old = poll_quota; poll_quota -= 1; return Some(old)
  else: return None                                    // no mutation
C asx_budget_consume_poll(asx_budget *b) -> uint32_t   // src/core/budget.c:60
  returns the old quota, or 0 (no mutation) when it is already 0
```

**Cost consume:** Rust `consume_cost(cost) -> bool` (`src/types/budget.rs:613`) and C `asx_budget_consume_cost` (`src/core/budget.c:65`) both return failure without mutation when the remainder is smaller than the charge, and success with no change when there is no cost quota.

---

## 9. MPSC Channel Semantics

> **Not covered by the v4 spec**, except that a permit taken by `reserve(&cx)` is a `SendPermit` obligation, so its commit/abort/leak follow `asupersync_v4_formal_semantics.md` §3.4 (C status: C_REFINEMENT_MAP.md rows `rule.obligation.reserve` … `rule.obligation.leak`). This section is extraction from `src/channel/mpsc.rs` at `5e60b1c4c` (`mpsc.rs:N` below means that file). C's channel semantics and their drift from Rust are recorded in `docs/CHANNEL_TIMER_SEMANTICS.md` §1; C cites below are `src/channel/mpsc.c`.

### 9.1 Channel State Model

Implicit state encoding via atomic fields (no explicit state enum; `src/channel/mpsc.rs:297`). C uses an explicit enum `asx_channel_state` (`include/asx/core/channel.h:68`).

| Implicit State | Condition | Description |
|----------------|-----------|-------------|
| **Open** | `receiver_dropped == false` AND `sender_count > 0` | Both sides active |
| **Half-Closed (Rx)** | `receiver_dropped == true` AND `sender_count > 0` | Receiver dropped or closed (`Receiver::close`, `src/channel/mpsc.rs:1698`; `Sender::close_receiver`, `src/channel/mpsc.rs:830`); senders observe disconnect |
| **Half-Closed (Tx)** | `receiver_dropped == false` AND `sender_count == 0` | All senders dropped; receiver drains |
| **Closed** | `receiver_dropped == true` AND `sender_count == 0` | Both sides gone |

`receiver_dropped` transitions `false -> true` exactly once (stores at `src/channel/mpsc.rs:830`, `src/channel/mpsc.rs:1705`, `src/channel/mpsc.rs:2130`). `sender_count` is **not** decrement-only (the first extraction said it was): `Sender::clone` increments it (`src/channel/mpsc.rs:1231`) and `WeakSender::upgrade` increments it but never raises it from 0 (`src/channel/mpsc.rs:1275`). Once it reaches 0 it stays 0. C has no sender count: `ASX_CHANNEL_SENDER_CLOSED` is terminal and a second close returns `ASX_E_INVALID_STATE` (`src/channel/mpsc.c:421`; `bd-9kll.5.4`).

### 9.2 Capacity Model

Capacity is **fixed at creation** and never changes:

```
channel(capacity) where capacity > 0  // panics on zero
used_slots = queue.len() + reserved
INVARIANT: used_slots <= capacity
```

Both queued messages AND reserved (uncommitted) permits consume capacity.

**Rust evidence:** `src/channel/mpsc.rs:349` (`used_slots`, `has_capacity`), `src/channel/mpsc.rs:557` (factory; the zero-capacity assert is at `src/channel/mpsc.rs:565`). Rust also has `unbounded_channel` = `channel(usize::MAX)` (`src/channel/mpsc.rs:599`).

**C:** capacity 0 or above `ASX_CHANNEL_MAX_CAPACITY` (64) returns `ASX_E_INVALID_ARGUMENT` instead of panicking (`src/channel/mpsc.c:383`), and a channel can only be created in an `Open` region (`src/channel/mpsc.c:387`). C has no unbounded channel. Recorded in `docs/CHANNEL_TIMER_SEMANTICS.md` §1.2 and §4.1 (`bd-9kll.5.7`).

### 9.3 Two-Phase Send Protocol

#### Phase 1: Reserve

```
reserve(&cx) -> Reserve future; Output = Result<SendPermit, SendError<()>>
```

(`src/channel/mpsc.rs:637`; `Output` at `src/channel/mpsc.rs:1175`.) Each poll checks, in order:
1. **Cancellation checkpoint:** if `cx.checkpoint().is_err()` -> `SendError::Cancelled(())` (`src/channel/mpsc.rs:1061`)
2. **Receiver alive:** if `receiver_dropped == true` -> `SendError::Disconnected(())` (`src/channel/mpsc.rs:1072`)
3. **First and capacity:** if the caller is first (no queued waiter, or its own token at the front) and `used_slots < capacity` -> `reserved += 1`, leave the queue, wake the next head if capacity remains, register the `SendPermit` obligation, return the permit (`src/channel/mpsc.rs:1081`, obligation at `src/channel/mpsc.rs:1116`)
4. **Otherwise:** register or refresh its waker in the FIFO waiter queue and return `Poll::Pending` (`src/channel/mpsc.rs:1129`). A caller that is not first is registered, not just told Pending. There is no "monotonic waiter ID": waiters are `SlabToken`s in a `VecDeque` (`src/channel/mpsc.rs:279`)

Only `reserve(&cx)` registers an obligation; `try_reserve` and `send` do not. C matches the order with two C-only codes: `ASX_E_INVALID_STATE` when the sender side is closed (`src/channel/mpsc.c:551`) before `ASX_E_DISCONNECTED` (`src/channel/mpsc.c:556`); cancellation is `ASX_E_CANCELLED` (`src/channel/mpsc.c:831`), and the obligation is registered after the slot is claimed (`src/channel/mpsc.c:837`). See `docs/CHANNEL_TIMER_SEMANTICS.md` §1.3.

#### Phase 2a: Send (Commit)

```
SendPermit::send(self, value: T) -> Outcome<(), SendError<T>>   // Err(Disconnected(value)) if the receiver is gone
SendPermit::try_send(self, value: T)                            // fails only if receiver dropped post-reserve
```

`send` is **not** infallible (the first extraction said it was; `src/channel/mpsc.rs:1565`). On success: marks permit consumed, decrements `reserved`, pushes value to back of queue, wakes receiver, and commits the permit's obligation; on disconnect the obligation is aborted with reason Error.

**Rust evidence:** `src/channel/mpsc.rs:1565`–`src/channel/mpsc.rs:1629`. C: `asx_send_permit_send` (`src/channel/mpsc.c:638`).

#### Phase 2b: Abort (Rollback)

```
SendPermit::abort(self)
```

Marks permit consumed, decrements `reserved`, aborts the obligation with reason Explicit, and wakes the head of the waiter queue (the head is woken, not removed).

**Rust evidence:** `src/channel/mpsc.rs:1633`. C: `asx_send_permit_abort` (`src/channel/mpsc.c:699`).

#### RAII Drop Safety

If `SendPermit` dropped without `send()` or `abort()`, the `Drop` impl runs abort logic and aborts the obligation with reason Cancel:

```
Drop for SendPermit: if !self.sent { reserved -= 1; wake head; abort obligation (Cancel) }
```

**Rust evidence:** `src/channel/mpsc.rs:1663`. C has no destructors: an unsent C permit holds its slot until it is sent or aborted; the conformance interpreter emulates the drop at task end (`src/conformance/interpreter.c`). Recorded in `docs/CHANNEL_TIMER_SEMANTICS.md` §1.3 (`bd-9kll.5.3`).

### 9.4 Backpressure and Waiter Queue

When channel is full (`used_slots >= capacity`):
1. `reserve()` returns `Poll::Pending`
2. Sender registered in the waiter queue: a `TokenSlab<Arc<RegisteredWaker>>` plus a `VecDeque<SlabToken>` (`src/channel/mpsc.rs:279`); a waiter keeps its position across polls
3. Strict FIFO by arrival (no monotonic ID)

**Release triggers:**
- Receiver consumes a message (`queue.pop_front()`, `src/channel/mpsc.rs:1801`; `try_recv` likewise)
- Permit dropped/aborted (decrements `reserved`)
- These wake the queue head; the cascade happens when that head claims a slot and capacity remains (`src/channel/mpsc.rs:1099`)
- Also: a cancelled or dropped `Reserve` passes the wake on (`src/channel/mpsc.rs:960`), and receiver close/drop wakes every sender (`src/channel/mpsc.rs:1698`, `src/channel/mpsc.rs:2147`)

**FIFO enforcement:** `try_reserve()` returns `Full` while a live waiter is queued, even if capacity is available (also `try_send` and eviction). Prevents queue-jumping. Rust counts every queued waiter until its `Reserve` polls or drops; stale tokens are pruned only from the front (`src/channel/mpsc.rs:363`).

**Rust evidence:** `src/channel/mpsc.rs:374` (`has_waiting_sender`), `src/channel/mpsc.rs:732` (`try_reserve`), `src/channel/mpsc.rs:1129` (waiter registration)

**C drift (not recorded elsewhere):** C's FIFO check counts only waiters whose task is Created or Running (`asx_wait_queue_live_ahead`, `src/sync/wait_queue.c:321`, used at `src/channel/mpsc.c:564`), so `try_reserve`/`reserve` can take a slot while a cancel-requested producer is queued ahead; Rust counts that producer. C's settle also wakes a cancel-pending head and the next live waiter together (`src/sync/wait_queue.c:286`), where Rust wakes only the head. Found by reading the code; not exercised by any fixture.

### 9.5 Eviction Mode

```
send_evict_oldest(value: T) -> Result<Option<T>, SendError<T>>
```

| Condition | Result |
|-----------|--------|
| Capacity available and no live waiter queued | `Ok(None)` — normal send |
| A live waiter is queued | `Err(Full(value))` — nothing evicted |
| Queue has committed entries | `Ok(Some(evicted))` — oldest committed entry evicted |
| All capacity is reserved (no committed entries) | `Err(Full(value))` — cannot evict reserved slots |
| Receiver dropped | `Err(Disconnected(value))` (checked first) |

**Rust evidence:** `src/channel/mpsc.rs:860`–`src/channel/mpsc.rs:936`. C has no eviction (`docs/CHANNEL_TIMER_SEMANTICS.md` §1.5, `bd-9kll.5.7`).

### 9.6 Receive Semantics

```
recv(cx) -> Poll<Result<T, RecvError>>
try_recv() -> Result<T, RecvError>
```

Order (`poll_recv`, `src/channel/mpsc.rs:1786`):
1. **Cancellation checkpoint:** if `cx.checkpoint().is_err()` -> `RecvError::Cancelled` (`src/channel/mpsc.rs:1786`)
2. **Queue non-empty:** `queue.pop_front()` -> `Ok(value)` (FIFO, `src/channel/mpsc.rs:1801`)
3. **Queue empty AND (all senders dropped OR receiver closed):** `RecvError::Disconnected` (`src/channel/mpsc.rs:1813`)
4. **Otherwise:** register waker, return `Poll::Pending`

`try_recv` (`src/channel/mpsc.rs:1956`) has no checkpoint and returns `Empty` or `Disconnected`. C: `asx_channel_recv` (`src/channel/mpsc.c:861`) and `channel_recv_impl` (`src/channel/mpsc.c:738`); C's `try_recv` returns `ASX_E_WOULD_BLOCK` for empty (`src/channel/mpsc.c:787`), and C keeps waiting when the sender is closed but permits are outstanding, a state Rust cannot reach (`bd-9kll.5.4`).

**Cancel safety:** Cancelled receive does NOT consume a message (the checkpoint runs before the pop). Message remains for next receive.

**Drain-after-sender-drop:** Receiver can drain remaining messages after all senders drop. Returns `Ok(value)` until queue empty, then `Disconnected`.

### 9.7 Error Taxonomy

Rust: `SendError` (`src/channel/mpsc.rs:111`), `RecvError` (`src/channel/mpsc.rs:182`), and `CheckedSendError` (`src/channel/mpsc.rs:136`, admission failures; no C counterpart). C errors never carry the value back. C status mapping: `docs/CHANNEL_TIMER_SEMANTICS.md` §1.7 and `docs/CANONICAL_VOCABULARY_V2.md` §5.

#### Send Errors

| Error | Condition | Carries Value |
|-------|-----------|---------------|
| `SendError::Disconnected(T)` | Receiver dropped or closed (C `ASX_E_DISCONNECTED`) | Yes |
| `SendError::Cancelled(T)` | Cancellation checkpoint (C `ASX_E_CANCELLED`) | Yes |
| `SendError::Full(T)` | Channel full or a live waiter queued (try_reserve/try_send, eviction); never from the waiting futures (C `ASX_E_CHANNEL_FULL`) | Yes |

#### Receive Errors

| Error | Condition |
|-------|-----------|
| `RecvError::Disconnected` | Queue empty AND (all senders dropped OR receiver closed) (C `ASX_E_DISCONNECTED`) |
| `RecvError::Cancelled` | Cancellation checkpoint (C `ASX_E_CANCELLED`) |
| `RecvError::Empty` | Queue empty (try_recv only), senders alive (C `ASX_E_WOULD_BLOCK`; the conformance interpreter maps it to `ASX_E_CHANNEL_EMPTY`) |

### 9.8 Cancellation Interaction

| Operation | Cancel Effect |
|-----------|--------------|
| `reserve()` poll | Returns `Cancelled`, no capacity consumed, waiter removed (C `src/channel/mpsc.c:831`) |
| `recv()` poll | Returns `Cancelled`, no message consumed |
| `SendPermit::send()` | Not checked (already committed) |
| `SendPermit::abort()` | Not checked (already releasing) |

Waiter cleanup on cancel: the `Reserve` future's drop removes its `SlabToken` and passes the wake on only if it held a queue position, the receiver is alive and capacity is free (`src/channel/mpsc.rs:965`). C: `asx_channel_wait_cancel` (`src/channel/mpsc.c:795`).

### 9.9 Obligation Integration (Session Layer)

Rust: `src/channel/session.rs` (`TrackedSender`/`TrackedPermit` at `src/channel/session.rs:220` and `src/channel/session.rs:405`). C has no tracked permits or proofs: C's `src/channel/session.c` is a different construct (`bd-9kll.5.9`); the base-permit obligation is implemented (`bd-9kll.5.3`).

| Component | Base Channel | Session-Tracked |
|-----------|-------------|-----------------|
| Sender | `Sender<T>` | `TrackedSender<T>` |
| Permit | `SendPermit<T>` (carries an optional runtime obligation, `src/channel/mpsc.rs:1460`) | `TrackedPermit<T>` (contains `ObligationToken<SendPermit>`) |
| Send result | `Outcome<(), SendError<T>>` | `Result<CommittedProof<SendPermit>, SendError<T>>`; on error the token is aborted (`src/channel/session.rs:416`) |
| Abort result | `()` | `AbortedProof<SendPermit>` |
| Leak behavior | Slot freed and the runtime obligation aborted with reason Cancel | **PANIC** `[ASUP-E101] OBLIGATION TOKEN LEAKED: send_permit token 'TrackedPermit(mpsc)' was dropped without being consumed. …` (`src/obligation/graded.rs:999`), except while the thread is already unwinding |

`#[must_use]` on `TrackedPermit` (`src/channel/session.rs:404`) and on `SendPermit` — compiler warning if not consumed.

### 9.10 Close/Drain Semantics

**Receiver Drop (Channel Close)** (`src/channel/mpsc.rs:2129`), in this order:
1. Drains the sender wakers
2. Sets `receiver_dropped = true` (monotone)
3. Takes the recv waker (dropped, not woken)
4. Takes the queue via `mem::take()` — pending messages DROPPED (not delivered)
5. After unlock: signals closed, wakes ALL waiting senders (they observe disconnect on next poll), then drops the items outside the lock

`Receiver::close()` (`src/channel/mpsc.rs:1698`) is different: it sets `receiver_dropped` and wakes senders but keeps the queue receivable. C's `asx_channel_close_receiver` is the drop: it discards the queue and wakes both wait queues, and a second close returns `ASX_E_INVALID_STATE` (`src/channel/mpsc.c:469`; `bd-9kll.5.4`). `asx_channel_seal` is `Receiver::close()`: it wakes the parked senders and keeps the queue receivable (`src/channel/mpsc.c:504`; `bd-g652`).

**Last Sender Drop** (`src/channel/mpsc.rs:1241`):
1. Decrements `sender_count` to 0
2. Wakes receiver (will return `Disconnected` after draining remaining queue)

C's `asx_channel_close_sender` (`src/channel/mpsc.c:421`) also wakes the reserve waiters.

### 9.11 Ordering Guarantees

| Dimension | Ordering | Mechanism |
|-----------|----------|-----------|
| Message delivery | FIFO | `VecDeque`: `push_back()` / `pop_front()` (`src/channel/mpsc.rs:276`); C ring buffer |
| Waiter scheduling | FIFO | Arrival order in the token queue (no monotonic ID) |
| Cascade wake | FIFO | Head of waiter queue woken first (`src/channel/mpsc.rs:386`) |
| try_reserve fairness | Strict FIFO | Returns `Full` while a live waiter is queued (C counts only Created/Running waiters, §9.4) |

---

## 10. Timer Wheel Semantics

> **Not covered by the v4 spec** beyond §3.6 `TICK` (virtual time advance; C status: C_REFINEMENT_MAP.md supplementary row "§3.6 TICK", Variant). This section is extraction from `src/time/wheel.rs` at `5e60b1c4c`. **C has no hierarchical wheel.** It has three separate mechanisms: a flat fixed table of `ASX_MAX_TIMERS` slots (default 128) in `src/time/timer_wheel.c`, whose header calls it a walking skeleton (`src/time/timer_wheel.c:4`) and which only the hedge combinator uses; a per-task timer min-heap in the scheduler keyed by (exact deadline, arm sequence) (`src/runtime/scheduler.c:101`), at most one entry per task; and, under lab dispatch, firing in (1 ms tick, registration) order (`src/runtime/scheduler.c:266`). C's timer semantics and drift are recorded in `docs/CHANNEL_TIMER_SEMANTICS.md` §2 (beads `bd-9kll.7.1`, `bd-9kll.7.4`). No v2 fixture fires a timer beyond the first 1 ms tick, so levels, cascades, overflow, coalescing and the duration limit have no Rust-parity coverage.

### 10.1 4-Level Hierarchical Wheel

Constants: `src/time/wheel.rs:43` (`LEVEL_COUNT = 4`, `SLOTS_PER_LEVEL = 256`, level-0 resolution 1 ms); each level's range is its resolution × 256 (`src/time/wheel.rs:316`).

| Level | Slots | Resolution | Range |
|-------|-------|------------|-------|
| 0 | 256 | 1 ms | 256 ms |
| 1 | 256 | 256 ms | 65.536 s |
| 2 | 256 | 65.536 s | 16,777.216 s (~4.66 h; the first extraction had ~16.78 min) |
| 3 | 256 | 16,777.216 s (~4.66 h) | 4,294,967.296 s (~49.7 days; the first extraction had ~37.2 hours, from a stale Rust module comment) |
| Overflow | `BinaryHeap<OverflowEntry>` (`src/time/wheel.rs:393`) | N/A | `delta >= min(max_wheel_duration, physical range)`, 24 h by default (`src/time/wheel.rs:718`, `src/time/wheel.rs:1091`) |

**Slot assignment:** per level, `tick = deadline_ns / level.resolution_ns`, `slot = tick % 256`; a level-0 tick at or before the current tick goes to the `ready` list (`src/time/wheel.rs:728`–`src/time/wheel.rs:747`).

**Bitmap occupation:** Each level has 4 x u64 bitmap words for O(1) skip of empty slots (`src/time/wheel.rs:296`).

**Rust evidence:** `src/time/wheel.rs:43` (constants), `src/time/wheel.rs:296` (`WheelLevel`), `src/time/wheel.rs:388` (`TimerWheel`)

### 10.2 Timer Entry Structure

```
TimerEntry {
    deadline: Time,      // When to fire
    waker: Waker,        // What to wake
    id: u64,             // Unique timer ID
    generation: u64,     // Use-after-cancel prevention
}
```

(`src/time/wheel.rs:244`.) Active timers are tracked in a `slab::Slab<u64>` holding each timer's generation (`TimerActivityMap`, `src/time/wheel.rs:258`, field at `src/time/wheel.rs:396`); the timer id is the slab index. It is not a `HashMap` as the first extraction said. C: `asx_timer_slot {deadline, waker_data, insertion_seq, generation, alive}` (`src/time/timer_wheel.c:26`).

### 10.3 Insert Semantics

```
try_register(deadline, waker) -> Result<TimerHandle, TimerDurationExceeded>   // src/time/wheel.rs:530
register(deadline, waker) -> TimerHandle                                      // src/time/wheel.rs:515, clamps
```

`try_register` has no caller in `src/` outside the wheel's tests. The runtime path, `TimerDriver::register` (`src/time/driver.rs`), calls the clamping `register`; an over-long `Sleep` fires early at the horizon and registers again.

**Placement logic:**
1. If `deadline <= current_time` (current time rounded down to the 1 ms tick, `src/time/wheel.rs:493`): pushed to `ready` queue (immediately expired; `src/time/wheel.rs:711`)
2. If `delta >= max_range` (min of `max_wheel_duration` and the physical range): pushed to overflow `BinaryHeap` (`src/time/wheel.rs:718`)
3. Otherwise: placed in appropriate level slot based on delta magnitude (`src/time/wheel.rs:728`), with a fallback push to overflow (`src/time/wheel.rs:750`)

**Duplicate handling:** No deduplication. Multiple timers with same deadline stored in insertion order in same slot Vec (`src/time/wheel.rs:744`); see §10.7 for cascades.

**Validation:** `try_register` returns `TimerDurationExceeded` if the duration is strictly greater than the configured max (default 7 days; `src/time/wheel.rs:536`). C: `asx_timer_register` rejects past the same 7 days (24 h before 2026-10-10) with `ASX_E_TIMER_DURATION_EXCEEDED` and has no clamping variant (`src/time/timer_wheel.c:145`); C task sleeps (the scheduler heap) have no limit at all; a full C table returns `ASX_E_RESOURCE_EXHAUSTED` (`src/time/timer_wheel.c:162`), where Rust's slab has no count limit.

**Generation tracking:** Each registration gets a generation (`wrapping_add(1)`, `src/time/wheel.rs:551`) and an id that is the slab index, reused after removal (`src/time/wheel.rs:554`). Only the generation wraps. C: a per-slot `uint32` generation that skips 0, plus a global u64 `insertion_seq`.

**Configuration** (`src/time/wheel.rs:98`):

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `max_wheel_duration` | 24 hours | Max for wheel placement (beyond = overflow heap); also capped at the physical range |
| `max_timer_duration` | 7 days | Max accepted by `try_register()` (C: one limit, the same 7 days, settable) |

### 10.4 Fire Semantics

```
collect_expired(now) -> WakerBatch
```

(`src/time/wheel.rs:699`.) Process:
1. **Advance:** `synchronize`, then `advance_to(target_tick)` (`src/time/wheel.rs:756`): with an empty wheel it jumps to the target; otherwise a bitmap skip of empty slots and `tick_level0` per tick (`src/time/wheel.rs:849`)
2. **Cascade:** When Level N cursor wraps to 0, advance Level N+1 cursor and re-insert entries from higher level into lower levels (`src/time/wheel.rs:865`). The overflow heap is refilled into the wheel on every tick (`refill_overflow`, `src/time/wheel.rs:903`)
3. **Generation check:** Dead entries (generation mismatch) silently dropped during cascade, refill and drain
4. **Collect:** All live entries with `deadline <= now` added to waker batch (`drain_ready`, `src/time/wheel.rs:921`); later ones go back into the wheel. Fired entries leave `active`, and the wheel is purged when `active` becomes empty
5. **Coalescing** (optional): when the current window holds a group, due timers in it are **held until the window end**; a coalesced timer never fires before its deadline (the first extraction said timers were "promoted" early)

**Fire criterion:** `entry.deadline <= now` (inclusive — fires at or after deadline; `src/time/wheel.rs:948`), subject to the coalescing hold. C: `asx_timer_collect_expired` (`src/time/timer_wheel.c:202`) scans all live slots with `deadline <= now` (`src/time/timer_wheel.c:222`), sorts them and caps the batch at `max_wakers` (Rust's batch has no cap); the scheduler heap fires tasks with `wake_at <= now` (`src/runtime/scheduler.c:339`).

### 10.5 Cancel Semantics (Lazy Deletion)

```
cancel(handle) -> bool     // src/time/wheel.rs:577
```

1. Check `active` slab: if `active[handle.id] == handle.generation`, remove entry -> return `true`
2. Generation mismatch or missing: return `false`
3. **No immediate physical removal** from wheel slots — the entry is skipped at fire time via `is_live()` (`src/time/wheel.rs:1071`); but once cancellations since the last compaction exceed 2 × live + 1024, `compact_inactive_storage()` removes cancelled entries (`src/time/wheel.rs:588`, `src/time/wheel.rs:1116`)
4. When all timers cancelled (`active` empty): `purge_inactive_storage()` clears all slot vectors and bitmaps (`src/time/wheel.rs:585`, `src/time/wheel.rs:1100`)

C marks the slot dead at once and reuses it later (`asx_timer_cancel`, `src/time/timer_wheel.c:174`).

### 10.6 Generation-Safe Handle Contract

```
TimerHandle { id: u64, generation: u64 }     // src/time/wheel.rs:224
```

- `id` is a slab index (reused); only `generation` wraps (`wrapping_add(1)`)
- Cancel requires exact `(id, generation)` match
- After cancel, same `id` may be reused with different `generation` — old handles cannot cancel new timer
- Safe across u64 wrap: the slab holds one generation per id, and a stale handle fails the generation-equality check (`src/time/wheel.rs:1071`); the first extraction's "HashMap stores both entries" mechanism does not exist

C's handle is `{uint32 slot, uint32 generation}` (`include/asx/time/timer_wheel.h:48`), with the generation per slot, so a stale handle could match again after 2^32 re-arms of the same slot (not recorded elsewhere; no practical test).

### 10.7 Timer Ordering Within Deadline

| Condition | Ordering | Mechanism |
|-----------|----------|-----------|
| Different deadlines | Tick order (earliest 1 ms tick first) | Wheel cursor advancement. Within one slot, push order wins even when the sub-millisecond deadlines differ |
| Same deadline, same slot | Insertion order | `Vec::push()` appends; iteration preserves. **Not across levels:** a timer registered ≥ 256 ms ahead sits in level 1 and is appended after later level-0 registrations when it cascades, so it fires after them |
| Overflow heap | Deadline, then wrap-aware generation, then id (`src/time/wheel.rs:276`) | Only decides the order of promotion back into the wheel |

C native mode and `src/time/timer_wheel.c` sort by exact deadline then insertion (`src/runtime/scheduler.c:101`, `src/time/timer_wheel.c:230`); C lab mode by (1 ms tick, registration) (`src/runtime/scheduler.c:266`). Recorded in `bd-9kll.7.1` and `docs/CHANNEL_TIMER_SEMANTICS.md` §2.8; the cascade exception above is not recorded there.

### 10.8 Coalescing

Optional window-based coalescing (`CoalescingConfig`, defaults off, 1 ms window, group size 1; `src/time/wheel.rs:157`):
- When the number of live timers with deadlines in the current window, counted across ready, all levels and overflow, reaches `min_group_size.max(1)` (`window_holds_group`, `src/time/wheel.rs:1012`), due timers in that window fire together at the window end
- Deterministic for fixed inputs and threshold config
- C has no coalescing (`docs/CHANNEL_TIMER_SEMANTICS.md` §2.4)

---

## 11. Deterministic Scheduler Semantics

> **Partly superseded by `asupersync_v4_formal_semantics.md` §1.11 (lanes, [Implementation]), §2.4 (`SchedulerState`, normative: a task is in at most one lane, lanes have strict priority, timed ties broken deterministically), §3.0 `ENQUEUE` / `SCHEDULE-STEP` and §5 INV-SCHED-LANES (none in the Canonical Rule Index), with the "Scheduler fairness" `pick_next` pseudo-code and bounded-fairness lemma (explanatory only) and §6 PROG-CANCEL (`prog.cancel.drains` #9); C status: see C_REFINEMENT_MAP.md supplementary rows "§1.11 lanes, §3.0 ENQUEUE, §5 INV-SCHED-LANES" (Variant) and "§3.0 SCHEDULE-STEP" (Partial), and row `prog.cancel.drains` (Partial).** This section describes Rust's production multi-worker `ThreeLaneScheduler` (`src/runtime/scheduler/three_lane.rs`, "TL" below). **C does not port it.** C ports the single-worker `LabScheduler` of Rust's lab runtime (`src/lab/runtime.rs:6064`), which the fixtures capture: `src/runtime/lab_dispatch.c` has a cancel lane and a ready lane, the cancel-streak limit 16, one xorshift64 draw per step and the RNG tie-break, and no timed lane. C's round-robin `asx_scheduler_run` (`src/runtime/scheduler.c:1295`) has no lanes. C has no governor, work stealing, fairness certificate, adaptive streak or spin/yield/park; `docs/CHANNEL_TIMER_SEMANTICS.md` §3 records these as not implemented. Two Rust facts the spec text does not match: no non-test Rust code at `5e60b1c4c` feeds the timed lane (no caller of `inject_timed`, `schedule_local_timed` or the lab's `schedule_timed` outside tests was found), and the cancel-streak fallback contradicts §2.4's strict lane priority.

### 11.1 Three-Lane Architecture

| Lane | Priority | Queue Type | Ordering |
|------|----------|------------|----------|
| **Cancel** | Highest (until the cancel streak reaches its limit) | `GlobalFifoQueue` (global; wraps crossbeam `SegQueue` on native targets) + `BinaryHeap` (local) | Global queue: plain FIFO (priority carried, not ordered). Local heap: priority, then generation, then task id; dispatch picks `rng % n` among the top-priority group |
| **Timed** | Middle | `Mutex<TimedQueue>` (global) + `BinaryHeap` (local) | EDF, then wrap-aware generation, then task id; RNG pick among equal deadlines. Never fed outside tests at `5e60b1c4c` |
| **Ready** | Lowest | `GlobalFifoQueue` (global) + `BinaryHeap` (local), plus `local_ready`, `fast_queue` and a LIFO slot | As cancel |

Rust evidence: `src/runtime/scheduler/global_injector.rs:167`, `src/runtime/scheduler/priority.rs:28` and `src/runtime/scheduler/priority.rs:61` (orderings). C lab dispatch: cancel and ready lanes only, arrays in push (= generation) order (`src/runtime/lab_dispatch.c:39`). `src/runtime/parallel.c` orders cancel > ready > timed, its "timed" meaning parked until woken (`bd-9kll.4.7`).

### 11.2 Governor Suggestions

Production only, and **off by default** (`ThreeLaneScheduler::new` passes `enable_governor = false`, TL `src/runtime/scheduler/three_lane.rs:2049`), so `NoPreference` is what runs. The lab and C have no governor. The `pick_next` pseudo-code in the v4 spec (§3.0, explanatory) states the same table.

| Suggestion | Lane Order | Effect |
|------------|------------|--------|
| `NoPreference` (default) | Cancel > Timed > Ready | Standard |
| `MeetDeadlines` | Timed > Cancel > Ready | Prioritize deadline tasks (while the timed dispatch streak is below 6) |
| `DrainObligations` | Cancel > Timed > Ready | Doubled cancel-streak limit |
| `DrainRegions` | Cancel > Timed > Ready | Doubled cancel-streak limit |

### 11.3 Scheduler Loop (6-Phase)

Rust's `next_task` (TL) now has more steps than six; the shape below is kept, with corrections:

```
run_loop():
  while !shutdown:
    Pre-phases: handle-cancel and deferred-cancel drains
    Phase 0: Process expired timers (fires wakers -> injects tasks)          (src/runtime/scheduler/three_lane.rs:6914)
    Phase 0.5-0.7: spawn admissions, local spawns, region commands
    Phase 1: Global queues (cancel/timed per governor order)
    Phase 2: Local PriorityScheduler (cancel/timed lanes)
    Phase 3: Fast ready paths (local_ready, fast_queue, global ready; mutex-protected, not lock-free)
    Phase 3b: Local ready with RNG hint
    Phase 4: Work stealing (ready work only)
    Phase 5: Fallback cancel (when the streak reached the limit and no timed, ready or steal work exists)
    Phase 6: Backoff/Park (spin -> yield -> park with timeout)
```

C's lab step follows `LabRuntime::step_inner` instead: drain commands, one RNG draw (`src/lab/runtime.rs:4499`), fire due timers, pick (C `src/runtime/scheduler.c:1186`, `src/runtime/lab_dispatch.c:380`).

### 11.4 Entry Ordering

#### Cancel/Ready Lanes (SchedulerEntry)
```
Compare (src/runtime/scheduler/priority.rs:28):
  1. Higher priority first (u8, higher value = more important)
  2. Earlier generation first (lower number = earlier insertion; plain comparison)
  3. Lower task id first
```

#### Timed Lane (TimedEntry)
```
Compare (src/runtime/scheduler/priority.rs:61):
  1. Earlier deadline first
  2. Earlier generation first (wrap-aware comparison)
  3. Lower task id first
```

Heap order is not dispatch order: every dispatch pop is RNG-hinted (§11.6). C lab entries are `{slot, task_gen, priority, gen}` (`src/runtime/lab_dispatch.c:43`); generations are unique, so no task-id key is needed.

### 11.5 Generation Counter (FIFO Guarantee)

Each `PriorityScheduler` maintains monotone `next_generation: u64` (`src/runtime/scheduler/priority.rs:329`); every insertion increments and records it, and the global `TimedQueue` has its own. This orders the heaps, but it does **not** make dispatch FIFO among equal priorities, because the dispatch pick is `rng % n` (§11.6). C lab dispatch uses one counter for both lanes.

### 11.6 RNG-Based Deterministic Tie-Breaking

For **every** RNG-hinted pop (cancel, timed and ready lanes), when multiple entries share the top priority (or deadline):
1. Collect the entries with that priority from the heap, at most the scratch capacity (`clamp(capacity, 32, 256)`; 256 for lab workers)
2. Compute `idx = rng_hint % count` (`tie_break_index`, `src/runtime/scheduler/priority.rs:417`)
3. Select entry at `idx`, push remaining back

RNG seeding: production workers use `DetRng::new(worker_id as u64)` (`src/runtime/scheduler/three_lane.rs:2365`). The lab uses one `DetRng` from the config seed, drawn once per step and shared by every pop in that step (`src/lab/runtime.rs:4499`). `DetRng` is xorshift64; seed 0 becomes 1 and five degenerate seeds are remapped (`src/util/det_rng.rs`). C matches the lab: seed remap and xorshift64 (`src/runtime/lab_dispatch.c:210`), group of at most 256, `r % n`, one draw per step (`src/runtime/lab_dispatch.c:344`). Fixtures `lab-dispatch-tie-break-001` and `scheduler-yield-interleave-001` check it.

### 11.7 Work Stealing

The first extraction's pseudo-code matches none of Rust's three stealing paths:

| Path | Where | Behaviour |
|------|-------|-----------|
| `steal_task` | `src/runtime/scheduler/stealing.rs:16` | Power of two choices (two random picks, longer queue first), then a circular scan from a third random start. Used only by the legacy `worker.rs` |
| `ThreeLaneWorker::try_steal` | TL | RNG-start circular scans over fast stealers (preferred cohort, then remote), then a batch heap steal; ready work only; not deterministic under concurrency |
| Lab `steal_for_worker` | `src/lab/runtime.rs:6375` | `start = r % n`, circular, skipping the thief; deterministic |

C has none: the C lab runs one worker (Rust's lab default is also one worker). `src/runtime/parallel.c` moves tasks with a round-robin cursor and no RNG.

### 11.8 Cancel-Streak Fairness

Limits consecutive cancel-lane dispatches:
- **Base limit:** 16 (configurable; TL `DEFAULT_CANCEL_STREAK_LIMIT`, `src/runtime/scheduler/three_lane.rs:166`; lab `DEFAULT_LAB_CANCEL_STREAK_LIMIT`, `src/lab/runtime.rs:6051`; C `src/runtime/lab_dispatch.c:41`). Production also has an adaptive mode, off by default
- **Doubled under:** `DrainObligations` or `DrainRegions` governor suggestions (production only; the lab never doubles)
- When the streak **reaches** the limit (`cancel_streak < effective_limit` gates the cancel lane, `src/runtime/scheduler/three_lane.rs:6969`): timed, ready and steal are tried; if none has work, one cancel is dispatched with the streak set to 1 (fallback, `src/runtime/scheduler/three_lane.rs:7086`); with no cancel work either, the streak becomes 0
- Streak resets to 0 on park and on every timed or ready dispatch

Lab sequence (Rust `pop_for_worker`, `src/lab/runtime.rs:6324`; C `asx_lab_pick`, `src/runtime/lab_dispatch.c:380`): cancel while streak < 16 → due timed → ready → fallback cancel (streak = 1) → none (streak = 0). Prevents cancellation storms from starving normal tasks. No Rust-captured fixture reaches the limit (the longest cancel run in the corpus is 10, in `sync-mutex-cancelled-front-baton-001`), and no C test crosses 16 (C_REFINEMENT_MAP row `prog.cancel.drains` notes the missing bounded-fairness test).

### 11.9 Fairness Certificate

Renamed `PreemptionFairnessCertificate` (`src/runtime/scheduler/three_lane.rs:4624`), per worker, now with 16 fields:

```
PreemptionFairnessCertificate {
    base_limit, effective_limit, observed_max_cancel_streak: usize,
    cancel_dispatches, timed_dispatches, ready_dispatches: u64,
    fairness_yields: u64,
    observed_max_ready_stall_steps, observed_max_timed_stall_steps: usize,
    ready_priority_inversions: u64, max_ready_priority_inversion_gap: u8,
    fallback_cancel_dispatches: u64,
    base_limit_exceedances, effective_limit_exceedances: u64,
    adaptive_enabled: bool, adaptive_current_limit: usize,
}
```

**Witness hash:** `witness_hash` (`src/runtime/scheduler/three_lane.rs:4709`) hashes all 16 fields to a deterministic u64. Same fields -> same hash. It is distinct from the lab's `ScheduleCertificate` (`src/runtime/scheduler/priority.rs:1276`), whose hash the fixtures record as `schedule.certificate_hash`. C computes neither; the conformance comparator checks the dispatch order, not the certificate hash (`tools/conformance/runner.c:260`; beads `bd-9kll.4.2`, `bd-9kll.4.8`).

### 11.10 Backoff/Park

Production only (TL constants at `src/runtime/scheduler/three_lane.rs:195`):

| Phase | Iterations | Action |
|-------|------------|--------|
| Spin | 8 | One `spin_loop()` per iteration, re-checking queues |
| Yield | 2 | Thread yield (OS scheduler cooperative) |
| Park | until woken | Park only if `enable_parking` (default true). Timeout = min(timer-driver deadline, local timed, global timed); a follower uses only the local deadline; an already-due deadline parks 1 ns; no deadline parks without timeout (`src/runtime/scheduler/three_lane.rs:5814`). Before backoff, the I/O leader blocks in the reactor for up to 250 ms |

C has none: native idle drains wakers, jumps the virtual clock or blocks for at most 1000 ms (`src/runtime/scheduler.c:937`); the lab auto-advances the clock.

### 11.11 Queue Data Structures

| Structure | Location | Purpose |
|-----------|----------|---------|
| `BinaryHeap<SchedulerEntry>` | Local cancel/ready | Priority + generation + task id ordering |
| `BinaryHeap<TimedEntry>` | Local timed | EDF + generation + task id ordering |
| `GlobalFifoQueue<PriorityTask>` (wraps `SegQueue` natively, `Mutex<VecDeque>` on wasm) | Global cancel/ready | Unbounded FIFO injection |
| `Mutex<TimedQueue>` | Global timed | EDF heap with generation counter |
| `LocalQueue` (`Mutex` over a `SmallVec<[TaskId; 32]>`, `src/runtime/scheduler/local_queue.rs:72`) | Local fast ready (`fast_queue`) | LIFO push/pop (owner), FIFO steal (thief). The first extraction named `IntrusiveStack` here; that type (`src/runtime/scheduler/intrusive.rs:390`) is not used by the scheduler |
| `ScheduledSet` (dense tag vector with a `DetHashSet` overflow); the lab uses a seed-hashed `DetHashSet` | Membership tracking | O(1) "is scheduled?" check |

C lab lanes are static arrays of `8 * ASX_MAX_TASKS` entries; overflow fails the step with `ASX_E_RESOURCE_EXHAUSTED` (`src/runtime/lab_dispatch.c:27`).

---

## 12. Cross-Domain Interactions

> **Partly superseded by `asupersync_v4_formal_semantics.md` §3.4.7 (cancellation does not resolve obligations; a cancelled holder that ends holding one leaks it) and §3.6 `TICK`; C status: see C_REFINEMENT_MAP.md rows `inv.obligation.no_leak`, `rule.obligation.leak` and the supplementary row "§3.6 TICK".** The rest is extraction at `5e60b1c4c`; C channel/timer interactions are recorded in `docs/CHANNEL_TIMER_SEMANTICS.md` §4.

### 12.1 Channel <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled while reserve pending | `Reserve` future polls `checkpoint()` -> `SendError::Cancelled` (`src/channel/mpsc.rs:1061`). Waiter removed. No capacity consumed. The pending reserve is woken by the cancellation itself. C: `src/channel/mpsc.c:831` |
| Task cancelled while recv pending | `poll_recv()` polls `checkpoint()` -> `RecvError::Cancelled`. No message consumed. Fixture `mpsc-recv-cancel-first-001` (empty channel, so preservation itself is not tested) |
| Region closing with channel open | Channel continues operating; no channel state refers to regions. But a permit from `reserve(&cx)` holds a `SendPermit` obligation in `cx`'s region (`src/channel/mpsc.rs:1116`), which blocks or leaks at that region's close like any obligation. C also requires an `Open` region to create a channel (`src/channel/mpsc.c:387`) |
| Obligation leak on permit drop | `TrackedPermit` panics if dropped without send/abort. Base `SendPermit` aborts the slot and its runtime obligation with reason Cancel (not silently; `src/channel/mpsc.rs:1663`). C has no drop (§9.3) |

### 12.2 Timer <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled with pending timer | A `Sleep` under a task context completes early when its task has a cancel whose kind is not Timeout or Deadline and an unmasked checkpoint reports it; it then cancels its registration (also on drop). C: `sleep_observes_cancel` (`src/time/sleep.c:49`) and `asx_task_cancel_timer` (`src/runtime/scheduler.c:586`) |
| Timer fires for cancelled task | For a Timeout or Deadline cancel the sleep runs to its deadline (fixture `budget-deadline-sleep-checkpoint-001`) and the cancel is acknowledged at the next checkpoint. For other kinds the wake comes from the cancel, not the timer |
| Region closing with timers pending | Unverified: no region code touches the timer driver. Fixture `region-lifecycle-close-cancels-children-001` shows a sleeper cancelled when its region closes |

### 12.3 Timer <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Timer expires | `process_timers()` in Phase 0 (`src/runtime/scheduler/three_lane.rs:6914`; lab before the pick). Expired wakers called. Woken tasks go to the ready or cancel lane, never the timed lane. C: lab step and round-robin loop fire due timers before dispatch (`src/runtime/scheduler.c:516`) |
| No timers pending | Production: the I/O leader waits in the reactor for at most 250 ms, others park without timeout (`src/runtime/scheduler/three_lane.rs:5814`). The lab never parks. C returns `ASX_E_WOULD_BLOCK` when nothing can wake a task, else blocks at most 1000 ms (`src/runtime/scheduler.c:937`) |
| Next timer deadline | Production park timeout = min(timer-driver, local timed, global timed deadlines); a follower uses only the local one. C jumps the virtual clock or waits until the earlier of the next timer and the run deadline |

### 12.4 Channel <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Channel send wakes receiver | Receiver's waker called (`src/channel/mpsc.rs:1616`). The lane is the scheduler's choice (not verified here). |
| Channel recv wakes sender | Head of the waiter queue woken (`src/channel/mpsc.rs:1801`); the cascade continues when it claims a slot. |
| Backpressure blocks sender | Sender task parks until woken by receiver consumption, an abort or drop, a waiter leaving, receiver close/drop, or its own cancellation. |

### 12.5 Channel <-> Obligation (Session Layer)

Rust only; C has no tracked permits (`bd-9kll.5.9`).

| Interaction | Behavior |
|-------------|----------|
| Reserve creates obligation | `TrackedPermit` contains `ObligationToken<SendPermit>` in Reserved state |
| Send resolves obligation | On success `send()` transitions Reserved -> Committed and returns `CommittedProof`; on a channel error the token is aborted and `Err` returned (`src/channel/session.rs:416`) |
| Abort resolves obligation | `abort()` transitions Reserved -> Aborted, returns `AbortedProof` |
| Drop without resolution | **PANIC**: obligation leaked (`[ASUP-E101]`), except while the thread is already unwinding. Linearity violation. |

### 12.6 Channel/Timer <-> Outcome/Budget

| Interaction | Behavior |
|-------------|----------|
| Channel send fails (Disconnected) | The channel returns `Err(Disconnected(value))`; the task's outcome is whatever its body returns |
| Channel send cancelled | The channel returns `Err(SendError::Cancelled(value))`. The task ends `Cancelled` through the cancel protocol (T8/T11), not because of the channel error (the recv case is fixture `mpsc-recv-cancel-first-001`; no fixture for send) |
| Timer fires deadline miss | Triggers cancellation with `Deadline` cancel kind (budget-deadline wake, `src/cx/cx.rs:357`; C `src/runtime/scheduler.c:224`) |
| Budget exhaustion during channel ops | There is no budget logic in `src/channel/mpsc.rs` (the first extraction's "remaining aborted" is wrong). Exhaustion surfaces through `cx.checkpoint()` as `Cancelled` at reserve/recv; held permits are not aborted. C differs: `asx_cx_checkpoint` spends a poll unit when a budget is bound and returns `ASX_E_POLL_BUDGET_EXHAUSTED` (`src/cx/cx.c:431`), which the channel reports as `ASX_E_CANCELLED` (`src/channel/mpsc.c:818`); Rust's checkpoint spends no quota (`bd-9kll.3.8`) |

---

## 13. Quiescence and Finalization Invariants

> **Superseded by `asupersync_v4_formal_semantics.md` §1.12 (`Quiescent(r)`), §5 INV-QUIESCENCE (`inv.region.quiescence` #27), INV-LEDGER-EMPTY-ON-CLOSE (`inv.obligation.ledger_empty_on_close` #20), §3.4.7, and §6 PROG-REGION (`prog.region.close_terminates` #28); C status: see C_REFINEMENT_MAP.md rows `inv.region.quiescence`, `inv.obligation.ledger_empty_on_close`, `prog.region.close_terminates` (Partial).** The spec defines quiescence per region: every child task Completed, every child region Closed and `ledger(r) = ∅`.

### 13.1 Quiescence Definition

Rust's runtime predicate (`RuntimeState::is_quiescent`, `src/runtime/state.rs:7364`; the lab adds an empty spawn mailbox and no pending obligation posts, `src/lab/runtime.rs:3069`) holds when ALL hold simultaneously:

1. `active_task_count == 0`
2. `reserved_obligation_count == 0` (and no pending cancel dispatch)
3. No region in `Closing`, `Draining` or `Finalizing`, none with a pending finalizer or spawn (Open regions are fine; the first extraction said "all regions Closed")
4. ~~Timer structures drained / no pending expirations~~ not a Rust condition
5. ~~Channel structures drained / no deliverable buffered work~~ not a Rust condition
6. The I/O driver, if any, has no registration

Fixture `quiescence-pending-timer-001` ends `quiescent: true` with both its regions Open. Every v2 fixture compares the snapshot's `quiescent` flag with Rust's.

### 13.2 Quiescence Checks

C checks quiescence per region: `asx_quiescence_check` (`src/runtime/quiescence.c:135`) and the Q1–Q4 report `asx_quiescence_check_detailed` (`src/runtime/quiescence.c:161`). The codes C actually returns are given first; the first extraction's codes, in brackets, are never returned.

| Check | Condition | Error if Violated |
|-------|-----------|-------------------|
| Task quiescence | the region has no live task | `ASX_E_QUIESCENCE_TASKS_LIVE` [`ASX_E_TASKS_STILL_ACTIVE`, `bd-udlh`] |
| Obligation quiescence | no Reserved obligation in the region | `ASX_E_OBLIGATIONS_UNRESOLVED` |
| Region quiescence | the region is `Closed`, its cleanup stack drained | `ASX_E_QUIESCENCE_NOT_REACHED` [`ASX_E_REGIONS_NOT_CLOSED`] |
| Timer quiescence | not checked | [`ASX_E_TIMERS_PENDING`] |
| Channel quiescence | not checked | [`ASX_E_CHANNEL_NOT_DRAINED`] |

### 13.3 Close-Time Leak Behavior

If finalization encounters unresolved obligations:
- Rust: once no task is left, still-Reserved obligations are leak-audited (`Leaked` under the leak policy) and the region closes (`src/runtime/state.rs:10271`)
- C: the region stays `Finalizing` and `asx_region_finalize_one` returns `ASX_E_OBLIGATIONS_UNRESOLVED`; nothing is marked Leaked at close (`src/runtime/quiescence.c:243`; `bd-9kll.3.4`)
- In both, the usual leak point is earlier: the holder's completion leaks what it still holds, and the leak is explicitly surfaced (trace event `obligation.leaked`, policy action), never silent

### 13.4 Cleanup Budget Under Cancellation

> **Superseded by `asupersync_v4_formal_semantics.md` §3.2.1 and §3.2.3; C status: see C_REFINEMENT_MAP.md rows `rule.cancel.acknowledge` and `prog.cancel.drains`.**

- Cleanup budget activates when task enters `Cancelling` (it replaces the task's budget)
- Repeated cancellation meets the cleanup budgets (never widens); a request during cleanup replaces the task's budget with the met one again
- Overrun strengthens the reason to `PollQuota`; the task keeps running until it completes (no force-complete in Rust; C offers it only as the opt-in `cleanup_hard_bound`)

### 13.5 Runtime Shutdown Sequence

The Rust runtime shutdown sequence was not re-verified at `5e60b1c4c` (unverified); a Rust region close command is `cancel_request` plus `advance_region_state` (`src/runtime/state.rs:5004`). C's subtree shutdown is `asx_region_drain` (`src/runtime/quiescence.c:291`):

1. Every `Open` region of the subtree moves to `Closing`, parents first
2. Every live task of the subtree is cancelled with `ParentCancelled` (task-only `asx_cancel_propagate`; the regions' reasons are not set)
3. Scheduler drives all tasks through cancel/complete paths (resumable after budget exhaustion)
4. Regions finalize bottom-up, cleanup stacks LIFO
5. Timers and channels are not drained by this sequence
6. Returns `ASX_OK` or the first blocking status

---

## 14. Deterministic Tie-Break Contract

> **Partly superseded by `asupersync_v4_formal_semantics.md` §2.4 (deterministic tie-breaking, normative), §7.1 and §8.3 (`inv.determinism.replayable` #46, `def.determinism.seed_equivalence` #47); C status: see C_REFINEMENT_MAP.md rows `inv.determinism.replayable`, `def.determinism.seed_equivalence` and the supplementary "§3.0 SCHEDULE-STEP" row.** The table is extraction at `5e60b1c4c`; the scheduler rows describe the production scheduler unless "lab" is said, and C ports only the lab.

### 14.1 Complete Ordering Summary

| Domain | Primary Key | Secondary Key | Tertiary Key |
|--------|------------|---------------|--------------|
| Channel messages | FIFO (VecDeque) | — | — |
| Channel waiters | FIFO (arrival order in the token queue; no monotonic ID) | — | — |
| Timer wheel (same slot) | Insertion order (Vec); cascades can reorder same-deadline timers (§10.7) | — | — |
| Timer overflow heap | Deadline | Generation (wrap-aware) | Timer id |
| Scheduler cancel lane | Priority (u8, higher first) | Generation (heap only) | Task id; dispatch picks `rng % n` among the top-priority group |
| Scheduler timed lane | Deadline (EDF) | Generation (wrap-aware) | Task id; RNG pick among equal deadlines; never fed outside tests |
| Scheduler ready lane | Priority (u8, higher first) | Generation (heap only) | Task id; RNG pick; the global queue is plain FIFO |
| Lab dispatch (Rust `LabScheduler`; C `src/runtime/lab_dispatch.c`) | Lane (cancel while streak < 16, due timed, ready, fallback cancel) | Priority | `rng % n` over the top-priority group in generation order (≤ 256) |
| Work stealing | Lab: RNG-start circular scan skipping the thief; production: see §11.7 | First available | — |
| Cancel propagation | Depth (parents first) | Stack-pop order (siblings reversed), Rust and C (§6.8) | Within a region: membership insertion order |
| Event journal | `event_seq` (strictly monotonic; C trace `sequence`, `src/runtime/trace.c:211`) | — | — |

### 14.2 Determinism Invariants

1. **Same seed -> same execution order:** holds for the lab, whose `DetRng` comes from the config seed (`src/lab/runtime.rs:4499`), and for C lab dispatch (`src/runtime/lab_dispatch.c:210`). Production workers seed their RNG with the worker id and run on OS threads
2. **Same inputs -> same outputs:** holds for the lab, which uses seed-derived hashers; unverified as a general production claim
3. **Generation monotonicity:** lane generations are monotone u64 counters (`+= 1`); the global cancel and ready queues have none
4. **Wrap safety:** timer generations wrap (`wrapping_add`) and timer ids are reused slab indices; a stale handle fails the generation-equality check (§10.6). There is no HashMap-based tracking. `SchedulerEntry` generations are compared without wrap handling
5. **Witness reproducibility:** `PreemptionFairnessCertificate::witness_hash` is deterministic over its 16 fields (§11.9); C has no certificate

### 14.3 Plan-Level Tie-Break Key

From `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`:

```
(lane_priority, logical_deadline, task_id, insertion_seq)
```

**This key is not implemented.** The first extraction said the C port uses it and that it is stronger than Rust's two-component keys; both statements are false. No C code orders by `lane_priority` or `logical_deadline`; `insertion_seq` exists only in the `src/time/timer_wheel.c` sort. What C does instead: lab dispatch takes the top-priority group in generation order and picks `r % n` (`src/runtime/lab_dispatch.c:344`); the round-robin scheduler polls in slot order; task timers order by (exact deadline, arm sequence) (`src/runtime/scheduler.c:101`); lab timers by (1 ms tick, registration). Rust uses three-component keys (§11.4) plus the RNG pick at dispatch. A different total order would not be "stronger": the fixtures compare the dispatch order exactly, so any other order is a semantic delta. C matches Rust because it does **not** use this key. The same claim appears in `docs/CHANNEL_TIMER_DETERMINISM.md`, `docs/CHANNEL_TIMER_KERNEL_SEMANTICS.md` and `docs/PHASE1_SPEC_REVIEW_GATE.md` (not corrected by this pass).

---

## 15. Forbidden Behavior Catalog

### 15.1 Critical Forbidden Behaviors

The Expected Result column gives C's actual status (and Rust's where it differs in kind). Per-row details and covering fixtures: `docs/LIFECYCLE_TRANSITION_TABLES.md` §8.

| ID | Behavior | Expected Result | Category |
|----|---------|-----------------|----------|
| FB-001 | Create child task in non-`Open` region | `ASX_E_REGION_CLOSED` (was `ASX_E_REGION_NOT_OPEN`, which C never returns); C still admits spawns in `Finalizing` (`bd-9kll.3.5`) | region-gate |
| FB-002 | Create obligation in non-`Open` region | `ASX_E_REGION_CLOSED` (fixture `obligation-reserve-closed-region-must-fail-001`) | region-gate |
| FB-003 | Double-commit obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` (`ASX_E_OBLIGATION_ALREADY_RESOLVED` is never returned by C) | obligation-linearity |
| FB-004 | Double-abort obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | obligation-linearity |
| FB-005 | Commit then abort obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | obligation-linearity |
| FB-006 | Abort then commit obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | obligation-linearity |
| FB-007 | Backward state transition (any domain) | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-008 | Skip intermediate state in region close | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-009 | Skip intermediate state in task lifecycle | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-010 | Operate on stale/freed handle | `ASX_E_STALE_HANDLE` (slot reused), `ASX_E_NOT_FOUND` (slot free) | handle-safety |
| FB-011 | Close region with unresolved obligations | C: region stays `Finalizing`, `ASX_E_OBLIGATIONS_UNRESOLVED`. Rust: leak-audit, then close (`bd-9kll.3.4`) | obligation-leak |
| FB-012 | Close region with active child tasks | Region stays `Closing` (`Draining` only with child regions) until they complete | region-drain |
| FB-013 | Cancel already-completed task | Idempotent no-op (not error) | cancel-idempotent |
| FB-014 | Access region arena after `Closed` | `ASX_E_REGION_CLOSED` | region-access |
| FB-015 | Non-deterministic behavior in deterministic mode | Detected by the conformance oracle's trace and dispatch-order comparison with Rust; the ghost determinism monitor has no runtime call site | determinism |

### 15.2 Resource Exhaustion Forbidden Behaviors

C's fixed-capacity resource plane (Rust allocates). Not re-verified in this pass, except that a full lab lane fails the lab step and a full `src/time/timer_wheel.c` table returns `ASX_E_RESOURCE_EXHAUSTED`.

| ID | Behavior | Expected Result |
|----|---------|-----------------|
| FB-100 | Exceed max ready queue capacity | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) |
| FB-101 | Exceed max timer node count | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) |
| FB-102 | Exceed max cancel queue capacity | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) |
| FB-103 | Exceed runtime memory ceiling | `ASX_E_RESOURCE_EXHAUSTED` (failure-atomic, no corruption) |
| FB-104 | Exceed trace event capacity | `ASX_E_RESOURCE_EXHAUSTED` or ring-buffer overwrite (profile-dependent) |

### 15.3 Protocol Violation Forbidden Behaviors

C-only debug monitors; Rust enforces these by types. Not re-verified in this pass.

| ID | Behavior | Expected Result |
|----|---------|-----------------|
| FB-200 | Yield across obligation boundary without copy | Ghost borrow ledger violation (debug) |
| FB-201 | Mutable access during shared borrow epoch | Ghost borrow ledger violation (debug) |
| FB-202 | Cross-thread access without transfer certificate | Ghost affinity violation (debug) |
| FB-203 | Non-checkpoint long-running loop in cancel path | CI lint flag |

### 15.4 Finalization Forbidden Behaviors

| ID | Behavior | Expected Result |
|----|---------|-----------------|
| FB-300 | Close with unresolved obligations without leak accounting | Must account for all (Rust leak-audits; C refuses to close, §13.3) |
| FB-301 | Complete close while child tasks/subregions non-terminal | Blocks in Closing, or in Draining when child regions remain |
| FB-302 | Quiescence success while timers/channels not drained | Not a rule in Rust or C: neither quiescence predicate looks at timers or channels (§13.1) |
| FB-303 | Phase regression during cancel/finalize | C task table and witness advance: `ASX_E_INVALID_TRANSITION`; Rust witness validator: `PhaseRegression` |
| FB-304 | Silent cleanup-budget overrun | Explicit: the reason is strengthened to `PollQuota` (Rust lab, C default); C's opt-in hard bound force-completes with the `ASX_SCHED_EVENT_CANCEL_FORCED` scheduler event |

---

## 16. Error Code Reference

C codes from `include/asx/asx_status.h`; Rust equivalents in `docs/CANONICAL_VOCABULARY_V2.md` §5. "Never returned" means no code under `src/` or `include/` other than the status table returns it (searched 2026-10-10 at `b412780`).

| Error Code | Description |
|-----------|-------------|
| `ASX_E_INVALID_TRANSITION` (200) | Attempted illegal state transition (C tables); also C's result for a double obligation resolve |
| `ASX_E_REGION_NOT_OPEN` (303) | Declared; never returned (admission failures return `ASX_E_REGION_CLOSED`) |
| `ASX_E_REGION_CLOSED` (301) | Region not admitting the spawn, child region or obligation (creating a channel in a non-`Open` region returns `ASX_E_INVALID_STATE`, `src/channel/mpsc.c:389`) |
| `ASX_E_ADMISSION_CLOSED` (304) | Not returned by the kernel admission paths (`src/runtime/lifecycle.c`); used by `src/runtime/parallel.c`, the adapters, the overload catalog and the circuit breaker |
| `ASX_E_OBLIGATION_ALREADY_RESOLVED` (500) | Declared; never returned (Rust returns `ObligationAlreadyResolved`; C returns `ASX_E_INVALID_TRANSITION`) |
| `ASX_E_OBLIGATION_LEAKED` | Does not exist in C |
| `ASX_E_UNRESOLVED_OBLIGATIONS` (501) | Containment fault status for a leak under the PANIC leak policy; not returned by finalization |
| `ASX_E_INCOMPLETE_CHILDREN` (903) | Declared; never returned (`bd-udlh`) |
| `ASX_E_STALE_HANDLE` (1100) | Handle generation mismatch |
| `ASX_E_RESOURCE_EXHAUSTED` (1000) | Resource contract ceiling exceeded |
| `ASX_E_BUDGET_EXHAUSTED` | Does not exist in C; the codes are `ASX_E_POLL_BUDGET_EXHAUSTED` (404), `ASX_E_COST_QUOTA_EXHAUSTED` (1002) and `ASX_E_POLL_QUOTA_EXHAUSTED` (1003) |
| `ASX_E_TASKS_STILL_ACTIVE` (900) | Declared; never returned (`bd-udlh`); region checks return `ASX_E_QUIESCENCE_TASKS_LIVE` (905) |
| `ASX_E_OBLIGATIONS_UNRESOLVED` (901) | Region still has a Reserved obligation (finalize, quiescence check) |
| `ASX_E_REGIONS_NOT_CLOSED` (902) | Declared; never returned; region checks return `ASX_E_QUIESCENCE_NOT_REACHED` (904) |
| `ASX_E_TIMERS_PENDING` (801) | Declared; never returned |
| `ASX_E_CHANNEL_NOT_DRAINED` (703) | Declared; never returned |
| `ASX_E_WITNESS_TASK_MISMATCH` (603) | Declared; never returned |
| `ASX_E_WITNESS_REGION_MISMATCH` (604) | Declared; never returned |
| `ASX_E_WITNESS_EPOCH_MISMATCH` (605) | Declared; never returned |
| `ASX_E_WITNESS_PHASE_REGRESSION` (601) | Declared; never returned (C witness advance returns `ASX_E_INVALID_TRANSITION`) |
| `ASX_E_WITNESS_REASON_WEAKENED` (602) | Declared; never returned |

---

## 17. Handle Encoding Reference

Bitmasked generational typestate handles: `[ 16-bit type_tag | 16-bit state_mask | 32-bit arena_index ]` (C only; Rust ids are typed arena indices). The 32-bit index is itself `[generation:16 | slot:16]` (`include/asx/asx_ids.h:78`); the generation is what detects stale handles.

### 17.1 Type Tags

| Type | Tag Value |
|------|----------|
| Region | `0x0001` |
| Task | `0x0002` |
| Obligation | `0x0003` |
| Cancel Witness | `0x0004` |
| Timer | `0x0005` |
| Channel | `0x0006` |

### 17.2 State Masks (Region Example)

| State | Mask Value |
|-------|-----------|
| `Open` | `0x0001` |
| `Closing` | `0x0002` |
| `Draining` | `0x0004` |
| `Finalizing` | `0x0008` |
| `Closed` | `0x0010` |

The mask values match the code (`1 << state`), but no endpoint checks them: the mask is set when a handle is packed and never updated, and `asx_handle_state_allowed` (`include/asx/asx_ids.h:71`) has no caller under `src/`. Endpoints validate type tag, slot bounds, liveness and generation, then read the state from the slot (`src/runtime/lifecycle.c:206`).

---

## 18. Invariant Schema

These IDs are this document's own (they are also in `schemas/invariant_schema.json`, whose INV-CH-03 and INV-CH-08 wording still has the errors corrected below). They are not v4 rule IDs; the v4 invariants (§5) are mapped in `docs/C_REFINEMENT_MAP.md`. The status column was added by the 2026-10-10 re-verification.

### 18.1 Channel Invariants

| ID | Invariant | Category | Status at `5e60b1c4c` / C |
|----|-----------|----------|---------------------------|
| `INV-CH-01` | `used_slots = queue.len() + reserved <= capacity` at all times | Capacity | Holds (`src/channel/mpsc.rs:349`); C `src/channel/mpsc.c:571` |
| `INV-CH-02` | Messages delivered in FIFO order | Ordering | Holds; C ring buffer |
| `INV-CH-03` | Waiters serviced in FIFO order (arrival order in the token queue; there is no monotonic ID) | Fairness | Holds as corrected |
| `INV-CH-04` | `try_reserve` returns `Full` when a live waiter is queued | Fairness | Holds in Rust; C ignores cancel-requested waiters (§9.4, unrecorded drift) |
| `INV-CH-05` | Cancelled reserve does not consume capacity | Cancel safety | Holds (checkpoint before `reserved += 1`); C `src/channel/mpsc.c:831` |
| `INV-CH-06` | Cancelled recv does not consume message | Cancel safety | Holds (checkpoint before pop) |
| `INV-CH-07` | `receiver_dropped` monotone: `false -> true`, never reverses | State monotonicity | Holds; C closed states never reopen |
| `INV-CH-08` | `sender_count` never rises from 0 (it is not decrement-only: clone and weak upgrade increment it) | State monotonicity | Corrected (`src/channel/mpsc.rs:1231`, `src/channel/mpsc.rs:1275`); C `SENDER_CLOSED` is terminal |
| `INV-CH-09` | Permit Drop without send/abort decrements `reserved`, wakes the head and aborts the obligation with reason Cancel | RAII safety | Holds (`src/channel/mpsc.rs:1663`); C has no destructor (`bd-9kll.5.3`) |
| `INV-CH-10` | Session-tracked permit leaked -> panic (not while already unwinding) | Obligation | Holds; not implemented in C (`bd-9kll.5.9`) |

### 18.2 Timer Invariants

| ID | Invariant | Category | Status at `5e60b1c4c` / C |
|----|-----------|----------|---------------------------|
| `INV-TM-01` | Cancel requires exact `(id, generation)` match | Handle safety | Holds (`src/time/wheel.rs:577`); C `src/time/timer_wheel.c:174` |
| `INV-TM-02` | Cancelled timers skipped at fire time via `is_live()` | Lazy deletion | Holds; compaction may also remove them earlier |
| `INV-TM-03` | Same-deadline timers fire in insertion order | Ordering | Only within one slot or the ready list; a cascade from level 1 can put an earlier registration after later ones (§10.7). C enforces it unconditionally |
| `INV-TM-04` | `try_register` rejects duration > `max_timer_duration` | Validation | Holds for `try_register`; the runtime path clamps instead. C rejects past 7 days, as `try_register`; C sleeps are unbounded |
| `INV-TM-05` | All-cancelled triggers `purge_inactive_storage()` | Cleanup | Holds (`src/time/wheel.rs:585`); n/a in C |
| `INV-TM-06` | Timer handle reuse is collision-safe: ids are reused slab indices and generations wrap (`wrapping_add`); a stale handle fails the generation check | Wrap safety | Corrected (the first extraction said u64 id wrap and a HashMap). C: 32-bit per-slot generation |
| `INV-TM-07` | Dead entries dropped during cascade level promotion | Cascade safety | Holds; n/a in C |
| `INV-TM-08` | Fire criterion: `entry.deadline <= now` (inclusive) | Fire semantics | Holds except the coalescing hold (§10.8) |

### 18.3 Scheduler Invariants

| ID | Invariant | Category | Status at `5e60b1c4c` / C |
|----|-----------|----------|---------------------------|
| `INV-SC-01` | Cancel lane dispatched before timed before ready (default) | Lane priority | Holds until the cancel streak reaches its limit; `MeetDeadlines` reorders. v4 §2.4 states strict priority (normative). C lab: cancel > ready |
| `INV-SC-02` | Equal-priority entries dispatched in generation (FIFO) order | Ordering | False for dispatch: the pick is `rng % n` among the top-priority group (§11.6); C lab does the same |
| `INV-SC-03` | Equal-deadline timed entries dispatched in generation (FIFO) order | EDF ordering | False for RNG-hinted pops; true for the global `TimedQueue`; the timed lane is never fed outside tests. Not implemented in C |
| `INV-SC-04` | Cancel-streak limit prevents cancellation starvation | Fairness | Holds per worker (Rust production and lab; C lab). No fixture or C test reaches the limit |
| `INV-SC-05` | Same seed -> same work-steal scan order | Determinism | Lab only; not implemented in C |
| `INV-SC-06` | Identical certificate fields produce identical fairness certificate hash | Replay | Holds (`src/runtime/scheduler/three_lane.rs:4709`); not implemented in C |
| `INV-SC-07` | Phase 0 (timers) executes before task dispatch | Phase ordering | Holds (Rust production and lab; C lab and round-robin) |
| `INV-SC-08` | Governor suggestion affects lane order but not correctness | Safety | Unverified (no proof or test; the governor is off by default); n/a in C |

---

## 19. Fixture Family Mapping

**Of the 122 planned IDs in this section, 121 were never captured** (no `fixtures/rust_reference_v2/<id>.json` and nothing under `fixtures/rust_reference/`, checked 2026-10-10). The one that exists, `budget-meet-identity-010`, is in the legacy corpus (`fixtures/rust_reference/core_budget/`), recorded at `a9e737d8` by the old capture tool, which wrote `budget_meet` as a user trace and never called `Budget::meet` (`docs/RUST_FIXTURE_CAPTURE_TOOLING.md` §4). `make conformance` runs only `fixtures/rust_reference_v2` (`Makefile:1564`). The planned IDs are kept as plain text so that the rows stay recognizable; the "Covered by" column names the Rust-captured v2 fixtures (at `5e60b1c4c`) or C tests that exercise each row today. Vocabulary v2 has no task-state, Draining or Finalizing events, so intermediate states are compared only when they are end states. Fixtures per v4 rule: `docs/C_REFINEMENT_MAP.md`.

### 19.1 Region Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| region-lifecycle-001 | Open -> Closing -> Draining -> Finalizing -> Closed (happy path) | Partly: `region-lifecycle-close-cancels-children-001` (closes without Draining), `region-lifecycle-draining-with-child-region-001` (ends in Draining) |
| region-lifecycle-002 | Open -> Closing with active children (drain required) | `region-lifecycle-close-cancels-children-001` |
| region-lifecycle-003 | Nested region cascade close | `region-lifecycle-cancel-propagates-001` (a chain; no sibling regions) |
| region-lifecycle-004 | Region close with unresolved obligations (leak detection) | None for an obligation still Reserved at close (C and Rust differ, §3.4) |
| region-lifecycle-005 | Backward transition (Closing -> Open) must fail | C test `tests/unit/core/test_transition.c` (`region_forbidden_backward`) |
| region-lifecycle-006 | Skip transition (Open -> Draining) must fail | C test `tests/unit/core/test_transition.c` (`region_forbidden_skip`) |
| region-lifecycle-007 | Create task in non-Open region must fail (`ASX_E_REGION_CLOSED`) | `spawn-into-cancelled-region-step-001`, `region-lifecycle-closed-child-spawn-001` |
| region-lifecycle-008 | Arena access after Closed must fail | None |
| region-lifecycle-009 | Empty region close (no children) fast path | `region-lifecycle-closed-child-spawn-001` (an empty child region closes at once) |
| region-lifecycle-010 | Region close under resource exhaustion | None (C-only resource plane) |

### 19.2 Task Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| task-lifecycle-001 | Created -> Running -> Completed(Ok) happy path | `task-lifecycle-spawn-join-001` |
| task-lifecycle-002 | Full cancel: all phases traversed | Partly: `cancel-masked-checkpoint-001` (ends Completed(Cancelled); C usually skips Finalizing) |
| task-lifecycle-003 | CancelRequested -> Completed: the outcome is `Cancelled(reason)` (the first extraction said the natural outcome is preserved; §4.3 T8) | None isolates it |
| task-lifecycle-004 | Cancel before first poll | `task-abort-next-step-001` (a fresh child aborted at its admission); the single Created → CancelRequested step, which vocabulary v2 cannot see, by `tests/unit/runtime/test_cancellation.c::cancel_created_task_goes_straight_to_cancel_requested` |
| task-lifecycle-005 | Error at spawn time | None; C never takes T3 |
| task-lifecycle-006 | Cancel strengthen: repeated requests with increasing severity | `cancel-strengthen-severity-001`, `task-abort-next-step-001` |
| task-lifecycle-007 | Cancel strengthen budget combine: min-plus algebra | C test `tests/unit/core/test_budget.c` (`budget_meet_tightens`) |
| task-lifecycle-008 | Backward transition must fail | C test `tests/unit/core/test_transition.c` |
| task-lifecycle-009 | Skip transition must fail | C test `tests/unit/core/test_transition.c` |
| task-lifecycle-010 | Completed is absorbing: all transitions rejected | C test `tests/unit/core/test_transition.c` (`task_completed_absorbing`) |
| task-lifecycle-011 | acknowledge_cancel() returns reason and applies budget | C test `tests/unit/runtime/test_cancellation.c` (`acknowledgement_replaces_the_budget_with_the_cleanup_budget`); `budget-inherit-before-cleanup-001` |
| task-lifecycle-012 | finalize_done() produces CancelWitness | None (the C witness has no task, region or epoch) |
| task-lifecycle-013 | cancel_epoch set only by the first cancel | No fixture (the epoch is not in the vocabulary); `tests/unit/runtime/test_cancellation.c::cancel_created_task_goes_straight_to_cancel_requested` |

### 19.3 Obligation Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| obligation-lifecycle-001 | Reserved -> Committed (happy path) | `obligation-reserve-commit-001` |
| obligation-lifecycle-002 | Reserved -> Aborted (rollback) | `obligation-abort-reasons-001` |
| obligation-lifecycle-003 | Reserved -> Leaked (holder completion or drop; not region finalization) | `obligation-cancelled-holder-leaks-001`, `leak-policy-leak-reported-001`, `obligation-drop-leaks-at-once-001` |
| obligation-lifecycle-004 | Double-commit must fail (C `ASX_E_INVALID_TRANSITION`) | C test `tests/invariant/lifecycle/test_lifecycle_legality.c` (`obligation_double_commit_rejected`); unreachable from DSL v2 |
| obligation-lifecycle-005 | Double-abort must fail | Same file (`obligation_double_abort_rejected`) |
| obligation-lifecycle-006 | Commit then abort must fail | Same file (`obligation_commit_then_abort_rejected`) |
| obligation-lifecycle-007 | Abort then commit must fail | Same file (`obligation_abort_then_commit_rejected`) |
| obligation-lifecycle-008 | No Reserved obligation at close | C test `tests/invariant/lifecycle/skeleton_test.c` (`region_drain_blocks_unresolved_obligations`) |
| obligation-lifecycle-009 | Multiple obligations in single region | `obligation-abort-reasons-001` |
| obligation-lifecycle-010 | Obligation in non-Open region must fail (`ASX_E_REGION_CLOSED`) | `obligation-reserve-closed-region-must-fail-001` |

### 19.4 Cancellation Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| cancel-protocol-001 | Full cancel protocol happy path | Partly: `cancel-masked-checkpoint-001` |
| cancel-protocol-002 | Cancel propagation through region tree | `region-lifecycle-cancel-propagates-001` |
| cancel-protocol-003 | Cancel with cleanup budget enforcement | Snapshot `cleanup_budget` is compared in every cancel fixture; `budget-inherit-before-cleanup-001` |
| cancel-protocol-004 | Cancel budget exceeded (reason strengthened to PollQuota; no force-completion) | None |
| cancel-protocol-005 | Multiple cancel on same target (idempotent) | `cancel-strengthen-severity-001`, `task-abort-next-step-001` |
| cancel-protocol-006 | Cancel attribution chain recorded | `region-lifecycle-cancel-propagates-001` |
| cancel-protocol-007 | Each reason kind produces correct metadata | User, Timeout, Deadline, PollQuota, ParentCancelled and Shutdown are fixture-checked (C_REFINEMENT_MAP row `def.cancel.reason_kinds`) |
| cancel-protocol-008 | Backward cancel phase must fail | None (witnesses are not compared) |
| cancel-protocol-009 | Nested cancel (cancel during cancelling) | None identified |
| cancel-protocol-010 | Deterministic cancel propagation order | Every fixture replays Rust's dispatch order; sibling-region order is not covered (§6.8) |

### 19.5 Outcome Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| outcome-lattice-001 | Severity ordering | `task-lifecycle-err-outcome-001`, `task-lifecycle-panic-001`; C test `tests/unit/core/test_outcome.c` (`outcome_lattice_order`) |
| outcome-lattice-002 | Join commutativity (in severity) | C test `tests/unit/core/test_outcome.c` |
| outcome-lattice-003 | Join associativity | C test `tests/unit/core/test_outcome.c` (`outcome_join_associative`) |
| outcome-lattice-004 | Join identity (Ok) | C test `tests/unit/core/test_outcome.c` |
| outcome-lattice-005 | Join absorbing (Panicked) | C test `tests/unit/core/test_outcome.c` |
| outcome-lattice-006 | Region outcome aggregation | None (C has no region outcome) |
| outcome-lattice-007 | Empty region outcome = Ok, or `Cancelled(reason)` if the region was cancelled (§7.5) | None |
| outcome-join-left-bias-001 | Equal-severity left-bias | C test `tests/unit/core/test_outcome.c` (`outcome_join_left_bias`) |
| outcome-join-order-002 | Join order independence (in severity only) | C test `tests/unit/core/test_outcome.c` |
| outcome-join-top-003 | Top element (Panicked) absorbs | C test `tests/unit/core/test_outcome.c` |
| outcome-cancel-strengthen-004 | Cancel outcome strengthening (Rust `Cancelled` + `Cancelled` join) | None; C outcomes carry no reason (`bd-9kll.3.14`) |

### 19.6 Budget Fixtures

| Planned ID | Description | Covered by |
|-----------|-------------|------------|
| `budget-meet-identity-010` (legacy, exists) | INFINITE is meet identity | Legacy fixture only (see the section note); C test `tests/unit/core/test_budget.c` (`budget_infinite_is_identity`) |
| budget-meet-absorbing-011 | ZERO is absorbing | C test `tests/unit/core/test_budget.c` (`budget_zero_is_absorbing`) |
| budget-deadline-none-012 | None deadline handling | None identified |
| budget-consume-poll-013 | Poll quota consumption | `budget-poll-quota-exhaustion-001` |
| budget-consume-cost-014 | Cost quota consumption | None (cost quota is not covered; `bd-9kll.3.10`) |
| budget-deadline-boundary-015 | Deadline boundary behavior | `budget-deadline-sleep-checkpoint-001` |
| runtime-pollquota-cancel-016 | Poll exhaustion triggers cancel | `budget-poll-quota-exhaustion-001`, `task-groups-quorum-poll-quota-001` |
| cancel-cleanup-budget-017 | Cleanup budget under cancel | `budget-inherit-before-cleanup-001` |

### 19.7 Channel Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| ch-reserve-send-001 | Basic reserve -> send -> recv cycle | `mpsc-two-phase-send-recv-001` |
| ch-reserve-abort-001 | Reserve -> abort -> capacity freed | None in v2 (the `permit_abort` DSL op is unused); C unit tests in `tests/unit/` |
| ch-backpressure-001 | Full channel blocks sender, recv unblocks | `mpsc-two-phase-send-recv-001` (capacity 1) |
| ch-fifo-001 | Multiple senders, FIFO delivery | None in v2 (no multi-sender fixture) |
| ch-fifo-fairness-001 | try_reserve respects waiter queue FIFO | None in v2 (C unit tests only) |
| ch-cancel-reserve-001 | Cancel during pending reserve | None in v2 |
| ch-cancel-recv-001 | Cancel during recv, message preserved | Partly: `mpsc-recv-cancel-first-001` (empty channel, so preservation is not tested) |
| ch-evict-001 | send_evict_oldest evicts front | None; not implemented in C |
| ch-evict-reserved-001 | Evict fails when all capacity reserved | None; not implemented in C |
| ch-close-drain-001 | Sender drop -> receiver drains remaining | `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001` |
| ch-close-wake-001 | Receiver drop -> all senders woken | Partly: `lab-dispatch-mpsc-disconnect-order-001` |
| ch-obligation-leak-001 | TrackedPermit drop -> panic | None; `TrackedPermit` is not in C |
| ch-obligation-commit-001 | TrackedPermit send -> CommittedProof | None; not in C |
| ch-obligation-abort-001 | TrackedPermit abort -> AbortedProof | None; not in C |
| ch-cascade-001 | Permit abort cascades wake | None in v2 (C unit tests only) |
| ch-capacity-invariant-001 | Capacity invariant holds across all ops | None in v2 (C unit tests only) |
| ch-capacity-zero-panic-001 | Zero capacity panics at creation | None; C returns `ASX_E_INVALID_ARGUMENT` instead |
| channel-two-phase-001 | Reserve -> send -> recv (value delivered) | `mpsc-two-phase-send-recv-001` |
| channel-abort-release-002 | Reserve -> abort (capacity restored) | None in v2 |
| channel-drop-permit-abort-003 | Drop = abort (no leaked reservation) | None (no v2 fixture drops an unsent channel permit) |
| channel-fifo-waiter-004 | Queued waiter + try_reserve -> Full | None in v2 |
| channel-reserve-cancel-005 | Pending reserve then cancel (waiter removed) | None in v2 |
| channel-recv-cancel-nonconsume-006 | Cancelled recv preserves message | Partly: `mpsc-recv-cancel-first-001` |
| channel-evict-reserved-007 | Evict with all reserved -> Full | None; not implemented in C |
| channel-evict-committed-008 | Evict oldest committed | None; not implemented in C |

Channel fixtures that do exist in v2: `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001`, `mpsc-recv-cancel-first-001`, `lab-dispatch-mpsc-disconnect-order-001`, `oneshot-send-recv-001`, `broadcast-fanout-001`, `watch-changed-001`. The legacy corpus has `channel-two-phase-send-recv-001`, `channel-fifo-ordering-002` and `channel-backpressure-full-003` (`a9e737d8`; not run by `make conformance`).

### 19.8 Timer Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| tm-insert-fire-001 | Basic insert -> advance -> fire | `quiescence-pending-timer-001` |
| tm-same-deadline-001 | Same deadline, insertion order preserved | `timers-same-deadline-001` |
| tm-cancel-001 | Insert -> cancel -> not fired | `task-abort-next-step-001`, `region-lifecycle-close-cancels-children-001` (sleepers cancelled before firing) |
| tm-cancel-stale-001 | Stale generation cancel rejected | None in v2 |
| tm-cancel-reuse-001 | Same ID, different generation independent | None |
| tm-duration-exceeded-001 | Beyond max duration -> error | None (no v2 timer exceeds 1 ms) |
| tm-cascade-001 | Level 0 wrap triggers Level 1 cascade | None |
| tm-overflow-001 | Far-future in overflow heap | None |
| tm-coalesce-001 | Coalescing window fires together | None; no coalescing in C |
| tm-wrap-001 | ID/generation wrap safe (generation only; ids are reused slab indices) | None |
| tm-purge-001 | All cancelled -> storage purged | None; n/a in C |
| tm-immediate-001 | Deadline in past -> immediately ready | None identified |
| timer-equal-deadline-order-010 | Insertion order preserved | `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` |
| timer-cancel-generation-011 | Stale rejected; live cancellable | None |
| timer-next-deadline-same-tick-012 | Same tick deadline preserved | `lab-dispatch-timer-wheel-order-001` (out-of-order registrations due in one advance wake in registration order) |
| timer-overflow-promotion-013 | Promoted and fired when in range | None |
| timer-coalescing-threshold-014 | Coalescing only when threshold met | None |

Timer-related v2 fixtures that exist: `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001`, `quiescence-pending-timer-001`, `budget-deadline-sleep-checkpoint-001`, `combinators-race-timeout-001`, `lab-dispatch-waker-rearm-001`. The legacy corpus has `timer-register-fire-001`, `timer-equal-deadline-ordering-002` and `timer-stale-cancel-rejected-003` (`a9e737d8`).

### 19.9 Scheduler Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| sc-lane-priority-001 | Cancel > timed > ready ordering | Cancel before ready only, e.g. `lab-dispatch-cancel-waker-once-001`; no fixture dispatches from the timed lane |
| sc-fifo-priority-001 | Equal priority -> FIFO dispatch | Premise wrong: equal-priority dispatch is an RNG pick (`lab-dispatch-tie-break-001`) |
| sc-edf-001 | Earliest deadline first dispatch | None (timed lane unused) |
| sc-edf-fifo-001 | Equal deadline -> FIFO | None |
| sc-rng-tiebreak-001 | RNG tie-break deterministic with seed | `lab-dispatch-tie-break-001`, `scheduler-yield-interleave-001` |
| sc-steal-001 | Work stealing follows RNG circular order | None (all fixtures are single-worker) |
| sc-cancel-streak-001 | Cancel streak triggers fairness yield | None (the longest cancel run is 10, in `sync-mutex-cancelled-front-baton-001`) |
| sc-governor-meet-001 | MeetDeadlines reorders timed > cancel | None (no governor in the lab or C) |
| sc-governor-drain-001 | DrainObligations doubles cancel limit | None |
| sc-certificate-001 | Identical traces -> identical hash | None: fixtures record `schedule.certificate_hash`, which C does not compute or compare |
| sc-timer-phase0-001 | Expired timers before task dispatch | Implicitly: `lab-dispatch-timer-wheel-order-001`, `timers-same-deadline-001`, `quiescence-pending-timer-001` |

### 19.10 Finalization Fixtures

| Planned ID (never captured) | Description | Covered by |
|-----------|-------------|------------|
| finalization-quiescence-001 | Full close reaches quiescence | Every v2 fixture compares the `quiescent` flag, e.g. `region-lifecycle-close-cancels-children-001` |
| finalization-quiescence-002 | Active task prevents quiescence | `budget-cancel-unacknowledged-stays-running-001` (the run ends non-quiescent) |
| finalization-leak-003 | Unresolved obligations leaked deterministically | `obligation-cancelled-holder-leaks-001` (leaked at holder completion) |
| finalization-channel-drain-004 | Non-drained channel blocks quiescence | Premise wrong: channels are not part of quiescence (§13.1) |
| finalization-timer-drain-005 | Pending timer blocks quiescence | Premise wrong in the form stated (§13.1); `quiescence-pending-timer-001` |
| finalization-cancel-budget-006 | Cleanup budget overrun -> reason strengthened to PollQuota, task runs to its own completion | None |
| finalization-phase-regression-007 | Illegal phase regression rejected | None (witnesses are not compared) |

---

## 20. C Port Implementation Contract

### 20.1 Non-Negotiable Preservation Rules

The per-rule C status against the v4 spec is in `docs/C_REFINEMENT_MAP.md` (28 Implemented, 4 Variant, 14 Partial, 1 Missing at the time of this pass); the rules below are the port's own contract.

1. State transition legality and monotonicity semantics remain intact
2. Cancellation strengthening and bounded cleanup remain deterministic
3. Obligation linearity remains explicit and auditable
4. Channel/timer ordering and stale-handle rejection remain deterministic
5. Exhaustion behavior is failure-atomic with explicit status surfaces
6. Profile differences affect operational limits only, not canonical outcomes
7. Scheduler dispatch ordering is replay-deterministic for same seed/input (C lab dispatch; the round-robin scheduler is deterministic but ignores the seed)

### 20.2 C99 Usage Guidelines

C99 is allowed only where it improves correctness/clarity without changing semantics:
- Fixed-width integer typing for handle encodings and counters
- Designated initialization for explicit state structures
- Structured compile-time checks/macros for transition tables

No dependency-bearing substitutions in core runtime semantics.

### 20.3 C Port Strengthening

The first extraction said the C port introduces one semantic strengthening over Rust, a **4-component deterministic tie-break key** `(lane_priority, logical_deadline, task_id, insertion_seq)`, and that Rust uses 2-component keys. **Both statements are false** (§14.3): no C code uses that key, Rust's heaps use three-component keys plus an RNG pick at dispatch, and a different total order would be a semantic delta, not a strengthening, because the fixtures compare dispatch order exactly. C's lab dispatch reproduces Rust's lab ordering instead (`src/runtime/lab_dispatch.c:344`, `src/runtime/lab_dispatch.c:380`). The port currently claims no semantic strengthening; the opt-in `cleanup_hard_bound` is a deviation excluded from parity (`bd-9kll.3.2`), not a strengthening.

### 20.4 Pending Clarifications

- Budget/cancellation naming alignment follows canonical constructor surfaces (`deadline`, `poll_quota`, `cost_quota`; the cancel kind is `CostBudget`) rather than stale comments in older text.
- Any semantic ambiguity discovered later must be resolved via explicit ruling plus fixture updates, not by implementation-side guesswork. Where `asupersync_v4_formal_semantics.md` and Rust's code disagree (for example the masking rule, `rule.cancel.checkpoint_masked`, or entering Draining, `rule.region.close_cancel_children`), C follows the code and `docs/C_REFINEMENT_MAP.md` records the row as Variant.

---

## Provenance Cross-Reference

### Source-to-Fixture Provenance Map Summary

`docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` (truth-passed 2026-10-10) has 38 provenance rows: 4 `mapped`, 20 `partial`, 14 `unmapped` (its section 6). The first extraction's table below claimed 33 rows, all `mapped`, covering ~112 fixture IDs; most of those IDs were never captured (section 19). The table is kept with the statuses taken from the current map.

| Domain | Prov IDs | Rows | Fixture IDs | Status (provenance map, 2026-10-10) |
|--------|----------|------|-------------|--------|
| Baseline | BASELINE-001 | 1 | metadata | mapped |
| Outcome | OUTCOME-001..002 | 2 | planned IDs not captured; v2 coverage partial | partial |
| Budget | BUDGET-001..002 | 2 | only legacy `budget-meet-identity-010` exists | BUDGET-001 unmapped, BUDGET-002 partial |
| Region | REGION-001..002 | 2 | v2 region fixtures | partial |
| Task | TASK-001 | 1 | v2 task fixtures | partial |
| Obligation | OBLIGATION-001 | 1 | v2 obligation fixtures | partial |
| Handle | HANDLE-001 | 1 | none in v2 | unmapped |
| Channel | CHANNEL-001..007 | 7 | v2 mpsc/oneshot/broadcast/watch fixtures | 5 partial, 2 unmapped |
| Timer | TIMER-001..008 | 8 | v2 timer fixtures cover the first tick only | 2 partial, 6 unmapped |
| Scheduler | SCHEDULER-001..006 | 6 | v2 lab-dispatch fixtures | 3 partial, 3 unmapped |
| Quiescence | QUIESCENCE-001 | 1 | v2 `quiescent` flag | partial |
| Finalization | FINALIZE-001 | 1 | see the provenance map | partial |
| **Total** | | **33 of 38** | | |

### Parity Requirements

Not re-verified in this pass (a C-side reporting contract). The v2 fixtures carry a different provenance record: `rust_baseline_commit`, `rust_toolchain_commit_hash`, `rust_toolchain_release`, `rust_toolchain_host`, `cargo_lock_sha256`, `capture_run_id`, `producer`, `scenario_dsl` and `vocabulary` (for example `fixtures/rust_reference_v2/quiescence-pending-timer-001.json`). Every parity report record must include:
- `rust_baseline_commit`, `rust_toolchain_commit_hash`, `rust_toolchain_release`, `rust_toolchain_host`
- `cargo_lock_sha256`, `cargo_lock_bytes`, `cargo_lock_tracked_by_git`
- `fixture_schema_version`, `scenario_dsl_version`
- `scenario_id`, `codec`, `profile`, `parity`, `semantic_digest`, `delta_classification`

### Delta Classification

- `none`: no semantic delta
- `intentional_upstream`: upstream Rust changed intentionally
- `c_regression`: C implementation deviated
- `spec_defect`: specification was incorrect/incomplete
- `harness_defect`: fixture/parity tooling error

---

*This document summarizes kernel semantics for the asx ANSI C port with Rust and C citations at the baselines in its header. Where a section is marked superseded, `asupersync_v4_formal_semantics.md` at `5e60b1c4c` is authoritative and `docs/C_REFINEMENT_MAP.md` gives C's status. Deviations require explicit approval, fixture additions, and delta classification.*
