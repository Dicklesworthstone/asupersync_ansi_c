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

## 11. Current ANSI C Truth Table (2026-03-14, `bd-yx9r.1`)

This section answers the missing question that the earlier inventory did not:

> Which upstream exported families are actually present in the current ANSI C
> tree, and which ones are still absent, skeletal, or materially narrower than
> the Rust crate contract?

Status terms used below:

- `substantive`: dedicated public headers and source exist, plus tests/examples
  show a real user-facing surface.
- `partial`: meaningful public API exists, but it is materially narrower than
  the upstream family or missing major subfamilies/contracts.
- `stub-only`: a named surface exists, but the implementation is explicitly
  skeletal, ghost-backed, or in-memory placeholder logic.
- `absent`: no comparable public module family exists in the ANSI C tree today.

One more nuance matters for roadmap truth-maintenance: tree presence alone is
still not enough to establish the shipped contract. The current umbrella header
(`include/asx/asx.h`) and archive build (`LIB_SRC` in `Makefile`) now expose a
much broader surface than the earlier kernel-only story, but profile-gated
hiding and partial implementations still mean "present in-tree" and "fully
parity-ready" are not the same thing.

### 11.1 Portable and Always-Exported Upstream Families

| Upstream family | Current ANSI C state | Evidence in this repo | Gap / dependency note |
|---|---|---|---|
| `actor` | `partial` | `include/asx/actor/actor.h`, `src/actor/actor.c`, actor task/mailbox/call loop | Real actor API exists, but far smaller than the upstream actor ecosystem |
| `app` | `partial` | `include/asx/app/app.h`, `include/asx/app/doctor.h`, `include/asx/app/report.h`, `src/app/*.c`, umbrella/header + archive build exposure | Bootstrap/doctor/report surface now ships through the umbrella and archive, but it is still not a full crate-level application stack |
| `audit` | `partial` | `include/asx/security/audit.h`, `src/security/audit.c`, `tests/vignettes/vignette_security.c` | Audit/evidence helpers now have a logged public usage flow, but the broader upstream audit + operator story is still narrower here |
| `bytes` | `substantive` | `include/asx/bytes/*.h`, `src/bytes/*.c`, buffer/codec APIs | Strong local byte/buffer surface with tests; still much smaller than full upstream ecosystem |
| `cancel` | `substantive` | `include/asx/core/cancel.h`, runtime cancellation integration, lifecycle tests/docs | Core cancel protocol is implemented and heavily verified |
| `channel` | `substantive` | `include/asx/core/channel.h`, `broadcast.h`, `oneshot.h`, `session.h`, `watch.h`; `src/channel/*.c`; unit tests | MPSC plus related channel families are real, not placeholders |
| `codec` | `partial` | `include/asx/codec/*.h`, `include/asx/bytes/codec.h`, `src/runtime/equivalence.c`, `src/bytes/codec.c` | Canonical fixture and framing codecs exist; far narrower than all upstream codec/application encoding surfaces |
| `combinator` | `partial` | `include/asx/core/combinator.h`, `src/core/combinator*.c` | Join/race/select/timeout-style APIs exist, but not the broader upstream orchestration family |
| `config` | `partial` | `include/asx/asx_config.h`, runtime/app config structs | Compile-time/runtime config exists, but not the richer upstream config module and loaders |
| `conformance` | `partial` | `tests/conformance/`, `fixtures/rust_reference/`, `tools/ci/run_conformance.sh`, docs | Strong verification infrastructure exists, but no matching public `include/asx/conformance/...` family |
| `console` | `partial` | `include/asx/console/console.h`, `src/console/console.c`, `tests/unit/console/test_console.c`, `tests/vignettes/vignette_console.c` | A thin public operator/console family now ships over doctor/report surfaces, but it is still much smaller than upstream console/operator scope |
| `cx` | `partial` | `include/asx/cx/cx.h`, `include/asx/cx/scope.h`, `src/cx/*.c`, umbrella/header + archive build exposure | Capability context and scopes now ship publicly, but the broader upstream `Cx` ecosystem and more propagation points are still missing |
| `decoding` | `partial` | `include/asx/decoding/decoding.h`, `src/decoding/decoding.c`, `tests/unit/decoding/test_decoding.c` | DecodingPipeline, DecodingConfig, DecodingProgress, DecodingError implemented; RaptorQ integration deferred |
| `distributed` | `partial` | `include/asx/net/distributed.h`, `src/net/distributed.c`, `tests/unit/net/test_distributed.c` | A public distributed/network-coordination family now ships, but it is still far narrower than the broader upstream distributed runtime contract |
| `encoding` | `partial` | `include/asx/encoding/encoding.h`, `src/encoding/encoding.c`, `tests/unit/encoding/test_encoding.c` | EncodingPipeline, EncodingStats, EncodedSymbol, EncodingError implemented; RaptorQ integration deferred |
| `epoch` | `partial` | `include/asx/core/epoch.h`, `include/asx/core/circuit_breaker.h`, `src/core/epoch.c`, `src/core/circuit_breaker.c` | Epoch and circuit-breaker primitives exist, but the broader upstream epoch/barrier ecosystem and adjacent contracts are still narrower here |
| `error` | `partial` | `include/asx/asx_status.h`, runtime error ledger, docs | Status/error taxonomy exists, but not the richer upstream `error` module and re-export set |
| `evidence` | `partial` | `include/asx/evidence/evidence.h`, `src/evidence/evidence.c`, `include/asx/runtime/diagnostic.h` | Evidence helpers now ship as a dedicated public family, but the broader upstream evidence ecosystem is still richer |
| `evidence_sink` | `partial` | `include/asx/evidence_sink/evidence_sink.h`, `src/evidence_sink/evidence_sink.c`, `src/app/report.c` | Structured sink helpers now ship publicly with summary and NDJSON rendering, but the sink story is still narrower than upstream scope |
| `gen_server` | `partial` | `include/asx/actor/gen_server.h`, `src/actor/gen_server.c`, `tests/unit/actor/test_gen_server.c` | A distinct public gen-server family now exists, but it remains much smaller than the broader upstream gen_server contract |
| `http` | `partial` | `include/asx/net/http.h`, `src/net/http.c`, `tests/unit/net/test_http.c` | A shipped HTTP family exists under the `net/` surface, but it is still a reduced deterministic skeleton rather than the broader upstream HTTP stack |
| `io` | `partial` | `include/asx/bytes/io_adapter.h`, `include/asx/runtime/io_driver.h`, `src/runtime/io_driver.c` | Some I/O adapters exist, but no broad public async-IO module matching upstream |
| `lab` | `partial` | `include/asx/runtime/lab.h`, `include/asx/runtime/replay.h`, `include/asx/runtime/snapshot.h`, `src/runtime/replay.c` | Deterministic lab/replay exists, but much narrower than upstream lab/oracle/explorer tooling |
| `link` | `partial` | `include/asx/link/link.h`, `src/link/link.c`, `tests/unit/link/test_link.c`, `tests/vignettes/vignette_link.c` | Public coordination link now exists on top of shipped sessions, but it is still far smaller than upstream cross-runtime/linkage scope |
| `migration` | `partial` | `include/asx/migration/migration.h`, `src/migration/migration.c`, `tests/unit/migration/test_migration.c` | Version checking, wire-format compatibility, migration reports, and feature availability probes implemented |
| `monitor` | `partial` | `include/asx/monitor/monitor.h`, `src/monitor/monitor.c`, `tests/unit/monitor/test_monitor.c`, `tests/vignettes/vignette_observability.c` | Threshold-based monitor evaluation now exists over runtime inspection and watchdog state, but it is still a reduced public monitor family |
| `net` | `partial` | `include/asx/net/net.h`, `src/net/net.c`, `src/net/pipe.c`, `tests/unit/app/test_app.c`, `tests/unit/net/test_pipe.c`, `tests/vignettes/vignette_network.c`, `tests/e2e/e2e_network_surface.c` | Public networking now includes deterministic in-memory loopback accept/read/write, UDP delivery, resolver ordering, and bounded pipe-backed lifecycle/backpressure evidence with raw/replay artifacts, but it is still not a full OS socket stack or near full upstream breadth |
| `obligation` | `partial` | `include/asx/obligation/obligation.h`, runtime obligation lifecycle, `tests/vignettes/vignette_obligations.c`, `tests/vignettes/vignette_link.c` | Dedicated public helpers now mirror the obligation family, but the broader upstream proof/lifecycle surface is still richer |
| `observability` | `partial` | `include/asx/observability/observability.h`, `src/observability/observability.c`, diagnostics/telemetry/reporting sources | Useful diagnostics now have an explicit observability snapshot family, but the broader upstream observability surface is still larger |
| `plan` | `partial` | `include/asx/plan/plan.h`, `src/plan/plan.c` | Plan DAG/IR surface exists in reduced form |
| `raptorq` | `stub` | `include/asx/raptorq/raptorq.h`, `src/raptorq/raptorq.c`, `tests/unit/raptorq/test_raptorq.c` | Fail-closed stub with config types, readiness probe, deferral reason, and stub encode/decode (per DEF-009 Wave D) |
| `record` | `partial` | `include/asx/record/record.h`, runtime event/snapshot capture, `tests/unit/record/test_record.c`, `tests/vignettes/vignette_link.c` | Public record helpers now expose snapshot and event-log summaries, but not the full upstream record/history ecosystem |
| `remote` | `partial` | `include/asx/remote/remote.h`, `src/remote/remote.c`, `tests/unit/remote/test_remote.c` | A public remote family now ships, but it remains a reduced subset of the broader upstream remote/idempotency/saga story |
| `runtime` | `substantive` | `include/asx/runtime/*.h`, `src/runtime/*.c`, extensive runtime tests/docs | This is the strongest implemented family after core/channel/time/trace |
| `security` | `partial` | `include/asx/security/*.h`, `src/security/*.c`, `tests/vignettes/vignette_security.c` | Security/audit primitives and logged authenticated flows exist, but the full upstream security scope is still broader |
| `service` | `partial` | `include/asx/service/service.h`, `src/service/service.c`, `tests/unit/service/test_service.c`, `tests/unit/service/test_service_stack.c` | A public service family now ships, but it is still much smaller than the broader upstream service-builder/service-stack ecosystem |
| `session` | `partial` | `include/asx/session/session.h`, `include/asx/core/session.h`, `src/channel/session.c`, `tests/unit/channel/test_session.c`, `tests/vignettes/vignette_link.c` | Session is now a shipped standalone public family, but it remains a reduced subset of the upstream session/orchestration story |
| `spork` | `partial` | `include/asx/spork/spork.h`, `src/spork/spork.c`, `tests/unit/spork/test_spork.c` | A public spork/orchestration family now exists, but it remains narrower than the broader upstream operator/orchestration scope |
| `stream` | `partial` | `include/asx/stream/stream.h`, `src/stream/stream.c` | Stream utilities exist, but not the full upstream streaming ecosystem |
| `supervision` | `partial` | `include/asx/actor/supervisor.h`, `src/actor/supervisor.c` | Real supervisor logic exists, but still much smaller than upstream supervision families |
| `sync` | `partial` | `include/asx/sync/*.h`, `src/sync/*.c` | Mutex/once/semaphore/barrier/notify exist, but not the full upstream sync surface |
| `time` | `substantive` | `include/asx/time/*.h`, `src/time/*.c`, timer tests/docs | Timer wheel, deadline, and sleep surfaces are real and verified |
| `trace` | `substantive` | `include/asx/runtime/trace.h`, event log/telemetry/hindsight/reporting, conformance docs/tests | Trace/replay evidence is a real product surface here |
| `tracing_compat` | `partial` | `include/asx/tracing_compat/tracing_compat.h`, `src/tracing_compat/tracing_compat.c`, `tests/unit/tracing_compat/test_tracing_compat.c` | A lightweight compatibility export over the deterministic trace ring now exists, but it is not a full upstream tracing integration ecosystem |
| `transport` | `partial` | `include/asx/transport/transport.h`, `src/transport/transport.c`, `tests/unit/transport/test_transport.c` | A public transport abstraction now ships, but it is still a reduced subset of the broader upstream transport ecosystem |
| `types` | `partial` | `include/asx/asx_ids.h`, `asx_status.h`, `asx_config.h`, `asx_abi.h`, `abi/wasm_abi.h` | Many foundational value types exist, but not the broad upstream `types` export set |
| `util` | `absent` | No public `include/asx/util/` family | Internal helpers exist, but no exported util module |
| `web` | `partial` | `include/asx/net/web.h`, `src/net/web.c`, `tests/unit/net/test_web.c` | A shipped web family exists under the `net/` surface, but it is still a reduced deterministic framework relative to the broader upstream web/browser/server stack |

