# Existing Asupersync Structure — Canonical Extracted Semantics

> **Bead:** `bd-296.1`
> **Status:** Canonical, implementation-driving semantic extraction (Phase 1)
> **Rust baseline commit:** `38c152405bd03e2bd9eecf178bfbbe9472fed861`
> **Rust toolchain snapshot:** `rustc 1.95.0-nightly (7f99507f5 2026-02-19)`
> **Baseline inventory:** `docs/rust_baseline_inventory.json`
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

This document is the **authoritative semantic baseline** for the ANSI C port. It consolidates all completed extraction slices into one canonical contract. Downstream implementation and test beads consume this document without re-reading Rust internals.

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

| Source File | Domain | Lines |
|-------------|--------|-------|
| `src/record/region.rs` | Region lifecycle | — |
| `src/record/task.rs` | Task lifecycle | — |
| `src/record/obligation.rs` | Obligation lifecycle | — |
| `src/types/outcome.rs` | Outcome lattice | — |
| `src/types/budget.rs` | Budget algebra | — |
| `src/types/cancel.rs` | Cancellation protocol | — |
| `src/channel/mpsc.rs` | MPSC channel core | 1679 |
| `src/channel/session.rs` | Obligation-tracked channel | ~170 |
| `src/time/wheel.rs` | Hierarchical timer wheel | 2123 |
| `src/time/driver.rs` | Timer driver/integration | ~550 |
| `src/time/deadline.rs` | Deadline abstraction | 168 |
| `src/runtime/timer.rs` | Runtime timer heap | 271 |
| `src/runtime/scheduler/three_lane.rs` | Scheduler loop | ~5200 |
| `src/runtime/scheduler/priority.rs` | Priority scheduling | ~730 |
| `src/runtime/scheduler/global_injector.rs` | Global task injection | ~450 |
| `src/runtime/scheduler/stealing.rs` | Work stealing | ~200 |
| `src/runtime/scheduler/intrusive.rs` | Intrusive queue | ~107 |
| `src/combinator/join.rs` | Join combinator | — |
| `tests/algebraic_laws.rs` | Algebraic law tests | — |

All paths relative to `/data/projects/asupersync/`.

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

### 1.3 Determinism Contract

For fixed `(scenario_input, seed, profile, codec_schema)`, all runtime behavior is deterministic:

- Scheduler tie-break sequencing
- Channel waiter/service ordering
- Timer equal-deadline ordering
- Cancellation witness phase progression
- Exhaustion and failure outcomes
- Event journal ordering

---

## 2. Core Semantic Model

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

### 3.1 States

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Open` | `ASX_REGION_OPEN` | Active; children (tasks, sub-regions, obligations) may be created |
| `Closing` | `ASX_REGION_CLOSING` | Close requested; no new children accepted; existing work continues |
| `Draining` | `ASX_REGION_DRAINING` | Waiting for child tasks and sub-regions to complete |
| `Finalizing` | `ASX_REGION_FINALIZING` | All children complete; resolving obligations and cleanup |
| `Closed` | `ASX_REGION_CLOSED` | Terminal; all resources released, arena eligible for reclamation |

### 3.2 Legal Transitions

| # | From | To | Trigger | Preconditions | Postconditions |
|---|------|----|---------|---------------|----------------|
| R1 | `Open` | `Closing` | `begin_close(reason)` | state==Open | No new spawns; cancel reason set |
| R1a | `Open` | `Closing` | Parent region begins closing | Parent transitioning | Cascading close propagated |
| R1b | `Open` | `Closing` | Cancellation from parent | Parent cancel reaches region | Cancel forwarded to child tasks |
| R2 | `Closing` | `Draining` | `begin_drain()` | state==Closing | Child cancellation requests issued |
| R3 | `Closing` | `Finalizing` | `begin_finalize()` | No children exist | **Fast path**: skip drain phase |
| R4 | `Draining` | `Finalizing` | All children completed | child_task_count==0; all child regions Closed | Finalizer execution begins (LIFO) |
| R5 | `Finalizing` | `Closed` | Finalization complete | All preconditions met (see 3.4) | Arena reclaimed; close waiters notified |

### 3.3 Forbidden Transitions (12 Must-Fail)

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

All must hold before transition to Closed:

1. All child tasks in terminal state (`Completed`)
2. All child sub-regions in `Closed` state
3. All obligations resolved (`Committed`, `Aborted`) or deterministically `Leaked`
4. Cleanup stack fully drained (LIFO execution)
5. Ghost linearity monitor confirms zero outstanding obligation count (debug)

Violation keeps region in `Finalizing` with `ASX_E_UNRESOLVED_OBLIGATIONS` or `ASX_E_INCOMPLETE_CHILDREN`.

### 3.5 Operations Gated by Region State

| Operation | Allowed States | Error if Wrong |
|-----------|---------------|----------------|
| Create child task | `Open`, `Finalizing` (finalizer tasks only) | `ASX_E_REGION_NOT_OPEN` / `ASX_E_ADMISSION_CLOSED` |
| Create child region | `Open` | `ASX_E_REGION_NOT_OPEN` |
| Create obligation | `Open` | `ASX_E_REGION_NOT_OPEN` |
| Resolve obligation | `Open`, `Closing`, `Draining`, `Finalizing` | N/A (always allowed pre-Closed) |
| Access arena | Not `Closed` | `ASX_E_REGION_CLOSED` |
| Query status | Any | Never fails |

**Edge case:** `Finalizing` allows task creation because finalizers may spawn work that must complete before region closure. Child region creation is NOT allowed during `Finalizing`.

---

## 4. Task Lifecycle

### 4.1 States

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Created` | `ASX_TASK_CREATED` | Allocated but not yet scheduled |
| `Running` | `ASX_TASK_RUNNING` | Actively being polled by scheduler |
| `CancelRequested` | `ASX_TASK_CANCEL_REQUESTED` | Cancel signal received; task not yet observing |
| `Cancelling` | `ASX_TASK_CANCELLING` | Task observed cancel; performing cleanup |
| `Finalizing` | `ASX_TASK_FINALIZING` | Cleanup complete; final outcome being determined |
| `Completed` | `ASX_TASK_COMPLETED` | Terminal; outcome determined, resources released |

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
| T1 | `Created` | `Running` | `start_running()` | Task poll function invoked |
| T2 | `Created` | `CancelRequested` | `request_cancel()` | Cancel before first poll; `cancel_epoch` incremented |
| T3 | `Created` | `Completed` | `complete(outcome)` | Error/panic at spawn time |
| T4 | `Running` | `CancelRequested` | `request_cancel()` | Cancel signal delivered; `cancel_epoch` incremented |
| T5 | `Running` | `Completed` | `complete(outcome)` | Normal completion, error, or panic |
| T6 | `CancelRequested` | `CancelRequested` | `request_cancel()` | **Strengthening**: severity raised, budget combined; returns false |
| T7 | `CancelRequested` | `Cancelling` | `acknowledge_cancel()` | Cleanup budget applied; `polls_remaining` set |
| T8 | `CancelRequested` | `Completed` | `complete(outcome)` | Natural completion preserves natural outcome |
| T9 | `Cancelling` | `Cancelling` | `request_cancel()` | **Strengthening**: reason/budget updated; returns false |
| T10 | `Cancelling` | `Finalizing` | `cleanup_done()` | User cleanup code finished |
| T11 | `Cancelling` | `Completed` | `complete(outcome)` | Error/panic during cleanup |
| T12 | `Finalizing` | `Finalizing` | `request_cancel()` | **Strengthening**: reason/budget updated; returns false |
| T13 | `Finalizing` | `Completed` | `finalize_done()` | Produces `CancelWitness`; outcome is `Cancelled(reason)` |

