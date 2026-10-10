# Quiescence, Close, Cleanup, and Leak-Detection Invariants

> **Bead:** bd-296.18 (original extraction); re-extracted for bd-9kll.2.12
> **Status:** Reference for what the Rust code does at the baseline below, with a **C status** line in each section. Where asupersync's normative small-step semantics, `asupersync_v4_formal_semantics.md` ("v4" below), states a rule, the section is marked superseded and keeps a short summary only. The C status of every v4 rule is in [`docs/C_REFINEMENT_MAP.md`](C_REFINEMENT_MAP.md); rows are named by rule ID.
> **Rust baseline:** asupersync `5e60b1c4c`. Every Rust `path:line` below was re-read at this commit with `git -C /dp/asupersync show 5e60b1c4c:<path>`; paths are relative to the asupersync repository.
> **C baseline:** C `file:line` citations were read in the working tree at `b412780`. Line numbers drift as the code changes: treat them as grep anchors for the symbol named beside each.
> **Cross-references:** `docs/C_REFINEMENT_MAP.md` (per-rule C status), `docs/CHANNEL_TIMER_SEMANTICS.md` (channel/timer facts, re-verified 2026-10-10), `docs/LIFECYCLE_TRANSITION_TABLES.md` (bd-296.15), `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (bd-296.16)
> **Last verified:** 2026-10-10 (bd-9kll.2.12)
> **Prior versions:** NobleCanyon and CopperSpire (2026-02-27), extracted from an unpinned Rust baseline. Facts and citations from those versions that no longer hold at `5e60b1c4c` were corrected in place; the corrections are called out where they change meaning.

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

### 1.1 Region-Level Quiescence

> **Superseded by** `asupersync_v4_formal_semantics.md` §1.12 (definition `Quiescent(r)`) and §5 INV-QUIESCENCE (rule `inv.region.quiescence`, #27). **C status:** see `C_REFINEMENT_MAP.md` row `inv.region.quiescence`.

Summary. v4 §1.12 defines `Quiescent(r)` with three conjuncts: every child task `Completed`, every subregion `Closed`, and `ledger(r) = ∅`. Finalizers are not part of it; an empty finalizer list is a separate precondition of CLOSE-COMPLETE (§3.3). The implementations differ from the spec and from each other:

| # | Condition | v4 §1.12 | Lean `Quiescent` (`formal/lean/Asupersync.lean:268-272`) | Rust `RegionRecord::is_quiescent` (`src/record/region.rs:1658-1666`) | C `asx_quiescence_check_detailed` (`src/runtime/quiescence.c:161-193`) |
|---|---|---|---|---|---|
| Q1 | Child tasks done | all `Completed` | `allTasksCompleted s r.children` | `tasks.is_empty()` (a completed task is unlinked from its region) | `task_count == 0` (`src/runtime/quiescence.c:174`) |
| Q2 | Child regions closed | all `Closed` | `allRegionsClosed s r.subregions` | `children.is_empty()` (a closed child is unlinked) | `child_count == 0` (`src/runtime/quiescence.c:177`) |
| Q3 | Obligations resolved | `ledger(r) = ∅` | `r.ledger = []` | `pending_obligations == 0 && unapplied_obligations == 0` | no Reserved obligation of the region (`src/runtime/quiescence.c:180`, counted at `:101-115`) |
| Q4 | Finalizers done | not a conjunct | `r.finalizers = []` | `finalizers` empty | cleanup stack drained (`src/runtime/quiescence.c:183`) |
| Q5 | Pending spawns | — | — | `pending_spawns.count() == 0` | no counterpart |
| — | Region state | — | — | not checked | region must be `CLOSED` (`src/runtime/quiescence.c:186-190`) |

**C status:** `asx_region_is_quiescent` takes a region handle, not a record pointer: `int asx_region_is_quiescent(asx_region_id id)` (`src/runtime/quiescence.c:150-155`) returns the `quiescent` flag of the detailed report. `asx_quiescence_check` (`src/runtime/quiescence.c:135-148`) is the status-returning form: `ASX_E_QUIESCENCE_NOT_REACHED` unless the region is `CLOSED` or while its cleanup stack has entries, `ASX_E_QUIESCENCE_TASKS_LIVE` while tasks are live, `ASX_E_OBLIGATIONS_UNRESOLVED` while an obligation is Reserved. Unlike Rust's structural predicate, C's is true only for a `CLOSED` region, and its Q4 is the C cleanup stack (section 7.3), not a finalizer list. Unit tests: `tests/unit/runtime/test_quiescence.c` `quiescence_detailed_fully_quiescent`, `quiescence_detailed_live_tasks_q1_fails`, `quiescence_detailed_open_child_q2_fails`, `quiescence_detailed_unresolved_obligations_q3_fails`, `quiescence_detailed_undrained_cleanup_q4_fails`.

`r->task_count` is the region's live-task count (`src/runtime/runtime_internal.h:33`), decremented when a task completes (`src/runtime/lifecycle.c:568`). It differs from `asx_runtime_task_count()` (`src/runtime/rt.c:364-367`), which returns the number of allocated task slots (`g_task_live`, incremented at slot allocation, `src/runtime/lifecycle.c:407`, decremented at release, `:304`), so it can include completed tasks whose slots have not been released.

### 1.2 Runtime-Level Quiescence

v4 has no rule for whole-runtime quiescence; this is an implementation predicate.

Rust `RuntimeState::is_quiescent` (`src/runtime/state.rs:7364-7382`; body `is_quiescent_in`, `:7396-7414`) requires all of:

| # | Condition | Rust check |
|---|-----------|------------|
| RQ1 | No live tasks | `live_task_count() == 0` |
| RQ2 | No pending obligations | `pending_count() == 0` on the obligation table |
| RQ3 | No pending cancel dispatches | `pending_cancel_dispatches.is_empty()` |
| RQ4 | No active I/O sources | `io_driver.is_none_or(IoDriverHandle::is_empty)` |
| RQ5 | Per region: no pending finalizers, not closing, no pending spawns, no pending or unapplied obligations | `finalizers_empty() && !state().is_closing() && pending_spawn_count() == 0 && pending_obligations() == 0 && unapplied_obligation_count() == 0` |

Correction to the earlier version: RQ5 is not "all regions Closed". `is_closing()` is true only for Closing, Draining and Finalizing (`src/record/region.rs:162-164`), so an `Open` region does not make the runtime non-quiescent. The v2 fixtures show this. Their `quiescent` flag is `LabRuntime::is_quiescent` (`tools/twin_run/src/run.rs:2572`), which is `RuntimeState::is_quiescent` plus an empty spawn mailbox and no pending obligation posts (`src/lab/runtime.rs:3069-3073`). In 63 of the 67 Rust-captured snapshots (counted 2026-10-10) the run ends `quiescent: true` with at least one region still Open, for example `cancel-strengthen-severity-001`, whose region `root` is Open.

**C status:** `asx_runtime_is_quiescent` (`src/runtime/rt.c:374-383`) requires no live task (`:83-94`), no Reserved obligation (`:71-81`), no I/O registration (`:379`), no region with a pending cleanup entry (`:96-106`) and **every live region `CLOSED`** (`:108-118`). It has no pending-cancel-dispatch or pending-spawn conjunct. The all-regions-Closed conjunct differs from Rust: a runtime whose root region is merely Open is quiescent in Rust and not quiescent by this C API. The conformance oracle does not use `asx_runtime_is_quiescent`: the interpreter computes the snapshot's `quiescent` flag the Rust way, failing it only for a region that is still closing (`build_snapshot`, `src/conformance/interpreter.c:2751-2861`), so fixtures do not see this API's difference. Not recorded in `C_REFINEMENT_MAP.md`.

### 1.3 Formal Proofs (Lean4)

The following theorems are in `formal/lean/Asupersync.lean` at `5e60b1c4c` (line of the `theorem` keyword):

| # | Theorem | Line | Statement |
|---|---------|------|-----------|
| T1 | `close_implies_quiescent` | 793 | Any closed region was quiescent at the moment of closing |
| T2 | `close_implies_ledger_empty` | 807 | Closed region has empty obligation ledger |
| T3 | `close_implies_finalizers_empty` | 819 | Closed region has no pending finalizers |
| T4 | `close_quiescence_decomposition` | 888 | Full decomposition into the four conjuncts of Lean's `Quiescent` |
| T5 | `quiescent_tasks_completed` | 859 | Quiescent implies all children completed |
| T6 | `quiescent_subregions_closed` | 866 | Quiescent implies all subregions closed |
| T7 | `quiescent_no_obligations` | 873 | Quiescent implies empty ledger |
| T8 | `quiescent_no_finalizers` | 880 | Quiescent implies no finalizers |
| T9 | `obligation_in_ledger_blocks_close` | 938 | Any obligation in ledger prevents close |
| T10 | `close_children_exist_completed` | 1406 | Close implies every child task exists and is completed |
| T11 | `close_subregions_exist_closed` | 1445 | Close implies every subregion exists and is closed |
| T12 | `call_obligation_resolved_at_close` | 3817 | All call obligations resolved at close |

**Lean close rule** (`Step.close`, `formal/lean/Asupersync.lean:578-587`):

```lean
| close ... (hState : region.state = RegionState.finalizing)
            (hFinalizers : region.finalizers = [])
            (hQuiescent : Quiescent s region)
```

Lean's `Quiescent` (`:268-272`) has four conjuncts, including `r.finalizers = []`; v4 §1.12 has three.

**C status:** no C witness is mapped to these theorems. The open bead bd-9kll.2.14 tracks a Lean/TLA crosswalk to C witnesses.

### 1.4 TLA+ Invariant

`formal/tla/Asupersync.tla:248-253` (the variable is `regionSubs`, not `regionSubregions` as the earlier version had it):

```tla
CloseImpliesQuiescent ==
    \A r \in RegionIds :
        regionState[r] = "Closed" =>
            /\ \A t \in regionChildren[r] : taskState[t] = "Completed"
            /\ \A r2 \in regionSubs[r] : regionState[r2] = "Closed"
            /\ regionLedger[r] = {}
