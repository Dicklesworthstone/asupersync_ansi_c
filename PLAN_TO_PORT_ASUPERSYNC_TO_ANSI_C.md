# PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C

Version: 2 (major revision)  
Date: February 26, 2026  
Status: Active planning document

## 0. Executive Summary

This project will port **asupersync** from Rust to **highly portable ANSI C** for embedded and constrained environments, with **zero third-party dependencies** in the core runtime.

The correct strategy is not source translation. It is:

1. extract a precise behavior spec from Rust,
2. re-architect for C with explicit safety and determinism controls,
3. implement in phases with conformance fixtures proving parity for in-scope behavior.

The first implementation target is a **kernel profile** (structured concurrency semantics, cancellation protocol, region/task/obligation lifecycle, timer/scheduler core, two-phase channel semantics). Higher-level surfaces (HTTP, TLS, distributed, advanced trace/lab machinery) are staged after kernel stabilization.

## 1. Inputs and Baseline Studied

This revision is grounded in direct review of the Rust codebase and docs:

- `/dp/asupersync/README.md`
- `/dp/asupersync/Cargo.toml`
- `/dp/asupersync/src/lib.rs`
- `/dp/asupersync/src/types/outcome.rs`
- `/dp/asupersync/src/types/budget.rs`
- `/dp/asupersync/src/types/cancel.rs`
- `/dp/asupersync/src/cx/cx.rs`
- `/dp/asupersync/src/record/region.rs`
- `/dp/asupersync/src/record/task.rs`
- `/dp/asupersync/src/record/obligation.rs`
- `/dp/asupersync/src/channel/mpsc.rs`
- `/dp/asupersync/src/runtime/mod.rs`
- `/dp/asupersync/src/runtime/config.rs`
- `/dp/asupersync/src/runtime/state.rs`
- `/dp/asupersync/src/runtime/scheduler/mod.rs`
- `/dp/asupersync/src/time/wheel.rs`

Scale snapshot used for planning:

- `516` Rust source files under `src/`
- ~`460k` LOC in `src/`
- Major subsystem LOC groups (approx):
  - `runtime`: 48k
  - `lab`: 37k
  - `trace`: 36k
  - `obligation`: 28k
  - `actor/supervision` (Erlang/OTP layer): 28k
  - `net`: 26k
  - `http`: 23k
  - `raptorq`: 18k
  - `combinator`: 17k

This confirms we need explicit scope waves and cannot “port everything at once” without creating an unmaintainable C codebase.

## 2. Program Constraints and Product Intent

## 2.1 Hard Constraints

- Core implementation language: ANSI C subset.
- Core dependency policy: standard C library only.
- Language baseline policy:
  - C99 is allowed where it materially simplifies correctness and maintainability.
  - Keep interfaces and core data structures portability-first (no compiler-specific extensions in core).
- No hidden runtime dependency on any external async executor.
- Must support constrained/embedded use cases.
- Deterministic mode is first-class, not an afterthought.
- Correctness invariants must be test-enforced.

## 2.2 Primary Port Goal

Recreate the **semantic contract** of asupersync’s kernel:

- structured ownership via region trees,
- explicit cancellation protocol with bounded cleanup,
- obligation lifecycle enforcement,
- deterministic execution mode with replay-oriented trace hooks.

## 2.3 Secondary Goals

- predictable memory footprint,
- explicit allocator integration,
- easy platform adaptation (POSIX, Windows, freestanding).

## 2.4 Non-Goal for Initial Release

“Feature parity with every Rust module” is not the initial objective. Kernel parity is.

## 2.5 Embedded-First Promise Without Feature Compromise

The embedded strategy is **semantic fidelity + operational adaptation**, not feature stripping:

- same kernel semantics and invariants across desktop/server/embedded profiles,
- no “embedded-lite” behavior forks that weaken correctness or lifecycle guarantees,
- constrained targets are handled by explicit resource contracts, bounded data structures, and deterministic backpressure,
- if a platform capability is missing, provide an adapter/fallback preserving semantics with documented cost envelopes.

## 3. Porting Methodology (Essence Extraction)

This project follows the same core method that works for language ports generally:

1. **Spec extraction** from source runtime behavior.
2. **Architecture synthesis** for target language and constraints.
3. **Implementation from spec only**.
4. **Conformance proofs** against source fixtures.

Direct transliteration is explicitly forbidden because:

- Rust ownership/lifetimes do not map 1:1 to C.
- Transliteration would hide undefined behavior risk.
- It produces large, fragile C with poor portability.

## 4. Rust Surface Decomposition and Port Waves

The Rust crate exports a very broad surface (`types`, `record`, `runtime`, `channel`, `sync`, `time`, `lab`, `trace`, `net`, `http`, `grpc`, `distributed`, `raptorq`, etc.). We need a principled wave model.

## 4.1 Wave A (Kernel, mandatory first)

- `types` (Outcome/Budget/Cancel/IDs)
- `record` (region/task/obligation records and transitions)
- `runtime` (single-thread kernel first, then optional parallel scheduler)
- `time` (timer wheel and budget/deadline checks)
- `channel` (at minimum MPSC two-phase send semantics)
- minimal `sync` and `util` required by kernel

## 4.2 Wave B (Determinism + parity harness)

- minimal `trace` required for replay/conformance evidence
- minimal `lab` profile to reproduce seeded schedule behavior
- fixture capture + conformance runner

## 4.3 Wave C (Selected systems surfaces)

- targeted networking primitives
- selected combinators
- selected observability surfaces

## 4.4 Wave D (Deferred advanced surfaces)

- full HTTP/2 and gRPC parity
- full database clients parity
- full distributed/remote parity
- full advanced trace topology stack
- full RaptorQ + advanced policy stack
- full Erlang/OTP-style Actor and Supervision tree parity (`gen_server`, `actor`, `supervision`)

## 5. Explicit Exclusions (Revision 2)

The first major C release excludes:

- Full Rust-surface parity across all 500+ modules.
- Rust proc-macro ergonomics.
- Immediate full replication of advanced mathematical governor/explorer surfaces.
- Immediate full transport stack parity (HTTP/2+TLS+gRPC+DB+distributed all at once).
- The massive 28k+ LOC Actor/Supervision layer (deferred to Wave D to keep kernel focus).

These are deferred by design to avoid shipping a brittle monolith.

## 6. Semantic Contract to Preserve

This section is the heart of the port. These contracts define correctness.

## 6.1 Outcome Severity Lattice

Preserve ordering and join semantics:

