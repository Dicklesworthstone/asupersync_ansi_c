# Source-to-Fixture Provenance Map

> **Bead:** bd-296.19 (original map); truth pass bd-9kll.1.8
> **Status:** Each row's `fixture_links` were checked against the repository on 2026-10-10. Most fixture IDs this map originally listed were never materialized; they are marked below, with the existing fixtures that cover the same behaviour where one does.
> **Rust baselines:** the rows were written against Rust commit `38c152405bd03e2bd9eecf178bfbbe9472fed861` (this document's previous baseline line). `docs/rust_baseline_inventory.json` records `a9e737d8` as the active source and `5e60b1c4c` as `pending_rebase`. The legacy fixtures in `fixtures/rust_reference/` carry `a9e737d8`; the fixtures in `fixtures/rust_reference_v2/` carry `5e60b1c4c`.
> **Last updated:** 2026-10-10 (bd-9kll.1.8)

This document maps extracted semantic rules to fixture families, parity rows, and test obligations.

## 1. Inputs and Scope

Extracted semantic artifacts:

- `bd-296.12`: `docs/RUST_BASELINE_PROVENANCE.md`, `docs/rust_baseline_inventory.json`
- `bd-296.15`: `docs/LIFECYCLE_TRANSITION_TABLES.md`
- `bd-296.16`: `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`
- `bd-296.17`: `docs/CHANNEL_TIMER_DETERMINISM.md`, `docs/CHANNEL_TIMER_SEMANTICS.md`
- `bd-296.18`: `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md`

Fixture sources in this repository:

- `fixtures/rust_reference_v2/*.json`: DSL v2 fixtures captured from asupersync's `LabRuntime` at `5e60b1c4c` by `tools/twin_run`. Each embeds its scenario (also in `tests/conformance/scenarios_v2/`). `make conformance` executes every one through the C runtime and compares trace, snapshot, observations and lab dispatch order with the Rust capture (`Makefile:1562-1572`, `tools/conformance/runner.c:7-12`). All 62 fixtures tracked at HEAD `cd69958` (`git ls-files 'fixtures/rust_reference_v2/*.json' | wc -l`) passed in CI run 38051289151 (job "conformance", 62 `PASS` lines).
- `fixtures/rust_reference/**`: legacy `fixture-v1` files (70 files). They were produced by `tools/fixture_capture`, whose ops mode records channel, timer and `budget_meet` ops as synthesized trace events instead of running Rust code (`docs/RUST_FIXTURE_CAPTURE_TOOLING.md` section 4). `make conformance` does not execute them; `make fixture-integrity` and `make codec-equivalence` check their schema, provenance and recorded digests without running the C runtime.
- `fixtures/trace_events/`, `fixtures/incident_bundles/`, `fixtures/replay_counterexamples/`: schema golden examples.

## 2. Row Contract

Each row tracks:

- `prov_id`: stable semantic-rule identifier.
- `semantic_unit`: human-readable unit name.
- `source_provenance`: canonical source artifacts (extracted docs, code).
- `fixture_links`: the fixture IDs originally listed, each annotated, followed by existing fixtures that cover the behaviour.
- `parity_targets`: parity dimensions the row is meant to reach (`rust_vs_c`, `codec_equivalence`, `profile_parity`). These are targets, not results.
- `test_obligations`: minimum test layers the row asks for.
- `status`: `mapped` (the listed fixtures exist), `partial` (the listed IDs are missing or legacy-only, and the named v2 fixtures cover part of the behaviour), or `unmapped` (no fixture in this repository covers the behaviour).

Annotations used in `fixture_links`:

- `(missing)`: no file under `fixtures/`, `tests/`, `tools/`, `src/` or `schemas/` has the ID in its name or content (`grep -rlF <id>` and `find -name '*<id>*'`, 2026-10-10).
- `(schema-only)`: the ID appears only as a `fixture_ids` entry in `schemas/invariant_schema.json`; no fixture file has it.
- `(catalog)`: a forbidden-behaviour ID defined in `docs/LIFECYCLE_TRANSITION_TABLES.md:555-569`; no fixture or test references it.
- `(Rust test)`: a test function in asupersync at `5e60b1c4c`, not a fixture in this repository.
- `(legacy)`: a file in `fixtures/rust_reference/` (see section 1 for what it does and does not show).
- `v2:` fixtures in `fixtures/rust_reference_v2/` that exercise the behaviour and pass `make conformance`.

## 3. Canonical Mapping Matrix

| prov_id | semantic_unit | source_provenance | fixture_links | parity_targets | test_obligations | status |
|---|---|---|---|---|---|---|
| `BASELINE-001` | Frozen Rust baseline identity | `docs/RUST_BASELINE_PROVENANCE.md`, `docs/rust_baseline_inventory.json` | provenance block in every fixture: v2 fixtures carry `rust_baseline_commit` (`5e60b1c4c`), toolchain fields and `cargo_lock_sha256` (written by `tools/twin_run/src/main.rs:70-84`); legacy fixtures carry the same fields at `a9e737d8` | `rust_vs_c` | conformance schema validation, provenance lint | mapped |
| `OUTCOME-001` | Outcome severity lattice (`Ok < Err < Cancelled < Panicked`) | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` section 2 | `outcome-lattice-001..007` (missing), `outcome-join-left-bias-001` (missing), `outcome-join-order-002` (missing), `outcome-join-top-003` (missing). v2: `task-lifecycle-err-outcome-001`, `task-lifecycle-panic-001` (Err and Panicked outcomes); no fixture checks the lattice join | `rust_vs_c`, `codec_equivalence`, `profile_parity` | unit + conformance | partial |
| `OUTCOME-002` | Equal-severity left-bias and cancel-strengthen behavior | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` sections 2.2/2.3/4.4 | `outcome-cancel-strengthen-004` (missing), `task-lifecycle-006` (schema-only), `task-lifecycle-007` (schema-only). v2: `cancel-strengthen-severity-001` (region reason strengthens User -> Shutdown), `task-abort-next-step-001` (two aborts coalesce to the stronger reason); equal-severity left bias is not covered | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | partial |
| `BUDGET-001` | Budget meet algebra and identities (`INFINITE`, `ZERO`) | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` section 3 | `budget-meet-identity-010` (legacy; the capture tool records `budget_meet` as a user trace and never calls `Budget::meet`, `tools/fixture_capture/src/main.rs:262-277`), `budget-meet-absorbing-011` (missing), `budget-deadline-none-012` (missing). No v2 fixture | `rust_vs_c`, `codec_equivalence` | unit + conformance | unmapped |
| `BUDGET-002` | Exhaustion checks and cancellation mapping | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` section 4 | `budget-consume-poll-013`, `budget-consume-cost-014`, `budget-deadline-boundary-015`, `runtime-pollquota-cancel-016`, `cancel-cleanup-budget-017` (all missing). v2: `budget-poll-quota-exhaustion-001` (poll quota -> PollQuota cancel), `budget-deadline-sleep-checkpoint-001` (deadline -> Deadline cancel at the next checkpoint), `task-groups-quorum-poll-quota-001`; cost quota is not covered | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | partial |
| `REGION-001` | Region close progression (`Open -> Closing -> Draining -> Finalizing -> Closed`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 1 | `region-lifecycle-001`, `region-lifecycle-002`, `region-lifecycle-009` (schema-only); `region-lifecycle-open-close-001`, `region-lifecycle-drain-finalize-002` (legacy). v2: `region-lifecycle-close-cancels-children-001`, `region-lifecycle-draining-with-child-region-001` | `rust_vs_c`, `profile_parity` | invariant + conformance + scenario | partial |
| `REGION-002` | Illegal region transitions and admission gate failures | `docs/LIFECYCLE_TRANSITION_TABLES.md` sections 1.3/1.4 | `region-lifecycle-005`, `region-lifecycle-006` (schema-only), `region-lifecycle-007` (missing), `FB-001`, `FB-002`, `FB-008` (catalog). v2: `obligation-reserve-closed-region-must-fail-001` (reserve in a Closing region refused), `spawn-into-cancelled-region-step-001`, `region-lifecycle-closed-child-spawn-001` (spawn into a closed region refused), `region-limits-admission-001` (spawn, child-region and reserve refused past region limits with `ASX_E_ADMISSION_LIMIT`) | `rust_vs_c`, `profile_parity` | unit + invariant | partial |
| `TASK-001` | Task lifecycle legality and cancel-phase progression | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 2 | `task-lifecycle-001..013` (schema-only), `cancel-protocol-001`, `cancel-protocol-002`, `cancel-protocol-010` (missing; the legacy corpus has `cancel-protocol-user-cancel-001` under a different ID). v2: `task-lifecycle-spawn-join-001`, `cancel-masked-checkpoint-001`, `budget-poll-quota-exhaustion-001` | `rust_vs_c`, `profile_parity` | invariant + conformance + scenario | partial |
| `OBLIGATION-001` | Obligation linearity (`Reserved -> Committed/Aborted/Leaked`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 3 | `obligation-lifecycle-001..003` (schema-only), `obligation-lifecycle-004..010` (missing; the legacy corpus has `obligation-lifecycle-reserve-commit-004` and `obligation-lifecycle-reserve-abort-005`), `FB-003..FB-006` (catalog). v2: `obligation-reserve-commit-001`, `obligation-abort-reasons-001`, `leak-policy-leak-reported-001`; double resolve is not covered by a fixture | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | partial |
| `HANDLE-001` | Stale-handle and generation-safety behavior | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 8, `docs/CHANNEL_TIMER_DETERMINISM.md` sections 3.1/3.2 | `FB-010` (catalog), `timer-cancel-generation-011` (missing); `timer-stale-cancel-rejected-003` (legacy; timer events synthesized by the capture tool). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + invariant | unmapped |
| `CHANNEL-001` | Two-phase MPSC reserve/send/abort linearity | `docs/CHANNEL_TIMER_DETERMINISM.md` sections 2.1/2.2 | `channel-two-phase-001`, `channel-abort-release-002`, `channel-drop-permit-abort-003` (missing); `channel-two-phase-send-recv-001` (legacy; channel ops recorded as user traces). v2: `mpsc-two-phase-send-recv-001` (`reserve_send` then `permit_send`); permit abort is not covered (the DSL v2 `permit_abort` op in `schemas/scenario_dsl_v2.json` is used by no scenario) | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | partial |
| `CHANNEL-002` | FIFO waiter discipline and queue-jump prevention | `docs/CHANNEL_TIMER_DETERMINISM.md` section 2.3 | `channel-fifo-waiter-004` (missing), `try_reserve_respects_fifo_over_capacity` (Rust test, `src/channel/mpsc_tests.rs:2404`); `channel-fifo-ordering-002` (legacy). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + stress + conformance | unmapped |
| `CHANNEL-003` | Cancellation/disconnect behavior without partial mutation | `docs/CHANNEL_TIMER_DETERMINISM.md` section 2.4 | `channel-reserve-cancel-005`, `channel-recv-cancel-nonconsume-006` (missing), `receiver_drop_unblocks_pending_reserve_without_leak` (Rust test, `src/channel/mpsc_tests.rs:2516`). v2: `mpsc-recv-cancel-first-001` (cancelled recv returns `ASX_E_CANCELLED`), `lab-dispatch-mpsc-disconnect-order-001` (send after receiver drop returns `ASX_E_DISCONNECTED`) | `rust_vs_c`, `profile_parity` | unit + invariant | partial |
| `CHANNEL-004` | Deterministic backpressure (`send_evict_oldest`) | `docs/CHANNEL_TIMER_DETERMINISM.md` section 2.5 | `channel-evict-reserved-007`, `channel-evict-committed-008` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + stress | unmapped |
| `TIMER-001` | Equal-deadline deterministic ordering | `docs/CHANNEL_TIMER_DETERMINISM.md` section 3.1 | `timer-equal-deadline-order-010` (missing), `same_deadline_pops_in_insertion_order` (Rust test, `src/runtime/timer.rs:336`); `timer-equal-deadline-ordering-002` (legacy). v2: `timers-same-deadline-001`, `lab-dispatch-timer-wheel-order-001` | `rust_vs_c`, `codec_equivalence`, `profile_parity` | unit + conformance | partial |
| `TIMER-002` | Timer register/cancel/fire and generation-safe handles | `docs/CHANNEL_TIMER_DETERMINISM.md` section 3.2 | `timer-cancel-generation-011` (missing), `wheel_cancel_prevents_fire` (Rust test, `src/time/wheel.rs:1359`), `wheel_cancel_rejects_generation_mismatch_without_removing` (Rust test, `src/time/wheel.rs:1377`); `timer-register-fire-001`, `timer-stale-cancel-rejected-003` (legacy). v2: `quiescence-pending-timer-001` (register and fire), `lab-dispatch-waker-rearm-001` (timer cancelled and re-registered); generation mismatch is not covered | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | partial |
| `TIMER-003` | `next_deadline` boundary and overflow promotion | `docs/CHANNEL_TIMER_DETERMINISM.md` sections 3.2/3.3 | `timer-next-deadline-same-tick-012`, `timer-overflow-promotion-013` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + conformance | unmapped |
| `TIMER-004` | Coalescing window determinism with threshold gating | `docs/CHANNEL_TIMER_DETERMINISM.md` section 3.3 | `timer-coalescing-threshold-014` (missing), `coalescing_min_group_size_enables_window_when_threshold_met` (Rust test, `src/time/wheel.rs:2153`). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + stress + conformance | unmapped |
| `QUIESCENCE-001` | Region close preconditions over tasks/obligations/regions/timers/channels | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 7, `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` sections 3 and 6 | `finalization-quiescence-001`, `finalization-quiescence-002`, `finalization-channel-drain-004`, `finalization-timer-drain-005` (missing), `FB-011`, `FB-012` (catalog); `finalization-quiescence-006` (legacy). v2: `quiescence-pending-timer-001`, `region-lifecycle-close-cancels-children-001`, `budget-cancel-unacknowledged-stays-running-001` (run ends non-quiescent) | `rust_vs_c`, `profile_parity` | invariant + scenario + conformance | partial |
| `FINALIZE-001` | Bounded cleanup and leak-reporting behavior under cancel/exhaustion | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` sections 4, 5, and 7 | `finalization-leak-003`, `finalization-cancel-budget-006`, `finalization-phase-regression-007` (missing). v2: `leak-policy-leak-reported-001`, `obligation-cancelled-holder-leaks-001` (leak reporting); bounded cleanup budget is not covered | `rust_vs_c`, `profile_parity` | invariant + stress + conformance | partial |
| `CHANNEL-005` | Close/drain semantics (receiver drop drains queue, last sender drop wakes receiver) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 1.10 | `ch-close-drain-001`, `ch-close-wake-001` (missing). v2: `mpsc-two-phase-send-recv-001` (receiver drains after the sender closes, then `ASX_E_DISCONNECTED`), `lab-dispatch-mpsc-disconnect-order-001` (receiver drop seen by the sender) | `rust_vs_c`, `profile_parity` | unit + invariant + scenario | partial |
| `CHANNEL-006` | Obligation integration via session layer (`TrackedPermit`, leak-on-drop panic) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 1.9 | `ch-obligation-leak-001`, `ch-obligation-commit-001`, `ch-obligation-abort-001` (missing). v2: `mpsc-two-phase-send-recv-001` (each `reserve_send` traces `obligation.reserved` of kind SendPermit and each `permit_send` `obligation.committed`); abort or leak of a channel permit and the Rust `TrackedPermit` panic are not covered | `rust_vs_c`, `profile_parity` | unit + invariant | partial |
| `CHANNEL-007` | Capacity invariant (`used_slots = queue.len + reserved <= capacity`, fixed at creation) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 1.2 | `ch-capacity-invariant-001`, `ch-capacity-zero-panic-001` (missing). v2: `mpsc-try-ops-001` (second `try_send` on a capacity-1 channel returns `ASX_E_CHANNEL_FULL`); zero capacity is not covered | `rust_vs_c`, `profile_parity` | unit + invariant | partial |
| `TIMER-005` | 4-level hierarchical wheel cascade (Level 0 wrap -> Level N+1 advance -> re-insert) | `docs/CHANNEL_TIMER_SEMANTICS.md` sections 2.1, 2.4 | `tm-cascade-001`, `tm-overflow-001`, `tm-immediate-001` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + conformance | unmapped |
| `TIMER-006` | ID/generation u64 wrap safety (wrapping arithmetic, HashMap tracking) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 2.6 | `tm-wrap-001` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + stress | unmapped |
| `TIMER-007` | Duration validation (`try_register` rejects > `max_timer_duration`) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 2.3 | `tm-duration-exceeded-001` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit | unmapped |
| `TIMER-008` | All-cancelled storage purge (`active` map empty -> purge slot vectors/bitmaps) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 2.5 | `tm-purge-001` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit | unmapped |
| `SCHEDULER-001` | Three-lane priority ordering (Cancel > Timed > Ready, governor overrides) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.1 | `sc-lane-priority-001`, `sc-governor-meet-001`, `sc-governor-drain-001` (missing). v2: `lab-dispatch-cancel-waker-once-001`, `task-abort-next-step-001` (the compared dispatch order names the lane, cancel lane before ready lane); governor suggestions are not covered | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | partial |
| `SCHEDULER-002` | Generation-based FIFO within equal priority/deadline (monotone u64 counter) | `docs/CHANNEL_TIMER_SEMANTICS.md` sections 3.3, 3.4 | `sc-fifo-priority-001`, `sc-edf-001`, `sc-edf-fifo-001` (missing). No v2 fixture targets it | `rust_vs_c`, `codec_equivalence`, `profile_parity` | unit + conformance | unmapped |
| `SCHEDULER-003` | Deterministic RNG tie-breaking and work stealing (seed per worker, circular scan) | `docs/CHANNEL_TIMER_SEMANTICS.md` sections 3.5, 3.6 | `sc-rng-tiebreak-001`, `sc-steal-001` (missing). v2: `lab-dispatch-tie-break-001`, `scheduler-yield-interleave-001` (seeded tie-break among equal-priority ready tasks); work stealing is not covered | `rust_vs_c`, `profile_parity` | unit + conformance + stress | partial |
| `SCHEDULER-004` | Cancel-streak fairness limit (base 16, doubled under drain governor) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.7 | `sc-cancel-streak-001` (missing). No v2 fixture | `rust_vs_c`, `profile_parity` | unit + stress | unmapped |
| `SCHEDULER-005` | Fairness certificate with deterministic witness hash for replay verification | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.8 | `sc-certificate-001` (missing). v2 fixtures record Rust's `schedule.certificate_hash` (`tools/twin_run/src/run.rs:2008`), but `asx-conformance` does not compare it (it compares trace, snapshot, observations and dispatches, `tools/conformance/runner.c:221-263`) | `rust_vs_c`, `profile_parity` | unit + conformance | unmapped |
| `SCHEDULER-006` | Phase 0 timer processing before task dispatch (expired timers -> inject tasks) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.2 | `sc-timer-phase0-001` (missing). v2: `lab-dispatch-timer-wheel-order-001` (timers due in one auto-advance step wake their tasks in registration order before the next picks) | `rust_vs_c`, `profile_parity` | unit + conformance | partial |
| `TRACE-001` | Versioned trace event schema and Rust/C event correlation | `docs/TRACE_EVENT_SCHEMA.md`; `schemas/trace_event.schema.json`; `include/asx/runtime/trace.h`; `src/runtime/trace.c` | `trace-schema-v1-c-region-task`, `trace-schema-v1-rust-correlated` (`fixtures/trace_events/*.json`) | `rust_vs_c`, `codec_equivalence`, `profile_parity`, `incident_bundle` | `test_trace` schema compatibility + `lint-schema-validation` | mapped |
| `INCIDENT-001` | Operator incident evidence bundle tying replay, status, telemetry, trace, and parity artifacts | `docs/INCIDENT_EVIDENCE_BUNDLE.md`; `schemas/incident_bundle.schema.json`; `include/asx/evidence/evidence.h`; `src/evidence/evidence.c`; `tests/e2e/parallel_swarm.sh` | `incident-bundle-v1-parallel-swarm` (`fixtures/incident_bundles/`); e2e `parallel_swarm.incident_bundle.jsonl` (written by `tests/e2e/parallel_swarm.sh:26`) | `incident_bundle`, `profile_parity` | `test_evidence` renderer coverage + `test-e2e-parallel` artifact emission + `lint-schema-validation` | mapped |
| `REPLAY-COUNTEREXAMPLE-001` | Class-preserving minimized replay/fuzz counterexample packet | `docs/REPLAY_COUNTEREXAMPLE_MINIMIZER.md`; `schemas/replay_counterexample.schema.json`; `include/asx/runtime/replay.h`; `src/runtime/replay.c`; `tests/fuzz/fuzz_minimize.c` | `replay-counterexample-v1-resource` (`fixtures/replay_counterexamples/`); fuzz CLI `minimized_scenario` JSONL (`tests/fuzz/fuzz_minimize.c:1013`) | `rust_vs_c`, `codec_equivalence`, `profile_parity`, `incident_bundle` | `test_replay` minimizer class-preservation coverage + `minimize-selftest` + `lint-schema-validation` | mapped |
| `COMBINATOR-001` | Join/race/select/timeout/first-ok/quorum fixed-branch semantics | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C02; `include/asx/core/combinator.h`; `src/core/combinator.c`; `docs/RUST_EXPORTED_SURFACE_INVENTORY.md` combinator row | `combinator-contract-001` (legacy, 4 files in `fixtures/rust_reference/core_combinator/`; classified hand-authored and excluded from Rust parity by `tools/ci/run_conformance.sh:497`). v2: `combinators-first-ok-001`, `combinators-race-timeout-001`, `task-groups-quorum-001` (also `task-groups-join-all-001`, `task-groups-race-losers-drained-001`); select is not covered | `rust_vs_c`, `codec_equivalence`, `profile_parity` | `test-combinator-contract` + conformance | partial |
| `COMBINATOR-002` | Retry/bracket/pipeline/bulkhead/rate-limit/hedge/map-reduce, JoinSet, plan DAG, and service middleware caps | `docs/DEFERRED_SURFACE_REGISTER.md` DS-C02; `include/asx/core/combinator2.h`; `include/asx/core/join_set.h`; `include/asx/plan/plan.h`; `include/asx/service/service.h`; `src/core/combinator2.c`; `src/core/join_set.c`; `src/plan/plan.c` | `combinator-contract-001` (legacy, hand-authored; see `COMBINATOR-001`). No v2 fixture | `rust_vs_c`, `codec_equivalence`, `profile_parity` | `test-combinator-contract` + conformance | unmapped |

## 4. Parity Row Conventions

For each `prov_id`, parity reports should include:

- `prov_id`
- `scenario_id`
- `codec` (`json` or `bin`)
- `profile` (`ASX_PROFILE_*`)
- `rust_baseline_commit`
- `semantic_digest`
- `delta_classification` (`none`, `intentional_upstream`, `c_regression`, `spec_defect`, `harness_defect`)

Parity dimensions:

1. `rust_vs_c`: semantic equivalence against the pinned Rust baseline. The executed form today is `make conformance` over `fixtures/rust_reference_v2` and `make fuzz-differential` (section 1).
2. `codec_equivalence`: JSON vs BIN digest equality for the same scenario.
3. `profile_parity`: shared fixture sets across target profiles.

## 5. Update Rules When Semantics Evolve

When the upstream Rust baseline changes:

1. Update `docs/rust_baseline_inventory.json` and `docs/RUST_BASELINE_PROVENANCE.md`.
2. Re-evaluate every row whose source provenance touches changed Rust files.
3. Recapture fixtures and classify each non-zero delta.
4. Record row-level status transitions.
5. Never drop a `prov_id`; supersede it by a version annotation if semantics materially change.

## 6. Coverage Summary

Counts of rows in section 3 by status (2026-10-10):

| Status | Rows | prov_ids |
|---|---|---|
| `mapped` | 4 | BASELINE-001, TRACE-001, INCIDENT-001, REPLAY-COUNTEREXAMPLE-001 |
| `partial` | 20 | OUTCOME-001..002, BUDGET-002, REGION-001..002, TASK-001, OBLIGATION-001, CHANNEL-001, CHANNEL-003, CHANNEL-005..007, TIMER-001..002, QUIESCENCE-001, FINALIZE-001, SCHEDULER-001, SCHEDULER-003, SCHEDULER-006, COMBINATOR-001 |
| `unmapped` | 14 | BUDGET-001, HANDLE-001, CHANNEL-002, CHANNEL-004, TIMER-003..008, SCHEDULER-002, SCHEDULER-004..005, COMBINATOR-002 |
| **Total** | **38** | |

## 7. Known Gaps

- None of the `ch-*`, `tm-*`, `sc-*`, `outcome-*`, `finalization-*` (except legacy `finalization-quiescence-006`), `channel-*` (except legacy `channel-two-phase-send-recv-001`, `channel-fifo-ordering-002`, `channel-backpressure-full-003`) or `budget-*` (except legacy `budget-meet-identity-010`) IDs listed in earlier versions of this map exist as fixtures.
- Hierarchical timer-wheel internals (cascade, overflow, coalescing, purge, wrap, duration limit), MPSC eviction, scheduler governor/streak/stealing/certificate behaviour and budget meet algebra have no Rust-captured fixture in this repository.
- `docs/FEATURE_PARITY.md` cross-references these rows by unit; `prov_id` is the key here.
