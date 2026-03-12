# Semantic Gap Risk Ranking

> **Bead:** `bd-1eqo.1.3`  
> **Status:** Roadmap sequencing rationale by user impact, implementation risk,
> unblock power, and evidence burden  
> **Last updated:** 2026-03-12 by CrimsonHarbor

This document converts the parity inventories into an explicit execution-order
argument. It exists so future contributors do not have to reconstruct from
memory why some gaps should be handled early, why some can wait, and why others
must carry unusually strong evidence obligations.

It is not a replacement for [`docs/FEATURE_PARITY.md`](./FEATURE_PARITY.md).
That file describes the current kernel-scope parity state. This document ranks
the remaining substantive `/dp/asupersync` gaps beyond that already-completed
kernel baseline.

## 1. Normative Inputs

- [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](./RUST_EXPORTED_SURFACE_INVENTORY.md)
- [`docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md`](./USER_WORKFLOW_ACCEPTANCE_INVENTORY.md)
- [`docs/SUBSYSTEM_EVIDENCE_MATRIX.md`](./SUBSYSTEM_EVIDENCE_MATRIX.md)
- [`docs/FEATURE_PARITY.md`](./FEATURE_PARITY.md)
- [`docs/DEFERRED_SURFACES.md`](./DEFERRED_SURFACES.md)
- `/dp/asupersync/README.md`
- `/dp/asupersync/src/lib.rs`

## 2. Ranking Dimensions

Each roadmap track is ranked along four dimensions:

1. **User impact if absent**  
   How badly real users notice the gap in onboarding, migration, production
   usage, browser adoption, diagnosis, or correctness trust.
2. **Implementation risk / redesign risk**  
   How likely early mistakes are to force wide churn later.
3. **Unblock power**  
   How much downstream roadmap work depends on this being defined first.
4. **Evidence burden**  
   How much proof infrastructure, conformance, deterministic logging, or e2e
   coverage is needed before the result can be trusted.

Scoring vocabulary used below:

- `Critical`
- `High`
- `Medium`
- `Lower`

“Lower” does not mean “unimportant.” It means the track is less likely to
corrupt the rest of the roadmap if deferred.

## 3. High-Level Ordering Logic

The roadmap should not be driven by module adjacency alone. The correct default
ordering is:

1. **Planning anchors that prevent false confidence**  
   source surface, user workflows, gap ranking, profile matrix, provenance, e2e
   harness/report contract.
2. **Foundational public value semantics and authority model**  
   types, errors, cancellation, ownership/task context, Cx/capability flow.
3. **Core runtime behavior that makes the promises believable**  
   runtime object, scheduler/reactor, deadlines, channels, combinators.
4. **Deterministic proof and diagnostic loops**  
   lab, replay, observability, evidence sinks, doctor/report flows.
5. **Integration surfaces users adopt directly**  
   bytes/codec/IO/net/app, service composition, native host/server.
6. **Specialized or broader ecosystem lanes**  
   Browser Edition, actor/gen_server/supervision, messaging/remote/distributed,
   advanced protocol/application surfaces.

The guiding rule is simple:

- first build the semantics users program *against*,
- then the behavior users must *trust*,
- then the diagnostics users need when trust is challenged,
- then the broader surfaces users adopt around that core.

## 4. Ranked Track Table

