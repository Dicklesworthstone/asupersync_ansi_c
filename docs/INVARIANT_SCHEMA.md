# Machine-Readable Invariant Schema and Generation Pipeline

> **Bead:** bd-296.4
> **Status:** Canonical spec artifact
> **Rust baseline commit:** `38c152405bd03e2bd9eecf178bfbbe9472fed861`
> **Last updated:** 2026-02-27 by MossySeal/GrayKite
> **Schema file:** `schemas/invariant_schema.json`

---

## 1. Overview

The invariant schema (`schemas/invariant_schema.json`) is a single machine-readable JSON file that encodes the complete state machine, transition legality, forbidden behavior, and postcondition contracts for all asx domains. It serves three purposes:

1. **Test generation source:** Automated tools consume the schema to generate transition legality tests and forbidden-path tests without manual authoring.
2. **CI drift gate:** Hash of the schema is compared against a committed baseline; schema changes that don't pass the update protocol block merges.
3. **Cross-domain consistency:** All domains (region, task, obligation, cancellation, outcome, budget, channel, timer, scheduler, quiescence) are defined in one file, enabling cross-domain invariant checks.

## 2. Schema Structure

### 2.1 Top-Level Fields

| Field | Type | Purpose |
|-------|------|---------|
| `version` | string | Semver version of the schema itself |
| `rust_baseline_commit` | string | Rust commit this schema was extracted from |
| `provenance` | object | Links to extraction beads and source documents |
| `domains` | object | Per-domain state machines, transitions, invariants |
| `schema_meta` | object | Summary counts and evolution policy |

### 2.2 Per-Domain Structure

Each domain entry in `domains` contains:

| Field | Type | Purpose |
|-------|------|---------|
| `states` | array | State definitions with `id`, `enum_value`, `ordinal`, `terminal` |
| `legal_transitions` | array | Legal transitions with `id`, `from`, `to`, `trigger`, `fixture_ids`, `postconditions` |
| `forbidden_transitions` | array | Forbidden transitions with `from`, `to`, `error`, `reason` |
| `admission_gates` | array | (optional) Operation-to-state legality table |
| `invariants` | array | Named invariants with `id`, `rule`, `category` |

### 2.3 Domain Coverage

| Domain | States | Legal Trans. | Forbidden Trans. | Invariants |
|--------|--------|-------------|-------------------|------------|
| Region | 5 | 7 | 15 | 5 |
| Task | 6 | 13 | 23 | 4 |
| Obligation | 4 | 3 | 13 | 4 |
| Cancellation | 4 phases + 11 kinds | N/A | N/A | 3 |
| Outcome | 4 variants | N/A | N/A | 3 |
| Budget | 4 components | N/A | N/A | 3 |
| Channel | 4 implicit | N/A | N/A | 10 |
| Timer | 4 levels | N/A | N/A | 8 |
| Scheduler | 3 lanes | N/A | N/A | 8 |
| Quiescence | 5 conditions | N/A | N/A | 3 |
| **Total** | **23 states** | **23 legal** | **51 forbidden** | **51** |

## 3. Test Generation Pipeline

### 3.1 Generator Input

The generator consumes `schemas/invariant_schema.json` and produces:

1. **Transition legality tests:** For each `legal_transitions` entry, generate a test that:
   - Creates the domain entity in the `from` state
   - Invokes the `trigger` operation
   - Asserts the entity transitions to the `to` state
   - Verifies all `postconditions`

2. **Forbidden-path tests:** For each `forbidden_transitions` entry, generate a test that:
   - Creates the domain entity in the `from` state
   - Invokes the operation that would transition to `to`
   - Asserts the specific `error` code is returned
   - Asserts state did NOT change

3. **Admission gate tests:** For each `admission_gates` entry, generate tests for:
   - Each allowed state: operation succeeds
   - Each disallowed state: operation returns the specific error

4. **Invariant assertion tests:** For each `invariants` entry, generate a test that:
   - Constructs a scenario exercising the invariant
   - Verifies the invariant holds under normal operation
   - Verifies violation is detected under abnormal operation

### 3.2 Generator Output Structure

