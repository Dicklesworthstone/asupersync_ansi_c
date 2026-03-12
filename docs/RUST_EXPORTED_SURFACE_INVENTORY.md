# Rust Exported Surface Inventory

> **Bead:** `bd-1eqo.1.1`  
> **Status:** Canonical inventory of `/dp/asupersync` exported modules, root
> re-exports, and adjacent public contracts that define the parity target  
> **Last updated:** 2026-03-12 by CrimsonHarbor

This document answers a narrow but foundational question:

> What does the Rust project publicly promise today, and which source files or
> contract docs make that promise authoritative?

The existing inventory/provenance artifacts in this repository are useful, but
they are not optimized for roadmap planning:

- `docs/rust_baseline_inventory.json` is machine-readable provenance.
- `docs/FEATURE_PARITY.md` tracks the currently implemented C semantic units.
- `docs/DEFERRED_SURFACES.md` records earlier wave deferrals.

This document is the roadmap-facing companion. It turns the upstream exported
surface into a self-contained planning map so future contributors do not need to
re-open `/dp/asupersync/src/lib.rs`, the upstream README, and multiple contract
documents just to remember what "substantive parity" is supposed to include.

## 1. Normative Upstream Inputs

Primary upstream sources reviewed for this inventory:

- `/dp/asupersync/src/lib.rs`
- `/dp/asupersync/README.md`

Primary local contract/provenance documents that refine or contextualize the
parity target:

