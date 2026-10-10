# Lifecycle Transition Tables — Canonical Reference

> **Bead:** bd-296.15 (first extraction); re-extracted under bd-9kll.2.12 (item 3)
> **Status:** Reference tables for the C port. Where asupersync's normative small-step semantics now states a rule, the section says so and names the rule; that spec is authoritative and this document only summarizes it.
> **Rust baseline:** asupersync `5e60b1c4c` (`5e60b1c4c53d62aaddae68de3ee7de4732f1755b`, the commit the v2 fixtures record as `rust_baseline_commit`). Normative spec: `asupersync_v4_formal_semantics.md` at that commit (Canonical Rule Index, §1 domains, §3 rules, §5 invariants). Rust's own code map: `formal/impl_refinement_map.md` (its line numbers are stale at `5e60b1c4c`; see [`docs/C_REFINEMENT_MAP.md`](C_REFINEMENT_MAP.md), "Sources and revisions").
> **C status per rule:** [`docs/C_REFINEMENT_MAP.md`](C_REFINEMENT_MAP.md) is the source of truth. Superseded sections name the row to read.
> **Citations:** a Rust `path:line` is asupersync at `5e60b1c4c` (`git -C /dp/asupersync show 5e60b1c4c:<path>`). A C `path:line` was read in this repository at commit `b412780` on 2026-10-10. Line numbers drift: treat them as grep anchors and look for the symbol named next to them.
> **Previous provenance:** extracted 2026-02-27 by MossySeal from `src/record/{region,task,obligation}.rs` and `src/types/cancel.rs` at an unpinned baseline (the field read `RUST_BASELINE_COMMIT`); the companion `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` used `38c15240`.
> **Last verified:** 2026-10-10 (bd-9kll.2.12)
> **Purpose:** Transition tables for the region, task, obligation and cancellation state machines, with the Rust and C code that implements each row

---

## Table of Contents

