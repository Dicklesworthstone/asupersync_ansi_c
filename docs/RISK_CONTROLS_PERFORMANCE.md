# Risk Controls: Performance Tails and Adaptive-Controller Complexity

> Operationalized controls for Plan Risks 5, 6, and 11.
> Bead: `bd-296.28`

---

## Scope

This document maps three interrelated performance risks to concrete owners, triggers, detection mechanisms, evidence artifacts, escalation paths, and CI enforcement points.

| Risk ID | Title | Plan Section |
|---------|-------|-------------|
| Risk 5 | Performance Regressions From Over-Defensive Checks | 14, Risk 5 |
| Risk 6 | Adaptive-Controller Complexity Regression | 14, Risk 6 |
| Risk 11 | Tail-Latency Blind Spots | 14, Risk 11 |

---

## Risk 5: Performance Regressions From Over-Defensive Checks

### Description

Safety checks (generation validation, ghost monitors, linearity ledger verification, sanitizer instrumentation) are essential for correctness but can bloat hot paths in production builds. If debug-only checks leak into release builds, or if mandatory checks are placed in tight loops without evidence of necessity, scheduler throughput and latency degrade.

### Owner

Primary: performance lead / kernel implementor (whoever holds `src/runtime/scheduler.*`).
Secondary: anyone adding assertions or validation to hot-path code.
Escalation: project owner.

### Triggers (When to Raise)

- Any new assertion, validation check, or branch added to the scheduler hot loop.
- Any change to ghost-monitor or linearity-ledger code that affects compile-time gating.
- Benchmark regression beyond budgeted threshold on any profile.
- Cold-start latency regression on embedded targets.
- Binary size increase beyond budgeted threshold on embedded targets.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Scheduler hot-path cycle count (release build) | `sched_hotpath_cycles_p50` in bench output | Must not regress > 5% vs baseline |
| Ready-queue operation latency (release build) | `ready_queue_op_ns_p99` in bench output | Must stay O(1) amortized |
| Debug-only assertion leak count in release | `debug_assertion_in_release_count` from static analysis | Must be 0 |
| Binary size delta (CORE kernel, release) | `size` output for CORE release build | Must be < 64 KB |
| Binary size delta (EMBEDDED_ROUTER, release) | `size` output for EMBEDDED_ROUTER release | Must be < 128 KB (with trace) |
| Cold-start time on embedded target | `cold_start_ms` in embedded bench output | Must meet R1/R2/R3 SLO |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Benchmark results (core suite) | `build/bench/core_bench_*.json` | JSON with p50/p95/p99/p99.9 latencies |
| Benchmark results (embedded suite) | `build/bench/embedded_bench_*.json` | JSON |
| Binary size report | `build/ci/binary_size_*.json` | JSON per target per profile |
| Debug-assertion-in-release audit | `build/ci/debug_release_audit_*.txt` | Text, list of leaked assertions |
| Hotspot profile (perf/callgrind) | `build/perf/hotspot_*.txt` | Text/JSON profile output |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Compile-time debug/prod assertion levels (`ASX_DEBUG_GHOST` off in release) | Preventive | Build system enforces; static analysis verifies |
| Debug-assertion-in-release audit | Detective | CI script scans release binary for debug-only symbols |
| Benchmark regression gate with explicit budgets | Detective | `make bench` in CI with threshold comparison |
| Per-target binary size SLO gate | Detective | CI checks `size` output against thresholds |
| Evidence-gated optimization rule: performance work requires baseline + hotspot + proof + rollback | Preventive | Code review process |
| Hot-path check justification requirement: every check in scheduler loop must have written rationale | Preventive | Code review + inline documentation |

### Escalation Path

1. **Auto-block**: Benchmark regression beyond budget -> merge blocked (or warn, per `perf-tail-deadline` job config).
2. **Triage**: Run hotspot profile to identify regression source.
3. **Root-cause**: Determine if regression is from:
   a. Leaked debug assertion -> fix by gating with `ASX_DEBUG_GHOST`.
   b. Necessary safety check -> evaluate if check can be hoisted outside hot loop.
   c. Algorithmic regression -> fix algorithm.
4. **Trade-off**: If a safety check is genuinely needed in the hot path, document the performance cost and get project owner sign-off on the budget impact.
5. **Escalate**: If safety and performance requirements conflict, escalate to project owner for ADR.

### CI Integration Points

- `make bench` runs core and embedded benchmark suites.
- Benchmark results compared against stored baselines with configurable regression thresholds.
- Binary size checks run in CI `embedded-matrix` job.
- Debug-assertion-in-release audit runs in CI `check` job.
- Perf job is warn-or-block per threshold configuration.

---

## Risk 6: Adaptive-Controller Complexity Regression

### Description

Adaptive runtime logic (e.g., dynamic scheduling policies, auto-tuning backpressure, adaptive cancellation timeouts) can become opaque. If adaptive decisions are not explainable, not reproducible, and not covered by deterministic fallback paths, they undermine replay confidence and incident forensics.

### Owner

