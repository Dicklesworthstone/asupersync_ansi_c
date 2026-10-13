# Feature Parity Matrix (Rust Reference -> ANSI C)

> **Bead:** `bd-296.3`
> **Status:** Semantic-unit parity tracker with acceptance-test mapping
> **Last updated:** 2026-10-12 (bd-9kll.3.4, bd-9kll.3.5, bd-9kll.3.19) - outcome-aware close/finish and synchronous mask guard; remaining Scope, finalizer, admission and liveness gaps retained

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

The 2026-10-11 lifecycle implementation adds public region finalizer
registration/unregistration and separate ordinary/cleanup spawn APIs, including
captured cleanup state. Ordinary admission is Open-only; cleanup admission is
Open or Finalizing, with the region task-limit exception only in Finalizing.
Unit tests exercise every state and both spawn forms, deferred admission,
cleanup-budget inheritance, failure atomicity, callback-before-ledger ordering,
exactly-once reentrant cleanup, and cleanup task execution through region close
(`tests/unit/runtime/test_quiescence.c`, `tests/unit/runtime/test_lab_dispatch.c`,
`tests/unit/runtime/test_budget_obligation.c`, `tests/unit/core/test_cleanup.c`).
These are C runtime tests. DSL v2 still cannot express finalizer registration or
cleanup spawning, so the existing lifecycle fixture cells do not establish
Rust parity for those additions. Separately, ten admission predicates are
captured by calling the pinned Rust `RegionState` methods and compared with the
actual C predicates by `make check-rust-constants`; planted ordinary/cleanup
Finalizing drift is rejected. This verifies the fixed admission policy, not
end-to-end finalizer execution. Region close outcomes now aggregate completed
task outcomes, and finalization audits unresolved obligations under the leak
policy after callbacks and cleanup tasks finish.

The bounded outcome-aware close increment adds
`asx_region_close_with_outcome` (`src/runtime/lifecycle.c:1170`),
`asx_region_wait_closed` (`src/runtime/quiescence.c:371`) and
`asx_scope_finish` (`src/cx/scope.c:203`).
The body's `OK` severity requests close without cancellation; `CANCELLED`
uses the supplied reason; `ERR` and `PANICKED` request FailFast, while
descendants receive ParentCancelled. The host-driven wait consumes its supplied
budget and advances the existing scheduler, synchronous finalizers, cleanup
tasks and leak audit without requesting cancellation. Scope finish uses the
scope's budget and reports the close/wait status. As in the pinned Rust
Scope body branch (`src/cx/scope.rs:877-932`), the body outcome selects close
and remains separate from the region's child-derived close outcome.
The first request is saved in the scope; a same-severity retry resumes the
wait without issuing another cancel, and a Cancelled retry may omit its reason.
`asx_region_drain` remains an explicit cancelling shutdown operation.

The direct C checks include
`tests/unit/runtime/test_quiescence.c::body_close_ok_preserves_child_value_and_outcome_without_cancel`,
`tests/unit/runtime/test_quiescence.c::body_close_wait_runs_lifo_then_cleanup_before_leak_audit`,
`tests/unit/cx/test_scope.c::scope_finish_exception_keeps_body_separate_from_child_outcome`
and `tests/unit/cx/test_scope.c::scope_finish_cancelled_retry_without_reason_does_not_cancel_again`.
These check normal child execution, finalizer/cleanup ordering, separate body
and child outcomes, and resumable cancellation without another request.
`tests/unit/cx/test_scope.c::scope_finish_finalizer_reentry_keeps_first_body_and_cancel_reason`
checks that the first body is bound before a cancellation finalizer reenters
finish, preventing another request or a changed body from replacing it.

Seven fresh linked-Rust captures exercise the primitive `finish_region` branch:
`region-body-ok-waits-001`, `region-body-ok-nested-001`,
`region-body-ok-parked-001`, `region-body-cancelled-reason-001`,
`region-body-err-fail-fast-001`, `region-body-panic-fail-fast-001` and
`region-body-closed-exception-001`.
The parked case waits for a sibling to notify before normal completion; the
direct C test separately checks a blocked wait and explicit retry. These
witnesses call Rust's actual RuntimeState/RegionRecord close primitives; they
do not invoke its typed Scope factory. The corrected C runner compares all
160 captured fixtures with 160 passes and no failures or errors, including
these seven primitive witnesses. The original 153 fixtures remain unchanged.
This establishes the captured body-branch behavior, not complete Scope or
async-finalizer parity; the broader K4 acceptance remains open.

