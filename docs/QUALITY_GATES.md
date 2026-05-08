# Quality Gates Registry

> **Bead:** `bd-66l.2`
> **Status:** Canonical gate registry (plan section 10.6 → Makefile → CI → artifacts)
> **Last updated:** 2026-05-08

This document is the single source of truth for all quality gates enforced on the
asx ANSI C port. Every gate listed here is mandatory — merges are blocked when any
gate fails. Gate status is binary: **pass** or **fail**. There is no soft-pass.

## 1. Gate Inventory

Nine mandatory gates from plan section 10.6, plus release artifact integrity and supplementary enforcement gates.

### 1.1 Portability CI Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-PORT` |
| **Plan ref** | Section 10.6 item 1 |
| **Makefile targets** | `build`, `build-gcc`, `build-clang`, `build-32`, `build-64` |
| **CI job** | `check`, `compiler-matrix` |
| **Scripts** | `tools/ci/run_compiler_matrix.sh`, `tools/ci/generate_layout_budget_report.sh`, `tools/ci/compute_layout_budget_delta.sh` |
| **Artifacts** | `tools/ci/artifacts/build/*.jsonl`, `tools/ci/artifacts/layout/*.json`, `tools/ci/artifacts/layout/*.kv` |
| **Pass criteria** | All compiler×bitness×profile matrix rows build with `-Werror`. Layout budget invariants (struct sizes, budget constants) match baseline. |
| **Rerun** | `make build CC=gcc BITS=64 PROFILE=CORE` (per-cell), `tools/ci/run_compiler_matrix.sh --quick` (matrix) |
| **Failure action** | Fix compilation errors or layout regressions. Layout regressions require proof-block update. |

### 1.2 OOM/Resource-Exhaustion Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-OOM` |
| **Plan ref** | Section 10.6 item 2 |
| **Makefile targets** | `test-unit` (test_budget, test_boundary_exhaustion), `test-e2e` (robustness_exhaustion.sh) |
| **CI job** | `unit-invariant`, `e2e` |
| **Scripts** | `tests/e2e/robustness_exhaustion.sh` |
| **Artifacts** | `build/test-logs/e2e-robustness-exhaustion.jsonl` |
| **Pass criteria** | All budget-exhaustion and resource-exhaustion tests pass. Exhaustion returns deterministic error codes (`ASX_E_POLL_BUDGET_EXHAUSTED`, `ASX_E_RESOURCE_EXHAUSTED`). Region remains healthy after exhaustion. |
| **Rerun** | `make test-unit` (unit), `ASX_E2E_SEED=42 tests/e2e/robustness_exhaustion.sh` (e2e) |
| **Failure action** | Fix non-deterministic exhaustion behavior. Budget paths must be failure-atomic. |

### 1.3 Differential Fuzzing Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-FUZZ` |
| **Plan ref** | Section 10.6 item 3 |
| **Makefile targets** | `fuzz-smoke` (CI, 100 iterations), `fuzz-nightly` (nightly, 100K iterations), `minimize-selftest` |
| **CI job** | `fuzz-parity` |
| **Scripts** | `tests/fuzz/fuzz_differential.c`, `tests/fuzz/fuzz_minimize.c` |
| **Artifacts** | `build/fuzz/fuzz_differential`, `build/fuzz/fuzz_minimize`, counterexample files |
| **Pass criteria** | Smoke test passes (100 scenario mutations self-consistent). Nightly passes (100K). Failing cases produce minimized counterexamples with parity diffs. |
| **Rerun** | `make fuzz-smoke` (CI), `make fuzz-run FUZZ_ARGS="--seed 42 --iterations 5000"` (targeted) |
| **Failure action** | Minimize with `make minimize-run MIN_ARGS="--failure-digest <digest>"`. Fix semantic divergence. |

