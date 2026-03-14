# E2E Scenario Packs and Report Contract

> **Bead:** `bd-1eqo.2.2`  
> **Status:** Roadmap-level definition of named end-to-end scenario packs and
> the required structured logging/report outputs for substantive
> `/dp/asupersync` parity  
> **Last updated:** 2026-03-12 by CrimsonHarbor

This document turns the user-workflow inventory into concrete scenario packs and
output expectations. It answers:

> Which end-to-end or smoke workflows must exist to prove this project is
> usable, and what exactly should those workflows emit so failures are
> diagnosable and replayable?

It is intentionally one layer above `tests/e2e/harness.sh`. The harness is the
mechanism; this document is the contract the harness must satisfy.

## 1. Normative Inputs

- [`docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md`](./USER_WORKFLOW_ACCEPTANCE_INVENTORY.md)
- [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](./RUST_EXPORTED_SURFACE_INVENTORY.md)
- [`docs/SUBSYSTEM_EVIDENCE_MATRIX.md`](./SUBSYSTEM_EVIDENCE_MATRIX.md)
- [`docs/RUST_PROVENANCE_CROSSWALK_AND_FIXTURE_WORKFLOW.md`](./RUST_PROVENANCE_CROSSWALK_AND_FIXTURE_WORKFLOW.md)
- [`docs/TEST_LOG_SCHEMA.md`](./TEST_LOG_SCHEMA.md)
- [`docs/QUALITY_GATES.md`](./QUALITY_GATES.md)
- [`tests/e2e/harness.sh`](/data/projects/asupersync_ansi_c/tests/e2e/harness.sh)
- `/dp/asupersync/README.md`

## 2. Contract Goals

The scenario-pack layer must prove all of the following:

1. the C port supports the user workflows that matter, not only local module
   semantics,
2. success paths are easy to understand and failure paths are easy to
   investigate,
3. every important scenario produces structured artifacts that can be consumed
   by humans and automation,
4. deterministic replay information is preserved whenever the workflow is meant
   to be replayable,
5. browser/native/operator/example flows have equal status with kernel-centric
   scenarios.

## 3. Scenario Pack Model

Each pack should be treated as a named contract bundle with:

- a stable pack ID,
- one or more scenarios,
- explicit personas and workflow families,
- required profiles and optional codecs,
- required emitted artifacts,
- golden-log expectations or replay requirements where applicable.

Each scenario should define:

- `scenario_id`
- happy-path objective
- failure-path or recovery objective
- primary persona
- governing `bd-1eqo` track(s)
- required evidence lanes

## 4. Required Pack Catalog

### 4.1 `PACK-ONBOARD-CORE`

Purpose:

- prove a new user can install, orient, and reach a first meaningful success.

Primary personas:

- runtime integrator

Representative scenarios:

- `onboard-core-install`
- `onboard-core-first-scope`
- `onboard-core-first-cancel-safe-channel`
- `onboard-core-first-timeout`

Required success slices:

- install/build path is identifiable,
- first structured-concurrency example completes cleanly,
- basic channel/time usage is visible,
- user can see where logs/artifacts land.

Required failure slices:

- invalid feature/profile choice produces a clear message,
- missing capability or unsupported path produces actionable diagnostics.

Required artifacts:

- JSONL e2e log,
- summary JSON,
- first-failure summary,
- rerun command,
- artifact manifest with paths.

### 4.2 `PACK-STRUCTURED-RUNTIME`

Purpose:

- prove the runtime’s core ownership, cancellation, and quiescence model under
  realistic end-to-end flows.

Primary personas:

- runtime integrator
- systems engineer

Representative scenarios:

- `scope-owned-task-clean-close`
- `sibling-failure-propagation`
- `cancel-request-drain-finalize`
- `deadline-triggered-bounded-cleanup`
- `quiescence-with-finalizers`

Required success slices:

- no orphan task behavior,
- explicit cancel protocol completion,
- bounded cleanup and clean close,
- visible state/result summaries.

Required failure slices:

- slow-tail or stalled cleanup diagnostics,
- invalid transition or exhausted-budget surfaces,
- retained trace/evidence pointers for debugging.

Required artifacts:

