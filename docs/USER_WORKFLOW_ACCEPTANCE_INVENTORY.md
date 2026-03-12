# User Workflow and Acceptance Inventory

> **Bead:** `bd-1eqo.1.2`  
> **Status:** Canonical user-facing workflow, persona, and acceptance-slice map
> for substantive `/dp/asupersync` parity  
> **Last updated:** 2026-03-12 by CrimsonHarbor

This document complements [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](./RUST_EXPORTED_SURFACE_INVENTORY.md).

That inventory answers:

> What upstream modules and contracts exist?

This document answers:

> Which real user workflows make those modules valuable, and what would make
> parity feel incomplete or hostile even if the raw symbols exist?

The point is to prevent the roadmap from converging on API-count parity while
still failing the workflows that users actually rely on to adopt, debug, and
operate the system.

## 1. Normative Inputs

Primary upstream sources reviewed:

- `/dp/asupersync/README.md`
- `/dp/asupersync/src/lib.rs`

Primary local planning contracts that this inventory should inform:

- `docs/RUST_EXPORTED_SURFACE_INVENTORY.md`
- `docs/SUBSYSTEM_EVIDENCE_MATRIX.md`
- `docs/FEATURE_PARITY.md`
- `docs/QUALITY_GATES.md`
- `docs/TEST_LOG_SCHEMA.md`

## 2. Planning Rules

For roadmap purposes, a workflow is considered first-class if at least one of
the following is true:

1. it appears in the upstream README as a usage path, comparison path, or
   operational promise,
2. it is backed by dedicated upstream docs/guides/examples,
3. it is necessary to make a public guarantee believable in practice,
4. it is a required success or failure loop for operators, browser consumers,
   framework integrators, or test authors.

Every first-class workflow must have:

- a primary persona,
- a minimum "happy path" acceptance slice,
- an explicit failure or recovery slice when the upstream contract is about
  robustness, diagnostics, or fail-closed behavior,
- a roadmap track owner,
- an eventual e2e or logged smoke representation.

## 3. Personas

The upstream project implies multiple distinct user types. The roadmap should
optimize for all of them, not only the systems programmer writing task code.

### 3.1 Runtime Integrator

Primary mission:

- adopt the runtime as the foundation for a new or migrated async system.

What they need:

- clear entry path from install to first working structured-concurrency example,
- ergonomic access to `Cx`, `Scope`, tasks, cancellation, channels, timers,
  and core types,
- confidence that host integration, service layering, networking, and config
  are operable.

What makes parity feel hostile:

- kernel primitives exist but there is no coherent “build a real service”
  workflow,
- examples stop at toy snippets and do not connect to runtime configuration,
  shutdown, or networking,
- the model differs from tokio but the migration path is undocumented.

### 3.2 Test Author / Determinism User

Primary mission:

- turn concurrency behavior into reproducible tests and replayable failures.

What they need:

- lab runtime setup,
- deterministic scheduling and virtual time,
- oracles, crashpacks, replay commands, and artifact bundles,
- clear interpretation of deterministic failures and schedule exploration.

What makes parity feel hostile:

- lab APIs exist but there is no stable replay/debug loop,
- tests can fail but do not emit structured evidence,
- deterministic claims exist in prose but cannot be demonstrated end-to-end.

### 3.3 Systems Engineer / Performance-Conscious Adopter

Primary mission:

- rely on cancellation correctness, bounded cleanup, fairness, and runtime
  control surfaces in demanding environments.

What they need:

- runtime controls, budgets, deadlines, scheduler behavior, reactor semantics,
  and synchronization behavior that are explainable and observable,
- clear coverage of native/network/database/protocol surfaces when those are
  part of the decision to adopt.

What makes parity feel hostile:

- correctness claims exist, but runtime behavior under pressure has no visible
  evidence model,
- host/network stacks are incomplete enough that the runtime cannot back a real
  service,
- operational warnings exist only as internal counters rather than usable
  diagnostics.

### 3.4 Operator / Diagnostic User

Primary mission:

- investigate failures, stalls, leaked obligations, and misconfiguration
  without reverse-engineering the runtime internals.

What they need:

- doctor/report/logging flows,
- deterministic evidence ingestion and report packaging,
- actionable failure summaries, replay pointers, and remediation guidance.

