# Wave Gating Protocol (A/B/C/D + Selective Surface Expansion)

> Bead: `bd-296.10`  
> Depends on: `bd-296.7`, `bd-296.13`  
> Downstream consumers: `bd-296.11`, `bd-296.8`  
> Effective baseline: `docs/OWNER_DECISION_LOG.md` + `docs/DEFERRED_SURFACE_REGISTER.md`

## 1. Purpose

This protocol defines auditable entry/exit gates for Wave A/B/C/D so no subsystem enters implementation without semantic and verification readiness.

The protocol is mandatory for:

- phase transitions,
- deferred-surface activation,
- selective higher-surface onboarding (Phase 9 style expansion).

## 2. Non-Negotiable Global Rules

These apply to every wave:

1. Semantic delta budget default is `0`; non-zero requires explicit approved exception record.
2. Deferred surfaces must be explicit (`DS-*` in `docs/DEFERRED_SURFACE_REGISTER.md`).
3. Decision assumptions must map to `DEC-*` keys in `docs/OWNER_DECISION_LOG.md`.
4. All parity/conformance artifacts must reference pinned baseline provenance fields.
5. Gate status is binary: `pass` or `fail`. No "soft pass" for mandatory gates.

## 3. Gate Model

Each wave has two gate types:

- **Entry Gate**: minimum prerequisites before implementation starts.
- **Exit Gate**: required evidence before wave close and downstream unblocking.

If any mandatory check fails, wave status remains `in_progress` and dependent beads stay blocked.

## 4. Wave Entry/Exit Criteria

| Wave | Entry Gate (all required) | Exit Gate (all required) |
|---|---|---|
| Wave A (kernel) | Spec extraction complete for in-scope semantics; transition/lifecycle contracts documented; deferred surfaces and decisions published (`DS-*`, `DEC-*`) | Core/kernel gates pass (portability, OOM/resource, determinism, conformance for in-scope fixtures); unresolved obligations/leak invariants covered |
| Wave B (determinism/parity harness) | Wave A exit pass; fixture schema + scenario DSL baseline accepted; trace/replay contract frozen for current milestone | Replay identity stable; Rust-vs-C parity runner producing machine-readable reports; codec equivalence pass for shared fixtures |
| Wave C (selected systems surfaces) | Wave B exit pass; subsystem-specific spec extracted; adapter boundaries defined; DS item promoted with explicit unblock evidence | Surface-specific conformance pack pass; cross-profile semantic parity preserved for shared fixtures; no regression in Wave A/B gate set |
| Wave D (advanced deferred surfaces) | Wave C exit pass; heavy-surface demand + cost justification documented; dedicated risk analysis approved | Advanced-surface acceptance pack pass (functional + stress + parity where applicable) without violating kernel invariants or decision constraints |

## 5. Mandatory Evidence Package (Wave Close)

Every wave close must produce one evidence bundle containing:

1. Unit coverage report for in-scope modules.
2. Invariant coverage report for lifecycle/cancel/obligation legality.
3. E2E/scenario pass manifest for required lanes.
4. Structured log/manifest completeness report with required fields.
5. Conformance/parity summary (including `delta_classification` records for non-`none` outcomes).
6. Deferred-surface delta report:
   - newly deferred surfaces,
   - promoted surfaces,
   - status changes with rationale.
7. Decision linkage report mapping touched subsystems to `DEC-*` keys.

Bundle completeness is a hard gate: missing artifacts fail wave closure.

## 6. Selective Surface Expansion Criteria (Phase 9+)

A deferred subsystem can be promoted only when all criteria below are met:

1. **Spec readiness**: subsystem semantics extracted and linked to fixture/parity plan.
2. **Dependency readiness**: all prerequisite waves/surfaces are gate-passed.
3. **Conformance readiness**: scenario and parity fixtures defined before implementation.
4. **Risk controls**: subsystem-specific failure modes and rollback triggers documented.
5. **Evidence path**: required logs/manifests/artifacts enumerated up front.
6. **Governance linkage**: promotion references matching `DS-*` and `DEC-*` rows.

Promotion request template (required fields):

- subsystem id (`DS-*` or new key),
- target wave,
- dependencies satisfied,
- required gates/tests,
- artifact plan,
- rollback trigger,
- owner approver.

## 7. Enforcement Workflow

1. **Gate proposal**: author submits wave status packet with artifact links.
2. **Independent review**: at least one reviewer who did not author the packet validates claims.
3. **Decision check**: verify `DEC-*` assumptions and pending queue impact.
4. **Deferred-surface check**: verify no implicit scope expansion occurred.
5. **Gate decision**:
   - `pass`: unblock dependent beads.
   - `fail`: record blocking reasons and remediation tasks.

## 8. Failure Conditions (Automatic Block)

Wave closure is automatically blocked when any of the following is true:

- required artifact missing,
- parity/conformance drift is unclassified,
- deferred surface started without `DS-*` registration,
- decision-dependent work missing `DEC-*` linkage,
- unresolved pending decision treated as effective without evidence.

## 9. Traceability Wiring

This protocol must remain aligned with:

- `docs/OWNER_DECISION_LOG.md` for decision authority (`DEC-*`, `PEND-*`),
- `docs/DEFERRED_SURFACE_REGISTER.md` for deferred scope (`DS-*`),
- `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` for semantic provenance (`prov_id`),
- `bd-296.8` traceability index for claims-to-artifacts mapping.

## 10. Operational Checklist

Use this checklist before closing any wave:

1. Confirm all entry assumptions are still true.
2. Regenerate required reports (unit/invariant/e2e/parity/log manifests).
3. Validate decision + deferred-surface mappings.
4. Verify pending decisions are not treated as effective scope.
5. Record pass/fail verdict with reviewer identity and timestamp.

If any checklist item fails, wave cannot close.