- e2e JSONL,
- scenario summary JSON,
- replay pointer if deterministic,
- structured failure object on non-pass,
- optional trace snapshot pointer.

### 4.3 `PACK-CHANNEL-SYNC`

Purpose:

- prove user-facing coordination semantics rather than only unit-level channel
  correctness.

Primary personas:

- runtime integrator
- systems engineer

Representative scenarios:

- `mpsc-reserve-send-cancel`
- `broadcast-lag-observation`
- `watch-change-propagation`
- `sync-lock-timeout-recovery`
- `notify-barrier-on-shutdown`

Required success slices:

- reserve/commit workflows are visible,
- fairness/ordering expectations are exercised,
- synchronization primitives coordinate meaningful work.

Required failure slices:

- lagged receiver,
- cancelled waiter,
- closed peer,
- timeout while waiting for coordination.

Required artifacts:

- JSONL records for every scenario,
- digest or summary token when determinism matters,
- loser/winner side diagnostics for races,
- first-failure summary with the exact scenario ID.

### 4.4 `PACK-LAB-REPLAY`

Purpose:

- prove deterministic investigation and replay loops.

Primary personas:

- test author
- operator/diagnostic user

Representative scenarios:

- `seeded-replay-stability`
- `virtual-time-timeout-sequence`
- `oracle-clean-close`
- `oracle-leak-detection`
- `crashpack-replay-linkage`

Required success slices:

- same seed produces same digest,
- replay command/manifest is emitted,
- deterministic artifacts are sufficient for follow-up investigation.

Required failure slices:

- failing run captures repro manifest,
- mismatch or oracle failure clearly names the invariant or digest drift,
- crashpack linkage survives into artifacts.

Required artifacts:

- repro manifest,
- summary JSON,
- replay command,
- optional minimized/failure bundle pointer,
- JSONL with `seed`, `digest`, and `scenario_id`.

### 4.5 `PACK-DOCTOR-DIAGNOSTICS`

Purpose:

- prove operator-facing evidence/report flows are usable.

Primary personas:

- operator/diagnostic user

Representative scenarios:

- `doctor-workspace-scan`
- `doctor-evidence-ingest`
- `doctor-report-generate`
- `doctor-remediation-recipe`
- `doctor-partial-evidence-failure`

Required success slices:

- operator can go from raw inputs to a structured report,
- evidence sources are preserved and attributed,
- suggested next actions are visible.

Required failure slices:

- malformed or partial evidence is reported deterministically,
- missing files/unsupported inputs do not degrade into vague errors.

Required artifacts:

- doctor event log,
- report JSON,
- summary JSON,
- evidence manifest,
- failure recipe or diagnostic object when non-pass.

### 4.6 `PACK-NATIVE-HOST`

Purpose:

- prove native service/bootstrap/shutdown workflows, not only primitive APIs.

Primary personas:

- runtime integrator
- systems engineer

Representative scenarios:

- `native-server-bootstrap`
- `signal-driven-shutdown`
- `process-child-cleanup`
- `fs-config-load-and-run`
- `host-surface-unsupported-profile`

Required success slices:

- service boots and shuts down cleanly,
- native hooks integrate with runtime and diagnostics,
- profile selection is visible in results.

Required failure slices:

- unsupported host surface or profile mismatch fails closed,
- shutdown problems remain diagnosable with retained evidence.

Required artifacts:

- JSONL e2e log,
- native smoke summary,
- shutdown report,
- artifact manifest,
- rerun command.

### 4.7 `PACK-BROWSER-CORE`

Purpose:

- prove Browser Edition is a real product lane with deterministic examples and
  diagnostics.

Primary personas:

- browser / JS-TS consumer
- framework integrator

Representative scenarios:

- `browser-quickstart-vanilla`
- `browser-typescript-basic`
- `browser-react-task-group`
- `browser-nextjs-bootstrap`
- `browser-unsupported-native-surface`

Required success slices:

- user can select a browser profile,
- canonical browser examples run,
- browser-oriented artifacts and troubleshooting hints are retained.

Required failure slices:

- forbidden native-only boundary use,
- unsupported browser profile combinations,
- anti-pattern-triggered diagnostics.

Required artifacts:

