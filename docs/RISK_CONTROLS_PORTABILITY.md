# Risk Controls: Portability, Embedded Viability, and Profile-Fork Drift

> Operationalized controls for Plan Risks 4, 9, and 10.
> Bead: `bd-296.27`

---

## Scope

This document maps three interrelated portability and profile risks to concrete owners, triggers, detection mechanisms, evidence artifacts, escalation paths, and CI enforcement points.

| Risk ID | Title | Plan Section |
|---------|-------|-------------|
| Risk 4 | Portability Debt | 14, Risk 4 |
| Risk 9 | Embedded Feature Erosion | 14, Risk 9 |
| Risk 10 | Domain-Specific Fork Drift (HFT/Automotive/Router) | 14, Risk 10 |

---

## Risk 4: Portability Debt

### Description

Accidental POSIX, Linux, or GCC assumptions in `asx_core` or the runtime kernel create hidden portability debt. Code that compiles and passes tests on the primary development host may fail silently or produce undefined behavior on MSVC, 32-bit targets, big-endian systems, or freestanding environments.

### Owner

Primary: build system maintainer (whoever holds `Makefile`, `CMakeLists.txt`, and `src/platform/`).
Secondary: every kernel implementor must follow `docs/C_PORTABILITY_RULES.md`.
Escalation: project owner.

### Triggers (When to Raise)

- Any new `#include` of a non-standard-C header in `src/core/` or `src/runtime/`.
- Any use of compiler-specific extensions (`__attribute__`, `__declspec`, `typeof`, statement expressions) outside guarded platform adapter files.
- Any use of POSIX APIs (`pthread_*`, `mmap`, `clock_gettime`) outside `src/platform/posix/`.
- CI failure on any compiler/target in the portability matrix.
- Endian or alignment test failure on cross-target builds.
- Use of `long` or `int` where fixed-width types (`uint32_t`, `int64_t`) are required by the portability contract.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Portability matrix pass rate | Per-target CI exit codes across GCC/Clang/MSVC x 32/64-bit | 100% pass |
| Cross-target endian/alignment test pass rate | `endian_alignment_test_pass` in embedded matrix output | 100% pass |
| Platform-specific include count in core/runtime | `grep -r '#include' src/core/ src/runtime/ \| grep -v 'asx/' \| grep -v '<std'` | Must be 0 |
| Compiler extension usage in core/kernel | Static analysis or `grep` for `__attribute__`, `__declspec`, `typeof` in `src/core/`, `src/runtime/` | Must be 0 |
| QEMU cross-target scenario pass rate | `qemu_scenario_pass_rate` in embedded matrix output | 100% pass |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Portability matrix report | `build/ci/portability_matrix_*.json` | JSON, per-target pass/fail |
| Endian/alignment test results | `build/test-results/endian_alignment_*.json` | JSON |
| QEMU cross-target results | `build/ci/qemu_results_*.json` | JSON per target triplet |
| Include audit report | `build/ci/include_audit_*.txt` | Text, list of non-standard includes |
| Compiler extension audit | `build/ci/extension_audit_*.txt` | Text, list of extensions found |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| `docs/C_PORTABILITY_RULES.md` with banned constructs and required wrappers | Preventive | Code review checklist + CI lint |
| `asx_core` and runtime kernel use only standard C headers + `asx/` headers | Preventive | Include-audit CI script |
| Compiler matrix CI (GCC + Clang + MSVC, 32-bit + 64-bit) with warnings-as-errors | Detective | CI `check` job, blocking |
| Cross-target builds for router triplets (mipsel, armv7, aarch64) | Detective | CI `embedded-matrix` job, blocking |
| QEMU scenario execution for cross-target builds | Detective | CI `embedded-matrix` job, blocking |
| Endian and unaligned-access test suite for binary codec paths | Detective | CI `conformance` job, blocking |
| Fixed-width type policy: no bare `int`/`long` in data structures or wire formats | Preventive | Static analysis lint + code review |

### Escalation Path

1. **Auto-block**: Any portability matrix target fails -> merge blocked.
2. **Triage**: Identify failing target(s) and specific compilation/test errors.
3. **Root-cause**: Determine if issue is platform assumption in core code or legitimate platform adapter gap.
4. **Fix options**:
   a. Move platform-specific code to `src/platform/<target>/`.
   b. Replace non-portable construct with standard C equivalent.
   c. Add audited wrapper primitive (must be tested on all matrix targets).
5. **Escalate**: If a new platform dependency is genuinely required in core, escalate to project owner with justification and portability-impact analysis.

### CI Integration Points

- `make build` with warnings-as-errors on all matrix compilers.
- `make ci-embedded-matrix` runs cross-target builds + QEMU scenarios.
- Include-audit and extension-audit scripts run in `check` job.
- `make lint` includes fixed-width type policy checks.
- All gates are blocking PR checks.

---

## Risk 9: Embedded Feature Erosion

### Description

