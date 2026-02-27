# Owner Decision Log (Resolved Decisions Only)

> **Bead:** `bd-296.25`  
> **Status:** Resolved-decision register with impact mapping  
> **Last updated:** 2026-02-27 by LilacTurtle

This log records only decisions that are already effective in project artifacts. Unresolved recommendations are intentionally excluded.

## 1. Inclusion Rules

A decision is included only if one of the following is true:

1. it is explicitly listed as owner-provided/resolved in the canonical plan, or
2. it is marked as applied/effective in repository governance artifacts.

Decisions still marked draft/recommendation-only are tracked as pending, not included in the resolved table.

## 2. Resolved Decision Register

| dec_id | decision | source evidence | effective status |
|---|---|---|---|
| `DEC-001` | C dialect baseline is ANSI C core with selective C99 use when it materially improves correctness/maintainability. | Plan section 17 resolved decisions; `AGENTS.md` toolchain policy. | resolved |
| `DEC-002` | Codec strategy is JSON-first for parity/debug, with clean architecture toggle to optimized binary codec. | Plan section 17 resolved decisions; `AGENTS.md` codec macros. | resolved |
| `DEC-003` | Optional parallel profile is deferred from Wave A kernel milestone (Wave B+), with explicit unblock criteria. | `DEFERRED_SURFACE_REGISTER.md` DS-P01 status “Deferred per ADR-001”; deferred-surface audit trail marks ADR-001 applied. | resolved-in-repo |
| `DEC-004` | Static arena backend is deferred; Wave A ships allocator-vtable-compatible dynamic backend plus allocator seal hook. | `DEFERRED_SURFACE_REGISTER.md` DS-S01 status “deferred per ADR-002”; deferred-surface audit trail marks ADR-002 applied. | resolved-in-repo |
| `DEC-005` | Semantic drift is disallowed by default: resource-plane tuning may vary, semantic behavior may not fork; semantic delta budget defaults to zero. | `AGENTS.md` core rule; plan semantic-delta policy and kernel DoD gate language. | resolved |

## 3. Decision-to-Impact Matrix

| dec_id | affected beads | CI gate impact | unit/invariant obligations | e2e/scenario obligations | logging/manifest obligations |
|---|---|---|---|---|---|
| `DEC-001` | `bd-hwb.1`, `bd-hwb.3`, `bd-ix8.2` | compiler matrix (`gcc/clang/msvc`, 32/64) must preserve C99 portability boundaries | verify APIs/types compile and behave under configured dialect switches | walking-skeleton scenario must run under baseline profile with same semantics | build manifests must record compiler + profile + language mode |
| `DEC-002` | `bd-2n0.*`, `bd-1md.2`, `bd-1md.11` | codec-equivalence and conformance gates are mandatory | unit coverage for canonical schema encode/decode paths | same scenario must match semantic digest across JSON/BIN | parity reports include `codec`, `semantic_digest`, `delta_classification` |
| `DEC-003` | `bd-2cw.7` (deferred), `bd-296.24`, `bd-296.10`, `bd-ix8.1` | Wave A excludes parallel-profile gates; parity scope remains single-thread kernel profiles | unit/invariant suites for Wave A do not assume parallel semantics | no parallel scheduling scenarios in Wave A e2e packs | deferred-surface entries + audit trail must keep explicit unblock criteria |
| `DEC-004` | `bd-hwb.6`, `bd-hwb.1`, `bd-j4m.*` (future static backend parity) | embedded/profile gates run dynamic allocator backend in Wave A; static backend gate deferred | allocator-vtable behavior and seal semantics must be unit-tested | wave scenarios must verify deterministic behavior independent of allocator backend | runtime logs record allocator backend and seal mode in artifacts |
| `DEC-005` | `bd-296.6`, `bd-2cw.*`, `bd-hwb.*`, `bd-1md.*`, `bd-66l.9` | semantic-delta budget gate default `0`; profile parity/conformance drift blocks merges | invariant suites must fail on lifecycle/cancellation/linearity semantic divergence | cross-profile shared scenarios must keep equal canonical digest | closure manifests require explicit decision/fixture/evidence linkage |

## 4. Explicitly Excluded Pending Decisions

These are intentionally excluded because they are still draft/recommendation-only:

1. ADR-003 default HFT wait-policy recommendation.
2. ADR-004 automotive assurance-scope recommendation.

They may be added only after explicit effective-status evidence is recorded.

## 5. Traceability Keys for Downstream Indexing

Downstream artifacts should reference decisions via `DEC-*` keys:

- `DEC-001` language mode and portability assumptions,
- `DEC-002` codec/parity assumptions,
- `DEC-003` parallel profile deferral scope,
- `DEC-004` allocator backend scope,
- `DEC-005` semantic-delta/no-drift enforcement.

This keeps bead closure notes and CI evidence bundles aligned to a stable decision namespace.

## 6. Pending Decision Queue (Explicitly Unresolved)

To prevent ambiguous scope, unresolved recommendations remain listed here until effective-status evidence exists.

| pending_id | source ADR | current state | unblock evidence required | tracking owner |
|---|---|---|---|---|
| `PEND-001` | ADR-003 (HFT wait-policy recommendation) | recommendation only | profile-default decision recorded as effective in canonical decision artifact and linked to CI gate thresholds | project maintainer |
| `PEND-002` | ADR-004 (automotive assurance-scope recommendation) | recommendation only | effective decision record plus evidence-bundle/skeleton contract acceptance | project maintainer |

## 7. Scope-Governance Consistency Checks

`bd-296.7` consolidation invariants:

1. Every deferred item in `docs/DEFERRED_SURFACE_REGISTER.md` has explicit status and unblock criteria.
2. Every effective decision in this log has at least one downstream bead/gate/test/log mapping row.
3. No recommendation-only ADR is treated as an effective decision without evidence.
4. Downstream planning beads (`bd-296.10`, `bd-296.8`) must reference these decision IDs directly.
