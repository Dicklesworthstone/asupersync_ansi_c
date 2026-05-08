# Deferred-Surface Register

> Explicit registry of all surfaces, features, and capabilities deferred from the initial (Wave A/B kernel) release.
> Bead: `bd-296.24`
> Inputs: Plan Sections 2.4.1, 4, 5; ADR-001 through ADR-004 (docs/OPEN_DECISIONS_ADR.md)

---

## Purpose

This register ensures that deferred work is:

1. **Explicit** — no feature is silently dropped; every deferral is a conscious, documented choice.
2. **Bounded** — each item has concrete unblock criteria, not open-ended "someday" status.
3. **Traceable** — each item links to the rationale, ADR, and downstream beads it affects.
4. **Risk-aware** — deferrals that could silently mutate kernel semantics are flagged.

---

## Deferral Categories

| Category | Count | Primary Rationale |
|----------|-------|------------------|
| Wave C — Selected Systems Surfaces | 3 | Kernel stabilization first |
| Wave D — Advanced Surfaces | 6 | Massive scope; kernel focus |
| Parallel Profile | 1 | ADR-001 started as post-kernel deferral; `bd-pweu` graduation is active |
| Static Arena Backend | 1 | ADR-002: dynamic default + vtable prep |
| Rust-C Interoperability | 3 | Plan Section 2.4.1: companion, not bridge |
| Explicit Exclusions | 3 | Plan Section 5: out of scope for first release |

---

## Wave C — Selected Systems Surfaces (Post-Kernel)

### DS-C01: Targeted Networking Primitives

**Status:** Wave C contract active under `bd-v12u.9`; first native socket/reactor e2e remains `bd-v12u.10`.
**Rationale:** Networking requires platform adapters (POSIX sockets, Win32 IOCP, embedded poll/select), reactor integration, and connection lifecycle management that are outside the kernel's semantic scope. Kernel parity must be proven before adding I/O surfaces.
**Rust source:** `src/net/` (~26k LOC)
**Activation beads:** `bd-v12u.9` (Wave C networking primitive spec), `bd-v12u.10` (first deterministic network reactor/socket e2e)
**Unblock criteria:**
- Wave A quality gates green (all kernel conformance/parity/embedded gates pass)
- Wave B conformance harness operational (differential fixture runner working)
- Reactor/event-loop adapter interface designed in `docs/PROPOSED_ANSI_C_ARCHITECTURE.md`
- POSIX native readiness evidence from `bd-v12u.4` remains linked before live sockets claim reactor-backed behavior
- Dedicated spec extraction for networking semantics (`bd-v12u.9`) is complete before `bd-v12u.10` expands native sockets
**Owner:** TBD (assigned when Wave C opens)
**Dependency path:** Wave A gates -> Wave B harness -> POSIX/native readiness -> networking spec -> native socket e2e -> implementation
**Semantic risk:** LOW — networking does not change kernel lifecycle/cancel/obligation semantics. Uses kernel primitives.

**Wave C primitive boundary:** Wave C owns bounded, runtime-level networking primitives only: socket-address value handling, deterministic resolver ordering/cache behavior, in-memory TCP listener/stream lifecycle, UDP datagram delivery, anonymous pipes, resource-cap enforcement, and fail-closed native readiness handoff. These primitives may use kernel regions, cancellation, tasks, timers, and obligations, but they must not alter their externally visible semantics.

**Wave D exclusion boundary:** HTTP/2, gRPC, TLS, database clients, distributed runtime protocols, remote regions, connection pooling policies, and protocol-specific flow-control semantics remain Wave D or later. A Wave C networking pass cannot count any of those application/protocol stacks as completed evidence.

**Fixture obligations:** Wave C networking evidence must cover:
- lifecycle: bind, connect, accept, read, write, datagram send/receive, close, reset, and stale-handle rejection;
- cancellation and backpressure: pending accepts/reads, closed peers, bounded pipe/socket buffers, queue-full behavior, and no silent data loss;
- resource exhaustion: listener, stream, UDP socket, pipe, resolver result, and cache caps fail atomically without partially live handles;
- unsupported platform diagnostics: profiles without live socket support fail closed and cannot report a live capability;
- semantic drift: shared fixture digests across CORE/POSIX/PARALLEL profiles remain unchanged unless an approved semantic-delta record exists.

Current executable coverage is `make test-e2e-network-surface` plus focused `tests/unit/net/test_net` and `tests/unit/net/test_pipe` unit binaries. `bd-v12u.10` extends this contract to the first reactor-backed native socket e2e without changing the kernel digest contract.

### DS-C02: Selected Combinators

**Status:** Wave C contract active under `bd-v12u.11`; actor/supervision harness expansion remains `bd-v12u.12`.
**Rationale:** Combinators (join, race, select, timeout, retry) compose kernel primitives. They require a stable task/region/cancellation substrate before they can be faithfully ported.
**Rust source:** `src/combinator/` (~17k LOC)
**Activation beads:** `bd-v12u.11` (combinator parity contracts and conformance fixture expansion)
**Unblock criteria:**
- Wave A task lifecycle, cancellation protocol, and obligation resolution stable
- Combinator semantic spec extracted from Rust behavior and current C surfaces
- Dedicated test suites with deterministic replay verification (`test-combinator-contract`, `make conformance`)
**Owner:** TBD
**Dependency path:** Wave A kernel -> combinator spec extraction -> parity fixtures -> actor/supervision harnesses
**Semantic risk:** MEDIUM — combinators interact deeply with cancellation propagation and outcome aggregation. Incorrect port could violate outcome lattice join semantics.