- `docs/FEATURE_PARITY.md`
- `docs/DEFERRED_SURFACES.md`
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`
- `docs/PLAN_EXECUTION_TRACEABILITY_INDEX.md`
- `docs/SUBSYSTEM_EVIDENCE_MATRIX.md`
- `docs/QUALITY_GATES.md`
- `docs/TEST_LOG_SCHEMA.md`

## 2. Planning Rules

For roadmap purposes, the upstream surface must be split into three buckets:

1. **Direct public API surface**  
   Items exported from crate root or exposed as named public modules.
2. **Adjacent public contract surface**  
   README guarantees, profile/feature compatibility rules, browser/native
   restrictions, examples, doctor/report expectations, and other user-visible
   behavior that may not appear as a single `pub fn`.
3. **Internal implementation detail**  
   Code that supports the public contract but is not itself a parity target
   unless another public promise makes it observable.

The roadmap must preserve buckets 1 and 2. Bucket 3 matters only when needed to
realize those promises.

## 3. Upstream Crate-Root Guarantees

From `/dp/asupersync/src/lib.rs` and `/dp/asupersync/README.md`, the upstream
project presents itself as:

- a spec-first async runtime,
- cancel-correct rather than "drop equals cancel",
- capability-secure via explicit `Cx`,
- structurally concurrent with region/scoped ownership,
- deterministic and replayable in lab/test mode,
- fail-closed across feature/profile boundaries,
- broader than a tiny kernel, with networking, services, browser/WASM, actor,
  supervision, observability, examples, and integration surfaces.

For roadmap planning, these are not marketing-only statements. They are
contractual expectations that imply concrete surfaces:

- "No orphan tasks" implies region/scope/task APIs and lifecycle evidence.
- "Cancellation is a protocol" implies explicit cancellation, cleanup, and
  failure-path observability.
- "Deterministic testing" implies lab runtime, reproducible logs, replay
  artifacts, and scenario packs.
- "Capability security" implies explicit authority-bearing APIs and must-fail
  tests for ambient-authority regressions.
- "Browser Edition" implies a real fail-closed compatibility matrix, not only
  wasm type aliases.

## 4. Direct Public Module Surface from `src/lib.rs`

### 4.1 Portable Always-Exported Modules

These modules are exported unconditionally at crate root and therefore define
the default parity surface, even when the current C implementation does not yet
match them.

| Module | Why it matters for parity planning |
|---|---|
| `actor` | Actor-facing public API exists; parity must account for actor semantics, not just kernel tasks |
| `app` | Application bootstrap/user-entry surface exists |
| `audit` | Audit/reporting is part of the user-visible trust story |
| `bytes` | Public buffer/value surface; not just an internal helper |
| `cancel` | Cancellation is a first-class public surface |
| `channel` | User-level channel family contract |
| `codec` | Framing and transport codec surface is public |
| `combinator` | Join/race/select-style orchestration is public |
| `config` | Runtime/config/profile objects are directly part of the API |
| `conformance` | Conformance/report-facing surface is exposed, not only CI internals |
| `console` | Operator/developer-facing console surface exists |
| `cx` | Capability context is core public API |
| `decoding` | Decoding pipeline is public |
| `distributed` | Distributed contract exists and cannot be ignored in long-range parity planning |
| `encoding` | Encoding/raptorq-facing pipeline is public |
| `epoch` | Epoch/barrier/circuit-breaker semantics are exported at root |
| `error` | Error taxonomy is part of the stable user contract |
| `evidence` | Evidence artifacts are explicit product surface |
| `evidence_sink` | Structured evidence sink/export behavior is public |
| `gen_server` | OTP-style surface is exported |
| `http` | HTTP-facing surface exists upstream |
| `io` | Async I/O abstractions are public |
| `lab` | Deterministic lab runtime is public and central |
| `link` | Cross-runtime/linkage surface exists |
| `migration` | Migration/change-management support exists upstream |
| `monitor` | Monitoring/inspection is public |
| `net` | Networking is public |
| `obligation` | Explicit obligation lifecycle is user-facing |
| `observability` | Diagnostics/metrics/report context are public |
| `plan` | Plan-DAG / rewrite IR is public |
| `raptorq` | Specialized but exported; cannot be forgotten in full-scope roadmap |
| `record` | Exported module, but usually implementation-adjacent; see Section 7 |
| `remote` | Remote execution/idempotency/saga surfaces are exported |
| `runtime` | Runtime object/scheduler surface is public |
| `security` | Authenticated symbol/security primitives are public |
| `service` | Service composition/retry/discovery surface is public |
| `session` | Session semantics are exported |
| `spork` | Upstream explicitly exports it; parity planning must treat it as a named surface |
| `stream` | Stream adapters/composition surface is public |
| `supervision` | Supervision trees are public |
| `sync` | Mutex/RwLock/Semaphore/etc. surface is public |
| `time` | Deadline/sleep/interval/time policies are public |
| `trace` | Trace/replay/debug surface is public |
| `tracing_compat` | Optional integration contract exists |
| `transport` | Transport-layer abstraction is public |
| `types` | Foundational IDs/outcomes/policies/WASM ABI values are public |
| `util` | Exported, though much of it is likely support-oriented; treat carefully |
| `web` | Web-facing/browser/server surface exists upstream |

### 4.2 Feature-Gated Public Modules

These modules are not always present, but they are still part of the upstream
public contract because users can enable them through supported features.

| Gated module | Gate | Contract implication |
|---|---|---|
| `cli` | `feature = "cli"` | CLI/user-operator surface is first-class |
| `database` | `sqlite` or `postgres` or `mysql` | database integration is in-scope for long-range parity |
| `tls` | `feature = "tls"` | transport security surface exists upstream |

### 4.3 Platform-Gated Public Modules

These modules are excluded from browser `wasm32` builds but remain part of the
native contract:

| Platform-gated module | Condition | Contract implication |
|---|---|---|
| `fs` | `not(target_arch = "wasm32")` | filesystem host integration is public |
| `grpc` | `not(target_arch = "wasm32")` | gRPC is not an internal experiment; it is exported |
| `messaging` | `not(target_arch = "wasm32")` | broker/streaming integrations are explicit upstream scope |
| `process` | `not(target_arch = "wasm32")` | process control/lifecycle is public |
| `server` | `not(target_arch = "wasm32")` | server bootstrap/shutdown is public |
| `signal` | `not(target_arch = "wasm32")` | signal handling is part of native runtime behavior |

### 4.4 Test-Only Exported Modules

These do not define production parity, but they do define developer and
evidence-generation expectations:

| Module | Gate | Planning implication |
|---|---|---|
| `test_logging` | `test` or `test-internals` | structured artifact/log schema is part of the verification contract |
| `test_ndjson` | `test` or `test-internals` | NDJSON evidence tooling exists upstream |
| `test_utils` | `test` or `test-internals` | helper/test harness conventions are part of proof production |

## 5. Root Re-Export Surface

Crate-root re-exports show what upstream expects users to import directly.
These are higher-priority parity surfaces than incidental nested module items.

### 5.1 Runtime and Configuration Re-Exports

- `Cx`, `Scope`
- `AdaptiveConfig`, `BackoffConfig`, `ConfigError`, `ConfigLoader`
- `EncodingConfig`, `RaptorQConfig`, `ResourceConfig`, `RuntimeProfile`
- `SecurityConfig`, `TimeoutConfig`, `TransportConfig`

Implication:

- capability context and scope APIs are central, not niche;
- profile/configuration objects are public user inputs;
- security/transport/time policies are explicit top-level contracts.

### 5.2 Foundational Value and Error Re-Exports

- `Budget`, `Outcome`, `OutcomeError`, `Policy`, `Severity`, `Time`
- `CancelKind`, `CancelReason`
- `RegionId`, `TaskId`, `ObligationId`
- `AcquireError`, `RecvError`, `SendError`
- `Error`, `ErrorKind`, `ErrorCategory`, `Recoverability`, `RecoveryAction`
- `Result`, `ResultExt`

Implication:

- the type algebra is a primary user-facing contract;
- cancellation and failure taxonomy are intended to be imported directly;
- parity cannot stop at runtime internals if these values remain incomplete.

### 5.3 Lab, Encoding, Decoding, and Epoch Re-Exports

- `LabConfig`, `LabRuntime`
- `EncodingPipeline`, `EncodingStats`, `EncodedSymbol`, `EncodingError`
- `DecodingPipeline`, `DecodingConfig`, `DecodingProgress`, `DecodingError`
- epoch/barrier/circuit-breaker/retry selection family such as `Epoch`,
  `EpochConfig`, `EpochBarrier`, `EpochRace2`, `epoch_join2`, `epoch_race2`,
  `epoch_select`, `bulkhead_call_in_epoch`, `circuit_breaker_call_in_epoch`

Implication:

- deterministic lab behavior is not optional garnish;
- encoding/decoding and epoch policy surfaces materially shape upstream usage.

### 5.4 Remote/Distributed Re-Exports

Representative re-exports include:

- `CancelRequest`, `RemoteCap`, `RemoteHandle`, `RemoteMessage`, `RemoteOutcome`
- `IdempotencyKey`, `IdempotencyRecord`, `IdempotencyStore`
- `Saga`, `SagaState`, `SagaStepError`
- `SpawnRequest`, `SpawnAck`, `SpawnAckStatus`, `SpawnRejectReason`
- `Lease`, `LeaseState`, `LeaseRenewal`, `LeaseError`
- `spawn_remote`

Implication:

- upstream public scope already includes remote execution and saga/idempotency
  semantics;
- a full substantive parity roadmap must retain these as explicit future tracks,
  even if short-term implementation sequencing defers them.

### 5.5 Browser/WASM and Web-Integration Re-Exports from `types`

Representative re-exports include:

- `WASM_ABI_MAJOR_VERSION`, `WASM_ABI_MINOR_VERSION`
- `WASM_ABI_SIGNATURES_V1`, `WASM_ABI_SIGNATURE_FINGERPRINT_V1`
- `WasmAbiVersion`, `WasmAbiSignature`, `WasmAbiValue`,
  `WasmAbiOutcomeEnvelope`, `WasmAbiCompatibilityDecision`,
  `WasmAbiVersionBump`, `WasmAbiBoundaryEvent`, `WasmAbiErrorCode`
- `WasmTaskSpawnBuilder`, `WasmTaskCancelRequest`, `WasmHandleRef`
- `NextjsBootstrapPhase`, `NextjsIntegrationSnapshot`,
  `NextjsNavigationType`, `NextjsRenderEnvironment`
- `ReactProviderConfig`, `ReactProviderState`, `ReactProviderPhase`
- `SuspenseBoundaryState`, `SuspenseDiagnosticEvent`,
  `SuspenseTaskConfig`, `SuspenseTaskSnapshot`
- helpers such as `classify_wasm_abi_compatibility`,
  `required_wasm_abi_bump`, `validate_wasm_boundary_transition`,
  `outcome_to_error_boundary_action`, `outcome_to_suspense_state`

Implication:

- browser/WASM support is a concrete, typed contract already surfaced at crate
  root;
- Next.js/React/Suspense transition semantics are explicitly in scope upstream;
- roadmap planning must treat Browser Edition as a product surface, not as a
  side effect of generic WASM support.

### 5.6 Macro Surface

With `proc-macros` enabled, upstream re-exports macro entry points including:

- `join_all`, `scope`, `spawn`
- explicit-path versions for `join`, `race`, `scope`, `session_protocol`,
  `spawn`

Implication:

- ergonomic orchestration syntax is part of the public experience;
- planning should preserve macro-driven workflows even if the C port uses a
  different implementation mechanism.

## 6. Adjacent Public Contract Surface

Some critical scope is defined more by contract text and compatibility rules
than by a single exported item.

### 6.1 README-Level Guarantees

The upstream README makes these promises prominently:

- no orphan tasks,
- cancel-correctness,
- bounded cleanup,
- no silent drops via two-phase effects,
- deterministic testing and replay,
- adaptive preemption fairness,
- drain progress certificates,
- spectral early warnings,
- capability security.

Planning implication:

- these guarantees create proof obligations even where the implementation shape
  differs between Rust and C;
- they justify roadmap tracks for fairness/diagnostics/evidence, not just
  runtime primitives.

### 6.2 Profile and Compatibility Rules in `lib.rs`

The crate root encodes explicit fail-closed rules for browser/WASM builds:

- `wasm32` must choose exactly one canonical browser profile,
- `native-runtime` is forbidden on browser builds,
- `cli`, `io-uring`, `tls`, `sqlite`, `postgres`, `mysql`, `kafka`, and
  certain browser tracing/IO combinations are forbidden under specific browser
  profiles.

Planning implication:

- compatibility failures and diagnostics are part of the user contract;
- Browser Edition parity must include valid/invalid matrix behavior, not just
  successful happy-path bindings.

### 6.3 Examples, Smoke Paths, and "Coming from tokio?" Mapping

The upstream README documents:

- structured-concurrency examples,
- cancellation-safe reserve/commit examples,
- deterministic lab examples,
- tokio-to-asupersync concept mappings,
- user-level explanations of surprising semantic differences.

Planning implication:

- examples and onboarding paths are part of the public surface;
- parity must eventually cover canonical example packs, smoke scripts, and
  user-facing diagnostics that explain semantic differences.

### 6.4 Evidence and Diagnostic Contract

Even without a single crate-root `doctor()` export, upstream surfaces like
`observability`, `evidence`, `evidence_sink`, `conformance`, `test_logging`,
and README replay/debug claims establish a clear contract:

- failures should leave structured evidence,
- deterministic runs should be replayable,
- diagnostics should be rich enough to explain concurrency behavior,
- reports/artifacts are part of the product, not only CI plumbing.

Planning implication:

- observability and evidence-sink parity deserve dedicated roadmap branches;
- every major implementation bead must carry artifact/logging obligations.

## 7. Public Surface vs Internal Detail

Not every exported module should be copied one-for-one into the C port without
thought. For planning purposes:

### 7.1 Clearly First-Class Public Surface

Treat these as unquestionably in-scope parity anchors:

- `cx`, `runtime`, `cancel`, `obligation`, `time`, `channel`, `sync`,
  `combinator`, `lab`, `trace`, `types`, `config`, `error`,
  `observability`, `evidence`, `evidence_sink`, `bytes`, `io`, `net`,
  `service`, `stream`, `remote`, `security`, `web`, `actor`, `gen_server`,
  `supervision`, `app`, `server`, `process`, `signal`, `fs`, `grpc`,
  `messaging`, `database`, `tls`, `cli`.

### 7.2 Exported but Potentially Support-Oriented

These should be handled carefully and mapped to user-visible guarantees before
they become direct ANSI C API goals:

- `record`
- `util`
- `console`
- `migration`
- `monitor`
- `link`
- `spork`
- some parts of `conformance`

These are still part of the public Rust surface. The planning distinction is
only that their parity value may be mediated through other user-facing
contracts rather than requiring a literal one-module-to-one-module port.

## 8. Mapping to the `bd-1eqo` Roadmap

This table ties the exported Rust surface to the roadmap tracks so contributors
know where each major promise lives.

| Roadmap track | Primary upstream modules/contracts |
|---|---|
| `bd-1eqo.15` foundational types/value semantics | `types`, `error`, root re-exports for `Outcome`, `Budget`, IDs, WASM ABI values, compatibility helpers |
| `bd-1eqo.3` capability context / structured concurrency | `cx`, `cancel`, `obligation`, README guarantees around no-orphan tasks and explicit authority |
| `bd-1eqo.4` runtime object / scheduler / reactor | `runtime`, `trace`, `config`, parts of `record`, README fairness and lifecycle guarantees |
| `bd-1eqo.5` time / deadlines / cancellation driver | `time`, `cancel`, timeout/sleep examples, cleanup-boundedness guarantees |
| `bd-1eqo.6` channels / sync primitives | `channel`, `sync`, two-phase effect guarantees in README and tokio-mapping docs |
| `bd-1eqo.7` combinators / orchestration | `combinator`, macro surface, `session`, parts of `epoch`, orchestration examples |
| `bd-1eqo.8` lab / replay / deterministic evidence | `lab`, `trace`, `conformance`, `test_logging`, replay/debug guarantees |
| `bd-1eqo.9` bytes / codec / IO / networking / app | `bytes`, `codec`, `io`, `net`, `http`, `app`, `transport`, `encoding`, `decoding`, examples/smoke flows |
| `bd-1eqo.10` actor / gen_server / supervision | `actor`, `gen_server`, `supervision`, `session` |
| `bd-1eqo.11` observability / diagnostics / evidence sinks | `observability`, `evidence`, `evidence_sink`, `audit`, `monitor`, report/replay promises |
| `bd-1eqo.12` security / audit / authority hardening | `security`, `audit`, `cx`, compatibility/fail-closed policy text |
| `bd-1eqo.13` streams / plan / service composition | `stream`, `plan`, `service`, portions of `transport` |
| `bd-1eqo.14` native host / server / shutdown | `fs`, `process`, `signal`, `server`, `cli`, native-only gates in crate root |
| `bd-1eqo.16` Browser Edition / WASM / web | `web`, browser/WASM re-exports in `types`, crate-root browser feature rules, Next.js/React/Suspense state types |

## 9. Relationship to Existing Deferred-Surface Docs

`docs/DEFERRED_SURFACES.md` was written for an earlier wave-based kernel-first
port plan. This inventory does not replace it; it broadens the planning frame.

Key consequence:

- if a surface is publicly exported upstream, it should appear in the roadmap
  even when short-term implementation sequencing defers it;
- "deferred" is a schedule choice, not evidence that the surface is outside the
  real parity target.

## 10. Review Checklist for Future Planning Beads

When creating or refining any new `bd-1eqo.*` bead, confirm:

1. which exported module(s) or root re-exports it covers,
2. which README-level or compatibility-level contract it protects,
3. whether it targets direct API parity, adjacent behavioral parity, or both,
4. whether browser/native/feature-gated variants change the obligation,
5. which evidence lanes from `docs/SUBSYSTEM_EVIDENCE_MATRIX.md` it must carry.

If a future contributor cannot answer those questions from this document, the
inventory is incomplete and should be extended before more implementation beads
are added.
