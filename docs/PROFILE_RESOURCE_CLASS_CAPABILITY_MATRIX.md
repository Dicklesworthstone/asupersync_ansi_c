# Profile and Resource-Class Capability Matrix

> User-visible limits, semantic guarantees, and fail-closed boundaries
> for every runtime profile and resource class.
>
> Bead: bd-1eqo.1.4
> SPDX-License-Identifier: MIT

---

## 1. Purpose

This document is the single planning anchor for profile and resource-class
parity. It defines:

- what each profile **promises** and what **limits** users should expect;
- which properties are **semantic** (identical everywhere) vs **operational** (may vary);
- which profile/feature combinations are **supported**, **degraded**, or **fail-closed**;
- how resource classes scale operational limits without altering semantics.

Downstream beads (bd-1eqo.2.4, bd-1eqo.2.6, bd-1eqo.4, bd-1eqo.5, bd-1eqo.9,
bd-1eqo.14, bd-1eqo.16) should reference this matrix instead of rediscovering
profile semantics.

---

## 2. Semantic Lock: Invariants Identical Across ALL Profiles

These 8 rules are non-negotiable. They are enforced at compile time,
tested via canonical digest comparison, and queryable at runtime via
`asx_profile_semantic_rule_enforced()` (always returns 1).

| Rule | Name | Contract |
|------|------|----------|
| 0 | Lifecycle transitions | Region, task, obligation state machines have identical legal transitions |
| 1 | Cancel protocol | Cancel-phase ordering, severity strengthen, cleanup budget mapping |
| 2 | Obligation linearity | reserve → commit XOR abort, exactly once, no double-use |
| 3 | Deterministic ordering | Same seed/input → same poll order, timer tiebreak, event sequence |
| 4 | Handle validation | Generation-safe, type-tagged, slot-recycling detects stale handles |
| 5 | Error codes | Same misuse → same `asx_status` return value |
| 6 | Quiescence definition | Quiescent iff all tasks complete + all obligations resolved |
| 7 | Budget exhaustion | Budget arithmetic (meet semilattice, poll/cost consume) is value-identical |

**Parity contract:** For any shared fixture F and profiles P1, P2:
`canonical_digest(F, P1) == canonical_digest(F, P2)`

---

## 3. Platform Profile Matrix

### 3.1 Profile Overview

| ID | Profile | Target Environment | Wait Policy | Build Flag |
|----|---------|-------------------|-------------|------------|
| 0 | CORE | Default / CI / development | Yield | `-DASX_PROFILE_CORE` (default) |
| 1 | POSIX | Linux/macOS production | Yield | `-DASX_PROFILE_POSIX` |
| 2 | WIN32 | Windows production | Yield | `-DASX_PROFILE_WIN32` |
| 3 | FREESTANDING | Bare-metal, no OS runtime | Yield | `-DASX_PROFILE_FREESTANDING` |
| 4 | EMBEDDED_ROUTER | OpenWrt/BusyBox class | Busy spin | `-DASX_PROFILE_EMBEDDED_ROUTER` |
| 5 | HFT | High-frequency trading | Busy spin | `-DASX_PROFILE_HFT` |
| 6 | AUTOMOTIVE | Safety-critical / deadline | Sleep | `-DASX_PROFILE_AUTOMOTIVE` |
| 7 | PARALLEL | Multi-threaded lane scheduler | Yield | `-DASX_PROFILE_PARALLEL` |
| 8 | BROWSER | Sandboxed browser/WASM boundary profile | Yield | `-DASX_PROFILE_BROWSER` |

### 3.2 Operational Parameters (R2 defaults)

| Property | CORE | POSIX | WIN32 | FREE | ROUTER | HFT | AUTO | PARALLEL | BROWSER |
|----------|------|-------|-------|------|--------|-----|------|----------|---------|
| Max regions | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 | 8 |
| Max tasks | 64 | 64 | 64 | 64 | 64 | 64 | 64 | 64 | 64 |
| Max obligations | 128 | 128 | 128 | 128 | 128 | 128 | 128 | 128 | 128 |
| Max timers | 128 | 128 | 128 | 128 | 128 | 128 | 128 | 128 | 128 |
| Trace capacity | 1024 | 1024 | 1024 | 256 | 256 | 1024 | 1024 | 1024 | 256 |
| Ghost monitors | debug | debug | debug | debug | debug | **no** | debug | debug | debug |
| Allocator sealable | no | no | no | **yes** | **yes** | no | no | no | **yes** |
| Default resource class | R2 | R2 | R2 | R2 | R2 | R2 | R2 | R2 | R2 |