- browser smoke summary,
- compatibility matrix output,
- repro manifest,
- error-taxonomy fields for failures,
- artifact pointers suitable for docs/tutorials.

### 4.8 `PACK-SERVICE-COMPOSITION`

Purpose:

- prove middleware/stream/plan/service composition is usable and observable.

Primary personas:

- runtime integrator
- advanced platform user

Representative scenarios:

- `service-retry-discovery-flow`
- `stream-backpressure-composition`
- `plan-ir-execution-summary`
- `middleware-error-propagation`

Required success slices:

- composition semantics are visible at user level,
- policy decisions and retries are observable,
- backpressure or ordering assumptions remain intact.

Required failure slices:

- retries exhausted,
- discovery or middleware failure surfaced with context,
- composition drift visible in summary output.

Required artifacts:

- e2e JSONL,
- service-flow summary,
- policy/middleware decision trace pointer,
- structured failure summary.

### 4.9 `PACK-ADVANCED-DISTRIBUTED`

Purpose:

- keep remote/distributed/advanced ecosystem lanes visible as future parity
  obligations instead of letting them disappear from the plan.

Primary personas:

- advanced platform user

Representative scenarios:

- `remote-spawn-basic`
- `lease-renew-expire`
- `idempotent-retry-collision`
- `saga-forward-and-compensate`

Required success slices:

- remote/distributed behavior is scenario-shaped and not just module presence,
- idempotency/lease/saga state changes are legible.

Required failure slices:

- compensation failure,
- duplicate request rejection,
- lease expiry visibility.

Required artifacts:

- scenario summary,
- structured transition log,
- evidence manifest,
- replay or inspection hints where applicable.

This pack is allowed to remain deferred longer than the earlier packs, but it
should still be specified now so the roadmap does not forget it.

## 5. Required Output Contract

Every e2e/scenario pack run should emit, at minimum:

1. **JSONL event log**  
   compatible with `docs/TEST_LOG_SCHEMA.md` and marked as `layer = "e2e"`.
2. **Pack summary JSON**  
   counts, scenario statuses, profile/codec/seed context, and overall verdict.
3. **First-failure summary**  
   stable machine-readable summary naming the first failing scenario and the key
   diagnostic fields.
4. **Artifact manifest**  
   file paths, kinds, retention hints, and which scenario produced them.
5. **Rerun command**  
   a concrete shell command that reproduces the pack or scenario.

When applicable, packs must also emit:

- `repro_manifest.json`
- replay pointer or replay command
- digest field(s)
- compatibility matrix output
- report JSON or doctor findings bundle
- trace snapshot pointer

## 6. Scenario Record Requirements

At scenario granularity, records should preserve:

- `scenario_pack`
- `scenario_id`
- `persona`
- `workflow_family`
- `profile`
- `codec`
- `seed` when deterministic
- `status`
- `digest` when meaningful
- `artifacts[]`
- `error` object on failure

These fields may be emitted via the shared harness implementation later, but the
contract itself belongs here.

## 7. Success and Failure Symmetry

Scenario packs are incomplete if they only exercise happy paths.

For each pack, at least one of the following must be planned:

- must-fail scenario,
- degraded-mode or recovery scenario,
- unsupported-configuration scenario,
- evidence-rich failure scenario,
- replay or crashpack scenario.

The goal is to ensure the project can explain failures, not merely survive them.

## 8. Golden-Log and Replay Expectations

This bead does not define the full harness/golden-log strategy; that belongs to
`bd-1eqo.2.6`. But it does set the decision boundary:

- packs with deterministic sequencing, compatibility matrices, or report shapes
  should expect golden-log or golden-summary assertions,
- packs centered on replay claims must emit reproducible seed/repro data,
- packs intended as canonical examples must keep outputs stable enough to be
  tutorial-quality and smoke-testable.

The following packs are presumptively golden-log sensitive:

- `PACK-ONBOARD-CORE`
- `PACK-LAB-REPLAY`
- `PACK-DOCTOR-DIAGNOSTICS`
- `PACK-BROWSER-CORE`
- `PACK-NATIVE-HOST`

## 9. Mapping to Roadmap Tracks

