# Vertical and Continuity Fixture Families (Rust Reference Capture)

> Bead: `bd-1md.15`  
> Status: Vertical/continuity fixture-family capture specification  
> Depends on: `bd-1md.1`, `bd-1md.13`, `bd-296.15`, `bd-296.16`, `bd-296.17`, `bd-296.18`, `bd-296.5`, `bd-296.4`  
> Downstream consumers: `bd-1md.8`, `bd-1md.9`, `bd-1md.18`, `bd-2n0.5`, `bd-j4m.1`, `bd-66l.2`

## 1. Scope

This document defines canonical fixture-family contracts for:

- HFT microburst and deterministic overload behavior,
- automotive watchdog/deadline/degraded-mode behavior,
- crash/restart replay continuity behavior.

This is a layering contract over core families in `docs/CORE_SEMANTIC_FIXTURE_FAMILIES.md`.
Vertical and continuity families are not semantic forks; they are profile or recovery stress lenses over the same lifecycle/cancel/budget/channel/timer semantics.

## 2. Family Matrix

| family_id | semantic focus | core family linkage | target profiles | fixture id patterns | parity targets |
|---|---|---|---|---|---|
| `VERT-HFT-MICROBURST` | burst overload/backpressure with deterministic tail behavior | `CORE-CANCEL`, `CORE-BUDGET`, `CORE-CHANNEL`, `CORE-TIMER` | `ASX_PROFILE_CORE`, `ASX_PROFILE_HFT` | `vert-hft-*`, `hft-microburst-*` | `rust_vs_c`, `profile_parity` |
| `VERT-AUTOMOTIVE-WATCHDOG` | watchdog/deadline checkpoints and deterministic degraded mode | `CORE-LIFECYCLE`, `CORE-CANCEL`, `CORE-BUDGET`, `CORE-TIMER` | `ASX_PROFILE_CORE`, `ASX_PROFILE_AUTOMOTIVE` | `vert-auto-*`, `auto-watchdog-*` | `rust_vs_c`, `profile_parity` |
| `CONT-CRASH-RESTART` | persisted trace/snapshot restart continuity and digest identity | `CORE-LIFECYCLE`, `CORE-OUTCOME`, `CORE-BUDGET`, `CORE-CANCEL`, `CORE-CHANNEL`, `CORE-TIMER` | `ASX_PROFILE_CORE`, `ASX_PROFILE_FREESTANDING`, `ASX_PROFILE_EMBEDDED_ROUTER`, `ASX_PROFILE_HFT`, `ASX_PROFILE_AUTOMOTIVE` | `cont-restart-*`, `continuity-*` | `rust_vs_c`, `codec_equivalence`, `profile_parity` |

## 3. Canonical Scenario Set

| scenario_id | family_id | core linkage | required `prov_ids` | expected outcome class | digest expectation |
|---|---|---|---|---|---|
| `hft-microburst-overload-001` | `VERT-HFT-MICROBURST` | cancel + budget exhaustion path | `BUDGET-002`, `SCHEDULER-001`, `SCHEDULER-004` | classified overload + deterministic backpressure | stable digest across repeated fixed-seed runs |
| `hft-microburst-fairness-002` | `VERT-HFT-MICROBURST` | scheduler fairness + channel FIFO | `CHANNEL-002`, `SCHEDULER-005` | no starvation and deterministic witness hash | same digest across core vs hft for shared semantic path |
| `hft-overload-recovery-003` | `VERT-HFT-MICROBURST` | cancel-strengthen and recovery | `OUTCOME-002`, `TASK-001`, `FINALIZE-001` | deterministic transition from overload to stable processing | replay digest stable on rerun |
| `auto-watchdog-checkpoint-001` | `VERT-AUTOMOTIVE-WATCHDOG` | lifecycle checkpoint legality | `REGION-001`, `TASK-001`, `QUIESCENCE-001` | checkpoint success under bounded budget | digest parity core vs automotive |
| `auto-degraded-transition-002` | `VERT-AUTOMOTIVE-WATCHDOG` | deterministic degraded-mode transition | `BUDGET-002`, `FINALIZE-001`, `SCHEDULER-001` | degraded-mode transition reason and phase captured | repeated-run digest equality for same seed |
| `auto-deadline-miss-003` | `VERT-AUTOMOTIVE-WATCHDOG` | deadline miss classification | `BUDGET-002`, `OUTCOME-001` | explicit deadline/watchdog error class without partial mutation | parity digest preserved where scenario is shared |
| `continuity-crash-midflight-001` | `CONT-CRASH-RESTART` | restart from persisted trace + snapshot | `REGION-001`, `TASK-001`, `CHANNEL-003`, `TIMER-002` | restart resumes legally without duplicated effects | post-restart digest equals golden digest |
| `continuity-checkpoint-rollback-002` | `CONT-CRASH-RESTART` | bounded rollback and replay | `FINALIZE-001`, `QUIESCENCE-001`, `OUTCOME-002` | rollback classification deterministic | digest identity across json/bin for same scenario |
| `continuity-replay-identity-003` | `CONT-CRASH-RESTART` | replay identity under persisted artifacts | `SCHEDULER-005`, `TIMER-001`, `CHANNEL-001` | deterministic event hash continuity across restart | same canonical digest across all parity profiles |