Primary: adaptive-systems implementor (whoever holds adaptive scheduling or auto-tuning code).
Secondary: determinism maintainer (overlaps with Risk 3 owner).
Escalation: project owner.

### Triggers (When to Raise)

- Any new adaptive/auto-tuning logic added to scheduler, timer, or backpressure paths.
- Any adaptive decision that cannot be replayed deterministically with a given seed.
- Expected-loss or confidence metric regression in adaptive controller.
- Counterfactual log analysis reveals unexplainable decision branches.
- Deterministic fallback mode produces different semantic outcomes than adaptive mode for same input.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Adaptive vs deterministic-fallback digest match | `adaptive_fallback_digest_match` in conformance output | Must be true for semantic outcomes |
| Counterfactual log completeness | `counterfactual_log_coverage_ratio` in adaptive test output | Must be >= 0.95 |
| Expected-loss trend (adaptive controller) | `expected_loss_trend` in evidence ledger | Must not increase vs baseline |
| Fallback-mode exercise rate in CI | `fallback_exercise_count` in test output | Must be > 0 per CI run |
| Adaptive decision reproducibility | `adaptive_repro_rate` with same seed/input | Must be 1.0 in deterministic mode |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Evidence ledger (adaptive controller) | `build/perf/evidence_ledger_*.json` | JSON with confidence/fallback/loss metrics |
| Counterfactual decision log | `build/test-logs/counterfactual_*.json` | JSON with decision points and alternatives |
| Adaptive vs fallback conformance report | `build/conformance/adaptive_fallback_*.json` | JSON with digest comparison |
| Deterministic mode replay with adaptive decisions | `build/traces/adaptive_replay_*.json` | JSON trace |
| Fallback-mode exercise log | `build/test-logs/fallback_exercise_*.json` | JSON |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Deterministic fallback mode always available for every adaptive path | Preventive | Design requirement + fallback test |
| Expected-loss + evidence-ledger contract for adaptive controllers | Detective | CI checks evidence ledger metrics |
| Counterfactual logging for decision explainability | Detective | Adaptive code must log alternatives considered |
| Adaptive-vs-fallback semantic conformance gate | Detective | CI runs same scenarios in both modes, compares digests |
| Deterministic replay of adaptive decisions with same seed/input | Detective | Conformance test with `ASX_DETERMINISTIC=1` |
| Adaptive complexity budget: maximum number of tunable parameters per controller | Preventive | Code review + documented parameter count |

### Escalation Path

1. **Auto-block**: Adaptive-vs-fallback digest mismatch on semantic events -> merge blocked.
2. **Triage**: Compare counterfactual logs to identify diverging decision point.
3. **Root-cause**: Determine if adaptive decision is:
   a. Performance-only (allowed to differ from fallback in timing, not semantics).
   b. Semantic (not allowed to differ).
4. **Fix**: Ensure adaptive path produces semantically identical outcomes to fallback.
5. **Escalate**: If adaptive behavior genuinely needs to differ semantically, this is a fundamental architecture question -> escalate to project owner.

### CI Integration Points

- Adaptive-vs-fallback conformance test runs in `make conformance`.
- Evidence ledger metrics checked in CI `perf-tail-deadline` job.
- Counterfactual log coverage checked in adaptive test suite.
- Fallback-mode exercise verification in CI test runner.
- All semantic gates are blocking; performance gates are warn-then-block.

---

## Risk 11: Tail-Latency Blind Spots

### Description

Average-latency improvements can mask catastrophic p99.9+ regressions. A change that improves mean scheduler throughput by 10% but introduces a 5x p99.99 spike is a net negative for HFT and automotive use cases. Without explicit tail-latency governance, these regressions go undetected until production incidents.

### Owner

Primary: performance lead / HFT profile maintainer.
Secondary: anyone modifying scheduler, timer, or channel hot paths.
Escalation: project owner.

### Triggers (When to Raise)

