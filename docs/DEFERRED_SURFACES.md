# Deferred Surface Register

> **Historical Note (2026-03-27):** This file is a planning-era snapshot, not the current canonical scope baseline. Some statements below describe an earlier "kernel-only until later waves" posture that no longer matches the current shipped umbrella/archive surface.
>
> Use these source-backed documents for current status instead:
> - `docs/DEFERRED_SURFACE_REGISTER.md`
> - `docs/RUST_EXPORTED_SURFACE_INVENTORY.md`
> - `docs/SUBSYSTEM_EVIDENCE_MATRIX.md`
> - `docs/SELECTIVE_SURFACE_PORTING_QUEUE.md`
>
> The bead IDs and `Not yet created` markers embedded in the tables below are also historical planning artifacts from the time this document was written; they do not reflect the current live Beads backlog state.

> **Bead:** bd-296.24
> **Status:** Historical planning register retained for context
> **Source:** Plan sections 2.4, 2.4.1, 4.2–4.4, 5, and ADR-001 (parallel profile deferral)
> **Author:** NobleCanyon (claude-code/opus-4.6)
> **Date:** 2026-02-27

---

## Purpose

This document tracks all subsystems, capabilities, and interoperability surfaces that are explicitly deferred from the kernel milestone (Wave A). Each entry includes rationale, unblock criteria, ownership, and parity/risk implications.

Deferrals are not feature cuts — they are scope-sequencing decisions that preserve kernel quality while enabling future expansion.

Where this file conflicts with the newer source-backed registers, treat this file as historical context rather than the controlling record.

---

## Register Format

Each entry follows this structure:

- **ID**: `DEF-###`
- **Surface**: What is deferred
- **Wave**: Target wave for inclusion (B/C/D/TBD)
- **Rationale**: Why deferred
- **Unblock Criteria**: What must be true before this can proceed
- **Owner**: Who decides activation
- **Parity Impact**: Effect on Rust-C semantic parity
- **Risk if Deferred Too Long**: Consequences of indefinite deferral
- **ADR Reference**: Linked ADR if applicable

---

## Deferred Subsystem Register

### DEF-001: Optional Parallel Profile (Worker Model, Work-Stealing)

| Field | Value |
|-------|-------|
| **Surface** | `asx_runtime_parallel` — worker threads, work-stealing, lane scheduling |
| **Wave** | B |
| **Rationale** | ADR-001: kernel focus; parallel adds atomics/sync complexity; ALPHA-5/6 require EV measurement from hotspot data |
| **Unblock Criteria** | (1) Single-thread kernel passes all quality gates, (2) EV >= 2.0 for ALPHA-5/6 levers measured from baseline, (3) fairness test framework ready |
| **Owner** | Project maintainer |
| **Parity Impact** | Rust asupersync has parallel scheduling. Until Wave B, C port is single-thread-only for deterministic-mode use cases. HFT/embedded users are unaffected (prefer single-thread). |
| **Risk if Deferred Too Long** | Multi-threaded workloads cannot be served; API design may ossify around single-thread assumptions |
| **ADR Reference** | ADR-001 |
| **Bead** | bd-2cw.7 |

### DEF-002: Static Arena Allocator Backend

| Field | Value |
|-------|-------|
| **Surface** | Static-memory-only arena allocation (no `malloc` in hot path) |
| **Wave** | B (or automotive-specific milestone) |
| **Rationale** | ADR-002: primary targets (routers) have malloc; vtable interface designed for forward compatibility |
| **Unblock Criteria** | (1) Allocator vtable interface stable, (2) `asx_runtime_seal_allocator` implemented, (3) automotive/freestanding adopter demand confirmed |
| **Owner** | Project maintainer |
| **Parity Impact** | No semantic impact — allocator is resource-plane. Static arena is a different backend behind the same API. |
| **Risk if Deferred Too Long** | Automotive/ASIL targets blocked from adoption; vtable interface may need revision if assumptions were wrong |
| **ADR Reference** | ADR-002 |
| **Bead** | Related to bd-hwb.6 |

### DEF-003: Full Trace Infrastructure

| Field | Value |
|-------|-------|
| **Surface** | Advanced trace topology, persistent trace management, distributed trace correlation |
| **Wave** | B |
| **Rationale** | Plan section 4.2: minimal trace for replay/conformance ships in Wave A; advanced trace is post-kernel |
| **Unblock Criteria** | (1) Kernel trace hooks stable, (2) trace format versioned, (3) replay parity verified for minimal trace |
| **Owner** | Project maintainer |
| **Parity Impact** | Rust has ~36k LOC trace subsystem. Wave A ships minimal replay/conformance subset only. |
| **Risk if Deferred Too Long** | Production users lack advanced observability; trace format may need breaking changes |
| **Bead** | Not yet created (Wave B scope) |