**Strengthening (T6, T9, T12):** Not state changes. Cancel reason strengthened via `reason.strengthen()` (higher severity wins, deterministic tie-break). Cleanup budget combined via `budget.combine()` (min on quota, max on priority). Return value is `false`.

### 4.4 Forbidden Transitions (23 Must-Fail)

10 backward + 6 skipped + 7 from-terminal = 23 forbidden. All return `ASX_E_INVALID_TRANSITION`. In debug builds, `debug_assert!` fires. In release builds, methods return `false`/`None`.

### 4.5 Cancellation Observation Rules

1. Cancel signals delivered by setting `CancelRequested` on the task
2. Task does **not** see cancel until it calls `asx_checkpoint()` or `asx_is_cancelled()`
3. Between `CancelRequested` and observation, task continues normal poll logic
4. Once observed, task **must** transition to `Cancelling` within bounded cleanup budget
5. If cleanup budget exceeded, scheduler may force-complete with `Cancelled` outcome
6. **A task that completes naturally while in `CancelRequested` produces its natural outcome, NOT `Cancelled`**

### 4.6 Poll Contract

| Poll Return | Meaning | State Effect |
|------------|---------|-------------|
| `ASX_POLL_PENDING` | Yielded; needs reschedule | Remains in current state |
| `ASX_POLL_READY` | Completed work | Toward `Completed` |
| `ASX_POLL_ERROR` | Fatal error | Records `Err` outcome; toward `Completed` |

---

## 5. Obligation Lifecycle

### 5.1 States

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Reserved` | `ASX_OBLIGATION_RESERVED` | Resource/promise reserved; not yet fulfilled |
| `Committed` | `ASX_OBLIGATION_COMMITTED` | Terminal: fulfilled successfully |
| `Aborted` | `ASX_OBLIGATION_ABORTED` | Terminal: explicitly cancelled/rolled back |
| `Leaked` | `ASX_OBLIGATION_LEAKED` | Terminal error: unresolved before region finalization |

### 5.2 Legal Transitions

| From | To | Trigger | Postconditions |
|------|----|---------|----------------|
| `Reserved` | `Committed` | `asx_obligation_commit()` | Fulfilled; linearity bit cleared |
| `Reserved` | `Aborted` | `asx_obligation_abort()` | Released; linearity bit cleared |
| `Reserved` | `Leaked` | Region finalization with unresolved | Leak reported; error logged |

### 5.3 Forbidden Transitions (Must-Fail)

| From | To | Error |
|------|----|-------|
| `Committed` | any | `ASX_E_OBLIGATION_ALREADY_RESOLVED` |
| `Aborted` | any | `ASX_E_OBLIGATION_ALREADY_RESOLVED` |
| `Leaked` | any | `ASX_E_OBLIGATION_LEAKED` |
| `Reserved` | `Reserved` | `ASX_E_INVALID_TRANSITION` |

Double-resolution is **not** idempotent by design; it indicates a logic error.

### 5.4 Linearity Enforcement

1. `reserve` sets bit in linearity ledger
2. `commit`/`abort` clears bit
3. Region `Finalizing` checks `popcnt == 0`
4. Non-zero triggers leak-report path: each unresolved obligation transitions to `Leaked`

### 5.5 Region-Obligation Interaction

| Region State | Creation Allowed | Resolution Allowed |
|-------------|-----------------|-------------------|
| `Open` | Yes | Yes |
| `Closing` | No | Yes |
| `Draining` | No | Yes |
| `Finalizing` | No | Yes (leak detection active) |
| `Closed` | No | No (all resolved or leaked) |

---

## 6. Cancellation Protocol

### 6.1 Phases

| Phase | Rank | Enum Value | Description |
|-------|------|-----------|-------------|
| `Requested` | 0 | `ASX_CANCEL_REQUESTED` | Cancel requested; not yet observed |
| `Cancelling` | 1 | `ASX_CANCEL_CANCELLING` | Observed; cleanup in progress |
| `Finalizing` | 2 | `ASX_CANCEL_FINALIZING` | Cleanup complete; final outcome determination |
| `Completed` | 3 | `ASX_CANCEL_COMPLETED` | Protocol complete; outcome is `Cancelled` |

No `None` phase. Absence of cancellation = no `CancelWitness` exists for the task.

### 6.2 Legal Phase Transitions

Valid when `next.phase.rank() >= prev.phase.rank()` (monotone non-decreasing). Same-phase transitions are strengthening (idempotent at phase level). Skip transitions (e.g., `Requested -> Completed`) are allowed because rank is non-decreasing.

### 6.3 Forbidden Phase Transitions

Any `next.phase.rank() < prev.phase.rank()` produces `ASX_E_WITNESS_PHASE_REGRESSION`. Any `next.reason.severity() < prev.reason.severity()` produces `ASX_E_WITNESS_REASON_WEAKENED`.

### 6.4 Cancellation Kinds (11 Variants)

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

**Severity rules:**
- Higher severity always wins in `strengthen()` operations
- Equal severity: deterministic tie-break by earlier timestamp, then lexicographically smaller message
- Severity is monotone non-decreasing across witness transitions

**Budget combination (min-plus algebra):**
- `combined_quota = min(quota1, quota2)` — tighter quota wins
- `combined_priority = max(priority1, priority2)` — higher urgency wins
- Combining never widens the cleanup allowance

### 6.5 Witness Structure

| Field | Type | Description |
|-------|------|-------------|
| `task_id` | `asx_task_id` | The task being cancelled |
| `region_id` | `asx_region_id` | The owning region |
| `epoch` | `uint64_t` | Monotonically increasing cancel epoch |
| `phase` | `asx_cancel_phase` | Current protocol phase |
| `reason` | `asx_cancel_reason` | Full reason with kind, origin, timestamp, cause chain |

### 6.6 Witness Validation Rules

A transition from `prev` to `next` is valid when ALL hold:

| Rule | Check | Error on Violation |
|------|-------|-------------------|
| Same task | `prev.task_id == next.task_id` | `ASX_E_WITNESS_TASK_MISMATCH` |
| Same region | `prev.region_id == next.region_id` | `ASX_E_WITNESS_REGION_MISMATCH` |
| Same epoch | `prev.epoch == next.epoch` | `ASX_E_WITNESS_EPOCH_MISMATCH` |
| Phase monotone | `next.phase.rank >= prev.phase.rank` | `ASX_E_WITNESS_PHASE_REGRESSION` |
| Severity monotone | `next.reason.severity >= prev.reason.severity` | `ASX_E_WITNESS_REASON_WEAKENED` |

### 6.7 Attribution Chain

`CancelReason` carries a recursive cause chain:

| Field | Type | Description |
|-------|------|-------------|
| `kind` | `asx_cancel_kind` | The cancellation kind |
| `origin_region` | `asx_region_id` | Region that initiated |
| `origin_task` | `asx_task_id` (optional) | Task that initiated |
| `timestamp` | `asx_time` | When requested |
| `message` | `const char*` (optional) | Human-readable message |
| `cause` | `asx_cancel_reason*` (optional) | Parent cause in chain |
| `truncated` | `bool` | Whether chain was truncated |

**Chain limits (configurable):** `max_chain_depth` default 16; `max_chain_memory` default 4096 bytes (~88 bytes per level). Truncated chains set `truncated=true`.

### 6.8 Propagation Rules

1. Region close propagates cancel to all child tasks
2. Propagation is **depth-first** through region tree
3. Each child receives its own witness with attribution chain extended
4. Cancel does NOT propagate to sibling regions (parent-to-child only)
5. Already-cancelled tasks: idempotent delivery (strengthening only)
6. Propagation is deterministic: same tree + same trigger = same order

---

## 7. Outcome Severity Lattice

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

```
join(a, b) = max(severity(a), severity(b))
```

Properties:
- **Commutative:** `join(a, b) == join(b, a)`
- **Associative:** `join(join(a, b), c) == join(a, join(b, c))`
- **Idempotent:** `join(a, a) == a`
- **Identity:** `Ok` — `join(Ok, x) == x`
- **Absorbing:** `Panicked` — `join(Panicked, x) == Panicked`
- **Equal-severity tie:** left-biased at payload level

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
region_outcome = fold(join, Ok, [child_1_outcome, ..., child_n_outcome])
```