| Pack | Primary roadmap consumers |
|---|---|
| `PACK-ONBOARD-CORE` | `bd-1eqo.9.3`, `bd-1eqo.14.*`, `bd-1eqo.16.*` |
| `PACK-STRUCTURED-RUNTIME` | `bd-1eqo.3.*`, `bd-1eqo.4.*`, `bd-1eqo.5.*` |
| `PACK-CHANNEL-SYNC` | `bd-1eqo.6.*`, `bd-1eqo.13.*` |
| `PACK-LAB-REPLAY` | `bd-1eqo.8.*`, `bd-1eqo.11.*` |
| `PACK-DOCTOR-DIAGNOSTICS` | `bd-1eqo.11.*`, `bd-1eqo.12.*`, `bd-1eqo.14.*` |
| `PACK-NATIVE-HOST` | `bd-1eqo.9.*`, `bd-1eqo.14.*` |
| `PACK-BROWSER-CORE` | `bd-1eqo.16.*`, `bd-1eqo.9.3`, `bd-1eqo.11.*` |
| `PACK-SERVICE-COMPOSITION` | `bd-1eqo.13.*`, `bd-1eqo.9.*` |
| `PACK-ADVANCED-DISTRIBUTED` | `bd-1eqo.9.*`, `bd-1eqo.10.*`, future distributed follow-ons |

## 10. Reopened Crate-Level Surface Contract (`bd-yx9r.3`)

The earlier `bd-1eqo.*` scenario-pack contract covered the kernel-centered
roadmap. The reopened crate-parity pass needs one more rule:

> no reopened subsystem family may begin implementation work without a default
> end-to-end evidence shape that names its happy path, degraded path, failure
> path, emitted artifacts, and any required differential-fixture lane.

This section defines that default shape so `bd-yx9r.*` child beads do not
recreate the old failure mode where a surface was "present" but had no honest
crate-level acceptance contract.

### 10.1 Default Family-to-Pack Mapping

| Reopened family | Default pack(s) | Minimum named paths before coding starts | Required artifacts beyond the shared minimum |
|---|---|---|---|
| Core public/runtime completion (`bd-yx9r.4`, `bd-yx9r.6`) | `PACK-STRUCTURED-RUNTIME`, `PACK-LAB-REPLAY` | happy: clean bootstrap/use/close; degraded: constrained-resource or compatibility fallback; failure: invalid transition, exhausted budget, or replay drift | replay pointer, transition summary, failure-classification object |
| Capability/security/evidence (`bd-yx9r.5`) | `PACK-STRUCTURED-RUNTIME`, `PACK-DOCTOR-DIAGNOSTICS` | happy: authority flow accepted; degraded: reduced-authority or audit-only mode; failure: fail-closed authority breach or evidence sink refusal | audit artifact, evidence manifest, remediation recipe when non-pass |
| Combinator/service/transport/remote/spork (`bd-yx9r.7`) | `PACK-SERVICE-COMPOSITION`, `PACK-ADVANCED-DISTRIBUTED` | happy: composed workflow succeeds; degraded: retry/discovery/backpressure fallback; failure: winner/loser mismatch, exhausted retries, or remote contract break | policy-decision trace, composition summary, remote inspection hint |
| Network core and advanced transport (`bd-yx9r.8`, `bd-yx9r.9`) | `PACK-NATIVE-HOST`, `PACK-ADVANCED-DISTRIBUTED` | happy: connect/exchange/shutdown flow; degraded: fallback transport, timeout, or partial capability mode; failure: connection refusal, protocol mismatch, lease expiry, or shutdown stall | transport transcript, shutdown report, network failure digest |
| HTTP/web/gRPC application surfaces (`bd-yx9r.10`) | `PACK-SERVICE-COMPOSITION`, `PACK-NATIVE-HOST`, `PACK-BROWSER-CORE` when browser-facing | happy: request/response or route flow; degraded: middleware fallback, downgraded protocol, or unsupported feature report; failure: routing/marshal/auth rejection with retained context | request-flow summary, route/middleware trace, compatibility matrix output when browser/native boundaries matter |
| Data, messaging, and distributed systems (`bd-yx9r.11`) | `PACK-ADVANCED-DISTRIBUTED`, `PACK-DOCTOR-DIAGNOSTICS` | happy: client/broker/store round-trip; degraded: retry, rebalance, or read-only/snapshot mode; failure: dedup conflict, compensation failure, consistency breach, or broker disconnect | transition log, state snapshot pointer, operator-facing failure recipe |
| Developer/operator/acceptance surfaces (`bd-yx9r.12`) | `PACK-ONBOARD-CORE`, `PACK-DOCTOR-DIAGNOSTICS`, `PACK-BROWSER-CORE` or `PACK-NATIVE-HOST` as applicable | happy: documented example or tool run succeeds; degraded: partial workspace or limited profile still yields actionable output; failure: unsupported profile, malformed input, or missing artifact is explained | tutorial-quality summary, rerun command, docs-safe artifact bundle |
| Actor/gen_server/supervision (`bd-yx9r.19`) | `PACK-ADVANCED-DISTRIBUTED`, `PACK-LAB-REPLAY` | happy: supervised workflow converges; degraded: restart with bounded service loss; failure: escalation, mailbox violation, or non-replayable recovery | ancestry/restart report, mailbox trace, replay manifest |

