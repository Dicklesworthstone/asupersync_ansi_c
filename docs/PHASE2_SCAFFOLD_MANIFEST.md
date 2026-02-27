# Phase 2 Scaffold Manifest (Build + Toolchain + Skeleton)

> **Lane:** `bd-ix8` epic prework
> **Status:** Execution manifest for Phase 2/2.5 scaffolding tasks
> **Last updated:** 2026-02-27 by CopperSpire, updated by PearlSwan

This manifest turns Phase 2 requirements in `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` into an implementation-ready scaffold contract so `bd-ix8.1` and `bd-ix8.2` can be executed with minimal ambiguity.

## 1. Normative Inputs

Primary source sections:

- `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md`:
  - Phase 1 exit gate
  - Phase 2 deliverables
  - Phase 2.5 walking skeleton
  - profile matrix + quality gates
- `README.md` architecture + directory intent
- `AGENTS.md` quality gates, profile requirements, and CI expectations

## 2. Repository Skeleton Contract

Target scaffold tree for first executable skeleton:

```text
include/asx/
  asx.h
  asx_config.h
  asx_ids.h
  asx_status.h

src/core/
  ids.c
  status.c
  outcome.c
  budget.c
  cancel.c

src/runtime/
  scheduler.c
  lifecycle.c
  quiescence.c

src/channel/
  mpsc.c

src/time/
  timer_wheel.c

src/platform/
  posix/
    hooks.c
  win32/
    hooks.c
  freestanding/
    hooks.c

tests/unit/
  core/
  runtime/

tests/invariant/
  lifecycle/
  quiescence/

tests/conformance/
  fixtures/

fixtures/rust_reference/

tools/
  ci/
  replay/
  fuzz/

docs/
```

Notes:

- Tree is intentionally minimal and buildable; semantic completeness comes in later phases.
- Empty directories should be introduced only alongside at least one owned artifact to avoid dead placeholders.

## 3. Module Boundary Rules

Boundary intent for scaffold stage:

1. `include/asx/*` defines only stable public contracts; no profile-specific internals leak here.
2. `src/core` contains pure semantic primitives with no platform hooks.
3. `src/runtime` may depend on `src/core`, never the reverse.
4. `src/platform/*` adapts hooks and profile integration points behind explicit interfaces.
5. `tests/invariant` validates state-machine legality independent of full runtime throughput.

### 3.1 Module Ownership Map

Ownership for initial scaffold boundaries:

| Path root | Ownership intent | Allowed dependencies |
|---|---|---|
| `include/asx/` | public API contracts only (`asx_*` names, stable types/macros) | may reference only other public headers |
| `src/core/` | semantic primitives and type logic | may include `include/asx/*`; no platform adapters |
| `src/runtime/` | scheduler/lifecycle/quiescence orchestration | may depend on `src/core` + public headers |
| `src/channel/` | bounded MPSC kernel channel semantics | may depend on `src/core`, `src/runtime` contracts |
| `src/time/` | timer wheel and deadline abstractions | may depend on `src/core`, `src/runtime` contracts |
| `src/platform/` | profile/platform hook adaptation only | may depend on public headers + adapter interfaces |
| `tests/unit/` | per-module correctness tests | may touch module-under-test + public contracts |
| `tests/invariant/` | lifecycle legality/quiescence invariants | may depend on core/runtime test harness |
| `tests/conformance/` | fixture parity and digest checks | may depend on fixture schemas + runtime entrypoints |
| `fixtures/rust_reference/` | captured Rust baseline fixtures | data-only canonical artifacts |
| `tools/` | CI/replay/fuzz tooling scripts | must not introduce runtime-core semantic drift |

### 3.2 Naming Conventions

Scaffold naming rules for consistency and auditability:

1. Public symbols use `asx_` prefix (`asx_runtime_create`, `asx_status`, etc.).
2. Internal helper symbols use module prefix (`asx_core_*`, `asx_runtime_*`, `asx_timer_*`, `asx_mpsc_*`).
3. Header names mirror semantic surface (`asx_status.h`, `asx_budget.h`, `asx_cancel.h`).
4. Source file names are lowercase snake_case and semantic (no `v2`, `new`, or variant suffixes).
5. Unit tests use `<module>_<behavior>_test.c`.
6. Invariant tests use `<domain>_<invariant>_inv_test.c`.
7. Fixture IDs remain stable and kebab-case (`region-lifecycle-001`, `robust-exhaustion-001`).
8. Scenario IDs must be globally unique and deterministic (`<domain>-<intent>-<nnn>`).

## 4. Makefile Baseline Target Matrix

Required first-pass targets:

```make
build
format-check
lint
test
test-unit
test-invariants
conformance
profile-parity
fuzz-smoke
```

Scaffold support targets (Phase 2 bring-up):

```make
build-gcc
build-clang
build-msvc
build-32
build-64
build-embedded-mipsel
build-embedded-armv7
build-embedded-aarch64
qemu-smoke
```

Bring-up acceptance for initial Makefile:

- target names exist and fail deterministically when prerequisite artifacts are missing,
- profile and codec flags are wired through variables (even if some implementations are stubs),
- warnings-as-errors policy is encoded for active compilers.