### 1.4 Embedded Target Matrix Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-EMBED` |
| **Plan ref** | Section 10.6 item 4 |
| **Makefile targets** | `ci-embedded-matrix`, `build-embedded-mipsel`, `build-embedded-armv7`, `build-embedded-aarch64`, `qemu-smoke` |
| **CI job** | `embedded-matrix` |
| **Scripts** | `tools/ci/run_embedded_matrix.sh`, `tools/ci/run_qemu_smoke.sh`, `tools/ci/check_endian_assumptions.sh`, `tools/ci/portability_check.c` |
| **Artifacts** | `tools/ci/artifacts/embedded/*.jsonl`, `tools/ci/artifacts/qemu/*.jsonl` |
| **Pass criteria** | All three router-class triplets (mipsel/armv7/aarch64 + musl) build cleanly. QEMU scenario replay passes. Layout budget invariants match host. |
| **Rerun** | `make build-embedded-mipsel PROFILE=EMBEDDED_ROUTER` (single target), `make ci-embedded-matrix` (full) |
| **Failure action** | Fix cross-compilation errors. Endian/alignment issues must update `include/asx/portable.h`. |

### 1.5 Profile Semantic Parity Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-PROFILE` |
| **Plan ref** | Section 10.6 item 5 |
| **Makefile targets** | `profile-parity` |
| **CI job** | `profile-parity` |
| **Scripts** | `tools/ci/run_profile_parity.sh`, `tools/ci/run_conformance.sh --mode profile-parity` |
| **Artifacts** | `build/conformance/profile_parity_*.summary.json`, `build/conformance/adapter_iso_*.json` |
| **Pass criteria** | CORE, FREESTANDING, EMBEDDED_ROUTER, HFT, and AUTOMOTIVE profiles emit identical canonical semantic digests for shared fixtures. Adapter isomorphism proofs pass. |
| **Rerun** | `make profile-parity` |
| **Failure action** | Identify profile-specific divergence via diff artifact. Fix or document as approved semantic delta. |

### 1.6 Tail-Latency/Jitter Gate (HFT)

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-HFT-PERF` |
| **Plan ref** | Section 10.6 item 6 |
| **Makefile targets** | `bench`, `bench-json`, `test-e2e-vertical` (hft_microburst.sh, market_open_burst.sh) |
| **CI job** | `perf-tail-deadline` (push to main), `e2e` |
| **Scripts** | `tests/bench/bench_runtime.c`, `tests/e2e/hft_microburst.sh`, `tests/e2e/market_open_burst.sh` |
| **Artifacts** | `bench-results.json` (p50/p95/p99/p99.9/p99.99), `build/e2e-artifacts/*/market-open-burst.summary.json` |
| **Pass criteria** | HFT benchmark produces valid metrics. Microburst and market-open-burst e2e pass. Overflow behavior under burst remains deterministic. p99 histogram bin coverage validated by test_hft_instrument (34 tests). |
| **Rerun** | `make bench-json` (perf), `ASX_E2E_SEED=42 ASX_E2E_PROFILE=HFT tests/e2e/market_open_burst.sh` (e2e) |
| **Failure action** | Profile-specific p99/jitter regressions require investigation. Check histogram bin distribution via test_hft_instrument. |
| **Note** | SLO-based blocking is active via `evaluate_slo_gates.sh --strict` in `perf.yml`. Per-profile budgets defined in `tools/ci/slo_baselines.json`. |

### 1.7 Deadline/Watchdog Gate (Automotive)

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-AUTO-DEADLINE` |
| **Plan ref** | Section 10.6 item 7 |
| **Makefile targets** | `test-e2e-vertical` (automotive_watchdog.sh, automotive_fault_burst.sh), `test-unit` (test_automotive_instrument) |
| **CI job** | `e2e`, `perf-tail-deadline` (push to main) |
| **Scripts** | `tests/e2e/automotive_watchdog.sh`, `tests/e2e/automotive_fault_burst.sh` |
| **Artifacts** | `build/e2e-artifacts/*/automotive-fault-burst.summary.json` |
| **Pass criteria** | Checkpoint and deadline-observation scenarios pass. Degraded-mode transitions are deterministic and audit-traceable. Clock skew/reversal/entropy faults contained. |
| **Rerun** | `ASX_E2E_SEED=42 ASX_E2E_PROFILE=AUTOMOTIVE tests/e2e/automotive_fault_burst.sh` |
| **Failure action** | Check containment via `asx_region_poison()`. Verify checkpoint/cancel paths. See `docs/DEPLOYMENT_HARDENING.md` automotive section. |

### 1.8 Crash/Restart Replay Continuity Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-CONTINUITY` |
| **Plan ref** | Section 10.6 item 8 |
| **Makefile targets** | `test-e2e-vertical` (continuity.sh, continuity_restart.sh) |
| **CI job** | `e2e` |
| **Scripts** | `tests/e2e/continuity.sh`, `tests/e2e/continuity_restart.sh` |
| **Artifacts** | `build/test-logs/e2e-continuity.jsonl`, `build/test-logs/e2e-continuity-restart.jsonl` |
| **Pass criteria** | Replay after restart reproduces canonical semantic digest. Persisted scenarios produce identical trace digests across runs. |
| **Rerun** | `ASX_E2E_SEED=42 tests/e2e/continuity_restart.sh` |
| **Failure action** | Compare trace digests. Non-deterministic replay indicates state corruption or non-deterministic hook dispatch. |

