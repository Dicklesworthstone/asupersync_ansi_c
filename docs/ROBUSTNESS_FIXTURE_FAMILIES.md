# Robustness Fixture Families (Boundary and Failure-Atomic Contracts)

> Bead: `bd-1md.14`  
> Status: Robustness fixture-family capture specification  
> Depends on: `bd-1md.1`, `bd-296.16`, `bd-296.17`, `bd-296.29`, `bd-296.4`  
> Downstream consumers: `bd-1md.17`, `bd-66l.2`, `bd-66l.9`, `bd-1md.19`

## 1. Scope

This document defines robustness fixture families for failure boundaries that are high-risk in C ports:

- allocator/resource exhaustion,
- endian/unaligned binary boundary handling,
- deterministic fault-injection handling for time/entropy/jitter anomalies.

The objective is reproducible failure behavior with explicit first-failure triage pointers.

## 2. Robustness Family Matrix

| family_id | boundary class | semantic sources | invariant/schema linkage | fixture id patterns | parity targets |
|---|---|---|---|---|---|
| `ROBUST-EXHAUSTION` | memory/queue/timer budget ceilings + failure-atomic rollback | `BUDGET-002`, `QUIESCENCE-001`, `FINALIZE-001`, `CHANNEL-004`, `TIMER-003` | `schemas/invariant_schema.json` + `P-REQ-005` + `FB-EXH-*` | `robust-exhaustion-*`, `region-lifecycle-010`, `ch-exhaustion-*`, `tm-exhaustion-*` | `rust_vs_c`, `profile_parity` |
| `ROBUST-ENDIAN-UNALIGNED` | decode/encode safety under endian and alignment stress | portability constraints from `docs/C_PORTABILITY_RULES.md` (`P-REQ-007`, `P-REQ-008`, `P-CI-004`) + timer/channel binary boundary semantics | invariant consistency checks over decode outcomes | `robust-endian-*`, `robust-unaligned-*`, `binary-boundary-*` | `rust_vs_c`, `codec_equivalence`, `profile_parity` |
| `ROBUST-FAULT-INJECTION` | deterministic handling of injected timing/entropy/jitter anomalies | deterministic scheduler/timer semantics + risk controls + forbidden behavior catalog | replay determinism checks and forbidden-path assertions | `robust-fault-time-*`, `robust-fault-entropy-*`, `robust-fault-jitter-*` | `rust_vs_c`, `profile_parity` |

## 3. Required Metadata Per Fixture

Each robustness fixture must include:

- `boundary_class`
- `expected_failure_class` (or explicit successful degraded-mode class)
- `failure_atomic_expected` (`true|false`, must be `true` for exhaustion classes)
- `error_taxonomy_expected` (stable error/status code class)
- `prov_ids` array
- `first_failure_pointer` (scenario + op id + stage)

## 4. Capture and Validation Flow

Capture one family per run via `bd-1md.1` tooling contract:

```bash
asx-fixture-capture capture \
  --scenario-dir fixtures/scenarios/robustness \
  --out-dir fixtures/rust_reference/robustness \
  --baseline-commit <rust_sha> \
  --family ROBUST-EXHAUSTION
```

Validate and deterministic replay check:

```bash
asx-fixture-capture validate --manifest fixtures/rust_reference/robustness/ROBUST-EXHAUSTION.manifest.json
asx-fixture-capture replay-check --manifest fixtures/rust_reference/robustness/ROBUST-EXHAUSTION.manifest.json
```

## 5. E2E Robustness Lane Matrix

| lane_id | family_id | invocation contract | pass condition |
|---|---|---|---|
| `E2E-ROBUST-EXHAUSTION` | `ROBUST-EXHAUSTION` | run exhaustion boundary scenarios across memory/queue/timer classes | deterministic failure code and no partial state mutation |
| `E2E-ROBUST-ENDIAN` | `ROBUST-ENDIAN-UNALIGNED` | run endian/unaligned binary fixtures on required target matrix | consistent decode outcomes and no UB-style divergence |
| `E2E-ROBUST-FAULT` | `ROBUST-FAULT-INJECTION` | run deterministic fault-injection scenarios with fixed seed | stable replay digest and classified failure/degraded-mode outcomes |

## 6. Structured Logging and Artifact Requirements

Every robustness run must emit:

- run-level manifest record (`schemas/robustness_fixture_family_manifest.schema.json`),
- per-scenario structured logs containing:
  - `run_id`, `family_id`, `scenario_id`, `fixture_id`,
  - `boundary_class`, `expected_failure_class`,
  - `error_code_observed`, `failure_atomic_observed`,
  - `first_failure_pointer`,
  - `rust_baseline_commit`, `profile`, `codec`,
  - `semantic_digest`, `delta_classification`,
  - `duration_ms`, `artifact_manifest_path`.

## 7. First-Failure Triage Pointer Contract

For any failing scenario, triage pointer must include:

- `failure_stage` (`capture|validate|replay-check|conformance|e2e`),
- `scenario_id`,
- `op_id` (when operation-specific),
- `expected_vs_observed` summary,
- `recommended_owner_bead` (e.g., `bd-296.16`, `bd-296.17`, `bd-296.29`, `bd-1md.17`).

This pointer is mandatory for CI artifacts consumed by `bd-66l.2`.

## 8. Reconciliation Checklist

Before marking robustness family complete:

1. map each fixture to one or more `prov_id` rows,
2. confirm invariant-schema alignment and portability-rule references,
3. confirm forbidden-behavior links (`FB-EXH-*`, relevant `FB-DTM-*`) where applicable,
4. confirm e2e lane assignment and manifest path,
5. confirm first-failure triage pointer completeness.

## 9. Downstream Handoff

This artifact is authoritative for:

- `bd-1md.17` robustness e2e script family,
- `bd-66l.2` hard quality gate enforcement evidence wiring,
- `bd-66l.9` per-bead evidence linkage checks,
- `bd-1md.19` API misuse fixtureization overlaps with robustness failure classes.