```
tests/generated/
  invariant_region_legality.c      # Region legal transition tests
  invariant_region_forbidden.c     # Region forbidden transition tests
  invariant_region_gates.c         # Region admission gate tests
  invariant_task_legality.c        # Task legal transition tests
  invariant_task_forbidden.c       # Task forbidden transition tests
  invariant_obligation_legality.c  # Obligation legal transition tests
  invariant_obligation_forbidden.c # Obligation forbidden transition tests
  invariant_cross_domain.c         # Cross-domain invariant tests
  generated_manifest.json          # Manifest linking schema version to generated files
```

### 3.3 Generator Invocation

```bash
# Generate all tests from schema
make generate-invariant-tests

# Verify generated tests match schema version
make verify-invariant-schema

# Run generated tests
make test-invariants
```

### 3.4 Generation Rules

1. **Deterministic output:** Same schema -> same generated tests (byte-identical)
2. **No hand-editing:** Generated files are overwritten on each generation; manual changes are lost
3. **Manifest tracking:** `generated_manifest.json` records schema version, generation timestamp, and file checksums
4. **Incremental safety:** If schema version changes, all generated tests must be regenerated

## 4. Drift Gate Strategy

### 4.1 CI Integration

The drift gate runs in CI on every PR and push:

```yaml
# Pseudo-workflow
steps:
  - name: Verify schema hash
    run: |
      EXPECTED=$(cat .schema-baseline-hash)
      ACTUAL=$(sha256sum schemas/invariant_schema.json | cut -d' ' -f1)
      if [ "$EXPECTED" != "$ACTUAL" ]; then
        echo "Schema changed. Checking update protocol..."
        # Verify generated tests are also updated
        make generate-invariant-tests
        git diff --exit-code tests/generated/
      fi

  - name: Run generated invariant tests
    run: make test-invariants
```

### 4.2 Drift Detection

| Drift Type | Detection | Action |
|-----------|-----------|--------|
| Schema changed, tests not regenerated | `git diff` on generated files | Block merge |
| Schema changed, tests regenerated and pass | Schema hash updated | Allow merge |
| Schema unchanged, tests fail | Invariant implementation bug | Block merge |
| New domain added to schema | New generated test files appear | Require review |

### 4.3 Schema Hash Baseline

The committed file `.schema-baseline-hash` contains the SHA-256 of the current canonical schema. Any schema change must also update this file.

## 5. Schema Evolution Policy

### 5.1 Versioning

- **Patch:** Fixing descriptions, typos, or metadata that don't affect generated tests
- **Minor:** Adding new domains, states, transitions, or invariants (additive)
- **Major:** Removing or renaming existing states/transitions/invariants (breaking)

### 5.2 Update Protocol

1. Update source extraction documents first (never diverge schema from docs)
2. Regenerate schema from extraction artifacts
3. Run invariant test generator against new schema
4. Verify all generated tests pass against current implementation
5. Update `.schema-baseline-hash`
6. Commit schema + generated tests + hash atomically

### 5.3 Compatibility Rules

- Generated test IDs are stable across minor versions (e.g., `test_region_R1_legality`)
- Test file names are stable across minor versions
- Major version changes may rename/remove test IDs (documented in changelog)

## 6. Cross-Domain Invariant Checks

The schema enables cross-domain invariant verification:

| Check | Domains | Rule |
|-------|---------|------|
| Task cancellation requires valid region | Task + Region | Task cancel only legal in region states Open/Closing/Draining |
| Obligation resolution requires valid task | Obligation + Task | Obligation commit/abort only while task is Running/CancelRequested/Cancelling |
| Quiescence covers all domains | All | All five quiescence conditions checked simultaneously |
| Outcome severity preserved through cancel | Outcome + Cancellation | Cancelled outcome severity always >= Err |
| Budget exhaustion triggers cancellation | Budget + Cancellation | Exhausted budget -> specific CancelKind |
| Channel obligation tracking | Channel + Obligation | TrackedPermit links to ObligationToken |

## 7. Fixture ID Mapping

Each legal transition entry includes `fixture_ids` that link to the provenance map (`docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`). This enables bidirectional traceability:

- Schema -> Fixture ID -> Parity row -> Rust test
- Rust test -> Parity row -> Fixture ID -> Schema transition
