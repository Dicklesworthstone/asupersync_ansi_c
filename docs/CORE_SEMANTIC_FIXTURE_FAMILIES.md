# Core Semantic Fixture Families (Rust Reference Capture)

> Bead: `bd-1md.13`  
> Status: Core fixture-family capture specification  
> Depends on: `bd-1md.1`, `bd-296.4`, `bd-296.16`, `bd-296.17`, `bd-296.18`  
> Downstream consumers: `bd-1md.2`, `bd-1md.15`, `bd-1md.16`, `bd-66l.9`

## 1. Scope

This document defines the core Rust-reference fixture families for:

- lifecycle,
- outcome,
- budget,
- cancellation,
- channel,
- timer.

Each family includes:

- canonical scenario/fixture naming,
- provenance mapping (`prov_id`),
- invariant-schema linkage,
- e2e invocation lanes,
- structured log + manifest requirements.

## 2. Family Matrix

| family_id | semantic focus | provenance rows | invariant domains | fixture id patterns | parity targets |
|---|---|---|---|---|---|
| `CORE-LIFECYCLE` | region/task/obligation lifecycle legality + quiescence-close paths | `REGION-001..002`, `TASK-001`, `OBLIGATION-001`, `QUIESCENCE-001`, `FINALIZE-001` | `region`, `task`, `obligation` | `region-lifecycle-*`, `task-lifecycle-*`, `obligation-lifecycle-*`, `finalization-*` | `rust_vs_c`, `profile_parity` |
| `CORE-OUTCOME` | severity lattice, join ordering, left-bias rules | `OUTCOME-001..002` | `task` cancellation-strengthen transitions | `outcome-lattice-*`, `outcome-join-*` | `rust_vs_c`, `codec_equivalence`, `profile_parity` |
| `CORE-BUDGET` | budget meet algebra, exhaustion mapping | `BUDGET-001..002` | `cancellation` budget coupling | `budget-meet-*`, `budget-consume-*`, `runtime-pollquota-*` | `rust_vs_c`, `codec_equivalence`, `profile_parity` |
| `CORE-CANCEL` | cancellation phase progression + strengthen behavior | `TASK-001`, `OUTCOME-002`, `BUDGET-002` | `task`, `cancellation` | `cancel-protocol-*`, `task-lifecycle-006`, `task-lifecycle-007` | `rust_vs_c`, `profile_parity` |
| `CORE-CHANNEL` | two-phase MPSC, FIFO waiters, backpressure, disconnect semantics | `CHANNEL-001..007` | `obligation` + channel invariants from extracted semantics | `channel-two-phase-*`, `channel-fifo-*`, `channel-evict-*`, `ch-*` | `rust_vs_c`, `profile_parity` |
| `CORE-TIMER` | timer register/cancel/fire, tie-break, coalescing, overflow/cascade | `TIMER-001..008` | timer/handle invariants from extracted semantics | `timer-*`, `tm-*` | `rust_vs_c`, `codec_equivalence`, `profile_parity` |

## 3. Capture and Validation Flow

Use the `bd-1md.1` tooling contract (`docs/RUST_FIXTURE_CAPTURE_TOOLING.md`) with one family pass per `family_id`.

Capture:

```bash
asx-fixture-capture capture \
  --scenario-dir fixtures/scenarios/core \
  --out-dir fixtures/rust_reference/core \
  --baseline-commit <rust_sha> \
  --family CORE-LIFECYCLE
```

Validate + replay-check:

```bash
asx-fixture-capture validate --manifest fixtures/rust_reference/core/CORE-LIFECYCLE.manifest.json
asx-fixture-capture replay-check --manifest fixtures/rust_reference/core/CORE-LIFECYCLE.manifest.json
```

Repeat for every family in Section 2.

## 4. E2E Invocation Set (Required Lanes)

| lane_id | family_id | invocation contract | pass condition |
|---|---|---|---|
| `E2E-CORE-LIFECYCLE` | `CORE-LIFECYCLE` | run full lifecycle/quiescence fixture pack through capture + conformance runner | all fixtures execute with expected terminal snapshots and zero unclassified deltas |
| `E2E-CORE-OUTCOME-BUDGET` | `CORE-OUTCOME`, `CORE-BUDGET` | run algebraic + exhaustion fixture pack in both JSON and BIN modes | canonical digest equality across codecs and expected error mappings |
| `E2E-CORE-CANCEL` | `CORE-CANCEL` | run cancellation phase and strengthen scenarios with replay-check | stable cancel-phase sequence and deterministic witness outputs |
| `E2E-CORE-CHANNEL` | `CORE-CHANNEL` | run channel reserve/send/abort/fifo/disconnect fixtures under bounded capacity settings | deterministic queue semantics and no leaked permits |
| `E2E-CORE-TIMER` | `CORE-TIMER` | run equal-deadline/cancel/coalescing/cascade fixtures | deterministic ordering and generation-safe cancellation behavior |

## 5. Structured Log Requirements

Each family run must emit structured records with at least:

- `run_id`
- `family_id`
- `scenario_id`
- `fixture_id`
- `prov_ids` (array)
- `rust_baseline_commit`
- `profile`
- `codec`
- `semantic_digest`
- `parity_result`
- `delta_classification`
- `duration_ms`
- `artifact_manifest_path`

Error records must additionally include:

- `failure_stage` (`capture|validate|replay-check|conformance`)
- `error_code`
- `first_failure_pointer` (scenario + op id when applicable).

## 6. Manifest Contract

Per-family capture runs must emit schema-valid manifests using:

- `schemas/core_fixture_family_manifest.schema.json`
- `schemas/fixture_capture_manifest.schema.json`
- `schemas/canonical_fixture.schema.json`

## 7. Reconciliation Checklist (Extraction Consistency)

Before marking a family complete:

1. verify every mapped `prov_id` row has at least one fixture in the family manifest,
2. verify invariant-domain coverage aligns with `schemas/invariant_schema.json`,
3. verify forbidden behaviors referenced in `docs/FORBIDDEN_BEHAVIOR_CATALOG.md` are represented where applicable,
4. verify e2e lane ID and artifact paths are present in the manifest,
5. verify structured logs contain required fields.

## 8. Downstream Handoff

This document is the contract input for:

- `bd-1md.2` conformance runner ingestion,
- `bd-1md.15` vertical/continuity family layering,
- `bd-1md.16` e2e script family packaging,
- `bd-66l.9` evidence-link enforcement.
