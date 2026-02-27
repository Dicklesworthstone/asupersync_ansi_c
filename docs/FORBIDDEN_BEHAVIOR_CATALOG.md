# Forbidden Behavior Catalog (Must-Fail Contract)

> **Bead:** `bd-296.5`
> **Status:** Canonical forbidden-behavior baseline
> **Last updated:** 2026-02-27 by CopperSpire

This catalog defines semantic behaviors that must fail or emit explicit deterministic diagnostics. Passing these scenarios is a regression.

## 1. Contract Rules

1. Every forbidden behavior has a stable `fb_id`.
2. Every `fb_id` maps to at least one scenario DSL fixture.
3. Expected result must be explicit (error/status/diagnostic class).
4. No forbidden behavior may silently degrade into best-effort success.

## 2. Forbidden Categories

### 2.1 Transition Legality Violations (`FB-TX-*`)

| fb_id | Forbidden behavior | Expected result |
|---|---|---|
| `FB-TX-001` | Region phase regression (e.g. `Finalizing -> Draining`) | `ASX_E_INVALID_TRANSITION` |
| `FB-TX-002` | Task phase skip/backward transition | `ASX_E_INVALID_TRANSITION` |
| `FB-TX-003` | Cancellation witness phase regression | witness phase error |
| `FB-TX-004` | Cancellation reason weakening on strengthen path | witness reason error |

### 2.2 Obligation Linearity Violations (`FB-OBL-*`)

| fb_id | Forbidden behavior | Expected result |
|---|---|---|
| `FB-OBL-001` | Commit already-committed obligation | `ASX_E_OBLIGATION_ALREADY_RESOLVED` |
| `FB-OBL-002` | Abort already-aborted obligation | `ASX_E_OBLIGATION_ALREADY_RESOLVED` |
| `FB-OBL-003` | Commit after abort / abort after commit | `ASX_E_OBLIGATION_ALREADY_RESOLVED` |
| `FB-OBL-004` | Finalize with unresolved obligations but no leak surfacing | test-fail (missing required leak path) |

### 2.3 Handle Safety Violations (`FB-HDL-*`)

| fb_id | Forbidden behavior | Expected result |
|---|---|---|
| `FB-HDL-001` | Use stale region/task handle generation | stale-handle error |
| `FB-HDL-002` | Timer cancel with stale generation mutates live timer | test-fail (mutation forbidden) |
| `FB-HDL-003` | Type-tag mismatch accepted as valid handle | test-fail |

### 2.4 Channel/Timer Determinism Violations (`FB-DTM-*`)

| fb_id | Forbidden behavior | Expected result |
|---|---|---|
| `FB-DTM-001` | `try_reserve` bypasses queued waiter | deterministic fairness violation |
| `FB-DTM-002` | Equal-deadline timer order unstable across repeated runs | deterministic ordering violation |
| `FB-DTM-003` | Permit drop leaks reserved capacity | invariant failure |
| `FB-DTM-004` | Reserved-only full queue evicts reserved slots | forbidden backpressure behavior |

### 2.5 Exhaustion and Failure-Atomic Violations (`FB-EXH-*`)

| fb_id | Forbidden behavior | Expected result |
|---|---|---|
| `FB-EXH-001` | Exhaustion path partially mutates state before returning error | failure-atomic violation |
| `FB-EXH-002` | Poll/cost/deadline exhaustion emits non-deterministic status | deterministic failure violation |
| `FB-EXH-003` | Capacity miss silently drops work | test-fail |

### 2.6 Finalization/Quiescence Violations (`FB-QS-*`)

| fb_id | Forbidden behavior | Expected result |
|---|---|---|
| `FB-QS-001` | Quiescence success while active tasks remain | quiescence failure (`ASX_E_TASKS_STILL_ACTIVE`) |
| `FB-QS-002` | Quiescence success while timers pending | quiescence failure (`ASX_E_TIMERS_PENDING`) |
| `FB-QS-003` | Quiescence success while channels undrained | quiescence failure (`ASX_E_CHANNEL_NOT_DRAINED`) |
| `FB-QS-004` | Cleanup-budget overrun handled silently | deterministic cleanup violation |

## 3. Mapping Rules to Scenario DSL Fixtures

Fixture naming convention:

- `forbidden-<category>-<nnn>`

Examples:

- `forbidden-transition-001` -> `FB-TX-001`
- `forbidden-obligation-003` -> `FB-OBL-003`
- `forbidden-determinism-002` -> `FB-DTM-002`

A fixture is valid only if it declares:

- `forbidden_ids` containing one or more `fb_id` values,
- `expected_error_codes` and/or `expected_violation_kind`,
- deterministic seed and operation sequence.

## 4. Evidence Policy

A forbidden behavior row can be moved from `draft` to `enforced` only when:

1. at least one fixture exists in scenario DSL,
2. fixture executes in conformance/invariant layer,
3. expected violation is observed and stable across replay runs,
4. provenance is linked in the source-to-fixture map.

## 5. Downstream Dependencies

This catalog is an input to:

- `bd-1md.1` / `bd-1md.3` (fixture capture + differential fuzzing),
- `bd-1md.13` / `bd-1md.14` / `bd-1md.15` (fixture families),
- `bd-66l.*` (hard CI gate enforcement),
- `bd-296.30` (spec-review gate packet).
