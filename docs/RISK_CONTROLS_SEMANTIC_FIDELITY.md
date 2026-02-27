# Risk Controls: Semantic Fidelity, Safety, and Determinism Drift

> Operationalized controls for Plan Risks 2, 3, and 7.
> Bead: `bd-296.26`

---

## Scope

This document maps three interrelated risks to concrete owners, triggers, detection mechanisms, evidence artifacts, escalation paths, and CI enforcement points.

| Risk ID | Title | Plan Section |
|---------|-------|-------------|
| Risk 2 | C Safety Regressions | 14, Risk 2 |
| Risk 3 | Determinism Drift | 14, Risk 3 |
| Risk 7 | Spec/Implementation Drift | 14, Risk 7 |

---

## Risk 2: C Safety Regressions

### Description

Memory and state bugs are structurally easier in C than Rust. Without compile-time ownership and lifetime guarantees, use-after-free, double-free, buffer overruns, uninitialised reads, and state-machine violations become first-order threats.

### Owner

Primary: kernel implementor (whoever holds the `src/core/` and `src/runtime/` file reservations).
Escalation: project owner.

### Triggers (When to Raise)

- Any new code in `src/core/` or `src/runtime/` that manipulates raw pointers, casts, or array indices.
- Any change to handle/generation logic or arena allocation paths.
- Any new platform adapter that introduces OS-specific pointer arithmetic.
- Sanitizer finding in CI (ASan, MSan, UBSan, TSan).
- Ghost-monitor assertion failure in debug/CI builds.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Ghost borrow-ledger violations per CI run | `ghost_borrow_violation_count` in test log JSON | Must be 0 |
| Ghost protocol-monitor violations per CI run | `ghost_protocol_violation_count` in test log JSON | Must be 0 |
| Ghost linearity-monitor violations per CI run | `ghost_linearity_violation_count` in test log JSON | Must be 0 |
| ASan/MSan/UBSan findings | sanitizer exit codes in CI | Must be 0 |
| Handle-generation reuse collision rate | `handle_generation_collision_count` in stress test output | Must be 0 |
| Invariant test pass rate | `tests/invariant/` suite exit code | 100% pass |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Ghost monitor logs | `build/test-logs/ghost_*.json` | JSON, structured per `ghost_log_schema.json` |
| Sanitizer reports | `build/sanitizer-reports/*.log` | Text, sanitizer standard format |
| Invariant test results | `build/test-results/invariant_*.json` | JSON, structured per `test_log_schema.json` |
| Handle stress test output | `build/test-results/handle_stress_*.json` | JSON |
| Unit test results for `src/core/` | `build/test-results/unit_core_*.json` | JSON |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Generation-safe handles with validation on every dereference | Preventive | Code review + ghost borrow-ledger in debug builds |
| Debug ghost monitors compiled into all CI builds (`ASX_DEBUG_GHOST=1`) | Detective | CI build flag, test runner checks |
| Sanitizer CI jobs (ASan + UBSan minimum, MSan where feasible) | Detective | CI workflow `check` job, blocking merge |
| Cleanup-stack + poison-pill pattern for resource lifecycle | Preventive | Invariant tests + code review |
| Transition-authority tables reject illegal state moves at runtime in debug | Detective | Ghost protocol monitor |
| Linearity ledger checks obligation exactly-once semantics | Detective | Ghost linearity monitor |

### Escalation Path

1. **Auto-block**: CI fails on any ghost/sanitizer/invariant finding -> merge blocked.
2. **Triage**: Implementor inspects artifact at path listed above within same session.
3. **Root-cause**: Fix at source, not by suppressing ghost assertion or sanitizer flag.
4. **Escalate**: If fix requires changing a semantic contract, escalate to project owner with written justification and fixture evidence.

### CI Integration Points

- `make test-invariants` must pass with `ASX_DEBUG_GHOST=1`.
- `make sanitize` (ASan+UBSan) must exit 0.
- Ghost monitor violation counts extracted from test logs and asserted == 0 in CI script.
- PR check job is blocking on all of the above.