### 1.9 Semantic Delta Budget Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-SEM-DELTA` |
| **Plan ref** | Section 10.6 item 9 |
| **Makefile targets** | `lint-anti-butchering`, `conformance` |
| **CI job** | `check`, `conformance` |
| **Scripts** | `tools/ci/check_anti_butchering.sh`, `tools/ci/run_conformance.sh` |
| **Artifacts** | `build/conformance/anti_butcher_*.json`, `build/conformance/semantic_delta_*.json` |
| **Pass criteria** | Semantic delta budget is `0` by default. Non-zero delta fails CI unless explicitly approved with a fixture-scoped exception. Proof-block metadata present for all semantic-sensitive changes. |
| **Rerun** | `make lint-anti-butchering` (proof blocks), `make conformance` (delta evaluation) |
| **Failure action** | Add "Guarantee Impact" block to commit message. See `docs/PROOF_BLOCK_EXCEPTION_WORKFLOW.md` for exception process. |

### 1.10 Release Artifact Integrity Gate

| Field | Value |
|-------|-------|
| **Gate ID** | `GATE-RELEASE-INTEGRITY` |
| **Plan ref** | `bd-56t.1` |
| **Makefile targets** | `release-artifacts` |
| **CI job** | `release-artifacts` (tag workflow) |
| **Scripts** | `tools/ci/run_release_artifacts.sh` |
| **Artifacts** | `build/release/dist/asx-*.tar.xz`, `build/release/dist/asx-*.tar.xz.sha256`, `build/release/dist/asx-*.tar.xz.sigstore.json`, `build/release/dist/asx-*.provenance.json`, `build/ci-manifests/release-artifacts.manifest.json` |
| **Pass criteria** | Each release target emits deterministic tarball, checksum, sigstore bundle, and provenance metadata whose version maps to the release tag. |
| **Rerun** | `make release-artifacts RELEASE_VERSION=<x.y.z> RELEASE_TARGET=linux-x86_64`, `make release-artifacts RELEASE_VERSION=<x.y.z> RELEASE_TARGET=source RELEASE_KIND=source` |
| **Failure action** | Fix packaging/integrity generation before publishing; do not release partial asset sets. |

## 2. Supplementary Enforcement Gates

These gates support the 9 mandatory gates above with additional static analysis
and documentation enforcement.

