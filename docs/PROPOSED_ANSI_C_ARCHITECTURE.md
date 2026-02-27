# Proposed ANSI C Architecture (Spec-Derived, Non-Transliteration)

> **Bead:** `bd-296.2`
> **Status:** Proposed architecture derived from canonical semantics
> **Last updated:** 2026-02-27 by CopperSpire

This architecture is derived from the canonical semantics (`bd-296.1`) and program constraints, not from Rust module-by-module transliteration. The goal is semantic fidelity under explicit C ownership, deterministic replay, and failure-atomic resource behavior.

## 1. Design Constraints (Non-Negotiable)

1. Semantic behavior is fixed by canonical extraction artifacts.
2. Resource-plane tuning may change performance only, never semantic outcomes.
3. Deterministic mode guarantees stable ordering and replay identity for fixed input/seed/profile.
4. Core runtime remains dependency-free ANSI C/C99 (standard library only in core).
5. All unsafe or UB-prone operations are either banned or wrapped in audited primitives.

## 2. Architectural Layers

```text
Scenario/API Input
  -> asx_core
  -> asx_runtime_kernel
  -> codecs (JSON/BIN)
  -> trace/replay/conformance
  -> profile/platform adapters
```

### 2.1 `asx_core` (Semantic Plane)

Responsibilities:

- IDs + generation counters,
- status/error taxonomy,
- outcome/cancel/budget semantics,
- transition authority tables and legality checks,
- resource-contract types and decision results.

Properties:

- platform-neutral,
- deterministic pure semantics,
- no reactor/thread dependencies.

### 2.2 `asx_runtime_kernel` (Execution Plane)

Responsibilities:

- region/task/obligation lifecycle engine,
- scheduler tie-break sequencing,
- cancellation propagation and checkpoints,
- timer/channel integration,
- close/finalize/quiescence driver.

Properties:

- single-thread deterministic baseline,
- no heap allocations on steady-state hot path,
- explicit error/status propagation with no silent fallback.

### 2.3 Codec Layer

Responsibilities:

- canonical schema model,
- JSON codec for diffability/bring-up,
- binary codec for throughput/footprint.

Properties:

- codec choice cannot alter semantic outcomes,
- canonical semantic digest equivalence required across codecs.

### 2.4 Trace/Replay/Conformance Layer

Responsibilities:

- deterministic event journaling,
- replay input/output normalization,
- Rust-vs-C parity harness integration,
- fixture and digest comparison.

Properties:

- trace output is evidence, not optional debug noise,
- replay identity must hold for same inputs.

### 2.5 Platform/Profile Adapters (Resource Plane)

Responsibilities:

- POSIX/WIN32/FREESTANDING hook integration,
- embedded/HFT/automotive profile defaults,
- compile-time feature gating and runtime config adaptation.

Properties:

- adapters may tune limits/scheduling policy knobs,
- adapters may not fork semantics.

## 3. Ownership and Lifetime Model

### 3.1 Handle Strategy

All externally visible runtime entities use generation-safe handles:

- region handle,
- task handle,
- obligation handle,
- timer handle.

Handle validation requires:

- type tag match,
- slot index bounds,
- generation match,
- liveness/state legality.

### 3.2 Arena/Table Ownership

Primary runtime state storage is arena/table-based:

- stable slot identity,
- O(1) lookup by handle,
- deterministic reclaim via lifecycle boundaries.

Ownership boundaries:

- runtime owns tables and region arenas,
- task state memory in kernel profiles is region-arena owned,
- external callers never mutate internal tables directly.

### 3.3 Cleanup and Linearity

C replacement for RAII guarantees:

- cleanup stack per close/finalize path,
- obligation linearity ledger (reserve -> commit/abort/leak exactly once),
- deterministic finalizer drain semantics.

## 4. State Authority and Transition Enforcement

### 4.1 Transition Tables as First-Class Authorities

Transition legality is table-driven (X-macro or generated tables) with explicit rules for:

- region transitions,
- task transitions,
- obligation transitions,
- cancellation witness phase transitions.

### 4.2 Enforcement Locations

Mandatory checks at:

- all external API boundaries,
- all state transition calls,
- all close/finalize/quiescence gates,
- all handle dereference paths.

### 4.3 Debug Ghost Monitors

Debug/CI profiles include:

- protocol monitor,
- linearity monitor,
- determinism monitor.

