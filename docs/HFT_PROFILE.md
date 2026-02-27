# HFT Profile Companion (`ASX_PROFILE_HFT`)

> **Bead:** `bd-296.9`
> **Status:** Companion profile specification
> **Last updated:** 2026-02-27 by CopperSpire, updated by PearlSwan

This document defines the HFT companion profile with strict tail-latency/jitter governance while preserving semantic parity with `ASX_PROFILE_CORE`.

## 1. Purpose and Scope

`ASX_PROFILE_HFT` targets low-latency environments requiring predictable tail behavior and deterministic incident replay.

Profile objectives:

1. tighten latency/jitter observability and control,
2. support deterministic overload behavior,
3. preserve canonical semantics under all profile policies.

## 2. Semantic Lock Rule

Allowed to tune:

- scheduling policy knobs,
- queue/lane operational defaults,
- instrumentation depth and sampling.

Not allowed to change:

- state transition legality,
- cancellation and finalization semantics,
- outcome/budget/exhaustion meaning,
- canonical semantic digest for shared fixtures.

## 3. Runtime Policy Surfaces

HFT profile policy surfaces may include:

- busy-spin vs hybrid spin/yield policy,
- queue/lane sizing defaults,
- burst handling and admission/backpressure strategy,
- deterministic overload fallback policy.

All policies must have deterministic fallback behavior for replay equivalence.

## 4. Measurement and Evidence Contract

Required observability outputs:

- `p50`, `p95`, `p99`, `p99.9`, `p99.99` latency summaries,
- jitter envelope reporting,
- overload transition markers,
- deterministic digest evidence for equivalent scenario inputs.

## 5. Overload and Degraded-Mode Rules

Under overload pressure:

- apply explicit deterministic policy transitions,
- emit auditable transition evidence,
- preserve semantic outcomes (no hidden behavior fork).

## 6. Acceptance Gate Mapping

HFT profile is considered ready when these are green:

1. tail-latency/jitter gate thresholds,
2. deterministic overload behavior fixtures,
3. cross-profile semantic digest parity for shared fixtures,
4. replay identity on repeated fixed-seed microburst scenarios.

## 7. Vertical Fixture Contract Linkage

Vertical fixture family authority for HFT is:

- `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md` family `VERT-HFT-MICROBURST`,
- e2e lane `E2E-VERT-HFT`,
- schema `schemas/vertical_continuity_fixture_family_manifest.schema.json`.

Required baseline scenarios:

- `hft-microburst-overload-001`,
- `hft-microburst-fairness-002`,
- `hft-overload-recovery-003`.

## 8. Downstream Bead Links

Primary implementation and validation consumers:

- `bd-1md.15` (vertical/continuity fixture family capture contract),
- `bd-j4m.*` (profile parity and vertical fixtures),
- `bd-66l.*` (quality gate enforcement),
- `bd-2cw.*` (scheduler and cancellation runtime mechanics).