- `Ok < Err < Cancelled < Panicked`
- Join/aggregation takes the worst severity.

This must be encoded as deterministic, testable C behavior.

## 6.2 Budget Algebra and Exhaustion

Preserve semantics from Rust `Budget`:

- Deadline + quota composition is “tighter constraint wins”.
- Priority semantics preserved as specified by source behavior.
- Exhaustion checks deterministically trigger cancellation paths.

## 6.3 Cancellation Protocol

Preserve explicit cancellation progression:

- `Requested -> Cancelling -> Finalizing -> Completed(Cancelled)`

and metadata semantics:

- reason kind,
- attribution chain bounds,
- cleanup budget association.

## 6.4 Region Lifecycle

Preserve region lifecycle states and close semantics:

- `Open -> Closing -> Draining -> Finalizing -> Closed`
- region close requires child/task quiescence and obligation resolution.

## 6.5 Task Lifecycle

Preserve task lifecycle state machine and transition legality checks:

- `Created`, `Running`, `CancelRequested`, `Cancelling`, `Finalizing`, `Completed`
- explicit valid/invalid transitions (with test coverage).

## 6.6 Obligation Lifecycle

Preserve two-phase resource guarantees:

- `Reserved -> Committed` or `Reserved -> Aborted`
- leak detection path for unresolved obligations
- region close cannot silently ignore unresolved obligations.

## 6.7 Quiescence Invariant

Preserve runtime quiescence definition:

- no live tasks,
- no live obligations,
- no live non-finalized region work,
- optional I/O registration considerations by profile.

## 6.8 Hard-Part Strategy: “Rust-Easy, ANSI-C-Hard” Without Butchering Asupersync

This is the core anti-regression strategy for preserving what makes asupersync special.

### 6.8.1 Problem Statement

Rust gives asupersync structural advantages that C does not:

- ownership/lifetimes (compile-time),
- RAII/finalization guarantees,
- rich enums/typestates for protocol phases,
- safe async composition ergonomics,
- strong aliasing guarantees by default.

If we port naively, we will keep names but lose guarantees.  
This section defines how we prevent that.

### 6.8.2 Strategy Pattern: “Semantic Scaffolding”

We replace lost compile-time guarantees with a stack of runtime-enforced scaffolds:

1. **State-machine authority tables** (single source of truth for transitions)
2. **Linearity ledgers** (obligation usage exactly-once)
3. **Ownership graph + generation handles** (stale-handle and alias defense)
4. **Deterministic event journal** (replay and parity proof)
5. **Twin-run differential checking against Rust** (semantic equivalence gate)

The C port must be “more instrumented than comfortable” in debug mode until parity is proven.

### 6.8.3 Creative Mechanisms for “Nearly Impossible in Straight C” Areas

Here is the architectural "alpha"—the tactical playbook for bridging the gap between a compiler that catches everything (Rust) and a language that assumes you are infallible (C). We synthesize compile-time safety into O(1) runtime constraints without bloating the hot path.

### A) Replacing `async`/`await` and Captured Environments (New)

Mechanism:

