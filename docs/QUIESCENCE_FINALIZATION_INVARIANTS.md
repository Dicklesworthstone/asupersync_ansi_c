# Quiescence, Close, Cleanup, and Leak-Detection Invariants

> **Bead:** bd-296.18
> **Status:** Canonical spec artifact for implementation
> **Provenance:** Extracted from Rust source at `/dp/asupersync/src/record/region.rs`, `/dp/asupersync/src/runtime/state.rs`, `/dp/asupersync/src/obligation/{ledger,recovery,calm,lyapunov,no_leak_proof}.rs`, `/dp/asupersync/src/record/finalizer.rs`, `/dp/asupersync/src/cancel/{symbol_cancel,progress_certificate}.rs`, `/dp/asupersync/formal/lean/Asupersync.lean`, `/dp/asupersync/formal/tla/Asupersync.tla`
> **Cross-references:** `LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15), `EXISTING_ASUPERSYNC_STRUCTURE.md` (bd-296.16), `CHANNEL_TIMER_KERNEL_SEMANTICS.md` (bd-296.17)
> **Rust baseline:** `RUST_BASELINE_COMMIT` (to be pinned per Section 1.1)
> **Last verified:** 2026-02-27 by NobleCanyon (Claude Opus 4.6)
> **Prior version:** CopperSpire (2026-02-27) — superseded with full Rust source extraction

---

## Table of Contents

1. [Quiescence Definition](#1-quiescence-definition)
2. [Close State Machine and Advance Logic](#2-close-state-machine-and-advance-logic)
3. [Close Preconditions](#3-close-preconditions)
4. [Obligation Leak Detection](#4-obligation-leak-detection)
5. [Leak Response Policies](#5-leak-response-policies)
6. [Finalizer Execution Contract](#6-finalizer-execution-contract)
7. [Bounded Cleanup Under Cancellation](#7-bounded-cleanup-under-cancellation)
8. [Close Rejection and Admission Gating](#8-close-rejection-and-admission-gating)
9. [Timer/Channel Coupling to Quiescence](#9-timerchannel-coupling-to-quiescence)
10. [Deterministic Ordering Guarantees](#10-deterministic-ordering-guarantees)
11. [Convergence and Termination Proofs](#11-convergence-and-termination-proofs)
12. [Oracle System](#12-oracle-system)
13. [Forbidden Behavior Catalog](#13-forbidden-behavior-catalog)
14. [Fixture Family Mapping](#14-fixture-family-mapping)
15. [C Port Contract](#15-c-port-contract)

---

## 1. Quiescence Definition

### 1.1 Region-Level Quiescence (Four Conjuncts)

A region is quiescent if and only if ALL four conditions hold simultaneously:

| # | Condition | Runtime Check | Formal (Lean4) |
|---|-----------|---------------|-----------------|
| Q1 | All child tasks completed | `tasks.is_empty()` (completed tasks removed from region-local live tracking) | `allTasksCompleted s r.children` |
| Q2 | All child sub-regions closed | `children.is_empty()` (closed regions removed from tracking) | `allRegionsClosed s r.subregions` |
| Q3 | Obligation ledger empty | `pending_obligations == 0` | `r.ledger = []` |
| Q4 | All finalizers executed | `finalizers.is_empty()` | `r.finalizers = []` |

**Rust implementation** (`region.rs:692`):

```rust
pub fn is_quiescent(&self) -> bool {
    let inner = self.inner.read();
    inner.children.is_empty()
        && inner.tasks.is_empty()
        && inner.pending_obligations == 0
        && inner.finalizers.is_empty()
}
```

**C equivalent:**

```c
bool asx_region_is_quiescent(const asx_region *r) {
    return r->child_count == 0
        && r->task_count == 0
        && r->pending_obligations == 0
        && r->finalizer_count == 0;
}
```

Here `r->task_count` is the region-local live-task count used by quiescence checks. It is intentionally different from the runtime-wide `asx_runtime_task_count()` surface, which reports arena-resident task slots and may still include completed tasks until a reset/reclaim path clears the slot.

### 1.2 Runtime-Level Quiescence (Five Conjuncts)

The entire runtime is quiescent when all hold:

| # | Condition | Runtime Check |
|---|-----------|---------------|
| RQ1 | No live tasks | `live_task_count() == 0` |
| RQ2 | No pending obligations | `pending_obligation_count() == 0` |
| RQ3 | No active I/O sources | `io_driver.is_none_or(IoDriverHandle::is_empty)` (profile-dependent) |
| RQ4 | No pending finalizers | All regions: `finalizers_empty()` |
| RQ5 | All regions closed | All region records in `Closed` state |

Runtime-wide quiescence must not be inferred from `asx_runtime_task_count() == 0` or the other runtime count APIs. Those counters are slot-occupancy queries, not semantic live-work queries.

**Rust implementation** (`runtime/state.rs:1476`):

```rust
pub fn is_quiescent(&self) -> bool {
    let no_tasks = self.live_task_count() == 0;
    let no_obligations = self.pending_obligation_count() == 0;
    let no_io = self.io_driver.as_ref().is_none_or(IoDriverHandle::is_empty);
    let no_finalizers = self.regions.iter().all(|(_, r)| r.finalizers_empty());
    no_tasks && no_obligations && no_io && no_finalizers
}
```

### 1.3 Formal Proofs (Lean4)

The following theorems are proven in `/dp/asupersync/formal/lean/Asupersync.lean`:

| # | Theorem | Line | Statement |
|---|---------|------|-----------|
| T1 | `close_implies_quiescent` | 797 | Any closed region was quiescent at the moment of closing |
| T2 | `close_implies_ledger_empty` | 811 | Closed region has empty obligation ledger |
| T3 | `close_implies_finalizers_empty` | 823 | Closed region has no pending finalizers |
| T4 | `close_quiescence_decomposition` | 892 | Full decomposition into all four Q1-Q4 properties |
| T5 | `quiescent_tasks_completed` | 862 | Quiescent implies all children completed |
| T6 | `quiescent_subregions_closed` | 869 | Quiescent implies all subregions closed |
| T7 | `quiescent_no_obligations` | 876 | Quiescent implies empty ledger |
| T8 | `quiescent_no_finalizers` | 883 | Quiescent implies no finalizers |
| T9 | `obligation_in_ledger_blocks_close` | 942 | Any obligation in ledger prevents close |
| T10 | `close_children_exist_completed` | 1410 | Close implies every child task exists and is completed |
| T11 | `close_subregions_exist_closed` | 1449 | Close implies every subregion exists and is closed |
| T12 | `call_obligation_resolved_at_close` | 3821 | All call obligations resolved at close |

**Lean4 close precondition** (line 581):

```lean
| close ... (hState : region.state = RegionState.finalizing)
            (hFinalizers : region.finalizers = [])
            (hQuiescent : Quiescent s region)