**Wave C combinator boundary:** Wave C covers fixed-capacity, allocation-free combinator state machines and service-layer wrappers already represented in C: join, race, select, timeout, first-ok, quorum, retry, bracket, pipeline, bulkhead, rate-limit, hedge, map-reduce, JoinSet, plan DAG join/race/timeout rewrites, and service middleware for timeout, retry, rate-limit, buffering, concurrency limit, map, filter, and load-shed. The contract is behavioral parity for these primitives, not broad Rust macro/API parity.

**Wave D / later exclusion boundary:** Full actor supervision policy, restart escalation, distributed actor routing, protocol stack orchestration, and user-facing macro ergonomics are not completed by this bead. `bd-v12u.12` owns actor/supervision fixture expansion after the combinator contract is stable.

**Fixture obligations:** Wave C combinator evidence must cover:
- cancellation: race, select, quorum, race-timeout, and retry-timeout drain/cancel losers without weakening cancellation severity or leaking unfinished branches;
- outcome aggregation: join and JoinSet preserve the outcome lattice and deterministic completion identity;
- ordering: select fairness rotation, same-input race winners, plan DAG associativity, and timeout-min rewrites are deterministic;
- resource/budget caps: branch, pipeline-stage, JoinSet, bulkhead, rate-limit, retry-attempt, and service-builder capacities fail closed without partial admission;
- cleanup: bracket release runs after successful acquire even when use fails, while acquire failure does not release an unowned resource;
- semantic drift: shared fixtures under CORE/PARALLEL and JSON/BIN keep identical semantic digests unless an approved semantic-delta record exists.

Current executable coverage is `make test-combinator-contract` plus the `fixtures/rust_reference/core_combinator/combinator-contract-001.*` conformance family. Unsupported depth remains explicitly outside the contract until a downstream bead maps it to source artifacts and fixtures.

### DS-C03: Selected Observability Surfaces

**Status:** Deferred to Wave C
**Rationale:** Observability (metrics, spans, structured logging beyond trace) builds on the trace/replay layer (Wave B). Adding observability before trace is stable would create untested instrumentation.
**Rust source:** Parts of `src/trace/` (~36k LOC), observability layers
**Activation beads:** `bd-v12u.7` (operator incident bundle), `bd-v12u.8` (deterministic counterexample minimizer), `bd-v12u.13` (versioned Rust/C trace schema)
**Unblock criteria:**
- Wave B trace/replay layer operational
- Trace format and semantic digest stable
- Observability adapter interface designed
**Owner:** TBD
**Dependency path:** Wave B trace -> observability spec -> implementation
**Semantic risk:** LOW — observability is read-only instrumentation; does not alter runtime behavior.

### Wave C Activation Contract (`bd-v12u.1`)

Wave C activation is gated by evidence, not by source-tree presence. The first
contract bead, `bd-v12u.1`, binds every newly opened Wave C child to a concrete
surface, test obligation, and no-drift rule.

| Surface | Activation beads | Required proof before implementation claims |
|---|---|---|
| Native adapter deterministic commit authority | `bd-v12u.2`, `bd-v12u.3`, `bd-v12u.4` | Native POSIX/Win32 hooks must preserve the same externally visible scheduler, channel, timer, cancellation, and trace commit order as CORE/PARALLEL logical mode, or fail closed. |
| Static arena backend | `bd-v12u.5`, `bd-v12u.6` | Static and dynamic allocator backends must produce identical semantic digests for shared fixtures; OOM and post-seal failures must be failure-atomic. |
| Observability and replay evidence | `bd-v12u.7`, `bd-v12u.8`, `bd-v12u.13` | Incident bundles, trace schema changes, and minimized counterexamples must be read-only evidence surfaces and must not alter runtime behavior. |
| Networking primitives | `bd-v12u.9`, `bd-v12u.10` | `test-e2e-network-surface`, focused net/pipe unit tests, and `profile-parity` must cover lifecycle, cancellation, backpressure, resource caps, and unsupported-profile diagnostics. `bd-v12u.10` owns the first native socket/reactor e2e and must not introduce kernel digest drift. |
| Combinators and actor/supervision harnesses | `bd-v12u.11`, `bd-v12u.12` | `test-combinator-contract`, `make conformance`, `codec-equivalence`, and `profile-parity` must pin outcome aggregation, cancellation propagation, timeout ordering, retry/budget caps, bracket cleanup, and unsupported actor/supervision depth before parity claims expand. |
| Overload SLOs and user acceptance | `bd-v12u.14`, `bd-v12u.15` | Performance/admission thresholds must be resource-plane only; final demos must include replay, profile compatibility, incident evidence, and fail-closed messaging. |

No Wave C child may close on a proxy signal alone. Passing tests, complete
manifests, or generated reports count only when they directly cover the
surface-specific obligations above. Expensive gates must run through
`rch exec -- ...`.

---

## Wave D — Advanced Surfaces (Significantly Deferred)

### DS-D01: Full HTTP/2 and gRPC Parity