### 10.2 Differential-Fixture Rules

Differential fixtures are required when a reopened family claims any of the
following:

- semantic equivalence to the Rust reference for a user-visible flow,
- identical canonical digest across codecs or profiles for a shared scenario,
- browser/native parity for the same workflow family,
- deterministic replay or trace stability across repeated executions.

At minimum, those beads must define:

- one canonical scenario ID shared between Rust and C when Rust coverage exists,
- the profile and codec matrix being compared,
- the exact digest, summary, or event-shape fields that must remain equal,
- where minimized counterexamples or first-failure artifacts are retained.

### 10.3 Feature-Gated and Platform-Specific Exclusion Rules

Justified exclusions are acceptable only when all of the following are true:

1. the excluded surface is explicitly absent, stub-only, or platform-forbidden
   in [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](./RUST_EXPORTED_SURFACE_INVENTORY.md)
   or
   [`docs/PROFILE_RESOURCE_CLASS_CAPABILITY_MATRIX.md`](./PROFILE_RESOURCE_CLASS_CAPABILITY_MATRIX.md),
2. the bead records which scenario pack would have applied if the surface were
   implemented,
3. the pack still includes an unsupported/degraded scenario proving the boundary
   fails closed with actionable diagnostics,
4. the artifact manifest preserves the exclusion rationale and the exact gate
   that triggered it.

Examples that normally require an exclusion record rather than silent omission:

- browser builds rejecting native host, filesystem, process, signal, or other
  native-only surfaces,
- browser or minimal-profile builds rejecting TLS, database, messaging, or web
  surfaces,
- native builds rejecting browser-only cookbook or compatibility scenarios,
- partial-in-tree but not-shipped modules where the pack must document why the
  source is insufficient for crate-level acceptance.

### 10.4 Closure Implication

No `bd-yx9r.*` child bead that reopens a crate-level family should close
without pointing back to this section and naming:

- the governing scenario pack(s),
- at least one happy, degraded, and failure scenario,
- required emitted artifacts,
- whether a differential fixture is required or explicitly excluded with
  rationale.

That rule is what makes final crate-level parity acceptance auditable instead of
implicit.

## 11. What This Bead Must Prevent

This scenario-pack contract exists to prevent:

- implementation beads shipping with unit tests but no user-facing smoke path,
- browser/native/operator flows being added ad hoc with inconsistent artifact
  shapes,
- examples that are impossible to validate deterministically,
- logs that are technically structured but not rich enough to diagnose failure,
- e2e suites that only prove “did not crash” rather than “is usable and
  explainable.”

## 12. Review Checklist

Before closing a future implementation bead that touches user-facing workflows,
contributors should confirm:

1. which scenario pack(s) it should eventually satisfy,
2. which success and failure slices it adds or changes,
3. which required artifacts must be emitted,
4. whether golden-log or replay-sensitive output is involved,
5. whether the change should also update canonical example or browser/operator
   smoke expectations.

If those answers are unclear, the implementation bead is under-specified for
end-to-end acceptance.