```

### 1.4 TLA+ Invariant

```tla
CloseImpliesQuiescent ==
    \A r \in RegionIds :
        regionState[r] = "Closed" =>
            /\ \A t \in regionChildren[r] : taskState[t] = "Completed"
            /\ \A sr \in regionSubregions[r] : regionState[sr] = "Closed"
            /\ regionLedger[r] = {}
```

### 1.5 CALM Classification

Region close is classified as a **coordination-requiring (non-monotone) operation** because it is a quiescence barrier: it requires aggregation over an incomplete set to determine that all work has finished.

Other coordination-requiring operations in the finalization domain:
- `CancelDrain` — quiescence barrier
- `MarkLeaked` — depends on absence of resolution
- `BudgetCheck` — threshold on depleting counter

---

## 2. Close State Machine and Advance Logic

### 2.1 Region Close Progression

```
Open ──(begin_close)──> Closing ──(begin_drain)──> Draining ──(all children done)──> Finalizing ──(quiescent)──> Closed
                            |                                                              ^
                            └──(no children / begin_finalize)──────────────────────────────┘
```

State transitions use compare-and-swap for atomicity (`region.rs:249`):

```rust
pub fn transition(&self, from: RegionState, to: RegionState) -> bool {
    self.inner.compare_exchange(
        from.as_u8(), to.as_u8(),
        Ordering::AcqRel, Ordering::Acquire,
    ).is_ok()
}
```

### 2.2 State Predicates

| Predicate | True When | Used For |
|-----------|-----------|----------|
| `can_spawn()` | `Open` only | Task/region creation admission |
| `can_accept_work()` | `Open` or `Finalizing` | Finalizer-spawned cleanup tasks |
| `is_closing()` | `Closing`, `Draining`, `Finalizing` | Finalizer registration rejection |
| `is_terminal()` | `Closed` only | Arena reclamation eligibility |
| `is_draining()` | `Draining` only | Drain progress tracking |

### 2.3 Advance Region State Driver

The `advance_region_state()` function (`runtime/state.rs:2194`) is the main driver. It is **iterative** (not recursive) to bound stack depth:

```
WHILE current_region != NULL:
  MATCH state:
    Closing | Draining:
      IF no_children AND no_tasks:
        begin_finalize() → set state=Finalizing → reprocess
      ELIF has_children AND state==Closing:
        begin_drain() → set state=Draining

    Finalizing:
      1. Execute sync finalizers inline (LIFO order)
      2. IF async finalizer encountered:
           Schedule as masked task → STOP (resume on next advance)
      3. IF obligations remain AND all tasks terminal:
           collect_obligation_leaks() → handle_obligation_leaks()
      4. IF can_region_complete_close():
           complete_close():
             a. Verify is_quiescent() (double-guard)
             b. Transition Finalizing → Closed (CAS)
             c. Clear region heap (memory reclaim)
             d. Wake close waiters
             e. Remove from parent's child list
             f. Remove from region arena
             g. Set current = parent_id (cascade upward)