**Status:** Deferred to Wave D
**Rationale:** HTTP/2 framing, HPACK, gRPC protobuf handling, and TLS integration represent massive surface area (~23k LOC for HTTP alone). Porting before kernel and networking are stable would create unmaintainable code.
**Rust source:** `src/http/` (~23k LOC), gRPC layers
**Unblock criteria:**
- Wave C networking primitives stable
- HTTP/2 semantic spec extracted (framing, flow control, stream lifecycle)
- TLS integration strategy decided (external lib vs. embedded)
- gRPC protobuf strategy decided (external codegen vs. manual)
**Owner:** TBD
**Dependency path:** Wave C networking -> HTTP/2 spec -> TLS decision -> implementation
**Semantic risk:** LOW for kernel — HTTP operates above the runtime kernel. However, incorrect stream lifecycle could leak obligations.

### DS-D02: Full Database Client Parity

**Status:** Deferred to Wave D
**Rationale:** Database clients are application-layer functionality dependent on networking and codec layers.
**Rust source:** Database-related modules
**Unblock criteria:**
- Wave C networking stable
- Database protocol spec extracted
- Connection pool lifecycle integrated with region/obligation model
**Owner:** TBD
**Dependency path:** Wave C networking -> database spec -> implementation
**Semantic risk:** LOW for kernel.

### DS-D03: Full Distributed/Remote Parity

**Status:** Deferred to Wave D
**Rationale:** Distributed runtime (remote regions, distributed cancellation, consensus) is the highest-complexity surface and requires stable kernel + networking + trace.
**Rust source:** `src/distributed/` and related modules
**Unblock criteria:**
- Wave C networking + trace stable
- Distributed protocol spec extracted
- Cross-node cancellation and obligation propagation semantics defined
- Dedicated conformance fixtures for distributed scenarios
**Owner:** TBD
**Dependency path:** Wave C -> distributed spec -> implementation + distributed parity fixtures
**Semantic risk:** HIGH — distributed cancellation and obligation propagation could introduce semantic divergence if not carefully specified.

### DS-D04: Full Advanced Trace/Lab Topology Stack

**Status:** Deferred to Wave D
**Rationale:** Advanced trace topology (DAG visualization, causal analysis, distributed trace correlation) builds on basic trace (Wave B) and observability (Wave C).
**Rust source:** Parts of `src/trace/` (~36k LOC), `src/lab/` (~37k LOC)
**Unblock criteria:**
- Wave B basic trace + replay stable
- Wave C observability surfaces implemented
- Advanced trace spec extracted
**Owner:** TBD
**Dependency path:** Wave B trace -> Wave C observability -> advanced trace spec -> implementation
**Semantic risk:** LOW — trace is read-only instrumentation.

### DS-D05: Full RaptorQ + Advanced Policy Stack

**Status:** Deferred to Wave D
**Rationale:** RaptorQ (fountain codes for erasure coding) and advanced policy engines are specialized surfaces not needed for kernel parity.
**Rust source:** `src/raptorq/` (~18k LOC)
**Unblock criteria:**
- Kernel stable
- RaptorQ semantic spec extracted
- Performance-critical codec implementation verified on embedded targets
**Owner:** TBD
**Dependency path:** Kernel stable -> RaptorQ spec -> implementation
**Semantic risk:** LOW for kernel.

### DS-D06: Full Erlang/OTP-Style Actor and Supervision Tree

**Status:** Deferred to Wave D
**Rationale:** The actor/supervision layer (~28k LOC) is a significant subsystem that builds on top of the kernel's region/task/obligation model. It requires stable kernel semantics before faithful porting.
**Rust source:** `src/actor/`, `src/supervision/` (~28k LOC combined)
**Activation harness bead:** `bd-v12u.12` covers deterministic actor/supervision semantics without claiming full OTP parity.
**Unblock criteria:**
- Wave A kernel lifecycle, cancellation, and obligation semantics stable
- Actor/supervision semantic spec extracted (gen_server, supervisor strategies, child specs, restart policies)
- Dedicated test suites for supervision tree behavior under failure
- Integration with deterministic replay for supervision scenario reproduction
**Owner:** TBD
**Dependency path:** Wave A kernel -> actor/supervision spec -> implementation + parity fixtures
**Semantic risk:** MEDIUM — supervision tree restart policies interact with cancellation and obligation resolution. Incorrect port could violate escalation semantics.

---

## Parallel Profile

### DS-P01: Optional Parallel Profile (Worker Model, Work-Stealing, Lane Scheduling)

**Status:** Graduation evidence complete under `bd-pweu`; core logical-worker scheduler, parity gates, large-swarm e2e, telemetry, locality routing, memory-model proofs, and RCH-backed benchmark baselines have landed. Remaining broad live-execution claims depend on native platform adapters preserving deterministic commit authority before executing lanes concurrently by default.
**Rationale:** Kernel correctness is the foundation. Parallel profile adds atomics, synchronization adapters, EBR/hazard pointers (ALPHA-5/6), work-stealing, and fairness validation -- doubling the verification surface. The three primary target verticals (HFT, automotive, embedded router) still require deterministic single-worker fallback behavior.
**ADR:** ADR-001 (docs/OPEN_DECISIONS_ADR.md)
**Beads:** `bd-2cw.7` (closed compile/simulation scaffold), `bd-pweu` (production graduation epic), closed children `bd-pweu.1`-`.15` for the contract/scheduler/atomic/MPSC/POSIX/parity/e2e/locality/proof/benchmark/docs slices.
**Native-adapter follow-up beads:** `bd-v12u.2` (deterministic commit authority), `bd-v12u.3` (Win32 hooks), `bd-v12u.4` (POSIX timed reactor readiness)
**Unblock criteria:**
- Landed: Wave A kernel quality gates green.
- Landed: deterministic worker-lane scheduling, bounded work stealing, timed-lane/waker/reactor readiness pumping, and replay-stable commit authority.
- Landed: portable atomics, seqlock/EBR metadata coverage, atomic two-phase MPSC publication, and memory-model litmus/codegen gates.
- Landed: parallel-specific gates for single-vs-multi-worker digest parity, telemetry/admission/locality evidence, large-swarm e2e logs, formal proofs, and benchmark baselines.
- Landed under `bd-v12u.2`: `parallel-parity` emits commit-authority records
  that compare the global replay-stable commit sequence against per-worker
  commit totals and fail the gate on drift. POSIX/Win32 live native adapters
  still report fail-closed until their child beads enable hooks.
