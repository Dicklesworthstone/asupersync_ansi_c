# Plan-to-Execution Traceability Index

> **Bead:** `bd-296.8`
> **Status:** Canonical traceability index (plan -> beads -> tests -> artifacts)
> **Last updated:** 2026-02-27 by CopperSpire, updated by PearlSwan

This index links plan commitments to bead execution, target test obligations, and concrete artifact outputs.

## 1. Traceability Contract

For each scoped semantic commitment, record:

1. originating plan section/theme,
2. execution bead(s),
3. required test/gate surface,
4. produced artifact(s),
5. current status.

No commitment is considered complete without an artifact and a mapped verification surface.

## 2. Core Traceability Table

| Trace ID | Plan Commitment Theme | Bead(s) | Verification Surface | Artifact(s) | Status |
|---|---|---|---|---|---|
| `TRC-SEM-001` | Canonical lifecycle transitions | `bd-296.15` | lifecycle unit + invariant suites | `docs/LIFECYCLE_TRANSITION_TABLES.md` | complete |
| `TRC-SEM-002` | Outcome lattice + budget algebra + exhaustion | `bd-296.16` | algebraic law fixtures + exhaustion tests | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | complete |
| `TRC-SEM-003` | Channel/timer determinism contract | `bd-296.17` | channel/timer conformance fixtures | `docs/CHANNEL_TIMER_DETERMINISM.md` | complete |
| `TRC-SEM-004` | Finalization/quiescence/leak invariants | `bd-296.18` | finalization/quiescence invariant tests | `docs/QUIESCENCE_FINALIZATION_INVARIANTS.md` | complete |
| `TRC-SEM-005` | Source-to-fixture provenance mapping | `bd-296.19` | provenance consistency checks | `docs/SOURCE_TO_FIXTURE_PROVENANCE_MAP.md` | complete |
| `TRC-SEM-006` | Exhaustive canonical extraction aggregation | `bd-296.1` | Phase 1 review packet + parity row checks | `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | complete |
| `TRC-ARCH-001` | Non-transliterative C architecture from spec | `bd-296.2` | architecture review + scaffold conformance | `docs/PROPOSED_ANSI_C_ARCHITECTURE.md` | complete |
| `TRC-PAR-001` | Semantic-unit parity scoreboard | `bd-296.3` | parity row completeness checks | `docs/FEATURE_PARITY.md` | complete |
| `TRC-INV-001` | Machine-readable invariant schema baseline | `bd-296.4` | schema validation + generated legality tests | `docs/INVARIANT_SCHEMA.md` | in_progress/complete by owner |
| `TRC-FBD-001` | Forbidden behavior catalog + shared scenario DSL | `bd-296.5` | forbidden fixture family + DSL parser/runner tests | `docs/FORBIDDEN_BEHAVIOR_CATALOG.md`, `docs/SCENARIO_DSL.md` | complete |
| `TRC-FIXCAP-001` | Rust reference fixture capture tooling contract | `bd-1md.1` | fixture schema validation + deterministic replay-check workflow | `docs/RUST_FIXTURE_CAPTURE_TOOLING.md`, `schemas/fixture_capture_manifest.schema.json`, `schemas/canonical_fixture.schema.json` | complete |
| `TRC-FIXCORE-001` | Core semantic fixture-family capture specification | `bd-1md.13` | family-manifest validation + e2e lane completeness checks | `docs/CORE_SEMANTIC_FIXTURE_FAMILIES.md`, `schemas/core_fixture_family_manifest.schema.json` | complete |
| `TRC-FIXROB-001` | Robustness fixture-family capture specification | `bd-1md.14` | boundary-class manifest validation + first-failure triage pointer completeness | `docs/ROBUSTNESS_FIXTURE_FAMILIES.md`, `schemas/robustness_fixture_family_manifest.schema.json` | complete |
| `TRC-FIXVERT-001` | Vertical and continuity fixture-family capture specification | `bd-1md.15` | vertical/continuity manifest validation + e2e lane completeness + profile/replay evidence checks | `docs/VERTICAL_CONTINUITY_FIXTURE_FAMILIES.md`, `schemas/vertical_continuity_fixture_family_manifest.schema.json` | complete |
| `TRC-GSM-001` | Rust->C guarantee substitution map | `bd-296.6` | substitution-evidence checklist + linked fixtures | `docs/GUARANTEE_SUBSTITUTION_MATRIX.md` | in_progress/complete by owner |
| `TRC-GSM-002` | Anti-butchering proof-block enforcement for semantic-sensitive changes | `bd-66l.7` | `make lint-anti-butchering` gate + check-lane manifest | `tools/ci/check_anti_butchering.sh`, `build/conformance/anti_butcher_*.json`, `.github/workflows/ci.yml` | in_progress |
| `TRC-GSM-003` | Semantic delta budget gate with approved exception ledger | `bd-66l.3` | `make conformance` semantic-delta budget evaluation + exception ledger validation | `tools/ci/run_conformance.sh`, `build/conformance/semantic_delta_*.json`, `docs/SEMANTIC_DELTA_EXCEPTIONS.json` | in_progress |
| `TRC-WAV-001` | Wave A/B/C/D gating protocol | `bd-296.10` | gate decision audit + blocked-wave enforcement | `docs/WAVE_GATING_PROTOCOL.md` | complete |
| `TRC-PROF-001` | Profile companion specs (embedded/HFT/automotive) | `bd-296.9` | profile parity + vertical fixture family mapping | `docs/EMBEDDED_TARGET_PROFILES.md`, `docs/HFT_PROFILE.md`, `docs/AUTOMOTIVE_PROFILE.md` | complete |
| `TRC-PORT-001` | C portability + UB-elimination contract | `bd-296.29` | warning/static-analysis/UB-focused gates | `docs/C_PORTABILITY_RULES.md` | complete |
| `TRC-REVIEW-001` | Phase 1 independent review gate | `bd-296.30` | reviewer sign-off + dispute/fixture rulings | `docs/PHASE1_SPEC_REVIEW_GATE.md`, parity status records | complete |
| `TRC-BUILD-001` | Phase 2 scaffold and matrix bring-up | `bd-ix8.*` | build/lint/test matrix + embedded/QEMU jobs + reproducibility logging contract | `Makefile`, `tools/ci/run_compiler_matrix.sh`, `tools/ci/check_endian_assumptions.sh`, `tools/ci/run_embedded_matrix.sh`, `docs/PHASE2_SCAFFOLD_MANIFEST.md`, `tools/ci/artifacts/manifest.jsonl` (planned) | in_progress |

## 3. Test/Gate Family Index

| Family ID | Scope | Typical Commands/Gates |
|---|---|---|
| `TG-UNIT` | module-level API/logic correctness | `make test-unit` |
| `TG-INV` | lifecycle + legality + quiescence invariants | `make test-invariants` |
| `TG-CONF` | Rust fixture parity | `make conformance` |
| `TG-CODEC` | JSON/BIN semantic equivalence | `make codec-equivalence` |
| `TG-PROFILE` | cross-profile semantic parity | `make profile-parity` |
| `TG-FUZZ` | differential fuzz + minimization | `make fuzz-smoke` |
| `TG-BUILD` | compiler/target matrix | `make build` + matrix scripts |
| `TG-EMBED` | embedded triplet + QEMU validation | embedded matrix + `qemu` harness |

## 4. Evidence Linking Rules

Each trace row should always maintain links to:

- at least one bead id,
- at least one artifact path,
- at least one verification family.

Rows with missing links are `incomplete` and cannot be counted as done in review gates.

## 5. Update Workflow

When work lands:

1. update relevant row `status`,
2. append/refresh artifact paths,
3. add or adjust verification surface linkage,
4. ensure corresponding `FEATURE_PARITY.md` rows remain aligned.

## 6. Planned Extensions

Future iterations may add machine-readable export (`traceability_index.json`) with stable IDs so CI can validate link completeness automatically.