Empty region outcome = `Ok`.

---

## 8. Budget Algebra

### 8.1 Budget Carrier

```
asx_budget = (deadline, poll_quota, cost_quota, priority)
```

### 8.2 Meet/Combine (Tightening)

Componentwise tightening:
- `deadline = min(finite a, finite b)` — earliest finite deadline wins
- `poll_quota = min(a, b)` — tighter quota wins
- `cost_quota = min(finite a, finite b)` — tighter quota wins
- `priority` follows canonical tightening order

### 8.3 Identities

- `INFINITE` is the identity for meet/tightening (combining with INFINITE preserves the other)
- `ZERO` is absorbing/tightest bound (combining with ZERO yields ZERO)

### 8.4 Exhaustion Predicates

| Predicate | Trigger | Behavior |
|-----------|---------|----------|
| Poll quota depletion | `polls_remaining == 0` | Cancel with `PollQuota` kind |
| Cost quota depletion | `cost_remaining == 0` | Cancel with `CostBudget` kind |
| Deadline expiration | `now >= deadline` | Cancel with `Deadline` kind |

### 8.5 Consume Semantics

Consumption is **failure-atomic**:
- No underflow behavior
- Insufficient quota returns explicit failure without partial mutation
- Exhaustion paths map to explicit cancellation reasons and diagnostics

**Example — poll quota consume:**
```
consume_poll(budget) -> ASX_OK | ASX_E_BUDGET_EXHAUSTED
  if budget.poll_quota == 0: return ASX_E_BUDGET_EXHAUSTED  // no mutation
  budget.poll_quota -= 1
  return ASX_OK
```

---

## 9. MPSC Channel Semantics

### 9.1 Channel State Model

Implicit state encoding via atomic fields (no explicit state enum):

| Implicit State | Condition | Description |
|----------------|-----------|-------------|
| **Open** | `receiver_dropped == false` AND `sender_count > 0` | Both sides active |
| **Half-Closed (Rx)** | `receiver_dropped == true` AND `sender_count > 0` | Receiver dropped; senders observe disconnect |
| **Half-Closed (Tx)** | `receiver_dropped == false` AND `sender_count == 0` | All senders dropped; receiver drains |
| **Closed** | `receiver_dropped == true` AND `sender_count == 0` | Both sides gone |

State transitions are **monotone**: `receiver_dropped` transitions `false -> true` exactly once; `sender_count` only decrements.

### 9.2 Capacity Model

Capacity is **fixed at creation** and never changes:

```
channel(capacity) where capacity > 0  // panics on zero
used_slots = queue.len() + reserved
INVARIANT: used_slots <= capacity
```

Both queued messages AND reserved (uncommitted) permits consume capacity.

**Rust evidence:** `mpsc.rs:140-147` (`used_slots`, `has_capacity`), `mpsc.rs:161-175` (factory)

### 9.3 Two-Phase Send Protocol

#### Phase 1: Reserve

```
reserve(cx) -> Poll<Result<SendPermit, SendError<()>>>
```

Preconditions checked in order:
1. **Cancellation checkpoint:** if `cx.checkpoint().is_err()` -> `SendError::Cancelled(())`
2. **Receiver alive:** if `receiver_dropped == true` -> `SendError::Disconnected(())`
3. **FIFO fairness:** if waiter queue non-empty AND caller is not head -> `Poll::Pending`
4. **Capacity:** if `used_slots < capacity` -> increment `reserved`, return `SendPermit`
5. **Otherwise:** register in waiter queue with monotonic ID, return `Poll::Pending`

**Rust evidence:** `mpsc.rs:337-407`

#### Phase 2a: Send (Commit)

```
SendPermit::send(self, value: T)       // infallible after reserve
SendPermit::try_send(self, value: T)   // fails only if receiver dropped post-reserve
```

Marks permit consumed, decrements `reserved`, pushes value to back of queue, wakes receiver.

**Rust evidence:** `mpsc.rs:545-585`

#### Phase 2b: Abort (Rollback)

```
SendPermit::abort(self)
```

Marks permit consumed, decrements `reserved`, wakes next sender in waiter queue (cascade).

**Rust evidence:** `mpsc.rs:588-602`

#### RAII Drop Safety

If `SendPermit` dropped without `send()` or `abort()`, the `Drop` impl runs abort logic:

```
Drop for SendPermit: if !self.sent { reserved -= 1; cascade_wake_next() }
```

**Rust evidence:** `mpsc.rs:605-623`

### 9.4 Backpressure and Waiter Queue

When channel is full (`used_slots >= capacity`):
1. `reserve()` returns `Poll::Pending`
2. Sender registered in `send_wakers` queue with monotonic waiter ID
3. Queue is `VecDeque<SendWaiter>` — strict FIFO ordering

**Release triggers:**
- Receiver consumes a message (`queue.pop_front()`)
- Permit dropped/aborted (decrements `reserved`)
- Both trigger cascade wake of next sender

**FIFO enforcement:** `try_reserve()` returns `Full` when waiters exist, even if capacity is available. Prevents queue-jumping.

**Rust evidence:** `mpsc.rs:214-225` (try_reserve FIFO check), `mpsc.rs:382-407` (waiter registration)

### 9.5 Eviction Mode

```
send_evict_oldest(value: T) -> Result<Option<T>, SendError<T>>
```

| Condition | Result |
|-----------|--------|
| Capacity available | `Ok(None)` — normal send |
| Queue has committed entries | `Ok(Some(evicted))` — oldest committed entry evicted |
| All capacity is reserved (no committed entries) | `Err(Full(value))` — cannot evict reserved slots |
| Receiver dropped | `Err(Disconnected(value))` |

**Rust evidence:** `mpsc.rs:281-321`

### 9.6 Receive Semantics

```
recv(cx) -> Poll<Result<T, RecvError>>
try_recv() -> Result<T, RecvError>
```

Order:
1. **Cancellation checkpoint:** if `cx.checkpoint().is_err()` -> `RecvError::Cancelled`
2. **Queue non-empty:** `queue.pop_front()` -> `Ok(value)` (FIFO)
3. **All senders dropped AND queue empty:** `RecvError::Disconnected`
4. **Otherwise:** register waker, return `Poll::Pending`

**Cancel safety:** Cancelled receive does NOT consume a message. Message remains for next receive.

**Drain-after-sender-drop:** Receiver can drain remaining messages after all senders drop. Returns `Ok(value)` until queue empty, then `Disconnected`.

### 9.7 Error Taxonomy

#### Send Errors

| Error | Condition | Carries Value |
|-------|-----------|---------------|
| `SendError::Disconnected(T)` | Receiver dropped | Yes |
| `SendError::Cancelled(T)` | Cancellation checkpoint | Yes |
| `SendError::Full(T)` | Channel full (try_reserve/try_send) | Yes |