- Landed under `bd-v12u.3`: Win32 hook installation exposes QPC clock and
  deterministic-safe entropy selection, keeps socket/IOCP registration and
  blocking hooks explicitly unsupported, and adds `test-win32-hooks-build` as
  the available mingw compile lane for the adapter contract.
- Landed under `bd-v12u.4`: POSIX reactor fd registration exposes the timed
  readiness seam behind explicit capability macros, rejects bad descriptors and
  invalid interest masks fail-closed, and adds `test-e2e-posix-adapter` evidence
  that pipe readiness wakes the runtime I/O driver, waker registry, timed lane,
  and parallel scheduler without commit-order drift.
- Remaining: native platform adapters must preserve deterministic commit authority before enabling concurrent lane execution by default.
**Owner:** `SilverFrog` started contract planning in `bd-pweu.1`; implementation ownership remains per-child bead
**Dependency path:** Wave A gates -> parallel contract -> scheduler/atomic/channel/platform gates -> e2e/proof/locality evidence -> benchmark baselines -> native adapter concurrency proof
**Semantic risk:** HIGH if not carefully handled — parallel scheduling must produce identical semantic outcomes to single-thread mode for deterministic scenarios. Cross-profile digest parity is mandatory.
**Rollback trigger:** Any semantic digest drift, unclassified event-order drift, data-race finding, silent drop, or benchmark/admission evidence that contradicts production-scale claims.

#### Production Graduation Contract (`bd-pweu.1`)

The production parallel profile graduates only by evidence. As of 2026-05-08,
`src/runtime/parallel.c` is no longer only a walking-skeleton simulation: it
ships deterministic logical-worker routing, bounded steal accounting, timed
lane promotion, reactor/waker pumping, drain states, replay-stable commit
counters, telemetry/admission evidence, and parity/proof gates. The remaining
claim boundary is performance and platform execution breadth, not the existence
of the logical scheduler.

- **Capacity target:** at least 64 logical workers must be representable and
  validated. Profile/resource-class defaults may still cap smaller targets, but
  the generic parallel profile cannot keep a hard 4-worker ceiling.
- **Semantic contract:** worker count, work stealing, lane placement, reactor
  wakeups, and queue backend selection are resource-plane mechanics. Shared
  deterministic fixtures must produce the same canonical semantic digest under
  single-worker and multi-worker execution unless an explicit semantic delta is
  approved through the existing exception workflow.
- **Replay contract:** concurrent workers may poll in parallel, but externally
  visible scheduler, lifecycle, cancellation, channel, timer, and trace events
  must commit through a deterministic sequence authority.
- **Safety contract:** no silent drops, unbounded queues, data races, weakened
  cancellation, stale-handle mutation, or allocator use after
  `asx_runtime_seal_allocator()` unless the behavior is explicitly configured,
  documented, and covered by tests.
- **Fallback contract:** CORE, FREESTANDING, EMBEDDED_ROUTER, HFT, AUTOMOTIVE,
  BROWSER, POSIX, and WIN32 must either provide the documented live-mode hooks
  or fail closed / fall back to a tested single-worker path without semantic
  drift.
- **Gate contract:** `bd-pweu.9`, `bd-pweu.11`, `bd-pweu.12`, `bd-pweu.13`, and
  `bd-pweu.14` provide the parallel parity, large-swarm e2e, memory-model
  proof, locality, and benchmark-baseline gates for semantic and operator
  claims. Native platform adapters remain responsible for preserving the same
  deterministic commit authority before concurrent lane execution is enabled
  by default. All expensive verification must run through `rch exec -- ...`.

---

## Static Arena Backend

### DS-S01: Static Arena Memory Backend (Freestanding/Automotive Targets)

**Status:** Interface designed; implementation deferred per ADR-002
**Rationale:** Primary embedded target (EMBEDDED_ROUTER / OpenWrt) has `malloc`. Arena tables already use allocator vtable, which can be swapped to static memory without API changes. Static arena is a different allocator backend, not a different architecture.
**ADR:** ADR-002 (docs/OPEN_DECISIONS_ADR.md)
**Activation beads:** `bd-v12u.5` (static arena sizing/seal contract), `bd-v12u.6` (static backend implementation and parity)
**What ships in Wave A:**
- Arena tables with `malloc`-based allocator backend
- Allocator vtable interface designed for static-arena forward compatibility
- `asx_runtime_seal_allocator` hook (post-init allocation detection)
**Unblock criteria:**
- Allocator vtable interface proven stable via Wave A usage
- Automotive or freestanding adopter demand signal
- Static arena sizing API designed (max regions, max tasks, max obligations per resource class)
- Cross-profile parity verified with static backend (same semantic digests)
**Owner:** TBD (assigned at automotive-specific milestone)
**Dependency path:** Wave A vtable design -> demand signal -> static arena spec -> implementation + parity
**Semantic risk:** LOW — allocator backend is resource-plane. Semantic behavior must be identical regardless of backend. Profile parity gate enforces this.
**Rollback trigger:** Automotive or freestanding adopter needs static mode before Wave B.

