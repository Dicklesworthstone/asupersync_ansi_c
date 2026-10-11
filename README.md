# asupersync ANSI C (`asx`)

<div align="center">
  <img src="asupersync_ani_c_illustration.webp" alt="asupersync ANSI C (asx) - portable deterministic runtime in ANSI C">
</div>

<div align="center">

![C99](https://img.shields.io/badge/C-C99-00599C)
![No external deps](https://img.shields.io/badge/dependencies-none-brightgreen)
![Deterministic replay](https://img.shields.io/badge/replay-deterministic-orange)
![Public API declarations](https://img.shields.io/badge/public%20API-1%2C985%20declarations-blue)
![C test programs](https://img.shields.io/badge/tests-225%20programs-brightgreen)
![9 profiles](https://img.shields.io/badge/profiles-9%20deployment%20targets-blue)
[![License: MIT+Rider](https://img.shields.io/badge/License-MIT%2BOpenAI%2FAnthropic%20Rider-blue.svg)](./LICENSE)

</div>

Portable, dependency-free async runtime in ANSI C with deterministic replay, strict resource contracts, and 9 deployment profiles spanning servers to low-cost routers. <!-- fact:api_declarations -->1,985<!-- /fact --> exported `ASX_API` declarations across <!-- fact:header_families -->38<!-- /fact --> public header families, backed by <!-- fact:test_programs -->225<!-- /fact --> C test programs across unit, invariant, vignette, e2e, conformance, fuzz, and formal layers.

<div align="center">
<h3>Quick Source Build</h3>

```bash
git clone https://github.com/Dicklesworthstone/asupersync_ansi_c.git
cd asupersync_ansi_c
make build
make test
```

</div>

## TL;DR

**The Problem:** Most C async runtimes force a tradeoff: speed without safety guarantees, or safety via heavyweight dependencies and platform lock-in. Embedded targets (routers, gateways, edge appliances) make this worse with hard memory limits and fragile storage. When something goes wrong, you can't reproduce it.

**The Solution:** `asx` ports the core of asupersync's semantics to ANSI C: region/task/obligation lifecycle, structured cancellation, budgets, channels, synchronization, task groups and actors, plus deterministic replay, fixed-size arenas that fail with explicit errors instead of degrading, and build profiles from servers to router-class targets. It is a partial port, and parity is measured rather than assumed: `make conformance` runs scenarios captured from asupersync's LabRuntime through the C runtime and compares the results exactly, and CI runs seeded Rust-vs-C differential fuzzing on every push. Surfaces the scenario language cannot express (networking, files, processes, HTTP) are not parity-checked. In a deterministic build, the same scenario and seed reproduce the same run.

### Status against asupersync (Rust)

The port targets asupersync commit `5e60b1c4c` (2026-10).

- **Compared with the Rust runtime, and matching:**
  - `make conformance` runs <!-- fact:rust_fixtures -->113<!-- /fact --> scenarios captured from asupersync's `LabRuntime` (`fixtures/rust_reference_v2`) through the C runtime. Each must match the capture's trace class, final snapshot, step observations and lab dispatch order.
  - CI also generates 200 scenarios on every push and compares the two runtimes live (`make fuzz-differential`, seed 9).
  - It checks that the two trace canonicalizers agree on 100,000 random traces (`make canon-differential`).
  - It compares the kernel constants and defaults no scenario observes, such as cancel-kind tables, budgets, the timer limit, supervision defaults and lab batch sizes, with Rust's (`make check-rust-constants`). `twin_run` reads Rust's values from the pinned crate into `schemas/rust_kernel_constants.json`. Each value matches, or is a recorded difference that names its bead.
  - Covered areas: region/task/obligation lifecycle, cancellation and masking, budgets, mpsc/oneshot/broadcast/watch channels, mutex/rwlock/semaphore/notify/barrier, task groups (join_all, race with a deadline, first_ok, quorum), region admission limits, GenServers (cast, call, stop, cancellation and the mailbox drain) and managed supervisors (one_for_one, one_for_all and rest_for_one restarts, restart intensity and backoff, stop or escalate when the limit is reached). All of it runs under the lab's single-worker dispatch model.
- **Known differences, open:** none in what the scenario language expresses ([`docs/SCENARIO_DSL_V2.md`](docs/SCENARIO_DSL_V2.md) §7 lists the closed ones); rule by rule in [`docs/C_REFINEMENT_MAP.md`](docs/C_REFINEMENT_MAP.md).
- **Not compared with Rust; tested in C only:** networking, files, processes and HTTP; live, non-lab scheduling.
- **Not ported:**
  - actors: the plain `Actor` trait, system messages, overflow policies, names, monitors and links;
  - supervision: registries, dynamic supervisors with shared restart domains, restart storm detection, per-child shutdown budgets;
  - sync: `Mutex::lock_until`;
  - I/O: kqueue, IOCP and io_uring reactors, and Windows native I/O.

  The module-by-module accounting is in [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](docs/RUST_EXPORTED_SURFACE_INVENTORY.md) §11 and, per semantic unit, in [`docs/FEATURE_PARITY.md`](docs/FEATURE_PARITY.md).

### Why Use `asx`?

| Feature | What It Gives You |
|---|---|
| **<!-- fact:api_declarations -->1,985<!-- /fact --> exported `ASX_API` declarations across <!-- fact:header_families -->38<!-- /fact --> header families** | Async runtime API: scheduler, channels, sync primitives, actors, combinators, timers, codecs, diagnostics, and more |
| **No external dependencies** | Pure C runtime core; ships into constrained and audited environments unchanged |
| **Deterministic replay and trace hashing** | Deterministic builds replay a scenario exactly from its seed and input; trace digests let you diff behavior across builds, profiles, and codec modes |
| **Structured cancellation with witness protocol** | 11 cancel kinds with severity lattice, witness phase tracking, and cleanup budgets (advisory as in Rust; an opt-in hard bound) |
| **Async combinators** | Poll-based join, race, select, timeout, retry, bracket, pipeline, quorum, first-ok, hedge and map-reduce (plus race and retry with a timeout), and bulkhead and rate-limit admission counters |
| **Circuit breaker and epoch-based execution** | Failure containment with open/half-open/closed states; phase-scoped execution with barrier triggers |
| **Dual codecs (JSON + binary)** | JSON for debugging and diffing, binary for compact encoding; both encode the same canonical fixture model, and round trips through each are checked to agree |
| **<!-- fact:status_codes -->66<!-- /fact --> typed error codes with recovery guidance** | Each error has a category, recoverability class, recovery action, and backoff hints |
| **Resource contracts instead of silent degradation** | Explicit memory/queue/timer ceilings with deterministic failure taxonomy per resource class (R1/R2/R3) |
| **9 deployment profiles** | CORE, POSIX, WIN32, FREESTANDING, EMBEDDED_ROUTER, HFT, AUTOMOTIVE, PARALLEL, BROWSER |
| **<!-- fact:test_programs -->225<!-- /fact --> tracked C test programs across 7 categories** | <!-- fact:test_unit -->164<!-- /fact --> unit, <!-- fact:test_e2e -->23<!-- /fact --> e2e, <!-- fact:test_invariant -->3<!-- /fact --> invariant, <!-- fact:test_vignettes -->12<!-- /fact --> vignette, <!-- fact:test_conformance -->6<!-- /fact --> conformance, <!-- fact:test_fuzz -->4<!-- /fact --> fuzz, and <!-- fact:test_formal -->13<!-- /fact --> formal files in the current tree |
| **Cross-profile checks** | Profiles are meant to change limits and defaults, never semantics. CI executes the Rust-captured fixtures under CORE, under EMBEDDED_ROUTER on five ISAs (QEMU) and on 32-bit x86; `make profile-parity` compares the digests recorded per profile in the fixture set |

The current repository is library-first: it ships the static library, public
headers, tests, examples, release-packaging machinery, the in-tree
`scripts/install.sh` helper, and a convenience CLI build target (`make cli`).
`make install` still installs the library and headers only; the `asx` CLI is an
in-tree convenience binary rather than the default installed artifact.

## Quick Repository Workflow

```bash
# 1) Build the static library with strict warnings-as-errors
make build

# 2) Run the current local test gate
make test

# 3) Check fixture integrity and recorded codec/profile digests
make fixture-integrity
make codec-equivalence
make profile-parity
# Rust parity: every Rust-captured fixture in fixtures/rust_reference_v2 is
# executed through the C runtime and compared (blocking in CI)
make conformance
# Seeded differential fuzzing against asupersync itself (builds twin_run)
make fuzz-differential FUZZ_V2_SEED=1 FUZZ_V2_COUNT=100

# 4) Produce deterministic release bundles (libasx.a + headers/docs)
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=linux-x86_64 PROFILE=CORE CODEC=BIN DETERMINISTIC=1
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

See `examples/` for more: channel flow, actor supervision, cancellation draining, timers and budgets, trace replay, browser boundary, and network surface demos.

## Design Philosophy

1. **Semantics first, mechanics second**
   The runtime never trades away lifecycle correctness, cancellation semantics, or obligation linearity for speed hacks. State machines are explicit. Transitions are validated.

2. **Determinism is a feature, not a debug trick**
   Reproducibility is part of the contract. In a deterministic build (`DETERMINISTIC=1`, the default), a run depends only on its scenario and seed: replaying with the same seed reproduces it, and trace digests are stable across runs. Live builds take time, entropy and I/O from the OS, so they cannot be replayed this way.

3. **Resource pressure must be explicit**
   In constrained systems, "best effort" often means undefined behavior. `asx` uses deterministic exhaustion and failure-atomic boundaries. Arena sizes are fixed at build time, and every resource class (R1/R2/R3) publishes concrete limits.

4. **Portable core, specialized adapters**
   Core logic is platform-neutral ANSI C. OS- or device-specific behavior lives behind profile/platform adapters. CI builds and tests it on Linux with GCC and Clang, on five router-class ISAs under QEMU, on 32-bit x86, and on Windows with MSVC (deterministic builds; Windows has no native I/O yet). Bare-metal targets have make targets (`make cross-baremetal-*`) that CI does not run, and there is no WebAssembly build yet.

5. **Evidence-gated optimization**
   Performance work requires baseline artifacts, hotspot evidence, semantic proof, and rollback path. Instrumentation modules for latency histograms, jitter tracking, and deadline/watchdog compliance (aimed at the HFT and automotive profiles) record the samples an application feeds them.

## How `asx` Compares

| Capability | `asx` | Ad-hoc C event loops | Generic async frameworks | Rust asupersync |
|---|---|---|---|---|
| Region/task/obligation semantic model | Core ported; parity measured against Rust-captured scenarios | Usually absent | Partial/varies | Reference implementation |
| Structured cancellation (11 kinds, witness protocol) | Built-in | Manual | Framework-specific | Built-in |
| Deterministic replay hash chain | Built-in (deterministic builds) | No | Rare | Built-in (lab runtime) |
| Strict OOM/exhaustion semantics | Fixed build-time arenas, explicit exhaustion errors | No | Framework-specific | Heap-allocated |
| Async combinators (join/race/select/retry/bracket) | Built-in | DIY | Varies | Built-in |
| Circuit breaker + epoch scoping | Built-in | No | Sometimes | Built-in |
| Native I/O reactors | epoll/poll(2), POSIX live builds only | DIY | Varies | epoll, kqueue, io_uring, Windows, browser |
| Zero external runtime deps | Yes (C library and OS APIs only) | Yes | Usually no | No (crate dependencies) |
| Embedded router profile (OpenWrt/QEMU) | Yes | DIY | Usually too heavy | No router profile |
| HFT tail-latency instrumentation | Built-in module (application-fed) | No | No | Metrics histograms, no HFT profile |
| Automotive deadline/watchdog compliance | Built-in module (application-fed) | No | No | Deadline monitor, no watchdog gate |
| Rust parity conformance suite | Yes | N/A | N/A | N/A |
| Formal methods | Exhaustive state-machine and algebraic property tests (CBMC-ready harnesses, CBMC not run) | No | Rare | Lean and TLA+ models (`formal/`) |

**Use `asx` when you need:**
- hard behavioral guarantees in plain C,
- deterministic, seed-driven reproduction of scenarios,
- embedded builds that keep the same kernel semantics (native OS I/O is compiled only into the POSIX profile today),
- one codebase that builds for 9 profiles from HFT to routers.

**Use alternatives when you need:**
- rapid app scaffolding over strict semantic control,
- large existing ecosystem integrations that outweigh runtime guarantees.

## Installation

The current repository ships:
- the static library `libasx.a`
- public headers under `include/asx/`
- the in-tree installer helper `scripts/install.sh`
- the in-tree convenience CLI target `make cli` (builds `build/bin/asx`)
- examples, tests, fixtures, and deterministic release packaging

The current repository does not ship:
- package-manager metadata that makes `brew install asx` / `apt install asx`
  / `opkg install asx` true for this repo by itself
- a default `make install` path that installs the `asx` CLI binary

### 1) From Source

```bash
git clone https://github.com/Dicklesworthstone/asupersync_ansi_c.git
cd asupersync_ansi_c
make release
sudo make install

# Convenience profile lanes
make build-browser
make build-parallel  # PARALLEL profile build with real atomics (lanes still run on one thread)
```

`make install` currently installs the static library plus public headers under
`$(PREFIX)`; it does not install a CLI binary.

If you want the in-tree convenience CLI as well, build it explicitly:

```bash
make cli
./build/bin/asx help
```

If you want a one-step source install helper for the library surface, the
repository also ships:

```bash
./scripts/install.sh [--prefix=/usr/local]
```

### 2) Release Artifacts

```bash
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=linux-x86_64 PROFILE=CORE CODEC=BIN DETERMINISTIC=1
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=source RELEASE_KIND=source
```

### 3) Cross-Compile for Embedded Targets

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

1. Build the library:
```bash
make build
```
2. Run the default local verification lane:
```bash
make test
```
3. Run the fixture and recorded-digest gates:
```bash
make fixture-integrity
make codec-equivalence
make profile-parity
```
4. Explore runnable examples and API walkthroughs:
```bash
ls examples
ls tests/vignettes
```

## Subsystem Overview

The main subsystem families are below. Their public headers live under `include/asx/` (channels and parts of the kernel under `core/` and `runtime/`), and their unit tests under `tests/unit/`.

| Subsystem | Purpose |
|---|---|
| **core** | Fundamental types: IDs, outcomes, budgets, symbols, cancellation, ghost monitors, combinators, epochs, circuit breakers |
| **runtime** | Wake-driven scheduler (park/wake, task timers, join/watch), lifecycle engine, budgets, obligation holders and leak policy, cancel masking, task groups (race/join/first-ok/quorum with loser drain), builder, blocking pool, readiness reactor and I/O driver, deadline monitor, waker system, virtual time, telemetry, diagnostics, HFT/automotive instrumentation |
| **channel** | Bounded MPSC, oneshot, broadcast, watch channels, and session endpoints |
| **sync** | Mutex, rwlock, semaphore, barrier (N-way rendezvous with leader election), once, notify |
| **actor** | GenServers (task, mpsc mailbox, cast/call/stop/join, drain on cancel) and managed supervisors (child generations in regions of their own, restart strategies, intensity, backoff, escalation) |
| **cx** | Capability context and structured concurrency scoping |
| **codec** | JSON + binary codecs with equivalence checking and schema validation |
| **time** | Deadline abstraction, sleep primitives, timer wheel with generation-safe handles |
| **stream** | Poll-based async iterators and streaming combinators |
| **bytes** | Buffer management, codec bridges, I/O adapters |
| **security** | Ambient-authority audit catalog, authenticated symbols and security contexts, crypto primitives (SHA-1/256/512, HMAC-SHA256, HKDF-SHA256, constant-time compare, Base64/hex, whitening of a caller-supplied entropy source) |
| **net** | TCP/UDP over in-memory or native sockets, DNS (getaddrinfo on the blocking pool), HTTP/1.1 server/client, WebSocket (RFC 6455) |
| **raptorq** | RFC 6330 fountain-code encoder/decoder (its unit tests match golden vectors from the crates.io `raptorq` 2.0.1 crate) |
| **fs** | Files and directories: in-memory model, plus native POSIX backend |
| **process** | Child processes: spawn, stdio pipes, exit status, kill/reap (native POSIX backend) |
| **signal** | Signal subscriptions with task wakeups; graceful shutdown on SIGTERM/SIGINT |
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

## Build and Verification Reference

This repository currently exposes build/test/release entry points through
`make` and library/operator surfaces through the public headers. `make cli`
also builds an in-tree convenience binary, `build/bin/asx` (`version`,
`info`, `doctor`, `help`); `make install` does not install it.

Build knobs:

```bash
PROFILE=CORE|POSIX|WIN32|FREESTANDING|EMBEDDED_ROUTER|HFT|AUTOMOTIVE|PARALLEL|BROWSER
CODEC=JSON|BIN
DETERMINISTIC=0|1
BUILD_TYPE=debug|release
TARGET=<cross-triplet>
BITS=32|64
```

Parallel operator knobs:

- `make build-parallel` builds `PROFILE=PARALLEL` with
  `ASX_LOCKFREE_SINGLE_THREAD=0`, so the atomic layer uses real atomic
  operations instead of plain loads and stores. The worker lanes still run on
  the calling thread.
- Generic PARALLEL builds represent up to 64 logical workers. Constrained
  profiles intentionally cap lower: freestanding/browser at 1,
  embedded-router at 4, and HFT/automotive at 16 unless `ASX_MAX_WORKERS` is
  explicitly overridden and revalidated.
- Worker count is resource-plane configuration. It must not change canonical
  semantic digests; use `make parallel-parity` before claiming a new worker
  count or platform path is equivalent.
- Admission telemetry is observe-only by default. Enforced reject/backpressure
  behavior must be selected explicitly and remains failure-atomic.
- For large-swarm evidence, run `make test-e2e-parallel` for structured JSONL
  scenario logs and `rch exec -- make parallel-bench-json` for the
  PARALLEL-profile 1/2/8/32/64 worker-count baseline artifact. Use
  `rch exec -- make parallel-bench-gate` when you want the artifact captured
  under `build/perf/parallel-bench-results.json` plus observe-only
  gross-regression warnings. Use `rch exec -- make wave-c-acceptance-demo` for
  the user-facing Wave C bundle under
  `build/e2e-artifacts/wave-c-acceptance-*`.
- Baseline refreshes are intentional changes, not incidental benchmark churn.
  Update `tools/ci/slo_baselines.json` only with a fresh artifact, git commit,
  profile, compiler, target, command, scenario set, timestamp, and threshold
  policy. The SLO evaluator rejects stale metadata, missing benchmark reports,
  profile mismatches, and supplied-command mismatches; it keeps resource-plane
  latency deltas separate from semantic proof, which still comes from
  `profile-parity` and conformance gates.

### `make build`

Compile `libasx.a` with strict warnings-as-errors.

```bash
make build
make build PROFILE=POSIX CODEC=BIN
```

### `make test`

Run the default local verification lane.

```bash
make test
make test-unit
make test-invariants
make test-vignettes
```

### `make conformance`

Rust parity: each fixture in `fixtures/rust_reference_v2` is the unmodified
output of `tools/twin_run`, which runs an `asx.scenario.v2` scenario inside
asupersync's LabRuntime at the pinned rev. `build/bin/asx-conformance` runs
the same scenario through the C runtime (lab dispatch, same seed; the runner
links a resource class R3 build of the library, since a supervisor opens a
region per child generation) and
compares the trace, final snapshot, per-step observations and the lab's
dispatch order (which task each step polled, from which lane, at what time)
exactly. The target fails on any difference and on an empty fixture set; CI
runs it as a blocking step. A failing fixture is fixed on the C side, never
by editing the fixture.

`make fuzz-differential` does the same for seeded generated scenarios
(`FUZZ_V2_SEED`, `FUZZ_V2_COUNT`), with Rust run live through twin_run; it
needs the Rust toolchain from `rust-toolchain.toml`. CI runs it on every push
and pull request with a fixed seed (9, 200 scenarios), and the nightly
workflow tries a new seed each day. The generator (`tools/twin_run/src/gen.rs`)
draws lifecycle, cancellation, budget, obligation, region, task-group,
mpsc, oneshot, broadcast, watch, mutex, rwlock, semaphore, notify, barrier
and GenServer (spawn, cast, call, stop) steps, and about one scenario in
twelve queues 17 to 32 tasks on one lock; it also starts supervisors of one
to three children and joins some of them. Race
groups are generated: a
same-round tie is drawn from the owner's entropy, which C keeps as Rust's
per-task DetEntropy streams under lab dispatch.

What this covers is what the scenario language (`docs/SCENARIO_DSL_V2.md`)
can express: lifecycle, cancellation, budgets, obligations, task groups,
channels, sync primitives, actors and supervision, run under the lab's
single-worker dispatch. Networking, files, processes, HTTP and live
(non-deterministic) builds are outside it. That document's §7 lists the
C-side gaps found and closed.

```bash
make conformance        # executed C-vs-Rust comparison of every v2 fixture
make fuzz-differential FUZZ_V2_SEED=7 FUZZ_V2_COUNT=200
make fixtures-fresh        # every fixture reproduced by the pinned asupersync + current twin_run (cargo)
make check-rust-constants  # C kernel constants vs schemas/rust_kernel_constants.json
make rust-constants-fresh  # re-derive that document from the pinned asupersync (cargo)
make fixture-integrity  # schema, provenance, digest recompute, codec round trip
make test-gates         # negative controls: the gates must reject bad fixtures
```

### `make codec-equivalence`

Compare the semantic digests recorded in the JSON and BIN files of each
fixture in `fixtures/rust_reference/`. It reads the fixtures and does not
execute the runtime; the C codec round trip is checked by `make
fixture-integrity` and by `make test-conformance-c`.

```bash
make codec-equivalence
```

### `make profile-parity`

Compare the semantic digests recorded for each profile variant of a fixture
in `fixtures/rust_reference/`. Like `codec-equivalence`, it does not execute
the runtime. Executed cross-profile evidence comes from `make conformance`
run under several profiles and targets (see the CI table below) and from
`make resource-pressure-gate`.

```bash
make profile-parity
```

### `make fuzz-smoke`

Run the C fuzz harness's smoke lane (`tests/fuzz/fuzz_differential.c`):
self-consistency and crash freedom of the C runtime. It compares against Rust
only when given `RUST_FUZZ_BINARY=...`; Rust-vs-C differential fuzzing proper
is `make fuzz-differential`.

```bash
make fuzz-smoke
```

### `make fuzz-counterexample-replay`

Replay the durable minimized fuzz counterexample corpus. The gate runs each
fixture with deterministic seeds, checks the C fuzz harness summary counters,
optionally compares a Rust reference binary when provided, and writes a
machine-readable report under `build/fuzz/counterexamples/`.

```bash
make fuzz-counterexample-replay
make fuzz-counterexample-replay RUST_FUZZ_BINARY=tools/rust_fuzz_target/target/release/rust_fuzz_target
```

### `make resource-pressure-gate`

Replay the resource-pressure failure-atomic scenario pack. The gate runs the
same region/task/obligation/channel/timer/scheduler/cancel cleanup scenarios
for CORE/R3 and EMBEDDED_ROUTER/R1 lanes, each built with its class's arenas
(`RESOURCE_CLASS=3` / `1`), so an R1 lane exhausts at 4 regions and 16 tasks.
It checks exact error outcomes and that each lane's arenas are its class
limits, writes per-case snapshots, and rejects cross-lane semantic digest
drift.

```bash
make resource-pressure-gate
```

### `make bench`

Run the benchmark suite.

```bash
make bench
make bench PROFILE=EMBEDDED_ROUTER
```

### `make release-artifacts`

Produce deterministic artifact bundles and integrity metadata.

```bash
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=linux-x86_64 PROFILE=CORE CODEC=BIN DETERMINISTIC=1
```

### `make release-install-smoke`

Build the binary release artifact, unpack it, install the extracted payload into
a fresh prefix, compile the README quickstart against only the installed
headers/library, run that binary, and emit a machine-readable report.

```bash
make release-install-smoke RELEASE_INSTALL_SMOKE_VERSION=0.1.0 RELEASE_INSTALL_SMOKE_TARGET=linux-x86_64
```

### Public Operator Surfaces

- Runtime construction: `include/asx/runtime/builder.h`, `include/asx/runtime/rt.h`, `include/asx/runtime/runtime.h`
- App bootstrap helpers: `include/asx/app/app.h`
- Doctor/reporting surfaces: `include/asx/app/doctor.h`, `include/asx/app/report.h`, `include/asx/console/console.h`
- CLI helpers and in-tree convenience binary surface: `include/asx/cli/cli.h`, `src/cli/main.c`, `make cli`

## Configuration

Runtime configuration is currently programmatic via `asx_runtime_config`,
`asx_runtime_builder`, and environment overrides consumed by
`asx_runtime_builder_apply_env()` / `asx_runtime_init_from_env()`. This tree
does not currently ship a TOML parser or `asx.toml` generator.

```c
#include <asx/asx.h>
#include <asx/runtime/builder.h>

asx_status build_runtime(asx_runtime *runtime) {
    asx_runtime_builder builder;
    asx_status st = asx_runtime_builder_init_current_thread(&builder);
    if (st == ASX_OK) st = asx_runtime_builder_set_wait_policy(&builder, ASX_WAIT_YIELD);
    if (st == ASX_OK) st = asx_runtime_builder_set_io_backend(&builder, ASX_IO_BACKEND_GHOST);
    if (st == ASX_OK) st = asx_runtime_builder_set_finalizer_poll_budget(&builder, 512u);
    if (st == ASX_OK) st = asx_runtime_builder_set_finalizer_time_budget_ns(&builder, 5000000u);
    if (st == ASX_OK) st = asx_runtime_builder_build(&builder, runtime);
    return st;
}
```

Environment-driven overrides use the `ASX_RUNTIME_` prefix:

```bash
ASX_RUNTIME_PRESET=current-thread
ASX_RUNTIME_WAIT_POLICY=yield
ASX_RUNTIME_IO_BACKEND=ghost
ASX_RUNTIME_FINALIZER_POLL_BUDGET=512
ASX_RUNTIME_FINALIZER_TIME_BUDGET_NS=5000000
```

### Resource Classes

Each resource class defines concrete capacity limits:

| Resource | R1 (Tight) | R2 (Balanced) | R3 (Roomy) |
|---|---|---|---|
| Max regions | <!-- fact:class_r1_max_regions -->4<!-- /fact --> | <!-- fact:class_r2_max_regions -->16<!-- /fact --> | <!-- fact:class_r3_max_regions -->64<!-- /fact --> |
| Max tasks | <!-- fact:class_r1_max_tasks -->16<!-- /fact --> | <!-- fact:class_r2_max_tasks -->64<!-- /fact --> | <!-- fact:class_r3_max_tasks -->256<!-- /fact --> |
| Max timers | <!-- fact:class_r1_max_timers -->32<!-- /fact --> | <!-- fact:class_r2_max_timers -->128<!-- /fact --> | <!-- fact:class_r3_max_timers -->512<!-- /fact --> |
| Max obligations | <!-- fact:class_r1_max_obligations -->16<!-- /fact --> | <!-- fact:class_r2_max_obligations -->64<!-- /fact --> | <!-- fact:class_r3_max_obligations -->256<!-- /fact --> |
| Max channels | <!-- fact:class_r1_max_channels -->8<!-- /fact --> | <!-- fact:class_r2_max_channels -->32<!-- /fact --> | <!-- fact:class_r3_max_channels -->128<!-- /fact --> |
| Max trace events | <!-- fact:class_r1_max_trace_events -->64<!-- /fact --> | <!-- fact:class_r2_max_trace_events -->256<!-- /fact --> | <!-- fact:class_r3_max_trace_events -->1,024<!-- /fact --> |

To build for a class, use `make RESOURCE_CLASS=1` (or `2`, `3`; the C
macro is `-DASX_RESOURCE_CLASS=N`). The six arenas in this table
(`ASX_MAX_REGIONS`, `ASX_MAX_TASKS`, `ASX_MAX_TIMERS`, `ASX_MAX_OBLIGATIONS`,
`ASX_MAX_CHANNELS`, `ASX_TRACE_CAPACITY`) are then sized from that class: the
class limit is both the compiled capacity and the point where allocation
fails with `ASX_E_RESOURCE_EXHAUSTED`. Other capacities keep their defaults,
and an explicit `-DASX_MAX_*` still overrides the class. The class is a build
choice, independent of `PROFILE`. Without a class, the arenas keep the
unclassed defaults listed in the capacity table below (`ASX_MAX_REGIONS`,
`ASX_MAX_TASKS`, ...). `make test-resource-classes` runs the unit suite built
for each class.

### Capacity Macros

Every arena and pool capacity (the macros named `ASX_MAX_*` or
`ASX_*_CAPACITY`) can be overridden at build time, e.g.
`make CFLAGS=-DASX_MAX_TCP_STREAMS=64u` (a value below a macro's minimum
fails the build with `#error`). Per-module size limits with other names,
such as `ASX_HTTP_SERVER_MAX_CONNS`, `ASX_HTTP_MAX_HEADERS` or
`ASX_WS_MAX_MESSAGE`, are `#ifndef`-guarded in their headers too, but are
not listed or bound-checked here yet. The defaults below are a CORE build's,
without a resource class. `ASX_MAX_WORKERS` depends on the profile (1 for
FREESTANDING and BROWSER, 4 for EMBEDDED_ROUTER, 16 for HFT and AUTOMOTIVE,
else 64).
`make test-capacity-x4` runs the unit suite with each of these raised 4x.
`make test-stress` runs it under ASan+UBSan with room for 1024 tasks. The crowd tests
scale with `ASX_MAX_TASKS`, so in that build 1000 producers share a capacity-8 channel,
about 30% of them cancelled, and about 1000 waiters line up on a mutex and on a notify.
The table
is generated: `make capacity-table` prints it, and `make lint-docs` fails if
it drifts from the headers or a new capacity macro is missing.

<!-- capacity-table:begin -->
| Macro | Default | Defined in |
|---|---|---|
| `ASX_ACTOR_MAILBOX_CAPACITY` | 64 | `include/asx/actor/actor.h` |
| `ASX_AFFINITY_TABLE_CAPACITY` | 256 | `include/asx/core/affinity.h` |
| `ASX_BROADCAST_MAX_CAPACITY` | 32 | `include/asx/core/broadcast.h` |
| `ASX_BUF_CAPACITY` | 4096 | `include/asx/bytes/buf.h` |
| `ASX_CHANNEL_MAX_CAPACITY` | 64 | `include/asx/core/channel.h` |
| `ASX_CLEANUP_STACK_CAPACITY` | 32 | `include/asx/core/cleanup.h` |
| `ASX_EVIDENCE_SINK_CAPACITY` | 64 | `include/asx/runtime/diagnostic.h` |
| `ASX_FS_FILE_CAPACITY` | 1024 | `include/asx/fs/fs.h` |
| `ASX_GHOST_BORROW_TABLE_CAPACITY` | 128 | `include/asx/core/ghost.h` |
| `ASX_GHOST_DETERMINISM_CAPACITY` | 256 | `include/asx/core/ghost.h` |
| `ASX_GHOST_LINEARITY_CAPACITY` | 256 | `src/core/ghost.c` |
| `ASX_GHOST_RING_CAPACITY` | 64 | `include/asx/core/ghost.h` |
| `ASX_HINDSIGHT_CAPACITY` | 256 | `include/asx/runtime/hindsight.h` |
| `ASX_IDEMPOTENCY_STORE_CAPACITY` | 16 | `include/asx/remote/remote.h` |
| `ASX_LANE_TASK_CAPACITY` | 64 | `include/asx/runtime/parallel.h` |
| `ASX_MAX_ACTORS` | 16 | `include/asx/actor/actor.h` |
| `ASX_MAX_BLOCKING_TASKS` | 16 | `include/asx/runtime/blocking.h` |
| `ASX_MAX_BROADCASTS` | 8 | `include/asx/core/broadcast.h` |
| `ASX_MAX_CANCEL_WITNESSES` | 64 | `src/core/cancel.c` |
| `ASX_MAX_CHANNELS` | 16 | `include/asx/core/channel.h` |
| `ASX_MAX_DEADLINE_MONITORS` | 32 | `include/asx/runtime/deadline_monitor.h` |
| `ASX_MAX_EPOCHS` | 16 | `include/asx/core/epoch.h` |
| `ASX_MAX_EPOCH_OBSERVERS` | 8 | `include/asx/core/epoch.h` |
| `ASX_MAX_FS_ENTRIES` | 16 | `include/asx/fs/fs.h` |
| `ASX_MAX_IO_TOKENS` | 64 | `include/asx/runtime/io_driver.h` |
| `ASX_MAX_MASK_DEPTH` | 64 | `include/asx/runtime/runtime.h` |
| `ASX_MAX_OBLIGATIONS` | 128 | `include/asx/runtime/runtime.h` |
| `ASX_MAX_ONESHOTS` | 32 | `include/asx/core/oneshot.h` |
| `ASX_MAX_OPEN_DIRS` | 8 | `include/asx/fs/fs.h` |
| `ASX_MAX_OPEN_FILES` | 16 | `include/asx/fs/fs.h` |
| `ASX_MAX_PIPES` | 8 | `include/asx/net/pipe.h` |
| `ASX_MAX_PROCESSES` | 8 | `include/asx/process/process.h` |
| `ASX_MAX_QUIC_CONNECTIONS` | 4 | `include/asx/net/quic.h` |
| `ASX_MAX_QUIC_STREAMS` | 16 | `include/asx/net/quic.h` |
| `ASX_MAX_REGIONS` | 8 | `include/asx/runtime/runtime.h` |
| `ASX_MAX_REGION_CHILDREN` | 8 | `include/asx/asx_config.h` |
| `ASX_MAX_SESSIONS` | 8 | `include/asx/core/session.h` |
| `ASX_MAX_SIGNAL_SUBSCRIPTIONS` | 16 | `include/asx/signal/signal.h` |
| `ASX_MAX_SUPERVISORS` | 8 | `include/asx/actor/supervisor.h` |
| `ASX_MAX_SUSPECTS` | 4 | `include/asx/runtime/regression_localize.h` |
| `ASX_MAX_TASKS` | 64 | `include/asx/runtime/runtime.h` |
| `ASX_MAX_TCP_LISTENERS` | 4 | `include/asx/net/net.h` |
| `ASX_MAX_TCP_STREAMS` | 16 | `include/asx/net/net.h` |
| `ASX_MAX_TIMERS` | 128 | `include/asx/time/timer_wheel.h` |
| `ASX_MAX_TLS_STREAMS` | 8 | `include/asx/net/tls.h` |
| `ASX_MAX_UDP_SOCKETS` | 16 | `include/asx/net/net.h` |
| `ASX_MAX_UNIX_DGRAM_SOCKETS` | 8 | `include/asx/net/net.h` |
| `ASX_MAX_UNIX_LISTENERS` | 4 | `include/asx/net/net.h` |
| `ASX_MAX_UNIX_STREAMS` | 16 | `include/asx/net/net.h` |
| `ASX_MAX_WAKERS` | 64 | `include/asx/runtime/waker.h` |
| `ASX_MAX_WATCHES` | 16 | `include/asx/core/watch.h` |
| `ASX_MAX_WORKERS` | 64 | `include/asx/runtime/parallel.h` |
| `ASX_MAX_WS_CONNECTIONS` | 8 | `include/asx/net/websocket.h` |
| `ASX_POSIX_BLOCKING_QUEUE_CAPACITY` | 8 | `src/platform/posix/hooks.c` |
| `ASX_POSIX_BLOCKING_WORKERS` | 4 | `src/platform/posix/hooks.c` |
| `ASX_RESOLVER_CACHE_CAPACITY` | 8 | `include/asx/net/net.h` |
| `ASX_RESOLVER_HOST_CAPACITY` | 256 | `include/asx/net/net.h` |
| `ASX_SCHED_EVENT_LOG_CAPACITY` | 256 | `src/runtime/scheduler.c` |
| `ASX_SERVICE_BUFFER_CAPACITY` | 8 | `include/asx/service/service.h` |
| `ASX_SESSION_MAX_CAPACITY` | 16 | `include/asx/core/session.h` |
| `ASX_SYMBOL_REGISTRY_CAPACITY` | 256 | `include/asx/core/symbol.h` |
| `ASX_TRACE_CAPACITY` | 1024 | `include/asx/runtime/trace.h` |
| `ASX_WAIT_NODE_CAPACITY` | 256 | `src/sync/wait_queue.h` |
| `ASX_WS_TX_CAPACITY` | 16896 | `include/asx/net/websocket.h` |
<!-- capacity-table:end -->

## Deployment Profiles

| Profile | Target | Key Properties |
|---|---|---|
| `ASX_PROFILE_CORE` | General-purpose (default) | Deterministic single-thread kernel, no OS assumptions |
| `ASX_PROFILE_POSIX` | Linux (CI-tested); other POSIX systems through the poll(2) fallback, untested | Live builds (`DETERMINISTIC=0`): epoll/poll(2) readiness reactor, pthread blocking pool, native sockets, files, processes and signals. Deterministic builds keep the in-memory backends |
| `ASX_PROFILE_WIN32` | Windows (MSVC) | QueryPerformanceCounter clock, BCrypt entropy in live builds, debugger/stderr logging. No native I/O yet: no sockets or IOCP (the reactor is a timed sleep that reports no readiness), no blocking pool, and net/fs run on the in-memory backends |
| `ASX_PROFILE_FREESTANDING` | Bare-metal/embedded | User-supplied hooks, no FS/network assumptions |
| `ASX_PROFILE_EMBEDDED_ROUTER` | OpenWrt/router-class | 4-worker cap, reject-at-75% overload policy. Arena sizes come from `RESOURCE_CLASS`, not the profile. No native OS I/O. CI runs its unit suite and the Rust fixtures under QEMU on five ISAs |
| `ASX_PROFILE_HFT` | High-frequency trading | 16-worker cap, shed-oldest overload policy; pairs with the HFT instrumentation module (latency histograms, jitter), which the application feeds |
| `ASX_PROFILE_AUTOMOTIVE` | Safety-critical systems | 16-worker cap, backpressure overload policy; pairs with the automotive instruments (deadline tracking, watchdog, degraded-mode audit, compliance gate), which the application feeds |
| `ASX_PROFILE_PARALLEL` | Multi-core (planned) | Deterministic logical worker-lane scheduler (64-worker generic cap) that runs every lane on the calling thread; observe-only admission by default |
| `ASX_PROFILE_BROWSER` | Browser/WebAssembly (no wasm32 build yet) | Native-only surfaces compiled out, browser boundary diagnostics; builds and runs natively today |

Profiles are meant to control operational envelopes (limits, defaults, instrumentation), never semantic behavior. That is checked by executing the Rust-captured fixtures under CORE and EMBEDDED_ROUTER and by the resource-pressure gate; `make profile-parity` compares recorded digests only.

## Architecture

```text
                       ┌──────────────────────────────────────────┐
                       │                Inputs                    │
                       │  scenarios | API calls | trace files     │
                       └──────────────────────────────────────────┘
                                          │
                                          ▼
┌───────────────────────────────────────────────────────────────────────┐
│                      asx_core                                         │
│  IDs + generation counters | outcomes | budgets | cancel + witness    │
│  combinators | symbols + typed values | epochs | circuit breakers     │
│  ghost monitors | error taxonomy | codec schema                       │
└───────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
┌───────────────────────────────────────────────────────────────────────┐
│                   asx_runtime_kernel                                  │
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
         │ evidence artifacts  │     │  HFT | automotive | Win32 │
         └──────────────────────┘     └──────────────────────────┘
```

## Repository Layout

```text
include/asx/                 Public C headers
  asx.h                      Umbrella header (single #include entry point)
  asx_status.h               Error codes with categories and recovery guidance
  asx_config.h               Profile, resource class, hook, and fault injection types
  asx_ids.h                  Handle types, type tags, lifecycle enums, cancel kinds
  core/                      Symbols, budgets, cancel, channels, combinators, epochs, circuit breakers
  runtime/                   Scheduler, builder, blocking, I/O, deadline, HFT, automotive
  sync/                      Sync primitives (mutex, rwlock, semaphore, barrier, once, notify)
  codec/                     Codec abstraction + equivalence checking
  ...                        + actor, cx, time, bytes, stream, security, net, fs, evidence, monitor, etc.

src/                         C sources, one directory per subsystem
  core/                      Status, cancel, combinators, symbols, epochs, circuit breakers
  runtime/                   Scheduler, lifecycle, builder, blocking, I/O, deadline, instruments
  channel/                   MPSC, oneshot, broadcast, watch, session
  platform/                  POSIX (hooks, net, fs, process, signal), Win32, freestanding adapters
  ...                        + sync, actor, time, bytes, cx, codec, security, net, fs, process, etc.

tests/                       C test programs across the verification lanes
  unit/                      Module unit tests, by subsystem
  e2e/                       End-to-end scenario programs (+ shell harnesses)
  invariant/                 Lifecycle/quiescence invariant suites
  vignettes/                 API ergonomics demonstrations
  conformance/               Rust parity + codec/profile equivalence suites
  fuzz/                      Differential fuzzing harnesses
  formal/                    Algebraic, CBMC-compatible (run natively), and litmus checks
  ...                        + ABI, bench, and embedded support programs under tests/abi, tests/bench, tests/embedded

examples/                    Example programs
fixtures/rust_reference_v2/  Fixtures captured from the Rust runtime (tools/twin_run), replayed by make conformance
tools/                       CI, capture, replay, fuzz, and minimization tooling
docs/                        Port architecture, parity tracking, deployment hardening
```

The tree holds <!-- fact:header_files -->127<!-- /fact --> public headers
(<!-- fact:headers_core -->22<!-- /fact --> in `core/`,
<!-- fact:headers_runtime -->29<!-- /fact --> in `runtime/`), <!-- fact:source_files -->135<!-- /fact -->
C sources (<!-- fact:sources_core -->19<!-- /fact --> in `core/`,
<!-- fact:sources_runtime -->39<!-- /fact --> in `runtime/`,
<!-- fact:sources_platform -->7<!-- /fact --> in `platform/`),
<!-- fact:test_programs -->225<!-- /fact --> C test programs and
<!-- fact:examples -->14<!-- /fact --> examples (`make lint-docs` checks these numbers
against `tools/count_inventory.sh`).

## Performance and Footprint

Design properties (these describe the code; none of them is a benchmark result):

- the default scheduler sweeps the task arena in index order each round, so a round costs O(arena size); that order is what makes it deterministic,
- O(1) timer cancel via generation-safe handles,
- no heap allocation in the kernel (regions, tasks, obligations, timers, channels, scheduler): its arenas are static and sized at build time. The fixture codec (`asx_codec_*`) is the exception; it grows its buffers with the C library's `malloc`/`realloc`,
- deterministic digest stability across repeated runs with identical seed/input (deterministic builds),
- deterministic exhaustion behavior at each resource class's limits.

Library size and RAM use are not published as measurements here. The perf
workflow (`.github/workflows/perf.yml`, pushes to `main`) checks the CORE
release `libasx.a` against the size budgets in
`tools/ci/size_cold_start_baselines.json`.

Recommended benchmark flow:

```bash
make bench
make bench-json
make parallel-parity
make test-e2e-parallel
rch exec -- make parallel-bench-json
rch exec -- make parallel-bench-gate
rch exec -- make wave-c-acceptance-demo
make bench PROFILE=EMBEDDED_ROUTER
make profile-parity
```

## Testing and Quality Gates

`asx` currently ships with <!-- fact:test_programs -->225<!-- /fact --> tracked C test programs across 7 categories in
the checked-in `tests/` tree. Individual assertion counts evolve over time; see
`tests/TEST.md` for the current indexed suite inventory and per-suite case
totals where tracked.

| Category | Files | Coverage |
|---|---|---|
| **Unit tests** | <!-- fact:test_unit -->164<!-- /fact --> | Broad public API and subsystem coverage across the current tree |
| **End-to-end scenarios** | <!-- fact:test_e2e -->23<!-- /fact --> | Core lifecycle, automotive, HFT, codec parity, continuity, network surfaces, POSIX adapter, HTTP server on real sockets (browser and CLI lanes are shell families) |
| **Invariant tests** | <!-- fact:test_invariant -->3<!-- /fact --> | Lifecycle transition legality, quiescence, obligation linearity |
| **API vignettes** | <!-- fact:test_vignettes -->12<!-- /fact --> | Ergonomics and usage pattern demonstrations |
| **Conformance** | <!-- fact:test_conformance -->6<!-- /fact --> | Codec round-trip equivalence, profile-variant fixture equivalence, parallel worker-count parity, API misuse, resource-pressure failure atomicity (the Rust parity runner is `tools/conformance/runner.c`) |
| **Fuzz** | <!-- fact:test_fuzz -->4<!-- /fact --> | C fuzz harness, minimizer, grammar vector generators (Rust-vs-C fuzzing is `make fuzz-differential`, driven by `tools/twin_run`) |
| **Formal verification** | <!-- fact:test_formal -->13<!-- /fact --> | Algebraic property suites, CBMC-compatible transition harnesses (run natively, not under CBMC), litmus tests |

Gate commands (the CI table below says which jobs run which):

```bash
make test               # Default local suites: unit + invariant + conformance-c + vignette
make test-unit          # Current unit test suite set in tests/unit/
make test-invariants    # Lifecycle and quiescence invariants
make test-conformance-c # The tests/conformance/*_test.c programs (codec, profile, parallel parity, misuse, pressure)
make test-vignettes     # API ergonomics demonstrations
make test-e2e           # Every e2e family (tests/e2e/run_all.sh)
make fixture-integrity  # Fixture schema, provenance, digest recompute, codec round trip
make test-gates         # Gate negative controls (bad fixtures must be rejected)
make conformance        # Rust parity: every v2 fixture executed in C and compared
make fuzz-differential  # Seeded Rust-vs-C differential fuzzing (needs Rust)
make codec-equivalence  # JSON vs BIN digests recorded in fixtures (runtime not executed)
make profile-parity     # Cross-profile digests recorded in fixtures (runtime not executed)
make fuzz-smoke         # C fuzz harness smoke (Rust comparison only with RUST_FUZZ_BINARY)
make formal-check       # Transition harnesses + algebraic + litmus (native; no CBMC run)
make ci-embedded-matrix # Cross-target embedded builds (size/layout rows; not run)
make test-unit TARGET=mips-linux-gnu PROFILE=EMBEDDED_ROUTER BUILD_DIR=build/qemu-mips \
  LDFLAGS=-static TEST_EXEC=qemu-mips-static  # Unit suite under QEMU (also: make conformance ...)
make check              # Local gate: format, lint gates, build, make test, model check, ABI, formal
make check-ci           # Wider local approximation of CI (see "Local vs CI" below)
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

# Binary package install/quickstart smoke
make release-install-smoke RELEASE_INSTALL_SMOKE_VERSION=0.1.0 RELEASE_INSTALL_SMOKE_TARGET=linux-x86_64

# Source package
make release-artifacts RELEASE_VERSION=0.1.0 RELEASE_TARGET=source RELEASE_KIND=source
```

Notes:
- `ASX_ENABLE_SIGSTORE=1` enables keyless Sigstore bundle generation when `cosign` is available.
- `ASX_USE_RCH=auto` keeps local release builds compatible with remote build offload.
- Root `rust-toolchain.toml` pins `nightly-2026-08-31`, the fleet-wide nightly that asupersync also pins, so `rch` does not infer a machine-specific nightly snapshot for C-only `make` lanes.
- Operational release/rollback checklist: `docs/DEPLOYMENT_HARDENING.md` ("Release Verification and Rollback Runbook").

## How It Works: Internal Design

This section covers the algorithms, data structures, and design patterns behind the API surface. None of this is required to use the library, but it helps when reasoning about performance characteristics and failure modes.

### Generation-Safe Handles

Regions, tasks, obligations and channels are 64-bit opaque handles with a packed layout:

```
[16-bit type_tag | 16-bit state_mask | 16-bit generation | 16-bit slot_index]
```

Timers use a `{slot, generation}` struct handle instead, and cancel witnesses are numbered from a counter.

When a slot is recycled, its generation counter increments (channel, timer and witness counters skip zero to avoid sentinel collision). Old handles still carry the old generation, so a stale handle fails in O(1): `ASX_E_STALE_HANDLE` if the slot has been reused, `ASX_E_NOT_FOUND` if it is free. The check is a single `uint16_t` comparison, with no hash table lookups or reference counting.

Each handle also carries a 16-bit state mask recording the entity's state when the handle was issued (`asx_handle_state_allowed`). It is not updated afterwards; the runtime checks state from the arena slot.

Region, task, obligation and channel lookups check the type tag, so type confusion across those entity families is caught at the handle layer, not deep inside business logic.

### Wake-Driven Deterministic Scheduler

`asx_scheduler_run(region, budget)` drives a whole region **subtree**. Runnable tasks are polled in ascending arena index within each round, so the same tasks and seed produce identical event sequences across runs, platforms, and profiles.

`asx_scheduler_use_lab_dispatch(seed)` switches to the dispatch model of Rust asupersync's `LabRuntime` (one worker): each step draws one value from a seeded xorshift64 and polls a single task. The task comes from the cancel lane (up to 16 in a row) before the ready lane, at the highest priority, the step's value picking among equal-priority entries in wake order. Due timers wake their tasks in timer-wheel order. Each task also gets Rust's per-task entropy stream (a DetEntropy fork keyed by the task's id in a shadow of Rust's task arena), from which a race picks its winner among members ready in the same round, as Rust's `race_all` does. The conformance oracle runs every scenario this way, and with the same scenario and seed the C and Rust runs dispatch identically (`make conformance`, `make fuzz-differential`).

Tasks are **wake-driven**, not busy-polled. A poll function that must wait calls `asx_task_park(self)` (directly, or through a primitive that does it for it: sleep, join, channel receive, mutex, socket read, actor mailbox, ...) and returns `ASX_E_PENDING`. The task is not polled again until something wakes it: `asx_task_wake`, a waker signal, a task timer, I/O readiness, a joined task completing, or cancellation. A task that returns `ASX_E_PENDING` without parking simply yields.

Each round:
1. Polls every live, non-parked task in the subtree in index order. Deadline and poll-quota budgets are enforced before each poll, consuming one poll unit per call.
2. Completes tasks that return a final status, recording the outcome by severity: PANICKED (`asx_task_panic`) > CANCELLED (a pending cancel dominates) > ERR > OK. A cancelled task's error is its acknowledgement and is not a fault.
3. Never force-completes a cancelled task (as in Rust): a spent cleanup budget only strengthens its cancel reason to `POLL_QUOTA`. The opt-in `asx_runtime_config.cleanup_hard_bound` force-completes tasks whose cleanup polls are spent (masked tasks exempt), a deviation from Rust for deployments that need a hard bound.
4. When nothing is runnable it **idles** instead of spinning: unpark cancelled tasks not yet polled since their cancel, drain wakers, fire due timers (an EDF heap keyed by `(deadline, sequence)`), and otherwise block in the reactor until I/O, a cross-thread wake, or the next timer. In deterministic builds the virtual clock jumps straight to the next timer. When nothing can ever wake the parked tasks, it returns `ASX_E_WOULD_BLOCK`.
5. Returns `ASX_OK` at quiescence, `ASX_E_POLL_BUDGET_EXHAUSTED` when the caller's poll budget runs out, or `ASX_E_TIMED_OUT` when its deadline passes.

Tasks in the FINALIZING phase complete without consuming poll units, ensuring cleanup can finish even under tight budgets. Tasks can be joined (`asx_task_join_poll` parks until the target completes), watched (`asx_task_watch`, monitor semantics), or detached. Arena slots are recycled with generation bumps, so long-running servers do not exhaust the task, region, or obligation arenas.

### Budget Algebra

Budgets are composable resource constraints with four dimensions: **deadline** (absolute nanosecond timestamp), **poll quota** (max poll calls), **cost quota** (abstract cost units), and **priority** (scheduling weight).

The key operation is the **meet** (componentwise tightening):

```
meet(a, b).deadline   = min(a.deadline,   b.deadline)     // 0 = unconstrained
meet(a, b).poll_quota = min(a.poll_quota, b.poll_quota)
meet(a, b).cost_quota = min(a.cost_quota, b.cost_quota)   // UINT64_MAX = unconstrained
meet(a, b).priority   = max(a.priority,   b.priority)     // most urgent wins
```

As in Rust asupersync's `Budget::combine`, priority meets upward: `asx_budget_infinite()` has priority 0 (the identity), `asx_budget_zero()` has 255 (absorbing), and ordinary budgets (`asx_budget_new()`, `asx_budget_from_polls()`) default to 128. The default sweep scheduler does not yet order tasks by budget priority (tracked as bridge bead S5); lab dispatch does.

This means budgets compose correctly: if a task has a 100-poll budget and its region has a 50-poll budget, the effective budget is 50. Cleanup budgets for cancellation follow the same rule: each cancel request's cleanup budget is met into the task's (severity 5, SHUTDOWN, gets 50 cleanup polls; severity 0, USER, gets 1,000). When the task acknowledges its cancel at a checkpoint, the cleanup budget replaces its budget (no deadline or cost quota, the cleanup priority), as Rust's `acknowledge_cancel` does; a later request during cleanup replaces it again with the met budget. Read it with `asx_task_get_cleanup_budget`.

Budgets are **enforced**, not advisory. Regions carry a budget that child regions (`asx_region_open_child_with_budget`) and tasks (`asx_task_spawn_with_budget`) inherit through the meet, so a child can only tighten it:

| Dimension | Enforcement | Cancel kind |
|---|---|---|
| deadline | checked before every poll and at every checkpoint; a parked task arms its deadline as a timer, so it wakes up in order to be cancelled | `DEADLINE` |
| poll quota | decremented per poll; at zero the task is cancelled | `POLL_QUOTA` |
| cost quota | `asx_task_consume_cost(self, n)` charges abstract cost; an unaffordable charge cancels the task and returns `ASX_E_COST_QUOTA_EXHAUSTED` | `COST_BUDGET` |

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

Cancellation strength can only increase, never decrease. If a task already has a pending `TIMEOUT` cancel (severity 1) and receives a `PARENT` cancel (severity 4), the stronger cancel wins. This monotonicity guarantee means cancel waves never weaken; the cleanup budget is the meet of every request's.

The cancellation state machine progresses through four phases:

```
Running → CancelRequested → Cancelling → Finalizing → Completed
```

Each cancel carries an **origin attribution chain** (source region, source task, timestamp, message) so propagation can be traced across cancel waves. The chain is bounded to prevent unbounded allocation.

Cancelling a region (`asx_cancel_propagate`, `asx_region_drain`) reaches its whole subtree: the root's tasks get the requested kind, descendants get `PARENT`. Draining closes the subtree parent-first, cancels and runs it to completion, then finalizes it deepest-first. A parked task is woken by its cancel.

**Masking** (`asx_task_mask` / `asx_task_unmask`, nestable to `ASX_MAX_MASK_DEPTH` = 64) defers acknowledgement for a critical section, such as committing a transaction or a finalizer. While a task is masked, `asx_checkpoint` reports `masked = 1` instead of `cancelled`, the cancel can still strengthen, and even the opt-in hard cleanup bound neither consumes cleanup polls nor force-completes the task. The cancel becomes observable at the first checkpoint after the depth returns to zero.

### Obligations: Holders and Leak Policy

An obligation (send permit, ack, lease, I/O op, semaphore permit, transaction) is a linear resource that must be committed or aborted exactly once. Each records its **kind** and its **holder task**: `asx_obligation_reserve` binds the task being polled, and `asx_obligation_reserve_ex` takes an explicit kind and holder. When a holder completes with obligations still reserved, the runtime resolves them deterministically:

- They are **leaks**, whether or not the holder was cancelled: a Rust task body that ends holding an unresolved obligation token drops it, and the drop posts a leak. They are handled by the configured `leak_response`:
  - `LOG` (the default) warns.
  - `SILENT` records them.
  - `RECOVER` aborts them with reason `ERROR`, as Rust's Recover does; they still count as leaks.
  - `PANIC` routes the leak through region fault containment, so FAIL_FAST surfaces `ASX_E_UNRESOLVED_OBLIGATIONS` from the scheduler.
- `leak_escalation` switches to a stricter response once the leak count reaches a threshold. As in Rust, the leak that reaches it is the first one escalated.

`asx_obligation_get_info` and `asx_obligation_leak_count` expose the results.

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

Ghost monitors are compile-time gated behind `ASX_DEBUG_GHOST`, with zero overhead in release builds. They provide four classes of runtime verification:

**Protocol Monitor**: Validates every region, task, and obligation state transition against precomputed transition tables. Invalid transitions (e.g., COMPLETED → RUNNING) are caught immediately and recorded in a ring buffer with entity ID, from-state, to-state, and sequence number. This catches state machine corruption that would otherwise manifest as subtle downstream bugs.

**Linearity Monitor**: Tracks obligation reserve/commit/abort lifecycle and detects double-resolution (calling both commit and abort on the same obligation) and leaks (reserved but never resolved). Each obligation gets exactly one resolution in a valid program; the linearity monitor enforces this at runtime in debug builds.

**Borrow Ledger**: Emulates Rust's borrow checker rules in C. Tracks shared and exclusive borrows per entity and enforces "multiple shared XOR one exclusive, never both." A borrow table of 128 entries records active borrows. `asx_ghost_borrow_shared()` checks for conflicting exclusive borrows; `asx_ghost_borrow_exclusive()` checks for any existing borrow. Violations are recorded with the conflicting entity and borrow type.

**Determinism Monitor**: Records scheduler event keys and flags ordering drift between runs (`asx_ghost_determinism_*`).

### Error Taxonomy and Recovery Guidance

Every error code carries structured metadata beyond the numeric value:

- **Category** (17 families): general, transition, region, task, obligation, cancellation, channel, timer, quiescence, resource, handle, hook, affinity, equivalence, replay, config, permission.
- **Recoverability**: `NONE` (not an error: `ASX_OK`, `ASX_E_PENDING`), `TRANSIENT` (retry may help), `PERMANENT` (logic error), `CONTEXT_DEPENDENT` (depends on usage).
- **Recovery action**: `RETRY_IMMEDIATELY`, `RETRY_WITH_BACKOFF`, `REINITIALIZE_CONTEXT`, `PROPAGATE`, `ESCALATE`, `INSPECT_CONFIGURATION`.
- **Backoff hints**: For transient errors, each code specifies initial delay, maximum delay, and maximum retry attempts.

The error ledger maintains a per-task ring buffer (16 entries deep, 64 task slots) that records recent errors with source location (`file:line`), operation name, and monotonic sequence number. This means when a task fails, you can inspect its recent error history without external logging infrastructure.

The `ASX_TRY(expr)` macro provides Rust-style `?` operator behavior in C: it evaluates `expr`, and if the result differs from `ASX_OK`, returns it immediately from the enclosing function.

### Trace, Replay, and Semantic Digests

Every scheduler round, lifecycle transition, and significant event is recorded in a bounded event journal (ring buffer, default 1,024 entries). Each event is a tuple of `(sequence, kind, entity_id, aux)`; the trace keeps a 64-bit emit counter, and each event stores its low 32 bits as `sequence`.

The **trace digest** (`src/runtime/trace.c`) uses FNV-1a mixing (64-bit). Every emitted event is folded in when it is emitted, so the digest covers events the ring has already dropped:

```
hash = 0x517cc1b727220a95   // the asx digest seed, not the FNV-1a offset basis
prime = 0x00000100000001B3
for each emitted event, for each byte of
        sequence (u32 LE), kind (u32 LE), entity_id (u64 LE), aux (u64 LE):
    hash ^= byte
    hash *= prime
```

Two deterministic runs with the same scenario, profile, seed, and runtime version must produce identical trace digests; this is the C runtime's self-consistency check. The parity gates use other digests:
- Rust parity (`make conformance`) compares the canonical vocabulary-v2 trace, snapshot and observations of each scenario exactly (`src/conformance/`, SHA-256 over canonical JSON for digests).
- `make codec-equivalence` and `make profile-parity` compare the SHA-256 `semantic_digest` values recorded in the fixture files, without running the runtime.

A Rust parity mismatch is a semantic drift bug in the C port, not an acceptable variation.

### Combinator Composition Model

The poll-based combinators (join, race, select, timeout, first_ok, quorum, race_timeout, retry, retry_timeout, bracket, pipeline, hedge, map_reduce) share the `asx_combinator_poll_fn` interface: each is a poll state machine that returns `ASX_E_PENDING` until terminal, then a final status. This uniformity means combinators compose naturally: a retry combinator can wrap a race combinator, which itself contains timeout-wrapped branches. Bulkhead and rate-limit are non-blocking admission counters (`asx_bulkhead_try_enter`, `asx_rate_limit_try_acquire`), not poll functions.

The **loser-drain protocol** is central to race/select/quorum semantics. When a winner is decided:
1. Each undecided branch receives exactly one final cooperative poll (cleanup opportunity).
2. The branch is then resolved as `ASX_E_CANCELLED`, whatever the final poll returned.
3. The combinator then returns the winner's result.

This bounded drain prevents indefinite hangs on losing branches while giving them a chance to release resources. The drain is deterministic: same input, same drain order, same result.

**Task groups** (`asx/runtime/task_group.h`) provide the full "losers are drained" guarantee over real spawned tasks, porting asupersync's `Scope::race_all` / `join_all` / `first_ok` / `quorum`. The owner drives the group with `asx_task_group_poll(&group, self)`; it parks, and members wake it as they complete. Once a race or quorum is decided, every unfinished member is cancelled with `RACE_LOST` and **awaited to completion** before the group resolves, so a loser's obligations, finalizers, and handles are resolved, never abandoned. Per mode, as in Rust:
- `join_all` awaits its members one by one, in order, and the owner's cancel does not reach them (joins are uninterruptible).
- `first_ok` runs registered attempts (`asx_task_group_add_attempt`) one at a time, in order, until one succeeds; the owner's cancel is passed to the running attempt and stops further attempts.
- `quorum` reports a member's panic even when met, and maps the rest of Rust's `QuorumError` cases onto statuses (see the header).

Ending early drains members the same way:

| Trigger | Members cancelled with | Group result |
|---|---|---|
| owner cancelled | the owner's own reason for race, quorum and first_ok; not at all for join_all | `ASX_E_CANCELLED` (race: or the status of the first member that panicked) |
| group deadline | `TIMEOUT` | `ASX_E_TIMED_OUT` (`ASX_E_THRESHOLD_TIMEOUT` for quorum) |
| `asx_task_group_cancel` | the requested kind | `ASX_E_CANCELLED` |

Members of a race completing in the same round are tie-broken by lowest index, so results are deterministic. Rust breaks that tie with the owner's entropy instead, an open divergence (bd-g652).

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
- **Barrier**: The epoch advances when the number of `asx_epoch_arrive()` calls reaches the number of registered observers (arrivals are counted, not matched to observers). Models N-way synchronization points.
- **Threshold**: The epoch advances when N arrivals occur (configurable threshold). Models quorum-based progression.

Each epoch tracks a monotonic phase counter, an arrival count that resets on advance, and up to 8 observer callbacks notified synchronously on phase transitions. An optional `max_phases` limit prevents unbounded phase growth.

### Adaptive Decision Engine

The adaptive subsystem provides expected-loss minimization for decisions a caller defines (e.g., "should we shed load or accept backpressure?"); the runtime itself routes no decision through it. For each candidate action, it computes:

```
E[Loss(action)] = sum over states: Loss(action, state) * P(state | evidence)
```

Loss values use 16.16 fixed-point arithmetic (no floating point). Posterior probabilities are 0.32 fixed-point. The engine selects the action with minimum expected loss, with fallback to a safe default when confidence is below a configurable threshold or the decision budget is exhausted.

An evidence ledger (ring buffer, 64 entries) records each decision with its selected action, the next-best (counterfactual) action, and contributing evidence terms, so you can reconstruct why a particular path was chosen after the fact.

### Profile Surface Gating

Each deployment profile declares which runtime surfaces are available. The browser profile, for example, gates out filesystem, process, signal, I/O driver, and blocking pool surfaces (and server, gRPC, messaging, TLS and database).

Surface gating works at two levels:
1. **Compile-time**: in a build with `ASX_PROFILE_BROWSER` the native-only surfaces are compiled out, so `asx_spawn_blocking()` is not declared at all and a call is a compile or link error.
2. **Runtime**: `asx_surface_available(profile, surface)` and `asx_surface_gate(surface)` report availability from the profile's table; `asx_surface_gate` returns `ASX_E_PERMISSION_DENIED` for a gated surface.

### Zero-Allocation Arena Design

The kernel subsystems use fixed-size static arenas instead of dynamic allocation (the fixture codec is the exception, see above). The sizes are build-time macros; the capacity table above lists the defaults of all but the error ledger's (16 entries for each of 64 task slots):

| Subsystem | Capacity macro | Slot Type |
|---|---|---|
| Regions | `ASX_MAX_REGIONS` | Region lifecycle state |
| Tasks | `ASX_MAX_TASKS` | Task state + poll function |
| Obligations | `ASX_MAX_OBLIGATIONS` | Reserve/commit/abort state |
| Timers | `ASX_MAX_TIMERS` | Deadline + waker + generation |
| Channels (MPSC) | `ASX_MAX_CHANNELS` | Ring buffer + permits |
| Blocking pool | `ASX_MAX_BLOCKING_TASKS` | Function + result + waker |
| I/O registrations | `ASX_MAX_IO_TOKENS` | fd + interest mask + waker |
| Deadline monitors | `ASX_MAX_DEADLINE_MONITORS` | Target + entity + callback |
| Epochs | `ASX_MAX_EPOCHS` | Policy + phase + observers |
| Ghost violations | `ASX_GHOST_RING_CAPACITY` | Ring buffer |
| Error ledger | `ASX_ERROR_LEDGER_DEPTH` x `ASX_ERROR_LEDGER_TASK_SLOTS` | Ring buffer per task |
| Trace events | `ASX_TRACE_CAPACITY` | Ring buffer |

Slots are reused via generation counters, never freed and reallocated. This design:
- eliminates malloc/free overhead and fragmentation,
- enables deterministic memory footprint (known at compile time),
- suits `ASX_PROFILE_FREESTANDING` targets with no system allocator (the mandatory allocator hook can be backed by a caller-supplied buffer, `asx_static_arena_init` / `asx_static_arena_bind_hooks`),
- allows the allocator to be sealed after initialization (`asx_runtime_seal_allocator()`).

Resource classes (R1/R2/R3, `make RESOURCE_CLASS=1|2|3`) resize the region, task, timer, obligation, channel and trace arenas without changing semantic behavior; the other sizes keep their defaults and can be overridden one macro at a time.

Per region, `asx_region_set_limits` caps live tasks, live child regions and pending obligations, as Rust's `RegionLimits` does: an admission past a cap is refused with `ASX_E_ADMISSION_LIMIT` and changes nothing (a new region is unlimited, `ASX_REGION_UNLIMITED`).

## Stackless Coroutines via Protothread Macros

`asx` implements cooperative multitasking without threads, fibers, or `setjmp`/`longjmp`. Instead, tasks use protothread macros that compile to a switch-case state machine (simplified from `include/asx/runtime/runtime.h`):

<!-- readme-c: excerpt, not compiled (simplified from include/asx/runtime/runtime.h) -->
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
#include <asx/asx.h>

typedef struct { uint32_t line; int progress; } my_task_state;

asx_status my_task_poll(void *user_data, asx_task_id self) {
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

The compiler sees a flat switch statement, with no hidden allocations, no platform-specific assembly, and no longjmp hazards. The same task code compiles for all 9 deployment profiles, since the macros need nothing beyond C99.

## Channel Families

`asx` provides five channel types for different communication patterns, all with fixed capacity. MPSC and session senders see backpressure when a queue is full; broadcast and watch senders never wait and overwrite older values:

### MPSC (Multi-Producer, Single-Consumer)

The primary message-passing channel. Uses the two-phase reserve/send protocol described in the internal design section. Capacity is set per channel at creation, up to `ASX_CHANNEL_MAX_CAPACITY` (64 unless overridden at build time); FIFO ordering, permit-token-based backpressure.

### Oneshot (Single-Value, Single-Use)

Exactly one send, exactly one receive. Optimized for request-reply and initialization signals:

```
EMPTY → (send) → FILLED → (receive) → CONSUMED
EMPTY → (sender dropped before sending) → SENDER_DROPPED
EMPTY or FILLED → (receiver dropped before receiving) → RECEIVER_DROPPED
```

No ring buffer needed, just a single value slot with a 5-state lifecycle. Useful for task spawn result delivery and one-time configuration handoff.

### Broadcast (Multi-Consumer, Lag-Tolerant)

Each receiver sees every message sent after it subscribed, unless it falls behind. Each receiver maintains its own cursor into a shared ring buffer, and a send into a full ring overwrites the oldest message. If a receiver falls behind the write position by more than capacity, it receives `ASX_E_LAGGED` (with the number of missed messages in `*out_value`) and its cursor advances to the oldest available entry. Applications must handle lag explicitly; `asx` reports it rather than hiding it.

### Watch (Single-Value Observable)

Sender replaces a single value; receivers always see the latest. Unlike broadcast, there is no message history, only the current version. Each receiver tracks `last_seen_version` and `asx_watch_has_changed()` returns whether the value has been updated since last read. Ideal for configuration propagation and health-status monitoring.

### Session (Bidirectional with Obligation Tracking)

Two endpoints (initiator and responder) with independent queues in each direction (`i2r` and `r2i`). Each request the initiator sends increments an obligation counter, and each response it receives decrements it (`asx_session_obligations()`). The counter only tracks; backpressure comes from the per-direction queues, which refuse a send with `ASX_E_CHANNEL_FULL`. Lifecycle: `OPEN → HALF_CLOSED → CLOSED`.

## Actor Supervision

The actor subsystem has GenServers and managed supervisors, both ported from asupersync and checked against it (the `actor-*` and `supervision-*` fixtures).

GenServers are ported from Rust's `gen_server.rs`:
- A server is a task with an mpsc mailbox. It waits in the mailbox's receive, and every cast, call or stop wakes it. It serves eight messages per poll, then yields.
- `asx_actor_cast` and `asx_actor_call` take the caller's `asx_cx` and are polled like the Rust futures: a cast waits while the mailbox is full; a call reserves a mailbox slot and a reply permit (two SendPermit obligations held by the caller), then parks until the reply. Calls from a root-region task are refused, as in Rust. `asx_actor_try_cast` needs no task.
- `asx_actor_stop` lets the server serve what is queued and stop; `asx_actor_join` waits for its task.
- A cancelled server stops at its next loop check or receive. It seals its mailbox and drains it without handling the casts, drops the queued calls (their callers get no reply), then runs `terminate` with `ASX_E_CANCELLED`. A failing callback is the C form of a Rust panic: the server stops at once and its task completes PANICKED.

A managed supervisor (Rust's `ManagedSupervisor` in `supervision.rs`) is a controller task that runs each child as a series of generations: every generation is a task in a region of its own, below the supervisor's region. A supervisor of n children therefore holds up to n + 1 regions and n + 1 tasks at once, which counts against `ASX_MAX_REGIONS` (4 in resource class R1). `asx_supervisor_spawn` takes the children's specs (a start function that returns the generation's body, a restart mode, dependencies on other children) and a config; `asx_supervisor_join` waits for the controller's report, and `asx_supervisor_abort` cancels the controller, which then drains every generation.

| Strategy | When a child ends and may be restarted |
|---|---|
| **ONE_FOR_ONE** | Only that child is replaced |
| **ONE_FOR_ALL** | Every running child is cancelled, drained and replaced |
| **REST_FOR_ONE** | That child and the children started after it are replaced |

Each child has a restart mode, as Rust's `ManagedRestartMode` decides:
- **PERMANENT**: replaced after any end, success included.
- **TRANSIENT**: replaced after an error or a panic; not after success or a cancellation.
- **TEMPORARY**: never replaced, even when a ONE_FOR_ALL or REST_FOR_ONE restart drains it.

Restart intensity limits the restarts to `max_restarts` replacement batches in any `window_ns`, each after a backoff (none, fixed or exponential; the default, Rust's, starts at 100 ms and doubles up to 10 s). When the limit refuses a restart, the escalation policy decides: STOP leaves the child stopped, RESET_COUNTER forgets the history and retries, ESCALATE ends the supervisor with a RestartLimit error and cancels the region it was spawned in (FailFast). A panic in a child's body is caught inside its generation and reported as the child's panicked outcome.

The controller watches its generations' tasks (`asx_task_watch`) and sleeps until one ends, so a quiet tree costs no polls. Its restart decisions, drain order and `managed_supervisor_v1` user traces match Rust's under lab dispatch. Name registries, dynamic supervisors with shared restart domains, restart storm detection and per-child shutdown budgets are not ported (see `include/asx/actor/supervisor.h`).

## Structured Concurrency and Capability Flow

The capability context (`asx_cx`) carries the authority a task needs to operate: region ID, task ID, capability bitmask, budget, clock source, and entropy state. Capabilities flow downward through explicit narrowing:

```c
#include <asx/asx.h>

/* A child context for a worker that may spawn and set timers, and nothing
 * else the parent holds. */
asx_status worker_cx(const asx_cx *parent_cx, asx_cx *child_cx) {
    return asx_cx_narrow(parent_cx, child_cx, ASX_CAP_SPAWN | ASX_CAP_TIMER);
}

/* Or keep everything except the right to spawn. */
asx_status no_spawn_cx(const asx_cx *parent_cx, asx_cx *child_cx) {
    return asx_cx_attenuate(parent_cx, child_cx, (asx_cap_flags)~ASX_CAP_SPAWN);
}
```

Scopes bind a capability context to a region for spawning child tasks. The `ASX_CAP_SPAWN` capability is required to build a scope: `asx_scope_init()` refuses a context without it (`ASX_E_PERMISSION_DENIED`), and `asx_scope_spawn()` returns `ASX_E_INVALID_STATE` for a scope lacking it, so no task creates children through a scope unless its parent granted that capability. The plain `asx_task_spawn()` checks no capability. Today the context's own operations check their bits (cancel check, budget read/consume, clock read, entropy) and `src/net/net.c` checks `ASX_CAP_CHANNEL`; `ASX_CAP_TIMER`, `ASX_CAP_OBLIGATION`, `ASX_CAP_TRACE` and `ASX_CAP_CANCEL_REQUEST` are carried and narrowed but not yet checked by the timer, obligation, trace or cancel APIs.

An `asx_cx` is a plain struct the caller owns; its budget, clock and entropy pointers borrow caller-owned storage, so creating or narrowing one allocates nothing. The design mirrors Rust's ownership-based authority model, adapted for C's manual lifetime management.

## Quiescence: When Is a Region Truly Done?

Quiescence is defined by four conditions on a CLOSED region:

| Condition | Check | Status from `asx_quiescence_check()` |
|---|---|---|
| **Q1** | All region-local tasks completed (`region.task_count == 0`) | `ASX_E_QUIESCENCE_TASKS_LIVE` |
| **Q2** | All child regions closed | none: `asx_quiescence_check()` does not test it, and finalization waits in DRAINING until children close (see `q2_children_closed`) |
| **Q3** | No reserved obligations remain (`obligations_reserved == 0`) | `ASX_E_OBLIGATIONS_UNRESOLVED` |
| **Q4** | Cleanup stack fully drained | `ASX_E_QUIESCENCE_NOT_REACHED` (also returned when the region is not CLOSED) |

`asx_quiescence_check_detailed()` reports a region quiescent when it is CLOSED and Q1-Q4 all hold, with one flag per condition (`q1_tasks_complete` ... `q4_cleanup_drained`), so you know exactly what's still alive.

This region-local `task_count` is not the same as `asx_runtime_task_count()`. The runtime-wide count APIs report arena-resident slot occupancy, so completed tasks and resolved obligations may still contribute there until the next runtime reset or reclaim path. Runtime-wide quiescence therefore uses a stricter live-work check rather than raw slot counts.

Poisoning (`asx_region_poison`) is a flag, not a lifecycle state: opening a child region, spawning, reserving an obligation and closing then return `ASX_E_REGION_POISONED`, existing tasks keep running, and the region is not finalized automatically when its tasks finish. Nothing is notified, and the quiescence checks do not read the flag.

## Virtual Time and Anomaly Injection

For deterministic testing of timing-sensitive code, `asx` provides a virtual time source with three anomaly types:

| Anomaly | Effect | Use Case |
|---|---|---|
| **Jitter** | Add/subtract nanosecond delta at trigger point | Test deadline sensitivity to clock noise |
| **Stall** | Freeze time for N queries (time stops advancing) | Test stall detection and watchdog recovery |
| **Jump** | Instantly advance by N nanoseconds | Test timeout handling and timer wheel edge cases |

Virtual time is installed via the clock hook system:

```c
#include <asx/asx.h>
#include <asx/runtime/rt.h>
#include <asx/runtime/virtual_time.h>

/* A runtime whose logical clock is `vtime`, starting at 0 with 1 µs ticks */
asx_status runtime_on_virtual_time(asx_runtime *rt, asx_vtime_state *vtime) {
    asx_runtime_config cfg;
    asx_runtime_hooks hooks;
    asx_status st;

    asx_vtime_init(vtime, 0, 1000);
    asx_runtime_config_init(&cfg);
    st = asx_runtime_hooks_init(&hooks);
    if (st != ASX_OK) return st;
    hooks.clock.logical_now_ns_fn = asx_vtime_now_ns;
    hooks.clock.ctx = vtime;
    return asx_runtime_init(rt, &cfg, &hooks);
}
```

On a runtime that is already up, `asx_vtime_install(&vt)` makes `vt` its
clock (wall and logical, through `asx_runtime_set_hooks`, so the hook
contract is still validated) and `asx_vtime_uninstall(&vt)` restores the
clock it replaced. A vtime advances on every read; the lab runtime instead
uses a frozen clock (below).

Combined with fault injection (`ASX_FAULT_CLOCK_SKEW`, `ASX_FAULT_CLOCK_REVERSE`, `ASX_FAULT_ENTROPY_CONST`, `ASX_FAULT_ALLOC_FAIL`), this lets you reproduce production timing anomalies in a deterministic test environment. Every injected fault has a trigger-after count and a duration, so you can say "after the 50th clock read, add 100ms of skew for the next 10 reads."

## Hot Config Reload

`asx_runtime_reload_config()` accepts changes to some `asx_runtime_config` fields without a restart:

**Reloadable at runtime** (no restart needed):
- `leak_response`, `leak_escalation` — panic, log, silent, or recover on detected leaks
- `wait_policy` — busy-spin, yield, or sleep (resource-plane only)
- `finalizer_poll_budget` — cleanup poll cap
- `finalizer_time_budget_ns` — cleanup time cap
- `finalizer_escalation` — soft, bounded-log, or bounded-panic

Today only the leak fields change behavior; `wait_policy` and the `finalizer_*` fields are validated and stored, but nothing reads them yet.

**Rejected** (require restart):
- `max_cancel_chain_depth`, `max_cancel_chain_memory` — existing in-flight chains may exceed new limits
- `io_backend`, `cleanup_hard_bound`, and the struct `size`

Validation compares each field with the active config and rejects a change to a non-reloadable one, naming it; the old config then remains in effect. The copy itself takes no lock: call it from the scheduler thread (between scheduler calls or inside a task). Tasks run only on that thread and blocking-pool workers never read the config, so no task sees a half-applied config.

## Telemetry Tiers

Events emitted through `asx_telemetry_emit()` are filtered at three levels, set with `asx_telemetry_set_tier()` and effective immediately:

| Tier | Events Retained | Digest Computed | Overhead |
|---|---|---|---|
| **FORENSIC** | All events in ring buffer | Yes | Full |
| **OPS_LIGHT** | Lifecycle + terminal events only | Yes | Moderate |
| **ULTRA_MIN** | None (digest only) | Yes | Minimal |

The telemetry digest (FNV-1a mixing, `asx_telemetry_digest()`) is updated regardless of tier, so an `ULTRA_MIN` run's digest is comparable with a `FORENSIC` run's. The tiers apply only to `asx_telemetry_emit()`: the runtime's own events go through the trace (`asx_trace_emit()`) and are recorded whatever the tier.

## Regression Localization

When a trace digest changes between versions, `asx_regression_localize()` points at the subsystem that diverged. It partitions events by subsystem (scheduler, lifecycle, obligation, channel, timer; other kinds count as unknown), counts events and sums auxiliary data per partition, then compares a baseline snapshot with the current one. The result is an `asx_regression_report`: whether it regressed, the first divergent trace event, and up to `ASX_MAX_SUSPECTS` suspects, each with an event delta, an aux delta and a 0-100 blame score. For example, a report might read:

```
regressed: 1
suspects[0]: scheduler  (event_delta +2, blame_score 50)
suspects[1]: channel    (event_delta +2, blame_score 50)
```

This narrows a digest mismatch from "something changed somewhere" to "the scheduler emitted 2 extra events," giving you a concrete starting point for investigation.

## Hindsight Logging

The hindsight ring buffer (256 entries) records the runtime's nondeterministic inputs: clock reads, entropy draws, and reactor wait/poll results (ready or timed out). Each entry carries the trace sequence number at which it occurred and the observed value. (Event kinds for timers, signals and tie-breaks exist in the enum but nothing logs them yet.)

When a replay diverges from the expected path, the hindsight log shows where nondeterminism entered. `asx_hindsight_check_divergence()` compares the log's digest with an expected one; flush the log (`asx_hindsight_flush_json()`) and compare it with the baseline run's to read something like "at trace sequence 847 the entropy draw returned 0x1f3a, where the baseline shows 0x7c2b."

The log has its own digest (FNV-1a mixing from the asx digest seed) for that comparison.

## Per-Profile Overload Policies

The overload catalog (`src/runtime/overload_catalog.c`) records an overload policy per profile:

| Profile | Strategy | Threshold | Safety Constraints |
|---|---|---|---|
| CORE, POSIX, WIN32, PARALLEL | Reject | 90% capacity | No silent drops, no nondeterminism |
| FREESTANDING | Reject | 80% | No silent drops, no nondeterminism, no unbounded queues |
| EMBEDDED_ROUTER | Reject | 75% | No silent drops, no nondeterminism, no unbounded queues, no latency spikes |
| HFT | Shed oldest | 85% | No nondeterminism, no latency spikes, no unbounded queues |
| AUTOMOTIVE | Backpressure | 90% | No silent drops, no nondeterminism, no deadline misses |
| BROWSER | Reject | 80% | No silent drops, no nondeterminism, no unbounded queues |

The constraints are a bitmask of forbidden behaviors (`ASX_FORBID_SILENT_DROP`, `ASX_FORBID_NONDETERMINISTIC`, `ASX_FORBID_UNBOUNDED_QUEUE`, `ASX_FORBID_LATENCY_SPIKE`, `ASX_FORBID_DEADLINE_MISS`). The mask documents the policy; no code checks a configuration against it. The catalog is applied through the vertical adapters (below); spawn and the scheduler do not consult it.

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

Config structs carry a size field (`cfg.size = sizeof(cfg)`; the `*_init()` functions set it). Today a size that does not match the library's struct is rejected with `ASX_E_INVALID_ARGUMENT`; older layouts are not read yet, so a caller built against different headers fails cleanly instead of being adapted.

## Plan DAG: Declarative Concurrency Patterns

The plan module describes structured concurrency as a directed acyclic graph. It builds, validates, hashes and rewrites the graph; nothing in `asx` executes a plan yet:

```c
#include <asx/plan/plan.h>

asx_status startup_plan(asx_plan_dag *dag) {
    asx_plan_id children[3];
    asx_plan_id join;

    asx_plan_dag_init(dag);
    children[0] = asx_plan_dag_leaf(dag, "fetch-config");
    children[1] = asx_plan_dag_leaf(dag, "fetch-secrets");
    children[2] = asx_plan_dag_leaf(dag, "health-check");

    /* All three must complete... */
    join = asx_plan_dag_join(dag, children, 3);

    /* ...but give up after 5 seconds (the duration is in microseconds). */
    asx_plan_dag_set_root(dag, asx_plan_dag_timeout(dag, join, 5000000u));
    return asx_plan_dag_validate(dag); /* missing children, empty groups, cycles */
}
```

DAG node types: **leaf** (unit of work), **join** (all children), **race** (first child), **timeout** (child with deadline). `asx_plan_dag_validate()` rejects any child ID not lower than its parent's, so a validated DAG has no cycle (the node constructors themselves do not check). `asx_plan_hash_compute()` gives a structural hash, and `asx_plan_rewrite()` flattens nested joins/races and collapses nested timeouts, recording each step and the before/after hashes in a rewrite certificate.

## Cleanup Stacks: Deterministic RAII in C

C has no destructors, so `asx` provides deterministic cleanup stacks, the closest analog to Rust's `Drop` or C++ RAII available in plain C99. Each region owns a fixed-size cleanup stack (32 entries). Handlers are pushed in registration order and executed in strict LIFO (reverse) order during region drain:

```c
#include <asx/asx.h>

void release_file(void *file);
void release_lock(void *lock);

asx_status hold_both(asx_cleanup_stack *cleanup, void *file, void *lock) {
    asx_cleanup_handle h_file;
    asx_cleanup_handle h_lock;
    asx_status st = asx_cleanup_push(cleanup, release_file, file, &h_file);
    if (st != ASX_OK) return st;
    return asx_cleanup_push(cleanup, release_lock, lock, &h_lock);
    /* asx_cleanup_drain(cleanup) then calls release_lock(lock) first and
     * release_file(file) second (LIFO); asx_cleanup_pop(cleanup, h) resolves
     * an entry early so drain skips it. */
}
```

Each entry carries a generation counter so stale cleanup handles cannot accidentally fire against a recycled slot. When a region reaches the FINALIZING state, its own cleanup stack is drained completely before transitioning to CLOSED, and quiescence condition Q4 verifies `asx_cleanup_pending() == 0`. There is no public call yet that registers a handler on a region's own stack, so application code keeps its own `asx_cleanup_stack` (as above) and calls `asx_cleanup_drain()` itself.

Handlers on a region's stack always run, LIFO, before the region closes; for a stack you own, `asx_cleanup_drain()` gives the same deterministic order.

## Outcome Severity Lattice

The outcome join operation is the algebraic heart of result aggregation. When multiple branches complete (as in a join combinator or a task group), their outcomes are merged using a **max-severity join**:

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

In Rust, `Send` and `Sync` prevent data races at compile time. `asx` offers opt-in runtime affinity checks instead (compile-time gated behind `ASX_DEBUG_AFFINITY` for zero overhead in release):

An affinity table (256 entries) maps entity IDs to domain bindings. Three special domains: `ANY` (unrestricted), `NONE` (sentinel), and numeric domain IDs (representing threads or execution contexts). Operations:

- **Bind**: Associate an entity with a domain. Fails if already bound elsewhere.
- **Check**: Verify the current execution context matches the entity's domain. Raises `ASX_E_AFFINITY_VIOLATION` on mismatch.
- **Transfer**: Move an entity from one domain to another. Requires the caller to be in the entity's current domain (`ASX_E_AFFINITY_VIOLATION` otherwise).

The runtime does not bind or check its own entities yet, and the current domain is one process-wide value rather than per-thread, so these checks catch misuse only in code that calls `asx_affinity_set_domain()`, `asx_affinity_bind()` and `asx_affinity_check()` itself. Tasks all run on the scheduler thread; only blocking-pool jobs run elsewhere.

## State Transition Tables: O(1) Lifecycle Validation

Every lifecycle transition in `asx` is validated against precomputed boolean tables:

```c
#include <asx/asx.h>

/* ASX_OK, or ASX_E_INVALID_TRANSITION for a move the table forbids. */
asx_status finish_task(asx_task_state from) {
    return asx_task_transition_check(from, ASX_TASK_COMPLETED);
}
```

A transition check is a single array lookup, `allowed[from][to]`, in the tables of `src/core/transition_tables.c`. No switch statements, no iteration, no hash maps. Region and obligation state changes go through this check in every build and are refused on failure. Task state changes are checked only by the ghost protocol monitor (`ASX_DEBUG_GHOST`), which records a violation but does not block the write.

Helper functions classify states: `asx_task_is_terminal()`, `asx_region_is_closing()`, `asx_region_can_spawn()`. The transition tables are the authoritative specification of lifecycle legality. If the table says a transition is forbidden, no code path may perform it.

## Waker System: Bridging I/O to Scheduling

Wakers carry readiness from outside the scheduler: the I/O driver, the blocking pool and `src/net` signal a waker for the waiting task, which marks it ready for the next scheduler poll. Channels, sync primitives and timers wake parked tasks directly (`asx_task_wake`, task timers) instead.

The waker arena holds 64 slots, each binding a task ID to a signaled/alive state with generation stamping. Key properties:

- **Wakers are cloneable**: Multiple I/O sources can hold references to the same waker slot.
- **Wakers are generation-safe**: Stale wakers (from a previous task that used the same slot) fail with `ASX_E_NOT_FOUND` rather than waking the wrong task.
- **Drain collects signaled tasks**: `asx_waker_drain_signaled()` collects up to `max_tasks` signaled task IDs (oldest signal first) under one lock hold and clears their flags, ready for the scheduler to poll them.

Wakers are also the **cross-thread** path. Blocking-pool workers (`asx_spawn_blocking`, which native DNS lookups use; they are threads only in live builds) signal a completion waker from their own thread. The waker arena is lock-protected, and the scheduler publishes "about to block" under the same lock: a wake that lands after its last drain calls the reactor's `notify_fn` (a self-pipe in the POSIX epoll/poll set), so a scheduler blocked in the reactor wakes within milliseconds and no wake is lost.

## Synchronization Primitives

All sync primitives are cooperative (no OS-level blocking), async-friendly (begin/poll/cancel; Once uses `get_or_init` called again while pending, plus `wait_cancel`), and cancel-safe (cancellation removes the waiter without corrupting shared state). They are meant for tasks on the scheduler thread, not for cross-thread use.

Waiters of every primitive and channel are nodes in one runtime-wide pool (`ASX_WAIT_NODE_CAPACITY`, four per task slot by default), so no primitive has a waiter limit of its own; as in Rust, whose waiter queues are unbounded, every task of the runtime can wait on one mutex and is served in arrival order. Only an exhausted pool is reported (`ASX_E_RESOURCE_EXHAUSTED` from a `*_begin`; a channel waiter yields and is re-polled instead), after the nodes of dead tasks have been reclaimed.

**Mutex**: Cooperative mutual exclusion implemented as a semaphore with count 1. `try_lock()` returns immediately; `lock_begin()`/`poll_lock()` yield until available. No priority inheritance: a waiting task does not raise the lock holder's priority. Releasing the guard with `asx_mutex_unlock_poisoned()`, the analog of Rust's guard dropped during a panic, poisons the mutex: every later lock fails with `ASX_E_INVALID_STATE`, as Rust's `LockError::Poisoned`.

**Semaphore**: Counting permit system with configurable initial count, following Rust's `Semaphore`. An acquire takes one permit, or `n` at once with `asx_semaphore_acquire_many_begin` / `asx_semaphore_try_acquire_many` (all or nothing; the permit returns all `n` on release). Waiters queue in arrival order (a waiter joins the line at its first poll that has to wait), and only the front of the line takes permits: a release wakes the front waiter if it can now run, and that waiter, taking its permit, wakes the next. `try_acquire()` is non-blocking and fails while anyone is queued. `asx_semaphore_add_permits` grows the pool (saturating) and wakes the front waiter if it can now run; `asx_semaphore_forget` drops a permit without returning its permits, aborting its obligation. A permit value is released once (C cannot consume a permit the way Rust's drop does), and a second release or forget is refused with `ASX_E_INVALID_STATE`: the runtime recognizes a mutex guard whose hold of the lock ended (so a stale guard cannot unlock a later holder's lock), a permit whose obligation its release committed or its forget aborted, and any release that would return permits nobody holds. Integrates with the obligation system for permit tracking. (The mutex instead hands the lock straight to the front waiter on unlock, as Rust's `Mutex` does.)

**RwLock**: Many readers or one writer, following Rust's `RwLock` and its bounded writer-preference policy. A read or write takes the lock at its first poll if it can (a read when no writer holds or waits for it, a write when nobody holds it and no writer is queued); otherwise it joins the line and waits for a grant. A writer's release serves the oldest queued writer unless readers queued before it, who go first; the last reader's release serves the oldest queued writer; after 16 writer hand-offs in a row while a reader waits, the oldest queued reader gets a turn. Grants go to the front of the line even when its task has a cancel request pending; that waiter gives the lock back at its next poll. `try_read` / `try_write` fail while a writer waits. A write guard released with `asx_rwlock_write_unlock_poisoned()` (Rust's write guard dropped during a panic) poisons the lock: it wakes every queued waiter, and every later read or write fails with `ASX_E_INVALID_STATE`; read guards never poison. Unlike Rust, a write guard cannot be downgraded.

**Barrier**: N-way rendezvous with leader election, following Rust's `Barrier`. A waiter arrives at its first `poll_wait` (not at `wait_begin`), after the Cx checkpoint that every poll starts with, so an already-cancelled task never arrives. All N must arrive before any proceed; the arrival that completes N trips the barrier and is elected leader (`is_leader = 1`), and the next arrival starts a new round. Cancel-safe: a cancelled waiter withdraws its arrival without tripping the barrier, unless its round already tripped, in which case release wins and the wait succeeds.

**Once**: Compute-once cell storing a 64-bit value. `get_or_init(init_fn)` caches the first successful result and later calls return it; a failed or cancelled initialization leaves the cell empty and the next caller retries. Deterministic initialization order (first caller wins).

**Notify**: Async event signaling supporting `notify_one()` (FIFO single-waiter wake) and `notify_all()` (broadcast). Close wakes all waiters with `ASX_E_DISCONNECTED`.

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

The monitor module provides threshold-based health evaluation. It runs when the caller invokes `asx_monitor_evaluate()` (the scheduler does not call it):

| Metric | Default Threshold | Level If Exceeded |
|---|---|---|
| Region utilization | 75% | WARN |
| Task utilization | 75% | WARN |
| Obligation utilization | 75% | WARN |
| I/O registration utilization | 75% | WARN |
| Blocking pool utilization | 75% | WARN |
| Ghost violations | 0 | FAIL |
| Deadline miss rate | 1% | WARN |
| Watchdog violations | 0 | FAIL |

The monitor queries the runtime inspection report, computes utilization percentages, and sets a triggered bitmask for each threshold exceeded. Results are recorded as evidence entries, feeding into the verdict system for automated health reporting.

## Doctor: Runtime Health Diagnosis

`asx_doctor_run()` checks a runtime you pass it; the `asx doctor` command of the in-tree CLI (`make cli`) runs the same checks on a fresh runtime of its own build:

1. Is the runtime initialized?
2. Region/task/obligation/IO/blocking utilization: OK up to 75%, WARN above 75%, FAIL at capacity. The I/O and blocking checks report OK when the profile has no such surface.
3. Safety profile (debug, hardened or release).
4. Containment policy (fail-fast, poison-region or error-only).
5. Which network backend is active (`net`: native OS sockets or the in-memory transport).

Ghost violations, deadline misses and watchdog violations are monitor checks (above), not doctor checks. The report renders as text or JSON (`asx_report_doctor_text`, `asx_report_doctor_json`, `asx doctor --json`). Each check produces a named finding with severity, message, current value, and capacity.

## Native I/O: Sockets, DNS, Files, Processes, Signals

Native I/O exists only in live (non-deterministic) builds of the POSIX profile; the other profiles compile it out. There, `asx_posix_hooks_install()` supplies a real readiness reactor (epoll with one-shot arming on Linux, poll(2) elsewhere) and a pthread blocking pool; `asx_runtime_init_default()` and the app runner install these hooks for you. When `asx_runtime_init` finds a live reactor, it switches the net, fs, process, and signal surfaces to their **NATIVE** backends. A runtime initialized with plain `asx_runtime_hooks_init()` hooks, deterministic builds, and `*_reset()` keep the in-memory **MEMORY** backends, so lab, replay, and conformance runs never touch the OS. There is no kqueue, IOCP or io_uring backend (selecting `ASX_IO_BACKEND_IO_URING` is refused with `ASX_E_PERMISSION_DENIED`). Every native operation that would block parks the polling task on readiness and returns `ASX_E_PENDING`, and the reactor wakes exactly that task:

| Surface | Native behaviour |
|---|---|
| TCP / UDP | Non-blocking BSD sockets. A read returning `ASX_OK` with 0 bytes is EOF, writes may be partial, and `ASX_E_DISCONNECTED` means reset or refused. |
| DNS | Literals (IPv4, full RFC 4291 IPv6, `localhost`) resolve deterministically. Other names run `getaddrinfo` on the blocking pool through `asx_resolve_poll`, and the task parks until the lookup's completion wakes it. |
| HTTP/1.1, WebSocket | Sans-IO engines (`asx/net/http.h`, `asx/net/websocket.h`) plus server/client connection drivers over the stream API. Keep-alive and RFC 6455 framing are included. The HTTP server (`asx_http_server_poll` over an `asx_server`) follows Rust's `Http1Listener` defaults: an idle timeout (60 s) bounds keep-alive idling, reading a whole request and stalled writes, so slow clients cannot hold a slot; at most 1000 requests per connection; at the connection limit (16 slots, `ASX_HTTP_SERVER_MAX_CONNS`) extra clients are accepted and closed at once instead of hanging; a graceful shutdown closes what is left at the drain deadline (`drain_timeout_ms`, 30 s); `listen_addr` binds any IPv4/IPv6 address, including `0.0.0.0` and `::`. `tests/e2e/http_robustness.sh` checks this on real sockets. |
| Files | Real files and directories (open/read/write/seek/sync/metadata, `mkdir -p`, rename, directory iteration). Regular-file I/O runs synchronously. |
| Processes | fork/execve with PATH lookup, env, and cwd. Exec failures come back through an error pipe. Non-blocking stdio pipes park on readiness, exit waits park on a pidfd on Linux, and children are always reaped. |
| Signals | Refcounted `sigaction` handlers feed a self-pipe, and each subscriber parks on its own duplicate. The app runner turns SIGTERM/SIGINT into a `SHUTDOWN` cancel of the server region. |

Real sockets need a live POSIX build. A plain `make build` is CORE and
deterministic, so its server listens on the in-memory transport below,
not on the OS:

```bash
make build PROFILE=POSIX DETERMINISTIC=0 LDFLAGS="-lpthread -lrt"
```

With CMake, configure the same build, or pull the library into your own
project (the `asx` target carries its include path and thread library):

```bash
cmake -S . -B build-posix -DASX_PROFILE=POSIX -DASX_DETERMINISTIC=OFF
cmake --build build-posix
```

```cmake
set(ASX_PROFILE POSIX CACHE STRING "" FORCE)
set(ASX_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(ASX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(asupersync_ansi_c)
target_link_libraries(my_server PRIVATE asx)
```

Once `asx_runtime_init` has run, `asx_net_get_backend()` reports which
backend is active (`ASX_NET_BACKEND_MEMORY` or `ASX_NET_BACKEND_NATIVE`), and
`asx doctor` lists it as its `net` check.

## In-Memory Network Transport

The MEMORY backend provides a deterministic in-memory transport that mirrors real socket semantics without touching the OS network stack. This enables network-aware scenarios to run deterministically in lab mode:

**TCP**: Listener with an accept queue `ASX_MAX_TCP_STREAMS` deep (16 by default), stream handles each with one receive buffer of `ASX_BUF_CAPACITY` bytes (4,096 by default); a write copies into the peer's buffer. `listen()` → `accept()` → `connect()` → `send()`/`recv()` → `close()`. The port namespace behaves like the OS's: port 0 takes an ephemeral port (from 49152, deterministic), and an address already listening is refused with `ASX_E_ALREADY_EXISTS`. An accept with no client, a read of an empty stream, or a write to a full peer parks the polling task until a connection, data, the peer's close, or inbox space wakes it, so an idle server costs no polls.

**UDP**: Socket with a 4-deep datagram queue, each datagram up to `ASX_BUF_CAPACITY` bytes (larger sends return `ASX_E_BUFFER_TOO_SMALL`, a full queue `ASX_E_WOULD_BLOCK`). `bind()` → `sendto()`/`recvfrom()` → `close()`.

**Address handling**: IPv4 dotted-quad parser, IPv4/IPv6 loopback constructors, address equality. Connecting to an address with no in-memory listener succeeds with an unconnected stream whose reads and writes return `ASX_E_PENDING` indefinitely (a read parks the polling task); a UDP send to an address with no bound socket returns `ASX_E_PENDING`.

**Happy eyeballs ordering**: Resolution results are ordered by address-family preference (IPv6 first by default), with a small per-resolver cache. `asx_tcp_connect_host` connects to the first ordered endpoint; racing several endpoints is a task-group `RACE` away.

## Buffer and Byte Slice Primitives

The bytes subsystem provides zero-allocation buffer management:

**`asx_buf`** (immutable slice): Pointer + length over `const uint8_t*`. Constructors from raw pointer, C string, or empty. Supports slicing with bounds validation and equality comparison.

**`asx_buf_mut`** (mutable buffer): Fixed-capacity buffer with read/write position cursors. Supports sequential write (append), sequential read (consume), clear (reset cursors), and remaining-capacity queries. All operations are bounds-checked: writes beyond capacity return `ASX_E_RESOURCE_EXHAUSTED`, and reads past the readable bytes return `ASX_E_INVALID_ARGUMENT`.

Both types are stack-allocable and designed for cursor-based I/O patterns: write into a `buf_mut`, then hand off an immutable `buf` slice for consumption.

## Stream Combinators: Async Iterators

The stream subsystem provides poll-based async iterators with zero dynamic allocation:

```c
#include <asx/stream/stream.h>
#include <stdio.h>

static int g_doubled;

static void *double_it(void *item, void *user_data) {
    (void)user_data;
    g_doubled = *(const int *)item * 2;
    return &g_doubled;
}

int main(void) {
    static const int numbers[] = {1, 2, 3};
    asx_stream source;
    asx_stream doubled;
    asx_stream_iter_state source_state;
    asx_stream_map_state map_state;
    void *item;

    /* An array source, mapped through double_it */
    asx_stream_iter_init(&source, &source_state, numbers, sizeof numbers[0], 3);
    asx_stream_map_init(&doubled, &map_state, source, double_it, NULL);

    /* ASX_STREAM_READY (item available) | ASX_STREAM_PENDING | ASX_STREAM_DONE;
     * the waker argument is accepted but no source uses it yet */
    while (asx_stream_poll_next(&doubled, NULL, &item) == ASX_STREAM_READY) {
        printf("%d\n", *(const int *)item); /* 2, 4, 6 */
    }
    return 0;
}
```

Sources and combinators include:
- **Iter source**: Iterate over a fixed array.
- **Channel/Watch/Broadcast adapters**: Bridge channel receives into the stream interface.
- **Transforming**: map, filter, filter_map, scan, inspect, enumerate, dedup, flatten, flat_map.
- **Slicing and combining**: take, skip, take_while, skip_while, chain, merge, zip, peekable, fuse, chunks, window, throttle.
- **Terminal**: fold, count, for_each, any, all, collect, nth, last.

No stream source uses the waker argument yet. The channel and broadcast adapters use waiting receives, so inside a scheduler poll a pending stream parks the polling task until a send or close wakes it; the watch adapter returns `ASX_STREAM_PENDING` without parking.

## Security Audit: Ambient Authority Catalog

The security subsystem tracks ambient authority, meaning hidden global state or implicit capabilities that bypass the explicit `asx_cx` capability flow.

The **audit runner** (`asx_ambient_audit`) walks a hand-maintained catalog of ambient-authority uses, records an evidence entry per catalog entry plus a pass/fail verdict, and fails if a non-exempted entry is added (violation ceiling 0):
- **Exempted**: the clock and entropy providers (capability boundaries by design).
- **Pristine list** (`asx_audit_pristine_modules`): modules meant to use no ambient authority.

The catalog exempts the clock and entropy providers: the POSIX and Win32 runtime hooks (`src/platform/*/hooks.c`), which deterministic builds and the lab replace with a virtual clock and a seeded PRNG, and the test-log helper's wall-clock timestamps. `make lint` scans the source (`tools/ci/check_ambient_authority.py`): it fails on an ambient clock or entropy call (`clock_gettime`, `time`, `rand`, `getrandom`, `/dev/urandom`, `QueryPerformanceCounter`, `BCryptGenRandom` and the like) in any file the catalog does not exempt, and on a catalog or pristine-list path that does not exist.

## Lab Runtime: Deterministic Test Execution

The lab runtime wires together virtual time, seeded PRNG, and scenario execution into a single convenient testing harness:

```c
#include <asx/asx.h>
#include <asx/runtime/lab.h>
#include <stdio.h>

static asx_status open_and_tick(asx_lab *lab, void *user_data) {
    asx_region_id region;
    (void)user_data;
    asx_lab_advance_time(lab, 5000000); /* 5 ms of virtual time */
    return asx_lab_open_region(lab, &region);
}

int main(void) {
    asx_lab_config cfg;
    asx_lab lab;
    asx_lab_scenario scenario;
    asx_lab_result result;
    asx_status st;

    asx_lab_config_init(&cfg);
    cfg.seed = 42;
    cfg.max_polls = 100; /* per scenario step */
    st = asx_lab_init(&lab, &cfg);
    if (st != ASX_OK) return 1;

    /* A scenario is a sequence of step functions */
    asx_lab_scenario_init(&scenario, "open-and-tick");
    st = asx_lab_scenario_add_step(&scenario, open_and_tick, NULL);
    if (st == ASX_OK) {
        st = asx_lab_run_scenario(&lab, &scenario, &result);
        printf("%u/%u steps, %llu ns, %llu polls\n", result.steps_completed, result.steps_total,
               (unsigned long long)result.elapsed_ns, (unsigned long long)result.polls_total);
    }
    asx_lab_shutdown(&lab);
    return st == ASX_OK ? 0 : 1;
}
```

The lab provides:
- **Seeded entropy**: `asx_lab_random_u64()` uses a deterministic PRNG seeded at init.
- **Virtual time**: the clock is frozen, as in Rust's `LabRuntime`: reading it never moves it. `asx_lab_advance_time(lab, ns)` moves it; `asx_lab_advance_to_next_timer()` moves it to the earliest armed timer and fires the timers then due; `asx_lab_next_timer_deadline()` reports that deadline. `asx_lab_run(lab, region, &budget)` runs the scheduler until idle, leaving the clock alone, unless `config.auto_advance` is set (off by default, like Rust's), in which case it jumps the clock to the next timer whenever every task waits on one.
- **Bounded execution**: a step whose scheduler polls exceed `max_polls` (default 1,024) fails with `ASX_E_POLL_BUDGET_EXHAUSTED`; the check runs after the step returns, so it cannot interrupt a step that never returns.
- **Region convenience**: `asx_lab_open_region()` opens a region after checking the lab is initialized.

This is a mechanism for writing tests that are reproducible across platforms: no real clocks, no real entropy, no real I/O. (The Rust parity oracle uses its own runner, `tools/conformance/runner.c`, with lab dispatch.)

## Vertical Adapters: Profile-Specific Admission Evaluators

Vertical adapters (`include/asx/runtime/vertical_adapter.h`) are overload-admission evaluators for the HFT, Automotive and Embedded Router profiles. `asx_adapter_evaluate(id, mode, used, capacity, &out)` applies that profile's overload-catalog policy (above). ACCELERATED mode returns the same decision plus annotations:

| Adapter | Profile | Policy | ACCELERATED annotations |
|---|---|---|---|
| HFT | `ASX_PROFILE_HFT` | Shed oldest | p99 and overflow from the scheduler latency histogram |
| Automotive | `ASX_PROFILE_AUTOMOTIVE` | Backpressure | Deadline miss rate and audit count |
| Router | `ASX_PROFILE_EMBEDDED_ROUTER` | Reject | Queue depth, headroom, reject streak |

`asx_adapter_isomorphism_builtin()` / `asx_adapter_isomorphism_check()` compare the two modes' decision digests over a range of loads. Both modes share one policy, so this confirms that the annotations do not change the decision. The adapters do not change scheduling, wait policy or tracing.

## Examples, Vignettes, and End-to-End Scenarios

The test and example suites serve as executable documentation:

### Examples (<!-- fact:examples -->14<!-- /fact --> programs in `examples/`)

| Example | Demonstrates |
|---|---|
| `ex_quickstart.c` | Kernel lifecycle: region open, task spawn, scheduler run, quiescence, drain |
| `ex_actor_supervision.c` | Actor mailbox, cast+call semantics, graceful stop, one-for-one restart |
| `ex_channel_flow.c` | MPSC channel two-phase send (reserve, then send or abort), FIFO receive, sender close then `ASX_E_DISCONNECTED` |
| `ex_cancel_drain.c` | Cancellation protocol and region drain semantics |
| `ex_timeout_deadline.c` | Timer wheel register / collect-expired / cancel, poll-budget exhaustion, budget meet |
| `ex_encoding_decoding.c` | Encoding/decoding pipelines, wire-format round-trips, progress tracking |
| `ex_evidence_monitoring.c` | Evidence collection, monitor policies, NDJSON rendering, observability snapshots |
| `ex_gen_server.c` | OTP-style gen_server lifecycle with call/cast/stop flow |
| `ex_join_set.c` | Dynamic JoinSet task collection and completion-order polling |
| `ex_lab_replay.c` | Trace capture and digest, identical digests across repeated runs, ghost determinism monitor |
| `ex_browser_boundary.c` | Browser profile surfaces, fail-closed gating, browser-safe operations |
| `ex_browser_replay.c` | Browser profile combined with replay verification |
| `ex_network_surface.c` | Resolver, dual-stack discovery, happy-eyeballs, TCP/UDP ghost contracts |
| `ex_pipe_io.c` | Anonymous pipe I/O, buffered reads/writes, disconnect handling |

### API Vignettes (<!-- fact:test_vignettes -->12<!-- /fact --> walkthroughs in `tests/vignettes/`)

Vignettes are self-contained programs that demonstrate one API pattern each with detailed comments. They serve as both documentation and regression tests:

| Vignette | Pattern |
|---|---|
| `vignette_lifecycle.c` | Region/task/scheduler/quiescence basics |
| `vignette_hooks.c` | Freestanding custom hook installation |
| `vignette_obligations.c` | Obligation reserve/commit/abort flow |
| `vignette_replay.c` | Event capture and deterministic replay |
| `vignette_network.c` | Network resolver and socket operations |
| `vignette_budgets.c` | Task budget algebra and exhaustion |
| `vignette_encoding_decoding.c` | Encode/decode pipeline walkthrough with artifact summaries |
| `vignette_link.c` | Long-lived request/response coordination |
| `vignette_observability.c` | Snapshot capture and inspection |
| `vignette_security.c` | Authenticated symbols, derived-context rejection, security negative-test suite |
| `vignette_console.c` | Doctor and inspection rendering |
| `vignette_plan.c` | Plan DAG and rewrite certificates |

### End-to-End Scenarios (<!-- fact:test_e2e -->23<!-- /fact --> programs in `tests/e2e/`)

E2E programs exercise full runtime paths. Each has a shell family script; `tests/e2e/run_all.sh` (`make test-e2e`) runs all of them except the POSIX adapter smoke, which CI's compiler-matrix job runs on its POSIX legs:

| Scenario | What It Tests |
|---|---|
| `e2e_core_lifecycle.c` | Region lifecycle, task spawn/complete, obligations, cancellation, quiescence, timers, MPSC two-phase send |
| `e2e_nested_regions.c` | Parent/child/grandchild shutdown order and Q2 quiescence evidence |
| `e2e_foundational_contracts.c` | IDs, outcomes, cancellation, budgets and policies working together |
| `e2e_actor_supervision.c` | Actor and supervision semantics |
| `e2e_native_host.c` | Filesystem, process and signal surfaces (smoke lane) |
| `e2e_network_surface.c` | Resolver, TCP and UDP surfaces on the deterministic in-memory transport |
| `e2e_posix_adapter_smoke.c` | POSIX hook installation: clock, entropy, runtime init |
| `e2e_server_shutdown.c` | Native server bootstrap and shutdown |
| `e2e_http_robustness.c` | A live HTTP server on real sockets, driven by `http_robustness.sh` (needs a live POSIX build) |
| `e2e_hft_microburst.c` | HFT admission under load, overload handling, fairness, mass cancellation |
| `e2e_market_open_burst.c` | HFT market-open admission spike and recovery |
| `e2e_automotive_watchdog.c` | Checkpoints under deadlines, degraded mode, watchdog containment |
| `e2e_automotive_fault_burst.c` | Fault-injection cascades under the automotive profile |
| `e2e_router_storm.c` | Region churn and task exhaustion under the router profile |
| `e2e_parallel_swarm.c` | Large-swarm PARALLEL-profile waves: tasks, cancellation, MPSC pressure, timers, blocking work |
| `e2e_codec_parity.c` | JSON and BIN round trips and cross-codec equivalence |
| `e2e_continuity.c` | Binary trace export/import, replay verification, divergence detection across simulated restarts |
| `e2e_continuity_restart.c` | Trace persistence across a simulated restart, corrupted-trace detection, snapshots |
| `e2e_raptorq_erasure.c` | RFC 6330 encode, symbol loss, decode and object reassembly |
| `e2e_robustness.c` | Exhaustion, stale handles, region poisoning, fault containment, allocator seal |
| `e2e_robustness_endian.c` | Endian round trips and unaligned access |
| `e2e_robustness_exhaustion.c` | Arena, channel, timer and obligation limits and failure-atomic rollback |
| `e2e_robustness_fault.c` | Clock anomalies, entropy, allocator failure paths under injected faults |

The browser (`browser_smoke.sh`), CLI (`cli_smoke.sh`), examples, install and OpenWrt packaging lanes are shell families without a C driver of their own.

## Build System and Compiler Policy

The Makefile enforces strict compilation standards across all targets:

**Compiler flags** for the library and its tests (warnings-as-errors, always on):
```
-std=c99 -Wall -Wextra -Wpedantic -Werror
-Wconversion -Wsign-conversion -Wshadow
-Wstrict-prototypes -Wmissing-prototypes
-Wswitch-enum -Wformat=2
-Wno-unused-parameter
```

Every enabled warning is an error. One warning is switched off, unused parameters (`-Wno-unused-parameter`; MSVC builds use `/W4 /WX /wd4100`, its equivalent). The benchmark and fuzz harnesses relax a few more (`-Wno-conversion`, `-Wno-sign-conversion`, `-Wno-unused-result`). Sign conversions, implicit truncations, missing prototypes, and unhandled enum values are all caught at compile time. Debug builds add `-O0 -g -DASX_DEBUG=1`; release builds use `-O2 -DNDEBUG`.

**Lint gates**:

| Gate | What It Catches | Run by CI |
|---|---|---|
| `format-check` | clang-format violations (deterministic formatting) | yes (`check`) |
| `lint` | cppcheck static analysis across src/ | yes (`check`) |
| `lint-docs` | Public API doc coverage; the README's C samples compile, its inventory numbers and capacity table match the tree | yes (`check`) |
| `lint-checkpoint` | Kernel loops missing `asx_checkpoint()` calls | yes (`check`) |
| `lint-anti-butchering` | Semantic-sensitive changes without a complete "Guarantee Impact" block and linked evidence | yes (`check`) |
| `lint-static-analysis` | cppcheck and clang-tidy with curated flags and explicit waivers | yes (`check`) |
| `lint-scenarios-v2` | The oracle's JSON schemas and every DSL v2 scenario validate | yes (`conformance`) |
| `lint-evidence` | Closed beads missing evidence linkage | no (`make check`, `make check-ci`) |
| `lint-semantic-delta` | Semantic behavior changes exceeding the delta budget (default: 0) | no (`make check`, `make check-ci`) |
| `lint-schema-validation` | JSON fixture files violating schema contracts | no (`make check`, `make check-ci`; nightly runs `validate_schemas.sh --strict`) |

**Size and performance gates**:
- `make size-gate`: Measures library size and cold-start time against the budgets in `tools/ci/size_cold_start_baselines.json`; exceeding a budget fails the gate.
- `make slo-gate`: Evaluates performance SLOs against `tools/ci/slo_baselines.json`. Latency or throughput regressions fail the gate.
- The perf workflow (`.github/workflows/perf.yml`) runs the same evaluators on pushes to `main`, not on pull requests.

**Local vs CI**: the gate of record is `.github/workflows/ci.yml` (table below). The two make aggregates approximate it:
- `make check`: format-check, every lint gate except `lint-scenarios-v2`, build, `make test`, model check, ABI checks, `formal-check`.
- `make check-ci`: the lint gates except `lint-docs`, build and browser builds/suites, `make test`, the 4x-capacity unit suite, model check, the vertical e2e lanes, fixture-integrity, test-gates, codec-equivalence, profile-parity, parallel-parity, fuzz-smoke, the embedded matrix and bare-metal builds. It does not run `make conformance`, `lint-docs`, `formal-check`, the full e2e suite or Rust-vs-C fuzzing.

Neither aggregate runs the sanitizer, QEMU, MSVC, `-m32` or compiler-matrix jobs.

## Formal Verification

Beyond conventional testing, `asx` includes exhaustive state-machine checks and algebraic property tests. None of them runs a model checker today.

**Transition harnesses** (`tests/formal/cbmc/`) are written so CBMC could run them (`-DCBMC` selects `__CPROVER_assert` / `__CPROVER_nondet_*` through `cbmc_compat.h`), but neither the Makefile nor CI invokes `cbmc`: `make formal-cbmc` builds them with the normal C compiler, and each enumerates its state space exhaustively:
- Task transition legality: all 36 cells of the 6x6 state matrix are checked. Out-of-range states are rejected. Terminal states have no outgoing transitions. Cancel phase progression is monotonic.
- Region transition legality: close/drain constraints checked for all state combinations.
- Obligation linearity: only RESERVED has outgoing transitions; COMMITTED, ABORTED and LEAKED are absorbing, so the table admits no double resolution. A leak is a legal RESERVED → LEAKED transition that is detected, not ruled out.

**Algebraic property tests** (`tests/formal/algebraic/`) check, over fixed sample sets (six representative budgets; every cancel kind):
- Budget lattice laws: identity (`meet(infinite, x) = x`), absorption (`meet(exhausted, x) = exhausted`), commutativity, associativity, idempotence, and the narrowing property (meet can only tighten, never loosen).
- Outcome lattice: join operator associativity and commutativity.
- Cancel monotonicity: severity is non-decreasing across kinds and `asx_cancel_strengthen` never lowers it (idempotent, associative); cleanup budgets only tighten.

**Litmus tests** (`tests/formal/litmus/`) are single-threaded checks of type layout, integer and enum assumptions, and the atomics, seqlock and EBR contracts, each built with `ASX_LOCKFREE_SINGLE_THREAD=1` and `=0`; no concurrent threads run.

Run all formal verification: `make formal-check` (includes `formal-cbmc`, `formal-algebraic`, `formal-tv`, `formal-litmus`, `formal-codegen`).

## Differential Fuzzing

The C fuzz harness (`tests/fuzz/fuzz_differential.c`) generates random operation sequences and checks on every iteration:

1. **Self-consistency**: Running the same scenario with the same seed produces identical semantic digests.
2. **Mutant robustness**: each scenario is mutated (remove, duplicate, swap, change kind, tweak argument, insert, change index) and run twice; both runs must match, and a crash aborts the run. The fuzz build (`-O2 -DNDEBUG`) has no sanitizers, so undefined behavior that does not crash goes undetected (CI's `sanitizers` job runs the unit suite, not the fuzzer, under ASan/UBSan).
3. **Rust parity**: only with `--rust-binary`; otherwise the summary reports the comparison as skipped. Rust-vs-C differential fuzzing proper is `make fuzz-differential`, which runs generated DSL v2 scenarios in asupersync itself (twin_run) and in C, and compares them exactly.

The fuzzer covers cancellation, timers, channels, obligations, budget exhaustion, region lifecycle, and quiescence. A companion minimizer (`fuzz_minimize.c`) performs delta-minimization on failing cases to produce minimal reproducing scenarios.

```bash
make fuzz-smoke          # 100 iterations (CI gate)
make fuzz-counterexample-replay
make fuzz-nightly        # 100,000 iterations (nightly)
```

Durable counterexamples live in `fixtures/fuzz_counterexamples/` with schema
`asx.fuzz_counterexample.v1`. Add minimized sanitizer crashes or Rust-vs-C
divergences there after triage; every fixture records the deterministic input
seed, expected summary counters, Rust-reference requirement, and minimization
metadata.

Resource-pressure scenarios live in `fixtures/resource_pressure/` with schema
`asx.resource_pressure.failure_atomic_scenarios.v1`. Each listed scenario must
have a matching record from `make resource-pressure-gate`; the gate emits
profile/resource-class evidence under `build/resource-pressure/` and fails if a
resource-exhausted operation mutates its pre-operation snapshot or if CORE and
the constrained lane disagree on canonical semantic digest.

## Conformance Fixture Format

The Rust parity fixtures live in `fixtures/rust_reference_v2/` (schema `asx.fixture.v2`). Each one is the unmodified output of `tools/twin_run`, which runs an `asx.scenario.v2` scenario in asupersync's LabRuntime at the pinned rev. Abridged:

```json
{
  "schema": "asx.fixture.v2",
  "scenario_id": "obligation-reserve-commit-001",
  "scenario": {
    "schema": "asx.scenario.v2", "unit": "obligation", "seed": 42,
    "regions": [{"name": "r.main", "parent": "root", "budget": null}],
    "tasks": [{"name": "t.a", "region": "r.main", "budget": null, "program": [
      {"op": "reserve", "kind": "Lease", "as": "o.lease"},
      {"op": "yield"},
      {"op": "commit", "obligation": "o.lease"}]}],
    "expect": {"kind": "ok"}
  },
  "schedule": {"forced_schedule": "...", "certificate_hash": "...", "dispatches": ["1@0 t.a ready", "..."], "decisions": "..."},
  "trace": [[{"k": "region.created", "region": "root", "parent": null}], "..."],
  "snapshot": {"regions": {}, "tasks": {}, "obligations": {}, "channels": {}, "timers_pending": 0, "now_ns": 0, "quiescent": true},
  "observations": [{"step": 1, "task": "t.a", "op": "reserve", "status": "ASX_OK", "value": null}],
  "trace_digest": "sha256:...", "snapshot_digest": "sha256:...", "semantic_digest": "sha256:...",
  "vocabulary": "asx.vocab.v2",
  "provenance": {"producer": "tools/twin_run", "rust_baseline_commit": "5e60b1c4c...",
                 "rust_toolchain_release": "...", "cargo_lock_sha256": "...", "capture_run_id": "<uuid>"}
}
```

`make conformance` runs each scenario through the C runtime (`build/bin/asx-conformance`, lab dispatch, same seed) and compares the trace (Foata layers), snapshot, observations and dispatch order canonically. A FAIL is a C/Rust divergence and an ERROR a capability the C side lacks; either is fixed on the C side, never by editing the fixture.

The older corpus in `fixtures/rust_reference/` (units `core_budget`, `core_cancel`, `core_channel`, `core_combinator`, `core_lifecycle`, `core_timer`, `smoke`, with profile and codec variants) is not executed. Only `fixture-integrity`, `codec-equivalence` and `profile-parity` read it. Its `*.codec_bin.json` variants are JSON text that differ from their JSON twins in the `codec` field and run ID, and its capture tool (`tools/fixture_capture`) largely wrote the expected events itself rather than running asupersync; the `core_combinator` fixtures are hand-written (see `docs/REALITY_CHECK_AND_BRIDGE_PLAN.md`).

## Portability Layer

`include/asx/portable.h` provides alignment-safe, endian-aware load/store functions for cross-platform binary I/O:

```c
#include <asx/portable.h>

void rewrite_port(const uint8_t *wire, uint8_t *out, uint16_t port) {
    /* Alignment-safe little-endian load (works on any address) */
    uint32_t header = asx_load_le_u32(wire);
    (void)header;

    /* Big-endian store for network byte order */
    asx_store_be_u16(out, port);
}
```

Byte order is detected at compile time from `__BYTE_ORDER__` (little-endian is assumed when the compiler does not define it, as with MSVC); the load/store helpers do not depend on it. Byte-order canaries (`0x04030201` for little-endian, `0x01020304` for big-endian) can be stored in a buffer and verified to catch producer/consumer mismatches. All functions use byte-by-byte access, with no pointer casts, no platform intrinsics, and no undefined behavior from misaligned reads. CI runs the unit suite, codec tests included, on little- and big-endian MIPS, ARMv7, AArch64 and RISC-V under QEMU as well as on x86.

Wire format conventions: binary fixture codec uses big-endian (network byte order); binary trace persistence uses little-endian.

## WASM ABI Contract

The ABI module (`include/asx/abi/wasm_abi.h`) defines type contracts for a future boundary between `asx` and a host environment (primarily WebAssembly). The repository has no wasm32 build target yet (see Limitations), so nothing here has run under WebAssembly:

**Feature flags** (8 bits defined in a `uint32_t`): deterministic mode, channels, timers, obligations, symbols, JSON codec, BIN codec, ghost monitors. Channels, timers, obligations and symbols are always set; the others follow the build. The host reads them from `asx_abi_version_current().feature_flags`.

**Compatibility classification** (`asx_abi_check_compat`): `COMPATIBLE` (full feature parity), `DEGRADED` (this build's minor version is older than the remote's, or it lacks features the remote wants; `missing_features` lists them and the host must not use them), or `INCOMPATIBLE` (major version mismatch; the host should not proceed). The function only classifies.

**Bootstrap phases** (9): `UNINITIALIZED → ABI_CHECK → HOOKS_BIND → PROVIDER_INIT → RUNTIME_INIT → READY → SHUTDOWN → TERMINATED`, plus `FAILED`, reachable from the init and shutdown phases. `asx_boot_transition_check(from, to)` is a lookup the host can call: skipping a phase returns `ASX_E_INVALID_TRANSITION`, while TERMINATED and FAILED may return to UNINITIALIZED. The runtime itself does not track a boot phase.

**Boundary types**: `asx_abi_bytes` (opaque byte span), `asx_abi_request` (method + payload + timeout), `asx_abi_response` (status + payload + recoverability), C type definitions for that future boundary.

## Lane-Based Parallel Scheduler

The parallel subsystem (`include/asx/runtime/parallel.h`) implements a
deterministic lane-based scheduler with logical workers (up to 64 in generic
PARALLEL builds). It does not use multiple cores today: `asx_parallel_run`
executes every lane on the calling thread, no platform adapter runs worker
lanes on separate OS threads, and the commit-authority report always shows
`native_live_enabled = 0`. The `parallel-parity` gate compares worker counts
1, 2, 8, and 64 for canonical digest stability. The shipped guarantee is
replay-stable event commitment and resource-plane equivalence; concurrent
execution of lanes is future work that must preserve that commit order.

**Three lane classes**:
- **READY**: Tasks with pending work (normal polling).
- **CANCEL**: Tasks in CancelRequested or Cancelling phase (priority drain).
- **TIMED**: Tasks the caller assigns there (`asx_inject_timed`); automatic classification only produces READY or CANCEL.

**Fairness policies**:
- `ROUND_ROBIN`: Equal poll share per lane per round.
- `WEIGHTED`: Proportional distribution per configurable lane weights.
- `PRIORITY`: Cancel lane drains first, then Ready, then Timed.

**Locality policies**:
- `COMPACT`: Legacy flat arena routing for constrained profiles and
  single-worker fallback.
- `WORKER_SHARDED`: Contiguous task-slot shards seed logical worker ownership
  while preserving generation-safe handles and replay-stable commit order.
- `NUMA_DOMAIN_SHARDED`: Explicit shard count for larger deployment domains;
  shard placement remains a resource-plane hint, not semantic behavior.

Cancel-streak limiting (default: 16) prevents starvation. If the cancel lane runs 16 consecutive polls without yielding, the scheduler forces a fairness yield to other lanes. Per-lane starvation detection reports when any lane goes unpolled for too many rounds.

The injector functions (`asx_inject_ready`, `asx_inject_cancel`, `asx_inject_timed`) are aliases for `asx_lane_assign(tid, lane)`. Nothing in the runtime calls them yet (the I/O driver and timer wheel do not route through them), and they are not safe to call from another thread.

Operator rule: treat `worker_count` as a capacity knob, not a semantic knob.
After changing worker count, locality mode, admission mode, queue backend, or
platform hooks, rerun `make parallel-parity` and
`rch exec -- make parallel-bench-json`; for release-facing claims also run
`rch exec -- make parallel-bench-gate` and
`rch exec -- make wave-c-acceptance-demo`. Preserve the emitted reports if the
change is used to justify a production-scale claim. The benchmark artifact
includes scheduler throughput, cancel latency, MPSC roundtrip throughput,
steal/commit/timed-wake telemetry, locality shard metadata, and observe-only
threshold status for worker counts 1, 2, 8, 32, and 64 where the active profile
supports them. The Wave C acceptance bundle adds the rerun command, profile
report, benchmark summary, incident evidence, and fail-closed unsupported-live
diagnostic in one artifact directory.

## Browser Developer Experience Diagnostics

A gated surface still reports a bare `ASX_E_PERMISSION_DENIED` (or is compiled out, see Profile Surface Gating). The browser diagnostic module lets a caller look up guidance for it: `asx_browser_dx_for_surface()` / `asx_browser_dx_lookup()` map each blocked surface to a DX (developer experience) class with a suggestion. Some of them:

| Blocked Surface | DX Class | Suggestion |
|---|---|---|
| Filesystem | `NATIVE_FS` | Use IndexedDB or channel-based I/O |
| Process spawn | `NATIVE_PROCESS` | Use Web Workers |
| OS signals | `NATIVE_SIGNAL` | Use the cancellation protocol |
| Native I/O (epoll/kqueue) | `NATIVE_IO` | Use timer wheel + channels |
| Blocking thread pool | `NATIVE_BLOCKING` | Use cooperative scheduling |

Server, gRPC, messaging, TLS, database and allocator-unseal have classes of their own. The diagnostic also checks allocator sealability and reports a surface availability matrix showing how many surfaces are available vs blocked in the current profile configuration.

## Tracing Integration Bridge

The tracing compatibility layer (`src/tracing_compat/`) exports `asx` trace-ring events, one line per event, to a sink you supply, for an external collector (there is no OpenTelemetry or journald integration in the tree):

```c
#include <asx/tracing_compat/tracing_compat.h>
#include <stdio.h>

/* Called once per event with one formatted line */
static void write_line(const char *json_line, void *user_data) {
    fprintf((FILE *)user_data, "%s\n", json_line);
}

/* Export every event in the trace ring, one line each */
asx_status export_trace(FILE *out, uint32_t *exported) {
    return asx_tracing_compat_export_current(write_line, out, exported);
}
```

Each event is formatted as one JSON line with `sequence`, `kind`, `entity` and `aux` fields, for example `{"sequence":7,"kind":"task_spawn","entity":"0x0000000000000024","aux":"0x0000000000000099"}`; `entity` and `aux` are hex strings.

## Structured Test Logging

Test programs that include `tests/test_log.h` before the harness (a handful of unit and invariant suites today; the rest log to stderr only) also emit machine-readable JSONL records. They append to `$ASX_TEST_LOG_DIR/<layer>-<suite>.jsonl` (default `build/test-logs`), and only if that directory exists:

```json
{"ts":"2026-03-18T06:30:00Z","run_id":"unit-20260318T063000Z","layer":"unit","suite":"core_cancel","test":"severity_monotone","status":"pass","event_index":7}
{"ts":"2026-03-18T06:30:00Z","run_id":"unit-20260318T063000Z","layer":"unit","suite":"core_cancel","test":"strengthen_equal","status":"fail","event_index":8,"error":{"file":"test_cancel.c","line":42,"assertion":"sev_a >= sev_b"}}
```

Summary records include aggregate metrics (count, passed, failed). Log files accumulate across runs (append mode, one file per layer and suite), so historical pass/fail rates are preserved; CI's `unit-invariant` job checks them with `tools/ci/validate_test_logs.sh --strict`.

## Documentation Suite

The top-level `docs/` directory holds the port's architecture, decision,
verification, and risk documents, among them:

| Category | Key Documents |
|---|---|
| **Semantic baseline** | `EXISTING_ASUPERSYNC_STRUCTURE.md` — authoritative lifecycle tables, cancellation protocol, budget algebra, channel semantics, timer ordering |
| **Parity tracking** | `FEATURE_PARITY.md` — semantic units with implementation/conformance status (last updated 2026-05-08, before the executed v2 conformance landed); `SCENARIO_DSL_V2.md` §6-7 — what Rust parity covers and the open C-side gaps; `REALITY_CHECK_AND_BRIDGE_PLAN.md` — the bridge program |
| **Architecture** | `PROPOSED_ANSI_C_ARCHITECTURE.md` — layering boundaries derived from semantics |
| **Decisions** | `OPEN_DECISIONS_ADR.md`, `OWNER_DECISION_LOG.md` — architecture decision records |
| **Safety** | `FORMAL_ASSURANCE_LADDER.md`, `GUARANTEE_SUBSTITUTION_MATRIX.md` — Rust-to-C safety mechanism mapping |
| **Profiles** | `HFT_PROFILE.md`, `AUTOMOTIVE_PROFILE.md`, `EMBEDDED_TARGET_PROFILES.md` — per-profile constraints |
| **Quality** | `QUALITY_GATES.md`, `TEST_COMPLETENESS_MATRIX.md` — gate definitions and coverage |
| **Risk** | `RISK_REGISTER.md`, `SEMANTIC_GAP_RISK_RANKING.md` — risk catalog and mitigation |
| **Deployment** | `DEPLOYMENT_HARDENING.md`, `OPENWRT_PACKAGING.md` — operational checklists |
| **Traceability** | `PLAN_EXECUTION_TRACEABILITY_INDEX.md` — machine-readable requirement-to-test mapping |

## CI Pipeline Architecture

The primary CI workflow (`.github/workflows/ci.yml`) runs <!-- fact:ci_jobs -->13<!-- /fact --> top-level jobs on
pushes and PRs:

| Job | Runs |
|-----|------|
| `check` | format, cppcheck, API docs and README facts (`lint-docs`), checkpoint coverage, anti-butchering proof block, static analysis, strict build, browser profile build and suites, bounded state-machine model check (a native program, not a model checker) |
| `unit-invariant` | unit, invariant and vignette suites; C conformance suites; ABI check; formal harnesses (natively compiled, see Formal Verification); the unit suite with every capacity macro raised 4x and built for each resource class; the resource-pressure gate |
| `e2e` | every e2e family (`tests/e2e/run_all.sh`, including HTTP on real sockets, browser, CLI and examples smoke), then the vertical lanes (HFT, automotive, continuity, router/market/fault bursts) |
| `conformance` | oracle schemas, fixture integrity, gate negative controls, recorded codec digests, and the Rust-captured fixtures executed in C and compared |
| `profile-parity` | recorded cross-profile digests (not executed), and the parallel-worker parity scenarios executed at worker counts 1, 2, 8 and 64 |
| `fuzz-parity` | fuzz smoke and artifact validation |
| `fuzz-rust-differential` | 200 generated scenarios (a fixed seed, so a regression gate) executed by asupersync's LabRuntime (`tools/twin_run`) and by the C runtime, which must match each one exactly; divergences are reduced by `twin_run minimize` and uploaded. The nightly workflow explores new seeds daily |
| `compiler-matrix` | GCC and Clang × CORE/POSIX/FREESTANDING/EMBEDDED_ROUTER, plus Clang 21 on CORE and POSIX; the POSIX legs also run the live unit suite and native I/O |
| `sanitizers` | the unit suite under ASan+UBSan (deterministic and live POSIX) and TSan (live POSIX) |
| `cross-qemu` | the unit suite and the Rust fixture replay, cross-compiled for mipsel, big-endian mips, armv7, aarch64 and riscv64 (Debian glibc, static) and run under QEMU user mode |
| `m32` | the unit suite and the Rust fixture replay on 32-bit x86 |
| `msvc` | the library and the unit suite built by MSVC (x64, `/W4 /WX /wd4100`, via CMake) and run with CTest, for CORE and WIN32 (deterministic builds) |
| `embedded-matrix` | router-class cross builds with size and layout rows (built, not run) |

None of the <!-- fact:ci_jobs -->13<!-- /fact --> jobs is marked `continue-on-error`, so a failure in any of
them fails the workflow. A nightly workflow (`nightly.yml`) runs the C fuzzer
for 100K iterations, Rust-vs-C differential fuzzing on a new seed each day,
the full e2e suite with the conformance and parity gates, and strict fixture
schema validation. The perf workflow (`perf.yml`, pushes to `main`) runs the
benchmarks and the SLO and size gates. The release workflow is tag-triggered
(`v*`) and builds `linux-x86_64` binary and source bundles with SHA-256
checksums, provenance files and Sigstore (cosign) signatures.

## Troubleshooting

### `ASX_E_RESOURCE_EXHAUSTED` during normal load

Your resource contract is too tight for the current workload.

Use `asx_inspect()`, `asx_doctor_run()`, or the console helpers in
`include/asx/console/console.h` to identify which gauge is saturated. Arena
limits are fixed at build time, not in the runtime config: rebuild with a
larger class (`make RESOURCE_CLASS=2` or `3`) or raise the specific
`ASX_MAX_*` / `*_CAPACITY` macro (see Capacity Macros).

### `profile-parity` fails between `core` and `embedded_router`

`make profile-parity` compares the digests recorded for each profile variant
of a fixture in `fixtures/rust_reference/`, so a failure means those recorded
digests disagree. To check executed behavior on a profile, run the Rust
fixtures under it, e.g. `make conformance PROFILE=EMBEDDED_ROUTER`.

```bash
make profile-parity
```

### JSON and BIN outputs differ

`make codec-equivalence` compares the digests recorded in the JSON and BIN
files of each fixture; the C codec round trip itself is checked by
`make fixture-integrity` and `make test-conformance-c`.

```bash
make codec-equivalence
```

### Runtime init fails on missing platform hooks

Common with freestanding or custom embedded integration. `asx_runtime_init`
returns an error (it does not abort) when the hooks are incomplete: an
allocator (`malloc_fn` and `free_fn`) is always required; deterministic builds
need a logical clock and refuse unseeded entropy (`ASX_E_DETERMINISM_VIOLATION`);
live builds need a wall clock. The log hook is optional.

Provide the required hook families through `asx_runtime_hooks`,
`asx_runtime_builder_set_*_hooks()`, or the freestanding patterns exercised in
`tests/vignettes/vignette_hooks.c`.

### Non-deterministic replay mismatch

Determinism can be broken by non-seeded entropy or profile mismatch.

Ensure `ASX_DETERMINISTIC=1`, keep `PROFILE`/`CODEC` aligned across runs, and
re-run the replay/conformance-focused suites (`make conformance`,
`make profile-parity`, `tests/unit/runtime/test_replay.c`).

### Circuit breaker stuck in OPEN state

The breaker tripped due to failure threshold exceeded. Transition to half-open for probing, or reset.

```c
#include <asx/asx.h>

/* Move an OPEN breaker to HALF_OPEN to probe recovery; once
 * config.success_threshold probes record success it closes again. */
asx_status probe_recovery(asx_circuit_breaker *cb) {
    return asx_breaker_half_open(cb);
}
```

### Deadline monitor reports unexpected misses

Check whether `asx_deadline_monitor_check()` is being called frequently enough in your poll loop. Misses are only detected when `check()` evaluates pending deadlines against the current time.

## Limitations

- `asx` is intentionally strict: undefined behavior in callers (invalid pointers, lifetime misuse outside API contract) is not masked. Ghost monitors (`ASX_DEBUG_GHOST`) catch some protocol misuse in debug builds; they do not detect memory errors.
- One thread runs every task: whichever thread calls the scheduler. The only
  other threads are the POSIX blocking pool's workers (`ASX_POSIX_BLOCKING_WORKERS`,
  default 4) in live POSIX builds; they run `asx_spawn_blocking` jobs such as
  DNS lookups and wake tasks through the lock-protected waker arena.
  Deterministic builds run blocking jobs inline. Call the rest of the API from
  the scheduler thread.
- `ASX_PROFILE_PARALLEL` runs its logical worker lanes on the calling thread;
  nothing executes lanes on multiple cores yet (`native_live_enabled` is
  always 0). Its parity gates, atomics/lock-free litmus coverage and
  large-swarm e2e lanes cover that logical model. Concurrent execution would
  have to preserve the deterministic commit order and pass the parallel gates
  first.
- Native I/O ships only in live builds of the POSIX profile: an epoll reactor on Linux with a poll(2) fallback elsewhere (only Linux runs in CI), a pthread blocking pool, sockets, files, processes and signals. There is no kqueue, IOCP or io_uring backend (`ASX_IO_BACKEND_IO_URING` is refused with `ASX_E_PERMISSION_DENIED`). The other profiles, EMBEDDED_ROUTER, HFT and AUTOMOTIVE included, compile the native backends out: their net, fs, process and signal surfaces use the in-memory backends, and their reactor is the ghost reactor unless you install reactor hooks of your own.
- WIN32 builds and passes its unit suite under MSVC in CI (deterministic builds), but has no native I/O: no sockets, no IOCP reactor (the reactor is a timed sleep), no blocking pool. See the win32 row of `docs/DEFERRED_STUBS_REGISTER.md` (`bd-v12u.3`).
- There is no WebAssembly build target. The BROWSER profile compiles natively with browser surface gating, and `tools/ci/wasm32_oracle_probe.sh`, which CI does not run, only test-compiles some sources for `wasm32-unknown-unknown` (`docs/WASM32_ORACLE_EVALUATION.md`).
- Rust parity covers what the DSL v2 scenarios can express (see `make conformance`). Networking, files, processes, HTTP and live builds are not compared with Rust, and `docs/SCENARIO_DSL_V2.md` §7 lists the open C-side gaps.
- Deterministic guarantees assume matching scenario, profile, seed, and compatible runtime version.
- Library size and RAM use per resource class have not been measured on a device; R1's limits are arena sizes, not a RAM figure.
- Binary codec schema compatibility follows explicit versioning; cross-major interoperability is not guaranteed without migration tooling.

## FAQ

### Is this a rewrite or a semantic port?

Semantic port. The C implementation is not transliterated Rust; it preserves behavior contracts through explicit state machines, parity fixtures, and replay checks. The public API is idiomatic C, not Rust-shaped.

### Does embedded mode remove features?

Not the kernel. Embedded profiles and resource classes change operational envelopes (limits/defaults), not kernel semantics: `R1` on a router runs the same combinators, cancellation protocol, and obligation tracking as `R3` on a server. The executed evidence is that the Rust-captured fixtures pass in an EMBEDDED_ROUTER build on five ISAs under QEMU, and that the resource-pressure gate gets the same digests from CORE/R3 and EMBEDDED_ROUTER/R1 lanes. What an embedded build does lack today is native OS I/O, which only the POSIX profile compiles in (see Limitations).

### Why both JSON and binary codecs?

JSON is ideal for diagnostics, debugging, and diffing; binary is the compact encoding. Both encode the same canonical fixture model. The C codec round trips are checked by `make fixture-integrity` and `make test-conformance-c`; `make codec-equivalence` compares the digests recorded in the JSON and BIN fixture files.

### Can I run this on cheap routers?

The kernel, yes. `ASX_PROFILE_EMBEDDED_ROUTER` plus `R1/R2` resource classes target OpenWrt/BusyBox-class systems, but that profile has no native socket, file, process or signal backend yet (only PROFILE=POSIX compiles them). CI cross-compiles the whole unit suite and the Rust fixture replay for mipsel, big-endian mips, armv7, aarch64 and riscv64 and runs both under QEMU user mode (Debian glibc cross compilers, static). No real device runs in CI, and the OpenWrt musl toolchains are used only when present.

### How do I validate parity against Rust asupersync?

Two ways, both blocking in CI:

- `make conformance` runs every Rust-captured scenario in
  `fixtures/rust_reference_v2/` through the C runtime and compares trace,
  snapshot, observations and dispatch order exactly (see Conformance Fixture
  Format).
- `make fuzz-differential FUZZ_V2_SEED=<n> FUZZ_V2_COUNT=<k>` generates
  scenarios, runs them live in asupersync's LabRuntime through
  `tools/twin_run` (needs the pinned Rust toolchain) and in C, and compares
  each one. CI runs seed 9 with 200 scenarios on every push; the nightly job
  tries a new seed.

Both cover what the scenario language can express (`docs/SCENARIO_DSL_V2.md`
§6 lists what it cannot, §7 the open C-side gaps). `make fuzz-smoke` is
different: it checks C self-determinism, and its Rust comparison is optional.

### Is deterministic mode slower?

It has not been benchmarked against live builds here, and the two differ in more than speed: deterministic builds use a logical clock, a seeded PRNG, the in-memory I/O backends and inline blocking jobs. In exchange you gain reproducibility and easier debugging. `make bench` measures a given build, and the HFT instrumentation module can record latencies your application measures.

### Can I embed this as a library without the CLI?

Yes. The C API is first-class: <!-- fact:api_declarations -->1,985<!-- /fact --> exported `ASX_API` declarations across <!-- fact:header_families -->38<!-- /fact -->
public header families in the current `include/asx/` tree, and one umbrella
`#include <asx/asx.h>`. The repository is library-first: `make install`
installs only `libasx.a` and the headers. The `asx` CLI (`version`, `info`,
`doctor`) is an in-tree convenience binary built separately by `make cli`
(`build/bin/asx`) and is never installed.

### How big is the compiled library?

There is no published measurement yet. The perf workflow checks the CORE release `libasx.a` on x86_64 against the archive, `.text`, `.data` and `.bss` budgets in `tools/ci/size_cold_start_baselines.json` on every push to `main`; run `make size-gate` (or `make release && size build/lib/libasx.a`) for your own build. The plan's footprint targets (a CORE kernel under 64 KB) are unproven (`docs/REALITY_CHECK_AND_BRIDGE_PLAN.md`, V31), and RAM use per resource class has not been measured on a device.

### What about thread safety?

All tasks run on one thread, the one that calls the scheduler, and most of the
API expects to be called from it. The cross-thread paths are narrow: in live
POSIX builds the blocking pool's worker threads run `asx_spawn_blocking` jobs
and signal completion through the lock-protected waker arena, which wakes a
scheduler blocked in the reactor through its self-pipe; CI runs the live unit
suite under TSan. `ASX_PROFILE_PARALLEL` adds a logical-worker scheduler and
atomic/lock-free building blocks (checked by single-threaded litmus tests),
but its lanes also run on the calling thread. Any future multi-threaded
execution must preserve the same semantic digest as the single-worker lane.

### What formal verification is included?

Exhaustive state-machine checks for task, region and obligation transitions (written to be CBMC-compatible, but compiled and run natively; nothing invokes CBMC), algebraic property tests for the budget and outcome lattices and cancel monotonicity over fixed sample sets, and single-threaded litmus checks of the atomics and lock-free building blocks. Run `make formal-check` to execute them all.

### Why not just use `setjmp`/`longjmp` for async?

`setjmp`/`longjmp` gives you stackful coroutines but destroys determinism (stack layout varies by compiler/platform), makes cancellation cleanup fragile (no destructor-like guarantees), and is invisible to static analysis. `asx` uses explicit poll-based state machines (`ASX_CO_BEGIN`/`ASX_CO_YIELD`/`ASX_CO_END` protothread macros) that compile to switch statements. Every state transition is visible, auditable, and deterministically replayable.

### How does the HFT instrumentation work?

The HFT instrumentation module (`include/asx/runtime/hft_instrument.h`, compiled into every profile) provides three instruments. The runtime does not feed them: the application records its samples (e.g. `asx_hft_record_poll_latency`).
- **Latency histogram**: 16 fixed-boundary log2 bins covering [0, 32768) nanoseconds. No floating point; bin index is `floor(log2(sample + 1))`. Records min, max, sum, and per-bin counts for O(1) percentile approximation.
- **Jitter tracker**: Streaming mean absolute deviation (MAD) computed from histogram bin midpoints. Recomputed every N samples (configurable) to bound overhead.
- **Overload policy**: Deterministic admission decisions with three modes: reject (hard fail), shed-oldest (reports how many of the oldest tasks to shed; the caller sheds them), and backpressure (return WOULD_BLOCK). The decision is a pure function of the policy (mode, threshold, shed cap), the load and the capacity, making overload behavior replayable.

A metric gate evaluates pass/fail against configurable p99, p99.9, p99.99, and jitter thresholds for CI integration.

### How does the automotive instrumentation work?

The automotive instrumentation module (`include/asx/runtime/automotive_instrument.h`, compiled into every profile) provides four instruments. As with HFT, the application feeds them (`asx_auto_record_deadline`, `asx_auto_record_checkpoint`, `asx_auto_audit_record`); `asx_monitor_evaluate()` reads the global deadline tracker and watchdog:
- **Deadline tracker**: Records hit/miss per deadline evaluation. Computes miss rate as percentage * 100 (e.g., 250 = 2.5%). Tracks worst and best margins in nanoseconds (signed: negative = miss).
- **Watchdog monitor**: Tracks intervals between checkpoint calls. Violations recorded when interval exceeds `watchdog_period_ns`. Clock reversals are clamped to zero interval to prevent fabricated violations.
- **Degraded-mode audit ring**: Bounded 64-entry ring of safety-critical events (region poison, forced cancel, deadline miss, watchdog violation, degraded-mode enter/exit) with monotonic sequence numbers.
- **Compliance gate**: Evaluates pass/fail against configurable deadline miss rate, max watchdog violations, and minimum checkpoint count. Returns a violation bitmask for CI integration.

### What's the codec equivalence system?

Codec equivalence checks that the JSON and binary encodings of the same fixture carry identical semantic content. The `asx_codec_fixture_semantic_eq()` function compares fixtures field-by-field, excluding the codec identifier itself. Mismatches are reported by field name, up to 16 per comparison (`ASX_EQUIV_MAX_DIFFS`).

Each fixture also carries provenance metadata: the Rust baseline commit hash, toolchain hash, and Cargo.lock SHA-256. For the v2 fixtures, which `tools/twin_run` writes from asupersync's LabRuntime, that traces a parity failure back to the exact Rust build that produced the fixture; the older `fixtures/rust_reference/` corpus was not captured that way (see Conformance Fixture Format).

### What makes the symbol registry useful?

Symbols are interned 16-bit IDs mapped to string names in a static 256-entry table, searched linearly by `strcmp` at registration and name lookup; the name pointer is stored, not copied, so it must outlive the registry. They provide a shared namespace for metrics, events, capabilities, and payloads across subsystems: once registered, code compares IDs instead of strings.

Symbol sets use a 256-bit bitfield for O(1) membership testing, union, intersection, and subset operations. Typed symbols pair a symbol ID with type metadata (kind, size, alignment) and reject mismatched re-registrations: if "request_latency" is registered as U64, attempting to re-register it as F64 returns `ASX_E_INVALID_STATE`. Typed values wrap a symbol + payload pointer with equality testing via `memcmp`.

## About Contributions

*About Contributions:* Please don't take this the wrong way, but I do not accept outside contributions for any of my projects. I simply don't have the mental bandwidth to review anything, and it's my name on the thing, so I'm responsible for any problems it causes; thus, the risk-reward is highly asymmetric from my perspective. I'd also have to worry about other "stakeholders," which seems unwise for tools I mostly make for myself for free. Feel free to submit issues, and even PRs if you want to illustrate a proposed fix, but know I won't merge them directly. Instead, I'll have Claude or Codex review submissions via `gh` and independently decide whether and how to address them. Bug reports in particular are welcome. Sorry if this offends, but I want to avoid wasted time and hurt feelings. I understand this isn't in sync with the prevailing open-source ethos that seeks community contributions, but it's the only way I can move at this velocity and keep my sanity.

## License

MIT License (with OpenAI/Anthropic Rider). See `LICENSE`.
