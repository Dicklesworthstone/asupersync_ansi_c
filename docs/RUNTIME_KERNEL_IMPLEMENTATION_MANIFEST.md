# Runtime Kernel Implementation Manifest

> **Lane:** `bd-2cw` epic prework  
> **Status:** Execution manifest for deterministic runtime kernel implementation  
> **Last updated:** 2026-02-27 by LilacTurtle

This manifest converts extracted Phase 1 semantics into an implementation contract for the runtime-kernel stream (`bd-2cw.*`), with explicit sequencing, invariants, and evidence requirements.

## 1. Normative Inputs

Primary semantic authority:

- `docs/LIFECYCLE_TRANSITION_TABLES.md` (`bd-296.15`)
- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` (`bd-296.16`)
- `docs/CHANNEL_TIMER_DETERMINISM.md` (`bd-296.17`)
- `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` (`bd-296.18`)
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` (`bd-296.19`)

Execution constraints:

- `AGENTS.md` (quality gates, no-semantic-drift policy, `rch` requirement for heavy builds/tests)
- `README.md` (target architecture and profile model)
- `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` (determinism and quality-gate intent)

## 2. Runtime Kernel Non-Negotiables

Any implementation in `bd-2cw.*` must preserve these invariants:

1. Deterministic scheduler ordering for equivalent inputs (stable tie-breaks).
2. Cancellation protocol monotonicity (`CancelRequested -> Cancelling -> Finalizing -> Completed`) with strengthening-only repeats.
3. Channel two-phase linearity (`reserve` resolves exactly once via send/abort/drop-abort).
4. Timer generation safety (stale handle cancel is rejected without mutating live timer state).
5. Quiescence truthfulness: no false-success while tasks/obligations/regions/timers/channels remain non-terminal.
6. Failure-atomic behavior under resource exhaustion (no partial state corruption).

## 3. Child-Bead Execution Graph

Dependency-accurate order (current beads graph):

1. `bd-2cw.1` arena-backed region/task/obligation tables (blocked by `bd-hwb.4`)
2. `bd-2cw.2` deterministic scheduler loop (blocked by `bd-2cw.1`, `bd-hwb.3`, `bd-hwb.10`, `bd-hwb.13`)
3. `bd-2cw.3` cancellation propagation/checkpoints (blocked by `bd-2cw.2`, `bd-hwb.10`)
4. `bd-2cw.4` timer wheel semantics + O(1) cancel (blocked by `bd-2cw.2`, `bd-2cw.3`)
5. `bd-2cw.6` bounded two-phase MPSC channel (blocked by `bd-2cw.1`, `bd-hwb.6`)
6. `bd-2cw.5` exhaustive scheduler checker (blocked by `bd-2cw.2`, `bd-2cw.3`, `bd-2cw.4`)
7. `bd-2cw.7` optional parallel profile lane (blocked by `bd-2cw.6`, `bd-j4m.1`, `bd-hwb.11`, `bd-296.13`)

## 4. Per-Child Implementation Contract

### 4.1 `bd-2cw.1` — Arena + Generation Handles

Must deliver:

- arena-backed tables for region/task/obligation records,
- generation-safe handle validation for stale-handle rejection,
- lifecycle-transition guards wired to transition legality tables.

Primary fixture families to target:

- `region-lifecycle-*`
- `task-lifecycle-*`
- `obligation-lifecycle-*`
- `FB-010` (stale handle behavior)

### 4.2 `bd-2cw.2` — Deterministic Scheduler Loop

Must deliver:

- deterministic event ordering with stable tie-break keys,
- replay-stable polling and wakeup sequencing,
- deterministic checkpoint boundaries for cancellation and budgets.

Primary fixture families to target:

- scheduler deterministic ordering fixtures (new in `bd-2cw.2`)
- `task-lifecycle-006/007` (cancel strengthening interactions)
- budget exhaustion fixtures from `EXISTING_ASUPERSYNC_STRUCTURE.md`

### 4.3 `bd-2cw.3` — Cancellation Propagation + Bounded Cleanup

Must deliver:

- parent-to-child cancel propagation checkpoints,
- cleanup-budget activation and bounded progression semantics,
- deterministic force-complete behavior on cleanup budget overrun.

