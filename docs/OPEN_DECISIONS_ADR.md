# Owner Decision Log - Resolved ADR Records (Section 17)

> Beads: `bd-296.13`, `bd-296.20`, `bd-296.21`, `bd-296.22`, `bd-296.23`, `bd-296.25`  
> Status: ADR analysis and rationale reference  
> Effective date: 2026-02-27  
> Scope: Kernel/Wave A and immediate post-kernel planning
> Canonical effective decisions: `docs/OWNER_DECISION_LOG.md`

This document preserves ADR option/rationale analysis for Section 17 planning choices. Effective decisions used by implementation and gate planning are tracked in `docs/OWNER_DECISION_LOG.md`.

## 1. Canonical Record Format

Each record in this log uses:

- `decision_id`: stable ADR key.
- `source_bead`: the decision bead.
- `selected_option`: chosen branch.
- `effective_date`: activation date.
- `rationale`: concise justification.
- `rollback_trigger`: explicit condition that forces review.
- `supersession_rule`: how a later ADR may replace this decision.
- `impact_refs`: machine-actionable references to beads, gates, tests, logs, and evidence.

## 2. Resolved Decision Index

| decision_id | source_bead | selected_option | effective_date | rationale (short) |
|---|---|---|---|---|
| ADR-001 | `bd-296.20` | Defer optional parallel profile until post-kernel milestone | 2026-02-27 | Preserve Wave A focus on deterministic kernel parity; avoid premature atomics/synchronization complexity |
| ADR-002 | `bd-296.21` | Dynamic allocator default with static-arena-prepared interface | 2026-02-27 | Router-class targets are served now; static backend remains a planned allocator backend swap |
| ADR-003 | `bd-296.22` | Configurable wait policy with `busy_spin` default only for HFT profile | 2026-02-27 | Keep semantic plane unchanged while tuning resource plane per profile intent |
| ADR-004 | `bd-296.23` | Automotive evidence bundle plus annotated mapping skeleton (not full templates) | 2026-02-27 | Certification-friendly artifacts without overclaiming integrator-level safety case completeness |

## 3. Decision Records

### ADR-001 - Optional Parallel Profile Timing

- `source_bead`: `bd-296.20`
- `selected_option`: defer optional parallel profile to Wave B+
- `effective_date`: 2026-02-27
- `rationale`: kernel lifecycle/cancel/obligation parity and deterministic replay are first-release critical path; parallel profile substantially expands verification surface.
- `rollback_trigger`: material early-adopter demand for parallel before Wave B plus readiness of dedicated fairness/non-regression gates.
- `supersession_rule`: new ADR must include full parity-risk assessment and explicit gate deltas.

### ADR-002 - Static Arena Requirement Timing

- `source_bead`: `bd-296.21`
- `selected_option`: dynamic default in Wave A; allocator interface prepared for static backend
- `effective_date`: 2026-02-27
- `rationale`: prioritizes near-term target coverage while preserving static-arena migration path with no semantic-plane drift.
- `rollback_trigger`: automotive/freestanding deployment requires static-memory-first mode before Wave B.
- `supersession_rule`: new ADR must preserve allocator API compatibility or document controlled break.

### ADR-003 - HFT Runtime Wait Policy

- `source_bead`: `bd-296.22`
- `selected_option`: configurable wait policy with HFT default = `busy_spin`; other profiles default to non-spin policies
- `effective_date`: 2026-02-27
- `rationale`: profile-specific resource tuning is allowed; semantic outcomes must remain equivalent across profiles.
- `rollback_trigger`: measured tail/jitter regressions or hardware-level contention under busy-spin defaults.
- `supersession_rule`: replacement ADR must include p99/p99.9/p99.99 before/after evidence and parity confirmation.

### ADR-004 - Automotive Assurance Scope

- `source_bead`: `bd-296.23`
- `selected_option`: evidence bundle + annotated mapping skeleton
- `effective_date`: 2026-02-27
- `rationale`: delivers audit-friendly runtime evidence while avoiding claims that require integrator hazard/FMEA ownership.
- `rollback_trigger`: partner-backed request to expand into full mapping templates with domain experts.
- `supersession_rule`: new ADR must keep evidence bundle continuity and explicit gap annotations for any unresolved work products.

