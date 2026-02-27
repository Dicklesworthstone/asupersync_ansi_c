# Rust -> C Guarantee-Substitution Matrix and Anti-Butchering Contract

> **Bead:** `bd-296.6`  
> **Status:** Canonical substitution contract (kernel scope fully mapped; deferred rows explicitly marked)  
> **Last updated:** 2026-02-27 by LilacTurtle

This document defines how Rust-side correctness guarantees are intentionally substituted in ANSI C, and which proof artifacts are mandatory before any substitution is accepted.

## 1. Normative Sources

- `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` sections 6.9, 6.10, 6.13, 6.14, 15.
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (canonical semantics baseline).
- `docs/LIFECYCLE_TRANSITION_TABLES.md` (state authority + forbidden transitions).
- `docs/CHANNEL_TIMER_DETERMINISM.md` and `docs/CHANNEL_TIMER_SEMANTICS.md` (kernel ordering contracts).
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` (prov-id and fixture linkage).
- `docs/DEFERRED_SURFACES.md`, `docs/OPEN_DECISIONS_ADR.md` (deferred scope guardrails).

## 2. Row Contract

Each row below defines:

- `gs_id`: stable guarantee-substitution identifier.
- `kernel_scope`: `yes` for Wave A kernel gates; `no` when explicitly deferred.
- `rust_guarantee`: guarantee category available in Rust.
- `c_substitution`: concrete C mechanism(s) that replace it.
- `proof_artifacts`: minimum verification obligations.
- `current_refs`: existing provenance/fixture anchors.
- `status`: `mapped-kernel`, `mapped-deferred`, or `gap`.

Design rule from plan section 6.9:

- if a Rust guarantee has no concrete C substitution plus proof artifacts, that surface is not ready to ship.

## 3. Canonical Substitution Matrix

| gs_id | kernel_scope | rust_guarantee | c_substitution | proof_artifacts | current_refs | status |
|---|---|---|---|---|---|---|
| `GS-001` | yes | Borrow checker + lifetimes (anti-alias/UAF) | Generation-tagged handles, strict handle validation at API boundaries, debug `ghost_borrow_ledger` | stale-handle rejection tests, alias/UAF stress corpus, ghost-ledger violation count = 0 | `HANDLE-001`, `FB-010`, `timer-cancel-generation-011` | mapped-kernel |
| `GS-002` | yes | RAII + `Drop` cleanup guarantees | `asx_cleanup_stack`, obligation linearity ledger, deterministic finalizer drain | reserve/commit/abort/leak invariants, unresolved-obligation zero checks at close, finalization leak fixtures | `OBLIGATION-001`, `FINALIZE-001`, `finalization-leak-003` | mapped-kernel |
| `GS-003` | yes | Exhaustive enums/match transition coverage | Transition-authority tables + generated validators + forbidden-transition assertions | transition completeness report, illegal-transition must-fail suite, invariant table coverage | `REGION-001/002`, `TASK-001`, `FB-001..FB-008` | mapped-kernel |
| `GS-004` | yes | Typestate-style protocol legality | Runtime phase tags with explicit precondition gates at each API edge | state-machine edge coverage, forbidden-path fixtures, protocol monitor clean report | `TASK-001`, `cancel-protocol-*`, `channel-two-phase-001` | mapped-kernel |
| `GS-005` | yes | Structured `async/await` poll + cancellation semantics | Explicit continuation records, mandatory checkpoints, bounded cleanup budget progression | checkpoint coverage audit, cancellation-latency/overrun tests, deterministic force-complete path tests | `BUDGET-002`, `cancel-cleanup-budget-017`, `finalization-cancel-budget-006` | mapped-kernel |
| `GS-006` | yes | Panic containment/unwinding model | Explicit `Outcome::Panicked` domain + compensation/finalization paths with no silent recovery | panic-domain scenario fixtures, compensation-path replay checks, containment invariants | `OUTCOME-001`, `outcome-join-top-003`, task panic transition rows | mapped-kernel |
| `GS-007` | yes | Deterministic ordering/replay identity | Stable scheduler/timer/channel tie-break keys + deterministic digest monitor | twin-run digest equality, codec/profile parity digest equivalence, determinism monitor clean report | `SCHEDULER-001..006`, `TIMER-001`, `CHANNEL-002`, parity conventions in provenance map | mapped-kernel |
| `GS-008` | yes | Failure-atomic resource handling | Explicit admission outcomes, bounded budgets, no partial mutation on exhaustion | exhaustion suites, failure-atomic boundary tests, deterministic error taxonomy assertions | `BUDGET-002`, `region-lifecycle-010`, `ch-exhaustion-001`, `tm-exhaustion-001` | mapped-kernel |
| `GS-009` | no | `Send`/`Sync` trait-level thread safety | Thread-affinity tags, explicit transfer certificates, ownership epoch increments | wrong-thread access tests, transfer-certificate checks, concurrency misuse fixtures | Plan 6.9 row + deferred parallel profile ADR context | mapped-deferred |
| `GS-010` | no | Safe concurrent reclamation without GC (`Arc`-style safety analog) | EBR/hazard-pointer strategy or seqlock-compatible deferred free policy | reclamation safety tests under parallel stress, ABA/retire-order checks, fallback parity report | ALPHA-5/6 in plan 6.14, deferred-surface records | mapped-deferred |

Kernel-scope check:

- required kernel rows (`GS-001`..`GS-008`) are all mapped.
- deferred rows are explicit (`GS-009`, `GS-010`) and do not block Wave A kernel completion.

## 4. Anti-Butchering Enforcement Contract

Any PR touching kernel semantics must include a `Guarantee Impact` block in description or commit notes.

Required fields:

1. affected `gs_id` row(s),
2. old mechanism -> new mechanism delta,
3. invariants preserved (explicit statement),
4. proof artifacts added/updated,
5. fallback/safe-mode behavior,
6. ordering/isomorphism note for deterministic mode,
7. semantic delta budget declaration (`0` unless explicitly approved).

Template:

```text
Guarantee Impact:
- gs_id: GS-00X
- change: <old mechanism> -> <new mechanism>
- preserved invariants: <list>
- proof artifacts: <tests/fixtures/parity rows>
- fallback mode: <safe-mode behavior>
- deterministic isomorphism note: <ordering/tie-break impact>
- semantic delta budget: 0
```

## 5. Automatic Rejection Conditions

A change must be rejected if any condition is true:

1. touches a mapped `GS-001..GS-008` mechanism without updated proof artifacts,
2. weakens handle validation, transition guards, or linearity enforcement on boundary paths,
3. introduces nondeterministic tie-break behavior in deterministic profile,
4. changes cancellation/exhaustion semantics without corresponding provenance/fixture updates,
5. relies on profile-specific behavior fork that changes semantic digest on shared fixture sets,
6. marks a kernel-scope row as deferred without owner-approved scope change,
7. claims performance wins while removing fallback/safe-mode behavior.

## 6. Risky Optimization Rollback Expectations

| optimization class | mandatory fallback | rollback trigger examples | evidence required |
|---|---|---|---|
| Boundary-check elision or relocation | Compile-time/profile flag restoring strict checks | invariant failure, ghost monitor violations, unexplained parity drift | before/after invariants + parity diff report |
| Scheduler/timer ordering optimization | Canonical stable-order path | digest mismatch on same-seed replay, tie-break nondeterminism | deterministic twin-run report + fixture replay pack |
| Channel throughput optimization | Canonical two-phase FIFO path | leaked reservation, queue-jump, cancellation partial-mutation | channel invariant suite + stress logs |
| Adaptive runtime policy changes | Deterministic fixed-policy mode | confidence gate failure, overload instability, unbounded latency tails | policy ledger + fallback activation evidence |
| Parallel/concurrency acceleration | Single-thread kernel mode and/or deferred free mode | race/lifetime violation under stress, fairness regression | concurrency stress artifacts + parity witness |

## 7. Verification Checklist for `bd-296.6`

Before closing this bead:

1. All kernel-scope rows remain `mapped-kernel`.
2. Every row has at least one concrete proof artifact family.
3. Deferred rows include explicit unblock criteria and remain outside Wave A gate.
4. PR/merge template for `Guarantee Impact` is referenced in downstream execution docs.
5. Substitution rows are traceable to `prov_id` domains in `SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`.

## 8. Downstream Usage

This matrix is the authoritative substitution contract for:

- `bd-hwb.*` core-type/safety mechanism implementation,
- `bd-2cw.*` runtime-kernel implementation and deterministic enforcement,
- `bd-1md.*` fixture/conformance/fuzz verification rows,
- `bd-66l.9` evidence-linkage enforcement in CI/closure notes.