Primary fixture families to target:

- `cancel-protocol-*`
- `cancel-cleanup-budget-017`
- `finalization-cancel-budget-006`
- `finalization-phase-regression-007`

### 4.4 `bd-2cw.4` — Timer Wheel + Handle-Safe Cancel

Must deliver:

- deterministic timer registration, fire, and cancel paths,
- equal-deadline insertion-stable ordering,
- generation-safe handle cancellation in O(1) active-map path.

Primary fixture families to target:

- `timer-equal-deadline-order-010`
- `timer-cancel-generation-011`
- `timer-next-deadline-same-tick-012`
- `timer-overflow-promotion-013`
- `timer-coalescing-threshold-014`

### 4.5 `bd-2cw.6` — Bounded Two-Phase MPSC Channel

Must deliver:

- `queue_len + reserved <= capacity` invariant across all transitions,
- FIFO waiter discipline and queue-jump prevention,
- cancellation/disconnect behavior without phantom reservation leaks.

Primary fixture families to target:

- `channel-two-phase-001`
- `channel-abort-release-002`
- `channel-drop-permit-abort-003`
- `channel-fifo-waiter-004`
- `channel-reserve-cancel-005`
- `channel-recv-cancel-nonconsume-006`
- `channel-evict-reserved-007`
- `channel-evict-committed-008`

### 4.6 `bd-2cw.5` — Small-State Exhaustive Checker

Must deliver:

- small-state transition explorer for scheduler/cancel/timer/channel interactions,
- cycle-budget checkpoint validation,
- minimized deterministic counterexamples for illegal transition or ordering drift.

Primary fixture families to target:

- forbidden behavior matrix (`FB-*`) against kernel state machine products
- new exhaustive checker scenario pack for cycle budgets and ordering ties

### 4.7 `bd-2cw.7` — Optional Parallel Profile Rules

Must deliver:

- deterministic compatibility mode and explicitly scoped nondeterministic zones,
- lane/worker fairness constraints and starvation checks,
- parity guardrails to prevent semantic fork from core profile.

Primary fixture families to target:

- profile parity packs for shared kernel scenarios
- fairness/starvation validation scenarios in optional parallel mode

## 5. Module Surface Plan (C Runtime)

Planned target surfaces (align with scaffold assumptions):

- `src/runtime/scheduler.c` — core loop, ordering, poll bookkeeping
- `src/runtime/cancel.c` — cancellation checkpoints + cleanup-budget progression
- `src/time/timer_wheel.c` — timer wheel + handle map + overflow/coalescing
- `src/channel/mpsc.c` — bounded two-phase channel
- `src/runtime/quiescence.c` — quiescence checks over tasks/obligations/regions/timers/channels

Supporting headers:

- `include/asx/asx_runtime.h` (internal runtime contract)
- `include/asx/asx_channel.h`, `include/asx/asx_time.h` (if split is adopted)

## 6. Evidence Contract (`bd-66l.9` Compatibility)

Each `bd-2cw.*` closure must include all three evidence classes, or explicit non-applicability rationale:

1. Unit evidence:
   - module-level tests for local invariants and edge/error paths.
2. Scenario/e2e evidence:
   - deterministic execution scenario(s) exercising integration behavior.
3. Structured logs/artifacts:
   - machine-readable outputs under an artifact path with bead id + command + result summary.

Minimum closure note fields:

- changed files
- fixture IDs added/updated
- commands run (heavy commands via `rch exec -- ...`)
- pass/fail summary with artifact paths

## 7. Heavy Command Execution Rule

All CPU-intensive build/test/lint runs in this lane must be offloaded:

```bash
rch exec -- make build
rch exec -- make test
rch exec -- make test-invariants
rch exec -- make conformance
```

If `rch` fails open, record that fallback explicitly in the bead completion notes.

## 8. Immediate Next Action Trigger

When `bd-hwb.4` becomes non-blocking, claim `bd-2cw.1` first and use this manifest as the execution checklist for:

1. transition-safe table/handle core,
2. deterministic scheduler foundations,
3. cancellation/timer/channel layering without semantic drift.