```

The **cascade** is critical: when a child region closes, it removes itself from the parent and attempts to advance the parent's state machine, enabling bottom-up close propagation without recursion.

### 2.4 Drain Phase Semantics

During `Draining`:
1. Cancel is issued to all child tasks with the region's cancel reason
2. Each child task receives a cleanup budget derived from the cancel reason severity
3. The scheduler drives cancelled tasks through their cleanup phase
4. As each task completes, it notifies the owning region
5. When `task_count == 0 && child_region_count == 0`, the region advances to `Finalizing`

### 2.5 Complete Close Implementation

From `region.rs:766`:

```rust
pub fn complete_close(&self) -> bool {
    if !self.is_quiescent() { return false; }
    let transitioned = self.state.transition(RegionState::Finalizing, RegionState::Closed);
    if transitioned {
        self.clear_heap();
        let waker = { ... notify.closed = true; notify.waker.take() };
        if let Some(waker) = waker { waker.wake(); }
    }
    transitioned
}
```

---

## 3. Close Preconditions

### 3.1 Finalizing-to-Closed Gate (Double Guard)

A region can transition from `Finalizing` to `Closed` only when ALL hold:

| # | Precondition | Check | Error if Violated |
|---|-------------|-------|-------------------|
| CP1 | State is `Finalizing` | `region.state() == Finalizing` | Not attempted |
| CP2 | All finalizers executed | `finalizers_empty()` | Region stays in `Finalizing` |
| CP3 | All tasks completed and removed | `task_count() == 0` | Region stays in `Finalizing` |
| CP4 | All obligations resolved | `pending_obligations() == 0` | Leak detection triggered |
| CP5 | All child regions closed and removed | `child_count() == 0` | Region stays in `Finalizing` |

**Guard 1 (runtime-level):** `can_region_complete_close()` (`state.rs:2131`) checks CP1-CP5 from the runtime perspective.

**Guard 2 (record-level):** `complete_close()` re-checks `is_quiescent()` inside the region record before executing the CAS transition.

Both guards must pass. This double-guard prevents TOCTOU races.

### 3.2 Precondition Failure Behavior

| Condition | Behavior |
|-----------|----------|
| Finalizers remain | Region stays in `Finalizing`; next scheduler tick re-attempts |
| Tasks remain | Region stays in `Finalizing`; tasks must complete first |
| Obligations remain (tasks done) | Leak detection fires; obligations marked `Leaked`; region retries close |
| Children remain | Region stays in `Finalizing`; child close must cascade first |

All failures are non-destructive: the region remains in `Finalizing` and will be re-attempted on the next advance cycle.

---

## 4. Obligation Leak Detection

### 4.1 Obligation Lifecycle Summary

```
Reserved ──(commit)──> Committed    (terminal)
    |
    ├──(abort)──> Aborted           (terminal)
    |
    └──(mark_leaked)──> Leaked      (terminal, detected by system)
```

The `ObligationToken` approximates linearity:
- `!Clone`, `!Copy` — cannot be duplicated
- `#[must_use]` — compiler warns if discarded without resolution
- Double-resolve (any terminal → any terminal) panics unconditionally

### 4.2 Detection Point 1: Task Completion (Non-Cancelled)

When a task completes without cancellation (`state.rs:1900`):

```
IF task completes AND NOT cancelled:
  leaks = collect_obligation_leaks_for_holder(task_id)
  IF leaks non-empty:
    handle_obligation_leaks(ObligationLeakError { ... })

// Always: abort orphaned obligations to prevent region-close deadlock
orphaned = obligations.sorted_pending_ids_for_holder(task_id)
FOR EACH ob_id IN orphaned:
  abort_obligation(ob_id, ObligationAbortReason::Cancel)
```

The always-abort path is critical: it prevents a region from being permanently stuck in `Finalizing` because a completed task left behind orphaned obligations.

### 4.3 Detection Point 2: Region Finalization

During `Finalizing`, if obligations remain and all tasks are terminal (`state.rs:2244`):

```
IF region.pending_obligations > 0:
  tasks_done = ALL task_ids_small().iter() → is_terminal()
  IF tasks_done:
    leaks = collect_obligation_leaks(filter: |record| record.region == region_id)
    IF leaks non-empty:
      handle_obligation_leaks(ObligationLeakError { ... })
```

### 4.4 Leak Detection Ordering

Obligations are stored in `BTreeMap<ObligationId, ObligationRecord>` (not `HashMap`). This ensures:
- Deterministic iteration order (ascending by obligation ID)
- Reproducible leak detection ordering across runs
- Required for lab-mode replay fidelity

C equivalent: sorted array or red-black tree keyed by obligation ID.

### 4.5 Reentrance Guard

A boolean `handling_leaks` flag prevents recursive leak detection when:

```
mark_obligation_leaked → advance_region_state → collect_obligation_leaks
```

If already handling leaks, the inner detection is suppressed.

### 4.6 Double-Panic Prevention

If `ObligationLeakResponse::Panic` is active but `std::thread::panicking()` is already true, the response is downgraded to `Log` to prevent process abort from double-panic.

C equivalent: check a thread-local `asx_panicking` flag before escalating.

---

## 5. Leak Response Policies

### 5.1 Response Enum

| Policy | Behavior | Use Case |
|--------|----------|----------|
| `Panic` | Panic immediately with diagnostic details | Development, strict correctness |
| `Log` | Log the leak and mark as `Leaked` | Production with monitoring |
| `Silent` | Suppress logging, still mark as `Leaked` | High-throughput paths |
| `Recover` | Auto-abort leaked obligations (→ `Aborted` not `Leaked`) | Graceful degradation |

### 5.2 Escalation Policy

```c
typedef struct {
    uint64_t threshold;                       /* leaks before escalation */
    asx_obligation_leak_response escalate_to; /* stricter response */
} asx_leak_escalation;
```

Example: Log first 3 leaks, then panic on the 4th.

