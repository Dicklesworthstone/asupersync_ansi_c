# Shared End-to-End Harness, Artifact Layout, and Golden-Log Assertion Strategy

> Reusable harness model, artifact conventions, and golden-log assertion
> strategy for all `bd-1eqo.*` end-to-end and smoke validation.
>
> Bead: bd-1eqo.2.6
> SPDX-License-Identifier: MIT

---

## 1. Purpose

Implementation beads must not invent ad-hoc e2e infrastructure. This document
defines the shared conventions that every e2e script, scenario pack, canonical
example, and smoke run must follow. It is the mechanism layer beneath the
scenario-pack contract (`docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md`).

### Normative Inputs

| Document | Role |
|----------|------|
| `docs/E2E_SCENARIO_PACKS_AND_REPORT_CONTRACT.md` | What packs exist and what they must prove |
| `docs/SUBSYSTEM_EVIDENCE_MATRIX.md` | Which evidence lanes each track must deliver |
| `docs/TEST_LOG_SCHEMA.md` | Structured log record format |
| `docs/PROFILE_RESOURCE_CLASS_CAPABILITY_MATRIX.md` | Profile/class parameters |
| `docs/RUST_PROVENANCE_CROSSWALK_AND_FIXTURE_WORKFLOW.md` | Fixture authority rules |
| `tests/e2e/harness.sh` | Reference implementation of this design |

---

## 2. Harness Architecture

### 2.1 Shared Harness Contract (`tests/e2e/harness.sh`)

Every e2e script sources `harness.sh` and follows a three-phase protocol:

```bash
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/../harness.sh"   # or relative path to harness

e2e_init "family-id" "LANE-ID"         # Phase 1: init
# ... test logic, calling e2e_scenario for each result ...
e2e_finish                              # Phase 3: summary + exit
```

The harness provides:

| Function | Purpose |
|----------|---------|
| `e2e_init(family, lane)` | Create artifact dirs, open report file, set counters |
| `e2e_scenario(id, diag, status, [digest])` | Record one scenario result (pass/fail/skip) |
| `e2e_build(source, output, [extra_flags])` | Compile C test against libasx with profile/codec flags |
| `e2e_run_binary(binary, stderr_file, prefix)` | Execute binary, parse `SCENARIO` lines from stdout |
| `e2e_should_run(id)` | Check scenario-pack filter |
| `e2e_rerun_command(id)` | Generate copy-pasteable rerun command |
| `e2e_finish()` | Emit summary JSON, JSONL summary record, human output, exit code |

### 2.2 Environment Contract

All e2e scripts accept these environment variables:

| Variable | Default | Purpose |
|----------|---------|---------|
| `ASX_E2E_SEED` | 42 | Deterministic seed for reproducible runs |
| `ASX_E2E_PROFILE` | CORE | Platform profile under test |
| `ASX_E2E_CODEC` | json | Codec variant |
| `ASX_E2E_SCENARIO_PACK` | all | Pack/scenario filter (prefix match) |
| `ASX_E2E_RESOURCE_CLASS` | R3 | Resource class for capacity-aware tests |
| `ASX_E2E_RUN_ID` | auto | Override run ID |
| `ASX_E2E_ARTIFACT_DIR` | auto | Override artifact output directory |
| `ASX_E2E_LOG_DIR` | `build/test-logs` | Override JSONL log directory |
| `ASX_E2E_STRICT` | 0 | Treat skipped scenarios as failures |
| `ASX_E2E_VERBOSE` | 0 | Verbose human output |
| `ASX_E2E_POLICY_ID` | (none) | Optional policy tag for governance tests |

### 2.3 Deterministic Seed Plumbing

Every e2e script receives `E2E_SEED` from the environment. Scripts that build
and run C binaries must pass the seed through to the binary:

```bash
e2e_build "test_foo.c" "$BIN" "-DASX_DETERMINISTIC=1"
ASX_SEED=$E2E_SEED "$BIN"
```

The seed flows: CI/user → environment → harness → binary → runtime PRNG.
This ensures any failure is replayable with the same seed.

---

## 3. Artifact Directory Layout

### 3.1 Directory Structure

```
build/
├── e2e-artifacts/
│   └── e2e-YYYYMMDDTHHMMSSZ/    # per-run artifact directory
│       ├── core-lifecycle.summary.json
│       ├── timer-determinism.summary.json
│       ├── *.stderr.log           # captured stderr from binaries
│       ├── *.golden.jsonl         # golden log snapshots (when updated)
│       └── *.replay.hint          # replay seed/config for failed scenarios
│
├── test-logs/
│   ├── e2e-core-lifecycle.jsonl   # per-family JSONL report
│   ├── e2e-timer-determinism.jsonl
│   ├── unit-core-budget.jsonl     # unit test logs (same schema)
│   └── fuzz-differential.jsonl
│
└── lib/
    └── libasx.a                   # built library
```

### 3.2 Naming Conventions