| Gate ID | Makefile Target | CI Job | Script | Purpose |
|---------|----------------|--------|--------|---------|
| `GATE-FORMAT` | `format-check` | `check` | (inline clang-format) | Source formatting consistency |
| `GATE-LINT` | `lint` | `check` | (inline cppcheck/clang-tidy) | Static analysis warnings |
| `GATE-LINT-DOCS` | `lint-docs` | `check` | `tools/ci/check_api_docs.sh` | Public API documentation coverage |
| `GATE-LINT-CHECKPOINT` | `lint-checkpoint` | `check` | `tools/ci/check_checkpoint_coverage.sh` | Kernel loop checkpoint coverage |
| `GATE-LINT-EVIDENCE` | `lint-evidence` | `check` | `tools/ci/check_evidence_linkage.sh` | Per-bead evidence linkage validation |
| `GATE-STATIC-ANALYSIS` | `lint-static-analysis` | `check` | `tools/ci/run_static_analysis.sh` | Section 10.7 deep static analysis |
| `GATE-MODEL-CHECK` | `model-check` | `check` | `tests/invariant/model_check/test_bounded_model.c` | Bounded state machine verification |
| `GATE-UNIT` | `test-unit` | `unit-invariant` | (compiled test binaries) | Module-level correctness |
| `GATE-INVARIANT` | `test-invariants` | `unit-invariant` | (compiled test binaries) | Lifecycle/quiescence invariants |
| `GATE-CONFORMANCE` | `conformance` | `conformance` | `tools/ci/run_conformance.sh` | Rust fixture parity |
| `GATE-CODEC` | `codec-equivalence` | `conformance` | `tools/ci/run_codec_equivalence.sh` | JSON/BIN codec equivalence |
| `GATE-PARALLEL-MEMORY-MODEL` | `formal-litmus`, `formal-codegen`, `formal-check` | `check` | `tests/formal/litmus/test_memory_model_litmus.c`, `tools/ci/check_codegen_stability.sh` | Parallel-runtime atomic publication, queue ownership, seqlock/EBR, trace-ordering, and bounded scheduler proofs |
| `GATE-PARALLEL-PARITY` | `parallel-parity` | `profile-parity` | `tools/ci/run_parallel_parity.sh` | Single-vs-multi-worker semantic digest and event-summary parity |
| `GATE-PARALLEL-TELEMETRY` | `test-unit`, `parallel-parity`, `bench-json` | `unit-invariant`, `profile-parity`, `perf-tail-deadline` | `tests/unit/runtime/test_parallel.c`, `tools/ci/run_parallel_parity.sh`, `tests/bench/bench_runtime.c` | Large-swarm pressure telemetry, admission evidence, and benchmark JSON artifacts |

### 2.1 Parallel Graduation Gate

`bd-pweu.1` defines the activation contract for the production parallel runtime
program. `bd-pweu.11` activates the single-vs-multi-worker parity gate that
child beads must satisfy before claiming that the parallel profile has graduated
beyond walking-skeleton behavior. `bd-pweu.10` activates the large-swarm
telemetry and deterministic admission evidence surface for diagnosing pressure
without hidden semantic drift. `bd-pweu.13` pins the parallel memory-model proof
surface: atomics, metadata publication/reclamation, trace commit ordering, and
bounded scheduler state are covered by formal litmus and codegen gates.

| Gate ID | Makefile Target | CI Job | Tracking Bead | Purpose |
|---------|-----------------|--------|---------------|---------|
| `GATE-PARALLEL-MEMORY-MODEL` | `formal-litmus`, `formal-codegen`, `formal-check` | `check` | `bd-pweu.13` | Prove portable atomic publication, two-phase queue slot ownership, seqlock/EBR metadata safety, gap-free trace commit tickets, bounded work stealing, cancel fairness, and shutdown/drain state reporting. |
| `GATE-PARALLEL-PARITY` | `parallel-parity` | `profile-parity` | `bd-pweu.11` | Compare canonical semantic digests and structured event summaries for the same scenarios under worker_count=1, 2, 8, and 64. |
| `GATE-PARALLEL-TELEMETRY` | `test-unit`, `parallel-parity`, `bench-json` | `unit-invariant`, `profile-parity`, `perf-tail-deadline` | `bd-pweu.10` | Validate pressure snapshots, deterministic admission decisions, JSONL rendering, and benchmark JSON telemetry for large-swarm runs. |