### 11.2 Feature-Gated and Platform-Gated Upstream Families

| Upstream family | Current ANSI C state | Evidence in this repo | Gap / dependency note |
|---|---|---|---|
| `cli` | `partial` | `include/asx/cli/cli.h`, `src/cli/cli.c`, `tests/unit/cli/test_cli.c` | A standalone public CLI family now ships, but it is still much smaller than the broader upstream CLI/operator ecosystem |
| `database` | `partial` | `include/asx/net/db.h`, `src/net/db.c`, `tests/unit/net/test_db.c` | A shipped database family exists under the `net/` surface, but it remains a deterministic reduced skeleton rather than the broader upstream database client stack |
| `tls` | `partial` | `include/asx/net/tls.h`, `src/net/tls.c`, `tests/unit/net/test_tls.c` | A shipped TLS family exists under the `net/` surface, but it remains much smaller than the broader upstream transport-security contract |
| `fs` | `partial` | `include/asx/fs/fs.h`, `src/fs/fs.c` deterministic in-memory host surface | Useful API exists, but it is not a native OS filesystem integration layer |
| `grpc` | `partial` | `include/asx/net/grpc.h`, `src/net/grpc.c`, `tests/unit/net/test_grpc.c` | A shipped gRPC family exists under the `net/` surface, but it is still a reduced deterministic scaffold rather than the broader upstream gRPC stack |
| `messaging` | `partial` | `include/asx/net/messaging.h`, `src/net/messaging.c`, `tests/unit/net/test_messaging.c` | A shipped messaging/broker family exists under the `net/` surface, but it remains much smaller than the broader upstream messaging ecosystem |
| `process` | `partial` | `include/asx/process/process.h`, `src/process/process.c` deterministic child-process model | Public process API exists, but it is not a full host-process integration layer |
| `server` | `partial` | `include/asx/net/server.h`, `src/net/server.c`, `tests/unit/net/test_server.c` | A standalone shipped server family now exists under the `net/` surface, but it remains much smaller than the broader upstream server substrate |
| `signal` | `partial` | `include/asx/signal/signal.h`, `src/signal/signal.c` deterministic signal subscription/raise logic | Real API exists, but it is not native signal integration |