### 5.3 Diagnostic Information

Each leak report includes:
- Obligation ID
- Owning task ID (if still known)
- Owning region ID
- Creation site metadata (file, line — debug builds only)
- Time since reservation
- Current region state

---

## 6. Finalizer Execution Contract

### 6.1 Finalizer Types

| Type | Execution Context | Masking |
|------|-------------------|---------|
| `Sync` | Runs inline on scheduler thread | N/A (no yield points) |
| `Async` | Runs as a masked task in the region | Cancel-masked (cannot be interrupted) |

From `record/finalizer.rs`:

```rust
pub enum Finalizer {
    Sync(Box<dyn FnOnce() + Send>),
    Async(Pin<Box<dyn Future<Output = ()> + Send>>),
}
```

### 6.2 Execution Order

Finalizers execute in **LIFO** (reverse registration) order. Stored in a `Vec` and popped from the back.

C equivalent: stack-based array with top pointer, decremented on each pop.

### 6.3 Async Finalizer Cancel Masking

Async finalizers run under a cancel mask (`runtime/state.rs:132`):

```rust
impl Future for MaskedFinalizer {
    fn poll(...) -> Poll<()> {
        self.enter_mask();   // Increments mask_depth
        let poll = self.inner.as_mut().poll(cx);
        if poll.is_ready() {
            self.exit_mask();  // Decrements mask_depth
        }
        poll
    }
}
```

C equivalent:

```c
asx_poll_result asx_masked_finalizer_poll(asx_masked_finalizer *f, asx_cx *cx) {
    asx_enter_cancel_mask(cx);
    asx_poll_result result = f->inner_poll(f->inner, cx);
    if (result == ASX_POLL_READY) {
        asx_exit_cancel_mask(cx);
    }
    return result;
}
```

### 6.4 Bounded Finalizer Budget

| Constant | Value | Purpose |
|----------|-------|---------|
| `FINALIZER_POLL_BUDGET` | 100 polls | Maximum polls per async finalizer |
| `FINALIZER_TIME_BUDGET_NANOS` | 5,000,000,000 (5s) | Maximum wall-clock time per async finalizer |

### 6.5 Finalizer Escalation Policies

| Policy | Behavior | Default |
|--------|----------|---------|
| `Soft` | Wait indefinitely (strict correctness) | No |
| `BoundedLog` | Log warning and continue to next finalizer | **Yes** |
| `BoundedPanic` | Panic after budget exceeded | No |

### 6.6 Registration Rejection

Registration is rejected once the region has begun closing (`state.rs:2038`):

```rust
if region.state().is_closing() || region.state().is_terminal() {
    return false;
}
```

### 6.7 Finalizer-Spawned Tasks

During `Finalizing`, finalizers may spawn cleanup tasks. These tasks:
- Are created in the finalizing region (admission allowed during `Finalizing`)
- Must complete before the region can close
- Do NOT reset the region state to an earlier phase
- Are counted in the region's task_count

Child region creation is NOT allowed during `Finalizing`.

---

## 7. Bounded Cleanup Under Cancellation

### 7.1 Cancellation Cleanup Budget Table

| Cancel Kind Group | Cleanup Poll Quota | Cleanup Priority |
|-------------------|-------------------|-----------------|
| `User` | 1000 | 200 |
| `Timeout` / `Deadline` | 500 | 210 |
| `PollQuota` / `CostBudget` | 300 | 215 |
| `FailFast` / `RaceLost` / `LinkedExit` | 200 | 220 |
| `ParentCancelled` / `ResourceUnavailable` | 200 | 220 |
| `Shutdown` | 50 | 255 |

From `cancel/symbol_cancel.rs:92`:

```rust
struct CancelTokenState {
    cleanup_budget: Budget,
    children: RwLock<SmallVec<...>>,
    listeners: RwLock<SmallVec<...>>,
}
```

Budget propagation to tasks during drain:

```rust
let task_budget = task_reason.cleanup_budget();
task.request_cancel_with_budget(task_reason.clone(), task_budget);
```

### 7.2 Budget Combination (Min-Plus Algebra)

- `combined_quota = min(quota1, quota2)` — tighter quota wins
- `combined_priority = max(priority1, priority2)` — higher urgency wins
- Monotone-narrowing: combining never widens the cleanup allowance

### 7.3 Cleanup Stack

The `asx_cleanup_stack` is per-task/per-region (plan Section 6.8.D):

1. Every `reserve`/`acquire` operation registers a cleanup action at acquisition time
2. `commit`/`abort` pops or marks entries resolved
3. During finalization, unresolved cleanup stack entries are drained deterministically in LIFO order
4. Poison pill pattern: discarding a token without resolution triggers detection during region finalization

### 7.4 Force-Completion

If a task's cleanup exceeds its budget:
1. The scheduler force-completes the task
2. Force-completion records the original cancel reason plus a `cleanup_budget_exceeded` flag
3. The event is deterministic and logged (not silent)
4. Remaining unresolved obligations are force-aborted

### 7.5 Exhaustion During Cleanup

| Exhaustion Type | Behavior |
|-----------------|----------|
| Poll quota exhausted | Task yields with `CancelReason::poll_quota()` |
| Cost quota exhausted | Operation fails (false return, no mutation) |
| Deadline exceeded | Task yields with `CancelReason::deadline()` |