## 4. Impact Matrix (Authoritative)

### 4.1 Bead Dependencies and Planning Effects

| Bead | ADR(s) | Impact |
|---|---|---|
| `bd-2cw.7` | ADR-001 | Parallel profile explicitly deferred beyond Wave A |
| `bd-ix8.1` | ADR-001 | Initial scaffold excludes parallel-only structure |
| `bd-hwb.1`, `bd-hwb.6` | ADR-002 | Allocator-vtable-based design required in first implementation wave |
| `bd-j4m.3` | ADR-003 | HFT thresholds benchmarked against busy-spin default behavior |
| `bd-j4m.4`, `bd-56t.3` | ADR-004 | Automotive lane requires evidence bundle + annotated skeleton outputs |
| `bd-296.24` | ADR-001 | Deferred-surface register must retain explicit parallel unblock criteria |
| `bd-296.7` | ADR-001..004 | Final scope/deferred checkpoint must consume this decision baseline |

### 4.2 CI / Gate Effects

| Gate | ADR(s) | Binding adjustment |
|---|---|---|
| Portability matrix | ADR-001 | No parallel CI/test lanes in Wave A; ad hoc compile-only scaffolds may exist for visibility but are not part of the portability matrix |
| Embedded target matrix | ADR-002 | Dynamic backend required now; static backend tests deferred |
| HFT tail/jitter gate | ADR-003 | Busy-spin baseline for HFT profile comparisons |
| Automotive deadline/watchdog gate | ADR-004 | Evidence bundle completeness + skeleton coverage checks |
| Profile parity gate | ADR-001, ADR-003 | Exclude parallel profile in Wave A; enforce semantic digest equivalence for included profiles |
| Semantic delta budget gate | ADR-001..004 | Remains zero-default; all ADR effects are resource/operational-plane only |

### 4.3 Test / E2E / Logging Effects

| Surface | ADR(s) | Obligation |
|---|---|---|
| Unit + invariant tests | ADR-002 | allocator backend callbacks + allocator-seal behavior tests |
| Unit + profile tests | ADR-003 | deterministic coverage of `busy_spin`, `yield`, `sleep` and per-profile defaults |
| E2E scenarios | ADR-001, ADR-004 | defer parallel lanes; include automotive evidence-capture lanes |
| Structured logs | ADR-001..004 | require `parallel_profile_enabled`, `allocator_backend`, `wait_policy`, `profile_name`, `evidence_bundle_version` fields |

## 5. Traceability and Evidence Links

Decision records must remain linked to semantic provenance and evidence assets:

| decision_id | provenance linkage | evidence expectation |
|---|---|---|
| ADR-001 | `SCHEDULER-001..006`, `CHANNEL-001..007`, `TIMER-001..008` in `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` | parity reports excluding parallel profile for Wave A; deferred-surface audit trail |
| ADR-002 | `BUDGET-001..002`, `HANDLE-001`, `QUIESCENCE-001`, `FINALIZE-001` | allocator backend test artifacts; resource exhaustion behavior traces |
| ADR-003 | `SCHEDULER-001..006`, `TIMER-001..004` | HFT tail/jitter benchmark logs and profile-default verification artifacts |
| ADR-004 | `QUIESCENCE-001`, `FINALIZE-001`, `OUTCOME-001..002` | evidence bundle index + annotated automotive mapping skeleton + watchdog/deadline scenario outputs |

## 6. Governance and Supersession

- This file is the canonical decision baseline for current planning/execution.
- New or changed decisions must be introduced via new ADR rows (`ADR-00X`) in this same format.
- Superseding an ADR requires:
  - explicit reason,
  - updated impact rows (beads/gates/tests/logs),
  - updated traceability/evidence links.
- Rollback triggers listed per ADR are mandatory review triggers, not optional notes.