Coverage requirements:

1. The gate must include lifecycle, cancellation, timer/waker, MPSC contention,
   blocking-pool, and reactor-readiness scenarios.
2. The gate must fail on semantic digest drift, unclassified event-order drift,
   silent drops, stale-handle mutation, weakened cancellation, or unbounded
   queue growth.
3. The gate must emit machine-readable reports under the existing conformance
   artifact layout, including seed, worker count, profile, codec, digest, and
   rerun command.
4. Memory-model proof evidence must cover release/acquire publication, queue
   slot single-owner terminal CAS, seqlock mid-write rejection, EBR stale
   generation and reader grace-period behavior, gap-free trace commit fetch-add,
   bounded work-steal/cancel fairness, and shutdown/drain budget exhaustion.
5. `formal-litmus` must build and run both `ASX_LOCKFREE_SINGLE_THREAD=1` and
   `ASX_LOCKFREE_SINGLE_THREAD=0` lanes, and `formal-codegen` must confirm that
   the relevant semantic helpers remain optimization-stable.
6. Telemetry evidence must include queue pressure, worker queue depth, steal
   attempts/success/failure, cancel streaks, timed wake latency, reactor/waker
   readiness, blocking backlog, and the deterministic admission decision.
7. Admission policy defaults to observe-only; enforced reject/backpressure must
   be explicit and failure-atomic.
8. Expensive local proof runs must be executed via `rch exec -- make ...`.

The large-swarm e2e pack is `tests/e2e/parallel_swarm.sh`. Its default
`ASX_E2E_PARALLEL_SCALE=smoke` lane is suitable for CI. Nightly/profile runs
use the same script with `ASX_E2E_PARALLEL_SCALE=large` and may raise
`ASX_E2E_PARALLEL_WORKERS` up to the compiled profile cap.

## 3. E2E Gate IDs

E2E gates are assigned by `tests/e2e/run_all.sh` and map to deployment hardening
scenario packs (see `docs/DEPLOYMENT_HARDENING.md`).

| Gate ID | Script(s) | Profile |
|---------|-----------|---------|
| `GATE-E2E-LIFECYCLE` | `core_lifecycle.sh` | CORE |
| `GATE-E2E-CODEC` | `codec_parity.sh` | CORE |
| `GATE-E2E-ROBUSTNESS` | `robustness.sh`, `robustness_fault.sh`, `robustness_endian.sh`, `robustness_exhaustion.sh` | CORE |
| `GATE-E2E-VERTICAL-HFT` | `hft_microburst.sh` | HFT |
| `GATE-E2E-VERTICAL-AUTO` | `automotive_watchdog.sh` | AUTOMOTIVE |
| `GATE-E2E-CONTINUITY` | `continuity.sh`, `continuity_restart.sh` | CORE |
| `GATE-E2E-PARALLEL-SWARM` | `parallel_swarm.sh` | PARALLEL |
| `GATE-E2E-DEPLOY-ROUTER` | `router_storm.sh` | EMBEDDED_ROUTER |
| `GATE-E2E-DEPLOY-HFT` | `market_open_burst.sh` | HFT |
| `GATE-E2E-DEPLOY-AUTO` | `automotive_fault_burst.sh` | AUTOMOTIVE |
| `GATE-E2E-PACKAGE` | `openwrt_package.sh` | EMBEDDED_ROUTER |

## 4. Aggregation Targets