### DEF-004: Lab/Determinism Profiling

| Field | Value |
|-------|-------|
| **Surface** | `lab` module — deterministic profiling, schedule exploration, workload replay analysis |
| **Wave** | B |
| **Rationale** | Plan section 4.2: lab profile for seeded schedule behavior ships after kernel |
| **Unblock Criteria** | (1) Deterministic scheduler stable, (2) PRNG stream per subsystem verified, (3) replay hash stability in CI |
| **Owner** | Project maintainer |
| **Parity Impact** | Rust has ~37k LOC lab subsystem. Wave A ships deterministic mode and replay but not advanced lab tooling. |
| **Risk if Deferred Too Long** | Limited debugging tooling for determinism issues |
| **Bead** | Not yet created (Wave B scope) |

### DEF-005: Networking Primitives

| Field | Value |
|-------|-------|
| **Surface** | TCP/UDP/socket abstractions, reactor integration |
| **Wave** | C |
| **Rationale** | Plan section 4.3: networking is a systems surface, not kernel semantics |
| **Unblock Criteria** | (1) Kernel stable, (2) platform adapters (POSIX/Win32) proven, (3) reactor vtable interface tested |
| **Owner** | Project maintainer |
| **Parity Impact** | Rust has ~26k LOC net subsystem. This reflects an earlier planning snapshot; current `asx` ships a broader surface than kernel-only, but higher network layers remain partial/profile-gated rather than at full upstream parity. |
| **Risk if Deferred Too Long** | Users needing I/O must bring their own reactor; API constraints may emerge from kernel assumptions |
| **Bead** | Not yet created (Wave C scope) |

### DEF-006: HTTP/2 and gRPC

| Field | Value |
|-------|-------|
| **Surface** | HTTP/2 codec, gRPC transport layer |
| **Wave** | D |
| **Rationale** | Plan section 4.4: heavyweight protocol surfaces deferred to final wave |
| **Unblock Criteria** | (1) Networking primitives stable (DEF-005), (2) binary codec proven, (3) demand justified |
| **Owner** | Project maintainer |
| **Parity Impact** | Rust has ~23k LOC HTTP subsystem. This is the largest deferral gap. |
| **Risk if Deferred Too Long** | Limits asx to kernel/embedded use cases; competitors may fill the gap |
| **Bead** | Not yet created (Wave D scope) |

### DEF-007: Database Clients

| Field | Value |
|-------|-------|
| **Surface** | Database connection pooling, prepared statements, async query execution |
| **Wave** | D |
| **Rationale** | Plan section 4.4: application-layer surface, not kernel semantics |
| **Unblock Criteria** | (1) Networking and channel semantics stable, (2) obligation lifecycle proven for resource management |
| **Owner** | Project maintainer |
| **Parity Impact** | Application-level feature; no kernel semantic impact |
| **Risk if Deferred Too Long** | Limited to embedded/kernel use cases |
| **Bead** | Not yet created (Wave D scope) |

### DEF-008: Distributed/Remote Operation

| Field | Value |
|-------|-------|
| **Surface** | Remote task spawning, distributed region trees, consensus integration |
| **Wave** | D |
| **Rationale** | Plan section 4.4: requires networking + serialization + consensus; extremely complex |
| **Unblock Criteria** | (1) All lower waves stable, (2) serialization format finalized, (3) remote semantics specified |
| **Owner** | Project maintainer |
| **Parity Impact** | Major gap vs Rust. Not relevant for embedded/HFT/automotive targets. |
| **Risk if Deferred Too Long** | Limits to single-node deployments |
| **Bead** | Not yet created (Wave D scope) |

### DEF-009: RaptorQ + Advanced Policy Stack

| Field | Value |
|-------|-------|
| **Surface** | RaptorQ erasure coding, advanced scheduling policies |
| **Wave** | D |
| **Rationale** | Plan section 4.4: specialized surface, ~18k LOC in Rust |
| **Unblock Criteria** | (1) Channel semantics stable, (2) binary codec proven, (3) demand from specific vertical |
| **Owner** | Project maintainer |
| **Parity Impact** | Specialized feature; not required for kernel parity |
| **Risk if Deferred Too Long** | Minimal; specialized audience |
| **Bead** | Not yet created (Wave D scope) |

