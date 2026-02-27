# Unit and Invariant Completeness Matrix

> **Bead:** `bd-1md.12`  
> **Status:** Canonical module-level test-obligation matrix + drift-check contract  
> **Last updated:** 2026-02-27 by LilacTurtle, updated by PearlSwan

This document defines per-module test obligations across happy-path, edge, error, forbidden-transition, and exhaustion behavior. It also defines drift checks that fail when obligations or mappings become stale.

## 1. Normative Inputs

- `docs/FEATURE_PARITY.md`
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`
- `docs/LIFECYCLE_TRANSITION_TABLES.md`
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`
- `docs/CHANNEL_TIMER_DETERMINISM.md`
- `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md`
- `docs/GUARANTEE_SUBSTITUTION_MATRIX.md`
- `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md`

## 2. Coverage Dimensions

Each module row must map obligations for:

1. `happy`: legal nominal behavior.
2. `edge`: boundary/limit behavior.
3. `error`: classified explicit error surfaces.
4. `forbidden`: must-fail illegal transitions or protocol misuse.
5. `exhaustion`: resource/budget ceiling behavior with failure-atomicity.
6. `parity`: rust-vs-c, codec, and/or profile parity obligations.
7. `evidence`: minimum artifact/log requirements for closure.

## 3. Module Obligation Matrix

| module_id | semantic surface | happy obligations | edge obligations | error/forbidden obligations | exhaustion obligations | parity obligations | evidence minimum |
|---|---|---|---|---|---|---|---|
| `M-CORE-OUTCOME` | outcome lattice/join | `outcome-lattice-001..007` | `outcome-join-left-bias-001` | severity-order violation checks | N/A | `OUTCOME-001/002` (`rust_vs_c`, codec where mapped) | unit logs + algebraic-law report |
| `M-CORE-BUDGET` | budget algebra + exhaustion mapping | `budget-meet-identity-010` | `budget-deadline-boundary-015` | invalid combine/weakening rejection | `budget-consume-*`, `runtime-pollquota-cancel-016` | `BUDGET-001/002` | unit + invariant logs + exhaustion report |
| `M-CORE-CANCEL` | cancel witness and strengthening | `cancel-protocol-001` | equal-rank tie and timestamp/message ordering | phase regression forbidden | cleanup-budget overrun deterministic path | `TASK-001`, `FINALIZE-001` | invariant logs + witness transition trace |
| `M-CORE-TRANSITIONS` | region/task/obligation legality tables | legal edge traversal per phase | fast-path close (`Closing->Finalizing`) | `FB-001..FB-008` + invalid transition matrix | close under constrained resource paths | `REGION-*`, `TASK-*`, `OBLIGATION-*` | generated legality matrix + forbidden report |
| `M-RUNTIME-LIFECYCLE` | runtime close/finalize flow | region close to `Closed` | no-child fast path | unresolved-child/illegal-phase failures | deterministic leak surfacing on finalize | `QUIESCENCE-001`, `FINALIZE-001` | scenario trace + close diagnostics |
| `M-RUNTIME-SCHEDULER` | deterministic event sequencing | stable lane/tie-break dispatch | equal-priority/deadline FIFO | illegal reorder/non-deterministic tie fail | bounded cycle-budget and overload behavior | `SCHEDULER-001..006` | replay digest pair + fairness/cycle logs |
| `M-RUNTIME-QUIESCENCE` | quiescence truth contract | quiescence success with all-zero conditions | timer/channel near-drain boundaries | false-success with pending work forbidden (`FB-011`, `FB-012`) | close under resource pressure stays failure-atomic | `QUIESCENCE-001` | quiescence checklist artifact + error-code report |
| `M-CHANNEL-MPSC` | bounded two-phase channel | reserve/send/recv normal flow | FIFO waiter + backpressure edge paths | queue-jump, phantom waiter, double-resolution forbidden | reserved/full and eviction boundary behavior | `CHANNEL-001..007` | unit + stress logs + invariant counters |
| `M-TIME-WHEEL` | timer wheel/register/cancel/fire | register/fire/cancel nominal | equal-deadline, overflow, wrap, coalescing thresholds | stale-handle mutation forbidden | duration/slot limits with deterministic rejection | `TIMER-001..008` | unit logs + replay ordering digest |
| `M-CODEC` | canonical schema across JSON/BIN | encode/decode roundtrip | schema version boundaries | invalid payload classification surfaces | bounded decode/encode resource behavior | `codec_equivalence` rows by `prov_id` | codec equivalence report |
| `M-TRACE-REPLAY` | deterministic replay continuity | same-seed same-digest replay (`continuity-replay-identity-003`) | restart boundaries (mid-flight crash, checkpoint rollback) | digest drift classification and duplicate side-effect detection | trace budget/resource boundaries + persistence write failure-atomicity | `U-REPLAY-CONTINUITY`, `CONT-CRASH-RESTART` | replay manifest + digest comparison + restart continuity report |
| `M-PROFILE-PARITY` | cross-profile semantic equivalence | shared fixture parity across required profiles | HFT microburst and automotive watchdog profile edge paths | profile-specific semantic fork forbidden | profile resource ceilings preserve semantics under overload/degraded mode | `U-PROFILE-PARITY`, `VERT-HFT-MICROBURST`, `VERT-AUTOMOTIVE-WATCHDOG` | profile parity matrix + vertical family manifests + exceptions log |

