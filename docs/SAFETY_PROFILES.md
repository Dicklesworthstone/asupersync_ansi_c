# Safety Profiles

**Version:** 1.0
**Bead:** bd-hwb.7

## Overview

Safety profiles control the tradeoff between diagnostic depth and runtime overhead. All profiles produce identical semantic behavior for the same inputs — profiles affect observability and checking depth, never outcomes.

## Profile Matrix

| Property                  | Debug        | Hardened     | Release      |
|---------------------------|--------------|--------------|--------------|
| Compile flag              | `ASX_DEBUG=1`| (no `ASX_DEBUG`, no `NDEBUG`) | `NDEBUG`    |
| Ghost monitors            | Active       | Disabled     | Disabled     |
| Transition checks         | Active       | Active       | Active       |
| Generation-safe handles   | Active       | Active       | Active       |
| Quarantine mode           | Optional     | Disabled     | Disabled     |
| Error ledger              | Active       | Active       | Active       |
| Must-use enforcement      | Active       | Active       | Active       |
| Resource exhaustion checks| Active       | Active       | Active       |
| Deterministic mode        | Default on   | Default on   | Default on   |

## Profile Definitions

### Debug (`ASX_DEBUG=1`)

**Intent:** Maximum diagnostic depth for development and testing.

**Invariants:**
- Ghost protocol monitors validate every lifecycle state transition.
- Ghost linearity monitors detect obligation double-use and leaks.
- Violation ring buffer captures diagnostic breadcrumbs.
- Optional `ASX_DEBUG_QUARANTINE` prevents slot recycling to catch stale handles.
- Error ledger records all error propagation paths.

**Overhead:** Moderate. Ghost checks add O(1) per transition. Ring buffer is fixed-size (64 entries). Quarantine mode trades slot reuse for stale-handle detection.

**Compile defines:**
- `ASX_DEBUG=1` (auto-set when `NDEBUG` absent)
- `ASX_DEBUG_GHOST` (auto-set when `ASX_DEBUG` set)
- `ASX_DEBUG_GHOST_DISABLE` (opt-out from ghost monitors)
- `ASX_DEBUG_QUARANTINE` (opt-in slot quarantine)

### Hardened (no `ASX_DEBUG`, no `NDEBUG`)

**Intent:** Production safety without debug overhead.

**Invariants:**
- All transition checks enforced (state machine legality).
- All generation-safe handle validation active.
- All resource exhaustion checks active with stable error codes.
- Error ledger active for post-mortem analysis.
- Must-use enforcement active (compiler-dependent).
- Ghost monitors compiled out (zero overhead).

**Overhead:** Minimal. No ghost checks, no ring buffer, no quarantine. Only mandatory safety checks that prevent undefined behavior.

### Release (`NDEBUG` defined)

**Intent:** Minimum overhead for performance-critical deployments.

**Invariants:**
- Same as Hardened. No checks are removed by `NDEBUG`.
- `assert()` statements (if any) are disabled by `NDEBUG`.
- Ghost monitors compiled out.
- Error ledger still active (zero-allocation, negligible overhead).

**Overhead:** Same as Hardened. The `NDEBUG` flag only affects standard `assert()`.

## Mandatory Checks (All Profiles)

These checks are never disabled regardless of profile:

1. **Transition legality** — State machine transitions are always validated.
2. **Generation-safe handles** — Stale handle detection via generation counters.
3. **Resource exhaustion** — Arena capacity checks with `ASX_E_RESOURCE_EXHAUSTED`.
4. **Null argument validation** — All public API entry points validate pointers.
5. **Allocator seal** — Sealed allocator blocks allocation in all profiles.
6. **Deterministic mode** — Hook validation enforces determinism policy.

## Profile-Gated Checks (Debug Only)

These checks are active only in debug profile:

1. **Ghost protocol monitors** — Transition violation recording.
2. **Ghost linearity monitors** — Obligation double-use and leak detection.
3. **Ghost violation ring buffer** — Diagnostic breadcrumbs.
4. **Slot quarantine** — Optional stale-handle detection.

## Deterministic Fault Injection

Fault injection uses the hook contract — no additional API. Test fixtures provide custom hook implementations that simulate:

| Fault Class        | Hook                  | Simulation                    |
|--------------------|-----------------------|-------------------------------|
| Clock backward     | `clock.now_ns_fn`     | Return decreasing timestamps  |
| Clock overflow     | `clock.now_ns_fn`     | Return near-`UINT64_MAX`      |
| Entropy degenerate | `entropy.random_u64_fn` | Return constant or zero     |
| Allocator failure  | `allocator.malloc_fn` | Return `NULL` after N calls   |
| Allocator seal     | `asx_runtime_seal_allocator()` | Block all allocation |

All fault scenarios are covered by `tests/unit/core/test_fault_injection.c`.

## Platform Profiles

Platform profiles (`ASX_PROFILE_*`) control resource-plane adaptation (memory limits, wait policies, timer resolution) without affecting semantic behavior. All platform profiles share the same safety profile hierarchy.

| Platform Profile       | Default Wait Policy | Recommended Safety |
|------------------------|--------------------|--------------------|
| `ASX_PROFILE_CORE`     | `BUSY_SPIN`        | Debug or Hardened  |
| `ASX_PROFILE_POSIX`    | `YIELD`            | Any                |
| `ASX_PROFILE_WIN32`    | `YIELD`            | Any                |
| `ASX_PROFILE_FREESTANDING` | `BUSY_SPIN`    | Hardened           |
| `ASX_PROFILE_EMBEDDED_ROUTER` | `BUSY_SPIN` | Hardened           |
| `ASX_PROFILE_HFT`      | `BUSY_SPIN`        | Release            |
| `ASX_PROFILE_AUTOMOTIVE` | `SLEEP`           | Hardened           |

## Cross-Profile Compatibility Contract

This project allows profile-specific operational tuning, but forbids profile-specific semantic behavior. The compatibility boundary is enforced by parity fixtures and CI gates.

### Allowed Operational Differences (Tuning Plane)

| Category | Allowed Difference | Why Allowed |
|----------|--------------------|-------------|
| Wait policy | `BUSY_SPIN` vs `YIELD` vs `SLEEP` defaults | Scheduling efficiency and host integration |
| Resource ceilings | Memory/queue/timer limits by profile/resource class | Hardware-fit and deterministic exhaustion behavior |
| Telemetry level | Trace verbosity and diagnostics depth | Observability cost control |
| Integration hooks | Platform adapter wiring (POSIX/WIN32/freestanding hooks) | OS/runtime boundary adaptation |

### Forbidden Semantic Differences (Behavior Plane)

| Category | Forbidden Drift | Enforcement Signal |
|----------|-----------------|--------------------|
| Lifecycle legality | Any profile accepts/denies a transition differently | Invariant suites + conformance mismatch |
| Cancellation semantics | Different cancel phase progression or witness ordering | cancel fixtures + parity diff |
| Channel semantics | Profile-specific reserve/send/abort ordering behavior | channel fixtures + parity diff |
| Timer ordering | Non-identical equal-deadline ordering or stale-handle behavior | timer fixtures + parity diff |
| Replay identity | Same scenario produces different canonical digest | replay/profile parity failure |

### Machine-Checkable Gate

For shared fixture sets, the following must hold:

1. Same `scenario_id` + same semantic inputs -> identical canonical digest across participating profiles.
2. Any parity mismatch is classified as `c_regression` unless explicitly approved by decision log.
3. Empty fixture/parity sets fail closed in strict mode.

Operational verification commands:

```bash
make conformance
make codec-equivalence
make profile-parity
```

## Deployment Hardening Playbook Lanes (`bd-j4m.6`)

These lanes are the operational hardening surface for router/HFT/automotive
deployment stress and must remain runnable in CI and local environments.

### Canonical Lane Commands

```bash
rch exec -- tests/e2e/router_storm.sh
rch exec -- tests/e2e/market_open_burst.sh
rch exec -- tests/e2e/automotive_fault_burst.sh
rch exec -- tests/e2e/continuity_restart.sh
rch exec -- tests/e2e/run_all.sh
```

Lane IDs:

- `E2E-DEPLOY-ROUTER` -> `tests/e2e/router_storm.sh`
- `E2E-DEPLOY-HFT` -> `tests/e2e/market_open_burst.sh`
- `E2E-DEPLOY-AUTO` -> `tests/e2e/automotive_fault_burst.sh`
- `E2E-CONT-RESTART` -> `tests/e2e/continuity_restart.sh`

### Profile-Specific Watchdog / Restart / Safe-Upgrade Guidance

| Profile | Watchdog / fault policy | Restart semantics | Safe-upgrade guidance |
|---|---|---|---|
| `ASX_PROFILE_EMBEDDED_ROUTER` | `router_storm` validates churn, saturation, exhaustion, poison isolation, cancel under load | restart continuity must preserve deterministic digest for shared fixtures | validate package/install with `tests/e2e/openwrt_package.sh`, then run `router_storm` + `continuity_restart` before and after upgrade |
| `ASX_PROFILE_HFT` | `market_open_burst` validates overload pressure and deterministic recovery | restart must preserve event ordering/digest identity on fixed seed | quiesce ingress, drain, deploy, restart, then verify `market_open_burst` and digest parity (`conformance` + `profile-parity`) |
| `ASX_PROFILE_AUTOMOTIVE` | `automotive_fault_burst` validates clock/entropy faults, containment, and deadline-cancel behavior | degraded-mode transitions and restart path must remain deterministic and classified | run `automotive_fault_burst` and `continuity_restart`; reject release if deadline/watchdog evidence drifts or unclassified deltas appear |

### Low-Overhead Remote Diagnostics Framing

Use harness-native structured outputs only; do not add ad-hoc instrumentation on constrained links.

- stream JSONL records from `build/test-logs/e2e-*.jsonl`,
- ship family summaries from `build/e2e-artifacts/<run_id>/*.summary.json`,
- include rerun pointers from the `run_manifest.json` first-failure block.

Minimum remote frame fields:

`run_id`, `suite`, `test`, `status`, `profile`, `codec`, `seed`, `digest`, `error.message`, `rerun_command`.

Transport recommendation:

- compress JSONL + summaries (`tar` + `gzip`),
- attach `sha256` for bundle integrity,
- preserve file paths so first-failure rerun commands remain copy/paste valid.

### Evidence Linkage Matrix (Lane -> Unit/Invariant/Parity)

| Lane | Unit evidence | Invariant/model evidence | Parity evidence |
|---|---|---|---|
| `E2E-DEPLOY-ROUTER` (`router_storm`) | `tests/unit/runtime/test_profile_compat.c`, `tests/unit/runtime/test_adapter.c`, `tests/unit/runtime/test_overload_catalog.c` | `tests/invariant/lifecycle/test_lifecycle_legality.c`, `tests/invariant/model_check/test_bounded_model.c` | `make conformance`, `make profile-parity`, `tests/e2e/run_all.sh` |
| `E2E-DEPLOY-HFT` (`market_open_burst`) | `tests/unit/runtime/test_hft_microburst.c`, `tests/unit/runtime/test_hft_instrument.c`, `tests/unit/runtime/test_overload_catalog.c` | `tests/invariant/lifecycle/test_lifecycle_legality.c`, `tests/invariant/model_check/test_bounded_model.c` | `make conformance`, `make profile-parity`, `tests/e2e/run_all.sh` |
| `E2E-DEPLOY-AUTO` (`automotive_fault_burst`) | `tests/unit/runtime/test_automotive_fixtures.c`, `tests/unit/runtime/test_automotive_instrument.c`, `tests/unit/runtime/test_adapter.c` | `tests/invariant/lifecycle/test_lifecycle_legality.c`, `tests/invariant/model_check/test_bounded_model.c` | `make conformance`, `make profile-parity`, `tests/e2e/run_all.sh` |
| `E2E-CONT-RESTART` (`continuity_restart`) | `tests/unit/runtime/test_continuity.c`, `tests/unit/runtime/test_trace.c` | `tests/invariant/lifecycle/test_lifecycle_legality.c`, `tests/invariant/model_check/test_bounded_model.c` | `make conformance`, `make codec-equivalence`, `make profile-parity`, `tests/e2e/run_all.sh` |