Production profiles may compile out heavy ghost paths but must keep boundary legality checks.

## 5. Failure-Atomic Boundaries

### 5.1 Resource Contract Surfaces

Resource ceilings include:

- runtime memory,
- ready/cancel queues,
- timer nodes,
- trace events.

Admission outcomes are explicit and deterministic.

### 5.2 Failure-Atomic Rule

On exhaustion or invalid operation:

- return classified status/error,
- avoid partial mutation,
- preserve invariant consistency for replay/diagnostics.

### 5.3 Exhaustion-Cancellation Coupling

Exhaustion and budget triggers map into canonical cancellation reasons and bounded cleanup behavior; they are not silent drops.

## 6. Hot Path vs Mandatory Checks

### 6.1 Hot Path Targets

Critical path goals:

- scheduler dispatch,
- queue operations,
- timer pop/cancel,
- transition dispatch.

Target properties:

- O(1) queue and cancel operations,
- no steady-state heap allocation,
- stable deterministic ordering keys.

### 6.2 Non-Removable Checks

Even in optimized profiles, retain:

- handle generation checks at boundaries,
- transition legality checks at boundaries,
- cancellation monotonicity checks,
- quiescence/finalization precondition checks.

### 6.3 Profile-Gated Checks

Can be profile-gated (not removed from CI/debug):

- deep ghost ledgers,
- expensive telemetry/detail logging,
- exhaustive internal assertions away from public boundaries.

## 7. Public API Boundary Proposal

Top-level public headers:

- `include/asx/asx.h`
- `include/asx/asx_config.h`

Proposed API characteristics:

- explicit init/destroy lifecycle,
- no implicit global runtime,
- status-first return model,
- context-explicit operations (`asx_runtime*`, region/task handles),
- deterministic mode and profile selection in config.

## 8. Module Layout Proposal

```text
include/asx/
  asx.h
  asx_config.h
  asx_ids.h
  asx_status.h

src/core/
  ids.c
  status.c
  outcome.c
  budget.c
  cancel.c
  transition_tables.c

src/runtime/
  scheduler.c
  lifecycle.c
  cancellation.c
  quiescence.c

src/channel/
  mpsc.c

src/time/
  timer_wheel.c

src/platform/
  posix/hooks.c
  win32/hooks.c
  freestanding/hooks.c

tests/
  unit/
  invariant/
  conformance/
```

## 9. Profile and Adapter Architecture

### 9.1 Core Profiles

- `ASX_PROFILE_CORE`: deterministic kernel baseline.
- `ASX_PROFILE_FREESTANDING`: explicit hooks, no host assumptions.
- `ASX_PROFILE_EMBEDDED_ROUTER`: constrained defaults, same semantics.

### 9.2 Extended Profiles

- `ASX_PROFILE_HFT`: tail/jitter-focused operations, semantic parity required.
- `ASX_PROFILE_AUTOMOTIVE`: watchdog/deadline/degraded-mode hooks, semantic parity required.

### 9.3 Cross-Profile Semantic Rule

Shared fixtures must produce equivalent canonical semantic digests across participating profiles.

## 10. Codec and Replay Integration Points

### 10.1 Codec vtable Boundary

A codec abstraction (`asx_codec_vtable`) isolates encode/decode mechanics from core semantics.

### 10.2 Replay Boundary

Replay consumes normalized trace/scenario input and drives runtime transitions using identical semantic authorities as live execution.

### 10.3 Digest Boundary

Semantic digest generation occurs from normalized event/state material independent of transport encoding.

## 11. Architecture-to-Bead Mapping

This architecture directly enables:

- `bd-ix8.*`: scaffold/build/matrix bring-up,
- `bd-hwb.1+`: core type and guarantee-substitution implementation,
- `bd-2cw.*`: scheduler/cancel/timer/channel kernel implementation,
- `bd-1md.*`: fixture, conformance, and differential fuzz pipelines.

Immediate unblock effect:

- `bd-296.2` closure unblocks `bd-296.3`, then downstream `bd-ix8.1` and Phase 2 execution lanes.

## 12. Explicit Non-Goals

This proposal intentionally does not include:

- actor/supervision wave-D surfaces,
- ecosystem integration wrappers,
- compatibility shims for deprecated semantics.

Those surfaces remain outside current kernel-first scope unless explicitly opened by beads.