---

## Rust-C Interoperability

### DS-R01: Rust-to-C FFI Bridge

**Status:** Deferred per Plan Section 2.4.1
**Rationale:** The C port is a companion implementation, not a replacement. Shared contract is semantic spec + fixture corpus, not API-level interoperability. FFI bridge adds maintenance burden and ABI stability constraints that are premature for kernel milestone.
**Unblock criteria:**
- Kernel parity proven via conformance suite
- C API stable (post-API-stability milestone)
- FFI use case identified with concrete user demand
- ABI stability contract defined (docs/API_ABI_STABILITY.md)
- Dedicated FFI test suite with sanitizer coverage
**Owner:** TBD
**Dependency path:** Wave A+B kernel stable -> API stability -> FFI spec -> implementation
**Semantic risk:** MEDIUM — FFI boundaries are common sources of ownership, lifetime, and error-handling bugs. Must not introduce UB or violate kernel contracts.

### DS-R02: Cross-Language Channel Interop

**Status:** Deferred per Plan Section 2.4.1
**Rationale:** Sharing channels between Rust and C runtimes requires compatible wire format, shared memory semantics, and cross-language cancellation propagation. Premature before either runtime's channel semantics are stable.
**Unblock criteria:**
- Both Rust and C channel implementations stable
- Shared channel wire format/protocol specified
- Cross-language cancellation propagation semantics defined
- Dedicated interop test suite with both runtimes
**Owner:** TBD
**Dependency path:** Wave A channel stable -> interop spec -> shared wire format -> implementation
**Semantic risk:** HIGH — cross-runtime channel interop could introduce obligation/cancellation propagation bugs.

### DS-R03: Shared Event Schema Constraints

**Status:** Deferred per Plan Section 2.4.1 (noted as consideration)
**Rationale:** Event schema compatibility between Rust and C trace formats enables cross-runtime analysis. Not needed for kernel parity, which uses fixture-based conformance.
**Unblock criteria:**
- Wave B trace format stable in both runtimes
- Schema versioning strategy defined
- Cross-runtime trace correlation use case identified
**Owner:** TBD
**Dependency path:** Wave B trace -> schema versioning -> cross-runtime correlation
**Semantic risk:** LOW — schema is a data format concern, not a runtime behavior concern.

---

## Explicit Exclusions (Plan Section 5)

### DS-X01: Full Rust-Surface Parity Across All 500+ Modules

**Status:** Explicitly excluded from all planned waves
**Rationale:** 516 Rust source files, ~460k LOC. Full parity is infeasible and unnecessary. Kernel parity is the objective.
**Activation criteria:** Not planned. Individual modules may be added to future waves based on user demand and cost-benefit analysis.

### DS-X02: Rust Proc-Macro Ergonomics

**Status:** Explicitly excluded
**Rationale:** Proc macros are Rust-specific compile-time metaprogramming. No C equivalent exists. The C API uses explicit function calls and configuration structs instead.
**Activation criteria:** Not applicable. C API design provides equivalent functionality through different mechanisms.

### DS-X03: Immediate Full Transport Stack Parity

**Status:** Explicitly excluded from initial release; partially addressed in Wave C/D
**Rationale:** HTTP/2 + TLS + gRPC + DB + distributed all at once would create a brittle monolith. Staged approach via Wave C (networking) and Wave D (full protocols) is the plan.
**Activation criteria:** Progressive activation via Wave C and D gating.

---

## Crate-Level Truth Table Audit (bd-yx9r.1)

This appendix records the gap that reopened crate-level parity planning on
2026-03-14: the local tree is broader than the kernel-only story, and the
current shipped surface is broader than that earlier closure implied, but much
of the reopened breadth is still only `partial`, profile-gated, or
walking-skeleton/stub-level rather than close to full upstream parity.

### Audit Rules

Classification used below:

- `substantive`: implemented and aligned with the currently shipped kernel/API story.
- `partial`: meaningful local surface exists, but the upstream contract is still materially broader.
- `stub-only`: local files primarily preserve API shape or ghost handles, not the real behavior.
- `absent`: no meaningful local public surface corresponding to the upstream family.

Additional shipping notes:

- `umbrella`: whether [`include/asx/asx.h`](/data/projects/asupersync_ansi_c/include/asx/asx.h) exposes the family.
- `archive`: whether the current [`Makefile`](/data/projects/asupersync_ansi_c/Makefile) compiles the implementation into `libasx.a`.

### Truth Table