**Key distinctions:**
- **HFT** disables ghost monitors entirely (even in debug) to eliminate jitter.
- **FREESTANDING**, **EMBEDDED_ROUTER**, and **BROWSER** enable allocator sealing.
- **FREESTANDING**, **EMBEDDED_ROUTER**, and **BROWSER** reduce trace capacity to 256.

### 3.3 Wait Policy Semantics

| Policy | Behavior | Profiles |
|--------|----------|----------|
| `ASX_WAIT_BUSY_SPIN` | Tight spin loop, no syscalls, lowest latency | EMBEDDED_ROUTER, HFT |
| `ASX_WAIT_YIELD` | Cooperative yield (platform-dependent), balanced | CORE, POSIX, WIN32, FREESTANDING, PARALLEL |
| `ASX_WAIT_SLEEP` | Sleep-based wait, power-efficient, deadline-safe | AUTOMOTIVE |

Wait policy is purely operational — it never changes which operations succeed, which errors are returned, or what the canonical digest is.

### 3.4 Platform Hooks

| Profile | Hook Source | Notes |
|---------|------------|-------|
| POSIX | `src/platform/posix/hooks.c` | Standard POSIX timing, entropy |
| WIN32 | `src/platform/win32/hooks.c` | Windows timing, entropy |
| FREESTANDING | `src/platform/freestanding/hooks.c` | Minimal stubs, user must provide |
| EMBEDDED_ROUTER | `src/platform/freestanding/hooks.c` | Shares freestanding hooks |
| Others | Built-in defaults | No platform-specific hooks needed |

---

## 4. Resource Classes

Resource classes are **capability envelopes**, not feature switches.
They scale operational limits while preserving identical semantic behavior.

### 4.1 Scaling Factors

| Class | Scale | Target Footprint | Trace Ring |
|-------|-------|------------------|------------|
| R1 (tight) | ½× | Very constrained (IoT, sensor) | 64 events |
| R2 (balanced) | 1× | Typical router-class device | 256 events |
| R3 (roomy) | 2× | Higher-capacity embedded/server | 1024 events |

### 4.2 Scaled Limits (from CORE R2 baseline)

| Resource | R1 | R2 | R3 |
|----------|----|----|-----|
| Max regions | 4 | 8 | 16 |
| Max tasks | 32 | 64 | 128 |
| Max obligations | 64 | 128 | 256 |
| Max timers | 64 | 128 | 256 |
| Trace capacity | varies* | varies* | varies* |

*Trace capacity is profile-dependent before class scaling. For CORE profile:
R1 = 512, R2 = 1024, R3 = 2048. For FREESTANDING: R1 = 128, R2 = 256, R3 = 512.
Minimum of 1 enforced for all scaled values.

### 4.3 Admission Gate Behavior

When a resource class limit is reached, the admission gate returns
`ASX_E_RESOURCE_EXHAUSTED`. This is deterministic and identical across profiles:

```
asx_resource_admit(ASX_RESOURCE_TASK, 1)  →  ASX_E_RESOURCE_EXHAUSTED
```

Users can query capacity at runtime:
- `asx_resource_capacity(kind)` — hard ceiling
- `asx_resource_used(kind)` — current count
- `asx_resource_remaining(kind)` — headroom
- `asx_resource_snapshot_get(kind, &snap)` — point-in-time diagnostic view

---

## 5. Safety Profiles (Orthogonal Axis)

Safety profiles are **orthogonal** to platform profiles. Any platform profile
can be combined with any safety level.

| Safety Level | Compile Flags | Ghost Monitors | Containment | Overhead |
|-------------|---------------|----------------|-------------|----------|
| DEBUG | `ASX_DEBUG=1` | Active | Fail-fast | Moderate |
| HARDENED | (neither) | Disabled | Poison region | Minimal |
| RELEASE | `NDEBUG` | Disabled | Error only | Minimal |

**Auto-selection:**
- `ASX_DEBUG=1` → DEBUG
- No flags → HARDENED (default production)
- `NDEBUG` → RELEASE

**Containment policy mapping:**