#### Receive Errors

| Error | Condition |
|-------|-----------|
| `RecvError::Disconnected` | All senders dropped AND queue empty |
| `RecvError::Cancelled` | Cancellation checkpoint |
| `RecvError::Empty` | Queue empty (try_recv only), senders alive |

### 9.8 Cancellation Interaction

| Operation | Cancel Effect |
|-----------|--------------|
| `reserve()` poll | Returns `Cancelled`, no capacity consumed, waiter removed |
| `recv()` poll | Returns `Cancelled`, no message consumed |
| `SendPermit::send()` | Not checked (already committed) |
| `SendPermit::abort()` | Not checked (already releasing) |

Waiter cleanup on cancel: `Reserve` future drop removes from `send_wakers` by ID and cascades wake.

### 9.9 Obligation Integration (Session Layer)

| Component | Base Channel | Session-Tracked |
|-----------|-------------|-----------------|
| Sender | `Sender<T>` | `TrackedSender<T>` |
| Permit | `SendPermit<T>` | `TrackedPermit<T>` (contains `ObligationToken<SendPermit>`) |
| Send result | `()` | `CommittedProof<SendPermit>` |
| Abort result | `()` | `AbortedProof<SendPermit>` |
| Leak behavior | Capacity freed silently | **PANIC**: "OBLIGATION TOKEN LEAKED: SendPermit" |

`#[must_use]` on `TrackedPermit` — compiler warning if not consumed.

### 9.10 Close/Drain Semantics

**Receiver Drop (Channel Close):**
1. Sets `receiver_dropped = true` (monotone)
2. Drains queue via `mem::take()` — pending messages DROPPED (not delivered)
3. Clears recv waker
4. Wakes ALL waiting senders (they observe disconnect on next poll)
5. Items dropped outside lock (prevents deadlock if `T::drop` touches channel)

**Last Sender Drop:**
1. Decrements `sender_count` to 0
2. Wakes receiver (will return `Disconnected` after draining remaining queue)

### 9.11 Ordering Guarantees

| Dimension | Ordering | Mechanism |
|-----------|----------|-----------|
| Message delivery | FIFO | `VecDeque`: `push_back()` / `pop_front()` |
| Waiter scheduling | FIFO | Monotonic waiter ID, pop-from-front |
| Cascade wake | FIFO | Head of waiter queue woken first |
| try_reserve fairness | Strict FIFO | Returns `Full` when waiters queued |

---

## 10. Timer Wheel Semantics

### 10.1 4-Level Hierarchical Wheel

| Level | Slots | Resolution | Range |
|-------|-------|------------|-------|
| 0 | 256 | 1 ms | 256 ms |
| 1 | 256 | 256 ms | 65.536 s |
| 2 | 256 | 65.536 s | ~16.78 min |
| 3 | 256 | ~4295 s | ~37.2 hours |
| Overflow | BinaryHeap | N/A | Beyond wheel range (>24h default) |

**Slot assignment:** `slot = (deadline_tick % 256)`

**Bitmap occupation:** Each level has 4 x u64 bitmap words for O(1) skip of empty slots.

**Rust evidence:** `wheel.rs:43-352`

### 10.2 Timer Entry Structure

```
TimerEntry {
    deadline: Time,      // When to fire
    waker: Waker,        // What to wake
    id: u64,             // Unique timer ID
    generation: u64,     // Use-after-cancel prevention
}
```

Active timers tracked in `HashMap<id, generation>` for O(1) cancel validation.

### 10.3 Insert Semantics

```
try_register(deadline, waker) -> Result<TimerHandle, TimerDurationExceeded>
```

**Placement logic:**
1. If `deadline <= current_time`: pushed to `ready` queue (immediately expired)
2. If `delta >= max_wheel_duration`: pushed to overflow `BinaryHeap`
3. Otherwise: placed in appropriate level slot based on delta magnitude

**Duplicate handling:** No deduplication. Multiple timers with same deadline stored in insertion order in same slot Vec.

**Validation:** Returns `TimerDurationExceeded` if duration exceeds configured max (default 7 days).

**Generation tracking:** Each registration allocates unique `(id, generation)` pair. Both wrap on u64 overflow using wrapping arithmetic.

**Configuration:**

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `max_wheel_duration` | 24 hours | Max for wheel placement (beyond = overflow heap) |
| `max_timer_duration` | 7 days | Max accepted by `try_register()` |

### 10.4 Fire Semantics

```
collect_expired(now) -> WakerBatch
```

Process:
1. **Advance:** Compute `target_tick`, advance cursor through Level 0 slots
2. **Cascade:** When Level N cursor wraps to 0, advance Level N+1 cursor and re-insert entries from higher level into lower levels
3. **Generation check:** Dead entries (generation mismatch) silently dropped during cascade and drain
4. **Collect:** All live entries with `deadline <= now` added to waker batch
5. **Coalescing** (optional): Timers within coalescing window promoted if group threshold met

**Fire criterion:** `entry.deadline <= now` (inclusive — fires at or after deadline)

### 10.5 Cancel Semantics (Lazy Deletion)

```
cancel(handle) -> bool
```

1. Check `active` map: if `active[handle.id] == handle.generation`, remove entry -> return `true`
2. Generation mismatch or missing: return `false`
3. **No physical removal** from wheel slots — entry remains but skipped at fire time via `is_live()` check
4. When all timers cancelled (`active` map empty): `purge_inactive_storage()` clears all slot vectors and bitmaps

### 10.6 Generation-Safe Handle Contract

```
TimerHandle { id: u64, generation: u64 }
```

- `id` wraps independently of `generation` (both use `wrapping_add(1)`)
- Cancel requires exact `(id, generation)` match
- After cancel, same `id` may be reused with different `generation` — old handles cannot cancel new timer
- Safe across u64 wrap: HashMap stores both old and new entries simultaneously

### 10.7 Timer Ordering Within Deadline

| Condition | Ordering | Mechanism |
|-----------|----------|-----------|
| Different deadlines | Deadline order (earliest first) | Wheel cursor advancement |
| Same deadline, same slot | Insertion order | `Vec::push()` appends; iteration preserves |
| Overflow heap | Deadline then generation | `BinaryHeap` with `Ord` on (deadline, generation) |

### 10.8 Coalescing

Optional window-based coalescing:
- Fires timers at a coalesced boundary when in-window ready count reaches `min_group_size`
- Deterministic for fixed inputs and threshold config

---

## 11. Deterministic Scheduler Semantics

### 11.1 Three-Lane Architecture

| Lane | Priority | Queue Type | Ordering |
|------|----------|------------|----------|
| **Cancel** | Highest | Lock-free `SegQueue` (global) + `BinaryHeap` (local) | Priority-ordered, FIFO within priority |
| **Timed** | Middle | `Mutex<TimedQueue>` (global) + `BinaryHeap` (local) | EDF (earliest deadline first), FIFO within deadline |
| **Ready** | Lowest | Lock-free `SegQueue` (global) + `BinaryHeap` (local) | Priority-ordered, FIFO within priority |

### 11.2 Governor Suggestions

| Suggestion | Lane Order | Effect |
|------------|------------|--------|
| `NoPreference` (default) | Cancel > Timed > Ready | Standard |
| `MeetDeadlines` | Timed > Cancel > Ready | Prioritize deadline tasks |
| `DrainObligations` | Cancel > Timed > Ready | Doubled cancel-streak limit |
| `DrainRegions` | Cancel > Timed > Ready | Doubled cancel-streak limit |

### 11.3 Scheduler Loop (6-Phase)

