# Embedded Target Profile Companion (`ASX_PROFILE_EMBEDDED_ROUTER`)

> **Bead:** `bd-296.9`
> **Status:** Companion profile specification
> **Last updated:** 2026-02-27 by CopperSpire

This document defines the embedded/router companion profile for asx with explicit operational envelopes and semantic-lock guarantees.

## 1. Purpose and Scope

`ASX_PROFILE_EMBEDDED_ROUTER` targets OpenWrt/BusyBox-class environments where memory and storage are constrained but semantic behavior must remain identical to core profile semantics.

Profile objectives:

1. preserve kernel semantics under constrained resource envelopes,
2. expose deterministic exhaustion and backpressure outcomes,
3. support reproducible replay and diagnostics with bounded trace cost.

## 2. Semantic Lock Rule

Embedded profile tuning may change:

- default memory/queue/timer ceilings,
- trace retention policy,
- operational throughput envelope.

Embedded profile tuning may not change:

- lifecycle legality,
- cancellation protocol semantics,
- obligation linearity,
- canonical semantic digest for shared fixtures.

## 3. Resource Class Contract

Resource classes are capability envelopes, not feature switches.

### 3.1 `ASX_CLASS_R1`

- very constrained footprint,
- aggressive queue/timer/trace caps,
- deterministic rejection on capacity misses.

### 3.2 `ASX_CLASS_R2`

- balanced defaults for typical router-class devices,
- deeper buffers than R1,
- same semantics and error taxonomy.

### 3.3 `ASX_CLASS_R3`

- higher-capacity embedded/server crossover envelope,
- richer diagnostic retention,
- same semantic behavior.

## 4. Target Matrix

Required embedded triplets:

- `mipsel-openwrt-linux-musl`
- `armv7-openwrt-linux-muslgnueabi`
- `aarch64-openwrt-linux-musl`

Required execution validation:

- cross-build success per triplet,
- QEMU scenario/replay smoke,
- optional hardware smoke where available.

## 5. Trace and Diagnostics Policy

Default trace behavior for embedded profile:

- RAM ring buffer first,
- optional spill-to-persistent path when configured,
- deterministic event ordering and digest generation retained.

## 6. Failure and Exhaustion Behavior

When envelopes are exceeded:

- return explicit classified status,
- avoid partial mutation,
- preserve deterministic replay surface.

No silent drop and no implicit semantic downgrade are permitted.

## 7. Acceptance Gate Mapping

Embedded companion profile is considered ready when these are green:

1. embedded matrix build gate,
2. QEMU scenario/replay gate,
3. profile-parity semantic digest gate for shared fixtures,
4. exhaustion/failure-atomic fixture set.

## 8. Downstream Bead Links

Primary implementation and gate consumers:

- `bd-j4m.*` (cross-vertical parity and router profile execution),
- `bd-ix8.4` and `bd-ix8.5` (embedded matrix + QEMU harness),
- `bd-66l.*` (hard quality gate enforcement).