This is a partial K4 increment. The genuine Scope child-region factory,
outcome payloads (`bd-9kll.3.14`), masked serialized async finalizers and their
budget escalation remain unproven or absent; original `bd-9kll.3.4` stays
open. No target-device or complete Scope claim follows from host-profile runs.
The admission fixture requirement remains open in `bd-9kll.3.5`.

Async finalizers (`bd-9kll.3.24`): `asx_region_defer_async` registers a
finalizer that the runtime spawns, once its region is Finalizing, as the
region's masked cleanup task, one at a time and last registered first,
with the finalizer budget; DSL v2 gains `regions[].finalizers`, and six
scenarios (order, the Finalizing-admission must-fail, poll and time budget
advisory, child before parent, masked under a region cancel) are written for
Rust capture. This is unit-tested C behavior, not parity: no Rust capture of
them exists yet, a sync callback still runs inline where Rust spawns a task,
and the region shutdown ceiling Rust's `BudgetedFinalizer` enforces has no C
counterpart.

The synchronous cancellation guard
`asx_task_masked` (`src/runtime/cancellation.c:755`) calls its body once within one admitted mask
level and releases that level on every normal C return, including an error or
`ASX_E_PENDING`. It returns the body's status unless release fails. Nested
guards preserve outer levels when additional raw masks are balanced; a pending
return keeps no guard across polls. A null body, stale or terminal handle, or
an attempt to add level 65 from an existing depth 64 runs no callback. Raw masks still need explicit release, and callbacks
must return normally; C promises neither `longjmp` cleanup nor Rust unwinding.
Both raw masks and this guard admit nesting after cancellation request or ACK,
following pinned Rust; formal rule #12 remains Variant.

The returning-body, nested-error, outer-raw-mask and admission boundaries are
checked by `tests/unit/runtime/test_cancellation.c`, including
`masked_callback_restores_on_success_error_and_pending_returns` and
`masked_callback_refuses_excess_depth_without_running_body`.
The real worker/joiner scenarios in `tests/e2e/e2e_nested_regions.c` show restored
nested guards, a raw-mask leak keeping cancel and join pending until explicit
release, and depth-65 refusal without callback or trace mutation. The DSL
interpreter uses the guard (`src/conformance/interpreter.c:2849`);
`cancel-masked-checkpoint-001` and `cancel-masked-nested-001` exercise balanced
normal scopes, not pending/error callback returns or depth 64.
`tests/gates/test_oracle_mutants.sh` checks omitted guard restoration against
the mask fixtures and excess-depth acceptance against the real public unit
boundary. Original `bd-9kll.3.19` stays open: these bounded witnesses do not
prove unconditional liveness, Rust unwinding or asynchronous finalizer barriers.

The interim K18 guard also rejects a second initialization before resetting
the active singleton. `conflicting_init_preserves_live_runtime_and_destination`
in `tests/unit/runtime/test_rt.c` verifies preserved runtime/destination bytes,
hooks, a live obligation and an actually executed task after the refusal.
Explicit teardown permits reinitialization. Independent runtime-owned storage
and cross-runtime handle isolation remain open in `bd-9kll.3.20`.

RB8 adds application/library build identity at runtime creation and its
config, hook and builder preparation calls. `ASX_CONFIG_FINGERPRINT` folds
effective resource limits, profile/codec/determinism/ghost settings and
bootstrap sizes, alignments and member offsets in the application translation
unit. ABI major 2 replaced unchecked creation symbols with fingerprint-taking
entries; incompatible builds return `ASX_E_CONFIG_MISMATCH` before object
access or singleton mutation. Matching consumers execute actual tasks through
explicit, default, environment and builder creation. Separately compiled
mismatched consumers retain their object bytes and canaries, while a matching
host retains and completes its task and resolves its reserved obligation
(`tests/abi/consumer_shim.c`, `make test-config-fingerprint`). This detects
accidental build mismatch; callers must check constructor errors before using
objects, and the fingerprint is not an authentication mechanism.