```
run_loop():
  while !shutdown:
    Phase 0: Process expired timers (fires wakers -> injects tasks)
    Phase 1: Global queues (cancel/timed per governor order)
    Phase 2: Local PriorityScheduler (cancel/timed lanes)
    Phase 3: Fast ready paths (lock-free global, local stack)
    Phase 3b: Local ready with RNG hint
    Phase 4: Work stealing (deterministic RNG-seeded target selection)
    Phase 5: Fallback cancel (when cancel-streak limit exceeded)
    Phase 6: Backoff/Park (spin -> yield -> park with timeout)
```

### 11.4 Entry Ordering

#### Cancel/Ready Lanes (SchedulerEntry)
```
Compare:
  1. Higher priority first (u8, higher value = more important)
  2. Earlier generation first (lower number = earlier insertion)
```

#### Timed Lane (TimedEntry)
```
Compare:
  1. Earlier deadline first
  2. Earlier generation first (FIFO for equal deadlines)
```

### 11.5 Generation Counter (FIFO Guarantee)

Each `PriorityScheduler` maintains monotone `next_generation: u64`. Every entry insertion increments and records generation, ensuring stable FIFO ordering for equal priorities/deadlines.

### 11.6 RNG-Based Deterministic Tie-Breaking

For local ready lane, when multiple entries share same priority:
1. Collect all entries with equal priority from heap
2. Compute `idx = rng_hint % count`
3. Select entry at `idx`, push remaining back

RNG seeded deterministically per worker: `DetRng::new(worker_id as u64)`

### 11.7 Work Stealing

Deterministic work stealing scans other workers in RNG-seeded circular order:

```
steal_task(stealers, rng):
  start = rng.next_usize(stealers.len())
  for i in 0..stealers.len():
    idx = (start + i) % stealers.len()
    if stealers[idx].steal() -> return task
  return None
```

### 11.8 Cancel-Streak Fairness

Limits consecutive cancel-lane dispatches:
- **Base limit:** 16 (configurable)
- **Doubled under:** `DrainObligations` or `DrainRegions` governor suggestions
- When limit exceeded: skip cancel lane for one iteration, dispatch from timed/ready
- Streak resets to 0 on park

Prevents cancellation storms from starving normal tasks.

### 11.9 Fairness Certificate

```
FairnessPreemptionCertificate {
    cancel_dispatches: u64,
    timed_dispatches: u64,
    ready_dispatches: u64,
    fallback_cancel_dispatches: u64,
    base_limit_exceedances: u64,
    effective_limit_exceedances: u64,
    max_effective_limit_observed: usize,
}
```

**Witness hash:** `DetHasher` over dispatch counts produces deterministic u64. Same trace -> same hash.

### 11.10 Backoff/Park

| Phase | Iterations | Action |
|-------|------------|--------|
| Spin | 8 | Busy-wait (check queues between spins) |
| Yield | 2 | Thread yield (OS scheduler cooperative) |
| Park | 1 | Conditional park with timeout = `min(next_timer_deadline, next_timed_entry_deadline)` |

### 11.11 Queue Data Structures

| Structure | Location | Purpose |
|-----------|----------|---------|
| `BinaryHeap<SchedulerEntry>` | Local cancel/ready | Priority + generation ordering |
| `BinaryHeap<TimedEntry>` | Local timed | EDF + generation ordering |
| `SegQueue<PriorityTask>` | Global cancel/ready | Lock-free unbounded FIFO injection |
| `Mutex<TimedQueue>` | Global timed | EDF heap with generation counter |
| `IntrusiveStack` | Local fast ready | LIFO push/pop (owner), FIFO steal (thief) |
| `DetHashSet<TaskId>` | Membership tracking | O(1) "is scheduled?" check |

---

## 12. Cross-Domain Interactions

### 12.1 Channel <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled while reserve pending | `Reserve` future polls `checkpoint()` -> `SendError::Cancelled`. Waiter removed. No capacity consumed. |
| Task cancelled while recv pending | `poll_recv()` polls `checkpoint()` -> `RecvError::Cancelled`. No message consumed. |
| Region closing with channel open | Channel continues operating. Channel closure is independent of region lifecycle. |
| Obligation leak on permit drop | `TrackedPermit` panics if dropped without send/abort. Base `SendPermit` silently aborts. |

### 12.2 Timer <-> Cancellation

| Interaction | Behavior |
|-------------|----------|
| Task cancelled with pending timer | Higher layer cancels timer via `cancel(handle)`. Entry marked dead in `active` map. |
| Timer fires for cancelled task | Waker called, but task's cancel checkpoint triggers on next poll. |
| Region closing with timers pending | Timers fire normally during drain phase. Cleanup timers may be registered during Finalizing. |

### 12.3 Timer <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Timer expires | `process_timers()` in Phase 0. Expired wakers called. Woken tasks injected into scheduler lanes. |
| No timers pending | Scheduler park timeout = infinity (waits for external wake). |
| Next timer deadline | Scheduler park timeout = `min(next_deadline - now)`. |

### 12.4 Channel <-> Scheduler

| Interaction | Behavior |
|-------------|----------|
| Channel send wakes receiver | Receiver's waker called. Task injected into ready lane. |
| Channel recv wakes sender | Next sender in waiter queue woken (cascade). Task injected into ready lane. |
| Backpressure blocks sender | Sender task parks until woken by receiver consumption or abort cascade. |

### 12.5 Channel <-> Obligation (Session Layer)

| Interaction | Behavior |
|-------------|----------|
| Reserve creates obligation | `TrackedPermit` contains `ObligationToken<SendPermit>` in Reserved state |
| Send resolves obligation | `send()` transitions Reserved -> Committed, returns `CommittedProof` |
| Abort resolves obligation | `abort()` transitions Reserved -> Aborted, returns `AbortedProof` |
| Drop without resolution | **PANIC**: obligation leaked. Linearity violation. |

### 12.6 Channel/Timer <-> Outcome/Budget

| Interaction | Behavior |
|-------------|----------|
| Channel send fails (Disconnected) | Produces `Err` outcome at task level |
| Channel send cancelled | Produces `Cancelled` outcome at task level |
| Timer fires deadline miss | May trigger cancellation with `Deadline` cancel kind |
| Budget exhaustion during channel ops | Cleanup budget governs pending operations; exhausted -> remaining aborted |

---

## 13. Quiescence and Finalization Invariants

### 13.1 Quiescence Definition

Runtime is quiescent when ALL hold simultaneously:

1. `active_task_count == 0`
2. `reserved_obligation_count == 0`
3. All regions `Closed`
4. Timer structures drained / no pending expirations
5. Channel structures drained / no deliverable buffered work

### 13.2 Quiescence Checks

| Check | Condition | Error if Violated |
|-------|-----------|-------------------|
| Task quiescence | `active_task_count == 0` | `ASX_E_TASKS_STILL_ACTIVE` |
| Obligation quiescence | `reserved_obligation_count == 0` | `ASX_E_OBLIGATIONS_UNRESOLVED` |
| Region quiescence | All regions `Closed` | `ASX_E_REGIONS_NOT_CLOSED` |
| Timer quiescence | Timer wheel empty | `ASX_E_TIMERS_PENDING` |
| Channel quiescence | All channels drained | `ASX_E_CHANNEL_NOT_DRAINED` |

### 13.3 Close-Time Leak Behavior

If finalization encounters unresolved obligations:
- Unresolved obligations transition to `Leaked` deterministically
- Leak is explicitly surfaced/logged (never silent)
- Close path remains auditable and deterministic

### 13.4 Cleanup Budget Under Cancellation