---

## 8. Close Rejection and Admission Gating

### 8.1 Optimistic Double-Check Locking

All admission operations (`add_child`, `add_task`, `try_reserve_obligation`, `heap_alloc`) follow this pattern (`region.rs:471`):

```
Step 1: Fast-path reject (atomic load, no lock)
  IF !state.can_spawn(): RETURN Err(AdmissionError::Closed)
Step 2: Acquire write lock
Step 3: Re-check under lock
  IF !state.can_spawn(): RETURN Err(AdmissionError::Closed)
Step 4: Check limits
  IF limit exceeded: RETURN Err(AdmissionError::LimitReached { kind, limit, live })
Step 5: Commit operation
```

### 8.2 Admission Errors

| Error | When | C Code |
|-------|------|--------|
| Region not accepting work | State not `Open` (or not `Finalizing` for task-only) | `ASX_E_ADMISSION_CLOSED` |
| Configured limit exceeded | `max_children`, `max_tasks`, `max_obligations` | `ASX_E_ADMISSION_LIMIT` |
| Region handle invalid | Generation mismatch or arena slot freed | `ASX_E_REGION_NOT_FOUND` |

### 8.3 State-Gated Operations

| Operation | Allowed States | Notes |
|-----------|---------------|-------|
| Create child task | `Open`, `Finalizing` | `Finalizing` allows finalizer-spawned cleanup tasks |
| Create child region | `Open` only | NOT allowed during finalization |
| Reserve obligation | `Open` only | NOT allowed during finalization |
| Register finalizer | `Open` only | Rejected once closing begins |
| Query status | Any | Always succeeds |
| Access arena | `Open`, `Closing`, `Draining`, `Finalizing` | Rejected after `Closed` |

### 8.4 Spawn Error Variants

| Error | Description |
|-------|-------------|
| `ASX_E_REGION_NOT_FOUND` | Region handle stale or invalid |
| `ASX_E_REGION_CLOSED` | Region not accepting work |
| `ASX_E_REGION_AT_CAPACITY` | Admission limit reached |
| `ASX_E_SCHEDULER_UNAVAILABLE` | Local scheduler not available |
| `ASX_E_NAME_CONFLICT` | Named task registration failed |

---

## 9. Timer/Channel Coupling to Quiescence

Quiescence must include kernel structures beyond region/task counters.

### 9.1 Timer Quiescence

- No pending expirations in the timer wheel
- No active handles that should still fire
- Stale timer-handle cancellation is rejected without mutating live timer state
- Equal-deadline timer ordering is insertion-stable (replay-safe)

### 9.2 Channel Quiescence

- No deliverable committed queue entries
- No phantom reserved slots
- `queue_len + reserved <= capacity` invariant always holds
- Permit drop is equivalent to abort (no leaked reserved capacity)

### 9.3 Quiescence Error Diagnostics

| Check | Error Code |
|-------|-----------|
| Active tasks remain | `ASX_E_TASKS_STILL_ACTIVE` |
| Obligations unresolved | `ASX_E_OBLIGATIONS_UNRESOLVED` |
| Regions not closed | `ASX_E_REGIONS_NOT_CLOSED` |
| Timers pending | `ASX_E_TIMERS_PENDING` |
| Channel not drained | `ASX_E_CHANNEL_NOT_DRAINED` |

---

## 10. Deterministic Ordering Guarantees

### 10.1 Finalizer Execution Order

LIFO (reverse registration). Implementation: array with top index, decremented on each pop.

### 10.2 Obligation Iteration Order

BTreeMap keyed by ObligationId (from `obligation/ledger.rs:137`) ensures monotonically increasing ID order for leak collection, drain iteration, and diagnostics.

### 10.3 Region Close Cascade Order

When multiple child regions close, the cascade follows child addition order (monotonic insertion). The iterative advance loop processes one parent at a time, preventing unbounded recursion.

### 10.4 Cancel Propagation Order

Two-pass approach (`runtime/state.rs:1660`):
1. First pass: transition all regions to `Closing` (depth-first traversal)
2. Second pass: mark all tasks for cancellation within each region

### 10.5 Task Completion Notification Order

When multiple tasks complete in the same scheduler tick, notifications are processed in task ID order.

---

## 11. Convergence and Termination Proofs

### 11.1 Lyapunov Potential Function

From `obligation/lyapunov.rs`:

```
V(Sigma) = w_t * |live_tasks|
         + w_o * SUM(age(obligation, now))
         + w_r * |draining_regions|
         + w_d * SUM(max(0, 1 - slack(t, now) / D0))
```

Properties:
- `V(Sigma) >= 0` (non-negativity)
- `V(Sigma) = 0 iff Sigma is quiescent` (zero iff quiescent)
- `Sigma →_s Sigma' => V(Sigma') <= V(Sigma)` (monotone decrease under scheduling steps)

### 11.2 Progress Certificates (Drain Termination)

From `cancel/progress_certificate.rs`, using supermartingale theory:

```
M_t = V(Sigma_t) + SUM_{i=1}^{t} c_i    (progress process)
E[tau] <= V(Sigma_0) / min_credit         (Optional Stopping theorem bound)
P(V(Sigma_t) > V(Sigma_0) - t*mu + lambda) <= exp(-2*lambda^2 / (t*c^2))  (Azuma-Hoeffding)
```

