# Rust Reference Fixture Capture Tooling Contract

> Bead: `bd-1md.1`  
> Status: Tooling skeleton and output contract for deterministic Rust fixture capture  
> Depends on: `bd-296.5`, `bd-296.12`, `bd-296.3`  
> Downstream consumers: `bd-1md.13`, `bd-1md.14`, `bd-1md.15`, `bd-1md.2`, `bd-1md.10`, `bd-66l.9`

## 1. Purpose

Define the canonical fixture-capture program skeleton used to generate Rust-reference fixtures for conformance and parity testing in the ANSI C port workflow.

This document specifies:

- capture CLI interfaces,
- deterministic capture execution rules,
- canonical schema writer obligations,
- provenance metadata requirements,
- output contracts for downstream fixture-family tasks.

## 2. Tooling Components (Program Skeleton)

The capture toolchain is decomposed into four deterministic stages:

1. **Scenario intake**
   - reads scenario DSL inputs (`docs/SCENARIO_DSL.md`),
   - validates schema + deterministic ordering constraints.
2. **Rust execution harness**
   - runs scenario against pinned Rust baseline runtime,
   - records normalized semantic events and final snapshot.
3. **Canonical schema writer**
   - emits fixture records under canonical JSON rules,
   - emits a run manifest with provenance metadata.
4. **Reproducibility verifier**
   - reruns selected scenarios with same seed/input,
   - fails if semantic digest differs.

## 3. CLI Contract

All commands are deterministic for fixed `{scenario, seed, baseline}`.

### 3.1 `capture`

Capture fixtures from one or more scenario files.

```bash
asx-fixture-capture capture \
  --scenario-dir fixtures/scenarios \
  --out-dir fixtures/rust_reference \
  --profile ASX_PROFILE_CORE \
  --codec json \
  --baseline-commit <rust_sha> \
  --seed-mode explicit
```

Required flags:

- `--scenario-dir` path to DSL scenarios
- `--out-dir` capture output root
- `--baseline-commit` pinned Rust baseline commit

Optional flags:

- `--profile` default `ASX_PROFILE_CORE`
- `--codec` default `json`
- `--seed-mode` one of `explicit|derive_from_scenario`
- `--run-id` explicit capture run id

### 3.2 `validate`

Validate previously emitted fixtures and manifest against schema.

```bash
asx-fixture-capture validate \
  --manifest fixtures/rust_reference/manifest.json \
  --fixtures-root fixtures/rust_reference
```

### 3.3 `replay-check`

Rerun captured scenarios to prove deterministic reproducibility.

```bash
asx-fixture-capture replay-check \
  --manifest fixtures/rust_reference/manifest.json \
  --sample-rate 1.0
```

### 3.4 `emit-provenance`

Emit only provenance bundle used by capture run.

```bash
asx-fixture-capture emit-provenance \
  --baseline-commit <rust_sha> \
  --out fixtures/rust_reference/provenance.json
```

## 4. Deterministic Capture Rules

1. Scenario iteration order is lexicographic by `scenario_id`.
2. Effective seed for each scenario is:
   - explicit `seed` from scenario when present, else
   - deterministic derivation from `sha256(scenario_id)`.
3. Event stream normalization must remove non-semantic ordering noise.
4. Canonical JSON output must use sorted object keys.
5. Re-running `capture` with same baseline and inputs must yield identical `semantic_digest`.

## 5. Output Artifacts

Capture run output root:

```text
fixtures/rust_reference/
  manifest.json
  provenance.json
  scenarios/
    <scenario_id>.json
  reports/
    reproducibility.json
    validation.json
```

Artifact requirements:

- `manifest.json`: capture run index + global provenance (`schemas/fixture_capture_manifest.schema.json`).
- `scenarios/<id>.json`: canonical fixture records (`schemas/canonical_fixture.schema.json`).
- `provenance.json`: baseline/toolchain lock snapshot.
- `reports/reproducibility.json`: deterministic rerun verification.

## 6. Provenance Metadata Requirements

Every capture run must include:

- `rust_baseline_commit`,
- `rust_toolchain_commit_hash`,
- `rust_toolchain_release`,
- `rust_toolchain_host`,
- `cargo_lock_sha256`,
- `cargo_lock_bytes`,
- `fixture_schema_version`,
- `scenario_dsl_version`,
- `capture_tool_version`.

If any field is missing, capture output is invalid and must fail validation.

## 7. Schema Contracts

Machine-readable schemas:

- `schemas/fixture_capture_manifest.schema.json`
- `schemas/canonical_fixture.schema.json`
- `schemas/core_fixture_family_manifest.schema.json` (family-level manifests from `bd-1md.13`)
- `schemas/robustness_fixture_family_manifest.schema.json` (family-level manifests from `bd-1md.14`)
- `schemas/vertical_continuity_fixture_family_manifest.schema.json` (family-level manifests from `bd-1md.15`)

Validation expectations:

- all manifests validate against capture manifest schema,
- all scenario fixture files validate against canonical fixture schema,
- schema version mismatch is a hard fail.

## 8. Downstream Integration Contract

### `bd-1md.13` / `bd-1md.14` / `bd-1md.15` fixture-family capture

Must consume:

- this CLI contract,
- manifest schema,
- canonical fixture schema.

Must produce:

- family-specific scenario packs,
- schema-valid fixture files,
- run manifests with complete provenance metadata.

### `bd-1md.2` conformance runner

Must consume:

- canonical fixture files,
- capture manifests,
- provenance fields for parity report population.

## 9. Failure Modes (Hard Fail)

Capture run fails when:

1. scenario schema invalid,
2. provenance fields incomplete,
3. manifest or fixture fails schema validation,
4. deterministic replay-check digest mismatch,
5. unknown scenario DSL version or fixture schema version.

## 10. Acceptance Checklist

This bead is satisfied when:

1. capture CLI surface is specified (`capture`, `validate`, `replay-check`, `emit-provenance`),
2. deterministic pipeline and seed rules are explicit,
3. canonical schema writer contract is machine-checkable,
4. provenance metadata requirements are complete,
5. downstream fixture-family beads can execute using this contract without re-opening the giant plan.
