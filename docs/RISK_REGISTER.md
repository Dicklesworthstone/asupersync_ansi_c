# Operationalized Risk Register

> Consolidated risk register for the asupersync ANSI C port.
> Bead: `bd-296.14`
> Sources: `bd-296.26` (semantic fidelity), `bd-296.27` (portability), `bd-296.28` (performance)

---

## Overview

This register covers all 12 risks from Plan Section 14 plus Resource-Exhaustion (Risk 8) and Upstream Drift (Risk 12). Each risk is mapped to:

- **Owner**: who is responsible for monitoring and remediation
- **Triggers**: observable conditions that signal the risk is materializing
- **Leading indicators**: measurable metrics for early detection
- **Evidence artifacts**: concrete file paths and formats for CI/audit verification
- **CI gates**: `make` targets that enforce controls
- **Escalation path**: who decides what when controls are breached

Detailed controls for each risk cluster are in companion documents:

| Document | Risks Covered |
|----------|--------------|
| [`RISK_CONTROLS_SEMANTIC_FIDELITY.md`](RISK_CONTROLS_SEMANTIC_FIDELITY.md) | Risk 2 (Safety), Risk 3 (Determinism), Risk 7 (Spec Drift) |
| [`RISK_CONTROLS_PORTABILITY.md`](RISK_CONTROLS_PORTABILITY.md) | Risk 4 (Portability), Risk 9 (Embedded Erosion), Risk 10 (Fork Drift) |
| [`RISK_CONTROLS_PERFORMANCE.md`](RISK_CONTROLS_PERFORMANCE.md) | Risk 5 (Over-Defensive Checks), Risk 6 (Adaptive Complexity), Risk 11 (Tail Latency) |

---

## Risk Summary Table

