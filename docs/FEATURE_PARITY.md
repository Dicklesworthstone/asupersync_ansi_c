# Feature Parity Matrix (Rust Reference -> ANSI C)

> **Bead:** `bd-296.3`
> **Status:** Semantic-unit parity tracker with acceptance-test mapping
> **Last updated:** 2026-05-08 by SilverFrog — parallel locality graduation evidence

This matrix tracks canonical semantic units, their source-of-truth extraction status, and required acceptance coverage before parity can be declared complete.

## 1. Status Legend

- `spec-reviewed`: canonical semantics extracted and reviewed for implementation use.
- `spec-drafted`: semantics extracted but not fully reviewed/locked.
- `impl-pending`: implementation not yet landed.
- `impl-in-progress`: implementation work has started.
- `impl-complete`: implementation landed and unit/invariant gates pass.
- `conformance-passed`: Rust-vs-C parity and/or unit-specific
  conformance/profile parity gates passed for this unit.

## 2. Canonical Unit Matrix

| Unit ID | Semantic Unit | Canonical Source Artifacts | Acceptance Test Obligations | Must-Fail / Negative Obligations | Current Status | Primary Bead(s) |
|---|---|---|---|---|---|---|
| `U-REG-TRANSITIONS` | Region lifecycle legality (`Open->...->Closed`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` | region transition unit + invariant suites; close/drain/finalize scenarios | invalid regressions/skips return `ASX_E_INVALID_TRANSITION` | `impl-complete` | `bd-296.15`, `bd-hwb.3` |
| `U-TASK-TRANSITIONS` | Task phase semantics + cancel checkpoints | `docs/LIFECYCLE_TRANSITION_TABLES.md` | task lifecycle fixtures; checkpoint/cancel observation tests | illegal backward/skip transitions rejected | `impl-complete` | `bd-296.15`, `bd-hwb.3` |
| `U-OBLIGATION-LINEARITY` | Reserve/commit/abort/leak exactly-once contract | `docs/LIFECYCLE_TRANSITION_TABLES.md`, `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | obligation lifecycle unit tests; leak-detection invariant tests | double resolve fails; unresolved finalization is surfaced | `impl-complete` | `bd-296.15`, `bd-296.18`, `bd-hwb.6` |
| `U-CANCEL-WITNESS` | Cancellation witness phase/rank/severity monotonicity | `docs/LIFECYCLE_TRANSITION_TABLES.md` | cancel protocol fixtures incl. strengthen paths | phase regression and reason weakening fail deterministically | `impl-complete` | `bd-296.15`, `bd-hwb.2` |
| `U-OUTCOME-LATTICE` | Outcome order and join semantics | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | algebraic law fixtures; aggregation tests | severity/order violations are test-fail regressions | `impl-complete` | `bd-296.16`, `bd-hwb.1` |
| `U-BUDGET-ALGEBRA` | Budget meet/combine and identity/absorbing behavior | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | budget law fixtures + edge-case tests | non-tightening combine and identity drift are failures | `impl-complete` | `bd-296.16`, `bd-hwb.1` |
| `U-EXHAUSTION-SEMANTICS` | Deadline/poll/cost exhaustion with failure-atomic semantics | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | exhaustion + rollback boundary tests | partial mutation on failure is forbidden | `impl-complete` | `bd-296.16`, `bd-hwb.6` |
| `U-CHANNEL-TWOPHASE` | MPSC reserve/send/abort/drop linearity | `docs/CHANNEL_TIMER_DETERMINISM.md` | channel kernel fixtures (`two-phase`, abort/drop release) | queue-jump, phantom waiter, reserved-slot theft forbidden | `impl-complete` | `bd-296.17`, `bd-2cw.5` |
| `U-CHANNEL-FAIRNESS` | FIFO waiter discipline and try-reserve gating | `docs/CHANNEL_TIMER_DETERMINISM.md` | fairness/backpressure scenario tests | bypassing queued waiter forbidden | `impl-complete` | `bd-296.17`, `bd-2cw.5` |
| `U-TIMER-ORDERING` | Equal-deadline insertion-stable ordering | `docs/CHANNEL_TIMER_DETERMINISM.md` | timer ordering fixtures + replay checks | nondeterministic equal-deadline order forbidden | `impl-complete` | `bd-296.17`, `bd-2cw.4` |
| `U-TIMER-HANDLE-SAFETY` | Generation-safe cancel handles | `docs/CHANNEL_TIMER_DETERMINISM.md` | stale-generation cancel tests | stale cancel mutating live timer state forbidden | `impl-complete` | `bd-296.17`, `bd-2cw.4` |
| `U-FINALIZATION-QUIESCENCE` | Quiescence criteria + finalization exit contract | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | close/quiescence invariants + shutdown scenarios | quiescence success with pending work forbidden | `impl-complete` | `bd-296.18`, `bd-2cw.6` |
| `U-LEAK-DETECTION` | Deterministic unresolved-obligation leak surfacing | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | leak fixtures + finalization diagnostics | silent leak and unresolved-close acceptance forbidden | `impl-complete` | `bd-296.18`, `bd-hwb.6` |
| `U-FORBIDDEN-CATALOG` | Explicit must-fail semantic catalog + IDs | `docs/FORBIDDEN_BEHAVIOR_CATALOG.md`, `docs/LIFECYCLE_TRANSITION_TABLES.md` | forbidden-path fixture family generation | missing expected failure outcome is parity break | `impl-complete` | `bd-296.5` |
| `U-INVARIANT-SCHEMA` | Machine-readable transition/invariant schema | `schemas/invariant_schema.json`, `docs/INVARIANT_SCHEMA.md` | schema validation + generated legality tests | schema drift without fixture updates forbidden | `impl-complete` | `bd-296.4` |
| `U-PROVENANCE-MAP` | Source->fixture->parity traceability mapping | `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` | provenance integrity checks in CI docs/reporting | orphan fixture rows forbidden | `spec-reviewed` | `bd-296.19` |
| `U-ARCH-LAYERING` | C architecture boundaries from semantics | `docs/PROPOSED_ANSI_C_ARCHITECTURE.md` | architecture conformance review + scaffold checks | transliteration-only or hidden coupling forbidden | `impl-complete` | `bd-296.2`, `bd-ix8.1` |
| `U-C-PORTABILITY-UB` | Portable C subset + UB elimination policy | `docs/C_PORTABILITY_RULES.md` | compiler/static-analysis + UB-focused tests | UB-prone constructs in core forbidden | `impl-complete` | `bd-296.29`, `bd-66l.10` |
| `U-GUARANTEE-SUBSTITUTION` | Rust guarantee -> C mechanism mapping | `docs/GUARANTEE_SUBSTITUTION_MATRIX.md` | proof artifact checklist + fixture links | missing substitution evidence forbidden | `impl-complete` | `bd-296.6` |
| `U-CODEC-EQUIVALENCE` | JSON/BIN canonical semantic equivalence | `tools/ci/run_conformance.sh`, `schemas/canonical_fixture.schema.json` | codec-equivalence conformance suite | semantic drift between codecs forbidden | `impl-complete` | `bd-2n0.1`, `bd-1md.11`, `bd-j4m.1` |
| `U-PROFILE-PARITY` | Cross-profile canonical digest equivalence | `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md`, `docs/HFT_PROFILE.md`, `docs/AUTOMOTIVE_PROFILE.md`, `tools/ci/run_conformance.sh` | profile parity suite across shared fixtures + vertical fixture lanes (`E2E-VERT-HFT`, `E2E-VERT-AUTO`) | profile-only semantic forks and hidden degraded-mode behavior forbidden | `impl-complete` | `bd-1md.15`, `bd-j4m.7`, `bd-1md.10`, `bd-j4m.1` |
| `U-REPLAY-CONTINUITY` | Deterministic replay + continuity under restart | `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md` + trace/replay docs | replay identity and crash/restart continuity tests (`E2E-CONT-RESTART`) | digest mismatch or duplicated side effects on restart forbidden | `impl-complete` | `bd-1md.15`, `bd-2n0.4`, `bd-j4m.8` |
| `U-PARALLEL-PROFILE` | Optional parallel worker-lane semantics and single-vs-multi-worker digest parity | `docs/DEFERRED_SURFACE_REGISTER.md`, `docs/QUALITY_GATES.md`, `include/asx/runtime/parallel.h` | `parallel-parity`; `test-e2e-parallel`; `formal-litmus`; `formal-codegen`; large-swarm telemetry/admission/locality tests; `parallel-bench-json` | semantic digest drift, unclassified event-order drift, silent drops, unbounded queues, data races, stale-handle mutation, locality routing semantic drift, and weakened cancellation forbidden | `conformance-passed` | `bd-pweu.1`, `bd-pweu.7`, `bd-pweu.9`, `bd-pweu.10`, `bd-pweu.11`, `bd-pweu.12`, `bd-pweu.13`, `bd-pweu.14` |

### 2.1 Wave C Activation Matrix

These rows are post-kernel activation contracts created by `bd-v12u.1`. They
do not change the Phase 1 status count below until the named child beads land
the corresponding specs, tests, and implementation evidence.

| Unit ID | Semantic Unit | Canonical Source Artifacts | Acceptance Test Obligations | Must-Fail / Negative Obligations | Current Status | Primary Bead(s) |
|---|---|---|---|---|---|---|
| `U-NATIVE-ADAPTER-COMMIT` | POSIX/Win32 native hooks preserving deterministic commit authority | `docs/DEFERRED_SURFACE_REGISTER.md` DS-P01; `src/platform/posix/hooks.c`; `src/platform/win32/hooks.c`; `src/runtime/parallel.c` | native adapter parity/e2e tests; `parallel-parity`; `profile-parity` | native event-order drift, silent drops, unsupported live-mode claims, stale-handle mutation | `spec-drafted` | `bd-v12u.2`, `bd-v12u.3`, `bd-v12u.4` |
| `U-STATIC-ARENA-BACKEND` | Static allocator backend preserving dynamic-backend semantics | `docs/DEFERRED_SURFACE_REGISTER.md` DS-S01; allocator vtable/runtime config docs | static-vs-dynamic unit tests; OOM rollback tests; `profile-parity` | partial allocation on failure, post-seal allocation, digest drift, over-capacity live handles | `spec-drafted` | `bd-v12u.5`, `bd-v12u.6` |
| `U-INCIDENT-REPLAY-EVIDENCE` | Operator incident bundle, trace schema, and minimized replay packet | trace/replay/lab docs; `docs/QUALITY_GATES.md` `GATE-INCIDENT-REPLAY-BUNDLE` | schema/unit tests; e2e bundle emission; golden trace examples; minimizer self-tests | evidence artifacts mutating runtime state, unversioned schema drift, minimized failures changing class | `spec-drafted` | `bd-v12u.7`, `bd-v12u.8`, `bd-v12u.13` |
| `U-WAVE-C-NETWORKING` | Minimal networking primitive lifecycle over native reactor readiness | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C01; `src/net/*`; `src/runtime/io_driver.c` | networking primitive spec; socket/pipe e2e; cancellation/backpressure/resource fixtures | kernel semantic drift, unsupported profiles passing live claims, unbounded connection/resource growth | `spec-drafted` | `bd-v12u.9`, `bd-v12u.10` |
| `U-COMBINATOR-ACTOR-HARNESS` | Combinator and actor/supervision semantic harnesses | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C02/DS-D06; `src/core/combinator*.c`; `src/actor/*` | combinator conformance fixtures; actor/supervision restart-policy tests; replay logs | cancellation weakening, outcome lattice drift, leaked obligations, restart/escalation divergence | `spec-drafted` | `bd-v12u.11`, `bd-v12u.12` |
| `U-OVERLOAD-SLO-DEMO` | Large-swarm SLO baselines and user-facing acceptance proof bundle | `docs/QUALITY_GATES.md` `GATE-OVERLOAD-SLO-DEMO`; benchmark/e2e docs | RCH benchmark baselines; release/demo gate with replay/profile/incident artifacts | SLO/admission changing semantics, stale README claims, unsupported platforms hidden as success | `spec-drafted` | `bd-v12u.14`, `bd-v12u.15` |

## 3. Phase 1 Exit Review Gate Checklist

A semantic unit may be marked Phase 1 complete only when:

1. Canonical source artifact is present and references pinned Rust baseline.
2. Unit row is at least `spec-reviewed`.
3. Acceptance obligations are mapped to concrete fixture/test families.
4. Must-fail obligations are explicit and deterministic.
5. Downstream implementation bead links are present.

## 4. Tracking Fields for Future CI Automation

Recommended machine-readable columns to derive later (CSV/JSON export):

- `unit_id`
- `status`
- `source_artifact_paths`
- `acceptance_fixture_ids`
- `forbidden_fixture_ids`
- `impl_bead_ids`
- `conformance_bead_ids`
- `last_reviewed_at`
- `reviewer`

## 5. Phase 1 Spec-Review Gate Sign-Off

**Reviewer:** BlueCat (claude-code/opus-4.6) — independent non-author reviewer
**Date:** 2026-02-27
**Review artifact:** `docs/PHASE1_SPEC_REVIEW_GATE.md`

All kernel-scope semantic units (U-REG-TRANSITIONS through U-GUARANTEE-SUBSTITUTION) have been independently reviewed and approved. Written rulings for 4 ambiguities documented in review artifact (WR-001 through WR-004). Phase 2 kickoff approved.

| Unit Status | Count |
|-------------|-------|
| `impl-complete` | 21 (all kernel units + codec equivalence + profile parity + replay continuity — all beads closed, CI gates pass) |
| `conformance-passed` | 1 (`U-PARALLEL-PROFILE` — logical-worker scheduler, parity/e2e/proof/benchmark/locality gates pass) |
| `spec-reviewed` | 1 (`U-PROVENANCE-MAP` — static documentation artifact, no runtime test needed) |
| **Total** | 23 |

**Status audit 2026-03-02 (WildCondor):** All 19 kernel units advanced from `spec-reviewed` to `impl-complete` based on passing unit tests (59 suites), invariant tests (2 suites/33 scenarios), formal gates (CBMC + algebraic + litmus), e2e lanes (14 families/85+ scenarios), and clean build with `-Werror`. `U-PROVENANCE-MAP` remains `spec-reviewed` as it is a static traceability artifact.

**Rust-vs-C conformance run 2026-03-12 (AmberCrane):** First full conformance run against 66 Rust reference fixtures (6 families: core_budget, core_cancel, core_channel, core_lifecycle, core_timer, smoke). Results: **133/133 pass, 0 fail, 0 semantic deltas, 0 provenance mismatches.** Baseline commit: `a9e737d8`. Run ID: `conformance-20260312T233136Z`. Summary artifact: `tools/ci/artifacts/conformance/conformance-20260312T233136Z-conformance.summary.json`.

## 6. Immediate Follow-On Dependencies

This file directly feeds:

- `bd-296.30` (Phase 1 spec-review gate packet),
- `bd-296.5` (forbidden-behavior catalog and DSL),
- `bd-296.4` (invariant schema generation scope),
- `bd-1md.*` conformance/fuzz fixture planning.
