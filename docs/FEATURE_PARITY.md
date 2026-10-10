# Feature Parity Matrix (Rust Reference -> ANSI C)

> **Bead:** `bd-296.3`
> **Status:** Semantic-unit parity tracker with acceptance-test mapping
> **Last updated:** 2026-10-10 (bd-9kll.1.8) - status cells re-derived from evidence; retired the 2026-03-12 conformance claim

This matrix tracks canonical semantic units, their source-of-truth extraction status, and required acceptance coverage before parity can be declared complete.

## 1. Status Legend

- `spec-reviewed`: canonical semantics extracted and reviewed for implementation use.
- `spec-drafted`: semantics extracted but not fully reviewed/locked.
- `impl-pending`: implementation not yet landed.
- `impl-in-progress`: implementation work has started.
- `impl-complete`: implementation landed and its unit/invariant tests pass
  (CI job "unit + invariant + vignettes"). Rust parity of the unit is not
  checked by an executed fixture unless the cell says so.
- `conformance-passed`: named fixtures in `fixtures/rust_reference_v2`
  exercise the unit and pass `make conformance`, which executes each
  fixture's scenario through the C runtime and compares trace, snapshot,
  observations and lab dispatch order with the Rust `LabRuntime` capture
  (`tools/conformance/runner.c:7-12`). The cell names the fixtures and any
  part of the unit they do not cover.

Evidence cited in status cells: unit test files under `tests/`, fixture names
under `fixtures/rust_reference_v2/`, and CI run 38051289151 (all jobs passed
at commit `cd69958`; see section 5).

## 2. Canonical Unit Matrix