This proves drain always terminates under bounded cleanup budgets.

### 11.3 Cleanup Budget Bound

Given cancel reason with cleanup poll quota `Q` and time budget `T`:
- A task can execute at most `Q` polls during cleanup
- Wall-clock time is bounded by `T`
- Whichever bound is reached first triggers force-completion
- Region drain is bounded by `max(Q_max, T_max) * task_count`

### 11.4 Recovery Convergence (Deferred to Wave B)

The recovery governor (`obligation/recovery.rs`) provides:
- Each recovery step only advances obligations toward terminal states
- Terminal states are absorbing
- Non-terminal obligation count is finite and monotonically decreasing
- Default stale timeout: 30s production, 5s test
- Budget: `max_resolutions_per_tick` (default 50) prevents resolution storms

---

## 12. Oracle System

### 12.1 Verification Oracles

| Oracle | File | Invariant | C Equivalent |
|--------|------|-----------|--------------|
| `QuiescenceOracle` | `lab/oracle/quiescence.rs` | `close_region => no_live_children AND no_live_tasks` | `asx_ghost_quiescence_check()` |
| `FinalizerOracle` | `lab/oracle/finalizer.rs` | `close_region => all_finalizers_ran` | `asx_ghost_finalizer_check()` |
| `TaskLeakOracle` | `lab/oracle/task_leak.rs` | `close_region => all_tasks_completed` | `asx_ghost_task_leak_check()` |
| `ObligationLeakOracle` | `lab/oracle/obligation_leak.rs` | `close_region => no_reserved_obligations` | `asx_ghost_obligation_leak_check()` |
| `NoLeakProver` | `obligation/no_leak_proof.rs` | Ghost counter proof | `asx_ghost_no_leak_proof()` |

### 12.2 NoLeakProver Liveness Properties

| # | Property | Statement |
|---|----------|-----------|
| NL1 | `CounterIncrement` | Ghost counter increases on reserve |
| NL2 | `CounterDecrement` | Ghost counter decreases on resolve (commit/abort/leak) |
| NL3 | `CounterNonNegative` | Ghost counter >= 0 at all times |
| NL4 | `TaskCompletion` | Task completion implies zero pending for that task |
| NL5 | `RegionQuiescence` | Region closure implies zero pending for that region |
| NL6 | `EventualResolution` | All obligations resolved at trace end |
| NL7 | `DropPathCoverage` | Drop path correctly resolves (leak is still a resolution) |

### 12.3 Ghost Monitor Integration (Debug Builds)

```c
#ifdef ASX_DEBUG
  #define ASX_GHOST_CHECK_QUIESCENCE(region) asx_ghost_quiescence_check(region)
  #define ASX_GHOST_CHECK_LEAKS(region)      asx_ghost_obligation_leak_check(region)
  #define ASX_GHOST_CHECK_FINALIZERS(region)  asx_ghost_finalizer_check(region)
#else
  #define ASX_GHOST_CHECK_QUIESCENCE(region) ((void)0)
  #define ASX_GHOST_CHECK_LEAKS(region)      ((void)0)
  #define ASX_GHOST_CHECK_FINALIZERS(region)  ((void)0)
#endif
```

Zero overhead in release builds.

---

## 13. Forbidden Behavior Catalog

### 13.1 Quiescence/Finalization Forbidden Behaviors

| ID | Forbidden Behavior | Expected Result | Detection Point |
|----|-------------------|-----------------|-----------------|
| QF-001 | Close region with active child tasks | Region blocks in `Draining` | `can_region_complete_close()` |
| QF-002 | Close region with unresolved obligations | Leak detection fires; obligations marked `Leaked` | Region finalization advance |
| QF-003 | Close region with pending finalizers | Region stays in `Finalizing` | `finalizers_empty()` check |
| QF-004 | Close region with open child regions | Region stays in `Draining`/`Finalizing` | `child_count()` check |
| QF-005 | Register finalizer after close initiated | Registration returns false | `state.is_closing()` check |
| QF-006 | Spawn child region during `Finalizing` | `ASX_E_ADMISSION_CLOSED` | `can_spawn()` check |
| QF-007 | Reserve obligation during `Finalizing` | `ASX_E_ADMISSION_CLOSED` | `can_spawn()` check |
| QF-008 | Double-resolve obligation | Panic (unconditional) | `ObligationRecord` state |
| QF-009 | Access region arena after `Closed` | `ASX_E_REGION_CLOSED` | State check |
| QF-010 | Backward region state transition | CAS fails | Expected state mismatch |
| QF-011 | Skip intermediate close state | `ASX_E_INVALID_TRANSITION` | Transition authority table |
| QF-012 | Recursive leak detection | Suppressed | `handling_leaks` flag |
| QF-013 | Quiescence report with pending timers | `ASX_E_TIMERS_PENDING` | Timer wheel check |
| QF-014 | Quiescence report with undrained channels | `ASX_E_CHANNEL_NOT_DRAINED` | Channel check |
| QF-015 | Silent cleanup-budget overrun | Must be explicit and logged | Force-completion path |

### 13.2 Cross-Reference to bd-296.15 Lifecycle Tables