Pressure to "simplify for routers" or "reduce footprint" can silently remove asupersync semantic differentiators. If embedded profiles strip lifecycle guarantees, cancellation semantics, or obligation linearity to save memory or code size, the port loses its fundamental value proposition.

### Owner

Primary: profile maintainer (whoever holds `src/platform/embedded/` and profile configuration).
Secondary: anyone proposing embedded-specific optimizations.
Escalation: project owner.

### Triggers (When to Raise)

- Any PR that modifies embedded profile behavior to skip or weaken a lifecycle check.
- Any `#ifdef ASX_PROFILE_EMBEDDED_ROUTER` that changes semantic behavior (not just resource limits).
- Cross-profile semantic digest mismatch between `CORE` and `EMBEDDED_ROUTER` for shared fixtures.
- Proposal to add an "embedded-lite" codepath that bypasses obligation linearity or cancellation protocol.
- Resource-class change (R1/R2/R3) that alters state-machine transitions instead of just limits.

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Cross-profile digest match (CORE vs EMBEDDED_ROUTER) | `profile_parity_pass` in conformance output | Must be true |
| Semantic-plane/resource-plane split audit | Manual or script check of `#ifdef` blocks in core | Semantic `#ifdef` count in core must be 0 |
| Embedded profile invariant test pass rate | `tests/invariant/` with `ASX_PROFILE_EMBEDDED_ROUTER` | 100% pass (same tests as CORE) |
| Binary size delta per change | `size` output diff for embedded target | Must stay within R1/R2/R3 budgets |
| Embedded feature coverage (same scenarios as CORE) | `embedded_scenario_coverage_ratio` | Must be 1.0 |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Profile parity report | `build/conformance/profile_parity_*.json` | JSON with per-profile digests |
| Semantic ifdef audit | `build/ci/semantic_ifdef_audit_*.txt` | Text, list of semantic-changing `#ifdef` blocks |
| Embedded invariant test results | `build/test-results/invariant_embedded_*.json` | JSON |
| Binary size report per profile per target | `build/ci/binary_size_*.json` | JSON with per-target sizes |
| Embedded scenario coverage report | `build/test-results/embedded_coverage_*.json` | JSON |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Semantic-plane / resource-plane architecture split | Preventive | Design rule: profiles change limits/defaults, never lifecycle semantics |
| Cross-profile semantic digest equivalence gate | Detective | `make profile-parity` in CI, blocking |
| Same invariant test suite runs on all profiles | Detective | CI runs `make test-invariants` per profile |
| Semantic `#ifdef` audit: no profile-conditional code in lifecycle/cancel/obligation paths | Detective | CI script scans for semantic `#ifdef` in `src/core/`, `src/runtime/` |
| Binary size SLO gate per resource class | Detective | CI checks `size` output against R1/R2/R3 thresholds |
| Embedded scenario parity: same scenario packs run on CORE and EMBEDDED_ROUTER | Detective | CI conformance job |

### Escalation Path

1. **Auto-block**: Profile parity digest mismatch -> merge blocked.
2. **Triage**: Compare digest diff to identify which semantic event diverged.
3. **Root-cause**: Determine if the change is resource-plane (allowed) or semantic-plane (forbidden).
4. **Fix**: Restore semantic equivalence. Resource-plane optimizations are fine; semantic-plane changes are not.
5. **Escalate**: If there is a genuine case for embedded-specific semantic behavior, escalate to project owner. Requires:
   - Written justification with user-impact analysis.
   - Explicit semantic delta budget exception (default budget is 0).
   - Updated `FEATURE_PARITY.md` with documented exception.

### CI Integration Points

- `make profile-parity` runs cross-profile digest comparison.
- `make test-invariants` runs on each profile build.
- Semantic `#ifdef` audit runs in CI `check` job.
- Binary size check runs in CI `embedded-matrix` job.
- All gates are blocking PR checks.

---

## Risk 10: Domain-Specific Fork Drift (HFT/Automotive/Router)

### Description

Optimization pressure from different domain verticals (ultra-low latency for HFT, deadline/watchdog for automotive, footprint for routers) can create hidden behavior forks between profiles. What starts as "just a performance tweak" can evolve into semantically different runtimes sharing the same codebase.

### Owner

Primary: profile maintainer for each vertical (`HFT`, `AUTOMOTIVE`, `EMBEDDED_ROUTER`).
Secondary: cross-profile conformance maintainer.
Escalation: project owner.

### Triggers (When to Raise)

- Cross-profile semantic digest mismatch for any shared fixture.
- Any vertical-specific optimization that touches scheduler ordering, cancellation propagation, or obligation resolution.
- Any new `#ifdef ASX_PROFILE_HFT` or `ASX_PROFILE_AUTOMOTIVE` block in core semantic paths.
- Semantic delta budget exceeded (default = 0).
- Vertical adapter that changes observable behavior (not just performance characteristics).

### Leading Indicators