| Unit ID | Semantic Unit | Canonical Source Artifacts | Acceptance Test Obligations | Must-Fail / Negative Obligations | Current Status | Primary Bead(s) |
|---|---|---|---|---|---|---|
| `U-REG-TRANSITIONS` | Region lifecycle legality (`Open->...->Closed`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` | region transition unit + invariant suites; close/drain/finalize scenarios | invalid regressions/skips return `ASX_E_INVALID_TRANSITION` | `conformance-passed` for the close/drain progression: `region-lifecycle-close-cancels-children-001`, `region-lifecycle-draining-with-child-region-001`, `region-lifecycle-cancel-propagates-001`. Illegal-transition rejection is unit-tested only (`tests/unit/core/test_transition.c`, `tests/invariant/lifecycle/test_lifecycle_legality.c`) | `bd-296.15`, `bd-hwb.3` |
| `U-TASK-TRANSITIONS` | Task phase semantics + cancel checkpoints | `docs/LIFECYCLE_TRANSITION_TABLES.md` | task lifecycle fixtures; checkpoint/cancel observation tests | illegal backward/skip transitions rejected | `conformance-passed`: `task-lifecycle-spawn-join-001`, `cancel-masked-checkpoint-001`, `task-abort-next-step-001`. Illegal transitions are unit-tested only (`tests/unit/core/test_transition.c`) | `bd-296.15`, `bd-hwb.3` |
| `U-OBLIGATION-LINEARITY` | Reserve/commit/abort/leak exactly-once contract | `docs/LIFECYCLE_TRANSITION_TABLES.md`, `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | obligation lifecycle unit tests; leak-detection invariant tests | double resolve fails; unresolved finalization is surfaced | `conformance-passed`: `obligation-reserve-commit-001`, `obligation-abort-reasons-001`, `obligation-drop-leaks-at-once-001`. Double resolve is unit-tested only (`tests/unit/core/test_obligation.c:53`) | `bd-296.15`, `bd-296.18`, `bd-hwb.6` |
| `U-CANCEL-WITNESS` | Cancellation witness phase/rank/severity monotonicity | `docs/LIFECYCLE_TRANSITION_TABLES.md` | cancel protocol fixtures incl. strengthen paths | phase regression and reason weakening fail deterministically | `conformance-passed` for reason strengthening: `cancel-strengthen-severity-001`, `task-abort-next-step-001`, `cancel-request-event-reason-001`. Witness phase/rank monotonicity is unit-tested only (`tests/unit/core/test_cancel.c`) | `bd-296.15`, `bd-hwb.2` |
| `U-OUTCOME-LATTICE` | Outcome order and join semantics | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | algebraic law fixtures; aggregation tests | severity/order violations are test-fail regressions | `impl-complete`: join and severity order unit-tested (`tests/unit/core/test_outcome.c:38-53`). `task-lifecycle-err-outcome-001` and `task-lifecycle-panic-001` check Err and Panicked outcomes against Rust; no fixture checks the lattice join, so its Rust parity is not fixture-checked | `bd-296.16`, `bd-hwb.1` |
| `U-BUDGET-ALGEBRA` | Budget meet/combine and identity/absorbing behavior | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | budget law fixtures + edge-case tests | non-tightening combine and identity drift are failures | `impl-complete`: meet laws unit-tested (`tests/unit/core/test_budget.c:30-117`). Rust parity not fixture-checked: no v2 fixture targets meet, and the legacy `budget-meet-identity-010` was recorded as a user trace by the capture tool (`docs/RUST_FIXTURE_CAPTURE_TOOLING.md` section 4) | `bd-296.16`, `bd-hwb.1` |
| `U-EXHAUSTION-SEMANTICS` | Deadline/poll/cost exhaustion with failure-atomic semantics | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | exhaustion + rollback boundary tests | partial mutation on failure is forbidden | `conformance-passed` for poll-quota and deadline exhaustion: `budget-poll-quota-exhaustion-001`, `budget-deadline-sleep-checkpoint-001`, `task-groups-quorum-poll-quota-001`. Cost exhaustion and failure-atomic rollback are tested in C only (`tests/unit/runtime/test_boundary_exhaustion.c`, `tests/conformance/resource_pressure_failure_atomic_test.c`) | `bd-296.16`, `bd-hwb.6` |
| `U-CHANNEL-TWOPHASE` | MPSC reserve/send/abort/drop linearity | `docs/CHANNEL_TIMER_DETERMINISM.md` | channel kernel fixtures (`two-phase`, abort/drop release) | queue-jump, phantom waiter, reserved-slot theft forbidden | `conformance-passed`: `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001`, `lab-dispatch-mpsc-disconnect-order-001`. Permit abort/drop release is unit-tested only (`tests/unit/channel/test_mpsc.c`); no v2 scenario uses `permit_abort` | `bd-296.17`, `bd-2cw.5` |
| `U-CHANNEL-FAIRNESS` | FIFO waiter discipline and try-reserve gating | `docs/CHANNEL_TIMER_DETERMINISM.md` | fairness/backpressure scenario tests | bypassing queued waiter forbidden | `impl-complete`: `tests/unit/channel/test_channel_wake.c:313` (reserve waiters served in arrival order), `:334` (try_reserve never jumps a parked producer). Rust parity not fixture-checked | `bd-296.17`, `bd-2cw.5` |
| `U-TIMER-ORDERING` | Equal-deadline insertion-stable ordering | `docs/CHANNEL_TIMER_DETERMINISM.md` | timer ordering fixtures + replay checks | nondeterministic equal-deadline order forbidden | `conformance-passed`: `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` | `bd-296.17`, `bd-2cw.4` |
| `U-TIMER-HANDLE-SAFETY` | Generation-safe cancel handles | `docs/CHANNEL_TIMER_DETERMINISM.md` | stale-generation cancel tests | stale cancel mutating live timer state forbidden | `impl-complete`: `tests/unit/time/test_timer_wheel.c:136`, `:440`, `:521` (stale handles, generation reuse). Rust parity not fixture-checked | `bd-296.17`, `bd-2cw.4` |
| `U-FINALIZATION-QUIESCENCE` | Quiescence criteria + finalization exit contract | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | close/quiescence invariants + shutdown scenarios | quiescence success with pending work forbidden | `conformance-passed`: `quiescence-pending-timer-001`, `region-lifecycle-close-cancels-children-001`, `budget-cancel-unacknowledged-stays-running-001` (a run that must end non-quiescent) | `bd-296.18`, `bd-2cw.6` |
| `U-LEAK-DETECTION` | Deterministic unresolved-obligation leak surfacing | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | leak fixtures + finalization diagnostics | silent leak and unresolved-close acceptance forbidden | `conformance-passed`: `leak-policy-leak-reported-001`, `obligation-drop-leaks-at-once-001`, `obligation-cancelled-holder-leaks-001` | `bd-296.18`, `bd-hwb.6` |
| `U-FORBIDDEN-CATALOG` | Explicit must-fail semantic catalog + IDs | `docs/FORBIDDEN_BEHAVIOR_CATALOG.md`, `docs/LIFECYCLE_TRANSITION_TABLES.md` | forbidden-path fixture family generation | missing expected failure outcome is parity break | `spec-reviewed`: the catalog exists (`docs/FORBIDDEN_BEHAVIOR_CATALOG.md`, 22 `FB-<area>-NNN` IDs; `docs/LIFECYCLE_TRANSITION_TABLES.md:555-569`, `FB-001`..`FB-015`), but no fixture, test or tool references these IDs (grep of `src`, `tests`, `tools`, `fixtures`, `schemas`), so the forbidden-path fixture family was not generated. One v2 must-fail fixture exists: `obligation-reserve-closed-region-must-fail-001` | `bd-296.5` |
| `U-INVARIANT-SCHEMA` | Machine-readable transition/invariant schema | `schemas/invariant_schema.json`, `docs/INVARIANT_SCHEMA.md` | schema validation + generated legality tests | schema drift without fixture updates forbidden | `impl-complete`: `make formal-tv` runs `tools/ci/check_translation_validation.sh --strict` against `schemas/invariant_schema.json` (`Makefile:1281-1284`), as part of `make formal-check` in the CI unit job (`.github/workflows/ci.yml:236-237`). Rust parity: not applicable | `bd-296.4` |
| `U-PROVENANCE-MAP` | Source->fixture->parity traceability mapping | `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` | provenance integrity checks in CI docs/reporting | orphan fixture rows forbidden | `spec-reviewed`: truth pass 2026-10-10 found 4 of 38 rows mapped, 20 partial, 14 unmapped (see that document, section 6). Read by `tools/ci/generate_traceability_index.sh`; no CI check validates it | `bd-296.19` |
| `U-ARCH-LAYERING` | C architecture boundaries from semantics | `docs/PROPOSED_ANSI_C_ARCHITECTURE.md` | architecture conformance review + scaffold checks | transliteration-only or hidden coupling forbidden | `spec-reviewed`: approved in `docs/PHASE1_SPEC_REVIEW_GATE.md` section 2.11. No automated layering check was found in `Makefile` or `tools/ci` | `bd-296.2`, `bd-ix8.1` |
| `U-C-PORTABILITY-UB` | Portable C subset + UB elimination policy | `docs/C_PORTABILITY_RULES.md` | compiler/static-analysis + UB-focused tests | UB-prone constructs in core forbidden | `impl-complete`: CI run 38051289151 passed the gcc/clang/clang-21 compiler matrix over CORE, POSIX, FREESTANDING and EMBEDDED_ROUTER, MSVC (CORE, WIN32), -m32, cross + QEMU (aarch64, armv7, riscv64, mips, mipsel) and the asan, asan-live and tsan jobs. Rust parity: not applicable | `bd-296.29`, `bd-66l.10` |
| `U-GUARANTEE-SUBSTITUTION` | Rust guarantee -> C mechanism mapping | `docs/GUARANTEE_SUBSTITUTION_MATRIX.md` | proof artifact checklist + fixture links | missing substitution evidence forbidden | `impl-complete`: `make lint-anti-butchering` (`tools/ci/check_anti_butchering.sh`, CI `.github/workflows/ci.yml:79`) fails a change to semantic-sensitive paths that lacks a Guarantee Impact block. Rust parity: not applicable | `bd-296.6` |
| `U-CODEC-EQUIVALENCE` | JSON/BIN canonical semantic equivalence | `tools/ci/run_conformance.sh`, `schemas/canonical_fixture.schema.json` | codec-equivalence conformance suite | semantic drift between codecs forbidden | `impl-complete`: codec round trips unit-tested (`tests/unit/runtime/test_codec_equivalence.c`, `tests/conformance/codec_equivalence_conformance_test.c`). The CI `codec-equivalence` step compares digests recorded in the legacy `fixtures/rust_reference` files without executing the C runtime (33 comparable records, 0 fail in run 38051289151). Rust parity: not applicable (C JSON vs BIN) | `bd-2n0.1`, `bd-1md.11`, `bd-j4m.1` |
| `U-PROFILE-PARITY` | Cross-profile canonical digest equivalence | `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md`, `docs/HFT_PROFILE.md`, `docs/AUTOMOTIVE_PROFILE.md`, `tools/ci/run_conformance.sh` | profile parity suite across shared fixtures + vertical fixture lanes (`E2E-VERT-HFT`, `E2E-VERT-AUTO`) | profile-only semantic forks and hidden degraded-mode behavior forbidden | `conformance-passed`: every v2 fixture passes `make conformance` built with `PROFILE=EMBEDDED_ROUTER` under QEMU on aarch64, armv7, riscv64, mips and mipsel (`.github/workflows/ci.yml:938-941`) and with -m32 (`ci.yml:1037-1038`); the aarch64, mips and -m32 job logs of run 38051289151 each show 62 `PASS` lines. The `profile-parity` step itself compares digests recorded in legacy fixtures without executing the C runtime | `bd-1md.15`, `bd-j4m.7`, `bd-1md.10`, `bd-j4m.1` |
| `U-REPLAY-CONTINUITY` | Deterministic replay + continuity under restart | `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md` + trace/replay docs | replay identity and crash/restart continuity tests (`E2E-CONT-RESTART`) | digest mismatch or duplicated side effects on restart forbidden | `impl-complete`: `tests/unit/runtime/test_replay.c`, `tests/unit/runtime/test_continuity.c`, e2e `tests/e2e/continuity_restart.sh` (`E2E-CONT-RESTART`). Rust parity: not applicable (C replay of C runs) | `bd-1md.15`, `bd-2n0.4`, `bd-j4m.8` |
| `U-PARALLEL-PROFILE` | Optional parallel worker-lane semantics and single-vs-multi-worker digest parity | `docs/DEFERRED_SURFACE_REGISTER.md`, `docs/QUALITY_GATES.md`, `include/asx/runtime/parallel.h` | `parallel-parity`; `test-e2e-parallel`; `formal-litmus`; `formal-codegen`; large-swarm telemetry/admission/locality tests; `parallel-bench-json` | semantic digest drift, unclassified event-order drift, silent drops, unbounded queues, data races, stale-handle mutation, locality routing semantic drift, and weakened cancellation forbidden | `impl-complete`: `make parallel-parity` compares single- and multi-worker digests of C runs (run 38051289151: status=pass, 28 records, 21 compared). `src/runtime/parallel.c` creates no threads, so worker lanes are logical and run on the calling thread (see `docs/DEFERRED_STUBS_REGISTER.md`). Rust parity not fixture-checked | `bd-pweu.1`, `bd-pweu.7`, `bd-pweu.9`, `bd-pweu.10`, `bd-pweu.11`, `bd-pweu.12`, `bd-pweu.13`, `bd-pweu.14` |

### 2.1 Wave C Activation Matrix

These rows are post-kernel activation contracts created by `bd-v12u.1`. They
do not change the Phase 1 status count below until the named child beads land
the corresponding specs, tests, and implementation evidence.

| Unit ID | Semantic Unit | Canonical Source Artifacts | Acceptance Test Obligations | Must-Fail / Negative Obligations | Current Status | Primary Bead(s) |
|---|---|---|---|---|---|---|
| `U-NATIVE-ADAPTER-COMMIT` | POSIX/Win32 native hooks preserving deterministic commit authority | `docs/DEFERRED_SURFACE_REGISTER.md` DS-P01; `src/platform/posix/hooks.c`; `src/platform/win32/hooks.c`; `src/runtime/parallel.c` | native adapter parity/e2e tests; `test-win32-hooks-build`; `test-e2e-posix-adapter`; `parallel-parity`; `profile-parity` | native event-order drift, silent drops, unsupported live-mode claims, stale-handle mutation | `posix-readiness-e2e`: POSIX hooks (epoll on Linux, poll elsewhere) and `make test-e2e-posix-adapter` (`Makefile:1364`); Win32 has a clock, entropy and a timed wait but no readiness backend (`docs/DEFERRED_STUBS_REGISTER.md`). Rust parity not fixture-checked | `bd-v12u.2`, `bd-v12u.3`, `bd-v12u.4` |
| `U-STATIC-ARENA-BACKEND` | Static allocator backend preserving dynamic-backend semantics | `docs/DEFERRED_SURFACE_REGISTER.md` DS-S01; `docs/PROFILE_RESOURCE_CLASS_CAPABILITY_MATRIX.md` static arena contract; `include/asx/asx_config.h`; `src/runtime/hooks.c`; `src/runtime/resource.c`; `src/runtime/profile_compat.c` | static-vs-dynamic unit tests for hook install/seal/alloc/realloc/free; OOM rollback tests; resource-class default checks; `profile-parity`; `codec-equivalence` | invalid config mutating hooks, partial allocation on failure, post-seal allocation, free-after-seal regression, digest drift, over-capacity live handles, resource-class limits treated as semantic forks | `static-contract`. Rust parity: not applicable (resource plane) | `bd-v12u.5`, `bd-v12u.6` |
| `U-INCIDENT-REPLAY-EVIDENCE` | Operator incident bundle, trace schema, and minimized replay packet | `docs/INCIDENT_EVIDENCE_BUNDLE.md`; `schemas/incident_bundle.schema.json`; `fixtures/incident_bundles/*`; `docs/TRACE_EVENT_SCHEMA.md`; `schemas/trace_event.schema.json`; `fixtures/trace_events/*`; `docs/REPLAY_COUNTEREXAMPLE_MINIMIZER.md`; `schemas/replay_counterexample.schema.json`; `fixtures/replay_counterexamples/*`; trace/replay/lab docs; `docs/QUALITY_GATES.md` `GATE-INCIDENT-REPLAY-BUNDLE` | `test_evidence` incident bundle renderer tests; `test_trace` schema compatibility tests; `test_replay` class-preserving minimizer tests; `lint-schema-validation`; `test-e2e-parallel` incident JSONL emission; golden incident, trace, and replay-counterexample examples; minimizer self-tests | evidence artifacts mutating runtime state, unversioned schema drift, required/digest-visible trace-field drift, minimized failures changing class, resource failures minimizing into success, unsupported native behavior reported as live success | `replay-counterexample-v1`: golden examples `fixtures/incident_bundles/incident-bundle-v1-parallel-swarm.json`, `fixtures/trace_events/*.json`, `fixtures/replay_counterexamples/replay-counterexample-v1-resource.json`. Rust parity: not applicable | `bd-v12u.7`, `bd-v12u.8`, `bd-v12u.13` |
| `U-WAVE-C-NETWORKING` | Minimal bounded networking primitives over deterministic core surfaces and native reactor readiness | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C01; `include/asx/net/*.h`; `src/net/*`; `tests/e2e/network_surface.sh`; `tests/unit/net/test_net.c`; `tests/unit/net/test_pipe.c` | `test-e2e-network-surface`; focused net/pipe unit tests; `profile-parity`; bounded pipe-backed/reactor-readiness e2e artifacts in `bd-v12u.10` | kernel semantic drift, unsupported profiles passing live claims, Wave D protocol stacks counted as Wave C evidence, unbounded connection/resource growth | `network-e2e-evidence`: `make test-e2e-network-surface` (`Makefile:1368`). TLS is a plaintext passthrough (`src/net/tls.c:167-168` marks the handshake ready at once; reads and writes go straight to the TCP stream, `:212`, `:230`), QUIC connect and accept both create an already-CONNECTED slot (`src/net/quic.c:86-115`) and `asx_db_execute` returns an empty result for any query (`src/net/db.c:136-139`); see `docs/DEFERRED_STUBS_REGISTER.md`. Rust parity not fixture-checked | `bd-v12u.9`, `bd-v12u.10` |
| `U-COMBINATOR-ACTOR-HARNESS` | Combinator contracts plus actor/supervision semantic harnesses | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C02/DS-D06; `include/asx/core/combinator*.h`; `include/asx/core/join_set.h`; `include/asx/plan/plan.h`; `include/asx/service/service.h`; `src/core/combinator*.c`; `fixtures/rust_reference/core_combinator/*`; `src/actor/*`; `tests/e2e/actor_supervision.sh` | `test-combinator-contract`; `test-actor-supervision-harness`; `test-e2e-actor-supervision`; `make conformance`; `codec-equivalence`; `profile-parity` | cancellation weakening, outcome lattice drift, timeout-order drift, retry/budget overrun, leaked obligations, unsupported actor/supervision depth counted as complete | Combinators `conformance-passed`: `combinators-first-ok-001`, `combinators-race-timeout-001`, `task-groups-quorum-001`. GenServer `conformance-passed`: the 11 `actor-*` fixtures (cast, call, stop before start, full mailbox, batch yield, cancellation with the mailbox drain, a spawn in a closing region, a region close stopping a server; both interpreters run `server_spawn`/`cast`/`call`/`server_stop`, `bd-g652`). Supervision not fixture-checked: `supervision-transient-restart-001.json` has no Rust fixture because neither interpreter handles `supervise` (Rust supervises tasks, C's `asx_supervisor` supervises servers); C harness tests only (`make test-actor-supervision-harness`). `fixtures/rust_reference/core_combinator/*` is hand-authored and `make conformance` does not execute it | `bd-v12u.11`, `bd-v12u.12` |
| `U-OVERLOAD-SLO-DEMO` | Large-swarm SLO baselines and user-facing acceptance proof bundle | `docs/QUALITY_GATES.md` `GATE-OVERLOAD-SLO-DEMO`; `tools/ci/slo_baselines.json`; `tests/bench/bench_runtime.c`; `tests/e2e/wave_c_acceptance_demo.sh` | `parallel-bench-gate` with RCH worker-count rows, R1/R2/R3 footprint rows, warn/block SLO summary; `wave-c-acceptance-demo` with replay command, profile report, benchmark summary, incident evidence, expected-output manifest, and fail-closed unsupported-live diagnostic | SLO/admission changing semantics, stale README claims, unsupported platforms hidden as success | `acceptance-demo-gate`. Rust parity: not applicable | `bd-v12u.14`, `bd-v12u.15` |
| `U-RAPTORQ-RFC6330` | RFC 6330 RaptorQ systematic FEC codec (block encode/decode, OTI, source-block/sub-block partitioning) | RFC 6330; upstream `src/raptorq/{rfc6330,gf256,systematic,decoder}.rs`; `include/asx/raptorq/raptorq.h`; `src/raptorq/raptorq.c`; `docs/DEFERRED_SURFACE_REGISTER.md` DS-D05 | `tests/unit/raptorq/test_raptorq.c` golden vectors bit-identical to an independent RFC 6330 implementation (K = 1..8192, far/max ESIs, Z>1 objects with N>1 sub-blocks, OTI wire bytes); round trips with random loss, repair-only and K=8192 blocks; `tests/e2e/raptorq_erasure.sh` | rank-deficient or fewer-than-K symbol sets return `ASX_E_RESOURCE_EXHAUSTED` without writing output; duplicate ESIs, undersized workspaces/buffers and invalid OTI fields fail closed | `impl-complete`: golden vectors in `tests/unit/raptorq/test_raptorq.c` were produced by the crates.io `raptorq` 2.0.1 crate (file header). Rust parity with asupersync not fixture-checked | DS-D05 |

## 3. Phase 1 Exit Review Gate Checklist

A semantic unit may be marked Phase 1 complete only when:

1. Canonical source artifact is present and references pinned Rust baseline.
2. Unit row is at least `spec-reviewed`.
3. Acceptance obligations are mapped to concrete fixture/test families.
4. Must-fail obligations are explicit and deterministic.
5. Downstream implementation bead links are present.

## 4. Tracking Fields for Future CI Automation

Recommended machine-readable columns to derive later (CSV/JSON export):

- `unit_id`
- `status`
- `source_artifact_paths`
- `acceptance_fixture_ids`
- `forbidden_fixture_ids`
- `impl_bead_ids`
- `conformance_bead_ids`
- `last_reviewed_at`
- `reviewer`

## 5. Phase 1 Spec-Review Gate Sign-Off

**Reviewer:** BlueCat (claude-code/opus-4.6) — independent non-author reviewer
**Date:** 2026-02-27
**Review artifact:** `docs/PHASE1_SPEC_REVIEW_GATE.md`

All kernel-scope semantic units (U-REG-TRANSITIONS through U-GUARANTEE-SUBSTITUTION) have been independently reviewed and approved. Written rulings for 4 ambiguities documented in review artifact (WR-001 through WR-004). Phase 2 kickoff approved.

Unit status counts for the 23 rows of section 2 (2026-10-10, bd-9kll.1.8):

| Unit Status | Count |
|-------------|-------|
| `conformance-passed` | 10 (`U-REG-TRANSITIONS`, `U-TASK-TRANSITIONS`, `U-OBLIGATION-LINEARITY`, `U-CANCEL-WITNESS`, `U-EXHAUSTION-SEMANTICS`, `U-CHANNEL-TWOPHASE`, `U-TIMER-ORDERING`, `U-FINALIZATION-QUIESCENCE`, `U-LEAK-DETECTION`, `U-PROFILE-PARITY`; several cover only part of the unit, as their cells say) |
| `impl-complete` | 10 (`U-OUTCOME-LATTICE`, `U-BUDGET-ALGEBRA`, `U-CHANNEL-FAIRNESS`, `U-TIMER-HANDLE-SAFETY`, `U-INVARIANT-SCHEMA`, `U-C-PORTABILITY-UB`, `U-GUARANTEE-SUBSTITUTION`, `U-CODEC-EQUIVALENCE`, `U-REPLAY-CONTINUITY`, `U-PARALLEL-PROFILE`) |
| `spec-reviewed` | 3 (`U-FORBIDDEN-CATALOG`, `U-PROVENANCE-MAP`, `U-ARCH-LAYERING`) |
| **Total** | 23 |

**Status audit 2026-03-02 (WildCondor), historical:** the kernel units were
moved from `spec-reviewed` to `impl-complete` on the basis of unit, invariant,
formal and e2e gates. The suite and scenario counts quoted in that audit are
not re-verified here.

**Retired claim:** an earlier version of this section reported a "Rust-vs-C
conformance run 2026-03-12 ... 133/133 pass" over 66 Rust reference fixtures.
That run executed no scenario through the C runtime: it was 1 smoke record,
66 fixture metadata checks and 66 codec round trips over the legacy
`fixtures/rust_reference` corpus. In `tools/ci/run_conformance.sh` at the
commit that added the claim (`59f49f7`), the smoke record runs
`codec_json_baseline_test`, each fixture record checks metadata and
provenance, and each `fixture_replay` record decodes the fixture JSON and
calls `asx_codec_cross_codec_verify`. It is not evidence of Rust parity.

**Rust parity evidence (2026-10-10):**

- `make conformance` (`Makefile:1562-1572`) executes every fixture in
  `fixtures/rust_reference_v2` (captured from asupersync's `LabRuntime` at
  `5e60b1c4c` by `tools/twin_run`) through the C runtime
  (`build/bin/asx-conformance compare`) and compares trace, snapshot,
  observations and lab dispatch order with the Rust capture, canonical byte
  for byte; it fails unless every fixture matches
  (`tools/conformance/runner.c:7-12`). A negative control with one corrupted
  expected trace event runs first and must FAIL (`Makefile:1571`). At commit
  `cd69958` there were 62 such fixtures (`git ls-files
  'fixtures/rust_reference_v2/*.json' | wc -l`), and the CI "conformance" job
  of run 38051289151 printed 62 `PASS` lines. The same fixtures pass under
  QEMU (`PROFILE=EMBEDDED_ROUTER`) and -m32 in that run.
- `make fuzz-differential` (`Makefile:1590-1608`) generates DSL v2 scenarios
  with `twin_run generate`, captures them in Rust and compares the C run of
  each captured scenario the same way. CI job `fuzz-rust-differential` runs
  200 scenarios with seed 9 on every push (`.github/workflows/ci.yml:978-1009`);
  in run 38051289151 all 200 were captured and all 200 compared as `PASS`.
  The nightly workflow runs three further seeds of 200 scenarios each
  (`.github/workflows/nightly.yml:66-72`).
- Semantics without a passing v2 fixture are Rust-parity unchecked even when
  their C unit tests pass; the status cells above say which.

## 6. Immediate Follow-On Dependencies

This file directly feeds:

- `bd-296.30` (Phase 1 spec-review gate packet),
- `bd-296.5` (forbidden-behavior catalog and DSL),
- `bd-296.4` (invariant schema generation scope),
- `bd-1md.*` conformance/fuzz fixture planning.