| Artifact | Pattern | Example |
|----------|---------|---------|
| JSONL report | `e2e-{family}.jsonl` | `e2e-core-lifecycle.jsonl` |
| Summary manifest | `{family}.summary.json` | `core-lifecycle.summary.json` |
| Build log | `{binary}.build.log` | `e2e_timer_smoke.build.log` |
| Stderr capture | `{binary}.stderr.log` | `e2e_timer_smoke.stderr.log` |
| Golden log | `{family}.golden.jsonl` | `core-lifecycle.golden.jsonl` |
| Replay hint | `{scenario}.replay.hint` | `timer-fire-order.replay.hint` |

### 3.3 Artifact Retention Rules

| Condition | Retention |
|-----------|-----------|
| All scenarios pass | Summary JSON retained; JSONL report retained; stderr deleted |
| Any scenario fails | All artifacts retained; replay hints generated |
| CI nightly | All artifacts uploaded as job artifacts |
| Local development | `build/e2e-artifacts/` is gitignored; user cleans manually |

---

## 4. Structured Logging

### 4.1 JSONL Record Schema

All test layers (unit, law, lab, conformance, e2e, fuzz, bench) emit
records conforming to `docs/TEST_LOG_SCHEMA.md`. The harness automatically
emits e2e-layer records via `_e2e_emit_record`.

Required fields per record:

```json
{
  "ts": "2026-03-12T20:00:00Z",
  "run_id": "e2e-20260312T200000Z",
  "layer": "e2e",
  "subsystem": "core-lifecycle",
  "suite": "E2E-CORE-LIFECYCLE",
  "test": "region_open_close",
  "status": "pass",
  "event_index": 1,
  "profile": "CORE",
  "codec": "json",
  "seed": 42
}
```

Optional fields: `digest`, `duration_ns`, `error`, `metrics`, `scenario_id`,
`policy_id`.

### 4.2 Summary Manifest Schema

Each `e2e_finish()` call produces a `.summary.json`:

```json
{
  "run_id": "e2e-20260312T200000Z",
  "timestamp": "20260312T200000Z",
  "git_rev": "6a1287c12345",
  "compiler": "gcc (gcc (Ubuntu 15.1.0-2ubuntu1) 15.1.0)",
  "target": "x86_64-linux-gnu",
  "family_id": "core-lifecycle",
  "lane_id": "E2E-CORE-LIFECYCLE",
  "profile": "CORE",
  "codec": "json",
  "seed": 42,
  "resource_class": "R3",
  "scenario_pack": "all",
  "total": 12,
  "pass": 12,
  "fail": 0,
  "skip": 0,
  "status": "pass",
  "report_file": "build/test-logs/e2e-core-lifecycle.jsonl",
  "artifact_dir": "build/e2e-artifacts/e2e-20260312T200000Z"
}
```

On failure, a `first_failure` object is included with `scenario`, `diagnostic`,
and `rerun` fields.

---

## 5. Golden-Log Assertion Strategy

### 5.1 What Golden Logs Assert

Golden logs are reference JSONL files that capture the **expected sequence of
structured events** for a deterministic scenario. They assert:

1. **Event count** — the same number of scenario results
2. **Event order** — scenarios execute and complete in the same order
3. **Status values** — each scenario produces the same pass/fail/skip
4. **Semantic digests** — when present, digest values match exactly

Golden logs do NOT assert:
- Timestamps (always differ between runs)
- Run IDs (auto-generated)
- Absolute file paths (machine-specific)
- Compiler version strings (may change)

### 5.2 Golden Log Location

```
tests/e2e/golden/
├── core-lifecycle.golden.jsonl
├── timer-determinism.golden.jsonl
├── channel-lifecycle.golden.jsonl
└── ...
```

Golden logs are checked into git and reviewed as part of PRs.

### 5.3 Golden Log Update Protocol

```bash
# 1. Run the e2e suite and capture output
ASX_E2E_SEED=42 ASX_E2E_PROFILE=CORE tests/e2e/test_core_lifecycle.sh

# 2. Review the JSONL report manually
cat build/test-logs/e2e-core-lifecycle.jsonl | jq .

# 3. Strip volatile fields and install as golden
scripts/update-golden.sh core-lifecycle

# 4. Commit the updated golden log with explanation
```

### 5.4 Golden Log Comparison

The comparison tool (`scripts/compare-golden.sh` or a C utility) strips
volatile fields before comparison:

```
Fields stripped: ts, run_id, report_file, artifact_dir, compiler, git_rev, target
Fields compared: layer, subsystem, suite, test, status, event_index, profile,
                 codec, seed, digest, error.message
```

Comparison algorithm:
1. Parse both files line-by-line as JSON objects
2. Strip volatile fields from both
3. Compare remaining fields with exact string equality
4. Report first divergence with line number, field name, expected/actual values

### 5.5 Failure and Recovery Scenarios

Golden logs must cover both:

- **Happy-path scenarios**: normal operation, all assertions pass
- **Failure-path scenarios**: expected errors like `ASX_E_RESOURCE_EXHAUSTED`,
  `ASX_E_INVALID_TRANSITION`, etc. These scenarios have `status: "pass"` in the
  golden log because the *test* passes (it correctly observes the expected error).