---

## Risk 3: Determinism Drift

### Description

Replay parity can silently drift as the runtime grows. Non-deterministic scheduling, uncontrolled entropy, timer resolution differences, or platform-specific behavior can cause the same scenario+seed to produce different semantic digests across runs or platforms.

### Owner

Primary: runtime kernel implementor (whoever holds `src/runtime/scheduler.*` and related files).
Escalation: project owner.

### Triggers (When to Raise)

- Any change to the scheduler loop, ready-queue ordering, or tie-break logic.
- Any change to timer wheel insertion, expiry, or cancel ordering.
- Any change to entropy/PRNG usage in runtime paths.
- Any change to event emission order or trace journal format.
- Digest mismatch between runs with identical scenario+seed+profile.
- Digest mismatch between JSON and BIN codec runs with identical input.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Digest stability across repeated runs | `digest_match_rate` in conformance output | Must be 100% |
| Cross-codec digest match | `codec_equivalence_pass` in conformance output | Must be true |
| Cross-profile digest match (shared fixtures) | `profile_parity_pass` in conformance output | Must be true |
| Ghost determinism-monitor violations | `ghost_determinism_violation_count` in test log JSON | Must be 0 |
| Hindsight nondeterminism log entries | `hindsight_nondet_event_count` in replay output | Must be 0 for deterministic mode |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Conformance report (Rust parity) | `build/conformance/rust_parity_*.json` | JSON per conformance report schema |
| Codec equivalence report | `build/conformance/codec_equiv_*.json` | JSON |
| Profile parity report | `build/conformance/profile_parity_*.json` | JSON |
| Semantic digest per run | `build/traces/*.digest` | Text (sha256 hex) |
| Hindsight nondeterminism ring dump | `build/test-logs/hindsight_nondet_*.json` | JSON |
| Ghost determinism monitor log | `build/test-logs/ghost_determinism_*.json` | JSON |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Deterministic tie-break key in scheduler (stable sort, explicit ordering) | Preventive | Design + unit tests |
| Seeded PRNG only in deterministic mode; no system entropy in hot paths | Preventive | Code review + ghost determinism monitor |
| Ghost determinism monitor detects nondeterministic event ordering | Detective | CI with `ASX_DEBUG_GHOST=1` |
| Hindsight nondeterminism logging ring captures suspect events for replay analysis | Detective | Enabled in CI/debug builds |
| Conformance digest stability gate: repeated runs must produce identical digests | Detective | `make conformance` in CI, blocking |
| Codec equivalence gate: JSON vs BIN must produce identical semantic digests | Detective | `make codec-equivalence` in CI, blocking |
| Profile parity gate: shared fixtures must produce identical digests across profiles | Detective | `make profile-parity` in CI, blocking |

### Escalation Path

1. **Auto-block**: Any digest mismatch in CI -> merge blocked.
2. **Triage**: Compare digest diff artifact to identify divergence point in event stream.
3. **Root-cause**: Identify source of nondeterminism (scheduling order, timer resolution, entropy leak).
4. **Fix**: Restore deterministic behavior at source. Never mask by adjusting expected digest.
5. **Escalate**: If fix requires changing tie-break semantics, escalate to project owner with replay evidence.

### CI Integration Points

- `make conformance` runs Rust-vs-C parity + repeated-run digest stability.
- `make codec-equivalence` runs JSON-vs-BIN digest comparison.
- `make profile-parity` runs cross-profile digest comparison for shared fixtures.
- All three are blocking PR checks.
- Ghost determinism monitor violation count extracted and asserted == 0.

---

## Risk 7: Spec/Implementation Drift

### Description

Over time, prose plan/spec documents diverge from actual code and test coverage. Machine-readable invariant schemas become stale. Transition tables in docs no longer match the code. This causes silent semantic regressions and makes audits unreliable.

### Owner

Primary: spec maintainer (whoever holds `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` and invariant schema files).
Secondary: every implementor is responsible for updating spec artifacts when changing behavior.
Escalation: project owner.