What makes parity feel hostile:

- logs exist but are not structured,
- failure artifacts are incomplete or non-deterministic,
- there is no clear operator loop from symptom to evidence to recommended next
  action.

### 3.5 Browser / JS-TS / Framework Consumer

Primary mission:

- use Browser Edition safely from JavaScript/TypeScript or frameworks such as
  React and Next.js.

What they need:

- browser-specific quick starts,
- compatibility and anti-pattern guidance,
- React/Next acceptance flows,
- fail-closed errors when crossing forbidden native/browser boundaries,
- deterministic browser replay and troubleshooting guidance.

What makes parity feel hostile:

- wasm types exist but there is no user-facing Browser Edition workflow,
- browser profiles succeed only in narrow demos,
- diagnostics do not teach the user why something is unsupported or how to
  recover.

### 3.6 Advanced Platform / Distributed User

Primary mission:

- use remote/distributed, saga, messaging, RaptorQ, gRPC, or similar higher
  layers where available.

What they need:

- explicit acknowledgement that these are real product lanes,
- scenario-driven validation rather than raw module presence,
- trustworthy evidence and compatibility guidance when features are partial or
  gated.

What makes parity feel hostile:

- public modules are exported, but roadmap planning treats them as accidental,
- feature-gated surfaces have no canonical “when should I use this?” slices,
- advanced flows lack example packs or failure guidance.

## 4. Workflow Families

### 4.1 Install, Orient, and Get to First Success

Representative upstream signals:

- README quick install
- quick example
- core exports section
- dependency and feature-flag guidance

Primary personas:

- runtime integrator
- browser consumer

Minimum acceptance slice:

- user can identify the correct installation path,
- user can run or understand a first structured-concurrency example,
- user can tell which profile/features they need,
- user can discover the relevant docs without scavenger hunting.

Failure slice:

- unsupported profile/feature combinations fail clearly,
- the docs explain why a path is not supported, not just that it failed.

Roadmap owners:

- `bd-1eqo.1.2`
- `bd-1eqo.9.3`
- `bd-1eqo.16.*`
- `bd-1eqo.14.*`

### 4.2 Structured Task Ownership and Scope Closure

Representative upstream signals:

- README quick example
- “no orphan tasks”
- `Cx` / `Scope`
- tokio comparison/migration section

Primary personas:

- runtime integrator
- systems engineer

Minimum acceptance slice:

- spawn owned tasks,
- await scope/region closure,
- observe that work is not orphaned,
- understand the migration difference from detached-task models.

Failure slice:

- invalid scope/capability usage is surfaced clearly,
- cancellation or sibling failure propagation is visible in logs/results.

Roadmap owners:

- `bd-1eqo.3.*`
- `bd-1eqo.4.*`
- `bd-1eqo.7.*`

### 4.3 Deadline, Timeout, and Bounded Cleanup Control

Representative upstream signals:

- timeout/sleep/interval mappings
- budget semantics
- cancel protocol and progress certificates

Primary personas:

- systems engineer
- runtime integrator
- test author

Minimum acceptance slice:

- configure deadlines/budgets,
- trigger timeout or cancellation,
- observe bounded cleanup behavior,
- inspect the outcome and supporting diagnostics.

Failure slice:

- stalled cleanup, deadline warnings, and slow-tail drain behavior remain
  explainable and evidence-backed.

Roadmap owners:

- `bd-1eqo.5.*`
- `bd-1eqo.11.*`

### 4.4 Channel and Sync Coordination Without Silent Loss

Representative upstream signals:

- two-phase send examples and tokio mapping
- channel and sync primitive sections

Primary personas:

- runtime integrator
- systems engineer

Minimum acceptance slice:

- user can wire MPSC/oneshot/broadcast/watch or sync primitives into a real
  coordination flow,
- cancellation during reserve/acquire does not silently lose work,
- wake/fairness behavior is semantically consistent.

Failure slice:

- closed-peer, lag, cancelled waiter, and fairness edge cases are testable and
  diagnosable.

Roadmap owners:

- `bd-1eqo.6.*`
- `bd-1eqo.13.*`

### 4.5 Deterministic Lab Investigation and Replay

Representative upstream signals:

- lab runtime quick example
- replay/debug claims
- crashpack/repro/artifact descriptions
- DPOR/oracle/evidence discussions