| Indicator | Measurement | Threshold |
|-----------|-------------|-----------|
| Cross-profile digest match (all profiles) | `profile_parity_pass` for full profile set | Must be true |
| Semantic delta budget consumption | `semantic_delta_count` in CI output | Must be 0 (or explicitly approved exception) |
| Vertical adapter isomorphism verification | `adapter_isomorphism_pass` in conformance output | Must be true |
| Profile-conditional code in semantic paths | `semantic_ifdef_count` from audit | Must be 0 |
| Cross-vertical fixture coverage | `cross_vertical_fixture_coverage_ratio` | Must be 1.0 for shared fixture set |

### Evidence Artifacts

| Artifact | Path/Query | Format |
|----------|-----------|--------|
| Cross-profile parity report (full) | `build/conformance/profile_parity_full_*.json` | JSON with all profile digests |
| Semantic delta budget report | `build/conformance/semantic_delta_*.json` | JSON |
| Vertical adapter isomorphism artifact | `build/conformance/adapter_iso_*.json` | JSON with equivalence proof |
| Profile-specific performance report | `build/perf/profile_perf_*.json` | JSON with tail/deadline/throughput metrics |
| Cross-vertical fixture results | `build/conformance/cross_vertical_*.json` | JSON |

### Controls

| Control | Type | Enforcement |
|---------|------|-------------|
| Mandatory cross-profile digest equivalence for shared fixture sets | Detective | `make profile-parity` in CI, blocking |
| Semantic delta budget gate (default = 0) | Detective | CI blocks merge when budget exceeded without explicit exception |
| Vertical adapter isomorphism requirement: every domain optimization must have a fallback path proven equivalent | Preventive | Design review + isomorphism artifact |
| Explicit fallback path for every vertical optimization | Preventive | Code review + fallback test |
| Golden domain scenario packs per vertical (HFT burst, automotive deadline, router storm) | Detective | CI runs vertical-specific scenarios |
| Profile-conditional code audit in semantic paths | Detective | CI `check` job |

### Escalation Path

1. **Auto-block**: Cross-profile digest mismatch -> merge blocked.
2. **Triage**: Identify which profile(s) diverged and at which event.
3. **Root-cause**: Determine if change is operational (scheduling policy, buffer sizing) or semantic (lifecycle, ordering, correctness).
4. **Fix options**:
   a. If operational: ensure fallback path produces identical semantics (isomorphism artifact required).
   b. If semantic: revert the change. Semantic forks are not allowed.
5. **Exception process**: If a genuine semantic divergence is needed for a vertical:
   - Requires explicit project owner approval.
   - Semantic delta budget exception documented in ADR.
   - `docs/SEMANTIC_DELTA_EXCEPTIONS.json` updated with approved fixture-scoped record.
   - Both old and new behavior must have fixture coverage.
   - `FEATURE_PARITY.md` updated with exception row.
6. **Escalate**: Unresolvable conflicts between vertical requirements go to project owner for ADR.

### CI Integration Points

- `make profile-parity` with full profile set (CORE, POSIX, FREESTANDING, EMBEDDED_ROUTER, HFT, AUTOMOTIVE).
- `make conformance` includes semantic delta budget check and emits
  `build/conformance/semantic_delta_*.json` using
  `docs/SEMANTIC_DELTA_EXCEPTIONS.json` for approved exceptions.
- Vertical scenario packs in dedicated CI jobs (`perf-tail-deadline`).
- Profile-conditional code audit in CI `check` job.
- All gates are blocking (perf gates may be warn-then-block per threshold config).

---

## Cross-Risk Dependencies

| From Risk | To Risk | Dependency |
|-----------|---------|------------|
| Risk 4 (Portability) | Risk 9 (Embedded Erosion) | Portability debt in core -> embedded targets get broken or "fixed" by stripping features |
| Risk 4 (Portability) | Risk 10 (Fork Drift) | Platform assumptions -> profile-specific workarounds -> semantic forks |
| Risk 9 (Embedded Erosion) | Risk 10 (Fork Drift) | Embedded simplifications -> precedent for other profiles to diverge |
| Risk 10 (Fork Drift) | Risk 9 (Embedded Erosion) | HFT/automotive forks -> pressure to "simplify embedded too" |

## Unified Portability/Profile Control Summary

The three risks share a common enforcement backbone:

1. **Cross-profile digest equivalence** is the single most important gate. If digests match, semantic forks are structurally impossible.
2. **Portability matrix CI** catches platform assumptions early, before they calcify into profile-specific workarounds.
3. **Semantic-plane / resource-plane architecture** is the design-level prevention mechanism. All three risks are mitigated primarily by this architectural choice.

## Verification Checklist

Before closing this risk control package:

- [ ] All leading indicators have concrete measurement mechanisms defined
- [ ] All evidence artifacts have path templates and format specifications
- [ ] All CI integration points are mapped to `make` targets
- [ ] All escalation paths terminate at a named role
- [ ] Cross-risk dependencies are documented
- [ ] Controls are classified as preventive or detective
- [ ] Each risk has at least one preventive and one detective control
- [ ] Semantic-plane / resource-plane split is the anchoring design principle