### Triggers (When to Raise)

- Any change to a state-machine transition in code that is not reflected in the invariant schema.
- Any new state, error code, or lifecycle event that is not in `FEATURE_PARITY.md`.
- Machine-readable schema generates tests that fail against current code.
- `FEATURE_PARITY.md` row status is stale (code changed but parity status not updated).
- Anti-butchering proof-block gate failure (semantic-sensitive change without evidence).

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Schema-generated test pass rate | `schema_gen_test_pass_rate` in CI output | Must be 100% |
| FEATURE_PARITY.md freshness | `parity_stale_row_count` from parity audit tool | Must be 0 |
| Semantic delta budget | `semantic_delta_count` in CI output | Must be 0 (default budget) |
| Anti-butchering gate pass | `anti_butchering_pass` in CI output | Must be true |
| Invariant schema coverage vs code transitions | `schema_coverage_ratio` from drift checker | Must be >= 1.0 |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Machine-readable invariant schema | `fixtures/invariant_schema.json` | JSON per invariant schema spec |
| Generated transition tests | `tests/invariant/generated_*.c` | C source (auto-generated) |
| FEATURE_PARITY.md | `docs/FEATURE_PARITY.md` | Markdown with machine-parseable status rows |
| Semantic delta budget report | `build/conformance/semantic_delta_*.json` | JSON |
| Anti-butchering proof block | `build/conformance/anti_butcher_*.json` | JSON |
| Drift-checker report | `build/conformance/drift_check_*.json` | JSON |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Machine-readable invariant schema as canonical source of truth | Preventive | Schema is checked into repo; tests generated from it |
| Generated transition tests from schema | Detective | CI runs generated tests; failures indicate drift |
| Semantic delta budget gate (default = 0) | Detective | CI blocks merge when budget exceeded |
| Anti-butchering proof-block gate for semantic-sensitive changes | Detective | CI requires evidence artifact for changes touching lifecycle/cancel/obligation code |
| FEATURE_PARITY.md freshness audit | Detective | CI tool checks parity rows match code state |
| Spec-review gate at phase boundaries | Preventive | Phase transition requires reviewer sign-off |

### Escalation Path

1. **Auto-block**: Schema/code drift detected -> merge blocked.
2. **Triage**: Run drift checker to identify specific mismatches.
3. **Resolution options**:
   a. Update schema to match intentional code change (requires justification + fixture evidence).
   b. Revert code change that unintentionally diverged from spec.
4. **Escalate**: If disagreement between spec and desired behavior, escalate to project owner for ADR ruling.

### CI Integration Points

- Schema-generated tests run as part of `make test-invariants`.
- Semantic delta budget gate runs in `make conformance`.
- Anti-butchering gate runs on PRs touching `src/core/` or `src/runtime/`.
- FEATURE_PARITY.md freshness check runs in CI `check` job.
- All gates are blocking.

---

## Cross-Risk Dependencies

| From Risk | To Risk | Dependency |
|-----------|---------|------------|
| Risk 7 (Spec Drift) | Risk 2 (Safety) | Stale spec -> implementors miss safety invariants |
| Risk 7 (Spec Drift) | Risk 3 (Determinism) | Stale transition tables -> undetected ordering changes |
| Risk 2 (Safety) | Risk 3 (Determinism) | Memory corruption -> nondeterministic behavior |
| Risk 3 (Determinism) | Risk 7 (Spec Drift) | Undetected nondeterminism -> conformance artifacts become unreliable |

## Verification Checklist

Before closing this risk control package:

- [ ] All leading indicators have concrete measurement mechanisms defined
- [ ] All evidence artifacts have path templates and format specifications
- [ ] All CI integration points are mapped to `make` targets
- [ ] All escalation paths terminate at a named role
- [ ] Cross-risk dependencies are documented
- [ ] Controls are classified as preventive or detective
- [ ] Each risk has at least one preventive and one detective control
