# Rust Reference Fixture Capture Tooling

> Bead: `bd-1md.1` (original contract); truth pass `bd-9kll.1.8`, 2026-10-10
> Status: **`tools/fixture_capture` is deprecated.** Rust fixtures used for
> parity are captured by `tools/twin_run` (section 2). Sections 3-4 describe
> what `tools/fixture_capture` actually does today. Section 5 records the CLI
> this document originally specified; it was never implemented.

## 1. Summary

| Tool | Input | Output | Used by |
|---|---|---|---|
| `tools/twin_run` (binary `twin_run`) | DSL v2 scenarios (`asx.scenario.v2`, `docs/SCENARIO_DSL_V2.md`), e.g. `tests/conformance/scenarios_v2/*.json` | one `asx.fixture.v2` file per scenario, e.g. `fixtures/rust_reference_v2/*.json` | `make conformance`, `make fuzz-differential`, `make fuzz-minimize` |
| `tools/fixture_capture` (binary `fixture_capture`), deprecated | v1 fixture files with an `input.ops` list (`fixtures/rust_reference/**`) | the same files rewritten in place | `tools/ci/capture_rust_fixtures.sh` only |

Both tools link asupersync at rev `5e60b1c4c53d62aaddae68de3ee7de4732f1755b`
(`tools/twin_run/Cargo.toml:15`, `tools/fixture_capture/Cargo.toml:12`).

## 2. `tools/twin_run` (current capture tool)

Subcommands (`tools/twin_run/src/main.rs:23-29`, dispatch at `main.rs:155-249`):

| Command | What it does | Source |
|---|---|---|
| `twin_run capture <scenario.json>... --out <dir>` | Runs each DSL v2 scenario in asupersync's `LabRuntime`, projects the run into vocabulary v2 and writes `<dir>/<id>.json` as RFC 8785 canonical JSON. Prints `PASS <id> <semantic_digest>` or `FAIL <id>: <reason>` per scenario and a final `twin_run capture: N scenario(s), M failed` line; exits 1 if any scenario failed. An unsupported op, an unprojectable trace event or a run error fails that scenario and no fixture is written for it. | `main.rs:1-8`, `main.rs:86-153`; `canon.rs:1-20` |
| `twin_run generate --seed <u64> --count <n> --out <dir>` | Writes `count` generated scenarios named `gen-<seed>-<index>.json`. | `main.rs:33-56`, `main.rs:202-220`; generator in `src/gen.rs` |
| `twin_run minimize <scenario.json> --runner <asx-conformance> --out <dir>` | Delta-debugging reducer for a scenario whose Rust capture the C runtime FAILs: deletes tasks, script entries, program steps and declarations while Rust still runs the scenario and C still FAILs in the same part of the comparison. Writes the reduced scenario (`<id>-min`) and its Rust fixture. | `minimize.rs:1-17`, `minimize.rs:192-201`; `main.rs:221-247` |
| `twin_run trace <scenario.json>` | Prints the raw lab trace of one scenario, for diagnosing a divergence. | `main.rs:174-201` |

Fixture contents (`tools/twin_run/src/run.rs:1996-2013`): `schema`
(`asx.fixture.v2`), `scenario_id`, the embedded `scenario`, `vocabulary`,
`trace`, `snapshot`, `observations`, `trace_digest`, `snapshot_digest`,
`semantic_digest`, and `schedule` (`certificate_hash`, `decisions`,
`forced_schedule`, `dispatches`).

