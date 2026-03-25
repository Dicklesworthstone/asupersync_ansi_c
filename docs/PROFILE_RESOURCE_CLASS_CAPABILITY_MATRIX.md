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
| `wasm32` must choose exactly one canonical browser profile | `ASX_PROFILE_BROWSER` exists in `include/asx/asx_config.h`, `src/runtime/profile_compat.c`, the explicit `build-browser` / `PROFILE=BROWSER` lane, and the focused `test-browser-focused` plus `test-browser-minimal-focused` CI/test lanes in `Makefile` | `partial` | The browser build/test surface is now explicit and CI-covered, `include/asx/asx_config.h` now hard-fails multiple `ASX_PROFILE_*` selections, browser builds now expose two compile-time browser subprofiles via `ASX_BROWSER_PROFILE_MODE_COUNT == 2` with default-extended semantics, and the new minimal-browser hidden-contract lane exercises `test_browser_boundary`, `encoding`, `decoding`, `lab`, `telemetry`, `regression_localize`, `replay`, and `tracing_compat`; remaining debt is deeper upstream feature granularity beyond the new minimal/extended split |
| `native-runtime` forbidden on browser builds | Browser boundary gates native surfaces, compile-time capability macros mark them unavailable, and the browser umbrella stops directly advertising native-only families | `partial` | `include/asx/runtime/browser_boundary.h` fail-closes filesystem/process/signal/io/blocking plus the shipped `server`/`grpc`/`messaging` families, `include/asx/asx_config.h` exposes `ASX_HAS_NATIVE_RUNTIME_SURFACES == 0` under browser builds, and `include/asx/asx.h` now omits direct inclusion of `fs`/`process`/`signal`, the native `app` bootstrap family, `cli`, `server`, `grpc`, `messaging`, `tls`, `db`, `blocking`, and `io_driver` when `ASX_PROFILE_BROWSER` is active. `include/asx/net/http.h` no longer drags in `fs.h` transitively, browser builds now hide the filesystem-backed `asx_http_serve_static()` helper itself, `include/asx/app/app.h` no longer drags `fs.h`/`process.h`/`signal.h` outside its native-surface guard, and `include/asx/fs/fs.h`, `include/asx/process/process.h`, `include/asx/signal/signal.h`, `include/asx/app/app.h`, `include/asx/cli/cli.h`, `include/asx/runtime/io_driver.h`, `include/asx/runtime/blocking.h`, `include/asx/net/server.h`, `include/asx/net/grpc.h`, `include/asx/net/messaging.h`, `include/asx/net/tls.h`, and `include/asx/net/db.h` now self-gate their native families directly. `tests/unit/runtime/test_rt.c` now also honors those compile-time-hidden blocking and IO-driver surfaces instead of assuming the browser umbrella still exposes native-only declarations; remaining debt is broader mixed-purpose API surfacing plus the lack of finer-grained subprofile removal |
| `browser-io` forbidden with upstream minimal browser profile | Minimal-browser now gates the adapter-based public I/O family at compile time | `partial` | `include/asx/asx_config.h` now exposes `ASX_HAS_BROWSER_IO == 0` under `ASX_BROWSER_PROFILE_MINIMAL`, `include/asx/asx.h` omits `bytes/io_adapter`, `encoding`, and `decoding` from the umbrella in that subprofile, the direct `include/asx/bytes/io_adapter.h`, `include/asx/encoding/encoding.h`, and `include/asx/decoding/decoding.h` headers now self-gate there, and `encoding.h` / `decoding.h` no longer drag `io_adapter.h` outside that subprofile guard; focused unit suites plus `test_browser_boundary` prove the split, and remaining debt is broader upstream browser feature granularity beyond this adapter-based I/O family |
| `browser-trace` forbidden with upstream minimal browser profile | Browser minimal-vs-extended subprofiles now gate the public trace family | `partial` | `include/asx/asx_config.h` now exposes `ASX_HAS_BROWSER_TRACE == 0` under `ASX_BROWSER_PROFILE_MINIMAL`, `include/asx/asx.h` omits `trace`/`telemetry`/`replay`/`lab`/`regression_localize`/`tracing_compat` from the umbrella in minimal browser builds, the direct headers now self-gate those families unless internal implementation files opt in via `ASX_INTERNAL_TRACE_FAMILY_ACCESS`, and `telemetry.h` / `replay.h` / `regression_localize.h` / `tracing_compat.h` no longer drag `trace.h` or `lab.h` outside those guards; remaining debt is broader upstream feature parity beyond this trace-family split |
| `cli` unsupported on browser builds | Public CLI family exists and is now browser-hidden | `partial` | The C tree ships `include/asx/cli/cli.h` and `src/cli/cli.c`, and browser builds now hide that family both from `include/asx/asx.h` and from direct `include/asx/cli/cli.h` inclusion via `ASX_HAS_NATIVE_RUNTIME_SURFACES`; remaining debt is broader compile-time/browser feature-matrix coverage beyond the CLI surface itself |
| `io-uring` unsupported on browser builds | `io_uring` is now modeled as an explicit runtime IO-backend selection contract that fails closed | `partial` | `include/asx/asx_config.h` now exposes `ASX_HAS_IO_URING == 0`, `asx_io_backend`, `asx_io_backend_str()`, and `asx_runtime_config_set_io_backend()`, `include/asx/runtime/builder.h` now accepts `IO_BACKEND=ghost|io-uring` in the builder/env surface, `src/runtime/rt.c` rejects unsupported `ASX_IO_BACKEND_IO_URING` selections with `ASX_E_PERMISSION_DENIED` during bootstrap, and `include/asx/runtime/io_driver.h` / `src/runtime/io_driver.c` now expose the selected initialized backend for focused tests; remaining debt is the actual native reactor/io_uring implementation rather than leaving the unsupported state implicit |
| `tls`, `tls-native-roots`, `tls-webpki-roots` unsupported on browser builds | Browser boundary denies the shipped TLS family in browser mode and the trust-root variants are now modeled in the public config surface as unsupported selections | `partial` | `ASX_SURFACE_TLS` is fail-closed at the boundary and in the status-returning TLS entry points, `include/asx/asx_config.h` exposes `ASX_HAS_TLS_SURFACE == 0` under browser builds, `include/asx/net/tls.h` now exposes `asx_tls_root_source` plus `asx_tls_config_set_root_source()`, and the trust-root variants remain explicit absences via `ASX_HAS_TLS_NATIVE_ROOTS == 0`, `ASX_HAS_TLS_WEBPKI_ROOTS == 0`, and `ASX_HAS_TLS_ROOT_VARIANTS == 0`, with unsupported selections returning `ASX_E_PERMISSION_DENIED`; remaining debt is actual implementation breadth rather than an unmodeled public contract |
| `sqlite`, `postgres`, `mysql` unsupported on browser builds | Browser boundary denies the shipped database family in browser mode and backend variants are now modeled in the public pool config surface as unsupported selections | `partial` | `ASX_SURFACE_DATABASE` is fail-closed at the boundary and in the status-returning database entry points, `include/asx/asx_config.h` exposes `ASX_HAS_DATABASE_SURFACE == 0` under browser builds, `include/asx/net/db.h` now exposes `asx_db_backend` plus `asx_db_pool_set_backend()`, and the backend-specific variants remain explicit absences via `ASX_HAS_DB_SQLITE_BACKEND == 0`, `ASX_HAS_DB_POSTGRES_BACKEND == 0`, `ASX_HAS_DB_MYSQL_BACKEND == 0`, and `ASX_HAS_DB_BACKEND_VARIANTS == 0`, with unsupported selections returning `ASX_E_PERMISSION_DENIED`; remaining debt is backend-specific implementation rather than an unmodeled public contract |
| `kafka` unsupported on browser builds | Browser boundary denies the shipped messaging/broker family in browser mode and Kafka is now modeled in the public broker config surface as an unsupported selection | `partial` | `ASX_SURFACE_MESSAGING` is fail-closed at the boundary and in the status-returning messaging entry points, `include/asx/asx_config.h` exposes `ASX_HAS_MESSAGING_SURFACE == 0` under browser builds, `include/asx/net/messaging.h` now exposes `asx_msg_backend` plus `asx_msg_broker_set_backend()`, and the broker-specific variant remains an explicit absence via `ASX_HAS_MESSAGING_KAFKA_BACKEND == 0` and `ASX_HAS_MESSAGING_BACKEND_VARIANTS == 0`, with unsupported selections returning `ASX_E_PERMISSION_DENIED`; remaining debt is actual backend-specific broker coverage rather than an unmodeled public contract |
| Native-only modules (`fs`, `grpc`, `messaging`, `process`, `server`, `signal`) excluded from wasm32 | Browser boundary denies the currently shipped native-facing families | `partial` | The boundary now covers filesystem/process/signal/io/blocking plus explicit `server`/`grpc`/`messaging`/`tls`/`database` surfaces, and CI now runs focused browser-profile suites over shipped browser-safe `runtime` contract coverage (`test_browser_boundary`, `test_browser_diagnostic`, `test_rt`, `test_blocking`, and `test_io_driver`), browser-hidden host/API exclusion suites (`test_cli`, `test_fs`, `test_process`, `test_signal`, `test_server`, `test_grpc`, `test_messaging`, `test_tls`, and `test_db`), plus `actor`/`sync`/`bytes`/`encoding`/`decoding`/`stream`/`security`/`plan`/`cx`/`link`/`app`/`console`/`tracing_compat`/`evidence`/`monitor`/`record`/`migration`/`raptorq`/`spork` and `net`/`http`/`web`/`websocket`/`quic`/`distributed`/`pipe` plus `service`/`transport`/`remote`; remaining debt is compile-time feature gating plus future higher-surface matrix growth |