## 4. E2E Lane Contract

| lane_id | family coverage | invocation contract | pass condition |
|---|---|---|---|
| `E2E-VERT-HFT` | `VERT-HFT-MICROBURST` | run microburst scenarios through capture -> validate -> replay-check -> profile-parity | deterministic overload/backpressure outcomes and digest parity (`CORE` vs `HFT`) |
| `E2E-VERT-AUTO` | `VERT-AUTOMOTIVE-WATCHDOG` | run watchdog/deadline scenarios through capture -> validate -> conformance -> profile-parity | deterministic degraded-mode evidence and digest parity (`CORE` vs `AUTOMOTIVE`) |
| `E2E-CONT-RESTART` | `CONT-CRASH-RESTART` | run crash/restart scenarios with persisted trace/snapshot artifacts and replay verification | restart continuity digest identity and zero unclassified deltas |

### Required Script and Artifact Outputs

Each lane must emit:

- run-level manifest (schema-valid),
- scenario-level canonical fixtures,
- parity report (`rust_vs_c`, plus `codec_equivalence` and/or `profile_parity` as required),
- first-failure pointer bundle for each failing scenario.

## 5. Structured Log and Manifest Requirements

For every scenario in this document, logs must include:

- `run_id`, `family_id`, `lane_id`, `scenario_id`, `fixture_id`,
- `prov_ids` and `core_family_links`,
- `profile_set` and `codec_set`,
- `semantic_digest`,
- `parity_result` and `delta_classification`,
- `restart_phase` (`none|pre-crash|recovered`) for continuity families,
- `artifact_manifest_path`,
- `first_failure_pointer` when failing.

Family manifests must validate against:

- `schemas/vertical_continuity_fixture_family_manifest.schema.json`,
- `schemas/fixture_capture_manifest.schema.json`,
- `schemas/canonical_fixture.schema.json`.

## 6. Canonicalization and Reproducibility Rules

1. Scenario ordering is lexicographic by `scenario_id`.
2. Fixed seed policy is mandatory for all vertical lanes.
3. Restart scenarios must persist and re-load artifacts by stable path conventions.
4. Re-running the same scenario with the same `{seed, profile, codec}` must reproduce the same digest.
5. Shared-fixture parity comparisons must use identical core semantic input envelopes.

## 7. Gate and Release Mapping

This family set is a required input for:

- `make conformance` (`rust_vs_c`),
- `make profile-parity` (cross-profile digest equivalence),
- `make codec-equivalence` (continuity family where JSON/BIN applies),
- release evidence bundles for HFT tail/jitter and automotive deadline/watchdog lanes.

## 8. Closure Checklist for `bd-1md.15`

Before closing:

1. all families in Section 2 have at least one captured canonical scenario,
2. scenario rows include explicit `prov_ids` and core linkage,
3. e2e lane manifests exist and validate,
4. parity report includes expected digest/outcome assertions for every scenario,
5. traceability and parity matrices are updated with this artifact.
