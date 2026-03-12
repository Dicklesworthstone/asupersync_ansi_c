# Rust Provenance Crosswalk and Fixture Workflow

> **Bead:** `bd-1eqo.2.3`  
> **Status:** Roadmap-level provenance crosswalk for substantive `/dp/asupersync`
> parity and the capture/update workflow for derived fixtures, examples, and
> evidence artifacts  
> **Last updated:** 2026-03-12 by CrimsonHarbor

This document extends, rather than replaces,
[`docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`](./SOURCE_TO_FIXTURE_PROVENANCE_MAP.md).

That earlier document is valuable for kernel-era semantic rows and concrete
fixture IDs. This document serves a different planning need:

> For each major `bd-1eqo` track, what is authoritative upstream, what is
> derived in the C port, what evidence artifacts should flow from it, and how
> do we update those derived artifacts without silently moving the parity
> target?

Without this crosswalk, the project risks letting fixtures, example packs,
browser smoke outputs, or doctor reports become de facto specifications.

## 1. Normative Inputs

Upstream authority and baseline identity:

- `/dp/asupersync/src/lib.rs`
- `/dp/asupersync/README.md`
- `docs/rust_baseline_inventory.json`

Existing local provenance and traceability artifacts:

- [`docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`](./SOURCE_TO_FIXTURE_PROVENANCE_MAP.md)
- [`docs/PLAN_EXECUTION_TRACEABILITY_INDEX.md`](./PLAN_EXECUTION_TRACEABILITY_INDEX.md)
- [`docs/FEATURE_PARITY.md`](./FEATURE_PARITY.md)
- [`docs/RUST_EXPORTED_SURFACE_INVENTORY.md`](./RUST_EXPORTED_SURFACE_INVENTORY.md)
- [`docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md`](./USER_WORKFLOW_ACCEPTANCE_INVENTORY.md)
- [`docs/SUBSYSTEM_EVIDENCE_MATRIX.md`](./SUBSYSTEM_EVIDENCE_MATRIX.md)
- [`docs/QUALITY_GATES.md`](./QUALITY_GATES.md)
- [`docs/TEST_LOG_SCHEMA.md`](./TEST_LOG_SCHEMA.md)

## 2. Provenance Roles

Every artifact in the roadmap should fit one of these roles:

### 2.1 Authority

Artifacts that define the target behavior we are trying to match.

Examples:

- upstream Rust source modules,
- upstream README guarantees,
- upstream browser/doctor/docs contracts where they materially define behavior,
- pinned baseline metadata in `docs/rust_baseline_inventory.json`.

### 2.2 Extraction / Interpretation

Artifacts that restate or structure the authority to make implementation and QA
possible, but do not replace the authority.

Examples:

- `docs/FEATURE_PARITY.md`
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`
- `docs/RUST_EXPORTED_SURFACE_INVENTORY.md`
- `docs/USER_WORKFLOW_ACCEPTANCE_INVENTORY.md`

### 2.3 Derived Verification Asset

Artifacts or fixtures produced to test the C implementation against the
authority.

Examples:

- conformance fixtures,
- profile-parity outputs,
- e2e smoke packs,
- browser example manifests,
- doctor report bundles,
- replay manifests and structured logs.

### 2.4 Derived Result

The actual run outputs, summaries, and retained evidence generated from a test
or scenario execution.

Examples:

- `build/conformance/*`,
- JSON summaries,
- replay/crashpack bundles,
- structured logs emitted by e2e or lab runs.

The cardinal rule is:

- **Authority may drive derived assets/results.**
- **Derived assets/results may never silently redefine authority.**

## 3. Baseline Identity Rules

The current pinned upstream baseline is defined by
`docs/rust_baseline_inventory.json`, which records:

- upstream repository identity,
- commit hash and subject,
- rust toolchain identity,
- cargo lock snapshot hash,
- provenance policy and allowed delta classifications.

Roadmap rule:

- every future conformance fixture family, browser scenario pack, or doctor
  evidence contract must be traceable to a pinned upstream baseline or an
  explicitly approved update.

If a contributor cannot name the upstream baseline a fixture/example/report pack
is meant to reflect, that artifact is not trustworthy enough to guide parity.

## 4. Crosswalk Matrix

This table maps each `bd-1eqo` track to:

- upstream authority,
- planning/extraction artifacts,
- expected derived verification assets,
- expected retained result artifacts,
- update-risk notes.

| Track | Upstream authority | Local interpretation layer | Derived verification assets | Derived result artifacts | Update-risk notes |
|---|---|---|---|---|---|
| `bd-1eqo.15` foundational types/value semantics | `src/lib.rs` root re-exports, `src/types/*`, README core types sections, browser/WASM value types | exported-surface inventory, workflow inventory, existing parity matrix rows where relevant | unit/law fixtures for values, compatibility matrices, typed symbol and wasm-envelope examples | unit logs, conformance reports, compatibility summaries | high drift risk because small type-taxonomy changes cascade into APIs, browser docs, and error/report payloads |
| `bd-1eqo.3` capability context / structured concurrency | `src/cx/*`, `src/cancel/*`, `src/obligation/*`, README quick example and tokio migration sections | workflow inventory, semantic gap ranking | capability misuse fixtures, scope/join examples, authority-boundary negative tests | scenario logs, replay manifests, structured failure summaries | high redesign risk because API signatures and authority model propagate everywhere |
| `bd-1eqo.4` runtime object / scheduler / reactor | `src/runtime/*`, README architecture/runtime sections | exported-surface inventory, feature parity, traceability index | runtime object tests, scheduler fairness packs, reactor/native compatibility checks | lab traces, conformance/profile outputs, runtime diagnostics | behavior can appear to pass local tests while drifting from upstream runtime rationale |
| `bd-1eqo.5` time / deadlines / cancellation driver | `src/time/*`, `src/cancel/*`, README timeout/deadline/progress sections | workflow inventory, evidence matrix | timeout/interval fixtures, cleanup-budget scenarios, drain progress checks | deadline warning logs, drain certificates, replay artifacts | easy to accidentally test only happy paths while missing tail/stall semantics |
| `bd-1eqo.6` channels / sync | `src/channel/*`, `src/sync/*`, README channel/sync sections | feature parity rows, workflow inventory | reserve/commit fairness fixtures, sync cancellation packs, law tests | e2e coordination logs, race summaries, profile reports | derived examples must not flatten two-phase semantics into ordinary send/recv tutorials |
| `bd-1eqo.7` combinators / orchestration | `src/combinator/*`, macro surface, README combinator sections | exported-surface inventory, workflow inventory | join/race/select/quorum packs, macro/wrapper examples | orchestration traces, logged scenario summaries | easy to produce convenience APIs that violate upstream cancel/drain guarantees |
| `bd-1eqo.8` lab / replay / deterministic evidence | `src/lab/*`, `src/trace/*`, README determinism/evidence sections | evidence matrix, semantic gap ranking | seeded scenario packs, replay manifests, crashpack/counterexample workflows | replay bundles, crashpacks, deterministic suite summaries | strongest risk of fixtures/results becoming self-justifying if provenance is loose |
| `bd-1eqo.9` bytes / codec / IO / networking / app | `src/bytes/*`, `src/codec/*`, `src/io/*`, `src/net/*`, `src/http/*`, `src/app/*`, README coverage map and networking sections | exported-surface inventory, workflow inventory | protocol/example packs, smoke scripts, app bootstrap scenarios | smoke logs, artifact manifests, protocol summaries | breadth is large; contributors may over-focus on module presence instead of end-user flows |
| `bd-1eqo.9.3` canonical examples / walkthroughs / smoke packs | README quick example, tokio mapping, Browser Edition docs list, integration docs list | workflow inventory, evidence matrix | curated native/browser/example manifests and deterministic smoke runners | summary JSON, replay pointers, artifact directories, example validation logs | example docs are derived, not authoritative; they must point back to tracked upstream behaviors |
| `bd-1eqo.10` actor / gen_server / supervision | `src/actor/*`, `src/gen_server.rs`, `src/supervision/*`, README OTP/debug references | exported-surface inventory, workflow inventory | supervision law packs, recovery scenarios, mailbox ordering examples | restart lineage logs, seeded failure bundles | layered surface; easy to drift from upstream restart semantics if built atop incomplete runtime/cancel assumptions |
| `bd-1eqo.11` observability / diagnostics / evidence sinks | `src/observability/*`, `src/evidence*`, README diagnostics/evidence/docs table | evidence matrix, workflow inventory | doctor/report schemas, evidence sink fixtures, failing e2e packs | reports, structured logs, evidence bundles, remediation outputs | if report schemas drift from upstream docs, operators will trust derived artifacts that no longer match the intended contract |
| `bd-1eqo.12` security / audit / authority hardening | `src/security/*`, `src/audit/*`, capability/boundary rules in README and browser/native restrictions | exported-surface inventory, semantic gap ranking | must-fail boundary fixtures, audit evidence packs, compatibility rejection tests | audit logs, boundary-failure reports, provenance-tagged evidence | fail-closed guarantees are especially vulnerable to being weakened by “helpful” fixtures/examples |
| `bd-1eqo.13` streams / plan / service composition | `src/stream/*`, `src/plan/*`, `src/service/*`, README composition coverage map | exported-surface inventory, workflow inventory | middleware/stream composition packs, backpressure scenarios, plan/service examples | e2e flow logs, composition summaries, profile reports | composition examples must remain faithful to upstream cancellation/diagnostic propagation |
| `bd-1eqo.14` native host / server / shutdown | `src/fs/*`, `src/process*`, `src/signal/*`, `src/server/*`, `src/cli/*`, README native host sections | workflow inventory, profile matrix, evidence matrix | server bootstrap/shutdown scripts, native smoke packs, operator install/run flows | structured smoke logs, shutdown reports, packaging artifacts | native-only docs and smoke packs must stay tied to explicit platform/profile constraints |
| `bd-1eqo.16` Browser Edition / WASM / web | browser-related `src/types/*`, crate-root wasm feature rules, README Browser Edition + docs table | exported-surface inventory, workflow inventory, evidence matrix | JS/TS/React/Next example packs, browser compatibility matrices, DX taxonomy fixtures | browser smoke artifacts, repro manifests, troubleshooting outputs | browser docs are rich and user-facing; stale or unsourced browser examples can quickly redefine perceived product behavior |

## 5. Artifact Lineage Rules

For every future verification asset or result, contributors should be able to
state lineage in this shape:

1. **Authority source**  
   Upstream file(s), doc(s), or baseline identity being matched.
2. **Interpretation layer**  
   Which local planning/spec artifacts explain how that authority maps into the
   C roadmap.
3. **Derived asset**  
   Fixture, scenario pack, example manifest, schema, or smoke runner produced
   from the authority.
4. **Derived result**  
   What a concrete run emits: logs, reports, replay bundle, parity diff, etc.

Example shape:

- Authority: `/dp/asupersync/src/types/wasm_abi.rs` + README Browser Edition
  docs list
- Interpretation: exported-surface inventory + workflow inventory +
  evidence matrix
- Derived asset: Browser example manifest and compatibility smoke pack
- Derived result: browser repro manifest, structured smoke summary, retained
  failure artifact bundle

If a contributor cannot describe lineage in that form, the artifact should not
be treated as parity evidence yet.

## 6. Update Workflow

When upstream Rust changes or when a new track is added, use this workflow:

### Step 1: Detect and Pin the Change

- update or verify `docs/rust_baseline_inventory.json`,
- note the changed upstream files/docs,
- determine which `bd-1eqo` tracks are touched.

### Step 2: Re-evaluate Authority, Not Just Fixtures

- update the exported-surface or workflow inventory if the user-visible surface
  changed,
- update this crosswalk when authority ownership or derived assets change,
- do not jump straight to regenerating fixtures/examples.

### Step 3: Mark Derived Assets Dirty

Potentially dirty derived assets include:

- conformance fixtures,
- example manifests,
- browser smoke packs,
- doctor/report schemas,
- profile matrices,
- replay/log schema expectations.

They should be treated as **derived and suspect** until refreshed against the
updated authority.

### Step 4: Refresh and Classify

When refreshing derived assets/results:

- regenerate or update the relevant fixture/example/report pack,
- run the associated tests or smoke flows,
- classify differences using the baseline policy already recorded in
  `docs/rust_baseline_inventory.json`:
  - `none`
  - `intentional_upstream`
  - `c_regression`
  - `spec_defect`
  - `harness_defect`

### Step 5: Preserve Auditability

Update notes should always say:

- what upstream authority changed,
- which derived assets were refreshed,
- whether any user-facing docs/examples changed,
- whether parity moved because upstream changed or because the C side drifted.

## 7. Fixture and Example Refresh Policy

The refresh rules differ by artifact type.

### 7.1 Conformance Fixtures

Allowed to change when:

- pinned upstream semantics changed, or
- harness/schema defects were fixed.

Not allowed to change just because:

- the current C implementation fails them,
- the fixture is inconvenient,
- a contributor wants to reduce evidence burden.

### 7.2 Canonical Examples and Smoke Packs

Allowed to change when:

- the upstream workflow or contract changed,
- diagnostics/logging contracts changed,
- a bug was fixed and the example was previously inconsistent with authority.

Not allowed to change just because:

- the current implementation has a gap that the example would expose.

### 7.3 Browser and Doctor Contracts

Allowed to change when:

- upstream Browser Edition or doctor docs materially change,
- compatibility/error/report contracts are intentionally revised and documented.

Not allowed to change just because:

- current error handling is weaker than the contract.

## 8. Required Provenance Fields for Future Assets

Future fixture/example/evidence manifests should carry, when applicable:

- `rust_baseline_commit`
- `rust_baseline_toolchain`
- `authority_paths`
- `roadmap_track`
- `workflow_family`
- `evidence_lanes`
- `artifact_schema_version`
- `delta_classification`

Not every asset must be JSON, but every asset should preserve equivalent
metadata in some durable form.

## 9. Relationship to Existing Kernel-Era Provenance Map

[`docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md`](./SOURCE_TO_FIXTURE_PROVENANCE_MAP.md)
remains the canonical detailed map for current kernel-scope `prov_id` rows and
their concrete fixture IDs.

This document adds:

- broader `bd-1eqo` track coverage,
- browser/native/examples/doctor lineage,
- explicit separation between authority and derived evidence,
- refresh rules that keep examples and report artifacts from becoming the spec.

The intended division of labor is:

- old map: detailed semantic row tracking for extracted kernel rules,
- this map: roadmap-level provenance ownership and update policy for the larger
  substantive parity program.

## 10. Review Checklist

Before closing any future planning or implementation bead under `bd-1eqo.*`,
confirm:

1. the upstream authority is named explicitly,
2. the relevant `bd-1eqo` track is mapped in this crosswalk,
3. any new fixture/example/report asset is marked as derived rather than
   authoritative,
4. the refresh policy for that asset is clear,
5. result artifacts can be traced back to both the upstream baseline and the
   local interpretation layer.

If any of those are missing, the project is drifting toward “fixtures as
specification,” which this bead exists to prevent.