| Rank band | Track | User impact | Redesign risk | Unblock power | Evidence burden | Why it belongs here |
|---|---|---|---|---|---|---|
| `P0 planning anchor` | `bd-1eqo.2.3` provenance crosswalk | High | High | High | High | Without it, fixtures and examples can silently become the spec instead of evidence about the spec |
| `P0 planning anchor` | `bd-1eqo.2.2` e2e scenario packs + logging contract | High | High | Medium | Critical | Prevents late-stage scramble around examples, smoke paths, and failure artifacts |
| `P0 planning anchor` | `bd-1eqo.2.6` shared harness/artifact/golden-log strategy | High | High | High | Critical | Prevents every subsystem from inventing incompatible evidence layouts |
| `P0 semantics` | `bd-1eqo.15` foundational types/value semantics | Critical | Critical | Critical | High | All higher layers depend on IDs, outcomes, budgets, ownership, error taxonomy, and WASM boundary values being right |
| `P0 semantics` | `bd-1eqo.3` capability context / structured concurrency API | Critical | Critical | High | High | `Cx`, scopes, authority flow, and no-orphan ownership shape the whole mental model and many downstream APIs |
| `P0 behavior` | `bd-1eqo.4` runtime object / scheduler / reactor | Critical | Critical | High | Critical | Users judge correctness through runtime behavior, fairness, wakeups, and shutdown, not only type signatures |
| `P0 behavior` | `bd-1eqo.5` deadlines / cancellation driver | Critical | High | Medium | Critical | Bounded cleanup and timeout semantics are headline promises; drift here destroys trust quickly |
| `P0 behavior` | `bd-1eqo.6` channels / sync primitives | Critical | High | High | High | Two-phase communication and cancel-safe coordination are central migration and correctness workflows |
| `P1 proof loop` | `bd-1eqo.8` lab / replay / deterministic evidence | High | High | High | Critical | Determinism is not a side feature; it is the mechanism that makes many guarantees auditable |
| `P1 proof loop` | `bd-1eqo.11` observability / diagnostics / evidence sinks | High | High | Medium | Critical | If failures are opaque, parity is user-hostile even when semantics are nominally correct |
| `P1 proof loop` | `bd-1eqo.12` security / audit / authority hardening | High | High | Medium | High | Must-fail boundary behavior and auditability need to be designed in before broader integrations multiply the attack surface |
| `P1 user workflow` | `bd-1eqo.9.3` canonical examples / walkthroughs / smoke packs | High | Medium | Medium | High | Users adopt the product through examples, not by reading internal matrices |
| `P1 integration` | `bd-1eqo.9` bytes / codec / IO / net / app | Critical | High | Medium | High | Real service adoption depends on these, but they rest on prior semantic/runtime/diagnostic foundations |
| `P1 integration` | `bd-1eqo.13` streams / plan IR / service composition | High | High | Medium | High | Composition bugs can silently undermine higher-level usability, so this should follow core semantics but not trail too far behind |
| `P1 platform` | `bd-1eqo.14` native host / server / shutdown | High | Medium | Medium | High | Native operability is user-facing and concrete, but it is safer after runtime, diagnostics, and base IO stories are defined |
| `P1 browser` | `bd-1eqo.16` Browser Edition / WASM / web | High | High | Medium | High | Browser Edition is a real product lane; however, it depends on type/value semantics, compatibility policy, examples, and evidence contracts first |
| `P2 higher layer` | `bd-1eqo.7` combinators / orchestration | High | Medium | Medium | High | Important for ergonomics and correct composition, but safer after core task/cancel/channel/runtime rules are fixed |
| `P2 higher layer` | `bd-1eqo.10` actor / gen_server / supervision | Medium-High | High | Medium | High | Publicly important, but clearly layered atop the structured runtime rather than defining it |

## 5. Detailed Rationale by Track

### 5.1 `bd-1eqo.15` Foundational Types, Value Algebra, and Interoperability

Why it is early:

- root re-exports show users import these directly;
- workflow inventory shows every persona touches outcomes, IDs, budgets, errors,
  or compatibility decisions;
- bad early choices here leak upward into runtime APIs, browser contracts,
  examples, diagnostics, and profile policy.

Why the risk is high:

- ownership/task-context models are architecture-shaping,
- error taxonomy and cancellation semantics affect every API,
- WASM/browser type contracts become expensive to change once examples and docs
  exist.

Evidence consequence:

- heavy unit/law burden,
- selective conformance/profile burden,
- at least one realistic scenario proving values behave correctly in context.

### 5.2 `bd-1eqo.3` Capability Context and Structured Concurrency

Why it is early:

- `Cx` is the conceptual center of the upstream model;
- if authority flow, scope semantics, or task ownership are wrong, later
  runtime/channel/time/integration work becomes structurally wrong rather than
  merely incomplete.

Why the risk is high:

- ambient-authority mistakes propagate everywhere,
- region/scope signatures are expensive to redesign after adoption begins.

Evidence consequence:

- must-fail invalid-authority tests,
- ownership/transition laws,
- example flows that make the non-tokio model understandable.

### 5.3 `bd-1eqo.4` Runtime Object, Scheduler, and Reactor

Why it is early:

- users feel the runtime through fairness, wake behavior, shutdown, and runtime
  config more than through many type-level abstractions;
- many platform and integration tracks depend on the runtime object shape.

Why the risk is critical:

- scheduler/reactor mistakes are hard to patch around later,
- runtime object API drift can break examples, host integration, and diagnostic
  tooling simultaneously.

Evidence consequence:

- heavy law/lab/artifact burden,
- deterministic run-order explanation,
- parity-safe behavior across native/profile variants.

### 5.4 `bd-1eqo.5` Time, Deadlines, and Cancellation Driver

Why it is early:

- bounded cleanup and explicit cancellation are headline README promises,
- timeouts and drain semantics show up in core user workflows and operator
  diagnosis loops.

Why the risk is high:

- timeout semantics infect channels, combinators, IO, and browser suspensions,
- users quickly lose trust if timeout behavior is confusing or poorly explained.

Evidence consequence:

- deterministic lab scenarios,
- explicit deadline-warning artifacts,
- success and slow-tail failure slices.

### 5.5 `bd-1eqo.6` Channel Family and Sync Primitives

Why it is early:

- two-phase send and cancel-safe coordination are among the clearest upstream
  differentiators,
- many migration comparisons are expressed through channels and sync primitives.

Why the risk is high:

- fairness/backpressure semantics are subtle and cross-cutting,
- later service, stream, and actor layers inherit these assumptions.

Evidence consequence:

- strong unit/law coverage,
- race-oriented scenarios,
- clear artifacts for loser/winner and closed-peer cases.

### 5.6 `bd-1eqo.8` Lab, Replay, and Deterministic Evidence

Why it is early but slightly behind core semantics:

- determinism is central to product identity,
- but it is most valuable once there is enough runtime/channel/time behavior to
  exercise.