| Target | Scope | Gates Included |
|--------|-------|----------------|
| `make check` | Local PR gate | FORMAT, LINT, LINT-DOCS, LINT-CHECKPOINT, GATE-SEM-DELTA (anti-butchering), LINT-EVIDENCE, STATIC-ANALYSIS, PORT (build), UNIT, INVARIANT, MODEL-CHECK, FORMAL-CHECK, PARALLEL-MEMORY-MODEL |
| `make check-ci` | Full CI gate (`CI=1`) | FORMAT, LINT, LINT-CHECKPOINT, GATE-SEM-DELTA (anti-butchering), LINT-EVIDENCE, STATIC-ANALYSIS, PORT (build), UNIT, INVARIANT, MODEL-CHECK, E2E-VERTICAL, CONFORMANCE, CODEC, PROFILE, PARALLEL-PARITY, PARALLEL-TELEMETRY, FUZZ, EMBED |
| `make test-e2e-suite` | Unified E2E manifest | All GATE-E2E-* gates |
| `make test-e2e-parallel` | Parallel swarm e2e pack | GATE-E2E-PARALLEL-SWARM |

## 5. CI Workflow Jobs

From `.github/workflows/ci.yml`:

| Job | Trigger | Dependencies | Blocking |
|-----|---------|-------------|----------|
| `check` | PR, push | — | Yes |
| `unit-invariant` | PR, push | `check` | Yes |
| `e2e` | PR, push | `check` | Yes |
| `conformance` | PR, push | `check` | Yes |
| `profile-parity` | PR, push | `check` | Yes |
| `fuzz-parity` | PR, push | `check` | Yes |
| `compiler-matrix` | PR, push | `check` | Yes |
| `embedded-matrix` | PR, push | `check` | Yes |

From `.github/workflows/perf.yml`:

| Job | Trigger | Dependencies | Blocking |
|-----|---------|-------------|----------|
| `perf-tail-deadline` | push to main | — | Warn (threshold enforcement planned) |

From `.github/workflows/release.yml`:

| Job | Trigger | Dependencies | Blocking |
|-----|---------|-------------|----------|
| `release-artifacts` | push tag `v*` | — | Yes |

## 6. Enforcement Rules

Per `docs/WAVE_GATING_PROTOCOL.md`:

1. **Binary pass/fail** — no soft-pass. Gate either passes or blocks.
2. **Semantic delta budget: 0** — any non-zero delta fails unless fixture-scoped exception approved.
3. **Evidence bundle completeness** — wave close requires unit + invariant + E2E + conformance + parity + log manifests.
4. **Automatic block conditions**:
   - Required artifact missing
   - Parity/conformance drift unclassified
   - Deferred surface started without `DS-*` registration
   - Decision-dependent work missing `DEC-*` linkage

## 7. Known Gaps

| Gap | Status | Tracking |
|-----|--------|----------|
| HFT p99/jitter numeric SLO enforcement in perf.yml | Done | HFT baselines in `slo_baselines.json`, `--strict` gate in `perf.yml` (bd-39o) |
| MSVC build integration | Stub only | `build-msvc` prints SKIP; requires `cl.exe` on PATH |
| Real-device smoke run for embedded | Manual | One device smoke required before milestone closure per plan |
| Machine-readable traceability export | Done | `make traceability-export` generates `build/traceability/traceability_index.json` (bd-3gn) |

## 8. Cross-References

| Document | Relationship |
|----------|-------------|
| `AGENTS.md` section 10.6 | Source of the 9 mandatory gates |
| `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` section 10.6 | Detailed gate specifications |
| `docs/WAVE_GATING_PROTOCOL.md` | Procedural enforcement rules |
| `docs/PLAN_EXECUTION_TRACEABILITY_INDEX.md` | Plan → bead → test → artifact tracing |
| `docs/DEPLOYMENT_HARDENING.md` | Operator runbooks for E2E deployment packs |
| `docs/GUARANTEE_SUBSTITUTION_MATRIX.md` section 4 | Anti-butchering enforcement contract |
| `docs/PROOF_BLOCK_EXCEPTION_WORKFLOW.md` | Exception process for semantic delta |
| `docs/FEATURE_PARITY.md` | Semantic unit parity scoreboard |
