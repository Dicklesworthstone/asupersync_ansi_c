# Owner Decision Log (Resolved Decisions Only)

> **Bead:** `bd-296.25`  
> **Status:** Resolved-decision register with impact mapping  
> **Last updated:** 2026-05-08 by SilverFrog

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
| `DEC-003` | Optional parallel profile was deferred from the Wave A kernel milestone, then reopened for post-kernel production graduation under `bd-pweu` with evidence-gated operator claims. | `DEFERRED_SURFACE_REGISTER.md` DS-P01 status; `QUALITY_GATES.md` parallel graduation gate; closed `bd-pweu.1`, `.7`, `.9`, `.10`, `.11`, `.12`, `.13`, `.14`, `.15`. | resolved-in-repo; graduation-evidence-complete |
| `DEC-004` | Static arena implementation is deferred; Wave A ships allocator-vtable-compatible dynamic backend plus allocator seal hook, and Wave C now carries the sizing/seal contract for the implementation bead. | `DEFERRED_SURFACE_REGISTER.md` DS-S01 status; deferred-surface audit trail marks ADR-002 applied; `bd-v12u.5` defines the static arena contract while `bd-v12u.6` owns implementation. | resolved-in-repo |
| `DEC-005` | Semantic drift is disallowed by default: resource-plane tuning may vary, semantic behavior may not fork; semantic delta budget defaults to zero. | `AGENTS.md` core rule; plan semantic-delta policy and kernel DoD gate language. | resolved |
| `DEC-006` | D1: cancel-kind ordinals follow Rust (ParentCancelled 7, ResourceUnavailable 8, Shutdown 9, LinkedExit 10). ABI break accepted; no compatibility shim. | Owner delegation 2026-10-12 (owner: "I ... defer to your judgement and expertise across the board"); decided by orchestrator AmberSummit adopting the `bd-9kll.17.1` recommendation. Revisitable by the owner. | resolved (delegated) |
| `DEC-007` | D2: the default obligation leak response is PANIC, as in Rust (RuntimeConfig and LabConfig); tests and callers that leak on purpose set LOG/SILENT/RECOVER explicitly. | Same delegation as DEC-006; adopts the `bd-9kll.17.1` recommendation; closes the drift recorded in `bd-9kll.2.19` / `bd-9kll.3.6`. | resolved (delegated) |
| `DEC-008` | D3: unbounded MPSC is provided, bounded only by the arena and failure-atomic on exhaustion, and refused in profiles that declare `ASX_FORBID_UNBOUNDED_QUEUE`. | Same delegation; adopts the `bd-9kll.17.1` recommendation (`bd-9kll.5.7`). | resolved (delegated) |
| `DEC-009` | D4: TLS goes through an `asx_tls_provider` hook; with no provider the `asx_tls_*` calls refuse with a typed error; the plaintext passthrough becomes an explicitly selected, test-only null provider. Crypto adapters stay outside the dependency-free core. | Same delegation; adopts the `bd-9kll.17.1` recommendation (`bd-9kll.17.2`). | resolved (delegated) |
| `DEC-010` | D5: QUIC, gRPC, DB, messaging, distributed, transport and remote are deterministic lab models; they are excluded from README feature claims and labelled MODEL-ONLY in their headers. | Same delegation; adopts the `bd-9kll.17.1` recommendation (`bd-9kll.17.3`). | resolved (delegated) |
| `DEC-011` | D6: an `ASX_KERNEL_ONLY` build (kernel without the conformance interpreter, CLI and protocol models) is the artifact measured against the CORE footprint targets. | Same delegation; adopts the `bd-9kll.17.1` recommendation (`bd-9kll.13.3`). | resolved (delegated) |
| `DEC-012` | S2: a poll function that returns `ASX_E_PENDING` without parking is an explicit yield (immediate self-wake). This stays a documented C API rule rather than Rust's never-repolled futurelock; instead, lab dispatch reports the stuck task when every task is parked (`bd-9kll.9.6`). | Same delegation; `bd-9kll.4.3` audit (inverting the contract touches ~800 `ASX_E_PENDING` sites with no fixture-observable gain, since DSL `yield` already maps to this rule). | resolved (delegated) |
| `DEC-013` | K14: `asx_outcome` carries the cancel reason and a panic descriptor as in Rust's `Outcome`; the ABI major version is bumped; no `_ex` shim. | Same delegation; AGENTS.md no-backwards-compatibility rule (`bd-9kll.3.14`). | resolved (delegated) |

D7 (deletions: tracked ELF binaries under `tests/fuzz`, legacy `asx_gen_server_*`, `tools/rust_fuzz_target`, `tools/zero_drift_extractor.py`, `timer_wheel.c`) is **not** decided: AGENTS.md RULE 1 requires the owner's explicit, item-by-item approval for every deletion.

## 3. Decision-to-Impact Matrix

| dec_id | affected beads | CI gate impact | unit/invariant obligations | e2e/scenario obligations | logging/manifest obligations |
|---|---|---|---|---|---|
| `DEC-001` | `bd-hwb.1`, `bd-hwb.3`, `bd-ix8.2` | compiler matrix (`gcc/clang/msvc`, 32/64) must preserve C99 portability boundaries | verify APIs/types compile and behave under configured dialect switches | walking-skeleton scenario must run under baseline profile with same semantics | build manifests must record compiler + profile + language mode |
| `DEC-002` | `bd-2n0.*`, `bd-1md.2`, `bd-1md.11` | codec-equivalence and conformance gates are mandatory | unit coverage for canonical schema encode/decode paths | same scenario must match semantic digest across JSON/BIN | parity reports include `codec`, `semantic_digest`, `delta_classification` |
| `DEC-003` | `bd-2cw.7`, `bd-296.24`, `bd-296.10`, `bd-ix8.1`, `bd-pweu.*` | Historical Wave A excluded parallel-profile gates; current post-kernel graduation requires `parallel-parity`, parallel telemetry/locality, large-swarm e2e, formal memory-model proof gates, and RCH-backed benchmark baselines | unit/invariant suites must preserve single-worker fallback semantics, parallel worker-count boundaries, stale-handle rejection, and compact-vs-sharded trace isomorphism | `tests/e2e/parallel_swarm.sh` records seed, worker count, profile, digest, and rerun command for current parallel scenarios | deferred-surface entries distinguish landed parity/e2e/proof/locality/benchmark evidence from remaining native-adapter concurrency claims |
| `DEC-004` | `bd-hwb.6`, `bd-hwb.1`, `bd-j4m.*`, `bd-v12u.5`, `bd-v12u.6` | embedded/profile gates run dynamic allocator backend in Wave A; `GATE-STATIC-ARENA-PARITY` binds the Wave C backend parity slice | allocator-vtable behavior, resource-class sizing defaults, partial rollback, and seal semantics must be unit-tested | wave scenarios must verify deterministic behavior independent of allocator backend | runtime logs record allocator backend, resource class, and seal mode in artifacts |
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
- `DEC-003` historical parallel profile deferral and current `bd-pweu`
  graduation boundary,
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