| Upstream family / promise | Upstream evidence | Local evidence | Umbrella | Archive | Classification | Gap / dependency note |
|---|---|---|---|---|---|---|
| Kernel semantics: outcomes, budgets, cancellation, lifecycle, scheduler, timer wheel, quiescence | `/dp/asupersync/src/lib.rs` core guarantees and portable modules; local Phase 1 parity docs | [`include/asx/asx.h`](/data/projects/asupersync_ansi_c/include/asx/asx.h), [`src/runtime/scheduler.c`](/data/projects/asupersync_ansi_c/src/runtime/scheduler.c), [`src/runtime/lifecycle.c`](/data/projects/asupersync_ansi_c/src/runtime/lifecycle.c), [`src/time/timer_wheel.c`](/data/projects/asupersync_ansi_c/src/time/timer_wheel.c) | yes | yes | substantive | This is the part of the project that actually matches the earlier "kernel parity" closure. |
| Runtime object/config/bootstrap beyond kernel loop | `/dp/asupersync/src/lib.rs` re-exports `Config*`, `RuntimeProfile`, `Cx`, `Scope`, lab/runtime families | [`include/asx/runtime/rt.h`](/data/projects/asupersync_ansi_c/include/asx/runtime/rt.h), [`src/runtime/rt.c`](/data/projects/asupersync_ansi_c/src/runtime/rt.c), [`include/asx/runtime/runtime.h`](/data/projects/asupersync_ansi_c/include/asx/runtime/runtime.h) | yes | yes | partial | The runtime-object/bootstrap surface now ships through both the umbrella header and `libasx.a`; remaining parity debt is broader upstream runtime-local/state-table breadth and richer cross-surface integration. |
| Capability / structured-concurrency surface (`cx`, `scope`, explicit authority flow) | `/dp/asupersync/src/lib.rs` exports `cx`; upstream README centers `Cx` as capability boundary | [`include/asx/cx/cx.h`](/data/projects/asupersync_ansi_c/include/asx/cx/cx.h), [`include/asx/cx/scope.h`](/data/projects/asupersync_ansi_c/include/asx/cx/scope.h), [`src/cx/cx.c`](/data/projects/asupersync_ansi_c/src/cx/cx.c), [`src/cx/scope.c`](/data/projects/asupersync_ansi_c/src/cx/scope.c) | yes | yes | partial | The family now ships through the umbrella header and `libasx.a`; remaining parity work is broader upstream ecosystem coverage and propagation into more higher-surface modules. |
| App / operator surfaces (`app`, `doctor`, `report`) | `/dp/asupersync/src/lib.rs` exports `app`; upstream README presents user-facing app/workflow surfaces | [`include/asx/app/app.h`](/data/projects/asupersync_ansi_c/include/asx/app/app.h), [`src/app/app.c`](/data/projects/asupersync_ansi_c/src/app/app.c), [`src/app/doctor.c`](/data/projects/asupersync_ansi_c/src/app/doctor.c), [`src/app/report.c`](/data/projects/asupersync_ansi_c/src/app/report.c) | yes | yes | partial | The operator/app family now ships publicly; the remaining gap is parity depth versus the broader upstream app/workflow surface, not basic umbrella/archive presence. |
| Actor / supervision / gen_server | `/dp/asupersync/src/lib.rs` exports `actor`, `gen_server`, `supervision` | [`include/asx/actor/actor.h`](/data/projects/asupersync_ansi_c/include/asx/actor/actor.h), [`include/asx/actor/supervisor.h`](/data/projects/asupersync_ansi_c/include/asx/actor/supervisor.h), [`include/asx/actor/gen_server.h`](/data/projects/asupersync_ansi_c/include/asx/actor/gen_server.h), [`src/actor/actor.c`](/data/projects/asupersync_ansi_c/src/actor/actor.c), [`src/actor/supervisor.c`](/data/projects/asupersync_ansi_c/src/actor/supervisor.c), [`src/actor/gen_server.c`](/data/projects/asupersync_ansi_c/src/actor/gen_server.c) | yes | yes | partial | The actor/gen_server/supervision family now ships through both the umbrella header and `libasx.a`; the remaining gap is parity breadth and behavioral depth versus the broader upstream actor/gen_server/supervision contract. |
| Networking core (`net`) | `/dp/asupersync/src/lib.rs` exports `net`; upstream README and reopened epic treat transport families as part of crate-level promise | [`include/asx/net/net.h`](/data/projects/asupersync_ansi_c/include/asx/net/net.h), [`src/net/net.c`](/data/projects/asupersync_ansi_c/src/net/net.c) | yes | yes | partial | The net family now ships publicly and has moved beyond pure stubs, but it is still a reduced walking skeleton relative to the full upstream transport/networking contract. |
| Bytes / codec / async I/O data plane | `/dp/asupersync/src/lib.rs` exports `bytes`, `codec`, `io`, `encoding`, `decoding` and re-exports related types | [`include/asx/bytes/buf.h`](/data/projects/asupersync_ansi_c/include/asx/bytes/buf.h), [`include/asx/bytes/codec.h`](/data/projects/asupersync_ansi_c/include/asx/bytes/codec.h), [`include/asx/bytes/io_adapter.h`](/data/projects/asupersync_ansi_c/include/asx/bytes/io_adapter.h), [`include/asx/codec/codec.h`](/data/projects/asupersync_ansi_c/include/asx/codec/codec.h), [`include/asx/encoding/encoding.h`](/data/projects/asupersync_ansi_c/include/asx/encoding/encoding.h), [`include/asx/decoding/decoding.h`](/data/projects/asupersync_ansi_c/include/asx/decoding/decoding.h), [`src/bytes/buf.c`](/data/projects/asupersync_ansi_c/src/bytes/buf.c), [`src/bytes/codec.c`](/data/projects/asupersync_ansi_c/src/bytes/codec.c), [`src/bytes/io_adapter.c`](/data/projects/asupersync_ansi_c/src/bytes/io_adapter.c), [`src/encoding/encoding.c`](/data/projects/asupersync_ansi_c/src/encoding/encoding.c), [`src/decoding/decoding.c`](/data/projects/asupersync_ansi_c/src/decoding/decoding.c) | yes | yes | partial | The bytes/codec/encoding/decoding family now ships through both the umbrella header and `libasx.a`; the remaining gap is upstream breadth and semantic depth rather than missing public/archive exposure. |
| Channels and sync family beyond core MPSC | `/dp/asupersync/src/lib.rs` exports `channel` and `sync` families | [`include/asx/core/channel.h`](/data/projects/asupersync_ansi_c/include/asx/core/channel.h), [`include/asx/core/oneshot.h`](/data/projects/asupersync_ansi_c/include/asx/core/oneshot.h), [`include/asx/core/broadcast.h`](/data/projects/asupersync_ansi_c/include/asx/core/broadcast.h), [`include/asx/core/watch.h`](/data/projects/asupersync_ansi_c/include/asx/core/watch.h), [`include/asx/core/session.h`](/data/projects/asupersync_ansi_c/include/asx/core/session.h), [`include/asx/sync/mutex.h`](/data/projects/asupersync_ansi_c/include/asx/sync/mutex.h), [`include/asx/sync/semaphore.h`](/data/projects/asupersync_ansi_c/include/asx/sync/semaphore.h), [`include/asx/sync/barrier.h`](/data/projects/asupersync_ansi_c/include/asx/sync/barrier.h), [`include/asx/sync/once.h`](/data/projects/asupersync_ansi_c/include/asx/sync/once.h), [`include/asx/sync/notify.h`](/data/projects/asupersync_ansi_c/include/asx/sync/notify.h), [`src/channel/mpsc.c`](/data/projects/asupersync_ansi_c/src/channel/mpsc.c), [`src/channel/oneshot.c`](/data/projects/asupersync_ansi_c/src/channel/oneshot.c), [`src/channel/broadcast.c`](/data/projects/asupersync_ansi_c/src/channel/broadcast.c), [`src/channel/watch.c`](/data/projects/asupersync_ansi_c/src/channel/watch.c), [`src/channel/session.c`](/data/projects/asupersync_ansi_c/src/channel/session.c), [`src/sync/mutex.c`](/data/projects/asupersync_ansi_c/src/sync/mutex.c), [`src/sync/semaphore.c`](/data/projects/asupersync_ansi_c/src/sync/semaphore.c), [`src/sync/barrier.c`](/data/projects/asupersync_ansi_c/src/sync/barrier.c), [`src/sync/once.c`](/data/projects/asupersync_ansi_c/src/sync/once.c), [`src/sync/notify.c`](/data/projects/asupersync_ansi_c/src/sync/notify.c) | yes | yes | partial | The channel/sync family now ships through both the umbrella header and `libasx.a`; the remaining gap is fuller upstream breadth and parity depth, not missing umbrella exposure. |
| Trace / replay / lab / diagnostics | `/dp/asupersync/src/lib.rs` exports `trace`, `lab`, `observability`, `monitor`; upstream README promises deterministic testing and replay | [`src/runtime/trace.c`](/data/projects/asupersync_ansi_c/src/runtime/trace.c), [`src/runtime/replay.c`](/data/projects/asupersync_ansi_c/src/runtime/replay.c), [`src/runtime/lab.c`](/data/projects/asupersync_ansi_c/src/runtime/lab.c), [`src/runtime/diagnostic.c`](/data/projects/asupersync_ansi_c/src/runtime/diagnostic.c) | yes | yes | partial | Trace, replay, lab, and diagnostic surfaces now ship through both the umbrella header and archive; remaining debt is richer upstream trace-analysis/minimization breadth rather than basic availability. |
| HTTP / web / gRPC / transport / service / remote / distributed / messaging / server / TLS / database | `/dp/asupersync/src/lib.rs` portable, feature-gated, and platform-gated modules | [`include/asx/net/http.h`](/data/projects/asupersync_ansi_c/include/asx/net/http.h), [`include/asx/net/web.h`](/data/projects/asupersync_ansi_c/include/asx/net/web.h), [`include/asx/net/grpc.h`](/data/projects/asupersync_ansi_c/include/asx/net/grpc.h), [`include/asx/net/messaging.h`](/data/projects/asupersync_ansi_c/include/asx/net/messaging.h), [`include/asx/net/server.h`](/data/projects/asupersync_ansi_c/include/asx/net/server.h), [`include/asx/net/tls.h`](/data/projects/asupersync_ansi_c/include/asx/net/tls.h), [`include/asx/net/db.h`](/data/projects/asupersync_ansi_c/include/asx/net/db.h), [`include/asx/transport/transport.h`](/data/projects/asupersync_ansi_c/include/asx/transport/transport.h), [`include/asx/service/service.h`](/data/projects/asupersync_ansi_c/include/asx/service/service.h), [`include/asx/remote/remote.h`](/data/projects/asupersync_ansi_c/include/asx/remote/remote.h), [`src/net/http.c`](/data/projects/asupersync_ansi_c/src/net/http.c), [`src/net/web.c`](/data/projects/asupersync_ansi_c/src/net/web.c), [`src/net/grpc.c`](/data/projects/asupersync_ansi_c/src/net/grpc.c), [`src/net/messaging.c`](/data/projects/asupersync_ansi_c/src/net/messaging.c), [`src/net/server.c`](/data/projects/asupersync_ansi_c/src/net/server.c), [`src/net/tls.c`](/data/projects/asupersync_ansi_c/src/net/tls.c), [`src/net/db.c`](/data/projects/asupersync_ansi_c/src/net/db.c), [`src/transport/transport.c`](/data/projects/asupersync_ansi_c/src/transport/transport.c), [`src/service/service.c`](/data/projects/asupersync_ansi_c/src/service/service.c), [`src/remote/remote.c`](/data/projects/asupersync_ansi_c/src/remote/remote.c) | yes | yes | partial | These higher-surface families now ship publicly through the umbrella header and archive. The remaining parity gap is breadth and depth versus the broader upstream contract, plus compile-time/browser-build-matrix completeness beyond the runtime fail-closed enforcement now present on the native-facing subsets. |
| Browser / wasm profile and fail-closed compatibility matrix | `/dp/asupersync/src/lib.rs` compile-time wasm feature/profile guards | [`src/runtime/browser_boundary.c`](/data/projects/asupersync_ansi_c/src/runtime/browser_boundary.c), [`src/runtime/browser_diagnostic.c`](/data/projects/asupersync_ansi_c/src/runtime/browser_diagnostic.c), [`include/asx/runtime/browser_boundary.h`](/data/projects/asupersync_ansi_c/include/asx/runtime/browser_boundary.h) | yes | yes | partial | Browser-boundary and diagnostic seams now ship publicly; the remaining gap is upstream-style profile/build-contract completeness rather than absence from the umbrella or archive. |