- p99 or p99.9 latency regression beyond budgeted threshold, even if mean/p50 improved.
- Jitter (max - min latency) increase beyond budgeted threshold.
- Deadline miss rate increase in automotive profile scenarios.
- Burst-overload scenario degrades beyond acceptable response-time envelope.
- New allocation or system call introduced in scheduler hot path.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Scheduler p99 latency | `sched_p99_ns` in bench output | Profile-specific SLO (e.g., HFT: < 1 us) |
| Scheduler p99.9 latency | `sched_p99_9_ns` in bench output | Profile-specific SLO |
| Scheduler p99.99 latency | `sched_p99_99_ns` in bench output | Profile-specific SLO |
| Jitter (max - min) | `sched_jitter_ns` in bench output | Profile-specific SLO |
| Deadline miss rate (automotive) | `deadline_miss_rate` in automotive bench output | Must be 0 for hard deadlines |
| Burst-overload response envelope | `burst_overload_p99_ns` in stress test output | Must stay within deterministic degradation bounds |
| Allocation count in hot path (release) | `hotpath_alloc_count` from instrumentation | Must be 0 in steady state |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Full latency histogram (core bench) | `build/bench/latency_histogram_core_*.json` | JSON with percentile breakdown |
| Full latency histogram (HFT bench) | `build/bench/latency_histogram_hft_*.json` | JSON |
| Deadline compliance report (automotive) | `build/bench/deadline_compliance_auto_*.json` | JSON with miss counts and worst-case |
| Burst-overload stress results | `build/stress/burst_overload_*.json` | JSON with p50/p95/p99/p99.9/p99.99 |
| Jitter report | `build/bench/jitter_report_*.json` | JSON with min/max/mean/jitter |
| Allocation-in-hot-path audit | `build/ci/hotpath_alloc_audit_*.txt` | Text, list of allocations found |
| Trend report (latency over time) | `build/perf/latency_trend_*.json` | JSON with per-commit latency values |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Dedicated tail/jitter gates with explicit p99/p99.9/p99.99 thresholds | Detective | `make bench` with threshold comparison in CI |
| Burst-overload scenario fixtures in stress test suite | Detective | `make stress` includes burst scenarios |
| Reject merges that improve mean latency while violating tail/jitter budgets | Detective | CI `perf-tail-deadline` job with dual-lens check |
| Allocation-free steady-state hot path requirement | Preventive | Design rule + CI hot-path allocation audit |
| Deterministic degradation under overload (no unbounded latency spikes) | Preventive | Design rule + stress test verification |
| Trend monitoring for latency drift across commits | Detective | CI stores per-commit latency and checks trend |
| Automotive deadline miss is hard-fail gate | Detective | CI blocks merge on any hard-deadline miss |

### Escalation Path

1. **Auto-block**: p99.9+ regression beyond budget -> merge blocked (for HFT/AUTOMOTIVE profiles).
2. **Warn**: p95 regression -> warn in CI output; block if trend continues.
3. **Triage**: Run latency histogram comparison to identify regression source.
4. **Root-cause**: Common sources:
   a. New allocation in hot path -> eliminate or move to setup phase.
   b. Lock contention (parallel profile) -> reduce critical section.
   c. Cache miss from data structure change -> profile and fix layout.
   d. New branch/check in tight loop -> evaluate necessity.
5. **Fix**: Address root cause. Never "fix" by adjusting threshold to accommodate regression.
6. **Escalate**: If safety check genuinely causes tail regression, escalate to project owner for safety-vs-performance trade-off ADR.

### CI Integration Points

- `make bench --suite core` runs core latency benchmarks with percentile extraction.
- `make bench --suite hft` runs HFT-specific latency benchmarks.
- `make bench --suite automotive` runs deadline compliance benchmarks.
- `make stress` includes burst-overload scenarios.
- `perf-tail-deadline` CI job checks dual-lens (mean AND tail) thresholds.
- Trend storage: CI writes per-commit latency JSON to trend artifact store.
- Allocation-in-hot-path audit runs in CI `check` job.
- Hard-deadline miss is always a blocking gate.
- Tail-latency regression is blocking for HFT/AUTOMOTIVE, warn-then-block for others.

---

## Cross-Risk Dependencies

| From Risk | To Risk | Dependency |
|-----------|---------|------------|
| Risk 5 (Over-Defensive Checks) | Risk 11 (Tail Blind Spots) | Leaked debug checks cause tail spikes |
| Risk 6 (Adaptive Complexity) | Risk 11 (Tail Blind Spots) | Opaque adaptive decisions cause unpredictable tails |
| Risk 6 (Adaptive Complexity) | Risk 5 (Over-Defensive Checks) | Adaptive fallback paths add hot-path complexity |
| Risk 11 (Tail Blind Spots) | Risk 5 (Over-Defensive Checks) | Tail regression investigation reveals unnecessary checks |

## Unified Performance Control Summary

The three performance risks share a common governance framework:

1. **Dual-lens performance governance**: Every performance metric is evaluated at BOTH mean/p50 AND tail (p99/p99.9/p99.99). Improving one at the expense of the other is rejected.
2. **Evidence-gated optimization**: No performance optimization is accepted without baseline measurement, hotspot evidence, semantic proof of correctness, and rollback path.
3. **Deterministic degradation**: Under overload, the system must degrade deterministically (bounded latency, explicit backpressure) rather than exhibiting unbounded spikes.
4. **Profile-specific SLOs**: Each profile (CORE, HFT, AUTOMOTIVE, EMBEDDED_ROUTER) has its own tail/deadline/jitter thresholds. A change can pass CORE but fail HFT.

## Verification Checklist

Before closing this risk control package:

- [ ] All leading indicators have concrete measurement mechanisms defined
- [ ] All evidence artifacts have path templates and format specifications
- [ ] All CI integration points are mapped to `make` targets
- [ ] All escalation paths terminate at a named role
- [ ] Cross-risk dependencies are documented
- [ ] Controls are classified as preventive or detective
- [ ] Each risk has at least one preventive and one detective control
- [ ] Dual-lens (mean + tail) governance is the anchoring measurement principle
- [ ] Profile-specific SLO thresholds are referenced (concrete values set when profiles are implemented)