| Safety | Policy | On Invariant Violation |
|--------|--------|----------------------|
| DEBUG | `ASX_CONTAIN_FAIL_FAST` | Abort immediately |
| HARDENED | `ASX_CONTAIN_POISON_REGION` | Poison owning region, other regions continue |
| RELEASE | `ASX_CONTAIN_ERROR_ONLY` | Return error code, no halt |

---

## 6. Feature-Gated Capabilities

### 6.1 Parallel Scheduler

| Property | Value |
|----------|-------|
| Gate | `-DASX_PROFILE_PARALLEL` |
| When disabled | Zero-overhead stubs (no code generation) |
| Max workers | 4 |
| Max lanes | 3 (READY, CANCEL, TIMED) |
| Lane capacity | 64 tasks per lane |
| Fairness policies | ROUND_ROBIN, WEIGHTED, PRIORITY |
| Status | Walking skeleton (single-threaded simulation) |

### 6.2 Deterministic Mode

| Property | Value |
|----------|-------|
| Gate | `-DASX_DETERMINISTIC=1` (default: enabled) |
| When disabled | Wall clock, ambient entropy, no fault injection |
| When enabled | Logical clock, seeded PRNG, fault injection API active |
| Fault kinds | Clock skew, clock reverse, constant entropy, alloc fail |

### 6.3 Codec Selection

| Codec | Gate | Purpose |
|-------|------|---------|
| JSON | `-DASX_CODEC_JSON` | Default, human-readable serialization |
| Binary | `-DASX_CODEC_BIN` | Compact wire format |

Codec selection is orthogonal to profile and does not affect semantic behavior
(canonical digests are computed before serialization).

---

## 7. Supported Combinations Matrix

### 7.1 Profile × Resource Class

All combinations are supported. Recommended pairings marked with **★**:

| Profile | R1 | R2 | R3 |
|---------|----|----|-----|
| CORE | ✓ | **★** | ✓ |
| POSIX | ✓ | **★** | ✓ |
| WIN32 | ✓ | **★** | ✓ |
| FREESTANDING | ✓ | **★** | ✓ |
| EMBEDDED_ROUTER | **★** | ✓ | ✓ |
| HFT | ✓ | ✓ | **★** |
| AUTOMOTIVE | ✓ | **★** | ✓ |
| PARALLEL | ✓ | ✓ | **★** |
| BROWSER | **★** | ✓ | ✓ |

### 7.2 Profile × Safety Level

All combinations are supported:

| Profile | DEBUG | HARDENED | RELEASE |
|---------|-------|----------|---------|
| CORE | **★** dev | **★** CI | ✓ |
| POSIX | ✓ | **★** | ✓ |
| WIN32 | ✓ | **★** | ✓ |
| FREESTANDING | ✓ | **★** | ✓ |
| EMBEDDED_ROUTER | ✓ | **★** | ✓ |
| HFT | ✓* | ✓ | **★** |
| AUTOMOTIVE | ✓ | **★** | ✓ |
| PARALLEL | **★** | ✓ | ✓ |
| BROWSER | ✓ | **★** | ✓ |

*HFT + DEBUG: ghost monitors remain disabled (HFT override), but error ledger
and transition checks remain active.

### 7.3 Fail-Closed Combinations

The following are **not profiles** but rather **build-time validation**
boundaries that produce compile errors or deterministic runtime failures:

| Combination | Failure Mode | Rationale |
|-------------|-------------|-----------|
| Multiple `ASX_PROFILE_*` defined | Compile error (mutual exclusion) | Exactly one profile per build |
| DETERMINISTIC=0 + fault injection API | No-op stubs | Faults require deterministic mode |
| Allocator sealed + allocation attempt | `ASX_E_ALLOCATOR_SEALED` | Enforced after `asx_runtime_seal_allocator()` |
| Resource exhausted + admit | `ASX_E_RESOURCE_EXHAUSTED` | Deterministic rejection, not degradation |
| Invalid hook contract + deterministic mode | `ASX_E_INVALID_ARGUMENT` from `asx_runtime_hooks_validate()` | Ambient entropy forbidden in deterministic mode |

---

## 8. Upstream Crate-Contract Drift Audit (`bd-yx9r.2`)

The upstream Rust crate contract is broader than the current C profile matrix.
The table below maps the crate-root feature/platform restrictions from
`/dp/asupersync/src/lib.rs` to the current ANSI C state.