1. [Region Lifecycle](#1-region-lifecycle)
2. [Task Lifecycle](#2-task-lifecycle)
3. [Obligation Lifecycle](#3-obligation-lifecycle)
4. [Cancellation Protocol](#4-cancellation-protocol)
5. [Outcome Severity Lattice](#5-outcome-severity-lattice)
6. [Cross-Domain Ordering Constraints](#6-cross-domain-ordering-constraints)
7. [Quiescence Invariant](#7-quiescence-invariant)
8. [Forbidden Behavior Catalog](#8-forbidden-behavior-catalog)
9. [Fixture ID Mapping](#9-fixture-id-mapping)
10. [Invariant Schema Cross-Reference](#10-invariant-schema-cross-reference)

---

## 1. Region Lifecycle

> **Superseded by `asupersync_v4_formal_semantics.md` §1.6 and §3.3 (rules `CLOSE-BEGIN` = `rule.region.close_begin` #22, `CLOSE-CANCEL-CHILDREN` = `rule.region.close_cancel_children` #23, `CLOSE-CHILDREN-DONE` = `rule.region.close_children_done` #24, `CLOSE-RUN-FINALIZER` = `rule.region.close_run_finalizer` #25, `CLOSE-COMPLETE` = `rule.region.close_complete` #26); C status: see C_REFINEMENT_MAP.md rows `rule.region.close_begin` through `rule.region.close_complete`.** Summary: a region moves Open → Closing → [Draining] → Finalizing → Closed; it may finalize only when every child task is Completed and every child region is Closed, and it may close only when its finalizers have run and no obligation in it is still Reserved. The tables below show how Rust's record (`src/record/region.rs`) and runtime (`src/runtime/state.rs`) implement that, and what C does.

### 1.1 States

Rust `RegionState` is at `src/record/region.rs:76`; C `asx_region_state` is at `include/asx/asx_ids.h:102`. The encodings match (Open=0 … Closed=4).

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Open` | `ASX_REGION_OPEN` | Accepting work: tasks, child regions and obligations may be created (`can_spawn`/`can_accept_work`, `src/record/region.rs:130`, `src/record/region.rs:140`) |
| `Closing` | `ASX_REGION_CLOSING` | Close requested or region cancelled; no new tasks, child regions or obligations; existing work continues |
| `Draining` | `ASX_REGION_DRAINING` | Entered only while the region still has child **regions** (Rust `advance_region_state`, `src/runtime/state.rs:10220`; C `src/runtime/quiescence.c:233`). When the close came from a cancel request, its tasks were already cancelled by it; a plain C `asx_region_close` cancels none |
| `Finalizing` | `ASX_REGION_FINALIZING` | All child tasks Completed and child regions Closed; region finalizers run here (LIFO). Rust admits only cleanup tasks here (`can_accept_cleanup_work`, `src/record/region.rs:150`); C admits every spawn (see §1.4) |
| `Closed` | `ASX_REGION_CLOSED` | Terminal. Rust keeps the close outcome on the record (`close_outcome`, `src/record/region.rs:1139`), not in the state; the v4 spec writes `Closed(outcome)`. C records no region outcome (C_REFINEMENT_MAP row `rule.region.close_complete`, `bd-9kll.3.4`) |

### 1.2 Legal Transitions

C's authority table is `src/core/transition_tables.c:22` (`asx_region_transition_check`, `src/core/transition_tables.c:72`); it allows exactly these five state pairs.

| # | From | To | Trigger | Preconditions | Postconditions |
|---|------|----|---------|---------------|----------------|
| R1 | `Open` | `Closing` | Rust `begin_close(reason)` (`src/record/region.rs:1699`); C `asx_region_close` (`src/runtime/lifecycle.c:893`) | state == Open (Rust CAS in `begin_close_transition`, `src/record/region.rs:1719`; C table check, `src/runtime/lifecycle.c:905`) | No new normal spawns, child regions or obligations. Rust sets or strengthens the region's cancel reason when one is given, even if the region is already closing (a no-op once Closed). `asx_region_close` itself cancels no task |
| R1a | `Open` | `Closing` | An ancestor region is cancelled | The cancel request walks the whole subtree, parents before descendants (Rust `cancel_request`, `src/runtime/state.rs:7547`, sort at `src/runtime/state.rs:7653`; C `asx_region_cancel`, `src/runtime/cancellation.c:430`) | Each descendant gets ParentCancelled attributed to its immediate parent, caused by the parent's reason, and moves Open → Closing (Rust `src/runtime/state.rs:7779`; C `src/runtime/cancellation.c:474`); a region already closing has its reason strengthened (Rust `src/runtime/state.rs:7794`). The tasks of every region in the subtree are then cancelled (§4.9) |
| R1b | `Open` | `Closing` | This region is cancelled (Rust `cancel_request`; C `asx_region_cancel_request` → `asx_region_cancel`) | none beyond a live region | Region reason set or strengthened; region moves to Closing; its live tasks get CancelRequested with the region's reason; a region with no live task or child region advances at once (Rust `src/runtime/state.rs:7880`; C third pass, `src/runtime/cancellation.c:484`) |
| R2 | `Closing` | `Draining` | Rust `begin_drain()` (`src/record/region.rs:1743`), called by `advance_region_state` when `child_count() > 0` (`src/runtime/state.rs:10220`); C `src/runtime/quiescence.c:233` and `src/runtime/quiescence.c:265` | state == Closing and the region has at least one child **region** | Waits for the child regions to close. Not a cancel step: when the close came from a cancel request (Rust's region close command is one, `src/runtime/state.rs:5004`), the tasks were already cancelled by it. The spec enters Draining whenever a child task is still live (Variant, C_REFINEMENT_MAP row `rule.region.close_cancel_children`) |
| R3 | `Closing` | `Finalizing` | Rust `begin_finalize()` (`src/record/region.rs:1756`) from `advance_region_state` (`src/runtime/state.rs:10181`); C `asx_region_finalize_one` (`src/runtime/quiescence.c:238`) | Rust `can_region_finalize` (`src/runtime/state.rs:7938`): every task terminal, every child region Closed, no pending spawn. C: `child_count == 0` and, from `asx_region_advance_internal`, `task_count == 0` | Draining is skipped. Region finalizers run next (LIFO) |
| R4 | `Draining` | `Finalizing` | Same functions as R3 | Same as R3 (the last child region has closed and unlinked) | Region finalizers run next (LIFO) |
| R5 | `Finalizing` | `Closed` | Rust `complete_close()` (`src/record/region.rs:1786`) from `advance_region_state` (`src/runtime/state.rs:10317`); C `asx_region_finalize_one` (`src/runtime/quiescence.c:255`) | Rust: no child region, no task, no pending or unapplied obligation, empty finalizer stack, no pending spawn; before that, with no task left, Rust leak-audits any still-Reserved obligation (`src/runtime/state.rs:10271`). C: no Reserved obligation (else it stays Finalizing with `ASX_E_OBLIGATIONS_UNRESOLVED`, `src/runtime/quiescence.c:243`), cleanup stack drained, no live task | Rust records the close outcome (default `Cancelled(reason)` if the region was cancelled, else `Ok`, `src/record/region.rs:1820`), reclaims the heap and wakes close waiters. C unlinks the region from its parent, emits `region.closed` and wakes close waiters (`src/runtime/quiescence.c:257`). The order and the leak audit differ: C_REFINEMENT_MAP rows `rule.region.close_run_finalizer`, `rule.region.close_complete`, `inv.obligation.ledger_empty_on_close` (`bd-9kll.3.4`) |

### 1.3 Forbidden Transitions (Must-Fail)

| From | To | Expected Error | Rationale |
|------|----|----------------|-----------|
| `Closing` | `Open` | `ASX_E_INVALID_TRANSITION` | Regions cannot reopen once close is initiated |
| `Draining` | `Open` | `ASX_E_INVALID_TRANSITION` | Regions cannot reopen |
| `Draining` | `Closing` | `ASX_E_INVALID_TRANSITION` | Cannot regress to prior phase |
| `Finalizing` | `Open` | `ASX_E_INVALID_TRANSITION` | Regions cannot reopen |
| `Finalizing` | `Closing` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `Draining` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Closed` | (any) | `ASX_E_INVALID_TRANSITION` | Terminal state; no transitions allowed |
| `Open` | `Draining` | `ASX_E_INVALID_TRANSITION` | Must pass through `Closing` first |
| `Open` | `Finalizing` | `ASX_E_INVALID_TRANSITION` | Must pass through `Closing` first |
| `Open` | `Closed` | `ASX_E_INVALID_TRANSITION` | Must pass through full close sequence |
| `Closing` | `Closed` | `ASX_E_INVALID_TRANSITION` | Must pass through `Finalizing` first |
| `Draining` | `Closed` | `ASX_E_INVALID_TRANSITION` | Must pass through `Finalizing` |

**Note:** `Closing` → `Finalizing` is **LEGAL** when the region has no child regions left and every task is Completed (skips `Draining`; R3 in section 1.2).

The error column is C's: every pair outside the table fails `asx_region_transition_check` with `ASX_E_INVALID_TRANSITION` (`src/core/transition_tables.c:74`). Rust has no error code here: its record transitions are compare-and-swap steps that return `false` (`src/record/region.rs:310`).

### 1.4 Operations Gated by Region State

| Operation | Allowed States | Error if Wrong State |
|-----------|---------------|---------------------|
| Create child task | Rust: `Open` for a normal task (`add_task` → `can_accept_work`, `src/record/region.rs:1381`); `Open` or `Finalizing` for a cleanup task (`add_cleanup_task`, `src/record/region.rs:1438`). C: `Open` or `Finalizing` for **every** spawn (`asx_region_can_accept_work`, `src/core/transition_tables.c:89`) | C `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:1007`), as Rust's `AdmissionError::Closed`; `ASX_E_ADMISSION_LIMIT` at the region's task limit |
| Create child region | `Open` (Rust `add_child` → `can_spawn`, `src/record/region.rs:1343`; C `src/runtime/lifecycle.c:746`) | C `ASX_E_REGION_CLOSED` |
| Create obligation | `Open` (Rust `try_reserve_obligation`, `src/record/region.rs:1449`; C `asx_region_can_spawn`, `src/runtime/lifecycle.c:1286`) | C `ASX_E_REGION_CLOSED` |
| Close region | `Open` | C `asx_region_close`: `ASX_E_INVALID_TRANSITION` if not `Open` (`src/runtime/lifecycle.c:905`). Rust `begin_close` returns `false` but still strengthens the reason of a region that is not yet Closed (`src/record/region.rs:1719`) |
| Query region status | (any) | `asx_region_get_state` fails only on a bad handle (`ASX_E_NOT_FOUND`, `ASX_E_STALE_HANDLE`) |
| Access region arena (internal) | C: the only arena user is `asx_task_spawn_captured`, gated like a spawn (`src/runtime/lifecycle.c:1105`). Rust reclaims the region heap at Closed (`complete_close`, `src/record/region.rs:1786`) | C `ASX_E_REGION_CLOSED` |

`ASX_E_REGION_NOT_OPEN` and `ASX_E_ADMISSION_CLOSED`, which the first extraction listed here, are never returned by the kernel admission paths (`src/runtime/lifecycle.c`); `ASX_E_ADMISSION_CLOSED` appears only outside the kernel (for example `src/runtime/adapter.c`).

**Note:** Rust no longer admits normal tasks in `Finalizing`: the old `admission_allowed_when_finalizing` test is gone, and `normal_task_admission_rejected_when_finalizing` (`src/record/region.rs:2826`) asserts that `add_task` fails while `add_cleanup_task` succeeds. C still admits every spawn in `Finalizing`, and its invariant test asserts that behaviour (C_REFINEMENT_MAP row `rule.ownership.spawn`, Partial, `bd-9kll.3.5`). Child regions and obligations are `Open`-only in both.

### 1.5 Region Close Precondition Checklist

> **Superseded by `asupersync_v4_formal_semantics.md` §3.3 `CLOSE-COMPLETE` (rule `rule.region.close_complete` #26) and §3.4.5 (ledger view, `inv.obligation.ledger_empty_on_close` #20); C status: see C_REFINEMENT_MAP.md rows `rule.region.close_complete` and `inv.obligation.ledger_empty_on_close`.**

Before a region transitions from `Finalizing` to `Closed`:

1. All child tasks in terminal state (`Completed`)
2. All child sub-regions in `Closed` state
3. No obligation of the region is `Reserved` (`Committed`, `Aborted` and `Leaked` are all terminal)
4. Finalizers drained. Rust: the finalizer stack is empty (`src/record/region.rs:1786`). C: the cleanup stack is drained LIFO inside `asx_region_finalize_one` (`src/core/cleanup.c:92`, called at `src/runtime/quiescence.c:252`)
5. C only: the ghost linearity monitor reports unresolved obligations (`asx_ghost_check_obligation_leaks`, `src/core/ghost.c:182`, called at `src/runtime/quiescence.c:247`); it reports, it does not block

What happens when item 3 fails differs. Rust, once no task is left, leak-audits the Reserved obligations (marks them `Leaked` under the leak policy) and then closes (`src/runtime/state.rs:10271`). C keeps the region in `Finalizing` and `asx_region_finalize_one` returns `ASX_E_OBLIGATIONS_UNRESOLVED` (`src/runtime/quiescence.c:243`, from `src/runtime/quiescence.c:47`); it never returns `ASX_E_UNRESOLVED_OBLIGATIONS` or `ASX_E_INCOMPLETE_CHILDREN` here (the latter is declared and never returned, `bd-udlh`). This drift is recorded (`bd-9kll.3.4`).

---

## 2. Task Lifecycle

> **Superseded by `asupersync_v4_formal_semantics.md` §1.5 (TaskState), §3.1 (rules `SPAWN` = `rule.ownership.spawn` #36, `SCHEDULE`, `COMPLETE-OK`, `COMPLETE-ERR`) and §3.2 / §3.2.5 (canonical cancellation automaton: `CANCEL-REQUEST` = `rule.cancel.request` #1, `CANCEL-ACKNOWLEDGE` = `rule.cancel.acknowledge` #2, `CANCEL-DRAIN` = `rule.cancel.drain` #3, `CANCEL-FINALIZE` = `rule.cancel.finalize` #4, `CHECKPOINT-MASKED` = `rule.cancel.checkpoint_masked` #10); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.request` … `rule.cancel.finalize`, `rule.cancel.checkpoint_masked`, `rule.ownership.spawn`, and the supplementary rows "§3.1 SCHEDULE", "§3.1 COMPLETE-OK", "§3.1 COMPLETE-ERR".** Summary: Created → Running → CancelRequested → Cancelling → Finalizing → Completed(Cancelled), with strengthening self-loops in the three cancel states and a Completed exit from every non-terminal state. Rust's legal-pair table (`TaskPhase::is_valid_transition`, `src/record/task.rs:166`) and C's (`src/core/transition_tables.c:48`) are the same 13 pairs.

### 2.1 States

Rust `TaskState` is at `src/record/task.rs:77` (the cancel states carry `reason` and `cleanup_budget`); the atomic `TaskPhase` mirror is at `src/record/task.rs:110`. C `asx_task_state` is at `include/asx/asx_ids.h:114`; the reason and budgets live on the C task slot.

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Created` | `ASX_TASK_CREATED` | Task allocated but not yet polled |
| `Running` | `ASX_TASK_RUNNING` | Task is being polled by the scheduler |
| `CancelRequested` | `ASX_TASK_CANCEL_REQUESTED` | Cancel requested but not yet acknowledged at a checkpoint |
| `Cancelling` | `ASX_TASK_CANCELLING` | Cancel acknowledged; the task runs its cleanup under the cleanup budget |
| `Finalizing` | `ASX_TASK_FINALIZING` | Cleanup done; the task is running finalizers |
| `Completed` | `ASX_TASK_COMPLETED` | Terminal state; outcome determined |

### 2.2 Legal Transitions (13 total of 36 state pairs)

| # | From | To | Trigger | Preconditions | Postconditions |
|---|------|----|---------|---------------|----------------|
| T1 | `Created` | `Running` | Rust `start_running()` (`src/record/task.rs:1078`); C first poll (`src/runtime/scheduler.c:1038`) | state==Created | Task poll function invoked |
| T2 | `Created` | `CancelRequested` | Rust `request_cancel*` (`src/record/task.rs:803`) | state==Created | Cancel before first poll; Rust sets `cancel_epoch` to 1 (or adds 1 if it was set). **C never takes this edge:** it moves the task Created → Running → CancelRequested and traces both steps (`src/runtime/cancellation.c:193`; open bead `bd-9kll.3.9`; C_REFINEMENT_MAP row "§3.1 SCHEDULE") |
| T3 | `Created` | `Completed` | Rust `complete(outcome)` (`src/record/task.rs:1260`) | state==Created | Completion before any poll (for example a spawn-time error or panic). The C scheduler always takes T1 before it polls, so C never takes T3 |
| T4 | `Running` | `CancelRequested` | Rust `request_cancel*` (`src/record/task.rs:803`); C `asx_task_cancel_reason_internal` (`src/runtime/cancellation.c:99`, first request at `src/runtime/cancellation.c:188`) | state==Running | Cancel delivered with the request's reason and cleanup budget. Rust: `cancel_epoch` 0 → 1. C: `cancel_epoch = 1` (`src/runtime/cancellation.c:213`) |
| T5 | `Running` | `Completed` | Rust `complete(outcome)`; C `sched_complete` (`src/runtime/scheduler.c:732`) | state==Running | Normal completion, error, or panic |
| T6 | `CancelRequested` | `CancelRequested` | Rust `request_cancel*` (`src/record/task.rs:735`); C strengthen branch (`src/runtime/cancellation.c:150`) | state==CancelRequested | **Strengthening:** reason strengthened (§4.4), cleanup budgets met; not a new cancellation |
| T7 | `CancelRequested` | `Cancelling` | Rust `acknowledge_cancel()` (`src/record/task.rs:1341`), from an unmasked checkpoint; C `asx_checkpoint` (`src/runtime/cancellation.c:603`) | state==CancelRequested and mask depth 0 | Rust: the cleanup budget becomes the task's budget and `polls_remaining` is set to its poll quota. C applies the cleanup budget after the acknowledging poll returns (`asx_task_apply_cleanup_budget_internal`, `src/runtime/lifecycle.c:465`) |
| T8 | `CancelRequested` | `Completed` | Rust `complete(outcome)` | state==CancelRequested | Completion before acknowledgement. **The outcome is `Cancelled(reason)`, not the returned value**: Rust's `complete` maps `Ok`/`Err` in any cancel state to `Cancelled(reason)` and strengthens a `Cancelled` outcome with the task's reason; only `Panicked` passes through (`src/record/task.rs:1267`). C does the same (`sched_cancel_dominates`, `src/runtime/scheduler.c:402`, used at `src/runtime/scheduler.c:1118` and `src/runtime/scheduler.c:1130`). The one exception in both is a task spawned from inside another task's poll that acknowledged a cancel arriving after its first poll: it keeps its value (Rust `classify_spawn_completion`, `src/runtime/task_handle.rs:173`) |
| T9 | `Cancelling` | `Cancelling` | Rust `request_cancel*` (`src/record/task.rs:751`); C strengthen branch | state==Cancelling | **Strengthening:** as T6; in addition the met budget becomes the task's budget and `polls_remaining` takes the smaller quota (Rust `src/record/task.rs:769`; C `src/runtime/cancellation.c:150`) |
| T10 | `Cancelling` | `Finalizing` | Rust `cleanup_done()` (`src/record/task.rs:1378`); C `asx_task_finalize` (`src/runtime/cancellation.c:671`) | state==Cancelling | Cleanup finished. Rust's lab takes this step when a task in a cancel state finishes its poll with `Ok` (Cancelling → Finalizing → Completed(Cancelled), `src/lab/runtime.rs:4876`); with any other result it calls `complete` (T11). C takes it only when the task calls `asx_task_finalize` (or under the opt-in hard bound); otherwise C completes a Cancelling task directly through T11 (Variant, C_REFINEMENT_MAP row `rule.cancel.drain`) |
| T11 | `Cancelling` | `Completed` | Rust `complete(outcome)`; C `sched_complete` | state==Cancelling | `Ok`/`Err` become `Cancelled(reason)` as in T8; a panic stays `Panicked` |
| T12 | `Finalizing` | `Finalizing` | Rust `request_cancel*` (`src/record/task.rs:777`); C strengthen branch | state==Finalizing | **Strengthening:** as T9 |
| T13 | `Finalizing` | `Completed` | Rust `finalize_done()` (`src/record/task.rs:1416`); C completes a Finalizing task at its next dispatch (`src/runtime/scheduler.c:1222` lab, `src/runtime/scheduler.c:1374` round-robin) | state==Finalizing | Outcome `Cancelled(reason)`. Rust returns a `CancelWitness` with phase `Completed` (`finalize_done_with_witness`, `src/record/task.rs:1422`). C advances its witness to `Completed` and releases it (`src/runtime/scheduler.c:718`) |

**Strengthening semantics (T6, T9, T12):** These are not state changes. Rust strengthens the reason with `CancelReason::strengthen` (`src/types/cancel.rs:956`; order in §4.4) and meets the cleanup budgets with `Budget::combine` (min deadline, min poll and cost quota, max priority; `src/types/budget.rs:556`). Rust leaves `cancel_epoch` unchanged; C increments it on every changed request (`src/runtime/cancellation.c:171`; C_REFINEMENT_MAP row `inv.cancel.idempotence`, `bd-9kll.3.7`). In Rust the request reports "not newly cancelled" (`false`).

### 2.3 Forbidden Transitions (23 total)

The 23 forbidden pairs are 10 backward pairs, 5 pairs that skip a state, 2 self-loops outside the cancel states (`Created → Created`, `Running → Running`) and the 6 pairs out of `Completed`. C rejects each with `ASX_E_INVALID_TRANSITION` (`src/core/transition_tables.c:79`); Rust's `TaskPhaseCell::store` asserts in debug builds (§2.3 note).

**Backward transitions (10):**

| From | To | Expected Error | Rationale |
|------|----|----------------|-----------|
| `Running` | `Created` | `ASX_E_INVALID_TRANSITION` | No backward transitions |
| `CancelRequested` | `Running` | `ASX_E_INVALID_TRANSITION` | Cannot un-cancel |
| `CancelRequested` | `Created` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Cancelling` | `CancelRequested` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Cancelling` | `Running` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Cancelling` | `Created` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `Cancelling` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `CancelRequested` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `Running` | `ASX_E_INVALID_TRANSITION` | Cannot regress |
| `Finalizing` | `Created` | `ASX_E_INVALID_TRANSITION` | Cannot regress |

**Skipped states (5) and non-cancel self-loops (2):**

| From | To | Expected Error | Rationale |
|------|----|----------------|-----------|
| `Created` | `Cancelling` | `ASX_E_INVALID_TRANSITION` | Must pass through `CancelRequested` |
| `Created` | `Finalizing` | `ASX_E_INVALID_TRANSITION` | Must pass through `CancelRequested` then `Cancelling` |
| `Running` | `Cancelling` | `ASX_E_INVALID_TRANSITION` | Must pass through `CancelRequested` |
| `Running` | `Finalizing` | `ASX_E_INVALID_TRANSITION` | Must pass through `CancelRequested` then `Cancelling` |
| `CancelRequested` | `Finalizing` | `ASX_E_INVALID_TRANSITION` | Must pass through `Cancelling` |
| `Created` | `Created` | `ASX_E_INVALID_TRANSITION` | No self-loop (only cancel-related states allow strengthening) |
| `Running` | `Running` | `ASX_E_INVALID_TRANSITION` | No self-loop (row missing from the first extraction) |

**From terminal (6):**

| From | To | Expected Error | Rationale |
|------|----|----------------|-----------|
| `Completed` | `Created` | `ASX_E_INVALID_TRANSITION` | Terminal is absorbing |
| `Completed` | `Running` | `ASX_E_INVALID_TRANSITION` | Terminal is absorbing |
| `Completed` | `CancelRequested` | `ASX_E_INVALID_TRANSITION` | Terminal is absorbing |
| `Completed` | `Cancelling` | `ASX_E_INVALID_TRANSITION` | Terminal is absorbing |
| `Completed` | `Finalizing` | `ASX_E_INVALID_TRANSITION` | Terminal is absorbing |
| `Completed` | `Completed` | `ASX_E_INVALID_TRANSITION` | No self-loop from terminal |

**Note:** In Rust, `TaskPhaseCell::store` asserts every phase change in debug builds with the message `"invalid TaskPhase transition: {current:?} -> {phase:?}"` (`src/record/task.rs:265`). The record methods match on the current state and return `false`/`None` when it does not fit (for example `acknowledge_cancel`, `src/record/task.rs:1341`). In C every runtime transition first calls the ghost monitor (`asx_ghost_check_task_transition`), which records a violation and does not block; `asx_task_transition_check` (`src/core/transition_tables.c:77`) is the table lookup that returns `ASX_E_INVALID_TRANSITION`.

### 2.3.1 Transition Matrix (Machine-Readable)

```text
6x6 matrix (36 total pairs: 13 valid, 23 invalid)

From\To        Created  Running  CancelReq  Cancelling  Finalizing  Completed
Created           .       T1       T2           .           .          T3
Running           .        .       T4           .           .          T5
CancelReq         .        .       T6          T7           .          T8
Cancelling        .        .        .          T9          T10         T11
Finalizing        .        .        .           .          T12         T13
Completed         .        .        .           .           .           .
```

### 2.4 Task Poll Contract

C has no `ASX_POLL_*` codes (the first extraction named some). A C poll function (`asx_task_poll_fn`, `include/asx/runtime/runtime.h:87`) returns an `asx_status`; the scheduler maps it in `sched_poll_slot` (`src/runtime/scheduler.c:1025`). Rust's equivalent is the `Poll` of the task's future plus the panic catch at the poll boundary.

| Poll Return | Meaning | State Transition |
|------------|---------|-----------------|
| `ASX_E_PENDING` | Task yielded or parked; needs another poll | Remains in its current state |
| `ASX_OK` | Task completed its work | `Completed(Ok)`, or `Completed(Cancelled)` when a pending cancel dominates (T8/T11; `src/runtime/scheduler.c:1118`) |
| any other status | Task failed | `Completed(Err)` with the status recorded, or `Completed(Cancelled)` when a pending cancel dominates (`src/runtime/scheduler.c:1130`); the region's fault-containment policy then applies unless the task was cancelled |
| (`asx_task_panic` called during the poll) | Task panicked | `Completed(Panicked)` whatever the poll returned, also over a pending cancel (`src/runtime/scheduler.c:1108`) |

### 2.5 Cancellation Observation Rules

> **Superseded by `asupersync_v4_formal_semantics.md` §3.2 `CANCEL-ACKNOWLEDGE` (rule `rule.cancel.acknowledge` #2) and `CHECKPOINT-MASKED` (rule `rule.cancel.checkpoint_masked` #10), with §6 `PROG-CANCEL` (`prog.cancel.drains` #9); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.acknowledge`, `rule.cancel.checkpoint_masked` (Variant) and `prog.cancel.drains` (Partial).**

1. A cancel request moves the task to `CancelRequested` (T2/T4) and wakes it so that it can reach a checkpoint (Rust cancel wakers; C `src/runtime/cancellation.c:221`)
2. The task acknowledges the cancel at a checkpoint: Rust `Cx::checkpoint` (`src/cx/cx.rs:2749`), C `asx_checkpoint` (`src/runtime/cancellation.c:521`). C has no `asx_is_cancelled` (the first extraction named one); `asx_task_get_cancel_reason` and `asx_task_get_cancel_phase` only read state
3. Between `CancelRequested` and acknowledgement the task continues with its normal poll logic
4. A checkpoint with mask depth 0 moves the task to `Cancelling` and its cleanup budget becomes its budget (T7). With mask depth > 0 the cancel stays pending and unacknowledged. Rust and C treat the mask as a nesting depth that unmask releases (`Cx::masked`, `src/cx/cx.rs:3315`; C `src/runtime/cancellation.c:589`, `asx_task_mask`/`asx_task_unmask` at `src/runtime/cancellation.c:638` and `src/runtime/cancellation.c:648`); the spec instead consumes one unit per masked checkpoint (Variant). The depth is bounded by 64 in both (`MAX_MASK_DEPTH`, `src/types/task_context.rs:673`; `ASX_MAX_MASK_DEPTH`, `include/asx/runtime/runtime.h:476`)
5. The cleanup budget is advisory in Rust: a spent cleanup poll quota only strengthens the reason to `PollQuota` (lab, `src/lab/runtime.rs:4667`), and Rust never force-completes a task. C is the same by default; the opt-in `cleanup_hard_bound` force-completes with `Cancelled` (`src/runtime/scheduler.c:1391`; a deviation excluded from parity, `bd-9kll.3.2`)
6. A task that completes while `CancelRequested` (or later) does **not** keep its natural outcome: `Ok`/`Err` become `Cancelled(reason)` (T8). The exception is in T8

---

## 3. Obligation Lifecycle

> **Superseded by `asupersync_v4_formal_semantics.md` §1.7, §1.9 and §3.4 (rules `RESERVE` = `rule.obligation.reserve` #13, `COMMIT` = `rule.obligation.commit` #14, `ABORT` = `rule.obligation.abort` #15, `LEAK` = `rule.obligation.leak` #16; §3.4.4 state machine = `inv.obligation.linear` #18; §3.4.5 ledger = `inv.obligation.ledger_empty_on_close` #20; §3.4.6 = `inv.obligation.no_leak` #17; §5 INV-OBLIGATION-BOUNDED = `inv.obligation.bounded` #19); C status: see C_REFINEMENT_MAP.md rows `rule.obligation.reserve` … `prog.obligation.resolves`.** Summary: a Running holder reserves an obligation in its Open region; only the holder commits or aborts it; if the holder completes while it is still Reserved, it becomes Leaked; Committed, Aborted and Leaked are absorbing; a region closes only with no Reserved obligation.

### 3.1 States

Rust `ObligationState` is at `src/record/obligation.rs:192`; C `asx_obligation_state` is at `include/asx/asx_ids.h:127`. Rust kinds (`src/record/obligation.rs:54`) are SendPermit, Ack, Lease, IoOp, SemaphorePermit and Transaction (the spec lists the first four); C has the same six plus `ASX_OBLIGATION_KIND_GENERIC` (`include/asx/runtime/runtime.h:674`).

| State | Enum Value | Description |
|-------|-----------|-------------|
| `Reserved` | `ASX_OBLIGATION_RESERVED` | Resource/promise reserved but not yet fulfilled; blocks region close |
| `Committed` | `ASX_OBLIGATION_COMMITTED` | Terminal: obligation fulfilled successfully |
| `Aborted` | `ASX_OBLIGATION_ABORTED` | Terminal: obligation released; Rust records an abort reason (Cancel, Error, Explicit; `src/record/obligation.rs:99`), as does C (`include/asx/runtime/runtime.h:684`, plus the C-only `LEAK_RECOVERED`) |
| `Leaked` | `ASX_OBLIGATION_LEAKED` | Terminal (error): the holder completed, or dropped the token, without resolving it |

### 3.2 Legal Transitions

C's authority table is `src/core/transition_tables.c:65`.

| From | To | Trigger | Preconditions | Postconditions |
|------|----|---------|---------------|----------------|
| `Reserved` | `Committed` | Rust `commit_obligation` (`src/runtime/state.rs:6534`); C `asx_obligation_commit` (`src/runtime/lifecycle.c:1367`) | Obligation `Reserved`. The spec also requires the caller to be the holder; Rust enforces that through the token-bound holder check, C does not check it (C_REFINEMENT_MAP row `rule.obligation.commit`, Partial) | Effect takes place; the region's pending count drops; C ghost monitor records the resolution |
| `Reserved` | `Aborted` | Rust `abort_obligation` (`src/runtime/state.rs:6745`); C `asx_obligation_abort` / `asx_obligation_abort_with_reason` (`src/runtime/lifecycle.c:1391`, `src/runtime/lifecycle.c:1417`) | Obligation `Reserved` | Resource released, no effect; abort reason recorded |
| `Reserved` | `Leaked` | The holder completes while holding it (Rust `audit_completion_obligation_leaks`, `src/runtime/state.rs:8341`; C `asx_task_resolve_held_obligations_internal`, `src/runtime/lifecycle.c:515`). Also: C `asx_obligation_drop` leaks at once (`src/runtime/lifecycle.c:1398`), as a dropped Rust token does; Rust's Finalizing leak audit (`src/runtime/state.rs:10271`) leaks what is still Reserved when no task is left | Holder Completed with the obligation still `Reserved` | Leak handled under the leak policy. Rust default is Panic (`src/runtime/state.rs:2338`); C default is LOG (`src/runtime/lifecycle.c:72`); RECOVER aborts with C's `ASX_OBLIGATION_ABORT_LEAK_RECOVERED` instead of marking Leaked (`src/runtime/lifecycle.c:493`). Drift recorded in C_REFINEMENT_MAP row `rule.obligation.leak` (`bd-9kll.3.6`) |

### 3.3 Forbidden Transitions (Must-Fail)

The first extraction listed `ASX_E_OBLIGATION_ALREADY_RESOLVED` and `ASX_E_OBLIGATION_LEAKED` here. C returns neither: `ASX_E_OBLIGATION_LEAKED` does not exist, and `ASX_E_OBLIGATION_ALREADY_RESOLVED` is declared (`include/asx/asx_status.h:52`) but no code returns it. Every resolve of a non-`Reserved` obligation fails the table check with `ASX_E_INVALID_TRANSITION` (`src/runtime/lifecycle.c:1377` for commit; the same check in abort and drop). Rust returns `ErrorKind::ObligationAlreadyResolved` (`src/runtime/obligation_table.rs:577`), which `docs/CANONICAL_VOCABULARY_V2.md` §5 maps to `ASX_E_OBLIGATION_ALREADY_RESOLVED`. DSL v2 cannot reach this case (`docs/SCENARIO_DSL_V2.md` §5), so no fixture compares it.

| From | To | Expected Error | Rationale |
|------|----|----------------|-----------|
| `Committed` | (any) | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | Terminal state; exactly-once semantics |
| `Aborted` | (any) | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | Terminal state; exactly-once semantics |
| `Leaked` | (any) | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | Terminal error state |
| `Reserved` | `Reserved` | C `ASX_E_INVALID_TRANSITION` (table) | Cannot re-reserve; a reserve always creates a new obligation |
| `Committed` | `Aborted` | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | Cannot change resolved outcome |
| `Aborted` | `Committed` | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | Cannot change resolved outcome |

### 3.4 Linearity Enforcement

> **Superseded by `asupersync_v4_formal_semantics.md` §1.9, §3.4.4 and §3.4.6 (rules `inv.obligation.linear` #18 and `inv.obligation.no_leak` #17); C status: see C_REFINEMENT_MAP.md rows `inv.obligation.linear` and `inv.obligation.no_leak`.**

The obligation lifecycle enforces **exactly-once resolution** (linear use). Neither engine keeps a bit-per-obligation ledger with a `popcnt` (the first extraction described one):

1. Rust keeps the obligation table plus a per-region pending count (`try_reserve_obligation`, `src/record/region.rs:1449`; `resolve_obligation`, `src/record/region.rs:1487`). C keeps the obligation slots, each Reserved one linked into its holder's held list (`src/runtime/lifecycle.c:1271`)
2. `commit` and `abort` resolve exactly once; the terminal states are absorbing (C table, `src/core/transition_tables.c:65`). The C ghost monitor records reserve and resolve and flags a double resolution (`src/core/ghost.c`)
3. Leaks are detected when the holder completes, not at region close: each obligation still Reserved in the holder's held set is leaked (§3.2)
4. At region close: Rust leak-audits any remaining Reserved obligation and then closes; C refuses to close while one is Reserved and stays `Finalizing` (§1.5; `bd-9kll.3.4`). An obligation C reserved outside any poll has no holder and so can never leak at a completion (C_REFINEMENT_MAP rows `rule.obligation.reserve` and `inv.obligation.bounded`, `bd-9kll.3.4` item 5)

### 3.5 Obligation-Region Interaction

| Region State | Obligation Creation Allowed | Obligation Resolution Allowed |
|-------------|---------------------------|------------------------------|
| `Open` | Yes | Yes |
| `Closing` | No (C `ASX_E_REGION_CLOSED`, `src/runtime/lifecycle.c:1286`; Rust `AdmissionError::Closed`) | Yes |
| `Draining` | No | Yes |
| `Finalizing` | No | Yes. Rust: until the region's Finalizing leak audit; C: any time, since C does not close with Reserved obligations |
| `Closed` | No | No Reserved obligation can remain. Rust rejects a late resolve with `ErrorKind::RegionFinalized` (`src/runtime/obligation_table.rs:569`) |

### 3.6 Double-Resolution Handling

Attempting to resolve an already-resolved obligation is a hard error (C status codes; Rust returns `ObligationAlreadyResolved`, §3.3):

- `commit` on `Committed` -> `ASX_E_INVALID_TRANSITION`
- `abort` on `Committed` -> `ASX_E_INVALID_TRANSITION`
- `commit` on `Aborted` -> `ASX_E_INVALID_TRANSITION`
- `abort` on `Aborted` -> `ASX_E_INVALID_TRANSITION`

This is **not** idempotent by design; double-resolution indicates a logic error. Rust's record-level `commit`/`abort`/`mark_leaked` assert `"obligation already resolved"` (`src/record/obligation.rs:391`).

---

## 4. Cancellation Protocol

> **Superseded by `asupersync_v4_formal_semantics.md` §1.3 (`def.cancel.reason_kinds` #7, `def.cancel.severity_ordering` #8), §3.2 and §3.2.5 (`rule.cancel.request` #1 … `rule.cancel.finalize` #4, `inv.cancel.idempotence` #5, `rule.cancel.checkpoint_masked` #10), the `strengthen` rule (`def.cancel.reason_ordering` #32), §5 INV-CANCEL-PROPAGATES (`inv.cancel.propagates_down` #6) and INV-MASK-BOUNDED (`inv.cancel.mask_bounded` #11, `inv.cancel.mask_monotone` #12); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.request` through `inv.cancel.mask_monotone` and `def.cancel.reason_ordering`.** The cancel *witness* (§4.1–§4.3, §4.5, §4.6 below) is not in the spec: it is a Rust runtime artifact (`CancelWitness`, `src/types/cancel.rs:323`) checked by the lab oracles, and C has only a reduced version.

### 4.1 Cancellation Phases

Rust `CancelPhase` and its rank are at `src/types/cancel.rs:295` and `src/types/cancel.rs:308`; C `asx_cancel_phase` is at `include/asx/asx_ids.h:164` (the enumerators are spelled `ASX_CANCEL_PHASE_*`, not `ASX_CANCEL_*` as the first extraction had them).

| Phase | Rank | Enum Value | Description |
|-------|------|-----------|-------------|
| `Requested` | 0 | `ASX_CANCEL_PHASE_REQUESTED` | Cancel requested; not yet acknowledged by the target |
| `Cancelling` | 1 | `ASX_CANCEL_PHASE_CANCELLING` | Target acknowledged the cancel; cleanup in progress |
| `Finalizing` | 2 | `ASX_CANCEL_PHASE_FINALIZING` | Cleanup done; the task is running finalizers |
| `Completed` | 3 | `ASX_CANCEL_PHASE_COMPLETED` | Task completed with a `Cancelled` outcome |

**Note:** There is no `None` phase. Rust's `TaskRecord::cancel_witness` returns no witness while `cancel_epoch == 0` and none for a task that completed with a non-`Cancelled` outcome (`src/record/task.rs:1053`). The first witness of a stream must carry a non-zero epoch (`validate_initial`, `src/types/cancel.rs:369`). C creates a witness slot on the first cancel of a task (`asx_cancel_witness_create`, `src/core/cancel.c:114`, called from `src/runtime/cancellation.c:99`).

### 4.2 Legal Phase Transitions

Rust's `CancelWitness::validate_transition` (`src/types/cancel.rs:382`) accepts a pair when task, region and epoch are equal, the phase rank does not decrease, and the reason's severity does not decrease. The runtime produces these pairs:

| From | To | Trigger | Preconditions | Postconditions |
|------|----|---------|---------------|----------------|
| (initial) | `Requested` | First cancel request (T2/T4) | Target not terminal | Rust: `cancel_epoch` becomes non-zero. C: witness slot created in phase `Requested` |
| `Requested` | `Requested` | Further request (T6) | Already CancelRequested | Strengthening only. C's `asx_cancel_witness_advance` is not called for it, and would reject a same-phase advance (`src/core/cancel.c:147`) |
| `Requested` | `Cancelling` | Acknowledgement at an unmasked checkpoint (T7) | Mask depth 0 | Cleanup budget becomes the task's budget |
| `Requested` | `Finalizing` | none | — | Rank-legal for the validator, but no runtime path produces it (Rust and C both forbid the task pair CancelRequested → Finalizing) |
| `Requested` | `Completed` | Target completes before acknowledging (T8) | — | Outcome `Cancelled(reason)` (not the natural outcome; T8), so the final witness has phase `Completed`. A panic yields `Panicked` and no final witness |
| `Cancelling` | `Cancelling` | Further request (T9) | Already Cancelling | Strengthening only |
| `Cancelling` | `Finalizing` | Rust `cleanup_done()` (T10); C `asx_task_finalize` | Cleanup finished | Finalizers run |
| `Cancelling` | `Completed` | `complete(outcome)` (T11) | — | `Cancelled(reason)`, or `Panicked` (then no final witness) |
| `Finalizing` | `Finalizing` | Further request (T12) | Already Finalizing | Strengthening only |
| `Finalizing` | `Completed` | Rust `finalize_done()` (T13); C next dispatch | Finalizers done | `Cancelled(reason)`; Rust returns the `Completed` witness |
| `Completed` | `Completed` | N/A | N/A | Terminal state; self-loop is no-op |

### 4.3 Forbidden Phase Transitions

Phase transitions where `next.phase.rank() < prev.phase.rank()`. Rust reports `CancelWitnessError::PhaseRegression` (`src/types/cancel.rs:414`). C never returns `ASX_E_WITNESS_PHASE_REGRESSION` (declared, unused): `asx_cancel_witness_advance` returns `ASX_E_INVALID_TRANSITION` for any advance that does not strictly increase the phase, a repeated phase included (`src/core/cancel.c:147`). Recorded in `bd-9kll.3.7` ("Witness: no region or epoch; rejects a repeated phase").

| From | To | Expected Error | Rationale |
|------|----|----------------|-----------|
| `Cancelling` | `Requested` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION` | Cannot regress phase rank (1 -> 0) |
| `Finalizing` | `Requested` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION` | Cannot regress phase rank (2 -> 0) |
| `Finalizing` | `Cancelling` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION` | Cannot regress phase rank (2 -> 1) |
| `Completed` | `Requested` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION` | Cannot regress phase rank (3 -> 0) |
| `Completed` | `Cancelling` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION` | Cannot regress phase rank (3 -> 1) |
| `Completed` | `Finalizing` | Rust `PhaseRegression`; C `ASX_E_INVALID_TRANSITION` | Cannot regress phase rank (3 -> 2) |

**Note:** The cancellation protocol does NOT have a `None` phase state. In Rust, phase skips (e.g., `Requested` -> `Completed`) pass `validate_transition` because the rank does not decrease; C's advance also accepts a skip, since it only requires a strictly larger phase.

**Additionally forbidden (Rust):** a witness pair whose reason severity decreases (`CancelWitnessError::ReasonWeakened`), or whose task, region or epoch differ (`TaskMismatch`, `RegionMismatch`, `EpochMismatch`). C has no witness-to-witness validator: its witness slot holds only an id, the phase and the reason (`src/core/cancel.c:91`), and the `ASX_E_WITNESS_*` codes are never returned.

### 4.4 Cancellation Kinds (11 Variants with Severity Ladder)

Severities and cleanup budgets match Rust exactly: `CancelKind::severity` (`src/types/cancel.rs:473`) and `CancelReason::cleanup_budget` (`src/types/cancel.rs:1027`), C `asx_cancel_severity` (`include/asx/asx_ids.h:172`) and `asx_cancel_cleanup_budget` (`src/core/cancel.c:35`). The cleanup budget is `Budget::new()` (no deadline, no cost quota) with the poll quota and priority set from the table below. The C ordinals in the Enum Value column are not Rust's declaration order: Rust has ParentCancelled=7, ResourceUnavailable=8, Shutdown=9, LinkedExit=10 (`src/types/cancel.rs:264`); C has LinkedExit=7, ParentCancelled=8, ResourceUnavailable=9, Shutdown=10 (`include/asx/asx_ids.h:149`). Vocabulary v2 encodes kinds by name, so fixtures are unaffected (C_REFINEMENT_MAP row `def.cancel.reason_kinds`, `bd-9kll.3.7`).

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

**Severity rules** (Rust `CancelReason::strengthen`, `src/types/cancel.rs:956`; C `strengthen_replaces`, `src/core/cancel.c:49`; the same order in both):
- Higher severity always wins in `strengthen()` operations
- Equal severity: the earlier timestamp wins; on equal timestamps a present message beats none, then the lexicographically smaller message wins; otherwise the current reason stays
- The winning reason replaces the old one whole (origin, timestamp, message, cause chain); chains are not merged
- The spec's `strengthen` (§3.2, "max on kind, tighter deadline wins") is stated more loosely; both engines follow Rust's implementation (C_REFINEMENT_MAP rows `inv.cancel.idempotence`, `def.cancel.reason_ordering`)
- Severity is monotone non-decreasing across witness transitions

**Budget combination (min-plus algebra):** Rust `Budget::combine` / `combine_untraced` (`src/types/budget.rs:502`, `src/types/budget.rs:556`); C `asx_budget_meet` (`src/core/budget.c:49`).
- `combined_quota = min(quota1, quota2)` -- tighter quota wins (poll and cost quotas)
- `combined_deadline = min` of the finite deadlines -- earlier wins
- `combined_priority = max(priority1, priority2)` -- higher urgency wins
- Monotone-narrowing: combining never widens the cleanup allowance

### 4.5 Cancellation Witness Structure

Rust's witness (`CancelWitness`, `src/types/cancel.rs:323`) carries the fields below. C's witness slot (`src/core/cancel.c:91`) holds only an id, the phase and the reason; it has no task, region or epoch field (`bd-9kll.3.7`).

| Field | Type | Description |
|-------|------|-------------|
| `task_id` | `asx_task_id` | The task being cancelled |
| `region_id` | `asx_region_id` | The owning region |
| `epoch` | `uint64_t` | Rust `cancel_epoch`: set to 1 by the first request (`src/record/task.rs:803`); strengthening leaves it unchanged. C keeps `cancel_epoch` on the task slot and increments it on every changed request (`src/runtime/cancellation.c:171`; `bd-9kll.3.7`) |
| `phase` | `asx_cancel_phase` | Current phase of the cancellation protocol |
| `reason` | `asx_cancel_reason` | Full cancellation reason with kind, origin, timestamp, and cause chain |

### 4.6 Witness Validation Rules

A witness transition from `prev` to `next` is valid when ALL hold. The checks are Rust's (`validate_transition`, `src/types/cancel.rs:382`; error enum `src/types/cancel.rs:414`). C defines the `ASX_E_WITNESS_*` codes (`include/asx/asx_status.h:57`) but never returns them; `docs/CANONICAL_VOCABULARY_V2.md` §5 lists them as C-only.

| Rule | Check | Error on Violation |
|------|-------|-------------------|
| Same task | `prev.task_id == next.task_id` | Rust `TaskMismatch` (C code `ASX_E_WITNESS_TASK_MISMATCH`, unused) |
| Same region | `prev.region_id == next.region_id` | Rust `RegionMismatch` (C code `ASX_E_WITNESS_REGION_MISMATCH`, unused) |
| Same epoch | `prev.epoch == next.epoch` | Rust `EpochMismatch` (C code `ASX_E_WITNESS_EPOCH_MISMATCH`, unused) |
| Phase monotone | `next.phase.rank >= prev.phase.rank` | Rust `PhaseRegression`; C `asx_cancel_witness_advance` returns `ASX_E_INVALID_TRANSITION` and also rejects an equal phase |
| Severity monotone | `next.reason.severity >= prev.reason.severity` | Rust `ReasonWeakened` (C code `ASX_E_WITNESS_REASON_WEAKENED`, unused) |
| Initial epoch | first witness has `epoch != 0` | Rust `InitialEpochZero` (`src/types/cancel.rs:369`); no C counterpart |

### 4.7 Attribution Chain

`CancelReason` carries a recursive cause chain. Rust: `src/types/cancel.rs:521`. C: `asx_cancel_reason` (`include/asx/core/cancel.h`).

| Field | Type | Description |
|-------|------|-------------|
| `kind` | `asx_cancel_kind` | The cancellation kind |
| `origin_region` | `asx_region_id` | Region that initiated cancellation; for a descendant region, the immediate parent (Rust `src/runtime/state.rs:7741`; C `src/runtime/cancellation.c:459`) |
| `origin_task` | `asx_task_id` (optional) | Task that initiated (if applicable) |
| `timestamp` | `asx_time` | When the cancellation was requested; descendants take the request's timestamp |
| `message` | `const char*` (optional) | Human-readable message (Rust `Option<String>`) |
| `cause` | `asx_cancel_reason*` (optional) | Parent cause in the chain. C sets it only in `asx_region_cancel`, pointing at the parent region's stored reason; the task-only `asx_cancel_propagate` sets none (`src/runtime/cancellation.c:399`) |
| `truncated` | `bool` | Whether the chain was truncated at limits (Rust also records `truncated_at_depth`) |

**Chain limits (configurable):**
- Rust `CancelAttributionConfig` (`src/types/cancel.rs:184`): `max_chain_depth` default 16, `max_chain_memory` default 4096 bytes, estimated at 80 bytes per reason plus 8 bytes per link (`src/types/cancel.rs:237`, `src/types/cancel.rs:244`)
- Rust truncates in `with_cause_limited` (`src/types/cancel.rs:706`), setting `truncated` and `truncated_at_depth`
- C never truncates: its chains are links to stored region reasons, and the `max_cancel_chain_depth` / `max_cancel_chain_memory` config fields are validated (`src/runtime/rt.c:159`) but not used by cancellation (`bd-9kll.3.7`, "the config knobs do nothing")

### 4.8 Cancellation Metadata (Legacy Section)

Each cancellation carries these operational fields:

| Field | Type | Description |
|-------|------|-------------|
| `reason_kind` | enum | Why cancellation was initiated (see 4.4) |
| `attribution_chain` | bounded chain | Recursive cause chain; bounded in Rust, unbounded links in C (§4.7) |
| `cleanup_budget` | `asx_budget` | Budget for the cleanup phase: the request's per-kind cleanup budget (§4.4), met with any earlier request's. Rust also meets it with a region's shutdown-budget ceiling (`src/runtime/state.rs:7832`) |
| `epoch` | `uint64_t` | Rust: set once by the first request. C: bumped by every changed request (§4.5) |
| `phase` | `asx_cancel_phase` | Current phase of the cancellation protocol |

### 4.9 Cancellation Propagation Rules (numbered 4.5 before 2026-10-10)

> **Superseded by `asupersync_v4_formal_semantics.md` §3.2 `CANCEL-REQUEST` (rule `rule.cancel.request` #1) and §5 INV-CANCEL-PROPAGATES (rule `inv.cancel.propagates_down` #6); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.request` and `inv.cancel.propagates_down`.**

1. A region **cancel** propagates to every task of the region's subtree (Rust `cancel_request`, `src/runtime/state.rs:7547`; C `asx_region_cancel`, `src/runtime/cancellation.c:430`). A plain C `asx_region_close` cancels no task; `asx_region_drain` uses the task-only `asx_cancel_propagate` with ParentCancelled (C_REFINEMENT_MAP row `rule.region.close_cancel_children`)
2. Order: parents before descendants in both, but not depth-first and not in the same sibling order. Rust collects the subtree with a stack-based walk and stable-sorts it by depth (`src/runtime/state.rs:7904`, sort at `src/runtime/state.rs:7653`): level by level, and within a level the last-inserted child comes first. C walks breadth-first over each region's `children[]` (`asx_region_subtree_internal`, `src/runtime/lifecycle.c:587`): within a level, `children[]` order, which is insertion order until a child closes and is swap-removed (`src/runtime/quiescence.c:35`). Within one region, both cancel tasks in membership insertion order (Rust `src/record/region.rs` membership; C `member_seq`, `src/runtime/cancellation.c:365`)
3. Each task takes its region's reason: the target region's tasks the request's reason; a descendant region's tasks `ParentCancelled`, attributed to the immediate parent and caused by the parent's reason (§4.7). The C witness is not extended with a chain
4. Cancel does **not** propagate to sibling regions (only parent-to-child)
5. A task already in `CancelRequested` or later is strengthened (T6/T9/T12), not ignored; a Completed task is skipped (Rust `src/record/task.rs:705`; C `src/runtime/cancellation.c:115`)
6. Cancel propagation is deterministic in each engine: same tree, same membership order, same trigger = same order. The two engines' orders differ for sibling regions (item 2); no Rust-captured fixture cancels a region that has two child regions, so the difference is not compared

### 4.10 Cleanup Budget Contract (numbered 4.6 before 2026-10-10)

> **Superseded by `asupersync_v4_formal_semantics.md` §3.2.1 and §3.2.3 (bounded cleanup) and §6 `PROG-CANCEL` (`prog.cancel.drains` #9); C status: see C_REFINEMENT_MAP.md rows `rule.cancel.acknowledge` and `prog.cancel.drains`.**

1. The cleanup budget is fixed by the request (per-kind table, §4.4) and becomes the task's budget when the cancel is acknowledged (T7); a later request during cleanup tightens it (T9/T12)
2. The budget bounds cleanup polls (its poll quota; the per-kind budgets set no deadline or cost quota). The spec's `budget ∧ policy(reason)` with the region budget is not what either engine computes (C_REFINEMENT_MAP row `rule.cancel.request`)
3. Exceeding it never force-completes a task in Rust: the lab strengthens the reason to `PollQuota` and the task runs on (`src/lab/runtime.rs:4667`). C behaves the same by default; only the opt-in `cleanup_hard_bound` force-completes (`src/runtime/scheduler.c:1391`, `bd-9kll.3.2`)
4. There is no `cleanup_budget_exceeded` flag in either engine (the first extraction described one). C's forced completion emits the scheduler event `ASX_SCHED_EVENT_CANCEL_FORCED`
5. Cleanup budget exhaustion is deterministic for a fixed seed and schedule

---

## 5. Outcome Severity Lattice

> **Superseded by `asupersync_v4_formal_semantics.md` §1.2 (`def.outcome.four_valued` #29, `def.outcome.severity_lattice` #30, `def.outcome.join_semantics` #31), §3.3 `CLOSE-COMPLETE` (`rule.region.close_complete` #26, region outcome) and §7 LAW-JOIN-ASSOC (`law.join.assoc` #42); C status: see C_REFINEMENT_MAP.md rows `def.outcome.four_valued`, `def.outcome.severity_lattice`, `def.outcome.join_semantics` (Partial), `rule.region.close_complete` (Partial) and `law.join.assoc` (Partial).** Rust `Outcome` is at `src/types/outcome.rs:218` and carries a payload (value, error, cancel reason, panic payload). C `asx_outcome` (`include/asx/core/outcome.h:21`) holds only the severity (`include/asx/asx_ids.h:138`); the cancel reason and panic message stay on the task record (`bd-9kll.3.14`).

### 5.1 Severity Ordering

```
Ok < Err < Cancelled < Panicked
```

This is a total order. The "worst" outcome dominates in joins/aggregations.

### 5.2 Outcome Values

| Outcome | Enum Value | Severity Rank | Description |
|---------|-----------|---------------|-------------|
| `Ok` | `ASX_OUTCOME_OK` | 0 (lowest) | Task completed successfully |
| `Err` | `ASX_OUTCOME_ERR` | 1 | Task encountered an error |
| `Cancelled` | `ASX_OUTCOME_CANCELLED` | 2 | Task was cancelled via cancellation protocol |
| `Panicked` | `ASX_OUTCOME_PANICKED` | 3 (highest) | Task encountered an unrecoverable failure |

### 5.3 Join Semantics

When aggregating outcomes from multiple children (e.g., region close). Rust `Outcome::join` is at `src/types/outcome.rs:566`; C `asx_outcome_join` is at `src/core/outcome.c:15`.

```
join(a, b) = the operand with the higher severity; on equal severity, a (left bias)
             Rust only: join(Cancelled(r1), Cancelled(r2)) = Cancelled(r1 strengthened by r2)
             when r2 is strictly more severe than r1
```

Properties (on severities; on values the left bias makes join non-commutative, as Rust's doc comment states):

- **Commutative:** `join(a, b) == join(b, a)` in severity
- **Associative:** `join(join(a, b), c) == join(a, join(b, c))` in severity (`law.join.assoc` #42)
- **Idempotent:** `join(a, a) == a`
- **Identity element:** `Ok` in severity (`join(Ok, x)` has the severity of `x`)
- **Absorbing element:** `Panicked` (joining with `Panicked` always yields `Panicked`)

C outcomes carry no reason, so C cannot strengthen a `Cancelled` + `Cancelled` join (C_REFINEMENT_MAP row `def.outcome.join_semantics`, `bd-9kll.3.14`).

### 5.4 Outcome Join Truth Table

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

### 5.5 Region Outcome Computation

The spec computes it at `CLOSE-COMPLETE` as `R[r].policy.aggregate(child_outcomes, finalizer_outcomes)`. Rust folds each completed task's outcome and each finalizer's outcome into the region's `close_outcome` with `join` (`record_close_outcome`, `src/record/region.rs:1144`, called at `src/runtime/state.rs:8521`):

```
region_outcome = fold(join, default, [child_1_outcome, ..., child_n_outcome, finalizer_outcomes...])
```

If nothing was folded, the default is `Cancelled(reason)` when the region was cancelled and `Ok` otherwise (`src/record/region.rs:1820`), so an empty region is not always `Ok`. C computes no region outcome: the region slot has no outcome field (`src/runtime/runtime_internal.h:28`; C_REFINEMENT_MAP row `rule.region.close_complete`, `bd-9kll.3.4`). Vocabulary v2 has no region outcome, so no fixture compares it.

---

## 6. Cross-Domain Ordering Constraints

### 6.1 Region-Task Ordering

> **Superseded by `asupersync_v4_formal_semantics.md` §3.1 `SPAWN` (`rule.ownership.spawn` #36), §5 INV-TASK-OWNED (`inv.ownership.task_owned` #34) and INV-QUIESCENCE (`inv.region.quiescence` #27); C status: see C_REFINEMENT_MAP.md rows `rule.ownership.spawn` (Partial), `inv.ownership.task_owned` and `inv.region.quiescence`.**

1. A normal task can only be created in a region that is `Open`. Rust also admits cleanup tasks in `Finalizing`; C admits every spawn in `Finalizing` (§1.4, `bd-9kll.3.5`)
2. A task's owning region must remain in a non-`Closed` state while the task is non-terminal (both engines close a region only with no live task)
3. Region `Closing`/`Draining` -> `Finalizing` requires all owned tasks to be `Completed` and all child regions `Closed`
4. Task completion advances the owning region: Rust unlinks the task and calls `advance_region_state` (`src/runtime/state.rs:8509`); C calls `asx_region_settle_internal` (`src/runtime/lifecycle.c:572`)

### 6.2 Task-Obligation Ordering

> **Superseded by `asupersync_v4_formal_semantics.md` §3.4 `RESERVE` (`rule.obligation.reserve` #13), §3.4.7 (cancellation does not resolve obligations) and §5 INV-OBLIGATION-BOUNDED (`inv.obligation.bounded` #19); C status: see C_REFINEMENT_MAP.md rows `rule.obligation.reserve` (Partial) and `inv.obligation.bounded` (Partial).**

1. An obligation is reserved by a Running holder in an `Open` region. Rust requires the holder to belong to that region and rejects otherwise with `ErrorKind::TaskNotOwned` (`create_obligation_in`, `src/runtime/state.rs:6130`). C checks neither that the holder belongs to the region nor, for `asx_obligation_reserve` outside a poll, that there is a holder at all (`src/runtime/lifecycle.c:1322`; the missing holder is `bd-9kll.3.4` item 5)
2. Obligations are associated with both the holder task and the region
3. Cancellation does **not** resolve obligations. When the holder completes, each obligation it still holds is leaked (Rust `src/runtime/state.rs:8341`; C `src/runtime/lifecycle.c:515`)
4. Outstanding obligations at region `Finalizing`: Rust leak-audits them once no task is left; C refuses to close (§1.5)

### 6.3 Cancel-Region Ordering

> **Superseded by `asupersync_v4_formal_semantics.md` §3.3 `CLOSE-CANCEL-CHILDREN` (`rule.region.close_cancel_children` #23) and `CLOSE-CHILDREN-DONE` (#24); C status: see C_REFINEMENT_MAP.md rows `rule.region.close_cancel_children` (Variant) and `rule.region.close_children_done`.**

1. A region cancel moves the region to `Closing` and cancels the tasks of its subtree in the same call (§4.9). A plain close (`asx_region_close`) cancels nothing
2. Propagation finishes before `cancel_request` / `asx_region_cancel` returns, so it precedes any later `Draining` step
3. Task completion advances region drain progress (§6.1 item 4)
4. All tasks must reach `Completed` (and child regions `Closed`) before the region enters `Finalizing`. `Draining` itself only means "child regions still open" (§1.2 R2)

### 6.4 Deterministic Ordering Keys

For deterministic scheduling and replay. The first extraction gave the plan's key `(lane_priority, logical_deadline, task_id, insertion_seq)` (`PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`) as the ready-queue key; **no C code uses that key**, and Rust does not either. The rows below are what the engines do. The lab-dispatch rows are normative only in the weak sense of `asupersync_v4_formal_semantics.md` §2.4 ("deterministic tie-breaking"); C status: see C_REFINEMENT_MAP.md supplementary rows "§1.11 lanes" and "§3.0 SCHEDULE-STEP" and rows `inv.determinism.replayable`, `def.determinism.seed_equivalence`.

| Context | Tie-Break Key | Ordering |
|---------|--------------|----------|
| Ready queue (Rust lab, C lab dispatch) | lane first (cancel lane while the cancel streak is below 16, then due timed, then ready, then fallback cancel), then highest priority, then `rng % n` over the top-priority group in generation order (at most 256 entries) | One xorshift64 draw per step from the scenario seed. Rust: `pop_for_worker` (`src/lab/runtime.rs:6324`), `tie_break_index` (`src/runtime/scheduler/priority.rs:417`). C: `asx_lab_pick` (`src/runtime/lab_dispatch.c:380`), tie-break `lab_pop` (`src/runtime/lab_dispatch.c:344`). C has no timed lane; Rust feeds its timed lane from no non-test code at `5e60b1c4c` |
| Ready queue (C round-robin `asx_scheduler_run`) | task slot index | Ascending; no lanes, priority unused (`src/runtime/scheduler.c:1295`; `bd-9kll.4.6`) |
| Timer wheel | Rust: 1 ms tick, then insertion order within a wheel slot (a cascade from a higher level can put an earlier-registered timer after later ones). C lab: `(1 ms tick, registration)` (`src/runtime/scheduler.c:269`); C native task timers: `(exact deadline, arm sequence)` (`src/runtime/scheduler.c:101`) | Ascending (earlier = fires first). Same-tick ordering is recorded in `bd-9kll.7.1`; see `docs/CHANNEL_TIMER_SEMANTICS.md` |
| Cancel propagation | Parents before descendants; sibling order differs between the engines (§4.9 item 2) | Deterministic tree walk order in each engine |
| Event journal | C: monotonic trace `sequence` (`src/runtime/trace.c:211`) | Strictly monotonic per runtime. The conformance oracle compares the Foata-canonical trace, not raw order (C_REFINEMENT_MAP supplementary row "§1.8 independence / Foata") |

---

## 7. Quiescence Invariant

> **Superseded by `asupersync_v4_formal_semantics.md` §1.12 (`Quiescent(r)`), §5 INV-QUIESCENCE (`inv.region.quiescence` #27) and INV-LEDGER-EMPTY-ON-CLOSE (`inv.obligation.ledger_empty_on_close` #20), with §6 PROG-REGION (`prog.region.close_terminates` #28); C status: see C_REFINEMENT_MAP.md rows `inv.region.quiescence`, `inv.obligation.ledger_empty_on_close` and `prog.region.close_terminates` (Partial).** The spec defines quiescence per region: `Quiescent(r) ≜` every child task Completed ∧ every child region Closed ∧ `ledger(r) = ∅`, and INV-QUIESCENCE requires it of every Closed region.

### 7.1 Definition

The runtime-level predicate the first extraction described is not Rust's. Rust's `RuntimeState::is_quiescent` (`src/runtime/state.rs:7364`) holds when there is no live task, no pending obligation, no pending cancel dispatch, the I/O driver (if any) has no registration, and every region has an empty finalizer stack, no pending spawn or obligation and is **not closing** (Open regions are fine). `LabRuntime::is_quiescent` adds an empty spawn mailbox and no pending obligation posts (`src/lab/runtime.rs:3069`). Items 4 and 5 below are not part of either; the fixture `quiescence-pending-timer-001` ends `quiescent: true` with both its regions Open.

1. No live tasks (all tasks in `Completed` state)
2. No pending obligations (none `Reserved`; `Committed`, `Aborted` and `Leaked` are terminal)
3. No region in `Closing`, `Draining` or `Finalizing` (Rust), and no pending finalizer or spawn
4. ~~No pending timer expirations in the timer wheel~~ not a Rust condition
5. ~~No pending channel messages awaiting delivery~~ not a Rust condition
6. (Profile-dependent) No pending I/O registrations

### 7.2 Quiescence Checks

C has two region-level checks: `asx_quiescence_check` (`src/runtime/quiescence.c:135`: the region is Closed, has no live task, its cleanup stack is drained and it has no Reserved obligation) and `asx_quiescence_check_detailed` (`src/runtime/quiescence.c:161`, Q1–Q4). Every fixture compares the snapshot's `quiescent` flag with Rust's (C_REFINEMENT_MAP row `inv.region.quiescence`). The "Error if Violated" column below is what C actually returns; the first extraction's codes are kept in brackets where C never returns them.

| Check | Condition | Error if Violated |
|-------|-----------|-------------------|
| Task quiescence | the region has no live task | `ASX_E_QUIESCENCE_TASKS_LIVE` (`src/runtime/quiescence.c:144`) [`ASX_E_TASKS_STILL_ACTIVE`: declared, never returned, `bd-udlh`] |
| Obligation quiescence | no `Reserved` obligation in the region | `ASX_E_OBLIGATIONS_UNRESOLVED` (`src/runtime/quiescence.c:47`) |
| Region quiescence | the region is `Closed` and its cleanup stack drained | `ASX_E_QUIESCENCE_NOT_REACHED` (`src/runtime/quiescence.c:143`) [`ASX_E_REGIONS_NOT_CLOSED`: declared, never returned] |
| Timer quiescence | not checked by C or Rust | [`ASX_E_TIMERS_PENDING`: declared, never returned] |
| Channel quiescence | not checked by C or Rust | [`ASX_E_CHANNEL_NOT_DRAINED`: declared, never returned] |

### 7.3 Quiescence and Runtime Shutdown

The Rust runtime-shutdown sequence was not re-verified at `5e60b1c4c` (unverified). What was verified: a Rust region close command is a `cancel_request` with an optional shutdown budget followed by `advance_region_state` (`close_region_command_in_task_table`, `src/runtime/state.rs:5004`). C's structured shutdown of a subtree is `asx_region_drain` (`src/runtime/quiescence.c:291`):

1. Every `Open` region in the subtree moves to `Closing`, parents first
2. Every live task of the subtree is cancelled with `ParentCancelled` through the task-only `asx_cancel_propagate` (the regions' own reasons are not set; C_REFINEMENT_MAP row `inv.cancel.propagates_down` notes)
3. The scheduler runs over the subtree until no task is live (resumable after budget exhaustion)
4. Regions finalize bottom-up (children before parents), draining cleanup stacks LIFO
5. Timers and channels are not drained by this sequence (unverified for Rust)
6. The call returns `ASX_OK`, or the first blocking status (`ASX_E_QUIESCENCE_TASKS_LIVE`, `ASX_E_OBLIGATIONS_UNRESOLVED`, …)

---

## 8. Forbidden Behavior Catalog

### 8.1 Critical Forbidden Behaviors (Must-Fail Immediately)

The Expected Result column gives C's actual status (verified in `src/runtime/lifecycle.c` and `src/core/transition_tables.c` at `b412780`) and Rust's result where it differs in kind. Rust-captured v2 fixtures are named where one exercises the row.

| ID | Forbidden Behavior | Expected Result | Fixture Category |
|----|-------------------|-----------------|------------------|
| FB-001 | Create child task in non-`Open` region | C `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:1007`), Rust `SpawnError::RegionClosed`; but C admits the spawn in `Finalizing` (`bd-9kll.3.5`). Fixtures: `spawn-into-cancelled-region-step-001`, `task-groups-refused-members-001`, `region-lifecycle-closed-child-spawn-001` | region-gate |
| FB-002 | Create obligation in non-`Open` region | C `ASX_E_REGION_CLOSED` (`src/runtime/lifecycle.c:1286`), Rust `ObligationAdmissionError::RegionClosed`. Fixture: `obligation-reserve-closed-region-must-fail-001` | region-gate |
| FB-003 | Double-commit obligation | C `ASX_E_INVALID_TRANSITION` (`src/runtime/lifecycle.c:1377`); Rust `ObligationAlreadyResolved` (§3.3). Not reachable from DSL v2; C test `tests/invariant/lifecycle/test_lifecycle_legality.c` (`obligation_double_commit_rejected`) | obligation-linearity |
| FB-004 | Double-abort obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | obligation-linearity |
| FB-005 | Commit then abort obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` (C test `obligation_commit_then_abort_rejected`) | obligation-linearity |
| FB-006 | Abort then commit obligation | C `ASX_E_INVALID_TRANSITION`; Rust `ObligationAlreadyResolved` | obligation-linearity |
| FB-007 | Backward state transition (any domain) | `ASX_E_INVALID_TRANSITION` from the C tables (`src/core/transition_tables.c:72`); Rust record transitions return `false` | transition-legality |
| FB-008 | Skip intermediate state in region close | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-009 | Skip intermediate state in task lifecycle (e.g., Created->Cancelling, Running->Finalizing) | `ASX_E_INVALID_TRANSITION` | transition-legality |
| FB-010 | Operate on stale/freed handle | C `ASX_E_STALE_HANDLE` when the slot was reused (generation mismatch), `ASX_E_NOT_FOUND` when the slot is free (`src/runtime/lifecycle.c:206`). Rust ids are typed arena ids | handle-safety |
| FB-011 | Close region with unresolved obligations | C: the region stays `Finalizing` and `asx_region_finalize_one` returns `ASX_E_OBLIGATIONS_UNRESOLVED`. Rust: leak-audits them (Leaked under the leak policy) and closes. Drift, `bd-9kll.3.4` (§1.5) | obligation-leak |
| FB-012 | Close region with active child tasks | The region stays `Closing` (or `Draining` if it has child regions) until its tasks complete. Fixtures: `region-lifecycle-close-cancels-children-001`, `region-lifecycle-draining-with-child-region-001` | region-drain |
| FB-013 | Cancel already-completed task | Idempotent no-op returning OK (Rust `src/record/task.rs:705`; C `src/runtime/cancellation.c:115`). C's handle abort (`asx_task_abort_request`) of a task that ended `Cancelled` and is not yet joined strengthens its stored reason, following Rust's handle-side reason cache (`apply_or_defer_cancel_reason`, `src/runtime/task_handle.rs:470`; the terminal case is unverified) | cancel-idempotent |
| FB-014 | Access region arena after `Closed` | C `ASX_E_REGION_CLOSED` (the only arena user is `asx_task_spawn_captured`, `src/runtime/lifecycle.c:1105`) | region-access |
| FB-015 | Non-deterministic behavior in deterministic mode | Detected by the conformance oracle: every fixture compares the Foata-canonical trace and the exact lab dispatch order with Rust's (C_REFINEMENT_MAP row `inv.determinism.replayable`). The ghost determinism monitor (`src/core/ghost.c`) has no runtime call site | determinism |

### 8.2 Resource Exhaustion Forbidden Behaviors

These rows describe C's fixed-capacity resource plane; Rust allocates and has no counterpart. They were not re-verified in this pass, except: a full C lab lane makes the lab step fail with `ASX_E_RESOURCE_EXHAUSTED` (`src/runtime/scheduler.c:1210`), and a full `src/time/timer_wheel.c` table returns `ASX_E_RESOURCE_EXHAUSTED`.

| ID | Forbidden Behavior | Expected Result | Fixture Category |
|----|-------------------|-----------------|------------------|
| FB-100 | Exceed max ready queue capacity | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) | exhaustion |
| FB-101 | Exceed max timer node count | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) | exhaustion |
| FB-102 | Exceed max cancel queue capacity | `ASX_E_RESOURCE_EXHAUSTED` (reject, no partial state) | exhaustion |
| FB-103 | Exceed runtime memory ceiling | `ASX_E_RESOURCE_EXHAUSTED` (failure-atomic, no corruption) | exhaustion |
| FB-104 | Exceed trace event capacity | `ASX_E_RESOURCE_EXHAUSTED` or ring-buffer overwrite (profile-dependent) | exhaustion |

### 8.3 Protocol Violation Forbidden Behaviors

C-only debug monitors (`src/core/ghost.c`, `src/core/affinity.c`); Rust enforces these through its type system. Not re-verified in this pass.

| ID | Forbidden Behavior | Expected Result | Fixture Category |
|----|-------------------|-----------------|------------------|
| FB-200 | Yield across obligation boundary without copy | Ghost borrow ledger violation (debug) | protocol-safety |
| FB-201 | Mutable access during shared borrow epoch | Ghost borrow ledger violation (debug) | protocol-safety |
| FB-202 | Cross-thread access without transfer certificate | Ghost affinity violation (debug) | thread-safety |
| FB-203 | Non-checkpoint long-running loop in cancel path | CI lint/static analysis flag | checkpoint-coverage |

---

## 9. Fixture ID Mapping

Each transition rule and forbidden behavior maps to candidate fixture IDs for conformance testing.

**None of the 50 planned IDs below was ever captured.** None exists as `fixtures/rust_reference_v2/<id>.json` or under `fixtures/rust_reference/` (checked 2026-10-10); 23 of them appear only as `fixture_ids` in `schemas/invariant_schema.json`. They are kept as plain text (not as fixture citations) so that the rows stay recognizable. The "Covered by" column names the Rust-captured v2 fixtures (`fixtures/rust_reference_v2/<id>.json`, run by `make conformance`) or C tests that exercise the row today. Vocabulary v2 has no task-state, Draining or Finalizing events, so a fixture compares intermediate states only when they are end states (`docs/C_REFINEMENT_MAP.md`, "What a fixture can show"). Fixtures per rule: C_REFINEMENT_MAP.md.

### 9.1 Region Fixtures

| Planned ID (never captured) | Description | Tests | Covered by |
|-----------|-------------|-------|------------|
| region-lifecycle-001 | Open -> Closing -> Draining -> Finalizing -> Closed (happy path) | Legal transition sequence | Partly: `region-lifecycle-close-cancels-children-001` (closes without Draining), `region-lifecycle-draining-with-child-region-001` (ends in Draining) |
| region-lifecycle-002 | Open -> Closing with active children (drain required) | Drain blocking behavior | `region-lifecycle-close-cancels-children-001` |
| region-lifecycle-003 | Nested region cascade close | Parent cancel propagates to children | `region-lifecycle-cancel-propagates-001` (a parent-child chain; no sibling regions) |
| region-lifecycle-004 | Region close with unresolved obligations | Leak detection and reporting | None for an obligation still Reserved at close (C and Rust differ, §1.5); C test `tests/invariant/lifecycle/skeleton_test.c` (`region_drain_blocks_unresolved_obligations`) |
| region-lifecycle-005 | Attempted backward transition (Closing -> Open) | Must fail with `ASX_E_INVALID_TRANSITION` | C test `tests/unit/core/test_transition.c` (`region_forbidden_backward`) |
| region-lifecycle-006 | Attempted skip transition (Open -> Draining) | Must fail with `ASX_E_INVALID_TRANSITION` | C test `tests/unit/core/test_transition.c` (`region_forbidden_skip`) |
| region-lifecycle-007 | Create task in non-Open region | Must fail with `ASX_E_REGION_CLOSED` (was `ASX_E_REGION_NOT_OPEN`) | `spawn-into-cancelled-region-step-001`, `region-lifecycle-closed-child-spawn-001` |
| region-lifecycle-008 | Region arena access after Closed | Must fail with `ASX_E_REGION_CLOSED` | None |
| region-lifecycle-009 | Empty region close (no children) | Closing -> Finalizing -> Closed at once | `region-lifecycle-closed-child-spawn-001` (an empty child region closes as soon as its parent is cancelled) |
| region-lifecycle-010 | Region close under resource exhaustion | Failure-atomic close behavior | None (C-only resource plane) |

### 9.2 Task Fixtures

| Planned ID (never captured) | Description | Tests | Covered by |
|-----------|-------------|-------|------------|
| task-lifecycle-001 | Created -> Running -> Completed(Ok) (happy path) | Legal transition sequence | `task-lifecycle-spawn-join-001` |
| task-lifecycle-002 | Full cancel: Created -> Running -> CancelRequested -> Cancelling -> Finalizing -> Completed(Cancelled) | All cancel phases traversed | Partly: `cancel-masked-checkpoint-001` ends Completed(Cancelled); the intermediate states are not compared, and C usually skips Finalizing (T10) |
| task-lifecycle-003 | CancelRequested -> Completed (completion before acknowledgement) | Outcome is `Cancelled(reason)`, not the natural outcome (T8; the first extraction had this backwards) | None isolates it |
| task-lifecycle-004 | Created -> CancelRequested (cancel before first poll) | T2 | `task-abort-next-step-001` (a fresh child is aborted at its admission). C goes through Running (`bd-9kll.3.9`), which vocabulary v2 cannot see |
| task-lifecycle-005 | Created -> Completed(Err) (error at spawn) | T3 | None; C never takes T3 |
| task-lifecycle-006 | Cancel strengthen: repeated requests with increasing severity | T6/T9/T12: reason/budget updated, not a new cancel | `cancel-strengthen-severity-001` (region reason), `task-abort-next-step-001` (two aborts coalesce to the stronger reason) |
| task-lifecycle-007 | Cancel strengthen budget combine: min-plus algebra | Combined quota=min, combined priority=max | C tests `tests/unit/core/test_budget.c` (`budget_meet_tightens`) |
| task-lifecycle-008 | Attempted backward transition (Running -> Created) | Must fail | C test `tests/unit/core/test_transition.c` |
| task-lifecycle-009 | Attempted skip transition (Running -> Cancelling) | Must fail (must go through CancelRequested) | C test `tests/unit/core/test_transition.c` |
| task-lifecycle-010 | Completed is absorbing: all transitions from Completed rejected | Must fail for all 6 possible targets | C test `tests/unit/core/test_transition.c` (`task_completed_absorbing`) |
| task-lifecycle-011 | Acknowledgement applies the cleanup budget | T7 | C test `tests/unit/runtime/test_cancellation.c` (`acknowledgement_replaces_the_budget_with_the_cleanup_budget`); `budget-inherit-before-cleanup-001` (cleanup starts after the acknowledging poll) |
| task-lifecycle-012 | finalize_done() produces CancelWitness | T13: witness with task_id, region_id, epoch, phase | None; the C witness has no task, region or epoch (§4.5) |
| task-lifecycle-013 | cancel_epoch set only by the first cancel | Epoch 0 then 1, constant on strengthening | None; C bumps it on every changed request (`bd-9kll.3.7`) |

### 9.3 Obligation Fixtures

| Planned ID (never captured) | Description | Tests | Covered by |
|-----------|-------------|-------|------------|
| obligation-lifecycle-001 | Reserved -> Committed (happy path) | Legal transition | `obligation-reserve-commit-001` |
| obligation-lifecycle-002 | Reserved -> Aborted (rollback path) | Legal transition | `obligation-abort-reasons-001` |
| obligation-lifecycle-003 | Reserved -> Leaked (holder completes or drops it; not "region finalization") | Leak detection | `obligation-cancelled-holder-leaks-001`, `leak-policy-leak-reported-001`, `obligation-drop-leaks-at-once-001` |
| obligation-lifecycle-004 | Double-commit | Must fail; C `ASX_E_INVALID_TRANSITION` (was `ASX_E_OBLIGATION_ALREADY_RESOLVED`, §3.3) | C test `tests/invariant/lifecycle/test_lifecycle_legality.c` (`obligation_double_commit_rejected`); not reachable from DSL v2 |
| obligation-lifecycle-005 | Double-abort | Must fail; C `ASX_E_INVALID_TRANSITION` | Same file (`obligation_double_abort_rejected`) |
| obligation-lifecycle-006 | Commit then abort | Must fail; C `ASX_E_INVALID_TRANSITION` | Same file (`obligation_commit_then_abort_rejected`) |
| obligation-lifecycle-007 | Abort then commit | Must fail; C `ASX_E_INVALID_TRANSITION` | Same file (`obligation_abort_then_commit_rejected`) |
| obligation-lifecycle-008 | No Reserved obligation at region close | Region close blocked (C) or leak-audited (Rust) | C test `tests/invariant/lifecycle/skeleton_test.c` (`region_drain_blocks_unresolved_obligations`) |
| obligation-lifecycle-009 | Multiple obligations in single region | Independent lifecycle tracking | `obligation-abort-reasons-001` (three obligations) |
| obligation-lifecycle-010 | Obligation in non-Open region | Must fail with `ASX_E_REGION_CLOSED` (was `ASX_E_REGION_NOT_OPEN`) | `obligation-reserve-closed-region-must-fail-001` |

### 9.4 Cancellation Fixtures

| Planned ID (never captured) | Description | Tests | Covered by |
|-----------|-------------|-------|------------|
| cancel-protocol-001 | Full cancel protocol happy path | All phases traversed | Partly: `cancel-masked-checkpoint-001` |
| cancel-protocol-002 | Cancel propagation through region tree | Parents before descendants (not depth-first, §4.9) | `region-lifecycle-cancel-propagates-001` |
| cancel-protocol-003 | Cancel with cleanup budget | Cleanup budget per kind | Fixtures compare the snapshot's `cleanup_budget` (C_REFINEMENT_MAP row `rule.cancel.request`); `budget-inherit-before-cleanup-001` |
| cancel-protocol-004 | Cancel budget exceeded | Reason strengthened to PollQuota, no force-completion (§4.10) | None |
| cancel-protocol-005 | Multiple cancel on same target | Idempotent (strengthen) | `cancel-strengthen-severity-001`, `task-abort-next-step-001` |
| cancel-protocol-006 | Cancel attribution chain | Correct chain recorded | `region-lifecycle-cancel-propagates-001` (ParentCancelled chained to the parent's reason) |
| cancel-protocol-007 | Cancel reason kinds | Each reason kind produces correct metadata | User, Timeout, Deadline, PollQuota, ParentCancelled and Shutdown are fixture-checked (C_REFINEMENT_MAP row `def.cancel.reason_kinds`), e.g. `budget-deadline-sleep-checkpoint-001`, `budget-poll-quota-exhaustion-001`, `combinators-race-timeout-001` |
| cancel-protocol-008 | Backward cancel phase transition | Must fail | None; witnesses are not compared |
| cancel-protocol-009 | Nested cancel (cancel during cancelling) | T9 strengthening | None identified |
| cancel-protocol-010 | Deterministic cancel propagation order | Same order on replay | Every fixture replays the Rust dispatch order; sibling-region order is not covered (§4.9 item 6) |

### 9.5 Outcome Fixtures

| Planned ID (never captured) | Description | Tests | Covered by |
|-----------|-------------|-------|------------|
| outcome-lattice-001 | Severity ordering correctness | `Ok < Err < Cancelled < Panicked` | `task-lifecycle-err-outcome-001`, `task-lifecycle-panic-001`; C test `tests/unit/core/test_outcome.c` (`outcome_lattice_order`) |
| outcome-lattice-002 | Join commutativity | `join(a,b) == join(b,a)` in severity | C test `tests/unit/core/test_outcome.c` (`outcome_join_max_severity`) |
| outcome-lattice-003 | Join associativity | `join(join(a,b),c) == join(a,join(b,c))` | C test `tests/unit/core/test_outcome.c` (`outcome_join_associative`, one triple; C_REFINEMENT_MAP row `law.join.assoc`) |
| outcome-lattice-004 | Join identity (Ok) | `join(Ok, x)` has the severity of `x` | C test `tests/unit/core/test_outcome.c` |
| outcome-lattice-005 | Join absorbing (Panicked) | `join(Panicked, x) == Panicked` for all x | C test `tests/unit/core/test_outcome.c` |
| outcome-lattice-006 | Region outcome aggregation | Correct fold over children | None; C has no region outcome (§5.5) |
| outcome-lattice-007 | Empty region outcome | `Ok`, or `Cancelled(reason)` if the region was cancelled (§5.5) | None |

---

## 10. Invariant Schema Cross-Reference

This section maps each transition table to the machine-readable invariant schema rows planned under bd-296.4. There is no `invariants/` directory: the schema that exists is `schemas/invariant_schema.json`. The rule IDs of the v4 spec are machine-readable in `schemas/v4_rule_index.json`, and their C status is in `docs/C_REFINEMENT_MAP.md`.

### 10.1 Schema Row Format (Conceptual)

```yaml
- domain: region          # region | task | obligation | cancel
  from_state: Open
  to_state: Closing
  trigger: region_close
  legal: true
  preconditions:
    - region.state == Open
  postconditions:
    - region.state == Closing
    - region.admission_gate == closed
  error_code: null
  fixture_ids:
    - region-lifecycle-001
    - region-lifecycle-002
```

### 10.2 Coverage Matrix

Counts are of ordered state pairs (self-loops included). "Fixtures" counts Rust-captured fixtures among the planned IDs of section 9: none was captured; section 9 names the v2 fixtures that cover each row instead.

| Domain | Total States | Legal Transitions | Forbidden Transitions | Self-Transitions | Fixtures |
|--------|-------------|-------------------|----------------------|-----------------|----------|
| Region | 5 | 5 (incl. the Closing -> Finalizing skip path) | 20 (25 pairs; §1.3 lists 12 rows, `Closed -> (any)` as one row, and omits the 4 non-terminal self-loops) | 0 | 0 of 10 planned |
| Task | 6 | 13 (10 state-changing + 3 strengthening) | 23 | 3 (strengthening) | 0 of 13 planned |
| Obligation | 4 | 3 | 13 (16 pairs) | 0 | 0 of 10 planned |
| Cancellation | 4 phases | 10 rank-non-decreasing pairs in Rust's validator (6 rank-increasing + 4 same-phase); C's witness advance accepts only the 6 | 6 | 4 in Rust (strengthening); 0 in C | 0 of 10 planned |
| Cancel Kinds | 11 kinds | N/A (severity ladder) | N/A | N/A | N/A |
| Outcome | 4 | N/A (lattice) | N/A | N/A | 0 of 7 planned |
| **Total** | **34** | **31** | **62** | **7** | **0 of 50 planned** |

**Note on task transitions:** 13 total valid transitions = 10 state-changing + 3 strengthening self-transitions (T6, T9, T12). The first extraction counted region forbidden pairs as 12, obligation as 9, cancellation legal pairs as 7 and fixtures as 55; those numbers did not match the tables.

### 10.3 Downstream Dependencies

This document is a canonical input for:

- **bd-296.17**: Extract deterministic channel/timer kernel semantics and tie-break ordering contract
- **bd-296.18**: Extract quiescence, close, cleanup, and leak-detection invariants for finalization paths
- **bd-296.4**: Author machine-readable invariant schema and generation pipeline
- **bd-296.6**: Publish Rust->C guarantee-substitution matrix
- **bd-296.1**: Create exhaustive `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`
- **bd-296.19**: Build source-to-fixture provenance map
- **bd-1md.13**: Capture core semantic fixture families from Rust reference
- **bd-1md.15**: Capture vertical and continuity fixture families

---

## Appendix A: Error Code Reference (Transition-Related)

Values are from `include/asx/asx_status.h`. "Never returned" means no code under `src/` or `include/` other than the status table returns it (searched 2026-10-10). Rust equivalents: `docs/CANONICAL_VOCABULARY_V2.md` §5.

| Error Code | Value | Description |
|-----------|-------|-------------|
| `ASX_E_INVALID_TRANSITION` | 200 | Attempted illegal state transition (every C table check; also a double obligation resolve, §3.3) |
| `ASX_E_REGION_NOT_OPEN` | 303 | Declared; never returned. Admission failures return `ASX_E_REGION_CLOSED` |
| `ASX_E_REGION_CLOSED` | 301 | Spawn, child region, obligation or captured spawn refused by a region that is not admitting (§1.4) |
| `ASX_E_OBLIGATION_ALREADY_RESOLVED` | 500 | Declared; never returned (C returns `ASX_E_INVALID_TRANSITION`; Rust returns `ObligationAlreadyResolved`) |
| `ASX_E_OBLIGATION_LEAKED` | — | Does not exist in C |
| `ASX_E_UNRESOLVED_OBLIGATIONS` | 501 | Fault status for a leak under the PANIC leak policy, routed to region containment (`src/runtime/lifecycle.c:557`); not returned by finalization |
| `ASX_E_INCOMPLETE_CHILDREN` | 903 | Declared; never returned (`bd-udlh`) |
| `ASX_E_STALE_HANDLE` | 1100 | Handle generation mismatch (use-after-free defense); `ASX_E_NOT_FOUND` (102) when the slot is free |
| `ASX_E_RESOURCE_EXHAUSTED` | 1000 | Resource contract ceiling exceeded |
| `ASX_E_TASKS_STILL_ACTIVE` | 900 | Declared; never returned (`bd-udlh`). Region checks return `ASX_E_QUIESCENCE_TASKS_LIVE` (905) |
| `ASX_E_OBLIGATIONS_UNRESOLVED` | 901 | Region still has a Reserved obligation (`asx_region_finalize_one`, `asx_quiescence_check`; `src/runtime/quiescence.c:47`) |
| `ASX_E_REGIONS_NOT_CLOSED` | 902 | Declared; never returned. Region checks return `ASX_E_QUIESCENCE_NOT_REACHED` (904) |
| `ASX_E_TIMERS_PENDING` | 801 | Declared; never returned |
| `ASX_E_CHANNEL_NOT_DRAINED` | 703 | Declared; never returned |

---

## Appendix B: State Encoding Reference

For the bitmasked generational typestate handles (`[ 16-bit type_tag | 16-bit state_mask | 32-bit arena_index ]`). This is a C-only encoding (Rust ids are typed arena indices). In C the 32-bit index is itself `[generation:16 | slot:16]` (`include/asx/asx_ids.h:78`), and the generation is what detects stale handles.

### B.1 Type Tags

| Type | Tag Value | Bit Pattern |
|------|----------|-------------|
| Region | `0x0001` | `0000 0000 0000 0001` |
| Task | `0x0002` | `0000 0000 0000 0010` |
| Obligation | `0x0003` | `0000 0000 0000 0011` |
| Cancel Witness | `0x0004` | `0000 0000 0000 0100` |
| Timer | `0x0005` | `0000 0000 0000 0101` |
| Channel | `0x0006` | `0000 0000 0000 0110` |

### B.2 State Masks (Region Example)

| State | Mask Value | Bit Pattern |
|-------|-----------|-------------|
| `Open` | `0x0001` | `0000 0000 0000 0001` |
| `Closing` | `0x0002` | `0000 0000 0000 0010` |
| `Draining` | `0x0004` | `0000 0000 0000 0100` |
| `Finalizing` | `0x0008` | `0000 0000 0000 1000` |
| `Closed` | `0x0010` | `0000 0000 0001 0000` |

The mask values match the code (`1 << state`), but no API endpoint checks them: a handle's state mask is set when the handle is packed (for example `asx_region_handle_for_slot` always packs the `Open` bit) and is never updated, and `asx_handle_state_allowed` (`include/asx/asx_ids.h:71`) has no caller under `src/`. Endpoints validate a handle by type tag, slot bounds, liveness and generation (`asx_region_slot_lookup`, `src/runtime/lifecycle.c:206`) and then check the state stored in the slot.

---

*This document gives the lifecycle transition tables of the asx ANSI C port with the Rust and C code behind each row. Where it says a section is superseded, `asupersync_v4_formal_semantics.md` at `5e60b1c4c` is authoritative and `docs/C_REFINEMENT_MAP.md` gives C's status. Deviations require explicit approval and fixture additions.*