- Cleanup budget activates when task enters `Cancelling`
- Repeated cancellation only strengthens constraints (never widens)
- Overrun produces deterministic force-complete with explicit metadata (never undefined stalling)

### 13.5 Runtime Shutdown Sequence

1. Shutdown initiates close on root region
2. Close cascades through region tree (depth-first)
3. Scheduler drives all tasks through cancel/complete paths
4. Timer wheel drained (expired fire, pending cancelled)
5. Channels drained (pending sends rejected or completed)
6. Runtime reports quiescence or failure diagnostic

---

## 14. Deterministic Tie-Break Contract

### 14.1 Complete Ordering Summary

| Domain | Primary Key | Secondary Key | Tertiary Key |
|--------|------------|---------------|--------------|
| Channel messages | FIFO (VecDeque) | — | — |
| Channel waiters | FIFO (monotonic ID) | — | — |
| Timer wheel (same slot) | Insertion order (Vec) | — | — |
| Timer overflow heap | Deadline | Generation (FIFO) | — |
| Scheduler cancel lane | Priority (u8, higher first) | Generation (FIFO) | — |
| Scheduler timed lane | Deadline (EDF) | Generation (FIFO) | — |
| Scheduler ready lane | Priority (u8, higher first) | Generation (FIFO) | — |
| Scheduler local ready | Priority | RNG % count (deterministic) | — |
| Work stealing | RNG-seeded circular scan | First available | — |
| Cancel propagation | Depth-first tree traversal | — | — |
| Event journal | `event_seq` (strictly monotonic) | — | — |

### 14.2 Determinism Invariants

1. **Same seed -> same execution order:** All RNG-dependent decisions use `DetRng` seeded from worker ID
2. **Same inputs -> same outputs:** No non-deterministic data structures in hot paths
3. **Generation monotonicity:** All FIFO ordering backed by monotone u64 counters
4. **Wrap safety:** u64 wrapping for IDs/generations safe due to HashMap-based tracking
5. **Witness reproducibility:** `FairnessPreemptionCertificate` hash deterministic over dispatch trace

### 14.3 Plan-Level Tie-Break Key

From `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`:

```
(lane_priority, logical_deadline, task_id, insertion_seq)
```

The C port uses this **4-component key** which is STRONGER than the Rust reference (Rust uses 2-component keys per domain). The C port adds `insertion_seq` to guarantee total ordering even for equal-priority equal-deadline tasks.

---

## 15. Forbidden Behavior Catalog

### 15.1 Critical Forbidden Behaviors

| ID | Behavior | Expected Result | Category |
|----|---------|-----------------|----------|
| FB-001 | Create child task in non-`Open` region | `ASX_E_REGION_NOT_OPEN` | region-gate |
| FB-002 | Create obligation in non-`Open` region | `ASX_E_REGION_NOT_OPEN` | region-gate |
| FB-003 | Double-commit obligation | `ASX_E_OBLIGATION_ALREADY_RESOLVED` | obligation-linearity |
| FB-004 | Double-abort obligation | `ASX_E_OBLIGATION_ALREADY_RESOLVED` | obligation-linearity |
| FB-005 | Commit then abort obligation | `ASX_E_OBLIGATION_ALREADY_RESOLVED` | obligation-linearity |
| FB-006 | Abort then commit obligation | `ASX_E_OBLIGATION_ALREADY_RESOLVED` | obligation-linearity |
| FB-007 | Backward state transition (any domain) | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-008 | Skip intermediate state in region close | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-009 | Skip intermediate state in task lifecycle | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-010 | Operate on stale/freed handle | `ASX_E_STALE_HANDLE` | handle-safety |
| FB-011 | Close region with unresolved obligations | Obligations leaked, `ASX_E_UNRESOLVED_OBLIGATIONS` logged | obligation-leak |
| FB-012 | Close region with active child tasks | Region blocks in `Draining` | region-drain |
| FB-013 | Cancel already-completed task | Idempotent no-op (not error) | cancel-idempotent |
| FB-014 | Access region arena after `Closed` | `ASX_E_REGION_CLOSED` | region-access |
| FB-015 | Non-deterministic behavior in deterministic mode | Digest mismatch by ghost monitor | determinism |

### 15.2 Resource Exhaustion Forbidden Behaviors

| ID | Behavior | Expected Result |
|----|---------|-----------------|
| FB-100 | Exceed max ready queue capacity | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) |
| FB-101 | Exceed max timer node count | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) |
| FB-102 | Exceed max cancel queue capacity | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) |
| FB-103 | Exceed runtime memory ceiling | `ASX_E_RESOURCE_EXHAUSTED` (failure-atomic, no corruption) |
| FB-104 | Exceed trace event capacity | `ASX_E_RESOURCE_EXHAUSTED` or ring-buffer overwrite (profile-dependent) |

### 15.3 Protocol Violation Forbidden Behaviors

| ID | Behavior | Expected Result |
|----|---------|-----------------|
| FB-200 | Yield across obligation boundary without copy | Ghost borrow ledger violation (debug) |
| FB-201 | Mutable access during shared borrow epoch | Ghost borrow ledger violation (debug) |
| FB-202 | Cross-thread access without transfer certificate | Ghost affinity violation (debug) |
| FB-203 | Non-checkpoint long-running loop in cancel path | CI lint flag |

### 15.4 Finalization Forbidden Behaviors

| ID | Behavior | Expected Result |
|----|---------|-----------------|
| FB-300 | Close with unresolved obligations without leak accounting | Must account for all |
| FB-301 | Complete close while child tasks/subregions non-terminal | Blocks in Draining |
| FB-302 | Quiescence success while timers/channels not drained | Must fail quiescence check |
| FB-303 | Phase regression during cancel/finalize | `ASX_E_INVALID_TRANSITION` |
| FB-304 | Silent cleanup-budget overrun | Must be explicit and deterministic |

---

## 16. Error Code Reference

| Error Code | Description |
|-----------|-------------|
| `ASX_E_INVALID_TRANSITION` | Attempted illegal state transition |
| `ASX_E_REGION_NOT_OPEN` | Operation requires region `Open` |
| `ASX_E_REGION_CLOSED` | Operation on `Closed` region |
| `ASX_E_ADMISSION_CLOSED` | Region not accepting new children |
| `ASX_E_OBLIGATION_ALREADY_RESOLVED` | Double-resolution attempt |
| `ASX_E_OBLIGATION_LEAKED` | Obligation reached `Leaked` state |
| `ASX_E_UNRESOLVED_OBLIGATIONS` | Finalization found unresolved obligations |
| `ASX_E_INCOMPLETE_CHILDREN` | Finalization found non-terminal children |
| `ASX_E_STALE_HANDLE` | Handle generation mismatch |
| `ASX_E_RESOURCE_EXHAUSTED` | Resource contract ceiling exceeded |
| `ASX_E_BUDGET_EXHAUSTED` | Budget quota depleted |
| `ASX_E_TASKS_STILL_ACTIVE` | Quiescence: active tasks remain |
| `ASX_E_OBLIGATIONS_UNRESOLVED` | Quiescence: obligations remain |
| `ASX_E_REGIONS_NOT_CLOSED` | Quiescence: regions not closed |
| `ASX_E_TIMERS_PENDING` | Quiescence: timers pending |
| `ASX_E_CHANNEL_NOT_DRAINED` | Quiescence: channel not drained |
| `ASX_E_WITNESS_TASK_MISMATCH` | Witness task ID mismatch |
| `ASX_E_WITNESS_REGION_MISMATCH` | Witness region ID mismatch |
| `ASX_E_WITNESS_EPOCH_MISMATCH` | Witness cancel epoch mismatch |
| `ASX_E_WITNESS_PHASE_REGRESSION` | Witness phase rank decreased |
| `ASX_E_WITNESS_REASON_WEAKENED` | Witness reason severity decreased |