### 8.1 Missing Fail-Closed Checks Before Parity Claims

The current C tree still needs these explicit fail-closed controls before it
can honestly claim equivalence with the upstream feature/platform matrix:

1. Explicit compile-time removal of browser-incompatible APIs from the public
   browser umbrella/header surface rather than relying on capability macros plus
   runtime fail-closed boundary and browser-focused test lanes.
2. A single compatibility matrix that states which combinations are supported,
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
| bd-1eqo.16 (browser/wasm) | Browser profile is now explicit; deeper browser subprofile and feature-split parity still must satisfy all 8 semantic rules |
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

1. **Browser feature granularity:** The runtime, diagnostics, build surface,
   and CI check lane now exercise `ASX_PROFILE_BROWSER`, and the C port now has
   explicit minimal/extended browser subprofiles. The remaining question is how
   far to mirror upstream feature granularity beyond the current trace-family
   split as the broader `web`/WASM surface expands.

2. **Resource class R0 (micro):** Some IoT targets may need even tighter limits
   than R1 (e.g., 2 regions, 8 tasks). Not currently planned — evaluate when
   real hardware constraints arise.

3. **Profile-specific test fixtures:** Currently all profiles share the same
   fixture set. Profile-specific operational fixtures (e.g., testing R1
   exhaustion) are planned but not yet implemented.