### DEF-010: Actor/Supervision (Erlang/OTP-Style)

| Field | Value |
|-------|-------|
| **Surface** | `gen_server`, actor model, supervision trees |
| **Wave** | D |
| **Rationale** | Plan section 4.4: ~28k LOC in Rust; heavyweight framework layer on top of kernel |
| **Unblock Criteria** | (1) Task lifecycle and region semantics battle-tested, (2) obligation lifecycle proven for actor resource management, (3) failure containment policy finalized |
| **Owner** | Project maintainer |
| **Parity Impact** | Large gap vs Rust. However, kernel semantics (regions, tasks, obligations) provide the foundation. |
| **Risk if Deferred Too Long** | Users wanting structured concurrency patterns must build their own supervision |
| **Bead** | Not yet created (Wave D scope) |

### DEF-011: Selected Combinators

| Field | Value |
|-------|-------|
| **Surface** | Async combinators (~17k LOC in Rust) |
| **Wave** | C |
| **Rationale** | Plan section 4.3: depends on task/poll model being stable |
| **Unblock Criteria** | (1) Task poll contract proven, (2) protothread macros ergonomic, (3) demand assessment |
| **Owner** | Project maintainer |
| **Parity Impact** | Ergonomics gap; kernel correctness unaffected |
| **Risk if Deferred Too Long** | Poor developer experience for complex async patterns |
| **Bead** | Not yet created (Wave C scope) |

---

## Rust Interoperability Decisions

### INTEROP-001: Rust-C FFI Bridge

| Field | Value |
|-------|-------|
| **Status** | Explicitly deferred (Plan section 2.4.1) |
| **Current Policy** | No mandatory Rust-C FFI bridge in kernel milestone |
| **Rationale** | Shared contract is semantic spec + fixture corpus, not API shape identity |
| **Future Track** | Optional FFI/interop in later waves with dedicated fixtures and parity evidence |
| **API Cornering Risk** | Low — C API is designed from spec, not from Rust API shape. FFI bridge would wrap C API. |

### INTEROP-002: Cross-Language Channel Interop

| Field | Value |
|-------|-------|
| **Status** | Explicitly deferred (Plan section 2.4.1) |
| **Current Policy** | No mandatory cross-runtime channel interop in kernel milestone |
| **Rationale** | Channel semantics must be proven in C first before cross-runtime bridging |
| **Future Track** | Post-kernel consideration; requires shared serialization format |
| **API Cornering Risk** | Moderate — channel message format should remain extensible for cross-runtime use |

### INTEROP-003: Shared Event Schema

| Field | Value |
|-------|-------|
| **Status** | Partially addressed |
| **Current Policy** | JSON fixture format serves as shared semantic schema for conformance |
| **Rationale** | Conformance testing uses normalized event streams; this implicitly defines a shared schema |
| **Future Track** | Formalize as explicit cross-runtime event schema if FFI/interop track opens |
| **API Cornering Risk** | Low — JSON conformance fixtures already define the canonical schema |

---

## Wave Summary

| Wave | Surfaces | Estimated LOC (Rust Equivalent) | Kernel Dependency |
|------|----------|--------------------------------|-------------------|
| **A (Kernel)** | types, record, runtime, time, channel, minimal sync/util | ~120k LOC equivalent | None (foundation) |
| **B** | trace (full), lab, parallel profile, static arena | ~75k LOC equivalent | Wave A stable |
| **C** | networking, combinators, observability | ~45k LOC equivalent | Wave B stable |
| **D** | HTTP/2, gRPC, DB, distributed, RaptorQ, actor/supervision | ~95k LOC equivalent | Wave C stable |

---

## Audit Trail

| Date | Action | Author | Details |
|------|--------|--------|---------|
| 2026-02-27 | Register created | NobleCanyon | Initial register with 11 deferred surfaces, 3 interop decisions |
| 2026-02-27 | ADR-001 applied | NobleCanyon | Parallel profile explicitly deferred to Wave B (DEF-001) |
| 2026-02-27 | ADR-002 applied | NobleCanyon | Static arena deferred to Wave B with vtable prep (DEF-002) |
| 2026-03-27 | Historical-status note added | SilverRiver | Marked this file as a planning snapshot and redirected readers to the current source-backed deferred-surface and shipped-surface registers |

---

*This register is retained for historical planning context. For current deferred-surface and shipped-surface status, use the newer source-backed docs listed at the top of this file.*