---

## 17. Handle Encoding Reference

Bitmasked generational typestate handles: `[ 16-bit type_tag | 16-bit state_mask | 32-bit arena_index ]`

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

API endpoints perform `handle.state_mask & expected_mask` for O(1) state validation.

---

## 18. Invariant Schema

### 18.1 Channel Invariants

| ID | Invariant | Category |
|----|-----------|----------|
| `INV-CH-01` | `used_slots = queue.len() + reserved <= capacity` at all times | Capacity |
| `INV-CH-02` | Messages delivered in FIFO order | Ordering |
| `INV-CH-03` | Waiters serviced in FIFO order (monotonic ID) | Fairness |
| `INV-CH-04` | `try_reserve` returns `Full` when waiter queue non-empty | Fairness |
| `INV-CH-05` | Cancelled reserve does not consume capacity | Cancel safety |
| `INV-CH-06` | Cancelled recv does not consume message | Cancel safety |
| `INV-CH-07` | `receiver_dropped` monotone: `false -> true`, never reverses | State monotonicity |
| `INV-CH-08` | `sender_count` monotone: only decrements | State monotonicity |
| `INV-CH-09` | Permit Drop without send/abort decrements `reserved` and cascades | RAII safety |
| `INV-CH-10` | Session-tracked permit leaked -> panic | Obligation |

### 18.2 Timer Invariants

| ID | Invariant | Category |
|----|-----------|----------|
| `INV-TM-01` | Cancel requires exact `(id, generation)` match | Handle safety |
| `INV-TM-02` | Cancelled timers skipped at fire time via `is_live()` | Lazy deletion |
| `INV-TM-03` | Same-deadline timers fire in insertion order | Ordering |
| `INV-TM-04` | `try_register` rejects duration > `max_timer_duration` | Validation |
| `INV-TM-05` | All-cancelled triggers `purge_inactive_storage()` | Cleanup |
| `INV-TM-06` | u64 ID/generation wrap is collision-safe (HashMap) | Wrap safety |
| `INV-TM-07` | Dead entries dropped during cascade level promotion | Cascade safety |
| `INV-TM-08` | Fire criterion: `entry.deadline <= now` (inclusive) | Fire semantics |

### 18.3 Scheduler Invariants

| ID | Invariant | Category |
|----|-----------|----------|
| `INV-SC-01` | Cancel lane dispatched before timed before ready (default) | Lane priority |
| `INV-SC-02` | Equal-priority entries dispatched in generation (FIFO) order | Ordering |
| `INV-SC-03` | Equal-deadline timed entries dispatched in generation (FIFO) order | EDF ordering |
| `INV-SC-04` | Cancel-streak limit prevents cancellation starvation | Fairness |
| `INV-SC-05` | Same seed -> same work-steal scan order | Determinism |
| `INV-SC-06` | Identical traces produce identical fairness certificate hash | Replay |
| `INV-SC-07` | Phase 0 (timers) executes before task dispatch | Phase ordering |
| `INV-SC-08` | Governor suggestion affects lane order but not correctness | Safety |

---

## 19. Fixture Family Mapping

### 19.1 Region Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `region-lifecycle-001` | Open -> Closing -> Draining -> Finalizing -> Closed (happy path) |
| `region-lifecycle-002` | Open -> Closing with active children (drain required) |
| `region-lifecycle-003` | Nested region cascade close |
| `region-lifecycle-004` | Region close with unresolved obligations (leak detection) |
| `region-lifecycle-005` | Backward transition (Closing -> Open) must fail |
| `region-lifecycle-006` | Skip transition (Open -> Draining) must fail |
| `region-lifecycle-007` | Create task in non-Open region must fail |
| `region-lifecycle-008` | Arena access after Closed must fail |
| `region-lifecycle-009` | Empty region close (no children) fast path |
| `region-lifecycle-010` | Region close under resource exhaustion |

### 19.2 Task Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `task-lifecycle-001` | Created -> Running -> Completed(Ok) happy path |
| `task-lifecycle-002` | Full cancel: all phases traversed |
| `task-lifecycle-003` | CancelRequested -> Completed (natural outcome preserved) |
| `task-lifecycle-004` | Cancel before first poll |
| `task-lifecycle-005` | Error at spawn time |
| `task-lifecycle-006` | Cancel strengthen: multiple `request_cancel()` with increasing severity |
| `task-lifecycle-007` | Cancel strengthen budget combine: min-plus algebra |
| `task-lifecycle-008` | Backward transition must fail |
| `task-lifecycle-009` | Skip transition must fail |
| `task-lifecycle-010` | Completed is absorbing: all transitions rejected |
| `task-lifecycle-011` | acknowledge_cancel() returns reason and applies budget |
| `task-lifecycle-012` | finalize_done() produces CancelWitness |
| `task-lifecycle-013` | cancel_epoch increments only on first cancel |

### 19.3 Obligation Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `obligation-lifecycle-001` | Reserved -> Committed (happy path) |
| `obligation-lifecycle-002` | Reserved -> Aborted (rollback) |
| `obligation-lifecycle-003` | Reserved -> Leaked (region finalization) |
| `obligation-lifecycle-004` | Double-commit must fail |
| `obligation-lifecycle-005` | Double-abort must fail |
| `obligation-lifecycle-006` | Commit then abort must fail |
| `obligation-lifecycle-007` | Abort then commit must fail |
| `obligation-lifecycle-008` | Linearity ledger zero check at close |
| `obligation-lifecycle-009` | Multiple obligations in single region |
| `obligation-lifecycle-010` | Obligation in non-Open region must fail |

### 19.4 Cancellation Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `cancel-protocol-001` | Full cancel protocol happy path |
| `cancel-protocol-002` | Cancel propagation through region tree |
| `cancel-protocol-003` | Cancel with cleanup budget enforcement |
| `cancel-protocol-004` | Cancel budget exceeded (force-completion) |
| `cancel-protocol-005` | Multiple cancel on same target (idempotent) |
| `cancel-protocol-006` | Cancel attribution chain recorded |
| `cancel-protocol-007` | Each reason kind produces correct metadata |
| `cancel-protocol-008` | Backward cancel phase must fail |
| `cancel-protocol-009` | Nested cancel (cancel during cancelling) |
| `cancel-protocol-010` | Deterministic cancel propagation order |

### 19.5 Outcome Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `outcome-lattice-001` | Severity ordering |
| `outcome-lattice-002` | Join commutativity |
| `outcome-lattice-003` | Join associativity |
| `outcome-lattice-004` | Join identity (Ok) |
| `outcome-lattice-005` | Join absorbing (Panicked) |
| `outcome-lattice-006` | Region outcome aggregation |
| `outcome-lattice-007` | Empty region outcome = Ok |
| `outcome-join-left-bias-001` | Equal-severity left-bias |
| `outcome-join-order-002` | Join order independence |
| `outcome-join-top-003` | Top element (Panicked) absorbs |
| `outcome-cancel-strengthen-004` | Cancel outcome strengthening |

### 19.6 Budget Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `budget-meet-identity-010` | INFINITE is meet identity |
| `budget-meet-absorbing-011` | ZERO is absorbing |
| `budget-deadline-none-012` | None deadline handling |
| `budget-consume-poll-013` | Poll quota consumption |
| `budget-consume-cost-014` | Cost quota consumption |
| `budget-deadline-boundary-015` | Deadline boundary behavior |
| `runtime-pollquota-cancel-016` | Poll exhaustion triggers cancel |
| `cancel-cleanup-budget-017` | Cleanup budget under cancel |

