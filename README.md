# asupersync ANSI C (`asx`)

<div align="center">
  <img src="asupersync_ani_c_illustration.webp" alt="asupersync ANSI C (asx) - portable deterministic runtime in ANSI C">
</div>

<div align="center">

![C99](https://img.shields.io/badge/C-C99-00599C)
![No external deps](https://img.shields.io/badge/dependencies-none-brightgreen)
![Deterministic replay](https://img.shields.io/badge/replay-deterministic-orange)
![Profiles](https://img.shields.io/badge/profiles-core%20%7C%20posix%20%7C%20win32%20%7C%20freestanding%20%7C%20embedded_router-blue)
[![License: MIT+Rider](https://img.shields.io/badge/License-MIT%2BOpenAI%2FAnthropic%20Rider-blue.svg)](./LICENSE)

</div>

Portable, dependency-free, high-performance asupersync runtime in ANSI C with deterministic replay, strict resource contracts, and embedded-grade operational profiles.

<div align="center">
<h3>Quick Install</h3>

```bash
curl -fsSL https://raw.githubusercontent.com/Dicklesworthstone/asupersync_ansi_c/main/scripts/install.sh | sh
```

</div>

## TL;DR

**The Problem:** Most C async runtimes force a bad tradeoff: either you get speed without safety guarantees, or safety via heavyweight dependencies and platform lock-in. Embedded targets (routers, gateways, edge appliances) make this worse with hard memory limits and fragile storage.

**The Solution:** `asx` ports asupersync’s semantic model to ANSI C: region/task/obligation lifecycle guarantees, deterministic replay, strict OOM/exhaustion behavior, and profile-based deployment from server-class hosts down to low-cost routers.

### Why Use `asx`?

| Feature | What It Gives You |
|---|---|
| **Semantic parity with asupersync kernel** | Region trees, cancellation protocol, obligation linearity, and quiescence contracts preserved in C |
| **No external dependencies** | Pure C runtime core; easy to ship into constrained and audited environments |
| **Deterministic replay and trace hashing** | Reproduce production failures exactly and diff behavior across builds/profiles |
| **Dual codecs (JSON + binary)** | JSON for debug/conformance, binary for production throughput/footprint |
| **Resource contracts instead of silent degradation** | Explicit memory/queue/timer ceilings with deterministic failure taxonomy |
| **Embedded router profile** | OpenWrt-class targets, QEMU + device smoke gates, RAM-ring diagnostics |
| **Cross-profile semantic parity gates** | `CORE`, `FREESTANDING`, and `EMBEDDED_ROUTER` produce equivalent semantic digests |

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

## Design Philosophy

1. **Semantics first, mechanics second**
   The runtime never trades away lifecycle correctness, cancellation semantics, or obligation linearity for speed hacks.

2. **Determinism is a feature, not a debug trick**
   Reproducibility is part of the contract. If a failure happened once, you can replay it and reason about it.

3. **Resource pressure must be explicit**
   In constrained systems, “best effort” often means undefined behavior. `asx` uses deterministic exhaustion and failure-atomic boundaries.

4. **Portable core, specialized adapters**
   Core logic is platform-neutral ANSI C. OS- or device-specific behavior lives behind profile/platform adapters.

5. **Evidence-gated optimization**
   Performance work requires baseline artifacts, hotspot evidence, semantic proof, and rollback path.

## How `asx` Compares

| Capability | `asx` | Ad-hoc C event loops | Generic async frameworks | Rust asupersync |
|---|---|---|---|---|
| Region/task/obligation semantic model | ✅ Full | ❌ Usually absent | ⚠️ Partial/varies | ✅ Full |
| Deterministic replay hash chain | ✅ Built-in | ❌ | ⚠️ Rare | ✅ Built-in |
| Strict OOM/exhaustion semantics | ✅ Contractual | ❌ | ⚠️ Framework-specific | ✅ Contractual |
| Zero external runtime deps | ✅ | ✅ | ❌ Common | ❌ Rust toolchain/runtime assumptions |
| Embedded router profile | ✅ | ⚠️ DIY | ⚠️ Often heavy | ⚠️ Target/toolchain constraints |
| Rust parity conformance suite | ✅ | ❌ | ❌ | N/A |

**Use `asx` when you need:**
- hard behavioral guarantees in plain C,
- deterministic incident reproduction,
- embedded viability without feature amputations.

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
```

### 4) Cross-Compile for Router Targets

```bash
# Example: mipsel OpenWrt toolchain
make release TARGET=mipsel-openwrt-linux-musl

# Example: armv7 OpenWrt toolchain
make release TARGET=armv7-openwrt-linux-muslgnueabi
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

## C API Quick Start

```c
#include <asx/asx.h>

int main(void) {
    asx_runtime_config cfg = asx_runtime_config_default();
    cfg.profile = ASX_PROFILE_EMBEDDED_ROUTER;
    cfg.resource_class = ASX_CLASS_R2;
    cfg.deterministic = 1;
    cfg.seed = 42;
    cfg.codec = ASX_CODEC_BIN;

    asx_runtime* rt = NULL;
    asx_status st = asx_runtime_create(&cfg, &rt);
    if (st != ASX_OK) return 1;

    st = asx_runtime_run(rt);
    asx_runtime_destroy(rt);
    return st == ASX_OK ? 0 : 2;
}
```

## Command Reference

Global flags:

```bash
--config <path>         # Config file path (default: ./asx.toml)
--profile <name>        # core|posix|win32|freestanding|embedded_router
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
profile = "embedded_router"        # core | posix | win32 | freestanding | embedded_router
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

## Architecture

```text
                           ┌───────────────────────────────────────┐
                           │              Inputs                   │
                           │  scenarios | API calls | trace files  │
                           └───────────────────────────────────────┘
                                           │
                                           ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                asx_core                                         │
│ IDs + generation counters | outcome/budget/cancel | invariant/state authorities│
│ ownership tables | error taxonomy | codec schema model                          │
└─────────────────────────────────────────────────────────────────────────────────┘
                                           │
                                           ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                           asx_runtime_kernel                                    │
│ scheduler | region/task/obligation lifecycle | cancel propagation | timer wheel │
│ deterministic event journal + semantic digest                                   │
└─────────────────────────────────────────────────────────────────────────────────┘
                 │                          │                            │
                 ▼                          ▼                            ▼
┌──────────────────────────┐    ┌──────────────────────────┐   ┌──────────────────────────┐
│      Codec Layer         │    │   Conformance Layer      │   │    Profile Adapters      │
│ JSON + BIN, equivalent   │    │ Rust parity + fuzz +     │   │ core/posix/win32/        │
│ semantic schema          │    │ counterexample minimizer │   │ freestanding/embedded     │
└──────────────────────────┘    └──────────────────────────┘   └──────────────────────────┘
                 │                          │                            │
                 └───────────────┬──────────┴───────────────┬────────────┘
                                 ▼                          ▼
                    ┌──────────────────────────┐  ┌──────────────────────────┐
                    │ Trace/Replay Artifacts   │  │ Runtime Deployments      │
                    │ digest-stable evidence   │  │ server + router + edge   │
                    └──────────────────────────┘  └──────────────────────────┘
```

## Repository Layout

```text
include/asx/                Public C headers
src/core/                   Core semantic types and state logic
src/runtime/                Scheduler, lifecycle engine, timer wheel
src/channel/                Bounded MPSC two-phase channel kernel
src/time/                   Time abstractions and timer primitives
src/platform/               posix/win32/freestanding/embedded adapters
tests/unit/                 Unit tests per module
tests/invariant/            Lifecycle and protocol invariants
tests/conformance/          Rust parity and codec/profile equivalence
fixtures/rust_reference/    Canonical fixtures captured from Rust runtime
tools/                      Capture, replay, fuzz, and minimization tooling
docs/                       Port architecture and parity tracking documents
```

## Performance and Footprint

Guaranteed/targeted properties in production builds:

- O(1) amortized ready/cancel queue operations,
- O(1) timer cancel via generation-safe handles,
- steady-state scheduler hot path without heap allocations,
- deterministic digest stability across repeated runs with identical seed/input,
- embedded resource-class operation with deterministic exhaustion behavior.

Recommended benchmark flow:

```bash
asx bench --suite core
asx bench --suite embedded --profile embedded_router --resource-class R1
asx conformance profile-parity --scenario scenarios/perf-critical
```

## Testing and Quality Gates

`asx` ships with:

- unit tests for all modules,
- invariant tests for lifecycle legality and linearly-used obligations,
- Rust-vs-C conformance suite,
- codec equivalence suite (JSON vs BIN),
- profile semantic parity suite (`core` vs `freestanding` vs `embedded_router`),
- differential fuzzing with deterministic minimization,
- cross-target embedded matrix (mipsel/armv7/aarch64) including QEMU runs.

Typical CI command set:

```bash
make test
make test-invariants
make conformance
make fuzz-smoke
make ci-embedded-matrix
```

## Troubleshooting

### `ASX_E_RESOURCE_EXHAUSTED` during normal load

Your resource contract is too tight for current workload.

```bash
asx inspect --snapshot current --format json
# raise limits in [resource_contract] or move R1 -> R2
```

### `profile-parity` fails between `core` and `embedded_router`

A resource-plane optimization likely changed semantic behavior.

```bash
asx conformance profile-parity --scenario scenarios/all --format json
asx replay --trace failing.trace.json --verify-digest
```

### JSON and BIN outputs differ

Codec implementation drift was detected.

```bash
asx conformance codec-equivalence --scenario scenarios/all
asx trace export --format json --out a.json
asx trace export --format bin --out a.bin
asx digest a.json && asx digest a.bin
```

### Runtime aborts on missing platform hooks

Common with freestanding or custom embedded integration.

```bash
asx doctor --config asx.toml
# provide allocator/clock/log hooks required by selected profile
```

### Non-deterministic replay mismatch

Determinism can be broken by non-seeded entropy or profile mismatch.

```bash
# Ensure deterministic settings and same profile/seed
grep -E 'deterministic|seed|profile' asx.toml
asx replay --trace run.trace.json --verify-digest
```

## Limitations

- `asx` is intentionally strict: undefined behavior in callers (invalid pointers, lifetime misuse outside API contract) is not masked.
- Some advanced surfaces in higher-phase modules require explicit platform adapters and kernel-level capabilities on minimal systems.
- Deterministic guarantees assume matching scenario, profile, seed, and compatible runtime version.
- Extremely tiny targets may need reduced trace retention and tighter queue ceilings; semantics remain intact, but throughput envelopes will differ.
- Binary codec schema compatibility follows explicit versioning; cross-major interoperability is not guaranteed without migration tooling.

## FAQ

### Is this a rewrite or a semantic port?

Semantic port. The C implementation is not transliterated Rust; it preserves behavior contracts through explicit state machines, parity fixtures, and replay checks.

### Does embedded mode remove features?

No. Embedded mode changes operational envelopes (limits/defaults), not semantics. Profile parity gates enforce this.

### Why both JSON and binary codecs?

JSON is ideal for diagnostics and diffing. Binary is optimized for production throughput and footprint. Both are required to produce equivalent semantic digests.

### Can I run this on cheap routers?

Yes. `ASX_PROFILE_EMBEDDED_ROUTER` plus `R1/R2` resource classes target OpenWrt/BusyBox-class systems, with cross-target CI and QEMU/device smoke validation.

### How do I validate parity against Rust asupersync?

Use the built-in conformance commands:

```bash
asx conformance rust-parity --fixtures fixtures/rust_reference
asx fuzz --target parity --minimize
```

### Is deterministic mode slower?

Usually slightly, depending on workload and trace settings. In exchange you gain reproducibility, easier incident debugging, and stronger regression guarantees.

### Can I embed this as a library without the CLI?

Yes. The C API is first-class; CLI tooling is operational scaffolding around the same runtime and conformance layers.

## About Contributions

*About Contributions:* Please don't take this the wrong way, but I do not accept outside contributions for any of my projects. I simply don't have the mental bandwidth to review anything, and it's my name on the thing, so I'm responsible for any problems it causes; thus, the risk-reward is highly asymmetric from my perspective. I'd also have to worry about other "stakeholders," which seems unwise for tools I mostly make for myself for free. Feel free to submit issues, and even PRs if you want to illustrate a proposed fix, but know I won't merge them directly. Instead, I'll have Claude or Codex review submissions via `gh` and independently decide whether and how to address them. Bug reports in particular are welcome. Sorry if this offends, but I want to avoid wasted time and hurt feelings. I understand this isn't in sync with the prevailing open-source ethos that seeks community contributions, but it's the only way I can move at this velocity and keep my sanity.

## License

MIT License (with OpenAI/Anthropic Rider). See `LICENSE`.