```

It is checked as part of the spec's invariant conjunction `CoreInv` (`formal/tla/Asupersync.tla:638-640`). **C status:** none (bd-9kll.2.14).

### 1.5 CALM Classification

`src/obligation/calm.rs` classifies saga operations by monotonicity (module doc, `:1-3`). Its table (`:27-33`) lists `RegionClose` as non-monotone ("Quiescence barrier (aggregation)"), together with `CancelDrain` (quiescence barrier), `MarkLeaked` (depends on absence of resolution) and `BudgetCheck` (threshold on depleting counter); the entries are at `:158`, `:182`, `:188`, `:194`.

**C status:** no counterpart.

---

## 2. Close State Machine and Advance Logic

### 2.1 Region Close Progression

> **Superseded by** v4 §1.6 (`RegionState`) and §3.3 (rules CLOSE-BEGIN `rule.region.close_begin` #22, CLOSE-CANCEL-CHILDREN `rule.region.close_cancel_children` #23, CLOSE-CHILDREN-DONE `rule.region.close_children_done` #24, CLOSE-RUN-FINALIZER `rule.region.close_run_finalizer` #25, CLOSE-COMPLETE `rule.region.close_complete` #26). **C status:** see `C_REFINEMENT_MAP.md` rows `rule.region.close_begin` through `rule.region.close_complete`.

```
Open ──(close begins)──> Closing ──(child regions exist)──> Draining ──(work done)──> Finalizing ──(finalizers done, ledger empty)──> Closed
                            |                                                              ^
                            └──(no child regions, work done)───────────────────────────────┘
```

Summary. v4 enters Draining whenever a child *task* is still live (CLOSE-CANCEL-CHILDREN). Rust enters Draining only when the region has child *regions* (`advance_region_state_in`, `src/runtime/state.rs:10220-10228`); a region whose live children are only tasks waits in Closing and goes straight to Finalizing (`src/record/region.rs:70-74`, `begin_finalize` at `:1756-1781` accepts Closing or Draining). State changes are compare-and-swap on an atomic (`AtomicRegionState::transition`, `src/record/region.rs:310-319`). Rust also closes a *sealed* Open region by itself once it holds no live work, without cancelling anything (`src/runtime/state.rs:10427-10462`; `RegionRecord::seal`, `src/record/region.rs:1019-1022`).

**C status:** the same five states and the same legal edges, as a lookup table (`src/core/transition_tables.c:11-28`, checked by `asx_region_transition_check`, `:72-75`). C keeps Rust's Draining condition (child regions only; row `rule.region.close_cancel_children`, Variant). There is no CAS: the kernel updates the slot after the table check (`asx_region_set_state`, `src/runtime/quiescence.c:214-221`). C has no sealed regions: the advance loop returns on an Open region (`src/runtime/quiescence.c:275`). Whether any v2 scenario reaches Rust's sealed-region path was not checked (unverified); this difference is not recorded in `C_REFINEMENT_MAP.md`.

### 2.2 State Predicates

| Predicate | Rust (`src/record/region.rs`) | True when | Rust uses it for | C (`src/core/transition_tables.c`) |
|-----------|------|-----------|------------------|---|
| `can_spawn()` | `:130` | `Open` | child-region admission (`add_child`, `:1343-1373`) | `asx_region_can_spawn` (`:87`), `Open`; used for obligation reserve (`src/runtime/lifecycle.c:1286`); child regions test `state != OPEN` directly (`src/runtime/lifecycle.c:746`) |
| `can_accept_work()` | `:140` | `Open` only. Correction: Finalizing is excluded (doc comment `:134-139`) | normal task admission (`add_task`, `:1430-1432`), obligation reserve (`try_reserve_obligation`, `:1449-1472`) | `asx_region_can_accept_work` (`:89-91`) is `Open` or `Finalizing`, used by every spawn (`src/runtime/lifecycle.c:1007`, `:1105`); differs from Rust: row `rule.ownership.spawn` (bd-9kll.3.5) |
| `can_accept_cleanup_work()` | `:150` | `Open` or `Finalizing` | cleanup-task admission (`add_cleanup_task`, `:1438-1440`), used for finalizer tasks (`src/runtime/state.rs:4314`) | no separate predicate |
| `is_closing()` | `:162` | `Closing`, `Draining`, `Finalizing` | finalizer registration rejection (`src/runtime/state.rs:9057`, `:9110`); runtime quiescence (`:7409`) | `asx_region_is_closing` (`:93-95`) |
| `is_terminal()` | `:124` | `Closed` | heap-allocation rejection (`heap_alloc`, `:1575-1604`) | `asx_region_is_terminal` (`:97`) |
| `is_draining()` | `:156` | `Draining` | drain bookkeeping | no C function |

### 2.3 Advance Region State Driver

Rust's driver is `advance_region_state` (`src/runtime/state.rs:10057-10097`), whose core is `advance_region_state_in` (`:10118-10467`). It is a `while` loop over one region at a time, not recursion:

```
WHILE current_region != NULL:
  MATCH state:
    Closing | Draining:                                              state.rs:10141-10245
      IF can_region_finalize (all tasks terminal, all child regions closed,
         no pending spawns; :7954-7992):
        begin_finalize() → Finalizing; reprocess this region          :10181, :10243
      IF child_count > 0 AND state == Closing: begin_drain() → Draining  :10220-10228
    Finalizing:                                                       :10246-10426
      1. IF an async or manual finalizer is in flight: STOP            :10247-10251
      2. IF finalizers remain: STOP. No user finalizer runs here; the
         scheduler takes the top LIFO entry and runs it as a masked
         task (section 6)                                              :10253-10264
      3. IF pending_obligations > 0 AND task_count == 0:
           collect_obligation_leaks(region) → handle_obligation_leaks  :10271-10295
      4. IF can_region_complete_close():                               :10298
           complete_close(): re-check under the record lock,
             CAS Finalizing → Closed, reclaim heap, wake waiters       :10317; region.rs:1786-1862
           mark the region finalized in the obligation table           :10350
           remove from the parent's child list; current = parent      :10395, :10398
           remove from the region arena                                :10412
    Open: a sealed region with no live work begins a non-cancelling
          close and is reprocessed                                     :10427-10462
    Closed: nothing                                                    :10464
```

Corrections to the earlier version: step 2 no longer executes sync finalizers inline (the comment at `:10253-10258` says so), and the leak audit in step 3 waits for `task_count == 0`, not merely for every task to be terminal (comment `:10266-10270`).

**C status:** `asx_region_advance_internal` (`src/runtime/quiescence.c:265-289`) is also iterative, bounded by `ASX_MAX_REGIONS`. It returns on an Open or Closed region (`:275`), moves a Closing region with child regions to Draining (`:279-282`), returns while the region is poisoned or has live tasks (`:283`), then calls `asx_region_finalize_one` (`:224-263`) and continues with the parent once the region is Closed (`:284-287`). It runs after every task completion (`asx_region_settle_internal`, `src/runtime/lifecycle.c:572-579`) and after a region cancel for regions already idle (`src/runtime/cancellation.c:484-491`). Differences from Rust: cleanup callbacks run synchronously inside `asx_region_finalize_one` (no finalizer tasks), there is no leak audit at finalization (C stops with `ASX_E_OBLIGATIONS_UNRESOLVED`; rows `rule.region.close_run_finalizer` and `inv.obligation.ledger_empty_on_close`, bd-9kll.3.4), and there are no sealed regions.

### 2.4 Drain Phase Semantics

> **Superseded by** v4 §3.3 CLOSE-CANCEL-CHILDREN (rule `rule.region.close_cancel_children`, #23) and §3.2 CANCEL-REQUEST (rule `rule.cancel.request`, #1). **C status:** see `C_REFINEMENT_MAP.md` rows `rule.region.close_cancel_children` (Variant) and `rule.cancel.request`.

Summary. In Rust the children are cancelled by `RuntimeState::cancel_request` (`src/runtime/state.rs:7547-7553`, core `cancel_request_in` from `:7625`), which also moves each Open region of the subtree to Closing (`begin_close_without_subscriber`, `:7779`). Each task gets its reason's cleanup budget (`CancelReason::cleanup_budget`, `src/types/cancel.rs:1027-1043`), met with a region shutdown-budget ceiling when one is set (`src/runtime/state.rs:7832-7833`). The scheduler drives the cancelled tasks; each completion re-runs the advance walk (section 2.3), and the region advances once its tasks are unlinked and its child regions closed.

**C status:** `asx_region_cancel` (`src/runtime/cancellation.c:430-496`) does the same three steps in C (region pass, task pass, then advance of idle regions). `asx_region_close` alone (`src/runtime/lifecycle.c:893-912`) moves Open to Closing and cancels nothing; `asx_region_close_request` (`:833-836`) cancels with the close reason. `asx_region_drain` cancels the subtree's tasks with ParentCancelled through `asx_cancel_propagate` (`src/runtime/quiescence.c:326`), not with a requested kind (bd-9kll.3.4).

### 2.5 Complete Close Implementation

> **Superseded by** v4 §3.3 CLOSE-COMPLETE (rule `rule.region.close_complete`, #26). **C status:** see `C_REFINEMENT_MAP.md` row `rule.region.close_complete` (Partial: C records no close outcome).

Summary of Rust `RegionRecord::complete_close` (`src/record/region.rs:1786-1862`): under the record's write lock it requires Finalizing (`:1792`), re-checks no children, no tasks, no pending or unapplied obligations, no finalizers and no pending spawns (`:1796-1808`), defaults the close outcome to `Cancelled(reason)` when the region was cancelled and `Ok` otherwise (`:1820-1825`), CASes Finalizing → Closed (`:1827-1829`), reclaims the heap (`:1833`), stores the close receipt and wakes the close waiters (`:1846-1853`). The earlier snippet (calling `clear_heap` and returning after a bare quiescence check) no longer matches the code.

**C status:** the Finalizing branch of `asx_region_finalize_one` (`src/runtime/quiescence.c:242-260`) checks that no obligation is Reserved (`:243-244`), runs the ghost leak check (`:247`), drains the cleanup stack (`:252`), re-checks that no task is live (`:253`), sets Closed (`:255`), unlinks from the parent (`:257`), emits `region.closed` (`:258`) and wakes close waiters (`:259`). No close outcome is computed or stored.

---

## 3. Close Preconditions

### 3.1 Finalizing-to-Closed Gate

> **Superseded by** v4 §3.3 CLOSE-COMPLETE (rule `rule.region.close_complete`, #26; preconditions `Finalizing`, `finalizers = []`, all obligations resolved) and v4 §3.4.5 / §5 INV-LEDGER-EMPTY-ON-CLOSE (rule `inv.obligation.ledger_empty_on_close`, #20). **C status:** see `C_REFINEMENT_MAP.md` rows `rule.region.close_complete` and `inv.obligation.ledger_empty_on_close`.

| # | Precondition | Rust (`src/runtime/state.rs`, `can_region_complete_close_in` `:9665-9771`) | C (`src/runtime/quiescence.c`) |
|---|-------------|------|---|
| CP1 | State is `Finalizing` | checked (and again in `complete_close`, `src/record/region.rs:1792`) | `asx_region_finalize_one` handles Finalizing at `:242` |
| CP2 | Finalizers done | `finalizers_empty()`, no tracked pending finalizer id, no active async or manual finalizer | cleanup stack drained synchronously just before close (`:252`); never blocks |
| CP3 | Tasks completed and unlinked | `task_count() == 0` (not merely terminal) | `task_count == 0` (`:253`; the advance loop also requires it, `:283`) |
| CP4 | Obligations resolved | `pending_obligations() == 0 && unapplied_obligation_count() == 0`, after the Finalizing-arm leak audit (`:10271-10295`) | no Reserved obligation (`:243-244`, `:47-60`); no leak audit: the region stays Finalizing and the call returns `ASX_E_OBLIGATIONS_UNRESOLVED` (bd-9kll.3.4) |
| CP5 | Child regions closed and unlinked | `child_count() == 0` | `child_count == 0` before Finalizing is entered (`:229-237`); a closing child unlinks itself (`:21-45`) |
| CP6 | No pending spawns | `pending_spawn_count() == 0` | no counterpart |

**Guard 1 (runtime-level):** `can_region_complete_close` (`src/runtime/state.rs:9654-9656`, core `:9665-9771`).

**Guard 2 (record-level):** `complete_close` re-checks the same conditions under the record's write lock before the CAS (`src/record/region.rs:1796-1808`).

**C status:** C performs one check sequence in `asx_region_finalize_one`; there is no second, record-level guard.

### 3.2 Precondition Failure Behavior

| Condition | Rust | C |
|-----------|------|---|
| Finalizers remain | Region stays Finalizing; the scheduler runs the next finalizer task (section 6) | not applicable (the cleanup stack is drained, not waited on) |
| Tasks remain | Region stays in its current closing state | `asx_region_finalize_one` returns `ASX_E_QUIESCENCE_TASKS_LIVE` (`src/runtime/quiescence.c:253`); `asx_region_drain` returns it too (`:339`) |
| Obligations remain, tasks gone | Leak audit marks them Leaked (or aborts them under Recover), then close proceeds | Region stays Finalizing; `ASX_E_OBLIGATIONS_UNRESOLVED` (bd-9kll.3.4) |
| Child regions remain | Region waits in Draining | Region waits in Draining; `ASX_E_PENDING` (`src/runtime/quiescence.c:229-237`) |

In both engines a failed attempt leaves the region where it was; the next task completion or child close re-runs the advance.

---

## 4. Obligation Leak Detection

### 4.1 Obligation Lifecycle Summary

> **Superseded by** v4 §3.4 (rules RESERVE `rule.obligation.reserve` #13, COMMIT `rule.obligation.commit` #14, ABORT `rule.obligation.abort` #15, LEAK `rule.obligation.leak` #16) and v4 §3.4.4 / §5 INV-OBLIGATION-LINEAR (rule `inv.obligation.linear`, #18). **C status:** see `C_REFINEMENT_MAP.md` rows `rule.obligation.reserve`, `rule.obligation.commit`, `rule.obligation.abort`, `rule.obligation.leak`, `inv.obligation.linear`.

```
Reserved ──(commit)──> Committed    (terminal)
    |
    ├──(abort)──> Aborted           (terminal)
    |
    └──(leak)──> Leaked             (terminal, error state)