Primary personas:

- test author
- operator/diagnostic user
- systems engineer

Minimum acceptance slice:

- run a deterministic scenario with a seed,
- capture artifact bundle and replay metadata,
- rerun the same scenario and observe the same behavior,
- inspect oracle or evidence outputs without bespoke internal knowledge.

Failure slice:

- deterministic failures carry replay commands, manifests, and structured
  summaries,
- flake analysis and crashpack linkage exist for failure investigation.

Roadmap owners:

- `bd-1eqo.8.*`
- `bd-1eqo.11.*`
- `bd-1eqo.2.2`

### 4.6 Observability, Doctor, and Evidence-Driven Triage

Representative upstream signals:

- doctor contracts in README docs table
- evidence/logging/report contracts
- replay and diagnostics promises

Primary personas:

- operator/diagnostic user
- systems engineer

Minimum acceptance slice:

- collect workspace/runtime evidence,
- generate a deterministic report,
- inspect findings, evidence pointers, commands, and provenance,
- hand that report to another human or agent without losing context.

Failure slice:

- missing evidence, malformed inputs, and partial diagnostics are surfaced with
  deterministic error taxonomy and remediation guidance.

Roadmap owners:

- `bd-1eqo.11.*`
- `bd-1eqo.12.*`
- `bd-1eqo.14.*`

### 4.7 Build a Real Native Service

Representative upstream signals:

- networking/protocol stack
- HTTP/gRPC/server/web/process/signal/CLI surfaces
- runtime builder and host integration sections

Primary personas:

- runtime integrator
- systems engineer

Minimum acceptance slice:

- bootstrap runtime,
- bind networking or server surfaces,
- serve requests or protocol traffic,
- shut down cleanly with structured diagnostics.

Failure slice:

- stale-token/reactor issues, shutdown stalls, or host-surface incompatibility
  remain observable and recoverable.

Roadmap owners:

- `bd-1eqo.9.*`
- `bd-1eqo.14.*`
- `bd-1eqo.11.*`

### 4.8 Browser Edition Quickstart, Troubleshooting, and Framework Adoption

Representative upstream signals:

- Browser Edition README section
- JS/TS consumer quick start
- Browser docs table: quickstart, examples, troubleshooting, DX taxonomy,
  ABI policy, scheduler semantics, React patterns, Next.js cookbook, flake
  governance, evidence matrix

Primary personas:

- browser consumer
- framework integrator
- operator/diagnostic user

Minimum acceptance slice:

- select the right browser profile,
- understand what works and what does not,
- run a canonical vanilla/TS/React/Next example,
- inspect deterministic artifacts and troubleshooting guidance.

Failure slice:

- unsupported runtime combinations fail closed,
- anti-patterns such as server-side wasm misuse or forbidden native surfaces
  produce actionable diagnostics,
- browser flake/replay workflow exists, not just compile-time gating.

Roadmap owners:

- `bd-1eqo.16.*`
- `bd-1eqo.9.3`
- `bd-1eqo.11.*`

### 4.9 Service Composition, Middleware, and Application Wiring

Representative upstream signals:

- service/tower-like coverage map
- web/service/server surfaces
- plan IR and stream adapters

Primary personas:

- runtime integrator
- advanced platform user

Minimum acceptance slice:

- compose services/middleware/streams,
- preserve cancellation and diagnostics through the composition,
- expose enough examples that application wiring is not guesswork.

Failure slice:

- retries, discovery, backpressure, and middleware failures are surfaced with
  structured context instead of becoming silent composition bugs.

Roadmap owners:

- `bd-1eqo.13.*`
- `bd-1eqo.9.*`

### 4.10 Messaging, Remote, and Saga-Style Distributed Coordination

Representative upstream signals:

- remote/distributed sections
- messaging/database/protocol coverage map
- saga/idempotency/lease exports

Primary personas:

- advanced platform user
- systems engineer

Minimum acceptance slice:

- run a named remote or distributed coordination flow,
- rely on leases/idempotency/saga state transitions,
- understand how the runtime reports forward progress and rollback.

Failure slice:

- retry/dedup conflict, lease expiry, and compensation failures are explicit and
  recoverable,
- feature-gated or partial status is documented honestly.

Roadmap owners:

- `bd-1eqo.9.*`
- `bd-1eqo.10.*`
- deferred follow-ons beyond current kernel parity

### 4.11 Actor, GenServer, and Supervision Recovery Loops

Representative upstream signals:

- actor/gen_server/supervision exports
- OTP-style debugging claims

Primary personas:

- advanced platform user
- runtime integrator
- test author

Minimum acceptance slice:

- start supervised actor/gen_server flows,
- observe mailbox/restart/escalation behavior,
- replay failures deterministically.

Failure slice:

- restart ancestry, escalation, and shutdown behavior are inspectable from
  evidence artifacts rather than inferred from logs alone.

Roadmap owners:

- `bd-1eqo.10.*`
- `bd-1eqo.8.*`
- `bd-1eqo.11.*`

## 5. Workflow-to-Track Matrix

| Workflow family | Primary roadmap tracks |
|---|---|
| Install/orient/first success | `bd-1eqo.9.3`, `bd-1eqo.16.*`, `bd-1eqo.14.*` |
| Structured task ownership | `bd-1eqo.3.*`, `bd-1eqo.4.*`, `bd-1eqo.7.*` |
| Deadline/timeout/bounded cleanup | `bd-1eqo.5.*`, `bd-1eqo.11.*` |
| Channel/sync coordination | `bd-1eqo.6.*`, `bd-1eqo.13.*` |
| Deterministic lab/replay | `bd-1eqo.8.*`, `bd-1eqo.11.*`, `bd-1eqo.2.2` |
| Doctor/diagnostic triage | `bd-1eqo.11.*`, `bd-1eqo.12.*`, `bd-1eqo.14.*` |
| Native service bootstrap | `bd-1eqo.9.*`, `bd-1eqo.14.*` |
| Browser Edition adoption | `bd-1eqo.16.*`, `bd-1eqo.9.3`, `bd-1eqo.11.*` |
| Service/middleware/application wiring | `bd-1eqo.13.*`, `bd-1eqo.9.*` |
| Messaging/remote/distributed | `bd-1eqo.9.*`, `bd-1eqo.10.*` and future deferred tracks |
| Actor/gen_server/supervision | `bd-1eqo.10.*`, `bd-1eqo.8.*`, `bd-1eqo.11.*` |

## 6. What “Technically Present but User-Hostile” Looks Like

This inventory should actively prevent the following failure modes:

- `Cx`, tasks, channels, and timers exist, but there is no end-to-end path that
  feels like building a usable service.
- Deterministic lab claims exist, but replay/debugging is too thin to localize
  failures without digging through internals.
- Browser/WASM types compile, but JS/TS/React/Next workflows remain undocumented
  or non-deterministic.
- Diagnostics exist as raw logs or counters, but not as structured reports,
  evidence contracts, and replay pointers.
- Feature-gated advanced surfaces are exported, but the roadmap forgets that
  users will eventually judge parity by those paths too.
- Canonical examples and smoke paths are omitted on the assumption that docs can
  be added later.

If any of those remain true, parity is incomplete even if individual module
tests pass.

## 7. Acceptance-Slice Requirements for Future Beads

Any implementation bead under `bd-1eqo.*` should be able to answer:

1. Which persona is this bead helping right now?
2. Which workflow family becomes more complete because of it?
3. What is the minimum happy-path acceptance slice it unlocks?
4. What failure or recovery slice must be tested/logged as part of the same
   work?
5. Which example, smoke, lab, or doctor flow should eventually exercise it?

If those answers are missing, the bead is underspecified for user-facing parity.

## 8. Relationship to E2E Planning

This document is meant to feed:

- `bd-1eqo.2.2` end-to-end scenario pack planning,
- `bd-1eqo.2.6` shared e2e-harness/logging planning,
- `bd-1eqo.9.3` canonical examples and smoke-pack work,
- Browser Edition and Doctor contract branches where explicit workflow slices
  matter as much as API parity.

In short:

- [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](./RUST_EXPORTED_SURFACE_INVENTORY.md)
  says what upstream exposes.
- this document says what users are actually trying to do with it.
- [`docs/SUBSYSTEM_EVIDENCE_MATRIX.md`](./SUBSYSTEM_EVIDENCE_MATRIX.md) says
  how we prove those workflows really work.