### 11.3 Adjacent Contract Surfaces That Still Exceed the C Tree

| Upstream contract surface | Current ANSI C state | Evidence in this repo | Gap / dependency note |
|---|---|---|---|
| Browser/WASM product surface | `partial` | `include/asx/abi/wasm_abi.h`, `include/asx/runtime/browser_boundary.h`, `include/asx/runtime/browser_diagnostic.h`, `include/asx/net/web.h`, examples `ex_browser_*` | ABI/boundary tooling and a shipped web-facing browser-safe family now exist, but the broader upstream browser/WASM product surface and fail-closed feature matrix are still incomplete |
| Examples and smoke paths | `partial` | `examples/*.c`, `tests/e2e/*.sh`, `README.md` examples | Good kernel/lab/browser-boundary examples exist; HTTP/web/grpc/database/distributed example families do not |
| Doctor/report/evidence workflow | `partial` | `include/asx/app/report.h`, `app/doctor.h`, `runtime/diagnostic.h`, `include/asx/evidence*.h`, `include/asx/monitor/monitor.h`, `src/app/*.c`, `tests/vignettes/vignette_console.c`, `tests/vignettes/vignette_observability.c` | Strong diagnostic story locally, now with explicit evidence/monitor families and logged observability flows, but still narrower than upstream evidence/observability contract |
| Test-helper / artifact surface | `partial` | `tests/test_log.h`, `tests/test_harness.h`, `include/asx/testing/log.h`, `tools/fixture_capture`, `fixtures/rust_reference/` | Verification tooling is strong and now has a public test-log helper, but it still is not a full public `test_logging` / `test_ndjson` / `test_utils` module family |

### 11.4 Bottom-Line Roadmap Implications

The current ANSI C tree is no longer just a tiny kernel spike. It already has
substantive runtime, channel, time, trace, byte-buffer, and selected actor/app
surfaces. That said, crate-level parity is still nowhere close to closure:

- the implementation is strongest in kernel semantics, deterministic replay,
  diagnostics, and a handful of host-style facades;
- several named families exist only in narrowed form (`cx`, `lab`, `plan`,
  `sync`, `stream`, `actor`, `supervision`, `app`, `security`);
- the current shipped surface is broader than the older kernel-only narrative,
  but many of those shipped families are still intentionally reduced or
  profile-gated rather than close to full upstream breadth;
- networking is still explicitly skeletal rather than production-capable;
- some major upstream families still remain entirely absent or materially
  underrepresented, but the absent set is now much smaller; the real gap is
  parity depth across shipped families plus truly missing ecosystems such as
  `util` and broader distributed/browser/platform stacks.

This is the truth-maintenance result that the reopened `bd-yx9r.*` roadmap
needs to preserve: earlier deferred-surface documents correctly explained why
many families were postponed, but they did not themselves prove that crate-level
parity claims had become misleading relative to the current upstream export
surface.
