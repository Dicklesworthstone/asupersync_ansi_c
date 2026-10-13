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
| `twin_run generate --seed <u64> --count <n> --out <dir>` | Writes `count` generated scenarios named `gen-<seed>-<index>-001.json`, with nonempty rule tags and a valid identifier suffix at every index. Filenames follow the scenario IDs; the requested count is not clamped. | `tools/twin_run/src/main.rs::generate`; `tools/twin_run/src/gen.rs::scenario` |
| `twin_run minimize <scenario.json> --runner <asx-conformance> --out <dir>` | Delta-debugging reducer for a scenario whose Rust capture the C runtime FAILs: deletes tasks, script entries, program steps and declarations while Rust still runs the scenario and C still FAILs in the same part of the comparison. Writes the reduced scenario (`<id>-min`) and its Rust fixture. | `minimize.rs:1-17`, `minimize.rs:192-201`; `main.rs:221-247` |
| `twin_run trace <scenario.json>` | Prints the raw lab trace of one scenario, for diagnosing a divergence. | `main.rs:174-201` |

Generated scenarios (`gen.rs`). The main stream draws the program steps and
the world (sync objects, channels, regions, budgets, a script of cancels and
clock jumps); other shapes (cancel storms, futurelock probes: one scenario in
24 is one or two holders that reserve and wait while a driver that first
sleeps keeps the lab stepping past `lab.futurelock_max_idle_steps`, expecting
`futurelock`, or a lone polled holder expecting `ok`; `--shape timers`) and
derived streams (rich supervised children, `return cancelled`) add to it
without consuming it. The **extension stream** (`EXT_SALT`, `gen::extend`, bd-9kll.2.10)
inserts the op families the main stream never draws, into scenarios that are
otherwise what they were: intervals and delay queues, the watch receiver and
sender steps, monitors and links (the peer opens with a sleep, so it is live
when the request is applied), a server holding a monitor or a link, a handed
obligation, a `timeout` around a sync wait, `try_cast` and `server_evicted`,
the capability envelope steps, and the driver ops `region_close_outcome`,
`finish_region` and `close_region`. It keeps to what both engines can run: a
delay queue's entries are all due at once where a task can be cancelled or the
script jumps the clock (a poll with nothing due is a harness error), nothing
follows a monitor or link open where the holder can be cancelled (a refused
open binds no name), `close_region` only names a region no task lives in (its
drain does not move the clock), and the interpreter's tables (4 delay queues, 8
intervals) are not exceeded. A second pass (`ext_declarations`) sets the
options of declarations and steps the main stream never does: finalizers, async
or `sync` (non-blocking steps only), on regions (not on one a driver
`close_region` closes, nor below it), a mask
`depth`, the obligation kinds `IoOp`, `SemaphorePermit` and `Transaction`, the
server behaviors `latest`, `budgets` and `budgets_stop`, and a supervisor's
`storm_threshold_milli`, explicit `stop`, `depends_on`, `required` and
`registration`, and a task group (`join_all`, `first_ok`, `race`) in a
supervised child's program, whose members the interpreter scopes by the child's
generation, and some mpsc channels made unbounded (`"unbounded": true` with
`requires: ["unbounded_queue"]`; a channel `send_evict_oldest` or `closed` names
stays bounded, Rust's `UnboundedSender` has neither).
`the_generator_draws_every_op_of_the_dsl` and
`the_generator_sets_every_property_of_the_dsl` read the op and property names
of `schemas/scenario_dsl_v2.json` and require 1200 default-shape scenarios to
contain each (an exemption list gives the reason of each one that cannot), so
an op or an option added to the schema without a draw fails the unit tests.

`make fuzz-coverage` measures what the corpus still leaves out of the C
runtime: the conformance runner is built with `gcc --coverage` into its own
directory, run over the committed fixtures and over the captured fixtures of
the `make fuzz-differential` runs under `build/fuzz_v2/seed-*/fixtures`, and
`tools/ci/fuzz_coverage.py` lists, per file, the lines the fixtures execute and
the generated corpus does not, and the files with the most lines neither does.
With seeds 1 to 8 and 10 to 13 at 400 scenarios the generated corpus executes
55.7% of the library's lines against the fixtures' 55.8%, and 260 lines are
the fixtures' alone (778 before the declaration pass). It is a report, not a
gate.

Fixture contents (`tools/twin_run/src/run.rs:2072-2089`): `schema`
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
  `fuzz-rust-differential`) and nightly with 20000 scenarios, 5 shards of 4000
  on seeds `day*5+shard` (`.github/workflows/nightly.yml:83-94`).
- `make fuzz-minimize` (`Makefile:1617-1623`) runs `twin_run minimize` on each
  scenario the last `fuzz-differential` run reported as FAIL.
- `make fuzz-timers` (`FUZZ_TIMERS_SEED`, default 7100, and
  `FUZZ_TIMERS_COUNT`, default 1000) is `fuzz-differential` over
  `twin_run generate --shape timers` (bd-9kll.7.1): every scenario holds
  groups of timers whose deadlines share one 1 ms wheel tick, at levels from
  under 256 ms to the overflow heap, registered early and late, with
  timeouts, lock deadlines, cancels, waker moves and clock advances. Its
  directory is `build/fuzz_v2/seed-<seed>-timers`, so `fuzz-minimize`,
  `fuzz-ghost` and `fuzz-promote` take the same `FUZZ_V2_SHAPE=timers`. The
  shape draws from a stream of its own: the default `generate` output is
  unchanged.

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
`schedule.forced_schedule` and, with `forced-replay`, replays the schedule the
C runtime recorded for a scenario through `LabRuntime::run_forced_schedule`
(bd-9kll.9.3; docs/SCHEDULING_MODEL_CONTRACT.md).

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
