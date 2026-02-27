# Selective Higher-Surface Porting Queue

> **Bead:** `bd-296.11`
> **Status:** Canonical queue + per-subsystem spec/conformance templates
> **Last updated:** 2026-02-27 by CopperSpire

This queue defines how deferred higher surfaces are selectively admitted after kernel-phase stability, with mandatory spec and conformance templates for each subsystem.

## 1. Admission Policy

A subsystem may enter active implementation queue only when:

1. current wave gate allows that subsystem class,
2. prerequisite beads/gates are green,
3. subsystem spec template is completed,
4. conformance template is completed,
5. owner + reviewer are explicitly assigned.

## 2. Queue Columns

- `queue_id`: stable queue row id.
- `surface`: subsystem/capability.
- `target_wave`: expected wave for first activation.
- `admission_prereqs`: gate prerequisites.
- `spec_template`: required spec artifact id.
- `conformance_template`: required parity/test artifact id.
- `status`: `deferred`, `ready-for-spec`, `spec-complete`, `impl-ready`, `in-progress`, `done`.

## 3. Queue Baseline

| queue_id | surface | target_wave | admission_prereqs | spec_template | conformance_template | status |
|---|---|---|---|---|---|---|
| `Q-B-001` | full trace subsystem expansion | Wave B | Wave A review gate + replay baseline stable | `SPEC-TPL-TRACE` | `CONF-TPL-TRACE` | deferred |
| `Q-B-002` | lab/runtime experiment surfaces | Wave B | Wave A review gate + determinism baseline stable | `SPEC-TPL-LAB` | `CONF-TPL-LAB` | deferred |
| `Q-B-003` | optional parallel profile | Wave B+ | ADR-001 unblock criteria + fairness spec | `SPEC-TPL-PARALLEL` | `CONF-TPL-PARALLEL` | deferred |
| `Q-B-004` | static arena backend | Wave B+ | ADR-002 unblock criteria + allocator parity plan | `SPEC-TPL-STATIC-ARENA` | `CONF-TPL-STATIC-ARENA` | deferred |
| `Q-C-001` | networking core surfaces | Wave C | Wave B conformance/fuzz gates green | `SPEC-TPL-NET` | `CONF-TPL-NET` | deferred |
| `Q-C-002` | combinator surface expansion | Wave C | Wave A/B lifecycle/cancel semantics stable | `SPEC-TPL-COMBINATOR` | `CONF-TPL-COMBINATOR` | deferred |
| `Q-C-003` | observability/metrics beyond trace | Wave C | trace baseline stable + schema compatibility | `SPEC-TPL-OBS` | `CONF-TPL-OBS` | deferred |
| `Q-D-001` | HTTP/2 and protocol stack | Wave D | Wave C networking stability | `SPEC-TPL-HTTP2` | `CONF-TPL-HTTP2` | deferred |
| `Q-D-002` | gRPC stack | Wave D | Wave C networking + serialization readiness | `SPEC-TPL-GRPC` | `CONF-TPL-GRPC` | deferred |
| `Q-D-003` | distributed runtime surfaces | Wave D | Wave C observability + trace continuity | `SPEC-TPL-DIST` | `CONF-TPL-DIST` | deferred |
| `Q-D-004` | actor/supervision surfaces | Wave D | kernel + cancellation + obligation semantics stable in production | `SPEC-TPL-ACTOR` | `CONF-TPL-ACTOR` | deferred |

## 4. Spec Template (Required per Queue Row)

Each subsystem must fill this structure before implementation:

```text
SPEC-TPL-<SUBSYSTEM>
- Scope boundary (what is included/excluded)
- Semantic contracts (state, transitions, errors)
- Determinism and replay expectations
- Resource contract interactions (memory/queue/timer/IO)
- Failure-atomic boundaries
- Profile-specific behavior (if any) with semantic-lock statement
- Forbidden behavior additions
- Fixture family plan (IDs + categories)
- Open decisions and explicit non-goals
```

## 5. Conformance Template (Required per Queue Row)

Each subsystem must fill this structure before merge:

```text
CONF-TPL-<SUBSYSTEM>
- Rust reference fixture set or explicit non-applicability rationale
- C fixture set and execution commands
- JSON/BIN codec parity expectations (if applicable)
- Cross-profile parity expectations
- Differential fuzz strategy (or justified exclusion)
- Required CI jobs and pass criteria
- Evidence artifact paths and digest outputs
- Counterexample minimization path
```

## 6. Promotion Workflow

1. Candidate row status moves `deferred -> ready-for-spec` only when wave gate criteria pass.
2. After spec template completion + review: `ready-for-spec -> spec-complete`.
3. After conformance template completion + owner assignment: `spec-complete -> impl-ready`.
4. Only `impl-ready` rows may be claimed for implementation.

## 7. Block Conditions

A row cannot be promoted if any of these are true:

- missing wave gate evidence,
- missing spec/conformance template fields,
- unresolved critical risk-register items,
- no named owner/reviewer,
- missing fixture family plan.

## 8. Initial Ownership Guidance

Suggested initial owners by active epics:

- queue rows tied to `bd-2n0` -> replay/codec owners,
- queue rows tied to `bd-j4m` -> profile and vertical owners,
- queue rows tied to `bd-1md` -> conformance/fuzz owners,
- queue rows tied to `bd-ix8` -> build/portability owners.

## 9. Synchronization Targets

This queue must remain synchronized with:

- `docs/DEFERRED_SURFACES.md`
- `docs/DEFERRED_SURFACE_REGISTER.md`
- `docs/WAVE_GATING_PROTOCOL.md`
- `docs/FEATURE_PARITY.md`
- `docs/PLAN_EXECUTION_TRACEABILITY_INDEX.md`

## 10. Immediate Next Actions

1. Use this queue to govern post-Wave-A scope opening decisions.
2. Backfill owner/reviewer fields as team assignments stabilize.
3. Generate machine-readable queue export once CI automation is ready.