Example failure-path golden record:
```json
{"layer":"e2e","test":"region_close_already_closed","status":"pass",
 "subsystem":"core-lifecycle","profile":"CORE","seed":42,"event_index":5}
```

---

## 6. Replay Hint Emission

### 6.1 When to Emit

Replay hints are emitted when:
- A scenario fails unexpectedly
- A golden-log comparison diverges
- A determinism check fails (different digest for same seed)

### 6.2 Replay Hint Format

```
# Replay hint for failed scenario: timer-fire-order
# Generated: 2026-03-12T20:00:00Z
ASX_E2E_SEED=42
ASX_E2E_PROFILE=CORE
ASX_E2E_CODEC=json
ASX_E2E_RESOURCE_CLASS=R3
ASX_E2E_SCENARIO_PACK=timer-fire-order
ASX_E2E_VERBOSE=1

# Rerun command:
ASX_E2E_SEED=42 ASX_E2E_PROFILE=CORE ASX_E2E_CODEC=json ASX_E2E_RESOURCE_CLASS=R3 ASX_E2E_SCENARIO_PACK=timer-fire-order ASX_E2E_VERBOSE=1 tests/e2e/test_timer_determinism.sh
```

### 6.3 Replay Determinism Guarantee

Given the same seed, profile, codec, resource class, and scenario pack,
the harness MUST produce byte-identical JSONL output (after stripping volatile
fields). If it does not, this is a determinism bug and should be reported as
a `determinism_failure` in the JSONL report.

---

## 7. CI Integration

### 7.1 Makefile Targets

```makefile
# Run all e2e scenario packs for the current profile
make e2e

# Run a specific scenario pack
make e2e SCENARIO_PACK=core-lifecycle

# Run with a specific profile
make e2e PROFILE=HFT

# Compare against golden logs
make e2e-golden-check

# Update golden logs (after manual review)
make e2e-golden-update

# Run cross-profile parity check
make profile-parity
```

### 7.2 CI Pipeline Flow

```
build lib → run unit tests → run law tests → run e2e packs
                                                  ↓
                                         compare golden logs
                                                  ↓
                                         upload artifacts (on failure)
                                                  ↓
                                         cross-profile parity (nightly)
```

### 7.3 Exit Code Contract

| Exit Code | Meaning |
|-----------|---------|
| 0 | All scenarios pass, golden logs match |
| 1 | At least one scenario failed |
| 2 | Golden log divergence detected |
| 3 | Build failure (libasx.a or test binary) |

---

## 8. Canonical Examples and Smoke Scripts

### 8.1 Examples Reuse Harness

Canonical examples (planned by bd-1eqo.9.3) should be executable as
e2e scenarios. Each example should:

1. Be compilable with `e2e_build`
2. Emit `SCENARIO` lines on stdout for harness parsing
3. Return 0 on success, nonzero on failure
4. Accept `ASX_SEED` for deterministic operation

### 8.2 Smoke Script Contract

Smoke scripts are lightweight e2e scripts that test a single subsystem
quickly (< 5 seconds). They follow the same harness protocol but with
fewer scenarios. Used for:

- Pre-commit validation
- Quick regression checks during development
- `make smoke` target

---

## 9. Downstream Bead Obligations

| Bead | What This Design Provides |
|------|--------------------------|
| bd-1eqo.8.1 (lab runtime) | Lab scenarios use same harness, emit same JSONL, support golden-log replay |
| bd-1eqo.9.3 (examples) | Examples are e2e scenarios; reuse `e2e_build`, `e2e_run_binary`, golden-log comparison |
| bd-1eqo.15.4 (wasm ABI) | ABI compatibility tests emit `SCENARIO` lines, covered by golden logs |
| All `bd-1eqo.*` implementation beads | Must not invent custom e2e infrastructure; source `harness.sh` and follow this contract |

---

## 10. Implementation Status

| Component | Status | Location |
|-----------|--------|----------|
| `harness.sh` | Implemented | `tests/e2e/harness.sh` |
| Environment contract | Implemented | harness.sh lines 40-48 |
| Artifact directory layout | Implemented | harness.sh lines 96-101 |
| JSONL emission | Implemented | harness.sh `_e2e_emit_record` |
| Summary manifest | Implemented | harness.sh `e2e_finish` |
| Build helper | Implemented | harness.sh `e2e_build` |
| Binary runner | Implemented | harness.sh `e2e_run_binary` |
| Rerun command | Implemented | harness.sh `e2e_rerun_command` |
| Golden log storage | **Not yet implemented** | Planned: `tests/e2e/golden/` |
| Golden log comparison tool | **Not yet implemented** | Planned: `scripts/compare-golden.sh` |
| Golden log update tool | **Not yet implemented** | Planned: `scripts/update-golden.sh` |
| Replay hint emission | **Not yet implemented** | Planned: extend `e2e_finish` on failure |
| Cross-profile parity runner | **Not yet implemented** | Planned: `make profile-parity` |