| ID | Risk | Severity | Owner Role | Primary CI Gate | Detailed Controls |
|----|------|----------|-----------|----------------|-------------------|
| R1 | Scope Explosion | High | Project owner | Wave gating protocol | Plan Section 4 + bd-296.10 |
| R2 | C Safety Regressions | Critical | Kernel implementor | `make test-invariants`, `make sanitize` | [Semantic Fidelity](RISK_CONTROLS_SEMANTIC_FIDELITY.md#risk-2-c-safety-regressions) |
| R3 | Determinism Drift | Critical | Runtime kernel implementor | `make conformance` | [Semantic Fidelity](RISK_CONTROLS_SEMANTIC_FIDELITY.md#risk-3-determinism-drift) |
| R4 | Portability Debt | High | Build system maintainer | `make build` (matrix), `make ci-embedded-matrix` | [Portability](RISK_CONTROLS_PORTABILITY.md#risk-4-portability-debt) |
| R5 | Performance from Over-Defensive Checks | Medium | Performance lead | `make bench` | [Performance](RISK_CONTROLS_PERFORMANCE.md#risk-5-performance-regressions-from-over-defensive-checks) |
| R6 | Adaptive-Controller Complexity | Medium | Adaptive-systems implementor | `make conformance` (adaptive vs fallback) | [Performance](RISK_CONTROLS_PERFORMANCE.md#risk-6-adaptive-controller-complexity-regression) |
| R7 | Spec/Implementation Drift | High | Spec maintainer | `make test-invariants` (schema-generated) | [Semantic Fidelity](RISK_CONTROLS_SEMANTIC_FIDELITY.md#risk-7-specimplementation-drift) |
| R8 | Resource-Exhaustion UB | Critical | Kernel implementor | `make test` (exhaustion suite) | See below |
| R9 | Embedded Feature Erosion | High | Profile maintainer | `make profile-parity` | [Portability](RISK_CONTROLS_PORTABILITY.md#risk-9-embedded-feature-erosion) |
| R10 | Domain-Specific Fork Drift | High | Cross-profile conformance maintainer | `make profile-parity` | [Portability](RISK_CONTROLS_PORTABILITY.md#risk-10-domain-specific-fork-drift-hftautomotiverouter) |
| R11 | Tail-Latency Blind Spots | High | Performance lead | `make bench` (tail gates) | [Performance](RISK_CONTROLS_PERFORMANCE.md#risk-11-tail-latency-blind-spots) |
| R12 | Rust Reference Upstream Drift | Medium | Baseline provenance maintainer | Fixture provenance checks | See below |

---

## Risks Not Yet Covered by Detailed Documents

### Risk 1: Scope Explosion

**Owner**: Project owner.
**Primary control**: Wave gating protocol (bd-296.10). No subsystem enters implementation without documented gate satisfaction.
**Leading indicator**: Number of Wave B/C/D items entering active work before Wave A gates are closed.
**CI gate**: Wave-close checklist verification (manual + scripted evidence check).
**Escalation**: Any attempt to start a later-wave item before current wave gates are met -> blocked by wave gating protocol.

### Risk 8: Resource-Exhaustion Undefined Behavior

**Owner**: Kernel implementor (same as Risk 2).
**Primary control**: Strict exhaustion semantics contract - deterministic behavior for OOM, queue overflow, timer overflow.
**Leading indicators**:
- Per-boundary failure-atomicity test pass rate: must be 100%.
- Exhaustion scenario coverage: every resource boundary has explicit test.
- Partial-update detection: no data structure is left in inconsistent state after exhaustion.

**Evidence artifacts**:
- `build/test-results/exhaustion_*.json` - per-boundary exhaustion test results.
- `build/test-results/failure_atomic_*.json` - failure-atomicity verification results.

**CI gate**: `make test` includes exhaustion test suite. Blocking.
**Escalation**: Partial-update detected after exhaustion -> same escalation as Risk 2 (safety regression).

### Risk 12: Rust Reference Upstream Drift

**Owner**: Baseline provenance maintainer (bd-296.12).
**Primary control**: Baseline freeze protocol (Plan Section 1.1).
**Leading indicators**:
- Fixture provenance freshness: `rust_baseline_commit` field matches pinned value.
- Rebase protocol compliance: delta classification completed before parity target update.

**Evidence artifacts**:
- `fixtures/rust_reference/provenance.json` - baseline commit, toolchain hash, Cargo.lock snapshot.
- Rebase decision records (ADR format).

**CI gate**: Fixture provenance check verifies `rust_baseline_commit` consistency. Blocking.
**Escalation**: Upstream drift detected without rebase protocol -> block until classification completed.

---

## CI Gate Summary

| `make` Target | Risks Covered | Blocking? |
|---------------|---------------|-----------|
| `make build` (warnings-as-errors, full matrix) | R4 | Yes |
| `make test` | R2, R8 | Yes |
| `make test-invariants` | R2, R7 | Yes |
| `make sanitize` | R2 | Yes |
| `make conformance` | R3, R6, R7 | Yes |
| `make codec-equivalence` | R3 | Yes |
| `make profile-parity` | R3, R9, R10 | Yes |
| `make bench` | R5, R11 | Warn/Block per threshold |
| `make ci-embedded-matrix` | R4, R9 | Yes |
| `make lint` | R4, R7 | Yes |
| `make format-check` | R4 | Yes |
| `make fuzz-smoke` | R2, R3 | Yes |

---

## Evidence Artifact Taxonomy

All CI-produced evidence follows a consistent naming and location scheme:

```
build/
  test-results/          # Unit, invariant, exhaustion test outputs
    unit_*.json
    invariant_*.json
    exhaustion_*.json
    failure_atomic_*.json
  test-logs/             # Ghost monitors, counterfactual logs, hindsight rings
    ghost_*.json
    counterfactual_*.json
    hindsight_nondet_*.json
    fallback_exercise_*.json
  conformance/           # Rust parity, codec, profile, semantic delta
    rust_parity_*.json
    codec_equiv_*.json
    profile_parity_*.json
    profile_parity_full_*.json
    semantic_delta_*.json
    anti_butcher_*.json
    drift_check_*.json
    adaptive_fallback_*.json
    cross_vertical_*.json
    adapter_iso_*.json
  sanitizer-reports/     # ASan, MSan, UBSan, TSan outputs
    *.log
  bench/                 # Benchmark results and histograms
    core_bench_*.json
    embedded_bench_*.json
    latency_histogram_*.json
    deadline_compliance_*.json
    jitter_report_*.json
  perf/                  # Profiling, evidence ledgers, trends
    hotspot_*.txt
    evidence_ledger_*.json
    latency_trend_*.json
    profile_perf_*.json
  stress/                # Stress test results
    burst_overload_*.json
  traces/                # Replay traces and digests
    *.digest
    adaptive_replay_*.json
  ci/                    # CI audit and matrix reports
    portability_matrix_*.json
    qemu_results_*.json
    binary_size_*.json
    include_audit_*.txt
    extension_audit_*.txt
    semantic_ifdef_audit_*.txt
    debug_release_audit_*.txt
    hotpath_alloc_audit_*.txt
```

---

## Escalation Hierarchy

All risk escalation paths follow this hierarchy:

1. **CI auto-block**: Merge is blocked. Implementor must fix before re-attempting.
2. **Triage**: Implementor inspects evidence artifacts to identify root cause.
3. **Fix at source**: Fix the actual problem. Never suppress or mask findings.
4. **ADR escalation**: If fix requires changing a semantic contract, behavioral boundary, or trade-off between competing requirements, escalate to project owner for a binding Architecture Decision Record.

---

## Breach Playbook Quick Reference

| Breach Type | First Action | Artifact to Check | Escalation Trigger |
|-------------|-------------|-------------------|-------------------|
| Ghost monitor violation | Check `ghost_*.json` | `ghost_borrow_violation_count` | Fix requires semantic change |
| Sanitizer finding | Check `sanitizer-reports/*.log` | Finding location and type | Fix is non-obvious |
| Digest mismatch | Check `conformance/*.json` | `diffs` array in report | Mismatch is intentional |
| Profile parity failure | Check `profile_parity_*.json` | Per-profile digests | Semantic divergence desired |
| Benchmark regression | Check `bench/*.json` | p99/p99.9 values vs baseline | Safety-performance conflict |
| Deadline miss | Check `deadline_compliance_*.json` | Miss count and worst-case | Any hard-deadline miss |
| Spec drift detected | Check `drift_check_*.json` | Schema vs code mismatches | Behavior change is intentional |
| Exhaustion test failure | Check `exhaustion_*.json` | Failure-atomicity status | Partial update detected |

---

## Verification Checklist

- [x] All 12 plan risks are assigned to owners
- [x] All risks have measurable leading indicators
- [x] All risks have evidence artifact paths
- [x] All risks are linked to CI gates
- [x] All escalation paths terminate at project owner
- [x] Evidence artifact taxonomy is documented
- [x] Breach playbook quick reference is provided
- [x] Detailed controls are linked to companion documents
- [x] CI gate summary covers all risks