Why the risk is high:

- weak replay or artifact design causes every later test lane to fragment,
- the project’s credibility is tightly coupled to deterministic debugging.

Evidence consequence:

- this track itself has the heaviest artifact burden in the roadmap.

### 5.7 `bd-1eqo.11` Observability, Diagnostics, and Evidence Sinks

Why it is early:

- operator and developer trust depends on actionable reports,
- doctor/browser/native lanes all need structured outputs rather than ad hoc
  logs.

Why the risk is high:

- adding diagnostics too late tends to bolt them onto already-opaque internals,
- evidence shape decisions become sticky once example packs and tooling appear.

Evidence consequence:

- failing-path e2e coverage is mandatory,
- report payloads need contract tests,
- artifact layout must align with shared harness plans.

### 5.8 `bd-1eqo.9` Bytes, Codec, I/O, Networking, and App Integration

Why it is high impact:

- many users will judge “is this usable?” by whether they can build a real
  service, not whether a scheduler invariant exists.

Why it is not first:

- these layers depend on value semantics, cancellation behavior, diagnostics,
  and host/runtime foundations being coherent first.

Evidence consequence:

- broad unit/e2e burden,
- canonical examples and smoke packs are not optional add-ons.

### 5.9 `bd-1eqo.16` Browser Edition, WASM Profile, and Web-Facing Contracts

Why it deserves explicit priority:

- upstream dedicates major docs and compatibility policy to Browser Edition,
- JS/TS/React/Next workflows are user-facing, not internal experiments.

Why it is not earliest:

- it depends on foundational type semantics, compatibility policy, examples,
  evidence contracts, and logging/report structure.

Evidence consequence:

- must include success/failure browser scenarios,
- fail-closed compatibility matrix tests,
- browser-specific replay/troubleshooting artifacts.

### 5.10 `bd-1eqo.10` Actor, GenServer, and Supervision

Why it matters:

- it is publicly exported and central for a class of users,
- deterministic OTP-style debugging is explicitly marketed upstream.

Why it is later:

- it clearly layers on top of core structured runtime semantics,
- supervision/restart semantics are safer after channels, cancellation, lab,
  and observability stories are stronger.

Evidence consequence:

- law-style restart/escalation tests,
- seeded recovery scenarios,
- ancestry/restart evidence, not just pass/fail outcomes.

## 6. Planning Mistakes Most Likely to Cascade

These are the roadmap errors most likely to force expensive rework:

1. treating fixtures or examples as the spec instead of building a provenance
   crosswalk first,
2. implementing higher-level integrations before foundational values and
   authority/cancellation semantics are locked,
3. postponing artifact/report contracts until after examples and e2e scripts
   proliferate,
4. treating Browser Edition as “later wasm stuff” rather than a first-class
   user lane with its own compatibility and diagnostic rules,
5. treating observability as optional because unit tests pass,
6. conflating “kernel parity is complete” with “substantive product parity is
   complete.”

## 7. Tracks Requiring Especially Strong Evidence

The following tracks deserve stronger-than-normal proof obligations:

- `bd-1eqo.15`
  because type/value drift contaminates every higher layer.
- `bd-1eqo.4`
  because runtime behavior determines whether guarantees are believable.
- `bd-1eqo.5`
  because timeout/cancel behavior is easy to misunderstand and highly visible.
- `bd-1eqo.8`
  because deterministic replay is itself a proof mechanism.
- `bd-1eqo.11`
  because diagnostics must succeed in failure cases, not only happy paths.
- `bd-1eqo.16`
  because browser compatibility failures are user-facing and often subtle.

These are the lanes where weak logging, weak e2e coverage, or ambiguous
provenance are most likely to produce false confidence.

## 8. Recommended Near-Term Sequencing

With the current planning inventory in place, the defensible near-term order is:

1. `bd-1eqo.2.3` provenance crosswalk
2. `bd-1eqo.2.2` e2e scenario packs + logging/report contract
3. `bd-1eqo.2.6` shared harness/artifact/golden-log strategy
4. `bd-1eqo.1.4` profile/resource-class capability matrix
5. `bd-1eqo.15.*` foundational type/value implementation planning and work
6. `bd-1eqo.3.*`, `bd-1eqo.4.*`, `bd-1eqo.5.*`, `bd-1eqo.6.*`
7. `bd-1eqo.8.*` and `bd-1eqo.11.*`
8. `bd-1eqo.9.*`, `bd-1eqo.13.*`, `bd-1eqo.14.*`, `bd-1eqo.16.*`
9. `bd-1eqo.10.*` and broader advanced ecosystem lanes

This order is not claiming the later tracks are less valuable. It is claiming
that this order minimizes the odds of building impressive but fragile parity.

## 9. Review Checklist

When a contributor wants to reorder `bd-1eqo` work, they should justify:

1. which ranking dimension changed,
2. which user workflow becomes more urgent,
3. which redesign risk has been reduced,
4. which evidence burden is now better understood,
5. why the new order will not cause downstream proof or compatibility churn.

If they cannot answer those questions, the reorder is probably intuition-driven
instead of defensible.