### Key Findings

1. The current shipped surface is broader than the earlier kernel-only closure:
   the truth table now shows the listed higher-surface families exposed through
   both the umbrella header and `libasx.a`.
2. Crate-level parity still cannot be inferred from tree presence alone;
   `umbrella` and `archive` exposure must continue to be tracked separately
   from raw source presence, profile-specific hiding, and behavioral depth.
3. The remaining deferred-surface gap is now primarily about `partial`
   shipped families and explicit Wave C/D deferrals, not basic absence from
   the umbrella header or archive.
4. Downstream work should treat this appendix as a shipped-surface exposure
   baseline, with feature/profile/build-matrix follow-on work and
   evidence/gating follow-on work layered on top of that baseline rather than
   re-litigating basic public/archive presence.

---

## Register Maintenance Rules

1. **Adding items:** Any feature explicitly deferred during implementation must be added to this register with rationale and unblock criteria.
2. **Activating items:** When unblock criteria are met, the item moves from "Deferred" to a new bead in the appropriate wave.
3. **Removing items:** Items are never removed. If permanently abandoned, status changes to "Abandoned" with rationale.
4. **Review cadence:** This register is reviewed at each wave boundary (Wave A close, Wave B close, etc.).
5. **Semantic risk check:** Any item with MEDIUM or HIGH semantic risk must have dedicated parity fixtures before activation.