Notes:

- `N/A` means the dimension is not primary for that module but cannot be omitted if introduced later.
- Every non-`N/A` obligation must map to concrete tests/fixtures before module closure.

## 4. Drift-Check Contract

Drift checks are mandatory and should be wired into CI as machine-readable outputs.

### 4.1 `DRIFT-001` Module Coverage Completeness

Fail when any module row is missing one of: `happy`, `edge`, `error/forbidden`, `parity`, or `evidence`.

Output field:

- `missing_dimensions: [{module_id, dimensions[]}]`

### 4.2 `DRIFT-002` Provenance Link Integrity

Fail when a fixture/prov-id referenced in this matrix is absent from `SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`.

Output field:

- `orphan_references: [{module_id, reference}]`

### 4.3 `DRIFT-003` Feature-Parity Synchronization

Fail when a semantic unit in `FEATURE_PARITY.md` lacks at least one owning module row in this matrix.

Output field:

- `unmapped_units: [unit_id]`

### 4.4 `DRIFT-004` Forbidden Coverage Regression

Fail when forbidden IDs (`FB-*` and explicit must-fail fixtures) decrease coverage without a linked approved decision update.

Output field:

- `forbidden_coverage_delta: {removed_ids[], added_ids[], decision_ref}`

### 4.5 `DRIFT-005` Evidence Artifact Drift

Fail when closure artifacts for a touched module do not include required evidence classes (unit/invariant/scenario/log manifest).

Output field:

- `missing_evidence: [{module_id, required_class}]`

## 5. Minimal CI Report Schema

Recommended JSON payload:

```json
{
  "matrix_version": "2026-02-27",
  "status": "pass|fail",
  "missing_dimensions": [],
  "orphan_references": [],
  "unmapped_units": [],
  "forbidden_coverage_delta": {
    "removed_ids": [],
    "added_ids": [],
    "decision_ref": ""
  },
  "missing_evidence": []
}
```

## 6. Review Checklist Integration

For each module-affecting PR/bead closure:

1. identify impacted `module_id` rows,
2. attach or update mapped fixture IDs,
3. run drift checks and include report artifact,
4. include evidence paths in closure notes,
5. record explicit decision reference when reducing any coverage class.

This matrix is the completeness authority for `bd-1md.*` planning, `bd-66l.9` evidence enforcement, and conformance/parity gate evolution.
