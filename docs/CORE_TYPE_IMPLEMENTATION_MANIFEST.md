# Core Type Implementation Manifest

> **Bead lane:** `bd-hwb` epic prework  
> **Purpose:** Implementation-ready manifest for `bd-hwb.1` (`asx_status`, `asx_outcome`, `asx_budget`, `asx_cancel_reason`, and generation-safe IDs/handles)  
> **Status:** Prework artifact while leaf tasks are still sequencing

## 1. Scope and Intent

This document reduces startup ambiguity for core-type implementation by normalizing:

- required C type surfaces,
- semantic invariants already extracted in Phase 1 docs,
- fixture/test obligations that must gate implementation,
- recommended implementation sequence once `bd-hwb.1` is claimable.

It does not supersede the canonical semantics documents; it is an execution map.

## 2. Normative Inputs

Primary semantic sources:

- `docs/LIFECYCLE_TRANSITION_TABLES.md`
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`
- `docs/CHANNEL_TIMER_DETERMINISM.md`
- `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md`
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`
- `docs/RUST_BASELINE_PROVENANCE.md`
- `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`

## 3. Core Surface Contract

### 3.1 `asx_status` (error taxonomy + stability)

Required shape:

- explicit enum with stable code families (`OK`, lifecycle legality, witness validation, resource exhaustion, stale-handle, serialization/conformance surfaces),
- no hidden side-channel errors; every state transition API returns status,
- status values are machine-mappable to fixture expectations.

Current semantic anchors:

- lifecycle/witness validation errors and forbidden transitions in `docs/LIFECYCLE_TRANSITION_TABLES.md`,
- hard-fail provenance/enforcement expectations in `docs/RUST_BASELINE_PROVENANCE.md`,
- quality gate expectations in `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`.

### 3.2 `asx_outcome` (severity lattice + joins)

Required shape:

- variants aligned with canonical order: `Ok < Err < Cancelled < Panicked`,
- join semantics preserve worst severity and equal-severity left-bias,
- cancellation outcome strengthening must be deterministic.

Current semantic anchors:

- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` sections 2 and 5,
- `docs/LIFECYCLE_TRANSITION_TABLES.md` outcome tables.

### 3.3 `asx_budget` (meet algebra + exhaustion)

Required shape:

- componentwise meet/tightening semantics,
- explicit `INFINITE` identity and `ZERO` absorbing behavior,
- deterministic exhaustion predicates and consume rules (`poll`, `cost`, `deadline`),
- no underflow or partial mutation on failed consume.

Current semantic anchors:

- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` sections 3 and 4.

### 3.4 `asx_cancel_reason` + witness metadata

Required shape:

- cancellation kind ladder with monotone severity,
- deterministic strengthening tie-break (severity, timestamp, message),
- bounded attribution chain metadata,
- witness transition legality checks with explicit error surfaces.

Current semantic anchors:

- `docs/LIFECYCLE_TRANSITION_TABLES.md` cancellation sections,
- cleanup budget mapping in `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`.

### 3.5 Generation-safe IDs and handles

Required shape:

- IDs/handles carry generation or epoch data needed to reject stale operations,
- stale-handle operations fail closed with explicit status,
- timer/channel coupling remains deterministic under cancellation/finalization.

Current semantic anchors:

- stale-handle and witness rules in `docs/LIFECYCLE_TRANSITION_TABLES.md`,
- timer handle generation semantics in `docs/CHANNEL_TIMER_DETERMINISM.md`,
- finalization coupling constraints in `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md`.

## 4. Proposed File Ownership for `bd-hwb.1`

Headers (public API surface):

- `include/asx/asx_status.h`
- `include/asx/asx_outcome.h`
- `include/asx/asx_budget.h`
- `include/asx/asx_cancel.h`
- `include/asx/asx_ids.h`

Core implementation units:

- `src/core/status.c`
- `src/core/outcome.c`
- `src/core/budget.c`
- `src/core/cancel.c`
- `src/core/ids.c`

Tests:

- `tests/unit/core/test_status.c`
- `tests/unit/core/test_outcome.c`
- `tests/unit/core/test_budget.c`
- `tests/unit/core/test_cancel.c`
- `tests/invariant/lifecycle/test_handle_and_witness_invariants.c`

## 5. Fixture and Test Obligation Matrix

| Surface | Must-prove behaviors | Candidate fixture/test links |
|---|---|---|
| `asx_status` | Forbidden transitions and witness validation produce stable codes | `FB-*` rows in `docs/LIFECYCLE_TRANSITION_TABLES.md` |
| `asx_outcome` | Lattice ordering + equal-severity left-bias + top/bottom behavior | `outcome-join-left-bias-001`, `outcome-join-top-003` |
| `asx_budget` | Meet identity/absorbing laws + deterministic consume failure behavior | `budget-meet-identity-010`, `budget-meet-absorbing-011`, `budget-consume-*` |
| `asx_cancel_reason` | Severity monotonicity + strengthening tie-break + cleanup budget mapping | cancellation sections in lifecycle + `cancel-cleanup-budget-017` |
| IDs/handles | Stale-handle rejection, generation mismatch safety, timer-cancel integrity | `FB-010`, `timer-cancel-generation-011` |

## 6. Dependency and Sequencing Notes

Immediate dependency posture:

- `bd-296.12` closed (baseline provenance complete),
- `bd-296.15`, `bd-296.16`, `bd-296.17`, `bd-296.18` provide required semantic slices,
- `bd-296.19` in progress should be treated as traceability authority for fixture/provenance mapping.

Recommended execution order once `bd-hwb.1` is claimable:

1. Freeze header enums/struct layouts for status/outcome/budget/cancel/IDs.
2. Implement pure semantic operations (`join`, `meet`, consume/exhaustion checks, reason strengthen).
3. Implement validation helpers for witness/handle legality.
4. Bind error/status mapping to forbidden-behavior fixture expectations.
5. Land unit + invariant tests and cross-link to provenance map rows.

## 7. Done Criteria for `bd-hwb.1` Readiness

`bd-hwb.1` should not be marked done until all hold:

- all five core surfaces have explicit C APIs and test-backed invariants,
- no semantic placeholder remains for lattice/algebra/witness rules,
- every exposed behavior maps to at least one fixture family or invariant test,
- status/error surfaces are stable and machine-checkable for conformance tooling,
- provenance links for each implemented rule are listed in the traceability map (`bd-296.19` artifact).
