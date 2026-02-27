# Source-to-Fixture Provenance Map

> **Bead:** bd-296.19  
> **Status:** Canonical mapping slice complete for current extracted semantics (`bd-296.12`, `.15`, `.16`, `.17`, `.18`)  
> **Rust baseline commit:** `38c152405bd03e2bd9eecf178bfbbe9472fed861`  
> **Baseline inventory artifact:** `docs/rust_baseline_inventory.json`  
> **Last updated:** 2026-02-27 by BeigeOtter, extended by MossySeal/GrayKite

This document maps extracted semantic rules to fixture families, parity rows, and test obligations so implementation/QA work can proceed without re-reading Rust internals.

## 1. Inputs and Scope

This map currently integrates extracted artifacts from:

- `bd-296.12`: `docs/RUST_BASELINE_PROVENANCE.md`, `docs/rust_baseline_inventory.json`
- `bd-296.15`: `docs/LIFECYCLE_TRANSITION_TABLES.md`
- `bd-296.16`: `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`
- `bd-296.17`: `docs/CHANNEL_TIMER_DETERMINISM.md`, `docs/CHANNEL_TIMER_SEMANTICS.md`
- `bd-296.18`: `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md`

## 2. Row Contract

Each provenance row in this file tracks:

- `prov_id`: stable semantic-rule identifier.
- `semantic_unit`: human-readable unit name.
- `source_provenance`: canonical source artifacts (Rust and extracted docs).
- `fixture_links`: fixture family or concrete fixture IDs.
- `parity_targets`: required parity dimensions (`rust_vs_c`, `codec_equivalence`, `profile_parity`).
- `test_obligations`: minimum required test layers.
- `status`: `mapped` or `partial`.

## 3. Canonical Mapping Matrix

