# asupersync ANSI C (`asx`)

<div align="center">
  <img src="asupersync_ani_c_illustration.webp" alt="asupersync ANSI C (asx) - portable deterministic runtime in ANSI C">
</div>

<div align="center">

![C99](https://img.shields.io/badge/C-C99-00599C)
![No external deps](https://img.shields.io/badge/dependencies-none-brightgreen)
![Deterministic replay](https://img.shields.io/badge/replay-deterministic-orange)
![494 API functions](https://img.shields.io/badge/public%20API-494%20functions-blue)
![2011 tests](https://img.shields.io/badge/tests-2%2C011-brightgreen)
![9 profiles](https://img.shields.io/badge/profiles-9%20deployment%20targets-blue)
[![License: MIT+Rider](https://img.shields.io/badge/License-MIT%2BOpenAI%2FAnthropic%20Rider-blue.svg)](./LICENSE)

</div>

Portable, dependency-free async runtime in ANSI C with deterministic replay, strict resource contracts, and 9 deployment profiles spanning servers to low-cost routers. 494 public API functions across 28 subsystems, backed by 2,011 tests.

<div align="center">
<h3>Quick Install</h3>

```bash
curl -fsSL https://raw.githubusercontent.com/Dicklesworthstone/asupersync_ansi_c/main/scripts/install.sh | sh
```

</div>

## TL;DR

**The Problem:** Most C async runtimes force a tradeoff: speed without safety guarantees, or safety via heavyweight dependencies and platform lock-in. Embedded targets (routers, gateways, edge appliances) make this worse with hard memory limits and fragile storage. When something goes wrong, you can't reproduce it.

**The Solution:** `asx` ports asupersync's full semantic model to ANSI C: region/task/obligation lifecycle guarantees, structured cancellation, deterministic replay, strict OOM behavior, and profile-based deployment from HFT servers to $5 routers. Every scenario produces identical results given the same seed and input. Every failure can be reproduced.

### Why Use `asx`?

| Feature | What It Gives You |
|---|---|
| **494 public API functions across 28 subsystems** | Full async runtime: scheduler, channels, sync primitives, actors, combinators, timers, codecs, diagnostics, and more |
| **No external dependencies** | Pure C runtime core; ships into constrained and audited environments unchanged |
| **Deterministic replay and trace hashing** | Reproduce production failures exactly; diff behavior across builds, profiles, and codec modes |
| **Structured cancellation with witness protocol** | 11 cancel kinds with severity lattice, witness phase tracking, and bounded cleanup budgets |
| **11 async combinators** | Join, race, select, timeout, retry, bracket, pipeline, bulkhead, rate-limit, quorum, first-ok |
| **Circuit breaker and epoch-based execution** | Failure containment with open/half-open/closed states; phase-scoped execution with barrier triggers |
| **Dual codecs (JSON + binary)** | JSON for debug/conformance, binary for production; both produce equivalent semantic digests |
| **59 typed error codes with recovery guidance** | Each error has a category, recoverability class, recovery action, and backoff hints |
| **Resource contracts instead of silent degradation** | Explicit memory/queue/timer ceilings with deterministic failure taxonomy per resource class (R1/R2/R3) |
| **9 deployment profiles** | CORE, POSIX, WIN32, FREESTANDING, EMBEDDED_ROUTER, HFT, AUTOMOTIVE, PARALLEL, BROWSER |
| **2,011 tests across 161 files** | Unit, invariant, e2e, vignette, conformance, fuzz, and formal verification |
| **Cross-profile semantic parity gates** | All profiles produce identical semantic digests for shared fixture sets |

## Quick Example

```bash
# 1) Generate a default config tuned for constrained devices
asx init --profile embedded_router --resource-class R1 --output asx.toml

# 2) Run a scenario in deterministic mode with JSON codec
asx run --config asx.toml --scenario scenarios/bootstrap.asxs --codec json --seed 42

# 3) Export trace and semantic digest
asx trace export --format json --out run.trace.json
asx digest run.trace.json

# 4) Replay the same trace and verify identity
asx replay --config asx.toml --trace run.trace.json --verify-digest

# 5) Switch to binary codec for production profile
asx run --config asx.toml --scenario scenarios/bootstrap.asxs --codec bin --seed 42

# 6) Assert JSON vs BIN semantic equivalence
asx conformance codec-equivalence --scenario scenarios/bootstrap.asxs

# 7) Differential check against Rust fixtures
asx conformance rust-parity --fixtures fixtures/rust_reference

# 8) Run stress + fuzz in CI mode
asx fuzz --target parity --time-budget 120s --minimize
```

## C API Quick Start

```c
#include <asx/asx.h>

static asx_status noop_poll(void *user_data, asx_task_id self) {
    (void)user_data;
    (void)self;
    return ASX_OK;
}

int main(void) {
    asx_runtime rt;
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_region_id rid;
    asx_task_id tid;
    asx_budget budget;
    asx_status st;

    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return 1;

    st = asx_runtime_init(&rt, &cfg, &hooks);
    if (st != ASX_OK) return 1;

    st = asx_region_open(&rid);
    if (st != ASX_OK) {
        asx_runtime_shutdown(&rt);
        return 1;
    }

    st = asx_task_spawn(rid, noop_poll, NULL, &tid);
    if (st != ASX_OK) {
        asx_runtime_shutdown(&rt);
        return 1;
    }

    budget = asx_budget_from_polls(64u);
    st = asx_scheduler_run(rid, &budget);
    asx_runtime_shutdown(&rt);
    return st == ASX_OK ? 0 : 2;
}
```

See `examples/` for more: channel flow, actor supervision, cancellation draining, timeout/deadline patterns, lab/replay mode, browser boundary, and network surface demos.

## Design Philosophy

1. **Semantics first, mechanics second**
   The runtime never trades away lifecycle correctness, cancellation semantics, or obligation linearity for speed hacks. State machines are explicit. Transitions are validated.

2. **Determinism is a feature, not a debug trick**
   Reproducibility is part of the contract. If a failure happened once, you can replay it with the same seed and reason about it. Trace digests are hash-stable across runs.

3. **Resource pressure must be explicit**
   In constrained systems, "best effort" often means undefined behavior. `asx` uses deterministic exhaustion and failure-atomic boundaries. Every resource class (R1/R2/R3) publishes concrete limits.

4. **Portable core, specialized adapters**
   Core logic is platform-neutral ANSI C. OS- or device-specific behavior lives behind profile/platform adapters. The same semantics run on Linux servers, Windows workstations, bare-metal routers, and WASM in browsers.

5. **Evidence-gated optimization**
   Performance work requires baseline artifacts, hotspot evidence, semantic proof, and rollback path. HFT and automotive profiles have built-in instrumentation for latency histograms, jitter tracking, and deadline compliance.

## How `asx` Compares

| Capability | `asx` | Ad-hoc C event loops | Generic async frameworks | Rust asupersync |
|---|---|---|---|---|
| Region/task/obligation semantic model | Full | Usually absent | Partial/varies | Full |
| Structured cancellation (11 kinds, witness protocol) | Built-in | Manual | Framework-specific | Built-in |
| Deterministic replay hash chain | Built-in | No | Rare | Built-in |
| Strict OOM/exhaustion semantics | Contractual | No | Framework-specific | Contractual |
| Async combinators (join/race/select/retry/bracket) | 11 built-in | DIY | Varies | Built-in |
| Circuit breaker + epoch scoping | Built-in | No | Sometimes | Library |
| Zero external runtime deps | Yes | Yes | Usually no | No (Rust toolchain) |
| Embedded router profile (OpenWrt/QEMU) | Yes | DIY | Usually too heavy | Target constraints |
| HFT tail-latency instrumentation | Built-in | No | No | No |
| Automotive deadline/watchdog compliance | Built-in | No | No | No |
| Rust parity conformance suite | Yes | N/A | N/A | N/A |
| Formal verification (CBMC, algebraic laws) | Yes | No | Rare | Partial |

**Use `asx` when you need:**
- hard behavioral guarantees in plain C,
- deterministic incident reproduction,
- embedded viability without feature amputations,
- one codebase deployable across 9 profiles from HFT to routers.

**Use alternatives when you need:**
- rapid app scaffolding over strict semantic control,
- large existing ecosystem integrations that outweigh runtime guarantees.

## Installation

### 1) Quick Install (Recommended)

```bash
curl -fsSL https://raw.githubusercontent.com/Dicklesworthstone/asupersync_ansi_c/main/scripts/install.sh | sh
```

Install script behavior:
- detects target architecture,
- installs `asx` binary + man page,
- keeps core/runtime dependency-free.

### 2) Package Managers

```bash
# Homebrew
brew install dicklesworthstone/tap/asx

# Debian/Ubuntu
sudo apt install asx

# OpenWrt/embedded (opkg feed)
opkg update
opkg install asx
```

### 3) From Source

```bash
git clone https://github.com/Dicklesworthstone/asupersync_ansi_c.git
cd asupersync_ansi_c
make release
sudo make install

# Convenience profile lanes
make build-browser
make build-parallel  # compile-only deferred scaffold; not a Wave A parity/CI lane
```

### 4) Cross-Compile for Embedded Targets

```bash
# mipsel OpenWrt
make release TARGET=mipsel-openwrt-linux-musl

# armv7 OpenWrt
make release TARGET=armv7-openwrt-linux-muslgnueabi

# Bare-metal ARM Cortex-M4 (freestanding)
make cross-baremetal-arm-m4-free

# Bare-metal RISC-V 32 (router profile)
make cross-baremetal-riscv32-router
```

## Quick Start

1. Create config:
```bash
asx init --profile embedded_router --resource-class R2 --output asx.toml
```
2. Validate environment:
```bash
asx doctor --config asx.toml
```
3. Run a scenario:
```bash
asx run --config asx.toml --scenario scenarios/hello_world.asxs --seed 7 --codec json
```
4. Inspect trace + digest:
```bash
asx trace export --format json --out trace.json
asx digest trace.json
```
5. Enable binary codec:
```bash
asx run --config asx.toml --scenario scenarios/hello_world.asxs --seed 7 --codec bin
```
6. Verify parity:
```bash
asx conformance codec-equivalence --scenario scenarios/hello_world.asxs
asx conformance rust-parity --fixtures fixtures/rust_reference
```

## Subsystem Overview

`asx` is organized into 28 subsystem families. Every subsystem has a public header, implementation, and dedicated tests.

| Subsystem | Purpose |
|---|---|
| **core** | Fundamental types: IDs, outcomes, budgets, symbols, cancellation, ghost monitors, combinators, epochs, circuit breakers |
| **runtime** | Scheduler, lifecycle engine, builder, blocking pool, I/O driver, deadline monitor, waker system, virtual time, telemetry, diagnostics, HFT/automotive instrumentation |
| **channel** | Bounded MPSC, oneshot, broadcast, watch channels, and session endpoints |
| **sync** | Mutex, semaphore, barrier (N-way rendezvous with leader election), once, notify |
| **actor** | Actor model with supervision trees |
| **cx** | Capability context and structured concurrency scoping |
| **codec** | JSON + binary codecs with equivalence checking and schema validation |
| **time** | Deadline abstraction, sleep primitives, timer wheel with generation-safe handles |
| **stream** | Poll-based async iterators and streaming combinators |
| **bytes** | Buffer management, codec bridges, I/O adapters |
| **security** | Audit trails and security policy enforcement |
| **net** | Network surface API |
| **fs** | File system operations |
| **process** | Child process lifecycle |
| **signal** | OS signal handling |
| **obligation** | Structured obligation lifecycle (reserve/commit/abort/leak tracking) |
| **session** | Bidirectional session endpoints |
| **link** | Long-lived coordination links between tasks |
| **record** | Event-log and snapshot grouping for audit/replay |
| **evidence** | Evidence collection and severity-based classification |
| **evidence_sink** | Evidence aggregation with pass/warn/fail verdict derivation |
| **monitor** | Runtime monitoring with threshold-based policy evaluation |
| **observability** | Observability snapshots and instrumentation hooks |
| **plan** | Planning and rewrite operations |
| **app** | Application utilities: doctor diagnostics, reporting |
| **console** | Console I/O and formatting |
| **tracing_compat** | Tracing compatibility layer for integration with external systems |
| **platform** | POSIX, Win32, and freestanding adapters |

## Command Reference

Global flags:

```bash
--config <path>         # Config file path (default: ./asx.toml)
--profile <name>        # core|posix|win32|freestanding|embedded_router|hft|automotive|parallel|browser
--resource-class <R1|R2|R3>
--codec <json|bin>
--deterministic
--seed <u64>
--format <text|json>
--verbose
```

### `asx init`

Generate starter config.

```bash
asx init --profile core --output asx.toml
asx init --profile embedded_router --resource-class R1 --output asx.toml
asx init --profile hft --output asx.toml
```

### `asx run`

Run a scenario or workload through the runtime kernel.

```bash
asx run --scenario scenarios/pipeline.asxs
asx run --scenario scenarios/pipeline.asxs --codec bin --deterministic --seed 123
```

### `asx replay`

Replay a prior trace and verify deterministic identity.

```bash
asx replay --trace run.trace.json --verify-digest
asx replay --trace run.trace.bin --verify-digest --format json
```

### `asx trace export`

Export runtime event stream.

```bash
asx trace export --format json --out trace.json
asx trace export --format bin --out trace.bin
```

### `asx digest`

Compute canonical semantic digest from a trace.

```bash
asx digest trace.json
asx digest trace.bin
```

### `asx conformance`

Conformance and parity tools.

```bash
# C runtime vs Rust fixture parity
asx conformance rust-parity --fixtures fixtures/rust_reference

# JSON codec vs BIN codec semantic equivalence
asx conformance codec-equivalence --scenario scenarios/all

# Profile semantic parity (core/freestanding/embedded_router)
asx conformance profile-parity --scenario scenarios/all
```

### `asx fuzz`

Differential fuzzing with automatic counterexample minimization.

```bash
asx fuzz --target parity --time-budget 300s --minimize
asx fuzz --target scheduler --seed 5 --cases 200000
```

### `asx bench`

Micro and scenario benchmarks.

```bash
asx bench --suite core
asx bench --suite embedded --profile embedded_router --resource-class R1
```

### `asx doctor`

Validate config, platform hooks, resource ceilings, and profile readiness.

```bash
asx doctor --config asx.toml
asx doctor --config asx.toml --format json
```

### `asx inspect`

Inspect runtime state and resource contract counters.

```bash
asx inspect --snapshot current
asx inspect --snapshot current --format json
```

## Configuration

Example `asx.toml`:

```toml
# Runtime profile
profile = "embedded_router"        # core | posix | win32 | freestanding | embedded_router | hft | automotive | parallel | browser
resource_class = "R1"              # R1 (tight), R2 (balanced), R3 (roomy)

# Deterministic execution
deterministic = true
seed = 42

# Codec selection
codec = "bin"                       # json or bin

[resource_contract]
# Hard limits (failure-atomic when exceeded)
max_runtime_bytes = 4194304         # 4 MiB
max_ready_queue = 4096
max_cancel_queue = 2048
max_timer_nodes = 8192
max_trace_events = 65536

[resource_contract.behavior]
oom_policy = "fail_atomic"          # fail_atomic | abort_process
queue_overflow = "reject"           # reject | backpressure
timer_overflow = "reject"           # reject | backpressure

[trace]
enabled = true
mode = "ram_ring"                   # ram_ring | persistent_spill
ring_bytes = 524288                 # 512 KiB
persistent_path = "/var/log/asx.trace.bin"
flush_interval_ms = 500
wear_safe = true

[conformance]
fixtures_path = "fixtures/rust_reference"
require_profile_parity = true
require_codec_equivalence = true

[platform]
# Hooks are required for freestanding/embedded targets
clock = "monotonic"
entropy = "deterministic_prng"      # deterministic_prng in deterministic mode
log_sink = "stderr"
```

### Resource Classes

Each resource class defines concrete capacity limits:

| Resource | R1 (Tight) | R2 (Balanced) | R3 (Roomy) |
|---|---|---|---|
| Max regions | 4 | 16 | 64 |
| Max tasks | 16 | 64 | 256 |
| Max timers | 32 | 128 | 512 |
| Max obligations | 16 | 64 | 256 |
| Max channels | 8 | 32 | 128 |
| Max trace events | 64 | 256 | 1,024 |

## Deployment Profiles

| Profile | Target | Key Properties |
|---|---|---|
| `ASX_PROFILE_CORE` | General-purpose (default) | Deterministic single-thread kernel, no OS assumptions |
| `ASX_PROFILE_POSIX` | Linux/macOS | Optional worker/reactor integration |
| `ASX_PROFILE_WIN32` | Windows | Windows runtime integration |
| `ASX_PROFILE_FREESTANDING` | Bare-metal/embedded | User-supplied hooks, no FS/network assumptions |
| `ASX_PROFILE_EMBEDDED_ROUTER` | OpenWrt/router-class | Low-memory defaults, RAM-ring diagnostics, wear-safe tracing |
| `ASX_PROFILE_HFT` | High-frequency trading | Tail-latency histograms, jitter tracking, overload admission control |
| `ASX_PROFILE_AUTOMOTIVE` | Safety-critical systems | Deadline tracking, watchdog monitoring, degraded-mode audit, compliance gates |
| `ASX_PROFILE_PARALLEL` | Multi-core | Parallel execution emphasis |
| `ASX_PROFILE_BROWSER` | WebAssembly | Browser boundary enforcement, surface gating |

All profiles produce identical canonical semantic digests for shared fixture sets. Profiles control operational envelopes (limits, defaults, instrumentation), never semantic behavior.

## Architecture

```text
                       ┌──────────────────────────────────────────┐
                       │                Inputs                    │
                       │  scenarios | API calls | trace files     │
                       └──────────────────────────────────────────┘
                                          │
                                          ▼
┌───────────────────────────────────────────────────────────────────────┐
│                      asx_core (15 modules)                            │
│  IDs + generation counters | outcomes | budgets | cancel + witness    │
│  combinators (11) | symbols + typed values | epochs | circuit breakers│
│  ghost monitors | error taxonomy (59 codes) | codec schema            │
└───────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
┌───────────────────────────────────────────────────────────────────────┐
│                   asx_runtime_kernel (37 modules)                     │
│  scheduler | builder | region/task/obligation lifecycle | cancel      │
│  timer wheel | waker | blocking pool | I/O driver | deadline monitor  │
│  telemetry | trace | replay | virtual time | event log | snapshot     │
│  HFT instrument | automotive instrument | diagnostics | config reload │
└───────────────────────────────────────────────────────────────────────┘
      │             │             │             │             │
      ▼             ▼             ▼             ▼             ▼
┌────────────┐┌────────────┐┌────────────┐┌────────────┐┌────────────┐
│  Channels  ││    Sync    ││   Actors   ││   Codecs   ││  Profiles  │
│ MPSC,1shot ││ Mutex,Sem  ││ Supervisor ││  JSON+BIN  ││ 9 targets  │
│ Bcast,Watch││ Barrier    ││ Mailbox    ││  Schema    ││ 3 platform │
│ Session    ││ Once,Notify││            ││  Equiv     ││  adapters  │
└────────────┘└────────────┘└────────────┘└────────────┘└────────────┘
      │             │             │             │             │
      └─────────────┴─────────────┴──────┬──────┴─────────────┘
                                         ▼
           ┌─────────────────────────────────────────────────────┐
           │         Observability + Evidence Layer              │
           │  evidence sinks | monitors | diagnostics | audit    │
           │  telemetry digests | conformance | inspection       │
           └─────────────────────────────────────────────────────┘
                      │                             │
                      ▼                             ▼
         ┌──────────────────────┐     ┌──────────────────────────┐
         │ Trace/Replay/Fuzz   │     │  Runtime Deployments      │
         │ digest-stable       │     │  server | router | edge   │
         │ evidence artifacts  │     │  HFT | automotive | WASM  │
         └──────────────────────┘     └──────────────────────────┘
```

## Repository Layout

```text
include/asx/                 95 public C headers across 28 subsystem families
  asx.h                      Umbrella header (single #include entry point)
  asx_status.h               59 error codes with categories and recovery guidance
  asx_config.h               Profile, resource class, hook, and fault injection types
  asx_ids.h                  Handle types, type tags, lifecycle enums, cancel kinds
  core/                      19 headers: symbols, budgets, cancel, combinators, epochs, circuit breakers
  runtime/                   28 headers: scheduler, builder, blocking, I/O, deadline, HFT, automotive
  channel/                   Channel family headers (MPSC, oneshot, broadcast, watch, session)
  sync/                      Sync primitives (mutex, semaphore, barrier, once, notify)
  codec/                     Codec abstraction + equivalence checking
  ...                        + actor, cx, time, bytes, stream, security, net, fs, evidence, monitor, etc.

src/                         95 C source files (~25,500 LOC)
  core/                      15 files: status, cancel, combinators, symbols, epochs, circuit breakers
  runtime/                   37 files: scheduler, lifecycle, builder, blocking, I/O, deadline, instruments
  channel/                   6 files: MPSC, oneshot, broadcast, watch, session
  sync/                      5 files | actor/ 2 files | time/ 3 files | bytes/ 3 files
  platform/                  3 files: POSIX, Win32, freestanding adapters
  ...                        + cx, codec, security, net, fs, process, signal, stream, evidence, etc.

tests/                       161 test files (~48,500 LOC), 2,011 test cases
  unit/                      116 files across 20 subsystem directories
  e2e/                       17 end-to-end scenario programs
  invariant/                 3 lifecycle/quiescence invariant suites
  vignettes/                 11 API ergonomics demonstrations
  conformance/               Rust parity + codec/profile equivalence
  fuzz/                      4 differential fuzzing harnesses
  formal/                    9 algebraic, CBMC, and litmus verification

examples/                    8 example programs
fixtures/rust_reference/     Canonical fixtures captured from Rust runtime
tools/                       CI, capture, replay, fuzz, and minimization tooling
docs/                        Port architecture, parity tracking, deployment hardening
```

## Performance and Footprint

Guaranteed/targeted properties in production builds:

- O(1) amortized ready/cancel queue operations,
- O(1) timer cancel via generation-safe handles,
- steady-state scheduler hot path without heap allocations,
- deterministic digest stability across repeated runs with identical seed/input,
- embedded resource-class operation with deterministic exhaustion behavior,
- zero dynamic allocation in core paths (static arenas with configurable capacity).

Recommended benchmark flow:

```bash
asx bench --suite core
asx bench --suite embedded --profile embedded_router --resource-class R1
asx conformance profile-parity --scenario scenarios/perf-critical
```

## Testing and Quality Gates

`asx` ships with 2,011 test cases across 161 files covering 7 categories:

| Category | Files | Coverage |
|---|---|---|
| **Unit tests** | 116 | Every public API function across all 28 subsystems |
| **End-to-end scenarios** | 17 | Core lifecycle, automotive, HFT, codec parity, continuity, browser, network |
| **Invariant tests** | 3 | Lifecycle transition legality, quiescence, obligation linearity |
| **API vignettes** | 11 | Ergonomics and usage pattern demonstrations |
| **Conformance** | 1 | Rust parity + codec/profile semantic equivalence |
| **Differential fuzz** | 4 | Rust-vs-C drift detection + deterministic minimization |
| **Formal verification** | 9 | Algebraic laws, CBMC bounded model checking, litmus tests |

CI command set:

```bash
make test               # All tests (unit + invariant + vignette)
make test-unit          # 116 unit test suites
make test-invariants    # Lifecycle and quiescence invariants
make test-vignettes     # API ergonomics demonstrations
make test-e2e           # End-to-end scenario lanes
make conformance        # Rust fixture parity
make codec-equivalence  # JSON vs BIN semantic equivalence
make profile-parity     # Cross-profile semantic digest comparison
make fuzz-smoke         # Differential fuzzing smoke
make formal-check       # All formal verification (CBMC + algebraic + litmus)
make ci-embedded-matrix # Cross-target embedded builds + QEMU
make check              # Full CI gate: format + lint + build + all tests
```

## Release Artifacts and Integrity

Tag-driven release automation (`.github/workflows/release.yml`) emits deterministic
asset bundles and integrity metadata per target.

Per-target release contract:

- `asx-<target>.tar.xz`
- `asx-<target>.tar.xz.sha256`
- `asx-<target>.tar.xz.sigstore.json`
- `asx-<target>.provenance.json`

Build artifacts locally:

```bash
# Binary package (libasx.a + public headers)
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=linux-x86_64 PROFILE=CORE CODEC=BIN DETERMINISTIC=1

# Source package
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=source RELEASE_KIND=source
```

Notes:
- `ASX_ENABLE_SIGSTORE=1` enables keyless Sigstore bundle generation when `cosign` is available.
- `ASX_USE_RCH=auto` keeps local release builds compatible with remote build offload.
- Operational release/rollback checklist: `docs/DEPLOYMENT_HARDENING.md` ("Release Verification and Rollback Runbook").

## How It Works: Internal Design

This section covers the algorithms, data structures, and design patterns behind the API surface. None of this is required to use the library, but it helps when reasoning about performance characteristics and failure modes.

### Generation-Safe Handles

Every externally visible entity (region, task, obligation, timer, channel, cancel witness) is represented as a 64-bit opaque handle with a packed layout:

```
[16-bit type_tag | 16-bit state_mask | 16-bit generation | 16-bit slot_index]
```

When a slot is recycled, its generation counter increments (skipping zero to avoid sentinel collision). Old handles still carry the old generation, so any access attempt fails with `ASX_E_STALE_HANDLE` in O(1): a single `uint16_t` comparison, no hash table lookups or reference counting.

The state mask enables O(1) admission gating: each handle carries a bitmask of the entity's current lifecycle state, so callers can check `(handle.state_mask & allowed_mask) != 0` without reading the arena slot. The usual "look up entity, check state, proceed" pattern collapses to a bitmask test on the handle itself.

Six type tags (region, task, obligation, cancel_witness, timer, channel) are validated on every lookup. Type confusion across entity families is caught at the handle layer, not deep inside business logic.

### Deterministic Round-Robin Scheduler

The scheduler uses a flat arena-based polling strategy that processes all non-terminal tasks in ascending arena index order within each round. This deterministic tie-break means: given the same set of tasks and the same seed, the scheduler produces identical event sequences across runs, platforms, and profiles.

Each round:
1. Iterates all task slots in index order.
2. Skips terminal tasks (COMPLETED, not alive).
3. Transitions CREATED tasks to RUNNING on first poll.
4. Calls each task's poll function, consuming one poll unit per call.
5. Joins error outcomes into the task's severity lattice (Cancelled > Err > Ok).
6. Checks budget exhaustion after each poll; returns `ASX_E_POLL_BUDGET_EXHAUSTED` if quota hits zero.
7. Terminates when `active_count == 0` (quiescent state).

Tasks in the FINALIZING phase complete without consuming poll units, ensuring cleanup can finish even under tight budgets. Tasks in the CANCELLING phase with exhausted cleanup budgets are force-completed; the runtime never blocks indefinitely on a misbehaving cleanup handler.

### Budget Algebra

Budgets are composable resource constraints with four dimensions: **deadline** (absolute nanosecond timestamp), **poll quota** (max poll calls), **cost quota** (abstract cost units), and **priority** (scheduling weight).

The key operation is the **meet** (componentwise tightening):

```
meet(a, b).deadline   = min(a.deadline,   b.deadline)     // 0 = unconstrained
meet(a, b).poll_quota = min(a.poll_quota, b.poll_quota)
meet(a, b).cost_quota = min(a.cost_quota, b.cost_quota)   // UINT64_MAX = unconstrained
meet(a, b).priority   = min(a.priority,   b.priority)
```

This means budgets compose correctly: if a task has a 100-poll budget and its region has a 50-poll budget, the effective budget is 50. Cleanup budgets for cancellation follow the same rule; severity 5 (SHUTDOWN) gets 50 cleanup polls, while severity 0 (USER) gets 1,000.

### Cancellation Protocol and Severity Lattice

Cancellation in `asx` is a structured protocol with 11 cancel kinds organized into a 6-level severity lattice:

| Severity | Kinds | Cleanup Budget |
|---|---|---|
| 0 | `USER` | 1,000 polls |
| 1 | `TIMEOUT`, `DEADLINE` | 500 polls |
| 2 | `POLL_QUOTA`, `COST_BUDGET` | 300 polls |
| 3 | `FAIL_FAST`, `RACE_LOST`, `LINKED_EXIT` | 200 polls |
| 4 | `PARENT`, `RESOURCE` | 200 polls |
| 5 | `SHUTDOWN` | 50 polls |

Cancellation strength can only increase, never decrease. If a task already has a pending `TIMEOUT` cancel (severity 1) and receives a `PARENT` cancel (severity 4), the stronger cancel wins. This monotonicity guarantee means cancel waves never weaken; the worst cancellation in the chain determines the cleanup budget.

The cancellation state machine progresses through four phases:

```
Running → CancelRequested → Cancelling → Finalizing → Completed
```

Each cancel carries an **origin attribution chain** (source region, source task, timestamp, message) so propagation can be traced across cancel waves. The chain is bounded to prevent unbounded allocation.

### Two-Phase Channel Protocol

MPSC channels use a two-phase send protocol to enable backpressure without blocking:

1. **Reserve**: Claims one slot in the bounded queue, returning a permit token. If `(queue_len + reserved_count) >= capacity`, returns `ASX_E_CHANNEL_FULL` immediately.
2. **Send** (via permit): Enqueues the value FIFO. The permit is consumed.
   **OR Abort** (via permit): Returns the slot without enqueuing. The permit is consumed.

This separation means a sender can check capacity before committing to a send, and can back out without data loss. The capacity invariant `queue_len + reserved_count <= capacity` is maintained atomically. Permit tokens are monotonic (skipping zero as a sentinel) to prevent token reuse.

Channel lifecycle flows through OPEN, SENDER_CLOSED, RECEIVER_CLOSED, and FULLY_CLOSED states with appropriate error codes (`ASX_E_DISCONNECTED`, `ASX_E_WOULD_BLOCK`) at each transition.

### Timer Wheel with Deterministic Ordering

The timer wheel uses a flat arena of `ASX_MAX_TIMERS` slots with O(1) cancel via generation-safe handles. When timers are collected (deadline <= now), they are sorted by a composite key: **(deadline ascending, insertion_sequence ascending)**.

The insertion sequence is a per-wheel monotonic counter incremented on every registration. This secondary key guarantees that timers with identical deadlines fire in FIFO registration order, which is critical for deterministic replay. Without this, hash-table iteration order or memory layout could cause non-determinism.

Duration validation rejects timers that would overflow `uint64_t` nanoseconds (`ASX_E_TIMER_DURATION_EXCEEDED`), preventing silent wraparound on extremely long timeouts.

### Ghost Monitors (Debug-Mode Safety Net)

Ghost monitors are compile-time gated behind `ASX_DEBUG_GHOST`, with zero overhead in release builds. They provide three classes of runtime verification:

**Protocol Monitor**: Validates every region, task, and obligation state transition against precomputed transition tables. Invalid transitions (e.g., COMPLETED → RUNNING) are caught immediately and recorded in a ring buffer with entity ID, from-state, to-state, and sequence number. This catches state machine corruption that would otherwise manifest as subtle downstream bugs.

**Linearity Monitor**: Tracks obligation reserve/commit/abort lifecycle and detects double-resolution (calling both commit and abort on the same obligation) and leaks (reserved but never resolved). Each obligation gets exactly one resolution in a valid program; the linearity monitor enforces this at runtime in debug builds.

**Borrow Ledger**: Emulates Rust's borrow checker rules in C. Tracks shared and exclusive borrows per entity and enforces "multiple shared XOR one exclusive, never both." A borrow table of 128 entries records active borrows. `asx_ghost_borrow_shared()` checks for conflicting exclusive borrows; `asx_ghost_borrow_exclusive()` checks for any existing borrow. Violations are recorded with the conflicting entity and borrow type.

### Error Taxonomy and Recovery Guidance

Every error code carries structured metadata beyond the numeric value:

- **Category** (17 families): general, transition, region, task, obligation, cancel, channel, timer, quiescence, resource, handle, hook, affinity, codec, replay, config, permission.
- **Recoverability**: `NONE` (fatal), `TRANSIENT` (retry may help), `PERMANENT` (logic error), `CONTEXT_DEPENDENT` (depends on usage).
- **Recovery action**: `RETRY_IMMEDIATELY`, `RETRY_WITH_BACKOFF`, `REINITIALIZE_CONTEXT`, `PROPAGATE`, `ESCALATE`, `INSPECT_CONFIGURATION`.
- **Backoff hints**: For transient errors, each code specifies initial delay, maximum delay, and maximum retry attempts.

The error ledger maintains a per-task ring buffer (16 entries deep, 64 task slots) that records recent errors with source location (`file:line`), operation name, and monotonic sequence number. This means when a task fails, you can inspect its recent error history without external logging infrastructure.

The `ASX_TRY(expr)` macro provides Rust-style `?` operator behavior in C: it evaluates `expr`, and if the result differs from `ASX_OK`, returns it immediately from the enclosing function.

### Trace, Replay, and Semantic Digests

Every scheduler round, lifecycle transition, and significant event is recorded in a bounded event journal (ring buffer, default 1,024 entries). Each event is a tuple of `(sequence, kind, entity_id, aux)` with a monotonic sequence counter that saturates at `UINT32_MAX`.

The **semantic digest** is computed using FNV-1a (64-bit) over all event tuples:

```
offset_basis = 0x517cc1b727220a95
prime        = 0x00000100000001B3
for each byte in (kind, entity_id, sequence):
    hash ^= byte
    hash *= prime
```

Two runs with the same scenario, profile, seed, and runtime version must produce identical digests. The conformance system compares digests across:
- JSON codec vs binary codec (codec equivalence),
- different deployment profiles (profile parity),
- C runtime vs Rust reference fixtures (Rust parity).

Any mismatch is a semantic drift bug, not an acceptable variation.

### Combinator Composition Model

All 11 async combinators share a common `asx_combinator_poll_fn` interface: each is a poll state machine that returns `ASX_E_PENDING` until terminal, then a final status. This uniformity means combinators compose naturally: a retry combinator can wrap a race combinator, which itself contains timeout-wrapped branches.

The **loser-drain protocol** is central to race/select/quorum semantics. When a winner is decided:
1. Each undecided branch receives exactly one final cooperative poll (cleanup opportunity).
2. If still pending after the cooperative poll, the branch is force-resolved to `ASX_E_CANCELLED`.
3. The combinator then returns the winner's result.

This bounded drain prevents indefinite hangs on losing branches while giving them a chance to release resources. The drain is deterministic: same input, same drain order, same result.

**Outcome aggregation** in join uses a severity lattice: `Ok < Err < Cancelled < Panicked`. The combined outcome of a join is the maximum severity across all branches. This means a join of (Ok, Ok, Err) produces Err, and a join of (Ok, Cancelled) produces Cancelled.

### Circuit Breaker State Machine

The circuit breaker implements the standard three-state pattern with explicit thresholds:

```
                  failure_count >= threshold
    CLOSED ──────────────────────────────────► OPEN
       ▲                                         │
       │  success_count >= threshold              │ manual half_open()
       │                                          ▼
       └──────────────────────────────────── HALF_OPEN
                                                  │
                                        any failure │
                                                  ▼
                                                OPEN
```

In CLOSED state, every successful call resets the consecutive failure counter. In HALF_OPEN state, traffic is limited to `half_open_max_calls` concurrent requests (default 1); additional attempts get `ASX_E_WOULD_BLOCK`. This prevents a thundering herd from overwhelming a recovering service. Once enough probe successes accumulate, the breaker closes and normal operation resumes.

### Epoch-Based Execution Phases

Epochs provide scoped execution phases with three advancement modes:

- **Manual**: The caller explicitly advances via `asx_epoch_advance()`. Useful for test orchestration and phased rollouts.
- **Barrier**: The epoch advances when all registered observers have called `asx_epoch_arrive()`. Models N-way synchronization points.
- **Threshold**: The epoch advances when N arrivals occur (configurable threshold). Models quorum-based progression.

Each epoch tracks a monotonic phase counter, an arrival count that resets on advance, and up to 8 observer callbacks notified synchronously on phase transitions. An optional `max_phases` limit prevents unbounded phase growth.

### Adaptive Decision Engine

The adaptive subsystem provides expected-loss minimization for runtime decisions (e.g., "should we shed load or accept backpressure?"). For each candidate action, it computes:

```
E[Loss(action)] = sum over states: Loss(action, state) * P(state | evidence)
```

Loss values use 16.16 fixed-point arithmetic (no floating point). Posterior probabilities are 0.32 fixed-point. The engine selects the action with minimum expected loss, with fallback to a safe default when confidence is below a configurable threshold or the decision budget is exhausted.

An evidence ledger (ring buffer, 64 entries) records each decision with its selected action, counterfactual alternatives, and contributing evidence terms, so you can reconstruct why the runtime chose a particular path after the fact.

### Profile Surface Gating

Each deployment profile declares which runtime surfaces are available. The browser profile, for example, gates out filesystem, process, signal, I/O driver, and blocking pool surfaces. A `main.wasm` compiled with `ASX_PROFILE_BROWSER` cannot accidentally call `asx_spawn_blocking()`; it returns `ASX_E_PERMISSION_DENIED`.

Surface gating is enforced at two levels:
1. **Compile-time**: Profile macros conditionally exclude code paths.
2. **Runtime**: `asx_surface_gate(surface)` checks the active profile's availability table and returns `ASX_E_PERMISSION_DENIED` for gated surfaces.

This dual gating ensures that even if headers are included, gated functions fail cleanly rather than linking against undefined behavior.

### Zero-Allocation Arena Design

All subsystems use fixed-size static arenas instead of dynamic allocation:

| Subsystem | Arena Size | Slot Type |
|---|---|---|
| Regions | 64 slots | Region lifecycle state |
| Tasks | 256 slots | Task state + poll function |
| Obligations | 256 slots | Reserve/commit/abort state |
| Timers | 128 slots | Deadline + waker + generation |
| Channels (MPSC) | 32 slots | Ring buffer + permits |
| Blocking pool | 16 slots | Function + result + waker |
| I/O registrations | 32 slots | fd + interest mask + waker |
| Deadline monitors | 32 slots | Target + entity + callback |
| Epochs | 16 slots | Policy + phase + observers |
| Ghost violations | 64 entries | Ring buffer |
| Error ledger | 16 entries x 64 tasks | Ring buffer per task |
| Trace events | 1,024 entries | Ring buffer |

Slots are reused via generation counters, never freed and reallocated. This design:
- eliminates malloc/free overhead and fragmentation,
- enables deterministic memory footprint (known at compile time),
- makes the runtime suitable for `ASX_PROFILE_FREESTANDING` where no allocator exists,
- allows the allocator to be sealed after initialization (`asx_runtime_seal_allocator()`).

Resource classes (R1/R2/R3) scale these capacities for different deployment targets without changing any semantic behavior.

## Stackless Coroutines via Protothread Macros

`asx` implements cooperative multitasking without threads, fibers, or `setjmp`/`longjmp`. Instead, tasks use protothread macros that compile to a switch-case state machine:

```c
#define ASX_CO_BEGIN(state)   switch ((state)->line) { case 0u:
#define ASX_CO_YIELD(state)   do { (state)->line = (uint32_t)__LINE__; \
                                   return ASX_E_PENDING; \
                              case __LINE__:; } while (0)
#define ASX_CO_END(state)     } (state)->line = 0u; return ASX_OK
```

On first call, `line` is 0, so execution starts at `case 0u`. When `ASX_CO_YIELD` is hit, the current `__LINE__` is saved and the function returns `ASX_E_PENDING`. On the next poll, the switch jumps directly to that line number via the case label. This gives you checkpoint-like resumption with zero dynamic allocation; all state lives in a caller-provided struct.

Example usage:

```c
typedef struct { uint32_t line; int progress; } my_task_state;

static asx_status my_task_poll(void *user_data, asx_task_id self) {
    my_task_state *s = (my_task_state *)user_data;
    (void)self;
    ASX_CO_BEGIN(s);

    s->progress = 0;
    while (s->progress < 10) {
        s->progress++;
        ASX_CO_YIELD(s);  /* return PENDING, resume here next poll */
    }

    ASX_CO_END(s);
}
```

The compiler sees a flat switch statement, with no hidden allocations, no platform-specific assembly, and no longjmp hazards. The same task code compiles and runs identically across all 9 deployment profiles, including bare-metal and WASM.

## Channel Families

`asx` provides five channel types for different communication patterns, all bounded and backpressure-aware:

### MPSC (Multi-Producer, Single-Consumer)

The primary message-passing channel. Uses the two-phase reserve/send protocol described in the internal design section. Bounded capacity (default 64), FIFO ordering, permit-token-based backpressure.

### Oneshot (Single-Value, Single-Use)

Exactly one send, exactly one receive. Optimized for request-reply and initialization signals:

```
EMPTY → (send) → FILLED → (receive) → CONSUMED
                          → SENDER_DROPPED (if sender drops before send)
                          → RECEIVER_DROPPED (if receiver drops before receive)
```

No ring buffer needed, just a single value slot with a 5-state lifecycle. Useful for task spawn result delivery and one-time configuration handoff.

### Broadcast (Multi-Consumer, Lag-Tolerant)

All receivers see all messages. Each receiver maintains its own cursor into a shared ring buffer. If a receiver falls behind the write position by more than capacity, it receives `ASX_E_LAGGED` and its cursor advances to the oldest available entry. Applications must handle lag explicitly; `asx` does not silently drop or replay.

### Watch (Single-Value Observable)

Sender replaces a single value; receivers always see the latest. Unlike broadcast, there is no message history, only the current version. Each receiver tracks `last_seen_version` and `asx_watch_has_changed()` returns whether the value has been updated since last read. Ideal for configuration propagation and health-status monitoring.

### Session (Bidirectional with Obligation Tracking)

Two endpoints (initiator and responder) with independent queues in each direction (`i2r` and `r2i`). Requests from the initiator increment an obligation counter; responses from the responder decrement it. This implicit backpressure prevents unbounded reply-less requests: if the obligation count grows too large, the session can reject new sends. Lifecycle: `OPEN → HALF_CLOSED → CLOSED`.

## Actor Supervision

The actor subsystem implements Erlang-style supervision trees with three restart strategies:

| Strategy | Behavior on Child Failure |
|---|---|
| **ONE_FOR_ONE** | Only the failed child restarts |
| **ONE_FOR_ALL** | All children stop and restart together |
| **REST_FOR_ONE** | Failed child and all younger siblings restart |

Each child has a restart policy:
- **PERMANENT**: Always restart on failure.
- **TRANSIENT**: Restart only on error exit (not on normal completion).
- **TEMPORARY**: Never restart. The child is gone.

The supervisor state machine progresses through INIT, RUNNING, STOPPING, RESTART, SHUTDOWN, and DONE phases. This structured lifecycle means supervision decisions are deterministic and auditable. The same failure sequence always produces the same restart pattern.

## Structured Concurrency and Capability Flow

The capability context (`asx_cx`) carries the authority a task needs to operate: region ID, task ID, capability bitmask, budget, clock source, and entropy state. Capabilities flow downward through explicit narrowing:

```c
asx_cx parent_cx;
asx_cx child_cx;

// Child gets a strict subset of parent capabilities
asx_cx_narrow(&child_cx, &parent_cx, ASX_CAP_SPAWN | ASX_CAP_TIMER);

// Or attenuate by removing specific capabilities
asx_cx_attenuate(&child_cx, &parent_cx, ~ASX_CAP_NETWORK);
```

Scopes bind a capability context to a region for spawning child tasks. The `ASX_CAP_SPAWN` capability is required to spawn; if a task's context lacks it, `asx_scope_spawn()` fails. This prevents ambient authority: no task can create children, open timers, or access the network unless its parent explicitly granted that capability.

All capability contexts are borrowed from static runtime tables, requiring no allocation. The design mirrors Rust's ownership-based authority model, adapted for C's manual lifetime management.

## Quiescence: When Is a Region Truly Done?

Region cleanup must verify four conditions before a region can reach the CLOSED state:

| Condition | Check | Error If Violated |
|---|---|---|
| **Q1** | All tasks completed (`task_count == 0`) | `ASX_E_TASKS_STILL_ACTIVE` |
| **Q2** | All child regions closed | `ASX_E_INCOMPLETE_CHILDREN` |
| **Q3** | No reserved obligations remain (`obligations_reserved == 0`) | `ASX_E_OBLIGATIONS_UNRESOLVED` |
| **Q4** | Cleanup stack fully drained | `ASX_E_QUIESCENCE_NOT_REACHED` |

A region is quiescent if and only if: `state == CLOSED && Q1 && Q2 && Q3 && Q4 && !poisoned`. If any condition fails, the runtime reports which specific condition was violated rather than returning a generic "not quiescent" error, so you know exactly what's still alive.

Poisoned regions are never quiescent. If a region is poisoned (invariant violation detected), it transitions to a terminal error state and downstream consumers are notified.

## Virtual Time and Anomaly Injection

For deterministic testing of timing-sensitive code, `asx` provides a virtual time source with three anomaly types:

| Anomaly | Effect | Use Case |
|---|---|---|
| **Jitter** | Add/subtract nanosecond delta at trigger point | Test deadline sensitivity to clock noise |
| **Stall** | Freeze time for N queries (time stops advancing) | Test stall detection and watchdog recovery |
| **Jump** | Instantly advance by N nanoseconds | Test timeout handling and timer wheel edge cases |

Virtual time is installed via the clock hook system:

```c
asx_runtime_hooks hooks;
asx_runtime_hooks_init(&hooks);
hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
hooks.clock.ctx = &vtime_state;
```

Combined with fault injection (`ASX_FAULT_CLOCK_SKEW`, `ASX_FAULT_CLOCK_REVERSE`, `ASX_FAULT_ENTROPY_CONST`, `ASX_FAULT_ALLOC_FAIL`), this lets you reproduce production timing anomalies in a deterministic test environment. Every injected fault has a trigger-after count and a duration, so you can say "after the 50th clock read, add 100ms of skew for the next 10 reads."

## Hot Config Reload

The runtime supports hot configuration changes without restart for operational parameters:

**Reloadable at runtime** (no restart needed):
- `wait_policy` — busy-spin, yield, or sleep (resource-plane only)
- `leak_response` — panic, log, silent, or recover on detected leaks
- `finalizer_poll_budget` — cleanup poll cap
- `finalizer_time_budget_ns` — cleanup time cap
- `finalizer_escalation` — soft, bounded-log, or bounded-panic

**Frozen** (require restart):
- `max_cancel_chain_depth` — existing in-flight chains may exceed new limits
- `max_cancel_chain_memory` — same reason

The reload mechanism validates the new configuration against the current state before applying. If validation fails, the old config remains in effect. In the single-threaded walking skeleton, this is an atomic swap; no partial config states are observable.

## Telemetry Tiers

Event collection operates at three levels, selectable at initialization:

| Tier | Events Retained | Digest Computed | Overhead |
|---|---|---|---|
| **FORENSIC** | All events in ring buffer | Yes | Full |
| **OPS_LIGHT** | Lifecycle + terminal events only | Yes | Moderate |
| **ULTRA_MIN** | None (digest only) | Yes | Minimal |

The FNV-1a rolling digest is updated regardless of tier. Even `ULTRA_MIN` mode produces a canonical semantic digest comparable against `FORENSIC` mode, so you can verify that a production deployment (running `ULTRA_MIN` for low overhead) produces the same semantic behavior as a lab run (using `FORENSIC` for full visibility).

## Regression Localization

When a trace digest changes between versions, `asx_regression_localize()` pinpoints which subsystem diverged. It partitions events by subsystem (scheduler, lifecycle, obligation, channel, timer), counts events and sums auxiliary data per partition, then compares baseline vs. current:

```
subsystem     baseline_events  current_events  delta
scheduler     1,247            1,249           +2
lifecycle     312              312             0
obligation    89               89              0
channel       456              458             +2
timer         203              203             0

worst_regressed: scheduler (+2 events)
```

This narrows a digest mismatch from "something changed somewhere" to "the scheduler emitted 2 extra events," giving you a concrete starting point for investigation.

## Hindsight Logging

The hindsight ring buffer (256 entries) records every nondeterministic boundary event: external I/O results, timer fires, entropy draws, and reactor readiness notifications. Each entry carries the trace sequence number at which it occurred.

When a deterministic replay diverges from the expected path, the hindsight log shows exactly where nondeterminism was injected. Rather than guessing "was it a timer race? a network callback? an entropy value?", you can inspect the hindsight log and see: "at trace sequence 847, entropy draw returned 0x1f3a instead of the expected 0x7c2b."

The log has its own FNV-1a digest for integrity verification during replay.

## Per-Profile Overload Policies

Each deployment profile ships with tuned overload handling:

| Profile | Strategy | Threshold | Safety Constraints |
|---|---|---|---|
| CORE, POSIX, WIN32 | Reject | 90% capacity | No silent drops |
| FREESTANDING | Reject | 80% | No silent drops, no nondeterminism |
| EMBEDDED_ROUTER | Reject | 75% | No silent drops, no unbounded queues |
| HFT | Shed oldest | 85% | No latency spikes |
| AUTOMOTIVE | Backpressure | 90% | No silent drops, no nondeterminism, no latency spikes |
| PARALLEL | Reject | 90% | No silent drops |
| BROWSER | Reject | 85% | No silent drops, no unbounded queues |

Each profile also declares forbidden behaviors via bitmask flags (`ASX_FORBID_SILENT_DROP`, `ASX_FORBID_NONDETERMINISTIC`, `ASX_FORBID_UNBOUNDED_QUEUE`, `ASX_FORBID_LATENCY_SPIKE`) that prevent misconfiguration.

## ABI Stability Contract

`asx` uses a three-part versioning scheme for binary compatibility:

- **MAJOR**: Incremented on ABI-breaking changes (enum reorder, handle layout change, hook signature change). Requires recompilation.
- **MINOR**: Backward-compatible additions (new error codes, new profiles, new functions). No recompilation needed.
- **PATCH**: Bug fixes with no API or ABI change.

Five surfaces are ABI-critical (changes require a MAJOR bump):
1. Handle layout: the 64-bit `[type_tag:16][state_mask:16][gen:16][slot:16]` format.
2. Enumeration values: numeric assignments for all lifecycle state enums are frozen.
3. Status code families: error code numbers are permanent. New codes are appended.
4. `asx_runtime_hooks` struct: field order and function signatures are frozen.
5. Binary wire formats: trace and codec formats are versioned independently.

Config structs use a size-field pattern for forward compatibility: `cfg.size = sizeof(cfg)`. If a newer library adds fields to the end of the struct, an older caller's smaller size tells the library to use defaults for the new fields.

## Plan DAG: Declarative Concurrency Patterns

The plan module lets you express structured concurrency as a directed acyclic graph before execution:

```c
asx_plan_dag_id leaf_a = asx_plan_dag_leaf(&dag, "fetch-config");
asx_plan_dag_id leaf_b = asx_plan_dag_leaf(&dag, "fetch-secrets");
asx_plan_dag_id leaf_c = asx_plan_dag_leaf(&dag, "health-check");

// All three must complete
asx_plan_dag_id join = asx_plan_dag_join(&dag, children, 3);

// ... but give up after 5 seconds
asx_plan_dag_id root = asx_plan_dag_timeout(&dag, join, 5000000);

asx_plan_dag_set_root(&dag, root);
asx_plan_dag_validate(&dag);  // checks acyclicity, bounds, references
```

DAG node types: **leaf** (unit of work), **join** (all children), **race** (first child), **timeout** (child with deadline). Forward references are prohibited (child IDs must be less than parent ID), which guarantees acyclicity by construction. The DAG is checksum-verifiable and supports algebraic rewriting for optimization.

## Cleanup Stacks: Deterministic RAII in C

C has no destructors, so `asx` provides deterministic cleanup stacks, the closest analog to Rust's `Drop` or C++ RAII available in plain C99. Each region owns a fixed-size cleanup stack (32 entries). Handlers are pushed in registration order and executed in strict LIFO (reverse) order during region drain:

```c
asx_cleanup_push(&region_cleanup, my_release_fn, my_resource);
asx_cleanup_push(&region_cleanup, another_release_fn, another_resource);
// ... on drain:
//   another_release_fn(another_resource)  ← called first (LIFO)
//   my_release_fn(my_resource)            ← called second
```

Each entry carries a generation counter so stale cleanup handles cannot accidentally fire against a recycled slot. When a region reaches the FINALIZING state, the cleanup stack is drained completely before transitioning to CLOSED. Quiescence condition Q4 verifies `asx_cleanup_pending() == 0`; the region cannot close until every registered handler has run.

This eliminates the "forgot to free" class of bugs common in C event loops: if you register a cleanup, it *will* run, in a deterministic order, regardless of how the region terminates.

## Outcome Severity Lattice

The outcome join operation is the algebraic heart of result aggregation. When multiple branches complete (as in a join combinator or region drain), their outcomes are merged using a **max-severity join**:

```
Ok(0) < Err(1) < Cancelled(2) < Panicked(3)
```

The join rule: `join(a, b) = max_severity(a, b)`, with left-bias on equal severity. This means:
- `join(Ok, Ok) = Ok` — everything succeeded.
- `join(Ok, Err) = Err` — the error propagates.
- `join(Err, Cancelled) = Cancelled` — cancellation is more severe.
- `join(anything, Panicked) = Panicked` — panic always wins.

This is a bounded semilattice with `Ok` as the bottom element and `Panicked` as the top. It composes associatively: `join(join(a, b), c) = join(a, join(b, c))`, so you can fold over any number of branch outcomes in any grouping order and get the same result.

## Affinity Domains: Send/Sync for C

In Rust, `Send` and `Sync` prevent data races at compile time. `asx` provides the same safety via runtime affinity domains (compile-time gated behind `ASX_DEBUG_AFFINITY` for zero overhead in release):

An affinity table (256 entries) maps entity IDs to domain bindings. Three special domains: `ANY` (unrestricted), `NONE` (sentinel), and numeric domain IDs (representing threads or execution contexts). Operations:

- **Bind**: Associate an entity with a domain. Fails if already bound elsewhere.
- **Check**: Verify the current execution context matches the entity's domain. Raises `ASX_E_AFFINITY_VIOLATION` on mismatch.
- **Transfer**: Move an entity from one domain to another. Requires ownership of the source domain (`ASX_E_AFFINITY_TRANSFER_REQUIRED` otherwise).

This catches the same class of bugs that Rust's ownership system catches, but at runtime in debug builds rather than at compile time. When migrating to Phase 4 (multi-threaded dispatch), affinity violations will surface threading bugs before they become data races.

## State Transition Tables: O(1) Lifecycle Validation

Every lifecycle transition in `asx` is validated against precomputed boolean tables:

```c
// Encoded as: allowed[from_state][to_state]
static const int region_allowed[5][5] = { ... };  // 5 states, 13 valid transitions
static const int task_allowed[6][6]   = { ... };  // 6 states, 13 valid transitions
static const int obligation_allowed[4][4] = { ... }; // 4 states, 3 valid transitions
```

A transition check is a single array lookup: `allowed[from][to]`. No switch statements, no iteration, no hash maps. This O(1) validation runs on every state change: in debug builds via ghost monitors, and in production as a direct return-code check.

Helper functions classify states: `asx_task_state_is_terminal()`, `asx_region_state_is_closing()`, `asx_region_can_spawn()`. These are also table lookups. The transition tables are the authoritative specification of lifecycle legality. If the table says a transition is forbidden, no code path may perform it.

## Waker System: Bridging I/O to Scheduling

Wakers are the glue between asynchronous I/O events and the task scheduler. When a task needs to wait for something (timer fire, channel message, I/O readiness), it registers a waker. When the event occurs, the waker is signaled, which marks the associated task as ready for the next scheduler poll.

The waker arena holds 64 slots, each binding a task ID to a signaled/alive state with generation stamping. Key properties:

- **Wakers are cloneable**: Multiple I/O sources can hold references to the same waker slot.
- **Wakers are generation-safe**: Stale wakers (from a previous task that used the same slot) silently fail to signal rather than waking the wrong task.
- **Drain collects all signaled tasks**: `asx_waker_drain_signaled()` atomically collects all signaled task IDs and clears their flags, ready for the scheduler to poll them.

This decouples I/O sources from scheduling policy. The I/O driver, timer wheel, and channel subsystems all signal wakers identically, and the scheduler handles them uniformly.

## Synchronization Primitives

All sync primitives are cooperative (no OS-level blocking), async-friendly (three-phase begin/poll/cancel), and cancel-safe (cancellation removes the waiter without corrupting shared state).

**Mutex**: Cooperative mutual exclusion implemented as a semaphore with count 1. `try_lock()` returns immediately; `lock_begin()`/`poll_lock()` yield until available. No priority inversion guards (single-threaded walking skeleton).

**Semaphore**: Counting permit system with configurable initial count. `try_acquire()` is non-blocking; async acquire uses FIFO waiter queue (16 slots). Permits are distributed to the longest-waiting waiter first. Integrates with the obligation system for permit tracking.

**Barrier**: N-way rendezvous with leader election. All N tasks must arrive before any proceed. The last task to arrive is elected leader (`is_leader = 1`). Cancel-safe: cancelling a waiter decrements the arrival count without tripping the barrier. Integrated with Cx checkpoint for structured concurrency cancellation.

**Once**: Compute-once cell storing a 64-bit value. `get_or_init(init_fn)` runs the initializer exactly once; subsequent calls return the cached value. Deterministic initialization order (first caller wins).

**Notify**: Async event signaling supporting `notify_one()` (FIFO single-waiter wake) and `notify_all()` (broadcast). Up to 16 concurrent waiters. Close wakes all waiters with `ASX_E_DISCONNECTED`.

## Evidence Collection and Verdict Derivation

The evidence system provides structured diagnostic output for lab, oracle, and doctor tools. Evidence entries carry:

- **Source**: Emitter name (e.g., `"oracle:leak"`, `"doctor:utilization"`)
- **Level**: `INFO`, `PASS`, `WARN`, or `FAIL`
- **Message**: Human-readable description
- **Entity ID**: Related entity (task, region, etc.)
- **Sequence**: Monotonic within each sink

An evidence sink accumulates entries and derives a verdict using simple max-severity logic:
- Any `FAIL` entry → verdict is **FAIL**
- No fails but any `WARN` → verdict is **WARN**
- Otherwise → **PASS**

Evidence can be rendered as NDJSON for machine consumption or human-readable text. The `asx_inspect_to_evidence()` pipeline captures a full runtime inspection and automatically records one evidence entry per subsystem with appropriate severity based on utilization thresholds.

## Runtime Monitor: Continuous Health Checks

The monitor module provides threshold-based health evaluation that can run on every scheduler tick or at configured intervals:

| Metric | Default Threshold | Error If Exceeded |
|---|---|---|
| Region utilization | 75% | WARN |
| Task utilization | 75% | WARN |
| Obligation utilization | 75% | WARN |
| I/O registration utilization | 75% | WARN |
| Blocking pool utilization | 75% | WARN |
| Ghost violations | 0 | FAIL |
| Deadline miss rate | 0% | FAIL |
| Watchdog violations | 0 | FAIL |

The monitor queries the runtime inspection report, computes utilization percentages, and sets a triggered bitmask for each threshold exceeded. Results are recorded as evidence entries, feeding into the verdict system for automated health reporting.

## Doctor: Runtime Health Diagnosis

The `asx doctor` command (and programmatic `asx_doctor_run()`) performs a comprehensive health check:

1. Is the runtime initialized?
2. Region/task/obligation/IO/blocking utilization — OK if < 75%, WARN if >= 75%, FAIL if at capacity.
3. Ghost monitor violation count.
4. Deadline miss rate.
5. Watchdog violation count.
6. Native host surface availability for current profile.

The report is available as structured text or JSON, suitable for CLI output, log ingestion, or CI gate evaluation. Each check produces a named finding with severity, message, current value, and capacity.

## In-Memory Network Transport

The network subsystem provides a deterministic in-memory transport that mirrors real socket semantics without touching the OS network stack. This enables network-aware scenarios to run deterministically in lab mode:

**TCP**: Listener with accept queue (16 deep), stream handles with send/recv buffers (1,460 bytes, matching typical MTU payload). `listen()` → `accept()` → `connect()` → `send()`/`recv()` → `close()`.

**UDP**: Socket with 4-deep datagram queue (576 bytes per datagram, matching minimum IP reassembly buffer). `bind()` → `sendto()`/`recvfrom()` → `close()`.

**Address handling**: IPv4 dotted-quad parser, IPv4/IPv6 loopback constructors, address equality. Addresses outside the in-memory model produce ghost-pending behavior: the call returns `ASX_E_PENDING` indefinitely, observable by the ghost monitor as a "network boundary event."

**Happy eyeballs**: The resolver simulates dual-stack address discovery. A TCP connect attempt against a resolved name tries addresses in parallel (race combinator pattern), preferring the first successful connection.

## Buffer and Byte Slice Primitives

The bytes subsystem provides zero-allocation buffer management:

**`asx_buf`** (immutable slice): Pointer + length over `const uint8_t*`. Constructors from raw pointer, C string, or empty. Supports slicing with bounds validation and equality comparison.

**`asx_buf_mut`** (mutable buffer): Fixed-capacity buffer with read/write position cursors. Supports sequential write (append), sequential read (consume), clear (reset cursors), and remaining-capacity queries. All operations are bounds-checked; writes beyond capacity return `ASX_E_BUFFER_TOO_SMALL`.

Both types are stack-allocable and designed for cursor-based I/O patterns: write into a `buf_mut`, then hand off an immutable `buf` slice for consumption.

## Stream Combinators: Async Iterators

The stream subsystem provides poll-based async iterators with zero dynamic allocation:

```c
// Create a stream that maps items through a transform function
asx_stream_map map_state;
asx_stream_map_init(&map_state, source_poll, source_state, my_transform, xform_ctx);

// Poll the stream
void *item;
asx_stream_status st = asx_stream_map_poll(&map_state, &waker, &item);
// st = ASX_STREAM_READY (item available) | ASX_STREAM_PENDING | ASX_STREAM_DONE
```

Available combinators:
- **Iter source**: Iterate over a fixed array.
- **Map**: Transform each item via function pointer.
- **Filter**: Pass items that satisfy a predicate.
- **Channel/Watch/Broadcast adapters**: Bridge channel receives into stream interface.

Streams are waker-aware: when a stream returns `ASX_STREAM_PENDING`, the waker is registered so the producing source can signal readiness when new data arrives.

## Security Audit: Proving No Ambient Authority

The security subsystem enforces that the C port does not introduce ambient authority, meaning hidden global state or implicit capabilities that bypass the explicit `asx_cx` capability flow.

An **audit runner** scans code modules against a catalog of known exemptions:
- **Exempted**: Time driver and entropy provider (these are capability boundaries by design).
- **Pristine**: Core modules (`channel.c`, `obligation.c`, `region.c`, `task.c`, `security.c`, `stream.c`) must use zero ambient authority.
- **Violation ceiling**: 0 non-exempted ambient accesses allowed.

The audit produces evidence entries and a pass/fail verdict. If a code change introduces an ambient access in a pristine module, the audit fails. Think of it as Rust's `#![forbid(unsafe_code)]` adapted for C: the code can be written, but it cannot pass the audit gate.

## Lab Runtime: Deterministic Test Execution

The lab runtime wires together virtual time, seeded PRNG, and scenario execution into a single convenient testing harness:

```c
asx_lab_config lab_cfg = { .seed = 42, .tick_ns = 1000000, .start_time_ns = 0, .max_polls_per_step = 100 };
asx_lab lab;
asx_lab_init(&lab, &lab_cfg);

// Execute a scenario as a sequence of steps
asx_lab_scenario scenario = { .steps = my_step_fns, .step_count = 5 };
asx_lab_result result;
asx_lab_run(&lab, &scenario, &result);

// result.steps_completed, result.elapsed_ns, result.total_polls
asx_lab_shutdown(&lab);
```

The lab provides:
- **Seeded entropy**: `asx_lab_random_u64()` uses a deterministic PRNG seeded at init.
- **Virtual time**: Time advances in configurable tick increments, controllable by the test.
- **Bounded execution**: `max_polls_per_step` prevents infinite loops in buggy task implementations.
- **Region convenience**: `asx_lab_open_region()` wires the lab's Cx into the opened region.

This is the primary mechanism for writing tests that are 100% reproducible across platforms: no real clocks, no real entropy, no real I/O.

## Vertical Adapters: Profile-Specific Fast Paths

For performance-critical profiles (HFT, Automotive, Embedded Router), vertical adapters provide domain-specific accelerated code paths that skip generic dispatch layers:

| Adapter | Profile | Optimization |
|---|---|---|
| HFT | `ASX_PROFILE_HFT` | Latency-optimized scheduling, busy-spin wait, jitter-minimized paths |
| Automotive | `ASX_PROFILE_AUTOMOTIVE` | Deadline-first scheduling, watchdog-integrated paths, compliance checkpoints |
| Router | `ASX_PROFILE_EMBEDDED_ROUTER` | Memory-minimized paths, wear-safe tracing, buffer-bloat-aware admission |

Each adapter includes an **isomorphism proof function** that verifies the accelerated path produces identical decisions to the generic path. This means you can enable vertical adapters for production throughput while retaining confidence that semantics haven't drifted.

## Examples, Vignettes, and End-to-End Scenarios

The test and example suites serve as executable documentation:

### Examples (9 programs in `examples/`)

| Example | Demonstrates |
|---|---|
| `ex_quickstart.c` | Kernel lifecycle: region open, task spawn, scheduler run, quiescence, drain |
| `ex_actor_supervision.c` | Actor mailbox, cast+call semantics, graceful stop, one-for-one restart |
| `ex_channel_flow.c` | MPSC bounded channel, two-phase send (reserve-send), backpressure handling |
| `ex_cancel_drain.c` | Cancellation protocol and region drain semantics |
| `ex_timeout_deadline.c` | Deadline arming, timeout handling, expiry detection |
| `ex_lab_replay.c` | Deterministic lab runtime, virtual time, scenario execution and replay |
| `ex_browser_boundary.c` | Browser profile surfaces, fail-closed gating, browser-safe operations |
| `ex_browser_replay.c` | Browser profile combined with replay verification |
| `ex_network_surface.c` | Resolver, dual-stack discovery, happy-eyeballs, TCP/UDP ghost contracts |

### API Vignettes (11 walkthroughs in `tests/vignettes/`)

Vignettes are self-contained programs that demonstrate one API pattern each with detailed comments. They serve as both documentation and regression tests:

| Vignette | Pattern |
|---|---|
| `vignette_lifecycle.c` | Region/task/scheduler/quiescence basics |
| `vignette_hooks.c` | Freestanding custom hook installation |
| `vignette_obligations.c` | Obligation reserve/commit/abort flow |
| `vignette_replay.c` | Event capture and deterministic replay |
| `vignette_network.c` | Network resolver and socket operations |
| `vignette_budgets.c` | Task budget algebra and exhaustion |
| `vignette_link.c` | Long-lived request/response coordination |
| `vignette_observability.c` | Snapshot capture and inspection |
| `vignette_security.c` | Ambient authority audit |
| `vignette_console.c` | Doctor and inspection rendering |
| `vignette_plan.c` | Plan DAG and rewrite certificates |

### End-to-End Scenarios (17 programs in `tests/e2e/`)

E2E tests exercise full runtime paths under realistic conditions:

| Scenario | What It Tests |
|---|---|
| `e2e_core_lifecycle.c` | Complete lifecycle from init through quiescence |
| `e2e_foundational_contracts.c` | Async contract validation across subsystems |
| `e2e_native_host.c` | OS integration: filesystem, process, signals |
| `e2e_network_surface.c` | TCP/UDP socket operations end-to-end |
| `e2e_hft_microburst.c` | High-frequency burst scheduling under tail-latency constraints |
| `e2e_market_open_burst.c` | Market-opening spike workload simulation |
| `e2e_codec_parity.c` | JSON vs binary codec semantic equivalence |
| `e2e_automotive_watchdog.c` | Deadline miss detection and watchdog recovery |
| `e2e_automotive_fault_burst.c` | Fault burst tolerance under automotive profile |
| `e2e_continuity.c` | Session persistence across runtime cycles |
| `e2e_continuity_restart.c` | Restart recovery with state preservation |
| `e2e_browser_smoke.c` | Browser profile surface gating and basic operations |

## Build System and Compiler Policy

The Makefile enforces strict compilation standards across all targets:

**Compiler flags** (warnings-as-errors, always on):
```
-std=c99 -Wall -Wextra -Wpedantic -Werror
-Wconversion -Wsign-conversion -Wshadow
-Wstrict-prototypes -Wmissing-prototypes
-Wswitch-enum -Wformat=2
```

Every warning is an error. No exceptions, no suppression flags. Sign conversions, implicit truncations, missing prototypes, and unhandled enum values are all caught at compile time. Debug builds add `-O0 -g -DASX_DEBUG=1`; release builds use `-O2 -DNDEBUG`.

**Eight lint gates** run before any merge:

| Gate | What It Catches |
|---|---|
| `format-check` | clang-format violations (deterministic formatting) |
| `lint` | cppcheck static analysis across src/ |
| `lint-checkpoint` | Kernel loops missing `asx_checkpoint()` calls |
| `lint-anti-butchering` | Proof blocks modified without updating evidence |
| `lint-evidence` | Beads missing evidence linkage |
| `lint-semantic-delta` | Semantic behavior changes exceeding the delta budget (default: 0) |
| `lint-schema-validation` | JSON fixture files violating schema contracts |
| `lint-static-analysis` | Additional static analysis passes |

**Size and performance gates**:
- `make size-gate`: Measures binary size and cold-start time against baselines stored in `tools/ci/size_cold_start_baselines.json`. Regressions fail the gate.
- `make slo-gate`: Evaluates performance SLOs against `tools/ci/slo_baselines.json`. Latency or throughput regressions fail the gate.

**Local vs CI**:
- `make check`: Local development gate (format + lint + build + test + formal).
- `make check-ci`: Full CI gate (adds e2e, conformance, codec-equivalence, profile-parity, fuzz-smoke, embedded-matrix, bare-metal cross-builds).

## Formal Verification

Beyond conventional testing, `asx` includes bounded model checking and algebraic proofs:

**CBMC harnesses** (`tests/formal/cbmc/`) use `__CPROVER_assert` and `__CPROVER_nondet_*` to verify:
- Task transition legality: all 36 cells of the 6x6 state matrix are checked. Out-of-range states are rejected. Terminal states have no outgoing transitions. Cancel phase progression is monotonic.
- Region transition legality: close/drain constraints verified for all state combinations.
- Obligation linearity: reserve/commit/abort exactly-once contracts. Double-resolution and leak are proven impossible in valid code paths.

**Algebraic property tests** (`tests/formal/algebraic/`) prove:
- Budget lattice laws: identity (`meet(infinite, x) = x`), absorption (`meet(exhausted, x) = exhausted`), commutativity, associativity, idempotence, and the narrowing property (meet can only tighten, never loosen).
- Outcome lattice: join operator associativity and commutativity.
- Cancel monotonicity: severity, rank, and phase can only increase, never decrease.

**Litmus tests** (`tests/formal/litmus/`) verify memory model properties for future multi-threaded dispatch.

Run all formal verification: `make formal-check` (includes `formal-cbmc`, `formal-algebraic`, `formal-tv`, `formal-litmus`, `formal-codegen`).

## Differential Fuzzing

The fuzz harness (`tests/fuzz/fuzz_differential.c`) generates random operation sequences and verifies three properties on every iteration:

1. **Self-consistency**: Running the same scenario with the same seed produces identical semantic digests.
2. **Crash freedom**: Mutated scenarios (randomized operation reordering, argument perturbation) must not crash or trigger undefined behavior.
3. **Rust parity**: When Rust reference fixtures are available, the C runtime's semantic digest must match.

The fuzzer covers cancellation, timers, channels, obligations, budget exhaustion, region lifecycle, and quiescence. A companion minimizer (`fuzz_minimize.c`) performs delta-minimization on failing cases to produce minimal reproducing scenarios.

```bash
make fuzz-smoke          # 100 iterations (CI gate)
make fuzz-nightly        # 100,000 iterations (nightly)
```

## Conformance Fixture Format

Rust reference fixtures in `fixtures/rust_reference/` use a structured JSON schema:

```json
{
  "scenario_id": "core-cancel-severity-003",
  "codec": "json",
  "profile": "ASX_PROFILE_CORE",
  "seed": 42,
  "input": { "ops": [{"id": 1, "op": "region_open"}, ...] },
  "expected_events": [
    {"seq": 0, "kind": "RegionOpened", "entity_id": 1, "aux": 0}
  ],
  "expected_final_snapshot": { "regions": 0, "tasks": 0, "obligations": 0 },
  "expected_error_codes": [],
  "semantic_digest": "sha256:abcdef...",
  "provenance": {
    "rust_baseline_commit": "a1b2c3d",
    "cargo_lock_sha256": "sha256:...",
    "capture_run_id": "uuid"
  }
}
```

Every fixture is captured from the Rust reference runtime with full provenance (commit hash, toolchain version, Cargo.lock hash, run ID). The conformance runner replays each fixture through the C runtime and compares semantic digests. Any mismatch is a parity bug in the C port, not an acceptable variation.

Fixtures are organized by semantic unit (core_budget, core_cancel, core_channel, core_lifecycle, core_timer) and encoded in both JSON and BIN formats with profile variants.

## Portability Layer

`include/asx/portable.h` provides alignment-safe, endian-aware load/store functions for cross-platform binary I/O:

```c
// Alignment-safe little-endian load (works on any address)
uint32_t val = asx_load_le_u32(wire_ptr);

// Big-endian store for network byte order
asx_store_be_u16(out_ptr, port_number);
```

Endianness is detected at compile time via byte-order canaries (`0x04030201` for little-endian, `0x01020304` for big-endian). All functions use byte-by-byte access, with no pointer casts, no platform intrinsics, and no undefined behavior from misaligned reads. The binary codec runs identically on ARM, MIPS, RISC-V, and x86 without `#ifdef` forests.

Wire format conventions: binary fixture codec uses big-endian (network byte order); binary trace persistence uses little-endian.

## WASM ABI Contract

The ABI module (`include/asx/abi/wasm_abi.h`) defines the contract between `asx` and external host environments (primarily WebAssembly):

**Feature flags** (8-bit bitmask): Each bit indicates a subsystem available in the current build: deterministic mode, channels, timers, obligations, symbols, JSON codec, BIN codec, ghost monitors. The host can query `asx_abi_feature_flags()` to discover available capabilities before calling any API.

**Compatibility classification**: `COMPATIBLE` (full feature parity), `DEGRADED` (partial; some operations return `ASX_E_NOT_SUPPORTED`), or `INCOMPATIBLE` (major version mismatch, refuse to initialize).

**Bootstrap state machine** (9 phases): `UNINITIALIZED → ABI_CHECK → HOOKS_BIND → PROVIDER_INIT → RUNTIME_INIT → READY → SHUTDOWN → TERMINATED`. Each phase transition is validated; skipping phases or going backward returns `ASX_E_INVALID_TRANSITION`. This prevents the common WASM integration bug of calling runtime functions before hooks are bound.

**Boundary types**: `asx_abi_bytes` (opaque byte span), `asx_abi_request` (method + payload + timeout), `asx_abi_response` (status + payload + recoverability). These are the wire types that cross the WASM/host boundary.

## Lane-Based Parallel Scheduler

The parallel subsystem (`include/asx/runtime/parallel.h`) declares a lane-based scheduler for multi-core dispatch (walking skeleton: single-worker mode that produces identical event streams to the sequential scheduler):

**Three lane classes**:
- **READY**: Tasks with pending work (normal polling).
- **CANCEL**: Tasks in CancelRequested or Cancelling phase (priority drain).
- **TIMED**: Tasks blocked on timer deadlines.

**Fairness policies**:
- `ROUND_ROBIN`: Equal poll share per lane per round.
- `WEIGHTED`: Proportional distribution per configurable lane weights.
- `PRIORITY`: Cancel lane drains first, then Ready, then Timed.

Cancel-streak limiting (default: 16) prevents starvation. If the cancel lane runs 16 consecutive polls without yielding, the scheduler forces a fairness yield to other lanes. Per-lane starvation detection reports when any lane goes unpolled for too many rounds.

The injector API (`asx_inject_ready`, `asx_inject_cancel`, `asx_inject_timed`) allows cross-boundary task submission into specific lanes, enabling I/O completions and timer fires to route tasks directly to the appropriate scheduling class.

## Browser Developer Experience Diagnostics

When a developer accidentally calls a native-only API from the browser profile, the browser diagnostic module goes beyond a bare `ASX_E_PERMISSION_DENIED`. It maps each blocked surface to a specific DX (developer experience) error class with actionable guidance:

| Blocked Surface | DX Class | Suggestion |
|---|---|---|
| Filesystem | `NATIVE_FS` | Use IndexedDB or channel-based I/O |
| Process spawn | `NATIVE_PROCESS` | Use Web Workers |
| OS signals | `NATIVE_SIGNAL` | Use the cancellation protocol |
| Native I/O (epoll/kqueue) | `NATIVE_IO` | Use timer wheel + channels |
| Blocking thread pool | `NATIVE_BLOCKING` | Use cooperative scheduling |

The diagnostic also checks allocator sealability and reports a surface availability matrix showing how many surfaces are available vs blocked in the current profile configuration.

## Tracing Integration Bridge

The tracing compatibility layer (`src/tracing_compat/`) bridges `asx` event traces to external observability systems (OpenTelemetry, journald, custom collectors):

```c
// Export all trace events as JSONL to a custom sink
void my_sink(const char *json_line, void *ctx) { /* write to file, network, etc. */ }

uint32_t exported;
asx_tracing_compat_export_current(my_sink, my_ctx, &exported);
```

Each event is formatted as a single-line JSON object with `sequence`, `kind`, `entity` (hex64), and `aux` (hex64) fields. The sink function receives complete, self-contained JSON strings that can be appended to any JSONL-consuming pipeline.

## Structured Test Logging

The test harness emits machine-readable JSONL records alongside human-readable stderr output. When `ASX_TEST_LOG_DIR` is set (defaults to `build/test-logs/`), each test run produces a JSONL file:

```json
{"ts":"2026-03-18T06:30:00Z","run_id":"unit-20260318T063000Z","layer":"unit","suite":"core_cancel","test":"severity_monotone","status":"pass","event_index":7}
{"ts":"2026-03-18T06:30:00Z","run_id":"unit-20260318T063000Z","layer":"unit","suite":"core_cancel","test":"strengthen_equal","status":"fail","event_index":8,"error":{"file":"test_cancel.c","line":42,"assertion":"sev_a >= sev_b"}}
```

Summary records include aggregate metrics (count, passed, failed). This structured output feeds CI dashboards, trend analysis, and the evidence-dashboard pipeline. Test log files accumulate across runs (append mode) so historical pass/fail rates are preserved.

## Documentation Suite

The `docs/` directory contains 60+ files covering architecture, decisions, verification, and risk:

| Category | Key Documents |
|---|---|
| **Semantic baseline** | `EXISTING_ASUPERSYNC_STRUCTURE.md` — authoritative lifecycle tables, cancellation protocol, budget algebra, channel semantics, timer ordering |
| **Parity tracking** | `FEATURE_PARITY.md` — 23 semantic units with status (all `impl-complete`) |
| **Architecture** | `PROPOSED_ANSI_C_ARCHITECTURE.md` — layering boundaries derived from semantics |
| **Decisions** | `OPEN_DECISIONS_ADR.md`, `OWNER_DECISION_LOG.md` — architecture decision records |
| **Safety** | `FORMAL_ASSURANCE_LADDER.md`, `GUARANTEE_SUBSTITUTION_MATRIX.md` — Rust-to-C safety mechanism mapping |
| **Profiles** | `HFT_PROFILE.md`, `AUTOMOTIVE_PROFILE.md`, `EMBEDDED_TARGET_PROFILES.md` — per-profile constraints |
| **Quality** | `QUALITY_GATES.md`, `TEST_COMPLETENESS_MATRIX.md` — gate definitions and coverage |
| **Risk** | `RISK_REGISTER.md`, `SEMANTIC_GAP_RISK_RANKING.md` — risk catalog and mitigation |
| **Deployment** | `DEPLOYMENT_HARDENING.md`, `OPENWRT_PACKAGING.md` — operational checklists |
| **Traceability** | `PLAN_EXECUTION_TRACEABILITY_INDEX.md` — machine-readable requirement-to-test mapping |

## CI Pipeline Architecture

The CI pipeline runs 7 parallel lanes on every push and PR:

```
┌────────────┐┌────────────────┐┌────────────────┐┌────────────────┐
│   check    ││ unit-invariant ││  conformance   ││ profile-parity │
│  format    ││  116 unit      ││  Rust fixture  ││  cross-profile │
│  8 lints   ││  3 invariant   ││  parity        ││  digest match  │
│  build     ││                ││                ││                │
└────────────┘└────────────────┘└────────────────┘└────────────────┘
┌──────────────┐┌──────────────────┐┌──────────────┐
│  fuzz-parity ││  embedded-matrix ││     e2e      │
│  diff fuzz   ││  mipsel/armv7/   ││  17 scen-    │
│  minimize    ││  aarch64/RISC-V  ││  arios       │
└──────────────┘└──────────────────┘└──────────────┘
```

All 7 lanes must pass before merge. A nightly workflow extends fuzz runs to 100K iterations and collects performance baselines. The release workflow is tag-triggered (`v*`) and produces signed artifact bundles with SHA-256 checksums and Sigstore signatures.

## Troubleshooting

### `ASX_E_RESOURCE_EXHAUSTED` during normal load

Your resource contract is too tight for the current workload.

```bash
asx inspect --snapshot current --format json
# Check which gauge is at capacity, then raise limits in [resource_contract]
# or move from R1 -> R2 for higher ceilings
```

### `profile-parity` fails between `core` and `embedded_router`

A resource-plane optimization likely changed semantic behavior. Semantic behavior must be identical across profiles.

```bash
asx conformance profile-parity --scenario scenarios/all --format json
asx replay --trace failing.trace.json --verify-digest
```

### JSON and BIN outputs differ

Codec implementation drift was detected. Both codecs must produce identical semantic digests.

```bash
asx conformance codec-equivalence --scenario scenarios/all
asx trace export --format json --out a.json
asx trace export --format bin --out a.bin
asx digest a.json && asx digest a.bin
```

### Runtime aborts on missing platform hooks

Common with freestanding or custom embedded integration. The runtime requires allocator, clock, and log hooks.

```bash
asx doctor --config asx.toml
# Provide the hooks required by your selected profile
```

### Non-deterministic replay mismatch

Determinism can be broken by non-seeded entropy or profile mismatch.

```bash
# Ensure deterministic settings and matching profile/seed
grep -E 'deterministic|seed|profile' asx.toml
asx replay --trace run.trace.json --verify-digest
```

### Circuit breaker stuck in OPEN state

The breaker tripped due to failure threshold exceeded. Transition to half-open for probing, or reset.

```c
asx_status st = asx_breaker_half_open(&cb);  // Probe recovery
// If probe succeeds, breaker returns to CLOSED automatically
```

### Deadline monitor reports unexpected misses

Check whether `asx_deadline_monitor_check()` is being called frequently enough in your poll loop. Misses are only detected when `check()` evaluates pending deadlines against the current time.

## Limitations

- `asx` is intentionally strict: undefined behavior in callers (invalid pointers, lifetime misuse outside API contract) is not masked. Ghost monitors catch violations in debug builds.
- The walking skeleton is single-threaded. Multi-threaded dispatch (parallel scheduler, real blocking pool threads) is planned for Phase 4.
- Deterministic guarantees assume matching scenario, profile, seed, and compatible runtime version.
- Extremely tiny targets (< 16 KB RAM) may need reduced trace retention and tighter queue ceilings; semantics remain intact, but throughput envelopes will differ.
- Binary codec schema compatibility follows explicit versioning; cross-major interoperability is not guaranteed without migration tooling.
- Native reactor backends (epoll, kqueue, IOCP) are abstracted via hooks but not yet shipped as built-in implementations. The ghost reactor provides deterministic testing.

## FAQ

### Is this a rewrite or a semantic port?

Semantic port. The C implementation is not transliterated Rust; it preserves behavior contracts through explicit state machines, parity fixtures, and replay checks. The public API is idiomatic C, not Rust-shaped.

### Does embedded mode remove features?

No. Embedded mode changes operational envelopes (limits/defaults), not semantics. Profile parity gates enforce this. `R1` on a router runs the same combinators, cancellation protocol, and obligation tracking as `R3` on a server.

### Why both JSON and binary codecs?

JSON is ideal for diagnostics, debugging, and diffing. Binary is optimized for production throughput and footprint. Both are required to produce equivalent semantic digests. This is verified by the codec-equivalence conformance gate.

### Can I run this on cheap routers?

Yes. `ASX_PROFILE_EMBEDDED_ROUTER` plus `R1/R2` resource classes target OpenWrt/BusyBox-class systems. The CI pipeline includes cross-target builds for mipsel, armv7, aarch64, and RISC-V, plus QEMU/device smoke validation.

### How do I validate parity against Rust asupersync?

Use the built-in conformance commands:

```bash
asx conformance rust-parity --fixtures fixtures/rust_reference
asx fuzz --target parity --minimize
```

### Is deterministic mode slower?

Usually slightly, depending on workload and trace settings. In exchange you gain reproducibility, easier incident debugging, and stronger regression guarantees. The HFT profile includes built-in latency histograms to measure the impact.

### Can I embed this as a library without the CLI?

Yes. The C API is first-class: 494 public functions, 95 headers, one umbrella `#include <asx/asx.h>`. CLI tooling is operational scaffolding around the same runtime and conformance layers.

### How big is the compiled library?

The static library (`libasx.a`) is typically 200-400 KB depending on profile and optimization level. With `R1` resource class on an embedded target, runtime RAM usage starts under 64 KB.

### What about thread safety?

The walking skeleton is single-threaded by design. All state is in static arenas with generation-safe handles for stale detection. The sync primitives (mutex, semaphore, barrier) provide the API surface for Phase 4 multi-threaded dispatch.

### What formal verification is included?

CBMC bounded model checking for core state machines, algebraic law verification for combinator properties (identity, commutativity, associativity), and litmus tests for memory ordering. Run `make formal-check` to execute all proofs.

### Why not just use `setjmp`/`longjmp` for async?

`setjmp`/`longjmp` gives you stackful coroutines but destroys determinism (stack layout varies by compiler/platform), makes cancellation cleanup fragile (no destructor-like guarantees), and is invisible to static analysis. `asx` uses explicit poll-based state machines (`ASX_CO_BEGIN`/`ASX_CO_YIELD`/`ASX_CO_END` protothread macros) that compile to switch statements. Every state transition is visible, auditable, and deterministically replayable.

### How does the HFT instrumentation work?

The HFT profile adds three built-in instruments:
- **Latency histogram**: 16 fixed-boundary log2 bins covering [0, 32768) nanoseconds. No floating point; bin index is `floor(log2(sample + 1))`. Records min, max, sum, and per-bin counts for O(1) percentile approximation.
- **Jitter tracker**: Streaming mean absolute deviation (MAD) computed from histogram bin midpoints. Recomputed every N samples (configurable) to bound overhead.
- **Overload policy**: Deterministic admission control with three modes: reject (hard fail), shed-oldest (evict oldest non-terminal task), and backpressure (return WOULD_BLOCK). The policy is a pure function of `(mode, load, capacity)`, making overload behavior replayable.

A metric gate evaluates pass/fail against configurable p99, p99.9, p99.99, and jitter thresholds for CI integration.

### How does the automotive instrumentation work?

The automotive profile adds four instruments:
- **Deadline tracker**: Records hit/miss per deadline evaluation. Computes miss rate as percentage * 100 (e.g., 250 = 2.5%). Tracks worst and best margins in nanoseconds (signed: negative = miss).
- **Watchdog monitor**: Tracks intervals between checkpoint calls. Violations recorded when interval exceeds `watchdog_period_ns`. Clock reversals are clamped to zero interval to prevent fabricated violations.
- **Degraded-mode audit ring**: Bounded 64-entry ring of safety-critical events (region poison, forced cancel, deadline miss, watchdog violation, degraded-mode enter/exit) with monotonic sequence numbers.
- **Compliance gate**: Evaluates pass/fail against configurable deadline miss rate, max watchdog violations, and minimum checkpoint count. Returns a violation bitmask for CI integration.

### What's the codec equivalence system?

Codec equivalence ensures that JSON and binary encodings of the same scenario produce identical semantic content. The `asx_codec_fixture_semantic_eq()` function compares fixtures field-by-field, excluding the codec identifier itself. Differences are reported as a diff of up to 32 field mismatches with path, expected value, and actual value.

Each fixture also carries provenance metadata: the Rust baseline commit hash, toolchain hash, and Cargo.lock SHA-256 that produced the reference. This lets you trace any parity failure back to the exact Rust build that generated the fixture.

### What makes the symbol registry useful?

Symbols are interned 16-bit IDs mapped to string names in a static 256-entry hash table. They provide a shared namespace for metrics, events, capabilities, and payloads across subsystems without string comparisons at runtime.

Symbol sets use a 256-bit bitfield for O(1) membership testing, union, intersection, and subset operations. Typed symbols pair a symbol ID with type metadata (kind, size, alignment) and reject mismatched re-registrations: if "request_latency" is registered as U64, attempting to re-register it as F64 returns `ASX_E_INVALID_STATE`. Typed values wrap a symbol + payload pointer with equality testing via `memcmp`.

## About Contributions

*About Contributions:* Please don't take this the wrong way, but I do not accept outside contributions for any of my projects. I simply don't have the mental bandwidth to review anything, and it's my name on the thing, so I'm responsible for any problems it causes; thus, the risk-reward is highly asymmetric from my perspective. I'd also have to worry about other "stakeholders," which seems unwise for tools I mostly make for myself for free. Feel free to submit issues, and even PRs if you want to illustrate a proposed fix, but know I won't merge them directly. Instead, I'll have Claude or Codex review submissions via `gh` and independently decide whether and how to address them. Bug reports in particular are welcome. Sorry if this offends, but I want to avoid wasted time and hurt feelings. I understand this isn't in sync with the prevailing open-source ethos that seeks community contributions, but it's the only way I can move at this velocity and keep my sanity.

## License

MIT License (with OpenAI/Anthropic Rider). See `LICENSE`.