## 2. Canonical Unit Matrix

| Unit ID | Semantic Unit | Canonical Source Artifacts | Acceptance Test Obligations | Must-Fail / Negative Obligations | Current Status | Primary Bead(s) |
|---|---|---|---|---|---|---|
| `U-REG-TRANSITIONS` | Region lifecycle legality (`Open->...->Closed`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` | region transition unit + invariant suites; close/drain/finalize scenarios | invalid regressions/skips return `ASX_E_INVALID_TRANSITION` | `conformance-passed` for the close/drain progression: `region-lifecycle-close-cancels-children-001`, `region-lifecycle-draining-with-child-region-001`, `region-lifecycle-cancel-propagates-001`. Illegal-transition rejection is unit-tested only (`tests/unit/core/test_transition.c`, `tests/invariant/lifecycle/test_lifecycle_legality.c`) | `bd-296.15`, `bd-hwb.3` |
| `U-TASK-TRANSITIONS` | Task phase semantics + cancel checkpoints | `docs/LIFECYCLE_TRANSITION_TABLES.md` | task lifecycle fixtures; checkpoint/cancel observation and synchronous masked-callback tests | illegal backward/skip transitions rejected; a depth-65 guard invokes no callback; raw masks require explicit release | `conformance-passed`: `task-lifecycle-spawn-join-001`, `cancel-masked-checkpoint-001`, `cancel-masked-nested-001`, `task-abort-next-step-001`; cancel visibility (`Cx::is_cancel_requested`) by `cancel-is-requested-001`, `budget-cancel-is-requested-001`. Callback error/pending restoration and the level-64/65 boundary are direct C tests; unconditional liveness remains unproven. Illegal transitions are unit-tested only (`tests/unit/core/test_transition.c`) | `bd-296.15`, `bd-hwb.3`, `bd-9kll.3.19` |
| `U-OBLIGATION-LINEARITY` | Reserve/commit/abort/leak exactly-once contract | `docs/LIFECYCLE_TRANSITION_TABLES.md`, `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | obligation lifecycle unit tests; leak-detection invariant tests | double resolve fails; unresolved finalization is surfaced | `conformance-passed`: `obligation-reserve-commit-001`, `obligation-abort-reasons-001`, `obligation-drop-leaks-at-once-001`; refused registration still permits physical semaphore/reply settlement in `obligation-quota-refused-semaphore-permit-releases-001` and `obligation-quota-refused-call-reply-delivered-001`. Late-poll MPSC/oneshot/broadcast settlement, spent-copy rejection and broadcast credit exhaustion/reuse are direct C tests (`tests/unit/runtime/test_lab_dispatch.c`, `tests/unit/channel/test_broadcast.c`); full K6 remains partial | `bd-296.15`, `bd-296.18`, `bd-hwb.6`, `bd-6ncn` |
| `U-CANCEL-WITNESS` | Cancellation witness phase/rank/severity monotonicity | `docs/LIFECYCLE_TRANSITION_TABLES.md` | cancel protocol fixtures incl. strengthen paths | phase regression and reason weakening fail deterministically | `conformance-passed` for reason strengthening: `cancel-strengthen-severity-001`, `task-abort-next-step-001`, `cancel-request-event-reason-001`. Witness phase/rank monotonicity is unit-tested only (`tests/unit/core/test_cancel.c`) | `bd-296.15`, `bd-hwb.2` |
| `U-OUTCOME-LATTICE` | Outcome order and join semantics | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | algebraic law fixtures; aggregation tests | severity/order violations are test-fail regressions | `impl-complete`: join and severity order unit-tested (`tests/unit/core/test_outcome.c:38-53`). `task-lifecycle-err-outcome-001` and `task-lifecycle-panic-001` check Err and Panicked outcomes against Rust; no fixture checks the lattice join, so its Rust parity is not fixture-checked | `bd-296.16`, `bd-hwb.1` |
| `U-BUDGET-ALGEBRA` | Budget meet/combine and identity/absorbing behavior | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | budget law fixtures + edge-case tests | non-tightening combine and identity drift are failures | `impl-complete`: meet laws unit-tested (`tests/unit/core/test_budget.c:30-120`). Rust parity not fixture-checked: no v2 fixture targets meet, and the legacy `budget-meet-identity-010` was recorded as a user trace by the capture tool (`docs/RUST_FIXTURE_CAPTURE_TOOLING.md` section 4) | `bd-296.16`, `bd-hwb.1` |
| `U-EXHAUSTION-SEMANTICS` | Deadline/poll/cost exhaustion with failure-atomic semantics | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | exhaustion + rollback boundary tests | partial mutation on failure is forbidden | `conformance-passed` for poll-quota and deadline exhaustion: `budget-poll-quota-exhaustion-001`, `budget-deadline-sleep-checkpoint-001`, `task-groups-quorum-poll-quota-001`. Cost exhaustion and failure-atomic rollback are tested in C only (`tests/unit/runtime/test_boundary_exhaustion.c`, `tests/conformance/resource_pressure_failure_atomic_test.c`) | `bd-296.16`, `bd-hwb.6` |
| `U-CHANNEL-TWOPHASE` | MPSC reserve/send/abort/drop linearity | `docs/CHANNEL_TIMER_DETERMINISM.md` | channel kernel fixtures (`two-phase`, abort/drop release) | queue-jump, phantom waiter, reserved-slot theft forbidden | `conformance-passed`: `mpsc-two-phase-send-recv-001`, `mpsc-try-ops-001`, `lab-dispatch-mpsc-disconnect-order-001`. Permit abort/drop release is unit-tested only (`tests/unit/channel/test_mpsc.c`); no v2 scenario uses `permit_abort` | `bd-296.17`, `bd-2cw.5` |
| `U-CHANNEL-FAIRNESS` | FIFO waiter discipline and try-reserve gating | `docs/CHANNEL_TIMER_DETERMINISM.md` | fairness/backpressure scenario tests | bypassing queued waiter forbidden | `impl-complete`: `tests/unit/channel/test_channel_wake.c:313` (reserve waiters served in arrival order), `:334` (try_reserve never jumps a parked producer). Rust parity not fixture-checked | `bd-296.17`, `bd-2cw.5` |
| `U-TIMER-ORDERING` | Timers fire in the Rust wheel's order: 1 ms tick, then arrival at the slot (a port of `TimerWheel`, one structure for task sleeps and the application wheel) | `docs/CHANNEL_TIMER_DETERMINISM.md` | timer ordering fixtures + replay checks | nondeterministic or deadline-sorted order forbidden | `conformance-passed`: `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001`. Written, capture from Rust pending (bd-9kll.7.1): `timers-same-tick-registration-order-001`, `timers-cascade-order-001`, `timers-multi-level-storm-001`, `timers-beyond-wheel-horizon-001`; unit vectors in `tests/unit/time/test_timer_wheel.c` | `bd-296.17`, `bd-2cw.4`, `bd-9kll.7.1` |
| `U-TIMER-HANDLE-SAFETY` | Generation-safe cancel handles | `docs/CHANNEL_TIMER_DETERMINISM.md` | stale-generation cancel tests | stale cancel mutating live timer state forbidden | `impl-complete`: `tests/unit/time/test_timer_wheel.c` `timer_stale_handle_cancel_returns_false`, `timer_update_refuses_stale_handles_and_registers_nothing`, `timer_generation_increments_on_reuse` (stale handles, generation reuse). Rust parity not fixture-checked | `bd-296.17`, `bd-2cw.4` |
| `U-FINALIZATION-QUIESCENCE` | Quiescence criteria + finalization exit contract | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | close/quiescence invariants + shutdown scenarios | quiescence success with pending work forbidden | `conformance-passed`: `quiescence-pending-timer-001`, `region-lifecycle-close-cancels-children-001`, `budget-cancel-unacknowledged-stays-running-001` (a run that must end non-quiescent) | `bd-296.18`, `bd-2cw.6` |
| `U-LEAK-DETECTION` | Deterministic unresolved-obligation leak surfacing | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | leak fixtures + finalization diagnostics | silent leak and unresolved-close acceptance forbidden | `conformance-passed`: `leak-policy-leak-reported-001`, `obligation-drop-leaks-at-once-001`, `obligation-cancelled-holder-leaks-001` | `bd-296.18`, `bd-hwb.6` |
| `U-FORBIDDEN-CATALOG` | Explicit must-fail semantic catalog + IDs | `docs/FORBIDDEN_BEHAVIOR_CATALOG.md`, `docs/LIFECYCLE_TRANSITION_TABLES.md` | forbidden-path fixture family generation | missing expected failure outcome is parity break | `spec-reviewed`: the catalog exists (`docs/FORBIDDEN_BEHAVIOR_CATALOG.md`, 22 `FB-<area>-NNN` IDs; `docs/LIFECYCLE_TRANSITION_TABLES.md:649-663`, `FB-001`..`FB-015`), but no fixture, test or tool references these IDs (grep of `src`, `tests`, `tools`, `fixtures`, `schemas`), so the forbidden-path fixture family was not generated. One v2 must-fail fixture exists: `obligation-reserve-closed-region-must-fail-001` | `bd-296.5` |
| `U-INVARIANT-SCHEMA` | Machine-readable transition/invariant schema | `schemas/invariant_schema.json`, `docs/INVARIANT_SCHEMA.md` | schema validation + generated legality tests | schema drift without fixture updates forbidden | `impl-complete`: `make formal-tv` runs `tools/ci/check_translation_validation.sh --strict` against `schemas/invariant_schema.json` (`Makefile:1281-1284`), as part of `make formal-check` in the CI unit job (`.github/workflows/ci.yml:236-237`). Rust parity: not applicable | `bd-296.4` |
| `U-PROVENANCE-MAP` | Source->fixture->parity traceability mapping | `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` | provenance integrity checks in CI docs/reporting | orphan fixture rows forbidden | `spec-reviewed`: truth pass 2026-10-10 found 4 of 38 rows mapped, 20 partial, 14 unmapped (see that document, section 6). Read by `tools/ci/generate_traceability_index.sh`; no CI check validates it | `bd-296.19` |
| `U-ARCH-LAYERING` | C architecture boundaries from semantics | `docs/PROPOSED_ANSI_C_ARCHITECTURE.md` | architecture conformance review + scaffold checks | transliteration-only or hidden coupling forbidden | `spec-reviewed`: approved in `docs/PHASE1_SPEC_REVIEW_GATE.md` section 2.11. No automated layering check was found in `Makefile` or `tools/ci` | `bd-296.2`, `bd-ix8.1` |
| `U-C-PORTABILITY-UB` | Portable C subset + UB elimination policy | `docs/C_PORTABILITY_RULES.md` | compiler/static-analysis + UB-focused tests | UB-prone constructs in core forbidden | `impl-complete`: CI run 38051289151 passed the gcc/clang/clang-21 compiler matrix over CORE, POSIX, FREESTANDING and EMBEDDED_ROUTER, MSVC (CORE, WIN32), -m32, cross + QEMU (aarch64, armv7, riscv64, mips, mipsel) and the asan, asan-live and tsan jobs. Rust parity: not applicable | `bd-296.29`, `bd-66l.10` |
| `U-GUARANTEE-SUBSTITUTION` | Rust guarantee -> C mechanism mapping | `docs/GUARANTEE_SUBSTITUTION_MATRIX.md` | proof artifact checklist + fixture links | missing substitution evidence forbidden | `impl-complete`: `make lint-anti-butchering` (`tools/ci/check_anti_butchering.sh`, CI `.github/workflows/ci.yml:79`) fails a change to semantic-sensitive paths that lacks a Guarantee Impact block. Rust parity: not applicable | `bd-296.6` |
| `U-CODEC-EQUIVALENCE` | JSON/BIN canonical semantic equivalence | `tools/ci/run_conformance.sh`, `schemas/canonical_fixture.schema.json` | codec-equivalence conformance suite | semantic drift between codecs forbidden | `impl-complete`: codec round trips unit-tested (`tests/unit/runtime/test_codec_equivalence.c`, `tests/conformance/codec_equivalence_conformance_test.c`). The CI `codec-equivalence` step compares digests recorded in the legacy `fixtures/rust_reference` files without executing the C runtime (33 comparable records, 0 fail in run 38051289151). Rust parity: not applicable (C JSON vs BIN) | `bd-2n0.1`, `bd-1md.11`, `bd-j4m.1` |
| `U-PROFILE-PARITY` | Cross-profile canonical digest equivalence | `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md`, `docs/HFT_PROFILE.md`, `docs/AUTOMOTIVE_PROFILE.md`, `tools/ci/run_conformance.sh` | profile parity suite across shared fixtures + vertical fixture lanes (`E2E-VERT-HFT`, `E2E-VERT-AUTO`) | profile-only semantic forks and hidden degraded-mode behavior forbidden | `conformance-passed`: every v2 fixture passes `make conformance` built with `PROFILE=EMBEDDED_ROUTER` under QEMU on aarch64, armv7, riscv64, mips and mipsel (`.github/workflows/ci.yml:938-941`) and with -m32 (`ci.yml:1037-1038`); the aarch64, mips and -m32 job logs of run 38051289151 each show 62 `PASS` lines. Current `make profile-parity` first runs `profile-parity-exec` across eight native host profiles against all 160 captured fixtures, then checks legacy recorded digests (`Makefile:1945-1972`); these host runs supply no new target-device evidence | `bd-1md.15`, `bd-j4m.7`, `bd-1md.10`, `bd-j4m.1` |
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
| `U-COMBINATOR-ACTOR-HARNESS` | Combinator contracts plus actor/supervision semantic harnesses | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C02/DS-D06; `include/asx/core/combinator*.h`; `include/asx/core/join_set.h`; `include/asx/plan/plan.h`; `include/asx/service/service.h`; `src/core/combinator*.c`; `fixtures/rust_reference/core_combinator/*`; `src/actor/*`; `tests/e2e/actor_supervision.sh` | `test-combinator-contract`; `test-actor-supervision-harness`; `test-e2e-actor-supervision`; `make conformance`; `codec-equivalence`; `profile-parity` | cancellation weakening, outcome lattice drift, timeout-order drift, retry/budget overrun, leaked obligations, unsupported actor/supervision depth counted as complete | Combinators `conformance-passed`: `combinators-first-ok-001`, `combinators-race-timeout-001`, `task-groups-quorum-001`. GenServer `conformance-passed`: the 11 `actor-*` fixtures (cast, call, stop before start, full mailbox, batch yield, cancellation with the mailbox drain, a spawn in a closing region, a region close stopping a server; both interpreters run `server_spawn`/`cast`/`call`/`server_stop`, `bd-g652`). Managed supervision `conformance-passed`: the 16 `supervision-*` fixtures (the three strategies, the three restart modes, restart intensity (including restarts aging out of the window) and backoff, stop and escalate, a caught panic, an aborted controller, cancellation while children run and during the backoff, a transient generation cancelled with and without acknowledging; both interpreters run `supervise`/`supervisor_join`, `bd-g652`), and `make fuzz-differential` generates supervisors; unit tests `make test-actor-supervision-harness`. Restart storm detection (the report flag and the `restart_storm` trace; the e-process alert cannot fire before the intensity check, so it is not modelled): written, capture from Rust pending: the four `supervision-restart-storm-*` scenarios. Not ported: name registries, dynamic supervisors with shared restart domains, per-child shutdown budgets (`include/asx/actor/supervisor.h`). `fixtures/rust_reference/core_combinator/*` is hand-authored and `make conformance` does not execute it | `bd-v12u.11`, `bd-v12u.12` |
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
- `tests/gates/test_oracle_mutants.sh` (run by `make test-gates`) shows the
  corpus can fail: it builds the runner four times, each with one planted
  semantic bug (`ASX_MUTANT_*` in the source: budget meet keeps the lower
  priority, a cancel reason is never strengthened, the lab tie-break is
  mirrored, a dropped obligation token commits instead of leaking), and
  each must fail fixtures of its family (2026-10-11: 21, 11, 88 and 8
  fixtures fail). A mutant on the holder-completion path was not caught:
  the interpreter drops a task's remaining tokens itself before the task
  completes, so no fixture reaches `asx_task_resolve_held_obligations_internal`.
- Only untouched Rust captures are compared: `asx-conformance` refuses (as
  an ERROR) a fixture whose `provenance.producer` is not `tools/twin_run` or
  whose `semantic_digest` does not recompute from its own trace, snapshot
  and observations, so a fixture edited after capture cannot make C pass;
  `tests/gates/test_fixture_authenticity.sh` checks both refusals.
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