| prov_id | semantic_unit | source_provenance | fixture_links | parity_targets | test_obligations | status |
|---|---|---|---|---|---|---|
| `BASELINE-001` | Frozen Rust baseline identity | `docs/RUST_BASELINE_PROVENANCE.md`, `docs/rust_baseline_inventory.json` | fixture metadata fields in all scenario files (`rust_baseline_commit`, toolchain, `cargo_lock_sha256`) | `rust_vs_c` | conformance schema validation, provenance lint | mapped |
| `OUTCOME-001` | Outcome severity lattice (`Ok < Err < Cancelled < Panicked`) | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` section 2 | `outcome-lattice-001..007`, `outcome-join-left-bias-001`, `outcome-join-order-002`, `outcome-join-top-003` | `rust_vs_c`, `codec_equivalence`, `profile_parity` | unit + conformance | mapped |
| `OUTCOME-002` | Equal-severity left-bias and cancel-strengthen behavior | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` sections 2.2/2.3/4.4 | `outcome-cancel-strengthen-004`, `task-lifecycle-006`, `task-lifecycle-007` | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | mapped |
| `BUDGET-001` | Budget meet algebra and identities (`INFINITE`, `ZERO`) | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` section 3 | `budget-meet-identity-010`, `budget-meet-absorbing-011`, `budget-deadline-none-012` | `rust_vs_c`, `codec_equivalence` | unit + conformance | mapped |
| `BUDGET-002` | Exhaustion checks and cancellation mapping | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` section 4 | `budget-consume-poll-013`, `budget-consume-cost-014`, `budget-deadline-boundary-015`, `runtime-pollquota-cancel-016`, `cancel-cleanup-budget-017` | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | mapped |
| `REGION-001` | Region close progression (`Open -> Closing -> Draining -> Finalizing -> Closed`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 1 | `region-lifecycle-001`, `region-lifecycle-002`, `region-lifecycle-009` | `rust_vs_c`, `profile_parity` | invariant + conformance + scenario | mapped |
| `REGION-002` | Illegal region transitions and admission gate failures | `docs/LIFECYCLE_TRANSITION_TABLES.md` sections 1.3/1.4 | `region-lifecycle-005`, `region-lifecycle-006`, `region-lifecycle-007`, `FB-001`, `FB-002`, `FB-008` | `rust_vs_c`, `profile_parity` | unit + invariant | mapped |
| `TASK-001` | Task lifecycle legality and cancel-phase progression | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 2 | `task-lifecycle-001..013`, `cancel-protocol-001`, `cancel-protocol-002`, `cancel-protocol-010` | `rust_vs_c`, `profile_parity` | invariant + conformance + scenario | mapped |
| `OBLIGATION-001` | Obligation linearity (`Reserved -> Committed/Aborted/Leaked`) | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 3 | `obligation-lifecycle-001..010`, `FB-003..FB-006` | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | mapped |
| `HANDLE-001` | Stale-handle and generation-safety behavior | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 8, `docs/CHANNEL_TIMER_DETERMINISM.md` sections 3.1/3.2 | `FB-010`, `timer-cancel-generation-011` | `rust_vs_c`, `profile_parity` | unit + invariant | mapped |
| `CHANNEL-001` | Two-phase MPSC reserve/send/abort linearity | `docs/CHANNEL_TIMER_DETERMINISM.md` sections 2.1/2.2 | `channel-two-phase-001`, `channel-abort-release-002`, `channel-drop-permit-abort-003` | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | mapped |
| `CHANNEL-002` | FIFO waiter discipline and queue-jump prevention | `docs/CHANNEL_TIMER_DETERMINISM.md` section 2.3 | `channel-fifo-waiter-004`, `try_reserve_respects_fifo_over_capacity` | `rust_vs_c`, `profile_parity` | unit + stress + conformance | mapped |
| `CHANNEL-003` | Cancellation/disconnect behavior without partial mutation | `docs/CHANNEL_TIMER_DETERMINISM.md` section 2.4 | `channel-reserve-cancel-005`, `channel-recv-cancel-nonconsume-006`, `receiver_drop_unblocks_pending_reserve_without_leak` | `rust_vs_c`, `profile_parity` | unit + invariant | mapped |
| `CHANNEL-004` | Deterministic backpressure (`send_evict_oldest`) | `docs/CHANNEL_TIMER_DETERMINISM.md` section 2.5 | `channel-evict-reserved-007`, `channel-evict-committed-008` | `rust_vs_c`, `profile_parity` | unit + stress | mapped |
| `TIMER-001` | Equal-deadline deterministic ordering | `docs/CHANNEL_TIMER_DETERMINISM.md` section 3.1 | `timer-equal-deadline-order-010`, `same_deadline_pops_in_insertion_order` | `rust_vs_c`, `codec_equivalence`, `profile_parity` | unit + conformance | mapped |
| `TIMER-002` | Timer register/cancel/fire and generation-safe handles | `docs/CHANNEL_TIMER_DETERMINISM.md` section 3.2 | `timer-cancel-generation-011`, `wheel_cancel_prevents_fire`, `wheel_cancel_rejects_generation_mismatch_without_removing` | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | mapped |
| `TIMER-003` | `next_deadline` boundary and overflow promotion | `docs/CHANNEL_TIMER_DETERMINISM.md` sections 3.2/3.3 | `timer-next-deadline-same-tick-012`, `timer-overflow-promotion-013` | `rust_vs_c`, `profile_parity` | unit + conformance | mapped |
| `TIMER-004` | Coalescing window determinism with threshold gating | `docs/CHANNEL_TIMER_DETERMINISM.md` section 3.3 | `timer-coalescing-threshold-014`, `coalescing_min_group_size_enables_window_when_threshold_met` | `rust_vs_c`, `profile_parity` | unit + stress + conformance | mapped |
| `QUIESCENCE-001` | Region close preconditions over tasks/obligations/regions/timers/channels | `docs/LIFECYCLE_TRANSITION_TABLES.md` section 7, `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` sections 3 and 6 | `finalization-quiescence-001`, `finalization-quiescence-002`, `finalization-channel-drain-004`, `finalization-timer-drain-005`, `FB-011`, `FB-012` | `rust_vs_c`, `profile_parity` | invariant + scenario + conformance | mapped |
| `FINALIZE-001` | Bounded cleanup and leak-reporting behavior under cancel/exhaustion | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` sections 4, 5, and 7 | `finalization-leak-003`, `finalization-cancel-budget-006`, `finalization-phase-regression-007` | `rust_vs_c`, `profile_parity` | invariant + stress + conformance | mapped |
| `CHANNEL-005` | Close/drain semantics (receiver drop drains queue, last sender drop wakes receiver) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 1.10 | `ch-close-drain-001`, `ch-close-wake-001` | `rust_vs_c`, `profile_parity` | unit + invariant + scenario | mapped |
| `CHANNEL-006` | Obligation integration via session layer (`TrackedPermit`, leak-on-drop panic) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 1.9 | `ch-obligation-leak-001`, `ch-obligation-commit-001`, `ch-obligation-abort-001` | `rust_vs_c`, `profile_parity` | unit + invariant | mapped |
| `CHANNEL-007` | Capacity invariant (`used_slots = queue.len + reserved <= capacity`, fixed at creation) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 1.2 | `ch-capacity-invariant-001`, `ch-capacity-zero-panic-001` | `rust_vs_c`, `profile_parity` | unit + invariant | mapped |
| `TIMER-005` | 4-level hierarchical wheel cascade (Level 0 wrap -> Level N+1 advance -> re-insert) | `docs/CHANNEL_TIMER_SEMANTICS.md` sections 2.1, 2.4 | `tm-cascade-001`, `tm-overflow-001`, `tm-immediate-001` | `rust_vs_c`, `profile_parity` | unit + conformance | mapped |
| `TIMER-006` | ID/generation u64 wrap safety (wrapping arithmetic, HashMap tracking) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 2.6 | `tm-wrap-001` | `rust_vs_c`, `profile_parity` | unit + stress | mapped |
| `TIMER-007` | Duration validation (`try_register` rejects > `max_timer_duration`) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 2.3 | `tm-duration-exceeded-001` | `rust_vs_c`, `profile_parity` | unit | mapped |
| `TIMER-008` | All-cancelled storage purge (`active` map empty -> purge slot vectors/bitmaps) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 2.5 | `tm-purge-001` | `rust_vs_c`, `profile_parity` | unit | mapped |
| `SCHEDULER-001` | Three-lane priority ordering (Cancel > Timed > Ready, governor overrides) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.1 | `sc-lane-priority-001`, `sc-governor-meet-001`, `sc-governor-drain-001` | `rust_vs_c`, `profile_parity` | unit + invariant + conformance | mapped |
| `SCHEDULER-002` | Generation-based FIFO within equal priority/deadline (monotone u64 counter) | `docs/CHANNEL_TIMER_SEMANTICS.md` sections 3.3, 3.4 | `sc-fifo-priority-001`, `sc-edf-001`, `sc-edf-fifo-001` | `rust_vs_c`, `codec_equivalence`, `profile_parity` | unit + conformance | mapped |
| `SCHEDULER-003` | Deterministic RNG tie-breaking and work stealing (seed per worker, circular scan) | `docs/CHANNEL_TIMER_SEMANTICS.md` sections 3.5, 3.6 | `sc-rng-tiebreak-001`, `sc-steal-001` | `rust_vs_c`, `profile_parity` | unit + conformance + stress | mapped |
| `SCHEDULER-004` | Cancel-streak fairness limit (base 16, doubled under drain governor) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.7 | `sc-cancel-streak-001` | `rust_vs_c`, `profile_parity` | unit + stress | mapped |
| `SCHEDULER-005` | Fairness certificate with deterministic witness hash for replay verification | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.8 | `sc-certificate-001` | `rust_vs_c`, `profile_parity` | unit + conformance | mapped |
| `SCHEDULER-006` | Phase 0 timer processing before task dispatch (expired timers -> inject tasks) | `docs/CHANNEL_TIMER_SEMANTICS.md` section 3.2 | `sc-timer-phase0-001` | `rust_vs_c`, `profile_parity` | unit + conformance | mapped |

## 4. Parity Row Conventions

For each mapped `prov_id`, parity reports should include:

- `prov_id`
- `scenario_id`
- `codec` (`json` or `bin`)
- `profile` (`ASX_PROFILE_*`)
- `rust_baseline_commit`
- `semantic_digest`
- `delta_classification` (`none`, `intentional_upstream`, `c_regression`, `spec_defect`, `harness_defect`)

Minimum parity coverage set:

1. `rust_vs_c`: semantic equivalence against pinned Rust baseline.
2. `codec_equivalence`: JSON vs BIN digest equality for same scenario.
3. `profile_parity`: shared fixture sets across target profiles.

## 5. Update Rules When Semantics Evolve

When upstream Rust baseline changes:

1. Update `docs/rust_baseline_inventory.json` and `docs/RUST_BASELINE_PROVENANCE.md`.
2. Re-evaluate every row whose source provenance touches changed Rust files.
3. Regenerate fixtures/parity outputs and classify each non-zero delta.
4. Record row-level status transitions:
   - `mapped -> partial` during rework,
   - `partial -> mapped` after verification.
5. Never drop a `prov_id`; supersede by version annotation if semantics materially change.

## 6. Coverage Summary

| Domain | prov_id range | Rows | Fixture IDs | Status |
|--------|--------------|------|-------------|--------|
| Baseline | BASELINE-001 | 1 | metadata fields | mapped |
| Outcome | OUTCOME-001..002 | 2 | ~10 | mapped |
| Budget | BUDGET-001..002 | 2 | ~8 | mapped |
| Region | REGION-001..002 | 2 | ~9 | mapped |
| Task | TASK-001 | 1 | ~16 | mapped |
| Obligation | OBLIGATION-001 | 1 | ~14 | mapped |
| Handle | HANDLE-001 | 1 | ~2 | mapped |
| Channel | CHANNEL-001..007 | 7 | ~18 | mapped |
| Timer | TIMER-001..008 | 8 | ~14 | mapped |
| Scheduler | SCHEDULER-001..006 | 6 | ~12 | mapped |
| Quiescence | QUIESCENCE-001 | 1 | ~6 | mapped |
| Finalization | FINALIZE-001 | 1 | ~3 | mapped |
| **Total** | | **33** | **~112** | |

## 7. Known Gaps (Current Session)

- `docs/FEATURE_PARITY.md` is now available; row-to-parity-table links use `prov_id` as the canonical key and can be cross-referenced against `FEATURE_PARITY.md` unit IDs.
- Finalization fixture IDs are mapped from `bd-296.18` candidate extensions and should be kept stable when they are materialized in conformance assets.
- Scheduler provenance rows reference `docs/CHANNEL_TIMER_SEMANTICS.md` which provides deeper Rust source analysis of the three-lane scheduler, work stealing, and fairness certificate semantics not covered in `CHANNEL_TIMER_DETERMINISM.md`.