---

## Summary Statistics

| Metric | Value |
|--------|-------|
| Total deferred items | 17 |
| Wave C items | 3 |
| Wave D items | 6 |
| ADR-driven deferrals | 2 |
| Rust interop items | 3 |
| Explicit exclusions | 3 |
| HIGH semantic risk items | 3 (DS-D03, DS-P01, DS-R02) |
| MEDIUM semantic risk items | 3 (DS-C02, DS-D06, DS-R01) |
| LOW semantic risk items | 11 |

---

## Consolidation Checkpoint (bd-296.7)

This section binds deferred-surface governance to the owner decision log so downstream planning uses one consistent scope baseline.

### Decision Crosswalk

| Deferred item | Decision key | Decision source | Consistency note |
|---|---|---|---|
| DS-P01 (parallel profile) | `DEC-003` | `docs/OWNER_DECISION_LOG.md` | Wave A deferral is historical; `bd-pweu` graduation evidence is complete. Parity/e2e/proof/locality/benchmark evidence has landed; native adapters still own deterministic commit authority before default concurrent lane execution. |
| DS-S01 (static arena backend) | `DEC-004` | `docs/OWNER_DECISION_LOG.md` | Deferral active; allocator-vtable compatibility required now |
| All semantic-plane-sensitive deferrals | `DEC-005` | `docs/OWNER_DECISION_LOG.md` | No semantic drift allowed while surfaces are deferred |

### Downstream Bead and Gate Alignment

| Consumer | Required linkage | Current alignment |
|---|---|---|
| `bd-296.10` (wave gating protocol) | Must enforce Wave A/B/C/D boundaries from this register | aligned via `docs/WAVE_GATING_PROTOCOL.md` + DS statuses |
| `bd-296.8` (traceability index) | Must map decisions and deferrals to test/gate/evidence artifacts | aligned via decision keys and DS IDs |
| CI gate docs | Must reflect parallel/static deferral boundaries | aligned with decision matrix in `docs/OWNER_DECISION_LOG.md` |

### No-Implicit-Deferral Assertion

Every out-of-scope surface in current planning is explicitly listed in this register as one of:

- deferred to a named wave,
- deferred by explicit interop policy, or
- explicitly excluded from planned waves.

Any newly deferred surface must add a `DS-*` row plus decision linkage before related bead closure.