| This Doc | bd-296.15 | Relationship |
|----------|-----------|--------------|
| QF-001 | FB-012 | Same invariant (close with active children) |
| QF-002 | FB-011 | Same invariant (close with unresolved obligations) |
| QF-008 | FB-003/004/005/006 | Expanded (all double-resolve variants) |
| QF-009 | FB-014 | Same invariant (arena access after close) |
| QF-010 | FB-007 | Same invariant (backward transition) |
| QF-011 | FB-008 | Same invariant (skip intermediate state) |

---

## 14. Fixture Family Mapping

### 14.1 Quiescence Fixtures

| Fixture ID | Description | Invariants |
|-----------|-------------|------------|
| `quiescence-001` | Empty region closes immediately | Q1-Q4 trivially satisfied |
| `quiescence-002` | Region waits for single child task | Q1 enforced |
| `quiescence-003` | Region waits for nested child region | Q2 enforced; cascade close |
| `quiescence-004` | Region waits for obligation resolution | Q3 enforced |
| `quiescence-005` | Region waits for finalizer execution | Q4 enforced |
| `quiescence-006` | Full four-conjunct quiescence | All Q1-Q4 |
| `quiescence-007` | Runtime-level quiescence after shutdown | RQ1-RQ5 |
| `quiescence-008` | Quiescence with cancel-masked finalizer task | Q1 includes finalizer-spawned tasks |

### 14.2 Close Precondition Fixtures

| Fixture ID | Description | Invariants |
|-----------|-------------|------------|
| `close-precond-001` | Close gate rejects when tasks remain | CP3 |
| `close-precond-002` | Close gate rejects when obligations remain | CP4 |
| `close-precond-003` | Close gate rejects when children remain | CP5 |
| `close-precond-004` | Close gate rejects when finalizers remain | CP2 |
| `close-precond-005` | Double-guard prevents TOCTOU race | CP1-CP5 both guards |
| `close-precond-006` | Close succeeds when all preconditions met | CP1-CP5 all pass |

### 14.3 Leak Detection Fixtures

| Fixture ID | Description | Invariants |
|-----------|-------------|------------|
| `leak-detect-001` | Task completion triggers leak scan | Detection Point 1 |
| `leak-detect-002` | Region finalization triggers leak scan | Detection Point 2 |
| `leak-detect-003` | Leak response: Panic | Policy enforcement |
| `leak-detect-004` | Leak response: Log | Policy enforcement |
| `leak-detect-005` | Leak response: Silent | Policy enforcement |
| `leak-detect-006` | Leak response: Recover (auto-abort) | Policy enforcement |
| `leak-detect-007` | Escalation threshold triggers stricter response | Escalation policy |
| `leak-detect-008` | Reentrance guard prevents recursive detection | Reentrance flag |
| `leak-detect-009` | Double-panic prevention (downgrade to Log) | Thread-panicking check |
| `leak-detect-010` | Deterministic leak iteration order | BTreeMap ordering |
| `leak-detect-011` | Orphaned obligations auto-aborted at task completion | Always-abort path |
| `leak-detect-012` | NoLeakProver ghost counter properties (NL1-NL7) | All 7 liveness properties |

### 14.4 Finalizer Fixtures

| Fixture ID | Description | Invariants |
|-----------|-------------|------------|
| `finalizer-001` | Sync finalizer runs inline | Execution order |
| `finalizer-002` | Async finalizer runs as masked task | Cancel masking |
| `finalizer-003` | LIFO execution order | Reverse registration |
| `finalizer-004` | Finalizer poll budget exceeded | Budget enforcement |
| `finalizer-005` | Finalizer time budget exceeded | Budget enforcement |
| `finalizer-006` | BoundedLog escalation (default) | Escalation policy |
| `finalizer-007` | Soft escalation (wait indefinitely) | Escalation policy |
| `finalizer-008` | BoundedPanic escalation | Escalation policy |
| `finalizer-009` | Finalizer registration rejected after close | Admission gating |
| `finalizer-010` | Finalizer spawns cleanup task | Task creation during Finalizing |

### 14.5 Cleanup Budget Fixtures

| Fixture ID | Description | Invariants |
|-----------|-------------|------------|
| `cleanup-budget-001` | User cancel: 1000 poll quota | Budget table |
| `cleanup-budget-002` | Shutdown cancel: 50 poll quota | Budget table |
| `cleanup-budget-003` | Budget combination: min-plus algebra | Monotone narrowing |
| `cleanup-budget-004` | Force-completion on budget exceeded | Bounded cleanup |
| `cleanup-budget-005` | Cleanup stack LIFO drain | Reverse order |
| `cleanup-budget-006` | Cleanup under poll quota exhaustion | Exhaustion mapping |
| `cleanup-budget-007` | Cleanup under deadline exhaustion | Exhaustion mapping |

### 14.6 Cascade Close Fixtures

| Fixture ID | Description | Invariants |
|-----------|-------------|------------|
| `cascade-close-001` | Child close cascades to parent advance | Bottom-up propagation |
| `cascade-close-002` | Deep nesting (3+ levels) cascade | Iterative not recursive |
| `cascade-close-003` | Multiple children close in deterministic order | Ordering guarantee |
| `cascade-close-004` | Shutdown initiates root region close | Runtime shutdown |
| `cascade-close-005` | Cascade with mixed sync/async finalizers | Mixed finalization |