```

Summary. `ObligationToken` (`src/obligation/graded.rs:874-875`) is `#[must_use]` and is consumed by `commit()`/`abort()`. Correction to the earlier version: a double resolution does not always panic. Through the runtime, it is an error: `ObligationTable::commit` returns `ErrorKind::ObligationAlreadyResolved` for a record that is no longer pending (`src/runtime/obligation_table.rs:576-578`; code `ASUP-E102`, `src/error.rs:73`, `:167`), and a commit after the region was finalized returns `RegionFinalized` (`src/runtime/obligation_table.rs:567-571`). Only the record-level methods `ObligationRecord::commit`/`abort`/`mark_leaked` assert and panic with "obligation already resolved" (`src/record/obligation.rs:391`).

**C status:** Committed, Aborted and Leaked are absorbing in the transition table (`src/core/transition_tables.c:65-70`). A second commit or abort returns `ASX_E_INVALID_TRANSITION` (`asx_obligation_commit`, `src/runtime/lifecycle.c:1377`; `asx_obligation_abort_with_reason`, `:1435`). `ASX_E_OBLIGATION_ALREADY_RESOLVED` (`include/asx/asx_status.h:52`) is declared but no file under `src/` returns it. Tests: `tests/invariant/lifecycle/test_lifecycle_legality.c` `obligation_double_commit_rejected`, `obligation_commit_then_abort_rejected`.

### 4.2 Detection Point 1: Holder Completion and Token Drop