| Upstream contract | Current ANSI C equivalent | State | Evidence / gap |
|---|---|---|---|
| `wasm32` must choose exactly one canonical browser profile | `ASX_PROFILE_BROWSER` exists in `include/asx/asx_config.h`, `src/runtime/profile_compat.c`, the explicit `build-browser` / `PROFILE=BROWSER` lane, and the focused `test-browser-focused` CI/test lane in `Makefile` | `partial` | The browser build/test surface is now explicit and CI-covered, and `include/asx/asx_config.h` now hard-fails multiple `ASX_PROFILE_*` selections while declaring `ASX_BROWSER_PROFILE_MODE_COUNT == 1`, but there is still no compile-time browser subprofile split because the C port only exposes one browser mode today |
| `native-runtime` forbidden on browser builds | Browser boundary gates native surfaces and compile-time capability macros mark them unavailable | `partial` | `include/asx/runtime/browser_boundary.h` fail-closes filesystem/process/signal/io/blocking plus the shipped `server`/`grpc`/`messaging` families, and `include/asx/asx_config.h` now exposes `ASX_HAS_NATIVE_RUNTIME_SURFACES == 0` under browser builds, but native-only APIs are still publicly declared rather than removed from the browser umbrella surface |
| `browser-io` forbidden with upstream minimal browser profile | Native I/O blocked under browser boundary and compile-time capability macros | `partial` | `ASX_SURFACE_IO_DRIVER` is denied in browser mode and `include/asx/asx_config.h` now exposes `ASX_HAS_NATIVE_IO_DRIVER == 0`, but there is still no separate browser-IO feature flag or profile-minimal compile check |
| `browser-trace` forbidden with upstream minimal browser profile | No separate browser trace feature | `gap` | The C tree has browser diagnostics and trace support, but no compile-time browser trace subprofile split |
| `cli` unsupported on browser builds | No `cli` module in C | `gap` | The C port currently lacks a public CLI family entirely, so there is no feature-gated fail-closed browser rule to enforce |
| `io-uring` unsupported on browser builds | No io_uring feature surface in C | `mapped-by-absence` | No equivalent feature exists yet; once native reactor features expand, browser incompatibility must become explicit |
| `tls`, `tls-native-roots`, `tls-webpki-roots` unsupported on browser builds | Browser boundary denies the shipped TLS family in browser mode | `partial` | `ASX_SURFACE_TLS` is fail-closed at the boundary and in the status-returning TLS entry points, and `include/asx/asx_config.h` now exposes `ASX_HAS_TLS_SURFACE == 0` under browser builds, but there is still no sub-feature split for trust-root variants |
| `sqlite`, `postgres`, `mysql` unsupported on browser builds | Browser boundary denies the shipped database family in browser mode | `partial` | `ASX_SURFACE_DATABASE` is fail-closed at the boundary and in the status-returning database entry points, and `include/asx/asx_config.h` now exposes `ASX_HAS_DATABASE_SURFACE == 0` under browser builds, but there is still no backend-specific compile-time split |
| `kafka` unsupported on browser builds | Browser boundary denies the shipped messaging/broker family in browser mode | `partial` | `ASX_SURFACE_MESSAGING` is fail-closed at the boundary and in the status-returning messaging entry points, and `include/asx/asx_config.h` now exposes `ASX_HAS_MESSAGING_SURFACE == 0` under browser builds, but there is still no broker-specific compile-time split |
| Native-only modules (`fs`, `grpc`, `messaging`, `process`, `server`, `signal`) excluded from wasm32 | Browser boundary denies the currently shipped native-facing families | `partial` | The boundary now covers filesystem/process/signal/io/blocking plus explicit `server`/`grpc`/`messaging`/`tls`/`database` surfaces, and CI now runs focused browser-profile suites over shipped browser-safe `actor`/`sync`/`bytes`/`encoding`/`decoding`/`stream`/`security`/`plan`/`cx`/`link`/`app`/`console`/`tracing_compat`/`evidence`/`monitor`/`record`/`migration`/`raptorq`/`spork` plus `net`/`http`/`web`/`websocket`/`quic`/`distributed`/`pipe` and `service`/`transport`/`remote`; remaining debt is compile-time feature gating plus future higher-surface matrix growth |

### 8.1 Missing Fail-Closed Checks Before Parity Claims

