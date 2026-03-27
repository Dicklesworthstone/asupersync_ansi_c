# Subsystem Evidence Matrix

> **Bead:** `bd-1eqo.2.1`  
> **Status:** Canonical roadmap-level evidence matrix for substantive `/dp/asupersync` parity  
> **Last updated:** 2026-03-12 by CrimsonHarbor

This document defines the verification contract for the `bd-1eqo` roadmap.
It sits above the existing module-level completeness matrix and answers a
different question:

- `docs/TEST_COMPLETENESS_MATRIX.md` answers: "What must each low-level module
  prove?"
- this document answers: "What proof lanes must each roadmap track deliver
  before we can honestly call that user-facing surface done?"

The point is to stop future implementation beads from shipping with uneven
evidence. A subsystem is not complete because it has some unit tests. It is
complete when it has the evidence mix that matches the kind of promise being
made to users.

## 1. Normative Inputs

- `docs/TEST_COMPLETENESS_MATRIX.md`
- `docs/TEST_LOG_SCHEMA.md`
- `docs/QUALITY_GATES.md`
- `docs/FEATURE_PARITY.md`
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`
- `docs/PLAN_EXECUTION_TRACEABILITY_INDEX.md`
- `tests/e2e/harness.sh`
- `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`
- `/dp/asupersync/README.md`
- `/dp/asupersync/src/lib.rs`

## 2. Evidence Lanes

Every roadmap track must classify evidence across these lanes.

| Lane | Meaning | Typical output |
|---|---|---|
| `unit` | Focused API and local logic correctness | unit test binary + structured test logs |
| `law` | Algebraic, ordering, transition, or protocol-law validation | invariant tests, transition matrices, property-style tests |
| `lab` | Deterministic scenario execution in seeded or virtual-time mode | lab run logs, replay manifests, scenario summaries |
| `conformance` | Rust-vs-C or canonical-schema equivalence | fixture diffs, semantic digest reports, parity summaries |
| `profile` | Cross-profile or compatibility-matrix validation | profile parity matrix, build/compatibility reports |
| `e2e` | User-visible end-to-end or smoke validation | e2e scripts, example packs, CLI/browser/native smoke runs |
| `artifacts` | Structured logs, replay hints, retained evidence, summaries | JSONL logs, summary JSON, manifests, artifact pointers |

## 3. Lane Severity Rules

### 3.1 Mandatory by Default

For any implementation bead under `bd-1eqo.*`:

- `unit`: mandatory unless the bead is pure planning/governance.
- `artifacts`: mandatory for every behavioral bead.
- `e2e`: mandatory when the bead changes a user-facing workflow.

### 3.2 Mandatory When Semantics Demand It

- `law` is mandatory for state machines, algebraic contracts, scheduling,
  ordering, fairness, capability rules, ownership/witness systems, and restart
  policy.
- `lab` is mandatory for deterministic runtime behavior, replay, timing,
  cancellation, flake triage, or any flow where seeded execution is part of the
  product promise.
- `conformance` is mandatory when the Rust reference or canonical semantic
  schema is the claim being made.
- `profile` is mandatory when semantics must remain stable across profiles or
  when fail-closed compatibility rules are part of the contract.

### 3.3 "Not Applicable" Is Not Implicit

If a lane is intentionally omitted for a future bead, closure notes must say
why. "We did not get to it" is not a valid reason.

## 4. Shared Logging and Artifact Contract

Unless a bead explicitly documents a stronger subsystem-specific contract, all
behavioral tracks must inherit the shared expectations below:

1. emit structured logs compatible with `docs/TEST_LOG_SCHEMA.md`,
2. preserve deterministic seed or replay-hint data where applicable,
3. retain first-failure summaries and artifact pointers,
4. make success and failure runs both observable,
5. use the shared `tests/e2e/harness.sh` conventions or a documented extension
   of them.

## 5. Track Matrix

Legend:

- `R` = required
- `S` = required only for a meaningful subset of child beads
- `N` = normally not required at track level, but may appear on child beads

| Track | User-facing promise | unit | law | lab | conformance | profile | e2e | artifacts |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `bd-1eqo.15` foundational types/value semantics | IDs, outcomes, budgets, ownership, typed payloads, wasm envelopes behave like the Rust contracts users program against | R | R | S | S | S | S | R |
| `bd-1eqo.3` capability context / structured concurrency | explicit `Cx`, scopes, join semantics, no ambient authority | R | R | S | S | S | R | R |
| `bd-1eqo.4` runtime object / scheduler / reactor | runtime lifecycle, fairness, injectors, reactor seams, parity-safe execution | R | R | R | S | S | R | R |
| `bd-1eqo.5` time / deadlines / cancellation-driver | sleeps, timeouts, intervals, bounded cleanup, deadline interaction | R | R | R | S | S | R | R |
| `bd-1eqo.6` channels / sync | cancel-correct communication and synchronization under realistic misuse and pressure | R | R | S | S | S | R | R |
| `bd-1eqo.7` combinators / orchestration | join/race/select/quorum semantics and higher-order orchestration guarantees | R | R | R | S | S | R | R |
| `bd-1eqo.8` lab / replay / deterministic evidence | seeded deterministic execution, replay, minimization, oracle support | R | R | R | R | S | R | R |
| `bd-1eqo.9` bytes / codec / IO / app integration | realistic dataflow, CLI/app entrypoints, canonical examples, operator smoke | R | S | S | S | S | R | R |
| `bd-1eqo.11` observability / diagnostics / doctor | failures are explainable with usable reports and evidence | R | S | R | S | S | R | R |
| `bd-1eqo.12` security / audit / authority hardening | invalid authority flows fail cleanly and observably | R | R | R | S | S | R | R |
| `bd-1eqo.13` streams / plan / service composition | reusable composition layers preserve ordering, policy, and diagnostics | R | R | R | S | S | R | R |
| `bd-1eqo.14` native host / server / shutdown | host integration is operable, cancel-correct, and diagnosable | R | S | S | S | R | R | R |
| `bd-1eqo.16` Browser Edition / WASM | browser profiles, examples, diagnostics, compatibility rules remain first-class | R | R | S | S | R | R | R |
| `bd-1eqo.10` actor / gen_server / supervision | high-level actor semantics and restart policy compose with the runtime model | R | R | R | S | S | R | R |

## 6. Subsystem-Specific Expectations

### 6.1 `bd-1eqo.15` Foundational Types and Value Semantics

Required evidence patterns:

- algebraic or transition-law tests for outcomes, budgets, policy joins,
  witness rules, symbol thresholds, and ABI transition helpers,
- negative tests for stale ownership, invalid credentials, type mismatches,
  unsupported compatibility decisions,
- at least one scenario that exercises these values in realistic runtime use,
  not only in isolated helpers.

### 6.2 `bd-1eqo.3` Capability Context and Structured Concurrency

Required evidence patterns:

- unit tests proving APIs reject missing or invalid authority,
- transition or ownership-law tests for scope exit, task handles, and join
  semantics,
- end-to-end scenarios that show healthy and failing structured workflows with
  retained artifacts.

### 6.3 `bd-1eqo.4` Runtime, Scheduler, and Reactor

Required evidence patterns:

- ordering/fairness/protocol-law tests,
- lab scenarios with fixed seeds proving replay-stable scheduling,
- targeted parity checks when parallel or profile-specific paths are added,
- scenario summaries that explain why a given run order happened.

### 6.4 `bd-1eqo.5` Time and Cancellation Driver

Required evidence patterns:

- deterministic time-driver unit coverage,
- seeded lab scenarios for timeout and interval behavior,
- explicit artifacts for deadline-triggered failure paths and cleanup progress.

### 6.5 `bd-1eqo.6` Channels and Sync Primitives

Required evidence patterns:

- unit + invariant coverage for lag, closed peers, cancelled waiters, fairness,
  wake ordering, and stale observers,
- e2e or lab scenarios showing realistic coordination behavior,
- structured artifacts that help diagnose the loser/winner side of a race.

### 6.6 `bd-1eqo.7` Combinators and Orchestration

Required evidence patterns:

- law-style tests for winner selection, cancellation propagation, quorum
  thresholds, and composition ordering,
- deterministic scenario packs proving orchestration remains replayable under
  timing pressure,
- user-level examples, not just primitive combinator tests.

### 6.7 `bd-1eqo.8` Lab, Replay, and Deterministic Evidence

Required evidence patterns:

- seeded scenario packs with stable digests,
- replay/report/minimization artifacts rich enough to debug failures without
  rerunning under a debugger,
- Rust-vs-C or schema conformance where the bead claims semantic equivalence.

### 6.8 `bd-1eqo.9` Bytes, Codec, IO, App, and Canonical Examples

Required evidence patterns:

- unit coverage for buffer ownership and framing semantics,
- smoke/e2e packs for CLI/app/doctor/example workflows,
- manifest/template validation for example assets and scripts,
- artifact pointers and replay commands in every canonical example pack.

### 6.9 `bd-1eqo.11` Observability and Diagnostics

Required evidence patterns:

- unit and contract tests for report payloads and evidence sinks,
- failing e2e runs that prove diagnostics remain actionable,
- summary artifacts that a human can use to localize the problem quickly.

### 6.10 `bd-1eqo.12` Security and Audit

Required evidence patterns:

- must-fail negative tests for ambient-authority regressions,
- invariant or lab validation for cancellation + authority interaction,
- structured audit artifacts that can be inspected without custom tooling.

### 6.11 `bd-1eqo.13` Streams, Plan IR, and Services

Required evidence patterns:

- ordering/backpressure/composition law tests,
- deterministic user-level scenarios showing policy and middleware decisions,
- certificates or equivalent artifacts linking service behavior to inputs.

### 6.12 `bd-1eqo.14` Native Host Surfaces

Required evidence patterns:

- unit coverage for lifecycle/cleanup behavior around FS/process/signal hooks,
- realistic native acceptance scripts with detailed phase logging,
- profile compatibility evidence where host behavior differs operationally.

### 6.13 `bd-1eqo.16` Browser Edition / WASM

Required evidence patterns:

- compatibility-matrix tests for valid and invalid browser profile combinations,
- browser-oriented smoke packs for canonical examples and framework scenarios,
- browser-specific diagnostics/evidence artifacts, replay hints, and failure
  retention,
- explicit fail-closed tests for forbidden native-only boundaries.

### 6.14 `bd-1eqo.10` Actor / GenServer / Supervision

Required evidence patterns:

- law-style tests for mailbox ordering, restart trees, escalation, and shutdown,
- seeded lab scenarios for recovery behavior,
- human-usable evidence that explains restart decisions and failure ancestry.

## 7. Reopened Crate-Level Verification Contract (`bd-yx9r.3`)

The reopened crate-level parity stream (`bd-yx9r.*`) needs a stricter
pre-implementation contract than the earlier kernel-only closure. The inventory
and profile-matrix work now make clear that many higher-surface families are
either still absent, explicitly stub-only, or shipped only in reduced/profile-
gated form rather than close to full upstream breadth.

Before any `bd-yx9r.*` implementation bead starts, it should declare the
default evidence bundle for its family using the rules below.

### 7.1 Family-Level Default Lanes

| Reopened family | unit | law | lab | conformance | profile | e2e | artifacts | Minimum extra rule |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Public-type/config/error/cancel expansion (`bd-yx9r.4*`) | R | R | S | S | R | S | R | Must prove shipped surface matches header/build story |
| Capability/security/obligation/evidence (`bd-yx9r.5*`) | R | R | R | S | S | R | R | Must include must-fail authority and leak-path evidence |
| Runtime/trace/lab/util completion (`bd-yx9r.6*`) | R | R | R | S | S | R | R | Must emit replay hints and first-failure localization artifacts |
| Combinator/service/transport/remote/spork (`bd-yx9r.7*`) | R | R | R | S | S | R | R | Must show winner/loser behavior and cancellation propagation in user flows |
| Networking substrate (`bd-yx9r.8*`, `bd-yx9r.9*`) | R | R | S | S | R | R | R | Must include degraded-path and unsupported-platform behavior, not only happy-path I/O |
| HTTP/web/gRPC/application surfaces (`bd-yx9r.10*`) | R | S | S | S | R | R | R | Browser/native and protocol/version matrix must be explicit |
| Data/messaging/distributed surfaces (`bd-yx9r.11*`) | R | R | R | R | R | R | R | Differential or semantic-fixture lanes are required unless a written exclusion is approved |
| Developer/operator/acceptance surfaces (`bd-yx9r.12*`) | R | S | S | S | R | R | R | Docs/examples/smokes must point back to retained artifacts and rerun commands |
| Final crate-level gate (`bd-yx9r.12.3`) | N | N | S | R | R | R | R | Closure requires evidence references from all reopened families, not just summary prose |

Legend:

- `R` = required by default for that family
- `S` = required for a meaningful subset; bead must say why if omitted
- `N` = not normally required at the final acceptance-gate row itself

### 7.2 Reopened-Surface E2E Contract

For reopened crate-level families, `e2e` means more than "binary runs":

1. one named happy-path scenario proving the user-facing workflow works,
2. one degraded-path scenario proving bounded/diagnosable fallback behavior,
3. one failure or rejection scenario proving fail-closed behavior where
   browser/native/feature/profile rules matter,
4. a structured summary JSON plus JSONL records under the shared harness
   contract,
5. a copy-paste rerun command and any replay hint or retained seed needed to
   reproduce the first failure.

### 7.3 Allowed Exclusions

Some reopened families will not have meaningful Rust-vs-C differential fixtures
on day one. That is allowed only when the bead records one of these explicit
reasons:

- no upstream semantic fixture exists yet for the family,
- the surface is purely operator/documentation-facing and is instead covered by
  e2e + artifact evidence,
- the feature is intentionally unsupported in the current profile/platform and
  the required proof is a fail-closed rejection test rather than a parity run.

"Too expensive right now" is not an acceptable exclusion reason.

## 8. Closure Rules for Future Implementation Beads

An implementation bead under `bd-1eqo.*` should not close unless it includes:

1. linked unit tests for all local API and error-path behavior,
2. linked law/invariant or transition tests when the subsystem semantics demand
   them,
3. at least one named scenario, lab run, or e2e smoke that reflects the real
   user workflow being added,
4. structured log and artifact locations,
5. explicit rationale for any lane marked not applicable.

This rule is intentionally stricter than "tests passed." The roadmap is trying
to preserve product behavior, not just code coverage.

## 9. Review Checklist

For every new `bd-1eqo.*` implementation bead or closure:

1. identify which row in this matrix governs the bead,
2. list the required lanes before coding starts,
3. confirm whether a shared harness or specialized rig is required,
4. define expected logs/artifacts before implementation begins,
5. refuse closure if the lane mix does not match the promise being made.

## 10. Relationship to Existing Contracts

- `docs/TEST_COMPLETENESS_MATRIX.md` remains the low-level module completeness
  authority.
- `docs/TEST_LOG_SCHEMA.md` remains the canonical structured log contract.
- `docs/QUALITY_GATES.md` remains the CI gate registry.
- this document is the roadmap-level bridge that tells future contributors which
  combination of those lower-level assets each substantive parity track must
  actually use.