---

## 15. C Port Contract

### 15.1 Structural Requirements

| # | Requirement | Rationale |
|---|-------------|-----------|
| C1 | Implement `asx_region_is_quiescent()` with exact four-conjunct check | Formal definition from Lean4 |
| C2 | Implement `asx_runtime_is_quiescent()` with five-conjunct check | Runtime-level quiescence |
| C3 | Region close: `Open→Closing→Draining→Finalizing→Closed` with CAS | Atomic state progression |
| C4 | `advance_region_state()` must be iterative (not recursive) | Bounded stack depth |
| C5 | Double-guard on Finalizing→Closed | Prevent TOCTOU races |
| C6 | Leak detection at both task-completion and region-finalization | Two detection points |
| C7 | Configurable leak response with escalation | Production flexibility |
| C8 | Finalizer LIFO with bounded budget | Deterministic cleanup |
| C9 | Cancel-masked async finalizers | Cannot be interrupted |
| C10 | Cleanup stack per task/region with LIFO drain | RAII equivalent |
| C11 | Sorted obligation storage (BTreeMap equivalent) | Deterministic iteration |
| C12 | Reentrance guard for leak detection | Prevent infinite recursion |
| C13 | Ghost monitors: debug builds only, zero-cost in release | Debug verification |

### 15.2 Error Codes Required

| Error Code | Context |
|------------|---------|
| `ASX_E_ADMISSION_CLOSED` | Region not accepting work |
| `ASX_E_ADMISSION_LIMIT` | Capacity limit reached |
| `ASX_E_REGION_NOT_FOUND` | Stale/invalid region handle |
| `ASX_E_REGION_CLOSED` | Arena access after close |
| `ASX_E_UNRESOLVED_OBLIGATIONS` | Obligations remain at close attempt |
| `ASX_E_INCOMPLETE_CHILDREN` | Children not complete at close attempt |
| `ASX_E_TASKS_STILL_ACTIVE` | Tasks remain active (quiescence check) |
| `ASX_E_OBLIGATIONS_UNRESOLVED` | Obligations remain (quiescence check) |
| `ASX_E_REGIONS_NOT_CLOSED` | Regions not closed (quiescence check) |
| `ASX_E_TIMERS_PENDING` | Timers remain (quiescence check) |
| `ASX_E_CHANNEL_NOT_DRAINED` | Channel messages remain (quiescence check) |
| `ASX_E_OBLIGATION_ALREADY_RESOLVED` | Double-resolve attempt |
| `ASX_E_INVALID_TRANSITION` | Backward or skip state transition |
| `ASX_E_SCHEDULER_UNAVAILABLE` | No local scheduler |
| `ASX_E_NAME_CONFLICT` | Named task conflict |

### 15.3 Configuration Surface

| Config Field | Type | Default | Description |
|-------------|------|---------|-------------|
| `leak_response` | enum | `Log` | Obligation leak handling policy |
| `leak_escalation` | struct (optional) | None | Threshold-based escalation |
| `finalizer_poll_budget` | `uint32_t` | 100 | Max polls per async finalizer |
| `finalizer_time_budget_ns` | `uint64_t` | 5,000,000,000 | Max time per async finalizer (ns) |
| `finalizer_escalation` | enum | `BoundedLog` | Finalizer budget exceeded response |
| `max_chain_depth` | `uint16_t` | 16 | Max cancel attribution chain depth |
| `max_chain_memory` | `size_t` | 4096 | Max cancel chain memory (bytes) |

### 15.4 Invariant Schema Mapping

Each invariant maps to `invariants/*.yaml` rows (per bd-296.4):

```yaml
- domain: region
  invariant: quiescence
  type: precondition
  trigger: complete_close
  conditions:
    - "task_count == 0"
    - "child_count == 0"
    - "pending_obligations == 0"
    - "finalizer_count == 0"
  error_on_violation: ASX_E_UNRESOLVED_OBLIGATIONS
  fixture_ids: [quiescence-001 .. quiescence-006]
  formal_ref: "Lean4: close_quiescence_decomposition (line 892)"
```

### 15.5 Validation Against Upstream Artifacts

| Upstream | Validation |
|----------|------------|
| bd-296.15 (Lifecycle Tables) | Close state machine matches Section 1.2; forbidden behaviors cross-referenced in Section 13.2 |
| bd-296.16 (Outcome/Budget) | Cleanup budget values match Table 4.4; exhaustion semantics applied to cleanup paths |
| bd-296.17 (Channel/Timer) | Channel/timer quiescence conditions (RQ3-RQ5) incorporate drain checks from channel/timer spec |

### 15.6 Downstream Handoff

This extraction feeds:
1. `bd-296.4` — machine-readable invariant schema and generation pipeline
2. `bd-296.5` — forbidden-behavior catalog + shared scenario DSL
3. `bd-296.6` — Rust→C guarantee-substitution matrix
4. `bd-296.19` — source-to-fixture provenance map
5. `bd-296.1` — exhaustive EXISTING_ASUPERSYNC_STRUCTURE.md
6. `bd-1md.13` — core semantic fixture families
7. `bd-1md.15` — vertical and continuity fixture families
