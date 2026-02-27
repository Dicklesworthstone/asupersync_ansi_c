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
| Parallel Profile | 1 | ADR-001: defer to post-kernel |
| Static Arena Backend | 1 | ADR-002: dynamic default + vtable prep |
| Rust-C Interoperability | 3 | Plan Section 2.4.1: companion, not bridge |
| Explicit Exclusions | 3 | Plan Section 5: out of scope for first release |

---

## Wave C — Selected Systems Surfaces (Post-Kernel)

### DS-C01: Targeted Networking Primitives

**Status:** Deferred to Wave C
**Rationale:** Networking requires platform adapters (POSIX sockets, Win32 IOCP, embedded poll/select), reactor integration, and connection lifecycle management that are outside the kernel's semantic scope. Kernel parity must be proven before adding I/O surfaces.
**Rust source:** `src/net/` (~26k LOC)
**Unblock criteria:**
- Wave A quality gates green (all kernel conformance/parity/embedded gates pass)
- Wave B conformance harness operational (differential fixture runner working)
- Reactor/event-loop adapter interface designed in `docs/PROPOSED_ANSI_C_ARCHITECTURE.md`
- Dedicated spec extraction for networking semantics (new bead required)
**Owner:** TBD (assigned when Wave C opens)
**Dependency path:** Wave A gates -> Wave B harness -> networking spec -> implementation
**Semantic risk:** LOW — networking does not change kernel lifecycle/cancel/obligation semantics. Uses kernel primitives.

### DS-C02: Selected Combinators

**Status:** Deferred to Wave C
**Rationale:** Combinators (join, race, select, timeout, retry) compose kernel primitives. They require a stable task/region/cancellation substrate before they can be faithfully ported.
**Rust source:** `src/combinator/` (~17k LOC)
**Unblock criteria:**
- Wave A task lifecycle, cancellation protocol, and obligation resolution stable
- Combinator semantic spec extracted from Rust behavior
- Dedicated test suites with deterministic replay verification
**Owner:** TBD
**Dependency path:** Wave A kernel -> combinator spec extraction -> implementation + parity fixtures
**Semantic risk:** MEDIUM — combinators interact deeply with cancellation propagation and outcome aggregation. Incorrect port could violate outcome lattice join semantics.

### DS-C03: Selected Observability Surfaces

**Status:** Deferred to Wave C
**Rationale:** Observability (metrics, spans, structured logging beyond trace) builds on the trace/replay layer (Wave B). Adding observability before trace is stable would create untested instrumentation.
**Rust source:** Parts of `src/trace/` (~36k LOC), observability layers
**Unblock criteria:**
- Wave B trace/replay layer operational
- Trace format and semantic digest stable
- Observability adapter interface designed
**Owner:** TBD
**Dependency path:** Wave B trace -> observability spec -> implementation
**Semantic risk:** LOW — observability is read-only instrumentation; does not alter runtime behavior.

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

**Status:** Deferred per ADR-001
**Rationale:** Kernel correctness is the foundation. Parallel profile adds atomics, synchronization adapters, EBR/hazard pointers (ALPHA-5/6), work-stealing, and fairness validation — doubling the verification surface. The three primary target verticals (HFT, automotive, embedded router) primarily need single-thread deterministic behavior.
**ADR:** ADR-001 (docs/OPEN_DECISIONS_ADR.md)
**Bead:** bd-2cw.7
**Unblock criteria:**
- Wave A kernel quality gates green
- Kernel deterministic scheduling verified
- Hotspot data available to evaluate ALPHA-5 (seqlocks) and ALPHA-6 (EBR/hazard pointers) EV >= 2.0
- Dedicated parallel-specific quality gates defined (fairness, no deterministic-mode regression, fallback parity)
- Early adopter demand signal for multi-threaded usage
**Owner:** TBD (assigned at Wave B planning)
**Dependency path:** Wave A gates -> hotspot measurement -> parallel spec -> implementation + fairness tests
**Semantic risk:** HIGH if not carefully handled — parallel scheduling must produce identical semantic outcomes to single-thread mode for deterministic scenarios. Cross-profile digest parity is mandatory.
**Rollback trigger:** Strong early adopter demand for parallel before Wave B.

---

## Static Arena Backend

### DS-S01: Static Arena Memory Backend (Freestanding/Automotive Targets)

**Status:** Interface designed; implementation deferred per ADR-002
**Rationale:** Primary embedded target (EMBEDDED_ROUTER / OpenWrt) has `malloc`. Arena tables already use allocator vtable, which can be swapped to static memory without API changes. Static arena is a different allocator backend, not a different architecture.
**ADR:** ADR-002 (docs/OPEN_DECISIONS_ADR.md)
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
| DS-P01 (parallel profile) | `DEC-003` | `docs/OWNER_DECISION_LOG.md` | Deferral active; Wave A excludes parallel profile lanes |
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