- **Protothreads on Region Arenas:** Implement Duff's Device-based coroutine macros (`ASX_CO_BEGIN`, `ASX_CO_YIELD`, `ASX_CO_END`) to flatten async logic into straight-line C functions.
- Mandate that the "captured environment" (the Future's state) is strictly allocated inside the parent **Region's Arena**, never via `malloc`.

Why this matters:

- prevents the #1 cause of memory leaks in C async runtimes (forgetting to free a suspended task's state). When the region closes, the arena is wiped—perfectly mirroring Rust's `Drop` of a Future.

### B) Replacing Rust Typestate and Exhaustive Enums

Mechanism:

- **Bitmasked Generational Typestates:** Encode the state machine into opaque 64-bit handles: `[ 16-bit type_tag | 16-bit state_mask | 32-bit arena_index ]`. Every API endpoint performs an O(1) bitwise AND against its expected `state_mask`.
- Define transition matrices using X-macro generated tables:
  - one table each for region/task/obligation/cancellation.
- Generate:
  - state enum,
  - transition validator,
  - string formatter,
  - test vectors.

Why this matters:

- prevents transition logic from being duplicated and drifting.
- guarantees illegal protocol transitions fail instantly rather than silently corrupting memory.
- gives a single auditable artifact equivalent to Rust enum+match discipline.

### C) Replacing `Send` and `Sync` Thread Safety (New)

Mechanism:

- **Token-Ring Thread Affinity Guards:** Every runtime object tracks an `affinity_domain_id` (usually the thread ID).
- Inject a `GHOST_ASSERT_AFFINITY(obj)` macro at the top of every mutable API boundary in debug profiles.
- For objects passed across threads, the transition API requires an explicit `atomic_exchange` of the domain ID.

Why this matters:

- data races in C are notoriously silent. This turns silent thread-safety violations into immediate, deterministic crashes at the exact line of the violation.

### D) Replacing RAII / Drop Semantics

Mechanism:

- Add an explicit `asx_cleanup_stack` per task/region.
- **Poison Pills:** Acquisition APIs do not return raw pointers; they return a struct. Discarding it without passing it to `asx_commit` or `asx_abort` triggers detection during Region finalization.
- Every reserve/acquire operation registers a cleanup action at acquisition time.
- Commit/abort pops or marks entries resolved.
- Runtime close/finalize drains unresolved cleanup stack deterministically.

Why this matters:

- simulates “drop on all exits” behavior explicitly,
- makes leaked obligations observable and recoverable with policy.

### E) Replacing Ownership/Lifetime Safety

Mechanism:

- Region-owned arenas + generation-tagged handles (`index + generation + kind`).
- **Alien Graveyard (§14.10 EBR / §14.4 Hazard Pointers):** For optional parallel profiles, implement Epoch-Based Reclamation (EBR) or precise Hazard Pointers to defer `free()` calls until no thread can observe the pointer, effectively replicating Rust's `Arc`/lifetime safety in a lock-free C environment without garbage collection.
- **Ghost Borrow Ledger:** In debug builds, track active borrow epochs (reader-counts and a writer-flag) to trap mutable writes while shared reads are active.
- Optional debug “quarantine ring” for freed slots before reuse.
- Any stale handle dereference fails closed with explicit error code.

Why this matters:

- blocks ABA-style bugs, use-after-free patterns, and dangling pointers that would silently corrupt C state.
- mechanizes the borrow checker at runtime.

### F) Preserving Linearity for Two-Phase Obligations

Mechanism:

- Obligation records carry:
  - owner task,
  - owner region,
  - phase (`reserved|committed|aborted|leaked`),
  - creation site metadata.
- **Hardware-Accelerated Linearity Ledgers:** Obligation records are tracked in a compact, SIMD-friendly bitset. Reserving sets a bit; resolving clears it. During finalization, a hardware `popcnt` verifies the ledger is zeroed.
- One-way transition enforcement with idempotence handling for duplicate calls.
- Region close gate checks unresolved obligation count before closure.

Why this matters:

- this is the “no silent drop” heart of asupersync.
- guarantees O(1) verification that every promise was kept before region closure.

### G) Preserving Cancellation Correctness

Mechanism:

- Cancellation witness objects with monotonically increasing epoch + phase rank.
- Checkpoint API contract:
  - every long-running loop must call `asx_checkpoint()` or explicit waiver macro.
- Static CI grep/lint rule:
  - flag loops in kernel paths missing checkpoint or waiver annotation.

Why this matters:

- explicit cancellation observability is a core differentiator of asupersync.

### H) Preserving Determinism (Not Just “Best Effort”)

Mechanism:

- Deterministic scheduler tie-break key:
  - `(lane_priority, logical_deadline, task_id, insertion_seq)`.
- Deterministic timer ordering for equal deadlines via insertion sequence.
- **Alien Graveyard (§3.10 Hindsight Logging):** Record minimal nondeterminism (like external I/O arrival times or OS signals) to a ring buffer, and only flush it upon an invariant failure, allowing perfect time-travel replay without the overhead of tracking every deterministic state transition.
- Deterministic PRNG stream per runtime seed and subsystem stream IDs.
- Deterministic event journal hash at end of run.

Why this matters:

- “same seed, same behavior” is non-negotiable for replay and confidence. Replay of external events requires Hindsight Logging to make asynchronous boundaries appear deterministic.

### I) Rust↔C Twin-Run Differential Oracle (Key Creative Lever)

Mechanism:

- For each fixture scenario:
  - run Rust reference and C runtime with same seed + scenario payload,
  - capture normalized event streams and final snapshots,
  - compare canonical hashes + structured diffs.

What is normalized:

- IDs remapped to canonical ordinal domain,
- timestamp fields normalized to logical units,
- platform-specific fields removed from parity hash.

Why this matters:

- catches semantic drift that unit tests miss,
- gives confidence we preserved behavior, not just APIs.

### 6.8.4 “Do Not Butcher Asupersync” Rules

The C port must reject shortcuts that weaken identity:

- No replacing two-phase obligations with best-effort sends.
- No implicit cancellation by object free without protocol state.
- No detached task model that can orphan outside region ownership.
- No non-deterministic scheduler path in deterministic mode.
- No hidden global authority replacing explicit context flow.

Any such shortcut is a failed port even if benchmarks look good.

### 6.8.5 Hard-Part Verification Gates

A phase cannot close unless these gates pass:

1. State-machine transition test corpus:
   - all legal transitions accepted,
   - all illegal transitions rejected.
2. Obligation linearity tests:
   - reserve->commit and reserve->abort paths,
   - leak detection/recovery policy paths.
3. Deterministic replay hash stability:
   - identical hash for repeated same-seed runs.
4. Twin-run parity:
   - Rust and C normalized traces match for in-scope fixtures.
5. Counterexample minimization:
   - failing scenario reduced to minimal reproducible fixture.

### 6.8.6 Practical Engineering Tradeoff

During early phases, debug instrumentation overhead is intentionally high.  
After parity hardens, production profile strips expensive checks while preserving:

- safety-critical handle validation,
- protocol transition checks on external boundaries,
- deterministic guarantees in deterministic profile.

## 6.9 Alpha Program: Guarantee-Substitution Matrix (Rust -> ANSI C)

This is the core “alpha” engine: every lost Rust guarantee gets a deliberate C substitute plus proof artifacts.

| Rust Advantage | Why It Matters in Asupersync | ANSI C Substitution Strategy | Proof Artifact |
|---|---|---|---|
| Borrow checker + lifetimes | Prevent aliasing corruption, stale refs, UAF | Generation-tagged handles + debug `ghost_borrow_ledger` enforcing shared/exclusive borrow epochs | Alias/UAF stress suite + stale-handle rejection corpus |
| RAII + Drop | Reliable cleanup under all exits | `asx_cleanup_stack` + obligation ledger + deterministic finalizer drain | Leak-free close proofs, unresolved-obligation zero checks |
| Exhaustive enums/match | No forgotten state transitions | X-macro transition authority tables + auto-generated validators | Transition completeness report + illegal-transition rejection tests |
| Typestate-style APIs | Prevent impossible protocol states | Runtime typestate tags + transition guards at each API boundary | State-machine coverage report (all nodes/edges) |
| `Send`/`Sync` trait safety | Prevent cross-thread misuse | Thread-affinity tags + explicit transfer certificates + ownership epoch increments | Wrong-thread access tests + transfer-certificate checks |
| Panic unwinding safety model | Consistent failure containment | Panic-equivalent `Outcome::Panicked` domain + explicit compensation paths | Failure-containment scenarios + compensation replay tests |
| `async/await` structured polling | Predictable cancellation/await semantics | Explicit continuation records + poll contract + mandatory checkpoints | Checkpoint-coverage audit + cancellation-latency bound tests |

Design rule:

- if a Rust guarantee cannot be substituted with a concrete mechanism and proof artifact, that subsystem is not ready to port.

## 6.10 Ghost Safety Architecture (Debug-Heavy, Production-Light)

To avoid butchering semantics in C, the runtime will include debug-only “ghost” subsystems:

- `ghost_borrow_ledger`:
  - tracks active borrow class (`shared`, `exclusive`) per handle + epoch.
  - rejects writes when shared borrows are active.
- `ghost_protocol_monitor`:
  - independent state observer that replays lifecycle events and checks transition legality.
- `ghost_linearity_monitor`:
  - validates obligation exactly-once resolution (`commit|abort`) and detects double-resolve.
- `ghost_determinism_monitor`:
  - verifies stable event ordering keys and reproducible final digest for identical seeds.

These ghost layers are compiled out in production profile but mandatory in CI/debug until kernel parity is complete.

## 6.11 Adaptive Decisions Without Heuristic Drift (Alien Framework)

Any adaptive runtime controller (for example cancel-lane tuning or wait-graph monitors) must use:

1. **Alien Graveyard (§0.4 Expected-Loss Decision Layer):** explicit expected-loss decision model:
   - `action* = argmin_a Σ_s L(a,s) * P(s | evidence)`
2. evidence ledger output:
   - posterior/confidence, selected action, expected loss, top evidence terms, counterfactual alternative.
3. **Alien Graveyard (§12.1 Conformal Prediction):** calibrated uncertainty gate:
   - Use conformal prediction intervals or e-process style thresholding to dynamically adjust cancellation bounds based on empirically observed drain rates rather than static guesses.
4. conservative deterministic fallback:
   - fixed policy mode (no adaptation) activated when confidence drops or budget is exhausted.

Non-negotiable:

- no adaptive controller ships without fallback mode and replayable decision logs.

## 6.12 Extreme Optimization Governance for Hard-Part Work

For every optimization touching core guarantees:

1. baseline (`p50/p95/p99`, throughput, memory),
2. profile (hotspot evidence),
3. isomorphism proof block,
4. one lever per change,
5. golden/conformance verify,
6. re-profile.

Scoring gate:

- implement only levers with `EV >= 2.0` using:
  - `EV = (Impact * Confidence * Reuse) / (Effort * AdoptionFriction)`.

Rollback discipline:

- every lever must declare explicit rollback trigger and conservative fallback behavior.

## 6.13 “Do Not Butcher” Enforcement Contract

A PR touching kernel semantics is rejected unless it includes:

- guarantee-substitution statement (`what Rust guarantee was replaced and how`),
- proof artifact references (tests + parity fixtures + deterministic digest evidence),
- fallback/safe-mode behavior,
- isomorphism note for ordering/tie-break/seed behavior.

This requirement applies even if performance improves.

## 6.14 EV-Ranked Alpha Levers for Guarantee Recovery

These are candidate high-ROI levers inspired by alien-graveyard + extreme-optimization discipline.
They are hypotheses until measured against this codebase.

| Lever | Primary Symptom/Gap | Baseline Comparator | Expected Alpha | Safe Fallback |
|---|---|---|---|---|
| `ALPHA-1`: Ghost Borrow Ledger | loss of borrow/lifetime guarantees | no ghost checks | catches alias/UAF protocol violations early with deterministic repro | compile-time off in prod profile |
| `ALPHA-2`: Obligation Linearity Ledger | silent resource/obligation misuse | ad hoc reserve/commit checks | exactly-once resolution proof surface and leak root-cause quality | strict abort-on-unknown path |
| `ALPHA-3`: **(Alien §3.10)** Hindsight Logging Hash Chain | determinism drift & I/O jitter | plain event logs | tamper-evident replay identity and fast divergence localization | canonical sorted event dump mode |
| `ALPHA-4`: **(Alien §0.4/§12.1)** Expected-Loss + Conformal Gate | heuristic drift in adaptive logic | static threshold policies | explainable adaptive behavior with quantified confidence and regret | deterministic fixed policy mode |
| `ALPHA-5`: **(Alien §14.9)** Seqlocks for Runtime Metadata | lock contention on hot reads | mutex/rwlock metadata path | sub-nanosecond read overhead without locking | fallback to rwlock path |
| `ALPHA-6`: **(Alien §14.10)** EBR / Hazard Pointers | safe lock-free reclamation in C | global lock + delayed free | replicates Rust's `Arc`/lifetime bounds in concurrent C without GC | region-quiescence deferred free mode |

Execution rule:
- each lever must pass EV gate (`EV >= 2.0`) from measured hotspots before implementation.
- one lever at a time with explicit rollback.

## 7. ANSI C Target Architecture

## 7.1 Layered Runtime Design

### `asx_core` (portable, no platform dependencies)

- stable IDs with generation counters,
- outcome/cancel/budget types,
- deterministic state-machine transition functions,
- intrusive queues/maps/arena primitives,
- error code taxonomy.

### `asx_runtime_kernel` (single-thread deterministic runtime)

- region/task/obligation tables,
- scheduler loop,
- cancellation propagation,
- timer wheel integration,
- quiescence/close driver.

### `asx_runtime_parallel` (optional)

- worker model,
- work-stealing,
- lane scheduling parity where feasible,
- atomics and synchronization adapter layer.

### `asx_platform_*` backends (optional)

- `posix`,
- `win32`,
- `freestanding` (minimal hooks only).

## 7.2 Proposed Repository Structure

```text
include/asx/
  asx.h
  asx_config.h
  core/
  runtime/
  channel/
  time/
  platform/
src/
  core/
  runtime/
  channel/
  time/
  platform/
tests/
  unit/
  invariant/
  conformance/
fixtures/
  rust_reference/
tools/
  rust_capture/
docs/
  EXISTING_ASUPERSYNC_STRUCTURE.md
  PROPOSED_ANSI_C_ARCHITECTURE.md
  FEATURE_PARITY.md
```

## 7.3 API Design Principles

- Public symbols prefixed `asx_`.
- Opaque handles for complex runtime objects.
- Explicit init/destroy for all subsystems.
- Zero ambient globals in core.
- Context passed explicitly (`asx_runtime*`, `asx_task_ctx*`).
- Error returns are explicit status codes, never hidden side channels.

## 7.4 Task Execution Model in C

Because C has no `async/await`, the runtime will use explicit pollable tasks. To prevent this from becoming an unergonomic mess, we mandate:

- **State Machine Ergonomics:** A formalized macro suite (e.g., Protothreads / Duff's Device style `ASX_BEGIN()`, `ASX_YIELD()`, `ASX_END()`) to make writing async C functions structured rather than manual `switch(state)` boilerplate.
- **Memory Ownership:** Explicit memory sizing and finalization for the "opaque state pointer" (the C equivalent of a Rust Future's captured environment). `asx_task_spawn` must accept a `size_t state_size` or an explicit `free_fn` to prevent leaks on region closure.
- **Poll Contract:** task function pointer + opaque state pointer, poll step returns `ASX_POLL_READY` or `ASX_POLL_PENDING` (plus result code).
- **Cancellation Checks:** scheduler drives task state transitions, cancellation is observed at explicit checkpoints via task context API.

## 7.5 Memory and Ownership Strategy

- Arena tables with generation counters for IDs.
- No raw external references to mutable internals.
- Debug-mode invariants on every state transition.
- Optional compile-time feature to poison freed slots in debug builds.
- Explicit ownership docs for each handle type.

## 7.6 Allocator, Reactor, Clock, Entropy, Logging Hooks

All provided via config struct at runtime creation:

- allocator vtable (`malloc/realloc/free` compatible signatures),
- **reactor vtable** (`epoll`/`kqueue`/`IOCP` adapter for multiplexing I/O and waking the scheduler),
- clock function (`now_ns` monotonic),
- entropy callback (`random_u64`) for non-deterministic mode only,
- log/trace sink callback.

Deterministic mode forbids entropy unless seeded deterministic PRNG is configured, and replaces the reactor vtable with a `ghost_reactor` for reproducible I/O readiness replays.

## 7.7 Dual-Format Serialization Strategy (JSON + High-Performance Binary)

The system will support both encodings from day one via a codec abstraction:

- `ASX_CODEC_JSON`:
  - default for fixtures, diffing, debugging, and parity bring-up.
- `ASX_CODEC_BIN`:
  - optimized production transport/storage format.

Design rules:

- same semantic schema for both encodings (single canonical message model),
- runtime toggle by config and compile-time profile,
- no behavior divergence permitted by encoding choice.

Binary codec direction (ANSI C, gRPC-like goals without external dependency):

- length-delimited frames with explicit message type + schema version,
- compact field encoding (varint-style integers, packed fixed fields where useful),
- optional checksum/footer for corruption detection,
- zero-copy decode path for hot payloads where safe.

Performance intent:

- JSON path optimized for observability,
- binary path optimized for throughput/latency and footprint,
- both paths validated by golden cross-encoding roundtrip tests.

## 7.8 Semantic Plane and Resource Plane (Core Embedded Strategy)

To avoid butchering asupersync on constrained hardware, separate concerns explicitly:

- **semantic plane**:
  - lifecycle invariants, cancellation semantics, obligation linearity, determinism contracts.
- **resource plane**:
  - memory ceilings, queue capacities, timer wheel sizing, trace retention, I/O batching strategy.

Rule:

- resource-plane tuning may change performance envelopes only;
- it must never change semantic outcomes, transition legality, or error taxonomy.

## 8. Portability Profiles

## 8.1 Profile `ASX_PROFILE_CORE` (default)

- single-thread deterministic kernel,
- ANSI C core only,
- no OS dependencies.

## 8.2 Profile `ASX_PROFILE_POSIX`

- optional worker threads,
- optional sockets/reactor adapters.

## 8.3 Profile `ASX_PROFILE_WIN32`

- optional worker and I/O backend for Windows.

## 8.4 Profile `ASX_PROFILE_FREESTANDING`

- no filesystem/network assumptions,
- user-supplied platform hooks mandatory,
- static allocation option for extremely constrained targets.

## 8.5 Profile `ASX_PROFILE_EMBEDDED_ROUTER`

- tuned for OpenWrt/BusyBox-class deployments (musl/uClibc-style environments),
- supports low-memory operation via bounded resource contracts and deterministic exhaustion behavior,
- includes lightweight trace sinks (RAM ring buffer first, optional persistent spill),
- keeps full kernel semantics; only defaults and capacities differ from host profiles.

## 8.6 Resource Classes (Capabilities, Not Features)

- `ASX_CLASS_R1` (very constrained):
  - aggressive default memory ceilings, smaller bounded queues/tables, compact trace windows.
- `ASX_CLASS_R2` (constrained):
  - moderate ceilings and deeper buffers.
- `ASX_CLASS_R3` (comfortable embedded/server):
  - wider buffers and richer diagnostics by default.

All classes expose the same API and semantics. Capacity misses must produce deterministic, classified failures rather than silent drops.

## 9. Rust-to-C Conformance Strategy

## 9.1 Golden Fixture Categories

Generate canonical fixtures from Rust for:

- outcome joins and severity ordering,
- budget composition and exhaustion,
- cancel reason and phase transitions,
- task state transition legality,
- region close/quiescence scenarios,
- obligation reserve/commit/abort/leak behavior,
- MPSC reserve/send/abort scenarios,
- timer ordering and cancellation handles,
- allocator-failure interleaving and low-memory recovery scenarios,
- endian/unaligned decode edge cases for binary codec safety.

## 9.2 Fixture Format

Use a canonical fixture schema with two encodings:

- JSON (default during early port phases),
- binary (for performance parity and production-like validation).

Each fixture contains:

- input scenario payload,
- expected transition/event sequence,
- expected final snapshot,
- expected error codes.

Rule:

- JSON and binary fixture variants for the same scenario must hash to the same canonical semantic digest.

## 9.3 C Conformance Runner

- Replays each fixture against ANSI C runtime.
- Compares state/event output deterministically.
- Emits machine-readable parity report.
- Runs both codec modes:
  - JSON baseline,
  - binary mode,
  - and cross-checks semantic equivalence.

## 9.4 Parity Tracking

`FEATURE_PARITY.md` must track each semantic unit:

- `not-started`, `in-progress`, `parity-pass`, `known-delta`, `deferred`.

No phase can close with unknown parity status for in-scope items.

## 10. Testing and Quality Program

## 10.1 Test Layers

- Unit tests per module.
- Invariant tests for lifecycle contracts.
- Scenario tests for cancellation/drain/finalize paths.
- Conformance tests against Rust fixtures.
- Stress tests (queue pressure, cancellation storms, timer churn).

## 10.2 Determinism Tests

- Fixed seed tests must reproduce identical event sequences.
- Same input + same seed = identical output hashes.

## 10.3 Memory Correctness

- Build/test with `ASAN`/`UBSAN` where available.
- Optional leak sanitization in CI for host platforms.
- Internal “handle validity” assertions in debug profile.

## 10.4 Performance Checks

Introduce reproducible microbenchmarks for:

- scheduler dispatch cost,
- timer insertion/cancel/fire cost,
- channel reserve/send throughput,
- quiescence close latency under load.

## 10.5 Proof and Artifact Contract (Mandatory for Alpha Work)

For any hard-part/alpha lever, attach:

- baseline artifact (`p50/p95/p99`, throughput, memory),
- profile artifact (CPU + allocation hotspot evidence),
- isomorphism proof block (ordering/tie-break/seed/error semantics),
- parity artifact (fixture diff or hash equivalence),
- fallback trigger and rollback command.

No artifact pack, no merge.

## 10.6 Promoted Hard Quality Gates

The following gates are mandatory:

- **Portability CI Gate**
  - required compiler/target matrix jobs must pass.
- **OOM/Resource-Exhaustion Gate**
  - deterministic and documented behavior for allocation/time budget exhaustion,
  - explicit boundary tests for exhaustion behavior.
- **Differential Fuzzing Gate (Rust vs C)**
  - continuous scenario mutation against both runtimes for in-scope semantics,
  - failing cases must produce minimized counterexamples and parity diffs.
- **Embedded Target Matrix Gate**
  - required cross-target builds for router-class triplets (mipsel/armv7/aarch64 + musl-style libc) must pass,
  - scenario/replay suite must pass under QEMU for those targets,
  - at least one real-device smoke run must pass before milestone closure.
- **Profile Semantic Parity Gate**
  - `ASX_PROFILE_CORE`, `ASX_PROFILE_FREESTANDING`, and `ASX_PROFILE_EMBEDDED_ROUTER` must emit identical canonical semantic digests for shared fixtures.

## 11. Performance and Footprint Targets

Initial target budgets for kernel profile:

- O(1) amortized enqueue/dequeue on ready/cancel queues.
- O(1) timer cancel by handle generation check.
- No heap allocations on scheduler hot path in steady state.
- Deterministic profile binary footprint suitable for embedded-class deployment.
- Bounded-memory mode must fail deterministically before ceiling breach (no partial mutation).
- Router-class defaults should target practical operation on low-end hardware (e.g., 32-128 MB RAM class) without semantic degradation.

These are validated per phase; thresholds will be refined once first baseline exists.

## 12. Security and Safety Posture in C

Since C cannot rely on Rust’s compiler ownership model:

- state-machine guards are mandatory,
- ID generation checks prevent stale-handle ABA-style misuse,
- unsafe transitions return explicit error codes,
- all pointer ownership paths documented and asserted in debug builds.

No “silent recovery” for invariant violations in debug profile.

## 13. Detailed Phase Plan

## Phase 0: Program Setup and Scope Freeze

Deliverables:

- this plan finalized,
- `EXISTING_ASUPERSYNC_STRUCTURE.md` skeleton created,
- initial `FEATURE_PARITY.md` with kernel rows,
- list of deferred subsystems approved.

Exit criteria:

- scope accepted,
- no unresolved ambiguity about initial kernel boundary.

## Phase 1: Semantic Extraction (Authoritative Spec)

Deliverables:

- exact transition tables:
  - region states,
  - task states/phases,
  - obligation states,
  - cancellation phases.
- exact budget composition rules.
- exact outcome join semantics.
- exact MPSC two-phase behavior definition.
- hard-part mapping table:
  - Rust advantage,
  - C replacement mechanism,
  - parity/invariant test that proves it.
- machine-readable invariant schema (`invariants/*.yaml`) as source of truth for:
  - states,
  - transitions,
  - forbidden transitions,
  - required postconditions.
- explicit forbidden-behavior catalog:
  - “must fail” scenarios and expected error/status outcomes.

Exit criteria:

- implementation can proceed from spec without re-reading Rust source.

## Phase 2: Build + Toolchain + Skeleton

Deliverables:

- build system (`Makefile` first, optional CMake generator),
- compiler matrix scripts,
- portability CI matrix hard gate:
  - GCC + Clang + MSVC,
  - 32-bit + 64-bit targets,
  - endian-variance checks where available,
- embedded target matrix:
  - `mipsel-openwrt-linux-musl`,
  - `armv7-openwrt-linux-muslgnueabi`,
  - `aarch64-openwrt-linux-musl`,
- QEMU harness for router-class scenario and replay tests,
- baseline lint/sanitizer configs,
- codec abstraction interface (`asx_codec_vtable`) with JSON and binary stubs,
- scenario DSL schema + parser stubs for cross-runtime replay/diff tests,
- source tree skeleton and public headers.

Exit criteria:

- clean warnings on target compilers for empty skeleton,
- portability CI matrix green,
- embedded target matrix jobs green.

## Phase 3: `asx_core` Implementation

Deliverables:

- IDs, errors, outcome, budget, cancel types,
- transition checker utilities,
- X-macro driven transition authority tables,
- `asx_cleanup_stack` primitives (foundation for RAII-equivalent behavior),
- debug ghost monitors (`ghost_protocol_monitor`, `ghost_linearity_monitor`),
- strict OOM/resource-exhaustion semantics contract:
  - deterministic failure/status codes,
  - no partial state corruption,
  - explicit rollback/abort behavior at kernel boundaries,
- resource-contract engine primitives:
  - per-runtime ceilings (memory/events/queue depth/timer nodes),
  - admission + backpressure decision points with deterministic error codes,
- unit tests for pure-core semantics.

Exit criteria:

- all core type tests pass,
- fixture parity pass for core semantic fixtures.

## Phase 4: Region/Task/Obligation Tables

Deliverables:

- arena-backed tables with generation-safe handles,
- debug quarantine mode for handle-reuse hardening,
- transition APIs with legality checks,
- obligation reserve/commit/abort/leak detection.

Exit criteria:

- invariant tests pass for leak/quiescence preconditions.

## Phase 5: Runtime Kernel Loop + Timer Wheel

Deliverables:

- scheduler loop,
- cancellation propagation + checkpoint handling,
- deterministic tie-break ordering and stable event sequencing,
- deterministic event hash-chain output for replay identity,
- small-state exhaustive scheduler checker (model-style test harness) for fairness/starvation edge cases,
- optional cycle-budget checkpoints for long-running loops on low-frequency CPUs,
- timer wheel + cancellation handles.

Exit criteria:

- runtime scenario tests pass,
- deterministic trace hashes stable in CI.

## Phase 6: Channel Kernel (MPSC two-phase)

Deliverables:

- bounded MPSC implementation,
- reserve/send/abort semantics,
- FIFO and wakeup correctness tests.

Exit criteria:

- conformance parity pass for channel fixtures.

## Phase 7: Trace/Replay Minimum Viable Layer

Deliverables:

- deterministic event emission format,
- replay input mode for kernel scenarios,
- Rust↔C twin-run differential oracle for in-scope fixtures,
- dual-codec fixture support (JSON and binary) with canonical equivalence checks,
- differential fuzzing harness (Rust vs C) driven by scenario DSL mutations,
- deterministic counterexample minimizer that shrinks failing traces/scenarios,
- embedded trace modes:
  - RAM-ring default for low-flash targets,
  - wear-aware persistent spill mode with bounded write cadence,
- snapshot/export hooks needed for conformance.

Exit criteria:

- replay of golden fixtures reproduces final snapshots exactly.

## Phase 8: Parallel Profile (Optional, guarded)

Deliverables:

- worker model,
- ready/cancel/timed lane scheduling rules,
- work-stealing with bounded fairness controls,
- optional `ALPHA-5`/`ALPHA-6` tracks behind feature flags:
  - seqlock metadata path,
  - EBR/hazard reclamation path.

Exit criteria:

- no regression in deterministic single-thread mode,
- fairness tests pass for enabled parallel profile,
- fallback paths (`rwlock`, deferred-free) are parity-verified.

## Phase 9: Selective Higher-Surface Ports

Deliverables:

- prioritized subset only (decided after kernel maturity),
- each subsystem has spec + conformance before code lands.

Exit criteria:

- subsystem passes parity gates before broadening scope further.

## 14. Risk Register and Mitigations

## Risk 1: Scope Explosion

Problem:

- Rust surface is enormous; naive parity goal will stall.

Mitigation:

- strict wave gating and deferred surfaces list.

## Risk 2: C Safety Regressions

Problem:

- memory and state bugs are easier in C.

Mitigation:

- handle generation checks, debug assertions, sanitizer CI, explicit ownership docs.

## Risk 3: Determinism Drift

Problem:

- replay parity can drift as runtime grows.

Mitigation:

- deterministic fixture hashing gate in CI for each phase.

## Risk 4: Portability Debt

Problem:

- accidental POSIX assumptions in core.

Mitigation:

- keep `asx_core` and kernel profile platform-neutral; isolate adapters.

## Risk 5: Performance Regressions From Over-Defensive Checks

Problem:

- safety checks can bloat hot paths.

Mitigation:

- compile-time debug/prod assertion levels and benchmark gates.

## Risk 6: Adaptive-Controller Complexity Regression

Problem:

- adaptive logic can become opaque and violate replay confidence.

Mitigation:

- expected-loss + evidence-ledger contract,
- deterministic fallback mode always available,
- counterfactual logging for decision explainability.

## Risk 7: Spec/Implementation Drift

Problem:

- prose plan/spec diverges from actual code and tests over time.

Mitigation:

- machine-readable invariant schema as the canonical source,
- generated transition tests from schema,
- CI gate that fails on schema/test/code drift.

## Risk 8: Resource-Exhaustion Undefined Behavior

Problem:

- OOM/time-budget exhaustion can cause partial updates and latent corruption in C.

Mitigation:

- strict exhaustion semantics contract,
- per-boundary failure-atomicity tests,
- deterministic failure/status codes with explicit rollback behavior.

## Risk 9: Embedded Feature Erosion

Problem:

- pressure to “simplify for routers” can silently remove asupersync differentiators.

Mitigation:

- enforce semantic-plane/resource-plane split,
- require cross-profile semantic digest equivalence in CI,
- block merges that improve footprint by changing lifecycle/cancellation/linearity behavior.

## 15. Definition of Done (Kernel Milestone)

Kernel milestone is complete when all are true:

- core has zero external deps,
- single-thread runtime kernel implemented,
- semantic invariants enforced by tests,
- fixture parity pass for all kernel items,
- deterministic replay hash stability in CI,
- guarantee-substitution matrix rows all mapped to implemented mechanisms or explicitly deferred,
- portability CI matrix gate green,
- strict OOM/resource-exhaustion suite green,
- differential fuzzing parity gate green,
- embedded target matrix gate green (build + QEMU + device smoke),
- cross-profile semantic parity digests match for shared fixture set,
- `FEATURE_PARITY.md` marks kernel rows `parity-pass`.

## 16. Immediate Work Plan (Next 16 Tasks)

1. Create `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` with exact state-machine tables.
2. Add a Rust->C guarantee-substitution table in `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` with proof artifacts per row.
3. Create `docs/FEATURE_PARITY.md` with kernel rows and acceptance tests.
4. Create `docs/EMBEDDED_TARGET_PROFILES.md` with router-class resource classes (`R1/R2/R3`) and semantic parity policy.
5. Scaffold `include/asx` and `src` with module boundaries from section 7.
6. Implement `asx_status`, `asx_outcome`, `asx_budget`, `asx_cancel_reason`.
7. Implement and test task/region/obligation transition validators before full runtime loop.
8. Implement debug ghost monitors for protocol/linearity checks.
9. Implement strict OOM/resource-exhaustion contract and failure/status taxonomy in core APIs.
10. Add resource-contract ceilings and admission/backpressure APIs for memory/queue/timer nodes.
11. Add per-boundary exhaustion tests (arena alloc, queue growth, timer ops, obligation ops).
12. Build Rust fixture capture tool for core semantic scenarios.
13. Add conformance runner that compares C outputs to Rust fixtures (JSON first, then binary equivalence).
14. Add cross-profile semantic digest checks (`CORE` vs `FREESTANDING` vs `EMBEDDED_ROUTER`) for shared fixtures.
15. Add router target cross-compile jobs + QEMU scenario runs for mipsel/armv7/aarch64.
16. Add Rust↔C differential fuzzing harness with automatic counterexample minimization.

## 17. Open Decisions Requiring Owner Confirmation

Resolved decisions (owner-provided):

1. C dialect baseline:
   - C99 is allowed where it materially simplifies implementation.
2. Trace/fixture encoding strategy:
   - JSON first for easier comparisons,
   - architecture must cleanly toggle to a hyper-optimized binary format.

Remaining open decisions:

1. Optional parallel profile requirement timing:
   - include in first public milestone, or
   - defer until kernel profile is fully stable?
2. Embedded memory mode:
   - dynamic allocation only initially, or
   - static arena mode required in first milestone?

## 18. Companion Documents Required Next

- `docs/EXISTING_ASUPERSYNC_STRUCTURE.md`
- `docs/PROPOSED_ANSI_C_ARCHITECTURE.md`
- `docs/FEATURE_PARITY.md`

This plan is intentionally strict: no implementation phase opens without spec and parity gates for that phase.

## 19. Idea-Wizard Quality Multipliers (30 -> 5 + 10)

This section captures an explicit idea-wizard pass focused on maximizing end-product quality and preserving asupersync’s differentiators in ANSI C.

## 19.1 Top 5 Ideas (Best -> Worst)

1. **Machine-Readable Invariant Spec + Generated Tests**
   - Why: prevents semantic drift and turns invariants into executable contracts.
   - Impact: highest reliability gain per unit effort.
   - Plan integration: Phase 1 + CI drift gate.

2. **Forbidden-Behavior Catalog (“Must Fail” Matrix)**
   - Why: high-quality systems specify what must be rejected, not just what works.
   - Impact: closes ambiguity around safety boundary behavior.
   - Plan integration: Phase 1 deliverable + conformance fixtures.

3. **Scenario DSL Shared by Rust and C**
   - Why: avoids hand-written, divergent tests and enables large differential suites.
   - Impact: dramatically improves parity confidence and test reuse.
   - Plan integration: Phase 2 deliverable.

4. **Deterministic Counterexample Minimizer**
   - Why: hard bugs become tractable only when failing traces are shrunk.
   - Impact: faster debugging, stronger confidence in fixes.
   - Plan integration: Phase 7 deliverable and hard gate.

5. **Small-State Exhaustive Scheduler Checker**
   - Why: catches starvation/fairness corner cases that random tests miss.
   - Impact: high assurance on the most fragile runtime logic.
   - Plan integration: Phase 5 deliverable.

## 19.2 Next Best 10 Ideas

Promoted to hard gates in this revision:

- #6 Portability CI Matrix
- #7 Strict OOM/Resource-Exhaustion Semantics
- #8 Differential Fuzzing Harness (Rust vs C)

6. **Portability CI Matrix as First-Class Gate**
   - GCC/Clang/MSVC + 32/64-bit + endian variants where available.

7. **Strict OOM/Resource-Exhaustion Semantics**
   - deterministic behavior contracts for allocation/time budget exhaustion.

8. **Differential Fuzzing Harness (Rust vs C)**
   - mutate scenarios and assert canonical semantic equivalence.

9. **Canonical Error Taxonomy + Stability Policy**
   - stable error families and evolution rules from day one.

10. **Safety Profiles (`debug-ghost`, `hardened`, `release-fast`)**
   - explicit guarantees and overhead envelopes per profile.

11. **ABI/API Stability Contract for C Consumers**
   - versioning, compatibility checks, and breaking-change process.

12. **Golden Performance SLO Gates**
   - p50/p95/p99 thresholds and regression budgets tied to baselines.

13. **Deterministic Time and Entropy Fault-Injection Modes**
   - simulate clock skew/stall and entropy failures in controlled tests.

14. **Evidence-Ledger Regression Dashboard**
   - track confidence/fallback rates and adaptive-controller behavior over time.

15. **Plan-to-Execution Traceability Index**
   - each plan claim maps to tests, fixtures, and code modules.

## 19.3 Alien Architecture Multipliers (New in Rev 2)

Extracted from extreme optimization and alien graveyard catalogs to replace missing Rust compile-time constraints:

16. **Formal Assurance Ladder (§0.11)**
    - Implement a structured progression from Golden Outputs -> Property Tests -> Bounded Model Checking -> Translation Validation (§6.14) to replace Rust's static borrow checker confidence without requiring a fully verified C compiler.
17. **LCRQ / Wait-Free Ring Queues (§14.6)**
    - For the core MPSC two-phase channel in the parallel profile, use a Fetch-and-add + CAS ring queue to beat standard lock-based queues or naive linked lists, achieving wait-free bounds on the hottest inter-task messaging path.
18. **Cache-Oblivious Task Arenas (§7.2)**
    - Structure the global `Σ = {regions, tasks, obligations}` arenas using a Van Emde Boas recursive memory layout or implicit blocking to ensure the scheduler traverses tasks optimally regardless of the target embedded processor's L1/L2 cache sizes.
19. **SOS Barrier Certificates (§0.4 Math Extension)**
    - Synthesize offline admissibility bounds for the adaptive cancellation scheduler, guaranteeing the runtime will never violate worst-case starvation constraints when falling back from adaptive mode.

## 19.4 Operational Rule

Any idea from this section becomes “active” only when:

- added to phase deliverables,
- attached to explicit tests/artifacts,
- and gated in CI.

Otherwise it remains a backlog idea, not a quality guarantee.

## 20. Idea-Wizard Round 2: Embedded Device Maximization (30 -> 5 + 10)

This round is optimized for low-cost embedded deployments (cheap routers and similar) while preserving full feature semantics and core architecture.

## 20.1 Top 5 Ideas (Best -> Worst)

1. **Semantic Plane / Resource Plane Contract (No-Compromise Backbone)**
   - Why: this is the only reliable way to optimize for tiny hardware without semantic drift.
   - Value: keeps asupersync special while still enabling aggressive footprint tuning.
   - Integration: sections 7.8, 8.6, 10.6 parity gate.

2. **Router-Class Target Matrix + Hardware Truth Gate**
   - Why: embedded usefulness is fake unless validated on real target ABIs and runtimes.
   - Value: prevents host-biased assumptions; catches libc/alignment/endian pathologies early.
   - Integration: Phase 2 embedded matrix + 10.6 embedded gate + DoD.

3. **Deterministic Resource-Contract Engine**
   - Why: constrained environments fail under pressure unless memory/queue/timer ceilings are first-class semantics.
   - Value: fail-fast, failure-atomic behavior under stress instead of latent corruption.
   - Integration: Phase 3 deliverables + Risk 8/9 mitigations.

4. **Wear-Aware Diagnostics Pipeline**
   - Why: embedded operators need diagnostics, but flash write amplification can kill devices.
   - Value: preserves observability with RAM-ring first and bounded persistent spill.
   - Integration: Phase 7 embedded trace modes.

5. **Cross-Profile Semantic Digest Equivalence**
   - Why: prevents accidental “embedded-lite” forks over time.
   - Value: mechanical proof that profile differences are operational, not behavioral.
   - Integration: hard gate in 10.6 + task #14.

## 20.2 Next Best 10 Ideas

6. **OpenWrt Packaging Kit (No deps, reproducible build recipes)**
7. **Interrupt/Jitter Fault Injection Scenarios for timer/cancel behavior**
8. **Compile-time memory layout report + budget delta checker per target**
9. **Zero-copy binary codec fast path with strict equivalence oracle**
10. **Endian + unaligned-access hardening suite for binary transport**
11. **Power-loss simulation for trace/snapshot durability boundaries**
12. **Low-overhead remote diagnostics framing for narrow-band links**
13. **Per-target binary size and cold-start SLO gates**
14. **Router workload benchmark corpus (burst traffic + cancellation storms)**
15. **Deployment hardening playbook (watchdog, restart semantics, safe upgrades)**

## 20.3 Promoted in This Revision

- Top #1, #2, #3, #4, and #5 are promoted into core plan sections and gates in this revision.

## 20.4 Operational Rule

Embedded optimizations are accepted only if:

- canonical semantic digest parity remains intact across profiles,
- failure-atomicity and deterministic error semantics are preserved,
- and measured embedded target gains are demonstrated (latency/throughput/footprint/power proxy).