Provenance written into every fixture (`main.rs:70-84`):
`rust_baseline_commit` (read from the tool's own `Cargo.lock`, `main.rs:58-68`),
`rust_toolchain_commit_hash` / `rust_toolchain_release` / `rust_toolchain_host`
(the compiler that built the binary, set by `build.rs`), `cargo_lock_sha256`,
`capture_run_id`, `scenario_dsl` (`asx.scenario.v2`), `vocabulary`
(`asx.vocab.v2`) and `producer` (`tools/twin_run`). Every file in
`fixtures/rust_reference_v2` carries `producer: tools/twin_run` and
`rust_baseline_commit: 5e60b1c4c...` (checked with a short Python loop over
`fixtures/rust_reference_v2/*.json` on 2026-10-10).

Consumers:

- `make conformance` (`Makefile:1562-1572`) runs `build/bin/asx-conformance
  compare` on every `fixtures/rust_reference_v2/*.json`. The runner executes
  each fixture's embedded scenario through the C runtime and compares trace,
  snapshot, observations and the lab dispatch order with the Rust capture,
  canonical byte for byte; it exits 1 unless every fixture passes
  (`tools/conformance/runner.c:7-12`). A self-test with one corrupted
  expected trace event runs first and must be reported as FAIL
  (`runner.c:13-17`, `Makefile:1571`).
- `make fuzz-differential FUZZ_V2_SEED=<s> FUZZ_V2_COUNT=<n>`
  (`Makefile:1590-1608`) runs `twin_run generate`, then `twin_run capture`,
  fails if fewer than 90% of the scenarios were captured, and compares the
  captured fixtures with `asx-conformance compare`. CI runs it with seed 9 and
  200 scenarios on every push (`.github/workflows/ci.yml:978-1009`, job
  `fuzz-rust-differential`) and with three seeds of 200 scenarios nightly
  (`.github/workflows/nightly.yml:66-72`).
- `make fuzz-minimize` (`Makefile:1617-1623`) runs `twin_run minimize` on each
  scenario the last `fuzz-differential` run reported as FAIL.

## 3. `tools/fixture_capture` (deprecated): what exists

- Binary name `fixture_capture` (`tools/fixture_capture/Cargo.toml:7-9`). No
  binary or script named `asx-fixture-capture` exists in the repository.
- No subcommands. Flags (`tools/fixture_capture/src/main.rs:28-87`):
  - `--fixture-dir <dir>`: "ops mode". Walks each family subdirectory of
    `<dir>` in sorted path order, executes every `*.json` file's `input.ops`
    against a fresh `LabRuntime` and **rewrites the file in place**
    (`main.rs:101-180`, write at `main.rs:135-137`). Logs one JSON line per
    file and a summary line to stderr; exits 1 if any file failed.
  - `--scenario <path.yaml> [--seed N]` and `--scenario-dir <dir>
    [--output-dir <dir>]`: "scenario mode". Parses asupersync lab `Scenario`
    YAML, runs it until quiescent and prints or writes a fixture; the
    directory form also writes a `manifest.json` with `fixture_schema_version`,
    `capture_run_id` and one entry per scenario (`main.rs:583-758`). No
    scenario YAML files exist in this repository.
- Seed: the input file's `seed`, or 42 when absent (`main.rs:205`).
- Provenance (`main.rs:818-859`): `rust_baseline_commit` (from the tool's
  `Cargo.lock`), `rust_toolchain_commit_hash`, `rust_toolchain_release`,
  `rust_toolchain_host` (from `rustc --version --verbose` on `PATH` at capture
  time, `main.rs:821-825`), `cargo_lock_sha256`, `capture_run_id`.
- Driver: `tools/ci/capture_rust_fixtures.sh` copies `fixtures/rust_reference`
  into `build/fixture_staging/<run_id>/` (`capture_rust_fixtures.sh:53`), runs
  `fixture_capture --fixture-dir` on the copy (`:57`) and prints a diff
  summary against the committed corpus. Promotion of a staged run is a
  separate step, `make fixtures-promote RUN_ID=<run_id>` (`Makefile:1637-1641`,
  `tools/ci/promote_fixtures.sh`). No Makefile target builds or runs
  `fixture_capture` directly.
- The committed corpus in `fixtures/rust_reference` (70 files, `find
  fixtures/rust_reference -type f | wc -l`) carries `rust_baseline_commit`
  `a9e737d869c04c2f6aa4222fe38be66b28628227` in every file, so it has not been
  re-captured at the rev the tool now pins. `tools/ci/run_conformance.sh`
  classifies 4 of these records as hand-authored and excludes them from Rust
  parity (`run_conformance.sh:497`; the CI log of run 38051289151 reports
  `hand_authored_records=4`).

## 4. Limits of `tools/fixture_capture` (ops mode)

The ops mode records events for most ops itself instead of running the
corresponding Rust code, so its fixtures do not show Rust runtime behaviour
for those ops:

| Op(s) | What the tool does | Source |
|---|---|---|
| `task_spawn` | Inserts a `TaskRecord` with no future and records a `spawn` trace event itself. No task body exists, so no task is ever polled. `obligation_reserve` creates such a task the same way when the region has none. | `main.rs:279-301`, `main.rs:534-565` |
| `task_cancel` | Always uses `CancelKind::User`; for a task handle it cancels the task's owning region. | `main.rs:303-327` (reason at `:309`, `:319`) |
| `budget_meet` | Records a `user_trace` string; `Budget::meet` is never called. | `main.rs:262-277` |
| `channel_create`, `channel_reserve`, `channel_send`, `channel_send_immediate`, `channel_try_send`, `channel_recv` | Recorded as `user_trace` strings; no channel is created. | `main.rs:381-401` |
| `timer_register`, `timer_cancel` | Record `timer_scheduled` / `timer_cancelled` trace events with a local counter as the timer id; no timer is registered with the runtime. | `main.rs:403-424` |
| `timer_check_fired`, `timer_check_fire_order` | Recorded as `user_trace`; nothing is checked. | `main.rs:433-440` |
| any other op (for example the `combinator_*` ops in `fixtures/rust_reference/core_combinator`) | Warning on stderr, recorded as `user_trace` `unknown_op:<name>`. | `main.rs:446-452` |
| `obligation_reserve` | Always `ObligationKind::SendPermit`. | `main.rs:346-351` |
| final snapshot | Only live region, live task and pending obligation counts. | `main.rs:475-479` |

In scenario mode each scheduled fault is recorded as a `user_trace` event
(`fault:<action>:<args>`) after advancing time; the loop does not call a
fault-injection API (`main.rs:654-693`).

## 5. Originally specified contract (not implemented)

This document originally specified an `asx-fixture-capture` CLI with
`capture`, `validate`, `replay-check` and `emit-provenance` subcommands, an
output tree of `manifest.json`, `provenance.json`, `scenarios/<id>.json` and
`reports/{reproducibility,validation}.json`, seed derivation from
`sha256(scenario_id)`, and provenance fields including `cargo_lock_bytes`,
`fixture_schema_version`, `scenario_dsl_version` and `capture_tool_version`.
None of these subcommands, outputs or the seed derivation exist in
`tools/fixture_capture`, and its provenance block has none of those four
fields (section 3). Neither tool has a `replay-check` step that re-runs a
capture and compares digests; `twin_run` records the lab's forced schedule in
`schedule.forced_schedule` (`run.rs:1910-1923`) but does not replay it.

## 6. Schemas

These schema files exist in `schemas/`; `tools/ci/validate_schemas.sh`
validates files matching the listed globs:

| Schema | Validated files |
|---|---|
| `canonical_fixture.schema.json` | `fixtures/rust_reference/**/*.json` (`validate_schemas.sh:111-117`) |
| `fixture_capture_manifest.schema.json` | `fixtures/**/*capture*manifest*.json` (`:151-157`); no such file exists |
| `core_fixture_family_manifest.schema.json` | `fixtures/**/*core*manifest*.json` (`:121-127`); no such file exists |
| `robustness_fixture_family_manifest.schema.json` | `fixtures/**/*robustness*manifest*.json` (`:131-137`); no such file exists |
| `vertical_continuity_fixture_family_manifest.schema.json` | `fixtures/**/*vertical*manifest*.json` (`:141-147`); no such file exists |

DSL v2 scenarios have their own schema, `schemas/scenario_dsl_v2.json`.