### 4.1 Optional CMake Generator Strategy (Non-Primary Path)

`Makefile` remains the normative build surface. CMake exists only as a compatibility bridge for IDE/toolchain integration.

Bridge requirements:

1. CMake must expose an equivalent target set for primary gates (`build`, `format-check`, `lint`, `test`, `conformance`, `profile-parity`, `fuzz-smoke`).
2. CMake targets must call or mirror the same compiler/profile/codec/deterministic knobs used by `Makefile`.
3. If CMake and Make disagree, Make is the authority and CMake must be updated.
4. CMake support must not introduce additional semantic behavior branches or alternate default profiles.

### 4.2 Reproducibility Contract for Build and Test Targets

Every gate invocation must be reproducible with explicit config and deterministic flags.

Minimum reproducibility controls:

- profile is explicit (`ASX_PROFILE_*`),
- codec is explicit (`json` or `bin` where applicable),
- deterministic mode/seed are explicit for replay-sensitive targets,
- compiler/toolchain tuple is logged for matrix jobs.

Recommended command-shape contract:

```bash
make <target> PROFILE=ASX_PROFILE_CORE CODEC=json DETERMINISTIC=1 SEED=42
```

## 5. Compiler/Platform Matrix Expectations

Minimum matrix dimensions to encode in scripts/CI stubs:

- compiler: `gcc`, `clang`, `msvc`
- bitness: `32`, `64`
- profile: `ASX_PROFILE_CORE`, `ASX_PROFILE_FREESTANDING`, `ASX_PROFILE_EMBEDDED_ROUTER`
- extended profiles (schema + harness stubs): `ASX_PROFILE_HFT`, `ASX_PROFILE_AUTOMOTIVE`

Embedded triplets required by plan:

- `mipsel-openwrt-linux-musl`
- `armv7-openwrt-linux-muslgnueabi`
- `aarch64-openwrt-linux-musl`

Resource-class defaults for initial embedded matrix runs:

- `mipsel-openwrt-linux-musl` -> `ASX_CLASS_R1`
- `armv7-openwrt-linux-muslgnueabi` -> `ASX_CLASS_R2`
- `aarch64-openwrt-linux-musl` -> `ASX_CLASS_R3`

Matrix script contract for `bd-ix8.3`/`bd-ix8.4`:

- `tools/ci/run_compiler_matrix.sh` is the canonical compiler/bits matrix runner and emits parseable JSONL logs.
- `tools/ci/check_endian_assumptions.sh` validates compile-time endian/type assumptions per compiler/bits tuple.
- `tools/ci/run_embedded_matrix.sh` is the canonical embedded triplet runner and emits build + metric rows per triplet.
- Missing required targets must fail with actionable diagnostics; optional targets may report `unsupported` with explicit install hints.

## 6. QEMU + Walking Skeleton Expectations

Phase 2.5 skeleton acceptance path should prove:

1. one no-op task can be spawned through public headers,
2. task is polled to completion,
3. root region can close,
4. quiescence check passes,
5. deterministic replay seed path is plumbed (even with minimal events),
6. same smoke scenario can be invoked in QEMU harness script.

## 7. Artifact and Logging Contract

For each scaffold gate introduced, record:

- command invoked,
- target/profile/compiler triplet,
- pass/fail status,
- canonical artifact path for logs.
- deterministic controls (`DETERMINISTIC`, `SEED`) when applicable.

Suggested artifact layout:

```text
tools/ci/artifacts/
  build/
  test/
  conformance/
  qemu/
  manifest.jsonl
```

Suggested `manifest.jsonl` fields:

- `run_id`
- `target`
- `profile`
- `codec`
- `compiler`
- `arch`
- `deterministic`
- `seed`
- `status`
- `artifact_path`
- `binary_size_bytes`
- `startup_metric_us`
- `startup_metric_status`

## 8. Task-Level Execution Split (Recommended)

Suggested split when `bd-ix8.*` children are claimed:

1. `bd-ix8.1`: create tree + initial headers/source ownership map.
2. `bd-ix8.2`: implement Makefile target contract + optional CMake generator stub.
3. `bd-ix8.3`: compiler/platform matrix scripts with deterministic exit behavior.
4. `bd-ix8.4` + `bd-ix8.5`: embedded triplets + QEMU scenario harness.
5. `bd-ix8.8`: walking skeleton integration path and smoke assertions.

## 9. Pre-Claim Checklist for bd-ix8.1 / bd-ix8.2

Before opening those child claims, confirm:

1. Phase 1 review gate dependencies are explicitly tracked in beads status.
2. active `bd-296` outputs are linked for architecture and invariant authority.
3. no file reservation conflicts exist for scaffold roots (`include/`, `src/`, `tests/`, `tools/`).
4. expected quality-gate commands are defined in AGENTS/CI docs and mirrored in Make targets.

## 10. Immediate Next Action Trigger

While `bd-ix8.4` is active, this manifest is the source-of-truth checklist for embedded triplet build coverage, resource-class defaults, and reproducibility/metrics evidence shape.