### 19.7 Channel Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `ch-reserve-send-001` | Basic reserve -> send -> recv cycle |
| `ch-reserve-abort-001` | Reserve -> abort -> capacity freed |
| `ch-backpressure-001` | Full channel blocks sender, recv unblocks |
| `ch-fifo-001` | Multiple senders, FIFO delivery |
| `ch-fifo-fairness-001` | try_reserve respects waiter queue FIFO |
| `ch-cancel-reserve-001` | Cancel during pending reserve |
| `ch-cancel-recv-001` | Cancel during recv, message preserved |
| `ch-evict-001` | send_evict_oldest evicts front |
| `ch-evict-reserved-001` | Evict fails when all capacity reserved |
| `ch-close-drain-001` | Sender drop -> receiver drains remaining |
| `ch-close-wake-001` | Receiver drop -> all senders woken |
| `ch-obligation-leak-001` | TrackedPermit drop -> panic |
| `ch-obligation-commit-001` | TrackedPermit send -> CommittedProof |
| `ch-obligation-abort-001` | TrackedPermit abort -> AbortedProof |
| `ch-cascade-001` | Permit abort cascades wake |
| `ch-capacity-invariant-001` | Capacity invariant holds across all ops |
| `ch-capacity-zero-panic-001` | Zero capacity panics at creation |
| `channel-two-phase-001` | Reserve -> send -> recv (value delivered) |
| `channel-abort-release-002` | Reserve -> abort (capacity restored) |
| `channel-drop-permit-abort-003` | Drop = abort (no leaked reservation) |
| `channel-fifo-waiter-004` | Queued waiter + try_reserve -> Full |
| `channel-reserve-cancel-005` | Pending reserve then cancel (waiter removed) |
| `channel-recv-cancel-nonconsume-006` | Cancelled recv preserves message |
| `channel-evict-reserved-007` | Evict with all reserved -> Full |
| `channel-evict-committed-008` | Evict oldest committed |

### 19.8 Timer Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `tm-insert-fire-001` | Basic insert -> advance -> fire |
| `tm-same-deadline-001` | Same deadline, insertion order preserved |
| `tm-cancel-001` | Insert -> cancel -> not fired |
| `tm-cancel-stale-001` | Stale generation cancel rejected |
| `tm-cancel-reuse-001` | Same ID, different generation independent |
| `tm-duration-exceeded-001` | Beyond max duration -> error |
| `tm-cascade-001` | Level 0 wrap triggers Level 1 cascade |
| `tm-overflow-001` | Far-future in overflow heap |
| `tm-coalesce-001` | Coalescing window fires together |
| `tm-wrap-001` | ID/generation u64 wrap safe |
| `tm-purge-001` | All cancelled -> storage purged |
| `tm-immediate-001` | Deadline in past -> immediately ready |
| `timer-equal-deadline-order-010` | Insertion order preserved |
| `timer-cancel-generation-011` | Stale rejected; live cancellable |
| `timer-next-deadline-same-tick-012` | Same tick deadline preserved |
| `timer-overflow-promotion-013` | Promoted and fired when in range |
| `timer-coalescing-threshold-014` | Coalescing only when threshold met |

### 19.9 Scheduler Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `sc-lane-priority-001` | Cancel > timed > ready ordering |
| `sc-fifo-priority-001` | Equal priority -> FIFO dispatch |
| `sc-edf-001` | Earliest deadline first dispatch |
| `sc-edf-fifo-001` | Equal deadline -> FIFO |
| `sc-rng-tiebreak-001` | RNG tie-break deterministic with seed |
| `sc-steal-001` | Work stealing follows RNG circular order |
| `sc-cancel-streak-001` | Cancel streak triggers fairness yield |
| `sc-governor-meet-001` | MeetDeadlines reorders timed > cancel |
| `sc-governor-drain-001` | DrainObligations doubles cancel limit |
| `sc-certificate-001` | Identical traces -> identical hash |
| `sc-timer-phase0-001` | Expired timers before task dispatch |

### 19.10 Finalization Fixtures

| Fixture ID | Description |
|-----------|-------------|
| `finalization-quiescence-001` | Full close reaches quiescence |
| `finalization-quiescence-002` | Active task prevents quiescence |
| `finalization-leak-003` | Unresolved obligations leaked deterministically |
| `finalization-channel-drain-004` | Non-drained channel blocks quiescence |
| `finalization-timer-drain-005` | Pending timer blocks quiescence |
| `finalization-cancel-budget-006` | Cleanup budget overrun -> deterministic force-complete |
| `finalization-phase-regression-007` | Illegal phase regression rejected |

---

## 20. C Port Implementation Contract

### 20.1 Non-Negotiable Preservation Rules

1. State transition legality and monotonicity semantics remain intact
2. Cancellation strengthening and bounded cleanup remain deterministic
3. Obligation linearity remains explicit and auditable
4. Channel/timer ordering and stale-handle rejection remain deterministic
5. Exhaustion behavior is failure-atomic with explicit status surfaces
6. Profile differences affect operational limits only, not canonical outcomes
7. Scheduler dispatch ordering is replay-deterministic for same seed/input

### 20.2 C99 Usage Guidelines

C99 is allowed only where it improves correctness/clarity without changing semantics:
- Fixed-width integer typing for handle encodings and counters
- Designated initialization for explicit state structures
- Structured compile-time checks/macros for transition tables

No dependency-bearing substitutions in core runtime semantics.

### 20.3 C Port Strengthening

The C port introduces one semantic strengthening over Rust:

**4-component deterministic tie-break key:** `(lane_priority, logical_deadline, task_id, insertion_seq)` provides total ordering even for equal-priority equal-deadline tasks, whereas Rust uses 2-component keys per domain. This is STRONGER, not weaker, so semantic parity is preserved.

### 20.4 Pending Clarifications

- Budget/cancellation naming alignment follows canonical constructor surfaces (`deadline`, `poll_quota`, `cost_budget`) rather than stale comments in older text.
- Any semantic ambiguity discovered later must be resolved via explicit ruling plus fixture updates, not by implementation-side guesswork.

---

## Provenance Cross-Reference

### Source-to-Fixture Provenance Map Summary

33 provenance rows covering ~112 fixture IDs across all domains. Full mapping in `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`.

| Domain | Prov IDs | Rows | Fixture IDs | Status |
|--------|----------|------|-------------|--------|
| Baseline | BASELINE-001 | 1 | metadata | mapped |
| Outcome | OUTCOME-001..002 | 2 | ~10 | mapped |
| Budget | BUDGET-001..002 | 2 | ~8 | mapped |
| Region | REGION-001..002 | 2 | ~9 | mapped |
| Task | TASK-001 | 1 | ~16 | mapped |
| Obligation | OBLIGATION-001 | 1 | ~14 | mapped |
| Handle | HANDLE-001 | 1 | ~2 | mapped |
| Channel | CHANNEL-001..007 | 7 | ~18 | mapped |
| Timer | TIMER-001..008 | 8 | ~14 | mapped |
| Scheduler | SCHEDULER-001..006 | 6 | ~12 | mapped |
| Quiescence | QUIESCENCE-001 | 1 | ~6 | mapped |
| Finalization | FINALIZE-001 | 1 | ~3 | mapped |
| **Total** | | **33** | **~112** | |

### Parity Requirements

Every parity report record must include:
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

*This document is the canonical reference for all kernel semantics in the asx ANSI C port. All implementation must conform to these specifications. Deviations require explicit approval, fixture additions, and delta classification.*
