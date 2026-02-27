# Automotive Profile Companion (`ASX_PROFILE_AUTOMOTIVE`)

> **Bead:** `bd-296.9`
> **Status:** Companion profile specification
> **Last updated:** 2026-02-27 by CopperSpire, updated by PearlSwan

This document defines the automotive companion profile for deadline/watchdog-governed operation with deterministic degraded-mode behavior and semantic parity guarantees.

## 1. Purpose and Scope

`ASX_PROFILE_AUTOMOTIVE` targets safety-critical deployments where bounded execution and deterministic fault handling are required.

Profile objectives:

1. expose deadline/checkpoint/watchdog hooks as first-class runtime surfaces,
2. guarantee deterministic degraded-mode transitions,
3. retain full semantic parity with `ASX_PROFILE_CORE`.

## 2. Semantic Lock Rule

Allowed to tune:

- static-memory-first defaults,
- watchdog/deadline thresholds,
- degraded-mode policy parameters.

Not allowed to change:

- lifecycle/cancellation/obligation semantics,
- outcome taxonomy,
- failure-atomic rules,
- canonical semantic digest for shared fixtures.

## 3. Safety-Oriented Runtime Surfaces

Automotive profile should expose:

- explicit deadline checkpoint APIs,
- watchdog heartbeat/checkpoint hooks,
- deterministic degraded-mode transition callbacks,
- evidence-oriented event markers for safety audit trails.

## 4. Degraded-Mode Contract

On deadline/watchdog stress:

1. trigger deterministic transition policy,
2. record transition reason and phase,
3. preserve semantic legality and failure-atomic behavior,
4. avoid silent partial processing.

## 5. Memory and Resource Envelope Rules

Profile defaults should favor bounded memory behavior:

- static-memory-first configuration where feasible,
- explicit resource ceilings and deterministic rejection behavior,
- no hidden dynamic fallback that bypasses configured ceilings.

## 6. Acceptance Gate Mapping

Automotive profile is considered ready when these are green:

1. deadline/watchdog fixture family,
2. deterministic degraded-mode transition fixtures,
3. cross-profile semantic digest parity for shared fixtures,
4. crash/restart/replay continuity evidence.

## 7. Vertical Fixture Contract Linkage

Vertical fixture family authority for automotive is:

- `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md` family `VERT-AUTOMOTIVE-WATCHDOG`,
- e2e lane `E2E-VERT-AUTO`,
- schema `schemas/vertical_continuity_fixture_family_manifest.schema.json`.

Required baseline scenarios:

- `auto-watchdog-checkpoint-001`,
- `auto-degraded-transition-002`,
- `auto-deadline-miss-003`.

## 8. Downstream Bead Links

Primary implementation and validation consumers:

- `bd-1md.15` (vertical/continuity fixture family capture contract),
- `bd-j4m.*` (automotive vertical behavior),
- `bd-66l.*` (quality gate enforcement and evidence policy),
- `bd-2n0.*` (trace/replay continuity surfaces).