> **Superseded by** v4 §3.4 LEAK (rule `rule.obligation.leak`, #16), §3.4.3, §3.4.6 (rule `inv.obligation.no_leak`, #17) and §3.4.7. **C status:** see `C_REFINEMENT_MAP.md` rows `rule.obligation.leak` and `inv.obligation.no_leak`.

Summary. Rust has three paths, in this order:

1. **Token drop.** Dropping an unresolved `ObligationToken` posts a `Leak` (or an `Abort` with reason `Error` when the token is `abort_on_drop`) (`Drop for ObligationToken`, `src/runtime/obligation_mailbox.rs:897-930`). This happens whether or not the holder was cancelled; fixtures `obligation-cancelled-holder-leaks-001` and `obligation-drop-leaks-at-once-001` show it.
2. **Completion audit.** `task_completed` calls `audit_completion_obligation_leaks` (`src/runtime/state.rs:8341-8368`), which collects the holder's still-pending obligations (`collect_obligation_leaks_for_holder`, `:5606-5624`) and handles them under the leak policy, but only when the completion is not `Cancelled` (`:8351`).
3. **Orphan abort.** Then `abort_orphaned_obligations_for_holder` (`:8453-8488`, called at `:8273` and `:8308`) aborts every obligation the holder still holds with reason `Cancel`, so a region is never stuck in Finalizing on them.

**C status:** on holder completion, `asx_task_on_complete_internal` (`src/runtime/lifecycle.c:550-570`) calls `asx_task_resolve_held_obligations_internal` (`:515-538`), which leaks every still-Reserved held obligation under the leak policy (`obligation_leak_slot`, `:493-513`), cancelled holder or not. There is no orphan abort with `Cancel`. The comment at `src/runtime/lifecycle.c:478-487` explains why: Rust's orphan abort reaches only tokens that were never dropped, and C, with no destructors, cannot tell those apart from dropped ones. `asx_obligation_drop` (`:1398-1415`) is the explicit drop path and leaks at once. Tests: `tests/unit/runtime/test_budget_obligation.c` `cancelled_holder_leaks_its_unresolved_obligation`, `drop_leaks_the_obligation_at_once_not_at_completion`. The `C_REFINEMENT_MAP.md` row `rule.obligation.leak` records the policy differences but not this one: when a *cancelled* holder completes while its token is still alive (not dropped), Rust skips the audit and aborts the obligation with `Cancel`, where C marks it Leaked. No v2 fixture is known to exercise that case (unverified).

### 4.3 Detection Point 2: Region Finalization

Rust, Finalizing arm (`src/runtime/state.rs:10271-10295`): when the region still has pending obligations and `task_count() == 0`, it collects every pending obligation of the region (`collect_obligation_leaks`, `:5568-5587`) and handles them under the leak policy with `task_id: None`. Correction: the condition is that the region has no tasks left (they are unlinked by `task_completed`), not that all tasks are terminal (comment `:10266-10270`).

**C status:** not implemented. `asx_region_finalize_one` stops at the first Reserved obligation (`src/runtime/quiescence.c:243-244`) and the region stays Finalizing; only explicit commit, abort or drop can unblock it. Rows `inv.obligation.ledger_empty_on_close` and `prog.region.close_terminates` (bd-9kll.3.4). Test: `tests/unit/runtime/test_quiescence.c` `quiescence_closed_region_with_unresolved_obligation_reports_error`, `tests/invariant/lifecycle/skeleton_test.c` `region_drain_blocks_unresolved_obligations`.

### 4.4 Leak Detection Ordering

Correction to the earlier version: the runtime's obligation table is not a `BTreeMap`. `ObligationTable` stores records in an `Arena<ObligationRecord>` (`src/runtime/obligation_table.rs:130-131`) with a per-holder index (`:136`):

- the region-finalization scan iterates the arena in slot order (`collect_obligation_leaks`, `src/runtime/state.rs:5578-5586`);
- the completion audit iterates the holder index in the order obligations were added to it (`ids_for_holder`, `src/runtime/obligation_table.rs:715-723`; entries pushed at `:357-367`);
- the orphan abort sorts the holder's pending ids by `ObligationId` (`sorted_pending_ids_for_holder`, `:725-742`).

`ObligationLedger` (`src/obligation/ledger.rs:441-443`) does use a `BTreeMap<ObligationId, ObligationRecord>`, but it is a separate component, not the table `RuntimeState` leak detection reads.

**C status:** obligations live in a fixed slot array scanned in slot order (`src/runtime/quiescence.c:47-60`). A holder's obligations are a singly linked list with each new reservation prepended (`src/runtime/lifecycle.c:1303-1308`), so completion leaks them newest first (`:515-538`), where Rust's completion audit goes oldest first. Whether any fixture can observe the order of two leaks from one holder is unverified.

### 4.5 Reentrance Guard

Correction to the earlier version: `handling_leaks` is a nesting depth (`usize`, `src/runtime/state.rs:2089`), not a boolean, and nested leak handling is allowed. Two mechanisms replace the old "suppress the inner detection" rule:

- `in_flight_leak_ids` (`:2096`) skips obligations an outer frame is already handling (`handle_obligation_leaks`, `:5684-5704`);
- while `handling_leaks > 0`, an obligation abort defers its region advance into `deferred_region_advancements` (`abort_obligation_in`, `:6891-6898`), and the outermost frame replays them in `RegionId` order (`finish_obligation_leak_batch`, `:5636-5650`), also before a Panic response unwinds.

`mark_obligation_leaked_in` still advances the region directly (`:7122`).

**C status:** no guard is needed in the current code: `obligation_leak_slot` (`src/runtime/lifecycle.c:493-513`) only changes the slot, emits a trace event and logs; it does not advance a region, so it cannot recurse.

### 4.6 Double-Panic Prevention

Rust (`src/runtime/state.rs:5726-5732`): if the effective response is `Panic` and `std::thread::panicking()` is true, the response is downgraded to `Log`.

**C status:** not applicable. C's PANIC policy does not abort the process: it marks the obligation Leaked and routes the fault through the region containment policy (`asx_region_contain_fault`, `src/runtime/lifecycle.c:551-562` and `:951-972`). There is no `asx_panicking` flag.

---

## 5. Leak Response Policies

v4 does not define leak policies: v4 §3.4 LEAK says only "In lab: panic or record error; In prod: log, recover, continue". This section describes the implementations.

### 5.1 Response Enum

| Policy | Rust behaviour (`handle_obligation_leaks`, `src/runtime/state.rs:5734-5822`) | C behaviour (`obligation_leak_slot`, `src/runtime/lifecycle.c:493-513`) |
|--------|----------|----------|
| `Panic` | Marks each leak Leaked, logs, then panics | `ASX_LEAK_PANIC`: marks Leaked, then routes the fault through region containment (`:551-562`) |
| `Log` | Marks Leaked and logs an error | `ASX_LEAK_LOG`: marks Leaked and logs a warning |
| `Silent` | Marks Leaked | `ASX_LEAK_SILENT`: marks Leaked |
| `Recover` | Aborts with `ObligationAbortReason::Error` instead of marking Leaked | `ASX_LEAK_RECOVER`: aborts with `ASX_OBLIGATION_ABORT_LEAK_RECOVERED` (`:500-508`) |

Rust enum: `ObligationLeakResponse` (`src/runtime/config.rs:1319-1333`). Defaults: `Panic` in `RuntimeConfig` (`src/runtime/config.rs:2242`) and in `RuntimeState`'s internal constructor (`src/runtime/state.rs:2338`); the lab uses `Panic` when `panic_on_obligation_leak` is set and `Log` otherwise (`src/lab/runtime.rs:2435-2439`).

**C status:** enum `asx_leak_response` (`include/asx/asx_config.h:530-535`), default `ASX_LEAK_LOG` (`src/runtime/lifecycle.c:72`, reset at `:147`). The default and the Recover abort reason differ from Rust: row `rule.obligation.leak` (bd-9kll.3.6). Rust parity under `Log`: `leak-policy-leak-reported-001` (`panic_on_leak: false`). Unit tests: `tests/unit/runtime/test_budget_obligation.c` `recover_policy_aborts_leaked_obligation`, `panic_policy_routes_leak_through_containment`.

### 5.2 Escalation Policy

Rust `LeakEscalation { threshold: u64, escalate_to: ObligationLeakResponse }` (`src/runtime/config.rs:1342-1347`; a threshold of 0 becomes 1, `:1354`). The cumulative leak count is incremented by the batch size before the threshold is compared (`src/runtime/state.rs:5711-5721`).

C:

```c
typedef struct {
    uint64_t threshold;
    asx_leak_response escalate_to;
} asx_leak_escalation_config;   /* include/asx/asx_config.h:544-548 */
```

**C status:** implemented, but C compares the count before incrementing it (`asx_leak_policy_effective`, `src/runtime/lifecycle.c:471-476`, increment at `:499`): row `rule.obligation.leak` (bd-9kll.3.6). Test: `leak_escalation_switches_policy_at_threshold`.

### 5.3 Diagnostic Information

Rust `LeakedObligationInfo` (`src/runtime/state.rs:5590-5601`) carries the obligation id, kind, holder task, region, `acquired_at`, held duration, description and an acquisition backtrace; the backtrace is captured only in debug builds (`:5555-5561`).

**C status:** a leak emits the trace event `ASX_TRACE_OBLIGATION_LEAK` with the obligation id only (`src/runtime/lifecycle.c:510`) and, under LOG, a fixed log message (`:511`). No holder, region, age or creation site is reported.

---

## 6. Finalizer Execution Contract

> **Superseded by** v4 §3.3 CLOSE-RUN-FINALIZER (rule `rule.region.close_run_finalizer`, #25): in Finalizing, pop finalizers LIFO and "run f as masked task". **C status:** see `C_REFINEMENT_MAP.md` row `rule.region.close_run_finalizer` (Partial).

### 6.1 Finalizer Types

Rust `Finalizer` (`src/record/finalizer.rs:17-28`):

```rust
pub enum Finalizer {
    Sync(Box<dyn FnOnce() + Send>),
    Async(Pin<Box<dyn Future<Output = ()> + Send>>),
}
```

Correction to the earlier version: both kinds normally run as masked finalizer tasks. The scheduler pops the top entry and wraps a `Sync` finalizer in an async block (`drain_ready_async_finalizers_in`, `src/runtime/state.rs:8578-8651`, wrapping at `:8612`), then spawns it with `spawn_finalizer_task_in` (`:8857-9028`). Sync finalizers run inline only on the fallback path taken when a finalizer task cannot start (`drive_failed_start_async_finalizer_inline`, `:8664`, using `run_sync_finalizers_tracked`, `:9518`). The enum's doc comment ("runs directly on scheduler thread", `src/record/finalizer.rs:18`) is accurate only for that fallback path.

**C status:** not implemented as finalizers. C regions have a cleanup stack of synchronous callbacks (`asx_cleanup_stack`, `include/asx/core/cleanup.h:43-50`; region field `src/runtime/runtime_internal.h:38`) drained inside `asx_region_finalize_one` (`src/runtime/quiescence.c:252`). No public API pushes onto a region's stack; only tests do, through the internal record (for example `tests/unit/runtime/test_quiescence.c`). DSL v2 has no finalizer op, so no Rust-captured fixture exercises finalizers.

### 6.2 Execution Order

LIFO: `FinalizerStack` keeps a `Vec` and pops from the back (`src/record/finalizer.rs:199-204`, `pop` at `:285`); `RegionRecord::pop_finalizer` returns an entry only in Finalizing (`src/record/region.rs:1533-1540`).

**C status:** the cleanup stack drains from the highest index down (`asx_cleanup_drain`, `src/core/cleanup.c:92-111`). Tests: `tests/unit/core/test_cleanup.c` `cleanup_drain_lifo_order`, `tests/unit/runtime/test_quiescence.c` `drain_cleanup_lifo_multiple`.

### 6.3 Async Finalizer Cancel Masking

Rust `MaskedFinalizer` (`src/runtime/state.rs:1781-1862`): `poll` enters the mask on first poll and exits it when the inner future is ready (`:1851-1862`). Correction: entering the mask at `MAX_MASK_DEPTH` now panics in release builds too (`enter_mask`, `:1796-1838`), instead of running the finalizer unmasked.

```rust
impl Future for MaskedFinalizer {
    fn poll(...) -> Poll<()> {
        self.enter_mask();
        let poll = self.inner.as_mut().poll(cx);
        if poll.is_ready() {
            self.exit_mask();
        }
        poll
    }
}
```

**C status:** not implemented. The C sketch in the earlier version (`asx_masked_finalizer_poll`, `asx_enter_cancel_mask`) names functions that do not exist. C cleanup callbacks are synchronous and cannot observe cancellation.

### 6.4 Bounded Finalizer Budget

| Constant | Value | Rust source |
|----------|-------|-------------|
| `FINALIZER_POLL_BUDGET` | 100 polls | `src/record/finalizer.rs:42` |
| `FINALIZER_TIME_BUDGET_NANOS` | 5,000,000,000 (5 s) | `src/record/finalizer.rs:45` |

A finalizer task gets `finalizer_budget()` (poll quota 100, `src/record/finalizer.rs:53-78`) with a deadline 5 s after it is spawned (`src/runtime/state.rs:8879-8882`), and is wrapped in `BudgetedFinalizer` (`:8923-8928`), which enforces a region shutdown ceiling when one is set (`src/record/finalizer.rs:402-431`). What happens when a finalizer task overruns the 100-poll quota was not traced (unverified).

**C status:** `asx_runtime_config.finalizer_poll_budget` and `finalizer_time_budget_ns` exist (`include/asx/asx_config.h:562-563`, defaults 100 and 5 s at `src/runtime/hooks.c:773-774`); `rt.c` rejects a zero poll budget (`src/runtime/rt.c:160`), but no C code reads either field otherwise (open bead bd-udlh).

### 6.5 Finalizer Escalation Policies

| Policy | Behavior (doc comments, `src/record/finalizer.rs:81-93`) | Default |
|--------|----------|---------|
| `Soft` | Wait indefinitely (strict correctness) | No |
| `BoundedLog` | Log warning and continue to next finalizer | **Yes** |
| `BoundedPanic` | Panic after budget exceeded | No |

Correction: at `5e60b1c4c` this enum is stored in `FinalizerStack` (`src/record/finalizer.rs:199-204`) but no runtime code outside `src/record/finalizer.rs` reads it (`git grep` finds only the re-export in `src/record/mod.rs:20`), so the policy described here is not wired into finalizer execution.

**C status:** `asx_finalizer_escalation` (`include/asx/asx_config.h:537-541`), default `ASX_FINALIZER_BOUNDED_LOG` (`src/runtime/hooks.c:775`), validated at `src/runtime/rt.c:157`, never otherwise read (bd-udlh).

### 6.6 Registration Rejection

Registration is rejected once the region has begun closing or is closed: `register_sync_finalizer` (`src/runtime/state.rs:9054-9060`) and `register_async_finalizer` (`:9107-9113`) return `false` when `is_closing() || is_terminal()`. `RegionRecord::add_finalizer` itself does not check the state (`src/record/region.rs:1525-1528`).

**C status:** not applicable: there is no registration API (section 6.1); `asx_cleanup_push` (`include/asx/core/cleanup.h:56-59`) takes a stack, not a region, and checks no region state.

### 6.7 Finalizer-Spawned Tasks

Rust admits cleanup tasks into a Finalizing region through `add_cleanup_task` (`src/record/region.rs:1438-1440`; predicate `can_accept_cleanup_work`, `:150`), and such a task bypasses `max_tasks` (`:1408`). Normal spawns are refused in Finalizing (`can_accept_work` is Open only, `:134-142`), as are child regions (`:1341-1354`) and obligation reserves (`:1449-1457`). A finalizing region closes only after its tasks are unlinked (`task_count() == 0`, `src/runtime/state.rs:9747-9751`).

**C status:** every spawn is admitted into a Finalizing region and is exempt from `max_tasks` (`src/runtime/lifecycle.c:1004-1021`): row `rule.ownership.spawn` (bd-9kll.3.5). A cleanup callback that spawns a task makes `asx_region_finalize_one` return `ASX_E_QUIESCENCE_TASKS_LIVE` after the drain (`src/runtime/quiescence.c:249-253`); a later drain closes the region (test `tests/unit/runtime/test_quiescence.c` `region_drain_finalizer_spawned_task_requires_followup_drain`). Child regions and obligation reserves are refused in Finalizing (`src/runtime/lifecycle.c:746`, `:1286`; test `finalizing_region_still_rejects_obligation_reserve`).

---

## 7. Bounded Cleanup Under Cancellation

### 7.1 Cancellation Cleanup Budget Table

> **Superseded in part by** v4 §3.2.1 (`cleanup_budget(reason, budget) = budget ∧ policy(reason)`) and §3.2 CANCEL-REQUEST (rule `rule.cancel.request`, #1). **C status:** see `C_REFINEMENT_MAP.md` row `rule.cancel.request`, whose note records that both engines use the per-kind table below rather than the spec's meet with the region budget.

| Cancel Kind Group | Cleanup Poll Quota | Cleanup Priority |
|-------------------|-------------------|-----------------|
| `User` | 1000 | 200 |
| `Timeout` / `Deadline` | 500 | 210 |
| `PollQuota` / `CostBudget` | 300 | 215 |
| `FailFast` / `RaceLost` / `LinkedExit` | 200 | 220 |
| `ParentCancelled` / `ResourceUnavailable` | 200 | 220 |
| `Shutdown` | 50 | 255 |

Rust source: `CancelReason::cleanup_budget` (`src/types/cancel.rs:1027-1043`). In `cancel_request_in` each task gets this budget, met with the region's shutdown-budget ceiling when one is set (`src/runtime/state.rs:7832-7833`), through `request_cancel_with_budget_and_publication`. Correction: the earlier version cited `CancelTokenState` in `src/cancel/symbol_cancel.rs`; that struct (`:80-107`, cleanup budget field at `:95`) belongs to the symbol-cancellation subsystem, not to task cancellation.

**C status:** same values, indexed by severity tier (`asx_cancel_cleanup_budget`, `src/core/cancel.c:15-42`). Rust parity of the snapshot's cleanup budget: `region-lifecycle-draining-with-child-region-001` (a Shutdown-cancelled task with poll quota 50, priority 255) and `cancel-request-event-reason-001` (PollQuota: 300, 215). No v2 fixture shows the other rows.

### 7.2 Budget Combination (Min-Plus Algebra)

> **Superseded by** v4 §1.4 (Budgets, `[Explanatory]`): `combine` takes the min of deadline, poll quota and cost quota and the max of priority. v4 gives it no rule ID.

Rust `Budget::combine_untraced` (`src/types/budget.rs:556-570`); `meet` is `combine` (`:592-594`).

**C status:** `asx_budget_meet` (`src/core/budget.c:49-58`): earliest finite deadline (0 is unconstrained), min quotas, max priority.

### 7.3 Cleanup Stack

This is a C design from the port plan (Section 6.8.D), not a Rust structure. The plan's four points and their status:

1. "Every reserve/acquire registers a cleanup action at acquisition time": **not implemented.** `asx_obligation_reserve_impl` (`src/runtime/lifecycle.c:1271-1320`) registers none; per-task stacks do not exist (`include/asx/core/cleanup.h:8-9` defers them).
2. "commit/abort pops entries": **not implemented** for obligations; `asx_cleanup_pop` exists (`include/asx/core/cleanup.h:61-65`).
3. "During finalization, unresolved entries are drained LIFO": **implemented** for the per-region stack (`src/runtime/quiescence.c:252`, `src/core/cleanup.c:92-111`).
4. "Discarding a token without resolution triggers detection during region finalization": C detects it at holder completion or `asx_obligation_drop` (section 4.2); at finalization an unresolved obligation blocks close (section 4.3).

### 7.4 Force-Completion

Correction to the earlier version: Rust never force-completes a task whose cleanup exceeds its budget, and has no `cleanup_budget_exceeded` flag (`git grep` finds no such identifier at `5e60b1c4c`). In the lab, a poll with no poll quota left only strengthens the task's cancel reason to `PollQuota` (`src/lab/runtime.rs:4663-4671`). The multi-worker scheduler does not charge a task in its cancellation cleanup phase at all: its cleanup budget stays advisory (`consume_budget_poll` and its doc comment, `src/runtime/scheduler/three_lane.rs:1266-1301`). v4 §3.2.3 is a proof sketch that assumes sufficient budgets, and v4 §6 PROG-CANCEL lists the premises (rule `prog.cancel.drains`, #9; C status row `prog.cancel.drains`, Partial).

**C status:** the default matches Rust (`cleanup_hard_bound = 0`, `src/runtime/hooks.c:778`; semantics in `include/asx/asx_config.h:567-577`). With the opt-in hard bound, the round-robin scheduler force-completes a cancelled, unmasked task whose cleanup polls are spent as Cancelled with `ASX_SCHED_EVENT_CANCEL_FORCED` (`src/runtime/scheduler.c:1383-1404`), a deliberate deviation excluded from parity. It sets no extra flag, and it does not force-abort obligations: completion goes through `sched_complete` (`src/runtime/scheduler.c:732`), whose completion hook (`:743`) leaks still-held obligations under the leak policy (section 4.2).

### 7.5 Exhaustion During Cleanup

| Exhaustion Type | Rust | C |
|-----------------|------|---|
| Poll quota exhausted | Lab: the cancel reason is strengthened to `PollQuota` at the next poll (`src/lab/runtime.rs:4663-4671`); parity `budget-poll-quota-exhaustion-001` | Same observable result in that fixture |
| Cost quota exhausted | `Budget::consume_cost` returns `false` and leaves the quota unchanged (`src/types/budget.rs:613`) | `asx_budget_consume_cost` returns 0 without mutation (`src/core/budget.c:65-71`) |
| Deadline exceeded | The task is cancelled with `Deadline` through its budget deadline (not a region `Timeout` as v4 §3.6 TICK says) | Same: see `C_REFINEMENT_MAP.md` supplementary row "§3.6 TICK"; parity `budget-deadline-sleep-checkpoint-001` |

---

## 8. Close Rejection and Admission Gating

> **Superseded by** v4 §3.1 SPAWN (rule `rule.ownership.spawn`, #36: `R[r].state = Open`) and v4 §3.4 RESERVE (rule `rule.obligation.reserve`, #13). **C status:** see `C_REFINEMENT_MAP.md` rows `rule.ownership.spawn` (Partial: C admits spawns in Finalizing, bd-9kll.3.5) and `rule.obligation.reserve` (Partial: C allows a reserve with no holder).

### 8.1 Optimistic Double-Check Locking

Rust admission (doc comment `src/record/region.rs:166-206`): (1) an atomic fast-path check of the relevant predicate, (2) take the record's write lock, (3) re-check the predicate, (4) check the limit, (5) commit. The predicate depends on the operation, correcting the earlier version's "`can_spawn()` everywhere":

| Operation | Rust predicate | Source |
|-----------|----------------|--------|
| `add_child` | `can_spawn()` (Open) | `src/record/region.rs:1343-1373` |
| `add_task` | `can_accept_work()` (Open) | `:1381-1432` |
| `add_cleanup_task` | `can_accept_cleanup_work()` (Open, Finalizing); no limit in Finalizing | `:1381-1424`, `:1438-1440` |
| `try_reserve_obligation` | `can_accept_work()` (Open) | `:1449-1472` |
| `heap_alloc` | `!is_terminal()` | `:1575-1604` |

`begin_close` makes the Open → Closing transition while holding the same write lock, so a locked re-check cannot see Open after close began (`:1682-1738`).

**C status:** the kernel's admission checks run in one place each, without locks: `asx_task_spawn` (`src/runtime/lifecycle.c:1004-1021`), `asx_region_open_child` (`:743-751`), `asx_obligation_reserve_impl` (`:1282-1291`).

### 8.2 Admission Errors

| Rust | When | C |
|-------|------|--------|
| `AdmissionError::Closed` (`src/record/region.rs:269-281`) | the predicate is false | `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:746`, `:1011`, `:1105`, `:1286`) |
| `AdmissionError::LimitReached { kind, limit, live }` | a configured limit is reached | `ASX_E_ADMISSION_LIMIT` (`src/runtime/lifecycle.c:749`, `:1020`, `:1290`); limits set with `asx_region_set_limits` (`:873-881`); parity `region-limits-admission-001` |
| `SpawnError::RegionNotFound` (`src/runtime/state.rs:1622-1660`) | stale or unknown region | `ASX_E_NOT_FOUND` or `ASX_E_STALE_HANDLE` (`asx_region_slot_lookup`, `src/runtime/lifecycle.c:206-224`) |
| — | poisoned region | `ASX_E_REGION_POISONED` (`src/runtime/lifecycle.c:743`, `:1002`, `:1282`) |

`ASX_E_ADMISSION_CLOSED` (`include/asx/asx_status.h:40`) is not returned by the region, task or obligation kernel; other modules use it (for example `src/runtime/adapter.c`). `ASX_E_REGION_NOT_FOUND` (`include/asx/asx_status.h:36`) is not returned by any file under `src/`.

### 8.3 State-Gated Operations

| Operation | Rust: allowed states | C: allowed states |
|-----------|---------------|-------|
| Create child task | `Open`; cleanup tasks also in `Finalizing` | `Open` and `Finalizing` for every spawn (bd-9kll.3.5) |
| Create child region | `Open` only | `Open` only (`src/runtime/lifecycle.c:746`) |
| Reserve obligation | `Open` only | `Open` only (`src/runtime/lifecycle.c:1286`); parity `obligation-reserve-closed-region-must-fail-001` (Closing) |
| Register finalizer | `Open` only (section 6.6) | no API |
| Query status | Any | Any |
| Region heap / arena | any state but `Closed` (`heap_alloc`) | the capture arena is reached only through `asx_task_spawn_captured`, so it follows spawn admission (`src/runtime/lifecycle.c:1105`) |

### 8.4 Spawn Error Variants

Rust `SpawnError` (`src/runtime/state.rs:1622-1660`): `RuntimeUnavailable`, `RegionNotFound`, `RegionClosed`, `LocalSchedulerUnavailable`, `NameRegistrationFailed`, `RegionAtCapacity`, `AuthorizationDenied`, `AdmissionSlotAlreadyReserved`.

**C status:** `asx_task_spawn` (`src/runtime/lifecycle.c:990-1089`) returns `ASX_E_INVALID_ARGUMENT`, `ASX_E_NOT_FOUND` / `ASX_E_STALE_HANDLE`, `ASX_E_REGION_POISONED`, `ASX_E_REGION_CLOSED`, `ASX_E_ADMISSION_LIMIT`, or `ASX_E_RESOURCE_EXHAUSTED` when no task slot is free (`asx_task_slot_alloc`, `:394-409`). `ASX_E_REGION_AT_CAPACITY`, `ASX_E_SCHEDULER_UNAVAILABLE` and `ASX_E_NAME_CONFLICT` (`include/asx/asx_status.h:38`, `:46`, `:47`) are declared but not returned by any file under `src/`. Parity: `spawn-into-cancelled-region-step-001`, `region-lifecycle-closed-child-spawn-001` (`ASX_E_REGION_CLOSED`), `region-limits-admission-001` (`ASX_E_ADMISSION_LIMIT`).

---

## 9. Timer/Channel Coupling to Quiescence

v4 has no rule tying timers or channels to quiescence, and neither engine's quiescence check inspects them directly.

### 9.1 Timer Quiescence

- Rust `RuntimeState::is_quiescent` has no timer conjunct (`src/runtime/state.rs:7400-7413`). In the lab, a pending sleep is completed by auto-advancing virtual time; parity `quiescence-pending-timer-001` (a task sleeps 500 ns, the finish phase advances to the timer, the run ends quiescent with no timer pending).
- C `asx_runtime_is_quiescent` has no timer conjunct either (`src/runtime/rt.c:374-383`). A completing task's sleep timer is disarmed (`asx_task_timer_disarm_internal`, called at `src/runtime/lifecycle.c:563`).
- Stale-handle cancellation and equal-deadline ordering: see `docs/CHANNEL_TIMER_SEMANTICS.md` §2.5, §2.6 and §2.8.

### 9.2 Channel Quiescence

- An unresolved send permit is a pending obligation in both engines, so it holds region close the way any obligation does: Rust registers a `SendPermit` obligation through the `Cx` (`docs/CHANNEL_TIMER_SEMANTICS.md` §1.9); C's `asx_channel_reserve` (`src/channel/mpsc.c:827`) registers an `ASX_OBLIGATION_KIND_SEND_PERMIT` obligation (`:841`).
- The capacity invariant `queue_len + reserved <= capacity`: `docs/CHANNEL_TIMER_SEMANTICS.md` §1.2.
- Queued messages do not affect quiescence in either engine; C region close does not touch channels (`docs/CHANNEL_TIMER_SEMANTICS.md` §4.1).

### 9.3 Quiescence Error Diagnostics

| Check | Error Code | Returned by C? |
|-------|-----------|---|
| Active tasks remain | `ASX_E_TASKS_STILL_ACTIVE` (`include/asx/asx_status.h:79`) | no (bd-udlh); C returns `ASX_E_QUIESCENCE_TASKS_LIVE` (`:84`) |
| Obligations unresolved | `ASX_E_OBLIGATIONS_UNRESOLVED` (`:80`) | yes (`src/runtime/quiescence.c:55`) |
| Regions not closed | `ASX_E_REGIONS_NOT_CLOSED` (`:81`) | no; C returns `ASX_E_QUIESCENCE_NOT_REACHED` (`:83`) for a region that is not Closed |
| Timers pending | `ASX_E_TIMERS_PENDING` (`:73`) | no |
| Channel not drained | `ASX_E_CHANNEL_NOT_DRAINED` (`:67`) | no |

---

## 10. Deterministic Ordering Guarantees

### 10.1 Finalizer Execution Order

> **Superseded by** v4 §3.3 CLOSE-RUN-FINALIZER (rule `rule.region.close_run_finalizer`, #25: LIFO). **C status:** row `rule.region.close_run_finalizer`; section 6.2 above.

### 10.2 Obligation Iteration Order

See section 4.4: arena slot order for the region scan, holder-index order for the completion audit, `ObligationId` order for the orphan abort. C: slot order for scans; newest-first for a holder's leaks.

### 10.3 Region Close Cascade Order

Rust: when a child region closes, it records its cleanup outcome and descendant panic on the parent, removes itself from the parent's child list, and the walk continues with the parent (`src/runtime/state.rs:10371-10398`). Child regions are kept in insertion order, and removal preserves that order (`Membership`, `src/record/region.rs:326-339`, `remove` at `:417-425`).

**C status:** the advance loop continues with the parent (`src/runtime/quiescence.c:284-287`). Unlinking moves the last child into the freed slot of `children[]` (`src/runtime/quiescence.c:30-37`), so after a removal C's child order is no longer insertion order. See section 10.4 for where that order matters.

### 10.4 Cancel Propagation Order

> **Superseded by** v4 §3.2 CANCEL-REQUEST (rule `rule.cancel.request`, #1) and v4 §5 INV-CANCEL-PROPAGATES (rule `inv.cancel.propagates_down`, #6). **C status:** rows `rule.cancel.request` and `inv.cancel.propagates_down`.

Rust `cancel_request_in`: the subtree is collected depth-first with an explicit stack (`collect_region_and_descendants_with_depth`, `src/runtime/state.rs:7904-7931`; children are pushed in membership order and therefore popped last-first), then stably sorted by depth (`:7653`). The first pass visits regions in that order (`:7665`), the second pass cancels each region's tasks in membership order (`:7811-7830`).

**C status:** `asx_region_cancel` (`src/runtime/cancellation.c:430-496`) takes the subtree breadth-first over `children[]` in array order (`asx_region_subtree_internal`, `src/runtime/lifecycle.c:587-610`), runs the region pass (`src/runtime/cancellation.c:442-479`), then the task pass (`cancel_subtree_tasks`, `:345-383`, each region's tasks in membership order), then advances idle regions (`:484-491`). Possible drift, not recorded in `C_REFINEMENT_MAP.md`: for a cancelled region with two or more child regions at the same depth, Rust visits the siblings in reverse membership order and C in `children[]` order (itself perturbed by unlinking, section 10.3). If those siblings hold live tasks, the order of cancel-lane pushes differs, and the lab's seeded pick among equal-priority entries can then choose a different task. No v2 fixture is known to cancel a region holding two child regions with live tasks; this was not checked by running anything (unverified).

### 10.5 Task Completion Notification Order

Not stated by v4 and not re-verified for Rust in this pass. Under lab dispatch both engines poll one task per step, so completions follow the dispatch order (`docs/CHANNEL_TIMER_SEMANTICS.md` §3.2). C's round-robin scheduler polls runnable tasks in ascending arena order each round (`docs/CHANNEL_TIMER_SEMANTICS.md` §3).

---

## 11. Convergence and Termination Proofs

> **Superseded by** v4 §6 (rules PROG-CANCEL `prog.cancel.drains` #9, PROG-REGION `prog.region.close_terminates` #28, PROG-OBLIGATION `prog.obligation.resolves` #21). v4 §6 states these as conditional liveness properties: fairness alone does not make them hold, and "runtime budgets and watchdogs do not prove those premises". **C status:** see `C_REFINEMENT_MAP.md` rows `prog.cancel.drains`, `prog.region.close_terminates`, `prog.obligation.resolves` (all Partial: finite-run evidence only).

### 11.1 Lyapunov Potential Function

From `src/obligation/lyapunov.rs` (module doc, `:5-31`), a *candidate* potential used by a scheduling governor:

```
V(Sigma) = w_t * |live_tasks|
         + w_o * SUM(age(obligation, now))
         + w_r * |draining_regions|          (regions in Draining or Finalizing)
         + w_d * SUM(max(0, 1 - slack(t, now) / D0))
```

with the stated properties `V >= 0`, `V = 0` iff quiescent, and `V` non-increasing under scheduling steps (`:10-12`).

**C status:** none. `src/runtime/barrier_cert_spike.c` evaluates Lyapunov-style decrease conditions but is a research spike, not runtime policy (its header, `:1-17`).

### 11.2 Progress Certificates (Drain Termination)

Correction to the earlier version: `src/cancel/progress_certificate.rs` provides *diagnostics*, not a termination proof. Its module doc (`:1-52`) says that one observed trace "does not itself prove bounded-time termination" (`:7-8`); the Azuma-Hoeffding candidate is `P(V(Σₜ) > V(Σ₀) - t·μ + λ) ≤ exp(-λ² / (2·t·c²))` (`:17`), not the earlier `exp(-2λ²/(t·c²))`; and the gross-credit sum is "not used as a supermartingale, Ville, or optional-stopping certificate" (`:49-52`). The earlier claim "This proves drain always terminates under bounded cleanup budgets" is withdrawn.

**C status:** none.

### 11.3 Cleanup Budget Bound

Withdrawn. The earlier bound assumed that a spent cleanup budget force-completes the task. Rust cleanup budgets are advisory (section 7.4), so no `max(Q_max, T_max) * task_count` drain bound follows from them. C's opt-in hard bound (section 7.4) does force completion, but it is excluded from parity.

### 11.4 Recovery Convergence (Deferred to Wave B)

`src/obligation/recovery.rs` resolves stale `Reserved` obligations (module doc `:31`). `RecoveryConfig::default()`: stale timeout 30 s, `max_resolutions_per_tick` 50 (`:80-88`); `default_for_test()`: 5 s and 100 (`:70-76`).

**C status:** none.

---

## 12. Oracle System

v4 §8 describes oracle usage (`[Implementation]`).

### 12.1 Verification Oracles

| Oracle | Rust file | Invariant (module doc) | C |
|--------|------|-----------|--------------|
| `QuiescenceOracle` | `src/lab/oracle/quiescence.rs:86` | region close implies its tasks completed, child regions closed, finalizers run, ledger empty | No oracle. Close itself checks these (`src/runtime/quiescence.c:224-263`); `asx_quiescence_check_detailed` reports Q1-Q4 |
| `FinalizerOracle` | `src/lab/oracle/finalizer.rs:84` | every registered finalizer ran before close | none |
| `TaskLeakOracle` | `src/lab/oracle/task_leak.rs:65` | every task of a closed region completed | none |
| `ObligationLeakOracle` | `src/lab/oracle/obligation_leak.rs:88` | obligations resolved before their region closes | ghost linearity check `asx_ghost_check_obligation_leaks` (`src/core/ghost.c:182-208`), which reports every unresolved obligation in its table whatever the region (`:201`) |
| `NoLeakProver` | `src/obligation/no_leak_proof.rs:389` | ghost-counter proof | none |

The C names in the earlier version (`asx_ghost_quiescence_check`, `asx_ghost_finalizer_check`, `asx_ghost_task_leak_check`, `asx_ghost_obligation_leak_check`, `asx_ghost_no_leak_proof`) do not exist. The open bead bd-9kll.9.2 tracks mapping Rust's lab oracles to C.

### 12.2 NoLeakProver Liveness Properties

`LivenessProperty` (`src/obligation/no_leak_proof.rs:204-218`):

| # | Property | Statement |
|---|----------|-----------|
| NL1 | `CounterIncrement` | Ghost counter increases on reserve |
| NL2 | `CounterDecrement` | Ghost counter decreases on resolve (commit/abort/leak) |
| NL3 | `CounterNonNegative` | Ghost counter >= 0 at all times |
| NL4 | `TaskCompletion` | Task completion implies zero pending for that task |
| NL5 | `RegionQuiescence` | Region closure implies zero pending for that region |
| NL6 | `EventualResolution` | All obligations resolved at trace end |
| NL7 | `DropPathCoverage` | Drop path correctly resolves (leak is still a resolution) |

The variant names were checked; the one-line statements are carried over from the earlier version and were not re-read (unverified). **C status:** none.

### 12.3 Ghost Monitor Integration (Debug Builds)

**C status:** the ghost monitors are compiled in when `ASX_DEBUG_GHOST` is defined, which `ASX_DEBUG` turns on unless `ASX_DEBUG_GHOST_DISABLE` is set (`include/asx/core/ghost.h:28-33`). Otherwise every ghost entry point is a macro stub with no cost (`include/asx/core/ghost.h:187-214`). The `ASX_GHOST_CHECK_*` macros of the earlier version do not exist. On the close path the monitors are called through `asx_ghost_check_region_transition` (`src/runtime/quiescence.c:216`) and `asx_ghost_check_obligation_leaks` (`:247`).

---

## 13. Forbidden Behavior Catalog

### 13.1 Quiescence/Finalization Forbidden Behaviors

| ID | Forbidden Behavior | Rust result | C result |
|----|-------------------|-----------------|-----------------|
| QF-001 | Close region with active child tasks | Region waits in Closing (or Draining if it has child regions) until the tasks are unlinked (`src/runtime/state.rs:7954-7992`) | Waits; `asx_region_drain` returns `ASX_E_QUIESCENCE_TASKS_LIVE` while tasks remain (`src/runtime/quiescence.c:339`) |
| QF-002 | Close region with unresolved obligations | Finalizing-arm leak audit marks them Leaked (policy), then close (`:10271-10295`) | Region stays Finalizing, `ASX_E_OBLIGATIONS_UNRESOLVED` (`src/runtime/quiescence.c:243-244`; bd-9kll.3.4) |
| QF-003 | Close region with pending finalizers | Region stays Finalizing until each finalizer task completes (`:10247-10264`) | not applicable: cleanup callbacks run synchronously at close |
| QF-004 | Close region with open child regions | Waits in Draining | Waits in Draining, `ASX_E_PENDING` (`src/runtime/quiescence.c:229-237`) |
| QF-005 | Register finalizer after close initiated | `register_*_finalizer` returns `false` (`src/runtime/state.rs:9054-9060`, `:9107-9113`) | no registration API |
| QF-006 | Spawn child region during `Finalizing` | `AdmissionError::Closed` (`src/record/region.rs:1341-1354`) | `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:746`); the earlier `ASX_E_ADMISSION_CLOSED` was wrong |
| QF-007 | Reserve obligation during `Finalizing` | `AdmissionError::Closed` (`src/record/region.rs:1449-1457`) | `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:1286`); test `finalizing_region_still_rejects_obligation_reserve` |
| QF-008 | Double-resolve obligation | `ObligationAlreadyResolved` error through the runtime; panic only at record level (section 4.1) | `ASX_E_INVALID_TRANSITION` (section 4.1) |
| QF-009 | Use region heap after `Closed` | `AdmissionError::Closed` from `heap_alloc` (`src/record/region.rs:1579-1589`) | `ASX_E_REGION_CLOSED` from `asx_task_spawn_captured` (`src/runtime/lifecycle.c:1105`) |
| QF-010 | Backward region state transition | CAS fails (`src/record/region.rs:310-319`) | `ASX_E_INVALID_TRANSITION` (`src/core/transition_tables.c:22-28`, `:72-75`) |
| QF-011 | Skip intermediate close state | No such CAS path exists | `ASX_E_INVALID_TRANSITION` (same table) |
| QF-012 | Recursive leak detection | Allowed but deduplicated (`in_flight_leak_ids`) with deferred region advances (section 4.5) | Cannot happen (section 4.5) |
| QF-013 | Quiescence report with pending timers | No timer conjunct (section 9.1) | `ASX_E_TIMERS_PENDING` is never returned (section 9.3) |
| QF-014 | Quiescence report with undrained channels | No channel conjunct (section 9.2) | `ASX_E_CHANNEL_NOT_DRAINED` is never returned (section 9.3) |
| QF-015 | Silent cleanup-budget overrun | Not forbidden in Rust: the budget is advisory (section 7.4) | Default advisory; the opt-in hard bound emits `ASX_SCHED_EVENT_CANCEL_FORCED` (`src/runtime/scheduler.c:1383-1404`) |

### 13.2 Cross-Reference to bd-296.15 Lifecycle Tables

| This Doc | bd-296.15 (`docs/LIFECYCLE_TRANSITION_TABLES.md` §8) | Relationship |
|----------|-----------|--------------|
| QF-001 | FB-012 | Same invariant (close with active children) |
| QF-002 | FB-011 | Same invariant (close with unresolved obligations) |
| QF-008 | FB-003/004/005/006 | Expanded (all double-resolve variants) |
| QF-009 | FB-014 | Same invariant (access after close) |
| QF-010 | FB-007 | Same invariant (backward transition) |
| QF-011 | FB-008 | Same invariant (skip intermediate state) |

---

## 14. Fixture Family Mapping

None of the candidate fixture IDs below exists in the repository: none is `fixtures/rust_reference_v2/<id>.json`, and `grep -rl <id> fixtures tests tools src` finds no file for any of them (checked 2026-10-10). Each row says "not materialized" and, where a Rust-captured v2 fixture covers the same behaviour (its scenario was read), names it. DSL v2 has no finalizer op and runs one leak policy per scenario, so several families cannot be captured as v2 fixtures today.

### 14.1 Quiescence Fixtures

| Fixture ID | Description | Invariants | Status |
|-----------|-------------|------------|--------|
| `quiescence-001` | Empty region closes immediately | Q1-Q4 trivially satisfied | Not materialized |
| `quiescence-002` | Region waits for single child task | Q1 enforced | Not materialized; covered by `region-lifecycle-close-cancels-children-001` (the closing child region waits for its cancelled sleeper, then closes) |
| `quiescence-003` | Region waits for nested child region | Q2 enforced; cascade close | Not materialized; partly covered by `region-lifecycle-draining-with-child-region-001` (a closing region with a child region waits in Draining) |
| `quiescence-004` | Region waits for obligation resolution | Q3 enforced | Not materialized; C differs from Rust here (section 4.3) |
| `quiescence-005` | Region waits for finalizer execution | Q4 enforced | Not materialized; no DSL v2 finalizer op |
| `quiescence-006` | Full four-conjunct quiescence | All Q1-Q4 | Not materialized |
| `quiescence-007` | Runtime-level quiescence after shutdown | RQ1-RQ5 | Not materialized; every v2 fixture compares the snapshot's `quiescent` flag, e.g. `quiescence-pending-timer-001` |
| `quiescence-008` | Quiescence with cancel-masked finalizer task | Q1 includes finalizer-spawned tasks | Not materialized; no DSL v2 finalizer op |

### 14.2 Close Precondition Fixtures

| Fixture ID | Description | Invariants | Status |
|-----------|-------------|------------|--------|
| `close-precond-001` | Close gate rejects when tasks remain | CP3 | Not materialized; partly covered by `region-lifecycle-close-cancels-children-001` |
| `close-precond-002` | Close gate rejects when obligations remain | CP4 | Not materialized; C differs (section 4.3) |
| `close-precond-003` | Close gate rejects when children remain | CP5 | Not materialized; partly covered by `region-lifecycle-draining-with-child-region-001` |
| `close-precond-004` | Close gate rejects when finalizers remain | CP2 | Not materialized; no DSL v2 finalizer op |
| `close-precond-005` | Double-guard prevents TOCTOU race | CP1-CP5 both guards | Not materialized; not applicable to C (one guard, section 3.1) |
| `close-precond-006` | Close succeeds when all preconditions met | CP1-CP5 all pass | Not materialized; covered by `region-lifecycle-close-cancels-children-001` and `region-lifecycle-cancel-propagates-001` (both cancelled regions end Closed) |

### 14.3 Leak Detection Fixtures

| Fixture ID | Description | Invariants | Status |
|-----------|-------------|------------|--------|
| `leak-detect-001` | Task completion triggers leak scan | Detection Point 1 | Not materialized; covered by `obligation-cancelled-holder-leaks-001` (a cancelled holder's Lease ends Leaked) |
| `leak-detect-002` | Region finalization triggers leak scan | Detection Point 2 | Not materialized; not implemented in C (section 4.3) |
| `leak-detect-003` | Leak response: Panic | Policy enforcement | Not materialized (C unit test `panic_policy_routes_leak_through_containment`) |
| `leak-detect-004` | Leak response: Log | Policy enforcement | Not materialized; covered by `leak-policy-leak-reported-001` (`panic_on_leak: false`, which the Rust lab maps to Log, `src/lab/runtime.rs:2435-2439`) |
| `leak-detect-005` | Leak response: Silent | Policy enforcement | Not materialized |
| `leak-detect-006` | Leak response: Recover (auto-abort) | Policy enforcement | Not materialized (C unit test `recover_policy_aborts_leaked_obligation`; reason differs from Rust, bd-9kll.3.6) |
| `leak-detect-007` | Escalation threshold triggers stricter response | Escalation policy | Not materialized (C unit test `leak_escalation_switches_policy_at_threshold`) |
| `leak-detect-008` | Reentrance guard prevents recursive detection | Reentrance flag | Not materialized |
| `leak-detect-009` | Double-panic prevention (downgrade to Log) | Thread-panicking check | Not materialized; not applicable to C |
| `leak-detect-010` | Deterministic leak iteration order | Ordering (section 4.4) | Not materialized |
| `leak-detect-011` | Orphaned obligations auto-aborted at task completion | Orphan abort | Not materialized; C leaks instead (section 4.2) |
| `leak-detect-012` | NoLeakProver ghost counter properties (NL1-NL7) | All 7 liveness properties | Not materialized; no C prover |

`obligation-drop-leaks-at-once-001` (a dropped SendPermit leaks at the drop, not at holder completion) covers the token-drop path of section 4.2, which had no candidate ID.

### 14.4 Finalizer Fixtures

| Fixture ID | Description | Invariants | Status |
|-----------|-------------|------------|--------|
| `finalizer-001` | Sync finalizer runs | Execution order | Not materialized; no DSL v2 finalizer op |
| `finalizer-002` | Async finalizer runs as masked task | Cancel masking | Not materialized; no DSL v2 finalizer op |
| `finalizer-003` | LIFO execution order | Reverse registration | Not materialized (C unit test `cleanup_drain_lifo_order`) |
| `finalizer-004` | Finalizer poll budget exceeded | Budget enforcement | Not materialized |
| `finalizer-005` | Finalizer time budget exceeded | Budget enforcement | Not materialized |
| `finalizer-006` | BoundedLog escalation (default) | Escalation policy | Not materialized; the policy is not wired at `5e60b1c4c` (section 6.5) |
| `finalizer-007` | Soft escalation (wait indefinitely) | Escalation policy | Not materialized; as `finalizer-006` |
| `finalizer-008` | BoundedPanic escalation | Escalation policy | Not materialized; as `finalizer-006` |
| `finalizer-009` | Finalizer registration rejected after close | Admission gating | Not materialized; no C registration API |
| `finalizer-010` | Finalizer spawns cleanup task | Task creation during Finalizing | Not materialized (C unit test `region_drain_finalizer_spawned_task_requires_followup_drain`) |

### 14.5 Cleanup Budget Fixtures

| Fixture ID | Description | Invariants | Status |
|-----------|-------------|------------|--------|
| `cleanup-budget-001` | User cancel: 1000 poll quota | Budget table | Not materialized; no v2 snapshot shows a User cleanup budget |
| `cleanup-budget-002` | Shutdown cancel: 50 poll quota | Budget table | Not materialized; covered by `region-lifecycle-draining-with-child-region-001` (snapshot cleanup budget 50 / 255) |
| `cleanup-budget-003` | Budget combination: min-plus algebra | Monotone narrowing | Not materialized |
| `cleanup-budget-004` | Force-completion on budget exceeded | Bounded cleanup | Not materialized; Rust never force-completes (section 7.4) |
| `cleanup-budget-005` | Cleanup stack LIFO drain | Reverse order | Not materialized; C-only structure (C unit test `drain_cleanup_lifo_multiple`) |
| `cleanup-budget-006` | Cleanup under poll quota exhaustion | Exhaustion mapping | Not materialized; covered by `budget-poll-quota-exhaustion-001` |
| `cleanup-budget-007` | Cleanup under deadline exhaustion | Exhaustion mapping | Not materialized; covered by `budget-deadline-sleep-checkpoint-001` |

### 14.6 Cascade Close Fixtures

| Fixture ID | Description | Invariants | Status |
|-----------|-------------|------------|--------|
| `cascade-close-001` | Child close cascades to parent advance | Bottom-up propagation | Not materialized; partly covered by `region-lifecycle-cancel-propagates-001` (parent and child both end Closed) |
| `cascade-close-002` | Deep nesting (3+ levels) cascade | Iterative not recursive | Not materialized (C unit test `region_drain_nested_grandchild_closes_inside_out`) |
| `cascade-close-003` | Multiple children close in deterministic order | Ordering guarantee | Not materialized; see the possible drift in section 10.4 |
| `cascade-close-004` | Shutdown initiates root region close | Runtime shutdown | Not materialized |
| `cascade-close-005` | Cascade with mixed sync/async finalizers | Mixed finalization | Not materialized; no DSL v2 finalizer op |

---

## 15. C Port Contract

### 15.1 Structural Requirements

| # | Requirement | Status in C |
|---|-------------|-------------|
| C1 | Region quiescence predicate | Implemented in a different shape: `asx_region_is_quiescent(asx_region_id)` is true only for a Closed region, and its Q4 is the cleanup stack (section 1.1) |
| C2 | Runtime quiescence predicate | Implemented with different conjuncts: requires every region Closed, where Rust only requires none to be closing (section 1.2) |
| C3 | Region close `Open→Closing→Draining→Finalizing→Closed` | Implemented by the transition table, without CAS (section 2.1) |
| C4 | Advance must be iterative (not recursive) | Implemented (`src/runtime/quiescence.c:265-289`) |
| C5 | Double guard on Finalizing→Closed | Not implemented: one check sequence (section 3.1) |
| C6 | Leak detection at task completion and region finalization | Partly: at holder completion and explicit drop only; no finalization-time audit (sections 4.2-4.3; bd-9kll.3.4) |
| C7 | Configurable leak response with escalation | Implemented with differences: default LOG, count checked before increment, Recover reason (section 5; bd-9kll.3.6) |
| C8 | Finalizer LIFO with bounded budget | LIFO only, for the C cleanup stack; the budget fields are not read (sections 6.2, 6.4; bd-udlh) |
| C9 | Cancel-masked async finalizers | Not implemented (section 6.3) |
| C10 | Cleanup stack per task/region with LIFO drain | Per region only, with no public registration API (section 7.3) |
| C11 | Sorted obligation storage (BTreeMap equivalent) | Not applicable as stated: Rust's runtime table is an arena too (section 4.4). C scans slots in order; a holder's leaks go newest first |
| C12 | Reentrance guard for leak detection | Not needed in the current code (section 4.5) |
| C13 | Ghost monitors: debug builds only, zero-cost in release | Implemented (section 12.3) |

### 15.2 Error Codes Required

| Error Code | Context | Returned by a file under `src/`? |
|------------|---------|---|
| `ASX_E_ADMISSION_CLOSED` | Region not accepting work | Not by the region kernel (section 8.2); C uses `ASX_E_REGION_CLOSED` |
| `ASX_E_ADMISSION_LIMIT` | Capacity limit reached | Yes |
| `ASX_E_REGION_NOT_FOUND` | Stale/invalid region handle | No; C uses `ASX_E_NOT_FOUND` / `ASX_E_STALE_HANDLE` |
| `ASX_E_REGION_CLOSED` | Admission refused by a closing or closed region | Yes |
| `ASX_E_UNRESOLVED_OBLIGATIONS` | Obligations remain at close attempt | Only as the fault passed to region containment by the PANIC leak policy (`src/runtime/lifecycle.c:557`, `:1409`) |
| `ASX_E_INCOMPLETE_CHILDREN` | Children not complete at close attempt | No (bd-udlh) |
| `ASX_E_TASKS_STILL_ACTIVE` | Tasks remain active (quiescence check) | No (bd-udlh); C uses `ASX_E_QUIESCENCE_TASKS_LIVE` |
| `ASX_E_OBLIGATIONS_UNRESOLVED` | Obligations remain (quiescence check) | Yes (`src/runtime/quiescence.c:55`) |
| `ASX_E_REGIONS_NOT_CLOSED` | Regions not closed (quiescence check) | No; C uses `ASX_E_QUIESCENCE_NOT_REACHED` |
| `ASX_E_TIMERS_PENDING` | Timers remain (quiescence check) | No |
| `ASX_E_CHANNEL_NOT_DRAINED` | Channel messages remain (quiescence check) | No |
| `ASX_E_OBLIGATION_ALREADY_RESOLVED` | Double-resolve attempt | No; C returns `ASX_E_INVALID_TRANSITION` |
| `ASX_E_INVALID_TRANSITION` | Backward or skip state transition | Yes (`src/core/transition_tables.c:72-85`) |
| `ASX_E_SCHEDULER_UNAVAILABLE` | No local scheduler | No |
| `ASX_E_NAME_CONFLICT` | Named task conflict | No |

All of these are declared in `include/asx/asx_status.h`. Codes the close and admission paths do return that the earlier table omitted: `ASX_E_QUIESCENCE_NOT_REACHED`, `ASX_E_QUIESCENCE_TASKS_LIVE` (`include/asx/asx_status.h:83-84`), `ASX_E_REGION_POISONED` (`:42`), `ASX_E_NOT_FOUND` (`:28`), `ASX_E_STALE_HANDLE` (`:93`), `ASX_E_PENDING` (Draining wait).

### 15.3 Configuration Surface

| Config Field | Type | C default | Rust counterpart | C status |
|-------------|------|---------|---|---|
| `leak_response` | enum | `ASX_LEAK_LOG` (`src/runtime/lifecycle.c:72`) | `ObligationLeakResponse`, default `Panic` (section 5.1) | Read by the leak path; default differs (bd-9kll.3.6) |
| `leak_escalation` | pointer (optional) | `NULL` | `Option<LeakEscalation>` | Read (section 5.2) |
| `finalizer_poll_budget` | `uint32_t` | 100 (`src/runtime/hooks.c:773`) | `FINALIZER_POLL_BUDGET` 100 | Validated, otherwise unread (bd-udlh) |
| `finalizer_time_budget_ns` | `uint64_t` | 5,000,000,000 (`src/runtime/hooks.c:774`) | `FINALIZER_TIME_BUDGET_NANOS` | Unread (bd-udlh) |
| `finalizer_escalation` | enum | `ASX_FINALIZER_BOUNDED_LOG` (`src/runtime/hooks.c:775`) | `FinalizerEscalation`, not wired in Rust (section 6.5) | Validated, otherwise unread (bd-udlh) |
| `max_cancel_chain_depth` | `uint16_t` | 16 (`src/runtime/hooks.c:776`) | `CancelAttributionConfig::DEFAULT_MAX_DEPTH` 16 (`src/types/cancel.rs:198`) | Validated non-zero (`src/runtime/rt.c:159`); no cancel code reads it |
| `max_cancel_chain_memory` | `uint32_t` | 4096 (`src/runtime/hooks.c:777`) | `DEFAULT_MAX_MEMORY` 4096 (`src/types/cancel.rs:201`) | No cancel code reads it |

Field declarations: `include/asx/asx_config.h:560-566`. The earlier names `max_chain_depth` / `max_chain_memory` are `max_cancel_chain_depth` / `max_cancel_chain_memory` in C.

### 15.4 Invariant Schema Mapping

The earlier version showed rows for an `invariants/*.yaml` directory, which does not exist. The machine-readable invariant schema is `schemas/invariant_schema.json` (bd-296.4). Its region domain includes `INV-RG-04` ("Finalizing->Closed requires all obligations resolved") and `INV-RG-05` ("Finalizers execute in LIFO order"). The YAML below is illustrative only; the fixture IDs in it are not materialized (section 14), and the C close path reports `ASX_E_OBLIGATIONS_UNRESOLVED`, not `ASX_E_UNRESOLVED_OBLIGATIONS`, for an unresolved obligation.

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
  error_on_violation: ASX_E_OBLIGATIONS_UNRESOLVED
  fixture_ids: []   # quiescence-001 .. quiescence-006 are not materialized
  formal_ref: "Lean4: close_quiescence_decomposition (formal/lean/Asupersync.lean:888)"
```

### 15.5 Validation Against Upstream Artifacts

Historical (2026-02), not re-checked in this pass:

| Upstream | Validation |
|----------|------------|
| bd-296.15 (Lifecycle Tables) | Close state machine matches section 2.1; forbidden behaviors cross-referenced in section 13.2 |
| bd-296.16 (Outcome/Budget) | Cleanup budget values match section 7.1 (re-verified at `5e60b1c4c`) |
| bd-296.17 (Channel/Timer) | Channel/timer quiescence conditions: see section 9 |

### 15.6 Downstream Handoff

Historical list from the original extraction; the statuses of these beads were not re-checked in this pass:
1. `bd-296.4` — machine-readable invariant schema and generation pipeline
2. `bd-296.5` — forbidden-behavior catalog + shared scenario DSL
3. `bd-296.6` — Rust→C guarantee-substitution matrix
4. `bd-296.19` — source-to-fixture provenance map
5. `bd-296.1` — exhaustive EXISTING_ASUPERSYNC_STRUCTURE.md
6. `bd-1md.13` — core semantic fixture families
7. `bd-1md.15` — vertical and continuity fixture families