The current C tree still needs these explicit fail-closed controls before it
can honestly claim equivalence with the upstream feature/platform matrix:

1. Compile-time mutual exclusion for browser-specific subprofiles if the C port
   adopts more than one browser mode.
2. Explicit compile-time removal of browser-incompatible APIs from the public
   browser umbrella/header surface rather than relying on capability macros plus
   runtime fail-closed boundary and browser-focused test lanes.
3. A single compatibility matrix that states which combinations are supported,
   denied, or currently unimplemented rather than leaving absence implicit.

---

## 9. User-Visible Limits Summary

### What users should expect:

1. **Semantic behavior is identical** across all profiles. If a test passes on
   CORE, it passes on HFT, AUTOMOTIVE, and FREESTANDING — same errors, same
   state transitions, same canonical digest.

2. **Operational limits vary** by profile and resource class. Code that spawns
   65 tasks succeeds on R3 but fails with `ASX_E_RESOURCE_EXHAUSTED` on R2.

3. **Ghost monitors are a debug-only tool.** They are compiled out in HARDENED
   and RELEASE. HFT never enables them. Users should not depend on ghost
   diagnostics in production.

4. **Allocator sealing is opt-in** and only meaningful on FREESTANDING and
   EMBEDDED_ROUTER. After sealing, all allocation paths return
   `ASX_E_ALLOCATOR_SEALED`.

5. **Wait policy affects latency, not correctness.** Busy-spin (HFT, ROUTER)
   gives lowest latency; sleep (AUTOMOTIVE) gives lowest power; yield (others)
   is balanced.

6. **Deterministic mode is on by default.** Disable with `-DASX_DETERMINISTIC=0`
   only when wall-clock timing and ambient entropy are acceptable.

---

## 10. Parity Obligations for Downstream Beads

| Downstream Bead | What This Matrix Provides |
|-----------------|--------------------------|
| bd-1eqo.2.4, bd-1eqo.2.6 | Harness must test all 8 profiles × at least R2; digest comparison is the parity gate |
| bd-1eqo.4 (runtime/scheduler) | Scheduler must respect profile descriptor limits; poll quota from budget, not profile |
| bd-1eqo.5 (time/cancel) | Cancel protocol is semantic (rule 1); timer limits are operational (resource class) |
| bd-1eqo.9 (bytes/codec/IO) | Codec is orthogonal; IO hooks are platform-specific but semantic contract is not |
| bd-1eqo.14 (native host) | Platform hooks vary; shutdown/bootstrap must follow lifecycle transitions (rule 0) |
| bd-1eqo.16 (browser/wasm) | Browser profile NOT yet defined; when added, must satisfy all 8 semantic rules |
| bd-reg6.1 (Rust baseline) | Rust fixture capture must record profile ID and resource class in metadata |
| bd-rkql (fuzz gate) | Fuzz harness already uses CORE/R2/DEBUG; parity gate should cross-check at least one other profile |

---

## 11. E2E Test Lanes by Profile

| Profile | E2E Script | Focus |
|---------|-----------|-------|
| CORE | `tests/e2e/test_foundational_contracts.sh` | Algebraic + unit evidence |
| EMBEDDED_ROUTER | `tests/e2e/router_storm.sh` | Capacity exhaustion under load |
| HFT | `tests/e2e/market_open_burst.sh` | Tail-latency, jitter governance |
| AUTOMOTIVE | `tests/e2e/automotive_fault_burst.sh` | Deadline/watchdog, degraded mode |
| (continuity) | `tests/e2e/continuity_restart.sh` | State persistence across restart |

---

## 12. Open Questions

1. **Browser profile lane:** The runtime, diagnostics, build surface, and CI
   check lane now exercise `ASX_PROFILE_BROWSER`, but the C port still has only
   one browser mode. We need an explicit decision on whether to formalize
   multiple browser subprofiles or keep a single browser boundary scaffold
   until the broader `web`/WASM surface is unblocked.

2. **Resource class R0 (micro):** Some IoT targets may need even tighter limits
   than R1 (e.g., 2 regions, 8 tasks). Not currently planned — evaluate when
   real hardware constraints arise.

3. **Profile-specific test fixtures:** Currently all profiles share the same
   fixture set. Profile-specific operational fixtures (e.g., testing R1
   exhaustion) are planned but not yet implemented.
