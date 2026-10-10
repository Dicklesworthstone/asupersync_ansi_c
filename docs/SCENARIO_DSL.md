# Scenario DSL Specification (Shared Rust/C Fixture Contract)

> **SUPERSEDED (2026-10-09)** by `docs/SCENARIO_DSL_V2.md` (`asx.scenario.v2`,
> bead bd-9kll.2.3). New scenarios use DSL v2 (`tests/conformance/scenarios_v2/`,
> captured by `tools/twin_run`). Everything below the line is a historical
> record of the v1 design; no code reads this format as written.
>
> What exists instead (checked 2026-10-10, bd-9kll.1.8):
>
> - The envelope of section 2 (`version`, `forbidden_ids`, `expected`) is not
>   read by any code; `forbidden_ids` occurs only in docs
>   (`grep -rl forbidden_ids` outside `.git`).
> - The legacy fixtures in `fixtures/rust_reference/` declare
>   `"scenario_dsl_version": "dsl-v1"` but use a different shape (`input.ops`,
>   `expected_events`, `expected_final_snapshot`, `expected_error_codes`) and
>   snake_case ops (`region_open`, `task_spawn`, `obligation_reserve`,
>   `channel_create`, `timer_register`, `scheduler_run`, ...), interpreted by
>   `tools/fixture_capture/src/main.rs:226-453`. That tool inserts task
>   records without futures (`main.rs:279-301`), so no Rust capture of those
>   fixtures polled a task. See `docs/RUST_FIXTURE_CAPTURE_TOOLING.md`.
> - Twelve of the sixteen opcodes in section 3.1 (`SpawnRegion`,
>   `CloseRegion`, `SpawnTask`, `ReserveObligation`, `CommitObligation`,
>   `AbortObligation`, `ChannelReserve`, `ChannelSend`, `ChannelAbort`,
>   `TimerRegister`, `TimerCancel`, `AdvanceTime`) are op names in the older
>   C differential-fuzz grammar (`tests/fuzz/fuzz_differential.c:161-167`)
>   and `tools/rust_fuzz_target/src/main.rs:210-230`, which use a compact
>   `{op, a, b, u32, u64}` record (`tests/fuzz/grammar_test_vectors.json`),
>   not the `{id, op, args, expect}` record of section 3.2. `PollTask`,
>   `RequestCancel`, `AckCancel` and `Assert` have no implementation anywhere
>   in the repository.

> **Bead:** `bd-296.5`
> **Status:** Superseded; see above
> **Last updated:** 2026-02-27 by CopperSpire (body); header 2026-10-10 (bd-9kll.1.8)

---

This DSL defines deterministic scenario inputs shared by Rust reference capture and ANSI C conformance/fuzz runners.

## 1. Design Goals

1. deterministic replay by explicit seed + ordered operations,
2. direct encoding of legal and forbidden behavior scenarios,
3. stable canonical serialization for diffing and minimization,
4. profile/codec-agnostic semantic intent.

## 2. Top-Level Shape

Canonical JSON envelope:

```json
{
  "scenario_id": "string",
  "version": "dsl-v1",
  "seed": 42,
  "profile": "ASX_PROFILE_CORE",
  "codec": "json",
  "forbidden_ids": [],
  "ops": [],
  "expected": {
    "final_snapshot": {},
    "error_codes": [],
    "semantic_digest": null
  }
}
```

## 3. Operation Grammar

### 3.1 EBNF (Conceptual)

```text
Scenario      = Header Ops Expected ;
Header        = scenario_id version seed profile codec [forbidden_ids] ;
Ops           = "ops" "[" { Op } "]" ;
Op            = SpawnRegion
              | CloseRegion
              | SpawnTask
              | PollTask
              | RequestCancel
              | AckCancel
              | ReserveObligation
              | CommitObligation
              | AbortObligation
              | ChannelReserve
              | ChannelSend
              | ChannelAbort
              | TimerRegister
              | TimerCancel
              | AdvanceTime
              | Assert ;
Expected      = final_snapshot error_codes [semantic_digest] [violation_kind] ;
```

### 3.2 Operation Record Format

Each op must include:

- `op`: stable opcode string,
- `id`: deterministic op sequence id,
- `args`: structured arguments,
- `expect`: optional per-op expected status/error.

Example:

```json
{ "id": 17, "op": "RequestCancel", "args": { "task": "t1", "kind": "Deadline" }, "expect": { "status": "ASX_OK" } }
```

## 4. Canonical Serialization Rules

To ensure stable hashing/minimization:

1. UTF-8 JSON, no comments.
2. Object keys sorted lexicographically.
3. No insignificant numeric format variants.
4. Omit null/empty optional fields unless semantically required.
5. `ops` order is authoritative and preserved.
6. `forbidden_ids` sorted lexicographically.

## 5. Forbidden Scenario Contract

A forbidden scenario MUST include:

- non-empty `forbidden_ids`,
- at least one expected violation in `expected.error_codes` or `expected.violation_kind`,
- operation sequence that deterministically triggers the violation.

Passing a forbidden scenario (no violation) is a conformance failure.

## 6. Minimization Contract

Counterexample minimizer may remove/rewrite ops only if it preserves:

1. same violated `forbidden_ids`,
2. same violation class/error code,
3. reproducibility under same seed/profile/codec.

Minimality objective:

- shortest op sequence with equivalent violation semantics.

## 7. Fixture Family Tags

Recommended tags for routing:

- `lifecycle`
- `obligation`
- `cancel`
- `channel`
- `timer`
- `quiescence`
- `exhaustion`
- `forbidden`

## 8. Runner Expectations

Runners must:

1. execute ops in-order with deterministic tie-break rules,
2. emit normalized event streams,
3. compare expected error/status outputs,
4. emit canonical semantic digest for parity comparison.

## 9. Versioning Policy

- `version` field is required.
- Backward-incompatible changes require new major DSL version (`dsl-v2`).
- New optional fields may be added in minor updates without changing canonical semantics.

## 10. Downstream Dependencies

Primary consumers:

- Rust fixture capture (`bd-1md.1`),
- C conformance runner (`bd-1md.2`),
- differential fuzzing + minimizer (`bd-1md.3`, `bd-1md.4`),
- codec/profile parity suites (`bd-1md.10`, `bd-1md.11`).

Related contract artifacts:

- `docs/RUST_FIXTURE_CAPTURE_TOOLING.md`
- `schemas/fixture_capture_manifest.schema.json`
- `schemas/canonical_fixture.schema.json`
- `schemas/core_fixture_family_manifest.schema.json`
- `schemas/robustness_fixture_family_manifest.schema.json`
